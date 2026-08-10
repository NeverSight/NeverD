**Languages**: [English](testing.md) | [简体中文](testing.zh-CN.md) | [繁體中文](testing.zh-TW.md) | [日本語](testing.ja.md) | [한국어](testing.ko.md) | [Français](testing.fr.md) | [Deutsch](testing.de.md) | [Español](testing.es.md) | [Italiano](testing.it.md) | [Русский](testing.ru.md) | [العربية](testing.ar.md)

[← ドキュメント索引](README.ja.md)

# NeverD のテスト

NeverD のテストは、表現が期待した形か、バイナリ fixture のパイプライン経路が
動くか、生成コードが動作を保つか、という 3 つの異なる問いに答えます。変更の
問いに答える最小のスイートを選び、リスクの高いプルリクエストではより広い集約を
実行してください。

## テストビルドの構成

`BUILD_TESTING` を有効にしない限りテストは無効です。全スイートには通常 Release
を使います。Debug はアサーションとステップ実行を保ちますが、意図的に未最適化で
デコードベンチマークを代表しません。

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build-release --parallel 4
```

全 fixture には、クロスターゲットコンパイル用の `clang` と、`PATH` 上の LLVM
linker（`ld.lld` と `lld-link`）が必要です。CMake は多数の再配置可能 fixture を
常に作り、対応する linker があればリンク済み ELF/PE fixture も作ります。ホストが
fixture をコンパイル/リンクできずスキップされたテストは未実行のカバレッジであり、
そのターゲットの合格ではありません。

クローン、ビルドプロファイル、macOS のプリビルド LLVM は
[CONTRIBUTING.md](i18n/CONTRIBUTING.ja.md)を参照してください。

## テスト構成

`add_neverd_unittest` は GoogleTest 実行ファイルを 1 つ作り、検出した各ケースに
その実行ターゲット名と同じ CTest ラベルを割り当てます。

| ソース領域 | ターゲットと CTest ラベル | 対象 |
|------------|---------------------------|------|
| `unittests/TestProcessTests.cpp` | `NeverDTestProcessTests` | クロスプラットフォーム子プロセス、引用、リダイレクト、終了コード |
| `unittests/libc` | `NeverDLibCTests` | 既知の libc 名と分類 |
| `unittests/lift` | `NeverDLiftTests` | Decoder/lifter の LowIR 形状、IR 段階、loader、relocation、形式 fixture、デコンパイル、代表的 patch 経路 |
| `unittests/semantic` の大半 | `NeverDSemanticTests` | 命令、ABI、制御フロー、C 式、lift/recompile の差分セマンティクス |
| `unittests/sbf` | `NeverDSBFMetadataTests`、`NeverDSBFLoaderTests`、`NeverDSBFAnalyzerTests`、`NeverDSBFSemanticTests`、`NeverDSBFLLVMEmitterTests`、`NeverDSBFEmitterTests`、`NeverDSBFIntegrationTests` | v0-v4 メタデータと ELF レイアウト、厳格な検証、CFG/復元、独立した raw 実行、LLVM 検証、C/Rust コンパイル、公開 API ルーティング |
| `PatchFullSubstRTTests.cpp` | `NeverDPatchFullTests` | 4 ISA×3 オブジェクト形式の書き換え/難読化等価性 |
| `unittests/semantic` の重点変換ファイル | `NeverDSwitchXformTests`、`NeverDIndCallXformTests`、`NeverDCFGLoopXformTests`、`NeverDTwoTableXformTests`、`NeverDAvxUpperXformTests` | 大きなセマンティック実行形式から分離した高速再リンク用プローブ |

登録の信頼できる情報源は
[`unittests/CMakeLists.txt`](../unittests/CMakeLists.txt)、
[`unittests/lift/CMakeLists.txt`](../unittests/lift/CMakeLists.txt)、
[`unittests/semantic/CMakeLists.txt`](../unittests/semantic/CMakeLists.txt)、
[`unittests/sbf/CMakeLists.txt`](../unittests/sbf/CMakeLists.txt) です。

## fixture の生成方法

### Lift と形式 fixture

`unittests/lift/CMakeLists.txt` はビルド中に C とアセンブリソースをクロス
コンパイルします。Clang の target triple が x86-64、i386、AArch64、ARM32 の
ELF オブジェクト、PE/COFF オブジェクトとリンク済みイメージ、PIC/no-PIC の
Mach-O i386 オブジェクトを生成します。LLD があれば、選択したオブジェクトを
patch テスト用の実行形式にもリンクします。`NeverDLiftTests` は
`lift-test-objects` ターゲットに依存するため、通常のテストバイナリビルドで生成
fixture が更新されます。

多くの lift テストは `NeverDLiftFixture.h` からビルド済み `neverd` CLI を呼び、
LowIR、MedIR、HighIR、LLVM IR、生成 C、書き換えバイナリを検査します。重点的な
手動実験では `NEVERD` 環境変数で CLI パスを上書きできます。通常の CTest は
CMake が埋め込んだ実行ファイルを使います。

### Unicorn 差分ラウンドトリップ

セマンティック fixture はテキストの形ではなく動作を検査します。

1. 小さな C/アセンブリケースを書くか LLVM IR を構築する。
2. Clang/LLVM で指定ターゲット向けにコンパイルする。
3. 元の機械語を Unicorn で実行し、期待する戻り値など fixture 定義の状態を取得する。
4. NeverD で読み込んで lift し、LLVM IR を出力して機械語へ再コンパイルする。
5. 同じ ABI、入力、メモリ配置、CPU モデルで再生成コードを実行する。
6. 観測可能な結果を比較する。

主な実装は
[`SemanticRoundTripFixture.h`](../unittests/semantic/SemanticRoundTripFixture.h)
です。patch-full fixture は patch 操作と同じ rewrite backend である
`Codegen::compileForRewrite` を使い、4×3 の全 ISA/形式グリッドで基準コードと
変換コードを比較します。

決定的な NeverD のセマンティック失敗はテスト失敗にしてください。skip は明示的な
外部能力の境界に限り、その理由を読んでください。クロス linker がない状態の緑色
サマリーは、形式経路の実行を証明しません。

### Solana SBF 差分バックエンド

SBF メタデータテストは、各バージョン機能、オペコード衝突境界、Murmur3 syscall hash、リロケーション、ELF machine、レジスタ、VM アドレス定数を検証します。Loader fixture は vendored バイナリを使わず、従来の v0-v2 section レイアウトと section を持たない厳格な v3/v4 program-header レイアウトの両方を生成します。

`NeverDSBFSemanticTests` は検証済み命令バイトを直接実行し、MedIR を消費しません。このため、正規化 IR の変更や破損によって source oracle と backend が偶然一致することはありません。非単調な v2 セマンティクス、メモリ、syscall、内部 call frame、fault、trace、resource limit を網羅します。LLVM module は検証され、生成 C は warning を error として、Rust は `-D warnings` 付きでコンパイルされます。公開 API テストは生成した厳格な SBF ELF から、全 IR 段階、逆アセンブル、CFG、メタデータ、LLVM、C、Rust を通過します。

## 一括ターゲット

カスタムターゲットは依存関係をビルドし、ホスト CPU から決めた並列度で CTest を
実行します。

| CMake ターゲット | 選択範囲 |
|------------------|----------|
| `check-neverd` | 登録済みの全テスト |
| `check-neverd-semantic` | `NeverDSemanticTests` のみ |
| `check-neverd-sbf` | すべての `NeverDSBF*Tests` ターゲット/ケース |
| `check-neverd-patch-full` | `NeverDPatchFullTests` のみ |
| `check-neverd-switch-xform` | `NeverDSwitchXformTests` のみ |
| `check-neverd-cfgloop-xform` | `NeverDCFGLoopXformTests` のみ |
| `check-neverd-twotable-xform` | `NeverDTwoTableXformTests` のみ |

```bash
cmake --build build-release --target check-neverd
cmake --build build-release --target check-neverd-semantic
cmake --build build-release --target check-neverd-sbf
```

`NeverDIndCallXformTests` と `NeverDAvxUpperXformTests` には現在
`check-neverd-*` の便宜ターゲットがありません。下記のとおりビルドしてラベルを
選択してください。`check-neverd-semantic` にも独立した変換や patch-full の
バイナリは含まれません。完全な集約には `check-neverd` を使います。

## 増分 CTest ワークフロー

所有する実行ファイルを先にビルドしてからラベルを選択します。無関係な大規模
セマンティックターゲットの再リンクを避けられます。

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

# すべての重点 Solana SBF ターゲット/ケース
cmake --build build-release --target \
  NeverDSBFMetadataTests NeverDSBFLoaderTests NeverDSBFAnalyzerTests \
  NeverDSBFSemanticTests NeverDSBFLLVMEmitterTests NeverDSBFEmitterTests \
  NeverDSBFIntegrationTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -R 'SBF' --output-on-failure --parallel 4
```

