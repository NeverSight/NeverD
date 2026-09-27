**語言**: [English](../android.md) | [简体中文](../zh-CN/android.md) | [繁體中文](android.md) | [日本語](../ja/android.md) | [한국어](../ko/android.md) | [Français](../fr/android.md) | [Deutsch](../de/android.md) | [Español](../es/android.md) | [Italiano](../it/android.md) | [Русский](../ru/android.md) | [العربية](../ar/android.md)

# Android Java 還原

[← 文件索引](README.md)

`neverd mobile` 預設使用 NeverD 內建引擎，將 APK、DEX 與 smali 還原為可讀 Java。獨立實作的讀取器共用帶型別的 Dalvik 模型與有界 Java 產生器。這是實驗性的 CLI 功能，不代表與 JADX 功能相等，也不保證任意 APK 都能完整還原。原生 C SDK、Python 外掛 SDK、GUI 載入器與 `neverd decompile --language` 均未提供 APK 容器與 Java 輸出的入口。

還原的 Java 是根據位元組碼重建的結果。原始註解、排版、原始程式語言的選擇與已移除的識別名稱皆無法取得；Kotlin 位元組碼也會輸出 Java。執行成功不代表已證明語意等價，也不保證每個方法都能重新編譯。此流程不會啟動受分析的應用程式。

## 快速開始

依下方說明準備執行環境後，選擇一個新的輸出目錄：

```sh
neverd mobile app.apk -o recovered-app
neverd mobile classes.dex -o recovered-dex
neverd mobile MainActivity.smali -o recovered-class
neverd mobile decoded/smali -o recovered-java
```

開啟 `recovered-app/sources/` 閱讀 Java 檔案，並查看 `recovered-app/report.json` 中的輸入清單與限制。smali 類別互相參照時，建議以目錄作為輸入。

## 執行環境設定

| 元件 | 要求 | 選擇方式 |
|------|------|----------|
| NeverD | 使用支援 C++20 的工具鏈建置 `neverd` 目標。行動端工作流程已編譯進原生 CLI，不呼叫 Python 直譯器。發佈時請附上目前建置所需的原生相依函式庫。| `build/bin/neverd` / PATH |

預設引擎以 C++20 實作，執行時不需要 Python、Java 或 JADX。它接受 DEX 035、037–040 與 smali 中可表示的一般宣告和操作。DEX 041、`invoke-custom` 等動態呼叫、部分初始化路徑、未知的語意註解或操作，以及無法以 Java 表示的識別名稱，都會明確失敗。接受某種檔案格式，不代表支援該格式中的所有指令與宣告。

Java 名稱繫結區分類別標頭與類別本體，可處理已知的同套件名稱遮蔽；若缺少外部父類別或介面宣告，導致無法確認型別名稱或產生的 Java 輔助程式碼參照的繫結對象，便會明確拒絕，目前僅在 `java.lang.Object` 缺少宣告時仍假定它不提供可繼承的成員型別。Smali 浮點數字面值直接捨入至目標單精度或雙精度，保留所得位元模式。

內建 C++ 引擎保留並驗證支援範圍內的類別、欄位與方法 `Signature` 中繼資料，包括型別變數、陣列、萬用字元、型別界限及方法層級的名稱遮蔽。泛型擦除必須符合原始 DEX 宣告的身分。`Throws` 不會被捨棄；無法證明例外繼承關係時會拒絕。泛型繼承或成員型別替換、橋接方法重新產生、泛型方法呼叫、參數化內部型別，以及建構式隱藏參數的對應，在缺少所需證明時仍明確不支援。

類別、欄位、方法及建構式上不含元素且執行時可見的 Java 8 `@java.lang.Deprecated` 標記會被保留。參數註解、帶有註解的靜態初始化器、其他可見性層級，以及 `since` 或 `forRemoval` 等元素值均會被拒絕。重新編譯後，CI 分別比對 classfile 的 `Deprecated` 屬性與執行時註解，並檢查未加註解的對照宣告及產生的輔助方法；輔助方法不計入原始方法數。

