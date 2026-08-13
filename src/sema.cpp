#include "sema.h"

#include <climits>
#include <utility>

namespace toyc {

Sema::Sema(Diag& diag) : diag_(diag) {}

// ---------- 作用域 ----------
Symbol* Sema::lookup(const std::string& name) const {
  for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
    auto f = it->find(name);
    if (f != it->end()) return f->second;
  }
  return nullptr;
}

Symbol* Sema::lookup_global(const std::string& name) const {
  auto f = scopes_[0].find(name);
  return f == scopes_[0].end() ? nullptr : f->second;
}

Symbol* Sema::declare(const std::string& name, Symbol sym, int line) {
  auto& cur = scopes_.back();
  if (cur.count(name)) {
    diag_.error(line, 0, "redefinition of '" + name + "' in the same scope");
    return nullptr;
  }
  arena_.push_back(std::move(sym));
  Symbol* p = &arena_.back();
  cur.emplace(name, p);
  return p;
}

// ---------- 顶层 ----------
void Sema::analyze(CompUnit& unit) {
  push_scope();  // 全局作用域
  for (auto& item : unit.items) {
    std::visit([this](auto& x) {
      if constexpr (std::is_same_v<std::decay_t<decltype(x)>, GlobalDecl>)
        analyze_global_decl(x);
      else
        analyze_func_def(x);
    }, item);
  }
  // main 检查:必须存在、返回 int、参数列表为空
  Symbol* m = lookup_global("main");
  if (!m || m->kind != Symbol::Kind::FUNC) {
    diag_.error(0, 0, "program must contain a function named 'main'");
  } else if (m->type != Type::INT || m->param_count != 0) {
    diag_.error(0, 0, "'main' must have return type int and an empty parameter list");
  }
}

void Sema::analyze_global_decl(GlobalDecl& d) {
  analyze_expr(*d.init, /*value_ctx=*/true);
  Symbol sym;
  sym.name = d.name;
  if (d.is_const) {
    int64_t v;
    if (!eval_const(*d.init, v)) {
      diag_.error(d.line, 0,
                  "initializer of const '" + d.name + "' must be a constant expression");
    } else if (v < INT32_MIN || v > INT32_MAX) {
      diag_.error(d.line, 0, "constant value of '" + d.name + "' out of int32 range");
    } else {
      sym.kind = Symbol::Kind::CONST;
      sym.const_val = static_cast<int32_t>(v);
      d.folded_init = sym.const_val;
    }
  } else {
    sym.kind = Symbol::Kind::GLOBAL_VAR;
    int64_t v;
    if (eval_const(*d.init, v) && v >= INT32_MIN && v <= INT32_MAX)
      d.folded_init = static_cast<int32_t>(v);  // 可静态初始化,直接写入 .data
  }
  if (Symbol* p = declare(d.name, std::move(sym), d.line)) globals_in_order_.push_back(p);
}

void Sema::analyze_func_def(FuncDef& f) {
  Symbol sym;
  sym.name = f.name;
  sym.kind = Symbol::Kind::FUNC;
  sym.type = f.ret_type;
  sym.param_count = static_cast<int>(f.params.size());
  // 先插入符号再分析函数体 → 支持函数体内调用自身(递归)
  if (Symbol* p = declare(f.name, std::move(sym), f.line)) funcs_in_order_.push_back(p);
  analyze_func(f);
}

void Sema::analyze_func(FuncDef& f) {
  push_scope();
  cur_func_ = &f;
  cur_stack_off_ = 0;
  for (Param& p : f.params) {
    Symbol sym;
    sym.name = p.name;
    sym.kind = Symbol::Kind::PARAM;
    sym.stack_offset = cur_stack_off_++;
    p.sym = declare(p.name, std::move(sym), p.line);
  }
  analyze_stmt(*f.body, /*in_loop=*/false);
  if (f.ret_type == Type::INT && !stmt_always_returns(*f.body))
    diag_.warning(f.line, 0,
                  "control may reach end of non-void function '" + f.name + "'");
  cur_func_ = nullptr;
  pop_scope();
}

