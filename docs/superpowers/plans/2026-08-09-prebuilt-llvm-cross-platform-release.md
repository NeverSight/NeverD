# Cross-Platform Prebuilt LLVM Release Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Publish and consume NeverD LLVM packages for macOS arm64, Linux x86_64, and Windows x64, while making replacement of an existing mutable GitHub Release an explicit and safe manual operation.

**Architecture:** A preparation job derives the LLVM source version, validates the requested release tag, and rejects accidental replacement of an existing or immutable release before the expensive matrix starts. One build matrix produces platform-native archives with identical trimmed LLVM settings and per-platform compiler caches. The NeverD CMake consumer resolves host OS/architecture to the matching package and archive format, verifies SHA256 before extraction, and keeps the existing tag/architecture cache layout. Normal push and pull-request CI continues building the LLVM submodule from source; a Boolean `workflow_dispatch` input explicitly opts a manual CI run into the prebuilt packages.

**Tech Stack:** GitHub Actions YAML, CMake 3.20+, Bash, PowerShell, Python `unittest`, ccache, sccache, Ninja, Clang/clang-cl, GitHub Releases.

---

## File structure

- Create `scripts/tests/test_prebuilt_llvm_release.py`: executable contract tests for workflow policy and CMake host/package resolution.
- Modify `third_party/llvm-project/.github/workflows/neverd-release.yml`: release preflight, three-host build matrix, packaging, caching, and explicit asset replacement.
- Modify `cmake/NeverDLLVMPrebuilt.cmake`: Linux and Windows package selection plus `.zip` support.
- Modify `.github/workflows/ci.yml`: keep push/pull-request CI on source LLVM and expose prebuilt LLVM as an explicit manual-dispatch option for the existing three-host matrix.
- Modify `README.md`: supported platforms, manual dispatch procedure, and immutable package-revision guidance.
- Update `third_party/llvm-project` gitlink after the workflow change is committed in that repository.

### Task 1: Add failing release-contract tests

**Files:**
- Create: `scripts/tests/test_prebuilt_llvm_release.py`
- Test: `scripts/tests/test_prebuilt_llvm_release.py`

- [ ] **Step 1: Write workflow contract tests**

Add tests that read `third_party/llvm-project/.github/workflows/neverd-release.yml` and require all of the following exact contracts:

```python
EXPECTED_PACKAGES = {
    "neverd-llvm-macos-arm64": ("macos-14", "tar.xz"),
    "neverd-llvm-linux-x86_64": ("ubuntu-24.04", "tar.xz"),
    "neverd-llvm-windows-x64": ("windows-latest", "zip"),
}

for package, (runner, archive) in EXPECTED_PACKAGES.items():
    self.assertIn(f"pkg: {package}", source)
    self.assertIn(f"runner: {runner}", source)
    self.assertIn(f"archive: {archive}", source)

self.assertIn("overwrite_existing_assets:", source)
self.assertIn("overwrite_files: ${{ needs.prepare.outputs.overwrite }}", source)
self.assertIn("mozilla-actions/sccache-action@", source)
self.assertIn("-DLLVM_CCACHE_BUILD=ON", source)
```

Also require a preparation job to validate existing releases before `build`, `BUILDINFO.txt` to contain full commit/platform/architecture metadata, and the release job to consume the preparation outputs.

Require `.github/workflows/ci.yml` to declare a Boolean `workflow_dispatch.inputs.use_prebuilt_llvm` input with `default: false`. The configure step must map only a manually selected `true` value to `ON`; push, pull-request, and default manual runs must pass `OFF` through one dynamic CMake argument. The existing Linux x64, macOS arm64, and Windows x64 matrix remains the compatibility proof when the manual option is selected.

- [ ] **Step 2: Write CMake package-resolution tests**

Generate a temporary CMake script for each host tuple, include `cmake/NeverDLLVMPrebuilt.cmake`, call `_neverd_resolve_prebuilt_llvm_package`, and assert these results:

```python
CASES = (
    ("Darwin", "arm64", "", "macos;arm64;neverd-llvm-macos-arm64;tar.xz"),
    ("Darwin", "x86_64", "arm64", "macos;arm64;neverd-llvm-macos-arm64;tar.xz"),
    ("Linux", "x86_64", "", "linux;x86_64;neverd-llvm-linux-x86_64;tar.xz"),
    ("Windows", "AMD64", "", "windows;x64;neverd-llvm-windows-x64;zip"),
)
```

Add negative cases for macOS universal builds, Linux arm64, Windows arm64, and an unsupported host OS. Each must fail with a useful diagnostic.

- [ ] **Step 3: Run the tests to verify they fail**

Run:

```bash
python3 -m unittest scripts.tests.test_prebuilt_llvm_release -v
```

