**語言**: [English](../architecture.md) | [简体中文](../zh-CN/architecture.md) | [繁體中文](architecture.md) | [日本語](../ja/architecture.md) | [한국어](../ko/architecture.md) | [Français](../fr/architecture.md) | [Deutsch](../de/architecture.md) | [Español](../es/architecture.md) | [Italiano](../it/architecture.md) | [Русский](../ru/architecture.md) | [العربية](../ar/architecture.md)

[← 文件索引](README.md)

# NeverD 架構

本指南說明貢獻者安全修改 NeverD 所需理解的生產邊界。內容刻意只涵蓋
NeverD 自有程式碼；LLVM、Capstone 與 Unicorn 子模組各自維護內部架構。

## 系統邊界

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

NeverD 有四種 IR 表示，但它們並非一條必須通過四跳的序列。`LowIR -> MedIR`
是共享部分；結構化反編譯接著使用 `MedIR -> HighIR -> C`，而 `lift`、
`decompile --llvm` 與 `patch` 則直接走 `MedIR -> LLVM IR`。尤其 patch 與
lift 模式會刻意略過 HighIR。

CLI 在 `tools/neverd` 解析命令、建立 `neverd_session_t`，並呼叫
`include/neverd/sdk/NeverDCAPI.h` 中的公開 API。引擎狀態位於
`lib/sdk/SessionImpl.h`；`neverd_session_load` 選擇 loader 並建立
`BinaryImage`，以 IR 為基礎的操作則按需執行 `lib/pipeline/Pipeline.cpp`。
`neverd` 可執行檔連結 `neverd_shared`；元件歸檔及其 LLVM/Capstone 相依是
該共享程式庫的私有實作細節。CLI 使用 LLVM Support 建立命令列介面，
但不會繞過 C API 驅動引擎。

HighIR 的 `HighSourceFlow` 統一管理輸出敘述的控制流邊、區域變數身分和確定賦值分析。
原始碼驗證與無用 PHI 複製消除共用此圖。重複的純量條件採用有界的零值/非零值分割；
寫入使舊條件失效，位址逸出的變數維持未知，超出分割預算時退回保守圖。
只有純量值在所有可行情境中都不會被使用時，才刪除 PHI 複製。
呼叫、載入及儲存保留可觀察行為，原始碼標籤也會保留。

Block 使用方的逃逸分析也使用這張圖，以有界不動點分析跨分支和迴圈傳播指標身分及私有堆疊槽資訊。合流保留可能的上下文位址，只有完整覆寫才能清除。未知跳躍、例外控制流和證明預算耗盡都會拒絕繫結。

Objective-C 接收物件事實區分方法入口的 self 與確定的類別參照；只有共用入口的所有中繼資料記錄一致時，才建立 self 型別事實。完整寬度複製及 ABI 規定保留的暫存器透過同一不動點分析傳播事實，入口回邊也參與合流。宣告檢查區分類別方法與實例方法，並納入已記錄的分類、父類別鏈及協定繼承；入口 self 也考慮已知子類別宣告。編譯器目錄分別保留宣告所屬物件與繼承關係，以及全域選擇器一致性資訊。發布原始碼前，SDK 依據目前映像重新驗證接收物件來源及適用宣告。這些事實不選擇具體 IMP，也不授權二進位重寫。
外部繼承資訊缺失時，必須改用全域選擇器一致性檢查，不能縮小到接收物件範圍；明確不支援或衝突的宣告仍保留為否定證據。

帶明確物件型別的成員變數可透過最多八次完整寬度讀取延伸接收物件證明。每一步記錄執行期偏移槽、偏移值寬度，以及機器存取使用的固定位元組偏移。固定偏移必須仍符合目前配置；執行期偏移參照則可隨欄位移動。載入器核對已記錄的類別繼承鏈與欄位宣告。部分讀取、裸 id、Block、僅協定型別、歧義儲存和未知指標基址都不能產生類別事實；原始碼驗證會依據目前映像重新檢查整條路徑。這些事實描述宣告型別，不代表物件身分，也不允許刪除記憶體操作。

載入器驗證 Darwin 常數字串、整數物件、陣列和有序字典組成的有界無環圖。容器欄位與每條邊都需要不可變映射儲存，以及無歧義的匯入或重定位證據；不支援的編碼、循環和不完整物件圖會明確失敗。原始碼繫結重新驗證物件圖及傳入指標槽。產生的輔助函式保留整數位元模式、子物件順序和共用位址，並重用既有字串身分。容器槽透過 acquire/release 發布機制只初始化一次，初始化僅呼叫已驗證的子物件輔助函式。每個輔助函式自帶子函式宣告，使獨立還原的方法可以共用同一定義。可攜式測試涵蓋損壞輸入與證明預算；原生編譯器樣例對照原始方法檢查內容、別名、複製身分和並行初始化。

## IR 表示與路徑

| 表示 | 用途 | 主要定義與轉換 |
|------|------|----------------|
| LowIR | 架構無關的 `NdOp` 操作、基本區塊、CFG 與跳躍表中繼資料 | `include/neverd/ir/low`、`lib/ir/low`，由 `lib/decode` + `lib/lift` 產生 |
| MedIR | 型別、ABI/呼叫慣例、記憶體與堆疊模型、旗標、呼叫和類 SSA 資料流 | `include/neverd/ir/med`、`lib/ir/med` |
| HighIR | 用於可讀 C 的結構化運算式與控制流 | `include/neverd/ir/high`、`lib/ir/high`，由 `lib/backend/c/HighC` 發射 |
| LLVM IR | 最佳化、LLVM 衍生 C、目標程式碼產生和二進位重寫輸入 | `lib/backend/llvm`，由 `lib/pipeline` 最佳化/編排 |

常數在 LowIR、MedIR 和 HighIR 中保留每次出現時的純量/位址來源及位址歸屬。數值位元相同不會合併不同來源。HighIR 符號簡化將位址身分視為不透明輸入；原始碼繫結使用共用的數值運算元分類，並仍要求記憶體與指標使用完成重定位繫結。

| 使用者路徑 | 表示路徑 | 出口 |
|------------|----------|------|
| Low/Med dump | Binary -> LowIR，可選 -> MedIR | 診斷文字 |
| High dump 或 `decompile` | Binary -> LowIR -> MedIR -> HighIR | HighIR 或結構化 C |
| `lift` | Binary -> LowIR -> MedIR -> LLVM IR | `.ll` |
| `decompile --llvm` | Binary -> LowIR -> MedIR -> LLVM IR | LLVM 衍生 C |
| `patch` | Binary -> LowIR -> MedIR -> LLVM IR -> codegen | 重寫後的二進位 |

