#include "ir.h"

#include "sema.h"

namespace toyc {

const char* irop_name(IROp op) {
  switch (op) {
    case IROp::MOV: return "mov";
    case IROp::LOADIMM: return "li";
    case IROp::LOADGLOBAL: return "loadg";
    case IROp::STOREGLOBAL: return "storeg";
    case IROp::ARG: return "arg";
    case IROp::ADD: return "add";
    case IROp::SUB: return "sub";
    case IROp::ADDK: return "addk";
    case IROp::SUBK: return "subk";
    case IROp::MUL: return "mul";
    case IROp::DIV: return "div";
    case IROp::REM: return "rem";
    case IROp::NEG: return "neg";
    case IROp::NOT: return "not";
    case IROp::SHL: return "shl";
    case IROp::SRL: return "srl";
    case IROp::SRA: return "sra";
    case IROp::LT: return "lt";
    case IROp::GT: return "gt";
    case IROp::LE: return "le";
    case IROp::GE: return "ge";
    case IROp::EQ: return "eq";
    case IROp::NE: return "ne";
    case IROp::JUMP: return "jmp";
    case IROp::BRZ: return "brz";
    case IROp::BRNZ: return "brnz";
    case IROp::CALL: return "call";
    case IROp::RETURN: return "ret";
    case IROp::RETURNVOID: return "ret";
    case IROp::LABEL: return "label";
  }
  return "?";
}

// ---------- 构建 ----------
IRModule IRBuilder::build(const CompUnit& unit) {
  IRModule m;
  mod_ = &m;
  // 第一遍:收集全局变量(区分静态初始化与运行时初始化)
  for (const auto& item : unit.items) {
    if (const GlobalDecl* d = std::get_if<GlobalDecl>(&item)) {
      IRGlobal g;
      g.name = d->name;
      g.static_init = d->folded_init;
      m.globals.push_back(std::move(g));
      if (!d->folded_init) runtime_init_globals_.push_back(d);
    }
  }
  // 第二遍:按源码顺序生成各函数
  for (const auto& item : unit.items) {
    if (const FuncDef* f = std::get_if<FuncDef>(&item)) gen_func(*f);
  }
  return m;
}

void IRBuilder::gen_func(const FuncDef& f) {
  mod_->functions.emplace_back();
  cur_func_ = &mod_->functions.back();
  cur_func_->name = f.name;
  cur_func_->ret_type = f.ret_type;
  cur_func_->param_count = static_cast<int>(f.params.size());
  next_vreg_ = 0;
  label_seq_ = 0;
  var_vregs_.clear();

  // main 入口先执行无静态初值的全局变量初始化(按声明顺序)
  if (f.name == "main") gen_global_runtime_init();

  for (size_t i = 0; i < f.params.size(); ++i) {
    int v = new_vreg();
    Instr in;
    in.op = IROp::ARG;
    in.rd = v;
    in.imm = static_cast<int64_t>(i);
    emit(in);
    var_vregs_[f.params[i].sym] = v;
  }
  gen_block(*f.body);
  // void 函数结尾兜底返回(合法输入下不可达,窥孔/跳转优化会消除)
  if (f.ret_type == Type::VOID) {
    Instr r;
    r.op = IROp::RETURNVOID;
    emit(r);
  }
  cur_func_->max_vreg = next_vreg_;
  cur_func_ = nullptr;
}

void IRBuilder::gen_global_runtime_init() {
  for (const GlobalDecl* d : runtime_init_globals_) {
    int v = gen_expr(*d->init);
    Instr st;
    st.op = IROp::STOREGLOBAL;
    st.rs1 = v;
    st.sym = d->name;
    st.line = d->line;
    emit(st);
  }
}

// ---------- 语句 ----------
void IRBuilder::gen_stmt(const Stmt& s) {
  if (const auto* b = dynamic_cast<const BlockStmt*>(&s)) {
    gen_block(*b);
  } else if (const auto* es = dynamic_cast<const ExprStmt*>(&s)) {
    gen_expr(*es->expr);  // 结果 vreg 不被使用,由 DCE 清除
  } else if (const auto* as = dynamic_cast<const AssignStmt*>(&s)) {
    int v = gen_expr(*as->rhs);
    Instr in;
    in.line = s.line;
    if (as->sym->kind == Symbol::Kind::GLOBAL_VAR) {
      in.op = IROp::STOREGLOBAL;
      in.rs1 = v;
      in.sym = as->name;
    } else {
      in.op = IROp::MOV;
      in.rd = var_vregs_[as->sym];
      in.rs1 = v;
    }
    emit(in);
  } else if (const auto* ds = dynamic_cast<const DeclStmt*>(&s)) {
    int v = new_vreg();
    var_vregs_[ds->sym] = v;
    int init = gen_expr(*ds->init);
    Instr mv;
    mv.op = IROp::MOV;
    mv.rd = v;
    mv.rs1 = init;
    mv.line = s.line;
    emit(mv);
  } else if (const auto* is = dynamic_cast<const IfStmt*>(&s)) {
    std::string Lelse = new_label(), Lend = new_label();
    gen_cond_false(*is->cond, Lelse);
    gen_stmt(*is->then_branch);
    Instr j;
    j.op = IROp::JUMP;
    j.label = Lend;
    emit(j);
    Instr lelse;
    lelse.op = IROp::LABEL;
    lelse.label = Lelse;
    emit(lelse);
    if (is->else_branch) gen_stmt(*is->else_branch);
    Instr lend;
    lend.op = IROp::LABEL;
    lend.label = Lend;
    emit(lend);
  } else if (const auto* ws = dynamic_cast<const WhileStmt*>(&s)) {
    std::string Lcond = new_label(), Lexit = new_label();
    Instr lc;
    lc.op = IROp::LABEL;
    lc.label = Lcond;
    emit(lc);
    gen_cond_false(*ws->cond, Lexit);
    loops_.push_back({Lcond, Lexit});
    gen_stmt(*ws->body);
    loops_.pop_back();
    Instr j;
    j.op = IROp::JUMP;
    j.label = Lcond;
    emit(j);
    Instr le;
    le.op = IROp::LABEL;
    le.label = Lexit;
    emit(le);
  } else if (dynamic_cast<const BreakStmt*>(&s)) {
    Instr j;
    j.op = IROp::JUMP;
    j.label = loops_.back().break_label;
    emit(j);
  } else if (dynamic_cast<const ContinueStmt*>(&s)) {
    Instr j;
    j.op = IROp::JUMP;
    j.label = loops_.back().cont_label;
    emit(j);
  } else if (const auto* rs = dynamic_cast<const ReturnStmt*>(&s)) {
    Instr r;
    r.line = s.line;
    if (rs->value) {
      r.op = IROp::RETURN;
      r.rs1 = gen_expr(*rs->value);
    } else {
      r.op = IROp::RETURNVOID;
    }
    emit(r);
  }
  // EmptyStmt:不产生代码
}

void IRBuilder::gen_block(const BlockStmt& b) {
  for (const auto& st : b.stmts) gen_stmt(*st);
}

// ---------- 表达式 ----------
int IRBuilder::gen_expr(const Expr& e) {
  if (const auto* n = dynamic_cast<const NumberExpr*>(&e)) {
    int v = new_vreg();
    Instr in;
    in.op = IROp::LOADIMM;
    in.rd = v;
    in.imm = n->value;
    in.line = n->line;
    emit(in);
    return v;
  }
  if (const auto* id = dynamic_cast<const IdExpr*>(&e)) {
    Symbol* sym = id->sym;
    if (sym->kind == Symbol::Kind::CONST) {  // const 引用:直接物化编译期值
      int v = new_vreg();
      Instr in;
      in.op = IROp::LOADIMM;
      in.rd = v;
      in.imm = sym->const_val;
      in.line = id->line;
      emit(in);
      return v;
    }
    if (sym->kind == Symbol::Kind::GLOBAL_VAR) {
      int v = new_vreg();
      Instr in;
      in.op = IROp::LOADGLOBAL;
      in.rd = v;
      in.sym = id->name;
      in.line = id->line;
      emit(in);
      return v;
    }
    return var_vregs_[sym];  // 局部变量/形参:直接复用其 vreg,零指令
  }
  if (const auto* u = dynamic_cast<const UnaryExpr*>(&e)) {
    int x = gen_expr(*u->operand);
    if (u->op == Op::POS) return x;
    int v = new_vreg();
    Instr in;
    in.op = (u->op == Op::NEG) ? IROp::NEG : IROp::NOT;
    in.rd = v;
    in.rs1 = x;
    in.line = u->line;
    emit(in);
    return v;
  }
  if (const auto* b = dynamic_cast<const BinaryExpr*>(&e)) {
    if (b->op == Op::AND || b->op == Op::OR) {
      // 短路求值,物化 0/1
      int v = new_vreg();
      std::string Lend = new_label(), Lother = new_label();
      if (b->op == Op::AND) {  // 假路径 → Lother
        gen_cond_false(*b->lhs, Lother);
        gen_cond_false(*b->rhs, Lother);
        Instr one;
        one.op = IROp::LOADIMM; one.rd = v; one.imm = 1;
        emit(one);
        Instr j;
        j.op = IROp::JUMP; j.label = Lend;
        emit(j);
        Instr lo;
        lo.op = IROp::LABEL; lo.label = Lother;
        emit(lo);
        Instr z;
        z.op = IROp::LOADIMM; z.rd = v; z.imm = 0;
        emit(z);
      } else {  // || :真路径 → Lother
        gen_cond_true(*b->lhs, Lother);
        gen_cond_true(*b->rhs, Lother);
        Instr z;
        z.op = IROp::LOADIMM; z.rd = v; z.imm = 0;
        emit(z);
        Instr j;
        j.op = IROp::JUMP; j.label = Lend;
        emit(j);
        Instr lo;
        lo.op = IROp::LABEL; lo.label = Lother;
        emit(lo);
        Instr one;
        one.op = IROp::LOADIMM; one.rd = v; one.imm = 1;
        emit(one);
      }
      Instr le;
      le.op = IROp::LABEL;
      le.label = Lend;
      emit(le);
      return v;
    }
    int l = gen_expr(*b->lhs), r = gen_expr(*b->rhs);
    int v = new_vreg();
    Instr in;
    in.rd = v;
    in.rs1 = l;
    in.rs2 = r;
    in.line = b->line;
    switch (b->op) {
      case Op::ADD: in.op = IROp::ADD; break;
      case Op::SUB: in.op = IROp::SUB; break;
      case Op::MUL: in.op = IROp::MUL; break;
      case Op::DIV: in.op = IROp::DIV; break;
      case Op::MOD: in.op = IROp::REM; break;
      case Op::LT: in.op = IROp::LT; break;
      case Op::GT: in.op = IROp::GT; break;
      case Op::LE: in.op = IROp::LE; break;
      case Op::GE: in.op = IROp::GE; break;
      case Op::EQ: in.op = IROp::EQ; break;
      case Op::NE: in.op = IROp::NE; break;
      default: break;  // AND/OR 已在上方处理
    }
    emit(in);
    return v;
  }
  if (const auto* c = dynamic_cast<const CallExpr*>(&e)) {
    Instr in;
    in.op = IROp::CALL;
    in.func = c->callee;
    in.line = c->line;
    for (const auto& a : c->args) in.args.push_back(gen_expr(*a));
    if (c->sym && c->sym->type == Type::INT) in.rd = new_vreg();
    emit(in);
    return in.rd;
  }
  return -1;  // 不可达
}

// e 为假时跳转到 L;短路结构直接展开,不物化 0/1。
// 四个方向组合的展开规则(按短路语义推导,勿改):
//   cf(a&&b, L) = cf(a,L); cf(b,L)
//   cf(a||b, L) = ct(a,T); cf(b,L); T:
//   ct(a&&b, L) = cf(a,T); ct(b,L); T:
//   ct(a||b, L) = ct(a,L); ct(b,L)
void IRBuilder::gen_cond_false(const Expr& e, const std::string& L) {
  if (const auto* b = dynamic_cast<const BinaryExpr*>(&e)) {
    if (b->op == Op::AND) {
      gen_cond_false(*b->lhs, L);
      gen_cond_false(*b->rhs, L);
      return;
    }
    if (b->op == Op::OR) {
      std::string t = new_label();
      gen_cond_true(*b->lhs, t);
      gen_cond_false(*b->rhs, L);
      Instr lt;
      lt.op = IROp::LABEL;
      lt.label = t;
      emit(lt);
      return;
    }
  }
  int v = gen_expr(e);
  Instr br;
  br.op = IROp::BRZ;
  br.rs1 = v;
  br.label = L;
  br.line = e.line;
  emit(br);
}

// e 为真时跳转到 L(展开规则见 gen_cond_false 注释)
void IRBuilder::gen_cond_true(const Expr& e, const std::string& L) {
  if (const auto* b = dynamic_cast<const BinaryExpr*>(&e)) {
    if (b->op == Op::AND) {
      std::string t = new_label();
      gen_cond_false(*b->lhs, t);
      gen_cond_true(*b->rhs, L);
      Instr lt;
      lt.op = IROp::LABEL;
      lt.label = t;
      emit(lt);
      return;
    }
    if (b->op == Op::OR) {
      gen_cond_true(*b->lhs, L);
      gen_cond_true(*b->rhs, L);
      return;
    }
  }
  int v = gen_expr(e);
  Instr br;
  br.op = IROp::BRNZ;
  br.rs1 = v;
  br.label = L;
  br.line = e.line;
  emit(br);
}

// ---------- dump ----------
void IRModule::dump(std::ostream& os) const {
  if (!globals.empty()) {
    os << ".globals:\n";
    for (const auto& g : globals) {
      os << "  " << g.name;
      if (g.static_init) os << "  (static init: " << *g.static_init << ")";
      else os << "  (runtime init)";
      os << "\n";
    }
  }
  for (const auto& f : functions) {
    os << "\n.function " << (f.ret_type == Type::INT ? "int" : "void") << " " << f.name
       << "  (params: " << f.param_count << ", vregs: " << f.max_vreg << ")\n";
    for (const auto& in : f.code) {
      if (in.op == IROp::LABEL) {
        os << "  " << in.label << ":\n";
        continue;
      }
      os << "  ";
      switch (in.op) {
        case IROp::MOV: os << "r" << in.rd << " = r" << in.rs1; break;
        case IROp::LOADIMM: os << "r" << in.rd << " = " << in.imm; break;
        case IROp::LOADGLOBAL: os << "r" << in.rd << " = " << in.sym; break;
        case IROp::STOREGLOBAL: os << in.sym << " = r" << in.rs1; break;
        case IROp::ARG: os << "r" << in.rd << " = arg" << in.imm; break;
        case IROp::ADD: case IROp::SUB: case IROp::MUL: case IROp::DIV: case IROp::REM:
        case IROp::LT: case IROp::GT: case IROp::LE: case IROp::GE: case IROp::EQ: case IROp::NE:
          os << "r" << in.rd << " = r" << in.rs1 << " " << irop_name(in.op) << " r" << in.rs2;
          break;
        case IROp::NEG: os << "r" << in.rd << " = -r" << in.rs1; break;
        case IROp::NOT: os << "r" << in.rd << " = !r" << in.rs1; break;
        case IROp::SHL: case IROp::SRL: case IROp::SRA:
          os << "r" << in.rd << " = r" << in.rs1 << " " << irop_name(in.op) << " " << in.imm;
          break;
        case IROp::ADDK: os << "r" << in.rd << " = r" << in.rs1 << " + " << in.imm; break;
        case IROp::SUBK: os << "r" << in.rd << " = r" << in.rs1 << " - " << in.imm; break;
        case IROp::JUMP: os << "jmp " << in.label; break;
        case IROp::BRZ: os << "brz r" << in.rs1 << ", " << in.label; break;
        case IROp::BRNZ: os << "brnz r" << in.rs1 << ", " << in.label; break;
        case IROp::CALL:
          if (in.rd >= 0) os << "r" << in.rd << " = ";
          os << "call " << in.func << "(";
          for (size_t i = 0; i < in.args.size(); ++i) {
            if (i) os << ", ";
            os << "r" << in.args[i];
          }
          os << ")";
          break;
        case IROp::RETURN: os << "ret r" << in.rs1; break;
        case IROp::RETURNVOID: os << "ret"; break;
        default: os << "?"; break;
      }
      os << "\n";
    }
  }
}

}  // namespace toyc
