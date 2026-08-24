**语言**: [English](evm.md) | [简体中文](evm.zh-CN.md) | [繁體中文](evm.zh-TW.md) | [日本語](evm.ja.md) | [한국어](evm.ko.md) | [Français](evm.fr.md) | [Deutsch](evm.de.md) | [Español](evm.es.md) | [Italiano](evm.it.md) | [Русский](evm.ru.md) | [العربية](evm.ar.md)

# EVM 反编译

[← 文档索引](README.zh-CN.md)

NeverD 可加载传统以太坊虚拟机字节码，构建专用的 256 位 LowIR、栈 SSA
MedIR 和恢复后的 HighIR，并输出 LLVM IR、C23 或 Solidity。默认采用严格分析，但
传统 EVM 并不对整段镜像预先验证操作码：只有确定 `Reachable` 的执行 lane 真正抵达
未分配或在所选硬分叉中未启用的操作码时，才在该操作码的准确 PC 报错。死字节和仅
`MayReachable` 的 CFG 候选不会被提升为严格错误。

Solidity 与 C 输出属于语义重建：它们保留解码后的操作码顺序、256 位算术、栈检查
和经过验证的控制流，但不声称还原合约最初的源码、标识符或类型。

## 快速开始

```bash
# 使用 i256/i512 值提升为经过验证的 LLVM IR。
./build/bin/neverd lift contract.evm -o contract.ll

# 查看各级 EVM 分析表示。
./build/bin/neverd lift --dump-low contract.evm
./build/bin/neverd lift --dump-med contract.evm
./build/bin/neverd lift --dump-high contract.evm

# 输出两种受支持的源码语言。
./build/bin/neverd decompile --language=c contract.evm -o contract.c
./build/bin/neverd decompile --language=solidity contract.evm -o contract.sol

# 选择历史操作码集合；或保留未知操作码作为显式故障节点以供取证。
./build/bin/neverd decompile --language=solidity \
  --evm-hardfork=cancun --evm-relaxed contract.evm
```

`disasm`、`cfg` 以及 C API 的 Low/Med/High/LLVM 查询同样接受 EVM 输入。
EVM 二进制重写会被明确拒绝；`patch` 仍只用于原生二进制。

## 可接受的输入

| 输入 | 识别与规范化 |
|------|--------------|
| 原始字节 | `.raw`、`.evmraw`，或带明确 EVM 扩展名的二进制内容 |
| 十六进制文本 | 可选 `0x`、任意 ASCII 空白；支持 `.evm`、`.hex`、`.bin`、`.bytecode`，也会探测通过验证且无扩展名的十六进制文本 |
| 编译器制品 | `.json` 根节点或 `evm` 下的 `deployedBytecode`、`runtimeBytecode`、`bytecode`；也支持 `contracts → file → contract → evm` 形式的 solc 标准 JSON |

运行时/部署字节码优先于创建字节码。若只有创建代码，NeverD 会识别有界、常量的
`CODECOPY`/`RETURN` 构造器包装，并提取被复制的运行时切片。构造器遍历使用与真正解码器
相同的单指令解码器，并以正在分析的硬分叉为准，因此某个字节在一个分叉里是数据、在另一
个分叉里是操作码时，也无法移动这条边界。只要 `deployedBytecode` 或
`runtimeBytecode` 字段存在，它就是权威输入：显式的 `0x` 会被接受为空的、自然停止的
运行时程序，并有意阻止回退到创建字节码。字段缺失时才会继续寻找下一候选；没有显式前缀的
缺失或纯空白 hex 会被拒绝。显式 raw 输入也允许为空。

### 编译器尾部数据

`EVMMetadataFields.def` 列出两种尾部格式。Solidity 写入一个 CBOR map，其末尾两个字节
只统计该 map 本身；`vyper` 写入一个以该 map 结尾的 CBOR array，其末尾两个字节统计包含
自身在内的整个尾部。把其中一种取帧方式当作另一种来读并不会大声失败——它只会落到偏离
两个字节的位置，并削去两个字节的真实代码——因此两种都会尝试，两者都不匹配的输入则原样
保留。

尾部会被读取两次：一次针对给定的输入，一次针对剥离部署包装后剩下的运行时代码。Vyper
把尾部移进了 initcode，运行时代码里不再留下尾部，因此只在剥离之后才查看的读取器，会把
一个自报家门的合约报告成未知构建。sequence footer 还给出运行时代码长度、data section
长度与 immutables 长度，从而无需执行构造器即可界定返回的代码。

### 不是指令的容器

`EVMBytecodeContainers.def` 在任何解码之前先对输入分类。自 EIP-3541 使 `0xEF` 不可
部署以来，开头的 `0xEF` 就等于承诺这些字节不是指令：

| 容器 | 标记 | 处置 |
|------|------|------|
| legacy | — | 按指令解码 |
| delegation（`eip-7702`） | `0xef0100` 且恰好 23 字节 | 报告目标账户；分析停止 |
| eof（`eip-3540`） | `0xef00` | 拒绝；尚无分叉启用它 |

delegation 指示符中的二十个字节是一个地址，而不是代码。解码它们等于把地址当成操作码
来读，并产出一个账户的控制流图，因此 `info` 会报告目标，分析则带着原因拒绝。这次拒绝
会区分两种情形：Pectra 之前该标记尚未分配，Pectra 起则是目标的运行时代码根本不存在。
长度不为此值的标记属于格式错误的输入，而不是该容器的一个变体，因此仍按指令处理，好让
解码器能指出它读不了的那个字节。

