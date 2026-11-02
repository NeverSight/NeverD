**語言**: [English](../../CONTRIBUTING.md) | [简体中文](../zh-CN/CONTRIBUTING.md) | [繁體中文](CONTRIBUTING.md) | [日本語](../ja/CONTRIBUTING.md) | [한국어](../ko/CONTRIBUTING.md) | [Français](../fr/CONTRIBUTING.md) | [Deutsch](../de/CONTRIBUTING.md) | [Español](../es/CONTRIBUTING.md) | [Italiano](../it/CONTRIBUTING.md) | [Русский](../ru/CONTRIBUTING.md) | [العربية](../ar/CONTRIBUTING.md)

<!-- i18n-source: af6c34901bacab00e5d1ed68b031f0a48a3ff64ac85ee208695647ec935b9790 -->

# 為 NeverD 做出貢獻

NeverD 是一個語意優先的二進位分析專案。有價值的貢獻應聚焦單一目標、
讓不支援的行為明確失敗，並包含足以證明變更契約的最小測試。

納入現有程式碼時，須保留著作權、授權與來源聲明，包括藉助 AI/LLM 工具複製或改作的程式碼。
請遵循[署名與引用指南](ATTRIBUTION.md)，註明重用內容的原始來源和版本，
並在提交前檢查其授權條款。第三方聲明應按其原有授權條款保留。

開始修改前，請閱讀[架構指南](architecture.md)。測試套件的選擇請參考
[測試指南](testing.md)，產品規劃請參考
[路線圖](roadmap.md)。

<!-- i18n-section: prerequisites -->
## 必要條件

- 支援遞迴子模組的 Git
- CMake 3.20 或更新版本
- Ninja
- 支援 C++20 的編譯器
- 啟用 Python 外掛時，需要 CPython 3.10+ 及其嵌入式開發檔案
- 用於完整跨目標 fixture 集合的 Clang 與 LLD（`ld.lld` 和 `lld-link`）

遞迴子模組提供 NeverD 的 LLVM fork、Capstone fork、Unicorn 與簽章資料。
驗證變更時，請勿將它們替換成任意的系統版本。

<!-- i18n-section: clone -->
## 複製並初始化

開發成果整合至 `dev`，它也是儲存庫的預設分支。複製時取得所有子模組：

```bash
git clone --branch dev --recurse-submodules \
  https://github.com/NeverSight/NeverD.git
cd NeverD
```

對既有複本，請在第一次建置前，以及任何改變子模組記錄版本的提交之後，
同步並初始化子模組：

```bash
git submodule sync --recursive
git submodule update --init --recursive
```

<!-- i18n-section: build -->
## 選擇建置設定

| 設定 | 用途 | 重要行為 |
|------|------|----------|
| Release | 一般開發、完整測試、解碼/提升基準 | 已最佳化；吞吐量具有代表性 |
| RelWithDebInfo | 分析或除錯已最佳化的熱點路徑 | 已最佳化並包含除錯符號 |
| Debug | 斷言、原始碼層級單步、局部正確性工作 | 未最佳化；解碼基準會顯著變慢 |

