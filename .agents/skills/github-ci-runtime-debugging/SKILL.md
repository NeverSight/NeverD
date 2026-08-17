---
name: github-ci-runtime-debugging
description: Diagnoses NeverD runtime failures in GitHub Actions that are platform-specific, intermittent, signal-terminated, or absent locally. Use when CI fails only on Linux x64, macOS arm64, or Windows x64; when CTest or GoogleTest flakes, crashes, times out, or reports a semantic mismatch; or when NeverD, LLVM, Unicorn, generated code, plugins, or child tools fail only on a runner. Trigger phrases include "CI fails but works locally", "random segfault", "core dumped", "flaky CTest", "only fails on x64/arm64/Windows", and "can't reproduce the CI crash".
---

# NeverD GitHub CI Runtime Debugging

## Goal

Reproduce the failure at the smallest faithful boundary, capture hard evidence, identify the
failing runtime layer, and validate the root-cause fix on the affected native runner.

Do not propose a fix from a single log fragment. Establish:

1. the failing workflow step and matrix leg;
2. the exact test/process that failed;
3. whether the failure is deterministic, resource-sensitive, or concurrency-sensitive;
4. whether the crash belongs to NeverD, its test host, an external tool, Unicorn/JIT code, or a
   generated program.

Treat the current repository files as authoritative. Before using commands from this skill, read:

- `.github/workflows/ci.yml`
- `scripts/audit_ci_test_inventory.py`
- `cmake/AddNeverD.cmake`
- `docs/testing.md`
- the failing test fixture

If the failure belongs to another workflow, also read that workflow rather than projecting
`ci.yml` onto it.

## NeverD-specific facts

NeverD is not interchangeable with NeverC:

- The main `ci.yml` currently uploads **no native build artifact**. An old failed run cannot supply
  the exact `neverd` or test binary through `gh run download`.
- Normal push and pull-request CI builds the integrated `third_party/llvm-project`
  (`NEVERD_LLVM_PREBUILT=OFF`). Manual dispatch may use the published, pinned prebuilt LLVM
  packages. Prebuilt mode is faster triage, but it is not proof that an integrated-LLVM failure is
  fixed.
- CTest discovers individual GoogleTest cases with `DISCOVERY_MODE PRE_TEST`; each case is labeled
  with its owning test executable.
- No single CI matrix leg runs both expensive suites. The three legs together provide the intended
  discovered CI profiles; local `check-neverd` is the complete aggregate available on that host.
  Neither claim means every optional differential backend executed: the workflow does not provision
  `solc`/`anvil`/`cast`/`jq`, `NEVERD_SBPF_ROOT`, or every optional source compiler, so read skips.
- NeverD has two independent concurrency controls: CTest process parallelism and internal
  `NEVERD_THREADS`. Always isolate both.
- The pinned binary corpus currently has six required lines: Windows, Rust, Go, C++ Itanium,
  Objective-C, and Ada/D exception handling.
- `python-plugin-sdk.yml` and `evm-upstream-audit.yml` are independent workflows with different
  failure boundaries and no native NeverD build.

Current matrix shape (verify against `ci.yml` before relying on it):

| Leg | Runner | Profile | Excluded labels | Test parallelism |
|---|---|---|---|---:|
| Linux x64 | `ubuntu-24.04` | `linux-semantic` | `^NeverDPatchFullTests$` | 4 |
| macOS arm64 | `macos-15` | `macos-patch` | `^NeverDSemanticTests$` | 3 |
| Windows x64 | `windows-latest` | `windows-focused` | `^NeverD(Semantic\|PatchFull)Tests$` | 4 |

CI builds Release into `build-ci`; executables and shared libraries are under `build-ci/bin`.

## Step 0: Characterize the failing run

Use `gh` to inspect the run without mutating it:

```bash
gh run list --workflow ci.yml --limit 30
gh run view <run-id> --json url,event,headSha,conclusion,jobs
gh run view <run-id> --log-failed
```

Record all of the following:

- exact commit SHA and event type;
- workflow step and matrix leg;
- whether `use_prebuilt_llvm` was enabled;
- exact CTest/GoogleTest case and owning binary;
- exit code, signal, exception code, assertion, timeout, or mismatch text;
- whether reruns fail in the same case and at the same phase;
- the last known green run on the same leg.

