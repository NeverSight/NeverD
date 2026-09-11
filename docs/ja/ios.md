**Languages**: [English](../ios.md) | [简体中文](../zh-CN/ios.md) | [繁體中文](../zh-TW/ios.md) | [日本語](ios.md) | [한국어](../ko/ios.md) | [Français](../fr/ios.md) | [Deutsch](../de/ios.md) | [Español](../es/ios.md) | [Italiano](../it/ios.md) | [Русский](../ru/ios.md) | [العربية](../ar/ios.md)

# iOS のネイティブコードとソースの復元

[← ドキュメント索引](README.md) · [モバイル概要](../mobile.md)

`neverd mobile` は IPA、`.app`、Mach-O を受け付け、ネイティブ C、ランタイムメタデータ、および対応するネイティブ本体から実験的に復元した Objective-C `.m` と Swift `.swift` を出力します。公開済みの結果にも未復元メソッドが含まれるため、使用前にカバレッジを確認してください。モバイルコンテナは CLI の機能であり、ネイティブ C SDK は選択済み Mach-O を別途読み込みます。

コンパイルによりコメント、書式、識別子、言語構造は失われます。この処理はソース表現を再構成するもので、元のテキストの復元や任意のアプリの動作同等性を保証しません。解析対象アプリを起動することもありません。

## 開始手順と依存関係

```sh
cmake --build build --target neverd
neverd mobile App.ipa -o recovered-ios
neverd mobile App.app -o recovered-arm64 --arch=arm64
neverd mobile executable -o metadata --metadata-only
neverd mobile App.app -o recovered-framework --artifact Frameworks/Example.framework/Example
```

C++20 対応のツールチェーンで `neverd` ターゲットをビルドします。モバイル処理はネイティブ CLI 内で動作し、Python インタープリターを必要としません。Swift 署名のデマングルには NeverD LLVM fork の `LLVMSwiftDemangle` を使います。このコンポーネントは fork のソースビルドと対応する配布 LLVM パッケージの両方に含まれ、NeverD が Swift ソースを別途取得することはありません。NeverD のビルドと実行に Swift コンパイラーやツールチェーンのインストールは不要です。LLVM、Capstone などのネイティブライブラリへの依存は引き続き存在するため、必要なライブラリとライセンス通知を配布してください。macOS で生成した Apple 言語のソースを独立してコンパイルし、Swift の動作回帰テストを実行する場合には、用途に応じて Apple Clang、SDK、`swiftc` が必要です。

Swift 署名の復元は、C++ プロセス内で `LLVMSwiftDemangle` の構造化ノードを直接利用します。外部のデマングル実行ファイルの検索・起動や、ツールチェーン探索コマンドの実行は行いません。従来の実行ファイルパス指定オプションは削除され、従来のデマングラー環境変数も読み取りません。`--metadata-only` はネイティブのソースエクスポーターも署名のデマングルも実行しません。

`metadata/swift-signatures.json` の署名一覧には、内蔵コンポーネントが次のように記録されます。

```json
{
  "demangler": {
    "name": "llvm-swift-demangle",
    "execution": "builtin",
    "version": "6.3.3"
  }
}
```

