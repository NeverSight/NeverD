**Languages**: [English](../ios.md) | [简体中文](../zh-CN/ios.md) | [繁體中文](../zh-TW/ios.md) | [日本語](ios.md) | [한국어](../ko/ios.md) | [Français](../fr/ios.md) | [Deutsch](../de/ios.md) | [Español](../es/ios.md) | [Italiano](../it/ios.md) | [Русский](../ru/ios.md) | [العربية](../ar/ios.md)

# iOS のネイティブコードとソースの復元

[← ドキュメント索引](README.md) · [モバイル概要](../mobile.md)

`neverd mobile` は IPA、`.app`、Mach-O を受け付け、ネイティブ C、ランタイムメタデータ、および対応するネイティブ本体から実験的に復元した Objective-C `.m` と Swift `.swift` を出力します。公開済みの結果にも未復元メソッドが含まれるため、使用前にカバレッジを確認してください。モバイルコンテナは CLI の機能であり、ネイティブ C SDK は選択済み Mach-O を別途読み込みます。

コンパイルによりコメント、書式、識別子、言語構造は失われます。この処理はソース表現を再構成するもので、元のテキストの復元や任意のアプリの動作同等性を保証しません。解析対象アプリを起動することもありません。

## 開始手順と依存関係

```sh
cmake --build build --target neverd
python3 --version
neverd mobile App.ipa -o recovered-ios
neverd mobile App.app -o recovered-arm64 --arch=arm64
neverd mobile executable -o metadata --metadata-only
neverd mobile App.app -o recovered-framework --artifact Frameworks/Example.framework/Example
```

NeverD を通常どおりビルドし、実行ファイルを配布する際は隣接する `mobile/` も保持してください。Python 3.10+ が必要です。選択順は `--python PATH`、`NEVERD_PYTHON`、PATH の `python3`/`python` です。依存関係は自動ダウンロードしません。生成した Apple 言語のソースを macOS で独立してコンパイルするには Apple Clang、SDK、Swift ツールチェーンが必要で、静的ネイティブ解析とは別の要件です。

Swift の署名復元は `--swift-demangle PATH`、`NEVERD_SWIFT_DEMANGLE`、PATH の `swift-demangle` の順で選択します。macOS では最後に時間制限付きの `xcrun --find swift-demangle` を試します。明示指定したツールがなければ失敗し、自動検索で見つからなければ未分類シンボルを残して `unavailable` と報告します。Swift シンボルがない入力には demangler は不要です。`--metadata-only` はネイティブバックエンドも demangler も呼びません。

```sh
neverd mobile App.ipa -o recovered-swift \
  --python python3 --swift-demangle /path/to/swift-demangle --timeout=600 --json
```

## 入力と選択

IPA には最上位の `Payload/*.app` がちょうど一つ必要です。IPA と `.app` のメイン実行ファイルは `Info.plist` の `CFBundleExecutable` で決まります。どちらも `--artifact` はアプリバンドルからの相対パスで、一つの埋め込み実行ファイルのみを選びます。すべての Framework や Extension を再帰解析する指定ではありません。生の Mach-O 入力には `--artifact` を使えません。

Fat バイナリの `--arch=auto` は arm64、arm、x86_64、i386 の順に優先します。不在または未対応のスライスは明示的に失敗します。現在のソース言語への出力は arm64/x86_64 が対象で、他のアーキテクチャ選択は Objective-C/Swift 対応を意味しません。選択したスライスが `cryptid != 0` なら拒否します。復号済みで読み取り可能な入力を用意してください。アーカイブとディレクトリでは危険なパス、シンボリックリンク、特殊ファイル、衝突する項目を拒否します。

## オプションとリソース制限

