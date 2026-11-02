**Languages**: [English](CONTRIBUTING.md) | [简体中文](docs/zh-CN/CONTRIBUTING.md) | [繁體中文](docs/zh-TW/CONTRIBUTING.md) | [日本語](docs/ja/CONTRIBUTING.md) | [한국어](docs/ko/CONTRIBUTING.md) | [Français](docs/fr/CONTRIBUTING.md) | [Deutsch](docs/de/CONTRIBUTING.md) | [Español](docs/es/CONTRIBUTING.md) | [Italiano](docs/it/CONTRIBUTING.md) | [Русский](docs/ru/CONTRIBUTING.md) | [العربية](docs/ar/CONTRIBUTING.md)

# Contributing to NeverD

NeverD is a semantics-first binary analysis project. A useful contribution is
focused, keeps unsupported behavior fail-loud, and includes the smallest test
that proves the changed contract.

Preserve copyright, license, and source notices when incorporating existing
code, including code copied or adapted with AI/LLM tools. Follow the
[attribution guide](ATTRIBUTION.md), identify the original source and revision
for reused material, and check its license before submitting. Keep third-party
notices under their original licenses.

Before editing, read the [architecture guide](docs/architecture.md). Use the
[testing guide](docs/testing.md) for suite selection and the
[roadmap](docs/roadmap.md) for planned product work.

<!-- i18n-section: prerequisites -->
## Prerequisites

- Git with recursive submodule support
- CMake 3.20 or newer
- Ninja
- A C++20 compiler
- CPython 3.10+ with embedding development files when Python plugins are enabled
- Clang and LLD (`ld.lld` and `lld-link`) for the complete cross-target fixture
  set

The recursive submodules provide NeverD's LLVM fork, Capstone fork, Unicorn,
and signature data. Do not replace them with arbitrary system revisions when
validating a change.

<!-- i18n-section: clone -->
## Clone and initialize

Development is integrated on `dev`, which is also the repository's default
branch. Clone it with every submodule:

```bash
git clone --branch dev --recurse-submodules \
  https://github.com/NeverSight/NeverD.git
cd NeverD
```

For an existing clone, synchronize submodules before the first build and after
any commit that changes their recorded revisions:

```bash
git submodule sync --recursive
git submodule update --init --recursive
```

<!-- i18n-section: build -->
## Choose a build profile

| Profile | Use it for | Important behavior |
|---------|------------|--------------------|
| Release | Normal development, full tests, decode/lift benchmarks | Optimized; representative throughput |
| RelWithDebInfo | Profiling or debugging optimized hot paths | Optimized with debug symbols |
| Debug | Assertions, source-level stepping, local correctness work | Unoptimized; decode benchmarks are deliberately much slower |

