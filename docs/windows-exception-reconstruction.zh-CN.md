**语言**: [English](windows-exception-reconstruction.md) | [简体中文](windows-exception-reconstruction.zh-CN.md) | [繁體中文](windows-exception-reconstruction.zh-TW.md) | [日本語](windows-exception-reconstruction.ja.md) | [한국어](windows-exception-reconstruction.ko.md) | [Français](windows-exception-reconstruction.fr.md) | [Deutsch](windows-exception-reconstruction.de.md) | [Español](windows-exception-reconstruction.es.md) | [Italiano](windows-exception-reconstruction.it.md) | [Русский](windows-exception-reconstruction.ru.md) | [العربية](windows-exception-reconstruction.ar.md)

# Windows 异常重建

[← 文档索引](README.zh-CN.md)

NeverD 在加载、提升、反编译和二进制重写的全过程中携带 Windows 表驱动异常信息。
异常元数据属于函数的可执行契约：只有能够证明生成代码、runtime-function 记录、
语言表与防护表相互一致时，NeverD 才允许重写。

本文区分三种支持级别：

- **分析**：将原生表示解码为经过检查的规范化记录，并提供给 IR 流水线。
- **反编译**：把可规约保护区表示为显式 HighIR 异常节点；其他形状保留确定性的
  原生注释，不丢失 handler 或状态迁移。
- **原生重建**：patch 模式可要求 LLVM 发射完整替代异常契约，并将其安装到最终 PE。

支持分析并不等于支持原生重建。

## 支持矩阵

| 原生形式 | 提升与分析 | 高层输出 | Patch 模式 |
|----------|------------|----------|------------|
| x64 unwind v1/v2 | 完整、经检查的 unwind 记录、操作、链、handler 数据与来源 | 栈帧/unwind 摘要，并在适用时给出结构化语言区域 | 支持完整 primary 记录；生成的 `.pdata` 与 `.xdata` 替换被覆盖的闭包 |
| x64 unwind v3/APX | 独立的 v3 payload、epilog 与操作计数 | 显式 v3 注释 | 仅分析；拒绝修改涉及的函数 |
| ARM32/ARM64 packed unwind | 函数范围、packed 字段、primary/fragment 身份 | 栈帧/unwind 摘要 | 仅当记录完整、无语言 handler 且映像没有可独立寻址 fragment 时支持 |
| ARM32/ARM64 unpacked unwind | 经检查的 xdata header/code 范围、handler 关联与 fragment | 栈帧/unwind 摘要 | 仅当记录完整、无语言 handler 且映像没有可独立寻址 fragment 时支持 |
| `__C_specific_handler` | scope 范围、filter、finally 目标、handler 与 continuation 目标 | 可规约区域变为 `__try`/`__except`/`__finally`；不完整或不可规约区域保留注释 | 对完整且可表示的 scope 图执行原生 x64 重建 |
| `__CxxFrameHandler3` | unwind map、try map、catch、catch-object/frame offset、continuation 与 IP-to-state map | 可规约状态区间变为显式 C++ HighIR，并带 C 兼容类型注释 | 对下文所述严格受限且 verifier-clean 的子集执行原生 x64 重建 |
| `__CxxFrameHandler4` | 有界变长解码到公共 C++ 图，包括 action kind 与 object offset | 同一 HighIR 图并保留 FH4 来源 | 仅分析；拒绝修改涉及的函数 |
| `__GSHandlerCheck_SEH/EH/EH4` | 包装后的 personality 与经检查的 GS cookie 来源 | 基础语言图加 wrapper 注释 | 仅分析；拒绝修改涉及的函数，不做降级 |
| x86 registration-chain EH | 与表驱动 EH 明确区分 | 不支持形式的注释 | 不重建 |

畸形记录绝不会按普通完整记录处理。部分解码记录仍可用于检查，但不能授权生成原生
元数据。如果 ARM xdata header 仍能证明一个有界可执行 fragment 范围，而后续 unwind
body 已损坏，反汇编仍可使用该范围，但记录会被标记为 malformed，且不会升级为可
patch 函数。

## 规范化模型

`ExceptionInfo` 由 `BinaryImage` 所有。每个 `ExceptionFunction` 包含：

- 经检查的半开代码范围；
- primary、chained 或 fragment 身份；
- 原生 unwind 编码以及精确的 runtime/unwind 来源；
- 规范化 unwind 操作与 epilog；对于语义未完全理解的操作保留 opaque operand bytes；
- 精确的 personality 身份及其 handler 数据；
- 可选 SEH scope、C++ 状态图与 GS cookie 数据；
- `Complete`、`Partial` 或 `Malformed` 状态，以及确定性的诊断。