```sh
neverd mobile App.ipa -o recovered-swift --timeout=600 --json
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
| `--timeout N` | `300` | 解析全体の正の時間上限（秒）。子プロセスには残りの時間を割り当てる |
| `--max-files N` | `20000` | 正の項目数予算。Swift シンボル一覧も制限対象 |
| `--max-bytes N` | `2147483648` | 入力、展開データ、最終出力の正のバイト予算 |
| `--json` | 無効 | バージョン付きレポートを JSON で出力 |

作業領域は監視され、一時入力と中間出力のために設定した項目数/バイト予算の最大三倍を許容します。ログはプロセスごとに 16 MiB、Swift 署名 JSON はさらに 32 MiB に制限されます。これはリソース制御でありプロセス隔離ではありません。タイムアウトを増やしても他の制限は無効になりません。`--max-func` で除外されてもメタデータ一覧にあるメソッドは未復元として残ります。

## Objective-C ソースとランタイム構造

ネイティブローダーはメソッド記録、実行可能な IMP アドレス、対応する型エンコーディングを明示的なソース ABI 位置に結び付けます。固定のスカラー/ポインター引数は隠れた `self`/`_cmd`、未使用引数、独立した整数/浮動小数点レジスタ群、対応するスタック位置を保持します。float/double のビット再解釈と数値変換は区別します。型ヒントはソース出力の入力であり、認証済み ABI 証拠や実行コードのパッチ許可ではありません。

`sources/objc.m` は実際に復元した文を `@implementation` 本体に置き、必要な C ヘルパーと型付き呼び出しを保持します。呼び出し先には対応するソース結合が必要で、未知の相手や不完全な依存グループは未復元です。定義欠落、不正な実行アドレス、競合する型、未対応 ABI、不完全なデコード、IR 検証拒否は、宣言があるだけで復元済みにはなりません。

クラスメタデータは親クラス、インスタンス開始位置/サイズ、オフセット・幅・アラインメントを検証したスカラー/ポインター ivar を保持します。宣言には必要なパディングを挿入します。必要なインスタンス配置が不明なメソッドは未復元です。Category はクラス/カテゴリ/アドレスの識別情報と実装を分離し、クラスとカテゴリ一覧に重複する完全に同じ記録は一度だけ数えます。対応する外部カテゴリは既存の Foundation クラス宣言を使用します。未知の外部ヘッダーは不足依存として報告し、代替クラス配置を捏造しません。

復元済みと判定するには、完全なクラス宣言とローカルの祖先クラスの定義に加え、生成ソースのすべての使用経路で値が使用前に定義され、到達可能な出口に必要な戻り処理があることも求められます。空のローカル親クラスの `@implementation` は、検証済みの配置と完全なメソッド一覧から自身の通常メソッドがないと証明できる場合にのみ生成し、証拠不足や既知の Foundation インポート名との衝突がある場合は、該当メソッドを理由付きでカバレッジの分母に残しつつ独立して復元可能なクラスを出力しますが、この名前検査はすべての SDK 名、iOS SDK、バージョンを網羅するものではありません。

対応する Objective-C Block 呼び出しには、暗黙の Block オブジェクトと全引数・戻り値の格納位置を含む、完全な固定スカラー呼び出し ABI が必要です。実行時エンコーディング `@?` を `id` に広げるのは宣言だけであり、呼び出しプロトタイプは確定しません。グローバル Block の参照は共有オブジェクトの同一性を維持します。対応する同期スカラーキャプチャでは、実際の格納位置と呼び出しフローを証明します。エスケープするキャプチャ、非同期キャプチャ、モデル化されていないオブジェクト/byref の所有権、copy/dispose ヘルパー、未知のレイアウトは未復元のままです。

ランタイム情報の復元は限定的です。完全なプロパティ・プロトコル、元の所有権注釈、任意の集約型、可変引数末尾、例外に依存する本体、モデル外の Block/キャプチャ配置は保証しません。ランタイム型記述は固定引数のみを示し、元の宣言に省略記号がなかったとは証明できません。Chained Pointer はローダーが対象スロットを解決した場合のみ利用し、未解決形式は診断を残します。

## Swift ソースと記憶配置

構造化 demangler 出力は呼び出せる署名と呼び出せないメタデータを分離します。対応する署名は本体復元前に選択バイナリのシンボル、入口、明示的な機械 ABI と照合されます。Swift レシーバーは Swift ABI に従い、Objective-C の隠れた引数で代用しません。利用者が渡す署名ファイルも検証が必要なヒントです。

実験的エミッターは対応する自由関数、クラスメソッド、指定イニシャライザー、固定配置 struct のメソッドを構築し、一部の mutating レシーバーにも対応します。クラス/struct 宣言と格納フィールドには復元した配置メタデータが必要です。必要な宣言と本体が完全な対応済み依存グループを構成する場合にのみネイティブ呼び出しを出力します。ソース単位には宣言とメソッドをまとめ、元のバイナリを呼ぶブリッジは生成しません。

対応する Swift getter/setter の本体はネイティブ実装から復元し、プロパティに組み込みます。非公開の backing storage は確認済みのフィールド配置を保ち、初期化子や他のメソッドも同じ格納名を使います。プロパティ宣言やフィールド記録だけでは、アクセサー本体の復元を証明できません。

対応する割り当て初期化子、単純なデストラクター/解放処理、型メタデータアクセサー、`_modify`/resume エントリーは、出力済みの型単位に投影できます。各項目には、ネイティブの全フローと効果の有界な証明、実際に復元されたコンテキスト・初期化子・プロパティの依存関係、および関連本体の例外処理と IR の監査が必要です。割り当て側の書き込みは実際の初期化子と一致し、`_modify` は正確な可変フィールドと継続エントリーに結び付かなければなりません。実行時メタデータ呼び出しのモデル上の意味は復元された型内で維持します。これらはコンパイラーのソース投影として明示され、個別に復元した通常のメソッド本体や元のソース文字列を意味しません。

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

ソース言語のファイルはコードを出力できる場合だけ存在します。`objc.json` はクラス、カテゴリ、ivar、生のメソッド型記述を、`objc.h` は対応する宣言を保存します。`swift.json` は名義型メタデータと mangled シンボルです。署名/メソッド JSON は分類、省略、理由、件数を保持します。ログにはネイティブ診断と、実行された場合のネイティブ Swift エクスポート診断が含まれます。外部 Swift ツールチェーンの探索やデマングルのログは生成されません。`report.json` の出力パスは同じディレクトリ基準です。選択バイナリは解析成果物であり、生成コードの復元ブリッジとしてリンクしません。

パッケージの一時コピーと中間 JSON は削除します。通常実行でネイティブ本体がなければ、メタデータがあっても失敗します。メタデータモードは選択ファイル、`objc.h`、`objc.json`、`swift.json`、`report.json` のみを生成し、ソースや署名/メソッドカバレッジはありません。`native_function_count`、`objc_method_recovery`、`swift_method_recovery` は `null`。すべてのモードで、ネイティブローダーが解決した Objective-C メタデータを使用します。Swift メタデータは範囲を検証したネイティブイメージ読み取りを使用し、未対応の fixup、再配置可能レイアウト、参照には部分的な解析の診断が残ります。

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
    "source_body_method_count": 1,
    "compiler_projection_method_count": 1,
    "unrecovered_method_count": 2,
    "metadata_symbol_count": 5,
    "unclassified_symbol_count": 1
  }
}
```