Use Release unless the task specifically needs Debug behavior:

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
python scripts/build.py build-release --parallel
```

By default, the build compiles `third_party/llvm-project` as an integrated
dependency. The first build commonly takes 30–60 minutes; later builds are
incremental. `CMakePresets.json` also defines `release`, `relwithdebinfo`, and
`debug` configure/build presets, but explicit build directories are used above
so the enabled test setting is visible.

For source-level debugging, use a separate directory rather than reconfiguring
the Release tree:

```bash
cmake -S . -B build-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON
python scripts/build.py build-debug --parallel
```

Never report decode or lift throughput from a Debug build. Use Release for
benchmarks, or RelWithDebInfo when profiling needs symbols.

<!-- i18n-section: allocator -->
### Process allocator

Linux, macOS and Windows MSVC/clang-cl builds enable `NEVERD_ENABLE_MIMALLOC`
by default. NeverD tools,
the analysis worker and tests created through the common CMake helper use the
NeverSight mimalloc fork from its default branch, following
NeverC's host allocator approach.
On Unix the allocator is embedded once in each executable. Windows uses one
mimalloc DLL and the matching architecture's `mimalloc-redirect*.dll` to redirect
dynamic CRT allocation, including libneverd and native plugins. Both DLLs are
staged beside executables and installed into `bin`; distribute them together.
Windows requires `/MD` or `/MDd`; static CRT builds must disable mimalloc.
The SDK library itself does not impose an allocator on embedding applications.
Use a native ARM64 build on Windows ARM64: an x64 allocator DLL cannot redirect
the ARM64EC CRT used for x64 emulation. MinGW builds retain the system allocator.
See the [upstream Windows requirements](https://github.com/NeverSight/mimalloc/blob/main3/bin/readme.md).

Use `-DNEVERD_ENABLE_MIMALLOC=OFF` for a system-allocator baseline or when using
an external allocator. The legacy `NEVERD_WORKER_MIMALLOC=OFF` option still
disables it for the worker alone. CMake automatically suppresses mimalloc for address,
thread, memory and leak sanitizer builds, including combined sanitizer flags
and configuration-specific compiler/linker flags. UBSan alone remains compatible.
Sanitizers added only through target-specific flags must also set the option OFF.
On Windows, `MIMALLOC_DISABLE_REDIRECT=1` also allows a runtime system-allocator
comparison without rebuilding; `MIMALLOC_VERBOSE=1` reports activation.
The supported build entry point, `python scripts/build.py BUILD_DIR`, refreshes
all five NeverSight forks from their remote default branches before every build.
They are Git submodules in `third_party`, like Capstone and Unicorn. The script
resolves the actual branch names, updates these five submodules, then exports
their committed sources to `BUILD_DIR/third_party/<library>/<commit>/src`.
Uncommitted or divergent local submodule work stops the build without being
overwritten. Network failures stop the build. The build directory is locked
across refresh, reconfiguration and compilation; sources stay at that snapshot
until the next invocation. Incremental builds therefore adopt new commits too.

`neverd-dependency-inputs.json` records configuration inputs, including commit and
export SHA-256, and travels with installed products. It does not certify a binary.
`neverd-build-receipt.json` records success or failure for the requested targets
and configuration only. CI retains both. Installation rejects failed builds,
changed inputs and unbuilt configurations; product CI builds every installed
target before testing the installation. Mainline can change between runs;
compare these commits before treating separate platform runs as one qualification.
Plain `cmake --build` checks freshness and refuses stale snapshots; use the Python
entry point to refresh them. Auto-regeneration during that entry point keeps the
same snapshot. Explicit `FETCHCONTENT_SOURCE_DIR_NEVERD_*` overrides are available
for isolated local fixtures, but are rejected by the mainline build entry point.
Multi-config generators require an explicit `--config`. Build compilation targets
with the entry point, then run tests and `cmake --install` separately; passing
the `install` target directly is rejected before compilation.

See [allocator validation](docs/testing.md#allocator-validation) for smoke tests
and performance comparison guidance.

<!-- i18n-section: acceleration -->
### Performance libraries

Normal builds enable Highway byte scanning, simdutf UTF-8 validation,
simdjson transport parsing and oneTBB pipeline scheduling. They retain the process
allocator. JSON keeps the existing nlohmann DOM and serialization. Valid integer-only
frames copy directly from the SIMD tape; decimal numbers and negative zero
retain the original numeric conversion. Inputs outside simdjson's domain
use the original admission path.
Invalid external text retains its byte-preserving escapes.
These libraries and mimalloc come from the NeverSight forks listed in
`third_party/forks.json`. Compatibility fixes live on each fork's default branch.
The build copies the current source's licenses and records its exact commit;
NeverD does not pin these five dependencies to an earlier release.

`NEVERD_ENABLE_HIGHWAY`, `NEVERD_ENABLE_SIMDUTF`, `NEVERD_ENABLE_SIMDJSON`
and `NEVERD_ENABLE_TBB` default to `ON`; each can be set to `OFF` for comparisons.
The oneTBB scheduler retains workers between batches, checks their 8 MiB stacks,
and never builds or loads `tbbmalloc` or its allocator proxy. Large-stack callers
reuse their available headroom; small callers receive a native coordinator. Hosts that fork
after first use or dynamically unload the SDK must use the native-thread
baseline (`NEVERD_ENABLE_TBB=OFF`); these lifecycles remain unsupported by the
retained scheduler. oneTBB is statically linked into the shared core, with its
symbols private to that core. CLI component calls and GUI worker requests use
that same scheduler; no TBB DLL/shared library is distributed. Targets linking
`neverd_shared` select the shared scheduler automatically. Clients that reach
the SDK through another target and also link component archives must use
`neverd_use_shared_task_scheduler` on their final target. Standalone component
tools retain their own runtime.
This constrained integration avoids multiple NeverD schedulers in one process;
upstream still describes [static oneTBB builds](https://uxlfoundation.github.io/oneTBB/main/intro/static_linking.html)
as unsupported and discourages them for general use.

See [acceleration validation](docs/testing.md#acceleration-libraries) for
native tests, opt-out comparisons and the scheduler compatibility limits.

<!-- i18n-section: prebuilt-llvm -->
### Prebuilt LLVM on macOS

Apple Silicon contributors can avoid compiling the LLVM fork locally:

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_LLVM_PREBUILT=ON
python scripts/build.py build-release --parallel
```

