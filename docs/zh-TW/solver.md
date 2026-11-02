**語言**：[English](../solver.md) | [简体中文](../zh-CN/solver.md) | [繁體中文](solver.md) | [日本語](../ja/solver.md) | [한국어](../ko/solver.md) | [Français](../fr/solver.md) | [Deutsch](../de/solver.md) | [Español](../es/solver.md) | [Italiano](../it/solver.md) | [Русский](../ru/solver.md) | [العربية](../ar/solver.md)

[← 文件索引](README.md)

# Bitvector 證明後端

NeverD 預設使用內建 bitvector solver。精確 MBA 推導不依賴一般 solver。表達式合成只會在證明等價後接受候選；反例或未決查詢都會保留原始表達式。

內建乘法電路先移除兩個因子中已知為零的低位前綴，只建立剩餘位元的乘積，再還原完整輸出字。可選 Z3 配接器也依偶數常數係數實際能觀測的位寬編碼乘積。兩者皆保留全位寬等價查詢、獨立輸入模型及既有資源限制。CLI 與 GUI 的原始碼最佳化共用內建電路，無需額外開關。

<!-- i18n-section: builtin-comparisons -->

## 內建比較電路

超過八位元的比較先比較高半部，高半部相等時再使用低半部的結果。小區塊使用減法進位。當部分暫存器更新只改變低位元時，共用編碼器可重用高位元前綴的邏輯閘。有號比較仍將兩個符號位元反相。運算式語意與現有資源限制維持不變；預算耗盡仍回傳 `Unknown`。

`BitBlaster.WidePredicatesAgreeWithTheEvaluator` 在 8 至 256 位元之間選取多種寬度，將有號與無號述詞和運算式求值器比較，涵蓋奇數寬度、邊界相鄰值及第 64 位以上的有效位元，並排除錯誤輸出值。部分計數器測試檢查完整查詢、反例模型及邏輯閘預算耗盡。原生快取比較回歸保留所有暫存器與旗標觀測，證明比較與更新的兩種順序，並拒絕被修改的原始迴圈本體。

<!-- i18n-section: pristine-encoding -->

## 搜尋前的編碼副本

`BitVectorSolver::cloneEncoding()` 複製尚未嘗試 SAT 搜尋的完整編碼；搜尋後或編碼失敗時傳回空指標。副本獨立持有可變子句、根傳播、閘和位元映射，保留變數順序、閘計費和求解器設定。上下文必須比兩個求解器存活更久；來源求解器可獨立修改或銷毀。

SAT 引擎在每個文字的 watch 串列內聯存放四個項目，較長串列動態增長，藉此減少建構、搜尋前複製和銷毀短串列時的獨立配置。傳播順序、子句內容、獨立所有權和全部工作上限維持不變。

<!-- i18n-section: context-finite-proofs -->

## 同一上下文內的完整證明

原生獨立性檢查器將有限域快取綁定至實際只追加節點的符號上下文。緊湊鍵保留精確謂詞、有序投影和值數量上限。僅重用完整數值域或已證明的非唯一性；每次未命中仍須完成列舉及最終排除證明。快取存活期間，上下文及既有節點含義必須保持穩定。預備權杖不能跨快取擁有者，也不能交給同一位址上的替代擁有者重用。儲存仍受原有字數上限約束。其他使用者保留結構鍵和變數重新命名重用。

<!-- i18n-section: z3-build -->

## 選用 Z3 建置

啟用 Z3 後，CMake 透過 `FetchContent` 下載固定的 4.13.3 原始碼版本，並與 NeverD 一起建置靜態程式庫；不需要系統安裝 Z3：

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_Z3=ON -DNEVERD_BUILD_SOLVER_BENCH=ON
python scripts/build.py build-release --parallel 4 --target neverd \
  NeverDSolverTests NeverDSymbolicTests neverd-solver-bench
