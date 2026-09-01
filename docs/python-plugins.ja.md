**言語**: [English](python-plugins.md) | [简体中文](python-plugins.zh-CN.md) | [繁體中文](python-plugins.zh-TW.md) | [日本語](python-plugins.ja.md) | [한국어](python-plugins.ko.md) | [Français](python-plugins.fr.md) | [Deutsch](python-plugins.de.md) | [Español](python-plugins.es.md) | [Italiano](python-plugins.it.md) | [Русский](python-plugins.ru.md) | [العربية](python-plugins.ar.md)

[← ドキュメント索引](README.ja.md)

# Python プラグイン

NeverD は Python ファイルを第一級のプラグインとして読み込めます。Python プラグインは、ネイティブプラグインと同じメタデータ、ライフサイクル、順序、名前重複規則、イベントストリーム、セッション C ABI を共有します。サポート対象の開発パッケージは `neverd-plugin` です。非公開の `_neverd_plugin` ブリッジを直接 import しないでください。

## ビルドと実行環境の要件

`NEVERD_ENABLE_PYTHON_PLUGINS` の既定値は `ON` です。有効なビルドでは、CMake から検出可能な CPython 3.10 以降のインタープリターと埋め込み用開発ライブラリが必要です。

```bash
cmake -S . -B build -G Ninja \
  -DNEVERD_ENABLE_PYTHON_PLUGINS=ON \
  -DPython3_EXECUTABLE="$(python3 -c 'import sys; print(sys.executable)')"
cmake --build build
```

CPython へのリンク依存がないネイティブ専用 `libneverd` を作るには、`-DNEVERD_ENABLE_PYTHON_PLUGINS=OFF` を指定します。Python を有効にしたビルドでは、対応するパッケージとサンプルが `build/bin/sdk/python/` に配置されます。このディレクトリは `python3 -m pip install build/bin/sdk/python` でも直接インストールできます。

## プラグインを書く

1 つのモジュールでは、デコレーター付きクラスをちょうど 1 つ宣言します。

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

すべての hook は省略可能です。`None` は成功を意味し、整数の戻り値は C の `int` に収まる必要があります。メタデータのバージョンには厳密な SemVer を使用します。名前は空でない UTF-8 でなければならず、埋め込み NUL を含むメタデータはすべて拒否されます。

リポジトリには [`minimal.py`](../pluginsdk/python/examples/minimal.py)、[`analysis_report.py`](../pluginsdk/python/examples/analysis_report.py)、証明ゲート付き最適化 API を示す [`semantic_optimizer.py`](../pluginsdk/python/examples/semantic_optimizer.py) のサンプルがあります。

## プラグインの読み込みと確認

C API では、特定の `.py` ファイルを決定的に読み込むか、ディレクトリを走査できます。

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

`neverd_plugins_list_json` は各項目を `"kind":"python"` または `"kind":"native"` で識別します。ディレクトリ探索は正規パス順に並び、同じディレクトリ内のネイティブライブラリと Python ファイルを扱えます。正規パスの重複とプラグイン名の重複はエラーです。

## セッションとイベント API

`Session` は C 呼び出しのたびにホスト機能を再検証します。型付き API には、ファイル／アーキテクチャ／形式のメタデータ、ビット数とテーブル件数、関数ビュー、読み込みと解析、バイト読み取り、逆アセンブル、逆コンパイル、一般的な問い合わせが含まれます。高度な操作では `session.raw` から `neverd_plugin.abi` の全宣言にアクセスできます。

```python
count = session.raw.session_call("neverd_plugins_count")
version = session.raw.owned_string("neverd_version")
object_bytes = session.raw.session_borrowed_bytes("neverd_roundtrip_obj")
```

### 境界付きシンボリックパス探索

ネイティブ LowIR 関数に対して、`session.symbolic_explore` は型付きのパス結果、基本ブロックのトレース、リソース使用量、および任意のパス述語を返します。

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

パス数、ステップ数、ループ訪問回数、または未解決分岐の上限によって探索が停止した場合、`complete` は false です。さらに `exact` が true になるには、未知状態による保守的な置換が一度も行われていない必要があります。未対応の LowIR 操作、要約のない呼び出し、未解決アドレスへのストアは `unmodelled_ops` に数えられます。EVM および SBF セッションではネイティブ LowIR 探索を利用できません。

### 検証済み LowIR コンコリック分岐反転

`session.lowir_concolic` は明示した入力レジスタのバイト範囲から 1 本のネイティブ LowIR 経路を追跡し、新しい再実行が同じ制御判断の出現位置で検証したソルバー生成候補だけを返します。

```python
from neverd_plugin import ConcolicRegisterSeed

report = session.lowir_concolic(
    0x401000,
    [ConcolicRegisterSeed(offset=56, bytes=4, value=0)],
)
for flip in report.flips:
    if flip.candidate_id is not None:
        print(report.candidates[flip.candidate_id].seed)
```