Expected: failures because the workflow has one macOS entry and the resolver function does not exist.

- [ ] **Step 4: Commit the test contract**

```bash
git add scripts/tests/test_prebuilt_llvm_release.py
git commit -m "test: define cross-platform LLVM release contract"
```

### Task 2: Implement safe cross-platform publishing in llvm-project

**Files:**
- Modify: `third_party/llvm-project/.github/workflows/neverd-release.yml`
- Test: `scripts/tests/test_prebuilt_llvm_release.py`

- [ ] **Step 1: Add manual replacement input and preparation job**

Keep `release_tag` optional so a dispatch can still build run artifacts only. Add this Boolean input:

```yaml
overwrite_existing_assets:
  description: 'Replace every asset if release_tag already exists (cached consumers must use a new tag or clear their cache)'
  required: true
  type: boolean
  default: false
```

The `prepare` job must:

1. check out the exact dispatch/tag SHA;
2. read `LLVM_VERSION_MAJOR`, `MINOR`, and `PATCH`;
3. accept only `neverd-llvm-v<source-version>` or `neverd-llvm-v<source-version>-rN`;
4. query `repos/$GITHUB_REPOSITORY/releases/tags/<tag>` when publishing;
5. reject an existing manual release unless `overwrite_existing_assets` is true;
6. reject replacement when the release reports `immutable: true`;
7. output `version`, `tag`, `publish`, `overwrite`, and `release_exists`.

For tag pushes, replacement is allowed so rerunning the original tag workflow remains idempotent. Key concurrency by the resolved manual release tag instead of only the dispatch branch.

- [ ] **Step 2: Expand the build matrix**

Use these entries:

```yaml
include:
  - name: macOS arm64
    platform: macos
    arch: arm64
    runner: macos-14
    pkg: neverd-llvm-macos-arm64
    archive: tar.xz
  - name: Linux x86_64
    platform: linux
    arch: x86_64
    runner: ubuntu-24.04
    pkg: neverd-llvm-linux-x86_64
    archive: tar.xz
  - name: Windows x64
    platform: windows
    arch: x64
    runner: windows-latest
    pkg: neverd-llvm-windows-x64
    archive: zip
```

Install Ninja, ccache, Clang, LLD, and xz on Linux. Retain Homebrew ccache on macOS. On Windows, activate the MSVC x64 environment, use preinstalled clang-cl/Ninja, install sccache with a commit-pinned `mozilla-actions/sccache-action`, and set `SCCACHE_GHA_ENABLED=true` plus the C/C++ compiler launchers.

- [ ] **Step 3: Keep one trimmed LLVM configuration across all hosts**

Configure every host with static libraries, `X86;ARM;AArch64`, no projects/runtimes/tools/tests/docs/examples/benchmarks/bindings/utilities/shared LLVM dylib, disabled optional compression/network/terminal dependencies, RTTI enabled to match NeverD's integrated LLVM configuration, and Release mode. Add only host-specific compiler, deployment-target, cache-launcher, and linker arguments.

- [ ] **Step 4: Verify, describe, and package each install**

Fail unless `lib/cmake/llvm/LLVMConfig.cmake` and the platform's LLVMCore static library exist. Write `BUILDINFO.txt` containing at least:

```text
name: <package>
llvm_version: <version>
llvm_commit: <full SHA>
platform: <macos|linux|windows>
host_arch: <arm64|x86_64|x64>
runner: <runner label>
targets: X86;ARM;AArch64
build_type: Release
link: static
built_at: <UTC timestamp>
built_by: <workflow and run URL>
```

Create `.tar.xz` plus `.sha256` on macOS/Linux and `.zip` plus `.sha256` on Windows. Upload both files as one run artifact per matrix entry.

- [ ] **Step 5: Publish through explicit overwrite policy**

Download all matrix artifacts and invoke the pinned release action with:

```yaml
tag_name: ${{ needs.prepare.outputs.tag }}
target_commitish: ${{ github.sha }}
overwrite_files: ${{ needs.prepare.outputs.overwrite }}
generate_release_notes: ${{ needs.prepare.outputs.release_exists != 'true' }}
```

Include both `dist/*.tar.xz*` and `dist/*.zip*`. Existing tags are never force-moved: GitHub ignores `target_commitish` when a tag already exists, while each archive records the actual source SHA.

- [ ] **Step 6: Run the workflow contract tests**

Run:

```bash
python3 -m unittest scripts.tests.test_prebuilt_llvm_release -v
ruby -e "require 'yaml'; YAML.load_file('third_party/llvm-project/.github/workflows/neverd-release.yml')"
```

Expected: workflow contract tests pass and YAML parses successfully.

- [ ] **Step 7: Commit the llvm-project workflow**

