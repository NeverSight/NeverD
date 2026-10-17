**語言**: [English](../darwin-emulation.md) | [简体中文](../zh-CN/darwin-emulation.md) | [繁體中文](darwin-emulation.md) | [日本語](../ja/darwin-emulation.md) | [한국어](../ko/darwin-emulation.md) | [Français](../fr/darwin-emulation.md) | [Deutsch](../de/darwin-emulation.md) | [Español](../es/darwin-emulation.md) | [Italiano](../it/darwin-emulation.md) | [Русский](../ru/darwin-emulation.md) | [العربية](../ar/darwin-emulation.md)

<!-- i18n-source: 26e0c6dba6d3b863bb8dc0c4690bbc6ea61bbe5dd2accd19643847c8968fc13e -->

[← 文件索引](README.md)

# macOS 與 iOS 客體行程環境

`lib/emulation/os/darwin/` 提供有界、獨立的 Mach-O 行程模型，與宿主 CPU 傳輸層分離。啟用 `NEVERD_ENABLE_CPU_EMULATION` 即可，不必開啟 Windows 驅動模擬。`macos/` 與 `ios/` 分別定義明確的平台契約。

| Profile | Mach-O 平台 | 客體 ISA | OS 頁大小 |
| --- | --- | --- | --- |
| `macos-macho64-v1` | macOS | x86-64、基礎 ARM64 | x64 4 KiB；ARM64 16 KiB |
| `ios-macho64-v1` | iOS 實體裝置 | 基礎 ARM64 | 16 KiB |
| `ios-simulator-macho64-v1` | iOS Simulator | x86-64、基礎 ARM64 | x64 4 KiB；ARM64 16 KiB |

裝置映像與模擬器映像不可互換，宿主不決定客體平台。macOS 上 ISA 相符時可用 [HVF](macos-hvf.md)，跨 ISA 的 `auto` 使用 Unicorn。CPU 映射粒度仍為 4 KiB。[C、Python 與 CLI API](process-emulation.md) 共用選項、限制及報告。

```sh
neverd emulate guest.macho --profile=ios-macho64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"]}'
```

## 映像與啟動

`MachOExecutionImage` 保留原始位元組，不套用分析階段的重定位修補。只接受 little-endian、thin `MH_EXECUTE`，且平台與進入點必須明確。通用映像需要先明確擷取所需架構切片。

完整檔案包含中繼資料及尾端位元組，在解析與複製前都必須符合 `memory_limit`。載入器從一般檔案讀取有界的私有快照，拒絕 NUL 路徑、短讀及大小變化，不保留即時檔案映射。檔案與客體記憶體各有獨立、同值的上限；宿主檔案 I/O 沒有硬即時期限。

區段保留目前／最大權限與零填入。`__PAGEZERO` 只保留位址，不配置整段實體記憶體。執行前檢查檔案／VM 範圍、OS 頁對齊、取整後重疊、標頭歸屬、可執行進入點及預算。標頭區段必須可讀且可執行；保護頁和私有返回入口持續保留。檔案最後一頁保留至頁界或 EOF 的原始位元組，其後完整 VM 頁清零，依據 [XNU 載入器](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/mach_loader.c)。

`LC_MAIN` 接收 `argc`、`argv`、`envp` 和 apple 向量四個整數參數；返回值低八位元成為退出狀態。在符合下述有限匯入契約的這種進入交接中接受 `/usr/lib/dyld`，不執行宿主 dyld。非零 `stacksize` 會拒絕，因為呼叫端明確的 `stack_size` 掌握預算。

`LC_UNIXTHREAD` 要求恰好一份完整的原生 64 位元一般暫存器紀錄，只有 PC 已設定。起始堆疊含 argc、以零結尾的 argv/envp，以及含 `executable_path=<input filename>` 的零結尾 apple 向量。自訂 SP、旗標、其他暫存器、額外 flavor 或衝突進入點均拒絕。不繼承宿主環境或 Linux 輔助向量。參考 [dyld 架構](https://github.com/apple-oss-distributions/dyld/blob/main/doc/dyld4.md)。

支援下述有限 libSystem、明確指定的啟動 dylib、啟動建構函式、主執行緒 C/C++ TLS 與行程終止時的 C/C++ 解構。選擇性 DSO 終結、舊式 TLS 初始化段、arm64e/PAC、未支援子型別、加密內容及未建模命令仍在執行前失敗。主程式使用偏好位址，dylib 採確定性獨立配置；簽章資料不實作 AMFI 或 entitlement。

## Darwin 服務

BSD 呼叫在 ARM64 使用 X16、X0–X5 與 `svc #0x80`；x64 使用 BSD 類別 `0x02000000`、RAX 及 RDI/RSI/RDX/R10/R8/R9。成功清除 carry，失敗設定 carry 並返回正 errno。ARM64 清除 X1；x64 成功清除 RDX、失敗保留 RDX。SYSCALL 的暫存器改寫明確定義。報告以 `result` 與 `error=true` 表達 BSD 錯誤；不返回或不支援的請求在 JSON 中使用 `result: null`，並省略 `error`。規則依據 XNU [ARM64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/arm/systemcalls.c) 與 [x64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/i386/systemcalls.c)，未納入 Apple 實作程式碼。

服務包含 `exit`、`write`、`getpid`、`getppid`、`getuid`、`geteuid`、`getgid`、`getegid`、`getgroups`、`mmap`、`mprotect`、`munmap`。PID 為 1000，PPID 為 1；UID/GID 預設 1000，可由下述憑據明確提供不同的真實/有效 ID。描述元 1、2 擷取原始位元組，包含 NUL 與非 UTF8；關閉或唯讀描述元返回 EBADF。部分複製已取得的資料會保留，但後續錯誤仍為 EFAULT。長度超過 `INT_MAX` 時，先返回 EINVAL，再談描述元、指標或預算檢查，依據 [XNU write](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/sys_generic.c)。

記憶體服務支援私有匿名資料映射：`flags=0x1002`、描述元 -1、offset 零。長度及非固定提示位址向上取整至 OS 頁；提示已占用時先向上搜尋，再回到預設配置區。舊式原始 mmap 零長度返回零且不配置；`MAP_UNIX03` 已支援，零長度返回 EINVAL。Unmap/protect 位址必須對齊。支援 NONE/READ/WRITE，WRITE 隱含 READ。每個 OS 頁獨立持有實體記憶體，部分解除映射可釋放預算，新頁面清零。Protect 跨空洞或超過最大權限時，整個範圍維持原狀。來源：[XNU VM 服務](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/kern_mman.c)。

共享／固定／JIT 映射、匿名可執行映射、其他 Mach trap、間接系統呼叫、執行緒、訊號、宿主檔案／網路、dyld、Objective-C/Swift runtime 和 Foundation/UIKit 均不支援，會明確停止。這不是完整 Apple OS，也不是 iOS Simulator 應用程式。

## 驗證

自行撰寫的 C 測試映像以 Clang 與 `ld64.lld` 產生，不依賴 Apple SDK 或專有二進位檔。涵蓋五種平台／ISA、畸形 Mach-O、4/16 KiB 頁與預算已滿時的部分釋放。`NeverDProcessPublicTests` 比對 C API/CLI；設定 `NEVERD_TEST_LIBNEVERD` 與 `NEVERD_TEST_DARWIN_FIXTURES` 可讓 Python SDK 測試相同五種組合。

## 明確的檔案輸入與描述元

`darwin_files` 為三個 Darwin profile 提供封閉的初始唯讀檔案目錄。必要的 `files` 項目包含規範絕對客體 `path` 和十六進位 `bytes_hex`；選用 `stdin_hex` 提供有限輸入流。省略表示未知，非零讀取會停止；空字串表示 EOF。未設定目錄時 open 停止，明確空目錄中缺失的絕對路徑返回 ENOENT，不會存取宿主檔案或輸入。

新增 `open`、`read`、`pread`、`lseek`、`close`、`dup`、`dup2`、`fcntl`；read/write/open/close/fcntl/pread 的 nocancel 入口共用實作。支援 O_RDONLY/O_CLOEXEC 與 F_DUPFD、F_DUPFD_CLOEXEC、F_GETFD、F_SETFD、F_GETFL。獨立開啟有獨立游標，複製描述元共用游標但各自保留 close-on-exec；pread 不移動游標。關閉或替換 0/1/2 會影響後續 I/O，複製輸出仍使用原擷取通道與共享預算。

上限為 256 個檔案、路徑/NUL/檔案/輸入合計 16 MiB、路徑少於 1024 位元組、每個分量最多 255 位元組。`descriptor_limit` 為排他上界 3–4096，預設 256；JSON 仍限 64 KiB。非法設定在載入前拒絕。read 超過 INT_MAX 先返回 EINVAL；EOF 不存取目的位址，無效位址返回 EFAULT。部分可寫緩衝區在任何複製或游標改動前停止。SET/CUR/END 定位失敗保留游標。舊版 stat 及其他 fcntl 仍未支援。普通檔案作為祖先返回 ENOTDIR。同一原始目標檔案對照原生 macOS；C/CLI/Python 涵蓋五種客體組合，不代表 iOS 真機驗證。

2026-10-05 Release Darwin 驗收共 381 項：177 通過、204 跳過、零失敗，ARM64 HVF 必需項 51/51 執行。原生 macOS 7 個程式、公共 C/CLI 與報告 35 項、Python 五種客體組合和驗收腳本 66 項通過，計數有重疊。新增檔案服務尚無 Intel HVF/KVM/WHP 原生證據；Intel HVF 仍未驗證且暫停 Actions。宿主缺少 iOS SDK，也沒有 iOS 真機對照。

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839"}],"stdin_hex":"00ff78","descriptor_limit":32}}
```

## 既有檔案的可寫內容

檔案以嚴格布林值 `"writable":true` 明確允許行程內修改；C++ 使用 `DarwinFileOptions::WritableFiles`。省略或 false 維持唯讀，未知授權停止，不存取宿主或改動輸入。write(4/397)、pwrite(154/415)、truncate(200)、ftruncate(201) 與 O_TRUNC 共用內容節點；各次 open 游標獨立，dup 共用游標與狀態，最後 close 後重開仍保留內容。增長補零，截斷保留游標；O_RDONLY|O_TRUNC 也會截斷。

F_SETFL 依原生旗標轉換只改 O_APPEND|O_NONBLOCK 並保留存取模式、close-on-exec 與 FWASWRITTEN；實際傳輸非零位元組後 F_GETFL 顯示 0x10000，包含 pwrite 和輸出擷取。pwrite 忽略 append、不移游標。INT_MAX 長度檢查先於 FD，偏移 -1 的 pwrite 更早返回 EINVAL；INT64_MAX 在零寫之前返回 EFBIG，長度先裁剪再選 EOF。

成功的 ftruncate（含大小不變）也為呼叫描述及其 dup 設定 FWASWRITTEN；O_TRUNC 為新描述設定，包含 O_RDONLY。路徑 truncate 不改變既有描述的旗標。

部分可讀輸入在效果前停止；完整 EFAULT 保留內容，但非空追加會移至 EOF。後端失敗不提交內容或游標。未設定 `mutation_policy` 時，非零寫入、截斷及非零完整 EFAULT 使完整 stat 觀察失效，後續查詢在輸出前停止；零寫不失效。16 MiB 為路徑/NUL、輸入、目錄記錄、CWD、現有內容及可寫路徑引用的合計邏輯預算；縮小替換 backing 並回收容量，原始輸入與一個有界替換緩衝區另計。已知 inode 別名和 immutable/append-only 旗標暫拒絕。

映射租約由 DarwinMemory 持有，全部區間 unmap 前拒絕 write、truncate、O_TRUNC，包含 PROT_NONE 和已 close 的 FD；失敗或舊式零長度映射不留租約。新映射取得目前內容。只寫 FD 的 READ/WRITE mmap 為 EACCES；PROT_NONE 可成功並以 mprotect 取得讀寫權限。

原生一般/nocancel 程式比對位元組、游標、旗標及錯誤順序；單元測試涵蓋 4K/16K，C/CLI/Python 涵蓋五種組合。權限強制檢查、刪除目錄、不同初始目錄域之間的改名、硬連結、真實檔案系統的中繼資料更新、映射一致性與 EOF SIGBUS 尚缺；完整環境、iOS 實機與 Intel HVF 仍未驗收，Intel Actions 維持暫停。

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233","writable":true}]}}
```

[XNU write](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/sys_generic.c), [XNU vnode](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c), [XNU mmap](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mman.c).

## 明確可變中繼資料

檔案可在 `writable: true` 與完整 `metadata` 旁設定 `mutation_policy`；C++ 使用 `DarwinFileOptions::MutationPolicies`。這是明確選擇的虛擬稀疏分配契約，不推斷 APFS 或讀取宿主時間；省略時修改後中繼資料仍未知。

allocation_unit、mutation_time 及 seconds/nanoseconds 全部必填，整數沿用無損規則。單位為 512 位元組至 16 MiB 的二次冪，獨立於 block_size 與 VM 頁。須為普通檔案權限（無 set-id/sticky）、flags=0、link_count=1，且初始密集分配滿足 blocks=ceil(size/allocation_unit)*(allocation_unit/512)。零位元組不能推斷為洞。策略路徑引用計入 16 MiB 邏輯預算；分配帳本不推導 ENOSPC。

寫入分配所有觸及單位，向洞寫零也分配。truncate 增長只補零；縮小丟棄向上取整 EOF 之後的單位並保留末尾已分配的部分單位，重新增長不恢復已丟棄分配。非零成功寫和每次成功截斷（含同大小、空 O_TRUNC）更新 size/blocks，並設定固定 mtime/ctime；其餘欄位不變，讀取不推進 atime。路徑 stat、獨立 open、dup、重開共用節點，原始輸入不變。

零寫、預算/映射拒絕、部分輸入和後端失敗保留已知狀態；非零完整 EFAULT 仍使中繼資料永久未知，後續成功修改不能恢復。stat 複製失敗不改節點。virtual-file-metadata 經五種組合與 C/CLI/Python 檢查144位元組記錄；分配是策略測試，非 APFS 原生等價證據。原生程式另驗證旗標、游標與錯誤順序。命名空間、真實檔案系統一致性、Mach 與動態執行階段仍待完成。

```json
{"mutation_policy":{"allocation_unit":4096,"mutation_time":{"seconds":-7,"nanoseconds":123456789}}}
```


## 稀疏檔案定位

普通檔案具有 mutation_policy 且分配仍已知時，lseek 支援 SEEK_HOLE=3、SEEK_DATA=4，共用 stat 的分配帳本。首次修改前為明確密集分配，零值不表示洞。位於所求類型內便回傳原偏移，否則回傳下一單位起點；末尾洞從 EOF 開始。負偏移為 EINVAL，位於/超過 EOF（含空檔）或沒有後續資料為 ENXIO=6。失敗保留游標，成功只改目前描述與 dup；獨立 open 不受影響，重開看目前分配。中繼資料、旗標、內容不變，whence 高位忽略。

缺少策略、目錄及完整 EFAULT 後未知分配仍拒絕，不從零值或失敗修改推斷分配。原生/客體 sparse-file-seek 驗證錯誤、已寫位元組、EOF 與描述生命週期，不假設較早區段位置；virtual-file-metadata 驗證精確策略幾何，C/CLI/Python 涵蓋五種組合。

[XNU lseek](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

## 刪除一般檔案名稱

目錄的 `mutable:true`（C++ `MutableDirectories`）明確授權修改直接子項名稱，獨立於檔案內容的 `writable`；下例允許刪除唯讀檔案。缺少授權會停止，不依權限位虛構憑證或 EACCES。拒絕父目錄/直接一般子檔案的非零已知 flags、父目錄特殊權限、子檔案 link_count 不為 1，以及父目錄或子檔案的已知身份別名。身份檢查包含 stat 與目錄快照 inode；明確不同的裝置保持獨立，授權路徑計入預算。

`unlink(10)` / `unlinkat(472)` 刪除既有一般名稱；一般檔案刪除僅接受低 32 位 flags=0 或 `AT_SYMLINK_NOFOLLOW_ANY=0x800`。未知位先回 EINVAL；AT_REMOVEDIR 使用下述受限目錄刪除；DATALESS、SYSTEM_DISCARDED 未支援。共用解析器保留路徑故障、dirfd、CWD 與絕對路徑順序；缺失 ENOENT、檔案後斜線 ENOTDIR、一般目錄 EPERM、純斜線根路徑 EISDIR，尾端帶 `.`/`..` 的根路徑 EBUSY。原生探針亦驗證末尾 `.`/`..`。

舊 FD、dup、獨立開啟物件保留資料、游標與旗標，新開啟失敗；隱式父目錄與 CWD 保留，F_GETPATH 仍回傳捕獲的舊路徑。寫授權屬於檔案物件；全部描述元與最後一段映射釋放後，close/dup2 或下次修改才回收目前位元組預算，初始路徑/引用費用仍計入。權限強制檢查、不同初始目錄域之間的改名與硬連結尚缺；刪除初始目錄使用下文的明確授權。

刪除後父目錄的 stat/列舉觀察對舊/新 FD、dup、路徑查詢皆失效；stat/readdir/SEEK_END 在複製或改游標前停止。read/pread 仍回 EISDIR，SET/CUR、F_GETPATH、fchdir 與相對查找可用。可信修改策略令 nlink=0、ctime=固定時間，保留其他時間、資料與分配；後續寫入不能恢復 nlink=1。無策略或整段 EFAULT 後中繼資料仍未知。失敗保留狀態；原生 `unlinked-file` 比較名稱/描述元，策略時間與目錄失效屬模型規則。

```json
{"darwin_files":{"files":[{"path":"/work/data","bytes_hex":"00"}],"directories":[{"path":"/work","mutable":true}]}}
```

[XNU unlink / unlinkat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [F_GETPATH](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_descrip.c).

## 建立一般檔案

O_CREAT=0x200 在明確 mutable 的直接父目錄建立空檔案，涵蓋一般/nocancel open、openat。新物件可寫，既有物件仍依 WritableFiles；唯讀 FD 可建立但不可寫入。未提供建立策略時，stat64 與稀疏定位仍未知；新物件始終不繼承同名舊物件的 metadata/mutation_policy。

搭配 O_EXCL=0x800 時，既有檔案或目錄先回 EEXIST，不截斷；單獨 O_EXCL 無效。唯讀 O_CREAT 可開啟既有目錄。經過下述 openat 首位元組與目錄預檢後，順序為無效存取模式→FD 容量→O_CREAT|O_DIRECTORY 的 EINVAL→路徑。只有原始路徑最後缺失分量可建立，缺失祖先或末尾 `/`、`//`、`/.`、`/..` 為 ENOENT。新建 O_CREAT|O_TRUNC 不設 FWASWRITTEN，截斷既有物件則設定。

僅實際插入使父目錄觀測失效。同名新舊物件的資料、描述元、中繼資料與映射租約獨立。256 項涵蓋固定初始非檔案項、具名與存活孤立物件；新規範路徑/NUL 和目前資料計入 16 MiB，刪除且最後 FD/映射釋放後才回收，初始費用保留。預算耗盡或規範路徑達 1024 位元組明確停止，不捏造 ENOSPC 或原生路徑錯誤，失敗不建立名稱/FD。created-file 原生/五種來賓及 4K/16K 邊界測試驗證此合約；權限強制檢查、不同初始目錄域之間的改名、連結及目錄修改仍待完善。

[XNU open](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c).

## 明確的建立中繼資料與程序 umask

可選 `darwin_files.umask`（C++ `InitialUmask`）獨立宣告初始程序遮罩，範圍為八進位 0..07777，不要求建立授權。`umask(60)` 回傳舊遮罩並保存輸入的低 07777 位，不存取來賓記憶體，也不需要空閒 FD。省略代表未知，不讀取主機或猜測預設值。遮罩只初始化一次，只影響後續建立，不修改呼叫者輸入。下例十進位 18 即八進位 0022。

未設定 `namespace_policy` 時，可選 `darwin_files.creation_policy`（C++ `CreationPolicy`）為新物件提供完整中繼資料。嚴格物件恰含 `first_inode`、`block_size`、`generation`、`creation_time`、`mutation_policy`，時間與修改策略沿用既有格式。必須明確提供 umask、至少一個 mutable 父目錄，以及每個授權父目錄的完整 metadata。block_size 為 1..INT32_MAX、generation 為 uint32；配置單元是 512..16 MiB 的二次方，獨立於區塊大小和 VM 頁，奈秒須在 [0,1000000000)。first_inode 是非零 uint64，嚴格大於所有 stat/快照 inode，包含其他裝置；超過 JSON 精確整數範圍時使用十進位字串。

只有成功插入新名稱才消耗全域遞增 inode；成功使用 UINT64_MAX 後永久耗盡，關閉、刪除、名稱重用、umask 或後續查找不能重置。排他、FD、路徑、項目或位元組預算失敗不留下名稱、FD 或編號增量；既有 O_CREAT 不消耗編號。新 stat64 的 device/GID 繼承直接父目錄，UID 為設定選取的來賓有效使用者 ID（預設 1000），mode 為 `S_IFREG | (mode & 0777 & ~umask)`，nlink=1，size/blocks/flags=0；區塊大小、generation 及四個初始固定時間由策略提供。父目錄完整 stat/列舉失效後，仍可使用不變的 device/GID，但不恢復完整記錄。

新節點獨立持有中繼資料與配置狀態，不繼承同名舊物件。後續寫入、截斷與刪除共用修改策略，保留 inode/mode/birthtime 和刪除後的 nlink=0；整段 EFAULT 後仍永久未知。策略不追溯套用到既有節點。原生 `created-file-metadata` 對照權限、遮罩回傳值、有效 UID、父裝置/群組及身分存續，涵蓋五種來賓；`virtual-created-metadata` 另比對完整 144 位元組記錄。原生四個建立時間不一定相等；固定時間與稀疏配置是虛擬檔案系統規則。權限強制檢查、憑據切換、ACL 與原生 APFS 行為仍待實作。

```json
{"darwin_files":{"files":[],"umask":18}}
```

[XNU creation](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c), [XNU umask](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c). [XNU rename / renameat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

## 一般檔案與建立目錄的原子交換

RENAME_SWAP=0x2 透過 renameatx_np 交換兩個既有一般檔案、兩個仍連結的行程建立目錄，或檔案與建立目錄；可加 RENAME_NOFOLLOW_ANY。明確初始目錄須宣告 mutable:true 與 swap_rename:true，C++ 使用 DarwinFileOptions::SwapRenameDirectories。建立後代繼承原始目錄物件的能力，刪除和重用名稱不轉移舊宣告。false 或省略表示未知；相同裝置和修改授權不能證明能力，不同初始目錄域仍不支援。

`openat_nocancel`、`fstatat64` 與 `F_GETPATH=50` 使用同一檔案元件；交換後的路徑與觀察仍屬於各自物件，完整 stat 的可用性仍依中繼資料契約。

目標不存在（含尾斜線）時先回傳 ENOENT，先於來源點/雙點、域、授權與能力檢查。初始或已刪除目錄運算元仍不支援。在獲准域內，任一方向的父子目錄交換及目錄與子檔案交換回傳 EINVAL；一般檔案目標尾斜線為 ENOTDIR。一般元件的同物件交換經命名空間授權後為空操作，即使沒有能力宣告。來源點/雙點與目標同物件時，檔案系統大小寫屬性仍未知。RENAME_EXCL=0x4 + RENAME_SWAP=0x2 和未知 flags 在路徑讀取前回傳 EINVAL；SECLUDE 不支援。

兩棵非空子樹依父物件鏈移動，包含 FD 或映射保留的已刪除子目錄與檔案孤兒。混合交換只移動確切的一般檔案根；舊同名孤兒仍跟隨自己的父物件。FD、dup、CWD 與雙點跟隨物件及新父目錄。後代檔案的位元組、身分、中繼資料、寫入授權、游標、旗標與租約保留；移動根及兩邊直接父目錄套用既有命名空間中繼資料規則。設定策略更新根檔案自身 ctime；無策略或已失效時完整中繼資料仍未知。

能力參照的路徑+NUL 保留於固定初始 16 MiB 預算。兩邊所有連結及保留路徑在發布前檢查 1024 位元組界限和共用總額，再共同撤下舊名稱並發布新名稱。交換不刪除根、不使用覆蓋回收額度，未開啟目標的位元組也不能抵扣。初始檔案首次移動取得動態路徑費用，交換回不清零，往返不累加；無需新 FD、項目或建立 inode。失敗時兩邊名稱、父物件、游標、觀察及映射不變。原創無 SDK 的 swapped-directory 程式透過原生 macOS 與五種 C++/C/CLI/Python 設定比對目錄交換及兩種混合順序。

```json
{"darwin_files":{"files":[{"path":"/work/data","bytes_hex":"3031"}],"directories":[{"path":"/work/empty"}],"working_directory":"/work"}}
```

## 明確目錄快照

`getdirentries64` (344) 列舉既有 `directories` 項目的可選唯讀 `contents`；C++ 使用 `DarwinFileOptions::DirectoryContents`。`entries` 必須依明確順序完整列出 `.`、`..` 和所有直接子項。空目錄缺少快照仍表示未知；快照不建立路徑或 stat 中繼資料，也不查詢宿主檔案。

每項必填 `name`、非零 `inode`、`type`（0 未知、4 目錄、8 一般檔案）、`next_offset`、`seek_offset`。類型必須符合路徑；相同解析路徑的 inode 必須與其他快照及中繼資料一致。`next_offset` 是目錄內唯一、非零、不超過 INT64_MAX 的游標標記，不必遞增；零表示回到開頭。`seek_offset` 是獨立的無符號 64 位 d_seekoff 觀察值，可重複為零。整數沿用 stat 的無損十進位字串規則。

`contents.minimum_buffer_size` 必填，表示包括 EOF 的載荷下限，範圍 1–128 MiB。項目可另設 `minimum_buffer_size`（預設 0），約束從該項開始的讀取。範例記錄 APFS 起始兩個點項至少需 64 位元組、EOF 只需 1 位元組；其他位置至少容納完整記錄。LP64 記錄按 8 位元組對齊，大小為 `roundUp(25 + nameBytes, 8)`。所有快照最多 4096 項，編碼位元組計入 16 MiB 預算；僅中繼資料/快照宣告的祖先路徑去重後計入 256 路徑上限；JSON 仍限 64 KiB。

獨立 open 有獨立游標，dup 共用；只接受零或宣告的標記，未知位置明確停止。每次回傳能容納的最大完整記錄前綴。長度 >=1024 時，原請求末四位元組為 EOF 旗標（末尾 1，否則 0），僅記錄載荷限制為 128 MiB；旗標位址保留原無符號長度及回繞。順序是複製資料、移動游標、複製讀取前位置、寫旗標。後續 EFAULT 保留已完成效果；EOF 不複製空資料。單次複製僅部分可寫時，在該次複製前明確停止，保留先前效果。

同一 `directory-entries` 對照原生 macOS 的記錄、dup/回繞、小塊讀取、EOF 和複製順序；獨立測試依 SDK 逐位元組核對捕獲記錄及長檔名。快照標記回繞後不變，不模擬 APFS 動態世代。舊 `getdirentries` (196)、修改後的目錄列舉、其他原生後端及 iOS 實機尚未納入驗收。

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"3031"}],"directories":[{"path":"/empty"},{"path":"/","contents":{
  "minimum_buffer_size":1,"entries":[
    {"name":".","inode":41,"type":4,"next_offset":11,"seek_offset":0,"minimum_buffer_size":64},
    {"name":"..","inode":41,"type":4,"next_offset":22,"seek_offset":0},
    {"name":"empty","inode":42,"type":4,"next_offset":7,"seek_offset":0},
    {"name":"data","inode":73,"type":8,"next_offset":99,"seek_offset":0}]}}]}}
```

[XNU getdirentries64](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [dirent ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/dirent.h), [extended flags](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/dirent_private.h).

目錄列舉驗收（2026-10-05，Release）：Darwin 共 498 項，246 通過、252 項因後端不可用跳過、零失敗；ARM64 HVF 必需項 63/63 執行。11 個原生 macOS 程式、40 項公共 C/CLI/報告（無跳過）、Python 五種客體组合各八種檔案情境及 66 項驗收腳本通過，計數重疊。證據：`build-hvf-arm64/darwin-dirents-merged-evidence/`。Intel HVF Actions 保持暫停；其他原生後端和 iOS 實機未驗收。

## 私有檔案映射

`mmap` 支援一般目錄檔案的 `MAP_PRIVATE` 映射：`flags=0x2`，或加上 `MAP_UNIX03` 的 `0x40002`；偏移必須按 OS 頁對齊。即使要求長度較短，整頁仍保留原始檔案位元組，EOF 尾頁剩餘部分清零。私有寫入不改變原檔、其他映射、固定中繼資料或共用游標。close 或重用描述元後映射仍有效。唯讀與 PROT_NONE 映射也有完整初始內容，可透過 `mprotect` 增加寫入權限。

檔案末尾溢位、UNIX03 零長度或未對齊偏移在 FD 查詢前返回 EINVAL；無效 FD 在預算檢查前返回 EBADF。舊式零長度仍檢查 FD，然後返回零而不配置。舊式未對齊偏移、串流、空檔頁和完整越過 EOF 的頁在配置前明確停止。原生 macOS 允許映射 EOF 外完整頁，但存取會觸發 SIGBUS；模型不偽造零頁或訊號傳遞。共享、固定、可執行與 JIT 映射仍未支援。

`DarwinFiles` 管理描述元和位元組，`DarwinMemory` 管理配置、權限、預算與回滾；資料只來自 `darwin_files`。`file-mapping` 原生/客體程式檢查私有寫入、close 後壽命、游標、錯誤與匿名頁重用；獨立原生測試比對非零偏移、整頁內容與 SIGBUS 邊界。

[XNU mmap](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mman.c)

### 私有映射驗證（2026-10-05）

Release Darwin：438 個唯一登記項，210 通過、228 跳過、零失敗；57/57 個 ARM64 HVF 必需項執行，Unicorn 涵蓋五種客體組合。9 個原生 macOS 程式、非零偏移整頁比對與隔離子程序 SIGBUS 驗證通過。36 項公共介面/報告無跳過，Python 五種組合含 `file-mapping`、66 項驗收腳本和 38 項來源回歸通過，計數重疊。證據：`build-hvf-arm64/darwin-mmap-verified-evidence/`。尚無新增 Intel HVF/KVM/WHP 或 iOS 真機證據；Intel HVF Actions 繼續暫停。

## 明確的檔案中繼資料

檔案項目可提供 `metadata`，下例全部欄位均必填。十進位字串保留完整整數精度，JSON 數字限於 ±(2^53−1) 內的精確整數。device 為有號 32 位，mode/link_count 為無號 16 位，inode 為無號 64 位，uid/gid/flags/generation 為無號 32 位。size 必須等於檔案位元組數；blocks 不超過有號 64 位上限，block_size 為非負有號 32 位。四個時間使用有號 64 位秒及 0–999999999 奈秒，mode 必須符合檔案或目錄類型。

`stat64` (338)、`fstat64` (339)、`lstat64` (340) 在 ARM64/x64 回傳相同的 144 位元組 LP64 記錄。路徑解析與 open 共用；FD 查詢遵循複製及關閉，不配置描述元也不改變游標。rdev、填補和保留欄位清零。輸入提供初始中繼資料，可選修改策略決定後續變更；讀取不推進時間，mode 不改變存取授權。缺少中繼資料、串流狀態、舊版 stat及擴充安全查詢仍不支援。路徑/FD 錯誤先於目的位址檢查；部分可寫輸出在寫入前停止。原生測試逐位元組對比真實檔案狀態並核對 SDK 配置，同一自編程式驗證三種呼叫。

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839","metadata":{
  "device":1,"inode":"18364758544493064720","mode":33188,"link_count":1,
  "uid":1000,"gid":1000,"size":10,"block_size":4096,"blocks":8,
  "flags":0,"generation":0,
  "access_time":{"seconds":-1,"nanoseconds":1},
  "modification_time":{"seconds":2,"nanoseconds":3},
  "change_time":{"seconds":4,"nanoseconds":5},
  "birth_time":{"seconds":6,"nanoseconds":7}
}}]}}
```

[XNU stat.h](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/sys/stat.h)

### 中繼資料驗證與後續工作（2026-10-05）

