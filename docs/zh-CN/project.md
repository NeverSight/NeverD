**语言**: [English](../../README.md) | [简体中文](project.md) | [繁體中文](../zh-TW/project.md) | [日本語](../ja/project.md) | [한국어](../ko/project.md) | [Français](../fr/project.md) | [Deutsch](../de/project.md) | [Español](../es/project.md) | [Italiano](../it/project.md) | [Русский](../ru/project.md) | [العربية](../ar/project.md)

<!-- i18n-source: 826aa66a4b07aee78f67638fb98159a69d7886c21769e56b6a0d6196a1437b63 -->

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

输入格式、host 契约与限制详见 [EVM 指南](evm.md)和 [Solana SBF 指南](sbf.md)。

实验性 CLI `neverd mobile app.apk -o recovered-app` 可从 APK（multidex）、DEX 和 smali 恢复 Java，并生成 `report.json`。Android 恢复仅使用 NeverD 内置的 C++20 引擎，运行时不需要 Python 或 Java。包含空格的路径需要加引号。支持的输入、报告与恢复限制见 [Android 指南](android.md)。

实验性 iOS 流程 `neverd mobile App.ipa -o recovered-ios` 从 IPA、`.app` 或 Mach-O 输出原生 C 和受支持的 Objective-C/Swift 源码，保留运行时布局、源码单元和逐方法省略原因；生成代码不通过桥接调用原始二进制。环境、覆盖口径和独立编译验证见 [iOS 指南](ios.md)。

实验性的[解释器源码恢复](interpreter-recovery.md)使用 `neverd decompile --devirtualize --func ENTRY`，通过共享 LowIR/MedIR 管线，将受支持的已链接 x64 ELF/PE 解释器特化为 HighC 或 LLVMC。控制提示用于区分解码器上下文，不固定运行时输入。未解析的控制流、不支持的语义和预算耗尽都会明确失败；该模式不证明二进制替换或异常等价性。

恢复预算可显式配置：`--vm-max-fields`、`--vm-max-refinements`、`--vm-max-queries` 的默认值仍为 16、16、4096。兼容的 v3 C API 与失败规则见恢复指南。

恢复还提供 `--vm-chain-transfers=N`（默认 0）和 `--vm-no-control-discovery`。串接在已证明唯一目标的控制转移之间保留符号关联；达到上限后回到普通 CFG 边界。机器状态恢复可通过 `--vm-entry-frame=begin:end` 声明未经运行时检查、不会回绕的入口 RSP 偏移范围。精确数值前提会写入生成的 C 和报告；它不授予内存访问权限，也不构成等价证明。

机器状态恢复支持用 `--vm-entry-alignment=A:R` 声明并检查入口 RSP 同余域。`A` 必须是正的二次幂，且 `R < A`。其他入口在客体访存或状态写入前返回状态 2。根地址高位仍自由，默认不假定对齐；此选项不提供原生等价认证。

`--vm-external-stores-disjoint-frame` 为机器状态恢复添加显式、未经运行时检查的前提：每次外部 STORE 的完整范围必须避开 `--vm-entry-frame`。这种写入只保留该区间内已有的事实，不约束 LOAD 或外部指针之间的别名；默认行为仍保守。原生证明 API 拒绝此域。

超过 SSA 构建限制的大型恢复函数可通过 `--llvm` 使用有界的标量可变存储契约。入口输入、循环携带值和较早读取的语义得到保留。不支持的隐式状态、向量寄存器参数、映像重定位、歧义存储和畸形控制流会明确失败；HighC 拒绝此回退路径。源码输出仍遵循现有机器状态契约，不新增等价证明证书。

独立的 C++ 循环证明 API 可在预算内推导嵌套循环不变量和字典序排名，再检查原生代码到 LowIR 的精化关系，详见[恢复指南](interpreter-recovery.md)；它不证明输出 C 的等价性。

独立的 C++ `checkBinaryLLVMRefinement` API 对精确 LLVM 产物组合全新的原生和 LLVM 检查；C 编译仍不在证明范围内。

PE 恢复还会认证首选基址下的 DIR64 字节并排除导入写入；固定映像契约不证明 ASLR 或初始化等价性。

在显式机器状态契约下，解释器恢复支持有界入口栈对齐分区及内部 `RET imm16` 栈清理。这些分区的自动原生到 LLVM 证明组合尚未完成。

有界 `REP MOVS/STOS` 恢复保留逐元素顺序和重叠行为；原始指令的证明覆盖仍待完成。

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

CPU 执行分离 ISA 准入、来宾内存、后端传输与来宾 OS 策略。`NEVERD_ENABLE_CPU_EMULATION` 启用 x64/ARM64 CPU 层；`NEVERD_ENABLE_DRIVER_EMULATION` 添加有界 x64 Windows WDM/KMDF 环境。`linux-elf64-v1` 配置运行受支持的 Linux ELF 进程。参见[CPU 执行](cpu-execution.md)、[来宾进程模拟](process-emulation.md)及[Windows 驱动模拟](driver-emulation.md)。

