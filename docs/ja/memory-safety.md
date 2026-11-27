**言語**: [English](../memory-safety.md) | [简体中文](../zh-CN/memory-safety.md) | [繁體中文](../zh-TW/memory-safety.md) | [日本語](memory-safety.md) | [한국어](../ko/memory-safety.md) | [Français](../fr/memory-safety.md) | [Deutsch](../de/memory-safety.md) | [Español](../es/memory-safety.md) | [Italiano](../it/memory-safety.md) | [Русский](../ru/memory-safety.md) | [العربية](../ar/memory-safety.md)

[← ドキュメント索引](README.md)

# メモリ安全性スキャン

`neverd scan <binary>` で両方の解析を実行します。個別に実行するには `--analyzer=audit` または `--analyzer=hunt` を指定します。[統合スキャンとレポート形式（英語）](../memory-safety-scan.md)。

NeverD は読み込んだバイナリに対して二系統のメモリ安全性解析を行い、構造化 JSON で報告します。どちらも形式に依存しないリフト済み IR 上で動くため、**PE/COFF・ELF・Mach-O は同等の第一級対象**です。発見が特定形式のスキャナやインポート表の後ろに隠れることはありません。

| トラック | コマンド | 報告内容 |
|----------|----------|----------|
| **監査（Audit）** | `neverd scan <binary> --analyzer=audit` | ヒープ寿命の欠陥と未初期化ローカルスタック読み取り |
| **ハント（Hunt）** | `neverd scan <binary> --analyzer=hunt` | 危険なコピー越境と記号的証拠／入力候補（完全な `process-input-v1` プランがある場合のみ `replayable=true`） |

エンジンは NeverD 内蔵の記号実行とビットベクトルソルバを証人生成と到達可能性に再利用します。外部ソルバ、VM、コンテナ依存はありません。

---

<!-- i18n-section: core-invariant -->

## 核心不変条件：失敗は閉じる

リフトされていない操作、ABI が引数を復元できなかった呼び出し、未解決の間接対象、または予算切れはすべて **UNKNOWN** であり、SAFE にはしません。容量を復元できない書き込み先も UNKNOWN です。strict lifting はそのままです。安全性層は、その上に保守的な判定だけを足します。

呼び出し効果は閉世界の意味論に従います。要約は前提条件と関連するすべての効果が既知の場合だけ適用されます。未知の効果や部分的にしか適用できない要約は UNKNOWN のままで、隙間を「効果なし」または「呼び出し成功」と仮定しません。

---

<!-- i18n-section: identity -->

## 形式ごとの識別契約

両トラックは lift パイプラインを必要とします（呼び出しごとの引数を復元するため）。被呼び出し名は NeverD の他部分と同じ識別ビューで付けます。デバッグ情報の探索順は変わりません。

| 形式 | デバッグ情報（優先度の高い順） | インポート / thunk 解決 |
|------|-------------------------------|-------------------------|
| **PE/COFF** | `--pdb`、デバッグディレクトリまたは隣接 `.pdb`、次いで MSVC `/MAP` | IAT スロットと `__imp_` thunk、序数インポート |
| **ELF** | イメージ内 DWARF、分割 `*.debug`、次いで GNU/LLD MAP | PLT stub をインポート名へ解決 |
| **Mach-O** | イメージ内 DWARF、隣接 `.dSYM`、次いで ld64 `-map` | dyld bind / 間接シンボルスロットと stub helper |

`--pdb` / `--map` は権威ある付属ファイルです。読めない場合はエラーであり、黙ってフォールバックしません。`--no-debug` はすべての形式でイメージだけを読みます。

PDB のプロシージャ署名は、値を返す確保関数と `void` の解放関数を区別するために使います。PDB のローカル変数とスタック型の本格的な復元は今も限定的です。正確なオブジェクトサイズを確定できない場合、ハントはフレーム／確保点のモデルへ落ち、サイズを捏造せず UNKNOWN を返します。

<!-- i18n-section: name-precedence -->

### 名前出所の優先順位

各発見は `name_source` を持ち、被呼び出し名の出所を次の優先順位で選びます。

1. `rename` — 呼び出し側が付けた改名
2. `import` — IAT（PE）、PLT（ELF）、または dyld-bind / stub（Mach-O）
3. `export` / `symbol` — イメージが既に表明したエクスポートまたは記号表の名前
4. `pdb` / `dwarf` / `map` — プレースホルダを確定するか、表明済みの名前と一致するデバッグ記号
5. `sig` — シグネチャ照合
6. `synthetic` — 無名ルーチン向けのプレースホルダ