stat64 的 Release 驗收共 409 個唯一項目：193 通過、216 跳過、零失敗；54/54 個 ARM64 HVF 必需項目執行，Unicorn 涵蓋五種客體組合。SDK 配置及真實檔案完整記錄比對、8 個原生程式、36 個公共介面/報告測試（無跳過）、Python 五種組合和 66 個驗收腳本測試均通過，計數有重疊。原生測試每例使用獨立輸出檔案，修復短輸出殘留舊尾端位元組的問題。新增功能仍無 Intel HVF/KVM/WHP 或 iOS 真機證據。

後續依序補共享映射與 EOF 缺頁、有界寫入（驗證 EOF 頁、close 後映射壽命與錯誤順序），顯式時間/系統資訊、必要 Mach/執行緒服務、Mach-O 相依性與重定位/繫結、初始化/TLS，再以原生程式推進 Objective-C/Swift 與 Foundation/UIKit。iOS 真機比對需要 SDK 與設備；Intel HVF 尚未驗證，Actions 繼續暫停。



```sh
cmake --build build-hvf --target NeverDDarwinProcessTests NeverDProcessPublicTests --parallel 8
ctest --test-dir build-hvf -L '^(NeverDDarwinProcessTests|NeverDProcessPublicTests)$' --output-on-failure
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/native-evidence --require-hvf
```

獨立工作負載驗收要求 ARM64 111 項或 x64 74 項全部執行，包含每個平台的 `LC_MAIN` 與 `LC_UNIXTHREAD`。必需項缺失、跳過或缺少 `ld64.lld` 都會失敗。

```sh
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/darwin-workload-evidence --require-darwin-backend hvf
```

Linux 使用 `kvm`、Windows 使用 `whp`。[Darwin 工作流程](../../.github/workflows/darwin-native.yml) 在關閉 Unicorn 後驗證兩種 x64 後端，也能單獨重跑。[核心參考工作流程](../../.github/workflows/darwin-kernel-reference.yml) 不建置 NeverD/LLVM，直接在兩種 macOS ISA 執行原始程式。`DarwinNativeCases.def` 統一模式、退出狀態及預期位元組；只有宿主參考程式為實際 dyld 入口連結 libSystem。架構不符、Rosetta、逾時或結果差異均失敗，不能當作 iOS 裝置核心證據。

## 證據與待完成範圍

以下為 2026-10-03 記錄，重疊列不能相加：

| 後端 | 原始碼 | 通過 | 失敗 | 跳過 | 原生工作負載 |
| --- | --- | ---: | ---: | ---: | ---: |
| ARM64 HVF | `defc93928` | 65 | 0 | 221 | 39/39 |
| Intel HVF | `8dcc74c59` | 52 | 0 | 234 | 26/26 |
| x64 KVM | `36e11ca8a` | 51 | 0 | 235 | 26/26 |
| x64 WHP | `36e11ca8a` | 51 | 0 | 235 | 26/26 |

