**言語**: [English](../mobile.md) | [简体中文](../zh-CN/mobile.md) | [繁體中文](../zh-TW/mobile.md) | [日本語](mobile.md) | [한국어](../ko/mobile.md) | [Français](../fr/mobile.md) | [Deutsch](../de/mobile.md) | [Español](../es/mobile.md) | [Italiano](../it/mobile.md) | [Русский](../ru/mobile.md) | [العربية](../ar/mobile.md)

# モバイルアプリケーションの復元

[← ドキュメント一覧](README.md) · [完全な Android ガイド](android.md) · [完全な iOS ガイド](ios.md)

`neverd mobile` は Android の APK、DEX、smali 入力から読みやすい Java を復元します。iOS の IPA、`.app`、Mach-O 入力ではネイティブ C をエクスポートし、対応する Objective-C メソッド本体を `.m` ソースに再構成するとともに、実験的な Swift ソースとランタイムメタデータを出力します。実験的な CLI ワークフローであり、ネイティブ C SDK と GUI ローダーはモバイルコンテナーを受け付けません。

## セットアップ

C++20 に対応する環境で `neverd` ターゲットをビルドしてください。モバイルワークフローはネイティブ CLI に組み込まれ、Python インタープリターを使用しません。実行ファイルには、そのビルドが必要とするネイティブライブラリーを添えて配布してください。

ネイティブ ZIP 処理は CRC-32 と DEFLATE に zlib を使用します。CMake は `find_package` でインストール済みライブラリーを優先し、なければ固定の SHA256 で検証する zlib 1.3.2 をダウンロードして静的にビルドします。このモバイル ZIP 実装は Windows でも Python 補助ツールを必要としません。[THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md) の依存関係に関する通知を保持してください。

