**语言**: [English](sbf.md) | [简体中文](sbf.zh-CN.md) | [繁體中文](sbf.zh-TW.md) | [日本語](sbf.ja.md) | [한국어](sbf.ko.md) | [Français](sbf.fr.md) | [Deutsch](sbf.de.md) | [Español](sbf.es.md) | [Italiano](sbf.it.md) | [Русский](sbf.ru.md) | [العربية](sbf.ar.md)

# Solana SBF 反编译

[← 文档索引](README.zh-CN.md)

NeverD 将 Solana 部署制品作为一等 SBF 程序加载，并通过 CLI 与 `libneverd`
提供完整管线：

```text
SBF ELF
  → 版本感知的 ELF loader 与 verifier
  → 无损 LowIR + CFG
  → 规范化 MedIR + 寄存器事实
  → 恢复函数、syscall、CPI/account 观察与区域
       ├─ 已验证 LLVM IR
       ├─ 可移植 C11
       └─ 安全的 stable Rust
```

实现遵循当前 Anza `sbpf` VM，而不是把 Solana 程序当作普通 Linux eBPF。
版本、opcode、syscall、relocation、调用参数 ABI 与协议元数据集中在 `include/neverd/sbf/`
下的 `.def` 数据库；loader 与 backend 使用生成的类型化表，不重复编码或拼写。

闭集表包括 `SBFVersions.def`、`SBFOpcodes.def`、`SBFRelocations.def`、
`SBFArgumentRegisters.def`、`SBFVersionFeatures.def`, `SBFProtocolLimits.def`、`SBFSyscalls.def`、
`SBFSyscallMemory.def`、`SBFCPIABI.def`、`SBFProgramInstructions.def` 与
`SBFUpstreamSources.def`；
单次使用的诊断文本和 LLVM block 名称仍留在局部，遵循 LLVM 自身的 `.def` 策略。

`SBFProtocolLimits.def` 记录历史上的 65,536 条指令值与当前 10 MiB account data
上限；NeverD 从后者推导保守的 decode 上限。

relocation 完成后，唯一的不可变 VM 地址化 `ProgramImage` 是语义事实来源。
decoder、interpreter、字符串恢复、LLVM backend 以及 C/Rust backend 都读取同一
image，不再保留可能与 loader 语义漂移的独立 text/rodata 副本。

## 支持的输入与 VM 版本

输入是 ELF64 little-endian Solana 程序（`.so`）。支持当前 VM 的两种布局：

| SBF 版本 | ELF 布局 | Machine ID | 关键 ISA 行为 | 状态 |
|----------|----------|------------|---------------|------|
| v0 | 传统 section 与 relocation | `EM_BPF`、`EM_SBPF` | 带虚拟间隙的固定 frame、LDDW、传统 memory opcode | legacy |
| v1 | 传统 section 与 relocation | `EM_BPF`、`EM_SBPF` | 手工调整 stack frame | legacy |
| v2 | 传统 section 与 relocation | `EM_BPF`、`EM_SBPF` | PQR 算术、移动后的 memory encoding、互换的立即数减法、source-register CALLX | legacy，非单调 |
| v3 | strict program header，无动态 relocation | `EM_BPF` | static syscall/call、JMP32、destination-register CALLX，bytecode 位于 `0x100000000`，rodata 位于零 | 当前已部署 toolchain 格式 |
| v4 | strict program header，无动态 relocation | `EM_BPF` | v3 ISA 加对齐 memory-mapping 契约 | 当前上游 `sbpf`；集群可用性可能不同 |

版本号本身不是规范，因此 `SBFVersionFeatures.def` 持有各项行为变更，由版本表将它们
组合起来。每条记录都带上接纳该变更的 SIMD 提案，以及 `anza-xyz/sbpf` 就同一问题
暴露的谓词——因为多个提案会落在同一个版本里，而一个提案又会改变彼此无关的多件事：
SIMD-0173 既搬迁了内存指令类，也淘汰了 `lddw`；SIMD-0174 则在同一版本中独立地加入
PQR 类。把提案记录在特性上而不是版本上，才能让恢复出的版本结论一路追溯到决定它的
文档；这也是两条 `callx` 规则被拆成两个特性的原因：SIMD-0173 读源寄存器，
SIMD-0377 读目的寄存器。

v2 变更刻意不会泄漏到 v3；feature check 是显式条件，不用 `version >= N`
猜测。默认 strict 会拒绝畸形 header/range/alignment、不支持的 writable legacy
section、非法 continuation/register/frame-pointer 写入/branch，以及版本未启用 opcode，
并报告 instruction slot 与 virtual address。

## 描述所针对的运行时

ISA 版本来自文件本身，其余几乎都不是。哪些 syscall 能解析取决于链和 slot；某个
account 字段落在哪些字节上取决于拥有该程序的 loader；entrypoint 会不会收到第二个参数
取决于链拨下的一个开关；而一个程序能否被部署，与它跑不跑得起来是两个问题。单一的版本
开关表达不了其中任何一项，因此它们是各自独立的维度，各有各的表。

`SBFRuntimeFeatures.def` 记录 cluster、用途，以及会改变 NeverD 报告内容的 gate，每项
都带有运行时标识符、用状态记录激活信息的 feature account，以及各 cluster 激活它时所在
的 slot。pending account 可以已经存在而不启用 gate；某个 gate 若在某个 cluster 下没有
激活行，就表示它在那里尚未激活。`simd-0321` 在每个
cluster 上都已开启；`simd-0449` 与 SHA-512 syscall 在 testnet 与 devnet 上开启、在
mainnet 上关闭，这正是一个在 devnet 上跑得通的程序会在 mainnet 上失败的原因。

在固定的 Agave revision 中，`syscall_parameter_address_restrictions` gate
（`simd-0459`）收紧 syscall 与 CPI 参数的 VM 地址及对齐契约；finalized RPC 状态记录的
激活 slot 分别是 mainnet 429,840,000、testnet 407,468,256 与 devnet 462,240,000。
`account_data_direct_mapping` gate 在采用调整后的地址空间时，把 account data 从 input
buffer 中的副本改为由 account memory region 直接 backing；它在 mainnet 尚未激活，
在 testnet 408,332,256 与 devnet 463,968,000 激活。这两个 gate 都不会创设新的
Account ABI，也不会改变 ABIv0/ABIv1 的逻辑字段偏移：序列化仍由 owning loader 决定，
NeverD 将两者记录为 runtime topology 元数据。

