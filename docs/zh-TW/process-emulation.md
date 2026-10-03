**語言**：[English](../process-emulation.md) | [简体中文](../zh-CN/process-emulation.md) | [繁體中文](process-emulation.md) | [日本語](../ja/process-emulation.md) | [한국어](../ko/process-emulation.md) | [Français](../fr/process-emulation.md) | [Deutsch](../de/process-emulation.md) | [Español](../es/process-emulation.md) | [Italiano](../it/process-emulation.md) | [Русский](../ru/process-emulation.md) | [العربية](../ar/process-emulation.md)

[← 文件索引](README.md)

# 客體程序模擬

`neverd emulate` 會在明確指定的客體 OS 設定檔下執行映像。CPU 傳輸、映像解析、程序進入點與 OS 服務由不同邊界負責。啟用 `NEVERD_ENABLE_CPU_EMULATION=ON`；驅動程式模擬也會包含它。

首個設定檔 `linux-elf64-v1` 會在 CPL3 或 EL0 執行 x64/AArch64 ELF `ET_EXEC` 與可自行重定位的靜態 PIE `ET_DYN`。它載入真實 ELF 區段、建構初始堆疊、依指令量恢復執行，並處理明確的 Linux 系統呼叫要求。這是獨立程序模型，不是完整 Linux 發行版，也不保證任意 libc 二進位檔都能執行。動態連結、訊號、執行緒、檔案系統及不支援的服務都會明確失敗。

<!-- i18n-section: cli-sdk -->

## CLI 與 SDK

```bash
neverd emulate guest.elf --profile=linux-elf64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"],"instruction_limit":100000}'
```

相符的 Linux 主機選擇 KVM，相符的 Windows 主機選擇 WHP；其他主機／客體 ISA 組合使用 Unicorn。所選後端不可用時會報錯，不會靜默回退。在 Windows 執行 ELF 時仍使用 Linux 程序模型。CPU 指令範圍與限制請見[CPU 執行](cpu-execution.md)。

CLI 輸出一份 JSON 報告。客體狀態為 0 時 CLI 回傳 0，其他狀態回傳 2，執行未完成（含故障與限制）回傳 3，設定/API 無效回傳 1。實際客體狀態位於 `exit_status`。新增 C 入口 [`neverd_emulate_process_json`](../../include/neverd/sdk/NeverDCAPIProcess.h) 接受 session、非空輸入路徑、明確設定檔與選用 options JSON。使用 `neverd_free_string` 釋放結果；NULL 表示設定失敗，可由 `neverd_last_error` 取得原因。客體故障或資源停止都會回傳報告。session 已載入的分析映像不需要，也不會被變更。

```python
report = session.emulate_process(
    "guest.elf", "linux-elf64-v1",
    '{"backend":"unicorn","arguments":["guest"],"environment":[]}',
)
output = bytes.fromhex(report["stdout_hex"])
```

<!-- i18n-section: options-results -->

## 選項與結果

選項是最多 64 KiB 的 JSON 物件。未知/null 欄位、型別錯誤、字串內嵌 NUL、非正數限制都會遭拒。

| 選項 | 預設值 | 契約 |
|---|---|---|
| `backend` | `auto` | `auto`、`unicorn`、`kvm` 或 `whp` |
| `arguments` | 輸入檔名 | 完整 argv，包含 argv[0]；空值採預設值 |
| `environment` | `[]` | 明確的客體字串；不繼承主機環境 |
| `instruction_limit` | 100000 | 共用的已准入指令嘗試數 |
| `event_limit` | 10000 | 系統呼叫事件數，在 OS 服務處理前扣除 |
| `timeout_microseconds` | 5000000 | 程序設定完成後開始的單調 deadline |
| `memory_limit` | 67108864 | 實體／對映記憶體預算 |
| `stack_size` | 1048576 | 預算內按頁對齊的堆疊 |
| `output_limit` | 1048576 | 擷取的 stdout/stderr 總位元組數 |
| `instruction_quantum` | 1024 | 交還 runtime 前的准入間隔 |

`schema_version` 為 1。結果包含 profile、架構、所選後端與原因、`stop_reason`、可為 null 的 `exit_status`、診斷、進入／目前 PC、計數器、服務記錄與最後的型別化 CPU exit。位址、syscall 編號、參數暫存器及原始回傳位元均為**不含** `0x` 的十六進位字串；`stdout_hex`／`stderr_hex` 保留 NUL 與無效 UTF-8。syscall 結果為 null 表示沒有建模回傳值（例如 exit 或不支援要求），不代表成功回傳 0。

