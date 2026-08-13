# ToyC 编译器(C++20 → RISC-V32)

ToyC 语言的编译器:手写词法分析、递归下降语法分析、语义分析、三地址码中间表示、RISC-V32 汇编代码生成。不依赖任何第三方库(仅 C++ 标准库)。

## 构建

```bash
cmake -B build
cmake --build build --config Release
```

生成可执行文件 `toyc`(Windows 下为 `build/Release/toyc.exe`)。

## 使用

```bash
toyc < input.tc > output.s        # 编译,输出 RISC-V32 汇编
toyc -opt < input.tc > output.s   # 开启优化
```

调试选项:`--dump-tokens`(词法)、`--dump-ast`(语法树)、`--dump-ir`(中间代码)。

## 运行环境

生成的汇编在 RARS(RISC-V 模拟器)上运行,程序的退出码为 main 函数的返回值。

## 目录结构

```
src/lexer.*    词法分析器
src/ast.*      抽象语法树
src/parser.*   递归下降语法分析器
src/sema.*     语义分析(符号表、类型检查、常量求值)
src/ir.*       三地址码中间表示
src/opt.*      优化遍(常量折叠/传播、DCE、CSE、寄存器分配、窥孔)
src/codegen.*  RISC-V32 汇编代码生成
tests/         测试用例与测试脚本
docs/          实践报告
```
