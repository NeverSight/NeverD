**Languages**: [English](../source-patching.md) | [简体中文](../zh-CN/source-patching.md) | [繁體中文](../zh-TW/source-patching.md) | [日本語](source-patching.md) | [한국어](../ko/source-patching.md) | [Français](../fr/source-patching.md) | [Deutsch](../de/source-patching.md) | [Español](../es/source-patching.md) | [Italiano](../it/source-patching.md) | [Русский](../ru/source-patching.md) | [العربية](../ar/source-patching.md)

<!-- i18n-source: eb2a70d2b1d15c943ee92c3ca762c2328fdf4f9c742fc05a8e52771179931675 -->

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
| Clang 22.1.x | LLVM IR | ELF x64/ARM64 C/C++、Mach-O x64/ARM64 C/C++、PE x86/x64/ARM64 C; PE x64/ARM64 C++ FH3 |
| Clang 22.1.6 | ネイティブオブジェクト | ELF x64 C/C++; PE x86/x64/ARM64 C |
| GCC/G++ 13.3.0 | ネイティブオブジェクト | 互換性のある Itanium ランタイムを使う ELF x64 C/C++ |
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
## 元の参照と記憶領域

束縛は外部宣言に対応します。`function` には確認済みの元関数またはインポートスタブ、`data` には書込み可能領域、`const` には const の IR 宣言が必要です。オブジェクトのデータ束縛には非ゼロの範囲を指定します。領域境界を越えたり RELRO への書込み権限を与えたりできません。置換対象の元本体を呼び戻すための、プロローグを保存するトランポリンはありません。

新しいコードは RX、可変状態は RW に置きます。既存のインポートだけを再利用し、GOT/IAT スロットと呼出可能なスタブを区別します。Mach-O は `-Wl,-headerpad,0x400` などのロードコマンド余白、PE はセクションヘッダー余白が必要です。不足すると失敗します。ELF `.data.rel.ro` と元の CET/IBT 保護には追加の契約が必要なため、現在は拒否します。

```sh
neverd patch app --from-source=replacement.c --entry=replacement --func=0x401140 \
  --bind=function:original_helper=0x401190 \
  --bind=data:shared_counter=0x404020:4 \
  --bind=const:lookup_table=0x402040:64 \
  --patch-report -o app.bound
```

Mach-O のアンワインド表の拡張には、隣接するファイル領域と仮想領域にも余白が必要です。ヘッダー余白だけでは予約できません。検証用入力は両方の要件を個別に満たします。

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
    compiler="gcc", compiler_path="/path/to/gcc", artifact="object",
    bindings={"shared_counter": ("data", original_counter_address, 4)},
)
```

<!-- i18n-section: verification -->
## ビルドと検証

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

ネイティブ検証は NeverC `182fd2c5dc`、独立した元プログラム、現在の NeverD インストーラを使います。`check_source_patch_hosted_pe_runtime.py` は Windows の各アーキテクチャで C IR・オブジェクト・hosted と二層 C フレームの例外伝播を 36 回観測します。`check_source_patch_elf_runtime.py` は Linux の各アーキテクチャで C、hosted 伝播、九つの Clang C++ 例外シナリオを 66 回観測します。証跡、ハッシュ、実際のホスト構成の一致が必須で、CI ホスト上の NeverC 本体実行を意味しません。

旧変換プロファイルの 204 回の観測は履歴であり、現在の C 専用契約の証拠にはなりません。現在の三つの NeverC 経路は C を使用し、Clang C++ FH3 は別途検証します。macOS x64 は Rosetta 2 を使います。Unicorn/QEMU の CPU 結果はネイティブ OS やアンワインダの検証を代替しません。