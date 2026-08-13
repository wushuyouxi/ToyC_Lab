#include "ast.h"

#include "sema.h"  // 仅 dump 注解需要(Symbol 的 kind/const_val)

namespace toyc {

const char* op_name(Op op) {
  switch (op) {
    case Op::ADD: return "+";
    case Op::SUB: return "-";
    case Op::MUL: return "*";
    case Op::DIV: return "/";
    case Op::MOD: return "%";
    case Op::AND: return "&&";
    case Op::OR: return "||";
    case Op::LT: return "<";
    case Op::GT: return ">";
    case Op::LE: return "<=";
    case Op::GE: return ">=";
    case Op::EQ: return "==";
    case Op::NE: return "!=";
    case Op::POS: return "+";
    case Op::NEG: return "-";
    case Op::NOT: return "!";
  }
  return "?";
}

const char* type_name(Type t) { return t == Type::INT ? "int" : "void"; }

namespace {
void indent(std::ostream& os, int depth) {
  for (int i = 0; i < depth; ++i) os << "  ";
}
}  // namespace

// ---------- 表达式 dump ----------
void NumberExpr::dump(std::ostream& os, int d) const {
  indent(os, d);
  os << "Number " << value << "\n";
}

void IdExpr::dump(std::ostream& os, int d) const {
  indent(os, d);
  os << "Id " << name;
  if (sym && sym->kind == Symbol::Kind::CONST) os << "  (= " << sym->const_val << ")";
  os << "\n";
}

void UnaryExpr::dump(std::ostream& os, int d) const {
  indent(os, d);
  os << "Unary " << op_name(op) << "\n";
  operand->dump(os, d + 1);
}

void BinaryExpr::dump(std::ostream& os, int d) const {
  indent(os, d);
  os << "Binary " << op_name(op) << "\n";
  lhs->dump(os, d + 1);
  rhs->dump(os, d + 1);
}

void CallExpr::dump(std::ostream& os, int d) const {
  indent(os, d);
  os << "Call " << callee << "\n";
  for (const auto& a : args) a->dump(os, d + 1);
}

// ---------- 语句 dump ----------
void BlockStmt::dump(std::ostream& os, int d) const {
  indent(os, d);
  os << "Block\n";
  for (const auto& s : stmts) s->dump(os, d + 1);
}

void EmptyStmt::dump(std::ostream& os, int d) const {
  indent(os, d);
  os << "Empty\n";
}

void ExprStmt::dump(std::ostream& os, int d) const {
  indent(os, d);
  os << "ExprStmt\n";
  expr->dump(os, d + 1);
}

void AssignStmt::dump(std::ostream& os, int d) const {
  indent(os, d);
  os << "Assign " << name << "\n";
  rhs->dump(os, d + 1);
}

void DeclStmt::dump(std::ostream& os, int d) const {
  indent(os, d);
  os << "Decl " << (is_const ? "const " : "") << "int " << name;
  if (sym && sym->kind == Symbol::Kind::CONST) os << "  (= " << sym->const_val << ")";
  os << " =\n";
  init->dump(os, d + 1);
}

void IfStmt::dump(std::ostream& os, int d) const {
  indent(os, d);
  os << "If\n";
  indent(os, d + 1);
  os << "cond:\n";
  cond->dump(os, d + 2);
  indent(os, d + 1);
  os << "then:\n";
  then_branch->dump(os, d + 2);
  if (else_branch) {
    indent(os, d + 1);
    os << "else:\n";
    else_branch->dump(os, d + 2);
  }
}

void WhileStmt::dump(std::ostream& os, int d) const {
  indent(os, d);
  os << "While\n";
  indent(os, d + 1);
  os << "cond:\n";
  cond->dump(os, d + 2);
  indent(os, d + 1);
  os << "body:\n";
  body->dump(os, d + 2);
}

void BreakStmt::dump(std::ostream& os, int d) const {
  indent(os, d);
  os << "Break\n";
}

void ContinueStmt::dump(std::ostream& os, int d) const {
  indent(os, d);
  os << "Continue\n";
}

void ReturnStmt::dump(std::ostream& os, int d) const {
  indent(os, d);
  os << "Return\n";
  if (value) value->dump(os, d + 1);
}

// ---------- 全局层 dump ----------
void GlobalDecl::dump(std::ostream& os, int d) const {
  indent(os, d);
  os << "Global " << (is_const ? "const " : "") << "int " << name;
  if (folded_init) os << "  (static init: " << *folded_init << ")";
  os << " =\n";
  init->dump(os, d + 1);
}

void FuncDef::dump(std::ostream& os, int d) const {
  indent(os, d);
  os << "Func " << type_name(ret_type) << " " << name << "(";
  for (size_t i = 0; i < params.size(); ++i) {
    if (i) os << ", ";
    os << "int " << params[i].name;
  }
  os << ")\n";
  body->dump(os, d + 1);
}

void CompUnit::dump(std::ostream& os) const {
  for (const auto& it : items)
    std::visit([&os](const auto& x) { x.dump(os, 0); }, it);
}

}  // namespace toyc
