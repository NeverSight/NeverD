# LLVM-Style Value-Flow Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace three magic-count SSA closure loops with a finite LLVM-style worklist, remove the related policy duplication, and retain a completely green local and remote validation baseline.

**Architecture:** Add one narrow reachability primitive to `med_calling_conv_detail`: it builds PHI/operation forwarding edges once and computes a monotone closure with `llvm::DenseSet` plus `llvm::SmallVector`. Each calling-convention analysis supplies its own forwarding predicate and classifies consumers only after closure. Keep ABI, truncation, and target-width policy in small named helpers so the two recovery passes and adjacent arithmetic paths cannot drift.

**Tech Stack:** C++20, LLVM ADT/Support libraries, MedIR SSA, GoogleTest, CMake/Ninja, CTest, GitHub Actions, CodeQL.

---

## File Map

- Modify `include/neverd/ir/med/MedCallingConvDetail.h`: declare the internal
  value key/set and finite forward-closure API.
- Modify `lib/ir/med/MedCallingConv.cpp`: implement the closure, migrate
  scratch-only analysis, and index definitions once.
- Modify `lib/ir/med/MedCallingConvX86.cpp`: migrate XMM live-in analysis and
  share the i386 variadic-overflow boundary predicate.
- Modify `lib/ir/med/MedVariadic.cpp`: express direct and advanced i386
  `va_list` flows as two bounded closures.
- Modify `unittests/lift/MedCallingConvTests.cpp`: directly test reverse-order
  reachability, PHI-cycle termination, and non-forwarding boundaries.
- Modify `lib/pipeline/Pipeline.cpp`: centralize open variadic recovery arity.
- Modify `lib/ir/low/JumpTableResolverSource.cpp`: centralize byte-width
  truncation for folded arithmetic.
- Modify `lib/backend/llvm/MedLLVMSwitch.cpp`: assert a supported pointer width
  before narrowing switch indices.
- Modify `lib/backend/llvm/X86/MedLLVMX86ValueEmitter.cpp`: assert a 32/64-bit
  x86 address-register width before constructing an LLVM integer type.
- Review only `unittests/semantic/CMakeLists.txt`: retain the Windows-only link
  as-is unless a concrete defect is found.

## Build and Test Context

Use the existing Release/Ninja source-LLVM tree:

```bash
build-ci-source-fixed
```

Its executables are under `build-ci-source-fixed/bin`. The pre-change full
baseline is 44,883 passing tests plus two documented optional skips. New
GoogleTest cases may increase the final inventory.

### Task 1: Specify the finite closure with failing unit tests

**Files:**
- Modify: `include/neverd/ir/med/MedCallingConvDetail.h`
- Modify: `unittests/lift/MedCallingConvTests.cpp`

- [ ] **Step 1: Add the internal API declarations**

Add the required LLVM and tuple includes, then declare:

```c++
using ValueKey = std::tuple<MedVar::VarKind, int, int>;
using ValueSet = llvm::DenseSet<ValueKey>;

ValueKey valueKey(const MedVar &V);
bool containsValue(const ValueSet &Values, const MedVar &V);

ValueSet computeForwardValueClosure(
    const MedFunc &Func, llvm::ArrayRef<MedVar> Seeds,
    llvm::function_ref<bool(const MedOp &, unsigned)> ForwardsInput);
```

The header must explicitly include `llvm/ADT/ArrayRef.h`,
`llvm/ADT/DenseSet.h`, `llvm/ADT/STLFunctionalExtras.h`, and `<tuple>`.

- [ ] **Step 2: Add three primitive tests**

In `MedCallingConvTests.cpp`, import the new detail functions and add:

```c++
TEST(MedCallingConvValueClosure, ReachesReverseBlockOrderChain) {
  MedVar Seed = temp(1, 0, 4, Arch::X86);
  MedVar Mid = temp(2, 0, 4, Arch::X86);
  MedVar End = temp(3, 0, 4, Arch::X86);
  MedFunc Func;
  Func.Blocks.resize(2);
  Func.Blocks[0].Ops.push_back(unary(NdOp::COPY, End, Mid));
  Func.Blocks[1].Ops.push_back(unary(NdOp::COPY, Mid, Seed));
  auto Values = computeForwardValueClosure(
      Func, Seed, [](const MedOp &Op, unsigned I) {
        return Op.Opcode == NdOp::COPY && I == 0;
      });
  EXPECT_TRUE(containsValue(Values, End));
}
```