DWARF が付けた静的リンクの `memcpy` は `dwarf`、インポートされた `memcpy` はどの形式でも `import` です。シグネチャ照合が、デバッガやインポート表が既に述べた名前を上書きすることはありません。

---

<!-- i18n-section: catalog -->

## シンクとソースのカタログ

カタログは設定可能な表であり、ハードコードされた集合ではありません。各 **シンク** は弱点クラス、役割（copy / format / alloc / free / realloc）、関係する引数スロット（宛先、送信元、長さ、容量）を宣言します。JSON の copy または format シンクは、実行可能な呼び出し effect も提供します。各 **ソース** は攻撃者の影響を受ける入力提供者です。

組み込みの行は [`SafetySinks.def`](../../include/neverd/safety/SafetySinks.def) と [`SafetySources.def`](../../include/neverd/safety/SafetySources.def) にあり、一般的な C ランタイムのコピー族（`memcpy`/`memmove`/`strcpy`/`strcat`/`strncpy`/`gets`/…）、明示的な宛先上限を持つ強化 `_chk` 変種、確保と解放族（`malloc`/`calloc`/`realloc`/`free`、operator `new`/`delete`）、任意の Win32 ヒープ API を覆います。入力源は POSIX（`getenv`、`read`、`recv`、`fgets`、`fread`、`scanf`、プログラム引数）**および** Win32（`GetCommandLineA/W`、`ReadFile`、`GetEnvironmentVariable*`）です。PE のハントが POSIX 入力だけに縛られることはありません。

形式ごとの綴りは同一エントリに折り畳みます。先頭のアンダースコアを剥がし（`_malloc`、`___strcpy_chk`）、mangling された operator new/delete は別名で照合します。

JSON の copy または format シンクで `effect` を省略した場合、適用条件は参照される最大の引数スロットから導出されます。copy はその正確な引数数を要求し、format シンクはその最小引数数から可変引数の上限までの呼び出しを受け入れます。省略可能な `effect` オブジェクトでは、`min_arity` と `max_arity`（または `"variadic"`）を使って、推論された copy の正確な引数数を超える追加 wrapper 引数を含む許容 arity 範囲を明示的に設定できます。`min_arity` は参照される最大の役割スロットに 1 を加えた値以上でなければならず、`formats` と `abis` は適用条件を制限します。呼び出しの引数数、オブジェクト形式、ABI のいずれかが一致しなければ要約は適用されず、閉世界規則により結果は UNKNOWN のままです。

仕様ファイルでカタログを拡張または上書きできます。

```bash
neverd scan --sinks extra_sinks.json --sources extra_sources.json app
```

```json
{ "sinks": [
    { "name": "my_copy", "kind": "copy", "dst": 0, "src": 1, "len": 2 },
    { "name": "my_format", "kind": "format", "dst": 0, "fmt": 2,
      "effect": { "min_arity": 3, "max_arity": "variadic",
                  "formats": ["elf"], "abis": ["sysv"] } }
  ],
  "sources": [
    { "name": "my_read", "out": 1, "return_tainted": true }
  ]
}
```

カスタムソースの `out` と `return_tainted` は発見用メタデータにすぎません。これらは、実行可能なメモリ、戻り値、taint の effect を確立しません。現在のソーススキーマには、その意味論に必要な型付きの成功条件、変更、形式、ABI の契約がないため、カスタムソースの effect に依存する解析は UNKNOWN のままです。組み込みソースは影響を受けず、適用条件を検査した型付き記述子が引き続き実行可能な effect を提供します。

宛先引数だけを持つ境界なしのカスタムシンクは、同名のソースエントリから推論されません。`gets` に似たカスタムシンクは `"unbounded": true` を明示的に有効化する必要があります。同名をソースカタログに追加しても実行可能な effect は付与されず、矛盾する送信元/長さフィールドはトランザクション単位で拒否されます。

---

<!-- i18n-section: copy-overflow -->

## ハント：コピー越境の判定

各コピーシンクについて、ハントは宛先容量を次の順で復元します。デバッグ宣言の配列サイズ、既知サイズのヒープ確保点、健全なスタックフレーム上界。書き込み長を決める引数は、スタックスロットの spill/reload を辿る後方 SSA 走査で分類します。

