# Attribution and citation

NeverD is developed by **NeverD contributors**. Its source repository is
[NeverSight/NeverD](https://github.com/NeverSight/NeverD).

## License obligations when reusing code

NeverD's original material is licensed under
[GNU AGPL version 3 only](LICENSE). When you convey covered copies or
adaptations, preserve the applicable copyright, license, and warranty
notices, including the project notice in [NOTICE](NOTICE). Preserve any
individual author notices as well. Modified covered source must carry
prominent notices identifying the changes and their relevant date.

These obligations apply to covered material reused manually, copied or
adapted with an AI assistant or large language model (LLM), or transformed
through LLVM IR, compilation, decompilation, or another programming language.
Changing names, formatting, language, or tools does not by itself remove
the obligations. Credit NeverD as the source of reused NeverD material;
crediting only an AI model or LLVM does not identify that source.

Keep the full license and applicable notices with source distributions and
the corresponding source for distributed binaries. For binaries and network
services, also follow the applicable provisions of AGPL sections 6 and 13.
A citation, link, or acknowledgement alone does **not** replace the AGPL's
licensing, modification-notice, or source-availability requirements.

This guide explains the existing license; it adds no restrictions or
additional terms under section 7. The authoritative terms are in
[LICENSE](LICENSE), especially sections 0, 2, 4–6, and 13, also available
from the [Free Software Foundation](https://www.gnu.org/licenses/agpl-3.0.html).

## Make the source traceable

For each reused portion, we recommend recording the original file or symbol,
the exact commit or release, and a short description of your changes next
to the code or in your project's notices. Use a GitHub permalink with the
full commit hash so that the citation continues to identify the same source.
This extra provenance detail is a citation recommendation, not an additional
license condition.

For example, replace the bracketed fields with the actual source details:

```text
This project includes material from NeverD.
Copyright (C) 2026 NeverD contributors (https://github.com/NeverSight/NeverD)
License: GNU Affero General Public License, version 3 only (AGPL-3.0-only).
Original source: https://github.com/NeverSight/NeverD/blob/[full-commit]/[path]
Changes: [description], [YYYY-MM-DD].
The applicable license and notices are included with this distribution.
```

For AI-assisted workflows, keep this provenance with the selected source
context and carry it into any covered code you publish. Review the resulting
code and its notices before sharing it. For datasets containing covered
NeverD source, preserve the applicable notices and licensing information
when conveying that source.

## Research, references, and output

Please cite NeverD in papers, documentation, benchmarks, and projects that
use it or draw on its implementation. [CITATION.cff](CITATION.cff) provides
machine-readable software citation metadata, and this plain-text citation
can be used with the version or commit you actually used:

```text
NeverD contributors. NeverD: Binary analysis and decompilation engine.
https://github.com/NeverSight/NeverD. Version or commit: [revision used].
```

Merely studying an idea or algorithm does not automatically make an independent
implementation subject to NeverD's license. Running NeverD on someone else's
program likewise does not automatically license the output under the AGPL:
under section 2, output is covered only if its content constitutes a covered
work. The same distinction applies to AI outputs; training on or reading
NeverD does not automatically make every model output a covered work.
For such uses without covered material, citation is requested as scholarly
and engineering practice, rather than imposed as a new license condition.

## Third-party material and earlier copies

Components such as LLVM, Capstone, and Unicorn retain their own licenses.
Consult their source notices, [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md),
and any directory-specific license, including the
[test corpus license](https://github.com/NeverSight/testbins/blob/9d9362d2cdfe0b4b0347bd0e12b3b8fac67d3a5b/LICENSE).
Preserve the original third-party credits and comply with those licenses
when reusing that material. This guide does not relicense third-party
material or revoke permissions previously granted for earlier copies.

## 中文说明

NeverD 原创代码采用 [AGPLv3](LICENSE)。分发受该许可证约束的代码副本或修改版时，
必须按许可证保留版权、许可和免责声明，包括 [NOTICE](NOTICE) 中的项目署名和来源信息；
已有的个人作者声明也应保留。发布修改后的受保护源码，还须明确说明修改及相关日期。
通过 AI/LLM 复制或改写代码，或借助 LLVM IR、编译、反编译、语言转换处理代码，
本身不会免除这些义务；只署名 AI 工具或 LLVM 不能代替 NeverD 的来源声明。

建议引用时同时写明原始文件或符号、实际使用的提交或版本，以及修改说明。
论文、评测和技术文档可以使用 [CITATION.cff](CITATION.cff) 和上面的引用模板。
仅参考思想、算法，或处理不包含 NeverD 受保护内容的输出时，引用属于项目倡议，
并非新增的许可限制。署名也不能代替 AGPL 要求的其他义务，例如适用的源码提供义务。
第三方代码继续遵循其各自的许可证；本指南不改变既有授权。