```

預設 provider 為 `NEVERD_Z3_PROVIDER=FETCH`。第一次 configure 下載原始碼，後續建置會重用 build 目錄的 `_deps`。不會建置 Z3 CLI、測試、範例、文件或語言繫結；Z3 Python 產生器會使用 NeverD 原本需要的 Python interpreter。使用已安裝程式庫時選擇 `-DNEVERD_Z3_PROVIDER=SYSTEM`，也可指定 `-DZ3_ROOT=/path/to/prefix`。缺少開發檔案就會失敗，不會靜默更換 provider。離線建置可用 `-DFETCHCONTENT_SOURCE_DIR_NEVERD_Z3=/path/to/z3` 指定本機原始碼。

預設 `NEVERD_ENABLE_Z3=OFF` 時，NeverD 不會下載、搜尋或連結 Z3。執行期間明確要求不可用後端會失敗，不會回退。

<!-- i18n-section: synthesis -->

## 以證明把關的表達式合成

```sh
build-release/bin/neverd simplify --synthesize --solver=z3 \
  --solver-timeout-ms=1000 --json '(x >> 4) + ((x >> 2) >> 2)'
```

預設為 `--solver=builtin`。選擇 solver 必須使用 `--synthesize`；一般 MBA 簡化器不會呼叫 Z3。Z3 timeout 套用於每次檢查，不包括表達式翻譯。取消是合作式控制，不是硬性牆鐘期限。0 代表採用預設 1000 ms；`--exhaustive` 會移除限制。SAT conflict、propagation 和 watch visit 限制僅適用內建後端，與 Z3 一起使用會遭拒。Z3 檢查會增加 proof-query 計數；內建 SAT 工作計數維持為 0。

C API 在 `neverd_synthesize_options` 追加 `solver_backend` 與 `solver_timeout_ms`。有大小界限的 reader 仍為舊呼叫端保留內建後端。`neverd_solver_backend_available()` 回報建置能力；Python 也可透過 `synthesize_expression(..., solver='z3', solver_timeout_ms=1000)` 選擇。目前此選擇僅涵蓋表達式合成；concolic 執行、安全分析及既有 IR 最佳化預設仍保留原 solver 政策。內部使用者可透過語意簡化器的 proof callback 提供 Z3 verifier。

```python
from neverd_plugin import synthesize_expression

result = synthesize_expression(
    '(x >> 4) + ((x >> 2) >> 2)', solver='z3', solver_timeout_ms=1000
)
```

<!-- i18n-section: checks -->

## 獨立檢查與查詢匯出

啟用 Z3 時，`NeverDSolverTests` 包含獨立表達式評估與跨後端檢查。參考表達式直接建構，避免 NeverD 表達式 builder 的錯誤同時簡化測試雙方。停用 Z3 的建置會明確略過 oracle 案例，但仍測試後端不可用契約。

縮放乘積測試使用獨立長乘法電路與整數運算檢查內建電路，涵蓋至 257 位元。Z3 測試證明它與獨立的全位寬編碼一致，並檢查高輸入位元及刻意改錯的最高輸出位元。

```sh
build-release/bin/NeverDSolverTests --gtest_brief=1
build-release/bin/neverd-solver-bench \
  tools/neverd-bench/solver-corpus.txt --width=32 --repeat=5 \
  --backend=both --timeout-ms=1000 --max-conflicts=10000 \
  --dump-dir=/tmp/neverd-queries > /tmp/neverd-solver-results.json
```

每行非註解資料格式為 `original ; candidate`；沒有分號時，以 MBA 簡化器結果作為候選。工具會回報判定、模型重播與時間；計時包含 session 建立、翻譯、求解，不含解析、MBA 簡化、匯出與結束處理。每次重複都建立新 solver。SAT model 必須能在表達式 evaluator 重現差異。互相矛盾的確定判定、無效查詢或模型會使執行失敗；`unknown` 會記錄，但不是等價證明。

匯出的 SMT-LIB 包含等價編碼的 DAG、永久 assertions 和最後一次查詢的 assumptions，可用 `z3 query-N.smt2` 重播。請記錄資源限制與 solver 版本；兩個後端的預算單位不同，故比較的是有限 workload，而非相同工作量。Bitvector 證明採用表達式語言的 total fixed-width 語意。機器例外、記憶體效果和 LLVM poison 仍屬於提升與翻譯邊界的責任，表達式證明不會替它們背書。

尚未搜尋的編碼副本從自身決策佇列移除根層已賦值變數，並重建嚴格的活躍度／編號堆積順序。賦值與子句保持完整，根層事實在所有回溯中保留。來源物件所有權、實際決策、完整模型和全部搜尋預算均不變。

SMT-LIB 匯出的 DAG 是語意等價的編碼，可能包含上述精確縮放商，不保證與原始 DAG 結構相同。
