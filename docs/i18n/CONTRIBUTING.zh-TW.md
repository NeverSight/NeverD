**語言**: [English](../../CONTRIBUTING.md) | [简体中文](CONTRIBUTING.zh-CN.md) | [繁體中文](CONTRIBUTING.zh-TW.md) | [日本語](CONTRIBUTING.ja.md) | [한국어](CONTRIBUTING.ko.md) | [Français](CONTRIBUTING.fr.md) | [Deutsch](CONTRIBUTING.de.md) | [Español](CONTRIBUTING.es.md) | [Italiano](CONTRIBUTING.it.md) | [Русский](CONTRIBUTING.ru.md) | [العربية](CONTRIBUTING.ar.md)

# 為 NeverD 做出貢獻

NeverD 是一個語意優先的二進位分析專案。有價值的貢獻應聚焦單一目標、
讓不支援的行為明確失敗，並包含足以證明變更契約的最小測試。

開始修改前，請閱讀[架構指南](../architecture.zh-TW.md)。測試套件的選擇請參考
[測試指南](../testing.zh-TW.md)，產品規劃請參考
[路線圖](../roadmap/README.zh-TW.md)。

## 必要條件

- 支援遞迴子模組的 Git
- CMake 3.20 或更新版本
- Ninja
- 支援 C++20 的編譯器
- 用於完整跨目標 fixture 集合的 Clang 與 LLD（`ld.lld` 和 `lld-link`）

遞迴子模組提供 NeverD 的 LLVM fork、Capstone fork、Unicorn 與簽章資料。
驗證變更時，請勿將它們替換成任意的系統版本。

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
cmake --build build-release --parallel
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
cmake --build build-debug --parallel
```

絕不要用 Debug 建置報告解碼或提升吞吐量。基準測試使用 Release；效能分析需要
符號時使用 RelWithDebInfo。

### macOS 上的預先建置 LLVM

Apple Silicon 貢獻者可避免在本機編譯 LLVM fork：

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_LLVM_PREBUILT=ON
cmake --build build-release --parallel
```

CMake 會下載儲存庫設定的發行套件、驗證 SHA-256，並在後續建置中重用已解壓的
使用者快取。預先建置通道僅支援 macOS arm64；Intel Mac 與通用二進位建置必須
使用預設的本機 LLVM 建置。`NEVERD_LLVM_PREBUILT_TAG`、鏡像 URL、快取目錄和
明確校驗和等進階覆寫項記錄於 `cmake/NeverDLLVMPrebuilt.cmake`。

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

## 程式碼風格

C 與 C++ 遵循 LLVM 編碼慣例，以 `.clang-format` 作為儲存庫的格式權威。
只格式化你修改的檔案：

```bash
clang-format -i path/to/changed.cpp path/to/changed.h
git diff --check
```

不要為局部修正執行全儲存庫格式化。遵循周邊檔案的命名與拆分方式，將平台特定
行為保留在相應的 loader、lifter 或 backend 邊界內，並避免透過純 C SDK 暴露
內部 C++ 型別。

Markdown 應簡潔且可由原始碼驗證。儲存庫內部檔案使用相對連結；當 CLI 行為、
公開 API、支援聲明、建置選項或測試命令變更時，在同一個提取請求中更新文件。

## 執行測試

透過彙總目標執行所有已註冊測試：

```bash
cmake --build build-release --target check-neverd
```

開發期間使用最小的相關目標或 CTest 標籤：

```bash
# Main Unicorn differential suite
cmake --build build-release --target check-neverd-semantic

# Lifter/loader/format binary only
cmake --build build-release --target NeverDLiftTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDLiftTests$' --output-on-failure --parallel 4
```

[測試指南](../testing.zh-TW.md)記錄所有便利目標、僅標籤的轉換套件、單一測試
正規表示式、fixture 編譯與 Unicorn 往返。如果某個目標因缺少交叉編譯器或
連結器而跳過，請回報此限制；不要把未執行的路徑描述成通過。

## 提取請求檢查清單

請求審查前：

- 依維護者偏好的流程 rebase 或 merge 最新 `dev`，並有意處理子模組變更。
- 以 Release 建置受影響的目標；若需要其他設定，請說明原因。
- 執行精準的回歸測試與實際可行的最廣相關套件；在 PR 描述中列出確切命令與
  所有跳過項。
- 維持嚴格提升：不支援的指令不得靜默變成猜測操作或 `NOP`。
- 行為變更應新增語意覆蓋，而不只是文字 IR 快照。
- 差異中不得包含無關清理、產生的檔案或本機建置產物。
- 當行為、支援範圍、選項、命令或測試所有權變更時，更新公開與貢獻者文件。

對不應以公開提取請求開始的安全敏感報告，請遵循
[SECURITY.md](../../SECURITY.md)。
