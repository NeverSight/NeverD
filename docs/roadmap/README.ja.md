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

NeverD をネイティブ ISA から **Ethereum Virtual Machine (EVM)** コントラクトバイトコードへ拡張し、同一 IR スタックへ 1:1 リフトして構造化 C / LLVM IR を出します。

### 目標

- **EVM loader** — ランタイムバイトコードと一般的な成果物形態
- **オペコード lifter** — 手書き 1:1；未知/新規は strict で失敗
- **スタックとメモリ** — EVM スタックマシンを MedIR へ
- **制御フロー** — JUMP / JUMPI → CFG；可能なら HighIR
- **ストレージと calldata** — `SLOAD`/`SSTORE` 等
- **出力** — 既存 HighIR / LLVM-C 経路
- **CLI / C API** — ネイティブと同じ入口

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

## 4. エンジンとプロダクトの強化（継続）

| 領域 | 方向 |
|------|------|
| Lifter カバレッジ | strict を緩めずネイティブ缺口を縮める |
| セマンティック試験 | 新 ISA とともに Unicorn / roundtrip を拡大 |
| プラグイン ABI | 新形式の loader / 解析をプラグイン化できる箇所 |
| 文書 / 行列 | 試験通過後にのみ README を更新 |

---

## タイムライン

Solana SBF 逆コンパイルとネイティブ形式の完成は実装済みで、回帰試験により保護されています。EVM は研究・設計段階です。リリース日は約束しません。進捗はこの文書で追跡します。

| 機能 | 状態 |
|------|------|
| ネイティブ形式の完成（PE ARM*、Mach-O i386） | 完了 |
| EVM バイトコード逆コンパイル | 研究 / 設計 |
| Solana eBPF（SBF）逆コンパイル | 完了 — v0-v4、C、Rust、LLVM；回帰試験済み |
| エンジンとプロダクト強化 | 継続 |
