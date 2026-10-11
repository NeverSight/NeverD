**语言**: [English](../README.md) | [简体中文](README.md) | [繁體中文](../zh-TW/README.md) | [日本語](../ja/README.md) | [한국어](../ko/README.md) | [Français](../fr/README.md) | [Deutsch](../de/README.md) | [Español](../es/README.md) | [Italiano](../it/README.md) | [Русский](../ru/README.md) | [العربية](../ar/README.md)

<!-- i18n-source: 6241c9252ec89317b9c488c7e2b6b8c7bd60329dfdd734df27e1a7a7139b1eff -->

[← NeverD 项目](project.md)

# NeverD 文档

项目概览、构建与 CLI 说明见仓库 README。面向贡献者的设计与测试资料统一收录于此。

**移动端支持（实验性 CLI）：** `neverd mobile` 支持从 [Android](android.md) APK、DEX、smali 恢复 Java，以及从 [iOS](ios.md) IPA、`.app`、Mach-O 恢复原生 C 和受支持的 Objective-C/Swift 源码。JSON 报告记录恢复结果与覆盖范围。从[移动端总览](mobile.md)开始查看，各平台指南提供命令与限制。

英文指南直接位于 `docs/`。译文按语言分布在 `ar/`、`de/`、`es/`、`fr/`、`it/`、`ja/`、`ko/`、`ru/`、`zh-CN/` 和 `zh-TW/` 目录中。各语言目录包含文档索引 `README.md`、项目概览 `project.md`、专题指南、`CONTRIBUTING.md`、`ATTRIBUTION.md` 和 `roadmap.md`。共享图片保存在 `assets/`。

| 文档 | 说明 |
|------|------|
| [项目说明（简体中文）](project.md) | 概览、快速开始、构建、SDK、CLI |
| [贡献指南](CONTRIBUTING.md) | 开发环境、构建配置、工作流、风格与 PR 要求 |
| [架构](architecture.md) | IR 路径、组件边界、严格提升、支持深度与修改位置 |
| [离线 Web 分析（英文）](../web-analysis.md) | C++ 制品、源码、绑定和映射检查，元数据策略、SDK 与当前验证范围 |
| [Bun 独立程序配置（英文）](../web-bun-profile.md) | 固定 ELF 提取、原始证据范围、源码解码与固定测试样本的来源 |
| [ASAR 提取配置（英文）](../web-asar-profile.md) | 包内与包外捕获关联、完整性状态、成员使用方及原生 Unicode 依赖 |
| [Electron 证据配置（英文）](../web-electron-profile.md) | 捕获的入口路径、清单与源码范围、窗口和桥接证据及 IPC 通道候选 |
| [HTML 源码配置（英文）](../web-html-profile.md) | 有界 C++ 脚本清单、捕获的本地引用与原始内联源码锚点 |
| [测试](testing.md) | 测试套件、生成 fixture、Unicorn 往返与增量命令 |
| [桌面工作台 (英文)](../gui.md) | 经典反汇编器布局、独立工作进程、项目数据库、本地化和 MCP 连接 |
| [库识别（英文）](../library-recognition.md) | 基于证据的 STL、ATL/MFC、COM 和 libc 身份、配置与可恢复的 C 源码折叠 |
| [桌面验收记录 (英文)](../gui-qualification.md) | 已测量的 GUI 证据、打包边界及尚未完成的平台验收 |
| [解释器源码恢复](interpreter-recovery.md) | 实验性 x64 解释器特化、HighC/LLVMC 输出、执行前提、证据与限制; 嵌套循环证明候选; 显式发现预算和版本化 C API; 精确的原生到 LLVM 证明 API |
| [Windows 异常重建](windows-exception-reconstruction.md) | SEH/C++ 展开支持矩阵、IR 契约、原生 patch 规则与 PE 验证 |
| [CPU 执行与来宾环境](emulation.md) | 后端选择、来宾环境、原生验证与当前限制 |
| [CPU 执行](cpu-execution.md) | 配置、能力查询、后端可用性与类型化结果 |
| [Bitvector 证明后端](solver.md) | 可选 Z3 证明、门控合成、独立检查与查询导出 |
| [来宾进程模拟](process-emulation.md) | Linux ELF 配置、进程启动、服务、限制与测试 |
| [macOS/iOS 进程环境](darwin-emulation.md) | Mach-O 启动、设备与模拟器平台、Darwin 服务与页规则 |
| [macOS HVF](macos-hvf.md) | 宿主同架构硬件执行、签名权限、打包与验证 |
| [Windows 驱动模拟](driver-emulation.md) | 有界 x64 WDM/KMDF 生命周期、请求、硬件场景、SEH、PnP 子集、后端选择及限制 |
| [内存安全审计与猎取](memory-safety.md) | 堆对象生命周期与拷贝越界分析：各格式身份契约、汇/源目录、判定、预算与 JSON 模式 |
| [原生插件](plugins.md) | 纯 C 描述符 ABI、回调与事件、构建/链接流程、发现顺序及兼容性规则 |
| [Python 插件](python-plugins.md) | 插件编写、会话与事件 API、隔离、测试及发布 |
| [移动端总览](mobile.md) | Android 与 iOS 支持范围、输入输出格式、快速开始及各平台指南入口 |
| [Android Java 代码恢复](android.md) | 支持 APK（含多 DEX）、DEX、smali → Java；CLI 参数、JSON 报告、验证与恢复限制 |
| [iOS 源码恢复](ios.md) | 支持 IPA/.app/Mach-O → 原生 C 和受支持的 Objective-C/Swift 源码；目标选择、JSON 覆盖报告、验证与恢复限制 |
| [EVM 反编译](evm.md) | EVM 输入、硬分叉、分级 IR、C/LLVM host ABI、Solidity 重建与限制 |
| [Solana SBF 反编译](sbf.md) | SBF v0-v4、LLVM IR、C/Rust 输出、验证与已知限制 |
| [路线图](roadmap.md) | 状态：原生格式、EVM 与 Solana SBF 均已实现 |
| 本地化文档 | 使用上方语言链接打开各语言的文档索引和项目概览 |
| [加壳可执行文件的脱壳](unpack.md) | 通过观察有界来宾进程恢复加壳的 PE32+ 镜像：入口规则、重建的镜像、识别与限制 |
