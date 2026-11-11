**言語**: [English](../../README.md) | [简体中文](../zh-CN/project.md) | [繁體中文](../zh-TW/project.md) | [日本語](project.md) | [한국어](../ko/project.md) | [Français](../fr/project.md) | [Deutsch](../de/project.md) | [Español](../es/project.md) | [Italiano](../it/project.md) | [Русский](../ru/project.md) | [العربية](../ar/project.md)

<!-- i18n-source: edadf84089252d2d8a80ae25926c8013d54fb4a3fea7fc631f3731016652375c -->

<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../assets/neverd-logo-dark.svg">
  <img src="../assets/neverd-logo-light.svg" width="72" alt="NeverD">
</picture>

# NeverD

**AI フレンドリーなバイナリ分析・逆コンパイルエンジン — 1:1 リフト、LLVM 上に構築**

PE · ELF · Mach-O · EVM · Solana SBF &nbsp;|&nbsp; x86-64 · i386 · AArch64 · ARM32 · EVM256 · SBF &nbsp;|&nbsp; C + Python SDK

[![AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-blue.svg)](../../LICENSE)
[![C++20](https://img.shields.io/badge/Standard-C%2B%2B20-brightgreen.svg)](#ビルド)
![Platform](https://img.shields.io/badge/Platform-macOS%20%7C%20Linux%20%7C%20Windows-informational.svg)
[![SDK](https://img.shields.io/badge/SDK-C%20%2B%20Python-orange.svg)](#sdk-とプラグイン)

[ドキュメント](README.md) · [Android](android.md) · [iOS](ios.md) · [ロードマップ](roadmap.md) · [貢献](CONTRIBUTING.md)

</div>

---

> GitHub のリポジトリトップは常に英語の `README.md` を表示します。上の言語リンクから各言語版を参照してください。

<!-- i18n-section: overview -->

## 概要

NeverD は **1:1 の命令レベルリフト** を中核とするネイティブおよびスマートコントラクト解析・逆コンパイルエンジンです。**PE**、**ELF**、**Mach-O**、legacy **EVM** bytecode、Solana **SBF ELF** program を読み込みます。native target は [Capstone](https://www.capstone-engine.org/) で decode し、EVM/SBF は専用 version-aware decoder と staged IR を使います。すべて hand-written semantics です。対応 instruction は **LLVM IR**、**C**、**SBF Rust**、**EVM Solidity reconstruction**、または native の**書き換え済み binary**で observable behavior を保持します。

**strict はデフォルト ON**。lifter がない命令は `UnliftedInstruction` を送出し、スキップ・推測・黙っての `NOP` 化はしません。

CLI・統合側・AI エージェントは **純粋 C API** 経由で同じエンジン **`libneverd`** を使い、Capstone・LLVM・内部 C++ には直接リンクしません。

使用方法、対応する契約と制限は [EVM](evm.md)、[Solana SBF](sbf.md)、[モバイルソース復元](mobile.md)、実験的な[インタープリターソース復元](interpreter-recovery.md)のガイドを参照してください。

<!-- i18n-section: why-neverd -->

## なぜ NeverD？

- **1:1 セマンティクス** — 手書き lifter；デフォルト strict では未対応命令が例外を送出
- **LLM フレンドリー** — 構造化 C・LLVM IR・JSON 分析を純粋 C API で公開し、エラーは決定的
- **1 本のパイプライン、複数の出口** — `lift` → LLVM IR · `decompile` → C/Solidity/Rust · `patch` → ネイティブバイナリ書き換え
- **バイナリ書き換え** — PE / ELF / Mach-O、section トランポリンまたは inplace
- **分析ツール群** — CLI、デバッグ情報、シグネチャ、プラグイン、任意の難読化パス

<!-- i18n-section: supported-targets -->

## 対応ターゲット

| | **x86-64** | **i386** | **AArch64** | **ARM32** |
|---|:---:|:---:|:---:|:---:|
| **PE** (Windows) | ✓ | ✓ | ✓ | ✓ |
| **ELF** (Linux / Android) | ✓ | ✓ | ✓ | ✓ |
| **Mach-O** (macOS / iOS) | ✓ | ✓ | ✓ | ✓ |

> 表の全セルは実装済みですが、統合テストの深さは異なります。詳細は[アーキテクチャのカバレッジ表](architecture.md#support-and-test-depth)を参照してください。Mach-O i386 では、現代の macOS が旧式の i386 実行ファイルをリンクできないため、`thin` 再配置可能オブジェクトを使用します。

**MIPS ELF 解析は実験的な対応です。** MIPS32r2/o32 と MIPS64r2/n64 の両エンディアンで、厳密に限定した整数命令の逆アセンブル、CFG 復元、C および LLVM 出力に対応します。ネイティブコードの書き換えには対応していません。[MIPS の対応範囲とファームウェアの例（英語）](../mips.md)で、個別に検証された microMIPS32、MIPS16e/e2、DSP、MIPS32r5 MSA の各サブセットとソース出力の制約を確認してください。

legacy EVM bytecode は native container と独立して対応します。Frontier から Fusaka
までの 150 assigned opcode が専用 Low/Med/High IR、verified LLVM `i256`、C23
`_BitInt(256)`、Solidity output に入ります。[EVM 逆コンパイル](evm.md)を参照。

Solana SBF v0-v4 ELF プログラムは専用 strict loader、完全なバージョン別 ISA
metadata、Low/Med/High IR、検証済み LLVM、portable C11、安全な stable Rust を
使用します。[Solana SBF 逆コンパイル](sbf.md)を参照してください。

<!-- i18n-section: mobile-source-recovery -->

### モバイルソース復元

実験的な `neverd mobile` CLI は、次のモバイル入力とソース出力に対応しています。

| プラットフォーム | 入力 | 出力 |
|------------------|------|------|
| [Android](android.md) | APK（multidex を含む）、DEX、smali ファイル／ディレクトリ | Java と JSON レポート |
| [iOS](ios.md) | IPA、`.app`、Mach-O（arm64 / x86_64） | ネイティブ C、対応する Objective-C / Swift ソース、JSON カバレッジレポート |

復元は対応するコードパターンに依存します。範囲と制限は[モバイル概要（英語）](../mobile.md)と各プラットフォームのガイドを参照してください。

<!-- i18n-section: cpu-workloads -->

### CPU 実行とゲスト環境

NeverD は CPU バックエンドとゲスト OS モデルを分離し、範囲を限定したプロセス、Android ネイティブライブラリ、x64 Windows ドライバーを実行します。Unicorn は x64/ARM64 のソフトウェア実行を提供し、同じ ISA のホストでは KVM（Linux）、WHP（Windows）、HVF（macOS）を使用できます。対応環境、検証結果、制限は [CPU 実行とゲストワークロード](emulation.md)を参照してください。

<!-- i18n-section: how-it-works -->

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

<!-- i18n-section: quick-start -->

## クイックスタート

```bash
git submodule update --init --recursive
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# パイプライン
./build/bin/neverd lift -o out.ll binary
./build/bin/neverd decompile -o out.c binary
./build/bin/neverd patch -hello -o patched binary

# プログラム自身の言語（C++、Rust、Go、C）による疑似コード
./build/bin/neverd decompile --language=source -o out.rs rust-binary
./build/bin/neverd decompile --language=cpp -o out.cpp cpp-binary
./build/bin/neverd decompile --language=go --func main.main go-binary

# 単一ネイティブ関数の任意変換（通常の HighC では実行しません）
./build/bin/neverd decompile --func main --symbolic-simplify -o enhanced.c binary
./build/bin/neverd decompile --func main --llvm --llvm-opt=O3 -o optimized.c binary
./build/bin/neverd decompile --func main --llvm --llvm-opt=custom \
  --llvm-passes=instcombine,early-cse,simplifycfg,dce -o cleaned.c binary

# EVM
./build/bin/neverd lift contract.evm -o contract.ll
./build/bin/neverd decompile --language=c contract.evm -o contract.c
./build/bin/neverd decompile --language=solidity contract.evm -o contract.sol

# Solana SBF
./build/bin/neverd info program.so
./build/bin/neverd lift program.so -o program.ll
./build/bin/neverd decompile --language=c program.so -o program.c
./build/bin/neverd decompile --language=rust program.so -o program.rs

# Android / iOS のソース復元（実験的）
./build/bin/neverd mobile app.apk -o recovered-android
./build/bin/neverd mobile App.ipa -o recovered-ios

# 分析
./build/bin/neverd funcs binary
./build/bin/neverd disasm --func 0x401000 binary
./build/bin/neverd sym-explore --func 0x401000 --expressions binary
./build/bin/neverd audit binary
./build/bin/neverd hunt binary
./build/bin/neverd sigs --auto binary
```

ビルド時にシグネチャライブラリは `build/bin/signatures/` にインストールされます。`sigs --auto` は形式・アーキ・ビット幅でセットを選びます。PE ファイルの Rich ヘッダーがリンカーの Visual Studio リリースを示す場合は、そのリリースの `vs<year>.pat` と、どのリリースにも属さないファイルだけを読み込みます。`--sig-base <dir>` は別のシグネチャツリーから同じ方法で選びます。 1 MiB 以上のパターンファイルは一度だけ解析されます。そのモジュールはユーザーのキャッシュディレクトリ内の `neverd/signatures` に保存され、以降の読み込みではマップされます。`NEVERD_SIGNATURE_CACHE` で別のディレクトリを指定でき、`off` でキャッシュを無効にします。

<!-- i18n-section: building -->

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
  -DNEVERD_LLVM_PREBUILT_TAG=neverd-llvm-v23.0.0-r6
cmake --build build
```

NeverD の通常の push および pull request CI は、意図的に LLVM submodule をソースからビルドします。`CI` ワークフローを手動実行する際に `use_prebuilt_llvm` を選ぶと公開パッケージを検証できます。プリビルド LLVM が有効になるのは手動で `true` を選んだときだけで、未選択なら自動 CI と同じソースビルド経路のままです。

公開パッケージは CMake を実行するホストに応じて選ばれます:

| ホスト | リリース資産 |
|--------|--------------|
| macOS arm64 | `neverd-llvm-macos-arm64.tar.xz` |
| Linux x86_64 | `neverd-llvm-linux-x86_64.tar.xz` |
| Windows x64 | `neverd-llvm-windows-x64.zip` |

各アーカイブは `~/.cache/neverd-llvm/<tag>/<arch>/`（または `NEVERD_LLVM_PREBUILT_CACHE_DIR` の指定先）への展開前に、`cmake/NeverDLLVMPrebuilt.cmake` の固定ダイジェストで検証されます。固定値の対象外のタグでは、公開された `.sha256` を使います。既定の固定版では `BUILDINFO.txt` の LLVM サブモジュールコミットも完全一致が必要です。リリースビルドは macOS/Linux で ccache、Windows clang-cl で sccache と GitHub Actions キャッシュを使用します。コンパイラーキャッシュは再ビルドの高速化専用で、リリース成果物には含めません。

既定のパッケージ改訂版は `neverd-llvm-v23.0.0-r6` です。Git タグ、リリース対象、ソースコミット、3アーカイブのダイジェストを不可変の改訂版として固定します。古い基本タグ、`neverd-llvm-v23.0.0-r1`、`neverd-llvm-v23.0.0-r2`、`neverd-llvm-v23.0.0-r3`、`neverd-llvm-v23.0.0-r4`、`neverd-llvm-v23.0.0-r5` をキャッシュしたビルドディレクトリは、明示的な `NEVERD_LLVM_PREBUILT_SHA256` がなければ自動で `r6` に移行します。`Prebuilt LLVM Audit` は push、pull request、6時間ごとに実行され、`scripts/audit_prebuilt_llvm_release.py` が固定値と GitHub の現在のリリースおよび各チェックサムファイルを照合します。

LLVM fork が変更されても LLVM のバージョンが `23.0.0` の場合は、次のパッケージ改訂版 `neverd-llvm-v23.0.0-r7`、続いて `-r8` を公開します。既存リリースの上書きや架空の LLVM バージョン `23.0.1` は使用しません。

```bash
gh workflow run neverd-release.yml \
  --repo NeverSight/llvm-project \
  --ref main \
  -f release_tag=neverd-llvm-v23.0.0-r7 \
  -f overwrite_existing_assets=false
```

成功後は `cmake/NeverDLLVMPrebuilt.cmake` の既定タグ、固定コミット、3つのダイジェストを同時に更新します。パッケージの改訂版ごとに専用のキャッシュディレクトリを使い、現在の既定版は `.cache/neverd-llvm/neverd-llvm-v23.0.0-r6` 以下に保存されます。古い、または再公開されたアーカイブは展開前に拒否されます。`overwrite_existing_assets` は旧版復旧専用で、通常の改訂版公開では無効にします。

**成果物**

| パス | 説明 |
|------|------|
| `build/bin/neverd` | 統合 CLI |
| `build/bin/neverd-bench` | ベンチマーク（JSON） |
| `build/bin/neverd-sigmaker` | 静的ライブラリから `.pat` 生成 |
| `build/bin/libneverd.*` | エンジン共有ライブラリ |
| `build/bin/sdk/` | C SDK の canonical include root。`neverd/sdk/` 階層を保った `<neverd/sdk/NeverDCAPI.h>` または `<neverd/sdk/NeverDPlugin.h>` を使用 |
| `build/bin/sdk/python/` | 型付き Python プラグインパッケージとサンプル |
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

フォーカスターゲット、CTest ラベル、fixture 要件、形式横断の書き換えグリッドについては、[NeverD のテスト](testing.md)を参照してください。

**CMake オプション**

| オプション | デフォルト | 説明 |
|------------|------------|------|
| `NEVERD_LLVM_PREBUILT` | `OFF` | CI プリビルド LLVM |
| `NEVERD_BUILD_SHARED` | `ON` | `libneverd` をビルド |
| `NEVERD_ENABLE_PYTHON_PLUGINS` | `ON` | CPython 3.10+ プラグインサポートを組み込む |
| `NEVERD_BUILD_PLUGINS` | `OFF` | サンプルプラグイン |
| `BUILD_TESTING` | `OFF` | ユニットテスト |
| `NEVERD_ENABLE_SEMANTIC_TESTS` | `ON` | Unicorn に依存する意味論テスト群（`BUILD_TESTING=ON` 時） |

</details>

<!-- i18n-section: desktop-workbench -->

## デスクトップワークベンチ

オプションの[デスクトップワークベンチ (英語)](../gui.md)は、従来の対話型逆アセンブラのレイアウトとショートカットを踏襲し、逆アセンブル・グラフ・疑似コード・IR・16進・一覧ビュー、Visual Studio Code 風のテーマ、全11言語の UI、`.nddb` プロジェクトデータベースを提供します。解析は Qt に依存しない別ワーカープロセスで動作し、CLI のみのビルドは独立しています。対応ワークフローとリリース前に必要なプラットフォーム検証は[適格性記録 (英語)](../gui-qualification.md)を参照してください。

<!-- i18n-section: cli -->

## CLI

```text
neverd <command> [options] <binary>
```

<!-- i18n-section: pipeline -->

### パイプライン

| コマンド | 出力 | 説明 |
|----------|------|------|
| `lift` | `.ll` | LLVM IR へリフト |
| `decompile` | `.c` / `.sol` / `.rs` | `--language` で C、EVM Solidity、SBF Rust を選択 |
| `decompile -llvm` | `.c` | LLVM IR + 最適化経由 |
| `decompile --devirtualize` | `.c` + 任意の JSON | 実験的な x64 インタープリター復元。`--func` が必須。[契約と例](interpreter-recovery.md) |
| `mobile` | `.java` / `.c` / `.m` / `.swift` + JSON | 実験的: [Android](android.md), [iOS](ios.md) |
| `patch` | バイナリ | 機械語の書き換え |

```bash
neverd decompile program --func vm_entry --devirtualize --vm-control=r10 \
  --recovery-report recovery.json -o recovered.c
neverd patch -hello -o patched binary
neverd patch --from-ir repl.ll -o patched binary
neverd patch --from-source repl.c --entry replacement --func 0x401000 -o patched binary
neverd patch --mode inplace -o patched binary
neverd patch --subst --flatten --mba -o patched binary
```

ソースパッチでは Clang、NeverC の通常モード、GCC を選択できます。[検証済みのソース/object プロファイル](../source-patching.md)を参照してください。native object は生成側の機械語を保持し、コンパイラとランタイムの互換性、および例外の伝播はプロファイルごとに検証します。

関数の ARM/Thumb モードメタデータがない32ビット ARM バイナリでは、逆コンパイル前に入口モードを指定します。

```bash
neverd decompile --arm-function-mode=0xADDRESS:thumb -o output.c binary
```

曖昧な入口が複数ある場合はオプションを繰り返し、必要に応じて `:arm` を使用します。検証済みのバイナリメタデータと矛盾する指定はロードに失敗し、有効な指定は該当する入口だけに作用します。C API では `neverd_session_set_arm_function_mode()` により同じロード前設定を行えます。

<details>
<summary><strong>分析コマンド</strong></summary>

<br>

| コマンド | 用途 |
|----------|------|
| `info` / `dashboard` / `headers` | メタデータと概要 |
| `funcs` | 検出された関数 |
| `disasm` | 逆アセンブル（`--func` 名または hex） |
| `sym-explore` | ネイティブ LowIR の有界パス探索（`--func`、JSON 出力） |
| `audit` | ヒープの寿命に関する不具合と未初期化ローカルスタック読み取り（JSON） |
| `hunt` | 危険なコピーの境界超過とシンボリックな証拠。完全な計画がある場合は `process-input-v1` 再生証拠を追加（JSON schema v1） |
| `hex` | アドレスの hex dump |
| `cfg` / `callgraph` | CFG / コールグラフ（JSON；DOT/SVG 任意） |
| `xrefs` | クロスリファレンス |
| `switches` | プログラム全体の解析が復元した switch のジャンプテーブル |
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

<!-- i18n-section: sdk-and-plugins -->

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
[EVM C API 例](evm.md#c-api)を参照してください。

ネイティブ共有ライブラリと Python `.py` file は同じプラグイン lifecycle を使います。
`-DNEVERD_BUILD_PLUGINS=ON` でネイティブ example をビルドします。純粋 C descriptor、
callback、build/link、discovery、CLI workflow、ABI 制約は
[ネイティブプラグインガイド](plugins.md)を参照してください。Python 対応は
default で有効で、`-DNEVERD_ENABLE_PYTHON_PLUGINS=OFF` で完全に除外できます。
typed SDK と package workflow は [Python プラグインガイド](python-plugins.md)
にあります。両方とも `<neverd-dir>/plugins`、`~/.neverd/plugins`、
`$NEVERD_PLUGIN_PATH` を使用します。

<!-- i18n-section: dependencies -->

## 依存関係

| コンポーネント | 役割 | ソース |
|----------------|------|--------|
| **LLVM**（fork） | IR、最適化、コード生成、診断 | `third_party/llvm-project` またはプリビルド |
| **Capstone** | デコード | `third_party/capstone` |

第三者コンポーネントは各々のライセンスを保持します。

<!-- i18n-section: contributing -->

## 貢献

開発成果は **`dev`** ブランチへ統合します。環境構築、Release/Debug の手順、スタイル、フォーカステスト、プルリクエスト要件は[貢献ガイド](CONTRIBUTING.md)を参照してください。[アーキテクチャ](architecture.md)と[テスト](testing.md)のガイドでは、一般的な変更を対応するコードと検証スイートへマッピングしています。

<!-- i18n-section: license -->

## ライセンス

[GNU AGPL バージョン3のみ](../../LICENSE)。対象となる NeverD コードや派生物を再配布する際は、著作権・ライセンス・無保証の表示と、[NOTICE](../../NOTICE) にあるプロジェクトの帰属および出典を保持してください。AI/LLM を使った再利用や LLVM ベースのコード変換にも適用されます。

要件・範囲・例は[帰属と引用](ATTRIBUTION.md)を参照してください。追跡可能な参照にはソースファイルと正確なバージョンまたはコミットの明記を推奨します。[CITATION.cff](../../CITATION.cff) はソフトウェア引用用メタデータを提供しますが、引用だけでライセンス遵守を代替することはできません。

LLVM コンポーネントは Apache-2.0 WITH LLVM-exception ライセンスを保持します。Capstone は独自のライセンスを保持します。
