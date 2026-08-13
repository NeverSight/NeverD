**语言**: [English](../../README.md) | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md) | [日本語](README.ja.md) | [한국어](README.ko.md) | [Français](README.fr.md) | [Deutsch](README.de.md) | [Español](README.es.md) | [Italiano](README.it.md) | [Русский](README.ru.md) | [العربية](README.ar.md)

<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../assets/neverd-logo-dark.svg">
  <img src="../assets/neverd-logo-light.svg" width="72" alt="NeverD">
</picture>

# NeverD

**AI 友好的二进制分析与反编译引擎 — 1:1 提升，基于 LLVM**

PE · ELF · Mach-O · EVM · Solana SBF &nbsp;|&nbsp; x86-64 · i386 · AArch64 · ARM32 · EVM256 · SBF &nbsp;|&nbsp; 纯 C SDK

[![AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-blue.svg)](../../LICENSE)
[![C++20](https://img.shields.io/badge/Standard-C%2B%2B20-brightgreen.svg)](#构建)
![Platform](https://img.shields.io/badge/Platform-macOS%20%7C%20Linux%20%7C%20Windows-informational.svg)
[![SDK](https://img.shields.io/badge/SDK-Pure%20C%20API-orange.svg)](#sdk-与插件)

[文档](../README.zh-CN.md) · [路线图](../roadmap/README.zh-CN.md) · [贡献](CONTRIBUTING.zh-CN.md)

</div>

---

> GitHub 仓库首页固定展示英文 `README.md`。请使用上方语言链接查看本地化版本。

## 概览

NeverD 是以 **1:1 指令级提升** 为核心的原生与智能合约分析/反编译引擎。它加载 **PE**、**ELF**、**Mach-O**、传统 **EVM** 字节码和 Solana **SBF ELF** 程序。原生目标由 [Capstone](https://www.capstone-engine.org/) 解码；EVM 与 SBF 使用各自感知版本的 decoder 和分阶段 IR。所有路径均采用手写语义。已支持指令在 **LLVM IR**、**C**、**面向 SBF 的 Rust**、**面向 EVM 的 Solidity 重建**，或原生目标的**重写后二进制**中保持可观察行为。

**默认开启 strict**：没有 lifter 的指令抛出 `UnliftedInstruction`，不会跳过、猜测或静默变成 `NOP`。

CLI、集成方与 AI 智能体通过 **纯 C API** 使用同一个引擎 **`libneverd`**，不直接链接 Capstone、LLVM 或内部 C++。

输入格式、host 契约与限制详见 [EVM 指南](../evm.zh-CN.md)和 [Solana SBF 指南](../sbf.zh-CN.md)。

## 为什么选 NeverD？

- **1:1 语义** — 手写 lifter；默认 strict 下未支持指令抛出异常
- **LLM 友好** — 结构化 C、LLVM IR 与 JSON 分析经纯 C API 暴露，错误行为确定
- **一条管线，多种出口** — `lift` → LLVM IR · `decompile` → C/Solidity/Rust · `patch` → 重写原生二进制
- **二进制重写** — PE / ELF / Mach-O，section 跳板或 inplace 覆盖
- **分析工具集** — CLI、调试信息、签名、插件，以及可选混淆通路

## 支持的目标

| | **x86-64** | **i386** | **AArch64** | **ARM32** |
|---|:---:|:---:|:---:|:---:|
| **PE**（Windows） | ✓ | ✓ | ✓ | ✓ |
| **ELF**（Linux / Android） | ✓ | ✓ | ✓ | ✓ |
| **Mach-O**（macOS / iOS） | ✓ | ✓ | ✓ | ✓ |

> 矩阵中的每个单元格都已实现，但集成测试深度不同。详见[架构覆盖矩阵](../architecture.zh-CN.md#support-and-test-depth)。Mach-O i386 使用 `thin` 可重定位对象，因为现代 macOS 无法链接历史 i386 可执行文件。

传统 EVM 字节码独立于原生 container：从 Frontier 到 Fusaka 的 150 个已分配
opcode 全部进入专用 Low/Med/High IR、已验证 LLVM `i256`、C23 `_BitInt(256)`
和 Solidity 输出。详见 [EVM 反编译](../evm.zh-CN.md)。

Solana SBF v0-v4 ELF 程序使用专用 strict loader、完整版本化 ISA metadata、
Low/Med/High IR、已验证 LLVM、可移植 C11 与安全 stable Rust。详见
[Solana SBF 反编译](../sbf.zh-CN.md)。

## 工作原理

```text
Binary (PE / ELF / Mach-O)
  → Loader + DebugInfo
  → Capstone decode
  → LowIR     架构无关 NdOp · CFG
  → MedIR     类型 · ABI · 调用 · 内存 · SSA
       │
       ├─ lift        MedIR → LLVM IR
       ├─ decompile   MedIR → HighIR → C
       │              MedIR → LLVM IR → opt → C   (-llvm)
       └─ patch       MedIR → LLVM IR → codegen → binary

EVM (raw / hex / compiler artifact)
  → runtime 正规化 + hardfork-aware decode
  → EVM LowIR → EVM stack-SSA MedIR → recovered EVM HighIR
       ├─ lift        → verified LLVM i256/i512
       └─ decompile   → C23 _BitInt(256) 或 Solidity reconstruction

Solana SBF ELF (v0-v4)
  → 感知版本的 legacy/strict loader + verifier
  → SBF LowIR → 规范化 MedIR → 恢复的 SBF HighIR
       ├─ lift        → 已验证 LLVM i64 runtime ABI
       └─ decompile   → 可移植 C11 或安全 stable Rust
```

| 阶段 | 作用 |
|------|------|
| **LowIR** | 约 77 种 `NdOp` + CFG |
| **MedIR** | 类型、调用约定、内存模型、SSA |
| **HighIR** | 结构化控制流（`if` / `while` / `for`） |
| **LLVM** | 优化、输出 C，或生成机器码 |

## 快速开始

```bash
git submodule update --init --recursive
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# 管线
./build/bin/neverd lift -o out.ll binary
./build/bin/neverd decompile -o out.c binary
./build/bin/neverd patch -hello -o patched binary

# EVM
./build/bin/neverd lift contract.evm -o contract.ll
./build/bin/neverd decompile --language=c contract.evm -o contract.c
./build/bin/neverd decompile --language=solidity contract.evm -o contract.sol

# Solana SBF
./build/bin/neverd info program.so
./build/bin/neverd lift program.so -o program.ll
./build/bin/neverd decompile --language=c program.so -o program.c
./build/bin/neverd decompile --language=rust program.so -o program.rs

# 分析
./build/bin/neverd funcs binary
./build/bin/neverd disasm --func 0x401000 binary
./build/bin/neverd sigs --auto binary
```

构建时签名库安装到 `build/bin/signatures/`。`sigs --auto` 按格式、架构、位宽选择匹配库集。

## 构建

**要求：** CMake ≥ 3.20 · Ninja · C++20 编译器 · Git submodule（LLVM fork + Capstone）

```bash
git submodule update --init --recursive
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

首次配置会本地编译 LLVM fork（常 30–60 分钟），之后为增量构建。预设见 `CMakePresets.json`：`release` / `relwithdebinfo` / `debug`。

<details>
<summary><strong>预编译 LLVM · 产物 · 测试 · CMake 选项</strong></summary>

<br>

**预编译 LLVM**

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DNEVERD_LLVM_PREBUILT=ON \
  -DNEVERD_LLVM_PREBUILT_TAG=neverd-llvm-v23.0.0
cmake --build build
```

NeverD 常规的 push 与 pull request CI 刻意从源码编译 LLVM submodule。手动运行 `CI` 工作流时勾选 `use_prebuilt_llvm` 可验证已发布的包；只有手动选择 `true` 才启用预编译 LLVM，不勾选则与自动 CI 走同一条源码编译路径。

发布包按运行 CMake 的主机选择：

| 主机 | 发布产物 |
|------|----------|
| macOS arm64 | `neverd-llvm-macos-arm64.tar.xz` |
| Linux x86_64 | `neverd-llvm-linux-x86_64.tar.xz` |
| Windows x64 | `neverd-llvm-windows-x64.zip` |

每个压缩包在解压到 `~/.cache/neverd-llvm/<tag>/<arch>/`（或 `NEVERD_LLVM_PREBUILT_CACHE_DIR` 指定的路径）之前，都会与 `cmake/NeverDLLVMPrebuilt.cmake` 中钉住的摘要核对；若 tag 不在这些 pin 的描述范围内，则与随包发布的 `.sha256` 核对。发布构建在 macOS 与 Linux 上使用 ccache，Windows clang-cl 使用 sccache 配合 GitHub Actions 缓存后端；编译器缓存只加速重建，从不作为发布产物上传。

发布 tag 标识 NeverD 包的版本，`BUILDINFO.txt` 记录确切的 LLVM fork commit。若 LLVM 仍报告 `23.0.0` 但 fork 源码已变，常规的不可变做法是发布包修订版，如 `neverd-llvm-v23.0.0-r1`（再 `-r2`），而不是 `23.0.1`——除非 LLVM 自身的 patch 版本变了。把 `NEVERD_LLVM_PREBUILT_TAG` 指向该新修订版即可。

要就地修复现有的可变 release `neverd-llvm-v23.0.0`，从 llvm-project 的 `main` 分支运行 `NeverD LLVM Release` 工作流并启用 `overwrite_existing_assets`：

```bash
gh workflow run neverd-release.yml \
  --repo NeverSight/llvm-project \
  --ref main \
  -f release_tag=neverd-llvm-v23.0.0 \
  -f overwrite_existing_assets=true
```

这会替换同名产物，但刻意不强制移动现有的 Git tag。请在同一次改动中刷新 `cmake/NeverDLLVMPrebuilt.cmake` 里钉住的摘要：标识某个 NeverD 修订所期望的构建的是这些摘要而非 tag，因此下次 configure 会替换掉过期的 `~/.cache/neverd-llvm/neverd-llvm-v23.0.0/`，而与任何 pin 都对不上的压缩包会让该次 configure 以校验和不匹配停下，不会拖到后来才表现为旧包里没有的某个头文件。改用新的 `-rN` tag 则可完全避免就地覆盖。除非勾选该选项，工作流会拒绝意外替换；若 GitHub 将 release 标记为不可变，则完全拒绝替换。

**产物**

| 路径 | 说明 |
|------|------|
| `build/bin/neverd` | 统一 CLI |
| `build/bin/neverd-bench` | 基准测试（JSON） |
| `build/bin/neverd-sigmaker` | 从静态库生成 `.pat` |
| `build/bin/libneverd.*` | 引擎共享库 |
| `build/bin/sdk/` | `NeverDCAPI.h`、`NeverDPlugin.h` |
| `build/bin/signatures/` | 内置签名库 |

**测试**

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --target check-neverd
```

| 目标 | 说明 |
|------|------|
| `check-neverd` | 全部测试 |
| `check-neverd-semantic` | 仅语义 roundtrip（Unicorn） |

聚焦目标、CTest 标签、fixture 要求与跨格式重写网格详见[测试 NeverD](../testing.zh-CN.md)。

**CMake 选项**

| 选项 | 默认 | 说明 |
|------|------|------|
| `NEVERD_LLVM_PREBUILT` | `OFF` | CI 预编译 LLVM |
| `NEVERD_BUILD_SHARED` | `ON` | 构建 `libneverd` |
| `NEVERD_BUILD_PLUGINS` | `OFF` | 示例插件 |
| `BUILD_TESTING` | `OFF` | 单元测试 |

</details>

## CLI

```text
neverd <command> [options] <binary>
```

### 管线命令

| 命令 | 输出 | 说明 |
|------|------|------|
| `lift` | `.ll` | 提升到 LLVM IR |
| `decompile` | `.c` / `.sol` / `.rs` | 通过 `--language` 选择 C、EVM Solidity 或 SBF Rust |
| `decompile -llvm` | `.c` | 经 LLVM IR + 优化器 |
| `patch` | 二进制 | 重写机器码 |

```bash
neverd patch -hello -o patched binary
neverd patch --from-ir repl.ll -o patched binary
neverd patch --from-c repl.c --func 0x401000 -o patched binary
neverd patch --mode inplace -o patched binary
neverd patch --subst --flatten --mba -o patched binary
```

<details>
<summary><strong>分析命令</strong></summary>

<br>

| 命令 | 功能 |
|------|------|
| `info` / `dashboard` / `headers` | 元数据与概览 |
| `funcs` | 发现的函数 |
| `disasm` | 反汇编（`--func` 名称或十六进制） |
| `hex` | 按地址十六进制转储 |
| `cfg` / `callgraph` | CFG / 调用图（JSON；可选 DOT/SVG） |
| `xrefs` | 交叉引用 |
| `strings` / `search` | 字符串 / 字节或文本搜索 |
| `imports` / `exports` / `symbols` / `relocs` | 表 |
| `segments` / `sections` / `entrypoints` | 布局 |
| `diff` | 对比两个二进制（`-a` / `-b`） |
| `sigs` | 签名（`--auto`） |
| `rename` / `annotate` / `bookmarks` | 会话标注 |
| `export` | 导出结果 |
| `plugins` | 列出或运行插件 |

大多数分析命令支持 `--json`。

</details>

## SDK 与插件

集成方使用 **`libneverd`** 的 **纯 C API**：

| 头文件 | 用途 |
|--------|------|
| `NeverDCAPI.h` | 会话、提升、反编译、patch、IR / CFG、标注 |
| `NeverDPlugin.h` | 动态库插件 ABI |

```c
neverd_session_t s = neverd_session_create();
neverd_session_load(s, "binary.exe");
neverd_session_analyze(s);

const char *c = neverd_decompile(s, 0x401000);
neverd_free_string(c);
neverd_session_destroy(s);
```

对 EVM 使用 `neverd_decompile_all_ex(..., NEVERD_OUTPUT_SOLIDITY, ...)` 明确
选择 Solidity；旧的 `neverd_decompile_all` 仍输出 C。参见
[EVM C API 示例](../evm.zh-CN.md#c-api)。

`-DNEVERD_BUILD_PLUGINS=ON` 构建示例插件。加载路径：`<neverd-dir>/plugins`、`~/.neverd/plugins`、`$NEVERD_PLUGIN_PATH`。

## 依赖

| 组件 | 作用 | 来源 |
|------|------|------|
| **LLVM**（fork） | IR、优化、代码生成、诊断 | `third_party/llvm-project` 或预编译 |
| **Capstone** | 解码 | `third_party/capstone` |

第三方保留各自许可证。

## 贡献

开发成果合入 **`dev`** 分支。环境搭建、Release/Debug 指引、风格、聚焦测试和拉取请求要求见[贡献指南](CONTRIBUTING.zh-CN.md)。[架构](../architecture.zh-CN.md)与[测试](../testing.zh-CN.md)指南将常见变更映射到对应代码与验证套件。

## 许可证

[AGPL-3.0](../../LICENSE)

LLVM 组件保留 Apache-2.0 WITH LLVM-exception 许可证。Capstone 保留其自身许可证。
