**語言**: [English](../driver-emulation.md) | [简体中文](../zh-CN/driver-emulation.md) | [繁體中文](driver-emulation.md) | [日本語](../ja/driver-emulation.md) | [한국어](../ko/driver-emulation.md) | [Français](../fr/driver-emulation.md) | [Deutsch](../de/driver-emulation.md) | [Español](../es/driver-emulation.md) | [Italiano](../it/driver-emulation.md) | [Русский](../ru/driver-emulation.md) | [العربية](../ar/driver-emulation.md)

[← 文件索引](README.md)

# Windows 驅動程式模擬

NeverD 的選用驅動程式模擬器執行受支援 x64 WDM 驅動程式的 PE 進入點，並可在卸載前執行明確指定的循序請求情境。它使用 Unicorn 執行 CPU 指令，使用 NeverD 自有的有界 Windows 環境模型。它不會將驅動程式載入主機核心，也不會把客體 API 呼叫轉送給主機作業系統服務。

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

報告一律以 JSON 輸出至 stdout；請求與環境設定診斷輸出至 stderr。預設預算為 100000 條客體指令、64 MiB 客體記憶體、10000 筆記錄事件及 5000 毫秒。指令數限制必須為正數。執行預算耗盡便停止，並保留已收集的部分觀察結果；配置 API 仍依各自的資源不足合約傳回，包含下述 MMIO 映射的 NULL。

| 結束碼 | 意義 |
|--------|------|
| `0` | 初始化及所有請求中已完成的操作皆成功 |
| `1` | 輸入或選項無效、執行環境建立失敗，或建置時未啟用此功能 |
| `2` | 初始化或已完成的請求傳回失敗的 `NTSTATUS` |
| `3` | 情境尚未完成便停止執行，例如遇到不支援的 API、錯誤或預算限制 |

傳回失敗狀態仍表示已完整觀察到該操作的結果。成功傳回只描述這一次模型執行，不能證明該驅動程式可在 Windows 下正常運作。

## 驅動程式相容性

相容性取決於實際執行的程式碼路徑及其相依項目，而非 `.sys` 副檔名。目前的驗收證據涵蓋原創的獨立測試樣例，以及 Microsoft SIOCTL WDM 範例的緩衝、in-direct 和 out-direct 路徑，包括啟用偵錯記錄的建置；這並不代表與任意第三方驅動程式相容。

驗收也涵蓋 Pavel Yosifovich 未修改的 Zero WDM 範例，包括 direct READ/WRITE、原子統計計數及統計 IOCTL。

| 驅動程式類別或需求 | 目前範圍 | 缺少的環境 |
|--------------------|----------|------------|
| 使用下列 API 的 x64 軟體 WDM 驅動程式 | 有界 x64 WDM 初始化、循序緩衝／直接請求、工作項目、計時器、DPC、事件與等待，以及行為報告和限制 | 每個額外執行到的 API 都必須有明確的模型 |
| `METHOD_BUFFERED` IOCTL | 循序緩衝／直接 I/O，可由工作項目或 DPC 完成 | 僅支援下列 API 子集；不支援並行公開情境提交 或 WDM 請求取消 |
| `METHOD_IN_DIRECT`、`METHOD_OUT_DIRECT` | 由請求擁有的 MDL、系統對映及唯讀共用模型 PFN | 使用者對映及其他 DMA 介面 |
| 驅動程式自行配置的 MDL | 描述模型非分頁集區的獨立描述元，重複使用原始緩衝區位址 | IRP 關聯、MDL 鏈、探查／鎖定頁面及使用者對映 |
| READ/WRITE | 循序緩衝／直接 I/O，可由工作項目或 DPC 完成 | 僅支援下列 API 子集；不支援並行公開情境提交 或 WDM 請求取消；`METHOD_NEITHER` 與隱含檔案位置 |
| `METHOD_NEITHER` | 拒絕 | 使用者位址空間脈絡、存取探測、鎖定/解除鎖定及使用者記憶體故障復原 |
| KMDF 1.33 非 PnP 驅動程式 | 版本繫結、物件／內容、具名控制裝置、循序預設佇列，以及實際執行回呼的緩衝／直接請求 | 不支援 PnP 裝置、一般佇列排程、類別擴充或 UMDF |
| PnP 匯流排／功能／篩選驅動程式 | 明確的無資源或暫存器組 PDO、客體 AddDevice 與八種常見 PnP 生命週期次要功能 | 其他 PnP 操作、一般電源管理、其他硬體／資源及 KMDF PnP |
| 儲存、網路、顯示、檔案系統及迷你篩選驅動程式 | 不支援相關子系統契約 | 連接埠／類別／迷你連接埠框架、NDIS/WFP、圖形或檔案系統服務 |
| 工作項目、計時器、DPC、事件與等待 | 目前執行 IRQL 在派送與工作項目中為 `PASSIVE_LEVEL`，在 DPC 中為 `DISPATCH_LEVEL` | 僅支援下列 API 子集；不支援並行公開情境提交 或 WDM 請求取消 |
| 使用處理程序／執行緒回呼、控制代碼、登錄／檔案操作或核心模組探索的驅動程式 | 支援配置的登錄；其他行為限於下列 API | 物件管理員、系統狀態及回呼／事件產生機制 |
| 硬體、DMA、PCI、中斷或虛擬化驅動程式 | 明確暫存器組、MMIO、獨佔 latched 中斷及一致性 common／SG／channel DMA | 其他裝置模型、任意實體 RAM、PCI、連接埠、共用／電位觸發／MSI 中斷、其他 DMA 介面及特權 CPU 狀態 |
| x86 或 ARM64 Windows 驅動程式 | 拒絕 | 對應架構的載入、ABI 及執行模型 |
| x64 CFG | 驗證目標表及檢查／分派呼叫；未啟用的插樁保留客體後援函式 | XFG、匯出抑制、不支援的載入組態與 TLS 仍遭拒絕 |

未使用的不支援匯入項目可以維持繫結。一旦執行到不支援的操作，便停止並提供診斷及先前收集的觀察結果。僅 DriverEntry 成功，不能證明後續派送、硬體或框架路徑也受支援。下方 API 表是受支援子集的權威定義。

## 執行契約

此設定在 CPU0 上以確定性的合作排程模擬 x64 WDM 生命週期。 執行從 PE 進入點開始；若有編譯器產生的進入點包裝函式，也會保留並執行。DriverEntry 必須傳回 `STATUS_SUCCESS` 才能完成初始化；非零的成功狀態或待處理狀態會因初始化契約不受支援而停止。失敗狀態則保留為已完成的初始化結果。所有物件、字串、堆疊、函式指標及配置均位於客體記憶體。模型依設定的服務名稱（預設為 `NeverDDriver`）提供 `DRIVER_OBJECT` 和登錄路徑。

配接器使用 Unicorn 的虛擬 TLB 模式保留客體虛擬位址，包括規範的高位核心位址，無須合成 Windows 頁表。初始 RFLAGS 為 `0x202`；軟體裝置設定採用固定的 64 位元組快取列。這些都是本執行情境的明確屬性。行內 x64 CR8 讀取觀察到相同的 `PASSIVE_LEVEL` / `DISPATCH_LEVEL`；CR8 寫入與其他控制暫存器操作仍不受支援。

未知匯入項目繫結至延遲陷阱。未使用的匯入項目不會阻止執行；執行其 thunk 或讀取未建模的匯出資料值時，會以 `unsupported_api` 停止。不支援的 CPU 環境效果也會明確停止。NeverD 不會用成功傳回值替代未實作的呼叫。格式錯誤的映像或不支援的載入需求會在執行前失敗。

排入佇列的 `DelayedWorkQueue` 工作項目在 `PASSIVE_LEVEL` 執行，客體 DPC 回呼在 `DISPATCH_LEVEL` 接收規定的四個參數。CPU0 在呼叫傳回及阻塞等待邊界進行確定性的合作排程。相對、絕對與週期計時器使用虛擬時間；沒有可執行的框架時，時間推進至下一計時器、等待或取消期限。通知型與同步型事件／計時器保留各自的訊號消耗語意。每個回呼擁有獨立的客體堆疊；多個阻塞框架保留區域變數及完整 CPU 內容，客體記憶體仍共用。Win64 回呼入口將前四個參數放入暫存器，其餘放入堆疊。請求仍循序處理：標記 IRP 為待處理的派送函式必須傳回 `STATUS_PENDING`，且完成後才能開始下一個請求。待處理請求或無限等待沒有可用來源時，以停滯的 `model_error` 停止。指令、記憶體、觀察記錄與實際時間預算仍共用。

這是有界排程模型，不代表完整 Windows 非同步支援。可警示或使用者模式等待、系統執行緒、APC、WDM 請求取消、自旋鎖、並行公開情境提交、一般 IRQL 切換、`METHOD_NEITHER`、UMDF、KMDF PnP 裝置及一般佇列排程、完整 PnP／電源、一般硬體、其他 DMA 介面與其他中斷模式仍不支援。僅初始化呼叫會執行明確排入佇列的回呼，不會隱含產生請求或卸載。

工作項目在回呼開始前出佇列，因此回呼可釋放自身的工作項目。釋放仍在佇列中的項目、重複排入、使用失效物件或非客體可執行記憶體中的回呼位址都會明確失敗。裝置參考保留到回呼傳回。請求卸載要求釋放所有工作項目並完成佇列工作。CPU 內容保存與還原包含通用、SIMD、FPU 與控制狀態；客體記憶體始終共用，故障 CPU 不能藉還原內容繼續執行。
刪除會延後到檔案物件及排隊／執行中的工作項目參考全部釋放。物件區耗盡時，工作項目配置傳回 NULL。

映像預設使用慣用基底位址，除非情境選擇了有效的重新定位位址。映像必須為使用 native 子系統的 PE32+ x64 可執行檔。匯入可來自 `ntoskrnl.exe`、`ntkrnlmp.exe` 或 `WDFLDR.SYS`。

