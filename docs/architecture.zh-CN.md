**语言**: [English](architecture.md) | [简体中文](architecture.zh-CN.md) | [繁體中文](architecture.zh-TW.md) | [日本語](architecture.ja.md) | [한국어](architecture.ko.md) | [Français](architecture.fr.md) | [Deutsch](architecture.de.md) | [Español](architecture.es.md) | [Italiano](architecture.it.md) | [Русский](architecture.ru.md) | [العربية](architecture.ar.md)

[← 文档索引](README.zh-CN.md)

# NeverD 架构

本指南说明贡献者安全修改 NeverD 所需了解的生产边界。内容有意仅涵盖
NeverD 自有代码；LLVM、Capstone 和 Unicorn 子模块维护各自的内部架构。

## 系统边界

```mermaid
flowchart LR
  CLI["tools/neverd CLI"] --> CAPI["libneverd C API"]
  SDKUser["SDK user or plugin"] --> CAPI
  CAPI --> Session["sdk::Session"]
  Session --> Loader["format loader"]
  Loader --> Image["BinaryImage"]
  Image --> Pipeline["Pipeline"]
  Pipeline --> Low["LowIR"]
  Low --> Med["MedIR"]
  Med --> High["HighIR"]
  High --> HighC["structured C"]
  Med --> LLVM["LLVM IR"]
  LLVM --> LLVMOut["LLVM IR or LLVM-derived C"]
  LLVM --> Codegen["target codegen"]
  Codegen --> Rewriter["PE / ELF / Mach-O rewriter"]
  Rewriter --> Patched["patched binary"]
```

NeverD 有四种 IR 表示，但它们并非一条必须经过四跳的序列。`LowIR -> MedIR`
为共享部分；结构化反编译随后采用 `MedIR -> HighIR -> C`，而 `lift`、
`decompile --llvm` 和 `patch` 则直接走 `MedIR -> LLVM IR`。尤其是 patch 与
lift 模式会有意跳过 HighIR。

CLI 在 `tools/neverd` 中解析命令，创建 `neverd_session_t`，并调用
`include/neverd/sdk/NeverDCAPI.h` 中的公共 API。引擎状态位于
`lib/sdk/SessionImpl.h`；`neverd_session_load` 选择 loader 并构造
`BinaryImage`，基于 IR 的操作则按需运行 `lib/pipeline/Pipeline.cpp`。
`neverd` 可执行文件链接 `neverd_shared`；组件归档以及其 LLVM/Capstone
依赖是该共享库的私有实现细节。CLI 使用 LLVM Support 构建命令行界面，
但不会绕过 C API 驱动引擎。

## IR 表示与路径

| 表示 | 用途 | 主要定义与转换 |
|------|------|----------------|
| LowIR | 架构无关的 `NdOp` 操作、基本块、CFG 和跳转表元数据 | `include/neverd/ir/low`、`lib/ir/low`，由 `lib/decode` + `lib/lift` 生成 |
| MedIR | 类型、ABI/调用约定、内存与栈模型、标志、调用和类 SSA 数据流 | `include/neverd/ir/med`、`lib/ir/med` |
| HighIR | 用于可读 C 的结构化表达式与控制流 | `include/neverd/ir/high`、`lib/ir/high`，由 `lib/backend/c/HighC` 发射 |
| LLVM IR | 优化、LLVM 派生 C、目标代码生成和二进制重写输入 | `lib/backend/llvm`，由 `lib/pipeline` 优化/编排 |

| 用户路径 | 表示路径 | 出口 |
|----------|----------|------|
| Low/Med dump | Binary -> LowIR，可选 -> MedIR | 诊断文本 |
| High dump 或 `decompile` | Binary -> LowIR -> MedIR -> HighIR | HighIR 或结构化 C |
| `lift` | Binary -> LowIR -> MedIR -> LLVM IR | `.ll` |
| `decompile --llvm` | Binary -> LowIR -> MedIR -> LLVM IR | LLVM 派生 C |
| `patch` | Binary -> LowIR -> MedIR -> LLVM IR -> codegen | 重写后的二进制 |

`lib/pipeline/Pipeline.cpp` 是路径选择的事实来源。特定表示的逻辑应留在其
所属 IR 或 backend 库中；pipeline 应编排这些组件，而不是吸收它们的算法。

