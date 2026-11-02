**言語**: [English](../solver.md) | [简体中文](../zh-CN/solver.md) | [繁體中文](../zh-TW/solver.md) | [日本語](solver.md) | [한국어](../ko/solver.md) | [Français](../fr/solver.md) | [Deutsch](../de/solver.md) | [Español](../es/solver.md) | [Italiano](../it/solver.md) | [Русский](../ru/solver.md) | [العربية](../ar/solver.md)

[← ドキュメント索引](README.md)

# Bitvector 証明バックエンド

NeverD は既定で組み込み bitvector solver を使います。厳密な MBA 導出は汎用 solver に依存しません。式合成では等価性を証明した候補だけを採用し、反例や未確定の問い合わせでは元の式を保持します。

組み込み乗算回路は、両因子の既知のゼロ下位ビットを除いて残りの積を構成し、出力の全ビットを復元します。任意の Z3 アダプターも、偶数定数の係数が実際に観測する幅で積を符号化します。どちらも全幅の等価性照会、独立した入力モデル、既存のリソース制限を維持します。CLI と GUI のソース最適化は同じ組み込み回路を使用し、追加のスイッチは不要です。

<!-- i18n-section: builtin-comparisons -->

## 組み込み比較回路

8 ビットを超える比較は上位半分を先に比較し、それらが等しい場合に下位半分の結果を使います。小さい部分は減算のキャリーを使います。部分レジスターの更新で下位ビットだけが変わる場合、共有エンコーダーは上位部分のゲートを再利用できます。符号付き比較は引き続き両方の符号ビットを反転します。式の意味と既存の資源制限は変わらず、予算を使い切ると `Unknown` を返します。

`BitBlaster.WidePredicatesAgreeWithTheEvaluator` は、8〜256 ビットから選んだ幅で符号付き・符号なし述語を式評価器と照合します。奇数幅、境界の隣接値、64 ビットを超える有効ビットを含み、誤った出力値も排除します。部分カウンターのテストは完全なクエリ、反例モデル、ゲート予算の枯渇を検証します。比較結果を保持するネイティブループの回帰テストは、全レジスターとフラグを観測して比較と更新の両順序を証明し、変更された元のループ本体を拒否します。

<!-- i18n-section: pristine-encoding -->

## 探索前のエンコードの複製

`BitVectorSolver::cloneEncoding()` は SAT 探索を一度も試みていない完全なエンコードを複製します。探索後やエンコード失敗時には null を返します。各コピーは変更可能な節、根での伝播、ゲート、ビット対応を独立して所有し、変数順序、ゲート計上、ソルバー設定を維持します。コンテキストは両ソルバーより長く生存する必要がありますが、元のソルバーは独立して変更または破棄できます。

SAT エンジンはリテラルごとの監視リストに四つの要素をインラインで保持し、長いリストは動的に拡張します。短いリストの構築、検索前コピー、破棄に伴う個別割り当てを減らします。伝播順序、節の内容、独立した所有権、全作業上限は変わりません。

<!-- i18n-section: context-finite-proofs -->

## 同一コンテキスト内の完了した証明

ネイティブ独立性検査器は有限領域キャッシュを、ノードを追加する実際のシンボリックコンテキストに結び付けます。小さなキーは正確な述語、順序付き射影、値数の上限を保持します。再利用できるのは完了した数値領域または証明済みの非一意性だけで、ミス時には完全な列挙と最後の除外証明が必要です。キャッシュの存続中、コンテキストと既存ノードの意味は不変でなければなりません。準備済みトークンは所有者をまたがず、同じアドレスの新しい所有者にも渡せません。記憶量の上限は既存のワード数で管理します。他の利用者は構造キーと変数名変更による再利用を維持します。

<!-- i18n-section: z3-build -->

## オプションの Z3 ビルド

Z3 を有効にすると、CMake は `FetchContent` で固定された 4.13.3 のソース revision を取得し、NeverD と静的ライブラリをビルドします。システムへの Z3 インストールは不要です。

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_Z3=ON -DNEVERD_BUILD_SOLVER_BENCH=ON
python scripts/build.py build-release --parallel 4 --target neverd \
  NeverDSolverTests NeverDSymbolicTests neverd-solver-bench
