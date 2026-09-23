**语言**: [English](../architecture.md) | [简体中文](architecture.md) | [繁體中文](../zh-TW/architecture.md) | [日本語](../ja/architecture.md) | [한국어](../ko/architecture.md) | [Français](../fr/architecture.md) | [Deutsch](../de/architecture.md) | [Español](../es/architecture.md) | [Italiano](../it/architecture.md) | [Русский](../ru/architecture.md) | [العربية](../ar/architecture.md)

[← 文档索引](README.md)

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

HighIR 的 `HighSourceFlow` 统一管理输出语句的控制流边、局部变量身份和确定赋值分析。
源码验证与无用 PHI 复制消除共享这张图。有界划分跟踪局部标量之间及与零的重复等值比较；
交换操作数和保持位宽的整数视图共享条件事实，写入任一操作数都会使其失效。
地址逃逸的变量保持未知，位宽不一致时拒绝推断，划分超限时退回保守控制流图。
仅当标量值在所有可行上下文中都不会被使用时，才删除 PHI 复制。
调用、加载和存储保留可观察行为，删除复制也保留源码标签。分析之前，HighIR 会把同一局部变量
的相邻字节切片简化为连续切片，并保留结果类型；这一恒等变换不会合并独立的加载或调用。

HighIR 可以在严格证明下把 64 位源码局部变量缩窄为 32 位，沿用 128 位载体的规则：所有定义的载体宽度和低位宽度必须一致，所有读取必须显式选择低位。完整宽度存储、逃逸、高位读取或高位表达式的副作用都会阻止缩窄。源码参数的填充位仍保持未知。
精确的整变量复制可以通过有界图共享这一证明，但必须先由构造值的定义确定低位宽度。每个复制目标都必须满足缩窄条件；完整宽度的消费者会使所有上游豁免失效。没有宽度依据的循环、宽度冲突或预算耗尽时保留原值。
有界整数转换、零偏移切片和扩展也可传递这一证明，但每个中间宽度都必须保留所需低位。每次使用都按其所属语句的表达式根分别检查。最多两轮有界分析可在移除向量填充后继续证明更窄的低位。

当后续常量掩码完全不会观察完整低位操作数以上的任何位时，HighIR 还可以从重建整数中丢弃
未定义高位。低位可以来自直接 `CONCAT`、有界整数转换或零偏移 `SUBBYTES` 视图；替换结果
使用显式零扩展，并保留原掩码。高位表达式必须有界且可安全丢弃；调用、加载、有序访问、可能陷阱、
畸形位宽以及触及任一高位的掩码都会保留原未知值。

Block 使用方的逃逸分析也使用这张图，以有界不动点分析跨分支和循环传播指针身份及私有栈槽信息。合流保留可能的上下文地址，只有完整覆盖才能清除。未知跳转、异常控制流和证明预算耗尽都会拒绝绑定。

精确的零偏移 `SUBBYTES` 视图可以从更宽的物理寄存器视图中保留完整低位指针字。栈帧、context 与 invoke 指针的部分视图会保留来源身份及字节区间；只有同源、连续、顺序正确并覆盖完整指针的 `CONCAT` 才能恢复地址。其他部分视图继续携带污点，一旦进入内存、调用、控制流、存储或可观察返回就会被拒绝。位模式相等的普通标量绝不会取得指针身份。
当精确源码 ABI 声明窄标量返回值时，显式 `CONCAT` 可以提供未定义的物理返回高位填充。逃逸分析会完整检查声明的低位结果，只忽略这种结构化高位填充；任何可观察返回字节中的 context 指针仍会被拒绝。

Block invoke 只有在自己的新建栈帧中尚未保存 context、invoke、ISA 或其他已证明的指针身份时，才能把该栈帧中的地址传给已绑定调用。一旦保存了任一此类身份，无边界的栈地址参数会再次被视为逃逸。这样可以区分错误结果槽等普通回调局部变量与 Block context，同时不放松 context 逃逸证明。

栈 Block 构造在把调用解释为 Block 使用方之前，会区分精确的 literal 基址与其他栈参数。因此普通栈数组可以在构造前传给调用；宽栈初始化会保留为毒化的字节覆盖，不能用来满足 Block 头或具有所有权的捕获字段。一旦 ISA 身份仍然存活，其他栈参数会继续被拒绝，直到已声明的复制型或非逃逸使用方使 literal 存储失效。这样既允许之后不相关的栈调用，又不会忘记任何到达路径上仍然存在的 Block 身份。

无捕获的全局 Block literal 可以共享同一个编译器 descriptor。源码依赖和生成 helper 只跟随当前源码闭包实际引用的 literal；共享该 descriptor 但未被引用的 literal，不会增加 invoke 依赖，也不会进入生成存储。descriptor 仍然共享，同一个原始 literal 在不同方法中继续使用同一个共享生成身份。

Darwin Block 使用方由加载器统一拥有回调和生命周期契约。生成的目录区分编译器声明的 `noescape` 参数与已审计的运行时复制型使用方。后者目前仅包括 `dispatch_async` 和 `dispatch_barrier_async`，其 SDK 契约规定复制并释放 Block。两类契约都要求导入及提供者身份精确、四种编译器配置一致，并且完整回调 ABI 匹配。源码发布仍会证明栈 Block 头、已初始化捕获、复制/析构 helper 和 invoke 依赖。两类使用方在调用后都会使调用方的构造事实失效；复制型使用方绝不会被报告为非逃逸。未标注的 Block 参数不会授予任何生命周期权限。

Objective-C SDK 的 `noescape` Block 参数使用同一个加载器边界。只有当前消息仍具有精确的 SDK 父方法 ABI 与参数位置、Block descriptor 具有精确的编译器回调 ABI，并且任何已限定接收者都能通过完整且无冲突的类层级到达声明所有者时，目录条目才会生效。未限定选择器只有在所有匹配条目对同一个回调契约达成一致时才能使用。该证明只覆盖调用方生命周期：动态派发保持不变，不选择任何实现地址；没有进入目录的 `noescape` 声明仍会拒绝栈 Block 逃逸。

