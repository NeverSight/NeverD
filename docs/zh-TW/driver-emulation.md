**語言**: [English](../driver-emulation.md) | [简体中文](../zh-CN/driver-emulation.md) | [繁體中文](driver-emulation.md) | [日本語](../ja/driver-emulation.md) | [한국어](../ko/driver-emulation.md) | [Français](../fr/driver-emulation.md) | [Deutsch](../de/driver-emulation.md) | [Español](../es/driver-emulation.md) | [Italiano](../it/driver-emulation.md) | [Русский](../ru/driver-emulation.md) | [العربية](../ar/driver-emulation.md)

[← 文件索引](README.md)

# Windows 驅動程式模擬

NeverD 的選用驅動程式模擬器執行受支援 x64 WDM 驅動程式的 PE 進入點，並可在卸載前執行明確指定的同步請求情境。它使用 Unicorn 執行 CPU 指令，使用 NeverD 自有的有界 Windows 環境模型。它不會將驅動程式載入主機核心，也不會把客體 API 呼叫轉送給主機作業系統服務。

## 建置與執行

此功能須明確啟用，且不依賴 `BUILD_TESTING`：

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEVERD_ENABLE_DRIVER_EMULATION=ON
cmake --build build-release --target neverd --parallel 4
build-release/bin/neverd emulate-driver path/to/driver.sys \
  --instruction-limit 100000 > driver-report.json