`lib/pipeline/Pipeline.cpp` 是路徑選擇的事實來源。特定表示的邏輯應留在其
所屬 IR 或 backend 程式庫；pipeline 應編排元件，而不是吸收其演算法。

## 跨架構翻譯契約

`include/neverd/translate` 定義的是契約層，而不是執行後端。`GuestState`
為 `x86_32`、`x86_64`、`AArch64` 與 `ARM32` 建模架構無關的機器可見狀態。
其規範的版本 1 序列化採用固定寬度的小端欄位、穩定的暫存器 ID、排序集合與
失敗即關閉的驗證，因此持久化狀態不依賴主機 C++ 配置。

`GuestState` 的 wire v1 基線永久凍結。基線以外的機器狀態只能使用擴充區間內的
extension-register ID，並搭配規範的小寫名稱；否則必須採用新的 wire 版本並提供
明確的 upgrader，禁止就地變更 v1 基線。

對於 `ARM32` guest，`ExecutionMode` 是權威解碼模式，且必須與 `CPSR.T` 一致。
儲存的 PC 一律是清除 bit 0 後的規範指令位址；ARM 模式還要求按字對齊。

架構對策略定義 `x86_64 -> AArch64`、`AArch64 -> x86_64`、
`x86_32 -> AArch64/ARM32` 與 `ARM32 -> x86_32/x86_64`。
`ContractDefined` 表示請求可以驗證並持久化，不表示程式碼已能翻譯或執行。
JIT 策略只接受執行中程序的原生主機；AOT 策略則要求明確提供主機架構、目標
triple；若選擇了 CPU 或特性集合，也必須明確提供。

`ResolvedHostTarget` 將此選擇解析為具體結果。`Native` 解析從目前程序取得 triple、
CPU 以及啟用/停用的特性集合；`Explicit` 解析會驗證並正規化呼叫端提供的架構、
triple、CPU 與特性，並拒絕互相衝突的輸入。其帶版本的快取識別按確定的位元組順序
由正規化目標輸入建構，不包含程序位址或依賴 locale 的文字。

帶版本的 `TranslationExit` 記錄穩定的停止原因及其對應的型別化承載資料，涵蓋
系統呼叫、例外或訊號、斷點、不支援的指令、自我修改、資源預算、外部呼叫、
記憶體錯誤及其他終止條件。使用者不必再依停止原因重新解讀無型別整數。

除與對應預算相符的 `BudgetExhausted` 外，結果回報的指令數、block 數與產生程式碼量
都不得超過請求中的對應非零預算。指令與 block 耗盡會精確停在 limit。產生目標檔案
大小只能在不可分割的 codegen 完成後精確量測，因此該預算耗盡結果可以回報
`Observed > Limit`；被拒絕的目標檔案絕不會被連結、發布或執行。每個
`BudgetExhausted` 承載資料都必須精確識別請求的 limit，不得回報推導值或實作私有門檻。

backend-private `RuntimeControlBlockV1` 契約固定為 128 位元組、8 位元組
對齊，並以固定的 v1 magic、version、size、欄位偏移、全零保留欄位與自洽的型別化
退出記錄加以約束。它不包含 C++ 容器、主機指標或 guest 位址別名，也不是
`GuestState` 的 C++ 配置或 wire 格式；實作該契約的後端必須明確將狀態轉換到此記錄。

固定的 v1 generated-code 呼叫面只包含八個 helper：
`nvd_rt_v1_load8_le`、`nvd_rt_v1_load16_le`、`nvd_rt_v1_load32_le`、
`nvd_rt_v1_load64_le`、`nvd_rt_v1_store8_le`、`nvd_rt_v1_store16_le`、
`nvd_rt_v1_store32_le` 與 `nvd_rt_v1_store64_le`。名稱、簽章與指標 provenance
必須精確相符；後端必須明確綁定此有限表，絕不能退回環境符號解析。可執行記憶體
generation 驗證與預算/取消輪詢只由受信任 dispatcher 執行；
`nvd_rt_v1_validate_generation` 與 `nvd_rt_v1_poll` 均不是 generated-code helper。
受信任主機 dispatcher 也負責選擇 block，產生 IR 不能呼叫它；translated block
只回傳型別化退出碼。產生 IR 只能直接讀取已宣告的 scalar-result runtime slot。

`RuntimeSymbolRegistryV1` 將此 helper 表實作為封閉的主機端登錄表。建構過程會驗證
完整的 ABI-v1 集合、精確的正規名稱、helper class、簽章，以及每一項唯一一個非空且
與 class 相符的函式指標。查找只接受精確名稱，絕不查詢程序環境或動態載入器的符號，
並向目標檔 verifier 提供同一組已排序名稱作為 allowlist。其帶版本的識別涵蓋名稱、
helper class 與 ABI 形狀，但刻意排除原生位址，因此不受 ASLR 影響。

`RuntimeCodeMemory` 管理逐頁隔離的產生程式碼儲存空間，只允許單向 `RW -> RX` 發布
轉換。記憶體不會同時可寫與可執行，發布後也不能重新開放寫入；寫入與進入點偏移都
經過邊界檢查，發布時還會清除主機指令快取。本機 smoke test 只在發布後執行一小段
主機指令；它證明的僅是此 W^X 記憶體邊界，而不是翻譯引擎。

`GuestMemoryRuntime` 與邏輯 `GuestState` 隔離：建構時先驗證狀態，再將記憶體區域
的位元組與中繼資料複製到排序的私有索引。guest 虛擬位址只作為查找鍵，絕不轉換
為主機指標。受檢純量存取會以型別化形式回報寬度、對齊、溢位、未映射、跨區域、
權限、可執行寫入、generation 溢位、generation 不符與策略錯誤。指令/block 預算、
取消、generation 追蹤，以及 `RejectExecutableWrites`、
`InvalidateOnExecutableWrite`、`ValidateBeforeDispatch` 三種程式碼寫入策略同樣產生
自洽的型別化記錄，而非隱式主機行為。

`TranslationObjectCompilerV1` 是經過驗證的 LLVM IR 到目標檔邊界。它先驗證 const
輸入 module，在任何轉換前完成 clone，將證明閘控的語意簡化與 LLVM `O0` 至 `O3`
最佳化組合，再次驗證最終 IR，並為四種契約主機架構發射 relocatable ELF、COFF 或
Mach-O 目標檔。它正規化精確的 target-mangled block/runtime 符號 manifest，稽核每個
發射結果，並回傳 runtime registry identity 以及帶版本的請求與成品 cache key。產生
位元組預算非零時，只有符合預算的目標檔才能繼續進入成品驗證。LLVM 先向私有緩衝區
完成一次不可分割的發射以取得精確大小；超限目標檔會在發布與成品稽核前被拒絕，型別化
遙測保留實際大小與請求的精確 limit。零表示呼叫端政策不設上限。編譯器止於已稽核的
relocatable 位元組：不負責連結、發布、分派或執行，也不提供 guest 指令 lowering。

