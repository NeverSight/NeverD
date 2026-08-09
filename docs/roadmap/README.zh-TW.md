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

將 NeverD 從原生 ISA 擴展到 **以太坊虛擬機（EVM）** 合約位元組碼——把 EVM 操作碼提升進同一 IR 堆疊，輸出結構化 C / LLVM IR，服務稽核與分析。

### 目標

- **EVM loader** — 接受執行時期位元組碼及常見製品形態
- **操作碼 lifter** — 手寫 1:1 語意；未知/新操作碼在 strict 下顯式失敗
- **堆疊與記憶體模型** — 將 EVM 堆疊機狀態回收為 MedIR 變數 / 記憶體操作
- **控制流還原** — JUMP / JUMPI → CFG；盡量結構化為 HighIR
- **儲存與 calldata** — 建模 `SLOAD`/`SSTORE`、calldata、returndata 及常見 ABI 呼叫形態
- **反編譯輸出** — 經現有 HighIR / LLVM-C 路徑輸出結構化 C
- **CLI / C API** — 對 EVM 輸入與原生二進位一致

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

### 為什麼做 Solana eBPF？

- 鏈上 SBF 與 EVM 同為重要稽核目標
- BPF 形態 ISA 適合 NeverD 現有 CFG + SSA MedIR
- 一套 C SDK 覆蓋原生 + 合約位元組碼

---

## 4. 引擎與產品加固（持續）

| 領域 | 方向 |
|------|------|
| Lifter 覆蓋 | 在不放鬆 strict 的前提下縮小原生操作碼缺口 |
| 語意測試 | 新 ISA 落地時擴展 Unicorn / roundtrip 覆蓋 |
| 外掛 ABI | 適合時用外掛承載新格式 |
| 文件 / 矩陣 | 僅在測試落地後更新 README 支援表 |

---

## 時間線

EVM 與 Solana 仍處於研究與設計階段；原生格式補齊已完成並有迴歸測試覆蓋。不承諾具體發布日期；進展將在此文件追蹤。

| 功能 | 狀態 |
|------|------|
| 原生格式補齊（PE ARM*、Mach-O i386） | 已完成 |
| EVM 位元組碼反編譯 | 研究 / 設計 |
| Solana eBPF（SBF）反編譯 | 研究 / 設計 |
| 引擎與產品加固 | 持續進行 |
