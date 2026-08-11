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

版本號本身不是規範，因此 `SBFVersionFeatures.def` 持有各項行為變更，由版本表將它們
組合起來。每筆記錄都帶上接納該變更的 SIMD 提案，以及 `anza-xyz/sbpf` 就同一問題
公開的述詞——因為多個提案會落在同一個版本裡，而一個提案又會改變彼此無關的多件事：
SIMD-0173 既搬遷了記憶體指令類，也淘汰了 `lddw`；SIMD-0174 則在同一版本中獨立加入
PQR 類。把提案記錄在特性上而不是版本上，才能讓復原出的版本結論一路追溯到決定它的
文件；這也是兩條 `callx` 規則被拆成兩個特性的原因：SIMD-0173 讀來源暫存器，
SIMD-0377 讀目的暫存器。

v2 變更刻意不延伸到 v3；feature check 採明確條件，不猜測 `version >= N`。預設
strict 拒絕畸形 header/range/alignment、不支援 writable legacy section、非法
continuation/register/frame-pointer write/branch，以及版本未啟用 opcode，並回報
instruction slot 與 virtual address。

## 描述所針對的執行環境

ISA 版本來自檔案本身，其他幾乎都不是。哪些 syscall 能解析取決於鏈與 slot；某個
account 欄位落在哪幾個位元組取決於擁有該程式的 loader；entrypoint 會不會收到第二個
引數取決於鏈撥下的一個開關；而一個程式能不能被部署，和它跑不跑得起來是兩回事。單一
的版本開關表達不了其中任何一項，因此這些是各自獨立的軸，各有各的表。

`SBFRuntimeFeatures.def` 記錄 cluster、用途，以及會改變 NeverD 回報內容的 gate，每筆
都帶有執行環境識別碼、其存在即代表開啟的 account，以及各 cluster 啟用它時所在的 slot。
某個 gate 若在某個 cluster 底下沒有對應列，就表示它在那裡尚未啟用。`simd-0321` 在每個
cluster 上都已開啟；`simd-0449` 與 SHA-512 syscall 在 testnet 與 devnet 上開啟、在
mainnet 上關閉，這正是一個在 devnet 跑得動的程式會在 mainnet 失敗的原因。

`SBFLoaders.def` 記錄歸屬與序列化。部署與執行早在多年前就不再是同一個答案：
`loader-v1` 與 `loader-v2` 會拒絕收到的每一條管理指令，同時繼續執行它們早已擁有的
程式，這正是它們的序列化至今仍必須可讀的原因。

| Loader | 序列化 | 可部署 | 可執行 |
|--------|--------|--------|--------|
| loader-v1 | `abi-v0` | 否 | 是 |
| loader-v2 | `abi-v1` | 否 | 是 |
| loader-v3 | `abi-v1` | 是 | 是 |
| loader-v4 | `abi-v1` | 否 | 否（內建程式已移除） |

`SBFAccountLayout.def` 標明每種序列化下每個 account 欄位的位置。兩者的差異不只在
padding——它們對欄位的排序也不同：在位移三的地方，未對齊形式放的是 account 位址的第
一個位元組，對齊形式放的卻是它的 executable 旗標，而數值本身完全不會宣告自己是從哪
一種讀出來的。重複出現的 account 在 `abi-v0` 佔一個位元組、在 `abi-v1` 佔八個位元組，
這會讓整趟走訪條目的過程錯位，而不只是錯開單一欄位。

一次呼叫能不能解析是三個問題而不是一個，因此 `SBFSyscallLifecycle.def` 保存已公布簽章
的確定程度，`SBFSyscallRegistration.def` 保存其餘部分：某個 syscall 出現在哪個
registry、由哪個 gate 管轄，以及那個 gate 指向哪一邊。方向很重要，因為 gate 拿走東西
和加上東西一樣容易——正是 `disable_fees_sysvar` 的啟用移除了 fees sysvar syscall——
把一個做減法的 gate 讀成做加法的，會一次把所有 cluster 的答案都反過來。
`sol_alloc_free_` 完全不需要 gate：執行環境仍持續兌現它，同時拒絕接受呼叫它的新程式，
這只是兩個 registry 之間的差別，而不是別的什麼。

在已啟用 `simd-0321` 的執行環境上，entrypoint 還會在 `r2` 收到 instruction data 的
位址。NeverD 把它模型化成一種自成一類的值而非常數，因為它落在哪裡取決於 account：
憑空捏造一個位址，會讓經由它的 load 被回報成某個具名的 account 欄位。啟用之前該暫存器
抵達時為零，讀取它的程式讀到的就是零。因此生成的 LLVM、C 與 Rust entry point 同時接收
input buffer 與 instruction data，因為一個無法被交付第二個引數的可呼叫體，也就無法重現
一個會讀取它的程式。

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

