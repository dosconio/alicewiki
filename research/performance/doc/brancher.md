# BRANCHER 使用文档

## 1. 概述

### 1.1 项目功能

BRANCHER 是一个基于 Intel PT（Intel Processor Trace）技术的程序分支采样与分析工具，提供两大核心功能：

- **采样（Record）**：对程序执行过程中的每一个基本块分支跳转进行完整记录，输出格式为"起始地址→目标地址"。
- **过滤（Filter）**：结合可执行程序的符号表，从采样结果中提取程序执行路径的基本块序列。

### 1.2 程序原理

**采样原理**

本程序通过调用 Linux perf 工具，并利用 Intel PT 硬件追踪技术实现完整采样。Intel PT 是 Intel CPU 的硬件追踪技术，从第五代酷睿处理器（Broadwell）开始支持，能够以极低的性能开销记录 CPU 指令流信息。

Intel PT 相比 Intel LBR 等其他采样方式的特点：
- 完全采样，不会遗漏任何分支跳转
- 采样数据尺寸较大

**过滤原理**

过滤功能需要可执行文件包含基本块符号信息。这要求编译器（LLVM）支持导出基本块符号的功能。程序使用 `nm` 工具导出的符号表（perf.name）进行基本块序列分析。

### 1.3 运行环境

- 仅支持 Unix-like 系统（Linux）
- 采样功能需要 Intel PT 支持（部分 Intel 处理器）
- 过滤功能不依赖特定硬件

---

## 2. 采样

### 2.1 命令格式

```
sudo brancher record <filepath>
```

### 2.2 参数说明

| 参数 | 说明 |
|------|------|
| filepath | 要采样的执行命令 |

### 2.3 示例

```bash
sudo brancher record './a.exe 1'
```

对 `a.exe 1` 的执行过程进行采样。

### 2.4 输出文件

| 文件 | 说明 |
|------|------|
| perf.data | perf 原始数据文件 |
| perf.txt | 人类可读的分支跳转记录（地址对形式） |

> 注：perf.txt 尺寸通常比 perf.data 大百倍甚至千倍。

---

## 3. 过滤

### 3.1 命令格式

```
brancher filter -f <logfile> -l <symfile> (<funcs...>)
```

### 3.2 参数说明

| 参数 | 说明 |
|------|------|
| -f logfile | 采样结果文件（perf.txt），默认使用 perf.txt |
| -l symfile | 符号文件（nm 输出），默认使用 perf.name |
| funcs | 要过滤的函数名（可选），省略时列出所有用户函数 |

### 3.3 符号文件准备

使用 `nm` 命令生成符号文件：

```bash
nm a.out > perf.name
```

### 3.4 输出格式

输出为基本块序列，格式为 `函数名*基本块编号`，例如：

```
main*0
main*1
fff*0
fff*1
```

表示程序依次执行了 main 函数的第 0、1 个基本块，然后是 fff 函数的第 0、1 个基本块。

### 3.5 示例

**[1] 基本用法**

列出所有用户函数的基本块序列：

```bash
brancher filter
```

使用默认的 perf.txt 和 perf.name 文件。

**[2] 指定文件**

```bash
brancher filter -f perf2.txt -l perf2.name
```

使用指定名称的采样结果和符号文件。

**[3] 过滤特定函数**

```bash
brancher filter fff
```

过滤 fff 函数的基本块序列。注意：C++ 重载函数使用 mangled name（如 `_Z3fffi` 表示 `fff(int)`）。

**[4] 过滤多个函数**

```bash
brancher filter main fff ggg
```

过滤多个指定函数的基本块序列。

---

## 4. 源代码说明

### 4.1 文件结构

| 文件 | 说明 |
|------|------|
| brancher.hpp | 公共头文件，定义 Linksym 结构体和 Linksyms 类 |
| brancher.cpp | 主程序，包含 main() 和 brancher() 函数 |
| brancher-nm.cpp | 符号解析，实现 Linksyms::StructSymbols() |

### 4.2 核心函数

**Linksyms::StructSymbols(Dchain& dc, HostFile& symfile)**

- 功能：解析 nm 输出的符号文件，构建基本块符号链表
- 输入：
  - dc：存储待过滤用户函数的链表
  - symfile：可执行程序的符号文件
- 输出：无返回值，结果存储在 dc 中

**brancher()**

- 功能：根据符号表和采样结果，输出基本块执行序列到标准输出
- 输入：全局变量 dc（待过滤用户函数链表）、file_trace（采样结果文件）、linksym_lists（符号链表）
- 输出：无返回值，结果输出到 stdout

### 4.3 数据结构

**Linksym 结构体**

```cpp
struct Linksym {
    uint64 offs;    // 基本块地址偏移
    char* name;     // 基本块符号名（格式：函数名*基本块编号）
};
```

**Linksyms 类**

```cpp
struct Linksyms {
    static bool StructSymbols(uni::Dchain& dc, uni::HostFile& symfile);
    static stduint Index(uni::Dchain& dc, const char* fname, stduint bbnumber);
};
```

---

## 5. 版本历史

| 版本 | 日期 | 说明 |
|------|------|------|
| 2.1 | 2025-02-11 | 初始文档版本 |
| 2.2 | 2025-05-13 | 增加用户空间自动检测，重构以适应 C++ |
