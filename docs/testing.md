**Languages**: [English](testing.md) | [简体中文](zh-CN/testing.md) | [繁體中文](zh-TW/testing.md) | [日本語](ja/testing.md) | [한국어](ko/testing.md) | [Français](fr/testing.md) | [Deutsch](de/testing.md) | [Español](es/testing.md) | [Italiano](it/testing.md) | [Русский](ru/testing.md) | [العربية](ar/testing.md)

[← Documentation Index](README.md)

# Testing NeverD

NeverD's tests cover three different questions: whether a representation has
the expected shape, whether a full pipeline route works for a binary fixture,
and whether generated code preserves behavior. Choose the smallest suite that
answers the question behind a change, then run the broader aggregate before a
high-risk pull request.

## Configure a test build

Tests are disabled unless `BUILD_TESTING` is enabled. A Release build is the
normal choice for the full suite; Debug preserves assertions and stepping but
is intentionally unoptimized and is not representative for decode benchmarks.

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build-release --parallel 4
```

The full fixture set needs `clang` for cross-target compilation and LLVM's
linkers (`ld.lld` and `lld-link`) on `PATH`. CMake builds many relocatable
fixtures unconditionally and linked ELF/PE fixtures when the matching linker is
available. A test skipped because the host cannot compile or link its fixture
is unexecuted coverage, not a pass for that target.

The SBF source differential suite additionally needs `rustc`; it executes both
generated C and generated Rust. Treat a missing compiler skip as missing
backend evidence, not as semantic success.

See [CONTRIBUTING.md](../CONTRIBUTING.md) for clone, build-profile, and macOS
prebuilt-LLVM guidance.

## Modular MBA simplification

`SymReadability.*` covers subtraction and complement spelling, n-ary operator
cost, one-bit and wide literals, shared-tree saturation, budgeted selection,
and exhaustive three-bit equivalence with sampling disabled. `SymMBASample.*`
compares narrow and arbitrary-width verification against the AP evaluator,
including all operators, deterministic assignments, and unused wide inputs.
Candidate quality
comparisons across scoring revisions must recount both outputs with the same
metric; the SDK's version-specific size counters are diagnostic only.

Build the symbolic engine, LLVM safety guards, HighIR bridge tests, and source
roundtrips together when changing shared bitvector simplification:

```sh
cmake --build build-release --target NeverDSymbolicTests NeverDSolverTests \
  NeverDSymSimplifyGuardTests NeverDLiftTests NeverDMBASourceTests \
  NeverDHighCStoreForwardingTests NeverDMetadataJSONTests --parallel 4
build-release/bin/NeverDSymbolicTests
build-release/bin/NeverDSolverTests
build-release/bin/NeverDSymSimplifyGuardTests
build-release/bin/NeverDLiftTests \
  --gtest_filter='HighSymSimplify.*:HighFrameStoreForwarding.*:ELFARM32ModeTest.*'
build-release/bin/NeverDHighCStoreForwardingTests
build-release/bin/NeverDMBASourceTests
build-release/bin/NeverDMetadataJSONTests --gtest_filter='ELFARM32ModeCAPITest.*'
```

`NeverDMBASourceTests` assembles generic 8/16/32/64-bit arithmetic and shared
Boolean identities as ELF, COFF, and Mach-O. It checks both HighC and LLVMC for
remaining MBA, recompiles their C at `-O0` and `-O2` with undefined-behavior
traps, and compares results with unsigned arithmetic, parity, and memory-store
oracles. A full-product counterexample ensures low-word rules do not discard
observable upper bits. The symbolic and HighIR tests additionally check signed
extension, carry boundaries, shared-DAG traversal, and effect preservation.
Missing cross-target Clang is a skip, not evidence for that format.
The x64 ELF, COFF, and Mach-O cases also exercise single-function LLVMC
decompilation. They distinguish default optimization from `--no-opt` and
execute both results at `-O0` and `-O2`. `SessionLLVMTests` verifies that
switching this policy rebuilds the cached module and retains the old module
when a replacement fails verification.
Nested source expressions additionally compile at `-O0` for x86-32/64,
ARM32, Thumb-1/2 and AArch64. Both C routes must remove the MBA, recompile at
`-O0` and `-O2`, and match unsigned arithmetic on byte pairs, edge values and
random words. A pipeline guard checks that exact synthetic-frame accesses can
be promoted while a dynamic store that may alias the frame remains observable.
The 32-bit register-pair fixture separately checks that both C routes preserve
an observed 64-bit return through a call on i386 (including PIC call/pop),
ARM32, Thumb-1, and Thumb-2. Its edge and random-word oracles check the complete
64-bit result and a caller that consumes both halves. Three independent
Boolean forms exercise wide addition and subtraction; two more add a nonzero
64-bit offset after the Boolean form. A three-input parity/majority form checks
carry recovery across the two words, with a matching native 64-bit check on
x86-64 and AArch64. A four-input carry-save form checks the same matrix with
independently randomized operands and the corresponding native 64-bit paths.
An independent five-input carry-save form extends the split-word matrix;
Thumb-2 HighC also checks a frame-pointer alias established after earlier
spills. Native x86-64 and AArch64 repeat the five-input check through both C
routes. The output must remove their residual
XOR, AND, and complement operations, while retaining the shifts and OR needed
to assemble input halves. Thumb-1 compilers may place these offsets in a
read-only literal island inside executable code; mapping and relocation
evidence must authorize constant reads in both C routes. The shared symbolic
candidate generator infers signed coefficients from basis responses and uses
mixed samples only to discard candidates. `NeverDSolverTests` checks that an
exact proof accepts equivalent arithmetic and rejects rare counterexamples or
an incomplete proof.

ARM, Thumb-1 and Thumb-2 Mach-O objects additionally run a complete
two-function five-input case through both C routes. The check includes
function-size recovery, a call to the first function at object address zero,
source MBA elimination, and host-recompiled `-O0`/`-O2` execution. Dedicated
MedIR tests cover exact stack-alignment offsets and reject dynamic, ambiguous,
and stronger-than-ABI masks.
One mixed ARM/Thumb Mach-O object checks mode recovery across direct calls in
both directions. Both C routes remove its arithmetic MBA and execute all five
functions against edge and randomized modular-addition inputs at `-O0`/`-O2`.
Loader tests separately require a Thumb symbol at object address zero to retain
its mode and verify every reachable mixed-mode function entry.
An additional Mach-O object has uncalled ARM and Thumb MBA functions whose
first instructions carry mode-specific relocations, without Thumb nlist mode
flags. Both C routes must recover and execute their simplified arithmetic.
Loader tests check that ARM branch, Thumb branch and halfword relocations record
only their exact instruction modes. Homogeneous ARM-only and Thumb-only objects
must keep a uniform mode when relocation evidence supplies the first entry;
contradictory symbol and relocation modes must fail loading.
Thumb-only ARMv6-M, ARMv7-M, ARMv7E-M, ARMv8-M Base/Main, and ARMv8.1-M Main
Mach-O objects have unmarked MBA functions without instruction relocations.
Both C routes must recover their arithmetic and execute it at `-O0`/`-O2`;
loader tests require the subtype to prove Thumb mode, reject an ARM instruction
relocation in a Thumb-only image, and fail loading on malformed subtype
capability bits.
An A-profile Mach-O object with unmarked ARM and Thumb functions and no
instruction relocations checks explicit function-entry mode assertions. Both
HighC and LLVMC must simplify their MBA and execute correctly after host
recompilation at `-O0` and `-O2`. Loader tests cover object address zero,
alignment, out-of-range entries, and conflict with an exact Thumb symbol.
Without mode evidence, or with only one of the two entries asserted, batch
decompilation must report the ambiguity instead of silently omitting a function.
Single-function decompilation must give the same mode diagnostic.
ELF mapping and Windows ARMNT machine evidence separately accept matching
assertions and reject contradictory ones.
An ELF object stripped of its mapping and function symbols must reject an
unmarked ARM or Thumb entry in both C routes. Exact caller assertions recover
both, and their single-function HighC and LLVMC output must simplify a
carry-save addition and execute at `-O0` and `-O2`.

The frame-spill source matrix also covers x86-32 (ELF/COFF/Mach-O), ARM32
(ARM, Thumb-2 and Cortex-M Thumb-1 ELF), and AArch64 (ELF/COFF/Mach-O)
through both C backends. Repeated private-frame reloads must reduce to
addition/subtraction and execute correctly
for all byte pairs, word-boundary pairs and deterministic random words at both
optimization levels. Clang AST checks inspect the complete spill functions for
residual MBA operators, including temporary assignments, while distinguishing
valid address and pointer expressions. `HighFrameStoreForwarding.*` checks exact
access widths,
source-local mutation, memory-home writes, prefix aliases, partial overlaps,
ordered memory, malformed/cyclic graphs and rendered expansion bounds. Its
narrow-complement regression executes after semantic simplification.
`HighCStoreForwarding.RetainsDefinitionsUsedByForwardedValues` keeps the cached
store-value dependencies live across all four architectures, including floating
reinterpretations and additional direct uses. `SymSimplifyGuard.OpaqueLoad*`
checks load identity/order, volatile/atomic state and poison boundaries.
`ELFARM32ModeTest.*` checks authenticated ARM/Thumb selection, normalized
function addresses, mapping-only objects, mixed-image decoding, cross-mode
calls with modular MBA arithmetic, wide Thumb branches, HighC/LLVMC
execution, forwarded argument chains, halfword-aligned interworking targets,
preserved conditional ARM calls, and rejection of contradictory modes or
malformed call relocations. A sectionless and symbolless linked ELF checks both
directions of direct ARM/Thumb calls, conditional Thumb fallthrough, unknown
unreached bytes, and executable HighC/LLVMC output before and after rewriting.
A mapped ELF checks generated executable bytes outside its section table,
Thumb-to-ARM call repair, and both C backends at `-O0` and `-O2`. An aligned
Thumb stack frame passed to an ARM callee checks live pointer preservation in
both C backends before and after rewriting; a MedIR contract test also checks
multi-hop pointer forwarding and rejects a redefined argument register.
`ELFARM32ModeCAPITest.*` checks that mixed-mode SDK disassembly and both C
backends work after replacing a Thumb image in the same session, then reloads
Thumb to verify decoder recovery. `ARM32InterworkingPatchRT.*` links a generic
mixed ARM/Thumb ELF and executes all four entries in Unicorn before and after
both section and in-place rewriting. `InstructionMode.*` covers the decoder,
code-pointer, direct-branch and code-generation boundaries.
`COFFRelocatableAbsoluteRelocation.MachOARM32*` checks ARM32 Mach-O object
relocations, including Thumb BL, B.W and BLX across sections, halfword-aligned
call sites, backward calls, malformed encodings and jumps that need a veneer.

`HighBoundPrivateFrameCopies.*` in `NeverDHighControlFlowTests` checks copies through nonescaping private frame slots after call ABI binding, including branches, slot reuse and agreement across guard contexts. For x64 and AArch64, the emitted C runs at `-O0` and `-O2` with undefined-behavior traps and independent arithmetic checks. Negative cases preserve the original function for frame escape, unknown call ABIs, missing or inconsistent frame aliases, reassigned entry inputs, overlapping accesses, ordered or atomic memory, malformed statements, cycles and exhausted budgets. Ordinary value conversions must not become PHI copies.

Source projection also revalidates variadic object lists after this cleanup: empty instruction anchors are accepted, while hidden effects or control transfers are rejected. Synchronized cleanup accepts a single `int64_t` or `uint64_t` view of the same saved receiver; narrowing, floating conversions, address arithmetic and reassignment remain rejected. Foundation object sets and normal/exceptional unlock traces run at both `-O0` and `-O2`.

```sh
cmake --build build-release --target NeverDARM32InterworkingTests --parallel 4
build-release/bin/NeverDARM32InterworkingTests
```

## Finite native dispatch

`NeverDJumpTableTests` groups the existing enhanced and proposal fixed-point
regressions with independent AArch64 and x64 finite-selector fixtures. The new
fixtures select slots 2 and 3 from four-slot and 96-slot absolute pointer
tables in both read-only and writable storage. Separate controls retain an
unknown large selector and ensure a feasible slot above the finite query
ceiling cannot be dropped. HighC, optimized LLVMC and `--no-opt` LLVMC
must compile and execute the same selection at `-O0` and `-O2` with undefined
behavior traps. Unknown selector arms, bypassed definitions, clobbered values,
reached backedges, mutated table storage and exhausted evidence must prevent
unsupported source publication. Clang and the existing lift fixture tools
are required; unavailable fixtures remain skips.
An unselected prefix pointer to a separate function must not prevent recovery
of local cases. A foreign target selected immediately or after a reached
backedge must remain outside the local switch. The prefix case also runs
through all three C routes at both optimization levels.

```sh
cmake --build build-release --target NeverDJumpTableTests --parallel 4
build-release/bin/NeverDJumpTableTests
```

`NeverDMachOPointerRelocationBoundaryTests` checks sparse dispatch origins
before the owned runtime slots. Missing maps, fixups or ownership, added filler
slots, unindexed reads, malformed strides, address overflow and exhausted
evidence must retain the ordinary load path.

`NeverDLLVMCValueTests` additionally checks relocated bytes that resemble
strings, generic builtins compiled for a different source ISA, and dead image
address calculations. The latter must retain volatile/atomic loads and
observable calls while leaving the input LLVM module unchanged.

## Interpreter recovery checks

`NeverDBytecodeAnalysisTests` uses independently constructed instruction
languages. It checks malformed encodings, overlapping CFGs, budgets, typed
temporaries, byte order and full-width fields. Source checks run both AArch64
and x64 state carriers through HighC and LLVMC, recompile at `-O0` and `-O2`
with undefined-behavior traps, and verify loops, narrow writes, memory canaries,
signed arithmetic and nested status-propagating calls. The state-forwarding
checks include unaligned banks, overlapping register views, guest aliases and
overwritten writes that a memory access can observe, using both portable byte
copies and the optional Clang/GCC unaligned pointer spelling. CLI checks also exercise
the optional LLVM optimization route and block-sensitive bounded graphs whose
branch arms produce distinct values, including merged conditional targets.
No external dialect or binary sample is needed.

`FloatingConversionsKeepArchitectureResultPolicies` checks float/double to
signed/unsigned 32/64-bit results for AArch64 saturation and x86 indefinite
values through both C routes, both memory spellings and O0/O2 with undefined
behavior traps. It also checks narrow-write canaries on an unaligned bank.

```sh
cmake --build build-release --target NeverDBytecodeAnalysisTests
build-release/bin/NeverDBytecodeAnalysisTests
```

`NeverDBytecodeCAPITests` exercises the pure C ABI with explicit buffer lengths,
unknown rules, truncated code, rejected options and reachable coverage. Both
C routes, including LLVM optimization, execute independently specified narrow
writes on an unaligned state with canaries at O0/O2 under undefined-behavior
traps. `NeverDPluginTests` and `NeverDPythonPluginTests` load the example C and
Python bytecode plugins and invoke the same public recovery entry point.

```sh
cmake --build build-release --target NeverDBytecodeCAPITests
build-release/bin/NeverDBytecodeCAPITests
PYTHONPATH=pluginsdk/python python3 -m unittest discover -s pluginsdk/python/tests -v
PYTHONPATH=pluginsdk/python python3 scripts/check_python_plugin_sdk.py
NEVERD_TEST_LIBNEVERD=/absolute/path/to/libneverd.so PYTHONPATH=pluginsdk/python \
  python3 -m unittest discover -s pluginsdk/python/tests -p 'test_bytecode*.py' -v
```

The last command uses the current platform's shared-library filename. Missing
native libraries skip the integration check explicitly; pure Python ownership
and request tests still run. Embedded Python plugin tests require
`NEVERD_ENABLE_PYTHON_PLUGINS=ON`.

`NeverDAArch64DivisionSemanticTests` checks both instruction decoders against
the architectural results for signed and unsigned division, including zero
divisors, signed overflow, aliased destinations, W-register zero extension and
unchanged condition flags.

```sh
cmake --build build-release --target NeverDAArch64DivisionSemanticTests
build-release/bin/NeverDAArch64DivisionSemanticTests
```

```sh
cmake --build build-release --target NeverDInterpreterSpecializationTests \
  NeverDDevirtualizationSourceTests NeverDInterpreterMachineStateTests --parallel 4
ctest --test-dir build-release -L '^NeverD(InterpreterSpecialization|DevirtualizationSource|InterpreterMachineState)Tests$' \
  --output-on-failure
```

```sh
cmake --build build-release --target NeverDLowIRUndefinedIndependenceTests \
  NeverDOriginalBinaryUndefinedIndependenceTests \
  NeverDX86UndefinedEffectsTests NeverDX86CarryArithmeticFlagTests \
  NeverDX86LogicIdentityTests NeverDX86NoIndexAddressTests --parallel 4
build-release/bin/NeverDLowIRUndefinedIndependenceTests
build-release/bin/NeverDOriginalBinaryUndefinedIndependenceTests \
  --gtest_filter='OriginalBinaryUndefinedIndependence.*'
