**语言**: [English](testing.md) | [简体中文](testing.zh-CN.md) | [繁體中文](testing.zh-TW.md) | [日本語](testing.ja.md) | [한국어](testing.ko.md) | [Français](testing.fr.md) | [Deutsch](testing.de.md) | [Español](testing.es.md) | [Italiano](testing.it.md) | [Русский](testing.ru.md) | [العربية](testing.ar.md)

[← 文档索引](README.zh-CN.md)

# 测试 NeverD

NeverD 的测试回答三个不同问题：表示形状是否符合预期、完整 pipeline 路径能否
处理二进制 fixture，以及生成的代码是否保持行为。先选择能回答本次变更问题的
最小套件；对于高风险拉取请求，再运行更广的聚合测试。

## 配置测试构建

除非启用 `BUILD_TESTING`，否则测试不会构建。完整套件通常使用 Release；Debug
保留断言和单步能力，但有意不优化，不代表解码基准性能。

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build-release --parallel 4
```

完整 fixture 集要求 `clang` 能进行跨目标编译，并要求 LLVM linker（`ld.lld`
与 `lld-link`）位于 `PATH`。CMake 无条件构建许多可重定位 fixture，并在存在
对应 linker 时构建已链接 ELF/PE fixture。因主机无法编译或链接 fixture 而跳过
的测试属于未执行覆盖，不代表该目标通过。

克隆、构建配置与 macOS 预编译 LLVM 说明见
[CONTRIBUTING.md](i18n/CONTRIBUTING.zh-CN.md)。

## 测试布局

`add_neverd_unittest` 创建一个 GoogleTest 可执行文件，并为每个发现的用例分配
与该可执行目标同名的 CTest 标签。

| 源码区域 | 目标与 CTest 标签 | 覆盖内容 |
|----------|-------------------|----------|
| `unittests/TestProcessTests.cpp` | `NeverDTestProcessTests` | 跨平台子进程调用、引号、重定向与退出码 |
| `unittests/libc` | `NeverDLibCTests` | 已知 libc 名称与分类 |
| `unittests/lift` | `NeverDLiftTests` | Decoder/lifter LowIR 形状、IR 阶段、loader、重定位、格式 fixture、反编译与代表性 patch 流程 |
| `unittests/semantic` 中的大多数文件 | `NeverDSemanticTests` | 指令、ABI、控制流、C 表达式和 lift/recompile 差分语义 |
| `unittests/evm` | `NeverDEVMOpcodeTests`、`NeverDEVMBytecodeTests`、`NeverDEVMLoaderTests`、`NeverDEVMAnalyzerTests`、`NeverDEVMSemanticTests`、`NeverDEVMEmitterTests`、`NeverDEVMIntegrationTests` | 硬分叉元数据、输入规范化、CFG/SSA/恢复、解释器语义、LLVM/C/Solidity 差分执行及公共 API 路由 |
| `unittests/sbf` | `NeverDSBFMetadataTests`、`NeverDSBFLoaderTests`、`NeverDSBFAnalyzerTests`、`NeverDSBFSemanticTests`、`NeverDSBFLLVMEmitterTests`、`NeverDSBFEmitterTests`、`NeverDSBFIntegrationTests` | v0-v4 元数据与 ELF 布局、严格验证、CFG/恢复、独立原始执行、LLVM 验证、C/Rust 编译及公共 API 路由 |
| `PatchFullSubstRTTests.cpp` | `NeverDPatchFullTests` | 四 ISA×三对象格式的重写/混淆等价性 |
| `unittests/semantic` 中的聚焦变换文件 | `NeverDSwitchXformTests`、`NeverDIndCallXformTests`、`NeverDCFGLoopXformTests`、`NeverDTwoTableXformTests`、`NeverDAvxUpperXformTests` | 从大型语义二进制拆出的快速重链接探针 |
| `unittests/corpus`（子模块） | `NeverDWindowsEHCorpusTests`、`NeverDRustEHCorpusTests`、`NeverDGoEHCorpusTests`、`NeverDCxxItaniumEHCorpusTests` | 从 305 个钉住的真实二进制中读出的异常与运行时元数据，每个都在清单里声明了其恢复必须达到的下限 |

注册的事实来源是
[`unittests/CMakeLists.txt`](../unittests/CMakeLists.txt)、
[`unittests/lift/CMakeLists.txt`](../unittests/lift/CMakeLists.txt) 和
[`unittests/semantic/CMakeLists.txt`](../unittests/semantic/CMakeLists.txt)、
[`unittests/evm/CMakeLists.txt`](../unittests/evm/CMakeLists.txt) 和
[`unittests/sbf/CMakeLists.txt`](../unittests/sbf/CMakeLists.txt)。

### 钉住的二进制 corpus

其它每个测试套件都自己构建被测对象，corpus 不是：它是一个子模块，装的是真实工具链
在本仓库够不到的宿主机上、为够不到的目标产出的二进制，每一个都按摘要钉住，旁边的
清单声明了它的恢复必须达到的下限。要回答"NeverD 从一个 `-O2` stripped 的 `armv7`
共享库里到底读出了什么"这类问题，只有这里给得出答案而不是论断。

这些套件只在 configure 被告知去找它们时才构建，所以这个开关就是它们是否受测的全部：

```bash
cmake -S . -B build-corpus -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_BINARY_CORPUS_TESTS=ON
cmake --build build-corpus --target check-neverd-corpus --parallel 4
```

`check-neverd-corpus` 跑全部产线；`check-neverd-windows-eh-corpus`、
`check-neverd-rust-eh-corpus`、`check-neverd-go-eh-corpus` 与
`check-neverd-cxx-itanium-eh-corpus` 各跑一条。三个 CI 宿主都带着这个开关配置并跑
全部四条产线：字节到处都一样，但读字节的东西不一样，在一台宿主上跑通不能说明另外
两台。`scripts/audit_ci_test_inventory.py` 会拒绝缺少四个标签中任何一个的清单——
构建悄悄不再读 corpus 是一种没有任何测试能捕获的回归，因为消失的正是那个测试。

EVM 操作码审计每次运行都会对官方
[go-ethereum 仓库](https://github.com/ethereum/go-ethereum)的远端 `HEAD` 执行浅层
`git fetch`，并报告实际审计的精确提交。它复用已忽略的 bare cache
`build/evm-opcode-audit/go-ethereum.git`，但在读取封闭的操作码清单与字节分配前
一定会刷新缓存：

```bash
python3 scripts/audit_evm_opcode_metadata.py
```

CI 会在每次 push、pull request、手动触发以及每日定时任务中运行同一项在线审计，
因此即使 NeverD 没有变化，也能发现上游漂移。离线测试或复现历史版本时，才显式指定
已有 checkout：

```bash
python3 scripts/audit_evm_opcode_metadata.py \
  --geth-root /path/to/go-ethereum
