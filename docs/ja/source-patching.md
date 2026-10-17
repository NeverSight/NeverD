**Languages**: [English](../source-patching.md) | [简体中文](../zh-CN/source-patching.md) | [繁體中文](../zh-TW/source-patching.md) | [日本語](source-patching.md) | [한국어](../ko/source-patching.md) | [Français](../fr/source-patching.md) | [Deutsch](../de/source-patching.md) | [Español](../es/source-patching.md) | [Italiano](../it/source-patching.md) | [Русский](../ru/source-patching.md) | [العربية](../ar/source-patching.md)

<!-- i18n-source: 1eae101089e730c29ba3fda92db515fe148fe41b758d6f5b69156252591d9b3f -->

[← Documentation Index](README.md)

# ソースとネイティブオブジェクトのパッチ

NeverD は指定したコンパイラで置換関数をコンパイルし、明示した元の関数入口にインストールします。C/C++ の言語意味論はコンパイラが担当し、アドレスの束縛、再配置、記憶領域、アンワインド情報、公開は NeverD が担当します。バイナリ修復とリフト後の書き換えはそれぞれのパイプラインを使い、プログラムの残りは再ビルドしません。

<!-- i18n-section: compiler -->
## コンパイラの選択

`--compiler=clang`、`--compiler=gcc`、`--compiler=neverc` を選択します。Clang と通常モードの NeverC は LLVM IR を生成できます。`object` は生成された機械命令を保持します。CLI は GCC に対してオブジェクトを既定で選び、Python SDK も GCC にはオブジェクトを既定で選びます。他のコンパイラへの暗黙のフォールバックはありません。

インクルードパス、マクロ定義、対応するソースオプションには `--compiler-arg` または SDK の `compiler_args` を使います。ターゲットや出力モードの上書きとレスポンスファイルのトークン（`@file`）は、独立したオプションの値を含め、どの引数位置でも拒否されます。通常の `-I` パスと `-D` 定義は引き続き使用できます。

例のアドレスは確認済みの元関数入口に置き換えてください。置換関数の宣言は呼出規約と構造体配置を含む元の機械 ABI と一致する必要があり、アドレスだけでは証明できません。C++ は `--language=c++` と適切な Clang++/G++ を使います。入口は `extern "C"` にするか、正確な修飾名を指定してください。補助関数は追加できます。`--entry` と `--func` で置換対象を明示し、従来の `--from-c` も指定アドレスを尊重します。

`artifact="object"` を明示することもできます。

```sh
# Clang C -> LLVM IR
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=clang --compiler-path=/path/to/clang \
  --patch-report -o app.clang

# NeverC ordinary C -> native object
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=neverc --compiler-path=/path/to/neverc \
  --artifact=object --patch-report -o app.neverc

# GCC -> native object (CLI default for GCC)
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=gcc --compiler-path=/path/to/gcc \
  --patch-report -o app.gcc

# Independently compiled ELF x64 object
clang --target=x86_64-unknown-linux-gnu -fPIC -fno-lto -c replacement.c -o replacement.o
neverd patch app --from-object=replacement.o --entry=replacement \
  --func=0x401140 --patch-report -o app.object
```

<!-- i18n-section: profiles -->
## 検証済みプロファイルと制限

