**Languages**: [English](testing.md) | [简体中文](testing.zh-CN.md) | [繁體中文](testing.zh-TW.md) | [日本語](testing.ja.md) | [한국어](testing.ko.md) | [Français](testing.fr.md) | [Deutsch](testing.de.md) | [Español](testing.es.md) | [Italiano](testing.it.md) | [Русский](testing.ru.md) | [العربية](testing.ar.md)

[← Documentation Index](README.md)

# Testing NeverD

NeverD's tests cover three different questions: whether a representation has
the expected shape, whether a full pipeline route works for a binary fixture,
and whether generated code preserves behavior. Choose the smallest suite that
answers the question behind a change, then run the broader aggregate before a
high-risk pull request.

## Configure a test build

Tests are disabled unless `BUILD_TESTING` is enabled. A Release build is the
normal choice for the full suite; Debug preserves assertions and stepping but
is intentionally unoptimized and is not representative for decode benchmarks.

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build-release --parallel 4
```

The full fixture set needs `clang` for cross-target compilation and LLVM's
linkers (`ld.lld` and `lld-link`) on `PATH`. CMake builds many relocatable
fixtures unconditionally and linked ELF/PE fixtures when the matching linker is
available. A test skipped because the host cannot compile or link its fixture
is unexecuted coverage, not a pass for that target.

The SBF source differential suite additionally needs `rustc`; it executes both
generated C and generated Rust. Treat a missing compiler skip as missing
backend evidence, not as semantic success.

See [CONTRIBUTING.md](../CONTRIBUTING.md) for clone, build-profile, and macOS
prebuilt-LLVM guidance.

## Test layout

`add_neverd_unittest` creates one GoogleTest executable and assigns every
discovered case a CTest label equal to that executable's target name.

| Source area | Target and CTest label | What it covers |
|-------------|-------------------------|----------------|
| `unittests/TestProcessTests.cpp` | `NeverDTestProcessTests` | Cross-platform child-process invocation, quoting, redirects, and exit codes |
| `unittests/libc` | `NeverDLibCTests` | Known libc names and classification |
| `unittests/lift` | `NeverDLiftTests` | Decoder/lifter LowIR shapes, IR stages, loaders, relocations, format fixtures, decompilation, and representative patch flows |
| Most files in `unittests/semantic` | `NeverDSemanticTests` | Instruction, ABI, control-flow, C-expression, and lift/recompile differential semantics |
| `unittests/evm` | `NeverDEVMOpcodeTests`, `NeverDEVMBytecodeTests`, `NeverDEVMLoaderTests`, `NeverDEVMAnalyzerTests`, `NeverDEVMSemanticTests`, `NeverDEVMEmitterTests`, `NeverDEVMIntegrationTests` | Hardfork metadata, input normalization, CFG/SSA/recovery, interpreter semantics, LLVM/C/Solidity differential execution, and public API routing |
| `unittests/sbf` | All `NeverDSBF*Tests` targets, including `ProgramImage`, `MalformedCorpus`, `ISAConformance`, `UpstreamConformance`, `LLVMDifferential`, and `SourceDifferential` | v0-v4 metadata/layouts, strict verification, official ELF conformance, exhaustive opcode availability, hostile inputs, CFG/recovery, and executed LLVM/C/Rust differential behavior |
| `unittests/plugin` | `NeverDPluginRuntimeTests`, `NeverDPythonRuntimeTests`, `NeverDPluginTests`, `NeverDPythonPluginTests` | Native/Python loading, metadata, duplicates, lifecycle, GIL handoff, stale sessions, tracebacks, mixed discovery, and public C routing |
| `PatchFullSubstRTTests.cpp` | `NeverDPatchFullTests` | Rewrite/obfuscation equivalence across four ISAs and three object formats |
| Focused transform files in `unittests/semantic` | `NeverDSwitchXformTests`, `NeverDIndCallXformTests`, `NeverDCFGLoopXformTests`, `NeverDTwoTableXformTests`, `NeverDAvxUpperXformTests` | Small, fast-to-relink probes split out of the large semantic binary |
| `unittests/corpus` (submodule) | `NeverDWindowsEHCorpusTests`, `NeverDRustEHCorpusTests`, `NeverDGoEHCorpusTests`, `NeverDCxxItaniumEHCorpusTests` | Exception and runtime metadata read out of 305 pinned real binaries, each declared in a manifest with the floors its recovery must clear |

The source of truth for registration is
[`unittests/CMakeLists.txt`](../unittests/CMakeLists.txt),
[`unittests/lift/CMakeLists.txt`](../unittests/lift/CMakeLists.txt), and
[`unittests/semantic/CMakeLists.txt`](../unittests/semantic/CMakeLists.txt),
[`unittests/evm/CMakeLists.txt`](../unittests/evm/CMakeLists.txt),
[`unittests/sbf/CMakeLists.txt`](../unittests/sbf/CMakeLists.txt), and
[`unittests/plugin/CMakeLists.txt`](../unittests/plugin/CMakeLists.txt).

### The pinned binary corpus

Every other suite builds what it tests. The corpus does not: it is a submodule
of binaries that real toolchains produced, on hosts and for targets this
repository cannot reach, and each one is pinned by digest beside a manifest
stating the floors its recovery has to clear. That is the only place a claim
about what NeverD reads out of, say, a `-O2` stripped `armv7` shared object is
answerable rather than argued.

The suites are built only when the configure step is told to look for them, so
the flag is what keeps them under test:

```bash
cmake -S . -B build-corpus -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_BINARY_CORPUS_TESTS=ON
cmake --build build-corpus --target check-neverd-corpus --parallel 4
```

`check-neverd-corpus` runs every line; `check-neverd-windows-eh-corpus`,
`check-neverd-rust-eh-corpus`, `check-neverd-go-eh-corpus`, and
`check-neverd-cxx-itanium-eh-corpus` run one each. All three CI hosts configure
with the flag and run all four lines: the bytes are identical everywhere, but
what reads them is not, and a corpus run on one host proves nothing about the
other two. `scripts/audit_ci_test_inventory.py` refuses an inventory that is
missing any of the four labels, because a build that quietly stopped reading
the corpus is a regression no test can catch — the test is what went missing.

The EVM opcode audit performs a shallow `git fetch` of the official
[go-ethereum repository](https://github.com/ethereum/go-ethereum) remote `HEAD`
on every run, then reports the exact audited commit. It reuses the ignored bare
cache at `build/evm-opcode-audit/go-ethereum.git`, but refreshes that cache
before reading the closed opcode inventory and byte assignments:

```bash
python3 scripts/audit_evm_opcode_metadata.py
```

CI runs the same live audit on every push and pull request, on manual dispatch,
and once per day so upstream drift is detected even when NeverD does not change.
For an offline or historical reproduction, explicitly select an existing
checkout instead:

```bash
python3 scripts/audit_evm_opcode_metadata.py \
  --geth-root /path/to/go-ethereum