| オプション | 既定値 | 意味 |
|------------|--------|------|
| `-o DIRECTORY` | 必須 | 入力ディレクトリの外にある新規出力先。既存出力は保持 |
| `--platform=auto\|ios` | `auto` | プラットフォームを推定、または iOS を選択 |
| `--arch=auto\|arm64\|arm\|x86_64\|i386` | `auto` | Mach-O スライスを一つ選択 |
| `--artifact PATH` | メイン実行ファイル | アプリ基準の実行ファイル相対パス |
| `--metadata-only` | 無効 | ソース復元やツール呼び出しをせずメタデータを読む |
| `--max-func N` | `0` | ネイティブ関数上限。ゼロは検出した全関数。メタデータモードでは無視 |
| `--python PATH` | 環境/PATH | ヘルパー用 Python 3.10+ |
| `--swift-demangle PATH` | 環境/PATH/ツールチェーン | Swift 署名 demangler |
| `--timeout N` | `300` | バックエンドプロセスごとの正の秒数上限 |
| `--max-files N` | `20000` | 正の項目数予算。Swift シンボル一覧も制限対象 |
| `--max-bytes N` | `2147483648` | 入力、展開データ、最終出力の正のバイト予算 |
| `--json` | 無効 | バージョン付きレポートを JSON で出力 |

作業領域は監視され、一時入力と中間出力のために設定した項目数/バイト予算の最大三倍を許容します。ログはプロセスごとに 16 MiB、Swift 署名 JSON はさらに 32 MiB に制限されます。これはリソース制御でありプロセス隔離ではありません。タイムアウトを増やしても他の制限は無効になりません。`--max-func` で除外されてもメタデータ一覧にあるメソッドは未復元として残ります。

## Objective-C ソースとランタイム構造

ネイティブローダーはメソッド記録、実行可能な IMP アドレス、対応する型エンコーディングを明示的なソース ABI 位置に結び付けます。固定のスカラー/ポインター引数は隠れた `self`/`_cmd`、未使用引数、独立した整数/浮動小数点レジスタ群、対応するスタック位置を保持します。float/double のビット再解釈と数値変換は区別します。型ヒントはソース出力の入力であり、認証済み ABI 証拠や実行コードのパッチ許可ではありません。

`sources/objc.m` は実際に復元した文を `@implementation` 本体に置き、必要な C ヘルパーと型付き呼び出しを保持します。呼び出し先には対応するソース結合が必要で、未知の相手や不完全な依存グループは未復元です。定義欠落、不正な実行アドレス、競合する型、未対応 ABI、不完全なデコード、IR 検証拒否は、宣言があるだけで復元済みにはなりません。

クラスメタデータは親クラス、インスタンス開始位置/サイズ、オフセット・幅・アラインメントを検証したスカラー/ポインター ivar を保持します。宣言には必要なパディングを挿入します。必要なインスタンス配置が不明なメソッドは未復元です。Category はクラス/カテゴリ/アドレスの識別情報と実装を分離し、クラスとカテゴリ一覧に重複する完全に同じ記録は一度だけ数えます。対応する外部カテゴリは既存の Foundation クラス宣言を使用します。未知の外部ヘッダーは不足依存として報告し、代替クラス配置を捏造しません。

ランタイム情報の復元は限定的です。完全なプロパティ・プロトコル、元の所有権注釈、任意の集約型、可変引数末尾、例外に依存する本体、モデル外の Block/キャプチャ配置は保証しません。ランタイム型記述は固定引数のみを示し、元の宣言に省略記号がなかったとは証明できません。Chained Pointer はローダーが対象スロットを解決した場合のみ利用し、未解決形式は診断を残します。

## Swift ソースと記憶配置

構造化 demangler 出力は呼び出せる署名と呼び出せないメタデータを分離します。対応する署名は本体復元前に選択バイナリのシンボル、入口、明示的な機械 ABI と照合されます。Swift レシーバーは Swift ABI に従い、Objective-C の隠れた引数で代用しません。利用者が渡す署名ファイルも検証が必要なヒントです。

実験的エミッターは対応する自由関数、クラスメソッド、指定イニシャライザー、固定配置 struct のメソッドを構築し、一部の mutating レシーバーにも対応します。クラス/struct 宣言と格納フィールドには復元した配置メタデータが必要です。必要な宣言と本体が完全な対応済み依存グループを構成する場合にのみネイティブ呼び出しを出力します。ソース単位には宣言とメソッドをまとめ、元のバイナリを呼ぶブリッジは生成しません。

