**语言**: [English](evm.md) | [简体中文](evm.zh-CN.md) | [繁體中文](evm.zh-TW.md) | [日本語](evm.ja.md) | [한국어](evm.ko.md) | [Français](evm.fr.md) | [Deutsch](evm.de.md) | [Español](evm.es.md) | [Italiano](evm.it.md) | [Русский](evm.ru.md) | [العربية](evm.ar.md)

# EVM 反编译

[← 文档索引](README.zh-CN.md)

NeverD 可加载传统以太坊虚拟机字节码，构建专用的 256 位 LowIR、栈 SSA
MedIR 和恢复后的 HighIR，并输出 LLVM IR、C23 或 Solidity。默认采用严格分析：
未分配的操作码或在所选硬分叉中尚未启用的操作码，会在其准确 PC 位置报错。

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
`CODECOPY`/`RETURN` 构造器包装，并提取被复制的运行时切片。只含可选 `0x`
前缀的制品字段视为空，因此空的 `deployedBytecode` 或 `runtimeBytecode` 不会遮蔽
可用的创建字节码回退。仅当编码长度、CBOR map 标记以及已知的 `solc`、`ipfs`
或 Swarm 键均通过验证时，才移除尾部 Solidity CBOR map。

格式错误的十六进制、奇数位数字、未解析的链接占位符、有歧义的多合约制品、无效
元数据边界和规范化后的空代码都会产生可操作的错误。C++ loader API 可通过
`BytecodeLoadOptions::ArtifactContract` 从多合约制品选择 `Contract` 或
`path/File.sol:Contract`。若多个源码文件定义了同名合约，未限定名称会被拒绝，
从而避免制品顺序静默选错字节码。

EVM 注册在 NeverD 核心 loader 注册表中，而非隐藏在 backend 插件后。因此 CLI、
C API、反汇编器、CFG 构建器以及 Low/Med/High/LLVM 查询路径会收到完全相同的
规范化镜像与 EVM 选项，入口之间不会出现格式识别或语义分析漂移。

## 硬分叉与操作码

传统操作码集从 Frontier 到 Fusaka 全部覆盖，包括 `PUSH0`、瞬态存储、`MCOPY`、
blob 操作码与 `CLZ`。默认的 `latest` 指向 Fusaka。可接受名称为：

```text
frontier, homestead, dao-fork, tangerine-whistle, spurious-dragon,
byzantium, constantinople, petersburg, istanbul, muir-glacier, berlin,
london, arrow-glacier, gray-glacier, paris, shanghai, cancun, pectra,
fusaka, latest
```

也接受常用别名：`dao`、`tangerine_whistle` 等下划线拼法、`merge`、`prague`
和 `osaka`。目前 `latest` 与 `osaka` 均解析为规范的 `fusaka` 执行版本。

