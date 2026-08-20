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

**状態:** Frontier から Fusaka の legacy EVM について完了しました。150 assigned
opcode、raw/hex/artifact input、runtime extraction、CFG/stack-SSA、strict/relaxed
analysis、C23/LLVM/Solidity backend、CLI/C API、Anvil differential を網羅します。
host ABI と制限は [EVM 逆コンパイル](../evm.ja.md)を参照してください。

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

リフト済みバイナリに対してヒープ寿命の欠陥（リーク、二重解放、解放後使用）と危険なコピー越境を解析し、構造化 JSON で報告します。証明された越境には具体的証人を付けます。解析は形式に依存しない IR と共有の識別ビュー上で動くため **PE、ELF、Mach-O は同等の対象**であり、自前の記号実行とビットベクトルソルバを再利用します。外部ソルバやコンテナはありません。

| 項目 | 説明 |
|------|------|
| `audit` トラック | IR 上のヒープ状態機械 + エスケープ要約：リーク、二重解放、解放後使用 |
| `hunt` トラック | シンクカタログ + 引数事前フィルタ + 宛先容量 + ソルバ証人 |
| 識別契約 | 形式ごとのシンク解決（PE IAT、ELF PLT、Mach-O dyld bind）と PDB / DWARF / MAP の名前出所 |

**状態：** PE、ELF、Mach-O の P0 は完了。判定と識別の被覆は [`unittests/safety`](../../unittests/safety) と、全ホストで必須の PE/ELF/Mach-O × x86-64/AArch64 6 セル fixture 行列を実行するエンドツーエンド [`SafetyIntegrationTests.cpp`](../../unittests/safety/SafetyIntegrationTests.cpp) で固定。詳細は [メモリ安全性の監査とハント](../memory-safety.ja.md)。P1 はスタック／グローバル越境、未初期化読み、書式文字列へ広がります。

---

## 5. エンジンとプロダクトの強化（継続）

| 領域 | 方向 |
|------|------|
| Lifter カバレッジ | strict を緩めずネイティブ缺口を縮める |
| セマンティック試験 | 新 ISA とともに Unicorn / roundtrip を拡大 |
| プラグイン ABI | 新形式の loader / 解析をプラグイン化できる箇所 |
| 文書 / 行列 | 試験通過後にのみ README を更新 |

---

## タイムライン

native format、EVM、Solana SBF、メモリ安全性 P0 は実装済みで回帰試験により保護されています。リリース日は約束しません。

| 機能 | 状態 |
|------|------|
| ネイティブ形式の完成（PE ARM*、Mach-O i386） | 完了 |
| EVM バイトコード逆コンパイル | 完了 — C、Solidity、LLVM；回帰試験済み |
| Solana eBPF（SBF）逆コンパイル | 完了 — v0-v4、C、Rust、LLVM；回帰試験済み |
| メモリ安全性の監査とハント | 完了 — PE、ELF、Mach-O の P0；回帰試験済み |
| エンジンとプロダクト強化 | 継続 |
