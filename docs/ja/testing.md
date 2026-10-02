**Languages**: [English](../testing.md) | [简体中文](../zh-CN/testing.md) | [繁體中文](../zh-TW/testing.md) | [日本語](testing.md) | [한국어](../ko/testing.md) | [Français](../fr/testing.md) | [Deutsch](../de/testing.md) | [Español](../es/testing.md) | [Italiano](../it/testing.md) | [Русский](../ru/testing.md) | [العربية](../ar/testing.md)

[← ドキュメント索引](README.md)

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
[CONTRIBUTING.md](CONTRIBUTING.md)を参照してください。

## インタープリター復元の検査

```sh
cmake --build build-release --target NeverDInterpreterSpecializationTests \
  NeverDDevirtualizationSourceTests --parallel 4
ctest --test-dir build-release -L '^NeverD(InterpreterSpecialization|DevirtualizationSource)Tests$' \
  --output-on-failure
```

```sh
cmake --build build-release --target NeverDLowIRUndefinedIndependenceTests \
  NeverDOriginalBinaryUndefinedIndependenceTests \
  NeverDX86UndefinedEffectsTests NeverDX86CarryArithmeticFlagTests \
  NeverDX86LogicIdentityTests NeverDX86NoIndexAddressTests --parallel 4
build-release/bin/NeverDLowIRUndefinedIndependenceTests
build-release/bin/NeverDOriginalBinaryUndefinedIndependenceTests \
  --gtest_filter='OriginalBinaryUndefinedIndependence.*'
build-release/bin/NeverDX86UndefinedEffectsTests
build-release/bin/NeverDX86CarryArithmeticFlagTests
build-release/bin/NeverDX86LogicIdentityTests
build-release/bin/NeverDX86NoIndexAddressTests
```

復元 API テストは v1/v2/v3 の既定値、明示予算、切り詰めた構造体、全 reserved フィールド、将来の末尾を検査します。CLI テストは両 ABI と両バックエンドでフィールド／問い合わせ予算超過と復元成功を確認し、不正な十進上限と `--devirtualize` の欠如を拒否します。予算超過時はソースも部分残余グラフも公開しません。

v4 テストはプレフィックスのサイズとパディング、切り詰めと不明フラグの拒否、旧 API/将来拡張との互換性、レポートなしでも C に残る符号付き範囲を検査します。独立した CLI 例では相関保持に連鎖が必要で、符号なしスタック比較の証明には範囲が必要です。両 C バックエンドを O0/O2 と未定義動作トラップで実行します。探索の無効化は探索依存例の結果を変える必要があります。解析テストはゼロ連鎖、整数極値、オーバーフロー、不正範囲と前提不足を網羅し、Python はレイアウト、フラグ、署名、所有権付き失敗レポートを検査します。

`NeverDByteMemoryForwardingTests` は重なる最後の書き込み、両バイト順、i128 までのバイト単位幅、定義済み値、相関を保つ undef/poison スナップショット、部分上書きを検証します。不明なエイリアス、アドレス空間変換、呼び出し、順序付きアクセス、寿命変更、欠落バイト、動的・無効オフセット、分岐、ループではロードを保持します。多数の入力を持つ PHI、ゼロ・厳密・不足予算、既定のスナップショット拒否も確認します。元の LLVM と変換後の LLVM を O0/O2 で独立した算術基準と比較し、既存の MBA・LLVMC・インタープリターソースのテストで互換性を確認します。

`NeverDMedMutableSourceTests` と `NeverDLLVMCValueTests` は、独立作成したループ、ブロック順序変更、入口への後退辺、実行時スタック演算、過去の読み取り、分岐合流、部分エイリアス、論理真値、ゼロを含むビットカウントを O0/O2 で実行します。負のテストでは不正な入力、切り詰められた分岐先、曖昧な値の受け渡し、予算超過を生成前に拒否します。SSA 上限を超える CLI ケースは実行可能な LLVMC 出力と HighC の明示的拒否を確認します。 連続更新とブロックをまたぐ格納式の連鎖についても、生成 C のサイズと実行結果を検証します。

複合条件の回帰テストは O0/O2 で、非ゼロ定数との等価比較、符号なし比較、両方のオペランド順の符号付き比較、拡張された真偽値、すべての否定の組合せによる論理積と論理和を実行します。C 出力は真理値表全体を保持し、存在しないゼロ比較オペランドを参照してはいけません。整数としてのアドレス格納は整列・非整列の 32/64/128 ビット型を検証します。バイト配列は明示的なアラインメントと先頭・部分アクセスを正確に保持し、配列へのスカラー代入や互換性のない型の別名参照を生成しません。

`NeverDLowIRRefinementTests` は実際の復元グラフ、構造の異なる有限ループ、ゼロ回反復、動的生成箇所、条件付き選択、重複入力ビュー、コピーとスピルの相関、両側の不変読み取り証拠、システムフラグと戻りスロット保存を検証します。誤った候補、余分な書き込み、不完全または無限の経路、古い証拠、一時領域の衝突、共有予算超過では証明書を拒否します。既存の独立性テストは観測可能な任意値を引き続き拒否します。

同じターゲットの `LowIRLoopRefinement.*` と `BinaryLowIRLoopRefinement.*` は、任意の64ビット回数、入れ子の辞書式順位、実際のネイティブ残余コード、入口接頭区間のテンプレート、重なるビューと相関した退避を検査します。誤った本体、入口領域の縮小、減少しない順位、符号なし回り込み、過去の書き込みの欠落、切断点の欠落、不正なテンプレート、共有予算の枯渇を負例で拒否します。有限の兄弟経路が成功しても不完全な帰納証明は承認されません。

`LowIRLoopInference.*` と `BinaryLowIRLoopInference.*` は独立に作成したカウンター、スタック保存、早期リターン、ネイティブ呼び出し、パック済みフラグを使用します。狭い幅の算術拡大と、式は異なるが意味が等しいフラグを検証します。不正なグラフ、欠落／偽造した起源、非停止やラップするループ、推論／証明予算の枯渇で証明書を生成してはいけません。

共有ヘッダーと共有ラッチの回帰検査は、ゼロ拡張した 32 ビットおよび全幅の 64 ビットカウンタ、非単位ステップのスカラー順位、誤った結果、進行しない経路とラップアラウンド、スカラーからタプルへの探索で累積される正確な予算と予算枯渇を対象とします。`LowIRLoopInference.SharedHeaderAndLatchNeedLexicographicRanks`。 追加の加算・リセット回帰検査は、各反復でカウンタを1ビットずつ展開せずに収束することを要求し、進行の欠落と符号なしラップアラウンドを拒否します。 スケジューリング回帰は、非単位ステップの累積変数、有効な非単位スカラー順位に隣接するラップ可能な単位カウンタ、および成功タプルが早期枠の後に現れる3個のカウンタを対象とします。ちょうど十分な順位予算と1回不足の予算で、候補を繰り返さず決定的に探索を再開することを確認します。

同じターゲットの `LowIRLoopPlanPairing.*` は、レジスター名の変更、異なる算術本体、各側の前置スナップショット、述語の保持、共有フレーム入力、入れ子のカット点網羅性、独立した証明予算を検査します。関係の欠落、誤った書き込み、不正な一時値束縛、不完全な対応付け、メタデータ上限の超過から証明書を生成してはいけません。

`LowIRLoopAlignment.*` は独立に作成した通常形と回転形のフレームカウンタループを検査します。既定の自己関係計画はそれぞれ証明できますが、最初の対応付けは失敗し、別の候補カットで関係が証明されます。複数カットの順列、誤った結果とフレーム書き込み、元の記録の欠落・陳腐化、明示的な未定義値 witness、非減少・桁あふれカウンタ、不正なグラフ、失敗試行の累積クエリ、ちょうど足りる総予算、探索限界の枯渇を網羅します。拒否結果に証明書があってはなりません。 追加ケースは、分離したリセットと進行段階、終了判定を移動して候補群間の対応付けが必要になる等価ループ、再推論しないキャッシュ、合計メタデータ超過を検査します。後続の独立ループは明示的な16384クエリ上限で全循環の被覆を確認します。空・重複候補群は記号クエリを使わず、カット不足は拒否します。ちょうど十分および一回不足の全体予算、誤った結果、進行の欠如、元の証拠、未定義値の witness も検査します。 フィルターの回帰は結果を変えない算術ダイヤモンド、局所合流と境界だけの合流、到達可能な合流点を迂回して終了または循環境界に戻る経路を検査します。元側の重複候補群が候補側の試行を妨げないこと、先に成功したフィルター計画を後続の全分岐試行が再利用すること、厳密・一単位不足・ゼロの `MaxCutSelectionWork`、失敗した `CutSelectionWork` の累積、全体のグラフ作業枠消費後に記号推論を始めないことを確認します。両分岐候補群の完全循環被覆を検査し、ダイヤモンドの関係には明示的な推論・証明クエリ上限を使います。

部分カウンターの回帰はフレーム・レジスター、増減両方向、下位・中間・上位、不規則な幅、両エンディアン、3バイトのフレーム語を扱います。遅れて現れるレーン、保持ビットの改変、進行しないループと無保護の周回、無効な追加入口、正確／不足の推論予算、既存の単一カット探索を検査します。

先頭フェーズの回帰は、同じカウントダウン語を再利用する二つ・三つの順次ループ、既存の入れ子ループフェーズとの合成、厳密および一回不足の順位・クエリ予算、進行しないループ、前のフェーズへ戻るリセットを検査します。同じ幅の誤ったフェーズ定数、誤った結果とフレーム書込みは完全チェッカーが証明書なしで拒否し、元の証拠が欠ける場合も未対応となります。

`NeverDLowIRRefinementTests` の `InterpreterMachineStateModel.*` は独立した LowIR 例で、生の入口フラグ、ステータスとゲスト RAX の分離、全 17 状態ワード、部分レジスタ、フラグのパック、動的拒否状態の保持、ゲストフレーム書込み、両分岐、循環推論後の新しい証明を検査します。誤った出力、消失したステータス、メモリ変更、古い命令記録、不正入力、生成予算の枯渇は失敗しなければなりません。既存のマシンソーステストは両 C 経路を O0/O2 で実行しますが、モデルテストだけではコンパイル済み C を証明しません。

`NeverDLLVMInterpreterModelTests` は独自 LLVM を全状態 LowIR 参照実装と比較し、ビット幅、並列 PHI、switch、ゲストメモリ、独立ステータス、poison ガード、組み込み関数の値域、拒否契約、四つの構築予算を検証します。任意ワードのカウントダウンを完全に証明し、変更されたステータスを拒否します。独自 C の O1/O2 コンパイル結果も同じ観測契約を満たす必要があります。これは対応モデルの検証であり、自動不変条件発見とコンパイラーの正しさは別の義務です。 可変シフトのケースは四つのビット幅、マスクや分岐で制限したシフト量、境界値と範囲外の値、オーバーフロー禁止と正確性フラグ、厳格な poison 拒否、O1/O2 でコンパイルした C を検証します。

初期化契約の回帰テストは、部分・分離バイト範囲、固定別名、分岐の両側、各リターン、ループ初回の読み取り、ループ内の書き込み後の読み取りを検証します。先行読み取り、書き込み不足、ゲスト書き込み、未知の別名、特殊メモリアクセス、オブジェクト外範囲、入力／作業予算の枯渇は失敗しなければなりません。出力専用状態ワードを持つ独立した C 例を O1/O2 でコンパイルし、正確な LLVM 属性を保持したまま新たなネイティブから LLVM への合成証明を検証します。

ガード付きカウントダウンの検証は、本体テンプレート拒否後の再試行、任意ワード入力に対する完全なヘッダー証明、共有カット・問い合わせ予算、実入口契約違反の即時拒否を含みます。

`NeverDInterpreterLLVMRefinementTests` は新規の合成証明、正確なテキスト／関数の結合、独立予算、全観測項目、広いソース領域を検証します。バイト、残余、結果、フラグ、ステータス、フレーム書込み、poison、誤った／古いループ案の変更は合成証明を拒否させます。任意ワード幅のカウントダウンは両帰納前提を要求し、独立 C 例の O1/O2 コンパイルは実際の LLVM テキストを検証します。状態モデルの回帰は隠れた入口後退辺を拒否し、付随する出自情報をコピーせずルートを予算計上します。

```sh
cmake --build build-release --target NeverDLLVMInterpreterModelTests --parallel 4
build-release/bin/NeverDLLVMInterpreterModelTests
cmake --build build-release --target NeverDInterpreterLLVMRefinementTests --parallel 4
build-release/bin/NeverDInterpreterLLVMRefinementTests
```

二段・三段のループのキャッシュされた等値終了条件では、オペランドの相関、変化する境界、カウンターのリセット、破損したコピーを検査します。

比較キャッシュの回帰は等値・不等値、入口ガードと定数畳み込み、拡幅後に初めて変化するフィールド、1/4/8 バイトのキャッシュのビット 7/31/63 を検証します。対象ビットを保ったまま隣接ビットだけを変更しても全状態比較で拒否します。ゼロ増分、移動する境界、カウンタのリセット、共通予算の枯渇も拒否します。

汎化前置状態の回帰テストは、合流入口、最初のゼロ回反復、隠れたレジスタ／フレーム差異、非正規ブール述語、ネイティブトラップ条件、関連するスピル、不正な計画と予算枯渇を扱います。独立した二重／三重の等値終了カウンタとネイティブバイトで、符号なし境界、ゼロ／最大入力、非単位ステップ、誤った元命令を検査します。推論と最終証明は不完全な結果を拒否します。

独立した分岐ループの回帰テストは両方の分岐方向、誤った本体、停止しない兄弟分岐、共有の探索／証明予算の超過を検査します。 `LowIRLoopInference.AlternativeLoopsReachBothPrefixesWithinSharedBudgets`.

入れ子推論の回帰は二重・三重ループ、増加・減少カウンタ、自動段階定数、実ネイティブループ本体の切断点を検査します。到達不能または互いに排他的な接頭辞領域、誤った本体、無限・折り返し遷移、共有探索／証明予算の枯渇は拒否されます。接頭辞の証拠は完全な区間網羅の代わりにはなりません。

```sh
cmake --build build-release --target NeverDLowIRRefinementTests --parallel 4
build-release/bin/NeverDLowIRRefinementTests
```

`NeverDLowIRUndefinedIndependenceTests` は、完全な非巡回 LowIR グラフについて二つの実行の独立性を検証します。通常の入口入力は共有し、アーキテクチャ上未定義の値を新たに生成するたびに、コピー、重複書き込み、スピルと再ロードを通じた相関を保持します。制御条件は経路の仮定を追加する前に検査します。証明書には `Complete` の効果メタデータと、各命令の完全な境界および正確な操作ダイジェストとの対応が必要です。証拠不足、到達可能なループ、呼び出し、未知のエイリアス、予算切れでは証明書を拒否します。結果は明示的な観測対象と障害を起こさないフレームの契約に限られ、ネイティブコードから C への完全な等価性証明ではありません。

以下は既定の厳密な監査契約での動作です。`NeverDOriginalBinaryUndefinedIndependenceTests` は独立に作成した固定マッピングの x64 バイト列で、物理的な CALL/RET、変更された戻り先、有限な間接ターゲットの網羅、メモリからの不変値の読み込みを検査します。同じターゲットが直接分岐の完全収集、バイト列・効果・マッピング・読み込み証拠への正確な結合、外側リターン時の入口 RSP と戻りスロットの保持、フレームとイメージの分離条件を検査します。命令の欠落・重複、厳密なトラップ規則と明示的なプロファイル投影規則に該当しない未監査の分岐、停止しない、または予算を超えるループ、不完全なターゲット列挙、設定・契約の不一致、予算切れでは証明書も残余コードも返しません。成功には全実行可能経路の完了が必要です。この任意ゲートはループ不変条件、例外ディスパッチ、CET 有効実行、ネイティブコードから C への等価性を認証せず、通常の復元は別に利用できます。さらに、厳密にリフトされた `INT3`/`UD2` の終端境界と、全バイト列および操作ダイジェストへの結合を検査します。未定義出力サイドカーの `Missing` はそのまま保持します。証明書に含められるのはシンボリック実行で到達不能と証明されたトラップのみで、実行可能なトラップ到達経路は `ContractViolation` となり、証明書も残余コードも返してはいけません。トラップ後の継続や例外からの復帰はモデル化せず、`codeFollowsTrap` は使わず、静的 LowIR API の対応範囲も変更しません。

明示的なネイティブ重複テストは、実際の x64 即値内への分岐、両方の実行可能な分岐結果、先行命令内部への間接リターン入口を検査します。合成プロバイダーでは、両収集順序の包含関係、未実行の直接分岐上のバイト矛盾、両順序のコードと読み取りの整合性、候補側の読み取りを検査します。ちょうど足りる予算と不足する予算は、間接転送をまたいで重複バイトも計上します。結果の変更、静的またはループ API での使用、矛盾した証拠は証明書を拒否し、オプションや上限の変更はダイジェストを変えます。