`latest` 特指 NeverD 已实现的最新主网最终版本，而非 Ethereum 开发分支顶端。
Ethereum 将 [Glamsterdam](https://ethereum.org/roadmap/glamsterdam/) 描述为计划于
2026 年第四季度的升级；仍处于 Review 阶段的
[SLOTNUM](https://eips.ethereum.org/EIPS/eip-7843) 与
[DUPN/SWAPN/EXCHANGE](https://eips.ethereum.org/EIPS/eip-8024) 在分叉及编码最终
确定前不会进入默认表。尤其是 EIP-8024 的立即数字节具有不同于 `PUSH` 的
`JUMPDEST` 屏蔽规则，不能假装成普通单字节立即数。

EOF 不属于 Fusaka：Ethereum 在
[Fusaka checkpoint 2](https://blog.ethereum.org/2025/04/29/checkpoint-2) 中移除了它，
execution-spec-tests 也记录 EOF 已
[从 Osaka 移除且尚未重新排期](https://github.com/ethereum/execution-spec-tests/blob/main/docs/CHANGELOG.md)。
NeverD 不会把已撤回的 EOF 提案当成已确定的主网行为。

严格模式拒绝未知字节和分叉中未启用的字节。`--evm-relaxed` 会把它们保留在
LowIR 与诊断中，但执行到它们时，生成 backend 仍会故障；宽松模式绝不把未知
字节静默当成 NOP。

## LLVM 风格的元数据架构

手工维护的 EVM 元数据采用 LLVM 的可多次包含 `.def` 模式：

- `EVMOpcodes.def` 是 150 个已分配传统操作码的唯一事实来源：编码、完整栈契约、
  立即数宽度、操作码类别、启用分叉、主要 effect、正交 EVM 内存访问、源码级
  状态访问、call-value 访问和终止属性都在同一记录中；新增操作码不会静默继承默认值。
- `EVMMemoryAccesses.def`、`EVMStateAccesses.def` 与
  `EVMCallValueAccesses.def` 定义封闭且具名的属性域。属性保持有类型且彼此正交：
  `CALL` 同时是外部调用和内存读写，`EXTCODECOPY` 同时读取上下文并写内存。
  状态访问使用显式 `None/Read/Write/Unknown` 格，而非可能形成非法组合的两个布尔值。
  payability 也是独立约束：`CALLVALUE` 的类型化读取属性使读取 `msg.value` 的区域
  输出为 `payable`，而不是错误的 `view`。分析器另行识别规范的
  `ISZERO(CALLVALUE)` 非 payable 守卫；只有确认其非零分支以 `REVERT` 结束时，
  才忽略这次编译器生成的读取。
- `EVMHardforks.def`、`EVMEffects.def`、`EVMExitStatuses.def` 和
  `OutputLanguages.def` 生成对应的有序枚举、解析器、显示名称、CLI 选项与 C ABI 值。
- `EVMConstants.h` 统一拥有协议宽度、限制和稳定默认名称。
- `Semantics.h` 拥有与目标无关的标量 ALU 求值器。常量折叠与解释器调用同一套经过
  检查的 `APInt` 实现；LLVM、C、Solidity 保持显式目标 lowering，使 backend
  契约及不支持情形始终可见。

decoder 是原始字节边界。操作码身份与硬分叉启用状态刻意分离：宽松解码会保留已分配
但在所选历史分叉中未启用指令的名称、引入分叉与立即数宽度，同时让其语义查询保持
保守并会故障。这样，携带立即数的未启用指令不会移动后续字节边界或意外获得当前语义。
分析、解释与所有 emitter 均使用生成的 `Opcode` 枚举和元数据查询；原始编码只在
trace、host callback 等面向字节的 ABI 边界再次出现。`SWAP16` 有 17 个逻辑栈输入，
而最大的非栈 host 操作有 7 个参数；两项限制分别在编译期推导。

`OpcodeInfo` 不能默认构造成半有效记录，其名称使用 `llvm::StringLiteral`，不会形成
悬空 `StringRef`。编译期表验证器会拒绝重复编码、未知类别或属性、非法标量 ALU
契约、effect/状态访问不一致、PUSH/DUP/SWAP/LOG 家族契约错误，以及未标记为基本块
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

- **EVM LowIR** 保留 PC、编码、PUSH 立即数（截断时右侧补零）、基本块、前驱/后继边、
  已验证的 `JUMPDEST` 目标、可达性与栈高度。
- **EVM MedIR** 把每个栈值表示为 256 位 SSA 值，创建合并 phi，对纯操作做常量折叠，
  并保留主要 effect、正交的 `none/read/write/readwrite` EVM 内存访问、源码级状态访问
  与 call-value 访问，为后续数据流、别名、mutability 和 payability 分析保留真实信息。
- **EVM HighIR** 恢复 Solidity dispatcher selector、可能的 calldata/return word、
  mutability、常量 storage slot、LOG/event、revert 事实和函数/CFG 区域。名称和类型明确
  属于启发式。payability 与状态访问格独立组合：无守卫 `CALLVALUE` 决定声明为
  `payable`；已证明的非 payable 守卫不会污染函数体 mutability。可达但未解析的动态
  jump 将状态访问合并为 `Unknown`，使 Solidity 保守回退为 `nonpayable`，不会作出
  不可靠的 `pure`/`view` 承诺。同一 selector 的冲突 dispatcher 模式会被诊断并省略。
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

解释器将 EIP-211 每帧 returndata buffer 与当前帧最终输出分开：
`RETURNDATASIZE`/`RETURNDATACOPY` 读取最近子调用 buffer，只有 `RETURN` 或
`REVERT` 写入 `ExecutionResult::ReturnData`。因此 call 后的 `STOP` 不会错误暴露
callee 字节。CREATE/CREATE2 模型遵循同一规则：创建失败压入零并通过 EIP-211
buffer 暴露配置的 revert 字节；成功则压入配置地址并清空 buffer。
`InitialReturnData` 只是显式快照/测试种子，不是当前帧最终输出。

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
- Review 阶段的 Amsterdam 操作码未启用；`latest` 当前选择最终确定的 Fusaka 指令集。
- 不提供 RPC 获取、链状态发现、gas 计量/退款或预编译执行。call 与环境值通过确定性
  解释器字段或 backend host hook 表达。
- 创建代码提取只识别常见静态包装，并非完整构造器交易模拟器。
- 动态 jump 保持显式间接 CFG 边，除非有界常量分析证明其为有效 `JUMPDEST`；可达
  未解析 jump 也会使恢复源码的 mutability 保持保守。
- ABI 类型、源码名称、mapping、event 与自定义 error 都是 best-effort 恢复事实；
  NeverD 不声称与原始源码相同。
- 使用 memory、storage、calldata、call、log、hash 或区块链上下文的合约若需独立
  执行，必须实现 C/Solidity 环境 hook。