post-codegen verifier 將 relocatable ELF、COFF、Mach-O 目標檔視為
閉集合稽核。格式與架構必須和選定主機精確相符；未定義符號必須精確屬於有限
helper allowlist，動態符號一律禁止。relocation 採用明確直接白名單，並檢查
encoding、width、alignment、offset、可載入目的區段，以及目標是否為目標檔內
non-preemptible 定義或精確獲准的 helper。verifier 拒絕 W+X、例外/展開與初始化
中繼資料、TLS、IFUNC、GOT 與一般 PLT 間接機制、動態 relocation、weak/preemptible
或可選擇定義、未知 allocated section 與 linker directive。只有當 v1 policy 證明
LLVM 隱藏的 x86-64 ELF `R_X86_64_PLT32` 是指向精確 runtime helper 的 sealed direct
branch 時才允許此拼寫；它不會放行 PLT 或 GOT 路徑。ELF `ET_REL` 成品不得包含
program header 或 segment。Mach-O load command 採用正向白名單：必須且只能有一個
位寬相符的 segment，symbol table、dynamic-symbol table、platform-version 與
data-in-code command 各至多一個，並檢查相依關係；linker option 與其他所有 command
均拒絕。

`TranslationObjectRequestV1` 是建立在上述契約上的第一個公開、且刻意收窄的
guest 位元組到目標檔切片。在目前發布的失敗封閉 x86-64 v1 純量暫存器子集中，它只
接受不含 legacy prefix 的 canonical 編碼：採用受支援暫存器/立即數 LowIR 形狀的
REX.W 全寬 GPR `MOV`、`ADD`/`SUB` 與 `AND`/`OR`/`XOR`。schema 9 也接受全寬
暫存器/暫存器 `CMP` 編碼 `39/3B`、暫存器/立即數 `CMP` 編碼 `81/7`、`83/7` 與
`3D`、暫存器/暫存器 `TEST` 編碼 `85`，以及暫存器/立即數 `TEST` 編碼 `F7/0` 與
`A9`。算術形式保留相應的純量 flags 計算；邏輯形式與 `TEST` 會計算架構定義的 flags，
並在 NeverD 狀態模型中保留 `AF`。canonical `C3` `RET` 與
`C2 iw` `RET imm16` 會終止返回 block；canonical `EB cb` 與 `E9 cd` 直接相對 `JMP`
編碼會終止直接分支 block。目前公開 lowering schema 為 9。canonical、無 legacy prefix
的傳統 Jcc 僅支援以下形式：`JO`/`JNO` 的短形式 `70/71 cb` 或近形式 `0F 80/81 cd`；
`JB`/`JAE` 的 `72/73 cb` 或 `0F 82/83 cd`；`JE`/`JNE` 的 `74/75 cb` 或
`0F 84/85 cd`；`JBE`/`JA` 的 `76/77 cb` 或 `0F 86/87 cd`；`JS`/`JNS` 的
`78/79 cb` 或 `0F 88/89 cd`；`JP`/`JNP` 的 `7A/7B cb` 或 `0F 8A/8B cd`；
`JL`/`JGE` 的 `7C/7D cb` 或 `0F 8C/8D cd`；`JLE`/`JG` 的 `7E/7F cb` 或
`0F 8E/8F cd`。`JRCXZ`/`JECXZ`/`JCXZ` 與 `LOOP`/`LOOPE`/`LOOPNE` 仍未發布，
並以 fail-closed 方式拒絕。保留的 `F7 /1`、guest-memory 運算元、部分暫存器形式、
legacy prefix 與語意冗餘的 REX 擴充位元同樣以 fail-closed 方式拒絕。輸出僅限經過稽核的
little-endian AArch64 ELF 或 Mach-O relocatable 目標檔。一般 guest 記憶體操作、部分
暫存器形式、該精確子集外的任意指令或編碼、返回、這些直接跳轉與上述已發布 Jcc
分支以外的控制流，以及 lowerer 尚未實作的任何 LowIR 操作，都會在目標檔產生前遭到
拒絕。`RET` 所需的受檢回傳位址讀取屬於其 terminator 契約的內部
行為，並不公開通用 guest 記憶體 lowering。請求會重建並驗證 block descriptor，
lowering 與目標檔產生共用同一個已解析 target machine，並將證明閘控的語意簡化與
LLVM 預設 `O2` 最佳化流水線組合。
此切片不代表支援其他 x86-64 指令、其他 guest/host 組合，或反向 AArch64 到 x86-64
翻譯。

公開 C 入口 `neverd_translate_x86_64_block_to_aarch64_object_v1`、Python ctypes wrapper
`translate_x86_64_block_to_aarch64_object` 與 `neverd translate-object` 命令公開同一個
僅產生目標檔的邊界。Python 使用 `TranslationObjectFormat.ELF` 或 `.MACHO`；原生程式庫
回報的翻譯失敗會拋出帶有 `TranslationErrorCode` 的型別化 `TranslationError`，本機
參數驗證則拋出 `TypeError` 或 `ValueError`。成功時回傳由 Python 擁有的不可變結果。
C 結果
擁有目標檔位元組、穩定 cache identity 與最佳化遙測；CLI 只寫出所選的 ELF 或
Mach-O 目標檔。這些 C、Python 與 CLI 物件介面都止於連結、載入、分派、執行與除錯
之前；它們不是執行 session 介面。

`verifyTranslationLinkGraphV1` 增加第二道獨立的 allocation 前稽核。它從已接受的 AArch64
ELF 或 Mach-O 目標檔建立暫時的 LLVM JITLink graph，並檢查 target、section 權限、
block/runtime 符號 manifest、外部符號閉包，以及 edge 類型與目標。產生不含位址的
稽核結果後即銷毀 graph。通過此稽核不等於連結、配置、解析、載入、發布、分派或執行
程式碼。

`linkTranslationObjectV1` 是獨立的原生連結邊界。它會在裁剪、配置、符號解析與 fixup
前後重新稽核受信任 descriptor、原始物件及 JITLink graph。runtime 符號只能來自 sealed
登錄表。dispatcher credential 將唯一的 manifest 條目綁定至其 session、block identity、
guest 入口 PC、cache generation 與 code epoch；呼叫時 runtime guest `RIP` 也必須符合
該入口。成功 finalize 後以最終權限發布可執行記憶體；unload 會撤銷新呼叫，並等待一個
進行中的呼叫結束後才釋放 allocation。無 credential 的 overload 仍僅供稽核，不能呼叫。