```

该审计只允许 `EVMUpstreamOpcodePolicy.def` 中明确列出的排除项；任何未表示或未明确
评审的上游操作码都会令命令失败。其 parser 与漂移诊断在 CI 中有独立 Python 单元测试：

```bash
python3 -m unittest -v scripts.tests.test_audit_evm_opcode_metadata
```

修改 EVM 控制流时，先运行不动点与高度域契约：

```bash
cmake --build build --target NeverDEVMAnalyzerTests --parallel 4
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.StackHeightDomain*:EVMAnalyzer.WholeProgram*'
```

这些用例覆盖跨基本块 internal return、有限多目标合并、循环收敛与确定性边排序、
路径相关栈高度、有界 widening、相关性丢失导致的 Cartesian 过近似、未知跳转、精确
非法目标，以及 strict/relaxed 栈 fault。随后应运行全部七个 EVM 二进制和上游元数据
审计；即使 analyzer 的局部形状正确，CFG 修改也可能影响 emitter 与集成行为。

修改 MedIR/HighIR 数据流时，还要运行 constant-phi、selector、类型化操作数、
格式错误图和深链契约：

```bash
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.MediumIR*:EVMAnalyzer.HighIR*:EVMAnalyzer.*Selector*:EVMAnalyzer.*MedIR*:EVMAnalyzer.RecoversStorageAndEventFactsFromTypedOperands:EVMAnalyzer.RecoversComputedCalldataArgumentOffset:EVMAnalyzer.*Return*:EVMAnalyzer.*Receive*'
```

这些用例验证相等与冲突的循环 phi、非相邻和跨基本块 selector 表达式、等式两种操作数
顺序、精确 ABI 位宽检查、类型化 storage/event/calldata 操作数、格式错误 MedIR 的
确定性处理，以及对 16,384 个值进行的迭代式 producer walk。

## fixture 如何生成

### Lift 与格式 fixture

`unittests/lift/CMakeLists.txt` 在构建期间跨目标编译 C 与汇编源码。Clang target
triple 生成 x86-64、i386、AArch64、ARM32 ELF 对象，PE/COFF 对象和已链接
镜像，以及 PIC/no-PIC Mach-O i386 对象。存在 LLD 时，选定对象还会链接为
patch 测试所需的可执行文件。`NeverDLiftTests` 依赖 `lift-test-objects` 目标，
因此正常构建该测试二进制会刷新生成的 fixture。

多数 lift 测试使用 `NeverDLiftFixture.h` 调用构建出的 `neverd` CLI，并检查
LowIR、MedIR、HighIR、LLVM IR、生成的 C 或重写后的二进制。聚焦手动实验可用
`NEVERD` 环境变量覆盖 CLI 路径；普通 CTest 运行使用 CMake 嵌入的可执行文件。

### Windows 异常重建

修改 Windows 表驱动异常时，既要测试表示层，也要对已链接 PE 运行 patch 测试。
下面的聚焦 lift-suite 过滤器覆盖规范化 unwind/SEH/C++ 模型、损坏输入处理、
异常 CFG 边、HighIR、LLVM WinEH 生成、异常目录替换，以及 Guard CF/EH
continuation 重建：

```bash
cmake --build build --target NeverDLiftTests --parallel 4
build/bin/NeverDLiftTests \
  --gtest_filter='COFFException*:*PatchCOFF_X64.ReconstructsGuardedSEHAndContinuationTable:*PatchCOFF_X64.ReconstructsNativeFH3StateGraph:*PatchCOFF_X64.RejectsInteriorExceptionDirectoryPadding:*PatchCOFF_X64.RebuildsSortedExceptionDirectoryInAppendedSection'
