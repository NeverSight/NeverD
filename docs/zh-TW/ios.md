**Languages**: [English](../ios.md) | [简体中文](../zh-CN/ios.md) | [繁體中文](ios.md) | [日本語](../ja/ios.md) | [한국어](../ko/ios.md) | [Français](../fr/ios.md) | [Deutsch](../de/ios.md) | [Español](../es/ios.md) | [Italiano](../it/ios.md) | [Русский](../ru/ios.md) | [العربية](../ar/ios.md)

# iOS 原生程式碼與原始碼恢復

[← 文件索引](README.md) · [行動應用程式概覽](../mobile.md)

`neverd mobile` 接受 IPA、`.app` 和 Mach-O，輸出原生 C、執行階段中繼資料，以及受支援原生方法本體的實驗性 Objective-C `.m` 和 Swift `.swift` 原始碼。釋出成功的結果仍可能包含未恢復方法，使用原始碼前請檢查覆蓋率報告。行動應用程式容器由 CLI 處理；原生 C SDK 可單獨載入選中的 Mach-O。

編譯會丟失註釋、排版、識別符號和來源語言結構。此流程重建原始碼表示，無法還原原始文字，也不能為任意應用證明行為等價。靜態恢復流程不會啟動被分析的應用。

## 開始使用與依賴

```sh
cmake --build build --target neverd
neverd mobile App.ipa -o recovered-ios
neverd mobile App.app -o recovered-arm64 --arch=arm64
neverd mobile executable -o metadata --metadata-only
neverd mobile App.app -o recovered-framework --artifact Frameworks/Example.framework/Example
```

使用支援 C++20 的工具鏈建置 `neverd` 目標。行動端工作流程在原生 CLI 內執行，不需要 Python 直譯器。Swift 簽名解名由 NeverD LLVM fork 中的 `LLVMSwiftDemangle` 提供；該 fork 的原始碼建置和相符版本的 LLVM 套件均包含此元件，NeverD 不另行取得 Swift 原始碼相依項目。建置和執行 NeverD 不需要本機安裝 Swift 編譯器或工具鏈。LLVM、Capstone 等原生函式庫相依需求仍然適用；發佈時請附上目前建置所需的函式庫及授權聲明。在 macOS 上獨立編譯產生的 Apple 語言原始碼和執行 Swift 行為迴歸測試，才視需要使用 Apple Clang、SDK 與 `swiftc`。

Swift 簽名恢復直接在 C++ 程序內讀取 `LLVMSwiftDemangle` 的結構化節點，不尋找或啟動外部解名程式，也不執行工具鏈探索命令。原執行檔路徑選項已移除，原解名環境變數不再讀取。`--metadata-only` 既不執行原生原始碼匯出器，也不執行簽名解名。

`metadata/swift-signatures.json` 的簽名清單以以下欄位記錄內建元件：

```json
{
  "demangler": {
    "name": "llvm-swift-demangle",
    "execution": "builtin",
    "version": "6.3.3"
  }
}
```

```sh
neverd mobile App.ipa -o recovered-swift --timeout=600 --json
```

## 輸入與選擇

IPA 必須包含唯一的頂層 `Payload/*.app`。`.app` 和 IPA 均通過 `Info.plist` 的 `CFBundleExecutable` 選擇主程式。兩種容器中的 `--artifact` 都相對於該應用目錄，只選擇一個內嵌執行檔，不遞迴分析所有 Framework 或 Extension。原始 Mach-O 輸入不接受 `--artifact`。

Fat 二進位檔案的 `--arch=auto` 優先順序是 arm64、arm、x86_64、i386。缺失或不支援的切片會明確失敗。目前來源語言輸出面向 arm64 和 x86_64；選擇其他架構不代表支援 Objective-C/Swift 原始碼恢復。選中切片若 `cryptid != 0` 會被拒絕，需要提供已經解密且可讀的分析輸入。歸檔和目錄輸入會拒絕不安全路徑、符號連結、特殊檔案及衝突條目。

## 選項與資源限制

| 選項 | 預設值 | 含義 |
|------|--------|------|
| `-o DIRECTORY` | 必填 | 不存在且位於目錄輸入之外的輸出目錄；保留已有輸出 |
| `--platform=auto\|ios` | `auto` | 推斷平臺或明確選擇 iOS |
| `--arch=auto\|arm64\|arm\|x86_64\|i386` | `auto` | 選擇一個 Mach-O 切片 |
| `--artifact PATH` | 主程式 | 相對於應用目錄的執行檔路徑 |
| `--metadata-only` | 關閉 | 只讀中繼資料，不恢復原始碼或呼叫工具 |
| `--max-func N` | `0` | 原生函式數量上限；零表示所有發現的函式；中繼資料模式忽略此項 |
| `--timeout N` | `300` | 正值總分析時間預算（秒）；子程序使用剩餘預算 |
| `--max-files N` | `20000` | 正整數條目預算；Swift 符號清單也有數量限制 |
| `--max-bytes N` | `2147483648` | 輸入、解包資料和最終輸出的正整數位元組預算 |
| `--json` | 關閉 | 以 JSON 輸出帶版本的報告 |

工作區會被監測，暫存輸入和中間輸出可使用配置條目/位元組預算的最多三倍。每個程序的日誌上限為 16 MiB，Swift 簽名 JSON 還受 32 MiB 限制。這些是資源控制，不是程序隔離。增加超時不會取消位元組或條目限制。因 `--max-func` 被排除但仍在中繼資料清單中的方法會保留為未恢復項。

## Objective-C 原始碼與執行階段結構

兩種原始碼報告模式都包含 `source_projection_graph`。其節點記錄最終具型別的原生函式本體、本地診斷、合併後的原生/Block `dependencies`，以及正式流程的 `closure_closed` 結果。本地檢查失敗與傳播而來的相依失敗使用不同原因；缺少具型別函式本體時，診斷也標為不完整。修正未繫結呼叫可能顯示新的相依。閉合節點只代表通過相依階段；每個方法還須通過產生及原始碼文字檢查，狀態才會成為 `recovered`。`native_dependency_graph` 仍是獨立的 LowIR 呼叫清單。

重複分析覆蓋率時，下列命令執行與 `--format=objc-methods` 相同的分析和發布檢查，但省略 `native_source` 及每個方法的 `source`。JSON 會加入 `sources_omitted=true`，方法身分、狀態、診斷、簽章、共用輔助函式參照和相依證據仍保留完整意義。渲染檢查仍會執行；需要可編譯原始碼時應使用完整模式。對應的 C 入口為 `neverd_objc_methods_summary_json(session, max_functions)`，結果以 `neverd_free_string` 釋放。

```sh
neverd export WMF --format=objc-methods-summary -o summary.json
```

原生載入器把執行階段方法記錄、可執行 IMP 地址和受支援的型別編碼繫結到明確的原始碼 ABI 位置。固定標量/指標繫結包含隱藏的 `self`/`_cmd`、未使用引數、獨立的整數/浮點暫存器組，以及受支援的棧引數位置。float/double 的位重解釋與數值轉換分別處理。型別提示只是原始碼輸出的輸入，不是經過認證的 ABI 證據，也不授權修改可執行程式碼。

