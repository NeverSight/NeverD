**Languages**: [English](CONTRIBUTING.md) | [简体中文](docs/i18n/CONTRIBUTING.zh-CN.md) | [繁體中文](docs/i18n/CONTRIBUTING.zh-TW.md) | [日本語](docs/i18n/CONTRIBUTING.ja.md) | [한국어](docs/i18n/CONTRIBUTING.ko.md) | [Français](docs/i18n/CONTRIBUTING.fr.md) | [Deutsch](docs/i18n/CONTRIBUTING.de.md) | [Español](docs/i18n/CONTRIBUTING.es.md) | [Italiano](docs/i18n/CONTRIBUTING.it.md) | [Русский](docs/i18n/CONTRIBUTING.ru.md) | [العربية](docs/i18n/CONTRIBUTING.ar.md)

# Contributing to NeverD

NeverD is a semantics-first binary analysis project. A useful contribution is
focused, keeps unsupported behavior fail-loud, and includes the smallest test
that proves the changed contract.

Before editing, read the [architecture guide](docs/architecture.md). Use the
[testing guide](docs/testing.md) for suite selection and the
[roadmap](docs/roadmap/README.md) for planned product work.

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
cmake --build build-release --parallel
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
cmake --build build-debug --parallel
```

Never report decode or lift throughput from a Debug build. Use Release for
benchmarks, or RelWithDebInfo when profiling needs symbols.

### Prebuilt LLVM on macOS

Apple Silicon contributors can avoid compiling the LLVM fork locally:

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_LLVM_PREBUILT=ON
cmake --build build-release --parallel
```

CMake downloads the repository's configured release package, verifies its
SHA-256 checksum, and reuses the extracted user cache on later builds. The
prebuilt channel supports macOS arm64 only. Intel Macs and universal builds
must use the default local LLVM build. Advanced overrides such as
`NEVERD_LLVM_PREBUILT_TAG`, mirror URL, cache directory, and an explicit
checksum are documented in `cmake/NeverDLLVMPrebuilt.cmake`.

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

## Code style

C and C++ follow LLVM coding conventions, with `.clang-format` as the
repository's formatting authority. Format the files you changed:

```bash
clang-format -i path/to/changed.cpp path/to/changed.h
git diff --check
```

Do not run a repository-wide reformat for a focused fix. Follow the surrounding
file's naming and decomposition patterns, keep platform-specific behavior at
the relevant loader/lifter/backend boundary, and avoid exposing internal C++
types through the pure C SDK.

Markdown should be concise and source-verifiable. Use relative links for files
inside the repository, and update documentation in the same pull request when
CLI behavior, public APIs, support claims, build flags, or test commands change.

## Run tests

Run all registered tests through the aggregate target:

```bash
cmake --build build-release --target check-neverd
```

During development, use the smallest relevant target or CTest label:

```bash
# Main Unicorn differential suite
cmake --build build-release --target check-neverd-semantic

# Lifter/loader/format binary only
cmake --build build-release --target NeverDLiftTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDLiftTests$' --output-on-failure --parallel 4
```

The [testing guide](docs/testing.md) documents all convenience targets,
label-only transform suites, single-test regexes, fixture compilation, and
Unicorn roundtrips. If a target is skipped because a cross-compiler or linker is
missing, report that limitation; do not describe the skipped path as passing.

For Python plugin SDK changes, run the pure-Python and drift suites before the
native adapter tests:

```bash
PYTHONPATH=pluginsdk/python python3 -m unittest discover \
  -s pluginsdk/python/tests -v
python3 -m mypy --config-file pluginsdk/python/pyproject.toml \
  pluginsdk/python/neverd_plugin
PYTHONPATH=pluginsdk/python python3 scripts/check_python_plugin_sdk.py
cmake --build build-release --target \
  NeverDPluginRuntimeTests NeverDPythonRuntimeTests NeverDPythonPluginTests
ctest --test-dir build-release -R 'NeverD(Python|Plugin)' --output-on-failure
```

Public C declarations, Python ABI signatures, package/CMake versions, and
delivery workflow policy are one reviewed contract; update them in the same
change. See the [Python plugin guide](docs/python-plugins.md).

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
- Update public and contributor documentation when behavior, support, flags,
  commands, or test ownership changes.

For security-sensitive reports that should not begin as a public pull request,
follow [SECURITY.md](SECURITY.md).