```bash
git -C third_party/llvm-project add .github/workflows/neverd-release.yml
git -C third_party/llvm-project commit -m "ci: publish NeverD LLVM for Linux and Windows"
```

### Task 3: Extend the NeverD prebuilt consumer

**Files:**
- Modify: `cmake/NeverDLLVMPrebuilt.cmake`
- Test: `scripts/tests/test_prebuilt_llvm_release.py`

- [ ] **Step 1: Add a focused host resolver**

Implement `_neverd_resolve_prebuilt_llvm_package(out_platform out_arch out_pkg out_archive)` with these supported mappings:

```text
Darwin arm64/aarch64 -> macos, arm64, neverd-llvm-macos-arm64, tar.xz
Linux x86_64/amd64  -> linux, x86_64, neverd-llvm-linux-x86_64, tar.xz
Windows AMD64/x64   -> windows, x64, neverd-llvm-windows-x64, zip
```

Honor a single `CMAKE_OSX_ARCHITECTURES` value for macOS and retain explicit errors for universal or unsupported builds.

- [ ] **Step 2: Make download/checksum/extraction archive-agnostic**

Replace `_tar*` variables with `_archive*`, construct `<pkg>.<archive>`, download the matching `.sha256`, validate that the parsed digest is exactly 64 hexadecimal characters, compute `file(SHA256)`, and extract with `file(ARCHIVE_EXTRACT)`. Preserve `NEVERD_LLVM_PREBUILT_SHA256`, mirror URL support, TLS verification, cleanup on failure, and `$HOME/.cache/neverd-llvm/<tag>/<arch>/`.

- [ ] **Step 3: Run resolver and regression tests**

Run:

```bash
python3 -m unittest scripts.tests.test_prebuilt_llvm_release -v
python3 -m unittest discover -s scripts/tests -v
```

Expected: all package mappings and existing script tests pass.

- [ ] **Step 4: Commit the consumer change**

```bash
git add cmake/NeverDLLVMPrebuilt.cmake scripts/tests/test_prebuilt_llvm_release.py
git commit -m "feat: consume Linux and Windows LLVM packages"
```

### Task 4: Add a manual prebuilt toggle to cross-platform CI and document release semantics

**Files:**
- Modify: `.github/workflows/ci.yml`
- Modify: `README.md`
- Test: `scripts/tests/test_prebuilt_llvm_release.py`

- [x] **Step 1: Write the failing CI dispatch-policy test**

Replace the old test that requires every CI event to use prebuilts with a contract test that requires this input block:

```yaml
workflow_dispatch:
  inputs:
    use_prebuilt_llvm:
      description: 'Use the published prebuilt NeverD LLVM packages'
      required: true
      type: boolean
      default: false
```

The test must also require the guarded expression and dynamic CMake argument shown in Step 3, assert that the workflow contains exactly one `-DNEVERD_LLVM_PREBUILT=` CMake argument, reject hard-coded `-DNEVERD_LLVM_PREBUILT=ON` and `-DNEVERD_LLVM_PREBUILT=OFF` arguments, and retain all three runner entries. Extend the README contract test to require `use_prebuilt_llvm`, normal push/pull-request source builds, and the rule that only a manually selected `true` value enables prebuilts.

- [x] **Step 2: Run the focused test and verify it fails**

Run:

```bash
python3 -m unittest scripts.tests.test_prebuilt_llvm_release.PrebuiltLlvmReleaseWorkflowTests.test_ci_defaults_to_source_llvm_and_manual_dispatch_can_opt_in -v
```

Expected: failure because CI currently hard-codes prebuilt LLVM to `ON` and `workflow_dispatch` has no input.

- [x] **Step 3: Implement the event-aware CI switch**

Add the Boolean input above and configure with one event-aware environment value:

```yaml
env:
  NEVERD_LLVM_PREBUILT_MODE: ${{ github.event_name == 'workflow_dispatch' && inputs.use_prebuilt_llvm && 'ON' || 'OFF' }}
```

Pass it to CMake without duplicating configure steps:

```bash
-DNEVERD_LLVM_PREBUILT="$NEVERD_LLVM_PREBUILT_MODE"
```

Therefore `push`, `pull_request`, and unchecked/default manual dispatches resolve to `OFF`; only a manual dispatch with `use_prebuilt_llvm=true` resolves to `ON`. Keep the three existing clean GitHub-hosted runner entries and their default-target builds/tests. Publish all six assets before actually selecting the prebuilt option on a remote run.

- [x] **Step 4: Document supported hosts and normal usage**

List macOS arm64, Linux x86_64, and Windows x64 with their archive names. Keep the existing configure example and explain that `NEVERD_LLVM_PREBUILT_TAG` selects the package release independently of the LLVM version reported inside it.

