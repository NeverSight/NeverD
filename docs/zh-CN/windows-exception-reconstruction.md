**语言**: [English](../windows-exception-reconstruction.md) | [简体中文](windows-exception-reconstruction.md) | [繁體中文](../zh-TW/windows-exception-reconstruction.md) | [日本語](../ja/windows-exception-reconstruction.md) | [한국어](../ko/windows-exception-reconstruction.md) | [Français](../fr/windows-exception-reconstruction.md) | [Deutsch](../de/windows-exception-reconstruction.md) | [Español](../es/windows-exception-reconstruction.md) | [Italiano](../it/windows-exception-reconstruction.md) | [Русский](../ru/windows-exception-reconstruction.md) | [العربية](../ar/windows-exception-reconstruction.md)

# Windows 异常重建

[← 文档索引](README.md)

NeverD 在加载、提升、反编译和二进制重写的全过程中携带 Windows 异常信息。
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
| x86 registration-chain SEH3 | 经检查的 scope 图、实际 FS:[0] 操作、callback root 与基于 CFG 的 try-level 状态集合 | 可规约且无歧义的区域生成显式 EH 节点；其他状态保留原生注释 | 对下文固定栈帧、caller-cleanup 的已证明子集支持原生 PE32 重建 |
| x86 registration-chain SEH4 | 经检查的 cookie 表达式、编码 scope 指针与基于 CFG 的状态流 | 可规约区域生成结构化 EH；其他形状保留无损注释 | 对下文已认证的直接栈帧子集支持原生 PE32 重建，包含 EH/GS cookie 初始化 |
| x86 registration-chain C++ EH | 绝对指针 FuncInfo、cleanup/对象契约与基于 CFG 的状态流 | 可规约区域生成结构化 EH；其他形状保留无损注释 | 对下文经过证明的单 try、标量 catch 子集执行原生 PE32 重建 |

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

### x86 registration 状态

x86 没有 runtime-function directory。loader 从 registration prologue 找到 EH3/EH4
scope table 或 C++ FuncInfo。SafeSEH 表必须已映射、严格排序，且目标可执行；损坏的表
不能降级成“没有表”。C++ 各张表共用一个累计解码预算。

`analyzeRegistrationStates` 是 LowIR 中 try-level 数据流的唯一所有者。它沿 CFG
前驱与回边传播状态，在汇合点保留所有到达的 level，并仅在已解码的状态写入指令完成后
应用新状态。loader 保留字节、字和双字写入的宽度；窄写入仅替换每个到达状态的对应
低位。未知高位、非法结果或与提升后宽度不符都会使证明失败。不可达代码中的写入不能
改变可达块的状态。运行时调用的 filter 与 cleanup
callback 不属于父函数的词法保护区；catch/except 入口使用对应的运行时状态迁移。
MedIR 单独携带这些推导结果，不修改 patch 事务认证的 loader 描述符。只有所有到达状态
对区域归属一致时，HighIR 才生成结构化区域；未知迁移保留注释，不虚构 IP-to-state map。
C++ catch 使用独立的运行时上下文：嵌套异常从活动 try 之上的状态开始搜索，catch
返回的是交给运行时的 continuation 代码地址。LowIR 要求精确解码的返回指令与已证明
的保存栈值，在同一函数内解码目标并重放恢复后的上下文。无法证明或相互冲突的
continuation 保留注释，同时撤销原生重建权限。这些推导结果与源 FuncInfo 及父函数的
标量返回值分别保留。
只有 FuncInfo 解析器认证过的 callback 指针字段才会被排除出普通间接入口发现。
同一目标的其他引用仍保留独立普通入口身份。MedIR 为仅由运行时进入的 catch 和 cleanup
恢复父帧 EBP，将其私有 callback ESP 保留为未知值；经过检查的 continuation 则使用
保存的父帧 ESP。
对于直接使用 EBP 的帧，catch 可以改写 SavedESP；运行时返回时仍会把派发前捕获的
指针写回该单元，再恢复 ESP。LowIR 与 MedIR 将这个隐式内存效果绑定到准确的
catch RETURN，HighIR 与原生 LLVM 显式生成写回。PE 安装器重新分析原始代码，
核对写入地址、值和顺序。未知的派发前快照不能靠 catch 写入补造。
对于已经验证的重新对齐帧，LowIR 分别跟踪回调私有栈、已初始化的保存单元和入口 EBP。
非嵌套 catch 必须平衡自己的 ESP 并恢复运行时 EBP，才能建立 continuation。
经检查的调用可以借用已初始化的父帧对象，其边界取自派发前的栈快照；保存入口 EBP
的管理单元不能作为借用对象。MedIR 保留独立的运行时入口定义；HighIR 与 LLVM
从原始入口 ESP 表达父帧对齐关系，不将其误当成固定栈偏移。SSA 构造前仅根据
精确匹配的 no-return 调用证明移除普通落入边，保留异常入口。HighIR 在转移到已检查的
continuation 前，使用同一对齐坐标写回捕获的 SavedESP，同时保留 handler 与
continuation 注释；这些对齐帧的完整结构化回调降级及原生再次重建仍未支持。
栈偏移证明、HighIR 与 LLVM 共享同一坐标。非法根操作或相互冲突的
保存栈值不能获得这些保证。
结构化 catch 保留到已检查 continuation 的显式转移；回退注释保留 catch-object 偏移、
parent-frame 偏移与 continuation 列表，同时让原生 handler 保持独立。
当同一 try 的已检查地址区间包围独立 catch 时，只有移出仅由运行时进入的 callback 后
能得到连续保护区，HighIR 才将其合并。普通前驱、夹在其间的未受保护语句或不明确的
状态流都会阻止移动；保护区与 catch 作为一个事务提交。单独转换的 PE32 callback
仍需证明父帧坐标投影后才能嵌入 clause；证明缺失时保留原生目标地址。
直接 MSVC prologue 必须证明实际 FS:[0] 写入与 registration/state 字段的位置；普通局部
变量中恰好相同的整数序列不能替代这一证据。

