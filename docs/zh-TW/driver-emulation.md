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

驗收也涵蓋 Pavel Yosifovich 未修改的 Zero WDM 範例，包括 direct READ/WRITE、原子統計計數及統計 IOCTL。

| 驅動程式類別或需求 | 目前範圍 | 缺少的環境 |
|--------------------|----------|------------|
| 使用下列 API 的 x64 軟體 WDM 驅動程式 | 有界 x64 WDM 初始化、循序緩衝／直接請求、工作項目、計時器、DPC、事件與等待，以及行為報告和限制 | 每個額外執行到的 API 都必須有明確的模型 |
| `METHOD_BUFFERED` IOCTL | 循序緩衝／直接 I/O，可由工作項目或 DPC 完成 | 僅支援下列 API 子集；不支援並行 IRP 或 WDM 請求取消 |
| `METHOD_IN_DIRECT`、`METHOD_OUT_DIRECT` | 由請求擁有的 MDL 及系統對映 | 實體頁面識別、DMA 及使用者對映 |
| 驅動程式自行配置的 MDL | 描述模型非分頁集區的獨立描述元，重複使用原始緩衝區位址 | IRP 關聯、MDL 鏈、探查／鎖定頁面、實體頁面及使用者對映 |
| READ/WRITE | 循序緩衝／直接 I/O，可由工作項目或 DPC 完成 | 僅支援下列 API 子集；不支援並行 IRP 或 WDM 請求取消；`METHOD_NEITHER` 與隱含檔案位置 |
| `METHOD_NEITHER` | 拒絕 | 使用者位址空間環境、存取探測及客體例外處理 |
| KMDF 1.33 非 PnP 驅動程式 | 版本繫結、物件／內容、具名控制裝置、循序預設佇列，以及實際執行回呼的緩衝／直接請求 | 不支援 PnP 裝置、一般佇列排程、類別擴充或 UMDF |
| PnP 匯流排／功能／篩選驅動程式 | 初始化可在 API 子集內執行；不支援裝置堆疊生命週期 | 裝置附加、向下層驅動程式派送、PnP 與電源 IRP |
| 儲存、網路、顯示、檔案系統及迷你篩選驅動程式 | 不支援相關子系統契約 | 連接埠／類別／迷你連接埠框架、NDIS/WFP、圖形或檔案系統服務 |
| 工作項目、計時器、DPC、事件與等待 | 目前執行 IRQL 在派送與工作項目中為 `PASSIVE_LEVEL`，在 DPC 中為 `DISPATCH_LEVEL` | 僅支援下列 API 子集；不支援並行 IRP 或 WDM 請求取消 |
| 使用處理程序／執行緒回呼、控制代碼、登錄／檔案操作或核心模組探索的驅動程式 | 支援配置的登錄；其他行為限於下列 API | 物件管理員、系統狀態及回呼／事件產生機制 |
| 硬體、DMA、PCI、中斷或虛擬化驅動程式 | 不支援所需環境 | 裝置模型、實體記憶體、匯流排、中斷及特權 CPU 狀態 |
| x86 或 ARM64 Windows 驅動程式 | 拒絕 | 對應架構的載入、ABI 及執行模型 |
| x64 CFG | 驗證目標表及檢查／分派呼叫；未啟用的插樁保留客體後援函式 | XFG、匯出抑制、不支援的載入組態與 TLS 仍遭拒絕 |

未使用的不支援匯入項目可以維持繫結。一旦執行到不支援的操作，便停止並提供診斷及先前收集的觀察結果。僅 DriverEntry 成功，不能證明後續派送、硬體或框架路徑也受支援。下方 API 表是受支援子集的權威定義。

## 執行契約

此設定在 CPU0 上以確定性的合作排程模擬 x64 WDM 生命週期。 執行從 PE 進入點開始；若有編譯器產生的進入點包裝函式，也會保留並執行。DriverEntry 必須傳回 `STATUS_SUCCESS` 才能完成初始化；非零的成功狀態或待處理狀態會因初始化契約不受支援而停止。失敗狀態則保留為已完成的初始化結果。所有物件、字串、堆疊、函式指標及配置均位於客體記憶體。模型依設定的服務名稱（預設為 `NeverDDriver`）提供 `DRIVER_OBJECT` 和登錄路徑。