DEX 與 smali 也會保留類別、欄位、方法及建構式上的平台註解 `@android.annotation.SuppressLint`。它必須採用 build 可見性，且僅包含字串陣列 `value`；空陣列、空字串、重複值、順序及跳脫字元值都會保留。參數註解及帶註解的靜態初始化器仍不支援。CI 使用實際 Android SDK，檢查重新編譯後以 CLASS 保留的註解並比較程式行為。

內建引擎也會保留受支援註解宣告上執行時可見的 `@Retention`、`@Target`、`@Documented` 和 `@Inherited`，並輸出真正的 `@interface`。此子集不含欄位、方法、型別參數或巢狀宣告，僅支援名稱、存取權限及所屬關係均可確認的頂層註解和靜態成員註解；區域類別或匿名類別作用域會被拒絕。

類別、介面和註解宣告上的空 marker 套用，要求同一批分析的類別中包含可存取且相符的 marker 定義，而且其 Retention 和 Target 允許該套用。可以重建 `SOURCE` 宣告，但已持久化至輸入中的套用會被拒絕；`CLASS` 或未指定 Retention 的套用要求 DEX build 可見性（`0`），`RUNTIME` 要求 runtime 可見性（`1`）。缺少 Retention 與明確指定 `CLASS` 保持區別，缺少 Target 與空陣列也保持區別，Target 陣列順序不會改變。Target 值限定為 Java 8，`MODULE`、`RECORD_COMPONENT` 等較新值會被拒絕。保留 `@Inherited` 時，不會將繼承而來的套用複製成子類別上的直接宣告。

註解元素宣告與預設值、非空自訂註解套用、欄位／方法／參數上的自訂 marker、外部註解定義、可重複註解的容器和 `kotlin.Metadata` 仍不支援。marker 定義會輸出為類別宣告，不包含元素方法或輔助方法，也不增加已恢復方法數。

CI 使用自有 Java 8 範例，經 D8 與 NeverD 處理後重新編譯全部產生的 Java，並比對完整 `Signature` 中繼資料、反射結果與行為。這些範例檢查不代表真實應用程式已達到完整恢復標準。

### Linux 與 macOS

```sh
cmake --build build --target neverd

./build/bin/neverd mobile app.apk -o recovered-app
```

`NEVERD_JADX` 與 PATH 中的 `jadx` 不會選用外部引擎；只有明確指定 `--jadx PATH` 才會啟用外部轉接器。沒有自動後備機制。包含空格的路徑必須加上引號。

### Windows PowerShell

```powershell
& .\build\bin\neverd.exe mobile .\app.apk -o .\recovered-app
```

多組態建置可能將執行檔放在 `build/bin/Release/`。發佈時請遵循該建置的一般原生相依函式庫部署要求。

## 支援的輸入與範圍

下表描述還原操作接受的輸入。後文介紹的查詢模式採用較窄的驗證範圍。

| 輸入 | 行為 | 重要限制 |
|------|------|----------|
| `.apk` | 驗證完整 ZIP，再一起分析根目錄中的 `classes.dex`、`classes2.dex` 與後續編號的 DEX 檔案 | 僅處理程式碼；不解碼資源或 manifest |
| `.dex` | 內建讀取器驗證並解析 DEX 035 或 037–040 | DEX 041、不支援的宣告或操作會失敗；改名或截斷的檔案不是有效位元組碼 |
| `.smali` | 分析提供的類別 | 不會隱含載入其參照的其他同層類別 |
| smali 目錄 | 遞迴收集 `.smali` 檔案，並一起分析 | 輸入目錄應包含巢狀類別與相依的 smali 根目錄 |

若要分析同時含有 `smali/` 與 `smali_classes2/` 的 APK 解碼目錄樹，請傳入兩者的共同目錄。後端只會收到 `.smali` 檔案，但整個輸入目錄樹都會先經過驗證與複製；無關的大型資源檔也會計入輸入限制。整理成只包含相關 smali 根目錄的精簡目錄，可減少處理工作量。

