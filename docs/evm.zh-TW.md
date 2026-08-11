**語言**: [English](evm.md) | [简体中文](evm.zh-CN.md) | [繁體中文](evm.zh-TW.md) | [日本語](evm.ja.md) | [한국어](evm.ko.md) | [Français](evm.fr.md) | [Deutsch](evm.de.md) | [Español](evm.es.md) | [Italiano](evm.it.md) | [Русский](evm.ru.md) | [العربية](evm.ar.md)

# EVM 反編譯

[← 文件索引](README.zh-TW.md)

NeverD 可載入傳統以太坊虛擬機位元組碼，建立專用的 256 位元 LowIR、堆疊 SSA
MedIR 與復原後的 HighIR，並輸出 LLVM IR、C23 或 Solidity。預設採嚴格分析：
未分配的操作碼或在所選硬分叉中尚未啟用的操作碼，會在其精確 PC 位置報錯。

Solidity 與 C 輸出屬於語意重建：它們保留解碼後的操作碼順序、256 位元算術、堆疊
檢查與已驗證的控制流程，但不宣稱還原合約原始的原始碼、識別字或型別。

## 快速開始

```bash
# 使用 i256/i512 值提升為已驗證的 LLVM IR。
./build/bin/neverd lift contract.evm -o contract.ll

# 檢視各層 EVM 分析表示。
./build/bin/neverd lift --dump-low contract.evm
./build/bin/neverd lift --dump-med contract.evm
./build/bin/neverd lift --dump-high contract.evm

# 輸出兩種支援的原始碼語言。
./build/bin/neverd decompile --language=c contract.evm -o contract.c
./build/bin/neverd decompile --language=solidity contract.evm -o contract.sol

# 選擇歷史操作碼集合，或保留未知操作碼為明確故障節點以供鑑識。
./build/bin/neverd decompile --language=solidity \
  --evm-hardfork=cancun --evm-relaxed contract.evm
```

`disasm`、`cfg` 與 C API 的 Low/Med/High/LLVM 查詢也接受 EVM 輸入。EVM 二進位
重寫會被明確拒絕；`patch` 仍只用於原生二進位。

## 可接受的輸入

| 輸入 | 辨識與正規化 |
|------|--------------|
| 原始位元組 | `.raw`、`.evmraw`，或具有明確 EVM 副檔名的二進位內容 |
| 十六進位文字 | 可選 `0x`、任意 ASCII 空白；支援 `.evm`、`.hex`、`.bin`、`.bytecode`，也會偵測通過驗證且無副檔名的十六進位文字 |
| 編譯器產物 | `.json` 根節點或 `evm` 下的 `deployedBytecode`、`runtimeBytecode`、`bytecode`；也支援 `contracts → file → contract → evm` 形式的 solc 標準 JSON |

執行期/部署位元組碼優先於建立位元組碼。若只有建立程式碼，NeverD 會辨識有界、常數
的 `CODECOPY`/`RETURN` 建構子包裝並擷取被複製的執行期切片。建構子巡訪使用與真正解碼
器相同的單指令解碼器，並以正在分析的硬分叉為準，因此某個位元組在一個分叉是資料、在
另一個分叉是操作碼時，也無法移動這條邊界。只含可選 `0x` 前綴的產物欄位視為空，因此
空的 `deployedBytecode` 或 `runtimeBytecode` 不會遮蔽可用的建立位元組碼回退。

### 編譯器尾端資料

`EVMMetadataFields.def` 列出兩種尾端格式。Solidity 寫入一個 CBOR map，其最後兩個位元
組只計算該 map 本身；`vyper` 寫入一個以該 map 結尾的 CBOR array，其最後兩個位元組計
算含自身在內的整段尾端。把其中一種取框方式當成另一種來讀不會大聲失敗——它只會落在偏
離兩個位元組之處，並削掉兩個位元組的真實程式碼——因此兩種都會嘗試，兩者皆不符的輸入
則原樣保留。

尾端會被讀取兩次：一次針對給定的輸入，一次針對剝除部署包裝後剩下的執行期程式碼。
Vyper 把尾端搬進了 initcode，執行期程式碼不再留有尾端，因此只在剝除之後才查看的讀取
器，會把一個自報名號的合約回報成未知的建置。sequence footer 另外載明執行期程式碼長
度、data section 長度與 immutables 長度，因此無須執行建構子即可界定回傳的程式碼。