明示的な境界テストは、到達不能な RCL、メモリ XADD、REP MOVS、記号的な経路矛盾、任意値に依存する分岐と、入口・間接分岐・CALL・RET からの到達時の正確な拒否を検査します。到達可能な後続部分への独立した入口、候補とネイティブのアドレス重複、不正または部分的な証拠、資源切れ、静的/ループ API の拒否、精緻化ダイジェストの三層も対象です。到達不能命令の変更や、境界がない場合のオプション変更も証明書ダイジェストを変更します。これらは宣言された有限証明の範囲を検査し、未監査命令の意味を証明するものではありません。

パック済みフラグのテストは、全スカラー入口フラグの組み合わせ、特権マスク、両実行の TF/AC 条件、異なる未定義値の生成、相関するコピー、ネイティブ呼び出し、兄弟経路の状態、最終システム状態の必須観測、不正な証拠、資源上限を検証します。有限ループは全実行可能入力経路が終了する必要があり、安全な分岐で無限経路や打ち切り経路を隠せません。RDSSPD/RDSSPQ は 16 汎用レジスタと両幅、上位ビット保持、`Missing` 証拠の保持、偽造投影の拒否を検証します。機械状態テストは両 C 経路の O0/O2 と未定義動作トラップを用い、独立したユーザーモードのフラグオラクルと比較し、プロファイル違反が後から消えないことも確認します。 INCSSPD/INCSSPQ は両幅と全汎用レジスタ、到達不能境界の保持、安全な兄弟経路完了後の実行可能なトラップ、ゼロオペランド、偽造したトラップ証拠を検証します。

`NeverDX86UndefinedEffectsTests` は未定義ビットのメタデータ、定義済み／保持されるフラグ、古い証明書の拒否を検査します。`NeverDX86CarryArithmeticFlagTests` は算術オラクルにより、レジスター形式とメモリ形式の ADC/SBB の補助キャリーを検査します。`NeverDX86LogicIdentityTests` は、同一オペランドの AND が 64 ビットモードで 32 ビットの宛先に書き込む際、対応する 64 ビットレジスターのビット 63:32 をゼロにし、狭い書き込みでは未書き込みのビットを保持することを検査します。

`X86RotateUndefinedEffects.*` は全生カウント、オペランド幅、CL の重なり、上位バイトの別名、メモリ宛先をスカラー算術の参照実装と比較します。`X86BitTestUndefinedEffects.*` はレジスタ/即値インデックス、ソースと宛先の重なり、拡張レジスタ、定義済みフラグ、上位レジスタへの書き込みを検査します。メタデータの反例は変更されたオペランド、符号化、未対応形式を拒否します。ネイティブ証明は相関する読み取りと独立した任意フラグを区別し、生成数予算の境界と不足を検査し、観測可能な未定義 OF を拒否します。全状態の精緻化は選択した証人を受け入れ、ゼロビット証人や変更した候補を拒否します。

`X86XaddAudit.*` は符号なし算術オラクルを用いて全 65,536 組のバイト入力、広い幅のフラグ境界、レジスタ/上位バイトの重なり、両書き戻し、REX のバイト幅制限、レジスタ全体の保持を検査します。ネイティブ検査は以前の依存関係を保持しつつ新たな任意ビットがゼロであることを要求します。両証人は未変更の XADD を受理し、和、交換元、定義済みフラグの改変を拒否します。`/6` 別名は完全なシフトカウント行列で検査し、グループ/デコード ID の変更による意味の取り違えを拒否します。

`NeverDPEFixedImageTests` は独立に構成した PE ファイルで、再配置付き命令と不変データ、インポート書き込み範囲、不正なヘッダー／表、別名、来歴の改変を検証します。ネイティブから LowIR および正確な LLVM への証明は一致する候補を受理し、結果、ステータス、元のバイトが変わった候補を拒否します。準備予算の超過は独立に分類し、明示的に上限を増やして再試行できます。通常のロードでは、既定の解析予算を超える 40000 件の有効な再配置も受理します。

`FrameOffsets.*`、`NativeStackSpecialization.*`、`OriginalBinaryUndefinedIndependence.*` は整列 2/4/8/16/32 の全剰余、自由な上位ビット、呼び出しをまたぐスピル、カウントダウンループ、エイリアス破損、誤分岐、無関係な大マスク、必要な分割拡大、ちょうど／一回不足の予算を検査します。別のネイティブ対照例は経路条件付き整列、内部の符号なし戻り解放、誤解放量、接頭辞付き戻りを検査します。分割ループの自動 native-to-LLVM 証明範囲を確立するテストではありません。

レジスターケースの回帰は両エンディアン、上位ビットが異なるフレーム基点、上書きと重複フィールド、後続辺、拡大された先行ノード、ネイティブ CALL/RET、ちょうど十分な予算と一つ不足する予算を扱います。両 C 経路は O0/O2 で四つのメモリーケースを実行します。ネイティブ精密化は二つの選択値を個別に固定する検査で、無制約入力の証明ではありません。独立 LLVM 例は、共有合流点の前へ移動した偽の分岐が真の分岐の後に実行されないことを、PHI コピーとストアを含めて検査します。

入口整列テストは細分化、全許可剰余、異なる上位ビット、最後のケースの失敗、正確な予算と 1 不足を扱います。両 C バックエンドを O0/O2 で実行し、拒否対象のアクセス不能なゲストアドレスと不正フラグでもステータス 2 と全状態バイトの保持を確認します。モデル、C/Python の v5 配置と所有権、旧版と将来の末尾も検証し、未対応の整列領域でネイティブ証明が出ないことを確認します。

`StringTransfer.*` と反復コピーの回帰テストは、重なり、回数ゼロ、一時変数の分離、容量・予算制限、ポインターの無効化を検査します。`MachineStringSourceTests.cpp` は四つの幅と両方向について、ネイティブ実行と両 C 経路の O0/O2 結果を、全レジスター・フラグ・スタックの独立した参照結果と比較します。

`ControlDiscovery.*` と `NativeStackSpecialization.*` は、ルートの下位ビット条件、上位ビットとルート全体への依存、不完全な走査、ちょうど十分な走査予算と不足する予算、および有限の不変アドレス証拠の保持を検証します。

`NativeStackSpecialization.NarrowAddressDemandRetainsCompletePointer` は、条件付きのレジスタ・フレームスロットのポインタ合流、両バイト順、剰余演算の折り返しと高位ルートアドレス、スタック復元、120フレームバイトを検査します。関連する反例は破損ポインタを拒否し、問い合わせ・操作・評価・細分化の予算がちょうど足りる場合と1回不足する場合、および依存探索とコンテキストの上限超過を検査します。

有限値の観測テストは早期拒否、定数、空の射影、最後の UNSAT クエリを対象とします。不変読み取りの回帰テストは、反例の後も実行時ロードを保持し、異なる読み取り範囲についてキャッシュ済み領域を再検証し、不正な証明書では部分的な証拠を公開しません。

アフィン制御のテストでは、8 回の求解予算での下位ビット条件、両方のバイト順でのレジスタとフレームスロット、剰余演算の折り返し、フレームのバイト観測、スタックの復元を検査します。リテラル定数ではない矛盾した条件は未対応の分岐を除外し、下位 32 ビットが同じで上位ビットが異なる二つの根値は両方の間接分岐先を保持する必要があります。

`FiniteQueryCache.*` は、必要量ちょうどと一単位不足の記憶量上限、ヒットによる使用順更新、サイズの異なる複数記録の置換、繰り返しの追い出し、変数名を変更したクエリを検証します。重複保存、ミス、不正な結果、過大な候補は使用順を変えません。返された証明のコピーは、対応するキャッシュ項目を追い出した後も有効です。

`ControlStateRecovery.*Marginal*` は、独立したターゲット値域と業務値域の積が結合上限を超える場合、残余コードの具体的な出力、結合関係の拡大後に遅い先行ノードが追加するターゲット、到達可能なターゲットの欠落、上限超過や不完全な列挙による値域全体の破棄を検証します。独自のフィクスチャで、値域の変化による再解析と、失敗時に部分グラフを公開しないことを確認します。 フレームスロットの変種では、結合関係の拡大後に両バイト順でエイリアスによる無効化を検証します。

`InterpreterTransferChain.*` は相関値、一意・動的分岐、遅れて到達する先行ノード、フレーム境界、エイリアス拒否、予算を検査します。追加のネイティブ CALL/RET テストは命令の反復、戻り先スロットのバイト、スタック復元を検査します。生成元の再生とネイティブから LLVM への検証は契約不一致、証拠の変更、誤った結果、スタック書き込みの欠落を扱います。

`NeverDX86NoIndexAddressTests` は、32／64 ビットのアドレス幅でインデックスを持たない x86 SIB アドレッシングを検証します。無視されるスケールビット、宛先幅、ロード／ストア、完全な未定義出力メタデータ、セグメントオフセット、アドレスの由来を対象とします。疑似レジスタをベースや幅の異なるインデックスとして使う場合は拒否し、REX.X が選択する実際の R12 インデックスは保持します。EVEX ブロードキャストとマスク付き移動のテストも、これらの形式、非アクティブなメモリアクセスの抑制、矛盾する SIB メタデータを検証します。

シフト回帰は全 8 ビット回数、ゼロ回数のフラグ組合せ、両 x86 モード、全スカラー幅、CL と宛先の重複、AH/CH/DH/BH、拡張レジスタ、メモリを網羅します。バイト単位のシンボリック実行を反復 1 ビット算術と比較し、定義済み結果とガードを検証します。関係テストはコピーと新規フラグ、スピル、ループ再訪、未定義値由来の回数、分岐拒否、不正形式、ダイジェストと予算を検証します。有限不変読み取りは 1/2/4/8 バイト、入力依存選択、パスごとの単一候補、全証拠、上限の結び付けと、依存・欠落・書き込み可能・未格納・再配置・無限候補の拒否を検証します。

コアテストはコンテキスト分離、不動点への合流、動的ループ、重複レジスタ、エイリアス無効化、有限ターゲット、拒否時に部分的な置換を出さないことを確認します。ソーステストは独自のレジスタ型、スタック型、有限アドレス型の x64 マシンを組み立て、両 C 経路を復元して、未定義動作トラップを有効にした O0/O2 でコンパイルし、独立した符号なし算術とメモリの参照実装と実行を比較します。有限アドレス fixture は入力で選ぶレコードとカーソル／キーの相関を検査し、ネイティブ検査は SysV/Win64 を対象にします。公開 CLI、復元予算、未対応入力のレポートも検査します。クロスターゲット Clang と LLD が必要で、元の ELF の実行には x64 Linux ホストも必要です。ツール不足やホスト不一致でのスキップは未実行の範囲であり、成功ではありません。

`ControlStateRecovery.LongTransparentLoop*` は、独立に作成した 20 段階のループ、動的算術の参照実装、未知のセレクターの拒否、予算切れを検証します。`LongTransparentPhasesKeepExactBitDemands` は、セレクターと同じバイト内の無関係なビットが、制御要求にならず観測可能な実行時データとして残ることを確認します。`ProducerClosureChargesWorkBeforeAnotherRestart` は、新しいグラフを開始する前の逆向き検出と再実行にも共通予算を課し、部分結果を公開しないことを検証します。

`X86ShiftCarry.*` は 1 ビットずつのシフトを基準に、狭い整数の算術右シフトのキャリー、マスク後の回数、APX の出力レジスターとフラグ抑制を検査します。
`NarrowArithmeticShiftCarrySurvivesBothSourceBackends` は未定義動作のトラップを有効にし、O0/O2 で両方の復元 C 経路を実行し、すべてのバイト値と生のシフト回数を網羅します。
`NeverDLLVMCIntrinsicSemanticTests` は i1/8/16/32/64/128 の符号付き・符号なし整数 min/max も O0/O2 で実行し、代入とインラインの結果、生成処理の順序、単一評価を検査します。未対応のスカラー幅と不正なオペランドは明示的に失敗する必要があります。

## 構造化 C の制御フローと呼び出しの検証

`HighControlFlowSemantics.*` は、ループの出口や末尾を移動しても、他のジャンプが参照するラベルが保持されることを検証します。先頭・末尾の出口への直接ジャンプと break の置換を対象に、生成 C を O0/O2 で実行し、独立した期待戻り値と比較します。

`HighCPointerAddresses.Required*` / `UnknownConditionsFailOnlyWhenRead` は、推定された必須レジスタ引数の末尾にある未知スロットが保持されることを検証します。未知の必須引数や条件を評価すると明示的にトラップし、省略・null・入れ子のオペランドを黙ってゼロに置き換えてはいけません。既知の値と、読み取られないことが証明された余分なオペランドは実行可能なままです。トラップは診断境界であり、復元動作の等価性の証明ではありません。

## CPU 実行テスト

`NeverDIntegerABITests` は Windows x64、Linux x64、Linux ARM64 向けの Clang original fixture をビルドします。10 引数の実関数で register/stack と call frame を検証します。Unicorn/KVM/WHP matrix で利用できない host/ISA 組み合わせは明示的に skip し、skip を成功扱いしません。`NeverDExecutionBudgetTests` は時間依存の sleep を使わず、共有 continuation budget、reservation、絶対 deadline を検査します。

`NeverDCPUEmulationTests` は ARM64 命令、制御、load、context、alias、cache invalidation、有限 loop を確認します。software profile は FP/SIMD と TLS も実行します。`NeverDUserExecutionTests` は CPL3/EL0 権限、alias、保護 fault、context、space 切替を検証します。`NeverDServiceRequestTests` は SYSCALL/SVC が transport 前に intercept され、状態を保持し request が一度だけ消費されることを確認します。これは handoff protocol の検証であり、OS service 全体の実装ではありません。`NeverDExecutionConfigurationTests` は共通 resolver、build support と live probe の区別、未対応設定の fail-closed 動作を確認します。公開 SDK/CLI test は Windows model を必要としません。`NeverDThreadPointerTests` は FS base、`TPIDR_EL0`、context restore、権限を検証します。`NeverDKvmCancellationTests` は終了しない x64 guest で active KVM interruption、resume、caller signal state の保持を検証し、KVM がない場合は明示的に skip します。

```bash
cmake --build build-cpu --target NeverDKvmRunTests NeverDKvmCancellationTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDKvm(Run|Cancellation)Tests$' --output-on-failure
```

```bash
cmake --build build-cpu --target NeverDIntegerABITests NeverDExecutionBudgetTests NeverDCPUEmulationTests NeverDUserExecutionTests NeverDServiceRequestTests NeverDExecutionConfigurationTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(IntegerABI|ExecutionBudget|CPUEmulation|UserExecution|ServiceRequest|ExecutionConfiguration)Tests$' --output-on-failure
```

ARM64 hardware や hypervisor がない場合は native coverage の skip であり、pass ではありません。Unicorn と cross-compilation は native KVM/WHP 実行の証拠になりません。

## Linux process profile のテスト