- [x] **Step 5: Document manual dispatch, replacement, CI selection, and compiler caching**

Document both UI and CLI operation:

```bash
gh workflow run neverd-release.yml \
  --repo NeverSight/llvm-project \
  --ref main \
  -f release_tag=neverd-llvm-v23.0.0 \
  -f overwrite_existing_assets=true
```

State that this replaces same-named assets only when the release is mutable; it does not move the existing Git tag. Because consumers cache by tag, exact-tag replacement is for repairing a release and existing users must remove `$HOME/.cache/neverd-llvm/neverd-llvm-v23.0.0/`.

For normal source updates with unchanged upstream LLVM `23.0.0`, recommend `neverd-llvm-v23.0.0-r1`, then `-r2`, and update the consumer default. Do not use `23.0.1` unless `LLVM_VERSION_PATCH` itself changes.

Document the build-cache strategy explicitly: macOS and Linux use ccache persisted by `actions/cache`; Windows uses sccache's GitHub Actions cache backend because ccache is not the native clang-cl/MSVC path. State that these caches accelerate rebuilds only and never become release artifacts.

Also state that normal NeverD push/pull-request CI deliberately uses source LLVM. To validate the published packages, manually run the `CI` workflow and select `use_prebuilt_llvm`; leaving it unchecked preserves the source-build path.

- [x] **Step 6: Run CI/documentation contract tests**

Run:

```bash
python3 -m unittest scripts.tests.test_prebuilt_llvm_release -v
```

Expected: CI defaults to source LLVM, manual dispatch can opt into prebuilt LLVM, and documentation mentions every platform, the Windows sccache strategy, explicit overwrite, cache invalidation, and `-rN` package revisions.

- [ ] **Step 7: Commit CI, documentation, and submodule pointer**

```bash
git add .github/workflows/ci.yml README.md third_party/llvm-project
git commit -m "docs: explain prebuilt LLVM release revisions"
```

### Task 5: Final verification and release handoff

**Files:**
- Verify: `third_party/llvm-project/.github/workflows/neverd-release.yml`
- Verify: `cmake/NeverDLLVMPrebuilt.cmake`
- Verify: `.github/workflows/ci.yml`
- Verify: `README.md`
- Verify: `scripts/tests/test_prebuilt_llvm_release.py`

- [ ] **Step 1: Run all local verification**

```bash
python3 -m unittest discover -s scripts/tests -v
cmake -S . -B build-issue-3-smoke -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEVERD_LLVM_PREBUILT=OFF \
  -DBUILD_TESTING=OFF
git diff --check
git -C third_party/llvm-project diff --check
```

Expected: tests and the local source-build configure regression pass with no whitespace errors. This local configure is not the cross-platform prebuilt acceptance test; the clean CI matrix in Step 3 is.

- [ ] **Step 2: Review the two-repository diff**

```bash
git diff origin/dev...HEAD -- .github/workflows/ci.yml README.md cmake/NeverDLLVMPrebuilt.cmake scripts/tests/test_prebuilt_llvm_release.py third_party/llvm-project
git -C third_party/llvm-project diff origin/main...HEAD -- .github/workflows/neverd-release.yml
```

Confirm that unrelated user changes are absent and the parent gitlink points to the intended llvm-project commit.

- [ ] **Step 3: Publish in dependency order after human approval**

Push the llvm-project workflow commit first. Then manually dispatch from `main` using either the recommended new package revision (`neverd-llvm-v23.0.0-r1`, overwrite false) or the explicit repair path (`neverd-llvm-v23.0.0`, overwrite true). Wait for all three jobs and verify six uploaded assets and their digests before pushing the NeverD consumer/CI commit.

After the NeverD push, wait for the automatic three-host CI and confirm every job configured with `NEVERD_LLVM_PREBUILT=OFF`, built the LLVM submodule from source, built the unfiltered default target, and passed its selected tests. Then manually dispatch the `CI` workflow with `use_prebuilt_llvm=true`; wait for all three manual jobs and confirm each configured with `NEVERD_LLVM_PREBUILT=ON`, downloaded and verified its host package, built the unfiltered default target, and passed its selected tests. Do not close Issue #3 until both the default source path and the manually selected prebuilt path succeed on Linux x64, macOS arm64, and Windows x64; the manual three-host results establish consumer compatibility and the macOS non-regression requirement.

Expected assets:

```text
neverd-llvm-macos-arm64.tar.xz
neverd-llvm-macos-arm64.tar.xz.sha256
neverd-llvm-linux-x86_64.tar.xz
neverd-llvm-linux-x86_64.tar.xz.sha256
neverd-llvm-windows-x64.zip
neverd-llvm-windows-x64.zip.sha256
```