```

The audit permits only exclusions named in
`EVMUpstreamOpcodePolicy.def`; an upstream opcode not represented or explicitly
reviewed fails the command. Its parser and drift diagnostics have independent
Python unit coverage in CI, runnable with:

```bash
python3 -m unittest -v scripts.tests.test_audit_evm_opcode_metadata
```

For EVM control-flow work, run the fixed-point and height-domain contract first:

```bash
cmake --build build --target NeverDEVMAnalyzerTests --parallel 4
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.StackHeightDomain*:EVMAnalyzer.WholeProgram*'
```

These cases cover cross-block internal returns, finite multi-target merges,
loop convergence and deterministic edge ordering, path-dependent stack
heights, bounded widening, correlation-induced Cartesian over-approximation,
unknown jumps, exact invalid targets, and strict versus relaxed stack faults.
Then run all seven EVM binaries plus the upstream metadata audit; CFG changes
can affect emitter and integration behavior even when the analyzer shape is
locally correct.

For MedIR/HighIR dataflow changes, also run the constant-phi, selector,
typed-operand, malformed-graph, and deep-chain contracts:

```bash
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.MediumIR*:EVMAnalyzer.HighIR*:EVMAnalyzer.*Selector*:EVMAnalyzer.*MedIR*:EVMAnalyzer.RecoversStorageAndEventFactsFromTypedOperands:EVMAnalyzer.RecoversComputedCalldataArgumentOffset:EVMAnalyzer.*Return*:EVMAnalyzer.*Receive*'
```

These cases prove equal and conflicting cyclic phis, non-adjacent and
cross-block selector expressions, both equality operand orders, exact ABI
width checks, typed storage/event/calldata operands, deterministic malformed
MedIR handling, and an iterative 16,384-value producer walk.

Python plugin changes also have an exact C/Python/workflow drift audit:

```bash
PYTHONPATH=pluginsdk/python python3 -m unittest discover \
  -s pluginsdk/python/tests -v