build-release/bin/NeverDX86UndefinedEffectsTests
build-release/bin/NeverDX86CarryArithmeticFlagTests
build-release/bin/NeverDX86LogicIdentityTests
build-release/bin/NeverDX86NoIndexAddressTests
```

Recovery API tests cover v1/v2/v3 defaults, explicit budgets, truncated structures, every reserved field and ignored future tails. CLI tests exercise field/query exhaustion and successful recovery through both ABIs and both source backends, reject invalid decimal limits and require `--devirtualize`. An exhausted run must publish no source or partial residual.

v4 tests freeze prefix sizes and padding, reject truncated layouts and unknown flags, preserve old/future-tail behavior and retain signed bounds in C even without a report. Independent CLI fixtures require chaining to preserve correlations and bounds to prove an unsigned stack comparison; both C backends execute at O0/O2 with undefined-behavior traps. Disabling discovery must change a discovery-dependent result. Parsing checks zero chaining, integer extrema, overflow, malformed ranges and missing prerequisites. Python checks layout, flags, signatures and owned failure reports.

`NeverDByteMemoryForwardingTests` covers overlapping last writers, both byte orders, byte-multiple widths through i128, defined values, correlated undef/poison snapshots and partial overwrites. Negative cases retain loads across unknown aliases, address-space casts, calls, ordered accesses, lifetime changes, missing bytes, dynamic or invalid offsets, branches and loops. Tests cover high-fan-in PHIs, zero/exact/exhausted budgets and default snapshot refusal. Original and rewritten LLVM execute against an independent arithmetic oracle at O0/O2; existing MBA, LLVMC and interpreter-source suites guard pipeline compatibility.

Numeric-memory cases check full and contained single-writer forwarding, undef/poison without added snapshots, both byte orders, 32/64-bit representations, modular negative offsets, partial overlap, aliases between different roots and allocas, throwing calls, loops, adjacent work/use budgets and store-only analysis invalidation. O0/O2 execution compares original and transformed IR against an independent oracle for both the return value and every byte of an aliased buffer.

`NeverDMedMutableSourceTests` and `NeverDLLVMCValueTests` execute independent loops, reordered blocks, entry backedges, runtime stack arithmetic, earlier reads, branch joins, partial aliases, Boolean truth values and zero-inclusive bit counts at O0/O2. Negative cases reject malformed inputs, truncated targets, ambiguous carriers and exhausted budgets before emission. A CLI fixture above the SSA limit requires executable LLVMC output and explicit HighC refusal. Repeated updates and stored-expression chains across blocks also check C output size and execution.

Additional regressions bound private loads/stores before LLVM promotion and C output size. They execute long mixed arithmetic chains, reordered SSA blocks, overlapping guest writes and zero returns at O0/O2, repeat key checks through the actual LLVM optimization pipeline, and reuse an emitter after rejected module generations.

Compound-condition regressions execute conjunctions and disjunctions with nonzero equality constants, unsigned comparisons, signed comparisons in both operand orders, widened Boolean inputs and every Boolean-negation combination. C emission must preserve the complete truth table at O0/O2 and must not dereference a missing zero-comparison operand. Integer-address stores cover aligned and unaligned 32/64/128-bit carriers; byte backing arrays retain explicit alignment and exact base/partial accesses without scalar array assignments or incompatible typed aliasing.

`NeverDLowIRRefinementTests` checks actual recovered residuals, differently structured finite loops, zero iterations, distinct dynamic producers, guarded witnesses, shared overlapping input views, correlated copies and spills, both-sided immutable-read evidence, mandatory system flags and return-slot preservation. Wrong candidates, extra writes, incomplete or infinite paths, stale evidence, scratch collisions and exhausted shared budgets must refuse a certificate. Existing independence tests continue to reject observable arbitrary values.

`LowIRLoopRefinement.*` and `BinaryLowIRLoopRefinement.*` in the same target exercise arbitrary 64-bit counts, nested lexicographic ranks, actual native residuals, entry-prefix templates, overlapping views and correlated spills. Negative controls reject incorrect bodies, narrowed entry domains, nondecreasing ranks, unsigned wraparound, forgotten prior writes, missing cuts, malformed templates and exhausted shared budgets. Successful finite siblings never authorize an incomplete induction proof.

`LowIRLoopInference.*` and `BinaryLowIRLoopInference.*` use independently authored counters, spills, early returns, native calls and packed flags. Regressions cover narrow arithmetic widening and semantically equal flags with different expressions. Malformed graphs, absent or forged origins, nonterminating/wrapping loops and exhausted inference or proof budgets must never yield a certificate.

Shared-header and shared-latch regressions cover zero-extended 32-bit and full 64-bit counters, non-unit scalar ranks, wrong results, stuttering and wrapping paths, and exact or exhausted budgets across scalar and tuple search. `LowIRLoopInference.SharedHeaderAndLatchNeedLexicographicRanks`. Additional increment/reset regressions require convergence without unfolding one counter bit per round and reject missing progress and unsigned wraparound. Scheduling regressions cover carried non-unit accumulators, wrapping unit noise beside a valid non-unit scalar rank, and three counters whose successful tuple lies beyond the early window. Exact and one-short rank budgets check deterministic continuation without repeated proposals.

`LowIRLoopPlanPairing.*` in the same target checks renamed registers, different arithmetic bodies, side-specific prefix snapshots, retained predicates, shared frame inputs, nested cut coverage and fresh proof budgets. Missing relations, incorrect writes, malformed temporary bindings, incomplete pairings and exhausted metadata limits must not establish a certificate.

`LowIRLoopAlignment.*` checks independently authored ordinary and rotated frame-counter loops: both default self-plans prove individually, their first pairing fails, and another candidate cut proves the relation. Regressions cover multiple-cut permutations, wrong results and frame writes, missing/stale original records, explicit undefined witnesses, nondecreasing/wrapping counters, malformed graphs, cumulative failed-attempt queries, exact total budgets and exhausted search limits. Every refusal must lack a certificate. Additional cases cover separate reset/progress phases, equivalent relocated exit guards requiring cross-family pairing, shared caching without repeated inference, and combined metadata exhaustion. A trailing independent cycle checks complete feedback coverage with an explicit 16384-query inference limit. Empty and duplicate families use no symbolic queries; too few cuts refuse. Exact and one-short global budgets, wrong results, missing progress, original evidence and undefined witnesses remain checked. Filtered-family regressions cover neutral arithmetic diamonds, local versus boundary-only joins, and a reachable join that can be bypassed to an exit or loop boundary. They check a filtered candidate even when the original filtered family is duplicate, reuse of a filtered plan before a later broad attempt, exact/one-short/zero `MaxCutSelectionWork`, cumulative failed `CutSelectionWork`, and no symbolic inference after global graph work is exhausted. Complete cycle coverage is checked for both branch families; the diamond relation uses explicit inference and proof query limits.

Partial-counter regressions cover frame and register lanes, both directions, low/interior/high positions, unusual widths, both byte orders and three-byte represented frame words. They check delayed lane discovery, preserved-bit mutations, nonprogress and unguarded wrap, invalid additional entries, exact/short inference budgets, and existing single-cut search.

Leading-phase regressions cover two and three sequential loops reusing one countdown word, composition with existing nested-loop phases, exact and one-short rank/query budgets, nonprogressing loops and resets back to an earlier phase. Same-width corrupted phase constants, wrong results and frame writes must fail the complete checker without a certificate; missing original evidence remains unsupported.

`InterpreterMachineStateModel.*` in `NeverDLowIRRefinementTests` uses independent LowIR examples to check raw entry flags, status versus guest RAX, all 17 state words, partial register lanes, packed flags, sticky dynamic rejection, guest frame writes, both branch arms and cyclic inference followed by a fresh proof. Wrong outputs, lost status, changed memory, stale instruction records, malformed inputs and exhausted generation budgets must fail. Existing machine-source tests also exercise both C routes at O0/O2; model tests alone do not certify compiled C.

`NeverDLLVMInterpreterModelTests` checks independently written LLVM against full-state LowIR oracles: widths, parallel PHIs, switches, guest memory, separate status, poison guards, intrinsic ranges, rejected contracts and all four construction budgets. It checks a complete arbitrary-word countdown proof and rejects changed status. Independently written C compiled at O1/O2 must match the same observations. These tests validate the admitted model; automatic invariant discovery and compiler correctness remain separate obligations. Variable-shift cases cover all four widths, masked and branch-bounded counts, boundary and oversized counts, no-wrap/exact flags, strict poison rejection and compiled C at O1/O2.

Initialization-contract regressions cover partial and separated byte ranges, fixed aliases, both branch arms, every return, first-iteration loop reads, and stores before loop reads. Read-before-write, missing writes, guest writes, unknown aliases, special memory accesses, out-of-object ranges and exhausted input/work budgets must fail. An independent output-only C fixture compiled at O1/O2 retains the exact LLVM attributes and passes fresh native-to-LLVM composition.

Guarded countdown coverage checks retry after a rejected body template, a complete arbitrary-word header proof, preserved shared cutpoint/query budgets, and immediate refusal of a real entry-contract violation.

`NeverDInterpreterLLVMRefinementTests` checks fresh native-to-LLVM composition, exact text/function binding, independent budgets, full observations and deliberately broader source domains. Changed bytes, residuals, results, flags, status, frame writes, poison and false/stale loop plans must refuse a composite receipt. Arbitrary-word countdowns require both inductive premises; independent C fixtures compiled at O1/O2 exercise actual serialized LLVM input. State-model regressions reject hidden entry backedges and bound roots without copying ancillary provenance.

```sh
cmake --build build-release --target NeverDLLVMInterpreterModelTests --parallel 4
build-release/bin/NeverDLLVMInterpreterModelTests
cmake --build build-release --target NeverDInterpreterLLVMRefinementTests --parallel 4
build-release/bin/NeverDInterpreterLLVMRefinementTests
```

Cached equality exits in two- and three-level loops check correlated operands, moving bounds, counter resets and corrupted copies.

Cached comparison regressions cover equality and inequality, guarded and constant-folded starts, fields first discovered after widening, and bits 7/31/63 in byte/dword/qword caches. Changing only an adjacent bit while preserving the tested bit must fail full state comparison. Zero steps, moving bounds, resets and exhausted shared budgets must refuse.

Generalized-prefix regressions cover joined entry arms, a zero-iteration first witness, hidden register/frame differences, noncanonical predicates, native trap guards, correlated spills and malformed or exhausted plans. Independent two/three-level equality-exit counters and native bytes check unsigned input bounds, zero/max input domains, non-unit increments and incorrect original instructions. Both inference and the final proof must refuse incomplete results.

Independent alternative-loop regressions cover both branch orientations, wrong loop bodies, a nonterminating sibling and exhausted shared search/proof budgets. `LowIRLoopInference.AlternativeLoopsReachBothPrefixesWithinSharedBudgets`.

Nested inference regressions include two and three levels, ascending and descending counters, inferred phase constants and actual native body cutpoints. Unreachable or disjoint prefix domains, incorrect bodies, infinite or wrapping transitions, and exhausted shared search/proof budgets must refuse. Prefix witnesses never replace complete segment coverage.

```sh
cmake --build build-release --target NeverDLowIRRefinementTests --parallel 4
build-release/bin/NeverDLowIRRefinementTests
```

`NeverDLowIRUndefinedIndependenceTests` checks two-execution independence on complete acyclic LowIR graphs. Ordinary entry inputs are shared; fresh architecture-undefined producers retain their correlations through copies, overlapping writes, spills and reloads. Control predicates are checked before path assumptions. Certificates require `Complete` effect metadata bound to each full instruction boundary and exact operation digest. Missing evidence, reachable loops, calls, unknown aliases and exhausted budgets refuse a certificate. The explicit observation and nonfaulting frame contracts limit the result; this is not full native-to-C equivalence.

The following behavior uses the default strict audit contract. `NeverDOriginalBinaryUndefinedIndependenceTests` uses independent fixed-map x64 bytes to test physical native CALL/RET, modified return targets, exhaustive finite indirect targets and immutable loads. The same target checks complete direct-branch collection, exact byte/effect/mapping/read-witness binding, outer-return preservation of entry RSP and its return slot, and image-disjoint frame feasibility. Missing or overlapping instructions, unaudited arms outside the exact trap and explicit profile-projection rules, nonterminating or over-budget loops, incomplete target enumeration, profile/contract mismatches and exhausted budgets must refuse without a certificate or residual code. Success requires every feasible native path to finish. This opt-in gate does not certify loop invariants, exception dispatch, CET-enabled execution or native-to-C equivalence; ordinary recovery remains separate. The target also checks strictly lifted `INT3`/`UD2` terminal boundaries and binding of their complete bytes and operation digests. A `Missing` undefined-output sidecar must remain `Missing`; only symbolically unreachable traps may appear in a certificate, while any feasible trap path must return `ContractViolation` without a certificate or residual code. Trap fallthrough and exception recovery are not modeled, `codeFollowsTrap` is not used, and static LowIR API support remains unchanged.

Explicit native-overlap tests check actual x64 branches into immediate operands, both feasible branch outcomes and indirect return entries inside earlier instructions. Synthetic provider tests check contained overlaps in both collection orders, conflicting bytes on an untaken direct arm, code/read consistency in both orders and candidate reads. Exact and short byte budgets count duplicate overlap bytes across indirect transfers. Changed branch results, static or loop use, and contradictory evidence must refuse certificates; enabling the option or changing its limit changes the digests.

Opt-in audit-boundary tests cover dead RCL, memory XADD and REP MOVS, symbolic path contradictions, arbitrary-controlled branches and exact refusals on entry, indirect, CALL and RET arrivals. They check independent access to a reachable suffix, candidate/native address collisions, malformed or partial evidence, resource exhaustion, static/loop API rejection and all three refinement digest layers. Changing an unreachable instruction or enabling the option with no retained boundaries changes the certificate digests. These tests establish the declared finite proof scope, not semantics for the unaudited instructions.

Packed-flags tests cover all scalar entry-flag combinations, privilege masks, both-execution TF/AC guards, distinct undefined producers, correlated copies, native calls, sibling state, mandatory final system-state observation, malformed evidence and charged resource limits. Finite loops must exhaust every feasible input path; a safe sibling cannot hide an infinite or truncated path. RDSSPD/RDSSPQ checks cover all 16 general-purpose registers and both widths, unchanged high bits, retained `Missing` evidence and rejection of forged projections. Machine-state tests compare both C routes at O0/O2 with undefined-behavior traps against an independent user-mode flags oracle and check sticky profile failure. INCSSPD/INCSSPQ tests cover both widths and every general-purpose register, unreachable-boundary retention, feasible traps after a completed sibling, zero operands and forged trap evidence.

`NeverDX86UndefinedEffectsTests` checks undefined-bit metadata, defined/preserved flags and stale-certificate refusal. `NeverDX86CarryArithmeticFlagTests` checks ADC/SBB auxiliary carry for register and memory forms against an arithmetic oracle. `NeverDX86LogicIdentityTests` checks that AND with identical operands still clears bits 63:32 of the enclosing 64-bit register for a 32-bit destination in 64-bit mode while preserving unwritten bits for narrower writes.

`X86RotateUndefinedEffects.*` covers every raw count, operand width, CL overlap, high-byte alias and memory destination against a scalar arithmetic oracle. `X86BitTestUndefinedEffects.*` covers register/immediate indexes, source/destination overlap, extended registers, defined flags and upper-register writes. Metadata controls reject changed operands, encodings and unsupported forms. Native proofs distinguish correlated reads from independent fresh flags, enforce exact/short producer budgets and reject observable undefined overflow. Full-state refinement checks accept the selected witness and reject zero-bit witnesses or altered candidates.

`X86XaddAudit.*` checks all 65,536 byte operand pairs, wider flag boundaries, register/high-byte overlap, both writebacks, REX byte-width limits and whole-register preservation against an unsigned arithmetic oracle. Native checks require zero new fresh bits without losing earlier dependencies; both witnesses accept unchanged XADD, while altered sum, exchanged source or defined flag outputs refuse. The `/6` alias uses the complete shift-count matrix, with changed group/decoded-ID controls rejecting semantic relabeling.

`NeverDPEFixedImageTests` uses independently constructed PE files to check relocated instructions and immutable data, import write footprints, malformed headers/tables, aliases and changed provenance. Native-to-LowIR and exact LLVM proofs accept matching candidates and reject changed results, status or native bytes. Preparation exhaustion remains distinct and permits an explicit retry with larger limits; ordinary loading also accepts a valid 40000-record relocation table beyond the default analysis budget.

`FrameOffsets.*`, `NativeStackSpecialization.*` and `OriginalBinaryUndefinedIndependence.*` check all residues for alignments 2/4/8/16/32, free high bits, spills across calls, countdown loops, alias corruption, wrong dispatch, irrelevant wide masks, necessary partition upgrades and exact/one-short budgets. Separate native controls check guarded alignment, internal unsigned return cleanup, incorrect cleanup and prefixed returns. These tests do not establish automatic native-to-LLVM proof coverage for partitioned loops.

Register-case regressions cover both byte orders, high frame roots, overwritten and overlapping fields, later edges, widened predecessors, native CALL/RET and exact/one-short budgets. Both C routes execute all four memory cases at O0/O2. Native refinement checks bind two selector values independently; they are not an unrestricted-input proof. A separate LLVM fixture checks that moving the false arm before a shared join cannot make it execute after the true arm, including PHI copies and stores.

Entry-alignment regressions cover finer partitions, every allowed residue, different high root bits, late failing cases and exact/short budgets. Source checks exercise both C backends at O0/O2 with inaccessible rejected guest addresses and invalid flags: status 2 must preserve all state bytes. Model refinement checks the same rejection semantics; C/Python tests cover v5 layout, ownership and old/future tails. Native proof controls reject unbound alignment domains.

`StringTransfer.*` and repeated-copy regressions check overlap, zero count, scratch isolation, capacity/budget limits and pointer invalidation. `MachineStringSourceTests.cpp` compares native execution and both C routes at O0/O2 against independent full-register, flags and stack observations for all four widths and both directions.

`ControlDiscovery.*` and `NativeStackSpecialization.*` cover low-bit root guards, high-bit and whole-root dependencies, incomplete walks, exact and short traversal budgets, and preserved finite immutable-address witnesses.

Unresolved-target guard tests cover an unreachable invalid phase, an unknown target reachable at entry or after a backedge, exact refinement limits, and adjacent successful/exhausted discovery budgets. Refusal publishes no residual code, origins or read witnesses. Optional discovery work after complete recovery is not a required-budget lower bound.

Demanded frame-projection regressions cover automatic and existing register carriers, high roots and modular wraparound, incomplete low-byte guards, conflicting or missing sibling facts, adjacent query-budget outcomes and solver unknown. A narrow demand never certifies only part of a full pointer; refused recovery publishes no residual code or witnesses.

Finite-subword regressions cover selectors in either half, observable arbitrary payloads, register and frame carriers, both byte orders, narrow carriers, later domain expansion, disjoint masks and missing facts. Missing reachable targets, free selectors, exhausted shared query budgets and solver unknown must publish no residual code or witnesses. Automatic discovery with whole-word demand, unused manual hints, root-derived payloads and constant leading windows are covered too.

Public work-budget tests cover default and maximum 32-bit limits, independent evaluation/discovery exhaustion, both source ABIs and C backends, and malformed CLI values. C/Python layout checks cover v7, inherited validation and v1–v6 ignoring future tails. Budget refusal must leave source and recovery witnesses unpublished; node-evaluation charging must not wrap at the maximum counter value.

Optional repeated-destination chain tests compare direct and nested loops under one fixed operation budget, execute the residual loops, distinguish decode modes, and check that zero chaining is unchanged. A correlation counterexample must still succeed by default and refuse publication with the option. Repeated native calls/return slots, dependency replay and late predecessors run with both settings. Public flag tests cover both source ABIs/backends, default and unknown flags, old APIs ignoring the extension, and the reported boolean.

`NativeStackSpecialization.NarrowAddressDemandRetainsCompletePointer` checks guarded pointer joins in registers and frame slots, both byte orders, modular wraparound and high roots, restored stack, and 120 frame bytes. Companion controls reject corrupted pointers and exercise exact/short query, operation, evaluation and refinement budgets, plus discovery and context exhaustion.

Finite-value observer tests cover early refusal, constants, empty projections and the final UNSAT query. Immutable-read regressions keep runtime loads after a refuting witness, revalidate cached address domains for a different read extent, and reject malformed certificates without publishing partial witnesses.

Affine-control tests cover low-bit guards with an eight-query budget, register and frame-slot carriers in both byte orders, modular wraparound, frame-byte observations and restored stack. A nonliteral contradictory guard must prune an unsupported arm; two roots with identical low 32 bits but different high bits must preserve both indirect destinations.

`FiniteQueryCache.*` checks exact and one-short storage bounds, hit-driven recency, replacement of several differently sized records, repeated eviction and renamed queries. Duplicate stores, misses, malformed results and oversized candidates preserve recency. Returned proof copies remain valid after their cache entries are evicted.

`ControlStateRecovery.*Marginal*` exercises independent target and business domains whose product exceeds the joint bound, concrete residual outputs, a later predecessor that adds a target after joint widening, missing reachable targets, and complete domain removal on overflow or incomplete enumeration. These original fixtures verify that marginal changes reschedule analysis and that failures publish no partial graph. A frame-slot variant checks alias invalidation after joint widening in both byte orders.

`InterpreterTransferChain.*` checks correlated values, unique and dynamic branches, later predecessors, frame bounds, alias refusal and budgets. Additional native CALL/RET tests check repeated occurrences, return-slot bytes and restored stack. Producer replay and native-to-LLVM controls cover contract mismatch, changed receipts, incorrect results and missing stack writes.

`NeverDX86NoIndexAddressTests` checks x86 SIB addressing without an index at 32- and 64-bit address widths: ignored scale bits, destination widths, loads/stores, complete undefined-output metadata, segment offsets and address provenance. It rejects pseudo-registers in base or wrong-width index roles and preserves real R12 indices selected by REX.X. EVEX broadcast and masked-move tests also cover these forms, inactive-memory suppression and inconsistent SIB metadata.

Shift regressions cover every eight-bit raw count, zero-count flag combinations, both x86 modes, all scalar widths, CL/destination aliases, AH/CH/DH/BH, extended registers and memory. Byte-accurate symbolic execution is checked against repeated one-bit arithmetic for defined results and exact guard activation. Relational tests check copied versus fresh flags, spills, loop visits, undefined-derived counts, branch refusal, malformed encodings and digest/budget failures. Finite immutable-load tests cover 1/2/4/8-byte values, input-dependent selection, path-specific singleton sets, complete read witnesses, address-limit binding and rejection of dependent, missing, writable, unbacked, relocated or unbounded candidates.

Core tests check context splitting, fixed-point joins, dynamic loops, overlapping registers, alias invalidation, finite dispatch, and refusal without a partial replacement. Source tests assemble original register, stack and finite-address x64 machines, recover both C routes, compile at O0/O2 with undefined-behavior traps, and compare execution with independent unsigned arithmetic and memory oracles. Finite-address fixtures exercise input-selected records and related cursor/key controls; native checks cover SysV and Win64 calling conventions. The suite also exercises the public CLI, recovery budgets and unsupported-input reports. Cross-target Clang and LLD are required; original ELF execution additionally requires an x64 Linux host. Missing tools or a nonmatching host are skipped coverage, not a pass.

`ControlStateRecovery.LongTransparentLoop*` covers an independently authored
20-phase loop, dynamic arithmetic oracles, unknown-selector refusal and budget
exhaustion. `LongTransparentPhasesKeepExactBitDemands` checks that unrelated
bits in the selector's byte remain observable runtime data without becoming
control demands. `ProducerClosureChargesWorkBeforeAnotherRestart` checks that
backward discovery and replay consume the shared budgets before a new graph
starts, with no partial publication.

`VMShapeSourceTests.cpp` adds three original shapes recovered without manual
control hints: direct-threaded pointer bytecode, a bounded software CALL/RET
stack with nested virtual calls, and a loop with rotating opcode-decoder state.
The tests compare native SysV/Win64 execution and both recovered C backends at
O0/O2 against independent mathematical oracles, including returned values,
output stores and canaries. They also assemble ELF and COFF variants. COFF
coverage is assembly-only; the Win64 calling-convention oracle runs as Linux
ELF with `ms_abi`, not as a Windows PE executable. Unknown
virtual return cursors, unconstrained decoder keys and an exhausted recovery
budget must refuse source publication.

`MachineControlSourceTests.cpp` exercises the explicit machine-state ABI with
finite register-indirect native CALL and finite internal RET dispatch. Its
separate native observer compares all 16 general registers, defined RFLAGS and
every byte of the tested guest stack with HighC and LLVMC at O0/O2. Callees read
and overwrite the target register; checks require one actual fallthrough-address
store, preserved flags and restored RSP. Expected code addresses come from ELF
symbols. Unknown targets, finite sets containing a missing or nonexecutable
destination and an unproved target at entry `[rsp]` must fail without source.
These original fixtures have been validated locally on x64 Linux;
their coverage does not establish support for arbitrary virtual machines.

`MachineMemoryCallSourceTests.cpp` adds four zero-hint, default-budget runtime
cases: an immutable RIP-relative pointer slot, an input-selected read-only
two-target table, an initialized guest `[rsp]` slot with two possible values,
and `call *-8(%rsp)` whose target slot is overwritten by the call's push. Native
execution and both recovered C backends at O0/O2 must match the independent
17-word register/flag oracle and every byte of the tested guest stack. The
callee observes the pushed return address; ELF symbols supply expected code
addresses. The oracle also requires a table address left in a register to retain
its original numeric guest value, rather than the address of a generated C
global; the caller supplies any required guest mappings. Unproved external or
writable slots, unknown or alias-clobbered stack values, invalid targets and
default-source-ABI use must refuse publication. Local x64 Linux validation
checks each of the four forms with 1,024 states through native execution, HighC
and LLVMC at O0/O2, comparing all 17 state words and 128 guest-stack bytes.
The lower-level memory-call tests also require canonical unsegmented `r/m64`
forms, optional `addr32` and REX, and rejection of FS/GS, far calls and extra or
noncanonical prefixes. Effective-address calculation and the target LOAD must
precede the native return-address push.

`MachineSourceUsesOriginalWritableMappingsThroughBothRoutes` and the wrapper's
`FixedGuestAddressesRemainNumericThroughLoadsStoresAndState` regression supply
writable guest memory at its original address. Both C backends at O0/O2 must
use that mapping for runtime reads and writes, retain numeric guest addresses
in register outputs and preserve adjacent canaries. Generated global objects
cannot substitute for these mappings. The fixed-mapping runtime coverage is
local to x64 Linux.

The flag-state fixture retains `PUSHFQ` and `POPFQ` across finite indirect
dispatch, then executes recovered HighC and LLVMC at O0/O2. Core negative cases
reject malformed flag intrinsics, snapshots of unbound entry flags, flag-derived
writes without return-slot nonalias evidence, and unbounded flag-derived targets.
They also reject direct reads of unbound flags after algebraic cancellation or
a control-flow join where only one predecessor defines the flag.
Temporary-definition tests require every byte to be written earlier in the
same lifted native instruction, even when an undefined value cancels
algebraically or reuses the previous instruction's temporary offset.

`X86ShiftCarry.*` checks narrow arithmetic-right-shift carry, masked counts,
and APX destination/flag-suppression behavior against repeated one-bit shifts.
`NarrowArithmeticShiftCarrySurvivesBothSourceBackends` executes both recovered C
routes at O0/O2 with undefined-behavior traps, including every byte value and
raw count. `NeverDLLVMCIntrinsicSemanticTests` also executes signed/unsigned
integer min/max for i1/8/16/32/64/128 at O0/O2, checking assigned and inline
results, producer ordering, and single evaluation. Unsupported scalar widths
and malformed operands must fail explicitly.

The same target executes scalar float/double saturating conversions at
O0/O2 with undefined-behavior traps, covering NaNs, infinities, fractional
values and exact signed/unsigned bounds through 128 bits. Bit-reversal checks
include non-byte widths and every one-hot input bit. Unsupported shapes fail
before source emission.

## Structured C control and call checks

`FoldedStoreArmsPublishTheirOutgoingPhiValues` executes a conditional with
side-effecting store arms and a shared PHI at O0/O2. Folding the two arms into
C `if/else` must retain each outgoing SSA assignment.
`RemaindersWithInlineOperandsPublishTheirResult` executes signed and unsigned
remainders with an inline divisor at both optimization levels; an unavailable
composed expression must not become a self-assignment.
`ThreadedSoleSuccessorKeepsItsTransferAndPhi` checks both AArch64 and x64
HighIR when CFG threading leaves a sole successor without a branch operation.
Its edge copies and transfer must run before any unrelated source-order block.

`HighControlFlowSemantics.*` checks that moving loop exits or tails preserves labels reached by other jumps. The entered head/tail exits and break replacement execute generated C at O0/O2 against independent return-value oracles.

`HighCPointerAddresses.Required*` / `UnknownConditionsFailOnlyWhenRead` checks that inferred required register arguments retain unknown trailing slots. Evaluating an unknown required argument or condition must trap explicitly; omitted, null and nested operands must not silently become zero. Known values and proven unread extra operands remain executable. A trap is a diagnostic boundary, not evidence of equivalent recovered behavior.

## CPU execution checks

`NeverDX64FPTests` checks all physical x87 lanes and tags at every TOP, exact
80-bit push/pop and arithmetic state against independent host FXSAVE/FXRSTOR,
FP/SSE reset, register-width rejection and complete CPU-context restoration.
It exercises explicit KVM/WHP/Unicorn transports without ISA preflight for the
state tests; unavailable native hosts are explicit skips. Exact arithmetic in
this suite proves state transport, not admission or all x87 rounding semantics.

`NeverDRAMTransactionTests` checks physical-alias deduplication, independent
cross-page owners, bounded footprints, complete staged reads, write-only RAM,
lease and phase errors, rejected devices/permissions, and cross-thread access.
Injected x64/ARM64 transports modify RAM and CPU before reporting failure,
cancellation or throwing; no speculative state may escape. An injected CPU
exception separately verifies restored RAM and retained architectural status
before OS delivery. These injections test ownership, not native ISA behavior.

Its `X64Atomic` matrix executes original 8/16/32/64-bit `XCHG`/`XADD`/`CMPXCHG`
encodings with and without LOCK at supervisor/user privilege. On an x64 host,
an independent native instruction oracle supplies exact RAM, accumulator,
source-register and flags results, including successful/failed comparisons.
Observers must see original CPU/aliases and staged results; read stops, result
stops, callback exceptions and missing write permissions cannot publish effects.
Unavailable transports and non-x64 oracle hosts are explicit skips. The target
builds with Unicorn disabled; native WHP and ARM64 still require those hosts.

```bash
cmake --build build-cpu --target NeverDRAMTransactionTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDRAMTransactionTests$' --output-on-failure
```

`NeverDIntegerABITests` builds three original Clang fixtures for Windows x64,
Linux x64 and Linux ARM64. The compiled ten-argument functions exercise real
register/stack parameters, local stack storage, return addresses and balanced
returns under checked user execution. The matrix uses Unicorn, KVM and WHP;
unavailable host/ISA pairs are explicit skips. Layout overflow, ABI mismatch,
frame permissions, payload separation, unused registers and the SysV red zone
have independent boundary checks. These are scalar call tests, not evidence of
a complete OS process environment.

`NeverDExecutionBudgetTests` checks shared continuation credit, failed
reservations, `UINT64_MAX`, one absolute deadline and unrepresentable durations
without timing-dependent sleeps. Driver lifecycle regression tests exercise the
same ABI and budget components in real callback, API and SEH flows.

```bash
cmake --build build-cpu --target NeverDIntegerABITests NeverDExecutionBudgetTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(IntegerABI|ExecutionBudget)Tests$' --output-on-failure
```

`NeverDRunControlTests` runs with or without Unicorn and without a hypervisor.
It checks cancellation before host entry, repeated requests until acknowledgement,
stop tokens with distant deadlines, cancellation versus a new entry, and worker
teardown before resource retirement. Injected x64/ARM64 machine entries also
check CPU-to-transport stop propagation and terminal failures retaining elapsed
deadline facts. `NeverDKvmRunTests` intercepts only its own `ioctl` calls to check
entry/retry admission, active-entry cancellation, initialization failure,
resumption and concurrent-entry rejection. `NeverDKvmCancellationTests` uses
a real, non-exiting x64 KVM guest to verify deadline/stop interruption, repeated
resumption, unchanged caller signal state and preservation of application
signals. It skips explicitly without a usable x64 KVM host. Neither suite
replaces native Windows or ARM64 execution.
`ExecutionDeadline.*` also checks unsigned duration overflow, the last
representable clock tick and negative clock epochs. Real software and checked
x64/ARM64 runs reject zero/overflowing budgets before instruction observations
or register changes, and remain usable with a valid budget afterward.

`NeverDCPUEmulationTests` exercises the public C++ CPU interface independently
of the Windows model. It runs ARM64 scalar arithmetic and control flow, signed
and indexed loads, pair/writeback operations, pre-effect observer stops,
CPU-only snapshots, live aliases, code-cache invalidation and bounded loops.
The software profile additionally executes FP/SIMD instructions.
`NeverDThreadPointerTests` checks FS-base address formation on x64 and exact
TPIDR_EL0 reads/writes on ARM64 across checked user transports. It verifies
context restoration, pre-effect read stops, user memory permissions and x64
address-size truncation before adding FS. Only TPIDR_EL0 and FPCR/FPSR system-register encodings are admitted; native ARM64 and WHP coverage requires those hosts.
`ExecutionExitTests.cpp` exercises real x64/ARM64 software and checked runs:
pre-effect stops, deadlines followed by clean resumption, faults overriding stop
requests, retained recoverable faults, guest traps versus unsupported operations,
observer failures and software HLT without a workload-completion claim.
`AArch64ProjectionTests.cpp` executes through the ARM MMU using the actual
native page-table projection, checking translations, remapping and write
protection. These tests deliberately use Unicorn's architectural CPU TLB, not
its virtual TLB.

```bash
cmake -S . -B build-cpu -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON -DNEVERD_ENABLE_CPU_EMULATION=ON \
  -DNEVERD_ENABLE_DRIVER_EMULATION=OFF