ジェネリック/resilient 配置、async/throwing 関数、不明な呼び出し規約、未対応 accessor/allocator/thunk、不完全な初期化、結合できないネイティブ/ランタイム依存は個別に `unrecovered` です。mangled シンボルや型名だけではメソッド復元ではありません。削除されたシンボルや未分類 demangler ノードはカバレッジを不完全または不明にします。

## 出力とカバレッジ

```text
recovered-ios/
  artifacts/selected.macho
  sources/native.c
  sources/objc.m
  sources/swift.swift
  metadata/objc.h
  metadata/objc.json
  metadata/objc-methods.json
  metadata/swift.json
  metadata/swift-signatures.json
  metadata/swift-methods.json
  logs/
  report.json
```

ソース言語のファイルはコードを出力できる場合だけ存在します。`objc.json` はクラス、カテゴリ、ivar、生のメソッド型記述を、`objc.h` は対応する宣言を保存します。`swift.json` は名義型メタデータと mangled シンボルです。署名/メソッド JSON は分類、省略、理由、件数を保持します。ログはネイティブ診断と、利用した場合の Swift ツール検索・demangling・ネイティブ Swift エクスポート診断を含みます。`report.json` の出力パスは同じディレクトリ基準です。選択バイナリは解析成果物であり、生成コードの復元ブリッジとしてリンクしません。

パッケージの一時コピーと中間 JSON は削除します。通常実行でネイティブ本体がなければ、メタデータがあっても失敗します。メタデータモードは選択ファイル、`objc.h`、`objc.json`、`swift.json`、`report.json` のみを生成し、ソースや署名/メソッドカバレッジはありません。`native_function_count`、`objc_method_recovery`、`swift_method_recovery` は `null` です。その Python Objective-C 読み取り器は chained pointer や再配置可能オブジェクトのポインターを解決しません。完全解析はネイティブローダーの解決済み情報を利用します。生の Swift メタデータは未対応参照を部分的と報告する場合があります。

以下は部分復元を示す短縮した例です。

```json
{
  "schema_version": 1,
  "status": "success",
  "platform": "ios",
  "architecture": "arm64",
  "objc_method_recovery": {
    "status": "partial",
    "method_count": 3,
    "recovered_method_count": 2,
    "unrecovered_method_count": 1
  },
  "swift_method_recovery": {
    "status": "partial",
    "coverage_status": "partial",
    "method_count": 4,
    "recovered_method_count": 2,
    "unrecovered_method_count": 2,
    "metadata_symbol_count": 5,
    "unclassified_symbol_count": 1
  }
}
```

外側の `status: "success"` は検証済み出力の公開を意味します。`recovered`、`partial`、`unrecovered`、`no-methods` は検出一覧に対する状態であり、意味的同等性や元プログラムの完全性ではありません。未復元メソッドには理由があります。Objective-C の `recovered` はランタイムメタデータの完全性も必要です。空一覧はメソッドが存在しなかった証拠にはなりません。

Swift の `coverage_status` は分類済みの呼び出し可能項目のみを数えます。全体の Swift `status` は未知シンボルも考慮し、`unavailable`、`unclassified`、`unsupported-architecture`、`no-symbols` になる場合があります。呼び出せないメタデータは `non_method_symbols` に `not-callable`、未知項目は `unclassified` として保存します。`types`、`type_metadata_count`、`source_type_count` は型情報と出力型単位を別に数え、メソッド数を増やす用途には使いません。

Swift バッチの `source_units` は `{kind, module, name, source, method_entries, method_identities}` を持ち、kind は `function` または `type`、各 identity は `{entry, mangled_symbol}` です。異なるシンボルは同じ入口を共有しつつ個別の ABI 出力を保持できます。各復元済み identity は一度だけ現れ、未復元 identity は含めません。`method_entries` は `method_identities` の入口を順に並べたものと一致し、アドレス重複を許します。同一 identity の重複を黙って統合してはいけません。バッチ `source` は各単位のソースと改行を順に結合したものです。Mobile は全体を `sources/swift.swift` に、単位の説明をカバレッジ JSON に保存します。個別メソッドの `source` は閲覧用で、単純連結ではクラス宣言を正しく構成できません。

