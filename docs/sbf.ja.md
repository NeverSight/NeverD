**言語**: [English](sbf.md) | [简体中文](sbf.zh-CN.md) | [繁體中文](sbf.zh-TW.md) | [日本語](sbf.ja.md) | [한국어](sbf.ko.md) | [Français](sbf.fr.md) | [Deutsch](sbf.de.md) | [Español](sbf.es.md) | [Italiano](sbf.it.md) | [Русский](sbf.ru.md) | [العربية](sbf.ar.md)

# Solana SBF 逆コンパイル

[← ドキュメント索引](README.ja.md)

NeverD は Solana deploy artifact を first-class SBF program として読み込み、CLI と
`libneverd` から全経路を提供します。

```text
SBF ELF
  → version-aware ELF loader / verifier
  → lossless LowIR + CFG
  → normalized MedIR + register facts
  → function、syscall、CPI/account observation、region を復元
       ├─ verified LLVM IR
       ├─ portable C11
       └─ safe stable Rust
```

Solana program を generic Linux eBPF とみなさず、現在の Anza `sbpf` VM に従います。
version/opcode/syscall/relocation/protocol metadata は `include/neverd/sbf/` の
`.def` database に集約し、loader/backend は generated typed table を利用します。

## 対応 input と VM version

入力は ELF64 little-endian Solana program（`.so`）です。

| SBF | ELF layout | Machine ID | 主な ISA behavior | 状態 |
|-----|------------|------------|-------------------|------|
| v0 | legacy section/relocation | `EM_BPF`, `EM_SBPF` | virtual gap 付き fixed frame、LDDW、legacy memory opcode | legacy |
| v1 | legacy section/relocation | `EM_BPF`, `EM_SBPF` | manually adjusted stack frame | legacy |
| v2 | legacy section/relocation | `EM_BPF`, `EM_SBPF` | PQR arithmetic、移動した memory encoding、swapped immediate subtraction、source-register CALLX | legacy、非単調 |
| v3 | strict program header、dynamic relocation なし | `EM_BPF` | static syscall/call、JMP32、destination-register CALLX、bytecode `0x100000000`、rodata zero | 現行 deploy toolchain format |
| v4 | strict program header、dynamic relocation なし | `EM_BPF` | v3 ISA と aligned memory-mapping contract | upstream `sbpf` 現行。cluster availability は異なり得る |

v2 の変更は v3 に漏れません。feature check は明示的で、`version >= N` とは推測
しません。既定の strict mode は malformed header/range/alignment、unsupported
writable legacy section、invalid continuation/register/frame-pointer write/branch、
version-inactive opcode を instruction slot と virtual address 付きで拒否します。

現行 toolchain は `cargo build-sbf` を使い、v3+ production program は Rust 中心です。
upstream C toolchain が v3 を target にしないことは NeverD output を制限せず、すべての
accepted SBF input を C/Rust の両方に出力できます。

