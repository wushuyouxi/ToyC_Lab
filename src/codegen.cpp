#include "codegen.h"

#include <climits>
#include <sstream>

#include "opt.h"

namespace toyc {

namespace {

// ---------- 汇编文本缓冲 ----------
struct Asm {
  std::ostringstream os;
  void line(const std::string& s) { os << s << "\n"; }
  void label(const std::string& s) { os << s << ":\n"; }
};

// ---------- 伪指令/真实指令选择 ----------
struct Emit {
  const CodeGenOptions& opt;
  Asm& a;

  void li(const std::string& rd, int64_t imm) {
    int32_t v = static_cast<int32_t>(imm);
    if (opt.use_pseudo) {
      a.line("  li " + rd + ", " + std::to_string(v));
    } else if (v >= -2048 && v <= 2047) {
      a.line("  addi " + rd + ", x0, " + std::to_string(v));
    } else {
      // 标准 lui+addi 序列(hi 需向上调整,保证 addi 符号扩展正确)
      int32_t hi = (v + 0x800) >> 12;
      int32_t lo = v - (hi << 12);
      a.line("  lui " + rd + ", " + std::to_string(hi));
      if (lo) a.line("  addi " + rd + ", " + rd + ", " + std::to_string(lo));
    }
  }
  void mv(const std::string& rd, const std::string& rs) {
    if (rd == rs) return;  // 窥孔:mv x, x 不发射
    a.line(std::string("  ") + (opt.use_pseudo ? "mv " : "addi ") + rd + ", " + rs +
           (opt.use_pseudo ? "" : ", 0"));
  }
  void j(const std::string& L) {
    a.line(std::string("  ") + (opt.use_pseudo ? "j " : "jal x0, ") + L);
  }
  void call(const std::string& f) {
    a.line(std::string("  ") + (opt.use_pseudo ? "call " : "jal ra, ") + f);
  }
  void ret() { a.line(opt.use_pseudo ? "  ret" : "  jalr x0, 0(ra)"); }
  void beqz(const std::string& rs, const std::string& L) {
    a.line(std::string("  ") + (opt.use_pseudo ? "beqz " : "beq ") + rs +
           (opt.use_pseudo ? ", " : ", x0, ") + L);
  }
  void bnez(const std::string& rs, const std::string& L) {
    a.line(std::string("  ") + (opt.use_pseudo ? "bnez " : "bne ") + rs +
           (opt.use_pseudo ? ", " : ", x0, ") + L);
  }
  void neg(const std::string& rd, const std::string& rs) {
    a.line(std::string("  ") + (opt.use_pseudo ? "neg " : "sub ") + rd + ", " +
           (opt.use_pseudo ? rs : std::string("x0, ") + rs));
  }
  void seqz(const std::string& rd, const std::string& rs) {
    a.line(std::string("  ") + (opt.use_pseudo ? "seqz " : "sltiu ") + rd + ", " + rs +
           (opt.use_pseudo ? "" : ", 1"));
  }
  void snez(const std::string& rd, const std::string& rs) {
    a.line(std::string("  ") + (opt.use_pseudo ? "snez " : "sltu ") + rd + ", " +
           (opt.use_pseudo ? rs : std::string("x0, ") + rs));
  }
};

// ---------- 单个函数的汇编生成 ----------
// O0(ra == nullptr):每个虚拟寄存器一个栈槽,指令 = 取操作数 → 运算 → 存回,正确性优先。
// 优化(ra != nullptr):vreg 分配物理寄存器(频率分配,见 opt.cpp),溢出者用紧凑栈槽。
// 帧布局(相对 sp 正偏移):
//   [0, 4*max_out_args)  出参区(被调用者从 s0+4*(i-8) 读取,与调用者布局无关)
//   [slot_base, ...)     vreg 栈槽(O0:每 vreg 一个;优化:仅溢出者,紧凑)
//   [frame-保存区, frame)  ra / s0 / 被使用的 s 寄存器
struct FuncGen {
  const IRFunction& fn;
  Asm& a;
  const CodeGenOptions& opt;
  const RegAlloc* ra;   // nullptr → O0 栈式
  int frame = 0;
  int max_out_args = 0;
  int slot_base = 8;
  bool leaf = false;
  std::vector<int> use_count;  // 各 vreg 在函数中的使用次数(比较-分支融合安全性判定用)