外側の `status: "success"` は検証済み出力の公開を意味します。`recovered`、`partial`、`unrecovered`、`no-methods` は検出一覧に対する状態であり、意味的同等性や元プログラムの完全性ではありません。未復元メソッドには理由があります。Objective-C の `recovered` はランタイムメタデータの完全性も必要です。空一覧はメソッドが存在しなかった証拠にはなりません。

Swift の `coverage_status` は分類済みの呼び出し可能項目のみを数えます。全体の Swift `status` は未知シンボルも考慮し、`unclassified`、`unsupported-architecture`、`no-symbols` になる場合があります。呼び出せないメタデータは `non_method_symbols` に `not-callable`、未知項目は `unclassified` として保存します。`types`、`type_metadata_count`、`source_type_count` は型情報と出力型単位を別に数え、メソッド数を増やす用途には使いません。

復元済み Swift 行の `source_representation` は `native-method-body` または `compiler-generated-from-type` です。コンパイラー投影には `compiler_projection_kind` と `compiler_projection_evidence` も残します。`source_body_method_count` は復元したネイティブメソッド本体、`compiler_projection_method_count` は証明済みのコンパイラー投影を数え、合計は `recovered_method_count` と一致します。コンパイラーのエントリーも `method_count` の分母に残り、正確な識別情報を対応する一つの `type` ソース単位に記録します。型メタデータや依存先の名前だけで復元数を増やすことはありません。ネイティブの一括 JSON ではコンパイラーの行と型単位に `source` を含みますが、mobile の `source_units` は説明のみを保持して `source` を含まず、完全なソースは `sources/swift.swift` に保存されます。