`sources/objc.m` 將實際恢復語句放入 `@implementation` 方法本體，保留必要的 C 輔助函式和具有型別繫結的呼叫。可輸出的呼叫目標必須有受支援的原始碼繫結；未知目標或不完整依賴組仍是未恢復項。缺失定義、無效可執行地址、衝突編碼、不支援的 ABI 對映、不完整解碼和被 IR 校驗拒絕的結果，不會僅因存在宣告就被標為已恢復。

一般輔助函式的完整入口值經 COPY/PHI 傳到已有原始碼繫結的指標參數，且不存在衝突的純量用途時，可以為原始碼投影補全指標型別。這種推斷保留實體 ABI 位置與通用 IR 型別；函式主體及其完整原生相依群組仍須通過驗證。

若原生純量輔助函式的已觀察入口參數位於確切的回傳暫存器，且涵蓋完整結果寬度，便可證明部分路徑原樣回傳該參數。有界分析同時檢查每條回傳路徑、初始入口與迴圈回邊；呼叫及部分寫入會使該值失效。未宣告的入口值、未使用的參數佔位、SSA 初始標記與 PHI 均不能建立輸入證明。這僅用於原始碼投影，保持實體位置不變，且仍須完整驗證函式本體與依賴。

本地原生輔助函式可將額外暫存器中的完整寬度整數輸入（包括結果緩衝區指標）恢復為明確參數，但入口可觀察性分析必須與完整 LowIR 的讀取證據一致。呼叫者保存的輸入暫存器之後可作為暫存空間；保持型上下文仍要求不寫入非堆疊框架的保持型暫存器。呼叫產生的隱含定義（包括 SSA 版本 0）不能變成入口輸入，只有明確保留的位元組可跨呼叫追蹤。重新產生的呼叫者與函式定義共用相同的實體輸入位置。這不推斷外部 C 或 Swift 原型，完整函式本體與相依閉包仍須驗證。

固定 C 回呼型別在原始碼宣告和轉型中保留參數及回傳簽章。精確的 `swift_once` 匯入綁定初始化標記、`void (*)(void *)` 回呼和上下文，無回傳值。這會保留執行時呼叫，但不證明回呼主體已還原，也不證明共用初始化儲存的所有權；相依性不完整的方法仍不計為已還原。

已知的 Objective-C 執行期匯入呼叫保留明確的參數與回傳值綁定，包括 retain/release、自動釋放、強弱參照儲存、物件配置及固定簽章的屬性設定函式。ARM64 暫存器專用的 retain/release 入口讀取指定暫存器；匯入身分與 ABI 必須一致。關聯物件的讀取、寫入和清除保留物件、鍵、值與指標寬度的策略參數，產生的 C 使用公開執行期標頭。macOS 重新編譯測試比對物件生命週期、弱參照歸零、複製與清除行為。

對 libobjc 最佳化後的型別與選擇子查詢，保留精確的執行階段呼叫，包括空物件處理和自訂覆寫。回傳位元組保留呼叫端的原生轉換，不替換為推測的類別繼承關係檢查。

原始碼繫結僅在指標參數具有常數位址時建立經過驗證的直接類別物件身分對照。此對照僅限目前的繫結作業；後續每次作業都會重新驗證目前映像，包括類別名稱、元類別旗標及身分衝突。

Objective-C 匯出也支援一組固定、使用一般 C ABI 的 Swift 執行階段匯入：參照計數、原生及未知物件弱參照、物件中繼資料，以及存取檢查的開始與結束。產生的 C 保留這些呼叫，連結時需要 Swift 執行階段。專用暫存器入口、未知的 Swift 呼叫慣例及任意 Swift 符號仍不支援；匯入身分與純量載體必須精確符合。

獨立綁定支援 arm64 和 x86_64 上精確的 Darwin String → NSString 匯入 `_$sSS10FoundationE19_bridgeToObjectiveCSo8NSStringCyF`，以及可選 NSString → String 匯入 `_$sSS10FoundationE36_unconditionallyBridgeFromObjectiveCySSSo8NSStringCSgFZ`。兩者均保留所有權語義和 Clang `swiftcall`；連結需要 Swift Foundation 與 Swift Core。反向橋接將傳回的兩個 String 機器字作為無號 128 位元整數傳遞，在建立 SSA 前拆入明確的傳回暫存器。這只是位元載體，不代表已還原 String 配置或通用聚合 ABI。未知簽章和不完整的初始化相依性仍不受支援。

對於已確認的關聯物件 key 參數，NeverD 可將唯讀 Mach-O C 字串區域內的確定位址重建為共用的鍵識別。同一原始位址共用一個鍵，不同內部偏移保持不同身分。mobile 匯出會自動合併輔助函式；C API 在 `shared_identity_functions` 中列出名稱，跨方法原始碼檔案連結時，每個輔助函式只保留一個定義。這些鍵屬於重新建構的程式碼，不指向已載入的原始映像；其他用途的未繫結映像位址仍會阻止完整還原。

經過驗證的 `os_unfair_lock_lock`, `os_unfair_lock_unlock`, `os_unfair_lock_trylock`, `os_unfair_lock_assert_owner`, `os_unfair_lock_assert_not_owner` 匯入透過 `<os/lock.h>` 呼叫真正的 Darwin 實作，保留鎖位址、所有權檢查與布林回傳值。未知變體仍不受支援。整數呼叫結果只保留宣告的 ABI 位元：Darwin arm64 依正負號屬性將 8/16 位元結果擴展至 32 位元，其餘暫存器位元保持未知。方法宣告的回傳寬度在分支合併前進入 SSA，避免未使用的高位元遮蔽有效的低位元組結果。

當類別匯入、配置、字元儲存和修正資訊完整時，經過驗證的 Darwin `__cfstring` 記錄可重建為常數物件。保留 ASCII 位元組和 UTF-16 碼元，包括內嵌 NUL。相同的原始物件位址共用一個重建身分，不同記錄保持獨立。輔助函式列入 `shared_identity_functions`，連結時需要 Foundation。這不會允許對常數物件記錄進行未經驗證的原始記憶體存取。

對於邊界明確且不含指標重定位的 `__DATA,__llvm_prf_cnts` 數值計數器節，NeverD 可重建跨方法共用的儲存空間，保留映像中的初始位元組、相互重疊的 1–16 位元組讀寫及更新。mobile 匯出會合併儲存輔助函式；使用 C API 時，跨原始碼檔案連結的每個 `shared_storage_functions` 輔助函式只能有一個定義。重建儲存獨立於原始映像及其效能分析執行環境；位址逸出、有序記憶體存取、不完整映射和指標重定位仍不受支援。 帶效能計數的 block 回呼可以更新這類映像儲存，同時仍須保證私有位址不逸出；一般計數器寫入不再被當作私有堆疊框架寫入。

類中繼資料保留父類身份、例項起始位置/大小，以及偏移、寬度和對齊已校驗的標量/指標例項變數；宣告在必要處插入填充。依賴不可用例項佈局的方法仍標記為未恢復。Category 保留獨立的類/分類/地址身份和實現；類與分類清單中重複出現的完全相同記錄只計一次。外部 Category 在受支援時使用已有 Foundation 類宣告；未知外部類標頭檔案會報告為缺失依賴，不會虛構替代類佈局。