  // vreg v 的栈槽偏移(O0:每 vreg;优化:溢出槽)
  int slot(int v) const {
    if (ra) return slot_base + 4 * ra->alloc[v].slot;
    return slot_base + 4 * v;
  }
  std::string epilogue_label() const { return ".L" + fn.name + "_epilogue"; }

  // ---------- 操作数抽象 ----------
  // O0:所有 vreg 在栈槽;优化:vreg 在物理寄存器或溢出槽。
  struct Operand {
    bool in_reg = false;
    std::string reg;
    int slot = 0;  // sp 偏移(仅 !in_reg 时)
  };
  Operand operand_of(int v) const {
    if (ra && ra->alloc[v].is_reg) return {true, ra->alloc[v].reg, 0};
    return {false, "", slot(v)};
  }
  // 把 vreg v 的值放入物理寄存器 phys(寄存器操作数用 mv,溢出操作数用 lw)
  void load_op(const std::string& phys, int v) {
    Operand o = operand_of(v);
    if (o.in_reg) {
      Emit{opt, a}.mv(phys, o.reg);
    } else {
      a.line("  lw " + phys + ", " + std::to_string(o.slot) + "(sp)");
    }
  }
  // 把物理寄存器 phys 的值存入 vreg v 的位置
  void store_res(int v, const std::string& phys) {
    Operand o = operand_of(v);
    if (o.in_reg) {
      Emit{opt, a}.mv(o.reg, phys);
    } else {
      a.line("  sw " + phys + ", " + std::to_string(o.slot) + "(sp)");
    }
  }
  // 把符号的绝对地址加载到 rd
  void la_addr(const std::string& rd, const std::string& sym) {
    if (opt.use_pseudo) {
      a.line("  la " + rd + ", " + sym);
    } else {
      a.line("  lui " + rd + ", %hi(" + sym + ")");
      a.line("  addi " + rd + ", " + rd + ", %lo(" + sym + ")");
    }
  }

  // ---------- 三操作数指令直发(优化模式) ----------
  // RISC-V 是三操作数指令:操作数在寄存器时直接在目标寄存器上运算,免去 t0/t1 往返。
  // 操作数溢出时才加载到 t0/t1 暂存。
  void emit_binop_direct(const Instr& in, const char* op) {
    Operand A = operand_of(in.rs1), B = operand_of(in.rs2), R = operand_of(in.rd);
    if (R.in_reg) {
      if (A.in_reg && B.in_reg) {
        a.line(std::string("  ") + op + " " + R.reg + ", " + A.reg + ", " + B.reg);
      } else if (A.in_reg) {
        a.line("  lw t1, " + std::to_string(B.slot) + "(sp)");
        a.line(std::string("  ") + op + " " + R.reg + ", " + A.reg + ", t1");
      } else if (B.in_reg) {
        a.line("  lw t0, " + std::to_string(A.slot) + "(sp)");
        a.line(std::string("  ") + op + " " + R.reg + ", t0, " + B.reg);
      } else {
        a.line("  lw t0, " + std::to_string(A.slot) + "(sp)");
        a.line("  lw t1, " + std::to_string(B.slot) + "(sp)");
        a.line(std::string("  ") + op + " t0, t0, t1");
        Emit{opt, a}.mv(R.reg, "t0");
      }
    } else {
      load_op("t0", in.rs1);
      load_op("t1", in.rs2);
      a.line(std::string("  ") + op + " t0, t0, t1");
      a.line("  sw t0, " + std::to_string(R.slot) + "(sp)");
    }
  }
  // 比较直发:swap 时交换操作数(x>y ⟺ y<x;x<=y ⟺ !(y<x);x>=y ⟺ !(x<y))
  void emit_cmp_direct(const Instr& in, bool swap) {
    Operand A = operand_of(in.rs1), B = operand_of(in.rs2), R = operand_of(in.rd);
    if (R.in_reg) {
      if (A.in_reg && B.in_reg) {
        a.line("  slt " + R.reg + ", " + (swap ? B.reg : A.reg) + ", " +
               (swap ? A.reg : B.reg));
      } else if (A.in_reg) {
        a.line("  lw t1, " + std::to_string(B.slot) + "(sp)");
        a.line("  slt " + R.reg + ", " + (swap ? std::string("t1") : A.reg) + ", " +
               (swap ? A.reg : std::string("t1")));
      } else if (B.in_reg) {
        a.line("  lw t0, " + std::to_string(A.slot) + "(sp)");
        a.line("  slt " + R.reg + ", " + (swap ? B.reg : std::string("t0")) + ", " +
               (swap ? std::string("t0") : B.reg));
      } else {
        a.line("  lw t0, " + std::to_string(A.slot) + "(sp)");
        a.line("  lw t1, " + std::to_string(B.slot) + "(sp)");
        a.line("  slt t0, " + std::string(swap ? "t1" : "t0") + ", " +
               std::string(swap ? "t0" : "t1"));
        Emit{opt, a}.mv(R.reg, "t0");
      }
    } else {
      load_op("t0", in.rs1);
      load_op("t1", in.rs2);
      a.line("  slt t0, " + std::string(swap ? "t1" : "t0") + ", " +
             std::string(swap ? "t0" : "t1"));
      a.line("  sw t0, " + std::to_string(R.slot) + "(sp)");
    }
  }
  void emit_xori_rd(const Instr& in) {
    Operand R = operand_of(in.rd);
    if (R.in_reg) {
      a.line("  xori " + R.reg + ", " + R.reg + ", 1");
    } else {
      a.line("  lw t0, " + std::to_string(R.slot) + "(sp)");
      a.line("  xori t0, t0, 1");
      a.line("  sw t0, " + std::to_string(R.slot) + "(sp)");
    }
  }

