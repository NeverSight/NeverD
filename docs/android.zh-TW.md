**語言**: [English](android.md) | [简体中文](android.zh-CN.md) | [繁體中文](android.zh-TW.md) | [日本語](android.ja.md) | [한국어](android.ko.md) | [Français](android.fr.md) | [Deutsch](android.de.md) | [Español](android.es.md) | [Italiano](android.it.md) | [Русский](android.ru.md) | [العربية](android.ar.md)

# Android Java 還原

[← 文件索引](README.zh-TW.md)

`neverd mobile` 透過另外安裝的 JADX 後端，將 APK、DEX 與 smali 輸入還原成可讀的 Java。此流程會驗證並暫存位元組碼、一併分析相關類別、檢查產生的輸出，最後發佈原始碼目錄與機器可讀的報告。這是實驗性的 CLI 功能。原生 C SDK、Python 外掛 SDK、GUI 載入器及 `neverd decompile --language` 尚未提供 APK 容器與 Java 輸出介面。

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
| NeverD | 建置 `neverd` 目標；發佈時也須附上與執行檔同層的 `mobile/` 目錄 | `build/bin/neverd` 或 PATH 中的執行檔 |
| Python | Python 3.10 以上，獨立於內嵌外掛主機的 Python 環境 | 依序使用 `--python`、`NEVERD_PYTHON`、PATH 中的 `python3`/`python` |
| Java 後端 | JADX 1.5.6 以上，含標準 DEX 與 smali 輸入外掛 | 依序使用 `--jadx`、`NEVERD_JADX`、PATH 中的 `jadx` |
| Java 執行環境 | Java 11 以上；若要以編譯及執行方式驗證結果，則需要 JDK | `JAVA_HOME` 或 PATH 中的 Java |