還原狀態也要求完整的類別宣告、本地祖先類別定義，以及產生的原始碼在每條使用路徑上先定義值、並在可達出口提供必要的回傳；只有配置已驗證、完整方法清單證明沒有自身一般方法的本地空父類別，才會產生空 `@implementation`。缺少這些證據或名稱與已知 Foundation 匯入項目衝突時，受影響的方法仍保留在涵蓋率分母中並註明原因，可獨立還原的類別繼續輸出；這些名稱檢查不涵蓋所有 SDK 名稱、iOS SDK 或版本。

支援的 Objective-C Block 呼叫必須具備完整的固定純量呼叫 ABI，包括隱藏的 Block 物件，以及所有參數和回傳值的載體。執行階段編碼 `@?` 只在宣告中擴大為 `id`，不能提供呼叫原型。全域 Block 參照保留共用物件身分。支援的同步純量擷取需要證明原生擷取儲存與呼叫流程。透過已驗證的 `objc_retainBlock` / `_Block_copy` 呼叫複製的強物件捕獲可以逸出，前提是每條可達建構路徑都已初始化捕獲儲存，並完整恢復呼叫、複製和銷毀函式本體。產生的描述符保留原始輔助函式 ABI 和所有權配置。弱參照/byref 所有權、未知配置和未經證明的使用者仍維持未恢復。 對於編譯器宣告為不逸出的 C block 參數，還須核對確切匯入、參數位置和完整回呼 ABI，符合時可建立繫結。此生命週期約束不表示記憶體唯讀。 合併連結 C API 原始碼單元時，每個 `shared_block_functions` 項目只能保留一份定義。mobile 匯出會合併相符的定義，並拒絕包括內部被呼叫函式差異在內的衝突。

協議方法宣告來自已解析的本機執行階段記錄，涵蓋繼承協議、必要/可選的實例方法與類別方法。一般列表與相對位址列表共用解碼器。只有所有相符的類別與協議宣告一致，才為 selector 提供固定呼叫簽章；格式錯誤的記錄與繼承循環不能提供型別提示。語法完整的結構、聯合與陣列指標以不透明指標傳遞，不推斷配置。`objc_metadata.protocols` 獨立呈現宣告，不計入已恢復實作，也不據此推斷類別遵循的協議。 其餘簽章完全一致時，有號與無號 64 位元整數回傳值可共用無號位元載體；較窄整數、浮點、指標或引數型別衝突仍會拒絕呼叫。

帶編譯器格式宣告的 NSString 呼叫，可從已驗證的常量格式物件恢復提升後的純量引數。順序與序號引數必須完整且型別一致。Darwin arm64 從八位元組堆疊槽讀取可變引數；x86_64 使用整數、浮點暫存器組與溢出堆疊槽。產生的呼叫保留省略號與動態派發。未知格式、計數寫入、long double 及未支援擴充仍會拒絕。 同一套格式分析亦支援 `NSLog` 等已宣告的 C 匯入，要求精確的動態函式庫匯出證據，並讓引數數量不同的呼叫共用正確的可變參數原型。

私有堆疊寫入會保留函式內任何位置讀取的每個位元組。只有證明堆疊框架邊界、入口別名不可變且位址未逸出後，NeverD 才能移除無人讀取的寫入，或縮短整數寫入中無人讀取的尾部。因此，僅未使用的堆疊填補包含未知位元時，提升後的純量引數仍可恢復。分析不會為未知位元補值。具有記憶體順序的存取、未繫結呼叫、模糊位址、有副作用的值及分析預算耗盡都會保留原始寫入。

分類中的同名方法即使執行時覆寫順序未知，也會保留已驗證的 ABI 宣告。只有所有相符宣告一致時，動態呼叫才可使用該 ABI；有歧義的實作仍不能被選為原始碼方法主體。型別衝突或格式錯誤的宣告仍會阻止呼叫還原。

已知的 `objc_enumerationMutation` 呼叫保留物件參數和後續執行路徑，因為已安裝的變更處理器可能返回。精確的 Darwin `__stack_chk_guard` 匯入綁定執行階段物件身分；還原原始碼保留堆疊保護值讀取、比較和 `__stack_chk_fail` 呼叫的可觀察行為。

對於匯入系統 Foundation 的 64 位元 Darwin 連結映像，NeverD 也會查詢內建、由編譯器擷取的框架宣告。同一選擇器的執行階段宣告與框架宣告必須全部相容；可變參數、不支援的聚合型別或平台間不一致的簽章仍維持未繫結。宣告庫只提供呼叫型別，不確定接收者的類別，也不產生函式本體。使用 NeverD 不需要在本機安裝 Apple SDK。 CoreData 使用獨立的宣告庫，僅由確切的系統框架相依項啟用。宣告歸屬於其公開標頭所屬的框架；間接包含的相依標頭不會啟用其他框架。

C 宣告庫也涵蓋 CoreGraphics 和 ImageIO 的匯出。不透明的影像及色彩指標、整數計數和浮點傳回值保留其宣告的 ABI。公共系統框架的路徑別名與匯出事實一起產生；私有路徑、不同的框架版本和未宣告的符號不會因此取得繫結。 mobile 宣告也使用載入器的型別語法：語法完整的聚合型別指標按不透明指標處理，不推測其配置。

對於已建立 Foundation 橋接繫結的 Swift 大型永久字串常值，可將其 UTF-8 位元組重建為共享靜態儲存。長度、標記、終止位元組、UTF-8 有效性、儲存不可變性及確切匯入必須同時獲得驗證。原有帶標記的表示及橋接呼叫保持完整；含內嵌零位元組的常值及其他儲存形式仍維持未繫結。

經驗證的常數 NSString 物件在賦值或儲存時，即使使用整數載體，只要保留完整資料位址的來源資訊，也能維持共享身分。純量立即數、不完整位址、數值運算及對物件私有位元組的存取不會因此取得繫結。

對於已證明不可變、且不涉及重定位的映像位元組，普通純量載入可還原為保留原始位元模式的常數。支援 1、2、4、8 位元組整數及 4、8 位元組浮點數。可寫或有歧義的儲存、有序載入和位址用途仍維持未繫結；同一運算式的數值用途不能為指標用途提供證明。

固定參數 C 呼叫也會使用編譯器擷取的宣告和 SDK 匯出、再匯出資訊。繫結必須符合實際 dyld 程式庫、符號及純量 ABI；弱匯入、未知提供程式庫和不支援的原型維持未繫結。產生的 C 使用獨立識別字連結原始符號。沒有語言分派表的一般同步呼叫保留實際呼叫及記憶體副作用。

當共用的原始碼控制流程分析證明每條到達路徑上的無號上界時，索引純量載入可使用重建的不可變位元組表。守衛、遮罩、原生整數寬度及回繞語意保持不變；寫入和區域變數位址逸出會使先前的證明失效。每張表最多包含 4,096 項、65,536 位元組。發布原始碼時重新檢查目前範圍、儲存、重定位及輔助函式的每處用途。完整指標寬度的位元模式若指向映像節，仍存在指標歧義；較窄的純量片段和區段填補本身不能證明指標身分。複製的位元組僅用於純量讀取，表位址不能逸出。可執行回歸將整數位元、包含負零的浮點位元及越界行為與原始方法逐項比較。

C 目錄包含 `sys/mount.h` 宣告，並保留架構專屬的連結符號：ARM64 使用 `getmntinfo`，x86-64 使用 `getmntinfo$INODE64`。輸出參數維持不透明的二級指標，模式參數與傳回值維持有號 32 位元整數，並執行精確的系統匯出檢查。不會推測檔案系統記錄布局或傳回緩衝區內容。