feature bit 保持 append-only。可观测 snapshot 已超过 32 bit，因此
`RuntimeFeatureMask` 是 storage 与 host ABI 唯一使用的 `uint64_t` 类型。
v2 ABI 的宽度已冻结，不会 in-place 扩展；超过 64 bit 时应新增 v3 或 multiword 表示，绝不能改动 v2 宽度。
`RuntimeFeatureDisposition` 把仍然存在的 `RuntimeBranch` 与 `FoldedBranch`
明确区分：后者的激活侧在固定 revision 中已成为无条件行为，但旧侧对历史 slot
仍有意义。finalized RPC 激活事实（`—` 表示尚未激活）：

| gate | domain / disposition | mainnet | testnet | devnet |
|------|----------------------|---------|---------|--------|
| `disable_deploy_of_alloc_free_syscall` | `ProgramAdmission` / `FoldedBranch` | 209,088,008 | 195,356,264 | 224,208,000 |
| `enable_bpf_loader_set_authority_checked_ix` | `LoaderManagement` / `RuntimeBranch` | 251,424,000 | 247,628,260 | 255,744,000 |
| `remove_bpf_loader_incorrect_program_id` | `LoaderManagement` / `FoldedBranch` | 237,168,000 | 224,300,256 | 247,104,000 |
| `simplify_alt_bn128_syscall_error_codes` | `SyscallSemantics` / `FoldedBranch` | 274,320,000 | 278,300,256 | 308,448,000 |
| `abort_on_invalid_curve` | `SyscallSemantics` / `RuntimeBranch` | 311,904,000 | 300,764,256 | 342,576,000 |
| `deplete_cu_meter_on_vm_failure` | `VMFaultPolicy` / `RuntimeBranch` | 327,888,000 | 319,340,257 | 364,176,000 |
| `fix_alt_bn128_multiplication_input_length` | `SyscallSemantics` / `FoldedBranch` | 361,152,000 | 346,988,256 | 397,440,000 |
| `raise_cpi_nesting_limit_to_8` | `CPIExecution` / `RuntimeBranch` | — | — | — |
| `increase_cpi_account_info_limit` | `CPIExecution` / `FoldedBranch` | 403,056,000 | 385,868,256 | 435,456,000 |
| `poseidon_enforce_padding` | `SyscallSemantics` / `FoldedBranch` | 406,080,000 | 385,868,256 | 438,048,000 |
| `fix_alt_bn128_pairing_length_check` | `SyscallSemantics` / `FoldedBranch` | 406,944,000 | 385,868,256 | 438,480,000 |
| `alt_bn128_little_endian` | `SyscallSemantics` / `RuntimeBranch` | 425,088,000 | 406,604,256 | 456,192,000 |
| `enable_alt_bn128_g2_syscalls` | `SyscallSemantics` / `RuntimeBranch` | 425,520,000 | 406,604,256 | 457,056,000 |
| `loader_v3_minimum_extend_program_size` | `LoaderManagement` / `RuntimeBranch` | 432,864,000 | 416,540,256 | 470,880,000 |

这里有意不宣称覆盖 Agave 的整个 `FeatureSnapshot`。NeverD 只纳入会直接改变解码或
输出 host contract 的 loader、verifier、VM、entry/input、syscall 与 CPI
infrastructure gate。transaction scheduling、fees、consensus、transaction-level
precompile verification 以及 `CPI target built-in` 业务语义由 `external runtime`
负责；若未实现这些 built-in 却只加入 bit，会虚假宣称 NeverD 并不具备的能力。

`SBFLoaders.def` 记录归属与序列化。部署与执行早在多年前就不再是同一个答案：
`loader-v1` 与 `loader-v2` 会拒绝收到的每一条管理指令，同时继续运行它们早已拥有的
程序，这正是它们的序列化至今仍必须可读的原因。

| Loader | 序列化 | 可部署 | 可执行 |
|--------|--------|--------|--------|
| loader-v1 | `abi-v0` | 否 | 是 |
| loader-v2 | `abi-v1` | 否 | 是 |
| loader-v3 | `abi-v1` | 是 | 是 |
| loader-v4 | `abi-v1` | 否 | 否（内置程序已移除） |

`SBFAccountLayout.def` 标明每种序列化下每个 account 字段的位置。两者的差别不只在
padding——它们对字段的排序也不同：在偏移三处，非对齐形式放的是 account 地址的第一个
字节，对齐形式放的却是它的 executable 标志，而数值本身完全不会声明自己是从哪一种读
出来的。重复出现的 account 在 `abi-v0` 中占一个字节、在 `abi-v1` 中占八个字节，这会
让整趟遍历条目的过程错位，而不只是错开单个字段。

一次调用能否解析是三个问题而不是一个，因此 `SBFSyscallLifecycle.def` 保存已公布签名
的确定程度，`SBFSyscallRegistration.def` 保存其余部分：某个 syscall 出现在哪个
registry 中、由哪个 gate 管辖，以及那个 gate 指向哪一边。方向很重要，因为 gate 拿走
东西和添加东西一样容易——正是 `disable_fees_sysvar` 的激活移除了 fees sysvar
syscall——把一个做减法的 gate 读成做加法的，会一次把所有 cluster 的答案都反过来。
`sol_alloc_free_` 在边界前后都始终注册于 execution registry。deployment 在
`disable_deploy_of_alloc_free_syscall` 激活前会注册它，并从各 cluster 的激活 slot
起拒绝它。固定的 Agave revision 已把激活后的 deployment 行为折叠进 registry 构造；
NeverD 仍保留该 gate，让历史 profile 能得到激活前的答案。

在已激活 `simd-0321` 的运行时上，entrypoint 还会在 `r2` 中收到 instruction data 的
地址。NeverD 把它建模成一种自成一类的值而不是常量，因为它落在哪里取决于 account：
凭空编造一个地址，会让经由它的 load 被报告成某个具名的 account 字段。激活之前该寄存器
到达时为零，读取它的程序读到的就是零。因此生成的 LLVM、C 与 Rust entry point 同时接收
input buffer 与 instruction data，因为一个无法被传入第二个参数的可调用体，也就无法重现
一个会读取它的程序。

