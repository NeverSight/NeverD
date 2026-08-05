# Cross-Platform CI Design

## Context

NeverD issue [#1](https://github.com/NeverSight/NeverD/issues/1) requires the
main repository to build and run its complete test suite on Linux, macOS, and
Windows for pushes to `dev` and for pull requests. The repository currently has
no root-level GitHub Actions workflow. Its build is CMake/Ninja based, uses
recursive Git submodules, and exposes the complete test suite through CTest and
the `check-neverd` target.

The implementation should follow the proven conventions in NeverC's current
platform workflows where they apply: native GitHub-hosted runners, macOS 15,
MSVC environment initialization on Windows, stale-run cancellation, and a
project-level `check-*` test gate. NeverC-specific packaging, PGO, LTO, plugin,
network, and release steps are not part of this change.

## Goals

- Build NeverD on Ubuntu x86_64, macOS arm64, and Windows x64.
- Build the default NeverD targets, including the `neverd` CLI, `libneverd`,
  test executables, and normal `ALL` targets.
- Run the complete CTest inventory on every platform and print failing test
  output.
- Make the existing test-process harness portable where Windows compilation or
  execution would otherwise prevent the full suite from running.
- Run automatically for pushes to `dev` and all pull requests, with an optional
  manual trigger for diagnostics.
- Keep the first workflow small and readable so platform failures are easy to
  reproduce from the logged commands.
- Add a README status badge for the workflow.

## Non-Goals

- Release packaging or GitHub Release publication (issue #4).
- Linux/Windows prebuilt LLVM publication (issue #3).
- PGO, LTO, sanitizers, fuzzing, CodeQL, or plugin smoke tests.
- Uploading build artifacts.
- Adding x86_64 macOS, Linux arm64, or Windows arm64 jobs. Issue #1 defines
  "all primary platforms" as the three operating-system hosts above; additional
  host architectures can be follow-up work.
- Changing NeverD engine semantics or weakening test assertions. Portability
  changes are limited to process launching, shell syntax, temporary-path naming,
  and exit-status handling needed to execute the same tests on Windows.
- An explicit job timeout. The user requested that `timeout-minutes` not be set.

## Workflow Architecture

Create one workflow, `.github/workflows/ci.yml`, with one matrix job. A single
matrix keeps checkout, configure, build, and test behavior in one place while
allowing small conditional setup steps for each operating system.

The matrix has three explicit entries:

| Name | Runner | Host architecture | C/C++ compiler | LLVM source |
|---|---|---|---|---|
| Linux x64 | `ubuntu-24.04` | x86_64 | Clang | bundled submodule, built locally |
| macOS arm64 | `macos-15` | arm64 | Apple Clang | `neverd-llvm-v23.0.0` prebuilt package |
| Windows x64 | `windows-latest` | x64 | MSVC | bundled submodule, built locally |

The strategy uses `fail-fast: false`, so a failure on one host does not hide the
other platform results. The workflow does not set `timeout-minutes`.

## Events, Permissions, and Concurrency

- `push` is limited to the `dev` branch.
- `pull_request` is enabled without a branch restriction so every PR is tested.
- `workflow_dispatch` allows maintainers to rerun the matrix manually.
- Workflow permissions are reduced to `contents: read`.
- A concurrency group keyed by workflow plus PR/ref cancels an older run when a
  newer commit arrives on the same branch or pull request.
- Tag pushes do not match the `dev` branch trigger and therefore remain the
  responsibility of the future release workflow.

## Job Flow

1. Check out the repository with recursive submodules.
2. Initialize platform build tools:
   - Linux installs Clang, LLD, and Ninja through APT.
   - macOS installs LLD through Homebrew; the hosted image supplies Apple Clang,
     CMake, and Ninja.
   - Windows initializes the MSVC development environment following NeverC and
     uses the hosted image's CMake, Ninja, Clang, and LLD tools.
3. Print CMake, Ninja, C/C++ compiler, `clang`, `ld.lld`, and `lld-link` versions.
   A missing cross-compilation tool therefore fails before the expensive build.
4. Configure a single-config Ninja Release build with `BUILD_TESTING=ON`.
   GoogleTest discovery uses `PRE_TEST` so test enumeration happens under the
   same runtime environment as CTest.
5. On macOS only, pass `NEVERD_LLVM_PREBUILT=ON` and pin
   `NEVERD_LLVM_PREBUILT_TAG=neverd-llvm-v23.0.0`. Linux and Windows retain the
   default integrated LLVM submodule build.
6. Run the default build target. This covers the CLI, shared library, test
   executables, tools, SDK header copy, and signatures copy.
7. Run CTest over the complete build tree with progress, parallel execution,
   and `--output-on-failure`. This is equivalent to `check-neverd` while keeping
   compilation and test failures in separate GitHub Actions steps.

## Test-Suite Portability Boundary

An initial source audit found that the current test harness is not yet compilable
as a full MSVC test build: it directly includes `<unistd.h>` and uses `getpid`
and `WEXITSTATUS`. Several native end-to-end commands also hard-code the POSIX
null device and POSIX shell grouping. Delivering only the workflow would therefore
fail the Windows acceptance criterion.

The implementation may add one focused, header-only test utility under
`unittests/` that provides:

- the current process ID (`_getpid`/`<process.h>` on Windows, `getpid`/
  `<unistd.h>` on POSIX);
- conversion of `std::system` results into a comparable process exit code
  (direct status on Windows, wait-status decoding on POSIX);
- the platform null device (`NUL` or `/dev/null`);
- shell-safe quoting/redirection for paths and commands used by tests; and
- the native executable suffix where a host-compiled fixture needs one.

The duplicated process handling in these known consumers should use that
utility:

- `unittests/lift/NeverDLiftFixture.h`
- `unittests/semantic/SemanticRoundTripFixture.h`
- `unittests/semantic/CLIEndToEndTests.cpp`
- `unittests/semantic/PatchFullSubstRTTests.cpp`

These changes must preserve the existing commands and assertions on POSIX. On
Windows they should execute the equivalent command rather than broadly disable
the suite. A narrowly documented skip is acceptable only when a test inherently
depends on a host behavior Windows cannot provide; a skip must not hide a build,
lift, decompile, patch, or semantic-equivalence failure.

## Failure Handling and Diagnostics

- Dependency and tool-version checks fail early with the missing executable in
  the step log.
- Configure and build are separate steps, preserving the failing CMake or Ninja
  command and its complete output.
- CTest uses `--output-on-failure`, so failed GoogleTest assertions and process
  errors appear directly in the Actions log.
- Matrix `fail-fast: false` preserves results from all operating systems.
- No `continue-on-error` is used; any platform build or test failure makes the
  workflow fail.
- No secrets, write permissions, or generated build products are persisted.

## Validation

Before handoff, validate the change at five levels:

1. Parse the YAML and inspect the resolved event and matrix structure.
2. Run a GitHub Actions-oriented workflow linter if one is available locally.
3. Configure, build, and run the complete CTest suite on the available macOS
   arm64 development host using the same Release/prebuilt-LLVM settings.
4. Treat Linux and Windows support as fully verified only after the corresponding
   GitHub-hosted matrix jobs complete successfully. Local static validation is
   not sufficient evidence for cross-platform completion.
5. Inspect each platform's CTest inventory and result counts so accidental test
   filtering or broad Windows skips cannot be mistaken for full-suite coverage.

## Acceptance-Criteria Mapping

| Issue #1 requirement | Design evidence |
|---|---|
| Root CI workflow | `.github/workflows/ci.yml` |
| Pushes to `dev` and pull requests | `push.branches`, `pull_request` triggers |
| Linux, macOS, Windows build | Three-entry explicit runner matrix |
| CLI, shared library, tests built | Default CMake build with `BUILD_TESTING=ON` |
| Full suite on every host | Unfiltered CTest invocation in the matrix job |
| Windows test harness builds/runs | Focused cross-platform process utility and migrated consumers |
| Actionable failure output | Separate steps plus `--output-on-failure` |
| Recursive submodules | Checkout `submodules: recursive` |
| Reasonable macOS runtime | Published arm64 prebuilt LLVM package |
| README visibility | CI badge linked to the workflow |
| No artifacts or secrets | Read-only permissions and no upload steps |

## Trade-Offs

Linux and Windows will have long cold builds because their prebuilt LLVM packages
do not exist yet. This is accepted for issue #1: it preserves real source-build
coverage and avoids coupling the initial CI implementation to issue #3. Caching
is deliberately deferred until actual run data shows it is needed; this keeps
the first workflow aligned with the user's request for the simplest solution.