## 跨架构翻译契约

`include/neverd/translate` 定义的是契约层，而不是执行后端。`GuestState`
为 `x86_32`、`x86_64`、`AArch64` 和 `ARM32` 建模架构无关的机器可见状态。
其规范的版本 1 序列化采用固定宽度的小端字段、稳定的寄存器 ID、有序集合和
失败即关闭的验证，因此持久化状态不依赖宿主 C++ 布局。

`GuestState` 的 wire v1 基线永久冻结。基线以外的机器状态只能使用扩展区间内的
extension-register ID，并配套规范的小写名称；否则必须采用新的 wire 版本并提供
显式 upgrader，禁止原地改变 v1 基线。

对于 `ARM32` guest，`ExecutionMode` 是权威解码模式，并且必须与 `CPSR.T`
一致。保存的 PC 始终是清除 bit 0 后的规范指令地址；ARM 模式还要求按字对齐。

架构对策略定义 `x86_64 -> AArch64`、`AArch64 -> x86_64`、
`x86_32 -> AArch64/ARM32` 和 `ARM32 -> x86_32/x86_64`。
`ContractDefined` 表示请求可以验证和持久化，并不表示代码已经可以翻译或执行。
JIT 策略只接受运行中进程的本机宿主；AOT 策略则要求显式给出宿主架构、目标
triple；若选择了 CPU 或特性集合，也必须显式给出。

`ResolvedHostTarget` 将该选择解析为具体结果。`Native` 解析从当前进程取得 triple、
CPU 以及启用/禁用的特性集合；`Explicit` 解析验证并规范化调用方提供的架构、triple、
CPU 和特性，并拒绝互相冲突的输入。其带版本的缓存标识按确定的字节顺序从规范化目标
输入构造，不包含进程地址或依赖 locale 的文本。

带版本的 `TranslationExit` 记录稳定的停止原因及其匹配的类型化载荷，覆盖系统
调用、异常或信号、断点、不支持的指令、自修改、资源预算、外部调用、内存故障
以及其他终止条件。使用方无需再根据停止原因重新解释一个无类型整数。

除与对应预算匹配的 `BudgetExhausted` 外，结果报告的指令数、block 数和生成代码量
都不得超过请求中的对应非零预算。指令与 block 耗尽会精确停在 limit。生成目标文件
大小只能在不可分割的 codegen 完成后精确测量，因此该预算耗尽结果可以报告
`Observed > Limit`；被拒绝的目标文件绝不会被链接、发布或执行。每个
`BudgetExhausted` 载荷都必须精确标识请求的 limit，不能报告推导值或实现私有阈值。

backend-private `RuntimeControlBlockV1` 契约固定为 128 字节、8 字节
对齐，并以固定的 v1 magic、version、size、字段偏移、全零保留字段和自洽的类型化
退出记录加以约束。它不包含 C++ 容器、宿主指针或 guest 地址别名，也不是
`GuestState` 的 C++ 布局或 wire 格式；实现该契约的后端必须显式把状态转换到该记录。

固定的 v1 generated-code 调用面只包含八个 helper：
`nvd_rt_v1_load8_le`、`nvd_rt_v1_load16_le`、`nvd_rt_v1_load32_le`、
`nvd_rt_v1_load64_le`、`nvd_rt_v1_store8_le`、`nvd_rt_v1_store16_le`、
`nvd_rt_v1_store32_le` 和 `nvd_rt_v1_store64_le`。名称、签名和指针 provenance
必须精确匹配；后端必须显式绑定这个有限表，绝不能回退到环境符号解析。可执行内存
generation 验证和预算/取消轮询只由受信任 dispatcher 执行；
`nvd_rt_v1_validate_generation` 和 `nvd_rt_v1_poll` 均不是 generated-code helper。
受信任宿主 dispatcher 还负责选择 block，生成 IR 不能调用它；translated block
只返回类型化退出码。生成 IR 只能直接读取声明过的 scalar-result runtime slot。

`RuntimeSymbolRegistryV1` 将该 helper 表实现为封闭的宿主侧注册表。构造过程验证完整的
ABI-v1 集合、精确的规范名称、helper class、签名，以及每项唯一一个非空且与 class
匹配的函数指针。查找只接受精确名称，绝不查询进程环境或动态加载器的符号，并向目标
文件 verifier 提供同一组有序名称作为 allowlist。其带版本的标识覆盖名称、helper
class 和 ABI 形状，但有意排除本机地址，因此不受 ASLR 影响。

