**語言**: [English](../../README.md) | [简体中文](../zh-CN/project.md) | [繁體中文](project.md) | [日本語](../ja/project.md) | [한국어](../ko/project.md) | [Français](../fr/project.md) | [Deutsch](../de/project.md) | [Español](../es/project.md) | [Italiano](../it/project.md) | [Русский](../ru/project.md) | [العربية](../ar/project.md)

<!-- i18n-source: 7d465e53d8b02df92cfd1d84c9817ee79377e7094f58b046232e3ea5523fe1e5 -->

<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../assets/neverd-logo-dark.svg">
  <img src="../assets/neverd-logo-light.svg" width="72" alt="NeverD">
</picture>

# NeverD

**AI 友好的二進位分析與反編譯引擎 — 1:1 提升，基於 LLVM**

PE · ELF · Mach-O · EVM · Solana SBF &nbsp;|&nbsp; x86-64 · i386 · AArch64 · ARM32 · EVM256 · SBF &nbsp;|&nbsp; C + Python SDK

[![AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-blue.svg)](../../LICENSE)
[![C++20](https://img.shields.io/badge/Standard-C%2B%2B20-brightgreen.svg)](#建置)
![Platform](https://img.shields.io/badge/Platform-macOS%20%7C%20Linux%20%7C%20Windows-informational.svg)
[![SDK](https://img.shields.io/badge/SDK-C%20%2B%20Python-orange.svg)](#sdk-與外掛)

[文件](README.md) · [Android](android.md) · [iOS](ios.md) · [路線圖](roadmap.md) · [貢獻](CONTRIBUTING.md)

</div>

---

> GitHub 倉庫首頁固定展示英文 `README.md`。請使用上方語言連結查看在地化版本。

<!-- i18n-section: overview -->

## 概覽

NeverD 是以 **1:1 指令級提升** 為核心的原生與智慧合約分析/反編譯引擎。它載入 **PE**、**ELF**、**Mach-O**、傳統 **EVM** 位元組碼與 Solana **SBF ELF** 程式。原生目標由 [Capstone](https://www.capstone-engine.org/) 解碼；EVM 與 SBF 使用各自感知版本的 decoder 和分階段 IR。所有路徑均採手寫語意。已支援指令在 **LLVM IR**、**C**、**面向 SBF 的 Rust**、**面向 EVM 的 Solidity 重建**，或原生目標的**重寫後二進位**中保持可觀察行為。

**預設開啟 strict**：沒有 lifter 的指令拋出 `UnliftedInstruction`，不會跳過、猜測或靜默變成 `NOP`。

CLI、整合方與 AI 智慧體透過 **純 C API** 使用同一個引擎 **`libneverd`**，不直接連結 Capstone、LLVM 或內部 C++。

輸入格式、host contract 與限制詳見 [EVM 指南](evm.md)及 [Solana SBF 指南](sbf.md)。

實驗性 CLI `neverd mobile app.apk -o recovered-app` 可從 APK、DEX 與 smali 還原 Java，並產生 `report.json`。Android 還原僅使用 NeverD 內建的 C++20 引擎，執行時不需要 Python 或 Java。包含空格的路徑必須加上引號。支援的輸入、報告與還原限制請見 [Android 指南](android.md)。

實驗性 iOS 流程 `neverd mobile App.ipa -o recovered-ios` 從 IPA、`.app` 或 Mach-O 輸出原生 C 和受支援的 Objective-C/Swift 原始碼，保留執行階段配置、原始碼單元和逐方法省略原因；生成程式碼不透過橋接呼叫原始二進位檔。環境、覆蓋率與獨立編譯驗證請見 [iOS 指南](ios.md)。

實驗性的[直譯器原始碼還原](interpreter-recovery.md)使用 `neverd decompile --devirtualize --func ENTRY`，透過共用 LowIR/MedIR 管線，將支援的已連結 x64 ELF/PE 直譯器特化為 HighC 或 LLVMC。控制提示用來區分解碼器情境，不固定執行時輸入。未解析控制流程、不支援的語義與預算耗盡都會明確失敗；此模式不證明二進位替換或例外等價性。

恢復預算可明確設定：`--vm-max-fields`、`--vm-max-refinements`、`--vm-max-queries` 的預設值仍為 16、16、4096。相容的 v3 C API 與失敗規則見恢復指南。

恢復也提供 `--vm-chain-transfers=N`（預設 0）和 `--vm-no-control-discovery`。串接在已證明唯一目標的控制轉移之間保留符號關聯；達到上限後回到普通 CFG 邊界。機器狀態恢復可透過 `--vm-entry-frame=begin:end` 宣告未經執行時檢查、不會回繞的入口 RSP 偏移範圍。精確數值前提會寫入產生的 C 和報告；它不授予記憶體存取權限，也不構成等價證明。

機器狀態恢復支援以 `--vm-entry-alignment=A:R` 宣告並檢查入口 RSP 同餘域。`A` 必須是正的二次冪，且 `R < A`。其他入口在客體記憶體存取或狀態寫入前回傳狀態 2。根位址高位仍自由，預設不假定對齊；此選項不提供原生等價認證。

`--vm-external-stores-disjoint-frame` 為機器狀態恢復加入顯式、未經執行期檢查的前提：每次外部 STORE 的完整範圍必須避開 `--vm-entry-frame`。這種寫入只保留該區間內已有的事實，不約束 LOAD 或外部指標之間的別名；預設行為仍保守。原生證明 API 拒絕此域。

超過 SSA 建構限制的大型恢復函式可透過 `--llvm` 使用有界的純量可變儲存契約。入口輸入、迴圈攜帶值和較早讀取的語意得到保留。不支援的隱含狀態、向量暫存器參數、映像重定位、歧義儲存和畸形控制流程會明確失敗；HighC 拒絕此回退路徑。原始碼輸出仍遵循既有機器狀態契約，不新增等價證明憑證。

獨立的 C++ 迴圈證明 API 可在預算內推導巢狀迴圈不變量與字典序排名，再檢查原生程式碼至 LowIR 的精化關係，詳見[恢復指南](interpreter-recovery.md)；它不證明輸出 C 的等價性。

獨立的 C++ `checkBinaryLLVMRefinement` API 對精確 LLVM 產物組合全新的原生與 LLVM 檢查；C 編譯仍不在證明範圍內。

PE 恢復也會認證偏好基底位址的 DIR64 位元組並排除匯入寫入；固定映像契約不證明 ASLR 或初始化等價性。

在明確的機器狀態契約下，解譯器恢復支援有界入口堆疊對齊分區及內部 `RET imm16` 清理。這些分區的自動原生至 LLVM 證明組合尚未完成。

有界 `REP MOVS/STOS` 還原保留逐元素順序與重疊行為；原始指令的證明覆蓋仍待完成。

<!-- i18n-section: why-neverd -->

## 為什麼選 NeverD？

- **1:1 語意** — 手寫 lifter；預設 strict 下未支援指令拋出例外
- **LLM 友好** — 結構化 C、LLVM IR 與 JSON 分析經純 C API 暴露，錯誤行為確定
- **一條管線，多種出口** — `lift` → LLVM IR · `decompile` → C/Solidity/Rust · `patch` → 重寫原生二進位
- **二進位重寫** — PE / ELF / Mach-O，section 跳板或 inplace 覆蓋
- **分析工具集** — CLI、除錯資訊、簽名、外掛，以及可選混淆通路

<!-- i18n-section: supported-targets -->

## 支援的目標

| | **x86-64** | **i386** | **AArch64** | **ARM32** |
|---|:---:|:---:|:---:|:---:|
| **PE** (Windows) | ✓ | ✓ | ✓ | ✓ |
| **ELF** (Linux / Android) | ✓ | ✓ | ✓ | ✓ |
| **Mach-O** (macOS / iOS) | ✓ | ✓ | ✓ | ✓ |

> 矩陣中的每個單元格都已實作，但整合測試深度不同。詳見[架構覆蓋矩陣](architecture.md#support-and-test-depth)。Mach-O i386 使用 `thin` 可重定位物件，因為現代 macOS 無法連結歷史 i386 可執行檔。

傳統 EVM 位元組碼獨立於原生 container：從 Frontier 到 Fusaka 的 150 個已分配
opcode 全部進入專用 Low/Med/High IR、已驗證 LLVM `i256`、C23 `_BitInt(256)`
與 Solidity 輸出。詳見 [EVM 反編譯](evm.md)。

Solana SBF v0-v4 ELF 程式使用專用 strict loader、完整版本化 ISA metadata、
Low/Med/High IR、已驗證 LLVM、可攜式 C11 與安全 stable Rust。詳見
[Solana SBF 反編譯](sbf.md)。

<!-- i18n-section: mobile-source-recovery -->

### 行動平台原始碼還原

實驗性 `neverd mobile` CLI 支援以下行動平台輸入與原始碼輸出。

| 平台 | 輸入 | 輸出 |
|------|------|------|
| [Android](android.md) | APK（含 multidex）、DEX、smali 檔案／目錄 | Java 與 JSON 報告 |
| [iOS](ios.md) | IPA、`.app`、Mach-O（arm64 / x86_64） | 原生 C、受支援的 Objective-C / Swift 原始碼與 JSON 覆蓋率報告 |

還原取決於支援的程式碼模式；覆蓋範圍與限制見[行動平台總覽（英文）](../mobile.md)及各平台指南。

<!-- i18n-section: cpu-workloads -->

### CPU 執行與客體環境

CPU 執行分離 ISA 准入、客體記憶體、後端傳輸與客體 OS 策略。`NEVERD_ENABLE_CPU_EMULATION` 啟用 x64/ARM64 CPU 層；`NEVERD_ENABLE_DRIVER_EMULATION` 加入有界 x64 Windows WDM/KMDF 環境。`linux-elf64-v1` 設定檔執行受支援的 Linux ELF 程序。參見[CPU 執行](cpu-execution.md)、[客體程序模擬](process-emulation.md)及[Windows 驅動程式模擬](driver-emulation.md)。

`windows-pe64-v1` 支援有界 Windows x64/ARM64 主控台程序，包括 PEB/TEB、靜態與動態 TLS、`DllMain`、具名 Win32 API 和明確的無環 DLL 圖。客體模組支援依名稱／序號匯入程式碼與資料、DIR64 重定位、轉送匯出及真實載入器串列身分。`LoadLibraryA`／`LoadLibraryW`、`FreeLibrary` 和 `GetProcAddress` 使用設定的模組目錄。CRT／GUI、使用者態 SEH、執行緒及通用 Windows 應用程式相容性仍待完成；原生 ARM64 KVM/WHP 證據仍缺失。

`GetEnvironmentVariableW`, `SetEnvironmentVariableW`, `GetEnvironmentStringsW`, `FreeEnvironmentStringsW`, `ExpandEnvironmentStringsW` 共用 PEB 程序參數中的即時客體環境區塊。名稱限 ASCII 且忽略大小寫，值為 UTF-16。修改前驗證輸入、容量及可寫記憶體。快照不受後續修改影響，釋放時回收客體記憶體。模型的環境區塊上限為 64 KiB；字串與展開操作有明確邊界並檢查工作負載期限。未知指標歸屬、格式錯誤的環境區塊、ANSI 字碼頁及展開緩衝區重疊仍不支援。`WindowsEnvironmentTests.cpp` 在可用後端比較原創 x64/ARM64 範例，CI 必須執行獨立的原生 Windows 對照。

Windows 虛擬記憶體新增 `VirtualAlloc`、`VirtualFree`、`VirtualProtect`、`VirtualQuery` 及目前行程的 `FlushInstructionCache`。OS 層管理保留區域，`AddressSpace` 統一管理已認可頁面、權限和實體儲存。測試涵蓋動態程式碼改寫、存取錯誤和記憶體額度回收。

`driver-strict` / `checked-x64-v1` 支援匹配 Linux x64 主機的 KVM 與 Windows x64 主機的 WHP；`auto` 選取對應原生傳輸，跨 ISA 執行選取 Unicorn。明確指定 Unicorn 及原有 V1 API 保留可移植軟體設定。原生執行在進入 CPU 前檢查規範位址和指令效果；硬體不可用時明確失敗且不回退。不支援的指令與 OS 行為仍明確報錯。Windows x64 原生 CI 在停用 Unicorn 的設定下通過全部 359 項必測檢查：131 項 CPU 檢查、26 個內建映像與 46 個 WDK 映像及 40 個情境組合在首選和重定位位址產生的 224 項驅動程式結果，以及 4 項 SEH 邊界檢查 ([`9d4c130c`](https://github.com/NeverSight/NeverD/actions/runs/36981864458)). 原生 ARM64 實機證據仍待補充，這不表示相容任意驅動程式或 Android/Darwin 環境。

受檢 x64 現支援一般 RAM 上的 `MOVS/STOS/LODS` 與 `CLD/STD`，並逐元素驗證恢復、取消及跨頁存取。依賴 CPU 型號的零次數高位元行為與 STOS/LODS 裝置運算元仍不在契約內。

受檢 x64 也支援一般 RAM 上的 `CMPS/SCAS` 與 `REPE/REPNE`，涵蓋算術旗標、提前終止、逐元素停止與錯誤恢復；裝置比較仍不支援。

`checked-aarch64-v1` 與 `checked-user-aarch64-v1` 提供有界 ARM64 FP32/FP64、定寬 SIMD 及完整 FPCR/FPSR/向量狀態。匹配 Linux ARM64 主機使用 KVM，Windows ARM64 主機使用 WHP，跨 ISA 使用 Unicorn。ARM64 原生執行仍待實機驗證；Windows 驅動程式載入仍限 x64。

x64 與 ARM64 原生啟動自檢在獨占記憶體租約下驗證有界的完整狀態執行。XSAVE 封包和包含 ISA 身分的頁表快取由唯一權威層管理；ARM64 原生工作負載證據仍未完整。

原生 x64 的 `FOP/FIP/FDP` 遵循主機儲存、還原規則：AMD 可能清零未生效的 x87 例外中繼資料。啟動自檢透過未遮罩的待處理例外驗證這些欄位。

<!-- i18n-section: how-it-works -->

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

<!-- i18n-section: quick-start -->

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

# Android / iOS 原始碼還原（實驗性）
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

建置時簽名庫安裝到 `build/bin/signatures/`。`sigs --auto` 依格式、架構、位寬選擇匹配庫集。若 PE 檔的 Rich 標頭指出其連結器所屬的 Visual Studio 版本，則只載入該版本的 `vs<year>.pat`，以及不屬於任何版本的庫。`--sig-base <dir>` 以相同方式從另一個簽名目錄樹選擇。 1 MiB 以上的模式檔只解析一次：其模組保存在使用者快取目錄下的 `neverd/signatures`，之後載入時直接對映。`NEVERD_SIGNATURE_CACHE` 可指定其他目錄，設為 `off` 則關閉快取。

<!-- i18n-section: building -->

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
  -DNEVERD_LLVM_PREBUILT_TAG=neverd-llvm-v23.0.0-r3
cmake --build build
```

NeverD 常規的 push 與 pull request CI 刻意從原始碼編譯 LLVM submodule。手動執行 `CI` 工作流程時勾選 `use_prebuilt_llvm` 可驗證已發布的套件；只有手動選擇 `true` 才會啟用預編譯 LLVM，不勾選則與自動 CI 走同一條原始碼編譯路徑。

發布套件依執行 CMake 的主機選擇：

| 主機 | 發布產物 |
|------|----------|
| macOS arm64 | `neverd-llvm-macos-arm64.tar.xz` |
| Linux x86_64 | `neverd-llvm-linux-x86_64.tar.xz` |
| Windows x64 | `neverd-llvm-windows-x64.zip` |

每個封存檔解壓縮至 `~/.cache/neverd-llvm/<tag>/<arch>/`（或 `NEVERD_LLVM_PREBUILT_CACHE_DIR` 指定的目錄）前，都會核對 `cmake/NeverDLLVMPrebuilt.cmake` 中固定的摘要；未被這些固定值描述的 tag 則核對隨包發布的 `.sha256`。預設固定版本的 `BUILDINFO.txt` 也必須記錄完全一致的 LLVM 子模組提交。發布建置在 macOS 與 Linux 使用 ccache，Windows clang-cl 使用 sccache 與 GitHub Actions 快取後端；編譯器快取只加速重建，不作為發布產物上傳。

預設套件修訂版為 `neverd-llvm-v23.0.0-r3`。其 Git tag、發布目標、原始碼提交與三個封存檔摘要共同構成不可變的版本化原始碼固定值。若現有建置目錄快取舊基礎 tag、`neverd-llvm-v23.0.0-r1` 或 `neverd-llvm-v23.0.0-r2`，會自動遷移至 `r3`，除非明確設定 `NEVERD_LLVM_PREBUILT_SHA256`。`Prebuilt LLVM Audit` 工作流程於 push、pull request 及每六小時執行，呼叫 `scripts/audit_prebuilt_llvm_release.py`，核對 GitHub 目前發布及各校驗和附屬檔案。

若 LLVM fork 原始碼改變而 LLVM 仍回報 `23.0.0`，應發布下一個套件修訂版——`neverd-llvm-v23.0.0-r4`，之後為 `-r5`——不要覆寫既有發布，也不要虛構 LLVM 版本 `23.0.1`：

```bash
gh workflow run neverd-release.yml \
  --repo NeverSight/llvm-project \
  --ref main \
  -f release_tag=neverd-llvm-v23.0.0-r4 \
  -f overwrite_existing_assets=false
```

工作流程成功後，同時更新 `cmake/NeverDLLVMPrebuilt.cmake` 中的預設 tag、固定提交與三個摘要。新套件快取於 `.cache/neverd-llvm/<tag>`；過期或重新發布的封存檔會在解壓縮前失敗。`overwrite_existing_assets` 僅用於舊版復原，正常的修訂版發布保持關閉。

**產物**

| 路徑 | 說明 |
|------|------|
| `build/bin/neverd` | 統一 CLI |
| `build/bin/neverd-bench` | 基準測試（JSON） |
| `build/bin/neverd-sigmaker` | 從靜態庫產生 `.pat` |
| `build/bin/libneverd.*` | 引擎共用函式庫 |
| `build/bin/sdk/` | C SDK 的 canonical include root；使用保留 `neverd/sdk/` 階層的 `<neverd/sdk/NeverDCAPI.h>` 或 `<neverd/sdk/NeverDPlugin.h>` |
| `build/bin/sdk/python/` | 具型別資訊的 Python 外掛套件與範例 |
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

聚焦目標、CTest 標籤、fixture 要求與跨格式重寫網格詳見[測試 NeverD](testing.md)。

**CMake 選項**

| 選項 | 預設 | 說明 |
|------|------|------|
| `NEVERD_LLVM_PREBUILT` | `OFF` | CI 預編譯 LLVM |
| `NEVERD_BUILD_SHARED` | `ON` | 建置 `libneverd` |
| `NEVERD_ENABLE_PYTHON_PLUGINS` | `ON` | 內嵌 CPython 3.10+ 外掛支援 |
| `NEVERD_BUILD_PLUGINS` | `OFF` | 範例外掛 |
| `BUILD_TESTING` | `OFF` | 單元測試 |
| `NEVERD_ENABLE_SEMANTIC_TESTS` | `ON` | 依賴 Unicorn 的語意測試組（啟用 `BUILD_TESTING=ON` 時） |

</details>

<!-- i18n-section: desktop-workbench -->

## 桌面工作台

選用的 [Qt Quick 桌面工作台 (英文)](../gui.md)提供可停駐的指令、CFG、十六進位、C 與 IR 檢視，支援全部 11 種介面語言、持久化註記與 MCP 連線。分析在不依賴 Qt 的獨立工作程序中執行，純 CLI 建置保持獨立。支援的工作流程及發布前仍需完成的平台驗證見[驗收紀錄 (英文)](../gui-qualification.md)。

<!-- i18n-section: cli -->

## CLI

```text
neverd <command> [options] <binary>
```

<!-- i18n-section: pipeline -->

### 管線命令

| 命令 | 輸出 | 說明 |
|------|------|------|
| `lift` | `.ll` | 提升到 LLVM IR |
| `decompile` | `.c` / `.sol` / `.rs` | 透過 `--language` 選擇 C、EVM Solidity 或 SBF Rust |
| `decompile -llvm` | `.c` | 經 LLVM IR + 最佳化器 |
| `decompile --devirtualize` | `.c` + 選用 JSON | 實驗性 x64 直譯器還原；需要 `--func`；[契約與範例](interpreter-recovery.md) |
| `mobile` | `.java` / `.c` / `.m` / `.swift` + JSON | 實驗性: [Android](android.md), [iOS](ios.md) |
| `patch` | 二進位 | 重寫機器碼 |

```bash
neverd decompile program --func vm_entry --devirtualize --vm-control=r10 \
  --recovery-report recovery.json -o recovered.c
neverd patch -hello -o patched binary
neverd patch --from-ir repl.ll -o patched binary
neverd patch --from-c repl.c --func 0x401000 -o patched binary
neverd patch --mode inplace -o patched binary
neverd patch --subst --flatten --mba -o patched binary
```

若 32 位元 ARM 二進位檔缺少函式的 ARM/Thumb 模式中繼資料，請在反編譯前宣告入口模式：

```bash
neverd decompile --arm-function-mode=0xADDRESS:thumb -o output.c binary
```

可重複使用此選項指定其他有歧義的入口，必要時使用 `:arm`。與已驗證二進位中繼資料衝突的宣告會使載入失敗；有效宣告只影響指定入口。C API 透過 `neverd_session_set_arm_function_mode()` 提供相同的載入前設定。

<details>
<summary><strong>分析命令</strong></summary>

<br>

| 命令 | 功能 |
|------|------|
| `info` / `dashboard` / `headers` | 中繼資料與概覽 |
| `funcs` | 發現的函式 |
| `disasm` | 反組譯（`--func` 名稱或十六進位） |
| `sym-explore` | 有界原生 LowIR 路徑探索（`--func`；JSON 輸出） |
| `audit` | 堆積物件生命週期缺陷及未初始化的區域堆疊讀取（JSON） |
| `hunt` | 危險複製越界與符號見證；存在完整方案時附加 `process-input-v1` 重播證據（JSON schema v1） |
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

<!-- i18n-section: sdk-and-plugins -->

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
[EVM C API 範例](evm.md#c-api)。

原生共享函式庫與 Python `.py` 檔案使用相同的外掛生命週期。透過
`-DNEVERD_BUILD_PLUGINS=ON` 建置原生範例；純 C 描述元、回呼、建置/連結步驟、
探索順序、CLI 工作流程與 ABI 限制請參閱[原生外掛指南](plugins.md)。Python
支援預設啟用，可用 `-DNEVERD_ENABLE_PYTHON_PLUGINS=OFF` 完全移除；typed SDK 與
package 工作流程見 [Python 外掛指南](python-plugins.md)。兩者都使用
`<neverd-dir>/plugins`、`~/.neverd/plugins` 和 `$NEVERD_PLUGIN_PATH`。

<!-- i18n-section: dependencies -->

## 相依元件

| 元件 | 作用 | 來源 |
|------|------|------|
| **LLVM**（fork） | IR、最佳化、程式碼產生、診斷 | `third_party/llvm-project` 或預編譯 |
| **Capstone** | 解碼 | `third_party/capstone` |

第三方保留各自授權條款。

<!-- i18n-section: contributing -->

## 貢獻

開發成果合入 **`dev`** 分支。環境設定、Release/Debug 指引、風格、聚焦測試與拉取請求要求見[貢獻指南](CONTRIBUTING.md)。[架構](architecture.md)與[測試](testing.md)指南將常見變更映射到對應程式碼與驗證套件。

<!-- i18n-section: license -->

## 授權條款

[GNU AGPL 僅第 3 版](../../LICENSE)。重新散布受其約束的 NeverD 程式碼或改作時，應保留著作權、授權與免責聲明，包括 [NOTICE](../../NOTICE) 中的專案署名與來源。這也適用於 AI/LLM 輔助重用及基於 LLVM 的程式碼轉換。

要求、適用範圍與範例見[署名與來源引用](ATTRIBUTION.md)。為方便追溯，建議引用原始碼檔案及確切版本或提交。[CITATION.cff](../../CITATION.cff) 提供軟體引用中繼資料；引用本身不能取代授權合規要求。

LLVM 元件保留 Apache-2.0 WITH LLVM-exception 授權條款。Capstone 保留其自身授權條款。
