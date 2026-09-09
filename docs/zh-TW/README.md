**語言**: [English](../README.md) | [简体中文](../zh-CN/README.md) | [繁體中文](README.md) | [日本語](../ja/README.md) | [한국어](../ko/README.md) | [Français](../fr/README.md) | [Deutsch](../de/README.md) | [Español](../es/README.md) | [Italiano](../it/README.md) | [Русский](../ru/README.md) | [العربية](../ar/README.md)

[← NeverD 專案](project.md)

# NeverD 文件

專案概覽、建置與 CLI 說明見儲存庫 README。面向貢獻者的設計與測試資料統一收錄於此。

實驗性 `neverd mobile` CLI 支援 Android 與 iOS 原始碼還原。Android 可從 APK（含 multidex）、DEX、smali 檔案／目錄產生 Java 與 JSON 報告；iOS 可從 IPA、`.app`、Mach-O（arm64 / x86_64）產生原生 C、受支援的 Objective-C / Swift 原始碼與 JSON 覆蓋率報告。還原範圍與限制詳見各平台指南。

| 文件 | 說明 |
|------|------|
| [專案說明（繁體中文）](project.md) | 概覽、快速開始、建置、SDK、CLI |
| [貢獻指南](CONTRIBUTING.md) | 開發環境、建置設定、工作流程、風格與 PR 要求 |
| [架構](architecture.md) | IR 路徑、元件邊界、嚴格提升、支援深度與修改位置 |
| [測試](testing.md) | 測試套件、產生的 fixture、Unicorn 往返與增量命令 |
| [Windows 例外重建](windows-exception-reconstruction.md) | SEH/C++ 展開支援矩陣、IR 契約、原生 patch 規則與 PE 驗證 |
| [記憶體安全稽核與獵取](memory-safety.md) | 堆積生命週期與拷貝越界分析：各格式身分契約、匯/源目錄、判定、預算與 JSON 模式 |
| [原生外掛](plugins.md) | 純 C 描述元 ABI、回呼與事件、建置/連結流程、探索順序及相容性規則 |
| [Python 外掛](python-plugins.md) | 外掛撰寫、工作階段與事件 API、隔離、測試及發佈 |
| [EVM 反編譯](evm.md) | EVM 輸入、硬分叉、分級 IR、C/LLVM host ABI、Solidity 重建與限制 |
| [Solana SBF 反編譯](sbf.md) | SBF v0-v4、LLVM IR、C/Rust 輸出、驗證與已知限制 |
| [行動平台支援總覽（English）](../mobile.md) | Android / iOS 輸入、原始碼輸出、CLI 流程與限制 |
| [Android Java 還原](android.md) | APK（multidex）、DEX、smali 檔案／目錄 → Java。CLI 流程、執行環境、選項、JSON 報告、錯誤處理與驗證限制 |
| [iOS 原始碼還原](ios.md) | IPA、`.app`、Mach-O → 原生 C 與受支援的 Objective-C / Swift 原始碼。輸入選擇、方法與配置、CLI/export、JSON 覆蓋率報告、限制及執行驗證 |
| [路線圖](roadmap.md) | 狀態：原生格式、EVM 與 Solana SBF 均已實作 |
| [English README](../../README.md) | 英文版主文件 |
| [其他語言 README](../README.md) | 其餘在地化版本 |