`RuntimeCodeMemory` 管理按页隔离的生成代码存储，只允许单向 `RW -> RX` 发布转换。
内存不会同时可写和可执行，发布后不能重新开放写入；写入和入口偏移均经过边界检查，
发布时还会刷新宿主指令缓存。本机 smoke test 只在发布后执行一小段宿主指令；它证明的
仅是这个 W^X 内存边界，而不是翻译引擎。

`GuestMemoryRuntime` 与逻辑 `GuestState` 隔离：构造时先验证状态，再把内存区域的
字节和元数据复制到有序的私有索引。guest 虚拟地址只作为查找键，绝不会转换为
宿主指针。受检标量访问会以类型化形式报告宽度、对齐、溢出、未映射、跨区域、
权限、可执行写入、generation 溢出、generation 不匹配和策略故障。指令/block
预算、取消、generation 跟踪以及 `RejectExecutableWrites`、
`InvalidateOnExecutableWrite`、`ValidateBeforeDispatch` 三种代码写入策略同样生成
自洽的类型化记录，而不是隐式宿主行为。

`TranslationObjectCompilerV1` 是经过验证的 LLVM IR 到目标文件边界。它先验证 const
输入 module，在任何变换前完成 clone，将证明门控的语义化简与 LLVM `O0` 至 `O3`
优化组合，再次验证最终 IR，并为四种契约宿主架构发射 relocatable ELF、COFF 或
Mach-O 目标文件。它规范化精确的 target-mangled block/runtime 符号 manifest，审计
每个发射结果，并返回 runtime registry identity 以及带版本的请求和制品 cache key。
生成字节预算非零时，只有满足它的目标文件才能继续进入制品验证。LLVM 先向私有缓冲区
完成一次不可分割的发射以取得精确大小；超限目标文件会在发布和制品审计前被拒绝，类型化
遥测保留实际大小与请求的精确 limit。零表示调用方策略不设上限。编译器止步于已审计的
relocatable 字节：它不负责链接、发布、分派或执行，也不提供 guest 指令 lowering。

post-codegen verifier 把 relocatable ELF、COFF、Mach-O 目标文件作为
闭集审计。格式和架构必须与选定宿主精确匹配；未定义符号必须精确属于有限 helper
allowlist，动态符号一律禁止。relocation 采用显式直接白名单，并检查 encoding、
width、alignment、offset、可加载目的节，以及目标是否为目标文件内
non-preemptible 定义或精确获准的 helper。verifier 拒绝 W+X、异常/展开与初始化
元数据、TLS、IFUNC、GOT 与普通 PLT 间接机制、动态 relocation、weak/preemptible
或可选择定义、未知 allocated section 和 linker directive。只有当 v1 policy 证明
LLVM 隐藏的 x86-64 ELF `R_X86_64_PLT32` 是指向精确 runtime helper 的 sealed direct
branch 时才允许该拼写；它不会放行 PLT 或 GOT 路径。ELF `ET_REL` 制品不得包含
program header 或 segment。Mach-O load command 采用正向白名单：必须且只能有一个
位宽匹配的 segment，symbol table、dynamic-symbol table、platform-version 和
data-in-code command 各至多一个，并检查相互依赖；linker option 和其他所有 command
均拒绝。