CMake downloads the repository's configured release package, verifies its
SHA-256 checksum, and reuses the extracted user cache on later builds. The
prebuilt channel supports macOS arm64 only. Intel Macs and universal builds
must use the default local LLVM build. Advanced overrides such as
`NEVERD_LLVM_PREBUILT_TAG`, mirror URL, cache directory, and an explicit
checksum are documented in `cmake/NeverDLLVMPrebuilt.cmake`.

LLVM fixes belong in the [NeverSight LLVM fork](https://github.com/NeverSight/llvm-project).
Commit and merge them there, then update NeverD's LLVM submodule revision.
Integrated builds compile that source directly; do not add LLVM patch files or
build-directory source overlays to NeverD. Prebuilt packages must be rebuilt
from the corrected fork before they can qualify the same behavior.

<!-- i18n-section: workflow -->
## Branch and pull-request workflow

Start work from an up-to-date `dev` and use a focused topic branch:

```bash
git switch dev
git pull --ff-only origin dev
git switch -c docs/contributor-guide
```

Open pull requests against `dev`, not an assumed release branch. Keep commits
reviewable: one coherent purpose, no generated build output, no unrelated
formatting, and no submodule revision changes unless they are part of the
proposal.

<!-- i18n-section: code-style -->
## Code style

C and C++ follow the [LLVM Coding Standards](https://llvm.org/docs/CodingStandards.html),
with [`.clang-format`](.clang-format) as the repository's formatting authority
and [`.editorconfig`](.editorconfig) for encoding, line endings and whitespace.
Use NeverD's C++ standard from `CMakeLists.txt`; the upstream style guide does
not select the project's language version. Format the files you changed:

```bash
clang-format -i path/to/changed.cpp path/to/changed.h
git diff --check
```

CI uses the formatter version pinned in
[`scripts/check_clang_format.py`](scripts/check_clang_format.py). Use that
version when reproducing its results. Formatting checks do not verify header
ownership or identify policy literals.

Do not run a repository-wide reformat for a focused fix. Follow the surrounding
file's naming and decomposition patterns, keep platform-specific behavior at
the relevant loader/lifter/backend boundary, and avoid exposing internal C++
types through the pure C SDK.

<!-- i18n-section: headers -->
### File and header layout

- New C++ source and header files start with the LLVM-style filename/purpose
  banner, `NeverD Decompiler`, and a Doxygen `\file` description, as in
  `lib/debug/PDBLoader.cpp`. Preserve existing copyright and license notices.
- Use `.h` for self-contained headers. Include their dependencies explicitly;
  an implementation includes its corresponding interface header first.
  Let `.clang-format` order the remaining includes: sibling private headers,
  `neverd/` public headers, quoted LLVM/Capstone headers, then angle-bracket
  headers. Do not rely on incidental include order or transitive includes.
- New headers use a path-derived `NEVERD_..._H` include guard, such as
  `NEVERD_SDK_LIMITS_H` for `neverd/sdk/Limits.h`. Include enough component
  directories to keep private/tool guards unique. Close the guard with its
  name in an `#endif` comment. Use this layout when explicitly standardizing
  an existing header; leave unrelated headers unchanged.
- Keep shared library interfaces in `include/neverd/<component>/` and private
  implementation headers beside their sources in `lib/` or `tools/`.
  A header's location does not make its contents a stable public C SDK API.
  Keep dependencies acyclic and keep core models/readers independent of
  session state and transport adapters. Place subsystem algorithms in their
  owning directory instead of duplicating semantics in CLI, SDK or worker
  code. See [architecture](docs/architecture.md).
- Use the owning namespace and descriptive names. New C++ types and fields use
  `UpperCamelCase`, functions use `lowerCamelCase`, and named policy constants
  use NeverD's existing `kUpperCamelCase` convention. When extending an existing
  interface, keep its surrounding naming. Preserve the spelling of public
  C/Python contracts and established STL-style interfaces. Do not add
  namespace-wide `using namespace` directives to headers.
- Intentional multiple-inclusion tables keep the established `.def`/`.inc`
  convention and document their macro contract; they are not standalone `.h`
  interfaces and do not acquire a guard that prevents repeated expansion.
  Change generated headers through their producer.

For example, a standalone policy header follows this layout:

```cpp
//===- Limits.h - SDK resource limits and defaults ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SDK request bounds, retained-state limits and default work budgets.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_LIMITS_H
#define NEVERD_SDK_LIMITS_H

#include <cstdint>

namespace neverd::sdk {

/// Traversal ceiling for a function without a recorded instruction extent.
inline constexpr uint64_t kMaxUnsizedInstructions = 65536;

} // namespace neverd::sdk

#endif // NEVERD_SDK_LIMITS_H
```

<!-- i18n-section: policy -->
### Limits, defaults and configuration ownership

Use `ResourceLimits.h` in tool directories added directly to the include search path. On a case-insensitive filesystem, a local `Limits.h` can shadow the system `<limits.h>`.

Give each policy one authoritative definition in its owning subsystem's
limits/configuration header, even if it currently has only one caller. This
covers input/output caps, recursion and graph bounds, work/iteration budgets,
heuristic thresholds, cache retention, paging/batching, retries and timeouts.
It applies equally to a named `constexpr`, a macro, a literal comparison,
`min`/`clamp` argument, constructor/field initializer or default parameter.

Choose the existing authority before adding a file:

| Policy owner | Definition location |
| --- | --- |
| Shared core/pipeline | [`include/neverd/Limits.h`](include/neverd/Limits.h) |
| SDK | [`include/neverd/sdk/Limits.h`](include/neverd/sdk/Limits.h) |
| Web | [`include/neverd/web/Limits.h`](include/neverd/web/Limits.h) or its existing focused limits/configuration definition |
| Other library component | Its existing limits/configuration header or definition table; otherwise a focused `Limits.h` in the owning layer |
| CLI, worker or GUI only | A tool-local `ResourceLimits.h` or `Config.h`; core code must not depend on tool headers |
| Caller-selectable settings | The owning `Config.h`, `*Options.h` or `*Configuration.h` model |

Keep policy headers lightweight and group values by purpose. Reuse an existing
`*Limits.def` or configuration table when it already owns the values; expose
its C++ view through a self-contained header. Avoid competing `Limits.h`,
`Constants.h` and `Config.h` definitions of the same policy. Shared core policy
does not belong in SDK/tool headers, and unrelated module policies do not all
belong in the root `Limits.h`.

Use typed `inline constexpr` values for C++ scalar policy constants, in the
owner's established namespace (`neverd::limits`, `neverd::sdk`, etc.). Name
the purpose and unit, such as `kMaxSourceBytes`, `kDefaultPageRows` or
`kRetryDelayMilliseconds`; document zero/sentinel meanings and unusual bounds.
Separate a default from a hard maximum when they have different meanings.
An options model may own a field's default directly; consumers must use that
model/default factory or the same named constant. Derive related defaults and
ceilings from their shared definition instead of repeating a literal.

Adapters, persistence validation, CLI/GUI/worker code and Python/MCP bindings
must reuse the relevant authority. Derive schema maxima and limit-bearing
diagnostics from the same value. Derive cross-language contract values from
an existing definition table or generated binding, or add a drift check when
generation is impractical. Preserve pure C header compatibility. An intentional
frontend preview/page limit may be stricter than an engine limit: give it a
separate name and owner. Equal numbers alone do not establish a shared policy.

Binary layouts, ABI/protocol/instruction constants, representable integer
ranges and accounting charges tied to a concrete proof algorithm stay with
their semantic models. State initializers, indexing arithmetic, allocation
hints and independent test inputs are not automatically policy. Do not replace
a test's independent expected boundary with the implementation constant it is
meant to verify.

During review, check both declarations and literal uses, including multiline
guards, loops, macros, defaults, timers and bindings. Verify the owner and
meaning of every policy introduced or touched; a `Max*` search alone is
insufficient. A consolidation preserves types, values, comparison directions,
sentinels and exhaustion/error behavior. Treat policy changes separately and
run the affected checks from the [testing guide](docs/testing.md).

<!-- i18n-section: artifacts -->
### Documentation and local artifacts

Markdown should be concise and source-verifiable. Use relative links for files
inside the repository, and update documentation in the same pull request when
CLI behavior, public APIs, support claims, build flags, or test commands change.

Synchronize every supported translation when changing this guide. Review each
translation before updating its `i18n-source` SHA-256; retain matching sections,
examples, API/options and link destinations. Run `python3 scripts/check_docs_i18n.py`
to check documentation links and translation parity.

Keep machine-local performance reports, raw traces and timing summaries in
ignored build or benchmark-results directories. Commit reusable measurement
tools and regression tests, not local benchmark reports or copied result tables.

<!-- i18n-section: tests -->
## Run tests

Run all registered tests through the aggregate target:

```bash
python scripts/build.py build-release --parallel
cmake --build build-release --target check-neverd
```

During development, use the smallest relevant target or CTest label:

```bash
# Main Unicorn differential suite
cmake --build build-release --target check-neverd-semantic

# Lifter/loader/format binary only
python scripts/build.py build-release --target NeverDLiftTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDLiftTests$' --output-on-failure --parallel 4
```

The [testing guide](docs/testing.md) documents all convenience targets,
label-only transform suites, single-test regexes, fixture compilation, and
Unicorn roundtrips. If a target is skipped because a cross-compiler or linker is
missing, report that limitation; do not describe the skipped path as passing.

<!-- i18n-section: python-sdk -->
### Python plugin SDK

For Python plugin SDK changes, run the pure-Python and drift suites before the
native adapter tests:

```bash
PYTHONPATH=pluginsdk/python python3 -m unittest discover \
  -s pluginsdk/python/tests -v
python3 -m mypy --config-file pluginsdk/python/pyproject.toml \
  pluginsdk/python/neverd_plugin
PYTHONPATH=pluginsdk/python python3 scripts/check_python_plugin_sdk.py
python scripts/build.py build-release --target \
  NeverDPluginRuntimeTests NeverDPythonRuntimeTests NeverDPythonPluginTests
ctest --test-dir build-release -R 'NeverD(Python|Plugin)' --output-on-failure
```

Public C declarations, Python ABI signatures, package/CMake versions, and
delivery workflow policy are one reviewed contract; update them in the same
change. See the [Python plugin guide](docs/python-plugins.md).

<!-- i18n-section: review -->
## Pull-request checklist

Before requesting review:

- Rebase or merge the latest `dev` according to the maintainer's preferred
  workflow, and resolve submodule changes deliberately.
- Build the affected targets in Release (or explain why another profile is
  required).
- Run the narrow regression tests and the broadest practical relevant suite;
  include exact commands and any skips in the PR description.
- Preserve strict lifting: an unsupported instruction must not silently become
  a guessed operation or `NOP`.
- Add semantic coverage for behavior changes, not only textual IR snapshots.
- Keep the diff free of unrelated cleanup, generated files, and local build
  artifacts.
- Check the [header and policy ownership rules](#code-style), including bare
  numeric guards/defaults and binding drift; formatter success alone is not
  sufficient.
- Update public and contributor documentation when behavior, support, flags,
  commands, or test ownership changes.

For security-sensitive reports that should not begin as a public pull request,
follow [SECURITY.md](SECURITY.md).
