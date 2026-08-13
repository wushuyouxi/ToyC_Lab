#pragma once
// M4 三地址码中间表示:虚拟寄存器 + 标签 + 平坦指令序列。
//
// 设计要点:
// - 局部变量/形参直接提升为虚拟寄存器(ToyC 无指针、无取地址,地址绝不逃逸,提升安全),
//   这是寄存器分配优化的前提;全局变量通过 LOADGLOBAL/STOREGLOBAL 显式访问;
//   const 引用直接 LOADIMM 其编译期值,不占任何存储。
// - 短路求值(&&/||)在 IR 构建时降级为条件跳转结构,控制流显式化。
// - O0 与 -opt 共用同一份 IR:差别只在代码生成阶段(每个 vreg 一个栈槽 vs 寄存器分配),
//   保证两条路径都经过完整测试。
// - 函数调用:实参在 CALL 指令的 args 列表中;被调函数入口用 ARG 指令把第 imm 号实参
//   拷贝到形参 vreg(前 8 个走 a0-a7,超出部分压栈,由代码生成层实现约定)。

#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast.h"
#include "lexer.h"  // Diag

namespace toyc {

enum class IROp {
  MOV,          // rd = rs1
  LOADIMM,      // rd = imm
  LOADGLOBAL,   // rd = [sym]
  STOREGLOBAL,  // [sym] = rs1
  ARG,          // rd = 第 imm 号实参(函数入口)
  ADD, SUB, MUL, DIV, REM,
  ADDK, SUBK,              // rd = rs1 ± imm(常量传播引入,直发 addi)
  NEG, NOT,
  SHL, SRL, SRA,           // 移位(强度削减引入,imm = 移位量)
  LT, GT, LE, GE, EQ, NE,  // rd = (rs1 cmp rs2) ? 1 : 0
  JUMP,                    // → label
  BRZ, BRNZ,               // rs1 == 0 / rs1 != 0 → label
  CALL,                    // rd = call func(args...);void 函数无 rd
  RETURN,                  // return rs1
  RETURNVOID,
  LABEL,  // label:
};

const char* irop_name(IROp op);

struct Instr {
  IROp op;
  int rd = -1, rs1 = -1, rs2 = -1;
  int64_t imm = 0;
  std::string label;      // JUMP/BRZ/BRNZ 目标;LABEL 自身名
  std::string func;       // CALL 被调函数
  std::string sym;        // LOADGLOBAL/STOREGLOBAL 全局符号
  std::vector<int> args;  // CALL 实参 vreg
  int line = 0;           // 来源行号(调试用)
};

struct IRFunction {
  std::string name;
  Type ret_type = Type::INT;
  int param_count = 0;
  std::vector<Instr> code;
  int max_vreg = 0;  // 实际用到的 vreg 编号数(= 最大编号 + 1)
};

struct IRGlobal {
  std::string name;
  std::optional<int32_t> static_init;  // 编译期可确定的初值 → .data 静态初始化;
                                       // 否则在 main 入口生成运行时初始化代码
};

struct IRModule {
  std::vector<IRGlobal> globals;
  std::vector<IRFunction> functions;
  void dump(std::ostream& os) const;
};

// AST(已通过语义分析,含符号注解)→ IR
class IRBuilder {
 public:
  explicit IRBuilder(Diag& diag) : diag_(diag) {}
  IRModule build(const CompUnit& unit);

 private:
  struct LoopCtx {
    std::string cont_label, break_label;
  };

  int new_vreg() { return next_vreg_++; }
  std::string new_label() { return ".L" + cur_func_->name + "_" + std::to_string(label_seq_++); }
  void emit(const Instr& in) { cur_func_->code.push_back(in); }

  int gen_expr(const Expr& e);                        // 返回结果 vreg
  void gen_cond_false(const Expr& e, const std::string& L);  // e 为假 → 跳 L
  void gen_cond_true(const Expr& e, const std::string& L);   // e 为真 → 跳 L
  void gen_stmt(const Stmt& s);
  void gen_block(const BlockStmt& b);
  void gen_func(const FuncDef& f);
  void gen_global_runtime_init();  // main 入口前初始化无静态初值的全局变量

  IRModule* mod_ = nullptr;
  IRFunction* cur_func_ = nullptr;
  std::vector<const GlobalDecl*> runtime_init_globals_;
  std::vector<LoopCtx> loops_;
  std::unordered_map<Symbol*, int> var_vregs_;  // 符号 → vreg(Symbol* 全局唯一,平铺即可)
  int next_vreg_ = 0;
  int label_seq_ = 0;
  Diag& diag_;
};

}  // namespace toyc
