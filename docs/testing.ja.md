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
| `unittests/evm` | `NeverDEVMOpcodeTests`、`NeverDEVMBytecodeTests`、`NeverDEVMLoaderTests`、`NeverDEVMAnalyzerTests`、`NeverDEVMSemanticTests`、`NeverDEVMEmitterTests`、`NeverDEVMIntegrationTests` | hardfork metadata、input normalization、CFG/SSA/recovery、interpreter semantics、LLVM/C/Solidity differential execution、public API routing |
| `unittests/sbf` | `NeverDSBFMetadataTests`、`NeverDSBFLoaderTests`、`NeverDSBFAnalyzerTests`、`NeverDSBFSemanticTests`、`NeverDSBFLLVMEmitterTests`、`NeverDSBFEmitterTests`、`NeverDSBFIntegrationTests` | v0-v4 メタデータと ELF レイアウト、厳格な検証、CFG/復元、独立した raw 実行、LLVM 検証、C/Rust コンパイル、公開 API ルーティング |
| `PatchFullSubstRTTests.cpp` | `NeverDPatchFullTests` | 4 ISA×3 オブジェクト形式の書き換え/難読化等価性 |
| `unittests/semantic` の重点変換ファイル | `NeverDSwitchXformTests`、`NeverDIndCallXformTests`、`NeverDCFGLoopXformTests`、`NeverDTwoTableXformTests`、`NeverDAvxUpperXformTests` | 大きなセマンティック実行形式から分離した高速再リンク用プローブ |
| `unittests/corpus`（submodule） | `NeverDWindowsEHCorpusTests`、`NeverDRustEHCorpusTests`、`NeverDGoEHCorpusTests`、`NeverDCxxItaniumEHCorpusTests` | pin された 305 個の実バイナリから読み取る例外とランタイム metadata。各バイナリは manifest で復元が満たすべき下限を宣言している |

登録の信頼できる情報源は
[`unittests/CMakeLists.txt`](../unittests/CMakeLists.txt)、
[`unittests/lift/CMakeLists.txt`](../unittests/lift/CMakeLists.txt)、
[`unittests/semantic/CMakeLists.txt`](../unittests/semantic/CMakeLists.txt)、
[`unittests/evm/CMakeLists.txt`](../unittests/evm/CMakeLists.txt)、
[`unittests/sbf/CMakeLists.txt`](../unittests/sbf/CMakeLists.txt) です。

### pin されたバイナリ corpus

他のスイートはテスト対象を自分でビルドしますが、corpus は違います。これは実際の
ツールチェーンが、このリポジトリからは到達できないホスト上で到達できないターゲット
向けに生成したバイナリの submodule であり、各ファイルはダイジェストで pin され、
隣の manifest がその復元の満たすべき下限を宣言しています。「`-O2` で strip された
`armv7` の共有オブジェクトから NeverD が何を読み取れるのか」という問いに、議論では
なく答えを出せる場所はここだけです。

これらのスイートは configure がそれらを探すよう指示されたときにのみビルドされるので、
このフラグがテスト対象であり続けるかどうかのすべてです。

```bash
cmake -S . -B build-corpus -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_BINARY_CORPUS_TESTS=ON
cmake --build build-corpus --target check-neverd-corpus --parallel 4
```

`check-neverd-corpus` は全ラインを、`check-neverd-windows-eh-corpus`、
`check-neverd-rust-eh-corpus`、`check-neverd-go-eh-corpus`、
`check-neverd-cxx-itanium-eh-corpus` はそれぞれ 1 ラインを実行します。CI の 3 ホスト
すべてがこのフラグ付きで configure し、4 ライン全部を実行します。バイトはどこでも
同一ですが、それを読むものは同一ではなく、1 ホストでの corpus 実行は他の 2 ホストに
ついて何も証明しません。`scripts/audit_ci_test_inventory.py` は 4 つの label のどれ
かを欠く inventory を拒否します。corpus を静かに読まなくなったビルドは、どのテスト
にも捕捉できない回帰だからです。消えたものがテストそのものなのです。