`NativeTranslationSessionV1` 將這些元件組合成實驗性的 C++ x86-64 到原生 AArch64
執行邊界。在 little-endian AArch64 ELF 或 Mach-O 程序上，它於 compile-link-validate-
invoke-unload dispatcher 迴圈中跨 block 保留同一個受檢 guest-memory runtime 與固定
guest state。canonical 直接跳轉會在其精確靜態目標繼續執行。已發布的 canonical
Jcc 分支只能在 block manifest 宣告的 taken 或 fallthrough successor 繼續；dispatcher
拒絕其他任何選定 PC。返回會終止執行。全域指令數、block 數與產生物件位元組數預算在
多個 block 間保持精確；guest 成功停止時，已執行狀態與權威記憶體會一起提交。取消操作
與最終提交線性化。

這是一個可執行的縱向切片，而不是完整翻譯器。它尚不支援一般 guest-memory 指令、
部分暫存器、上述精確 schema-9 傳統 Jcc 切片以外的條件控制流（包括
`JRCXZ`/`JECXZ`/`JCXZ` 與 `LOOP`/`LOOPE`/`LOOPNE`）、間接控制流、
呼叫、浮點、SIMD、x87、原子操作、系統指令、
通用例外傳播、block cache、其他 guest/host 架構組合或反向 AArch64 到 x86-64。執行
session 目前沒有 C、Python、CLI 或 JSON 介面，除錯仍是獨立且不受支援的能力。上述
物件 API 不需啟用原生執行仍可單獨使用。

產生 IR 的契約要求受該契約約束的每個 translated block 都是 hidden、non-preemptible，
並採用 C ABI `i32 (ptr state, ptr runtime)`。runtime 只能透過私有登錄表發現 block，
不得依賴程序環境的符號查找；禁止 block 之間直接呼叫。

IR verifier 也會把整數寬度限制在主機純量暫存器寬度以內，以避免 legalization
引入已知的 compiler-runtime libcall。這項檢查只是必要條件：任何實作該契約的執行
後端都必須依同一個有限的 runtime-symbol allowlist，精確稽核 post-codegen 控制轉移、
`MachineIR` 與目標檔 relocation。

TranslationIR 的直接 load/store，以及 private constant 儲存的值，只能包含單一、
不寬於主機純量暫存器寬度的純量整數。聚合值必須在 verifier 邊界前完成純量化，
避免緊湊 IR 觸發後端無界展開。

generated-code ABI 只為純量整數定義。浮點、SIMD、x87、原子操作與系統指令均在
該契約之外。選擇 `ProvenSemanticAndLLVM` 策略的實作必須執行 NeverD 現有的證明
閘控語意簡化，並與 LLVM 最佳化共同達到不動點；該策略本身不提供可執行翻譯後端。

## Windows 驅動程式模擬

`lib/emulation` 是由 `NEVERD_ENABLE_DRIVER_EMULATION` 啟用的選用執行元件。`emulate-driver` CLI 透過公開 C API 存取此元件。`DriverSession` 負責有界的 x64 WDM 初始化，以及選用的循序 create／IOCTL／read／write／cleanup／close／unload 呼叫；Windows 映像映射使用現有載入器提供的完整 `BinaryImage`，Windows 模型負責客體物件與 API 語義。Unicorn 配接器負責 CPU 執行，並持有客體記憶體的權威狀態。此路徑不使用實驗性的原生翻譯管線，也不改變其支援範圍。

Unicorn 透過 `cmake/NeverDUnicorn.cmake` 統一設定一次，與語義測試共用，並在 `BUILD_TESTING=OFF` 時仍可用。未知 API 與 CPU 環境行為會明確停止；驅動程式傳回失敗與模擬未完成始終保持區分。限制、報告及不支援的生命週期操作見[驅動程式模擬](driver-emulation.md)。

原有 C API 仍僅執行初始化。情境 JSON 在相同執行選項上使用統一的嚴格解析器，欄位與請求類型透過 `.def` 目錄宣告。要求的基底重新定位與安全性 cookie 初始化由執行載入器負責。Windows 模型負責 IRP／堆疊位置／檔案物件，並驗證同步完成或工作項目驅動的待處理完成；工作階段在共用執行預算下依序呼叫回呼。未使用的未知匯入採用延遲繫結；執行它們或讀取未建模的匯出資料時會明確停止。

匯出登錄表為靜態匯入與動態解析提供穩定的客體位址，並將可用性與實作分開處理：明確不存在的匯出解析為 NULL；存在但未建模的匯出導向陷阱；未指定可用性的動態查詢會停止。Windows 模型擁有獨立的檔案識別碼及由請求擁有的 MDL，包括對映權限及完成時的失效。執行階段 API 格式化透過經檢查的 Win64 參數讀取器存取客體參數。後端保留第一個結構化錯誤；後續觀察不會清除該錯誤，也不會繼續執行或提供 Windows SEH。

Windows 模型亦管理獨立的非分頁池 MDL；釋放描述符不會釋放底層緩衝區。獨立登錄模型管理明確場景樹、控制代碼權限與機碼值生命週期，與靜態匯出目錄分開。場景預檢與執行使用相同登錄驗證規則，報告保留最終機碼值；卸載檢查遺留控制代碼。

`KernelScheduler` 管理就緒佇列順序、回呼識別與計時器期限；`KernelDispatcher` 管理不透明 DPC、計時器、事件及其訊號。`KernelModel` 管理等待登記、工作項目／裝置生命週期與 IRP 完成。`DriverSession` 儲存並還原各回呼的獨立堆疊及完整 CPU 內容，包括 Win64 堆疊參數，客體記憶體保持共用。僅在沒有就緒執行框架時，虛擬時間才推進至計時器／等待／取消邊界；CPU0 以確定性的合作排程執行 `DISPATCH_LEVEL` 的 DPC 和 `PASSIVE_LEVEL` 的工作項目。這不提供一般執行緒／APC／自旋鎖排程、WDM／PnP 取消、並行 IRP、完整 PnP／電源或硬體。 API 的 IRQL 上限來自 `KernelAPIIRQL.def`，參數相關限制由所屬模型檢查。