Swift バッチの `source_units` は `{kind, module, name, source, method_entries, method_identities}` を持ち、kind は `function` または `type`、各 identity は `{entry, mangled_symbol}` です。異なるシンボルは同じ入口を共有しつつ個別の ABI 出力を保持できます。各復元済み identity は一度だけ現れ、未復元 identity は含めません。`method_entries` は `method_identities` の入口を順に並べたものと一致し、アドレス重複を許します。同一 identity の重複を黙って統合してはいけません。バッチ `source` は各単位のソースと改行を順に結合したものです。Mobile は全体を `sources/swift.swift` に、単位の説明をカバレッジ JSON に保存します。個別メソッドの `source` は閲覧用で、単純連結ではクラス宣言を正しく構成できません。

## ネイティブ直接エクスポートと SDK

```sh
neverd export recovered-ios/artifacts/selected.macho \
  --format=objc-methods --max-func=20 -o objc-batch.json
neverd export recovered-ios/artifacts/selected.macho \
  --format=swift-methods \
  --source-signatures=recovered-ios/metadata/swift-signatures.json -o swift-batch.json
```

Swift エクスポートは、通常の mobile 実行で内蔵の署名パーサーが生成した構造化署名一覧を受け取ります。Objective-C バッチ JSON は `native_source`、`native_function_count`、`objc_metadata` と、各メソッドの C ソース、関数名、戻り型、引数を含みます。Mobile は `.m` の前に宣言、本体、配置を追加検証するため、最終カバレッジは C バッチより狭くなる場合があります。ネイティブエクスポート成功時も復元メソッドがゼロの場合があります。

Mach-O 読み込み済みセッションでは `neverd_objc_methods_json(session, max_functions)` と `neverd_swift_methods_json(session, signatures_json, max_functions)` が対応するレポートを返します。ゼロは検出した全関数です。返された文字列は `neverd_free_string` で解放してください。`NULL` は失敗で、理由はセッションエラーにあります。これらの API は IPA/`.app` コンテナを読み込みません。

## 検証とトラブルシューティング

macOS で `BUILD_TESTING` を有効にしたビルドには `check-neverd-mobile-ios` があり、CTest で三つのネイティブ復元テストスイートを実行します。

Python は以下の開発用テストスクリプトでのみ使用します。内蔵のモバイル復元はネイティブ C++20 CLI で実行されます。

```sh
cmake --build build --target check-neverd-mobile-ios
ctest --test-dir build -L NeverDMobileTests --output-on-failure
python3 scripts/test_mobile_ios_backend.py --neverd build/bin/neverd
python3 scripts/test_mobile_ios_calls_backend.py --neverd build/bin/neverd
python3 scripts/test_mobile_swift_backend.py --neverd build/bin/neverd
```

macOS の Objective-C 検証スクリプトは元のサンプルをコンパイルし、`.m` を復元した後、生成ソースだけを独立した呼び出しハーネスとリンクします。スカラー用スクリプトは整数境界、分岐、ループ、ポインター読み書き、暗黙引数、float/double のビット同一性、混合引数、スタック引数を扱います。呼び出し用スクリプトはメッセージ配送、継承、Category、インスタンス変数、ネイティブヘルパー、Block 呼び出し・キャプチャ・共有同一性も扱います。呼び出しサンプルは、arm64/x86_64 × classic/default の各構成で 21/21 件のメソッド復元と 134/134 件の独立した期待結果への一致を要求します。現在のネイティブ CLI ビルドで検証してください。