PYTHONPATH=pluginsdk/python python3 -m unittest \
  scripts.tests.test_check_python_plugin_sdk -v
python3 -m mypy --config-file pluginsdk/python/pyproject.toml \
  pluginsdk/python/neverd_plugin
PYTHONPATH=pluginsdk/python python3 scripts/check_python_plugin_sdk.py
```

The first two runtime targets do not depend on the decompiler core and are the
fastest way to isolate loader, CPython, GIL, traceback, and capsule-lifetime
failures. `NeverDPluginTests` and `NeverDPythonPluginTests` then exercise the
same behavior through the exported `libneverd` C API.

## How fixtures are produced

### Lift and format fixtures

`unittests/lift/CMakeLists.txt` cross-compiles C and assembly sources during the
build. Clang target triples produce x86-64, i386, AArch64, and ARM32 ELF
objects, PE/COFF objects and linked images, and PIC/no-PIC Mach-O i386 objects.
When LLD is available, selected objects are also linked into executables for
patch tests. `NeverDLiftTests` depends on the `lift-test-objects` target, so a
normal build of that test binary refreshes its generated fixtures.

Most lift tests use `NeverDLiftFixture.h` to invoke the built `neverd` CLI and
inspect LowIR, MedIR, HighIR, LLVM IR, generated C, or a rewritten binary. The
`NEVERD` environment variable can override the CLI path for a focused manual
experiment; ordinary CTest runs use the executable embedded by CMake.

### Windows exception reconstruction

Windows table-based exception changes need both representation tests and a
linked-PE patch test. The focused lift-suite filter covers the normalized
unwind/SEH/C++ model, corrupt-input handling, exceptional CFG edges, HighIR,
LLVM WinEH generation, exception-directory replacement, and Guard CF/EH
continuation reconstruction:

```bash
cmake --build build --target NeverDLiftTests --parallel 4
build/bin/NeverDLiftTests \
  --gtest_filter='COFFException*:*PatchCOFF_X64.ReconstructsGuardedSEHAndContinuationTable:*PatchCOFF_X64.ReconstructsNativeFH3StateGraph:*PatchCOFF_X64.RejectsInteriorExceptionDirectoryPadding:*PatchCOFF_X64.RebuildsSortedExceptionDirectoryInAppendedSection'