`KernelFramework` 管理 KMDF 1.33 繫結、函式表識別、WDF 物件與內容、控制裝置初始化記錄、循序預設佇列及要求控制代碼。其具型別的裝置與要求主控介面將 WDM 命名空間、儲存空間、封包狀態、MDL 對應及完成驗證交由 `KernelModel` 負責；雙方均不建立重複的裝置或 IRP。佇列路由將框架擁有的分派狀態與傳回型別為 `void` 的客體回呼返回分開記錄。完成接續流程先執行清理及子物件銷毀，再釋放 IRP；外部參考僅保留 WDF 內容。刪除待處理要求會在修改上層祖先物件之前遭到拒絕；刪除時自動取消或排空要求仍不受支援。`DriverSession` 在共用預算下執行巢狀回呼。`DriverImage` 驗證 CFG 中繼資料；`GuardControlFlow` 管理已宣告的映像／API 目標，CPU 介面卡保留檢查／分派呼叫狀態。PnP 裝置、一般佇列排程及其取消、類別擴充及 UMDF 仍不受支援。

情境透過 `cancel_after_100ns` 為傳輸要求設定虛擬取消期限，`KernelModel` 的獨立 IRP 記錄管理此期限及實際發生的絕對 `cancel_requested_at_100ns`；公開情境仍循序提交要求。`KernelModel` 在框架路由後、客體 I/O 回呼前套用零延遲取消，保留完成先發生的結果，並在閒置時間推進時考慮正數取消期限。`KernelFramework` 管理標記／解除標記、排入佇列／已遞送狀態，以及保留至取消回呼傳回的內部參考。僅排入佇列的取消回呼不允許完成要求；遞送後，工作項目可在回呼等待期間協調完成。保留 WDF 物件不會恢復已失效的 IRP 儲存空間。`KernelScheduler` 將取消回呼與工作項目分開管理，在暫停／還原時保留回呼類別並共用容量與派送預算；DPC 優先，其後是 FIFO 取消回呼，再執行一般工作項目或還原就緒的被動等待框架。取消回呼在 `PASSIVE_LEVEL` 執行。此控制裝置契約不提供 WDM 取消常式或一般佇列排程器。


## 例外重寫邊界

Mach-O compact unwind 目前具備原始 `__unwind_info` 的嚴格 parser、產生
`__LD,__compact_unwind` 記錄的 fixup-aware parser、原始/產生範圍的精確 merge、
regular page 的確定性 encoder，以及交易式最終 section installer。installer 僅在既有、
file-backed 的 `__TEXT,__unwind_info` 能容納編碼結果時原位重寫；它會重新驗證架構、版面
與原始位元組，清零未使用尾端，並在 Mach-O 外層交易單次提交前重新解析結果、證明語意等價。
產生的記錄以編譯器精確記錄的 IR 來源函式到目標 MC owner symbol 對應（包括私有定義，且
不猜測物件格式前綴或改名規則）、opaque 非零 range ID，以及精確半開片段範圍進行驗證。
每個產生的 FDE 都必須精確匹配唯一的已驗證片段；每個必要片段也必須精確匹配該交易安裝的
唯一 FDE，除非它由一筆精確且通過嚴格 encoding 驗證的非 DWARF compact 記錄覆蓋。同一
函式擁有的相鄰或不相鄰片段可重用同一來源 recipe；缺失、重複、懸空、跨 owner 或邊界不
一致的身分會在修改輸出前失敗。新增 RX segment 只有在證明 `__LINKEDIT` 唯一且位於
file/VM 末端、所有 offset relocation 均經過溢位檢查，並嚴格重播最終檔案與虛擬位址版面
後才會提交。最終 section 缺失時不安裝產生的 compact 記錄，且僅在通過上述精確、已驗證的
DWARF-FDE 閉環時才可繼續交易；既有最終 section 容量不足或格式錯誤時仍會 fail closed。
已連結的原生 throw/catch 證明仍待完成。

外部參照依完整的 MC fixup 契約分類。call 只能選擇已驗證的可呼叫目標；產生的
compact-unwind personality 欄位只能選擇通過驗證的 non-lazy pointer slot，且絕不解參照
其檔案內容。TLS、authenticated pointer、減項、格式錯誤的 compact 欄位與未知 relocation
都會 fail closed。

ARM32 compact unwind 的已編碼堆疊調整與 GPR 版面為 `Complete`；D 暫存器模式選擇值 0 至 3
同樣為 `Complete`。選擇值 4 至 7 為 `Partial`，因為僅憑 compact word 無法證明每個經執行期
對齊的 CFA 相對 slot。`Partial` 項目可為分析保留已證明的暫存器身分，但所有重寫路徑都會
以 fail-closed 方式拒絕。每份 EH-frame 安裝 receipt 都精確綁定目標架構、指標寬度與位元組序；
compact-unwind DWARF 綁定會拒絕任何 receipt target identity 不相符。

頂層 ARM32 section 交易的能力邊界比 compact-unwind 解碼器更窄。只有 Mach-O header
精確為 `CPU_SUBTYPE_ARM_V7K`，且原始 symbol table 的 `N_ARM_THUMB_DEF` 位元對每個必要
函式都提供 Thumb code 的正向證明時，才會開放這條路徑。此後，精確的
`thumbv7k-apple-watchos` triple 與 Thumb mode 會貫穿並約束整個 code generation，輸入的
feature 需求也不得超過 Cortex-A7 上限。未標記或模式未知的函式、generic non-v7k
subtype、ARM mode、混合或未知的 external-code target、ARM Mach-O in-place entry point，
以及從 C source 發起的 ARM Mach-O patch，都會在修改輸出前 fail closed。對於 stripped
輸入，如果只能透過 `LC_FUNCTION_STARTS` 發現函式，目前仍不支援。

PE、ELF 與 Mach-O 各自具備格式特定的例外元件，但 NeverD 尚未公開涵蓋所有格式、
所有例外類型的端到端重寫流水線。不支援的 encoding 或未解析的註冊/layout 要求必須
在修改輸出前失敗；現有的局部格式能力不能描述為例外重寫已完全閉環。

辨識 Ada 或 D 的 Itanium personality 並不等於支援 Ada 或 D 例外。GNAT、GDC、DMD
與 LDC 的 address-form LSDA 可解析；type-table 槽位保持不透明（GNAT 為
`Exception_Id` / `Exception_Data`，D 為 `ClassInfo`），且絕不會依
`std::type_info` 解參考。原生重建會發出 LLVM `personality` 以及 address-form 的
`invoke`/`landingpad` 子句。corpus-proven 是另一層聲明，不能由 personality 辨識
或原生 lowering 自行推出。

## 元件對照

每個元件都是由 `add_neverd_component_library` 建立的靜態歸檔。下表列出重要的
NeverD 相依，不窮舉 CMake helper 統一提供的 LLVM 與 Capstone 程式庫。

