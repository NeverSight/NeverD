**言語**: [English](memory-safety.md) | [简体中文](memory-safety.zh-CN.md) | [繁體中文](memory-safety.zh-TW.md) | [日本語](memory-safety.ja.md) | [한국어](memory-safety.ko.md) | [Français](memory-safety.fr.md) | [Deutsch](memory-safety.de.md) | [Español](memory-safety.es.md) | [Italiano](memory-safety.it.md) | [Русский](memory-safety.ru.md) | [العربية](memory-safety.ar.md)

[← ドキュメント索引](README.ja.md)

# メモリ安全性の監査とハント

NeverD は読み込んだバイナリに対して二系統のメモリ安全性解析を行い、構造化 JSON で報告します。どちらも形式に依存しないリフト済み IR 上で動くため、**PE/COFF・ELF・Mach-O は同等の第一級対象**です。発見が特定形式のスキャナやインポート表の後ろに隠れることはありません。

| トラック | コマンド | 報告内容 |
|----------|----------|----------|
| **監査（Audit）** | `neverd audit <binary>` | ヒープ寿命の欠陥と未初期化ローカルスタック読み取り |
| **ハント（Hunt）** | `neverd hunt <binary>` | 危険なコピー越境と記号的証拠／入力候補（完全な `process-input-v1` プランがある場合のみ `replayable=true`） |

エンジンは NeverD 内蔵の記号実行とビットベクトルソルバを証人生成と到達可能性に再利用します。外部ソルバ、VM、コンテナ依存はありません。

---

## 核心不変条件：失敗は閉じる

リフトされていない操作、ABI が引数を復元できなかった呼び出し、未解決の間接対象、または予算切れはすべて **UNKNOWN** であり、SAFE にはしません。容量を復元できない書き込み先も UNKNOWN です。strict lifting はそのままです。安全性層は、その上に保守的な判定だけを足します。

呼び出し効果は閉世界の意味論に従います。要約は前提条件と関連するすべての効果が既知の場合だけ適用されます。未知の効果や部分的にしか適用できない要約は UNKNOWN のままで、隙間を「効果なし」または「呼び出し成功」と仮定しません。

---

## 形式ごとの識別契約

両トラックは lift パイプラインを必要とします（呼び出しごとの引数を復元するため）。被呼び出し名は NeverD の他部分と同じ識別ビューで付けます。デバッグ情報の探索順は変わりません。

| 形式 | デバッグ情報（優先度の高い順） | インポート / thunk 解決 |
|------|-------------------------------|-------------------------|
| **PE/COFF** | `--pdb`、デバッグディレクトリまたは隣接 `.pdb`、次いで MSVC `/MAP` | IAT スロットと `__imp_` thunk、序数インポート |
| **ELF** | イメージ内 DWARF、分割 `*.debug`、次いで GNU/LLD MAP | PLT stub をインポート名へ解決 |
| **Mach-O** | イメージ内 DWARF、隣接 `.dSYM`、次いで ld64 `-map` | dyld bind / 間接シンボルスロットと stub helper |

`--pdb` / `--map` は権威ある付属ファイルです。読めない場合はエラーであり、黙ってフォールバックしません。`--no-debug` はすべての形式でイメージだけを読みます。

PDB のプロシージャ署名は、値を返す確保関数と `void` の解放関数を区別するために使います。PDB のローカル変数とスタック型の本格的な復元は今も限定的です。正確なオブジェクトサイズを確定できない場合、ハントはフレーム／確保点のモデルへ落ち、サイズを捏造せず UNKNOWN を返します。

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

## シンクとソースのカタログ

カタログは設定可能な表であり、ハードコードされた集合ではありません。各 **シンク** は弱点クラス、役割（copy / format / alloc / free / realloc）、関係する引数スロット（宛先、送信元、長さ、容量）を宣言します。JSON の copy または format シンクは、実行可能な呼び出し effect も提供します。各 **ソース** は攻撃者の影響を受ける入力提供者です。