`TranslationObjectRequestV1` 是建立在上述契约上的首个公开、且有意收窄的
guest 字节到目标文件切片。在当前发布的失败封闭 x86-64 v1 标量寄存器子集中，它只
接受无 legacy prefix 的 canonical 编码：采用受支持寄存器/立即数 LowIR 形状的
REX.W 全宽 GPR `MOV`、`ADD`/`SUB` 与 `AND`/`OR`/`XOR`。schema 9 还接受全宽
寄存器/寄存器 `CMP` 编码 `39/3B`、寄存器/立即数 `CMP` 编码 `81/7`、`83/7` 与
`3D`、寄存器/寄存器 `TEST` 编码 `85`，以及寄存器/立即数 `TEST` 编码 `F7/0` 与
`A9`。算术形式保留相应的标量 flags 计算；逻辑形式和 `TEST` 计算架构定义的 flags，
并在 NeverD 状态模型中保持 `AF`。canonical `C3` `RET` 和
`C2 iw` `RET imm16` 终止返回 block；canonical `EB cb` 与 `E9 cd` 直接相对 `JMP`
编码终止直接分支 block。当前公开 lowering schema 为 9。canonical、无 legacy prefix
的传统 Jcc 仅支持以下形式：`JO`/`JNO` 的短形式 `70/71 cb` 或近形式 `0F 80/81 cd`；
`JB`/`JAE` 的 `72/73 cb` 或 `0F 82/83 cd`；`JE`/`JNE` 的 `74/75 cb` 或
`0F 84/85 cd`；`JBE`/`JA` 的 `76/77 cb` 或 `0F 86/87 cd`；`JS`/`JNS` 的
`78/79 cb` 或 `0F 88/89 cd`；`JP`/`JNP` 的 `7A/7B cb` 或 `0F 8A/8B cd`；
`JL`/`JGE` 的 `7C/7D cb` 或 `0F 8C/8D cd`；`JLE`/`JG` 的 `7E/7F cb` 或
`0F 8E/8F cd`。`JRCXZ`/`JECXZ`/`JCXZ` 与 `LOOP`/`LOOPE`/`LOOPNE` 仍未发布，
并以 fail-closed 方式拒绝。保留的 `F7 /1`、guest-memory 操作数、部分寄存器形式、
legacy prefix 和语义冗余的 REX 扩展位同样以 fail-closed 方式拒绝。输出仅限经过审计的
little-endian AArch64 ELF 或 Mach-O relocatable 目标文件。普通 guest 内存操作、部分
寄存器形式、该精确子集外的任意指令或编码、返回、这些直接跳转和上述已发布 Jcc
分支以外的控制流，以及 lowerer 尚未实现的任何 LowIR 操作都会在目标文件生成前被拒绝。
`RET` 所需的受检返回地址读取属于其 terminator 契约的内部
行为，并不发布通用 guest 内存 lowering。请求会重新构建并验证 block descriptor，
lowering 与目标文件生成共用同一个已解析 target machine，并将证明门控的语义化简与
LLVM 默认 `O2` 优化流水线组合。该切片不代表支持其他 x86-64 指令、其他 guest/host
组合或反向 AArch64 到 x86-64 翻译。

公开 C 入口 `neverd_translate_x86_64_block_to_aarch64_object_v1`、Python ctypes wrapper
`translate_x86_64_block_to_aarch64_object` 和 `neverd translate-object` 命令暴露同一个
仅生成目标文件的边界。Python 使用 `TranslationObjectFormat.ELF` 或 `.MACHO`；原生库
报告的翻译失败会抛出携带 `TranslationErrorCode` 的类型化 `TranslationError`，本地
参数验证则抛出 `TypeError` 或 `ValueError`。成功时返回由 Python 拥有的不可变结果。
C 结果拥有目标文件字节、稳定 cache identity 与优化遥测；CLI 只写出选定的 ELF 或
Mach-O 目标文件。这些 C、Python 与 CLI 对象接口都止步于链接、加载、分派、执行和
调试之前；它们不是执行 session 接口。

`verifyTranslationLinkGraphV1` 增加第二道独立的 allocation 前审计。它从已接受的
AArch64 ELF 或 Mach-O 目标文件建立临时 LLVM JITLink graph，并检查 target、section
权限、block/runtime 符号 manifest、外部符号闭包以及 edge 类型与目标。产生不含地址的
审计结果后即销毁 graph。通过该审计不等于链接、分配、解析、加载、发布、分派或执行代码。

`linkTranslationObjectV1` 是独立的原生链接边界。它在裁剪、分配、符号解析和 fixup
前后重新审计可信 descriptor、原始对象和 JITLink graph。runtime 符号只能来自 sealed
注册表。dispatcher credential 将唯一的 manifest 条目绑定到对应 session、block identity、
guest 入口 PC、cache generation 与 code epoch；调用时 runtime guest `RIP` 还必须匹配
该入口。成功 finalize 后以最终权限发布可执行内存；unload 会撤销新调用，并等待一个
正在进行的调用结束后再释放 allocation。无 credential 的 overload 仍仅用于审计，不能调用。

