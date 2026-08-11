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
`SBFArgumentRegisters.def`、`SBFProtocolLimits.def`、`SBFSyscalls.def` 与
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

v2 变更刻意不会泄漏到 v3；feature check 是显式条件，不用 `version >= N`
猜测。默认 strict 会拒绝畸形 header/range/alignment、不支持的 writable legacy
section、非法 continuation/register/frame-pointer 写入/branch，以及版本未启用 opcode，
并报告 instruction slot 与 virtual address。

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
```

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
typedef struct neverd_sbf_environment {
  void *context;
  int (*load)(void *, uint64_t address, uint32_t width, uint64_t *value);
  int (*store)(void *, uint64_t address, uint32_t width, uint64_t value);
  int (*syscall)(void *, uint32_t hash,
                 uint64_t r1, uint64_t r2, uint64_t r3,
                 uint64_t r4, uint64_t r5, uint64_t *result);
} neverd_sbf_environment;
```

`width` 以 bit 为单位。host 非零返回值成为显式 SBF status。生成源码完整表达
register、return PC、callee-saved r6-r9、frame pointer、VM address、division fault、
wide PQR operation 和 wrapping shift；只输出程序实际使用的 helper，因此最小输出可
通过 `clang -Wall -Wextra -Werror`。

## 生成 Rust 的 host 契约

Rust 输出是安全 stable Rust，使用 trait 而非 raw pointer：

```rust
pub trait SbfEnvironment {
    fn load(&mut self, address: u64, width: u8) -> Result<u64, SbfError>;
    fn store(&mut self, address: u64, width: u8, value: u64)
        -> Result<(), SbfError>;
    fn syscall(&mut self, hash: u32, args: [u64; 5])
        -> Result<u64, SbfError>;
}
```

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
const char *rust = neverd_decompile_all_ex(
    session, "program.so", NEVERD_OUTPUT_RUST, 0, 0);
/* consume rust, then: */
neverd_free_string(rust);
neverd_session_destroy(session);
```

## 验证与限制

当前 conformance baseline 于 2026-08-10 审计，锁定 Anza `sbpf`
`71425d0de59e0bff048c6be8f4a8a9bc655916e2` 与 Agave master
`cae40aa610fdbdb313209bc1eec737079eb59688`。

| 证据 | 强制契约 |
|------|----------|
| 官方 ELF manifest | `sbpf/tests/elfs` 全部 20 个制品都有显式 load/execution 结果，当前 20/20 通过，包含 legacy relocated data、strict header、relative call 与 function pointer |
| 穷举 ISA matrix | v0-v4 每个版本检查全部 256 个 byte encoding，共 1,280 个 version/encoding cell，并覆盖 verifier 边界 |
| 恶意输入 corpus | ELF table/segment overflow、重叠、畸形可选 metadata、非法 register、LDDW continuation、branch 与 immediate domain 均在预期边界拒绝或隔离 |
| raw-byte oracle | 直接执行验证后的 instruction bytes，不读取 MedIR，因此 MedIR 构造/损坏与 backend lowering 缺陷不会自动一致；显式上游结果与 semantic unit test 独立约束共享的类型化语义模型 |
| LLVM ORC 差分 | 对 versioned arithmetic、call/CALLX、memory、syscall 与 runtime fault 比较 return/fault、可写 memory 和 syscall trace |
| C/Rust 执行差分 | 生成 C11 以 `-Werror`、stable Rust 以 `-D warnings` 编译，并比较同一可观察状态，包含官方 relocated-data ELF |
| SBF 集成聚合 | `check-neverd-sbf` 在 13 个测试二进制中发现并通过 107/107 个 case，其中包含 3 个 public C API integration case |
| ASan + UBSan | 12 个核心二进制的 101/101 个 case 在 fail-fast sanitizer 配置下通过；prebuilt LLVM package 缺少 public integration 所需的 NeverD fork-only header，因此该 integration binary 在 integrated build 中链接并运行 |

backend 的执行契约对外暴露 `r0` return value、fault status、VM memory effect 与
syscall call/result；其他最终 register 属于内部实现细节，不宣称为外部 ABI。

刷新证据时，更新 `SBFUpstreamManifest.def`、`SBFUpstreamOpcodes.def`、
`SBFUpstreamSources.def` 中的完整 revision，审阅上游 loader/verifier/config 与
Agave syscall 注册变化，然后运行：

```bash
NEVERD_SBPF_ROOT=$PWD/local_docs/sbpf \
  cmake --build build --target check-neverd-sbf
```

### 对比工具审计

这些工具可提供展示与恢复思路，但不能互相替代为语义 oracle。2026-08-10 的本地
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
