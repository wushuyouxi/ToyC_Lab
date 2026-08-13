#pragma once
// M1 词法分析器:ToyC 源码 → Token 流。
//
// 设计要点(负数字面量):语言定义中 NUMBER 的正则为 -?(0|[1-9][0-9]*),
// 但 ToyC 源文件必须能被 C 编译器直接编译,因此 "1-2" 必须是合法的减法。
// 若词法器按正则贪心匹配,会把 "1-2" 切成 NUMBER(1) NUMBER(-2) 导致语法错误。
// 故本实现始终把 '-' 作为 MINUS 运算符产出,NUMBER 只匹配非负十进制整数,
// 负号由解析器按一元运算符处理(语义与原文法完全等价,见报告「负数字面量」)。

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace toyc {

enum class Tok {
  // 字面量 / 标识符
  ID, NUMBER,
  // 关键字
  CONST, INT, VOID, IF, ELSE, WHILE, BREAK, CONTINUE, RETURN,
  // 运算符 / 标点
  PLUS, MINUS, STAR, SLASH, PERCENT, BANG,
  AND_AND, OR_OR,
  LT, GT, LE, GE, EQ_EQ, NE,
  ASSIGN, SEMI, COMMA,
  LPAREN, RPAREN, LBRACE, RBRACE,
  END,  // 输入结束
};

const char* tok_name(Tok t);

struct Token {
  Tok kind = Tok::END;
  std::string text;  // 源码中的原始文本
  int line = 0;      // 1 起始
  int col = 0;       // 1 起始
  int64_t num = 0;   // kind == NUMBER 时的值
};

// 轻量诊断:收集行/列定位的错误信息(stderr 输出内容评测系统不关心)
struct Diag {
  bool has_error = false;
  void error(int line, int col, const std::string& msg);
  void warning(int line, int col, const std::string& msg);  // 不影响退出码
};

class Lexer {
 public:
  Lexer(std::string src, Diag& diag);
  Token next();
  Token peek();  // 单 token 前瞻缓存
  std::vector<Token> tokenize_all();

 private:
  void skip_ws_and_comments();
  Token lex_token();
  Token lex_number(int sl, int sc);
  Token lex_ident(int sl, int sc);
  char advance();
  char cur() const { return pos_ < src_.size() ? src_[pos_] : '\0'; }
  char cur2() const { return pos_ + 1 < src_.size() ? src_[pos_ + 1] : '\0'; }
  Token make(Tok k, size_t start, size_t len, int line, int col);

  std::string src_;
  Diag& diag_;
  size_t pos_ = 0;
  int line_ = 1, col_ = 1;
  std::optional<Token> peeked_;
};

}  // namespace toyc
