**语言**: [English](../README.md) | [简体中文](README.md) | [繁體中文](../zh-TW/README.md) | [日本語](../ja/README.md) | [한국어](../ko/README.md) | [Français](../fr/README.md) | [Deutsch](../de/README.md) | [Español](../es/README.md) | [Italiano](../it/README.md) | [Русский](../ru/README.md) | [العربية](../ar/README.md)

[← NeverD 项目](project.md)

# NeverD 文档

项目概览、构建与 CLI 说明见仓库 README。面向贡献者的设计与测试资料统一收录于此。

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
| [Android Java 代码恢复](android.md) | APK/DEX/smali 环境配置、多 DEX 与类上下文、CLI 参数、JSON 报告、排查与验证 |
| [iOS 源码恢复](ios.md) | IPA/.app/Mach-O 选择、Objective-C/Swift 方法与布局、CLI/export、覆盖口径、限制及执行验证 |
| [EVM 反编译](evm.md) | EVM 输入、硬分叉、分级 IR、C/LLVM host ABI、Solidity 重建与限制 |
| [Solana SBF 反编译](sbf.md) | SBF v0-v4、LLVM IR、C/Rust 输出、验证与已知限制 |
| [路线图](roadmap.md) | 状态：原生格式、EVM 与 Solana SBF 均已实现 |
| [English README](../../README.md) | 英文版主文档 |
| [其他语言 README](../README.md) | 其余本地化版本 |
