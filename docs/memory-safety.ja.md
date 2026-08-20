**言語**: [English](memory-safety.md) | [简体中文](memory-safety.zh-CN.md) | [繁體中文](memory-safety.zh-TW.md) | [日本語](memory-safety.ja.md) | [한국어](memory-safety.ko.md) | [Français](memory-safety.fr.md) | [Deutsch](memory-safety.de.md) | [Español](memory-safety.es.md) | [Italiano](memory-safety.it.md) | [Русский](memory-safety.ru.md) | [العربية](memory-safety.ar.md)

[← ドキュメント索引](README.ja.md)

# メモリ安全性の監査とハント

NeverD は読み込んだバイナリに対して二系統のメモリ安全性解析を行い、構造化 JSON で報告します。どちらも形式に依存しないリフト済み IR 上で動くため、**PE/COFF・ELF・Mach-O は同等の第一級対象**です。発見が特定形式のスキャナやインポート表の後ろに隠れることはありません。

| トラック | コマンド | 報告内容 |
|----------|----------|----------|
| **監査（Audit）** | `neverd audit <binary>` | ヒープ寿命の欠陥：リーク、二重解放、解放後使用 |
| **ハント（Hunt）** | `neverd hunt <binary>` | 危険なコピー越境と、再現可能な具体的証人 |

エンジンは NeverD 内蔵の記号実行とビットベクトルソルバを証人生成と到達可能性に再利用します。外部ソルバ、VM、コンテナ依存はありません。

---

## 核心不変条件：失敗は閉じる

リフトされていない操作、ABI が引数を復元できなかった呼び出し、未解決の間接対象、または予算切れはすべて **UNKNOWN** であり、SAFE にはしません。容量を復元できない書き込み先も UNKNOWN です。strict lifting はそのままです。安全性層は、その上に保守的な判定だけを足します。

---

## 形式ごとの識別契約

両トラックは lift パイプラインを必要とします（呼び出しごとの引数を復元するため）。被呼び出し名は NeverD の他部分と同じ識別ビューで付けます。デバッグ情報の探索順は変わりません。

| 形式 | デバッグ情報（優先度の高い順） | インポート / thunk 解決 |
|------|-------------------------------|-------------------------|
| **PE/COFF** | `--pdb`、デバッグディレクトリまたは隣接 `.pdb`、次いで MSVC `/MAP` | IAT スロットと `__imp_` thunk、序数インポート |
| **ELF** | イメージ内 DWARF、分割 `*.debug`、次いで GNU/LLD MAP | PLT stub をインポート名へ解決 |
| **Mach-O** | イメージ内 DWARF、隣接 `.dSYM`、次いで ld64 `-map` | dyld bind / 間接シンボルスロットと stub helper |

`--pdb` / `--map` は権威ある付属ファイルです。読めない場合はエラーであり、黙ってフォールバックしません。`--no-debug` はすべての形式でイメージだけを読みます。

### 名前出所の優先順位

各発見は `name_source` を持ち、被呼び出し名の出所を次の優先順位で選びます。

1. `rename` — 呼び出し側が付けた改名
2. `import` — IAT（PE）、PLT（ELF）、または dyld-bind / stub（Mach-O）
3. `pdb` / `dwarf` / `map` — ローダ種別に応じたデバッグ記号
4. `export` / `symbol` — エクスポート表または記号表
5. `sig` — シグネチャ照合
6. `synthetic` — 無名ルーチン向けのプレースホルダ

DWARF が付けた静的リンクの `memcpy` は `dwarf`、インポートされた `memcpy` はどの形式でも `import` です。シグネチャ照合が、デバッガやインポート表が既に述べた名前を上書きすることはありません。

---

## シンクとソースのカタログ

カタログは設定可能な表であり、ハードコードされた集合ではありません。各 **シンク** は弱点クラス、役割（copy / format / alloc / free / realloc）、関係する引数スロット（宛先、送信元、長さ、容量）を宣言します。各 **ソース** は攻撃者の影響を受ける入力提供者です。