組み込みの行は [`SafetySinks.def`](../include/neverd/safety/SafetySinks.def) と [`SafetySources.def`](../include/neverd/safety/SafetySources.def) にあり、一般的な C ランタイムのコピー族（`memcpy`/`memmove`/`strcpy`/`strcat`/`strncpy`/`gets`/…）、明示的な宛先上限を持つ強化 `_chk` 変種、確保と解放族（`malloc`/`calloc`/`realloc`/`free`、operator `new`/`delete`）、任意の Win32 ヒープ API を覆います。入力源は POSIX（`getenv`、`read`、`recv`、`fgets`、`fread`、`scanf`、プログラム引数）**および** Win32（`GetCommandLineA/W`、`ReadFile`、`GetEnvironmentVariable*`）です。PE のハントが POSIX 入力だけに縛られることはありません。

形式ごとの綴りは同一エントリに折り畳みます。先頭のアンダースコアを剥がし（`_malloc`、`___strcpy_chk`）、mangling された operator new/delete は別名で照合します。

JSON の copy または format シンクで `effect` を省略した場合、適用条件は参照される最大の引数スロットから導出されます。copy はその正確な引数数を要求し、format シンクはその最小引数数から可変引数の上限までの呼び出しを受け入れます。省略可能な `effect` オブジェクトでは、`min_arity` と `max_arity`（または `"variadic"`）を使って、推論された copy の正確な引数数を超える追加 wrapper 引数を含む許容 arity 範囲を明示的に設定できます。`min_arity` は参照される最大の役割スロットに 1 を加えた値以上でなければならず、`formats` と `abis` は適用条件を制限します。呼び出しの引数数、オブジェクト形式、ABI のいずれかが一致しなければ要約は適用されず、閉世界規則により結果は UNKNOWN のままです。

仕様ファイルでカタログを拡張または上書きできます。

