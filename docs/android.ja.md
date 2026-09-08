**言語**: [English](android.md) | [简体中文](android.zh-CN.md) | [繁體中文](android.zh-TW.md) | [日本語](android.ja.md) | [한국어](android.ko.md) | [Français](android.fr.md) | [Deutsch](android.de.md) | [Español](android.es.md) | [Italiano](android.it.md) | [Русский](android.ru.md) | [العربية](android.ar.md)

# Android の Java 復元

[← ドキュメント一覧](README.ja.md)

`neverd mobile` は、別途インストールした JADX バックエンドを使い、APK、DEX、smali の入力から読みやすい Java を復元します。バイトコードを検証して作業領域に配置し、関連するクラスをまとめて解析したうえで、生成結果を検査し、ソースディレクトリと機械可読のレポートを出力します。これは実験的な CLI 機能です。APK コンテナと Java 出力は、ネイティブ C SDK、Python プラグイン SDK、GUI ローダー、`neverd decompile --language` からは利用できません。

復元された Java は、バイトコードを再構成したものです。元のコメント、書式、記述に使われたソース言語、削除された識別子は取り戻せません。Kotlin のバイトコードからも Java が生成されます。処理の成功は意味的等価性の証明ではなく、すべてのメソッドが再コンパイルできる保証でもありません。この処理で解析対象のアプリケーションが起動されることはありません。

## クイックスタート

以下の手順で実行環境を準備し、新しい出力ディレクトリを指定します。

```sh
neverd mobile app.apk -o recovered-app
neverd mobile classes.dex -o recovered-dex
neverd mobile MainActivity.smali -o recovered-class
neverd mobile decoded/smali -o recovered-java
```

Java ファイルは `recovered-app/sources/`、入力一覧と制限事項は `recovered-app/report.json` で確認できます。smali のクラスが相互に参照する場合は、ディレクトリを入力にすることをお勧めします。

## 実行環境の準備

| コンポーネント | 要件 | 選択方法 |
|----------------|------|----------|
| NeverD | `neverd` ターゲットをビルドし、実行ファイルと同じ階層の `mobile/` ディレクトリも配布する | `build/bin/neverd` または PATH 上の実行ファイル |
| Python | Python 3.10 以降。組み込みプラグインホストの Python 環境とは独立 | `--python`、`NEVERD_PYTHON`、PATH 上の `python3`/`python` の順に選択 |
| Java バックエンド | 標準の DEX および smali 入力プラグインを含む JADX 1.5.6 以降 | `--jadx`、`NEVERD_JADX`、PATH 上の `jadx` の順に選択 |
| Java 実行環境 | Java 11 以降。コンパイルと実行による検証には JDK が必要 | `JAVA_HOME` または PATH 上の Java |