```

The guarded x64 assembly fixture requires Clang's Windows target and
`lld-link`; its CMake link uses `/guard:cf` and `/guard:ehcont`. A skip caused
by a missing cross-linker is not evidence for the final-image path. A passing
integration case proves that the rewritten PE can be reloaded and that its
runtime-function, unwind, load-config, Guard CF, and Guard EH continuation
tables remain sorted, file-backed, and executable-target valid.

The linked FH3 fixture covers the native C++ closure independently: fixed
state tables, HighC annotations, personality preservation, generated catch
targets, and the reloaded IP-to-state graph.

See [Windows Exception Reconstruction](windows-exception-reconstruction.md)
for the analysis/native support matrix and the fail-closed patch contract.

### Unicorn differential roundtrips

The semantic fixture tests behavior rather than textual shape:

1. Write a small C/assembly case or construct LLVM IR.
2. Compile it for the requested target with Clang/LLVM.
3. Execute the original machine code in Unicorn and capture the expected
   return value or other fixture-defined state.
4. Load and lift it through NeverD, emit LLVM IR, and compile the result back to
   machine code.
5. Execute the regenerated code with the same ABI, inputs, memory layout, and
   CPU model.
6. Compare the observable results.

The main implementation is
[`SemanticRoundTripFixture.h`](../unittests/semantic/SemanticRoundTripFixture.h).
The patch-full fixture uses `Codegen::compileForRewrite`, the same rewrite
backend as patch operations, then compares baseline and transformed code across
the full 4 x 3 ISA/format grid.

A deterministic NeverD semantic failure should be a failed test. Reserve skips
for an explicit external capability boundary, and read the skip reason: a green
summary with a missing cross-linker does not prove that format path ran.

### EVM differential backends

EVM interpreter tests provide a deterministic 256-bit oracle. The emitter
suite compiles and runs generated LLVM directly, lowers generated C23 through
Clang and executes it against the same host harness, and—when `solc`, `anvil`,
`cast`, and `jq` are installed—deploys a generated Solidity harness to a local
Anvil node. It compares status, storage, and instruction trace counts rather
than relying only on output text. A separate raw-bytecode corpus executes the
pre-Fusaka scalar ALU, calldata/memory copying, overlapping `MCOPY`, Keccak,
and return-data paths directly in Anvil and compares them with the interpreter,
guarding against a lowering mistake shared by all generated backends.

`NeverDEVMOpcodeTests` also enforces the metadata architecture: all 150 assigned
opcodes round-trip between byte encodings and typed values, family helpers are
checked at their boundaries, hardfork aliases resolve through the shared
database, and the complete stack-contract and host-argument maxima remain
derived rather than duplicated in backends.

### Solana SBF differential backends

SBF metadata tests validate every version feature, opcode collision boundary,
Murmur3 syscall hash, relocation, syscall source/availability, ELF machine,
register, and VM-address constant. Loader fixtures generate both legacy v0-v2
section layouts and sectionless strict v3/v4 program-header layouts without
vendored binaries. The hostile corpus probes overflowed ELF tables and
segments, overlapping runtime regions, malformed optional metadata, invalid
registers/branches/LDDW continuations, and immediate-domain violations.

`NeverDSBFISAConformanceTests` checks all 256 encodings for each v0-v4 version
against an upstream-derived manifest. `NeverDSBFUpstreamConformanceTests`
assigns explicit outcomes to all 20 ELFs at the pinned Anza revision.
`NeverDSBFSemanticTests` executes verified instruction bytes directly and does
not consume MedIR, so changing or corrupting normalized IR cannot make the
source oracle agree accidentally with a backend. It covers non-monotonic v2
semantics, memory, syscalls, internal call frames, faults, traces, and resource
limits.

The ORC suite executes lifted LLVM against that raw oracle. The source suite
compiles and runs generated C with warnings as errors and Rust with
`-D warnings`; both compare return/fault state, writable-memory hashes, and
syscall traces. Public API tests traverse every IR stage, disassembly, CFG,
metadata, LLVM, C, and Rust from a generated strict SBF ELF.

### Solana SBF sanitizer profile

Use a separate build directory so sanitizer flags cannot contaminate the
normal integrated-LLVM build. The prebuilt NeverD LLVM package has RTTI
disabled, so standalone consumers must add `-fno-rtti` as well as the sanitizer
flags:

```bash
cmake -S . -B build-sbf-asan-ubsan -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DNEVERD_BUILD_PLUGINS=OFF \
  -DNEVERD_ENABLE_PYTHON_PLUGINS=OFF \
  -DLLVM_DIR=/path/to/neverd-llvm/lib/cmake/llvm \
  -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-rtti' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined' \
  -DCMAKE_SHARED_LINKER_FLAGS='-fsanitize=address,undefined'