レジスタオフセットは NeverD のレジスタファイル内のバイトオフセットであり、ネイティブポインタやレジスタ番号ではありません。レポートは常に非網羅的で、UNSAT、ソルバー上限、射影拒否、再実行拒否は例外ではなく型付き反転結果として残ります。

### メモリ安全性の監査とハント

`session.audit()` と `session.hunt()` は解析済み JSON レポートを返します（CLI と同じスキーマ）。リフト済みのネイティブセッションが必要です。

```python
audit = session.audit()
hunt = session.hunt(max_paths=64, max_steps=1 << 16)
print(audit.get("ok"), hunt.get("findings"))
```

EVM および SBF セッションではこれらの呼び出しは拒否されます。

6 種類の不変イベントは `BINARY_LOADED`、`BINARY_CLOSING`、`FUNCTION_SELECTED`、`ADDRESS_CHANGED`、`ANALYSIS_DONE`、`PATCH_APPLIED` です。コールバック中に payload 文字列がコピーされ、イベント種別に無関係なフィールドは `None` になります。

終了後に使う目的で `Session` を保存しないでください。ネイティブ capsule は `on_term` の開始前、かつネイティブセッションを解放できるようになる前に無効化されます。それ以降の呼び出しは古いメモリを参照せず、`RuntimeError` で失敗します。

### 証明ゲート付き合成と LLVM 最適化

`synthesize_expression` は、ABI 互換性のために残された MBA 専用の
`simplify_expression` とは独立しています。ソルバーが
`ProofStatus.EQUIVALENT` を返した場合だけ書き換えを確定します。反例、
未完了の証明、検索予算切れでは元の式を保持し、それぞれの結果理由と
検索・証明作業量を返します。
`ProofStatus.INVALID` は証明問題そのものが不正であることを示し、予算による
`ProofStatus.UNKNOWN` と区別されます。どちらも書き換えを安全側で拒否します。

`optimize_llvm_ir` はトランザクション複製上で NeverD の意味論的固定点と
標準 LLVM パイプラインを組み合わせ、検証済みの確定モジュールだけを返します：

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

本番環境では MBA の作業量と項数、合成の探索と SAT 作業量、LLVM の収束を
個別に制限できます。`simplify_expression` で明示的に `exhaustive=True` を
指定すると、項数と作業量が無制限の MBA ポリシーが選ばれ、ネイティブ
パーサーの入れ子とビット幅に関するポリシー上限も外れます。
`synthesize_expression` では呼び出し側が指定した文法を維持したまま、
パーサー、探索作業量、SAT の上限を外します。`optimize_llvm_ir` では収束、
探索作業量、SAT の上限を外します。Python 層は式に追加制限を設けませんが、
メモリ安全性と IR 表現の境界は引き続き適用されます。対応する C API は
`neverd_simplify_expr`、`neverd_synthesize_expr`、`neverd_optimize_llvm_ir` で、
型付き解放関数と版付き JSON API もあります。

## エラー、分離、信頼

Python 例外が C++ を越えて unwind することはありません。NeverD は完全に整形された traceback を捕捉し、`neverd_last_error` で公開します。各プラグインの正規パスは固有のモジュール名で読み込まれます。終了時にモジュールを削除するため、再読み込みでは新しいモジュール／クラス状態になります。CPython は一度だけ初期化され、bootstrap GIL は解放されます。任意のホストスレッド上のコールバックは GIL を取得し、NeverD は他のコンポーネントと共有され得るインタープリターを終了しません。

プラグインは NeverD プロセス内で任意の Python を実行し、完全な C API を呼び出せます。信頼できるファイルだけを読み込んでください。これは拡張境界であり、sandbox ではありません。

## 開発、テスト、パッケージ

エディターと型チェッカーの支援を得るには、純 Python パッケージをインストールするか、ソースツリーを `PYTHONPATH` に指定します。

```bash
python3 -m pip install -e pluginsdk/python

PYTHONPATH=pluginsdk/python python3 -m unittest discover \
  -s pluginsdk/python/tests -v
python3 -m mypy --config-file pluginsdk/python/pyproject.toml \
  pluginsdk/python/neverd_plugin
PYTHONPATH=pluginsdk/python python3 scripts/check_python_plugin_sdk.py
```

監査では、export されるすべての C 宣言と `ctypes` のシグネチャ／所有権規則が正確に一致することを求めます。さらに出力言語の値、CMake とパッケージのバージョン、CI の機能フラグ、Action の固定バージョン、artifact の流れ、PyPI OIDC ポリシーも確認します。ネイティブアダプターのテストは `NeverDPluginRuntimeTests`、埋め込み Python のテストは `NeverDPythonRuntimeTests` と `NeverDPythonPluginTests` です。

`Python Plugin SDK` workflow は wheel とソース配布物を 1 つずつ作成し、両方をクリーンな環境へインストールして、検証済み artifact をアップロードします。公開済み GitHub Release の場合だけ、承認で保護された `pypi` environment と Trusted Publishing を通して公開します。長期 PyPI token は使用しません。