NeverD は依存ソフトウェアを自動ダウンロードしません。完全な [JADX 配布パッケージ](https://github.com/skylot/jadx/releases/tag/v1.5.6)を入手し、`bin/` と `lib/` の構成を維持してください。再配布時は同梱のライセンス文書も保持してください。検証済みのバックエンドは 1.5.6 です。それ以降のバージョンにも同じ CLI 契約への適合が必要です。これらの依存ソフトウェアの準備は、NeverD の LLVM パイプラインのビルドとは別に行います。

### Linux と macOS

```sh
cmake --build build --target neverd
python3 --version
java -version
/opt/jadx/bin/jadx --version

./build/bin/neverd mobile app.apk -o recovered-app \
  --python python3 --jadx /opt/jadx/bin/jadx
```

継続して使用する場合は `NEVERD_JADX=/opt/jadx/bin/jadx` を設定し、必要に応じて `NEVERD_PYTHON` にインタープリターのパスを設定します。Java がまだ利用できない場合は、`JAVA_HOME` を JDK のインストールディレクトリに向けてください。空白を含むパスは引用符で囲む必要があります。

### Windows PowerShell

```powershell
$env:JAVA_HOME = 'C:\Tools\jdk'
& .\build\bin\neverd.exe mobile .\app.apk -o .\recovered-app `
  --python 'C:\Tools\Python\python.exe' `
  --jadx 'C:\Tools\jadx\bin\jadx.bat'
```

バックエンドの `.bat`/`.cmd` パスから、配布パッケージにある唯一の `lib/jadx-*-all.jar` を特定し、NeverD が Java を直接起動します。この JAR 自体を `--jadx` に渡すこともできます。アプリケーションのパスがコマンドシェルに埋め込まれることはありません。マルチ構成ビルドでは、実行ファイルが `build/bin/Release/` に配置される場合があります。実行ファイルだけを移動し、同じ階層の `mobile/` ディレクトリを伴わない場合は、ヘルパーが見つからないというエラーになります。

## 対応する入力と範囲

| 入力 | 動作 | 重要な制限 |
|------|------|------------|
| `.apk` | ZIP 全体を検証し、ルートの `classes.dex`、`classes2.dex` および後続番号の DEX ファイルをまとめて解析 | コードのみ。リソースやマニフェストはデコードしない |
| `.dex` | DEX のマジック値を検証した後、バックエンドが内容をデコード | 拡張子を変えたファイルや途中で切れたファイルは、有効なバイトコードにはならない |
| `.smali` | 指定されたクラスを解析 | 参照先のほかのクラスは暗黙には読み込まない |
| smali ディレクトリ | `.smali` ファイルを再帰的に収集し、まとめて解析 | 入力ディレクトリにネストしたクラスと依存先の smali ルートを含める |

`smali/` と `smali_classes2/` を含む APK のデコード済みツリーを解析するには、両者の共通ディレクトリを指定します。バックエンドに渡されるのは `.smali` ファイルだけですが、最初に指定したツリー全体を検証してコピーするため、無関係な大きなアセットも入力制限に計上されます。関連する smali ルートだけを含むコンパクトなディレクトリにすると、処理量を減らせます。

分割 APK はそれぞれ独立した入力です。DEX を含む各 APK は個別に処理できますが、このコマンドは APK セットを統合しません。リソースのみの分割 APK は、ルートに DEX がないため失敗します。`.aab`、`.apks`、`.xapk`、`.odex`、`.oat`、`.vdex` は、この mobile コマンドの入力として受け付けません。バックエンド自体が一部の形式に対応していても、この NeverD コマンドが対応しているとは限りません。

APK のリソース、`AndroidManifest.xml`、アセット、JNI／ネイティブライブラリ、実行時にダウンロードされるコードは Java として復元されません。ネイティブの `.so` は別途取り出し、`neverd decompile library.so -o library.c` を使用してください。この静的な処理では、暗号化またはパックされたペイロードが、あらかじめ通常の DEX/smali として用意されている必要があります。アンパック、デバイスへのアタッチ、保護の回避は行いません。

## オプションと優先順位

```sh
neverd mobile app.apk -o recovered-app --platform=android \
  --jadx /opt/jadx/bin/jadx --timeout=600 \
  --max-files=30000 --max-bytes=4294967296 --json
```

| オプション | 既定値 | 意味 |
|------------|--------|------|
| `-o DIRECTORY` | 必須 | 入力がディレクトリの場合はその外部に置く、新しい出力ディレクトリ。既存の出力は上書きしない |
| `--platform=auto\|android` | `auto` | Android を明示的に選択するか、入力からプラットフォームを推定 |
| `--jadx PATH` | 環境変数／PATH | バックエンドのランチャーまたは配布 JAR。明示したオプションを優先 |
| `--python PATH` | 環境変数／PATH | 同梱ヘルパー用のインタープリター。明示したオプションを優先 |
| `--timeout N` | `300` | バックエンドの各プロセスに対する正の秒数による制限。バージョン確認も対象 |
| `--max-files N` | `20000` | 実際に作成されるディレクトリを含む、正のエントリー数上限 |
| `--max-bytes N` | `2147483648` | 入力、展開データ、最終出力のバイト数上限。正の値を指定 |
| `--json` | 無効 | 人向けの要約ではなく、JSON でレポートを出力 |

`--arch` の既定値以外、`--artifact`、`--metadata-only`、ゼロ以外の `--max-func` は iOS 用であり、Android では拒否されます。`--arch=auto` の明示指定は可能です。任意のバックエンドオプションをそのまま渡す機能はありません。バックエンドの設定、キャッシュ、一時ディレクトリは実行ごとに分離され、既存のバックエンド設定やプラグイン設定は取り込まれません。

これらの上限はリソース制御であり、バックエンドプロセスのサンドボックスではありません。作業領域も監視され、入力、展開データ、出力のために、設定したエントリー数／バイト数予算の最大 3 倍まで使用できます。ログはプロセスごとに 16 MiB までです。大きな入力では、Java のヒープやタイムアウトの追加が必要な場合があります。ある上限を引き上げても、ほかの上限は無効になりません。

## 出力構成と JSON レポート

```text
recovered-app/
  sources/                 復元された Java のパッケージとクラス
  logs/jadx-version.log    バックエンドのバージョン確認
  logs/jadx.log            バックエンドの診断情報
  report.json              バージョン付きの一覧と復元の制限
```

一時コピーとバックエンドのキャッシュは削除されます。Java ファイルの具体的な名前や数はバックエンドの再構成によって変わり、ネストしたクラスが外側のクラスとソースファイルを共有することもあります。そのため、Java ソースファイル数は DEX のクラス数とは異なります。

以下は一部を省略したレポート例です。

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

`input_kind` は `apk`、`dex`、`smali`、`smali-directory` のいずれかです。`input_code_files` は入力バイトコードの名前または smali のパスを列挙し、`java_sources` と `logs` は出力ルートからの相対パスです。`source` は入力のベース名です。実際のレポートには、ほかの復元上の制限事項も含まれます。結果を別のツールに渡す際にも、この情報を保持してください。

自動化では、`status` を利用する前にプロセスの終了コードを確認します。標準出力をリダイレクトする場合は、レポートを新しい出力ディレクトリの外に保存してください。

```sh
neverd mobile app.apk -o recovered-app --json > recovery-result.json
```

ヘルパーの実行が成功するとゼロ、復元に失敗するとゼロ以外を返します。ヘルパーの起動後は、`--json` により `schema_version`、`status: "error"`、`error` を含むエラーオブジェクトが生成されます。ネイティブ側の引数解析、Python の不足や 3.10 未満のバージョン、ヘルパーの欠落は、それ以前の段階で失敗し、JSON ではなく stderr に出力される場合があります。中断も stderr に報告されることがあります。利用側はこれらにも対応する必要があります。

## 失敗時の処理とトラブルシューティング

結果はトランザクションとして公開されます。既存の出力は保持され、失敗した作業領域の出力は削除されます。バックエンドのゼロ以外の終了コード、ログに記録されたアセンブル／逆コンパイルエラー、クラス重複による省略、不完全なコードを示す明示的なマーカー、空の Java ファイル、Java が生成されなかった結果は、いずれもコマンドの失敗となります。バックエンドの成功報告だけでは、各メソッドの正しさを独立に証明できません。

| 症状 | 対処 |
|------|------|
| Python／ヘルパーが見つからない | Python 3.10 以降をインストールまたは選択し、NeverD と同じ階層に `mobile/` ディレクトリを置く |
| バックエンドを起動できない、またはバージョンが未対応 | `--jadx`、完全な配布パッケージの構成、Java、バックエンドの最低バージョンを確認 |
| 不正な DEX ヘッダー／ルートに DEX がない | 実際の入力形式を確認し、コードを含む APK、通常の DEX、smali を使う |
| smali ファイルがない | Java ソースやアセットだけのツリーではなく、`.smali` ファイルを含むディレクトリを指定 |
| クラスの重複、または部分的な復元 | 重複する入力定義を除くか、関連するバイトコードの集合を分けて解析する。不完全な結果を受け入れず、不正な smali を修正する |
| タイムアウト／バイト数・エントリー数超過 | 関連する小さな入力に絞るか、対応する上限を意図的に引き上げる |
| 安全でないアーカイブパスやリンク | パストラバーサル名、リンク、特殊ファイル、競合するパスを含まない通常の可搬な入力を作り直す |
| 出力がすでに存在する | 別の出力ディレクトリを選び、過去に成功した出力先を再利用しない |

成功した実行ではバックエンドのログが残ります。失敗した作業ディレクトリはログも含めて削除されます。バックエンドがゼロ以外で終了した場合は、エラーに長さを制限した診断ログ末尾が含まれ、タイムアウトやリソース予算のエラーにはそれぞれのメッセージが表示されます。バックエンド固有の調査をする場合は、対象を切り分けた入力と別の診断ディレクトリを使い、バックエンド自身の CLI で再現してください。失敗の前に一部の Java が生成されただけで、成功と判断してはいけません。

## 検証とサポートの深さ

```sh
cmake --build build --target check-neverd-mobile
NEVERD_BUILD_DIR=build python3 -m unittest discover -s scripts/tests -p 'test_mobile_*.py' -v
python3 scripts/test_mobile_android_backend.py --jadx /opt/jadx/bin/jadx --neverd build/bin/neverd
```

最初の 2 つのコマンドは、mobile コンポーネントとビルド済み CLI の契約を検証します。プラットフォーム固有のテストフィクスチャ要件により、一部のテストが明示的にスキップされることがあります。実バックエンドのテストランナーには、さらに JDK（`java` と `javac`）が必要です。単一 smali、クラス間参照／ネストを含む smali、DEX、実際の multidex APK を作成し、復元した Java をコンパイルして実行します。分岐、ループ、配列、例外処理、クラス参照、不正な入力、クラス重複による省略を検証します。これは該当フィクスチャについての検証結果であり、任意のアプリケーションを完全に復元できるという約束ではありません。

[Mobile Decompilation ワークフロー](../.github/workflows/mobile.yml)では、Linux、macOS、Windows 上の Python 3.10 と 3.13 でコンポーネントテストを実行し、さらに Linux でチェックサムによりバージョンを固定した実 Android バックエンドのジョブを実行します。別系統の iOS ワークフローと現時点の制限については、[mobile の概要](mobile.md)（英語）を参照してください。
