#pragma once
// M6 优化:-opt 时对 IR 运行优化遍,并提供频率寄存器分配供代码生成使用。
//
// 优化遍(全部保证语义等价,与 O0 输出对照验证):
//   1. 不可达代码删除(无条件跳转/return 之后到下一个标签之前)+ 跳转到下一条指令消除
//   2. 常量折叠 + 常量/拷贝传播(顺扫;到达标签清空常量表;按 int32 回绕语义折叠)
//   3. 强度削减(乘/除/模 2^k → 移位序列,带负数修正)
//   4. 死代码删除(基于 CFG 的精确活跃性分析)
//   5. 未引用标签删除
//   反复迭代至不动点。
//
// 寄存器分配:频率分配器——每个物理寄存器至多分配给一个 vreg(无重叠判定,天然正确);
// 跨调用存活的 vreg 只能用被调用者保存寄存器(s1-s11),其余可用 t3-t6/a0-a7;
// 按使用频率降序分配,分配不到寄存器的溢出到栈槽。t0/t1/t2 留作代码生成暂存。

#include <string>
#include <vector>

#include "ir.h"

namespace toyc {

void optimize(IRModule& m);

struct Allocation {
  bool is_reg = false;
  std::string reg;  // 物理寄存器名(如 "s1"),is_reg 时有效
  int slot = -1;    // 溢出槽编号(0 起,紧凑),!is_reg 时有效
};

struct RegAlloc {
  std::vector<Allocation> alloc;      // 按 vreg 编号
  std::vector<std::string> used_s;    // 需在序言/尾声保存的 s 寄存器
  bool leaf = false;                  // 无 CALL → 免存 ra/s0
  int spilled = 0;                    // 溢出槽数量
};

RegAlloc allocate_registers(const IRFunction& fn);

}  // namespace toyc