SEH3/EH4 原生重建还会证明固定且私有的源栈帧、平衡的 FS:[0] 操作，以及每个普通 CFG
block 唯一的活动状态。LLVM 拥有新的物理 registration；独立 filter 与 termination
callback 通过 escaped frame 恢复源 EBP/ESP 和 exception pointer。只有经过认证的源
链操作才会被替换。每个活动区间都有显式异步 scope 边界，包括内层 handler 进入外层
保护区的入口。原函数和保留的直接被调函数必须证明 caller-cleanup 栈行为；间接调用
或无法证明的栈清理约定仍被拒绝。

cdecl 的传入栈槽在原内存操作发生处映射到真实调用者栈帧，独立 callback 中的读写也
使用这一映射。保留 callee 的整个调用图必须证明栈帧私有：读取已初始化的栈字节，
访问限于当前已分配区域，并恢复 callee-saved 寄存器和 SP。flags、vector 别名、spill
和调用不会清洗栈帧来源。返回地址探针可以记录真实生成调用点供外部观察，但源函数或
保留调用图内不能重新读取该值。除经过认证的 `RaiseException` 外，未知 import 和没有
已检查内存访问契约的 intrinsic 不属于这一原生子集。

编译器为 scope 行发射索引、精确表范围、外层状态、filter/handler 目标及源语义凭据。
PE 事务检查实际字节和 DIR32 fixup，合并 SafeSEH 与 HIGHLOW relocation，并重新解析
最终映像。新建的 Guard CF/EH continuation 表指针也会获得独立的 base relocation。
注册链函数在 `section` 与 `inplace` 模式下都使用这个完整事务。

SEH3 要求保留参数的已知 CRT import veneer。EH4 要求精确的转发 wrapper：四个
dispatcher 参数、load-config cookie 地址、可执行 cookie checker 和 CRT common
handler import 必须一致；仅凭 handler 名称不能授权重写。共享栈帧分析在 registration
公开前证明编码 scope 指针与每个 cookie 表达式。源函数及保留的 callee 不能修改映像
cookie 或 scope table；合成 cookie 值不能逸出私有栈帧。LLVM 按实际 registration
record 推导生成 cookie 偏移，包括运行时的虚拟帧基址；安装器再与实际表字节逐项核对。
直接初始化的 GS 栈槽由编译器生成 stack protector。GS 编码与退出校验使用同一虚拟
基址，栈重新对齐时也保持一致。校验函数必须保留精确的 fastcall ABI 和原 wrapper
使用的代码地址；同名函数或 import 不能替代这一身份。
校验函数的经检查成功路径只将 ECX 与 load-config cookie 比较并返回，不修改栈存储
或其他寄存器。LLVM 重新对齐栈时，回调引用的参数副本仍保留在可恢复的局部帧中。

显式源 GS 校验要求在精确解码的调用发生处证明完整位宽的 ECX cookie 表达式，并认证
校验函数身份。原生 lowering 只将该已证明的调用替换为带索引的执行事件，LLVM 再发射
物理 stack protector 校验。公开安装器独立重放源证明，拒绝删除、复制、去标记或改变
顺序的事件。独立 callback 内的源校验仍不属于这一子集。

