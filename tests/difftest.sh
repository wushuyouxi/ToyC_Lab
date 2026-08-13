#!/usr/bin/env bash
# 差分测试(核心正确性防线):同一份 ToyC 源码,分别
#   1) toyc(O0)→ RARS    2) toyc(-opt)→ RARS    3) cl.exe /O2 原生编译运行
# 三个退出码必须一致(ToyC 是合法 C 子集,语义等价)。原生码掩码 & 255 与 RARS 截断一致。
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TOYC="$ROOT/build/Release/toyc.exe"
RARS="$ROOT/tools/rars.jar"
WORK="$ROOT/build/difftmp"
mkdir -p "$WORK"

pass=0
fail=0
for tc in "$ROOT"/tests/cases/*.tc "$ROOT"/tests/perf/*.tc; do
  name=$(basename "$tc" .tc)
  "$TOYC" < "$tc" > "$WORK/$name.s" 2>/dev/null
  java -jar "$RARS" nc "$WORK/$name.s" > /dev/null 2>&1
  c0=$?
  "$TOYC" -opt < "$tc" > "$WORK/${name}_opt.s" 2>/dev/null
  java -jar "$RARS" nc "$WORK/${name}_opt.s" > /dev/null 2>&1
  c1=$?
  # 参考实现:cl.exe 编译运行(Git Bash 直接调用 bat,自动处理路径引号)
  rm -f "$WORK/$name.exe"  # 防止编译失败时误用残留旧 exe
  "$ROOT/tests/compile_c.bat" "$tc" "$WORK/$name.exe" > /dev/null 2>&1
  "$WORK/$name.exe" > /dev/null 2>&1
  c2=$(($? & 255))
  if [ "$c0" = "$c1" ] && [ "$c1" = "$c2" ]; then
    pass=$((pass + 1))
    echo "PASS $name (exit $c0)"
  else
    fail=$((fail + 1))
    echo "FAIL $name: O0=$c0 opt=$c1 cl=$c2"
  fi
done
echo "通过 $pass,失败 $fail"
[ "$fail" = 0 ]