執行載入器支援經驗證的 x64 `DIR64` 基底重新定位，以及有限的安全性 cookie 載入組態；它會在進入點包裝函式執行前設定具確定性的客體 cookie。其他未建模的載入組態欄位、TLS、延遲／繫結匯入、依序號匯入及受控映像皆會遭拒絕。映像還必須通過嚴格的範圍與對齊檢查。

啟用的控制流程防護（CFG）會驗證 PE 旗標、指標槽與已排序的可執行目標表。檢查與分派輔助函式僅允許已宣告的映像進入點或已登記的 API 跳板，保留 Win64 呼叫狀態，並拒絕未宣告的目標。只有插樁而未啟用 CFG 時，保留原始客體後援指標。啟用的 XFG、匯出抑制和其他未建模的防護策略仍遭拒絕；位址位於可執行記憶體不代表它是合法目標。

WDM 裝置堆疊可以包含同一客體驅動程式擁有的多個裝置物件。`IoAttachDeviceToDeviceStack` 將獨立來源裝置附加到目標的目前堆疊頂端，傳回原頂端，並設定 `StackSize` 和 `AlignmentRequirement`；它不修改驅動程式的 `NextDevice` 串列，也不複製緩衝旗標。`IoDetachDevice` 接收儲存的下層裝置，要求 `PASSIVE_LEVEL`；附加允許 IRQL 不高於 `DISPATCH_LEVEL`。開啟具名下層裝置時，請求派送至目前頂端，而 `FILE_OBJECT.DeviceObject` 和報告保留具名裝置身分。READ/WRITE 使用所選頂端的緩衝旗標。儲存的請求路徑在解除附加、刪除後仍保留各層裝置，直到派送傳回；內部參考不增加表示開啟控制代碼數的 `ReferenceCount`。

`IofCallDriver` 和 `IoCallDriver` 輔助入口呼叫保留路徑中的確切目標。真正的內嵌 `IoCopyCurrentIrpStackLocationToNext`、`IoSkipCurrentIrpStackLocation` 和 `IoSetCompletionRoutine` 操作原始客體 IRP；模型驗證游標、數量及控制旗標。下層派送傳回實際狀態，與 `IoStatus` 和完成回呼傳回值分離。完成展開先推進游標，再依成功／錯誤／取消旗標選擇回呼並向上傳遞 pending；執行完成回呼時，回呼負責傳播 pending，包括派送已傳回 `STATUS_PENDING` 後的傳播。`STATUS_MORE_PROCESSING_REQUIRED` 暫停展開並保留 IRP、MDL 和緩衝區，後續完成呼叫可繼續。巢狀完成要求外層傳回停止結果，最終展開只釋放一次儲存空間。標示所屬子系統的續接保留巢狀 WDM／WDF 呼叫框架及繼承的 IRQL。 呼叫上層完成回呼之前，已消耗的下層堆疊位置會清零。

一般電源管理、驅動程式自行配置的 IRP、其他 PnP 次要功能及其他硬體／資源模型仍不支援。WDF 附加／轉送、向仍有檔案或回呼的堆疊附加裝置、移除中間層、變更轉送的主要功能及指向保留路徑以外的裝置都會明確失敗。選用的真正 WDK 範例 `driver_wdm_stack.c` 使用 `NEVERD_WDM_STACK_FIXTURE` 和 `NEVERD_WDM_STACK_CFG_FIXTURE`；原生與 C API／CLI 測試包含重新定位，缺少產物時明確略過。執行證據仍僅來自 Linux。

情境可明確設定 `pnp_devices`，最多 64 個。每項必須包含 `id`、`bus: "resource_free"` / `bus: "register_bank"`、`initial_device_power: "D0"` 與 `initial_system_power: "working"`，不會猜測遺漏事實。ID 為區分大小寫的 ASCII，長度 1–64 位元組，以字母或數字開頭，其餘僅允許字母、數字、`_`、`-`、`.`。一般要求可用已設定的 `device_id` 取代 `device`，兩者互斥。`kind: "pnp"` 必須提供 `device_id`、`minor` 與 `bus_completion`；支援 `start`、`query_remove`、`cancel_remove`、`remove`、`query_stop`、`stop`、`cancel_stop`、`surprise_removal`。`bus_completion.status` 必須為 32 位元整數或十六進位字串；選用 `delay_100ns` 是不超過 INT64_MAX 的非負整數，自提供者實際收到要求時計時。最終匯流排狀態不能是 `STATUS_PENDING`；stop/cancel-stop/surprise-removal/cancel-remove/remove 必須精確傳回 `STATUS_SUCCESS` (0)。PnP 要求拒絕檔案、傳輸及取消欄位，即使值為零；C++ API 執行相同預檢。

```json
{
  "pnp_devices": [
    {"id": "sensor0", "bus": "resource_free", "initial_device_power": "D0", "initial_system_power": "working"}
  ],
  "requests": [
    {"kind": "pnp", "device_id": "sensor0", "minor": "start", "bus_completion": {"status": "0x0", "delay_100ns": 10}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "query_stop", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "cancel_stop", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "query_stop", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "stop", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "start", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "query_remove", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "cancel_remove", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "surprise_removal", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "remove", "bus_completion": {"status": "0x0"}}
  ],
  "unload": true
}
```

DriverEntry 成功後，每個設定的 PDO 執行一次 `AddDevice`，提供者擁有獨立的 `DRIVER_OBJECT`；客體不能刪除或冒充提供者物件。PnP IRP 為 `KernelMode`、不關聯檔案，資源依設定的匯流排提供，初始狀態為 `STATUS_NOT_SUPPORTED`。僅轉送至對應 PDO 後才使用匯流排回應；延遲完成沿用虛擬時鐘與既有完成續接。最終上層完成決定生命週期提交或回復，與匯流排狀態分開。正常自 Started 移除必須先成功 query、關閉檔案、排空先前要求，並由客體拆鏈／刪除。乾淨的 AddDevice 失敗僅退役提供者；新客體裝置洩漏會觸發 `model_error`，已拆鏈者亦同。卸載前所有提供者必須已退出。本範圍不實作其他 PnP 次要功能、一般電源管理、其他硬體／資源或 KMDF PnP。 成功 PnP 必須實際完成提供者；START/QUERY_STOP/QUERY_REMOVE 的上層早期失敗可保留空匯流排觀測。裝置／檔案生命週期身分在拆鏈後仍保留。

報告於 `configuration.pnp_devices` 保留初始設定。觀測的 `pnp_devices` 包含 `id`、`pdo`、可空 `add_device_status`、目前 `attached`、`pnp_state` 與 `provider_present`；移除後 `attached` 為 false。AddDevice 階段為 `add_device:<ID>`，失敗影響 `scenario_success`，但不覆寫 DriverEntry 的 `nt_status`。每個要求新增可空 `device_id` 與 `pnp`；PnP 的 `file` 為 null。`pnp` 記錄 `minor`、`state_before`、`state_after`、可空 `bus_status`、`bus_received_at_100ns` 與 `bus_completed_at_100ns`。設定狀態僅在匯流排實際完成後成為觀測；接收時間獨立記錄。既有要求欄位型別不變。

除了 Removing/Removed，裝置仍存在時，一般 CREATE/READ/WRITE/IOCTL/CLEANUP/CLOSE 會進入真正客體派送。模型不根據 Stopped、StopPending、RemovePending 或電源狀態虛構失敗；驅動程式可依自身程式碼完成軟體 I/O、拒絕或保留要求。公開執行器仍為循序；目前保留 IRP 若無可用生產者，不能靠後續情境中的 start 或 cleanup 喚醒，會以停滯 `model_error` 結束。Remove 前關閉檔案、排空先前要求是目前設定的限制。`query_stop` 的最終 `STATUS_RESOURCE_REQUIREMENTS_CHANGED` (0x119) 要求尚未實作的資源重新查詢，因此情境預檢與客體最終完成都明確拒絕；參見 [Microsoft QUERY_STOP 合約](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/irp-mn-query-stop-device)。停止／重啟及突然移除不代表一般資源重新配置、一般電源管理或 KMDF PnP 支援。

無資源 PnP 使用原創真正 WDK `driver_wdm_pnp.c`、選用的 `NEVERD_WDM_PNP_FIXTURE`／`NEVERD_WDM_PNP_CFG_FIXTURE`，並提供原生及 C API／CLI 測試；缺少產物時明確略過，執行證據仍僅來自 Linux。

WDM remove-lock 使用真正匯出 `IoInitializeRemoveLockEx`、`IoAcquireRemoveLockEx`、`IoReleaseRemoveLockEx` 和 `IoReleaseRemoveLockAndWaitEx`；不帶 Ex 的 WDK 名稱是巨集。鎖屬於完整對齊儲存所在延伸區的確切 DEVICE_OBJECT，與 PDO 生命週期或 Tag 形狀無關；附加前即可初始化。支援 retail 32 位元組及 DBG 120 位元組，須傳入符合的獨立大小參數，登記後整個區域不透明。NULL 和重複 Tag 依每把鎖計數，Tag 從不解參考，因此 IRP 完成後仍可釋放。初始化與 AndWait 要求 `PASSIVE_LEVEL`，acquire/release 允許 `DISPATCH_LEVEL`。

AndWait 關閉取得入口、釋放一次符合的取得，並暫停真正客體框架直到其餘取得全部釋放；之後 acquire 傳回 `STATUS_DELETE_PENDING` 且不產生釋放義務。最後一次 release 在釋放回呼傳回前鎖存就緒，允許工作項目 release 後等待 REMOVE 續接送出的事件。模型不製造回呼、逾時或無生產者的成功。目前 AndWait 要求擁有者位於關聯的活動 REMOVE 路徑且提供者已實際接收（`bus_received_at_100ns` 可為零），不需等待下層完成。下層在到達提供者前將 REMOVE 排入佇列仍不支援；這不是完整 OutsideRemoveDevice／Driver Verifier。REMOVE 前仍須關閉檔案並排空先前要求，但允許尚未結束的回呼釋放鎖。保留路徑涵蓋等待、客體拆鏈／刪除及下層 pending；相關回呼框架全數傳回後才最終釋放路徑。