```

報告一律以 JSON 輸出至 stdout；請求與環境設定診斷輸出至 stderr。預設預算為 100000 條客體指令、64 MiB 客體記憶體、10000 筆記錄事件及 5000 毫秒。指令數限制必須為正數。任一預算耗盡便停止執行，並保留已收集的部分觀察結果。

| 結束碼 | 意義 |
|--------|------|
| `0` | 初始化及所有請求中已完成的操作皆成功 |
| `1` | 輸入或選項無效、執行環境建立失敗，或建置時未啟用此功能 |
| `2` | 初始化或已完成的請求傳回失敗的 `NTSTATUS` |
| `3` | 情境尚未完成便停止執行，例如遇到不支援的 API、錯誤或預算限制 |

傳回失敗狀態仍表示已完整觀察到該操作的結果。成功傳回只描述這一次模型執行，不能證明該驅動程式可在 Windows 下正常運作。

## 驅動程式相容性

相容性取決於實際執行的程式碼路徑及其相依項目，而非 `.sys` 副檔名。目前的驗收證據涵蓋原創的獨立測試樣例，以及 Microsoft SIOCTL WDM 範例的緩衝、in-direct 和 out-direct 路徑，包括啟用偵錯記錄的建置；這並不代表與任意第三方驅動程式相容。

| 驅動程式類別或需求 | 目前範圍 | 缺少的環境 |
|--------------------|----------|------------|
| 使用下列 API 的 x64 軟體 WDM 驅動程式 | 初始化與同步檔案生命週期 | 每個額外執行到的 API 都必須有明確的模型 |
| `METHOD_BUFFERED` IOCTL | 支援獨立檔案識別碼與交錯請求 | 尚不支援非同步完成 |
| `METHOD_IN_DIRECT`、`METHOD_OUT_DIRECT` | 由請求擁有的 MDL 及系統對映 | 驅動程式自行配置的 MDL、實體頁面識別、DMA 及使用者對映 |
| 同步 READ/WRITE | 依裝置旗標使用緩衝或直接傳輸 | Neither I/O、隱含檔案位置選擇及非同步完成 |
| `METHOD_NEITHER` | 拒絕 | 使用者位址空間環境、存取探測及客體例外處理 |
| KMDF / UMDF 驅動程式 | 不支援 | 框架繫結、物件、佇列、回呼及對應的主機執行階段 |
| PnP 匯流排／功能／篩選驅動程式 | 初始化可在 API 子集內執行；不支援裝置堆疊生命週期 | 裝置附加、向下層驅動程式派送、PnP 與電源 IRP |
| 儲存、網路、顯示、檔案系統及迷你篩選驅動程式 | 不支援相關子系統契約 | 連接埠／類別／迷你連接埠框架、NDIS/WFP、圖形或檔案系統服務 |
| 使用背景工作執行緒、計時器、DPC、APC、等待或取消的驅動程式 | 不支援 | 排程、IRQL 轉換、同步與非同步所有權 |
| 使用處理程序／執行緒回呼、控制代碼、登錄／檔案操作或核心模組探索的驅動程式 | 下列 API 以外的行為不支援 | 物件管理員、系統狀態及回呼／事件產生機制 |
| 硬體、DMA、PCI、中斷或虛擬化驅動程式 | 不支援所需環境 | 裝置模型、實體記憶體、匯流排、中斷及特權 CPU 狀態 |
| x86 或 ARM64 Windows 驅動程式 | 拒絕 | 對應架構的載入、ABI 及執行模型 |
| 需要 CFG、不支援的載入組態、TLS 或其他遭拒絕 PE 功能的 x64 映像 | 載入時拒絕 | 針對這些需求的明確載入器／執行階段語義 |

未使用的不支援匯入項目可以維持繫結。一旦執行到不支援的操作，便停止並提供診斷及先前收集的觀察結果。僅 DriverEntry 成功，不能證明後續派送、硬體或框架路徑也受支援。下方 API 表是受支援子集的權威定義。

## 執行契約

此設定在 `PASSIVE_LEVEL` 下模擬單執行緒的 x64 WDM 生命週期。執行從 PE 進入點開始；若有編譯器產生的進入點包裝函式，也會保留並執行。DriverEntry 必須傳回 `STATUS_SUCCESS` 才能完成初始化；非零的成功狀態或待處理狀態會因初始化契約不受支援而停止。失敗狀態則保留為已完成的初始化結果。所有物件、字串、堆疊、函式指標及配置均位於客體記憶體。模型依設定的服務名稱（預設為 `NeverDDriver`）提供 `DRIVER_OBJECT` 和登錄路徑。

配接器使用 Unicorn 的虛擬 TLB 模式保留客體虛擬位址，包括規範的高位核心位址，無須合成 Windows 頁表。初始 RFLAGS 為 `0x202`；軟體裝置設定採用固定的 64 位元組快取列。這些都是本執行情境的明確屬性。行內 x64 CR8 讀取觀察到相同的 `PASSIVE_LEVEL`；CR8 寫入與其他控制暫存器操作仍不受支援。

未知匯入項目繫結至延遲陷阱。未使用的匯入項目不會阻止執行；執行其 thunk 或讀取未建模的匯出資料值時，會以 `unsupported_api` 停止。不支援的 CPU 環境效果也會明確停止。NeverD 不會用成功傳回值替代未實作的呼叫。格式錯誤的映像或不支援的載入需求會在執行前失敗。

此設定並未實作完整的 Windows 核心、KMDF 執行階段、PnP／電源生命週期、非同步或待處理 IRP、neither 方法 IOCTL、中斷或多執行緒排程。只有情境明確請求時才執行回呼；僅初始化模式仍在 DriverEntry 之後停止。

映像預設使用慣用基底位址，除非情境選擇了有效的重新定位位址。映像必須為使用 native 子系統的 PE32+ x64 可執行檔。匯入可來自 `ntoskrnl.exe` 或 `ntkrnlmp.exe`。

執行載入器支援經驗證的 x64 `DIR64` 基底重新定位，以及有限的安全性 cookie 載入組態；它會在進入點包裝函式執行前設定具確定性的客體 cookie。CFG、其他未建模的載入組態欄位、TLS、延遲／繫結匯入、依序號匯入及受控映像皆會遭拒絕。映像還必須通過嚴格的範圍與對齊檢查。

初始 API 模型刻意採用有限契約：

| API | 建模行為與限制 |
|-----|----------------|
| `RtlInitUnicodeString` | 根據有界且以 NUL 結尾的來源字串建立客體 `UNICODE_STRING` |
| `RtlCopyUnicodeString`、`RtlCompareUnicodeString`、`RtlEqualUnicodeString` | 計數式 UTF-16 複製及區分大小寫的比較；不區分大小寫的比較需要 Windows 大小寫對照表，因此會停止 |
| `ExAllocatePool2` | 分頁／非分頁 NX 配置，預設清零；建模未初始化與快取對齊旗標；無效的必要旗標傳回 NULL，配額／可執行集區及引發的配置例外會停止 |
| `MmGetSystemRoutineAddress` | 透過共用匯出清單解析客體計數式名稱 |
| `MmMapLockedPagesSpecifyCache`、`MmGetSystemAddressForMdlSafe`、`MmUnmapLockedPages` | 由請求擁有的 MDL、快取的 KernelMode 系統對映、明確權限及生命週期；重複使用既有的安全對映 |
| `ExAllocatePoolWithTag`、`ExFreePoolWithTag`、`ExFreePool` | 對集區類型 `0`、`1`、`512` 提供資料配置；大小／標籤必須為正，帶標籤釋放必須相符，位址不重複使用 |
| `IoCreateDevice`、`IoDeleteDevice` | 裝置類型為 `0x22`，characteristics 為 `0` 或 `0x100`，擴充區大小有界，名稱為 ASCII `\Device\Name` |
| `IoCreateSymbolicLink`、`IoDeleteSymbolicLink` | 一個工作階段命名空間內的 ASCII `\DosDevices\Name` 或 `\??\Name`，目標為 `\Device\Name` |
| `DbgPrint`、`DbgPrintEx` | 經檢查的 Win64 可變參數格式化，最多輸出 512 位元組；啟用所有偵錯器篩選器 |
| `IoGetCurrentIrpStackLocation` | 傳回目前建模 IRP 的堆疊位置；正常編譯的 WDM 巨集讀取相同客體欄位 |
| `KeGetCurrentIrql` | 傳回 `PASSIVE_LEVEL` |
| `IofCompleteRequest`、`IoCompleteRequest` | 以 `IO_NO_INCREMENT` 完成目前同步建模 IRP；已完成的 IRP 或緩衝區不能再次存取 |
| `memcpy`、`memmove`、`memset`、`memcmp`、`RtlCopyMemory`、`RtlMoveMemory`、`RtlFillMemory`、`RtlZeroMemory`、`RtlCompareMemory` | 有界的客體緩衝區操作，每次呼叫最多 1 MiB；要求不重疊的複製 API 會拒絕重疊 |

`DbgPrint` 格式化支援整數 `d/i/u/o/x/X`、指標 `p`、文字 `s/c`、`%%`、計數式 Unicode `wZ/lZ`、寬字元 `ls/ws`、旗標、包含 `*` 的寬度／精度，以及 Windows 整數長度修飾符。最多讀取 32 個可變參數及 1024 個格式位元組。寬度與精度上限為 512。浮點數、`%n`、未知組合及非 ASCII 文字轉換會明確停止；模型不會猜測 Windows 字碼頁，也不會對客體資料呼叫主機 printf。

原始 RegistryPath 記錄及其緩衝區在 DriverEntry 傳回時失效。後續仍需使用該字串的驅動程式，必須在初始化期間複製它。

物件／集區區域大小為 1 MiB。未初始化集區位元組採用具確定性的 `0xCD` 內容；釋放後的集區位元組採用 `0xDD`。這是一種具體執行情境。CPU 存取及建模的緩衝區 API 會拒絕存取已釋放的集區配置、已刪除裝置、區域內尚未配置的位元組、不透明物件欄位，以及對唯讀物件欄位的寫入。這些檢查涵蓋的是本模型的物件生命週期，並非通用的驅動程式記憶體安全分析。未寫入的派送表槽位在報告中以零表示「未註冊」。情境請求若對應未註冊的主要功能，則由模型的預設處理常式完成，狀態為 `STATUS_INVALID_DEVICE_REQUEST`；該失敗在派送狀態與 I/O 狀態中皆可見。模型不會為此處理常式虛構客體函式位址，客體讀取未寫入槽位仍不受支援。明確註冊空回呼屬於錯誤。

## 請求情境

透過 `--scenario` 傳入 JSON 檔案，指定請求及選用的卸載操作：

```bash
build-release/bin/neverd emulate-driver path/to/driver.sys \
  --scenario scenario.json > driver-report.json