配接器使用 Unicorn 的虛擬 TLB 模式保留客體虛擬位址，包括規範的高位核心位址，無須合成 Windows 頁表。初始 RFLAGS 為 `0x202`；軟體裝置設定採用固定的 64 位元組快取列。這些都是本執行情境的明確屬性。行內 x64 CR8 讀取觀察到相同的 `PASSIVE_LEVEL` / `DISPATCH_LEVEL`；CR8 寫入與其他控制暫存器操作仍不受支援。

未知匯入項目繫結至延遲陷阱。未使用的匯入項目不會阻止執行；執行其 thunk 或讀取未建模的匯出資料值時，會以 `unsupported_api` 停止。不支援的 CPU 環境效果也會明確停止。NeverD 不會用成功傳回值替代未實作的呼叫。格式錯誤的映像或不支援的載入需求會在執行前失敗。

排入佇列的 `DelayedWorkQueue` 工作項目在 `PASSIVE_LEVEL` 執行，客體 DPC 回呼在 `DISPATCH_LEVEL` 接收規定的四個參數。CPU0 在呼叫傳回及阻塞等待邊界進行確定性的合作排程。相對、絕對與週期計時器使用虛擬時間；沒有可執行的框架時，時間推進至下一計時器、等待或取消期限。通知型與同步型事件／計時器保留各自的訊號消耗語意。每個回呼擁有獨立的客體堆疊；多個阻塞框架保留區域變數及完整 CPU 內容，客體記憶體仍共用。Win64 回呼入口將前四個參數放入暫存器，其餘放入堆疊。請求仍循序處理：標記 IRP 為待處理的派送函式必須傳回 `STATUS_PENDING`，且完成後才能開始下一個請求。待處理請求或無限等待沒有可用來源時，以停滯的 `model_error` 停止。指令、記憶體、觀察記錄與實際時間預算仍共用。

這是有界排程模型，不代表完整 Windows 非同步支援。可警示或使用者模式等待、系統執行緒、APC、WDM 請求取消、自旋鎖、並行 IRP、一般 IRQL 切換、`METHOD_NEITHER`、UMDF、KMDF PnP 裝置及一般佇列排程、完整 PnP／電源、硬體、DMA 與中斷仍不支援。僅初始化呼叫會執行明確排入佇列的回呼，不會隱含產生請求或卸載。

工作項目在回呼開始前出佇列，因此回呼可釋放自身的工作項目。釋放仍在佇列中的項目、重複排入、使用失效物件或非客體可執行記憶體中的回呼位址都會明確失敗。裝置參考保留到回呼傳回。請求卸載要求釋放所有工作項目並完成佇列工作。CPU 內容保存與還原包含通用、SIMD、FPU 與控制狀態；客體記憶體始終共用，故障 CPU 不能藉還原內容繼續執行。
刪除會延後到檔案物件及排隊／執行中的工作項目參考全部釋放。物件區耗盡時，工作項目配置傳回 NULL。

映像預設使用慣用基底位址，除非情境選擇了有效的重新定位位址。映像必須為使用 native 子系統的 PE32+ x64 可執行檔。匯入可來自 `ntoskrnl.exe`、`ntkrnlmp.exe` 或 `WDFLDR.SYS`。

執行載入器支援經驗證的 x64 `DIR64` 基底重新定位，以及有限的安全性 cookie 載入組態；它會在進入點包裝函式執行前設定具確定性的客體 cookie。其他未建模的載入組態欄位、TLS、延遲／繫結匯入、依序號匯入及受控映像皆會遭拒絕。映像還必須通過嚴格的範圍與對齊檢查。