  void gen() {
    for (const Instr& in : fn.code)
      if (in.op == IROp::CALL && static_cast<int>(in.args.size()) > 8)
        max_out_args = std::max(max_out_args, static_cast<int>(in.args.size()) - 8);
    leaf = ra && ra->leaf;
    slot_base = std::max(8, 4 * max_out_args);
    int spills = ra ? ra->spilled : fn.max_vreg;  // O0:每 vreg 一槽
    int save_area = (leaf ? 0 : 8) + 4 * static_cast<int>(ra ? ra->used_s.size() : 0);
    frame = slot_base + 4 * spills + save_area;
    frame = (frame + 15) & ~15;

    // 使用计数:比较-分支融合要求比较结果仅被紧随的分支使用
    use_count.assign(fn.max_vreg + 8, 0);
    for (const Instr& in : fn.code) {
      auto count = [this](int v) {
        if (v >= 0 && v < static_cast<int>(use_count.size())) ++use_count[v];
      };
      count(in.rs1);
      count(in.rs2);
      for (int a : in.args) count(a);
    }

    a.label(fn.name);
    // 序言
    a.line("  addi sp, sp, -" + std::to_string(frame));
    if (!leaf) {
      a.line("  sw ra, " + std::to_string(frame - 4) + "(sp)");
      a.line("  sw s0, " + std::to_string(frame - 8) + "(sp)");
      a.line("  addi s0, sp, " + std::to_string(frame));
    }
    if (ra) {
      int save_base = leaf ? 4 : 12;
      for (size_t i = 0; i < ra->used_s.size(); ++i)
        a.line("  sw " + ra->used_s[i] + ", " +
               std::to_string(frame - save_base - 4 * static_cast<int>(i)) + "(sp)");
    }

    bool skip_next = false;
    for (size_t idx = 0; idx < fn.code.size(); ++idx) {
      if (skip_next) {
        skip_next = false;
        continue;
      }
      skip_next = gen_instr(fn.code[idx], idx);
    }

    // 公共尾声
    a.label(epilogue_label());
    if (ra) {
      int save_base = leaf ? 4 : 12;
      for (size_t i = 0; i < ra->used_s.size(); ++i)
        a.line("  lw " + ra->used_s[i] + ", " +
               std::to_string(frame - save_base - 4 * static_cast<int>(i)) + "(sp)");
    }
    if (!leaf) {
      a.line("  lw ra, " + std::to_string(frame - 4) + "(sp)");
      a.line("  lw s0, " + std::to_string(frame - 8) + "(sp)");
    }
    a.line("  addi sp, sp, " + std::to_string(frame));
    if (fn.name == "main" && opt.main_exit_ecall) {
      // RARS exit2(新编号 93):a0 = 退出码,进程退出码随之传播(实测验证)。
      a.line("  li a7, 93");
      a.line("  ecall");
    } else {
      Emit{opt, a}.ret();
    }
  }