```

Build and run the focused SBF targets listed below with
`ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:strict_string_checks=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. macOS ASan does not support
LeakSanitizer, hence the explicit `detect_leaks=0`; use a Linux sanitizer shard
for leak coverage. The prebuilt package also omits the NeverD LLVM fork's
`llvm/MC/BinaryRewrite.h`, so `NeverDSBFIntegrationTests` must be linked and run
in the normal integrated-LLVM build. This is a packaging boundary, not a
sanitizer skip for the 13 core SBF binaries. The audited profile passes 141/141
core cases with no ASan or UBSan report; the normal integrated profile passes
all 145/145 SBF cases across 14 binaries.

```bash
cmake --build build-sbf-asan-ubsan --parallel 4 --target \
  NeverDSBFMetadataTests NeverDSBFProgramImageTests NeverDSBFLoaderTests \
  NeverDSBFAnalyzerTests NeverDSBFISAConformanceTests \
  NeverDSBFSemanticTests NeverDSBFEmitterTests NeverDSBFLLVMEmitterTests \
  NeverDSBFLLVMDifferentialTests NeverDSBFSourceDifferentialTests \
  NeverDSBFMalformedCorpusTests NeverDSBFUpstreamConformanceTests \
  NeverDSBFSolanaModelTests

ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:strict_string_checks=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
NEVERD_SBPF_ROOT=$PWD/local_docs/sbpf \
ctest --test-dir build-sbf-asan-ubsan --output-on-failure --parallel 4 \
  -L '^NeverDSBF' -E 'SBFIntegration'
```

## One-shot targets

The custom targets build their dependencies and then run CTest with parallelism
derived from the host CPU:

| CMake target | Selection |
|--------------|-----------|
| `check-neverd` | Every registered test |
| `check-neverd-semantic` | `NeverDSemanticTests` only |
| `check-neverd-sbf` | Every `NeverDSBF*Tests` target/case |
| `check-neverd-patch-full` | `NeverDPatchFullTests` only |
| `check-neverd-switch-xform` | `NeverDSwitchXformTests` only |
| `check-neverd-cfgloop-xform` | `NeverDCFGLoopXformTests` only |
| `check-neverd-twotable-xform` | `NeverDTwoTableXformTests` only |

```bash
cmake --build build-release --target check-neverd
cmake --build build-release --target check-neverd-semantic
cmake --build build-release --target check-neverd-sbf
```

`NeverDIndCallXformTests` and `NeverDAvxUpperXformTests` currently have no
`check-neverd-*` convenience target. Build and select them by label as shown
below. `check-neverd-semantic` also does not include the separate transform or
patch-full binaries; use `check-neverd` for the complete aggregate.

## Incremental CTest workflow

Build the owning executable first, then select its label. This avoids relinking
unrelated large semantic targets.

```bash
# Lifter, loader, and format tests
cmake --build build-release --target NeverDLiftTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDLiftTests$' --output-on-failure --parallel 4

# Main semantic binary
cmake --build build-release --target NeverDSemanticTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDSemanticTests$' --output-on-failure --parallel 4

# A label-only focused transform binary
cmake --build build-release --target NeverDIndCallXformTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDIndCallXformTests$' --output-on-failure --parallel 4

# Every focused EVM target/case
cmake --build build-release --target \
  NeverDEVMOpcodeTests NeverDEVMBytecodeTests NeverDEVMLoaderTests \
  NeverDEVMAnalyzerTests NeverDEVMSemanticTests NeverDEVMEmitterTests \
  NeverDEVMIntegrationTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -R 'EVM' --output-on-failure --parallel 4

# Every focused Solana SBF target/case
cmake --build build-release --target \
  NeverDSBFMetadataTests NeverDSBFLoaderTests NeverDSBFProgramImageTests \
  NeverDSBFMalformedCorpusTests NeverDSBFISAConformanceTests \
  NeverDSBFAnalyzerTests NeverDSBFSemanticTests NeverDSBFLLVMEmitterTests \
  NeverDSBFLLVMDifferentialTests NeverDSBFEmitterTests \
  NeverDSBFSourceDifferentialTests NeverDSBFIntegrationTests \
  NeverDSBFUpstreamConformanceTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -R 'SBF' --output-on-failure --parallel 4
```