[NeverC producer: 182fd2c5dc](https://github.com/NeverSight/NeverC/commit/182fd2c5dc)

**NeverC C と hosted DynCode**

NeverC は x64/ARM64 の ELF、PE、Mach-O で純粋な C ソースだけを受け付けます。NeverD は NeverC と `--language=c++` の組み合わせを起動前に拒否し、暗黙の変換やコンパイラ変更を行いません。C++ と例外の置換には Clang、または検証済み ELF オブジェクト構成に適合する GCC を使います。hosted C フレームは実際のアンワインド記録により、周囲の C++ プログラムが送出・捕捉する例外を通過させます。NeverC 自体の C++ 対応ではありません。

`dyncode`: `--compile-mode=dyncode` は NeverC の `-fdyncode-hosted` を使い、ソース、一つの明示入口、ネイティブオブジェクト、既存シンボルへのバインドなしを要求します。CLI と Python はこのモードと GCC にオブジェクトを既定で選択します。両成果物のハッシュ、リンク済みコード全バイト、入口を検証し、実際の CFI/SEH または完全な native compact 記録を登録します。可変/TLS 状態、暗黙の syscall/PEB/heap/string 変換、未解決参照、コード再配置は拒否します。データやコールバックは引数で渡せます。

```sh
neverd patch app --from-source=replacement.cpp --language=c++ \
  --compiler=clang --compiler-path=/path/to/clang++ \
  --entry=replacement --func=0x401140 --patch-report -o app.cpp
neverd patch app --from-source=replacement.c --compiler=neverc \
  --compiler-path=/path/to/neverc --compile-mode=dyncode \
  --entry=replacement --func=0x401140 --patch-report -o app.dyncode
```

Clang LLVM IR は Windows ARM64 C++ FH3 も受け付け、xdata、親フレーム ABI、funclet/FuncInfo の所有関係を確認します。オブジェクトの言語ハンドラは対象外です。`check_source_patch_cpp_runtime.py` は x64/ARM64 のネイティブ Windows で九つの例外シナリオを実行し、CPU と OS 展開器の証拠を区別します。

| コンパイラ | 成果物 | OS 上で検証した実行プロファイル |
|---|---|---|
| Clang 22.1.x | LLVM IR | ELF x64/ARM64 C/C++、Mach-O x64/ARM64 C/C++、PE x86/x64/ARM64 C; PE x64/ARM64 C++ FH3; ELF x86 C (`compatibility`, `qemu-user`); ELF ARM32 C (ARM/Thumb, `compatibility`, `qemu-user`) |
| Clang 22.1.6 | ネイティブオブジェクト | ELF x64/ARM64 C/C++; PE x86/x64/ARM64 C; Mach-O x64/ARM64 C; ELF x86 C (`compatibility`, `qemu-user`); ELF ARM32 C (ARM/Thumb, `compatibility`, `qemu-user`) |
| GCC/G++ 13.3.0 | ネイティブオブジェクト | 互換性のある Itanium ランタイムを使う ELF x64/ARM64 C/C++; ELF x86 C (`compatibility`, `qemu-user`); ELF ARM32 C (ARM/Thumb, `compatibility`, `qemu-user`) |
| NeverC 3389.1.5 通常モード | LLVM IR | ELF x64 C、Mach-O x64/ARM64 C |
| NeverC 3389.1.5 通常モード | ネイティブオブジェクト | ELF x64 C |
| NeverC 3389.1.5 C | LLVM IR / ネイティブオブジェクト | ELF / PE / Mach-O x64/ARM64: 純粋な C ソース |
| NeverC 3389.1.5 `hosted-v1` | ネイティブオブジェクト + 封印したペイロード | ELF / PE / Mach-O x64/ARM64: C ソースと C フレームを通る例外伝播 |

上表は検証済み OS 実行構成です。すべてのコンパイラ版やオプションを保証するものではありません。ELF、PE、Mach-O のネイティブインストーラは四つのアーキテクチャを受け付けます。GCC は一致する ELF ドライバが必要で、LLVM IR は生成できません。未知の再配置、TLS、ロード時のグローバルコンストラクタ、LTO、common/IFUNC シンボル、未解決インポートは明示的に失敗します。JSON 証跡はコンパイラ、引数、バインド、設置アドレスを記録し、ネイティブオブジェクトには SHA-256 も記録します。

ネイティブオブジェクトの証跡にある `target` は、読み込んだイメージの ABI に基づきます。ARM ELF の浮動小数点フラグと iOS/watchOS の区別も含まれます。コンパイル済みオブジェクトと外部オブジェクトには同じ識別規則を適用します。

CPU 行列は十二の組み合わせすべてでソース、外部 IR、ネイティブオブジェクトの設置成功を要求します。ARMv7k watchOS と ARMv7 iOS、可変／読み取り専用状態、元の浮動小数点・スタック引数バインド、ロードアドレス変更後の入口実行も検証します。CPU 実行は OS ローダー、C++ アンワインダ、任意の ABI を保証しません。上表の OS 構成を参照してください。

| Format | `x86` | `x64` | `ARM32` | `ARM64` |
|---|---|---|---|---|
| `ELF` | `CPU` | `CPU` | `CPU` | `CPU` |
| `PE` | `CPU` | `CPU` | `CPU` | `CPU` |
| `Mach-O` | `CPU` | `CPU` | `CPU` | `CPU` |

`CPU` はソース／IR／オブジェクトの設置、再ロード、正確な関数実行を意味します。ARM ELF はイメージから EABI5、ARM/Thumb 状態、浮動小数点 ABI を選択し、完全な整列済み `EHABI` 索引を登録します。PE は x64/ARM 実行時行と `HIGHLOW`/`DIR64`/`MOV32T` ベース再配置を保持し、x86 はコンパイラ登録コードを使います。Mach-O は CPU サブタイプに従い watchOS DWARF と iOS `SjLj` を区別します。ソースの ARM/Thumb 入口は相対分岐で、再ベースに耐えます。

ネイティブオブジェクトは自身の境界付き関数シンボル、補助／セクション定義、アンワインド記録で範囲を検証します。MC 所有者 ID の捏造や IR 再コンパイルは行いません。COFF の関数範囲には正確な補助サイズ、単一関数セクション（`-ffunction-sections`）、またはデコード済みのネイティブ実行時関数記録が必要です。次のシンボルから長さを推測しません。watchOS アダプタは実際の記録を得るため葉関数フレームを保持します。ネイティブ COFF/Mach-O 言語ハンドラは検証済みオブジェクト構成外です。C++ 例外には検証済み IR 経路を使ってください。固定 exidx レジストリを持つ静的 ARM アンワインダ、未登録 Mach-O 絶対ポインタ、未対応 RELRO は拒否します。

GCC C オブジェクトも ELF x86、ARM32、ARM64 に設置し、Linux QEMU user mode で実行しました。アダプタはイメージから ARM/Thumb を選択し、GNU property は ELF ポインタ幅で整列します。

<!-- i18n-section: references -->
## 元の実装へのコールバック、配置拡張と依存関係

バインディングは外部宣言を指定します。`function` は元の関数またはインポートスタブを認証し、`data` は書き込み可能な領域、`const` は const IR 宣言を要求します。オブジェクトのデータにはゼロでない範囲が必要で、領域境界を越えたり RELRO への書き込みを許可したりできません。置換する関数の元の本体を呼ぶには `original` を明示します。公開入口は置換関数を呼び、保存された入口は完全な命令単位で再配置した先頭部分を実行して元の本体へ続きます。ソース、外部 IR、ネイティブオブジェクトは、浮動小数点・スタック引数や複数コールバックを含め同じ契約を使います。

コールバックには認証済みの関数範囲、命令モード、継続先、アンワインド証拠が必要です。未知の命令、安全でない PC 相対参照、コピー領域内への分岐、未対応のローダー再配置、重複するコールバック、未モデル化のハンドラー付きプロローグは公開前に拒否されます。ABI の一致する宣言は呼び出し側の責任です。レシートの `original_callbacks` に保存入口を記録します。

PE32/PE32+ はファイル内領域を移動し、モデル化されたデバッグ、シンボル、証明書、オーバーレイのポインターを更新して、満杯のセクションヘッダー表を拡張できます。元の RVA は維持します。Mach-O x64/ARM64 PIE は、隣接する page-zero 保護領域と末尾の linkedit セグメントが条件を満たせば、検査済みのヘッダー先頭拡張からコマンドとアンワインド領域を確保し、元のコード・データのアドレスを維持します。このプロファイルは `-Wl,-headerpad,0x400` や隣接する未使用アンワインドページを必要としません。明示的なヘッダーアドレス参照、未知のロードコマンド、コードとデータの混在所有、壊れた範囲、未対応配置は拒否されます。ヘッダーとアンワインド容量はインストール前にまとめて計画します。

新しいコードは RX、可変状態は RW です。明示的な `--dependency` は ELF 動的メタデータ、PE インポート記述子／IAT、Mach-O dylib／bind メタデータを通じて関数・データを追加します。既存インポートの識別、序数、バージョン記録を維持し、スロットと呼び出しスタブを区別します。対象 OS ローダーに適したライブラリ名・パスを指定してください。下の例は ELF 共有ライブラリ名を使います。ライブラリやシンボルの欠落は実行時の実際のローダーエラーで確認します。TLS、IFUNC、新規シンボルバージョン契約、ロード時コンストラクター、未モデル化の再配置は未対応です。ELF `.data.rel.ro` と CET/IBT にも追加契約が必要です。 ネイティブ COFF オブジェクトでは、新しい関数・データのインポートを `__declspec(dllimport)` で宣言し、再配置が IAT を指定する必要があります。通常の直接外部参照は拒否されます。

```sh
# ELF example: the original target is at 0x401140.
neverd patch app --from-source=replacement.c --entry=replacement --func=0x401140 \
  --bind=original:saved_target=0x401140 \
  --bind=function:original_helper=0x401190 \
  --bind=data:shared_counter=0x404020:4 \
  --bind=const:lookup_table=0x402040:64 \
  --dependency=function:extra_function=libpatch_extra.so \
  --dependency=data:extra_state=libpatch_extra.so \
  --patch-report -o app.bound
```

構造体サイズで検査する追記式の依存関係オプションは、ソース、外部 IR、ネイティブオブジェクトに共通です。レシートは `added_imports` を含み、候補を再ロードして全コールバック、インポート、領域権限、インストール済みバイトを再検査します。失敗時は入力と既存の出力を保持します。

<!-- i18n-section: examples -->
## 実行可能なサンプル

[Windows/macOS/Linux の完全なサンプル](../../examples/source-patch/README.md) には元プログラム、置換関数、依存ライブラリのソース、ビルドコマンドと実際のホストで動く Python SDK プラグインが含まれます。Python を有効にした NeverD ビルドを使用し、リポジトリのルートから新しい出力ディレクトリを指定して実行します。

```sh
# Linux / macOS
python3 examples/source-patch/run.py --neverd build-example-host/bin/neverd --clang clang --output build-example-native
```

```powershell
# Windows: Visual Studio developer PowerShell
python examples/source-patch/run.py --neverd build-example-host/bin/neverd.exe --clang clang --output build-example-native
```

既定では両方の経路を実行します。`--api cli` または `--api sdk` で一方を選べます。対象アドレスは自動検出されます。入力 `7` に対し、元プログラムは `8`、両方のパッチは `36` を出力します。ドライバーは実行、レシート、ライブラリ欠落時のロード失敗を検証し、完全なビルドと patch コマンドを `commands.json` に保存します。

<!-- i18n-section: exceptions -->
## C++ 例外

ELF はコンパイラの FDE/LSDA、Mach-O は compact unwind/DWARF を使います。PE x64/ARM64 はコンパイラの FH3 テーブルを受け入れ、元プログラムに互換性のある MSVC ランタイムと型識別が必要です。type-info データは明示的な束縛を要する場合があります。FH4、独自 personality、SEH ソース、有効な CFG/EHCont/XFG は拒否し、元の保護フラグを保持します。

IBT を要求する ELF x86/x64 元プログラムは現在拒否します。ソースインストーラは ENDBR を保持するトランポリンをまだ提供しません。

実行ケースは通常の戻り、内部 catch、新しい例外を元の呼出元で捕捉、元の呼出先から置換関数を通る例外、RAII の一度だけの解放、再送出、noexcept 終了、未変更の関数を検証します。元の呼出箇所は既に例外伝播を許容している必要があります。元コンパイラが非送出関数の周囲から削除した catch はパッチで復元できません。

<!-- i18n-section: publication -->
## SDK と公開

追加型 C API は `neverd_patch_source` と `neverd_source_patch_options_v1` です。`neverd_patch_source_report` は記録を返します。オブジェクト入力ではバイト長を渡します。従来の `neverd_patch_from_c` も使用できます。

入力をスナップショットし、出力先の隣に候補を作り、再ロードしてすべての入口を確認してから排他的に公開します。既存ファイルやシンボリックリンクの別名は上書きしません。一つでも対応が欠ければ全体が失敗します。`--mode=inplace` は拒否します。セッションの IR 変換はネイティブオブジェクトには適用できません。Mach-O は署名を厳密に検証して再署名し、PE の署名を黙って削除しません。

```python
report = session.patch_source(
    replacement_source, "app.patched",
    mappings={"replacement": original_address},
    compiler="clang", compiler_path="/path/to/clang", artifact="object",
    bindings={
        "saved_target": ("original", original_address),
        "shared_counter": ("data", original_counter_address, 4),
    },
    dependencies={
        "extra_function": ("libpatch_extra.so", "function"),
        "extra_state": ("libpatch_extra.so", "data"),
    },
)
```

<!-- i18n-section: verification -->
## ビルドと検証

選択した六つのネイティブ OS／ISA ジョブは失敗・スキップゼロで合格しました。GoogleTest 3669 回、ネイティブプロセス観測 1008 回（機能 504 回、C++ 例外 504 回）、実ローダー拒否 12 回、96 イメージ、84 公開レシートです。

| ホスト | ソース | Harness | CI ジョブ | テスト | 観測／拒否 |
|---|---|---|---|---|---|
| Linux x64 | [61fb1d3e759](https://github.com/NeverSight/NeverD/commit/61fb1d3e759f6796d23341782e7f4d624295f27c) | [197126208b6](https://github.com/gmh5225/test_mac_intel/commit/197126208b665bec0db5c6dc74278931300229af) | [116845056048](https://github.com/gmh5225/test_mac_intel/actions/runs/38861651227/job/116845056048) | 626 | 168 / 2 |
| Linux ARM64 | [61fb1d3e759](https://github.com/NeverSight/NeverD/commit/61fb1d3e759f6796d23341782e7f4d624295f27c) | [197126208b6](https://github.com/gmh5225/test_mac_intel/commit/197126208b665bec0db5c6dc74278931300229af) | [116845055863](https://github.com/gmh5225/test_mac_intel/actions/runs/38861651227/job/116845055863) | 600 | 168 / 2 |
| macOS Intel | [b7a180c7ac3](https://github.com/NeverSight/NeverD/commit/b7a180c7ac35fc2f05be3568a86104d42fc32800) | [cba0906a114](https://github.com/gmh5225/test_mac_intel/commit/cba0906a11461d6e29bbe68f9337515fe084b610) | [117185095063](https://github.com/gmh5225/test_mac_intel/actions/runs/38963782346/job/117185095063) | 624 | 168 / 2 |
| macOS ARM64 | [61fb1d3e759](https://github.com/NeverSight/NeverD/commit/61fb1d3e759f6796d23341782e7f4d624295f27c) | [f6311a611e0](https://github.com/gmh5225/test_mac_intel/commit/f6311a611e095bcffb7537c7ae3b40c826d1e2e1) | [116879179194](https://github.com/gmh5225/test_mac_intel/actions/runs/38872352550/job/116879179194) | 624 | 168 / 2 |
| Windows x64 | [76ce83e138d](https://github.com/NeverSight/NeverD/commit/76ce83e138de639b8d75206c78bd5ccbb9cfd8dd) | [a42b117681f](https://github.com/gmh5225/test_mac_intel/commit/a42b117681f81d922e09d98ccb1a1c4525055947) | [116908799030](https://github.com/gmh5225/test_mac_intel/actions/runs/38881414688/job/116908799030) | 598 | 168 / 2 |
| Windows ARM64 | [76ce83e138d](https://github.com/NeverSight/NeverD/commit/76ce83e138de639b8d75206c78bd5ccbb9cfd8dd) | [cba0906a114](https://github.com/gmh5225/test_mac_intel/commit/cba0906a11461d6e29bbe68f9337515fe084b610) | [117014878954](https://github.com/gmh5225/test_mac_intel/actions/runs/38913475295/job/117014878954) | 597 | 168 / 2 |

各成果物が検証するのは下表の正確なソースと harness コミットです。後続の dev 統合と文書コミットは別のローカル・文書チェックで扱います。過去の失敗・キャンセルは失敗のまま保持します。過去の補足観測216回、PE ARM32、旧32ビット Apple の CPU／互換層の結果は、新機能のネイティブハードウェアや C++ 例外の証明にはなりません。

Windows ARM64 の検証では、固定されたサブモジュールと検証済み LLVM-MinGW ツールチェーンからネイティブ ARM64 Unicorn C DLL を実際にビルドし、MSVC インポートライブラリと明示的な CMake ターゲット上書きを使います。隔離したビルド用コピーは、非 SEH ジャンプ関数の組、命令キャッシュ同期を伴う ARM64 ページコミットコールバック、Windows LLP64 用の64ビット即値マスクという三つのホスト修正を認証します。サブモジュールのソースとリビジョンは変更しません。これは CPU 行列のテストホストを検証するもので、標準の MSVC ARM64 JIT 対応を示すものではありません。五つのホスト復帰チェックは、パッチ済みプロセスのネイティブ実行や C++ 例外の証拠と分けて記録します。

`NEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE` で互換性のあるコンパイラを指定します（Clang 22.1.6 を検証済み）。統合ビルドはビルドディレクトリ内の LLVM コピーだけを修正し、サブモジュールや共有プリビルドキャッシュは変更しません。r3/r4 の x64 PLT 不具合により IR パッチが拒否される場合があります。ネイティブオブジェクトはこの MC 経路を通りません。`NEVERD_TEST_REQUIRE_FIXED_LLVM=1` は修正済みの正確な PLT 生成を必須にします。

`NeverDSourcePatchTests` はネイティブ C/API/オブジェクト/例外を、`NeverDSourcePatchMatrixTests` は Unicorn ですべての組合せを検証します。`NEVERD_TEST_WINDOWS_CRT_LIB`、`NEVERD_TEST_WINDOWS_UCRT_LIB`、`NEVERD_TEST_WINDOWS_SDK_LIB` を設定すると `SourcePatchPE` が 7 つの C++ シナリオを実行します。Windows 以外では `NEVERD_TEST_WINDOWS_RUNNER` も必要です。`NEVERD_TEST_REQUIRE_WINDOWS_RUNTIME=1` は入力不足を失敗とします。専用 Windows ワークフローはネイティブ実行を要求し、スキップを許容しません。

開発 Mac の Linux 検証は Docker と明示的な Linux ローダー、PE 検証は Wine 11 を使います。Mach-O ARM64 は Mac 本体で実行します。Windows ネイティブ CI の結果は別途確認が必要です。ジョブの設定は成功した実行の証拠にはなりません。履歴、ベースラインの失敗、エミュレーターの制限は[詳細契約と検証記録](../source-patching-details.md)を参照してください。

```sh
cmake -S . -B build-source -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON -DNEVERD_LLVM_PREBUILT=OFF \
  -DNEVERD_ENABLE_SEMANTIC_TESTS=ON \
  -DNEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE=/path/to/clang
cmake --build build-source --target NeverDSourcePatchTests \
  NeverDSourcePatchMatrixTests --parallel 4
ctest --test-dir build-source -L \
  '^(NeverDSourcePatchTests|NeverDSourcePatchMatrixTests)$' \
  --output-on-failure --parallel 4
```

LLVM のビルド用コピーは Mach-O x64 の GOT データ参照も修正します。Clang の型付き catch 表は 4 バイト再配置フィールドの末尾を基準にします。r3/r4 は 4 バイトずれるため、公開前検証で未修正の結果を拒否します。開発 Mac の x64 Mach-O ケースは Rosetta 2、ARM64 ケースは直接実行します。

Windows x64 のネイティブ検証 [2ed7e78fc](https://github.com/NeverSight/NeverD/actions/runs/38307461805) では、PE/オブジェクト/trampoline の専用テスト 16 件と CPU ターゲット行列テスト 143 件が、スキップなしですべて成功しました。FH3 のテストは C++ の実行シナリオ 7 種類を検証し、ターゲット行列は Unicorn を使用します。Clang 22.1.6、ネイティブの MSVC と Windows SDK、統合 LLVM を使用しました。

Windows ワークフローは x86、x64、ARM64 向けに 27 個の PE 置換バイナリも生成します。`scripts/check_source_patch_pe_runtime.py` はソース IR、コンパイル済みオブジェクト、外部オブジェクトの各経路で、非リーフ呼び出し、変更可能なポインター状態、浮動小数点・スタック引数を確認します。元のプログラムと各置換結果をそれぞれ 3 回実行し、アーキテクチャごとに 30 回のプロセス実行を観測します。x86/x64 は Windows x64、ARM64 は `windows-11-arm` で実行します。検証にはネイティブホストの種類、コミット、ファイルダイジェストの一致と全ケースの存在が必要です。外部ランナーの結果は別に記録します。このプロファイルは ARM64 C++ 例外のサポートを証明しません。

行列のオプションのコンパイラーは `NEVERD_TEST_MATRIX_NEVERC` で選択します。NeverC 3389.1.5 は x64/ARM64 と ELF、PE、Mach-O の全 66 チェックに成功し、IR・ネイティブオブジェクトのインストールと CPU 実行を確認しました。このバージョンは x86 と ARM32 に対応しないため、これらには Clang を使用し、ELF では対応する GCC ドライバーも使用できます。NeverC の PE コンパイルではアダプターが `-fno-autolink` を追加し、別の CRT をリンクせず既存イメージ内で参照を解決します。`.drectve` リンカー契約を含む外部オブジェクトは引き続き拒否します。

Windows のネイティブプロセス検証 [a4a5ae3cb](https://github.com/NeverSight/NeverD/actions/runs/38350079817) では、ソース IR、コンパイル済みオブジェクト、外部オブジェクトの各経路を含む全 90 回の観測が成功しました。x86、x64、ARM64 はそれぞれ 30 回で、x86/x64 は Windows x64、ARM64 は同じハッシュ検証済みイメージを使って `windows-11-arm` で実行しました。同じ実行で PE/オブジェクト/trampoline の専用テスト 16 件と Unicorn ターゲット行列テスト 143 件も、スキップなしですべて成功しました。FH3 のテストでは x64 C++ の実行シナリオ 7 種類も引き続き検証しました。

ネイティブ検証は NeverC `182fd2c5dc`、独立した元プログラム、現在の NeverD インストーラを使います。`check_source_patch_hosted_pe_runtime.py` は Windows の各アーキテクチャで C IR・オブジェクト・hosted と二層 C フレームの例外伝播を 36 回観測します。`check_source_patch_elf_runtime.py` は Linux の各アーキテクチャで C、hosted 伝播、九つの Clang C++ 例外シナリオを 120 回観測します。証跡、ハッシュ、実際のホスト構成の一致が必須で、CI ホスト上の NeverC 本体実行を意味しません。 Linux の x64 と ARM64 では Clang IR、Clang ネイティブオブジェクト、GCC ネイティブオブジェクトそれぞれの九つの例外シナリオを検証します。NeverC は C 専用です。

旧変換プロファイルの 204 回の観測は履歴であり、現在の C 専用契約の証拠にはなりません。現在の三つの NeverC 経路は C を使用し、Clang C++ FH3 は別途検証します。開発用の Apple Silicon Mac では、macOS x64 は Rosetta 2 を使います。Unicorn/QEMU の CPU 結果はネイティブ OS やアンワインダの検証を代替しません。

AArch64 の命令再配置には明示的な RELA 加数が必要です。暗黙の REL 命令加数は公開前に拒否され、命令ビットフィールドを通常のデータとして解釈しません。対応する PREL64 データ再配置は、REL と RELA の両方で完全な符号付き加数を保持します。

C 専用の生成元検証 [cc91afb3b](https://github.com/NeverSight/NeverD/actions/runs/38591930213) は 312 回のネイティブプロセス観測すべてに合格しました。Windows x64 と ARM64 は各 36 回、Linux x64 と ARM64 は各 120 回です。Linux は Clang IR、Clang ネイティブオブジェクト、GCC ネイティブオブジェクトの九つの例外シナリオを個別に検証します。NeverC の IR、オブジェクト、hosted DynCode 入力はすべて C です。

インストーラを再ビルドした検証 [cc91afb3b](https://github.com/NeverSight/NeverD/actions/runs/38591928600) は、19 件の PE/オブジェクト検査、143 件すべての CPU マトリックス、Windows/Linux の 474 回のネイティブプロセス観測に合格しました。Windows x86 プロセスは Windows x64 ホストで、x64 と ARM64 は対応するホストで実行しました。別の [Linux 検証](https://github.com/NeverSight/NeverD/actions/runs/38585759996) は元の 108 件と追加の LLVM-C 三件、Clang/GCC C++ 三件すべてに合格し、失敗もスキップもありません。

[統合検証 0a6f6d768](https://github.com/NeverSight/NeverD/actions/runs/38627694097) は、Windows/Linux の x64 と ARM64 ネイティブ環境で 312 回すべてのプロセス観測を再検証しました。最終的な 30 イメージは以前に検証したバイト列と一致し、ソースのハッシュと C 専用の出力レシートを更新して独立に確認しました。

[0a6f6d768 の拡張 Linux 検証](https://github.com/NeverSight/NeverD/actions/runs/38626866257) は、元の 108 件、Swift 全体 55 件、CPU ターゲット 143 件、ネイティブセッション 117 件、LLVM-C 3 件、Clang/GCC C++ 3 件の計 429 件を、失敗・スキップなしで実行しました。この統合版ではローカルの全バイナリパッチ回帰 22,961 件もすべて成功しました。ネイティブプロセス数と CPU エミュレーションの検証範囲は区別して記録します。

[0a6f6d768 の Windows インストーラー再ビルド検証](https://github.com/NeverSight/NeverD/actions/runs/38627581732)では、182 件の重点チェックと 474 回のネイティブプロセス実行がすべて成功し、失敗もスキップもありませんでした。独立した成果物監査で、各イメージのハッシュ、アーキテクチャ、シナリオ、反復回数を照合しました。その後の ABI/アンワインド修正を統合した `e1cac949f` でも、完全なバイナリパッチスイート 22,961 件が再度すべて成功し、失敗もスキップもありませんでした。NeverC は純粋な C のみを扱い、C++ の置換には Clang/GCC を使用します。

<!-- i18n-section: runtime-coverage -->
## macOS と 32 ビットプロセスの検証

プロセスイメージのワークフローは、macOS ARM64（`macos-15`）と Intel x64（`macos-15-intel`）でのネイティブ実行を必須とします。各アーキテクチャで 96 回の観測を行い、Clang C のソース／オブジェクト導入、書き込み可能なポインタ状態、浮動小数点／スタック引数、9 種の Clang C++ 例外シナリオ、および既存 NeverC の純粋な C の IR／オブジェクト／hosted 出力を検証します。実行前に Mach-O 署名を厳密に検査し、導入した hosted コードと生成ペイロードの全バイト一致を要求します。

同じワークフローで、Windows x64 上の Windows x86 を 30 回、Linux x64 上の Linux x86 を 48 回、QEMU ユーザーモードの Linux ARM32 を 96 回観測します。ARM32 は ARM と Thumb が半数ずつで、Linux では対応する GCC オブジェクトも検証します。各シナリオを 3 回実行します。報告では `native`、`compatibility`、`rosetta`、`qemu-user` を区別し、対象と実際のホストのアーキテクチャを記録します。Rosetta は Intel ネイティブ実行の要件を満たしません。

全 14 CPU 対象プロファイルは、引き続き 283 件のソース／IR／オブジェクト行列で検証します。PE ARM32、Mach-O i386、ARMv7 iOS、ARMv7k watchOS は現在 CPU 検証であり、ネイティブ OS ローダーや C++ アンワインダーの保証ではありません。NeverC は x64／ARM64 の純粋な C 専用のままです。32 ビットの生成には Clang と対応する ELF GCC ドライバーを使用します。検証器は NeverC をビルドも変更もしません。

Windows x64 と ARM64 でも、それぞれ 36 回の Clang C++ FH3 観測を実行します。

Mach-O の書き込み可能ポインタは実行時に初期化します。静的なアドレス初期化は未対応のロード時再配置登録を必要とするため、明示的な拒否テストで検証します。ELF のサイズゼロ COMDAT 関数は、署名が実行可能セクションの先頭にある唯一の関数で、CFI がセクション全体と厳密に一致する場合のみ受け入れます。内部オフセット、共有、末尾パディング、グループなし、CFI 欠落は拒否します。

ELF ARM と Thumb は別プロファイルです。ソース生成器へ `-marm` または `-mthumb` を明示し、ネイティブ関数のバインドは元の命令状態ビットを保持します。32 ビット ELF は実際の動的再配置を持つ PIE を使用します。ARM32 QEMU では glibc の明示的な `--loader` と sysroot を記録します。QEMU の AUXV 計算は、移動したプログラムヘッダーを Linux カーネルと同じ方法では扱わないためです。この経路は QEMU 上の glibc ロードと実行の検証であり、ARM32 実機やネイティブカーネルローダーの証明ではありません。

これらのパッチ適用済み ELF イメージでは、Linux x86 と ARM32 の `qemu-user` 経路はいずれも、対象アーキテクチャに対応する sysroot 内の glibc `--loader` を明示的に指定する必要があります。Linux x86 の `compatibility` 経路は Linux x64 ホストカーネルの 32 ビット互換機能を使用し、QEMU 実行とは別に記録されます。これはネイティブの 32 ビット x86 ホスト上での実行を証明するものではありません。

[`fcee81c9a5e2f20a71090ade23ae833175f64c59` のプロセス検証](https://github.com/NeverSight/NeverD/actions/runs/38694434082)では、対応するネイティブホストで 576 回、x86 互換モードで 78 回、QEMU ユーザーモードの ARM／Thumb で 96 回、計 750 回の観測すべてが成功しました。独立した成果物監査で全 126 イメージのハッシュ、実行環境、シナリオ一覧を照合しました。[Windows 再ビルド検証](https://github.com/NeverSight/NeverD/actions/runs/38694628242)は重点チェック 194 件と Windows／Linux のプロセス観測 474 回に成功しました。[Linux 再ビルド検証](https://github.com/NeverSight/NeverD/actions/runs/38694638358)は、元の失敗 62 件とスキップ 46 件をすべて含む 444 件に、失敗・スキップなしで成功しました。上記のハードウェアとローダーの検証範囲は変わりません。NeverC は純粋な C 専用のままで、リポジトリは変更していません。

追加の [ARM/Thumb 検証](https://github.com/gmh5225/test_mac_intel/actions/runs/38750504263)では、`ubuntu-24.04-arm` 上で `fcee81c9a5e2f20a71090ade23ae833175f64c59` の C テスト用イメージ 32 個を実行し、全 96 回の観測（ARM と Thumb 各 48 回）が成功しました。Linux カーネルが `/lib/ld-linux-armhf.so.3` を通じて各イメージを直接ロードし、QEMU や明示的なローダーコマンドは使用していません。これは ARM64 ホスト上の `compatibility` であり、ネイティブ 32 ビットホストの検証ではありません。テストハーネスのコミット `3ad17ac9608d6bd9f5a4d44953ec375524e5730b` とイメージのコミットは別々に記録されます。独立した成果物監査でイメージのハッシュ、全 30 件の公開レシート、全シナリオと反復回数、および一致する `binfmt_misc` インタープリタールールがないことを確認しました。

別の[ホスト型 runner 調査](https://github.com/gmh5225/test_mac_intel/actions/runs/38749138719)では、提供された Windows 11 ARM64 runner が ARM32 プログラムの起動を `WinError 216` で拒否しました。macOS 15.7.9 と Xcode 16.4 の環境では、付属 SDK で macOS i386 または iOS ARMv7 プログラムをリンクできませんでした。watchOS ARMv7k プログラムのリンクは成功しましたが、実機での実行証拠は得られていません。調査ジョブの成功は各プラットフォームの検証完了を意味せず、ネイティブハードウェアの検証は未完了です。NeverC のビルドや変更は行っていません。

追加の[個人 Actions の Linux 検証](https://github.com/gmh5225/test_mac_intel/actions/runs/38773830274)では、32 ビット Linux カーネルと glibc を備えた完全な QEMU TCG ARM32 ゲスト内で ARM/Thumb の 96 回の実行が成功し、同じゲストの Wine で PE ARM32 の 30 回の実行も成功しました。ELF イメージはゲストカーネルが直接ロードします。これはシステム全体のエミュレーションであり、明示的なローダーを使う `qemu-user` とは区別します。ARM64 ホストの互換レイヤーで行う Wine レジストリのインストールは別記録で、パッチ実行回数には含めません。ゲスト内の Wine 初期化と各測定実行は、当初の制限時間内に成功する必要があります。

[Apple 互換性検証](https://github.com/gmh5225/test_mac_intel/actions/runs/38771214358)では、Linux 上の Darling で Mach-O i386 の 30 回、Intel macOS 上の touchHLE で ARMv7/ARMv7k の 60 回の実行が成功しました。原本と、ソース IR・コンパイル済みオブジェクト・外部オブジェクトによる各置換を、それぞれ 3 回実行します。独立監査では、追加実行全体の 72 イメージ、66 件のパッチ公開記録、216 回の実行に加え、GitHub 成果物のサイズ、SHA256、正確なコミット、完全なケース一覧を確認しました。Linux 入力は引き続き `fcee81c9a5` に固定し、PE 入力は `a542f97ba`、Mach-O 入力は `8f5f06762` で生成しています。検証スクリプトのコミットはリンク先の実行に別途記録しています。

Mach-O の修正は、ARM/Thumb 混在というイメージ全体の情報とは独立して選択した Thumb 関数を認証し、証拠のある Thumb エクスポートポインターと ARM32 アンワインドのコードアドレスを正規化します。データポインターや範囲長は変更しません。統合 LLVM ソースビルドでは、ローカルの CPU マトリクス 283 件と例外契約 258 件がすべて成功し、失敗もスキップもありませんでした。旧 Mach-O プローブには記録済みのオープンソースリンカーとアンワインド領域の予約を使用し、i386 には正しく整列した起動入口と 64 KiB のページゼロ領域を用意します。

これらの結果が示すのは、指定したエミュレーターと互換レイヤーでの C コード実行です。Wine は Microsoft Windows ではなく、Darling は macOS ではありません。iOS HLE 内での ARMv7k 実行は watchOS、WatchKit、Apple 実機の検証にはなりません。旧 Apple SDK やネイティブ 32 ビットハードウェアの対応を推定せず、C++ 例外アンワインドの検証にも数えません。既存のネイティブ実行と互換モードの証拠は別に保持します。NeverC はビルドも変更もしていません。