  static const char* arg_reg(size_t i) {
    static const char* r[] = {"a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7"};
    return r[i];
  }

  // 比较-分支融合:cmp 结果仅被紧随其后的分支使用且只此一处 →
  // 直接把比较与分支合并成一条真实分支指令(如 LE+BRZ → blt y,x)。
  // 返回 true 表示下一条 IR 指令(分支)已被消费,主循环应跳过。
  bool try_fuse_cmp_branch(const Instr& in, size_t idx) {
    if (idx + 1 >= fn.code.size()) return false;
    const Instr& next = fn.code[idx + 1];
    if ((next.op != IROp::BRZ && next.op != IROp::BRNZ) || next.rs1 != in.rd) return false;
    if (use_count[in.rd] != 1) return false;  // 结果被多处使用则不能融合

    // !x + 分支 → 单操作数分支(brz !x ⟺ x≠0 → bnez;brnz !x ⟺ x==0 → beqz)
    if (in.op == IROp::NOT) {
      Operand A = operand_of(in.rs1);
      std::string x;
      if (A.in_reg) x = A.reg;
      else {
        a.line("  lw t0, " + std::to_string(A.slot) + "(sp)");
        x = "t0";
      }
      a.line(std::string("  ") + (next.op == IROp::BRZ ? "bnez " : "beqz ") + x + ", " +
             next.label);
      return true;
    }

    Operand opA = operand_of(in.rs1), opB = operand_of(in.rs2);
    std::string x, y;
    if (opA.in_reg) x = opA.reg;
    else {
      a.line("  lw t0, " + std::to_string(opA.slot) + "(sp)");
      x = "t0";
    }
    if (opB.in_reg) y = opB.reg;
    else {
      a.line("  lw t1, " + std::to_string(opB.slot) + "(sp)");
      y = "t1";
    }
    // 查表:cmp op × 分支条件 → 真实分支指令(x,y 顺序已含语义)
    const char* br = nullptr;
    switch (in.op) {
      case IROp::LT: br = (next.op == IROp::BRZ) ? "bge" : "blt"; break;
      case IROp::GT: br = (next.op == IROp::BRZ) ? "bge" : "blt"; break;
      case IROp::LE: br = (next.op == IROp::BRZ) ? "blt" : "bge"; break;
      case IROp::GE: br = (next.op == IROp::BRZ) ? "blt" : "bge"; break;
      case IROp::EQ: br = (next.op == IROp::BRZ) ? "bne" : "beq"; break;
      case IROp::NE: br = (next.op == IROp::BRZ) ? "beq" : "bne"; break;
      default: return false;
    }
    const std::string* r1 = &x;
    const std::string* r2 = &y;
    if (in.op == IROp::GT || in.op == IROp::LE) std::swap(r1, r2);  // y,x 形式
    a.line(std::string("  ") + br + " " + *r1 + ", " + *r2 + ", " + next.label);
    return true;
  }