源代码调用发现以有界前向不动点传播寄存器事实。普通控制流合流仅保留所有已到达前驱一致的选择器、导入槽或数字地址。
独立入口和异常入口从未知状态开始；循环回边收敛后才发布调用绑定。物理寄存器别名写入、未知调用和指令内临时量
不会把过期事实带到后续基本块。非法边或计算预算耗尽会拒绝这项证明。

Objective-C 的全局选择器查找通常要求完整的本地方法、协议、属性和已启用 SDK 声明全部一致。
当完整声明仅在返回合同上冲突时，调用后对标量返回寄存器的精确读取可以缩小候选，但必须只有一个
声明的返回载体能定义该观察。整数观察可以读取完全已定义的子区间，包括带明确扩展规则的窄结果；
浮点观察必须与声明的寄存器、偏移和宽度完全一致，不能用四字节读取把声明的 double 位模式解释成
float。完整宽度复制可以传递该证据。已认证的运行时调用只能保留 ABI 明确保护的别名，包括
AArch64 `Q8`–`Q15` 的低 64 位前缀。未知调用、重叠写入、未使用结果、不完整声明或多个匹配载体
都会保持未解析。选中的载体范围随源码调用提示保存，并在发布时依据当前镜像重新验证，因此局部
数据流观察不能绕过全局声明检查。

必需的 Swift 值见证操作使用独立且不依赖符号的调用证明。有界反向追踪必须证明间接目标取自
该操作规定的 `metadata[-1][slot]` 表项，并且同一 metadata 值位于其规范 Swift 参数载体。
当前支持 `destroy` 和 `initializeWithCopy`。所有前驱都必须一致；损坏的 CFG 边、部分写入、
有序加载、调用破坏或预算耗尽都会拒绝绑定。生成的 C 通过实时 metadata 重新读取该表项，
不会保留被分析镜像中的见证函数地址。

编译器生成的 Swift 协议见证访问器使用另一套精确证明。loader 将 `Wl` 代码符号与匹配的可写、
零初始化 `WL` 缓存配对，然后验证缓存加载、以精确一致性描述符和类型 metadata 对象调用
`swift_getWitnessTable`、release store 以及两条返回路径。调用点取得零参数、指针结果的 callee
合同，但不会改变访问器自身由机器事实推断的入口签名。生成的 C 重建新的共享缓存并重新执行
运行时查询；不会发布已捕获进程中的缓存或见证指针。

标准 Swift metadata 存储地址由编译器生成的 `.self` 查询证明；标准 Hashable 见证存储则由
编译器生成的受约束泛型调用中的直接见证参数证明。两者都要求 ARM64/x86-64 macOS 与
Mac Catalyst 的 SDK 导出一致。`Any.self` 是唯一支持的完整存在类型例外：编译器 IR 必须
返回外部导出 `%swift.full_existential_type` 中偏移恰好为八字节的内部 metadata 成员，目录
记录并重建其所属的 `$sypN` 存储基址。这不会授权其他内部指针，也不会从修饰名推断完整存在
类型布局。只有直接、外部、非 TLS 全局量符合条件；加载器会先认证符号和提供方，再绑定其
运行时地址。这些证据既不提供 metadata/见证布局，也不提供访问器、见证成员或任意修饰符号的
调用 ABI。

