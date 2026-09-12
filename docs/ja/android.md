**言語**: [English](../android.md) | [简体中文](../zh-CN/android.md) | [繁體中文](../zh-TW/android.md) | [日本語](android.md) | [한국어](../ko/android.md) | [Français](../fr/android.md) | [Deutsch](../de/android.md) | [Español](../es/android.md) | [Italiano](../it/android.md) | [Русский](../ru/android.md) | [العربية](../ar/android.md)

# Android の Java 復元

[← ドキュメント一覧](README.md)

`neverd mobile` は既定で NeverD の内蔵エンジンを使用し、APK、DEX、smali から読みやすい Java を復元します。独自実装の読み取り器が、型付き Dalvik モデルと処理量に上限のある Java 生成器を共有します。実験的な CLI 機能であり、JADX と同等の機能や、任意の APK の完全復元を保証しません。ネイティブ C SDK、Python プラグイン SDK、GUI ローダー、`neverd decompile --language` は APK コンテナと Java 出力に対応していません。

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
| NeverD | C++20 対応のツールチェーンで `neverd` ターゲットをビルドします。モバイル処理はネイティブ CLI に組み込まれており、Python インタープリターを呼び出しません。配布時は、そのビルドが必要とするネイティブライブラリを添付してください。| `build/bin/neverd` / PATH |

既定のエンジンは C++20 で実装されており、実行時に Python、Java、JADX は不要です。DEX 035、037–040 と smali のうち、表現可能な通常の宣言や操作を受け付けます。DEX 041、`invoke-custom` などの動的呼び出し、一部の初期化経路、未知の意味的な注釈や操作、Java で表現できない識別子は明示的に失敗します。ファイル形式への対応は、その形式のすべての命令や宣言への対応を意味しません。

Java の名前解決はクラスのヘッダーと本体を区別し、既知の同一パッケージ内での名前の隠蔽に対応しますが、外部のスーパークラスやインターフェースの宣言が不足し、型名や生成する Java 補助コードの参照先を確定できない場合は明示的に拒否し、宣言がなくても継承可能なメンバー型を持たないと扱うのは現在 `java.lang.Object` だけです。Smali の浮動小数点リテラルは、対象の単精度または倍精度へ直接丸め、その結果のビットパターンを保持します。

内蔵 C++ エンジンは、対応範囲内のクラス・フィールド・メソッドの `Signature` メタデータを保持し、検証します。型変数、配列、ワイルドカード、型境界、メソッド単位の名前の隠蔽が対象で、型消去後の宣言は元の DEX の宣言識別情報と一致する必要があります。`Throws` は破棄せず、例外の継承関係を証明できない場合は拒否します。ジェネリック継承やメンバー型の置換、ブリッジメソッドの再生成、ジェネリックメソッドの呼び出し、パラメーター化された内部型、コンストラクターの隠れた引数の対応付けは、必要な証明がない場合には引き続き明示的に非対応です。

クラス、フィールド、メソッド、コンストラクターに付いた、要素を持たず実行時に可視な Java 8 の `@java.lang.Deprecated` マーカーを保持します。パラメーターのアノテーション、アノテーション付きの静的初期化子、その他の可視性、および `since` や `forRemoval` などの要素値は拒否します。CI は再コンパイル後の classfile の `Deprecated` 属性と実行時アノテーションを別々に比較し、アノテーションのない対照宣言と生成された補助メソッドも確認します。補助メソッドは元のメソッド数に含めません。

DEX と smali は、クラス、フィールド、メソッド、コンストラクターのプラットフォームアノテーション `@android.annotation.SuppressLint` も保持します。build 可視性と、文字列配列の `value` 一つだけを要求し、空の配列や文字列、重複値、順序、エスケープ文字の値を保持します。パラメーターのアノテーションとアノテーション付き静的初期化子は引き続き非対応です。CI は実際の Android SDK を使い、再コンパイル後の CLASS 保持アノテーションとプログラムの動作を検証します。

内蔵エンジンは、対応するアノテーション宣言上の実行時に可視な `@Retention`、`@Target`、`@Documented`、`@Inherited` も保持し、実際の `@interface` を出力します。対象はフィールド、メソッド、型パラメーター、ネストした宣言を持たないものに限られます。名前、アクセス権、所属関係を確認できるトップレベルまたは static メンバーのアノテーションに対応し、ローカルクラスや匿名クラスのスコープは拒否します。

