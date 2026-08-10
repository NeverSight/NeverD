**語言**: [English](../../README.md) | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md) | [日本語](README.ja.md) | [한국어](README.ko.md) | [Français](README.fr.md) | [Deutsch](README.de.md) | [Español](README.es.md) | [Italiano](README.it.md) | [Русский](README.ru.md) | [العربية](README.ar.md)

<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../assets/neverd-logo-dark.svg">
  <img src="../assets/neverd-logo-light.svg" width="72" alt="NeverD">
</picture>

# NeverD

**AI 友好的二進位分析與反編譯引擎 — 1:1 提升，基於 LLVM**

PE · ELF · Mach-O · EVM · Solana SBF &nbsp;|&nbsp; x86-64 · i386 · AArch64 · ARM32 · EVM256 · SBF &nbsp;|&nbsp; 純 C SDK

[![AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-blue.svg)](../../LICENSE)
[![C++20](https://img.shields.io/badge/Standard-C%2B%2B20-brightgreen.svg)](#建置)

[文件](../README.zh-TW.md) · [路線圖](../roadmap/README.zh-TW.md) · [貢獻](CONTRIBUTING.zh-TW.md)

</div>

---

> GitHub 倉庫首頁固定展示英文 `README.md`。請使用上方語言連結查看在地化版本。

## 概覽

NeverD 是以 **1:1 指令級提升** 為核心的原生與智慧合約分析/反編譯引擎。它載入 **PE**、**ELF**、**Mach-O**、傳統 **EVM** 位元組碼與 Solana **SBF ELF** 程式。原生目標由 [Capstone](https://www.capstone-engine.org/) 解碼；EVM 與 SBF 使用各自感知版本的 decoder 和分階段 IR。所有路徑均採手寫語意。已支援指令在 **LLVM IR**、**C**、**面向 SBF 的 Rust**、**面向 EVM 的 Solidity 重建**，或原生目標的**重寫後二進位**中保持可觀察行為。

**預設開啟 strict**：沒有 lifter 的指令拋出 `UnliftedInstruction`，不會跳過、猜測或靜默變成 `NOP`。

CLI、整合方與 AI 智慧體透過 **純 C API** 使用同一個引擎 **`libneverd`**，不直接連結 Capstone、LLVM 或內部 C++。

輸入格式、host contract 與限制詳見 [EVM 指南](../evm.zh-TW.md)及 [Solana SBF 指南](../sbf.zh-TW.md)。

## 為什麼選 NeverD？

- **1:1 語意** — 手寫 lifter；預設 strict 下未支援指令拋出例外
- **LLM 友好** — 結構化 C、LLVM IR 與 JSON 分析經純 C API 暴露，錯誤行為確定
- **一條管線，多種出口** — `lift` → LLVM IR · `decompile` → C/Solidity/Rust · `patch` → 重寫原生二進位
- **二進位重寫** — PE / ELF / Mach-O，section 跳板或 inplace 覆蓋
- **分析工具集** — CLI、除錯資訊、簽名、外掛，以及可選混淆通路

## 支援的目標

| | **x86-64** | **i386** | **AArch64** | **ARM32** |
|---|:---:|:---:|:---:|:---:|
| **PE** (Windows) | ✓ | ✓ | ✓ | ✓ |
| **ELF** (Linux / Android) | ✓ | ✓ | ✓ | ✓ |
| **Mach-O** (macOS / iOS) | ✓ | ✓ | ✓ | ✓ |

> 矩陣中的每個單元格都已實作，但整合測試深度不同。詳見[架構覆蓋矩陣](../architecture.zh-TW.md#support-and-test-depth)。Mach-O i386 使用 `thin` 可重定位物件，因為現代 macOS 無法連結歷史 i386 可執行檔。

傳統 EVM 位元組碼獨立於原生 container：從 Frontier 到 Fusaka 的 150 個已分配
opcode 全部進入專用 Low/Med/High IR、已驗證 LLVM `i256`、C23 `_BitInt(256)`
與 Solidity 輸出。詳見 [EVM 反編譯](../evm.zh-TW.md)。

Solana SBF v0-v4 ELF 程式使用專用 strict loader、完整版本化 ISA metadata、
Low/Med/High IR、已驗證 LLVM、可攜式 C11 與安全 stable Rust。詳見
[Solana SBF 反編譯](../sbf.zh-TW.md)。

## 工作原理

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
  → runtime 正規化 + hardfork-aware decode
  → EVM LowIR → EVM stack-SSA MedIR → recovered EVM HighIR
       ├─ lift        → verified LLVM i256/i512
       └─ decompile   → C23 _BitInt(256) 或 Solidity reconstruction

Solana SBF ELF (v0-v4)
  → 感知版本的 legacy/strict loader + verifier
  → SBF LowIR → 正規化 MedIR → 復原的 SBF HighIR
       ├─ lift        → 已驗證 LLVM i64 runtime ABI
       └─ decompile   → 可攜式 C11 或安全 stable Rust
```

| 階段 | 作用 |
|------|------|
| **LowIR** | 約 77 種 `NdOp` + CFG |
| **MedIR** | 型別、呼叫慣例、記憶體模型、SSA |
| **HighIR** | 結構化控制流（`if` / `while` / `for`） |
| **LLVM** | 最佳化、輸出 C，或產生機器碼 |

## 快速開始

```bash
git submodule update --init --recursive
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# 管線
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

建置時簽名庫安裝到 `build/bin/signatures/`。`sigs --auto` 依格式、架構、位寬選擇匹配庫集。

## 建置

**需求：** CMake ≥ 3.20 · Ninja · C++20 編譯器 · Git submodule（LLVM fork + Capstone）

```bash
git submodule update --init --recursive
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

首次設定會本地編譯 LLVM fork（常 30–60 分鐘），之後為增量建置。預設見 `CMakePresets.json`：`release` / `relwithdebinfo` / `debug`。

<details>
<summary><strong>預編譯 LLVM · 產物 · 測試 · CMake 選項</strong></summary>

<br>

**預編譯 LLVM**

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DNEVERD_LLVM_PREBUILT=ON \
  -DNEVERD_LLVM_PREBUILT_TAG=neverd-llvm-v23.0.0
cmake --build build
```

**產物**

| 路徑 | 說明 |
|------|------|
| `build/bin/neverd` | 統一 CLI |
| `build/bin/neverd-bench` | 基準測試（JSON） |
| `build/bin/neverd-sigmaker` | 從靜態庫產生 `.pat` |
| `build/bin/libneverd.*` | 引擎共用函式庫 |
| `build/bin/sdk/` | `NeverDCAPI.h`、`NeverDPlugin.h` |
| `build/bin/signatures/` | 內建簽名庫 |

**測試**

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --target check-neverd
```

| 目標 | 說明 |
|------|------|
| `check-neverd` | 全部測試 |
| `check-neverd-semantic` | 僅語意 roundtrip（Unicorn） |

聚焦目標、CTest 標籤、fixture 要求與跨格式重寫網格詳見[測試 NeverD](../testing.zh-TW.md)。

**CMake 選項**

| 選項 | 預設 | 說明 |
|------|------|------|
| `NEVERD_LLVM_PREBUILT` | `OFF` | CI 預編譯 LLVM |
| `NEVERD_BUILD_SHARED` | `ON` | 建置 `libneverd` |
| `NEVERD_BUILD_PLUGINS` | `OFF` | 範例外掛 |
| `BUILD_TESTING` | `OFF` | 單元測試 |

</details>

## CLI

```text
neverd <command> [options] <binary>
```

### 管線命令

| 命令 | 輸出 | 說明 |
|------|------|------|
| `lift` | `.ll` | 提升到 LLVM IR |
| `decompile` | `.c` / `.sol` / `.rs` | 透過 `--language` 選擇 C、EVM Solidity 或 SBF Rust |
| `decompile -llvm` | `.c` | 經 LLVM IR + 最佳化器 |
| `patch` | 二進位 | 重寫機器碼 |

```bash
neverd patch -hello -o patched binary
neverd patch --from-ir repl.ll -o patched binary
neverd patch --from-c repl.c --func 0x401000 -o patched binary
neverd patch --mode inplace -o patched binary
neverd patch --subst --flatten --mba -o patched binary
```

<details>
<summary><strong>分析命令</strong></summary>

<br>

| 命令 | 功能 |
|------|------|
| `info` / `dashboard` / `headers` | 中繼資料與概覽 |
| `funcs` | 發現的函式 |
| `disasm` | 反組譯（`--func` 名稱或十六進位） |
| `hex` | 依位址十六進位傾印 |
| `cfg` / `callgraph` | CFG / 呼叫圖（JSON；可選 DOT/SVG） |
| `xrefs` | 交叉參照 |
| `strings` / `search` | 字串 / 位元組或文字搜尋 |
| `imports` / `exports` / `symbols` / `relocs` | 表 |
| `segments` / `sections` / `entrypoints` | 配置 |
| `diff` | 比較兩個二進位（`-a` / `-b`） |
| `sigs` | 簽名（`--auto`） |
| `rename` / `annotate` / `bookmarks` | 工作階段標註 |
| `export` | 匯出結果 |
| `plugins` | 列出或執行外掛 |

大多數分析命令支援 `--json`。

</details>

## SDK 與外掛

整合方使用 **`libneverd`** 的 **純 C API**：

| 標頭 | 用途 |
|------|------|
| `NeverDCAPI.h` | 工作階段、提升、反編譯、patch、IR / CFG、標註 |
| `NeverDPlugin.h` | 動態函式庫外掛 ABI |

```c
neverd_session_t s = neverd_session_create();
neverd_session_load(s, "binary.exe");
neverd_session_analyze(s);

const char *c = neverd_decompile(s, 0x401000);
neverd_free_string(c);
neverd_session_destroy(s);
```

EVM 使用 `neverd_decompile_all_ex(..., NEVERD_OUTPUT_SOLIDITY, ...)` 明確選擇
Solidity；舊 `neverd_decompile_all` 仍輸出 C。參見
[EVM C API 範例](../evm.zh-TW.md#c-api)。

`-DNEVERD_BUILD_PLUGINS=ON` 建置範例外掛。載入路徑：`<neverd-dir>/plugins`、`~/.neverd/plugins`、`$NEVERD_PLUGIN_PATH`。

## 相依元件

| 元件 | 作用 | 來源 |
|------|------|------|
| **LLVM**（fork） | IR、最佳化、程式碼產生、診斷 | `third_party/llvm-project` 或預編譯 |
| **Capstone** | 解碼 | `third_party/capstone` |

第三方保留各自授權條款。

## 貢獻

開發成果合入 **`dev`** 分支。環境設定、Release/Debug 指引、風格、聚焦測試與拉取請求要求見[貢獻指南](CONTRIBUTING.zh-TW.md)。[架構](../architecture.zh-TW.md)與[測試](../testing.zh-TW.md)指南將常見變更映射到對應程式碼與驗證套件。

## 授權條款

[AGPL-3.0](../../LICENSE)

LLVM 元件保留 Apache-2.0 WITH LLVM-exception 授權條款。Capstone 保留其自身授權條款。
