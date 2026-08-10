**語言**: [English](sbf.md) | [简体中文](sbf.zh-CN.md) | [繁體中文](sbf.zh-TW.md) | [日本語](sbf.ja.md) | [한국어](sbf.ko.md) | [Français](sbf.fr.md) | [Deutsch](sbf.de.md) | [Español](sbf.es.md) | [Italiano](sbf.it.md) | [Русский](sbf.ru.md) | [العربية](sbf.ar.md)

# Solana SBF 反編譯

[← 文件索引](README.zh-TW.md)

NeverD 將 Solana 部署產物視為一等 SBF 程式載入，並透過 CLI 與 `libneverd`
提供完整管線：

```text
SBF ELF
  → 版本感知 ELF loader 與 verifier
  → 無損 LowIR + CFG
  → 正規化 MedIR + 暫存器事實
  → 復原 function、syscall、CPI/account 觀察與 region
       ├─ 已驗證 LLVM IR
       ├─ 可攜 C11
       └─ 安全 stable Rust
```

實作遵循目前 Anza `sbpf` VM，不把 Solana 程式視為一般 Linux eBPF。version、
opcode、syscall、relocation 和 protocol metadata 集中於 `include/neverd/sbf/`
下的 `.def` database；loader/backend 使用生成 typed table，不重複 encoding 或拼字。

## 支援的輸入與 VM 版本

輸入為 ELF64 little-endian Solana 程式（`.so`）。支援目前 VM 的兩種 layout：

| SBF 版本 | ELF layout | Machine ID | 關鍵 ISA 行為 | 狀態 |
|----------|------------|------------|---------------|------|
| v0 | legacy section/relocation | `EM_BPF`、`EM_SBPF` | 固定 frame 加虛擬 gap、LDDW、legacy memory opcode | legacy |
| v1 | legacy section/relocation | `EM_BPF`、`EM_SBPF` | 手動調整 stack frame | legacy |
| v2 | legacy section/relocation | `EM_BPF`、`EM_SBPF` | PQR arithmetic、移動 memory encoding、互換 immediate subtraction、source-register CALLX | legacy、非單調 |
| v3 | strict program header、無 dynamic relocation | `EM_BPF` | static syscall/call、JMP32、destination-register CALLX，bytecode 在 `0x100000000`、rodata 在零 | 目前部署 toolchain format |
| v4 | strict program header、無 dynamic relocation | `EM_BPF` | v3 ISA 加 aligned memory-mapping contract | 目前上游 `sbpf`；cluster availability 可能不同 |

v2 變更刻意不延伸到 v3；feature check 採明確條件，不猜測 `version >= N`。預設
strict 拒絕畸形 header/range/alignment、不支援 writable legacy section、非法
continuation/register/frame-pointer write/branch，以及版本未啟用 opcode，並回報
instruction slot 與 virtual address。

目前 Solana toolchain 使用 `cargo build-sbf`。現代 v3+ production program 以 Rust
為主，上游 C toolchain 不產生 v3；這不限制 NeverD，任何接受的 SBF 輸入均可輸出 C/Rust。

持續更新的權威參考：