Split APK 各自視為獨立輸入。每個含 DEX 的 APK 都能單獨處理，但此命令不會合併整組 APK；只有資源的分割套件會因根目錄不含 DEX 而失敗。`.aab`、`.apks`、`.xapk`、`.odex`、`.oat` 與 `.vdex` 不屬於可接受的行動應用程式輸入。即使後端支援其中某些格式，也不表示 NeverD 的此命令支援它們。

APK 資源、`AndroidManifest.xml`、assets、JNI／原生函式庫，以及執行期間下載的程式碼，都不會還原為 Java。請另外擷取原生 `.so`，再使用 `neverd decompile library.so -o library.c`。此靜態流程要求加密或加殼內容已經以一般 DEX/smali 形式提供；不會執行脫殼、連接裝置或繞過保護。

## 快速類別清單

```sh
neverd mobile app.apk --list-classes
neverd mobile classes.dex --list-classes --class-prefix com.example
neverd mobile app.apk --list-classes --class-prefix Lcom/example/ --json
neverd mobile app.apk --list-classes -o classes.txt
```

此查詢讀取類別身分，不解碼方法本體，也不產生 Java；不需要暫存工作目錄或外部執行階段。
文字輸出每行保留一個精確的 DEX 描述符，先依 ZIP 目錄順序，再依 DEX 定義順序排列。
`--class-prefix` 接受描述符前綴或以點分隔的套件前綴，採用字面前綴比對，不檢查套件邊界。
重複類別定義（包括跨 DEX 檔案）會明確失敗；無法無損表示為 UTF-8 的類別身分也會失敗。

未指定 `-o` 時寫入 stdout。查詢模式中的 `-o` 指定**新檔案**，不是還原目錄；既有檔案保持不變。
所有選定 DEX 成功前，結果都會暫存在記憶體中，因此後面的 DEX 格式錯誤不會發布部分清單。
`--json` 包含符合條件及總類別數、DEX 數，以及
`validation_scope: "dex-envelope-and-class-identities"`。

所有 ZIP 名稱、標頭、範圍與宣告的資源限制都會檢查。只有根目錄的 `classes.dex` 和編號的
`classesN.dex` 內容會解壓縮及檢查 CRC；無關資源內容的完整性未驗證。每個 DEX 仍會檢查標頭、
SHA-1、Adler-32、map 範圍和被參照的類別中繼資料。方法本體及未被參照的中繼資料不在驗證範圍內。
這是清單查詢，不是整個封存檔的完整性檢查，也不是 Java 還原證明。DEX 041、method-handle 和
custom-call-site 區段仍不支援。一般完整還原路徑仍會驗證每個封存內容。

`--timeout`、`--max-files` 和 `--max-bytes` 都會套用。類別限制在篩選前計算所有 DEX 的定義；
未選定的 ZIP 項目仍計入封存限制。清單操作不接受 `--jadx`、iOS 選項或 smali 輸入。

## 程式碼參照查詢

無須產生 Java 即可尋找指令的直接運算元：

```sh
neverd mobile app.apk --find-refs string --query 'login failed' --json
neverd mobile classes.dex --find-refs type --query 'Lcom/example/Service;' --exact
neverd mobile app.apk --find-refs method --query '->connect(' --owner 'Lcom/example/Client;'
neverd mobile app.apk --find-refs field --query 'Lcom/example/State;->ready:Z' --exact -o refs.jsonl
```

比對採用區分大小寫的字面子字串。`--exact` 比對完整目標：字串內容、型別描述符、
例如 `Lpkg/Type;->name(I)V` 的方法身分，或例如 `Lpkg/Type;->name:I` 的欄位身分。
`--owner` 以精確描述符限制方法或欄位參照的目標擁有者，不篩選包含參照的方法。
不支援空查詢、smali 輸入及混合查詢／還原選項；正規表示式特殊字元視為一般字面字元。

