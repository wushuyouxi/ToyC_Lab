#pragma once
// M5 RISC-V32 汇编代码生成。
//
// 输出约定层:评测环境的汇编方言差异集中在本文件(CodeGenOptions 与 main 退出序列)。
// 拿到课程示例汇编后,只需调整这里的约定,不影响其余编译器代码。

#include <string>

#include "ir.h"

namespace toyc {

struct CodeGenOptions {
  bool use_pseudo = true;      // li/mv/j/ret/call/beqz 等伪指令(RARS 均支持)
  bool use_m_ext = true;       // RV32IM:mul/div/rem 直接一条指令
  bool main_exit_ecall = true; // main 以 exit2(li a7,93; ecall,a0=退出码)结束;
                               // 关闭时 main 用 ret 正常返回
  bool reg_alloc = false;      // -opt:频率寄存器分配 + 叶子函数优化
};

std::string generate_assembly(const IRModule& m, const CodeGenOptions& opts);

}  // namespace toyc