除非工作明確需要 Debug 行為，否則使用 Release：

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
python scripts/build.py build-release --parallel
```

預設會將 `third_party/llvm-project` 作為整合相依元件一併編譯。第一次建置通常
需要 30–60 分鐘，之後皆為增量建置。`CMakePresets.json` 也定義了 `release`、
`relwithdebinfo` 與 `debug` 設定/建置預設；上面使用明確建置目錄，是為了讓測試
開關清楚可見。

進行原始碼層級除錯時，請使用獨立目錄，不要重新設定 Release 建置樹：

```bash
cmake -S . -B build-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON
python scripts/build.py build-debug --parallel
```

絕不要用 Debug 建置報告解碼或提升吞吐量。基準測試使用 Release；效能分析需要
符號時使用 RelWithDebInfo。

<!-- i18n-section: allocator -->
### 程序記憶體配置器

Linux、macOS 和 Windows MSVC/clang-cl 建置預設啟用 `NEVERD_ENABLE_MIMALLOC`。
透過公開 CMake 輔助函式建立的工具、分析 worker 和測試使用 NeverSight mimalloc fork
的預設分支，遵循 NeverC 的宿主記憶體配置器方案。Unix 在每個可執行檔案中嵌入一次記憶體配置器；
Windows 使用一個 mimalloc DLL 和對應架構的 `mimalloc-redirect*.dll` 重定向動態 CRT
分配，覆蓋 libneverd 和原生外掛。兩個 DLL 都放在可執行檔案旁並安裝至 `bin`，發布時一併分發。
Windows 要求 `/MD` 或 `/MDd`，靜態 CRT 建置必須關閉 mimalloc。SDK 程式庫本身不強制嵌入
應用使用某個記憶體配置器。Windows ARM64 應使用原生 ARM64 建置；x64 記憶體配置器 DLL 無法重定向
x64 模擬所用的 ARM64EC CRT。MinGW 保留系統記憶體配置器。參見
[上游 Windows 要求](https://github.com/NeverSight/mimalloc/blob/main3/bin/readme.md)。

使用 `-DNEVERD_ENABLE_MIMALLOC=OFF` 獲取系統記憶體配置器基線或使用外部記憶體配置器；舊選項
`NEVERD_WORKER_MIMALLOC=OFF` 僅關閉 worker 的記憶體配置器。CMake 會在 address、thread、
memory、leak sanitizer 建置中自動禁用 mimalloc，包括組合及特定設定的編譯/連結標誌；
僅啟用 UBSan 仍相容。只透過目標專用標誌啟用 sanitizer 時，也須顯式設為 OFF。
Windows 可透過 `MIMALLOC_DISABLE_REDIRECT=1` 不重新建置就進行執行時基線比較，
`MIMALLOC_VERBOSE=1` 顯示啟用情況。

支援的建置入口 `python scripts/build.py BUILD_DIR` 每次建置前更新五個 NeverSight fork
的遠端預設分支。它們與 Capstone、Unicorn 一樣是 `third_party` 下的 Git 子模組。
指令碼解析實際分支、更新這五個子模組，並將已提交原始碼匯出至
`BUILD_DIR/third_party/<library>/<commit>/src`。未提交或已分叉的本機子模組工作會阻止建置，
不會被覆蓋；網路失敗也會停止建置。更新、重新設定和編譯全程鎖定建置目錄，直到下次呼叫前
使用同一原始碼快照，因此增量建置也會採用新提交。

`neverd-dependency-inputs.json` 記錄設定輸入、提交及匯出內容的 SHA-256，隨安裝產物分發，
不作為二進位檔案認證。`neverd-build-receipt.json` 僅記錄所請求目標和設定的成功或失敗，CI 保留
兩者。安裝拒絕失敗建置、已變輸入及未建置設定；產品 CI 先建置全部安裝目標再測試安裝。
主線可能在兩次執行間變化，跨平臺資格驗證前先比較提交。直接 `cmake --build` 檢查新鮮度，
拒絕過期快照；使用 Python 入口更新，其自動重新生成仍保持同一快照。
`FETCHCONTENT_SOURCE_DIR_NEVERD_*` 僅供隔離的本機 fixture 覆蓋，主線入口會拒絕它。
多設定產生器須顯式傳入 `--config`。透過入口編譯目標，再分別執行測試和 `cmake --install`；
直接傳入 `install` 目標會在編譯前被拒絕。

冒煙測試和效能比較見[記憶體配置器驗證](../testing.md#allocator-validation)。

<!-- i18n-section: acceleration -->
### 加速程式庫

常規建置啟用 Highway 位元組掃描、simdutf UTF-8 校驗、simdjson 傳輸解析和 oneTBB 流水線
排程，保留程序記憶體配置器。JSON 繼續使用 nlohmann DOM 和序列化；合法的純整數訊框直接從 SIMD
tape 複製，小數和負零沿用原數值轉換，超出 simdjson 範圍的輸入走原准入路徑。無效外部文字
保持按位元組保真的轉義。這些程式庫與 mimalloc 來自 `third_party/forks.json` 列出的 NeverSight
fork，相容性修復在各預設分支上；建置複製當前原始碼授權並記錄準確提交，不將這五個依賴
固定在舊發行版。

`NEVERD_ENABLE_HIGHWAY`、`NEVERD_ENABLE_SIMDUTF`、`NEVERD_ENABLE_SIMDJSON`、
`NEVERD_ENABLE_TBB` 預設均為 `ON`，可分別設為 `OFF` 進行比較。oneTBB 在批次間保留
worker，檢查其 8 MiB 堆疊，絕不建置或載入 `tbbmalloc` 及其記憶體配置器代理。大堆疊呼叫方複用可用
堆疊空間，小堆疊呼叫方獲得原生協調執行緒。首次使用後 fork 或動態卸載 SDK 的宿主必須使用原生
執行緒基線 `NEVERD_ENABLE_TBB=OFF`，保留排程器不支援這些生命週期。oneTBB 靜態連結至共用
核心，其符號僅供該核心內部使用。CLI 元件呼叫與 GUI worker 請求使用同一個排程器；
不發佈 TBB DLL/共用程式庫。連結 `neverd_shared` 的目標會自動選用共用排程器。
透過其他目標存取 SDK、同時連結元件封存程式庫的用戶端，必須對最終目標使用
`neverd_use_shared_task_scheduler`。獨立的元件工具保留各自的執行階段環境。
這項限定範圍的整合避免同一行程中存在多個 NeverD 排程器；上游仍將
[oneTBB 靜態建置](https://uxlfoundation.github.io/oneTBB/main/intro/static_linking.html)
列為不受支援，並不建議將其用於一般用途。

原生測試、關閉選項比較及排程器相容範圍見[加速驗證](../testing.md#acceleration-libraries)。

<!-- i18n-section: prebuilt-llvm -->
### macOS 上的預先建置 LLVM

Apple Silicon 貢獻者可避免在本機編譯 LLVM fork：

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_LLVM_PREBUILT=ON
python scripts/build.py build-release --parallel
```