クラス、インターフェース、アノテーション宣言への空のマーカーの付与には、同じ解析対象のクラス集合内に、アクセス可能で一致するマーカー定義が必要です。その Retention と Target が実際の付与を許可していなければなりません。`SOURCE` の宣言は再構成できますが、入力に残るその付与は拒否します。`CLASS` または Retention 未指定の付与には DEX の build 可視性（`0`）、`RUNTIME` には runtime 可視性（`1`）が必要です。Retention の欠如と明示的な `CLASS`、Target の欠如と空配列を区別し、Target 配列の順序を保持します。Target は Java 8 の値に限定し、`MODULE` や `RECORD_COMPONENT` などの新しい値は拒否します。`@Inherited` を保持しても、継承された付与をサブクラスの直接の宣言としてコピーしません。

アノテーション要素の宣言やデフォルト値、値を持つ独自アノテーションの付与、フィールド・メソッド・パラメーターへの独自マーカー、外部のアノテーション定義、繰り返し可能なアノテーションのコンテナー、`kotlin.Metadata` は引き続き非対応です。マーカーの定義は要素メソッドや補助メソッドを持たないクラス宣言として出力され、復元済みメソッド数も増やしません。

CI では自作の Java 8 フィクスチャを D8 と NeverD で処理し、生成された Java 全体を再コンパイルして、完全な `Signature` メタデータ、リフレクションの結果、動作を比較します。これらのフィクスチャ検証は、実際のアプリの完全な復元を認定するものではありません。

### Linux と macOS

```sh
cmake --build build --target neverd

./build/bin/neverd mobile app.apk -o recovered-app
```

`NEVERD_JADX` や PATH 上の `jadx` は外部エンジンを選択しません。明示的な `--jadx PATH` のみが互換アダプターを選びます。自動切り替えはありません。空白を含むパスは引用符で囲んでください。

### Windows PowerShell

```powershell
& .\build\bin\neverd.exe mobile .\app.apk -o .\recovered-app
```

複数構成のビルドでは、実行ファイルが `build/bin/Release/` に置かれる場合があります。そのビルドの通常のネイティブライブラリ配布要件に従ってください。

## 対応する入力と範囲

| 入力 | 動作 | 重要な制限 |
|------|------|------------|
| `.apk` | ZIP 全体を検証し、ルートの `classes.dex`、`classes2.dex` および後続番号の DEX ファイルをまとめて解析 | コードのみ。リソースやマニフェストはデコードしない |
| `.dex` | 内蔵の読み取り器で DEX 035 または 037–040 を検証・解析 | DEX 041 と未対応の宣言・操作は失敗。名前変更や切り詰めでは有効なバイトコードにならない |
| `.smali` | 指定されたクラスを解析 | 参照先のほかのクラスは暗黙には読み込まない |
| smali ディレクトリ | `.smali` ファイルを再帰的に収集し、まとめて解析 | 入力ディレクトリにネストしたクラスと依存先の smali ルートを含める |

`smali/` と `smali_classes2/` を含む APK のデコード済みツリーを解析するには、両者の共通ディレクトリを指定します。バックエンドに渡されるのは `.smali` ファイルだけですが、最初に指定したツリー全体を検証してコピーするため、無関係な大きなアセットも入力制限に計上されます。関連する smali ルートだけを含むコンパクトなディレクトリにすると、処理量を減らせます。

分割 APK はそれぞれ独立した入力です。DEX を含む各 APK は個別に処理できますが、このコマンドは APK セットを統合しません。リソースのみの分割 APK は、ルートに DEX がないため失敗します。`.aab`、`.apks`、`.xapk`、`.odex`、`.oat`、`.vdex` は、この mobile コマンドの入力として受け付けません。バックエンド自体が一部の形式に対応していても、この NeverD コマンドが対応しているとは限りません。

APK のリソース、`AndroidManifest.xml`、アセット、JNI／ネイティブライブラリ、実行時にダウンロードされるコードは Java として復元されません。ネイティブの `.so` は別途取り出し、`neverd decompile library.so -o library.c` を使用してください。この静的な処理では、暗号化またはパックされたペイロードが、あらかじめ通常の DEX/smali として用意されている必要があります。アンパック、デバイスへのアタッチ、保護の回避は行いません。

## オプションと優先順位

```sh
neverd mobile app.apk -o recovered-app --platform=android \
  --timeout=600 --max-files=30000 --max-bytes=4294967296 --json
```

