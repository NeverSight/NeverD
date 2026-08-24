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
都帶有執行環境識別碼、以狀態記錄啟用資訊的 feature account，以及各 cluster 啟用它時所在
的 slot。pending account 可以已經存在而不啟用 gate；某個 gate 若在某個 cluster 底下沒有
啟用列，就表示它在那裡尚未啟用。`simd-0321` 在每個
cluster 上都已開啟；`simd-0449` 與 SHA-512 syscall 在 testnet 與 devnet 上開啟、在
mainnet 上關閉，這正是一個在 devnet 跑得動的程式會在 mainnet 失敗的原因。

在固定的 Agave revision 中，`syscall_parameter_address_restrictions` gate
（`simd-0459`）會收緊 syscall 與 CPI 參數的 VM 位址及對齊契約；finalized RPC 狀態記錄的
啟用 slot 分別是 mainnet 429,840,000、testnet 407,468,256 與 devnet 462,240,000。
`account_data_direct_mapping` gate 在採用調整後的位址空間時，會把 account data 從 input
buffer 中的副本改成由 account memory region 直接 backing；它在 mainnet 尚未啟用，
並在 testnet 408,332,256 與 devnet 463,968,000 啟用。這兩個 gate 都不會建立新的
Account ABI，也不會改變 ABIv0/ABIv1 的邏輯欄位偏移：序列化仍由 owning loader 決定，
NeverD 將兩者記錄為 runtime topology 中繼資料。

feature bit 維持 append-only。可觀測 snapshot 已超過 32 bit，因此
`RuntimeFeatureMask` 是 storage 與 host ABI 唯一採用的 `uint64_t` 型別。
v2 ABI 的寬度已凍結，不會 in-place 擴充；超過 64 bit 時應新增 v3 或 multiword 表示，絕不能改動 v2 寬度。
`RuntimeFeatureDisposition` 會明確區分仍存在的 `RuntimeBranch` 與
`FoldedBranch`：後者的啟用側在固定 revision 中已成為無條件行為，但舊側對歷史
slot 仍有意義。finalized RPC 啟用事實（`—` 表示尚未啟用）：

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

此處刻意不宣稱涵蓋 Agave 的整個 `FeatureSnapshot`。NeverD 只納入會直接改變解碼或
輸出 host contract 的 loader、verifier、VM、entry/input、syscall 與 CPI
infrastructure gate。transaction scheduling、fees、consensus、transaction-level
precompile verification 以及 `CPI target built-in` 業務語義由 `external runtime`
負責；若未實作這些 built-in 卻只加入 bit，會虛假宣稱 NeverD 並不具備的能力。

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
`sol_alloc_free_` 在邊界前後都始終註冊於 execution registry。deployment 在
`disable_deploy_of_alloc_free_syscall` 啟用前會註冊它，並從各 cluster 的啟用 slot
起拒絕它。固定的 Agave revision 已把啟用後的 deployment 行為折疊進 registry 建構；
NeverD 仍保留該 gate，讓歷史 profile 能得到啟用前的答案。

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
| read-only 資料中的 base58 位址 | 命中 `SBFKnownAddresses.def` 與 `SBFAnchorNamespaces.def`，或程式碼物化出的常數 |
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

scratch 復原採按需策略：只有存在真正的 `scratch consumer` 才建立 Solana CPI/PDA
scratch 的 fixed point；沒有該 consumer 的程式會跳過 `whole-CFG fixed point`。
`SBFAnalysisLimits.def` 定義的是主機 `analysis policy`，不是 `protocol limits`：
`MaxModeledScratchBytes` 為每個 `program point` 保留 1,024 bytes，
`ScratchFlowRetainedByteBudget` 是 8,388,608 bytes 的 `logical retained estimate`。
超過預算時，復原會明確 widening 為 `ScratchRecoveryPrecision::BlockLocal`。
只丟棄 `cross-block must-facts`；`block-local replay` 仍然 `sound`，並且仍可復原
`same-block stores`。printer 穩定輸出
`recovery scratch-precision=block-local`，widening 絕不回傳
`half-converged must-facts`。

