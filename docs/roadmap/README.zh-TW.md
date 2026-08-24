**語言**: [English](README.md) | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md) | [日本語](README.ja.md) | [한국어](README.ko.md) | [Français](README.fr.md) | [Deutsch](README.de.md) | [Español](README.es.md) | [Italiano](README.it.md) | [Русский](README.ru.md) | [العربية](README.ar.md)

[← 文件索引](../README.zh-TW.md)

# NeverD 路線圖

本文檔概述 NeverD 在現有原生 PE / ELF / Mach-O 管線之外的主要規劃方向。全域原則不變：**1:1 指令級提升**、**strict 顯式失敗**（不支援則報錯，不靜默跳過），以及同一套 **四級 IR** 支撐 lift / decompile / patch。

---

## 1. 原生格式補齊

完成 loader 已部分識別、但格式級尚未端到端打通的目標，使支援矩陣與使用者真實可用能力一致。

| 項目 | 說明 |
|------|------|
| PE AArch64 | Windows ARM64：unwind/`.pdata`、跳板、rewrite 往返 |
| PE ARM32（Thumb-2） | Windows on ARM 僅 Thumb；解碼/發射必須遵守該模式 |
| Mach-O i386 | 應用常見 clang 重定位；優先 thin object |

### 設計原則

- 格式×架構儲存格在格式級測試通過（load → lift → decompile / patch）前不標為支援
- 不破壞現有 ELF / PE x86 / Mach-O arm64+x64 行為
- 優先使用映像級指令模式（如 Thumb vs ARM），避免分散啟發式

---

## 2. EVM 位元組碼反編譯

將 NeverD 擴展至 **以太坊虛擬機（EVM）** 合約位元組碼——把 EVM 操作碼提升至同一 IR 堆疊，輸出 C、面向 Solidity 的原始碼與 LLVM IR，服務稽核與分析。

### 目標

- **EVM loader** — 接受執行時期位元組碼及常見製品形態
- **操作碼 lifter** — 手寫 1:1 語意；未知/新操作碼在 strict 下顯式失敗
- **堆疊與記憶體模型** — 將 EVM 堆疊機狀態回收為 MedIR 變數 / 記憶體操作
- **控制流還原** — JUMP / JUMPI → CFG；盡量結構化為 HighIR
- **儲存與 calldata** — 建模 `SLOAD`/`SSTORE`、calldata、returndata 及常見 ABI 呼叫形態
- **反編譯輸出** — 具明確 host-effect contract 的可編譯 C23 與 Solidity state machine，以及已驗證 LLVM IR
- **CLI / C API** — 對 EVM 輸入與原生二進位一致

**狀態：** Frontier 至 Fusaka 的傳統 opcode decode 與 lifting 已完成並有迴歸測試覆蓋。
原始碼重建仍持續保守演進：selector、event、型別、標準、名稱與動態 control flow 只在
證據充分時回報，不聲稱原始碼身分、完整 ABI 或完整 ERC 相容性。canonical function
selector、逐標準 ABI variant 與成功 return shape 彼此分離，因此共享 ERC selector
既不能憑空證明某個標準，也不會借用不相容的 return type。Amsterdam 僅是明確
opt-in 的 Review/development target；`latest` 仍為 Fusaka。EOFv1/EIP-7692 尚未排程，
EIP-3540 為 Stagnant，兩者都不冒充已定案 mainnet 行為。host ABI 與限制見
[EVM 反編譯](../evm.zh-TW.md)。

### 為什麼做 EVM？

- 稽核需要忠實還原鏈上邏輯；近似反編譯會掩蓋語意
- 復用 Low → Med → High → LLVM，原生與合約共用一套引擎
- 與原生側一致：不靜默「不支援就跳過」

---

## 3. Solana eBPF（SBF）反編譯

支援 **Solana eBPF / SBF** 鏈上程式——將 SBF 機器碼提升進 NeverD IR，並以同樣的 strict 語意反編譯。

### 目標