### 不是指令的容器

`EVMBytecodeContainers.def` 在任何解碼之前先為輸入分類。自 EIP-3541 使 `0xEF` 不可
部署之後，開頭的 `0xEF` 就等於承諾這些位元組不是指令：

| 容器 | 標記 | 處置方式 |
|------|------|----------|
| legacy | — | 以指令解碼 |
| delegation（`eip-7702`） | `0xef0100` 且恰好 23 位元組 | 回報目標帳戶；分析停止 |
| eof（`eip-3540`） | `0xef00` | 拒絕；尚無分叉啟用它 |

delegation 指示子的二十個位元組是一個位址，而不是程式碼。解碼它們等於把位址當成操作
碼來讀，並產出一個帳戶的控制流程圖，因此 `info` 會回報目標，分析則帶著理由拒絕。這次
拒絕會區分兩種情況：Pectra 之前該標記尚未指派，自 Pectra 起則是目標的執行期程式碼根
本不存在。長度不是此值的標記屬於格式錯誤的輸入，而非該容器的一種變體，因此仍當作指令
處理，好讓解碼器能指出它讀不了的那個位元組。

格式錯誤的十六進位、奇數位數、未解析 linker placeholder、有歧義的多合約產物、
無效 metadata 邊界與正規化後的空程式碼都會產生可操作錯誤。C++ loader API 可用
`BytecodeLoadOptions::ArtifactContract` 選取 `Contract` 或
`path/File.sol:Contract`。多個來源檔定義同名合約時，未限定名稱會被拒絕，避免產物
順序靜默選錯位元組碼。

EVM 註冊在 NeverD 核心 loader registry，而非隱藏在 backend plugin 後。因此 CLI、
C API、反組譯器、CFG builder 與 Low/Med/High/LLVM 查詢都收到同一正規化映像與
EVM 選項，入口間不會產生格式辨識或語意分析漂移。

## 硬分叉與操作碼

傳統操作碼從 Frontier 到 Fusaka 全部涵蓋，包括 `PUSH0`、暫態儲存、`MCOPY`、
blob 操作碼與 `CLZ`。預設 `latest` 指向 Fusaka。可接受名稱為：

```text
frontier, homestead, dao-fork, tangerine-whistle, spurious-dragon,
byzantium, constantinople, petersburg, istanbul, muir-glacier, berlin,
london, arrow-glacier, gray-glacier, paris, shanghai, cancun, pectra,
fusaka, amsterdam, bogota, latest
```

也接受 `dao`、`tangerine_whistle` 等底線拼法、`merge`、`prague` 與 `osaka`。
目前 `latest` 和 `osaka` 都解析為規範的 `fusaka` 執行版本。

