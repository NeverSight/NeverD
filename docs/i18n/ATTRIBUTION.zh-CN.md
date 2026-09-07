**语言**: [English](../../ATTRIBUTION.md) | [简体中文](ATTRIBUTION.zh-CN.md) | [繁體中文](ATTRIBUTION.zh-TW.md) | [日本語](ATTRIBUTION.ja.md) | [한국어](ATTRIBUTION.ko.md) | [Français](ATTRIBUTION.fr.md) | [Deutsch](ATTRIBUTION.de.md) | [Español](ATTRIBUTION.es.md) | [Italiano](ATTRIBUTION.it.md) | [Русский](ATTRIBUTION.ru.md) | [العربية](ATTRIBUTION.ar.md)

# 署名与引用

本页是[英文指南](../../ATTRIBUTION.md)的译文；具体条款以 [LICENSE](../../LICENSE) 为准。

NeverD 由 **NeverD 贡献者**开发。源码仓库为
[NeverSight/NeverD](https://github.com/NeverSight/NeverD)。

## 复用代码时的许可义务

NeverD 的原创内容采用
[GNU AGPL 第 3 版（仅限此版本）](../../LICENSE)。分发受许可证约束的副本或
改编版本时，须保留适用的版权、许可和免责声明，包括 [NOTICE](../../NOTICE)
中的项目声明。任何个人作者声明也须保留。修改后的受许可证约束的源码必须附有
醒目的声明，说明修改内容及其相关日期。

这些义务适用于手动复用、借助 AI 助手或大语言模型（LLM）复制或改编，
以及通过 LLVM IR、编译、反编译或其他编程语言转换的受许可证约束的内容。
更改名称、格式、语言或工具本身并不能免除这些义务。复用 NeverD 内容时，
应将 NeverD 标明为其来源；仅注明 AI 模型或 LLVM 无法指明该来源。

分发源码以及为所分发的二进制文件提供对应源码时，须附上完整的许可证和适用声明。
对于二进制文件和网络服务，还须遵守 AGPL 第 6 条和第 13 条的适用规定。
仅有引用、链接或致谢，**不能**代替 AGPL 对许可、修改声明或源码可获取性的要求。

本指南说明现有许可证，不增加第 7 条所述的限制或附加条款。
权威条款见 [LICENSE](../../LICENSE)，特别是第 0、2、4–6 和 13 条；也可从
[自由软件基金会](https://www.gnu.org/licenses/agpl-3.0.html)获取。

## 让来源可追溯

对于每一处复用内容，我们建议在代码旁或项目声明中记录原始文件或符号、
确切的提交或发行版本，以及简短的修改说明。请使用包含完整提交哈希的
GitHub 永久链接，以便引用始终指向同一份源码。
这些额外的来源信息属于引用建议，并非附加许可条件。

例如，将方括号内的字段替换为实际来源信息：

```text
This project includes material from NeverD.
Copyright (C) 2026 NeverD contributors (https://github.com/NeverSight/NeverD)
License: GNU Affero General Public License, version 3 only (AGPL-3.0-only).
Original source: https://github.com/NeverSight/NeverD/blob/[full-commit]/[path]
Changes: [description], [YYYY-MM-DD].
The applicable license and notices are included with this distribution.
```

在 AI 辅助工作流程中，请将这些来源信息与所选源码上下文一并保留，
并将其带入您发布的任何受许可证约束的代码中。分享前，请检查生成的代码及其声明。
对于包含受许可证约束的 NeverD 源码的数据集，分发这些源码时，
须保留适用的声明和许可信息。

## 研究、参考与输出

在使用 NeverD 或借鉴其实现的论文、文档、基准测试和项目中，请引用 NeverD。
[CITATION.cff](../../CITATION.cff) 提供机器可读的软件引用元数据；
也可使用以下纯文本引用，并注明您实际使用的版本或提交：

```text
NeverD contributors. NeverD: Binary analysis and decompilation engine.
https://github.com/NeverSight/NeverD. Version or commit: [revision used].
```

仅研究某个想法或算法，并不会自动使独立实现受 NeverD 许可证约束。
同样，在他人的程序上运行 NeverD，也不会自动使输出采用 AGPL：
根据第 2 条，只有当输出内容构成受许可证约束的作品时，输出才受该许可证约束。
这一划分同样适用于 AI 输出；使用 NeverD 进行训练或读取其内容，
并不会自动使模型的每一份输出都成为受许可证约束的作品。
对于这些不含受许可证约束内容的用途，我们请求引用，作为学术与工程实践，
而不是将其设为新的许可条件。

## 第三方内容与早期副本

LLVM、Capstone 和 Unicorn 等组件保留各自的许可证。
请查阅其源码声明、[THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md)
以及任何目录专用的许可证，包括
[测试语料库许可证](https://github.com/NeverSight/testbins/blob/9d9362d2cdfe0b4b0347bd0e12b3b8fac67d3a5b/LICENSE)。
复用这些内容时，须保留原有第三方署名，并遵守相应许可证。
本指南不对第三方内容重新授权，也不撤销此前授予早期副本的权限。