cmake --build build-cpu --target NeverDCPUEmulationTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDCPUEmulationTests$' --output-on-failure
```

`NeverDUserExecutionTests` also builds with Unicorn disabled. It validates
CPL3/EL0 checked execution, user stacks, user/supervisor page and alias rights,
permission revocation, recoverable protection faults, CPU contexts and address
space switching. Its `UserProjection` cases deliberately bypass instruction
admission and memory preflight: real machine execution must allow user loads
and reject supervisor memory, non-executable data, read-only stores and
privileged instructions. Warm translations must observe revocation. Missing
transports/host ISAs are reported as skips, never native execution coverage.

```bash
cmake --build build-cpu --target NeverDUserExecutionTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDUserExecutionTests$' --output-on-failure
```

`NeverDServiceRequestTests` also builds without Unicorn. Injected x64/ARM64
machines fail if entered, proving that service interception precedes transport
execution. Real CPU cases retain all register state and memory, distinguish
observer stops/failures and fetch protection, reject other entry mechanisms,
block context rollback and mutation while a request is pending, and resume a
following store only after an explicit service return. Unsupported transports
are skipped; injection-only cases do not claim execution of that store. These
tests establish a handoff protocol, not working OS syscalls or exception vectors.

```bash
cmake --build build-cpu --target NeverDServiceRequestTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDServiceRequestTests$' --output-on-failure
```

`NeverDExecutionConfigurationTests` builds even with Unicorn disabled. It checks
static capabilities against the same resolver used by the factory, rejects
unsupported requirements without changing an attached address space, separates
build support from live probes, and validates configuration JSON and report
round trips. When Unicorn is enabled, real x64 and ARM64 loads validate its
reported MMIO support. `NeverDExecutionCapabilitiesPublicTests` requires the
shared SDK and CLI, but no Windows model or driver fixture; it queries unloaded
sessions, validates bounded input and compares C and CLI reports.

```bash
cmake --build build-cpu --target NeverDExecutionConfigurationTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDExecutionConfigurationTests$' --output-on-failure
# In a shared-SDK/CLI build with CPU or driver emulation enabled:
cmake --build build-release --target NeverDExecutionCapabilitiesPublicTests --parallel 4
ctest --test-dir build-release -L '^NeverDExecutionCapabilitiesPublicTests$' --output-on-failure
python3 scripts/check_python_plugin_sdk.py
```

`MemoryLifecycleTests.cpp` covers independent physical, address-space and CPU
ownership. Its real execution matrix includes software and checked Unicorn on
x64/ARM64, plus the matching native adapter. It checks shared writes, destruction
of one CPU, same-generation space switches, context-space identity, cross-CPU
instruction-cache invalidation, stale projection pins and owner-wide execution
exclusion. Separate cases check mapping transaction failure, allocation reuse,
retained byte views after address reuse and space destruction, whole-span pinned
access validation, partial alias retirement and a terminal page-table-capacity
failure that leaves another CPU and the published address space usable. Native
ARM64 cases remain
skips on an x64 host; software profiles do not run reserved-monitor-range checks.
These tests establish cooperative CPU sharing, not parallel SMP or complete OS
process compatibility. Changes here also require the Windows driver suites,
whose MMIO, backing access, alias and context contracts use the same RAM.

`KernelPhysicalMemoryTests.cpp` also checks pins surviving canonical address reuse
and source-space destruction, different processes using the same VA, shared-region
PFN/cache identity, alias-aware release protection, partial address replacement,
repeated backing pages and reusable PFN lifetime. The DMA test provider uses real
`AddressSpace` storage while retaining validation-hole injection and device-access
counters; a failed cross-page transaction must have no write prefix. These tests
exercise the Windows physical-memory authority, not a complete Windows process
loader or scheduler. `KernelMDLChainTests.cpp` checks that partial-descriptor
retirement and IRP completion revoke only their own aliases while an independent
root MDL remains locked and readable.

The same checked ARM64 cases run against the native adapter on an ARM64 host.
They skip with a typed reason on other hosts or when the hypervisor is
unavailable. Set `NEVERD_REQUIRE_AARCH64_HARDWARE=1` on an ARM64 validation host
to turn that missing coverage into a failure. Run on both Linux ARM64 and
Windows ARM64 before claiming native runtime coverage. Genuine platform
headers permit compile checking on another host, but do not validate vCPU
initialization, debug delivery or cancellation at runtime.

`NEVERD_ENABLE_CPU_EMULATION=ON`,
`NEVERD_ENABLE_DRIVER_EMULATION=OFF`,
`NEVERD_ENABLE_SEMANTIC_TESTS=OFF` and
`NEVERD_EMULATION_BACKEND_UNICORN=OFF` build `NeverDEmulationCPU` and its
`BUILD_TESTING=ON` tests without Unicorn or the Windows driver environment.
This configuration must still link and execute a matching native CPU through the public factory. The current
Unicorn dependency requires LLVM-MinGW rather than MSVC on Windows ARM64;
see [CPU execution](architecture.md#cpu-execution) for build requirements.

On Linux, `NeverDKvmRunTests` injects host-entry interruptions without requiring
`/dev/kvm`. The x64 and ARM64 transports share this cancellation boundary;
`NeverDKvmCancellationTests` additionally validates real x64 host interruption
without enabling guest debug or relying on a guest instruction to exit.

```bash
cmake --build build-cpu --target NeverDKvmRunTests NeverDKvmCancellationTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDKvm(Run|Cancellation)Tests$' --output-on-failure
```

`X64CrossPageTests.cpp` in `NeverDX64MemoryUpdateTests` checks scalar/SSE
loads, stores and arithmetic across every byte split of adjacent RAM pages,
with separate physical owners and aliases. User and supervisor cells verify
per-page rights, precise second-page faults, observer stops, fault consumption
and retry. REP tests preserve completed elements and restart registers; mixed
RAM/device tests reject without callbacks. Linux x64 additionally executes the
same original scalar/SSE instruction bytes and a REP copy natively in child
processes to establish independent store-fault behavior. Other native-oracle
hosts skip explicitly; backend cells distinguish unavailable execution.

## Process emulation checks

`LinuxMemory.*` in `NeverDLinuxProcessTests` checks raw anonymous-memory syscall
rules, partial protection before a hole, page reclamation, transactional budget
failure and program-break preservation. Original x64/ARM64 ELF fixtures verify
those services through actual instructions and repeated process continuations;
the x64 fixture additionally calls rewritten executable memory. Protection
failures use real guest stores. `MemoryLifecycle` also verifies that virtual
layout snapshots expose RAM/device rights without pinning retired allocations.
`NeverDProcessPublicTests` runs the memory fixture through the shared SDK/CLI.

The independent [process emulation suites](process-emulation.md#verification)
compile real x64/AArch64 ELF process fixtures. `NeverDLinuxProcessTests` verifies
startup, program-header policy, service continuation, binary output, guest
faults and resource stops across configured transports. The compiler-emitted
local-exec TLS fixtures initialize two independent blocks from `PT_TLS`, zero
TLS BSS and install their own thread pointers. They check x64 `arch_prctl`
error returns without poisoning execution; malformed templates fail before
CPU entry. Static PIE fixtures require original zero RELA slots before guest
relocation, then call relocated functions and access relocated data at the
actual load bias. Tests reject malformed dynamic tables and external
dependencies independently of section metadata. `NeverDProcessPublicTests`
checks the shared C API/CLI without mutating a loaded analysis image.
`NeverDExecutionSessionTests` covers two CPUs sharing memory and budgets,
exactly-once service/fault consumption and image mapping plans.
`NeverDX64MemoryUpdateTests` checks scalar arithmetic widths/flags, SETcc truth
tables, register BT, XMM/MXCSR transport and restoration, vector-store observers,
REP restart boundaries and device-read preparation/commit failures. It exercises
Unicorn, KVM and WHP separately at supported privileges, with explicit unavailable
backend skips. The public Python wrapper also participates in
the ordinary Python SDK tests and declaration audit.

Checked x64 also admits masked legacy `ADD`, `SUB`, `MUL`, `DIV`, `SQRT`, `MIN` and `MAX` in `SS`, `SD`, `PS` and `PD` forms. `X64SSEInstructions.def` owns operand widths, alignment and admission. `MaskedSSEArithmeticMatchesIndependentHostExecution` compares register and RAM forms against an independent host CPU oracle, including all four rounding modes, FTZ, signed zero, subnormal inputs and NaNs; `SSEMemoryObserverStopsBeforeResultAndStatusChanges` verifies cancellation before effects. This does not admit DAZ, unmasked exceptions, x87 or AVX.

## Driver emulation checks

`NeverDX64ExceptionTests` builds with Unicorn disabled. Its raw native machine
cases bypass decoding and memory preflight to check real divide, invalid-opcode,
alignment, noncanonical-address and page exceptions at CPL0/CPL3. They verify
vectors, error codes, fault addresses, original CPU/RAM state, repeated recovery,
gateway relocation, projection variants and rejection of forged gateway exits.
Its public CPU matrix checks register/RAM `DIV`/`IDIV` at 8/16/32/64 bits,
partial-register results, observer stops, terminal traps and explicit recoverable
continuations. Missing native hosts are skips, including WHP on Linux.
`DriverWDMCPUException` additionally executes the original genuine-WDK SEH
fixture on explicit transports, normal/CFG images and both load addresses;
real zero-divisor and quotient-overflow faults unwind `__finally`, while a
negative filter edits RCX and retries the original division successfully.
These cases require `NEVERD_WDM_SEH_FIXTURE` and its optional CFG companion.

`HardwareBackendTests.cpp` exercises the native checked backend on supported
hosts: high virtual addresses, pre-effect observer stops, RAM aliases, context
restore against current mappings, fault lifetime, bounded loops, unsupported
instructions and driver fixture comparisons. It runs in
`NeverDDriverEmulationTests` and skips explicitly when no native backend is
available. Linux x64 runs exercise KVM; Windows x64 runs exercise WHP. On ARM64 hosts
the x64 checked cases use Unicorn and do not constitute native x64 evidence.
`HardwareBackendPublicTests.cpp` protects backend selection and the unchanged
v1 C ABI. Focus hardware checks with `--gtest_filter='HardwareBackend.*:BackendSelection.*'`.

`DriverBackendParityTests.cpp` reuses the original built-in and optional WDK
images and example scenarios. Each checked transport runs original and relocated
images and compares the complete observable report against legacy Unicorn,
including negative outcomes, request bytes, API results, write events, object
and device state. It excludes backend metadata, diagnostic wording and engine
instruction-count conventions. Fixed-address fixtures without relocation
records must produce the same loader rejection on a rebase; those cases do not
claim guest execution. Missing external images and unavailable native
backends are explicit skips. Select it with `--gtest_filter='*DriverBackendParity*'`.

Enable `NEVERD_ENABLE_DRIVER_EMULATION=ON` together with `BUILD_TESTING=ON`
to build the focused execution suite and the shared C API/CLI checks:

```bash
cmake --build build-release --target \
  NeverDDriverEmulationTests NeverDDriverEmulationPublicTests --parallel 4
