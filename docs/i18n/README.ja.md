**言語**: [English](../../README.md) | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md) | [日本語](README.ja.md) | [한국어](README.ko.md) | [Français](README.fr.md) | [Deutsch](README.de.md) | [Español](README.es.md) | [Italiano](README.it.md) | [Русский](README.ru.md) | [العربية](README.ar.md)

<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../assets/neverd-logo-dark.svg">
  <img src="../assets/neverd-logo-light.svg" width="72" alt="NeverD">
</picture>

# NeverD

**AI フレンドリーなバイナリ分析・逆コンパイルエンジン — 1:1 リフト、LLVM 上に構築**

PE · ELF · Mach-O &nbsp;|&nbsp; x86-64 · i386 · AArch64 · ARM32 &nbsp;|&nbsp; 純粋 C SDK

[![AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-blue.svg)](../../LICENSE)
[![C++20](https://img.shields.io/badge/Standard-C%2B%2B20-brightgreen.svg)](#ビルド)
[![Formats](https://img.shields.io/badge/Formats-PE%20%7C%20ELF%20%7C%20Mach--O-informational.svg)](#対応ターゲット)
[![Arch](https://img.shields.io/badge/Arch-x86__64%20%7C%20i386%20%7C%20AArch64%20%7C%20ARM-orange.svg)](#対応ターゲット)
[![SDK](https://img.shields.io/badge/SDK-Pure%20C%20API-lightgrey.svg)](#sdk-とプラグイン)

[ドキュメント](../README.ja.md) · [ロードマップ](../roadmap/README.ja.md) · [貢献](#貢献)

</div>

---

> GitHub のリポジトリトップは常に英語の `README.md` を表示します。上の言語リンクから各言語版を参照してください。

## 概要

NeverD は **1:1 の命令レベルリフト** を中核とするネイティブバイナリ分析・逆コンパイルエンジンです。**PE**、**ELF**、**Mach-O** を読み込み、[Capstone](https://www.capstone-engine.org/) でデコードし、**手書きセマンティクス** の 4 段階 IR パイプラインでリフトします——近似変換ではありません。目標は **100% の意味論的忠実性**：サポート済み命令を **LLVM IR**、**構造化 C**、または **書き換え済みバイナリ** で完全な観測可能振る舞いまま保つことです。

**strict はデフォルト ON**。lifter がない命令は `UnliftedInstruction` を送出し、スキップ・推測・黙っての `NOP` 化はしません。

CLI・統合側・AI エージェントは **純粋 C API** 経由で同じエンジン **`libneverd`** を使い、Capstone・LLVM・内部 C++ には直接リンクしません。

今後のリリースでは、同じ IR スタック上に [EVM](../roadmap/README.ja.md#2-evm-バイトコード逆コンパイル) と [Solana eBPF / SBF](../roadmap/README.ja.md#3-solana-ebpf-sbf-逆コンパイル) の逆コンパイルを追加します — [ロードマップ](../roadmap/README.ja.md) を参照。

## なぜ NeverD？

- **1:1 セマンティクス** — 手書き lifter；デフォルト strict では未対応命令が例外を送出
- **LLM フレンドリー** — 構造化 C・LLVM IR・JSON 分析を純粋 C API で公開し、エラーは決定的
- **1 本のパイプライン、3 つの出口** — `lift` → LLVM IR · `decompile` → C · `patch` → 書き換えバイナリ
- **バイナリ書き換え** — PE / ELF / Mach-O、section トランポリンまたは inplace
- **分析ツール群** — CLI、デバッグ情報、シグネチャ、プラグイン、任意の難読化パス

## 対応ターゲット

| | **x86-64** | **i386** | **AArch64** | **ARM32** |
|---|:---:|:---:|:---:|:---:|
| **PE** (Windows) | ✓ | ✓ | ✓ | ✓ |
| **ELF** (Linux / Android) | ✓ | ✓ | ✓ | ✓ |
| **Mach-O** (macOS / iOS) | ✓ | ✓ | ✓ | ✓ |

> Mach-O i386 の統合カバレッジには `thin` 再配置可能オブジェクトと実行形式リライトバックエンドのテストを使用しています。現在の macOS ホストでは旧式の i386 実行ファイルをリンクできません。

## 仕組み

```text
Binary (PE / ELF / Mach-O)
  → Loader + DebugInfo
  → Capstone decode
  → LowIR     architecture-neutral NdOps · CFG
  → MedIR     types · ABI · calls · memory · SSA
       │
       ├─ lift        MedIR → LLVM IR
       ├─ decompile   MedIR → HighIR → C
       │              MedIR → LLVM IR → opt → C   (-llvm)
       └─ patch       MedIR → LLVM IR → codegen → binary
```

| 段階 | 役割 |
|------|------|
| **LowIR** | 約 77 種の `NdOp` + CFG |
| **MedIR** | 型、呼び出し規約、メモリモデル、SSA |
| **HighIR** | 構造化制御フロー（`if` / `while` / `for`） |
| **LLVM** | 最適化、C 出力、またはマシンコード生成 |

## クイックスタート

```bash
git submodule update --init --recursive
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# パイプライン
./build/bin/neverd lift -o out.ll binary
./build/bin/neverd decompile -o out.c binary
./build/bin/neverd patch -hello -o patched binary

# 分析
./build/bin/neverd funcs binary
./build/bin/neverd disasm --func 0x401000 binary
./build/bin/neverd sigs --auto binary
```

ビルド時にシグネチャライブラリは `build/bin/signatures/` にインストールされます。`sigs --auto` は形式・アーキ・ビット幅でセットを選びます。

## ビルド

**要件：** CMake ≥ 3.20 · Ninja · C++20 コンパイラ · Git submodule（LLVM fork + Capstone）

```bash
git submodule update --init --recursive
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

初回の configure で LLVM fork をローカルビルドします（多くは 30–60 分）。以降は増分。プリセット：`CMakePresets.json` → `release` / `relwithdebinfo` / `debug`。

<details>
<summary><strong>プリビルド LLVM · 成果物 · テスト · CMake オプション</strong></summary>

<br>

**プリビルド LLVM**

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DNEVERD_LLVM_PREBUILT=ON \
  -DNEVERD_LLVM_PREBUILT_TAG=neverd-llvm-v23.0.0
cmake --build build
```

**成果物**

| パス | 説明 |
|------|------|
| `build/bin/neverd` | 統合 CLI |
| `build/bin/neverd-bench` | ベンチマーク（JSON） |
| `build/bin/neverd-sigmaker` | 静的ライブラリから `.pat` 生成 |
| `build/bin/libneverd.*` | エンジン共有ライブラリ |
| `build/bin/sdk/` | `NeverDCAPI.h`、`NeverDPlugin.h` |
| `build/bin/signatures/` | 同梱シグネチャ |

**テスト**

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --target check-neverd
```

| ターゲット | 説明 |
|------------|------|
| `check-neverd` | 全テスト |
| `check-neverd-semantic` | セマンティック roundtrip のみ（Unicorn） |

**CMake オプション**

| オプション | デフォルト | 説明 |
|------------|------------|------|
| `NEVERD_LLVM_PREBUILT` | `OFF` | CI プリビルド LLVM |
| `NEVERD_BUILD_SHARED` | `ON` | `libneverd` をビルド |
| `NEVERD_BUILD_PLUGINS` | `OFF` | サンプルプラグイン |
| `BUILD_TESTING` | `OFF` | ユニットテスト |

</details>

## CLI

```text
neverd <command> [options] <binary>
```

### パイプライン

| コマンド | 出力 | 説明 |
|----------|------|------|
| `lift` | `.ll` | LLVM IR へリフト |
| `decompile` | `.c` | 構造化 C（HighIR） |
| `decompile -llvm` | `.c` | LLVM IR + 最適化経由 |
| `patch` | バイナリ | 機械語の書き換え |

```bash
neverd patch -hello -o patched binary
neverd patch --from-ir repl.ll -o patched binary
neverd patch --from-c repl.c --func 0x401000 -o patched binary
neverd patch --mode inplace -o patched binary
neverd patch --subst --flatten --mba -o patched binary
```

<details>
<summary><strong>分析コマンド</strong></summary>

<br>

| コマンド | 用途 |
|----------|------|
| `info` / `dashboard` / `headers` | メタデータと概要 |
| `funcs` | 検出された関数 |
| `disasm` | 逆アセンブル（`--func` 名または hex） |
| `hex` | アドレスの hex dump |
| `cfg` / `callgraph` | CFG / コールグラフ（JSON；DOT/SVG 任意） |
| `xrefs` | クロスリファレンス |
| `strings` / `search` | 文字列 / バイトまたはテキスト検索 |
| `imports` / `exports` / `symbols` / `relocs` | テーブル |
| `segments` / `sections` / `entrypoints` | レイアウト |
| `diff` | 2 バイナリ比較（`-a` / `-b`） |
| `sigs` | シグネチャ（`--auto`） |
| `rename` / `annotate` / `bookmarks` | セッション注釈 |
| `export` | 結果のエクスポート |
| `plugins` | プラグインの一覧または実行 |

多くの分析コマンドは `--json` を受け付けます。

</details>

## SDK とプラグイン

統合側は `libneverd` の **純粋 C API** を使います：

| ヘッダ | 役割 |
|--------|------|
| `NeverDCAPI.h` | セッション、リフト、逆コンパイル、patch、IR / CFG、注釈 |
| `NeverDPlugin.h` | 動的ライブラリプラグイン ABI |

```c
neverd_session_t s = neverd_session_create();
neverd_session_load(s, "binary.exe");
neverd_session_analyze(s);

const char *c = neverd_decompile(s, 0x401000);
neverd_free_string(c);
neverd_session_destroy(s);
```

`-DNEVERD_BUILD_PLUGINS=ON` でサンプルプラグインをビルド。読み込みパス：`<neverd-dir>/plugins`、`~/.neverd/plugins`、`$NEVERD_PLUGIN_PATH`。

## 依存関係

| コンポーネント | 役割 | ソース |
|----------------|------|--------|
| **LLVM**（fork） | IR、最適化、コード生成、診断 | `third_party/llvm-project` またはプリビルド |
| **Capstone** | デコード | `third_party/capstone` |

第三者コンポーネントは各々のライセンスを保持します。

## 貢献

スタイルは LLVM 慣習（`.clang-format`）に従います。

開発は **`dev`** ブランチで行われます（GitHub のデフォルトブランチ）。

```bash
git clone -b dev https://github.com/NeverSight/NeverD.git
cd NeverD
git submodule update --init --recursive
```

## ライセンス

[AGPL-3.0](../../LICENSE)

LLVM コンポーネントは Apache-2.0 WITH LLVM-exception ライセンスを保持します。Capstone は独自のライセンスを保持します。