- **SBF / sbpf loader** — 載入 Solana program ELF
- **eBPF/SBF lifter** — 針對 Solana BPF ISA 子集手寫 1:1 語意；缺口 strict 報錯
- **Account 與 CPI 感知** — 在表現為呼叫/intrinsic 時還原常見 Solana 執行時期模式
- **CFG 與結構化輸出** — 與原生相同管線
- **CLI / C API** — 統一的 session 入口

**狀態：** 目前 Anza `sbpf` v0-v4 合約支援已完成。實作支援舊式 section/relocation ELF 與嚴格的僅 program-header ELF、完整的版本化指令資料庫、嚴格驗證、分階段 Low/Med/High IR、syscall/CPI/account 觀察、已驗證的 LLVM、可攜式 C11、安全的穩定版 Rust、CLI/C API 整合，以及獨立且有界的原始位元組碼語意 oracle。v4 會跟隨上游維護；能否在特定叢集部署或執行仍取決於該叢集的 feature activation。詳見 [Solana SBF 反編譯](../sbf.zh-TW.md)。

### 為什麼做 Solana eBPF？

- 鏈上 SBF 與 EVM 同為重要稽核目標
- BPF 形態 ISA 適合 NeverD 現有 CFG + SSA MedIR
- 一套 C SDK 覆蓋原生 + 合約位元組碼

---

## 4. 記憶體安全稽核與獵取

對已提升的二進位做堆積生命週期缺陷（洩漏、重複釋放、釋放後使用）與危險拷貝越界分析，並以結構化 JSON 報告；對已證明的越界給出具體見證。分析跑在格式無關的 IR 與共享身分檢視上，因此 **PE、ELF、Mach-O 是同等目標**，並複用自研符號執行與位向量求解器——不依賴外部求解器或容器。

| 項目 | 說明 |
|------|------|
| `audit` 軌道 | IR 上的堆積狀態機 + 逃逸摘要：洩漏、重複釋放、釋放後使用 |
| `hunt` 軌道 | 匯目錄 + 參數預過濾 + 目標容量 + 求解器見證 |
| 身分契約 | 依格式解析匯（PE IAT、ELF PLT、Mach-O dyld bind）以及 PDB / DWARF / MAP 名稱來源 |

**狀態：** PE、ELF、Mach-O 的 P0 已完成。判定與身分覆蓋由 [`unittests/safety`](../../unittests/safety) 以及在每個主機強制執行 PE/ELF/Mach-O × x86-64/AArch64 六單元 fixture 矩陣的端到端 [`SafetyIntegrationTests.cpp`](../../unittests/safety/SafetyIntegrationTests.cpp) 鎖定。詳見 [記憶體安全稽核與獵取](../memory-safety.zh-TW.md)。P1 將擴展到堆疊／全域越界、未初始化讀取與格式字串。

---

## 5. 引擎與產品加固（持續）

| 領域 | 方向 |
|------|------|
| Lifter 覆蓋 | 在不放鬆 strict 的前提下縮小原生操作碼缺口 |
| 語意測試 | 新 ISA 落地時擴展 Unicorn / roundtrip 覆蓋 |
| 外掛 ABI | 適合時用外掛承載新格式 |
| 文件 / 矩陣 | 僅在測試落地後更新 README 支援表 |

---

## 時間線

原生格式補齊、Fusaka 以前的傳統 EVM decode/lifting、Solana SBF 反編譯與記憶體安全
P0 已有迴歸覆蓋；保守 EVM 原始碼重建仍在進行。不承諾具體發布日期。

| 功能 | 狀態 |
|------|------|
| 原生格式補齊（PE ARM*、Mach-O i386） | 已完成 |
| EVM 傳統 decode/lifting | 至 Fusaka 已完成；有迴歸測試覆蓋 |
| EVM 原始碼重建 | 持續進行 — 只回報有證據的保守結果 |
| Solana eBPF（SBF）反編譯 | 已完成 — v0-v4、C、Rust 與 LLVM；有迴歸測試覆蓋 |
| 記憶體安全稽核與獵取 | 已完成 — PE、ELF、Mach-O 的 P0；有迴歸測試覆蓋 |
| 引擎與產品加固 | 持續進行 |