啟用的控制流程防護（CFG）會驗證 PE 旗標、指標槽與已排序的可執行目標表。檢查與分派輔助函式僅允許已宣告的映像進入點或已登記的 API 跳板，保留 Win64 呼叫狀態，並拒絕未宣告的目標。只有插樁而未啟用 CFG 時，保留原始客體後援指標。啟用的 XFG、匯出抑制和其他未建模的防護策略仍遭拒絕；位址位於可執行記憶體不代表它是合法目標。

KMDF 1.33 支援使用精確的 1.33.0 ABI：458 個函式槽具有穩定的客體識別，下列 38 個 API 實作了執行語義。`WdfVersionBind` 與 `WdfVersionUnbind` 在真實 WDK `FxDriverEntry` 包裝函式前後管理客體繫結。`WdfGetDriver` 讀取公用驅動程式全域結構。非 PnP 驅動程式、一般物件、控制裝置、佇列和傳入請求共用具型別內容、參考計數，以及實際執行的清理／銷毀／卸載回呼。所有已建模的框架呼叫與回呼目前都要求 `PASSIVE_LEVEL`；清理完成後新增參考仍不在此設定的支援範圍內。未建模的函式槽、`WdfLdrQueryInterface`、類別擴充和 UMDF 會明確停止。

控制裝置要求複製可列印 ASCII 名稱，且 SDDL 必須精確為 `D:P(A;;GA;;;WD)`。這授予所有呼叫端存取權限，無需虛構呼叫端權杖；不支援其他安全描述元、未命名裝置和自動名稱。裝置初始化擁有一個 WDM 裝置。請求可透過現有工作階段命名空間中的符號連結別名 `\DosDevices\Name` 或 `\??\Name` 選擇裝置，報告仍保留正規裝置名稱。建立成功會消耗初始化物件並清空其指標；失敗則回復部分裝置擁有權。`WdfControlFinishInitializing` 決定何時可以遞送 I/O。僅在已建模的檔案、工作項目和請求允許時，刪除操作才移除裝置及其連結；不支援刪除過程中取消或排空請求。

96 位元組的 `WDF_IO_QUEUE_CONFIG` 支援循序預設佇列，要求明確被動執行且不使用框架同步。控制裝置佇列不參與電源管理。專用 READ／WRITE／IOCTL 回呼優先於預設回呼。已接受的佇列請求即使同步完成也傳回 `STATUS_PENDING`；void 回呼的傳回暫存器不會使請求完成。延後完成使用現有排程器。沒有處理函式時，請求以 `STATUS_INVALID_DEVICE_REQUEST` 完成；未啟用零長度遞送時，零長度 READ／WRITE 直接完成。預設檔案套件以成功狀態和 Information=0 完成 CREATE／CLEANUP／CLOSE。不支援並行／手動佇列、檔案回呼、PnP 裝置和完整 PnP／電源。

請求參數使用 40 位元組的 `WDF_REQUEST_PARAMETERS` 配置。輸入／輸出存取函式傳回邏輯長度，保留緩衝區別名及現有直接 I/O 的 MDL 對映；直接 IOCTL 的輸入仍使用緩衝區。方向錯誤或緩衝區不足會傳回文件規定的狀態。完成操作先執行請求清理和子物件銷毀，再使 IRP／緩衝區失效，最後在參考允許時銷毀請求。一旦開始完成操作，就拒絕新的緩衝區與參數存取函式呼叫；已取得的緩衝區指標在清理期間仍可使用。外部物件參考保留內容，但不保留對已完成 IRP 的存取權。使用者模式 `METHOD_NEITHER` 仍需要尚未實作的呼叫端內容／探測／鎖定支援。