厳格な Swift スクリプトは、ユーザー宣言 22 件、getter/setter エントリー 3 件、コンパイラー生成の呼び出し可能エントリー 7 件を確認し、一覧からの欠落を許しません。各バリアントで元のプログラムに対して 855 件の独立した期待結果を確認します。生成した `.swift` とハーネスを独立にコンパイルし、元の dylib、モジュール、ブリッジ、手書きの代替宣言は使用しません。スカラー/ネイティブ呼び出し、クラス初期化と格納、構造体の値渡し/mutating メソッド、浮動小数点とスタック引数、ポインター、ループを扱います。ネイティブ C++20 CLI の受け入れ条件は、arm64/x86_64 × classic/default の四構成ですべてスキップなしに成功することです。各構成でネイティブ本体 25 件とコンパイラー投影 7 件を復元し、呼び出し可能な識別情報 32 件を保持し、元のプログラムと独立にコンパイルした生成 Swift がそれぞれ 855/855 件の期待結果に一致する必要があります。この結果は当該サンプルに限られ、任意のアプリケーションや元のソース文字列の復元を保証しません。スクリプトはカバレッジ欠落、ソースのコンパイル失敗、動作の不一致を拒否します。

三つのスクリプトは `--arch all|arm64|x86_64`、`--fixups both|classic|default`、`--timeout N`、`--work-dir NEW_DIRECTORY` に対応します。`--setup-only` は元のサンプルだけを検証し、復元はテストしません。スカラー検証を含め、指定したすべてのアーキテクチャと fixup の組み合わせを完了する必要があります。組み合わせが欠ける場合や、ホストで指定アーキテクチャを実行できない場合は失敗となり、スキップは認められません。保存された失敗成果物でソースの欠落、コンパイルエラー、動作の差を区別し、検証済みと主張する前に現在のテスト結果を確認してください。

[Mobile Real Applications ワークフロー](../../.github/workflows/mobile-real-apps.yml) は、[コーパスマニフェスト](../../scripts/mobile_real_apps.json)でバージョンを固定した公開アプリを使用します。合格には、iOS バンドル内の全 Mach-O と APK 内の全 DEX の独立した一覧、元のアプリと生成ソースの独立した再ビルド、動作比較が必要です。未完了の段階、一覧の網羅性が不明な項目、必須ケースの欠落はいずれも不合格になります。実アプリの recompile と behavior 段階はまだ未完了のため、Experimental 表記を維持します。テスト基盤の誤判定防止テストに通っても、実アプリの検証に合格したことにはなりません。

公開はトランザクション方式です。新規ディレクトリを選び、最初に終了状態を確認し、リダイレクトする JSON はその外に置いてください。失敗時は一時出力を削除し既存結果を保持します。バックエンドの非ゼロ終了時には、長さを制限したログ末尾がエラーに付きます。バックエンドのタイムアウトでも、すでに取得したログテキストが利用可能な場合は、元のタイムアウトメッセージを保ったまま長さを制限した末尾を付加します。予算超過のメッセージは従来どおりです。ネイティブ CLI は成功時にゼロ、復元失敗時に非ゼロを返します。`--json` では処理済みの失敗に `schema_version`、`status: "error"`、`error` が含まれます。引数解析、ネイティブ実行ファイルやライブラリの起動失敗、中断は stderr のみで報告される場合があります。まず終了状態を確認してください。

暗号化スライスには読み取り可能な入力を用意し、必要なアーキテクチャが見つからない場合は利用可能なスライスを確認してください。省略メソッドの正確な理由とメタデータ診断を確認します。`--max-func` の増加が有効なのは上限で除外された関数だけです。配置、署名、外部ヘッダー、例外、ABI 対応不足には実装や追加の有効メタデータが必要で、完全復元という主張では解消しません。配布時には適用される依存ライセンス通知を保持してください。
