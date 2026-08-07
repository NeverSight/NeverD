# CI Fail-Fast and Windows Process Launch Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make each CI platform stop promptly on its first test failure, fix the shared Windows `cmd.exe` command-envelope defect, and prove all three hosted platform profiles pass.

**Architecture:** Keep individual argument/path escaping in the existing `TestProcess.h` helpers and add one centralized `runShellCommand` boundary that supplies the Windows-only whole-command quote envelope before calling `std::system`. Add CTest's platform-local `--stop-on-failure` policy without changing matrix-wide `fail-fast: false`, then migrate every unit-test process caller and validate locally plus on GitHub-hosted runners.

**Tech Stack:** C++17, GoogleTest, CMake/CTest, Python `unittest`, GitHub Actions YAML, Bash, GitHub CLI

---

## File Structure

- Modify `.github/workflows/ci.yml`: add the platform-local CTest early-stop flag.
- Modify `scripts/tests/test_ci_configuration.py`: lock early-stop and
  matrix-wide continuation to their intended workflow scopes.
- Modify `unittests/TestProcess.h`: own `std::system` and the Windows command
  envelope in one header-only helper.
- Modify `unittests/TestProcessTests.cpp`: add a real quoted-command and quoted-
  redirection process regression.
- Modify `unittests/lift/NeverDLiftFixture.h`: migrate lift subprocesses to the
  centralized helper.
- Modify `unittests/semantic/SemanticRoundTripFixture.h`: migrate semantic
  subprocesses to the centralized helper.
- Modify `unittests/semantic/CLIEndToEndTests.cpp`: migrate CLI subprocesses to
  the centralized helper.
- Modify `unittests/semantic/PatchFullSubstRTTests.cpp`: migrate availability
  probes and native executable runs to the centralized helper.

The implementation follows `@systematic-debugging`: treat the first hosted
failure as the root-cause input. The code changes follow `@tdd`: demonstrate a
failing contract or compile first, make the smallest production change, and
then re-run the focused tests.

### Task 1: Bind CI Early-Stop Semantics

**Files:**
- Modify: `scripts/tests/test_ci_configuration.py:55-91`
- Modify: `.github/workflows/ci.yml:139-158`

- [ ] **Step 1: Write the failing workflow contract test**

Add a test that extracts the strategy and named CTest step rather than searching
the whole YAML indiscriminately:

```python
    def test_workflow_stops_each_failed_profile_but_keeps_other_hosts_running(self):
        source = WORKFLOW.read_text(encoding="utf-8")
        strategy = source.split("    strategy:\n", 1)[1].split(
            "\n    steps:", 1
        )[0]
        self.assertIn("      fail-fast: false\n", strategy)

        step_marker = "      - name: Run selected test profile\n"
        self.assertEqual(source.count(step_marker), 1)
        run_step = source.split(step_marker, 1)[1].split(
            "\n      - name:", 1
        )[0]
        self.assertIn("set -o pipefail", run_step)
        self.assertIn("--stop-on-failure", run_step)
        self.assertIn("--output-on-failure", run_step)
```

- [ ] **Step 2: Run the test and verify RED**

Run:

```bash
python3 -m unittest \
  scripts.tests.test_ci_configuration.CiConfigurationTests.test_workflow_stops_each_failed_profile_but_keeps_other_hosts_running
```

Expected: FAIL because `--stop-on-failure` is absent from the named run step.

- [ ] **Step 3: Add the minimal workflow flag**

Add the flag to CTest in `Run selected test profile`, adjacent to the other
failure-reporting option:

```yaml
          ctest --test-dir build-ci \
            --build-config Release \
            --label-exclude "$EXCLUDE_LABELS" \
            --stop-on-failure \
            --output-on-failure \
```

Do not change `strategy.fail-fast: false`, the profile expressions, the pipe to
`tee`, or the success-only executed-count assertion.

- [ ] **Step 4: Run the focused and complete configuration tests**

Run:

```bash
python3 -m unittest \
  scripts.tests.test_ci_configuration.CiConfigurationTests.test_workflow_stops_each_failed_profile_but_keeps_other_hosts_running
python3 -m unittest discover -s scripts/tests -p 'test_*.py'
```

Expected: both commands PASS; the full discovery count remains at least the
existing 15 tests plus the new contract test.

- [ ] **Step 5: Commit the CI policy**

```bash
git add .github/workflows/ci.yml scripts/tests/test_ci_configuration.py
git commit -m "ci: stop test profiles after first failure"
```

### Task 2: Add a Failing Quoted-Process Regression and Central Helper

**Files:**
- Modify: `unittests/TestProcessTests.cpp:1-41`
- Modify: `unittests/TestProcess.h:4-105`

- [ ] **Step 1: Write the real process regression**

Add `<algorithm>`, `<filesystem>`, `<fstream>`, and `<iterator>` to
`TestProcessTests.cpp`. Add a local file reader and cleanup guard:

```cpp
namespace {

namespace fs = std::filesystem;

std::string readFile(const fs::path &Path) {
  std::ifstream Input(Path, std::ios::binary);
  return {std::istreambuf_iterator<char>(Input),
          std::istreambuf_iterator<char>()};
}

struct ScopedDirectory {
  fs::path Path;
  ~ScopedDirectory() {
    std::error_code Error;
    fs::remove_all(Path, Error);
  }
};

} // namespace
```

Change `DecodesSystemExitCode` to call the not-yet-implemented
`runShellCommand` instead of `std::system`. Then add:

```cpp
TEST(TestProcess, RunsQuotedExecutableWithQuotedRedirects) {
  auto Directory = fs::temp_directory_path() /
                   ("neverd process " + std::to_string(currentProcessId()));
  std::error_code Error;
  fs::remove_all(Directory, Error);
  Error.clear();
  ASSERT_TRUE(fs::create_directories(Directory, Error)) << Error.message();
  ScopedDirectory Cleanup{Directory};

  auto StdoutPath = Directory / "stdout file.txt";
  auto StderrPath = Directory / "stderr file.txt";
#ifdef _WIN32
  const char *CommandProcessor = std::getenv("COMSPEC");
  ASSERT_NE(CommandProcessor, nullptr);
  std::string Command = shellQuote(CommandProcessor) +
                        " /d /c echo neverd-process-probe";
#else
  std::string Command = shellQuote("/bin/echo") +
                        " neverd-process-probe";
#endif
  Command += redirectOutput(StdoutPath.string(), StderrPath.string());

  EXPECT_EQ(systemExitCode(runShellCommand(Command)), 0);
  auto Output = readFile(StdoutPath);
  Output.erase(std::remove(Output.begin(), Output.end(), '\r'), Output.end());
  EXPECT_EQ(Output, "neverd-process-probe\n");
  EXPECT_EQ(readFile(StderrPath), "");
}
```

The Windows branch deliberately uses a quoted executable plus two redirect
paths containing spaces. This reproduces the command shape from the hosted
failure without depending on NeverD engine behavior.

- [ ] **Step 2: Build and verify RED**

Run:

```bash
cmake --build build --target NeverDTestProcessTests --parallel 4
```

Expected: compilation FAILS because `runShellCommand` is not declared.

- [ ] **Step 3: Implement the smallest centralized process boundary**

Add `<cstdlib>` to `TestProcess.h`, then add this helper before
`systemExitCode`:

```cpp
inline int runShellCommand(std::string_view Command) {
#ifdef _WIN32
  std::string NativeCommand;
  NativeCommand.reserve(Command.size() + 2);
  NativeCommand.push_back('"');
  NativeCommand.append(Command);
  NativeCommand.push_back('"');
#else
  std::string NativeCommand(Command);
#endif
  return std::system(NativeCommand.c_str());
}
```

The helper returns the raw C runtime status. Callers must retain
`systemExitCode` when they need a normalized child exit code.

- [ ] **Step 4: Build and run the focused unit tests**

Run:

```bash
cmake --build build --target NeverDTestProcessTests --parallel 4
ctest --test-dir build -R '^TestProcess\.' --output-on-failure
```

Expected: build succeeds and all five `TestProcess.*` tests PASS locally. The
Windows branch is fully accepted only after the same test passes on the hosted
Windows job.

- [ ] **Step 5: Commit the process boundary and regression**

```bash
git add unittests/TestProcess.h unittests/TestProcessTests.cpp
git commit -m "test: fix quoted Windows shell commands"
```

### Task 3: Migrate All Unit-Test Process Callers

**Files:**
- Modify: `unittests/lift/NeverDLiftFixture.h:81`
- Modify: `unittests/semantic/SemanticRoundTripFixture.h:217-218`
- Modify: `unittests/semantic/CLIEndToEndTests.cpp:61`
- Modify: `unittests/semantic/PatchFullSubstRTTests.cpp:34252-34256`
- Test: `unittests/TestProcessTests.cpp`

- [ ] **Step 1: Replace each direct C runtime call**

Use the centralized helper without changing command construction or exit-code
normalization:

```cpp
int RC = neverd::test::runShellCommand(Cmd);
```

For the semantic fixture's temporary expression:

```cpp
int RC = neverd::test::runShellCommand(
    Cmd + neverd::test::redirectOutput(OutF, ErrF));
```

For the patch helper functions:

```cpp
return neverd::test::systemExitCode(
           neverd::test::runShellCommand(Cmd)) == 0;
```

and:

```cpp
int RC = neverd::test::runShellCommand(Cmd);
```

- [ ] **Step 2: Prove the ownership boundary statically**

Run:

```bash
rg -n 'std::system' unittests
```

Expected: exactly one match, inside `unittests/TestProcess.h` in
`runShellCommand`.

