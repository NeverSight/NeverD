**Languages**: [English](README.md) | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md) | [日本語](README.ja.md) | [한국어](README.ko.md) | [Français](README.fr.md) | [Deutsch](README.de.md) | [Español](README.es.md) | [Italiano](README.it.md) | [Русский](README.ru.md) | [العربية](README.ar.md)

[← ドキュメント索引](../README.ja.md)

# NeverD ロードマップ

本稿は、現行のネイティブ PE / ELF / Mach-O パイプラインを超える NeverD の主な計画を示します。原則は不変です：**1:1 命令レベルリフト**、**strict の fail-loud**（未対応は黙って飛ばさずエラー）、共通の **4 段階 IR** が lift / decompile / patch を支えます。

---

## 1. ネイティブ形式の完成度

ローダが部分的に認識しているが、形式レベルで端到端未完成のターゲットを仕上げ、サポート行列を実利用と一致させます。

| 項目 | 内容 |
|------|------|
| PE AArch64 | Windows ARM64：unwind/`.pdata`、トランポリン、rewrite 往復 |
| PE ARM32（Thumb-2） | Windows on ARM は Thumb のみ；デコード/放出がそのモードに従う |
| Mach-O i386 | 一般的な clang リロケーションを適用；thin object 優先 |

### 設計原則

- 形式×アーキのセルは形式レベル試験通過（load → lift → decompile / patch）まで「対応」としない
- 既存の ELF / PE x86 / Mach-O arm64+x64 を壊さない
- イメージ単位の命令モード（Thumb vs ARM など）を優先し、散在ヒューリスティックを避ける

---

## 2. EVM バイトコード逆コンパイル

NeverD を **Ethereum Virtual Machine (EVM)** bytecode へ拡張し、同一 IR stack へ 1:1 lift して C、Solidity-oriented source、LLVM IR を出力します。

### 目標

- **EVM loader** — ランタイムバイトコードと一般的な成果物形態
- **オペコード lifter** — 手書き 1:1；未知/新規は strict で失敗
- **スタックとメモリ** — EVM スタックマシンを MedIR へ
- **制御フロー** — JUMP / JUMPI → CFG；可能なら HighIR
- **ストレージと calldata** — `SLOAD`/`SSTORE` 等
- **出力** — explicit host contract を持つ C23/Solidity state machine と verified LLVM IR
- **CLI / C API** — ネイティブと同じ入口

**状態:** Frontier から Fusaka までの legacy opcode decode/lifting は完了し、回帰試験で
保護されています。source reconstruction は保守的に継続中です。selector、event、type、
standard、name、dynamic control flow は evidence が十分な場合だけ報告し、original source、
完全な ABI、完全な ERC compliance を主張しません。canonical function selector、
standard ごとの ABI variant、成功時の return shape は分離されているため、共有 ERC
selector が standard を捏造したり非互換な return type を借用したりしません。
Amsterdam は Review/development の
explicit opt-in target で、`latest` は Fusaka のままです。EOFv1/EIP-7692 は未予定、
EIP-3540 は Stagnant であり、確定 mainnet behavior として扱いません。詳細は
[EVM 逆コンパイル](../evm.ja.md)を参照してください。

### なぜ EVM か

- 監査には忠実な復元が必要
- Low → Med → High → LLVM を契約解析にも再利用
- ネイティブ同様、未対応を黙ってスキップしない

---

## 3. Solana eBPF（SBF）逆コンパイル

**Solana eBPF / SBF** オンチェーンプログラムをサポートし、同一の strict セマンティクスで逆コンパイルします。

### 目標

- **SBF loader** — Solana program ELF
- **eBPF/SBF lifter** — 1:1 手書き；欠落は strict エラー
- **Account / CPI** — 実行時パターンの復元
- **CFG と構造化出力** — ネイティブと同じパイプライン
- **CLI / C API** — 統一 session API

**状況：** 現行 Anza `sbpf` v0-v4 コントラクトへの対応は完了しています。従来の section/relocation ELF と厳格な program-header-only ELF、完全なバージョン別命令データベース、厳格な検証、段階化された Low/Med/High IR、syscall/CPI/account の観測、検証済み LLVM、移植可能な C11、安全な stable Rust、CLI/C API 統合、および独立した有界 raw-bytecode セマンティック oracle を実装済みです。v4 は upstream に追従しますが、特定クラスターでのデプロイ・実行可否は、そのクラスターの feature activation に依存します。詳しくは [Solana SBF 逆コンパイル](../sbf.ja.md) を参照してください。

### なぜ Solana eBPF か

- EVM と並ぶ監査対象
- BPF 系 ISA は CFG + SSA MedIR に適合
- 一つの C SDK でネイティブと契約バイトコードを扱う

---

## 4. メモリ安全性の監査とハント

リフト済みバイナリに対してヒープ寿命の欠陥（リーク、二重解放、解放後使用）と危険なコピー越境を解析し、構造化 JSON で報告します。証明された越境には有界なソルバモデルを付けます。解析は形式に依存しない IR と共有の識別ビュー上で動くため **PE、ELF、Mach-O は同等の対象**であり、自前の記号実行とビットベクトルソルバを再利用します。外部ソルバやコンテナはありません。

