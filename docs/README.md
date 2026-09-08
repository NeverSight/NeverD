**Languages**: [English](README.md) | [简体中文](zh-CN/README.md) | [繁體中文](zh-TW/README.md) | [日本語](ja/README.md) | [한국어](ko/README.md) | [Français](fr/README.md) | [Deutsch](de/README.md) | [Español](es/README.md) | [Italiano](it/README.md) | [Русский](ru/README.md) | [العربية](ar/README.md)

[← NeverD project](../README.md)

# NeverD Documentation

Project overview and build/CLI notes live in the repository README. Contributor-facing design and test references are indexed here.

English guides live directly in `docs/`. Translations are grouped in language
directories: `ar/`, `de/`, `es/`, `fr/`, `it/`, `ja/`, `ko/`, `ru/`, `zh-CN/`,
and `zh-TW/`. Each language directory contains a `README.md` documentation index,
a `project.md` project overview, topic guides, `CONTRIBUTING.md`, `ATTRIBUTION.md`,
and `roadmap.md`. Shared images remain in `assets/`.

| Document | Description |
|----------|-------------|
| [README (English)](../README.md) | Overview, quick start, build, SDK, CLI |
| [Contributing](../CONTRIBUTING.md) | Development setup, build profiles, workflow, style, and PR expectations |
| [Architecture](architecture.md) | IR routes, component boundaries, strict lifting, support depth, and where to edit |
| [Testing](testing.md) | Test suites, generated fixtures, Unicorn roundtrips, and incremental commands |
| [Windows exception reconstruction](windows-exception-reconstruction.md) | SEH/C++ unwind support matrix, IR contract, native patch rules, and PE validation |
| [Memory-safety audit & hunt](memory-safety.md) | Heap-lifetime and copy-overflow analysis: identity contract per format, sink/source catalog, verdicts, budgets, and JSON schema |
| [Native plugins](plugins.md) | Pure-C descriptor ABI, callbacks and events, build/link workflow, discovery, and compatibility rules |
| [Python plugins](python-plugins.md) | Typed authoring SDK, embedded host, lifecycle, loading, safety, tests, and publishing |
| [Android Java recovery](android.md) | APK/DEX/smali setup, multidex and class context, CLI options, JSON reports, troubleshooting, and verification |
| [iOS source recovery](ios.md) | IPA/.app/Mach-O selection, Objective-C/Swift bodies and layouts, CLI/export, coverage, limitations, and execution checks |
| [EVM decompilation](evm.md) | EVM inputs, hardforks, staged IR, C/LLVM host ABI, Solidity reconstruction, and limitations |
| [Solana SBF decompilation](sbf.md) | SBF v0-v4 ELF rules, staged IR, syscalls, C/Rust/LLVM backends, and host contracts |
| [Roadmap](roadmap.md) | Status: native formats, EVM, and Solana SBF implemented |
| Localized documentation | Use the language links above to open each language's index and project overview |