預設每次出現輸出一個精簡 JSON 物件（JSON Lines）；`--json` 傳回包含計數及 `references`
陣列的報告。每列記錄 `dex_entry`、完整的所屬 `method`、`pc_code_units`（從方法第一個指令
起算的 16 位元單位）、`opcode`、`kind`、`target_index`（DEX 內部索引）及 `target`。
保留所有出現次數，包括歸屬於多個方法定義的共用程式碼。字串列另含精確的 `target_utf16`
單位；若孤立代理碼使內容無法無損發布為 UTF-8，`target` 為 null。其他身分必須可表示為 UTF-8。

掃描器使用還原讀取器的指令邊界、運算元檢查、payload 處理及控制流程檢查。立即值及 switch／array
payload 資料不會被視為參照。即使沒有目標符合條件，也會先檢查每個已定義的程式碼本體，
再設定 `code_scan_complete: true`。`defined_method_count` 包含 native 和 abstract 宣告；
`scanned_method_count` 計算具有本體的定義，`scanned_code_item_count` 計算每個 DEX 中不同的
實體本體。`matching_pool_entries` 計算符合條件的目標，包括未被參照的目標。

`validation_scope: "dex-code-references"` 涵蓋 DEX 外層結構、識別碼表、類別／成員歸屬，以及
實際讀取的程式碼、例外和除錯資料。註解、靜態編碼值、僅出現在宣告中的使用及未被參照的中繼資料
不在查詢範圍內。這不是 Java 還原或 ART 驗證器。不支援的程式碼及格式錯誤的已讀取資料會明確失敗。
APK 驗證採用與類別清單相同的選定內容邊界；未選定資源內容的完整性未驗證。

所有選定 DEX 的結果都會先暫存，包括跨 DEX 重複類別檢查，然後才發布。選用的 `-o` 指定新檔案。
`--max-files` 分別限制累計類別定義數、方法定義數、結果出現次數，以及 ZIP 項目數。
`--max-bytes` 限制輸入、保留的查詢資料、每個本體的工作儲存及輸出（保守計入 JSON 展開量）。
這些是操作預算；程序 RSS 還包含輸入緩衝區、配置器額外負擔及執行階段。一般逾時與工作量預算也會套用。

## 選項與優先順序

```sh
neverd mobile app.apk -o recovered-app --platform=android \
  --timeout=600 --max-files=30000 --max-bytes=4294967296 --json
```

| 選項 | 預設值 | 意義 |
|------|--------|------|
| `-o PATH` | 還原時必填 | 新還原目錄；兩種查詢模式中則為選用的新輸出檔案 |
| `--list-classes` | 關閉 | 查詢 APK/DEX 類別身分，不進行 Java 還原 |
| `--class-prefix PREFIX` | 所有類別 | 字面描述符或點分隔前綴；須搭配 `--list-classes` |
| `--find-refs KIND` | 關閉 | 查詢 string、type、method 或 field 的直接指令參照 |
| `--query TEXT` | 參照查詢時必填 | 目標身分的字面子字串 |
| `--exact` | 關閉 | 比對完整參照目標身分 |
| `--owner DESCRIPTOR` | 任意擁有者 | method／field 查詢的精確目標擁有者 |
| `--platform=auto\|android` | `auto` | 明確選擇 Android，或根據輸入推斷平台 |
| `--jadx PATH` | 未設定：內建引擎 | 明確選用另外安裝的 JADX 相容轉接器；不透過環境變數選用，也不自動切換 |
| `--timeout N` | `300` | 內建分析的正值時間預算；外部後端則為每個程序的秒數上限，包含版本探測 |
| `--max-files N` | `20000` | 正值的項目數上限，包含實際建立的目錄 |
| `--max-bytes N` | `2147483648` | 輸入、解壓縮資料與最終輸出的位元組上限，須為正值 |
| `--json` | 關閉 | 以 JSON 列印報告；參照查詢未指定時輸出 JSON Lines |

