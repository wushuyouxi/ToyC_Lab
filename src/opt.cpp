#include "opt.h"

#include <algorithm>
#include <climits>
#include <map>
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
    // 拷贝传播:沿 MOV 别名链改写操作数(别名仅在定义点之后、源被重定义之前
    // 有效,重定义时 kill_aliases_to 已清除;到达标签清空,保守正确)。
    int r1 = resolve(in.rs1), r2 = resolve(in.rs2);
    if (r1 != in.rs1) { in.rs1 = r1; changed = true; }
    if (r2 != in.rs2) { in.rs2 = r2; changed = true; }
    for (auto& a : in.args) {
      int ra = resolve(a);
      if (ra != a) { a = ra; changed = true; }
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
        // x == 0 ⟺ !x:改写为 NOT(seqz 单指令,省 xor+seqz 两条)
        if (in.op == IROp::EQ && r != known.end() && r->second == 0) {
          in.op = IROp::NOT;
          in.rs2 = -1;
          changed = true;
          if (l != known.end()) {  // 立即折叠
            in.op = IROp::LOADIMM;
            in.imm = (l->second == 0) ? 1 : 0;
            in.rs1 = -1;
            known[in.rd] = in.imm;
          }
          break;
        }
        if (in.op == IROp::EQ && l != known.end() && l->second == 0) {
          in.op = IROp::NOT;
          in.rs1 = in.rs2;
          in.rs2 = -1;
          changed = true;
          if (r != known.end()) {
            in.op = IROp::LOADIMM;
            in.imm = (r->second == 0) ? 1 : 0;
            in.rs1 = -1;
            known[in.rd] = in.imm;
          }
          break;
        }
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

// ---------- 遍 4b:定义-使用合并 ----------
// 单定义、单使用且使用者是 MOV 的临时量,把定义直接写入 MOV 的目标:
//   r_tmp = i + 1;  i = r_tmp;   →   i = i + 1;
// 消除循环携带变量更新时的 addi+mv 两连(每迭代省一条指令)。
// 安全性要求:MOV 目标在定义与 MOV 之间没有被其他指令使用。
bool coalesce_def_use(IRFunction& f) {
  std::unordered_map<int, int> def_count;
  std::unordered_map<int, int> def_idx;
  for (size_t i = 0; i < f.code.size(); ++i) {
    if (f.code[i].rd >= 0 && f.code[i].op != IROp::LABEL) {
      ++def_count[f.code[i].rd];
      def_idx[f.code[i].rd] = static_cast<int>(i);
    }
  }
  std::unordered_map<int, int> use_count;
  for (const auto& in : f.code) {
    if (in.rs1 >= 0) ++use_count[in.rs1];
    if (in.rs2 >= 0) ++use_count[in.rs2];
    for (int a : in.args) ++use_count[a];
  }
  std::vector<char> drop(f.code.size(), 0);
  bool changed = false;
  for (size_t i = 0; i < f.code.size(); ++i) {
    const Instr& in = f.code[i];
    if (in.op != IROp::MOV || in.rs1 < 0) continue;
    int src = in.rs1, dst = in.rd;
    if (src == dst) continue;
    if (def_count[src] != 1 || use_count[src] != 1) continue;
    int d = def_idx[src];
    if (d >= static_cast<int>(i)) continue;
    bool used_between = false;
    for (size_t k = d + 1; k < i && !used_between; ++k) {
      const Instr& o = f.code[k];
      if (o.rs1 == dst || o.rs2 == dst) used_between = true;
      for (int a : o.args)
        if (a == dst) {
          used_between = true;
          break;
        }
    }
    if (used_between) continue;
    f.code[d].rd = dst;  // 定义直接写入 MOV 目标
    drop[i] = 1;         // 删除该 MOV
    changed = true;
  }
  if (!changed) return false;
  std::vector<Instr> out;
  for (size_t i = 0; i < f.code.size(); ++i)
    if (!drop[i]) out.push_back(std::move(f.code[i]));
  f.code = std::move(out);
  return true;
}

// ---------- 遍 5:函数内联(模块级) ----------
// 把叶子小函数展开进调用点:消除调用/序言/尾声开销;实参为常量时,
// 后续常量传播在展开体上生效,相当于函数特化。被调体 vreg 整体平移重定位,
// 标签加唯一后缀,形参用 MOV 绑定实参,RETURN 改写为对结果 vreg 的 MOV。
bool inline_calls(IRModule& m) {
  std::unordered_map<std::string, int> func_idx;
  for (size_t i = 0; i < m.functions.size(); ++i) func_idx[m.functions[i].name] = static_cast<int>(i);

  auto is_inlinable = [](const IRFunction& g, const std::string& caller) {
    if (g.name == "main" || g.name == caller) return false;
    for (const auto& in : g.code)
      if (in.op == IROp::CALL) return false;  // 只内联叶子函数
    return g.code.size() <= 30;
  };

  bool changed = false;
  int seq = 0;  // 进程内唯一标签后缀
  for (auto& f : m.functions) {
    std::vector<Instr> out;
    for (const Instr& in : f.code) {
      if (in.op != IROp::CALL) {
        out.push_back(in);
        continue;
      }
      auto it = func_idx.find(in.func);
      if (it == func_idx.end() || !is_inlinable(m.functions[it->second], f.name)) {
        out.push_back(in);
        continue;
      }
      const IRFunction& g = m.functions[it->second];
      std::string suffix = "_inl" + std::to_string(seq++);
      std::string end_label = ".Linl_end" + std::to_string(seq - 1) + "_" + f.name;
      int base = f.max_vreg;
      // 形参 vreg 映射(ARG imm → vreg)
      std::unordered_map<int, int> param_vreg;
      for (const auto& gi : g.code)
        if (gi.op == IROp::ARG) param_vreg[static_cast<int>(gi.imm)] = gi.rd;
      int result = f.max_vreg + g.max_vreg;
      f.max_vreg = result + 1;
      // 实参 → 形参 vreg
      for (size_t i = 0; i < in.args.size(); ++i) {
        Instr mv;
        mv.op = IROp::MOV;
        mv.rd = base + param_vreg[static_cast<int>(i)];
        mv.rs1 = in.args[i];
        mv.line = in.line;
        out.push_back(std::move(mv));
      }
      // 展开被调函数体。关键:RETURN 改写为「写结果 + 跳转到内联块结尾」——
      // 否则 return 路径会落进其后的循环体继续执行(prime 回归暴露)。
      for (size_t j = 0; j < g.code.size(); ++j) {
        const Instr& gi = g.code[j];
        if (gi.op == IROp::ARG) continue;
        if (gi.op == IROp::RETURN || gi.op == IROp::RETURNVOID) {
          if (gi.op == IROp::RETURN) {
            Instr mv;
            mv.op = IROp::MOV;
            mv.rd = result;
            mv.rs1 = base + gi.rs1;
            mv.line = in.line;
            out.push_back(std::move(mv));
          }
          Instr jmp;
          jmp.op = IROp::JUMP;
          jmp.label = end_label;
          out.push_back(std::move(jmp));
          // 其后到下一个 LABEL 的代码不可达(与 eliminate_unreachable 一致)
          while (j + 1 < g.code.size() && g.code[j + 1].op != IROp::LABEL) ++j;
          continue;
        }
        Instr c = gi;
        if (c.rd >= 0) c.rd += base;
        if (c.rs1 >= 0) c.rs1 += base;
        if (c.rs2 >= 0) c.rs2 += base;
        for (auto& a : c.args) a += base;
        if (c.op == IROp::LABEL) c.label += suffix;
        else if (!c.label.empty()) c.label += suffix;
        out.push_back(std::move(c));
      }
      Instr lend;
      lend.op = IROp::LABEL;
      lend.label = end_label;
      out.push_back(std::move(lend));
      // 调用结果 → 调用处 rd
      if (in.rd >= 0) {
        Instr mv;
        mv.op = IROp::MOV;
        mv.rd = in.rd;
        mv.rs1 = result;
        mv.line = in.line;
        out.push_back(std::move(mv));
      }
      changed = true;
    }
    f.code = std::move(out);
  }
  return changed;
}

// ---------- 遍 6:全局变量寄存器提升(仅叶子函数) ----------
// 叶子函数不调用任何函数 → 全局不可能被他人修改:
// 入口加载一次到缓存 vreg,函数内全部改走 MOV,每个出口写回。
// 循环内反复读写全局的基准题每迭代省 la+lw / la+sw 4~5 条指令。
// 惰性说明:入口加载放在 ARG 块之后 —— 对 main 而言位于运行时全局初始化代码之前,
// 其 LOADGLOBAL/STOREGLOBAL 也被提升,读取的正是 .data 初值,语义不变。
bool promote_globals(IRFunction& f) {
  for (const auto& in : f.code)
    if (in.op == IROp::CALL) return false;
  std::vector<std::string> order;
  for (const auto& in : f.code) {
    if (in.op == IROp::LOADGLOBAL || in.op == IROp::STOREGLOBAL) {
      if (std::find(order.begin(), order.end(), in.sym) == order.end()) order.push_back(in.sym);
    }
  }
  if (order.empty()) return false;
  std::unordered_map<std::string, int> cache;
  for (auto& g : order) cache[g] = f.max_vreg++;
  // ARG 块结束位置(入口加载插在此处)
  size_t insert_at = 0;
  for (size_t i = 0; i < f.code.size(); ++i) {
    if (f.code[i].op == IROp::ARG) insert_at = i + 1;
    else if (insert_at > 0) break;
  }
  std::vector<Instr> out;
  for (size_t i = 0; i < f.code.size(); ++i) {
    if (i == insert_at) {
      for (auto& g : order) {
        Instr ld;
        ld.op = IROp::LOADGLOBAL;
        ld.rd = cache[g];
        ld.sym = g;
        out.push_back(std::move(ld));
      }
    }
    const Instr& in = f.code[i];
    if (in.op == IROp::LOADGLOBAL && cache.count(in.sym)) {
      Instr mv = in;
      mv.op = IROp::MOV;
      mv.rs1 = cache[in.sym];
      mv.sym.clear();
      out.push_back(std::move(mv));
    } else if (in.op == IROp::STOREGLOBAL && cache.count(in.sym)) {
      Instr mv = in;
      mv.op = IROp::MOV;
      mv.rd = cache[in.sym];
      mv.sym.clear();
      out.push_back(std::move(mv));
    } else if (in.op == IROp::RETURN || in.op == IROp::RETURNVOID) {
      for (auto& g : order) {
        Instr st;
        st.op = IROp::STOREGLOBAL;
        st.rs1 = cache[g];
        st.sym = g;
        st.line = in.line;
        out.push_back(std::move(st));
      }
      out.push_back(in);
    } else {
      out.push_back(in);
    }
  }
  f.code = std::move(out);
  return true;
}

// ---------- 遍 7:循环不变量外提(仅 LOADIMM) ----------
// LOADIMM 是天然不变量:把循环体内的常量加载移到循环头之前。
// ToyC 无 goto,循环头只能由顺序流(首次进入)与回边进入,
// 外提后首次进入执行加载、后续迭代复用,语义不变。
bool hoist_loop_imm(IRFunction& f) {
  std::unordered_map<std::string, size_t> label_idx;
  for (size_t i = 0; i < f.code.size(); ++i)
    if (f.code[i].op == IROp::LABEL) label_idx[f.code[i].label] = i;
  std::vector<std::pair<size_t, size_t>> loops;  // 回边构成的循环区间 [header, end]
  for (size_t i = 0; i < f.code.size(); ++i) {
    const Instr& in = f.code[i];
    if (in.op == IROp::JUMP || in.op == IROp::BRZ || in.op == IROp::BRNZ) {
      auto it = label_idx.find(in.label);
      if (it != label_idx.end() && it->second < i) loops.push_back({it->second, i});
    }
  }
  if (loops.empty()) return false;
  // 定义次数:常量传播的 MOV 折叠会把「MOV v, 常量」改写成第二个 LOADIMM v
  // (v 在循环内另有重定义,如 j = j+1 的循环变量重置)—— 多定义的 vreg
  // 不是不变量,禁止外提(while_nest 回归暴露)。
  std::unordered_map<int, int> def_count;
  for (const auto& in : f.code)
    if (in.rd >= 0 && in.op != IROp::LABEL) ++def_count[in.rd];
  std::map<size_t, std::vector<Instr>> insert_before;  // 最外层循环头 → 外提指令
  std::unordered_set<size_t> to_remove;
  for (size_t i = 0; i < f.code.size(); ++i) {
    if (f.code[i].op != IROp::LOADIMM || f.code[i].rd < 0) continue;
    if (def_count[f.code[i].rd] > 1) continue;  // 多定义:非不变量
    size_t best = SIZE_MAX;
    for (auto& L : loops)
      if (L.first < i && i <= L.second) best = std::min(best, L.first);
    if (best == SIZE_MAX) continue;
    insert_before[best].push_back(f.code[i]);
    to_remove.insert(i);
  }
  if (to_remove.empty()) return false;
  std::vector<Instr> out;
  for (size_t i = 0; i < f.code.size(); ++i) {
    auto it = insert_before.find(i);
    if (it != insert_before.end())
      for (auto& in : it->second) out.push_back(in);
    if (to_remove.count(i)) continue;
    out.push_back(f.code[i]);
  }
  f.code = std::move(out);
  return true;
}

// ---------- 遍 7b:通用循环不变量外提(LICM) ----------
// 循环体内、操作数全部定义在循环头之前的纯计算(算术/比较/移位/MOV,
// 无 CALL、无全局访问)移到循环头之前——嵌套循环中只依赖外层变量的
// 计算在每个内层迭代被重复求值,外提后只算一次;逐轮迭代可逐层外提。
// 正确性:循环头只能由顺序流(首次进入)与回边进入(ToyC 无 goto),
// 外提指令首次进入时执行、后续迭代复用;操作数定义于循环头之前保证值不变。
bool hoist_loop_invariant(IRFunction& f) {
  auto is_candidate = [](IROp op) {
    switch (op) {
      case IROp::MOV: case IROp::LOADIMM:
      case IROp::ADD: case IROp::SUB: case IROp::MUL: case IROp::DIV: case IROp::REM:
      case IROp::ADDK: case IROp::SUBK:
      case IROp::NEG: case IROp::NOT:
      case IROp::SHL: case IROp::SRL: case IROp::SRA:
      case IROp::LT: case IROp::GT: case IROp::LE: case IROp::GE: case IROp::EQ: case IROp::NE:
        return true;
      default: return false;
    }
  };
  std::unordered_map<std::string, size_t> label_idx;
  for (size_t i = 0; i < f.code.size(); ++i)
    if (f.code[i].op == IROp::LABEL) label_idx[f.code[i].label] = i;
  std::vector<std::pair<size_t, size_t>> loops;  // [header, back_edge]
  for (size_t i = 0; i < f.code.size(); ++i) {
    const Instr& in = f.code[i];
    if (in.op == IROp::JUMP || in.op == IROp::BRZ || in.op == IROp::BRNZ) {
      auto it = label_idx.find(in.label);
      if (it != label_idx.end() && it->second < i) loops.push_back({it->second, i});
    }
  }
  if (loops.empty()) return false;
  std::vector<std::vector<size_t>> defs(f.max_vreg);
  for (size_t i = 0; i < f.code.size(); ++i)
    if (f.code[i].rd >= 0 && f.code[i].op != IROp::LABEL) defs[f.code[i].rd].push_back(i);

  std::map<size_t, std::vector<Instr>> insert_before;
  std::unordered_set<size_t> to_remove;
  for (auto& L : loops) {
    size_t h = L.first, e = L.second;
    for (size_t i = h + 1; i <= e; ++i) {
      if (to_remove.count(i)) continue;
      const Instr& in = f.code[i];
      if (!is_candidate(in.op)) continue;
      if (in.rd < 0 || defs[in.rd].size() != 1) continue;  // rd 必须单定义
      bool inv = true;
      for (int v : {in.rs1, in.rs2}) {
        if (v < 0) continue;
        for (size_t d : defs[v])
          if (d >= h) {  // 操作数在循环头之后有定义 → 非不变量
            inv = false;
            break;
          }
        if (!inv) break;
      }
      if (!inv) continue;
      insert_before[h].push_back(in);
      to_remove.insert(i);
    }
  }
  if (to_remove.empty()) return false;
  std::vector<Instr> out;
  for (size_t i = 0; i < f.code.size(); ++i) {
    auto it = insert_before.find(i);
    if (it != insert_before.end())
      for (auto& in : it->second) out.push_back(in);
    if (to_remove.count(i)) continue;
    out.push_back(f.code[i]);
  }
  f.code = std::move(out);
  return true;
}

// ---------- 优化入口:各遍迭代至不动点 ----------
void optimize(IRModule& m) {
  for (int round = 0; round < 8; ++round) {
    bool changed = false;
    changed |= inline_calls(m);
    for (auto& f : m.functions) {
      changed |= eliminate_unreachable(f);
      changed |= constant_propagation(f);
      changed |= strength_reduction(f);
      changed |= coalesce_def_use(f);
      changed |= dead_code_elimination(f);
      changed |= hoist_loop_imm(f);
      changed |= hoist_loop_invariant(f);
    }
    if (!changed) break;
  }
  // 收尾:全局提升(每函数至多一次,否则会把入口加载自身再"提升"导致错误)
  for (auto& f : m.functions) {
    promote_globals(f);
    dead_code_elimination(f);  // 清理提升产生的死代码
  }
}

// ---------- 线性扫描寄存器分配 ----------
// 按活跃区间 [first_def, last_use] 的起点排序逐一分配,已结束的区间释放寄存器,
// 寄存器耗尽时把"存活最久"的区间踢到溢出槽(被踢者此后一律走槽,代码生成
// 按最终分配表逐指令处理,天然正确)。
// 池规则:
//   - 跨调用存活(区间内部有 CALL)→ 仅 s1-s11(被调用者保存);
//   - 形参 → s1-s11 优先,其次 t3-t6(绝不可进 a0-a7:序言拷贝 ARG 时会互相覆盖);
//   - 其余 → t3-t6 与 a0-a7;区间恰在 call 处结束(实参)时偏好对应的 a_i。
// 相比频率分配器,短命临时量在热循环里复用寄存器,不再被低使用频率误溢出。
RegAlloc allocate_registers(const IRFunction& fn) {
  RegAlloc ra;
  const int n = fn.max_vreg;
  ra.alloc.resize(n);

  std::vector<int> first_def(n, INT_MAX), last_use(n, -1);
  std::vector<bool> is_param(n, false);
  bool leaf = true;
  std::vector<int> call_idx;
  std::unordered_map<int, int> arg_pos;  // vreg → 该 call 的实参位置(仅单次实参使用时)
  for (size_t idx = 0; idx < fn.code.size(); ++idx) {
    const Instr& in = fn.code[idx];
    if (in.op == IROp::CALL) {
      leaf = false;
      call_idx.push_back(static_cast<int>(idx));
      for (size_t i = 0; i < in.args.size() && i < 8; ++i)
        arg_pos[in.args[i]] = static_cast<int>(i);
    }
    auto use = [&](int v) {
      if (v >= 0) last_use[v] = static_cast<int>(idx);
    };
    use(in.rs1);
    use(in.rs2);
    for (int a : in.args) use(a);
    if (in.rd >= 0 && in.op != IROp::LABEL) {
      first_def[in.rd] = std::min(first_def[in.rd], static_cast<int>(idx));
      if (in.op == IROp::ARG) is_param[in.rd] = true;
    }
  }
  ra.leaf = leaf;

  // 循环区间(回边构成)——线性扫描的区间必须按"执行序"而非线性下标计算:
  // 循环内的使用每次迭代都会再次发生,循环内的定义每次迭代都会重写寄存器。
  // 保守修正:定义落在循环内 → 区间起点推到循环头;使用落在循环内 → 区间终点
  // 推到循环尾(回边处)。basic.tc 回归:循环界常量(循环外定义)在每次迭代被
  // 使用,若不扩展终点,其寄存器会被循环内的临时值抢占。
  std::vector<std::pair<int, int>> loops;  // [header, back_edge]
  for (size_t i = 0; i < fn.code.size(); ++i) {
    const Instr& in = fn.code[i];
    if (in.op == IROp::JUMP || in.op == IROp::BRZ || in.op == IROp::BRNZ) {
      for (size_t j = 0; j < i; ++j)
        if (fn.code[j].op == IROp::LABEL && fn.code[j].label == in.label)
          loops.push_back({static_cast<int>(j), static_cast<int>(i)});
    }
  }
  auto extend = [&](int p, bool is_def) -> int {
    int best = p;
    for (auto& L : loops)
      if (L.first <= p && p <= L.second)
        best = is_def ? std::min(best, L.first) : std::max(best, L.second);
    return best;
  };
  for (int v = 0; v < n; ++v) {
    if (first_def[v] != INT_MAX) first_def[v] = extend(first_def[v], /*is_def=*/true);
    if (last_use[v] >= 0) last_use[v] = extend(last_use[v], /*is_def=*/false);
  }

  // 跨调用判定(实参在 call 处被消费,不算跨调用)
  std::vector<bool> crosses(n, false);
  for (int v = 0; v < n; ++v) {
    if (last_use[v] < 0) continue;
    for (int c : call_idx)
      if (first_def[v] <= c && c < last_use[v]) {
        crosses[v] = true;
        break;
      }
  }

  // 区间按起点排序
  std::vector<int> order;
  for (int v = 0; v < n; ++v)
    if (last_use[v] >= 0) order.push_back(v);
  std::sort(order.begin(), order.end(), [&](int a, int b) {
    if (first_def[a] != first_def[b]) return first_def[a] < first_def[b];
    return last_use[a] < last_use[b];
  });

  static const std::vector<std::string> s_pool = {
      "s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11"};
  static const std::vector<std::string> t_pool = {
      "t3", "t4", "t5", "t6", "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7"};
  static const std::vector<std::string> param_t_pool = {"t3", "t4", "t5", "t6"};

  // 活动寄存器表:reg → (占用的 vreg, 其 end)。s 与 t 分表,但同一表内的
  // 寄存器互斥 —— t 表统一管理 t3-t6/a0-a7,形参只从 t3-t6 子集里挑,
  // 从根本上杜绝"同一物理寄存器分给两个 vreg"。
  struct Active {
    std::unordered_map<std::string, std::pair<int, int>> m;
    void expire(int cur_start) {
      for (auto it = m.begin(); it != m.end();) {
        if (it->second.second < cur_start) it = m.erase(it);  // 区间已结束,归还
        else ++it;
      }
    }
  };
  Active s_act, t_act;

  // 从活动表中取一个空闲寄存器;池满则踢出 end 最远者(受害者改走溢出槽,
  // 代码生成按最终分配表逐指令处理,定义/使用自动落到槽上)。
  auto take_reg = [&](Active& act, const std::vector<std::string>& pool, int v, int end,
                      int& slot_id) -> std::string {
    for (const auto& r : pool)
      if (!act.m.count(r)) {
        act.m[r] = {v, end};
        return r;
      }
    std::string victim_reg;
    int victim_end = -1, victim_vreg = -1;
    for (const auto& r : pool) {
      auto it = act.m.find(r);
      if (it != act.m.end() && it->second.second > victim_end) {
        victim_end = it->second.second;
        victim_reg = r;
        victim_vreg = it->second.first;
      }
    }
    if (victim_end > end) {
      act.m.erase(victim_reg);
      act.m[victim_reg] = {v, end};
      ra.alloc[victim_vreg].is_reg = false;
      ra.alloc[victim_vreg].slot = slot_id++;
      return victim_reg;
    }
    return "";  // 溢出当前 vreg
  };

  auto push_used_s = [&](const std::string& r) {
    if (std::find(ra.used_s.begin(), ra.used_s.end(), r) == ra.used_s.end())
      ra.used_s.push_back(r);
  };

  int slot_id = 0;
  for (int v : order) {
    if (last_use[v] < 0) {
      ra.alloc[v].slot = -1;
      continue;
    }
    int start = first_def[v], end = last_use[v];
    s_act.expire(start);
    t_act.expire(start);
    if (is_param[v]) {  // 形参:s 优先 → t3-t6;绝不进 a 寄存器
      std::string r = take_reg(s_act, s_pool, v, end, slot_id);
      if (!r.empty()) {
        ra.alloc[v] = {true, r, -1};
        push_used_s(r);
      } else {
        r = take_reg(t_act, param_t_pool, v, end, slot_id);
        if (!r.empty()) ra.alloc[v] = {true, r, -1};
        else ra.alloc[v] = {false, "", slot_id++};
      }
    } else if (crosses[v]) {  // 跨调用:s 寄存器
      std::string r = take_reg(s_act, s_pool, v, end, slot_id);
      if (!r.empty()) {
        ra.alloc[v] = {true, r, -1};
        push_used_s(r);
      } else {
        ra.alloc[v] = {false, "", slot_id++};
      }
    } else {  // 不跨调用:t 池;实参偏好其 a_i(定义直写,调用零搬运)
      std::string prefer;
      auto ap = arg_pos.find(v);
      if (ap != arg_pos.end()) prefer = "a" + std::to_string(ap->second);
      std::string r;
      if (!prefer.empty() && !t_act.m.count(prefer)) {
        t_act.m[prefer] = {v, end};
        r = prefer;
      } else {
        r = take_reg(t_act, t_pool, v, end, slot_id);
      }
      if (!r.empty()) ra.alloc[v] = {true, r, -1};
      else ra.alloc[v] = {false, "", slot_id++};
    }
    if (!ra.alloc[v].is_reg && ra.alloc[v].slot >= 0) ++ra.spilled;
  }
  return ra;
}

}  // namespace toyc
