**語言**: [English](python-plugins.md) | [简体中文](python-plugins.zh-CN.md) | [繁體中文](python-plugins.zh-TW.md) | [日本語](python-plugins.ja.md) | [한국어](python-plugins.ko.md) | [Français](python-plugins.fr.md) | [Deutsch](python-plugins.de.md) | [Español](python-plugins.es.md) | [Italiano](python-plugins.it.md) | [Русский](python-plugins.ru.md) | [العربية](python-plugins.ar.md)

[← 文件索引](README.zh-TW.md)

# Python 外掛

NeverD 可以將 Python 檔案作為第一級外掛載入。Python 外掛與原生外掛共用相同的中繼資料、生命週期、排序、重名規則、事件串流和工作階段 C ABI。受支援的開發套件是 `neverd-plugin`；請勿直接匯入私有的 `_neverd_plugin` 橋接模組。

## 建置與執行需求

`NEVERD_ENABLE_PYTHON_PLUGINS` 預設為 `ON`。啟用後，CMake 必須能找到 CPython 3.10 或更新版本的直譯器及嵌入開發程式庫：

```bash
cmake -S . -B build -G Ninja \
  -DNEVERD_ENABLE_PYTHON_PLUGINS=ON \
  -DPython3_EXECUTABLE="$(python3 -c 'import sys; print(sys.executable)')"
cmake --build build
```

若要建立不連結 CPython 的純原生 `libneverd`，請設定 `-DNEVERD_ENABLE_PYTHON_PLUGINS=OFF`。啟用 Python 的建置會將對應套件和範例放到 `build/bin/sdk/python/`；也可直接執行 `python3 -m pip install build/bin/sdk/python` 安裝該目錄。

## 撰寫外掛

一個模組必須只宣告一個帶裝飾器的類別：

```python
from neverd_plugin import Event, Plugin, PluginType, Session


@Plugin(
    name="Analysis Report",
    version="1.0.0",
    author="Your team",
    description="Reports basic information about the loaded binary",
    type=PluginType.PROCESSOR,
)
class AnalysisReport:
    def on_init(self, session: Session) -> int | None:
        print(session.architecture)
        return None

    def on_run(self, session: Session, arg: int) -> int | None:
        print(session.file_path, session.function_count)
        return 0

    def on_event(self, event: Event) -> int | None:
        print(event.type.name)
        return None

    def on_term(self) -> None:
        pass
```

所有 hook 都是選用的。`None` 表示成功；整數回傳值必須能容納於 C `int`。中繼資料版本必須採用嚴格 SemVer。名稱必須是非空 UTF-8 字串，任何含有內嵌 NUL 的中繼資料都會被拒絕。

儲存庫提供 [`minimal.py`](../pluginsdk/python/examples/minimal.py)、[`analysis_report.py`](../pluginsdk/python/examples/analysis_report.py)，以及示範證明閘控最佳化 API 的 [`semantic_optimizer.py`](../pluginsdk/python/examples/semantic_optimizer.py) 三個範例。

## 載入與檢查外掛

C API 可以確定性地載入指定 `.py` 檔案，也可以掃描目錄：

```c
if (!neverd_plugins_load_file(session, "plugins/report.py")) {
  const char *message = neverd_last_error(session);
  /* log message */
  neverd_free_string(message);
}

neverd_plugins_init(session);
int result = neverd_plugins_run(session, "Analysis Report", 0);
neverd_plugins_term(session);
```

`neverd_plugins_list_json` 以 `"kind":"python"` 或 `"kind":"native"` 標示每一項。目錄探索依標準路徑排序，並可在同一目錄中同時接受原生程式庫與 Python 檔案。重複的標準路徑與重複的外掛名稱都會產生錯誤。

## 工作階段與事件 API

`Session` 在每次呼叫 C API 前都會重新驗證主機能力。其型別化介面包含檔案、架構與格式中繼資料，位元數與資料表計數，函式檢視，載入與分析，位元組讀取，反組譯、反編譯及常用查詢。進階操作可透過 `session.raw` 存取 `neverd_plugin.abi` 中的每一項宣告：

```python
count = session.raw.session_call("neverd_plugins_count")
version = session.raw.owned_string("neverd_version")
object_bytes = session.raw.session_borrowed_bytes("neverd_roundtrip_obj")
```

### 有界符號路徑探索

對於原生 LowIR 函式，`session.symbolic_explore` 會傳回具型別的路徑結果、基本區塊軌跡、資源使用量及可選的路徑述詞：

```python
result = session.symbolic_explore(
    0x401000,
    max_paths=64,
    max_steps=1 << 16,
    max_block_visits=3,
    include_expressions=True,
)
if not result.exact:
    print(result.unmodelled_ops)
for path in result.paths:
    print(path.outcome, path.blocks, path.predicate)
```

若路徑數、步數、迴圈造訪次數或未解析分支達到上限並終止遍歷，`complete` 為 false。`exact` 還要求沒有任何操作被保守地替換為未知狀態；不受支援的 LowIR 操作、沒有摘要的呼叫，以及透過未解析位址進行的儲存都會計入 `unmodelled_ops`。EVM 與 SBF 工作階段不提供原生 LowIR 探索。

