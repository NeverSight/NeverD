**語言**: [English](testing.md) | [简体中文](testing.zh-CN.md) | [繁體中文](testing.zh-TW.md) | [日本語](testing.ja.md) | [한국어](testing.ko.md) | [Français](testing.fr.md) | [Deutsch](testing.de.md) | [Español](testing.es.md) | [Italiano](testing.it.md) | [Русский](testing.ru.md) | [العربية](testing.ar.md)

[← 文件索引](README.zh-TW.md)

# 測試 NeverD

NeverD 的測試回答三個不同問題：表示形狀是否符合預期、完整 pipeline 路徑能否
處理二進位 fixture，以及產生的程式碼是否維持行為。先選擇能回答本次變更問題的
最小套件；對高風險提取請求，再執行較廣的彙總測試。

## 設定測試建置

除非啟用 `BUILD_TESTING`，否則不會建置測試。完整套件通常使用 Release；Debug
保留斷言與單步能力，但刻意不最佳化，不代表解碼基準效能。

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build-release --parallel 4
```

完整 fixture 集合要求 `clang` 能跨目標編譯，並要求 LLVM linker（`ld.lld`
與 `lld-link`）位於 `PATH`。CMake 無條件建置許多可重定位 fixture，並在存在
對應 linker 時建置已連結 ELF/PE fixture。因主機無法編譯或連結 fixture 而略過
的測試屬於未執行覆蓋，不代表該目標通過。

複製、建置設定與 macOS 預先建置 LLVM 說明見
[CONTRIBUTING.md](i18n/CONTRIBUTING.zh-TW.md)。

## 測試配置

`add_neverd_unittest` 建立一個 GoogleTest 可執行檔，並為每個發現的案例指定
與該可執行目標同名的 CTest 標籤。

| 原始碼區域 | 目標與 CTest 標籤 | 涵蓋內容 |
|------------|-------------------|----------|
| `unittests/TestProcessTests.cpp` | `NeverDTestProcessTests` | 跨平台子行程呼叫、引號、重新導向與結束碼 |
| `unittests/libc` | `NeverDLibCTests` | 已知 libc 名稱與分類 |
| `unittests/lift` | `NeverDLiftTests` | Decoder/lifter LowIR 形狀、IR 階段、loader、重定位、格式 fixture、反編譯與代表性 patch 流程 |
| `unittests/semantic` 中的大多數檔案 | `NeverDSemanticTests` | 指令、ABI、控制流、C 運算式與 lift/recompile 差分語意 |
| `PatchFullSubstRTTests.cpp` | `NeverDPatchFullTests` | 四 ISA×三物件格式的重寫/混淆等價性 |
| `unittests/semantic` 中的聚焦轉換檔案 | `NeverDSwitchXformTests`、`NeverDIndCallXformTests`、`NeverDCFGLoopXformTests`、`NeverDTwoTableXformTests`、`NeverDAvxUpperXformTests` | 從大型語意二進位拆出的快速重新連結探針 |

註冊的事實來源是
[`unittests/CMakeLists.txt`](../unittests/CMakeLists.txt)、
[`unittests/lift/CMakeLists.txt`](../unittests/lift/CMakeLists.txt) 與
[`unittests/semantic/CMakeLists.txt`](../unittests/semantic/CMakeLists.txt)。

## fixture 如何產生

### Lift 與格式 fixture

`unittests/lift/CMakeLists.txt` 在建置期間跨目標編譯 C 與組合語言原始碼。Clang
target triple 產生 x86-64、i386、AArch64、ARM32 ELF 物件、PE/COFF 物件與
已連結映像，以及 PIC/no-PIC Mach-O i386 物件。存在 LLD 時，選定物件還會連結為
patch 測試所需的可執行檔。`NeverDLiftTests` 依賴 `lift-test-objects` 目標，
因此正常建置該測試二進位會更新產生的 fixture。

多數 lift 測試使用 `NeverDLiftFixture.h` 呼叫建置出的 `neverd` CLI，並檢查
LowIR、MedIR、HighIR、LLVM IR、產生的 C 或重寫後的二進位。聚焦手動實驗可用
`NEVERD` 環境變數覆寫 CLI 路徑；一般 CTest 執行使用 CMake 內嵌的可執行檔。

### Unicorn 差分往返

語意 fixture 測試行為而非文字形狀：

1. 撰寫小型 C/組合語言案例，或建構 LLVM IR。
2. 使用 Clang/LLVM 為要求的目標編譯。
3. 在 Unicorn 中執行原始機器碼，並擷取預期回傳值或 fixture 定義的其他狀態。
4. 透過 NeverD 載入並提升，發射 LLVM IR，再把結果編譯回機器碼。
5. 以相同 ABI、輸入、記憶體配置與 CPU 模型執行重新產生的程式碼。
6. 比較可觀察結果。

主要實作位於
[`SemanticRoundTripFixture.h`](../unittests/semantic/SemanticRoundTripFixture.h)。
patch-full fixture 使用 `Codegen::compileForRewrite`（與 patch 操作相同的重寫
backend），接著在完整 4×3 ISA/格式網格中比較基準與轉換後程式碼。

確定性的 NeverD 語意失敗應成為失敗測試。略過只適用於明確的外部能力邊界，並應
閱讀 skip 原因：缺少跨目標 linker 的綠色摘要不能證明該格式路徑實際執行。

## 一次性目標

自訂目標會建置相依，再以主機 CPU 推導的平行度執行 CTest：

| CMake 目標 | 選擇範圍 |
|------------|----------|
| `check-neverd` | 所有已註冊測試 |
| `check-neverd-semantic` | 僅 `NeverDSemanticTests` |
| `check-neverd-patch-full` | 僅 `NeverDPatchFullTests` |
| `check-neverd-switch-xform` | 僅 `NeverDSwitchXformTests` |
| `check-neverd-cfgloop-xform` | 僅 `NeverDCFGLoopXformTests` |
| `check-neverd-twotable-xform` | 僅 `NeverDTwoTableXformTests` |

```bash
cmake --build build-release --target check-neverd
cmake --build build-release --target check-neverd-semantic
```

`NeverDIndCallXformTests` 與 `NeverDAvxUpperXformTests` 目前沒有
`check-neverd-*` 便利目標；請依下文先建置，再以標籤選擇。
`check-neverd-semantic` 也不包含個別的轉換或 patch-full 二進位；完整彙總應使用
`check-neverd`。

## 增量 CTest 工作流程

先建置所屬可執行檔，再選擇其標籤。這可避免重新連結無關的大型語意目標。

```bash
# Lifter, loader, and format tests
cmake --build build-release --target NeverDLiftTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDLiftTests$' --output-on-failure --parallel 4