```bash
neverd hunt --sinks extra_sinks.json --sources extra_sources.json app
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

## ハント：コピー越境の判定

各コピーシンクについて、ハントは宛先容量を次の順で復元します。デバッグ宣言の配列サイズ、既知サイズのヒープ確保点、健全なスタックフレーム上界。書き込み長を決める引数は、スタックスロットの spill/reload を辿る後方 SSA 走査で分類します。

- **定数長** が正確な容量内なら SAFE です。定数越境は、裏付けられた経路でシンクへ到達できる場合だけ UNSAFE となり、それ以外は UNKNOWN のままです。
- **強化** `_chk` コピーは実行時の宛先上界を持ちます。要求が拒否されるか、その上界が復元済みオブジェクト内に収まると証明できれば SAFE、オブジェクト外への書き込みが実行可能なら UNSAFE、上界が未復元または結論不能なら UNKNOWN です。
- **証明可能な有界長**（長さを返す呼び出し、マスク、クランプ）は解法前に退き、理由を記録します。宛先サイズが正確な場合だけ SAFE で、包含領域の上界しかない場合は UNKNOWN のままです。
- **攻撃者の影響を受ける長さ** で容量が既知ならビットベクトルソルバに渡します。容量を超える長さが充足可能なら UNSAFE です。候補を再生できるのは完全な `process-input-v1` プランを作れる場合だけです。当初の対象は正確なリテラル環境値と、最初の標準入力消費が返すバイト列までです。argv、ファイル、ネットワーク、カスタム、または曖昧な入力は理由付きで再生不能のままです。
- それ以外（未知の長さまたは未知の容量）は UNKNOWN。

復元された容量は常に実オブジェクトサイズの **上界** なので、証明された越境は偽陽性になりません。

### 書式付き入力

`scanf`/`fscanf` とそのバージョン付き表記では、読み取り可能な定数書式が、抑制されていない各変換を実際の可変引数出力引数へ対応付けます。境界なしの `%s`/`%[` 出力は後続の文字列使用へ taint を伝播し、数値および文字出力は、出力ポインタ値そのものではなく、書き込まれたオブジェクトからロードされる値へ taint を伝播します。`sscanf` がこれらの effect を伝播するのは、入力文字列がすでに攻撃者の影響を受けている場合だけです。`%Ns`/`%N[` のような有界テキスト出力は、終端文字を含む `MaxBytes` extent とともに taint を伝播し、ワイド文字の変種はプラットフォームの `wchar_t` 幅を使ってそのバイト extent を計算します。抑制された変換、余分な引数、位置依存または未対応の書式、および `%n` は、推測せず UNKNOWN のままにします。

---

## 監査：ヒープ寿命の判定

各確保について、監査は CFG 上でハンドルを追跡し（スタック spill/reload を含む）、エスケープ要約（返却、非スタック番地への格納、不透明な被呼び出しへの引き渡し）を適用します。

- **リーク** — ハンドルが解放もエスケープもされていない。
- **二重解放** — ある経路で二回目の解放が初回の後に到達可能。
- **解放後使用** — 解放後に間接参照または不透明な使用が到達可能。

確保と解放の **ラッパ** は関数ごとのエスケープ要約で認識するので、`malloc`/`free` の転送関数が欠陥を隠しません。相互排他分岐上の解放は二重解放として報告しません。

ヒープ状態機械はまず候補イベント列（確保、解放、使用、または返却による出口）を出します。二回目の走査がその列を記号 LowIR 経路上で順に再生し、経路述語の充足可能性を証明して初めて、発見は高信頼度の UNSAFE になります。LowIR の欠落、不透明な操作、要約のない呼び出し、ソルバの不確定、探索上限は、いずれも候補を UNKNOWN へ格下げします。保守的な may-alias によるメモリ havoc は別に追跡するため、通常のスタックフレームへの格納が、本来正確な到達可能性の根拠を無効にすることはありません。

---

## 予算、出力、バインディング

ハントの探索とソルバは予算で制限します（`--max-paths`、`--max-steps`、`--max-loop`、`--solver-conflicts`）。予算切れは UNKNOWN です。両コマンドは JSON を出力し、`-o` を尊重します。終了コードは SAFE が `0`、UNSAFE が `2`、UNKNOWN またはエラーが `1` です。

同じ解析は C API（`neverd_session_audit_json` / `neverd_session_hunt_json`、版付き `neverd_safety_options`）と Python SDK（`Session.audit()` / `Session.hunt()`）でも使えます。

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
  "evidence": { "concrete_input": { "copy_length": "17", "argv[1]": "16 bytes" }, "candidate_values": [{ "name": "copy_length", "value": "17" }, { "name": "argv[1]", "value": "16 bytes" }], "replayable": false, "replay": { "adapter": "process-input-v1", "reason": "argv input is not supported by process-input-v1" }, "symbolic_model": [{ "id": 0, "name": "copy_len", "width": 64, "value_hex": "0x11", "origin": "input" }] }
}
```

`replayable` は独立した約束ではなく導出された証拠です。`replay` に `process-input-v1` アダプタ向けの完全な入力プランがある場合に限り真です。プランには正確な環境バイト、使用する場合は最初の標準入力バイト列、ソルバ割り当て ID から各入力への対応を記録し、作れない場合は `replay.reason` が理由を示します。これらのフィールドは追加的で、トップレベルの `schema_version` は `1` のままです。

---

## 偽陽性の境界と範囲

- 容量は正確値または実オブジェクトサイズの上界なので、UNSAFE は実際の越境を表します。正確な宣言サイズがなく、包含領域の上界だけでは安全性を証明できない場合は UNKNOWN です。
- 長さ制限付きコピーは解法前に退き `skipped` に数えられます。正確な容量なら SAFE を証明でき、上界だけなら UNKNOWN のままです。
- カタログ済みのワイド文字コピーと追加コピーは、要素幅と既存の宛先長を復元できるまで UNKNOWN です。出力引数型アロケータと条件付き `realloc` の所有権も、ハンドル遷移を証明できなければ UNKNOWN のままです。
- **P0**（本リリース、三形式すべて）：シンクカタログ、引数事前フィルタ、コピー越境ハント、ヒープ寿命監査。全テストホストで PE、ELF、Mach-O × x86-64、AArch64 の 6 fixture を実行します。
- **P1**：スタック/グローバル越境、未初期化ローカル読み、書式文字列検査は利用可能です。より豊かな PDB スタック型と追加のプラットフォーム確保 API は段階的なカバレッジであり、正確な要約がなければ UNKNOWN のままです。
- **P2**：patch で挿入する実行時検査、手続き間の攻撃者到達可能性。
