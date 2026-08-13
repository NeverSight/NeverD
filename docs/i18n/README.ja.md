**言語**: [English](../../README.md) | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md) | [日本語](README.ja.md) | [한국어](README.ko.md) | [Français](README.fr.md) | [Deutsch](README.de.md) | [Español](README.es.md) | [Italiano](README.it.md) | [Русский](README.ru.md) | [العربية](README.ar.md)

<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../assets/neverd-logo-dark.svg">
  <img src="../assets/neverd-logo-light.svg" width="72" alt="NeverD">
</picture>

# NeverD

**AI フレンドリーなバイナリ分析・逆コンパイルエンジン — 1:1 リフト、LLVM 上に構築**

PE · ELF · Mach-O · EVM · Solana SBF &nbsp;|&nbsp; x86-64 · i386 · AArch64 · ARM32 · EVM256 · SBF &nbsp;|&nbsp; 純粋 C SDK

[![AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-blue.svg)](../../LICENSE)
[![C++20](https://img.shields.io/badge/Standard-C%2B%2B20-brightgreen.svg)](#ビルド)
![Platform](https://img.shields.io/badge/Platform-macOS%20%7C%20Linux%20%7C%20Windows-informational.svg)
[![SDK](https://img.shields.io/badge/SDK-Pure%20C%20API-orange.svg)](#sdk-とプラグイン)

[ドキュメント](../README.ja.md) · [ロードマップ](../roadmap/README.ja.md) · [貢献](CONTRIBUTING.ja.md)

</div>

---

> GitHub のリポジトリトップは常に英語の `README.md` を表示します。上の言語リンクから各言語版を参照してください。

## 概要

NeverD は **1:1 の命令レベルリフト** を中核とするネイティブおよびスマートコントラクト解析・逆コンパイルエンジンです。**PE**、**ELF**、**Mach-O**、legacy **EVM** bytecode、Solana **SBF ELF** program を読み込みます。native target は [Capstone](https://www.capstone-engine.org/) で decode し、EVM/SBF は専用 version-aware decoder と staged IR を使います。すべて hand-written semantics です。対応 instruction は **LLVM IR**、**C**、**SBF Rust**、**EVM Solidity reconstruction**、または native の**書き換え済み binary**で observable behavior を保持します。

**strict はデフォルト ON**。lifter がない命令は `UnliftedInstruction` を送出し、スキップ・推測・黙っての `NOP` 化はしません。

CLI・統合側・AI エージェントは **純粋 C API** 経由で同じエンジン **`libneverd`** を使い、Capstone・LLVM・内部 C++ には直接リンクしません。

input format、host contract、制限は [EVM ガイド](../evm.ja.md)と [Solana SBF ガイド](../sbf.ja.md)を参照してください。

## なぜ NeverD？

- **1:1 セマンティクス** — 手書き lifter；デフォルト strict では未対応命令が例外を送出
- **LLM フレンドリー** — 構造化 C・LLVM IR・JSON 分析を純粋 C API で公開し、エラーは決定的
- **1 本のパイプライン、複数の出口** — `lift` → LLVM IR · `decompile` → C/Solidity/Rust · `patch` → ネイティブバイナリ書き換え
- **バイナリ書き換え** — PE / ELF / Mach-O、section トランポリンまたは inplace
- **分析ツール群** — CLI、デバッグ情報、シグネチャ、プラグイン、任意の難読化パス

## 対応ターゲット

| | **x86-64** | **i386** | **AArch64** | **ARM32** |
|---|:---:|:---:|:---:|:---:|
| **PE** (Windows) | ✓ | ✓ | ✓ | ✓ |
| **ELF** (Linux / Android) | ✓ | ✓ | ✓ | ✓ |
| **Mach-O** (macOS / iOS) | ✓ | ✓ | ✓ | ✓ |

> 表の全セルは実装済みですが、統合テストの深さは異なります。詳細は[アーキテクチャのカバレッジ表](../architecture.ja.md#support-and-test-depth)を参照してください。Mach-O i386 では、現代の macOS が旧式の i386 実行ファイルをリンクできないため、`thin` 再配置可能オブジェクトを使用します。

legacy EVM bytecode は native container と独立して対応します。Frontier から Fusaka
までの 150 assigned opcode が専用 Low/Med/High IR、verified LLVM `i256`、C23
`_BitInt(256)`、Solidity output に入ります。[EVM 逆コンパイル](../evm.ja.md)を参照。

Solana SBF v0-v4 ELF プログラムは専用 strict loader、完全なバージョン別 ISA
metadata、Low/Med/High IR、検証済み LLVM、portable C11、安全な stable Rust を
使用します。[Solana SBF 逆コンパイル](../sbf.ja.md)を参照してください。

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

EVM (raw / hex / compiler artifact)
  → runtime normalization + hardfork-aware decode
  → EVM LowIR → EVM stack-SSA MedIR → recovered EVM HighIR
       ├─ lift        → verified LLVM i256/i512
       └─ decompile   → C23 _BitInt(256) または Solidity reconstruction

Solana SBF ELF (v0-v4)
  → バージョン対応 legacy/strict loader + verifier
  → SBF LowIR → 正規化 MedIR → 復元 SBF HighIR
       ├─ lift        → 検証済み LLVM i64 runtime ABI
       └─ decompile   → portable C11 または安全な stable Rust
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

# EVM
./build/bin/neverd lift contract.evm -o contract.ll
./build/bin/neverd decompile --language=c contract.evm -o contract.c
./build/bin/neverd decompile --language=solidity contract.evm -o contract.sol

# Solana SBF
./build/bin/neverd info program.so
./build/bin/neverd lift program.so -o program.ll
./build/bin/neverd decompile --language=c program.so -o program.c
./build/bin/neverd decompile --language=rust program.so -o program.rs

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

NeverD の通常の push および pull request CI は、意図的に LLVM submodule をソースからビルドします。`CI` ワークフローを手動実行する際に `use_prebuilt_llvm` を選ぶと公開パッケージを検証できます。プリビルド LLVM が有効になるのは手動で `true` を選んだときだけで、未選択なら自動 CI と同じソースビルド経路のままです。

公開パッケージは CMake を実行するホストに応じて選ばれます:

| ホスト | リリース資産 |
|--------|--------------|
| macOS arm64 | `neverd-llvm-macos-arm64.tar.xz` |
| Linux x86_64 | `neverd-llvm-linux-x86_64.tar.xz` |
| Windows x64 | `neverd-llvm-windows-x64.zip` |

各アーカイブは `~/.cache/neverd-llvm/<tag>/<arch>/`（または `NEVERD_LLVM_PREBUILT_CACHE_DIR` が指すパス）へ展開される前に、`cmake/NeverDLLVMPrebuilt.cmake` に固定されたダイジェストと照合されます。その pin が記述していない tag の場合は、アーカイブと共に公開された `.sha256` と照合します。リリースビルドは macOS と Linux で ccache を、Windows の clang-cl では GitHub Actions キャッシュを backend にした sccache を使います。コンパイラキャッシュは再ビルドを速くするだけで、リリース資産として公開されることはありません。

リリース tag は NeverD パッケージのバージョンを表し、`BUILDINFO.txt` が正確な LLVM fork commit を記録します。LLVM が `23.0.0` を報告し続けていても fork のソースが変わった場合、通常の不変な選択は `neverd-llvm-v23.0.0-r1`（次は `-r2`）のようなパッケージリビジョンであり、LLVM 自身の patch バージョンが変わらない限り `23.0.1` ではありません。`NEVERD_LLVM_PREBUILT_TAG` をその新しいリビジョンに向けてください。

既存の可変な `neverd-llvm-v23.0.0` リリースをその場で修復するには、llvm-project の `main` ブランチから `NeverD LLVM Release` ワークフローを実行し、`overwrite_existing_assets` を有効にします:

```bash
gh workflow run neverd-release.yml \
  --repo NeverSight/llvm-project \
  --ref main \
  -f release_tag=neverd-llvm-v23.0.0 \
  -f overwrite_existing_assets=true
```

これは同名の資産を置き換えますが、既存の Git tag は意図的に強制移動しません。同じ変更で `cmake/NeverDLLVMPrebuilt.cmake` に固定されたダイジェストを更新してください。ある NeverD リビジョンが期待するビルドを名指すのは tag ではなくそれらのダイジェストであり、そのおかげで古い `~/.cache/neverd-llvm/neverd-llvm-v23.0.0/` は次の configure で置き換えられ、どの pin とも一致しないアーカイブはその configure をチェックサム不一致で止めます。古いパッケージに無かったヘッダとして後から表面化することはありません。新しい `-rN` tag を使えば、その場での書き換え自体が不要になります。ワークフローはチェックボックスが有効でない限り誤った置き換えを拒否し、GitHub がリリースを immutable と印付けている場合は置き換えを完全に拒否します。

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

フォーカスターゲット、CTest ラベル、fixture 要件、形式横断の書き換えグリッドについては、[NeverD のテスト](../testing.ja.md)を参照してください。

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
| `decompile` | `.c` / `.sol` / `.rs` | `--language` で C、EVM Solidity、SBF Rust を選択 |
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

EVM では `neverd_decompile_all_ex(..., NEVERD_OUTPUT_SOLIDITY, ...)` で Solidity
を明示選択します。従来の `neverd_decompile_all` は C を出力します。詳細は
[EVM C API 例](../evm.ja.md#c-api)を参照してください。

`-DNEVERD_BUILD_PLUGINS=ON` でサンプルプラグインをビルド。読み込みパス：`<neverd-dir>/plugins`、`~/.neverd/plugins`、`$NEVERD_PLUGIN_PATH`。

## 依存関係

| コンポーネント | 役割 | ソース |
|----------------|------|--------|
| **LLVM**（fork） | IR、最適化、コード生成、診断 | `third_party/llvm-project` またはプリビルド |
| **Capstone** | デコード | `third_party/capstone` |

第三者コンポーネントは各々のライセンスを保持します。

## 貢献

開発成果は **`dev`** ブランチへ統合します。環境構築、Release/Debug の手順、スタイル、フォーカステスト、プルリクエスト要件は[貢献ガイド](CONTRIBUTING.ja.md)を参照してください。[アーキテクチャ](../architecture.ja.md)と[テスト](../testing.ja.md)のガイドでは、一般的な変更を対応するコードと検証スイートへマッピングしています。

## ライセンス

[AGPL-3.0](../../LICENSE)

LLVM コンポーネントは Apache-2.0 WITH LLVM-exception ライセンスを保持します。Capstone は独自のライセンスを保持します。