| 項目 | 説明 |
|------|------|
| `audit` トラック | IR 上のヒープ状態機械 + エスケープ要約：リーク、二重解放、解放後使用 |
| `hunt` トラック | シンクカタログ + 引数事前フィルタ + 宛先容量 + ソルバ証人 |
| 到達可能性証拠 | 既知エントリからの制御状態、独立した攻撃者制御の固定点、正確なルート／呼び出し鎖の証人 |
| 識別契約 | 形式ごとのシンク解決（PE IAT、ELF PLT、Mach-O dyld bind）と PDB / DWARF / MAP の名前出所 |

**状態：** PE、ELF、Mach-O の Phase 1 は実装済みです。P0 はヒープ寿命と危険なコピーの閉世界解析、および正確なリテラル環境値と最初の標準入力消費に対する schema v1 の追加的な `process-input-v1` 再生証拠を含みます。その他の入力種別は理由付きで再生不能のままです。P1 はスタック／グローバル越境、未初期化ローカル読み、書式文字列をカバーします。未知または部分的にしか適用できない呼び出し効果は UNKNOWN のままです。判定と識別の被覆は [`unittests/safety`](../../unittests/safety) と、全ホストで必須の PE/ELF/Mach-O × x86-64/AArch64 6 セル fixture 行列を実行するエンドツーエンド [`SafetyIntegrationTests.cpp`](../../unittests/safety/SafetyIntegrationTests.cpp) で固定。詳細は [メモリ安全性の監査とハント](../memory-safety.ja.md)。

現在の手続き間スライスは、独立した `verdict` を変えずに schema v1 へ
`reachability.status` と `reachability.attacker_control` を追加します。
`application`、`image`、`export` のルート、正確な内部呼び出し鎖、fail-closed な
UNKNOWN 状態を報告します。`max_call_depth` と `max_summary_iterations` の予算は
C API、両 CLI コマンド、両 Python メソッドから設定できます。したがって
`control_reachable` と
`attacker_reachable` は到達可能性の集計であり、別の判定集計ではありません。

P2 の解析面と計画は、明示的な状態を持つバージョン付き境界を使用します。

| 計画 | 範囲 | 状態 |
|------|------|------|
| `lowir-concolic-v1` | LowIR のハイブリッド／concolic 探索と seed 生成 | 実験的；PE/ELF/Mach-O × x86-64/AArch64 でリプレイ検証済みのレジスタ seed |
| `binary-sanitizer-v1` | 書き換えたネイティブバイナリへ挿入する実行時検査 | Darwin で実験的：全サイトを保護できなければ拒否する counted-write ガードと、認証済み create-exclusive または同一 source の no-change 公開 |
| `process-replay-v1` | argv、ファイル、ネットワーク、反復 read を含む `process-input-v1` より広いプロセス再生 | Phase 0 境界のみ：plan/coordinator 検証と fail-closed なネイティブ可用性照会。ネイティブ replay 操作を提供するホストはない |

concolic アダプタは独立した解析面であり、Phase 1 の安全性レポート受け入れ契約を拡張するものではありません。実験的 sanitizer は `neverd_session_sanitize`、`neverd patch --sanitize=strict`、Python `Session.sanitize` から利用でき、Darwin 以外では lifting や namespace 変更の前に拒否します。完全な receipt が認証するのはトランザクション中に保持した宛先ディレクトリ object だけです。ディレクトリは open 後に rename できるため、元の pathname が処理中または返却後もその object を指すことや、永続的な path binding を証明しません。`NativeProcessReplayAdapter` は引き続き能力が全部そろうか全くないかの Phase 0 query/factory 境界であり、現在は全ホストが全能力を false とし、操作 table を返しません。

---

## 5. エンジンとプロダクトの強化（継続）

| 領域 | 方向 |
|------|------|
| Lifter カバレッジ | strict を緩めずネイティブ缺口を縮める |
| セマンティック試験 | 新 ISA とともに Unicorn / roundtrip を拡大 |
| プラグイン ABI | [ネイティブプラグイン ABI](../plugins.ja.md) をプロセス内 extension contract として維持する。Loader と UI の値は明示的な host API ができるまで metadata のみ |
| 文書 / 行列 | 試験通過後にのみ README を更新 |

---

## タイムライン

native format、Fusaka までの legacy EVM decode/lifting、Solana SBF、メモリ安全性
Phase 1 と現在の既知エントリ到達可能性スライスは回帰試験で保護されています。
保守的な EVM source reconstruction は継続中です。リリース日は約束しません。

| 機能 | 状態 |
|------|------|
| ネイティブ形式の完成（PE ARM*、Mach-O i386） | 完了 |
| EVM legacy decode/lifting | Fusaka まで完了；回帰試験済み |
| EVM source reconstruction | 継続 — evidence-backed かつ保守的 |
| Solana eBPF（SBF）逆コンパイル | 完了 — v0-v4、C、Rust、LLVM；回帰試験済み |
| メモリ安全性の監査とハント | Phase 1 と既知エントリ到達可能性スライスを完了；`lowir-concolic-v1` と Darwin の `binary-sanitizer-v1` は実験的；ネイティブ `process-replay-v1` は fail-closed な Phase 0 アダプタの背後で未提供 |
| エンジンとプロダクト強化 | 継続 |