未知或大小不符的儲存、不符 release、重複 drain、重新初始化，以及仍有取得或未消耗 drain 等待時刪除延伸區，均在修改前失敗。乾淨的 AddDevice 失敗可刪除已初始化但未使用的鎖。鎖不取代真正裝置／工作項目參考，只在延伸區實體退役時取消登記；有效除錯中繼資料不啟用 Verifier 逾時／高水位行為。真正 WDK `driver_wdm_remove_lock.c` 以 `NEVERD_WDM_REMOVE_LOCK_FIXTURE`／`NEVERD_WDM_REMOVE_LOCK_CFG_FIXTURE`／`NEVERD_WDM_REMOVE_LOCK_DBG_FIXTURE`／`NEVERD_WDM_REMOVE_LOCK_DBG_CFG_FIXTURE` 提供四種變體。缺少產物明確略過；原生及 C API／CLI 證據僅來自 Linux，不代表完整移除管理或一般並行 I/O 排空。

合成 `bus: "register_bank"` 為 PDO 加入明確的固定記憶體資源。若提供 `resources` 陣列，則包含 `id`、`raw_start`、`translated_start`、`length` 和 `registers`；每個暫存器必須有 `offset`、`width`、`access`（`read_only` 或 `read_write`）及初始 `value`。`DriverResources.h`／`DriverResources.def` 定義 C++ 與 JSON 的共用契約。資源 ID 遵循有界 ASCII 識別碼規則，在每個 PDO 內唯一。限制為每 PDO 8 個／總計 32 個資源、每資源 256 個／總計 4096 個暫存器，以及每資源 1–1048576 位元組。僅支援自然對齊、精確 1／2／4 位元組存取，數值必須符合寬度。兩種實體區間皆不可溢位；同 PDO 的原始範圍不可重疊，轉譯範圍則全域不可重疊。空 `registers` 明確表示整組皆不可存取。位址與初值是宣告，不是主機硬體或預設填零的記憶體。`resource_free` 維持省略資源清單與 null START 指標。

START 取得分開配置且唯讀的原始與轉譯 `CM_RESOURCE_LIST`，包含順序對應的 Memory 描述子：一個完整描述子、Internal 介面、匯流排 0、版本／修訂 1、DeviceExclusive 共用方式和 READ_WRITE 範圍旗標。個別暫存器的 RO 權限獨立存在。下層 START 成功後，資源配置先於上層完成回呼可用；每次從 NotStarted／Stopped 發起 START 都以相同固定配置建立新的資源世代。暫存器值只在每個 PDO 初始化一次，解除映射、STOP 和重新啟動皆保留。START 失敗及 STOP／REMOVE 成功時，驅動程式必須在 IRP 最終完成前釋放映射，不會靜默清理。意外移除立即禁止新映射及暫存器存取，但仍允許解除既有映射。提供者實際成功完成裝置 SET 時變更硬體可存取性：D3 阻止存取，D0 僅在資源配置可用時允許存取；D3 仍可建立映射但不可存取暫存器，電源變更不會丟棄映射或重設數值。

`MmMapIoSpace` 支援 NonCached；`MmMapIoSpaceEx` 支援 PAGE_NOCACHE 搭配 PAGE_READONLY 或 PAGE_READWRITE。兩者僅接受單一配置內已宣告的轉譯子範圍，保留頁內偏移。映射別名共用一組暫存器、保有獨立權限，並要求 `MmUnmapIoSpace` 使用完全相同的原始基址與長度。映射數量、視窗或設定的記憶體預算耗盡時傳回 NULL；後端錯誤仍明確失敗。已解除的位址不會因之後建立映射而重新有效。純量與真正 REP 暫存器緩衝區指令經過 CPU MMIO 檢查；未宣告區域、錯誤寬度、不對齊、RO 寫入、跨映射存取及執行都會在影響暫存器前失敗。模型 API 的記憶體存取也必須符合單次對齊的 1／2／4 位元組交易；較大的範圍會明確失敗，不會拆成多次暫存器存取。不實作任意實體 RAM、資源重新平衡、連接埠、其他中斷模式、其他 DMA 介面或一般硬體行為。

可執行的[暫存器組情境](../examples/driver-register-bank-scenario.json) 透過選用 `NEVERD_WDM_RESOURCE_FIXTURE`／`NEVERD_WDM_RESOURCE_CFG_FIXTURE` 使用原創且由真正 WDK 建置的 `driver_wdm_resources.c`。它執行 14 個要求，涵蓋延遲 START、檔案 I/O、STOP、重新啟動與移除，並由驅動程式 IOCTL 觀測持久數值。`configuration.pnp_devices[].resources` 以無損十六進位實體位址記錄初始事實，不是第二份暫存器組狀態報告。C API 與 Python 沿用既有 `scenario_json` 入口和未變更的 `neverd_driver_options_v1`。缺少真正產物時明確跳過；目前執行證據僅來自 Linux。

相同 `register_bank` 提供者可單獨宣告 `interrupts`，或與 `resources` 一起宣告；至少一份清單必須非空。`resource_free` 拒絕明確提供的 `interrupts` 欄位，包含 `[]`。每個中斷必須有 `id`、`raw_vector`、`raw_level`、`raw_affinity`、`translated_vector`、`translated_level`、`translated_affinity`、`mode: "latched"` 及 `share: "device_exclusive"`。`DriverInterrupts.h`／`DriverInterrupts.def` 定義型別、拼法及限制：每 PDO 8 個／總計 32 個中斷、每 PDO 內唯一的有界 ASCII ID、全域獨佔的轉譯向量、原始層級 0–65535 與轉譯 DIRQL 3–12。向量保留完整 32 位元，不猜測向量與 IRQL 的關係。兩個 affinity 遮罩都必須是 1；CPU0 與群組0 是提供者明確事實。原始與轉譯值相互獨立。記憶體描述元保留順序，接著在同一 `CM_RESOURCE_LIST` 內排列中斷描述元；中斷使用型別 2、DeviceExclusive 共用設定及 LATCHED 旗標。此合成邊緣觸發設定不代表 PCI 共用電位觸發線路。

READ／WRITE／IOCTL 要求可宣告 `interrupt_events`，每項明確提供 `after_100ns`、`device_id` 及 `interrupt_id`；其他要求種類拒絕此欄位，即使為空。最多每要求 64 個／總計 1024 個事件，延遲為不超過 INT64_MAX 的非負值。要求成功提交時確定延遲起點，並擷取已連接中斷及其 PDO 資源世代。事件是獨立外部線路脈衝：來源 IRP 完成不會取消事件，寫入暫存器也不會推導 enable、status 或 acknowledgment 行為。時間只在閒置時推進；到期事件於下一個支援的回呼邊界執行，因此 `after_100ns: 0` 不承諾指令級搶占或在第一條客體指令前遞送。同一時刻的產生者一起接受容量預檢；提供者硬體狀態發布先於資格檢查，ISR 回呼先於 DPC／工作項目。失去連線、過期／不可用世代或實體 D3 會記錄 `undelivered_reason` 並以 `model_error` 停止；事件不會重新綁定或默默延後。

`IoConnectInterrupt` 使用實際十一個引數及完全相符的轉譯配置。`IoConnectInterruptEx` 支援 FullySpecified（1）、LineBased（2，明確 PDO 上的一條配置線路）及 FullySpecifiedGroup（4，群組 0）；`IoDisconnectInterruptEx` 要求相符版本／內容。註冊與斷開都要求 PASSIVE_LEVEL。僅支援中斷私有鎖、獨佔 latched 模式、CPU0／群組0，且不保存浮點狀態。同步 IRQL 必須等於配置的 DIRQL；LineBased 的零值選用該層級。`KINTERRUPT` 不透明，其位址永不重用。真正 ISR 接收 `(Interrupt, ServiceContext)`，從 AL 傳回 BOOLEAN；`FALSE` 表示未認領，不是 NTSTATUS 失敗。`KeSynchronizeExecution` 在同一把鎖與 DIRQL 下執行真正單引數回呼，傳回 BOOLEAN 並恢復呼叫者 IRQL／CR8。`KeAcquireInterruptSpinLock`／`KeReleaseInterruptSpinLock` 強制不可遞迴所有權、原始執行身分及儲存的 IRQL；未釋放的鎖不能跨越回呼返回。中斷等待、呼叫者提供的共用鎖、共用／電位觸發／MSI／被動中斷與指令級搶占仍不支援。失敗 START 與成功 STOP／REMOVE 必須在最終完成前斷開連線；上層完成回呼可先清理，而斷開連線絕不默默丟棄已排入佇列的 DPC。

報告分開記錄宣告與觀測。`configuration.pnp_devices[].interrupts` 保留資源，`configuration.interrupt_events` 將輸入事件展平，帶有從零開始的 `source_request_index`（設定要求索引）及 `event_index`。根層級 `interrupts` 資料列加入 `device_id`、`interrupt_id`、`epoch`、絕對 `due_at_100ns`，以及可為 null 的 `occurred_at_100ns`、`delivered_at_100ns`、`returned_at_100ns`、`interrupt_object`、`return_value`、`claimed`、`undelivered_reason`。`claimed` 僅由實際傳回值的低位元組推導；時間戳記是觀測，不會虛構回呼結果。`scenario_success` 要求每個設定事件都返回且沒有未遞送失敗；未認領的 ISR 仍有效。DPC 效果透過真正要求完成、API 呼叫及訊息呈現。可執行的[中斷情境](../examples/driver-interrupt-scenario.json) 使用原創、真正 WDK 建置的 `driver_wdm_interrupts.c` 及選用 `NEVERD_WDM_INTERRUPT_FIXTURE`／`NEVERD_WDM_INTERRUPT_CFG_FIXTURE`：七個要求包含延遲 START、由 ISR→DPC 完成的 pending IOCTL、檔案清理／關閉與移除。既有 C／Python `scenario_json` 邊界及 `neverd_driver_options_v1` 版面維持不變。缺少真正產物時明確跳過；執行證據僅來自 Linux。

