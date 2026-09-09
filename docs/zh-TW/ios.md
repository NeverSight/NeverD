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

原生載入器把執行階段方法記錄、可執行 IMP 地址和受支援的型別編碼繫結到明確的原始碼 ABI 位置。固定標量/指標繫結包含隱藏的 `self`/`_cmd`、未使用引數、獨立的整數/浮點暫存器組，以及受支援的棧引數位置。float/double 的位重解釋與數值轉換分別處理。型別提示只是原始碼輸出的輸入，不是經過認證的 ABI 證據，也不授權修改可執行程式碼。

`sources/objc.m` 將實際恢復語句放入 `@implementation` 方法本體，保留必要的 C 輔助函式和具有型別繫結的呼叫。可輸出的呼叫目標必須有受支援的原始碼繫結；未知目標或不完整依賴組仍是未恢復項。缺失定義、無效可執行地址、衝突編碼、不支援的 ABI 對映、不完整解碼和被 IR 校驗拒絕的結果，不會僅因存在宣告就被標為已恢復。

類中繼資料保留父類身份、例項起始位置/大小，以及偏移、寬度和對齊已校驗的標量/指標例項變數；宣告在必要處插入填充。依賴不可用例項佈局的方法仍標記為未恢復。Category 保留獨立的類/分類/地址身份和實現；類與分類清單中重複出現的完全相同記錄只計一次。外部 Category 在受支援時使用已有 Foundation 類宣告；未知外部類標頭檔案會報告為缺失依賴，不會虛構替代類佈局。

還原狀態也要求完整的類別宣告、本地祖先類別定義，以及產生的原始碼在每條使用路徑上先定義值、並在可達出口提供必要的回傳；只有配置已驗證、完整方法清單證明沒有自身一般方法的本地空父類別，才會產生空 `@implementation`。缺少這些證據或名稱與已知 Foundation 匯入項目衝突時，受影響的方法仍保留在涵蓋率分母中並註明原因，可獨立還原的類別繼續輸出；這些名稱檢查不涵蓋所有 SDK 名稱、iOS SDK 或版本。

支援的 Objective-C Block 呼叫必須具備完整的固定純量呼叫 ABI，包括隱藏的 Block 物件，以及所有參數和回傳值的載體。執行階段編碼 `@?` 只在宣告中擴大為 `id`，不能提供呼叫原型。全域 Block 參照保留共用物件身分。支援的同步純量擷取需要證明原生擷取儲存與呼叫流程。逸出或非同步擷取、模型未涵蓋的物件/byref 所有權、copy/dispose 輔助函式及未知配置仍列為未還原。

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

對已載入 Mach-O 的會話，`neverd_objc_methods_json(session, max_functions)` 和 `neverd_swift_methods_json(session, signatures_json, max_functions)` 返回相應報告。零表示所有發現的函式。成功返回的字串用 `neverd_free_string` 釋放；`NULL` 表示失敗，原因見會話錯誤。這些 API 不載入 IPA 或 `.app` 容器。

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

嚴格 Swift 腳本檢查 22 個使用者宣告、3 個 getter/setter 入口和 7 個編譯器產生的可呼叫入口，任何項目都不能從清單消失。每個變體有 855 個原程式獨立預期結果檢查。腳本獨立編譯產生的 `.swift` 與呼叫程式，不使用原始 dylib、模組、橋接或手寫替代宣告。案例涵蓋純量/原生呼叫、類別初始化與儲存、結構按值/mutating 方法、浮點及堆疊參數、指標與迴圈。原生 C++20 CLI 的驗收要求是 arm64/x86_64 × classic/default 四個變體零略過：每組必須還原 25 個原生方法本體和 7 個編譯器投影，保留全部 32 個可呼叫身分，原程式與獨立編譯的產生 Swift 均須通過 855/855 項獨立預期結果檢查。這些結果僅適用於此樣本，不保證任意應用程式或原始程式文字的還原。腳本會拒絕涵蓋缺漏、原始碼編譯失敗及行為差異。

三個腳本都支援 `--arch all|arm64|x86_64`、`--fixups both|classic|default`、`--timeout N` 和 `--work-dir NEW_DIRECTORY`。`--setup-only` 只驗證原始樣本，不測試還原。包含純量腳本在內，驗收要求完成所有指定的架構與 fixup 變體；缺少變體或主機無法執行指定架構均視為失敗，不允許略過。保留失敗產物可區分原始碼涵蓋缺漏、編譯錯誤與行為差異；宣稱已驗證前應查看目前測試結果。

[Mobile Real Applications 工作流程](../../.github/workflows/mobile-real-apps.yml) 使用[樣本清單](../../scripts/mobile_real_apps.json)中固定版本的公開應用程式。驗收要求獨立列出完整 iOS bundle 與 APK 中的全部 Mach-O/DEX、獨立重建原版與產生的原始碼，並比對行為。階段缺失、清單涵蓋範圍未知或必要 case 缺失都會使驗收失敗。真實應用程式的 recompile 與 behavior 階段目前仍未完成，因此保留 Experimental 標記；測試框架的防誤判測試通過不代表真實應用程式驗收通過。

結果釋出具有事務性：選擇新目錄，先檢查程序退出狀態，並把重定向的 JSON 放在該目錄外。失敗會刪除暫存輸出並保留已有結果。後端非零退出附帶長度受限的日誌尾部；超時與預算失敗有獨立訊息。原生 CLI 成功時回傳零，還原失敗時回傳非零。使用 `--json` 時，已處理的失敗包含 `schema_version`、`status: "error"` 與 `error`。參數解析、原生程式或相依函式庫啟動失敗，以及中斷仍可能只透過 stderr 回報。使用端應先檢查結束狀態。

加密切片需要可讀輸入；缺少架構時檢查可用切片；方法被省略時檢視其準確原因和中繼資料診斷。增加 `--max-func` 只對因數量上限被排除的函式有幫助。缺少佈局、簽名、外部標頭檔案、異常支援或 ABI 行為支援，需要補充實現或有效中繼資料，不能直接宣稱完整恢復。分發工具或生成的軟體包時保留適用的依賴許可證宣告。