```

對於建立 `\Device\NeverDIO` 並接受緩衝 IOCTL `0x222000` 的驅動程式，`scenario.json` 範例為：

```json
{
  "requests": [
    {"kind": "create", "device": "\\Device\\NeverDIO"},
    {"kind": "ioctl", "code": "0x222000", "input": "00112233", "output_size": 4},
    {"kind": "cleanup"},
    {"kind": "close"}
  ],
  "unload": true
}
```

裝置名稱與 IOCTL 代碼必須符合驅動程式。create 省略 `device` 時選擇唯一的存活裝置；無法唯一選擇則失敗。後續請求使用其檔案的裝置，除非明確提供相符的名稱。選用的 `file` 是無號 32 位元情境識別碼，預設為零。每個識別碼各有自己的 FILE_OBJECT 與 FsContext，且必須依 create、傳輸、cleanup、close 的順序執行。獨立檔案的請求可以交錯執行。獨占裝置會拒絕第二次開啟。這些識別碼代表檔案物件，而非複製的控制代碼。支援緩衝及兩種直接 IOCTL 方法。派送必須同步完成每個 IRP；傳回 `STATUS_PENDING`、未完成請求、輸出長度無效及存取已完成的 IRP 都會明確失敗。請求卸載後，不得留下存活的裝置、符號連結、集區配置或檔案物件。

選用根欄位 `"load_address": "0x190000000"` 請求變更載入基底位址；省略此欄位或指定 `"0x0"` 時使用慣用位址。映像必須滿足重新定位需求。原有的初始化命令或 C API 不會隱含執行任何請求情境。

根物件僅接受 `load_address`、`requests`、`unload` 和 `kernel_exports`。所有請求都接受 `kind`、選用的 `device` 和選用的 `file`。IOCTL 必須提供 `code`，並接受 `input`、`output_size` 和 `direct_input`。`read` 接受 `output_size` 和 `byte_offset`；`write` 接受 `input` 和 `byte_offset`。位移預設為零，可使用整數或十六進位字串，且必須落在非負有號 64 位元值的範圍內。生命週期請求會拒絕傳輸欄位。未知或重複欄位會遭拒絕。`code` 接受無號 32 位元 JSON 整數或 `0x` 十六進位字串。`input` 是長度為偶數且不帶前綴或空格的十六進位位元組字串；省略表示空輸入。`output_size` 為無號 JSON 整數，省略表示零。不接受小數及浮點數寫法。

對於直接 IOCTL，`input` 初始化第一個系統緩衝區，`direct_input` 則初始化由 MDL 描述的獨立第二個緩衝區，並以零補齊至 `output_size`。`METHOD_IN_DIRECT` 要求可讀取，但不代表系統對映唯讀。兩種方法都使用可讀寫的情境緩衝區。`MdlMappingNoWrite` 移除對映的寫入權限，`MdlMappingNoExecute` 移除執行權限。解除對映會撤銷系統 VA；重新對映仍保留相同的鎖定資料。完成請求後，MDL 及對映皆失效。模型提供 WDM 巨集使用的公開 MDL 欄位；處理程序／PFN 欄位、手工建立的 MDL、使用者對映，以及透過原始 UserBuffer 直接存取都會遭拒絕。長度為零的直接緩衝區使用空 MDL。

對於 READ/WRITE，`DO_BUFFERED_IO` 或 `DO_DIRECT_IO` 選擇傳輸方法。Neither 或互相衝突的旗標會停止。Information 會依傳輸長度檢查；寫入傳回計數，讀取傳回位元組。

`kernel_exports` 將常式名稱對應至明確的可用性布林值，例如 `"kernel_exports": {"OptionalRoutine": false}`。已建模的匯出與靜態匯入會取得與 `MmGetSystemRoutineAddress` 共用的穩定位址。明確不存在的匯出解析為 NULL，且不能滿足靜態匯入需求。宣告存在但沒有 API 模型的匯出解析為延遲陷阱。未知的動態名稱會停止，並診斷其可用性未指定；絕不因缺少實作而推斷匯出不存在。名稱為有界的可列印 ASCII，解析時區分大小寫。此清單是具體情境的屬性，不代表符合每個 Windows 版本。
`IoGetCurrentIrpStackLocation` 和 `MmGetSystemAddressForMdlSafe` 是已建模的 WDM 標頭檔輔助函式；模型預設不會據此宣告它們是匯出項目，其匯出可用性需要靜態匯入或明確的 `kernel_exports` 宣告。

情境文字上限為 2 MiB，最多包含 64 個請求，每個輸入或輸出緩衝區最多 65536 位元組，包含 `direct_input` 內容在內的總請求位元組數最多 512 KiB。指令、觀察事件、客體記憶體及時間預算涵蓋整個情境。1 MiB 區域還需存放物件與中繼資料，因此即使尚未用盡情境緩衝區總額度，也可能耗盡模型記憶體。

## Microsoft 範例驗收檢查

選用的[驗證指令碼](../../scripts/validate_windows_driver_sample.py)下載[驗證清單](../../unittests/emulation/fixtures/sioctl-validation.json)中固定版本的 Microsoft SIOCTL 原始碼，驗證 SHA-256 雜湊，並使用 MinGW-w64 DDK 標頭檔編譯未修改的原始碼。指令碼在選定的輸出目錄中保留上游授權／來源資訊、建置命令、情境與報告。它需要網路存取、Clang、`lld-link`、`nm` 及 MinGW-w64 的 DDK 標頭檔：

```bash
python3 scripts/validate_windows_driver_sample.py \
  --neverd build-release/bin/neverd \
  --output build-release/driver-validation/sioctl