Use `llvm::ArrayRef(Seed)` or a one-element local array if the pinned LLVM
constructor requires it. Add a PHI-cycle case (`Seed -> PhiA -> CopyB -> PhiA`)
that reaches both values and returns, and a boundary case where `INT_MULT` is
rejected by the predicate and its output remains unreachable.

- [ ] **Step 3: Build to prove the tests fail before implementation**

Run:

```bash
cmake --build build-ci-source-fixed --target NeverDLiftTests -j8
```

Expected: link failure for the newly declared, undefined closure functions.

### Task 2: Implement the bounded worklist primitive

**Files:**
- Modify: `lib/ir/med/MedCallingConv.cpp`
- Test: `unittests/lift/MedCallingConvTests.cpp`

- [ ] **Step 1: Add LLVM ADT includes**

Add explicit includes for `llvm/ADT/DenseMap.h` and
`llvm/ADT/SmallVector.h`. Retain only standard includes still used after the
migrations.

- [ ] **Step 2: Implement canonical value identity and membership**

Inside `neverd::med_calling_conv_detail` implement:

```c++
ValueKey valueKey(const MedVar &V) { return {V.Kind, V.Id, V.SSAVer}; }

bool containsValue(const ValueSet &Values, const MedVar &V) {
  return !V.isConst() && Values.contains(valueKey(V));
}
```

- [ ] **Step 3: Implement graph construction and closure**

Implement `computeForwardValueClosure` with this structure:

```c++
llvm::DenseMap<ValueKey, llvm::SmallVector<ValueKey, 2>> Successors;
auto AddEdge = [&](const MedVar &From, const MedVar &To) {
  if (!From.isConst() && !To.isConst())
    Successors[valueKey(From)].push_back(valueKey(To));
};

for (const MedBlock &Block : Func.Blocks) {
  for (const PhiNode &Phi : Block.Phis)
    for (const auto &[Pred, Arg] : Phi.Args) {
      (void)Pred;
      AddEdge(Arg, Phi.Output);
    }
  for (const MedOp &Op : Block.Ops)
    for (unsigned I = 0; I < Op.NumInputs; ++I)
      if (!Op.Output.isConst() && ForwardsInput(Op, I))
        AddEdge(Op.Inputs[I], Op.Output);
}

ValueSet Reached;
llvm::SmallVector<ValueKey, 32> Worklist;
for (const MedVar &Seed : Seeds)
  if (!Seed.isConst() && Reached.insert(valueKey(Seed)).second)
    Worklist.push_back(valueKey(Seed));

while (!Worklist.empty()) {
  ValueKey From = Worklist.pop_back_val();
  auto It = Successors.find(From);
  if (It == Successors.end())
    continue;
  for (const ValueKey &To : It->second)
    if (Reached.insert(To).second)
      Worklist.push_back(To);
}
return Reached;
```

- [ ] **Step 4: Build and run primitive tests**

Run:

```bash
cmake --build build-ci-source-fixed --target NeverDLiftTests -j8
build-ci-source-fixed/bin/NeverDLiftTests \
  --gtest_filter='MedCallingConvValueClosure.*:MedCallingConvValueFlow.*'
```

Expected: all selected tests pass and the PHI cycle terminates.

- [ ] **Step 5: Commit the tested primitive**

```bash
git add include/neverd/ir/med/MedCallingConvDetail.h \
  lib/ir/med/MedCallingConv.cpp unittests/lift/MedCallingConvTests.cpp
git commit -m "refactor: add finite MedIR value-flow closure"
```

### Task 3: Migrate all three fixed-count analyses

**Files:**
- Modify: `lib/ir/med/MedCallingConv.cpp`
- Modify: `lib/ir/med/MedCallingConvX86.cpp`
- Modify: `lib/ir/med/MedVariadic.cpp`

- [ ] **Step 1: Migrate `liveInOnlyFeedsScratch`**

Build `llvm::DenseMap<ValueKey, const MedOp *> Definitions` once. Replace the
whole `Changed`/`Guard` loop with `computeForwardValueClosure`, using a predicate
that forwards only the existing same-register `COPY`, unary `INT_ZEXT`/
`INT_SEXT`, and zero-offset `SUBBYTES` forms whose outputs are `Reg` or `Temp`.
After closure, scan consumers once. Preserve the current partial-write and
BSR/BSF exclusions exactly; every other reached consumer returns false.