Check the run conclusion before interpreting a truncated log. The workflows use
`cancel-in-progress: true`; a newer push can cancel the old run and mimic a killed or incomplete
test process.

Do not rerun workflows, push debug commits, or create a temporary workflow for a read-only
diagnosis unless the user has authorized those external changes.

Useful Unix interpretations:

- 134: `SIGABRT`, often assertion/sanitizer/explicit abort
- 137: `SIGKILL`, commonly runner OOM or external cancellation
- 139: `SIGSEGV`
- missing final GoogleTest summary: killed or incomplete process, not a clean test run

On Windows, `0xC0000005` (often shown as `-1073741819`) is an access violation.

## Step 1: Classify the failing CI stage

| Failing step | First boundary to investigate |
|---|---|
| Verify Debug and Release target flags | Named Python unit/check, capabilities, SDK, or provenance |
| Check the localized documentation matrix | Linux-only `check_docs_i18n.py` |
| Verify pinned binary corpus | Recursive submodule checkout and six corpus manifests |
| Configure Release build | CMake, LLVM mode, package checksum, host toolchain |
| Build default targets | Compiler/linker error or compiler process crash |
| Native/Python plugin examples | `neverd` CLI, native loader, CPython plugin runtime |
| Audit and select test profile | CTest discovery/inventory; not a test-case runtime failure |
| Run selected test profile | Owning GoogleTest binary and its nested runtime layer |
| Check the simplifier's surfaces agree | CLI/C API/JSON API parity via `NEVERD_BUILD_DIR` |

For `python-plugin-sdk.yml`, the artifact `neverd-python-plugin-dist` contains only the verified
Python distribution. It is not a substitute for the native binaries absent from `ci.yml`.

For `evm-upstream-audit.yml`, distinguish the audit unit test from the live
`audit_evm_opcode_metadata.py` fetch against current go-ethereum HEAD. That workflow is not a
NeverD native runtime shard.

Inventory errors such as `CTest targets are NOT_BUILT`, a missing corpus label, semantic count below
20,000, patch count below 22,000, or `CTest executed X tests; expected Y` are CI
configuration/discovery failures. Do not debug them as random C++ crashes.

## Step 2: Identify the runtime layer from the log

NeverD tests often nest several runtimes. Separate them before debugging:

| Log signal | Likely layer |
|---|---|
| `_stdout.txt`, `_stderr.txt`, `neverd lift`, CLI `error:` | Spawned NeverD CLI |
| `clang compilation failed`, `ld.lld`, `lld-link`, `rustc`, `solc` | External compiler/linker/tool |
| `Original emulation failed` | Fixture/input or original-code Unicorn execution |
| `Lift-to-obj failed` | NeverD lift/codegen pipeline |
| `LLVM verification failed`, `LLVM shard` | NeverD LLVM emission/optimization |
| `Recompiled emulation failed` | NeverD-produced machine code under Unicorn |
| `Return value mismatch after roundtrip` | Semantic/codegen error, not necessarily a native crash |
| `runJIT`, `LLJIT`, ORC error | In-process LLVM ORC/JIT path |
| `Traceback`, `Manager.lastError()` | Python plugin runtime |
| `anvil`, `cast`, `jq`, generated C/Rust/Solidity | External generated-program harness |
| Bare segfault from a test binary | Test host, NeverD library, LLVM, Unicorn, JIT/native code |

A controlled Unicorn error is not a host `SIGSEGV`. Conversely, ORC/native translation tests can
execute generated code in the GoogleTest process, so a host crash may have no child-program banner.

An expected `GTEST_SKIP` is neither execution coverage nor a runtime failure. To inspect skip
reasons, rerun the affected label/case with verbose CTest output and search for skipped/unavailable
capabilities:

```bash
ctest --test-dir build-ci --build-config Release -V -R '<case-fragment>' 2>&1 |
  rg 'SKIPPED|Skipped|is unavailable|requires .* shell|set NEVERD_'
```

Use verbose CTest discovery to recover the owning executable and exact GoogleTest filter:

```bash
ctest --test-dir build-ci --build-config Release -N -V -R '<case-fragment>'
```

## Step 3: Reproduce the exact CI configuration

Start from the failing SHA and initialize every submodule:

```bash
git submodule sync --recursive
git submodule update --init --recursive
```

Use a fresh build directory. For a faithful Linux/macOS reproduction:

```bash
PYTHON=python3
PYTHON_EXECUTABLE="$("$PYTHON" -c 'import sys; print(sys.executable)')"
cmake -S . -B build-ci -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DBUILD_TESTING=ON \
  -DNEVERD_BUILD_PLUGINS=ON \
  -DNEVERD_ENABLE_PYTHON_PLUGINS=ON \
  -DNEVERD_ENABLE_BINARY_CORPUS_TESTS=ON \
  -DPython3_EXECUTABLE="$PYTHON_EXECUTABLE" \
  -DNEVERD_LLVM_PREBUILT=OFF
cmake --build build-ci --config Release --parallel <matrix-parallel>
```

On Windows, run from an MSVC x64 developer environment and use `cl` for both C and C++. The current
runner configures, builds, and tests through Bash after enabling MSVC; preserve that shell boundary
when reproducing quoting or child-process failures.

`-DNEVERD_LLVM_PREBUILT=ON` is a valid fast-triage variant on the currently published host set, but
label the result as non-faithful when the failed CI run used `OFF`. Never reuse one build directory
while switching LLVM mode or build type.

Reproduce only the owning target first:

```bash
cmake --build build-ci --target <NeverDTestBinary> --parallel <matrix-parallel>
ctest --test-dir build-ci --build-config Release \
  -R '^<exact-ctest-name>$' \
  --output-on-failure --parallel 1
```

For repetition in fresh CTest child processes:

```bash
ctest --test-dir build-ci --build-config Release \
  -R '^<exact-ctest-name>$' \
  --repeat until-fail:100 \
  --output-on-failure --parallel 1
```

For direct GoogleTest debugging, copy the exact command shown by `ctest -N -V`:

```bash
build-ci/bin/<NeverDTestBinary> \
  --gtest_filter='<Suite.Case>' \
  --gtest_repeat=100 \
  --gtest_break_on_failure
```

Use `.exe` on Windows when needed.

## Step 4: Separate the two concurrency dimensions

First isolate the exact case, then restore enough neighboring cases to make CTest launch concurrent
processes. `--parallel N` has no effect when the selection contains only one case.

| Run | Selection | CTest `--parallel` | `NEVERD_THREADS` | What it isolates |
|---|---|---:|---:|---|
| A | exact case | 1 | 1 | Logic/platform baseline |
| B | exact case | 1 | default | NeverD internal pipeline concurrency |
| C | owning label or representative case set | matrix value | 1 | Multi-process/tool/resource pressure |
| D | original CI profile | matrix value | default | Exact CI stress |

Example:

```bash
NEVERD_THREADS=1 ctest --test-dir build-ci --build-config Release \
  -R '^<exact-name>$' --repeat until-fail:100 --output-on-failure --parallel 1
```

Interpretation:

- A fails identically: prioritize deterministic logic, ABI, codegen, or platform behavior.
- A passes and B fails: investigate NeverD's internal parallel phases and shared state.
- B passes and C fails: investigate process/FD/memory pressure, temp names, child tools, and CTest
  concurrency.
- Only D fails: inspect multiplicative thread/process pressure and cross-process resources first.

`scripts/run_semantic_parallel.sh` is useful for large semantic stress and checks for killed shards,
but it is hard-coded to `build/`, not `build-ci/`. Do not use it as proof of CI fidelity without
first reconciling that build configuration. It also calls `make`, so a Ninja-configured `build/`
will fail there. `ND_NO_BUILD=1` can silently reuse a stale binary.

## Step 5: Test determinism and resource sensitivity

For a flaky failure:

1. Repeat the exact case enough times to estimate frequency.
2. Compare the failing case and phase across runs.
3. Record whether the process printed its final GoogleTest summary.
4. Compare A/B/C/D from the concurrency grid.
5. Check runner memory, process count, disk space, and timeout before changing code.

