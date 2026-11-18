# Where NeverD decides SEH

Paths are relative to the repository root. `dev` moves quickly: grep for the
named function before relying on a line.

## Loader: what the tables say

| Question | Owner |
| --- | --- |
| x64/ARM/AArch64 scope tables, row validation | `lib/loader/COFF/eh/COFFExceptionSEH.cpp`, `COFFExceptionX64.cpp` |
| Which personality a handler RVA is (named, inferred, unknown) | `lib/loader/COFF/eh/COFFExceptionPersonality.cpp` |
| GS cookie records and GS wrappers | `lib/loader/COFF/eh/COFFExceptionGS.cpp` |
| C++ FH3/FH4 tables | `COFFExceptionFH3.cpp`, `COFFExceptionFH4.cpp` |
| x86 registration SEH (SEH4, scope tables, callbacks) | `lib/loader/COFF/eh/COFFRegistrationEH*.cpp`, `COFFSEH4Entry.cpp` |
| Load-configuration runtime slots (CFG check/dispatch) | `lib/loader/COFF/COFFLoaderUtils.cpp` (`RuntimeCallablePointerSlots`) |

A stripped image names a GS wrapper only when a symbol or an inference from
its base handler does; otherwise the record is `personality=sub_...`,
`status=partial`, and one such function keeps its whole emission shard
unoptimized.

## LowIR and MedIR: handlers, frames, targets

| Question | Owner |
| --- | --- |
| A handler's established frame | `lib/ir/low/SEHEstablisherFrame.cpp` |
| Local unwind protocol, per architecture | `lib/ir/low/SEHLocalUnwindProtocol.cpp`, `X86/SEHLocalUnwindX64.cpp`, `AArch64/SEHLocalUnwindAArch64.cpp`, `ARM/SEHLocalUnwindARM.cpp` |
| Which parent blocks a handler's `_local_unwind` resumes | `lib/ir/med/lower/LowToMedSEHLocalUnwind.cpp` sets `MedFunc::SEHHandlerUnwindTargets` on the parent; `MedSSA` keeps them as roots |
| x64 CFG dispatch transfers are transfers through RAX | `lib/ir/low/X86/CFGBuilderX86GuardDispatch.cpp`; forwarders: `forwardsToGuardDispatch` in `lib/pipeline/PipelineLowIR.cpp`; dispatcher calls: `LowToMed.cpp` (`DispatcherTargetRegister`) |

## LLVM emission: lowered, annotated or refused

| Question | Owner |
| --- | --- |
| Classification of every function's EH (lowered, metadata only, refused) | `lib/backend/llvm/eh/MedLLVMExceptionMetadata.cpp` (`isIntentionalMetadataOnlyNativeIR`; the IRLowering classification is used by every route) |
| Native SEH lowering, local unwind placement, callback admission | `lib/backend/llvm/eh/MedLLVMNativeSEH.cpp` (`sehCallbackReadsOnlyArguments`), `WindowsEHNativeSource.cpp` (scope-graph reasons such as `unsupported-seh-scope-graph`) |
| Printed C only: a handler's unwind target spelled parent+offset | `resolveLiftedFunctionInterior` in `lib/backend/llvm/emit/MedLLVMEmitter.cpp` (`AnalysisRecoveryForSource` gate) |
| Printed C only: resume targets no parent edge reaches | `noteUnreachedSEHUnwindTargets` in `MedLLVMEmitter.cpp` |
| A local unwind's handler calls observe the runtime call | `lib/backend/llvm/emit/MedLLVMCall.cpp` |
| Metadata names (attachments, notes, provenance roles) | `include/neverd/backend/llvm/WindowsEHMetadata.h` |
| Module CFG flag (`cfguard` 1/2, `ehcontguard`) | end of function emission in `MedLLVMEmitter.cpp` |

## C writers

| Question | Owner |
| --- | --- |
| LLVM C block form: one `__try` per run of blocks under the same clauses, no jump into a try, `__finally` exit flags | `lib/backend/c/LLVMC/LLVMCFuncWriter.cpp` |
| LLVM C notes (`neverd.seh: not lowered`, unreached resume targets) | `LLVMCFuncWriter.cpp`, reading the metadata above |
| HighC structured SEH, handler inlining, callback registers | `lib/ir/high/lower/MedToHigh.cpp`, `lib/backend/c/HighC/HighCFuncWriter.cpp`, `SEHFuncletRegisters` in `include/neverd/ir/high/HighIR.h` |
| HighC labels and the second exit pass | `collectLabelTargets`, `inlineGotoReturns` in `lib/ir/high/structure/HighCFSimplify.cpp` and the HighC writer |

## Patch (rewrite) path

| Question | Owner |
| --- | --- |
| Source-side authentication of each replaced function's table | `lib/backend/codegen/COFF/COFFExceptionPatch.cpp` |
| Output gate: generated tables, exact frame (`HasExactFrame`), callbacks reading the original frame (`sehCallbackUsesOriginalFrame`), refused provenance roles (`LocalUnwindHandlerCall`) | `COFFExceptionPatch.cpp`, `COFFNativeEHProvenance.h` |
| CFG and EH continuation tables; guard slot symbols (`findCOFFGuardRuntimeSlotVA`) | `prepareCOFFGuardTables` in `COFFExceptionPatch.cpp`; resolvers in `COFFPatch.cpp` (section mode) and `lib/backend/codegen/rewrite/InplaceRewriter.cpp` (in place) |
| In-place binding of replacements and callees | source identity first, then names, in `InplaceRewriter.cpp` |

## Tests next to the owners

- `unittests/lift/eh/`: `TableSEHSourceScopeTests.cpp` (parent+offset
  targets, unreached targets), `SEHSharedTryLayoutTests.cpp` with
  `PrintedTries.h` (try nesting, no jump into a try),
  `SEHDefinitionOrderTests.cpp` (text independent of definition order),
  `COFFExceptionIRTests.cpp`, `COFFExceptionPatchTests.cpp`.
- `unittests/lift/format/PatchFormatCOFFTests.cpp`: whole patch routes,
  including in-place source identity and CFG slot binding.
- `unittests/lift/core/HighCPointerAddressTests.cpp`: guard dispatch calls
  and forwarders as HighC prints them.
- `scripts/check_llvmc_seh_layout_runtime.py` with
  `unittests/lift/eh/fixtures/seh_layout_runtime.c`: IR versus printed C
  under wine64.
- Corpus: `unittests/corpus/corpus/windows-eh/` (384 files; sources in
  `unittests/corpus/sources/msvc-exceptions/`).
