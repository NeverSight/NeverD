**Languages**: [English](README.md) | [简体中文](docs/i18n/README.zh-CN.md) | [繁體中文](docs/i18n/README.zh-TW.md) | [日本語](docs/i18n/README.ja.md) | [한국어](docs/i18n/README.ko.md) | [Français](docs/i18n/README.fr.md) | [Deutsch](docs/i18n/README.de.md) | [Español](docs/i18n/README.es.md) | [Italiano](docs/i18n/README.it.md) | [Русский](docs/i18n/README.ru.md) | [العربية](docs/i18n/README.ar.md)

<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/assets/neverd-logo-dark.svg">
  <img src="docs/assets/neverd-logo-light.svg" width="72" alt="NeverD">
</picture>

# NeverD

**The AI-friendly binary analysis & decompilation engine — 1:1 lift, built on LLVM**

PE · ELF · Mach-O · EVM · Solana SBF &nbsp;|&nbsp; x86-64 · i386 · AArch64 · ARM32 · EVM256 · SBF &nbsp;|&nbsp; C + Python SDKs

[![AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-blue.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/Standard-C%2B%2B20-brightgreen.svg)](#building)
![Platform](https://img.shields.io/badge/Platform-macOS%20%7C%20Linux%20%7C%20Windows-informational.svg)
[![SDK](https://img.shields.io/badge/SDK-C%20%2B%20Python-orange.svg)](#sdk-and-plugins)

[Documentation](docs/README.md) · [Roadmap](docs/roadmap/README.md) · [Contributing](CONTRIBUTING.md)

</div>

---

> GitHub always shows this English `README.md` on the repository homepage. Use the language links above for localized versions.

## Overview

NeverD is a native and smart-contract analysis/decompilation engine built around **1:1 instruction-level lifting**. It loads **PE**, **ELF**, **Mach-O**, legacy **EVM** bytecode, and Solana **SBF ELF** programs. Native targets decode with [Capstone](https://www.capstone-engine.org/); EVM and SBF use dedicated version-aware decoders and staged IR. Every path uses hand-written semantics rather than approximate translation. Supported instructions preserve their observable behavior in **LLVM IR**, **C**, **Rust for SBF**, **Solidity-oriented EVM reconstruction**, or—on native targets—a **rewritten binary**.

Strict mode is **on by default**. An instruction with no lifter throws `UnliftedInstruction` instead of skipping, guessing, or emitting a silent `NOP`.

CLI tools, integrators, and AI agents use one engine — **`libneverd`** — through a **pure C API**. They do not link Capstone, LLVM, or internal C++ directly.

Input formats, host contracts, and limitations are documented in the [EVM guide](docs/evm.md) and [Solana SBF guide](docs/sbf.md).

## Why NeverD?

- **1:1 semantics** — hand-written lifters; unsupported opcodes throw under default strict mode
- **LLM-friendly** — structured C, LLVM IR, and JSON analysis through a pure C API with deterministic errors
- **One pipeline, multiple exits** — `lift` → LLVM IR · `decompile` → C/Solidity/Rust · `patch` → rewritten native binary
- **Binary rewrite** — PE / ELF / Mach-O with section trampolines or in-place overwrite
- **Analysis toolkit** — CLI, debug info, signatures, plugins, and optional obfuscation passes

## Supported targets

| | **x86-64** | **i386** | **AArch64** | **ARM32** |
|---|:---:|:---:|:---:|:---:|
| **PE** (Windows) | ✓ | ✓ | ✓ | ✓ |
| **ELF** (Linux / Android) | ✓ | ✓ | ✓ | ✓ |
| **Mach-O** (macOS / iOS) | ✓ | ✓ | ✓ | ✓ |

> Every cell is implemented, but integration depth differs. See the [architecture coverage matrix](docs/architecture.md#support-and-test-depth). Mach-O i386 uses thin relocatable objects because modern macOS cannot link historical i386 executables.

Legacy EVM bytecode is supported independently of native containers: all 150 assigned opcodes through Fusaka feed dedicated Low/Med/High IR, verified LLVM `i256`, C23 `_BitInt(256)`, and Solidity output. See [EVM decompilation](docs/evm.md).

Solana SBF v0-v4 ELF programs use a dedicated strict loader, complete
versioned ISA metadata, Low/Med/High IR, verified LLVM, portable C11, and safe
stable Rust. See [Solana SBF decompilation](docs/sbf.md).

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

EVM (raw / hex / compiler artifact)
  → runtime normalization + hardfork-aware decode
  → EVM LowIR → EVM stack-SSA MedIR → recovered EVM HighIR
       ├─ lift        → verified LLVM i256/i512
       └─ decompile   → C23 _BitInt(256) or Solidity reconstruction

Solana SBF ELF (v0-v4)
  → version-aware legacy/strict loader + verifier
  → SBF LowIR → normalized MedIR → recovered SBF HighIR
       ├─ lift        → verified LLVM i64 runtime ABI
       └─ decompile   → portable C11 or safe stable Rust
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

# EVM
./build/bin/neverd lift contract.evm -o contract.ll
./build/bin/neverd decompile --language=c contract.evm -o contract.c
./build/bin/neverd decompile --language=solidity contract.evm -o contract.sol

# Solana SBF
./build/bin/neverd info program.so
./build/bin/neverd lift program.so -o program.ll
./build/bin/neverd decompile --language=c program.so -o program.c
./build/bin/neverd decompile --language=rust program.so -o program.rs

# Analysis
./build/bin/neverd funcs binary
./build/bin/neverd disasm --func 0x401000 binary
./build/bin/neverd sym-explore --func 0x401000 --expressions binary
./build/bin/neverd audit binary
./build/bin/neverd hunt binary
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

NeverD's normal push and pull-request CI deliberately builds the LLVM submodule
from source. When manually running the `CI` workflow, select
`use_prebuilt_llvm` to validate the published packages; only a manually
selected `true` enables prebuilt LLVM. Leaving it unchecked keeps the same
source-build path as automatic CI.

Published packages are selected from the host running CMake:

| Host | Release asset |
|------|---------------|
| macOS arm64 | `neverd-llvm-macos-arm64.tar.xz` |
| Linux x86_64 | `neverd-llvm-linux-x86_64.tar.xz` |
| Windows x64 | `neverd-llvm-windows-x64.zip` |

Each archive is checked against the digest pinned in
`cmake/NeverDLLVMPrebuilt.cmake` — or its published `.sha256` file, for a tag
those pins do not describe — before extraction under
`~/.cache/neverd-llvm/<tag>/<arch>/` (or the path set by
`NEVERD_LLVM_PREBUILT_CACHE_DIR`). The release build uses ccache on macOS and
Linux. Windows clang-cl builds use sccache with the GitHub Actions cache
backend; compiler caches only accelerate rebuilds and are never published as
release assets.

The release tag versions the NeverD package, while `BUILDINFO.txt` records the
exact LLVM fork commit. If LLVM still reports `23.0.0` but the fork source has
changed, the normal immutable choice is a package revision such as
`neverd-llvm-v23.0.0-r1` (then `-r2`) — not `23.0.1`, unless LLVM's own patch
version changed. Point `NEVERD_LLVM_PREBUILT_TAG` at that new revision.

To repair the existing mutable `neverd-llvm-v23.0.0` release in place, run the
`NeverD LLVM Release` workflow from the llvm-project `main` branch and enable
`overwrite_existing_assets`:

```bash
gh workflow run neverd-release.yml \
  --repo NeverSight/llvm-project \
  --ref main \
  -f release_tag=neverd-llvm-v23.0.0 \
  -f overwrite_existing_assets=true
```

This replaces same-named assets but deliberately does not force-move the
existing Git tag. Refresh the digests pinned in
`cmake/NeverDLLVMPrebuilt.cmake` as part of the same change: those digests,
rather than the tag, are what names the build a NeverD revision expects, so a
stale `~/.cache/neverd-llvm/neverd-llvm-v23.0.0/` is replaced on the next
configure and an archive matching no pinned digest stops that configure with a
checksum mismatch instead of surfacing later as a header the older package did
not carry. A new `-rN` tag avoids the in-place rewrite altogether. The workflow
rejects accidental replacement unless the checkbox is enabled and rejects
replacement entirely if GitHub marks the release immutable.

**Artifacts**

| Path | Description |
|------|-------------|
| `build/bin/neverd` | Unified CLI |
| `build/bin/neverd-bench` | Benchmark harness (JSON timings) |
| `build/bin/neverd-sigmaker` | `.pat` generator from static libraries |
| `build/bin/libneverd.*` | Engine shared library |
| `build/bin/sdk/` | Canonical C SDK include root; use `<neverd/sdk/NeverDCAPI.h>` or `<neverd/sdk/NeverDPlugin.h>` with the `neverd/sdk/` hierarchy preserved |
| `build/bin/sdk/python/` | Typed Python plugin package and examples |
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

See [Testing NeverD](docs/testing.md) for focused targets, CTest labels, fixture requirements, and the cross-format rewrite grid.

**CMake options**

| Option | Default | Description |
|--------|---------|-------------|
| `NEVERD_LLVM_PREBUILT` | `OFF` | CI prebuilt LLVM |
| `NEVERD_BUILD_SHARED` | `ON` | Build `libneverd` |
| `NEVERD_ENABLE_PYTHON_PLUGINS` | `ON` | Embed CPython 3.10+ plugin support |
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
| `decompile` | `.c` / `.sol` / `.rs` | C, EVM Solidity, or SBF Rust selected with `--language` |
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
| `sym-explore` | Bounded native LowIR path exploration (`--func`; JSON output) |
| `audit` | Heap-lifetime defects and uninitialized local stack reads (JSON) |
| `hunt` | Dangerous-copy overflows with symbolic witnesses and additive `process-input-v1` replay evidence when a complete plan is available (JSON schema v1) |
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

For EVM, use `neverd_decompile_all_ex(..., NEVERD_OUTPUT_SOLIDITY, ...)` to
select Solidity explicitly; legacy `neverd_decompile_all` continues to emit C.
See the [EVM C API examples](docs/evm.md#c-api).

Native shared libraries and Python `.py` files use the same plugin lifecycle.
Build the native example with `-DNEVERD_BUILD_PLUGINS=ON`; Python support is on
by default and can be removed completely with
`-DNEVERD_ENABLE_PYTHON_PLUGINS=OFF`. See the
[Python plugin guide](docs/python-plugins.md) for the typed SDK, lifecycle,
loading API, examples, safety model, and package workflow. Load paths:
`<neverd-dir>/plugins`, `~/.neverd/plugins`, `$NEVERD_PLUGIN_PATH`.

## Dependencies

| Component | Role | Source |
|-----------|------|--------|
| **LLVM** (fork) | IR, optimize, codegen, diagnostics | `third_party/llvm-project` or prebuilt |
| **Capstone** | Decode | `third_party/capstone` |

Third-party components keep their own licenses.

## Contributing

Development is integrated on the **`dev`** branch. See
[CONTRIBUTING.md](CONTRIBUTING.md) for setup, Release versus Debug guidance,
style, targeted tests, and pull-request expectations. The
[architecture](docs/architecture.md) and [testing](docs/testing.md) guides map
common changes to their owning code and verification suites.

## License

[AGPL-3.0](LICENSE)

LLVM components retain their Apache-2.0 WITH LLVM-exception license. Capstone retains its own license.