`DriverDMA.h`／`DriverDMA.def` 為 `register_bank` PDO 增加選用的 `dma` 物件，與記憶體／中斷配置並存；DMA 本身不能取代這兩類資源。七個欄位都必須明確提供：`address_bits`（32 或 64）、`maximum_length`（1–1048576 位元組）、`map_registers`（1–256）、`alignment`（1–4096 之間的二次方）、`logical_base`（非零且頁面對齊）、`logical_length`（頁面對齊，4096–1073741824 位元組）與布林值 `scatter_gather`。邏輯位址視窗不得溢位，且必須符合位址寬度。每個 PDO 都有獨立邏輯定址域，不同裝置的相同位址不會互為別名。所有轉譯 MMIO 資源均須避開模型 RAM 保留範圍 `[0x1000000000, 0x1000100000)`，即使未設定 DMA 也一樣。這些宣告描述一致性的合成匯流排主控裝置，絕非主機實體記憶體或 PCI 裝置。

`IoGetDmaAdapter` 接受 Internal 匯流排主控的歷史 `DEVICE_DESCRIPTION` 版本 0／1 欄位，並發布版本一 `DMA_ADAPTER` 及實際 104 位元組 `DMA_OPERATIONS` 表。版本 2／3 探查傳回 NULL，不讀取現代描述的尾端。每個間接方法繫結至確切有效的 adapter，與核心匯入分開。已實作 `AllocateCommonBuffer`、`FreeCommonBuffer`、`GetDmaAlignment`、`GetScatterGatherList`、`PutScatterGatherList`、`PutDmaAdapter`、`AllocateAdapterChannel`、`MapTransfer`、`FlushAdapterBuffers` 與 `FreeMapRegisters`。`FreeAdapterChannel` 和 `ReadDmaCounter` 仍是具名拒絕項目，因為此設定檔沒有從屬／系統 DMA 控制器。common buffer 配置／釋放及對齊查詢要求 PASSIVE_LEVEL；Get／PutScatterGatherList 要求 DISPATCH_LEVEL，adapter 釋放允許最高 DISPATCH_LEVEL。x64 忽略 `CacheEnabled`。不支援的版本探查、不相容的宣告能力與有文件依據的配置不足傳回 NULL；格式錯誤、未建模介面選擇及後端失敗仍明確報錯。

`KernelPhysicalMemory` 最多為既有 RAM 指派 256 個 4096 位元組的模型實體頁面。CPU 虛擬位址、實體頁面身分與裝置邏輯位址彼此不同。已建立 MDL 的 PFN 陣列以唯讀方式公開共用身分；未建立的描述元沒有可用 PFN。相鄰小配置可共用 PFN，但保留獨立位元組範圍與生命週期。common buffer、pool 與請求緩衝區使用 `GuestMemory` 原已擁有的同一份位元組，不另建 DMA 副本。有效 SG 映射會固定其確切資料範圍與描述元；完成、pool／MDL 釋放和拆除均先檢查仍有效的依賴。解除直接 MDL 映射只撤銷 CPU 系統映射，DMA 仍能存取其鎖定儲存。`DmaWritable` 將寫入鎖定契約與 CPU 映射權限分開記錄：裝置寫入要求直接 READ／OUT_DIRECT 或可寫的非分頁儲存；WRITE／IN_DIRECT 不會因 CPU 映射可寫而取得此權限。

`GetScatterGatherList` 依 MDL 原始範圍驗證 CurrentVa／Length，在既有儲存上建立邏輯頁面片段。map register 可用時，真正的四引數 void `AdapterListControl` 可在 API 返回前直接執行；否則保留資料／描述元與回呼名額，等待 PDO 的 FIFO 取得資源。本設定沒有 StartIo 所有權，所以回呼第二個 IRP 引數為 NULL。回呼返回不會釋放映射。`PutScatterGatherList` 可在回呼內執行；Put 後驅動程式可完成請求並釋放最後一個 adapter，而回呼續接與裝置參考持續至返回。釋放 common buffer 必須提供原始 adapter、長度、邏輯位址及 CPU 位址。工作階段內不重用邏輯位址，重新啟動也一樣。缺乏真正生產者的資源等待會明確停滯，不虛構完成或期限。 有效 SG 映射持有裝置資料所有權期間，CPU 必須先 Put 才能存取資料；仍在等待 map register 的回呼尚未交出位元組的裝置所有權。

`AllocateAdapterChannel` 要求 DISPATCH_LEVEL，並保留不透明、非 NULL 的 map-register token。common buffer、SG list 與 channel reservation 共用每個 PDO 的配額與 FIFO。成功代表接受真正直接或排隊執行的 `AdapterControl`；要求數量過大時傳回 `STATUS_INSUFFICIENT_RESOURCES`，不呼叫回呼。每個客體裝置只允許一個尚未結束的配置回呼；禁止從 AdapterControl 呼叫 AllocateAdapterChannel，包含透過巢狀回呼重入。四個回呼參數包含註冊時擷取的實際 `DEVICE_OBJECT.CurrentIrp`。客體裝置中這個確切的八位元組欄位可寫，接受零或路徑包含該裝置的有效 IRP。排隊回呼保留該封包直到進入回呼，之後回呼可完成它。設定檔仍沒有 StartIo；另一個 SG 回呼未使用的 IRP 參數仍為 NULL。

`AdapterControl` 傳回 32 位元 `IO_ALLOCATION_ACTION`；RAX 高位會被忽略，動作不會取代 AllocateAdapterChannel 的 `STATUS_SUCCESS`。`DeallocateObject` 在回呼返回時釋放未使用或已完整 flush 的 registers。`DeallocateObjectKeepRegisters` 保留它們，直到 `FreeMapRegisters` 使用完全相符的 adapter、token 與原始數量。`KeepObject` 需要未建模的系統控制器，因此明確失敗。剛交付的配置不能在其回呼返回前當作已保留配置釋放；另一筆先前保留的配置則可依其自身契約釋放。回呼身分、保留的 registers 與已映射位元組各有獨立壽命，在 adapter 或裝置拆除前均會檢查。

`MapTransfer` 與 `FlushAdapterBuffers` 允許最高 DISPATCH_LEVEL；channel 配置與 register 釋放要求 DISPATCH_LEVEL。MapTransfer 接受相對於 MDL 的索引、讀寫真正 ULONG 長度，並按值傳回邏輯位址。有限 SG 設定檔每次傳回一個底層頁面片段；同一 MDL、同一方向的後續連續索引會延伸同一操作。非 SG 在保留數量足夠時一次映射整個要求範圍，不縮短長度。首次映射依 register reservation 大小保留不重用的邏輯視窗；其他配置可交錯發生而不重疊。所有片段共用原地擴展的單一實體固定範圍。裝置交易可跨越目前整個已映射操作，但 CPU 存取、MDL 釋放及會退役固定儲存的完成操作，均須等到整體 flush。Flush 必須符合起始索引、MDL、方向與實際映射總長度。它釋放映射位元組而保留 registers，使相同 token 能支援下一操作。部分 flush、混用 MDL 與其他 MapTransfer 模式不在此設定檔範圍內；這不表示它們在所有 Windows 系統上都不合法。

`KeFlushIoBuffers` 驗證有效且已鎖定／非分頁的 MDL。建模平台具快取一致性，因此 ReadOperation 或 DmaOperation 的任一值都不需要另一份快取複本；此呼叫不會釋放 DMA 所有權，也不能取代 FlushAdapterBuffers。[Channel 情境](../examples/driver-dma-channel-scenario.json) 執行原創 `driver_wdm_dma_channel.c`：兩次 MapTransfer、一次跨頁裝置交易、另行宣告的 IRQ／DPC、整體 flush 與確切 register 釋放。真正 normal／CFG 映像使用 `NEVERD_WDM_DMA_CHANNEL_FIXTURE` 和 `NEVERD_WDM_DMA_CHANNEL_CFG_FIXTURE`。

只有 READ／WRITE／IOCTL 接受 `dma_events`。每個事件都要求 `after_100ns`、`device_id`、`logical_address`、`direction` 與 `length`；`write_memory` 另要求完全符合長度的 `data_hex`，`read_memory` 則禁止該欄位。方向以裝置觀點定義。上限為每請求 64 個、總計 1024 個事件、總交易資料 16 MiB、每筆 1 MiB；延遲範圍為非負值至 INT64_MAX。提交會擷取 PDO 目前配置世代並固定虛擬起始時間，但不要求即將執行的派送已建立映射。遞送時解析完整有效的邏輯範圍和方向，要求實體 D0，並在任何效果前驗證全部儲存位元組。來源 IRP 完成不會取消事件。不存在／已釋放映射、舊世代、意外移除或 D3 都會記錄失敗並停止；事件不重新繫結，也不虛構中斷、暫存器協定或 IRP 完成。同一排程邊界依序發布提供者硬體狀態、DMA 位元組，再處理獨立宣告的中斷脈衝。時序仍採協作式排程，不是指令級搶占。

報告保留 `configuration.pnp_devices[].dma` 與攤平的 `configuration.dma_events`。根層 `dma_transfers` 每筆記錄 `source_request_index`、`event_index`、`device_id`、`epoch`、`logical_address`、`direction`、`length`、`due_at_100ns`，以及可為 null 的 `occurred_at_100ns`、`completed_at_100ns`、`mapping`、`adapter`、`failure_reason`；`data_hex` 含實際傳輸位元組。每筆宣告的交易都必須完成且無失敗，`scenario_success` 才成立。[DMA 情境](../examples/driver-dma-scenario.json) 使用原創、以真正 WDK 建置的 `driver_wdm_dma.c`，選用路徑為 `NEVERD_WDM_DMA_FIXTURE`／`NEVERD_WDM_DMA_CFG_FIXTURE`，透過真正 adapter 指標操作 common buffer，並由另行宣告的 ISR→DPC 完成。C／Python 繼續使用 `scenario_json`，不變更 `neverd_driver_options_v1`。缺少產物時明確跳過；證據僅限 Linux。從屬控制器、V2／V3 方法、硬體描述元引擎、一般 KMDF DMA 與其他裝置模型仍不支援。