该路径要求 LLVM fork 提供 `LLVM_NEVERD_X86_REGISTRATION_EH` 契约；旧的已发布 r3
预编译包会拒绝原生安装。EH4 还要求 `LLVM_NEVERD_X86_REGISTRATION_COOKIES`，
GS 初始化还要求 `LLVM_NEVERD_X86_REGISTRATION_GS`。

x86 C++ 原生重建目前支持一个同步 typed try 和一个按值或引用捕获的标量 catch，
源 unwind state 最多 128 个。输入必须具有经过证明的 MSVC 直接 registration frame，
采用 magic `0x19930522` 的 FuncInfo，且不带 GS wrapper。保留的每个调用、throw type
和 cleanup relay 都需要独立 ABI 证明。源对象借用必须有界、已经初始化且不与注册
存储重叠；引用访问必须在 catch 返回前保留 CRT 提供的对象身份。所有读写保持原始
映像存储身份。

LLVM 重新发射物理 registration、typed catch home、有序 cleanup dispatch、完整
FuncInfo 和私有 handler。公开安装要求编译器同时提供
`LLVM_NEVERD_X86_CXX_CATCH_SUBFIELDS`、`LLVM_NEVERD_X86_CXX_FUNCTION_RECEIPTS`
与 `LLVM_NEVERD_X86_CXX_HANDLER_RECEIPTS`，随后独立重放编辑后的 IR，并检查实际
机器码、语言表、SafeSEH 和全部绝对重定位。入口 patch 不得覆盖保留的 helper 或 CRT
指令。当前运行样本在 Wine 和 Windows CRT 下证明整数按值/引用捕获、嵌套析构和强制
重定位。其他 try/catch 图、未经证明的对象类型、传入栈参数、动态帧以及 GS 或异步
C++ 仍保留分析信息，并拒绝原生安装。

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
- 当前 schema version：`9`。

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

registration 状态与 PE32 运行基线可单独验证：

```bash
cmake --build build-release --target NeverDRegistrationStateTests \
  NeverDRegistrationEHTests NeverDWindowsRegistrationFrameTests --parallel 4
build-release/bin/NeverDRegistrationStateTests
build-release/bin/NeverDRegistrationEHTests
NEVERD_REGISTRATION_RUNTIME_OBJECT=/tmp/neverd-frame.obj \
  build-release/bin/NeverDWindowsRegistrationFrameTests
python3 -m unittest scripts.tests.test_check_windows_registration_eh \
  scripts.tests.test_check_windows_registration_frame \
  scripts.tests.test_check_windows_registration_cookie -v
python3 scripts/check_windows_registration_eh.py --output build-registration/evidence
python3 scripts/check_windows_registration_frame.py --object /tmp/neverd-frame.obj \
  --output build-registration/callback-runtime
```

执行器运行固定版本的 MSVC x86 SEH/C++ 样本，覆盖 `/GS` 开关与 O0/O2；Linux 使用 Wine，
Windows 使用原生 loader。默认报告标记为 `original-runtime`。传入 `--patched-root` 后，
必须提供全部八份重写样本；字节完全相同的副本会被拒绝，并逐个比较运行结果。只有这一
模式报告 `changed-image-runtime`，证明修改后的映像运行结果一致。原生重建还需绑定
源文件、输出文件和替换入口的 patch 回执，以及替换入口实际执行的证据。缺少运行环境
或样本会失败。回调帧测试覆盖 PE32 filter/finally 的父帧恢复、既有 escape 索引、
有界异常指针槽、独立回调栈、原子拒绝，以及实际 i386 COFF scope 表的代码生成；
帧执行器保留链接时的 SafeSEH 检查，执行 16 次真实异常，并要求 filter 调用次数和处理结果
完全符合预期；报告标记为 `generated-x86-callback-abi`。这些检查本身不授权原生 patch。
源码重建 runner 在首选及强制重定位基址执行原始、手动安装、公开 COFF、符号冲突、
CLI section 和 CLI inplace 六种映像。严格 EH4 oracle 还要求正常执行，以及分别损坏
EH 和 GS cookie 后在 dispatch 前拒绝。Wine 的 common EH4 dispatcher 不检查 cookie，
因此 fixture 会先完成该检查，再由真实 runtime 分派异常。
CI 的 `windows_eh_only` 手动配置还验证 ARM32 交叉目标 PE 生成与重建；这不等于在
Windows ARM32 上执行。提供精确 LLVM artifact build 后，下游 Windows job 会用原生
Windows CRT 复跑同一批经哈希核对的 PE32 映像。

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
