#!/usr/bin/env bash
# ToyC 回归测试:编译全部用例(O0 与 -opt)→ RARS 运行 → 与 "// expect: N" 注释对比。
# 前提:已构建 toyc(见 README);tools/rars.jar 已下载。
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TOYC="$ROOT/build/Release/toyc.exe"
RARS="$ROOT/tools/rars.jar"
WORK="$ROOT/build/testtmp"
mkdir -p "$WORK"

pass=0
fail=0
for mode in "" "-opt"; do
  if [ -z "$mode" ]; then tag="O0"; else tag="opt"; fi
  for tc in "$ROOT"/tests/cases/*.tc "$ROOT"/tests/perf/*.tc; do
    name=$(basename "$tc" .tc)
    exp=$(grep -oE "// expect: -?[0-9]+" "$tc" | grep -oE "\-?[0-9]+$" | head -1)
    if [ -z "$exp" ]; then
      echo "$name: 缺少 '// expect: N' 注释,跳过"
      continue
    fi
    "$TOYC" $mode < "$tc" > "$WORK/$name.s" 2>/dev/null
    java -jar "$RARS" nc "$WORK/$name.s" > /dev/null 2>&1
    got=$?
    if [ "$got" = "$exp" ]; then
      pass=$((pass + 1))
    else
      fail=$((fail + 1))
      echo "FAIL [$tag] $name: got $got, expected $exp"
    fi
  done
done
echo "通过 $pass,失败 $fail"
[ "$fail" = 0 ]