外部資料繫結要求各平台共有的非 TLS SDK 宣告及精確的程式庫匯出證據。產生的 C 參照實際符號儲存並保留後續記憶體存取，包括全域指標與其所指物件的區別。弱匯入、身分衝突及不支援的儲存維持未繫結。資料宣告不能證明 block 的建構或所有權。 目錄涵蓋 CoreData、CoreImage、CoreGraphics、ImageIO 和 CoreSpotlight 資料。內建字面值儲存透過在每個目標上編譯空集合和布林物件取得；只接受外部非 TLS 資料的直接位址，並執行相同的匯出檢查。產生目錄時，除 libclang 外，還須透過 `--clang` 指定 Clang 編譯器。

在 ARM64 上，外部 `kCIContextPriorityRequestLow` 與 `kCIContextUseSoftwareRenderer` 的儲存身分由完整 iPhoneOS 與 arm64 iPhoneSimulator SDK 中一致的宣告及精確的 CoreImage 匯入驗證。內容選項保留真實的指標載入與執行階段值；繫結不會替換字串鍵或算繪行為。

ARM64 的 `UIApplicationDidEnterBackgroundNotification` 也繫結到精確的 UIKit 外部儲存。兩個 SDK 的宣告一致；原始指標載入與執行階段通知身分維持不變。

這只是對執行階段資訊的有限重建，不承諾完整恢復屬性、協議、原始所有權標註、任意聚合型別、可變引數尾部、依賴異常的方法本體和模型未覆蓋的 Block/捕獲佈局。執行階段編碼只描述固定引數，不能證明原宣告不存在省略號。只有原生載入器已解析相關槽時才使用鏈式指標；未解析格式會保留診斷。

## Swift 原始碼與儲存佈局

結構化 demangler 輸出將可呼叫簽名與不可呼叫中繼資料分別分類。受支援簽名在恢復原生方法本體前，必須繫結選中二進位檔案的符號、入口和明確的機器 ABI。Swift 接收者遵循 Swift ABI，不替換成 Objective-C 隱藏引數。使用者提供的簽名檔案也只是需要驗證的提示。

實驗性輸出器可以建置受支援的自由函式、類方法、指定初始化器和固定佈局結構體方法，包括受支援的 mutating 接收者形式。類/結構體宣告和儲存欄位需要恢復出的佈局中繼資料。只有所需原始碼宣告和方法本體組成完整且受支援的依賴組時，才輸出原生呼叫。恢復的原始碼單元將宣告與方法放在一起，不通過橋接程式碼呼叫原始二進位檔案。

支援的 Swift getter/setter 本體來自原生實作，再組合為屬性。私有 backing storage 保留已確認的欄位配置，初始化器和其他方法使用同一組儲存名稱。只有屬性宣告或欄位紀錄，不能證明已還原存取器本體。

支援的配置式初始化器、平凡解構器/釋放器、型別中繼資料存取器和 `_modify`/resume 入口可以投影至已輸出的型別單元。每項都需要對完整原生流程與效果進行有界證明、實際已還原的環境/初始化器/屬性相依項，以及相關本體的例外處理與 IR 稽核。配置器的寫入必須與真實初始化器一致；`_modify` 必須綁定確切的可變欄位及繼續執行入口。執行階段中繼資料呼叫在還原型別內保留其模型語意。這些入口明確報告為編譯器原始碼投影，不代表個別還原出的普通方法本體或原始程式文字。

泛型或 resilient 佈局、async/throwing 函式、未知呼叫約定、不支援的訪問器/分配器/thunk、不完整初始化及未繫結的原生或執行階段依賴，會逐項保留為 `unrecovered`。僅有 mangled 符號或名義型別名稱不等於恢復了方法。符號裁剪和未分類的 demangler 節點會使覆蓋不完整或未知。

## 輸出與覆蓋率計算方式

```text
recovered-ios/
  artifacts/selected.macho
  sources/native.c
  sources/objc.m
  sources/swift.swift
  metadata/objc.h
  metadata/objc.json
  metadata/objc-methods.json
  metadata/swift.json
  metadata/swift-signatures.json
  metadata/swift-methods.json
  logs/
  report.json
```

只有能夠輸出原始碼時，才生成對應來源語言檔案。`objc.json` 儲存類、Category、例項變數和原始方法編碼，`objc.h` 儲存受支援宣告；`swift.json` 儲存名義型別中繼資料及 mangled 符號。簽名和方法 JSON 保留分類、省略項、原因及數量。日誌包含原生診斷，以及實際執行時的原生 Swift 匯出診斷，不再產生外部 Swift 工具鏈探索或解名日誌。`report.json` 中的輸出路徑相對於其目錄。選中的二進位檔案是分析產物，生成原始碼不會把它作為恢復橋接依賴來連結。

臨時包副本和中間後端 JSON 會被刪除。正常執行若沒有原生函式體，即使存在中繼資料也會失敗。中繼資料模式僅生成選中的檔案、`objc.h`、`objc.json`、`swift.json` 和 `report.json`，沒有原始碼目錄或方法覆蓋/簽名檔案；`native_function_count`、`objc_method_recovery`、`swift_method_recovery` 均為 `null`。所有模式均使用原生載入器解析的 Objective-C 中繼資料。Swift 中繼資料透過有界的原生映像讀取取得；不支援的修正、可重定位配置或參照會保留部分還原診斷。

下面的縮略示例明確展示部分恢復：

```json
{
  "schema_version": 1,
  "status": "success",
  "platform": "ios",
  "architecture": "arm64",
  "objc_method_recovery": {
    "status": "partial",
    "method_count": 3,
    "recovered_method_count": 2,
    "unrecovered_method_count": 1
  },
  "swift_method_recovery": {
    "status": "partial",
    "coverage_status": "partial",
    "method_count": 4,
    "recovered_method_count": 2,
    "source_body_method_count": 1,
    "compiler_projection_method_count": 1,
    "unrecovered_method_count": 2,
    "metadata_symbol_count": 5,
    "unclassified_symbol_count": 1
  }
}
```

最外層 `status: "success"` 表示已釋出通過校驗的輸出。方法覆蓋 `recovered`、`partial`、`unrecovered`、`no-methods` 描述的是已發現清單，不是語義等價或原程式完整性。每個未恢復方法都有原因。Objective-C 的 `recovered` 還要求執行階段中繼資料完整。空清單不能證明原程式沒有方法。

未恢復的 Objective-C 方法列可能包含 `native_backend: {status, reason, diagnostics}`，前提是唯一的後端結果與執行階段身分完全相符。這個有大小限制的選用摘要會保留後端的中間結果，即使宣告或配置檢查先失敗。方法列的主要狀態、原因和還原計數仍為最終依據；沒有摘要表示該證據無法取得。

Swift 的 `coverage_status` 只統計已分類的可呼叫項。整體 Swift `status` 還考慮未知符號，可為 `unclassified`、`unsupported-architecture` 或 `no-symbols`。不可呼叫中繼資料位於 `non_method_symbols`，狀態為 `not-callable`；未知符號使用 `unclassified`。`types`、`type_metadata_count`、`source_type_count` 分別記錄型別中繼資料/輸出型別單元，不得用來增加方法數量。