[独立 process suite](process-emulation.md#検証) は x64/AArch64 の実際の ELF fixture を compile します。`NeverDLinuxProcessTests` は起動、program-header policy、service continuation、binary output、guest fault、resource stop を検証します。`NeverDProcessPublicTests` は分析用 image を変更せず C API/CLI を確認します。`NeverDExecutionSessionTests` は memory/budget を共有する CPU と request/fault の exactly-once 消費を扱います。`NeverDX64MemoryUpdateTests` は memory arithmetic、SETcc、BT、XMM/MXCSR、write observer、REP boundary、prepared device read を確認します。`DriverBackendParityTests.cpp` は original/relocated WDK fixture を実行し、観測可能な完全 report を Unicorn と比較します。fixture/backend 不在は明示的に skip します。

checked x64 は mask 付き legacy `ADD`、`SUB`、`MUL`、`DIV`、`SQRT`、`MIN`、`MAX` の `SS`、`SD`、`PS`、`PD` 形式も許可します。`X64SSEInstructions.def` が operand 幅、alignment、admission を一元管理します。`MaskedSSEArithmeticMatchesIndependentHostExecution` は独立した host CPU oracle で register/RAM 形式、4 種の rounding、FTZ、signed zero、subnormal、NaN を検証し、`SSEMemoryObserverStopsBeforeResultAndStatusChanges` は効果反映前の停止を検証します。DAZ、unmasked exception、x87、AVX は許可しません。

```bash
cmake --build build-cpu --target NeverDLinuxProcessTests NeverDExecutionSessionTests NeverDX64MemoryUpdateTests NeverDThreadPointerTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(LinuxProcess|ExecutionSession|X64MemoryUpdate|ThreadPointer)Tests$' --output-on-failure
```

未対応 backend は明示的に skip します。cross-compilation と Unicorn ARM64 は native KVM/WHP の証拠ではありません。

## ドライバーエミュレーションの検査

`NEVERD_ENABLE_DRIVER_EMULATION=ON` と `BUILD_TESTING=ON` を両方有効にすると、専用の実行スイートと共有 C API／CLI の検査をビルドできます。

```bash
cmake --build build-release --target \
  NeverDDriverEmulationTests NeverDDriverEmulationPublicTests --parallel 4
ctest --test-dir build-release -L '^NeverDDriverEmulation' --output-on-failure
```

fixture は、ゲストの初期化、成功／失敗の戻り値、未対応動作、メモリフォールト、厳格なシナリオ解析、予算内の実行、および create、転送、cleanup、close、unload を通る同期バッファード／ダイレクト I/O、READ/WRITE、独立したファイルの寿命、MDL の権限、動的エクスポート、ゲスト可変引数、構造化 CPU フォールトを検証します。[`emulate-driver` CLI](driver-emulation.md) で JSON とプロセスの終了コードを確認してください。製品ビルドは `BUILD_TESTING=OFF` でもこの機能を有効にできます。`libneverd` がテスト専用の Unicorn 構成を必要としてはなりません。

追加テストでは、ドライバー所有の非ページプール MDL、記述子とバッファーの独立した寿命、レジストリ問い合わせの配置と短いバッファー、ハンドル権限、削除とリーク、および出力のない IOCTL の完全な64ビット `information_hex` を検証します。実際のサンプル検証には Zero の同期直接読み書きと統計問い合わせも含まれます。

バックエンドテストは完全な CPU コンテキスト（レジスタ、フラグ、SIMD、FPU、CR8）、共有メモリ、別バックエンドや障害後コンテキストの拒否を検証します。コンパイル済み `driver_dispatcher.c` は実際の DPC／ワーク項目、タイマー境界、通知／同期イベントとタイマー、理由 `Executive` の非アラート `KernelMode` 待機、タイムアウト／遅延、複数の待機スタック、セット直後のリセットでも保持する起床、コールバック引数、不正 IRQL／寿命を検証します。ワーク項目の保留／完了、キュー、停滞、共通予算の検証も維持します。文書化した部分集合の証拠であり、完全な Windows 非同期対応ではありません。

`driver_context_limits.c`: API の IRQL 上限は `KernelAPIIRQL.def` にあり、引数依存の制約は担当モデルが検査します。DPC からレジストリ API やページプールの割り当て・解放・アクセスはできません。Unicode `DbgPrint` 変換は `PASSIVE_LEVEL` を要求し、対応する ANSI 出力と非ページ操作は `DISPATCH_LEVEL` で使用できます。コールバックスタックには範囲があり、逸脱したスタックポインターは別の待機ワーカーのスタックへ侵入できません。デバイス拡張内の有効なタイマーは早期解放を防ぎます。一般の IRQL 変更を公開する機能ではありません。

`KernelDeviceStackTests.cpp` は所有関係と接続の独立性、最上位選択、失敗時の原子性、スタック容量、不透明フィールド、ハンドル数、切断・削除をまたぐワーク項目／要求の保持、ファイルとディスパッチ対象の違いを検証します。独自の `driver_wdm_stack.c` は実 WDK ヘッダーとインライン Copy/Skip/SetCompletion を使用し、通常／有効 CFG イメージを任意の `NEVERD_WDM_STACK_FIXTURE`／`NEVERD_WDM_STACK_CFG_FIXTURE` で指定します。`DriverWDMStackTests.cpp` は再配置、下位の実状態、完了順序／条件、遅延 pending 伝播、ワーカー／DPC、待機、`STATUS_MORE_PROCESSING_REQUIRED`、直接 MDL 保持、入れ子の完了、不正カーソル／制御を検証します。`DriverScenarioPublicTests.cpp` は C API／CLI 転送と C API の保持／入れ子完了を、設定された CFG イメージも含めて検証します。欠落時は明示的にスキップし、Linux の証拠は同一ドライバーのスタック範囲だけを示します。PDO／PnP／電源対応は示しません。 `KernelIRPStackTests.cpp` は個数に基づくカーソル、完全なインライン Copy 範囲、消費済み位置のクリア、状態／pending 伝播、MPR と入れ子完了、継続所有者、保持経路を検証します。実 READ/WRITE とファイルのライフサイクルもインライン Copy で検証します。

`DriverPnpScenarioTests.cpp` は JSON／ネイティブ検証の一致、必須初期情報、ID／個数制限、禁止フィールド、最終バス状態、nullable な観測レポートを検証します。`KernelPnpDeviceTests.cpp`、`KernelPnpRequestTests.cpp`、`KernelPnpCompletionTests.cpp` は所有権、AddDevice 成功／失敗／リーク、初期 IRP、ファイル受付、状態ロールバック、遅延完了、MPR／入れ子／待機継続、失敗の原子性を検証します。独自の実 WDK `driver_wdm_pnp.c` は任意の `NEVERD_WDM_PNP_FIXTURE`／`NEVERD_WDM_PNP_CFG_FIXTURE` を使います。`DriverWDMPnpTests.cpp` は通常／有効 CFG の再配置イメージで AddDevice、ファイル I/O、順序付き削除、遅延開始／削除、開始／query 失敗、リーク有無の AddDevice 失敗を実行します。成果物がなければ明示的にスキップします。実行証拠は Linux のみで、記載したリソースなし PnP 範囲に限られます。 `DriverScenarioPublicTests.cpp` は通常／有効 CFG イメージの 7 要求の遅延 PnP レポートを C API と CLI でも検証します。

V9 の schema テストは 8 種の名前の往復と共有最終状態検証を確認し、QueryStop 0x119 をイメージ読み込み前に拒否します。拡張モデルと実フィクスチャは query-stop のロールバック、cancel-stop、停止／再開、突然の取り外し、厳密成功値、停止／取り外し待ちのソフトウェア I/O、取り外し後のゲスト拒否、デバイス識別、混在 AddDevice 結果を検証します。`DriverScenarioPublicTests.cpp` は通常／有効 CFG の 16 要求を C API と CLI から実行し、ソフトウェア IOCTL の正常バイト、突然の取り外し後の失敗、最終 cleanup/close/remove を保持します。公開実行は直列なので、現在の生成元がない保持 IRP を後続の start/cleanup で解除できません。Remove の排出条件はプロファイル境界であり、一般的な Windows の I/O 受付規則ではありません。証拠は Linux のみです。

`KernelRemoveLocksTests.cpp` は独立した所有者、NULL／重複 Tag、retail／DBG サイズ、即時／遅延排出、失敗取得の義務、原子性、容量、退役を検証します。`KernelRemoveLockBridgeTests.cpp` は接続前初期化、拡張部範囲、不透明領域、IRQL、危険な Delete／Detach の変更前拒否を確認します。真正 `driver_wdm_remove_lock.c` は retail／DBG と通常／active-CFG の4種で、`NEVERD_WDM_REMOVE_LOCK_FIXTURE`、`NEVERD_WDM_REMOVE_LOCK_CFG_FIXTURE`、`NEVERD_WDM_REMOVE_LOCK_DBG_FIXTURE`、`NEVERD_WDM_REMOVE_LOCK_DBG_CFG_FIXTURE` を指定します。`DriverWDMRemoveLockTests.cpp` はパケット退役後の release、排出後のバス完了、コールバック復帰前の待機準備、ワーカー待機、リークなし AddDevice 失敗を確認します。C API／CLI は既存 PnP スキーマで受信／完了／最終撤去を区別します。欠落は明示スキップ、証拠は Linux のみで、完全な Driver Verifier や一般並行排出ではありません。

`DriverPowerScenarioTests.cpp` は必須電源値、JSON／ネイティブ整合、不透明32ビット context、応答 FIFO 上限、独立子報告を検証します。`KernelPowerRequestTests.cpp` と `KernelPowerCompletionTests.cpp` はレイアウト、経路フラグ、ライフサイクルと通知状態の区別、FIFO 一致、最終コールバック所有権、MPR、待機、解放境界を確認します。真正 WDK の独自 `driver_wdm_power.c` には任意の `NEVERD_WDM_POWER_FIXTURE`／`NEVERD_WDM_POWER_CFG_FIXTURE` を使います。`DriverWDMPowerTests.cpp` は通常／active-CFG 再配置、直接／入れ子 Query/Set、独立遅延完了、S0 が D0 に先行する順序、待機を越える5引数スナップショット、ワーカー由来の子、null コールバック、query 拒否、PDO 別初期値／FIFO、明示値欠落を検証します。`DriverScenarioPublicTests.cpp` は不正入力と C API／CLI の6シナリオ／3子要求のスリープ復帰を追加します。成果物欠落は明示的にスキップし、Linux の実行証拠は文書化したページ可能・リソースなしの部分範囲に限ります。

`KernelUsbIdleTests.cpp` は所有権・D2・借用・最初の完了原因、`KernelUsbIdleBridgeTests.cpp`／`KernelUsbIdleReceiptTests.cpp` は実 IRP、取消、複合容量、受領と完了の順序、`DriverUsbIdleScenarioTests.cpp` は入力と報告を検証します。`DriverWdmUsbIdleTests.cpp` は本物の `driver_wdm_usb_idle.c`（`NEVERD_WDM_USB_IDLE_FIXTURE`／`NEVERD_WDM_USB_IDLE_CFG_FIXTURE`）で idle、D0/D3、取消、ウェイク、再登録／再起動、独立／複合機能、PDO/FDO 経路を検証します。`DriverWdmUsbIdlePublicTests.cpp` の [USB シナリオ](../examples/driver-wdm-usb-idle-scenario.json) は C API/CLI、通常/active-CFG、優先/再配置を対象とし、不在なら明示スキップ、実行証拠は Linux のみです。KMDF USB の対応を意味しません。

`KernelFrameworkUsbIdleTests.cpp`、`KernelFrameworkUsbIdleStorageTests.cpp`、`KernelFrameworkUsbIdleBridgeTests.cpp` はポリシー・実ストレージ・型付きスケジューリングを検証します。実 `driver_kmdf_usb_idle.c` 自身は USB idle IRP を送りません。`DriverKMDFUsbIdleTests.cpp` は許可なし、管理 I/O と遅延 D2/D0、callback 前／中 StopIdle、arm 失敗、Maximum 能力、remote wake、複合メンバーを検証します。`DriverKMDFUsbIdlePublicTests.cpp` は [KMDF USB シナリオ](../examples/driver-kmdf-usb-idle-scenario.json) を `NEVERD_KMDF_USB_IDLE_FIXTURE`／`NEVERD_KMDF_USB_IDLE_CFG_FIXTURE` で C API/CLI、通常/active-CFG、優先/再配置実行します。不在は明示スキップ、実行証拠は Linux のみです。 モデルテストでは wake-arm コールバック成功後の割り当て枯渇でも、実際の disarm と WAIT_WAKE のキャンセルが行われ、D2 応答を消費しないことを確認します。

`DriverKMDFUsbPoFxTests.cpp` は初期 SystemManaged／WithHint、独立した PoFx／USB 許可、D0 取消、遅延 D2／D0 と実 worker による F0 応答、StopIdle、READ 前のウェイク復帰、arm 失敗、削除・再起動を検証します。`DriverKMDFUsbPoFxPublicTests.cpp` は [USB PoFx シナリオ](../examples/driver-kmdf-usb-pofx-scenario.json) を C API／CLI、両モード、通常／active-CFG、優先／再配置で実行します。`DriverKMDFUsbIdleTests.cpp` は転送回帰を保持し、直接 READ の D0Entry 後配信を確認します。`KernelFrameworkRequestTests.cpp` は対応付け、caller-context 所有権、手動／停止キュー、IRQL を検証します。割り当て枯渇と独立ゲートはモデル橋テストの証拠であり、実 fixture は arena を枯渇させません。欠落 fixture は明示スキップし、実行証拠は Linux のみです。 `KernelFrameworkUsbPoFxBridge.RemovalPowerUpFailureAcknowledgesRequiredWithoutReleasingIdleWait` は RemovePending 中の実 D0Entry 失敗も検証します。Required の正確な応答と quiesce が F0／ActiveCondition なしで後処理を可能にし、一般の SET_POWER 失敗や自律的 surprise removal は主張しません。

`KernelPowerCompletionTests.cpp`, `KernelProviderWaitWakeTests.cpp`, `KernelWdmWakeEventTests.cpp`, `DriverWdmWaitWakeTests.cpp`: [ネイティブ WAIT_WAKE シナリオ](../examples/driver-wdm-wait-wake-scenario.json) は実際の WDK `driver_wdm_wait_wake.c` を `NEVERD_WDM_WAIT_WAKE_FIXTURE`／`NEVERD_WDM_WAIT_WAKE_CFG_FIXTURE` で実行します。START 中の発行、引数、自動 D0 なしのウェイク、再発行、取消、MPR、DPC 取消後のワーカー D0、正確なイベント捕捉、独立プロバイダーを検証します。通常／active-CFG と優先／再配置アドレスの実行証拠は Linux のみで、成果物がなければ明示的にスキップします。

`KernelPowerCompletionTests.cpp` は APC／DPC 受理、容量不足の原子的再試行、プロバイダーのみの同期／遅延・callback 有無、保持経路と MPR を検証します。実際の `DriverWdmWaitWakeTests.cpp` は DPC 取消 callback からの直接 D0、APC／DPC Query/Set、IRQL／CR8 維持、呼出し復帰後の PASSIVE 実行、禁止される高 IRQL WAIT_WAKE を検証します。[高 IRQL シナリオ](../examples/driver-wdm-elevated-power-scenario.json) は `DriverWdmWaitWakePublicTests.cpp` で C API／CLI、通常／active-CFG、優先／再配置を実行し、証拠は Linux のみです。

`DriverResourceScenarioTests.cpp` は JSON／ネイティブ明示情報、整数幅、個数、物理／レジスター重複、整列、ID、空バンク、設定の直列化を検証します。`KernelMMIOTests.cpp`、`KernelMMIOFailureTests.cpp`、`KernelResourceBridgeTests.cpp`、`UnicornMMIOTests.cpp` はバンク／マッピング所有権、別名、世代、packed リストの寿命、プロバイダー時序、再開時の値保持、突然の取り外し／電源可用性、正確な CPU／API 取引、失敗の原子性を扱います。独自の真正 WDK `driver_wdm_resources.c` には `NEVERD_WDM_RESOURCE_FIXTURE`／`NEVERD_WDM_RESOURCE_CFG_FIXTURE` を使います。`DriverWDMResourceTests.cpp` は実際のスカラー／REP アクセサー、通常／active-CFG 再配置、部分範囲の別名、ページ末尾マッピング、STOP／再開、不正アクセスを実行します。C API／CLI はイメージロード前の不正情報拒否と同じ14要求の再開シナリオを検証し、保持された IOCTL 出力と正確な map／unmap 回数を確認します。共通 [driver-register-bank-scenario.json](../examples/driver-register-bank-scenario.json) にはこの fixture のレジスター／IOCTL プロトコルが必要です。成果物不足は明示スキップ、証拠は Linux のみで、ホスト物理メモリや一般デバイスバックエンドは扱いません。

`DriverDMAScenarioTests.cpp` は明示能力、論理ドメイン、バイト／件数／時刻上限、方向の厳密性、設定と観測の分離を検証します。`KernelPhysicalMemoryTests.cpp` と `BackendBackingTests.cpp` は同一ページ上の割り当て境界、固定参照、CPU 権限不変、MMIO／再入の排除、全範囲検証失敗の原子性を確認します。`KernelRequestMDLTests.cpp` は構築済み記述子の別名と読み取り専用 PFN が同じ物理識別子を使うことを確認します。`KernelDMATests.cpp`、`KernelDMABridgeTests.cpp`、`SchedulerDMATests.cpp` は実 RAM、束縛されたテーブル呼び出し、インライン／待機 FIFO、別々のコールバック／マッピング寿命、ページ断片、方向違反、解放前検証、PDO 分離、世代／電源失敗を実行します。真正 WDK の独自 `driver_wdm_dma.c` は `NEVERD_WDM_DMA_FIXTURE`／`NEVERD_WDM_DMA_CFG_FIXTURE` を使い、`DriverWDMDMATests.cpp` と C API／CLI は実アダプターポインター、共通／SG ストレージ、別々に設定した DMA／割り込みを実行します。共有 [driver-dma-scenario.json](../examples/driver-dma-scenario.json)はこの fixture のプロトコルが必要です。成果物不足は明示スキップし、証拠は Linux に限定され、実ホスト DMA、PCI、一般デバイスエンジンを保証しません。`pluginsdk/python/tests/test_driver_dma_integration.py` は `NEVERD_TEST_LIBNEVERD`、`NEVERD_TEST_WDM_DMA_FIXTURE`、任意の `NEVERD_TEST_WDM_DMA_CFG_FIXTURE` で同じ JSON 境界を Python から実行し、バイト列、FIFO、コールバック内完了、不正 JSON、IRP 完了後の独立 DMA 失敗を検証します。

`KernelSEHTests.cpp` は純粋な展開計画、スコープ順序、不揮発 GPR の復元、有界スタック、明示的な未対応メタデータを検証します。`KernelExceptionTests.cpp` は正確な API 引数数、下位 32 ビットのステータス、型付き例外、IRQL 上限、モデル／CPU 状態を変更しないことを確認します。真正 WDK と `/GS-` の `driver_wdm_seh.c` は任意の `NEVERD_WDM_SEH_FIXTURE`／`NEVERD_WDM_SEH_CFG_FIXTURE` を使います。`DriverWDMSEHTests.cpp` は通常／有効 CFG／再配置イメージで直接およびヘルパーからの例外送出、入れ子ハンドラー、再送出、実際のフィルター、展開時の finally、検索順序、安定した例外レコード、対応する CPU フォールトの継続、完全な CPU 状態の復元を検証します。入れ子のフィルターと衝突した finally は関連付けられた論理スタックを使用し、無関係な CPU フォールトは明示的に拒否します。C API／CLI は [driver-seh-scenario.json](../examples/driver-seh-scenario.json) を実行し、null の API 結果と実際のゲストハンドラーメッセージを確認します。`pluginsdk/python/tests/test_driver_seh_integration.py` は `NEVERD_TEST_LIBNEVERD`、`NEVERD_TEST_WDM_SEH_FIXTURE`、`NEVERD_TEST_WDM_SEH_CFG_FIXTURE` を使用します。外部イメージがなければ明示スキップし、証拠は Linux に限定され、ユーザーバッファーや一般 SEH への対応を示しません。

`KernelDMAChannelTests.cpp`、`KernelDMAChannelBridgeTests.cpp`、共有の `SchedulerDMATests.cpp` は、混在する割り当ての FIFO、コールバック戻り値幅、純粋な受付／解放検証、レジスター再利用、連続ページ断片、操作全体のフラッシュ、CurrentIrp の捕捉、パケット／MDL／デバイス寿命を検証します。独自に作成し真正 WDK でビルドした `driver_wdm_dma_channel.c` には任意の `NEVERD_WDM_DMA_CHANNEL_FIXTURE`／`NEVERD_WDM_DMA_CHANNEL_CFG_FIXTURE` を使います。`DriverWDMDMAChannelTests.cpp` は通常／有効 CFG／再配置イメージを実行し、実際の MapTransfer と FlushAdapterBuffers、共通／SG／チャネルの共有上限、明示的デバイストランザクション、IRQ/DPC 完了、連続する操作、二つの PDO、失敗ケースを扱います。C API／CLI は 7 リクエストの [driver-dma-channel-scenario.json](../examples/driver-dma-channel-scenario.json)を実行し、二つのマッピング断片にまたがる単一トランザクションも含みます。`pluginsdk/python/tests/test_driver_dma_channel_integration.py` は `NEVERD_TEST_LIBNEVERD`、`NEVERD_TEST_WDM_DMA_CHANNEL_FIXTURE`、`NEVERD_TEST_WDM_DMA_CHANNEL_CFG_FIXTURE` で同じ公開 JSON インターフェースを使います。成果物不足は明示的にスキップし、Linux の実行証拠はシステム DMA コントローラーや任意の HAL マッピング／フラッシュの対応を意味しません。

`DriverInterruptScenarioTests.cpp` は明示的な raw／変換後記述子、混在および割り込み専用割り当て、厳密なイベントフィールド／件数、発生元識別子、独立 BOOLEAN 観測を検証します。`KernelInterruptsTests.cpp`、`KernelInterruptBridgeTests.cpp`、`SchedulerInterruptTests.cpp` は排他的な組の照合、不透明トークン、世代／接続の捕捉、イベント寿命、選択した Ex フィールドだけの読み取り、共有するロックと IRQL 復元、コールバック所有権、同時刻 ISR の優先度、変更前の容量失敗を扱います。`KernelFrameworkRequestTests.cpp` は呼び出し公開や参照消費を伴わない純粋なキャンセル事前検証と一括トークン容量を確認します。真正 WDK を使う独自 fixture `driver_wdm_interrupts.c` は `NEVERD_WDM_INTERRUPT_FIXTURE` / `NEVERD_WDM_INTERRUPT_CFG_FIXTURE` を使用します。`DriverWDMInterruptTests.cpp` は通常／active-CFG 再配置、旧来の 11 引数 ABI、Ex 1／2／4、実際の ISR→DPC 完了、AL 下位の FALSE、同期／手動ロック、独立 PDO、再開時の世代、不正なハードウェア情報を実行します。C API／CLI テストはイメージロード前に不正宣言を拒否し、7 要求の [driver-interrupt-scenario.json](../examples/driver-interrupt-scenario.json) で保留 IOCTL のバイト列と独立した配信観測を確認します。イメージ不足は明示スキップします。実行証拠は Linux のみで、共有／レベル／MSI 割り込みや命令単位のプリエンプションを証明するものではありません。

`DriverGuardTests.cpp` と独自の `driver_guard.c` の 4 変種は、有効／無効の CFG、ベースアドレスの再配置、check/dispatch ABI、不正なターゲットを検証します。`KernelFrameworkTests.cpp`、`KernelFrameworkControlTests.cpp`、`KernelFrameworkQueueTests.cpp`、`KernelFrameworkRequestTests.cpp` は、バインド、失敗時にロールバックするデバイス作成、キューのルーティング、バッファーの論理長、クリーンアップの順序と IRP／コンテキストの寿命を検証します。独自の `driver_kmdf_lifecycle.c` と `driver_kmdf_control.c` は、実際の WDK 1.33 ヘッダーで任意にコンパイルし、本物の `FxDriverEntry` ライブラリを通じてリンクします。CMake キャッシュのパスとして、ライフサイクルのイメージには `NEVERD_KMDF_FIXTURE` / `NEVERD_KMDF_CFG_FIXTURE`、制御デバイスの通常版／CFG 有効版には `NEVERD_KMDF_CONTROL_FIXTURE` / `NEVERD_KMDF_CONTROL_CFG_FIXTURE` を設定します。外部成果物がなければ明示的にスキップします。`DriverKMDFLifecycleTests.cpp`、`DriverKMDFControlTests.cpp`、`DriverScenarioPublicTests.cpp` の C API／CLI ケースは、実際のコールバック、バッファード／ダイレクト I/O、ワークアイテムによる保留要求の完了、失敗ステータス、アンロード、再配置後の CFG 実行を検証します。実行証拠は Linux に限定され、完全な KMDF や PnP／電源管理の対応を示すものではありません。

旧版キャンセルのテストは、キャンセル、入れ子のクリーンアップ、最終破棄まで API 継続処理を保持します。既にキャンセル済みの Ex は引き続きコールバックなしでキャンセル状態を返します。`KernelFrameworkRequestAccessorTests.cpp` と `KernelRequestMDLTests.cpp` の検証対象は、共有 64 ビット Information、完了時の長さ検査、元キュー／IRP 識別、NULL WDF ファイルハンドル、保持ハンドルの getter 結果、バッファー MDL キャッシュと最初の方向の ByteCount、直接記述子の識別と遅延マッピング、完了時の無効化、WDM 完了による迂回の拒否です。実際の制御デバイス fixture の L、M、D、C モードは通常／CFG 有効イメージで、旧版キャンセル、バッファー MDL／情報、直接 READ／WRITE MDL、完了後アクセスを実行します。

キャンセルのテストは、転送要求だけに許す仮想期限とレポートフィールド、完了優先と既にキャンセル済みの経路、マーク／解除の結果、キュー登録と配信済みの完了権限、コールバック待機、内部参照の寿命を検証します。スケジューラーのテストは DPC／キャンセル／ワーク項目の順序、容量、識別子の分離、中断／再開を独立に検証します。WDM キャンセルは明示的なモデルエラーのままです。


## テスト構成

`add_neverd_unittest` は GoogleTest 実行ファイルを 1 つ作り、検出した各ケースに
その実行ターゲット名と同じ CTest ラベルを割り当てます。

| ソース領域 | ターゲットと CTest ラベル | 対象 |
|------------|---------------------------|------|
| `unittests/TestProcessTests.cpp` | `NeverDTestProcessTests` | クロスプラットフォーム子プロセス、引用、リダイレクト、終了コード |
| `unittests/libc` | `NeverDLibCTests` | 既知の libc 名と分類 |
| `unittests/safety` | `NeverDSafetyTests`、`NeverDSafetyIntegrationTests` | シンクカタログ、識別優先順位、引数事前フィルタ、コピー越境ハント、ヒープ寿命監査、必須の PE/ELF/Mach-O × x86-64/AArch64 6 セル行列 |
| `unittests/lift` | `NeverDLiftTests` | Decoder/lifter の LowIR 形状、IR 段階、loader、relocation、形式 fixture、デコンパイル、代表的 patch 経路 |
| `unittests/semantic` の大半 | `NeverDSemanticTests` | 命令、ABI、制御フロー、C 式、lift/recompile の差分セマンティクス |
| `unittests/evm` | `NeverDEVMOpcodeTests`、`NeverDEVMBytecodeTests`、`NeverDEVMLoaderTests`、`NeverDEVMABITests`、`NeverDEVMAnalyzerTests`、`NeverDEVMDecoderPropertyTests`、`NeverDEVMProxyTests`、`NeverDEVMCallTests`、`NeverDEVMSemanticTests`、`NeverDEVMEmitterTests`、`NeverDEVMIntegrationTests` | hardfork metadata、input normalization、ABI/signature ambiguity、CFG/SSA/recovery、decoder boundary 全網羅と hostile input、proxy/call fact、interpreter semantics、LLVM/C/Solidity differential execution、public API routing |
| `unittests/sbf` | `NeverDSBFMetadataTests`、`NeverDSBFProgramImageTests`、`NeverDSBFLoaderTests`、`NeverDSBFAnalyzerTests`、`NeverDSBFVerifierTests`、`NeverDSBFISAConformanceTests`、`NeverDSBFAgaveConformanceTests`、`NeverDSBFSemanticTests`、`NeverDSBFEmitterTests`、`NeverDSBFLLVMEmitterTests`、`NeverDSBFLLVMDifferentialTests`、`NeverDSBFSourceDifferentialTests`、`NeverDSBFMalformedCorpusTests`、`NeverDSBFUpstreamConformanceTests`、`NeverDSBFExternalOracleTests`、`NeverDSBFSolanaModelTests`、`NeverDSBFIntegrationTests` | v0-v4 メタデータと ELF レイアウト、厳格な verifier/loader 動作、固定済み ELF 成果物 23 個、独立 official oracle、全 opcode の可用性、敵対的入力、CFG/復元、実行済み LLVM/C/Rust 差分 |
| `PatchFullSubstRTTests.cpp` | `NeverDPatchFullTests` | 4 ISA×3 オブジェクト形式の書き換え/難読化等価性 |
| `unittests/semantic` の重点変換ファイル | `NeverDSwitchXformTests`、`NeverDIndCallXformTests`、`NeverDCFGLoopXformTests`、`NeverDTwoTableXformTests`、`NeverDAvxUpperXformTests` | 大きなセマンティック実行形式から分離した高速再リンク用プローブ |
| `unittests/corpus`（submodule） | `NeverDWindowsEHCorpusTests`、`NeverDRustEHCorpusTests`、`NeverDGoEHCorpusTests`、`NeverDCxxItaniumEHCorpusTests`、`NeverDObjCEHCorpusTests`、`NeverDAdaDEHCorpusTests` | pin された 545 個の実バイナリから読み取る例外とランタイム metadata。各バイナリは manifest で復元が満たすべき下限を宣言している |

登録の信頼できる情報源は
[`unittests/CMakeLists.txt`](../../unittests/CMakeLists.txt)、
[`unittests/lift/CMakeLists.txt`](../../unittests/lift/CMakeLists.txt)、
[`unittests/semantic/CMakeLists.txt`](../../unittests/semantic/CMakeLists.txt)、
[`unittests/evm/CMakeLists.txt`](../../unittests/evm/CMakeLists.txt)、
[`unittests/sbf/CMakeLists.txt`](../../unittests/sbf/CMakeLists.txt)、
[`unittests/safety/CMakeLists.txt`](../../unittests/safety/CMakeLists.txt) です。

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
`check-neverd-cxx-itanium-eh-corpus`、`check-neverd-objc-eh-corpus`、`check-neverd-ada-d-eh-corpus` はそれぞれ 1 ライン
を実行します。CI の 3 ホストすべてがこのフラグ付きで configure し、6 ライン全部を
実行します。バイトはどこでも同一ですが、それを読むものは同一ではなく、1 ホストでの
corpus 実行は他の 2 ホストについて何も証明しません。
`scripts/audit_ci_test_inventory.py` は 6 つの label のどれかを欠く inventory を拒否
します。corpus を静かに読まなくなったビルドは、どのテストにも捕捉できない回帰だから
です。消えたものがテストそのものなのです。

live EVM opcode audit は次のコマンドで実行します。

```bash
python3 scripts/audit_evm_opcode_metadata.py
```

public CLI が受理する唯一の option は `--manifest-output` で、remote/ref/toolchain override は
提供しません。出力 manifest の closed contract は `schema 3` です。

ローカルと CI の標準経路は、公式
`https://github.com/ethereum/go-ethereum.git` に対して必ず
`git fetch --depth=1 --force` を行い、default branch の remote `HEAD` から得た正確な
SHA だけを detached worktree で検査します。各実行は予測不能な名前の private temporary
bare repository を使い、official fetch の authority ref と exact SHA を detached worktree
の生存期間中保持して、最後に repository と worktree をまとめて破棄します。shared persistent
Git repository や cache はありません。`local_docs`、既存 checkout、
submodule は監査経路ではなく、pin された submodule は live drift を検出すべき時点で古くなります。

各 Git command は継承した `GIT_*`（`GIT_CONFIG_*` を含む）を最初に全消去し、監査済みの
値だけを設定します。`GIT_CONFIG_NOSYSTEM` と `GIT_CONFIG_GLOBAL` は system/global
config を、`GIT_ATTR_NOSYSTEM` と command scope の `core.attributesFile` は system/global
attributes を、`core.hooksPath` は hooks を無効化します。想定外の private-repository config、graft、
`objects/info/alternates`、`refs/replace` は検証失敗となり、
`GIT_NO_REPLACE_OBJECTS` も replacement lookup を無効化します。

probe は `params.Rules` の export 済み bool field をすべて反射し、各 fork で
`LookupInstructionSet(params.Rules)` を呼んで 256 byte slot 全体を走査します。
`EVMUpstreamOpcodePolicy.def` は typed historical/unscheduled-EOF exclusion と alias、
`EVMUpstreamSemanticsPolicy.def` は closed Rules inventory、fork mapping、base-stack
exception、EIP-8024 dynamic opcode family の宣言をそれぞれ所有します。

CI は `dev` への push、pull request、manual dispatch、daily schedule でだけ同じ live
audit を実行します。Go probe は対応する各 fork で公開 API
`LookupInstructionSet(params.Rules)` を呼びます。`EVMUpstreamOpcodePolicy.def` は name
alias と review 済み historical/unscheduled-EOF exclusion を、直交する
`EVMUpstreamSemanticsPolicy.def` は fork rule、stack semantics 例外、EIP-8024 dynamic opcode
family の membership/activation を所有します。
closed manifest は正確な revision、fork activation、byte/name、`base_min_stack`、
`net_stack_delta` を検査し、未知または重複した field、fork、name、byte を拒否します。
allocation は `operation.undefined` だけで判定し、`HasCost` は defined zero-cost operation
でも false なので cost cross-check にのみ使います。すべての `defined && !HasCost` slot は
宣言した fork から `EVM_GETH_ACTIVE_WITHOUT_COST` と正確に一致する必要があります。cost を
持つ undefined slot、未レビューの defined slot、marker の消失は fail closed です。
CI 失敗時には正確な revision、manifest、log が artifact になります。parser と drift
diagnostic には独立した Python unit coverage があります。

`EVMUpstreamSemanticsPolicy.def` は export された boolean `params.Rules` field ごとに唯一の
`EVM_GETH_RULE_FIELD` を置き、`MappedForkSelector`、`NoOpcodeAllocation`、
`ExcludedSelectorExpectedError` のいずれかに分類します。probe は field を 1 つだけ有効にして
`LookupInstructionSet` を呼びます。最初の 2 category は nil error、3 番目は error でなければ
ならず、返された完全な 256-slot opcode/stack fingerprint は `ExpectedFork` と一致する必要が
あります。`IsEIP155`、`IsEIP2929`、`IsEIP4762`、`IsPetersburg` は現在 Frontier fingerprint
の no-allocation fields、`IsUBT` は error と Cancun fingerprint が期待値です。

`EVMEIP8024Immediates.def` は引き続き single/pair の各 byte に対する immediate semantics の
唯一の authority で、各 256 byte を明示分類します。production は直接 lookup します。live
audit は `go -overlay` で `core/vm` に virtual wrapper を注入して本物の private
`operation.execute` handler を得て、active な table/family ごとに `DUPN`、`SWAPN`、
`EXCHANGE` の `3x256` candidates と `3 missing-operand cases` を実行します。acceptance、PC
delta、marker-derived operand/stack mutation、valid case の正確な underflow、operand 欠落時の
`0x00` を検査し、Python は formula を再記述せず同じ `.def` と比較します。

`EVM_HARDFORK_LATEST` の canonical target は 1 つだけです。closed
`EVMUpstreamForkAliases.def` は Prague→Pectra、Osaka と BPO1〜BPO5→Fusaka、
Paris/Shanghai/Cancun/Amsterdam/Bogota→自身を定義し、未知名は fail closed です。記録した
1 つの `audit_unix_time` で `MainnetChainConfig.LatestFork(time)`（NeverD latest と一致必須）
と `LatestFork(max uint64)` の alias/probed canonical fork を検査します。probe は実在する
`canonical fork jump tables` と `mainnet active/scheduled jump tables` を列挙して一表ずつ完全
比較し、dynamic family または fork の `inactive` 状態を明示的に記録します。一部の
table/family/probe しか得られない `partial` result は受理せず fail closed です。manifest は
`authority=official-fresh-fetch`、公式 URL、要求 `HEAD`、SHA を固定
します。public CLI に remote/ref/toolchain bypass はなく、probe は `GOTOOLCHAIN=local` です。

Go の request/response と Python controller は hostile metadata を allocate する前に
`input/collection/string hard limits` を適用し、上限を超える input、array、string を fail
closed にします。別途 `bounded diagnostic output` を強制し、長すぎる表示には full-content
`digest` と `explicit truncated marker` が含まれます。すべての command に bounded child output
と共通 deadline が適用され、timeout または output-limit 違反は `process group` 全体と子孫
process tree を kill して pipe を drain します。すべての `.def parser` は unparsed、unknown、
duplicate、missing、out-of-range の entry を拒否して fail closed します。

現在の schema-3 live receipt は `schema_version=3`、
`audit_unix_time=1787534659`、`authority=official-fresh-fetch`、
`remote=https://github.com/ethereum/go-ethereum.git`、`ref=HEAD`、revision
`02b73d4ea7181464175e0a6cbecc0a3a2655a562`、local `Go 1.24.0`、
`stack_limit=1024`、`diagnostics=[]` を記録しています。`21 fork tables` と
`20 Rules probes` を対象とし、分類は `15 mapped/4 no-op/1 expected-error` です。2 つの
`mainnet active/scheduled` record は `upstream BPO2` を報告し、closed map はこれを
`NeverD Fusaka` に対応させます。EIP-8024 の `23 table targets` のうち active なのは
`Amsterdam/Bogota` だけで、`1536 candidate executions` と `6 missing-operand cases` を
生成します。`three handler symbols` は 2 つの active target 間で一致します。Python audit は
`67/67`、`C++ Opcode 10/10` です。macOS の実 run は `sandbox-exec` 内で成功し、最後の
`go run` は offline でした。Linux workflow は `bubblewrap` を必須にします。

すべての Go stage、すなわち `go env`、`go mod init`、`go mod edit`、`go mod tidy`、
`go mod download`、`go run` は `capability-root` filesystem sandbox を通過する必要があります。
read capability は private probe、fresh geth、検証済み `resolved GOROOT`、必要な system runtime
root の正確な集合だけを含み、書き込み可能なのは isolated environment root だけです。network は
必要な dependency stage にのみ許可され、final run は offline です。test は
`host HOME/workspace` に sentinel を置き、access が拒否され、どの output にも内容が現れないことを
要求します。Linux は `/` broad bind を持たない同型の `bubblewrap` policy を検証します。

```bash
python3 -m unittest -v scripts.tests.test_audit_evm_opcode_metadata
```

CMake に登録された 11 個の EVM test target は次のとおりです。

```text
NeverDEVMOpcodeTests
NeverDEVMBytecodeTests
NeverDEVMLoaderTests
NeverDEVMABITests
NeverDEVMAnalyzerTests
NeverDEVMDecoderPropertyTests
NeverDEVMProxyTests
NeverDEVMCallTests
NeverDEVMSemanticTests
NeverDEVMEmitterTests
NeverDEVMIntegrationTests
```

`NeverDEVMDecoderPropertyTests` は decoder が変わる各 fork で全 2-byte input を網羅し、
完全な decode と正確な `JUMPDEST` boundary を比較します。さらに長さを制限した決定的な
hostile input を全 fork に通します。

EVM control-flow の変更では、まず fixed-point と height-domain contract を実行します。

```bash
cmake --build build --target NeverDEVMAnalyzerTests --parallel 4
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.StackHeightDomain*:EVMAnalyzer.WholeProgram*'
```

これらの case は block をまたぐ internal return、有限 multi-target merge、loop
convergence、deterministic edge ordering、path-sensitive whole-stack lane、correlation
preservation、unknown jump、exact invalid target、fail-loud budget、strict/relaxed stack
fault を網羅します。`MayReachable` は CFG candidate のみで確定 semantic fact を作れません。
続けて 11 個すべての EVM target と live upstream audit を実行してください。

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

### メモリ安全性 fixture

`unittests/safety/fixtures/binaries` には x86-64 と AArch64 の PE、ELF、Mach-O
イメージが検入されており、各形式が用意する PDB または dSYM の付属ファイルに
加えて、イメージごとにリンカ MAP が付きます。MAP は strip されたビルドが唯一
残す識別情報なので、各セルは MAP を明示的に指定した解析も行い、型も行番号も
残っていない状態で発見が何を主張できるかを固定します。
`NeverDSafetyIntegrationTests` はすべてのホストで 6 セルすべてを実行します。
必要なイメージや付属ファイルが欠けていれば構成段階で失敗し、ホストのツール
チェーンによるスキップ経路はありません。

6 つの等価なバイナリは 1 つのソースから生成します。`make` はホストネイティブ
の smoke fixture だけを再構築します。検入済みの完全な行列を再生成するには次を
使います。

```bash
make -C unittests/safety/fixtures matrix
```

行列のレシピには Clang の Linux／Windows クロスターゲット、LLD の COFF ツール、
両方の Darwin アーキテクチャ、そして `dsymutil` が必要です。デバッグパスは
再マップされ、CodeView のコマンドライン記録は無効化されるため、検入された
付属ファイルが開発者のワークスペース絶対パスを取り込むことはありません。

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
[Windows 例外再構築](windows-exception-reconstruction.md)を参照してください。

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
[`SemanticRoundTripFixture.h`](../../unittests/semantic/SemanticRoundTripFixture.h)
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

Low/Med のテストは path-sensitive whole-stack execution lane と phi lane identity を
維持し、`MaxAbstractInstructionTransfers` を含む budget exhaustion を hard error にします。
strict は証明済み `Reachable` lane 上の unknown/fork-inactive opcode だけを拒否し、
`MayReachable` は確定 fact を生成しません。HighIR の selector/receive/fallback は root
lane と成功 terminal に制限されます。共有 selector は独立した standard evidence では
なく、standard ごとの `KnownFunctionVariantInfo` と成功 terminal の厳密な return shape
が一致したときだけ variant と return list を選びます。

interpreter は opcode 固有の side effect より前に typed stack preflight を実行します。
`EVMForkSemantics.def` は byte `0x44` を Paris より前の `DIFFICULTY`、Paris 以降の
`PREVRANDAO` と定義します。`REVERT`、fault、step limit、resource exhaustion は state を
rollback します。allocation failure は `ExecutionFaultKind::ResourceExhausted` であり、
entry snapshot 自体が作れなければ `HasPersistentStateSnapshot` は false で commit 不能です。

### EVM public boundary と budget regression

public API test は canonical
`Code`/`Fork`/`Instructions`/`JumpDestinations` と、すべての LowIR table、range、ID、lane、edge
reference を個別に改ざんします。`execute` は instruction lookup 前に `llvm::Error` を返し、
`lowerToMedIR` は index 構築や入力比例 allocation の前に、完全な malformed/over-budget LowIR
を拒否しなければなりません。`lowerToMedIR` については option validation、resource validation、
structure validation の順序を強制し、field ごとの `canonical decode replay` と
`lowerCanonicalLowToMedIR` より前に完了させます。public HighIR recovery は外部 LowIR/MedIR を
replay 検証し、`analyze` だけが自身の canonical IR に `lowerCanonicalLowToMedIR` と
`recoverCanonicalHighIR` を使用できます。これにより recursive/duplicate replay を避けつつ、
すべての HighIR option/resource budget を引き続き適用します。
interpreter は `EVMInterpreterLimits.def` の全 limit を exact
boundary/+1 で検証します。`MaxSteps` は専用 `StepLimit`、`MaxMemoryBytes`、
`MaxTraceEntries`、`MaxLogEntries`、aggregate `MaxLogDataBytes`、runtime
`MaxPersistentStateEntries` の exhaustion は `ResourceExhausted` で transaction effect を
rollback します。初期 aggregate `MaxHostReturnDataBytes` または persistent state の超過は API
error です。初期 `MaxCalldataBytes`、`BlockHashes`/`Balances`/`CodeHashes`/`ExternalCode`/
`BlobHashes` 全体の aggregate `MaxHostEnvironmentEntries`、aggregate
`MaxExternalCodeBytes` も API error です。`const execute preflight` は environment、snapshot、
result の copy より前にこれらを拒否します。return-data `ArrayRef` view と sort 済み table の
`lower_bound` lookup も、buffer copy や PC map なしで検証します。

独立した LowIR boundary test は aggregate diagnostic limit
`MaxLowDiagnostics` と `MaxLowDiagnosticBytes` を検証し、linear decode/CFG construction が
正確な count/最終 bytes を precharge して zero を拒否することを確認します。
HighIR safety test は lane ごとの sort 済み `Any/Exact/Excluded` domain、equality
match/exclusion、raw `XOR(selector, constant)` の false-edge match/true-edge mismatch、zero
word/calldata size/call value refinement、unknown condition の fail-closed を網羅します。
さらに `EQ` と `raw XOR` の両方の back-jump regression を検証し、別の function によって
`arguments`、`mutability`、`return shape`、`region` が汚染されないことを保証します。
`EVMAnalysisLimits.def` の `MaxHighDispatchCandidates`、aggregate
`MaxHighRecoveredArguments`、`MaxHighDiagnostics`、`MaxHighDiagnosticBytes`、
`MaxHighReferenceVisits`、`MaxHighMemoryTransferCells`、`MaxHighMemoryValueVisits` は exact
boundary/-1 で検証されます。fixed malformed diagnostic を含む全 output diagnostic は allocation
前に count と最終 bytes を課金しなければなりません。LowIR と HighIR の diagnostic budget は
独立に検証し、default root CFG region は block-PC list の reserve/copy より前に
`MaxHighRegionBlockReferences` を課金しなければなりません。
外部 CALL/CREATE result は nondeterministic host outcome として 2 本の正確な CFG edge を
検査するため ERC-1167 fallback recovery が保たれます。読めない selector condition は Unknown
のままで、fallback/function fact を作れません。

control-flow test は `EVMLowFaultKinds.def` の `InvalidJumpDestination` を
`end-of-code JUMPI` に適用します。invalid target かつ確実に true なら successful tail はなく
definite fault、確実に false なら成功です。unknown は成功し得る false path を残し、lane 全体を
definite fault としません。

ABI test は `EVMABIParserLimits.def` の grammar boundary と `EVMABITableLimits.def` の public
table cardinality/text boundary を exact limit/+1 で検証します。また invalid
kind/standard/evidence enum、metadata mismatch、noncanonical signature/return list、誤って
independent とされた shared selector、dangling/duplicate variant、word width でない event-topic
`APInt` を indexed selector/sorted topic lookup より前に拒否します。

`NeverDEVMOpcodeTests` は metadata architecture も強制します。割り当て済み opcode の
encoding/typed-value roundtrip、family boundary、hardfork alias、derived stack/host
maxima を検証します。

### Solana SBF 差分バックエンド

SBF メタデータテストは、各バージョン機能、オペコード衝突境界、Murmur3 syscall hash、リロケーション、ELF machine、レジスタ、VM アドレス定数を検証します。Loader fixture は vendored バイナリを使わず、従来の v0-v2 section レイアウトと section を持たない厳格な v3/v4 program-header レイアウトの両方を生成します。

`NeverDSBFISAConformanceTests` は v0-v4 の各 version について、すべての byte
encoding を独立監査済みの typed manifest と照合します。
`NeverDSBFExternalOracleTests` は activation と boundary の判断を、別途 build
した official Anza process と比較します。`NeverDSBFUpstreamConformanceTests`
は pinned Anza revision にある 23 個すべての ELF に明示的な outcome を割り当てます。

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
  NeverDEVMABITests NeverDEVMAnalyzerTests NeverDEVMDecoderPropertyTests \
  NeverDEVMProxyTests NeverDEVMCallTests NeverDEVMSemanticTests \
  NeverDEVMEmitterTests \
  NeverDEVMIntegrationTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -R 'EVM' --output-on-failure --parallel 4

# すべての重点 Solana SBF ターゲット/ケース
cmake --build build-release --target check-neverd-sbf --parallel 4
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
| ヒープ寿命監査またはコピー越境ハント | `NeverDSafetyTests` | `NeverDSafetyIntegrationTests` の全 6 セル |
| プロセス実行または quoting | `NeverDTestProcessTests` | 対応各ホストの影響を受ける CLI/セマンティックケース 1 件 |

テストは最も低い安定した境界で契約を表現してください。LowIR 形状テストは lifter
への帰属に有用です。妥当に見える 2 つの IR 形状が異なる動作をし得る場合は
セマンティックラウンドトリップが必要です。小さな opcode、CFG、観測状態の assertion
で十分なら関数全体の golden dump は避けてください。

## CI との関係

CI は Linux、macOS、Windows でテストを有効にした Release をビルドし、検出した
一覧を監査してからプラットフォーム固有のラベル除外を適用します。プロファイルは
`.github/workflows/ci.yml` と `scripts/audit_ci_test_inventory.py` にあります。
`NeverDSafetyTests` と `NeverDSafetyIntegrationTests` はすべての matrix ホストで
必須であり、各実行は同じチェックイン済み PE、ELF、Mach-O × x86-64、AArch64 fixture を読みます。高コストスイートのすべてを表す単一 matrix shard はないため、必要なクロスツールが揃うマシンではローカルの `check-neverd` が最も明確な完全マージ前シグナルです。

## 現在の Solana SBF conformance / sanitizer profile

この current list は上の短い SBF list を置き換えます。source differential suite は
clang に加えて `rustc` が必要で、compiler skip は coverage 欠落です。完全な aggregate
には `NeverDSBFProgramImageTests`、`NeverDSBFMalformedCorpusTests`、
`NeverDSBFISAConformanceTests`、`NeverDSBFUpstreamConformanceTests`、
`NeverDSBFLLVMDifferentialTests`、`NeverDSBFSourceDifferentialTests` と、metadata、
loader、analyzer、semantic、emitter、integration target が含まれます。integrated
profile は変動する総数ではなく、named target と結果を記録します。

sanitizer profile は `build-sbf-asan-ubsan` に分離して build します。revision を固定した
prebuilt package は必要な fork-only header を含むため、integration も同じ fail-fast
ASan/UBSan profile で実行します。

```bash
cmake --build build-sbf-asan-ubsan --parallel 4 --target \
  NeverDSBFMetadataTests NeverDSBFProgramImageTests NeverDSBFLoaderTests \
  NeverDSBFAnalyzerTests NeverDSBFISAConformanceTests \
  NeverDSBFVerifierTests NeverDSBFAgaveConformanceTests \
  NeverDSBFSemanticTests NeverDSBFEmitterTests NeverDSBFLLVMEmitterTests \
  NeverDSBFLLVMDifferentialTests NeverDSBFSourceDifferentialTests \
  NeverDSBFMalformedCorpusTests NeverDSBFUpstreamConformanceTests \
  NeverDSBFSolanaModelTests NeverDSBFIntegrationTests

ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:strict_string_checks=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
NEVERD_SBPF_ROOT=/path/to/sbpf \
NEVERD_AGAVE_CONFORMANCE_ROOT=/path/to/firedancer-test-vectors \
NEVERD_AGAVE_CONFORMANCE_REVISION=68bb4af40235562e8852fa23d5727e49c2a0b862 \
ctest --test-dir build-sbf-asan-ubsan --output-on-failure --parallel 4 \
  -L '^NeverDSBF'
```

### pinned SBF evidence snapshot（2026-08-24）

gate は Anza `sbpf`
`2510663bb8d894e8e3094be351e4bb4b604f1f84`、Agave
`ef210d67f2fabeee1730498188fa78854260c679`、Solana SDK
`122f32e571ce39face4beffaccea733e37c207fd` を固定します。official ELF manifest
は 23/23 を通過し、`NeverDSBFExternalOracleTests` は 1,411 opcode/boundary
case を `SBFOfficialOracleProtocol.def`、`SBFOfficialVerifierCases.def`、
`SBFOfficialExecutionConstants.def` 経由で
照合します。`SBFOfficialELFMutations.def` が malformed ELF の table-driven
contract であり、変動する総数は固定しません。
別軸の `41-case strict ELF differential` は strict-v3 matrix 全体を official
`verify-elf-batch` と NeverD に通します。この 41 case は 1,411 total に含みません。
`NeverDSBFAgaveConformanceTests` は Firedancer test-vectors の
`68bb4af40235562e8852fa23d5727e49c2a0b862` を認証し、loader fixture 1,955 `sol_compat_elf_loader_v1` 個
（accept 1,399、reject 556）を照合し、accept された各 ELF について `entry_pc`、`text_off`、
`text_cnt`、`rodata_hash`、`calldests_hash` を比較します。この gate は後段の instruction verifier を実行しません。

追加の official execution matrix は別枠です。active `(Version,Opcode)` case が
正確に 508、boundary case が 58、合計 566 の exact execution case です。1,411 の
verifier probe や `41-case strict ELF differential` を置き換えず、その総数にも含みません。
Linux Release CI は `--print-pinned-revision`、`--print-test-vectors-revision`、
`--print-toolchain` を使い、`NEVERD_SBPF_ORACLE` と
`NEVERD_AGAVE_CONFORMANCE_ROOT` を export するため両 external gate は必須です。
明示 oracle/corpus env がない local run は case を discover しますが skip できます。

`SBF_RUNTIME_VERSION` により `RuntimeVersionPolicy::ChainProfile` は historical
cluster/slot を反映し、official feature account activation に従って maximum ISA を
V0→V1→V2→V3 と進めます。現在は V3 です。明示 v4 は offline 分析用の
`RuntimeVersionPolicy::UpstreamToolchain` を使います。
現在の 10 MiB 上限は正確に `10'485'760` byte、65,536 は historical
provenance/test のみです。`SBFFaultCodes.def` は execution fault の安定値、
`SBFSourceStatuses.def` は別レイヤーの generated-source ABI を持ちます。

10,000 scale fixture が worklist、function ownership、multi-latch を守り、
machine 固有時間は固定しません。cluster/account/slot row は通常 test を
deterministic/offline に保ったまま `RPC activation audit` を可能にします。

## Android クラス一覧の性能

`neverd mobile INPUT --list-classes` を測定する前に、`NeverDMobileTests` を Release でビルドし、そのラベルを実行します。
リーダーテストは疎なメタデータ、Unicode、不正な参照、未対応のメソッド本体、チェックサム、予算を対象とします。
アーカイブテストは完全展開と選択ペイロードのクエリーを区別し、CLI テストは接頭辞の絞り込み、JSON の範囲、
既存出力の保持、multidex の失敗原子性を検査します。

独立したフィクスチャー／測定ハーネスは、計測サンプルを受け入れる前に各プロセスの完全な記述子一覧を検証します。

```sh
python3 -m unittest scripts.tests.test_benchmark_mobile_inventory -v
python3 scripts/benchmark_mobile_inventory.py \
  --output-dir /tmp/neverd-inventory-benchmark \
  --class-count 6000 --dex-count 3 --code-units 64 \
  --extra-string-bytes 8388608 --resource-bytes 16777216 \
  --repetitions 7 --neverd build-release/bin/neverd
```

実行ごとに新しい出力ディレクトリを使用します。`--generate-only` はフィクスチャーと manifest を書き込み、計時しません。
`--workload` は任意の `--peer-command 'tool {input} {prefix}'` 用に共通入力の種類を選択し、入力準備は計測コマンドの外で行います。
レポートにはハッシュ、コマンド、すべての新規プロセスのサンプル、ウォームキャッシュの前提、および GNU time のある Linux では
最大子プロセス RSS を保持します。この RSS は複数プロセスを使うツールの合計ピークではありません。
合成 APK はクエリー用コンテナーであり、インストール可能なアプリではありません。一覧取得の速度は参照検索の速度や Java 復元の品質を証明しません。

ハイブリッド CPU では、ハーネスと継承される子プロセスを同じ許可済み CPU に固定します
（Linux なら `taskset -c 4 python3 ...` など）。これにより高性能コアと高効率コアの混在を避けます。
レポートには継承した CPU アフィニティーを記録します。

## Android コード参照の性能

参照クエリーは復元リーダーの命令境界とコード検証を共有します。この境界を変更したらモバイルスイートを実行してください。
リーダーテストはオペランドプールの種類、照合モード、メソッド所有関係と共有コード、参照に見えるペイロード／即値、
不正入力、リソース上限を対象とします。共有デバッグストリームは各所有本体のフレーム、範囲、引数に対して検査します。
記憶領域上限のテストでは、大きなメンバー一覧と分岐が密集した本体の両方を維持します。永続索引と一時コンテナー拡張では寿命が異なります。
順序が変わった項目や重なる項目、同じ幅でも互換性のないプロトタイプを持つ共有コード、アラインされない入力記憶領域、
検索ブロック境界をまたぐ部分文字列一致も検査します。非公開デコーダーデータの性能変更は、完全な所有復元モデルと
参照出現の多重集合を維持し、未対応の復元メタデータでの失敗動作も保つ必要があります。
独立して生成した期待値と、実入力でのツール間の一致は区別してください。

独立した参照ハーネスは、命令を出力する際に期待する出現を記録します。測定する毎回の実行で完全なメソッド識別情報、
code unit 単位の PC、opcode、ターゲット識別情報、UTF-16 単位、重複数を検査します。
独立に期待する NeverD のカバレッジ集計数と `code_scan_complete` も検査します。

```sh
NEVERD_REFERENCE_TEST_BINARY="$PWD/build-release/bin/neverd" \
  python3 -m unittest scripts.tests.test_benchmark_mobile_references -v
python3 scripts/benchmark_mobile_references.py \
  --output-dir /tmp/neverd-reference-benchmark \
  --class-count 500 --methods-per-class 64 --matching-methods 2 \
  --dex-count 3 --resource-bytes 16777216 \
  --repetitions 7 --neverd build-release/bin/neverd
```

`--kind` と `--workload` でケースを選択します。`--extra-strings 65536` は実際の 32 ビット文字列索引を検査します。
ペイロードの囮は既定で有効です。`--no-payload-lookalikes` は同じレイアウトと真の参照を維持し、囮のペイロード値を置き換えて共通入力で比較できます。
正しさと計測の両結果を保持してください。偽のペイロード参照を返す、または実際の参照を取りこぼすクエリーは検証に失敗し、計測を受け入れません。

任意の `--peer-command` は `{input}`、`{kind}`、`{query}` を含む argv テンプレートを受け付けます。
別のツールが異なる意味を使う場合はクエリー構文を明示的に調整し、完全な出現多重集合を比較します。
宣言された検証範囲を保持し、完全なコードスキャンを行ったとは主張しません。一覧ベンチマークと同じ新規ディレクトリ、
CPU アフィニティー、新規プロセス、ウォームキャッシュ、RSS の条件が適用されます。
`NEVERD_REFERENCE_TEST_BINARY` がビルド済み実行ファイルを指さない限り、任意の CLI 単体テストはスキップされるため、そのスキップを報告してください。

## モバイル SDK のエクスポート証拠

手動ワークフロー `Mobile SDK Export Evidence` は、固定した Xcode SDK に対して `collect_mobile_ios_sdk_declarations.py --exports-only` を実行します。iOS 実機用とシミュレータ用の両 SDK から Foundation、CoreFoundation、UIKit のリンカーマップをそのまま保存し、ターゲット、SDK バージョン、SDK 設定のハッシュ、ファイルサイズ、SHA-256 を記録します。通常の宣言収集でも同じマップを保存します。ファイルの欠落、空ファイル、サイズ超過、SDK 外のファイルは収集を失敗させ、完了済みの証拠は保持します。リンカーマップはシンボルのエクスポートの証拠であり、呼び出し ABI やメソッド復元の成功を証明するものではありません。

## モバイル Swift String の ABI 証拠

手動ワークフロー `Mobile Swift String ABI Evidence` は、Xcode 26.5 を使い、arm64 iOS 実機とシミュレーター向けに固定の Swift 等価・順序比較プローブと C の `swiftcall` プローブをコンパイルします。`collect_mobile_swift_string_abi.py` は、ソース、LLVM IR、アセンブリ、コンパイラーの識別情報、SDK 設定、`libswiftCore.tbd` をハッシュ付きで保存します。両言語とも、正確な比較インポートが 5 引数を取り `i1` を返す必要があり、C はその結果を明示的に 1 バイトへ拡張する必要があります。ターゲットやシグネチャの相違、コマンド失敗、タイムアウトでは部分的な証拠を残して収集を失敗とします。この証拠はランタイム宣言を登録せず、メソッドの復元も証明しません。SDK 不要のテストは `python3 -m unittest scripts.tests.test_mobile_swift_string_abi` で実行できます。

## モジュール式 MBA 簡約

`SymReadability.*` は減算と補数の表記、結合的演算のコスト、1 ビットおよび広幅リテラル、共有木の飽和、予算付き候補選択、サンプリングを無効にした 3 ビットの総当たり同値性を確認します。`SymMBASample.*` は全演算子、決定的な割り当て、未使用の広幅入力について、狭幅と任意精度の検証を AP 評価器と比較します。評価方式の異なる版で候補品質を比べる場合は、両方の出力を同じ尺度で数え直してください。SDK の版ごとのサイズカウンターは診断専用です。

## ARM32 とフレーム転送のテスト行列

```sh
cmake --build build-release --target NeverDSymbolicTests \
  NeverDSymSimplifyGuardTests NeverDLiftTests NeverDMBASourceTests \
  NeverDHighCStoreForwardingTests NeverDMetadataJSONTests --parallel 4
build-release/bin/NeverDSymbolicTests
build-release/bin/NeverDSymSimplifyGuardTests
build-release/bin/NeverDLiftTests \
  --gtest_filter='HighSymSimplify.*:HighFrameStoreForwarding.*:ELFARM32ModeTest.*'
build-release/bin/NeverDHighCStoreForwardingTests
build-release/bin/NeverDMBASourceTests
build-release/bin/NeverDMetadataJSONTests --gtest_filter='ELFARM32ModeCAPITest.*'
```

フレーム spill の行列は両方の C バックエンドで x86-32（ELF/COFF/Mach-O）、ARM32（ARM/Thumb ELF）、AArch64（ELF/COFF/Mach-O）も対象にします。プライベートフレームの反復ロードは加減算へ簡約され、両方の最適化レベルでバイトの組、ワード境界の組、決定的な乱数ワードを正しく実行する必要があります。Clang AST は関数全体の残存 MBA 演算子を確認し、有効なアドレス式と区別します。HighFrameStoreForwarding はアクセス幅、ローカル変数の変更、メモリ書き込み、エイリアス、重複、順序付きアクセス、不正なグラフ、展開予算を検査します。HighCStoreForwarding は浮動小数点の再解釈を含む 4 アーキテクチャでキャッシュ値の定義を生存させ、SymSimplifyGuard はロードの同一性と順序、volatile/atomic、poison 境界を検査します。ELFARM32ModeTest は ARM/Thumb 選択、アドレス正規化、混在メタデータと矛盾の拒否を検査します。ELFARM32ModeCAPITest は SDK の明示的なエラーと Thumb 再ロード後のデコーダー復旧を検査し、InstructionMode はデコーダー、コードポインター、分岐、コード生成の境界を検査します。クロスターゲット Clang がなければスキップであり、形式が正しい証拠にはなりません。

`NeverDHighControlFlowTests` の `HighBoundPrivateFrameCopies.*` は、呼び出し ABI のバインド後に、外部へ流出しない私有フレーム領域を通るコピーを検証します。分岐、領域の再利用、ガード条件ごとの事実の一致を含みます。x64 と AArch64 の生成 C を `-O0` と `-O2` で実行し、未定義動作をトラップさせながら独立した算術結果と比較します。フレームアドレスの流出、未知の呼び出し ABI、未定義または不一致のフレーム別名、入口引数の再代入、重複アクセス、順序付きまたはアトミックなメモリ、不正な文、循環、予算超過では元の関数を保持することを確認します。通常の値変換を PHI コピーとして扱ってはいけません。

ソース投影はこの整理後にも可変引数のオブジェクトリストを再検証する。空の命令アドレスアンカーは許可し、隠れた副作用や制御移動は拒否する。同期クリーンアップでは、同じ保存済みレシーバに対する単一の `int64_t` または `uint64_t` ビューを許可するが、幅の縮小、浮動小数点変換、アドレス演算、再代入は拒否する。Foundation のオブジェクト集合と正常時・例外時の解錠トレースを `-O0` と `-O2` の両方で実行する。

## x64 のネイティブ同期例外

checked x64 の `DIV`/`IDIV` は実際のプロセッサ結果と `#DE` を使用します。KVM は非公開の supervisor IDT/IST、WHP は明示的な例外ビットマップを使用し、元のコンテキストと利用可能なエラーコードを転送エラーと区別します。OS は回復可能なイベントを消費してから継続コンテキストを設定します。Windows ドライバはゼロ除算と商のオーバーフローを `STATUS_INTEGER_DIVIDE_BY_ZERO` に変換し、実際の SEH filter、`__finally`、再試行を実行します。`NeverDX64ExceptionTests` は Unicorn 無効でも構築でき、`DriverWDMCPUException` は元の WDK 用例を検証します。利用できない ARM64 ホストは明示的にスキップします。

## 段階的な RAM 効果

`RAMTransaction` は物理実行リースの下で、命令が宣言した書き込み範囲の物理的な和集合だけを保持します。結果観測器を呼ぶ前に元の RAM を復元し、取消し、転送エラー、観測器の例外では部分的な RAM やレジスタを公開しません。CPU 例外では RAM を戻した後もアーキテクチャの例外状態を保持します。ARM64 の単一・ペアストアも同じ管理層を使います。x64 は 8/16/32/64 ビットの `XCHG`、`XADD`、`CMPXCHG` を実行し、LOCK または暗黙のロックを持つ形式には自然整列を要求します。`NeverDRAMTransactionTests` はホスト CPU との結果比較、復元、エイリアス、権限を検証し、利用できないプラットフォームを明示的にスキップします。デバイスと並列 SMP は対象外で、CPU スナップショットは確定済み RAM を戻しません。

## 完全な x87 状態

`NeverDEmulationArch` は ISA、ページテーブル、FP 状態の配置を所有し、ネイティブと Unicorn の転送が共有します。x64 コンテキストは x87 制御、状態、TOP、物理タグ、オペコード、命令／データポインター、8 個の 80 ビットレジスターを保持します。`FP0`–`FP7` は `RegisterValue` を使い、スカラーアクセスによる切り捨ては拒否します。`FPTag` は物理レジスターの非空ビットマップです。`NeverDX64FPTests` は全 TOP、正確な演算のホスト FXSAVE/FXRSTOR 比較と復元を検証します。checked x87 命令や全丸め意味論の証明を追加するものではなく、利用できないネイティブホストは明示的にスキップします。

`driver-strict` は一致する Linux x64 host の KVM と Windows x64 host の WHP を許可します。`auto` は対応する native transport を選び、cross-ISA は Unicorn を選びます。明示的な Unicorn と従来の V1 API は portable software profile を保持します。native 実行は entry 前に canonical address と instruction effect を検証し、hardware 不可用時は fallback なしで失敗します。未対応 instruction/OS behavior は明示的な error です。Windows x64 のネイティブ CI は Unicorn を無効にして必須の 359 検査すべてに合格します。内訳は CPU 検査 131 件、組み込みイメージ 26 個・WDK イメージ 46 個・シナリオケース 40 件を優先アドレスと再配置先で実行したドライバー結果 224 件、および SEH 境界検査 4 件です ([`9d4c130c`](https://github.com/NeverSight/NeverD/actions/runs/36981864458)). native ARM64 の実機証拠は未取得で、任意 driver や Android/Darwin の互換性を保証しません。

上記のネイティブ検証は、宣言済みのドライバーエントリーポイントと公開シナリオを対象とします。以下の機能別回帰テストと C API／CLI／Python の検証は、Windows での実行が明記されない限り、証拠の範囲が Linux に限られます。ネイティブのサンプル群が合格しても、すべてのテスト変種を Windows で検証したことにはなりません。

選択したバックエンドの機能は `executionCapabilities(Contract, ISA, Backend)` で照会します。`NativeLegacyX64` はネイティブ x64 ドライバー実行を表し、`NeverDNativeDriverTests` は既存のドライバー群を検証します。このテストは Unicorn を無効にしたビルドでも実行できます。

既存の CI ワークフローは一般のテストプロファイルより先にエミュレーションの全テストを実行し、検出一覧、JUnit 結果、CTest ログを `emulation-focused` に保存します。他のモジュールの失敗はこの実行を妨げません。利用できないハードウェアと任意のドライバー資料は明示的なスキップとなり、ソフトウェア実行やコンパイルの成功はネイティブ実行の証拠にはなりません。

Linux の `NeverDUnicornDeadlineTests` は pthread のスケジューリングを制御し、実際のタイマースレッドをゲストへの進入前に完了させます。x64、ARM32、ARM64 で、進入前の取り消しがゲストに影響せず、次の実行が独立した予算を使うことを検証します。公開エンジン API を使い、エンジン内部状態は変更しません。

`NeverDX64ExceptionTests` の `X64StateTransition` は、ネイティブ CPU で独立した RAM 読み取りと CR8 読み取りを実行します。TLS 基底と特権レベルを交互に変え、反復する除算例外から再開し、キャンセルされたエントリ後に TLS を変更します。ネイティブ状態転送の変更後は、この CTest ラベルと、エイリアス再マッピング、CPU コンテキスト、FP 状態、元のドライバーの結果比較を実行します。KVM/WHP が利用できなければ明示的にスキップします。


`NeverDKvmRunTests` は `/dev/kvm` を必要とせずに `KvmRunControl` の借用転送を検証します。`StateTransfersUseTheEntryThreadAndPrepareOnceAcrossRetries` は、準備、取得、インターセプトしたホストエントリが同じスレッドを使い、中断後の再試行でも準備が一度だけ行われることを確認します。他のケースは、エントリ前の準備失敗、取得失敗、準備中の停止、実行中エントリのキャンセルを検証し、その後の実行で古いコールバックが再利用されないことを確認します。実際のキャンセル、RAM ロールバック、例外、元のドライバーのテストも引き続き実行します。 `SequentialEntriesReuseWorkerWithoutRetainingPriorTransfers` は、同一期限内の複数エントリがワーカーを再利用し、各転送を一度だけ実行して以前の状態パケットを変更しないことを検証します。

KVM x64/ARM64 は `KvmRunControl` により同じ専用 vCPU worker で状態準備、`KVM_RUN`、状態取得を実行します。`EINTR` の再試行でも準備は一度で、取消や取得失敗は状態を公開しません。`KvmAArch64Machine.cpp` の変換維持と全スカラー・ベクトル転送も一つの step deadline を共有します。呼び出し側は完了確認後に確定し、ISA decode、RAM transaction、OS policy、observer は呼び出し側に残ります。native ARM64 の実機証拠は未取得です。

`ReusesCapturedStateAndInstallsHostChangesAcrossFaultsAndStops` は、ホスト側で汎用レジスタ、先頭・末尾 XMM レジスタ、MXCSR、x87 制御語を変更した後の継続実行と実 CPU のストアを検証します。停止したエントリ後の実際の `FXSAVE64` バイトで、全物理 80 ビットレジスタ、TOP、タグ、オペコード、ポインタを確認します。反復する除算例外も再利用を無効にします。これらの機械境界テストは checked プロファイルに追加の x87 命令を許可しません。

`NeverDKvmStateTransferTests` は実際の KVM 実行後にレジスタまたは XSAVE の読み取り失敗を注入し、変更前の入力で再試行します。独立した整数とパックドバイトの結果で、取得失敗後に進行済みのネイティブ状態が再利用されないことを確認します。このテスト実行ファイルだけが `ioctl` をラップし、ネイティブホストが利用できなければ明示的にスキップします。

Checked ARM64 の完全な状態は一つの境界で確定します。`Registers.def` が39個のスカラー項目と32個の128ビットベクトルを定義し、`captureAArch64State` が全読み取り、ビット幅、NZCV 正規化を検証して一度だけ公開します。Unicorn、KVM、WHP は TPIDR_EL0、TPIDRRO_EL0、TPIDR_EL1、FPCR、FPSR を含む同じ状態を転送します。native adapter は CPACR_EL1 で FP/SIMD を有効化します。読み取り失敗や entry の取消では呼び出し側の全状態を保持します。

ARM64 KVM/WHP の初期化は専用の `AArch64MachineProbe.def` を実行します。NOP、正の無限大へ丸める FP32 加算、2レーンの SIMD 加算です。各ステップで39個のスカラー値と32個のベクトルを比較し、TLS、NZCV、結果の上位ビット消去、FPCR/FPSR の保持と累積状態を確認します。監視用メモリは supervisor 専用で、全体の期限は共通です。成功が証明するのはこの有限の初期化プログラムであり、独立した native ARM64 ワークロード検証は未完了です。

x64 KVM/WHP のネイティブ初期化は、非公開の supervisor ページで `X64MachineProbe.def` を実行します。単一の期限内で NOP、正の無限大方向に丸める FP32 加算、2 レーンの SIMD 加算、FS/GS ロード、CS/SS/CR8 読み出しを行い、各ステップで全スカラー、XMM、物理 x87、制御状態を比較します。x64 と ARM64 の検査には物理メモリの排他的実行リースが必要です。`MemoryProjection` がキャッシュ識別子（ISA、アドレス空間、マッピング世代、権限、モニター構成）と ISA ごとの確定済みページテーブルルート履歴を所有します。非公開バイトを書き換える前にキャッシュを無効化するため、再構築失敗時の不完全なテーブルや呼び出し側の古いルートを再利用しません。この検査が証明するのは限定された初期化のみで、ARM64 の独立したネイティブ負荷検証は未完了です。

共有 XSAVE デコーダーは標準形式と圧縮形式の SSE 初期状態を区別します。XSTATE_BV[1] が 0 の場合、どちらも XMM を初期化しますが、標準形式は MXCSR を読み取り検証し、圧縮形式は MXCSR を初期化します。`X64XsaveCases.def` は独立したデータ配置と独自のホスト XRSTOR プログラムを提供します。`X64XsaveTests.cpp` は拒否時の状態の原子性を検証し、呼び出し元の FP/SSE 状態を保存しながら、両形式を実ホストの実行結果と比較します。ホストのアーキテクチャーや必要な命令機能が利用できなければ明示的にスキップします。

`X64FPState.def` は圧縮 AVX、AVX-512、CET_U/CET_S、AMX の転送配置と成分の 64 バイト境界を宣言します。存在する拡張成分は全ゼロの初期状態に限り、欠落成分のデータと境界調整領域は状態を定義しません。配置ビットがオフセットを決め、未知の配置、非初期値、不正な長さは公開前に失敗します。`CompactedOffsetsFollowLayoutRatherThanPresentBits`、`WideLayoutIgnoresAbsentComponentsAndAlignmentPadding`、`InitialCETComponentsDoNotHideFPState`、`InitialWideComponentsDoNotHideFPState` は 872 バイトと 10752 バイトの WHP パケットを検証します。これらの拡張命令の実行を許可するものではありません。

`WhpXsaveRegisters.def` は完全な XSAVE パケットを名前付き x87/SSE 制御レジスターで補完します。最終オペコードと命令・データポインターを明示的に書き込み、ホストから取得します。ゼロのパケット項目は補完できますが、非ゼロのメタデータ衝突や共通制御値の不一致は状態公開前に失敗します。`NamedMetadataRestoresOmittedPacketFields` は FP ペイロードを保持したまま欠落項目を検証します。

ネイティブの `FOP/FIP/FDP` はホストの x87 保存・復元規則に従います。マスクされていない保留例外がなければ AMD はこれらをゼロにでき、スナップショットは観測値を保持します。`X64MachineProbe.def` と厳密な NOP/コンテキストテストは整合する保留例外を設定し、全フィールドを有効な状態で差分を隠さず比較します。ホストプロセスの FXRSTOR64/FXSAVE64 参照は両状態を検証し、バックエンドはホストの結果を入力メタデータで置き換えません。

`NativeGuestRAMDistinguishesEntryFromCaptureLoss` はゲストの FXSAVE64 が RAM に保存した完全な FP/SSE とホストの XSAVE 取得結果を比較します。両 API で直接設定とゲスト内 FXRSTOR64 を試し、既定のポインター保存機能とホスト対応値を明示指定した場合を検証します。入口、ゲスト実行、取得の境界を分離し、値を修正せず、不一致は失敗として保持します。 境界マトリックスは保留中のマスクされていない x87 例外も検証し、ホストプロセスでの FXRSTOR64/FXSAVE64 の参照結果とプロセッサーベンダーを記録して、条件付きポインター保存と WHP 転送動作を区別します。

共通の `encodeX64XsaveState` / `decodeX64XsaveState` が標準・圧縮 FP/SSE パケット、物理 TOP の回転、欠落成分の初期状態、アトミックな検証を所有します。WHP は完全な XSAVE API を使い、`WHvGetVirtualProcessorState` / `WHvSetVirtualProcessorState` を優先し、旧 XSAVE API を互換経路とします。旧式の個別 x87 レジスター転送は完全なパケットを代替できません。非初期状態の拡張成分、不正なヘッダー、制御値、切り詰められた取得結果は明示的に失敗します。WHP マッピングエラーは診断用に HRESULT、GPA、サイズを保持します。

`CheckedX64Instructions.def` は既存の CPU バックエンドで 8/16/32/64 ビットの符号なし `MUL` と `CBW/CWDE/CDQE/CWD/CDQ/CQO` を許可します。`NeverDX64IntegerTests` は独立した `X64IntegerCases.def` の命令列と期待値を使い、両特権レベルで部分レジスターの保持、32 ビットのゼロ拡張、積の上位・下位、定義された CF/OF、符号拡張によるフラグの不変性を検証します。通常 RAM の乗算はアクセス範囲全体の権限検査と読み取り観測を維持し、障害や観測コールバックによる停止では暗黙の出力レジスターと PC を保持します。デバイスオペランドは未対応です。checked Unicorn でも実行し、利用できないネイティブバックエンドは明示的にスキップします。

`X64BitInstructions.def` は 16/32/64 ビットのレジスタと通常 RAM の `BT/BTS/BTR/BTC` を許可します。レジスタのビット索引はオペランド幅の符号付き値としてワード全体を選択し、即値は基底ワード内に限定されます。アドレス幅による切り詰めは FS/GS 基底の加算より前に行います。CF と書き込み値はプロセッサが生成し、`RAMTransaction` は観測コールバックの承認まで結果を非公開に保ちます。全範囲の権限検査は独立したページ割り当てとエイリアスを対象とし、停止、コールバック失敗、アクセス拒否では元の CPU と RAM を保持します。LOCK は自然整列されたメモリ変更形式に限られ、MMIO とハードウェア並列 SMP は未対応です。`X64BitStringTests.cpp` は独立した符号化を x64 ホストの実行と比較し、負の索引、幅の切り詰め、ページ境界、キャンセル、不正な LOCK 形式を検証します。[Intel 命令リファレンス](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)を参照してください。

`X64StringInstructions.def` は通常 RAM の 8/16/32/64 ビット `MOVS/STOS/LODS` を管理し、`CLD/STD` は他のフラグを変えずに方向を制御します。REP は各要素の全範囲を観測前に検証し、再開可能な境界で確定します。後続の障害でも完了済み要素は残り、停止やコールバック例外は現在の要素を変更しません。FS/GS はアドレス幅の切り詰め後にソースだけへ加算します。AL/AX のロードは上位ビットを保持し、EAX はゼロ拡張します。32 ビットアドレスのゼロ回 REP はカウントの上位ビットがゼロである必要があり、MOVS/STOS では使用するアドレスレジスタも同様です。それ以外は実 CPU ごとに結果が異なります。MOVS/STOS/LODS の REPNE 形式と STOS/LODS のデバイス操作数は未対応です。`X64StringTransferTests.cpp` は独立したホスト命令で幅、方向、重なり、ゼロ回を照合し、権限、エイリアス、折り返し、障害、再開も検証します。独自の WDK リソースドライバは `driver_resource_strings.def` を使い STOS/LODS の全四幅を実行します。

`X64StringInstructions.def` は通常 RAM 上の 8/16/32/64 ビット `CMPS/SCAS` と `REPE/REPNE` も管理します。各要素は観測前に読み取り範囲全体を検証し、六つの算術フラグを更新して最初の終了条件で停止します。データ障害では、この連続した REP の開始時のフラグを復元し、完了済みのポインタとカウント更新は保持します。公開 API からの再開は公開済み CPU 状態を出発点とします。停止や観測例外は現在の要素を変更せず、早期終了後は次の要素を読みません。FS/GS は CMPS のソースだけに作用し、SCAS は累算器と未使用のソースレジスタを保持します。デバイス操作数と曖昧な 32 ビットゼロ回実行時の上位ビットは対象外です。`X64StringComparisonTests.cpp` は独立したホスト命令とフラグ、方向、エイリアス、折り返し、権限、再開を照合し、Linux x64 シグナルで実際の障害時レジスタも検証します。独自 WDK リソースドライバは `driver_resource_strings.def` で全四幅の両条件反復を実行します。[Intel 命令リファレンス](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)を参照してください。

`WhpResourceCache.h` は論理 CPU の状態と WHP パーティションを分離します。ランタイムは一つのネイティブパーティションを保持し、同じ CPU の連続ステップでは再利用します。CPU 切り替え時は古いパーティションを破棄してから、マッピングと仮想プロセッサを再構築し、完全な状態を復元します。各論理 CPU は独立した `MemoryProjection` ビューと正本の RAM を保持します。リース取得はキャンセルと現在の期限を守り、非アクティブ CPU の破棄は別の CPU のパーティションを破棄しません。x64 はホスト既定の XSAVE 機能群を保持し、`WHvGetPartitionProperty` で実効設定を検証します。依存機能を消してマスクを縮小しません。協調的な CPU 切り替えは並列ハードウェア SMP を提供しません。

`NeverDX64FPTests` は起動状態の全 79 破損位置を検査し、独立にアセンブルした `X64ProbeCases.def` をネイティブ実行して単一期限とゲスト RAM 保持を確認します。`NeverDProjectionCacheTests` は呼び出し側変更、ISA 順序、ルート履歴、権限・モニター構成、マッピング世代、アドレス空間識別、再構築失敗を検査します。`NeverDRunControlTests` の `WhpXsaveTests.cpp` は新旧 API パケット、全 TOP、サイズ上限、失敗時の状態保持を検査します。このメモリ内プロトコル検査は WHP ネイティブ実行の証拠ではなく、利用できないネイティブ転送は明示的にスキップします。

XSAVE 検証診断はサイズ照会、ローカルデータ準備、取得データのデコードを区別し、API 名、返却バイト数、容量、限定したヘッダーと制御フィールドを保持します。独立した期待値は `WhpHostFailureCases.def` にあり、ゲストのレジスターデータは出力しません。`InvalidInputReportsPreparationWithoutHostMutation` は無効な入力でホストを呼び出さず、そのデータも変更しないことを検証します。共有 ISA コーデックが検証を一元的に担当します。

WHP の能力照会、パーティション/仮想 CPU の初期化、レジスター/XSAVE 転送、実行で発生したホスト API エラーは、HRESULT と `WhpProtocol.def` で宣言した API 名を保持します。 能力照会の失敗は型付きの利用不可結果を維持します。 `WhpHostFailureCases.def` は、キャンセルと同時に起きるホスト障害、および新旧 XSAVE API の照会・設定・取得失敗に対する独立した期待値を定義します。 Windows 専用 CI は 166 件の実機成功を要求します。内訳はマッピング 16 件、起動 2 件、FP/コンテキスト 10 件、共有 CPU 7 件、整数 8 件、`NativeInstallRetainsFPStateBeforeAnyGuestExecution` の API 2 種類です。後者はゲスト実行前に完全な FP/SSE と個別に取得したメタデータを比較します。 未登録、スキップ、無効化、未実行は原生実行証拠の監査失敗となります。 追加の 26 件は、両方の特権レベルでの `X64BitStringTests.cpp` の全ケースです。 Windows PE64 は WHP プロセスの 33 ケースと独立したネイティブ Windows 比較 2 ケースを必須とします。

`NeverDMemoryLifecycleTests` は Unicorn と独立して構築され、ネイティブ専用構成でも登録されます。Unicorn を無効にすると専用のソフトウェア投影・デバイスケースは明示的にスキップされますが、ホストに一致する共有 CPU ケースは残ります。`WhpMemoryTests.cpp` は `WhpMemoryCases.def` の 16 ケースでネイティブメモリ API を分離します。ページ・投影サイズの領域、共有・独立した割り当て、未アクセス・常駐したバイト、最初の仮想プロセッサの有無を検証します。各ケースは二つの論理所有者を維持し、マッピング済みパーティションを繰り返し切り替え、非アクティブ所有者を破棄した後も残るマッピングが再構築なしで利用可能なことを確認します。実際のマッピングエラーは HRESULT を保持して失敗となります。これはメモリ API の証拠であり、命令実行の証明ではありません。

`X64MachineProbe.def` の起動診断は、失敗した命令と、不一致のスカラー、TLS、特権、x87 制御、物理 FP レーン、XMM ワードをすべて列挙し、期待値と観測値を保持します。`DiagnosticIdentifiesStepFieldAndBothValues` は独立した期待メッセージを検証します。状態比較は引き続き完全一致を要求し、転送損失と命令実行の問題を区別します。失敗した実機プローブを成功扱いにはしません。

`WhpResourceTests.cpp` はキャッシュ再利用、置換前の破棄、失敗からの復旧、期限と停止の競合を検証します。`LogicalCPUSwitchingRestoresPhysicalFPAndTLS` は両特権モードで二つのマシンを交互に実行し、物理 x87/XMM と FS/GS の独立した状態を確認してから、一方の破棄後に残るマシンを再開します。Windows CI は両モードの WHP ケースを必須とします。

`NEVERD_ENABLE_SEMANTIC_TESTS` の既定値は `ON` で、`unittests/semantic` のテスト群と集約実行ターゲットを制御します。Unicorn を使わずにネイティブ CPU テストを構築するには、`BUILD_TESTING=ON` を維持し、`NEVERD_ENABLE_SEMANTIC_TESTS=OFF` と `NEVERD_EMULATION_BACKEND_UNICORN=OFF` を指定します。適切な SDK ヘッダーを備えた Windows ARM64/MSVC を含め、KVM/WHP のネイティブテストは引き続き構築できます。Windows ARM64 で Unicorn を有効にする場合は ARM64 LLVM-MinGW ツールチェーンが必要です。このビルド分離は ARM64 の実機実行を検証するものではありません。

ネイティブ CPU 専用 CI は固定リビジョンの Capstone ソースを初期化し、検証済みの LLVM パッケージを使います。`NEVERD_ENABLE_SEMANTIC_TESTS=OFF` と Unicorn アダプターの無効化により、CPU テストの構成・ビルド・リンクに Unicorn ソースは不要です。署名データや外部コーパスにも依存しません。既定の CI は完全な意味論テスト群を引き続き有効にします。

既存の `ci.yml` は Windows x64 runner で明示的に選ぶ手動モード `native_cpu_only` を提供します。`NativeCPUTests.def` が十のテスト所有者を選び、`run_native_cpu_ci.py` が構築してから絞り込んだ CTest を実行し、一覧・JUnit・ログ・集計を保存します。共通 CI パーサーは成功、失敗、スキップ、無効、未実行を区別します。宣言された WHP ネイティブマッピングケースはすべて検出・実行が必須で、証拠の欠落やスキップはこのジョブを失敗にします。既定の LLVM ソースビルド CI は変わりません。プロトコルテストやコンパイルは WHP・ARM64 のネイティブワークロード検証を代替しません。

`native_cpu_only=true` と `native_driver_tests=true` を指定すると、Unicorn なしで `NeverDNativeDriverTests` を有効にします。構成前に `build_wdk_driver_fixtures.py` が Microsoft 公式 WDK/SDK 10.0.26100.6584 パッケージ全体の SHA-256 を検証し、元のソースから通常版・CFG 版・DBG 版のドライバーイメージを計 46 個構築します。`WDKDriverFixtures.def` がパッケージ識別子、コンパイラーとリンカーの引数、フィクスチャの対応を定義します。変更していない Microsoft のファイルとライセンスはローカルのビルド／キャッシュ内に保持し、CI はビルドメタデータとログだけをアップロードします。マニフェストにはツールのバージョン、コマンド、ソースとヘッダーのハッシュ、出力イメージのハッシュを記録します。

`NativeDriverTests.def` は `DriverBuiltinImages.def` と `DriverBackendParityCases.def` の全 112 ワークロードについて、元のアドレスと再配置先で計 224 の WHP 結果を必須とします。内訳は組み込みイメージ 26 個、WDK イメージ 46 個、要求シナリオ 40 個です。CPU の 166 検査と純粋な SEH 継続の回帰検査 4 件を合わせ、必須の結果は 394 件です。固定イメージの再配置では従来どおり拒否を期待します。WDK イメージやシナリオが欠落またはスキップされると、この任意の CI ジョブは失敗します。通常のローカルビルドでは外部フィクスチャは任意のままです。`run_native_cpu_ci.py --with-drivers` は構成済みのテストターゲット、完全な一覧、JUnit 証拠を記録します。イメージの構築だけでは Windows や ARM64 のネイティブ実行を証明しません。次のコマンドでローカルに再現でき、生成したキャッシュを既存のエミュレーションビルドへ読み込むこともできます。 `166 CPU + 224 WHP + 4 SEH = 394`.

C SEH のスコープは終端を含まない半開区間です。有効な `__C_specific_handler` の着地点が保護区間内にある場合もあります。[LLVM 20.1.8](https://github.com/llvm/llvm-project/blob/llvmorg-20.1.8/llvm/lib/CodeGen/AsmPrinter/WinException.cpp#L600-L608) は区間終端に `EndLabel + 1` を出力します。Windows OS モデルは元の端点を保持し、再配置後も実行可能性、所属関数、継続先の一致を独立に検証します。`KernelSEHContinuationCases.def` は元のフィクスチャの配置を保持し、`ScopeEndLabelMayOverlapTheHandlerLandingPad` は定数ハンドラーとフィルターを検査します。関連テストは終端の除外と、不正な対象を拒否してもディスパッチ状態を消費せず再試行できることを確認します。これらの純粋なモデル検査は Unicorn を無効にした `NeverDNativeDriverTests` でも実行されます。

ターゲットへの展開も元のスコープ終端を使用します。ハンドラーの対象が `finally` の保護区間内に残る場合、そのスコープからは退出しません。`FinallyRespectsRawScopeEndAtHandlerTarget` は境界の両側を検査し、Windows x64 では `ntdll.dll!__C_specific_handler` と直接比較します。NeverD はコンパイラー生成の区間を修正しません。元のフィクスチャを Clang 20/21 で構築すると、偏った終端が選択対象を含むため `T` と `J` モードはゲストの失敗を返します。Clang 23 では両方のクリーンアップを実行します。[LLVM 変更 #144745](https://github.com/llvm/llvm-project/pull/144745) は古い `+1` バイアスを削除します。これらのコンパイラー依存の結果はバックエンド障害と区別します。

ビルド一覧はすべての独自 WDM/KMDF C フィクスチャと任意の WDK CMake パスを網羅します。公開された各 `driver-*-scenario.json` には `DriverBackendParityCases.def` 内に通常版と CFG 版があり、ソース・ビルド・シナリオの対応漏れは一覧テストを失敗させます。`Original` はシナリオのロードアドレス指定を解除して推奨ベースを確認し、`Rebased` は宣言した再配置先を確認します。ドライバー所有 IRP のシナリオは子要求を意図的にキャンセルするため、正常な後処理後も `DriverNativeOutcomes.def` に集約結果の失敗を期待値として残します。

```bash
python3 scripts/build_wdk_driver_fixtures.py \
  --output build-driver-fixtures --cache build-driver-packages
cmake -S . -B build-native -G Ninja \
  -C build-driver-fixtures/fixtures.cmake \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_DRIVER_EMULATION=ON \
  -DNEVERD_ENABLE_SEMANTIC_TESTS=OFF \
  -DNEVERD_EMULATION_BACKEND_UNICORN=OFF
```

`NeverDAArch64StateTests` は全スカラーフィールド、全ベクトルの両ワード、権限変更、浮動小数点命令の未実行、転送エラー診断の保持を検証します。`NeverDAArch64FPTests` の `OriginalProgramChecksCompleteStateAndOneDeadline` は独立に組み立てた `AArch64ProbeCases.def` の命令を実際の転送で両権限から実行します。テストは PC に依存しない命令をゲストコードへ移し、監視ページへの user アクセスは許可しません。Unicorn の実行と明示的な native skip は native ARM64 起動証拠を代替しません。

`CheckedAArch64Instructions.def` と `AArch64InstructionEffects` は EL0/EL1 で範囲を限定した基本 FP32/FP64 演算、比較、転送、固定幅 SIMD を許可します。FPCR は4種類の丸め、FZ、DN に対応し、FPSR は累積状態と QC を保持します。未対応の制御・状態ビットは変更前に拒否します。FP16 演算、SVE/SME、非マスク例外、追加拡張、未列挙の形式は明示的なエラーです。Windows ARM64 ドライバーのロードや新しい OS 環境は追加しません。

`AArch64InstructionEffects` が最大128ビットの scalar/FP/SIMD 単一・ペア RAM 範囲を所有します。共有 address space は CPU entry 前に全ページを検証し、`RAMTransaction` は宣言された完全な物理書き込みのみを確定します。128ビット書き込みは実行前に二つの64ビット値として順序付きで観測されます。停止・fault は RAM、vector、writeback を保持します。Xn/Vn の番号重複は有効で、pair 範囲のアドレス wrap は拒否します。`NeverDAArch64MemoryTests` は独立した `AArch64CrossPageCases.def` と `AArch64VectorMemoryCases.def` を使用します。

`NeverDAArch64StateTests` は両 privilege の完全状態の71箇所の読み取り、幅の正規化、欠落 reader、retry を検証します。`NeverDAArch64FPTests` は `AArch64FPCases.def` の原始命令で全 vector lane、packed arithmetic、scalar/vector FP、4種類の丸め、FZ/DN、累積 FPSR、context と拒否条件を確認します。`NeverDAArch64MemoryTests` は全 crossing offset、observer の順序・停止、拒否ページ、alias、復元後の vector input を検証します。`NeverDUnicornStateTransferTests` (`UnicornStateTransferCases.def`, `CapturesDeclaredWidthsWithoutStaleUpperBits`) は実際の実行後に全 scalar/vector read failure を注入します。利用不可の native transport は明示的に skip し、これらは native ARM64 KVM/WHP の実機証拠を代替しません。

`NeverDAArch64MemoryTests` は両権限レベルの Unicorn、KVM、WHP で18種類のスカラー／ペア命令を検証します。全ページ跨ぎ offset、符号と幅、observer 順序、第2ページの権限拒否／欠落、障害の明示的な消費と再試行、同一物理領域のエイリアス、エイリアス置換後の context 復元を含みます。変更前には合法なページ跨ぎ load の拒否を再現しました。利用不能な transport は明示的に skip します。Unicorn と cross-compile は ARM64 KVM/WHP 実機の証拠を代替しません。

`NeverDDriverGuardMetadataTests` (`DriverGuardCases.def`) は、フラグがゼロの無効状態の CFG メタデータ、両方のロードアドレスで保持されるフォールバックポインタ、不正なスロット/ターゲット、欠落した再配置を検証します。実行ケースは明示的な Unicorn/KVM/WHP と `driver-strict`、`checked-x64-v1` を使用し、利用できないバックエンドは個別にスキップします。`DriverPublicCLICases.def` は、互換性を維持する v1 C API と比較する CLI に `--backend unicorn` を指定します。ネイティブと `auto` の選択は独立した公開 API テストを維持し、ホスト API が利用できない場合に暗黙のフォールバックは行いません。

checked Unicorn は `MachineRunControl` を使い、ARM64 の保守、ゲスト実行、完全な状態読み出しを一つのステップ時間枠で処理します。`UC_HOOK_CODE` は命令入口で借用した停止トークンと期限を確認します。同期エンジン呼び出しは戻る前に hook の借用を解除しますが、マシンステップは状態公開まで制御を保持します。Unicorn と WHP は完全な CPU 状態を一時保存し、成功したステップの公開直前に同じ制御を確認します。WHP は準備前に時間枠を一度だけ作ります。確認済みの x64 CPU 例外は読み出し中の停止要求に優先します。読み出しが中止されると checked RAM トランザクションは投機的な書き込みを破棄し、非制限ソフトウェア契約は変わりません。 `MachineInterruptedError` は確認済みの中止をホストや状態読み出しの失敗と区別します。共通 checked CPU は `Stopped` または `Deadline` を返し、CPU/RAM を保持して再試行を許可します。同時に停止要求があっても実際の失敗は `BackendFailure` のままです。

状態読み出しの回帰テスト： `NeverDUnicornStateTransferTests`, `NeverDUnicornMachineControlTests`: `StopDuringCaptureCannotPublishAndAllowsRetry`, `ExpiredCaptureCannotPublishAndAllowsRetry`, `CompletedStoreCannotPublishCancelledCapture`, `StopDuringCaptureCannotHideRealGuestException`. `UnicornPublicCapture.CancellationKeepsTypedExitStateAndRAMConsistent`; `NeverDKvmStateTransferTests`: `PublicCancellationRetainsStateRAMAndFailurePriority`.

`NeverDUnicornMachineControlTests` は実際の x64 と ARM64 エンジンの両権限レベルで `UnicornMachineControlCases.def` の原始ストア命令を実行します。`RejectedEntryPreservesStateAndRAMAndAllowsRetry` はステップ前のキャンセル、実際のゲスト入口での停止または期限切れ、入力状態全体と RAM の保持、その後の一度の正常なストアを検証します。テスト専用の入口ラッパーはハイパーバイザーを必要とせず、ネイティブ ARM64/WHP の証拠にはなりません。

`RunDeadline::invoke` は WHP の停止済み・期限切れの実行をホスト呼び出し前に拒否し、キャンセル中も実際のホスト結果を保持し、借用した停止トークンを解放する前に割り込みコールバックの完了を確認します。KVM と WHP は、完全に取得した非公開状態を実行リースの所有スレッドで検証してから、同時に到着した停止や期限を分類します。実際のホスト・取得エラーと認証済み x64 CPU 例外が優先されます。通常の成功状態はキャンセル確認が終わるまで公開せず、確認済みの中断では投機的な CPU/RAM 効果を破棄して再試行を許可します。準備、ネイティブ実行、状態取得には単一のステップ猶予を使います。協調キャンセルを提供しますが、厳密な実時間上限は保証しません。

`NeverDRunControlTests` は移植可能な `NativeEntryTests.cpp` と、Windows で WHP を有効にした場合の `WhpEntryControlTests.cpp` を含みます。メモリ内のホストコールバックで、実行拒否、再試行、遅いキャンセル、実エラーの保持、完了結果の優先順位、確認済みコールバックの寿命を Hyper-V なしで検証します。`NeverDKvmRunTests` は所有スレッドでの完了、エラーの優先順位、再入拒否を確認します。実際の `NeverDKvmStateTransferTests` は `KvmStateTransferCases.def` の元の命令を実行します。`ActualCPUExceptionOutranksStopDuringCapture` と `PublicCPUExceptionOutranksStopDuringCapture` は実際のレジスタ/XSAVE 読み出し後に停止し、除算例外、元のコンテキスト、RAM、明示的な回復を保持します。Wine 上の Windows ABI による移植可能テストはスレッドと制御の証拠であり、ネイティブ WHP 実行の証拠ではありません。利用できないネイティブ経路は明示的にスキップします。

`windows-pe64-v1` は PEB/TEB、EXE TLS、名前付き Win32 API、明示的で非循環の起動 DLL グラフを備えた有界 Windows x64/ARM64 コンソールプロセスに対応します。DLL は名前／序数によるコード・データのインポート、DIR64 再配置、実際のローダーリストに対応します。DLL 入口／TLS、動的ロード、転送エクスポート、CRT/GUI、ユーザー SEH、スレッドは未完成です。ARM64 KVM/WHP のネイティブ実行証拠も未取得です。

入力総バイト数と全イメージ範囲はそれぞれ `memory_limit` に制限され、実行環境のマッピングも後者に含みます。準備は 65,536 レコード、64 MiB のメタデータ読み取り、名前長、共通期限で制限します。ホスト I/O の硬い時間保証はありません。独自 EXE→DLL→DLL は再配置、序数、共有データ、API ポインター、`MEM_IMAGE`、リスト、EXE TLS attach/detach を検証します。`NeverDWindowsProcessTests` はネイティブ Windows 対照、`NeverDPEProgramExportsTests` は不正メタデータと予算、`NeverDProcessPublicTests` は C ABI/CLI の一致を検証します。利用不能なバックエンドは明示的にスキップします。

Windows 仮想メモリに `VirtualAlloc`、`VirtualFree`、`VirtualProtect`、`VirtualQuery` と現在のプロセスの `FlushInstructionCache` を追加しました。OS 層が予約領域を所有し、コミット済みページ、権限、物理記憶域は `AddressSpace` が一元管理します。動的コードの書き換え、アクセス違反、メモリ予算の再利用をテストします。

`NeverDEmulationWindowsProcess` → `os/windows/process/`; `NeverDWindowsProcessTests` + `NeverDProcessPublicTests`: [windows-pe64-v1](process-emulation.md#windows-pe64-profile).

分離契約の回帰テストは、厳密・不足予算、部分ワード、両エンディアン、未アクセスメモリの同一性、アフィン保存領域の境界、遅れて到着する先行辺の上書き、既定の無効化を検証します。C API／CLI は v6 の互換性と無効ドメインを検査します。HighC と LLVMC を O0／O2 で実行し、戻り値、メモリ、スタック、保存状態を確認しますが、ネイティブ等価証明ではありません。
