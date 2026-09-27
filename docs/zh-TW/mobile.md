**語言**: [English](../mobile.md) | [简体中文](../zh-CN/mobile.md) | [繁體中文](mobile.md) | [日本語](../ja/mobile.md) | [한국어](../ko/mobile.md) | [Français](../fr/mobile.md) | [Deutsch](../de/mobile.md) | [Español](../es/mobile.md) | [Italiano](../it/mobile.md) | [Русский](../ru/mobile.md) | [العربية](../ar/mobile.md)

# 行動應用程式還原

[← 文件索引](README.md) · [完整 Android 指南](android.md) · [完整 iOS 指南](ios.md)

`neverd mobile` 從 Android APK、DEX 和 smali 輸入還原可讀的 Java。對於 iOS IPA、`.app` 和 Mach-O 輸入，它會匯出原生 C，將受支援的 Objective-C 方法本體重建為 `.m` 原始碼，並提供實驗性的 Swift 原始碼及執行階段中繼資料。這是實驗性的 CLI 流程；原生 C SDK 和 GUI 載入器不接受行動平台容器。

## 設定

使用支援 C++20 的工具鏈建置 `neverd` 目標。行動平台流程編譯於原生 CLI 中，不使用 Python 直譯器。散布可執行檔時，須附上該建置所需的原生函式庫。

原生 ZIP 處理使用 zlib 執行 CRC-32 和 DEFLATE。CMake 優先透過 `find_package` 使用已安裝的函式庫；否則下載以固定 SHA256 驗證的 zlib 1.3.2，並以靜態方式建置。此行動 ZIP 實作在 Windows 上不需要 Python 輔助工具。請保留 [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md) 中的相依套件聲明。