- **定数長** が正確な容量内なら SAFE です。定数越境は、裏付けられた経路でシンクへ到達できる場合だけ UNSAFE となり、それ以外は UNKNOWN のままです。
- **強化** `_chk` コピーは実行時の宛先上界を持ちます。要求が拒否されるか、その上界が復元済みオブジェクト内に収まると証明できれば SAFE、オブジェクト外への書き込みが実行可能なら UNSAFE、上界が未復元または結論不能なら UNKNOWN です。
- **証明可能な有界長**（長さを返す呼び出し、マスク、クランプ）は解法前に退き、理由を記録します。宛先サイズが正確な場合だけ SAFE で、包含領域の上界しかない場合は UNKNOWN のままです。
- **攻撃者の影響を受ける長さ** で容量が既知ならビットベクトルソルバに渡します。容量を超える長さが充足可能なら UNSAFE です。候補を再生できるのは完全な `process-input-v1` プランを作れる場合だけです。当初の対象は正確なリテラル環境値と、最初の標準入力消費が返すバイト列までです。argv、ファイル、ネットワーク、カスタム、または曖昧な入力は理由付きで再生不能のままです。
- それ以外（未知の長さまたは未知の容量）は UNKNOWN。

復元された容量は常に実オブジェクトサイズの **上界** なので、証明された越境は偽陽性になりません。

<!-- i18n-section: formatted-input -->

### 書式付き入力

`scanf`/`fscanf` とそのバージョン付き表記では、読み取り可能な定数書式が、抑制されていない各変換を実際の可変引数出力引数へ対応付けます。境界なしの `%s`/`%[` 出力は後続の文字列使用へ taint を伝播し、数値および文字出力は、出力ポインタ値そのものではなく、書き込まれたオブジェクトからロードされる値へ taint を伝播します。`sscanf` がこれらの effect を伝播するのは、入力文字列がすでに攻撃者の影響を受けている場合だけです。`%Ns`/`%N[` のような有界テキスト出力は、終端文字を含む `MaxBytes` extent とともに taint を伝播し、ワイド文字の変種はプラットフォームの `wchar_t` 幅を使ってそのバイト extent を計算します。抑制された変換、余分な引数、位置依存または未対応の書式、および `%n` は、推測せず UNKNOWN のままにします。

<!-- i18n-section: formatted-output -->

### 書式付き出力

`snprintf`/`vsnprintf` の最大書き込み長と、強化版 `_chk` のコンパイラ由来の宛先オブジェクトサイズは別の境界です。リテラルと `%%` のみの信頼できる定数書式では、終端 NUL を含む出力長が正確に分かり、コピー sink と同じヒープ／スタックモデルで検査します。最大書き込みが正確な容量内なら SAFE、リテラル出力が宛先を超えるなら UNSAFE です。値変換は引数の出力長を証明するまで UNKNOWN です。高信頼度 UNSAFE には LowIR パス再生による到達可能性の証明も必要です。攻撃者が制御する書式は切り詰めとは別に `format_string` として報告します。

---

<!-- i18n-section: heap-lifetime -->

## 監査：ヒープ寿命の判定

各確保について、監査は CFG 上でハンドルを追跡し（スタック spill/reload を含む）、エスケープ要約（返却、非スタック番地への格納、不透明な被呼び出しへの引き渡し）を適用します。

- **リーク** — ハンドルが解放もエスケープもされていない。
- **二重解放** — ある経路で二回目の解放が初回の後に到達可能。
- **解放後使用** — 解放後に間接参照または不透明な使用が到達可能。

確保と解放の **ラッパ** は関数ごとのエスケープ要約で認識するので、`malloc`/`free` の転送関数が欠陥を隠しません。相互排他分岐上の解放は二重解放として報告しません。

ヒープ状態機械はまず候補イベント列（確保、解放、使用、または返却による出口）を出します。二回目の走査がその列を記号 LowIR 経路上で順に再生し、経路述語の充足可能性を証明して初めて、発見は高信頼度の UNSAFE になります。LowIR の欠落、不透明な操作、要約のない呼び出し、ソルバの不確定、探索上限は、いずれも候補を UNKNOWN へ格下げします。保守的な may-alias によるメモリ havoc は別に追跡するため、通常のスタックフレームへの格納が、本来正確な到達可能性の根拠を無効にすることはありません。

