# Remaining x64 C-quality work

This is the backlog, not a claim of completion. Close items with a dump plus
a regression, then delete the row.

## Proven open

| Gap | Surface | Notes |
|---|---|---|
| Live x64 PE vs Hex-Rays | HighC | Cookie is `void` + unnamed `jmp` fail helper treated as noreturn when a sibling path is a bare `return`. Rotate prints `__builtin_rotateleft64`. `__GSHandlerCheck` keeps `return 1` (Const is not void). `__GSHandlerCheckCommon` prints the GS_HANDLER_DATA bit-2 align branch (`test [r8],4` / `and i8 …, 4`). Hex-Rays still wins PDB types/struct fields (non-goal), `capture_previous_context` vs `sub_*`, and `__wind` ctor unwind. Release `--func` of MapleStory2 cookie/GS is 0.14–0.15s (Ghidra already-analyzed UDS 0.23–0.27s) after PE load materializes only the requested `.pdata` body. |
| C++ ctor unwind | HighC | Destructor `__unwind` vs unstructured `__try` on large ctors is still open. Catch funclets now attach into `catch` bodies on `--func`. |
| LLVMC EH is a wrap | LLVMC | Whole-function `__try` + goto, not nested `__try`/`__except` regions. HighC structured regions are the readable target. |
| `--llvm` shard opt quality | pipeline | Default `decompile --llvm` now still emits C if a shard's input fails verifier/EH contracts (opt skipped). The IR is still not a valid opt input; flag/popcount/`*(T*)0` DCE in LLVM remains the real fix. |
| Flag / popcount noise | lift + LLVMC | Corpus dump still materializes PF/AF/OF, `__builtin_popcount`, and `*(T*)0 =` clobbers. Junk on obfuscated x64 will be worse until DCE owns it in LLVM, not the C printer. |
| Extra Win64 params on LLVM route | MedLLVM / LLVMC | HighC compacted `probe_plain_seh` to `int32_t arg0`. LLVMC still showed `arg0..arg7`. GUI can show LLVM C via representation `llvmc` (`neverd_decompile_llvm`); default **C** tab remains HighC. |
| Wrapping casts | HighC | `return (int32_t)(uint32_t)((uint32_t)var + 1)` is required by sanitizer tests. Do not strip. |
| Source names | both | No PDB → `var_m18` / `arg0` / `g_1400050E0` / `sub_1400024E0`, not `Result` / `Value` / `ProbeSink` / `probe_filter`. MSVC `?A@B@@` now prints `B_A` instead of `_x3F_`. Hex-Rays still wins C++ types/`::`. |
| x86 outlined except | HighC | Handler body can sit outside the function range; epilogue may read an adjacent slot instead of the try Result. |

## Closed (do not regress)