非預設 `--arch`、`--artifact`、`--metadata-only` 與非零 `--max-func` 屬於 iOS，在 Android 下會被拒絕；明確指定 `--arch=auto` 則可接受。不支援傳遞任意後端選項。明確選用的 JADX 轉接器會隔離各次執行的設定、快取與暫存目錄，不匯入環境中的後端設定或外掛組態。

輸入、解包資料與最終輸出仍受檔案數和位元組預算限制。內建讀取器與產生器也會檢查有界工作量及經過時間。外部後端工作區最多使用設定項目數和位元組數的三倍，以容納暫存輸入與中間輸出；每個程序的日誌最多 16 MiB。這些是資源控制，不是安全沙箱。提高其中一項上限不會停用其他限制。

## 輸出配置與 JSON 報告

```text
recovered-app/
  sources/                       還原的 Java 套件與類別
  metadata/android-methods.json  內建引擎的方法覆蓋資訊
  report.json                    具版本的清單與還原限制
```

暫存輸入會被清除。巢狀類別可能共用外層類別的原始碼檔案，因此 Java 檔案數不等於 DEX 類別數。產生的方法可能使用 Java 分派迴圈，不執行原始 DEX，也不透過執行時橋接呼叫它。

內建報告包含 `android_method_recovery`，相同內容會寫入 `metadata/android-methods.json`，並保留每個原始方法。計數滿足 `method_count = recovered_method_count + projected_method_count + declaration_only_method_count + unrecovered_method_count`；未提供的 `projected_method_count` 視為零，發布前 `unrecovered_method_count` 仍須為零。原有 `native`、`abstract` 方法的狀態是 `declaration-only`，不計入已還原的方法本體。

部分具名且不擷取外部變數的區域類別可以還原到其精確所屬的靜態方法內。目前要求外層為一般純量方法，區域類別直接繼承 `Object`、沒有欄位、具有真實無參數建構子，其他方法為純量執行個體方法，且物件用途經過驗證、不會逸出支援的範圍。匿名類別、變數擷取、不支援的修飾詞和無法證明的用途仍會明確失敗。

這類輸出中的區域類別方法及所屬外層方法標記為 `source-projected`，`projection_kind` 為 `named-method-local`；即使外層流程報告為 `success`，方法涵蓋狀態仍為 `partial`。重新編譯後的二進位名稱和存取旗標均尚未驗證。Java 編譯器可能選擇不同的區域類別二進位名稱，因此 `class_source_bindings` 保留原始類別、精確所屬方法、原始碼路徑和區域名稱，並將 `binary_name_status` 標為 `unverified`。

`generated_source_helpers` 另列額外方法，類型精確為 `throw-helper`、`constant-helper`、`default-constructor` 和 `field-initializer`。最後一種表示額外產生、原始方法清單中不存在的 `<clinit>`。這些額外方法不計入原始方法總數。編譯成功或單次名稱一致不會將其升級為完整還原。以下簡化範例不含這類投影方法：

```json
{
  "schema_version": 1,
  "status": "success",
  "platform": "android",
  "source": "app.apk",
  "input_kind": "apk",
  "backend": {
    "name": "neverd",
    "version": "1",
    "execution": "builtin"
  },
  "input_code_files": [
    "classes.dex",
    "classes2.dex"
  ],
  "dex_count": 2,
  "smali_count": 0,
  "java_source_count": 2,
  "java_sources": [
    "sources/example/Main.java",
    "sources/example/Peer.java"
  ],
  "logs": [],
  "android_method_recovery": {
    "schema_version": 1,
    "status": "recovered",
    "class_count": 2,
    "method_count": 6,
    "recovered_method_count": 5,
    "declaration_only_method_count": 1,
    "unrecovered_method_count": 0
  }
}
```

`input_kind` 為 `apk`、`dex`、`smali` 或 `smali-directory`。`input_code_files` 列出輸入位元組碼名稱或 smali 路徑，`java_sources` 與 `logs` 則是相對於輸出根目錄的路徑。`source` 是輸入的基本檔名。實際報告還會列出其他重建限制；將結果提供給其他工具時，請保留這些資訊。

