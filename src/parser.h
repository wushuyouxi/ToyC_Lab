#pragma once
// M2 语法分析:手写递归下降,文法见课程文档「ToyC 语言的文法」。
// 文法为 LL(1)(ID 后跟 '=' 还是 '(' 通过直接索引 token 数组判定,即 LL(2) 判定、单次前瞻)。
// 所有产生式一一对应成员函数,便于对照文法阅读与报告撰写。

#include <memory>
#include <vector>

#include "ast.h"
#include "lexer.h"

namespace toyc {

struct ParseError {};  // 解析失败后抛出,由 main 捕获

class Parser {
 public:
  Parser(std::vector<Token> tokens, Diag& diag);
  std::unique_ptr<CompUnit> parse_comp_unit();

 private:
  const Token& cur() const { return tokens_[i_]; }
  bool check(Tok k) const { return cur().kind == k; }
  void advance() {
    if (i_ + 1 < tokens_.size()) ++i_;  // 永远停在 END
  }
  bool accept(Tok k) {
    if (check(k)) {
      advance();
      return true;
    }
    return false;
  }
  void expect(Tok k, const char* what);

  // 顶层
  std::unique_ptr<GlobalDecl> parse_global_decl(bool is_const);  // 已消耗 const/int
  std::unique_ptr<FuncDef> parse_func_def(Type ret);             // 已消耗返回类型
  // 语句
  std::unique_ptr<Stmt> parse_stmt();
  std::unique_ptr<BlockStmt> parse_block();
  std::unique_ptr<Stmt> parse_expr_stmt();
  std::unique_ptr<DeclStmt> parse_local_decl(bool is_const);     // 已消耗 const/int
  // 表达式(优先级链)
  std::unique_ptr<Expr> parse_expr() { return parse_lor(); }
  std::unique_ptr<Expr> parse_lor();
  std::unique_ptr<Expr> parse_land();
  std::unique_ptr<Expr> parse_rel();
  std::unique_ptr<Expr> parse_add();
  std::unique_ptr<Expr> parse_mul();
  std::unique_ptr<Expr> parse_unary();
  std::unique_ptr<Expr> parse_primary();

  std::vector<Token> tokens_;
  size_t i_ = 0;
  Diag& diag_;
};

}  // namespace toyc