// ---------- 语句 ----------
void Sema::analyze_stmt(Stmt& s, bool in_loop) {
  if (auto* b = dynamic_cast<BlockStmt*>(&s)) {
    push_scope();
    for (auto& st : b->stmts) analyze_stmt(*st, in_loop);
    pop_scope();
  } else if (auto* es = dynamic_cast<ExprStmt*>(&s)) {
    analyze_expr(*es->expr, /*value_ctx=*/false);
  } else if (auto* as = dynamic_cast<AssignStmt*>(&s)) {
    Symbol* sym = lookup(as->name);
    if (!sym) {
      diag_.error(as->line, 0, "use of undeclared identifier '" + as->name + "'");
      return;
    }
    if (sym->kind == Symbol::Kind::CONST)
      diag_.error(as->line, 0, "cannot assign to const '" + as->name + "'");
    else if (sym->kind == Symbol::Kind::FUNC)
      diag_.error(as->line, 0, "function '" + as->name + "' cannot be used as a value");
    as->sym = sym;
    analyze_expr(*as->rhs, /*value_ctx=*/true);
  } else if (auto* ds = dynamic_cast<DeclStmt*>(&s)) {
    analyze_expr(*ds->init, /*value_ctx=*/true);
    Symbol sym;
    sym.name = ds->name;
    if (ds->is_const) {
      int64_t v;
      if (!eval_const(*ds->init, v)) {
        diag_.error(ds->line, 0,
                    "initializer of const '" + ds->name + "' must be a constant expression");
      } else if (v < INT32_MIN || v > INT32_MAX) {
        diag_.error(ds->line, 0, "constant value of '" + ds->name + "' out of int32 range");
      } else {
        sym.kind = Symbol::Kind::CONST;
        sym.const_val = static_cast<int32_t>(v);
      }
    } else {
      sym.kind = Symbol::Kind::LOCAL_VAR;
    }
    sym.stack_offset = cur_stack_off_++;
    // 初始化表达式求值之后再声明符号 → "int x = x;" 中的 x 查不到,自然报错
    ds->sym = declare(ds->name, std::move(sym), ds->line);
  } else if (auto* is = dynamic_cast<IfStmt*>(&s)) {
    analyze_expr(*is->cond, /*value_ctx=*/true);
    analyze_stmt(*is->then_branch, in_loop);
    if (is->else_branch) analyze_stmt(*is->else_branch, in_loop);
  } else if (auto* ws = dynamic_cast<WhileStmt*>(&s)) {
    analyze_expr(*ws->cond, /*value_ctx=*/true);
    analyze_stmt(*ws->body, /*in_loop=*/true);
  } else if (dynamic_cast<BreakStmt*>(&s)) {
    if (!in_loop) diag_.error(s.line, 0, "'break' outside of a loop");
  } else if (dynamic_cast<ContinueStmt*>(&s)) {
    if (!in_loop) diag_.error(s.line, 0, "'continue' outside of a loop");
  } else if (auto* rs = dynamic_cast<ReturnStmt*>(&s)) {
    if (cur_func_->ret_type == Type::INT) {
      if (!rs->value)
        diag_.error(s.line, 0, "non-void function must return a value");
      else
        analyze_expr(*rs->value, /*value_ctx=*/true);
    } else if (rs->value) {
      diag_.error(s.line, 0, "void function must not return a value");
    }
  }
}

// ---------- 表达式 ----------
Type Sema::analyze_expr(Expr& e, bool value_ctx, bool allow_big_literal) {
  if (auto* n = dynamic_cast<NumberExpr*>(&e)) {
    // 字面量 > INT32_MAX 只允许出现在一元负号之下(-2147483648 = INT_MIN 的写法)
    if (n->value > INT32_MAX && !allow_big_literal)
      diag_.error(n->line, 0, "integer literal out of range");
    return Type::INT;
  }
  if (auto* id = dynamic_cast<IdExpr*>(&e)) {
    Symbol* sym = lookup(id->name);
    if (!sym) {
      diag_.error(id->line, 0, "use of undeclared identifier '" + id->name + "'");
      return Type::INT;
    }
    if (sym->kind == Symbol::Kind::FUNC) {
      diag_.error(id->line, 0, "function '" + id->name + "' cannot be used as a value");
      return Type::INT;
    }
    id->sym = sym;
    return Type::INT;
  }
  if (auto* u = dynamic_cast<UnaryExpr*>(&e)) {
    if (u->op == Op::NEG && dynamic_cast<NumberExpr*>(u->operand.get())) {
      // -2147483648 合法(-(2^31) = INT_MIN);更大会溢出
      auto* n = static_cast<NumberExpr*>(u->operand.get());
      if (n->value > 2147483648LL)
        diag_.error(n->line, 0, "integer literal out of range");
      analyze_expr(*u->operand, /*value_ctx=*/true, /*allow_big_literal=*/true);
    } else {
      analyze_expr(*u->operand, /*value_ctx=*/true);
    }
    return Type::INT;
  }
  if (auto* b = dynamic_cast<BinaryExpr*>(&e)) {
    Type l = analyze_expr(*b->lhs, /*value_ctx=*/true);
    Type r = analyze_expr(*b->rhs, /*value_ctx=*/true);
    if (l == Type::VOID || r == Type::VOID)
      diag_.error(b->line, 0, "void value used in an expression");
    return Type::INT;
  }
  if (auto* c = dynamic_cast<CallExpr*>(&e)) {
    Symbol* sym = lookup(c->callee);
    c->sym = sym;
    if (!sym) {
      diag_.error(c->line, 0, "use of undeclared function '" + c->callee + "'");
      for (auto& a : c->args) analyze_expr(*a, /*value_ctx=*/true);
      return Type::INT;
    }
    if (sym->kind != Symbol::Kind::FUNC) {
      diag_.error(c->line, 0, "'" + c->callee + "' is not a function");
      for (auto& a : c->args) analyze_expr(*a, /*value_ctx=*/true);
      return Type::INT;
    }
    if (static_cast<int>(c->args.size()) != sym->param_count)
      diag_.error(c->line, 0, "function '" + c->callee + "' expects " +
                                  std::to_string(sym->param_count) + " argument(s) but got " +
                                  std::to_string(c->args.size()));
    for (auto& a : c->args) analyze_expr(*a, /*value_ctx=*/true);
    if (sym->type == Type::VOID && value_ctx)
      diag_.error(c->line, 0, "void function call used as a value");
    return sym->type;
  }
  return Type::INT;  // 不可达
}