After excluding a workflow-level cancellation, different cases dying on different runs, exit 137,
missing final summaries, or repeated
`link failed after retries (transient infra)` strongly suggests resource/process pressure. The
semantic fixture already uses fixed test inputs and retry logic; do not call such failures
"random-input bugs" without evidence.

For suspected nondeterministic code generation, emit the same Release output repeatedly and hash the
behaviorally relevant bytes. Exclude debug sections, paths, timestamps, UUIDs/build IDs, and other
known metadata before concluding that code generation differs. Then compare the first differing
section or IR stage, not only the whole-file hash.

When a stable reproduction exists, change one variable at a time: optimization level, LLVM mode,
`NEVERD_THREADS`, CTest parallelism, host architecture, or tool version.

## Step 6: Match the failing platform

### macOS arm64

An Apple Silicon development host matches the current macOS CI architecture. Match the CI compiler,
Release mode, LLVM mode, and parallelism. Debug a direct test command with LLDB:

```bash
lldb -- build-ci/bin/<NeverDTestBinary> --gtest_filter='<Suite.Case>'
```

Inside LLDB, use `run` and `thread backtrace all`. A Debug-only success does not clear a
Release-only bug; use a separate RelWithDebInfo directory if symbols are needed.

### Linux x64 from Apple Silicon

Use an amd64 Ubuntu 24.04 container for first-pass environment matching:

Initialize the host checkout recursively before mounting it read-only; the container cannot repair
missing LLVM, Capstone, Unicorn, signatures, or corpus submodules.

```bash
docker build --platform linux/amd64 -t neverd-ci-repro - <<'EOF'
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y \
    build-essential clang cmake gdb lld ninja-build \
    python3 python3-dev git ca-certificates
EOF

docker run --rm --platform linux/amd64 --cpuset-cpus=0-3 \
  -v "$PWD":/src:ro \
  -v neverd-amd64-build:/build \
  -v neverd-amd64-llvm-cache:/root/.cache/neverd-llvm \
  neverd-ci-repro \
  bash -lc 'PYTHON_EXECUTABLE="$(python3 -c "import sys; print(sys.executable)")"; \
    cmake -S /src -B /build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DBUILD_TESTING=ON \
    -DNEVERD_BUILD_PLUGINS=ON -DNEVERD_ENABLE_PYTHON_PLUGINS=ON \
    -DNEVERD_ENABLE_BINARY_CORPUS_TESTS=ON \
    -DPython3_EXECUTABLE="$PYTHON_EXECUTABLE" \
    -DNEVERD_LLVM_PREBUILT=ON && cmake --build /build --parallel 4'
```

This intentionally uses prebuilt LLVM for fast triage. Switch to `OFF` only when integrated LLVM is
part of the hypothesis and the much slower emulated build is justified.

QEMU can hide real scheduling, CPU-feature, alignment, and timing bugs. A pass under emulation does
not clear a native Linux x64 failure.

### Windows x64

Use a native MSVC x64 environment for final reproduction. Preserve the workflow's `cl` + Ninja
combination. Wine or cross-linking may test file generation but cannot clear a native Windows
runtime or command-quoting failure. Investigate `NeverDTestProcessTests` when paths, quoting, or
child exit codes differ only on Windows.

Use Visual Studio or WinDbg for an access violation. Verify debugger/dump-tool availability on a
runner before writing a workflow around it.

## Step 7: Preserve evidence

Always retain:

- `ctest.log`;
- `build-ci/Testing/Temporary/LastTest.log` and `LastTestsFailed.log` when present;
- the verbose CTest command and exact GoogleTest filter;
- compiler, linker, CMake, OS, architecture, CPU-count, and LLVM-mode details;
- debugger backtrace or sanitizer report;
- output hashes and the command used to produce them.

Lift and CLI fixtures preserve failure directories named like `nd_test_*`/`nd_e2e_*` in the host
temporary directory. Inspect their `_stdout.txt` and `_stderr.txt`.

Semantic roundtrip work directories are deleted by `WorkGuard` on every exit path, including
assertions and skips. If those intermediates are essential, make an explicitly temporary diagnostic
change that preserves or uploads the per-test directory; do not assume the current fixture retained
it.