呼叫之後還剩下什麼由兩張表決定。`SBFSyscalls.def` 說明哪些參數暫存器攜帶 VM 位址；
`SBFSyscallMemory.def` 說明 runtime 透過它們做什麼，即一次讀或寫，附帶 `Fixed`、
`Counted` 或 `Opaque` 的範圍。沒有寫視窗的 syscall 無法改動呼叫方的任何位元組，所以
`sol_log_` 之前證明的內容之後依然成立；由長度參數界定的寫只作廢該視窗；`Opaque` 寫
作廢其基底位址及其之上的部分，因為緩衝區不會向下延伸，也不會跨越 VM region 邊界。
`SBFSyscalls.def` 的效應摘要與該視窗表會雙向互校，任一方都無法單獨漂移。

`sol_memcpy_`、`sol_memmove_` 與 `sol_memset_` 會被跟進而不只是作廢：目的位址、長度
與來源都可證明時，目的位元組隨之已知。Anchor 程式的 payload 是複製到位而非直接映射，
正是這一步復原出它呼叫了哪個操作。

只有已解析的 runtime syscall 才可能保留 scratch，且必須嚴格遵循其已稽核的寫入視窗。
每個內部、間接或其他未解析呼叫都會清空已建模位元組——即使目前沒有參數指向
scratch——因為先前逸出的指標或全域別名仍可能讓被呼叫方改寫它們。
`sol_invoke_signed_rust` 與 `sol_invoke_signed_c` 寫的是 account data 而非呼叫方記憶
體，所以同一個 block 內組出的兩次呼叫都可讀。

該模型是函式內 CFG 上的前向 must 分析：只有當到達某 block 的每條路徑都寫入相同的值，
該位元組才會存活到這個 block。call 邊不跟進，因為被呼叫方不繼承呼叫方的 frame。依賴
worklist 沒有按 block 數降低精度的逃生閥；可選 Release 門會跑滿 10 MiB、`1,310,720` 條
指令的協定上限。

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

`width` 以 bit 為單位。所有生成的 C callback（包括 `syscall_with_features`）都回傳
`int`。在 v1 entrypoint `neverd_sbf_program` 中，0 表示成功；`load` 或 `store` 的任何非零
回傳都會歸一為 `NEVERD_SBF_MEMORY_ACCESS`，`syscall` 的任何非零回傳都會歸一為
`NEVERD_SBF_UNKNOWN_SYSCALL`；對應契約標記為 `v1-load-store-nonzero` 與
`v1-syscall-nonzero`，v1 不透傳 callback 的 exact status。內部
`InvalidRegister` 與 `InvalidBranch` fault 也會歸一為
`NEVERD_SBF_INVALID_INSTRUCTION`（`internal-invalid-instruction`）。
v2 entrypoint `neverd_sbf_program_v2` 才是 exact status 路徑：已識別的
`neverd_sbf_status_v2` callback 值（包括 9 與 10）會作為已處理 fault 保留
（`v2-exact-status`）。v2 entrypoint
也會將內部 `InvalidRegister` 與 `InvalidBranch` 保留為 9 與 10。未知 callback 值使用生成器
針對該 operation 的 fallback（`operation-specific-fallback`）。
`syscall_with_features` 為 null 時回退到 `base.syscall`，其 callback 同樣回傳 `int`
（`feature-aware-null-base-syscall`）。
v1 struct 與 entrypoint 仍兼容 legacy host。應使用獨立的 v2 entrypoint 取得
`syscall_with_features` 與已解析的 runtime-feature snapshot。生成 source 表達 register、return PC、
callee-saved r6-r9、frame pointer、VM address、division fault、wide PQR operation 與 wrapping
shift。僅輸出程式實際使用的 helper，因此最小輸出也能通過 `clang -Wall -Wextra -Werror`。

## 生成 Rust host contract

Rust 輸出為安全 stable Rust，以 trait 取代 raw pointer：

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

舊 entrypoint `neverd_sbf_program` 與 `SbfEnvironment` 構成
`v1-result-abi`，host method 使用 `Result`。`Some(SbfRuntimeFeatures::from_bits(0))`
表示 `explicit-empty-snapshot`，與 `None` 不同。`syscall_outcome` 是從基於
Result 的 host method 到 `SbfSyscallOutcomeV2` 的 `result-host-bridge`。
由於 `SbfErrorV2` 標有 `#[non_exhaustive]`，呼叫方在 match 時必須使用
`non-exhaustive-wildcard`（`_`）。

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
/* Optional: name Anchor handlers from the program's own IDL. */
neverd_sbf_set_idl(session, idl_json);
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