取消支援限於上述控制裝置佇列中的請求。若取消已經發生，`WdfRequestMarkCancelableEx` 傳回 `STATUS_CANCELLED`，不會呼叫取消回呼。`WdfRequestUnmarkCancelable` 成功後會移除回呼；之後發生的取消只記錄已取消狀態，不再遞送該回呼。`WdfRequestIsCanceled` 可在未標記為可取消的存活請求上讀取此狀態。成功標記為可取消後，完成請求需要成功解除標記，或取消回呼已開始遞送；僅排入佇列仍不允許完成。回呼開始後可與工作項目協調完成，包括回呼正在等待的情況。獨立的內部參考保留請求直到取消回呼傳回；完成操作仍先使 IRP 失效，最終的請求銷毀接續本身也可等待。排程優先順序為 DPC、按 FIFO 排列的取消回呼、一般工作項目；排入佇列的取消回呼也先於就緒的被動層級等待框架恢復。

請求已取消時，舊版 void `WdfRequestMarkCancelable` 會在傳回前同步執行客體取消回呼。此子接續可以等待，透過巢狀清理完成請求，並在原 API 恢復前執行最終銷毀。註冊後才發生的取消仍走上述排程路徑。這重現了 `PASSIVE_LEVEL` 與 `WdfSynchronizationScopeNone` 下的公開原始碼行為；[Microsoft](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfrequest/nf-wdfrequest-wdfrequestmarkcancelable) 要求不使用自動同步的驅動程式選擇 Ex。此處執行舊版 API 是相容行為，並非建議在該設定下使用它。

`WdfRequestGetInformation` 與 `WdfRequestSetInformation` 共用原 IRP 中的 64 位元 `IoStatus.Information`，與客體直接寫入保持一致。Set 只指派數值，傳輸長度在完成時才驗證。`WdfRequestCompleteWithInformation` 在清理前寫入同一欄位；清理回呼透過事先儲存的 IRP 修改此值後，最終 Information 使用修改後的值，即使此階段 GetInformation 已傳回零。`WdfRequestGetIoQueue` 傳回來源佇列。預設檔案設定下，`WdfRequestGetFileObject` 傳回 NULL，不會把 WDM FILE_OBJECT 偽裝成 WDF 檔案物件。`WdfRequestWdmGetIrp` 傳回同一個 IRP；客體不能透過 `IoCompleteRequest`／`IofCompleteRequest` 繞過 WDF 完成流程。完成過程中或完成後，只要請求控制代碼仍有效，GetInformation／GetIoQueue 傳回零；MDL 擷取先將有效輸出槽清為 NULL，再傳回 `STATUS_INTERNAL_ERROR`。此時 SetInformation／GetFileObject／WdmGetIrp 仍遭拒絕，既有的緩衝區與參數存取限制不變。

`WdfRequestRetrieveInputWdmMdl` 與 `WdfRequestRetrieveOutputWdmMdl` 視需要為緩衝 WRITE 輸入、READ 輸出及 IOCTL 輸入／輸出描述現有 SystemBuffer。每次擷取均先驗證方向有效且長度非零，再使用該請求唯一的快取描述元；首次成功擷取決定 ByteCount，即使另一方向的邏輯長度不同也保留此值。對此描述元，`MmGetSystemAddressForMdlSafe` 傳回原 VA；額外對映、解除對映及驅動程式釋放均遭拒絕。直接 READ 輸出、WRITE 輸入及 IOCTL 輸出傳回現有 `IRP.MdlAddress`，擷取本身不建立對映；直接 IOCTL 輸入使用 SystemBuffer 快取。描述元、IRP 和緩衝區在完成時一同失效。取消內部參考或外部參考只保留 WDF 內容，不保留已完成的 I/O 儲存空間。`METHOD_NEITHER` 和實體 PFN 存取仍不支援。