`windows-pe64-v1` 新增有界 Windows x64/ARM64 控制台进程：PE 装载、PEB/TEB、静态和动态 TLS、启动／退出回调及具名 Win32 API 模型。它独立使用 CPU 层，无需启用驱动模拟；DLL/CRT 装载、GUI、用户态 SEH、线程及通用 Windows 兼容性仍待完成。

Windows 虚拟内存新增 `VirtualAlloc`、`VirtualFree`、`VirtualProtect`、`VirtualQuery` 及当前进程的 `FlushInstructionCache`。OS 层管理预留区域，`AddressSpace` 统一管理已提交页面、权限和物理存储。测试覆盖动态代码改写、访问故障和内存额度回收。

`driver-strict` / `checked-x64-v1` 支持匹配的 Linux x64 主机上的 KVM 和 Windows x64 主机上的 WHP；`auto` 选择对应原生传输，跨 ISA 执行选择 Unicorn。显式 Unicorn 和原有 V1 API 保留可移植软件配置。原生执行在进入 CPU 前检查规范地址和指令效果；硬件不可用时明确失败且不回退。未支持的指令及 OS 行为仍明确报错。Windows x64 原生 CI 在关闭 Unicorn 的配置下通过全部 359 项必跑检查：131 项 CPU 检查、26 个内置映像与 46 个 WDK 映像及 40 个场景组合在首选和重定位地址产生的 224 项驱动结果，以及 4 项 SEH 边界检查 ([`9d4c130c`](https://github.com/NeverSight/NeverD/actions/runs/36981864458)). 原生 ARM64 的实机证据仍待补充，这不表示兼容任意驱动或 Android/Darwin 环境。

受检 x64 现支持普通 RAM 上的 `MOVS/STOS/LODS` 与 `CLD/STD`，并逐元素验证恢复、取消和跨页访问。依赖 CPU 型号的零次数高位行为与 STOS/LODS 设备操作数仍不在契约内。

受检 x64 还支持普通 RAM 上的 `CMPS/SCAS` 与 `REPE/REPNE`，涵盖算术标志、提前终止、逐元素停止和故障恢复；设备比较仍不支持。

`checked-aarch64-v1` 和 `checked-user-aarch64-v1` 提供有界 ARM64 FP32/FP64、定宽 SIMD，以及完整 FPCR/FPSR/向量状态。匹配的 Linux ARM64 主机使用 KVM，Windows ARM64 主机使用 WHP，跨 ISA 使用 Unicorn。ARM64 原生运行仍待实机验证；Windows 驱动加载仍限 x64。

x64 与 ARM64 原生启动自检在独占内存租约下验证有界的完整状态执行。XSAVE 数据包和包含 ISA 身份的页表缓存由唯一权威层管理；ARM64 原生工作负载证据仍未完整。

原生 x64 的 `FOP/FIP/FDP` 遵循宿主保存、恢复规则：AMD 可能清零未生效的 x87 异常元数据。启动自检通过未屏蔽的待处理异常验证这些字段。

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
  -DNEVERD_LLVM_PREBUILT_TAG=neverd-llvm-v23.0.0-r3
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

默认包修订版为 `neverd-llvm-v23.0.0-r3`。其 Git tag、发布目标、源码提交及三个归档摘要共同构成不可变的版本化源码固定值。已有构建目录若缓存旧基础 tag、`neverd-llvm-v23.0.0-r1` 或 `neverd-llvm-v23.0.0-r2`，会自动迁移到 `r3`，除非显式设置 `NEVERD_LLVM_PREBUILT_SHA256`。`Prebuilt LLVM Audit` 工作流在 push、pull request 及每六小时运行，调用 `scripts/audit_prebuilt_llvm_release.py`，对照 GitHub 当前发布和每个校验和附属文件核查源码固定值。

如果 LLVM fork 源码发生变化而 LLVM 仍报告 `23.0.0`，应发布下一个包修订版——`neverd-llvm-v23.0.0-r4`，之后是 `-r5`——不要覆盖已有发布，也不要虚构 LLVM 版本 `23.0.1`：

```bash
gh workflow run neverd-release.yml \
  --repo NeverSight/llvm-project \
  --ref main \
  -f release_tag=neverd-llvm-v23.0.0-r4 \
  -f overwrite_existing_assets=false
```

工作流成功后，同时更新 `cmake/NeverDLLVMPrebuilt.cmake` 中的默认 tag、固定提交和三个摘要。新包缓存在 `.cache/neverd-llvm/<tag>` 下；过期或被重新发布的归档会在解压前失败。`overwrite_existing_assets` 仅用于旧版恢复，正常的修订版发布保持关闭。

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

可选的 [Qt Quick 桌面工作台 (英文)](../gui.md)提供可停靠的指令、CFG、十六进制、C 和 IR 视图，支持全部 11 种界面语言、持久化标注及 MCP 连接。分析在不依赖 Qt 的独立工作进程中运行，纯 CLI 构建保持独立。支持的工作流及发布前仍需完成的平台验证见[验收记录 (英文)](../gui-qualification.md)。

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
neverd patch --from-c repl.c --func 0x401000 -o patched binary
neverd patch --mode inplace -o patched binary
neverd patch --subst --flatten --mba -o patched binary
```

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