### 記憶體安全稽核與獵取

`session.audit()` 與 `session.hunt()` 會傳回解析後的 JSON 報告（與 CLI 同一模式）。它們需要已提升的原生工作階段：

```python
audit = session.audit()
hunt = session.hunt(max_paths=64, max_steps=1 << 16)
print(audit.get("ok"), hunt.get("findings"))
```

EVM 與 SBF 工作階段會拒絕這些呼叫。

六種不可變事件分別是 `BINARY_LOADED`、`BINARY_CLOSING`、`FUNCTION_SELECTED`、`ADDRESS_CHANGED`、`ANALYSIS_DONE` 與 `PATCH_APPLIED`。回呼期間會複製承載字串；與目前事件類型無關的欄位為 `None`。

切勿儲存 `Session` 並在終止後繼續使用。原生 capsule 會在 `on_term` 開始前及原生工作階段釋放前失效。後續呼叫會擲出 `RuntimeError`，而不會取消參照過期記憶體。

### 證明閘控的合成與 LLVM 最佳化

`synthesize_expression` 與為 ABI 相容而保留、僅處理 MBA 的
`simplify_expression` 分開。只有求解器回報
`ProofStatus.EQUIVALENT` 時才會提交改寫；反例、未完成證明與搜尋預算
耗盡都會保留原運算式，並分別回報結果原因及搜尋/證明工作量。
`ProofStatus.INVALID` 表示證明問題本身無效，與預算造成的
`ProofStatus.UNKNOWN` 嚴格區分；兩者都會以拒絕改寫的方式關閉。

`optimize_llvm_ir` 在交易副本上結合 NeverD 的語意不動點與標準 LLVM
最佳化管線，通過驗證後才傳回已提交的模組：

```python
from neverd_plugin import (
    LLVMOptimizationLevel,
    OptimizationMode,
    ProofStatus,
    optimize_llvm_ir,
    synthesize_expression,
)

rewrite = synthesize_expression(
    "(x >> 4) + ((x >> 2) >> 2)", exhaustive=True
)
if rewrite.changed:
    assert rewrite.proof_status is ProofStatus.EQUIVALENT

module = optimize_llvm_ir(
    llvm_ir,
    mode=OptimizationMode.DEEP,
    llvm_level=LLVMOptimizationLevel.O2,
    enable_synthesis=True,
    exhaustive=True,
)
print(module.output_ir, module.semantic_rewrites, module.proof_queries)
```

正式環境可分別限制 MBA 的工作量與元數、合成搜尋與 SAT 工作量，以及 LLVM
收斂。對 `simplify_expression` 明確使用 `exhaustive=True` 會選用無上限的 MBA
元數/工作策略，並移除原生解析器的巢狀與位元寬度策略上限；對
`synthesize_expression`，它會移除解析、搜尋工作量與 SAT 上限，但保留呼叫端
指定的文法範圍；對 `optimize_llvm_ir`，它會移除收斂、搜尋工作量與 SAT 上限。
Python 層不會另加運算式限制，記憶體安全與 IR 表示邊界仍然適用。對應的 C
入口為 `neverd_simplify_expr`、`neverd_synthesize_expr` 與
`neverd_optimize_llvm_ir`，並提供型別化釋放函式及版本化 JSON 介面。

## 錯誤、隔離與信任

Python 例外絕不會穿越 C++ 堆疊展開。NeverD 會擷取完整格式化 traceback，並透過 `neverd_last_error` 公開。每個標準外掛路徑都以唯一模組名稱載入；終止時會移除模組，之後重新載入便取得全新的模組與類別狀態。CPython 僅初始化一次，啟動階段的 GIL 會被釋放，任何主機執行緒執行回呼時都會取得 GIL；NeverD 不會終止可能與其他元件共用的直譯器。

外掛在 NeverD 行程內執行任意 Python，並可呼叫完整 C API。只載入受信任的檔案。這是擴充邊界，不是沙箱。

## 開發、測試與封裝

若需要編輯器和型別檢查支援，請安裝純 Python 套件，或將原始碼樹加入 `PYTHONPATH`：

```bash
python3 -m pip install -e pluginsdk/python

PYTHONPATH=pluginsdk/python python3 -m unittest discover \
  -s pluginsdk/python/tests -v
python3 -m mypy --config-file pluginsdk/python/pyproject.toml \
  pluginsdk/python/neverd_plugin
PYTHONPATH=pluginsdk/python python3 scripts/check_python_plugin_sdk.py
```

稽核要求每個匯出的 C 宣告與其 `ctypes` 簽章和擁有權規則完全一致；同時檢查輸出語言值、CMake 與套件版本、CI 功能開關、Action 固定版本、成品流轉及 PyPI OIDC 原則。原生轉接器測試是 `NeverDPluginRuntimeTests`；嵌入式 Python 測試是 `NeverDPythonRuntimeTests` 與 `NeverDPythonPluginTests`。

`Python Plugin SDK` 工作流程會建置一個 wheel 與一份原始碼發行套件，將兩者安裝至乾淨環境，並上傳通過驗證的成品。只有已發佈的 GitHub Release 才會透過受核准保護的 `pypi` environment 和 Trusted Publishing 發佈；不使用長期 PyPI token。
