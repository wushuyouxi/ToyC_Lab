#pragma once
// M2 抽象语法树:继承体系 + unique_ptr 所有权。
// 语义分析完成后通过节点上的 sym/constVal 字段回填符号与常量值(单遍编译器,不做旁路 map)。

#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <variant>
#include <vector>

namespace toyc {

struct Symbol;  // 定义于 sema.h

enum class Op {          // 运算符
  ADD, SUB, MUL, DIV, MOD,  // 算术
  AND, OR,                  // 逻辑(短路)
  LT, GT, LE, GE, EQ, NE,   // 关系
  POS, NEG, NOT,            // 一元
};

enum class Type { INT, VOID };

const char* op_name(Op op);
const char* type_name(Type t);

// ---------- 表达式 ----------
struct Expr {
  virtual ~Expr() = default;
  int line = 0;  // 报错定位用
  virtual void dump(std::ostream& os, int depth) const = 0;
};

struct NumberExpr : Expr {
  int64_t value;
  void dump(std::ostream& os, int depth) const override;
};

struct IdExpr : Expr {
  std::string name;
  Symbol* sym = nullptr;  // 语义分析回填
  void dump(std::ostream& os, int depth) const override;
};

struct UnaryExpr : Expr {
  Op op;
  std::unique_ptr<Expr> operand;
  void dump(std::ostream& os, int depth) const override;
};

struct BinaryExpr : Expr {
  Op op;
  std::unique_ptr<Expr> lhs, rhs;
  void dump(std::ostream& os, int depth) const override;
};

struct CallExpr : Expr {
  std::string callee;
  std::vector<std::unique_ptr<Expr>> args;
  Symbol* sym = nullptr;  // 语义分析回填(被调函数符号)
  void dump(std::ostream& os, int depth) const override;
};

// ---------- 语句 ----------
struct Stmt {
  virtual ~Stmt() = default;
  int line = 0;
  virtual void dump(std::ostream& os, int depth) const = 0;
};

struct BlockStmt : Stmt {
  std::vector<std::unique_ptr<Stmt>> stmts;
  void dump(std::ostream& os, int depth) const override;
};

struct EmptyStmt : Stmt {
  void dump(std::ostream& os, int depth) const override;
};

struct ExprStmt : Stmt {
  std::unique_ptr<Expr> expr;
  void dump(std::ostream& os, int depth) const override;
};

struct AssignStmt : Stmt {
  std::string name;
  std::unique_ptr<Expr> rhs;
  Symbol* sym = nullptr;  // 语义分析回填
  void dump(std::ostream& os, int depth) const override;
};

struct DeclStmt : Stmt {  // 局部声明:int / const int ID "=" Expr ";"
  bool is_const;
  std::string name;
  std::unique_ptr<Expr> init;
  Symbol* sym = nullptr;
  void dump(std::ostream& os, int depth) const override;
};

struct IfStmt : Stmt {
  std::unique_ptr<Expr> cond;
  std::unique_ptr<Stmt> then_branch;
  std::unique_ptr<Stmt> else_branch;  // 可为空
  void dump(std::ostream& os, int depth) const override;
};

struct WhileStmt : Stmt {
  std::unique_ptr<Expr> cond;
  std::unique_ptr<Stmt> body;
  void dump(std::ostream& os, int depth) const override;
};

struct BreakStmt : Stmt {
  void dump(std::ostream& os, int depth) const override;
};

struct ContinueStmt : Stmt {
  void dump(std::ostream& os, int depth) const override;
};

struct ReturnStmt : Stmt {
  std::unique_ptr<Expr> value;  // void 返回时为 null
  void dump(std::ostream& os, int depth) const override;
};

// ---------- 全局层 ----------
struct GlobalDecl {  // int / const int ID "=" Expr ";"
  bool is_const;
  std::string name;
  std::unique_ptr<Expr> init;
  int line = 0;
  std::optional<int32_t> folded_init;  // 语义分析:初始化可编译期求值时记录,供 .data 静态初始化
  void dump(std::ostream& os, int depth) const;
};

struct Param {
  std::string name;
  int line = 0;
  Symbol* sym = nullptr;  // 语义分析回填
};

struct FuncDef {
  Type ret_type;
  std::string name;
  std::vector<Param> params;
  std::unique_ptr<BlockStmt> body;
  int line = 0;
  void dump(std::ostream& os, int depth) const;
};

struct CompUnit {
  // 按源码顺序存放顶层项(顺序敏感:调用必须发生在声明之后)。
  // variant<GlobalDecl, FuncDef> 天然保持原始顺序。
  std::vector<std::variant<GlobalDecl, FuncDef>> items;
  void dump(std::ostream& os) const;
};

}  // namespace toyc