格式错误的十六进制、奇数位数字、未解析的链接占位符、有歧义的多合约制品、无效
元数据边界，以及缺失或纯空白的 hex 都会产生可操作的错误；显式空 raw 输入或 `0x`
运行时仍是合法空程序。C++ loader API 可通过
`BytecodeLoadOptions::ArtifactContract` 从多合约制品选择 `Contract` 或
`path/File.sol:Contract`。若多个源码文件定义了同名合约，未限定名称会被拒绝，
从而避免制品顺序静默选错字节码。

EVM 注册在 NeverD 核心 loader 注册表中，而非隐藏在 backend 插件后。因此 CLI、
C API、反汇编器、CFG 构建器以及 Low/Med/High/LLVM 查询路径会收到完全相同的
规范化镜像与 EVM 选项，入口之间不会出现格式识别或语义分析漂移。

## 硬分叉与操作码

已最终确定的传统操作码集从 Frontier 到 Fusaka 全部覆盖，包括 `PUSH0`、瞬态存储、
`MCOPY`、blob 操作码与 `CLZ`。Amsterdam 的四个计划操作码也已作为显式开发分叉
target 实现；默认 `latest` 仍指向 Fusaka。可接受名称为：

```text
frontier, homestead, dao-fork, tangerine-whistle, spurious-dragon,
byzantium, constantinople, petersburg, istanbul, muir-glacier, berlin,
london, arrow-glacier, gray-glacier, paris, shanghai, cancun, pectra,
fusaka, amsterdam, bogota, latest
```

也接受常用别名：`dao`、`tangerine_whistle` 等下划线拼法、`merge`、`prague`、
`osaka` 和 `glamsterdam`。目前 `latest` 与 `osaka` 均解析为规范的 `fusaka`
执行版本；`glamsterdam` 解析为 `amsterdam`。