---

<!-- i18n-section: stack-initialization -->

## 監査：ローカルスタックの初期化

関数入口のスタックポインタより下にあるローカルスロットへの全幅書き込みと後続ロードを追跡します。先行する初期化があり得ないロードは `uninitialized_read` です。高信頼度 UNSAFE には LowIR パス再生でロードの到達可能性を確認します。条件付き初期化、部分書き込み、スロットアドレスのエスケープなど不確実な定義は UNKNOWN のままです。入口スタックポインタ以上の呼び出し元所有引数スロットは対象外です。

---

<!-- i18n-section: reachability -->

## 既知エントリからの手続き間到達可能性

各発見には、混同してはならない 3 つの独立した主張があります。

| フィールド | 問い | 値 |
|------------|------|----|
| `verdict` | ローカルな安全性解析は操作について何を証明したか | `SAFE`, `UNSAFE`, `UNKNOWN` |
| `reachability.status` | その関数は既知のネイティブエントリから復元した制御経路上にあるか | `REACHABLE`, `UNREACHABLE`, `UNKNOWN` |
| `reachability.attacker_control` | 引数スライスはこの発見での攻撃者影響について何を証明したか | `TAINTED`, `BOUNDED`, `UNKNOWN` |

到達可能性は追加証拠であり、発見の `verdict`、集約判定、CLI 終了コードを
書き換えません。そのため、ローカルに証明された越境が
`verdict=UNSAFE` と `reachability.status=UNREACHABLE` を同時に持つことが
あります。実行可能な攻撃経路を必要とする利用側は両方を検査します。

ルートは、認識済みアプリケーションエントリ（`application`、例：`main`、
`WinMain`）、イメージエントリ（`image`）、エクスポート関数（`export`）です。
同一関数が複数の識別を持つ場合の決定的な優先順は `application`、`image`、
`export` です。`reachability.entry` は `va`、`name`、`kind` を記録します。
到達可能な非ルート発見では、`call_chain` も正確な内部エッジの最短かつ決定的な
経路を示し、各エッジは `caller_va`、呼び出し位置 `call_va`、`callee_va`、
`direct` または `indirect` の `kind` を持ちます。

`UNREACHABLE` は、ルートが存在し、内部呼び出しインベントリが完全で、深さ予算を
使い切っておらず、それでも経路がない場合だけ出力します。他の正の証拠でまだ到達して
いない関数では、ルート欠落、重複・曖昧な関数識別、不整合な CFG／呼び出し
インベントリ、未解決の実行可能な内部対象、深さ予算切れによって否定証明ができず、
`reachability.status=UNKNOWN` となり、該当時は `reason` と
`budget_hit` を伴います。未知 ABI、引数幅不一致、可変長部分だけの slot、不完全な
スライス、深さまたは要約予算切れでも、未証明の攻撃者制御は UNKNOWN のままです。
すでに証明した事実は有効なままで、伝播を推測で補いません。

---

<!-- i18n-section: budgets -->

## 予算、出力、バインディング

ハントの探索とソルバは予算で制限します（`--max-paths`、`--max-steps`、`--max-loop`、`--solver-conflicts`）。手続き間解析では、`max_call_depth` が既知エントリからの内部呼び出しエッジ数を、`max_summary_iterations` が攻撃者制御の固定点反復数を制限します。既定値はそれぞれ 64 エッジと、有効な深さ制限に 1 を加えた反復数です。予算切れは上記のとおり fail closed です。`max_call_depth` の枯渇では未到達関数が `status=UNKNOWN` になり得ます。`max_summary_iterations` の枯渇は構造的な証人を消さないため、`status=REACHABLE` と `attacker_control=UNKNOWN`、`budget_hit=true` が共存できます。`neverd scan` は JSON を出力し、`-o` で出力先を指定できます。終了コードは SAFE が `0`、UNSAFE が `2`、UNKNOWN またはエラーが `1` です。

どの公開インターフェイスでも 0 はエンジン既定値を選びます。

| インターフェイス | 制御深さ | 攻撃者要約 |
|------------------|----------|------------|
| CLI（`scan`） | `--max-call-depth <n>` | `--max-summary-iterations <n>` |
| C（`neverd_safety_options`） | `max_call_depth` | `max_summary_iterations` |
| Python（`Session.scan()`） | `max_call_depth=<n>` | `max_summary_iterations=<n>` |