loader 不通过该模型暴露原始文件指针。原生 RVA 用于诊断和 patch 替换；IR 使用者只
操作已验证的 VA 与范围。

全映像索引允许 chained/fragment 记录重叠，并返回覆盖某地址的最具体函数。任何损坏
目录、范围、指针、计数、状态迁移、压缩整数、chain cycle 或 decode budget 耗尽都会
降低相应解析状态。

语言表限制既按每张原生表执行，也按单个函数的完整规范化图累计执行。因此，即使多个
try-map entry 复用同一 handler map，解析工作也不能超过总预算。共享同一 `FuncInfo`
与 personality 的 FH3 记录按有界函数组解码，使父函数的 IP-to-state map 可以合法指向
其 catch funclet，同时拒绝不相关 runtime function 的地址。

## IR 契约

异常元数据贯穿每种 IR 表示，同时不改变普通 CFG 的含义：

- LowIR 在保护范围边界、状态迁移、filter、handler、cleanup action 与 continuation
  target 处拆分 basic block。
- 异常 successor/predecessor 与普通 successor/predecessor 分开保存，现有 dominator
  与 structuring 算法不会把运行时分派边误认为机器分支。
- MedIR 保留规范化函数描述符与稳定异常边。
- HighIR 使用独立的 `SEHTry` 与 `CxxTry` statement。clause descriptor 保留原生
  target VA、type descriptor、adjective、catch-object/parent-frame offset、cleanup
  action kind/object offset、state 与 continuation VA。

HighIR structurer 对区间采取保守策略。它只移动地址完全位于完整保护范围中的一个连续
statement slice，并按从内到外处理嵌套区域。交叉区域、partial graph、无地址的歧义
边界和 out-of-line funclet 保留原控制流，并增加函数的 unstructured-EH 计数。

C backend 为可规约的单 clause SEH 区域发射 MSVC SEH 语法。由于 HighC 是 C backend，
C++ catch 与 cleanup state 以确定性的 C 兼容注释输出，不会伪称生成可编译 C++。
out-of-line 原生 funclet 保留精确地址。

## LLVM 元数据模式

每个与已发射函数关联的已解析异常函数都会获得无损 LLVM 元数据，即使它不能使用原生
WinEH lowering：

- 函数 attachment：`neverd.windows.eh`；
- 原生 lowering 标记：`neverd.windows.eh.native`；
- module table：`neverd.windows.eh.functions`；
- 当前 schema version：`3`。

固定函数记录携带 parse status、encoding、code range、原生 runtime/unwind RVA、
runtime-record kind 与 chain 来源、packed-unwind word、frame description、规范化和
已解析 personality 名称、handler data、精确原生 unwind bytes、规范化 operation
（含原生 slot count）与 epilog、SEH scope、C++ header/map、GS data、diagnostic 和
regeneration flag。patch 验证要求 schema version 精确匹配，并且范围与已加载映像
完全一致。具有异常契约的自动命名提升函数不能静默省略 attachment。

原生 x64 SEH lowering 使用 LLVM WinEH 结构；只有完整 scope graph 可表示时才发射
verifier-clean 的 `invoke`/funclet 控制流。原生 FH3 lowering 更严格，要求：

- x64 COFF、unwind v1/v2、完整元数据、有效的同步 FH3 状态图；
- 不含 `noexcept`、异步、separated-funclet、GS-wrapper、FH4 或未知 flag 语义；
- 保护区间嵌套或互不相交，不能交叉；
- 不含 destructor/unwind action、catch-object 构造或 parent-frame 依赖；
- handler 是提升函数中无普通 predecessor 且无 call 的 block；
- 每个可能 unwind 的受保护操作都由 LLVM `invoke` 表示。

任一条件不满足时，提升后的 LLVM 仍可分析并保留无损元数据，但 patch 规划会拒绝替换
原生语言表。PE entry point、TLS callback 与 CRT callback root 是保留边界，不作为
普通 ABI 重写候选。

## Patch 事务

对于受支持的重写，NeverD 将异常重建作为一个 PE 事务处理：

1. 根据已加载异常图与 LLVM metadata attachment 验证每个受影响函数。
2. 编译替代代码，同时保留 section identity、alignment、allocation flag、code/data
   trait 与语义 symbol-index reference。代码生成前将本地建模的 Windows personality
   externalize，使发射的 xdata 绑定到已证明的原始可执行 handler，而不是重新编译
   私有 ABI routine。