```

既定の `NEVERD_Z3_PROVIDER=FETCH` は初回 configure でソースを取得し、以降は build の `_deps` を再利用します。Z3 の CLI、テスト、例、ドキュメント、language binding はビルドしません。Z3 の Python generator は NeverD が既に必要とする Python を使います。インストール済みライブラリを使う場合は `-DNEVERD_Z3_PROVIDER=SYSTEM`、必要なら `-DZ3_ROOT=/path/to/prefix` を指定します。開発ファイルがなければ失敗し、provider を暗黙に切り替えません。オフラインでは `-DFETCHCONTENT_SOURCE_DIR_NEVERD_Z3=/path/to/z3` にローカル checkout を指定できます。

既定の `NEVERD_ENABLE_Z3=OFF` では Z3 を取得・検索・リンクしません。利用不能 backend を明示的に要求すると、fallback せず失敗します。

<!-- i18n-section: synthesis -->

## 証明ゲート付き式合成

```sh
build-release/bin/neverd simplify --synthesize --solver=z3 \
  --solver-timeout-ms=1000 --json '(x >> 4) + ((x >> 2) >> 2)'
```

既定は `--solver=builtin` です。solver 選択には `--synthesize` が必要で、通常の MBA simplifier は Z3 を呼びません。Z3 timeout は式翻訳を除く各検査に適用されます。キャンセルは協調的であり、厳密な wall-clock deadline ではありません。0 は既定の 1000 ms を選び、`--exhaustive` は制限を外します。SAT conflict/propagation/watch visit 制限は組み込み backend 専用で、Z3 使用時には拒否されます。Z3 検査は proof-query counter を増やし、組み込み SAT work counter は 0 のままです。

C API は `neverd_synthesize_options` に `solver_backend` と `solver_timeout_ms` を追加します。サイズ境界付き reader は古い caller のため組み込み backend を維持します。`neverd_solver_backend_available()` は build capability を返し、Python でも `synthesize_expression(..., solver='z3', solver_timeout_ms=1000)` を選べます。現時点で対象となるのは式合成だけです。concolic 実行、安全性解析、既存 IR 最適化の既定 solver policy は変更されません。内部利用者は意味論 simplifier の proof callback に Z3 verifier を渡せます。

```python
from neverd_plugin import synthesize_expression

result = synthesize_expression(
    '(x >> 4) + ((x >> 2) >> 2)', solver='z3', solver_timeout_ms=1000
)
```

<!-- i18n-section: checks -->

## 独立検査と query export

Z3 有効時の `NeverDSolverTests` は独立した式評価と backend 間検査を含みます。参照式を直接構築するため、NeverD の式 builder の bug がテスト両辺を同時に簡約することはありません。Z3 無効 build は oracle ケースを明示的に skip しつつ、backend unavailable 契約を検査します。

スケール付き積のテストでは、独立した筆算乗算回路と整数演算により、組み込み回路を最大 257 ビットまで検証します。Z3 テストは独立した全幅の符号化との一致を証明し、入力の上位ビットと意図的に誤らせた出力の最上位ビットも確認します。

```sh
build-release/bin/NeverDSolverTests --gtest_brief=1
build-release/bin/neverd-solver-bench \
  tools/neverd-bench/solver-corpus.txt --width=32 --repeat=5 \
  --backend=both --timeout-ms=1000 --max-conflicts=10000 \
  --dump-dir=/tmp/neverd-queries > /tmp/neverd-solver-results.json
```

コメント以外の各行は `original ; candidate` です。セミコロンがなければ MBA simplifier の結果を候補として使います。ツールは判定、model replay、時間を報告します（session 構築・翻訳・solver 実行を含み、parse、MBA 簡約、export、teardown は除外）。各反復で新しい solver を使います。SAT model は式 evaluator 上で差分を再現する必要があります。相反する確定判定、不正 query/model は実行失敗です。`unknown` は記録されますが、等価性の証明ではありません。

SMT-LIB export には等価に符号化された DAG、恒久 assertion、最後の query assumptions が含まれ、`z3 query-N.smt2` で再実行できます。resource limit と solver version を記録してください。backend の budget 単位は異なるため、同一作業量ではなく上限付き workload の比較です。bitvector proof は式言語の total fixed-width semantics を使います。machine exception、memory effect、LLVM poison は lifting/translation boundary の責任であり、式の証明だけでは保証しません。

未探索の符号化のコピーは、自身の決定キューから根レベルで割り当て済みの変数を除き、活性度と番号による厳密なヒープ順序を再構築します。割り当てと節は保持され、根の事実はすべてのバックトラックで有効です。元の所有権、実際の決定、完全なモデル、すべての探索予算は変わりません。

SMT-LIB の DAG は意味的に等価な符号化であり、上記の正確な縮小幅の商を含む場合があります。元の DAG と構造が同一であるとは限りません。