`NativeTranslationSessionV1` 将这些组件组合为实验性的 C++ x86-64 到原生 AArch64
执行边界。在 little-endian AArch64 ELF 或 Mach-O 进程上，它在 compile-link-validate-
invoke-unload dispatcher 循环中跨 block 保持同一个受检 guest-memory runtime 和固定
guest state。canonical 直接跳转会在其精确静态目标继续执行。已发布的 canonical
Jcc 分支只能在 block manifest 声明的 taken 或 fallthrough successor 继续；dispatcher
拒绝其他任何选定 PC。返回会终止执行。全局指令数、block 数和生成对象字节数预算在多个
block 间保持精确；guest 成功停止时，已执行状态与权威内存一并提交。取消操作与最终提交
线性化。

这是一个可执行的纵向切片，而不是完整翻译器。它尚不支持普通 guest-memory 指令、
部分寄存器、上述精确 schema-9 传统 Jcc 切片之外的条件控制流（包括
`JRCXZ`/`JECXZ`/`JCXZ` 与 `LOOP`/`LOOPE`/`LOOPNE`）、间接控制流、
调用、浮点、SIMD、x87、原子操作、系统指令、
通用异常传播、block cache、其他 guest/host 架构对或反向 AArch64 到 x86-64。执行 session
尚无 C、Python、CLI 或 JSON 接口，调试仍是独立且不受支持的能力。上述对象 API 无需
启用原生执行仍可单独使用。

生成 IR 契约要求受该契约约束的每个 translated block 都是 hidden、non-preemptible，
并采用 C ABI `i32 (ptr state, ptr runtime)`。runtime 只能通过私有注册表发现 block，
不能依赖进程环境的符号查找；禁止 block 之间直接调用。

IR verifier 还将整数宽度限制在宿主标量寄存器宽度以内，以避免 legalization 引入
已知 compiler-runtime libcall。该检查只是必要条件：任何实现该契约的执行后端都
必须依据同一有限的 runtime-symbol allowlist，对 post-codegen 控制转移、`MachineIR`
和目标文件 relocation 进行精确审计。

TranslationIR 的直接 load/store 以及 private constant 保存的值，只能包含单个、不宽
于宿主标量寄存器宽度的标量整数。聚合值必须在 verifier 边界前完成标量化，避免紧凑
IR 触发后端无界展开。

generated-code ABI 只为标量整数定义。浮点、SIMD、x87、原子操作和系统指令均在
该契约之外。选择 `ProvenSemanticAndLLVM` 策略的实现必须运行 NeverD 现有的证明
门控语义简化，并与 LLVM 优化共同达到不动点；该策略本身不提供可执行翻译后端。

## 异常重写边界

Mach-O compact unwind 当前具备原始 `__unwind_info` 的严格 parser、生成
`__LD,__compact_unwind` 记录的 fixup-aware parser、原始/生成区间的精确 merge、
regular page 的确定性 encoder，以及事务式最终 section installer。installer 仅在已有、
file-backed 的 `__TEXT,__unwind_info` 能容纳编码结果时原位重写；它会重新校验架构、布局
和原始字节，清零未使用尾部，并在 Mach-O 外层事务单次提交前重新解析结果、证明语义等价。
生成记录通过编译器精确记录的 IR 源函数到目标 MC owner symbol 映射（包括私有定义，且不
猜测对象格式前缀或改名规则）、opaque 非零 range ID 和精确半开片段区间进行认证。每个生成
FDE 都必须精确匹配唯一认证片段；每个必需片段也必须精确匹配该事务安装的唯一 FDE，除非它
由一条精确且经过严格 encoding 校验的非 DWARF compact 记录覆盖。同一函数拥有的相邻或
不相邻片段可以复用同一源 recipe；缺失、重复、悬空、跨 owner 或边界不一致的身份都会在
修改输出前失败。新增 RX segment 只有在证明 `__LINKEDIT` 唯一且位于 file/VM 末端、所有
offset relocation 均经过溢出检查，并严格回放最终文件与虚拟地址布局后才会提交。最终
section 缺失时不安装生成 compact 记录，且仅在通过上述精确、已认证的 DWARF-FDE 闭环时
才可继续事务；已有最终 section 容量不足或格式错误时仍会 fail closed。已链接的原生
throw/catch 证明仍未完成。