合成匯流排 WDM 電源要求使用 `kind: "power"` 和已設定的 `device_id`。每個封包必須明確提供 `minor`（`query`／`set`）、`power_type`（`device`／`system`）、`power_state`（`D0`／`D3` 或 `working`／`sleeping3`）、`power_action`（`none`／`sleep`）、32 位元整數或十六進位字串 `system_context` 及 `bus_completion`。不支援 System Query 到 Working；拒絕檔案、傳輸及取消欄位。完整 `system_context` 保留為不透明事實，不推斷父要求、休眠或快速啟動。路徑必須具有 `DO_POWER_PAGABLE` 且沒有 `DO_POWER_INRUSH`；目前電源派送及 `PoRequestPowerIrp` 在 `PASSIVE_LEVEL` 執行。`PoCallDriver` 轉送同一受管理的電源 IRP；`PoStartNextPowerIrp` 遵循 Vista+ 無額外序列化交握的契約。一般電源原則、WAIT_WAKE、其他狀態／動作、關機／休眠、浪湧、不可分頁路徑、一般硬體及 KMDF PnP 仍不支援。

每個 `pnp_devices` 項目可提供 `initial_reported_device_power: "D0"` 或 `"D3"`，與必填的初始生命週期 D0／working 獨立。PDO 與首次關聯的每個客體 DEVICE_OBJECT 各有通知狀態；`PoSetPowerState` 只傳回並更新呼叫裝置的前值。缺少此事實時實際呼叫失敗，不猜測 D0。選用的 `requested_device_power` 保存相同六項必填事實的 device 類型範本；所有 PDO 合計最多 64 項。只有真正 `PoRequestPowerIrp` 的 PDO、minor 和目標符合該 PDO 的 FIFO 首項才消耗；缺失／不符報錯，未消耗項目不產生要求，也不從回呼 context 猜父要求。子要求有獨立 IRP 與報告列，`origin: "PoRequestPowerIrp"` 及零起算 `response_index`；情境列使用 `origin: "scenario"` 和空索引。同步子要求可在 API 傳回 `STATUS_PENDING` 前執行五參數 void 回呼；回呼可等待，System S0 可先於獨立 D0 子要求完成。回呼的 IO_STATUS_BLOCK 快照有效至回呼傳回。

要求報告增加可空的 `power`，電源列的 `file` 為 null。`power` 記錄封包事實、`device_state_before`／`device_state_after`、`system_state_before`／`system_state_after`、可空的 `requested_device_object` 及實際 `bus_status`／`bus_received_at_100ns`／`bus_completed_at_100ns`。最終 PnP 裝置增加 `device_power`／`system_power`；存活裝置增加可空的 `reported_device_power`。`scenario_success` 只用情境來源列核對設定數量，但所有實際情境／子要求都須成功完成；未消耗範本不導致失敗。真正 `driver_wdm_power.c` 使用 `NEVERD_WDM_POWER_FIXTURE`／`NEVERD_WDM_POWER_CFG_FIXTURE`，涵蓋普通／active-CFG 原生與 C API／CLI 路徑；缺少產物明確略過，執行證據僅來自 Linux。

[完整電源情境](../examples/driver-power-scenario.json) 可透過 `--scenario` 執行真正範例，包含啟動、系統查詢／睡眠／喚醒、移除及三個明確子回應。

KMDF 1.33 支援使用精確的 1.33.0 ABI：458 個函式槽具有穩定的客體識別，下列 38 個 API 實作了執行語義。`WdfVersionBind` 與 `WdfVersionUnbind` 在真實 WDK `FxDriverEntry` 包裝函式前後管理客體繫結。`WdfGetDriver` 讀取公用驅動程式全域結構。非 PnP 驅動程式、一般物件、控制裝置、佇列和傳入請求共用具型別內容、參考計數，以及實際執行的清理／銷毀／卸載回呼。所有已建模的框架呼叫與回呼目前都要求 `PASSIVE_LEVEL`；清理完成後新增參考仍不在此設定的支援範圍內。未建模的函式槽、`WdfLdrQueryInterface`、類別擴充和 UMDF 會明確停止。

控制裝置要求複製可列印 ASCII 名稱，且 SDDL 必須精確為 `D:P(A;;GA;;;WD)`。這授予所有呼叫端存取權限，無需虛構呼叫端權杖；不支援其他安全描述元、未命名裝置和自動名稱。裝置初始化擁有一個 WDM 裝置。請求可透過現有工作階段命名空間中的符號連結別名 `\DosDevices\Name` 或 `\??\Name` 選擇裝置，報告仍保留正規裝置名稱。建立成功會消耗初始化物件並清空其指標；失敗則回復部分裝置擁有權。`WdfControlFinishInitializing` 決定何時可以遞送 I/O。僅在已建模的檔案、工作項目和請求允許時，刪除操作才移除裝置及其連結；不支援刪除過程中取消或排空請求。

96 位元組的 `WDF_IO_QUEUE_CONFIG` 支援循序預設佇列，要求明確被動執行且不使用框架同步。控制裝置佇列不參與電源管理。專用 READ／WRITE／IOCTL 回呼優先於預設回呼。已接受的佇列請求即使同步完成也傳回 `STATUS_PENDING`；void 回呼的傳回暫存器不會使請求完成。延後完成使用現有排程器。沒有處理函式時，請求以 `STATUS_INVALID_DEVICE_REQUEST` 完成；未啟用零長度遞送時，零長度 READ／WRITE 直接完成。預設檔案套件以成功狀態和 Information=0 完成 CREATE／CLEANUP／CLOSE。不支援並行／手動佇列、檔案回呼、PnP 裝置和完整 PnP／電源。

請求參數使用 40 位元組的 `WDF_REQUEST_PARAMETERS` 配置。輸入／輸出存取函式傳回邏輯長度，保留緩衝區別名及現有直接 I/O 的 MDL 對映；直接 IOCTL 的輸入仍使用緩衝區。方向錯誤或緩衝區不足會傳回文件規定的狀態。完成操作先執行請求清理和子物件銷毀，再使 IRP／緩衝區失效，最後在參考允許時銷毀請求。一旦開始完成操作，就拒絕新的緩衝區與參數存取函式呼叫；已取得的緩衝區指標在清理期間仍可使用。外部物件參考保留內容，但不保留對已完成 IRP 的存取權。使用者模式 `METHOD_NEITHER` 仍需要尚未實作的呼叫端內容／探測／鎖定支援。

取消支援限於上述控制裝置佇列中的請求。若取消已經發生，`WdfRequestMarkCancelableEx` 傳回 `STATUS_CANCELLED`，不會呼叫取消回呼。`WdfRequestUnmarkCancelable` 成功後會移除回呼；之後發生的取消只記錄已取消狀態，不再遞送該回呼。`WdfRequestIsCanceled` 可在未標記為可取消的存活請求上讀取此狀態。成功標記為可取消後，完成請求需要成功解除標記，或取消回呼已開始遞送；僅排入佇列仍不允許完成。回呼開始後可與工作項目協調完成，包括回呼正在等待的情況。獨立的內部參考保留請求直到取消回呼傳回；完成操作仍先使 IRP 失效，最終的請求銷毀接續本身也可等待。排程優先順序為 DPC、按 FIFO 排列的取消回呼、一般工作項目；排入佇列的取消回呼也先於就緒的被動層級等待框架恢復。

請求已取消時，舊版 void `WdfRequestMarkCancelable` 會在傳回前同步執行客體取消回呼。此子接續可以等待，透過巢狀清理完成請求，並在原 API 恢復前執行最終銷毀。註冊後才發生的取消仍走上述排程路徑。這重現了 `PASSIVE_LEVEL` 與 `WdfSynchronizationScopeNone` 下的公開原始碼行為；[Microsoft](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfrequest/nf-wdfrequest-wdfrequestmarkcancelable) 要求不使用自動同步的驅動程式選擇 Ex。此處執行舊版 API 是相容行為，並非建議在該設定下使用它。

`WdfRequestGetInformation` 與 `WdfRequestSetInformation` 共用原 IRP 中的 64 位元 `IoStatus.Information`，與客體直接寫入保持一致。Set 只指派數值，傳輸長度在完成時才驗證。`WdfRequestCompleteWithInformation` 在清理前寫入同一欄位；清理回呼透過事先儲存的 IRP 修改此值後，最終 Information 使用修改後的值，即使此階段 GetInformation 已傳回零。`WdfRequestGetIoQueue` 傳回來源佇列。預設檔案設定下，`WdfRequestGetFileObject` 傳回 NULL，不會把 WDM FILE_OBJECT 偽裝成 WDF 檔案物件。`WdfRequestWdmGetIrp` 傳回同一個 IRP；客體不能透過 `IoCompleteRequest`／`IofCompleteRequest` 繞過 WDF 完成流程。完成過程中或完成後，只要請求控制代碼仍有效，GetInformation／GetIoQueue 傳回零；MDL 擷取先將有效輸出槽清為 NULL，再傳回 `STATUS_INTERNAL_ERROR`。此時 SetInformation／GetFileObject／WdmGetIrp 仍遭拒絕，既有的緩衝區與參數存取限制不變。

`WdfRequestRetrieveInputWdmMdl` 與 `WdfRequestRetrieveOutputWdmMdl` 視需要為緩衝 WRITE 輸入、READ 輸出及 IOCTL 輸入／輸出描述現有 SystemBuffer。每次擷取均先驗證方向有效且長度非零，再使用該請求唯一的快取描述元；首次成功擷取決定 ByteCount，即使另一方向的邏輯長度不同也保留此值。對此描述元，`MmGetSystemAddressForMdlSafe` 傳回原 VA；額外對映、解除對映及驅動程式釋放均遭拒絕。直接 READ 輸出、WRITE 輸入及 IOCTL 輸出傳回現有 `IRP.MdlAddress`，擷取本身不建立對映；直接 IOCTL 輸入使用 SystemBuffer 快取。描述元、IRP 和緩衝區在完成時一同失效。取消內部參考或外部參考只保留 WDF 內容，不保留已完成的 I/O 儲存空間。`METHOD_NEITHER` 與未建立 MDL 的 PFN 存取仍不支援。

