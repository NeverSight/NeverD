**語言**: [English](../../README.md) | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md) | [日本語](README.ja.md) | [한국어](README.ko.md) | [Français](README.fr.md) | [Deutsch](README.de.md) | [Español](README.es.md) | [Italiano](README.it.md) | [Русский](README.ru.md) | [العربية](README.ar.md)

<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../assets/neverd-logo-dark.svg">
  <img src="../assets/neverd-logo-light.svg" width="72" alt="NeverD">
</picture>

# NeverD

**AI 友好的二進位分析與反編譯引擎 — 1:1 提升，基於 LLVM**

PE · ELF · Mach-O &nbsp;|&nbsp; x86-64 · i386 · AArch64 · ARM32 &nbsp;|&nbsp; 純 C SDK

[![AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-blue.svg)](../../LICENSE)
[![C++20](https://img.shields.io/badge/Standard-C%2B%2B20-brightgreen.svg)](#建置)
[![Formats](https://img.shields.io/badge/Formats-PE%20%7C%20ELF%20%7C%20Mach--O-informational.svg)](#支援的目標)
[![Arch](https://img.shields.io/badge/Arch-x86__64%20%7C%20i386%20%7C%20AArch64%20%7C%20ARM-orange.svg)](#支援的目標)
[![SDK](https://img.shields.io/badge/SDK-Pure%20C%20API-lightgrey.svg)](#sdk-與外掛)

[文件](../README.zh-TW.md) · [路線圖](../roadmap/README.zh-TW.md) · [貢獻](CONTRIBUTING.zh-TW.md)

</div>

---

> GitHub 倉庫首頁固定展示英文 `README.md`。請使用上方語言連結查看在地化版本。

## 概覽

NeverD 是以 **1:1 指令級提升** 為核心的原生二進位分析與反編譯引擎。它載入 **PE**、**ELF**、**Mach-O**，用 [Capstone](https://www.capstone-engine.org/) 解碼，再透過四種 IR 表示搭配 **手寫語意** 完成處理——不是近似翻譯。目標是 **100% 語意保真**：已支援指令在 **LLVM IR**、**結構化 C** 或 **重寫後的二進位** 中保持完整可觀察行為。

**預設開啟 strict**：沒有 lifter 的指令拋出 `UnliftedInstruction`，不會跳過、猜測或靜默變成 `NOP`。

CLI、整合方與 AI 智慧體透過 **純 C API** 使用同一個引擎 **`libneverd`**，不直接連結 Capstone、LLVM 或內部 C++。

後續版本將在同一 IR 堆疊上增加 [EVM](../roadmap/README.zh-TW.md#2-evm-位元組碼反編譯) 與 [Solana eBPF / SBF](../roadmap/README.zh-TW.md#3-solana-ebpfsbf反編譯) 反編譯 — 見 [路線圖](../roadmap/README.zh-TW.md)。

## 為什麼選 NeverD？

- **1:1 語意** — 手寫 lifter；預設 strict 下未支援指令拋出例外
- **LLM 友好** — 結構化 C、LLVM IR 與 JSON 分析經純 C API 暴露，錯誤行為確定
- **一條管線，三種出口** — `lift` → LLVM IR · `decompile` → C · `patch` → 重寫二進位
- **二進位重寫** — PE / ELF / Mach-O，section 跳板或 inplace 覆蓋
- **分析工具集** — CLI、除錯資訊、簽名、外掛，以及可選混淆通路

## 支援的目標

| | **x86-64** | **i386** | **AArch64** | **ARM32** |
|---|:---:|:---:|:---:|:---:|
| **PE** (Windows) | ✓ | ✓ | ✓ | ✓ |
| **ELF** (Linux / Android) | ✓ | ✓ | ✓ | ✓ |
| **Mach-O** (macOS / iOS) | ✓ | ✓ | ✓ | ✓ |

> 矩陣中的每個單元格都已實作，但整合測試深度不同。詳見[架構覆蓋矩陣](../architecture.zh-TW.md#support-and-test-depth)。Mach-O i386 使用 `thin` 可重定位物件，因為現代 macOS 無法連結歷史 i386 可執行檔。

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
| `decompile` | `.c` | 結構化 C（HighIR） |
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