組み込みカタログは一般的な C ランタイムのコピー族（`memcpy`/`memmove`/`strcpy`/`strcat`/`strncpy`/`gets`/…）、明示的な宛先上限を持つ強化 `_chk` 変種、確保と解放族（`malloc`/`calloc`/`realloc`/`free`、operator `new`/`delete`）、任意の Win32 ヒープ API を覆います。入力源は POSIX（`getenv`、`read`、`recv`、`fgets`、`fread`、`scanf`、プログラム引数）**および** Win32（`GetCommandLineA/W`、`ReadFile`、`GetEnvironmentVariable*`）です。PE のハントが POSIX 入力だけに縛られることはありません。

形式ごとの綴りは同一エントリに折り畳みます。先頭のアンダースコアを剥がし（`_malloc`、`___strcpy_chk`）、mangling された operator new/delete は別名で照合します。

仕様ファイルでカタログを拡張または上書きできます。

```bash
neverd hunt --sinks extra_sinks.json --sources extra_sources.json app
```

```json
{ "sinks": [
    { "name": "my_copy", "kind": "copy", "dst": 0, "src": 1, "len": 2 }
] }
```

---

## ハント：コピー越境の判定

各コピーシンクについて、ハントは宛先容量を次の順で復元します。デバッグ宣言の配列サイズ、既知サイズのヒープ確保点、健全なスタックフレーム上界。書き込み長を決める引数は、スタックスロットの spill/reload を辿る後方 SSA 走査で分類します。

- **定数長** は容量と直接比較 → SAFE または UNSAFE。
- **強化** `_chk` コピーは実行時の宛先上界を持つ → SAFE。
- **証明可能な有界長**（長さを返す呼び出し、マスク、クランプ）は SAFE skip として退き、理由を記録します。
- **攻撃者の影響を受ける長さ** で容量が既知ならビットベクトルソルバに渡します。容量を超える長さが充足可能なら UNSAFE とし、ソルバのモデルを具体的証人にします。
- それ以外（未知の長さまたは未知の容量）は UNKNOWN。

復元された容量は常に実オブジェクトサイズの **上界** なので、証明された越境は偽陽性になりません。

---

## 監査：ヒープ寿命の判定

各確保について、監査は CFG 上でハンドルを追跡し（スタック spill/reload を含む）、エスケープ要約（返却、非スタック番地への格納、不透明な被呼び出しへの引き渡し）を適用します。

- **リーク** — ハンドルが解放もエスケープもされていない。
- **二重解放** — ある経路で二回目の解放が初回の後に到達可能。
- **解放後使用** — 解放後に間接参照または不透明な使用が到達可能。

確保と解放の **ラッパ** は関数ごとのエスケープ要約で認識するので、`malloc`/`free` の転送関数が欠陥を隠しません。相互排他分岐上の解放は二重解放として報告しません。

---

## 予算、出力、バインディング

ハントの探索とソルバは予算で制限します（`--max-paths`、`--max-steps`、`--max-loop`、`--solver-conflicts`）。予算切れは UNKNOWN です。両コマンドは JSON を出力し、`-o` を尊重します。終了コードは、問題なしが `0`、UNSAFE 発見ありが `2`、失敗が `1` です。

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
  "evidence": { "concrete_input": { "copy_length": "17", "argv[1]": "17 bytes" } }
}
```

---

## 偽陽性の境界と範囲

- 容量は常に上界なので、UNSAFE は実際の越境を表します。宣言サイズが取れない小さすぎるバッファは UNSAFE ではなく SAFE と出ることがあります（保守的な見逃しであり、誤警報ではありません）。
- 長さ制限付きコピーは SAFE skip として退きます。ハントが証明したい攻撃者制御の事例の精度を優先します。
- **P0**（本リリース、三形式すべて）：シンクカタログ、引数事前フィルタ、コピー越境ハント、ヒープ寿命監査。
- **P1**：スタック/グローバル越境、未初期化読み、書式文字列、より豊かな PDB スタック型、追加のプラットフォーム確保 API。
- **P2**：patch で挿入する実行時検査、手続き間の攻撃者到達可能性。