CMake 會下載儲存庫設定的發行套件、驗證 SHA-256，並在後續建置中重用已解壓的
使用者快取。預先建置通道僅支援 macOS arm64；Intel Mac 與通用二進位建置必須
使用預設的本機 LLVM 建置。`NEVERD_LLVM_PREBUILT_TAG`、鏡像 URL、快取目錄和
明確校驗和等進階覆寫項記錄於 `cmake/NeverDLLVMPrebuilt.cmake`。

LLVM 修正應在 [NeverSight LLVM fork](https://github.com/NeverSight/llvm-project) 中維護。
先將修正提交並合併到 fork，再更新 NeverD 的 LLVM 子模組版本。
整合建置直接編譯該原始碼，請勿在 NeverD 中加入 LLVM patch 檔案或建置目錄原始碼覆蓋層。
預建套件必須從修正後的 fork 重新建置，才能驗證相同行為。

<!-- i18n-section: workflow -->
## 分支與提取請求流程

從最新的 `dev` 開始，並建立聚焦的主題分支：

```bash
git switch dev
git pull --ff-only origin dev
git switch -c docs/contributor-guide
```

提取請求應以 `dev` 為目標，不要假定某個發行分支。保持提交容易審查：每次提交
只做一件連貫的事，不包含產生的建置輸出和無關格式化；除非提案本身需要，否則
不要更改子模組版本。

<!-- i18n-section: code-style -->
## 程式碼風格

C 和 C++ 遵循 [LLVM Coding Standards](https://llvm.org/docs/CodingStandards.html)，
以 [`.clang-format`](../../.clang-format) 為格式權威，
[`.editorconfig`](../../.editorconfig) 負責編碼、換行和空白。
C++ 版本以 `CMakeLists.txt` 為準，上游風格文件不決定專案語言版本。只格式化修改的檔案：

```bash
clang-format -i path/to/changed.cpp path/to/changed.h
git diff --check
```

CI 使用 [`scripts/check_clang_format.py`](../../scripts/check_clang_format.py) 固定的版本；
復現結果時使用同一版本。格式檢查不會判斷標頭檔歸屬，也不會識別策略裸數字。
局部修正不進行整個儲存庫格式化。遵循周邊命名和拆分方式，平臺行為留在對應 loader/lifter/backend
邊界內，不透過純 C SDK 暴露內部 C++ 型別。

<!-- i18n-section: headers -->
### 原始檔與標頭檔佈局

- 新 C++ 原始檔和標頭檔使用 LLVM 檔名/用途橫幅、`NeverD Decompiler` 和 Doxygen
  `\file` 描述，參照 `lib/debug/PDBLoader.cpp`。保留版權和授權宣告。
- 可獨立使用的標頭檔使用 `.h`，顯式包含依賴；實現首先包含對應介面。其餘由 `.clang-format` 排序：
  同目錄私有標頭檔、`neverd/` 公開標頭檔、引號形式 LLVM/Capstone 標頭檔、尖括號標頭檔，不依賴偶然順序或傳遞包含。
- 新標頭檔使用路徑推導的 `NEVERD_..._H` guard，如 `neverd/sdk/Limits.h` 對應
  `NEVERD_SDK_LIMITS_H`；私有/工具標頭檔保留足夠目錄資訊避免重名，結尾 `#endif` 註明 guard。
  明確整理既有標頭檔時使用此佈局，不順帶修改無關標頭檔。
- 共享介面位於 `include/neverd/<component>/`，私有實作標頭檔放在 `lib/` 或 `tools/` 原始碼旁。
  位置不等於穩定公開 C SDK API。依賴無環，核心模型/reader 不依賴工作階段或傳輸適配層；
  模組演算法歸所屬目錄，CLI、SDK、worker 不重複語義。參見[架構](architecture.md)。
- 使用所屬命名空間及清晰名稱。新型別/欄位用 `UpperCamelCase`，函式用 `lowerCamelCase`，
  策略常數用 `kUpperCamelCase`。擴充既有介面遵循周邊命名，保留公開 C/Python 契約和既有 STL
  介面拼寫。標頭檔不加入命名空間級 `using namespace`。
- 有意多次包含的表沿用 `.def`/`.inc` 並說明巨集契約；它們不是獨立 `.h` 介面，不新增阻止
  重複展開的 guard。產生的標頭檔透過產生器修改。

獨立策略標頭檔示例：

```cpp
//===- Limits.h - SDK resource limits and defaults ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SDK request bounds, retained-state limits and default work budgets.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_LIMITS_H
#define NEVERD_SDK_LIMITS_H

#include <cstdint>

namespace neverd::sdk {

/// Traversal ceiling for a function without a recorded instruction extent.
inline constexpr uint64_t kMaxUnsizedInstructions = 65536;

} // namespace neverd::sdk

#endif // NEVERD_SDK_LIMITS_H
```

<!-- i18n-section: policy -->
### Limits、預設值與設定歸屬

直接加入標頭檔搜尋路徑的工具目錄使用 `ResourceLimits.h`。在不區分大小寫的檔案系統上，本地 `Limits.h` 會遮蔽系統 `<limits.h>`。

每個策略在所屬模組的 limits/configuration 標頭檔中只有一個權威定義，即使只有一個呼叫方。
包括輸入/輸出上限、遞迴/圖邊界、工作/迭代預算、啟發閾值、快取、分頁/批次、重試及超時；
同樣適用於 `constexpr`、巨集、裸比較、`min`/`clamp` 引數、構造/欄位初始化和預設引數。

先複用已有歸屬：

| 策略歸屬 | 定義位置 |
| --- | --- |
| 共享 core/pipeline | [`include/neverd/Limits.h`](../../include/neverd/Limits.h) |
| SDK | [`include/neverd/sdk/Limits.h`](../../include/neverd/sdk/Limits.h) |
| Web | [`include/neverd/web/Limits.h`](../../include/neverd/web/Limits.h) 或已有專門定義 |
| 其他程式庫模組 | 已有策略標頭檔/定義表，否則在所屬層建立專門 `Limits.h` |
| 僅 CLI、worker、GUI | 工具內 `ResourceLimits.h` 或 `Config.h`；core 不依賴工具標頭檔 |
| 呼叫方可選設定 | 所屬 `Config.h`、`*Options.h` 或 `*Configuration.h` 模型 |

策略標頭檔保持輕量、按用途分組。已有 `*Limits.def` 或設定表負責時複用，透過可獨立使用的標頭檔暴露 C++
檢視。不要在 `Limits.h`、`Constants.h`、`Config.h` 重複定義同一策略；共享 core 策略不放
SDK/工具標頭檔，無關模組策略不全部塞進根 `Limits.h`。

C++ 標量使用有型別的 `inline constexpr` 及既有命名空間（`neverd::limits`、`neverd::sdk`）。
名稱說明用途和單位，如 `kMaxSourceBytes`、`kDefaultPageRows`、`kRetryDelayMilliseconds`；
說明零/哨兵含義和特殊邊界。預設值與硬上限意義不同則分開。Options 模型可直接擁有預設值，
呼叫方複用模型/預設工廠或同一常數；有關聯的預設值和上限從共同定義推導，不重複裸數字。

適配層、持久化校驗、CLI/GUI/worker、Python/MCP 繫結複用權威定義，schema 上限及含限值
診斷也從同一值派生。跨語言值由既有表/產生的繫結派生，不適合產生時新增漂移檢查，並保持純 C
標頭檔相容。前端預覽/分頁可比引擎更嚴格，但單獨命名和歸屬；數值相等不代表同一策略。

二進位配置、ABI/協議/指令常數、整數範圍、具體證明演算法的計費項保留在語義模型。狀態初值、
索引運算、分配提示、獨立測試輸入不自動屬於策略；不可用被測實作常數替代測試獨立預期邊界。

審查定義及裸值使用，包括跨行保護、迴圈、巨集、預設值、計時器、繫結；核實每個新增/改動策略
的含義和歸屬，僅搜 `Max*` 不夠。集中定義保持型別、值、比較方向、哨兵及耗盡/錯誤行為；
策略變化分開處理，並執行[測試指南](testing.md)要求的受影響檢查。

<!-- i18n-section: artifacts -->
### 文件與本機產物

Markdown 應簡潔且可從原始碼核實。內部檔案用相對連結，CLI、公開 API、支援範圍、建置選項、
測試命令變化時同一 PR 更新文件。修改本指南時同步所有支援語言；先審閱譯文，再更新
`i18n-source` SHA-256，保持章節、示例、API/選項和連結目標一致。執行
`python3 scripts/check_docs_i18n.py` 檢查連結與翻譯同步。

本機效能報告、原始追蹤和耗時彙總儲存在忽略的建置或 benchmark-results 目錄。提交可複用
測量工具和回歸測試，不提交本機報告或複製的結果表。

<!-- i18n-section: tests -->
## 執行測試

透過彙總目標執行所有已註冊測試：

```bash
python scripts/build.py build-release --parallel
cmake --build build-release --target check-neverd
```

開發期間使用最小的相關目標或 CTest 標籤：

```bash
# Main Unicorn differential suite
cmake --build build-release --target check-neverd-semantic

# Lifter/loader/format binary only
python scripts/build.py build-release --target NeverDLiftTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDLiftTests$' --output-on-failure --parallel 4
```

[測試指南](testing.md)記錄所有便利目標、僅標籤的轉換套件、單一測試
正規表示式、fixture 編譯與 Unicorn 往返。如果某個目標因缺少交叉編譯器或
連結器而跳過，請回報此限制；不要把未執行的路徑描述成通過。

<!-- i18n-section: python-sdk -->
### Python 外掛 SDK

修改 Python 外掛 SDK 時，先執行純 Python 和漂移檢查，再執行原生適配層測試：

```bash
PYTHONPATH=pluginsdk/python python3 -m unittest discover \
  -s pluginsdk/python/tests -v
python3 -m mypy --config-file pluginsdk/python/pyproject.toml \
  pluginsdk/python/neverd_plugin
PYTHONPATH=pluginsdk/python python3 scripts/check_python_plugin_sdk.py
python scripts/build.py build-release --target \
  NeverDPluginRuntimeTests NeverDPythonRuntimeTests NeverDPythonPluginTests
ctest --test-dir build-release -R 'NeverD(Python|Plugin)' --output-on-failure
```

公開 C 宣告、Python ABI 簽名、套件/CMake 版本和交付工作流策略屬於同一審查契約，同次變更
一起更新。參見 [Python 外掛指南](python-plugins.md)。

<!-- i18n-section: review -->
## 提取請求檢查清單

請求審查前：

- 依維護者偏好的流程 rebase 或 merge 最新 `dev`，並有意處理子模組變更。
- 以 Release 建置受影響的目標；若需要其他設定，請說明原因。
- 執行精準的回歸測試與實際可行的最廣相關套件；在 PR 描述中列出確切命令與
  所有跳過項。
- 維持嚴格提升：不支援的指令不得靜默變成猜測操作或 `NOP`。
- 行為變更應新增語意覆蓋，而不只是文字 IR 快照。
- 差異中不得包含無關清理、產生的檔案或本機建置產物。
- 檢查[標頭檔與策略歸屬規範](#程式碼風格)，包括裸數字防護、預設值及繫結漂移；
  僅通過格式化檢查還不夠。
- 當行為、支援範圍、選項、命令或測試所有權變更時，更新公開與貢獻者文件。

對不應以公開提取請求開始的安全敏感報告，請遵循
[SECURITY.md](../../SECURITY.md)。