GoogleTest 由来の CTest 名を使って単一の回帰を実行します。

```bash
ctest --test-dir build-release --build-config Release -N \
  -L '^NeverDLiftTests$'
ctest --test-dir build-release --build-config Release \
  -R '^COFFARMPipeline\.ARM32ThumbLiftAndDecompile$' \
  --output-on-failure
```

便利なセレクター：

| コマンド | 目的 |
|----------|------|
| `ctest --test-dir build-release -N` | 検出ケースを実行せず一覧表示 |
| `ctest --test-dir build-release -L '<regex>'` | テストバイナリのラベルを選択 |
| `ctest --test-dir build-release -R '<regex>'` | ケース名を選択 |
| `ctest --test-dir build-release --output-on-failure` | 失敗時だけ診断を表示 |
| `ctest --test-dir build-release --stop-on-failure` | 最初の失敗で停止 |
| `ctest --test-dir build-release --parallel 4` | 最大 4 ケースを並列実行 |

GoogleTest の検出は `DISCOVERY_MODE PRE_TEST` を使うため、CTest が列挙する前に
対応するテストバイナリが必要です。ケースごとの timeout と独立した検出 timeout は
`cmake/AddNeverD.cmake` に定義され、実測で重いケースがあるスイートだけ拡大できます。