每個已還原 Swift 項目的 `source_representation` 為 `native-method-body` 或 `compiler-generated-from-type`。編譯器投影另保留 `compiler_projection_kind` 和 `compiler_projection_evidence`。`source_body_method_count` 計算已還原原生方法本體，`compiler_projection_method_count` 計算通過證明的編譯器投影，兩者相加等於 `recovered_method_count`。編譯器入口仍計入 `method_count` 分母，其確切身分必須出現在唯一對應的 `type` 原始碼單元中。只有型別中繼資料或相依項名稱不能增加已還原涵蓋率。原生批次 JSON 的編譯器項目和型別單元包含 `source`；mobile 的 `source_units` 僅保留描述、不含 `source`，完整原始碼請見 `sources/swift.swift`。

原生 Swift 批次報告的 `source_units` 記錄 `{kind, module, name, source, method_entries, method_identities}`，kind 為 `function` 或 `type`，每個 identity 為 `{entry, mangled_symbol}`。不同符號可以共用入口並保留各自的 ABI 輸出；每個已還原 identity 必須且只能出現一次，未還原 identity 不得出現。`method_entries` 必須精確等於 `method_identities` 的有序入口投影，允許重複位址；不能默默合併完全相同的重複 identity。批次 `source` 等於依序串接每個單元原始碼再加一個換行。Mobile 在 `sources/swift.swift` 儲存完整原始碼，在覆蓋率 JSON 保留單元描述。逐方法 `source` 用於檢視，直接串接無法正確重建類別宣告。

## 直接原生匯出與 SDK

```sh
neverd export recovered-ios/artifacts/selected.macho \
  --format=objc-methods --max-func=20 -o objc-batch.json
neverd export recovered-ios/artifacts/selected.macho \
  --format=swift-methods \
  --source-signatures=recovered-ios/metadata/swift-signatures.json -o swift-batch.json
```

Swift 匯出使用正常 mobile 流程由內建簽名解析器產生的結構化簽名清單。Objective-C 批次 JSON 包含 `native_source`、`native_function_count`、`objc_metadata`，以及逐方法 C 原始碼、函式名、返回型別和引數。Mobile 在生成 `.m` 前還會校驗宣告、方法本體和佈局，因此最終方法覆蓋可能少於批次 C 覆蓋。原生匯出成功也可能沒有任何已恢復方法。

每個方法另提供 `projection_diagnostics`，以 `items` 的 `code`、`reason` 及可用的語句、呼叫、值或相依證據記錄可獨立檢查的問題。`checks_complete: false` 表示前提不足或資源限制；空的部分報告不代表恢復成功。檢查與准入判定共用，既有 `status`、`reason`、`unbound_call` 契約不變。

批次 `native_dependency_graph` 從簽章受支援的方法出發，沿映像程式碼的直接呼叫遍歷最終 LowIR，保留呼叫者、區塊及指令位址、共用目標與循環；間接目標為 null。缺少所需 LowIR 函式或達到記錄上限時，`inventory_complete` 為 false；`targets_complete` 另要求所有記錄均有直接目標。不含未支援的方法根與未解析的間接目標，也不證明完整原生執行覆蓋或相依原始碼恢復。

已驗證的本地協定引用槽透過 `objc_getProtocol` 保持已註冊協定的身分。批次結果中的 `runtime_protocols` 相依清單包含原生被呼叫函式的需求。名稱衝突、宣告不完整、匯入槽及未解析重定位仍不綁定。獨立匯出會回報缺少協定註冊，不會產生可能取得空協定物件的方法本體。

Swift 執行階段 ABI 目錄從固定版本的上游宣告產生，並保留 C 或 Swift 呼叫慣例。已知指標和指標寬度的無號型別組成固定純量簽章。Swift 呼叫必須有精確、非弱的 libswiftCore 匯入；支援的兩類版本化中繼資料可用性不允許弱匯入。特殊參數暫存器、未知表示、未支援的可用性類別及衝突宣告仍被排除。尤其 swift_willThrow 需要編譯器另行加入的 swiftself/swifterror 屬性，宣告 DSL 並未包含這些屬性。呼叫保留副作用及既有回呼、位元組契約。用 `scripts/generate_swift_runtime_declarations.py --input RuntimeFunctions.def --source-sha256 <pinned-hash>` 重新產生，附加 `--check` 驗證目錄。宣告事實附有版本、內容雜湊與第三方說明。

固定 Swift 執行階段宣告也保留雙字回傳值：兩個指標，或宣告規定的中繼資料指標與狀態字。共用 ABI 層分配兩個回傳暫存器，原始碼驗證重新核對順序、型別和精確匯入。Box 配置與中繼資料查詢仍執行真實執行階段呼叫；聚合參數、未知配置和隱藏上下文仍不支援。

此外，經編譯器精確觀測的 URLRequest、Notification、URL、Data、Date 與 IndexPath Foundation 值橋接會保留 `swiftcall` 及提供者身分。間接結果與 context/self 透過專用暫存器上的 `swift_indirect_result` 與 `swift_context` 傳遞（arm64 為 x8/x20，x86_64 為 RAX/R13），不占用一般整數參數暫存器組。Data 保留明確的雙字傳輸。這些只是呼叫載體宣告，不代表已還原值配置或通用 Swift ABI。

Native 純量回傳值推斷可識別已驗證呼叫結果 ABI 後緊接的完整雙字擷取。暫存值身分、成員偏移、寬度與實體暫存器必須全部相符。輔助函式因此可以直接回傳第一個成員，同時保留一般暫存器檢視規則、呼叫覆寫規則，以及每條回傳路徑都具有已定義結果的要求。

固定 C 執行階段目錄也保留 32 位元整數載體及明確零擴充的布林結果。布林結果使用完整的無號位元組；這不提供布林參數的擴充約定。更窄的未知表示及採用 Swift 呼叫約定的 32 位元宣告仍不納入。執行階段測試涵蓋成功與失敗的型別轉換、準確的計數式保留及物件解構；呼叫與所有權效果始終保留。

儲存欄位中繼資料區分已知位元組配置與未知語言型別。穩定 ABI 的 Swift 類別即使在 arm64 上也使用指標寬度的欄位偏移變數；未公開的欄位可能具有空的 Objective-C 型別編碼。NeverD 保留型別缺失的事實，同時驗證偏移、大小、對齊與重疊。還原的 C 函式主體透過執行時期 ivar 查詢取得偏移。欄位型別不可用時，仍不會產生需要虛構型別的類別宣告。

穩定 ABI 的 Swift 成員若使用執行階段初始化的零填充偏移槽，仍可保留其中繼資料身分。此類類別標記為 `ivar_status: "runtime"`；未知偏移在 JSON 中為 `null`，欄位大小為零表示寬度由執行階段決定。方法本體可向既有執行階段類別查詢偏移，宣告的物件型別也可沿精確偏移槽追蹤；常值偏移仍要求已知配置。這不會重建 Swift 類別配置：獨立類別匯出仍拒絕未知配置及欄位型別。

