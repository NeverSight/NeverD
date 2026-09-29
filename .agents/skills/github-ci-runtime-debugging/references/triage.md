# Triage

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
`cancel-in-progress: true`, and closing a pull request cancels its unfinished runs; either a newer
push or the close can cancel the old run and mimic a killed or incomplete test process.

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
