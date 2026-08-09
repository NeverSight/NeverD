# Native Format Completeness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the remaining evidence and documentation gaps for GitHub issue #2 without rewriting loader or patcher paths that already satisfy their acceptance tests.

**Architecture:** Treat the existing PE ARM64, PE ARM32/Thumb, and Mach-O i386 implementations as the production baseline because their focused format, pipeline, and patch regressions pass. Strengthen the boundary tests where acceptance coverage is asymmetric: make both PIC and no-PIC Mach-O i386 objects run the complete lift/decompile pipeline, and centralize generated-C syntax validation so both Windows ARM targets use the same contract. Finish by synchronizing the roadmap status across every maintained locale.

**Tech Stack:** C++20, LLVM Object APIs, GoogleTest, CMake/CTest, Clang cross-target syntax checks, Markdown

**Source specification:** [NeverSight/NeverD issue #2](https://github.com/NeverSight/NeverD/issues/2) and `docs/roadmap/README.md` section 1.

---

## Scope Decision

- Keep `lib/loader/COFF`, `lib/loader/MachO`, and the COFF/Mach-O rewrite backends unchanged unless a new regression fails. Existing tests already prove ARM64 `.pdata` handling, image-level Thumb mode, tiny Thumb boundaries, ARM64/ARM32 PE trampolines and relifting, and common i386 relocation formulas.
- Add missing end-to-end coverage for the no-PIC Mach-O i386 fixture; the current end-to-end case covers only the PIC fixture even though lower-level relocation tests inspect both.
- Reuse one Windows ARM C-syntax helper for both Thumb and AArch64 instead of duplicating test setup.
- Do not claim runnable Mach-O i386 executable coverage on modern macOS. Preserve the README limitation and mark the roadmap item complete based on thin-object integration plus format-independent Mach-O rewrite-backend coverage.

## File Structure

- Modify `unittests/lift/MachOI386RelocationTests.cpp`: parameterize the thin-object pipeline contract over PIC and no-PIC fixtures.
- Modify `unittests/lift/COFFARMFormatTests.cpp`: centralize cross-target C syntax validation and apply it to both Windows ARM pipelines.
- Modify `docs/roadmap/README.md`: mark native format completeness done and link the concrete regression suites.
- Modify `docs/roadmap/README.{ar,de,es,fr,it,ja,ko,ru,zh-CN,zh-TW}.md`: keep localized timeline status aligned with the primary roadmap.

### Task 1: Cover Both Mach-O i386 Object Models End to End

**Files:**
- Modify: `unittests/lift/MachOI386RelocationTests.cpp:45-55`
- Modify: `unittests/lift/MachOI386RelocationTests.cpp:1326-1371`

- [x] **Step 1: Record the missing test inventory**

Run:

```bash
ctest --test-dir build -N -R 'MachOI386Pipeline.*(PIC|NoPIC)'
```

Expected before the change: zero parameterized pipeline tests; only `MachOI386Relocation.ThinObjectCompletesLiftAndDecompilation` exists.

- [x] **Step 2: Add a named pipeline case model and parameterized fixture**

Add beside `MachOI386Relocation`:

```cpp
struct MachOI386PipelineCase {
  const char *FixtureName;
  const char *TestName;
};

class MachOI386Pipeline
    : public MachOI386Relocation,
      public ::testing::WithParamInterface<MachOI386PipelineCase> {};
```

Convert the single PIC-only test body to:

```cpp
TEST_P(MachOI386Pipeline, CompletesLiftAndDecompilation) {
  const fs::path Path = fixture(GetParam().FixtureName);
  // Keep every existing all-stage, strict-mode, symbol-reference, and
  // decompiled-C assertion unchanged.
}

INSTANTIATE_TEST_SUITE_P(
    ThinObjects, MachOI386Pipeline,
    ::testing::Values(
        MachOI386PipelineCase{"test_macho_i386.o", "PIC"},
        MachOI386PipelineCase{"test_macho_i386_nopic.o", "NoPIC"}),
    [](const ::testing::TestParamInfo<MachOI386PipelineCase> &Info) {
      return Info.param.TestName;
    });
```

The named parameters keep CTest output actionable and ensure a no-PIC failure cannot be hidden behind a loop that aborts on the PIC case.

- [x] **Step 3: Build and run the two pipeline cases**

Run:

```bash
cmake --build build --target NeverDLiftTests --parallel 4
ctest --test-dir build \
  -R 'MachOI386Pipeline\.CompletesLiftAndDecompilation' \
  --output-on-failure
```

Expected: two tests (`PIC` and `NoPIC`) PASS; neither output contains `unlifted`.

### Task 2: Apply One Generated-C Contract to Both Windows ARM Targets

**Files:**
- Modify: `unittests/lift/COFFARMFormatTests.cpp:457`
- Modify: `unittests/lift/COFFARMFormatTests.cpp:1288-1300`
- Modify: `unittests/lift/COFFARMFormatTests.cpp:1330-1336`

- [x] **Step 1: Centralize the cross-target syntax helper**

Replace the empty `COFFARMPipeline` fixture with a helper that owns the freestanding shim and Clang invocation:

```cpp
class COFFARMPipeline : public NeverDLiftTest {
protected:
  void expectGeneratedCCompiles(const fs::path &CPath,
                                llvm::StringRef TargetTriple) {
    const fs::path IncludeDir = tmpFile("windows-arm-include");
    fs::create_directories(IncludeDir);
    std::ofstream StringHeader(IncludeDir / "string.h");
    StringHeader << "#include <stddef.h>\n"
                    "void *memcpy(void *, const void *, size_t);\n";
    StringHeader.close();
    ASSERT_TRUE(StringHeader.good());

    RunResult Syntax = exec(
        "clang", {"-std=c11", "-target", TargetTriple.str(),
                  "-ffreestanding", "-I", IncludeDir.string(),
                  "-fsyntax-only", CPath.string()});
    EXPECT_EQ(Syntax.exitCode, 0) << Syntax.err;
  }
};
```

- [x] **Step 2: Reuse the helper from both pipeline tests**

Replace the inline ARM32 setup with:

```cpp
expectGeneratedCCompiles(CPath, "thumbv7-pc-windows-msvc");
```

Append the matching AArch64 assertion:

```cpp
expectGeneratedCCompiles(CPath, "aarch64-pc-windows-msvc");
```

- [x] **Step 3: Run both format-level pipelines**

Run:

```bash
cmake --build build --target NeverDLiftTests --parallel 4
ctest --test-dir build \
  -R '^COFFARMPipeline\.(ARM32ThumbLiftAndDecompile|AArch64LiftAndDecompile)$' \
  --output-on-failure
```

Expected: both tests PASS, including target-specific C syntax checking.

### Task 3: Make the Roadmap Match Verified Support

**Files:**
- Modify: `docs/roadmap/README.md:11-24,86-90`
- Modify: `docs/roadmap/README.ar.md:74`
- Modify: `docs/roadmap/README.de.md:74`
- Modify: `docs/roadmap/README.es.md:74`
- Modify: `docs/roadmap/README.fr.md:74`
- Modify: `docs/roadmap/README.it.md:74`
- Modify: `docs/roadmap/README.ja.md:84-88`
- Modify: `docs/roadmap/README.ko.md:74`
- Modify: `docs/roadmap/README.ru.md:74`
- Modify: `docs/roadmap/README.zh-CN.md:90-95`
- Modify: `docs/roadmap/README.zh-TW.md:88`

- [x] **Step 1: Add primary-roadmap acceptance evidence**

After the native-format table, add a status note linking:

```markdown
**Status:** Complete. Format-level coverage is locked by
[`COFFARMFormatTests.cpp`](../../unittests/lift/COFFARMFormatTests.cpp),
[`MachOI386RelocationTests.cpp`](../../unittests/lift/MachOI386RelocationTests.cpp),
and the PE/Mach-O cases in
[`PatchFormatTests.cpp`](../../unittests/lift/PatchFormatTests.cpp). Mach-O i386
uses PIC and no-PIC thin objects because modern macOS hosts cannot link
historical i386 executables.
```

Change the timeline status to `Complete — regression-covered`; remove the sentence that calls native completeness an early implementation while leaving EVM/Solana research status unchanged.

- [x] **Step 2: Synchronize every localized timeline row**

Replace only the native-format status cell with the locale's equivalent of `Complete`; update the Japanese and Simplified Chinese timeline preambles that explicitly describe this item as early implementation. Do not alter EVM, Solana, or hardening statuses.

- [x] **Step 3: Verify documentation consistency**

Run:

```bash
rg -n 'Native format completeness|原生格式补齐|原生格式補齊|ネイティブ形式の完成|네이티브 포맷 완성|Complétude formats natifs|Native Formatvollständigkeit|Completitud formatos nativos|Completezza formati nativi|Завершение нативных форматов|اكتمال الصيغ الأصلية' docs/roadmap/README*.md
rg -n 'early implementation|早期实现|早期實作|早期実装|초기 구현|début d.implémentation|frühe Umsetzung|implementación temprana|ранняя реализация|تنفيذ مبكر' docs/roadmap/README*.md
```

Expected: every native-format timeline row reports completion, and the second command finds no stale native-format status.

### Task 4: Verify the Issue Contract and the Whole Repository

**Files:**
- Test: `unittests/lift/COFFARMFormatTests.cpp`
- Test: `unittests/lift/MachOI386RelocationTests.cpp`
- Test: `unittests/lift/PatchFormatTests.cpp`

- [x] **Step 1: Run all issue-specific format and rewrite tests**

Run:

```bash
ctest --test-dir build \
  -R '^(COFFARMFormat|COFFARMPipeline|PatchCOFF_ARM32|PatchCOFF_AArch64|MachOI386Relocation|ThinObjects/MachOI386Pipeline)\.|^RenamedSectionPatch\.MachO' \
  --output-on-failure --parallel 4
```

Expected: every selected test PASS with no unexpected skips on the configured macOS toolchain, where `clang` and `lld-link` are available.

- [ ] **Step 2: Run the complete lift suite**

Run:

```bash
ctest --test-dir build -L NeverDLiftTests --output-on-failure --parallel 4
```

Expected: 100% PASS.

- [ ] **Step 3: Run the repository aggregate target**

Run:

```bash
cmake --build build --target check-neverd --parallel 4
```

Expected: build and complete test aggregate PASS. If the target is too large for one process window, keep it running and report periodic progress rather than substituting a smaller suite.

- [ ] **Step 4: Inspect the final patch**

Run:

```bash
git diff --check
git status --short
git diff --stat
```

Expected: no whitespace errors; only the plan, two focused test files, and roadmap files are changed.