外部引用依据完整的 MC fixup 契约分类。call 只能选择经过认证的可调用目标；生成的
compact-unwind personality 字段只能选择经过校验的 non-lazy pointer slot，且绝不解引用
其文件内容。TLS、authenticated pointer、减项、格式错误的 compact 字段和未知 relocation
都会 fail closed。

ARM32 compact unwind 的已编码栈调整与 GPR 布局为 `Complete`；D 寄存器模式选择值 0 至 3
同样为 `Complete`。选择值 4 至 7 为 `Partial`，因为仅凭 compact word 无法证明每个经运行时
对齐的 CFA 相对 slot。`Partial` 条目可为分析保留已证明的寄存器身份，但所有重写路径都会
以 fail-closed 方式拒绝。每份 EH-frame 安装 receipt 都精确绑定目标架构、指针宽度与字节序；
compact-unwind DWARF 绑定会拒绝任何 receipt target identity 不匹配。

顶层 ARM32 section 事务的能力边界比 compact-unwind 解码器更窄。只有 Mach-O header
精确为 `CPU_SUBTYPE_ARM_V7K`，且原始 symbol table 的 `N_ARM_THUMB_DEF` 位对每个必需
函数都提供 Thumb code 的正向证明时，才会开放这条路径。此后，精确的
`thumbv7k-apple-watchos` triple 与 Thumb mode 会贯穿并约束整个 code generation，输入的
feature 需求也不得超过 Cortex-A7 上限。未标记或模式未知的函数、generic non-v7k
subtype、ARM mode、混合或未知的 external-code target、ARM Mach-O in-place entry point，
以及从 C source 发起的 ARM Mach-O patch，都会在修改输出前 fail closed。对于 stripped
输入，如果只能通过 `LC_FUNCTION_STARTS` 发现函数，目前仍不支持。

PE、ELF 与 Mach-O 各自具备格式特定的异常组件，但 NeverD 尚未公开覆盖所有格式、
所有异常类型的端到端重写流水线。不支持的 encoding 或未解析的注册/layout 要求必须
在修改输出前失败；现有的局部格式能力不能描述为异常重写已经完全闭环。

识别 Ada 或 D 的 Itanium personality 并不等于支持 Ada 或 D 异常。GNAT、GDC、DMD
与 LDC 的 address-form LSDA 可解析；type-table 槽位保持不透明（GNAT 为
`Exception_Id` / `Exception_Data`，D 为 `ClassInfo`），且绝不会按
`std::type_info` 解引用。原生重建会发出 LLVM `personality` 以及 address-form 的
`invoke`/`landingpad` 子句。corpus-proven 是另一层声明，不能由 personality 识别
或原生 lowering 自行推出。

## 组件映射

每个组件都是由 `add_neverd_component_library` 创建的静态归档。下表列出重要的
NeverD 依赖，不穷举 CMake helper 统一提供的 LLVM 和 Capstone 库。

| 目录 | 职责 | 重要依赖 |
|------|------|----------|
| `lib/loader` | 格式检测、PE/COFF、ELF、Mach-O 加载；规范化 `BinaryImage`；函数发现 | LLVM Object API |
| `lib/lift` | 手写 x86/i386、AArch64、ARM32 指令语义 | IR 数据类型 |
| `lib/decode` | Capstone/native 解码并分派到架构 lifter | `NeverDIR`、`NeverDLift` |
| `lib/ir` | 公共类型以及 LowIR、MedIR、HighIR、intrinsic 定义/转换 | 四个 IR 子组件 |
| `lib/pipeline` | 函数检测与 Low/Med/High/LLVM 路径编排 | IR、decode、lift、LLVM backend、调试信息、IR pass |
| `lib/backend/c` | HighIR 到 C 与 LLVM IR 到 C 的渲染 | IR |
| `lib/backend/llvm` | MedIR 到 LLVM 的 lowering | IR |
| `lib/backend/codegen` | 目标代码生成及 PE/ELF/Mach-O patch 与原地重写 | IR、loader |
| `lib/sdk` | 公共 C ABI、session 生命周期、查询、持久化、插件、lift/decompile/patch 入口 | 将引擎组件聚合为 `libneverd` |
| `lib/pass` | LLVM IR 混淆 pass 与 MIR pass runner | IR |
| `lib/debug` | DWARF、PDB 和 linker-map 调试上下文 | IR |
| `lib/sigs` | 签名解析、数据库与匹配 | Loader |
| `lib/libc` | 已知 libc 名称与调用模型支持 | 独立组件 |
| `lib/support` | 共享二进制加载 helper | Loader |
| `lib/translate` | 带版本的 guest state/策略/退出、固定 runtime ABI、受检 guest memory、生成 IR/目标文件/LinkGraph 审计、sealed 原生链接，以及实验性的 x86-64 到 AArch64 C++ dispatcher | IR、LLVM、LLVM Object 与 JITLink 契约 |

