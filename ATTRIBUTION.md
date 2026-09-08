**Languages**: [English](ATTRIBUTION.md) | [简体中文](docs/zh-CN/ATTRIBUTION.md) | [繁體中文](docs/zh-TW/ATTRIBUTION.md) | [日本語](docs/ja/ATTRIBUTION.md) | [한국어](docs/ko/ATTRIBUTION.md) | [Français](docs/fr/ATTRIBUTION.md) | [Deutsch](docs/de/ATTRIBUTION.md) | [Español](docs/es/ATTRIBUTION.md) | [Italiano](docs/it/ATTRIBUTION.md) | [Русский](docs/ru/ATTRIBUTION.md) | [العربية](docs/ar/ATTRIBUTION.md)

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