C の呼び出し側は `neverd_safety_options` をゼロ初期化し、
`struct_size=sizeof(neverd_safety_options)` を設定します。以前の構造体サイズでは既定値を
維持します。Python は両値を符号なし 32-bit 整数として検証します。

C では `neverd_session_scan_json`、Python では `Session.scan()` を呼び出します。C の解析上限は `neverd_scan_options.analysis` で渡します。

<!-- i18n-section: finding-schema -->

### 発見スキーマ

```json
{
  "class": "buffer_overflow",
  "function": "parse_header",
  "name": "strcpy",
  "name_source": "import",
  "call_va": "0x11a4",
  "source": "reader.c:42",
  "sink": "strcpy",
  "arg_index": 1,
  "flow": "TAINTED",
  "verdict": "UNSAFE",
  "confidence": "HIGH",
  "capacity": 16,
  "capacity_kind": "exact",
  "corroboration": "path predicate and overflow are jointly satisfiable",
  "reachability": {
    "status": "REACHABLE",
    "attacker_control": "TAINTED",
    "budget_hit": false,
    "entry": { "va": "0x1000", "name": "main", "kind": "application" },
    "call_chain": [
      { "caller_va": "0x1000", "call_va": "0x1080",
        "callee_va": "0x1100", "kind": "direct" }
    ]
  },
  "evidence": {
    "concrete_input": { "copy_length": "17", "argv[1]": "16 bytes" },
    "candidate_values": [
      { "name": "copy_length", "value": "17" },
      { "name": "argv[1]", "value": "16 bytes" }
    ],
    "replayable": false,
    "replay": {
      "adapter": "process-input-v1",
      "reason": "argv input is not supported by process-input-v1"
    },
    "symbolic_model": [
      { "id": 0, "name": "copy_len", "width": 64,
        "value_hex": "0x11", "origin": "input" }
    ]
  }
}
```

`replayable` は独立した約束ではなく導出された証拠です。`replay` に `process-input-v1` アダプタ向けの完全な入力プランがある場合に限り真です。プランには正確な環境バイト、使用する場合は最初の標準入力バイト列、ソルバ割り当て ID から各入力への対応を記録し、作れない場合は `replay.reason` が理由を示します。再生と到達可能性のフィールドは追加的で、トップレベルの `schema_version` は `1` のままです。

---

<!-- i18n-section: strict-publication -->

## 厳密な実行時ガードと認証済み公開

`binary-sanitizer-v1` は audit/hunt 判定を変えない独立した実験的変更トランザクションです。完了した厳密 hunt は全検出を、正確な残容量と一致するコンパイラ呼び出し点情報を持つ、一意なカウント付き書き込みの発生に対応させる必要があります。未対応検出、古い／曖昧な識別、不完全なリフト、予算枯渇、ガード生成エラー、対象／署名制約は全体を拒否し、部分成功はありません。再配置可能オブジェクト、動的ライブラリ、ユニバーサル Mach-O、ガード対象の Mach-O bundle メンバーなども拒否します。

公開 API は C の `neverd_session_sanitize`、CLI の `neverd patch --sanitize=strict`、Python の `Session.sanitize` です。認証済み公開が必要な C 呼び出し元は先に `neverd_sanitize_publication_abi_version()` を確認します。全 API が完全で整合した publication receipt v1 を成功条件とします。非 Darwin では入力の認証と正規化後、リフト・ガード生成・候補作成・名前空間変更の前に `UNSUPPORTED_TARGET` を返します。

Darwin の成功形態は二つです。`CREATE_EXCLUSIVE` は同じディレクトリの新候補を、存在しない宛先へ一回の原子的な上書き禁止操作で公開します。receipt は名前空間の原子性、排他的作成、実際のオペランドの結び付きを証明しますが、置換 CAS やクラッシュ耐久性は保証しません。読み取り専用の `NO_CHANGE` は空のガード計画で保持中のロード元オブジェクトを再認証し、`NOT_PUBLISHED` を報告します。公開保証やオペランドの結び付きは主張しません。ガード付き計画はロード元を出力にできず、別の既存宛先も置換しないため、新しいパスが必要です。不確定な公開や不完全な最終 receipt は失敗です。宛先が存在する可能性があるので、使用・再試行・削除の前に確認します。