```

非預設 MinGW-w64 包含目錄可透過 `--headers` 指定。指令碼根據已編譯物件的相依項目產生 MS COFF 匯入程式庫。檢查分別以緩衝、in-direct 和 out-direct 情境執行 DriverEntry、create、IOCTL、cleanup、close 和 unload。加入 `--debug` 並選擇獨立輸出目錄，可使用 `DBG=1` 編譯並驗證客體日誌訊息。上游範例未註冊 cleanup 處理常式，因此模型的預設處理常式以 `STATUS_INVALID_DEVICE_REQUEST`（`0xC0000010`）完成 cleanup。驅動程式仍會關閉並卸載，成功的 IOCTL 則傳回預期位元組。對於這個完整情境，CLI 的預期結束碼為 **2**，`scenario_success` 為 false。指令碼僅在所有結果皆符合預期時成功，包括可見的 cleanup 失敗；它不會改寫範例來隱藏該結果。

## 報告與 SDK

JSON 報告區分 `stop_reason`、可為空值的 `nt_status` 和 `nt_success`、停止位置 PC，以及指令計數。它保留停止前收集的 API 呼叫及可觀察狀態，包括裝置物件與驅動程式回呼位址。客體位址以十六進位字串表示，避免 JSON 使用端遺失 64 位元精確度。

`configuration` 物件記錄本次執行的限制、服務名稱與 `kernel_exports` 覆寫值。設定識別為 `wdm-x64-synchronous-v2`。`nt_status` 始終是 DriverEntry 的結果，而 `scenario_success` 綜合描述初始化及已完成請求的結果。`phase`、`requests` 和 `unload_completed` 表明請求生命週期的哪些部分已執行。每次 API 呼叫及 CPU 寫入也會記錄階段（`driver_entry`、`request:N` 或 `unload`）。每個請求報告派送狀態與 I/O 狀態、是否完成、information 長度及傳回的 `output_hex` 位元組。`preferred_image_base` 描述原始 PE 基底位址。`security_cookie` 是已初始化 cookie 的客體位址；若不需要 cookie，則為 `"0x0"`。請求報告欄位為 `kind`、`device`、`file`、`byte_offset`、`code`、`irp`、`completed`、`dispatch_status`、`io_status`、`information` 和 `output_hex`。

可為空值的 `fault` 物件保留第一個後端錯誤。其 `kind`、`pc`、可為空值的 `address`、`size`、`access` 及 `interrupt` 可區分未對映或受保護的記憶體、無效範圍、無效指令及 CPU 例外。位址使用十六進位字串；大小及中斷向量使用整數。觀察讀取不能取代原始錯誤。發生錯誤的後端不能繼續執行，此記錄也不代表提供客體 SEH 處理。

`instructions` 統計執行策略已准許的客體指令嘗試次數。遭執行策略拒絕的指令不計數；已准許但在 CPU 中發生錯誤的指令計數。合成的 API 派送及返回哨兵不會增加此計數。

每個 `writes` 項目都帶有 `semantics: "attempted_guest_write"`：記錄堆疊外的 CPU 寫入嘗試，包括之後可能發生錯誤或被預算停止的嘗試。它不保證寫入已完成，也不包含 API 模型所做的寫入。裝置與驅動程式物件快照描述執行停止時觀察到的狀態。

包含 `neverd/sdk/NeverDCAPIEmulation.h`（或 C API 總標頭檔），建立工作階段，然後呼叫 `neverd_emulate_driver_json(session, path, options)`。明確傳入非空路徑會直接進入嚴格執行預檢，無須先透過通用分析 API 載入。CLI 採用此路徑。傳入 `NULL` options 使用預設值。明確指定 `neverd_driver_options_v1` 選項時，要求 `struct_size` 精確相符，且指令、記憶體、事件及逾時預算皆為正數。結果須用 `neverd_free_string` 釋放。

若路徑傳入 `NULL`，則要求工作階段已載入檔案，並獨立於 IR 分析及函式受限載入重新解析該檔案。兩種方式都會保留工作階段映像不變。呼叫期間必須保持輸入檔案可用且不變。請求／執行環境建立失敗時傳回 `NULL` 並設定 `neverd_last_error`；執行停止則傳回 JSON。未啟用此功能的建置仍提供此 API，並回報啟用方法。

`neverd_emulate_driver_scenario_json(session, path, scenario_json, options)` 使用相同的 v1 選項及所有權規則，另外接受嚴格驗證的情境輸入。必須傳入非 NULL、以 NUL 結尾的 JSON 字串。原有 `neverd_emulate_driver_json` ABI 維持不變，仍僅執行初始化。C++ 解析器 `driverOptionsFromScenarioJSON` 為 `emulateDriver` 呼叫端提供相同的情境驗證。

內部 C++ 進入點為 `include/neverd/emulation/DriverSession.h` 中的 `neverd::emulation::emulateDriver`。格式解析由現有載入器負責；Windows 物件／API 行為由 `lib/emulation/windows` 負責；CPU 狀態及執行由 Unicorn 配接器負責。配接器與模型使用相同的客體記憶體介面。Windows API 行為不應放入 Unicorn fork。