NeverD 不會自動下載相依套件。請取得完整的 [JADX 發行套件](https://github.com/skylot/jadx/releases/tag/v1.5.6)，保留其 `bin/` 與 `lib/` 目錄配置，重新散佈時也應保留附帶的授權文件。已測試的後端版本為 1.5.6；更新版本必須符合相同的 CLI 契約。這些相依套件的設定與 NeverD LLVM 管線的建置分開進行。

### Linux 與 macOS

```sh
cmake --build build --target neverd
python3 --version
java -version
/opt/jadx/bin/jadx --version

./build/bin/neverd mobile app.apk -o recovered-app \
  --python python3 --jadx /opt/jadx/bin/jadx
```

經常使用時，可設定 `NEVERD_JADX=/opt/jadx/bin/jadx`，並視需要將 `NEVERD_PYTHON` 設為直譯器路徑。若目前無法直接執行 Java，請將 `JAVA_HOME` 指向 JDK 安裝目錄。含空白的路徑必須加上引號。

### Windows PowerShell

```powershell
$env:JAVA_HOME = 'C:\Tools\jdk'
& .\build\bin\neverd.exe mobile .\app.apk -o .\recovered-app `
  --python 'C:\Tools\Python\python.exe' `
  --jadx 'C:\Tools\jadx\bin\jadx.bat'
```

後端的 `.bat`/`.cmd` 路徑會解析為發行套件中唯一的 `lib/jadx-*-all.jar`，由 NeverD 直接呼叫 Java。也可以將該 JAR 傳給 `--jadx`。應用程式路徑不會被插入命令殼層。多組態建置可能將執行檔放在 `build/bin/Release/` 下。若只移動執行檔而未附上同層的 `mobile/` 目錄，將出現輔助程式遺失的錯誤。

## 支援的輸入與範圍

| 輸入 | 行為 | 重要限制 |
|------|------|----------|
| `.apk` | 驗證完整 ZIP，再一起分析根目錄中的 `classes.dex`、`classes2.dex` 與後續編號的 DEX 檔案 | 僅處理程式碼；不解碼資源或 manifest |
| `.dex` | 驗證 DEX 檔案魔數，再交由後端解碼內容 | 變更副檔名或遭截斷的檔案不會因此成為有效位元組碼 |
| `.smali` | 分析提供的類別 | 不會隱含載入其參照的其他同層類別 |
| smali 目錄 | 遞迴收集 `.smali` 檔案，並一起分析 | 輸入目錄應包含巢狀類別與相依的 smali 根目錄 |

若要分析同時含有 `smali/` 與 `smali_classes2/` 的 APK 解碼目錄樹，請傳入兩者的共同目錄。後端只會收到 `.smali` 檔案，但整個輸入目錄樹都會先經過驗證與複製；無關的大型資源檔也會計入輸入限制。整理成只包含相關 smali 根目錄的精簡目錄，可減少處理工作量。

Split APK 各自視為獨立輸入。每個含 DEX 的 APK 都能單獨處理，但此命令不會合併整組 APK；只有資源的分割套件會因根目錄不含 DEX 而失敗。`.aab`、`.apks`、`.xapk`、`.odex`、`.oat` 與 `.vdex` 不屬於可接受的行動應用程式輸入。即使後端支援其中某些格式，也不表示 NeverD 的此命令支援它們。

APK 資源、`AndroidManifest.xml`、assets、JNI／原生函式庫，以及執行期間下載的程式碼，都不會還原為 Java。請另外擷取原生 `.so`，再使用 `neverd decompile library.so -o library.c`。此靜態流程要求加密或加殼內容已經以一般 DEX/smali 形式提供；不會執行脫殼、連接裝置或繞過保護。

## 選項與優先順序

```sh
neverd mobile app.apk -o recovered-app --platform=android \
  --jadx /opt/jadx/bin/jadx --timeout=600 \
  --max-files=30000 --max-bytes=4294967296 --json
```

| 選項 | 預設值 | 意義 |
|------|--------|------|
| `-o DIRECTORY` | 必填 | 位於任何目錄輸入之外的新輸出目錄；不覆寫既有輸出 |
| `--platform=auto\|android` | `auto` | 明確選擇 Android，或根據輸入推斷平台 |
| `--jadx PATH` | 環境變數／PATH | 後端啟動程式或發行套件的 JAR；明確指定的選項優先 |
| `--python PATH` | 環境變數／PATH | 執行隨附輔助程式的直譯器；明確指定的選項優先 |
| `--timeout N` | `300` | 每個後端程序的時限，單位為秒且須為正值；版本查詢也適用 |
| `--max-files N` | `20000` | 正值的項目數上限，包含實際建立的目錄 |
| `--max-bytes N` | `2147483648` | 輸入、解壓縮資料與最終輸出的位元組上限，須為正值 |
| `--json` | 關閉 | 以 JSON 列印報告，而非供人閱讀的摘要 |

`--arch` 的非預設值、`--artifact`、`--metadata-only` 與非零的 `--max-func` 屬於 iOS 選項，Android 會拒絕使用；明確指定 `--arch=auto` 仍可使用。此命令不提供任意後端選項的直接轉送。每次執行的後端設定、快取與暫存目錄彼此隔離；不會匯入使用者既有的後端設定與外掛設定。

這些限制用於控制資源，不是後端程序的沙箱。暫存工作區也會受到監控，為輸入、解壓縮資料與輸出預留空間，最多允許設定的項目數與位元組預算的三倍。每個程序的日誌上限為 16 MiB。大型輸入仍可能需要更多 Java 堆積記憶體或更長的時限；調高一項限制不會停用其他限制。

## 輸出配置與 JSON 報告

```text
recovered-app/
  sources/                 還原的 Java 套件與類別
  logs/jadx-version.log    後端版本查詢
  logs/jadx.log            後端診斷訊息
  report.json              含版本號的清單與還原限制
```

暫存副本與後端快取都會刪除。Java 檔案的確切名稱與數量取決於後端重建結果；巢狀類別可能與外層類別共用一個原始碼檔案。因此，Java 原始碼檔案數不等於 DEX 類別數。

以下是省略部分內容的報告範例：

```json
{
  "schema_version": 1,
  "status": "success",
  "platform": "android",
  "source": "app.apk",
  "input_kind": "apk",
  "backend": {"name": "jadx", "version": "1.5.6"},
  "input_code_files": ["classes.dex", "classes2.dex"],
  "dex_count": 2,
  "smali_count": 0,
  "java_source_count": 2,
  "java_sources": ["sources/example/Main.java", "sources/example/Peer.java"],
  "logs": ["logs/jadx-version.log", "logs/jadx.log"],
  "limitations": ["Java is reconstructed from bytecode; original comments, formatting, and stripped names cannot be restored."]
}
```

`input_kind` 為 `apk`、`dex`、`smali` 或 `smali-directory`。`input_code_files` 列出輸入位元組碼名稱或 smali 路徑，`java_sources` 與 `logs` 則是相對於輸出根目錄的路徑。`source` 是輸入的基本檔名。實際報告還會列出其他重建限制；將結果提供給其他工具時，請保留這些資訊。

自動化流程應先檢查程序結束碼，再讀取 `status`；若重新導向標準輸出，請將報告放在新輸出目錄之外：

```sh
neverd mobile app.apk -o recovered-app --json > recovery-result.json
```

輔助程式執行成功時回傳零，還原失敗時回傳非零。輔助程式啟動後，`--json` 會在失敗時產生包含 `schema_version`、`status: "error"` 與 `error` 的錯誤物件。原生命令列參數解析、Python 遺失或版本低於 3.10，以及輔助程式遺失等問題，可能在更早階段就透過 stderr 回報，而不產生 JSON。中斷也可能透過 stderr 回報。使用端必須處理這些情況。

## 失敗處理與疑難排解

結果採交易式發佈：保留既有輸出，並刪除失敗的暫存輸出。後端以非零狀態結束、日誌中的組譯／反編譯錯誤、因類別重複而省略內容、明確的不完整程式碼標記、空白 Java 檔案，以及未產生 Java 的結果，都會使命令失敗。後端回報成功，並不能獨立證明各方法正確。

| 症狀 | 處理方式 |
|------|----------|
| Python／輔助程式遺失 | 安裝或選擇 Python 3.10 以上，並讓 `mobile/` 目錄與 NeverD 執行檔保持同層 |
| 無法執行後端或版本不支援 | 檢查 `--jadx`、完整發行套件的目錄配置、Java 與後端最低版本要求 |
| DEX 標頭無效／根目錄沒有 DEX | 確認實際輸入格式；使用含程式碼的 APK、一般 DEX 或 smali |
| 找不到 smali 檔案 | 指定含 `.smali` 檔案的目錄，而非 Java 原始碼或只有 assets 的目錄樹 |
| 類別重複或只還原部分內容 | 移除重複的輸入定義，或分別分析相關位元組碼集合；修正格式錯誤的 smali，不要接受不完整結果 |
| 逾時／位元組或項目數超限 | 改用較小的相關輸入，或有意識地調高對應限制 |
| 壓縮檔路徑或連結不安全 | 重新建立一般且可跨平台的輸入，排除路徑穿越名稱、連結、特殊檔案與衝突路徑 |
| 輸出已存在 | 選擇其他輸出目錄；不要重複使用先前成功結果的目錄 |

成功執行時會保留後端日誌。失敗的暫存目錄及其日誌都會刪除；後端以非零狀態結束時，錯誤訊息會附帶長度受限的診斷日誌尾端，逾時與資源預算錯誤則各有對應訊息。若要調查特定後端問題，請以獨立的輸入及另一個診斷目錄，透過後端自己的 CLI 重現。不能僅因失敗前已出現部分 Java 檔案，就推斷執行成功。

## 驗證與支援深度

```sh
cmake --build build --target check-neverd-mobile
NEVERD_BUILD_DIR=build python3 -m unittest discover -s scripts/tests -p 'test_mobile_*.py' -v
python3 scripts/test_mobile_android_backend.py --jadx /opt/jadx/bin/jadx --neverd build/bin/neverd
```

前兩個命令驗證行動應用程式元件與已建置 CLI 的契約；平台特定測試樣本的需求可能使某些測試明確標示為略過。真實後端測試程式還需要 JDK（`java` 與 `javac`）。它會建立單一 smali、跨類別／巢狀 smali、DEX 與真正的 multidex APK 案例，接著編譯並執行還原出的 Java。案例涵蓋分支、迴圈、陣列、例外處理、類別參照、格式錯誤輸入，以及重複類別造成的省略。這些結果只能佐證所測試的樣本，不代表任意應用程式都能完整還原。

[Mobile Decompilation 工作流程](../.github/workflows/mobile.yml) 會在 Linux、macOS 與 Windows 上使用 Python 3.10 和 3.13 執行元件測試，另在 Linux 執行以校驗碼固定版本的真實 Android 後端測試作業。關於獨立的 iOS 流程及其目前限制，請參閱[行動應用程式概覽](mobile.md)（英文）。