Use a GoogleTest-derived CTest name for a single regression:

```bash
ctest --test-dir build-release --build-config Release -N \
  -L '^NeverDLiftTests$'
ctest --test-dir build-release --build-config Release \
  -R '^COFFARMPipeline\.ARM32ThumbLiftAndDecompile$' \
  --output-on-failure
```

Useful selectors:

| Command | Purpose |
|---------|---------|
| `ctest --test-dir build-release -N` | List discovered cases without running them |
| `ctest --test-dir build-release -L '<regex>'` | Select a test-binary label |
| `ctest --test-dir build-release -R '<regex>'` | Select case names |
| `ctest --test-dir build-release --output-on-failure` | Show diagnostics only for failures |
| `ctest --test-dir build-release --stop-on-failure` | Stop after the first failing case |
| `ctest --test-dir build-release --parallel 4` | Run up to four cases concurrently |

GoogleTest discovery uses `DISCOVERY_MODE PRE_TEST`, so the corresponding test
binary must exist before CTest can enumerate it. Per-case timeouts and the
separate discovery timeouts are defined in `cmake/AddNeverD.cmake` and may be
widened only for suites with measured heavy cases.

## Which tests should change with code?

| Change area | Start with | Then consider |
|-------------|------------|---------------|
| Architecture lifter or decode | Named case in `NeverDLiftTests` | Matching ISA semantic roundtrip |
| LowIR CFG, function detection, jump tables | Lift CFG/switch cases | `NeverDSwitchXformTests`, `NeverDCFGLoopXformTests`, or `NeverDTwoTableXformTests` |
| MedIR, ABI, flags, types, SSA | MedIR/calling-convention lift cases | Cross-ISA `NeverDSemanticTests` cases |
| HighIR or structured C | HighIR/decompile cases | `NeverDCFGLoopXformTests` and generated-C compilation checks |
| PE/ELF/Mach-O loader or input relocation | Matching `unittests/lift` format fixture | All-stage load/decompile test for that cell |
| Rewrite codegen or output relocation | `RewriteCodegenRTTests` cases | `NeverDPatchFullTests` and a linked patch fixture where available |
| LLVM IR transform used by patch | Focused transform binary | `NeverDPatchFullTests` composed-pass grid |
| C API or CLI | Direct SDK/query test and `unittests/semantic/CLIEndToEndTests.cpp` | Relevant pipeline/format suite |
| EVM loader, opcode, IR, or backend | Smallest owning `NeverDEVM*Tests` target | All EVM targets plus generated C/Solidity compilation |
| SBF loader, ISA, IR, or backend | Smallest owning `NeverDSBF*Tests` target | All SBF targets plus generated C/Rust compilation |
| Libc recognition | `NeverDLibCTests` | Semantic call/ABI cases if behavior changes |
| Process execution or quoting | `NeverDTestProcessTests` | One affected CLI/semantic case on each supported host |

Tests should express the contract at the lowest stable boundary. A LowIR shape
test is useful for lifter attribution; a semantic roundtrip is required when
two plausible IR shapes could behave differently. Avoid golden dumps of whole
functions when a small opcode, CFG, or observable-state assertion is enough.

## CI relationship

CI builds Release with tests enabled on Linux, macOS, and Windows, then audits
the discovered inventory before applying platform-specific label exclusions.
Those profiles are defined in `.github/workflows/ci.yml` and
`scripts/audit_ci_test_inventory.py`. Because no single matrix shard represents
every expensive suite, a local `check-neverd` remains the clearest complete
pre-merge signal when the machine has all required cross tools.