ctest --test-dir build-release -L '^NeverDDriverEmulation' --output-on-failure
```

Fixtures exercise guest initialization, returned success and failure,
unsupported behavior, memory faults, strict scenario parsing, bounded execution,
and buffered/direct I/O, READ/WRITE, independent file lifetimes,
MDL permissions, dynamic export resolution, guest varargs, and structured CPU
faults through create, transfers, cleanup, close and unload.
Use the
[`emulate-driver` CLI](driver-emulation.md) to verify JSON and process exit
codes. Production builds may enable this feature with `BUILD_TESTING=OFF`;
test-only Unicorn configuration must not be required by `libneverd`.

Explicit nested user-memory tests cover strict JSON/native graph validation,
shared and cyclic references, unaligned pointer slots, page rights, request and
process revocation, and WDF caller-context ownership. Genuine WDK WDM/KMDF tests
follow two-level structures and interior aliases in normal/active-CFG images at
preferred/rebased addresses; a WDM worker completes through locked aliases after
unmap or requestor exit. C API/CLI and Python run
`docs/examples/driver-nested-user-scenario.json` and inspect `backing_hex` without
mistaking revoked memory for caller-visible output. Backend tests prove terminal
snapshots preserve the original fault and reject MMIO, running CPUs and invalid
spans without destination changes.

`KernelMDLChainTests` and `KernelMDLUserMappingTests` verify partial-MDL
capacity, reuse, physical identity, mapping ownership, process permissions and
failure-atomic retirement. Optional genuine WDK user-view fixtures use
`NEVERD_WDM_USER_MAPPING_FIXTURE` and
`NEVERD_WDM_USER_MAPPING_CFG_FIXTURE`; missing artifacts are explicit skips.
The [user mapping scenario](examples/driver-user-mapping-scenario.json) also
runs through the CLI and Python C API binding. It checks shared bytes, caught
read-only faults, process attachment and mapping ownership. Group-retirement
tests release several relocked views before their request backing and verify
that external pins reject completion without changing IRP or MDL state.

Additional fixtures cover driver-owned nonpaged pool MDLs, independent descriptor
and buffer lifetimes, registry query layouts and short buffers, handle rights,
deletion and leaks, and full-width `information_hex` for zero-output IOCTLs.
External acceptance also covers Zero synchronous direct reads/writes and
statistics queries.

Backend tests verify full CPU contexts (registers, flags, SIMD, FPU and CR8),
shared memory and rejection of foreign or faulted contexts. Compiled
`driver_dispatcher.c` fixtures execute real DPC and worker callbacks, timer
boundaries, notification/synchronization events and timers, nonalertable
`KernelMode` waits with reason `Executive`, timeout/delay, multiple blocked
stacks, set/reset wake latching, callback arguments and invalid
IRQL/lifetimes. Worker tests retain pending/completion, queue, stall and
shared-budget coverage. These cases establish the documented subset, not full
Windows asynchronous support.

`DriverAsyncTests.cpp` submits two pending WDM IOCTLs on independent file
objects with `defer_callback_drain`, then checks that both dispatches precede
either worker and that each IRP completes with its own output. A second batch
cancels one IRP while the other completes and releases each work item and
request independently. C API/CLI tests run both eight-request scenarios.
Synchronous dispatch and same-file overlap are rejected explicitly; arbitrary
thread races are not covered by this deterministic batch boundary. The genuine
WDK neither-I/O fixture also verifies that another requestor can complete a
synchronous request before the first exited requestor's locked-MDL worker runs,
in normal/active-CFG images at preferred and relocated bases.

`DriverKMDFControl.ParallelQueueDeliversTwoPendingRequestsBeforeWorkers`
uses a genuine WDK control driver with an unlimited parallel default queue.
Two IOCTLs on an asynchronous file enter separate work items before either
worker runs; a third request completes synchronously, then both workers
complete their own IRPs. The case runs normal/active-CFG images at preferred
and relocated bases. Model-level queue tests verify parallel overlap and
sequential exclusion; another native case batches two independent synchronous
file objects. The C API test checks the asynchronous-file batch and output bytes.

`DriverKMDFControl.SequentialQueuePresentsWaitingRequestAfterWorker` submits
two IOCTLs to a default sequential queue while the first worker remains
pending. The queue accepts the second request and presents it only after the
first worker completes, in normal/active-CFG images at preferred and relocated
bases. A companion genuine-driver case cancels an undelivered waiter. Model
tests cover FIFO promotion, cancellation before delivery, and framework-only
completion when a handler is absent or a zero-length transfer is disabled. The
C API covers both successful waiting and cancellation.

`DriverKMDFControl.StoppedSequentialQueueResumesWaitingRequestAfterWorker`
stops delivery from a genuine WDK default queue while its first request is
driver-owned. The second request remains queued; after the worker completes
the first, `WdfIoQueueStart` presents the second before returning. The driver
checks queue state bits and queued/delivered counts at each boundary. Normal and
active-CFG images run at preferred and relocated bases. Model tests also cover
paused explicit retrieval, state counts, forwarding to a stopped nondefault
automatic queue, and resuming all waiters on an unlimited parallel queue; a C
API test covers the public sequential scenario.

`DriverKMDFControl.StopCompletionCallbackRunsBeforeQueueRestart` uses a
genuine WDK control driver to verify that a stop-completion callback receives
its context only after the previously delivered request finishes, without
waiting for a second request still queued. The callback runs before the worker
restarts the queue, and the waiting request is then delivered from Start.
Normal and active-CFG images run at preferred and relocated bases. Model tests
also cover immediate notification on an idle queue, forwarding away the final
delivered request, and rejection of a second pending registration; the C API
exercises the callback path.

`DriverKMDFControl.BoundedParallelQueueWaitsForPresentedCompletion` uses
`NumberOfPresentedRequests=1`; the second handler runs only after the first
worker completes. Normal/active-CFG and preferred/rebased images are covered.
Another genuine-driver case cancels a request before presentation. Model tests
exercise limits of one and two, FIFO handoff and queued cancellation; the C API
runs both unlimited and finite modes.

`DriverKMDFControl.NondefaultAutomaticQueuesForwardAndRespectPresentationLimits`
uses genuine WDK sequential and two-presented nondefault queues. Requests
forwarded from a sequential default queue enter the destination callbacks in
FIFO order, while excess requests wait until a worker completes. Normal and
active-CFG images run at preferred and relocated bases; the C API runs both
modes. Model tests cover destination ownership, cancellation before delivery,
completion when no destination callback matches, explicit retrieval from a
sequential queue, bounded parallel presentation and release of a bounded source
queue. A genuine-driver case cancels an automatic waiter before delivery. A
separate model test covers incoming
requests in a manual default queue, FIFO retrieval and queued cancellation.

`DriverKMDFControl.ManualQueueReleasesSequentialSourceAndRetrievesInWorker`
uses a genuine WDK control driver with a nondefault manual queue. Its first
IOCTL is forwarded from a sequential default queue; a second IOCTL on the
same asynchronous file completes before a work item retrieves and completes
the first. Normal/active-CFG images run at preferred and relocated bases. Model
tests cover FIFO retrieval, ownership transfer, invalid forwards and deletion
of a queue with live requests; a C API case checks the public scenario. A
second genuine mode checks queued cancellation at virtual time zero or while
the worker waits, with model cases for request cleanup ordering.
`DriverKMDFControl.ManualRequestRequeueReturnsSameRequestToWorker` exercises
`WdfRequestRequeue` against both WDK images and load bases. Model cases verify
head insertion, ownership transfer, cancelable-request rejection and cleanup
when cancellation precedes requeue; the C API runs the same request flow.

An `asynchronous_file` CREATE case checks that the guest FILE_OBJECT and IRP
omit synchronous flags, two pending IOCTLs on the same file complete with
distinct output, canceling one overlapping IRP leaves the other live, and
CLEANUP cannot overtake an active transfer. A model-level
READ test completes two same-file IRPs out of order without advancing an
implicit current byte offset. Genuine WDK
neither-I/O runs a second same-file transfer before its first worker across
normal/active-CFG and relocated images. The C API/CLI use the same public
schema. These cases do not model completion ports or implicit file position.

`driver_context_limits.c`: API IRQL ceilings come from `KernelAPIIRQL.def`,
with argument-dependent checks in the owning model. DPCs cannot call registry
APIs or allocate, free or access paged pool; Unicode `DbgPrint` conversions
require `PASSIVE_LEVEL`, while supported ANSI output and nonpaged operations
remain usable at `DISPATCH_LEVEL`. Callback stacks have bounded ranges; an
escaping stack pointer cannot enter another blocked worker’s stack. Armed
timers in a device extension prevent premature device retirement. Explicit
IRQL raises/restores use the actual x64 WDK imports, pair on one execution and
update guest CR8; arbitrary transitions outside those pairs remain unsupported.

`KernelDeviceStackTests.cpp` checks independent ownership/attachment graphs, top selection, invalid-operation atomicity, stack capacity, opaque fields, open-handle counts, work-item/request retention across detach/delete and named-file versus dispatch-top identity. The original `driver_wdm_stack.c` uses genuine WDK headers and inline Copy/Skip/SetCompletion helpers; configure optional `NEVERD_WDM_STACK_FIXTURE` / `NEVERD_WDM_STACK_CFG_FIXTURE` paths for normal/active-CFG images. `DriverWDMStackTests.cpp` exercises rebased execution, exact lower status, completion ordering and flags, delayed pending propagation, worker/DPC completion, waits, `STATUS_MORE_PROCESSING_REQUIRED`, direct-MDL retention, nested completion and malformed cursors/control. `DriverScenarioPublicTests.cpp` covers C API/CLI forwarding and C API retained/nested completion, including configured CFG images. Missing artifacts skip visibly. These Linux-only checks establish the same-driver stack subset, not PDO/PnP/power support. `KernelIRPStackTests.cpp` checks counted cursors, full inline Copy prefixes, consumed-slot clearing, status/pending propagation, MPR and nested completion, continuation-owner checks and retained routes. Genuine READ/WRITE and file-lifecycle coverage also uses inline Copy.

`KernelDriverIRPAllocationTests.cpp` checks real packet headers, bounded stack counts, quota rejection, exhaustion, exact-base release and independent caller storage. `KernelDriverIRPOwnershipTests.cpp` checks adoption preflight, immutable lower routes, completion ownership, MPR/free ordering, suspended callback protection and independent kernel output snapshots. `DriverWDMOwnedIRPTests.cpp` executes the genuine WDK `driver_wdm_owned_irp.c` with optional `NEVERD_WDM_OWNED_IRP_FIXTURE` / `NEVERD_WDM_OWNED_IRP_CFG_FIXTURE`: unsent allocation, inline free-before-dispatch-return, worker completion, held MPR, cancellation and nested outer completion, with or without the caller stack slot. Normal/active-CFG images run at preferred/rebased addresses. `DriverScenarioTests.cpp` checks kernel-only input rejection and origin/report parity; `DriverScenarioPublic.CAPIAndCLIReportIndependentDriverAllocatedIRP` verifies actual worker completion and independent rows in both public paths. The [caller-owned IRP scenario](examples/driver-owned-irp-scenario.json) uses that fixture and deliberately cancels one child: CLI exit code 2 / `scenario_success: false` are expected with a returned stop reason, clean unload and zero fixture failures. Missing artifacts skip explicitly; execution evidence remains Linux-only.

`DriverPnpTopologyTests.cpp` checks forward-declared parents, independent roots, strict native/JSON graph validation and stable nullable report fields. `KernelPnpTopologyTests.cpp` checks resolved parent PDO identity, independent device stacks, retained relationship observations after failed AddDevice, powered-parent START admission and explicit bottom-up STOP, SurpriseRemoval and REMOVE ordering.

`DriverPnpScenarioTests.cpp` covers strict JSON/native parity, explicit initial facts, ID/count limits, forbidden field combinations, final bus statuses and nullable observed reports. `KernelPnpDeviceTests.cpp`, `KernelPnpRequestTests.cpp` and `KernelPnpCompletionTests.cpp` cover provider ownership, AddDevice success/failure/leaks, initial IRPs, file admission, lifecycle rollback, delayed completion, MPR/nested/waiting continuations and failure atomicity. The original genuine-WDK `driver_wdm_pnp.c` uses optional `NEVERD_WDM_PNP_FIXTURE` / `NEVERD_WDM_PNP_CFG_FIXTURE` paths. `DriverWDMPnpTests.cpp` exercises normal/active-CFG rebased AddDevice, file I/O, orderly removal, delayed start/removal, failed start/query and clean/leaking AddDevice failure. Missing artifacts skip explicitly. Execution evidence is Linux-only and establishes only the documented resource-free PnP subset. `DriverScenarioPublicTests.cpp` also exercises seven-request delayed PnP reports through the C API and CLI with normal/active-CFG images.

V9 schema tests round-trip all eight minor spellings and share final-status validation with lifecycle completion; QueryStop 0x119 is rejected before image loading. Expanded model and genuine fixture checks cover query-stop rollback, cancel-stop, stop/restart, surprise removal, exact-success failures, software I/O while stopped or remove-pending, guest rejection after surprise removal, device identity and mixed AddDevice outcomes. `DriverScenarioPublicTests.cpp` runs a 16-request stop/restart/surprise sequence through both C API and CLI with normal/active-CFG fixtures, preserving successful software IOCTL bytes, the guest's failed surprise-removal IOCTL and final cleanup/close/remove. Public execution is serial: a held IRP without a currently available producer cannot wait for a later scenario request to start or clean up the device. Remove-drain restrictions are profile boundaries, not a general Windows I/O admission policy. Evidence remains Linux-only.

`KernelRemoveLocksTests.cpp` checks independent lock/device identity, NULL/repeated Tags, exact retail/DBG sizes, immediate and delayed drain, failed-acquire obligations, failure atomicity, capacity and storage retirement. `KernelRemoveLockBridgeTests.cpp` and `DriverWDMRemoveLockTests.cpp` cover pre-attachment initialization, opaque extension storage, IRQL boundaries, release after packet retirement, provider completion after lock drain, final-release readiness before callback return, waiting workers and clean AddDevice failure. The original genuine-WDK `driver_wdm_remove_lock.c` is built in retail/DBG and normal/active-CFG forms through optional `NEVERD_WDM_REMOVE_LOCK_FIXTURE`, `NEVERD_WDM_REMOVE_LOCK_CFG_FIXTURE`, `NEVERD_WDM_REMOVE_LOCK_DBG_FIXTURE` and `NEVERD_WDM_REMOVE_LOCK_DBG_CFG_FIXTURE` paths. Public C API/CLI tests use the existing PnP schema and preserve distinct bus receipt/completion and final teardown observations. Missing artifacts skip explicitly; execution evidence is Linux-only and does not establish full Driver Verifier or general concurrent request draining.

`DriverResourceScenarioTests.cpp` checks explicit JSON/native facts, integer widths, counts, physical/register overlap, alignment, IDs, empty banks and configuration serialization. `KernelMMIOTests.cpp`, `KernelMMIOFailureTests.cpp`, `KernelResourceBridgeTests.cpp` and `UnicornMMIOTests.cpp` cover bank/mapping ownership, aliases, epochs, packed-list lifetime, provider timing, restart persistence, surprise/power accessibility, exact CPU/API transactions and failure atomicity. The original genuine-WDK `driver_wdm_resources.c` uses `NEVERD_WDM_RESOURCE_FIXTURE` / `NEVERD_WDM_RESOURCE_CFG_FIXTURE`; `DriverWDMResourceTests.cpp` executes real scalar and REP accessors, normal/active-CFG rebasing, subrange aliases, page-tail mappings, STOP/restart and invalid accesses. C API/CLI tests reject invalid facts before image loading and execute the same 14-request restart scenario with persistent IOCTL output and exact map/unmap counts. The shared [driver-register-bank-scenario.json](examples/driver-register-bank-scenario.json) needs this fixture's register/IOCTL protocol. Missing artifacts skip explicitly, and evidence is Linux-only; no host physical memory or general device backend is exercised.

`DriverInterruptScenarioTests.cpp` covers explicit raw/translated descriptors, mixed and interrupt-only assignments, strict event fields/counts, source identity and independent BOOLEAN observations. `KernelInterruptsTests.cpp`, `KernelInterruptBridgeTests.cpp` and `SchedulerInterruptTests.cpp` cover exclusive tuple matching, opaque tokens, epoch/connection capture, event lifetime, exact selected Ex fields, shared lock/IRQL restoration, callback ownership, same-time ISR priority and capacity failure before mutation. `KernelFrameworkRequestTests.cpp` checks pure cancellation previews and batch token capacity without publishing calls or consuming references. The original genuine-WDK `driver_wdm_interrupts.c` uses `NEVERD_WDM_INTERRUPT_FIXTURE` / `NEVERD_WDM_INTERRUPT_CFG_FIXTURE`; `DriverWDMInterruptTests.cpp` exercises normal/active-CFG rebasing, the legacy eleven-argument ABI, Ex versions 1/2/4, actual ISR→DPC completion, low-AL FALSE, synchronization/manual locks, independent PDOs, restart epochs and invalid hardware facts. C API/CLI tests reject invalid declarations before image loading and execute the seven-request [driver-interrupt-scenario.json](examples/driver-interrupt-scenario.json), checking the pending IOCTL bytes and separate delivery observations. Missing images skip explicitly; execution evidence remains Linux-only and does not establish instruction-level preemption. Explicit message and passive-ISR coverage is described below. Shared-line tests verify every latched handler runs even after a claim, matching cross-PDO resource tuples, caller-provided nonpaged locks and assigned-DIRQL scheduling distinct from synchronization IRQL. Level tests cover source OR, same-time assert/deassert, repeated sampling without fabricated edges or acknowledgements, delivery limits and source lifetime. Genuine normal/CFG images at both bases execute shared ISR chains and level assertion/repetition/deassertion.

`KernelFrameworkInterruptTests.cpp` checks the WDF interrupt lifecycle, exact
configuration failures, excess unassigned objects, synchronization return width,
resource lifetime, enable-failure rollback and deferred-callback draining before
D0 exit. `KernelInterruptsTests.cpp` checks WDF service arguments on the original
connection, descriptor-local message selection and waitable passive-lock
ownership. `KernelSchedulerTests.cpp` checks coalescing, namespace separation,
retention while suspended and separate passive framework continuations.
The genuine WDK `driver_kmdf_pnp.c` includes `driver_kmdf_interrupt.h`;
`NEVERD_KMDF_PNP_FIXTURE` / `NEVERD_KMDF_PNP_CFG_FIXTURE` cover real line/MSI and
passive callbacks, creation in PrepareHardware, all eleven interrupt table
slots, enable failure and STOP/restart reconnection through
`DriverKMDFPnpTests.cpp`. Normal and active-CFG images run at preferred and
rebased addresses. Missing external images skip explicitly; execution evidence
remains Linux-only. `KernelInterruptActivityTests.cpp` checks inactive connections,
shared-line eligibility, retained wake pulses, power failure and D0 admission.
`KernelFrameworkInterruptTests.cpp` checks report-active/inactive lifecycle and
wake callback ordering, including failed D0Entry cleanup.

`KernelFrameworkLockTests.cpp` covers real thread ownership, waiter references,
IRQL restoration, APC state, relative timeouts and parent deletion. Dispatcher
and interrupt tests cover shared external locks and recursive callback locks.
The original genuine-WDK `driver_kmdf_locks.c` uses optional
`NEVERD_KMDF_LOCK_FIXTURE` / `NEVERD_KMDF_LOCK_CFG_FIXTURE`;
`DriverKMDFLockTests.cpp` executes two-thread contention, zero/relative timeouts,
spin IRQL restoration, scheduled and foreground APC_LEVEL lock waits, and
unload at preferred/rebased addresses with normal
and active-CFG images. Missing images skip explicitly. The PnP fixture also
checks device/queue automatic callback serialization and explicit
`WdfObjectAcquireLock` / `WdfObjectReleaseLock` across real callback waits.

`DriverPowerScenarioTests.cpp` checks D2 native/JSON admission, exact reports and unsupported-state rejection. `KernelPowerRequestTests.cpp` checks exact D2 packet values, independent PoSetPowerState history and IRQL; `DeviceLifecycleTests.cpp` exercises all distinct low-power transition pairs, pure preflight, ticket preservation and the required completed D0 between them. Resource/MMIO/DMA/interrupt tests prove that D2 blocks hardware access while preserving provider assignments and caller-owned common buffers; D3cold tests prove D2 does not reset registers or advance the cold generation.

`DriverDeviceD2Tests.cpp` checks real synchronous/delayed PoRequestPowerIrp FIFO ownership, Query versus notification independence and rejection without FIFO consumption. Its genuine `driver_kmdf_child_wake.c` tests use `NEVERD_KMDF_CHILD_WAKE_FIXTURE` / `NEVERD_KMDF_CHILD_WAKE_CFG_FIXTURE` for D0/D2 callback order/counts, reported power packet states, separate D3 cycles, rejected direct D2→D3 and real D0 preparation before system sleep even without wake settings. The [D2 power scenario](examples/driver-d2-power-scenario.json) and public C API/CLI tests preserve these observations. Framework model tests separately inspect exact callback state arguments and cover explicit S0/Sx D2 settings, reassignment, child wake target selection and PoFx permission; this does not claim genuine WDF-settings coverage for those paths. Normal/active-CFG images run at preferred/rebased addresses; absent artifacts skip explicitly and runtime evidence remains Linux-only.

`KernelPowerCompletionTests.cpp`, `KernelProviderWaitWakeTests.cpp`, `KernelWdmWakeEventTests.cpp`, `DriverWdmWaitWakeTests.cpp`: The [native WAIT_WAKE scenario](examples/driver-wdm-wait-wake-scenario.json) uses the genuine WDK `driver_wdm_wait_wake.c` fixture via `NEVERD_WDM_WAIT_WAKE_FIXTURE` / `NEVERD_WDM_WAIT_WAKE_CFG_FIXTURE`. Tests cover actual START issuance, callback arguments, wake without implicit D0, rearm, cancellation, MPR, DPC cancellation with worker-issued D0, exact event capture and independent providers. Normal/active-CFG and preferred/rebased execution evidence is Linux-only; unavailable artifacts skip explicitly.

`KernelUsbIdleTests.cpp` verifies protocol ownership, exact callback/D2 identity, borrowed storage and sticky completion causes; `KernelUsbIdleBridgeTests.cpp` and `KernelUsbIdleReceiptTests.cpp` exercise actual caller IRPs, malformed admission, queued cancellation, composite capacity and nested receipt/completion timing. `DriverUsbIdleScenarioTests.cpp` checks native/JSON parity and evidence-limited reports. Genuine `driver_wdm_usb_idle.c` uses `NEVERD_WDM_USB_IDLE_FIXTURE` / `NEVERD_WDM_USB_IDLE_CFG_FIXTURE`; `DriverWdmUsbIdleTests.cpp` covers retained idle, cancellation, D0/D3, real remote wake, exact rearm/restart and independent/composite membership, including direct PDO versus FDO forwarding and allocator-stack ownership. `DriverWdmUsbIdlePublicTests.cpp` runs the [USB scenario](examples/driver-wdm-usb-idle-scenario.json) through C API/CLI, normal/active-CFG and preferred/rebased images. Missing artifacts skip explicitly; runtime evidence remains Linux-only. This suite does not establish KMDF USB selective-suspend support.

`KernelFrameworkUsbIdleTests.cpp`, `KernelFrameworkUsbIdleStorageTests.cpp` and `KernelFrameworkUsbIdleBridgeTests.cpp` verify policy, real storage and typed scheduling. Genuine `driver_kmdf_usb_idle.c` never submits its own USB idle packet. `DriverKMDFUsbIdleTests.cpp` covers no permission, delayed D2/D0 with managed I/O, StopIdle before/during the callback, failed arm, explicit Maximum capability, remote wake and composite membership. `DriverKMDFUsbIdlePublicTests.cpp` runs the [KMDF USB scenario](examples/driver-kmdf-usb-idle-scenario.json) through C API/CLI using `NEVERD_KMDF_USB_IDLE_FIXTURE` / `NEVERD_KMDF_USB_IDLE_CFG_FIXTURE`, normal/active-CFG and preferred/rebased. Missing artifacts skip explicitly; execution evidence remains Linux-only. After a successful wake-arm callback, modeled allocation exhaustion executes the real disarm callback and cancels WAIT_WAKE without consuming a D2 response.

`DriverKMDFUsbPoFxTests.cpp` exercises initial SystemManaged/WithHint assignment, independent PoFx/USB grants, cancellation in D0, delayed D2/D0 and a real worker-delayed F0 acknowledgement, activity and StopIdle, wake recovery before READ, failed arm, removal and restart. `DriverKMDFUsbPoFxPublicTests.cpp` runs the [USB PoFx scenario](examples/driver-kmdf-usb-pofx-scenario.json) through C API/CLI with both service modes, normal/active-CFG and preferred/rebased images. The direct READ cases in `DriverKMDFUsbIdleTests.cpp` and its public suite retain the original forwarding regressions and verify D0Entry before delivery without entering the router. `KernelFrameworkRequestTests.cpp` separately checks mapping, caller-context ownership, manual/stopped queues and IRQL. Model USB/PoFx bridge tests provide allocation-failure and independent-gate evidence; the genuine fixture does not exhaust the arena. Missing external fixtures skip explicitly; execution evidence remains Linux-only. `KernelFrameworkUsbPoFxBridge.RemovalPowerUpFailureAcknowledgesRequiredWithoutReleasingIdleWait` separately covers real D0Entry failure during RemovePending: the exact Required acknowledgement and quiescence permit failed-IRP/hardware cleanup without F0/ActiveCondition. It does not establish general present-device SET_POWER failure or autonomous surprise removal.

`KernelPowerCompletionTests.cpp` covers APC/DPC admission, full-capacity atomic retry, provider-only synchronous/delayed completion with and without callbacks, captured-route lifetime and MPR. The genuine `DriverWdmWaitWakeTests.cpp` extension checks direct D0 issuance from a DPC cancellation callback, independent APC/DPC Query/Set, unchanged caller IRQL/CR8, caller return before PASSIVE dispatch/completion and the still-rejected elevated WAIT_WAKE. The [elevated power scenario](examples/driver-wdm-elevated-power-scenario.json) runs via `DriverWdmWaitWakePublicTests.cpp` through C API/CLI, normal/active-CFG and preferred/rebased images; runtime evidence remains Linux-only.

`KernelFrameworkPowerPolicyTests.cpp` checks arm/disarm continuations, nested idle references and real D0 waits. `DriverKMDFPowerPolicyTests.cpp` uses the genuine `driver_kmdf_pnp.c` fixture with `driver_kmdf_power_policy.h`, via `NEVERD_KMDF_PNP_FIXTURE` / `NEVERD_KMDF_PNP_CFG_FIXTURE`, to cover D3hot idle, S0/Sleeping3 wake, retained and canceled WAIT_WAKE IRPs, arm failure, StopIdle waits, queue-triggered D0 and explicit system resume. The [KMDF power-policy scenario](examples/driver-kmdf-power-policy-scenario.json) uses the same optional fixture. `DriverScenarioPublic.CAPIAndCLIExecuteFrameworkIdleWake` runs its policy events and retained WAIT_WAKE observations through the C API and CLI. Missing images skip explicitly; normal/active-CFG and preferred/rebased execution evidence remains Linux-only.

`KernelFrameworkChildWakeTests.cpp` checks the independent own/child arm reasons, recursive opt-in propagation, batch preflight, captured epochs, late-arm rejection, arm failures and exactly-once disarm after the last child cancellation. `KernelFrameworkChildWakeBridgeTests.cpp` verifies actual retained IRPs, failure-atomic batch preflight and nullable causal report fields through the kernel host bridge. `DriverKMDFChildWakeTests.cpp` executes the genuine WDK `driver_kmdf_child_wake.c` through optional `NEVERD_KMDF_CHILD_WAKE_FIXTURE` / `NEVERD_KMDF_CHILD_WAKE_CFG_FIXTURE`. It checks own-only and child-only reasons, real retained WAIT_WAKE completion with causal source fields, disabled siblings, per-hop propagation opt-in, failed child or parent arming, cancellation and STOP/restart generations. The [child-wake scenario](examples/driver-kmdf-child-wake-scenario.json) runs the same fixture via CLI. Missing images skip explicitly; normal/active-CFG and preferred/rebased execution evidence remains Linux-only.

`ExecutionPolicyTests.cpp` checks exact 8-byte GS current-thread reads, including compiler MOV/CMP forms, and rejects neighboring, indexed, partial-width and store forms. Original instructions retain their architectural register and flag effects. `KernelPoFxAPITests.cpp` checks stable current-thread identity through nested callbacks, independent threads, reuse of modeled system-thread objects and opaque-object access rejection. `DriverSessionTests.cpp` exercises the default fixture’s current-thread read, real MOV/CMP flag effects, exact instruction budgets and rejection of ordinary-address access to the private processor view. The genuine PoFx fixture uses the original WDK current-thread helper to verify blocking callback identity.

`KernelPoFxTests.cpp` checks that framework quiescence cancels unissued decisions, retains pending callbacks and preserves driver ownership. `KernelPoFxTests.cpp` and `KernelPoFxAPITests.cpp` also check copied PoFx v1 descriptions, balanced references, explicit Fx/device decisions, hint constraints, callback admission/acknowledgement/return ownership, blocking thread continuations and release failures. `DriverPoFxScenarioTests.cpp` checks strict action fields and JSON/native report parity. The genuine-WDK `driver_wdm_pofx.c` uses optional `NEVERD_WDM_POFX_FIXTURE` / `NEVERD_WDM_POFX_CFG_FIXTURE`; `DriverWDMPoFxTests.cpp` covers synchronous/asynchronous callbacks, delayed acknowledgements and explicit Fx/power decisions at preferred/rebased addresses with normal/active-CFG images. The [PoFx scenario](examples/driver-pofx-scenario.json) uses its F1 command. Missing artifacts skip explicitly; execution evidence remains Linux-only.

`KernelFrameworkPoFxTests.cpp` checks single-component custom settings at `WdfDeviceWdmAssignPowerFrameworkSettings`, copied state, managed-idle prerequisites, one-time admission before first START completion, unsupported directed/PEP fields, Post/Pre callback ordering, callback drain before D0Exit across STOP/restart, failed-Post self-managed I/O suspension and independent physical-D0/component-F0 readiness. `DriverKMDFCustomPoFxTests.cpp` uses the genuine `driver_kmdf_pofx.c` with optional `NEVERD_KMDF_POFX_FIXTURE` / `NEVERD_KMDF_POFX_CFG_FIXTURE`. The [custom KMDF PoFx scenario](examples/driver-kmdf-pofx-scenario.json) uses this fixture. Tests exercise guest Fx callbacks and framework defaults, held requests until F0/D0 recovery, request-driven F0 restoration without another D-state IRP, balanced registration across STOP/restart, and failed-Post cleanup through normal/active-CFG images at preferred/rebased addresses. Missing images skip explicitly; execution evidence remains Linux-only.

`KernelFrameworkPoFxPolicyTests.cpp` checks framework policy integration. `DriverKMDFPoFxTests.cpp` uses the existing genuine KMDF PnP fixtures to distinguish idle/hints from explicit OS power-down permission, verify real D3/D0 IRPs and release/re-register across STOP, restart and REMOVE. `KernelD3ColdResourceTests.cpp` checks dedicated power capability, cold wake, assignment versus power generations, alias restoration and access failure. `DriverD3ColdScenarioTests.cpp` checks strict native/JSON facts. `DriverKMDFD3ColdTests.cpp` and `DriverD3ColdPublicTests.cpp` execute genuine WDF ExcludeD3Cold policy, cold register restoration, D3hot fallback, repeated SET D3 while already cold, and S0/Sx wake through native and public C APIs. DMA unit tests distinguish live transactions/channels from retained idle adapters and common RAM buffers. These fixtures use `NEVERD_KMDF_PNP_FIXTURE` / `NEVERD_KMDF_PNP_CFG_FIXTURE`; unavailable images skip, and the evidence remains Linux-only.

`DriverDMAScenarioTests.cpp` validates explicit capabilities, logical domains, byte/count/time limits, strict event directions and separate configuration/observations. `KernelPhysicalMemoryTests.cpp` and `BackendBackingTests.cpp` check shared-page allocation boundaries, pins, unchanged CPU permissions, MMIO/reentry exclusion and whole-span failure atomicity; `KernelRequestMDLTests.cpp` checks built descriptor aliases against the same physical identities. `KernelDMATests.cpp`, `KernelDMABridgeTests.cpp` and `SchedulerDMATests.cpp` exercise actual RAM bytes, adapter-bound table calls, inline/queued FIFO ownership, separate callback/map lifetimes, page fragments, wrong directions, release preflight, independent PDO domains and epoch/power failures. The original genuine-WDK `driver_wdm_dma.c` uses `NEVERD_WDM_DMA_FIXTURE` / `NEVERD_WDM_DMA_CFG_FIXTURE`; `DriverWDMDMATests.cpp` and C API/CLI coverage execute real adapter pointers, common/SG storage and separately configured DMA/interrupt events. The shared [driver-dma-scenario.json](examples/driver-dma-scenario.json) requires that fixture's protocol. Missing artifacts skip explicitly; execution evidence is Linux-only and does not establish real host DMA, PCI or a general device engine. `pluginsdk/python/tests/test_driver_dma_integration.py` exercises the existing owned JSON binding with `NEVERD_TEST_LIBNEVERD`, `NEVERD_TEST_WDM_DMA_FIXTURE` and `NEVERD_TEST_WDM_DMA_CFG_FIXTURE`, including bytes, callback order and reported failures.

`KernelSEHTests.cpp` checks pure unwind plans, scope order, nonvolatile GPR restoration, bounded stacks and explicit unsupported metadata; `KernelExceptionTests.cpp` checks exact API arity, low-32-bit statuses, typed exceptions, IRQL limits and unchanged model/CPU state. The genuine-WDK `/GS-` `driver_wdm_seh.c` uses optional `NEVERD_WDM_SEH_FIXTURE` / `NEVERD_WDM_SEH_CFG_FIXTURE`; `DriverWDMSEHTests.cpp` runs normal/active-CFG/rebased images through direct and helper raises, nested handlers, rethrows, actual filters and unwind finally callbacks, search ordering, stable guest exception records, valid CPU-fault continuation and full flags/SIMD restoration. Foreground, unrelated worker and attached-worker filters check inherited process identity and probe/MDL authority. Nested filters and collided finally callbacks execute on linked logical stacks; unrelated CPU faults remain explicit negative cases. C API/CLI executes [driver-seh-scenario.json](examples/driver-seh-scenario.json) and verifies null API results with actual guest handler messages. `pluginsdk/python/tests/test_driver_seh_integration.py` uses `NEVERD_TEST_LIBNEVERD`, `NEVERD_TEST_WDM_SEH_FIXTURE` and `NEVERD_TEST_WDM_SEH_CFG_FIXTURE`. Missing external images skip explicitly; evidence remains Linux-only and does not establish general SEH support.

`KernelSEHTests.cpp` checks GS fixed/aligned/negative cookie offsets, frame-pointer
encoding, wrapped-handler flags, search/unwind rechecks after filter or finally
mutation, prologue/epilogue exclusion and malformed metadata without stack reads.
`driver_seh_gs.h` first calls the linked genuine WDK cookie checker with each
original frame, then raises through `__GSHandlerCheck_SEH` or standalone
`__GSHandlerCheck`; normal/CFG images
at both load addresses must handle intact cookies and stop before handlers or
unload for corrupted ones. The C API and Python binding preserve that outcome. Standalone checks
must not invent C scope records. `COFFExceptionGSTests.cpp` verifies exact
standalone identities, truncated aligned-cookie payloads and rejection of
anonymous cookie-shaped metadata; `KernelSEHTests.cpp` verifies search and
unwind-only checks, outer-filter order and malformed standalone payloads.


`BackendBackingTests.cpp` also checks exact alias retirement, surviving derived
aliases and shared backing, same-address reuse with saved CPU contexts, and
whole-batch replacement at the mapping budget. Invalid duplicate, partial,
overlapping or retiring-source replacements preserve every original alias;
running, callback-reentrant and terminal-fault boundaries reject changes.
`KernelPhysicalMemoryTests.cpp` verifies immutable physical cache attributes.
`KernelMDLUserMappingTests.cpp` checks requested-address reuse, independent pins,
per-process view ownership, same-address process switching, wrong-process
unmapping/exit and failure-atomic attachment. The genuine user-mapping fixture
retains two processes' same-address views simultaneously across four requests,
checking distinct physical identities and read/write permissions after each
switch in normal/CFG and preferred/rebased images.

`KernelRequestOwnershipTests.cpp` checks the distinct `METHOD_NEITHER` input/output pointers, zero-length and failing probes, actual write protection, independent user MDL pins and aliases, completion bytes and failure isolation. It also rejects raw user pointers without caller context while permitting a previously locked kernel alias. `BackendFaultTests.cpp` checks in-context recoverable user faults while preserving terminal kernel faults; `BackendBackingTests.cpp` checks that user and kernel aliases share one RAM authority. The original genuine-WDK `driver_wdm_neither.c` uses optional `NEVERD_WDM_NEITHER_FIXTURE` / `NEVERD_WDM_NEITHER_CFG_FIXTURE`; `DriverWDMNeitherTests.cpp` executes six successful paths and four selected page-protection failures in normal/active-CFG images at preferred/rebased addresses. C API/CLI and `pluginsdk/python/tests/test_driver_neither_integration.py` run [driver-neither-scenario.json](examples/driver-neither-scenario.json), verify plain and locked-alias output bytes, and observe guest access violations from configured `no_access` input or `read_only` output. Python takes `NEVERD_TEST_LIBNEVERD`, `NEVERD_TEST_WDM_NEITHER_FIXTURE` and optional `NEVERD_TEST_WDM_NEITHER_CFG_FIXTURE`. Missing images skip explicitly; evidence remains Linux-only and does not establish arbitrary process address spaces.

The same genuine-WDK fixture acquires an executive spin lock through the actual `KeAcquireSpinLock` macro, checks `DISPATCH_LEVEL`, observes a failed try-acquire while held, releases it, and checks the original IRQL. Native normal/active-CFG and preferred/rebased runs, C API/CLI, and Python cover that path. `KernelRequestOwnershipTests.cpp` checks resident storage, exact ownership and release variant, saved IRQL, repeated acquisition, and rejected return/free while held. This single-CPU cooperative model stops on contended blocking acquisition; it does not model cross-CPU progress or in-stack queued locks.

The WDK fixture also initializes a `KSEMAPHORE` at count zero, releases two units, consumes them through two actual zero-timeout waits, confirms a third timeout, and releases up to its limit. `KernelDispatcherTests.cpp` checks count and limit validation, one-unit wait consumption, priority/Wait restrictions, IRQL limits and a typed `STATUS_SEMAPHORE_LIMIT_EXCEEDED` exception without count mutation. Native normal/active-CFG and preferred/rebased, C API/CLI and Python paths exercise the same count transitions.

The WDK fixture's inline `KeRaiseIrqlToDpcLevel`, `KeRaiseIrqlToSynchLevel` and `KeRaiseIrql` use actual `KfRaiseIrql` / `KeLowerIrql` imports. It observes CR8 through `KeGetCurrentIrql` after nested DISPATCH, APC and synchronization-level transitions in normal/active-CFG preferred/rebased images, C API/CLI and Python. Model tests reject lower without a saved raise, wrong LIFO order, cross-execution restoration, a held spin lock and return with an unmatched raise. No instruction-level interrupt preemption is inferred.

The same genuine WDK fixture uses a resident `KMUTEX` through `KeInitializeMutex`, `KeWaitForMutexObject` (the WDK `KeWaitForSingleObject` macro), `KeReadStateMutex` and `KeReleaseMutex`, checking initial signaled state, recursive acquisition, signed previous-state returns and final release. A second mode catches `STATUS_MUTANT_NOT_OWNED` from an unowned release. Native normal/active-CFG preferred/rebased, C API/CLI and Python paths run both modes. Model tests check owner isolation, incorrect IRQL, waiter-frame ownership, a held object's storage and callback-return lifetime, and unsupported `Wait=TRUE` handoff.

The genuine WDK neither fixture also creates a system thread with `PsCreateSystemThread`, references its opaque object with `ObReferenceObjectByHandle`, closes the handle with `ZwClose`, and waits for `PsTerminateSystemThread` to signal it. The thread observes system PID 4, kernel mode and `PASSIVE_LEVEL`; code after termination does not execute. Native normal/active-CFG preferred/rebased, C API/CLI and Python paths run this case. A model test checks that closing the handle does not retire a referenced object, a normal start-routine return is rejected, and waiting/dereferencing retire the object in order. APC delivery and non-system process handles are outside this profile.

The same WDK fixture checks the system thread's initial critical region, then leaves and reenters it; a separate IOCTL nests critical and guarded regions and checks `KeAreApcsDisabled` and `KeAreAllApcsDisabled` before and after each transition and an APC_LEVEL raise. The KMUTEX mode checks implicit normal-APC suppression while held. Native normal/active-CFG preferred/rebased, public C API/CLI and Python cases cover the transitions. Model tests reject unmatched leaves and returning with an open region. These state queries do not establish APC queue or delivery support.

The same genuine-WDK neither fixture also exposes READ/WRITE on a device with neither buffering flag. Native normal/active-CFG and preferred/rebased tests verify actual `IRP.UserBuffer` bytes, no implicit SystemBuffer or MDL, successful caller-context probe/access and catchable read-only/no-access failures. A model test rejects explicit user rights on a buffered READ before guest dispatch. C API/CLI and Python execute the public READ/WRITE scenario and check returned bytes and `configuration.user_page_access` request indices. This does not imply deferred raw pointer access is safe; a worker must use a locked MDL alias.
The independent `driver_direct.c` stream fixture now reads `IRP.UserBuffer` for its neither device; its READ/WRITE test checks byte and count results alongside the genuine-WDK paths.
Additional genuine-WDK pending-worker modes lock both user buffers during dispatch, use only mapped kernel aliases after `STATUS_PENDING`, unlock/free both MDLs, free the work item and complete the IRP. A raw-user-pointer worker stops as `model_error` with a requesting-process diagnostic. Native normal/CFG and preferred/rebased cases plus C API/CLI and Python cover the successful pending route; native and C API/CLI also cover the rejected raw route. WDM cancel cases exercise a scheduled cancellation callback with locked MDLs, completion before a deadline, a wrong saved IRQL, synchronous `IoCancelIrp` with and without a registered routine, and scheduler ordering. Public C/CLI and Python cases exercise the canceled pending IRP and report time. Explicit `user_unmap_after_dispatch` cases revoke the original user VAs before queued work, verify that locked aliases still complete or cancel, that raw pointer access faults, and that successful completion has no caller-visible output buffer after unmapping.
The same genuine WDK worker now compares `IoGetRequestorProcessId` in dispatch and deferred work, and checks that `PsGetCurrentProcessId` changes to the modeled system worker thread. Unit tests cover explicit requestor IDs, cross-process VA protection, process-exit revocation, retained MDL aliases, and rejection of new I/O from an exited identity. Native normal/CFG and rebased, C API/CLI and Python cases verify successful completion or cancellation after the bounded exit event; cleanup/close remain explicit scenario requests rather than inferred handle rundown.

`KernelRequestOwnershipTests.cpp` also checks process-object identity, opaque storage, attachment-only user VA access across two nested process contexts, exact APC-state pairing, return and wait rejection while attached, and restoration to the system process. The genuine WDK neither fixture executes `IoGetRequestorProcess`, `KeStackAttachProcess`, `IoGetCurrentProcess`, `PsGetProcessId` and `KeUnstackDetachProcess` in a work item through normal/active-CFG and preferred/rebased images, including inaccessible pages and exited requestors. Public C API/CLI and Python scenarios verify the returned bytes. `PsGetCurrentProcessId` continues to identify the work item's creator, PID 4, while the attached process is read from its APC state.

`KernelDMAChannelTests.cpp`, `KernelDMAChannelBridgeTests.cpp` and the shared `SchedulerDMATests.cpp` check mixed allocation FIFO, callback return widths, pure admission/release checks, register reuse, contiguous page fragments, whole-operation flush, CurrentIrp snapshots and packet/MDL/device lifetimes. The original genuine-WDK `driver_wdm_dma_channel.c` uses optional `NEVERD_WDM_DMA_CHANNEL_FIXTURE` / `NEVERD_WDM_DMA_CHANNEL_CFG_FIXTURE`; `DriverWDMDMAChannelTests.cpp` executes normal/active-CFG/rebased drivers with actual MapTransfer and FlushAdapterBuffers calls, shared common/SG/channel quota, explicit device transactions, IRQ/DPC completion, sequential operations, two PDOs and failure cases. C API/CLI runs the seven-request [driver-dma-channel-scenario.json](examples/driver-dma-channel-scenario.json), including a single transaction spanning both mapped fragments. `pluginsdk/python/tests/test_driver_dma_channel_integration.py` uses `NEVERD_TEST_LIBNEVERD`, `NEVERD_TEST_WDM_DMA_CHANNEL_FIXTURE` and `NEVERD_TEST_WDM_DMA_CHANNEL_CFG_FIXTURE` for the same public JSON interface. Missing artifacts skip explicitly; Linux evidence does not establish system DMA controllers or arbitrary HAL mapping/flush patterns.

`DriverPowerScenarioTests.cpp` verifies strict power packet facts, JSON/native parity, opaque 32-bit context, response FIFO limits and independent child reports. `KernelPowerRequestTests.cpp` and `KernelPowerCompletionTests.cpp` check real packet layout, route flags, lifecycle versus per-object notification, FIFO matching, terminal callback ownership, MPR, waits and release boundaries. The original genuine-WDK `driver_wdm_power.c` uses optional `NEVERD_WDM_POWER_FIXTURE` / `NEVERD_WDM_POWER_CFG_FIXTURE` paths. `DriverWDMPowerTests.cpp` covers normal/active-CFG rebasing, direct and nested Query/Set, delayed independent completion, S0-before-D0 ordering, five-argument callback snapshots across waits, worker-originated children, null callbacks, query rejection, independent PDO seeds/FIFOs and explicit missing-fact failures. `DriverScenarioPublicTests.cpp` adds malformed-power preflight and a six-scenario/three-child sleep/wake sequence through both C API and CLI. Missing genuine artifacts skip explicitly; execution evidence remains Linux-only and establishes only the documented pageable resource-free power subset.

`DriverGuardTests.cpp` and four original `driver_guard.c` variants cover active/inactive CFG, rebasing, check/dispatch ABI and malformed targets. `KernelFrameworkTests.cpp`, `KernelFrameworkControlTests.cpp`, `KernelFrameworkQueueTests.cpp` and `KernelFrameworkRequestTests.cpp` cover bindings, validated registry-path snapshots, transactional device creation, queue routing, logical buffer lengths and cleanup/IRP/context lifetimes. The original `driver_kmdf_lifecycle.c` and `driver_kmdf_control.c` are optionally compiled against genuine WDK 1.33 headers and linked through the real `FxDriverEntry` library. Set `NEVERD_KMDF_FIXTURE` / `NEVERD_KMDF_CFG_FIXTURE` for lifecycle images and `NEVERD_KMDF_CONTROL_FIXTURE` / `NEVERD_KMDF_CONTROL_CFG_FIXTURE` for normal/active-CFG control-device images. Missing external artifacts are explicitly skipped. `DriverKMDFLifecycleTests.cpp`, `DriverKMDFControlTests.cpp` and C API/CLI cases in `DriverScenarioPublicTests.cpp` cover actual callbacks, buffered/direct I/O, pending worker completion, failure status, unload and rebased CFG execution. `DriverKMDFControlTests.cpp` also runs genuine-WDK buffered/direct/neither caller-context callbacks in normal/active-CFG images at preferred/rebased bases, verifying explicit enqueue before sequential queue delivery, completion within caller context, rejection of a callback that returns without either action, request-owned WDFMEMORY aliases for neither IOCTL/READ/WRITE, and no-access/read-only probe failures; `DriverScenarioPublicTests.cpp` executes positive caller-context and neither paths through the C API. KMDF cancellation coverage includes strict transfer-only virtual deadlines and report fields, completion-first and already-canceled paths, mark/unmark outcomes, queued-versus-delivered completion ownership, callback waits and internal-reference lifetimes. Scheduler tests independently verify DPC/cancellation/worker order, capacity, identity separation and suspend/resume. WDM cancellation has the separate genuine-WDK cases above. The original WDK-linked `driver_kmdf_pnp.c` fixture uses `NEVERD_KMDF_PNP_FIXTURE` / `NEVERD_KMDF_PNP_CFG_FIXTURE`; `DriverKMDFPnpTests.cpp` checks resource-free AddDevice, FDO/PDO and WDM/WDF handle identity, start, queue I/O, removal, automatic failed-Add cleanup and optional unload through normal/active-CFG images. The same genuine driver registers D0Entry/D0Exit and PrepareHardware/ReleaseHardware callbacks in service modes; tests cover delayed START, STOP/restart, final removal, empty resource-list getters, failed PrepareHardware and D0Entry with guaranteed ReleaseHardware, and unsupported callback rejection. Its default and explicit power-managed queue modes verify `WdfIoQueuePnpHeld` before D0 entry and during D0 exit, then normal I/O after each successful START, on normal/active-CFG images at preferred/rebased bases. `DriverScenarioPublicTests.cpp` exercises power-managed queue I/O through the C API and the hardware lifecycle through C API and CLI. A register-bank mode exercises read-only WDF resource descriptors, `MmMapIoSpace` and `MmUnmapIoSpace` across STOP/restart, with mutation and unreleased-mapping failure cases; the C API runs the positive resource lifecycle. Additional modes verify `EvtIoStop` completion, `WdfRequestStopAcknowledge` requeue and retained-request `EvtIoResume` across normal/active-CFG images, surprise-removal purge flags, worker completion after the callback returns without action, and an explicit stalled error when neither a callback nor a worker can complete the request. Native and C API tests also cover a power-managed queue without EvtIoStop that waits for a real worker completion, including a device with no registered hardware power callbacks. A separate genuine WDK mode verifies that `WdfDeviceInitSetDeviceType` reaches the FDO `DEVICE_OBJECT` on normal/active-CFG images at preferred/rebased bases; WDM unit coverage checks opaque vendor device types and control initializer rejection. Separate genuine control and PnP modes verify that `WdfDeviceInitSetExclusive` reaches `DO_EXCLUSIVE` on the created device at both image bases, while two opens through a nonexclusive named PDO remain valid. WDM stack tests check that the named lower object, rather than an exclusive unnamed upper object, determines open admission and that closing the first file releases exclusive access. A queued request submitted while stopped runs after restart, including the no-hardware-callback path; the C API covers both acknowledgment modes. Evidence remains Linux-only and does not establish full KMDF or PnP/power support.

Additional genuine `driver_kmdf_pnp.c` modes execute QueryStop, QueryRemove and
SurpriseRemoval notifications, including suspended callbacks before bus receipt,
query veto without lower side effects, later successful query and invalid
callback statuses. `AbsoluteSendTimeoutUsesCurrentTimeAndPreservesImmediateWins`
checks synchronous/asynchronous absolute deadlines from zero and advanced
virtual time, earlier timeouts, exact ties and immediate lower completion.

`KernelFrameworkFileTests.cpp` covers configured file identities, live WDM name and flags, FsContext/FsContext2 handle storage and retirement, optional file-object identities, request association, file-filtered manual and sequential retrieval, failed CREATE disposal, and CLOSE callback/context order. The genuine WDK `driver_kmdf_control.c` modes f/g/h/i/j exercise those paths in normal/active-CFG images at preferred/rebased bases, while the default no-file-object mode remains covered by request-accessor tests.


`KernelFrameworkRequestTests.cpp` checks cached input/output WDFMEMORY aliases, their original buffer pointers and logical lengths, zero-length and neither-I/O rejection, and invalidation after completion. The genuine WDK `driver_kmdf_control.c` modes b/c exercise both APIs and `WdfMemoryGetBuffer` over buffered/direct IOCTLs in normal/active-CFG images at preferred/rebased bases. `DriverScenarioPublicTests.cpp` runs the same buffered alias path through the C API.
The genuine WDK `driver_kmdf_pnp.c` file modes cover filter-default, explicit enabled and explicit disabled forwarding over a direct FDO/PDO route on normal/active-CFG images at preferred/rebased bases. `DriverKMDFPnpTests.cpp` verifies lower CREATE/CLEANUP/CLOSE status and callback order, failed lower CREATE disposal, and errors for missing or unused bus responses. `AutomaticFileForwardingWaitsForLowerCompletion` checks delayed automatic CREATE/CLEANUP/CLOSE; `OwnedCreateWaitsForDelayedLowerCompletion` and `OwnedCreateTimeoutCompetesWithLowerCompletion` cover callback-based asynchronous and synchronous CREATE, preservation of the suspended guest caller, Boolean send results, final lower NTSTATUS and lower-first deadline ties. Delayed send-and-forget remains independently covered. `DriverKernelFrameworkFileSend.PendingSynchronousSendRetainsOwnershipUntilLowerCompletion` rejects completion and request mutation while the caller is suspended, then restores driver ownership after the lower response. `DriverWDMPnp.DelayedFileForwardingRunsActualCompletionCallbacks` exercises delayed WDM CREATE/CLEANUP/CLOSE and failed CREATE through the genuine WDK completion routine, pending propagation and virtual timing on normal/active-CFG images at preferred/rebased bases. `DriverPnpScenarioTests.cpp` checks file-response schema constraints, while `DriverScenarioPublicTests.cpp` runs the forwarded lifecycle through the C API.

`KernelMDLChainTests.cpp` checks primary replacement, secondary append including an empty head, manual guest relinking, detached descriptor ownership, automatic completion-time unlock/release and surviving pool storage. Invalid cycles, dangling or shared links and removal of the original direct-I/O MDL fail before retirement; `KernelRequestMDLTests.cpp` also rejects private WDF descriptors inserted into WDM chains. `DriverMDL.GuestChainLinksSupportAssociationReplacementAndUnlinking` executes the genuine WDK macros and APIs at preferred/rebased addresses, while `DriverMDL.MalformedChainsAndOriginalDirectReplacementFailExplicitly` covers rejected guest chains. These cases do not establish arbitrary user mappings or hand-built MDLs.

Legacy cancellation tests retain the API continuation across cancellation, nested cleanup and final destruction; Ex still returns cancellation without callback delivery for an already canceled request. `KernelFrameworkRequestAccessorTests.cpp` and `KernelRequestMDLTests.cpp` check shared 64-bit Information, completion-time length validation, originating queue/IRP identity, NULL WDF file handles, retained-handle getter results, buffered MDL caching and first-direction ByteCount, direct descriptor identity and deferred mapping, completion retirement and rejected WDM-completion bypass. Genuine control-fixture modes L, M, D and C exercise legacy cancellation, buffered MDL/information, direct READ/WRITE MDLs and accessors after completion in normal/active-CFG images.

## Test layout

For optional independent bitvector oracles, configure `NEVERD_ENABLE_Z3=ON`
and run `NeverDSolverTests`; see [solver validation](solver.md) for the
cross-backend benchmark and replayable query export. Validate an OFF build too
when changing this boundary, so the default dependency-free backend remains
usable and explicit unavailable-backend requests are rejected.

`add_neverd_unittest` creates one GoogleTest executable and assigns every
discovered case a CTest label equal to that executable's target name.

| Source area | Target and CTest label | What it covers |
|-------------|-------------------------|----------------|
| `unittests/TestProcessTests.cpp` | `NeverDTestProcessTests` | Cross-platform child-process invocation, quoting, redirects, and exit codes |
| `unittests/libc` | `NeverDLibCTests` | Known libc names and classification |
| `unittests/safety` | `NeverDSafetyTests`, `NeverDSafetyIntegrationTests` | Sink catalog, identity precedence, argument prefilter, copy-overflow hunt, heap-lifetime audit, and the mandatory six-cell PE/ELF/Mach-O × x86-64/AArch64 matrix |
| `unittests/lift` | `NeverDLiftTests` | Decoder/lifter LowIR shapes, IR stages, loaders, relocations, format fixtures, decompilation, and representative patch flows |
| Most files in `unittests/semantic` | `NeverDSemanticTests` | Instruction, ABI, control-flow, C-expression, and lift/recompile differential semantics |
| `unittests/evm` | `NeverDEVMOpcodeTests`, `NeverDEVMBytecodeTests`, `NeverDEVMLoaderTests`, `NeverDEVMABITests`, `NeverDEVMAnalyzerTests`, `NeverDEVMDecoderPropertyTests`, `NeverDEVMProxyTests`, `NeverDEVMCallTests`, `NeverDEVMSemanticTests`, `NeverDEVMEmitterTests`, `NeverDEVMIntegrationTests` | Hardfork metadata, input normalization, ABI and signature ambiguity, CFG/SSA/recovery, exhaustive decoder boundaries and hostile inputs, proxy/call facts, interpreter semantics, LLVM/C/Solidity differential execution, and public API routing |
| `unittests/sbf` | `NeverDSBFMetadataTests`, `NeverDSBFProgramImageTests`, `NeverDSBFLoaderTests`, `NeverDSBFAnalyzerTests`, `NeverDSBFVerifierTests`, `NeverDSBFISAConformanceTests`, `NeverDSBFAgaveConformanceTests`, `NeverDSBFSemanticTests`, `NeverDSBFEmitterTests`, `NeverDSBFLLVMEmitterTests`, `NeverDSBFLLVMDifferentialTests`, `NeverDSBFSourceDifferentialTests`, `NeverDSBFMalformedCorpusTests`, `NeverDSBFUpstreamConformanceTests`, `NeverDSBFExternalOracleTests`, `NeverDSBFSolanaModelTests`, `NeverDSBFIntegrationTests` | v0-v4 metadata/layouts, strict verifier and loader behavior, 23 pinned ELF artifacts, official-process oracle, exhaustive opcode availability, hostile inputs, CFG/recovery, and executed LLVM/C/Rust differential behavior |
| `unittests/plugin` | `NeverDPluginRuntimeTests`, `NeverDPythonRuntimeTests`, `NeverDPluginTests`, `NeverDPythonPluginTests` | Native/Python loading, metadata, duplicates, lifecycle, GIL handoff, stale sessions, tracebacks, mixed discovery, and public C routing |
| `PatchFullSubstRTTests.cpp` | `NeverDPatchFullTests` | Rewrite/obfuscation equivalence across four ISAs and three object formats |
| Focused transform files in `unittests/semantic` | `NeverDSwitchXformTests`, `NeverDIndCallXformTests`, `NeverDCFGLoopXformTests`, `NeverDTwoTableXformTests`, `NeverDAvxUpperXformTests` | Small, fast-to-relink probes split out of the large semantic binary |
| `unittests/corpus` (submodule) | `NeverDWindowsEHCorpusTests`, `NeverDRustEHCorpusTests`, `NeverDGoEHCorpusTests`, `NeverDCxxItaniumEHCorpusTests`, `NeverDObjCEHCorpusTests`, `NeverDAdaDEHCorpusTests` | Exception and runtime metadata read out of 545 pinned real binaries, each declared in a manifest with the floors its recovery must clear |

The source of truth for registration is
[`unittests/CMakeLists.txt`](../unittests/CMakeLists.txt),
[`unittests/lift/CMakeLists.txt`](../unittests/lift/CMakeLists.txt), and
[`unittests/semantic/CMakeLists.txt`](../unittests/semantic/CMakeLists.txt),
[`unittests/evm/CMakeLists.txt`](../unittests/evm/CMakeLists.txt),
[`unittests/sbf/CMakeLists.txt`](../unittests/sbf/CMakeLists.txt),
[`unittests/plugin/CMakeLists.txt`](../unittests/plugin/CMakeLists.txt), and
[`unittests/safety/CMakeLists.txt`](../unittests/safety/CMakeLists.txt).

### The pinned binary corpus

Every other suite builds what it tests. The corpus does not: it is a submodule
of binaries that real toolchains produced, on hosts and for targets this
repository cannot reach, and each one is pinned by digest beside a manifest
stating the floors its recovery has to clear. That is the only place a claim
about what NeverD reads out of, say, a `-O2` stripped `armv7` shared object is
answerable rather than argued.

The suites are built only when the configure step is told to look for them, so
the flag is what keeps them under test:

```bash
cmake -S . -B build-corpus -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_BINARY_CORPUS_TESTS=ON
cmake --build build-corpus --target check-neverd-corpus --parallel 4
```

`check-neverd-corpus` runs every line; `check-neverd-windows-eh-corpus`,
`check-neverd-rust-eh-corpus`, `check-neverd-go-eh-corpus`,
`check-neverd-cxx-itanium-eh-corpus`, `check-neverd-objc-eh-corpus`, and
`check-neverd-ada-d-eh-corpus` run one each. All three CI hosts configure with
the flag and run all six lines: the bytes are identical everywhere, but what
reads them is not. A corpus run on one host proves nothing about the other two.
`scripts/audit_ci_test_inventory.py` refuses an inventory that is missing any of
the six labels, because a build that quietly stopped reading the corpus is a
regression no test can catch — the test is what went missing.

The EVM opcode audit always runs `git fetch` against the official
`https://github.com/ethereum/go-ethereum.git` default branch's remote `HEAD`
with `--depth=1 --force`, resolves and reports the exact SHA, and probes that
object in a detached temporary worktree. Each run uses an unpredictable
private temporary bare repository,
holds the fetched authority ref and its resolved exact SHA through the detached
worktree lifetime, and then destroys the repository and worktree together.
There is no shared persistent Git repository or cache. A
`local_docs` checkout, existing source tree, or submodule is never an audit
path; a pinned submodule would go stale instead of detecting live drift:

```bash
python3 scripts/audit_evm_opcode_metadata.py
```

Every Git command first clears all inherited `GIT_*`, including
`GIT_CONFIG_*`, then installs only audited settings. `GIT_CONFIG_NOSYSTEM`
and `GIT_CONFIG_GLOBAL` disable system/global configuration;
`GIT_ATTR_NOSYSTEM` and command-scoped `core.attributesFile` disable
system/global attributes, and `core.hooksPath` disables hooks. Unexpected
private-repository configuration, grafts,
`objects/info/alternates`, and `refs/replace` fail validation, while
`GIT_NO_REPLACE_OBJECTS` disables replacement lookup.

CI runs the same live audit for pushes to `dev`, pull requests, manual
dispatch, and once per day, so upstream drift is detected even when NeverD does
not change. The public CLI exposes only `--manifest-output`; it cannot select a
remote, ref, checkout, or toolchain. The emitted `schema 3` manifest is closed.
`EVMUpstreamOpcodePolicy.def` owns the closed name-alias plus
historical and unscheduled-EOF exclusion policy. The orthogonal
`EVMUpstreamSemanticsPolicy.def` owns the closed reflected boolean inventory of
`params.Rules`, maps forks, and declares exceptional stack prechecks and
dynamic-immediate families. The Go probe calls `LookupInstructionSet`, scans all
256 byte slots at each mapped fork, and decides allocation from geth's
`operation.undefined`. `HasCost` is only a cost cross-check because defined
zero-cost operations also return false: every `defined && !HasCost` slot must
match `EVM_GETH_ACTIVE_WITHOUT_COST` exactly at its declared activation fork.
An undefined slot with cost, an unreviewed defined slot, or loss of the marker
fails closed. The manifest verifies activation, byte/name identity,
`base_min_stack`, and `net_stack_delta`. Typed historical
and unscheduled-EOF exclusions must satisfy their declared overlap or inactive
invariant; unknown or duplicate schema fields, rules, forks, names, or bytes
fail. Missing, out-of-range, and syntactically unconsumed declarations fail too:
every `.def parser` rejects partial policy input. A failed CI run uploads the
exact geth revision, manifest, and log as an artifact. Parser and drift
diagnostics have independent Python unit coverage:

`EVMUpstreamSemanticsPolicy.def` assigns every exported boolean `params.Rules`
field exactly one `EVM_GETH_RULE_FIELD` category: `MappedForkSelector`,
`NoOpcodeAllocation`, or `ExcludedSelectorExpectedError`. The probe enables
each field alone through `LookupInstructionSet`; the first two categories must
return nil error, the third must return error, and every returned complete
256-slot opcode/stack fingerprint must equal `ExpectedFork`. Current
no-allocation fields `IsEIP155`, `IsEIP2929`, `IsEIP4762`, and `IsPetersburg`
fingerprint as Frontier; `IsUBT` must error and fingerprint as Cancun.

`EVMUpstreamSemanticsPolicy.def` declares the EIP-8024 dynamic opcode families,
operation kinds, and valid stack deltas; `EVMEIP8024Immediates.def` separately
owns immediate decoding and explicitly classifies all 256 bytes in both its
single- and pair-operand inventories. Production uses direct lookup. With
`go -overlay`, the live audit obtains the real private `operation.execute`
handlers and covers the `canonical fork jump tables` plus the
`mainnet active/scheduled jump tables` one table at a time. It records an
`inactive` family explicitly and rejects a `partial` family. Every active table
runs `DUPN`, `SWAPN`, and `EXCHANGE` over every immediate (`3x256`) plus
`3 missing-operand cases`, checking acceptance, PC delta, marker-derived
operands and stack mutation, exact valid underflow, and missing operand `0x00`.
Python compares the observations with the same declarative inputs without
restating the formula.

