---
name: neverd-x64-c-quality
description: >
  Continue NeverD x64 PE/COFF C-quality work: HighC structured C, LLVMC
  (llvm-to-C / `neverd decompile --llvm`) for obfuscated executables,
  Windows SEH/C++ EH readability, unnamed globals, local declarations, and
  Win64 ABI. Use when the user asks to improve x64 exe decompiled C, fix
  HighC/LLVMC output, beat IDA on MSVC EH, or work the windows-eh corpus.
  Use when the user runs /neverd-x64-c-quality.
---

# NeverD x64 C quality

Readable C for Windows x64 (and x86) PE is not finished. HighC is closer to
MSVC source on reducible functions. LLVMC is the route for obfuscated x64
because it keeps goto-form LLVM instead of forcing HighIR structure.

Use `neverd-semantic-invariant-debugging` when the bug is a duplicated
semantic decision (ABI, PHI, provenance) rather than C pretty-print.

## Which emitter

| Input | Emitter |
|---|---|
| Reducible MSVC, EH regions, "looks like source" | HighC (`HighCEmitter`) |
| Obfuscated x64, flattened/junk CFG, later optimization | LLVMC (`LLVMCEmitter`, `neverd decompile --llvm`, `UseLlvmRoute`) |

A HighC-only C-quality fix is incomplete if the same class of output exists
on the LLVM route. Match both unless the change is HighIR structuring that
LLVMC cannot express.

Yardstick (not a private guest binary):

- Binaries: `unittests/corpus/corpus/windows-eh/msvc/...`
- Sources: `unittests/corpus/sources/msvc-exceptions/{seh_probe.c,cxx_eh_probe.cpp}`
- First function to dump: x64 `probe_plain_seh` @ `0x140001050`, x86 @ `0x401040`

Do not start a full-image decompile. Do not spawn a second IDA analysis.
Do not commit guest product or game strings.

## Settled C policy

Put the decision in one emitter helper and keep HighC/LLVMC consistent.

**Names**

- Unnamed image data: `g_<hex VA>` (`makeSyntheticGlobalName` in
  `lib/backend/c/CIdentifier.h`). Not IDA `byte_`/`dword_`/`qword_` — C
  already prints the type.
- Image objects are `extern` (they already live in the original image).
- Debug/`DataObjectSym`/symbol-table names win over `g_`.
- `.rdata` scalar loads fold to immediates (`0xE0421001`), not `*(T*)(VA)`.
- Functions without symbols stay `sub_<va>`. An export or image
  function symbol replaces that name when debug info does not
  (`ImageSymbolReplacesSynthesizedFunctionName`).

**Locals**

- Every C identifier that is assigned must be declared.
- HighC: collect `Var` and `Phi`; walk `__except`/`catch` bodies
  (`collectUsedVars` / `collectUsedVarsExpr`). Unused call results print as
  statements (`analyzeUnusedCallResults`); do not declare the dest.
- LLVMC: declare every instruction `writeInstruction` assigns, including
  unused non-calls (obfuscation junk arithmetic). Skip only void/token,
  inlined values, dead frame stores, and calls whose result is omitted.

**Windows EH**

- HighC: structured `SEHTry`/`CxxTry` as `__try`/`__except`/`__finally` and
  C++ `try`/`catch`. Filter as `name(GetExceptionInformation())`.
- LLVMC: goto-form plus recovered EH wraps. A single CatchSwitch uses that
  filter; a sibling CleanupPad+CatchSwitch nest `__try/__finally` inside
  `__except`. CatchSwitch/CatchPad/handler-only blocks print inside the
  matching `__except`/`__finally` (`LLVMCExceptWrapContainsHandlerBody`,
  `CorpusFuncLoadSehProbeLlvmcExceptContainsHandler`). A matched
  `llvm.seh.try.begin` / `try.end` pair per wrap prints the outer
  continuation between `__finally` and `__except`
  (`LLVMCNestedFinallyInsideExceptContainsBothBodies`). Analysis-only C++
  without pads is `try { } /* unwind cleanup */`. HighC structured regions
  remain the readable target.

**ABI / calls**

- Win64 integer/pointer args: `rcx, rdx, r8, r9`, not SysV.
- Compact unused HighC params only when unused count ≥ 4 (do not drop
  `identity(values, 0)`).
- x86 `call dword ptr [IAT]` is an import, not `call eax`.
- Integer wrapping casts stay; `HighCIntegerWidths` needs them.

## How to change it

1. Dump the actual C (`neverd export --format decompile --func 0x...` for
   HighC, add `--llvm` for LLVM-to-C, or `neverd decompile --llvm --func 0x...`)
   and diff against the MSVC source. Do not argue from memory of an earlier dump.
2. Own the rule in one layer (loader/ABI/HighIR/emitter). Do not paper over
   a wrong lift with a C special case.
3. Add a regression next to the owner:
   - HighC: `unittests/lift/core/HighCPointerAddressTests.cpp`
   - LLVMC: same file `LLVMCPointerAddresses.*` and
     `unittests/lift/eh/COFFExceptionIRTests.cpp`
   - EH pipeline: `COFFExceptionIR.*`
4. Run the smallest disproof from `build-release`:

```bash
cmake --build build-release --target NeverDLiftTests NeverDHighCStoreForwardingTests -j
./bin/NeverDLiftTests --gtest_filter='HighCPointerAddresses.*:LLVMCPointerAddresses.*:COFFExceptionIR.*'
./bin/NeverDHighCStoreForwardingTests
```

`NeverDHighCStoreForwardingTests` is 17/17. A later slot that stays in
memory cannot drop a producer whose load was not substituted; accepted
slots nest already-forwarded values so `arg0` remains visible. Do not
treat a green `NeverDLiftTests` filter as the only store-forwarding check.

Code owners: HighC `lib/backend/c/HighC/`, LLVMC `lib/backend/c/LLVMC/`,
shared identifiers `lib/backend/c/CIdentifier.h`.

## Remaining work

Read [remaining-work.md](references/remaining-work.md) before starting an
x64 exe pass. Update that file when a gap is closed or a new one is proven.