ソースのバイト列を session の外部ダイジェストと照合し、保持した記述子で観測します。ソースと宛先ディレクトリのオブジェクトは処理中に保持しますが、メタデータ／識別の観測は最初のロード以降の不変性を証明しません。開いたディレクトリは改名され得るため、receipt は保持したオブジェクト内の公開のみを認証し、元のパスが処理中や返却後も同じ対象を指すことは証明しません。永続的で独立検証可能なパスの結び付きではなく、後で開き直す側は外部アンカーを保ち再認証する必要があります。Darwin rename は候補記述子でなくパスを使うため、宛先ディレクトリは実効 uid 所有、拡張 ACL なし、グループ／他者書き込みなし、通常の POSIX 所有権を保証するボリューム上である必要があります。候補作成と公開前に再確認します。同権限プロセス、root/DAC 回避、敵対的ファイルシステム／サーバーは保証外で、カーネル・VFS・ファイルシステムの申告した意味に依存します。

---

<!-- i18n-section: native-replay -->

## ネイティブプロセス再生：Phase 0 のみ

プラットフォーム中立の `process-replay-v1` 計画と実行調整器は将来の認証済み実行の要件を定めますが、現在それを実行できるホストはありません。`NativeProcessReplayAdapter` は C++ の可用性／ファクトリ境界であり、C、Python、CLI、JSON の実行 API ではありません。

問い合わせは完全な計画、実行制限、絶対実行ファイルロケーター、一意な物理呼び出し点マップを検証するだけです。対象を開かず、バックエンドを呼ばず、プロセスも開始しません。ロケーターは将来の検索情報であり、認証後のパス実行権限に変えてはなりません。隔離、識別、入力仲介、発生の認証、制限、後始末のいずれかが欠ければ `Available` と全能力は false、ファクトリは操作表なしでエラーを返します。

全ホストが現在この状態です。Linux は信頼できる静的 ELF 計装、永続監督、完全な隔離・制限・実行時認証が未完成です。macOS の対応公開機構では保持オブジェクト実行、対象コード前の任意対象サンドボックス、競合のないプロセスツリー隔離を揃えられません。他プラットフォームも未対応です。単体テストの操作表は調整器の契約を試すだけで、ネイティブ可用性の証拠にはなりません。

---

<!-- i18n-section: scope -->

## 偽陽性の境界と範囲

- 容量は正確値または実オブジェクトサイズの上界なので、UNSAFE は実際の越境を表します。正確な宣言サイズがなく、包含領域の上界だけでは安全性を証明できない場合は UNKNOWN です。
- 長さ制限付きコピーは解法前に退き `skipped` に数えられます。正確な容量なら SAFE を証明でき、上界だけなら UNKNOWN のままです。
- カタログ済みのワイド文字コピーと追加コピーは、要素幅と既存の宛先長を復元できるまで UNKNOWN です。出力引数型アロケータと条件付き `realloc` の所有権も、ハンドル遷移を証明できなければ UNKNOWN のままです。
- **Phase 1**: 本リリースは三形式で sink カタログ、引数事前フィルタ、スタック／グローバルのコピー越境探索、ヒープ寿命監査、ローカルスタック未初期化読み取り、書式文字列検査を提供します。全テストホストで PE、ELF、Mach-O × x86-64、AArch64 の登録済み fixture が必須です。
- より豊富な PDB スタック型と追加のプラットフォームアロケータは今後の追加範囲です。正確な要約がなければ UNKNOWN を維持します。
- 現在のスライスは既知エントリ、構造的な手続き間到達可能性、攻撃者パラメータの単調伝播を対象にします。独立した実験的アダプタ `lowir-concolic-v1` は、必須のネイティブ形式／アーキテクチャ行列で、レジスタ seed によるリプレイ検証済みのブランチ反転を提供します。これは常に非網羅的であり、安全性 verdict を変更しません。実験的 `binary-sanitizer-v1` は Darwin で全サイトを保護できなければ拒否する counted-write ガードと認証済み公開を提供しますが、receipt が認証するのはトランザクション中に保持したディレクトリ object であり、元の pathname の永続的かつ再検証可能な binding ではありません。より広い `process-replay-v1` は引き続き plan、coordinator、可用性の fail-closed な Phase 0 境界だけで、ネイティブ replay を実行するホストはありません。
