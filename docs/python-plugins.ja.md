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

リポジトリには [`minimal.py`](../pluginsdk/python/examples/minimal.py) と [`analysis_report.py`](../pluginsdk/python/examples/analysis_report.py) のサンプルがあります。

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

6 種類の不変イベントは `BINARY_LOADED`、`BINARY_CLOSING`、`FUNCTION_SELECTED`、`ADDRESS_CHANGED`、`ANALYSIS_DONE`、`PATCH_APPLIED` です。コールバック中に payload 文字列がコピーされ、イベント種別に無関係なフィールドは `None` になります。

終了後に使う目的で `Session` を保存しないでください。ネイティブ capsule は `on_term` の開始前、かつネイティブセッションを解放できるようになる前に無効化されます。それ以降の呼び出しは古いメモリを参照せず、`RuntimeError` で失敗します。

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