| 目錄 | 職責 | 重要相依 |
|------|------|----------|
| `lib/loader` | 格式偵測、PE/COFF、ELF、Mach-O 載入；正規化 `BinaryImage`；函式發現 | LLVM Object API |
| `lib/lift` | 手寫 x86/i386、AArch64、ARM32 指令語意 | IR 資料型別 |
| `lib/decode` | Capstone/native 解碼並分派到架構 lifter | `NeverDIR`、`NeverDLift` |
| `lib/ir` | 共用型別以及 LowIR、MedIR、HighIR、intrinsic 定義/轉換 | 四個 IR 子元件 |
| `lib/pipeline` | 函式偵測與 Low/Med/High/LLVM 路徑編排 | IR、decode、lift、LLVM backend、除錯資訊、IR pass |
| `lib/backend/c` | HighIR 到 C 與 LLVM IR 到 C 的呈現 | IR |
| `lib/backend/llvm` | MedIR 到 LLVM 的 lowering | IR |
| `lib/backend/codegen` | 目標程式碼產生及 PE/ELF/Mach-O patch 與原地重寫 | IR、loader |
| `lib/sdk` | 公開 C ABI、session 生命週期、查詢、持久化、外掛、lift/decompile/patch/audit/hunt 進入點 | 將引擎元件聚合為 `libneverd` |
| `lib/pass` | LLVM IR 混淆 pass 與 MIR pass runner | IR |
| `lib/debug` | DWARF、PDB 與 linker-map 除錯內容 | IR |
| `lib/sigs` | 簽章解析、資料庫與比對 | Loader |
| `lib/libc` | 已知 libc 名稱與呼叫模型支援 | 獨立元件 |
| `lib/safety` | 提升 IR 上的堆積生命週期稽核與拷貝越界獵取 | Symbolic、Solver |
| `lib/support` | 共用二進位載入 helper | Loader |
| `lib/translate` | 帶版本的 guest state/策略/退出、固定 runtime ABI、受檢 guest memory、產生 IR/目標檔/LinkGraph 稽核、sealed 原生連結，以及實驗性的 x86-64 到 AArch64 C++ dispatcher | IR、LLVM、LLVM Object 與 JITLink 契約 |

公開標頭在 `include/neverd` 下對應這些區域。不要意外讓內部 C++ 類別成為 SDK
的一部分：穩定的外部操作應放在純 C 標頭及職責明確的
`lib/sdk/NeverDCAPI*.cpp` 檔案中。

## 嚴格提升契約

`Decoder` 與每個架構 lifter 預設以嚴格模式啟動。如果 Capstone 能解碼指令，
但選定的 lifter 沒有實作，lifter 會拋出 `UnliftedInstruction`。例外記錄指令
位址、助記符和運算元字串；因此，不支援的語意必須明確失敗，不能被省略或猜測。

內部非嚴格路徑會發射 `NdOp::NOP`，但它只是診斷逃生口，不是指令的可接受實作。
貢獻者測試與 CI 應保持嚴格模式開啟。出現嚴格失敗時：

1. 以最小的架構特定 fixture 重現。
2. 在 `lib/lift/<ISA>` 中加入缺少的語意。
3. 在 `unittests/lift` 中斷言預期的 LowIR 形狀。
4. 若指令有可觀察行為，在 `unittests/semantic` 中新增 Unicorn 差分往返。

不要只為讓 pipeline 繼續而捕捉 `UnliftedInstruction`。新的刻意近似需要明確契約
與測試；不得偽裝成 1:1 提升。

## 格式與 ISA 所有權

輸入格式邏輯與輸出重寫邏輯刻意分離：

| 格式 | 載入、中繼資料與輸入重定位 | Patch 與輸出重定位 |
|------|----------------------------|--------------------|
| PE/COFF | `lib/loader/COFF` | `lib/backend/codegen/COFF` |
| ELF | `lib/loader/ELF` | `lib/backend/codegen/ELF` |
| Mach-O | `lib/loader/MachO` | `lib/backend/codegen/MachO` |

架構 lifter 位於 `lib/lift/X86`、`lib/lift/AArch64` 與 `lib/lift/ARM`。
對應的公開 lifter/register 宣告位於 `include/neverd/lift`。目標特定的 LLVM
發射與程式碼產生位於 `lib/backend/llvm/<ISA>` 及
`lib/backend/codegen/CodeGen<ISA>.cpp`。

<a id="support-and-test-depth"></a>

### 支援範圍與測試深度

根目錄支援矩陣表示每個單元格皆已實作；這不代表每個 opcode、ABI 邊界案例、
二進位產生器或作業系統版本都已窮盡測試。指令語意超出 lifter 已實作涵蓋範圍時，
嚴格模式會以失敗即關閉方式停止。

所有 12 個格式×架構單元格都在
`unittests/semantic/PatchFullSubstRTTests.cpp` 中具有語意重寫後端覆蓋。
整合深度則更具體：

| 格式 | x86-64 | i386 | AArch64 | ARM32 |
|------|--------|------|---------|-------|
| PE/COFF | 已連結 fixture | 後端網格 | 已連結 fixture | 已連結 Thumb fixture |
| ELF | 已連結 fixture + 語意往返 | 物件流水線 + 語意往返 | 已連結 fixture + 語意往返 | 已連結 fixture + 語意往返 |
| Mach-O | 已連結 fixture\* | PIC/no-PIC 物件流水線\* | 已連結 fixture\* | 後端網格 |

- **已連結 fixture** 對代表性程式執行已連結可執行檔的 loader/pipeline 與
  patch 行為。
- **物件流水線** 對可重定位物件執行載入、所有 IR 階段和反編譯，但不涵蓋主機
  連結及 patch 後二進位的執行。
- **後端網格** 透過精確的重寫程式碼產生路徑編譯代表性 IR，並在 Unicorn 中比較
  行為；它不會對已連結可執行檔執行該格式的 loader。
- `*` Mach-O 已連結 fixture 依賴能產生所需目標的主機工具鏈。現代 macOS 無法
  連結歷史 i386 可執行檔，因此 i386 使用 PIC 與 no-PIC thin 物件加重寫網格。

對這些代表性程式，應將已連結 fixture 單元格視為最強的格式整合證據。
物件流水線與後端網格單元格只有部分格式整合覆蓋。沒有任何單元格能在不加限定時
稱為「完全測試」，也沒有單元格宣稱窮盡 ISA 覆蓋。