| オプション | 既定値 | 意味 |
|------------|--------|------|
| `-o DIRECTORY` | 必須 | 入力がディレクトリの場合はその外部に置く、新しい出力ディレクトリ。既存の出力は上書きしない |
| `--platform=auto\|android` | `auto` | Android を明示的に選択するか、入力からプラットフォームを推定 |
| `--jadx PATH` | 未指定：内蔵エンジン | 別途インストールした JADX 互換アダプターを明示的に選択。環境変数による選択や自動切り替えはない |
| `--timeout N` | `300` | 内蔵解析の正の時間上限。外部バックエンドではバージョン確認を含む各プロセスの秒数上限 |
| `--max-files N` | `20000` | 実際に作成されるディレクトリを含む、正のエントリー数上限 |
| `--max-bytes N` | `2147483648` | 入力、展開データ、最終出力のバイト数上限。正の値を指定 |
| `--json` | 無効 | 人向けの要約ではなく、JSON でレポートを出力 |

既定以外の `--arch`、`--artifact`、`--metadata-only`、ゼロ以外の `--max-func` は iOS 用であり、Android では拒否されます。明示的な `--arch=auto` は使用できます。任意のバックエンドオプションの転送には対応しません。明示的に選択した JADX アダプターは実行ごとに設定・キャッシュ・一時ディレクトリを隔離し、既存のバックエンド設定やプラグイン設定を読み込みません。

入力、展開データ、最終出力にはファイル数とバイト数の上限が適用されます。内蔵の読み取り器と生成器は処理量と経過時間も検査します。外部バックエンドの作業領域では入力と中間出力を併存させるため、設定した項目数・バイト数の最大 3 倍まで許可します。ログはプロセスごとに 16 MiB までです。これらはリソース制御であり、サンドボックスではありません。一つの上限を増やしても他の制限は無効になりません。

## 出力構成と JSON レポート

```text
recovered-app/
  sources/                       復元した Java のパッケージとクラス
  metadata/android-methods.json  内蔵エンジンのメソッド網羅状況
  report.json                    バージョン付き一覧と復元の制約
```

一時入力は削除されます。ネストしたクラスが外側のクラスのソースファイルを共有することがあるため、Java ファイル数と DEX クラス数は一致しません。生成したメソッドでは Java のディスパッチループを使うことがあります。元の DEX を実行したり、実行時のブリッジから呼び出したりはしません。

内蔵レポートは `android_method_recovery` を含み、同じ内容を `metadata/android-methods.json` に書き込み、元の全メソッドを保持します。計数は `method_count = recovered_method_count + projected_method_count + declaration_only_method_count + unrecovered_method_count` を満たします。省略された `projected_method_count` はゼロと扱い、公開前の `unrecovered_method_count` もゼロである必要があります。元の `native`・`abstract` メソッドは `declaration-only` として記録し、復元した本体には数えません。

名前があり外部変数をキャプチャしないローカルクラスの一部は、正確な所属先の static メソッド内に出力できます。通常のスカラーメソッド、`Object` を直接継承するフィールドのないクラス、実際の引数なしコンストラクター、スカラーのインスタンスメソッド、および対応範囲から逸出しないと検証されたオブジェクト使用が必要です。匿名クラス、キャプチャ、未対応の修飾子、証明できない使用は引き続き明示的に失敗します。

この出力ではローカルクラスのメソッドと所属先メソッドを `source-projected`、`projection_kind: "named-method-local"` と記録します。外側の処理結果が `success` でも網羅状況は `partial` です。再コンパイル後のバイナリ名とアクセスフラグは、いずれも未検証です。Java コンパイラーが異なるバイナリ名を選ぶ可能性があるため、`class_source_bindings` は元のクラス、正確な所属先メソッド、ソースパス、ローカル名を保持し、`binary_name_status` を `unverified` とします。

`generated_source_helpers` は追加メソッドを列挙し、種別には正確に `throw-helper`、`constant-helper`、`default-constructor`、`field-initializer` を使用します。最後の種別は、元のメソッド一覧には存在せず、追加で生成された `<clinit>` を示します。これらの追加メソッドは元のメソッド総数に含めません。コンパイル成功や一度の名前一致で完全復元へ昇格させることはありません。以下の省略例には投影メソッドはありません。

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

`input_kind` は `apk`、`dex`、`smali`、`smali-directory` のいずれかです。`input_code_files` は入力バイトコードの名前または smali のパスを列挙し、`java_sources` と `logs` は出力ルートからの相対パスです。`source` は入力のベース名です。実際のレポートには、ほかの復元上の制限事項も含まれます。結果を別のツールに渡す際にも、この情報を保持してください。