已建模的 KMDF API: `WdfDriverCreate`, `WdfDriverGetRegistryPath`, `WdfDriverWdmGetDriverObject`, `WdfWdmDriverGetWdfDriverHandle`, `WdfObjectGetTypedContextWorker`, `WdfObjectAllocateContext`, `WdfObjectContextGetObject`, `WdfObjectReferenceActual`, `WdfObjectDereferenceActual`, `WdfObjectCreate`, `WdfObjectDelete`, `WdfControlDeviceInitAllocate`, `WdfDeviceInitFree`, `WdfDeviceInitAssignName`, `WdfDeviceInitSetIoType`, `WdfDeviceCreate`, `WdfDeviceCreateSymbolicLink`, `WdfControlFinishInitializing`, `WdfDeviceWdmGetDeviceObject`, `WdfIoQueueCreate`, `WdfDeviceGetDefaultQueue`, `WdfIoQueueGetDevice`, `WdfRequestComplete`, `WdfRequestCompleteWithInformation`, `WdfRequestGetParameters`, `WdfRequestRetrieveInputBuffer`, `WdfRequestRetrieveOutputBuffer`, `WdfRequestRetrieveInputWdmMdl`, `WdfRequestRetrieveOutputWdmMdl`, `WdfRequestSetInformation`, `WdfRequestGetInformation`, `WdfRequestGetFileObject`, `WdfRequestGetIoQueue`, `WdfRequestWdmGetIrp`, `WdfRequestMarkCancelable`, `WdfRequestMarkCancelableEx`, `WdfRequestUnmarkCancelable`, `WdfRequestIsCanceled`.

可選的真實 WDK 驗證以真正的 KMDF 進入點程式庫分別編譯 `driver_kmdf_lifecycle.c` 和 `driver_kmdf_control.c`。`NEVERD_KMDF_FIXTURE`／`NEVERD_KMDF_CFG_FIXTURE` 選擇生命週期映像；`NEVERD_KMDF_CONTROL_FIXTURE`／`NEVERD_KMDF_CONTROL_CFG_FIXTURE` 選擇一般／啟用 CFG 的控制裝置映像。缺少外部產物時會明確略過。原生及 C API／CLI 涵蓋範圍見[測試指南](testing.md)。目前執行證據僅來自 Linux 主機。

初始 API 模型刻意採用有限契約：

| API | 建模行為與限制 |
|-----|----------------|
| `RtlInitUnicodeString` | 根據有界且以 NUL 結尾的來源字串建立客體 `UNICODE_STRING` |
| `RtlCopyUnicodeString`、`RtlCompareUnicodeString`、`RtlEqualUnicodeString` | 計數式 UTF-16 複製及區分大小寫的比較；不區分大小寫的比較需要 Windows 大小寫對照表，因此會停止 |
| `ExAllocatePool2` | 分頁／非分頁 NX 配置，預設清零；建模未初始化與快取對齊旗標；無效的必要旗標傳回 NULL，配額／可執行集區及引發的配置例外會停止 |
| `MmGetSystemRoutineAddress` | 透過共用匯出清單解析客體計數式名稱 |
| `MmMapLockedPagesSpecifyCache`、`MmGetSystemAddressForMdlSafe`、`MmUnmapLockedPages` | 請求擁有的 MDL 支援 KernelMode 快取對映及權限；非分頁集區 MDL 透過安全輔助函式重複使用原始集區對映 |
| `IoAllocateMdl`, `MmBuildMdlForNonPagedPool`, `IoFreeMdl` | 獨立描述元，完整範圍須位於同一個有效非分頁集區配置內；描述元與緩衝區生命週期獨立，不支援 IRP 關聯、MDL 鏈或配額 |
| `ZwOpenKey`, `ZwCreateKey`, `ZwQueryValueKey`, `ZwSetValueKey`, `ZwDeleteValueKey`, `ZwDeleteKey`, `ZwClose` | 明確配置的工作階段登錄、每個控制代碼的權限與生命週期、查詢緩衝區大小及修改；不存取主機登錄 |
| `ExAllocatePoolWithTag`、`ExFreePoolWithTag`、`ExFreePool` | 對集區類型 `0`、`1`、`512` 提供資料配置；大小／標籤必須為正，帶標籤釋放必須相符，位址不重複使用 |
| `IoCreateDevice`、`IoDeleteDevice` | 裝置類型為 `0x22`，characteristics 為 `0` 或 `0x100`，擴充區大小有界，名稱為 ASCII `\Device\Name` |
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
| `IofCompleteRequest`、`IoCompleteRequest` | 以 `IO_NO_INCREMENT` 完成目前同步或待處理的建模 IRP；已完成的 IRP 或緩衝區不能再次存取 |
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

