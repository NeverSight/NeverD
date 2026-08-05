**Languages**: [English](README.md) | [简体中文](docs/i18n/README.zh-CN.md) | [繁體中文](docs/i18n/README.zh-TW.md) | [日本語](docs/i18n/README.ja.md) | [한국어](docs/i18n/README.ko.md) | [Français](docs/i18n/README.fr.md) | [Deutsch](docs/i18n/README.de.md) | [Español](docs/i18n/README.es.md) | [Italiano](docs/i18n/README.it.md) | [Русский](docs/i18n/README.ru.md) | [العربية](docs/i18n/README.ar.md)

<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/assets/neverd-logo-dark.svg">
  <img src="docs/assets/neverd-logo-light.svg" width="72" alt="NeverD">
</picture>

# NeverD

**The AI-friendly binary analysis & decompilation engine — 1:1 lift, built on LLVM**

PE · ELF · Mach-O &nbsp;|&nbsp; x86-64 · i386 · AArch64 · ARM32 &nbsp;|&nbsp; Pure C SDK

[![AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-blue.svg)](LICENSE)
[![CI](https://github.com/NeverSight/NeverD/actions/workflows/ci.yml/badge.svg?branch=dev)](https://github.com/NeverSight/NeverD/actions/workflows/ci.yml)
[![C++20](https://img.shields.io/badge/Standard-C%2B%2B20-brightgreen.svg)](#building)
[![Formats](https://img.shields.io/badge/Formats-PE%20%7C%20ELF%20%7C%20Mach--O-informational.svg)](#supported-targets)
[![Arch](https://img.shields.io/badge/Arch-x86__64%20%7C%20i386%20%7C%20AArch64%20%7C%20ARM-orange.svg)](#supported-targets)
[![SDK](https://img.shields.io/badge/SDK-Pure%20C%20API-lightgrey.svg)](#sdk-and-plugins)

[Documentation](docs/README.md) · [Roadmap](docs/roadmap/README.md) · [Contributing](#contributing)

</div>

---

> GitHub always shows this English `README.md` on the repository homepage. Use the language links above for localized versions.

## Overview

NeverD is a native binary analysis and decompilation engine built around **1:1 instruction-level lifting**. It loads **PE**, **ELF**, and **Mach-O**, decodes with [Capstone](https://www.capstone-engine.org/), and lifts through a four-stage IR pipeline with **hand-written semantics** — not approximate translation. The goal is **100% semantic fidelity**: supported instructions keep their full observable behavior in **LLVM IR**, **structured C**, or a **rewritten binary**.

Strict mode is **on by default**. An instruction with no lifter throws `UnliftedInstruction` instead of skipping, guessing, or emitting a silent `NOP`.

CLI tools, integrators, and AI agents use one engine — **`libneverd`** — through a **pure C API**. They do not link Capstone, LLVM, or internal C++ directly.

Future releases will add [EVM](docs/roadmap/README.md#2-evm-bytecode-decompilation) and [Solana eBPF / SBF](docs/roadmap/README.md#3-solana-ebpf-sbf-decompilation) decompilation on the same IR stack — see the [roadmap](docs/roadmap/README.md).

## Why NeverD?

- **1:1 semantics** — hand-written lifters; unsupported opcodes throw under default strict mode
- **LLM-friendly** — structured C, LLVM IR, and JSON analysis through a pure C API with deterministic errors
- **One pipeline, three exits** — `lift` → LLVM IR · `decompile` → C · `patch` → rewritten binary
- **Binary rewrite** — PE / ELF / Mach-O with section trampolines or in-place overwrite
- **Analysis toolkit** — CLI, debug info, signatures, plugins, and optional obfuscation passes

## Supported targets

| | **x86-64** | **i386** | **AArch64** | **ARM32** |
|---|:---:|:---:|:---:|:---:|
| **PE** (Windows) | ✓ | ✓ | ✓ | ✓ |
| **ELF** (Linux / Android) | ✓ | ✓ | ✓ | ✓ |
| **Mach-O** (macOS / iOS) | ✓ | ✓ | ✓ | ✓ |

> Mach-O i386 integration coverage uses thin relocatable objects plus executable rewrite-backend tests; the current macOS host cannot link historical i386 executables.

## How it works

```text
Binary (PE / ELF / Mach-O)
  → Loader + DebugInfo
  → Capstone decode
  → LowIR     architecture-neutral NdOps · CFG
  → MedIR     types · ABI · calls · memory · SSA
       │
       ├─ lift        MedIR → LLVM IR
       ├─ decompile   MedIR → HighIR → C
       │              MedIR → LLVM IR → opt → C   (-llvm)
       └─ patch       MedIR → LLVM IR → codegen → binary
```

| Stage | Role |
|-------|------|
| **LowIR** | ~77 `NdOp` opcodes + CFG |
| **MedIR** | Types, calling conventions, memory model, SSA |
| **HighIR** | Structured control flow (`if` / `while` / `for`) |
| **LLVM** | Optimize, emit C, or codegen machine code |

## Quick start

```bash
git submodule update --init --recursive
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Pipeline
./build/bin/neverd lift -o out.ll binary
./build/bin/neverd decompile -o out.c binary
./build/bin/neverd patch -hello -o patched binary

# Analysis
./build/bin/neverd funcs binary
./build/bin/neverd disasm --func 0x401000 binary
./build/bin/neverd sigs --auto binary
```

Signature libraries are installed to `build/bin/signatures/` at build time. `sigs --auto` selects the matching set from format, architecture, and bitness.

## Building

**Requirements:** CMake ≥ 3.20 · Ninja · C++20 compiler · Git submodules (LLVM fork + Capstone)

```bash
git submodule update --init --recursive
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The first configure builds the LLVM fork locally (often 30–60 minutes). Later builds are incremental. Presets: `CMakePresets.json` → `release` / `relwithdebinfo` / `debug`.

<details>
<summary><strong>Prebuilt LLVM · artifacts · tests · CMake options</strong></summary>

<br>

**Prebuilt LLVM**

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DNEVERD_LLVM_PREBUILT=ON \
  -DNEVERD_LLVM_PREBUILT_TAG=neverd-llvm-v23.0.0
cmake --build build
```

**Artifacts**

| Path | Description |
|------|-------------|
| `build/bin/neverd` | Unified CLI |
| `build/bin/neverd-bench` | Benchmark harness (JSON timings) |
| `build/bin/neverd-sigmaker` | `.pat` generator from static libraries |
| `build/bin/libneverd.*` | Engine shared library |
| `build/bin/sdk/` | `NeverDCAPI.h`, `NeverDPlugin.h` |
| `build/bin/signatures/` | Bundled signature libraries |

**Tests**

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --target check-neverd
```

| Target | Description |
|--------|-------------|
| `check-neverd` | All tests |
| `check-neverd-semantic` | Semantic roundtrip only (Unicorn) |

**CMake options**

| Option | Default | Description |
|--------|---------|-------------|
| `NEVERD_LLVM_PREBUILT` | `OFF` | CI prebuilt LLVM |
| `NEVERD_BUILD_SHARED` | `ON` | Build `libneverd` |
| `NEVERD_BUILD_PLUGINS` | `OFF` | Example plugins |
| `BUILD_TESTING` | `OFF` | Unit tests |

</details>

## CLI

```text
neverd <command> [options] <binary>
```

### Pipeline

| Command | Output | Description |
|---------|--------|-------------|
| `lift` | `.ll` | Lift to LLVM IR |
| `decompile` | `.c` | Structured C (HighIR) |
| `decompile -llvm` | `.c` | Via LLVM IR + optimizer |
| `patch` | binary | Rewrite machine code |

```bash
neverd patch -hello -o patched binary
neverd patch --from-ir repl.ll -o patched binary
neverd patch --from-c repl.c --func 0x401000 -o patched binary
neverd patch --mode inplace -o patched binary
neverd patch --subst --flatten --mba -o patched binary
```

<details>
<summary><strong>Analysis commands</strong></summary>

<br>

| Command | Purpose |
|---------|---------|
| `info` / `dashboard` / `headers` | Metadata and overview |
| `funcs` | Discovered functions |
| `disasm` | Disassemble (`--func` name or hex) |
| `hex` | Hex dump at an address |
| `cfg` / `callgraph` | CFG / call graph (JSON; DOT/SVG optional) |
| `xrefs` | Cross-references |
| `strings` / `search` | Strings / byte or text search |
| `imports` / `exports` / `symbols` / `relocs` | Tables |
| `segments` / `sections` / `entrypoints` | Layout |
| `diff` | Compare two binaries (`-a` / `-b`) |
| `sigs` | Signature patterns (`--auto`) |
| `rename` / `annotate` / `bookmarks` | Session markup |
| `export` | Export results |
| `plugins` | List or run plugins |

Most analysis commands accept `--json`.

</details>

## SDK and plugins

Integrators use the **pure C API** from `libneverd`:

| Header | Role |
|--------|------|
| `NeverDCAPI.h` | Session, lift, decompile, patch, IR / CFG, annotations |
| `NeverDPlugin.h` | Dynamic-library plugin ABI |

```c
neverd_session_t s = neverd_session_create();
neverd_session_load(s, "binary.exe");
neverd_session_analyze(s);

const char *c = neverd_decompile(s, 0x401000);
neverd_free_string(c);
neverd_session_destroy(s);
```

Build the example plugin with `-DNEVERD_BUILD_PLUGINS=ON`. Load paths: `<neverd-dir>/plugins`, `~/.neverd/plugins`, `$NEVERD_PLUGIN_PATH`.

## Dependencies

| Component | Role | Source |
|-----------|------|--------|
| **LLVM** (fork) | IR, optimize, codegen, diagnostics | `third_party/llvm-project` or prebuilt |
| **Capstone** | Decode | `third_party/capstone` |

Third-party components keep their own licenses.

## Contributing

Style follows LLVM conventions (`.clang-format`).

Development happens on the **`dev`** branch (GitHub default).

```bash
git clone -b dev https://github.com/NeverSight/NeverD.git
cd NeverD
git submodule update --init --recursive
```

## License

[AGPL-3.0](LICENSE)

LLVM components retain their Apache-2.0 WITH LLVM-exception license. Capstone retains its own license.