## ネイティブ直接エクスポートと SDK

```sh
neverd export recovered-ios/artifacts/selected.macho \
  --format=objc-methods --max-func=20 -o objc-batch.json
neverd export recovered-ios/artifacts/selected.macho \
  --format=swift-methods \
  --source-signatures=recovered-ios/metadata/swift-signatures.json -o swift-batch.json
```

Swift エクスポートは demangler を使った通常の mobile 実行が生成する構造化署名一覧を受け取ります。Objective-C バッチ JSON は `native_source`、`native_function_count`、`objc_metadata` と、各メソッドの C ソース、関数名、戻り型、引数を含みます。Mobile は `.m` の前に宣言、本体、配置を追加検証するため、最終カバレッジは C バッチより狭くなる場合があります。ネイティブエクスポート成功時も復元メソッドがゼロの場合があります。

Mach-O 読み込み済みセッションでは `neverd_objc_methods_json(session, max_functions)` と `neverd_swift_methods_json(session, signatures_json, max_functions)` が対応するレポートを返します。ゼロは検出した全関数です。返された文字列は `neverd_free_string` で解放してください。`NULL` は失敗で、理由はセッションエラーにあります。これらの API は IPA/`.app` コンテナを読み込みません。

## 検証とトラブルシューティング

```sh
NEVERD_BUILD_DIR=build python3 -m unittest discover -s scripts/tests -p 'test_mobile_*.py' -v
python3 scripts/test_mobile_ios_backend.py --neverd build/bin/neverd
python3 scripts/test_mobile_swift_backend.py --neverd build/bin/neverd
```

macOS の自作 Objective-C 検証スクリプトは原サンプルをコンパイルし、`.m` を復元して、生成ソースだけを独立した呼び出しハーネスとリンクします。整数境界、分岐、ループ、ポインター読み書き、隠れた引数、float/double ビット同一性、混合/スタック引数を検証します。Swift スクリプトは生成 `.swift` とハーネスだけを再コンパイルし、原 dylib、モジュール、ブリッジ、手書き代替宣言を使いません。スカラー/ネイティブ呼び出し、クラス初期化/格納、struct の値/mutating メソッド、浮動小数点、スタック、ポインター、ループを調べます。厳格な検証は未対応ケースを検出する場合があり、スクリプトの存在自体は全ビルドでの全項目合格を意味しません。

両スクリプトは `--arch all|arm64|x86_64`、`--fixups both|classic|default`、`--timeout N`、`--work-dir NEW_DIRECTORY` を受け付けます。`--setup-only` は原サンプルのみを検証し、復元を検証しません。ホストが実行できないアーキテクチャは許容される場合に明示的にスキップし、合格とは数えません。保存した失敗成果物でカバレッジ不足、コンパイル失敗、動作差異を区別できます。検証済みと主張する前に現在の結果を確認してください。

公開はトランザクション方式です。新規ディレクトリを選び、最初に終了状態を確認し、リダイレクトする JSON はその外に置いてください。失敗時は一時出力を削除し既存結果を保持します。バックエンドの非ゼロ終了は制限付きログ末尾を含み、タイムアウトと予算超過には個別のメッセージがあります。`--json` の処理済みヘルパーエラーは `status: "error"` ですが、引数解析、ヘルパー/インタープリター不足、Python 3.10 未満、中断は先に stderr で失敗することがあります。

暗号化スライスには読み取り可能な入力を、架構不足には利用可能スライスの確認を、Swift ツール不足には実在する demangler の指定を行ってください。省略メソッドの正確な理由とメタデータ診断を確認します。`--max-func` の増加が有効なのは上限で除外された関数だけです。配置、署名、外部ヘッダー、例外、ABI 対応不足には実装や追加の有効メタデータが必要で、完全復元という主張では解消しません。配布時には適用される依存ライセンス通知を保持してください。