`latest` 是 NeverD 已實作的最新主網最終版本，而非 Ethereum 開發分支頂端。
Ethereum 將 [Glamsterdam](https://ethereum.org/roadmap/glamsterdam/) 描述為預計於
2026 年第四季的升級；仍在 Review 階段的
[SLOTNUM](https://eips.ethereum.org/EIPS/eip-7843) 與
[DUPN/SWAPN/EXCHANGE](https://eips.ethereum.org/EIPS/eip-8024) 僅在選擇
`--evm-hardfork=amsterdam`（或 `bogota`）時啟用，最終確定前不會進入 `latest`。
EIP-8024 只消耗合法立即數；非法候選仍是下一條指令，缺失位元組的語義值為零。

EOF 不屬於 Fusaka：Ethereum 在
[Fusaka checkpoint 2](https://blog.ethereum.org/2025/04/29/checkpoint-2) 移除了它，
execution-spec-tests 亦記錄 EOF 已
[自 Osaka 移除且未重新排程](https://github.com/ethereum/execution-spec-tests/blob/main/docs/CHANGELOG.md)。
NeverD 不會把已撤回的 EOF 提案當作最終主網行為。

嚴格模式拒絕未知及分叉中未啟用的位元組。`--evm-relaxed` 會保留它們於 LowIR 與
診斷中，但執行抵達時生成 backend 仍會故障；寬鬆模式絕不靜默視未知位元組為 NOP。

## LLVM 風格的 metadata 架構

手工維護的 EVM metadata 採用 LLVM 可多次 include 的 `.def` 模式：

- `EVMOpcodes.def` 是 150 個已定案操作碼與 4 個 opt-in 開發操作碼的唯一事實來源：
  編碼、實際 pop/push 變化、立即數種類、類別、啟用分叉、主要 effect、正交 EVM
  記憶體存取、原始碼層狀態存取、
  call-value 存取與終止屬性全在同一筆記錄，新增操作碼不會靜默繼承預設值。
- `EVMMemoryAccesses.def`、`EVMStateAccesses.def`、
  `EVMCallValueAccesses.def` 定義封閉且具名的屬性域。屬性具型別且彼此正交：`CALL`
  同時是外部呼叫和記憶體讀寫，`EXTCODECOPY` 同時讀取 context 並寫入記憶體。
  狀態存取採明確 `None/Read/Write/Unknown` 格，而非可能有非法組合的兩個布林值。
  payability 也是獨立約束：`CALLVALUE` 的型別化讀取屬性使讀取 `msg.value` 的區域
  輸出為 `payable`，不會錯標為 `view`。分析器另辨識規範的
  `ISZERO(CALLVALUE)` 非 payable guard；僅確認其非零分支以 `REVERT` 結束時，
  才忽略這次編譯器生成的讀取。
- `EVMImmediateKinds.def` 定義固定寬度 PUSH data 與 EIP-8024 條件 single/pair
  encoding；`EVMDecodeStatuses.def` 統一 LowIR 與 disassembly 公開的穩定狀態詞彙。
  `EVMUpstreamOpcodePolicy.def` 記錄 go-ethereum naming alias 及有意排除的歷史/已撤回
  項目；`scripts/audit_evm_opcode_metadata.py` 會拒絕 byte drift 和任何未經 review 的
  upstream 新常數。
- `EVMHardforks.def`、`EVMEffects.def`、`EVMExitStatuses.def` 和
  `OutputLanguages.def` 生成有序 enum、parser、顯示名稱、CLI 選項與 C ABI 值。
- `EVMCalls.def` 描述呼叫另一個程式的四條指令，以及被呼叫位址來源的 lattice。
  每筆記錄只有一個旗標——value operand 是否位於被呼叫者與引數 window 之間——由它
  推導出之後每一個 operand 位置；該表會與 opcode 資料庫交叉驗證，使推導不會偏離
  已宣告的 pop 數。
- `EVMPrecompiles.def` 是協定自身回應的位址字典，每項都帶有保留該位址的 fork，以及
  排定它的提案。`0x100` 上的 `P256VERIFY` 歸於 `eip-7951`：它才是隨 Fusaka 在主網上
  保留該位址的 Final 提案；其介面所出自的 rollup 提案從未排程。其中刻意不含 gas：
  precompile 的成本是其輸入的函式，且在位址與操作皆未改變的情況下被多次重新定價。
- `EVMMetadataFields.def` 與 `EVMBytecodeContainers.def` 描述一個輸入在被解碼之前
  究竟是什麼：兩種編譯器尾端取框方式，以及那些位元組根本不是指令的容器。
- `EVMRecoveredFacts.def` 擁有復原事實各詞彙的拼寫，使出現在輸出中的名稱集中於
  一處，而不是散落在可能遺漏新列舉項的 `switch` 中。`EVMKnownSignatures.def`
  對 signature 的三種角色做同樣的事。
- `EVMConstants.h` 統一管理協定寬度、限制與穩定預設名稱。
- `Semantics.h` 管理與目標無關的 scalar ALU evaluator。常數折疊與 interpreter 使用
  同一套已檢查 `APInt` 實作；LLVM、C、Solidity 保持明確 target lowering，使
  backend 契約與不支援情況可見。

decoder 是原始位元組邊界。操作碼身分與硬分叉啟用狀態刻意分離：寬鬆解碼保留已分配
但在歷史分叉未啟用指令的名稱、引入分叉與立即數寬度，同時讓語意查詢保持保守並故障。
因此帶立即數的未啟用指令不會改變後續邊界或意外取得目前語意。分析、解譯與所有
emitter 使用生成的 `Opcode` enum 和 metadata query；原始編碼只在 trace、host
callback 等位元組 ABI 邊界再現。`SWAP16` 有 17 個邏輯堆疊輸入，最大非堆疊 host
操作有 7 個參數；兩個上限分別於編譯期推導。

`OpcodeInfo` 無法預設建構成半有效記錄，其名稱為 `llvm::StringLiteral`，不會產生
懸空 `StringRef`。編譯期驗證器拒絕重複編碼、未知類別/屬性、無效 scalar ALU
契約、effect/狀態存取不一致、PUSH/DUP/SWAP/LOG family 契約錯誤，以及未標成基本塊
terminator 的 branch。它也拒絕有多於一個 pushed result 的非堆疊操作（共用 host
ABI 只回傳一個 word），並在專用 lowering 尚未存在時拒絕未知 stack-family 操作。
寬鬆解碼只能經由明確的未知位元組 factory 取得保守故障 metadata。

這些 `.def` 是手寫資料庫，類似 LLVM
[`Instruction.def`](https://github.com/llvm/llvm-project/blob/main/llvm/include/llvm/IR/Instruction.def)。
EVM 子系統只把 `.inc` 用於真正生成或 literal include fragment（例如 TableGen
輸出），不把手工資料庫偽裝成生成產物；target-language template 留在 C/Solidity
emitter。周邊 C++ 遵循 LLVM [編碼標準](https://llvm.org/docs/CodingStandards.html)，
包括公共邊界的 LLVM ADT/string type 和 fail-loud、窮盡的 semantic switch。

這與 LLVM 自身分層一致：小型手寫 X-macro database 用 `.def`；豐富 declarative
record 用 `.td`，再由 [TableGen](https://llvm.org/docs/TableGen/ProgRef.html) 生成
C++ 消費的 `.inc`。NeverD 尚無 TableGen generation step，因此提交沒有 source
generator、只看似生成的 EVM `.inc` 只會增加形式負擔。

新增操作碼時，加入一筆完整 `EVM_OPCODE`，再補上適用的共享 scalar semantics、
明確 backend lowering 與聚焦測試。新增硬分叉時，加入一筆有序 `EVM_HARDFORK`
和所需 alias。typed API、lookup table、validation、classification 和 CLI 值會一起
擴充；backend semantic switch 維持明確，遺漏 ALU case 時立即失敗。

## 分析模型

- **EVM LowIR** 保留 PC、編碼、型別化立即數狀態與解碼後的堆疊深度操作元（包括 PUSH
  截斷時右側補零及 EIP-8024 條件消耗規則）、基本塊、前驅/後繼 edge、已驗證
  `JUMPDEST` target、可達性與堆疊高度 domain。CFG 復原採確定性的全程式 fixed point：
  每個堆疊 slot 傳播一組有界的 256 位元有限值，每個具體高度保留一個 abstract stack。
  因此，跨 internal-call/return 基本塊攜帶的常數、stack shuffle、`PC`/`CODESIZE` 與
  scalar ALU operation 能解析一個或多個具體 jump target；真正未知的 target 則保留為
  明確 indirect edge，不會被猜測。

  `AnalyzeOptions::MaxAbstractValuesPerSlot` 限制每個 finite value set，超限時將 slot
  widening 為 `Unknown`；`MaxStackHeightVariants` 限制一個基本塊內與路徑相關的高度數量，
  超限會回報明確的 analysis-limit error，而不是截斷 CFG。兩個限制均拒絕零值。非關聯
  stack merge 後的 Cartesian operation 會將 finite value 標為 over-approximation：
  無效 candidate 會被診斷，但 strict analysis 不會僅因 slot correlation 遺失就拒絕
  bytecode；精確的無效 target 仍會在對應 jump PC 失敗。relaxed mode 會診斷 stack fault，
  並只終止出錯的 abstract path，不會虛構 fault 後的 fallthrough。
- **EVM MedIR** 將每個堆疊值表示為 256 位元 SSA value，先連接所有 merge phi，再執行
  確定性的 sparse constant worklist。私有 lattice 為 `Uninitialized`、一個精確
  `Constant` 或 `Overdefined`：相同常數可跨基本塊及有 anchor 的 phi cycle 傳播，衝突或
  依賴 runtime 的 cycle 則不能虛構常數。worklist 會檢查 def-use ID，並使用與 interpreter
  相同的 `Semantics.h` ALU evaluator。MedIR 亦保留主要 semantic effect，以及正交的
  `none/read/write/readwrite` EVM-memory access、source-level state access 與 call-value
  access。多高度 LowIR stack 在此邊界保守地以堆疊頂端對齊；某些 incoming height 不存在
  的 slot 會成為明確 unknown value，並由確定性 diagnostic 記錄精確度損失。
- **EVM HighIR** 復原 Solidity dispatcher selector、可能的 calldata/return word、
  mutability、常數 storage slot、LOG/event 與 revert 事實及 function/CFG region。經檢查的
  producer index 與 iterative、memoized value walk 從型別化 MedIR operand 復原事實，
  不再依賴指令距離：selector comparison 可跨基本塊與 phi，支援 `EQ` 任一 operand order，
  並保留推導出的 32 位元 mask；argument offset、storage key、event topic0、
  non-payable/receive guard 與精確 32-byte return size 均使用其 semantic input。iterative
  walk 由 MedIR graph 提供結構邊界，將 malformed、mixed 或 cyclic expression 視為 unknown。
  同一 selector 的 conflicting target 會被診斷並省略。payability 與 state-access lattice
  維持獨立，可達但未解析的 dynamic jump 會強制保守的 `nonpayable` recovery。在 MedIR
  尚無 memory SSA 前，custom-error payload recovery 與對外 call 的 payload recovery
  是僅存的兩項 bounded instruction-window heuristic；復原的名稱與型別仍明確屬於 heuristic。

  HighIR 同時記錄 interface 的對外一半：每一條 `CALL`、`CALLCODE`、`DELEGATECALL`
  與 `STATICCALL`，包含被呼叫者的來源、當所分析的 fork 在該位址上有保留時它所命名的
  保留位址、呼叫寫入被呼叫者 calldata 開頭的 selector，以及轉帳金額為常數時的取值。
  `CREATE` 與 `CREATE2` 被排除在外：它們執行的程式碼尚無位址，因此沒有可復原的被呼叫者。

  復原出的對外 signature 絕不會計入程式自身回應的標準集合。送出
  `transfer(address,uint256)` 說明程式使用了某個代幣，而非說明它本身是代幣；混淆兩者
  會把每一個 router 與 vault 都報告為 ERC-20。delegate 呼叫會額外記為一條 proxy 事實，
  因為它是該族中唯一讓被呼叫者程式碼執行在本程式自身 storage 上的成員。

  precompile 查找以所分析的 fork 為準，而不是以現存最新的 fork 為準。呼叫某個由後續
  fork 引入的 precompile 位址，實際到達的是一個沒有程式碼的帳戶，呼叫會成功且不回傳
  任何內容，因此為其命名等於報告一個程式確鑿未曾執行的操作。
- **LLVM** 輸出通過 verifier 的 `i32 @evm_execute(ptr)` state machine，包含已檢查
  1024-word `i256` stack、`i512` modular intermediate、有 guard signed division、
  saturated shift、精確 `BYTE`/`SIGNEXTEND`/`CLZ` 與驗證過的 dynamic-jump switch。

內建確定性 interpreter 是測試的 semantic oracle。LLVM 與生成 C 編譯執行後與它
比較；生成 Solidity 編譯、部署至 Anvil，並比較可觀察 storage 與 trace。另有
pre-Fusaka raw-bytecode corpus 在 Anvil 原生 EVM 執行，涵蓋 scalar ALU、calldata
複製、重疊 `MCOPY`、memory expansion、Keccak 與 returndata，提供獨立 client 對照。

帶 account 參數的操作碼依[執行規範](https://github.com/ethereum/execution-specs/blob/master/src/ethereum/forks/osaka/vm/instructions/environment.py)
把堆疊 operand 遮罩為協定的 160 位元 address；公開 environment value 和 map 會先
驗證，避免錯誤 `APInt` width 觸發 LLVM assertion。`BLOCKHASH` 亦執行前 256 區塊窗口。

interpreter 將 EIP-211 每 frame returndata buffer 與目前 frame 最終輸出分離：
`RETURNDATASIZE`/`RETURNDATACOPY` 觀察最近 subcall buffer，僅 `RETURN`/`REVERT`
寫入 `ExecutionResult::ReturnData`。因此 call 後 `STOP` 不會錯誤暴露 callee bytes。
CREATE/CREATE2 遵循同一規則：建立失敗壓入零並以 EIP-211 buffer 暴露 revert bytes；
成功壓入設定 address 並清除 buffer。`InitialReturnData` 只是 snapshot/test seed。

## 生成 C 的契約

C 輸出使用 C23 擴充整數，避免算術截為 64 或 128 位元：

```c
#define NEVERD_EVM_WORD_BITS 256u
#define NEVERD_EVM_WIDE_WORD_BITS (2u * NEVERD_EVM_WORD_BITS)
typedef unsigned _BitInt(NEVERD_EVM_WORD_BITS) evm_word;
typedef signed _BitInt(NEVERD_EVM_WORD_BITS) evm_sword;
typedef unsigned _BitInt(NEVERD_EVM_WIDE_WORD_BITS) evm_wide;
```

純操作、堆疊操作和控制流程直接輸出；依賴環境的操作呼叫：

```c
evm_word neverd_evm_host_op(
    struct neverd_evm_env *environment,
    uint8_t opcode,
    evm_word a0, evm_word a1, evm_word a2, evm_word a3,
    evm_word a4, evm_word a5, evm_word a6);

void neverd_evm_trace(
    struct neverd_evm_env *environment, uint64_t pc, uint8_t opcode);
```

參數採 EVM pop 順序，`a0` 是原堆疊頂。callback 於 `environment` 實作 memory、
storage、calldata、hash、block context、call、log 與 halt side effect；回傳第一個
pushed value，未用參數為零。每條解碼指令前呼叫 `neverd_evm_trace`。

以 frontend 至少支援 512 位元 `_BitInt` 的 Clang target 編譯：

```bash
clang -std=c2x -ffreestanding -c contract.c
```

Apple Darwin Clang target 目前上限不足。macOS 應使用具備能力的非 Darwin target
驗證原始碼，或直接使用 NeverD LLVM 輸出；Linux Clang 支援所需寬度。

## 生成 Solidity 的契約

Solidity 輸出提供兩個互補視圖：

1. 供稽核閱讀的 selector-specific function、storage、event、error 宣告；
2. 保留精確算術與控制流程、且檢查 PC/stack 的狀態機。

復原的 constant storage fact 以具名絕對 slot constant 輸出，例如
`recovered_storage_slot_3 = uint256(0x3)`，不會偽裝為循序 Solidity state variable
並虛構 storage layout。

合約刻意宣告為 `abstract`。覆寫 `_evmHost` 以實作環境操作：`args_[0]` 是原堆疊頂，
回傳值是第一個 pushed result，亦可更新 storage 或其他 state。`_evmTrace` 為 virtual，
預設發出 `EVMTrace`。此邊界明確呈現環境假設，不編造無法從位元組碼復原的 Solidity。

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

`neverd_decompile_all` 維持向後相容並輸出 C。新 EVM-aware 入口是
`neverd_session_bitness`、`neverd_evm_set_strict`、`neverd_evm_set_hardfork`、
`neverd_decompile_all_ex`。原生二進位要求 Solidity 會得到明確 unsupported-language
錯誤。舊 LLVM-to-C routing flag 對 EVM 會被拒絕，不會靜默忽略：C 使用專用 C23
backend，驗證 LLVM IR 使用 `lift`/LLVM query API。native object round-trip API
也明確拒絕 EVM；LLVM IR 可供分析，但 NeverD 不假裝 native object target 有 EVM ABI。

## 明確限制

- 僅支援傳統位元組碼，尚不解碼 EOF container。
- Amsterdam/Bogota 是明確的開發 target；在計畫操作碼最終確定前，`latest` 仍選擇
  已定案的 Fusaka 指令集。
- 無 RPC 擷取、鏈狀態探索、gas 計量/退款或 precompile 執行。call 和環境值以確定性
  interpreter field 或 backend host hook 表達。
- 建立程式碼擷取僅辨識常見靜態 wrapper，不是完整 constructor transaction emulator。
- dynamic jump 維持明確 indirect CFG edge，除非有界 constant analysis 證明有效
  `JUMPDEST`；可達未解析 jump 也讓復原原始碼 mutability 保守。
- ABI type、source name、mapping、event、自訂 error 是 best-effort recovery fact；
  NeverD 不宣稱原始碼同一性。
- 使用 memory、storage、calldata、call、log、hash 或 blockchain context 的合約若要
  獨立執行，必須實作 C/Solidity environment hook。