預設引擎以 C++20 實作，不需要 Python、Java 或 JADX 執行階段。只有明確指定 `--jadx PATH` 才會選用另外安裝的相容轉接器；`NEVERD_JADX` 和 PATH 不會自動選用它，也沒有自動後備機制。選用轉接器需要 JADX 1.5.6+、標準 DEX/smali 輸入外掛，以及 Java 11+。其報告會列出實際的 `jadx` 引擎和版本。安裝方式與相依套件授權記錄於 [Android 指南](android.md#可選的-jadx-相容轉接器)。

## Android

若只需快速取得類別清單而不還原 Java，可使用
`neverd mobile app.apk --list-classes`，並可加上
`--class-prefix com.example` 或 `--json`。
[清單契約](android.md#快速類別清單) 說明排序、限制和選定內容的驗證。
查詢模式不需要輸出目錄；選用的 `-o` 指定新檔案。

若要尋找直接位元組碼參照，可使用
`neverd mobile app.apk --find-refs string --query 'example' --json`。
[參照查詢契約](android.md#程式碼參照查詢) 也涵蓋 type、method 和 field 運算元、
字面／精確比對、每次出現的位置、UTF-16 保留方式和程式碼驗證範圍。
未指定 `--json` 時，此操作輸出 JSON Lines，並採用相同的 `-o` 新檔案行為。

以下範例與輸出描述還原操作。

```sh
neverd mobile app.apk -o recovered-app
neverd mobile classes.dex -o recovered-dex
neverd mobile MainActivity.smali -o recovered-class
neverd mobile decoded/smali -o recovered-java
```

APK 根目錄的所有 `classes.dex`、`classes2.dex` 及後續編號 DEX 檔案會一起分析。smali 目錄會遞迴搜尋，所有類別（包括巢狀與同層類別）會在一次呼叫中共同分析。還原互相參照的類別時，請使用目錄。單一 smali 檔案只提供該類別。

內建還原輸出包含 `sources/`、`metadata/android-methods.json` 和 `report.json`，並標示 `backend: {"name": "neverd", "version": "1", "execution": "builtin"}`。報告內嵌 `android_method_recovery`：`method_count = recovered_method_count + declaration_only_method_count`，而 `unrecovered_method_count` 在發布前必須為零。原有 `native`／`abstract` 宣告與已還原本體分開計數。明確選用的外部轉接器保留自己的後端日誌。APK 資源、manifest、原生函式庫及動態載入程式碼不在此 Java 路徑範圍內；原生函式庫可另用 `neverd decompile` 分析。

內建還原讀取器共用獨立實作的具型別 Dalvik 模型，以及具有工作量上限的 Java 產生器，可處理能夠表示的一般 DEX 035/037–040 和 smali 程式碼。DEX 041、`invoke-custom` 等動態呼叫、部分初始化路徑、未知操作，以及無法以 Java 表示的識別碼都會明確失敗。產生的 Java 可能使用分派迴圈；它不執行原始 DEX，也不透過執行階段橋接呼叫原始 DEX。原始註解、格式及已移除名稱無法還原。此實驗性引擎不保證與 JADX 功能相等、語義等價，或能完整還原任意 APK。

## iOS

[完整 iOS 指南](ios.md) 說明 IPA、`.app` 和 Mach-O 的選擇、設定、全部 CLI 選項、原始碼結構描述、覆蓋範圍及驗證方式。

```sh
neverd mobile App.ipa -o recovered-ios
neverd mobile App.app -o recovered-arm64 --arch=arm64
neverd mobile App.app -o framework-analysis --artifact Frameworks/Example.framework/Example
neverd mobile executable -o metadata --metadata-only
```

每次執行選擇一個可執行檔。對於 IPA 和 `.app`，`--artifact` 都是相對於應用程式套件的路徑。Fat 切片依 arm64、arm、x86_64、i386 的優先順序選擇；來源語言投影以 arm64/x86_64 為目標。選定切片若已加密則拒絕處理。實驗性原生路徑輸出 C 和受支援的 Objective-C 方法本體，包括純量／指標、浮點、混合及堆疊 ABI 繫結。經驗證的執行階段類別／ivar 配置與獨立 category 會保留；未解決的配置、簽章、呼叫及其他相依項目仍明確列為缺漏。

Swift 還原使用 NeverD LLVM 分支的 `LLVMSwiftDemangle`，在 C++ 程序內分類簽章。不啟動外部名稱解碼器或工具鏈探索命令；建置和執行 NeverD 也不需要安裝 Swift 編譯器。該分支的原始碼建置與對應 LLVM 套件都包含此元件；NeverD 不會另行抓取 Swift 原始碼相依套件。簽章清單記錄 `demangler: {"name": "llvm-swift-demangle", "execution": "builtin", "version": "6.3.3"}`。受支援的簽章會先繫結至原生進入點和 ABI 位置，再產生真正的 `.swift` 函式、類別方法／初始化器及固定配置結構體方法。泛型／resilient、async／throwing、不支援的執行階段產生可呼叫形式，以及不完整的原始碼相依群組仍無法還原。

一般輸出包含 `sources/native.c`、選用的 `sources/objc.m` 和 `sources/swift.swift`、宣告與執行階段中繼資料、方法／簽章覆蓋 JSON、日誌、`artifacts/selected.macho` 和 `report.json`。不會有外部 Swift 工具鏈探索或名稱解碼日誌。產生的原始碼不會呼叫原始二進位檔作為還原橋接。Swift 的 `source_units` 將型別宣告與方法分組；不得串接獨立方法列來重建類別。外層 `status: "success"` 代表輸出已發布，不表示方法完整覆蓋或語義等價。

`--metadata-only` 不執行原生原始碼匯出器，也不執行簽章名稱解碼，不輸出原始碼或方法覆蓋資料。所有模式都使用原生載入器已解析的 Objective-C 中繼資料。Swift 中繼資料採用有界的原生映像讀取；不支援的 fixup、可重定位配置或參照會保留部分診斷。`--max-func` 限制原生函式還原，在僅中繼資料模式中忽略。缺少原生函式本體會使一般執行失敗。暫時解包的輸入會刪除。

原始註解、格式、已移除的識別碼及編譯時丟失的原始碼結構無法精確重建。使用輸出前，請閱讀每個方法的還原狀態與原因、Swift 分開記錄的可呼叫／不可呼叫／未分類計數，以及文件列出的限制。

## 限制與失敗

還原時，`-o` 必須指定任何目錄輸入之外的新目錄；查詢模式則接受選用的新輸出檔案。既有輸出絕不覆寫。還原作業先暫存，僅在還原成功並通過輸出驗證後發布。查詢結果則暫存至所有選定 DEX 都成功。原生 CLI 執行成功時回傳零。還原失敗回傳非零；`--json` 以 `schema_version`、`status: "error"` 和 `error` 回報已處理的失敗。參數解析、原生可執行檔或函式庫啟動失敗，以及中斷，也可能改在 stderr 回報。使用方必須先檢查結束狀態。

預設限制為 20,000 個項目、2 GiB 輸入／解壓縮或最終輸出資料，以及內建 Android/iOS 分析或每個明確選用的 JADX 程序 300 秒。iOS 子程序取得總分析預算的剩餘時間。內建讀取器和產生器也施加有界的工作量預算。透過 `--max-files`、`--max-bytes` 和 `--timeout` 調整這些正值限制。後端執行期間會監控暫存工作區，允許最多三倍項目／位元組限制，以便暫存輸入和中間輸出共存。診斷限制為每個程序 16 MiB。

還原期間，APK 暫存只寫出根目錄的 `classes.dex`、`classes2.dex` 及後續編號 DEX 檔案。每個 ZIP 成員仍會接受標頭／範圍檢查、解壓縮、長度和 CRC 驗證，並計入封存項目及未壓縮位元組限制。未寫出的資源可以使用區分大小寫的不同名稱，例如 `res/-A.xml` 和 `res/-a.xml`。完全重複的 ZIP 名稱與檔案／目錄身分衝突仍是錯誤；可攜式檔案系統的大小寫衝突檢查只套用於實際寫出的成員。包括 IPA 輸入的完整擷取仍拒絕這類輸出衝突。整個封存檔都拒絕路徑穿越、連結、特殊檔案及加密 ZIP 項目。目錄輸入也拒絕符號連結和特殊檔案。

這些限制是健全性控制，不是第三方後端程式碼的沙箱。明確選用的 JADX 與原生原始碼匯出命令以本機子程序執行。失敗的暫存輸出會刪除。非零後端結束碼會附帶有界的診斷尾端內容。後端逾時會保留逾時訊息，若有擷取的日誌文字，還會附上有界尾端內容。啟動失敗及預算違規保留各自的錯誤訊息。

## 驗證

Python 僅供下列開發測試工具使用；內建行動還原在原生 C++20 CLI 中執行。

```sh
cmake --build build --target check-neverd-mobile
ctest --test-dir build -L NeverDMobileTests --output-on-failure
python3 scripts/test_mobile_android_internal.py --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_backend.py --jadx /opt/jadx/bin/jadx --neverd build/bin/neverd
```

元件測試涵蓋解析、不安全容器、後端失敗、輸出清理、架構選擇和既有輸出保護。呼叫已建置 CLI 的測試使用 `NEVERD_BUILD_DIR`。內建 Android 測試工具使用 JDK（`java` 和 `javac`）與 D8 建立獨立 DEX/APK 樣本，然後編譯和執行還原的 Java。這些是驗證相依套件，不是內建還原的要求。將案例視為已驗證前，必須針對目前建置執行並檢查結果。獨立相容性測試另需 JADX；樣本成功不代表任意應用程式都能還原。

在具備 Apple Clang、其 SDK 和已建置 NeverD 的 macOS 上，執行真正的 Objective-C 執行對照：

```sh
python3 scripts/test_mobile_ios_backend.py --neverd build/bin/neverd
cmake --build build --target check-neverd-mobile-ios
```

此測試工具建置自己的 Objective-C 樣本，還原其方法實作，再只將還原的 `.m` 與相同的獨立呼叫測試程式連結。它的 22 方法樣本比較 141 個可觀測結果，涵蓋整數邊界、分支、迴圈、指標讀寫、隱藏及未使用參數、float/double 精確位元、混合參數及堆疊位置。將案例視為已驗證前，必須有目前建置通過的執行結果。每個要求的架構及 fixup 變體都必須完成；預設涵蓋 arm64/x86_64 × classic/default。缺少變體或主機無法執行某個架構都視為失敗，不允許略過。此樣本證據不能證明任意 iOS 程式都已完整支援。`NeverDMobileIOSBackend` 在 macOS 上註冊於 CTest，包括主要 CI 測試設定檔。

獨立 Swift 還原測試工具是 `python3 scripts/test_mobile_swift_backend.py --neverd build/bin/neverd`。它重新編譯產生的 Swift 和其測試程式，不連結原始二進位檔；不支援的可呼叫項目與行為差異都視為失敗。覆蓋語義與保留的失敗證據請見 [iOS 指南](ios.md)。

[Mobile Real Applications 工作流程](../../.github/workflows/mobile-real-apps.yml) 使用 [corpus manifest](../../scripts/mobile_real_apps.json) 中固定版本的公開應用程式。其驗收門檻要求：獨立盤點每個 APK 中所有 DEX 及每個完整 iOS 套件中所有 Mach-O、獨立重建原始程式與產生的原始碼，並比較行為。缺少階段、清單覆蓋未知或缺少必要案例都會使門檻檢查失敗。真實應用程式的重新編譯與行為階段仍未完成，因此支援狀態保留 Experimental 標籤；測試工具的防護測試通過不代表真實應用程式成功。

Android 案例嘗試只使用 Android SDK 宣告，以 javac 和 D8 編譯盤點出的完整 Java 產生結果。原始應用程式位元組碼、相依套件實作及替代 stub 不能填補還原缺漏。部分還原及編譯器錯誤會保留於證據中；即使編譯成功，仍需獨立驗證、完整 APK 重建及 ART 行為比較。iOS 判定基準將磁碟上的 Objective-C 方法記錄與 Apple 工具輸出核對，保留 class、metaclass、category、list 和 ordinal 身分。未解決的指標或省略的槽位會使清單保持未知。獨立的裝置與模擬器 SDK 宣告清單支援框架匯入工作，但不將標頭視為執行個體配置或行為的證明。