For Linux core dumps on a disposable debug runner:

```bash
ulimit -c unlimited
echo "$PWD/core.%e.%p" | sudo tee /proc/sys/kernel/core_pattern
```

Run the smallest reproducer, then symbolize with the matching executable:

```bash
gdb -batch \
  -ex 'thread apply all bt full' \
  -ex 'info registers' \
  build-ci/bin/<NeverDTestBinary> <core-file>
```

Core dumps may contain sensitive process data. Upload only what is needed and only to an
appropriately protected workflow run.

Keep sanitizer builds separate from `build-ci`. The documented SBF ASan/UBSan profile in
`docs/testing.md` uses prebuilt LLVM, requires `-fno-rtti`, and has a stated integration-test
packaging boundary; do not generalize it blindly to the integrated-LLVM Release shard.

## Step 8: Use a real runner only when needed

If local native reproduction is unavailable, or Docker/QEMU passes while the real runner fails, add
a minimal temporary `workflow_dispatch` workflow only with user authorization.

The debug workflow should:

1. hard-code one affected runner leg;
2. copy checkout, dependency, compiler, CMake, and LLVM-mode details from current `ci.yml`;
3. build only the owning target where possible;
4. repeat one exact test before adding parallel stress;
5. capture backtraces/logs even when the test step fails;
6. upload minimal evidence with `if: always()` using the repository's currently pinned
   `actions/upload-artifact` revision;
7. use minimal permissions and a bounded `timeout-minutes`.

Because the normal CI has no native artifact, a debug workflow must build the reproducer itself or
debug a newly instrumented run. Do not claim it is using the original failed run's exact binary.

Remove the throwaway workflow and diagnostic-only code after the investigation.

## Step 9: Fix and validate

Trace backward from the faulting frame. In LLVM/Unicorn/JIT/native-code crashes, the top frame may
be the victim of earlier corruption. Check callers, generated bytes/IR, ownership, allocator use,
and concurrency before editing the crash site.

Prefer the smallest regression test at the lowest stable boundary. Then validate in this order:

1. exact reproducer, same Release configuration and affected native platform;
2. enough repetitions to cover the observed flake rate, with zero incomplete runs;
3. owning test binary/label;
4. the original CI profile and inventory count;
5. relevant sibling backends/platforms;
6. all required CI legs.

For a flaky fix, one green run is not evidence. Require both zero failures and complete expected
test counts.

## NeverD-specific pitfalls

- Trying `gh run download` for native binaries from `ci.yml`; none are uploaded.
- Using prebuilt LLVM `ON` to declare an integrated-LLVM (`OFF`) failure fixed.
- Reproducing only Debug when CI fails in optimized Release.
- Varying CTest parallelism and `NEVERD_THREADS` together, then attributing the result to one.
- Treating a Unicorn error as a host segfault, or treating in-process JIT code as a child program.
- Treating expected platform skips as passes without checking whether coverage moved to another leg.
- Ignoring workflow cancellation before diagnosing exit 137 or a truncated log as OOM.
- Ignoring a missing GoogleTest summary or under-counted shard after a killed process.
- Trusting QEMU to reproduce native scheduling, alignment, or CPU-feature behavior.
- Hashing debug metadata and calling benign differences nondeterministic codegen.
- Reusing a build directory after changing build type, compiler, architecture, or LLVM mode.
- Treating host-scaled `check-neverd` parallelism as identical to CI's fixed matrix parallelism.
- Leaving a temporary debug workflow or artifact upload behind.

## Quick reference

1. Read current CI/profile definitions.
2. Identify leg, step, exact case, owning binary, signal, and LLVM mode.
3. Separate NeverD/test host/tool/Unicorn/JIT/generated-program layers.
4. Reproduce exact Release configuration and one case.
5. Run the CTest-parallelism × `NEVERD_THREADS` isolation grid.
6. Repeat and capture complete logs/backtraces.
7. Escalate from local native, to Docker/QEMU, to an authorized real-runner debug workflow.
8. Fix the root cause and validate with high-iteration native repro plus complete CI counts.