3. 保留未涉及的 runtime-function entry，并删除每个受影响 primary function 被替换的
   完整原生闭包，包括相关 chained record。
4. relocation 生成的 code/xdata，合并 generated/retained pdata，按 begin RVA 排序并
   拒绝重叠；证明每个重定向语言 EH entry 都被具备相同 personality class 的 generated
   runtime-function record 覆盖，然后安装唯一的替代 PE exception directory。
5. 保留输入 CFG instrumentation mode，解析 `.gfids` 语义引用，并将这些 target 与
   redirected entry 合并进原 Guard CF table。解析 `.gehcont` 语义引用为 generated
   executable VA，合并进原 Guard EH continuation table，并在保留 guard flag 的同时
   更新 load-config pointer/count。无法解析 CFG dispatch/check helper 会中止事务。
   需要不同 code-generation contract 的 guard mode（CFW、return-flow guard、
   retpoline、XFG）仅支持分析，并拒绝重写。
6. 写盘前重新解析完整 byte image。

LLVM fork 扩展有意保持通用：final-image writer 保存 object section 扁平化时会丢失的
section trait 与语义 symbol-index reference。PE 解析、MSVC 语言表解码、策略、目录
合并、load-config 更新和最终验证仍位于 NeverD。

原 Guard CF 与 Guard EH continuation entry 会保留，因为原 entry trampoline 仍是有效
间接 target。生成 target 必须指向 emitted code；最终表必须严格按 RVA 排序。

## 最终映像验证

除非满足以下全部条件，否则拒绝 patched PE：

- LLVM 将 bytes 接受为 COFF object，且 PE machine、class、section table、optional-header
  directory bounds、image base 与 image extent 一致；
- 每个 section 的 raw/virtual extent 在界内，且 section range 不重叠；
- exception-directory extent 由文件承载且位于映像内；
- runtime-function entry 有序、非空、无重叠且完全位于可执行区域；
- x64 unwind RVA 对齐，header/code array 由文件承载，version/flag 受支持，handler
  target 可执行，chained record 无环且满足 depth limit；
- 在内存中重建最终 import、export 与 COFF symbol，使已知 SEH/FH3 personality 能从
  完整 bytes 再次解析其 scope/state table；
- ARM runtime entry 与 xdata 标识有效且受支持的 version/range；
- guard flag 声明表时，load-config 中存在 Guard CF 与 Guard EH continuation 字段；
- guard pointer/count/stride 同时位于 PE image 与文件范围内，且每个 entry 严格排序并
  指向可执行目标。

验证失败会中止 patch。NeverD 不会在验证失败后写出 best-effort 映像。

## 聚焦验证

构建 lift suite，并运行 Windows EH model、parser、IR、codegen 与 PE integration case：

```bash
cmake --build build --target NeverDLiftTests --parallel 4
build/bin/NeverDLiftTests \
  --gtest_filter='COFFException*:*PatchCOFF_X64.ReconstructsGuardedSEHAndContinuationTable:*PatchCOFF_X64.ReconstructsNativeFH3StateGraph:*PatchCOFF_X64.RejectsInteriorExceptionDirectoryPadding:*PatchCOFF_X64.RebuildsSortedExceptionDirectoryInAppendedSection'
```

受保护 x64 fixture 使用 `/guard:cf` 与 `/guard:ehcont` 交叉汇编和链接。集成测试加载其
SEH scope 与 guard table，检查结构化 HighC 输出，patch 映像、重新加载，并验证更新后
table count、顺序及 executable target。

独立链接的 x64 FH3 fixture 通过同一完整事务覆盖受支持 C++ 闭包。它验证原固定表、
HighC 状态注释、personality 绑定保留、重建的 try/catch 图，以及 patch 后重新加载得到的
IP-to-state map。

修改 parser 时还要运行现有 ARM format case，因为 ARM packed/unpacked xdata 共用规范化
模型与最终 runtime-entry 检查。

## 扩展原生支持

新增原生重建支持时，必须在同一变更中包含：

- 完整有界 parser 与规范化模型 invariant；
- HighIR 和 LLVM metadata round-trip 覆盖；
- 每种新接受 graph shape 对应的 verifier-clean 原生 IR；
- 必要的 emitted-section 与 semantic-reference 保留；
- 对精确 architecture/personality/version 的已链接 PE fixture；
- exception-directory、load-config 与 final-image 结构验证；
- 对最相近不支持形状的显式拒绝测试。

不能仅因能够解码新记录就扩大 allow-list。接受标准是最终链接映像中的运行时异常行为
得到保留。