根物件僅接受 `load_address`、`requests`、`unload`、`kernel_exports` 和 `registry`。所有請求都接受 `kind`、選用的 `device` 和選用的 `file`。IOCTL 必須提供 `code`，並接受 `input`、`output_size` 和 `direct_input`。`read` 接受 `output_size` 和 `byte_offset`；`write` 接受 `input` 和 `byte_offset`。位移預設為零，可使用整數或十六進位字串，且必須落在非負有號 64 位元值的範圍內。生命週期請求會拒絕傳輸欄位。未知或重複欄位會遭拒絕。`code` 接受無號 32 位元 JSON 整數或 `0x` 十六進位字串。`input` 是長度為偶數且不帶前綴或空格的十六進位位元組字串；省略表示空輸入。`output_size` 為無號 JSON 整數，省略表示零。不接受小數及浮點數寫法。

僅 READ／WRITE／IOCTL 請求接受選用欄位 `cancel_after_100ns`，它必須是 0 到 `INT64_MAX`（9223372036854775807）之間的 JSON 整數。此值相對於請求提交時刻，以虛擬 100 ns 為單位，並非實際時間。零表示框架路由後、客體 I/O 回呼前觸發取消；若路由已直接完成請求，則完成優先。正數延遲僅在沒有就緒回呼或執行框架時，隨時間推進至計時器、等待或取消期限而觸發。為 WDM 請求設定取消會以 `model_error` 停止；仍不支援一般佇列與 PnP 取消。每個請求報告都包含 `cancel_requested_at_100ns`，值為取消實際發生時的絕對虛擬時間；若未發生取消，包括完成先發生的情況，則為 null。請求取消本身不會完成 IRP，也不規定最終狀態。

對於直接 IOCTL，`input` 初始化第一個系統緩衝區，`direct_input` 則初始化由 MDL 描述的獨立第二個緩衝區，並以零補齊至 `output_size`。`METHOD_IN_DIRECT` 要求可讀取，但不代表系統對映唯讀。兩種方法都使用可讀寫的情境緩衝區。`MdlMappingNoWrite` 移除對映的寫入權限，`MdlMappingNoExecute` 移除執行權限。解除對映會撤銷系統 VA；重新對映仍保留相同的鎖定資料。完成請求後，MDL 及對映皆失效。模型提供 WDM 巨集使用的公開 MDL 欄位；處理程序／PFN 欄位、手工建立的 MDL、使用者對映，以及透過原始 UserBuffer 直接存取都會遭拒絕。長度為零的直接緩衝區使用空 MDL。

`IoAllocateMdl` 為非空、不溢位且不超過 1 MiB 的緩衝區配置獨立中繼資料，不探查或鎖定緩衝區。`Irp` 必須為 NULL，`SecondaryBuffer` 與 `ChargeQuota` 必須為 FALSE；物件區耗盡時傳回 NULL。`MmBuildMdlForNonPagedPool` 要求完整描述範圍位於同一個有效非分頁集區配置內。安全輔助函式及一般 WDM 巨集重複使用原始位址，保留別名關係與既有權限，即使再次傳入禁止寫入／執行旗標也不改變權限。額外系統對映與解除對映會遭拒絕。`IoFreeMdl` 僅使描述元失效，集區緩衝區有獨立生命週期；只要不再使用已釋放的儲存空間，兩種釋放順序皆受支援。所有模型 MDL 欄位皆為唯讀；處理程序／PFN 存取、描述元鏈及手動修改欄位仍不受支援。卸載前必須釋放所有驅動程式擁有的描述元。

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