對於已直接綁定的原生呼叫，只有證明被呼叫函式在入口、可觀察副作用之前讀取該指標恰好一次，且不寫入、保留、比較或另作他用時，才能將 ivar 偏移變數的位址替換為區域純量的位址。區域值由既有執行階段 ivar 查詢取得，原生 ABI 與被呼叫函式本體維持原樣。此有界證明僅接受平直控制流程，對用途不確定或分析超限的情況拒絕還原。C API 支援還原 `.cxx_destruct`，獨立 Objective-C 方法語法仍無法產生此 selector。

只有匯入呼叫的已驗證契約限定了非負讀取長度，並排除寫入、保留指標及比較位址身分時，才重建不可變位元組緩衝區。保留原始位元組與長度，其他指標用途仍維持未解析。精確識別的 Swift 標準函式庫斷言失敗會保留編譯器推導出的純量與堆疊載體、`swiftcall`、`noreturn` 效果及準確連結符號；C 診斷墊片仍使用宣告的 C ABI，並保留獨立的後續陷阱。只有面對這個已認證、不會保留指標的終止消費者時，才複製靜態字串與不朽 String 常值儲存；動態或帶有所有權的 String 值不會被視為常值。

對已載入 Mach-O 的會話，`neverd_objc_methods_json(session, max_functions)` 和 `neverd_swift_methods_json(session, signatures_json, max_functions)` 返回相應報告。零表示所有發現的函式。成功返回的字串用 `neverd_free_string` 釋放；`NULL` 表示失敗，原因見會話錯誤。這些 API 不載入 IPA 或 `.app` 容器。

繫結原始碼呼叫後，Objective-C 原生相依性推斷可將 `NativeAnalysis` 得到的 64 位元整數暫存器參數進一步縮窄為 32 位元。窮盡式 HighIR 使用證明必須確認該參數的每次出現都只精確觀察低 4 位元組：或經由零偏移位元組擷取，或作為已精確繫結呼叫的整數實參。證明按參數獨立進行；任何全寬、非零偏移、格式錯誤或耗盡預算的使用都會保留原始寬度。縮窄後的輔助函式會重新提升，且仍須通過一般函式本體與相依性閉包檢查；重寫 ABI 維持不變。

透過 Objective-C 入口 thunk 公開的 Swift 延遲初始化靜態物件 getter，僅在 `vgZTo` 符號、執行期方法簽章、述詞測試、`swift_once` 呼叫、儲存載入與 `objc_retainAutoreleaseReturnValue` 結果共同構成精確的編譯器模式時才能投影。述詞 `_Wz`、初始化器 `_WZ` 與儲存 `vpZ` 符號必須彼此吻合；原始第三個暫存器只能作為 once context，初始化器必須忽略該 context，且不得有一般直接呼叫端。投影會重建述詞與物件儲存，將 null 作為無關 context 傳入，並把初始化器保留為已檢查的相依項目。形狀、符號、參數用途或 callback 有任何偏差時均維持未復原。

具有已驗證 `cfcTo` 符號及執行期 `init` 簽章的 Swift Objective-C 建構子 thunk，也可在未宣告的第三參數暫存器僅作為一次精確 `swift_once` 呼叫的上下文出現時將其移除。述詞 `_Wz` 與初始化器 `_WZ` 必須相符；回呼必須忽略該上下文，且不得有一般直接呼叫端。投影傳入 null，僅推導述詞的八位元組儲存範圍，並保留其他所有控制流程、記憶體及呼叫效果；一般原始碼主體與相依閉合檢查仍然適用。

ARM64 once 回呼可以將除此之外未使用的 x2 上下文轉送給一次巢狀 `swift_once`，前提是外層 `_WZ` 符號及內層 `_Wz`/`_WZ` 配對精確相符、兩個回呼皆無一般直接呼叫端，且具有獨立型別證明的葉回呼忽略其上下文。探索與投影使用同一契約。投影傳入 null 並保留每條陳述式；void 回呼 ABI 僅允許捨棄純暫存器、暫存變數或常數回傳值。回傳運算式中的堆疊讀取、載入、呼叫及未閉合相依仍會被拒絕。

## 驗證與故障排查

macOS 上啟用 `BUILD_TESTING` 的建置提供 `check-neverd-mobile-ios`，透過 CTest 執行三組原生還原驗證。

Python 僅用於下列開發測試腳本；內建行動端還原在原生 C++20 CLI 中執行。

```sh
cmake --build build --target check-neverd-mobile-ios
ctest --test-dir build -L NeverDMobileTests --output-on-failure
python3 scripts/test_mobile_ios_backend.py --neverd build/bin/neverd
python3 scripts/test_mobile_ios_calls_backend.py --neverd build/bin/neverd
python3 scripts/test_mobile_swift_backend.py --neverd build/bin/neverd
```

macOS 上的 Objective-C 驗證腳本先編譯原始樣本，再還原 `.m`，最後只將產生的原始碼與獨立呼叫程式連結。純量腳本涵蓋整數邊界、分支、迴圈、指標讀寫、隱藏參數、float/double 位元身分、混合參數和堆疊參數。呼叫腳本另涵蓋訊息分派、繼承、Category、執行個體變數儲存、原生輔助函式及 Block 呼叫/擷取/共用身分。呼叫樣本要求 arm64/x86_64 × classic/default 每個變體還原 21/21 個方法，並通過 134/134 項獨立預期結果檢查。請對目前的原生 CLI 建置執行這些驗證。

嚴格 Swift 腳本檢查 22 個使用者宣告、3 個 getter/setter 入口和 9 個編譯器產生的可呼叫入口，任何項目都不能從清單消失。每個變體有 858 個原程式獨立預期結果檢查。腳本獨立編譯產生的 `.swift` 與呼叫程式，不使用原始 dylib、模組、橋接或手寫替代宣告。案例涵蓋純量/原生呼叫、類別初始化與儲存、結構按值/mutating 方法、浮點及堆疊參數、指標與迴圈。原生 C++20 CLI 的驗收要求是 arm64/x86_64 × classic/default 四個變體零略過：每組必須還原 25 個原生方法本體和 9 個編譯器投影，保留全部 34 個可呼叫身分，原程式與獨立編譯的產生 Swift 均須通過 858/858 項獨立預期結果檢查。這些結果僅適用於此樣本，不保證任意應用程式或原始程式文字的還原。腳本會拒絕涵蓋缺漏、原始碼編譯失敗及行為差異。

這四個變體的編譯目標是 macOS。新增的兩個編譯器入口分別為空值初始化器及其中繼資料存取器，兩者經過獨立的原生證明，並在同一個 `struct Empty {}` 原始碼單元中保留各自身分。通過此樣本不代表真實 iOS 應用程式已通過驗收。

三個腳本都支援 `--arch all|arm64|x86_64`、`--fixups both|classic|default`、`--timeout N` 和 `--work-dir NEW_DIRECTORY`。`--setup-only` 只驗證原始樣本，不測試還原。包含純量腳本在內，驗收要求完成所有指定的架構與 fixup 變體；缺少變體或主機無法執行指定架構均視為失敗，不允許略過。保留失敗產物可區分原始碼涵蓋缺漏、編譯錯誤與行為差異；宣稱已驗證前應查看目前測試結果。

[Mobile Real Applications 工作流程](../../.github/workflows/mobile-real-apps.yml) 使用[樣本清單](../../scripts/mobile_real_apps.json)中固定版本的公開應用程式。驗收要求獨立列出完整 iOS bundle 與 APK 中的全部 Mach-O/DEX、獨立重建原版與產生的原始碼，並比對行為。階段缺失、清單涵蓋範圍未知或必要 case 缺失都會使驗收失敗。真實應用程式的 recompile 與 behavior 階段目前仍未完成，因此保留 Experimental 標記；測試框架的防誤判測試通過不代表真實應用程式驗收通過。

