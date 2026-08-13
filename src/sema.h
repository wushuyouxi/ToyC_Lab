#pragma once
// M3 语义分析:名字解析、类型检查、const 编译期求值、全路径 return 检查。
//
// 检查策略:
// - 精确检查(名字未声明、const 赋值、void 用于值上下文、break 在循环外等)
//   作为 error 报告 —— 合法输入绝不会触发,不会误伤评测用例;
// - 全路径 return 检查是保守算法,作为 warning 报告(宁可漏报不可误报)。

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast.h"
#include "lexer.h"

namespace toyc {

struct Symbol {
  std::string name;
  enum class Kind { GLOBAL_VAR, LOCAL_VAR, CONST, PARAM, FUNC } kind;
  Type type = Type::INT;  // FUNC: 返回类型;其他: INT
  int32_t const_val = 0;  // CONST
  int param_count = 0;    // FUNC
  int stack_offset = 0;   // LOCAL_VAR/PARAM: 栈槽编号(4 字节为单位),codegen 时换算成帧偏移
};

class Sema {
 public:
  explicit Sema(Diag& diag);
  void analyze(CompUnit& unit);

  // 供后端使用:.data 布局 / 函数标签按声明顺序
  const std::vector<Symbol*>& globals_in_order() const { return globals_in_order_; }
  const std::vector<Symbol*>& funcs_in_order() const { return funcs_in_order_; }

 private:
  // ---------- 作用域 ----------
  void push_scope() { scopes_.emplace_back(); }
  void pop_scope() { scopes_.pop_back(); }
  Symbol* lookup(const std::string& name) const;
  Symbol* lookup_global(const std::string& name) const;
  Symbol* declare(const std::string& name, Symbol sym, int line);

  // ---------- 全局层 ----------
  void analyze_global_decl(GlobalDecl& d);
  void analyze_func_def(FuncDef& f);
  void analyze_func(FuncDef& f);

  // ---------- 语句 ----------
  void analyze_stmt(Stmt& s, bool in_loop);

  // ---------- 表达式:返回表达式类型(INT / VOID) ----------
  // value_ctx: 该位置需要"值"(条件、赋值右值、运算操作数、实参、return 值),
  //            void 函数调用出现在值上下文中是语义错误。
  Type analyze_expr(Expr& e, bool value_ctx, bool allow_big_literal = false);

  // ---------- const 编译期求值 ----------
  // 成功返回 true,out 为 int64 域结果(调用方决定截断与范围检查)。
  // && / || 遵循短路规则,右侧可能不求值(与运行期语义一致)。
  bool eval_const(const Expr& e, int64_t& out) const;

  // ---------- 全路径 return(warning 级,保守) ----------
  bool stmt_always_returns(const Stmt& s) const;
  bool stmt_has_break(const Stmt& s, int loop_depth) const;

  Diag& diag_;
  std::vector<std::unordered_map<std::string, Symbol*>> scopes_;  // 作用域栈
  std::deque<Symbol> arena_;                                      // 符号稳定地址池
  std::vector<Symbol*> globals_in_order_;
  std::vector<Symbol*> funcs_in_order_;
  FuncDef* cur_func_ = nullptr;
  int cur_stack_off_ = 0;  // 当前函数的栈槽计数器(形参在前,局部变量接续)
};

}  // namespace toyc