EVM opcode audit は実行のたびに公式
[go-ethereum repository](https://github.com/ethereum/go-ethereum) の remote `HEAD` を
shallow `git fetch` し、実際に監査した exact commit を報告します。ignore される bare
cache `build/evm-opcode-audit/go-ethereum.git` を再利用しますが、closed opcode
inventory と byte assignment を読む前に必ず refresh します。

```bash
python3 scripts/audit_evm_opcode_metadata.py
```

CI は同じ live audit を各 push、pull request、manual dispatch、daily schedule で実行し、
NeverD に変更がなくても upstream drift を検出します。offline または historical
reproduction では、既存 checkout を明示的に選択します。

```bash
python3 scripts/audit_evm_opcode_metadata.py \
  --geth-root /path/to/go-ethereum
```

この監査が許可する除外は `EVMUpstreamOpcodePolicy.def` に明記されたものだけです。
表現も明示的な review もない upstream opcode があれば command は失敗します。parser と
drift diagnostic は CI で独立した Python unit coverage を持ち、次で実行できます。

```bash
python3 -m unittest -v scripts.tests.test_audit_evm_opcode_metadata
```

EVM control-flow の変更では、まず fixed-point と height-domain contract を実行します。

```bash
cmake --build build --target NeverDEVMAnalyzerTests --parallel 4
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.StackHeightDomain*:EVMAnalyzer.WholeProgram*'
```

これらの case は block をまたぐ internal return、有限 multi-target merge、loop
convergence と deterministic edge ordering、path-dependent stack height、bounded
widening、correlation による Cartesian over-approximation、unknown jump、exact invalid
target、strict/relaxed の stack fault を網羅します。続けて 7 つすべての EVM binary と
upstream metadata audit を実行してください。CFG の変更は analyzer の局所的な形が正しく
ても emitter と integration に影響し得ます。

MedIR/HighIR dataflow の変更では、constant-phi、selector、typed-operand、
malformed-graph、deep-chain contract も実行します。

```bash
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.MediumIR*:EVMAnalyzer.HighIR*:EVMAnalyzer.*Selector*:EVMAnalyzer.*MedIR*:EVMAnalyzer.RecoversStorageAndEventFactsFromTypedOperands:EVMAnalyzer.RecoversComputedCalldataArgumentOffset:EVMAnalyzer.*Return*:EVMAnalyzer.*Receive*'
```

これらは equal/conflicting cyclic phi、非隣接または block をまたぐ selector expression、
両方の equality operand order、exact ABI width check、typed storage/event/calldata
operand、malformed MedIR の deterministic handling、16,384-value の iterative producer
walk を検証します。

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

### Windows 例外再構築

Windows のテーブルベース例外を変更する場合は、表現テストとリンク済み PE の
patch テストの両方が必要です。対象を絞った lift-suite フィルターは、正規化された
unwind/SEH/C++ モデル、破損入力処理、例外 CFG エッジ、HighIR、LLVM WinEH 生成、
例外ディレクトリの置換、および Guard CF/EH continuation の再構築を網羅します。

```bash
cmake --build build --target NeverDLiftTests --parallel 4
build/bin/NeverDLiftTests \
  --gtest_filter='COFFException*:*PatchCOFF_X64.ReconstructsGuardedSEHAndContinuationTable:*PatchCOFF_X64.ReconstructsNativeFH3StateGraph:*PatchCOFF_X64.RejectsInteriorExceptionDirectoryPadding:*PatchCOFF_X64.RebuildsSortedExceptionDirectoryInAppendedSection'
```

保護された x64 assembly fixture には Clang の Windows target と `lld-link` が必要で、
CMake link は `/guard:cf` と `/guard:ehcont` を使用します。cross-linker 不足による
skip は final-image 経路の証拠にはなりません。統合ケースが成功すれば、書き換えた
PE を再ロードでき、runtime-function、unwind、load-config、Guard CF、Guard EH
continuation の各テーブルがソート済みでファイルに裏付けられ、実行可能 target のみを
指すことを確認できます。

リンク済み FH3 fixture は、固定状態テーブル、HighC 注釈、personality の保持、生成した
catch target、再ロード後の IP-to-state グラフからなるネイティブ C++ closure を独立して
検証します。

解析／ネイティブのサポート表と fail-closed patch 契約については、
[Windows 例外再構築](windows-exception-reconstruction.ja.md)を参照してください。

### 言語例外モデル

Windows テーブルモデル以外のすべては一つの絞り込み target にまとまっています。
`NeverDLanguageEHTests` は DWARF フレームチェーン、Itanium 言語固有データ領域、
ARM EHABI、Darwin compact unwind、Go ランタイムのフレームメタデータ、Rust の
panic 機構、そして三つの Objective-C ランタイムを網羅します。

```bash
cmake --build build --target NeverDLanguageEHTests --parallel 4
build/bin/NeverDLanguageEHTests --gtest_filter='ObjC*'
```

このスイートのテーブルはコンパイルではなくバイト単位で組み立てています。
検証したい組み合わせの多くは、単一の toolchain がまとめて出力することがない
からです。Objective-C が最も分かりやすい例で、三つのランタイムはいずれも
Itanium LSDA を出力し、違いは型テーブルのスロットに何を置くかだけ——しかも
その違いは程度ではなく全面的です。Apple のスロットは `objc_typeinfo` を指し、
その最初の二つのフィールドは意図的に `std::type_info` を模しています。GNUstep
の Objective-C++ スロットは本物の `std::type_info` 派生型を指し、GNU ランタイム
のスロットはそもそもポインタですらなくクラス名の文字列そのものです。あるランタイム
の規約を別のランタイムのテーブルに当てはめても失敗はせず、まったく別のものの
途中から読み取ったクラス名を報告するだけです。だからスロットを読む前に、フレーム
の personality からランタイムを確定します。

同じスイートは、まとめてしまいがちで、まとめると誤りになる二つの区別も固定します。
`@catch(id)` と `@catch(...)` は別のハンドラで——前者は任意の Objective-C
オブジェクトを受け取り、外来例外はその横を通過させます——各ランタイムで綴りが
異なります。両方を catch-all として報告するデコーダは、本来素通りするはずの例外に
ハンドラを付けてしまいます。また setjmp/longjmp の call-site テーブルはアドレスでは
なく呼び出し位置の索引を並べるため、SJLJ personality を認識しそこねた読み取り側は
エラーにならず、プログラムが指定していない保護範囲と landing pad を捏造します。

その形式を認識することと、解読を拒むことは別です。SJLJ の 1 エントリは ULEB128 の
組——ディスパッチ用のセレクタと action オフセット——であり、この action オフセットの
意味はアドレス形式のそれと完全に同じです。したがって action チェーンも catch の型も
例外仕様も、コードを一切名指ししない表から読み出せます。読み出せないのは各エントリが
守る範囲だけで、それを語るのは関数自身が call-site スロットへ行う書き込みであって、
表の中の何かではありません。本スイートはここで信用してはならない 1 バイトも固定します。
call-site エンコーディングとして GCC は `DW_EH_PE_uleb128` を、LLVM は
`DW_EH_PE_udata4` を書きますが、どちらもその後 ULEB128 を出力し、どの personality も
それを読みません——ならばデコーダも読んではなりません。

personality の同定も併せて固定します。上のあらゆる表をどう読むかを決めるのがそれだから
です。GNAT は GCC が各フロントエンドに与える 3 通りの綴り——`_v0`、`_sj0`、`_seh0`——で
自らのルーチンを名づけ、Windows では一方のシンボルを登録して他方へ転送するので、4 つの
綴りすべてが Ada に行き着かねばなりません。D はその鏡像で、3 つのコンパイラ、1 つの
ルーチンに対する 3 つの名前、その背後にあるのは同一の表です。

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

### EVM 差分バックエンド

interpreter test は deterministic 256-bit oracle です。emitter suite は LLVM を
compile/execute し、C23 を Clang で同じ host harness に lower し、`solc`、`anvil`、
`cast`、`jq` があれば generated Solidity を local node に deploy します。status、
storage、instruction trace count を比較します。別の raw-bytecode corpus は Anvil native
EVM 上で pre-Fusaka ALU、calldata/memory copy、overlapping `MCOPY`、Keccak、return
data を実行します。

`NeverDEVMOpcodeTests` は metadata architecture も強制します。150 opcode の
encoding/typed-value roundtrip、family boundary、hardfork alias、derived stack/host
maxima を検証します。

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

# すべての重点 EVM ターゲット/ケース
cmake --build build-release --target \
  NeverDEVMOpcodeTests NeverDEVMBytecodeTests NeverDEVMLoaderTests \
  NeverDEVMAnalyzerTests NeverDEVMSemanticTests NeverDEVMEmitterTests \
  NeverDEVMIntegrationTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -R 'EVM' --output-on-failure --parallel 4

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
| EVM loader、opcode、IR、backend | 所有する最小の `NeverDEVM*Tests` ターゲット | 全 EVM ターゲットと生成 C/Solidity のコンパイル |
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

## 現在の Solana SBF conformance / sanitizer profile

この current list は上の短い SBF list を置き換えます。source differential suite は
clang に加えて `rustc` が必要で、compiler skip は coverage 欠落です。完全な aggregate
には `NeverDSBFProgramImageTests`、`NeverDSBFMalformedCorpusTests`、
`NeverDSBFISAConformanceTests`、`NeverDSBFUpstreamConformanceTests`、
`NeverDSBFLLVMDifferentialTests`、`NeverDSBFSourceDifferentialTests` と、metadata、
loader、analyzer、semantic、emitter、integration target が含まれます。integrated
profile は 14 binary の 145/145 case を通過します。

sanitizer profile は `build-sbf-asan-ubsan` に分離して build します。13 core binary
の 141/141 case が ASan/UBSan report なしで通過します。prebuilt package に必要な
fork-only header がないため、integration は integrated LLVM build で実行します。

```bash
cmake --build build-sbf-asan-ubsan --parallel 4 --target \
  NeverDSBFMetadataTests NeverDSBFProgramImageTests NeverDSBFLoaderTests \
  NeverDSBFAnalyzerTests NeverDSBFISAConformanceTests \
  NeverDSBFSemanticTests NeverDSBFEmitterTests NeverDSBFLLVMEmitterTests \
  NeverDSBFLLVMDifferentialTests NeverDSBFSourceDifferentialTests \
  NeverDSBFMalformedCorpusTests NeverDSBFUpstreamConformanceTests \
  NeverDSBFSolanaModelTests

ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:strict_string_checks=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
NEVERD_SBPF_ROOT=/path/to/sbpf \
ctest --test-dir build-sbf-asan-ubsan --output-on-failure --parallel 4 \
  -L '^NeverDSBF' -E 'SBFIntegration'
```