<!-- i18n-section: linux-semantics -->

## Linux 設定檔語意

OS 政策重用既有 ELF 載入器解碼的 program headers。它驗證 ABI 標籤、區段對齊、已對映的 program-header tables 和使用者位址範圍。通用對映計畫會在配置前檢查範圍、權限、重疊和預算，且只公開完全準備好的私有位址空間。保留檔案頁面前／尾位元組、將 BSS 清零、遵循區段權限並為堆疊保留 guard gaps。頁面重疊版面與矛盾 header 會被拒絕，不會猜測。

靜態 PIE 使用至少 `0x40000000` 的確定性 load bias，並依較大的 `PT_LOAD` 對齊需求提高。所有對映區段、入口 PC、`AT_PHDR`/`AT_ENTRY` 使用相同 bias；原始 program header 值不變，且無 interpreter 時 `AT_BASE` 為 0。對映來源明確採用原始檔案位元組，不包含分析階段 pointer fixup；客體啟動必須自行執行 relocation 與初始化。Loader 從有界的原始檔案記錄解碼 `PT_DYNAMIC`，不依賴 section header。若存在，table 必須可讀、正確終止且最多 4096 筆。拒絕 `PT_INTERP` 與外部 dependency/filter/audit 標籤；不提供 dynamic linker、symbol resolver 或 constructor runner。

初始堆疊包含對齊的 argc/argv/envp/auxv、PHDR/PHENT/PHNUM、entry、頁面大小與 identity 值。模型 PID/TID/UID/GID 均為 1000。為了可重現，`AT_RANDOM` 使用輸入 SHA-256 的前 16 個位元組；這是確定性模型政策，不是密碼學熵。HWCAP/HWCAP2 為 0，沒有 vDSO。