`configuration` 物件記錄本次執行的限制、服務名稱與 `kernel_exports` 覆寫值。設定識別為 `wdm-x64-scheduled-v6`。`nt_status` 始終是 DriverEntry 的結果，而 `scenario_success` 綜合描述初始化及已完成請求的結果。`phase`、`requests` 和 `unload_completed` 表明請求生命週期的哪些部分已執行。每次 API 呼叫及 CPU 寫入也會記錄階段（`driver_entry`、`request:N`, `callback:N` 或 `unload`）。每個請求報告派送狀態與 I/O 狀態、是否完成、information 長度及傳回的 `output_hex` 位元組。`preferred_image_base` 描述原始 PE 基底位址。`security_cookie` 是已初始化 cookie 的客體位址；若不需要 cookie，則為 `"0x0"`。請求報告欄位為 `kind`、`device`、`file`、`byte_offset`、`code`、`irp`、`completed`、`cancel_requested_at_100ns`、`dispatch_status`、`io_status`、`information`、`information_hex` 和 `output_hex`。 `configuration.registry` 保留原始登錄配置。 `information_hex` 以十六進位字串精確保留原始 64 位元 `IoStatus.Information`；原有數值欄位 `information` 仍然保留。

工作項目觀察記錄使用 `callback:N` 階段。待處理請求的 `dispatch_status` 保留 `STATUS_PENDING`，最終完成狀態分別記錄於 `io_status`，並據此計算該請求對 `scenario_success` 的影響。

可為空值的 `fault` 物件保留第一個後端錯誤。其 `kind`、`pc`、可為空值的 `address`、`size`、`access` 及 `interrupt` 可區分未對映或受保護的記憶體、無效範圍、無效指令及 CPU 例外。位址使用十六進位字串；大小及中斷向量使用整數。觀察讀取不能取代原始錯誤。發生錯誤的後端不能繼續執行，此記錄也不代表提供客體 SEH 處理。

`instructions` 統計執行策略已准許的客體指令嘗試次數。遭執行策略拒絕的指令不計數；已准許但在 CPU 中發生錯誤的指令計數。合成的 API 派送及返回哨兵不會增加此計數。

每個 `writes` 項目都帶有 `semantics: "attempted_guest_write"`：記錄堆疊外的 CPU 寫入嘗試，包括之後可能發生錯誤或被預算停止的嘗試。它不保證寫入已完成，也不包含 API 模型所做的寫入。裝置與驅動程式物件快照描述執行停止時觀察到的狀態。

包含 `neverd/sdk/NeverDCAPIEmulation.h`（或 C API 總標頭檔），建立工作階段，然後呼叫 `neverd_emulate_driver_json(session, path, options)`。明確傳入非空路徑會直接進入嚴格執行預檢，無須先透過通用分析 API 載入。CLI 採用此路徑。傳入 `NULL` options 使用預設值。明確指定 `neverd_driver_options_v1` 選項時，要求 `struct_size` 精確相符，且指令、記憶體、事件及逾時預算皆為正數。結果須用 `neverd_free_string` 釋放。

若路徑傳入 `NULL`，則要求工作階段已載入檔案，並獨立於 IR 分析及函式受限載入重新解析該檔案。兩種方式都會保留工作階段映像不變。呼叫期間必須保持輸入檔案可用且不變。請求／執行環境建立失敗時傳回 `NULL` 並設定 `neverd_last_error`；執行停止則傳回 JSON。未啟用此功能的建置仍提供此 API，並回報啟用方法。

`neverd_emulate_driver_scenario_json(session, path, scenario_json, options)` 使用相同的 v1 選項及所有權規則，另外接受嚴格驗證的情境輸入。必須傳入非 NULL、以 NUL 結尾的 JSON 字串。原有 `neverd_emulate_driver_json` ABI 維持不變，仍僅執行初始化。C++ 解析器 `driverOptionsFromScenarioJSON` 為 `emulateDriver` 呼叫端提供相同的情境驗證。

內部 C++ 進入點為 `include/neverd/emulation/DriverSession.h` 中的 `neverd::emulation::emulateDriver`。格式解析由現有載入器負責；Windows 物件／API 行為由 `lib/emulation/windows` 負責；CPU 狀態及執行由 Unicorn 配接器負責。配接器與模型使用相同的客體記憶體介面。Windows API 行為不應放入 Unicorn fork。