## コード変更に伴うテスト

| 変更領域 | 最初に実行 | 次に検討 |
|----------|------------|----------|
| アーキテクチャ lifter または decode | `NeverDLiftTests` の名前付きケース | 対応 ISA のセマンティックラウンドトリップ |
| LowIR CFG、関数検出、ジャンプテーブル | Lift CFG/switch ケース | `NeverDSwitchXformTests`、`NeverDCFGLoopXformTests`、`NeverDTwoTableXformTests` |
| MedIR、ABI、フラグ、型、SSA | MedIR/呼出規約 lift ケース | ISA 横断の `NeverDSemanticTests` ケース |
| HighIR または構造化 C | HighIR/decompile ケース | `NeverDCFGLoopXformTests` と生成 C のコンパイル検査 |
| PE/ELF/Mach-O loader または入力 relocation | 対応する `unittests/lift` の形式 fixture | そのセルの全段階読込み/デコンパイルテスト |
| Rewrite codegen または出力 relocation | `RewriteCodegenRTTests` ケース | `NeverDPatchFullTests` と利用可能なリンク済み patch fixture |
| patch で使う LLVM IR 変換 | 重点変換バイナリ | `NeverDPatchFullTests` の合成 pass グリッド |
| C API または CLI | 直接 SDK/query テストと `unittests/semantic/CLIEndToEndTests.cpp` | 関連 pipeline/形式スイート |
| SBF loader、ISA、IR、backend | 所有する最小の `NeverDSBF*Tests` ターゲット | 全 SBF ターゲットと生成 C/Rust のコンパイル |
| Libc 認識 | `NeverDLibCTests` | 動作変更時のセマンティック call/ABI ケース |
| プロセス実行または quoting | `NeverDTestProcessTests` | 対応各ホストの影響を受ける CLI/セマンティックケース 1 件 |

テストは最も低い安定した境界で契約を表現してください。LowIR 形状テストは lifter
への帰属に有用です。妥当に見える 2 つの IR 形状が異なる動作をし得る場合は
セマンティックラウンドトリップが必要です。小さな opcode、CFG、観測状態の assertion
で十分なら関数全体の golden dump は避けてください。

## CI との関係

CI は Linux、macOS、Windows でテストを有効にした Release をビルドし、検出した
一覧を監査してからプラットフォーム固有のラベル除外を適用します。プロファイルは
`.github/workflows/ci.yml` と `scripts/audit_ci_test_inventory.py` にあります。
高コストスイートのすべてを表す単一 matrix shard はないため、必要なクロスツールが
揃うマシンではローカルの `check-neverd` が最も明確な完全マージ前シグナルです。