結果釋出具有事務性：選擇新目錄，先檢查程序退出狀態，並把重定向的 JSON 放在該目錄外。失敗會刪除暫存輸出並保留已有結果。後端非零退出會附帶長度受限的日誌尾端。後端逾時會保留原始逾時訊息，若已擷取的日誌文字可用，會附加長度受限的尾端。預算失敗仍保留各自的訊息。原生 CLI 成功時回傳零，還原失敗時回傳非零。使用 `--json` 時，已處理的失敗包含 `schema_version`、`status: "error"` 與 `error`。參數解析、原生程式或相依函式庫啟動失敗，以及中斷仍可能只透過 stderr 回報。使用端應先檢查結束狀態。

加密切片需要可讀輸入；缺少架構時檢查可用切片；方法被省略時檢視其準確原因和中繼資料診斷。增加 `--max-func` 只對因數量上限被排除的函式有幫助。缺少佈局、簽名、外部標頭檔案、異常支援或 ABI 行為支援，需要補充實現或有效中繼資料，不能直接宣稱完整恢復。分發工具或生成的軟體包時保留適用的依賴許可證宣告。

原生純量輔助函式推斷也支援 float/double 暫存器參數與回傳值。共用 MedIR 入口位元組分析必須證明寬向量輸入只觀察一個低位純量通道。只有所有定義的低位寬度相同、捨棄的高位運算式沒有副作用，且每次使用都明確讀取該低位前綴時，才能縮窄原始碼區域 CONCAT 值。呼叫、儲存、分支位置與非 NaN 浮點值的精確位元型態均保留；不會為未知高位補造數值。

當葉子原生輔助函式轉送至已驗證的外部 void 尾呼叫，且沒有完整純量回傳值時，可使用內部 void 原始碼簽章。LowIR 必須逐一匹配已繫結呼叫及其合成回傳。葉子證明禁止寫入保留暫存器、框架暫存器、堆疊指標及連結暫存器；有界的逐位元組污點不動點還會拒絕儲存堆疊衍生值或將其傳給呼叫。控制流程、函式本體與相依性檢查仍然適用。此簽章不提供任何結果位元：讀取未知結果的呼叫端仍維持未復原。執行回歸檢查條件物件銷毀、呼叫端獨立回傳值，以及對讀取未知結果呼叫端的拒絕。

對於保存暫存器、使用堆疊框架並執行一般呼叫的原生輔助函式，有界 LowIR 分析必須證明每條退出路徑都恢復原有的保留暫存器位元組、堆疊指標及連結暫存器，才能產生 void 原始碼摘要。部分寫入、隱式零擴展、重疊儲存及呼叫破壞會使相關事實失效。經證明不含框架衍生位元組的位址所執行的儲存與本次呼叫的私有堆疊框架互不相交；部分或完整的框架衍生別名、儲存或逸出框架位址都會使證明失敗。唯一的堆疊框架借用例外是已有精確原始碼繫結的 `objc_msgSendSuper2` 首參數：它可以指向完全位於已配置堆疊框架內的完整 16 位元組 `objc_super` 物件，因為該執行階段合約只在呼叫期間同步讀取它。一般訊息、不完整指標、超出堆疊框架邊界的物件及其他任何堆疊框架參數仍會被拒絕。HighIR 位元組活躍性把堆疊槽值視為對該槽精確位元組範圍的有界讀取，而取得或傳遞其位址仍屬於逸出；格式錯誤或範圍重疊的存取仍採保守處理。因此，互不相交的私有儲存可以只捨棄未被讀取的尾端位元組。重新提升後，可移除在 HighIR 中完全沒有出現的輔助暫存器參數，再次執行管線。私有儲存消除仍由既有 HighIR 清理負責，標準參數與實際輸入使用保持不變。

arm64 UIKit 目錄也綁定 `UIProgressView` 的 `observedProgress` 物件結果及 `setProgress:animated:`；後者保留獨立的 `float` 和布林參數暫存器。兩項宣告皆要求編譯器產生的 iPhoneOS 與 arm64 iPhoneSimulator 證據一致，而且提供程式庫必須是精確的系統 UIKit；其他架構仍不受支援。

帶標籤的字面值字組可以保留不可變 C 字串池中的完整映像位址及精確最高位標籤。原始碼繫結只將已證明的位址重定位至現有的永久共用池，保留整數 OR、標籤與池內位移。數值或部分位址碰撞、來源衝突、可寫或帶重定位的池，以及直接作為記憶體位址的用法仍被拒絕；這不會推斷 Swift String 物件配置。

`UIProgressView` 的 `setProgress:` 要求已證明的接收者，可來自經過精確 ARC 身分呼叫保留的型別化屬性結果。已驗證的父類別與協定閉包選擇 `float` 參數。未限定接收者的呼叫仍有歧義，因為 UIKit 和本地類別也為同一 selector 宣告了物件參數 setter。

arm64 UIKit 目錄也記錄 `UIButton` 的 `setTitleColor:forState:`（物件參數及無號 64 位元 `UIControlState`），以及 `UIView` 的 void 方法 `invalidateIntrinsicContentSize`；完整裝置和模擬器 AST 的證據一致。已觀測的 `UIImageView → UIView` 繼承關係讓現有 receiver 證明可以區分本地物件 setter 與無關類別的同名浮點 setter。未知 receiver、子類別宣告衝突、父類別提供方缺失及 x86_64 仍不受支援。

字典查詢回傳未限定型別的 `id` 時，即使與透過 `new` 回傳已知類別的路徑合流，也不能取得該接收者型別。每條輸入路徑都必須保留接收者證據；即使參數是明確型別的指標，也不能據此在有衝突的浮點宣告中選中物件參數 setter。

Arm64 的 `+[NSSet setWithObjects:]` 保留 SDK 中以 nil 終止的可變參數宣告。首批僅支援精確的平台類別匯入和 selector 樁，以及所有入邊均證明完整八位元組機器值的不可變 Objective-C 字串參數。恢復在首個確定的 nil 處停止；首個物件為 nil 時無須證明堆疊尾參，也不讀取終止值之後的槽。共用 Darwin 可變參數 ABI 保留三個固定參數，其餘物件與 nil 位於堆疊上。發佈階段針對目前映像重新驗證宣告、提供程式庫、接收者、selector 及每個參數。動態物件、缺失或部分尾參寫入、逸出堆疊框架、本機覆寫和其他架構仍不受支援。 堆疊參數還須在線性私有堆疊框架中證明每次具體讀取的值；後續覆寫不能改變已讀出的值，重疊寫入、未知呼叫、逸出或控制流程會阻止認證。

原始碼匯出可逐一投影編譯器產生的 BOOL super getter，但必須由目前完整 ARM64 指令、CFG 與儲存/還原證明共同認證 Objective-C 入口、共享函式本體及 metadata accessor（`CMa`）。探索、繫結與轉譯共用同一契約。helper 保留實際 CMa 呼叫和已閉合相依項，隨後讀取經執行階段註冊的 SEL 儲存格，再按認證型別呼叫 `objc_msgSendSuper2`；不同 selector 保留獨立儲存格。loader 透過 LLVM Mach-O 符號及 export trie，並嚴格核對目前 segment/section 對映，重新認證本地連結屬性。投影不改變其他呼叫者或共享函式的全域 native ABI；缺失或過時證據仍會被拒絕。