`EVM_HARDFORK_LATEST` has exactly one canonical target, while the closed
`EVMUpstreamForkAliases.def` maps Prague to Pectra, Osaka and BPO1 through BPO5
to Fusaka, and Paris/Shanghai/Cancun/Amsterdam/Bogota to themselves. Unknown
names fail closed. One recorded `audit_unix_time` drives both
`MainnetChainConfig.LatestFork(time)` (which must equal NeverD latest) and the
`LatestFork(max uint64)` alias/inventory check; both resulting instruction sets
receive a complete table comparison. The manifest fixes
`authority=official-fresh-fetch`, official URL, requested `HEAD`, and resolved
SHA. The probe uses `GOTOOLCHAIN=local`.

The Go request/response and the Python controller enforce
`input/collection/string hard limits` before allocating hostile metadata.
Oversized input, arrays, or strings fail closed. They separately enforce
`bounded diagnostic output`: an overlong display includes a full-content
`digest` and an `explicit truncated marker`. Bounded child output and a shared
deadline cover every command; a timeout or output-limit violation kills the
entire `process group`/process tree and drains its pipes.

The current schema-3 live receipt records `schema_version=3`,
`audit_unix_time=1787534659`, `authority=official-fresh-fetch`,
`remote=https://github.com/ethereum/go-ethereum.git`, `ref=HEAD`, revision
`02b73d4ea7181464175e0a6cbecc0a3a2655a562`, local `Go 1.24.0`,
`stack_limit=1024`, and `diagnostics=[]`. It covers `21 fork tables` and
`20 Rules probes` with `15 mapped/4 no-op/1 expected-error`. Both
`mainnet active/scheduled` records report `upstream BPO2`, closed-mapped to
`NeverD Fusaka`. EIP-8024 has `23 table targets`; only `Amsterdam/Bogota` are
active, yielding `1536 candidate executions` and `6 missing-operand cases`.
The `three handler symbols` agree across the two active targets. Python audit
is `67/67`, and `C++ Opcode 10/10`. The real macOS run succeeded under
`sandbox-exec` with network disabled for the final `go run`; the Linux workflow
mandates `bubblewrap`.

All Go stages—`go env`, `go mod init`, `go mod edit`, `go mod tidy`,
`go mod download`, and `go run`—must pass through the capability-root filesystem
sandbox. Its read capabilities contain only the private probe, fresh geth,
validated `resolved GOROOT`, and exact required system runtime roots; only
isolated environment roots are writable. Network is granted only to dependency
stages that need it and the final run is offline. Tests place sentinels in the
`host HOME/workspace`, require access denial, and require their contents to be
absent from every output. Linux exercises the isomorphic `bubblewrap` policy
without a `/` broad bind.

```bash
python3 -m unittest -v scripts.tests.test_audit_evm_opcode_metadata
```

For EVM control-flow work, run the fixed-point and height-domain contract first:

```bash
cmake --build build --target NeverDEVMAnalyzerTests --parallel 4
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.StackHeightDomain*:EVMAnalyzer.WholeProgram*'
```

These cases cover cross-block internal returns, finite multi-target merges,
loop convergence and deterministic edge ordering, path-dependent whole-stack
lanes, correlation preservation, unknown jumps, exact invalid targets,
fail-loud analysis budgets including `MaxAbstractInstructionTransfers`, and
strict versus relaxed stack faults. Strict rejects unknown or inactive opcodes
only on proven `Reachable` lanes; a
`MayReachable` edge remains a CFG candidate and cannot produce a definite
semantic fact.

All eleven registered EVM test executables are:

```text
NeverDEVMOpcodeTests
NeverDEVMBytecodeTests
NeverDEVMLoaderTests
NeverDEVMABITests
NeverDEVMAnalyzerTests
NeverDEVMDecoderPropertyTests
NeverDEVMProxyTests
NeverDEVMCallTests
NeverDEVMSemanticTests
NeverDEVMEmitterTests
NeverDEVMIntegrationTests
```

Run the complete registered family plus the live upstream audit after CFG,
decoder, ABI, proxy, call, or emitter changes. In particular,
`NeverDEVMDecoderPropertyTests` exhaustively compares complete decoding and
exact `JUMPDEST` boundaries for every two-byte input at each decoder-changing
fork, then exercises deterministic hostile byte strings through every fork
with a bounded input size.

For MedIR/HighIR dataflow changes, also run the constant-phi, selector,
typed-operand, malformed-graph, and deep-chain contracts:

```bash
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.MediumIR*:EVMAnalyzer.HighIR*:EVMAnalyzer.*Selector*:EVMAnalyzer.*MedIR*:EVMAnalyzer.RecoversStorageAndEventFactsFromTypedOperands:EVMAnalyzer.RecoversComputedCalldataArgumentOffset:EVMAnalyzer.*Return*:EVMAnalyzer.*Receive*'
```

These cases prove equal and conflicting cyclic phis, non-adjacent and
cross-block selector expressions, both equality operand orders, exact ABI
width checks, typed storage/event/calldata operands, root-constrained selector /
receive / fallback walks, shared-selector standard ambiguity, per-standard
`KnownFunctionVariantInfo` selection, successful-terminal return-shape
agreement, deterministic malformed MedIR handling, and an iterative
16,384-value producer walk.

Python plugin changes also have an exact C/Python/workflow drift audit:

```bash
PYTHONPATH=pluginsdk/python python3 -m unittest discover \
  -s pluginsdk/python/tests -v
PYTHONPATH=pluginsdk/python python3 -m unittest \
  scripts.tests.test_check_python_plugin_sdk -v
python3 -m mypy --config-file pluginsdk/python/pyproject.toml \
  pluginsdk/python/neverd_plugin
PYTHONPATH=pluginsdk/python python3 scripts/check_python_plugin_sdk.py
```

The first two runtime targets do not depend on the decompiler core and are the
fastest way to isolate loader, CPython, GIL, traceback, and capsule-lifetime
failures. `NeverDPluginTests` and `NeverDPythonPluginTests` then exercise the
same behavior through the exported `libneverd` C API.

## How fixtures are produced

### Lift and format fixtures

`unittests/lift/CMakeLists.txt` cross-compiles C and assembly sources during the
build. Clang target triples produce x86-64, i386, AArch64, and ARM32 ELF
objects, PE/COFF objects and linked images, and PIC/no-PIC Mach-O i386 objects.
When LLD is available, selected objects are also linked into executables for
patch tests. `NeverDLiftTests` depends on the `lift-test-objects` target, so a
normal build of that test binary refreshes its generated fixtures.

Most lift tests use `NeverDLiftFixture.h` to invoke the built `neverd` CLI and
inspect LowIR, MedIR, HighIR, LLVM IR, generated C, or a rewritten binary. The
`NEVERD` environment variable can override the CLI path for a focused manual
experiment; ordinary CTest runs use the executable embedded by CMake.

### Memory-safety fixtures

`unittests/safety/fixtures/binaries` contains checked-in PE, ELF, and Mach-O
images for x86-64 and AArch64, together with the PDB or dSYM companion each
format supplies and a linker MAP for every image. The MAP is what a stripped
build still ships, so each cell is also analysed with the MAP named explicitly,
which pins what a finding may claim once no types and no source lines are left.
`NeverDSafetyIntegrationTests` runs all six cells on every host; configure fails
if any required image or companion is absent, and the suite has no
host-toolchain skip path.

The equivalent binaries come from one source file. Rebuild the host-native
smoke fixture with `make`, or regenerate the complete checked-in matrix with:

```bash
make -C unittests/safety/fixtures matrix
```

The matrix recipe needs Clang's Linux and Windows cross targets, LLD's COFF
tools, both Darwin architectures, and `dsymutil`. Its debug paths are remapped
and CodeView command-line recording is disabled so checked-in companions do not
capture a developer's absolute workspace path.

### Windows exception reconstruction

Windows table-based exception changes need both representation tests and a
linked-PE patch test. The focused lift-suite filter covers the normalized
unwind/SEH/C++ model, corrupt-input handling, exceptional CFG edges, HighIR,
LLVM WinEH generation, exception-directory replacement, and Guard CF/EH
continuation reconstruction:

```bash
cmake --build build --target NeverDLiftTests --parallel 4
build/bin/NeverDLiftTests \
  --gtest_filter='COFFException*:*PatchCOFF_X64.ReconstructsGuardedSEHAndContinuationTable:*PatchCOFF_X64.ReconstructsNativeFH3StateGraph:*PatchCOFF_X64.RejectsInteriorExceptionDirectoryPadding:*PatchCOFF_X64.RebuildsSortedExceptionDirectoryInAppendedSection'
```

The guarded x64 assembly fixture requires Clang's Windows target and
`lld-link`; its CMake link uses `/guard:cf` and `/guard:ehcont`. A skip caused
by a missing cross-linker is not evidence for the final-image path. A passing
integration case proves that the rewritten PE can be reloaded and that its
runtime-function, unwind, load-config, Guard CF, and Guard EH continuation
tables remain sorted, file-backed, and executable-target valid.

The linked FH3 fixture covers the native C++ closure independently: fixed
state tables, HighC annotations, personality preservation, generated catch
targets, and the reloaded IP-to-state graph.

See [Windows Exception Reconstruction](windows-exception-reconstruction.md)
for the analysis/native support matrix and the fail-closed patch contract.

### Language exception models

Everything that is not the Windows table model lives in one focused target.
`NeverDLanguageEHTests` covers the DWARF frame chain, the Itanium
language-specific data area, ARM EHABI, Darwin compact unwind, the Go runtime's
frame metadata, Rust's panic machinery, and the three Objective-C runtimes:

```bash
cmake --build build --target NeverDLanguageEHTests --parallel 4
build/bin/NeverDLanguageEHTests --gtest_filter='ObjC*'
```

The tables in this suite are assembled byte by byte rather than compiled,
because the point of most of them is a combination no single toolchain emits.
Objective-C is the clearest case: all three runtimes emit an Itanium LSDA and
differ only in what a type-table slot holds, and they differ completely rather
than in degree. Apple's slot addresses an `objc_typeinfo` whose first two
fields imitate `std::type_info`, GNUstep's Objective-C++ slot addresses a real
`std::type_info` subclass, and the GNU runtime's slot is not a pointer at all
but the class name string itself. Applying one runtime's convention to
another's table does not fail; it reports a class name read out of the middle
of something else, which is why the runtime is established from the frame's
personality before any slot is read.

The same suite pins two distinctions that are easy to collapse and wrong to.
`@catch(id)` and `@catch(...)` are different handlers — the first takes any
Objective-C object and lets a foreign exception continue past it — and every
runtime spells them differently, so a decoder that reports both as a catch-all
puts a handler on exceptions that would in fact have flown by. And a
setjmp/longjmp call-site table indexes call sites rather than addresses, so a
reader that fails to recognize one of the SJLJ personalities does not error
out; it invents guarded ranges and landing pads the program never named.

Recognizing that form is not the same as refusing it. An SJLJ entry is a pair
of ULEB128 values — a dispatch selector and an action offset — and the action
offset means there what it means in the address form, so the action chain, the
catch types, and the exception specifications all read out of a table that
names no code at all. Only the region each entry guards stays unknown, because
the function's own stores into its call-site slot are what say it. The suite
also pins the byte that must not be trusted here: GCC writes `DW_EH_PE_uleb128`
as the call-site encoding and LLVM writes `DW_EH_PE_udata4`, both then emit
ULEB128 regardless, and no personality ever reads it — so neither may a
decoder.

Personality identity is pinned alongside, because it decides how every table
above is read. GNAT spells its routine the three ways GCC spells every front
end's — `_v0`, `_sj0`, `_seh0` — and on Windows registers one symbol while
forwarding to another, so all four spellings have to land on Ada. D is the
mirror image: three compilers, three names for one routine, one set of tables
behind them.

### Unicorn differential roundtrips

The semantic fixture tests behavior rather than textual shape:

1. Write a small C/assembly case or construct LLVM IR.
2. Compile it for the requested target with Clang/LLVM.
3. Execute the original machine code in Unicorn and capture the expected
   return value or other fixture-defined state.
4. Load and lift it through NeverD, emit LLVM IR, and compile the result back to
   machine code.
5. Execute the regenerated code with the same ABI, inputs, memory layout, and
   CPU model.
6. Compare the observable results.

The main implementation is
[`SemanticRoundTripFixture.h`](../unittests/semantic/SemanticRoundTripFixture.h).
The patch-full fixture uses `Codegen::compileForRewrite`, the same rewrite
backend as patch operations, then compares baseline and transformed code across
the full 4 x 3 ISA/format grid.

A deterministic NeverD semantic failure should be a failed test. Reserve skips
for an explicit external capability boundary, and read the skip reason: a green
summary with a missing cross-linker does not prove that format path ran.

### EVM differential backends

EVM interpreter tests provide a deterministic 256-bit oracle. The emitter
suite compiles and runs generated LLVM directly, lowers generated C23 through
Clang and executes it against the same host harness, and—when `solc`, `anvil`,
`cast`, and `jq` are installed—deploys a generated Solidity harness to a local
Anvil node. It compares status, storage, and instruction trace counts rather
than relying only on output text. A separate raw-bytecode corpus executes the
pre-Fusaka scalar ALU, calldata/memory copying, overlapping `MCOPY`, Keccak,
and return-data paths directly in Anvil and compares them with the interpreter,
guarding against a lowering mistake shared by all generated backends.

The oracle performs typed stack preflight before any opcode-specific side
effect. `EVMForkSemantics.def` defines byte `0x44` as `DIFFICULTY` before Paris
and `PREVRANDAO` from
Paris, and checks transaction rollback for `REVERT`, faults, step limits, and
`ExecutionFaultKind::ResourceExhausted`. Resource exhaustion that prevents the
entry snapshot is explicitly non-committable through
`HasPersistentStateSnapshot`; it is not reported as an ordinary semantic
success.

### EVM public-boundary and budget regressions

Public-API tests tamper independently with canonical
`Code`/`Fork`/`Instructions`/`JumpDestinations` and with every LowIR table,
range, ID, lane, and edge reference. `execute` must return `llvm::Error` before
instruction lookup, and `lowerToMedIR` must reject the complete malformed or
over-budget LowIR before building indexes or allocating proportional output.
For `lowerToMedIR`, tests enforce option validation, resource validation, and
structure validation before a field-by-field `canonical decode replay` and
before `lowerCanonicalLowToMedIR`. Public HighIR recovery replay-checks external
LowIR/MedIR; `analyze` alone may use `lowerCanonicalLowToMedIR` and
`recoverCanonicalHighIR` for its own canonical IR, avoiding recursive or
duplicate replay while still enforcing every HighIR option/resource budget.
The interpreter then tests exact-boundary and one-past-boundary behavior for
all limits declared by `EVMInterpreterLimits.def`: `MaxSteps` keeps the
dedicated `StepLimit`, while `MaxMemoryBytes`, `MaxTraceEntries`,
`MaxLogEntries`, aggregate `MaxLogDataBytes`, and runtime
`MaxPersistentStateEntries` exhaustion return `ResourceExhausted` and roll back
transactional effects. Oversized initial aggregate `MaxHostReturnDataBytes` or
persistent state is an API error. Initial `MaxCalldataBytes`, aggregate
`MaxHostEnvironmentEntries` across `BlockHashes`, `Balances`, `CodeHashes`,
`ExternalCode`, and `BlobHashes`, and aggregate `MaxExternalCodeBytes` are also
API errors. The `const execute preflight` rejects them before environment,
snapshot, or result copying. Return-data `ArrayRef` views and sorted-table
`lower_bound` lookup are covered without requiring a copied buffer or PC map.

Separate LowIR exact-boundary tests cover the aggregate diagnostic limits
`MaxLowDiagnostics` and `MaxLowDiagnosticBytes`: linear decode and CFG
construction both precharge exact count/final bytes, and zero is rejected.
HighIR safety tests exercise the sorted per-lane `Any/Exact/Excluded` domain,
equality match/exclusion, raw `XOR(selector, constant)` false-edge match and
true-edge mismatch, zero-word/calldata-size/call-value refinement, and
fail-closed unknown conditions. Their exact-boundary and one-less cases cover
`MaxHighDispatchCandidates`, aggregate `MaxHighRecoveredArguments`,
`MaxHighDiagnostics`, `MaxHighDiagnosticBytes`, `MaxHighReferenceVisits`,
`MaxHighMemoryTransferCells`, and `MaxHighMemoryValueVisits` from
`EVMAnalysisLimits.def`. They require every emitted diagnostic—including the
fixed malformed diagnostic—to charge count and final bytes before allocation.
The LowIR and HighIR diagnostic budgets are therefore tested independently, and construction
of the default root CFG region must charge `MaxHighRegionBlockReferences`
before reserve or block-PC copy.
Function-scope regressions cover both `EQ` and `raw XOR` back-jumps into a
shared dispatcher. They verify that another function cannot contaminate the
recovered `arguments`, `mutability`, `return shape`, or `region`, while shared
bodies and tail calls remain reachable.
External CALL/CREATE results are tested as nondeterministic host outcomes with
both precise CFG edges, preserving ERC-1167 fallback recovery; an unreadable
selector condition remains Unknown and cannot manufacture fallback or function
facts.

Control-flow tests derive `InvalidJumpDestination` from
`EVMLowFaultKinds.def` for an `end-of-code JUMPI`: definitely true with an
invalid target has no successful tail and is a definite fault; definitely false
succeeds; unknown retains the possible successful false path without marking
the whole lane definitely faulting.

ABI tests apply the grammar boundaries from `EVMABIParserLimits.def` and the
public-table cardinality/text boundaries from `EVMABITableLimits.def` at the
exact limit and one beyond. They also reject invalid kind/standard/evidence
enums, mismatched metadata, noncanonical signatures and return lists, shared
independent selectors, dangling or duplicate variants, and a non-word-sized
event-topic `APInt` before indexed selector or sorted topic lookup.

`NeverDEVMOpcodeTests` also enforces the metadata architecture: every assigned
opcode round-trips between byte encodings and typed values, family helpers are
checked at their boundaries, hardfork aliases resolve through the shared
database, and the complete stack-contract and host-argument maxima remain
derived rather than duplicated in backends.

### Solana SBF differential backends

SBF metadata tests validate every version feature, opcode collision boundary,
Murmur3 syscall hash, relocation, syscall source/availability, ELF machine,
register, and VM-address constant. Loader fixtures generate both legacy v0-v2
section layouts and sectionless strict v3/v4 program-header layouts without
vendored binaries. The hostile corpus probes overflowed ELF tables and
segments, overlapping runtime regions, malformed optional metadata, invalid
registers/branches/LDDW continuations, and immediate-domain violations.

`NeverDSBFISAConformanceTests` checks every byte encoding for each v0-v4 version
against an independently audited typed manifest. `NeverDSBFExternalOracleTests`
then compares the activation and boundary decisions with a separately built
official Anza process. `NeverDSBFUpstreamConformanceTests` assigns explicit
outcomes to all 23 ELFs at the pinned Anza revision.
`NeverDSBFSemanticTests` executes verified instruction bytes directly and does
not consume MedIR, so changing or corrupting normalized IR cannot make the
source oracle agree accidentally with a backend. It covers non-monotonic v2
semantics, memory, syscalls, internal call frames, faults, traces, and resource
limits.

The ORC suite executes lifted LLVM against that raw oracle. The source suite
compiles and runs generated C with warnings as errors and Rust with
`-D warnings`; both compare return/fault state, writable-memory hashes, and
syscall traces. Public API tests traverse every IR stage, disassembly, CFG,
metadata, LLVM, C, and Rust from a generated strict SBF ELF.

#### Audited SBF evidence snapshot

The reproducible gate was audited on 2026-08-24 and pins Anza `sbpf`
`2510663bb8d894e8e3094be351e4bb4b604f1f84`, Agave
`ef210d67f2fabeee1730498188fa78854260c679`, and the Solana SDK
`122f32e571ce39face4beffaccea733e37c207fd`. The Firedancer test-vectors corpus
is pinned at `68bb4af40235562e8852fa23d5727e49c2a0b862`. The pinned official ELF
manifest passes 23/23. `NeverDSBFAgaveConformanceTests` authenticates that Git
tree and matches all 1,955 `sol_compat_elf_loader_v1` fixtures (1,399 accepts,
556 rejects), including `entry_pc`, `text_off`, `text_cnt`, `rodata_hash`, and
`calldests_hash` for every accepted ELF. This loader-only gate deliberately
does not run the later instruction verifier. The independent official-process
gate checks 1,411 opcode and verifier-boundary cases through
`SBFOfficialOracleProtocol.def` and
`SBFOfficialVerifierCases.def` and `SBFOfficialExecutionConstants.def`;
`SBFOfficialELFMutations.def` names malformed
ELF dimensions, so no changing malformed-corpus total is frozen here.
Separately, the `41-case strict ELF differential` runs the complete deterministic
strict-v3 mutation table through the official `verify-elf-batch` process and
NeverD; its 41 cases are not included in the 1,411 opcode/verifier total.

The official additional execution matrix is separate: it has exactly 508 active (Version,Opcode)
cases plus 58 boundary cases = 566 exact execution cases. It
does not replace or count toward the 1,411 verifier probes or the 41-case strict
ELF differential.
Linux Release CI obtains the exact source/corpus pins and Rust toolchain with
`--print-pinned-revision`, `--print-test-vectors-revision`, and
`--print-toolchain`, builds the official process, authenticates the sparse
fixture checkout, and exports `NEVERD_SBPF_ORACLE` plus
`NEVERD_AGAVE_CONFORMANCE_ROOT`, making both external gates mandatory.
Ordinary local runs without the explicit oracle/corpus environment variables
still discover these cases and may skip them rather than downloading or
building upstream implicitly.

The default `RuntimeVersionPolicy::ChainProfile` uses `SBF_RUNTIME_VERSION` to
advance a slot-qualified cluster maximum from V0 through the official V1, V2,
and V3 enable-feature activations; the current maximum is V3. Explicit v4
analysis uses `RuntimeVersionPolicy::UpstreamToolchain` against pinned `sbpf`;
it is an offline capability, not a chain activation claim. The current 10 MiB
cap is exactly `10'485'760` bytes. 65,536 is historical provenance/test data
only and is not enforced. Execution faults have stable explicit values in
`SBFFaultCodes.def`; generated-source host status values remain a separate ABI
in `SBFSourceStatuses.def`.

Scale gates cover dependency worklists, per-function ownership, shared tails,
and multi-latch loops with 10,000-scale fixtures without asserting a
machine-specific duration. Runtime feature rows also support an
`RPC activation audit` of cluster account/slot evidence while ordinary tests
remain deterministic and offline.

### Solana SBF sanitizer profile

Use a separate build directory so sanitizer flags cannot contaminate the
normal integrated-LLVM build. The prebuilt NeverD LLVM package has RTTI
disabled, so standalone consumers must add `-fno-rtti` as well as the sanitizer
flags:

```bash
cmake -S . -B build-sbf-asan-ubsan -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DNEVERD_BUILD_PLUGINS=OFF \
  -DNEVERD_ENABLE_PYTHON_PLUGINS=OFF \
  -DLLVM_DIR=/path/to/neverd-llvm/lib/cmake/llvm \
  -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-rtti' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined' \
  -DCMAKE_SHARED_LINKER_FLAGS='-fsanitize=address,undefined'
```

Build and run the focused SBF targets listed below with
`ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:strict_string_checks=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. macOS ASan does not support
LeakSanitizer, hence the explicit `detect_leaks=0`; use a Linux sanitizer shard
for leak coverage. The pinned, revisioned prebuilt package includes the NeverD
LLVM fork's `llvm/MC/BinaryRewrite.h`, so `NeverDSBFIntegrationTests` runs in
the same fail-fast ASan/UBSan profile. Release evidence records named targets
and their results rather than a brittle aggregate case count.

```bash
cmake --build build-sbf-asan-ubsan --parallel 4 --target \
  NeverDSBFMetadataTests NeverDSBFProgramImageTests NeverDSBFLoaderTests \
  NeverDSBFAnalyzerTests NeverDSBFISAConformanceTests \
  NeverDSBFVerifierTests NeverDSBFAgaveConformanceTests \
  NeverDSBFSemanticTests NeverDSBFEmitterTests NeverDSBFLLVMEmitterTests \
  NeverDSBFLLVMDifferentialTests NeverDSBFSourceDifferentialTests \
  NeverDSBFMalformedCorpusTests NeverDSBFUpstreamConformanceTests \
  NeverDSBFSolanaModelTests NeverDSBFIntegrationTests

ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:strict_string_checks=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
NEVERD_SBPF_ROOT=/path/to/sbpf \
NEVERD_AGAVE_CONFORMANCE_ROOT=/path/to/firedancer-test-vectors \
NEVERD_AGAVE_CONFORMANCE_REVISION=68bb4af40235562e8852fa23d5727e49c2a0b862 \
ctest --test-dir build-sbf-asan-ubsan --output-on-failure --parallel 4 \
  -L '^NeverDSBF'