公共头文件在 `include/neverd` 下对应这些区域。不要意外让内部 C++ 类成为 SDK
的一部分：稳定的外部操作应放入纯 C 头文件及某个职责明确的
`lib/sdk/NeverDCAPI*.cpp` 文件。

## 严格提升契约

`Decoder` 和每个架构 lifter 默认以严格模式启动。如果 Capstone 可以解码一条
指令，但选中的 lifter 没有实现，lifter 会抛出 `UnliftedInstruction`。异常记录
指令地址、助记符和操作数字符串；因此，不支持的语义必须明确失败，而不能被省略
或猜测。

内部非严格路径会发射 `NdOp::NOP`，但这只是诊断逃生口，不是指令的可接受实现。
贡献者测试和 CI 应保持严格模式开启。当出现严格失败时：

1. 使用最小的架构特定 fixture 复现。
2. 在 `lib/lift/<ISA>` 中补充缺失语义。
3. 在 `unittests/lift` 中断言预期的 LowIR 形状。
4. 若指令存在可观察行为，在 `unittests/semantic` 中添加 Unicorn 差分往返。

不要仅为了让 pipeline 继续而捕获 `UnliftedInstruction`。新的有意近似需要明确契约
和测试；不得伪装成 1:1 提升。

## 格式与 ISA 所有权

输入格式逻辑与输出重写逻辑有意分离：

| 格式 | 加载、元数据与输入重定位 | Patch 与输出重定位 |
|------|--------------------------|--------------------|
| PE/COFF | `lib/loader/COFF` | `lib/backend/codegen/COFF` |
| ELF | `lib/loader/ELF` | `lib/backend/codegen/ELF` |
| Mach-O | `lib/loader/MachO` | `lib/backend/codegen/MachO` |

架构 lifter 位于 `lib/lift/X86`、`lib/lift/AArch64` 和 `lib/lift/ARM`。
相应的公共 lifter/register 声明位于 `include/neverd/lift`。目标特定的 LLVM
发射和代码生成位于 `lib/backend/llvm/<ISA>` 及
`lib/backend/codegen/CodeGen<ISA>.cpp`。

<a id="support-and-test-depth"></a>

### 支持范围与测试深度

根目录支持矩阵表示每个单元格均已实现；这不代表每条 opcode、ABI 边界情况、
二进制生产器或操作系统版本都已穷尽测试。指令语义超出 lifter 已实现覆盖范围时，
严格模式会以失败即关闭方式停止。

全部 12 个格式×架构单元格都在
`unittests/semantic/PatchFullSubstRTTests.cpp` 中具有语义重写后端覆盖。
集成深度则更具体：

| 格式 | x86-64 | i386 | AArch64 | ARM32 |
|------|--------|------|---------|-------|
| PE/COFF | 已链接 fixture | 后端网格 | 已链接 fixture | 已链接 Thumb fixture |
| ELF | 已链接 fixture + 语义往返 | 对象流水线 + 语义往返 | 已链接 fixture + 语义往返 | 已链接 fixture + 语义往返 |
| Mach-O | 已链接 fixture\* | PIC/no-PIC 对象流水线\* | 已链接 fixture\* | 后端网格 |

- **已链接 fixture** 对代表性程序执行已链接可执行文件的 loader/pipeline 与
  patch 行为。
- **对象流水线** 对可重定位对象执行加载、全部 IR 阶段和反编译，但不涵盖主机
  链接及 patch 后二进制的执行。
- **后端网格** 通过精确的重写代码生成路径编译代表性 IR，并在 Unicorn 中比较
  行为；它不对已链接可执行文件运行该格式的 loader。
- `*` Mach-O 已链接 fixture 依赖能够生成所需目标的主机工具链。现代 macOS
  无法链接历史 i386 可执行文件，因此 i386 使用 PIC 与 no-PIC thin 对象加重写网格。