已實作 `write`、`exit`、`exit_group`、`getpid`、`gettid`, `mmap`, `mprotect`, `munmap`, `brk`，編號分別採用 [x64](https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl) 與 [ARM64](https://github.com/torvalds/linux/blob/master/include/uapi/asm-generic/unistd.h)。x64 SYSCALL 返回時會套用 RCX/R11 clobber、RAX 與下一個 PC；ARM64 使用 x8 作為編號，x0 作為結果。未知呼叫會以 `unsupported_service` 停止，絕不執行主機 syscall。

靜態 `PT_TLS` 樣板會視為 loader 提供的事實加以驗證：單一樣板、有限的檔案／記憶體範圍、相符對齊及可讀的初始化位元組。客體啟動會配置並初始化 TLS 區塊、安裝 thread pointer；Linux 模型不會虛構 libc 專用 TCB/DTV。這讓 freestanding 程式支援 compiler-generated local-exec TLS。Dynamic TLS 與 OS 執行緒仍是獨立工作。

x64 的 `arch_prctl` 支援 `ARCH_SET_FS`、`ARCH_GET_FS`、`ARCH_SET_GS`、`ARCH_GET_GS`。Set 可接受尚未對映的 user-range 基底，後續解參照仍會檢查權限。kernel-range 基底回傳客體 `EPERM`；無效 Get 目標回傳 `EFAULT`，不觸發 CPU fault。其他操作明確失敗。ARM64 啟動以 `MSR` 安裝 `TPIDR_EL0`；`MRS`、FS/GS 記憶體存取與內容還原會跨執行量和 backend 入口保留 thread pointer。這本身不實作 thread scheduler。

描述元 1、2 是虛擬位元組 sink。`write` 會驗證可讀的 user pages；若後續頁面無法存取，回傳可讀前綴；若沒有任何位元組可讀，回傳客體 `EFAULT`。錯誤描述元回傳 `EBADF`；有效描述元的零位元組寫入不會讀取指標。此處不模擬 Linux pipe 原子性或檔案物件。輸出超過限制時，會在發布寫入前停止。

匿名記憶體服務與映像、堆疊共用行程位址空間及實體記憶體預算。`mmap` 僅接受 `MAP_PRIVATE | MAP_ANONYMOUS`，權限為一般 `PROT_NONE`、`PROT_READ`、`PROT_READ | PROT_WRITE`、`PROT_READ | PROT_EXEC` 或可讀的 RWX。空閒且頁對齊的提示位址會被採用；否則先從 `0x100000000`、再從最低使用者位址搜尋空隙，並保留堆疊保護區。此確定性配置不模擬 Linux ASLR。新頁各自配置並清零；部分解除映射能回收未被固定的頁。CPU 投影或仍持有的 backing view 可將退役配置的生命週期延長至自身釋放時。

長度向上取整至頁。`munmap` 允許空洞及重複移除；`mprotect` 遇到空洞前會修改已映射前綴，再回傳 `ENOMEM`。`PROT_NONE` 保留配置與位元組，但禁止客體存取。原始 `brk` 成功時回傳請求的位元組邊界，失敗時回傳舊邊界，不採用 libc 包裝器的零／負一慣例。初始 break 為頁對齊的映像結尾。成長受其他映射及預算限制；縮減保留剩餘部分頁的位元組。支援子集的規則與錯誤優先序遵循 Linux [映射](https://github.com/torvalds/linux/blob/v6.8/mm/mmap.c)及[保護](https://github.com/torvalds/linux/blob/v6.8/mm/mprotect.c)服務。

檔案、共享、固定映射，向下成長、大頁、記憶體鎖定、保護鍵、僅執行／僅寫入策略及其他旗標均明確不支援：在發布效果或建立回傳值前停止。支援子集內的一般範圍、長度及對齊錯誤會回傳客體錯誤，允許繼續執行。任何記憶體服務均不會將客體指標或映射請求轉交主機 OS。

<a id="windows-pe64-profile"></a>

<!-- i18n-section: windows-pe64 -->

## Windows PE64 設定檔

`windows-pe64-v1` 支援有界 Windows x64/ARM64 主控台程序，包括 PEB/TEB、靜態與動態 TLS、`DllMain`、具名 Win32 API 和明確的無環 DLL 圖。客體模組支援依名稱／序號匯入程式碼與資料、DIR64 重定位、轉送匯出及真實載入器串列身分。`LoadLibraryA`／`LoadLibraryW`、`FreeLibrary` 和 `GetProcAddress` 使用設定的模組目錄。CRT／GUI、使用者態 SEH、執行緒及通用 Windows 應用程式相容性仍待完成；原生 ARM64 KVM/WHP 證據仍缺失。

Windows 虛擬記憶體新增 `VirtualAlloc`、`VirtualFree`、`VirtualProtect`、`VirtualQuery` 及目前行程的 `FlushInstructionCache`。OS 層管理保留區域，`AddressSpace` 統一管理已認可頁面、權限和實體儲存。測試涵蓋動態程式碼改寫、存取錯誤和記憶體額度回收。

私有配置支援 `MEM_RESERVE`、`MEM_COMMIT`、`MEM_DECOMMIT`、`MEM_RELEASE` 和 `MEM_TOP_DOWN`，保留區域以 64 KiB 對齊，頁面大小為 4 KiB。僅保留不消耗客體 RAM。重複認可保留資料並更新權限，取消認可歸還個別頁面的儲存。完整範圍檢查和分階段配置避免一般配置或權限失敗留下部分修改。查詢傳回 48 位元組的 x64/ARM64 記憶體資訊結構，僅在同一次配置內向後合併。初始映像、環境、堆積區域、API 入口和堆疊邊界都參與位址配置；堆疊的配置識別與 TEB 一致。如果成功的 `VirtualProtect` 將舊權限的輸出位址改成唯讀，新權限仍生效、輸出內容保持不變，呼叫仍傳回成功。 對未完整認可範圍的權限修改失敗時，傳回 `ERROR_INVALID_ADDRESS`，將舊權限輸出設為 `PAGE_NOACCESS`，各頁權限保持不變。

支援的權限為 `PAGE_NOACCESS`、`PAGE_READONLY`、`PAGE_READWRITE`、`PAGE_EXECUTE_READ` 和 `PAGE_EXECUTE_READWRITE`。防護頁、僅執行與寫入時複製策略、快取修飾符、大頁面、reset/write-watch/預留位置及修改模型擁有的執行階段映射仍明確拒絕。僅私有虛擬配置可取消認可或釋放。本項不增加使用者態例外派送能力，也不構成 ARM64 硬體原生執行證據。

[VirtualAlloc](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc), [VirtualFree](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualfree), [VirtualProtect](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualprotect), [VirtualQuery](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualquery), [MEMORY_BASIC_INFORMATION](https://learn.microsoft.com/windows/win32/api/winnt/ns-winnt-memory_basic_information).

```bash
neverd emulate guest.exe --profile=windows-pe64-v1 \
  --options='{"backend":"auto","arguments":["guest.exe","argument"],"environment":["MODE=test"]}'
```

單執行緒設定將 PE32+ EXE 保持在偏好基址，接受可含進入點及靜態 TLS 的明確 DLL。`WindowsProcessOptions::Modules` 或 JSON `windows.modules` 以 `name`、`path` 提供最多 64 個客體基本名稱與主機輸入路徑，不搜尋或執行主機 DLL。ASCII 名稱不分大小寫；拒絕重複名稱及覆寫系統 API 提供者，只讀取可達檔案。名稱／序號函式與資料匯入繫結實際映射匯出；空洞、缺少符號、循環、繫結／延遲匯入及未支援的載入設定／CFG 明確失敗。可移動 DLL 衝突時套用 DIR64；固定衝突及寫入連結中繼資料的重定位在發布受影響映像前失敗。

`readPEProgramExports` 擁有原始匯出身分與有界中繼資料讀取範圍；`WindowsProcessModules` 擁有模組圖和全程序精確提供者／名稱 API 跳板。`VirtualMemory` 在映射前登記全部映像，`AddressSpace` 管理頁面及權限。PEB/LDR 僅列真實映像，初始化串列保留載入器登記順序，並與依相依關係計算的掛接呼叫順序分別維護。`GetModuleHandleW` 接受 NULL 或 ASCII 基本名稱，不分大小寫，無副檔名時補 `.dll`；路徑、非 ASCII 查詢及結尾點規則仍不支援。找不到名稱回傳錯誤 126，成功保留 LastError。API 模型不是已安裝系統 DLL。

輸入總位元組與映像總範圍各受 `memory_limit` 限制，執行期映射也計入映像預算。準備階段共用 65,536 筆紀錄、64 MiB 中繼資料讀取、名稱長度及整體截止時間；阻塞主機 I/O 無硬即時保證。原創 EXE→DLL→DLL 樣例驗證重定位指標、序號呼叫、共享資料、API 指標身分、`MEM_IMAGE`、載入器串列及 EXE TLS 掛接／分離。`NeverDWindowsProcessTests` 包含直接原生 Windows 對照，`NeverDPEProgramExportsTests` 驗證畸形資料與預算，`NeverDProcessPublicTests` 驗證 C ABI/CLI 目錄一致性。不可用後端明確略過。

`WindowsProcessLifetime` 在相同 CPU 與執行預算下，依相依順序執行 DLL TLS 回呼及 `DllMain`，再執行 EXE TLS 與進入點。各模組具有獨立 TLS 索引與對齊區塊，從完成重定位和匯入繫結的映像複製，共用 64 KiB 空間。TLS 保留參數為零，啟動／程序結束的 `DllMain` 接收不透明非空值。明確程序結束依載入器串列的反向順序分離已完成初始化的 DLL，再執行 EXE TLS 結束回呼，即使 EXE 初始化尚未執行。啟動 `DllMain(FALSE)` 以 `0xc0000142` 結束，不發送分離通知。故障和預算耗盡不捏造清理。含客體 DLL 的 PE 進入點返回需要尚未支援的執行緒終止，因此明確停止。非零 `SizeOfZeroFill` 仍不支援；實際 TLS 範本中的零初始化位元組受支援。 無進入點 DLL 接收 TLS 掛接通知，但不接收程序分離通知。

`WindowsProcessExports` 為靜態匯入和 `GetProcAddress` 共用名稱／序號解析，涵蓋程式碼、資料、別名與鏈式轉送。 只有實際引用的啟動轉送會引入目錄模組及初始化相依；未使用的轉送不載入檔案。 匯出名稱區分大小寫；名稱缺失回傳 NULL／錯誤 127，直接查詢缺失序號（含空洞）回傳 NULL／錯誤 182，查詢參數為空指標回傳錯誤 87，成功保留 LastError。 未知模組控制代碼仍不支援。 有界 API 清單依精確提供者／名稱一次保留呼叫入口。 解析檢查每個映像的即時 PE 標頭與匯出中繼資料，拒絕修改或不可讀位元組，轉送鏈最多 64 項，並共用準備階段剩餘中繼資料額度及執行期限。 轉送到空洞時回傳目標映像基址並保留 LastError；轉送到零序號回傳錯誤 87。 回傳基址是資料位址，不授予映像標頭執行權限。 執行期轉送可載入設定目錄中的模組，並在回傳查詢結果前完成初始化。仍不支援即時改寫匯出表。

`WindowsProcessLoader` 從 `windows.modules` 載入 ASCII DLL 基底名稱，統一管理明確參考、共用相依與啟動模組保留。重複查詢轉送匯出不會增加額外參考。模組目錄槽位在重新載入時使用新的駐留世代。TLS 與 `DllMain` 在同一 CPU 上、暫停 API 的堆疊框架下方執行；還原暫存器保留客體記憶體寫入，並使用即時返回位址。動態附加／分離的保留指標為零。顯式載入期間的附加失敗在清理後回傳錯誤 1114，並保留已成功的獨立巢狀載入。卸載釋放映像映射與 TLS，重新載入恢復原始映像內容。模型之外對載入器串列或 TLS 指標的修改會明確失敗。失敗與重新載入皆不會重設檔案、映像及中繼資料工作額度。API 提供者沒有虛構 DLL 控制代碼。檔案系統搜尋、非 ASCII 路徑、`LoadLibraryEx` 旗標、循環匯入及正在初始化或卸載之同一模組的重入轉換仍不支援。

動態卸載回呼開始前，模組已退出初始化串列；其映射、名稱查詢及載入／記憶體串列成員身分在回呼期間仍然有效。入口返回的原生對照單獨觀察初始執行緒，不將系統工作執行緒的存活時間當作入口返回時間。

`WindowsDynamicTests.cpp` 使用原始 x64/ARM64 DLL 與 EXE，比對獨立原生 Windows 觀測，涵蓋參考計數、共用相依、巢狀載入、附加失敗清理、轉送查詢、程序退出、無入口 DLL 及重新載入時的 TLS 初始化。額外回歸拒絕遭修改的載入器中繼資料與失效程式碼指標，保持累計準備額度，並確保中斷 API 的結果仍未完成。Windows CI 強制執行原生對照與 WHP 案例；交叉編譯及 Unicorn ARM64 不代表原生 ARM64 執行驗證。

`GetProcAddress` 轉發鏈任一位置缺少程式庫皆傳回錯誤 127；顯式 `LoadLibrary` 載入目錄中缺少的模組傳回 126。原生對照與各可用後端均斷言全部 41 個已宣告載入情境；Windows 上每種 DLL 變體都重複 16 次驗證全部卸載後從入口返回。 `GetProcAddress` 轉發目標初始化失敗也在清理後回傳 127。行程分離回呼保留退出呼叫端的堆疊內容。

`WindowsExportTests.cpp` 使用原始 x64/ARM64 DLL 與 EXE，驗證轉送的程式碼／資料／序號呼叫、別名、初始化查詢、重定位、大小寫敏感的缺失項、LastError、循環與非駐留目標、無效指標，以及成功查詢後的中繼資料修改。同一 EXE 有獨立原生 Windows 對照；原生 CI 強制執行 WHP 案例。C ABI／CLI 測試比對完整報告。原生 ARM64 硬體證據仍待補齊。 具有及不具有匯出表的 EXE 變體涵蓋兩種相依圖、PEB 串列順序、結束通知順序，以及名稱／序號／空指標的錯誤碼。

`WindowsLifetimeTests.cpp` 將固定通知序列與獨立原生 Windows 程序及 KVM/WHP/Unicorn 執行比對，涵蓋正常結束、進入點返回、兩個 DLL 初始化失敗、四處提早結束及無進入點 DLL。另驗證回呼故障、共用預算、重定位 TLS 欄位及 TLS 總容量。原生進入點返回探針保留初始執行緒控制代碼，重複64 次核對執行緒結束碼及精確執行緒／程序通知序列。觀察後終止其餘子程序執行緒，不將程序結束碼視為進入點返回值。

```json
{"windows":{"modules":[{"name":"middle.dll","path":"inputs/middle.dll"},{"name":"leaf.dll","path":"inputs/leaf.dll"}]}}
```

x64 GS、ARM64 x18 指向 TEB，提供堆疊邊界、自指標、PID/TID、PEB、程序參數、LastError 與 TLS。 嚴格將 UTF-8 轉為 UTF-16，argv 依 Microsoft CRT 規則加引號。 環境名稱限 ASCII，拒絕不分大小寫的重複名稱；值可為 Unicode，排序後以雙 NUL 結尾，不繼承主機環境或檔案系統。 靜態 TLS 複製範本、清零 BSS、寫入 32 位索引；動態 TLS 使用獨立 TEB 槽位。 啟動／結束依序讀取即時回呼表，共用截止時間及資源額度。 正常程序結束會執行結束回呼。 入口返回僅支援目前沒有駐留客體 DLL 的情況；程序退出清理期間再次呼叫 `ExitProcess` 仍不支援。

`WindowsProcessServices.def` 管理完整 API 清單：`ExitProcess`、`RtlExitUserProcess`、標準輸出控制代碼與同步 `WriteFile`、LastError、程序／執行緒識別與虛擬控制代碼、`GetCommandLineW`、程序堆配置／釋放／大小、動態 TLS、`LoadLibraryA` / `LoadLibraryW` / `FreeLibrary` / `GetModuleHandleW` / `GetProcAddress`。提供者限 `kernel32.dll`、`kernelbase.dll`、`ntdll.dll` 並精確匹配匯出名稱。直接 syscall 與偽造回呼入口無法選擇 API。堆有程序所有權並於釋放時回收；輸出保留二進位資料，API 參數錯誤、非同步 I/O 與使用者例外限制分開處理。別名指標會看到完成計數的初始清零與實際返回位址變更。

`windows.native_calls` 保留 DLL／函式名稱、宣告的純量參數及可空結果，不假造 NT syscall 編號。`NeverDWindowsProcessTests` 驗證真實 PE、編譯器 TLS、回呼修改、堆／LastError、別名、畸形資料、權限與預算；`NeverDProcessPublicTests` 驗證 CLI/C ABI。Windows CI 直接執行相同 EXE 作獨立對照，並要求 WHP 測試通過；原生 ARM64 執行證據仍需對應機器。

`WriteFile` 的非空輸入緩衝區無法讀取時，會回傳 `ERROR_INVALID_USER_BUFFER`（1784）、將完成計數歸零，且不輸出任何位元組。

[PE/COFF](https://learn.microsoft.com/windows/win32/debug/pe-format), [ARM64 ABI](https://learn.microsoft.com/cpp/build/arm64-windows-abi-conventions), [WriteFile](https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-writefile), [TLS](https://learn.microsoft.com/windows/win32/api/processthreadsapi/nf-processthreadsapi-tlsgetvalue), [Wine 10.0 loader](https://github.com/wine-mirror/wine/blob/wine-10.0/dlls/ntdll/loader.c). [GetProcAddress](https://learn.microsoft.com/windows/win32/api/libloaderapi/nf-libloaderapi-getprocaddress).

<!-- i18n-section: verification -->

## 驗證

```bash
cmake --build build-cpu --target NeverDLinuxProcessTests NeverDExecutionSessionTests NeverDX64MemoryUpdateTests NeverDThreadPointerTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(LinuxProcess|ExecutionSession|X64MemoryUpdate|ThreadPointer)Tests$' --output-on-failure
# shared-library/CLI 建置：
cmake --build build-cpu --target NeverDProcessPublicTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDProcessPublicTests$' --output-on-failure
```

測試為兩種 ISA 編譯獨立 ELF 入口組合語言和 C，驗證 data/BSS、真實啟動中繼資料、系統呼叫錯誤、二進位輸出、權限錯誤、部分寫入、不支援的服務與跨執行量預算。TLS 案例初始化獨立對齊區塊、清零 TLS BSS、安裝執行緒指標並檢查切換後保留；x64 也檢查 `arch_prctl` 錯誤不會丟失舊基址。不可用後端明確略過。公開測試經共享 C ABI 與 CLI 核對報告及結束碼。靜態 PIE 在自行重定位資料及函式指標前驗證 auxv 與原始為零的 RELA 槽。映射測試另外檢查選擇分析位元組來源時保留 fixup；動態表測試涵蓋缺少 section 及畸形／相依輸入。匿名記憶體案例涵蓋兩種 ISA 的配置、保護、空洞、重新映射、堆積成長／縮減與可處理的系統呼叫錯誤；真實客體寫入驗證一般及部分保護更改後的錯誤。x64 還在 RW/RX 切換間重寫同址程式碼並呼叫兩版；相同 ELF 在 Linux 原生執行，作為獨立結果／錯誤參照。純記憶體測試涵蓋預算耗盡、回收及不持有 RAM 的權威映射快照。交叉編譯與 Unicorn ARM64 結果不構成原生 ARM64 KVM/WHP 證據。