已建模的 KMDF API: `WdfDriverCreate`, `WdfDriverGetRegistryPath`, `WdfDriverWdmGetDriverObject`, `WdfWdmDriverGetWdfDriverHandle`, `WdfObjectGetTypedContextWorker`, `WdfObjectAllocateContext`, `WdfObjectContextGetObject`, `WdfObjectReferenceActual`, `WdfObjectDereferenceActual`, `WdfObjectCreate`, `WdfObjectDelete`, `WdfControlDeviceInitAllocate`, `WdfDeviceInitFree`, `WdfDeviceInitAssignName`, `WdfDeviceInitSetIoType`, `WdfDeviceCreate`, `WdfDeviceCreateSymbolicLink`, `WdfControlFinishInitializing`, `WdfDeviceWdmGetDeviceObject`, `WdfIoQueueCreate`, `WdfDeviceGetDefaultQueue`, `WdfIoQueueGetDevice`, `WdfRequestComplete`, `WdfRequestCompleteWithInformation`, `WdfRequestGetParameters`, `WdfRequestRetrieveInputBuffer`, `WdfRequestRetrieveOutputBuffer`, `WdfRequestRetrieveInputWdmMdl`, `WdfRequestRetrieveOutputWdmMdl`, `WdfRequestSetInformation`, `WdfRequestGetInformation`, `WdfRequestGetFileObject`, `WdfRequestGetIoQueue`, `WdfRequestWdmGetIrp`, `WdfRequestMarkCancelable`, `WdfRequestMarkCancelableEx`, `WdfRequestUnmarkCancelable`, `WdfRequestIsCanceled`.

可選的真實 WDK 驗證以真正的 KMDF 進入點程式庫分別編譯 `driver_kmdf_lifecycle.c` 和 `driver_kmdf_control.c`。`NEVERD_KMDF_FIXTURE`／`NEVERD_KMDF_CFG_FIXTURE` 選擇生命週期映像；`NEVERD_KMDF_CONTROL_FIXTURE`／`NEVERD_KMDF_CONTROL_CFG_FIXTURE` 選擇一般／啟用 CFG 的控制裝置映像。缺少外部產物時會明確略過。原生及 C API／CLI 涵蓋範圍見[測試指南](testing.md)。目前執行證據僅來自 Linux 主機。

初始 API 模型刻意採用有限契約：

| API | 建模行為與限制 |
|-----|----------------|
| `RtlInitUnicodeString` | 根據有界且以 NUL 結尾的來源字串建立客體 `UNICODE_STRING` |
| `RtlCopyUnicodeString`、`RtlCompareUnicodeString`、`RtlEqualUnicodeString` | 計數式 UTF-16 複製及區分大小寫的比較；不區分大小寫的比較需要 Windows 大小寫對照表，因此會停止 |
| `ExRaiseStatus`, `ExRaiseAccessViolation`, `ExRaiseDatatypeMisalignment` | 引發客體例外並交給支援的常數 C `__except` 處理常式；API 不會正常返回，篩選函式/finally 與 CPU 故障復原仍不支援 |
| `ExAllocatePool2` | 分頁／非分頁 NX 配置，預設清零；建模未初始化與快取對齊旗標；無效的必要旗標傳回 NULL，配額／可執行集區及引發的配置例外會停止 |
| `MmGetSystemRoutineAddress` | 透過共用匯出清單解析客體計數式名稱 |
| `MmMapIoSpace`, `MmMapIoSpaceEx`, `MmUnmapIoSpace` | 宣告之轉譯子範圍的非快取映射、共用 RO／RW 別名，以及原始基址／長度完全相符的解除映射 |
| `IoGetDmaAdapter` | 明確 Internal 匯流排主控版本 0／1 描述與 PASSIVE_LEVEL 的版本一繫結表；更新版本探查傳回 NULL |
| `AllocateCommonBuffer`, `FreeCommonBuffer`, `GetDmaAlignment`, `PutDmaAdapter` | 共用一致性 RAM 上的 adapter 表方法、確切配置身分與獨立 adapter 壽命 |
| `GetScatterGatherList`, `PutScatterGatherList` | DISPATCH_LEVEL 的 adapter 表方法；真正直接或資源排隊回呼、固定 MDL 檢視與明確映射釋放 |
| `AllocateAdapterChannel`, `MapTransfer`, `FlushAdapterBuffers`, `FreeMapRegisters` | Channel reservation、32 位元動作回呼、片段映射及整體 flush；與 SG 共用配額 |
| `KeFlushIoBuffers` | 有效且已鎖定／非分頁的 MDL 與一致性快取；不釋放映射或 registers |
| `IoConnectInterrupt`, `IoDisconnectInterrupt`, `IoConnectInterruptEx`, `IoDisconnectInterruptEx` | 精確配置的獨佔 latched 線路，PASSIVE_LEVEL 下的傳統及 Ex 1／2／4；不透明連線與精確世代壽命 |
| `KeSynchronizeExecution`, `KeAcquireInterruptSpinLock`, `KeReleaseInterruptSpinLock` | 真正 BOOLEAN 同步回呼及配置 DIRQL 下的同一把不可遞迴鎖；恢復原始呼叫者 IRQL 與所有權 |
| `MmMapLockedPagesSpecifyCache`、`MmGetSystemAddressForMdlSafe`、`MmUnmapLockedPages` | 請求擁有的 MDL 支援 KernelMode 快取對映及權限；非分頁集區 MDL 透過安全輔助函式重複使用原始集區對映 |
| `IoAllocateMdl`, `MmBuildMdlForNonPagedPool`, `IoFreeMdl` | 獨立描述元，完整範圍須位於同一個有效非分頁集區配置內；描述元與緩衝區生命週期獨立，不支援 IRP 關聯、MDL 鏈或配額 |
| `ZwOpenKey`, `ZwCreateKey`, `ZwQueryValueKey`, `ZwSetValueKey`, `ZwDeleteValueKey`, `ZwDeleteKey`, `ZwClose` | 明確配置的工作階段登錄、每個控制代碼的權限與生命週期、查詢緩衝區大小及修改；不存取主機登錄 |
| `ExAllocatePoolWithTag`、`ExFreePoolWithTag`、`ExFreePool` | 對集區類型 `0`、`1`、`512` 提供資料配置；大小／標籤必須為正，帶標籤釋放必須相符，位址不重複使用 |
| `IoCreateDevice`、`IoDeleteDevice` | 裝置類型為 `0x22`，characteristics 為 `0` 或 `0x100`，擴充區大小有界，名稱為 ASCII `\Device\Name` |
| `IoAttachDeviceToDeviceStack`, `IoDetachDevice` | 同驅動程式附加；傳回原頂端，解除附加接收儲存的下層裝置；遵守上述拓撲與生命週期限制 |
| `IofCallDriver`, `IoCallDriver` | 在保留路徑中向確切目標派送；驗證客體堆疊游標，保留下層 NTSTATUS |
| `IoInitializeRemoveLockEx`, `IoAcquireRemoveLockEx` | 延伸區內確切擁有者及不透明32／120位元組；NULL／重複 Tag |
| `IoReleaseRemoveLockEx`, `IoReleaseRemoveLockAndWaitEx` | 符合 Tag 釋放及提供者接收後可恢復的 REMOVE 等待 |
| `PoCallDriver`, `PoStartNextPowerIrp` | 轉送同一受管理的電源 IRP；Vista+ start-next 驗證不增加序列化交握 |
| `PoSetPowerState`, `PoRequestPowerIrp` | 獨立裝置通知狀態及明確 PDO FIFO 驅動的真正子要求；範圍見上 |
| `IoCreateSymbolicLink`、`IoDeleteSymbolicLink` | 一個工作階段命名空間內的 ASCII `\DosDevices\Name` 或 `\??\Name`，目標為 `\Device\Name` |
| `DbgPrint`、`DbgPrintEx` | 經檢查的 Win64 可變參數格式化，最多輸出 512 位元組；啟用所有偵錯器篩選器 |
| `IoGetCurrentIrpStackLocation` | 傳回目前建模 IRP 的堆疊位置；正常編譯的 WDM 巨集讀取相同客體欄位 |
| `KeGetCurrentIrql` | 目前執行 IRQL 在派送與工作項目中為 `PASSIVE_LEVEL`，在 DPC 中為 `DISPATCH_LEVEL` |
| `IoAllocateWorkItem`, `IoQueueWorkItem`, `IoFreeWorkItem` | 裝置擁有的不透明工作項目；僅支援 `DelayedWorkQueue`，在 `PASSIVE_LEVEL` 將裝置與內容傳給回呼；禁止釋放仍在佇列中的項目 |
| `KeInitializeDpc`, `KeInsertQueueDpc`, `KeRemoveQueueDpc`, `KeSetImportanceDpc`, `KeSetTargetProcessorDpc` | 不透明 DPC 儲存、四個客體回呼參數、`DISPATCH_LEVEL`、重複排入／移除及優先順序；僅目標 CPU0 |
| `KeInitializeTimer`, `KeInitializeTimerEx`, `KeSetTimer`, `KeSetTimerEx`, `KeCancelTimer`, `KeReadStateTimer` | 通知／同步計時器；相對／絕對 100 ns 期限、毫秒週期、重新設定／取消及虛擬時間中的訊號查詢 |
| `KeInitializeEvent`, `KeSetEvent`, `KeResetEvent`, `KeClearEvent`, `KeReadStateEvent` | 通知／同步事件保留不同訊號消耗行為；`KeSetEvent` 僅接受 Increment=0、Wait=FALSE |
| `KeWaitForSingleObject` | 單個已初始化事件或計時器；非警示 `KernelMode`、原因 `Executive`；零逾時輪詢、有限相對／絕對或無限等待；非零／無限等待要求 IRQL <= APC_LEVEL |
| `KeDelayExecutionThread` | IRQL <= APC_LEVEL 的非警示 `KernelMode` 相對／絕對延遲；虛擬時間推進後還原儲存的客體執行框架 |
| `IoMarkIrpPending` | 標記目前存活的 IRP；也支援 WDM 巨集對堆疊控制欄位的等效寫入；派送必須傳回 `STATUS_PENDING` |
| `IofCompleteRequest`、`IoCompleteRequest` | 以 `IO_NO_INCREMENT` 執行完成展開，支援暫停／繼續；僅在最終展開邊界釋放 IRP、MDL 與緩衝區 |
| `memcpy`、`memmove`、`memset`、`memcmp`、`RtlCopyMemory`、`RtlMoveMemory`、`RtlFillMemory`、`RtlZeroMemory`、`RtlCompareMemory` | 有界的客體緩衝區操作，每次呼叫最多 1 MiB；要求不重疊的複製 API 會拒絕重疊 |