- [ ] **Step 2: Migrate XMM live-in use detection**

Replace `liveInValueUsed`'s fixed-count loop with a closure through its current
pass-through operations and self-copies. Scan consumers after closure so the
result is independent of block order. Preserve these decisions exactly:

- `x ^ x` and `x - x` discard the value;
- FP-to-non-vector-register copies/casts are genuine uses;
- transparent forwards are not consumers; and
- other operations are genuine uses.

Extract one `isVariadicOverflowOffset` lambda and call it from both aligned and
raw i386 stack-offset helpers.

- [ ] **Step 3: Migrate i386 variadic direct/advanced flow**

Collect valid homed pointer values as seeds. Compute `Direct` using transparent
forwards plus exactly:

```text
INT_ADD(value, constant) input 0
INT_ADD(constant, value) input 1
INT_SUB(value, constant) input 0
```

Collect qualifying arithmetic outputs whose relevant input is in `Direct`.
Compute `Advanced` from those outputs through only PHIs, `COPY`, `INT_ZEXT`,
`INT_SEXT`, and `SUBBYTES(value, 0)`. A load proves the direct walk only when
its address is in `Advanced`.

- [ ] **Step 4: Prove the magic guards are gone**

Run:

```bash
rg -n '100000|10000' lib/ir/med/MedCallingConv.cpp \
  lib/ir/med/MedCallingConvX86.cpp lib/ir/med/MedVariadic.cpp
```

Expected: no matches.

- [ ] **Step 5: Build and run calling-convention regressions**

Run:

```bash
cmake --build build-ci-source-fixed --target NeverDLiftTests NeverDSemanticTests -j8
build-ci-source-fixed/bin/NeverDLiftTests \
  --gtest_filter='MedCallingConvValueClosure.*:MedCallingConvValueFlow.*:TargetRegInfo.*'
build-ci-source-fixed/bin/NeverDSemanticTests \
  --gtest_filter='VariadicAbi/*:OptStress86/X86OptStress86RT.*:OptStress300/X86OptStress300RT.*:BsrPreserveParam/*:BsrParamReg/*'
```

Expected: all selected tests pass, including `x86o86_vargp`,
`x86o86_vargp2`, and `x86o300_vll`.

- [ ] **Step 6: Commit the analysis migrations**

```bash
git add lib/ir/med/MedCallingConv.cpp \
  lib/ir/med/MedCallingConvX86.cpp lib/ir/med/MedVariadic.cpp
git commit -m "refactor: bound calling convention value flow"
```

### Task 4: Remove adjacent drift and assert target invariants

**Files:**
- Modify: `lib/pipeline/Pipeline.cpp`
- Modify: `lib/ir/low/JumpTableResolverSource.cpp`
- Modify: `lib/backend/llvm/MedLLVMSwitch.cpp`
- Modify: `lib/backend/llvm/X86/MedLLVMX86ValueEmitter.cpp`
- Review: `unittests/semantic/CMakeLists.txt`

- [ ] **Step 1: Centralize variadic call-recovery arity**

Add a small file-local helper in `Pipeline.cpp`:

```c++
int callRecoveryTotalArity(const MedFunc &Func, int MaxParamIndex) {
  return Func.IsVariadic ? limits::kMaxCallArgs : MaxParamIndex + 1;
}
```

Use it for both the initial `CalleeTotalArity` map and i386's second-pass
`CTA2` map. Keep the explanatory comment at the policy definition rather than
duplicating it at one call site.

- [ ] **Step 2: Centralize folded-value truncation**

Include `llvm/Support/MathExtras.h` in `JumpTableResolverSource.cpp` and add a
file-local `truncateToByteWidth(uint64_t Value, uint16_t Bytes)` that returns
the input for zero/full widths and otherwise masks with
`llvm::maskTrailingOnes<uint64_t>(Bytes * CHAR_BIT)`. Use it for `SUBBYTES`,
`INT_ADD`, and `INT_SUB` folding.

- [ ] **Step 3: Assert machine-width preconditions**

Add explicit `<cassert>` includes and assertions:

```c++
assert((PtrBits == 32 || PtrBits == 64) &&
       "jump-table switch requires a supported pointer width");
```