```

受保护的 x64 汇编 fixture 需要 Clang Windows target 和 `lld-link`；其 CMake
链接使用 `/guard:cf` 与 `/guard:ehcont`。因缺少交叉链接器而跳过，不能作为
final-image 路径的有效证据。集成用例通过后，才证明重写后的 PE 可以重新加载，
且 runtime-function、unwind、load-config、Guard CF 与 Guard EH continuation
表保持有序、由文件承载，并只指向可执行目标。

已链接的 FH3 fixture 独立覆盖原生 C++ 闭包：固定状态表、HighC 注释、
personality 保留、生成的 catch 目标，以及重新加载后的 IP-to-state 图。

分析/原生支持矩阵和 fail-closed patch 契约见
[Windows 异常重建](windows-exception-reconstruction.zh-CN.md)。

### 语言异常模型

除 Windows 表模型以外的一切都集中在一个聚焦 target 中。
`NeverDLanguageEHTests` 覆盖 DWARF 帧链、Itanium 语言特定数据区、ARM EHABI、
Darwin compact unwind、Go 运行时帧元数据、Rust panic 机制，以及三种
Objective-C 运行时：

```bash
cmake --build build --target NeverDLanguageEHTests --parallel 4
build/bin/NeverDLanguageEHTests --gtest_filter='ObjC*'
```

本套件中的表是逐字节手工装配而非编译出来的，因为其中大多数要验证的组合
没有任何单一工具链会同时产出。Objective-C 是最典型的例子：三种运行时都发出
Itanium LSDA，差别只在类型表槽位里放什么——而这个差别是彻底的，不是程度问题。
Apple 的槽位指向 `objc_typeinfo`，其前两个字段刻意模仿 `std::type_info`；
GNUstep 的 Objective-C++ 槽位指向真正的 `std::type_info` 子类；GNU 运行时的
槽位根本不是指针，而是类名字符串本身。把一种运行时的约定套到另一种的表上
不会报错，只会报出一个从别的东西中间读出来的类名——所以在读任何槽位之前，
先由帧的 personality 确定运行时。

同一套件还钉住两个容易混为一谈、但混淆即错误的区分。`@catch(id)` 与
`@catch(...)` 是不同的处理器——前者接收任意 Objective-C 对象，并放外来异常
从旁边继续传播——而每种运行时对二者的拼写都不同，所以把两者都报成 catch-all
的解码器，等于给那些本会飞过去的异常安上了处理器。另外，setjmp/longjmp 的
call-site 表索引的是调用点序号而不是地址，因此没能认出某个 SJLJ personality
的读取器不会报错，而是会凭空造出程序从未指定过的保护区间和 landing pad。

认出这种形式，和拒绝解码它，是两回事。一条 SJLJ 条目是一对 ULEB128 值——
一个派发选择子和一个动作偏移——而这个动作偏移在此处的含义与地址形式中完全
一致，所以动作链、catch 类型、异常规格，全都能从一张根本不指名任何代码的表
里读出来。唯一读不出的是每条条目守护的区间，因为说明它的是函数自己对
call-site 槽位的写入，而不是表里的任何东西。该套件还钉住了此处唯一不可信的
那个字节：GCC 把 call-site 编码写成 `DW_EH_PE_uleb128`，LLVM 写成
`DW_EH_PE_udata4`，两者随后都照样发射 ULEB128，而没有任何 personality 会去
读它——所以解码器也不许读。

personality 身份同样在这里钉住，因为它决定了上面每张表该怎么读。GNAT 用
GCC 给每个前端的那三种拼法命名自己的例程——`_v0`、`_sj0`、`_seh0`——并且在
Windows 上注册一个符号却转发到另一个，所以这四种拼法都必须落到 Ada 上。D
则是镜像的情形：三个编译器，同一个例程的三个名字，背后是同一套表。

### Unicorn 差分往返

语义 fixture 测试行为而不是文本形状：

1. 编写一个小型 C/汇编用例，或构造 LLVM IR。
2. 用 Clang/LLVM 为请求的目标编译它。
3. 在 Unicorn 中执行原始机器码，并捕获期望返回值或 fixture 定义的其他状态。
4. 通过 NeverD 加载并提升，发射 LLVM IR，再将结果编译回机器码。
5. 使用相同 ABI、输入、内存布局和 CPU 模型执行再生成的代码。
6. 比较可观察结果。

主要实现位于
[`SemanticRoundTripFixture.h`](../unittests/semantic/SemanticRoundTripFixture.h)。
patch-full fixture 使用 `Codegen::compileForRewrite`（与 patch 操作相同的重写
backend），随后在完整 4×3 ISA/格式网格中比较基线与变换后代码。

确定性的 NeverD 语义失败应当成为失败测试。跳过只用于明确的外部能力边界，并应
阅读 skip 原因：缺少跨目标 linker 的绿色摘要不能证明该格式路径实际运行。

### EVM 差分后端

EVM 解释器测试提供确定性的 256 位 oracle。emitter suite 直接编译执行生成 LLVM，
通过 Clang lower 生成 C23 并在同一 host harness 中执行；安装 `solc`、`anvil`、
`cast` 与 `jq` 后，还会将生成 Solidity harness 部署至本地 Anvil。测试比较 status、
storage 与 instruction trace count。独立 raw-bytecode corpus 直接在 Anvil 原生 EVM
中执行 pre-Fusaka 标量 ALU、calldata/memory 复制、重叠 `MCOPY`、Keccak 与 return data。

`NeverDEVMOpcodeTests` 还约束 metadata 架构：全部 150 个 opcode 在 byte encoding 与
typed value 之间往返，测试 family helper 边界与 hardfork alias，完整 stack contract
和 host argument maximum 保持推导而不在 backend 中重复。

### Solana SBF 差分后端

SBF 元数据测试会验证每个版本特性、操作码冲突边界、Murmur3 syscall hash、重定位、ELF machine、寄存器和 VM 地址常量。Loader fixture 不依赖 vendored 二进制，直接生成旧式 v0-v2 section 布局和无 section 的严格 v3/v4 program-header 布局。

`NeverDSBFSemanticTests` 直接执行已验证的指令字节而不消费 MedIR，因此修改或破坏规范化 IR 不会让源 oracle 与后端意外达成一致。覆盖范围包括非单调的 v2 语义、内存、syscall、内部调用帧、fault、trace 和资源限制。LLVM module 会被验证；生成的 C 以 warnings-as-errors 编译，Rust 使用 `-D warnings`。公共 API 测试从生成的严格 SBF ELF 出发，遍历所有 IR 阶段、反汇编、CFG、元数据、LLVM、C 与 Rust。

## 一次性目标

自定义目标会构建其依赖，然后以主机 CPU 推导的并行度运行 CTest：

| CMake 目标 | 选择范围 |
|------------|----------|
| `check-neverd` | 所有已注册测试 |
| `check-neverd-semantic` | 仅 `NeverDSemanticTests` |
| `check-neverd-sbf` | 所有 `NeverDSBF*Tests` 目标/用例 |
| `check-neverd-patch-full` | 仅 `NeverDPatchFullTests` |
| `check-neverd-switch-xform` | 仅 `NeverDSwitchXformTests` |
| `check-neverd-cfgloop-xform` | 仅 `NeverDCFGLoopXformTests` |
| `check-neverd-twotable-xform` | 仅 `NeverDTwoTableXformTests` |

```bash
cmake --build build-release --target check-neverd
cmake --build build-release --target check-neverd-semantic
cmake --build build-release --target check-neverd-sbf
```

`NeverDIndCallXformTests` 与 `NeverDAvxUpperXformTests` 当前没有
`check-neverd-*` 便捷目标；请按下文先构建，再用标签选择。
`check-neverd-semantic` 也不包含单独的变换或 patch-full 二进制；完整聚合应使用
`check-neverd`。

## 增量 CTest 工作流

先构建所属可执行文件，再选择其标签。这样可以避免重链接无关的大型语义目标。

```bash
# Lifter, loader, and format tests
cmake --build build-release --target NeverDLiftTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDLiftTests$' --output-on-failure --parallel 4