API 的 IRQL 上限來自 `KernelAPIIRQL.def`，參數相關限制由所屬模型檢查。DPC 不能呼叫登錄 API，也不能配置、釋放或存取分頁集區；Unicode `DbgPrint` 轉換要求 `PASSIVE_LEVEL`，支援的 ANSI 輸出與非分頁操作仍可在 `DISPATCH_LEVEL` 使用。回呼堆疊有明確邊界，越界堆疊指標不能進入另一阻塞工作項目的堆疊。裝置擴充中的已啟動計時器會阻止裝置提早回收。這些檢查並未開放一般 IRQL 切換。

計時器到期會先滿足已登記的等待，再允許 DPC 重設或重新設定計時器。排入佇列的 DPC 先於已喚醒的 `PASSIVE_LEVEL` 執行框架還原執行。若請求儲存仍包含排入佇列的 DPC，IRP 完成操作會在完成及緩衝區失效之前拒絕釋放。

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

裝置名稱與 IOCTL 代碼必須符合驅動程式。create 省略 `device` 時選擇唯一的存活裝置；無法唯一選擇則失敗。後續請求使用其檔案的裝置，除非明確提供相符的名稱。選用的 `file` 是無號 32 位元情境識別碼，預設為零。每個識別碼各有自己的 FILE_OBJECT 與 FsContext，且必須依 create、傳輸、cleanup、close 的順序執行。獨立檔案的請求可以交錯執行。獨占裝置會拒絕第二次開啟。這些識別碼代表檔案物件，而非複製的控制代碼。支援緩衝及兩種直接 IOCTL 方法。派送必須同步完成，或遵循上述回呼待處理契約。輸出長度無效及存取已完成的 IRP 都會明確失敗。請求卸載後，不得留下存活的裝置、符號連結、集區配置或檔案物件。

選用根欄位 `"load_address": "0x190000000"` 請求變更載入基底位址；省略此欄位或指定 `"0x0"` 時使用慣用位址。映像必須滿足重新定位需求。原有的初始化命令或 C API 不會隱含執行任何請求情境。

根物件僅接受 `load_address`、`requests`、`unload`、`kernel_exports`、`registry` 和 `pnp_devices`。一般檔案要求接受 `kind`、選用且互斥的 `device` 或 `device_id`，以及選用的 `file`。IOCTL 必須提供 `code`，並接受 `input`、`output_size` 和 `direct_input`。`read` 接受 `output_size` 和 `byte_offset`；`write` 接受 `input` 和 `byte_offset`。位移預設為零，可使用整數或十六進位字串，且必須落在非負有號 64 位元值的範圍內。生命週期請求會拒絕傳輸欄位。未知或重複欄位會遭拒絕。`code` 接受無號 32 位元 JSON 整數或 `0x` 十六進位字串。`input` 是長度為偶數且不帶前綴或空格的十六進位位元組字串；省略表示空輸入。`output_size` 為無號 JSON 整數，省略表示零。不接受小數及浮點數寫法。

僅 READ／WRITE／IOCTL 請求接受選用欄位 `cancel_after_100ns`，它必須是 0 到 `INT64_MAX`（9223372036854775807）之間的 JSON 整數。此值相對於請求提交時刻，以虛擬 100 ns 為單位，並非實際時間。零表示框架路由後、客體 I/O 回呼前觸發取消；若路由已直接完成請求，則完成優先。正數延遲僅在沒有就緒回呼或執行框架時，隨時間推進至計時器、等待或取消期限而觸發。為 WDM 請求設定取消會以 `model_error` 停止；仍不支援一般佇列與 PnP 取消。每個請求報告都包含 `cancel_requested_at_100ns`，值為取消實際發生時的絕對虛擬時間；若未發生取消，包括完成先發生的情況，則為 null。請求取消本身不會完成 IRP，也不規定最終狀態。

對於直接 IOCTL，`input` 初始化第一個系統緩衝區，`direct_input` 則初始化由 MDL 描述的獨立第二個緩衝區，並以零補齊至 `output_size`。`METHOD_IN_DIRECT` 要求可讀取，但不代表系統對映唯讀。兩種方法都使用可讀寫的情境緩衝區。`MdlMappingNoWrite` 移除對映的寫入權限，`MdlMappingNoExecute` 移除執行權限。解除對映會撤銷系統 VA；重新對映仍保留相同的鎖定資料。完成請求後，MDL 及對映皆失效。模型提供 WDM 巨集使用的公開 MDL 欄位；處理程序欄位與未建立 MDL 的 PFN、手工建立的 MDL、使用者對映，以及透過原始 UserBuffer 直接存取都會遭拒絕。長度為零的直接緩衝區使用空 MDL。

`IoAllocateMdl` 為非空、不溢位且不超過 1 MiB 的緩衝區配置獨立中繼資料，不探查或鎖定緩衝區。`Irp` 必須為 NULL，`SecondaryBuffer` 與 `ChargeQuota` 必須為 FALSE；物件區耗盡時傳回 NULL。`MmBuildMdlForNonPagedPool` 要求完整描述範圍位於同一個有效非分頁集區配置內。安全輔助函式及一般 WDM 巨集重複使用原始位址，保留別名關係與既有權限，即使再次傳入禁止寫入／執行旗標也不改變權限。額外系統對映與解除對映會遭拒絕。`IoFreeMdl` 僅使描述元失效，集區緩衝區有獨立生命週期；只要不再使用已釋放的儲存空間，兩種釋放順序皆受支援。所有模型 MDL 欄位皆為唯讀；處理程序及未建立 MDL 的 PFN 存取、描述元鏈及手動修改欄位仍不受支援。卸載前必須釋放所有驅動程式擁有的描述元。

IOCTL 的 `output_size` 非零時，`Information` 不得超過該大小，即使輸入緩衝區較大亦然。沒有輸出緩衝區的 IOCTL 可在此欄位傳回驅動程式自訂結果，不會複製輸出位元組；`information_hex` 精確保留原始 64 位元值。

對於 READ/WRITE，`DO_BUFFERED_IO` 或 `DO_DIRECT_IO` 選擇傳輸方法。Neither 或互相衝突的旗標會停止。Information 會依傳輸長度檢查；寫入傳回計數，讀取傳回位元組。

`kernel_exports` 將常式名稱對應至明確的可用性布林值，例如 `"kernel_exports": {"OptionalRoutine": false}`。已建模的匯出與靜態匯入會取得與 `MmGetSystemRoutineAddress` 共用的穩定位址。明確不存在的匯出解析為 NULL，且不能滿足靜態匯入需求。宣告存在但沒有 API 模型的匯出解析為延遲陷阱。未知的動態名稱會停止，並診斷其可用性未指定；絕不因缺少實作而推斷匯出不存在。名稱為有界的可列印 ASCII，解析時區分大小寫。此清單是具體情境的屬性，不代表符合每個 Windows 版本。
`IoMarkIrpPending`, `IoGetCurrentIrpStackLocation` 和 `MmGetSystemAddressForMdlSafe` 是已建模的 WDM 標頭檔輔助函式；模型預設不會據此宣告它們是匯出項目，其匯出可用性需要靜態匯入或明確的 `kernel_exports` 宣告。

情境文字上限為 2 MiB，最多包含 64 個請求，每個輸入或輸出緩衝區最多 65536 位元組，包含 `direct_input` 內容在內的總請求位元組數最多 512 KiB。指令、觀察事件、客體記憶體及時間預算涵蓋整個情境。1 MiB 區域還需存放物件與中繼資料，因此即使尚未用盡情境緩衝區總額度，也可能耗盡模型記憶體。

## 登錄情境

選用的 `registry` 陣列定義具體的工作階段內登錄樹。每個機碼必須提供 `path`，可選擇提供 `values` 陣列；每個值包含 `name`、無號整數 `type` 與十六進位 `data`。空名稱表示預設值，例如 DWORD 值 `{"name":"Mode","type":4,"data":"01000000"}`。位元組原樣保留，不修復字串終止符或展開環境變數。

路徑必須是 `\Registry\Machine` 或 `\Registry\User` 下的絕對 ASCII 路徑，祖先機碼會自動建立。機碼與值依 ASCII 規則忽略大小寫；拒絕非 ASCII 名稱及重複識別。省略配置表示可用性未指定，登錄呼叫會停止。`"registry": []` 明確表示空命名空間。不會根據驅動程式推斷機碼、值、主機登錄資料或服務設定。

`ZwOpenKey` 與 `ZwCreateKey` 傳回獨立的不透明控制代碼，逐一檢查查詢、設定、子機碼建立及刪除權限。配置樹授予受支援的 `KEY_ALL_ACCESS` 位元，包括一般 `KEY_READ` 與 `KEY_WRITE` 遮罩。這是明確可存取的測試樹，不模擬 Windows ACL 或權限評估。通用權限、`MAXIMUM_ALLOWED`、其他登錄檢視、自訂安全性描述元、類別及符號連結均不支援。相對建立需要具有 `KEY_CREATE_SUB_KEY` 的直接父機碼控制代碼。輸入機碼為非揮發性，新建機碼可以為揮發性；拒絕在揮發性機碼下建立非揮發性子機碼。不模擬重新啟動或磁碟持續保存。

`ZwQueryValueKey` 實作 Basic、Full、Partial 及其有定義的 Align64 資訊類別，包括精確長度、資料對齊、部分輸出，以及不同的 `STATUS_BUFFER_TOO_SMALL`／`STATUS_BUFFER_OVERFLOW` 結果。`ZwSetValueKey` 與 `ZwDeleteValueKey` 只修改此工作階段的樹。`ZwDeleteKey` 拒絕刪除仍有子機碼的機碼；已刪除機碼的控制代碼在關閉前傳回 `STATUS_KEY_DELETED`。`ZwClose` 獨立釋放控制代碼，不刪除機碼；卸載時仍有開啟的登錄控制代碼會導致失敗。