```

## One-shot targets

The custom targets build their dependencies and then run CTest with parallelism
derived from the host CPU:

| CMake target | Selection |
|--------------|-----------|
| `check-neverd` | Every registered test |
| `check-neverd-semantic` | `NeverDSemanticTests` only |
| `check-neverd-sbf` | Every `NeverDSBF*Tests` target/case |
| `check-neverd-patch-full` | `NeverDPatchFullTests` only |
| `check-neverd-switch-xform` | `NeverDSwitchXformTests` only |
| `check-neverd-cfgloop-xform` | `NeverDCFGLoopXformTests` only |
| `check-neverd-twotable-xform` | `NeverDTwoTableXformTests` only |

```bash
cmake --build build-release --target check-neverd
cmake --build build-release --target check-neverd-semantic
cmake --build build-release --target check-neverd-sbf
```

`NeverDIndCallXformTests` and `NeverDAvxUpperXformTests` currently have no
`check-neverd-*` convenience target. Build and select them by label as shown
below. `check-neverd-semantic` also does not include the separate transform or
patch-full binaries; use `check-neverd` for the complete aggregate.

## Incremental CTest workflow

Build the owning executable first, then select its label. This avoids relinking
unrelated large semantic targets.

```bash
# Lifter, loader, and format tests
cmake --build build-release --target NeverDLiftTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDLiftTests$' --output-on-failure --parallel 4

# Main semantic binary
cmake --build build-release --target NeverDSemanticTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDSemanticTests$' --output-on-failure --parallel 4

# A label-only focused transform binary
cmake --build build-release --target NeverDIndCallXformTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDIndCallXformTests$' --output-on-failure --parallel 4

# Every focused EVM target/case
cmake --build build-release --target \
  NeverDEVMOpcodeTests NeverDEVMBytecodeTests NeverDEVMLoaderTests \
  NeverDEVMABITests NeverDEVMAnalyzerTests NeverDEVMDecoderPropertyTests \
  NeverDEVMProxyTests NeverDEVMCallTests NeverDEVMSemanticTests \
  NeverDEVMEmitterTests \
  NeverDEVMIntegrationTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -R 'EVM' --output-on-failure --parallel 4

# Every focused Solana SBF target/case
cmake --build build-release --target check-neverd-sbf --parallel 4
```

Use a GoogleTest-derived CTest name for a single regression:

```bash
ctest --test-dir build-release --build-config Release -N \
  -L '^NeverDLiftTests$'
ctest --test-dir build-release --build-config Release \
  -R '^COFFARMPipeline\.ARM32ThumbLiftAndDecompile$' \
  --output-on-failure
```

Useful selectors:

| Command | Purpose |
|---------|---------|
| `ctest --test-dir build-release -N` | List discovered cases without running them |
| `ctest --test-dir build-release -L '<regex>'` | Select a test-binary label |
| `ctest --test-dir build-release -R '<regex>'` | Select case names |
| `ctest --test-dir build-release --output-on-failure` | Show diagnostics only for failures |
| `ctest --test-dir build-release --stop-on-failure` | Stop after the first failing case |
| `ctest --test-dir build-release --parallel 4` | Run up to four cases concurrently |

GoogleTest discovery uses `DISCOVERY_MODE PRE_TEST`, so the corresponding test
binary must exist before CTest can enumerate it. Per-case timeouts and the
separate discovery timeouts are defined in `cmake/AddNeverD.cmake` and may be
widened only for suites with measured heavy cases.

## Which tests should change with code?

| Change area | Start with | Then consider |
|-------------|------------|---------------|
| Architecture lifter or decode | Named case in `NeverDLiftTests` | Matching ISA semantic roundtrip |
| LowIR CFG, function detection, jump tables | Lift CFG/switch cases | `NeverDSwitchXformTests`, `NeverDCFGLoopXformTests`, or `NeverDTwoTableXformTests` |
| MedIR, ABI, flags, types, SSA | MedIR/calling-convention lift cases | Cross-ISA `NeverDSemanticTests` cases |
| HighIR or structured C | HighIR/decompile cases | `NeverDCFGLoopXformTests` and generated-C compilation checks |
| PE/ELF/Mach-O loader or input relocation | Matching `unittests/lift` format fixture | All-stage load/decompile test for that cell |
| Rewrite codegen or output relocation | `RewriteCodegenRTTests` cases | `NeverDPatchFullTests` and a linked patch fixture where available |
| LLVM IR transform used by patch | Focused transform binary | `NeverDPatchFullTests` composed-pass grid |
| C API or CLI | Direct SDK/query test and `unittests/semantic/CLIEndToEndTests.cpp` | Relevant pipeline/format suite |
| EVM loader, opcode, IR, or backend | Smallest owning `NeverDEVM*Tests` target | All EVM targets plus generated C/Solidity compilation |
| SBF loader, ISA, IR, or backend | Smallest owning `NeverDSBF*Tests` target | All SBF targets plus generated C/Rust compilation |
| Libc recognition | `NeverDLibCTests` | Semantic call/ABI cases if behavior changes |
| Heap-lifetime audit or copy-overflow hunt | `NeverDSafetyTests` | All six cells in `NeverDSafetyIntegrationTests` |
| Process execution or quoting | `NeverDTestProcessTests` | One affected CLI/semantic case on each supported host |

Tests should express the contract at the lowest stable boundary. A LowIR shape
test is useful for lifter attribution; a semantic roundtrip is required when
two plausible IR shapes could behave differently. Avoid golden dumps of whole
functions when a small opcode, CFG, or observable-state assertion is enough.

## CI relationship

CI builds Release with tests enabled on Linux, macOS, and Windows, then audits
the discovered inventory before applying platform-specific label exclusions.
Those profiles are defined in `.github/workflows/ci.yml` and
`scripts/audit_ci_test_inventory.py`. `NeverDSafetyTests` and
`NeverDSafetyIntegrationTests` are required on every matrix host, and every
such run reads the same checked-in PE, ELF, and Mach-O fixtures for both native
architectures. Because no single matrix shard represents every expensive
suite, a local `check-neverd` remains the clearest complete pre-merge signal
when the machine has all required cross tools.

Discovery is not execution evidence. CI writes a CTest JUnit report and the
original CTest exit status, then `scripts/audit_ci_test_results.py` reconciles
every selected `(test name, labels)` identity with its result. Passed, skipped,
disabled, infrastructure-not-run, failed, and missing results remain separate.
In particular, CTest can encode a missing executable as a JUnit `<skipped>`;
that is an execution failure, not an optional-backend skip. A stopped run can
omit unstarted tests entirely, so matching only the XML summary count is not
sufficient either. The audit runs even after CTest fails and preserves that
failure. Each matrix leg uploads its inventory, JUnit, exit status, outcome
JSON, and log as `ctest-evidence-<profile>`.

The required execution policy follows existing CI ownership:

- Linux must execute all selected `NeverDSemanticTests` cases; macOS must
  execute all selected `NeverDPatchFullTests` cases.
- Every host must execute the mandatory safety, native example plugin,
  concolic, binary corpus, and failure-integrity suites. The integrity suites
  cover worker exception transport, SDK session state, exact native LLVM
  function identity and verified caching, the semantic fixture, and pipeline
  outcome publication.
- Linux must also execute the SBF external oracle, upstream conformance, and
  Agave conformance suites because that leg installs their pinned dependencies.
- The only platform exceptions within these required suites are
  `ObjCEHCorpus.HonorsHostMachORewriteContractForEveryVariant` and
  `CxxItaniumEHCorpus.HonorsHostMachORewriteContractForEveryProbeVariant` on
  non-macOS hosts. Both execute native Mach-O probes; their exact identities
  and existing skip reasons are checked. Portable corpus-reading cases remain
  required everywhere.

A missing toolchain still appears as skipped, but a skip in a required suite
fails the CI evidence gate: the promised execution was not obtained. Optional
suite skips, such as an unavailable external Solidity toolchain, stay visible
in the JSON and JUnit artifacts and never contribute to the passed count.
Disabled tests and infrastructure-not-run results are never optional success.

The unit being audited is a **CTest registration**. A passed Python aggregate
runner does not prove that every child unittest executed; the separate
capability-evidence audit remains responsible for its more specific contracts.
Neither report is a claim of complete code coverage.

CI outcome auditing requires CTest 3.28 or newer for JUnit labels. This is a
CI-only tooling requirement; the project's minimum build version is unchanged.
The parser, policy, actual CTest outcomes, and workflow failure handling can be
verified without compiling NeverD:

```bash
python3 scripts/audit_ci_test_results.py check-tool
python3 -m unittest scripts.tests.test_audit_ci_test_inventory \
  scripts.tests.test_audit_ci_test_results scripts.tests.test_ci_configuration -v
```

## Android class inventory performance

Build `NeverDMobileTests` in Release and run its label before measuring
`neverd mobile INPUT --list-classes`. Reader tests cover sparse metadata,
Unicode, invalid references, unsupported method bodies, checksums and budgets;
archive tests distinguish full extraction from selected-payload queries. CLI
tests check prefix filtering, JSON scope, output preservation and multidex
failure atomicity.

The independent fixture/measurement harness validates every process's complete
descriptor inventory before accepting a timing sample:

```sh
python3 -m unittest scripts.tests.test_benchmark_mobile_inventory -v
python3 scripts/benchmark_mobile_inventory.py \
  --output-dir /tmp/neverd-inventory-benchmark \
  --class-count 6000 --dex-count 3 --code-units 64 \
  --extra-string-bytes 8388608 --resource-bytes 16777216 \
  --repetitions 7 --neverd build-release/bin/neverd
```

Use a new output directory for each run. `--generate-only` writes fixtures and
their manifest without timings. `--workload` selects shared input kinds for an
optional `--peer-command 'tool {input} {prefix}'`; input preparation is outside
the timed command. Reports retain hashes, commands, all fresh-process samples,
warm-cache assumptions and, on Linux with GNU time, maximum child RSS. That RSS
is not the combined peak of a multiprocess tool. Synthetic APKs are query
containers, not installable apps. Inventory speed proves neither reference
search speed nor Java recovery quality.

On hybrid CPUs, pin the harness and inherited children to the same allowed CPU
(for example, `taskset -c 4 python3 ...` on Linux) to avoid mixing performance
and efficiency cores. The report records the inherited CPU affinity.

## Android code reference performance

Reference queries share the recovery reader's instruction boundaries and code
validation. Run the mobile suite after changing this boundary. Reader tests
cover operand pool kinds, matching modes, method ownership and shared code,
payload/immediate lookalikes, malformed inputs and resource limits. Shared debug
streams are checked against every owning body's frame, extent and parameters.
Keep both large member inventories and branch-dense bodies in storage-limit
coverage; persistent indexes and temporary container growth have different
lifetimes.
Also check reordered and overlapping items, shared code with incompatible
same-width prototypes, unaligned input storage, and substring matches crossing
search-block boundaries. Performance changes to private decoder data must
preserve complete owned recovery models and reference occurrence multisets,
including failure behavior on unsupported recovery metadata. Distinguish
independently emitted expectations from cross-tool agreement on real inputs.

The independent reference harness records expected occurrences while emitting
instructions. It checks full method identities, code-unit PCs, opcodes, target
identities, UTF-16 units and multiplicity on every measured run. It also checks
NeverD's independently expected coverage counts and `code_scan_complete`:

```sh
NEVERD_REFERENCE_TEST_BINARY="$PWD/build-release/bin/neverd" \
  python3 -m unittest scripts.tests.test_benchmark_mobile_references -v
python3 scripts/benchmark_mobile_references.py \
  --output-dir /tmp/neverd-reference-benchmark \
  --class-count 500 --methods-per-class 64 --matching-methods 2 \
  --dex-count 3 --resource-bytes 16777216 \
  --repetitions 7 --neverd build-release/bin/neverd
```

Use `--kind` and `--workload` to select cases. `--extra-strings 65536` exercises
actual 32-bit string indices. Payload decoys are enabled by default;
`--no-payload-lookalikes` retains the same layout and true references while
replacing decoy payload values, for common-input comparisons. Keep both
correctness and timing results. A query returning false payload references or
missing real references fails validation and receives no accepted timing.

An optional `--peer-command` accepts an argv template containing `{input}`,
`{kind}` and `{query}`. Adapt query syntax explicitly where another tool uses
different semantics, and compare complete occurrence multisets. Its declared
validation scope is retained without claiming a full code scan. The same
fresh-directory, CPU-affinity, fresh-process, warm-cache and RSS qualifications
as the inventory benchmark apply. The optional CLI unit test is skipped unless
`NEVERD_REFERENCE_TEST_BINARY` names the built executable; report that skip.

## Mobile SDK export evidence

The manual `Mobile SDK Export Evidence` workflow runs `collect_mobile_ios_sdk_declarations.py --exports-only` against the pinned Xcode SDKs. It retains the exact Foundation, CoreFoundation, and UIKit linker maps for both the iOS device and simulator SDKs, with target, SDK version, SDK settings hash, file size, and SHA-256. The normal declaration collector retains these maps too. Missing, empty, oversized, or SDK-external files fail collection while preserving completed evidence. Linker maps establish symbol export evidence; they do not prove a call ABI or method recovery.

## Mobile Swift String ABI evidence

The manual `Mobile Swift String ABI Evidence` workflow compiles fixed Swift equality/ordering probes and a C `swiftcall` probe with Xcode 26.5 for arm64 iOS devices and simulators. `collect_mobile_swift_string_abi.py` retains source, LLVM IR, assembly, compiler identity, SDK settings, and `libswiftCore.tbd` with hashes. Both languages must show the exact five-argument comparison import returning `i1`; C must explicitly extend that result to a byte. Wrong targets, changed signatures, failed commands, and timeouts preserve partial evidence and fail collection. This compiler evidence neither installs a runtime declaration nor establishes method recovery. Test the collector without an SDK using `python3 -m unittest scripts.tests.test_mobile_swift_string_abi`.

The driver gap regression suites additionally cover:

- `DriverKernelSEH` validates chained records, partial prologues, complete XMM
  restores and canonical epilogues at each partially restored instruction.
  Return-address bias and invalid/truncated epilogues cannot trigger speculative
  stack reads. Genuine WDK SEH tests exercise nested filters and collided cleanup.
- `DriverWDMInterrupt` executes independent and unified MSI messages, line
  fallback, and passive ISR waits with repeated and independent arrivals.
  `DriverScenarioPublic` verifies message observations through the C API.
- `KernelMDLChain` checks independent page allocation, contiguous chunks,
  partial ownership, alias reuse, all six PAGE protection modes and pool pins.
  Genuine WDK user-mapping modes exercise allocation/protection/free and
  KernelMode pool locks at the documented IRQLs in normal/CFG/rebased images.
- `DriverKMDFPnp` checks self-managed callback ordering, failed initialization,
  ordinary D3/D0 resource retention, surprise-removal queue ordering, independent
  system-policy Query/Set children and
  missing/mismatched explicit responses. The public C API checks child origins
  and response indices without assuming result rows are grouped by origin.

`driver-strict` supports KVM on matching Linux x64 hosts and WHP on matching Windows x64 hosts; `auto` selects that native transport, and cross-ISA execution selects Unicorn. Explicit Unicorn and the original V1 API retain the portable software profile. Native execution checks canonical addresses and instruction effects before entry; unavailable hardware fails without fallback. Unsupported instructions and OS behavior remain explicit errors. Native Windows x64 CI with Unicorn disabled passes all 359 required checks: 131 CPU checks, 224 driver outcomes from 26 built-in images, 46 WDK images and 40 scenario cases at both preferred and relocated bases, plus four SEH boundary checks ([`9d4c130c`](https://github.com/NeverSight/NeverD/actions/runs/36981864458)). Native ARM64 runtime evidence is still pending, and this does not establish arbitrary-driver or Android/Darwin compatibility.

The native acceptance above covers the declared driver entry points and published scenarios. Detailed per-feature regressions and C API/CLI/Python checks described below retain their Linux-only evidence scope unless Windows execution is explicitly recorded; passing the native corpus does not extend evidence to every test variation.

Use `executionCapabilities(Contract, ISA, Backend)` to query the selected profile. `NativeLegacyX64` describes native x64 driver execution. `NeverDNativeDriverTests` validates the original corpus and can run with Unicorn disabled.

The existing CI workflow runs the complete emulation test directory before the general profiles and saves the discovery inventory, JUnit results and CTest log in `emulation-focused`. A failure elsewhere cannot prevent this focused run. Unavailable hardware and optional driver fixtures remain explicit skips; a passing software or compile check does not establish native execution.

On Linux, `NeverDUnicornDeadlineTests` completes the actual timer thread before guest entry using controlled pthread scheduling. It covers x64, ARM32 and ARM64, requires zero guest effects after pre-entry cancellation, and verifies that the next run uses its own budget. The test uses public engine APIs and does not mutate engine-private state.

`X64StateTransition` in `NeverDX64ExceptionTests` executes independent RAM loads and CR8 reads on the native CPU. It alternates TLS bases and privilege, resumes after repeated divide faults, and changes TLS after a cancelled entry. Run the owning CTest label together with alias-remapping, CPU-context, FP-state and original-driver parity coverage after changing native state transfer. An unavailable KVM/WHP transport remains an explicit skip.


`NeverDKvmRunTests` verifies the borrowed transfers in `KvmRunControl` without requiring `/dev/kvm`. `StateTransfersUseTheEntryThreadAndPrepareOnceAcrossRetries` checks that preparation, capture and the intercepted host entry share a thread, with one preparation across interrupted retries. Further cases cover preparation failure without entry, capture failure, stop during preparation and cancelled active entry, followed by a fresh run that cannot reuse the old callbacks. Keep the real cancellation, RAM rollback, exception and original-driver suites in the validation set. `SequentialEntriesReuseWorkerWithoutRetainingPriorTransfers` verifies that multiple entries under one deadline reuse the worker, execute each transfer once and leave prior packets unchanged.

KVM x64/ARM64 uses `KvmRunControl` to prepare state, enter `KVM_RUN` and capture state on one private vCPU worker. Preparation runs once across `EINTR` retries; cancelled entry or failed capture cannot publish. `KvmAArch64Machine.cpp` performs translation maintenance and complete scalar/vector transfers on this worker under one step deadline. The caller publishes only after acknowledgement; ISA decoding, RAM transactions, OS policy and observers remain on the caller thread. Native ARM64 runtime evidence is still pending.

`ReusesCapturedStateAndInstallsHostChangesAcrossFaultsAndStops` checks continued execution and independent CPU stores after host changes to general registers, both edge XMM lanes, MXCSR and x87 control. Actual `FXSAVE64` bytes verify all physical 80-bit registers, TOP, tags, opcode and pointers after a stopped entry; repeated divide faults also invalidate reuse. These machine-boundary tests do not admit additional x87 instructions into checked profiles.

`NeverDKvmStateTransferTests` injects a failed register or XSAVE read after real KVM execution, then retries the unchanged input. Independent integer and packed-byte results prove that failed captures cannot reuse advanced native state. Only this executable wraps `ioctl`; unavailable native hosts skip explicitly.

Checked ARM64 has one complete state boundary. `Registers.def` defines 39 scalar fields and 32 128-bit vectors; `captureAArch64State` stages every read, applies declared widths and NZCV normalization, then publishes once. Unicorn, KVM and WHP transfer the same inventory, including TPIDR_EL0, TPIDRRO_EL0, TPIDR_EL1, FPCR and FPSR. Native adapters enable FP/SIMD through CPACR_EL1. Any scalar/vector read failure or cancelled entry preserves all caller state.

ARM64 KVM/WHP startup executes the private `AArch64MachineProbe.def` program: NOP, FP32 addition rounded toward positive infinity and a two-lane SIMD addition. Each step compares all 39 scalar fields and 32 vectors, including TLS, NZCV, cleared upper destination bits and retained/cumulative FPCR/FPSR state. The probe uses supervisor monitor storage and one overall deadline. Success verifies this bounded initialization program; independent native ARM64 workload validation remains outstanding.

Native x64 KVM/WHP initialization executes `X64MachineProbe.def` in private supervisor pages. One deadline covers NOP, rounded FP32 addition, two-lane SIMD addition, FS/GS loads and CS/SS/CR8 reads; every step compares the complete scalar, XMM, physical x87 and control state. x64 and ARM64 probes require the exclusive physical-memory execution lease. `MemoryProjection` owns cache identity (ISA, address space, mapping generation, privilege and monitor variant) and committed root history per ISA. Builders invalidate before rewriting private bytes; failed replacement cannot reuse partially written tables, and callers cannot supply stale roots. These probes certify bounded initialization only; independent native ARM64 workload validation remains outstanding.

The shared XSAVE decoder distinguishes standard and compacted initial SSE state. With XSTATE_BV[1] clear, both forms initialize XMM registers; standard format still reads and validates MXCSR, while compacted format initializes MXCSR. `X64XsaveCases.def` supplies independent packet layouts and original host XRSTOR programs. `X64XsaveTests.cpp` checks rejected-state atomicity and compares both formats with actual host execution, preserving the caller’s FP/SSE state. The host oracle skips explicitly when the architecture or required instruction feature is unavailable.

`X64FPState.def` declares compacted AVX, AVX-512, CET_U/CET_S and AMX transport layouts, including 64-byte component alignment. Present extension payloads must be architectural zero init state; absent payloads and alignment padding do not define state. Layout bits determine offsets, and unknown layouts, non-initial payloads or incorrect lengths fail before publication. `CompactedOffsetsFollowLayoutRatherThanPresentBits`, `WideLayoutIgnoresAbsentComponentsAndAlignmentPadding`, `InitialCETComponentsDoNotHideFPState` and `InitialWideComponentsDoNotHideFPState` cover 872-byte and 10752-byte WHP packets. This transport support does not admit those extension instructions.

`WhpXsaveRegisters.def` supplements complete XSAVE packets with named x87/SSE control registers. Last opcode and instruction/data pointers are written explicitly and captured from the host; zero packet slots may be supplemented, while conflicting nonzero metadata or inconsistent shared controls fail before publication. `NamedMetadataRestoresOmittedPacketFields` checks the omitted-field case without dropping any FP payload.

Native `FOP/FIP/FDP` follow the host’s x87 save/restore rules. AMD may clear these fields without a pending unmasked exception; snapshots retain observed values. `X64MachineProbe.def` and exact NOP/context tests seed a coherent pending exception so every field remains valid and is compared without masking. The host-process FXRSTOR64/FXSAVE64 oracle checks both states; backends never substitute input metadata for host results.

`NativeGuestRAMDistinguishesEntryFromCaptureLoss` compares complete FP/SSE state saved by actual guest FXSAVE64 with the host XSAVE capture. Both API variants test direct installation and in-guest FXRSTOR64, with the default pointer-save feature and the explicitly selected host-supported setting. This isolates entry, guest execution and capture without repairing values in the test; a mismatch remains a failure. The boundary matrix also covers a pending unmasked x87 exception and records a host-process FXRSTOR64/FXSAVE64 reference plus processor vendor, to distinguish conditional pointer-save semantics from WHP transport behavior.

The shared `encodeX64XsaveState` / `decodeX64XsaveState` codec owns standard/compacted FP/SSE packets, physical TOP rotation, absent-component init state and atomic validation. WHP uses complete XSAVE APIs, preferring `WHvGetVirtualProcessorState` / `WHvSetVirtualProcessorState` with the older XSAVE APIs as a compatibility path. Legacy individual x87 registers cannot replace complete packets. Non-initial extended components, malformed headers, invalid controls and truncated captures fail explicitly. WHP mapping failures retain HRESULT, GPA and size for diagnosis.

`CheckedX64Instructions.def` admits unsigned `MUL` at 8/16/32/64 bits and `CBW/CWDE/CDQE/CWD/CDQ/CQO` through the existing processor transport. `NeverDX64IntegerTests` uses independent `X64IntegerCases.def` encodings and expected values at both privilege levels: partial-register preservation, 32-bit zero extension, both product halves, defined CF/OF results and unchanged flags for sign extension. Ordinary-RAM multiplication retains whole-span permission checks and read observers; a fault or observer stop preserves implicit output registers and PC. Device operands remain unsupported. These cases also run on checked Unicorn; unavailable native transports skip explicitly.

`X64BitInstructions.def` admits register and ordinary-RAM `BT/BTS/BTR/BTC` at 16/32/64 bits. A register bit index is signed at the operand width and selects a complete word; an immediate stays within the base word. Address-size wrapping occurs before FS/GS base addition. The processor supplies CF and written values; `RAMTransaction` keeps the result private until observers accept it. Whole-span permission checks cover separate page allocations and aliases. Stops, callback failures and denied pages preserve the original CPU and RAM. LOCK is limited to naturally aligned modifying memory forms; MMIO and parallel hardware SMP remain unsupported. `X64BitStringTests.cpp` compares independent encodings with actual x64 host execution and checks negative indices, width truncation, cross-page accesses, cancellation and invalid LOCK forms. See the [Intel instruction reference](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).

`X64StringInstructions.def` owns ordinary-RAM `MOVS/STOS/LODS` at 8/16/32/64 bits; `CLD/STD` controls direction without changing other flags. Each REP element validates the entire operand before observations and commits at one restart boundary. Earlier completed elements survive a later fault; cancellation or observer failure leaves the current element untouched. FS/GS applies only to the source, after address-size truncation. AL/AX loads preserve upper bits and EAX loads zero-extend. Zero-count address-size-32 REP requires zero upper count bits and, for MOVS/STOS, zero upper participating address bits: real CPU implementations differ otherwise. REPNE on MOVS/STOS/LODS and STOS/LODS device operands remain unsupported. `X64StringTransferTests.cpp` uses independent host instructions for widths, direction, overlap and zero counts, with separate checks for permissions, aliases, wraparound, faults and resumption. The original WDK resource driver executes all four STOS/LODS widths through `driver_resource_strings.def`.

`X64StringInstructions.def` also owns ordinary-RAM `CMPS/SCAS` at 8/16/32/64 bits with `REPE/REPNE`. Every element validates both complete read operands before observers, updates all six arithmetic flags, and stops on the first matching termination condition. A data fault restores the flags from entry to this uninterrupted REP while retaining completed pointer/count changes; a public resume starts from the published CPU state. Stops and observer exceptions leave the current element untouched. Early termination never reads the next element. FS/GS affects only the CMPS source; SCAS leaves the accumulator and unused source register unchanged. Device operands and ambiguous inactive 32-bit upper halves remain excluded. `X64StringComparisonTests.cpp` compares independent host instructions, flags, direction, aliases, wrapping, permissions and recovery; its Linux x64 signal oracle checks actual fault-time registers. The original WDK resource driver executes both conditional-repeat forms at all four widths through `driver_resource_strings.def`. See the [Intel instruction reference](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).

`WhpResourceCache.h` separates logical CPU state from WHP partitions. The runtime keeps one active native partition: consecutive steps on the same CPU reuse it; switching CPU retires the old partition before rebuilding mappings, a virtual processor and full state. Logical CPUs retain independent `MemoryProjection` views and authoritative RAM. Lease acquisition observes cancellation and the current deadline; retiring an inactive CPU cannot destroy another CPU's partition. x64 preserves the host's default XSAVE feature set and validates the effective partition via `WHvGetPartitionProperty`; it does not clear dependent features to force a reduced mask. Cooperative CPU switching does not provide parallel hardware SMP.

`NeverDX64FPTests` checks all 79 startup corruption positions and executes independently assembled `X64ProbeCases.def` instructions on native transports with one deadline and unchanged guest RAM. `NeverDProjectionCacheTests` checks caller changes, ISA order, root history, privilege/monitor variants, mapping generations, address-space identity and failed replacement. `NeverDRunControlTests` includes `WhpXsaveTests.cpp` for modern/legacy API packets, every TOP, size bounds and unchanged state on failure; these in-memory protocol tests do not establish native WHP evidence. Unavailable native transports skip explicitly.

XSAVE validation diagnostics distinguish size queries, local packet preparation and captured-packet decoding. They retain the API name, returned byte count, capacity and bounded header/control metadata from `WhpHostFailureCases.def` expectations; guest register payloads are not printed. `InvalidInputReportsPreparationWithoutHostMutation` also checks that rejected input never calls the host or changes its packet. The shared ISA codec remains the sole validation authority.

WHP host failures during capability queries, partition/virtual-CPU setup, register/XSAVE transfer and execution preserve the HRESULT and failing API name declared in `WhpProtocol.def`; capability-query failures retain the typed unavailable result. `WhpHostFailureCases.def` provides independent expectations for host failure concurrent with cancellation and modern/legacy XSAVE query, install and capture failures. The focused Windows dispatch requires 181 native passes: 16 mapping cases, two startup cases, ten FP/context cases, seven shared-CPU cases, eight integer cases and both `NativeInstallRetainsFPStateBeforeAnyGuestExecution` API variants. The latter compare complete FP/SSE and independently queried metadata before running guest code. Missing registrations, skips, disabled tests and not-run outcomes fail the native evidence audit. The additional 26 checks cover all `X64BitStringTests.cpp` cases at both privilege levels. Windows PE64 requires 44 WHP process cases and five independent native Windows oracle cases.

`NeverDMemoryLifecycleTests` is built independently of Unicorn, including native-only configurations. Its software-specific projection/device cases skip explicitly when Unicorn is disabled; matching-host shared-CPU tests remain registered. `WhpMemoryTests.cpp` isolates the native memory API with 16 cases in `WhpMemoryCases.def`: page/projection-sized backing, shared/independent allocations, untouched/resident bytes and a first virtual processor present/absent. Each case keeps two logical owners alive, switches their mapped partition repeatedly, retires the inactive owner and verifies that the surviving mapping stays usable without recreation. Genuine mapping failures retain HRESULT and fail the test; this is memory-API evidence, not instruction-execution proof.

`X64MachineProbe.def` names the failing startup instruction and every differing scalar, TLS, privilege, x87 control, physical FP lane and XMM word, retaining expected and observed values. `DiagnosticIdentifiesStepFieldAndBothValues` checks independent golden messages. State comparison remains exact; these diagnostics distinguish transport loss from instruction execution without certifying a failed native probe.

`WhpResourceTests.cpp` covers cache reuse, retirement before replacement, failure recovery and deadline/stop races. `LogicalCPUSwitchingRestoresPhysicalFPAndTLS` alternates two live machines in both privilege modes, checks independent physical x87/XMM and FS/GS state, then resumes the survivor after peer destruction. Windows CI requires both WHP privilege cases.

`NEVERD_ENABLE_SEMANTIC_TESTS` defaults to `ON` and controls the test group in `unittests/semantic`, including its aggregate runners. To build native CPU tests without Unicorn, keep `BUILD_TESTING=ON` and set both `NEVERD_ENABLE_SEMANTIC_TESTS=OFF` and `NEVERD_EMULATION_BACKEND_UNICORN=OFF`. The native KVM/WHP tests remain available, including Windows ARM64/MSVC builds with suitable SDK headers. Enabling Unicorn on Windows ARM64 still requires an ARM64 LLVM-MinGW toolchain. This build separation does not establish native ARM64 runtime coverage.

The native-only CI checkout initializes pinned Capstone sources and uses the verified prebuilt LLVM package. With `NEVERD_ENABLE_SEMANTIC_TESTS=OFF` and the Unicorn adapter disabled, its CPU owners configure, build and link without Unicorn sources. Signatures and the external corpus are not dependencies of these owners. Default CI keeps the complete semantic test group enabled.

The existing `ci.yml` provides an opt-in `native_cpu_only` manual mode on its Windows x64 runner. `NativeCPUTests.def` selects eleven owners; `run_native_cpu_ci.py` builds them before filtered CTest and saves inventory, JUnit, logs and a summary. Shared CI parsers distinguish passed, failed, skipped, disabled and not-run results. Every declared native WHP mapping case must be discovered and executed; missing or skipped native evidence fails the focused job. Default source-built CI remains unchanged. Protocol tests and compilation do not replace native WHP or ARM64 workload validation.

With `native_cpu_only=true`, `native_driver_tests=true` enables `NeverDNativeDriverTests` without Unicorn. Before configuring, `build_wdk_driver_fixtures.py` verifies the complete SHA-256 of the official Microsoft WDK/SDK 10.0.26100.6584 packages and rebuilds 46 original normal/CFG/DBG driver images. `WDKDriverFixtures.def` owns package identities, compiler/linker arguments and fixture bindings. Unmodified Microsoft inputs and their licenses remain in the local build/cache directories; CI uploads only build metadata and logs. The manifest records tool versions, commands, source/header hashes and output image hashes.

`NativeDriverTests.def` requires 224 WHP outcomes from all 112 workloads in `DriverBuiltinImages.def` and `DriverBackendParityCases.def`: 26 built-in images, 46 WDK images and 40 request scenarios, each at original and rebased addresses. Together with the 181 CPU checks and four shared SEH continuation regressions, 409 outcomes are mandatory. Fixed images retain their expected rebase rejection. Missing or skipped WDK images/scenarios fail this opt-in job; ordinary local builds keep external fixtures optional. `run_native_cpu_ci.py --with-drivers` records the configured owners and complete inventory/JUnit evidence. Building these images does not establish native Windows or ARM64 execution. Local reproduction uses the following commands; the generated cache can also be loaded into an existing emulation build. `181 CPU + 224 WHP + 4 SEH = 409`.

C SEH ranges remain half-open. A valid `__C_specific_handler` landing pad may lie inside its protected range: [LLVM 20.1.8](https://github.com/llvm/llvm-project/blob/llvmorg-20.1.8/llvm/lib/CodeGen/AsmPrinter/WinException.cpp#L600-L608) emits `EndLabel + 1` as the scope end. The Windows OS model preserves the raw endpoints and independently validates executable targets, function ownership and continuation identity, including after rebasing. `KernelSEHContinuationCases.def` retains the original fixture layout; `ScopeEndLabelMayOverlapTheHandlerLandingPad` checks constant handlers and filters. Companion tests preserve the exclusive end and reject invalid targets without consuming the dispatch state. These pure checks run in `NeverDNativeDriverTests` with Unicorn disabled.

Target unwind also uses the raw scope end: a `finally` whose protected range still contains the handler target is not exited. `FinallyRespectsRawScopeEndAtHandlerTarget` tests both sides of that boundary and, on Windows x64, compares them directly with `ntdll.dll!__C_specific_handler`. NeverD does not repair compiler-generated ranges. The Clang 20/21 builds of the original fixture return guest failure in modes `T` and `J` because their biased end includes the selected target; Clang 23 builds execute both cleanups. [LLVM change #144745](https://github.com/llvm/llvm-project/pull/144745) removes the old `+1` bias. These compiler-specific outcomes are distinct from backend failures.

The build inventory covers every original WDM/KMDF C fixture and every optional WDK CMake path. Each published `driver-*-scenario.json` has normal and CFG cases in `DriverBackendParityCases.def`; missing source, build or scenario bindings fail the inventory tests. `Original` clears any address override in the scenario and verifies the image's preferred base; `Rebased` verifies the declared relocated base. The caller-owned IRP scenario deliberately cancels a child packet, so `DriverNativeOutcomes.def` retains its expected unsuccessful aggregate result after successful teardown.

```bash
python3 scripts/build_wdk_driver_fixtures.py \
  --output build-driver-fixtures --cache build-driver-packages