# 說明答案針對的是哪個執行環境。這些都不在程式檔案裡。
neverd lift --dump-high --sbf-cluster=devnet program.so
neverd lift --dump-high --sbf-slot=410400000 program.so
neverd lift --dump-high --sbf-loader=loader-v1 program.so
neverd lift --dump-high --sbf-purpose=deployment program.so
```

`--sbf-cluster`、`--sbf-slot`、`--sbf-loader` 與 `--sbf-purpose` 用來選擇執行環境
profile。預設值描述的是目前狀態下的 mainnet-beta、`loader-v3`，以及一個已經部署的
程式。改問部署，回報的就是哪些 syscall 會讓一個程式上不了鏈，即使鏈本身會繼續執行它。

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

## Solana 程式復原

在 SBF 機器模型之上，NeverD 報告一個程式作為 Solana 程式的意義。每筆記錄下來的
事實都附帶產生它的證據；位元組沒有決定的內容保持未設定，而不是猜測。

| 復原內容 | 證據 |
|----------|------|
| read-only 資料中的 base58 位址 | 命中 `SBFKnownAddresses.def`，或程式碼物化出的常數 |
| 程式自身宣告的位址 | 針對 read-only 常數、長度恰為一個 key 的 `sol_memcmp_` |
| Anchor instruction dispatch | 常數等於帶 namespace 的 SHA-256 discriminator 的 64-bit 比較 |
| CPI 目標 | 從 invoke 參數可達的 instruction 紀錄 |
| 一次呼叫選中的操作 | `SBFProgramInstructions.def` 中已列出的 selector，或開頭的 Anchor discriminator |
| PDA 種子 | 從 derivation 參數可達的 seed descriptor 陣列 |
| account 欄位讀寫 | 位址可證明落在 serialized input 內的 load/store |

loader 只傳一個參數，即 input region 起始處的 serialized input buffer，因此從該
entry state 出發的常數傳播會給出具名的 account 欄位而非裸 offset。
`SBFAccountLayout.def` 保存官方序列化布局，其固定欄位會被檢查為無空隙地鋪滿整個區間。

Anchor 以 SHA-256 對 `<namespace>:<name>` 求雜湊並保留前 8 位元組得到
discriminator，這是單向的。因此 NeverD 只做候選確認：`SBFAnchorNames.def` 是部署
程式中反覆出現的名稱字典，`--sbf-idl` 提供程式自身的 IDL 並優先。只有當其中至少一個
解析出名稱後，64-bit 比較才會被稱作 discriminator。

`SBFKnownAddresses.def` 記錄協定與正規程式位址；每個條目必須恰好解碼為 32 位元組，
測試會強制這一點。復原還需要 syscall ABI：SBPFv3 把 read-only 資料映射到虛擬位址 0，
於是長度參數與低位資料位址是同一個數值。因此 `SBFSyscalls.def` 記錄哪些參數暫存器
攜帶 VM 位址，只有這些會被追蹤。

兩個 invoke syscall 用兩種不同結構描述同一條 instruction，`SBFCPIABI.def` 依選中它
的 syscall 分別記錄兩套配置；用錯配置不會報錯，只會把第一個 account 當成被呼叫程式。
`SBFProgramInstructions.def` 再依各程式自己公布的 selector 命名操作：system、stake、
lookup-table 與 upgradeable-loader 用 bincode 變體序號，token 程式用首位元組，並在與
原 token 程式共用的編號之上疊加 Token-2022 的擴充區間。未列出的 selector 以數字回報。

### scratch 記憶體與 syscall 視窗

程式幾乎不會把常數直接交給 runtime：它在自己的 frame 或 heap 上組出 seed 陣列、
序列化 instruction 及其 payload，然後只傳一個指標。只讀映像只會看到指標而看不到它指
向的內容，因此復原維護一份只有本程式能寫的記憶體的位元組級模型，上限為
`kMaxModeledScratchBytes`。

呼叫之後還剩下什麼由兩張表決定。`SBFSyscalls.def` 說明哪些參數暫存器攜帶 VM 位址；
`SBFSyscallMemory.def` 說明 runtime 透過它們做什麼，即一次讀或寫，附帶 `Fixed`、
`Counted` 或 `Opaque` 的範圍。沒有寫視窗的 syscall 無法改動呼叫方的任何位元組，所以
`sol_log_` 之前證明的內容之後依然成立；由長度參數界定的寫只作廢該視窗；`Opaque` 寫
作廢其基底位址及其之上的部分，因為緩衝區不會向下延伸，也不會跨越 VM region 邊界。
`SBFSyscalls.def` 的效應摘要與該視窗表會雙向互校，任一方都無法單獨漂移。

`sol_memcpy_`、`sol_memmove_` 與 `sol_memset_` 會被跟進而不只是作廢：目的位址、長度
與來源都可證明時，目的位元組隨之已知。Anchor 程式的 payload 是複製到位而非直接映射，
正是這一步復原出它呼叫了哪個操作。

對本分析未描述的函式呼叫，則假定它會寫到一切可達之處。被呼叫方執行在自己的 frame 上，
因此當所有參數暫存器都可證明不指向 scratch 時模型得以保留，否則整體丟棄。
`sol_invoke_signed_rust` 與 `sol_invoke_signed_c` 寫的是 account data 而非呼叫方記憶
體，所以同一個 block 內組出的兩次呼叫都可讀。

該模型是函式內 CFG 上的前向 must 分析：只有當到達某 block 的每條路徑都寫入相同的值，
該位元組才會存活到這個 block。call 邊不跟進，因為被呼叫方不繼承呼叫方的 frame。
block 數超過 `kMaxScratchFlowBlocks` 的程式保留逐塊復原，只失去跨 block 的事實。

`SBFLints.def` 歸類整個程式的觀察：缺少 signer 或 owner 檢查、非常數的呼叫目標、已
棄用或受 feature gate 限制的 syscall，以及 SIMD-0500 將不再接受部署的 SBPF 版本。
每項都帶 severity 與 confidence，且 lint 從不改變已解碼的語義。這一層不做任何網路
存取。

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
/* 答案針對的是哪個執行環境。預設值描述的是目前狀態下的 mainnet-beta、loader-v3，
   以及一個已經部署的程式。 */
neverd_sbf_set_cluster(session, "devnet");
neverd_sbf_set_slot(session, 474768000);
neverd_sbf_set_loader(session, "loader-v3");
neverd_sbf_set_purpose(session, "deployment");
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

## 目前 conformance baseline（2026-08-10）

relocation 完成後，唯一且不可變、以 VM address 定址的 `ProgramImage` 是 decoder、
interpreter、string recovery 以及 LLVM/C/Rust backend 共用的事實來源；不再存在
可能與 loader semantics 漂移的獨立 text/rodata copy。

封閉資料表放在 `SBFVersions.def`、`SBFOpcodes.def`、`SBFRelocations.def`、
`SBFArgumentRegisters.def`、`SBFVersionFeatures.def`, `SBFProtocolLimits.def`、`SBFSyscalls.def`、
`SBFSyscallMemory.def`、`SBFCPIABI.def`、`SBFProgramInstructions.def` 與
`SBFUpstreamSources.def`。
只使用一次的診斷文字與 LLVM block name 仍留在 local，符合 LLVM 自身慣例。

`SBFProtocolLimits.def` 記錄歷史上的 65,536 條指令值與目前 10 MiB account data
上限；NeverD 從後者推導保守的 decode 上限。

strict v3/v4 以完成 bounds check 的 program header 作為 runtime contract；
section/symbol table 只是 optional debug enrichment，缺失或損壞不會否決有效 image。
legacy v0-v2 合併 `.text`、`.rodata`、`.data.rel.ro` 與 `.eh_frame`，並在 image
凍結前只套用一次 `R_BPF_64_64`、`R_BPF_64_RELATIVE` 與 `R_BPF_64_32`。

| 證據 | 稽核結果 |
|------|----------|
| 官方 ELF manifest | `sbpf/tests/elfs` 的 20/20 個 artifact |
| ISA matrix | v0-v4 各版本全部 256 個 encoding，共 1,280 個 cell，另含 verifier boundary |
| differential execution | raw-byte oracle 對 LLVM ORC、C11、stable Rust，比較 memory/fault/syscall trace |
| integrated aggregate | 14 個 test binary 的 145/145 個 case |
| ASan + UBSan | 13 個 core binary 的 141/141 個 case，無 report |

稽核固定於 Anza `sbpf`
`71425d0de59e0bff048c6be8f4a8a9bc655916e2` 與 Agave
`cae40aa610fdbdb313209bc1eec737079eb59688`。更新時請檢查
`SBFUpstreamManifest.def`、`SBFUpstreamOpcodes.def`、`SBFUpstreamSources.def`
後執行：

```bash
NEVERD_SBPF_ROOT=$PWD/local_docs/sbpf \
  cmake --build build --target check-neverd-sbf
```

對比顯示 `sol-azy` 會在目前 strict ELF crash，且 legacy CFG 保留 undefined node；
`solana-data-reverser` 聚焦 account data，`SolDragon` 將 analysis 標為 WIP，
`bn-ebpf-solana` 需要 Binary Ninja。因此 official `sbpf` 與 Agave 仍是 semantic
authority。
