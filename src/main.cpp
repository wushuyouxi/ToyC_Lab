// ToyC 编译器入口:stdin 读源码 → stdout 写 RISC-V32 汇编。
// 调试模式(--dump-*)仅本地开发使用,评测系统只传 -opt。

#include <iostream>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "codegen.h"
#include "ir.h"
#include "lexer.h"
#include "opt.h"
#include "parser.h"
#include "sema.h"

using namespace toyc;

int main(int argc, char* argv[]) {
  bool opt = false;
  std::string mode;  // "", "tokens", "ast", "check", "ir"
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "-opt") opt = true;
    else if (a == "--dump-tokens") mode = "tokens";
    else if (a == "--dump-ast") mode = "ast";
    else if (a == "--check") mode = "check";
    else if (a == "--dump-ir") mode = "ir";
    else {
      std::cerr << "toyc: unknown option '" << a << "'\n";
      return 2;
    }
  }

#ifdef _WIN32
  // 二进制模式:防止重定向输入时 CRLF 转换、输出时 \n → \r\n
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif

  std::ostringstream buf;
  buf << std::cin.rdbuf();
  std::string src = buf.str();

  Diag diag;
  Lexer lexer(src, diag);
  std::vector<Token> tokens = lexer.tokenize_all();

  if (mode == "tokens") {
    for (const Token& t : tokens)
      std::cout << t.line << ":" << t.col << "  " << tok_name(t.kind) << "  '" << t.text << "'\n";
    return diag.has_error ? 1 : 0;
  }

  std::unique_ptr<CompUnit> unit;
  try {
    Parser parser(tokens, diag);
    unit = parser.parse_comp_unit();
  } catch (const ParseError&) {
    return 1;
  }
  if (diag.has_error) return 1;

  Sema sema(diag);
  sema.analyze(*unit);
  if (diag.has_error) return 1;

  if (mode == "ast") {  // 语义分析之后的带注解 AST(报告素材)
    unit->dump(std::cout);
    return 0;
  }
  if (mode == "check") return 0;  // 只做前端检查

  IRModule ir = IRBuilder(diag).build(*unit);
  if (opt) optimize(ir);
  if (mode == "ir") {
    ir.dump(std::cout);
    return 0;
  }

  CodeGenOptions cg_opts;
  cg_opts.reg_alloc = opt;
  std::cout << generate_assembly(ir, cg_opts);
  return 0;
}