cmake -S . -B build-native -G Ninja \
  -C build-driver-fixtures/fixtures.cmake \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_DRIVER_EMULATION=ON \
  -DNEVERD_ENABLE_SEMANTIC_TESTS=OFF \
  -DNEVERD_EMULATION_BACKEND_UNICORN=OFF
```

`NeverDAArch64StateTests` checks corruption of every scalar field, both words of every vector, privilege changes, missing floating-point execution and preserved transport diagnostics. `NeverDAArch64FPTests` runs `OriginalProgramChecksCompleteStateAndOneDeadline` using independently assembled `AArch64ProbeCases.def` instructions at both privileges on real transports. The test relocates these PC-independent words to guest code without granting user access to monitor pages. Unicorn execution and explicit native skips do not replace native ARM64 startup evidence.

`CheckedAArch64Instructions.def` and `AArch64InstructionEffects` admit bounded baseline FP32/FP64 arithmetic, comparisons, moves, scalar conversions and fixed-width SIMD operations at EL0/EL1. Scalar conversion tests check signed/unsigned W/X inputs, truncation and saturation, FP32/FP64 resizing, rounding, upper-lane clearing and cumulative status. Fixed-point, packed and FP16 conversion forms remain rejected. FPCR supports four rounding modes, FZ and DN; FPSR retains cumulative status and QC. Unsupported control/status bits are rejected before mutation. FP16 arithmetic, SVE/SME, unmasked exceptions, optional extensions and unlisted forms fail explicitly. This CPU support does not add Windows ARM64 driver loading or another OS environment.

`AArch64InstructionEffects` owns scalar and FP/SIMD single/pair RAM footprints, including operands up to 128 bits. The shared address space validates every page before CPU entry; `RAMTransaction` commits only complete declared physical writes. A 128-bit write observer receives two ordered 64-bit words before effects. Stops and faults preserve RAM, vectors and writeback. Numeric Xn/Vn overlap is valid; wrapping pair footprints are rejected. `NeverDAArch64MemoryTests` uses independent `AArch64CrossPageCases.def` and `AArch64VectorMemoryCases.def` encodings.

`NeverDAArch64StateTests` checks all 71 complete-state read positions at both privileges, including width normalization, missing readers and retry. `NeverDAArch64FPTests` executes original `AArch64FPCases.def` instructions: all vector lanes, packed arithmetic, scalar/vector floating results, four rounding modes, FZ/DN, cumulative FPSR, context state and rejected extensions/control bits. `NeverDAArch64MemoryTests` tests every crossing offset, ordered observers, stops, denied pages, aliases and restored vector inputs. `NeverDUnicornStateTransferTests` (`UnicornStateTransferCases.def`, `CapturesDeclaredWidthsWithoutStaleUpperBits`) injects every scalar/vector read failure after real execution. Unavailable native transports skip explicitly; these tests and cross-compilation do not replace native ARM64 KVM/WHP evidence.

`NeverDAArch64MemoryTests` covers 18 scalar/pair forms at both privilege levels with Unicorn, KVM and WHP. It checks every crossing offset, sign/width results, observer ordering, denied/unmapped second pages, explicit fault consumption and retry, repeated physical aliases, and context restoration after alias replacement. The former valid crossing-load rejection is reproduced before the change. Unavailable transports skip explicitly; portable Unicorn verification and cross-compilation do not replace native ARM64 KVM/WHP evidence.

`NeverDDriverGuardMetadataTests` (`DriverGuardCases.def`) checks zero-flag dormant CFG metadata, unchanged fallback pointers at both load addresses, invalid slots/targets and missing relocations. Its execution cases use `driver-strict` and `checked-x64-v1` on explicit Unicorn/KVM/WHP transports; unavailable transports skip separately. `DriverPublicCLICases.def` selects `--backend unicorn` for the CLI comparisons with the compatible v1 C API. Native and `auto` selection retain their separate public coverage and never silently fall back when the host API is unavailable.

Checked Unicorn uses `MachineRunControl`: one allowance covers ARM64 maintenance, guest execution and complete state capture. `UC_HOOK_CODE` checks the borrowed stop token and deadline at instruction entry; the synchronous engine call retires its hook borrow before returning, while the machine step retains control through publication. Unicorn and WHP stage complete CPU state and check the same control before publishing a successful step. WHP creates its allowance once before preparation. An authenticated x64 CPU exception takes precedence over a stop arriving during capture. The checked RAM transaction discards speculative stores when capture is cancelled; the unrestricted software contract is unchanged. `MachineInterruptedError` distinguishes acknowledged cancellation from host or capture failure. The shared checked CPU returns `Stopped` or `Deadline`, preserves CPU/RAM and permits retry; genuine failures remain `BackendFailure` even with a simultaneous stop.

Capture regressions: `NeverDUnicornStateTransferTests`, `NeverDUnicornMachineControlTests`: `StopDuringCaptureCannotPublishAndAllowsRetry`, `ExpiredCaptureCannotPublishAndAllowsRetry`, `CompletedStoreCannotPublishCancelledCapture`, `StopDuringCaptureCannotHideRealGuestException`. `UnicornPublicCapture.CancellationKeepsTypedExitStateAndRAMConsistent`; `NeverDKvmStateTransferTests`: `PublicCancellationRetainsStateRAMAndFailurePriority`.

`NeverDUnicornMachineControlTests` uses original store instructions from `UnicornMachineControlCases.def` on real x64 and ARM64 engines at both privileges. `RejectedEntryPreservesStateAndRAMAndAllowsRetry` checks cancellation before stepping, stop/deadline expiry at the actual guest entry, unchanged complete input and RAM, and one successful subsequent store. Its test-only entry wrapper does not require a hypervisor and does not establish native ARM64/WHP evidence.

`RunDeadline::invoke` rejects a stopped or expired WHP entry before calling the host, retains an actual host result during cancellation, and acknowledges interrupt callbacks before releasing the borrowed token. KVM and WHP validate a successfully captured private packet on the owning caller before classifying a concurrent stop or deadline. Genuine host/capture failures and authenticated x64 CPU exceptions retain priority. Ordinary successful state stays private until cancellation checks finish; an acknowledged interruption discards speculative CPU/RAM effects and permits retry. Preparation, native execution and capture share one step allowance. These controls provide cooperative cancellation, without a hard wall-clock guarantee.

`NeverDRunControlTests` includes portable `NativeEntryTests.cpp` and Windows WHP-enabled `WhpEntryControlTests.cpp`. In-memory host callbacks check rejected entry, retry, late cancellation, retained failures, completion priority and acknowledged callback lifetime without requiring Hyper-V. `NeverDKvmRunTests` checks caller-thread completion, error priority and reentry rejection. Real `NeverDKvmStateTransferTests` executes `KvmStateTransferCases.def` instructions; `ActualCPUExceptionOutranksStopDuringCapture` and `PublicCPUExceptionOutranksStopDuringCapture` stop after actual register/XSAVE reads and preserve divide exceptions, original context, RAM and explicit recovery. Portable tests executed with the Windows ABI under Wine are threading/control evidence only; they do not establish native WHP execution. Unavailable native transports remain explicit skips.

### Android native workloads

With CPU emulation enabled, build `NeverDAndroidNativeTests`,
`NeverDLinuxProcessTests`, `NeverDExecutionSessionTests`, and
`NeverDProcessPublicTests`. The Android fixtures are independently authored
freestanding ARM64 C, linked with Clang/LLD as ordinary, APS2, and RELR shared
libraries. They exercise constructors, sectionless linking, stack arguments,
TLS/stack guards, explicit properties, memory allocation, raw versus Bionic
error returns, output and execution limits, unsupported imports, and SDK/CLI
report parity. No Android device, NDK sysroot, or proprietary fixture is used.
The dynamic lookup fixtures exercise explicit library catalogues, provider
identity, repeated opens and NOLOAD, missing symbols, stale handles, null and
invalid names, TLS slot 6 consume-once errors, and named calls through guest
traps. Unknown implementations and unsupported process-wide lookup scopes
must stop. C/CLI and Python integration tests check the same lookup names and
returned addresses; no host library supplies those functions.
Linux regression tests guard the shared kernel-service boundary. Native KVM
and WHP cells may be unavailable on the host; report their skips separately.

`windows-pe64-v1` supports bounded Windows x64/ARM64 console processes with PEB/TEB, static and dynamic TLS, `DllMain`, named Win32 APIs and explicit acyclic DLL graphs. Guest modules support named/ordinal code and data imports, DIR64 rebasing, forwarded exports and actual loader-list identities. `LoadLibraryA` / `LoadLibraryW`, `FreeLibrary` and `GetProcAddress` use the configured module catalogue. CRT/GUI, user SEH, threads and general Windows application compatibility remain unfinished; native ARM64 KVM/WHP evidence is still pending.

Input-file bytes and aggregate image extents each share `memory_limit`; runtime mappings also consume the image budget. Preparation shares a 65,536-record and 64 MiB metadata-read allowance, bounded names and the workload deadline. Blocking host I/O has no hard time guarantee. The original EXE→DLL→DLL fixture checks rebased pointers, ordinal calls, shared data, API pointer identity, `MEM_IMAGE`, loader lists and executable TLS attach/detach. `NeverDWindowsProcessTests` owns these checks and the direct native Windows oracle; `NeverDPEProgramExportsTests` checks malformed metadata and work accounting, and `NeverDProcessPublicTests` checks C ABI/CLI catalogue parity. Unavailable transports are explicit skips.

`WindowsProcessLifetime` runs dependency DLL TLS callbacks then `DllMain`, followed by EXE TLS and entry, on one CPU under the same execution budget. Each module gets an independent TLS index and aligned block copied from the relocated, linked image within a shared 64 KiB arena. TLS reserved arguments are zero; startup/process-detach `DllMain` receives an opaque non-null value. Explicit process exit detaches successfully initialized DLLs in reverse loader-list order, then EXE TLS, even if EXE initialization had not run. Startup `DllMain(FALSE)` exits with `0xc0000142` without detach notifications. Faults and exhausted budgets do not invent cleanup. Returning from the PE entry with guest DLLs requires unsupported thread termination and stops explicitly. Nonzero `SizeOfZeroFill` remains unsupported; zero-initialized bytes in the actual TLS template are supported. DLLs without entry points receive TLS attach but no process-detach notifications.

`WindowsProcessExports` resolves static imports and `GetProcAddress` through the same named/ordinal identities, including code, data, aliases and chained forwarders. Only demanded startup forwarders add catalogue modules and initialization dependencies; unused forwarders do not load files. Export names are case sensitive; a missing name returns NULL/error 127, a direct missing ordinal (including a hole) returns NULL/error 182, and a null query argument returns error 87, while success preserves LastError. Unknown module handles remain unsupported. Exact provider/name API gates are reserved once from the bounded registry. Resolution checks every queried image’s live PE headers and export metadata, rejects changes or unreadable bytes, limits chains to 64 entries, and shares remaining preparation metadata credits and the workload deadline. A forwarder targeting a hole returns the target image base and preserves LastError; forwarding to ordinal zero returns error 87. The returned base is a data address, not permission to execute image headers. Runtime forwarders can load configured catalogue modules and complete initialization before returning a lookup result. Live export-table rewriting remains unsupported.

`WindowsProcessLoader` loads ASCII DLL basenames from `windows.modules` and owns explicit references, shared dependencies and startup retention. Repeated forwarded queries do not acquire extra references. Catalogue slots carry a new resident generation on reload. TLS and `DllMain` execute on the same CPU below suspended API frames; restoring registers preserves guest writes and uses the live return slot. Dynamic attach/detach reserved pointers are zero. Failed runtime attach returns error 1114 after cleanup, retaining successful independent nested loads. Unload releases image mappings and TLS; reload restores original image contents. Loader-list or TLS-pointer changes outside the model fail explicitly. File, image and metadata work credits remain cumulative across failures and reloads. API providers have no fabricated DLL handles. Filesystem search, non-ASCII paths, `LoadLibraryEx` flags, cyclic imports and reentrant transitions of an already initializing or unloading module remain unsupported.

`WindowsDynamicTests.cpp` compares original x64/ARM64 DLLs and EXEs with independent native Windows observations: reference counts, shared dependencies, nested loading, failed attach cleanup, forwarded lookup, process exit, no-entry DLLs and fresh TLS on reload. Additional regressions reject modified loader metadata and stale code pointers, preserve cumulative preparation budgets and keep interrupted API results incomplete. Windows CI requires the native oracle and WHP cases; cross-compilation and Unicorn ARM64 do not establish native ARM64 execution.

A missing library anywhere in a `GetProcAddress` forwarder chain returns error 127; an explicit `LoadLibrary` of a missing catalogue module returns 126. The native oracle and each available backend assert all 41 declared loader scenarios; return after unloading every DLL is observed 16 times per DLL variant on Windows. Failed initialization in a `GetProcAddress` forwarder also returns 127 after cleanup. Process-detach callbacks preserve the exiting caller’s stack contents.

`WindowsExportTests.cpp` uses original x64/ARM64 DLLs and an EXE to check forwarded code/data/ordinal calls, aliases, initializer queries, rebasing, case-sensitive misses, LastError, cyclic and nonresident targets, invalid pointers and metadata changes after successful queries. The same EXE has an independent native Windows oracle; WHP cases are mandatory in native CI. C ABI/CLI tests compare complete reports. Native ARM64 hardware evidence remains pending. Variants with and without EXE exports cover both dependency graphs, PEB-list order, detach order and name/ordinal/null error codes.

`WindowsLifetimeTests.cpp` compares frozen traces with independent native Windows processes and KVM/WHP/Unicorn execution: normal exit, entry return, both DLL initialization failures, four early exits and DLLs without entry points. It separately checks callback faults, shared budgets, relocated TLS fields and aggregate TLS capacity. The native entry-return probe retains the initial thread handle and checks its exit code and exact thread/process notification sequence in 64 repetitions. Remaining child threads are terminated after observation; their process exit is not treated as the entry return value.

Windows virtual memory adds `VirtualAlloc`, `VirtualFree`, `VirtualProtect`, `VirtualQuery` and current-process `FlushInstructionCache`. The OS layer owns reservations; `AddressSpace` remains the authority for committed pages, permissions and backing. Tests cover dynamic code rewriting, access faults and memory-budget reuse.

`NeverDEmulationWindowsProcess` → `os/windows/process/`; `NeverDWindowsProcessTests` + `NeverDProcessPublicTests`: [windows-pe64-v1](process-emulation.md#windows-pe64-profile).

External-store separation regressions cover exact/short budgets, partial words, both byte orders, untouched-memory identity, complete affine-spill boundaries, late predecessor overwrites and default invalidation. Public C API/CLI checks cover v6 layout compatibility and invalid domains. HighC and LLVMC outputs execute at O0/O2 with return, memory, stack and preserved-state checks; these tests are not a native equivalence certificate.

## macOS and iOS processes

`NeverDDarwinProcessTests` covers the macOS/iOS device/iOS Simulator profiles,
independent Mach-O admission and thread-entry fixtures, and Darwin 4 KiB/16 KiB
memory rules. Clang and `ld64.lld` build the SDK-free C fixtures.
`NeverDProcessPublicTests.DarwinProfilesPreserveBSDResultsAcrossSDKAndCLI`
checks all five platform/ISA combinations through the public API and CLI.
The required HVF gate includes the Darwin target. See
[Darwin process environments](darwin-emulation.md).

## macOS HVF backend

Hypervisor.framework supplies the `hvf` native transport. `auto` selects it for
a matching macOS host ISA; x64 driver execution on Apple Silicon continues to
use Unicorn. The existing ISA and OS contracts remain authoritative. See
[HVF ownership, signing and hardware tests](macos-hvf.md). ARM64 hardware
evidence and Intel runtime coverage are reported separately.

Finite-dispatch regressions cover register and frame phases, both byte orders, reachable invalid arms, later predecessors, exhausted inner guards and adjacent discovery limits. Destination-cap tests cover retained arithmetic correlations, nested loops, decode modes, fallthrough counting, total work limits and legacy precedence. CLI tests execute both C routes and source ABIs at O0/O2; C/Python v8 tests check layouts, invalid fields and ignored future tails.