自動化流程應先檢查程序結束碼，再讀取 `status`；若重新導向標準輸出，請將報告放在新輸出目錄之外：

```sh
neverd mobile app.apk -o recovered-app --json > recovery-result.json
```

原生 CLI 成功時回傳零，還原失敗時回傳非零。使用 `--json` 時，已處理的失敗包含 `schema_version`、`status: "error"` 與 `error`。參數解析、原生程式或相依函式庫啟動失敗，以及中斷仍可能只透過 stderr 回報。使用端應先檢查結束狀態。

## 失敗處理與疑難排解

發布具交易性：保留既有輸出，失敗後刪除暫存結果。不支援的操作、未解析的暫存器資料流、無法表示的宣告、格式錯誤的例外處理與預算耗盡，會使內建流程失敗，不會發布缺漏的方法本體。外部轉接器也會拒絕非零退出、日誌中的組譯或反編譯錯誤、重複類別省略、不完整程式碼標記、空白 Java 檔案，以及未產生 Java 的結果。還原成功不是語意等價的證明。

| 現象 | 處理方式 |
|------|----------|
| 不支援的 DEX、指令、宣告或初始化 | 閱讀明確診斷並核對支援範圍；只有主動選用獨立相容轉接器時才使用 `--jadx PATH` |
| 輸入無效或類別重複 | 修正輸入位元組碼或類別集合；不支援的方法本體不會被默默省略 |
| 逾時或超出預算 | 縮小輸入，或依可用資源調整 `--timeout`、`--max-files`、`--max-bytes` |
| 輸出已存在 | 選擇新的輸出目錄 |

## 可選的 JADX 相容轉接器

`--jadx PATH` 選用外部 JADX，而非內建實作。必須安裝含標準 DEX/smali 輸入外掛的 JADX 1.5.6 或更新版本，以及 Java 11 或更新版本。請取得完整的 [JADX 發行套件](https://github.com/skylot/jadx/releases/tag/v1.5.6)，保留 `bin/`、`lib/` 結構，重新散布時保留隨附的相依套件授權文件。不會自動下載任何相依套件。轉接器報告會記錄實際的 `jadx` 引擎與偵測到的版本，不宣稱提供內建方法覆蓋資訊。

Windows 可指定發行套件的 `.bat`/`.cmd` 啟動器或 `lib/jadx-*-all.jar`。NeverD 會解析套件 JAR 並直接呼叫 Java，應用程式路徑不會進入命令殼層。`JAVA_HOME` 或 PATH 用於選擇 Java。成功執行的轉接器會保留 `logs/jadx-version.log` 與 `logs/jadx.log`；失敗的暫存目錄與日誌會刪除。後端非零退出會附帶長度受限的日誌尾端。後端逾時會保留原始逾時訊息，若已擷取的日誌文字可用，會附加長度受限的尾端。啟動失敗與預算錯誤仍保留各自的診斷。

```sh
neverd mobile app.apk -o recovered-jadx --jadx /opt/jadx/bin/jadx
python3 scripts/test_mobile_android_backend.py --jadx /opt/jadx/bin/jadx --neverd build/bin/neverd
```

## 驗證與支援深度

Python 僅用於下列開發測試腳本；內建行動端還原在原生 C++20 CLI 中執行。

```sh
cmake --build build --target check-neverd-mobile
ctest --test-dir build -L NeverDMobileTests --output-on-failure
python3 scripts/test_mobile_android_internal.py --d8 PATH --neverd build/bin/neverd
```

元件與 CLI 測試檢查解析、輸出契約及失敗清理。內建執行對照腳本使用 JDK（`java`、`javac`）與 D8 建立獨立 DEX/APK 範例，再編譯執行還原的 Java；這些是測試相依套件，不是內建還原的執行需求。請針對目前建置執行並檢查結果，再宣稱某個範例已驗證。獨立相容性測試還需要 JADX，用於驗證外部轉接器。範例成功不代表任意應用程式都能完整還原。

相關 iOS 流程請見 [行動應用程式概覽](mobile.md)。
