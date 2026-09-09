**语言**: [English](../README.md) | [简体中文](README.md) | [繁體中文](../zh-TW/README.md) | [日本語](../ja/README.md) | [한국어](../ko/README.md) | [Français](../fr/README.md) | [Deutsch](../de/README.md) | [Español](../es/README.md) | [Italiano](../it/README.md) | [Русский](../ru/README.md) | [العربية](../ar/README.md)

[← NeverD 项目](project.md)

# NeverD 文档

项目概览、构建与 CLI 说明见仓库 README。面向贡献者的设计与测试资料统一收录于此。

**移动端支持（实验性 CLI）：** `neverd mobile` 已支持从 [Android](android.md) APK、DEX、smali 恢复 Java，以及从 [iOS](ios.md) IPA、`.app`、Mach-O 恢复原生 C 和受支持的 Objective-C/Swift 源码，并通过 JSON 报告记录恢复结果与覆盖情况。从[移动端总览](mobile.md)开始查看，各平台指南提供命令示例与恢复限制。

| 文档 | 说明 |
|------|------|
| [项目说明（简体中文）](project.md) | 概览、快速开始、构建、SDK、CLI |
| [贡献指南](CONTRIBUTING.md) | 开发环境、构建配置、工作流、风格与 PR 要求 |
| [架构](architecture.md) | IR 路径、组件边界、严格提升、支持深度与修改位置 |
| [测试](testing.md) | 测试套件、生成 fixture、Unicorn 往返与增量命令 |
| [Windows 异常重建](windows-exception-reconstruction.md) | SEH/C++ 展开支持矩阵、IR 契约、原生 patch 规则与 PE 验证 |
| [内存安全审计与猎取](memory-safety.md) | 堆对象生命周期与拷贝越界分析：各格式身份契约、汇/源目录、判定、预算与 JSON 模式 |
| [原生插件](plugins.md) | 纯 C 描述符 ABI、回调与事件、构建/链接流程、发现顺序及兼容性规则 |
| [Python 插件](python-plugins.md) | 插件编写、会话与事件 API、隔离、测试及发布 |
| [移动端总览](mobile.md) | Android 与 iOS 支持范围、输入输出格式、快速开始及各平台指南入口 |
| [Android Java 代码恢复](android.md) | 支持 APK（含多 DEX）、DEX、smali → Java；CLI 参数、JSON 报告、验证与恢复限制 |
| [iOS 源码恢复](ios.md) | 支持 IPA/.app/Mach-O → 原生 C 和受支持的 Objective-C/Swift 源码；目标选择、JSON 覆盖报告、验证与恢复限制 |
| [EVM 反编译](evm.md) | EVM 输入、硬分叉、分级 IR、C/LLVM host ABI、Solidity 重建与限制 |
| [Solana SBF 反编译](sbf.md) | SBF v0-v4、LLVM IR、C/Rust 输出、验证与已知限制 |
| [路线图](roadmap.md) | 状态：原生格式、EVM 与 Solana SBF 均已实现 |
| [English README](../../README.md) | 英文版主文档 |
| [其他语言 README](../README.md) | 其余本地化版本 |