限制為包含祖先的 256 個機碼、總計 1024 個值、每值 65536 位元組、值資料總計 512 KiB、機碼路徑 1024 個 ASCII 位元組、值名稱 256 位元組，以及同時開啟 256 個控制代碼。建立與修改遵守和情境預檢相同的限制。報告的 `configuration.registry` 保留原始輸入；`registry` 列出最終存活的機碼路徑和值，包括停止前的修改。未指定登錄時報告 null。此值快照不包含揮發性屬性及控制代碼識別。

## Microsoft 範例驗收檢查

選用的[驗證指令碼](../../scripts/validate_windows_driver_sample.py)下載[驗證清單](../../unittests/emulation/fixtures/sioctl-validation.json)中固定版本的 Microsoft SIOCTL 原始碼，驗證 SHA-256 雜湊，並使用 MinGW-w64 DDK 標頭檔編譯未修改的原始碼。指令碼在選定的輸出目錄中保留上游授權／來源資訊、建置命令、情境與報告。它需要網路存取、Clang、`lld-link`、`nm` 及 MinGW-w64 的 DDK 標頭檔：

```bash
python3 scripts/validate_windows_driver_sample.py \
  --neverd build-release/bin/neverd \
  --output build-release/driver-validation/sioctl
```

非預設 MinGW-w64 包含目錄可透過 `--headers` 指定。指令碼根據已編譯物件的相依項目產生 MS COFF 匯入程式庫。檢查分別以緩衝、in-direct 和 out-direct 情境執行 DriverEntry、create、IOCTL、cleanup、close 和 unload。加入 `--debug` 並選擇獨立輸出目錄，可使用 `DBG=1` 編譯並驗證客體日誌訊息。上游範例未註冊 cleanup 處理常式，因此模型的預設處理常式以 `STATUS_INVALID_DEVICE_REQUEST`（`0xC0000010`）完成 cleanup。驅動程式仍會關閉並卸載，成功的 IOCTL 則傳回預期位元組。對於這個完整情境，CLI 的預期結束碼為 **2**，`scenario_success` 為 false。指令碼僅在所有結果皆符合預期時成功，包括可見的 cleanup 失敗；它不會改寫範例來隱藏該結果。

## Zero 範例驗收檢查

額外的 [Zero 驗證腳本](../../scripts/validate_zero_driver_sample.py)依[清單](../../unittests/emulation/fixtures/zero-validation.json)固定的版本與雜湊，建置 Pavel Yosifovich 未修改的公開 Zero WDM 範例。使用相同工具鏈執行 `python3 scripts/validate_zero_driver_sample.py`。預設在 `build-release/driver-validation/zero` 保留原始碼、MIT 授權、命令、情境及報告。九個請求涵蓋跨頁 direct read、write 計數、客體原子統計及 buffered 統計 IOCTL。範例的零長度 read 失敗及缺少 CLEANUP 處理函式仍會顯示；預期 CLI 結束代碼為 2，之後 close 與 unload 成功。僅當所有結果及輸出位元組精確符合時，驗證腳本才成功。

## 報告與 SDK

JSON 報告區分 `stop_reason`、可為空值的 `nt_status` 和 `nt_success`、停止位置 PC，以及指令計數。它保留停止前收集的 API 呼叫及可觀察狀態，包括裝置物件與驅動程式回呼位址。客體位址以十六進位字串表示，避免 JSON 使用端遺失 64 位元精確度。

`configuration` 物件記錄本次執行的限制、服務名稱與 `kernel_exports` 覆寫值。設定識別為 `wdm-x64-scheduled-v16`。`nt_status` 始終是 DriverEntry 的結果，而 `scenario_success` 綜合描述初始化及已完成請求的結果。`phase`、`requests` 和 `unload_completed` 表明請求生命週期的哪些部分已執行。每次 API 呼叫及 CPU 寫入也會記錄階段（`driver_entry`、`add_device:<ID>`、`request:N`, `callback:N` 或 `unload`）。每個請求報告派送狀態與 I/O 狀態、是否完成、information 長度及傳回的 `output_hex` 位元組。`preferred_image_base` 描述原始 PE 基底位址。`security_cookie` 是已初始化 cookie 的客體位址；若不需要 cookie，則為 `"0x0"`。請求報告欄位為 `kind`、`device`、`device_id`、`pnp`、`file`、`byte_offset`、`code`、`irp`、`completed`、`cancel_requested_at_100ns`、`dispatch_status`、`io_status`、`information`、`information_hex` 和 `output_hex`。 `configuration.registry` 保留原始登錄配置。 `information_hex` 以十六進位字串精確保留原始 64 位元 `IoStatus.Information`；原有數值欄位 `information` 仍然保留。

工作項目觀察記錄使用 `callback:N` 階段。待處理請求的 `dispatch_status` 保留 `STATUS_PENDING`，最終完成狀態分別記錄於 `io_status`，並據此計算該請求對 `scenario_success` 的影響。

`ExRaiseStatus` 將 NTSTATUS 的低 32 位元傳給客體例外處理常式；`ExRaiseAccessViolation` 與 `ExRaiseDatatypeMisalignment` 分別引發 `STATUS_ACCESS_VIOLATION` 與 `STATUS_DATATYPE_MISALIGNMENT`。此設定依循 Microsoft 各函式的 DDI 文件：ExRaiseStatus 允許 `APC_LEVEL`，另兩個無參數常式則要求 `PASSIVE_LEVEL`。部分 WDK SAL 註記允許這兩個包裝常式使用 APC_LEVEL；此設定保留文件中較嚴格的限制。引發例外的呼叫維持 `result: null`，並在 `detail` 記錄例外碼；不會回報成功返回 API。

例外傳遞使用映像已解碼的 x64 第一版展開表，以及 `__C_specific_handler` 的常數 `EXCEPTION_EXECUTE_HANDLER` 範圍。它執行真正的客體處理常式主體，支援一般輔助函式框架展開，還原已儲存的非揮發性通用暫存器，並保留目前執行的堆疊邊界。`GetExceptionCode()` 可取得引發的例外碼。處理常式可再次引發例外，交給支援的外層範圍。遇到篩選函式、`__finally`、GS/C++ 處理機制、鏈結或不完整中繼資料、前置程式碼展開與 XMM 還原時，會明確失敗。未捕捉的 API 例外以 `model_error` 停止；CPU 記憶體、插斷與無效指令故障仍會終止執行。

原創測試驅動 `driver_wdm_seh.c` 使用真正的 WDK 標頭與 `/GS-`。設定 `NEVERD_WDM_SEH_FIXTURE` 及 `NEVERD_WDM_SEH_CFG_FIXTURE`，即可提供一般及啟用 CFG 的映像。[driver-seh-scenario.json](../examples/driver-seh-scenario.json) 範例會重定位映像、在 DriverEntry 捕捉 API 例外，然後卸載。這項 API 例外支援不會啟用 `ProbeForRead`、`ProbeForWrite`、使用者 MDL 鎖定或 `METHOD_NEITHER`。

可為空值的 `fault` 物件保留第一個後端錯誤。其 `kind`、`pc`、可為空值的 `address`、`size`、`access` 及 `interrupt` 可區分未對映或受保護的記憶體、無效範圍、無效指令及 CPU 例外。位址使用十六進位字串；大小及中斷向量使用整數。觀察讀取不能取代原始錯誤。發生錯誤的後端不能繼續執行，此記錄也不代表提供客體 SEH 處理。

`instructions` 統計執行策略已准許的客體指令嘗試次數。遭執行策略拒絕的指令不計數；已准許但在 CPU 中發生錯誤的指令計數。合成的 API 派送及返回哨兵不會增加此計數。

每個 `writes` 項目都帶有 `semantics: "attempted_guest_write"`：記錄堆疊外的 CPU 寫入嘗試，包括之後可能發生錯誤或被預算停止的嘗試。它不保證寫入已完成，也不包含 API 模型所做的寫入。裝置與驅動程式物件快照描述執行停止時觀察到的狀態。

包含 `neverd/sdk/NeverDCAPIEmulation.h`（或 C API 總標頭檔），建立工作階段，然後呼叫 `neverd_emulate_driver_json(session, path, options)`。明確傳入非空路徑會直接進入嚴格執行預檢，無須先透過通用分析 API 載入。CLI 採用此路徑。傳入 `NULL` options 使用預設值。明確指定 `neverd_driver_options_v1` 選項時，要求 `struct_size` 精確相符，且指令、記憶體、事件及逾時預算皆為正數。結果須用 `neverd_free_string` 釋放。

若路徑傳入 `NULL`，則要求工作階段已載入檔案，並獨立於 IR 分析及函式受限載入重新解析該檔案。兩種方式都會保留工作階段映像不變。呼叫期間必須保持輸入檔案可用且不變。請求／執行環境建立失敗時傳回 `NULL` 並設定 `neverd_last_error`；執行停止則傳回 JSON。未啟用此功能的建置仍提供此 API，並回報啟用方法。

`neverd_emulate_driver_scenario_json(session, path, scenario_json, options)` 使用相同的 v1 選項及所有權規則，另外接受嚴格驗證的情境輸入。必須傳入非 NULL、以 NUL 結尾的 JSON 字串。原有 `neverd_emulate_driver_json` ABI 維持不變，仍僅執行初始化。C++ 解析器 `driverOptionsFromScenarioJSON` 為 `emulateDriver` 呼叫端提供相同的情境驗證。

內部 C++ 進入點為 `include/neverd/emulation/DriverSession.h` 中的 `neverd::emulation::emulateDriver`。格式解析由現有載入器負責；Windows 物件／API 行為由 `lib/emulation/windows` 負責；CPU 狀態及執行由 Unicorn 配接器負責。配接器與模型使用相同的客體記憶體介面。Windows API 行為不應放入 Unicorn fork。