对于这些代表性程序，应把已链接 fixture 单元格视为最强的格式集成证据。
对象流水线和后端网格单元格只有部分格式集成覆盖。没有任何单元格能在不加限定的
情况下称为“完全测试”，也没有单元格宣称穷尽 ISA 覆盖。

主要证据包括：用于已链接 ELF 与 PE fixture 的
[`PatchFormatTests.cpp`](../unittests/lift/format/PatchFormatTests.cpp)，用于 Windows ARM
加载/反编译的
[`COFFARMFormatTests.cpp`](../unittests/lift/format/COFFARMFormatTests.cpp)，用于 i386 thin
对象的
[`MachOI386RelocationTests.cpp`](../unittests/lift/format/MachOI386RelocationTests.cpp)，
用于已链接 Mach-O 的
[`X86_64_PipelineE2ETests.cpp`](../unittests/lift/x86_64/X86_64_PipelineE2ETests.cpp) 与
[`AArch64_PipelineE2ETests.cpp`](../unittests/lift/aarch64/AArch64_PipelineE2ETests.cpp)，
以及覆盖 12 单元后端网格的
[`PatchFullSubstRTTests.cpp`](../unittests/semantic/probe/patchfull/PatchFullSubstRTTests.cpp)。
命令见[测试指南](testing.zh-CN.md)。

## 在哪里修改

| 变更 | 从这里开始 | 最小聚焦验证 |
|------|------------|--------------|
| 添加或修复指令 | `lib/lift/X86`、`AArch64` 或 `ARM` 中的相应文件；分派变化时修改公共 lifter 头文件 | `unittests/lift` 中的架构测试；`unittests/semantic` 中的语义往返 |
| 添加 `NdOp` | `include/neverd/ir/NdOps.h`，随后审查 Low-to-Med、emitter/renderer、verifier/emulator 与 dump | `NeverDLiftTests` + 相关 `NeverDSemanticTests` 用例 |
| 修改 CFG 或函数发现 | `lib/ir/low`、`lib/loader/FunctionDiscovery*.cpp`、`lib/pipeline/PipelineFuncDetect.cpp` | lift CFG/跳转表测试和聚焦的语义变换套件 |
| 添加 PE 输入重定位或 unwind 规则 | `lib/loader/COFF` | `COFFARMFormatTests` 或新的聚焦 loader fixture |
| 添加 PE 输出重定位或 patch 规则 | `lib/backend/codegen/COFF` | `PatchFormatTests`、`RewriteCodegenRTTests` 与 PE 后端网格 |
| 修改 ELF 或 Mach-O 格式行为 | 对应的 `lib/loader/<Format>` 和/或 `lib/backend/codegen/<Format>` 目录 | 对应格式测试加重写网格 |
| 修改 MedIR/ABI 恢复 | `lib/ir/med` | 调用约定 lift 测试 + 跨 ISA 语义往返 |
| 修改结构化控制流恢复 | `lib/ir/high` | `NeverDCFGLoopXformTests` 与结构化 C 测试 |
| 添加 LLVM 变换 | `lib/pass/ir`、`include/neverd/pass/ir` 中的公共头文件，暴露时添加 pipeline 开关 | 聚焦变换套件 + patch 输出变化时的 `NeverDPatchFullTests` |
| 添加 C API 操作 | `include/neverd/sdk/NeverDCAPI.h`、聚焦的 `lib/sdk/NeverDCAPI*.cpp`，仅在需要状态时使用 `SessionImpl.h` | SDK/CLI 语义测试；保持 `neverd_last_error` 与分配约定 |
| 添加 CLI 命令 | `tools/neverd/NeverDCLIOptions.cpp`、`NeverDCLI.h`、聚焦的 `NeverDCmd*.cpp`，以及 `neverd.cpp` 中的分派 | `unittests/semantic/CLIEndToEndTests.cpp` 与直接 CLI smoke test |
| 添加语义回归 | 聚焦的 `unittests/semantic/*Tests.cpp`；在 `unittests/semantic/CMakeLists.txt` 注册新文件 | 构建其测试二进制，再用 `ctest -R` 选择命名用例 |

保持修改范围精确。定义某种表示的文件可以与其转换一起变化，但不要仅为让大型
重构看起来统一而修改无关的 loader、lifter 和 backend。
