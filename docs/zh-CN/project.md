**语言**: [English](../../README.md) | [简体中文](project.md) | [繁體中文](../zh-TW/project.md) | [日本語](../ja/project.md) | [한국어](../ko/project.md) | [Français](../fr/project.md) | [Deutsch](../de/project.md) | [Español](../es/project.md) | [Italiano](../it/project.md) | [Русский](../ru/project.md) | [العربية](../ar/project.md)

<!-- i18n-source: 9a100b1a0b2a5ec44fc5623ea91ed93b02f978ecbe29cc51a156ea53dcde63ff -->

<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../assets/neverd-logo-dark.svg">
  <img src="../assets/neverd-logo-light.svg" width="72" alt="NeverD">
</picture>

# NeverD

**AI 友好的二进制分析与反编译引擎 — 1:1 提升，基于 LLVM**

PE · ELF · Mach-O · EVM · Solana SBF &nbsp;|&nbsp; x86-64 · i386 · AArch64 · ARM32 · EVM256 · SBF &nbsp;|&nbsp; C + Python SDK

[![AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-blue.svg)](../../LICENSE)
[![C++20](https://img.shields.io/badge/Standard-C%2B%2B20-brightgreen.svg)](#构建)
![Platform](https://img.shields.io/badge/Platform-macOS%20%7C%20Linux%20%7C%20Windows-informational.svg)
[![SDK](https://img.shields.io/badge/SDK-C%20%2B%20Python-orange.svg)](#sdk-与插件)

[文档](README.md) · [Android](android.md) · [iOS](ios.md) · [路线图](roadmap.md) · [贡献](CONTRIBUTING.md)

</div>

---

> GitHub 仓库首页固定展示英文 `README.md`。请使用上方语言链接查看本地化版本。

<!-- i18n-section: overview -->

## 概览

NeverD 是以 **1:1 指令级提升** 为核心的原生与智能合约分析/反编译引擎。它加载 **PE**、**ELF**、**Mach-O**、传统 **EVM** 字节码和 Solana **SBF ELF** 程序。原生目标由 [Capstone](https://www.capstone-engine.org/) 解码；EVM 与 SBF 使用各自感知版本的 decoder 和分阶段 IR。所有路径均采用手写语义。已支持指令在 **LLVM IR**、**C**、**面向 SBF 的 Rust**、**面向 EVM 的 Solidity 重建**，或原生目标的**重写后二进制**中保持可观察行为。

**默认开启 strict**：没有 lifter 的指令抛出 `UnliftedInstruction`，不会跳过、猜测或静默变成 `NOP`。

CLI、集成方与 AI 智能体通过 **纯 C API** 使用同一个引擎 **`libneverd`**，不直接链接 Capstone、LLVM 或内部 C++。

使用方式、支持范围和限制见 [EVM](evm.md)、[Solana SBF](sbf.md)、[移动端源码恢复](mobile.md)及实验性[解释器源码恢复](interpreter-recovery.md)指南。

<!-- i18n-section: why-neverd -->

## 为什么选 NeverD？

- **1:1 语义** — 手写 lifter；默认 strict 下未支持指令抛出异常
- **LLM 友好** — 结构化 C、LLVM IR 与 JSON 分析经纯 C API 暴露，错误行为确定
- **一条管线，多种出口** — `lift` → LLVM IR · `decompile` → C/Solidity/Rust · `patch` → 重写原生二进制
- **二进制重写** — PE / ELF / Mach-O，section 跳板或 inplace 覆盖
- **分析工具集** — CLI、调试信息、签名、插件，以及可选混淆通路

<!-- i18n-section: supported-targets -->

## 支持的目标

| | **x86-64** | **i386** | **AArch64** | **ARM32** |
|---|:---:|:---:|:---:|:---:|
| **PE**（Windows） | ✓ | ✓ | ✓ | ✓ |
| **ELF**（Linux / Android） | ✓ | ✓ | ✓ | ✓ |
| **Mach-O**（macOS / iOS） | ✓ | ✓ | ✓ | ✓ |

> 矩阵中的每个单元格都已实现，但集成测试深度不同。详见[架构覆盖矩阵](architecture.md#support-and-test-depth)。Mach-O i386 使用 `thin` 可重定位对象，因为现代 macOS 无法链接历史 i386 可执行文件。

**MIPS ELF 分析为实验性支持：** MIPS32r2/o32 和 MIPS64r2/n64 均支持大小端，可对严格限定的整数指令子集进行反汇编、CFG 恢复以及 C 和 LLVM 输出。不支持原生代码重写。详见 [MIPS 覆盖范围与固件示例（英文）](../mips.md)，其中说明了单独验证的 microMIPS32、DSP 和 MIPS32r5 MSA 子集及其源码输出限制。

传统 EVM 字节码独立于原生 container：从 Frontier 到 Fusaka 的 150 个已分配
opcode 全部进入专用 Low/Med/High IR、已验证 LLVM `i256`、C23 `_BitInt(256)`
和 Solidity 输出。详见 [EVM 反编译](evm.md)。

Solana SBF v0-v4 ELF 程序使用专用 strict loader、完整版本化 ISA metadata、
Low/Med/High IR、已验证 LLVM、可移植 C11 与安全 stable Rust。详见
[Solana SBF 反编译](sbf.md)。

<!-- i18n-section: mobile-source-recovery -->

### 移动端源码恢复

实验性 **`neverd mobile` CLI 已支持 Android 和 iOS**：

| 平台 | 支持的输入 | 源码输出 |
|------|------------|----------|
| [Android](android.md) | APK（含多 DEX）、DEX、smali 文件或目录 | Java + JSON 报告 |
| [iOS](ios.md) | IPA、`.app`、Mach-O（arm64 / x86_64） | 原生 C 和受支持的 Objective-C / Swift 源码 + JSON 覆盖报告 |

恢复取决于受支持的代码模式；覆盖范围与限制见[移动端总览](mobile.md)及各平台指南。

<!-- i18n-section: cpu-workloads -->

### CPU 执行与来宾环境

NeverD 将 CPU 后端与来宾 OS 模型分开，用于有界进程、Android 原生库和 x64 Windows 驱动。Unicorn 提供 x64/ARM64 软件执行；同架构主机可用 KVM（Linux）、WHP（Windows）或 HVF（macOS）。支持环境、验证结果和限制见 [CPU 执行与来宾环境](emulation.md)。

<!-- i18n-section: how-it-works -->

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

<!-- i18n-section: quick-start -->

## 快速开始

```bash
git submodule update --init --recursive
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# 管线
./build/bin/neverd lift -o out.ll binary
./build/bin/neverd decompile -o out.c binary
./build/bin/neverd patch -hello -o patched binary

# 以程序自身的语言（C++、Rust、Go 或 C）显示伪代码
./build/bin/neverd decompile --language=source -o out.rs rust-binary
./build/bin/neverd decompile --language=cpp -o out.cpp cpp-binary
./build/bin/neverd decompile --language=go --func main.main go-binary

# 单个原生函数的可选变换（普通 HighC 会跳过这些步骤）
./build/bin/neverd decompile --func main --symbolic-simplify -o enhanced.c binary
./build/bin/neverd decompile --func main --llvm --llvm-opt=O3 -o optimized.c binary
./build/bin/neverd decompile --func main --llvm --llvm-opt=custom \
  --llvm-passes=instcombine,early-cse,simplifycfg,dce -o cleaned.c binary

# EVM
./build/bin/neverd lift contract.evm -o contract.ll
./build/bin/neverd decompile --language=c contract.evm -o contract.c
./build/bin/neverd decompile --language=solidity contract.evm -o contract.sol

# Solana SBF
./build/bin/neverd info program.so
./build/bin/neverd lift program.so -o program.ll
./build/bin/neverd decompile --language=c program.so -o program.c
./build/bin/neverd decompile --language=rust program.so -o program.rs

# 移动端源码恢复（实验性 CLI）
./build/bin/neverd mobile app.apk -o recovered-android
./build/bin/neverd mobile App.ipa -o recovered-ios

# 分析
./build/bin/neverd funcs binary
./build/bin/neverd disasm --func 0x401000 binary
./build/bin/neverd sym-explore --func 0x401000 --expressions binary
./build/bin/neverd audit binary
./build/bin/neverd hunt binary
./build/bin/neverd sigs --auto binary
```

构建时签名库安装到 `build/bin/signatures/`。`sigs --auto` 按格式、架构、位宽选择匹配库集。若 PE 文件的 Rich 头给出其链接器所属的 Visual Studio 版本，则只加载该版本的 `vs<year>.pat`，以及不属于任何版本的库。`--sig-base <dir>` 以同样方式从另一个签名目录树选择。 1 MiB 及以上的模式文件只解析一次：其模块保存在用户缓存目录下的 `neverd/signatures` 中，之后加载时直接映射。`NEVERD_SIGNATURE_CACHE` 可指定其他目录，设为 `off` 则关闭缓存。

<!-- i18n-section: building -->

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
  -DNEVERD_LLVM_PREBUILT_TAG=neverd-llvm-v23.0.0-r6
cmake --build build
```

NeverD 常规的 push 与 pull request CI 刻意从源码编译 LLVM submodule。手动运行 `CI` 工作流时勾选 `use_prebuilt_llvm` 可验证已发布的包；只有手动选择 `true` 才启用预编译 LLVM，不勾选则与自动 CI 走同一条源码编译路径。

发布包按运行 CMake 的主机选择：

| 主机 | 发布产物 |
|------|----------|
| macOS arm64 | `neverd-llvm-macos-arm64.tar.xz` |
| Linux x86_64 | `neverd-llvm-linux-x86_64.tar.xz` |
| Windows x64 | `neverd-llvm-windows-x64.zip` |

每个归档在解压到 `~/.cache/neverd-llvm/<tag>/<arch>/`（或 `NEVERD_LLVM_PREBUILT_CACHE_DIR` 指定的目录）前，都会核对 `cmake/NeverDLLVMPrebuilt.cmake` 中固定的摘要；未被这些固定值描述的 tag 则核对随包发布的 `.sha256`。默认固定版本的 `BUILDINFO.txt` 还必须记录完全一致的 LLVM 子模块提交。发布构建在 macOS 和 Linux 上使用 ccache，Windows clang-cl 使用 sccache 和 GitHub Actions 缓存后端；编译器缓存只加速重建，不作为发布产物上传。

默认包修订版为 `neverd-llvm-v23.0.0-r6`。其 Git tag、发布目标、源码提交及三个归档摘要共同构成不可变的版本化源码固定值。已有构建目录若缓存旧基础 tag、`neverd-llvm-v23.0.0-r1`、`neverd-llvm-v23.0.0-r2`、`neverd-llvm-v23.0.0-r3`、`neverd-llvm-v23.0.0-r4` 或 `neverd-llvm-v23.0.0-r5`，会自动迁移到 `r6`，除非显式设置 `NEVERD_LLVM_PREBUILT_SHA256`。`Prebuilt LLVM Audit` 工作流在 push、pull request 及每六小时运行，调用 `scripts/audit_prebuilt_llvm_release.py`，对照 GitHub 当前发布和每个校验和附属文件核查源码固定值。

如果 LLVM fork 源码发生变化而 LLVM 仍报告 `23.0.0`，应发布下一个包修订版——`neverd-llvm-v23.0.0-r7`，之后是 `-r8`——不要覆盖已有发布，也不要虚构 LLVM 版本 `23.0.1`：

```bash
gh workflow run neverd-release.yml \
  --repo NeverSight/llvm-project \
  --ref main \
  -f release_tag=neverd-llvm-v23.0.0-r7 \
  -f overwrite_existing_assets=false
```

工作流成功后，同时更新 `cmake/NeverDLLVMPrebuilt.cmake` 中的默认 tag、固定提交和三个摘要。每个包修订版使用独立的缓存目录；当前默认版本位于 `.cache/neverd-llvm/neverd-llvm-v23.0.0-r6` 下。过期或被重新发布的归档会在解压前失败。`overwrite_existing_assets` 仅用于旧版恢复，正常的修订版发布保持关闭。

**产物**

| 路径 | 说明 |
|------|------|
| `build/bin/neverd` | 统一 CLI |
| `build/bin/neverd-bench` | 基准测试（JSON） |
| `build/bin/neverd-sigmaker` | 从静态库生成 `.pat` |
| `build/bin/libneverd.*` | 引擎共享库 |
| `build/bin/sdk/` | C SDK 的 canonical include root；使用保留 `neverd/sdk/` 层级的 `<neverd/sdk/NeverDCAPI.h>` 或 `<neverd/sdk/NeverDPlugin.h>` |
| `build/bin/sdk/python/` | 带类型信息的 Python 插件包与示例 |
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

聚焦目标、CTest 标签、fixture 要求与跨格式重写网格详见[测试 NeverD](testing.md)。

**CMake 选项**

| 选项 | 默认 | 说明 |
|------|------|------|
| `NEVERD_LLVM_PREBUILT` | `OFF` | CI 预编译 LLVM |
| `NEVERD_BUILD_SHARED` | `ON` | 构建 `libneverd` |
| `NEVERD_ENABLE_PYTHON_PLUGINS` | `ON` | 嵌入 CPython 3.10+ 插件支持 |
| `NEVERD_BUILD_PLUGINS` | `OFF` | 示例插件 |
| `BUILD_TESTING` | `OFF` | 单元测试 |
| `NEVERD_ENABLE_SEMANTIC_TESTS` | `ON` | 依赖 Unicorn 的语义测试组（启用 `BUILD_TESTING=ON` 时） |

</details>

<!-- i18n-section: desktop-workbench -->

## 桌面工作台

可选的[桌面工作台 (英文)](../gui.md)沿用经典交互式反汇编器的布局和快捷键，提供反汇编、图形、伪代码、IR、十六进制和列表视图，采用 Visual Studio Code 风格主题，支持全部 11 种界面语言和 `.nddb` 项目数据库。分析在不依赖 Qt 的独立工作进程中运行，纯 CLI 构建保持独立。支持的工作流及发布前仍需完成的平台验证见[验收记录 (英文)](../gui-qualification.md)。

<!-- i18n-section: cli -->

## CLI

```text
neverd <command> [options] <binary>
```

<!-- i18n-section: pipeline -->

### 管线命令

| 命令 | 输出 | 说明 |
|------|------|------|
| `lift` | `.ll` | 提升到 LLVM IR |
| `decompile` | `.c` / `.sol` / `.rs` | 通过 `--language` 选择 C、EVM Solidity 或 SBF Rust |
| `decompile -llvm` | `.c` | 经 LLVM IR + 优化器 |
| `decompile --devirtualize` | `.c` + 可选 JSON | 实验性 x64 解释器恢复；需要 `--func`；[契约与示例](interpreter-recovery.md) |
| `mobile` | `.java` / `.c` / `.m` / `.swift` + JSON | 实验性: [Android](android.md), [iOS](ios.md) |
| `patch` | 二进制 | 重写机器码 |

```bash
neverd decompile program --func vm_entry --devirtualize --vm-control=r10 \
  --recovery-report recovery.json -o recovered.c
neverd patch -hello -o patched binary
neverd patch --from-ir repl.ll -o patched binary
neverd patch --from-source repl.c --entry replacement --func 0x401000 -o patched binary
neverd patch --mode inplace -o patched binary
neverd patch --subst --flatten --mba -o patched binary
```

源码补丁可选择 Clang、NeverC 普通模式或 GCC，参见[已验证的源码/object 配置](../source-patching.md)。native object 保留生成端的机器码；编译器与运行时兼容性、异常传播按配置分别验证。

对于缺少函数 ARM/Thumb 模式元数据的 32 位 ARM 二进制，在反编译前声明入口模式：

```bash
neverd decompile --arm-function-mode=0xADDRESS:thumb -o output.c binary
```

可重复使用该选项指定其他有歧义的入口，必要时使用 `:arm`。与已验证二进制元数据冲突的声明会使加载失败；有效声明只影响指定入口。C API 通过 `neverd_session_set_arm_function_mode()` 提供相同的加载前设置。

<details>
<summary><strong>分析命令</strong></summary>

<br>

| 命令 | 功能 |
|------|------|
| `info` / `dashboard` / `headers` | 元数据与概览 |
| `funcs` | 发现的函数 |
| `disasm` | 反汇编（`--func` 名称或十六进制） |
| `sym-explore` | 有界原生 LowIR 路径探索（`--func`；JSON 输出） |
| `audit` | 堆对象生命周期缺陷和未初始化局部栈读取（JSON） |
| `hunt` | 危险拷贝越界及符号见证；存在完整方案时附加 `process-input-v1` 重放证据（JSON schema v1） |
| `hex` | 按地址十六进制转储 |
| `cfg` / `callgraph` | CFG / 调用图（JSON；可选 DOT/SVG） |
| `xrefs` | 交叉引用 |
| `switches` | 全程序分析恢复的 switch 跳转表 |
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

<!-- i18n-section: sdk-and-plugins -->

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
[EVM C API 示例](evm.md#c-api)。

原生共享库与 Python `.py` 文件使用相同的插件生命周期。通过
`-DNEVERD_BUILD_PLUGINS=ON` 构建原生示例；纯 C 描述符、回调、构建/链接步骤、
发现顺序、CLI 工作流与 ABI 限制请参阅[原生插件指南](plugins.md)。Python
支持默认启用，可用 `-DNEVERD_ENABLE_PYTHON_PLUGINS=OFF` 完全移除；typed SDK 与
package 工作流见 [Python 插件指南](python-plugins.md)。两者都使用
`<neverd-dir>/plugins`、`~/.neverd/plugins` 和 `$NEVERD_PLUGIN_PATH`。

<!-- i18n-section: dependencies -->

## 依赖

| 组件 | 作用 | 来源 |
|------|------|------|
| **LLVM**（fork） | IR、优化、代码生成、诊断 | `third_party/llvm-project` 或预编译 |
| **Capstone** | 解码 | `third_party/capstone` |

第三方保留各自许可证。

<!-- i18n-section: contributing -->

## 贡献

开发成果合入 **`dev`** 分支。环境搭建、Release/Debug 指引、风格、聚焦测试和拉取请求要求见[贡献指南](CONTRIBUTING.md)。[架构](architecture.md)与[测试](testing.md)指南将常见变更映射到对应代码与验证套件。

<!-- i18n-section: license -->

## 许可证

[GNU AGPL 仅第 3 版](../../LICENSE)。再分发受其约束的 NeverD 代码或改编作品时，应保留版权、许可证和免责声明，包括 [NOTICE](../../NOTICE) 中的项目署名与来源。这也适用于借助 AI/LLM 复用代码和基于 LLVM 的代码转换。

要求、适用范围及示例见[署名与来源引用](ATTRIBUTION.md)。为便于追溯，建议引用源码文件及准确版本或提交。[CITATION.cff](../../CITATION.cff) 提供软件引用元数据；引用本身不能替代许可证合规要求。

LLVM 组件保留 Apache-2.0 WITH LLVM-exception 许可证。Capstone 保留其自身许可证。