主要證據包括：用於已連結 ELF 與 PE fixture 的
[`PatchFormatTests.cpp`](../../unittests/lift/format/PatchFormatTests.cpp)，用於 Windows ARM
載入/反編譯的
[`COFFARMFormatTests.cpp`](../../unittests/lift/format/COFFARMFormatTests.cpp)，用於 i386 thin
物件的
[`MachOI386RelocationTests.cpp`](../../unittests/lift/format/MachOI386RelocationTests.cpp)，
用於已連結 Mach-O 的
[`X86_64_PipelineE2ETests.cpp`](../../unittests/lift/x86_64/X86_64_PipelineE2ETests.cpp) 與
[`AArch64_PipelineE2ETests.cpp`](../../unittests/lift/aarch64/AArch64_PipelineE2ETests.cpp)，
以及涵蓋 12 單元後端網格的
[`PatchFullSubstRTTests.cpp`](../../unittests/semantic/probe/patchfull/PatchFullSubstRTTests.cpp)。
命令見[測試指南](testing.md)。

## 在哪裡修改

| 變更 | 從這裡開始 | 最小聚焦驗證 |
|------|------------|--------------|
| 新增或修正指令 | `lib/lift/X86`、`AArch64` 或 `ARM` 中的對應檔案；分派變更時修改公開 lifter 標頭 | `unittests/lift` 中的架構測試；`unittests/semantic` 中的語意往返 |
| 新增 `NdOp` | `include/neverd/ir/NdOps.h`，接著稽核 Low-to-Med、emitter/renderer、verifier/emulator 與 dump | `NeverDLiftTests` + 相關 `NeverDSemanticTests` 案例 |
| 修改 CFG 或函式發現 | `lib/ir/low`、`lib/loader/FunctionDiscovery*.cpp`、`lib/pipeline/PipelineFuncDetect.cpp` | lift CFG/跳躍表測試與聚焦語意轉換套件 |
| 新增 PE 輸入重定位或 unwind 規則 | `lib/loader/COFF` | `COFFARMFormatTests` 或新的聚焦 loader fixture |
| 新增 PE 輸出重定位或 patch 規則 | `lib/backend/codegen/COFF` | `PatchFormatTests`、`RewriteCodegenRTTests` 與 PE 後端網格 |
| 修改 ELF 或 Mach-O 格式行為 | 對應的 `lib/loader/<Format>` 和/或 `lib/backend/codegen/<Format>` 目錄 | 對應格式測試加重寫網格 |
| 修改 MedIR/ABI 復原 | `lib/ir/med` | 呼叫慣例 lift 測試 + 跨 ISA 語意往返 |
| 修改結構化控制流復原 | `lib/ir/high` | `NeverDCFGLoopXformTests` 與結構化 C 測試 |
| 新增 LLVM 轉換 | `lib/pass/ir`、`include/neverd/pass/ir` 中的公開標頭，公開時加入 pipeline 切換 | 聚焦轉換套件 + patch 輸出變更時的 `NeverDPatchFullTests` |
| 新增 C API 操作 | `include/neverd/sdk/NeverDCAPI.h`、聚焦的 `lib/sdk/NeverDCAPI*.cpp`，只有狀態需要時使用 `SessionImpl.h` | SDK/CLI 語意測試；保持 `neverd_last_error` 與配置慣例 |
| 新增 CLI 命令 | `tools/neverd/NeverDCLIOptions.cpp`、`NeverDCLI.h`、聚焦的 `NeverDCmd*.cpp`，以及 `neverd.cpp` 中的分派 | `unittests/semantic/CLIEndToEndTests.cpp` 與直接 CLI smoke test |
| 修改堆積生命週期稽核或拷貝越界獵取 | `lib/safety`、`include/neverd/safety`、`include/neverd/sdk/NeverDCAPISafety.h` | `NeverDSafetyTests` 與 `NeverDSafetyIntegrationTests` |
| 新增語意回歸 | 聚焦的 `unittests/semantic/*Tests.cpp`；在 `unittests/semantic/CMakeLists.txt` 註冊新檔案 | 建置其測試二進位，再以 `ctest -R` 選擇命名案例 |

保持修改精確。定義某種表示的檔案可與其轉換一起變更，但不要只為讓大型重構看起來
一致而修改無關的 loader、lifter 與 backend。

原始碼記錄型別獨立保存欄位配置。統一 ABI 層支援 Darwin ARM64 中含一至四個同類 float 或 double 成員的巢狀結構，浮點暫存器不足時將整個參數放入堆疊。MedIR 在 SSA 前綁定各個實體成員，HighIR 保留單一邏輯參數或回傳值，C 輸出驗證配置。Darwin ARM64 與 x86_64 也支援由一至兩個 64 位元整數或指標組成的記錄，包括巢狀配置。整個記錄溢出到堆疊時，ARM64 耗盡該暫存器組，x86_64 則保留剩餘暫存器給後續參數。有填補、壓縮欄位、混合浮點與整數及不完整分量仍拒絕；這些提示不能授權二進位改寫。

執行階段呼叫目錄僅在精確匯入的函式明確回傳原參數指標時宣告 `ReturnedArgument`。接收者分析先讀取已宣告的實體參數，再套用正常 ABI 暫存器清除，最後僅在回傳值上恢復既有的接收者型別事實。SDK 會重新驗證此效果；它不允許刪除呼叫、所有權效果或記憶體存取。

編譯器產生的框架目錄與接收者目錄共用提供者清單：Foundation、CoreData、CoreLocation、CoreSpotlight、QuartzCore、UniformTypeIdentifiers 和 UserNotifications。QuartzCore 使用公開入口 `CoreAnimation.h`，其他框架的相容性匯入不提供所屬宣告。兩個產生器均保留四種前置處理設定、精確框架身分及宣告的否定證據。

物件回傳型別透過一致的方法宣告延伸同一條有界接收者證明。具名物件回傳型別和編譯器宣告的關聯回傳型別可提供類別事實；單獨的 id 不可以。欄位讀取和訊息回傳共用八步預算，原始碼驗證會依目前宣告重新檢查每一步。精確繫結的配置輔助函式使用對應訊息的回傳型別契約，保留呼叫、自訂覆寫和所有權效果。回傳類別衝突或接收者繼承關係不完整時停止傳播。

格式呼叫綁定保留所屬語言規則。NSString 屬性和公開述詞入口皆核對 SDK 宣告及所有執行期替代宣告。述詞不替換單引號或雙引號中的佔位符，`%K` 接收屬性名稱物件；實際參數沿用統一的純量提升與 Darwin 可變參數 ABI。不支援的跳脫及格式修飾符明確拒絕。原始碼發布前重新驗證語法、常數物件身分及參數證明，產生程式碼仍呼叫原框架剖析器。

編譯器宣告的固定參數 C 匯入函式與 Objective-C 訊息共用受支援結構體的來源 ABI 分配規則。只有明確宣告的參數占用載體；Objective-C 層提供隱藏的接收者與選擇器參數。仍須嚴格匹配 SDK 匯出身分與簽章。僅支援純量的回呼與可變參數保留現有限制。