## 目前 conformance baseline（2026-08-24）

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
| 官方 ELF manifest | `sbpf/tests/elfs` 的 23/23 個 artifact |
| official oracle | `NeverDSBFExternalOracleTests` 將 1,411 個 opcode/verifier boundary case 與獨立建置的 pinned verifier 對照 |
| differential execution | raw-byte oracle 對 LLVM ORC、C11、stable Rust，比較 memory/fault/syscall trace |
| integrated aggregate | `check-neverd-sbf` 執行所有已登錄 suite；不固定快速變動的總 case 數 |
| ASan + UBSan | focused target 以 fail-fast sanitizer 設定執行且無 report；不固定快速變動的總 case 數 |

稽核固定於 Anza `sbpf`
`2510663bb8d894e8e3094be351e4bb4b604f1f84` 與 Agave
`ef210d67f2fabeee1730498188fa78854260c679`。更新時請檢查
`SBFUpstreamManifest.def`、`SBFUpstreamOpcodes.def`、`SBFUpstreamSources.def`
後執行：

```bash
NEVERD_SBPF_ROOT=/path/to/sbpf \
NEVERD_AGAVE_CONFORMANCE_ROOT=/path/to/firedancer-test-vectors \
NEVERD_AGAVE_CONFORMANCE_REVISION=68bb4af40235562e8852fa23d5727e49c2a0b862 \
  cmake --build build --target check-neverd-sbf
```

對比顯示 `sol-azy` 會在目前 strict ELF crash，且 legacy CFG 保留 undefined node；
`solana-data-reverser` 聚焦 account data，`SolDragon` 將 analysis 標為 WIP，
`bn-ebpf-solana` 需要 Binary Ninja。因此 official `sbpf` 與 Agave 仍是 semantic
authority。

## 2026-08-24 可重現證據契約

`SBFUpstreamSources.def` 將稽核固定於 Anza `sbpf`
`2510663bb8d894e8e3094be351e4bb4b604f1f84`、Agave
`ef210d67f2fabeee1730498188fa78854260c679` 與 Solana SDK
`122f32e571ce39face4beffaccea733e37c207fd`。官方 manifest 全部 23/23 通過；
`NeverDSBFExternalOracleTests` 經 `SBFOfficialOracleProtocol.def` 與
`SBFOfficialVerifierCases.def` 與 `SBFOfficialExecutionConstants.def`，將 1,411 個 opcode/verifier boundary case 和獨立建置
的官方 verifier 對照。畸形 ELF 來自 `SBFOfficialELFMutations.def` 與 table-driven
corpus；總數仍會演進，因此文件不猜測也不固定匯總數。
另有獨立的 `41-case strict ELF 差分比對`：將完整 strict-v3 mutation matrix 同時
送入官方 `verify-elf-batch` process 與 NeverD。這 41 個 case 不計入 1,411 個
opcode/verifier case 總數。

官方額外執行矩陣（`additional execution matrix`）是獨立的：正好包含 508 個
active `(Version,Opcode)` case 與 58 個 boundary case，共 566 個 exact execution
case。它不會取代 1,411 個 `verifier probes`，也不計入其中；同樣不取代或計入
41-case strict ELF 差分比對。

`NeverDSBFAgaveConformanceTests` 也會驗證 Firedancer test-vectors revision
`68bb4af40235562e8852fa23d5727e49c2a0b862`，並逐項比對全部 1,955 個
`sol_compat_elf_loader_v1` fixture（1,399 個接受、556 個拒絕）。對每個接受的 ELF，
它也比對 `entry_pc`、`text_off`、`text_cnt`、`rodata_hash` 與 `calldests_hash`。此 gate
刻意只檢查 loader，不執行後續 instruction verifier，以免混淆 Agave 的兩個階段。

預設 chain profile 忠實描述 Agave：`SBF_RUNTIME_VERSION` 表依歷史 cluster/slot
查詢官方 feature account activation，使最大 ISA 依序從 V0 推進至 V1、V2、V3；目前
最大值仍為 V3。這屬於 `RuntimeVersionPolicy::ChainProfile`。只有明確
`--sbf-version=v4` 才選擇
`RuntimeVersionPolicy::UpstreamToolchain`，依 pinned `sbpf` 做專家級 offline 分析；
這不代表 v4 已在鏈上啟用。目前 10 MiB 上限精確為 `10'485'760` byte；65,536 僅
保留為歷史 provenance/test 資料，不作為執行或解碼限制。