  bool gen_instr(const Instr& in, size_t idx) {
    Emit e{opt, a};
    switch (in.op) {
      case IROp::LABEL:
        a.label(in.label);
        break;
      case IROp::MOV:
        if (ra && ra->alloc[in.rd].is_reg) {
          load_op(ra->alloc[in.rd].reg, in.rs1);  // mv 同寄存器时自动跳过
        } else {
          load_op("t0", in.rs1);
          store_res(in.rd, "t0");
        }
        break;
      case IROp::LOADIMM:
        if (ra && ra->alloc[in.rd].is_reg) {
          e.li(ra->alloc[in.rd].reg, in.imm);
        } else {
          e.li("t0", in.imm);
          store_res(in.rd, "t0");
        }
        break;
      case IROp::LOADGLOBAL:
        la_addr("t0", in.sym);
        a.line("  lw t0, 0(t0)");
        store_res(in.rd, "t0");
        break;
      case IROp::STOREGLOBAL:
        load_op("t0", in.rs1);
        la_addr("t1", in.sym);
        a.line("  sw t0, 0(t1)");
        break;
      case IROp::ARG:
        if (in.imm < 8) {
          // a0-a7 仍在寄存器中(序言未动参数寄存器)
          store_res(in.rd, arg_reg(static_cast<size_t>(in.imm)));
        } else {
          // 第 9 个起的实参在调用者帧底出参区(位于本帧上方:sp + frame)
          a.line("  lw t0, " + std::to_string(frame + 4 * (in.imm - 8)) + "(sp)");
          store_res(in.rd, "t0");
        }
        break;
      case IROp::ADD: emit_binop_direct(in, "add"); break;
      case IROp::SUB: emit_binop_direct(in, "sub"); break;
      case IROp::MUL: emit_binop_direct(in, "mul"); break;
      case IROp::DIV: emit_binop_direct(in, "div"); break;
      case IROp::REM: emit_binop_direct(in, "rem"); break;
      case IROp::ADDK: case IROp::SUBK: {
        // rd = rs1 ± imm:imm 落 12 位有符号范围时直发 addi,否则 li+add/sub
        Operand A = operand_of(in.rs1), R = operand_of(in.rd);
        int64_t imm = (in.op == IROp::ADDK) ? in.imm : -in.imm;
        std::string target;  // 结果暂存位置(寄存器)
        bool need_store = true;
        if (R.in_reg) {
          target = R.reg;
          need_store = false;
        } else {
          target = "t0";
        }
        if (imm >= -2048 && imm <= 2047) {
          if (A.in_reg && !need_store) {
            a.line("  addi " + target + ", " + A.reg + ", " + std::to_string(imm));
          } else {
            if (A.in_reg) e.mv(target, A.reg);
            else a.line("  lw " + target + ", " + std::to_string(A.slot) + "(sp)");
            a.line("  addi " + target + ", " + target + ", " + std::to_string(imm));
          }
        } else {
          e.li("t1", in.imm);
          if (A.in_reg && !need_store) {
            a.line(std::string("  ") + (in.op == IROp::ADDK ? "add " : "sub ") + target +
                   ", " + A.reg + ", t1");
          } else {
            if (A.in_reg) e.mv(target, A.reg);
            else a.line("  lw " + target + ", " + std::to_string(A.slot) + "(sp)");
            a.line(std::string("  ") + (in.op == IROp::ADDK ? "add " : "sub ") + target +
                   ", " + target + ", t1");
          }
        }
        if (need_store) a.line("  sw t0, " + std::to_string(R.slot) + "(sp)");
        break;
      }
      case IROp::SHL: case IROp::SRL: case IROp::SRA: {
        Operand A = operand_of(in.rs1), R = operand_of(in.rd);
        const char* op = in.op == IROp::SHL ? "slli" : (in.op == IROp::SRL ? "srli" : "srai");
        if (R.in_reg) {
          if (A.in_reg) {
            a.line(std::string("  ") + op + " " + R.reg + ", " + A.reg + ", " +
                   std::to_string(in.imm & 31));
          } else {
            a.line("  lw " + R.reg + ", " + std::to_string(A.slot) + "(sp)");
            a.line(std::string("  ") + op + " " + R.reg + ", " + R.reg + ", " +
                   std::to_string(in.imm & 31));
          }
        } else {
          load_op("t0", in.rs1);
          a.line(std::string("  ") + op + " t0, t0, " + std::to_string(in.imm & 31));
          store_res(in.rd, "t0");
        }
        break;
      }
      case IROp::NEG:
      case IROp::NOT: {
        if (in.op == IROp::NOT && try_fuse_cmp_branch(in, idx)) return true;
        Operand A = operand_of(in.rs1), R = operand_of(in.rd);
        if (R.in_reg) {
          if (A.in_reg) {
            if (in.op == IROp::NEG) e.neg(R.reg, A.reg);
            else e.seqz(R.reg, A.reg);
          } else {
            a.line("  lw " + R.reg + ", " + std::to_string(A.slot) + "(sp)");
            if (in.op == IROp::NEG) e.neg(R.reg, R.reg);
            else e.seqz(R.reg, R.reg);
          }
        } else {
          load_op("t0", in.rs1);
          if (in.op == IROp::NEG) e.neg("t0", "t0");
          else e.seqz("t0", "t0");
          store_res(in.rd, "t0");
        }
        break;
      }
      case IROp::LT: case IROp::GT: case IROp::LE: case IROp::GE: {
        if (try_fuse_cmp_branch(in, idx)) return true;  // 融合成功:下一条(分支)已被消费
        emit_cmp_direct(in, in.op == IROp::GT || in.op == IROp::LE);
        if (in.op == IROp::LE || in.op == IROp::GE) emit_xori_rd(in);  // <= / >= 补反相
        break;
      }
      case IROp::EQ: case IROp::NE: {
        if (try_fuse_cmp_branch(in, idx)) return true;  // 融合成功:下一条(分支)已被消费
        Operand A = operand_of(in.rs1), B = operand_of(in.rs2), R = operand_of(in.rd);
        if (R.in_reg) {
          if (A.in_reg && B.in_reg) {
            a.line("  xor " + R.reg + ", " + A.reg + ", " + B.reg);
          } else if (A.in_reg) {
            a.line("  lw t1, " + std::to_string(B.slot) + "(sp)");
            a.line("  xor " + R.reg + ", " + A.reg + ", t1");
          } else if (B.in_reg) {
            a.line("  lw t0, " + std::to_string(A.slot) + "(sp)");
            a.line("  xor " + R.reg + ", t0, " + B.reg);
          } else {
            a.line("  lw t0, " + std::to_string(A.slot) + "(sp)");
            a.line("  lw t1, " + std::to_string(B.slot) + "(sp)");
            a.line("  xor t0, t0, t1");
            e.mv(R.reg, "t0");
          }
          if (in.op == IROp::EQ) e.seqz(R.reg, R.reg);
          else e.snez(R.reg, R.reg);
        } else {
          load_op("t0", in.rs1);
          load_op("t1", in.rs2);
          a.line("  xor t0, t0, t1");
          if (in.op == IROp::EQ) e.seqz("t0", "t0");
          else e.snez("t0", "t0");
          store_res(in.rd, "t0");
        }
        break;
      }
      case IROp::JUMP:
        e.j(in.label);
        break;
      case IROp::BRZ: case IROp::BRNZ: {
        Operand A = operand_of(in.rs1);
        if (A.in_reg) {
          if (in.op == IROp::BRZ) e.beqz(A.reg, in.label);
          else e.bnez(A.reg, in.label);
        } else {
          a.line("  lw t0, " + std::to_string(A.slot) + "(sp)");
          if (in.op == IROp::BRZ) e.beqz("t0", in.label);
          else e.bnez("t0", in.label);
        }
        break;
      }
      case IROp::CALL: {
        for (size_t i = 0; i < in.args.size(); ++i) {
          if (i < 8) {
            load_op(arg_reg(i), in.args[i]);
          } else {
            load_op("t0", in.args[i]);
            a.line("  sw t0, " + std::to_string(4 * (i - 8)) + "(sp)");
          }
        }
        e.call(in.func);
        if (in.rd >= 0) store_res(in.rd, "a0");
        break;
      }
      case IROp::RETURN:
        load_op("a0", in.rs1);
        e.j(epilogue_label());
        break;
      case IROp::RETURNVOID:
        e.j(epilogue_label());
        break;
    }
    return false;
  }
};

}  // namespace

std::string generate_assembly(const IRModule& m, const CodeGenOptions& opts) {
  Asm a;

  // 数据段:全局变量(静态初值直接写入;运行时初始化初值先置 0)
  if (!m.globals.empty()) {
    a.line(".data");
    for (const IRGlobal& g : m.globals) {
      a.line(g.name + ": .word " + std::to_string(g.static_init ? *g.static_init : 0));
    }
  }

  a.line(".text");
  a.line(".globl main");
  // 保险:若仿真器从 .text 起始执行(而非跳到 main 标号),先跳 main;
  // 若仿真器直接定位 main,此指令无害。
  a.line("  j main");

  for (const IRFunction& f : m.functions) {
    if (opts.reg_alloc) {
      RegAlloc ra = allocate_registers(f);
      FuncGen{f, a, opts, &ra}.gen();
    } else {
      FuncGen{f, a, opts, nullptr}.gen();
    }
  }

  return a.os.str();
}

}  // namespace toyc