- [ ] **Step 3: Rebuild all affected test executables**

Run:

```bash
cmake --build build --parallel 4
```

Expected: the default build succeeds with all changed headers and consumers.

- [ ] **Step 4: Run the process tests and focused host profile**

Run:

```bash
ctest --test-dir build -R '^TestProcess\.' --output-on-failure
ctest --test-dir build \
  --label-exclude '^NeverD(Semantic|PatchFull)Tests$' \
  --stop-on-failure \
  --output-on-failure \
  --parallel 4
```

Expected: process tests PASS; the focused profile completes with zero failures
and retains only documented optional skips.

- [ ] **Step 5: Commit the caller migration**

```bash
git add \
  unittests/lift/NeverDLiftFixture.h \
  unittests/semantic/SemanticRoundTripFixture.h \
  unittests/semantic/CLIEndToEndTests.cpp \
  unittests/semantic/PatchFullSubstRTTests.cpp
git commit -m "test: centralize shell command execution"
```

### Task 4: Perform Local Release-Equivalent Validation

**Files:**
- Verify only; modify production or test files only if a failure exposes a
  specific defect, following a new RED/GREEN cycle.

- [ ] **Step 1: Run every script test**

```bash
python3 -m unittest discover -s scripts/tests -p 'test_*.py'
```

Expected: all inventory and workflow contract tests PASS.

- [ ] **Step 2: Reconfigure and build the existing local tree**

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel 4
```

Expected: configuration and the unfiltered default build both succeed.

- [ ] **Step 3: Repeat the focused execution from a fresh inventory**

```bash
ctest --test-dir build --show-only=json-v1 |
  python3 scripts/audit_ci_test_inventory.py \
    --profile windows-focused \
    --exclude-label-regex '^NeverD(Semantic|PatchFull)Tests$' \
    --matrix-name 'local focused validation'
ctest --test-dir build \
  --label-exclude '^NeverD(Semantic|PatchFull)Tests$' \
  --stop-on-failure \
  --output-on-failure \
  --parallel 4
```

Expected: inventory audit accepts the profile and all selected tests pass.

- [ ] **Step 4: Verify repository hygiene**

```bash
git diff --check
test "$(rg -n 'std::system' unittests | wc -l | tr -d ' ')" = 1
git status --short --branch
```

Expected: no whitespace errors, one centralized `std::system` occurrence, and
no uncommitted implementation changes.

### Task 5: Push and Close the Hosted Three-Platform Loop

**Files:**
- Modify only when a hosted failure supplies a new reproducible root cause.

- [ ] **Step 1: Push the tested commits**

```bash
git push origin dev
```

Expected: `origin/dev` advances to the local HEAD and the `CI` workflow starts.

- [ ] **Step 2: Resolve the run for the exact pushed commit**

```bash
HEAD_SHA="$(git rev-parse HEAD)"
gh run list --workflow CI --branch dev --commit "$HEAD_SHA" \
  --json databaseId,status,conclusion,url,headSha --limit 5
```

Run creation can be briefly asynchronous. Retry the query at most 12 times at
intervals no longer than 10 seconds until a record whose `headSha` equals
`HEAD_SHA` appears. Record that row's `databaseId` as `RUN_ID`, then guard
against selecting a stale run:

```bash
test "$(gh run view RUN_ID --json headSha --jq .headSha)" = "$HEAD_SHA"
```

Expected: exactly one current run whose `headSha` equals `HEAD_SHA`; its ID is
captured and verified before monitoring starts.

- [ ] **Step 3: Monitor without losing platform detail**

Poll the exact run with:

```bash
gh run view RUN_ID --json status,conclusion,url,jobs
```

Expected: Linux x64, macOS arm64, and Windows x64 each reach a terminal state.
Do not infer success from the top-level state alone; inspect every job.

- [ ] **Step 4: Validate the Windows regression boundary**

Inspect the Windows job log when it reaches tests:

```bash
gh run view RUN_ID --job WINDOWS_JOB_ID --log
```

Expected: `TestProcess.RunsQuotedExecutableWithQuotedRedirects` passes, the old
`AArch64_Arith.AllStagesPass` `cmd.exe` syntax error does not recur, and the
audited focused profile completes (about 1,858 tests at the current inventory).

- [ ] **Step 5: Diagnose and fix any new first failure**

If a job fails, download or inspect only that job's complete log, identify its
first actual error, reproduce locally where possible, add a focused regression,
implement the minimal fix, run the proportional local suite, commit, push, and
return to Step 2 for the new exact SHA. Do not respond by disabling tests or
loosening inventory assertions.

- [ ] **Step 6: Record final hosted evidence**

Completion requires all of the following:

- Linux x64 job: `success`.
- macOS arm64 job: `success`.
- Windows x64 job: `success`.
- Top-level workflow: `success`.
- Local branch and `origin/dev` point at the same final commit.