arm64 Swift once 初始化器可透過已有型別的 native helper，在獨立靜態槽之間複製物件。統一的用途契約核對所有目前參數與每條路徑，依序保留 predicate 讀取、可選的 `swift_once`、來源讀取、目的寫入、`objc_retain` 與返回。契約接受四個用途參數，或另加一個未使用的輸入；參數移除仍僅由 native 推斷負責。探索與繫結都針對目前 image 重驗同一契約、精確回呼及儲存符號、互不重疊的八位元組範圍和回呼未使用的 context。ABI、控制流程、記憶體效果及相依檢查維持完整；別名、部分或有序存取、額外用途和過期證據仍會拒絕。

完整 iOS 實機與模擬器 SDK 宣告也確認 `UIGraphicsBeginImageContextWithOptions(CGSize, BOOL, CGFloat)` 是回傳 void 的固定 C 呼叫。ARM64 上，尺寸的兩個欄位使用 `d0`、`d1`，布林值使用 `w0` 攜帶的位元組，縮放值使用 `d2`。精確 UIKit 連結器匯出認證提供程式庫。原始碼保留真實呼叫與包裝函式的計數器更新；錯誤提供程式庫、不支援的架構和被變更的宣告仍遭拒絕。 本機包裝函式仍需獨立的狀態與回傳值證明。

同一組已驗證的 UIKit SDK 證據支援 `CGSizeFromString(NSString *)` 及其包含兩個 double 的 `CGSize` 回傳值。共用 ABI 保留 `d0` 和 `d1`，包括跨越另一次呼叫仍需使用的獨立回傳結果。目前匯入與簽章複核會拒絕錯誤提供函式庫、純量替代、修改的指標參數，以及缺失或交換的回傳載體。

ARM64 原生堆疊框架保存分析可讀取目前推斷入口簽名明確描述、按八位元組對齊的完整純量入堆疊參數槽。LowIR 讀取必須與整個槽精確相符。這些位元組仍是未知輸入值，不能證明已保存暫存器的身分或被呼叫函式私有堆疊框架內的位址。寫入、部分讀取、間隙及未宣告的槽不會取得此權限。原始碼函式本體、每個呼叫端的引數值和相依性閉包仍須驗證。

完整的裝置與模擬器 UIKit SDK 宣告將 `UIAccessibilityPostNotification` 綁定為 `void(uint32_t, id nullable)`：ARM64 透過 `w0` 傳遞無號通知值，透過 `x1` 傳遞物件指標。`UIAccessibilityAnnouncementNotification` 是外部 `const uint32_t` 儲存空間。精確 UIKit 匯入只證明儲存位址，原始碼保留原有四位元組讀取和實際呼叫，包括 nil 引數，不替換通知編號。錯誤提供者、弱匯入、寬度或正負號變化及回傳契約變化均被拒絕；其他延遲儲存相依性仍須獨立證明。

Mach-O generic64 鏈式重定位在 `DYLD_CHAINED_PTR_64` 與 `DYLD_CHAINED_PTR_64_OFFSET` 兩種格式下皆保留編碼中的高八位元。偏移格式先重建完整字組，再檢查溢位並加上偏好的映像基底位址，與 [dyld 的執行方式](https://github.com/apple-oss-distributions/dyld/blob/fd8d0c4d52320ebf64db34f3cb280310d905c5ae/common/MachOLoaded.cpp#L771-L792) 一致。載入後的位元組保留完整執行期值。一般程式碼與資料指標的歸屬檢查使用完整映射位址；帶標記的 Swift 字串需要獨立的字串表示證明。合法映射的高位址仍受支援。

獨立的不可變鏈式值讀取介面，僅對具有唯一歸屬、由檔案支援的不可變儲存空間中的精確 Mach-O 重定位槽，回傳完整的已解析執行期字組。未解析、歧義、匯入或重疊的修正記錄都會遭拒絕。此值不授予一般指標身分或複製重定位位元組的權限；原始碼消費者必須獨立證明其表示與重定位方式，包括 Swift 字串標記。

不可變 Swift 字串的 once 回呼，只有在完整驗證六指令呼叫端、十二指令共用函式和三指令位址存取器構成的精確路徑後，才能投影。原始碼保留兩次八位元組寫入的順序及真正的 `swift_bridgeObjectRetain` 呼叫；驗證字串標記後，以整個常值區段的共用儲存空間重建位址，同時保留目標符號的共用儲存空間。目前映像位元組、完整管線稽核、堆疊框架保存、強執行期匯入及最終原始碼函式都會重新驗證，不向共用函式或位址存取器授予通用 ABI。

ARM64 Objective-C 原始碼復原支援由呼叫端證明的最多八個 Swift 字串比較及其原始單一位元結果。零擴展前會驗證目前的入口 ABI、強匯入、精確呼叫點與所有使用端。產生的 C 以 `swiftcall` 的 `_Bool` 宣告執行階段函式，再明確轉換為 `uint8_t`，保留原有遮罩與周圍呼叫。發布時重新檢查 LowIR 與管線稽核，拒絕重複求值及來自其他函式的證據。一般位元組回傳、原生呼叫猜測以及 patch、lift 模式不使用此正規化。 每個呼叫點必須獨立通過證明；其他原始布林呼叫只證明輸入位置，不宣告任何結果位元組已定義。發布時，每次保留的求值都必須對應不同的目前機器呼叫點。

手動 Swift String ABI 取證工作流程也支援固定的 `prefix` 探針，用於 `String.hasPrefix`。它保留兩個 ARM64 iOS SDK 的獨立 Swift/C 編譯輸出，並要求精確的 `swiftcc i1(i64, ptr, i64, ptr)` 呼叫。清單記錄探針、符號、編譯器身分及原始碼雜湊；收集證據不會自動安裝原始碼繫結。

逐次呼叫的布林值證明現已支援精確的 `String.hasPrefix` 匯入。Xcode 26.5 的裝置與模擬器探針確認它是四參數 `swiftcc i1` 呼叫，前綴 String 的兩個值位於接收者 String 之前。產生的 `_Bool swiftcall` 宣告與五參數比較助手分別匹配，發布時同時複核目前匯入種類及每次呼叫。連續兩次前綴判斷保留原有分支順序和副作用。

強綁定的 Swift 泛型單載荷列舉標籤函式現在使用其宣告的 `swiftcall` ABI：分支與標籤值為 32 位元，後設資料與回呼為指標，讀取函式回傳 32 位元標籤，寫入函式沒有回傳值。弱匯入或其他模組的同名函式不適用；此綁定不會推斷未定義的回傳高位或回呼函式主體。

固定版本的 Swift 宣告也為 `swift_initClassMetadata2` 與 `swift_updateClassMetadata2` 綁定五個指標寬度參數，以及雙字的 `swiftcall` 後設資料相依回傳值（後設資料指標、狀態字）。僅精確符合的 libswiftCore 強匯入適用。產生器現在也能從固定版本原始碼重現這些項目與泛型列舉標籤項目。
