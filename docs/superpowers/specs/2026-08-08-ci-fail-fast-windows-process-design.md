# CI Fail-Fast and Windows Process Launch Design

## Context

GitHub Actions run
[`31182496171`](https://github.com/NeverSight/NeverD/actions/runs/31182496171)
for commit `6505ea90516beb9d1bfc22a13998d3cfd5a4be79` did not report a
successful Windows test job. Linux and macOS completed successfully, while the
Windows job continued running tests for roughly three hours and was cancelled
when a newer push triggered the workflow concurrency policy.

The Windows log shows the first failing NeverDLift test at test 29:
`AArch64_Arith.AllStagesPass`. Its child process exits with code 1 and prints
`The filename, directory name, or volume label syntax is incorrect.` The same
process-launch error then repeats across more than 1,200 tests before the job is
cancelled. This is one shared harness defect, not evidence of more than 1,200
independent lifting regressions.

CTest currently uses its default collect-all behavior. It continues scheduling
the remaining selected tests after a failure, and only returns nonzero after the
profile finishes. The workflow's `set -o pipefail` correctly propagates that
eventual nonzero status through `tee`; it simply does not make CTest stop early.
Matrix `fail-fast: false` is a separate setting: it preserves results from the
other operating systems when one matrix job fails.

The shared test harness constructs shell command strings from individually
quoted executable paths and arguments, appends quoted output redirections, and
passes the resulting string directly to `std::system`. On Windows, the C runtime
invokes `cmd.exe /c`. `cmd.exe` applies special parsing and quote stripping when
the command contains multiple quoted tokens. A command shaped like
`\"program\" \"argument\" >\"output\" 2>\"error\"` therefore loses the command-level
envelope needed by `cmd.exe`, producing the observed syntax error. Wrapping the
complete command in an additional quote pair preserves its inner token quoting.

## Goals

- Make a failing CTest profile stop scheduling new tests after its first
  observed failure and make the platform job fail immediately afterward.
- Keep Linux, macOS, and Windows matrix jobs independent so one failing host
  does not hide results from the other hosts.
- Correct the shared Windows shell-command envelope without weakening path
  quoting or disabling tests.
- Route all unit-test shell execution through one helper so behavior cannot
  drift between fixtures.
- Add a regression test that actually launches a quoted command with quoted
  stdout and stderr paths.
- Re-run hosted CI and continue fixing evidence-backed failures until all three
  platform jobs pass.

## Non-Goals

- Replacing the shell-based test harness with `CreateProcess`, `posix_spawn`, or
  a general subprocess library. Existing test commands rely on shell
  redirection, grouping, and operators, so that would be a substantially larger
  migration.
- Making one platform failure cancel the other matrix jobs.
- Changing CI profile ownership or expanding the Windows focused profile. The
  current audited profiles remain Linux semantic, macOS patch, and Windows
  focused.
- Hiding failures with `continue-on-error`, test skips, retries, or reduced
  assertions.
- Treating arbitrary user-provided strings as safe shell input. Commands and
  paths in this harness are repository-controlled test inputs; introducing a
  security boundary for untrusted command construction is outside this change.

## Chosen Design

### Platform-local early failure

Add `--stop-on-failure` to the CTest command in the workflow step named
`Run selected test profile`. CTest may allow tests already running under
`--parallel` to finish, but it must stop scheduling additional tests once a
failure is observed. Because the step starts with `set -o pipefail`, CTest's
nonzero result remains the pipeline result even though output is piped through
`tee`; no explicit status rewriting is needed.

Keep `strategy.fail-fast: false`. The resulting failure model is intentional:

- within one platform, stop promptly after the first observed failure;
- across platforms, continue collecting Linux, macOS, and Windows evidence.

The executed-count assertion after CTest is a success-path inventory invariant.
GitHub Actions invokes Bash with `-e`; on a test failure, `pipefail` preserves
CTest's nonzero pipeline status and `errexit` exits the step before the count
parser runs. On success, the parser must still prove that the selected test
count equals the audited inventory count.

### Central shell execution helper

Add `runShellCommand(std::string_view Command)` to
`unittests/TestProcess.h`. The helper owns the call to `std::system` and returns
its raw status so existing callers continue converting it with
`systemExitCode`.

Its platform behavior is:

- POSIX: copy the command to a null-terminated string and pass it unchanged to
  `std::system`.
- Windows: surround the complete command with one additional pair of double
  quotes before passing it to `std::system`. Inner executable, argument, and
  redirect-path quotes remain intact for `cmd.exe /c` parsing.

`TestProcess.h` will include `<cstdlib>` because it becomes the single owner of
the direct C runtime call. The helper is deliberately narrow: `shellQuote`
continues escaping individual tokens, redirect helpers continue constructing
shell syntax, and `systemExitCode` continues normalizing the returned status.

Migrate every direct unit-test `std::system` consumer to
`runShellCommand`, including:

- `unittests/lift/NeverDLiftFixture.h`
- `unittests/semantic/SemanticRoundTripFixture.h`
- `unittests/semantic/CLIEndToEndTests.cpp`
- `unittests/semantic/PatchFullSubstRTTests.cpp`
- `unittests/TestProcessTests.cpp`

After migration, the only direct `std::system` call under `unittests/` should be
inside `runShellCommand` itself.

### Process integration regression

Extend `unittests/TestProcessTests.cpp` with a test that exercises the full
boundary rather than only comparing constructed strings. The test creates a
temporary directory and stdout/stderr files whose names contain spaces, builds
a command from a quoted executable and quoted redirections using the production
helpers, executes it through `runShellCommand`, and asserts:

- the normalized exit code is zero;
- stdout contains an expected marker;
- stderr is empty or contains only explicitly expected content; and
- temporary files are cleaned up.

On Windows, the executable path must itself be quoted and the command must pass
through `cmd.exe` semantics, reproducing the shape that failed in hosted CI. On
POSIX, the analogous quoted executable and redirect paths verify that the new
helper remains a pass-through. This is an execution test, not only a string
formatting test.

### Workflow contract regression

Extend `scripts/tests/test_ci_configuration.py` so it structurally binds
`--stop-on-failure` to the `Run selected test profile` step. Retain and assert
`strategy.fail-fast: false`. These assertions encode the distinction between
platform-local early stop and matrix-wide diagnostic completion, preventing a
future cleanup from accidentally reversing either policy.

## Alternatives Considered

### Native subprocess API

Using `CreateProcess` on Windows and `posix_spawn` on POSIX would avoid shell
quote rules and provide stronger process control. It was rejected for this fix
because the current tests intentionally use redirection and shell composition.
Replacing those features would expand the scope across several large fixtures
and introduce more risk than the diagnosed defect requires.

### Quote only paths that contain spaces

Leaving simple paths unquoted can mask the immediate hosted-run failure, but it
would make behavior depend on runner installation paths and fail again as soon
as a workspace or tool path contains a space. It also leaves duplicated launch
logic untouched. This is not a robust contract.

### Preserve collect-all CTest behavior

Allowing every selected test to run can expose multiple independent failures,
but the current parallel profiles contain tens of thousands of tests. A shared
harness failure then consumes hours and floods the log with duplicates. The
other matrix hosts already provide independent diagnostics, so platform-local
early stop is the better trade-off.

## Failure Handling and Diagnostics

- A child-process launch failure remains a normal nonzero test assertion and is
  printed by CTest's `--output-on-failure` output.
- CTest returns nonzero on the first observed failing batch; `pipefail` makes
  the GitHub Actions step and platform job fail.
- The other matrix jobs continue because `strategy.fail-fast` remains false.
- Successful runs still verify their executed count against the prior JSON
  inventory audit.
- No retry or skip is added for deterministic command-construction failures.
- If hosted CI exposes a different failure after this root cause is removed,
  diagnose that failure from its first occurrence, add the smallest relevant
  regression where practical, and repeat until the matrix is green.

## Validation

1. Run the Python configuration tests and confirm the workflow contract asserts
   both `--stop-on-failure` and `fail-fast: false` in their correct scopes.
2. Build and run the `TestProcess` unit tests, including the real quoted-command
   integration regression.
3. Search `unittests/` and confirm no direct `std::system` consumer remains
   outside `TestProcess.h`.
4. Run the locally available focused CTest profile to catch regressions in the
   migrated command consumers.
5. Push the branch and inspect the GitHub-hosted Linux, macOS, and Windows jobs.
6. Confirm Windows passes the prior first-failure boundary at test 29 and
   completes its audited focused inventory (currently about 1,858 tests).
7. Confirm Linux and macOS complete their selected audited profiles.
8. For any new hosted failure, use the first failing test and its process output
   as the next root-cause input; do not classify the work complete until all
   three platform jobs are green.

## Acceptance Criteria

- `Run selected test profile` invokes CTest with `--stop-on-failure`, retains
  `--output-on-failure`, and is protected by `set -o pipefail`.
- Matrix `strategy.fail-fast` remains false.
- All unit-test shell commands execute through `runShellCommand`.
- The Windows helper adds one command-level quote envelope while POSIX behavior
  remains unchanged.
- A real quoted-executable/quoted-redirection regression passes on every host.
- The successful-run inventory assertion remains active.
- GitHub-hosted Linux x64, macOS arm64, and Windows x64 jobs all finish
  successfully; Windows no longer emits the old `cmd.exe` syntax error.