來源函式宣告將呼叫慣例保留為簽章身分的一部分。共用 ABI 層支援範圍明確的 Swift 呼叫，可承載 1、2、4 或 8 位元組整數參數及指標參數：先分配整數暫存器組，再分配相對入口 SP 的堆疊載體。每個窄載體都記錄精確的延伸規則，結果仍限制為最多兩個整數或指標字。HighC 在宣告和定義中保留 `swiftcall`。經編譯器觀測確認的 Foundation 值橋接還可宣告一個 `swift_indirect_result` 指標及一個 `swift_context` 指標：arm64 使用 x8/x20，x86_64 使用 RAX/R13，兩者皆不占用一般整數參數暫存器組。HighC 保留這兩個參數屬性。公開 Foundation 中繼資料匯入必須在 ARM64/x86-64 的 macOS 和 Mac Catalyst 設定間取得編譯器符號圖、實際中繼資料查詢 IR 與精確 SDK 匯出的共同證明。僅憑重整符號後綴不能確定 ABI。泛型或未宣告的隱藏參數、Swift 回呼型別及不支援的實體載體仍被拒絕。Mac Catalyst 宣告不代表已有 iOS 裝置執行驗證。

MedIR 統一負責同寬 SSA 複製與完整 PHI（包括迴圈）的有界常數傳播。所有入邊值必須收斂到相同的位元、寬度、來源和位址歸屬。未知定義、無初值迴圈、不完整邊及衝突常數阻止替換；預算耗盡時函式保持原樣。分析只替換運算元，保留呼叫、載入、儲存及其副作用。HighIR 與 LLVM 使用同一分析結果。

單次操作內的程式碼歸屬索引也保存執行階段中繼資料中主函式與程式碼片段的精確關係。索引查詢與直接掃描共用同一關係走訪，保留主函式入口的原始位址，並拒絕孤立參照及指向非主函式的父參照。跳轉目標驗證、邊界證明和暫時分組分析使用同一個不可變索引及查詢成本計算。其他映像的索引會退回直接掃描；預算耗盡仍拒絕不完整證明。

原始碼 ABI 明確記錄 Darwin ARM64 和 x86_64 窄整數暫存器參數的 32 位元符號擴展或零擴展。HighIR 在儲存與複製參數時保留這些已知位元，同時維持原始參數型別。超過 32 位元的讀取、堆疊填補及被呼叫破壞的暫存器仍為未知。這遵循 Apple 的 ARM64 與 Intel 呼叫慣例；僅觀察到原生低位元組值不能證明擴展。

Mach-O 載入器保留區段的「重定位後唯讀」保證，同時保留初始權限。原始碼位元組與指標讀取共用唯一檔案映射檢查；區段名稱本身不能證明不可變性。普通全寬載入可將已解析的本機資料指標綁定至獨立驗證的常量字串物件。綁定保留來源槽以便重新驗證，別名共用目標物件的產生身分。可寫儲存、衝突重定位、部分或有序載入，以及槽本身的位址仍不受支援。

跳躍表恢復只產生前往一般後繼區塊的轉移，不再透過另一條路徑重建區塊內陳述式。每個區塊只由統一流程轉換一次，包括共用分支、預設目標與迴圈入口。分支邊上的 PHI 賦值在對應轉移前執行，並保留平行賦值快照；不完整的邊綁定仍明確失敗。

迴圈結構化保留原生入口的精確歸屬。恆真迴圈的包裝節點保留首條迴圈主體指令的標籤，不重複建立入口。有條件的回邊退出到原有後續陳述式，包括沒有原生位址的邊複製。只有精確符合後續入口的跳轉才轉換為 break；巢狀迴圈和 switch 內的跳轉保留其控制範圍。

死值消除在刪除 PHI 邊複製之前正規化原生入口的歸屬。共用的合併邏輯區分條件分支入口及其緊鄰的合成邊複製前綴；不連續的標籤以及無關的巢狀標籤仍判為具有歧義。

`scripts/collect_objc_sdk_declarations.py` 從實際裝置、模擬器、Mac Catalyst 和桌面 SDK 組態採集框架所屬的類別、協定、分類及方法 ABI 候選。每個組態保留公開標頭、匯出和編譯器證據，包括不支援的宣告及相關物件回傳型別。部分 SDK 採集會記錄確切範圍；任一組態失敗都會留下未完成的記錄。CI 產物供後續宣告目錄一致性驗證使用，本身不會啟用新的原始碼呼叫繫結。

Objective-C 呼叫事實包含有界、相對入口 SP 的私有堆疊槽。控制流匯合時，精確值取交集，可能源自堆疊框架的位元組取聯集，避免衝突路徑和部分暫存器寫入掩蓋位址逸出。具有明確 ABI 的呼叫僅保留已配置且位於傳出參數之外的私有儲存。位址逸出、未知呼叫、重疊或原子寫入及堆疊空間釋放會撤銷相應證明。迴圈回邊必須收斂後才能發布繫結。

HighC 使用精確位寬的 `_BitInt` 型別表示不超過 128 位元的部分整數載體。一般記憶體輔助函式依 IR 位元組數傳輸，不依賴 C 物件填補；無號運算保留環繞及位移邊界。不支援的部分位寬原子存取會明確拒絕，不會擴大存取範圍。

MedIR 原始碼參數驗證從宣告的回傳值、控制流程、記憶體副作用和呼叫反向追蹤所需位元組。COPY、PHI、CONCAT、位元組擷取和擴充保留位元組需求；其他操作保守地要求全部輸入。未使用的浮點暫存器高位不會產生額外參數。可觀察的高位、不完整的圖和耗盡的分析預算仍保留原有拒絕結果。此分析不刪除機器操作，也不授予重寫 ABI。

條件結構化保留原有邊上的未跳轉路徑 PHI 賦值。其來源位址不能成為新建的延續跳轉目標；若移動程式碼段需要這樣的目標，共享延續路徑就保留在原處。 無條件的合成迴圈在精確迴圈頭之前沒有任何操作時，也與內部首條原生指令共享延續入口；有條件的測試或前置副作用不具備這種等價性。

原生輔助函式的原始碼簽章推斷透過有界 CFG 分析，證明每條機器返回路徑都具有完整的整數結果。共享出口匯合前驅事實，入口路徑阻止未初始化的迴圈自證。呼叫和局部寫入會撤銷返回暫存器的證明，直到再次完整計算。畸形控制流、僅返回傳入值的路徑以及 x86-64 尾聲恢復仍會被拒絕。這只產生候選原始碼簽章；第二次管線仍須驗證函式本體及其相依閉包，不會改變重寫 ABI。