自動化では、`status` を利用する前にプロセスの終了コードを確認します。標準出力をリダイレクトする場合は、レポートを新しい出力ディレクトリの外に保存してください。

```sh
neverd mobile app.apk -o recovered-app --json > recovery-result.json
```

ネイティブ CLI は成功時にゼロ、復元失敗時に非ゼロを返します。`--json` では処理済みの失敗に `schema_version`、`status: "error"`、`error` が含まれます。引数解析、ネイティブ実行ファイルやライブラリの起動失敗、中断は stderr のみで報告される場合があります。まず終了状態を確認してください。

## 失敗時の処理とトラブルシューティング

出力の公開はトランザクションとして扱い、既存出力を保持し、失敗した一時出力を削除します。未対応の操作、未解決のレジスターデータフロー、表現できない宣言、不正な例外処理、処理上限超過では、欠落した本体を公開せず内蔵処理を失敗させます。外部アダプターも、非ゼロ終了、ログ上のアセンブル・逆コンパイルエラー、重複クラスの省略、不完全なコードのマーカー、空の Java ファイル、Java 出力の欠落を拒否します。復元成功は意味的等価性の証明ではありません。

| 症状 | 対処 |
|------|------|
| 未対応の DEX・命令・宣言・初期化 | 診断と対応範囲を確認する。独立した互換アダプターを意図して選ぶ場合のみ `--jadx PATH` を使う |
| 不正な入力や重複クラス | 入力バイトコードやクラス集合を修正する。未対応の本体が黙って省略されることはない |
| タイムアウトや処理上限超過 | 入力を絞るか、利用可能なリソースに合わせて `--timeout`、`--max-files`、`--max-bytes` を調整する |
| 出力が既に存在する | 新しい出力ディレクトリを選ぶ |

## 任意の JADX 互換アダプター

`--jadx PATH` は内蔵実装ではなく外部 JADX を選択します。標準の DEX/smali 入力プラグインを含む JADX 1.5.6 以降と Java 11 以降が必要です。完全な [JADX 配布パッケージ](https://github.com/skylot/jadx/releases/tag/v1.5.6)を取得し、`bin/` と `lib/` の構成を保ち、再配布時は同梱の依存ソフトウェアのライセンスも保持してください。自動ダウンロードは行いません。アダプターのレポートには実際の `jadx` エンジンと検出したバージョンを記録し、内蔵エンジンのメソッド網羅情報を提供するとは主張しません。

Windows では配布パッケージの `.bat`/`.cmd` または `lib/jadx-*-all.jar` を指定できます。NeverD は配布 JAR を解決して Java を直接起動し、アプリケーションのパスをコマンドシェルに渡しません。Java は `JAVA_HOME` または PATH で選択します。成功時は `logs/jadx-version.log` と `logs/jadx.log` を保持し、失敗した一時領域とログは削除します。バックエンドの非ゼロ終了時には、長さを制限したログ末尾がエラーに付きます。バックエンドのタイムアウトでも、すでに取得したログテキストが利用可能な場合は、元のタイムアウトメッセージを保ったまま長さを制限した末尾を付加します。起動失敗と処理上限超過の診断は従来どおりです。

```sh
neverd mobile app.apk -o recovered-jadx --jadx /opt/jadx/bin/jadx
python3 scripts/test_mobile_android_backend.py --jadx /opt/jadx/bin/jadx --neverd build/bin/neverd
```

## 検証とサポートの深さ

Python は以下の開発用テストスクリプトでのみ使用します。内蔵のモバイル復元はネイティブ C++20 CLI で実行されます。

```sh
cmake --build build --target check-neverd-mobile
ctest --test-dir build -L NeverDMobileTests --output-on-failure
python3 scripts/test_mobile_android_internal.py --d8 PATH --neverd build/bin/neverd
```

コンポーネントと CLI のテストでは、解析、出力契約、失敗時の削除を確認します。内蔵エンジンの実行比較ランナーは JDK（`java` と `javac`）と D8 で独立した DEX/APK サンプルを作り、復元した Java をコンパイルして実行します。これらは検証用の依存関係であり、内蔵復元の実行要件ではありません。現在のビルドで実行結果を確認してから、各ケースを検証済みとしてください。別の互換性ランナーはさらに JADX を必要とし、外部アダプターを検証します。サンプルの成功は任意のアプリケーションの完全復元を意味しません。

関連する iOS の処理は [モバイル概要](../mobile.md)を参照してください。