| Decision | Test / helper |
|---|---|
| `g_<hex>` unnamed globals, not `dword_` | `makeSyntheticGlobalName`; `HighCPointerAddresses.NamesWritableImageDataStore`; `LLVMCPointerAddresses.NamesWritableNdDataGlobal` |
| `extern` on image objects | HighC `writeImageObjects`; LLVMC `writeGlobals` for `__nd_data_*` |
| Readonly image scalar → immediate | `HighCPointerAddresses.FoldsReadonlyImageIntegerLoad` |
| HighC assigns are declared (`t22_1`, `v36_0`, Phi) | `HighCPointerAddresses.DeclaresAssignedTempsAndUnusedCallResults` |
| LLVMC assigns are declared (including unused non-calls) | `LLVMCPointerAddresses.DeclaresAssignedTempsAndUnusedCallResults` |
| Win64 `rcx,rdx,r8,r9` | HighC param recovery; dump `probe_plain_seh` as one `int32_t` |
| x86 `call [IAT]` is an import | HighC `RaiseException` |
| Unused params compacted only if unused ≥ 4 | `emittedParamIndices` |
| Alloca load/store as named locals + `&` | `COFFExceptionIR.LLVMCAllocaLoadStoreUsesNamedLocalsAndAddressOf` |
| CatchSwitch renders `__except` syntax | `COFFExceptionIR.LLVMCCatchSwitchRendersExceptSyntax` |
| Unnamed x64 pdata with MSVC `mov [rsp+8], ecx` home | `COFFFunctionListingTest.UnnamedPdataAcceptsWin64RegisterHome`; `export --func 0x140001050` |
| Named frame slots stay declared after copy-forward | `NeverDHighCStoreForwardingTests`; FrameSlots emit even if DeadVars |
| LLVMC PHI edge copies, not `/* phi: */` | `LLVMCPointerAddresses.AssignsPhiAtPredecessorEdges` |
| Const `return 1` is not void | `HighCPointerAddresses.ConstantReturnIsNotInferredVoid` |
| Unnamed cookie fail + bare `return` is noreturn/void | `HighCPointerAddresses.UnnamedGsFailureCallOmitsSuccessReturn` |
| `GetCurrentProcess()` is 0-arg | `HighCPointerAddresses.GetCurrentProcessTakesNoArguments` |
| `TerminateProcess` is noreturn | `HighCPointerAddresses.TerminateProcessOmitsSuccessReturn`; `LLVMCPointerAddresses.TerminateProcessOmitsSuccessReturn` |
| Image function exclusive-end code pointer | `LLVMCodePointerInvariantBoundary.UnliftedFunctionExclusiveEndResolvesAsGep` |
| LLVMC IAT load → import call name | `LLVMCPointerAddresses.NamesIATIndirectCall` |
| `--func` hex on a large PE lifts only that entry | `PipelineOptions.OnlyFunctionEntries`; `COFFExceptionIR.OnlyFunctionEntriesSkipsUnrequestedFunctions`; `neverd_decompile` sets the filter before `ensurePipeline` |
| No clobber-0 operand comment | `HighCPointerAddresses.UndefOperandIsNotClobberComment`; Undef prints `0 /* unknown */`; add/sub of 0/Undef folds |
| MSVC `throw` not `CxxThrowException`+`__debugbreak` | `HighCPointerAddresses.CxxThrowCallPrintsThrowWithoutDebugBreak`; `LLVMCPointerAddresses.CxxThrowCallPrintsThrowWithoutDebugTrap`; `LLVMCPointerAddresses.OmitsReturnAfterThrowDespiteJunkAssigns`; `CxxThrowException` is noreturn |
| Empty `catch` filled from funclets | `HighCPointerAddresses.AttachesCxxFuncletBodyIntoCatch`; `attachCxxFuncletBodies`; OnlyFunctionEntries expands HandlerVA |
| GS-like empty-if + join return | `HighCPointerAddresses.EmptyIfCallReturnDoesNotLeaveUninitOrUnknownArgs`; `stmtHiddenFromC`; if/else join → `return call(); return <then>` |
| Win64 call reloads from callee-saves | `HighCPointerAddresses.Win64CallReloadsParamsFromCalleeSaves`; same-block COPYs + reaching defs |
| MedIR Param SSA id → ABI slot | `HighCPointerAddresses.Win64ParamSsaIdMapsToAbiSlot`; `abiParamIndex` |
| Same-reg new-SSA COPY is a call arg | `isNoopRegisterCopy` requires matching SSA; `Win64GsHandlerRestoresParamsAcrossCall` |
| Win64 3-arg call does not take live-in r9 | `Win64ThreeArgCallIgnoresLiveInR9`; tail-call still fills rcx (`Win64ReportGsFailureKeepsCookieArgument`) |
| Cookie PHI/rol/ror is the incoming cookie | `Win64CookiePhiPrefersIncomingParam`; no last-COPY-by-address across blocks |
| Callee name from image symbols | `CallNameUsesImageFunctionSymbol`; `calleeDisplayName` |
| Rotate `(x<<n)|(x>>(w-n))` | `HighCPointerAddresses.RotateOrPrintsBuiltin`; `__builtin_rotateleftN` |
| CRT names keep leading `_` | `HighCPointerAddresses.KeepsLeadingUnderscoreRuntimeNames` |
| PDB data publics name image objects | `HighCPointerAddresses.NamesImageDataFromDebugObject`; Phase A `allDataObjects` |
| Copy-forwarded temps are not declared | `HighCPointerAddresses.OmitsCopyForwardedTempDeclarations` |
| `sbb`/CF idiom is not `0 /* unknown */` | `HighCPointerAddresses.SbbCfIdiomDoesNotPrintUnknown` |
| MSVC `?` decoration is not the only spelling | `HighCPointerAddresses.MsvcDecorationIsNotTheOnlyCalleeSpelling`; `LLVMCPointerAddresses.MsvcDecorationIsNotTheOnlyCalleeSpelling`; `msvcDecorationStem` |
| Empty `if` is not printed | `HighCPointerAddresses.EmptyIfIsNotPrinted` |
| `__security_check_cookie` is one Win64 arg | `HighCPointerAddresses.SecurityCheckCookieKeepsSingleArgument`; `libcArity("security_check_cookie")` |
| Frame address used as a value still declares the slot | `HighCPointerAddresses.FrameAddressValueDeclaresSlot` |
| Unused non-effect load assigns are not declared | `HighCPointerAddresses.UnusedLoadAssignIsNotDeclared`; `analyzeUnusedAssigns` |
| Frame-alias temps do not declare unused `var_mN` | `HighCPointerAddresses.UnusedFrameAliasIsNotDeclared`; PrintedAddrSlots only from `&slot` / slot load-store |
| `int 0x29` prints `__fastfail` | `HighCPointerAddresses.FastFailPrintsIntrinsicWithoutAssign`; `LLVMCPointerAddresses.FastFailPrintsIntrinsicWithoutAssign`; MedLLVM `@__fastfail(i32)` noreturn, not `int $0` with vector 41 as the code |
| `_raise_securityfailure` is noreturn, no success `return` | `HighCPointerAddresses.RaiseSecurityFailureOmitsSuccessReturn`; `LLVMCPointerAddresses.RaiseSecurityFailureOmitsSuccessReturn`; `LibCNoReturn.inc` |
| Used GS TEB/TLS loads print `__readgsqword` | `HighCPointerAddresses.GsTebLoadPrintsReadGsQword`; `LLVMCPointerAddresses.GsTebLoadPrintsReadGsQword`; `x86SegmentedReadIntrinsic`; hide only FS EH registration |
| GS_HANDLER_DATA bit-2 align branch | `HighCPointerAddresses.GsHandlerDataBit2AlignBranchIsPrinted`; MedFlags folds `jz` after `test [r8],4`; Win64 callee-save r10 is not forced to arg0 after a computed def |
| `--func` PE skips padding/data scans | `FunctionDiscoveryAlignment.CoffFuncLoadSkipsPaddingAndDataScans`; full-image still runs `scanDataFuncPointers` (`CoffFullLoadStillScansDataFuncPointers`) |
| `--func` PE skips image-wide `.reloc` and Go pclntab | `COFFFunctionListingTest.LoadOnlyFunctionEntriesSkipsUnrelatedPdataBodies`; `parseBaseRelocations` / `parseGoExceptions` return when `LoadOnlyFunctionEntries` is set |
| Catch-funclet attach does not recurse on cyclic handler VA | `HighCPointerAddresses.AttachCxxFuncletBodiesDoesNotRecurseOnCyclicCatch`; `attachCxxFuncletBodies` keeps an in-flight HandlerVA set |

## Next x64 exe pass

Prefer HighC on reducible MSVC, LLVMC on obfuscated guests. Sequence:

1. Kill HighC caller-saved `0 /* clobbered */` operands and uninitialized success returns on the CRT cookie / GS-handler shapes (public `seh_probe` plus a single `--func` dump, not a full-image guest decompile).
2. Print C++ `throw` and ctor destructor unwind as source-like C; keep corpus `cxx_eh_probe` as the gate, re-dump one guest throw site only to scratch.
3. Use PDB names for callees and image objects when debug info actually loaded.
4. Make default `--llvm` (with opt) complete without shard `input-invalid` on the EH corpus.
5. Kill flag/popcount/`*(T*)0` in LLVM (or mark them analysis-only) before pretty-print.
6. Compact unused Win64 params on the LLVM route the same way HighC does.

Private PE/PDB fixtures are not the contract. Re-dump corpus `probe_plain_seh` after each of those layers.