feature、syscall、fault 與 source ABI 均由 typed `.def` registry 統一定義：
`SBFSyscallRegistration.def`、`SBFValidationRules.def`, `SBFFaultCodes.def`、
`SBFSourceStatuses.def`、`SBFArgumentRegisters.def`、`SBFEdgeKinds.def`。
`SBFFaultCodes.def` 固定 execution fault 的穩定值，`SBFSourceStatuses.def` 則獨立
擁有 generated-source host ABI。loader
採 `raw-first`：先修復 relative CALL，再依 ELF ordinal 將 raw relocation 精確套用
一次；穩定錯誤順序為 text identity、CALL、relocation、entrypoint、read-only layout。
file offset 與 VM address 的 mapping 是 gap-aware，不會在空洞虛構 byte。

CFG 與 dataflow 依 function 隔離：call edge 不會成為同一 frame 的 predecessor；
shared tail 保持 ambiguous；同一自然迴圈的所有 latch 合成一個 multi-latch region。
worklist 與扁平 ownership 以 10,000 個 function、逆序 block、conditional latch fixture
守門，只約束可擴展性與完成性，不猜特定機器的秒數。

公開 SBF call graph 採用 `callgraph-budget=fail-closed`：typed input、
provenance、node、edge、element 與 `CallGraphOutputByteBudget` 讓 JSON
嚴格為完整結果或空結果。預算耗盡時回傳 `{"nodes":[],"edges":[]}`，
同時設定 `neverd_last_error()`；永不發佈部分 relation。

每條 activation row 都記錄 cluster、feature account 與 slot，因此可對 live node 做
`RPC activation audit`，一般分析仍完全 offline。競品稽核涵蓋 Blueshift、`qedsvm`
（能為指定 bytecode path 產生 Lean 證明，但目前 ELF loader 僅接受 V0）、
`leanprover-solanalib`、`sol-azy`、`bn-ebpf-solana` 與 Ghidra/SolDragon。
`ezBPF` 在 `88829078a6d7682a2baed0d696d500401c263750` 明確標註自身已 deprecated，
並指向 Blueshift；它是採用單一 byte-to-enum 對映的 archived predecessor，並不是理解
moved-memory、JMP32 與目前 v0-v4 矩陣的 version-aware decoder。在本次已稽核
比較 pin 為 Blueshift `704e40f7aa82446555b19d9ffbc0a6e18a35480f`、`qedsvm`
`99bd5ede85374adc7fc5c835c2432ecf4e123fd1`、`leanprover-solanalib`
`6c115ef1ef6a0cde8dbd6fd875b7dc87d60939ec`；四個本地工具固定為 `sol-azy`
`362327a798e5dad6e12aa9abf3ed9ed52c17ef6a`、`solana-data-reverser`
`bf90923adec984a61ca0437e9d341360ac1b11ee`、`SolDragon`
`002b98677a5e595a773af6607b77210f5ea71db7` 與 `bn-ebpf-solana`
`c3fe0de45d37eb68dcb08f2498c6e1f986056572`。在本次已稽核
的公開通用 SBF 反編譯器範圍內，NeverD 擁有我們找到的最強可重現證據；這是有邊界
的比較結論，不是絕對或永久的「世界第一」宣稱。

公開競爭工具稽核也納入 `r2ghidra-solana`（固定在
`eca0b8e2d307e00991e289b8f9b0f45743819f1b`）：它提供 Ghidra C-like UX、
`C-like-pdg` 以及 account/Anchor/string/syscall 檢視；該 pin 的 CI 已通過，
但 Solana 專用 testsuite 被註解，CI smoke 只反編譯 `/bin/ls`。直接重現也確認：
官方 V0 的 `relative_call_sbpfv0.so` 能產出合理 C，而官方 V3 的
`relative_call.so` 會在 `pdg` 失敗；結果可重現。`radare2-solana` 固定在
`292d845681be377cadc9959a74c2cadeb6e7f412`，會把
V2-only 的 SIMD-0173/0174 以 `>=V2` 擴到 V3/V4，而官方 `program.rs` 明確它們
只屬於 V2。`SBPF-3-1` 固定在 `0e602c93007faa96bccb8e1e12040954ff108b6f`，
只有 2/2 個簡單 cargo test、沒有 CI；version detection 仍是回傳 none/V0 的
placeholder，high-nibble opcode decoder 錯誤，jump 使用 imm 而非 off。V0/V3 的
relative_call ELF 也產生相同錯誤的 pseudocode。NeverD 的優勢是可重現的官方
V0–V4 loader/verifier/runtime/process-oracle evidence；這不否定這些工具的 UX 或 C output。