`latest` 特指 NeverD 已实现的最新主网最终版本，而非 Ethereum 开发分支顶端。
Ethereum 将 [Glamsterdam](https://ethereum.org/roadmap/glamsterdam/) 描述为计划于
2026 年第四季度的升级。其仍处于 Review 阶段的
[SLOTNUM](https://eips.ethereum.org/EIPS/eip-7843) 与
[DUPN/SWAPN/EXCHANGE](https://eips.ethereum.org/EIPS/eip-8024) 仅在显式选择
`--evm-hardfork=amsterdam`（或 `bogota`）时启用，最终确定前不会进入 `latest`。
EIP-8024 只有合法候选字节会被消费；非法候选仍是下一条指令，缺失字节的语义值为零。
把它当作普通定宽立即数会破坏指令与 `JUMPDEST` 边界。

EOF 不属于 Fusaka：Ethereum 在
[Fusaka checkpoint 2](https://blog.ethereum.org/2025/04/29/checkpoint-2) 中移除了它。
EOFv1/EIP-7692 尚未排期，容器提案
[EIP-3540](https://eips.ethereum.org/EIPS/eip-3540) 的状态为 Stagnant。旧的
`execution-spec-tests` 仓库已经归档，仍维护的测试已迁入
[execution-specs](https://github.com/ethereum/execution-specs/tree/master/tests)。NeverD
不会把实验性 EOF 容器当成已确定的主网行为。

严格模式只在确定 `Reachable` 的状态 lane 证明执行会抵达时，才拒绝未知字节或分叉中
未启用的字节。`--evm-relaxed` 将它们保留为类型化 fault prefix 与诊断，生成 backend
执行到它们时仍会故障；宽松模式绝不把未知字节当成 NOP。

## LLVM 风格的元数据架构

手工维护的 EVM 元数据采用 LLVM 的可多次包含 `.def` 模式：

- `EVMOpcodes.def` 是全部已定案传统操作码与 opt-in 开发分叉操作码的唯一事实来源：
  编码、实际 pop/push 变化、立即数编码、操作码类别、启用分叉、主要 effect、正交
  EVM 内存访问、源码级
  状态访问、call-value 访问和终止属性都在同一记录中；新增操作码不会静默继承默认值。
- `EVMMemoryAccesses.def`、`EVMStateAccesses.def` 与
  `EVMCallValueAccesses.def` 定义封闭且具名的属性域。属性保持有类型且彼此正交：
  `CALL` 同时是外部调用和内存读写，`EXTCODECOPY` 同时读取上下文并写内存。
  状态访问使用显式 `None/Read/Write/Unknown` 格，而非可能形成非法组合的两个布尔值。
  payability 也是独立约束：`CALLVALUE` 的类型化读取属性使读取 `msg.value` 的区域
  输出为 `payable`，而不是错误的 `view`。分析器另行识别规范的
  `ISZERO(CALLVALUE)` 非 payable 守卫；只有确认其非零分支以 `REVERT` 结束时，
  才忽略这次编译器生成的读取。
- `EVMImmediateKinds.def` 定义定宽 PUSH data 与 EIP-8024 条件 single/pair 编码；
  `EVMDecodeStatuses.def` 统一 LowIR 与反汇编公开的稳定状态词汇。
  `EVMUpstreamOpcodePolicy.def` 记录 go-ethereum 名称别名，以及有意排除的历史项与
  未排期 EOF 项；
  `scripts/audit_evm_opcode_metadata.py` 会拒绝字节漂移和任何未审阅的上游新常量。
- `EVMHardforks.def`、`EVMEffects.def`、`EVMExitStatuses.def` 和
  `OutputLanguages.def` 生成对应的有序枚举、解析器、显示名称、CLI 选项与 C ABI 值。
- `EVMCalls.def` 描述调用另一个程序的四条指令，以及被调用地址来源的格。
  每条记录只有一个标志——value 操作数是否位于被调用者与参数窗口之间——由它推导出
  之后每一个操作数位置；该表会与操作码数据库交叉校验，使推导不会偏离已声明的
  pop 数。
- `EVMPrecompiles.def` 是协议自身应答的地址字典，每项都带有保留该地址的分叉，以及
  排定它的提案。`0x100` 上的 `P256VERIFY` 记在 `eip-7951` 名下：它才是随 Fusaka 在
  主网上保留该地址的 Final 提案；其接口所源自的 rollup 提案从未排期。其中有意不含
  gas：precompile 的开销是其输入的函数，且在地址与操作都不变的情况下被多次重新定价。
- `EVMMetadataFields.def` 与 `EVMBytecodeContainers.def` 描述一个输入在被解码之前
  究竟是什么：两种编译器尾部取帧方式，以及那些字节根本不是指令的容器。
- `EVMRecoveredFacts.def` 拥有恢复事实各词汇的拼写，使出现在输出中的名称集中在
  一处，而不是散落在可能遗漏新枚举项的 `switch` 里。`EVMKnownSignatures.def`
  将每个规范函数拼写与 selector 只存一次，再以独立的每标准
  `KnownFunctionVariantInfo` 记录声明返回列表及 independent/non-independent 证据角色。
  因此 ERC-20/ERC-721 共享拼写仍是同一个可调用候选，却不会独立证明任一标准，也不会
  继承表中第一个 variant 的返回类型；event 与 custom error 仍使用各自的类型化记录。
- `EVMAnalysisLimits.def`、`EVMInterpreterLimits.def`、
  `EVMABIParserLimits.def` 与 `EVMABITableLimits.def` 分别声明分析、解释器、
  parser 和公开表的阶段性上限。`EVMConstants.h` 统一管理共享协议宽度与稳定内部名称，
  并从 `EVMAnalysisLimits.def` 生成分析默认值和诊断选项名；解释器与 ABI header 则从
  各自的表生成所声明的限制。
- `Semantics.h` 拥有与目标无关的标量 ALU 求值器。常量折叠与解释器调用同一套经过
  检查的 `APInt` 实现；LLVM、C、Solidity 保持显式目标 lowering，使 backend
  契约及不支持情形始终可见。

独立 decoder 是原始字节边界；CFG 与栈分析在后一阶段消费其无损结果。操作码身份、
分叉启用、立即数合法性、实际 pop/push 变化和执行前所需栈深度彼此分离。宽松解码
不会让未启用操作码获得语义，也不会让其“可能的立即数”移动后续边界。EIP-8024
深度只解码一次并存入类型化指令字段，分析器、解释器与所有 backend 统一消费该字段。
最大动态栈需求为 236 word，最大非栈 host 操作为 7 个参数；两项限制独立命名并推导
或验证。

`OpcodeInfo` 不能默认构造成半有效记录，其名称使用 `llvm::StringLiteral`，不会形成
悬空 `StringRef`。编译期表验证器会拒绝重复编码、未知类别或属性、非法标量 ALU
契约、effect/状态访问不一致、立即数/栈家族契约错误，以及未标记为基本块
终结指令的分支。它还拒绝返回多于一个栈结果的非栈操作（共享 host ABI 只返回一个
word），并在专用 lowering 尚未实现时拒绝未知栈家族操作。宽松解码只能通过显式的
未知字节工厂取得保守故障元数据。

这些 `.def` 是手写数据库，类似 LLVM 的
[`Instruction.def`](https://github.com/llvm/llvm-project/blob/main/llvm/include/llvm/IR/Instruction.def)。
在 EVM 子系统中，`.inc` 只用于真实生成或字面 include 片段（例如 TableGen 输出），
不会把手工数据库伪装成生成产物。目标语言模板留在各自 C/Solidity emitter 中。
周边 C++ 遵循 LLVM [编码规范](https://llvm.org/docs/CodingStandards.html)，包括在公共
边界使用 LLVM ADT/字符串类型，以及对语义 switch 做显式、fail-loud 的穷尽处理。

这与 LLVM 自身的分层一致：小型手写 X-macro 数据库使用 `.def`；更丰富的声明式
记录使用 `.td`，再由 [TableGen](https://llvm.org/docs/TableGen/ProgRef.html) 生成
C++ 消费的 `.inc`。NeverD 目前没有 TableGen 生成步骤，因此提交一个看似生成、
却没有源生成器的 EVM `.inc` 只会增加形式负担。

新增操作码时，先增加一条完整 `EVM_OPCODE` 记录，再补充适用的共享标量语义、
显式 backend lowering 与聚焦测试。新增硬分叉时，增加一条有序 `EVM_HARDFORK`
记录及所需别名。类型 API、查找表、验证、分类和 CLI 值会自动扩展；backend 的语义
switch 仍保持显式，遗漏 ALU case 时立即失败。

## 分析模型

- **EVM LowIR** 保留 PC、编码、类型化立即数状态与解码后的栈深度操作数（包括 PUSH
  截断右补零和 EIP-8024 条件消费规则）、基本块、前驱/后继边、
  已验证的 `JUMPDEST` 目标、可达性与栈高度域。CFG 恢复采用确定性的全程序不动点：
  每个栈槽传播一个有界的 256 位有限值集合，每个具体高度保留一个抽象栈。因此，跨
  internal-call/return 基本块携带的常量、栈置换、`PC`/`CODESIZE` 与标量 ALU 运算都能
  解析一个或多个具体跳转目标；真正未知的目标会保留为显式 indirect 边，不会被猜测。

  back-edge 上发生变化的 loop-carried slot 会按语义 over-approximate 为 `Top`，使不动点
  收敛；这项 loop recurrence 抽象与资源预算无关。指令、block、状态、值节点、抽象栈、
  lane、边、worklist 更新和“指令×lane”传递都由具名预算计费，包括
  `MaxAbstractValuesPerSlot`、`MaxStackHeightVariants` 与
  `MaxAbstractInstructionTransfers`。零值或超限会在插入前返回硬错误，绝不触发额外的
  emergency widening 或静默截断。精确非法目标仍在对应 jump PC 失败；relaxed 模式只
  终止 fault 所属 lane。

  `EVMLowFaultKinds.def::InvalidJumpDestination` 在 `end-of-code JUMPI` 处按路径判定：条件
  确定为 true 且目标非法时，没有成功 tail，并记录确定 fault；条件确定为 false 时成功。
  条件未知时只保留可能成功的 false 路径，不会把整条 lane 错标为确定 fault。
- **EVM MedIR** 把每个栈值表示为 256 位 SSA 值，先连接全部合流 phi，再执行确定性的
  稀疏常量工作队列。私有格由 `Uninitialized`、单个精确 `Constant` 和 `Overdefined`
  组成：相同常量能跨基本块及带锚点的 phi 环传播，冲突或依赖运行时的环则不能伪造
  常量。工作队列检查全部 def-use ID；value、state lane、stack entry、operation、
  operation-lane reference、phi incoming 和 worklist update 各有独立预算。它还复用
  解释器所用的 `Semantics.h` ALU 求值器。
  MedIR 还保留主要 effect、正交的 `none/read/write/readwrite` EVM 内存访问、源码级状态
  与 call-value 访问。它为每个 LowIR whole-stack lane 保留独立 SSA execution lane，phi
  明确携带来源 lane；不再以最大高度对不兼容的栈做栈顶对齐。
- **EVM HighIR** 恢复 Solidity dispatcher selector、可能的 calldata/return word、
  mutability、常量 storage slot、LOG/event、revert 事实和函数/CFG 区域。经过检查的
  producer 索引与显式栈、带 memo 的值遍历从 MedIR 类型化操作数恢复事实，不再依赖
  指令距离：selector 比较可跨基本块与 phi，支持 `EQ` 任一操作数顺序及推导出的 32 位
  mask；参数偏移、storage key、event topic0、non-payable/receive 守卫和精确 32 字节
  return size 均读取语义输入。遍历由有限 MedIR 图提供结构边界；格式错误、混合或成环
  的表达式保守为 unknown。同一 selector 指向不同入口时会诊断并省略。payability 与
  状态访问格仍保持正交，可达但未解析的动态 jump 会令恢复保守回退为 `nonpayable`。
  逐字节、flow-sensitive 的 memory dataflow 会跨基本块跟踪固定偏移写入，按 overlap/kill
  合并字节，并在动态或未知写入时使相应知识失效。目前有证明的 payload 恢复只包括
  selector 与已知 Panic 字节。对已知 custom-error declaration，Solidity emitter 会保留
  canonical parameter type；这不代表恢复了每一个运行时 argument value。

  selector 发现只从 root lane 开始，并沿 dispatcher 的未匹配分支前进；handler 内部形似
  selector 的判断不会被提升为公开函数。receive 与 fallback 同样受 root 约束，而且必须
  到达确定可达的成功终态；revert、fault、non-payable 的空 calldata handler 或仅可能路径
  都不能建立入口。规范函数候选会被相冲突的 calldata 使用否决；共享 selector 不贡献独立
  标准证据。只有达到配置数量的独立兼容 selector，或出现精确 event topic/arity、storage
  slot、proxy 等强证据，才会识别标准并选择每标准 variant。其静态返回列表也只有在所有
  确定可达的成功终态都同意精确 ABI 字节数时才输出；未解析转移、冲突形状或不匹配都会
  fail closed，且 revert/fault 不算成功返回。名称、类型、event 与标准标签仍只是有证据
  支持的候选。

  HighIR 对 function、lane/operation visit、region block reference、memory read request、
  tracked byte、memory state cell 与 memory worklist update 分别设有恶意输入预算。memory
  不动点只消费确定可达且实际执行的 lane，在前驱间按字节共识做 meet；预算耗尽直接报硬错，
  不会截断事实。

  HighIR 同时记录接口的对外一半：每一条 `CALL`、`CALLCODE`、`DELEGATECALL` 和
  `STATICCALL`，包括被调用者的来源、当所分析的分叉在该地址上有保留时它所命名的保留
  地址、调用写入被调用者 calldata 开头的 selector，以及转账金额为常量时的取值。
  `CREATE` 与 `CREATE2` 被排除在外：它们执行的代码尚无地址，因此没有可恢复的被调用者。

  恢复出的对外签名绝不会计入程序自身应答的标准集合。发送
  `transfer(address,uint256)` 说明程序使用了某个代币，而不是说明它本身是代币；混淆
  两者会把每一个 router 和 vault 都报告为 ERC-20。委托调用会额外记为一条 proxy 事实，
  因为它是该族中唯一让被调用者代码运行在本程序自身 storage 上的成员。

  precompile 查找以所分析的分叉为准，而不是以现存最新的分叉为准。调用某个由后续分叉
  引入的 precompile 地址，实际到达的是一个没有代码的账户，调用会成功且不返回任何内容，
  因此为其命名等于报告一个程序确凿没有执行过的操作。
- **LLVM** 输出通过 verifier 的 `i32 @evm_execute(ptr)` 状态机，包含受检查的
  1024-word `i256` 栈、`i512` 模运算中间值、有守卫的有符号除法、饱和移位、精确
  `BYTE`/`SIGNEXTEND`/`CLZ`，以及经过验证的动态 jump switch。

内置确定性解释器是测试套件的语义 oracle。LLVM 与生成 C 会编译执行并与其比较；
生成 Solidity 会编译、部署到 Anvil，并比较可观察的 storage 与 trace。另有
pre-Fusaka 原始字节码 corpus 直接在 Anvil 原生 EVM 中执行，覆盖标量 ALU、calldata
复制、重叠 `MCOPY`、内存扩展、Keccak 和 returndata，形成独立的解释器到客户端对照。

带账户参数的操作码按[执行规范](https://github.com/ethereum/execution-specs/blob/master/src/ethereum/forks/osaka/vm/instructions/environment.py)
把栈操作数屏蔽到协议规定的 160 位地址宽度；公开环境值与 map 在执行前验证，避免
错误 `APInt` 宽度触发 LLVM 断言。`BLOCKHASH` 还执行前 256 个区块的协议窗口。

解释器会在任何操作码专属副作用之前，预检类型化的 required stack height、pop 数以及
保留后再 push 的高度，因此栈下溢或上溢不会让指令执行一半。`EVMForkSemantics.def`
规定字节 `0x44` 在 Paris 之前读取 `DIFFICULTY`，从 Paris 起读取 `PREVRANDAO`。
`REVERT`、语义 fault、step limit，以及 allocation/length 资源耗尽都会把 storage、
transient storage、log 与 selfdestruct 效果回滚到入口快照，同时保留帧内诊断和显式 revert
字节。分配失败用 `ExecutionFaultKind::ResourceExhausted` 表示，无需再分配错误字符串；若连
入口快照都无法建立，`HasPersistentStateSnapshot` 为 false，结果绝不可提交。

解释器将 EIP-211 每帧 returndata buffer 与当前帧最终输出分开：
`RETURNDATASIZE`/`RETURNDATACOPY` 读取最近子调用 buffer，只有 `RETURN` 或
`REVERT` 写入 `ExecutionResult::ReturnData`。因此 call 后的 `STOP` 不会错误暴露
callee 字节。CREATE/CREATE2 模型遵循同一规则：创建失败压入零并通过 EIP-211
buffer 暴露配置的 revert 字节；成功则压入配置地址并清空 buffer。
`InitialReturnData` 只是显式快照/测试种子，不是当前帧最终输出。

### 公开 IR 与资源边界

公开 `execute` 会先验证
`Code`/`Fork`/`Instructions`/`JumpDestinations` 共同构成规范 LowIR。篡改 fork、伪造
instruction record、让 encoding 不一致或改坏 jump-destination table，都会在解释器索引
instruction table 前返回 `llvm::Error`。公开 `lowerToMedIR` 同样会先验证所有配置的
option、resource bound 与 structural invariant，并严格保持这一顺序；随后才用内嵌的
fork/strictness 解码 `Low.Code`，通过 `canonical decode replay` 逐字段比较 LowIR。只有通过后
才能调用 `lowerCanonicalLowToMedIR`、建立索引或按调用方可控 record 分配输出。公开
`recoverHighIR` 同样会重放校验外部 LowIR/MedIR。私有 `lowerCanonicalLowToMedIR` 与
`recoverCanonicalHighIR` 仅用于 `analyze` 自己持有的 IR；它们只跳过冗余且非递归的重放，
HighIR 的 option/resource 预算仍强制执行。

dispatcher 证明为每个 `MedStateLane` 保存排序的 `Any/Exact/Excluded` selector domain。
join 会合并 Exact 集、求 Excluded 排除集的交集，并从 cofinite exclusion 中减去 Exact 集；
domain 变宽后会重新访问该 lane。相等判断只有在 selector 仍被允许时才记录 true-edge
candidate，并在 false edge 排除它。原始 `XOR(selector, constant)` 在所有规范 successor
指向同一入口时，把 zero/false edge 记录为 match；这种 fallthrough 不要求目标是
`JUMPDEST`。nonzero/true mismatch edge 会排除该 selector，`ISZERO` 则把同一表达式转换为
相等判断。selector word、零 calldata word、calldata size 与 call value guard 都逐边精化；
unknown conditional 会停止 dispatcher 证明，不会沿仅有可能性的分支继续。

识别出函数后，function-scope traversal 会携带该候选的 `exact singleton selector` 继续。
若控制流跳回 shared dispatcher，`SelectorEquality`、raw `XOR` 与 `SelectorWord` 只沿与已匹配
selector 一致的 `definite edge`；predicate 为 Unknown 或与 selector 无关时，则保守保留全部
`definite edges`。这里绝不采用“排除其它 entry block”的启发式，以保留合法的
`shared body/tail-call`。

外部 CALL/CREATE 的结果与此不同：host 结果本来就是非确定的，因此分析会探索两条精确 CFG
边。这既能保留 ERC-1167 fallback 恢复，又不会把不可读的 selector 条件当成证据；真正 Unknown
的 dispatcher 仍会封闭失败。

`EVMAnalysisLimits.def` 通过 `MaxLowDiagnostics` 和 `MaxLowDiagnosticBytes` 为线性 decoder 与
CFG 构造器提供同一个 aggregate LowIR diagnostic 预算。两条路径都按精确数量和最终字节数预先
计费，并拒绝零上限。LowIR 与 HighIR 的 diagnostic 预算彼此独立。同一张表还独立计费
`MaxHighDispatchCandidates`、全程序 aggregate
`MaxHighRecoveredArguments`、`MaxHighDiagnostics` 与 `MaxHighDiagnosticBytes`、
`MaxHighReferenceVisits`、`MaxHighMemoryTransferCells` 和
`MaxHighMemoryValueVisits`。candidate 与 recovered-argument record 会在写入任一目标
container 或分配 name/type 前预先计费。所有 HighIR 输出 diagnostic 都在构造或复制前按
数量与最终消息字节数计费，固定 malformed-IR diagnostic 也没有豁免；预算不足会返回带名称的
hard error，不会静默省略 diagnostic 或 fact。
默认根 CFG region 会在 reserve 或复制 block-PC 清单前计入
`MaxHighRegionBlockReferences`。

`EVMABIParserLimits.def` 限制 tuple nesting、type node 和 aggregate array dimension；
`EVMABITableLimits.def` 限制公开 signature/variant table 的基数与 aggregate text。
公开表校验会在 parse 或 hash 前先应用这些上限，再拒绝非法 enum、kind metadata、standard、
selector-evidence role、非规范类型、错误的派生 hash、membership 与 collision。生产 selector
lookup 使用索引，event lookup 使用按 topic 排序的表；topic API 会先确认 `APInt` 恰为一个
EVM word，再做比较或排序。

`EVMInterpreterLimits.def` 声明 `MaxSteps`、`MaxMemoryBytes`、`MaxTraceEntries`、
`MaxLogEntries`、aggregate `MaxLogDataBytes`、aggregate
`MaxHostReturnDataBytes`、`MaxCalldataBytes`、aggregate `MaxHostEnvironmentEntries`、
aggregate `MaxExternalCodeBytes` 与 `MaxPersistentStateEntries`。host entry 总量横跨
`BlockHashes`、`Balances`、`CodeHashes`、`ExternalCode` 与 `BlobHashes`；external-code
字节上限累加所有 `ExternalCode` body。`MaxSteps` 保持明确的 `StepLimit` 结果。运行期
memory、trace、log、log data 与新增 persistent-state key 都会
预先计费；超过配置上限会返回 `ResourceExhausted`，并回滚持久状态、log 和 selfdestruct
效果。初始 host return-data aggregate 或 persistent-state map 过大则属于 `execute` API
错误。解释器以 `ArrayRef` view 保留 host return data，并在已经验证、排序的 instruction
table 上使用 `lower_bound`，不会复制 buffer 或为每次执行重建 PC map。
`const execute preflight` 会在复制 environment、取得 persistent-state snapshot 或构造 result
之前，验证 program 与全部 host-input 上限。

### 实时 go-ethereum 差分审计

标准的本地与 CI 审计每次都以 `git fetch --depth=1 --force` 强制获取官方
`https://github.com/ethereum/go-ethereum.git` 默认分支的远端 `HEAD`。每次运行都会创建
名称不可预测的私有临时 bare repository，不使用共享持久 Git repository 或 cache。只有本次
fetch 返回的 authority ref 及从它解析出的精确 SHA 才能选择 revision。脚本会报告 SHA，在
detached 临时 worktree 中探测它，然后一起销毁 authority repository 与 worktree。`local_docs`、已有源码
checkout 和 submodule 都不是审计路径；固定 submodule 恰会在最需要发现实时漂移时变陈旧。

每条 Git 命令都会先清空全部继承的 `GIT_*`（包括 `GIT_CONFIG_*`），再只装入经过审计的
设置。`GIT_CONFIG_NOSYSTEM` 和 `GIT_CONFIG_GLOBAL` 禁用 system/global 配置；
`GIT_ATTR_NOSYSTEM` 与按命令设置的 `core.attributesFile` 禁用 system/global attributes，
`core.hooksPath` 禁用 hooks。私有 repository 会拒绝意外的本地配置、graft、`objects/info/alternates` 和
`refs/replace`；`GIT_NO_REPLACE_OBJECTS` 还会禁用 replacement 查找。任何偏离都会封闭失败。

```bash
python3 scripts/audit_evm_opcode_metadata.py
```

公开 CLI 唯一接受的选项是 `--manifest-output`，不提供 remote/ref/toolchain override。输出
manifest 的封闭契约是 `schema 3`。

Go 探针会反射 `params.Rules` 全部导出的布尔字段，对每个映射分叉调用公开的
`LookupInstructionSet(params.Rules)`，并扫描全部 256 个字节槽位，使未评审启用无法藏在
NeverD 请求之外。槽位分配只依据 geth 的 `operation.undefined` 判定；`HasCost` 只用于费用
交叉检查，因为已定义的零费用操作也会返回 false。每个 `defined && !HasCost` 槽位都必须从
声明的启用分叉起与 `EVM_GETH_ACTIVE_WITHOUT_COST` 精确匹配；未定义却有费用的槽位、未经
评审的已定义槽位，或隐藏该 marker 的上游表示变更都会封闭失败。封闭 schema 只接受 schema version、精确 geth revision、Go version、
stack limit、fork rules，以及各操作码的 byte、name、`base_min_stack` 与
`net_stack_delta`；最终 audit manifest 只额外加入 diagnostics 数组。未知或重复字段、规则、
分叉、名称和字节都会失败。`EVMUpstreamOpcodePolicy.def` 管理名称别名与类型化的已评审
历史/未排期 EOF 排除项，并验证其 overlap/inactive 不变量；正交的
`EVMUpstreamSemanticsPolicy.def` 管理封闭 `params.Rules` 反射清单、分叉映射、基础栈例外和
EIP-8024 dynamic opcode family 声明。CI 只在
`dev` 分支 push、pull request、手动触发和每日计划中运行，并在失败时上传精确 revision、
manifest 和日志 artifact。

更具体地说，`EVMUpstreamSemanticsPolicy.def` 用唯一一条 `EVM_GETH_RULE_FIELD` 将每个导出的
布尔 `params.Rules` 字段归入 `MappedForkSelector`、`NoOpcodeAllocation` 或
`ExcludedSelectorExpectedError`。审计每次只启用一个字段并调用 `LookupInstructionSet`：前两类
必须无错误，第三类必须返回错误；返回的完整 256 槽 opcode/stack 指纹始终必须等于
`ExpectedFork`。当前已评审的无分配字段 `IsEIP155`、`IsEIP2929`、`IsEIP4762` 与
`IsPetersburg` 指纹为 Frontier；`IsUBT` 应报错并返回 Cancun 指纹。

EIP-8024 dynamic opcode family 的成员与启用条件由 `EVMUpstreamSemanticsPolicy.def` 声明；
`EVMEIP8024Immediates.def` 仍是 single/pair 各字节 immediate semantics 的唯一权威。single/pair
清单分别显式把 256 个字节值全部分类为有效或无效，生产 decoder 直接查表。实时审计通过
`go -overlay` 向 `core/vm` 虚拟注入 wrapper，取得真正的私有 `operation.execute` handler，并对
每个 active table/family 执行 `DUPN`、`SWAPN` 与 `EXCHANGE` 的 `3x256` candidates 加
`3 missing-operand cases`。它核对接受性、PC 增量、由唯一 marker 推导的栈操作数和变更、有效
情况的精确 underflow，以及缺 operand 时的 `0x00`；Python 逐项对照同一 `.def`，不复制解码
公式。

`EVM_HARDFORK_LATEST` 只有一个规范目标。封闭的 `EVMUpstreamForkAliases.def` 将 Prague 映射到
Pectra，将 Osaka 及 BPO1 至 BPO5 映射到 Fusaka；Paris、Shanghai、Cancun、Amsterdam 与
Bogota 为恒等映射，未知新名称会封闭失败。每次审计固定并记录一个 `audit_unix_time`，要求
`MainnetChainConfig.LatestFork(time)` 映射到 NeverD latest，且
`LatestFork(max uint64)` 位于 alias 清单并已探测其规范分叉。探针枚举真实的
`canonical fork jump tables` 与 `mainnet active/scheduled jump tables`，逐表完整比较，并显式
记录 dynamic family 或分叉的 `inactive` 状态。只得到部分表、family 或探针的 `partial` result
不会被接受为 manifest，而会封闭失败。manifest
记录 `authority=official-fresh-fetch`、官方 URL、请求的 `HEAD` 与解析出的 SHA。公开 CLI 不提供
remote/ref/toolchain 绕过入口，probe 固定使用 `GOTOOLCHAIN=local`。

Go 与 Python 都会在具现恶意元数据前施加边界。两侧采用
`input/collection/string hard limits`，超限的 JSON 输入、数组或字符串均封闭失败；另行执行
`bounded diagnostic output`：超长 diagnostic 的展示会携带 full-content `digest` 与
`explicit truncated marker`，不会被误认为完整消息。每个子命令的输出和 deadline 也都有界；
超时或输出超限会终止整个 `process group` 及其后代 process tree，并排空 pipe。所有
`.def parser` 都会拒绝 unparsed、unknown、duplicate、missing、out-of-range 条目，任何偏离均
封闭失败。

当前 schema-3 实时回执记录 `schema_version=3`、`audit_unix_time=1787534659`、
`authority=official-fresh-fetch`、`remote=https://github.com/ethereum/go-ethereum.git`、
`ref=HEAD`、revision `02b73d4ea7181464175e0a6cbecc0a3a2655a562`、本地 `Go 1.24.0`、
`stack_limit=1024` 与 `diagnostics=[]`。它比较 `21 fork tables` 和 `20 Rules probes`，分类为
`15 mapped/4 no-op/1 expected-error`。两个 `mainnet active/scheduled` 记录均为
`upstream BPO2`，并由封闭 alias 映射到 `NeverD Fusaka`。EIP-8024 覆盖
`23 table targets`；只有 `Amsterdam/Bogota` 为 active，共产生
`1536 candidate executions` 和 `6 missing-operand cases`，且 `three handler symbols` 在两个
active target 间一致。收尾测试为 Python audit `67/67` 与 `C++ Opcode 10/10`。macOS 上的真实
审计在 `sandbox-exec` 中成功，最终 `go run` 保持 offline；Linux workflow 强制使用
`bubblewrap`。

所有 Go 阶段——`go env`、`go mod init`、`go mod edit`、`go mod tidy`、
`go mod download` 与 `go run`——都在 `capability-root` 文件系统沙箱中运行。读取能力只授予
私有 probe、fresh geth worktree、校验后的 `resolved GOROOT` 和精确必需的系统 runtime root；
只有隔离的 environment root 可写。网络只在需要它的依赖阶段开放，最终运行保持离线。
`host HOME/workspace` 中的 sentinel 会被拒绝访问，其内容也不允许出现在输出中。Linux 使用
同构的 `bubblewrap` 策略，且不使用 `/` broad bind。

`NeverDEVMDecoderPropertyTests` 还会在每个改变 decoder 的分叉上穷举全部双字节输入，
比较完整解码和精确 `JUMPDEST` 边界，并以有界长度的确定性恶意字节串覆盖所有分叉。

LowIR/MedIR 的路径 lane 保留路径内相关性，`MayReachable` 只提供 CFG 候选而不能生成确定
语义；HighIR 的 selector、receive、fallback、return shape 与逐字节 memory 事实都只消费
确定可达的执行 lane。共享 selector 与每标准 `KnownFunctionVariantInfo` 分离，返回类型必须
通过所有成功终态的形状检查。所有分析预算耗尽都会 fail loud，不会触发 emergency widening
或静默截断。

## 生成 C 的契约

C 输出使用 C23 扩展整数，避免算术截断到 64 或 128 位：

```c
#define NEVERD_EVM_WORD_BITS 256u
#define NEVERD_EVM_WIDE_WORD_BITS (2u * NEVERD_EVM_WORD_BITS)
typedef unsigned _BitInt(NEVERD_EVM_WORD_BITS) evm_word;
typedef signed _BitInt(NEVERD_EVM_WORD_BITS) evm_sword;
typedef unsigned _BitInt(NEVERD_EVM_WIDE_WORD_BITS) evm_wide;
```

纯操作、栈操作与控制流直接输出。依赖环境的操作调用：

```c
evm_word neverd_evm_host_op(
    struct neverd_evm_env *environment,
    uint8_t opcode,
    evm_word a0, evm_word a1, evm_word a2, evm_word a3,
    evm_word a4, evm_word a5, evm_word a6);

void neverd_evm_trace(
    struct neverd_evm_env *environment, uint64_t pc, uint8_t opcode);
```

参数采用 EVM pop 顺序：`a0` 是原栈顶。callback 在 `environment` 中实现内存、
storage、calldata、哈希、区块上下文、call、log 与 halt 副作用；返回值是该操作码
压入的第一个值，未用参数为零。每条解码指令前调用 `neverd_evm_trace`。

使用 frontend 至少支持 512 位 `_BitInt` 的 Clang target 编译：

```bash
clang -std=c2x -ffreestanding -c contract.c
```

Apple 的 Darwin Clang target 当前上限不足。在 macOS 上应使用具备该能力的非 Darwin
target 做源码验证，或直接使用 NeverD LLVM 输出；Linux Clang 支持所需宽度。

## 生成 Solidity 的契约

Solidity 输出同时提供两种视图：

1. 面向审计可读性的 selector 特定函数、storage、event 与 error 声明；
2. 保留精确算术与控制流、且检查 PC/stack 的状态机。

恢复出的常量 storage 事实以具名绝对 slot 常量输出，例如
`recovered_storage_slot_3 = uint256(0x3)`，不会伪装成顺序 Solidity 状态变量并
虚构 storage layout。

合约有意声明为 `abstract`。覆盖 `_evmHost` 以实现环境操作：`args_[0]` 是原栈顶，
返回值是首个压栈结果，并可更新 storage 或其他状态。`_evmTrace` 是 virtual，默认
发出 `EVMTrace`。该边界明确表达环境假设，而不是编造无法从字节码恢复的 Solidity。

```bash
solc --bin contract.sol
```

## C API

```c
neverd_session_t session = neverd_session_create();
neverd_evm_set_hardfork(session, "cancun");
neverd_evm_set_strict(session, 1);

if (!neverd_session_load(session, "contract.evm") ||
    !neverd_session_analyze(session)) {
  /* inspect neverd_last_error(session) */
}

const char *solidity = neverd_decompile_all_ex(
    session, "contract.evm", NEVERD_OUTPUT_SOLIDITY, 0, 0);
const char *c = neverd_decompile_all_ex(
    session, "contract.evm", NEVERD_OUTPUT_C, 0, 0);

neverd_free_string(solidity);
neverd_free_string(c);
neverd_session_destroy(session);
```

`neverd_decompile_all` 保持向后兼容并输出 C。EVM 相关的新入口为
`neverd_session_bitness`、`neverd_evm_set_strict`、`neverd_evm_set_hardfork`
和 `neverd_decompile_all_ex`。为原生二进制请求 Solidity 会返回明确的不支持语言错误。
旧 LLVM-to-C 路由标志对 EVM 会被拒绝而非静默忽略：C 源码使用专用 C23 backend，
验证过的 LLVM IR 使用 `lift`/LLVM 查询 API。原生对象码 round-trip API 同样明确
拒绝 EVM；LLVM IR 可用于分析，但 NeverD 不会假装原生对象 target 提供 EVM ABI。

## 明确限制

- 仅支持传统字节码；尚不解码 EOF 容器。
- Amsterdam/Bogota 是显式开发 target；在计划操作码最终确定前，`latest` 仍选择
  已最终确定的 Fusaka 指令集。
- 不提供 RPC 获取、链状态发现、gas 计量/退款或预编译执行。call 与环境值通过确定性
  解释器字段或 backend host hook 表达。
- 创建代码提取只识别常见静态包装，并非完整构造器交易模拟器。
- 动态 jump 保持显式间接 CFG 边，除非有界常量分析证明其为有效 `JUMPDEST`；可达
  未解析 jump 也会使恢复源码的 mutability 保持保守。
- ABI 类型、源码名称、mapping、event 与自定义 error 都是 best-effort 恢复事实；
  NeverD 不声称与原始源码相同。
- 使用 memory、storage、calldata、call、log、hash 或区块链上下文的合约若需独立
  执行，必须实现 C/Solidity 环境 hook。