当前 Solana 工具链使用 `cargo build-sbf`。现代 v3+ 生产程序以 Rust 为主，上游 C
工具链不生成 v3；这不限制 NeverD backend，任一已接受 SBF 输入都可输出 C 或 Rust。

持续更新的权威资料：

- [Solana 程序](https://solana.com/docs/core/programs)
- [程序执行](https://solana.com/docs/core/programs/program-execution)
- [Syscall 参考](https://solana.com/docs/core/programs/syscall-reference)
- [Anza sbpf VM](https://github.com/anza-xyz/sbpf)
- [Agave changelog](https://github.com/anza-xyz/agave/blob/master/CHANGELOG.md)

## Loader 信任边界与运行时 image

strict v3/v4 只把经过边界检查的 executable program header 作为必需运行时输入。
section/symbol table 仅用于可选 debug enrichment；缺失或畸形会记录类型化状态，但
不会否决本来有效的运行时 image。这样既支持 stripped/sectionless 程序，也不放宽
指令验证。

legacy v0-v2 依照上游把 `.text`、`.rodata`、`.data.rel.ro` 与 `.eh_frame`
合并为只读 image，并检查 gap 和重叠。`R_BPF_64_64`、`R_BPF_64_RELATIVE`、
`R_BPF_64_32` 在 image 冻结前只应用一次，包括已部署 data/function-pointer
fixture 所需的旧式非 text relative relocation 行为。

## CLI

```bash
# 查看 machine、version、layout、VM address 与 section。
neverd info program.so
neverd headers --json program.so

# 查看全部分析阶段。
neverd lift --dump-low program.so
neverd lift --dump-med program.so
neverd lift --dump-high program.so

# 已验证 LLVM IR。
neverd lift -o program.ll program.so

# C 与 Rust 都是一等 backend。
neverd decompile --language=c -o program.c program.so
neverd decompile --language=rust -o program.rs program.so

# 对研究 fixture 指定 VM 契约，或为取证保留畸形输入。
neverd lift --sbf-version=v2 program.so
neverd lift --sbf-relaxed --dump-low program.so

# 说明答案针对的是哪个运行时。这些信息都不在程序文件里。
neverd lift --dump-high --sbf-cluster=devnet program.so
neverd lift --dump-high --sbf-slot=410400000 program.so
neverd lift --dump-high --sbf-loader=loader-v1 program.so
neverd lift --dump-high --sbf-purpose=deployment program.so
```

`--sbf-cluster`、`--sbf-slot`、`--sbf-loader` 与 `--sbf-purpose` 选择运行时 profile。
默认值描述的是当前状态下的 mainnet-beta、`loader-v3`，以及一个已经部署的程序。改问
部署，报告的就是哪些 syscall 会让一个程序上不了链，即便链本身会继续运行它。

`--sbf-version=auto|v0|v1|v2|v3|v4` 只在 ELF 通过已探测布局检查后改变
指令语义，用于损坏或研究 fixture；不能用它把不可信文件重新解释成另一种封装标准。

## 分析与恢复

LowIR 保留每个 8-byte encoding、raw field、LDDW continuation、解析的 call、
syscall hash、block、edge、reachability 与诊断。MedIR 把版本专用编码规范化为
typed 32/64-bit operation、显式 immediate/result extension、guarded arithmetic、
memory width 与 call kind。寄存器数据流跟踪常量及 stack/rodata address。

HighIR 恢复 entry/internal function、direct call edge、官方 syscall 名称、字符串、
natural loop、reducible conditional 及保守的 Solana 观察。对
`sol_invoke_signed_rust`/`sol_invoke_signed_c` 的 call 标为 CPI；基于 input
register 的 memory 标为 account/input access。不凭空虚构 Anchor 类型或 account layout。

C/Rust 共用 backend-neutral structuring pass。当所有可达 block 有唯一 reducible
表示时，输出直接 `if`/`if-else` 和 natural `while`/`loop`；internal call、CALLX
和 irreducible control flow 保留精确 PC dispatcher，使可读性不会改变语义。

syscall 数据库涵盖日志、memory、PDA、SHA-256/Keccak/Blake3、Poseidon、secp256k1、
curve/alt-bn128、大整数模幂、CPI、return data、sibling instruction、compute unit
查询及 epoch rewards 等当前 sysvar。每条记录携带精确 register arity、return kind、
effect、availability 和 provenance；官方 feature-gated 项与 stable 项分开，只有
Agave master 存在的 SHA-512、BLS12-381 decompress/pairing 不会冒充集群稳定 ABI。
审计 revision 位于 `SBFUpstreamSources.def`，不散落在 backend 中。

传统 relocation `R_BPF_64_64`、
`R_BPF_64_RELATIVE`、`R_BPF_64_32` 集中处理。text relocation 在解码前应用，
包括 LDDW address 两半和传统 VM loader 写入的官方 Murmur3 CALL key。对于已应用并
剥离 `R_BPF_64_32` 的制品，NeverD 从 function symbol 与 target slot 重算官方
function-registry key，以保留 internal-call 恢复。

## Solana 程序恢复

在 SBF 机器模型之上，NeverD 报告一个程序作为 Solana 程序意味着什么。每条记录下来的
事实都带有产生它的证据；字节没有决定的内容保持未设置，而不是猜测。

| 恢复内容 | 证据 |
|----------|------|
| read-only 数据中的 base58 地址 | 命中 `SBFKnownAddresses.def` 与 `SBFAnchorNamespaces.def`，或代码物化出的常量 |
| 程序自身声明的地址 | 针对 read-only 常量、长度恰为一个 key 的 `sol_memcmp_` |
| Anchor instruction dispatch | 常量等于带 namespace 的 SHA-256 discriminator 的 64-bit 比较 |
| CPI 目标 | 从 invoke 参数可达的 instruction 记录 |
| 一次调用选中的操作 | `SBFProgramInstructions.def` 中已列出的 selector，或开头的 Anchor discriminator |
| PDA 种子 | 从 derivation 参数可达的 seed descriptor 数组 |
| account 字段读写 | 地址可证明落在 serialized input 内的 load/store |

loader 只传一个参数，即 input region 起始处的 serialized input buffer，因此从该
entry state 出发的常量传播给出的是有名字的 account 字段而非裸 offset。
`SBFAccountLayout.def` 保存官方序列化布局，其固定字段会被检查为无空洞地铺满整个区间。

Anchor 用 SHA-256 对 `<namespace>:<name>` 求哈希并保留前 8 字节得到 discriminator，
这是单向的。因此 NeverD 只做候选确认：`SBFAnchorNames.def` 是部署程序中反复出现的
名字词典，`--sbf-idl` 提供程序自身的 IDL 并优先。只有当其中至少一个解析出名字后，
64-bit 比较才会被称作 discriminator。

`SBFKnownAddresses.def` 记录协议与规范程序地址；每个条目必须恰好解码为 32 字节，
测试会强制这一点。恢复还需要 syscall ABI：SBPFv3 把 read-only 数据映射到虚拟地址 0，
于是长度参数与低位数据地址是同一个数值。因此 `SBFSyscalls.def` 记录哪些参数寄存器
携带 VM 地址，只有这些会被跟踪。

两个 invoke syscall 用两种不同结构描述同一条 instruction，`SBFCPIABI.def` 按选中它
的 syscall 分别记录两套布局；用错布局不会报错，只会把第一个 account 当成被调用程序。
`SBFProgramInstructions.def` 再按各程序自己公布的 selector 命名操作：system、stake、
lookup-table 与 upgradeable-loader 用 bincode 变体序号，token 程序用首字节，并在与原
token 程序共享的编号之上叠加 Token-2022 的扩展区间。未列出的 selector 按数字报告。

### scratch 内存与 syscall 窗口

程序几乎不会把常量直接交给 runtime：它在自己的 frame 或 heap 上拼出 seed 数组、
序列化 instruction 及其 payload，然后只传一个指针。只读镜像会看到指针而看不到它指向
的内容，因此恢复维护一份只有本程序能写的内存的字节级模型，上限为
`kMaxModeledScratchBytes`。

scratch 恢复按需进行：只有存在真正的 `scratch consumer` 时才构造 Solana CPI/PDA
scratch 的 fixed point；没有该 consumer 的程序跳过 `whole-CFG fixed point`。
`SBFAnalysisLimits.def` 定义的是主机 `analysis policy`，不是 `protocol limits`：
`MaxModeledScratchBytes` 为每个 `program point` 保留 1,024 bytes，
`ScratchFlowRetainedByteBudget` 是 8,388,608 bytes 的 `logical retained estimate`。
超过预算时，恢复会显式 widening 为 `ScratchRecoveryPrecision::BlockLocal`。
只丢弃 `cross-block must-facts`；`block-local replay` 仍然 `sound`，并且仍可恢复
`same-block stores`。printer 稳定输出
`recovery scratch-precision=block-local`，widening 绝不返回
`half-converged must-facts`。

调用之后还剩下什么由两张表决定。`SBFSyscalls.def` 说明哪些参数寄存器携带 VM 地址；
`SBFSyscallMemory.def` 说明 runtime 通过它们做什么，即一次读或写，附带 `Fixed`、
`Counted` 或 `Opaque` 的范围。没有写窗口的 syscall 无法改动调用方的任何字节，所以
`sol_log_` 之前证明的内容之后依然成立；由长度参数界定的写只作废该窗口；`Opaque` 写
作废其基址及其之上的部分，因为缓冲区不会向下延伸，也不会跨越 VM region 边界。
`SBFSyscalls.def` 的效应摘要与该窗口表会双向互校，任一方都无法单独漂移。

`sol_memcpy_`、`sol_memmove_` 与 `sol_memset_` 会被跟进而不只是作废：目的地址、长度
与来源都可证明时，目的字节随之已知。Anchor 程序的 payload 是拷贝到位而非直接映射的，
正是这一步恢复出它调用了哪个操作。

只有已解析的 runtime syscall 才可能保留 scratch，且必须严格遵循其已审计的写窗口。每个
内部、间接或其他未解析调用都会清空已建模字节——即使当前没有参数指向 scratch——因为
先前逸出的指针或全局别名仍可能让被调用方改写它们。`sol_invoke_signed_rust` 与
`sol_invoke_signed_c` 写的是 account data 而非调用方内存，所以同一个 block 内拼出的
两次调用都可读。

该模型是函数内 CFG 上的前向 must 分析：只有当到达某 block 的每条路径都写入相同的值，
该字节才会存活到这个 block。call 边不跟进，因为被调用方不继承调用方的 frame。依赖
worklist 没有按 block 数降低精度的逃生阀；可选 Release 门会跑满 10 MiB、`1,310,720` 条
指令的协议上限。

`SBFLints.def` 归类整程序观察：缺失 signer 或 owner 检查、非常量的调用目标、已废弃
或受 feature gate 约束的 syscall，以及 SIMD-0500 将不再接受部署的 SBPF 版本。每项都
带 severity 与 confidence，且 lint 从不改变已解码的语义。这一层不做任何网络访问。

## 生成 LLVM 的 runtime 契约

提升后的 LLVM 绝不把 VM address 当 host pointer。受检查的 load/store/syscall 声明
返回 `i32` status；load 与 syscall 通过 output pointer 写入 `i64`。任何非零 status
都跳转到显式 SBF fault block。module 离开 backend 前通过 `llvm::verifyModule`。
runtime 声明按 ABI 使用类型化 `nounwind`、`captures(none)` 与 `writeonly` 属性，
fault callback 标为 cold。普通程序保持一个 LLVM block 对应一个已分析 SBF basic
block；包含 CALLX 时只增加表达任意有效 raw instruction address 所需的动态入口。

## 生成 C 的 host 契约

C backend 输出可移植 C11 与类型化 environment：

```c
#include <stdint.h>

typedef enum neverd_sbf_status {
  NEVERD_SBF_OK = 0,
  NEVERD_SBF_INVALID_INSTRUCTION = 1,
  NEVERD_SBF_MEMORY_ACCESS = 2,
  NEVERD_SBF_DIVIDE_BY_ZERO = 3,
  NEVERD_SBF_DIVIDE_OVERFLOW = 4,
  NEVERD_SBF_CALL_DEPTH = 5,
  NEVERD_SBF_UNKNOWN_SYSCALL = 6,
  NEVERD_SBF_UNKNOWN_FUNCTION = 7,
  NEVERD_SBF_EXECUTION_OVERRUN = 8,
} neverd_sbf_status;
/* v2 is fixed-width: values 0..8 reuse the legacy constants above. */
typedef uint32_t neverd_sbf_status_v2;
enum {
  NEVERD_SBF_INVALID_REGISTER = 9,
  NEVERD_SBF_INVALID_BRANCH = 10,
};
typedef uint64_t neverd_sbf_runtime_feature_mask;
typedef struct neverd_sbf_runtime_features {
  neverd_sbf_runtime_feature_mask bits;
} neverd_sbf_runtime_features;

/* Generated feature constants have the form NEVERD_SBF_RUNTIME_FEATURE_<Name>. */
typedef struct neverd_sbf_syscall_invocation {
  uint32_t hash;
  uint64_t arguments[5];
  neverd_sbf_runtime_features runtime_features;
} neverd_sbf_syscall_invocation;

/* v1 is the exact legacy four-field ABI. */
/* All callback fields return int, including the v2 callback. */
typedef struct neverd_sbf_environment {
  void *context;
  int (*load)(void *, uint64_t address, uint32_t width, uint64_t *value);
  int (*store)(void *, uint64_t address, uint32_t width, uint64_t value);
  /* Legacy syscall callback: hash, five arguments, output value. */
  int (*syscall)(void *, uint32_t hash,
                 uint64_t r1, uint64_t r2, uint64_t r3,
                 uint64_t r4, uint64_t r5, uint64_t *result);
} neverd_sbf_environment;

/* The v1 entrypoint reads only the four fields above. */
neverd_sbf_status neverd_sbf_program(
    neverd_sbf_environment *env, uint64_t input,
    uint64_t instruction_data, uint64_t *result);

/* v2 is a distinct ABI: the old layout is embedded and never extended in place. */
typedef struct neverd_sbf_environment_v2 {
  neverd_sbf_environment base;
  /* NULL callback falls back to base.syscall. */
  int (*syscall_with_features)(
      void *, const neverd_sbf_syscall_invocation *, uint64_t *result);
  /* NULL selects the program snapshot; a pointer to zero is an explicit empty snapshot. */
  const neverd_sbf_runtime_features *runtime_features;
} neverd_sbf_environment_v2;

neverd_sbf_status_v2 neverd_sbf_program_v2(
    neverd_sbf_environment_v2 *env, uint64_t input,
    uint64_t instruction_data, uint64_t *result);
```

`width` 以 bit 为单位。所有生成的 C callback（包括 `syscall_with_features`）都返回
`int`。在 v1 entrypoint `neverd_sbf_program` 中，0 表示成功；`load` 或 `store` 的任意非零
返回都会归一为 `NEVERD_SBF_MEMORY_ACCESS`，`syscall` 的任意非零返回都会归一为
`NEVERD_SBF_UNKNOWN_SYSCALL`；对应契约标记为 `v1-load-store-nonzero` 与
`v1-syscall-nonzero`，v1 不透传 callback 的 exact status。内部
`InvalidRegister` 与 `InvalidBranch` fault 也会归一为
`NEVERD_SBF_INVALID_INSTRUCTION`（`internal-invalid-instruction`）。
v2 entrypoint `neverd_sbf_program_v2` 才是 exact status 路径：已识别的
`neverd_sbf_status_v2` callback 值（包括 9 和 10）会作为已处理 fault 保留
（`v2-exact-status`）。v2 entrypoint
也会将内部 `InvalidRegister` 与 `InvalidBranch` 保留为 9 和 10。未知 callback 值使用生成器
针对该 operation 的 fallback（`operation-specific-fallback`）。
`syscall_with_features` 为 null 时回退到 `base.syscall`，其 callback 同样返回 `int`
（`feature-aware-null-base-syscall`）。
v1 struct 与 entrypoint 继续兼容 legacy host。应使用独立的 v2 entrypoint 获取
`syscall_with_features` 与已解析的 runtime-feature snapshot。生成源码表达 register、return PC、
callee-saved r6-r9、frame pointer、VM address、division fault、wide PQR operation 与 wrapping
shift。只生成程序实际使用的 helper，因此最小输出也能通过 `clang -Wall -Wextra -Werror`。

## 生成 Rust 的 host 契约

Rust 输出是安全 stable Rust，使用 trait 而非 raw pointer：

```rust
// The v1 source contract remains Result-based.
pub enum SbfError {
    InvalidInstruction, MemoryAccess, DivideByZero, DivideOverflow,
    CallDepth, UnknownSyscall, UnknownFunction, ExecutionOverrun,
}

#[repr(u32)]
#[non_exhaustive]
pub enum SbfErrorV2 {
    InvalidInstruction = 0, MemoryAccess = 1, DivideByZero = 2,
    DivideOverflow = 3, CallDepth = 4, UnknownSyscall = 5,
    UnknownFunction = 6, ExecutionOverrun = 7, InvalidRegister = 8,
    InvalidBranch = 9,
}

pub struct SbfRuntimeFeatures { bits: u64 }
impl SbfRuntimeFeatures {
    pub const fn from_bits(bits: u64) -> Self { Self { bits } }
    pub const fn bits(self) -> u64 { self.bits }
    pub const fn contains(self, feature: Self) -> bool {
        (self.bits & feature.bits) != 0
    }
}

pub struct SbfSyscallInvocation {
    pub hash: u32,
    pub args: [u64; 5],
    pub runtime_features: SbfRuntimeFeatures,
}

pub enum SbfSyscallOutcomeV2 {
    Unregistered,
    Returned(u64),
    Fault(SbfErrorV2),
}

pub trait SbfEnvironment {
    fn load(&mut self, address: u64, width: u8) -> Result<u64, SbfError>;
    fn store(&mut self, address: u64, width: u8, value: u64)
        -> Result<(), SbfError>;
    fn syscall(&mut self, hash: u32, args: [u64; 5])
        -> Result<u64, SbfError>;
}

pub trait SbfEnvironmentV2 {
    fn load(&mut self, address: u64, width: u8) -> Result<u64, SbfErrorV2>;
    fn store(&mut self, address: u64, width: u8, value: u64)
        -> Result<(), SbfErrorV2>;
    fn syscall(&mut self, hash: u32, args: [u64; 5])
        -> Result<u64, SbfErrorV2> {
        let _ = (hash, args);
        Err(SbfErrorV2::UnknownSyscall)
    }
    fn syscall_outcome(&mut self, hash: u32, args: [u64; 5])
        -> SbfSyscallOutcomeV2 {
        match self.syscall(hash, args) {
            Ok(value) => SbfSyscallOutcomeV2::Returned(value),
            Err(SbfErrorV2::UnknownSyscall) => SbfSyscallOutcomeV2::Unregistered,
            Err(error) => SbfSyscallOutcomeV2::Fault(error),
        }
    }
    // Some(SbfRuntimeFeatures::from_bits(0)) is an explicit empty snapshot.
    fn runtime_features(&self) -> Option<SbfRuntimeFeatures> { None }
    fn syscall_with_features(
        &mut self, invocation: SbfSyscallInvocation
    ) -> SbfSyscallOutcomeV2 {
        self.syscall_outcome(invocation.hash, invocation.args)
    }
}

pub fn neverd_sbf_program<E: SbfEnvironment>(
    env: &mut E, input: u64, instruction_data: u64,
) -> Result<u64, SbfError> {
    let _ = (env, input, instruction_data);
    unimplemented!("generated program body")
}
pub fn neverd_sbf_program_v2<E: SbfEnvironmentV2>(
    env: &mut E, input: u64, instruction_data: u64,
) -> Result<u64, SbfErrorV2> {
    let _ = (env, input, instruction_data);
    unimplemented!("generated v2 program body")
}
```

旧 entrypoint `neverd_sbf_program` 与 `SbfEnvironment` 构成
`v1-result-abi`，host method 使用 `Result`。`Some(SbfRuntimeFeatures::from_bits(0))`
表示 `explicit-empty-snapshot`，与 `None` 不同。`syscall_outcome` 是从基于
Result 的 host method 到 `SbfSyscallOutcomeV2` 的 `result-host-bridge`。
由于 `SbfErrorV2` 标有 `#[non_exhaustive]`，调用方在 match 时必须使用
`non-exhaustive-wildcard`（`_`）。

生成 entry point 对该 trait 泛型化，并用固定大小安全 array 表示 register 与 call
frame。测试以 `rustc --edition=2021 -D warnings` 编译代表性输出。

## C API

加载 SBF 后，现有 session 操作保持不变：同步的 recovered function、disassembly、
Low/Med/High/LLVM dump、CFG/call graph JSON、section、symbol、relocation、string、
header。用追加且 ABI 稳定的 output-language enum 明确选择 Rust。

```c
neverd_session_t session = neverd_session_create();
neverd_sbf_set_strict(session, 1);
neverd_sbf_set_version(session, "auto");
/* 答案针对的是哪个运行时。默认值描述的是当前状态下的 mainnet-beta、loader-v3，
   以及一个已经部署的程序。 */
neverd_sbf_set_cluster(session, "devnet");
neverd_sbf_set_slot(session, 474768000);
neverd_sbf_set_loader(session, "loader-v3");
neverd_sbf_set_purpose(session, "deployment");
/* Optional: name Anchor handlers from the program's own IDL. */
neverd_sbf_set_idl(session, idl_json);
const char *rust = neverd_decompile_all_ex(
    session, "program.so", NEVERD_OUTPUT_RUST, 0, 0);
/* consume rust, then: */
neverd_free_string(rust);
neverd_session_destroy(session);
```

## 验证与限制

当前 conformance baseline 于 2026-08-24 审计，锁定 Anza `sbpf`
`2510663bb8d894e8e3094be351e4bb4b604f1f84` 与 Agave master
`ef210d67f2fabeee1730498188fa78854260c679`。

| 证据 | 强制契约 |
|------|----------|
| 官方 ELF manifest | `sbpf/tests/elfs` 全部 23 个制品都有显式 load/execution 结果，当前 23/23 通过，包含 legacy relocated data、strict header、relative call 与 function pointer |
| 官方进程 oracle | `NeverDSBFExternalOracleTests` 将 1,411 个 opcode/verifier boundary case 与独立构建的 pinned verifier 对照 |
| 恶意输入 corpus | ELF table/segment overflow、重叠、畸形可选 metadata、非法 register、LDDW continuation、branch 与 immediate domain 均在预期边界拒绝或隔离 |
| raw-byte oracle | 直接执行验证后的 instruction bytes，不读取 MedIR，因此 MedIR 构造/损坏与 backend lowering 缺陷不会自动一致；显式上游结果与 semantic unit test 独立约束共享的类型化语义模型 |
| LLVM ORC 差分 | 对 versioned arithmetic、call/CALLX、memory、syscall 与 runtime fault 比较 return/fault、可写 memory 和 syscall trace |
| C/Rust 执行差分 | 生成 C11 以 `-Werror`、stable Rust 以 `-D warnings` 编译，并比较同一可观察状态，包含官方 relocated-data ELF |
| SBF 集成聚合 | `check-neverd-sbf` 运行全部已注册 suite；不把快速变化的汇总 case 数写成契约 |
| ASan + UBSan | focused target 在 fail-fast sanitizer 配置下无 report；不把快速变化的汇总 case 数写成契约 |

backend 的执行契约对外暴露 `r0` return value、fault status、VM memory effect 与
syscall call/result；其他最终 register 属于内部实现细节，不宣称为外部 ABI。

刷新证据时，更新 `SBFUpstreamManifest.def`、`SBFUpstreamOpcodes.def`、
`SBFUpstreamSources.def` 中的完整 revision，审阅上游 loader/verifier/config 与
Agave syscall 注册变化，然后运行：

```bash
NEVERD_SBPF_ROOT=/path/to/sbpf \
NEVERD_AGAVE_CONFORMANCE_ROOT=/path/to/firedancer-test-vectors \
NEVERD_AGAVE_CONFORMANCE_REVISION=68bb4af40235562e8852fa23d5727e49c2a0b862 \
  cmake --build build --target check-neverd-sbf
```

### 对比工具审计

这些工具可提供展示与恢复思路，但不能互相替代为语义 oracle。2026-08-24 的本地
审计结果如下：

- `sol-azy`（`362327a798e5dad6e12aa9abf3ed9ed52c17ef6a`）没有提交
  `Cargo.lock`。仅在隔离临时副本固定损坏的传递依赖后，它能解码官方 legacy
  `relative_call_sbpfv0.so`；其 `sbpf` v0.14.2 loader 会在当前 strict
  `relative_call.so` 上 panic，生成的 legacy CFG 还保留未定义节点。因此 NeverD
  只把它当展示层参考。
- `solana-data-reverser`（`bf90923adec984a61ca0437e9d341360ac1b11ee`）分析
  account-data byte 与 RPC metadata，不覆盖可执行 SBF 语义。
- `SolDragon`（`002b98677a5e595a773af6607b77210f5ea71db7`）明确把 stack
  frame、VM memory map、syscall name/signature 与 analysis plugin 标成 WIP。
- `bn-ebpf-solana`（`c3fe0de45d37eb68dcb08f2498c6e1f986056572`）提供 Binary
  Ninja UI/LLIL 与 SDK type，但依赖 Binary Ninja 5 及插件环境，不能作为 headless
  oracle 实跑。

语义权威始终是官方 `sbpf` 与 Agave。

明确限制：

- SBF binary rewriting 与 object-code roundtrip 会被明确拒绝。
- Anchor IDL/type 恢复和实时 Solana RPC/account 获取不属于 loader；可叠加在已恢复
  address 与 call metadata 之上。
- 生成源码通过 host contract 暴露 syscall 与 VM memory，不是独立 Solana runtime。
- relaxed mode 只用于检查；非法指令保持显式，绝不被赋予猜测语义。

## 2026-08-24 可复现证据契约

`SBFUpstreamSources.def` 将审计固定到 Anza `sbpf`
`2510663bb8d894e8e3094be351e4bb4b604f1f84`、Agave
`ef210d67f2fabeee1730498188fa78854260c679` 与 Solana SDK
`122f32e571ce39face4beffaccea733e37c207fd`。官方 manifest 全部 23/23 通过；
`NeverDSBFExternalOracleTests` 经 `SBFOfficialOracleProtocol.def` 与
`SBFOfficialVerifierCases.def` 与 `SBFOfficialExecutionConstants.def`，将 1,411 个 opcode/verifier boundary case 和独立构建
的官方 verifier 对照。畸形 ELF 来自 `SBFOfficialELFMutations.def` 与表驱动 corpus；
其总数仍在演进，因此文档不猜、不冻结汇总数。
另有独立的 `41-case strict ELF 差分验证`：将完整 strict-v3 mutation matrix 同时
送入官方 `verify-elf-batch` 进程与 NeverD。这 41 个 case 不计入 1,411 个
opcode/verifier case 总数。

官方额外执行矩阵（`additional execution matrix`）是独立的：它正好包含 508 个
active `(Version,Opcode)` case 与 58 个 boundary case，共 566 个 exact execution
case。它不会替代 1,411 个 `verifier probes`，也不计入其中；同样不替代或计入
41-case strict ELF 差分验证。

`NeverDSBFAgaveConformanceTests` 还会认证 Firedancer test-vectors revision
`68bb4af40235562e8852fa23d5727e49c2a0b862`，并逐项匹配全部 1,955 个
`sol_compat_elf_loader_v1` fixture（1,399 个接受、556 个拒绝）。对于每个接受的 ELF，
它还比较 `entry_pc`、`text_off`、`text_cnt`、`rodata_hash` 与 `calldests_hash`。此门禁
刻意只检查 loader，不运行后续 instruction verifier，以免混淆 Agave 的两个阶段。

默认 chain profile 对 Agave 保持诚实：`SBF_RUNTIME_VERSION` 表按历史 cluster/slot
查询官方 feature account activation，使最大 ISA 依次从 V0 推进到 V1、V2、V3；当前
最大值仍是 V3。这属于 `RuntimeVersionPolicy::ChainProfile`。只有显式
`--sbf-version=v4` 才选择
`RuntimeVersionPolicy::UpstreamToolchain`，按 pinned `sbpf` 做专家级离线分析；这不
暗示 v4 已在链上激活。当前 10 MiB 上限精确为 `10'485'760` byte；65,536 仅保留为
历史 provenance/test 数据，不作为执行或解码限制。

feature、syscall、fault 与 source ABI 都由 typed `.def` registry 统一定义：
`SBFSyscallRegistration.def`、`SBFValidationRules.def`, `SBFFaultCodes.def`、
`SBFSourceStatuses.def`、`SBFArgumentRegisters.def`、`SBFEdgeKinds.def`。
`SBFFaultCodes.def` 固定 execution fault 的稳定值，`SBFSourceStatuses.def` 则单独
拥有 generated-source host ABI。loader
采用 `raw-first`：先修复 relative CALL，再按 ELF ordinal 将 raw relocation 精确应用
一次；稳定错误顺序为 text identity、CALL、relocation、entrypoint、read-only layout。
file offset 与 VM address 的映射是 gap-aware 的，绝不会在空洞中虚构 byte。

CFG 与 dataflow 按 function 隔离：call edge 不会成为同一 frame 的 predecessor；
shared tail 保持 ambiguous；同一自然循环的所有 latch 合成一个 multi-latch region。
worklist 与扁平 ownership 用 10,000 个 function、逆序 block、conditional latch fixture
守门，只约束可扩展性与完成性，不猜某台机器的秒数。

公开 SBF call graph 采用 `callgraph-budget=fail-closed`：typed input、
provenance、node、edge、element 和 `CallGraphOutputByteBudget` 使 JSON
严格为完整结果或空结果。预算耗尽时返回 `{"nodes":[],"edges":[]}`，
同时设置 `neverd_last_error()`；永远不发布部分 relation。

每条 activation row 都记录 cluster、feature account 与 slot，所以可对 live node 做
`RPC activation audit`，普通分析仍完全离线。竞品审计覆盖 Blueshift、`qedsvm`
（可对选定 bytecode path 生成 Lean 证明，但当前 ELF loader 只接受 V0）、
`leanprover-solanalib`、`sol-azy`、`bn-ebpf-solana` 与 Ghidra/SolDragon。
`ezBPF` 在 `88829078a6d7682a2baed0d696d500401c263750` 明确标注自身已 deprecated，
并指向 Blueshift；它是采用单一 byte-to-enum 映射的 archived predecessor，并不是理解
moved-memory、JMP32 与当前 v0-v4 矩阵的 version-aware decoder。在本次已审计
比较 pin 为 Blueshift `704e40f7aa82446555b19d9ffbc0a6e18a35480f`、`qedsvm`
`99bd5ede85374adc7fc5c835c2432ecf4e123fd1`、`leanprover-solanalib`
`6c115ef1ef6a0cde8dbd6fd875b7dc87d60939ec`；四个本地工具固定为 `sol-azy`
`362327a798e5dad6e12aa9abf3ed9ed52c17ef6a`、`solana-data-reverser`
`bf90923adec984a61ca0437e9d341360ac1b11ee`、`SolDragon`
`002b98677a5e595a773af6607b77210f5ea71db7` 与 `bn-ebpf-solana`
`c3fe0de45d37eb68dcb08f2498c6e1f986056572`。在本次已审计
的公开通用 SBF 反编译器范围内，NeverD 拥有我们找到的最强可复现证据；这是有边界
的比较结论，不是绝对、永久的“世界第一”宣称。

公开竞争工具审计还纳入 `r2ghidra-solana`（固定在
`eca0b8e2d307e00991e289b8f9b0f45743819f1b`）：它提供 Ghidra C-like UX、
`C-like-pdg` 以及 account/Anchor/string/syscall 视图；该 pin 的 CI 已通过，
但 Solana 专项 testsuite 被注释，CI smoke 只反编译 `/bin/ls`。直接复现也确认：
官方 V0 的 `relative_call_sbpfv0.so` 能产出合理 C，而官方 V3 的
`relative_call.so` 会在 `pdg` 失败；结果可重复。`radare2-solana` 固定在
`292d845681be377cadc9959a74c2cadeb6e7f412`，会把
V2-only 的 SIMD-0173/0174 按 `>=V2` 扩到 V3/V4，而官方 `program.rs` 明确它们
仅属 V2。`SBPF-3-1` 固定在 `0e602c93007faa96bccb8e1e12040954ff108b6f`，
只有 2/2 个简单 cargo test、没有 CI；version detection 仍是返回 none/V0 的
placeholder，high-nibble opcode decoder 错误，jump 使用 imm 而非 off。V0/V3 的
relative_call ELF 也产生同样错误的 pseudocode。NeverD 的优势是可复现的官方
V0–V4 loader/verifier/runtime/process-oracle evidence；这不否认这些工具的 UX 或 C output。

`SBFComparisonTools.def` 是竞品显示名称与完整 revision 的唯一权威。最终一次有边界的
公开扫描还得到以下结论：

- `blastrock/Solana-eBPF-for-Ghidra` 固定在
  `c3ad719004726fe924dbed901eca2744ad82c85d`，有真正的 Ghidra P-code UX，但只有一个
  不区分版本的 SLEIGH 模型，CALLX 固定取 `dst`，还混用 legacy/current opcode；没有
  真实测试或 CI，默认源码也缺少被引用的 relocation constant class。
- `SolEmu-Ghidra` 固定在 `6520af2ff104d5adbec24632ba3afa3bef0da529`，继承逐字相同的
  decoder，并在明确模拟或 placeholder 的 CPI、密码学与 ZK 行为外增加 emulator UI；同样
  没有真实测试或 CI。`Ghidra_sBPF` 固定在
  `907bd4476432ca83bb2352686ad1ccafdb38504c`，可手选 v1-v3，却把 V2-only 编码累积进
  V3，没有 V0/V4 自动选择，也没有测试或 CI。
- `solana-ebpf-ida-processor` 固定在
  `aacd215907266190ed9c6c1b408ca9309f92ecdd`，是有用的 IDA disassembler/relocation UI，
  不是源码 lifter；其混合 opcode 表总从 `imm` 读取 CALLX，且没有测试或 CI。
  `solana-bpf-reverse` 固定在 `39479a3bddb8cb866ee499266a76a1b54069b222`，从硬编码
  layout 猜测生成 heuristic report 与 Rust TODO 脚手架；实跑为 9 pass、2 fail、1 skip，
  没有 CI。
- `solens` 固定在 `22defa1c8f4118dacd42f5c291f1ac31609fc0e5`，是 V2-only 终端
  disassembler，测试数为 0，也没有 CI。`sbpf-decompiler` 固定在
  `37b8bc0edc7ce347abee466f5f974e900c1948df`，当前实现只有三行
  `Hello, world!`，测试数为 0，也没有 CI。
- `sbpf-eye` 固定在 `5277a52aeb58e50b6ff8f9020414334765369b49`，明确是 lightweight
  WIP instruction/CFG TUI；3 个测试通过，但没有 semantic IR、source emitter 或 CI。
  `svm_bytecode_analyzer` 固定在
  `12aa236db8964e6be661e38131c2dc81588cf19c`，是 disassembler/CFG analyzer 而非 lifter；
  register/offset byte 解码错误，实跑 17 pass、1 fail，也没有 CI。
- `giraffexiu/Solana-eBPF-for-Ghidra` 固定在
  `81c1e3c2b9ba35091e4a2d8bb6eb23fd59339f07`，只是同一 Ghidra 血系的单 commit
  snapshot，没有新增版本语义、测试或 CI。`CertSBF` 固定在
  `bb93a97cf0c64d119d08ec851e8e820315beb59e`，是有价值的旧 rBPF Isabelle/HOL
  形式化，不是当前 V0-V4 整程序源码反编译器。

这些结果只加强本次限定公开样本中的比较证据，不构成对未来工具或私有项目的绝对结论。

2026-08-24 最终 RPC audit 完全匹配：38 个 feature accounts、89 条 activation
rows；mainnet slot 为 441305159，testnet 为 433055669，devnet 为 487238699。
system-owned 的空 pending account（mainnet 上的 `VirtualAddressSpaceAdjustments`）
未激活。文档不硬编码 RPC URL。

Linux Release CI 通过 `--print-pinned-revision`、`--print-test-vectors-revision` 与
`--print-toolchain` 读取 exact pin，构建官方 oracle、认证稀疏 corpus，并导出
`NEVERD_SBPF_ORACLE` 和 `NEVERD_AGAVE_CONFORMANCE_ROOT`，因此两个 external test
都在 CI 中强制执行。普通本地运行若未提供明确的 oracle/corpus env，仍会发现这些
case，但允许 skip。