# Main semantic binary
cmake --build build-release --target NeverDSemanticTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDSemanticTests$' --output-on-failure --parallel 4

# A label-only focused transform binary
cmake --build build-release --target NeverDIndCallXformTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDIndCallXformTests$' --output-on-failure --parallel 4
```

使用 GoogleTest 衍生的 CTest 名稱執行單一迴歸：

```bash
ctest --test-dir build-release --build-config Release -N \
  -L '^NeverDLiftTests$'
ctest --test-dir build-release --build-config Release \
  -R '^COFFARMPipeline\.ARM32ThumbLiftAndDecompile$' \
  --output-on-failure
```

常用選擇器：

| 命令 | 用途 |
|------|------|
| `ctest --test-dir build-release -N` | 列出已發現案例而不執行 |
| `ctest --test-dir build-release -L '<regex>'` | 選擇測試二進位標籤 |
| `ctest --test-dir build-release -R '<regex>'` | 選擇案例名稱 |
| `ctest --test-dir build-release --output-on-failure` | 只為失敗顯示診斷 |
| `ctest --test-dir build-release --stop-on-failure` | 第一個失敗後停止 |
| `ctest --test-dir build-release --parallel 4` | 最多平行執行四個案例 |

GoogleTest 發現使用 `DISCOVERY_MODE PRE_TEST`，因此 CTest 列舉前必須存在對應測試
二進位。每案例 timeout 與獨立發現 timeout 定義在 `cmake/AddNeverD.cmake`，
只有存在量測到的重型案例時才應放寬。

## 哪些測試應隨程式碼變更？

| 變更區域 | 從這裡開始 | 接著考慮 |
|----------|------------|----------|
| 架構 lifter 或 decode | `NeverDLiftTests` 中的命名案例 | 對應 ISA 語意往返 |
| LowIR CFG、函式偵測、跳躍表 | Lift CFG/switch 案例 | `NeverDSwitchXformTests`、`NeverDCFGLoopXformTests` 或 `NeverDTwoTableXformTests` |
| MedIR、ABI、旗標、型別、SSA | MedIR/呼叫慣例 lift 案例 | 跨 ISA 的 `NeverDSemanticTests` 案例 |
| HighIR 或結構化 C | HighIR/decompile 案例 | `NeverDCFGLoopXformTests` 與產生 C 編譯檢查 |
| PE/ELF/Mach-O loader 或輸入重定位 | 對應的 `unittests/lift` 格式 fixture | 該單元格的全階段載入/反編譯測試 |
| 重寫 codegen 或輸出重定位 | `RewriteCodegenRTTests` 案例 | `NeverDPatchFullTests` 與存在時的已連結 patch fixture |
| patch 使用的 LLVM IR 轉換 | 聚焦轉換二進位 | `NeverDPatchFullTests` 組合 pass 網格 |
| C API 或 CLI | 直接 SDK/query 測試與 `unittests/semantic/CLIEndToEndTests.cpp` | 相關 pipeline/格式套件 |
| Libc 辨識 | `NeverDLibCTests` | 行為變更時的語意 call/ABI 案例 |
| 行程執行或 quoting | `NeverDTestProcessTests` | 每個支援主機上的一個受影響 CLI/語意案例 |

測試應在最低穩定邊界表達契約。LowIR 形狀測試適合歸因至 lifter；若兩種看似
合理的 IR 形狀可能行為不同，則必須使用語意往返。若小型 opcode、CFG 或可觀察狀態
斷言已足夠，應避免儲存整個函式的 golden dump。

## 與 CI 的關係

CI 在 Linux、macOS 和 Windows 上以 Release 開啟測試建置，先稽核發現的測試清單，
再套用平台特定標籤排除。設定定義於 `.github/workflows/ci.yml` 與
`scripts/audit_ci_test_inventory.py`。由於沒有單一矩陣 shard 代表所有昂貴套件，當
機器具備全部跨目標工具時，本機 `check-neverd` 仍是最清楚的完整合併前訊號。