[Intel 工作](https://github.com/NeverSight/NeverD/actions/runs/37106013999) 核對 286 個 CTest 身分、32 個行程及原始 XML。234 個跳過項為 65 個停用的 Unicorn、39 個 ARM64 客體、130 個其他宿主平台。產物 `11267489438` 的 SHA-256 已驗證為 `cd8fabbd7d031ac4ad7b891b8e5a52f3e3abe3c39306d9c4a1893e40912e78ef`。[KVM/WHP](https://github.com/NeverSight/NeverD/actions/runs/37062839703) 也獨立核驗。[核心對照](https://github.com/NeverSight/NeverD/actions/runs/37064795867) 在兩種 ISA 各通過 4/4 個程式，退出狀態 37、輸出逐位元組一致、stderr 為空。

開啟 Unicorn 的 C API/CLI 檢查為 138 通過、156 跳過、零失敗。Python 涵蓋五種組合；封裝引擎與 18 份 ARM64 CLI 報告相符，186 個 Mach-O 簽章通過。同時關閉 HVF/Unicorn 時，38 項通過、231 項跳過，且不連結 Hypervisor.framework。這些是整合證據，不能增加原生執行計數。Intel 完整 CPU 仍未驗收；詳見 [HVF](macos-hvf.md)及[完整記錄](../darwin-emulation.md#hosted-native-verification-2026-10-03)。

## 明確的時間觀察值

`ProcessOptions::DarwinTime` / `darwin_time` 為所有 Darwin profile 提供 raw `gettimeofday` (116) 的固定觀察值，包含第三個 `mach_absolute_time` 輸出。`time_of_day`、`timezone` 和 `mach_absolute_time` 都可省略；省略代表未知，明確的零是有效值，空物件不會補上預設時鐘。模型不讀取主機時鐘、不推斷時區、不推進時間，也不換算絕對 tick。

提供記錄時必須包含全部成員。`seconds` 是無號 32 位，`microseconds` 為 [0, 999999]，`minutes_west` / `dst_time` 為有號 32 位，絕對 tick 為無號 64 位。JSON 沿用無損整數規則：超出安全整數範圍時使用十進位字串。未知欄位、越界值及非 Darwin profile 在載入映像前拒絕。

LP64 `timeval` 為 16 位元組：偏移 0 是零擴展秒數，偏移 8 是 32 位微秒，偏移 12 的四位元組填充為零。時區是兩個有號 32 位欄位，tick 佔八位元組。日曆與絕對時間先聯合取樣，因此請求的兩種觀察值必須在任何複製或指標檢查前存在。接著依序寫入 timeval、timezone、absolute ticks。時區缺失在自身階段停止，保留先前的 timeval；後續 EFAULT 也保留先前寫入，重疊位址依相同順序處理。單次輸出僅部分可寫時，在該次複製前明確停止，保留更早的複製。三個空指標無須設定即可成功；選擇性查詢只要求所請求的值。

原創 `time` 工作負載核對原生行為；來賓 `time-values` 在五種來賓組合的 C/CLI/Python 路徑輸出設定的精確 32 位元組。獨立 SDK 對照從一次原生 raw 呼叫擷取三輸出，再逐位元組比較。此功能不包含時鐘推進、tick 換算、commpage 計數器、計時器或 Mach 時鐘物件/IPC；dyld、執行緒、Objective-C/Swift 和 Foundation/UIKit 仍待完善。Intel HVF Actions 維持暫停，本輪不增加原生 Intel 或實體 iOS 驗收結論。

```json
{"darwin_time":{"time_of_day":{"seconds":4045620583,"microseconds":654321},"timezone":{"minutes_west":-480,"dst_time":-1},"mach_absolute_time":"18364758544493064720"}}
```

[XNU gettimeofday](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_time.c), [time ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/time.h).

時間驗證（2026-10-06，Release）：538 項 Darwin 註冊測試，274 項通過、264 項因後端不可用跳過，零失敗；66/66 項必要 ARM64 HVF 測試全部執行。12 個原創原生 macOS 工作負載及單次取樣的 SDK 位元組對照通過。公共 C/CLI/報告為 43/43，無跳過。Python 五種來賓組合通過，包含精確時間位元組與原有八種檔案情境。66 項執行器測試、本地化、能力清單與格式檢查通過。計數重疊。證據：`build-hvf-arm64/darwin-time-verified-evidence/`、`darwin-time-native-first/`、`darwin-time-public.xml`。

## Mach 時間與返回約定

`darwin_time.timebase` 提供非零無符號 32 位 `numerator` 和 `denominator`；比例原樣保留，不約分或換算。`mach_timebase_info_trap` 索引 89 對應 ARM64 X16=-89 或 x64 RAX=0x01000059，寫入小端分子、分母共八位元組並返回零。完全無效的輸出位址仍返回零；部分可寫輸出在複製前停止，底層傳輸錯誤繼續傳播。缺少 timebase 時在指標檢查前停止，包含空指標。

ARM64 X16=-3、X16=-4 返回完整無符號 64 位 `mach_absolute_time`、`mach_continuous_time`。各自僅需對應觀測值，明確的零有效。x64 對應原生表項會觸發 EXC_SYSCALL，模型明確拒絕。時鐘推進、commpage、計時器及 Mach 時鐘物件/IPC 尚未實作。

分派僅用編號低 32 位，報告保留原始 64 位。ARM64 負數選擇 Mach；x64 Mach 類別為 0x01000000，BSD 為 0x02000000。BSD 3/4 仍為 read/write；未知編號及外來類別停止。綁定統一決定返回約定：Mach 保留旗標與 X1/RDX，BSD 保留既有 carry 規則；x64 仍更新 RCX/R11。Mach 記錄含 `result` 並省略 `error`，即使輸入 carry 已設定。

原創 `mach-time` 對照 ARM64 原生旗標、次結果、高位編號、無效指標與 BSD 切換。`mach-timebase-values` 在五種來賓輸出精確位元組，`mach-clock-values` 在 ARM64 驗證完整 tick；SDK 另驗證配置與原生比例。Intel HVF Actions 維持暫停，x64 軟體與語法檢查不構成原生 Intel 或實體 iOS 驗收。

```json
{"darwin_time":{"timebase":{"numerator":125,"denominator":3},"mach_absolute_time":"18364758544493064720","mach_continuous_time":"18446744073709551615"}}
```

[XNU clock traps](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/kern/clock.c), [ARM64 entry](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/bsd_arm64.c), [ARM64 special traps](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/sleh.c), [x64 entry](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/x86_64/idt64.s).

Mach 驗證（2026-10-06，Release）：Darwin 569 項中 293 通過、276 項後端不可用而跳過、零失敗；ARM64 HVF 必需 69/69 全部執行。最終通過 13 個原生工作負載及兩個時間 SDK 對照。公開 C/CLI/report 為 100/100、無跳過；Python 覆蓋五種來賓。公開比較依平台與場景獨立執行，明確給予 10 秒來賓預算，產品預設與超時回歸不變。計數重疊。

首次原生啟動曾超過既有 5 秒門限，測得 6.056 秒，復用後為 0.010 秒；同一二進位檔隨後在原門限通過 13 項，保留失敗記錄。宿主負載下的超時經獨立串行複核通過。證據：`build-hvf-arm64/darwin-mach-time-final-evidence/`、`darwin-mach-time-native-recheck/existing-binary-recheck.json`、`darwin-mach-time-public-accepted.xml`，描述提交前工作樹。當時 ARM64 MRS/MSR NZCV 尚未納入受檢查 CPU 契約，因此測試以整數指令觀察旗標。下述增量已補上這項 CPU 缺口。

## ARM64 條件旗標暫存器

共用的受檢查 ARM64 契約在 EL0、EL1 接受精確的 `MRS Xt, NZCV` 與 `MSR NZCV, Xt` 編碼。讀取只回傳第 31–28 位元；寫入只取輸入的這四位，其餘忽略。讀至 `XZR` 會丟棄結果；由 `XZR` 寫入會清除四個旗標，不會讀取 SP。各後端執行原始指令。宿主暫存器設定介面的驗證、FPCR/FPSR 的限制策略不變；鄰近且未列出的系統暫存器仍明確不支援。

`NeverDAArch64NZCVTests` 將全部旗標組合與宿主原始指令對照，並檢查完整純量/向量狀態、記憶體、暫存器邊界、觀察器停止/失敗、還原上下文重試及共用指令預算。ARM64 `mach-time` 現在以真實 MSR/MRS 包圍 SVC，涵蓋 Mach 旗標保持及返回 BSD 的切換。原生 HVF 必要項包含兩種權限下的六個方法與宿主對照。ARM64 KVM/WHP、實體 iOS 尚未驗證。可寫檔案、系統資訊、推進時鐘、Mach IPC/執行緒、dyld/執行環境/框架及裝置驗收仍待完成。

[Arm NZCV (DDI0601, 2025-06)](https://developer.arm.com/documentation/ddi0601/2025-06/AArch64-Registers/NZCV--Condition-Flags).


可寫檔案驗證（2026-10-06）：Release Darwin 610 項中 322 通過、288 後端不可用而跳過、零失敗；ARM64 HVF 必需項 72/72 執行。最終專項 114 項中 102 通過、12 跳過，包含新增 EFAULT 中繼資料斷言；15 個原生程式與 111 個公共 C/CLI/報告測試全數通過。計數重疊。首次原生用例發現 FWASWRITTEN 遺漏，修復後通過，失敗證據保留。時限未變；完整 GitHub CI 與 iOS 實機仍須另行驗收，Intel Actions 暫停。

`build-hvf-arm64/writable-darwin-evidence/` · `writable-native-final/` · `writable-focused-final.xml` · `writable-public.xml`

Python 首次整組測試的三個 ARM64 目錄案例超時。不改參數的診斷中，可寫案例 10/10 通過，但一個 iOS 目錄呼叫牆鐘 5.005 秒、CPU 1.263 秒後超時。同一 5 秒限制下單獨複驗三者全通過（2.43–3.17 秒、10,941 條指令、輸出 65）。16 邏輯核負載為 54–70，支持排程壓力解釋，不保證延遲穩定；保留原失敗。

最終未改參數的 Python 整組測試涵蓋五種 profile/ISA 並通過，耗時41.118秒；每行程仍限5秒，先前失敗及診斷獨立保留。


中繼資料策略驗證（2026-10-06）：Release專項148項為124通過/24跳過。完整Darwin645項為343通過/300跳過/2項既有ARM64 HVF目錄逾時；相同20項與原5秒時限複測為8通過/12跳過，受影響項3.818/3.949秒。75項必需HVF都有通過觀察，首次整輪失敗仍保留。公共C/CLI/報告117/117（Darwin73項）、Python五組27.359秒、原生15/15、runner66/66通過。分配策略非APFS證據；未改時限，完整CI、Intel、iOS實機和完整環境仍待驗收。

`build-hvf-arm64/mutation-metadata-validation-summary.json`; `mutation-metadata-darwin-evidence/`; `mutation-metadata-directory-recheck/`; `mutation-metadata-focused.xml`; `mutation-metadata-public.xml`.

稀疏定位驗證（2026-10-06）：Release Darwin 共 671 項，359 通過、312 項後端不可用而跳過、零失敗；78 項必需 ARM64 HVF 全部執行，Unicorn 覆蓋五種來賓。專項 147 項為 123 通過、24 跳過；16 個原生程式、122 項公共 C/CLI/報告（含 78 項 Darwin 比較）、Python 五組（12.344 秒）和 66 項 runner 全部通過。計數重疊，未改時限，歷史失敗保留。證據：`build-hvf-arm64/sparse-seek-validation-summary.json`。分配幾何屬於明確的虛擬策略，不能視為 APFS 等價；完整 CI 和 iOS 實機仍需驗收，Intel HVF Actions 保持暫停。

刪除驗證（2026-10-06）：Release Darwin 708 項為 384 通過、324 項後端不可用跳過、零失敗，81 項必需 ARM64 HVF 全部執行。專項 156 項為 137 通過/19 跳過；原生 17/17、公共 C/CLI/報告 128/128（83 項 Darwin 比較）、Python 五組 16.268 秒、runner 66/66 皆通過。獨立設計與實作審查無剩餘阻塞；計數重疊、時限未改，無須重試。證據：`build-hvf-arm64/unlink-validation-summary.json`。目錄失效與固定策略時間屬模型規則，完整檔案系統/執行階段及 iOS 實機仍待驗收；Intel HVF Actions 維持暫停，完整 GitHub CI 另行驗收。

### 建立驗證，2026-10-06

Release Darwin：748 項，412 通過、336 項後端不可用跳過、零失敗；84 項 ARM64 HVF 必需項全執行。專項 162：150 通過/12 跳過；C/CLI/報告 133/133（Darwin 88）、Python 五組 9.982 秒、原生 18/18、runner 66/66 通過。初輪 ARM64 正確拒絕測試指標表的重定位，改為內聯位元組後通過，未放寬載入規則；原失敗和二進位檔保留。runner 清單預期由 27 更新為 28。獨立審查無剩餘阻塞，包含父目錄觀測的容量失敗測試。計數重疊、時限未改；完整 CI 與 iOS 實機另驗，Intel HVF Actions 暫停。

`build-hvf-arm64/create-validation-summary.json`, `create-darwin-evidence/`, `create-focused.xml`, `create-public.xml`, `create-native-final/`, `create-initial-evidence/`.

### 建立中繼資料驗證，2026-10-06

Release Darwin 共 787 項：439 通過、348 項後端不可用跳過、零失敗，87 項必需 ARM64 HVF 全部執行。專項 151 項中 139 通過、12 跳過。公共 C/CLI/報告 145/145 通過，含 98 項 Darwin 輸入比較；未改動的 Python 方法在 12.211 秒內通過五組設定。原生程式 19/19、驗收腳本 66/66 通過。獨立審查無剩餘阻塞，新增跨父目錄裝置/群組與全域 inode、首次寫入前刪除、FD 滿且輸入不可用時修改 umask 的測試。計數重疊、時限未改，無須失敗重試。固定建立/修改時間與配置仍是明確虛擬策略；完整 GitHub CI 與 iOS 實機另行驗收，Intel HVF Actions 維持暫停。

`build-hvf-arm64/creation-metadata-validation-summary.json`, `creation-metadata-darwin-evidence/`, `creation-metadata-focused.xml`, `creation-metadata-public.xml`, `creation-metadata-native/`.

### 改名驗證，2026-10-06

Release Darwin 共 835 項：474 通過、360 項後端不可用跳過，既有 macOS ARM64 HVF 虛擬中繼資料案例逾時一次（5.087 秒）。原參數及 5 秒時限複查：8 通過、12 跳過，受影響項為 0.113 秒。兩次合計覆蓋全部 90 個必需 ARM64 HVF 身分；完整門檻仍記錄失敗。改名專項 42/54 通過、12 跳過；C/CLI/報告 150/150，含 103 項 Darwin 比較。未改動 Python 方法五組設定於 18.478 秒通過；原生 20/20、腳本 66/66。獨立審查發現巢狀點路徑分類錯誤，4K/16K 回歸先失敗後修正通過；較早的唯讀 ftruncate 測試預期改為 EINVAL。保留所有失敗與探針版本，計數重疊、時限不變。完整 GitHub CI 與 iOS 實機另行驗收，Intel HVF Actions 維持暫停。

`build-hvf-arm64/rename-validation-summary.json`, `rename-focused-final.xml`, `rename-darwin-final-evidence/`, `rename-metadata-recheck.xml`, `rename-public.xml`, `rename-native-final/`, `rename-review-initial-evidence/`.

## 明確的系統觀察值

`ProcessOptions::DarwinSystem` / `darwin_system` 為所有 Darwin 設定的 `sysctl(202)` 與原始 `sysctlbyname(274)` 提供固定觀察值。各欄位皆可省略；缺少的值或未列出的鍵明確停止為不支援。不查詢主機、不推測版本或機型。嚴格 JSON 與 C++ 驗證在載入映像前拒絕非法值與非 Darwin 設定。

`os_revision` 為有符號 32 位；`cpu_count` 為 1..INT32_MAX；`memory_size` 保留無符號 64 位；`max_files_per_process` 為 0..INT32_MAX，採四位元組 int 編碼。其餘純量欄位是最多 1023 位元組（`hostname` 限 255 位元組）且無內嵌 NUL 的字串，允許明確空字串，結果包含結尾 NUL。觀察值不改變排程、配置或描述符預算。

| JSON 欄位 | sysctl 名稱 | MIB |
| --- | --- | --- |
| `os_type` | `kern.ostype` | `1,1` |
| `os_release` | `kern.osrelease` | `1,2` |
| `os_revision` | `kern.osrevision` | `1,3` |
| `kernel_version` | `kern.version` | `1,4` |
| `os_version` | `kern.osversion` | `1,65` |
| `machine` | `hw.machine` | `6,1` |
| `model` | `hw.model` | `6,2` |
| `cpu_count` | `hw.ncpu` | `6,3` |
| `memory_size` | `hw.memsize` | `6,24` |
| `max_files_per_process` | `kern.maxfilesperproc` | `1,29` |
| `hostname` | `kern.hostname` | `1,10` |

`hw.pagesize` 取自既有客體記憶體策略，通常為8位元組；非空輸出且容量恰為4時為4位元組。舊 MIB `[6,7]` 與 `hw.pagesize_compat` 固定回傳4位元組。`hw.pagesize` 的動態數字 OID 仍不支援。`hw.memsize` 在容量恰為4時，僅當64位模式是帶符號32位值的符號擴展才縮窄，否則 ERANGE34 保持輸出與長度不變。

MIB 數量取低32位且須為2–12；名稱長度取完整64位且須小於1024。先檢查全部指定的位元組，再依首個 NUL 解讀並移除一個末尾點；空名稱回傳 ENOENT，部分可讀輸入不支援。非空 `oldlenp` 在副作用前須完整具備8位元組讀寫權限；原生錯誤長度指標探測未在期限內返回，因此明確不支援。空 `oldlenp` 表示容量0；空 `oldp` 僅查長度。除 `kern.hostname` 外，短緩衝區回傳 ENOMEM12、不寫資料並將長度設0；資料 EFAULT 保持原長度。先擷取輸入與容量，再寫資料、最後寫長度，保留別名順序與後續傳輸失敗前完成的複製。

`hostname` 宣告此 guest 呼叫者可見的位元組，不查詢宿主、不替行動環境補上 `localhost`，也不推斷 entitlement。省略為未知，明確空字串回傳一個 NUL。`kern.hostname` 的非空輸出容量若為正且不足，會成功回傳恰好該容量的位元組，以 NUL 結尾並報告該容量。零容量仍回傳 ENOMEM12、長度0且不寫資料；空輸出指標報告包含 NUL 的完整長度。只檢查實際輸出範圍；部分可寫範圍仍不支援且不發布前綴，原生部分複製行為不在模型內。這只增加 libc uname/gethostname 使用的原始觀察值，不實作其 dylib 匯入或完整執行環境。

newp/newlen 均非零才是寫入請求。先保留名稱/MIB 與 oldlenp 完整讀寫預檢，再由預設或明確非 root EUID 在觀察值和資料輸出前回傳 EPERM1。EUID0 的 kern.osversion / kern.maxfilesperproc / kern.hostname 特權寫入明確停止 unsupported；RUID 不決定此分支。其他原生唯讀節點即使 root 仍 EPERM1。新長度0忽略指標；未知鍵、樹和動態 OID 不猜 ENOENT。

原創 `system-info` 檢查原生 macOS 與客體 ABI；`virtual-system` 透過 C++、C/CLI、Python 比對明確設定的位元組。獨立 SDK 對照將主機九項觀察值作為明確測試輸入，比較名稱與數字輸出；不代表 iOS 實機或 Intel HVF 驗收。

```json
{"darwin_system":{"hostname":"guest-node","os_type":"Darwin","os_release":"24.test","os_revision":0,"kernel_version":"Virtual kernel","os_version":"V42","machine":"virtual64","model":"VirtualModel","cpu_count":4,"memory_size":"17179869184"}}
```

[XNU sysctl](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_newsysctl.c), [XNU hardware MIB](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mib.c), [Apple sysctl(3)](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man3/sysctl.3.html).

### 系統查詢驗證，2026-10-06

Release Darwin 共881項：509通過、372因後端不可用跳過、零失敗；93項 ARM64 HVF 必需項全數執行。聚焦49項中37通過、12跳過。公開 C/CLI/報告163/163通過，含113項 Darwin 輸入比對。未更改的 Python 方法在15.302秒內通過五種設定；原生21/21、驗證腳本66/66通過。獨立審查無阻塞，補充錯誤優先序與 SDK 捕獲對照皆通過。新增對照曾因缺 StringExtras 標頭而編譯失敗，補上後通過，原始日誌與程式碼已保留。原生錯誤長度指標探測已保留且明確不在支援範圍。測試後只整理兩個檔頭註解，並成功重建。計數重疊、期限不變，不需執行失敗重測。完整 GitHub CI、iOS 實機另行驗證；Intel HVF Actions 維持暫停。

`build-hvf-arm64/sysctl-validation-summary.json`, `sysctl-darwin-final-evidence/`, `sysctl-focused-final.xml`, `sysctl-public.xml`, `sysctl-native-final/`, `sysctl-sdk-build-failure/`, `sysctl-initial-probe-evidence/`.

## 向量檔案與輸出擷取 I/O

`readv`/`writev`、`preadv`/`pwritev` 及 nocancel 入口共用純量檔案與輸出擷取實作，不增加設定或主機存取。LP64 iovec 包含8位元組位址和8位元組長度；iovcnt 的有號低32位元須為1–1024。查詢描述符前複製完整陣列，輸出別名不能改寫要求；部分可讀陣列仍明確不支援。

先檢查描述符權限與串流定位能力，再驗證長度。每項及總長須在 INT64_MAX 內；一般檔案和目錄另限總長 INT_MAX。串流不套用 vnode 限制：有限 stdin 依可用位元組截短，擷取遵守輸出預算。pwritev 的所有負偏移在陣列讀取前拒絕；preadv 在描述符與長度後檢查偏移。零長度項忽略位址，但保留描述符、目錄及偏移檢查；EOF 後不碰多餘項。定位呼叫保留游標，pwritev 忽略追加旗標；一般追加先按原游標一次裁剪總要求，再選 EOF。

依向量順序複製。後續完全無效的項回傳 EFAULT，保留先前位元組、一般游標進度及實際寫入非零位元組時的 FWASWRITTEN。已獲准的非零檔案寫入因資料緩衝區 EFAULT 返回時，使完整中繼資料失效，即使有虛擬成功策略；參數錯誤、模型准入拒絕及後端失敗保留中繼資料。單項讀取目的地部分可寫時，回傳 UnsupportedService，不複製當前項，保留早先複製；單項檔案寫入來源部分可讀時，在任何檔案副作用前拒絕。授權、映射租約和總儲存預算先檢查；後端預檢或讀取失敗不發布檔案或擷取位元組。

向量擷取先檢查 stdout/stderr 共用預算。跨越使用者位址上限的項不產生位元組，先前項仍保留；其他部分可讀項保留已檢查前綴並回傳 EFAULT。純量範圍錯誤仍優先於預算。複製或重新導向描述符保留原輸出流。原創 `vectored-io` 在原生 macOS、五種客體及公開 C/CLI/Python 驗證全部8個入口；不增加取消、管線、執行緒或 iOS 實機驗收。

`readv`: 120/411; `writev`: 121/412; `preadv`: 540/542; `pwritev`: 541/543.

[XNU vector calls](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/sys_generic.c), [XNU iovec lengths](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_subr.c), [XNU vnode I/O](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c).

### 向量 I/O 驗證，2026-10-06

Release Darwin 共937項：553通過、384因後端不可用跳過、零失敗；96項必需 ARM64 HVF 全部執行。專項57項中45通過、12跳過。公開 C/CLI/報告168/168通過，含118項 Darwin 輸入對照；Python 在20.397秒內涵蓋五種設定。原生工作負載22/22、驗證腳本66/66通過。獨立審查補充稀疏定位寫入故障測試，驗證游標、實際 EOF、中繼資料拒絕與精確剩餘儲存容量。初次建置因舊測試仍呼叫已移除的內部查詢而失敗，現改驗證實際擷取輸出；新增事件斷言誤用 optional<bool> 曾使8項已成功的客體執行報失敗，修正後相關檢查全數通過。兩次失敗均保留原始碼與日誌。計數重疊、時限不變；完整 GitHub CI 與 iOS 實機另驗，Intel HVF Actions 保持暫停。

`build-hvf-arm64/vector-validation-summary.json`, `vector-darwin-final-evidence/`, `vector-focused-final.xml`, `vector-public.xml`, `vector-native-initial/`, `vector-initial-build-failure/`, `vector-initial-assertion-evidence/`.

## 檔案存在性查詢

`access(33)` 與 `faccessat(466)` 查詢目前虛擬目錄，不分配描述符、不改變檔案位元組、游標、旗標或中繼資料。F_OK 依既有目錄遍歷契約確認名稱存在。中繼資料不授予或撤銷目錄存取權；這些呼叫不驗證原生祖先搜尋權限、ACL 或 MAC。缺少或失效的 stat 觀測值不影響查詢。即使舊 FD 或映射仍持有物件，已刪除名稱仍回傳 ENOENT；建立、名稱重用及重新命名都查詢目前命名空間。

模式取低32位元。R/W/X 使用位元0–2，延伸權限使用位元9–21；`(mode & 0x003ffe07) == 0` 時是存在性查詢，其餘位元（含符號位元）依原生規則忽略，不誤報 EINVAL。權限要求在成功查找後明確回傳 UnsupportedService，即使中繼資料或修改授權看似允許；已知路徑及描述符錯誤先回傳，不猜測權限。

Faccessat 接受 AT_EACCESS(0x10)、AT_SYMLINK_NOFOLLOW(0x20)、AT_SYMLINK_NOFOLLOW_ANY(0x800) 低位旗標的任意組合。其他旗標在路徑或 FD 存取前回傳 EINVAL，即使目錄未設定。真實/有效身分固定；固定連結採用下文解析策略。絕對路徑忽略 dirfd；相對路徑沿用 CWD/目錄 FD 規則。非 AT_FDCWD 的 nameiat 先讀首位元組、檢查相對目錄 FD，再匯入完整字串；`/` 跳過 FD。首位元組故障為 EFAULT14；相對壞/檔案 FD 的 EBADF9/ENOTDIR20 先於後續故障。相對空路徑仍檢查 FD：未知 FD 為 EBADF、一般檔案為 ENOTDIR，其餘為 ENOENT。目錄未設定或串流目錄身分未知仍不支援。

獨立 `file-access` 在原生 macOS、五種客體與 C++、C/CLI/Python 比較兩個呼叫、忽略位元、旗標組合和查找順序。原生 NOFOLLOW_ANY 使用相對目錄 FD，避免主機 `/tmp`、`/var` 符號連結干擾。直接測試涵蓋命名空間變更、混合權限位元、描述符耗盡、中繼資料獨立性及客體記憶體失敗。

[XNU access and faccessat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU access mode bits](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/unistd.h).

### 檔案存在性驗證，2026-10-06

Release Darwin 共971項：575通過、396因後端不可用跳過、零失敗；99項必需 ARM64 HVF 全部執行。專項35項中23通過、12跳過，含全部14項直接測試。公開 C/CLI/報告173/173通過，含123項 Darwin 輸入對照。Python 在16.235秒內通過五種設定；原生工作負載23/23、驗證腳本66/66通過。獨立設計與實作審查未發現阻塞。原生探針保留主機 /tmp 符號連結造成的初始 NOFOLLOW_ANY 結果與後續正規路徑對照；共用工作負載使用相對目錄 FD。計數重疊、時限不變，無須執行失敗重測；完整 GitHub CI 與 iOS 實機另驗，Intel HVF Actions 保持暫停。

`build-hvf-arm64/access-validation-summary.json`, `access-darwin-final-evidence/`, `access-focused-final.xml`, `access-public.xml`, `access-native-initial/`, `access-evidence/`.

## 建立與刪除目錄

`mkdir(136)`、`mkdirat(475)` 在明確可修改的直接父目錄建立目錄。新目錄繼承命名空間修改授權；初始目錄保留各自授權。僅繼承父目錄已知裝置與群組身分，不產生完整 stat、大小、配置、時間或列舉游標；重用舊檔名時遮蔽所有舊觀測。`creation_policy` 僅套用一般檔案：巢狀檔案使用繼承的父身分與既有全域 inode 序列，mkdir 不消耗檔案 inode。權限執行與原生目錄中繼資料不在本契約內。

共用逐元件解析器。mkdir 可建立尾端只接斜線的缺失名稱；缺失祖先後接點/雙點仍為 ENOENT，一般檔案祖先為 ENOTDIR，既有名稱為 EEXIST。相對路徑沿用 FD/CWD，絕對路徑忽略 dirfd；字串錯誤先於相對 FD。建立不需可用 FD；查找、授權、位元組/項目預算或傳輸失敗不發布名稱、不使父觀測失效。

`rmdir(137)` 與帶 AT_REMOVEDIR(0x80) 的 `unlinkat(472)` 刪除本程序建立的空目錄，可加 AT_SYMLINK_NOFOLLOW_ANY(0x800)。未知低32位旗標先回 EINVAL；DATALESS、SYSTEM_DISCARDED 未支援。保留已知路徑/類型/根目錄錯誤；沒有 removable 授權的初始目錄刪除仍為 UnsupportedService。已准入目錄尾點為 EINVAL，從仍有名稱的目錄出發的雙點或非空目標為 ENOTEMPTY。目錄 FD（含 dup）或 CWD 保留原目錄物件，不再阻止刪除。已 unlink 的一般檔案 FD/映射不算名稱；原生對照確認位元組、inode 與最後連結 F_GETPATH 在父目錄刪除及名稱重用後保留。

每個新目錄以規範路徑加 NUL 和一項計入共享16 MiB/256項預算。已刪目錄不可達後僅退回自身費用，保留孤立檔案及映射租約。成功修改使直接父目錄完整 stat/列舉失效，失敗則保留；新目錄中繼資料及快照即使有一般檔案建立策略仍未知。原創 `directory-mutations` 在原生 macOS、五種客體與 C++/C/CLI/Python 比較巢狀建立、改名、unlink、刪除及孤立物件重用。 檔案描述或映射租約保留的孤立檔案也保留父目錄物件；目錄 FD 全關閉後，父鏈目前路徑/NUL 和項目仍計費，直到先回收檔案、再逐層回收目錄。移動仍連結的建立祖先會更新舊物件的 F_GETPATH；同名替代物件不會接管它們。

[XNU mkdir/rmdir](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU directory creation lookup](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_lookup.c).

### 目錄修改驗證，2026-10-06

Release Darwin 共1,017項：609通過、408因後端不可用略過、零失敗；102項必需 ARM64 HVF 全部執行。定向測試58通過、12略過，含26項新增4K/16K直接測試。公開 C/CLI/報告178/178，含128項 Darwin 輸入對照；Python 在17.255秒內通過五種組合，原生24/24、驗證腳本66/66通過。獨立審查核對預算、名稱重用、父身分、檔案租約與回滾。初始測試通過後，額外原生探針發現純斜線根刪除為 EISDIR、尾點/雙點根為 EBUSY；已統一判斷並加入共用斷言，初始測試與原始碼/二進位快照保留。計數重疊、時限不變；Intel HVF Actions 暫停，完整 GitHub CI 與 iOS 實機另驗。

`build-hvf-arm64/directory-mutation-validation-summary.json`, `directory-mutation-darwin-final-evidence/`, `directory-mutation-focused-final.xml`, `directory-mutation-public-final.xml`, `directory-mutation-native-final/`, `directory-mutation-before-root-fix/`.

## 保留的目錄身分

刪除後的目錄由 FD/CWD 保留原父鏈，父名稱刪除與重用不改變身分。開啟點路徑有獨立游標，dup 共用；雙點沿原父物件，已刪目錄的一般子名稱回 ENOENT，不會接到替代目錄。LOOKUP 可穿越保留的已刪父物件；建立/刪除/改名查找回 ENOENT。改名目標尾點/雙點於該元件遍歷前回 EINVAL，更早祖先錯誤優先。F_GETPATH 保留末次路徑，完整 stat/列舉未知。新目錄的路徑+NUL與一項費用保留至所有 FD、CWD、舊子物件參照釋放；close、dup2、CWD 變更與修改准入回收不可達鏈。初始費用與一般檔案租約分開。原創 `deleted-directories` 在原生 macOS 與五種客體比較持有刪除、父鏈/名稱重用、查找意圖與僅 CWD 保留。 檔案描述或映射租約保留的孤立檔案也保留父目錄物件；目錄 FD 全關閉後，父鏈目前路徑/NUL 和項目仍計費，直到先回收檔案、再逐層回收目錄。移動仍連結的建立祖先會更新舊物件的 F_GETPATH；同名替代物件不會接管它們。

### 目錄生命週期驗證，2026-10-06

Release Darwin 1,051項：631通過、420後端不可用略過、零失敗；105項必需 ARM64 HVF 全執行。定向98通過、12略過；初始直接64/64，含14項新增及持有刪除更新。公開 C/CLI/報告183/183，含133項 Darwin；Python 五種設定18.691秒，原生25/25、腳本66/66。額外原生改名探針修正初始通過後發現的尾點錯誤順序，原始碼/結果/快照保留。主代理已核對源碼與證據，最終獨立審查不可用。計數重疊、時限不變；完整 CI/iOS實機另驗，Intel HVF Actions 暫停。

`build-hvf-arm64/directory-lifetime-validation-summary.json`, `directory-lifetime-darwin-final-evidence/`, `directory-lifetime-focused-final.xml`, `directory-lifetime-public.xml`, `directory-lifetime-native/`, `directory-lifetime-before-rename-fix/`.

## 明確准入的初始目錄刪除

目錄項可使用嚴格布林值 `"removable": true`，C++ 對應 `DarwinFileOptions::RemovableDirectories`。這聲明目標是只有一個命名空間身分的普通非掛載目錄。目標必須是明確配置的初始 `directories` 項且不是根，其直接父目錄必須明確允許修改。已知特殊模式/旗標、inode 別名（含目錄快照）或父子已知裝置號衝突均拒絕准入。相同裝置號本身不能證明沒有掛載。省略或 false 保持不支援，其他 JSON 型別無效；此選項不提供通用權限或掛載模型。

```json
{"darwin_files":{"files":[],"directories":[{"path":"/","mutable":true},{"path":"/empty","removable":true}],"working_directory":"/empty"}}
```

刪除要求目前命名空間為空；初始隱式子目錄不會因最後一個原始檔案被 unlink 而消失。成功刪除使物件及直接父目錄的完整 stat/列舉失效，舊 FD/dup/CWD 保留原物件與父鏈。已刪初始名稱不會從不可變輸入重新出現；同名新檔案或目錄有獨立身分，不繼承舊中繼資料或快照，呼叫端輸入不變。

每個 removable 參照的路徑與 NUL 計入初始 16 MiB 預算。初始目錄項、路徑、參照及快照在刪除與最後關閉後仍保留費用，也不退還 256 項限制中的初始項；新物件另計動態費用。原創 `initial-directory-removal` 持有原生測試既有空目錄時刪除它，先重建為檔案再建為目錄，檢查僅 CWD 保留並恢復空目錄；同一程式透過 C++/C/CLI/Python 覆蓋五種客體組合。

### 初始目錄刪除驗證，2026-10-06

最終 Release 原始碼核對了 1,089 項 Darwin 註冊測試：657 項通過、432 項因後端不可用略過、零失敗；108 項必要的 ARM64 HVF 測試全部執行。聚焦驗證為 27/39 通過、12 項不可用略過，新增的僅快照別名檢查也通過。公共 C/CLI/報告測試為 191/191；Python 在 76.276 秒內涵蓋五種組合；原生獨立工作負載 26/26、證據執行器測試 66/66。初次測試中不一致的 inode/快照輸入已修正，失敗記錄仍保留。

先前兩輪完整驗證分別在既有檔案/重新命名案例出現 1 次和 3 次逾時；帶診斷的驗證重現一次檔案逾時，實際耗時 5.008 秒、程序 CPU 用時 0.171 秒。相同方法及舊程式對照均通過，但延遲根因仍未確定，最終通過不代表逾時穩定性已解決。臨時診斷已移除，程式檔案雜湊已還原，客體原有 5 秒時限未變。已完成主代理原始碼/證據核查；獨立審查不可用。計數彼此重疊；完整 GitHub CI、實體 iOS 與已暫停的 Intel HVF Actions 不屬於本機驗收。

`build-hvf-arm64/initial-directory-validation-summary.json`, `initial-directory-darwin-restored-evidence/`, `initial-directory-public.xml`, `initial-directory-native-evidence/`, `initial-directory-timeout-probe/`.

## 一般檔案的獨占重新命名

完成來源與目標查找後，RENAME_EXCL 對不同的既有檔案或目錄回傳 EEXIST，先於掛載及命名空間修改檢查。更早的路徑錯誤仍優先，包括末尾點/雙點的 EINVAL。目標不存在時重用相同的有界重新命名交易，保留已開啟描述、游標、旗標、映射租約和設定的中繼資料變化。同物件獨占重新命名仍明確不支援：原生結果依賴檔案系統是否區分大小寫，精確目錄鍵無法證明此屬性。大小寫折疊、初始目錄重新命名及 SECLUDE 尚未納入。既有原創 `renamed-file` 工作負載現驗證拒絕時中繼資料不變，以及 EXCL|NOFOLLOW_ANY 成功移動，涵蓋原生 macOS 和 C++/C/CLI/Python。

驗證，2026-10-06（Release）：1,097 項 Darwin 註冊測試，665 項通過、432 項因後端不可用略過、零失敗；108 項必要 ARM64 HVF 測試全部執行。聚焦測試 44 項通過、12 項不可用略過，含八項新增直接測試。公共 C/CLI/報告 191/191；Python 五種組合耗時 19.241 秒。獨立原始呼叫探針通過 26 項檢查。首次完整原生驗證中，既有 return 案例逾時，其餘 25 項（含 renamed-file）通過。同一未修改程式的 return 三次複查耗時 0.014–0.034 秒，隨後全部 26 項原生案例在原有 5 秒時限內通過。初次失敗保留且根因未明；這些結果與先前最終 HVF 通過均不保證延遲穩定性。已完成主代理核查，獨立審查不可用。計數重疊；實體 iOS、完整 GitHub CI 與暫停的 Intel HVF Actions 不屬於本機驗收。

`build-hvf-arm64/exclusive-rename-validation-summary.json`, `exclusive-rename-darwin-evidence/`, `exclusive-rename-public.xml`, `exclusive-rename-native-evidence/`, `exclusive-rename-native-rechecked-summary.json`, `exclusive-rename-probe/`.


## 已建立目錄之間的跨父目錄改名

同一初始目錄及本程序透過 mkdir/mkdirat 建立的全部後代共享一個虛擬名稱空間域。`rename`、`renameat`、`renameatx_np` 可在這些父目錄之間移動一般檔案，不需新增 JSON 欄位。例如在可修改的初始 `/work` 內建立 `/work/left` 與 `/work/right` 後，`/work/data` 可移入任一子目錄，也可在子目錄之間移動。若 `/work/left` 是單獨提供的初始目錄，裝置號相同仍不能推定共享掛載。一般掛載拓撲依然未知。

兩個直接父目錄均需名稱空間修改授權；新建目錄繼承授權及已知 device/GID。改名保留檔案自己的身分、擁有者/群組、寫授權與配置。已知裝置衝突仍明確拒絕。實際移動使兩個父目錄的完整 stat/列舉觀察失效。已刪除初始目錄與重用路徑始終是不同物件，舊目錄 FD/CWD 不會得到替代物件的域。

既有有界替換交易、映射保留、路徑/NUL 費用及錯誤順序繼續適用。EXCL 遇到其他既存目標，仍在域與授權檢查之前回傳 EEXIST。原始 `renamed-file` 程式現在建立子目錄、移入子目錄、回到初始父目錄覆蓋檔案，再移入子目錄，透過 C++/C/CLI/Python 和原生 macOS 比對。權限強制檢查、初始目錄改名及硬連結、動態符號連結及原生 APFS 中繼資料仍未完成。

### 跨父目錄驗證，2026-10-06

最終 Release 驗收核對了 1,115 個 Darwin 註冊項：683 通過、432 因後端不可用略過、零失敗；108 個必要 ARM64 HVF 項全部執行。直接檢查 56/56 通過，包括新增 18 個 4K/16K 案例。公開 C/CLI/report 191/191 通過；Python 方法在 22.254 秒內涵蓋五種環境。原始原生程式 26/26、獨立原始系統呼叫探針 34 項、文件/能力/證據執行器腳本測試 296/296 均通過。計數有重疊。僅針對 MSVC 的 CMake 調整後，十個驗收二進位的雜湊均未改變。

先前兩次完整驗收分別保留了既有 HVF 檔案方法的三次和兩次逾時。完整方法、工作目錄與工作階段控制檢查通過，但未確立原因；最終通過不能證明延遲穩定。探針起初將 /tmp 文字路徑與 /private/tmp 標準路徑比較，改從 root FD 查詢路徑後修正四個預期。擴充原生程式最初清理誤用 mkdir(136)，改為 rmdir(137) 後修復 exit150。初始原始碼與失敗證據均保留，來賓時限仍為原來的 5 秒。

完整 Linux CI 發現的四處 LP64 測試初始化列表衝突已改為明確 uint64_t；MSVC 下的 NeverDJumpTableTests 已加入 /bigobj。實際 Linux/Windows 編譯仍等待 CI。先前完整 CI 也報告了獨立的 Windows EH 語料與關閉 PR 取消工作失敗。已完成主代理原始碼/證據自審；未宣稱獨立審查、實體 iOS 或暫停的 Intel HVF 驗收通過。

`build-hvf-arm64/cross-parent-rename-validation-summary.json`, `cross-parent-rename-darwin-synced-evidence/`, `cross-parent-rename-darwin-evidence/`, `cross-parent-rename-darwin-rechecked-evidence/`, `cross-parent-rename-public-synced.xml`, `cross-parent-rename-python-synced.log`, `cross-parent-rename-native-fixed-evidence/`, `cross-parent-rename-probe/`.

## 一般檔案名稱的原子交換

RENAME_SWAP=0x2 透過 renameatx_np 交換兩個既有一般檔案的名稱，可加 RENAME_NOFOLLOW_ANY。明確初始目錄必須同時宣告 mutable:true 和 swap_rename:true；C++ 使用 DarwinFileOptions::SwapRenameDirectories。建立後代繼承原始目錄物件的能力，刪除後重用路徑不會取得舊物件的宣告。false 或省略表示能力未知，不能僅憑裝置號相同或命名空間授權推斷支援；不同初始目錄域仍不支援。

來源與目標重用元件解析器。目標缺少時，在目錄域、授權及能力檢查前回傳 ENOENT。任一目錄運算元都明確不支援：原生可交換檔案與目錄，模型不能套用一般重新命名的 EISDIR。同物件交換取得命名空間授權後為空操作，即使未宣告交換能力也不改變狀態。RENAME_EXCL=0x4 + RENAME_SWAP=0x2、未知 flags 仍在讀取路徑前回傳 EINVAL；SECLUDE 仍不支援。

兩份檔案都保持連結，各自的身分、擁有者/群組、位元組、寫入授權、開啟描述、游標、旗標及映射租約保留。已設定虛擬政策只更新各物件自身 ctime；政策缺少或已失效時完整中繼資料仍未知。實際交換使兩邊父目錄的完整中繼資料及列舉觀察失效，不消耗建立 inode、目錄項或 FD，也不改變呼叫者輸入。

每個能力參照的路徑+NUL 占用固定初始 16 MiB 預算。交易先驗證兩邊完整動態名稱費用，再發布兩個名稱；仍連結的位元組和租約不能提供覆蓋回收額度。重複交換重用這些動態費用。原創 renamed-file 程式跨建立的子目錄交換並交換回，檢查兩份物件後繼續一般覆蓋，涵蓋原生 macOS 及所有 C++/C/CLI/Python 來賓設定。權限強制、掛載拓撲、大小寫折疊及初始目錄重新命名仍待實作。

[Apple 卷交換能力](https://developer.apple.com/documentation/foundation/urlresourcevalues/volumesupportsswaprenaming?changes=__1_2)、[XNU 重新命名旗標及查找順序](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c)。

```json
{"darwin_files":{"files":[],"directories":[{"path":"/work","mutable":true,"swap_rename":true}]}}
```

### 交換驗證，2026-10-06

Release Darwin 共 1,133 項：701 通過、432 項後端不可用略過、零失敗，108 項必要 ARM64 HVF 全部執行。重新命名直接檢查 68/68 通過，含 18 項新增選項及 4K/16K 檢查。公共 C/CLI/報告 192/192；Python 方法在 16.153 秒內涵蓋五組設定。原創原生工作負載 26/26，獨立原始呼叫探針通過 45 項。計數重疊，來賓時限未改。

最初兩項直接測試對未知寫入授權及修改後未知中繼資料使用錯誤預期；修正預期保留既有語義所有者。最初 JSON 篩選器選中零項，不計入驗收；隨後實際測試所有者與完整公共測試通過。初始來源及結果皆保留。既有 HVF/原生延遲原因仍未知，本次通過不證明穩定性。已完成主要原始碼/證據自查；不宣稱獨立審查、iOS 實機、完整 GitHub CI 或暫停中的 Intel HVF 驗收。

`build-hvf-arm64/swap-rename-validation-summary.json`, `swap-rename-darwin-evidence/`, `swap-rename-public.xml`, `swap-rename-python.log`, `swap-rename-native-evidence/`, `swap-rename-probe/`.


## 行程建立目錄的重新命名

一般 rename、renameat、renameatx_np 可在同一初始目錄物件域內移動仍連結的行程建立目錄及子樹；兩邊直接父目錄都要修改授權。不存在的目錄目標可帶尾斜線；一般檔案目標為 ENOTDIR、非空目錄為 ENOTEMPTY、移入後代為 EINVAL。獲准一般同名操作不變更狀態。EXCL 對另一既存目標先回傳 EEXIST，先於類型、循環、域與授權；目標查找錯誤先於來源點/雙點。來源點/雙點與目標為同物件時，大小寫屬性未知，明確 UnsupportedService。初始或已刪除目錄來源、初始目錄替換目標、不同初始域、連結、權限執行和 SECLUDE 仍未支援。

交易依父物件鏈選擇後代，更新具名子樹、FD 保留的已刪子目錄及 FD/映射保留的孤立檔案路徑。來源 FD、dup、CWD 與雙點仍指同物件及新父目錄。覆蓋的空建立目錄保留舊路徑與原父鏈；一般子名稱仍 ENOENT，點/雙點與 CWD 保留舊物件。再次移動新來源，不會移動路徑文字相同的舊目標孤立檔案。

後代檔案資料、身分、中繼資料、寫授權、共用或獨立游標、FD 旗標及映射租約保持。來源目錄與兩個直接父目錄的完整 stat/列舉失效，子檔案不因祖先改名而失效。建立目錄的命名空間授權繼續用於後續建立和獲准一般檔案改名；不消耗新項目、FD 或建立 inode。

發布前預配置所有具名與保留後代的新鍵、路徑，檢查每條規範路徑/NUL 的 1024 位元組界限及共享 16 MiB 預算，舊動態費用各取代一次。只有沒有 FD、CWD 或保留後代的空目標能提供回收額度，最終回收只計一次。失敗保留名稱、父物件、檔案狀態、游標及映射；僅映射保留的孤立檔案也保留已刪父鏈費用。

原創 SDK-free `renamed-directory` 經原生 macOS 與五種客體及 C++/C/CLI/Python 比對。4K/16K 檢查涵蓋物件重用、完整回復、精確容量、長後代路徑、替換額度、映射保留父鏈回收及項目/FD/inode 耗盡。


### 2026-10-06

最終 Release Darwin 共1,175項：731通過、444後端不可用略過、零失敗；111項必要 ARM64 HVF 全執行。聚焦32/44（12略過，含22項新增4K/16K），最終邊界/舊測試4/4。公開 C/CLI165/165、報告32/32無略過；Python五種21.759秒，原生27/27、探針79項。獨立審查確認修正測試程式根分隔符與同物件來源點的檔案系統屬性邊界。初次來賓exit124、兩項過時預期失敗及來源/二進位均保留，最終完整驗收通過。計數重疊、時限不變；舊 HVF/原生逾時仍未定位，不證明延遲穩定。Intel HVF Actions 暫停，實體 iOS 及完整 GitHub CI 分別驗證。

`build-hvf-arm64/directory-rename-validation-summary.json`, `directory-rename-darwin-final-evidence/`, `directory-rename-public.xml`, `directory-rename-reports.xml`, `directory-rename-python.log`, `directory-rename-native-evidence/`, `directory-rename-probe/`, `directory-rename-initial-fixture/`, `directory-rename-darwin-evidence/failed-source/`.

### 目錄與混合類型交換驗證，2026-10-07

Release Darwin 驗證核對了 1,219 項註冊：763 項通過、456 項因後端不可用跳過、零失敗；114 項必需 ARM64 HVF 案例全部執行。聚焦檢查為 32/44 通過、12 項不可用跳過，包含全部 24 項新增 4K/16K 直接案例；完整檔案元件為 317/317。原生程式為 28/28，獨立原始系統呼叫探針在本機不區分大小寫的 macOS 檔案系統上記錄了 32 項成功觀察。計數有重疊，客體程式時限未變。

獨立計畫與實作審查核對了雙向交易、初始檔案路徑計費、兩側保留子樹、僅映射保留的孤兒及準確回收。最初紅測試漏設明確根目錄交換宣告，排除於驗收；修正後的舊實作在六項案例中均因拒絕目錄交換而失敗。兩項早期混合檔案斷言錯誤地捨棄設定中繼資料，現比較僅 ctime 改變的完整紀錄。區域常數指標表曾引入 ARM64 重定位，導致六項載入拒絕；改為四項純量斷言後，修正程式沒有傳統重定位，載入器仍明確拒絕不支援的 fixups。

修正程式後的首次聚焦執行保留了三次五秒 HVF 逾時。單例與三設定控制檢查通過，隨後完整驗證通過；原因仍不明，不能據此證明延遲穩定。初始原始碼、二進位、失敗與控制紀錄均保留。初始目錄移動、獨立初始域、權限/掛載/大小寫宣告、動態相依與框架執行環境仍未完成；實體 iOS、已暫停的 Intel HVF 與完整 GitHub CI 是獨立驗收邊界。

`build-hvf-arm64/directory-swap-research/`: `darwin-final-evidence/`, `darwin-evidence/`, `focused-v5.xml`, `file-owner-v5.xml`, `native-workload-fixed-evidence/`, `native-probe-v2-results.json`, `fixture-fixups-before/`, `hvf-controls.json`.

最終連結後的二進位再次通過完整 Darwin 驗證。固定 dev 0a9a1d28d 的 20 組共享元件檢查共記錄 4,515 項：4,489 項通過、6 項可選 Z3 與 20 項缺少 Windows EH 樣本跳過、零失敗。其中公共 C/CLI 為 170/170，報告為 32/32。Python 在 30.780 秒內驗證全部五種設定。12 種原生行動端架構/重定位組合的 4,532 項結果與原程式一致；單工作階段及完整 Swift 中繼資料對照為 12/12。Swift witness 產生器修正註解縮排後，使用記錄的 SDK/編譯器重現了目錄。這是本機 Release LLVM 23/Apple Clang 17 的驗收，不代表後續 dev 修訂或 Linux Clang 18 已驗收。


## 明確宣告之預置目錄子樹的普通移動

預置非根目錄可用嚴格布林值 `"movable": true` 授權該根的普通 rename，並宣告整個初始子樹皆為具有唯一名稱的普通非掛載目錄。C++ 的 `DarwinFileOptions::MovableDirectories` 附加於原聚合成員之後；直接父目錄必須可變。省略或 false 保持未知，其他 JSON 型別無效。後代保留自己的 mutable、removable、movable 和檔案寫入授權；此宣告不授權預置目錄 SWAP、權限判斷或一般掛載語義。已知 flags、特殊目錄權限、多連結普通檔案和 stat/目錄快照中的 inode 別名拒絕准入。宣告連接的完整非掛載域只能有一個已知裝置編號，包括沒有 stat 的共同祖先下之兄弟目錄及檔案；裝置相等本身不能連接其他域。

目錄物件持有名稱、父關係、原 stat/快照及授權。舊輸入路徑不會重新建立已移動或刪除的名稱。未開啟後代、FD/dup/CWD、映射及已刪除後代保留原物件；未變動後代繼續使用原 stat、快照、cookie 和 SEEK_END。移動根及名稱變動的父目錄使完整觀察變為未知。舊名重用不繼承原觀察或授權。初始輸入只宣告一次同步執行的環境，不是執行期替換目錄的 API。SWAP 能力分開保存：初始物件保留直接 swap_rename 宣告，mkdir 從父物件複製能力，移動不重算；兩側 SWAP 父物件皆須已宣告支援。

替換空的預置目標另需 removable 授權。交易在發布名稱前檢查所有路徑不超過 1023 位元組及共用 16 MiB 預算。每個 movable 引用固定預留原路徑加 NUL，不增加項目。初始目錄路徑、引用、快照和項目刪除後仍固定預留；動態 PathCharge 從零開始，移動時為所有連結中或保留成員的目前路徑計費一次。只能抵扣可立即釋放目標已有的動態費用；FD/CWD/子物件/孤兒映射保留不提供抵扣，最終回收只退一次動態費用。隱式初始祖先不增加 256 項目上限的計數，初始普通檔案的項目回收行為不變。

例：

```json
{"darwin_files":{"files":[],"directories":[{"path":"/","mutable":true},{"path":"/work","mutable":true,"movable":true}]}}
```

不依賴 SDK 的 initial-directory-move 工作負載檢查原目錄移動、共用與獨立游標、FD 旗標、CWD、私有映射、替換及原名復原；本機 guest 設定與實體 iOS 對照是獨立驗收。

此處的「初始目錄 SWAP 不支援」指系統呼叫的來源或目標根物件；交換建立出的祖先目錄可攜帶先前移入的初始後代，保留物件狀態與動態費用。前文的初始目錄限制適用於未提供所需宣告的情況。

最終獨立複審後的 ARM64 macOS 驗證登記 1,264 項 Darwin 測試：796 通過、468 項因後端不可用跳過、零失敗；117 項必需 ARM64 HVF 工作負載全部執行。檔案服務子集 342/342，包含 22 個新增 4K/16K 行為實例及三項准入檢查。CreationPolicy 沿用移動父物件原 Device/GID，舊名重用、inode 序列與目前 umask 不會混淆；精確 16 MiB 容量下的 16 次往返交換不累積收費，移回僅釋放實際六位元組差額。公共 C/CLI 175/175、報告解析 33/33、原生核心工作負載 29/29，獨立原始探針有 19 項成功觀察。Python 五種設定在 27.865 秒內通過，純 API 71 項、SDK 漂移及 runner 49 項檢查通過。計數有重疊；原始碼、二進位、失敗嘗試與最終結果保留於 build-hvf-arm64/initial-directory-move/ 並綁定提交。實體 iOS、暫停的 Intel HVF、初始根 SWAP、權限/掛載/大小寫、共享映射 EOF、Mach/執行緒/dyld 及框架執行環境仍是獨立缺口。

## 顯式初始目錄根的原子交換

嚴格布林值 exchangeable:true（C++ DarwinFileOptions::ExchangeableDirectories，追加於聚合末尾）僅授權顯式非根初始目錄物件作為 RENAME_SWAP 運算元；直接初始父目錄必須可變。省略/false 仍不支援，其他型別無效。與 movable 共用普通非掛載、唯一名稱子樹及 flags/特殊模式/別名/硬連結/完整連接域裝置准入，但聯集僅定義拓撲。兩種宣告各固定預留原路徑加 NUL，同一根也分別收費，不增加項目；裝置相等不連接其他域。

普通及 EXCL 初始來源仍需 movable，普通初始替換目標仍需 removable。exchangeable 不授予這些權限、後代 mutable、檔案寫入或通用權限/掛載語意。不同物件交換時，兩側實際父物件須可變且分別支援 swap_rename；已授權普通元件同名 SWAP 在父授權/裝置檢查後無副作用，不首次計費，也不要求不同物件交換能力。同物件 dot/大小寫仍未知，缺少目標與 dot 順序不變。

支援兩個初始非空根、初始/建立目錄及目錄/檔案的兩個方向。雙方完整連結與保留子樹先預檢，再全部摘取後發布；两根保持連結，不抵扣目標或檔案內容，不配置 FD/inode/項目。原 FD/dup/CWD/游標、父物件、映射租約與授權維持歸屬；未變後代保留 stat/快照，同名刪除舊物件與新物件互不混淆。初始動態路徑費用為零，首次交換計一次現有路徑，後續替換舊費用；固定費用不退。路徑或預算失敗維持雙方狀態。

原始 SDK 無關 initial-directory-swap 工作負載交換預置 empty 目錄與 data 檔案並換回，檢查子樹、映射、CWD、游標、FD 旗標、移動後建立與清理。前節 f98068c07 是獨立凍結的普通移動驗收；初始根 SWAP 僅由此宣告擴展。

```json
{"darwin_files":{"files":[],"directories":[{"path":"/","mutable":true,"swap_rename":true},{"path":"/left","exchangeable":true},{"path":"/right","exchangeable":true}]}}
```


本輪在 macOS ARM64 Release 核對 1,303 項 Darwin 註冊：823 通過、480 因後端不可用跳過、零失敗；120 項必要 ARM64 HVF 檢查均已執行。檔案檢查 361/361（含 16 個新增 4K/16K 行為實例及 3 項准入检查）、C/CLI 180/180、報告解析 34/34、原始核心程式 30/30，獨立探針 35 項觀測通過。Python 五種配置用時 33.894 秒，71 項純 API、49 項清單/對照單元檢查及 SDK 漂移、格式、能力、來源、文件檢查通過。計數互相重疊。

精確容量下，同名操作及 16 次往返交換保留物件與費用；需要 6 位元組而僅餘 5 位元組時，雙向拒絕均保留兩棵樹、游標及後續建立預算。獨立計畫及最終原始碼審查通過。嘗試、原始碼、二進位及結果保存在 `build-hvf-arm64/initial-directory-swap/` 並綁定提交；誤重疊的報告檢查排除後循序重跑，翻譯標記已同步，未放寬時限或負控。權限、未宣告掛載/大小寫、共享映射/EOF、推進時鐘、Mach/執行緒/dyld/框架仍未完成；實體 iOS、暫停的 Intel HVF 及遠端合併 CI 分開驗收。

## 明確的唯讀資源限制觀察值

五種 Darwin guest 配置的 `getrlimit(194)` 讀取 `DarwinSystemOptions::ResourceLimits`（`darwin_system.resource_limits`）。0..8 的 `DarwinResourceLimit` 含偏移 0/8 的兩個小端 uint64，共 16 位元組；`0 <= current <= maximum <= 9223372036854775807`，零是明確值，INT64_MAX 表示無窮。不查詢宿主，不改變 FD、VM、儲存或執行預算；`setrlimit`、限制執行、訊號與排程尚未完成。

嚴格 JSON 最多九個唯一鍵，每項恰含 `resource`、`current`、`maximum`，接受精確整數或無符號十進位字串。重複、缺項、錯誤型別、非規範鍵及倒置值在載入前失敗；省略或空陣列仍是未知。配置鍵不套用系統呼叫的旗標或截斷規則。

選擇器只取低 32 位並清除 `_RLIMIT_POSIX_FLAG=0x1000`。非法資源先回 EINVAL，缺值先停止為不支援，均不存取輸出。完全不可寫回 EFAULT；部分可寫保持全部位元組並停止為不支援。完整未對齊或跨頁複製僅寫 16 位元組；後端錯誤保留為傳輸錯誤。原生 ARM64 探針通過 23 項值/選擇器與四項獨立故障檢查，局部前綴不變不能推廣為通用保證。

```json
{"darwin_system":{"resource_limits":[{"resource":8,"current":256,"maximum":"9223372036854775807"}]}}
```

macOS ARM64 Release：註冊/通過/未執行略過/失敗為 1337 / 845 / 492 / 0，123 項必要 HVF 全部執行。新增十二項 4K/16K 行為檢查及 SDK 捕獲對照；C/CLI 190/190、解析 37/37、原生負載 31/31，Python 五配置 71.978 秒、純 API 71 項、執行器 49 項通過。SDK 漂移、格式、能力、來源與文件檢查通過；數量重疊。證據封存在 `build-hvf-arm64/resource-limit-observations/` 並綁定提交，首次編譯的測試列舉筆誤及接線/篩選嘗試均保留，修正後串行驗收，期限與負控不變。資源執行、權限、變更後目錄資料/枚舉、共享映射/EOF、推進時鐘、Mach/執行緒/dyld 與框架仍未完成；實體 iOS、暫停的 Intel HVF 及遠端合併 CI 另行驗收。

## 明確唯讀資源用量觀察

五種 Darwin guest 設定的 `getrusage(117)` 讀取獨立可選的 `DarwinSystemOptions::ResourceUsageSelf` / `ResourceUsageChildren`；嚴格 JSON 為 `darwin_system.resource_usage.self` / `.children`。每個 `DarwinResourceUsage` 需要 int64 的 user_seconds/system_seconds、小於 1000000 的 uint32 user_microseconds/system_microseconds，以及十四個 int64 counters。精確整數或有號十進位字串保留全範圍；錯誤型別/欄位/微秒/長度於載入前拒絕。缺少的另一快照仍未知，不妨礙已提供的快照；全零有效。

一次完整 144 位元組小端複製：timeval 在 0/16，各為 8 位元組秒、4 微秒、4 零填補；counter 從 32 起各 8 位元組。保留 Darwin 原值與單位，不對 ru_maxrss 套用 Linux KiB 換算。固定觀察不取樣宿主效能，也不實作記帳、fork/wait、排程或限制執行。

選擇子只取低 32 位元，0=SELF、-1=CHILDREN；0x1000 無效，不移除 POSIX 旗標。無效值於記憶體存取前 EINVAL，缺值於輸出存取前 unsupported。完整跨頁/非對齊複製保留護欄；全不可寫 EFAULT，部分可寫在任何複製前 unsupported；後端錯誤仍是傳輸錯誤。SDK 每個選擇子只捕獲一次，SELF 後續可能變化。原生探針通過 13 個值/佈局檢查與 4 個獨立故障程序，五秒時限不變。本機 partial SELF 在 EFAULT 前寫 64 位元組，不推廣前綴保證。

0..13: `ru_maxrss`, `ru_ixrss`, `ru_idrss`, `ru_isrss`, `ru_minflt`, `ru_majflt`, `ru_nswap`, `ru_inblock`, `ru_oublock`, `ru_msgsnd`, `ru_msgrcv`, `ru_nsignals`, `ru_nvcsw`, `ru_nivcsw`.

```json
{"darwin_system":{"resource_usage":{"self":{"user_seconds":"-9223372036854775808","user_microseconds":999999,"system_seconds":0,"system_microseconds":0,"counters":["9223372036854775807",0,0,0,0,0,0,0,0,0,0,0,0,0]}}}}
```

[Apple getrusage](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/getrusage.2.html), [XNU](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_resource.c), [SDK layout](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/resource.h).

macOS ARM64 Release 登記/通過/不可用略過/失敗 1371/867/504/0，必需 HVF 126 全執行；檔案 361/361，C/CLI 200/200, JSON 40/40, System/SDK 53/53, native 32/32 通過。新增十二個 4K/16K 行為及准入/SDK 對照，Python 五設定 42.865 秒；API 71、runner/reference 49 和 SDK 漂移/格式/能力/來源/文件檢查通過。計數重疊，獨立複審通過。證據凍結於 `build-hvf-arm64/resource-usage-observations/` 並綁定提交。初版 x64 EINVAL 次要返回斷言修正為保留 RDX，ARM64 清零 X1；失敗與空過濾記錄保留，執行模型、期限與負控不變。限制執行、權限、修改後目錄、共享 map/EOF、時鐘、Mach/thread/dyld 與框架仍未完成；實體 iOS、暂停 Intel HVF 與遠端合併 CI 分別驗收。

首輪完整驗證為 866 通過、1 個既有 iOS ARM64 HVF rename 五秒超時、504 略過；同一二進位複查 386 毫秒通過，後續完整串行驗證通過。原記錄保留，超時原因未知，不推論時延保證。


## 明確憑據、群組與一致建立屬主

可選 Credentials 含 RealUID/EffectiveUID/RealGID/EffectiveGID 及獨立可選 GroupAccessList 與 GroupMembershipUID。省略時四查詢為1000；明確零/root有效，四個純量 ID 與群組項目0..INT32_MAX；GroupMembershipUID 的獨立範圍見後文。群組1..16，首項EffectiveGID，保留順序/重複；缺少仍未知，不推斷宿主/EGID。嚴格 darwin_system.credentials 恰需 real_uid/effective_uid/real_gid/effective_gid，可加獨立可選 groups 與 group_membership_uid；無損整數與統一驗證器在載入前拒絕欄位/形狀/範圍/數量/首組錯誤，非Darwin仍拒絕。

getuid24/geteuid25/getgid47/getegid43/getgroups79 共用系統所有者；新普通檔UID用有效使用者，device/GID繼承直接父目錄。改名、保留FD及舊名重用保持物件；輸入stat不變，root不授權寫入/目錄修改/權限/ACL，setuid/setgid/setgroups與程序/工作階段未實作。

getgroups容量低32位有號int：負數先EINVAL，未知先unsupported，已知0只回傳數量不碰指標，正數不足先EINVAL，足夠一次複製4*數量小端位元組。0x1000為正容量，不剝POSIX旗標；非對齊/跨頁保留保護，完全不可寫EFAULT，部分在任何位元組前unsupported，backend錯誤保持傳輸錯誤。BSD錯誤保留x64 RDX、清ARM64 X1，成功皆清次結果；報告保留原始參數。


~~~json
{"darwin_system":{"credentials":{"real_uid":101,"effective_uid":202,
 "real_gid":303,"effective_gid":404,"groups":[404,0,"2147483647",7,7]}}}
~~~


[Apple getgroups contract](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/getgroups.2.html), [pinned XNU credential/group ordering](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_prot.c).

註冊/通過/不可用略過/失敗: 1413/897/516/0; ARM64 HVF 129/129. System/SDK72/72, File365/365, Report43/43, C/CLI210/210; guest8/12skip; native33/33; Python5 82.918s; API71, runner/reference49. SDK drift / clang-format22.1.2 / capabilities23 / provenance / docs11+negative3: OK. Independent source review: OK.

21 value +5 native fault,5s; host16groups, positive short capacity: OK. Native partial32byte thenEFAULT: observation only. Counts overlap. `build-hvf-arm64/credential-observations/`.

首輪8失敗（5指令預算、3個5秒期限）、12略過；兩頁掃描太重。改為同一跨頁完整132位元組保護（64前、最多64資料、至少4後），直接測試仍驗整兩頁。預算/參數/故障負控不變，源碼/二進位/兩次結果保留；直接傳輸跨頁幾何執行前修正，公共選項使用內容比較。權限/ACL、連結、修改後目錄完整觀察、shared maps/EOF、時鐘、Mach/thread/dyld/framework未完；實體iOS、暂停Intel HVF、merge CI分開驗收。

## 由明確進程與核心限制決定的描述符表查詢

可選 `DarwinSystemOptions::MaxFilesPerProcess` / `darwin_system.max_files_per_process` 宣告非負 int 觀察值，缺省未知，明確零有效。名稱 `kern.maxfilesperproc` 與數字 MIB `[1,29]` 獨立讀取同一個四位元組值，不要求資源限制。無損整數解析與中央驗證在載入前拒絕錯誤型別、負數及越界值；非 Darwin 設定明確拒絕。

BSD `getdtablesize(89)` 必須同時具有該上限與 `ResourceLimits[8].Current`，回傳兩者較小值。先截斷完整 64 位 Current，再按 int 回傳：Current=`0x100000001`、cap=64 得到 64，無窮限制也安全截斷。Maximum、宿主值、目前 FD 數與 `DescriptorLimit` 不替代觀察值；任何一項缺失時，即使另一項為零仍 unsupported。忽略全部六個引數，不存取使用者記憶體，沿用 BSD carry/次級暫存器約定；Mach timebase 的 89 號陷入保持獨立。

sysctl 沿用既有複製階段。EUID0 的真實寫入在名稱/MIB 與 oldlenp 預檢後、觀察值和輸出前停止 unsupported；非 root 回傳 EPERM，新指標長度為零仍讀取。不據此執行限制或授予寫入權限。既有必需資源 workload 核對兩種 cap 查詢及低位/高位 syscall number，輸出仍為 `l` / 144 位元組，後續 unsupported 保留已輸出資料。獨立純量 fixture 驗證缺值、零、寬 Current 與 DescriptorLimit=3 的預算獨立性。

```json
{"darwin_system":{"max_files_per_process":64,"resource_limits":[{"resource":8,"current":"4294967297","maximum":"9223372036854775807"}]}}
```

[XNU getdtablesize](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_descrip.c), [XNU proc_limitgetcur_nofile](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_resource.c), [XNU MIB constants](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/sysctl.h).

驗證: Darwin (登記/通過/不可用/失敗) 1441/913/528/0; ARM64 HVF 132/132; System/SDK 80/80; File 365/365; C/CLI 211/211; ProcessReport 45/45; native 33/33; Python 5 (42.254s); API 71; runner 44 + reference 5. clang-format 22.1.2; capabilities/provenance; docs 286 / locales 10 / negative controls 3.

嘗試記錄：首次 runner 檢查在 ARM64 與 x86-64 清單子項失敗，因為新純量方法缺少必需登記。補齊登記並保留全部方法相等規則。初始 129 必需項門檻及失敗記錄保留，重新執行的最終 ARM64 門檻要求 132 項。

計數互有重疊。新證據於 `build-hvf-arm64/descriptor-table-observations/` 保留實際執行基線及精確原始碼、二進位、日誌雜湊，舊證據不變。原生探針為五個一次性 ARM64 macOS 子進程、40 項檢查，每進程五秒期限；預設 Current=1048575/cap=245760 回傳 245760，子進程 Current=0/1/32/245777 回傳 0/1/32/245760，父進程與系統限制未改變。iOS 實機、暫停 Intel HVF、遠端 merge CI 分別驗收。權限、變更後目錄觀察、共享映射/EOF、推進時鐘、Mach/thread/dyld 與框架執行期仍未完整。

## 明確的程序觀察值

`DarwinSystemOptions::ProcessGroupID`、`SessionID`、`ProcessTainted` 是彼此獨立的選用輸入，對應 JSON `process_group_id`、`session_id`、`process_tainted`。兩個 ID 必須為正且不超過 INT32_MAX；污染狀態只接受 JSON 布林值 `true`/`false`。省略仍表示未知，明確的 `false` 是已知零值。不從主機、PID1000、憑證或其他觀察值推斷。

原始 `getpgrp(81)` 讀取程序群組；`getpgid(151)`、`getsid(310)` 使用帶正負號的低32位 `pid_t`，零或固定目前 PID1000 查詢自身，例如 `0xffffffff000003e8` 仍指自身。負的低32位 PID 在讀取觀察值之前回傳 ESRCH3，經唯讀原生探針及 XNU 程序配置、查找規則確認。未知的正數其他程序（包括 `0x1000`）以 UnsupportedService 停止，不猜測 ESRCH，也不套用其他介面的旗標遮罩。缺少所選自身觀察值同樣停止。`getpgrp`、`issetugid(327)` 忽略全部參數；四個純量查詢均不存取客體記憶體，沿用 BSD carry 與第二回傳暫存器規則。

`process_tainted` 提供固定的 `P_SUGID` 觀察值，與真實、有效 ID 是否相等無關；不改變 EUID、檔案所有權、sysctl 寫入權限、授權、領導關係或終端狀態。`setpgid`、`setsid` 與憑證修改仍不支援。原始唯讀 `process-observations` 工作負載擷取自身 PID，檢查高位參數、負數錯誤與回傳狀態；`virtual-process-observations` 輸出設定的群組、工作階段及污染狀態位元組。其他程序、缺失值及設定器的模型案例不進入原生執行清單。macOS 原生參照不構成實體 iOS 驗收。

```json
{"darwin_system":{"process_group_id":7,"session_id":16909060,"process_tainted":false}}
```

[XNU process queries](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_prot.c), [XNU service numbers](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/syscalls.master), [XNU PID allocation](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_fork.c), [XNU PID lookup](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_proc.c).


## 明確的工作階段登入緩衝區

`DarwinSystemOptions::LoginNameBytes` 獨立宣告工作階段的全部255個原始位元組（`MAXLOGNAME`）。JSON `login_name_hex` 必須包含恰好510個 ASCII 十六進位字元，允許大小寫。嵌入NUL及終止符後的非零位元組均有效。省略仍為未知，明確的全零記錄則是已知值。不會補齊短名稱，也不從主機、憑證、程序群組、工作階段ID或污染狀態推斷資料；此觀察值不授予登入或檔案權限。

原始 `getlogin(49)` 以無符號低32位 `u_int` 解讀長度，恰好複製 min(length,255) 個位元組，不解碼字串、不補NUL，也不輸出所需大小。零長度不需要觀察值或客體記憶體存取，無效指標也成功；`0xffffffff00000000` 因而選擇零長度。非零請求先要求完整記錄，再檢查目的地。完全不可寫回傳 EFAULT14；部分可寫範圍在複製前停止，預檢錯誤不發布資料。後端寫入錯誤由原有使用者複製層傳遞，不承諾一般回復。保留 BSD carry 和第二回傳暫存器規則，包括 x64 錯誤時的原始 RDX。

`setlogin(50)` 在明確root憑證或全零緩衝區下仍不支援。原始唯讀 `login-buffer` 檢查完整長度參數、前綴、相鄰位元組、零長度指標及EFAULT；`virtual-login-buffer` 輸出宣告的255個位元組。缺失值及設定器的模型測試不進入原生執行目錄。macOS ARM64參照不代表實體iOS或Intel原生驗收。範例明確宣告全部255個零位元組，不是推斷的空登入名稱。

客體驗證器在每次複製前仍初始化全部265個輸出位元組，再按升序檢查三個不相交範圍：[0,3)、[3,3+n)、[3+n,265)，n 沿用原有截斷後的複製長度。首尾範圍檢查全部保護位元組，中間逐位元組比較原始快照。12個完整長度參數、21次零長度呼叫、42次EFAULT、carry/第二暫存器檢查及原始/虛擬路徑均保留，並沿用現有執行限額。此改動減少驗證程式自身的工作量，保持全部位元組覆蓋和首次錯誤順序；正式執行階段效能仍需獨立測量。

```json
{"darwin_system":{"login_name_hex":"000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"}}
```

[XNU getlogin / MAXLOGNAME](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_prot.c#L1561).


## 明確的目前程序優先權

`DarwinSystemOptions::ProcessNice` / JSON `darwin_system.nice` 獨立宣告 -20 至 20 的固定有號 nice 值。省略表示未知，明確的零與 -1 都是已知值。沿用無損整數解析，接受整數數值與十進位整數字串；錯誤型別、小數、指數字串、空白及超出範圍的值在載入映像前拒絕，精確整數的數值 JSON 仍有效。非 Darwin 設定拒絕此欄位。不從主機、憑證、群組、工作階段、污染、登入、資源或 CPU 觀察值推斷 nice，也不改變排程、權限或執行預算。

原始 `getpriority(100)` 使用 int 選擇器與無號 `id_t` 目標的低32位。目標超過 INT32_MAX 先回傳 EINVAL22；未知選擇器（包括 GPU5、0x1000）及執行緒選擇器3的非零低32位目標，也在讀取觀察值前回傳 EINVAL。`PRIO_PROCESS`0 僅接受零或目前 PID1000，選中自身後才要求 nice。其他正數 PID 不支援，不猜測 ESRCH；群組1、使用者2、執行緒3/目標0及擴充選擇器4、6、7、8 即使相關欄位齊全也不支援。執行緒目標僅高位非零仍選擇未知狀態，不是 EINVAL。結果以符號擴展至完整64位：-1 是 carry 清除的成功 UINT64_MAX。查詢不存取客體記憶體並忽略未用參數；BSD 第二回傳暫存器維持 x64 錯誤保留 RDX、成功清零，ARM64 兩種路徑均清零 X1。

`setpriority(96)` 在明確 root 與 nice 下仍不支援。原始唯讀 `process-priority` 檢查自身參數、必定非法的參數及成功/錯誤/成功轉換；`virtual-process-priority` 輸出設定的八個有號位元組。其他程序、聚合、缺失與設定器僅用模型測試。新 ARM64 macOS 探針通過191項，nice樣本為0；非負樣本不能證明負數硬體擴展，負數使用固定版本的有號入口宣告及獨立模型邊界驗證。實體 iOS 與 Intel 原生驗收仍分開進行。

```json
{"darwin_system":{"nice":-1}}
```

[XNU getpriority](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_resource.c), [XNU signed INT entry](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/dev/arm/systemcalls.c).


## 禁止跟隨旗標與目錄預檢

一般及 nocancel `open` / `openat` 接受無符號低32位 O_NOFOLLOW=0x100 或 O_NOFOLLOW_ANY=0x20000000。查找旗標不出現在 F_GETFL，也不改變存取、附加、截斷、建立或 FD 獨立的 CLOEXEC。兩者同時設定時，先檢查 FD 容量，再於完整路徑匯入前回傳 EINVAL22；滿表先回傳 EMFILE24。未知旗標仍不支援。

非 AT_FDCWD 的 `openat` 在存取模式及容量檢查前只匯入首位元組。首位元組不可讀回傳 EFAULT14；相對前綴（含 NUL）先檢查 FD 持有的目錄物件：未知 FD 為 EBADF9，一般檔案為 ENOTDIR20，未知串流 vnode 類型仍不支援。`/` 跳過 dirfd 驗證，之後才按既有 open 順序匯入完整路徑。一般 `open`、AT_FDCWD 不做此前綴預檢，其他 nameiat 共用首位元組/相對 FD 預檢；AT_FDONLY 繞過路徑。拒絕操作不占用 FD 或新 inode，傳輸錯誤直接傳遞，不發布命名空間變更。

原始 `file-access` 以相對目錄 FD 檢查 NOFOLLOW_ANY，避開原生 `/var`、`/tmp` 連結別名。直接測試涵蓋首位元組及後續故障、斜線/NUL、使用者位址與頁面邊界、滿表及已刪除目錄物件。獨立 ARM64 macOS 原始探針在既有五秒期限內通過30項；最後的滿表絕對路徑實際使用有效目錄 FD，保留原標籤但不擴大證據範圍。實體 iOS 與 Intel 原生驗收仍需分別完成。

[XNU open1at / open1](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c), [XNU vn_open_auth](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_vnops.c), [XNU open flags / FMASK](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/sys/fcntl.h).

## 固定初始符號連結

下述初始名稱預設受保護；明確授權後的行為見文末初始連結可變授權一節。 C++ `DarwinFileOptions::SymbolicLinks` 與 JSON `darwin_files.symbolic_links` 宣告固定連結：規範絕對 `path`、原始十六進位 `target_hex`，可選 `metadata` 描述連結自身。目標為1..1023非 NUL位元組，保留非 UTF-8、重複斜線與點，可懸空。`files` 仍必填，可為空陣列。名稱不得與檔案/目錄衝突或成為其他名稱祖先；路徑/NUL及目標共用256項、16 MiB限額。S_IFLNK、size=目標長度、DT_LNK=10及一致 inode 必須相符；配置 CWD須為實際目錄。

解析器先展開再處理點，相對目標從實際包含目錄開始，絕對目標從客體根開始。每次重解析尾斜線，已消耗的輸入斜線不會變成目標斜線。最多32次，第33次 ELOOP62；目標+剩餘後綴+NUL超過1024位元組 ENAMETOOLONG63。FD、CWD、F_GETPATH、mmap 持有最終目標。

stat64/open/access/truncate/chdir 跟隨末端；lstat64/readlink保留末端。O_NOFOLLOW返回 ELOOP，疊加 O_DIRECTORY先返回 ENOTDIR20。O_NOFOLLOW_ANY拒絕所需展開；O_CREAT|O_EXCL對已有末端連結返回 EEXIST17，包括懸空/循環。AT0x20保留末端，AT0x800也保留末端並拒絕中間/尾斜線所需展開，可合併。AT_FDONLY在旗標驗證後忽略路徑。

readlink(58)使用有符號低32位count，readlinkat(473)保留完整size_t；皆返回int，超過INT32_MAX先於路徑/FD返回EINVAL22。只複製min(count,目標長度)，不補NUL，只預檢實際前綴。零長度仍驗證路徑/類型後忽略輸出指標；非連結EINVAL22，全不可寫EFAULT14，部分可寫在複製前停止；傳輸/記憶體預算錯誤傳遞。

固定連結名稱與原始目標位元組保持不變。MutableDirectories 不得為根或任何固定連結名稱的路徑分段祖先；/work 不包含 /workspace/link。獨立可變目錄可容納連結目標，包括執行期間建立、移動、刪除及替換的名稱。既有父目錄、掛載、別名、旗標、交換授權與建立策略檢查仍適用；新 inode 必須大於所有中繼資料/快照 inode，包括受保護連結。 固定 WritableFiles/MutationPolicies仍可修改最終普通檔案；連結unlink/rename在效果前明確停止。執行期間連結建立見下節；硬連結、ACL與未獲單獨授權的初始連結修改未支援。ARM64 macOS獨立探針在原五秒內通過189觀察、115完整緩衝檢查；不單獨證明物理iOS、Intel HVF或完整OS。

新增 60 項 ARM64 macOS DELETE/RENAME 原生矩陣在原五秒期限內記錄完整 stat 緩衝、變更前後命名空間及保留 FD/CWD 身分。尾端斜線可展開固定連結並修改實際目標；NOFOLLOW_ANY 拒絕必要展開並回傳 ELOOP。無 SDK 的 symbolic-link-mutations 程式亦檢查建立、懸空目標、改名/刪除/替換、保留 CWD 父物件及關閉描述符前全部原始 10 個檔案位元組，以及關閉後仍保留的全部 10 個映射位元組；這不證明實體 iOS 或原生 Intel 覆蓋。

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"3031"}],"symbolic_links":[{"path":"/link","target_hex":"64617461"}],"working_directory":"/"}}
```

[XNU namei](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_lookup.c), [XNU readlink / AT](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c), [XNU open authorization](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_subr.c).

## 執行期間建立符號連結

原始 `symlink(57)` 與 `symlinkat(474)` 可在獲准修改的目錄內建立程序本地連結，回傳 int；目錄 FD 僅取低32位，絕對目的名稱忽略 FD。先匯入連結目標，再檢查目的名稱及 FD；首個 NUL 截斷0..1023個原始位元組，允許空、非 UTF-8、點與重複斜線。1024位元組無 NUL 回傳 ENAMETOOLONG63，先遇到不可讀位元組回傳 EFAULT14。初始 JSON 連結仍要求1..1023位元組。

目前連結表持有實際名稱、父物件及原始位元組。既有末端名稱回傳 EEXIST17；懸空連結後已消耗的尾斜線可使建立發生於其目標名稱，舊連結不變。展開空目標回傳 ENOENT2；readlink 讀取空目標回傳0，正容量也不存取輸出指標，但仍先檢查計數、路徑與類型。

名稱/NUL與目標僅計費一次，共用256項、16 MiB配額。拒絕不發布節點、不改變父目錄、不消耗 FD或普通檔案 inode。新連結完整中繼資料明確未知，不沿用普通檔案 CreationPolicy 或重用名稱的舊觀察；成功建立使父目錄 stat/快照未知。刪除或替換目標後，舊 FD/CWD/映射保留原物件。rmdir 與目錄替換會檢查連結子項；移動或 SWAP 任一側包含受保護初始連結時在作用前停止。連結與目錄的替換、硬連結、ACL與未獲單獨授權的初始連結修改仍未支援；別名不轉移實際父目錄授權。

原生 ARM64 macOS 的150項記錄保留四項觀察器失敗；獨立10項補充只核對實際新目標與空連結邊界，不改寫舊結果。無 SDK 的 `symbolic-link-creation` 程式檢查兩個入口、原始位元組與緩衝邊界、父物件、替換檔案及舊 FD/映射全部10位元組；不證明實體 iOS、原生 Intel 或完整 OS相容性。

[XNU symlink / symlinkat](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c), [XNU empty-link expansion](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_lookup.c).

## 刪除執行時建立的符號連結

`unlink(10)` 與 `unlinkat(472)` 可刪除實際父目錄已授權修改的執行時連結；初始固定連結仍受保護。裸 `AT_SYMLINK_NOFOLLOW_ANY` 保留末端連結，允許刪除懸空、循環及空目標連結；中間或尾斜線要求展開時回傳 ELOOP62。無此旗標時由統一解析器選擇實際節點：`a → b → target` 中刪除 `a/` 會刪除 `b`，保留 `a` 與最終目標。既有旗標、路徑及 dirfd 錯誤順序不變。

成功刪除立即退還一個動態項目及目前路徑/NUL/原始目標位元組費用，只使實際父目錄的完整 stat/列舉觀察失效，不消耗 FD 或建立 inode。目標內容、修改策略、舊描述符與共用游標、CWD、映射租約保留原物件；重用名稱不恢復舊連結。拒絕不改變模型狀態或退款。執行時建立連結的完整中繼資料仍未知。連結與目錄的替換，以及仍含受保護初始連結的子樹移動/SWAP 仍不支援。

獨立 ARM64 macOS 原生參考於原五秒期限內記錄40例：28次成功刪除、12次原生錯誤及17次重複刪除 ENOENT，並檢查 FD/CWD/私有映射。這不是客體、實體 iOS 或原生 Intel 驗收；`symbolic-link-unlink` 與公開 API 用例另行驗證。

## 重新命名執行時建立的符號連結

`rename(128)`、`renameat(465)` 與 `renameatx_np(488)` 支援普通改名及 `RENAME_EXCL=4`：執行時連結移至空名稱、連結取代連結、連結取代普通檔案及普通檔案取代連結。兩個實際父目錄須在已證明的同一掛載域授權修改；初始固定連結仍不可變。統一解析器選取實際節點，空、懸空、循環與非 UTF-8 目標位元組不變；跨父目錄移動後，相對目標從新父目錄解析。裸 `RENAME_NOFOLLOW_ANY=16` 保留末端連結，中間路徑需要展開時回傳 ELOOP62。EXCL 對不同的既有目標回傳 EEXIST17；同物件 EXCL 缺少檔案系統大小寫屬性時明確不支援。

交易在發布名稱前預留新路徑/NUL費用，含 NUL 最多1024位元組。被取代執行時連結的目前路徑/NUL/目標費用立即且只退還一次，與目標檔案 FD 或映射無關。被取代普通檔案的內容/動態路徑費用由舊描述符與映射租約持有，全部釋放才回收；只有可立即回收的普通目標提供預留額度。改名不需額外項目、FD 或普通檔案建立 inode。拒絕保留兩個節點，成功使實際父目錄完整 stat/列舉觀察失效；執行時連結完整中繼資料仍未知。連結與目錄替換、硬連結，以及仍含受保護初始連結的子樹移動/SWAP 仍不支援。

獨立 ARM64 macOS 的19項原生對照在原五秒期限內記錄14次成功、5次 EEXIST/ELOOP 錯誤，並核對連結 inode/原始位元組、相對目標重新繫結及舊 FD/dup/游標/CWD/私有映射。無 SDK 的 `symbolic-link-rename` 與公開 SDK/CLI 用例另行檢查；原生參考本身不證明實體 iOS、原生 Intel 或完整 OS 相容。

[XNU rename / namei](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c).

## 交換執行時建立的符號連結

`renameatx_np(488)` 的 `RENAME_SWAP=2` 支援執行時連結/連結、連結/普通檔案與普通檔案/連結的葉節點交換。兩個實際父目錄須在已證明的同一掛載域授權修改及 SWAP；完全相同名稱的連結交換不需額外 SWAP 宣告即可無作用成功。目標缺少回傳 ENOENT2；`SWAP|NOFOLLOW_ANY=18` 保留末端連結，必要中間展開回傳 ELOOP62；`SWAP|EXCL=6` 在讀取路徑前回傳 EINVAL22。原始目標位元組不變，相對目標分別從兩個新父目錄解析。

交易先預留兩個路徑/NUL費用，再發布名稱。兩個節點持續連結，內容與映射租約不提供替換退款；初始普通檔案首次交換取得動態路徑費用，交換回來繼續重用。不需項目、FD 或建立 inode。檔案身分、nlink、舊描述符/共用游標、CWD 與映射保留原物件，實際父目錄完整觀察變為未知。執行時連結完整中繼資料、實際目錄/連結組合與含受保護初始連結的子樹移動/SWAP 仍不支援。獨立 ARM64 macOS 的22項原生對照在原五秒期限內記錄14次交換、2次同物件成功與6次錯誤。擴充 `symbolic-link-rename` 與 C++/SDK/CLI/Python 檢查實際 SWAP 標誌及錯誤；這些參考不證明實體 iOS、原生 Intel 或完整 OS 相容。

[XNU rename / namei](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c).

## 移動包含執行時符號連結的目錄樹

普通/EXCL目錄移動及SWAP現已將執行時連結子項納入既有目錄交易，也支援目錄與普通檔案交換。交易先檢查所有目錄、檔案與連結的新名稱、共用命名空間衝突及含NUL的1024位元組路徑上限，再完整抽出三個表的節點並發布新鍵。目標位元組持續計費且不改寫，只更新目前路徑/NUL費用；SWAP不提供替換退款，不消耗額外項目、FD或建立inode。

連結保留實際父目錄物件，父目錄路徑移動後，相對目標按新路徑解析；舊描述符、共用游標、CWD、已刪除節點與映射租約繼續保有原物件。受保護初始連結與所在樹仍不可變，根節點目錄/連結組合、執行時連結完整中繼資料、硬連結及ACL仍不支援。獨立ARM64 macOS的25項對照在原五秒期限內記錄5次移動、10次交換、2次同物件成功及8次保留命名空間的拒絕；跨父目錄案例檢查原文不變與相對目標重新綁定。擴充 `symbolic-link-rename` 覆蓋C++/SDK/CLI/Python，這些觀察不證明實體iOS、原生Intel或完整OS相容。

[XNU rename / namei](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c).

## 新建連結與目錄的中繼資料

可選 `creation_policy.namespace_policy`（C++ `DarwinFileCreationPolicy::Namespace`）在原有五個必需欄位之外新增嚴格物件，僅含 `symbolic_link_allocation_unit`、`directory_entry_size`、`directory_blocks`。連結分配單位為512至16 MiB的2次冪；目錄項目大小為正值且不超過16 MiB；區塊數為不超過INT64_MAX的uint64，可用十進位字串。父目錄中繼資料、初始umask與新inode條件不變。省略擴充時，前文連結/目錄資料未知、僅一般檔案消耗建立inode的行為仍為預設。

啟用後，一般檔案、symlink與mkdir成功插入共用一條inode序列，UINT64_MAX永久耗盡；失敗與開啟既有名稱不消耗inode。Device/GID來自實際父物件，UID來自有效來賓身分。連結模式為S_IFLNK加 `0777 & ~umask`，nlink=1，size為原始目標位元組數（含空目標與非UTF-8），blocks按宣告單位向上取整後以512位元組計。目錄模式為S_IFDIR加 `mode & 0777 & ~umask`，nlink為2加所有直接具名子項，size為nlink乘宣告項目大小，blocks固定；持有的已刪除空目錄也保留此規則。這是明示虛擬策略，不推斷APFS分配行為。

初始時間用creation_time；子名稱成功變更更新新建父目錄mtime/ctime，直接移動連結/目錄僅用mutation_time更新其ctime；移動祖先保留後代資料。完整記錄隨物件經過dup、CWD、替換、SWAP、刪除與名稱重用。僅建立擴充不會保留初始父目錄完整 stat 或固定快照；下文獨立目錄策略提供 stat 與即時列舉依據。ACL、未獲單獨授權的初始連結修改與一般目錄/連結根交易仍未支援。原生 `created-namespace-metadata` 檢查模式、擁有者、身分與生命週期；`virtual-created-namespace-metadata` 經C++/SDK/CLI/Python對照完整144位元組目錄與連結常數記錄。定向驗證通過11項模型/准入、1項嚴格JSON、43項原生工作負載（仍限5秒）、8項客體組合（12項後端不可用略過，3項必需HVF皆執行）及10項公共入口。保留指標表造成的客體失敗及修正後ARM64靜態封裝；原生Intel與實體iOS尚未驗證。

```json
{"namespace_policy":{"symbolic_link_allocation_unit":512,"directory_entry_size":32,"directory_blocks":7}}
```

## 名稱空間修改後的虛擬目錄列舉

可選 `directories[].enumeration_policy` 讓 `getdirentries64` 列舉目前名稱；C++ 使用 `DarwinFileOptions::DirectoryEnumerationPolicies` 與 `DarwinDirectoryEnumerationPolicy`。嚴格物件恰好要求 `minimum_buffer_size`、`initial_minimum_buffer_size`、`seek_offset`：前者為正，兩種下限不超過128 MiB，seek_offset 為無損 uint64。初始目錄須有自身非零 inode 中繼資料，不可同時提供不可變 `contents`；每個策略參照計入一次路徑加 NUL。mkdir 繼承實際父物件策略，既有子目錄保留自己的宣告；列舉與修改授權獨立。

虛擬順序為 `.`、`..`，再按無符號位元組排序的直接連結名稱。inode 來自已觀察或行程建立的物件，`..` 沿實際保留父物件；子項或父身分缺失/為零會在輸出前停止。完整初始 stat 失效後只保留 inode 身分，不重用其他過期欄位。游標為從1開始的區域序號，d_seekoff 為宣告常數；dup 共用游標，open 各自獨立。成功成員變更或直接目錄移動後須先回捲到零，拒絕及同物件無操作不使游標失效，祖先移動保留後代游標。版本耗盡明確停止。保留的已刪除空目錄輸出零筆記錄，名稱重用不取代舊物件。

本段描述僅設定列舉策略、尚未設定下文初始目錄 stat 策略的行為。 沿用記錄編碼、完整記錄封裝、緩衝下限、負載上限、EOF 後綴及資料/游標/位置/旗標順序。初始父目錄完整 stat 與固定快照仍失效；省略策略維持先前不支援行為，並不模擬 APFS 世代。ARM64 原生準備在不變的五秒期限內記錄25個事件、16個視圖。`directory-enumeration-mutations` 核對原生身分與物件保留；`virtual-directory-enumeration` 經客體、C/CLI、Python 核對160位元組固定視圖。Intel 原生、iOS 實機、ACL、硬連結與完整系統/框架仍未驗證或不支援。

## 初始目錄的顯式 stat 修改策略

可選 `directories[].mutation_policy` 讓已准入的初始目錄在名稱空間變更後保留完整 stat。C++ 使用 `DarwinDirectoryMutationPolicy` 與 `DarwinFileOptions::DirectoryMutationPolicies`。嚴格物件恰含 `directory_entry_size` 和 `mutation_time`：項目大小為正且不超過 16 MiB，時間採無損有號 64 位元秒數及 [0,1000000000) 範圍奈秒。目錄須提供自身完整 metadata 和非零 inode。策略參照計入一次路徑加 NUL 的費用；投影 size 不配置檔案位元組。策略不授予名稱空間操作或權限，也不要求建立或列舉策略。

首次真正提交修改前，完整觀察記錄保持原樣；首次修改將純量欄位複製到目錄物件，無需配置。子名稱變更令 nlink 為 2 加所有類型的直接連結名稱數，size 為 nlink 乘 directory_entry_size，mtime/ctime 為 mutation_time。直接移動、SWAP 或刪除僅改 ctime；移動祖先保留後代記錄。拒絕與同一物件無操作均不修改記錄。Device、inode、mode、擁有者、blocks、區塊大小、flags、generation、atime 和 birthtime 保持觀察值。這是顯式虛擬規則，不推論 APFS 配置、連結數或時鐘。

記錄與策略隨原物件經過 dup、保留 FD、CWD、替換、刪除和名稱重用。新 mkdir 物件使用獨立建立策略（若提供），不繼承父目錄或同名舊物件的初始目錄 stat 策略。不可變目錄快照在修改後仍未知；獨立 enumeration_policy 可提供即時檢視。省略本 stat 策略時，已修改初始目錄的完整中繼資料仍未知。

獨立 ARM64 原生準備在不變的 5 秒期限內保留 27 個受保護原始 stat 檢視和 15 項操作，核對 144 位元組 SDK ABI 及保留身分，不推廣原生時間戳或配置規律。原創 `initial-directory-metadata` 檢查原生共同觀察；`virtual-initial-directory-metadata` 經 guest、C/CLI 和 Python 對照完整 144 位元組常數記錄。模型另涵蓋首次刪除、缺少修改授權、省略建立策略及快照/列舉獨立性。原生 Intel、實體 iOS、ACL 執行、硬連結、未獲單獨授權的初始連結修改及完整 OS/框架仍未驗證或未支援。

```json
{"mutation_policy":{"directory_entry_size":17,"mutation_time":{"seconds":-11,"nanoseconds":321}}}
```

## 初始符號連結的明確可變授權

`symbolic_links[].mutable:true`（C++ `DarwinFileOptions::MutableSymbolicLinks`）授權原始連結物件修改命名空間，實際父目錄仍須另行可變。已知 flags、特殊 mode 位、非一的 link_count、身分別名或父裝置衝突拒絕輸入。省略時名稱受保護，不可位於可變祖先域。授權固定預留路徑/NUL 引用，不新增項目、不消耗建立 inode；初始目標位元組保持不變。

unlink、普通/EXCL rename、葉連結/檔案/link SWAP 與授權目錄子樹交易保留實際父物件、mount 和 SWAP 條件。相對目標依新實際父目錄重新解析；FD/dup 游標、CWD、映射租約保留原物件。初始名稱/目標/引用成本於刪除或取代後仍固定預留，首次改名另預留動態名稱。僅退還物件擁有的動態名稱/新建目標成本；只有新建連結增加動態項目。拒絕及同物件空操作不提交變更。

`symbolic_links[].mutation_policy` 使用 `DarwinSymbolicLinkMutationPolicy`、`DarwinFileOptions::SymbolicLinkMutationPolicies`。嚴格唯一欄位 `mutation_time` 的秒為無損有符號 64 位，奈秒 [0, 1000000000)；須有明確授權及完整非零 inode 觀測。首次直接移動/SWAP 無配置複製標量，只將 ctime 設為宣告時間，其餘欄位包括 blocks 保持；祖先移動、拒絕、空操作保留完整記錄。無策略時完整 stat 變為未知，列舉仍保留 inode，已知裝置衝突仍拒絕。策略預留一次路徑/NUL。重用名稱、新 symlink 不繼承初始記錄/策略，建立使用獨立 namespace policy。

原生 ARM64 私有準備保存 14 個 guarded raw-stat 視圖、11 個操作、獨立 144 位元組 SDK ABI，維持 compile120s/native5s/drain1s/reap1s 並記錄清理。`mutable-initial-links` 驗證原生身分、目標重解析及持有目標；`virtual-mutable-initial-links` 於五種 guest、C/CLI、Python 檢查完整 stat。模型覆蓋兩種頁大小、子樹 SWAP、精確成本與條目/inode 耗盡。Intel、實體 iOS 未原生驗證；硬連結、ACL、完整 OS/runtime/framework 尚有缺口。

```json
{"mutable":true,"mutation_policy":{"mutation_time":{"seconds":-13,"nanoseconds":456}}}
```

## 目錄與符號連結根交易

`renameatx_np(RENAME_SWAP)` 接受實際目錄與符號連結雙向交換，包含非空子樹。初始目錄需要 `exchangeable`，初始連結需要 `mutable`，雙方實際父目錄仍須在已確立的同一 mount 中有修改/SWAP 授權；受保護初始後代仍拒絕。在既有一般移動授權下，目錄到連結為 ENOTDIR20，反向為 EISDIR21，EXCL 對不同既有名稱為 EEXIST17。目錄與自身後代連結的兩個方向均在變更前回傳 EINVAL22。成功交換可能使連結自我引用，後續跟隨為 ELOOP62。

既有三張名稱表的交易先檢查根、已連結或持有的後代、完整路徑及動態空間。初始名稱/目標/引用、檔案內容及映射租約不提供 SWAP 抵扣；首次初始名稱重鍵另計動態路徑/NUL，重複交換只替換舊費用。不消耗新條目、FD 或建立 inode。成員關係遵循實際父物件，同名舊孤立物件不加入新樹。原始目標不變，相對查找重新綁定父目錄；FD/dup 游標、CWD、目標與映射保持有效。直接根按各自 stat 策略只改 ctime，祖先移動保留後代記錄；省略策略保留未知完整 stat，但即時列舉仍可用已知 inode。相關列舉版本按既有歸零重讀契約失效，沒有新 JSON 欄位或權限。

原創 ARM64 私有準備保存 36 個保護式 144 位元組 stat 視圖、16 次原始 rename，維持 compile120s/native5s/drain1s/reap1s、回收及清理。`directory-link-roots` 與 `virtual-directory-link-roots` 經五種 guest、C/CLI、Python 檢查原生身分與完整常量 stat；兩種頁模型涵蓋精確費用、未開啟後代溢出、孤立物件及 FD/條目/inode 耗盡。本節在上述授權內擴充前文限制；Intel 原生、實體 iOS、硬連結、ACL 與完整 OS/runtime/framework 仍待驗證或實作。

## 固定核心 pathconf 查詢

原始 pathconf(191) / fpathconf(192) 支援固定 XNU vnode 查詢：15/16/17→1，19/25→0，20/22/23→4096，21→65536，24→255。這些查詢涵蓋符號連結、分配與 I/O 宣告及傳輸建議；不啟用非同步執行或授權，也不推測頁面或目錄預算的檔案系統含義。

完整路徑跟隨解析與 FD 查找先於低32位選擇器；保留 CWD、符號連結與 EFAULT/ENOENT/ENOTDIR/ELOOP，無效/關閉 FD 為 EBADF。一般檔案與目錄的保留物件跨 dup、重新命名、刪除及名稱重用仍可查詢，無須完整 stat。輸入/擷取描述符原生類型未知，明確停止。沿用 BSD int/進位/次要暫存器規則；查詢不複製輸出、不改游標或中繼資料/列舉，不配置條目/FD/inode。NAME_MAX、大小寫屬性及未知選擇器仍不支援；不借用宿主或猜測 EINVAL。

kernel-pathconf、kernel-pathconf-values、kernel-pathconf-unsupported 檢查共同語義、獨立80位元組值及保留既有輸出的停止報告，涵蓋來賓/C/CLI/Python。私人 ARM64 原生準備完成250次查詢，249次對照 SDK，0.262秒且5秒期限不變。原生 Intel、實體 iOS、ACL、硬連結與完整執行階段/框架仍未驗證或未實作。

[XNU vn_pathconf](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_vnops.c), [XNU pathconf](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU fpathconf](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/kern/kern_descrip.c).

`pathconf`: `file/` → ENOTDIR; `alias/`, `alias//` → 1 (selector15); `alias/.`, `alias/child` → ENOTDIR.

## 固定共通屬性列表

getattrlist(220)、fgetattrlist(228)、getattrlistat(476) 查詢明確目錄中的十一項固定共通屬性：裝置、物件類型、四種時間、擁有者/群組、完整模式、旗標及檔案 ID。與 stat64 共用完整中繼資料的有效性判斷；缺失或失效的觀察維持未知。類型與空選擇不需 stat。根/掛載 NAME、卷宗、目錄/檔案/fork 專用屬性、ACL 及未知選項明確不支援，即使要求回傳遮罩也不省略未知資訊；不推斷主機資料或掛載名稱。

路徑/at 入口先匯入 24 位元組請求，FD 入口先檢查 low32 FD 與原生類型，忽略 reserved 字。共用 CWD、相對 FD、連結解析，在大小/位圖檢查前保留原生錯誤。記錄採小端、四位元組對齊、完整 st_mode 與有號秒數；含回傳遮罩為 120 位元組，普通記錄為 100。短緩衝區僅接收指定前綴，長度仍報告完整需求。部分可存取的複製在該次複製前停止；超出有號 uio 大小只於有效且受支援的請求後回傳 EINVAL。查詢不改變游標、資料、列舉或項目/FD/inode 預算，持有物件沿用 dup、刪除、名稱重用生命週期。

三個模式經 guest、C、CLI、Python 檢查原生共通行為、獨立設定位元組與保留既有輸出的未知 ATTR_CMN_EXTENDED_SECURITY。ARM64 私有準備通過 187 次原始查詢及 176 次路徑/FD SDK 對照，原生 5 秒限制不變；SDK15.5 未宣告 getattrlistat，raw476 另記。新增 guest/Python 保持 5,000,000us/quantum1024，既有公開測試保持 10s。原生 Intel、實體 iOS、檔案系統專用資訊、硬連結、權限/ACL 與完整執行階段/框架仍未驗證或未完成。

```text
ATTR_CMN_RETURNED_ATTRS=0x80000000
FSOPT_NOFOLLOW=1, FSOPT_REPORT_FULLSIZE=4
FSOPT_PACK_INVAL_ATTRS=8 (requires ATTR_CMN_RETURNED_ATTRS)
FSOPT_NOFOLLOW_ANY=0x800
common-attributes
common-attributes-values
common-attributes-unsupported
```

[XNU attrlist](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_attrlist.c), [XNU attr.h](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/sys/attr.h).


## 明確的延伸屬性讀取

getxattr(234)、fgetxattr(235)、listxattr(240)、flistxattr(241) 讀取 darwin_files 中完整、有序的一般屬性觀察值。檔案、目錄、連結可提供 extended_attributes 陣列，嚴格使用 {name,bytes_hex}。省略代表未知，[] 代表已知空清單。名稱是 1..127 位元組 UTF-8，可含斜線；值為不透明位元組。拒絕重複名稱並保留順序。最多 4096 個屬性，名稱加 NUL 與值計入原有 16 MiB 預算，不重複計算既有物件路徑。

屬性隨物件經歷 dup、移動、刪除、CWD 與名稱重用，獨立於完整 stat 與目錄列舉的有效性。新物件未知；內容寫入、成功截斷與不明確的非空複製失敗使屬性未知，複製前拒絕部分緩衝區則保留觀察值。查詢不改變偏移、輸入、中繼資料或條目/FD/inode 預算。

ABI 使用低32位 FD/options/position、完整64位 size 與 BSD user_ssize_t/carry/secondary。NULL 查詢忽略 position；非空值的路徑介面在非NULL size0 時回傳 ERANGE，FD size0 查詢長度。只有路徑 get 的 UINT32_MAX/UINT64_MAX 是相容長度查詢；FD get 限制為 INT32_MAX。正長度短清單可先寫完整名稱前綴再回傳 ERANGE；非空清單的非NULL負64位長度回傳 ERANGE。未取得原生空清單證據，因此宣告為空的負長度清單仍為 UnsupportedService。輸出無法完整寫入時在複製前停止。NOFOLLOW1 與 NOFOLLOW_ANY64 獨立；8/16 在查找前拒絕，FD1/64 在 FD/名稱前拒絕。CREATE2/REPLACE4 在讀取中忽略；SHOWCOMPRESSION32 與未知位不支援。受保護的 com.apple.system.*、ResourceFork、FinderInfo、decmpfs、設定/刪除、權限/ACL 與檔案系統推測不在讀取契約內。

extended-attributes / extended-attributes-values / extended-attributes-unsupported 分別驗證原生共用行為、虛擬字面值及保留先前輸出的未知觀察停止。guest/Python5,000,000us/quantum1024、公開介面10s、原生5s 不變。ARM64 私有準備完成320組 raw/SDK 對照，全部288位元組保護區、carry、secondary 一致。自動 com.apple.provenance 是觀察值，不能推測預設空清單。Intel 原生與 iOS 真機未驗證；完整 dyld、Mach IPC、Objective-C/Swift 與框架仍未完成。

Sources: [XNU syscall ABI](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/kern/syscalls.master), [XNU xattr calls](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU ordinary attributes](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_xattr.c), [XNU xattr definitions](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/sys/xattr.h). Original code/probes; Apple implementation is not copied.

## 有界物件名稱

ATTR_CMN_NAME=1 經 getattrlist220/fgetattrlist228/getattrlistat476 支援明確目錄中名稱唯一的非根物件。葉名稱須為 1..255 位元組合法 UTF-8。與 F_GETPATH 共用的實際物件路徑在 dup、CWD、移動、SWAP、刪除與名稱重用後保留最後連結的拼寫；呼叫者別名不覆蓋它。名稱與類型不依賴 stat；所選 stat 欄位仍需完整有效觀察。根/掛載標籤、非法名稱、硬連結或大小寫別名、正規化與完整路徑屬性保持未知。

8 位元組 attrreference_t 位於其他共通欄位之前；attr_dataoffset 相對於引用本身，attr_length 包含 NUL，末尾名稱區域按四位元組填補。短輸出保留完整所需長度及精確前綴，包括截斷的 UTF-8。attribute-names / attribute-names-values / attribute-names-unsupported 經 guest/C/CLI/Python 檢查原生行為、獨立位元組及保留既有輸出的根名稱停止。ARM64 私有對照通過 601 次原始查詢、453 次完整保護緩衝區 SDK 比較與 384 次前綴檢查。SDK15.5 無 raw476 類型宣告。native5s、guest/Python5,000,000us/quantum1024、既有公開測試10s 不變。原生 Intel、實體 iOS 與完整執行階段/框架仍未驗證或未完成。

## 有界目錄批次屬性

getattrlistbulk(461) 需要明確 enumeration_policy.bulk_attributes=true；省略或 false 不授權。此虛擬 TYPE 契約依無號位元組順序回傳目前直接子項名稱，不含點項目，使用本地序號而非原生檔案系統 cookie。初始目錄物件跨 dup、移動、SWAP、刪除與名稱重用保留授權；新建目錄不繼承批次授權。既有 minimum_buffer_size、initial_minimum_buffer_size、seek_offset 僅用於 getdirentries64。

必須選擇 NAME|OBJTYPE|RETURNED_ATTRS (0x80000009)，選取的觀察仍有效時可用已有十一項共通欄位。支援 Options0/8；bulk 忽略兩個16位元 bitmap/reserved 字，與獨立 attrlist 驗證分離。唯一屬性編碼器共用 attrreference_t 與 stat64 有效性。只回傳完整紀錄：放得下時依8位元組填補，否則允許最後一組為4位元組大小。首組放不下時回傳 ERANGE，輸出與游標不變；所需輸出僅部分可寫時在複製前明確停止。只有實際回傳的位元組需要可寫記憶體。

dup 共用進度，獨立 open 各自前進。非零已完成遍歷在命名空間變化後仍保留 EOF，在請求驗證後略過大小與輸出檢查；初始空目錄 offset0 重新檢查檢視。零 lseek 重設迭代；EOF 前成員變化、任意非零 seek、混用 getdirentries64/bulk 明確停止。NAME-only 回退、含 ERROR 的項目、快照、ACL/權限判斷、主機順序及其他 mask/options 不支援。bulk-attributes / bulk-attributes-values / bulk-attributes-unsupported 檢查原生共同行為、虛擬字面位元組及保留既有輸出的未知選擇停止。ARM64 私有準備通過728組含保護區的 raw/SDK 比較。native5s、guest/Python5,000,000us/quantum1024、public10s 不變。原生 Intel、iOS 真機及完整執行期/框架仍未驗證或未完成。

Sources: [XNU bulk ABI](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/man/man2/getattrlistbulk.2), [XNU attribute definitions](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/sys/attr.h). Original implementation and probes; no Apple implementation copied.

## 明確授權的普通延伸屬性修改

setxattr(236)、fsetxattr(237)、removexattr(238) 與 fremovexattr(239) 使用針對初始物件的獨立授權：C++ `MutableExtendedAttributes`，嚴格布林 JSON `mutable_extended_attributes=true`。獲授權的檔案、目錄或符號連結必須宣告完整的普通 `extended_attributes` 清單，也可宣告已知空清單。內容可寫或名稱空間可修改不會授予此權限。授權和值隨保留的物件經過 dup、移動、刪除及映射租約繼續存在；新建物件及重用名稱的屬性起初未知。已知別名、衝突的中繼資料旗標、受保護的系統屬性、ResourceFork、FinderInfo 與壓縮語義仍不支援。

取代保留虛擬清單位置，刪除移除該項，新建附加至末尾。這是宣告的行程內順序，不推測 APFS 順序。初始屬性的位元組、數量及授權路徑參照始終預留。執行期超出初始預留的部分由既有 16 MiB/4096 限額統一管理；刪除、內容失效或最終釋放物件只回收這部分增量。容量、傳輸或截止時間失敗不會發布暫存狀態。必要輸入完全不可讀時傳回 EFAULT；部分可讀時在發布前明確報告不支援。修改成功使完整 stat 失效，不猜測時間，同時保留物件身分、目錄成員、列舉版本/快照與游標。

修改 ABI 使用 low32 FD/options/position 及 full64 size。特權與 FD 連結選項的早期檢查先於名稱匯入，名稱匯入先於物件查找。set 在檢查過大的 VFS 輸入（E2BIG7）前拒絕非空長度的 NULL；查找先於普通名稱、position 及衝突檢查。set 先匯入完整值，再傳回 CREATE 已存在的 EEXIST17 或 REPLACE 不存在的 ENOATTR93。CREATE 與 REPLACE 同時設定傳回 EINVAL；刪除操作忽略這兩個位元。長度為零的 set 不讀取值指標。其他旗標、未知權限及未觀察的提供者行為均明確停止。

xattr-mutations / xattr-mutations-values / xattr-mutations-unsupported 使用原創原生/客體對照、獨立虛擬位元組常值，以及保留既有輸出的缺少授權停止案例。ARM64 私有準備驗證了726次 raw/SDK 呼叫、完整544位元組保護區觀察及完整可讀頁。native5s/compile120s/drain1s/reap1s、guest/Python5,000,000us/quantum1024、public10s 均不變。原生 Intel、iOS 實機、dyld、Mach IPC、執行緒/訊號、Objective-C/Swift 執行環境及完整框架仍未驗證或未完成。

主要 ABI 參考：[XNU 系統呼叫宣告](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/kern/syscalls.master)、[xattr 定義](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/sys/xattr.h)。實作與探針均為原創，未複製 Apple 實作。

## 有界 Darwin 硬連結

原始 link 跟隨最終符號連結目標；linkat 的 flags=0 連結符號連結物件本身，AT_SYMLINK_FOLLOW 跟隨目標。僅接受 low32 的 0/0x40，其餘低位在匯入前回傳 EINVAL。來源查找與目錄 EPERM 先於目標匯入；已存在目標回傳 EEXIST。目標父目錄需修改授權且雙方須屬明確建立的同一掛載域。初始身分別名及已知裝置/模式/旗標衝突仍不支援。

別名只消耗名稱項目及路徑/NUL 費用，不消耗新 inode；位元組、屬性授權及映射租約由共享物件持有。既有明確策略更新連結數與 ctime；缺少策略時完整 stat 未知。屬性修改使完整 stat 失效，內容修改使屬性觀察失效。描述物件及最後映射保留刪除名稱的費用；替換只抵扣可立即釋放的費用。子樹依確切身分及父目錄移動，樹外別名留在原位；相對符號目標使用所選項目的父目錄。

曾有多個名稱的物件，即使剩一個或零個名稱，F_GETPATH/ATTR_CMN_NAME 仍不支援；APFS 快取尚無通用模型。批量 NAME 來自實際項目，相同物件的普通重新命名/SWAP 保留雙方。EXCL 大小寫、原生 Intel HVF、實體 iOS、ACL、映射一致性/EOF 訊號、dyld、Mach IPC、執行緒與完整框架仍是缺口。本節僅在上述契約內擴展前文限制。

```text
link(9), linkat(471), AT_SYMLINK_FOLLOW=0x40
DarwinFiles, FileEntry, LinkEntry, NameIdentity, Contents, LinkNode
LinkedNames, HadMultipleNames, DetachedNames
F_GETPATH, ATTR_CMN_NAME, getattrlist(220), fgetattrlist(228), getattrlistat(476)
getattrlistbulk(461), O_SYMLINK
hard-links
hard-links-values
hard-links-name-unsupported
hard-links-attributes-unsupported
HardLink*, HardLinksShareObjectsAndRetainExplicitNameBoundary
native5s / compile120s / drain1s / reap1s
guest/Python5,000,000us / quantum1024 / public10s
34 model cases / 20 guest cases / 20 public cases / 5 Python profiles
66 mandatory workloads per platform / ARM64 198 / Intel 132
```

## 有界 O_SYMLINK 描述符

O_SYMLINK=0x00200000 在唯讀、唯寫和讀寫模式下保留最後的符號連結物件，包括懸空或循環連結；它不授予目標內容寫入權限。O_CREAT 仍跟隨最後目標；NOFOLLOW 保持 ELOOP 優先順序，獨占建立保持 EEXIST，O_DIRECTORY 對保留的連結傳回 ENOTDIR。必要的中間路徑或結尾斜線展開沿用現有解析器及 NOFOLLOW_ANY 邊界。F_GETFL 不傳回物件選擇位元。

描述物件共用實際 LinkNode 與所選 NameIdentity。dup 共用狀態旗標與游標，獨立 open 使用各自的描述物件。重新命名、刪除、名稱重用或刪除父目錄後，既有 FD 仍保留原物件。唯一名稱的 F_GETPATH 與 ATTR_CMN_NAME 使用保留的所選名稱；曾有多個名稱的物件，即使名稱全部刪除，仍明確拒絕 vnode 名稱推斷。初始固定費用持續保留；動態名稱、目標、項目及屬性增量等待實際最後持有者釋放。其他描述物件仍持有物件或所選名稱時，取代不能抵扣最後別名的費用。

符號描述符 I/O 不將原始目標字串視為檔案內容。既有純量/向量匯入、存取及數量檢查後，負位移傳回 EINVAL。INT64_MAX 的讀取傳回零，其他允許位移傳回 EPERM，包含零長度要求。INT64_MAX 的寫入傳回 EFBIG，其餘在零長度成功、APPEND 或資料存取前傳回 EPERM。既有 pwrite/pwritev 負位移的早期規則仍是唯一依據。DATA/HOLE seek 對非負位置傳回 ENXIO，負位置傳回 EINVAL，游標不變。

唯寫/讀寫描述物件的非負 ftruncate 與允許的 open TRUNC 只設定 WasWritten，不改變目標位元組、完整 stat、擴充屬性、游標、儲存准入或 inode 分配。唯讀 ftruncate 及負長度傳回 EINVAL。允許參數的 F_SETFL 先改變 APPEND|NONBLOCK，再傳回 ENOTTY25；dup 可觀察變化，獨立開啟不受影響。未知參數在產生效果前停止。

固定 fpathconf、fgetattrlist 及獨立宣告的普通 FD 擴充屬性權限作用於符號物件。相對目錄 FD 查找與 fchdir 傳回 ENOTDIR。屬性修改仍使 stat 失效，截斷不會恢復中繼資料。舊式、位移對齊、不可執行的 private/shared mmap 選擇到達符號物件類型的 EINVAL 拒絕，不建立映射或租約。普通 shared 映射、未知旗標、執行權限及其他現有不支援邊界保持不變。原生映射對照僅涵蓋 length16384、offset0、protection1/2/3，並未驗證所有 mmap 變體。

原創 ARM64 準備對15項截斷及隔離的物件選擇保留完整144位元組 stat 與保護區比較。O0/O2 觀察涵蓋純量/向量極端位移與數量、稀疏 seek、18項映射拒絕及失敗 F_SETFL 的副作用。無 SDK 共用程式另以 O0/O1/O2 編譯，比較描述符實際父目錄路徑，避免原生暫存目錄的拼寫別名。虛擬模式比較獨立的完整 stat/type 字面值，或在多名稱查詢時明確停止並保留輸出。原生 Intel HVF、iOS 實機、ACL/權限執行、映射 EOF/訊號一致性、dyld、Mach IPC、執行緒與完整執行時期/框架仍未驗證或未完成。

主要解釋依據：[對應版本 XNU 映射邊界](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/kern/kern_mman.c)。實作與探針皆為原創，未複製 Apple 實作。

```text
O_SYMLINK=0x00200000; ENOTTY=25
open O_RDONLY|O_SYMLINK|O_TRUNC: WasWritten only
LinkNode, NameIdentity, HadMultipleNames, DetachedNames
INT64_MAX read=0 / write=EFBIG; other admitted offsets=EPERM
SEEK_DATA/SEEK_HOLE nonnegative=ENXIO / negative=EINVAL
F_SETFL: APPEND|NONBLOCK effect before ENOTTY; dup shares / independent open separate
symbolic-descriptors
symbolic-descriptors-values
symbolic-descriptors-name-unsupported
SymbolicDescriptor*, SymbolicDescriptorsRetainObjectsAndNativeErrorOrder
native5s / compile120s / drain1s / reap1s
guest/Python5,000,000us / quantum1024 / public10s
34 model cases / 20 transport parameters / 15 public cases / 5 Python profiles
66 mandatory workloads per platform / ARM64 198 / Intel 132
```

## 有界的非阻塞描述符狀態

一般檔案、目錄及 O_SYMLINK 開啟允許 O_NONBLOCK=4，F_GETFL 保留此狀態。dup 共用狀態與游標，獨立開啟保留各自的描述。既有存取模式、close-on-exec、WasWritten、中繼資料、位元組與游標規則仍適用。明確有限標準輸入保留 EOF 及指標錯誤順序；省略輸入仍屬未知。輸出擷取保留複製錯誤與共用輸出預算。

F_SETFL 在效果前檢查低32位允許參數，再依原生開啟旗標轉換加一，只改變 APPEND|NONBLOCK。高32位忽略；存取及輸入 WasWritten 位不能授權或偽造寫入。原生字面控制的參數3/7/11/15選擇狀態4/8/12/0。符號描述先提交狀態變更，再傳回 ENOTTY25。ASYNC0x40 等未知旗標在效果前停止。

原創 ARM64 macOS 準備記錄122項觀察，包含每個有效物件/存取組合的16項低位請求、保留 dup、獨立開啟、清除狀態、實際寫入及每個 FD 的 CLOEXEC。無 SDK 共用程式於 O0/O1/O2 執行，比較位元組、狀態、游標及原始 BSD carry/errno ABI。來賓、C/CLI 與 Python 路徑也要求未知旗標拒絕，三個 ARM64 HVF profile 均須實際執行。不提供就緒等待、管道、網路、kqueue、非同步訊號或宿主 I/O。O_EVTONLY 程序策略仍不支援；原生 Intel HVF、iOS 實機及完整 macOS/iOS 環境仍未驗證或未完成。

```text
O_NONBLOCK=4; F_SETFL raw low32 mask=0x1000f
requests3/7/11/15 -> APPEND|NONBLOCK status4/8/12/0
nonblocking-descriptors / nonblocking-flags-unsupported
Nonblocking*, NonblockingDescriptorsKeepNativeControlState
native5s / compile120s / drain1s / reap1s
guest/Python5,000,000us / quantum1024 / public10s
8 model cases / 20 transport parameters / 10 public cases / 5 Python profiles
67 mandatory workloads per platform / ARM64 201 / Intel 134
```

## 明確有限 getentropy 觀測

原始 BSD getentropy500 使用有序 `DarwinSystemOptions::EntropyReads` 觀測佇列，JSON 欄位為 `darwin_system.entropy_reads`。每筆必須是非空、偶數長度的十六進位字串；最多256筆，每筆1..256位元組。這是模型限制，既有65536位元組 JSON 傳輸上限不變。省略表示未知，`[]` 表示明確耗盡。原生選項與 JSON 在載入映像或變更後端前嚴格驗證；其他 OS 設定拒絕 Darwin 選項。

先檢查完整64位元長度：超過256回傳 EINVAL22且不存取記憶體或消耗記錄；零長度對任何指標成功且不需要輸入。非零請求先接受下一筆長度完全相符的記錄，再複製。缺少、耗盡或長度不符均以 UnsupportedService 在效果之前停止，包括無效位址；這是重播准入順序。成功複製或完全無法寫入的 EFAULT14消耗一筆；部分可寫目的地在複製與游標更新前拒絕，記憶體傳輸錯誤不前進。後續回傳暫存器失敗保留已完成效果。相同選項每次執行都從第一筆開始。

每次執行的 DarwinEntropy 獨立持有游標，輸入位元組保持不變。既有 BSD 分派與 returnService統一處理兩種 ISA、進位及次要暫存器。無 SDK 重播程式覆蓋五種客體與三種 ARM64 HVF設定；固定字節不加入原生確定性 RNG清單。原始 ARM64 O0/O1/O2探針有594次呼叫；哨兵變化數不代表準確複製長度。未提供宿主隨機源、密碼學品質、/dev/random、libc匯入或框架；Intel HVF、iOS實機與完整 OS相容性仍待驗證或完成。

```text
BSD getentropy500 / DarwinEntropy / EntropyReads / darwin_system.entropy_reads
full64 size>256 -> EINVAL22; zero ->0; whole EFAULT14 consumes one record
1..256 bytes per record / at most256 records / JSON transport65536 bytes
entropy-replay / entropy-missing / entropy-exhausted / entropy-mismatch / entropy-partial
15 model cases / 20 transport parameters / 26 public cases / 5 Python profiles
68 mandatory workloads per platform / ARM64 204 / Intel 136
native5s / compile120s / drain1s / reap1s
owner/build1200s / guest/Python5,000,000us / quantum1024 / public10s
```

[XNU getentropy ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/syscalls.master), [XNU generation/copyout boundary](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/dev/random/randomdev.c).

## 明確的目前執行緒身分

原始 BSD thread_selfid372 讀取不可變的選用 `DarwinSystemOptions::ThreadID`，JSON 欄位為 `darwin_system.thread_id`。所有 uint64 位元模式（包含零）都是已知觀測；省略時以 UnsupportedService 停止。十進位字串保留完整64位元，JSON 數值僅接受不超過2^53-1的精確整數。不從主機、PID 或 Mach 埠推論 ID。無參數呼叫忽略六個參數載體且不存取記憶體；既有低32位元解析保留事件中的完整原始編號，BSD 回傳層保留64位元結果並清除 carry 與 RDX/X1。Mach 編號仍不支援。

重複執行保留輸入觀測，不同選項互相獨立；不配置 ID、不保證唯一性、不寫入排程事件身分，也不實作執行緒生命週期、pthread、TLS 或 Mach IPC。原始 ARM64 O0/O1/O2 探針保留24次呼叫，與 SDK 目前 pthread ID 比較並檢查任意參數及編號高32位元。原生通用程式只比較同一程序內的關係；明確 ID 位元組不加入確定性原生參考清單。Intel HVF、iOS 實機與完整 OS 相容性仍未驗證或未完成。

```text
BSD thread_selfid372 / Wide / ThreadID / darwin_system.thread_id
known uint64 including0 / missing -> UnsupportedService / no memory
full64 return / low32 resolution / carry clear / RDX-X1 zero / raw event number
thread-identity / thread-identity-value / thread-identity-missing
4 model cases / 20 transport parameters / 16 public cases / 5 Python profiles
69 mandatory workloads per platform / ARM64 207 / Intel 138 unverified
original ARM64 O0/O1/O2 probes24 / native5s / compile120s / drain1s / reap1s
owner/build1200s / guest/Python5,000,000us / quantum1024 / public10s
```

[XNU thread_selfid ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/syscalls.master), [libpthread current-thread owner](https://github.com/apple-oss-distributions/libpthread/blob/42d026df5b07825070f60134b980a1ec2552dfee/kern/kern_support.c).

## 顯式 Mach 自身埠觀測

原始 Mach thread_self_trap27、task_self_trap28 與 host_self_trap29 分別讀取獨立的可選 uint32 觀測：DarwinSystemOptions::ThreadSelfPort、TaskSelfPort、HostSelfPort，對應 darwin_system.thread_self_port、task_self_port、host_self_port。每次查詢只要求自身欄位。缺失表示未知並停止為 UnsupportedService；零、相等名稱及全部32位模式均為顯式值。輸入接受 UINT32_MAX 範圍內的精確整數或十進位字串，不套用 process_group_id/session_id 的正 pid_t 限制。

系統層將名稱經有符號 int32 核心結果轉為原始64位載體：0x80000001 回傳0xffffffff80000001，UINT32_MAX 回傳UINT64_MAX。既有 Mach 綁定保留旗標及 X1/RDX，並保留 x64 RCX/R11 系統呼叫覆寫規則；忽略所有參數且不存取記憶體。低32位解析保留事件中的完整原始號碼。Mach 查詢事件省略 BSD error 欄位，不產生排程器 ThreadID。重複使用選項保留觀測，獨立選項互不影響。

原生 ARM64 O0/O1/O2 探針保留432次原始觀測，涵蓋全部16種 NZCV、高32位號碼前綴、暫存器種子與 SDK 對照。未觀測到原生 bit31 埠名稱；高位符號延伸依據固定 XNU 回傳路徑，並由獨立字面值模型、客體及公共測試檢查。原生共同程式只比較同一行程內的關係；虛擬名稱與缺失輸入不進入原生確定性參考。這些觀測不配置名稱或傳送引用、不認證有效權限、不推斷唯一性，也不實作 IPC、生命週期或執行緒排程。權限/ACL、就緒等待、推進時鐘、真正 Mach IPC/執行緒、dyld/TLS 及完整執行階段/框架仍不完整；原生 Intel HVF 與實體 iOS 尚未驗證。

```json
{"darwin_system":{"thread_self_port":2147483649,"task_self_port":0,"host_self_port":"4294967295"}}
```

```text
Mach thread_self_trap27 / task_self_trap28 / host_self_trap29
ThreadSelfPort / TaskSelfPort / HostSelfPort / uint32 / signed-int32 -> raw64
known0 / missing -> UnsupportedService / no arguments or memory
low32 resolution / complete raw number / flags and RDX-X1 preserved / no BSD error
mach-self-ports / mach-self-port-values / mach-self-port-missing
MachSelfPortsPreserveExplicitBitsAndIndependentRuns
6 model cases / 20 transport parameters / 26 public cases / 5 Python profiles
70 mandatory workloads per platform / ARM64 210 / Intel 140 unverified
original ARM64 O0/O1/O2 probes432 / no native bit31 name observed
native5s / compile120s / drain1s / reap1s
owner/build1200s / guest/Python5,000,000us / quantum1024 / public10s
```

[XNU Mach trap table](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/kern/syscall_sw.c), [self-port name owners](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/kern/ipc_tt.c), [host-port owner](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/kern/ipc_host.c), [ARM64 return](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/bsd_arm64.c), [x64 return](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/i386/bsd_i386.c).


## 靜態普通擁有者權限查詢

darwin_files.authorization="static-owner-queries"（DarwinFileAuthorization::StaticOwnerQueries）明確宣告不可變的普通本機權限環境：無 ACL、MAC、額外 kauth 監聽器、entitlement 或其他繞過；掛載可寫、可執行、非 opaque 且啟用所有權；已知 flags=0，無特殊模式位。舊章節的觀測本身不授權，宣告僅啟用 access/faccessat 查詢。

實際檢查需要明確的 darwin_system.credentials 與實際物件中繼資料。access 使用 real_uid；AT_EACCESS 對 SEARCH 與最終 R/W/X 使用 effective_uid。選取 UID 必須非零且符合擁有者，所有請求位均須允許；已知拒絕回傳 EACCES13 與原有 BSD carry。未知身分/資料、選取 UID0、非擁有者、擴充動作及保留末端連結的 R/W/X 停止 UnsupportedService；未選取 UID0 仍合法。不推測主機、UID1000、群組/其他人權限或 root 豁免。

父目錄 X 位的 SEARCH 先於子項查找、缺失名稱、適用點/點點與連結重啟。F_OK/忽略位僅需實際 SEARCH；純斜線根及根處鉗制點點不做 SEARCH，已消耗尾斜線不增加最終目錄檢查。旗標、複製、相對 dirfd 與空名稱錯誤順序保持。Name255 為模型上限：允許 SEARCH 後超限停止 Unsupported，拒絕先 EACCES，未知先停止，不猜原生 errno。

其他檔案路由，包括 open、stat、chdir、readlink、屬性、列舉與命名空間修改，均在效果前停止。只有真正 Input/Output/Error 標準串流及 dup 別名保留既有 I/O、close、dup/dup2、lseek、fcntl；數字 FD0/1/2 不足以豁免。檔案 mmap/mappingSource 關閉，匿名記憶體獨立。共享 C++/JSON 准入拒絕修改/建立授權、已知 flags/特殊位與 inode/device 別名，維持未知資料及256項/16MiB計費。

範例同時提供根 SEARCH 與0400檔案擁有者：讀檢查成功、寫檢查 EACCES，但不能開啟檔案。原生 ARM64 O0/O1/O2 保留2472 raw/SDK組、2439字面值檢查、33僅捕获，compile120s/native5s不變。fstatx/filesec獨立確認 ACL 缺省；最初 NULL/ENOENT 誤判尚未執行權限查詢，記錄保留。主機 real/effective 相同，全域安全鉤子與 opaque 內部未獨立證實；不同身分依据固定原始碼/模型。owner-queries 排除於58個原生確定性參考。群組/root/ACL/MAC、動態身分、一般 vnode 授權、就緒、時鐘、Mach IPC/執行緒、dyld/TLS與完整框架仍有缺口；原生 Intel HVF 與實體 iOS 未驗證。

```json
{"darwin_files":{"authorization":"static-owner-queries","files":[{"path":"/data","bytes_hex":"00","metadata":{"device":7,"inode":2,"mode":33024,"link_count":1,"uid":501,"gid":20,"size":1,"block_size":4096,"blocks":0,"flags":0,"generation":0,"access_time":{"seconds":0,"nanoseconds":0},"modification_time":{"seconds":0,"nanoseconds":0},"change_time":{"seconds":0,"nanoseconds":0},"birth_time":{"seconds":0,"nanoseconds":0}}}],"directories":[{"path":"/","metadata":{"device":7,"inode":1,"mode":16832,"link_count":2,"uid":501,"gid":20,"size":0,"block_size":4096,"blocks":0,"flags":0,"generation":0,"access_time":{"seconds":0,"nanoseconds":0},"modification_time":{"seconds":0,"nanoseconds":0},"change_time":{"seconds":0,"nanoseconds":0},"birth_time":{"seconds":0,"nanoseconds":0}}}],"working_directory":"/"},"darwin_system":{"credentials":{"real_uid":501,"effective_uid":501,"real_gid":20,"effective_gid":20}}}
```

```text
DarwinFileAuthorization::StaticOwnerQueries / authorization=static-owner-queries
access33 / faccessat466 / real_uid / effective_uid / AT_EACCESS0x10
owner R/W/X / all requested bits / directory SEARCH / EACCES13
no-action root LOOKUP / root-clamped dotdot / consumed terminal separators
unknown credentials-metadata-root-nonowner -> UnsupportedService
all other vnode routes closed / typed standard streams and dup aliases only
anonymous memory independent / file-backed mmap and mappingSource closed
Name255 availability stop after allowed SEARCH / no guessed filesystem errno
owner-queries / owner-query-stop / owner-query-open / owner-query-map
OwnerQueriesKeepPermissionAndUnknownBoundaries
73 mandatory workloads per platform / ARM64 219 / Intel 146 unverified
original ARM64 O0/O1/O2 pairs2472 / literal2439 / capture-only33
native5s / compile120s / owner-build1200s / guest-Python5,000,000us
```
[XNU access and subject selection](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [real credential copy](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_credential.c), [owner authorization](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_subr.c), [pathname SEARCH](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_lookup.c), [cached lookup](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_cache.c), [Libc ACL properties](https://github.com/apple-oss-distributions/Libc/blob/Libc-1698.140.3/gen/filesec.c), [fstatx ACL absence](https://github.com/apple-oss-distributions/Libc/blob/Libc-1698.140.3/sys/statx_np.c).

## 群組身分部分已知時的靜態普通查詢

`darwin_files.authorization="static-ordinary-queries"`（DarwinFileAuthorization::StaticOrdinaryQueries）沿用前節不可變普通環境的掛載、安全及中繼資料約束，以及預設關閉檔案操作、標準串流例外和獨立匿名記憶體規則。舊 static-owner-queries 仍限於擁有者。實際檢查仍需明確憑據、中繼資料和非零選定 UID；root、延伸權限及最終連結物件的 R/W/X 仍不支援。

擁有者使用全部請求的擁有者權限位元。非擁有者比較群組與其他使用者對完整請求遮罩的結果：相同結果可直接允許或回傳 EACCES13；不同位元集合也可能同樣拒絕。結果不同時，已知成員選群組，已證實非成員選其他使用者；未知身分在查找或效果前停止 UnsupportedService，不合併不同類別。

credentials.groups 是有序的核心憑據群組清單，第0項為 EffectiveGID，保留重複項；不是 SDK getgroups 的延伸解析器清單。選定主群組及明確的正向成員已知，但缺少項目通常不能證明非成員，省略清單也不能。真實身分在兩對 UID/GID 相同時保留原上下文；否則將第0項換為 RealGID，並把舊 EffectiveGID 換入第一個匹配的補充 RealGID 項。無匹配時移除舊主群組並停用 memberd；已證實的轉換或明確原始 KAUTH_UID_NONE，加上明確完整清單，才能證明缺失群組為非成員。UID 不同而 GID 相同仍轉換，重複主群組可保留未知外部成員身分。AT_EACCESS 使用原有效上下文，輸入不變。

下例真實查詢因移除 GID20 而拒絕；AT_EACCESS 因已知有效主群組20而成功。目錄 SEARCH 由群組與其他使用者結果一致確定；不授權選定 UID0 或開啟檔案。

```json
{"darwin_files":{"authorization":"static-ordinary-queries","files":[{"path":"/data","metadata":{"device":7,"inode":2,"mode":32816,"link_count":1,"uid":700,"gid":20,"size":1,"block_size":4096,"blocks":0,"flags":0,"generation":0,"access_time":{"seconds":0,"nanoseconds":0},"modification_time":{"seconds":0,"nanoseconds":0},"change_time":{"seconds":0,"nanoseconds":0},"birth_time":{"seconds":0,"nanoseconds":0}},"bytes_hex":"00"}],"directories":[{"path":"/","metadata":{"device":7,"inode":1,"mode":16895,"link_count":2,"uid":0,"gid":0,"size":0,"block_size":4096,"blocks":0,"flags":0,"generation":0,"access_time":{"seconds":0,"nanoseconds":0},"modification_time":{"seconds":0,"nanoseconds":0},"change_time":{"seconds":0,"nanoseconds":0},"birth_time":{"seconds":0,"nanoseconds":0}}}],"working_directory":"/"},"darwin_system":{"credentials":{"real_uid":501,"effective_uid":502,"real_gid":30,"effective_gid":20,"groups":[20,40]}}}
```

原生 ARM64 的 O0/O1/O2 唯讀驗證保留270對原始／SDK呼叫，檢查非自有物件、SEARCH、錯誤、物件身分及獨立 ACL 屬性缺失；原始憑據群組16項，SDK延伸清單17項。初次因錯誤限制 SDK 清單長度，在權限查詢前停止並保留失敗記錄。真實／有效身分相同，不同身分與完整群組轉換依固定 XNU 原始碼及獨立模型，原生結果不證明所有外部安全鉤子。五種軟體和三種 ARM64 HVF設定檢查實際來賓、C/CLI/Python及未知憑據、資料、root和群組。此供應模型不加入原58項原生共同基準。完整群組解析、root、ACL/MAC、一般 vnode、動態憑據、就緒／網路、推進時鐘、Mach IPC／執行緒、dyld/TLS及完整框架仍未完成；Intel原生HVF及實體iOS未驗證，期限不變。

```text
DarwinFileAuthorization::StaticOrdinaryQueries / authorization=static-ordinary-queries
owner bits / whole-mask group-world outcomes / EACCES13
credentials.groups / in-credential16 / EffectiveGID index0 / duplicates retained
real credential copy / first supplementary match / displacement disables memberd
missing membership usually unknown / original NONE or displaced real plus complete list proves negatives
all43 other file routes and direct/file-backed mappings closed / typed streams only
ordinary-queries / ordinary-query-unknown / ordinary-query-open / ordinary-query-map
OrdinaryQueriesPreserveGroupKnowledgeAndSelectedSearch
73 mandatory workloads per platform / ARM64 219 / Intel 146 unverified
original ARM64 O0/O1/O2 nonowner pairs270 / raw-groups16 / SDK-extended-groups17
native5s / compile120s / guest-Python5,000,000us / quantum1024 / public10s
```

[XNU ordinary mode authorization](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_subr.c), [real credential and group membership](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_credential.c), [raw in-credential getgroups](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_prot.c), [SDK extended getgroups](https://github.com/apple-oss-distributions/Libc/blob/Libc-1698.140.3/sys/getgroups.c).

## 明確原始群組成員上下文

可選 `DarwinCredentials::GroupMembershipUID` / `darwin_system.credentials.group_membership_uid` 宣告原始 cr_gmuid，獨立於四個身分 ID 及 groups。允許0..INT32_MAX或恰為 KAUTH_UID_NONE=4294967195（0xffffff9b，UINT32_MAX減100）。沿用無損十進位字串與精確數值整數解碼；錯誤型別、小數、負數及其他越界值在載入前拒絕。哨兵仍不能用於一般 UID/GID 或群組清單。省略代表外部成員上下文未知，其他有效 UID 也不啟用解析器。

選定主群組與清單中的正向成員優先確定。原始 NONE 加明確完整清單才能證明缺失項為非成員；省略清單仍未知。真實憑據轉換即使匹配首個補充群組並保留舊有效主群組，也保留原始 NONE；已證實的移除同樣停用外部解析。輸入不改變 getuid/geteuid/getgid/getegid、原始 getgroups、建立所有權或舊授權模式。同一普通查詢所有者選擇其他使用者權限，先檢查目錄 SEARCH 再查找子項；其他 vnode 操作保持關閉。

下例宣告呼叫者不是 GID50 成員：兩種身分可讀 /data，寫入權限查詢回傳 EACCES13。省略 group_membership_uid 後，群組與其他使用者結果不同的查詢仍不支援。

```json
{"darwin_files":{"authorization":"static-ordinary-queries","files":[{"path":"/data","bytes_hex":"00","metadata":{"device":7,"inode":2,"mode":32772,"link_count":1,"uid":700,"gid":50,"size":1,"block_size":4096,"blocks":0,"flags":0,"generation":0,"access_time":{"seconds":0,"nanoseconds":0},"modification_time":{"seconds":0,"nanoseconds":0},"change_time":{"seconds":0,"nanoseconds":0},"birth_time":{"seconds":0,"nanoseconds":0}}}],"directories":[{"path":"/","metadata":{"device":7,"inode":1,"mode":16895,"link_count":2,"uid":0,"gid":0,"size":0,"block_size":4096,"blocks":0,"flags":0,"generation":0,"access_time":{"seconds":0,"nanoseconds":0},"modification_time":{"seconds":0,"nanoseconds":0},"change_time":{"seconds":0,"nanoseconds":0},"birth_time":{"seconds":0,"nanoseconds":0}}}],"working_directory":"/"},"darwin_system":{"credentials":{"real_uid":501,"effective_uid":501,"real_gid":20,"effective_gid":20,"groups":[20],"group_membership_uid":4294967195}}}
```

本機 O0/O1/O2 SDK 執行僅驗證哨兵與四位元組 uid_t，不觀察宿主 cr_gmuid、不停用宿主解析器，也不取代先前270對實際 raw/SDK 非所有者觀察。明確 ordinary-queries-closed-groups 用例檢查173個事件，包含有效非成員拒絕及缺失子項、點、點點和連結的搜尋許可/拒絕，涵蓋五個軟體設定、三個必需 ARM64 HVF 設定與 C/CLI/Python 輸入驗證；不加入58個 native-common 確定性參考。root、ACL/MAC、完整群組解析、通用 vnode 授權、動態憑據、就緒等待/網路、推進時鐘、Mach IPC/執行緒、dyld/TLS 與完整框架仍未完成。原生 Intel HVF 與實體 iOS 未驗證，原時限不變。

```text
GroupMembershipUID / group_membership_uid / original cr_gmuid
0..INT32_MAX or KAUTH_UID_NONE=4294967195 / 0xffffff9b / not UINT32_MAX
positive entries first / original NONE plus complete list proves negatives
omitted list unknown / first-match real copy preserves original NONE
ordinary-queries-closed-groups / 173 events / stdout GN
OrdinaryQueriesUseExplicitMembershipUIDWithoutResolver
73 mandatory workloads per platform / ARM64 219 / Intel 146 unverified
SDK constant O0/O1/O2 only / prior actual nonowner pairs270 remain separate
58 native-common references unchanged / Intel and physical iOS unverified
native5s / compile120s / guest-Python5,000,000us / quantum1024 / public10s
```

[XNU KAUTH_UID_NONE](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/kauth.h), [XNU credential membership](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_credential.c).

## 一般檔案的即時就緒查詢

原始 BSD poll(230) 與 poll_nocancel(417) 支援 timeout 的 low int32=0。描述元所有者把 nfds 解碼為 uint32；超過 OPEN_MAX10240 先傳回 EINVAL22，不存取指標。正數項目需要明確 darwin_system.resource_limits 資源8 Current，省略代表未知。超過 Current 時，項目數大於 FD_SETSIZE1024 傳回 EINVAL；較小項目數需要原始明確有效 UID，非零傳回 EINVAL，零允許繼續。root 例外只有原始碼/模型證據。DescriptorLimit、kern.maxfilesperproc、FD 數值與執行預算不提供此觀測。零項目不存取陣列；非零逾時在項目准入後報告不支援。

建構的虛擬一般檔案明確採用未撤銷、一般讀寫過濾器註冊成功、沒有 MAC 或提供方拒絕的契約；原生 vnode 類型本身不能證明註冊成功。此前提下 IN/RDNORM、OUT/WRBAND 在 EOF 或唯讀開啟時也就緒，不由剩餘位元組、存取旗標、O_NONBLOCK 或寫入授權推斷。過濾器依數字 FD 與讀/寫類別各自保留最後請求位置，不合併 dup 的不同 FD。單獨 HUP 登記讀過濾器但不輸出就緒位元。負 FD 與僅忽略位元為零；關閉 FD 的有效登記逐項回傳 POLLNVAL32。活動 OOB/vnode、串流、目錄和符號連結提供方在任何輸出前拒絕。先完整快照八位元組記錄；輸入不完整或輸出完全不可寫回傳 EFAULT14，部分輸出拒絕且不複製前綴。完整複製保留 fd/events，替換全部 revents。兩種靜態權限查詢模式於預檢前關閉 Poll，包含零項目；共享清單現有42條關閉路由。

無 SDK 的 immediate-poll 保留86個事件；原清單為59例，immediate-select 新增一例，select 階段的公共原生清單為60例。獨立 ARM64 O0/O1/O2 poll582次及 select792次觀測分別保留字面 ABI/錯誤控制與提供方捕獲，不是通用常數。舊58/59例證明保留原始碼身分、失敗嘗試與原時限。原生 Intel HVF、實體 iOS 未驗證；撤銷/MAC/提供方拒絕、等待、pselect 訊號遮罩變更、非同步就緒、網路、推進時鐘、真實 Mach IPC/執行緒、dyld/TLS 與完整框架未完成。

```text
poll230 / poll_nocancel417 / timeout low int32=0 / nfds uint32
ResourceLimits[8].Current / OPEN_MAX10240 / FD_SETSIZE1024 / explicit original effective UID
constructive regular provider: nonrevoked / successful ordinary filter attachment / no MAC-provider refusal
numeric FD + read/write filter / independent last requested index / distinct dup aliases
IN1 RDNORM64 OUT4 WRBAND256 / HUP16 trigger only / closed registrations POLLNVAL32
negative and ignored-only rows zero / whole input snapshot / whole output / partial output unsupported
EFAULT14 EINVAL22 / no ready prefix on unsupported row / zero count no pointer
static-owner-queries and static-ordinary-queries / all57 other file routes closed before preflight
immediate-poll86 events / five software + three mandatory ARM64 HVF profiles / C CLI Python
64 current native-common cases / prior58/59 source identities preserved / 582 mixed controls and captures
native Intel and physical iOS unverified / select waits pselect signal-masks revoked-MAC-provider failures networking unfinished
```

[XNU poll ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/poll.h), [poll registration and copy order](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/sys_generic.c), [vnode registration and regular filters](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c).

## 普通檔案的即時 select

原始 BSD `select`93 與 `select_nocancel`407 僅接受明確零 timeval 與非負的有號低32位數量；指標保留完整64位。LP64 timeval 為16位元組：偏移0為有號64位秒，偏移8為有號32位微秒，偏移12填補忽略且不改寫。秒範圍0..100000000，微秒範圍0..999999。先匯入及驗證 timeval，再處理負數的 EINVAL22；不完整匯入為 EFAULT14。空指標或正逾時明確不支援，數量0不存取位圖。最高活動 FD 加1僅證明 `fd_nfiles` 下界；關閉高 FD 縮小此證明，並非原生分配容量縮小。NOFILE、預算及 vnode 類型不提供容量或提供方事實。

小端32位遮罩向完整字取整。先快照全部非空輸入，再檢查所有選中 FD，最後解讀提供方；缺少 FD 為 EBADF9，選中的串流、目錄及符號描述符不支援。構造的普通描述符明確宣告純粹且不會拒絕的 select 提供方，對讀、寫、例外均回傳非零；此規則獨立於 poll。跨三組及不同 dup 編號逐位計數，包含唯讀、非阻塞與 EOF，不改變偏移、旗標、內容及中繼資料。清除複製字內尾位，保留外部全部位元組。

輸出之間及輸出與16位元組 timeval 不得重疊。寫入前檢查所有完整輸出權限；部分可寫，包括不可寫前綴後有可寫尾部，皆在全部複製前拒絕。可寫前綴為0不足以證明整段不可寫。整段依讀、寫、例外順序嘗試複製；完全不可寫的組回傳 EFAULT14，後方可寫組仍更新並保留首個錯誤。兩種靜態查詢模式於預檢前關閉 Select，共43條關閉路由。無 SDK 樣例由五個軟體、三個必要 ARM64 HVF 設定與 C、CLI、Python 檢查115個有序事件及完整保護物件。等待、非空 `pselect` 訊號遮罩、非同步提供方與完整作業系統仍未完成。

```text
select93 / select_nocancel407 / zero supplied user64_timeval16 / signed low32 count
current live FD lower bound / no guessed fd_nfiles allocation or NOFILE capacity
read write exception / bit count across sets / distinct dup numbers
all copyins before all descriptor checks / all checks before provider interpretation
partial outputs and overlaps unsupported before all output / zero prefix is not whole unwritable
ordered read write exception copyout / first EFAULT14 after later whole writable copies
792 mixed controls and captures / 540 literal controls and 252 provider captures / 225 provider projections
immediate-select115 events / five software + three required ARM64 HVF / C CLI Python
64 current native-common cases / 76 mandatory workloads per platform / ARM64228 / Intel152 unverified
native5s / compile120s / guest-Python5,000,000us / quantum1024 / public10s
```

[XNU select and copy order](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/sys_generic.c), [FD mask ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/_types/_fd_def.h), [user64 timeval](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/time.h), [ordinary select provider](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c).

即時 `pselect394 / pselect_nocancel395` 接受已提供的零值 `user64_timespec16` 與空訊號遮罩指標。兩個欄位均為 signed64：秒數為 0..2147483647，奈秒為 0..1000000000，包含上界。有效的非零逾時仍因等待尚未實作而停止。先匯入並驗證 timespec，再匯入四位元組遮罩，最後檢查計數與位元圖。不可讀遮罩回傳 EFAULT14；完整的非空遮罩因尚未建立執行緒訊號替換與還原模型而停止。空遮罩路徑共用 select 的位元圖、容量、提供者及輸出規則，包括完整輸出範圍的權限檢查與依序複製。這尚未完成訊號、等待、dyld、Apple 框架或 iOS 實機驗證。

```text
pselect394 / pselect_nocancel395 / zero supplied user64_timespec16 / null signal mask
signed64 seconds0..2147483647 / signed64 nsec0..1000000000 inclusive
time import validation before mask4 import before signed low32 count
complete nonnull mask unsupported / no guessed signal state
shared select bitmap capacity provider output owner
918 mixed controls and captures / 729 literal controls and 189 provider captures
```

## 顯式普通檔案權限修改

原始 chmod15、fchmod124、fchmodat467 使用 mode_mutations 中獨立宣告的初始普通檔案物件及固定虛擬 ctime。空 objects 只宣告普通環境，不提供 provider 成功；省略表示未知，且與兩種靜態查詢環境不相容。契約排除 ACL、MAC、額外監聽器、特權繞過、撤銷、檔案/目錄 lease 與修改前等待及通知；VM 映射 lease 與其他操作的授權保持獨立。

唯一授權層比較請求和目前完整 stat 權限，採用明確非 root 有效憑證。非所有者要求不同權限得到 EPERM；相同權限不能僅因所有權遭拒，但每次成功仍需物件 provider grant。路徑依實際目前目錄逐一檢查 SEARCH；EACCES 先於缺失名稱，未知群組、憑證或紀錄保持不支援。唯讀 FD 無需路徑 SEARCH。成功只更改普通權限與 ctime，保留其他欄位、位元組、游標、屬性、儲存及映射生命週期；grant 隨 dup、硬連結、重新命名及持有的已刪物件保留，新物件或重用名稱不繼承，失效紀錄不能恢復。

低32旗標錯誤先回傳 EINVAL；獨立指標、空路徑和 FD 錯誤保持共同匯入順序，絕對路徑略過 dirfd。NOFOLLOW 保留最終連結，ANY 禁止先前展開；目錄、連結、特殊權限及所選 root 修改仍不支援。工作負載記錄54次原始呼叫事件，並檢查31份保護完整紀錄；main回傳37不追加原始exit事件。450份所有者觀測不能充當非所有者原生證明；63/78清單及234個匹配 ARM64 身分仍需實際成功。Intel 原生、實體 iOS、一般授權與完整系統/框架仍未完成。

```text
chmod15 / fchmod124 / fchmodat467 / low32 mode & ALLPERMS07777
mode_mutations / objects / path / time / fixed virtual ctime
effective explicit credential / actual current directory SEARCH
ordinary security attributes / no ACL MAC file-directory leases or lifecycle effects
same current permissions: no WRITE_SECURITY / nonowner not denied solely for ownership
different current permissions: WRITE_SECURITY / nonroot nonowner EPERM1
object provider grant still required for every success / no grant inference
stat current-validity owner / invalidated records never restored
type bytes offsets xattrs storage and VM mapping leases preserved
AT_SYMLINK_NOFOLLOW0x20 / AT_SYMLINK_NOFOLLOW_ANY0x800
flags EINVAL22 before path EFAULT14 / relative bad FD EBADF9 / absolute FD bypass
empty declaration supplies environment without object provider grants
missing names ENOENT2 only after all required SEARCH / denied SEARCH EACCES13
SDK-free54 service records / 54 native calls / 31 guarded stat records / main return is not a raw exit event
OrdinaryModesKeepCurrentMetadataAuthorityAndObjectState
64 current native-common cases / 79 mandatory workloads per platform
ARM64 matching native mandatory237 / native Intel and physical iOS unverified
450 original owner observations / 414 literal controls and 36 final-link provider captures
nonowner authorization is source/model evidence, not owner-native evidence
all57 other file routes closed before preflight / static query declarations incompatible
native guest5s / compile120s / build-C++1200s / Python-config300s / docs-caps120s
```

[XNU chmod/fchmodat/fchmod](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU current-mode authorization and lease lifecycle](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_subr.c).

## 明確普通檔案所有權修改

chown16、fchown123、fchownat468 使用 ownership_mutations 宣告初始普通檔案的獨立 provider 與固定虛擬 ctime。空 objects 只提供環境，省略保持未知，可與獨立 mode_mutations 共存，靜態查詢環境不相容。唯一授權層使用目前完整中繼資料及明確非 root 有效憑證：先拒絕第三方 UID，再檢查目標群組；未知資格停止，改為呼叫者 UID 仍因 TAKE_OWNERSHIP 無普通回退而 EPERM，僅改 GID 需要目前所有者 WRITE_SECURITY。低32 UINT32_MAX 表示不修改；兩個 ID 均不活動仍需 provider，完整紀錄與 ctime 不變；任何活動 ID 使用宣告 ctime，即使值相同。路徑檢查實際目前目錄 SEARCH，fchownat 僅接受 NOFOLLOW0x20，ANY0x800 先回 EINVAL。grant 跟隨共用物件，建立及名稱重用不繼承，失效紀錄不恢復。其他資料、命名空間、屬性、儲存與 VM lease 維持獨立。下列清單說明界線和驗證要求；所有者原生觀測不證明非所有者，Intel 原生、實體 iOS 及完整系統和框架仍未完成。

```text
chown16 / fchown123 / fchownat468 / low32 UID GID / UINT32_MAX inactive
ownership_mutations / objects / path / time / independent provider authority
explicit effective nonroot credentials / actual current directory SEARCH
changed third-party UID EPERM1 before target-group lookup
changed GID: known membership required / unknown membership unsupported
TAKE_OWNERSHIP == CHANGE_OWNER / no ordinary POSIX fallback
changed UID to caller EPERM1 / group-only WRITE_SECURITY requires current owner
same IDs: no change-derived security rights / provider still required
both IDs inactive: provider still required / entire record and ctime unchanged
any active ID: fixed virtual ctime even for numerically same IDs
AT_SYMLINK_NOFOLLOW0x20 only / ANY0x800 EINVAL22 before path or FD
initial regular object grants / dup hardlink rename held unlink retain identity
creation and name reuse do not inherit grants / invalidated records never restored
mode data namespace xattr storage and VM mapping leases keep independent contracts
SDK-free77 service records / 77 authored raw calls / 41 guarded stat records
separate main return38 / OrdinaryOwnershipKeepsCurrentAuthorityAndObjectState
64 current native-common cases / 79 mandatory workloads per platform
ARM64 matching native mandatory237 / actual successful execution required
720 owner observations / 702 literal controls and 18 final-link provider captures
nonowner authorization is source/model evidence, not owner-native evidence
all57 other file routes closed before preflight / static query declarations incompatible
no ACL MAC listener bypass revocation native file-directory leases or setattr lifecycle
native Intel and physical iOS unverified / full macOS-iOS environment incomplete
```

[XNU ownership routes](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU ownership actions](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_subr.c), [XNU vnode_setattr provider](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/kpi_vfs.c).


## 明確普通檔案整檔諮詢鎖

flock131 以 advisory_locks 的唯一字串 objects 宣告初始普通檔案的獨立提供者；省略為未知，空陣列不授權物件。鎖屬於開啟描述，dup、fcntl 複製和 dup2 共用所有者，最後一個描述符關閉即釋放，VM 映射不保留鎖。UN 優先於 EX，再優先於 SH；非阻塞衝突 EAGAIN35 保留原共享鎖，競爭中的阻塞等待在修改模型前停止。執行期連結、重新命名和持有物件 unlink 保留授權，建立及名稱重用不繼承。以下事實列出驗證界線；Intel 原生、實體 iOS 和完整系統環境仍未完成。

```text
flock131 / Int ABI / low32 FD and how / raw high carriers retained
advisory_locks / objects / explicit initial ordinary regular-object provider
missing environment unknown / empty objects no provider / initial locks empty
complete initial metadata / flags0 / ordinary mode / link_count1 / no initial inode alias
path+NUL shares input byte budget / strict unique string array / unknown keys rejected
FD lookup first / known vnode without action EBADF9 / unrelated action bits ignored
UN8 > EX2 > SH1 / NB4 selects nonblocking / readonly EX allowed
whole-file description owner / dup fcntl duplication dup2 share one owner
separate open descriptions arbitrate / shared-shared compatible / exclusive conflicts
nonblocking conflict EAGAIN35 before old-lock removal / failed upgrade keeps SH
success replaces upgrade or downgrade / uncontended blocking completes immediately
contended blocking unsupported before model mutation / no wait or unlock fabrication
current descriptor membership owns lock lifetime / last duplicate close releases lock
VM mapping Contents leases do not retain description locks
runtime hardlink rename held zero-name unlink keep object grant
created or reused names do not inherit provider grants / path registry not lock authority
flock preserves bytes cursors stat xattrs and allocation / successful lock sets persistent FWASLOCKED0x4000
mode ownership data namespace policies independent / invalidated records never restored
all57 other file routes closed before preflight / either static query rejects even empty declaration
SDK-free77 service records / 13 guarded full stat observations / separate main return39
OrdinaryAdvisoryLocksKeepDescriptionAndObjectLifetime
64 current native-common cases / 79 mandatory workloads per platform
ARM64 matching native mandatory237 / actual successful execution required
720 literal native lock observations / 522 action controls and198 lifetime controls
native O0 O1 O2 / raw SVC and syscall / three high-carrier variants
no external or interprocess peers / no POSIX OFD range locks / no fork or descriptor transfer
no MAC listener revocation provider error native-lease or blocking-wait lifecycle
native Intel and physical iOS unverified / full macOS-iOS environment incomplete
```


## 明確的普通檔案完整性同步完成

fsync95 與 fsync_nocancel408 在既有無取消請求的單執行緒設定中共用 Fsync/Int 擁有者。DarwinFileOptions::Synchronization 或嚴格的 darwin_files.synchronization={"objects":["/data"]} 宣告立即成功的普通檔案完整性同步提供者：成功的模型修改已提交，沒有待完成工作或取消請求。回傳零只確認該明確提供者完成，不會刷新宿主儲存，也不證明實體持久化。XNU 普通入口先檢查取消，nocancel 入口沒有這一步；一般取消、提供者錯誤、租約和非同步等待仍未知。

```text
fsync95 / fsync_nocancel408 / Fsync Int owner / low32 FD / raw high carriers retained
synchronization / objects / explicit immediate successful file integrity completion
successful modeled mutations already committed / no deferred provider work or requested cancellation
zero acknowledges declared provider / no host storage flush / no physical durability claim
normal XNU entry tests cancellation / nocancel entry does not / full cancellation unsupported
complete ordinary initial regular objects / flags0 / link_count1 / no initial inode alias
unique strings / at most256 objects / canonical path+NUL shares catalogue byte budget
missing environment unknown / empty objects no completion grant
FD lookup first / absent or closed descriptor EBADF9 even without environment
opaque streams unsupported kind / directory and symlink descriptions need independent providers
readonly regular description accepted only with explicit object completion grant
Contents owns grant / dup fcntl dup2 select current description and object
runtime hardlink rename held zero-name unlink retain original grant
created and reused names do not inherit initial completion grants
VM mapping and retained Contents cannot make closed descriptors valid
bytes cursors flags stat xattrs allocation namespace and advisory locks unchanged
unknown metadata and xattrs remain unknown / no observation restoration
all57 other logical file routes closed before preflight / both static queries reject empty declaration
one alias shares owner / no extra query route for fsync_nocancel
SDK-free68 service records / 20 guarded full stat observations / separate main return40 / stdout S
OrdinaryFileSynchronizationKeepsProviderAndCommittedState
68 current native-common cases / 83 mandatory workloads per platform
ARM64 matching native mandatory249 / actual successful execution required
504 original native literal observations / O0 O1 O2 / raw SVC and syscall / three high carriers
176-byte guarded native stat and cursors unchanged / actual native times
file/data grants never supply F_FULLFSYNC51 F_BARRIERFSYNC85 / global sync remains unsupported
no provider error resource-fork shadow-flush lease revocation wait or full cancellation lifecycle
native Intel and physical iOS unverified / full macOS-iOS environment incomplete
```

[XNU fsync paths](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU descriptor lookup](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_descrip.c), [XNU syscall declarations](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/syscalls.master).


## 明確的普通檔案資料完整性同步完成

fdatasync187 是獨立的 Fdatasync/Int 操作。XNU 先檢查取消，再要求 MNT_DWAIT；fsync95/fsync_nocancel408 要求 MNT_WAIT。DarwinFileOptions::DataSynchronization 或嚴格的 darwin_files.data_synchronization={"objects":["/data"]} 宣告立即成功的資料完整性提供者：成功的模型資料修改已提交，沒有待完成工作或取消要求。檔案 Synchronization 與資料 DataSynchronization 授權不能互相替代。零只確認明確提供者，不刷新宿主儲存，也不證明實體持久化；一般取消、提供者錯誤、租約與非同步等待仍未知。

```text
fdatasync187 / distinct Fdatasync Int purpose / low32 FD / raw high carriers retained
XNU tests cancellation and requests MNT_DWAIT / fsync95 and fsync_nocancel408 request MNT_WAIT
data_synchronization / objects / explicit immediate successful data integrity completion
DataSynchronization and Synchronization grants never supply each other's purpose
successful modeled data mutations already committed / no deferred provider work or requested cancellation
zero acknowledges declared provider / no host storage flush / no physical durability claim
complete ordinary initial regular objects / flags0 / link_count1 / no initial inode alias
unique strings / at most256 objects / canonical path+NUL shares catalogue byte budget
missing environment unknown / empty objects no data completion grant
FD lookup first / absent or closed descriptor EBADF9 even without environment
opaque streams unsupported kind / directory and symlink descriptions need independent providers
readonly regular description accepted only with explicit data object completion grant
Contents owns separate grants / dup fcntl dup2 select current description and object
runtime hardlink rename held zero-name unlink retain original grant
created and reused names do not inherit initial completion grants
VM mapping and retained Contents cannot make closed descriptors valid
bytes cursors flags stat xattrs allocation namespace and advisory locks unchanged
unknown metadata and xattrs remain unknown / no observation restoration
all57 other logical file routes closed before preflight / both static queries reject empty declarations
fsync_nocancel408 remains one Fsync alias / fdatasync187 is not that alias
SDK-free41 service records / 11 guarded full stat observations / separate main return41 / stdout D
OrdinaryDataSynchronizationKeepsProviderAndCommittedState
68 current native-common cases / 83 mandatory workloads per platform
ARM64 matching native mandatory249 / actual successful execution required
252 original native literal observations / O0 O1 O2 / raw SVC and syscall / three high carriers
176-byte guarded native stat and cursors unchanged / actual native times
directory and pipe captures do not expand ordinary initial regular-object provider
file/data grants never supply F_FULLFSYNC51 F_BARRIERFSYNC85 / global sync remains unsupported
no provider error resource-fork shadow-flush lease revocation wait or full cancellation lifecycle
native Intel and physical iOS unverified / full macOS-iOS environment incomplete
```

[XNU synchronization purposes](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU descriptor lookup](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_descrip.c), [XNU syscall declarations](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/syscalls.master).


## 新建普通檔案的明確同步完成提供者

兩個獨立同步宣告都可包含 created_regular_objects 布林值，對應 C++ CreatedRegularObjects。只有 true 且已提供完整 CreationPolicy，才在普通檔案成功建立時授予該項立即完成能力；省略或 false 保持原有預設。必填 objects 仍只授權初始物件，名稱重用不會轉移舊授權。檔案與資料同步能力不能互相替代。

```text
CreatedRegularObjects / created_regular_objects / defaultfalse / mandatory objects array
synchronization={"objects":[],"created_regular_objects":true}
data_synchronization={"objects":[],"created_regular_objects":true}
true requires complete CreationPolicy / current umask / parent metadata / valid inode and mutation policy
ordinary regular creation commit only / independent File95+408 and Data187 grants
initial grants and creation grants never supply each other / existing O_CREAT receives no new grant
name reuse never transfers old authority / fresh explicit creation grant may authorize a replacement
false or omission retains old default / missing and empty environments remain distinct
single set-provider parser and input byte owner / no extra path entry FD inode or byte charge
failed creation preserves admission resources and grants / directories links streams remain unsupported
Contents lifetime through dup fcntl dup2 runtime links rename held zero-name and mapping leases
closed FD returns EBADF9 / retained Contents or VM cannot revive a descriptor
bytes stat cursor flags xattrs allocation namespace locks unchanged / no observation restoration
completion grants supply no mode ownership xattr flock or general DAC authority
both immutable query environments reject even false declarations / all57 other logical routes remain closed
SDK-free147 service records / 33 guarded full stat observations / separate main return42 / stdout C
CreatedRegularSynchronizationKeepsIndependentProvidersAndState
68 current native-common cases / 83 mandatory workloads per platform / ARM64 matching native mandatory249
702 original native literal observations / O0 O1 O2 / raw SVC and syscall / three upper carriers
freshly created target without preparatory sync / own176-byte guarded native stat and content preserved
native and model post-write F_GETFL65538 / existing FileWasWritten0x10000 / own initial flags captured and preserved
immediate successful declared provider / committed model mutations / no deferred work or requested cancellation
no host flush or physical durability / general cancellation provider errors leases waits remain unknown
native Intel and physical iOS unverified / complete macOS iOS frameworks remain incomplete
```


## 新建普通檔案的明確鎖定授權

獨立 advisory_locks 宣告可包含 created_regular_objects 布林值，對應 C++ CreatedRegularObjects，預設 false。true 要求完整 CreationPolicy，只授權成功新建的普通檔案；必填 objects 獨立授權初始物件，開啟已有檔案或重用名稱都不轉移授權。成功 flock 會設定開啟描述共享的持續 FWASLOCKED0x4000 狀態，可由 F_GETFL 觀察；解除鎖定保留此狀態，F_SETFL 不能憑空設定或清除它。

```text
advisory_locks={"objects":[],"created_regular_objects":true}
CreatedRegularObjects / created_regular_objects / defaultfalse / mandatory unique objects array
true requires complete CreationPolicy / regular creation commit only / initial objects independent
existing O_CREAT and reused names transfer no authority / explicit replacement grant is fresh
single set-provider parser and admission / shared path+NUL budget / no extra entry byte FD or inode
failed creation preserves budgets and next inode / directories links streams receive no regular grant
Contents grants follow hardlink rename held zero-name and mapping / current FD lookup first
description owns lock and history / dup fcntl dup2 share / independent opens own separate history
successful flock sets FileWasLocked FWASLOCKED0x4000 / F_GETFL includes persistent description status
unlock preserves history / failed and unsupported actions do not set it
F_SETFL accepts observed history for roundtrip but cannot manufacture or clear it
symbolic recognized history returns ENOTTY25 / opaque streams zero / actual history unchanged
written created F_GETFL65538 becomes81922 after successful flock / readonly0 becomes16384
nonblocking conflict35 before old-lock removal / failed upgrade retains shared lock
last description close releases lock / VM Contents lease cannot retain lock or revive closed FD9
flock preserves bytes stat cursor xattrs allocation namespace and observation validity
mode ownership completion xattr permissions remain independent / no general DAC grant
both static query declarations reject even false environment / all57 other file routes closed
SDK-free131 service records / 12 guarded full stat observations / separate main return43 / stdout L
CreatedRegularAdvisoryLocksKeepProviderAndDescriptionStatus
68 current native-common cases / 83 mandatory workloads per platform / ARM64 matching native mandatory249
486 original native flock observations / 72 fcntl history controls / O0 O1 O2 / raw SVC and libc
162 additional native symbolic and pipe status controls / O0 O1 O2 / two APIs three carriers
whole176-byte guarded native stat / dirty bytes and current cursors preserved / literal flag transitions
failed first flag-preservation assumption retained / sole semantic owner repaired from native and pinned XNU
native5s guest5000000us quantum1024 public10s unchanged / no timing stability claim
no external peers range locks blocking waits cancellation revocation leases or host provider qualification
native Intel and physical iOS unverified / complete macOS iOS frameworks remain incomplete
```


## Explicit mode and ownership providers for created files

新建一般檔案可透過明確宣告分別取得權限修改與擁有者修改能力。兩個開關預設關閉，啟用時需要完整建立策略。既有物件保留各自宣告的授權；名稱重用不會轉移權限。成功修改使用指定的 mutation_time 作為虛擬 ctime。擁有者與群組均不修改時仍需提供器並保持所有位元組不變。中繼資料或群組成員資訊未知時仍明確拒絕。


```text
mode_mutations={"objects":[],"created_regular_objects":true}
ownership_mutations={"objects":[],"created_regular_objects":true}
two independent timed creation providers / defaultfalse / mandatory objects array
created ModeTime and OwnershipTime use CreationPolicy.Mutation.Time as explicit virtual ctime
initial fixed times remain independent / existing O_CREAT grants nothing / fresh replacement only
both UINT32_MAX IDs require provider execution and preserve every byte including ctime
explicit effective groups20/31 / real GID31 alone does not prove effective membership
attribute grants cannot restore invalidated metadata or supply write namespace lock completion xattr authority
SDK-free235 service records / 137 guarded full stat observations / separate main return44 / stdout A
virtual-only nine service records / separate main return45 / 320 independent literal guarded bytes
CreatedRegularAttributesKeepIndependentProvidersAndObjectState
69 current native-common cases / 84 mandatory workloads per platform / ARM64 matching native mandatory252
450 original native mode observations / 720 original native ownership observations
native trace not captured / virtual literal bytes not a native timestamp oracle
native Intel and physical iOS unverified / complete macOS iOS frameworks remain incomplete
```


## Explicit full and barrier completion providers

完整同步與屏障同步透過明確宣告的提供器分別完成一般檔案操作。既有物件與新建物件的授權獨立；建立開關預設關閉，啟用時需要完整建立策略。完成操作保持模型所有觀察不變，並忽略未使用的參數。屏障能力直接提供，不執行掛載提升或退回。兩種宣告都不刷新主機儲存，也不證明實體持久化。


```text
full_synchronization={"objects":[],"created_regular_objects":true}
barrier_synchronization={"objects":[],"created_regular_objects":true}
F_FULLFSYNC51 / F_BARRIERFSYNC85 / existing raw fcntl92 and406 / low32 FD and command
two independent initial and new-regular providers / strict objects array / defaultfalse / complete CreationPolicy
full and barrier grants never borrow file data or each other / reused names transfer no authority
direct barrier support / no mount promotion or fallback / no host flush or physical durability
bad FD before opaque kind query scope regular target environment and exact object grant
unused argument ignored without reading memory / complete model observations unchanged
directory link and failed creation publish no regular grant / VM mappings never revive closed FDs
FullAndBarrierSynchronizationKeepIndependentObjectState
51 private test identities / pages4096 and16384 / commands51 and85
SDK-free148 authored service records / 38 guarded full stat observations / separate main46 / stdout F
virtual-only ten services / separate main47 / 320 independent literal guarded bytes / not native metadata
70 current native-common cases / 85 mandatory workloads per platform / ARM64 matching native mandatory255
216 original native raw calls / 648 guarded176-byte stat comparisons / native pipe EBADF9
native observed completion does not distinguish direct barrier from mount promotion
native Intel and physical iOS unverified / full Apple frameworks remain incomplete
```

The native constants and descriptor behavior are checked against [Apple XNU xnu-11417.140.69 kern_descrip.c](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_descrip.c) and [fcntl.h](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/fcntl.h). Native non-vnode EBADF observations do not identify opaque model-native kinds.


## Explicit ordinary timestamp mutations

明確宣告的時間修改提供器獨立授權一般檔案。既有物件與成功新建一般檔案的權限分開；建立開關預設關閉，啟用時需要完整建立策略。NULL 請求只使用另行宣告的目前時間樣本。請求時間的儲存規則與固定虛擬 ctime 是不同觀測。模型不讀取主機時鐘或檔案系統，完整 macOS/iOS 框架環境仍未完成。


```text
DarwinFileTimestampMutationOptions / timestamp_mutations / objects / created_regular_objects defaultfalse
utimes138 and futimes139 / LP64 two timeval records32 bytes / signed64 seconds signed32 microseconds ignored padding
virtual request-storage rule / signed wide nanoseconds saturated to int64 / floor-normalized seconds nanoseconds
birth reduction only for earlier nonnegative resulting mtime including zero / negative mtime preserves birth
fixed virtual ctime is separate from request conversion and current-time sample / no host filesystem identity inferred
NULL uses only explicit TimeOfDay / zero sample valid / missing sample stays unknown / no timezone or Mach prerequisite
utimes pathname before input / futimes input before low32 FD / fully unmapped EFAULT14 / partial input unsupported
explicit effective ownership even equal times EPERM1 / current-time owner or proved ordinary POSIX write / group knowledge preserved
all57 other file routes closed before preflight / both static queries reject even empty timestamp declaration
ordinary regular Contents grants retain actual dup link rename held-zero-name lifetime / replacement names inherit none
opaque kind unsupported / native pipe EINVAL22 does not identify model kind / directories and symbolic vnodes outside provider
62 private identities / pages4096 and16384 / full160-byte stat guards / independent dirty bytes locks xattrs and real VM
TimestampMutationsKeepExplicitClockAndObjectAuthority / 124 authored common services / main48 stdout T
virtual-timestamp-creation / ten authored services / main49 / literal320 guarded bytes / explicit plus NULL request
351 accepted original ARM64 O0 O1 O2 observations / signed micros saturation and birth limits belong to captured host
native-common71 / mandatory86 per platform / matching ARM64 native required258 identities
native Intel and physical iOS unverified / original five-second native and guest bounds unchanged / full Apple frameworks incomplete
```

The original model references [pinned XNU syscall/vnode ordering](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c) and [ordinary WRITE_ATTRIBUTES authorization](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_subr.c). The discovered futimes number is139; the rejected206 probe is retained as failure evidence. No XNU implementation was copied.

## Explicit filesystem observations

明確宣告的檔案系統快照獨立提供一般檔案和目錄的容量、類型與掛載名稱。零值和空名稱都是有效輸入，模型不讀取主機檔案系統。繫結隨實際物件保留，支援複製描述符、移動及刪除後仍開啟的物件，替代名稱不繼承舊繫結。成功建立一般檔案時，只有明確啟用建立權限並提供完整建立策略，才使用實際父目錄已有的快照。這補上可用的檔案環境查詢入口；完整 macOS/iOS 框架環境仍未完成。


```text
DarwinFileSystemSnapshot / DarwinFileSystemOptions / FileSystemObservations / filesystem_observations
statfs64 BSD345 and fstatfs64 BSD346 / LP64 record2168 bytes / legacy157158 unsupported
block_size uint32 io_size int32 / five uint64 counters / filesystem_id two int32 words
owner type flags subtype uint32 / type_name16 mount_point1024 mounted_from1024 / extended_flags uint32
reserved seven uint32 words zero / bounded UTF8 NUL-free names / all sixteen JSON members required / zero and empty valid
explicit exact immediate mount-stat provider / no host query mount identity quota live refresh MAC refusal wait or process mountpoint rewrite
initial regular directory and symbolic bindings / actual Contents DirectoryNode LinkNode / no target filestat prerequisite or recovery
created_regular_objects defaultfalse / true requires complete CreationPolicy / successful new regular uses actual bound parent snapshot
initial children unbound parents default-created directories and links receive none / no fsid device inode or pathname prefix inference
effective ordinary ancestor search uses shared authorizer and group knowledge / FD query borrows no target DAC credentials or access flags
pathname and low32 FD kind before output / ENOENT2 EBADF9 EFAULT14 / individually partial output unsupported before effects
both immutable query domains reject even empty filesystem declaration / all57 other logical file routes closed before preflight
67 private identities / both4096 and16384 pages bothroutes / full2200 guarded literal / shared16MiB budget
dup independent opens hardlinks rename directory moves CWD held-unlink reuse / dirty bytes locks xattrs and real VM independent
FileSystemSnapshotsPreserveLayoutAndIndependentObjectLifetime / 49 authored common services / main50 stdout W
virtual-filesystem-creation / nine authored services / main51 / independent4400 guarded literal bytes
114 original ARM64 O0 O1 O2 observations / readonly writeonly directory and held-zero-name FD controls / native pipe EINVAL22 opaque kind unsupported
native-common72 / mandatory87 per platform / matching ARM64 required261 identities / no captured native service trace
constant guest validation loops unrolled with every assertion retained / native5s guest5000000us quantum1024 public10s original owner bounds unchanged
native Intel physical iOS full dyld Mach IPC ObjC Swift Foundation UIKit unqualified / full Apple environment incomplete
```

Raw BSD statfs64 345 and fstatfs64 346 use the independent FileSystemObservations provider. JSON places a unique array of {"path":canonical_initial_path,"snapshot":complete_record} entries under darwin_files.filesystem_observations.objects. Complete records require block_size, io_size, blocks, free_blocks, available_blocks, files, free_files, filesystem_id, owner, type, flags, subtype, type_name, mount_point, mounted_from and extended_flags. Integer fields accept the established exact integer/string-integer forms and enforce their widths. filesystem_id is exactly two signed32 words. The three strings contain no NUL, are valid UTF8 and fit15/1023/1023 bytes; empty values are explicit. Unknown members, incomplete records, duplicate paths, unknown objects and excess shared input budgets fail before image loading. Every snapshot plus its path reference is charged2168 bytes and NUL-inclusive path cost against the shared16MiB catalogue budget. Otherwise implicit observed directories consume one catalogue entry; directories already declared by metadata, enumeration, attributes or namespace authority are counted once within the shared256-entry limit.

Path lookup follows symlinks and shares effective ordinary ancestor search, requiring only relevant current ordinary ancestor metadata and credentials. Root/special attributes and unresolved external group membership remain unsupported; no target filestat or access-mode authority is inferred. Known invalid/closed FD returns9 and missing/empty path returns2 before output preflight. Known regular, directory and symbolic FD observations need no unrelated credentials or read/write flags. Opaque standard streams remain unsupported; symbolic-vnode FDs require their own independent snapshot. A valid complete output record contains zeroed string padding and seven reserved words; fully unmapped output returns14, while individually partial output stops before effects.

Snapshots bind to actual initial regular/directory/symbolic objects, including implicit directories, and survive dup, independent opens, runtime hardlinks, rename, moved directories/CWD and held zero-name lifetime. Different or reused names inherit no initial grant. Optional created_regular_objects defaultsfalse. True requires complete CreationPolicy and explicitly binds each successful new regular file to its actual parent's already-bound snapshot, if one exists. Unbound parents, initial children, mkdir/symlink and children through new unbound directories receive none. This relation comes from the declared provider, never equality of filesystem_id/device/inode or a pathname prefix. Counters remain exactly declared observations and are not derived from bytes, allocation units or virtual storage limits.

The original model follows the [pinned XNU namei/file-vnode/mount-stat ordering](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c) and the native SDK LP64 layout. No XNU implementation was copied. Native probes verify captured ABI and ordering on their owned test filesystem; dynamic capacity counts and IDs do not establish universal constants, real mount identity, quota or lifecycle. The SDK-free common workload compares stable fields and guards while deliberately excluding all five dynamic capacity counters. The independent virtual workload checks all4400 emitted bytes against authored literals. The first native virtualized round timed out in validation loops; fixed loops are unrolled without changing a single service, assertion, quantum or deadline. Broader default-fatal cleanup and framework execution are separate unfinished work.

## Declared mount enumeration

掛載表列舉現在支援依宣告順序查詢快取，以及使用明確提供的成功更新結果。每個行程維護自己的快取，重複記錄保持獨立，空表與未知表有明確區別。這補上了 Apple 應用程式常用的一類檔案系統查詢；dyld、Mach IPC 與完整的 Objective-C/Swift、Foundation/UIKit 環境仍未完成。

```text
DarwinFileSystemEnumerationOptions / DarwinMountObservation / FileSystemEnumeration / filesystem_enumeration
getfsstat64 BSD347 Int / shared LP64 record2168 / independent ordered live accessible mounts
mounts required array / cached complete16 fields required / refreshed complete16 fields optional
empty table known empty / missing table unsupported / duplicate entries retain order / no inferred mount identity
NULL output returns full count without refresh / short buffer capacity zero without memory access
low32 signed byte size promoted to LP64 unsigned before division and narrowed int32 / negative size large capacity
NOWAIT2 uses current cache unless WAIT1 or DWAIT4 / other bits follow same test / low32 flag carriers
process cache starts Cached / refresh selects Refreshed before complete EFAULT14 / new process resets cache
copy complete records sequentially / later fault retains prior output and refresh / partial current record unsupported before its effects
missing refresh stops before current effects / earlier records stay committed / path and FD snapshots independent
one catalogue entry per mount / each snapshot2168 bytes / shared256 entries16MiB / public JSON64KiB unchanged
both static query domains reject empty enumeration / all57 other logical file routes closed before preflight
MountEnumerationKeepsOrderedSnapshotsAndRefreshCache / common16 authored services main52 stdout M
virtual-mount-enumeration / 19 authored services main53 / independent19609 full guarded output bytes
270 ARM64 O0 O1 O2 native observations / count stability and literal guard checks / no universal host counters
native-common73 / mandatory88 per platform / matching ARM64 required264 identities
native5s guest5000000us quantum1024 public30s existing method and owner bounds unchanged
no dead unmount MAC refusal failed refresh asynchronous wait host query or complete Apple framework claim
```

Raw BSD getfsstat64 347 reads only the explicitly declared mount table. JSON retains the required darwin_files.files array (which may be empty) and adds filesystem_enumeration: {"mounts":[{"cached":complete_snapshot,"refreshed":complete_snapshot}]}. Each snapshot uses all sixteen fields documented for filesystem_observations. Refreshed is optional; its presence declares immediate successful refresh for that mount. Empty mounts is a known empty table. Absence remains unsupported even for count-only requests. Entries model live, accessible mounts in exactly the declared order, including duplicates, without MAC denial, dead or unmounting state, refresh failure or asynchronous provider work.

NULL output returns the full count without refresh. NonNULL output copies only complete 2168-byte records, returning the smaller of table size and capacity. The low signed32 buffer length is promoted to unsigned64 before division and the quotient is narrowed to signed32, as in the pinned XNU ABI; negative lengths therefore have a large positive capacity. Zero capacity does not access the pointer. Flags use the low32 bits: NOWAIT2 selects the current cache unless WAIT1 or DWAIT4 is also present. Otherwise the current mount needs an explicit Refreshed snapshot. Unknown bits follow this same bit test.

Each process begins with Cached. A successful declared refresh selects Refreshed before copyout, so even a completely unmapped destination returning EFAULT14 updates that mount’s cache. Later NOWAIT observes the selected record. New process preparation resets all selections. Sequential records preserve earlier output and cache changes when a later copy faults or lacks a refresh provider. An individually partially accessible record is explicitly unsupported before its own refresh or output; this is not a modeled native partial-copy result.

Each mount consumes one shared catalogue entry and each cached/refreshed snapshot consumes2168 bytes within the existing256-entry and16MiB budgets. The public JSON envelope stays64KiB. Both static query environments reject even an empty table. Enumeration supplies no statfs/fstatfs object binding and derives no identity from equal filesystem IDs, devices, inodes or path prefixes.

Original implementation and tests use [pinned XNU getfsstat64 ordering](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c) and [mount-stat cache updates](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_subr.c); no implementation was copied. ARM64 O0/O1/O2 probes retain270 observations with stable before/after counts, high carriers, size and flag boundaries, refresh/fault sequences and guarded output. Actual host counters are evidence for those runs, not virtual constants. Native and virtual workloads remain within native5 seconds, guest5,000,000 microseconds, quantum1024 and public30 seconds.

## Explicit ordinary directory modes

明確宣告的 mode provider 現在支援一般目錄與一般檔案。初始目錄授權與新建目錄的獨立開關，都不取代命名空間變更權限，也不將權限傳給子物件。完整 dyld、Mach IPC、Objective-C/Swift、Foundation/UIKit 環境與 iOS 真機驗證仍未完成。

```text
directory mode: chmod15 fchmod124 fchmodat467 / ordinary initial regular or directory objects
CreatedDirectoryObjects=false / created_directory_objects exact Boolean / separate CreatedRegularObjects
created directory mode requires complete CreationPolicy.Namespace / resulting ctime uses Mutation.Time
DirectoryNode ModeTime separate from MutationTime / CurrentMetadata never supplies namespace authority
directory nlink greater than1 valid / equal known Device-Inode aliases rejected / ownership and timestamps require separate providers
mode changes only permissions and ctime / independent namespace time preserves mode / missing namespace time permanently invalidates stat
dup independent opens chdir fchdir direct rename exchange held rmdir and reused names / no child grant inference
directory-mode-mutations59 services / created-directory-mode60 services / 33 full guarded stat144 records / main53 stdout Q
five software profiles C CLI Python / three matching ARM64 HVF required / native common75 mandatory90 ARM64270
60 native directory observations plus3 separate unexpanded-symlink controls / ARM64 O0 O1 O2 / native5s guest5000000us quantum1024 public30s
```

Raw BSD chmod15, fchmod124 and fchmodat467 accept initial ordinary directory objects through the same required mode_mutations.objects array of {path,time} declarations. Complete ordinary directory metadata, flags0, no special permission bits, a valid independent ctime and no known Device/Inode alias are required. Directory nlink may exceed1; regular-only nlink1 is not applied to directories. Equal Device/Inode declarations under different paths remain rejected. A directory grant needs no creator or mutable namespace. Neither a parent grant nor namespace mutability grants mode changes to initial children.

The separate optional created_directory_objects Boolean defaultsfalse. True requires a complete creation_policy including namespace_policy; successful mkdir/mkdirat attaches mode authority to the actual new directory and uses creation_policy.mutation_policy.mutation_time for resulting ctime. created_regular_objects remains independent and never grants directories. Unknown fields, non-Booleans and incomplete policy fail during strict import. Failed creation, existing names, initial objects and name reuse transfer no grant.

The existing effective-credential authorizer and current ancestor SEARCH owner are shared. Ordinary changed-mode nonowner requests return EPERM1, while an equal request can reach the independently required provider. FD lookup precedes authorization; readonly descriptions are eligible. Mode changes preserve all other complete stat fields and all namespace, enumeration, byte/entry/FD/inode accounting, xattrs and VM observations. Directory ownership and explicit timestamp mutation remain outside these mode providers. Special/security/root-caller and symbolic-vnode effects remain unknown.

Namespace changes require their own metadata time and shape. With a directory mutation policy, mkdir/rmdir/direct rename/exchange preserve current mode while applying declared namespace effects. Without it, even a chmod-produced CurrentMetadata becomes permanently unknown and is cleared. A later mode provider cannot recover it or borrow its mode time as a namespace time. Dup, independent opens, chdir/fchdir, direct moves/exchanges and held removed directories follow the actual object; reused names get separate authority.

Original ARM64 macOS O0/O1/O2 probes retain60 directory observations and3 separate final-unexpanded-symlink controls outside the directory provider. The common SDK-free workload performs59 authored services and33 guarded complete stat144 observations, including high carriers, path/fd/*at, CWD, held rename, closed descriptors and unchanged error effects. A native execution returning53/Q validates the authored common behavior without a captured service trace. Virtual initial and created variants independently check every stat byte, including declared mode and namespace times; created execution adds mkdir for60 services. Those virtual values are not native constants. Native nonowner/special security, physical Intel and physical iOS remain unqualified. Native5s, compile120s, per-component build1200s/parallel2, guest5000000us/quantum1024 and public30s remain fixed.

The implementation and original probes use [pinned XNU chmod ordering](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c) and [current-mode authorization](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_subr.c). No implementation was copied.

## Explicit ordinary directory timestamps

顯式目錄時間戳提供器現已支援初始目錄和新建目錄的 utimes/futimes，各自需要獨立授權。命名空間、模式和所有權權限保持獨立。完整 Apple 執行階段與框架及實體 iOS 驗證仍未完成。

```text
directory timestamps: utimes138 futimes139 / ordinary initial regular or directory objects
TimestampMutations CreatedDirectoryObjects=false / created_directory_objects strict Boolean / separate CreatedRegularObjects
created directory timestamp provider requires complete CreationPolicy.Namespace / fixed ctime Mutation.Time
DirectoryNode TimestampTime independent of ModeTime MutationTime DirectoryEntrySize / unknown metadata never restored
explicit timestamps require owner even when equal / NULL uses declared TimeOfDay and shared WRITE_ATTRIBUTES authorization
LP64 timeval32 signed64 seconds signed32 microseconds ignored padding / int64 nanosecond saturation floor normalization / earlier nonnegative mtime reduces birthtime
directory-timestamp-mutations46 services / created-directory-timestamps47 services / 24 complete guarded stat144 records / main54 stdout T
five software profiles C CLI Python / three matching ARM64 HVF required / native common75 mandatory90 ARM64270
141 owned native directory timestamp observations / ARM64 O0 O1 O2 / native5s compiler120s build1200s parallel2 guest5000000us quantum1024 public30s
```

Raw BSD utimes138 and futimes139 now accept independently granted ordinary directories through timestamp_mutations.objects: [{path,time}]. The time is a separately fixed virtual resulting ctime. Initial grants need complete ordinary metadata, flags0, no special mode bits and no known Device/Inode alias; ordinary directory nlink can exceed1. Initial directory grants do not require creation or mutable namespace authority.

The optional strict created_directory_objects Boolean defaultsfalse and requires a complete creation_policy including namespace_policy when true. Only successful mkdir/mkdirat attaches this independent directory timestamp provider, using creation_policy.mutation_policy.mutation_time for ctime. created_regular_objects remains separate. Regular creation flags, mode providers, parents and children supply no timestamp grant to an initial or newly reused directory name.

utimes resolves its path before importing the request. futimes imports the32-byte LP64 timeval array, or the separately declared NULL sample, before low32 FD lookup. Explicit requests require current ownership even when values are equal. NULL uses only darwin_time.time_of_day and shared WRITE_ATTRIBUTES authorization: an ordinary owner is allowed, while a nonowner needs the shared known group/world write decision. Missing time samples, credentials, metadata or providers remain unsupported; bad paths, pointers and descriptors preserve the established raw errno/carry order.

The existing declared storage rule is unchanged: signed64 seconds and signed32 microseconds with ignored padding form signed wide nanoseconds, saturate to int64 and floor-normalize. The fixed provider ctime is independent of requested values and the NULL sample. An earlier nonnegative resulting mtime reduces the known birthtime; negative mtime does not. These virtual rules do not identify APFS or synthesize native filesystem time ranges. Success preserves every unrelated stat field, guards, input bytes, enumeration positions, FD flags and accounting.

Direct moves/exchanges and held rmdir retain actual object grants, while reused names receive none. Namespace changes need their own MutationTime and DirectoryEntrySize. A timestamp-produced CurrentMetadata never supplies these premises; without them, namespace changes permanently invalidate complete metadata and later timestamp calls cannot restore it. Timestamp authority grants no mode, ownership, data or namespace mutation.

Original owned ARM64 macOS O0/O1/O2 probes retain141 guarded directory observations, including low/high syscall/FD carriers, read-only/dup descriptors, errors, link following, direct moves, held removal and name reuse. Native secondary results were zero in these observations. The common SDK-free directory-timestamp-mutations workload checks46 authored calls and24 complete guarded stat144 records, returning54/T; it captures no native service trace. Initial and created virtual variants independently check every stat byte and separately declared times; creation adds mkdir for47 calls. Native nonowner/special-security effects, Intel hardware and physical iOS remain unqualified.

The original implementation and probes use [pinned XNU utimes/futimes ordering](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c) and [timestamp authorization](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_subr.c). No implementation was copied.

## Explicit ordinary directory ownership

顯式所有權提供器現已支援獨立授權的初始一般目錄和新建目錄的 chown/fchown/fchownat。群組變更同時更新子物件建立使用的實際父目錄身分。命名空間、模式和時間戳記權限保持獨立。完整 Apple 執行階段與框架及實體 iOS 驗證仍未完成。

```text
directory ownership: chown16 fchown123 fchownat468 / ordinary initial regular or directory objects
OwnershipMutations CreatedDirectoryObjects=false / created_directory_objects strict Boolean / separate CreatedRegularObjects
created directory ownership provider requires complete CreationPolicy.Namespace / fixed ctime Mutation.Time
DirectoryNode OwnershipTime independent of ModeTime TimestampTime MutationTime DirectoryEntrySize / unknown metadata never restored
active low32 UID GID including equal values set fixed ctime / both UINT32_MAX inactive require provider and preserve all observations
actual DirectoryIdentity.GID and CurrentMetadata.GID commit together / newly created regular directory symbolic objects inherit current actual parent group
directory-ownership-mutations61 services / created-directory-ownership62 services / 29 complete guarded stat144 records / main55 stdout O
five software profiles C CLI Python / three matching ARM64 HVF required / native common76 mandatory91 ARM64273
219 owned native directory ownership observations / ARM64 O0 O1 O2 / native5s compiler120s build1200s parallel2 guest5000000us quantum1024 public30s
```

Raw BSD chown16, fchown123 and fchownat468 now support explicitly granted ordinary directories through ownership_mutations.objects: [{path,time}]. Initial directory grants require complete ordinary metadata, flags0, no special mode bits and no known Device/Inode alias, without requiring mutable namespace authority. Ordinary directory nlink may exceed1; regular nlink1 remains required.

The optional strict created_directory_objects Boolean defaultsfalse. True requires complete creation_policy with namespace_policy; only successful mkdir/mkdirat attaches the independent new-directory provider. Its fixed virtual ctime comes from creation_policy.mutation_policy.mutation_time. created_regular_objects remains separate. Child ownership authority comes only from a child's own initial grant or the independently selected creation flag.

Existing effective ordinary authorization and error order are unchanged: FD lookup precedes object checks; pathname routes authorize actual ancestor SEARCH; fchownat admits only low32 NOFOLLOW0x20 before pathname/FD lookup. Low32 UINT32_MAX is inactive independently for each ID. Both inactive IDs require a provider and preserve the whole record. Equal active IDs require a provider and set declared ctime. Changed third-party UID is denied before group lookup; changed GID needs known membership and current-owner WRITE_SECURITY. Unknown credentials/security/group information remains unsupported. Readonly descriptions and duplicates retain actual-object authority.

Directory GID changes also update the actual parent's retained group identity before any child can be published. New regular files, directories and symbolic links inherit this current GID, including after a direct parent move. A later namespace invalidation preserves only this independently known identity; stat and subsequent ownership mutations remain unsupported and cannot restore complete metadata. Ownership supplies no mode, timestamp, data, namespace or filesystem-observation grant.

Original owned nonroot ARM64 macOS O0/O1/O2 probes retain219 observations, including high syscall/FD/ID/flags carriers, active equal and inactive IDs, errors, final-link following, NOFOLLOW directory requests, dup/readonly descriptions, moves, held removal/reuse and positively observed alternate-group changes. Created regular, directory and symbolic objects inherited that actual group. Complete144-byte stat records include8-byte guards on both sides. Native secondary results were zero in these observations. These owner-operated probes do not establish native nonowner, special-security, Intel or physical iOS behavior.

The SDK-free directory-ownership-mutations workload checks61 authored calls and29 guarded whole stat records, returning55/O. Virtual initial and created variants independently check every stat byte and fixed times; creation adds mkdir for62 calls. The common native workload captures no native service trace and compares actual native ctime rather than inventing its value. All four ID patterns use the base carrier per route, while upper carriers repeat active-both requests. The independent native219 observations and private model checks retain full Cartesian typing. Namespace shape uses the separately declared virtual all-child policy, not inferred native filesystem link counts.

Original implementation and probes use [pinned XNU ownership ordering](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c) and [ordinary setattr authorization](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_subr.c). No implementation was copied.

## Filesystem snapshots for created directories

新增獨立建立開關，可將新目錄繫結至實際父目錄已宣告的檔案系統快照。移動和刪除後，已開啟的描述元仍保留原繫結。完整 Apple 執行階段、框架和實體 iOS 驗證仍未完成。

```text
FileSystemObservations CreatedDirectoryObjects=false / filesystem_observations.created_directory_objects strict Boolean / independent CreatedRegularObjects
created directory filesystem binding requires complete CreationPolicy.Namespace / successful mkdir binds actual parent DirectoryNode.FileSystem before publication
unbound parent snapshot remains unknown / creation succeeds without manufacturing a snapshot / zero snapshot explicitly known
actual snapshots survive moves exchanges dup held rmdir and metadata invalidation / replacement objects bind their own actual parent
filesystem owner and directory stat UID GID independent / no attribute namespace mount identity or global fsid inference
created-directory-filesystem33 authored services / virtual-created-directory-filesystem35 services / main56 stdout D / independent4400 guarded payload bytes
102 owned ARM64 native filesystem observations O0 O1 O2 / native common77 mandatory92 per platform ARM64276
125 initial private and strict JSON checks / five software C CLI Python profiles / three matching ARM64 HVF profiles required
```

Set darwin_files.filesystem_observations.created_directory_objects to the strict Boolean true to opt in. The provider still requires objects (an empty array is valid); entries retain all sixteen snapshot fields and the existing shared2168-byte/path charge. CreationPolicy must include namespace_policy, with the existing explicit mutable parent, complete parent identity and umask prerequisites. False or omission preserves the default closed new-directory domain.

Each successful complete mkdir uses its actual parent’s bound snapshot if present. A zero snapshot is known; absence remains unknown and statfs64/fstatfs64 refuse a missing provider before output preflight. A regular child of a bound created directory needs the independent created_regular_objects flag; another new directory needs the directory flag. Initial children and symbolic vnodes receive no implicit grant. Filesystem owner remains independent of stat UID/GID, including after chown. Mode, timestamp, namespace, quota and mount identity authority are not inferred.

Queries preserve every declared2168-byte payload and zero padding, descriptor state and unrelated full stat bytes. An existing descriptor retains its actual snapshot through parent/child moves, exchange, invalidated stat and held deletion. A replacement binds its own actual parent. Counters stay fixed declared observations; no advancing live mount is claimed. Original probes use the previously pinned XNU statfs64/fstatfs64 declarations and native SDK LP64 layout; no implementation was copied.

## Filesystem snapshots for symbolic vnodes

明確宣告的檔案系統快照現已支援符號連結描述元，包括懸空和循環目標。路徑查詢仍跟隨目標。新符號連結透過獨立開關繫結實際父目錄快照。完整 Apple 執行階段、框架及實體 iOS 驗證仍未完成。

```text
symbolic filesystem: fstatfs64 BSD346 O_SYMLINK own LinkNode.FileSystem / statfs64 BSD345 follows target snapshot
CreatedSymbolicLinkObjects=false / FileSystemCreatedSymbolicLinkObjects created_symbolic_link_objects strict Boolean / filesystem purpose only
created symbolic snapshot requires complete CreationPolicy.Namespace / actual parent binding before successful publication / independent regular directory flags
initial symbolic snapshots need no target stat metadata or valid target / unbound unknown and explicit zero distinct / shared2168-byte plus path reference budget
link object snapshots survive dup parent moves exchanges rename held unlink and name reuse / invalidated stat never restored
symbolic-filesystem-observations32 created-symbolic-filesystem33 virtual-symbolic-filesystem35 authored services / main57 stdout Y / independent6600 guarded bytes
114 owned ARM64 O0 O1 O2 native filesystem observations / common native79 mandatory93 per platform ARM64279
166 initial private strict JSON checks / five software C CLI Python and three matching ARM64 HVF profiles / public30s guest5000000us quantum1024
```

Declare an initial link snapshot with the existing filesystem_observations.objects {path,snapshot} entry. All sixteen snapshot fields remain required; zero/empty values are explicit. The link may have a dangling, cyclic or opaque byte target and no stat observation. A descriptor opened with O_SYMLINK queries the link's own declaration. A normal path query follows its target and queries the resulting object; it does not borrow the link declaration. Missing/empty environments and absent per-object snapshots remain distinct unsupported completions before output preflight.

For new links, set filesystem_observations.created_symbolic_link_objects to Boolean true and supply complete creation_policy.namespace_policy. Successful symlink creation uses the actual parent's already-bound snapshot if present, independently of regular/directory flags. Initial children and equal filesystem_id/device/inode values grant nothing. An unbound parent permits creation but leaves the new link query unknown. Incomplete/failed/existing-name creation publishes no new binding. Name reuse never recovers an old initial observation.

Descriptor lookup narrows FD before kind/environment/provider/output checks. Valid symbolic snapshots need no target stat, credentials or access flags. Whole2168-byte records, padding and guards retain the existing copyout contract: unmapped output returns EFAULT14; partial output is unsupported before effects. Other pointer/FD/path errors preserve ordering. Raw native references use the cached pinned XNU fstatfs64 file_vnode/mount-stat implementation and native SDK O_SYMLINK/LP64 declarations. No implementation was copied; dynamic native counters and real mount identity are not literal virtual facts. Full framework execution remains incomplete.

## 明確的普通符號連結擁有權

明確的擁有權設定現在支援符號連結描述元與 NOFOLLOW 路徑。跟隨連結的目標仍需獨立授權；完整 Apple 執行環境與實體 iOS 驗證尚未完成。

```text
symbolic ownership: chown16 fchown123 fchownat468 / ordinary initial regular directory symbolic objects
fchown O_SYMLINK and fchownat NOFOLLOW0x20 select actual LinkNode / chown and default fchownat follow target
OwnershipMutations CreatedSymbolicLinkObjects=false / FileOwnershipCreatedSymbolicLinkObjects created_symbolic_link_objects strict Boolean
initial exact Objects grant / created symbolic ownership requires complete CreationPolicy.Namespace and uses Mutation.Time / independent regular directory flags
LinkNode OwnershipTime independent of MutationTime and FileSystem / unknown complete metadata never restored
symbolic-ownership-mutations59 created-symbolic-ownership60 virtual-symbolic-ownership67 virtual-created-symbolic-ownership68 authored services / main58 stdout W
literal variants each1281 output bytes / eight complete stat144 records with two8-byte guards / separate link target and replacement inodes
318 owned native ownership observations O0 O1 O2 / six common native runs / native common81 mandatory94 per platform ARM64282
74 direct ownership plus6 strict JSON checks / five software C CLI Python profiles / three matching ARM64 HVF required
native5s compiler120s build1200s parallel2 guest5000000us quantum1024 public30s / native Intel and physical iOS unqualified
```

An initial symbolic ownership grant names the exact symbolic vnode in `ownership_mutations.objects`. Admission requires complete ordinary metadata, flags0, no special mode bits, link_count1 and no known Device/Inode alias. Grant presence, namespace mutability, target metadata and filesystem snapshots are independent. Mode and timestamp declarations still reject symbolic grants. The optional `created_symbolic_link_objects` Boolean defaults false; true requires complete `creation_policy.namespace_policy`. Successful new symlink creation alone attaches its own fixed ctime provider from `creation_policy.mutation_policy.mutation_time`. A parent's grant or another creation flag cannot supply it.

`fchown` on an `O_SYMLINK` descriptor and `fchownat` with low32 `NOFOLLOW0x20` select the actual link. Ordinary `chown` and default `fchownat` follow its target, whose separate grant and complete metadata are required. Dangling and cyclic targets do not affect retained-link or NOFOLLOW operations; followed routes retain ENOENT2 and ELOOP62. FD lookup and complete metadata checks retain their existing precedence; pathname routes authorize actual ancestor SEARCH before final-object checks, and invalid fchownat flags precede pathname import.

The shared effective ordinary authorizer remains authoritative. Both inactive low32 UINT32_MAX IDs require a provider and preserve every stat byte. Any active request, including equal IDs, commits requested UID/GID and the separately declared ctime together after authorization. Unknown credentials, security or group membership remain unsupported. A link GID change affects neither its target nor its parent's retained identity. Namespace and xattr changes without their own complete metadata policy permanently invalidate stat; ownership never repairs it.

Object grants survive duplicate and independent descriptions, runtime symbolic hardlinks, direct and parent moves, exchange and held unlink. A replacement name binds a new inode and receives only an independently selected creation grant. Darwin link9 follows the source link; symbolic hardlink construction uses linkat471 flags0. The original native probe corrected this distinction before production edits; its failed first epoch remains preserved.

Original owned nonroot ARM64 macOS O0/O1/O2 probes retain318 observations, including raw carry/secondary/high carriers, whole guarded stat records, libc controls, flags/cursors/target bytes, observed alternate groups, dangling/cyclic targets and object lifetime. The compact SDK-free common workload passes on both initial and created links at each optimization level. It captures no native service trace and assumes no fixed native ctime. Its literal virtual variants emit eight independent complete guarded stat records and verify every byte, while all variants verify authored raw service outcomes and fixed scalar arguments. Positive runs repeat from fresh state; omitted initial grants stop at the first ownership call, and a missing new-object grant stops at the replacement while preserving the preceding six literal records.

C, CLI and Python routes use all five image profiles without loading an analysis session, compare exact output and services, and reject malformed Booleans. Three matching ARM64 HVF identities execute under required-native policy. Current inventory requires94 methods per matching platform,282 on ARM64; the common native catalogue contains81 cases. Intel hardware, physical iOS, special-security/nonowner native effects, full dyld, Mach IPC and Apple frameworks remain unqualified or unfinished.

The implementation uses the existing shared ownership authorizer and [pinned XNU ownership and link ordering](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), plus [ordinary setattr authorization](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_subr.c). No kernel implementation was copied.

## 透過 lchown 修改路徑末端物件的所有權

`lchown`（BSD 364）修改路徑末端物件，不跟隨末端符號連結，也適用於懸空與循環連結。中間連結仍正常解析，實際祖先目錄仍需 SEARCH 權限。一般檔案、目錄及符號連結使用既有的獨立 `ownership_mutations` 授權。有效的低 32 位元 ID 提交請求的 ID 及固定 ctime；兩個 ID 均為無效值 `UINT32_MAX` 時仍需提供者，並保留完整 stat 記錄。建立旗標、物件生命週期及中繼資料永久失效沿用既有所有權規則。

共用工作負載核對完整的帶哨兵記錄、原始 BSD 結果、別名、移動、持有後刪除與名稱重用。虛擬變體輸出獨立編寫的位元組，並保留目標物件的獨立授權。Intel 原生執行、實體 iOS 及完整 Apple 執行階段與框架仍未驗證或未完成。

```text
lchown364 / final symbolic vnode / existing ownership_mutations grants
lchown-ownership-mutations59 services / virtual-lchown-ownership67 services / main58 stdoutW / virtual1281 guarded bytes
native preparation489 observations including258 lchown / ARM64 O0 O1 O2
common native82 / mandatory95 per matching platform / ARM64 required285
```

## 有界 libSystem 匯入

基線薄執行檔及下述明確指定的啟動 dylib 可繫結強依賴 `/usr/lib/libSystem.B.dylib`。主入口須為 `LC_MAIN`，並使用 `MH_TWOLEVEL` 與一般庫 ordinal；libSystem 相容需求不得超過 1.0。原始 classic/generic64 fixup、未定義符號閉包及槽均須完整驗證。未知符號、weak/threaded 繫結、其他 relocation、初始化回呼與認證指標仍被拒絕。

僅在存在匯入時配置函式入口及每次全新的主執行緒 errno。包裝函式共用原始 Darwin 核心行為，失敗轉成 `-1` 與 errno，成功保留 errno。C `_exit` 的 Mach-O 名為 `__exit`；C `exit` 的 `_exit` 仍不支援。普通函式驗證返回框架；不返回的 `_exit` 無需可用返回位址。報告保留庫、連結名、參數數量及完整位元；未完成或不返回的結果為 null，原始系統呼叫報告保持相容。

`SG_READ_ONLY` 只收緊 fixup 後的目前權限，保留宣告的最大權限；原生 O0/O1/O2 證明讀寫最大權限允許 `mprotect` 恢復寫入。入口與 errno 保留頁不能由來賓解除映射或變更保護。準備及執行共用期限、事件帳戶與既有記憶體預算。

同一自製物件以系統、classic、chained 連結在真實 libSystem 上執行；五種平台/架構經進程/C/CLI/Python 驗證，三個 ARM64 HVF profile 實際執行。任意 dylib、dlopen/dlsym、dyld shared cache、建構函式、執行緒及 Apple 框架仍未完成，Intel HVF 與實體 iOS 未驗收。

classic lazy 指標在進入客體前預先解析。每個程式必須自行指定目的槽、普通函式庫 ordinal 與符號，僅允許指標型別、零 addend，以及一條緊接 DONE 的 DO_BIND，不繼承前一程式狀態。只有同槽的一次本地可執行 rebase 與一次 lazy bind 可合併，兩筆原始記錄都計入預算。dyld_stub_binder 僅可作為伴隨實際 lazy 呼叫的強、零 addend 普通 classic bind；執行其閘道會在讀取 C 框架或參數、修改 errno 或記錄呼叫之前失敗。這支援穩定的預解析指標，不實作執行期 resolver 或 dyld 私有狀態。六函式原生與客體樣本比較槽位穩定性、輸出與結束狀態。

`open`、`lseek`、`pread`、`pwrite`、`dup` 和 `dup2` 共用既有檔案目錄、變更及描述符游標。`open` 通常記錄兩個參數，低 32 位 flags 含 O_CREAT 時記錄三個。ARM64 的可選提升 int 只讀取入口 SP 的四個有效位元組，不讀取填補；x64 使用第三個通用暫存器參數。報告保留原始載體，獨立的核心參數把 mode 截斷至 16 位。沒有 O_CREAT 時 mode 為零且不讀取可選儲存；x64 必需的返回位址仍正常驗證。參數讀取失敗先於呼叫記錄、errno 和檔案副作用。此契約依據 [Apple ARM64 ABI](https://developer.apple.com/documentation/xcode/writing-arm64-code-for-apple-platforms) 和 [XNU open 包裝函式](https://github.com/apple-oss-distributions/xnu/blob/main/libsyscall/wrappers/open-base.c)。O0/O1/O2 原生對照涵蓋共用偏移、定位 I/O、dup2 失敗、超過 4 GiB 的偏移、mode 截斷與二進位內容。執行緒取消與相容路徑轉換仍不支援。

`mmap`、`mprotect` 和 `munmap` 沿用既有記憶體映射、權限、常駐保留區與檔案引用。C 包裝層將保護位、旗標及描述符縮為 32 位元，報告仍保留原始參數。`mmap` 在長度為零或缺少 MAP_PRIVATE/MAP_SHARED 時先回傳 MAP_FAILED 並設定 EINVAL，再為可進入核心的呼叫加入 MAP_UNIX03；原始 BSD 語義不變。依據為 [XNU UNIX03 包裝層](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/libsyscall/wrappers/unix03/mmap.c)。原創 C 對照涵蓋匿名頁清零、權限切換、錯誤順序、關閉描述符後的映射壽命、私有寫入及 EOF 尾頁。本次不擴充共享、固定、可執行映射或配置器行為。

```text
provider: /usr/lib/libSystem.B.dylib (strong LC_LOAD_DYLIB; compatibility <= 1.0)
C -> Mach-O: read -> _read; write -> _write; close -> _close
C -> Mach-O: getpid -> _getpid; _exit -> __exit; __error -> ___error
C -> Mach-O: open -> _open; lseek -> _lseek; pread -> _pread; pwrite -> _pwrite; dup -> _dup; dup2 -> _dup2
C -> Mach-O: mmap -> _mmap; mprotect -> _mprotect; munmap -> _munmap
entry: LC_MAIN main + MH_TWOLEVEL images; ordinary positive ordinals; main preferred base
fixups: classic pointer rebase/non-lazy bind; independent lazy programs prepared eagerly; PTR_64 / PTR_64_OFFSET
unsupported: runtime lazy resolver; weak/threaded streams; authenticated/multi-start chains; other relocations
errno: fresh main-thread 4 bytes; error => -1 + errno; success preserves errno
SG_READ_ONLY: current R after fixup; original declared maximum is retained
gates: RX/max RX; errno: RW/max RW; both resident model reservations
budget: one preparation/execution deadline; one event per trap; bounded metadata/model memory
preparation timeout: typed Timeout, zero guest work; original format/backend errors propagate
report v1: optional darwin.native_calls[] = {pc,module,name,arguments,result}
call carriers: hex; full GP bits or defined ARM64 int vararg bytes; exact arity; null result if incomplete/nonreturning
open: 2 args or 3 with O_CREAT; ARM64 mode int at SP, x64 third GP; kernel mode_t 16 bits
basic: main status 58; stdout_hex 4100ff4221; named calls 14; events 15
classic lazy: eager stable slots; status 83; stdout_hex 41005a53; named calls/events 13
_exit: status 35; stdout_hex 58; named calls 2; events 2
file calls: main status 73; stdout_hex 460044; named calls 40; events 41
memory calls: main status 79; stdout_hex 4d0050; named calls 18; events 19
native common catalogue: 82; required methods/platform: 104; matching ARM64 identities: 312
```

```json
{"backend":"unicorn","instruction_quantum":1024,"timeout_microseconds":5000000,"arguments":["libsystem"],"darwin_files":{"files":[],"stdin_hex":"6162006364"}}
```

## 明確指定的啟動 dylib 與主執行緒 TLS

`darwin.modules` 以精確且區分大小寫的安裝名稱指定原始 Mach-O 路徑，最多 32 項；只讀取可達依賴。`@loader_path` 按字面比對，不搜尋宿主函式庫。MH_DYLIB 必須具有相同 ISA/平台及相符身分、版本；一般依賴序號選擇實際提供者。同名匯出與共享依賴保持身分，完整檔案、映射、TLS 和中繼資料使用共同預算。

支援普通 C 初始化/零初始化 TLS，對齊上限 16 位元組。準備時映射並計費每個模組的獨立儲存，首次存取複製重定位後的即時連續範本，保留先前對 BSS/間隙的修改。描述符與返回框架須通過驗證；TLS 頁不能被重新配置、解除映射或變更保護。原創樣例比較原生 O0/O1/O2 classic/chained 與五個平台的 C/CLI/Python。循環、弱/重匯出、選擇性 DSO 解構與舊式 TLS 初始化段、dlopen/dlsym、執行緒與完整框架尚未實作；Intel 原生及實體 iOS 未驗證。

```json
{"backend":"unicorn","instruction_quantum":1024,"timeout_microseconds":5000000,"darwin":{"modules":[{"name":"@loader_path/libneverd-base.dylib","path":"/inputs/libneverd-base.dylib"},{"name":"@loader_path/libneverd-leaf.dylib","path":"/inputs/libneverd-leaf.dylib"}]}}
```

```text
graph: strong acyclic startup; exact install name; same ISA/platform; MH_DYLIB + LC_ID_DYLIB
catalogue: max 32; only reachable inputs; no host lookup or @ path expansion
budget: aggregate full files; aggregate resident mappings/TLS/metadata; 65536 records
exports: regular / thread-local; ordinary dependency ordinal; rebased actual provider
TLS: initialized / zero-only / alignment <=16; exact 24-byte descriptor; live contiguous first copy
resolver: __tlv_get_addr; x64 RDI -> RAX; ARM64 X0 -> X0; one named event; errno preserved
lifetime: one main-thread block/image/process; stable addresses; fresh subsequent processes
addends: checked signed ordinary provider offsets; data/TLS initializers; original provider spans only
unsupported: cycles; weak/reexports/resolvers; gate/TLV/lazy addends; selective DSO finalization/legacy TLS init; dlopen/dlsym; threads
workload: shared dependency; duplicate export spelling; TLS aliasing; template mutation; main 83; stdout 440054
native common catalogue: 82; required methods/platform: 104; matching ARM64 identities: 312
```

## Darwin dylib 匯入偏移

一般強 dylib 匯入支援帶符號的位元組偏移，可初始化全域指標與 TLS 範本指標。`DarwinImage` 統一計算匯出位址、程式庫載入偏移及匯入加數，檢查溢位並限定在所選提供程式庫的原始區段內；指標值檢查不推測存取寬度。libSystem 入口、匯入的 TLV 描述元和延遲繫結仍要求零加數。

原創 O0/O1/O2 classic/chained 原生對照均傳回 78，輸出為空。C、CLI 和 Python 涵蓋全部五種設定；Intel 原生執行與 iOS 實機仍未驗證。

## 啟動建構函式與 TLS 結束回呼

原始 `S_MOD_INIT_FUNC_POINTERS`、唯讀 `S_INIT_FUNC_OFFSETS` 與單一 `LC_ROUTINES_64` 在 `LC_MAIN` 前執行；依賴依宣告順序後序初始化，共用映像只執行一次。入口限原始可執行段，ARM64 須有完整且對齊的四個位元組；待執行表異動會明確失敗。建構函式接收 argc/argv/envp/apple/`ProgramVars`，程式名取執行檔名稱而非自訂 argv0。C++ `thread_local` 包裝程式碼自行執行惰性建構。

`__tlv_atexit` 使用兩參數 void ABI，忽略編譯器額外 DSO 參數，保留 errno 與結果暫存器；事件結果為 null。預先計費的佇列最多 1024 項，含空回呼與重複項，物件值保留 64 位元。main 返回與 C `exit` 逆序執行，新註冊項先於舊項。結束堆疊位於目前 SP 與 x64 red zone 下方，保留深層退出時的堆疊物件。所有工作共用原預算，完成回呼才發布退出結果；C `_exit` 與原始 BSD exit 立即終止。重入 exit、選擇性 DSO 解構、舊式 TLS 初始化段與執行緒仍未支援。五種軟體設定、三種 ARM64 HVF 設定及獨立 O0/O1/O2 classic/chained 原生控制覆蓋這些路徑；控制樣本關閉堆疊保護，guard 資料匯入仍拒絕。Intel 原生與實體 iOS 未驗證。

## C/C++ 程序退出解構

C `atexit` 與 C++ `__cxa_atexit` 回傳整數零並保留 errno。C 回呼無參數，C++ 回呼接收完整物件值，DSO 僅保留為不透明事件資料。正常退出先完成一次主執行緒 TLS 解構，再依統一註冊順序逆序執行全域回呼；新註冊項先於舊項，全域階段不會重新清空 TLS。只有強註冊匯入才預留獨立的 1024 項全域佇列，既有 raw、建構函式與 TLS 預算不變。

原創雙 dylib 程式涵蓋編譯器產生的全域解構、重複存取區域 static、巢狀註冊、深層堆疊物件與解構中的直接 `_exit`。區域 static 控制使用 `-fno-threadsafe-statics`；`__cxa_finalize`、dlopen/dlclose、執行緒及完整框架仍明確拒絕。五種軟體設定、三種 ARM64 HVF 設定與 C/CLI/Python 沿用報告格式及預算；Intel 原生與實體 iOS 尚未驗證。

## 主執行緒 C++ 靜態初始化保護

只接納 `/usr/lib/libc++.1.dylib`（相容版本不高於 1.0.0）的精確強匯入 `___cxa_guard_acquire`、`___cxa_guard_release`、`___cxa_guard_abort`，加數須為零。其他 C++ 符號、libc++abi、別名、弱匯入、重匯出及覆寫內建庫仍拒絕；與 libSystem 共用兩頁，單獨 guard 不預留回呼佇列。

八位元組 guard 須非空且自然對齊。初始化位元組非零時 acquire 只讀此位元組並傳回整數零，不要求寫入權限或執行緒埠。零狀態需要明確非零 `darwin_system.thread_self_port`（`DarwinSystemOptions::ThreadSelfPort`）；埠 P 的 pending 為 `(uint64_t(P)<<32)|0x200`，acquire 傳回一，abort 恢復零，release 寫入 `(uint64_t(P)<<32)|0x101`。void 呼叫保留 errno/結果暫存器，報告結果為 null。遞迴、外部擁有者、等待位元、未知狀態及無效儲存在修改前失敗，不推斷宿主執行緒。

預設 static 雙 dylib 樣例涵蓋啟動、TLS/全域解構中新建物件、重複存取、完整 guard/哨兵、abort/retry 與正常/raw 結束；沿用 LIFO、預算與新程序隔離。五種軟體、三種 ARM64 HVF、C/CLI/Python 及 O0/O1/O2 classic/chained 原生控制共同驗證。ARM64 使用 `-march=armv8-a -mcpu=generic` 並檢查成品 LDARB、無 LDAPR；僅 march 仍可能產生 LDAPRB。RCpc、並行初始化、例外及選擇性 DSO 終結仍未支援；x64 控制經 Rosetta，實體 Intel/iOS 尚未驗證。