- [Solana 程式](https://solana.com/docs/core/programs)
- [程式執行](https://solana.com/docs/core/programs/program-execution)
- [Syscall 參考](https://solana.com/docs/core/programs/syscall-reference)
- [Anza sbpf VM](https://github.com/anza-xyz/sbpf)
- [Agave changelog](https://github.com/anza-xyz/agave/blob/master/CHANGELOG.md)

## CLI

```bash
# 檢視 machine、version、layout、VM address 和 section。
neverd info program.so
neverd headers --json program.so

# 檢視所有分析階段。
neverd lift --dump-low program.so
neverd lift --dump-med program.so
neverd lift --dump-high program.so

# 已驗證 LLVM IR。
neverd lift -o program.ll program.so

# C 和 Rust 都是一等 backend。
neverd decompile --language=c -o program.c program.so
neverd decompile --language=rust -o program.rs program.so

# 對研究 fixture 指定 VM contract，或保留畸形輸入供鑑識。
neverd lift --sbf-version=v2 program.so
neverd lift --sbf-relaxed --dump-low program.so
```

`--sbf-version=auto|v0|v1|v2|v3|v4` 只在 ELF 通過偵測 layout check 後改變
instruction semantics，用於損壞或研究 fixture；不可把不可信檔案重新解讀為另一封裝標準。

## 分析與復原

LowIR 保留每個 8-byte encoding、raw field、LDDW continuation、已解析 call、
syscall hash、block、edge、reachability 與 diagnostic。MedIR 將版本專用 encoding
正規化成 typed 32/64-bit operation、明確 immediate/result extension、guarded
arithmetic、memory width 與 call kind。register dataflow 追蹤 constant 和
stack/rodata address。

HighIR 復原 entry/internal function、direct call edge、官方 syscall name、string、
natural loop、reducible conditional 與保守 Solana observation。呼叫
`sol_invoke_signed_rust`/`sol_invoke_signed_c` 標成 CPI；以 input register 為基底
的 memory 標成 account/input access。不在無 IDL 時虛構 Anchor type 或 account layout。

C/Rust 共用 backend-neutral structuring pass。所有可達 block 有唯一 reducible 表示時，
直接輸出 `if`/`if-else` 和 natural `while`/`loop`；internal call、CALLX、irreducible
control flow 保留精確 PC dispatcher，使可讀性不改變執行語意。

syscall database 涵蓋 logging、memory、PDA、SHA-256/Keccak/Blake3、Poseidon、
secp256k1、curve/alt-bn128、big modular exponentiation、CPI、return data、sibling
instruction、compute-unit query 與 epoch rewards 等 sysvar。legacy relocation
`R_BPF_64_64`、`R_BPF_64_RELATIVE`、`R_BPF_64_32` 集中處理。text relocation
在 decode 前套用，包括 LDDW address 兩半與 legacy VM loader 的官方 Murmur3 CALL key。
若 artifact 已套用並 strip `R_BPF_64_32`，NeverD 由 function symbol 與 target slot
重算官方 function-registry key，保留 internal-call recovery。

## 生成 LLVM runtime contract

提升 LLVM 絕不把 VM address 視為 host pointer。checked load/store/syscall declaration
回傳 `i32` status；load/syscall 經 output pointer 寫 `i64`。非零 status 跳到明確 SBF
fault block。module 離開 backend 前通過 `llvm::verifyModule`。

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

`width` 單位為 bit。host 非零回傳成為明確 SBF status。生成 source 表達 register、
return PC、callee-saved r6-r9、frame pointer、VM address、division fault、wide PQR
operation 和 wrapping shift；僅輸出實際使用 helper，因此通過
`clang -Wall -Wextra -Werror`。

## 生成 Rust host contract

Rust 輸出為安全 stable Rust，以 trait 取代 raw pointer：

```rust
pub trait SbfEnvironment {
    fn load(&mut self, address: u64, width: u8) -> Result<u64, SbfError>;
    fn store(&mut self, address: u64, width: u8, value: u64)
        -> Result<(), SbfError>;
    fn syscall(&mut self, hash: u32, args: [u64; 5])
        -> Result<u64, SbfError>;
}
```

生成 entry point 對 trait generic，並以 fixed-size safe array 表示 register/call frame。
測試用 `rustc --edition=2021 -D warnings` 編譯代表性輸出。

## C API

載入 SBF 後，現有 session operation 不變：同步 recovered-function list、disassembly、
Low/Med/High/LLVM dump、CFG/call graph JSON、section、symbol、relocation、string、header。
以追加且 ABI-stable 的 output-language enum 明確選 Rust。

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

## 驗證與限制

`unittests/sbf/` 涵蓋 metadata invariant、v0-v4 loader fixture、strict verification、
normalization/CFG/recovery、已驗證 LLVM module、warning-free C/Rust compile、獨立於
MedIR 的 raw-bytecode interpreter 與 public C API。非平凡 conditional+loop fixture
在兩語言編譯執行並與 raw oracle 比較；官方 `sbpf` ELF corpus 也用於本機相容檢查，
但不 vendoring 第三方 binary。

明確限制：

- 明確拒絕 SBF binary rewriting 與 object-code roundtrip。
- Anchor IDL/type recovery 及 live Solana RPC/account fetch 不屬於 loader；可疊加在
  recovered address/call metadata。
- 生成 source 透過 host contract 暴露 syscall 與 VM memory，不是獨立 Solana runtime。
- relaxed mode 僅供檢查；invalid instruction 保持明確，絕不指派猜測 semantics。