Objective-C 属性元数据提供独立于方法实现的访问器声明，支持动态属性、只读属性和自定义访问器。
加载器验证类、分类或协议记录的布局，再通过共享编码解析器和 Darwin ABI 层生成标量或指针签名。
调用绑定要求所有匹配的属性、方法、协议及已启用 SDK 声明一致；属性记录不会创建原生函数，
也不会增加运行时方法总数。64 位记录布局依据
[Apple 运行时 ABI](https://github.com/apple-oss-distributions/objc4/blob/main/runtime/objc-runtime-new.h)，
分类中可选的类属性字段还需要
[镜像布局标志](https://github.com/apple-oss-distributions/objc4/blob/main/runtime/objc-abi.h) 才能读取。
不支持的属性编码和损坏的属性列表会保留明确诊断。

Objective-C 接收对象事实区分方法入口的 self 与确定的类引用；只有共用入口的所有元数据记录一致时，才建立 self 类型事实。完整宽度复制和 ABI 规定保留的寄存器通过同一不动点分析传播事实，入口回边也参与合流。声明检查区分类方法与实例方法，并纳入已记录的分类、父类链和协议继承；入口 self 还需考虑已知子类声明。编译器目录分别保留声明所属对象和继承关系，以及全局选择器一致性信息。发布源码前，SDK 会依据当前镜像重新验证接收对象的来源及适用声明。这些事实不会选择具体 IMP，也不授权二进制重写。
编译器共享的 native thunk 可能接收已验证 Objective-C 类引用槽的地址，而不是槽中保存的类对象。
源码恢复会为每个原始槽重建独立单元，并通过 `objc_getClass` 或 `objc_getMetaClass` 初始化，从而
保留这一层额外间接关系。只有精确匹配且具有完整类型的 native 依赖证明该参数仅发生完整宽度
读取，且没有写入、偏移、逃逸或不支持的内存效果时，才能建立绑定。裸槽地址仍不能充当消息
接收者，也不能取得类对象身份。
外部继承信息缺失时，必须改用全局选择器一致性检查，不能缩小到接收对象范围；明确不支持或冲突的声明仍保留为否定证据。

带明确对象类型的成员变量可通过最多八次完整宽度读取延伸接收对象证明。每一步记录运行时偏移槽、偏移值宽度，以及机器访问中使用的固定字节偏移。固定偏移必须仍与当前布局一致；运行时偏移引用则可以跟随字段移动。加载器核对已记录的类继承链和具体字段声明。部分读取、裸 id、Block、仅协议类型、歧义存储和未知指针基址都不能产生类事实；源码验证会依据当前镜像重新检查整条路径。这些事实描述声明类型，不代表对象身份，也不允许删除内存操作。

当控制流从多个已认证的 Objective-C 成员变量偏移槽中选择时，源码绑定可以在各前驱边上取得运行时偏移值，再合流这些值，而不是合流槽地址。所有可达叶节点必须属于同一类并使用相同载体宽度；地址链只能包含同宽局部变量或 SSA 别名，合流后的地址也只能供一次完整宽度加载使用。类或宽度不一致、算术、写入、逃逸以及额外加载都会保留未解析地址诊断。

共享 Swift once getter 只有满足一种精确的四载体契约时，才能公开 `String` 的两个机器字：谓词、初始化器和两个顺序存储字。getter 必须恰好进行一次已认证的 `swift_once` 调用，把两个未修改的字传给精确的 Swift `String` 到 `NSString` 桥接，并且每个地址参数只能通过纯同宽别名和已证明的加载使用。调用方必须传入同一具名、可写 16 字节对象中相邻的两个字。回调及其全部依赖仍必须按普通源码闭包完成；存储证明不能授权跳过初始化器或臆造内容。

编译器生成的零参数 Swift 延迟全局 addressor 只有在精确的 `vau`/`vpZ`/`_Wz`/`_WZ` 符号族与一种规范结构完全一致时才会重建：一次加载、一次完成状态检查、一次已认证的 `swift_once` 调用，以及两条路径返回同一存储地址。初始化器必须忽略附带的 context，并像普通源码一样完成依赖闭包。投影会新建共享 once 谓词和数值槽，不保留已加载镜像中的谓词、数值或初始化器地址。它的零参数源码被调用方 ABI 只应用于调用点；原生入口 ABI 保持独立，使附带的 context 载体仍可用于契约证明。

已认证的 `dispatch_once_f` 调用也可以重建其谓词与本地回调地址。加载器必须证明精确的 libdispatch 导出、完整的可写谓词单元、没有普通直接调用方的唯一代码目标，以及公开的 `void (*)(void *)` 回调 ABI。回调会按该契约重新提升，并继续作为普通源码依赖。生成单元使用新的共享谓词存储和已恢复的回调定义，不保留两个原始镜像地址。未知提供者、未经证明的存储、非代码目标和冲突调用 ABI 均保持未绑定。

如果该初始化器调用编译器生成的导入 Objective-C 类元数据访问器，投影只接受零值缓存、类引用、`objc_opt_self`、`swift_getObjCClassMetadata` 和 release 发布完全吻合的精确模板。投影直接发出已认证的运行时查询，不保留镜像缓存。

Swift 具体类型元数据的缓存/引用对只有在零值缓存、不可变元数据引用记录、每个相对描述符槽和精确的 `MR`/`Md` 符号拼写全部一致时，才能重建其重整类型引用。类型引用仅由可打印重整字节和最多八个间接 `0x02` 上下文描述符组成；展开全部描述符后必须精确还原完整符号名，而且该符号名本身必须通过有界 Swift 类型反重整。导入的名义类型描述符需要有界反重整，并且提供者必须是由所声明模块推导出的精确系统框架安装名。本地协议以及有界的模块/类/结构体/枚举名义路径必须通过已解析的只读重定位指向唯一数据符号，并且恰好存在一个匹配导出。直接 `0x01` 或其他符号引用、未导出的私有上下文、弱提供者或不匹配的提供者、畸形记录以及歧义符号仍不受支持。

精确的具名存储参数证明可以沿直接原生调用链传播，但每条边都必须具有完整且一致的源码 ABI，并且只能原样转发同一个指针参数，不能进行算术。末端被调用方仍只能用该参数进行有界、完整宽度访问。普通加载和存储可以接受；可写缓存还可以使用一次精确的 release 存储，因为生成的依赖会保留该原子顺序。只读类引用槽仍拒绝所有写入。间接或 ABI 不匹配的调用、其他原子顺序、逃逸、循环、深度或证据预算耗尽都会失败关闭。

加载器验证 Darwin 常量字符串、整数对象、数组和有序字典组成的有界无环图。容器字段和每条边都需要不可变映射存储，以及无歧义的导入或重定位证据；不支持的编码、循环和不完整对象图会明确失败。源码绑定重新验证对象图及传入指针槽。生成的辅助函数保留整数位模式、子对象顺序和共享地址，并复用已有字符串身份。容器槽通过 acquire/release 发布机制只初始化一次，初始化仅调用已验证的子对象辅助函数。每个辅助函数自带子函数声明，使独立恢复的方法可以共用同一定义。可移植测试覆盖损坏输入和证明预算；原生编译器样例对照原始方法检查内容、别名、复制身份和并发初始化。

## IR 表示与路径

| 表示 | 用途 | 主要定义与转换 |
|------|------|----------------|
| LowIR | 架构无关的 `NdOp` 操作、基本块、CFG 和跳转表元数据 | `include/neverd/ir/low`、`lib/ir/low`，由 `lib/decode` + `lib/lift` 生成 |
| MedIR | 类型、ABI/调用约定、内存与栈模型、标志、调用和类 SSA 数据流 | `include/neverd/ir/med`、`lib/ir/med` |
| HighIR | 用于可读 C 的结构化表达式与控制流 | `include/neverd/ir/high`、`lib/ir/high`，由 `lib/backend/c/HighC` 发射 |
| LLVM IR | 优化、LLVM 派生 C、目标代码生成和二进制重写输入 | `lib/backend/llvm`，由 `lib/pipeline` 优化/编排 |

常量在 LowIR、MedIR 和 HighIR 中保留每次出现时的标量/地址来源及地址归属。数值位相同不会合并不同来源。HighIR 符号简化将地址身份作为不透明输入；源码绑定使用共享的数值操作数分类，并仍要求内存和指针使用完成重定位绑定。

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
| `lib/sdk` | 公共 C ABI、session 生命周期、查询、持久化、插件、lift/decompile/patch/audit/hunt 入口 | 将引擎组件聚合为 `libneverd` |
| `lib/pass` | LLVM IR 混淆 pass 与 MIR pass runner | IR |
| `lib/debug` | DWARF、PDB 和 linker-map 调试上下文 | IR |
| `lib/sigs` | 签名解析、数据库与匹配 | Loader |
| `lib/libc` | 已知 libc 名称与调用模型支持 | 独立组件 |
| `lib/safety` | 提升 IR 上的堆生命周期审计与拷贝越界猎取 | Symbolic、Solver |
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
[`PatchFormatTests.cpp`](../../unittests/lift/format/PatchFormatTests.cpp)，用于 Windows ARM
加载/反编译的
[`COFFARMFormatTests.cpp`](../../unittests/lift/format/COFFARMFormatTests.cpp)，用于 i386 thin
对象的
[`MachOI386RelocationTests.cpp`](../../unittests/lift/format/MachOI386RelocationTests.cpp)，
用于已链接 Mach-O 的
[`X86_64_PipelineE2ETests.cpp`](../../unittests/lift/x86_64/X86_64_PipelineE2ETests.cpp) 与
[`AArch64_PipelineE2ETests.cpp`](../../unittests/lift/aarch64/AArch64_PipelineE2ETests.cpp)，
以及覆盖 12 单元后端网格的
[`PatchFullSubstRTTests.cpp`](../../unittests/semantic/probe/patchfull/PatchFullSubstRTTests.cpp)。
命令见[测试指南](testing.md)。

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
| 修改堆生命周期审计或拷贝越界猎取 | `lib/safety`、`include/neverd/safety`、`include/neverd/sdk/NeverDCAPISafety.h` | `NeverDSafetyTests` 与 `NeverDSafetyIntegrationTests` |
| 添加语义回归 | 聚焦的 `unittests/semantic/*Tests.cpp`；在 `unittests/semantic/CMakeLists.txt` 注册新文件 | 构建其测试二进制，再用 `ctest -R` 选择命名用例 |

保持修改范围精确。定义某种表示的文件可以与其转换一起变化，但不要仅为让大型
重构看起来统一而修改无关的 loader、lifter 和 backend。

源码记录类型独立保存字段布局。统一 ABI 层支持 Darwin ARM64 中含一至四个同类 float 或 double 成员的嵌套结构体，并在浮点寄存器不足时将整个参数放到栈上。MedIR 在 SSA 前绑定每个物理成员，HighIR 保留单个逻辑参数或返回值，C 输出校验布局。Darwin ARM64 和 x86_64 还支持由一至两个 64 位整数或指针组成的记录，包括嵌套布局。整个记录溢出到栈上时，ARM64 耗尽该寄存器组，x86_64 则保留剩余寄存器给后续参数。有填充、压缩字段、混合浮点与整数及不完整分量仍明确拒绝；这些提示不能授权二进制改写。

Darwin ARM64 的固定 C 调用还支持通过隐藏的 x8 指针返回由三个有符号 64 位整数构成的自然布局记录。共享源码 ABI 层负责分类；Low→Med 在调用前保存该指针，将一个逻辑记录结果逐字段写回调用者存储区，普通参数寄存器保持原位，x0 不被视为返回值。调用提示分析会使结果存储区中的旧事实失效。入口投影和原生状态保存证明尚无对应存储证明，继续拒绝此类返回；含无符号或指针字段的三字记录、三字参数和 x86_64 间接记录返回仍不支持。 Objective-C 间接返回在全局选择器查询和普通接收者查询中仍会拒绝，因为 nil 消息分发保留原结果缓冲区。仅当接收者精确等于当前方法的非空 self，且 x8 指向完整、未逃逸的私有栈帧范围时，按接收者限定的 ARM64 调用才可使用固定记录 ABI。发布源码时会重新验证方法入口、self 实参、接收者声明、记录大小和栈帧边界；证据缺失或变化时，消息仍保持未解析。

运行时调用目录仅在准确导入的函数明确返回原参数指针时声明 `ReturnedArgument`。接收者分析先读取已声明的物理参数，再执行正常 ABI 寄存器清除，最后仅在返回值上恢复已有的接收者类型事实。SDK 会重新验证此效果；它不允许删除调用、所有权效果或内存访问。

编译器生成的框架目录与接收者目录共享提供方列表：Foundation、CoreData、CoreLocation、CoreSpotlight、QuartzCore、UniformTypeIdentifiers 和 UserNotifications。QuartzCore 使用公共入口 `CoreAnimation.h`，其他框架的兼容性导入不提供所属声明。两个生成器均保留四种预处理配置、准确框架身份及声明的否定证据。

对象返回类型通过一致的方法声明扩展同一条有界接收者证明。具名对象返回类型和编译器声明的关联返回类型可以提供类事实；单独的 id 不可以。字段读取和消息返回共用八步预算，源码验证会根据当前声明重新检查每一步。精确绑定的分配辅助函数使用对应消息的返回类型契约，保留调用、自定义重写和所有权效果。返回类冲突或接收者继承关系不完整时停止传播。

格式调用绑定保留所属语言规则。NSString 属性和公开谓词入口均核对 SDK 声明及所有运行时替代声明。谓词不会替换单引号或双引号中的占位符，`%K` 则接收属性名对象。精确匹配的 Darwin `snprintf` 导入只有在格式串是唯一映射、不可变、无 fixup 且以 NUL 结尾的 C 字符串时，才使用其公开的固定参数原型；它的 `printf` 语法只接受受支持并完成默认提升的标量转换，拒绝 Objective-C 转换、`%n`、long double 和未支持的宽字符串形式。实参沿用统一的标量提升和 Darwin 可变参数 ABI。不支持的转义及格式修饰符明确拒绝。源码发布前重新校验语法、常量身份及参数证明，生成代码仍调用原框架或 C 运行时解析器。

精确的 selector 专用 Objective-C stub 还可以在恢复出的调用恰好止于声明的固定参数前缀，或每个可变实参都具有完整源码指针类型，或其完整定义链终止于已认证的 Objective-C 运行时指针返回调用时，绑定动态格式对象。空尾部不依赖格式内容；已证明的纯指针尾部对任意运行时格式都保留相同的提升后指针载体。生成代码把原格式对象和实参交给框架解析器，不推断转换。受限指针与整数契约之外的尾部、不精确的 stub、声明冲突及物理 ABI 不匹配仍保持未解析状态。

编译器声明的固定参数 C 导入函数与 Objective-C 消息共用受支持结构体的源 ABI 分配规则。只有显式声明的参数占用载体；Objective-C 层提供隐藏的接收者与选择器参数。仍须严格匹配 SDK 导出身份与签名。仅支持标量的回调和可变参数保留现有限制。

源函数声明将调用约定保留为签名身份的一部分。共享 ABI 层支持有明确边界的 Swift 调用，可承载 1、2、4 或 8 字节整数参数及指针参数：先分配整数寄存器组，再分配相对入口 SP 的栈载体。每个窄载体都记录精确的扩展规则，结果仍限制为最多两个整数或指针字。HighC 在声明和定义中保留 `swiftcall`。由编译器观测确认的 Foundation 值桥接还可以声明一个 `swift_indirect_result` 指针和一个 `swift_context` 指针：arm64 使用 x8/x20，x86_64 使用 RAX/R13，二者均不占用普通整数参数寄存器组。HighC 保留这两个参数属性。公开 Foundation 元数据导入必须在 ARM64/x86-64 的 macOS 和 Mac Catalyst 配置间取得编译器符号图、实际元数据查询 IR 与精确 SDK 导出的共同证明。仅凭重整符号的后缀不能确定 ABI。泛型或未声明的隐藏参数、Swift 回调类型及不支持的物理载体仍被拒绝。Mac Catalyst 声明不代表已有 iOS 设备执行验证。

MedIR 统一负责同宽 SSA 复制与完整 PHI（包括循环）的有界常量传播。所有入边值必须收敛到相同的位、宽度、来源和地址归属。未知定义、无初值循环、不完整边及冲突常量阻止替换；预算耗尽时函数保持原样。分析只替换操作数，保留调用、加载、存储及其副作用。HighIR 与 LLVM 使用同一分析结果。

单次操作内的代码归属索引也保存运行时元数据中主函数与代码片段的精确关系。索引查询与直接扫描共享同一关系遍历，保留主函数入口的原始地址，并拒绝孤立引用及指向非主函数的父引用。跳转目标验证、边界证明和临时分组分析使用同一个不可变索引及查询成本计算。其他镜像的索引回退到直接扫描；预算耗尽仍拒绝不完整证明。

源代码 ABI 显式记录 Darwin ARM64 和 x86_64 窄整数寄存器参数的 32 位符号扩展或零扩展。HighIR 在保存和复制参数时保留这些已知位，同时保持原始参数类型。超过 32 位的读取、栈填充和被调用破坏的寄存器仍为未知。这遵循 Apple 的 ARM64 和 Intel 调用约定；仅观察到原生低字节值不能证明扩展。

Mach-O 加载器保留段的“重定位后只读”保证，同时保留初始权限。源码字节与指针读取共用唯一文件映射检查；段名本身不能证明不可变性。普通全宽加载可以把已解析的本地数据指针绑定到独立验证的常量字符串对象。绑定保留来源槽以便重新验证，别名共用目标对象的生成身份。可写存储、冲突重定位、部分或有序加载以及槽本身的地址仍不受支持。

跳转表恢复只生成到普通后继块的转移，不再通过另一条路径重建块内语句。每个块只由统一流程转换一次，包括共享分支、默认目标和循环入口。分支边上的 PHI 赋值在对应转移前执行，并保留并行赋值快照；不完整的边绑定仍明确失败。

循环结构化保留原生入口的精确归属。恒真循环的包装节点保留首条循环体指令的标签，不重复创建入口。有条件的回边退出到原有后续语句，包括没有原生地址的边复制。只有精确匹配后续入口的跳转才转换为 break；嵌套循环和 switch 内的跳转保留其控制作用域。

死值消除在删除 PHI 边复制之前规范化原生入口的归属。共享的合并逻辑区分条件分支入口及其紧邻的合成边复制前缀；不连续的标签以及无关的嵌套标签仍判为存在歧义。

`scripts/collect_objc_sdk_declarations.py` 从真实设备、模拟器、Mac Catalyst 和桌面 SDK 配置中采集框架自有的类、协议、分类及方法 ABI 候选。每个配置保留公共头文件、导出和编译器证据，包括不受支持的声明及相关对象返回类型。部分 SDK 采集会记录准确范围；任一配置失败都会留下未完成的记录。CI 产物供后续声明目录一致性校验使用，本身不会启用新的源码调用绑定。

Objective-C 调用事实包含有界的、相对入口 SP 的私有栈槽。控制流汇合时，精确值取交集，可能源自栈帧的字节取并集，避免冲突路径和部分寄存器写入掩盖地址逃逸。具有明确 ABI 的调用仅保留已分配且位于传出参数之外的私有存储。地址逃逸、未知调用、重叠或原子写入以及栈空间释放会撤销相应证明。循环回边必须收敛后才能发布绑定。

HighC 使用精确位宽的 `_BitInt` 类型表示不超过 128 位的部分整数载体。普通内存辅助函数按 IR 字节数传输，不依赖 C 对象填充；无符号运算保留回绕和移位边界。不支持的部分位宽原子访问会明确拒绝，不会扩大访问范围。

MedIR 源码参数验证从声明的返回值、控制流、内存副作用和调用反向追踪所需字节。COPY、PHI、CONCAT、字节提取和扩展保持字节需求；其他操作保守地要求全部输入。未使用的浮点寄存器高位不会产生额外参数。可观察的高位、不完整的图和耗尽的分析预算仍保留原有拒绝结果。此分析不删除机器操作，也不授予重写 ABI。

条件结构化保留原有边上的落空路径 PHI 赋值。其来源地址不能成为新建的延续跳转目标；如果移动代码段需要这样的目标，共享延续路径就保留在原处。 无条件的合成循环在精确循环头之前没有任何操作时，也与体内首条原生指令共享延续入口；有条件的测试或前置副作用不具备这种等价性。

原生辅助函数的源码签名推断通过有界 CFG 分析，证明每条机器返回路径都具有完整的整数结果。共享出口汇合前驱事实，入口路径阻止未初始化的循环自证。调用和局部写入会撤销返回寄存器的证明，直到再次完整计算。畸形控制流、仅返回传入值的路径以及 x86-64 尾声恢复仍会被拒绝。这只产生候选源码签名；第二次流水线仍须验证函数体及其依赖闭包，不会改变重写 ABI。

对已绑定源码的运行时调用，Low→Med 降低阶段将已认证的外部不返回声明传入 MedIR 调用效果。运行时导入跳板也可能出现在原生函数清单中。只有经过验证的运行时绑定与完整调用操作数一致时，不返回固定点才保留已有的机器终止事实；仅有源码提示不能建立该事实。绑定指向导入槽，而调用指向跳板；推断出的原生效果仍从当前图重新计算，源码发布仍重新验证导入身份。

单一直线 ARM64 辅助函数在至多两个 Swift 运行时导入分别经过验证、且只有最后一个调用不返回时，可以保留未被覆盖且完整观测到的入口上下文。此前的返回调用沿用普通调用的寄存器破坏规则。同一套字节身份和栈帧逃逸证明检查此前的每个操作，并要求所有传出的标量栈参数占用已完整写入的私有字节。分支、返回、异常边和未知调用仍不支持。仅副作用的入口字节需求允许独立证明的终止控制流；默认的无用输入证明仍要求观测到返回。这只产生候选签名，源码函数体和依赖闭包仍须验证。

Swift 惰性全局地址访问器只有通过当前映像和流水线结果独立重建的契约，才能向原生状态恢复提供仅用于调用的 ABI。共享 once 验证器重新检查精确函数体、存储与初始化器身份、规范回调 ABI，并证明当前初始化器不使用上下文。已有 MedIR 或持久选项提示不能认证此契约。原生推断匹配精确直接目标和零参数指针 ABI，然后沿用完整的 Low/Med 调用对应与字节、栈帧恢复证明。调用保留普通寄存器破坏和初始化效果；此契约不声明只读或终止调用，也不闭合任何源码函数体或依赖。

普通 ARM64 调用只有在同一 LowIR 块中，已分配私有栈帧内的每个字节都已写入且不源自栈帧地址时，才能使用对齐的八字节标量栈参数。调用 ABI 与每处机器调用仍需独立验证。AAPCS64 允许被调函数改写传入参数区域，因此证明在调用后使整个传出参数区间（含填充）失效，再检查后续参数或恢复保存的寄存器。复用槽位必须重新完整写入。本次扩展不支持跨块定义、部分字、尾调用和 x64；所有普通寄存器破坏与退出恢复检查继续适用。

对于已链接的 Mach-O ARM64 代码，LowIR 可沿原始无条件 B 进入完整范围位于当前函数入口之前的共享尾块。该有界形状仅包含从 SP 恢复 x19–x30 的全宽 LDP、恰好一次对齐的正向栈释放，以及到已登记可执行导入的最终分支。系统针对该精确边验证唯一不可变字节、无重定位修正和无内部函数入口。CFG 的两个入口门控共享该边证据；BL、条件分支和顺序落入本身不能授权跨入口解码；尾块解码后保留所有真实 CFG 前驱。原始加载、栈更新和外部跳转保留在 LowIR 中，共享函数仍独立提升，源码恢复仍由现有帧证明决定。较早的共享块不会扩大主函数大小。

arm64 动态格式调用还可使用独立的 64 位整数尾参契约：每个到达定义必须最终来自当前已验证的 Objective-C 声明，并返回相同的完整整数类型。等宽整数转换保留载体；原始加载、未经证明的参数、常量、循环、浮点或窄中间值，以及有符号性冲突仍不支持。绑定前，完整原生 ABI 必须与共享 Darwin 可变参数布局一致；发布时重新验证值和声明。该契约不推测运行时格式文本，也不改变解析器：生成的消息保留固定前缀、真正的省略号和原始参数位值。

栈 block 的源码控制流传递会暂时把自有输出事实交换到局部临时状态，避免每个节点重复复制全部局部值和字节事实；合流规则、工作量与存储预算、成功证据及失败诊断保持一致。

原生状态保存分析通过统一且已验证的 ABI 映射检查结构体参数的每个寄存器分量，包括 ARM64 同质浮点聚合。有栈帧调用和无栈帧尾调用都会逐分量检查栈地址逃逸。栈帧借用仍绑定原始标量参数索引，不能授权结构体成员；栈上传递的结构体和间接结果存储仍需独立证明。此变更仅补全状态保存证据，不改变被调用函数 ABI 或源码闭包要求。

对于受限的 ARM64 Objective-C 类工厂，SDK 先证明完整的五条指令调用者和九条指令共享函数体，再投影该调用者。调用者确定 profiling 计数器与元数据访问器；共享函数体递增同一计数器、间接调用访问器、恢复栈帧，最后尾调用经过认证的 Swift 类转换。父类 getter 与工厂共用当前八条指令类访问器的证明。原始 LowIR 间接调用位置保留独立验证的 ABI 和完整栈帧证明。源代码 helper 使用既有整段 profiling 存储，保留无符号 64 位回绕及调用顺序，保留访问器依赖，并在发布时重新验证。其他调用者和共享 native ABI 不变。

ARM64 原生源码恢复仅在既有整数返回未通过完整定义载体检查后，才尝试 Float64 候选。源码专用 SSA 证明要求每个正常出口所在块都完整写入返回值低 8 字节，且这些字节的来源包含一处可达、当前已绑定源码签名的 Float64 调用。所有 PHI 输入必须有定义，定义必须支配使用，每个循环分量都必须有真实的外部初值；首批不支持指向函数入口块的回边。调用部分破坏必须沿当前 CallSiteId 和记录的 PreservedInput，核对目标 ABI 保留的精确 8 字节前缀，不能用陈旧的窄寄存器别名代替。单靠常量、加载、算术或未知返回值不能证明 Float64。既有当前调用、definedReturnPaths、原生状态、重新提升及最终绑定检查仍全部必需，通用 Med 类型推断不变。

普通 ARM64 栈帧证明也可包含精确的 `__stack_chk_fail` 终止出口，但必须由当前 loader 调用绑定及 `/usr/lib/libSystem.B.dylib` 的强 `___stack_chk_fail` 导入认证。声明必须是无参数、返回类型为 `void` 且不会返回的调用。只有这个末尾调用可在没有后继块、无需恢复寄存器的情况下结束该块；此前所有内存和实参检查仍然保留。必须至少存在一个可达的正常返回，且每条正常返回路径都要恢复全部应保存的机器状态。独立的终止入口证明仍使用原有规则。

LowIR 统一管理调用点的精确身份：指令地址、操作序号、操作码和静态目标。原生状态证明与源码返回值证明共享这一身份，但各自保留独立的许可边界。ARM64 单比特结果证明会检查每条可达路径，才能认定将第 63:1 位清零不会产生可观察差异；调用可以破坏寄存器，并不证明被调用方实际覆盖了这些位。若差异到达一次调用，该调用之后所有 ABI 可变的通用寄存器、向量寄存器和标志字节都可能不同；受保护字节保留原有事实。普通调用仍需独立建立完整 ABI。单独认证的 Swift 比较候选只记录编译器的原始单比特返回值和精确导入提供方，本身不会发布字节返回声明，也不会授权源码投影。

HighC 通过按精确值宽度复制字节的辅助函数输出普通内存写入。机器地址本身不能证明 C 的对齐要求或有效类型。写入语句、写入表达式和通过内存进行的赋值共用这一输出路径：地址和值各求值一次，并返回写入值作为表达式结果。

Swift 布尔结果验证联合检查当前 Objective-C 入口 ABI、不可变的直接调用指令、精确强导入和完整 LowIR 消费者证明。其他调用必须具备当前运行时目录 ABI、完整的 8 指令类访问器证明，或精确强导入且具有两个指针参数与指针返回值的 super `init`。发布时仍需复核原生依赖、super 接收者及栈帧。未证明的原生或动态调用和重复调用点均被拒绝；这些事实本身不会发布源码，也不会声明运行时字节返回 ABI。

具有入口地址上的函数符号的原生入口可暂时只把完整 x0 字作为可观察结果。源码推断绑定完整入口 ABI 后，发布检查会用该 ABI 重新运行同一 LowIR 证明；临时假设本身不提供源码绑定或字节返回 ABI。 原生保留寄存器推断只有用当前入口 ABI 重新验证该 Bool 调用的精确 LowIR 位置后，才能使用它。入口字节与完整状态恢复证明仍决定观察到的保留寄存器能否成为参数。 已绑定的双字原生结果只有在两个返回载体均通过原生配对证明时，才能把观察范围扩展到 x0/x1；发布前会重新推断完整的双字返回，再接受该 Bool 调用。

精确的 libswiftCore Hasher seed、String.hash(into:) 和 Hasher.finalize 导入可通过已验证的 Swift ABI 参数借用 ARM64 私有栈帧中的 72 字节区域。状态证明在调用后使所有借用字节失效，并拒绝与保存寄存器重叠或栈帧逃逸；仅名称匹配而缺少当前导入及 ABI 证明，不能授权借用。

Swift 6.1.2 客户端 IR 还表明，精确的 libswiftCore `_DictionaryStorage.allocate(capacity:)` 导入在 ARM64 和 x64 上返回指针，接收整数容量以及 `swiftself` 中的字典元数据。此经验证的 ABI 在保留分配效果的同时绑定调用；它本身并不能恢复调用者或其他字典依赖。

Swift 6.1.2 还将精确的 libswiftCore `_DictionaryStorage.copy(original:)` 与 `resize(original:capacity:move:)` 导入定义为返回指针、通过 `swiftself` 接收具体字典元数据的调用。扩容还接收整数容量和一个布尔字节。证明保留调用及分配效果，验证提供者和完整 ABI；仍有其他未解决依赖的调用者不会发布。

精确的 libswiftCore `KEY_TYPE_OF_DICTIONARY_VIOLATES_HASHABLE_REQUIREMENTS` 导入按 Swift 6.1.2 的声明接收一个类型元数据指针且永不返回。只有提供者及 ABI 均经验证时才应用此终止契约；原始调用与陷阱仍保留在源码路径中。

8 条指令的 ARM64 类元数据访问器现在由同一处机器证明验证。它检查不可变指令和 `objc_opt_self` 的强导入，证明入口参数未被使用，且返回值的全部 8 字节来自该运行时调用。这些事实不会授予类对象身份或源码闭包许可。super getter 与元数据工厂仍分别检查类身份、流水线、栈帧和依赖。结构化展开元数据的接受规则也改为共享；部分解析和语言异常分派仍被拒绝。

布尔归一化证明将 SP 视为每次调用的隐式输入，包括无参数调用和仅使用寄存器参数的调用。调用前若 SP 存在差异就必须拒绝；之后恢复 SP 无法撤销被调用方已经发生的栈访问。

布尔结果证明会逐位跟踪操作数位宽内的常量整数左移、逻辑右移、算术右移，以及截断到较小目标的按位运算。这覆盖了 ARM64 位测试谓词，同时不把未观察的运行时填充位声明为已定义。仅当两次执行的所有输入完全相同时，才允许变量移位或 SELECT；输入存在差异的变量移位、超范围常量移位，以及影响分支、参数、存储或返回值的填充位仍会拒绝归一化。

当所有返回路径都支持低 32 位投影，且至少一处明确包含未定义的高位填充时，可将推断出的原生 64 位整数返回候选缩窄为 32 位。完整源码控制流和所有局部定义必须一致；低位未知、循环定义、缺失分支和有副作用的高位表达式仍被拒绝。候选按新的源码 ABI 重新提升；读取被舍弃高字的调用者仍含未解析值，不能发布为已恢复源码。

Objective-C 常量数组和字典的元素可以保留精确导入的 CoreFoundation 布尔单例。每条边都必须来自唯一、不可变且有文件内容的存储，并具有无重叠重定位的强导入、零加数 SDK 数据绑定。生成的助手直接返回导入对象地址，保留重复元素的同一性，不复制对象表示。导入槽地址不等同于其加载的对象，布尔导入也不能充当字典字符串键；发布时重新验证完整图。

AArch64 Swift 类型引用配方还接受 `libswiftCore` 导出的精确 `_ContiguousArrayStorage` 名义类型描述符，其依据是保留的设备及模拟器 SDK 导出证据。导入必须为强绑定、零加数并位于唯一不可变存储中，同时通过既有的缓存、引用和名称编码证明；生成的 C 保留描述符身份、相对引用及共享可写缓存，不复制描述符字节。其他标准库描述符仍不受支持。

不可变自指向全局指针的精确数据地址，与读取其值共用重建后的指针宽度存储。初始指针指向该存储自身，同时保留不透明键身份与指针内容。发布直接地址时会重新验证映射存储的唯一性、本地自重定位及不可变性；可变、重叠、截断或存在冲突的存储保持未解决状态。

源码恢复可投影一个有界的 AArch64 本地叶函数，函数只包含完整宽度的寄存器复制和 `RET x30`。加载器验证已链接 Mach-O 的不可变机器码、本地链接属性以及原始 BL 的精确调用位置；平台、帧、链接及零寄存器操作数、内存效果和其他指令均被拒绝。顺序复制归一化为叶函数入口值，各使用方均先读取全部输入再写入目标。MedIR 转换、Objective-C 接收者与栈帧事实、原生状态恢复共用该变换，同时保留 BL 对链接寄存器的真实写入。原始 LowIR 以及通用提升、补丁行为保持不变。MedIR 与 HighIR 保存一致的证明记录，源码发布前重新验证当前字节和调用方指令边界。缺失、重复、冲突或过期证据仍保持未解析；含内存操作的外提辅助函数需要独立证明。 若叶函数写入被调用方须保存的寄存器，也不得将其声明为普通 C 函数。

该源码专用叶函数投影还接受 `ADRP` 后接不移位的 64 位 `ADD`，用于生成完整常量字符串对象地址。寄存器值显式区分入口输入和对象地址。页地址只作为内部中间值；返回时仍有页地址、算术溢出或对象未经验证时，拒绝整个投影。地址计算使用被调用函数中该指令的 PC。`readObjCConstantString` 验证每个最终对象及其内容，并将结果保存在证明记录中供重新比对。MedIR 将完整对象标记为 `DataAddress`，所有者为对象本身；发布仍经过常规源码绑定。常量写入清除重叠的接收者、入口寄存器和栈帧字节事实，保留未改动的寄存器与内存。原生输入推断和私有输出拒绝规则使用同一效果。

独立的普通调用证明记录支持本地 `ADRP x8; LDR x0,[x8,#imm]; RET x30` 类引用读取函数。共享加载器证明验证原始 BL、完整叶函数、不可变类导入槽及精确的 SDK 类与提供库归属。Objective-C 事实传递利用已证明的无输入行为保留栈帧私有性并恢复类接收者，绝不清除此前的逃逸。CALL 仍然保留，且必须另行取得原生签名绑定和完整源码依赖。MedIR 与 HighIR 保存一致的读取函数证明记录；发布时重新检查当前机器码、导入身份及唯一保留的普通调用。专用类导入存储检查只允许匹配的类元数据；普通字节和导入读取接口维持原有规则。

投影叶函数还可执行且仅执行一次 `STR Xn,[SP,#0]`，写入经过重新认证的常量字符串地址。凭据独立记录原始指令、写入时的值和最终寄存器值。事实传播与字节保存检查均要求当前 SP 已知且按 16 字节对齐，完整八字节槽位位于已分配的私有帧内；被覆盖的事实和调用的栈参数区域会失效。已逃逸的帧不能恢复私有性。所有包含此效果的发布函数（包括 Objective-C 和预先声明类型的原生函数）必须以一致的显式 Med/High 入口 ABI 通过完整栈帧状态证明；不得使用无栈帧回退，也不得为该辅助函数推断普通独立 ABI。

同一条 SP 存储还可写入归一化的叶函数入口寄存器值。所有消费者在最终寄存器写回前快照这一独立输入。字节保存检查拒绝任何源自当前栈帧的字节，并替换全部被覆盖的事实；槽位已写入不代表未知输入位已定义。只有已声明的出栈参数实际消费连续、完整的八个入口寄存器字节时，才可证明入口值被使用；未消费的保存/恢复溢出不能增加参数。Objective-C 传播仅保留已证明的标量、接收者或参数事实，拒绝已知复制 Block 身份，且不能恢复栈帧私有性。完整输入定义、ABI、栈帧状态与源码闭包检查仍然必需。

不透明的直接本机调用只有在该调用处两次执行的所有物理寄存器和标志位均相同时，才能参与布尔归一化证明。证明仍拒绝存在差异的内存观测、保留临时值差异，并在循环回边重新检查。这不授予被调用函数任何 ABI、返回值定义或源码绑定权限；发布仍要求原调用及依赖分别完成绑定。缺少当前 ABI 的可识别导入跳板和无效的现有绑定仍会被拒绝。

Swift 延迟对象 getter 也接受分别验证的 `swift_retain` 和后续 `objc_autoreleaseReturnValue`，保留两次调用及其实际返回值传递链。初始化前可存在一个空标签，但不能携带表达式、嵌套语句或内存效果。移除附带的 once 上下文仍须独立证明初始化器不使用上下文，并在发布时重新验证。

Swift `NSObject` 相等比较候选使用已独立验证的真机和模拟器 ABI `swiftcc i1(ptr, ptr, ptr swiftself)`：对象参数位于 x0/x1，元数据位于 x20。它要求来自 `libswiftObjectiveC` 的精确强导入、不可变存储及现有的完整调用方归一化证明。HighC 从同一规范输入契约生成 `_Bool` 原型和 `swift_context` 参数；仅查找到符号不能发布字节返回 ABI。

固定的零参数 Objective-C 对象 getter 只能作为 opaque identical-state 调用参与该归一化证明。当前 selector stub 的全部 20 个不可变指令字节、selector 引用、强 `objc_msgSend` 导入及精确 SDK 指针 ABI 必须一致；失败的 `__objc_stubs` 证据不能退回未知原生调用。该规则不授予 clobber、结果或源码绑定事实，因此调用点的每个物理寄存器、标志和内存观察都必须已经相同。