既定エンジンは C++20 で実装され、Python、Java、JADX ランタイムは不要です。明示的な `--jadx PATH` だけが、別途インストールした互換アダプターを選択します。`NEVERD_JADX` や PATH から自動選択せず、自動フォールバックもありません。任意のアダプターには標準 DEX/smali 入力プラグインを備えた JADX 1.5.6+ と Java 11+ が必要です。レポートには実際の `jadx` エンジンとバージョンを記録します。インストール方法と依存関係のライセンスは [Android ガイド](android.md#任意の-jadx-互換アダプター) に記載しています。

## Android

Java を復元せずにクラス一覧を素早く取得するには、
`neverd mobile app.apk --list-classes` を使用し、必要に応じて
`--class-prefix com.example` または `--json` を追加します。
[一覧の契約](android.md#高速なクラス一覧取得) は順序、上限、選択ペイロードの検証を説明します。
クエリーモードに出力ディレクトリは不要で、任意の `-o` は新しいファイルを指定します。

直接のバイトコード参照には
`neverd mobile app.apk --find-refs string --query 'example' --json` を使用します。
[参照クエリーの契約](android.md#コード参照クエリー) は type、method、field オペランド、
リテラル／完全一致、各出現位置、UTF-16 の保持、コード検証範囲も扱います。
`--json` がなければ JSON Lines を出力します。`-o` で新しいファイルを指定する動作も共通です。

以下の例と出力は復元を説明します。

```sh
neverd mobile app.apk -o recovered-app
neverd mobile classes.dex -o recovered-dex
neverd mobile MainActivity.smali -o recovered-class
neverd mobile decoded/smali -o recovered-java
```

APK のルートにあるすべての `classes.dex`、`classes2.dex` と後続の番号付き DEX ファイルを一緒に解析します。smali ディレクトリは再帰検索し、入れ子と兄弟のクラスを含む全クラスを 1 回の呼び出しで解析します。相互参照するクラスを復元する場合はディレクトリを使用してください。単一の smali ファイルから得られるのは、そのクラスだけです。

内蔵復元の出力は `sources/`、`metadata/android-methods.json`、`report.json` を含み、`backend: {"name": "neverd", "version": "1", "execution": "builtin"}` を記録します。レポートは `android_method_recovery` を埋め込み、`method_count = recovered_method_count + declaration_only_method_count` を満たします。公開前に `unrecovered_method_count` がゼロである必要があります。元の `native`／`abstract` 宣言は復元した本体と分けて数えます。明示的な外部アダプターは独自のバックエンドログを保持します。APK リソース、マニフェスト、ネイティブライブラリー、動的ロードされるコードはこの Java 経路の対象外です。ネイティブライブラリーは `neverd decompile` で別途解析できます。

内蔵復元リーダーは、独立に実装した型付き Dalvik モデルと処理量に上限のある Java エミッターを共有し、表現可能な通常の DEX 035/037–040 と smali コードを扱います。DEX 041、`invoke-custom` などの動的呼び出し、一部の初期化経路、未知の操作、Java で表現できない識別子は明示的に失敗します。生成 Java はディスパッチループを使う場合がありますが、元の DEX を実行したりランタイムブリッジを通して呼び出したりしません。元のコメント、書式、除去された名前は復元できません。実験的なエンジンは JADX との機能同等性、意味的等価性、任意の APK の完全復元を約束しません。

## iOS

[完全な iOS ガイド](ios.md) は IPA、`.app`、Mach-O の選択、セットアップ、すべての CLI オプション、ソーススキーマ、カバレッジ、検証を説明します。

```sh
neverd mobile App.ipa -o recovered-ios
neverd mobile App.app -o recovered-arm64 --arch=arm64
neverd mobile App.app -o framework-analysis --artifact Frameworks/Example.framework/Example
neverd mobile executable -o metadata --metadata-only
```

実行ごとに実行ファイルを 1 個選択します。`--artifact` は IPA と `.app` のどちらでもアプリケーションバンドルからの相対パスです。Fat の選択は arm64、arm、x86_64、i386 の順に優先し、ソース言語への投影は arm64/x86_64 を対象とします。選択したスライスが暗号化されている場合は拒否します。実験的なネイティブ経路は C と対応する Objective-C メソッド本体を出力し、スカラー／ポインター、浮動小数点、混合、スタックの ABI バインディングを含みます。ランタイムのクラス／ivar レイアウトと個別カテゴリは検証できた場合に保持し、未解決のレイアウト、シグネチャ、呼び出し、その他の依存関係は明示的な欠落として残します。

Swift 復元は NeverD LLVM フォークの `LLVMSwiftDemangle` を使用し、C++ プロセス内でシグネチャを分類します。外部デマングラーやツールチェーン探索コマンドは起動せず、NeverD のビルドにも実行にもインストール済み Swift コンパイラーは不要です。フォークのソースビルドと対応する LLVM パッケージにこのコンポーネントを含み、NeverD は別個の Swift ソース依存関係を取得しません。シグネチャ一覧は `demangler: {"name": "llvm-swift-demangle", "execution": "builtin", "version": "6.3.3"}` を記録します。対応するシグネチャをネイティブエントリーと ABI 位置に結び付けてから、実際の `.swift` 関数、クラスメソッド／イニシャライザー、固定レイアウト構造体メソッドを出力します。ジェネリック／resilient、async／throwing、未対応のランタイム生成 callable 形式、不完全なソース依存グループは未復元のままです。

通常の出力には `sources/native.c`、任意の `sources/objc.m` と `sources/swift.swift`、宣言とランタイムメタデータ、メソッド／シグネチャのカバレッジ JSON、ログ、`artifacts/selected.macho`、`report.json` が含まれます。外部 Swift ツールチェーン探索やデマングルのログはありません。生成ソースは元のバイナリーを復元ブリッジとして呼び出しません。Swift の `source_units` は型宣言とメソッドをグループ化します。単独のメソッド行を連結してクラスを再構築してはいけません。外側の `status: "success"` は出力の公開を意味し、完全なメソッドカバレッジや意味的等価性を意味しません。

`--metadata-only` はネイティブソースエクスポーターもシグネチャのデマングルも実行せず、ソースやメソッドカバレッジを出力しません。すべてのモードでネイティブローダーの解決済み Objective-C メタデータを使用します。Swift メタデータは範囲を限定したネイティブイメージ読み取りを使用し、未対応の fixup、再配置可能なレイアウト、参照には部分的な診断を残します。`--max-func` はネイティブ関数の復元を制限し、メタデータ専用モードでは無視します。ネイティブ関数本体が欠落すると通常の実行は失敗します。一時展開した入力は削除します。

元のコメント、書式、除去された識別子、コンパイル時に失われたソース構造は正確に再構成できません。出力を使用する前に、各メソッドの復元状態と理由、Swift の callable／non-callable／unclassified の個別集計、文書化された制限を確認してください。

## 上限と失敗

復元では `-o` はディレクトリ入力の外側にある新しいディレクトリを指定する必要があります。クエリーモードは任意の新規出力ファイルを受け付けます。既存の出力は上書きしません。復元作業は一時領域で行い、復元成功と出力検証の後だけ公開します。クエリー結果は選択したすべての DEX が成功するまでバッファリングします。ネイティブ CLI は成功するとゼロを返します。復元の失敗は非ゼロを返し、`--json` は処理済みの失敗を `schema_version`、`status: "error"`、`error` で報告します。引数解析、ネイティブ実行ファイルやライブラリーの起動失敗、中断は代わりに stderr へ報告する場合があります。利用側は終了ステータスを最初に確認する必要があります。

既定値は 20,000 エントリー、入力／展開データまたは最終出力データ 2 GiB、内蔵 Android/iOS 解析または明示的な各 JADX プロセスに 300 秒です。iOS 子プロセスには合計解析予算の残り時間を渡します。内蔵リーダーとエミッターも有限の処理量予算を強制します。`--max-files`、`--max-bytes`、`--timeout` で、正の値であるこれらの上限を調整します。バックエンドの実行中は一時作業領域を監視し、一時入力と中間出力が共存できるようにエントリー／バイト上限の最大 3 倍まで許可します。診断はプロセスごとに 16 MiB までです。

復元中の APK ステージングは、ルートの `classes.dex`、`classes2.dex` と後続の番号付き DEX ファイルだけを書き出します。それでもすべての ZIP メンバーについてヘッダー／範囲検査、展開、長さと CRC の検証を行い、アーカイブのエントリー数と非圧縮バイト数の上限に含めます。書き出さないリソースは `res/-A.xml` と `res/-a.xml` のように大文字と小文字で区別される名前を持てます。完全に重複した ZIP 名とファイル／ディレクトリの同一性の衝突は引き続きエラーです。可搬ファイルシステムの大文字小文字衝突検査は、実際に書き出すメンバーに適用します。IPA 入力を含む完全展開は、引き続きこのような出力衝突を拒否します。アーカイブ全体でパストラバーサル、リンク、特殊ファイル、暗号化 ZIP エントリーを拒否します。ディレクトリ入力でもシンボリックリンクと特殊ファイルを拒否します。

これらの上限は堅牢性のための制御であり、第三者のバックエンドコード用のサンドボックスではありません。明示的な JADX とネイティブソースエクスポートコマンドはローカル子プロセスとして実行します。失敗したステージング出力は削除します。バックエンドが非ゼロで終了すると、長さを限定した診断の末尾を含めます。バックエンドのタイムアウトではタイムアウトメッセージを保持し、取得したログテキストがある場合は長さを限定した末尾を追加します。起動失敗と予算違反はそれぞれのエラーメッセージを保持します。

## 検証

Python は以下の開発テストハーネスだけで使用します。内蔵モバイル復元はネイティブ C++20 CLI 内で動作します。

```sh
cmake --build build --target check-neverd-mobile
ctest --test-dir build -L NeverDMobileTests --output-on-failure
python3 scripts/test_mobile_android_internal.py --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_backend.py --jadx /opt/jadx/bin/jadx --neverd build/bin/neverd
```

コンポーネントテストは解析、安全でないコンテナー、バックエンド失敗、出力クリーンアップ、アーキテクチャ選択、既存出力の保持を検査します。ビルド済み CLI を呼び出すテストは `NEVERD_BUILD_DIR` を使用します。内蔵 Android ランナーは JDK（`java` と `javac`）および D8 で独立した DEX/APK フィクスチャーを作り、復元した Java をコンパイルして実行します。これらは検証の依存関係であり、内蔵復元の要件ではありません。ケースを検証済みと扱う前に、現在のビルドで実行して結果を確認してください。別の互換性ランナーは追加で JADX が必要です。フィクスチャーの成功は任意のアプリケーションの復元を証明しません。

Apple Clang とその SDK、ビルド済み NeverD がある macOS では、実際の Objective-C 実行比較を行います。

```sh
python3 scripts/test_mobile_ios_backend.py --neverd build/bin/neverd
cmake --build build --target check-neverd-mobile-ios
```

ランナーは自身の Objective-C フィクスチャーをビルドし、メソッド実装を復元して、復元した `.m` だけを同じ独立した呼び出しハーネスにリンクします。22 メソッドのフィクスチャーは 141 個の観測可能な結果を比較し、整数境界、分岐、ループ、ポインターの読み書き、隠れた引数と未使用引数、float/double の同一性ビット、混合引数、スタック位置を対象とします。ケースを検証済みと扱うには現在のビルドでの成功が必要です。要求したすべてのアーキテクチャと fixup 変種が完了する必要があり、既定は arm64/x86_64 × classic/default です。変種の欠落やホストが実行できないアーキテクチャは失敗とし、スキップは許可しません。このフィクスチャー証拠は任意の iOS プログラムに対する完全性を確立しません。`NeverDMobileIOSBackend` はメイン CI テストプロファイルを含め、macOS で CTest に登録されます。

独立した Swift 復元ランナーは `python3 scripts/test_mobile_swift_backend.py --neverd build/bin/neverd` です。元のバイナリーをリンクせずに、生成 Swift とハーネスを再コンパイルします。未対応の callable と動作の差異は失敗です。カバレッジの意味と保持する失敗証拠は [iOS ガイド](ios.md) を参照してください。

[Mobile Real Applications ワークフロー](../../.github/workflows/mobile-real-apps.yml) は [corpus manifest](../../scripts/mobile_real_apps.json) で固定した公開アプリケーションを使用します。合格ゲートには、各 APK 内のすべての DEX と完全な各 iOS バンドル内のすべての Mach-O の独立した一覧、元のプログラムと生成ソースの独立した再ビルド、動作比較が必要です。段階の欠落、一覧のカバレッジが不明な状態、必須ケースの欠落はゲートを失敗させます。実アプリの再コンパイルと動作段階は未完了であるため、サポートには Experimental ラベルを維持します。ハーネスのガードテストに合格しても、実アプリの成功を確立したことにはなりません。

Android ケースは Android SDK の宣言だけを使い、一覧に含まれる生成 Java 全体を javac と D8 でコンパイルしようとします。元のアプリケーションのバイトコード、依存関係の実装、代替スタブで復元の欠落を補うことはできません。部分復元とコンパイラーエラーは証拠に残します。コンパイル成功後も、独立検証、完全な APK 再構築、ART の動作比較が必要です。iOS オラクルはディスク上の Objective-C メソッド記録を Apple ツール出力と照合し、class、metaclass、category、list、ordinal の識別情報を保持します。未解決ポインターや省略スロットがあれば一覧は不明のままです。実機とシミュレーターの独立した SDK 宣言一覧はフレームワークインポート作業を支援しますが、ヘッダーをインスタンスレイアウトや動作の証拠として扱いません。
