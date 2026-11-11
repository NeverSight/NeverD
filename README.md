**Languages**: [English](README.md) | [简体中文](docs/zh-CN/project.md) | [繁體中文](docs/zh-TW/project.md) | [日本語](docs/ja/project.md) | [한국어](docs/ko/project.md) | [Français](docs/fr/project.md) | [Deutsch](docs/de/project.md) | [Español](docs/es/project.md) | [Italiano](docs/it/project.md) | [Русский](docs/ru/project.md) | [العربية](docs/ar/project.md)

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

[Documentation](docs/README.md) · [Android](docs/android.md) · [iOS](docs/ios.md) · [Roadmap](docs/roadmap.md) · [Contributing](CONTRIBUTING.md)

</div>

---

> GitHub always shows this English `README.md` on the repository homepage. Use the language links above for localized versions.

<!-- i18n-section: overview -->

## Overview

NeverD is a native and smart-contract analysis/decompilation engine built around **1:1 instruction-level lifting**. It loads **PE**, **ELF**, **Mach-O**, legacy **EVM** bytecode, and Solana **SBF ELF** programs. Native targets decode with [Capstone](https://www.capstone-engine.org/); EVM and SBF use dedicated version-aware decoders and staged IR. Every path uses hand-written semantics rather than approximate translation. Supported instructions preserve their observable behavior in **LLVM IR**, **C**, **Rust for SBF**, **Solidity-oriented EVM reconstruction**, or—on native targets—a **rewritten binary**.

Strict mode is **on by default**. An instruction with no lifter throws `UnliftedInstruction` instead of skipping, guessing, or emitting a silent `NOP`.

CLI tools, integrators, and AI agents use one engine — **`libneverd`** — through a **pure C API**. They do not link Capstone, LLVM, or internal C++ directly.

See the guides for [EVM](docs/evm.md), [Solana SBF](docs/sbf.md), [mobile source recovery](docs/mobile.md) and experimental [interpreter source recovery](docs/interpreter-recovery.md) for usage, supported contracts and limitations.

<!-- i18n-section: why-neverd -->

## Why NeverD?

- **1:1 semantics** — hand-written lifters; unsupported opcodes throw under default strict mode
- **LLM-friendly** — structured C, LLVM IR, and JSON analysis through a pure C API with deterministic errors
- **One pipeline, multiple exits** — `lift` → LLVM IR · `decompile` → C/Solidity/Rust · `patch` → rewritten native binary
- **Binary rewrite** — PE / ELF / Mach-O with section trampolines or in-place overwrite
- **Analysis toolkit** — CLI, debug info, signatures, plugins, and optional obfuscation passes

<!-- i18n-section: supported-targets -->

## Supported targets

| | **x86-64** | **i386** | **AArch64** | **ARM32** |
|---|:---:|:---:|:---:|:---:|
| **PE** (Windows) | ✓ | ✓ | ✓ | ✓ |
| **ELF** (Linux / Android) | ✓ | ✓ | ✓ | ✓ |
| **Mach-O** (macOS / iOS) | ✓ | ✓ | ✓ | ✓ |

> Every cell is implemented, but integration depth differs. See the [architecture coverage matrix](docs/architecture.md#support-and-test-depth). Mach-O i386 uses thin relocatable objects because modern macOS cannot link historical i386 executables.

**MIPS ELF analysis is experimental:** MIPS32r2/o32 and MIPS64r2/n64,
in both byte orders, support a strict integer subset through disassembly,
CFG recovery, C and LLVM output. Native rewriting is not supported. See the
[MIPS coverage and firmware example](docs/mips.md) for the separately qualified
microMIPS32, MIPS16e/e2, DSP and MIPS32r5 MSA subsets and their source restrictions.

Legacy EVM bytecode is supported independently of native containers: all 150 assigned opcodes through Fusaka feed dedicated Low/Med/High IR, verified LLVM `i256`, C23 `_BitInt(256)`, and Solidity output. See [EVM decompilation](docs/evm.md).

Solana SBF v0-v4 ELF programs use a dedicated strict loader, complete
versioned ISA metadata, Low/Med/High IR, verified LLVM, portable C11, and safe
stable Rust. See [Solana SBF decompilation](docs/sbf.md).

<!-- i18n-section: mobile-source-recovery -->

### Mobile source recovery

The experimental **`neverd mobile` CLI supports Android and iOS**:

| Platform | Supported inputs | Source output |
|----------|------------------|---------------|
| [Android](docs/android.md) | APK (including multidex), DEX, smali files or directories | Java + JSON reports |
| [iOS](docs/ios.md) | IPA, `.app`, Mach-O (arm64 / x86_64) | Native C and supported Objective-C / Swift sources + JSON coverage reports |

Recovery depends on supported code patterns; see the [mobile overview](docs/mobile.md) and platform guides for coverage and limitations.

<!-- i18n-section: cpu-workloads -->

### CPU execution and guest workloads

NeverD separates CPU backends from guest OS models for bounded processes, Android native libraries and x64 Windows drivers. Unicorn provides x64/ARM64 software execution; matching hosts can use KVM (Linux), WHP (Windows) or HVF (macOS). See [CPU execution and guest workloads](docs/emulation.md) for supported environments, validation and limits.

<!-- i18n-section: how-it-works -->

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

<!-- i18n-section: quick-start -->

## Quick start

```bash
git submodule update --init --recursive
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Pipeline
./build/bin/neverd lift -o out.ll binary
./build/bin/neverd decompile -o out.c binary
./build/bin/neverd patch -hello -o patched binary

# Pseudocode in the program's own language (C++, Rust, Go or C)
./build/bin/neverd decompile --language=source -o out.rs rust-binary
./build/bin/neverd decompile --language=cpp -o out.cpp cpp-binary
./build/bin/neverd decompile --language=go --func main.main go-binary

# Optional transformations for one native function (ordinary HighC skips them)
./build/bin/neverd decompile --func main --symbolic-simplify -o enhanced.c binary
./build/bin/neverd decompile --func main --llvm --llvm-opt=O3 -o optimized.c binary
./build/bin/neverd decompile --func main --llvm --llvm-opt=custom \
  --llvm-passes=instcombine,early-cse,simplifycfg,dce -o cleaned.c binary

# EVM
./build/bin/neverd lift contract.evm -o contract.ll
./build/bin/neverd decompile --language=c contract.evm -o contract.c
./build/bin/neverd decompile --language=solidity contract.evm -o contract.sol

# Solana SBF
./build/bin/neverd info program.so
./build/bin/neverd lift program.so -o program.ll
./build/bin/neverd decompile --language=c program.so -o program.c
./build/bin/neverd decompile --language=rust program.so -o program.rs

# Mobile source recovery (experimental CLI)
./build/bin/neverd mobile app.apk -o recovered-android
./build/bin/neverd mobile App.ipa -o recovered-ios

# Analysis
./build/bin/neverd funcs binary
./build/bin/neverd disasm --func 0x401000 binary
./build/bin/neverd sym-explore --func 0x401000 --expressions binary
./build/bin/neverd audit binary
./build/bin/neverd hunt binary
./build/bin/neverd sigs --auto binary
```

Signature libraries are installed to `build/bin/signatures/` at build time. `sigs --auto` selects the matching set from format, architecture, and bitness. For a PE file whose Rich header names its linker's Visual Studio release, it loads only that release's `vs<year>.pat` beside the files that belong to no release. `--sig-base <dir>` selects the same way from another signature tree. A pattern file of 1 MiB or more is parsed once: its modules are kept in `neverd/signatures` under the user's cache directory and mapped on later loads. `NEVERD_SIGNATURE_CACHE` names another directory, or `off` turns the cache off.

<!-- i18n-section: building -->

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
  -DNEVERD_LLVM_PREBUILT_TAG=neverd-llvm-v23.0.0-r6
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
`NEVERD_LLVM_PREBUILT_CACHE_DIR`). For the pinned default, the package's
`BUILDINFO.txt` must also name the exact LLVM submodule commit. The release
build uses ccache on macOS and Linux. Windows clang-cl builds use sccache with
the GitHub Actions cache backend; compiler caches only accelerate rebuilds and
are never published as release assets.

The default uses the package revision `neverd-llvm-v23.0.0-r6`; unlike the
legacy mutable base tag, its Git tag, release target, source commit, and three
archive digests form one revisioned source pin that NeverD treats as
immutable. Existing build directories still caching the legacy base tag,
`neverd-llvm-v23.0.0-r1`, `neverd-llvm-v23.0.0-r2`, `neverd-llvm-v23.0.0-r3`,
`neverd-llvm-v23.0.0-r4`, or `neverd-llvm-v23.0.0-r5` move to `r6`
automatically unless they also provide an explicit `NEVERD_LLVM_PREBUILT_SHA256` override. The `Prebuilt LLVM Audit` workflow runs
on pushes, pull requests, and every six hours. It invokes
`scripts/audit_prebuilt_llvm_release.py` to compare that source pin with the
live GitHub release and each published checksum sidecar.

If the LLVM fork changes while LLVM still reports `23.0.0`, publish the next
package revision—`neverd-llvm-v23.0.0-r7`, then `-r8`—rather than overwriting
an existing release or inventing LLVM version `23.0.1`:

```bash
gh workflow run neverd-release.yml \
  --repo NeverSight/llvm-project \
  --ref main \
  -f release_tag=neverd-llvm-v23.0.0-r7 \
  -f overwrite_existing_assets=false
```

After the workflow succeeds, update the default tag, pinned commit, and all
three digests in `cmake/NeverDLLVMPrebuilt.cmake` together. Each package revision
uses its own cache directory; the current default lives below
`.cache/neverd-llvm/neverd-llvm-v23.0.0-r6`. A stale or republished archive fails
before extraction. `overwrite_existing_assets`
exists only for legacy recovery; the normal revision workflow leaves it off.

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
| `NEVERD_ENABLE_SEMANTIC_TESTS` | `ON` | Unicorn-dependent semantic test group (when `BUILD_TESTING=ON`) |

</details>

<!-- i18n-section: desktop-workbench -->

## Desktop workbench

The optional [desktop workbench](docs/gui.md) follows the classic interactive
disassembler layout and shortcuts, with disassembly, graph, pseudocode, IR, hex
and list views, a Visual Studio Code-style theme, all 11 UI languages and
`.nddb` project databases. Analysis runs in a separate Qt-free worker; CLI-only
builds remain independent. See the [qualification record](docs/gui-qualification.md)
for supported workflows and platform validation still required before release.

<!-- i18n-section: cli -->

## CLI

```text
neverd <command> [options] <binary>
```

<!-- i18n-section: pipeline -->

### Pipeline

| Command | Output | Description |
|---------|--------|-------------|
| `lift` | `.ll` | Lift to LLVM IR |
| `decompile` | `.c` / `.sol` / `.rs` | C, EVM Solidity, or SBF Rust selected with `--language` |
| `decompile -llvm` | `.c` | Via LLVM IR + optimizer |
| `decompile --devirtualize` | `.c` + optional JSON | Experimental x64 interpreter recovery; requires `--func`; [contract and examples](docs/interpreter-recovery.md) |
| `mobile` | `.java` / `.c` / `.m` / `.swift` + JSON | Experimental: [Android](docs/android.md), [iOS](docs/ios.md) |
| `patch` | binary | Rewrite machine code |

```bash
neverd decompile program --func vm_entry --devirtualize --vm-control=r10 \
  --recovery-report recovery.json -o recovered.c
neverd patch -hello -o patched binary
neverd patch --from-ir repl.ll -o patched binary
neverd patch --from-source repl.c --entry replacement --func 0x401000 -o patched binary
neverd patch --mode inplace -o patched binary
neverd patch --subst --flatten --mba -o patched binary
```

Source patches can select Clang, NeverC ordinary mode, or GCC through the
[verified source/object profiles](docs/source-patching.md). Native objects
preserve producer machine code; compiler/runtime compatibility and exception
propagation are checked per profile.

For a 32-bit ARM binary that omits a function's ARM/Thumb mode metadata, assert
the entry mode before decompiling:

```bash
neverd decompile --arm-function-mode=0xADDRESS:thumb -o output.c binary
```

Repeat the option for other ambiguous entries, using `:arm` where appropriate.
Assertions that conflict with verified binary metadata fail loading. Valid
assertions affect only their exact entries. The C API exposes the same pre-load
setting as `neverd_session_set_arm_function_mode()`.

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
| `switches` | Switch jump tables from whole-program analysis |
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

<!-- i18n-section: sdk-and-plugins -->

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
Build the native example with `-DNEVERD_BUILD_PLUGINS=ON`; see the
[native plugin guide](docs/plugins.md) for the pure-C descriptor, callbacks,
build/link steps, discovery, CLI workflow, and ABI constraints. Python support
is on by default and can be removed completely with
`-DNEVERD_ENABLE_PYTHON_PLUGINS=OFF`; the
[Python plugin guide](docs/python-plugins.md) covers its typed SDK and package
workflow. Both kinds use `<neverd-dir>/plugins`, `~/.neverd/plugins`, and
`$NEVERD_PLUGIN_PATH`.

<!-- i18n-section: dependencies -->

## Dependencies

| Component | Role | Source |
|-----------|------|--------|
| **LLVM** (fork) | IR, optimize, codegen, diagnostics | `third_party/llvm-project` or prebuilt |
| **Capstone** | Decode | `third_party/capstone` |

Third-party components keep their own licenses.

<!-- i18n-section: contributing -->

## Contributing

Development is integrated on the **`dev`** branch. See
[CONTRIBUTING.md](CONTRIBUTING.md) for setup, Release versus Debug guidance,
style, targeted tests, and pull-request expectations. The
[architecture](docs/architecture.md) and [testing](docs/testing.md) guides map
common changes to their owning code and verification suites.

<!-- i18n-section: license -->

## License

[GNU AGPL version 3 only](LICENSE). When redistributing covered NeverD code or
adaptations, retain its copyright, license, and warranty notices, including
the project attribution and source in [NOTICE](NOTICE). This also applies to
AI/LLM-assisted reuse and LLVM-based code transformations.

See [Attribution and citation](ATTRIBUTION.md) for requirements, scope, and
examples. For traceable references, we recommend citing the source file and
exact version or commit. [CITATION.cff](CITATION.cff) provides software citation
metadata; citation alone does not replace license compliance.

LLVM components retain their Apache-2.0 WITH LLVM-exception license. Capstone retains its own license.