and:

```c++
assert((AddrRegBits == 32 || AddrRegBits == 64) &&
       "x86 REP emission requires a 32- or 64-bit target");
```

Place each immediately before the assumption is consumed.

- [ ] **Step 4: Finish the focused review**

Verify the Windows-only `NeverDLibC` link remains inside `if(WIN32)` and has a
documented direct-reference reason. Make no CMake edit if those conditions
hold. Record every code change with Symptom -> Source -> Consequence -> Remedy;
do not add unrelated cleanup.

- [ ] **Step 5: Build and run affected backend families**

Run:

```bash
cmake --build build-ci-source-fixed --target NeverDSemanticTests NeverDSwitchXformTests -j8
build-ci-source-fixed/bin/NeverDSemanticTests \
  --gtest_filter='StringRep/*:RepString/*:StringDir/*:VariadicAbi/*:OptStress86/*:OptStress300/*'
ctest --test-dir build-ci-source-fixed --output-on-failure \
  -L NeverDSwitchXformTests
```

Expected: all selected semantic and switch tests pass.

- [ ] **Step 6: Commit the focused hardening**

```bash
git add lib/pipeline/Pipeline.cpp lib/ir/low/JumpTableResolverSource.cpp \
  lib/backend/llvm/MedLLVMSwitch.cpp \
  lib/backend/llvm/X86/MedLLVMX86ValueEmitter.cpp
git commit -m "refactor: centralize ABI and width invariants"
```

### Task 5: Format, review, and run complete local validation

**Files:**
- Review all files changed since `99c9e5b`

- [ ] **Step 1: Format only touched C++ files**

Run the repository-compatible `clang-format` on the changed `.h`/`.cpp` files.
Inspect the result to ensure formatting did not touch unrelated code.

- [ ] **Step 2: Run static checks**

```bash
git diff --check
git diff --check 99c9e5b..HEAD
rg -n '100000|10000' lib/ir/med/MedCallingConv.cpp \
  lib/ir/med/MedCallingConvX86.cpp lib/ir/med/MedVariadic.cpp
git status --short
```

Expected: no whitespace errors, no magic closure guards, and only intended
changes.

- [ ] **Step 3: Run component and affected semantic suites**

Run the complete lift component executable and all affected semantic filters.
Any failure must be diagnosed from its first semantic divergence before code is
changed.

- [ ] **Step 4: Build default targets**

```bash
cmake --build build-ci-source-fixed -j8
```

Expected: successful default build.

- [ ] **Step 5: Run the complete test inventory**

```bash
ctest --test-dir build-ci-source-fixed -j8 --output-on-failure
```

Expected: every discovered test passes; only the two documented optional
real-binary parity tests may skip. The total may exceed 44,883 because Task 1
adds tests.

- [ ] **Step 6: Perform final Brooks-style maintenance review**

Review `99c9e5b..HEAD` for change propagation, cognitive overload, knowledge
duplication, accidental complexity, dependency disorder, domain distortion,
and test coverage. Fix only findings with a concrete consequence, then rerun
the proportionate tests.

- [ ] **Step 7: Commit any format/review-only corrections**

Commit only if Step 1 or Step 6 changed files; otherwise leave the three
validated commits unchanged.

### Task 6: Push and require remote green

**Files:**
- No additional source changes unless CI exposes a real defect.

- [ ] **Step 1: Verify local repository state**

```bash
git status --short --branch
git log --oneline origin/dev..HEAD
```

Expected: clean worktree and the design plus implementation commits ahead of
`origin/dev`.

- [ ] **Step 2: Push `dev`**

```bash
git push origin dev
```

- [ ] **Step 3: Monitor replacement CI and CodeQL runs**

Require Linux x64, macOS arm64, Windows x64, CodeQL C/C++, CodeQL Python, and
CodeQL Actions to complete successfully. On failure, inspect the exact job log,
reproduce proportionately, fix, commit, push, and monitor the replacement run.

- [ ] **Step 4: Verify security and synchronization**

Query open code-scanning alerts and require zero. Fetch remote state, then
verify `HEAD == origin/dev` and a clean worktree.

- [ ] **Step 5: Disable the CI heartbeat and report**

Delete `monitor-neverd-ci` only after the latest source commit is fully green.
Report commits, test counts, CI URLs, CodeQL status, zero alerts, and repository
synchronization.