# Main semantic binary
cmake --build build-release --target NeverDSemanticTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDSemanticTests$' --output-on-failure --parallel 4

# A label-only focused transform binary
cmake --build build-release --target NeverDIndCallXformTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDIndCallXformTests$' --output-on-failure --parallel 4

# 所有聚焦的 EVM 目标/用例
cmake --build build-release --target \
  NeverDEVMOpcodeTests NeverDEVMBytecodeTests NeverDEVMLoaderTests \
  NeverDEVMAnalyzerTests NeverDEVMSemanticTests NeverDEVMEmitterTests \
  NeverDEVMIntegrationTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -R 'EVM' --output-on-failure --parallel 4

# 所有聚焦的 Solana SBF 目标/用例
cmake --build build-release --target \
  NeverDSBFMetadataTests NeverDSBFLoaderTests NeverDSBFAnalyzerTests \
  NeverDSBFSemanticTests NeverDSBFLLVMEmitterTests NeverDSBFEmitterTests \
  NeverDSBFIntegrationTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -R 'SBF' --output-on-failure --parallel 4
```

用 GoogleTest 派生的 CTest 名称运行单个回归：

```bash
ctest --test-dir build-release --build-config Release -N \
  -L '^NeverDLiftTests$'
ctest --test-dir build-release --build-config Release \
  -R '^COFFARMPipeline\.ARM32ThumbLiftAndDecompile$' \
  --output-on-failure
