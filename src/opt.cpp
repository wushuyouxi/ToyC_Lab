#include "opt.h"

#include <algorithm>
#include <climits>
#include <unordered_map>
#include <unordered_set>

namespace toyc {

namespace {

// 无副作用、可删除死定义的指令
bool is_pure_arith(IROp op) {
  switch (op) {
    case IROp::MOV: case IROp::LOADIMM: case IROp::LOADGLOBAL: case IROp::ARG:
    case IROp::ADD: case IROp::SUB: case IROp::MUL: case IROp::DIV: case IROp::REM:
    case IROp::ADDK: case IROp::SUBK:
    case IROp::NEG: case IROp::NOT:
    case IROp::SHL: case IROp::SRL: case IROp::SRA:
    case IROp::LT: case IROp::GT: case IROp::LE: case IROp::GE: case IROp::EQ: case IROp::NE:
      return true;
    default: return false;
  }
}

// ---------- 遍 1:不可达代码删除 + 跳转到下一条指令 + 未引用标签 ----------
// 本编译器生成的所有跳转都以 LABEL 为目标,因此:
//   JUMP/RETURN/RETURNVOID 之后到下一个 LABEL 之前的指令不可达;
//   跳转到紧跟其后的标签是多余指令;
//   未被任何跳转引用的标签可删除。
bool eliminate_unreachable(IRFunction& f) {
  std::unordered_set<std::string> used_labels;
  for (const auto& in : f.code)
    if (in.op == IROp::JUMP || in.op == IROp::BRZ || in.op == IROp::BRNZ)
      used_labels.insert(in.label);

  std::vector<Instr> out;
  bool changed = false;
  for (size_t i = 0; i < f.code.size(); ++i) {
    out.push_back(f.code[i]);
    if (f.code[i].op == IROp::JUMP || f.code[i].op == IROp::RETURN ||
        f.code[i].op == IROp::RETURNVOID) {
      size_t j = i + 1;
      while (j < f.code.size() && f.code[j].op != IROp::LABEL) {
        changed = true;
        ++j;
      }
      i = j - 1;  // 循环自增后落在 LABEL 上
    }
  }
  // 跳转到下一条指令:消除
  std::vector<Instr> out2;
  for (size_t i = 0; i < out.size(); ++i) {
    if (out[i].op == IROp::JUMP && i + 1 < out.size() && out[i + 1].op == IROp::LABEL &&
        out[i + 1].label == out[i].label) {
      changed = true;
      continue;
    }
    out2.push_back(std::move(out[i]));
  }
  // 未引用标签:删除
  std::vector<Instr> out3;
  for (auto& in : out2) {
    if (in.op == IROp::LABEL && !used_labels.count(in.label)) {
      changed = true;
      continue;
    }
    out3.push_back(std::move(in));
  }
  f.code = std::move(out3);
  return changed;
}

// ---------- 遍 2:常量折叠 + 常量/拷贝传播 ----------
// 顺扫维护 known(vreg→int64) 与 alias(vreg→源 vreg,MOV 链)。
// 到达 LABEL 清空(控制流汇合,保守)。
// 算术折叠一律在 int32 回绕语义下进行:运行时 RV32 指令自然回绕,
// 用 (int32_t) 强制截断(C++20 定义良好)保证一致。
bool constant_propagation(IRFunction& f) {
  std::unordered_map<int, int64_t> known;
  std::unordered_map<int, int> alias;
  bool changed = false;

  auto resolve = [&](int v) {
    int steps = 0;
    while (alias.count(v) && steps++ < 1000) v = alias[v];
    return v;
  };
  auto kill_aliases_to = [&](int v) {
    for (auto it = alias.begin(); it != alias.end();) {
      if (it->second == v) it = alias.erase(it);
      else ++it;
    }
  };

  std::vector<Instr> out;
  for (auto& in : f.code) {
    if (in.op == IROp::LABEL) {
      known.clear();
      alias.clear();
      out.push_back(std::move(in));
      continue;
    }
    // 各类"定义"指令:先失效旧常量/别名
    if (in.rd >= 0 && in.op != IROp::MOV) {
      known.erase(in.rd);
      kill_aliases_to(in.rd);
    }

    switch (in.op) {
      case IROp::LOADIMM:
        known[in.rd] = in.imm;
        break;
      case IROp::MOV: {
        int src = resolve(in.rs1);
        kill_aliases_to(in.rd);
        alias[in.rd] = src;
        auto it = known.find(src);
        if (it != known.end()) {  // 拷贝传播 + 折叠
          known[in.rd] = it->second;
          in.op = IROp::LOADIMM;
          in.imm = it->second;
          in.rs1 = -1;
          changed = true;
        }
        break;
      }
      case IROp::ADD: case IROp::SUB: case IROp::MUL: case IROp::DIV: case IROp::REM:
      case IROp::LT: case IROp::GT: case IROp::LE: case IROp::GE: case IROp::EQ: case IROp::NE: {
        auto l = known.find(resolve(in.rs1));
        auto r = known.find(resolve(in.rs2));
        // 单常量加减 → ADDK/SUBK(代码生成直发 addi,省 li+add 两条)
        if (in.op == IROp::ADD && l == known.end() && r != known.end()) {
          in.op = IROp::ADDK;
          in.imm = static_cast<int32_t>(r->second);
          in.rs2 = -1;
          changed = true;
          break;
        }
        if (in.op == IROp::ADD && l != known.end() && r == known.end()) {
          in.op = IROp::ADDK;
          in.imm = static_cast<int32_t>(l->second);
          in.rs1 = in.rs2;  // 加法交换律
          in.rs2 = -1;
          changed = true;
          break;
        }
        if (in.op == IROp::SUB && r != known.end() && l == known.end()) {
          in.op = IROp::SUBK;
          in.imm = static_cast<int32_t>(r->second);
          in.rs2 = -1;
          changed = true;
          break;
        }
        if (l != known.end() && r != known.end()) {
          int64_t a = l->second, b = r->second;
          int64_t res = 0;
          bool ok = true;
          switch (in.op) {
            case IROp::ADD: res = static_cast<int32_t>(a + b); break;
            case IROp::SUB: res = static_cast<int32_t>(a - b); break;
            case IROp::MUL: res = static_cast<int32_t>(a * b); break;
            case IROp::DIV:
              if (b == 0 || (a == INT32_MIN && b == -1)) ok = false;  // UB 域,保守不折
              else res = a / b;
              break;
            case IROp::REM:
              if (b == 0 || (a == INT32_MIN && b == -1)) ok = false;
              else res = a % b;
              break;
            case IROp::LT: res = (a < b) ? 1 : 0; break;
            case IROp::GT: res = (a > b) ? 1 : 0; break;
            case IROp::LE: res = (a <= b) ? 1 : 0; break;
            case IROp::GE: res = (a >= b) ? 1 : 0; break;
            case IROp::EQ: res = (a == b) ? 1 : 0; break;
            case IROp::NE: res = (a != b) ? 1 : 0; break;
            default: ok = false; break;
          }
          if (ok) {
            in.op = IROp::LOADIMM;
            in.imm = res;
            in.rs1 = in.rs2 = -1;
            known[in.rd] = res;
            changed = true;
          }
        }
        break;
      }
      case IROp::ADDK: case IROp::SUBK: {
        auto it = known.find(resolve(in.rs1));
        if (it != known.end()) {
          int64_t a = it->second;
          int64_t res = (in.op == IROp::ADDK) ? static_cast<int32_t>(a + in.imm)
                                              : static_cast<int32_t>(a - in.imm);
          in.op = IROp::LOADIMM;
          in.imm = res;
          in.rs1 = -1;
          known[in.rd] = res;
          changed = true;
        }
        break;
      }
      case IROp::NEG: case IROp::NOT: {
        auto it = known.find(resolve(in.rs1));
        if (it != known.end()) {
          int64_t res = (in.op == IROp::NEG) ? static_cast<int32_t>(-it->second)
                                             : (it->second == 0 ? 1 : 0);
          in.op = IROp::LOADIMM;
          in.imm = res;
          in.rs1 = -1;
          known[in.rd] = res;
          changed = true;
        }
        break;
      }
      case IROp::SHL: case IROp::SRL: case IROp::SRA: {
        auto it = known.find(resolve(in.rs1));
        if (it != known.end()) {
          uint32_t a = static_cast<uint32_t>(it->second);
          int64_t res = 0;
          if (in.op == IROp::SHL) res = static_cast<int32_t>(a << (in.imm & 31));
          else if (in.op == IROp::SRL) res = static_cast<int32_t>(a >> (in.imm & 31));
          else res = static_cast<int32_t>(static_cast<int32_t>(a) >> (in.imm & 31));
          in.op = IROp::LOADIMM;
          in.imm = res;
          in.rs1 = -1;
          known[in.rd] = res;
          changed = true;
        }
        break;
      }
      case IROp::BRZ: {
        auto it = known.find(resolve(in.rs1));
        if (it != known.end()) {
          changed = true;
          if (it->second == 0) {
            in.op = IROp::JUMP;  // 恒跳
            in.rs1 = -1;
          } else {
            continue;  // 永不跳:删除
          }
        }
        break;
      }
      case IROp::BRNZ: {
        auto it = known.find(resolve(in.rs1));
        if (it != known.end()) {
          changed = true;
          if (it->second != 0) {
            in.op = IROp::JUMP;
            in.rs1 = -1;
          } else {
            continue;
          }
        }
        break;
      }
      default:
        break;
    }
    out.push_back(std::move(in));
  }
  f.code = std::move(out);
  return changed;
}

// ---------- 遍 3:强度削减(乘/除/模 2^k) ----------
// x*2^k → slli;x/2^k → 带符号修正的算术右移序列;
// x%2^k → x - (x/2^k)*2^k。负数修正必不可少(经典陷阱)。
bool strength_reduction(IRFunction& f) {
  auto is_pow2 = [](int64_t c) { return c > 0 && (c & (c - 1)) == 0; };
  auto log2c = [](int64_t c) {
    int k = 0;
    while (c > 1) { c >>= 1; ++k; }
    return k;
  };

  std::unordered_map<int, int64_t> imm_val;  // 顺扫追踪常量 vreg(简化版)
  std::vector<Instr> out;
  bool changed = false;

  auto mk = [](IROp op, int rd, int rs1, int rs2, int64_t imm, int line) {
    Instr in;
    in.op = op;
    in.rd = rd;
    in.rs1 = rs1;
    in.rs2 = rs2;
    in.imm = imm;
    in.line = line;
    return in;
  };

  auto emit_seq = [&](const Instr& in, int64_t c) -> bool {
    // 常量在 rs2 或 rs1 上;x 是另一个操作数
    int x = (imm_val.count(in.rs2)) ? in.rs1 : in.rs2;
    int k = log2c(c);
    switch (in.op) {
      case IROp::MUL:
        if (c == 0) {
          out.push_back(mk(IROp::LOADIMM, in.rd, -1, -1, 0, in.line));
        } else if (c == 1) {
          out.push_back(mk(IROp::MOV, in.rd, x, -1, 0, in.line));
        } else {
          out.push_back(mk(IROp::SHL, in.rd, x, -1, k, in.line));
        }
        changed = true;
        return true;
      case IROp::DIV:
        // 注:RV32IM 上 div 已是单指令,除以 2^k 的移位修正序列(4 条)反而更慢,
        // 故只做"除 1"特化(消除指令与依赖)。
        if (c == 1) {
          out.push_back(mk(IROp::MOV, in.rd, x, -1, 0, in.line));
          changed = true;
          return true;
        }
        return false;
      case IROp::REM:
        // 同上:rem 已是单指令,只做"模 1 恒 0"特化。
        if (c == 1) {
          out.push_back(mk(IROp::LOADIMM, in.rd, -1, -1, 0, in.line));
          changed = true;
          return true;
        }
        return false;
      default:
        return false;
    }
  };

  for (auto& in : f.code) {
    if (in.op == IROp::LABEL) {
      imm_val.clear();
      out.push_back(std::move(in));
      continue;
    }
    if (in.op == IROp::LOADIMM) imm_val[in.rd] = in.imm;
    else if (in.rd >= 0) imm_val.erase(in.rd);
    if (in.op == IROp::MOV && imm_val.count(in.rs1)) imm_val[in.rd] = imm_val[in.rs1];

    if (in.op == IROp::MUL || in.op == IROp::DIV || in.op == IROp::REM) {
      bool have_const = false;
      int64_t c = 0;
      if (imm_val.count(in.rs2)) {
        have_const = true;
        c = imm_val[in.rs2];
      } else if (in.op == IROp::MUL && imm_val.count(in.rs1)) {
        have_const = true;
        c = imm_val[in.rs1];
      }
      // 仅当操作数确实是已知常量才削减:正数 2 的幂、1、0(MUL 乘 0 / REM 模 1 特化);
      // DIV/REM 除数为 0 时 emit_seq 拒绝
      if (have_const && c >= 0 && (c == 0 || is_pow2(c)) && emit_seq(in, c)) continue;
    }
    out.push_back(std::move(in));
  }
  f.code = std::move(out);
  return changed;
}

// ---------- 遍 4:死代码删除(CFG 上的精确活跃性分析) ----------
bool dead_code_elimination(IRFunction& f) {
  const int n = static_cast<int>(f.code.size());
  // 块划分:LABEL 开新块;跳转/返回结束块
  struct Block { int start, end; };  // [start, end)
  std::vector<Block> blocks;
  int start = 0;
  for (int i = 0; i < n; ++i) {
    bool term = f.code[i].op == IROp::JUMP || f.code[i].op == IROp::BRZ ||
                f.code[i].op == IROp::BRNZ || f.code[i].op == IROp::RETURN ||
                f.code[i].op == IROp::RETURNVOID;
    if (f.code[i].op == IROp::LABEL && i > start) {
      blocks.push_back({start, i});
      start = i;
    } else if (term) {
      blocks.push_back({start, i + 1});
      start = i + 1;
    }
  }
  if (start < n) blocks.push_back({start, n});

  std::unordered_map<std::string, int> label_block;
  for (size_t b = 0; b < blocks.size(); ++b)
    if (f.code[blocks[b].start].op == IROp::LABEL)
      label_block[f.code[blocks[b].start].label] = static_cast<int>(b);

  // 后继
  std::vector<std::vector<int>> succ(blocks.size());
  for (size_t b = 0; b < blocks.size(); ++b) {
    const Instr& last = f.code[blocks[b].end - 1];
    if (last.op == IROp::JUMP) {
      succ[b].push_back(label_block[last.label]);
    } else if (last.op == IROp::BRZ || last.op == IROp::BRNZ) {
      if (b + 1 < blocks.size()) succ[b].push_back(static_cast<int>(b) + 1);
      succ[b].push_back(label_block[last.label]);
    } else if (b + 1 < blocks.size()) {
      succ[b].push_back(static_cast<int>(b) + 1);
    }
  }

  // gen / kill
  std::vector<std::unordered_set<int>> gen(blocks.size()), kill(blocks.size());
  for (size_t b = 0; b < blocks.size(); ++b) {
    std::unordered_set<int> seen_def;
    for (int i = blocks[b].start; i < blocks[b].end; ++i) {
      const Instr& in = f.code[i];
      auto add_use = [&](int v) {
        if (v >= 0 && !seen_def.count(v)) gen[b].insert(v);
      };
      add_use(in.rs1);
      add_use(in.rs2);
      for (int a : in.args) add_use(a);
      if (in.rd >= 0 && in.op != IROp::LABEL) {
        seen_def.insert(in.rd);
        kill[b].insert(in.rd);
      }
    }
  }

  // 迭代不动点(标准反向数据流)
  std::vector<std::unordered_set<int>> live_in(blocks.size()), live_out(blocks.size());
  bool changed = true;
  while (changed) {
    changed = false;
    for (int b = static_cast<int>(blocks.size()) - 1; b >= 0; --b) {
      std::unordered_set<int> out;
      for (int s : succ[b]) out.insert(live_in[s].begin(), live_in[s].end());
      std::unordered_set<int> in = out;
      for (int v : kill[b]) in.erase(v);
      in.insert(gen[b].begin(), gen[b].end());
      if (in != live_in[b] || out != live_out[b]) {
        changed = true;
        live_in[b] = std::move(in);
        live_out[b] = std::move(out);
      }
    }
  }

  // 逆扫删除死定义
  std::vector<Instr> out_code;
  bool removed = false;
  for (size_t b = 0; b < blocks.size(); ++b) {
    std::unordered_set<int> live = live_out[b];
    std::vector<Instr> block_code;
    for (int i = blocks[b].end - 1; i >= blocks[b].start; --i) {
      Instr& in = f.code[i];
      if (is_pure_arith(in.op) && in.rd >= 0 && !live.count(in.rd)) {
        removed = true;
        continue;  // 死定义:删除
      }
      if (in.op == IROp::CALL && in.rd >= 0 && !live.count(in.rd)) {
        in.rd = -1;  // 保留调用副作用,丢弃返回值
        removed = true;
      }
      if (in.rd >= 0 && in.op != IROp::LABEL) live.erase(in.rd);
      live.insert(in.rs1);
      live.insert(in.rs2);
      for (int a : in.args) live.insert(a);
      block_code.push_back(std::move(in));
    }
    std::reverse(block_code.begin(), block_code.end());
    out_code.insert(out_code.end(), block_code.begin(), block_code.end());
  }
  f.code = std::move(out_code);
  return removed;
}

}  // namespace

// ---------- 优化入口:各遍迭代至不动点 ----------
void optimize(IRModule& m) {
  for (auto& f : m.functions) {
    bool changed = true;
    for (int iter = 0; changed && iter < 6; ++iter) {
      changed = false;
      changed |= eliminate_unreachable(f);
      changed |= constant_propagation(f);
      changed |= strength_reduction(f);
      changed |= dead_code_elimination(f);
    }
  }
}

// ---------- 频率寄存器分配 ----------
// 设计:每个物理寄存器至多分配给一个 vreg —— 无区间重叠判定,构造上保证正确。
// - 跨调用存活的 vreg(定义与最后使用之间存在 CALL)只能进被调用者保存寄存器 s1-s11;
// - 不跨调用的 vreg 可用 t3-t6 与 a0-a7;
// - t0/t1/t2 留给代码生成的暂存与地址计算;
// - 按"定义+使用"次数降序分配,排不上的溢出到栈槽(紧凑编号)。
RegAlloc allocate_registers(const IRFunction& fn) {
  RegAlloc ra;
  const int n = fn.max_vreg;
  ra.alloc.resize(n);

  std::vector<int> op_uses(n, 0);  // 仅操作数使用次数(直通判定的"单次使用")
  std::vector<int> uses(n, 0);     // 定义+使用(频率排序的热度)
  std::vector<int> first_def(n, INT_MAX), last_use(n, -1);
  bool leaf = true;
  std::vector<int> call_idx;
  for (size_t idx = 0; idx < fn.code.size(); ++idx) {
    const Instr& in = fn.code[idx];
    if (in.op == IROp::CALL) {
      leaf = false;
      call_idx.push_back(static_cast<int>(idx));
    }
    auto use = [&](int v) {
      if (v >= 0) {
        ++op_uses[v];
        ++uses[v];
        last_use[v] = static_cast<int>(idx);
      }
    };
    use(in.rs1);
    use(in.rs2);
    for (int a : in.args) use(a);
    if (in.rd >= 0 && in.op != IROp::LABEL) {
      first_def[in.rd] = std::min(first_def[in.rd], static_cast<int>(idx));
      if (in.op != IROp::ARG) ++uses[in.rd];  // ARG 不计使用频率,参数不进热榜
    }
  }
  ra.leaf = leaf;

  std::vector<bool> crosses(n, false);
  for (int v = 0; v < n; ++v) {
    if (last_use[v] < 0) continue;
    for (int c : call_idx)
      if (first_def[v] <= c && c <= last_use[v]) {
        crosses[v] = true;
        break;
      }
  }

  // 形参 vreg 禁止分配 a0-a7:序言中逐条拷贝 ARG 时,写入 a 寄存器
  // 会覆盖尚未拷贝的其余实参(经典陷阱,见 args.tc 回归)。
  std::vector<bool> is_param(n, false);
  for (const Instr& in : fn.code)
    if (in.op == IROp::ARG && in.rd >= 0 && in.rd < n) is_param[in.rd] = true;

  static const std::vector<std::string> s_pool = {
      "s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11"};

  // 调用实参直通:只被某次 call 使用一次、且定义紧邻该 call 的 vreg,
  // 直接分配到对应实参寄存器 a_i —— 定义指令直接写 a_i,调用时零搬运,
  // 且该 vreg 不再需要被调用者保存(相比 s 寄存器省 2 条存/取)。
  // 安全性:直通 vreg 的活跃区间是 [call-1, call],任何含该区间的区间都跨此 call,
  // 不会被分配 a 寄存器,故与其他分配天然无冲突;不同 call 的直通区间互不相交。
  std::vector<bool> assigned(n, false);
  for (size_t idx = 0; idx < fn.code.size(); ++idx) {
    const Instr& in = fn.code[idx];
    if (in.op != IROp::CALL) continue;
    for (size_t i = 0; i < in.args.size() && i < 8; ++i) {
      int v = in.args[i];
      if (v < 0 || v >= n || assigned[v]) continue;
      if (op_uses[v] != 1) continue;                        // 单次使用(仅操作数)
      if (first_def[v] != static_cast<int>(idx) - 1) continue;  // 定义紧邻 call
      ra.alloc[v].is_reg = true;
      ra.alloc[v].reg = "a" + std::to_string(i);
      assigned[v] = true;
    }
  }

  static const std::vector<std::string> t_pool = {
      "t3", "t4", "t5", "t6", "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7"};

  std::vector<int> order;
  for (int v = 0; v < n; ++v)
    if (!assigned[v]) order.push_back(v);
  std::sort(order.begin(), order.end(), [&](int a, int b) {
    if (uses[a] != uses[b]) return uses[a] > uses[b];
    return a < b;
  });

  size_t si = 0, ti = 0;
  int slot_id = 0;
  for (int v : order) {
    if (last_use[v] < 0) {  // 死 vreg:不占槽、不进寄存器
      ra.alloc[v].slot = -1;
      continue;
    }
    if (is_param[v]) {  // 形参:s 寄存器或溢出,绝不进 a 寄存器,也不进 t 池
      if (si < s_pool.size()) {
        ra.alloc[v].is_reg = true;
        ra.alloc[v].reg = s_pool[si++];
        ra.used_s.push_back(s_pool[si - 1]);
      } else {
        ra.alloc[v].slot = slot_id++;
        ++ra.spilled;
      }
    } else if (crosses[v] && si < s_pool.size()) {
      ra.alloc[v].is_reg = true;
      ra.alloc[v].reg = s_pool[si++];
      ra.used_s.push_back(s_pool[si - 1]);
    } else if (!crosses[v] && ti < t_pool.size()) {
      ra.alloc[v].is_reg = true;
      ra.alloc[v].reg = t_pool[ti++];
    } else {
      ra.alloc[v].slot = slot_id++;
      ++ra.spilled;
    }
  }
  return ra;
}

}  // namespace toyc