- [Solana programs](https://solana.com/docs/core/programs)
- [Program execution](https://solana.com/docs/core/programs/program-execution)
- [Syscall reference](https://solana.com/docs/core/programs/syscall-reference)
- [Anza sbpf VM](https://github.com/anza-xyz/sbpf)
- [Agave changelog](https://github.com/anza-xyz/agave/blob/master/CHANGELOG.md)

## CLI

```bash
neverd info program.so
neverd headers --json program.so

neverd lift --dump-low program.so
neverd lift --dump-med program.so
neverd lift --dump-high program.so

neverd lift -o program.ll program.so
neverd decompile --language=c -o program.c program.so
neverd decompile --language=rust -o program.rs program.so

neverd lift --sbf-version=v2 program.so
neverd lift --sbf-relaxed --dump-low program.so
```

`--sbf-version=auto|v0|v1|v2|v3|v4` は検出 layout の ELF check 後に instruction
semantics だけを変更する研究 fixture 向け option です。不信な file を別 packaging
standard として再解釈するためには使えません。

## 解析と復元

LowIR は 8-byte encoding、raw field、LDDW continuation、resolved call、syscall hash、
block/edge、reachability、diagnostic を保持します。MedIR は version-specific encoding
を typed 32/64-bit operation、explicit extension、guarded arithmetic、memory width、
call kind に正規化し、register dataflow は constant と stack/rodata address を追跡します。

HighIR は entry/internal function、direct call edge、official syscall name、string、
natural loop、reducible conditional、conservative Solana observation を復元します。
`sol_invoke_signed_rust`/`sol_invoke_signed_c` は CPI、input register 基点 memory は
account/input access です。IDL なしに Anchor type/account layout を作りません。

C/Rust は backend-neutral structuring pass を共有します。一意な reducible 表現なら
`if`/`if-else`、`while`/`loop` を出力し、internal call、CALLX、irreducible flow は
exact PC dispatcher を残すため、可読性が semantics を変えません。

syscall database は logging、memory、PDA、SHA-256/Keccak/Blake3、Poseidon、
secp256k1、curve/alt-bn128、big modular exponentiation、CPI、return data、sibling
instruction、compute unit、epoch rewards 等を含みます。legacy relocations
`R_BPF_64_64`、`R_BPF_64_RELATIVE`、`R_BPF_64_32` は中央処理されます。text
relocation は LDDW 両 half と official Murmur3 CALL key を decode 前に適用します。
すでに `R_BPF_64_32` が適用・strip 済みなら symbol/target slot から registry key を
再計算して internal call を復元します。

## 生成 LLVM runtime contract

LLVM は VM address を host pointer にしません。checked load/store/syscall declaration
は `i32` status を返し、load/syscall value は output pointer に `i64` で書きます。
nonzero status は explicit SBF fault block に分岐し、module は
`llvm::verifyModule` を通過します。

## 生成 C host contract

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

`width` は bit 単位で、nonzero host return は explicit SBF status です。register、
return PC、callee-saved r6-r9、frame pointer、VM address、division fault、wide PQR、
wrapping shift を表し、使用 helper だけを出力するため
`clang -Wall -Wextra -Werror` を通ります。

## 生成 Rust host contract

```rust
pub trait SbfEnvironment {
    fn load(&mut self, address: u64, width: u8) -> Result<u64, SbfError>;
    fn store(&mut self, address: u64, width: u8, value: u64)
        -> Result<(), SbfError>;
    fn syscall(&mut self, hash: u32, args: [u64; 5])
        -> Result<u64, SbfError>;
}
```

出力は raw pointer のない safe stable Rust です。entry point は trait generic で、
register/call frame に fixed-size safe array を使い、test は
`rustc --edition=2021 -D warnings` で compile します。

## C API

SBF load 後も recovered function、disassembly、IR dump、CFG/call graph JSON、section、
symbol、relocation、string、header の session operation は共通です。Rust は ABI-stable
に追加された output-language enum で明示します。

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

## 検証と制限

`unittests/sbf/` は metadata invariant、v0-v4 loader、strict verifier、CFG/recovery、
verified LLVM、warning-free C/Rust compile、MedIR から独立した raw interpreter、public
C API を網羅します。conditional+loop fixture は両言語で実行して raw oracle と比較し、
official `sbpf` ELF corpus も vendoring せず local compatibility check に使います。

- SBF binary rewrite と object-code roundtrip は明示的に拒否します。
- Anchor IDL/type recovery と live RPC/account fetch は loader 範囲外です。
- generated source の syscall/VM memory は host contract 経由で、単独 runtime ではありません。
- relaxed mode は inspection 用で、invalid instruction に推測 semantics を与えません。

## 現在の conformance baseline（2026-08-10）

relocation 後は、VM address を持つ単一の immutable `ProgramImage` が decoder、
interpreter、string recovery、LLVM/C/Rust backend 共通の source of truth です。
loader semantics とずれ得る独立した text/rodata copy はありません。

閉じた record は `SBFVersions.def`、`SBFOpcodes.def`、
`SBFRelocations.def`、`SBFArgumentRegisters.def`、`SBFProtocolLimits.def`、
`SBFSyscalls.def`、
`SBFUpstreamSources.def` に置きます。一度しか使わない diagnostic と LLVM block
name は、LLVM 自身の方針どおり local に保ちます。

`SBFProtocolLimits.def` は旧来の 65,536 instruction と現在の 10 MiB account
data 上限を記録し、NeverD は後者から保守的な decode 上限を導出します。

strict v3/v4 では bounds-check 済み program header が runtime contract です。
section/symbol table は optional debug enrichment なので、欠落・破損しても有効な
image を無効にしません。legacy v0-v2 は `.text`、`.rodata`、`.data.rel.ro`、
`.eh_frame` を統合し、`R_BPF_64_64`、`R_BPF_64_RELATIVE`、`R_BPF_64_32`
を image の immutable 化前に一度だけ適用します。

| Evidence | 監査結果 |
|----------|----------|
| official ELF manifest | `sbpf/tests/elfs` の 20/20 artifact |
| ISA matrix | v0-v4 ごとに全 256 encoding、合計 1,280 cell と verifier boundary |
| differential execution | raw-byte oracle と LLVM ORC/C11/stable Rust の memory/fault/syscall trace 比較 |
| integrated aggregate | 13 test binary の 107/107 case |
| ASan + UBSan | 12 core binary の 101/101 case、report なし |

監査 pin は Anza `sbpf`
`71425d0de59e0bff048c6be8f4a8a9bc655916e2` と Agave
`cae40aa610fdbdb313209bc1eec737079eb59688` です。更新時は
`SBFUpstreamManifest.def`、`SBFUpstreamOpcodes.def`、
`SBFUpstreamSources.def` を確認して実行します。

```bash
NEVERD_SBPF_ROOT=$PWD/local_docs/sbpf \
  cmake --build build --target check-neverd-sbf
```

比較では `sol-azy` が現在の strict ELF で crash し、legacy CFG に undefined node
を残しました。`solana-data-reverser` は account data 向け、`SolDragon` は analysis
を WIP とし、`bn-ebpf-solana` は Binary Ninja を必要とします。したがって official
`sbpf` と Agave が semantic authority です。