```

常用选择器：

| 命令 | 用途 |
|------|------|
| `ctest --test-dir build-release -N` | 列出已发现用例而不运行 |
| `ctest --test-dir build-release -L '<regex>'` | 选择测试二进制标签 |
| `ctest --test-dir build-release -R '<regex>'` | 选择用例名称 |
| `ctest --test-dir build-release --output-on-failure` | 仅为失败显示诊断 |
| `ctest --test-dir build-release --stop-on-failure` | 第一个失败后停止 |
| `ctest --test-dir build-release --parallel 4` | 最多并行运行四个用例 |

GoogleTest 发现使用 `DISCOVERY_MODE PRE_TEST`，因此 CTest 枚举前必须存在对应测试
二进制。每用例 timeout 与独立的发现 timeout 定义于 `cmake/AddNeverD.cmake`，
只有存在测得的重型用例时才应放宽。

## 哪些测试应随代码变化？

| 变更区域 | 从这里开始 | 随后考虑 |
|----------|------------|----------|
| 架构 lifter 或 decode | `NeverDLiftTests` 中的命名用例 | 对应 ISA 语义往返 |
| LowIR CFG、函数检测、跳转表 | Lift CFG/switch 用例 | `NeverDSwitchXformTests`、`NeverDCFGLoopXformTests` 或 `NeverDTwoTableXformTests` |
| MedIR、ABI、标志、类型、SSA | MedIR/调用约定 lift 用例 | 跨 ISA 的 `NeverDSemanticTests` 用例 |
| HighIR 或结构化 C | HighIR/decompile 用例 | `NeverDCFGLoopXformTests` 与生成 C 编译检查 |
| PE/ELF/Mach-O loader 或输入重定位 | 对应的 `unittests/lift` 格式 fixture | 该单元格的全阶段加载/反编译测试 |
| 重写 codegen 或输出重定位 | `RewriteCodegenRTTests` 用例 | `NeverDPatchFullTests` 及存在时的已链接 patch fixture |
| patch 使用的 LLVM IR 变换 | 聚焦变换二进制 | `NeverDPatchFullTests` 组合 pass 网格 |
| C API 或 CLI | 直接 SDK/query 测试与 `unittests/semantic/CLIEndToEndTests.cpp` | 相关 pipeline/格式套件 |
| EVM loader、opcode、IR 或 backend | 最小的所属 `NeverDEVM*Tests` 目标 | 所有 EVM 目标，以及生成 C/Solidity 的编译检查 |
| SBF loader、ISA、IR 或后端 | 最小的所属 `NeverDSBF*Tests` 目标 | 所有 SBF 目标，以及生成 C/Rust 的编译检查 |
| Libc 识别 | `NeverDLibCTests` | 行为变化时的语义 call/ABI 用例 |
| 进程执行或 quoting | `NeverDTestProcessTests` | 每个受支持主机上的一个受影响 CLI/语义用例 |

测试应在最低的稳定边界表达契约。LowIR 形状测试适合归因到 lifter；若两种看似
合理的 IR 形状可能行为不同，则必须使用语义往返。若小型 opcode、CFG 或可观察状态
断言已经足够，应避免保存整个函数的 golden dump。

## 与 CI 的关系

CI 在 Linux、macOS 和 Windows 上以 Release 开启测试构建，先审核发现的测试清单，
再应用平台特定的标签排除。配置定义于 `.github/workflows/ci.yml` 和
`scripts/audit_ci_test_inventory.py`。由于没有单个矩阵 shard 代表所有昂贵套件，当
机器具备全部跨目标工具时，本地 `check-neverd` 仍是最清晰的完整合并前信号。

## 当前 Solana SBF 一致性与 sanitizer 配置

本节的当前清单取代上方较短的 SBF 清单。source differential suite 除 clang 外还
需要 `rustc`；compiler skip 表示覆盖缺失。完整 aggregate 包括
`NeverDSBFProgramImageTests`、`NeverDSBFMalformedCorpusTests`、
`NeverDSBFISAConformanceTests`、`NeverDSBFUpstreamConformanceTests`、
`NeverDSBFLLVMDifferentialTests`、`NeverDSBFSourceDifferentialTests`，以及 metadata、
loader、analyzer、semantic、emitter、integration target。integrated profile 在
14 个 binary 中通过 145/145 个 case。

sanitizer profile 单独构建在 `build-sbf-asan-ubsan`。13 个 core binary 的
141/141 个 case 均通过，且没有 ASan/UBSan report；prebuilt package 缺少所需的
fork-only header，因此 integration 在 integrated LLVM build 中运行。

```bash
cmake --build build-sbf-asan-ubsan --parallel 4 --target \
  NeverDSBFMetadataTests NeverDSBFProgramImageTests NeverDSBFLoaderTests \
  NeverDSBFAnalyzerTests NeverDSBFISAConformanceTests \
  NeverDSBFSemanticTests NeverDSBFEmitterTests NeverDSBFLLVMEmitterTests \
  NeverDSBFLLVMDifferentialTests NeverDSBFSourceDifferentialTests \
  NeverDSBFMalformedCorpusTests NeverDSBFUpstreamConformanceTests \
  NeverDSBFSolanaModelTests

ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:strict_string_checks=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
NEVERD_SBPF_ROOT=$PWD/local_docs/sbpf \
ctest --test-dir build-sbf-asan-ubsan --output-on-failure --parallel 4 \
  -L '^NeverDSBF' -E 'SBFIntegration'
```
