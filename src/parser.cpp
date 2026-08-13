#include "parser.h"

#include <utility>

namespace toyc {

Parser::Parser(std::vector<Token> tokens, Diag& diag)
    : tokens_(std::move(tokens)), diag_(diag) {}

void Parser::expect(Tok k, const char* what) {
  if (!check(k)) {
    diag_.error(cur().line, cur().col,
                std::string("expected ") + what + " but got '" + cur().text + "'");
    throw ParseError{};
  }
  advance();
}

static Op unop_of(Tok t) {
  switch (t) {
    case Tok::PLUS: return Op::POS;
    case Tok::MINUS: return Op::NEG;
    case Tok::BANG: return Op::NOT;
    default: return Op::NEG;  // 不可达
  }
}

// ---------- CompUnit → (Decl | FuncDef)+ ----------
std::unique_ptr<CompUnit> Parser::parse_comp_unit() {
  auto unit = std::make_unique<CompUnit>();
  while (!check(Tok::END)) {
    if (check(Tok::CONST)) {
      advance();
      unit->items.emplace_back(std::move(*parse_global_decl(/*is_const=*/true)));
    } else if (check(Tok::INT)) {
      // int ID '=' ... 为变量声明;int ID '(' ... 为函数定义
      bool is_func = tokens_[i_ + 1].kind == Tok::ID && tokens_[i_ + 2].kind == Tok::LPAREN;
      if (is_func) {
        advance();  // int
        unit->items.emplace_back(std::move(*parse_func_def(Type::INT)));
      } else {
        // 不消费 int:parse_global_decl 内部消费
        unit->items.emplace_back(std::move(*parse_global_decl(/*is_const=*/false)));
      }
    } else if (check(Tok::VOID)) {
      advance();
      unit->items.emplace_back(std::move(*parse_func_def(Type::VOID)));
    } else {
      diag_.error(cur().line, cur().col, "expected declaration or function definition");
      throw ParseError{};
    }
  }
  return unit;
}

// ---------- 声明 ----------
std::unique_ptr<GlobalDecl> Parser::parse_global_decl(bool is_const) {
  auto d = std::make_unique<GlobalDecl>();
  d->is_const = is_const;
  d->line = cur().line;
  expect(Tok::INT, "'int'");
  expect(Tok::ID, "identifier");
  d->name = tokens_[i_ - 1].text;
  expect(Tok::ASSIGN, "'='");
  d->init = parse_expr();
  expect(Tok::SEMI, "';'");
  return d;
}

std::unique_ptr<DeclStmt> Parser::parse_local_decl(bool is_const) {
  auto s = std::make_unique<DeclStmt>();
  s->is_const = is_const;
  s->line = cur().line;
  expect(Tok::INT, "'int'");
  expect(Tok::ID, "identifier");
  s->name = tokens_[i_ - 1].text;
  expect(Tok::ASSIGN, "'='");
  s->init = parse_expr();
  expect(Tok::SEMI, "';'");
  return s;
}

// ---------- FuncDef → ("int" | "void") ID "(" (Param ("," Param)*)? ")" Block ----------
std::unique_ptr<FuncDef> Parser::parse_func_def(Type ret) {
  auto f = std::make_unique<FuncDef>();
  f->ret_type = ret;
  f->line = cur().line;
  expect(Tok::ID, "function name");
  f->name = tokens_[i_ - 1].text;
  expect(Tok::LPAREN, "'('");
  if (!check(Tok::RPAREN)) {
    do {
      expect(Tok::INT, "'int' in parameter list");
      expect(Tok::ID, "parameter name");
      Param p;
      p.name = tokens_[i_ - 1].text;
      p.line = tokens_[i_ - 1].line;
      f->params.push_back(std::move(p));
    } while (accept(Tok::COMMA));
  }
  expect(Tok::RPAREN, "')'");
  f->body = parse_block();
  return f;
}

// ---------- Block → "{" Stmt* "}" ----------
std::unique_ptr<BlockStmt> Parser::parse_block() {
  auto b = std::make_unique<BlockStmt>();
  b->line = cur().line;
  expect(Tok::LBRACE, "'{'");
  while (!check(Tok::RBRACE)) {
    if (check(Tok::END)) {
      diag_.error(cur().line, cur().col, "unexpected end of input (missing '}'?)");
      throw ParseError{};
    }
    b->stmts.push_back(parse_stmt());
  }
  advance();  // '}'
  return b;
}

// ---------- Stmt ----------
std::unique_ptr<Stmt> Parser::parse_stmt() {
  switch (cur().kind) {
    case Tok::LBRACE:
      return parse_block();
    case Tok::SEMI: {
      auto s = std::make_unique<EmptyStmt>();
      s->line = cur().line;
      advance();
      return s;
    }
    case Tok::CONST: {
      advance();
      return parse_local_decl(/*is_const=*/true);
    }
    case Tok::INT:
      // 不消费 int:parse_local_decl 内部消费
      return parse_local_decl(/*is_const=*/false);
    case Tok::IF: {
      auto s = std::make_unique<IfStmt>();
      s->line = cur().line;
      advance();
      expect(Tok::LPAREN, "'('");
      s->cond = parse_expr();
      expect(Tok::RPAREN, "')'");
      s->then_branch = parse_stmt();
      if (accept(Tok::ELSE)) s->else_branch = parse_stmt();
      return s;
    }
    case Tok::WHILE: {
      auto s = std::make_unique<WhileStmt>();
      s->line = cur().line;
      advance();
      expect(Tok::LPAREN, "'('");
      s->cond = parse_expr();
      expect(Tok::RPAREN, "')'");
      s->body = parse_stmt();
      return s;
    }
    case Tok::BREAK: {
      auto s = std::make_unique<BreakStmt>();
      s->line = cur().line;
      advance();
      expect(Tok::SEMI, "';'");
      return s;
    }
    case Tok::CONTINUE: {
      auto s = std::make_unique<ContinueStmt>();
      s->line = cur().line;
      advance();
      expect(Tok::SEMI, "';'");
      return s;
    }
    case Tok::RETURN: {
      auto s = std::make_unique<ReturnStmt>();
      s->line = cur().line;
      advance();
      if (!check(Tok::SEMI)) s->value = parse_expr();
      expect(Tok::SEMI, "';'");
      return s;
    }
    case Tok::ID: {
      if (tokens_[i_ + 1].kind == Tok::ASSIGN) {  // ID "=" Expr ";" 赋值语句
        auto s = std::make_unique<AssignStmt>();
        s->line = cur().line;
        s->name = cur().text;
        advance();
        advance();  // ID '='
        s->rhs = parse_expr();
        expect(Tok::SEMI, "';'");
        return s;
      }
      return parse_expr_stmt();
    }
    default:
      return parse_expr_stmt();
  }
}

std::unique_ptr<Stmt> Parser::parse_expr_stmt() {
  auto s = std::make_unique<ExprStmt>();
  s->line = cur().line;
  s->expr = parse_expr();
  expect(Tok::SEMI, "';'");
  return s;
}

// ---------- 表达式优先级链(与 C 一致,左结合) ----------
std::unique_ptr<Expr> Parser::parse_lor() {
  auto lhs = parse_land();
  while (accept(Tok::OR_OR)) {
    auto e = std::make_unique<BinaryExpr>();
    e->line = tokens_[i_ - 1].line;
    e->op = Op::OR;
    e->lhs = std::move(lhs);
    e->rhs = parse_land();
    lhs = std::move(e);
  }
  return lhs;
}

std::unique_ptr<Expr> Parser::parse_land() {
  auto lhs = parse_rel();
  while (accept(Tok::AND_AND)) {
    auto e = std::make_unique<BinaryExpr>();
    e->line = tokens_[i_ - 1].line;
    e->op = Op::AND;
    e->lhs = std::move(lhs);
    e->rhs = parse_rel();
    lhs = std::move(e);
  }
  return lhs;
}

std::unique_ptr<Expr> Parser::parse_rel() {
  auto lhs = parse_add();
  for (;;) {
    Op op;
    switch (cur().kind) {
      case Tok::LT: op = Op::LT; break;
      case Tok::GT: op = Op::GT; break;
      case Tok::LE: op = Op::LE; break;
      case Tok::GE: op = Op::GE; break;
      case Tok::EQ_EQ: op = Op::EQ; break;
      case Tok::NE: op = Op::NE; break;
      default: return lhs;
    }
    advance();
    auto e = std::make_unique<BinaryExpr>();
    e->line = tokens_[i_ - 1].line;
    e->op = op;
    e->lhs = std::move(lhs);
    e->rhs = parse_add();
    lhs = std::move(e);
  }
}

std::unique_ptr<Expr> Parser::parse_add() {
  auto lhs = parse_mul();
  for (;;) {
    Op op;
    if (check(Tok::PLUS)) op = Op::ADD;
    else if (check(Tok::MINUS)) op = Op::SUB;
    else return lhs;
    advance();
    auto e = std::make_unique<BinaryExpr>();
    e->line = tokens_[i_ - 1].line;
    e->op = op;
    e->lhs = std::move(lhs);
    e->rhs = parse_mul();
    lhs = std::move(e);
  }
}

std::unique_ptr<Expr> Parser::parse_mul() {
  auto lhs = parse_unary();
  for (;;) {
    Op op;
    if (check(Tok::STAR)) op = Op::MUL;
    else if (check(Tok::SLASH)) op = Op::DIV;
    else if (check(Tok::PERCENT)) op = Op::MOD;
    else return lhs;
    advance();
    auto e = std::make_unique<BinaryExpr>();
    e->line = tokens_[i_ - 1].line;
    e->op = op;
    e->lhs = std::move(lhs);
    e->rhs = parse_unary();
    lhs = std::move(e);
  }
}

std::unique_ptr<Expr> Parser::parse_unary() {
  if (check(Tok::PLUS) || check(Tok::MINUS) || check(Tok::BANG)) {
    auto e = std::make_unique<UnaryExpr>();
    e->line = cur().line;
    e->op = unop_of(cur().kind);
    advance();
    e->operand = parse_unary();
    return e;
  }
  return parse_primary();
}

// ---------- PrimaryExpr → ID | NUMBER | "(" Expr ")" | ID "(" ... ")" ----------
std::unique_ptr<Expr> Parser::parse_primary() {
  if (check(Tok::NUMBER)) {
    auto e = std::make_unique<NumberExpr>();
    e->line = cur().line;
    e->value = cur().num;
    advance();
    return e;
  }
  if (check(Tok::ID)) {
    std::string name = cur().text;
    int line = cur().line;
    advance();
    if (check(Tok::LPAREN)) {  // 函数调用
      advance();
      auto c = std::make_unique<CallExpr>();
      c->line = line;
      c->callee = name;
      if (!check(Tok::RPAREN)) {
        do {
          c->args.push_back(parse_expr());
        } while (accept(Tok::COMMA));
      }
      expect(Tok::RPAREN, "')'");
      return c;
    }
    auto e = std::make_unique<IdExpr>();
    e->line = line;
    e->name = name;
    return e;
  }
  if (accept(Tok::LPAREN)) {
    auto e = parse_expr();
    expect(Tok::RPAREN, "')'");
    return e;
  }
  diag_.error(cur().line, cur().col, "expected expression");
  throw ParseError{};
}

}  // namespace toyc