// ---------- const 编译期求值 ----------
bool Sema::eval_const(const Expr& e, int64_t& out) const {
  // 说明:合法程序的每个中间结果都落在 int32 内(溢出是 C 的 UB,测试用例规避),
  // 因此 int64 域计算不会溢出(唯一例外是 -2147483648,量级 2^31,同样安全)。
  if (auto* n = dynamic_cast<const NumberExpr*>(&e)) {
    out = n->value;
    return true;
  }
  if (auto* id = dynamic_cast<const IdExpr*>(&e)) {
    if (id->sym && id->sym->kind == Symbol::Kind::CONST) {
      out = id->sym->const_val;
      return true;
    }
    return false;
  }
  if (auto* u = dynamic_cast<const UnaryExpr*>(&e)) {
    int64_t x;
    if (!eval_const(*u->operand, x)) return false;
    switch (u->op) {
      case Op::POS: out = x; break;
      case Op::NEG: out = -x; break;
      case Op::NOT: out = (x == 0) ? 1 : 0; break;
      default: return false;
    }
    return true;
  }
  if (auto* b = dynamic_cast<const BinaryExpr*>(&e)) {
    int64_t l;
    if (!eval_const(*b->lhs, l)) return false;
    // 短路:右侧可能不求值,与运行期语义一致
    if (b->op == Op::AND) {
      if (l == 0) { out = 0; return true; }
      int64_t r;
      if (!eval_const(*b->rhs, r)) return false;
      out = (r != 0) ? 1 : 0;
      return true;
    }
    if (b->op == Op::OR) {
      if (l != 0) { out = 1; return true; }
      int64_t r;
      if (!eval_const(*b->rhs, r)) return false;
      out = (r != 0) ? 1 : 0;
      return true;
    }
    int64_t r;
    if (!eval_const(*b->rhs, r)) return false;
    switch (b->op) {
      case Op::ADD: out = l + r; break;
      case Op::SUB: out = l - r; break;
      case Op::MUL: out = l * r; break;
      case Op::DIV:
        if (r == 0) {
          diag_.error(b->line, 0, "division by zero in constant expression");
          return false;
        }
        out = l / r;  // int64 除法截断向零,与 C 一致
        break;
      case Op::MOD:
        if (r == 0) {
          diag_.error(b->line, 0, "division by zero in constant expression");
          return false;
        }
        out = l % r;  // C 语义:结果符号与被除数一致
        break;
      case Op::LT: out = (l < r) ? 1 : 0; break;
      case Op::GT: out = (l > r) ? 1 : 0; break;
      case Op::LE: out = (l <= r) ? 1 : 0; break;
      case Op::GE: out = (l >= r) ? 1 : 0; break;
      case Op::EQ: out = (l == r) ? 1 : 0; break;
      case Op::NE: out = (l != r) ? 1 : 0; break;
      default: return false;
    }
    return true;
  }
  return false;  // CallExpr 等不可折叠
}

// ---------- 全路径 return(保守,warning 级) ----------
bool Sema::stmt_always_returns(const Stmt& s) const {
  if (auto* b = dynamic_cast<const BlockStmt*>(&s)) {
    for (const auto& st : b->stmts)
      if (stmt_always_returns(*st)) return true;
    return false;
  }
  if (auto* i = dynamic_cast<const IfStmt*>(&s))
    return i->else_branch && stmt_always_returns(*i->then_branch) &&
           stmt_always_returns(*i->else_branch);
  if (auto* w = dynamic_cast<const WhileStmt*>(&s)) {
    // while(常量非零) 且循环体内无 break → 循环永不正常退出
    int64_t c;
    if (eval_const(*w->cond, c) && c != 0 && !stmt_has_break(*w->body, 0))
      return stmt_always_returns(*w->body);
    return false;
  }
  return dynamic_cast<const ReturnStmt*>(&s) != nullptr;
}

bool Sema::stmt_has_break(const Stmt& s, int loop_depth) const {
  if (auto* b = dynamic_cast<const BlockStmt*>(&s)) {
    for (const auto& st : b->stmts)
      if (stmt_has_break(*st, loop_depth)) return true;
    return false;
  }
  if (auto* i = dynamic_cast<const IfStmt*>(&s))
    return stmt_has_break(*i->then_branch, loop_depth) ||
           (i->else_branch && stmt_has_break(*i->else_branch, loop_depth));
  if (auto* w = dynamic_cast<const WhileStmt*>(&s))
    return stmt_has_break(*w->body, loop_depth + 1);
  return dynamic_cast<const BreakStmt*>(&s) != nullptr && loop_depth == 0;
}

}  // namespace toyc