`SBFComparisonTools.def` 是競品顯示名稱與完整 revision 的唯一權威。最後一次有邊界的
公開掃描另得到以下結論：

- `blastrock/Solana-eBPF-for-Ghidra` 固定於
  `c3ad719004726fe924dbed901eca2744ad82c85d`，有真正的 Ghidra P-code UX，但只有一個
  不分版本的 SLEIGH model，CALLX 固定取 `dst`，且混用 legacy/current opcode；沒有
  真實測試或 CI，預設 source 也缺少被引用的 relocation constant class。
- `SolEmu-Ghidra` 固定於 `6520af2ff104d5adbec24632ba3afa3bef0da529`，繼承逐字相同的
  decoder，並在明確模擬或 placeholder 的 CPI、密碼學與 ZK 行為外增加 emulator UI；
  同樣沒有真實測試或 CI。`Ghidra_sBPF` 固定於
  `907bd4476432ca83bb2352686ad1ccafdb38504c`，可手選 v1-v3，卻把 V2-only encoding
  累積進 V3，沒有 V0/V4 自動選擇，也沒有測試或 CI。
- `solana-ebpf-ida-processor` 固定於
  `aacd215907266190ed9c6c1b408ca9309f92ecdd`，是有用的 IDA disassembler/relocation UI，
  不是 source lifter；其混合 opcode table 總從 `imm` 讀取 CALLX，且沒有測試或 CI。
  `solana-bpf-reverse` 固定於 `39479a3bddb8cb866ee499266a76a1b54069b222`，從硬編碼
  layout 猜測產生 heuristic report 與 Rust TODO scaffold；實跑為 9 pass、2 fail、1 skip，
  沒有 CI。
- `solens` 固定於 `22defa1c8f4118dacd42f5c291f1ac31609fc0e5`，是 V2-only terminal
  disassembler，測試數為 0，也沒有 CI。`sbpf-decompiler` 固定於
  `37b8bc0edc7ce347abee466f5f974e900c1948df`，目前實作只有三行
  `Hello, world!`，測試數為 0，也沒有 CI。
- `sbpf-eye` 固定於 `5277a52aeb58e50b6ff8f9020414334765369b49`，明確是 lightweight
  WIP instruction/CFG TUI；3 個測試通過，但沒有 semantic IR、source emitter 或 CI。
  `svm_bytecode_analyzer` 固定於
  `12aa236db8964e6be661e38131c2dc81588cf19c`，是 disassembler/CFG analyzer 而非 lifter；
  register/offset byte 解碼錯誤，實跑 17 pass、1 fail，也沒有 CI。
- `giraffexiu/Solana-eBPF-for-Ghidra` 固定於
  `81c1e3c2b9ba35091e4a2d8bb6eb23fd59339f07`，只是同一 Ghidra 血系的單 commit
  snapshot，沒有新增版本語義、測試或 CI。`CertSBF` 固定於
  `bb93a97cf0c64d119d08ec851e8e820315beb59e`，是有價值的舊 rBPF Isabelle/HOL
  形式化，不是目前 V0-V4 整體程式 source decompiler。

這些結果只加強本次限定公開樣本中的比較證據，不是對未來工具或私人專案的絕對結論。

2026-08-24 最終 RPC audit 完全吻合：38 個 feature accounts、89 條 activation
rows；mainnet slot 為 441305159，testnet 為 433055669，devnet 為 487238699。
system-owned 的空 pending account（mainnet 的 `VirtualAddressSpaceAdjustments`）
未啟用。文件不硬編碼 RPC URL。

Linux Release CI 透過 `--print-pinned-revision`、`--print-test-vectors-revision` 與
`--print-toolchain` 讀取 exact pin，建置官方 oracle、驗證 sparse corpus，並匯出
`NEVERD_SBPF_ORACLE` 和 `NEVERD_AGAVE_CONFORMANCE_ROOT`，因此兩個 external test
都在 CI 中強制執行。一般本機執行若未提供明確的 oracle/corpus env，仍會發現這些
case，但允許 skip。
