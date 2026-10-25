**Languages**: [English](testing.md) | [简体中文](zh-CN/testing.md) | [繁體中文](zh-TW/testing.md) | [日本語](ja/testing.md) | [한국어](ko/testing.md) | [Français](fr/testing.md) | [Deutsch](de/testing.md) | [Español](es/testing.md) | [Italiano](it/testing.md) | [Русский](ru/testing.md) | [العربية](ar/testing.md)

[← Documentation Index](README.md)

# Testing NeverD

`DriverKernelSEH.V2*`, `COFFExceptionParser.*V2*` and `DriverSEHV2Execution.*` cover normalized V2 tables, partial restoration, malformed metadata and original linked driver exceptions. Run `NeverDNativeDriverTests`, `NeverDDriverEmulationTests`, `NeverDWindowsProcessTests` and `NeverDCOFFExceptionV2Tests`; unavailable host backends remain explicit skips.

`DriverProcessPebDeclaration.*`, `DriverProcessPebExecution.*`, `KernelProcessPeb.*` and `KernelProcessPebInitialization.*` cover strict transport, explicit zero/full-width inputs, per-process identity, repeated native calls, forged tokens, missing declarations, IRQL and retained recovery dependencies. Run the native driver, driver emulation and kernel snapshot suites; unavailable backends remain explicit skips.

`DriverMSRExecution.*` and `DriverMSRDeclaration.*` check explicit MSR values, full-width splitting, preserved state, repeated reads, exact instruction limits, missing/invalid inputs, and lack of memory authority across available backends. `KernelMSRRead.*` retains unused-declaration dependencies. Run the native driver, driver emulation and kernel snapshot owners; unavailable WHP/HVF execution is a skip, not native qualification.

`NeverDIntegerCallArgumentsTests` checks integer record carriers, unused
register positions before memory arguments, register exhaustion, i386 slots,
indirect copies and refusal of incomplete layouts. `NeverDRecordCallContractTests`
checks the shipped glibc, musl and Darwin header layouts. On Linux x86-64 it
compiles genuine libc declarations at O0/O2 and checks both C emitters at O0/O2:
selected functions execute against an independent provider observing every
record field and following parameter; whole modules are syntax-checked with
their original import-slot aliases. Those aliases need the SDK's image linker
for runtime binding. Other hosts explicitly skip the native glibc test.
`LocalTypesDebugContext.RecordParametersRetainCompleteABILayout` checks alignment,
scalar leaves and refusal to publish a partial bitfield layout.

Run these with the System V, i386 and AAPCS64 call-contract tests and the source
control-flow tests after changing this shared boundary. The stack-join
regression executes generated C at O0/O2 for ELF and Mach-O; conflicting joins,
narrowing and overflowing arithmetic retain negative coverage. ARM32 record
placement and native non-x64 record-call execution remain unqualified.

`AndroidSignals.Descriptor*` and `Receive*` check caught/ignored/blocked and
terminating signals during original poll/epoll/pselect6 and TCP/UDP receive
calls. Independent O0/O2 fixtures run across relocation packing and available
backends, checking raw versus libc errno, full output guards, remaining timeout
and fault ordering, one-shot preservation, short reads, SA_RESTART, captured
SO_RCVTIMEO, FD reuse, mutable context, deferred handlers and nested receives.
Re-executed traps have separate attempts; abandoned attempts must not acquire
a fabricated result. Run these with all Android signal/poll/readiness/receive,
Linux process and execution-session regressions. All peers are offline inputs.

`AndroidSignals.Sleep*`, `IgnoredAndBlockedSignalsDoNotShortenSleep` and
`TerminationAndUnsupportedWaitsDoNotFabricateCompletion` execute independent
relative-sleep callers at O0/O2 across relocation packing and available
backends. They check remaining-time copy ordering and aliasing, raw versus
libc errno timing, SA_RESTART, bad/partial output, nested interrupted handlers,
mutable x0/PC, blocked/ignored signals, default termination and explicit
refusal for unmodeled synchronization waits. Run with the full signal, sleep, Android
native, Linux process and execution-session suites after changes to wait
ownership. The clock and signal delivery are guest-only.

`AndroidSignals.Delivery*` executes independent O0/O2 AArch64 callers across
relocation packing and available backends. It checks complete output buffers,
blocked coalescing, nested handlers, NODEFER/RESETHAND, mutable ucontext,
integer/vector/FP control restoration, inherited thread masks, shared pending
state, raw versus Bionic copy ordering and malformed-frame refusal.
`LinuxProcess.SignalDelivery*` covers both raw LP64 ABIs and explicitly rejects
x64 handler frames. `ProcessReport.SignalDelivery*` checks unknown and strict
input state. Run these with all signal, Linux process, Android native and
execution-session tests after changing shared delivery ownership. Unavailable
CPU backends are skips, not qualified execution; all fixtures are authored
locally and never signal a host process.

`AndroidSignals.Kill*` executes independently authored direct, dynamically
resolved, variadic and raw callers at O0/O2 across relocation formats and
available ARM64 backends. It checks full-buffer effects, argument narrowing,
errno, unknown process/group and handler boundaries, provider lifetime and
whole-process termination initiated by either thread, including a completed
join. `LinuxProcess.Kill*` exercises the x64 and AArch64 raw ABIs.
`ProcessReport.SignalTermination*` checks the additive signal result and retained
entry/thread history. Run the Linux, Android and execution-session suites after
changing the shared signal service. These tests never signal a host process.

`AndroidJNI.RegisteredNativeCalls*` executes independently authored guest native
targets through late-bound JNI method IDs at O0/O2 across ordinary, APS2 and
RELR images and available ARM64 backends. It checks all three JNI argument
forms, scalar widths, declaring-class identity, virtual/nonvirtual dispatch,
rebinding, nested JNI, two sleeping native threads, caller/global reference
survival, implicit frame retirement, exceptions and failed class initialization.
Counterexamples cover missing/unregistered targets, alignment/access faults,
argument/result types, retired references, frame underflow, monitor/FP/stack
ABI refusal, recursion and original-machine instruction limits. Run the full
Dalvik, Android JNI and existing callback/thread suites after changing this
shared call boundary. These tests do not execute a device JVM or qualify
DEX-to-native continuations or Java library loading.

`AndroidFrameworkTest.*` checks explicit API 28 lifecycle phases, retained
Application/Context identities, separate loader namespaces and parents,
load-without-initialization, JNI-library/DEX sharing, type/null checks and
terminal resource stops. `AndroidJNI.Application*` executes independent AArch64
callers at O0/O2 across relocation packing and available CPU backends. It checks
two native threads, global/local reference deletion and detach/reattach against
the same process objects. `ProcessReport.AndroidApplicationSnapshot*` checks
strict JSON input. These tests use synthetic inputs without a phone, host JVM
or network; they do not establish arbitrary framework lifecycle equivalence.
The loader failure tests check exact loader/name isolation, null versus empty
and UTF-8 messages, fresh throwable identities, known-class conflicts, strict
typed/JSON admission, DEX catch and shared message access, terminal instruction
and heap limits. `AndroidJNI.LoaderFailures*` executes original O0/O2 AArch64
callers across available backends and relocation formats, including pending
exception identity, clear/detach/reattach and unknown FindClass/system lookups.
The File cases check retained JNI references across detach, shared DEX/library
objects, exact path/name/parent strings, missing input, foreign instances and
terminal budget refusal. `LinuxFileNamespace.WorkingDirectory*` independently
checks both LP64 ABIs, output bounds, root and unknown snapshots.
`AndroidFiles.*WorkingDirectory*` executes raw, syscall and Bionic callers at
O0/O2 across relocation packing, including guest allocation/free and errno.
`ProcessReport.GuestDirectoriesAreExplicitAndTyped` checks JSON boundaries.

`AndroidFrameworkTest.Instrumentation*`,
`ExplicitFieldWritesDoNotNeedAnInventedInitialValue`, and
`DexGetterAndNativeFieldShareMutableSubclassIdentity` cover independent lifecycle
inputs, unknown state, mutable identities, receiver/type checks and resource
stops. `AndroidJNI.InstrumentationFieldAndGetterShareLifetimeAndWrites` exercises
field IDs, getters, null/replacement writes and detach/reattach across native
fixture variants and CPU backends. `ProcessReport.AndroidInstrumentationInputIsExplicitAndStrict`
checks JSON types and phase constraints. Run these with the full Dalvik suite,
Android JNI regressions and ProcessReport tests after changing framework fields.

`LinuxFileNamespace.CreatedDirectories*` and `Directory*` check directory
inodes, complete stat layouts, parent updates, setgid/umask, reservation/time
refusal, EOF insertion and duplicated cursors on both LP64 ABIs.
`AndroidFiles.DirectoryCreationHasSharedStatAndLifetimeAcrossCallABIs` checks
native, variadic and raw callers. `AndroidSearch.Substring*` checks exact first
match, empty/overlapping inputs, high bytes, page ends, errno and dynamic library
closure; `AndroidSearchMemory.*` checks memory/time bounds. The strict directory
JSON check is `ProcessReport.DirectoryCreationRequiresCompleteBoundedMetadata`.

`AndroidStringCopy.*` checks `strncpy` using independent AArch64 callers at
O0/O2 across three relocation formats and available CPU backends: exact padding,
truncation, high bytes, errno, full-width and zero counts, page ends, read-only
source/destination, overlap and dynamic provider lifetime.
`AndroidStringCopyMemory.*` checks atomic refusal, memory limits and timeout
against complete guarded guest bytes. No host libc copy is used as the oracle.

`AndroidSearch.*Span*` checks `strspn` and `strcspn` at O0/O2 across relocation formats and
available CPU backends. Independent expected counts cover empty sets, duplicates,
all nonzero byte values, read-only/overlapping strings, page-tail termination or
rejection, pointer faults, unchanged errno and dynamic provider closure.
`AndroidSearchMemory.Span*` checks both scan limits and terminal deadline refusal
against unchanged guarded memory. These cases do not call host libc as an oracle.
Complement spans separately cover empty reject sets, first-member page tails,
nonzero prefixes and both bounded scans; all shared accept-span cases still run.

`NeverDDirectTailCallMetadataTests` builds real x86, x64 and AArch64 CFGs and
validates direct tail-call targets at strict instruction boundaries. It covers
fallthrough and conditional transfers into known functions, an existing direct
jump, ordinary calls and indirect tail calls.

`NeverDSymMBAKnownFactTests` checks reuse of existing known-bit facts at
mask and predicate consumers, including independent sign-extension and
unbounded-value counterexamples. The shared deep-work budget remains in force.

`NeverDSymConditionalControlTests` checks branch-scoped conditional
specialization and constant-tag recovery, with unrelated predicates,
zero/partial budgets and independent exhaustive small-word evaluation.

`NeverDSymBatchSubstitutionTests` checks simultaneous substitutions over
shared DAG roots, stable ordering/duplicates, borrowed operand storage,
opaque operators and deep iterative traversal. Single-root substitution uses
the same implementation.

`NeverDSymMBARewriteCacheTests` checks context-owned exact rewrite reuse
across calls and opaque parents, capacity/zero-work limits, unchanged variable
identities, independent byte evaluation and reduced measured work. This cache
is separate from per-call Solved maps and low-word projection caching.

`NeverDSymSynthAffineTests` runs both the semantic affine regressions and the
existing synthesis contract suite without building the CLI. It covers
8/16/32/64/128/256-bit coefficients, eight-input factoring with noninvertible
coefficients, root conversions, signed one-bit inputs, an exhaustive byte-pair
arithmetic oracle, full-width sampling collisions, refusal on incomplete proofs,
and cost/work ceilings. An equal-budget comparison disables affine fitting to
isolate its contribution. Enable `NEVERD_ENABLE_Z3` to run the independent Z3 cases;
unavailable Z3 is explicitly skipped and is not proof coverage.
`SymSimplifyGuard.SemanticAffineSynthesisAcrossBlocksAndAtCallArguments` checks
the production LLVM pass across a dominated block boundary and at an observable
call argument. It requires a proof, verifies the emitted IR and retains the call.

```sh
python scripts/build.py build-release --target NeverDSymSynthAffineTests NeverDSolverTests --parallel 4
build-release/bin/NeverDSymSynthAffineTests
build-release/bin/NeverDSolverTests
```

`NeverDSymSynthQuadraticTests` checks oracle reconstruction of squares and
pairwise products, full-width coefficients, the noninvertible diagonal, opaque
typed inputs, narrow wrapping, nonlinear rejection and budget/proof refusal.
It includes exhaustive byte-pair arithmetic and a same-budget comparison with
quadratic fitting disabled. The Z3 cases explicitly skip when unavailable.
Run it with the affine/contract suite after changing shared fitting gates:

```sh
python scripts/build.py build-release --target NeverDSymSynthQuadraticTests NeverDSymSynthAffineTests --parallel 4
build-release/bin/NeverDSymSynthQuadraticTests
build-release/bin/NeverDSymSynthAffineTests
```

`NeverDSymMBAMaskRelationTests` checks full-word mask complements and masked-XOR
abstraction at 8/16/32/64/128/257 bits. It combines independent bitvector proofs,
full-width APInt arithmetic, exhaustive byte pairs and nearby nonidentities;
zero and incomplete work allowances must preserve semantics. The 257-bit test
explicitly increases only its verifier's width allowance, retaining finite SAT
and gate budgets. Production proof policies are unchanged.
`NeverDSymMBAWeightedMaskTests` adds 7–31-term overlapping inclusion-exclusion
sums and up to 16 disjoint weighted partitions, including opaque arithmetic,
flattened carriers, scaled/negated groups, opaque residuals, even/negative
coefficients and 257-bit words. Checks include
an independent grammar/basis verifier, full-width Z3 proofs, exhaustive byte
pairs, high-bit near misses, incomplete budgets, continued child simplification
and reuse of the proved root. The small-budget case must finish within its
8,192-unit allowance. The same cases also run in `NeverDSymbolicTests`.
The Z3 gate covers odd weights through 64 bits and even weights through 16 bits.
Dense 32/64-bit even cases exceeding its five-second allowance remain in a
separate complete-certificate test with an 8,192-unit budget, every one/two-bit
input, all-ones input and deterministic full-word samples.
The same target includes `SymMBABitwiseView`: independently flipped partitions,
nested literal AND/OR/XOR/NOT views, negative and even coefficients, opaque
arithmetic carriers and grouped remainders through 257 bits. Its separate
pointwise-grammar verifier rejects high-bit errors, different carriers and
nonlinear expressions that agree only at zero/all-ones. Exhaustive byte
oracles, deterministic mixed-view tests, finite work/storage limits and Z3
partition checks complement the production derivation. The 64-bit sixteen-part
case must complete within 16,384 work units and support exact rewrite reuse.
`SymMBAMultiCarrier` interleaves two to four different words inside shared
scales and subtractions. It checks distribution separately from each word's
bitwise proof, including flattened products, repeated residuals, constants,
negative/even weights and 257-bit words. Exhaustive byte arithmetic, high-bit
near misses, independent Z3 checks and exact/one-short work limits protect
proof composition. The two-word 64-bit case must finish within 32,768 units.
The independent dense multiword Z3 gate covers 8/16 bits. Its 32/64-bit cases
exceed the five-second allowance; the wide certificate test instead requires
complete derivation and checks every one/two-bit assignment across both words,
zero and all-ones through 257 bits against independent arithmetic.
The factored-result regression keeps a proved enclosing scale when subsequent
child rewrites would increase the final expression's cost.
`NeverDSymMBAQuadraticTests` (also `SymMBAQuadratic` in the full symbolic suite)
checks direct factor recovery against every pair of two-input
Boolean functions and nonzero scales modulo four, plus 4,096 independently
generated coefficient matrices. Wide even scales include cross coefficients
that vanish modulo the word width, through 257 bits. Exact/one-short work,
storage refusal and malformed matrices protect the reconstruction boundary.
The two previously capped masked-product regions must complete within 16,384
work units; the byte case also checks every input pair against independent
arithmetic. Expanded products retain unrelated partition variables until a
complete proof removes them.
The five-input expansion covers sixteen products, enclosing scales and a
high-bit remainder through 257 bits. Root recovery must refuse zero and
one-short proof allowances. Optional three-input Boolean tables that do not
fit must leave work for subsequent regions. All 256 three-input selectors
must remain exact with a 24-unit construction allowance, including wide words
and refusal with no work. Shared recovered roots under opaque consumers retain
their short Boolean form within the single-root work allowance plus 4,096.
Independent Z3 gates cover the
complete partitioned product at 2/4 bits; its 8–64-bit queries exceed the
five-second allowance and are not counted as SMT coverage. Wider cases use the
complete coefficient proof, independent word arithmetic and generated-C checks;
the four-bit case also exhausts all 65,536 assignments of its four variables.
`SymMBACubic` runs in both the full symbolic suite and
`NeverDSymMBAQuadraticTests`. Independent multiplication checks every unordered
triple of two-input Boolean functions with odd/even scales modulo eight.
Random anchored tables and scales through 257 bits check coefficients outside
the anchor slice, including multiplicities that wrap to zero. Zero/one-short
work, storage refusal and malformed coefficients must fail closed.
The previously capped mixed-degree cases and expanded cubes with unrelated
linear high-bit masks must finish within 65,536 units at 8/16/32/64 bits.
Heap-backed 128/257-bit cube coefficients use a 131,072-unit allowance to
include their limb costs. Independent Z3 gates cover complete cubic rewrites
at 2/3 bits; exhaustive four-bit and randomized wide arithmetic checks also
preserve multiple cubic products and high-bit remainders.
`SymMBAHigherProduct` runs in the same two targets. It covers four through
twelve factors at 8–257 bits, 64 separately expanded products, nonlinear
quotients, repeated factors, even scales and changed coefficients. Exhaustive
three-bit assignments check partial-budget results against an independent
formula; zero/one-short work and storage refusals protect the distribution
proof. Independent Z3 checks cover complete four/six-factor rewrites at 2/3
bits, with an explicit skip when unavailable. Wider checks use independent
word arithmetic. Independently enumerated modular products validate the
necessary coefficient bounds, and unchanged sixteen-factor residuals must
cancel before minterm expansion in the difference proof.
Sparse coefficient differences additionally cover four through thirty-two factors
at 8–257 bits within 65,536 work units. Negative/even scales, vanished
coefficients, high-bit differences and two independently disturbed terms use
independent word arithmetic. Exhaustive three-bit assignments check partial
proof budgets; separate Z3 checks cover the complete four/eight-factor rewrites
at 2/3 bits. Wider checks are algebraic and arithmetic coverage, not SMT claims.
The highest-bit coefficient additionally checks arbitrary arithmetic factors at
2–257 bits, neighboring coefficients that still observe carries, and separate
full-width Z3 proofs at 8/16/32/64 bits for the product-parity transfer.
Missing-corner completion covers four through sixty-four factors at 8–257 bits,
two missing corners, up to six independent partitions, opaque arithmetic words
and high-bit coefficient changes. Independent word oracles and exhaustive
three-bit assignments check completed and partial-budget results. Separate Z3
queries cover complete four-factor rewrites at 2/3 bits. A region without a
linear tail must retain its original word width; generated parity quotients
must finish within the unchanged 65,536-unit allowance.
Partially factored quartic and sextic inputs exercise the form produced by
source preprocessing, with full-width oracles through 257 bits, exhaustive
three-bit partial-budget checks and an independent SMT proof.
Subset-degree recovery checks every two-input cubic factor triple at all seven
nonzero three-bit scales, including vanished mixed coefficients. Independently
expanded fourth through eighth degrees also check the recovered factor supports
at 2–257 bits. Refusal controls separate insufficient work/storage from an
impossible support, and a nearby polynomial with the same support must still
fail coefficient validation. Single cubic products retain their exact factors
while the linear tail improves; independent word oracles and separate small-word
Z3 checks cover the resulting expressions.
Extremal-coefficient controls retain general cubic/quartic expansion and handle
cancelled leading terms. The slow fifth- and seventh-degree mixed tails have explicit
work ceilings and independent full-word arithmetic oracles through 257 bits.
`SymMBAArithmetic.SharedFlatProductsKeepProofsIndependentAndBudgeted` checks
repeated 24-factor tails at 2–257 bits, both complement policies, a changed
constant, zero storage and the exact last proof-work unit. Independent modular
arithmetic checks both expanded and factored forms. These tests also cover
reuse of completed subexpressions without sharing incomplete normalization.
Incremental product construction is checked against complete canonical
construction through 64 factors at 1–257 bits, in both operand orders, with
repeated factors, zero/even coefficients, wide literals and independent modular
arithmetic. Merging two products and later coefficient changes retain the
ordinary canonicalization rules.
Long distribution proofs additionally check 48 repeated factors, even scales,
unchanged opaque summands, changed coefficients and the exact final work unit.
Product valuation tests use independent word arithmetic through 257 bits,
neighboring coefficients that still observe carries, zero/short work, absent
storage and a refused seven-input domain. Separate Z3 checks prove the complete
zero-product rewrites at 8/16/32/64 bits. These are exhaustive parity proofs in
the implementation; randomized values are additional test coverage.
The `deep-residual` family of `benchmark_symbolic_masks.py` adds sparse
24/32/48/64-degree products, disturbed coefficients, interleaved blocks and
opaque arithmetic words. Use `--count 32` to cover each degree at all four
standard widths; the unchanged per-query allowance is 65,536 work units.
`SymMBAResidualBudget` runs in the symbolic and quadratic test targets. It
checks bulk extraction of 64 shared factors through 257 bits, exact and
one-short work limits, storage refusal, and delayed polynomial multiplication.
A sparse degree-32 regression retains a shorter completed candidate when
later search exhausts its budget; its independent word oracle evaluates the
closed product minus missing corners and a coefficient disturbance. Run these
with `NeverDHighSymSimplifyTests`, `NeverDSymSimplifyGuardTests`,
`NeverDMBASourceTests` and the worker source-synthesis test to check the common
CLI and GUI optimization path.
`SymMBAQuadraticRows` checks direct coefficient-row recovery against independent
matrices at 1–257 bits, including noninvertible scales, canceled inputs,
perturbations outside the proposal rows, exact/short work and storage refusal.
Single products retain their complete factors without reconstructing the
coefficient matrix. Separate word-congruence proofs compare complete bitwise
factor tables, retain noninvertible scales, and reject arithmetic expressions
that agree only on Boolean corners. Exact and one-short proof budgets remain
covered. Flat-product traversal and normalization share the same admission
rule; short products retain their required leaf normalization.
Pure bitwise tables also check parallel corner evaluation at 8/9/63/64/65/257
bits, permuted variable order and incomplete final words. Short known factors
must retain a simpler linear tail under the same bounded allowance; independent
word oracles cover the entire Boolean cube and arbitrary word values. Affine
selector cost bounds require complete weights, with a changed nonsingleton
corner and opaque inputs preventing an unjustified affine assumption.
Common-power tests require a separately proved low-degree quotient; repeated
normalized monomials retain their modular coefficients and multiplicities.
The worker source-synthesis test also executes missing-corner degree-48/96
fixtures through both source backends, CLI whole/function requests and GUI
worker requests, with optimization toggles kept independent.
Use `--family row-residual --count 24` for additional degree-8/16/32/64/96
products with overlapping quadratic quotients, opaque arithmetic inputs,
coefficient disturbances and canceled summands. Keep the existing families as
controls when measuring candidate reuse or changes to polynomial search.
Use `--family mixed-frontier --count 24` for degree-12/36/72/108/144
products with odd nonlinear common powers, mixed degrees, missing corners
and disturbed coefficients. Keep the same 65,536-work allowance and compare
all previous families; independent modular evaluation checks every result.
Interleaved residual tests cover the completed-product traversal boundary at
15/16/17 factors, plus products through degree 64 and 257-bit words. They keep
nonzero odd-factor assignments, reject leaked temporary inputs, and require
the previously exhausted 32-bit two-block case to finish within its allowance.
`SymParseLexer` checks every punctuation pair at end of input and before an
identifier, plus unknown bytes and exact diagnostics for malformed operators.
Sixteen repeated small partitions check that exponential expansion is refused
before consuming the budget needed to simplify their shared child.
`SymSimplifyGuard.MaskedNoWrapAdmissionMatchesExhaustiveByteDomains` checks
signed/unsigned overflow admission against all byte pairs, including unsafe
neighboring masks. `NestedMaskedBoundsPreserveHiddenPoisonAndWideDomains`
checks nested additions through 257 bits and rejects masked hidden poison.
`MaskedDisjointOrMatchesExhaustiveByteDomains` and `NestedOrAdmission*` check
OR-to-addition admission independently of compiler flags, including exhaustive
byte domains, wrapping carries, nested ORs, hidden poison, and width/depth
refusal. Supported masks through 512 bits are checked alongside a 513-bit
refusal; unsafe neighboring masks must remain ineligible.
`HighSymSimplify` covers signedness-preserving definition conversion, disjoint
frame locals, overlapping views, address escape and transitive store kills.
`HighFrameStoreForwarding.EmptyDeadStoreAnchorsPreservePrivateFacts` checks
empty versus effectful/malformed block boundaries across four architectures.

For reproducible performance comparisons, keep each CLI beside its matching
shared SDK and run `tools/neverd-bench/benchmark_symbolic_masks.py BASELINE
CANDIDATE --output REPORT.json`. The fixed seed generates complementary and
nearby noncomplementary expressions at four widths. Alternating warmed runs
record both binaries/libraries' hashes, results, work and in-process latency;
timings are measurements rather than CI thresholds. Copying an executable
without its SDK does not preserve an old engine.
Use `--family weighted --count 24` for seeded overlapping masks and partitions
over XOR, sums, products and nested carriers, including compiler-style grouped
subtractions and enclosing scales. Warm results from both builds pass an
independent modular evaluator on boundary and random full-word
inputs before timing; this sampling is a benchmark check, not rewrite authority.
Use `--family bitwise --count 24` for independently flipped partitions,
compensated flips, nested forced-bit views and high-bit near misses over the
same four opaque carriers. Retain the previous engine and its matching SDK to
measure each new change independently of earlier improvements.
Use `--family interleaved --count 24` for two-to-four-word partitions and
overlapping inclusion-exclusion sums mixed inside common scales, including
nearby high-bit errors. Run the earlier families as controls.
Use `--family quadratic --count 24` for expanded products with three to five
inputs, independent Boolean partitions, odd/even scales, modular zeros,
high-bit near misses and mixed-degree remainders.
Use `--family cubic --count 24` for three independently partitioned Boolean
factors over three to five inputs, even/negative scales, mixed quadratic
remainders and high-bit perturbations. Run all earlier families as controls.
Use `--family higher --count 64` for fourth, fifth, sixth and eighth degrees:
independent partitions up to 64 products, products of quadratic identities,
mixed degrees, same-degree residuals and deliberately changed coefficients.
The previous six families remain controls.
Use `--family perturbed --count 64` for fourth, sixth, eighth and twelfth degrees
with one or two coefficient disturbances, modular zeros, high-bit differences,
up to 32 distributed products and mixed-degree residuals. Retain all seven
earlier families as controls and report any remaining budget refusals.
Use `--family completion --count 64` for fourth, eighth, twelfth and sixteenth
degrees with one to three missing corners, up to 64 distributed terms, opaque
arithmetic factors, additional coefficient changes and mixed-degree residuals.
Run all eight earlier families as controls. Report remaining refusals separately
for the previous and newly added corpus; timing remains descriptive.
Use `--family residual --count 64` for eighth through thirty-second degrees,
two interleaved incomplete products, opaque arithmetic factors, three missing
corners and multiple coefficient disturbances. Keep all nine earlier families
as controls and retain separate refusal counts for this harder corpus.

`tools/neverd-worker/tests/source_synthesis_test.py WORKER --cli CLI` checks the
real source-option request used by the GUI and both CLI source emitters, for
single-function and whole-program requests. The raw machine-code bitwise fixture
retains sixteen separately flipped masks; enhanced output must recombine them.
The interleaved fixture combines thirty masked terms from XOR and product
carriers in shared positive/negative accumulators before scaling the result.
The partitioned-product fixture executes four products of Boolean pieces before
adding its linear remainder; both enhanced emitters must remove the partitions.
The mixed-degree fixture retains a scaled cubic residual beside the expanded
quadratic. The cubic fixture executes eight products before adding a linear
MBA and an observable high-bit remainder. Both exercise the same existing
symbolic option through worker requests and single/whole-program CLI output.
The fourth- and sixth-degree fixtures multiply additional factors in original
x64 machine code, so a fixture compiler cannot simplify the input first.
Their coefficient-disturbed variants multiply one original product by three;
the resulting difference must survive every enhanced CLI and worker output.
The missing-corner variants omit that original product altogether. Their
negative residual must survive both source emitters and single/whole-program
requests; the remaining complemented partitions must still disappear.
The twelfth-degree missing-corner fixture adds repeated tails in raw x64 code
and checks the same worker option and both CLI source-request modes.
With Clang available, each generated C output executes at O0/O2 against an
independent arithmetic oracle on boundary and deterministic random inputs.

`NeverDSymLoadOriginBindingTests` checks transactional history rebinding,
sorted conflict unions, independent forks and invalid batches. Historical
metadata never creates available bytes or restores a pre-clobber memory epoch.

`NeverDSymMemoryKnowledgeTests` checks caller-proved memory-fact import,
byte order, canonical aliases, transactional conflicts, modular relative
coordinates, absolute overflow, memory epochs and later store invalidation.
Snapshots contain materialized facts only; history never seeds their bytes.

`AndroidFDSet.*` checks Bionic checked descriptor bitmaps at O0/O2 with ordinary,
packed and RELR relocations on available CPU backends. Independent byte oracles
cover word boundaries, untouched canaries, boolean queries, full-width extents,
signed-int narrowing, read-only and partial-page access, overflow, FORTIFY error
priority and dynamic provider closure. The bitmap-only cases do not establish
readiness or execute host networking.

`LinuxSelect.*` checks the common x64/AArch64 `pselect6` owner with independent
bitmap, alias and stream-cursor assertions. It covers non-consuming TCP/UDP
readiness, EOF and empty datagrams, gated/absent inputs, wrapper/timeout/mask
error order, closed descriptors, ordered and partial output faults, aliased
timeout output and explicit unsupported waits. `AndroidFDSet.RawAndBionicPselect*`
executes actual AArch64 SVC and Bionic `syscall` callers, checking errno separation
against the same regular-file description. Timed Linux cases check arrival,
EOF, empty datagrams, independent timeout arithmetic, finite quiet intervals,
non-consuming queries, early nonblocking receives, suspended input copies,
output faults, FD replacement and atomic clock-overflow refusal.
`AndroidReadiness.*` runs two original guest threads at O0/O2 across relocation
formats and available CPU backends. It checks sleep/I/O deadline ordering,
exact timeout ties, shared-memory mutation during waits, output permissions
changed by another thread, raw versus libc errno, and exactly one completion
of the original call. No case opens a host socket.

`LinuxReceive.*` checks direct blocking TCP/UDP receives on x64/AArch64: partial
reads, delayed EOF, timeout/arrival ties, bounded silence, nonblocking and
negative timeouts, retained sockets after FD reuse, alias competition, captured
deadlines, late memory permissions and UDP peer lengths, and atomic refusal on
clock overflow. `AndroidReceive.*` executes independent raw SVC, variadic
syscall and libc read/recv/recvfrom callers at O0/O2 across relocation formats
and available CPU backends. Two guest threads check sleep/arrival ordering,
timeout changes, close/reuse, competing consumers, errno ownership and one
completion of the original call. All network input is synthetic and offline.

`AArch64SIMD.RegisterShifts*` checks baseline USHL/SSHL scalar D and fixed
vectors against independent bit selection, including signed low-byte amounts,
oversized shifts, destination aliases, cleared upper lanes, complete register
and guarded memory state, and reserved lane encodings. It runs in both checked
execution modes on available transports; integer shifts retain FP control and
status. Instruction semantics follow the [Arm instruction reference](https://documentation-service.arm.com/static/6245c734b059dc5ff9a8bdab).

`AndroidThread.ForeignTasks*` checks foreign pidfds and CPU-clock observations
while children live and after they exit, raw and Bionic errno results, identity
reservation conflicts, invalid flags and refusal of unsupported local thread
pidfd lifetimes. `LinuxPIDFD.*` retains the fixed-catalogue kernel matrix.

`AndroidAllocation.*` checks explicit heap-fill inputs with cross-page copies,
`calloc` zeros, `realloc` growth/shrinkage, failed allocation ownership and
zero-size identities at O0/O2 across Android relocation formats and available
Unicorn/HVF backends. `AndroidAllocationOptions.*` rejects non-byte inputs.

`AndroidSemaphore.*` runs independently authored API 28 callers at O0/O2
with ordinary, Android-packed and RELR relocations on available CPU backends.
It checks full object bytes, reserved padding, errno, overflow, aliasing,
read-only tails, dynamic provider lifetimes, multiple consumers, token
competition after wake, deadlock and failed resumption. Timed waits remain an
explicit unsupported boundary.

`AndroidRandom.*` compares 64-value sequences with answers produced by a
separate Android 9 BSD implementation, including zero/high-bit seeds, aliases,
reseeding, per-process isolation, two guest threads, dynamic provider lifetime,
`rand_r` writes and invalid/read-only pointers. Native callers run at O0/O2
with ordinary, packed and RELR relocations on available CPU backends. Custom
state generators retain an explicit unsupported test.

## Allocator validation

Use `python scripts/build.py BUILD_DIR` after CMake configuration to refresh
NeverSight default branches on each invocation. Keep `neverd-dependency-inputs.json`
and `neverd-build-receipt.json` with test evidence; compare commits across hosts.
A failed refresh is not a test pass. Fixture-only source overrides remain
available through raw CMake, but are not mainline qualification.

The allocator regression can run independently of LLVM:

```bash
cmake -S unittests/allocator -B build-allocator -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
python scripts/build.py build-allocator
ctest --test-dir build-allocator --output-on-failure
```

`NeverDAllocatorTests` exercises calloc/realloc, C++ containers, aligned new,
cross-thread frees, fork children and allocation/free pairs crossing a
shared-library boundary.
Enabled builds check that ordinary allocation APIs actually return mimalloc
memory. A dependency's pre-main allocation is allowed to predate registration
of the macOS malloc zone, and must survive through shutdown. In a full NeverD
build the test also checks sessions and strings allocated by the actual SDK:

```bash
python scripts/build.py build-release --target NeverDAllocatorTests
ctest --test-dir build-release/unittests/allocator --output-on-failure
```

`NeverDMimallocConfigure` checks explicit opt-out and suppression for combined,
configuration-specific and LLVM sanitizer settings, plus Windows CRT/toolchain
rejections, without fetching dependencies.
Repeat the standalone runtime check with `-DNEVERD_ENABLE_MIMALLOC=OFF` and with
`-DCMAKE_C_FLAGS=-fsanitize=address -DCMAKE_CXX_FLAGS=-fsanitize=address` to check
the system-allocator paths.

On Windows use an MSVC or clang-cl developer shell with the dynamic CRT:

```powershell
cmake -S unittests/allocator -B build-allocator -G "Ninja Multi-Config"
python scripts/build.py build-allocator --config Release
ctest --test-dir build-allocator -C Release --output-on-failure
python scripts/build.py build-allocator --config Debug
ctest --test-dir build-allocator -C Debug --output-on-failure
```

The Windows runtime test verifies CRT redirection, allocations during DLL
initialization, `_aligned_malloc`, and transfers between the executable and a
plugin loaded with `LoadLibrary`. A second invocation disables redirection via
`MIMALLOC_DISABLE_REDIRECT=1` and verifies system allocation instead. The
standalone install test relocates the executable, probe/plugin DLLs, allocator
DLL and architecture-matched redirect DLL into a temporary prefix and runs
there. `.github/workflows/allocator.yml` covers MSVC x64/ARM64 and clang-cl x64,
Release/Debug and the compile-time opt-out. The full Windows CI profile also
runs the allocator test against the actual SDK. The same focused workflow
checks Linux and macOS with mimalloc enabled, disabled and under ASan/UBSan.
The normal jobs leave `NEVERD_ENABLE_MIMALLOC` unset and require mimalloc to be
active, so a regression in the default cannot pass by testing the system heap.
Use native ARM64 binaries on
Windows ARM64; x64 emulation needs a separately built ARM64EC allocator and is
not covered by this integration.

For performance comparisons, build the same revision and Release configuration
with `NEVERD_ENABLE_MIMALLOC=OFF` and `ON`. Use `neverd-bench` on identical inputs
and `--max-func` settings, separate `--out-dir` directories, and the same
`NEVERD_SIGNATURE_CACHE` setting. Warm up both binaries, alternate several runs,
and compare median wall time, peak RSS and the per-stage `*.nd.bench.json`
timings. Check function counts, instruction counts and rejection/audit records
for equality before attributing a speed difference to the allocator. Use
`MIMALLOC_VERBOSE=1` for a separate activation check, not during timed runs.
Decode-only results do not measure the allocation-heavy IR stages; an allocator
change does not guarantee an improvement for every workload.


## Legacy GS recovery tests

`NeverDX86LegacyGSRecoveryTests` checks universal exchange/twice-exchange postconditions, full-width segmented memory addresses, aliasing, unchanged FS/memory/flags, coexistence with MXCSR, malformed states and scratch slots, atomic refusal, retained effects and budget boundaries. `PEFixedImageTest.RawLegacyGS*` checks native admission and receipt binding. Machine-state and native/LLVM refinement tests refuse unsupported source contracts, including unused declarations. `LegacyGSExecutionPolicy.RemainsUnmodeled` preserves concrete execution refusal. These are symbolic and policy tests, not native privileged-instruction execution.

## MXCSR recovery checks

`InterpreterSpecialization.MXCSR*` covers arbitrary entry state, exact memory values, reserved bits, DAZ, aliases, malformed forms, node projection and exact/short budgets. `PEFixedImageTest.RawMXCSR*` checks legacy/VEX native bytes and mask-bound receipts. The machine-state and native/LLVM refinement owners verify that unused capabilities cannot expand their interfaces.

## Segmented store recovery checks

`InterpreterSpecialization.SegmentedWrite*` checks independent byte-memory results, all scalar widths, base-addition wraparound, frame aliases, overwritten pointer origins, cross-node invalidation and exact/short budgets. `SegmentedWritesAndReadsShareLinearMemory` covers equal and distinct segment bases. `PEFixedImageTest.RawSegmented*` checks native bytes, live return-cell alias refusal and changed capability receipts. The existing receipt compatibility cases retain their values. Build the specialization, PE fixed-image, machine-state and native/LLVM refinement test owners; source and proof refusal checks include an unused write capability.

## Symbolic SHUFPS semantics

`SymIntegerIntrinsics.*` checks all 256 shuffle controls at 128, 256 and 512
bits, bit-pattern preservation, source/destination aliases and temporary
outputs. It also checks malformed shapes, unavailable values and unsupported
intrinsics. `NeverDSymbolicIntegerIntrinsicTests` runs these checks together
with the existing execution, store-preservation and x87 regressions:

```sh
python scripts/build.py build-release --target NeverDSymbolicIntegerIntrinsicTests --parallel 4
ctest --test-dir build-release -L '^NeverDSymbolicIntegerIntrinsicTests$' --output-on-failure
```

## Kernel provider residency

`NeverDKernelSnapshotTests` includes `KernelProviderResidentQuery.*` for
model-created provider residency: read-only headers/export tables, exact
endpoints, input-image metadata, aliases, changed permissions, missing mappings,
side-effect-free MMIO refusal, opaque fields and retained recovery dependencies.
`KernelResidentQuery.*` keeps the existing object, pool, lifetime and IRQL
boundaries.

## Concrete entry-memory concolic replay

Build `NeverDSymbolicTests`, `NeverDConcolicTests` and
`NeverDConcolicIntegrationTests`. `SymExecConcreteShadow.*`,
`SymExplore.ConcreteMemoryDoesNotConstrainSymbolicEntryBytes` and
`LowIRConcolicMemory.*` cover address-order snapshots in both byte orders,
overlapping stores, independent register/memory banks, missing bytes, invalid
ranges, reset and late seeding, exact/short/zero snapshot budgets, epoch and
model-width rejection, projected-memory replay and candidate identity. The
format matrix retains register-only replay on x86-64 and AArch64 PE/ELF/Mach-O.

These checks exercise the C++ memory-seed extension. The version 1 C/JSON,
Python and CLI interfaces remain register-only and keep their own contract
tests. Concrete trace success does not establish region disjointness or whole
function equivalence.

NeverD's tests cover three different questions: whether a representation has
the expected shape, whether a full pipeline route works for a binary fixture,
and whether generated code preserves behavior. Choose the smallest suite that
answers the question behind a change, then run the broader aggregate before a
high-risk pull request.

`PipelineOutcome.ResolvedCalls*` checks register banks, call/invoke identity, packed and split stack bytes, narrow values in their own slots, and refusal of missing bytes. `MedCallABI.AnIndirectCallRecordsTheSlotOfEachNarrowStackValue` checks the recorded offsets. `MedCallABI.CallbackTableAddressDoesNotReplaceTheActualFirstArgument` covers x64, ARM32 and AArch64 in ELF, PE and Mach-O. `LowInstructionBoundary.AArch64ImportedCopyNeedsExactLengthSourceOwnerAndAllPaths` checks all three formats and rejects clobbered lengths, writable or oversized spans, local names, wrong source registers, alternate entries and relocatable images. Host C differentials compile the source reference independently at O0 and emitted C at O2, preserving an independent oracle when a host optimizer miscompiles a recurrence. The AArch64 FMA check requests `-mfma` only on x86 hosts.

## Acceleration libraries

The bounded byte/Unicode, JSON and scheduler contracts can be checked without
LLVM. This includes the default oneTBB scheduler and mimalloc:

```bash
cmake -S unittests/acceleration -B build-acceleration -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DNEVERD_TEST_EXPECT_ACCELERATION=ON
python scripts/build.py build-acceleration --parallel 4
ctest --test-dir build-acceleration --output-on-failure --no-tests=error
```

Repeat with all four `NEVERD_ENABLE_{TBB,HIGHWAY,SIMDUTF,SIMDJSON}` options
set to `OFF` for the native/scalar/parser baseline; allocator settings remain
independent. Also check `NEVERD_ENABLE_MIMALLOC=OFF`, Debug, and Unix
AddressSanitizer/UBSan. Do not treat a skipped, absent or cancelled case as a
pass. The acceleration CI also runs installed binaries on each native host.

`NeverDAccelerationTests` checks supported host SIMD targets, every byte value,
alignments and inaccessible tails; UTF-8 validity; JSON decoded duplicates,
number boundaries, BOMs, depth and error precedence; nested/concurrent queues,
thread limits, original exceptions and large stacks. The full build additionally
runs `NeverDStringScanTests`, `NeverDTextEncodingTests`, pipeline, SDK and worker
owners. Compare complete string metadata and output, not only throughput.
Native pipeline CI repeats the complete selected owners and actual SDK/worker
checks with both the default oneTBB backend and the explicit native-thread baseline.
It also runs the pinned Capstone fork's x86 unit inventory, including guard-page
truncation and far-immediate encoding checks, with MSVC and clang-cl on Windows.
The Linux and macOS decoder checks enable ASan and UBSan. Every selected case
must run and pass; the jobs retain the source commit and JUnit records.

The default scheduler retains oneTBB workers between synchronous calls. Large
callers reuse spare stack headroom; small callers use a native coordinator.
It does not support fork after first use or dynamic SDK unloading; those hosts
must select `NEVERD_ENABLE_TBB=OFF`.
Its task arena limits each batch, while nested batches drain on a fresh native
worker stack. It never links `tbbmalloc` or `tbbmalloc_proxy`.

Performance measurements belong in ignored build directories. Measure Release
with matching inputs, process allocator and thread count; separate primitive
throughput from whole-request latency and compare all non-timing output.
The full build's `neverd-acceleration-bench` target reports wall and process CPU
time separately; use both when the host has competing workloads.

## Configure a test build

`NeverDInstructionFetchTests` checks warm code reuse, operand-byte changes,
page tails, execute-only code, guest/host alias writes and bounded working-set
eviction on checked x64 and ARM64. `NeverDMemoryLifecycleTests` adds shared-CPU
writes, address-space rebinding with equal mapping generations and context
restoration. Run these with ARM64 memory/atomic, FP/state, cache and cancellation
regressions when changing Unicorn single-step translation reuse. Keep timing
measurements separate from exact execution-state comparisons.

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

The native source dialect suite also uses `rustc` and `go` from `PATH`, or
`NEVERD_TEST_RUSTC` and `NEVERD_TEST_GO`. On Windows, its Rust executable
fixtures select `lld-link` explicitly so Git Bash's POSIX `link.exe` cannot
replace the COFF linker. It executes 32 Rust/C observations and
12 Go/C observations covering integer promotion, narrowing, signed/unsigned
comparison, arithmetic shifts and loops. The Go check compiles its generated
scalar functions unchanged; it does not establish that every Go pseudocode
construct is directly compilable. Missing compilers remain explicit local
skips and are failures in the main CI outcome audit.

See [CONTRIBUTING.md](../CONTRIBUTING.md) for clone, build-profile, and macOS
prebuilt-LLVM guidance.

## GUI function lists and navigation

`NeverDFunctionAdmissionTests` checks unnamed non-leaf entries with partial
unwind coverage, cached and uncached selections, actual split-chunk ownership,
and rejection of truncated instructions. `SafetyReport.PipelineCoverageRejectsPartialFunctionLifts`
also refuses legacy audits that dropped code solely for missing unwind data.

`NeverDWorkerFunctionalRegressions` checks cached function sorting in both
directions, exclusive navigation cursors beyond the first 512 functions, string
sorting across pages, and search continuation in both directions. Length sorting also exercises multi-megabyte literals with bounded UTF-8 previews and filtering on text beyond the preview. On Windows it
holds a reader handle without delete sharing: a short hold must let Save finish,
while a persistent hold must preserve the old file and recovery journal with a
path and Win32 error in the failure. A same-session retry completes only its own
unchanged journal, preserves newer staged edits and undo/redo, and refuses foreign
sidecars. Reopening also recovers the pending edit.
`SearchCAPITest.SearchPagesContinuePast4096AndReverseWithoutRepeating` checks
the real search matcher beyond its page budget, address zero, exact-limit
completion, reverse cursors, text case folding and invalid options.

GUI controller cases `sourceExportWaitsForItsOwnCompleteDocument`,
`searchResultsClearOnFailureCloseAndProjectSwitch`,
`newDisassemblyWindowKeepsIndependentCursorAndHistory` and
`recoveredDatabaseStaysDirtyAcrossReopenUntilSaved` exercise the actual views
and actions with the fixture worker. `databaseRecoverySurvivesConsumedJournalAndRejectsConflictingPack`
checks working-copy preservation and explicit discard when the packed state
changes. Run these with the native Windows Qt platform when checking Windows
file dialogs and view shortcuts.

`SessionCAPITest.SourceOptions*` checks opt-in HighC variants, complete paging,
restoration of ordinary output, reuse of prepared IR, navigation invalidation
and strict option validation. `SourceLLVMProfiles*` checks separate source and
module cache identities. `NativeLLVMInspection*` preserves inspection branches
across source profiles and both view orders. `NativeAnalysisViewsDeferOnlyHighIR*`
checks integer and floating call results, unchanged LowIR/MedIR/CFG/LLVM/source
output and IR page anchors, and resumption without relifting after analysis
views; `FunctionViewRejectsUnknown*` preserves the
prepared function on invalid view names. `LLVMViewShowsTheFunctionInAModuleOfItsOwn`
builds a glibc PIE: `_start`'s LLVM IR must assemble and verify alone, declare
the GOT's mirror before the definition under a comment saying what it stands
for, and name the `__libc_start_main` slot as the listing does; pages keep the prelude, epilogue and by-line references
through small pages, a rename of the slot reaches the comment, and `main`'s
strings read beside their references.
`LLVMViewMapsEachSpellingOfAMirrorToItsOwnOffset` renders a hand-written
module: a mirror spelled at two offsets on one line, and on a call whose
callee prints before its arguments, maps each spelling to its own address, and
each mirror's comment lists the offsets used, a bare name as +0. Worker source-cache tests also check
that preparation receives the requested LLVM representation.
`HighSymSimplify.OrdinaryConversion*` keeps optional
rewriting out of ordinary HighC; the other HighSymSimplify cases explicitly
enable it through the production converter. `SymSimplifyGuard.SourceProfiles*`
checks that the optional expression pass stays off when disabled and evaluates
all LLVM profiles against independent integer results, including wraparound.
`ControlFlowRecovery.SourceRecovery*` and `DefaultSource*` check independent
recovery, default O2 recovery and the conservative patch boundary. Dispatcher
regressions preserve effects, externally used values, subsequent state reads
and atomic accesses. GUI source-transformation
tests cover menu/keyboard activation, cancellation with retained source,
concurrent listing/function reads and reset on navigation. Worker source-cache
tests count engine calls separately for each option set. `HighExprSimplify.*`
checks exact masked status bits, signed extensions, shared expressions and
effect preservation without the symbolic bridge. The x87 classification test
executes ordinary HighC against native extended-precision encodings; ARM mode
tests execute both ordinary and explicitly enhanced source.

`NeverDHighSymSimplifyTests` provides the focused HighIR suite, including
8–256-bit affine and quadratic synthesis, synthesis inside signed/unsigned
width conversions
and exhaustive byte-pair overflow preservation. Wide-constant roundtrips check
positive, negated and complemented high-bit payloads in enclosing regions.
`SymSimplifyGuard.SourceSymbolicToggleEnablesBoundedSemanticSearch` checks
actual search work through every source LLVM profile. Proof-budget cases check
that synthesis exhaustion preserves IR while the separate split-word recovery
still completes.
`SymSimplifyGuard.SemanticQuadraticSynthesisAcrossBlocksAndAtCallArguments`
also requires a proof across a dominated block boundary and retains the
observable call. The real-engine
`NeverDWorkerSourceSynthesis` test sends the GUI's `source_options` request,
checks enhanced/ordinary cache isolation, and, when a CLI is configured,
exercises single-function and whole-program source in both routes, using
affine, quadratic, complemented-mask, private-frame and 15-term overlapping-mask
machine-code fixtures.
With Clang available it compiles all 30 enhanced outputs at O0/O2 and executes
each against 1,331 boundary triples and 4,096 deterministic random triples.
Missing Clang explicitly skips only that generated-C execution coverage.

`NeverDGuiMicrosoftSymbols` uses an in-memory network reply and the fixture SDK
to check the Load PDB file dialog (a matching local copy, declining, downloads
into the store the worker names, 404 and another build's file leaving the store
again) and the offer for an open file (local/disabled/absent-identity gating,
strict PDB application, the next load finding the stored copy, cancellation on
file changes). It never contacts a real symbol server. `SymbolStore.*` covers
`_NT_SYMBOL_PATH` parsing, store and server paths and candidate order;
`DebugInfoDiscovery.SymbolStoreKey*` and
`DebugInfoDiscovery.MissingAmbiguousOrUnsafePdb*` cover GUID byte order,
hexadecimal age, NB10 keys and refusal of unusable identities or unsafe path
components. `NeverDWorkerPDBLoad` opens a PE without the PDB beside it and finds
that PDB before opening it.

`DWARFSourceTypes.OriginsAndSpecificationsDeclareTheTypes` builds DWARF in
which an inlined function's out-of-line copy names its parameter and local
only by `DW_AT_abstract_origin`, and a variable is defined only by
`DW_AT_specification` of its declaration. It requires the declarations' types
for the signature, the authenticated record parameter, the local's width and
the data object's size, where the loader used to guess `int32_t`.

`MapFileTest.*` holds every safety fixture's linker MAP against its image, all
six reading as consistent, changes a section's size or a link.exe build stamp
to see another build refused, and reads GNU ld's file-aligned PE section sizes
and IDA's segment:offset MAPs. `LoadDebugInfoTest.*Map*` checks that a PDB
outranks the MAP beside it, that a named MAP fills only what the PDB leaves
unnamed, and that a MAP of another build is passed over beside the binary and
refused when named unless it is taken unchecked.
`DebugInfoDiscovery.FindsTheCompanion*` finds DWARF companions by build ID under
`NEVERD_DEBUG_FILE_DIRECTORY` and by `.gnu_debuglink` and its CRC, from the
fixtures `unittests/debug/fixtures/Makefile` rebuilds;
`LoadDebugInfoTest.ANamedDWARFFile*` refuses another build's or machine's file
and `CompressedDWARFSectionsReadExpanded` reads zlib-compressed sections.
`SessionCAPITest.DebugFilesLoadedAfterTheImage*` is the workbench flow: the
open loads the MAP, and a PDB loaded afterwards replaces its names.
`NeverDWorkerMapReference` runs the MAP and DWARF worker operations against the
real engine, and `NeverDGuiController` asks the MAP question (the same build,
declined, another build), asks nothing about it after an accepted PDB, and
keeps the answer with the project.

`SessionCAPITest.PrepareFunction*` checks analysis without source emission,
restricted and whole-image scope, repeated and switched entries, address zero,
unloaded sessions and unknown ARM mode. `NeverDWorkerSourceCache` verifies
preparation does not render discarded C and that a failed function switch
requires real preparation before the preceding function's CFG is read again.

`PipelineOutcome.AnalysisCache*` compares cold and retained call chains in
one- and four-thread runs, including physical floating returns and integer
argument/register summaries. It checks exact boundary/verified-entry changes,
explicit reset after byte edits, separate images and whole-image exclusion.
`SessionCAPITest.AnalysisCache*` checks navigation without reusing old output
IR, same-address image replacement, failed reload, function edits, target
policy, signature matching and feature changes between selections. Run these
with `NeverDPipelineOutcomeTests` and `NeverDSessionCAPITests`; the no-return,
x87 and jump-table suites cover their shared proof consumers.
`SessionCAPITest.RecoveredExtentsKeepFunctionNamesAndLaterRenames` checks public
size publication, name lookup and a later analyst rename after navigation.
`PipelineOutcome.Warm*` checks that idle preparation leaves selected runs
unchanged, keeps a cache built for other entries, and stops at the cache bound
without evicting. `NeverDWorkerWarmFunctions` checks the worker's `warm` open
option against the mock engine: off by default, a running pass, a restart
after an edit, a full cache and an engine without preparation.

`MedDCE.KeepsLivePhiCyclesAndDropsDeadCyclesInEitherBlockOrder` checks register
DCE liveness through cyclic PHIs, dead-cycle removal and sparse identifiers on
x86, x64, ARM32 and AArch64. The module jump-table and Windows continuation
tests cover arbitration after body rebuilds; unchanged bodies may reuse the
same run-local decision, including its budget refusal.

`SessionCAPITest.DataAccessWidths*` compares retained navigation facts with
fresh native decoding, preserves renamed source, and checks changed function
addresses, extents and inventories. It also covers edits, policy/signature
changes, feature changes between selections, failed loads and same-address
image replacement. Width changes discard source using the preceding data names.

With the optional Qt desktop targets enabled, `NeverDGuiController` exercises
the production views against the deterministic worker. Its chooser regressions
replace queued filters and scroll a 20,000-function fixture across more pages
than the dispatcher admits. Functions activation covers Pseudocode, explicit C,
LLVM C and IR windows, multiple windows, pinned views and back/forward history.
`llvmIrNamesGoWhereTheyStandFor` opens LLVM IR between its folded prelude and
epilogue, jumps to the lines defining values, blocks, parameters, attribute
groups and metadata, shows the declaration of a name the binary lacks, resolves
a comment's address, shows the comment over a mirror's declaration on hover,
and sends an image mirror to its first byte where declared,
to the byte its constant offset reaches on a load, and each of two spellings on
one line to its own offset.
Cross-reference cases query the name under the code cursor while assembly is
at a different function, then select an instruction in the caller. They check
the originating window, representation and pin, proven source-line mapping,
the unmapped warning and assembly-origin navigation. Published source names
use their image addresses even when their displayed identifiers cannot be
resolved by name. Delayed lookups are discarded after navigation, function or
representation changes, project closure and hiding the originating window.
The file-drop cases wait for the worker's loader-identification capability and
check UTF-8 filenames through the Windows fixture boundary.
Character-selection cases drag address prefixes, assembly text, C and Go
pseudocode, and ordinary source rows beside a folded prelude in both directions.
They check Ctrl+C and Edit Copy, selection retention on mouse release and right
click, Shift-arrow selection and partial first/last rows across a newline.

The controller also starts a controlled 30-second decompile, proves uncached
function and listing reads complete while it is running, then switches
functions with Tab and checks cancellation without losing staged comments. A
20,000-line source fixture checks event-loop responsiveness and folded
declarations across pages. Concurrent analysis cases check that a slow view cannot block another
function, cancellation only retires its own executor, subsequent pages keep
their dispatcher when a third function queues, external graph snapshots survive
interleaved requests, and opening another project retires both replicas.
Cancelling view reads must preserve a queued project snapshot needed by a
retained external analysis request.
`NeverDWorkerAllocator` checks the process allocator across the worker's
C++ runtime, the shared SDK and libc, including reallocation, over-aligned
objects and cross-thread release. `NEVERD_ENABLE_MIMALLOC=OFF` selects the
platform allocator throughout NeverD; the legacy `NEVERD_WORKER_MIMALLOC=OFF`
option still disables it for the worker alone. Allocation-intercepting
sanitizers suppress mimalloc automatically. Real worker and workbench
integration checks run with the actual linked worker, without `LD_PRELOAD`.

`NeverDWorkerAnalysisSnapshot` verifies read-only replica state,
unchanged owner files, staged comments, signature replay and stale-input
rejection. Database restore coverage keeps pseudocode closed until requested.

`NeverDWorkerCodeEdits` checks declared local targets, image-name replacements,
UTF-8 span remapping, comments and raw string isolation, and rejection of stale,
colliding or shadowed names. The controller edits source, C and LLVM C with a
pinned view and a different hidden assembly location, then checks undo/redo,
save/restart and instruction-mapped comments. Snapshot coverage also verifies
that read-only replicas load these edits without changing durable owner files.
Source-name checks preserve unordered spans, distinguish partial overlap from
complete token containment, keep adjacent spans separate, reject duplicate or
overlapping accepted ranges, and remap every name after repeated renames.

`NeverDWorkerSourceCache` counts actual preparation and source-page calls across
A-to-B-to-A navigation, including a subsequent graph or IR request. It checks
edits, undo, replica restore, project replacement, representation separation and
both document-count and retained-byte eviction. Page checks preserve repeated
references, overlapping names, nested spans and unsorted producer order, including
empty pages after the end of the text. Unordered metadata uses cached numeric
bounds owned by the same immutable document; their vector capacities count
toward its retained-byte limit. Encoded-page count and byte pressure must not
rerender a retained document. `NeverDWorkerProtocol` checks fresh envelopes,
escaped and invalid UTF-8 payloads, and the exact encoded-frame byte limit.
Timing-independent cache tests complement
`source_revisit_bench.py`, which measures complete real source pages.

`NeverDSourceAnchorTests` checks byte-identical HighC/LLVMC emission and refuses
changed-kind, synthetic, ambiguous and mismatched-function observations.
It also checks that statement navigation without library or call-expression
evidence needs only the ordinary HighC render,
preserves definitions and instruction locations, and matches independent marker
proofs across nested statements and multiple functions. Buffered byte positions
cover UTF-8, marker-looking text, nested relocation, suppressed statements and
refusal of invalid or overflowing ranges without partial publication. Private
recordings still reject changed text. Ordinary rendering and output failures
propagate without retrying them as optional mapping failures.
Call-expression maps reject position mode before mutation and use independent
recording; the mapped output must equal ordinary C without leaked delimiters,
and changed expression kinds still lose their anchors. The bytecode observation
test also recompiles and runs the resulting call and pointer-store evidence.
`SourceDialect.Navigation*` checks distinct statement ranges in C++, Rust and
Go, partial slices, conflicting spans and functions shown as C after a dialect
refusal. `SessionCAPITest.DialectNavigationMapsOnlyTheReturnRowAcrossPages`
checks real x64/AArch64 source and explicit dialect pages, unmapped headers and
single-line paging without spreading return addresses across the function.
`SessionCAPITest.CSourcePagesRetainCanonicalReturnAnchors` and the real worker's
native mapping test verify x64 and AArch64 high-VA source rows through small
pages, including return instruction addresses distinct from the function entry.
The controller's `tabFromCodeUsesSelectedInstruction` verifies that clicks and
arrow keys take the assembly cursor along without focus or history changes, and
both Tab directions; its optional native rows use the
same PE/PDB and environment variables described below. Unmapped declarations
exercise the explicit entry fallback. `assemblyAndCodeCursorsFollowEachOther`
moves the assembly within the shown function, to an unmapped instruction and
into another function that must not decompile, then turns synchronization off
and on from the context menu. `pseudocodeNumbersTakeTheBaseTheUserPicks` covers
H's toggle, the context menu's bases and their previews, `#`, undo, and a
refused character constant; `codeEditKeepsThePlaceInALongFunction` checks that
the reload after an edit puts the cursor and the view back on a third-page row;
`codeTextSelectsLikeAnEditor` presses Ctrl+A while pages still arrive and
after the load, drags beyond the bottom edge until the view scrolls past the
first page, and right-clicks inside the selection to keep it for Copy;
`gotoLabelsGoToTheirBlock` double-clicks a goto's label and checks the code
cursor, the disassembly, Back, and that the label's row maps to its block
rather than to the statement the engine's rows give it;
`NeverDWorkerCodeEdits` checks the literal scan
and type-keeping spellings of `neverd/support/IntegerLiterals.h` for C, Rust
and Go. `tabFromAssemblyWaitsForPagesAndUnfoldsTarget` checks
listing jumps, rows beyond the first page, folded operations, secondary address
round trips, cross-function navigation and missing instruction mappings.
`graphRefreshKeepsLatestRequestedAddress` exercises navigation and keyboard/mouse
selection while a replacement graph layout is pending, plus generation/revision
refreshes before the first layout arrives. `graphJumpDuringFunctionResolutionKeepsLatestAddress`
checks that a late function lookup cannot overwrite a newer in-graph jump or
move the hidden listing to the stale destination.

```sh
cmake --build build-gui --target neverd-gui-tests neverd-gui-query-tests
ctest --test-dir build-gui -R '^NeverDGui' --output-on-failure
```

Two optional profiles use a real worker and user-owned input copies. Set
`NEVERD_LARGE_PE_WORKER` and `NEVERD_LARGE_PE_FILE` for the `native-pe` row of
`rapidlyScrollingLargeListsKeepsTheLastPageAvailable`; discovery and table
replies retain a finite 30-second deadline. Set `NEVERD_CODE_NAV_WORKER` and
`NEVERD_CODE_NAV_FILE` for the `native-c` and `native-llvmc` rows of
`functionsActivationKeepsCodeWindow`, using a supported PE with at least three
functions. The Pseudocode profile needs a worker that supports the `source`
representation. Each worker must keep its matching engine and runtime libraries.
The same variables enable `native-source` and `native-llvmc` rows of
`pseudocodeNamesAndCommentsEditTheirSource` with the controlled Windows
`acceptance.exe`/PDB fixture (`sample(int value)`). These rows copy the inputs
to a disposable project, edit the real output through native widgets, and
verify persistence. `NEVERD_CODE_NAV_CAPTURE_DIR` saves window captures.
Run `neverd-gui-tests` directly with the native Qt platform to exercise native
widgets; CTest sets the controller's platform to `offscreen`.
On Windows, point `QT_QPA_FONTDIR` at `%WINDIR%/Fonts` for offscreen runs,
as desktop CI does. That plugin uses FreeType and needs an explicit font
directory to measure translated text with real glyphs.

`NeverDWorkerCLIDataPresentation` checks CLI/GUI array, alignment and enum
parity against a real engine: identical sidecars, listing display, native IR
isolation, saved type declarations, writer exclusion, resets, failed edits and
malformed SDK item/operand fields, and decimal/explicit-hex CLI and enum values.
Numeric cases include leading zeros, signed/unsigned bounds and overflow. Invalid
replacements retain the previous item or operand format. Run it with
`NeverDWorkerDataPresentation` and `NeverDWorkerFormat` when changing these
presentation operations.

`NeverDWorkerImmediatesRegister` checks decoded immediate search and register
queries through the real SDK/worker and, when available, CLI. It covers search
cache refresh after navigation discovers a function beyond an undecodable gap
and decimal patterns with leading zeros. Standalone GUI builds run the worker
checks without requiring a CLI target.

`NeverDGuiSourceVariableTypes` uses the real worker for type editing and
variable associations, plus decimal, leading-zero decimal and explicit-hex
stack slot/SP edits. It is registered only with
`NEVERD_WORKER_REAL_ENGINE_TESTS=ON`; fixture SDK builds keep it disabled.
`NeverDWorkerSourceVariableTypes` and `NeverDWorkerSourceVariableEdits` also
compile and execute their generated C using the configured C compiler.
When the SDK is built with MSVC, these worker execution fixtures use Clang,
like the native unit fixtures, because generated C uses Clang C extensions.
The same compiler selection applies to the worker stack-frame and call-site
execution fixtures.
Run these with `NeverDSourceVariableFlowTests`, `NeverDHighVariableEditsTests`
and `NeverDLLVMVariableEditsTests`. The HighIR tests preserve instruction
anchors for unchanged statements and refuse old store anchors when a split
replaces a stack store with a scalar assignment.

`NeverDSourceStackFrameTests` covers entry-relative offsets, joins, ordinary
loops, entry-backedge refusal and complete temporary lifetimes.
`NeverDWorkerStackFrame` checks saved stack corrections, both C routes, native
IR isolation, history and replicas. `NeverDWorkerCLIStackFrame` compares the
CLI's `--stack-frame-info` and `--stack-frame` with the worker, compiles and
executes the resulting C, and checks stale/image identities, dynamic SP
recovery, lost storage, invalid options and output preservation. Build `neverd`,
`neverd-worker` and `NeverDSourceStackFrameTests`, then run
`ctest --test-dir build-release -R 'SourceStackFrame|Worker.*StackFrame' --output-on-failure`.

## Source dialects and DWARF ingestion

`IsNoReturnTarget.*` checks cached/live lookup parity for exact IAT precedence,
invalid veneer ownership, continuation/data boundaries, alignment, tagged
Thumb addresses, unregistered x86 veneers and section-symbol aliases.
`ObjectExterns.*` checks direct external tail calls across ELF, Mach-O and COFF
and rejects treating ordinary data/IAT slots as synthetic object callees.
`NeverDWorkerCallSiteTypes` compiles and executes both C routes, including all
five Win64 variadic integer arguments, and retains partial-stack-write refusal.
The engine's C is Clang's dialect (`__builtin_bit_cast`, calls through casts
to a call site's own ABI, which GCC folds into the declared one), so a host
without Clang skips it after the analysis checks, naming the routes it did not
execute.
`NeverDWorkerHeaderRecords` builds a program with the host compiler that reads
`<netdb.h>`'s `struct addrinfo` and `struct servent` through a pointer
`getaddrinfo` stores in a local and through `getservbyname`'s result, and
requires C that names the members; a host without `<netdb.h>` skips it.
`NeverDWorkerProgramMain` strips a program the host compiler builds and
requires the function its start-up code passes `__libc_start_main` to resolve
as `main`, defined `int32_t main(int32_t argc, char** argv, char** envp)` by
HighC and LLVM-C alike; a host without `strip` skips it.
`NeverDSessionCAPITests` (`ProgramMain`) reads the same argument from x86-64,
i386 and AArch64 start-up code.
`NeverDReturnContractTests` requires the shared no-return proof before an absent
ordinary return can erase a function's result, including an external catch
edge. Native pipeline CI executes the complete owner on each compiler.
Run these with pipeline, session and image-boundary suites after changing
shared import or source-call ownership. Native platform skips retain their
own runner requirements.

The main CI matrix installs Rust 1.93.1 and Go 1.27.2, records their versions,
and uses its built CLI to emit HighC and LLVMC for small x64/AArch64 ELF inputs.
This supplies the two optional dialect corpus/file checks without an external
download. Empty or unreadable corpus directories fail. Reproduce those checks
locally after building `neverd` and `NeverDSourceDialectTests`:

```sh
python3 scripts/prepare_source_dialect_corpus.py \
  --neverd build-release/bin/neverd --output build-release/source-dialects
NEVERD_SOURCE_DIALECT_CORPUS="$PWD/build-release/source-dialects/corpus" \
NEVERD_SOURCE_DIALECT_FILE="$PWD/build-release/source-dialects/corpus/x64-c.c" \
NEVERD_SOURCE_DIALECT=go \
  ctest --test-dir build-release -L '^NeverDSourceDialectTests$' --output-on-failure
```

`NeverDSourceDialectTests` checks C++ symbol validation, STL aliases, preserved
custom template arguments, ATL names, and the C exception projection's native
calls and handlers. `CxxOverloadsShareTheirName` reads two numbered overloads by
their one C++ name and keeps C identifiers for an object and a function of one
name and for two functions of one symbol; `ReadsTheDeclarationsHighCWrites`
reads a typedef name declared again, a block's variable of a typedef's name, a
C23 enumeration's type and `va_arg` without showing C, and
`GoViewComputesWhatCComputes` runs a pointer to a copy (`&(int64_t){v}`) in Go.
`SourceAggregate.SourceNamesAreSpelledAsC` checks the C spelling of C++ record
names and of records that share a C library function's name;
`HighCPointerAddresses.RecordsNamedLikeFunctionsAreSpelledByTheirTags`,
`CallResultsTakeNoFileScopeName`, `IntegerOperatorsTakePointerLocalsAsIntegers`
and `IntegersTakeForwardedPointerMembersAsIntegers` check that HighC writes such
a record, a call's result, a pointer operand and a pointer an integer receives
as C a compiler accepts. `CSymbolNames.CopiesOfHeaderRoutinesStepAsideFromTheirNames`
renames a stub and a C++ function named like C library routines and calls the
stub's routine by its name, `HighCPointerAddresses.TrapsUseTheBuiltinsTheirHeadersDeclare`
spells x86 traps for ELF and COFF, and
`AAPCS64CallContract.PLTEntriesPassArgumentsOnAsTheirCLibraryDeclares` declares
bionic's ptrace request an int.
`HighCStoreForwarding.ALandingPadStoresTheExceptionTheUnwinderHandsIt` keeps a
landing pad's store of `eh_exception` after a call that returns and one that
does not. `TypeRender.UnitsTakeTheSystemsOwnTypesFromItsHeaders` takes glibc's
struct stat from its header with checks of its layout and keeps a record of
the name the program lays out its own way, and
`HighCPointerAddresses.RecordsNamedLikeLibraryObjectsAreSpelledByTags` spells
`struct timezone` beside <time.h>'s object.
`PointerLocalsTakeIntegersAsAddresses`, `IntegerResultsReturnPointersAsAddresses`,
`IndirectCallResultsConvertToPointerLocals` and
`PointersToHeaderTypedefsPassThroughVoidPointers` compile the conversions
between pointers and integers that locals, results and a C library routine's
typedef parameters need, and `IndirectCallsTakeNoSignatureFromTheirCaller`
keeps the caller's signature off an indirect call inside it.
`X86_32_DebugParams.ParametersAfterAPointerTakeTheNextSlot` decompiles
`test_debug_params32.o`, built with `-g`, whose parameters follow a pointer of
four bytes. `ReadsTheDefinitionsOfTheProgramsTypes` reads a unit's
local types definitions back in C as written, whole in C++, and in Rust and
Go as far as their structures lay them out, saying why for the rest.
`TheProgramsTypesSpellTheirRecordsAndTheirMembers` gives Rust and Go the
program's types and checks the paths their code reaches members by;
`TypeRender.EmbeddedSourceSaysHowCodeReachesEachMember` checks those paths
as the renderers write them. `SessionCAPITest.RustAndGoPseudocodeDefineTheProgramsTypesAsTheyDo`
reads `unittests/types/fixtures/rust_types.exe`, which reads as Rust and as
Go, through both views; its Rust and Go views compile with rustc and
`go vet`. Decompiled
kernel units read back the same way: `NEVERD_SOURCE_DIALECT_CORPUS` on the
units `scripts/check_decompiled_types.py --keep` writes. `NeverDSessionCAPITests` checks detected defaults, explicit
language pages and emission caches; `NeverDGuiController` checks the C++ title,
switching back to C, and absence of a redundant C choice in C-only images.
`NeverDDebugInfoTests` compares the extent sweep with a pairwise policy oracle,
merges duplicate DIEs across parallel workers, and checks that DWARF references
retain qualified record identity. Existing language/EH and source ABI suites
cover the shared runtime detection and parameter-placement boundaries.

For load benchmarks use Release, the same binary and warm-cache repetitions
with `NEVERD_THREADS=1` and a fixed parallel count. Measure session loading
separately from decompilation. Report algorithmic and parallel gains separately;
thread count alone does not establish faster loading on every input.

## Local types

`NeverDTypesTests` covers the local type library: builtin sizes per data
model, how anonymous unions and structures are rebuilt from MSVC's flat member
lists (each nesting must flatten back exactly), the PDB import against
`unittests/types/fixtures/local_types.pdb`, layout JSON, and the C, C++, Rust
and Go renderers. The fixture's source says how to rebuild the
`local_types.exe` and `local_types.pdb` pair with clang-cl and lld-link.
`NeverDSessionCAPITests` reads the same fixture through the C API.

To check the whole Windows kernel, point `NEVERD_TYPES_NTKRNLMP` at a copy of
`ntkrnlmp.pdb`. `PDBTypeImportKernel.RebuildsTheKernelTypes` then imports it,
and every nesting must hold. `TypeParseKernel.ReadsBackEveryKernelTypeAsRendered`
renders every kernel type as C, Rust and Go and reads it back; each record
must keep its size and every member's bit offset and width, and every
rendered size and offset check must hold.

`TypeFit.*` lands accesses on members (union alternatives, bitfield units,
array elements, padding and bytes outside a record) and ranks records by
them, a pointer into a record and quick-open matching included.
`NeverDMedPointerAccessesTests` gathers the accesses through one pointer from
hand-built MedIR, through copies and constant additions.
`SessionCAPITest.LocalTypesFitWhatTheCodeDoesThroughAPointer` ranks the
fixture's types for `use`'s read of `Device->Size` and keeps a structure
offset in the operands sidecar. `tools/neverd-worker/tests/local_types_test.py`
does the same through the worker with the real engine, the listing's
structure offsets and their undo included, and the GUI controller test
`structureOffsetsRankTypesByTheCodesAccesses` drives T through the picker on
the mock engine.

`NdTypeDisplayPaths.ArrayMembersNameTheirElements` names an array member's
elements as a record names its members: the element an access of its width
reads, the element an address inside the array points at, members of record
elements, and arrays of arrays. Half an element, a wider read of characters
and two arrays overlaying each other stay raw, and a record alternative keeps
its bytes from an array overlaying it.
`HighCPointerAddresses.ArrayMembersPrintTheirElements` prints `Apc->Reserved[1]`
for a store 40 bytes into a declared record like the kernel's `_KAPC`, an
untyped pointer stored to a record pointer member without a cast, and an
integer stored to one with its conversion. LLVM-C names no elements: its
lookups pass `ArrayElements` false.

`TypeParse.ReadsOneFunctionsDeclarationAgainstTheLibrary` reads a function's
declaration against the library without changing it, and refuses one that
names a type the library lacks, defines one, or declares more than one
function. `SessionCAPITest.FunctionDeclarationsBindByAddress` states a
declaration for the fixture's `use` over its PDB's, through the C API:
where each parameter arrives, the records the first parameter's accesses
rank, the declaration Convert to struct * proposes, the refused
declarations, and the sidecar's load. `FunctionDeclarationsNeedNoDebugInformation`
does the same on the image read without its PDB, of a type the user
declares, and checks that another function's text stays as it was.

`TypeParse.SizesAMemberReachedThroughANullPointer` reads
`sizeof(((T *)0)->Member)`, as phnt's `RTL_FIELD_SIZE` spells it, and
`TypeParse.ARoutineTakesItsPrototypeUnderItsAssemblerName` reads glibc's
`__REDIRECT` (`__isoc23_strtol`), musl's `__REDIR` (`__typeof__(stat)`) and
C99's qualified parameter arrays, and
`TypeParse.AnLP64ParameterPassesItsVaListByAddress` each C library's name
for the compiler's `va_list`;
`TypeParse.SizesWideAndUnicodeLiterals` sizes EDK II's `sizeof(L'A')` and
`sizeof(L"A")`,
`TypeParse.ARoutineDeclaredAgainWithAnotherTypeIsRefused` refuses a second
`_ModuleEntryPoint` of another type, and
`TypeParse.ARecordsTypedefRendersBeforeWhatNamesIt` renders EDK II's
`EFI_PEI_NOTIFY_DESCRIPTOR`, whose record holds a function pointer that
names the record's typedef.
`HeaderLibrary.*` reads the embedded libraries and checks how a header
library joins a program's types;
`HeaderLibrary.TheLinuxLibrariesLayRecordsOutAsGlibcDoes` checks the sizes
and offsets clang measures for glibc's `stat`, `timespec`,
`pthread_mutex_t`, `FILE`, `sigaction` and `dirent` on each architecture,
`HeaderLibrary.TheAndroidLibrariesLayRecordsOutAsBionicDoes` bionic's and
the NDK's JNI records,
`HeaderLibrary.TheMuslLibrariesLayRecordsOutAsMuslDoes` musl's,
`HeaderLibrary.TheDarwinLibrariesLayRecordsOutAsLibSystemDoes` libSystem's
and the routines it names by assembler names (`stat$INODE64`),
`HeaderLibrary.TheUefiLibrariesLayOutTheFirmwaresTables` the system table,
the boot and runtime services and protocols MdePkg lays out,
`SessionCAPITest.LinuxProgramsJoinTheirGlibcLibrary` decompiles
`unittests/types/fixtures/linux_libc_calls_{x64,x86,arm64,arm}`, built as
`linux_libc_calls.c` says, and
`SessionCAPITest.AndroidProgramsJoinTheirBionicLibrary` loads
`android_libc_calls_arm64`, a program that asks for Android's linker, and
`android_libc_calls_arm`, a library only the NDK's note marks, built as
`android_libc_calls.c` says;
`SessionCAPITest.MuslProgramsJoinTheirMuslLibrary` loads
`musl_libc_calls_{x64,x86}`, `linux_libc_calls.c` built against musl's
headers as `musl_libc_stub.c` says;
`SessionCAPITest.DarwinProgramsJoinTheirPlatformsLibrary` loads
`darwin_libc_calls_{x64,arm64,ios_arm64}`, built against the headers the
Darwin libraries read as `darwin_libc_stub.c` says;
`SessionCAPITest.AProgramsOwnDeclarationOfAnImportDecides` checks zero-, one- and
four-parameter user declarations through both C backends on those three Darwin
targets as well as Linux x64, i386, AArch64 and ARM32. The Darwin cases first
populate the SDK-bound pipeline to check declaration invalidation too.
`NativeSourceHints.UserPrototypeKeepsExplicitCallSiteAuthority` checks that an
explicit occurrence contract still wins over user and inferred callee summaries
on x64 and AArch64;
`ObjCCallHints.UserPrototypesOverrideOnlyMatchingAutomaticSDKCalls` checks
direct stubs and indirect import slots, unrelated declarations and SDK precedence
over inferred callee signatures on both architectures;
`SessionCAPITest.UefiApplicationsJoinTheirFirmwaresLibrary` loads
`uefi_calls_{x64,x86,arm64}.efi`, built as `uefi_calls.c` says, whose entry
takes `EFI_IMAGE_ENTRY_POINT`;
`SessionCAPITest.CallsThroughTypedMembersReadEachPointerOnce` decompiles
`member_calls_x64` and checks that a call through a typed member's pointer
reads each pointer once;
`HeaderLibrary.TheUserLibrariesDeclareNtdllsNativeAPI` checks phnt's
routines and the PEB, TEB and loader records each user mode library lays
out. The user mode libraries read a checkout of
[phnt](https://github.com/winsiderss/phnt) besides mingw-w64's headers:
`scripts/generate_windows_types.py --phnt <checkout>` (each generated
header names the commit). After running it, check each library in every
language:

```sh
python3 scripts/check_type_renders.py --neverd build/bin/neverd \
  --declare lib/types/headers/WindowsKernelX64.h --data-model LLP64
python3 scripts/check_type_renders.py --neverd build/bin/neverd \
  --declare lib/types/headers/WindowsUserX64.h --data-model LLP64
python3 scripts/check_type_renders.py --neverd build/bin/neverd \
  --declare lib/types/headers/WindowsUserX86.h --data-model ILP32
python3 scripts/check_type_renders.py --neverd build/bin/neverd \
  --declare lib/types/headers/WindowsUserARM64.h --data-model LLP64
```

The Linux libraries read glibc's headers as each architecture's toolchain
installs them: the host's for x64 and x86 (libc6-dev, libc6-dev-i386),
`/usr/aarch64-linux-gnu` for ARM64 and `/usr/arm-linux-gnueabihf` for ARM
(libc6-dev-arm64-cross, libc6-dev-armhf-cross), or another sysroot:
`scripts/generate_linux_types.py --sysroot linux-arm=<directory>`. The
Android libraries read a checkout of bionic
(`--bionic <checkout>`, of https://android.googlesource.com/platform/bionic;
its libc/include, libc/kernel and libc/NOTICE are all they need), with the
NDK's `<jni.h>` from libnativehelper (`--libnativehelper <checkout>`, of
https://android.googlesource.com/platform/libnativehelper) and
`<android/log.h>` from system/logging (`--liblog <checkout>`, of
https://android.googlesource.com/platform/system/logging), and name their
commits. The musl libraries read a checkout of musl (`--musl <checkout>`,
of https://git.musl-libc.org/git/musl), whose headers it installs for each
architecture in a build directory of its own, and name its version. The
Darwin libraries read a checkout of each project `PROJECTS` in
`scripts/generate_darwin_types.py` lists, of
https://github.com/apple-oss-distributions, at the tag each generated
library names, side by side in one directory
(`scripts/generate_darwin_types.py --darwin <directory>`): the libraries
name each project by its tag, so a checkout that is not at one fails. They
need Perl for Libc's feature script, and write
`LICENSES/darwin-headers.txt` again.
`--install-headers <directory>` installs the headers alone and prints what
clang takes to read them for each target, as the Darwin fixtures were
built. The UEFI libraries read a checkout of EDK II
(`scripts/generate_uefi_types.py --edk2 <checkout>`, of
https://github.com/tianocore/edk2; MdePkg/Include and License.txt are all
they need) and name its commit. `scripts/fetch_render_toolchains.py`
fetches what the ARM, Android and musl ones read besides the host's glibc:
the armhf and arm64 cross packages (`--sysroot linux-arm64=<cache>/arm64/usr/aarch64-linux-gnu`,
`--sysroot linux-arm=<cache>/armhf/usr/arm-linux-gnueabihf`), bionic,
libnativehelper and system/logging at the commits the Android libraries name
(`--bionic <cache>/bionic --libnativehelper <cache>/libnativehelper --liblog <cache>/logging`)
and musl's release (`--musl <cache>/musl`), and Apple's projects at the tags
the Darwin libraries name (`scripts/generate_darwin_types.py --darwin
<cache>/darwin`), where `<cache>` is `~/.cache/neverd-render-toolchains`. It
installs Apple's headers as those libraries read them into
`<cache>/darwin-sdk` and records what clang takes to read them for each
library's target.

Check each library with its system library, whose types its own system's
headers give:

```sh
python3 scripts/check_type_renders.py --neverd build/bin/neverd \
  --declare lib/types/headers/LinuxX64.h --data-model LP64 \
  --system-library linux-x64
python3 scripts/check_type_renders.py --neverd build/bin/neverd \
  --declare lib/types/headers/LinuxX86.h --data-model ILP32 \
  --record-abi i386-sysv --system-library linux-x86
```

Every spelling compiles on the library's own system, where the C library's
types come from its headers: the host's glibc for x64 and x86, the fetched
sysroots for ARM, bionic for Android, zig's musl for musl, and Apple's
installed headers for macOS and iOS (`--system-library macos-arm64` and so
on). Its portable
C also compiles on every other system of its pointer width, where the
header defines them: MinGW with mingw-w64's headers, Microsoft's target with
clang's own, macOS and musl with zig's, glibc and bionic. Microsoft's
compilers lay GNU's bit-fields out otherwise, which the checks of glibc's
`_IO_FILE` and `fenv_t` and musl's `fenv_t` report there: those are the
failures the Linux libraries keep. With a system library the prototypes
unit compiles on that system, which declares them.

With `NEVERD_TYPES_NTOSKRNL` and `NEVERD_TYPES_NTKRNLMP` pointing at
ntoskrnl.exe and its PDB,
`SessionCAPIKernelInputs.TheKernelsTypesJoinTheHeadersOfItsDrivers` loads the kernel
and checks that the headers join its PDB without conflicts.
`SessionCAPIKernelInputs.KernelFunctionsDecompileWithTheirPrototypes` decompiles
`NtCreateFile`, `IoAllocateIrp`, `PsCreateSystemThread` and
`IofCompleteRequest` and checks the signatures the headers give them; also
`IoThreadToProcess`, which the linker folded with `PsGetThreadProcess` so
that the PDB names neither, and `FsRtlAreNamesEqual`, exported under another
name than the PDB's. `SessionCAPIKernelInputs.KernelExportsTakeTheirPrototypesWithoutThePDB`
loads the kernel without its PDB and checks `NtCreateFile` again.
Run these external-input checks with `--gtest_filter=SessionCAPIKernelInputs.*`.
Without the two inputs they remain explicit skips. The Linux Lift qualification
requires every `SessionCAPITest.*` case to execute and pass using repository
fixtures; it does not claim Windows kernel input coverage.
`LocalTypesDebugContext.TypesTheFunctionsDebugInformationLeavesUntyped` covers
the conversion and which functions take a prototype without a program.
`LocalTypesDebugContext.TypesTheFunctionsByTheNamesTheyAreShownUnder` covers
the name a function takes its prototype by: its export or a symbol the
program states, else the debug information's, and never the other name of a
function the linker folded with it, nor one analysis gave.
`LocalTypesDebugContext.NamesTheHeaderLibraryAPrototypeComesFrom` checks the
library and the declaration the signature records.
`SessionCAPITest.KnownRoutinesTakeTheirPrototypesByTheirNames` loads
`unittests/types/fixtures/known_routines.sys`, a driver without debug
information whose exports have the names of kernel routines, three pairs of
them folded, one pair returning differently, and checks the signatures and
the `neverd.prototype` comment HighC prints.
`SessionCAPITest.KernelImportsPassWhatTheirPrototypesTake` loads
`kernel_imports.sys`, a driver calling kernel imports, and checks that a call
passes what the prototype takes, a tail call forwarding it, and that the unit
declares the imports as the headers do.
`SessionCAPITest.UserImportsPassWhatTheirPrototypesTake` loads
`user_imports.exe`, a console program calling kernel32, user32 and ntdll,
and checks that it joins the user mode headers and then the kernel's, that
`CreateFileW` takes the parameter its caller forwards as the first of seven,
that `ZwClose` takes the kernel headers' prototype, that a function stops at
`ExitProcess` rather than running into the next one, that an export
reading no parameter shows the one its prototype declares, and that LLVM-C
declares the imports it calls and calls `GetLastError` with no argument.
`SessionCAPITest.X86ImportsTakeTheirStdcallPrototypes` loads
`user_imports_x86.exe`, the same program built for x86, and checks that it
joins the x86 libraries in ILP32, that its externs declare
`__attribute__((stdcall))` with their prototypes, and that a function stops
at `ExitProcess`; it finds `Finish`, which only its export names, although
Start inlines it and ends `.text` at that call; and that `SetFilePointerEx`
takes its `LARGE_INTEGER` in two of its five slots, without the local stored
above them.
`SessionCAPITest.ARM64ImportsPassWhatTheirPrototypesCount` loads
`user_imports_arm64.exe`, the same program built for ARM64 with a call to
`CreateWindowExW` added, and checks that it joins the ARM64 user mode
library alone in LLP64, that `CreateFileW`
takes its caller's untouched x0 as the first of seven, that `GetLastError`
takes no argument, that `ExitProcess` returns to no caller, and that
`CreateWindowExW` takes its last four of twelve from the stack.
`HeaderLibrary.EveryLibraryReadsLaidOutForItsDataModel` reads every library
the build embeds and checks its pointers' and records' sizes, and
`HeaderLibrary.TheNoReturnTablesHoldEveryNoReturnRoutine` checks that the
noreturn tables hold every routine a library declares `_Noreturn`.
`SessionCAPITest.LocalTypesFitWhatTheCodeDoesThroughAPointer` checks that the
program's own record ranks before a header record the accesses fit as
exactly.
`HeaderLibrary.DeclaresTheKernelRoutinesTheCallAnalysisFixes` checks that the
kernel's headers and `WindowsKernelRoutines.inc` agree on the bytes each
parameter of a routine both know takes, and
`ReturnContracts.ACallToAFunctionDeclaredVoidElsewhereHandsBackNoValue` and
`AStubOfAnImportDeclaredVoidReturnsNoValue` cover the return contracts that
read declarations. `known_routines.sys` also checks that `KeSetEvent` prints
the parameters its prototype declares and its code never reads, and that
`neverd_decompile_all` takes the prototypes; the kernel inputs check
`ZwCreateKey`'s seven parameters and that `FsRtlAllocatePool` stops at the
`ExRaiseStatus` the kernel exports without its PDB.
`IsNoReturnTarget.ExportsAndDeclaredPrototypesNameNoReturnRoutines` covers
exports and `_Noreturn` prototypes in the no-return index.
`HighCReturnJoin.BareReturnBesideATailCallReturnsTheParameter` decompiles
`if (b) return g(b); return a;` with a direct and an indirect tail call for
x86-64, i386, ARM and AArch64 (`test_return_join.c`) and requires
`return arg0;` without a trap; on ARM and AArch64 that path is a bare return.
The HighC route of `IndirectTailFrame.ExclusiveReleaseCallsItsVirtualSlotInEveryRoute`
has the same early return beside a virtual tail call.
Build and run `NeverDSessionLLVMTests` as well: its private Session queries
link the actual local-type/debug implementations and shared atomic sidecar
helper into the test executable. It must not acquire a libneverd dependency.
This standalone link includes the real project-input recorder through the
sidecar owner; information panels must not be required to resolve it. Run
`SessionCAPITest.InputSha256IsTheHashOfTheLoadedFile` and the existing sidecar
persistence/failure tests against the shared SDK when changing these owners.
`SidecarWritesRecordInputAndKeepAMatchingManifest` checks that saving a sidecar
records the real input, public queries agree, and recording the same input
preserves other manifest fields and bytes.
`scripts/compare_prototypes_with_ida.py` compares the signatures NeverD
decompiles with the prototypes an IDA dump shows; it decompiles each
function by address and resolves IDA's typedefs through the shipped headers:

```sh
python3 scripts/compare_prototypes_with_ida.py --neverd build/bin/neverd \
  --binary ntoskrnl.exe --pdb ntkrnlmp.pdb --ida prototypes.json
```

Declared types are covered at each layer:
- `SessionCAPITest.LibSystemsTypesComeFromApplesHeaders`: a macOS
  program's header takes libSystem's types from Apple's headers on macOS;
  `TypeRender.TheWindowsRuntimesRecordsStayMinGWsUnderMinGW` the Windows C
  runtime's records a Windows program joins;
- `SessionCAPITest.TheCLibrarysTypesComeFromTheSystemsHeaders`: the DWARF
  of `dwarf_libc.{x64,x86}.o`, built as `dwarf_libc.c` says, whose C header
  includes glibc's headers for glibc's types where the compiler has them;
  `TypeRender.TheCLibrarysTypesComeFromItsHeadersOnItsSystem`,
  `TypeParse.HeaderPragmasNameTheSystemHeaderOfTheTypesAfterThem` and
  `TypeRender.I386RecordsLayOutAsI386OnEveryCompiler` the parts;
- `SessionCAPITest.MinGWProgramsKeepTheX87LongDouble`: the DWARF of
  `mingw_types.{x64,x86,msvc}.exe`, built as `mingw_types.c` says, read under
  MinGW's record ABI on x64 and x86 and under Microsoft's for clang-cl's,
  with the Windows libraries joined;
- `SessionCAPITest.DeclaredTypesJoinThePDBsAndStayWithTheProject`: declaring,
  replacing a PDB type in place, problems with their place, removal, and the
  declarations coming back with the next load, a PDB-less load reporting the
  one that no longer reads;
- `NeverDWorkerLocalTypes`: the worker's commands, the sidecar and the
  analysis replica;
- `WorkbenchTests::localTypesAreDeclaredEditedAndDeletedByIdasKeys`: the
  editor under IDA's Ins, Ctrl+E and Del.

Rendered source must also compile with each record's size and offsets checked.
`scripts/check_type_renders.py` runs `neverd types --all --asserts` and
compiles the result:
- C and C++ in all three builtin spellings with clang for the MSVC target, and
  the portable spelling for the GNU target too; the Windows spelling for both
  Windows compilers, MSVC's and MinGW's, whose long doubles differ; the C
  header, which the workbench shows as C/C++, compiles as C++ as well. Under
  `--record-abi mingw-x64` or `mingw-x86` the program's compiler is MinGW's
  and the other one MSVC's;
- Rust with rustc in both of its spellings, exact widths and Windows' names
  as aliases, for the program's target (`--target`, `--emit=metadata`, which
  needs the target's standard library and no linker);
- Go with `go vet` for Windows, in both spellings too;
- Python by running the module with `-I -W error`, in both spellings, where
  ctypes checks each size and offset, on a Python of the program's kind: the
  host's for 64-bit layouts, i386 Linux's for i386 System V and Android's
  i386, 32-bit ARM's under qemu for the AAPCS, 32-bit Windows' under Wine
  for Windows x86;
- JavaScript by running the module with `node`, in both spellings, whose
  checks read and write back every accessor within its record, and by
  `tsc --noEmit --allowJs --checkJs --strict`, which checks its JSDoc as
  TypeScript (`--tsc` names the compiler; without one it is skipped);
- Lua by running the module with Lua 5.3 or later, in both spellings, whose
  checks do the same (`--lua` names the interpreter, `lua5.4`, `lua5.3` or
  `lua` by default).

A missing toolchain is reported and skipped. The 32-bit and cross ones come
from `scripts/fetch_render_toolchains.py`, which fetches them without root into
`~/.cache/neverd-render-toolchains` and writes `toolchains.json` there for
the checker (`--toolchains` names another directory): rustup with the
standard library of each cross target; Python for i386 Linux from Debian's
i386 packages, which runs on the host's 32-bit C library; Python for 32-bit
ARM and 32-bit Windows from python-build-standalone, run by qemu-arm against
the armhf C library of Debian's cross packages and by Wine; the C
libraries' headers of armhf and arm64 Linux and of Android (bionic, at the
commit the Android libraries name) and the NDK's other headers and musl's
sources, which their generator reads too; zig, whose C compiler holds
macOS's and musl's headers; Lua 5.4, built from lua.org's sources; and
TypeScript's compiler, from npm. The checker takes a Lua or a `tsc` it
finds first, and these where it finds none. It needs an x86-64 Debian or
Ubuntu host with dpkg, apt-get, Wine, make, a C compiler and npm; a part it
cannot fetch is reported, and the checker skips what it would have checked,
naming the script. Without
`--binary` it checks the fixture:

```sh
python3 scripts/check_type_renders.py --neverd build/bin/neverd
python3 scripts/check_type_renders.py --neverd build/bin/neverd \
  --binary /path/to/ntoskrnl.exe --pdb /path/to/ntkrnlmp.pdb
```

Decompiled units must compile with the records they name.
`scripts/check_decompiled_types.py` decompiles kernel functions in HighC and
LLVM-C (`--route` picks one) and compiles each unit for the Windows target
with clang and mingw-w64's headers, as errors for int-to-pointer and
incompatible pointer conversions: a unit that reads members of a record it
leaves incomplete, or passes a value C does not convert to the type of the
member or parameter it reaches, fails:

```sh
python3 scripts/check_decompiled_types.py --neverd build/bin/neverd \
  --binary ntoskrnl.exe --pdb ntkrnlmp.pdb
```

Without the kernel, the fixture checks a function its PDB types:
`--binary unittests/types/fixtures/local_types.exe --pdb
unittests/types/fixtures/local_types.pdb --func use`.

`LocalTypesDebugContext.GivesTheDebugInformationsRecordsTheirMembers` covers
the members the context gives, the definitions a unit holds and what a
dropped context takes back. `SessionCAPITest.FunctionsHoldTheRecordsTheirPDBTypes`
covers the same through a session on the fixture in both routes, a renamed
member reaching the pseudocode among them, in every build;
`SessionCAPIKernelInputs.KernelFunctionsHoldTheRecordsTheyName` (with the
kernel's environment variables) covers `KeInitializeDpc`, the kernel's
unions and LLVM-C's typed signatures.

## Scalar x86 floating-point state

`NeverDX86X87IntegerLoadTests` compares original `FILD m16/m32/m64`
with Codegen, HighC and LLVMC at default/NoOpt and C O0/O2. Raw binary80
values, complete output guards, control words and defined C1 cover all RC,
defined PC and exception-mask combinations. Concrete checks execute every
16-bit input and wide power-of-two boundaries; pure conversions preserve FP
state and padding. Pending/unknown waiting state, exact memory extents,
malformed metadata, and all-path empty-destination admission receive negative
controls, including branches, balanced/unbalanced loops and unknown calls.
A function entry starts from the calling convention's empty register
stack with undefined slot values; an unknown or occupied FILD destination
elsewhere rejects complete function publication. Native oracles require x64
GCC/Clang. These checks do not certify complete x87 TOP/tags/status delivery,
stack-overflow emulation, native exception/memory-fault priority, or native
Linux/macOS/x86-32 execution. `NeverDLLVMCIntrinsicSemanticTests` additionally
executes scalar `llvm.abs` projections through 64 bits at C O0/O2 with undefined
behavior traps, including the defined signed minimum and single evaluation.

```sh
cmake --build build-release --target NeverDX86X87IntegerLoadTests NeverDLLVMCIntrinsicSemanticTests
ctest --test-dir build-release -L '^(NeverDX86X87IntegerLoadTests|NeverDLLVMCIntrinsicSemanticTests)$' --output-on-failure
```

`NeverDX86X87SignAccuracyTests` compares original FABS/FCHS byte sequences
with Codegen, HighC and LLVMC at default/NoOpt and C O0/O2. Full binary80
images and guards cover precision/range beyond binary64, signed zeros,
subnormals, infinities, NaN payloads and noncanonical encodings. FXAM seeds
C1 before sign changes; observers retain only the subsequently defined C1.
The concrete matrix compares every sign/exponent word against original
instructions with zero/full significands. Contract checks cover physical
slots/padding, malformed and unknown pure values, pending waiting boundaries,
and all-path tag admission across branches and loops in ELF/Mach-O/COFF.
Native checks require x64 and GCC/Clang. This does not certify complete x87
TOP/tags/status delivery, general arithmetic precision, memory-fault priority,
or native Linux/macOS/x86-32 execution.

```sh
cmake --build build-release --target NeverDX86X87SignAccuracyTests
ctest --test-dir build-release -L '^NeverDX86X87SignAccuracyTests$' --output-on-failure
```

`NeverDX86MaskCompareStateTests` compares twenty legacy/VEX scalar/packed
register/memory forms with original bytes, Codegen, HighC and LLVMC at
default/NoOpt and C O0/O2. One original function executes all eight legacy or
32 VEX predicates, retaining each raw mask and MXCSR separately. Full YMM
destination/source observations distinguish scalar preservation, VEX clearing
and inactive lanes; all six defined arithmetic flags and complete output guards
are retained. Signed zeros, subnormals, infinities and quiet/signaling NaNs
exercise RC/DAZ/FTZ/sticky environments. Discarded masks, mixed memory/register
helpers and ignored immediate bits receive separate execution probes.
The concrete matrix imports actual completing states; faulting combinations
use masked native exception classification and separate native fault children.
Children cover invalid, denormal, combined cross-lane status, source-page faults
and legacy packed alignment faults before output. Page-end and actual Windows
GS sources verify exact extents. Decoder checks retain every imm8 and physical
register role, reject metadata mutations and distinguish scalar CMP from string
CMPSD. Low/Med/High shapes, raw provenance, owned assembly and immutable scalar
homes in ELF/Mach-O/COFF are checked. Native observers require x64/Clang/AVX;
fault contexts require Windows/Linux and native GS requires Windows x64.
These checks do not certify native x86-32, AVX-512 hardware, enabled #AC or
unavailable-feature faults. Existing EVEX comparison checks remain software/IR.

`NeverDX86ComiStateTests` compares the sixteen legacy/VEX COMISS/UCOMISS and
COMISD/UCOMISD register/memory forms with Codegen, HighC and LLVMC at
default/NoOpt and C O0/O2. Observers retain all defined arithmetic flags,
MXCSR, both complete source YMM values and output guards. Signed zeros,
subnormals, infinities and quiet/signaling NaNs exercise all RC/DAZ/FTZ/sticky
combinations. The concrete matrix additionally checks invalid/denormal mask
controls; normally completing states execute on hardware, while faulting
controls use masked native exception classification and separate fault children.
Native children retain exact outgoing exception status and uncommitted output.
Protected page ends distinguish complete scalar reads from partial faults;
Windows private GS sources retain the actual base. Contract checks cover raw
physical XMM roles, metadata mutations, Low/Med/High shapes, owned assembly,
memory permissions, canonical addresses and unknown state. The accompanying
`MedCallingConvFPStack` test covers immutable scalar homes in ELF/Mach-O/COFF.
These native observers require x64/Clang/AVX; fault contexts use Windows/Linux,
and the native GS observer requires Windows x64. They do not certify x86-32
native, EVEX COMI/SAE, enabled alignment checking or CPU-feature faults.

`NeverDX86X87ConstantTests` compares all seven x87 constant-load encodings
with native Codegen, HighC and LLVMC at default/NoOpt and C O0/O2. Each form
checks the ten raw binary80 bytes, complete destination guards and the outgoing
control word for every RC, defined PC and exception-mask combination. Separate
incoming and internally loaded controls detect lost FLDCW commits. The concrete
matrix uses independent original-byte instructions, including masked sticky
precision; malformed shapes, unknown control and pending waiting boundaries
refuse without publishing a result. Owned assembly retains exact selectors,
pointer types, side effects and clobbers. C uses raw eighty-bit storage even
where the host long double is only binary64. This is numerical/control-word
coverage, not complete source TOP, tags, condition codes, exception delivery or
pending-exception versus source-memory-fault ordering. Native checks require
x64 and Clang; x86-32 has contract checks only.


`X87CallStack` tests call/consume loops and forwarding chains on x86/x64 in
ELF, COFF and Mach-O. Negative cases cover inconsistent returns, recursion,
depth exhaustion, uninitialized/popped slots, resets, tag invalidation and
opaque state restoration. Builder reuse after an image edit invalidates old
proofs. `ImportCalleeTrace` additionally checks partial pointer writes and
instruction-local temporary identity on x86, x64 and AArch64.
`PipelineOutcome.SelectedX87WrappersPreserveReturnsAcrossParallelBuilds`
compares serial and parallel LowIR analysis of 32 selected wrappers on x86 and
x64. Each wrapper has no local x87 instruction and calls the same forwarding
chain; every call and return must retain its physical 80-bit result. The
register summaries must also agree across both schedules.

`CallRegisterBodyCache` compares every machine operation, instruction boundary
and ordinary/exceptional edge against an independent ordinary build. It covers
ELF, COFF and Mach-O, depth-sensitive no-return disagreement, local x87/MMX
state, floating call returns, incomplete input, exact context/opt-in rules,
parallel readers and retained-entry eviction. Existing x87 proof-budget sweeps
also run with body retention enabled; cached blocks grant no larger proof
allowance. Full pipeline checks still exercise format-derived arguments and
register/stack summaries through their ordinary consumers.

`CompleteFunctionCache` compares reconstructed default functions with cold
builds, including address-taken roots, instruction coverage, relocation
provenance, jump-table storage and exact selector/load occurrences. Exception
records retain nested language data and remain owned by each result after the
image and cache are destroyed. Names are rebound and output mutations cannot
alter later results. The body-cache cases also exercise complete-function
refusal for floating returns, local x87 state, incomplete input, changed
contexts and depth-sensitive no-return answers, plus replay exceptions,
concurrent readers and byte/entry eviction. The byte-limit case also keeps a
working set whose live bodies fit the allowance, so construction slack cannot
silently replace useful cached functions.

`X87CallGraphCache` compares independent builders with and without shared
graphs, including exact remaining proof budgets, recursion-depth failures,
changed image and analysis contexts, concurrent queries and bounded eviction.
Complete instruction graphs rejected for exceptional edges or unresolved
terminal dispatch retain only an immutable rejection marker: tests require
actual warm hits, identical cold/warm budget consumption and a fresh successful
proof when the same address belongs to a different valid image. Truncated
instruction streams remain unretained.

Publishing an additional code reference may retain the pipeline's raw x87
graphs, but cannot retain an earlier tag proof. A shared-cache test publishes a
new entry before FXAM: every raw-query budget still matches a cold query, while
ordinary warm and cold builds both reject the newly unknown tag state.
`InternalNoReturn` checks that termination-only builds avoid querying unused
floating-return callees, restore binary80 call/return definitions on builder
reuse, and preserve the full path for local x87 operations, resets and FXAM.
Returning, terminating and unresolved exception handlers retain their original
no-return verdicts on x86 and x64. A function the user states never returns
reaches its callers' callers to the proof depth, even through callees that
hold an unreachable `ret`, but not around a path that still returns; the
screen sends a callee to the proof only when each of its returns follows such
a call, so one that calls the function on an error path costs no proof. A
tail jump into a function that is stated, named or proved never to return
ends its path, and a statement that the target returns decides over both.

The concurrent graph tests hold a builder at a real prover callback and observe
registered waiters before releasing it. They check one construction for matching
inputs, independent proof budgets, exception and incomplete-lift wakeups,
different contexts, and reentrant callbacks across threads and caches. These
overlaps are coordinated by conditions rather than assumed from timing.
Independent call-proof tests hold two distinct callee builds at their real
no-return callbacks, require both to enter before release, and compare complete
LowIR with a serial build on x86/x64 in ELF, COFF and Mach-O. Injected failures
must return only after all workers join. An unregistered callback stays on its
original thread even when the caller supplies a parallel quota.

`X86_32_X87FPU.NativeAndLiftedCallLoopsReturnTheIndependentSum` compares native
machine bytes, generated LLVM, HighC and a separately generated C caller linked
to native callees. Linux x64 hosts execute both i386 and x64, default/NoOpt
pipelines and compiled O0/O2. Its binary80 sum retains bits below double
precision, so a premature conversion cannot pass. These checks cover return
transport and do not certify every x87 instruction, rounding mode or exception.

`X87Examine` checks the all-path slot-tag and payload proof across x86/x64 in
ELF, COFF and Mach-O, including branches, pops, independent entries and opaque
calls. `X86_32_X87FPU.ExamineClassificationMatchesNativeExtendedEncodings`
compares raw machine bytes with LLVM and HighC on 70 binary80 encodings,
including unsupported encodings and the sign retained after FFREE. Linux x64
hosts execute i386 and x64, occupied and empty slots, default/NoOpt pipelines
and C/LLVM O0/O2. An unproven slot or retained payload remains unsupported;
this does not establish floating-point exception delivery or environment
restore coverage.

`NeverDX64MemoryUpdateTests` also checks the SSE2 word transfers that Clang 21
can emit for scalar structure comparisons. `X64WordLane` uses independent
`PINSRW` and `PEXTRW` encodings across the available KVM, WHP, HVF and Unicorn
checked backends. It verifies all eight lanes, masked imm8 indices, extended
registers, ignored REX.W, two-byte memory reads, page faults and retry,
observer cancellation, unchanged FLAGS/MXCSR and register preservation.
The [Intel instruction reference](https://cdrdv2-public.intel.com/825760/325383-sdm-vol-2abcd.pdf)
specifies these SSE2 forms separately from the SSE4.1 extraction encoding;
MMX, VEX/EVEX and SSE4.1 neighbors remain negative controls. These instruction
tests are independent of the compiler version used to build process fixtures.

`NeverDX86FPStateAccuracyTests` compares original scalar SSE byte fixtures
against actual NeverD Codegen objects and standalone HighC/LLVMC source.
Default/NoOpt IR and C O0/O2 cover all four rounding modes, DAZ/FTZ, seeded
sticky status, signed zeros, subnormals, infinities and different NaN payloads
in both operand orders. Complete memory and MXCSR observations also cover dead
numerical results, repeated operations, branches and loop joins. Bounded
native children verify unmasked divide-by-zero before numerical output is
committed. The concrete evaluator runs the same scalar state matrix against
an independent native SSE oracle; malformed contracts and unknown callee state
must refuse. Standalone source also checks typed state bits, helper-name
collisions and unaligned state memory at C O0/O2 with UB traps.
Actual scalar C return observers verify numerical bit patterns and MXCSR,
including NaN payloads and signed zeros, at both source optimization levels.
MedIR controls distinguish a complete numerical slice from the whole state
aggregate, status bytes, a partial slice or a narrowed copy. The accompanying
`NeverDSysVCallContractTests` execute internal and loader-bound math calls,
including mixed FP/integer parameters, while retaining mixed-aggregate evidence
for unknown imports and same-name local functions. Windows uses Clang's own
dynamic ASan runtime beside each private test executable; both address and
undefined-behavior checks remain enabled.
Native execution requires an x64 host and Clang; the in-process SSE oracle
additionally requires GCC/Clang. Skips on other hosts are explicit.
This suite does not establish packed FP, VEX or x87 state coverage.

`MedCallingConvValueFlow.VectorParameterBanksRespectTheImageABI` checks entry
vector arguments for x86, x64, ARM and AArch64 across ELF, COFF and Mach-O.
`MedABIPass.IndirectFPSetupUsesOnlyTheTargetArgumentBank` checks the outgoing
bank on x64, ARM and AArch64. Win64 admits four floating slots while the other
tested banks retain eight. `Win64CallContract.SavedXmmRegistersDoNotShiftTheFifthParameter`
lifts a raw five-integer kernel that saves XMM6/XMM7 with optimization enabled
and disabled, then executes its LLVM-derived C at O0/O2 with UB traps for
1,024 independent argument tuples. This checks host C execution, not native
execution of every source architecture.
`Win64CallContract.CallersPassWhatAnExportedRoutinesPrototypeDeclares`
(`NeverDWin64CallContractTests`) checks that a routine the image exports,
whose name binds a prototype, reads the parameters it declares, a system call
stub's fifth on the stack too, together with any its body reads past them.

`NeverDFPParameterMaskTests` runs the shared ABI suite and `FPParameterMask.*`.
The mask controls retain live bytes, earlier stores, ambiguous definitions and
unmasked return paths while discarding only fully cleared or replaced bytes.
They cover intermediate D0 transport, D16-D31 scratch registers and both arms
of an FP return join. `NeverDARM32ObjectImmediateTests` runs the five ARM32 host C
differentials separately; the Windows EH CI lane runs these cases with its
Clang 18 toolchain, including NEON zip/mask shapes different from Clang 23.
The shared HighC host fixture reports native `-m32` execution unavailable on
macOS, where a 64-bit build would truncate the kernels' addresses. Linux and
Windows retain the actual 32-bit differential. Native x87 IR inputs use
`LLVMHostFixture.h` at both host compiler boundaries; the original verified
module and semantic proofs remain unchanged. The compiler copy lowers
whole-object lifetime markers to the older `(i64 size, ptr)` signature, using
the actual static allocation size or `-1` for a variable-sized allocation.
The Clang 18 O2 execution remains required; it must not consume the newer
one-argument intrinsic with verification disabled. The same copy spells
binary80 hexadecimal literals with the older `0xK` prefix while retaining all
80 bits. LLVM tokenization keeps identical-looking
names, labels, inline assembly and metadata strings unchanged.
The source dialect bit-field differential uses its native C oracle's record
ABI: Microsoft on Windows and Itanium on the 64-bit System V and Apple hosts,
with independent layout
assertions before the Rust/Go byte comparisons. Both record ABIs' actual
generated Rust/Go views are also read on every host. The C syntax reader
recognizes only the emitted `NEVERD_MS_STRUCT` prefix, including after
`DECLSPEC_ALIGN(N)`; unknown layout macros still fail visibly.
The call controls retain explicitly prepared floating registers for both direct
and indirect calls whose ABI operands have not yet been recovered.

`NeverDX86RdSspAccuracyTests` checks every x86/x64 encoded GPR destination,
full-width disabled preservation, enabled concrete SSP reads, flags, unknown
state, call/mutator invalidation, malformed descriptors in both strict modes,
Med/High shapes and owned LLVM assembly. Native x64 tests authenticate disabled
CET inside the executing child and compare original bytes against Codegen
default/NoOpt and HighC/LLVMC default/NoOpt at C O0/O2, including RSP and typed
pointer returns. Enabled native guest recompilation is outside this coverage.
The standalone LLVMC frame/value controls retain materialized dependencies of
forwarded expressions and execute deep bit assembly with UB traps.

`HighSwiftEmitter` checks typed scalar SSE transports, malformed shapes,
definite local assignment, bounded 96-bit slices and module-wide helper names.
Generated Swift executes at Onone/O with rounding, DAZ/FTZ, sticky status,
subnormals, signed zeros, NaN payloads and exact numerical/MXCSR observations.
Execution markers identify startup and each check boundary. Failed runs retain
generated source, executables, captured output and actual native elapsed time;
setting
`NEVERD_KEEP_SWIFT_EXECUTION_ARTIFACTS` also retains successful local controls.
Compiler and native execution bounds remain 120 seconds and 5 seconds.
A separate bounded divide-by-zero control requires native x86_64 macOS;
Rosetta forces exception masks even for raw SSE, so its unmasked-trap coverage
is unavailable. `SwiftSourceProperties` compiles actual class members with
file-level compiler declarations. `MobileIOSNative` rejects false source,
missing or malformed preamble prefixes and mismatched method identities.
The unchanged `scripts/test_mobile_swift_backend.py` acceptance workflow
recompiles and executes arm64/x86_64 with classic/default fixups, checks all
858 original behavior oracles per variant, and verifies the callable inventory.
Swift execution requires swiftc; other hosts retain explicit skips. Native
Intel hardware, Intel HVF and physical iOS are separate validation surfaces.

`NeverDX86FPConversionAccuracyTests` separately covers signed scalar
CVTSS2SI/CVTSD2SI and CVTTSS2SI/CVTTSD2SI, including their VEX forms. Independent
32/64-bit source and destination widths, register and memory sources, every
MXCSR rounding mode, DAZ/FTZ, sticky status, integer boundaries, NaNs and
subnormals are compared against native instructions. The byte fixtures execute
through bundled LLVM Codegen with default/NoOpt pipelines and standalone
HighC/LLVMC at O0/O2. Complete memory and MXCSR observers cover dead results and
32-bit GPR zero-extension. Bounded native children verify unmasked invalid and
precision exceptions before output stores; the concrete evaluator retains the
old aggregate on failure. Source controls check typed bit transport, allocated
helper names and unaligned state memory with UB traps. Decode/lift controls
reject reserved VEX fields and inconsistent address tails. Supported SIB,
displacement, segment and extended-register controls also verify that
VEX.W is ignored for the supported 32-bit destination in x86-32 mode. A
64-bit integer destination in that mode is rejected at each IR/C boundary.
Direct C return observers verify an integer result and MXCSR after a floating
argument; default HighC and LLVMC emission must retain those results despite
their state effects. LLVM vector argument carriers are passed using their
recovered C types, rather than claiming scalar prototype recovery.
This conversion suite does not certify packed, unsigned, EVEX/SAE or x87
conversion state. Its native byte oracles require x64 and Clang; VEX byte
oracles additionally require AVX.
The pinned Capstone decoder supports the legacy i386 address-override fixture
`67 F2 0F 2D 00`. The encoding suite checks its 16-bit memory addressing;
conversion execution remains covered by the conversion suites above.

```bash
cmake --build build-release --target NeverDX86FPStateAccuracyTests --parallel 4
ctest --test-dir build-release -L '^NeverDX86FPStateAccuracyTests$' --output-on-failure
cmake --build build-release --target NeverDX86FPConversionAccuracyTests --parallel 4
ctest --test-dir build-release -L '^NeverDX86FPConversionAccuracyTests$' --output-on-failure
cmake --build build-release --target NeverDX86FPRoundAccuracyTests --parallel 4
ctest --test-dir build-release -L '^NeverDX86FPRoundAccuracyTests$' --output-on-failure
```

`NeverDX86FPScalarConvertStateTests` compares 24 original legacy/VEX
CVTSI2SS/SD and CVTSS2SD/CVTSD2SS register/memory forms with SDK Codegen and
HighC/LLVMC, at default/NoOpt and C O0/O2. Full YMM/MXCSR observations cover
integer boundaries, precision narrowing, NaNs, all rounding/DAZ/FTZ modes and
sticky flags. Discarded results retain state completion and complete faulting
reads; native exception contexts verify untouched output and flag priority.
Additional probes cover unaligned sources, source/destination/merge aliases,
GS addressing, typed scalar precision returns and declared stack source ABI.
The concrete suite compares
raw bits and MXCSR with native instructions, and separately checks exception
atomicity, active masks, SAE status suppression and independent FTZ lanes in
the shared conversion evaluator. Low/Med/High shapes, LLVM assembly ownership
and changed decode metadata have negative controls. `MedCallingConvFPStack`
checks source extents and preserved controls/state across x64 ELF/Mach-O/COFF.
Native tests require x64; full YMM probes also require OS-enabled AVX.
These tests do not certify native x86-32, AVX-512/EVEX, F16C, MMX, x87,
enabled alignment checking or unavailable-feature faults.

```sh
cmake --build build-release --target NeverDX86FPScalarConvertStateTests
ctest --test-dir build-release -L '^NeverDX86FPScalarConvertStateTests$' --output-on-failure
```

`NeverDX86FPPackedConvertStateTests` checks 48 SSE/VEX register/memory/width
forms of CVTDQ2PS/PD, CVTPS2PD/CVTPD2PS and CVT(T)PS/PD2DQ against original
x64 bytes. Codegen and HighC/LLVMC run at default/NoOpt and C O0/O2, observing
full YMM bits and MXCSR across rounding, DAZ/FTZ, sticky flags, NaNs and directed
boundaries. Narrowing padding, low64 widening carriers, aliases, GS and source
extents are independent controls. Fault probes cover unmasked exceptions,
cross-lane priority, legacy alignment before numerical evaluation and discarded
complete-source protected-page reads. The concrete evaluator compares padded
results and state with native packed instructions; Low/Med/High and owned asm
reject malformed roles, source/state address spaces and unsupported effects.
`MedCallingConvFPStack` checks source-width home initialization without replacing
packed memory completion with scalar parameter reads. Native YMM observers
require OS-enabled AVX on x64. This suite does not certify native x86-32,
AVX-512/EVEX, MMX, unsigned conversions, F16C, x87, enabled #AC or unavailable
CPU-feature faults.

```sh
cmake --build build-release --target NeverDX86FPPackedConvertStateTests
ctest --test-dir build-release -L '^NeverDX86FPPackedConvertStateTests$' --output-on-failure
```

`NeverDX86F16CStateTests` compares VEX128/256 VCVTPH2PS and VCVTPS2PH
register, memory-source and memory-destination forms against native x64 bytes.
SDK Codegen and HighC/LLVMC run at default/NoOpt and C O0/O2. Full numerical,
YMM, memory and MXCSR observations cover every imm8 encoding, immediate/MXCSR
rounding, DAZ, FTZ, NaN payloads, narrowing padding, aliases, unaligned accesses
and discarded results. The concrete oracle exhausts all binary16 encodings
and tests directed and deterministic random binary32 inputs. Low/Med/High,
decoded metadata and owned asm contracts reject altered roles and state.
Protected-page probes retain complete read faults, competing floating/access
faults at invalid targets, and no partial cross-page store. Newly raised
status at a failed cross-page store varies even for identical native
bytes on the same host: this observer checks other state and atomicity, and
the concrete regression requires a subsequent exact MXCSR read to refuse.
Native GS store observers use private allocated targets through the Windows
TEB base. Native Linux/macOS, x86-32, AVX-512 and unavailable-feature faults
remain unexecuted coverage in a Windows-only run.

```sh
cmake --build build-release --target NeverDX86F16CStateTests
ctest --test-dir build-release -L '^NeverDX86F16CStateTests$' --output-on-failure
```

`NeverDX86FPRoundAccuracyTests` compares original legacy/VEX scalar and packed
ROUND bytes with actual SDK Codegen and HighC/LLVMC at default/NoOpt and O0/O2.
Directed controls cover all immediate rounding selectors, ignored high bits,
precision suppression, sticky flags, DAZ/FTZ, NaN payloads, scalar return ABI,
upper lanes, dead numerical results and mixed invalid/inexact lane priority.
Memory probes retain the actual instruction access for aligned, unaligned,
protected-page and cross-page sources. Complete-source failure must not publish
numerical output or add FP flags. Low/Med/High and owned assembly controls
check malformed shapes, address spaces and typed numerical provenance. Software
FS controls cover resolved alignment and i386 offset truncation; native GS
source probes require Windows/Linux and FS probes require Linux. Native ROUND
requires x64 SSE4.1; VEX probes additionally require AVX. These directed tests
do not certify EVEX/SAE, unavailable-feature faults or general x87 state.

`NeverDX86FPApprox12AccuracyTests` covers legacy/VEX RCP/RSQRT scalar and
packed forms with original byte kernels, SDK Codegen default/NoOpt and both C
routes default/NoOpt at O0/O2. Numerical bits, unchanged MXCSR (including
unmasked exceptions), scalar merging and vector upper bits are complete
observations. Memory checks cover legacy packed alignment faults, scalar/VEX
unaligned access, canonical ranges, permissions and FS/GS. Scalar C return
observers check FP type and raw bits. The portable software reference is an
explicit non-strict opt-in: check the architectural error envelope separately
from implementation-dependent RCP tininess and record approximated execution.
Default/strict concrete evaluation refuses unproved normal results; definite
special values remain exact. Native FS requires Linux and AVX probes require
native OS-enabled AVX. These tests do not certify x86-32 execution, enabled #AC,
unavailable-feature faults or EVEX 14/28-bit approximations.

`NeverDX86FPArithStateAccuracyTests` compares original scalar and packed
ADD/SUB/MUL/DIV/SQRT/MIN/MAX bytes with SDK Codegen default/NoOpt and HighC/LLVMC
default/NoOpt at O0/O2. Its 140 forms cover legacy scalar/128-bit packed and VEX
scalar/128/256-bit packed register and memory sources. Directed raw values and
full MXCSR observations cover rounding, DAZ/FTZ, sticky flags, NaN payloads and
complete YMM upper state. Additional probes cover distinct source registers,
destination/RHS aliasing, unaligned scalar/VEX memory, unmasked exceptions,
instruction-wide exception priority, legacy alignment and protected cross-page
sources. Discarded numerical results must retain state changes and faults.
Scalar C return probes separately verify floating type, raw bits and state.
Another 36 forms cover HADD/HSUB and ADDSUB single/double precision, legacy 128
and VEX 128/256 register/memory sources. A native SSE3 raw-bit oracle compares
248,832 concrete masked-completion observations across all rounding modes,
DAZ/FTZ and seeded status, including both source orders and distinct NaN payloads.
Generated-code probes additionally retain discarded results, unmasked invalid
priority, legacy misalignment and full protected sources. VEX 256 native probes
exercise the production feature scan as well as SIMD register allocation.
Declared scalar stack-argument probes compare original execution and readable C
through default/NoOpt and O0/O2, including complete raw results and MXCSR. They
exercise the fifth Win64 or ninth SysV floating argument. `MedCallingConvFPStack`
additionally checks arithmetic, ROUND and APPROX12 incoming ranges across
ELF/Mach-O/COFF and x86-32/x64, two-slot composition, writes, escaped addresses,
packed/straddling home initialization and malformed or segmented refusals.
Low/Med/High and owned assembly tests reject malformed roles, controls, state
pointers, effects and addressing contracts. Native execution requires x64 and
OS-enabled AVX; descriptor checks also cover x86-32 without claiming native
x86-32 execution. These tests do not certify FMA,
EVEX/SAE, x87, enabled #AC or unavailable-feature faults.

`NeverDX86FPFmaStateAccuracyTests` compares 192 VEX FMA3 forms: 132/213/231,
single/double, scalar 128 and packed 128/256, register/memory, four product/addend
sign combinations and both packed alternating forms. Original bytes are
compared with Codegen default/NoOpt and standalone HighC/LLVMC default/NoOpt at
O0/O2. Every comparison retains full YMM output, memory and MXCSR. The concrete
matrix independently executes 5,971,968 native comparisons across rounding,
DAZ/FTZ, seeded status and raw zero/denormal/infinity/NaN values. Child processes
check unmasked invalid/overflow, instruction-wide priority, unchanged output
and discarded protected-source accesses. Malformed controls and owned-assembly
contracts must fail clearly. Typed source stack probes separately cover the
fifth Win64 or ninth SysV floating argument. Native execution requires an x64
host with OS-enabled FMA3. FMA4, EVEX/SAE and unavailable features are excluded.

## x86 invalid encodings and instruction boundaries

`NeverDResumableTrapBoundaryTests` checks that an x86/x64 debugger trap at a
known function end or before another function's entry does not acquire a
synthetic tail call. Sized symbols, code ranges and unwind records supply
independent boundary evidence. Interior debug breaks, adjacent owned fragments,
ordinary fallthrough and explicit tail jumps remain controls. The PE source cases
check HighC and LLVM-to-C with and without optimization, preserving the preceding
returning call and the breakpoint.
Sibling fixtures exercise short, long and prefixed x86 breakpoints, `int1`,
`ud0`/`ud1`/`ud2`, fast-fail, ARM/Thumb `bkpt`/`udf`, and AArch64 traps. They
check detailed and lightweight decode, range-only and entry-only metadata,
interior continuations, conditional ARM paths and Thumb IT state. Ordinary
interrupts remain a negative control. The target also runs AArch64 fast-classifier
parity, including all UDF immediates; its optional real-binary walk needs
`NEVERD_PARITY_BIN` and otherwise skips.
Consecutive-break fixtures include both bounded and unsized functions, long
owned runs, and x86 trap/NOP padding before a distinct entry. Live-return fixtures
place the return value before each resumable breakpoint, so a synthetic
RAX/R0/X0 definition cannot hide behind a later overwrite. Both C routes run
with and without optimization, retaining each repeated break and the returned
constant. AArch64 `brk #0xF000` participates in the detailed/light decode,
function-boundary and source tests; fatal BRK codes remain negative controls.
`ResumableTrapIdentity` distinguishes INT1 from INT3 across both C routes,
x86/x64 and normalized COFF/ELF/Mach-O images. On Linux x64, emitted C at O0/O2
runs against independent raw bytes, checking return values, exception vectors,
signal codes and event order for single, repeated and mixed breaks. The native
check explicitly skips on other hosts; source and LLVM verification still run.
The same identity suite checks repeated ARM/Thumb BKPT with zero, nonzero and
maximum immediates, preserving the actual instruction in both C routes.
It exhausts the 256 narrow Thumb BKPT and UDF codes, the 64 Thumb HLT codes,
and all 65,536 codes of both ARM and wide Thumb UDF. Compiler aliases retain
the exception kind, code, void effect and live return. The division-trap alias
also participates in the boundary matrix; ARM mode discovery and COFF GS-wrapper
tests cover the same
aliases. Wrapper recovery separately checks consistent and conflicting
personality paths after resumable ARM/AArch64 debugger breaks, including wide
Thumb UDF #254.
Generated sources are cross-compiled at O0/O2 in both ARM and Thumb mode and
their object bytes checked, including the 255/256 transition requiring `.w`.
ARMv8 HLT also participates in source, object-byte and boundary checks,
including repeated interior halts and a halt immediately before another entry.
This is a compilation and encoding check, not native ARM execution.
Live and indexed symbol queries distinguish padding guesses from confirmed
names, independent aliases, code ranges and discovered call targets. Full and
selected pipeline runs absorb a guessed entry actually reached after repeated
breaks; a reached cold block can continue beyond its primary range.
`NeverDNoReturnTests` checks that only fatal BRK codes propagate no-return to
callers. `NeverDMachineInstructionTextTests` preserves raw UDF/HLT inline
assembly without replacing its continuation with a non-returning C builtin.
The boundary suite also covers ARM/Thumb/AArch64 NOP padding, owned versus
unowned continuations, malformed/truncated/missing owned bytes, instructions
straddling metadata ends, and cold-fragment ends without separate unwind
records. Calls and traps share the fragment-end controls; adjacent fragments
still continue. Owned NOP suffixes include runs beyond the heuristic lookahead
limit, sized symbols/ranges and fragment-only ownership, with and without a
neighbor entry. Prefix and MPX-disabled x86 NOPs share decoder classification;
REX.B register exchanges, upper-half-clearing copies, PAUSE and 32-bit INC remain
observable controls. Interior NOP runs still reach real code, and an independent
branch into a suffix retains its own ordinary fallthrough. Every partial
multi-byte trap extent, ordinary MOV/CALL extents, and primary/cold-fragment
ownership must retain an exact decode failure across the normalized formats.
Fragment-to-primary resumption remains a cycle.
ARM NOPs cover every ordinary predicate, excluding other hints and the
unconditional encoding space. Detailed/light/native decoding and text-on/off
classification must agree.
Coarse unwind endpoints cannot split instructions or clip NOP continuations;
contiguous owned extents cover a complete instruction while gaps still fail.
`NeverDSessionLLVMTests` checks that rejected LLVM function queries retain the
pipeline's decode-failure address, matching the existing HighIR diagnostic.
Its private frame-preservation dependency is compiled from the same
`AnalystStackObjects.cpp` implementation as the SDK.
`NativeBooleanResultProof` feeds the actual lifted fatal BRK
into normalization and rejects debugger, missing-code and stale-output variants.
`SwiftBooleanProjection` requires the LowIR code to match immutable machine
bytes and rejects a debugger BRK even when it has no CFG successor.

`NeverDX86EncodingAccuracyTests` requires raw illegal LOCK forms (including
memory-source arithmetic and multi-byte NOP) and MOV-to-CS to fail in every
decode route, independently of strict, detail and text settings. Legal
memory-destination LOCK operations, HLE prefix orders and segment moves remain
controls.
UD1 golden encodings exercise complete ModR/M, SIB and displacement extents,
32/64-bit address modes, address overrides, every truncated prefix, and the
15-byte limit, including REX2 map 1 encodings. Detailed, lightweight and lifting
decode routes must agree and leave a following NOP at the correct boundary.
Capstone owns validity and instruction extent; NeverD has no duplicate UD1
length parser or LOCK/MOV-to-CS predicate. UD1 operands remain an
unconditional invalid-opcode intrinsic without ordinary memory or arithmetic
operations; this suite does not select a processor's UD0 policy.

```sh
cmake --build build-release --target NeverDX86EncodingAccuracyTests
ctest --test-dir build-release -L '^NeverDX86EncodingAccuracyTests$' \
  --output-on-failure
```

`NeverDMachineInstructionTextTests` lifts AArch64 `brk` (clang's `#1` and
GCC's `#0x3e8`), `udf`, `hlt`, `hvc`, `smc` and barriers, ARM's `trap`,
`bkpt` and barriers, and x86's sanitizer `ud1`, and checks the operand each
intrinsic keeps and the instruction text both C routes print: beside a
builtin, as an `asm` statement, and as the ACLE value of a barrier's own
option. It lifts AArch64 `mrs`, `msr` (register and PSTATE), `dc`, `ic`,
`tlbi`, `at`, `sys` and `sysl` and checks the field each keeps and that an
MSR or SYS writes no register, prints every system register, SYS operation
and PSTATE field encoding against capstone's listing, and checks the ACLE
and `asm` forms both C routes print.

## LLVMC parameter names and CPU queries

`NeverDLLVMCIntrinsicSemanticTests` compiles and executes cached parameter views,
colliding debug names, narrow arguments and MSVC hidden result/`this` controls
at O0/O2. CPU array tests retain repeated queries, parameter/local collisions,
partial register extracts and cross-block uses. CPUID includes nonzero subleafs;
native XGETBV requires an x64 host with OSXSAVE. Changed assembly, constraints,
types and unrepresented aggregate uses must fail before array projection.

On Windows x64, a single-step exception observer compares original
`CPUID(0,0); INT3; CPUID(7,1)` bytes with emitted C at O0/O2. It observes query
inputs, the query count at the breakpoint, event order and the returned value;
matching only the final return is insufficient. Other hosts skip this Windows
event observer explicitly. Windows standalone C checks select Clang's
compiler-rt so the existing i128 floating conversion controls also execute.

```bash
cmake --build build-release --target NeverDLLVMCIntrinsicSemanticTests \
  NeverDLLVMCValueTests NeverDLLVMCFrameMemoryTests NeverDLLVMCPhiTests \
  NeverDLLVMCVectorTests NeverDLLVMCVoidAnalysisTests
ctest --test-dir build-release \
  -L '^NeverDLLVMC(IntrinsicSemantic|Value|FrameMemory|Phi|Vector|VoidAnalysis)Tests$' \
  --output-on-failure
```

## Owned x64 x87 context

`NeverDX87CapabilityTests` exhausts admitted/rejected encoding and prefix
combinations, checks independent operand/flag/wait fixtures, and prevents
native or typed support from promoting portable, state or proof capabilities.
`NeverDX87CapabilitiesPublicTests` compares unloaded-session C and CLI reports;
Python API tests cover the ownership-safe adapter. These inventory tests do
not establish native instruction or ARM64 portable execution coverage.

```sh
cmake --build build-release --target NeverDX87CapabilityTests NeverDX87CapabilitiesPublicTests NeverDX87ArithmeticTests NeverDX87PortableContextTests NeverDX87PortableEmulatorTests --parallel 4
ctest --test-dir build-release -L '^NeverDX87(Capability|CapabilitiesPublic|Arithmetic|PortableContext|PortableEmulator)Tests$' --output-on-failure
python3 scripts/generate_x87_capabilities.py --neverd build-release/bin/neverd --check
```

Use your configured Release build directory in place of `build-release`.
`NeverDX87EnvironmentTests` checks 32/64-bit scan-to-detail transitions,
effective and cancelled REX prefixes, and both typed and owned-context lifting.
Its emission matrix checks all four environment/state instructions with flat,
FS and GS addresses through LLVM, HighC and LLVMC, compiles emitted C, and
rejects missing or invalid image extents. These checks run on non-x86 hosts;
they do not establish native x87 memory fault behavior.

`NeverDX87ArithmeticTests` checks direct PC/RC rounding, extended exponents,
signed zero, masked overflow/underflow numerical results and invalid domains.
LLVM and Unicorn are independent numerical references; the native x64 test
compares original instruction bits and defined arithmetic status and skips on
other hosts. `NeverDX87PortableContextTests` checks all TOP values, retained raw
payloads and tags, control/exception boundaries and atomic refusal. It records
the fields Unicorn cannot independently qualify. `NeverDX87PortableEmulatorTests`
decodes original x64 bytes through the existing lifter and explicitly executes
the portable slice, including a result distinguishable from binary64, provenance,
memory rejection and external-state refusal. No test enables translation.
See the [x87 contracts](x87-semantics.md) for the meaning of each capability.

The numerical replay protocol retains raw requests, responses and all failures
with source/driver hashes, declared build commands, CPU/backend metadata and
seeds. Build `NeverDX87ArithmeticReplay` for the portable driver. On an x64
GCC/Clang build, `NeverDX87OriginalReplay` executes original instructions. On
Apple ARM64 with Rosetta already installed, the original driver can be compiled
separately without LLVM; it reports Rosetta explicitly:

```sh
clang++ -arch x86_64 -std=c++20 -O2 -DNEVERD_X87_ORIGINAL_REFERENCE=1 unittests/lift/core/X87ArithmeticReplay.cpp -o build-release/X87OriginalReplay
python3 scripts/check_x87_arithmetic_replay.py --portable-driver build-release/bin/NeverDX87ArithmeticReplay --original-driver build-release/X87OriginalReplay --output-dir build-release/x87-replay-run1 --portable-build 'configured Release build and pinned LLVM' --original-build 'clang++ -arch x86_64 -std=c++20 -O2 -DNEVERD_X87_ORIGINAL_REFERENCE=1'
```

Use a fresh output directory for every observation and record the actual build
configuration. A Rosetta pass is not native x86 hardware evidence. Process
timings include startup and I/O and do not establish application throughput.

`NeverDX86X87ConvertStateTests` compares original bytes with native LLVM, HighC and LLVMC at default/NoOpt and C O0/O2. Its fixtures pin the owned context on every host through `BinaryImage::X87ContextOverride`; the integer-load, sign and x87 FPU suites also exercise the typed System V projection. It checks conversions, eight arithmetic forms, flags, MMX/EMMS, full/empty stacks, calls, wrappers, entry loops and independent roots. Four FLDENV/FRSTOR formats cover pending exceptions, tags and retained payloads. All four RC values, three defined PC values and 64 exception masks are exercised where applicable. Direct Low/Med/High contracts reject malformed shapes, forged handles and truncated selectors. Narrow returns retain call effects and reject demanded unknown bits. The host oracle requires x64 and Clang; tests do not establish every instruction, native platform, CPU model, fault priority or typed floating import ABI.

## Inline C memory accesses

`NeverDCMemoryCopyTests` executes HighC and LLVMC output at `-O0` and `-O2`
with undefined-behavior traps. It checks unaligned byte copies, adjacent-byte
preservation, aligned accesses with mixed effective types, pointer copies,
signed narrow loads, expression-store results, single evaluation, guarded and
short-circuit loads, loop reloads, and temporary-name collisions. It repeats the
checks with `UseUnalignedPointers`. `HighCIntegerWidths.*`,
`HighCPointerAddresses.*`, `HighCStoreForwarding.*`, `LLVMCValues.*`, and the
frame-memory, atomic and segmented-memory suites cover the surrounding paths.

`HighX86CallSetup` compiles and executes recovered outgoing-stack arithmetic
through straight-line and nested continuation uses, including 32-bit wraparound.
It also requires unused argument stores and address chains to disappear.

`LLVMCValues.RawMemoryUpdates*` compares emitted C with independently compiled
LLVM at O0/O2 with undefined-behavior traps. It covers constant add/subtract and
bitwise updates at 8/16/32/64 bits, captured wrapped results, unaligned byte
storage and surrounding sentinels. Intervening calls, reads, aliasing stores,
earlier result users, old-value users, changed addresses/widths and volatile or
atomic accesses retain their memory ordering and separate stores. The byte-copy
mode and the unchanged input module are checked too.

`LLVMCValues.SelectedFunctionKeeps*SynthesizedTableStorage` compiles and executes
selected lookup-table output at O0/O2 for x86, x64, ARM and AArch64 source
profiles across ELF, COFF and Mach-O. Execution uses host scalar C for 64-bit
layouts and freestanding i386 C for 32-bit layouts on Linux x86 hosts; other
hosts explicitly skip the i386 execution test. Initializer-dependency coverage
retains forward-referenced storage and function providers while excluding
unrelated globals. `HighCPointerAddresses.ExceptTailUsesOnlyItsPrintedContinuation`
checks except fallthrough, live intervening effects and finally transfers;
the x86 SEH probe also checks shared result-slot identity after frame projection.
`LLVMCValues.NativeBlockAddressesCannotBecomeZeroConstants` rejects native
control addresses in returns, integer casts, address arithmetic and nested table
initializers through full and selected-function emission. The matrix covers
32/64-bit, both byte orders and ELF/COFF/Mach-O profiles. Unsupported symbol
constants also fail explicitly; they cannot become null pointers. This is an
emission refusal contract, not a portable implementation of native resumption.

`X86_64_PipelineE2E.DecompileC_InteriorCodeAddressRefusesWithoutReplacingOutput` checks full and selected-function CLI refusal at both optimization settings, retaining an existing output file. The mixed atomic fixture still requires C emission for every other function. Returning the low bits of an interior code address remains unsupported; LLVM verification alone does not establish defined address-bit observation.

`LLVMCValues.WideConstantsPreserveEveryStoredByte` executes unaligned loads and
stores at eleven widths from i129 through i512, including i224/i384 and partial
final bytes, for both LLVM byte orders at O0/O2 with undefined-behavior traps.
It checks every meaningful bit and the surrounding sentinels. Only unused bits
of a final partial byte are excluded, as permitted by the
[LLVM store semantics](https://llvm.org/docs/LangRef.html#store-instruction).
Widths beyond the supported C carrier fail explicitly.

```sh
cmake --build build-release --target NeverDCMemoryCopyTests \
  NeverDLLVMCValueTests NeverDLLVMCFrameMemoryTests \
  NeverDHighCStoreForwardingTests --parallel 4
ctest --test-dir build-release \
  -L '^NeverD(CMemoryCopy|LLVMCValue|LLVMCFrameMemory|HighCStoreForwarding)Tests$' \
  --output-on-failure
```

## LLVM C object storage

`NeverDMedLLVMDirectDataTests` covers finite native data accesses in pointer-bearing
segments, unrelated code/data relocations, exact byte values, partial overlap,
promotion to a whole mirror, access order, strict pointer-overlap refusal and
the existing mergeable-shard storage contract. Its synthetic images cover
x86, x64, ARM and AArch64 with PE/COFF, ELF and Mach-O storage.

`NeverDLLVMCAllocaTests` compiles original LLVM and generated C at O0/O2 with undefined-behavior traps. Native observations check complete dynamic extents, padded element stride, 16/32/64-byte alignment, independent simultaneous/loop allocations and one evaluation of a count-producing call. Both ordinary and preserved-type C routes retain storage. Unsupported/overflowing allocation shapes and explicit stack save/restore refuse before dead-code removal. These tests validate C allocation behavior, not exact source stack addresses or exception-frame identity.

The same target compares static scalar, pointer, array and packed-record alignment through 128 bytes, global alignment, complete padded-string storage and scalar-global address identity in initializers, returns, calls, selects and pointer-to-integer conversions. Ordinary/preserved-type and whole/selected-function routes are covered where applicable.

## Offline web analysis

`WebWindowsHost.*` qualifies native Windows x64 local NTFS capture, junction
and alternate-stream refusal, replacement, Unicode identity, private unlinked
spools, concurrent range reads and export/rollback ownership. The required
`web-windows-qualification.yml` lane compares selected CTest discovery with
JUnit and forbids missing or skipped required tests. Its independent installed
C++ consumer exercises the extracted DLL/ICU, immutable export, CLI, worker
and MCP with development paths removed. The product release additionally
requires full producer/CEF qualification at the same commit, then publishes
those exact verified bytes. See [build and release details](web-windows-product.md).

`WebDataPack.*` checks both resource-ID widths, complete partitions, aliases,
sorted uniqueness, malformed/truncated indexes, identity and count overflow.
`WebDataPackGzip.*` checks one validated stream, corrupt CRC/size, truncation,
concatenation/trailing bytes and output refusal; the separate no-decoder target
qualifies honest native omission. `WebDataPackSDK.*` checks private canaries,
one-row pages, source anchors, stored/deflate ZIP composition, explicit gzip,
failed-publication atomicity, cache limits, revocation and exclusive export.
CLI cases use an unusable external-tool PATH; worker/MCP compare the shared API
and reject raw-export/execution options. See [resource profiles](web-datapack-profile.md).

Select the fixed real inputs without running the applications:

```sh
NEVERD_CEF_REDIST_12027_X64=/path/cef.redist.x64.120.2.7.nupkg \
NEVERD_TEAMS_WV2_154_PAK=/path/to/the/pinned/resources.pak \
ctest --test-dir build-release/unittests/web --output-on-failure \
  -R '^WebDataPackReal\.'
```

`NEVERD_REQUIRE_CEF_RESOURCE_CORPUS=1` forbids an absent CEF input. The fixed
download and original SHA are in `web-resource-qualification.yml`; its actual
ZIP/resource/JS/HTML case requires native ZIP decoding, ICU 77.1 and the embedded
parser. Local Teams bytes are not redistributed. Shared changes require the
owning Web suite, worker/MCP/availability and parser/backend omission, then
enabled restoration. [Full producer qualification](web-producer-corpus-qualification.md)
separately requires all eleven fixed-image cases with
`NEVERD_REQUIRE_WEB_PRODUCER_CORPUS=1`; missing files or consumers fail instead
of skipping. Ordinary optional-corpus behavior remains explicit.

`WebLargeSource.*` checks explicit profile identities, unchanged interactive
refusal, complete lexical scope beyond 100,000 nodes, late source anchors,
invalid encoding/syntax, dense over-budget input and rejection of recovery
models by semantic consumers. Its optional pinned Claude check uses
`NEVERD_CLAUDE_CODE_21296_CROSS_DIR` and the original NeverD exports of macOS
ARM64 modules 141, 299, 415 and 1452; missing samples are explicit skips.
`WebSDK.LargeSourceHasIndependentCacheAnchorsAndRevisionOwnership` checks the
additive C API, one-source cache, interactive isolation, ordinary pages and
revision revocation. Worker/MCP cover the same operation, capability filtering,
private canaries and source/report/anchor parity. Run the owning Web directory,
availability and transport suites plus both omission configurations after
changing this shared source-profile boundary. See the
[large-source contract](web-passive-report-profile.md#explicit-large-source-selections).
Run CTest invocations sequentially within the same build directory, especially
after changing build flags. GoogleTest discovery writes shared generated lists;
concurrent owning-directory and root runs can duplicate registrations. Check
the unique names in one generated inventory before reporting case totals.
`WebSourceFeatures.CapturedClaudeLargeClientUnitsWhenSupplied` additionally
checks every shipped JS unit in the retained macOS ARM64 package sequentially
through explicit large parsing, lexical navigation, features and configuration
order. It asserts all 2,364 units parse and records independent analysis counts.
The existing interactive corpus case retains its narrower admission.

`WebPackageSemVer.*`, `WebPackageEvidenceTime.*`, `WebPackageEvidence.*` and
`WebPackageAdvisories.*` cover strict arbitrary-width precedence, source/time
admission, OSV union/order/endpoints, wildcard and PURL identity, unsupported
coverage and bounded matching. Set `NEVERD_OSV_NPM_ADVISORY` to the hash-pinned
public record in the [qualification](web-package-advisory-qualification.md).
`WebPackageAdvisorySDK.*` checks original anchors, 600-entry paging, uncomposed
import capacity, aggregate retained bytes and work, atomic failures, cache and
revision invalidation, private canaries and CLI parity with an unusable PATH.
Run the complete owning Web directory and worker/MCP suites after changes to
the session boundary; verify advisory core/SDK/transport checks with JS omitted
and all C stubs with the backend omitted, then restore both flags to ON.

`WebAttestationEncoding.*`, `WebPackageAttestations.*` and
`WebPackageAttestationSDK.*` cover independent Base64 vectors, exact DSSE bytes,
registry conflicts, SLSA chronology, unsupported digests, known SHA-256/SHA-512
byte comparisons, private publication, import/result ceilings, shared descriptor
accounting, canonical digest pages, reset and CLI operation with unusable PATH.
`NEVERD_CLAUDE_CODE_21295_REGISTRY` selects the previously captured metadata
SHA-256 `5b08bcfe0bd28ab796d40388a360e8f5b5d5db672376106887972e0715109578`;
the test verifies two separately retained signature declarations, never their
cryptographic validity. Worker/MCP compare the same metadata and digest receipts
and reject key/verification fields. Use the same omission/restoration matrix.

`WebPackageAssessment.*`, `WebPackageReview.*` and
`WebPackageAssessmentSDK.*` check foreign registry declarations, immediate
archive ancestry, independently computed assessment identities, canonical
selection order, unknown advisory coverage, legacy digest strength, retained
metadata refusal, subject receipt ownership, exact behavior/comparison owners,
source byte anchors, 610-finding paging, head-bound reports, immutable caller
assertions, stale heads and revisions, last-request replay, shared 256-event
capacity, atomic refusal, reset and CLI JSON/Markdown without an external PATH.
Worker and MCP compare assessment/review operations to the shared API and reject arbitrary
notes, safety scores and raw-export fields. Run the owning Web directory and
worker/MCP/ABI checks, then the focused suites with JS omitted and all additive
C stubs with Web omitted. Restore both build flags and rerun affected checks.
The [assessment profile](web-package-assessment-profile.md) documents explicit
selection, evidence relationships, report fields and shared limits.

The additive journal cases also check entire-chain validation, exact-prefix
restoration, zero-charge current-head replay, shared suffix capacity, late
tampering, duplicate/unknown JSON fields and reconstruction after capture reset.
`WebAvailability.*` includes the always-exported restore symbol and discovery.
Use the full Web regression and worker/MCP/ABI checks for this shared mutation
boundary, then JS/backend omission and enabled restoration.

With Qt and `NEVERD_WORKER_WEB_ENGINE_TESTS=ON`, `NeverDGuiWebReview` uses only
C++ fixtures and the production worker. It covers canonical journal persistence,
failed-write preservation, duplicate/byte/record refusal, fixed confidence versus
assertion columns, multi-page inventories, tar/registry ancestry, worker restart,
atomic late-chain refusal, changed-input rejection and RTL/large-font controls.
The optional `NEVERD_CLAUDE_CODE_21296_CROSS_DIR` selects the already pinned macOS
ARM64 Claude platform package; `NEVERD_GUI_WEB_RECEIPT` explicitly writes its
metadata receipt and a local widget PNG for visual review. No sample is executed
or committed. Backend omission skips dependent positive cases and qualifies an
explicit unavailable window; journals/models still work. Run the existing GUI
unit/controller/language and smoke suites when modifying workbench commands.
See the [journal/desktop profile](web-package-review-journal.md).

`WebPassiveStructure.*` distinguishes equal-histogram bodies with different
field nesting, absent versus null fields, mixed array/object parents, private
name identity, endpoint/direction isolation and aggregate private-byte limits.
`WebRequestStructure.*` checks exact template-expression/segment anchors,
lexical path/query roles, direct header/body syntax, unknown dispatch/getters/
spreads/deferred functions, unchanged HAR matching and shared record budgets.
`WebInterfaceSDK.PassiveStructureRetainsFieldsAcrossSingleRowPages` verifies
parent IDs and every field across one-row public pages, including parser-off
builds. Worker/MCP verify the same observed structure with private canaries.
`WebPassiveReport.*` verifies independently counted observed JSON types,
malformed/encoded/missing bodies, aggregate node work, explicit selections and
record refusal. `WebPassiveSource.*` verifies exact transformation/message
syntax, deferred/computed/alias exclusions, all five client domains and selected
MCP method candidates without protocol inference. SDK cases qualify commit
gates, paging, revocation, reports and resource directory/ZIP/native consumers.
CLI cases run with an unusable external-tool PATH; worker/MCP compare the same
backend. `WebDesktopManifest.*` qualifies supplied CEF/WebView descriptor
versions, exact scope, opaque/missing files and list budgets. Run owning Web,
worker/MCP and parser/backend omission when changing these boundaries, then
restore the enabled build. See the [report](web-passive-report-profile.md) and
[resource](web-desktop-resource-profile.md) profiles.

`WebNodeBehavior.*` checks exact builtin import/member forms, aliases, primitive
Buffer overloads, overload-neutral argument anchors, uncertainty and budgets.
`WebPackageBehavior.*` checks direct file membership, unknown ownership, bounded
retained extractor ancestry, foreign namespaces and atomic aggregate refusal.
`WebNodeBehaviorSDK.*` checks 16-source selection independent of the four-result
cache, revisions, private-value canaries, truncated entry lists, archive/Bun/SEA
association and CLI processes with an unusable PATH. Supply the pinned
`NEVERD_CLAUDE_CODE_21296_ELF` and `NEVERD_CLAUDE_CODE_21296_NPM_DIR` inputs for
the two optional real source cases. `WebSourceDeclarations.*` and the class
cases in origins/paths cover the shared declaration projection. Changes to
that shared boundary require the owning Web directory, worker/MCP parity and
parser/backend omission checks, followed by restoration of the enabled build.
See the [Node behavior profile](web-node-behavior-profile.md).

`WebPackageInstall.*` covers command grammar and reserved Node dispatch, exact
entries, mixed namespaces/selections, deferred code, ESM/CJS chains and cycles,
shadowed or written require, unknown sources, independent package ownership,
embedded delivery and atomic budgets. Tar symlinks, encrypted/unsupported ZIP
members and absent files preserve the same honest unavailable boundary.
`WebPackageInstallReal.*` selects the two JS members from the original pinned
Claude npm archive and checks six entry-associated calls against the existing
behavior records. `WebNodeBehaviorSDK.*` also covers optional selection, 512-row
trace paging, canaries, cache/revision invalidation and directory/archive CLI
compositions with an unusable PATH. Worker/MCP test the same metadata; MCP also
checks manifest-only and lock-only coverage. Follow shared-boundary changes with
complete owning Web regression and both omission profiles, then restore ON.
See the [installation trace profile](web-package-install-profile.md).

`WebPackageBehaviorDiff.*` checks changed arguments with equal pattern counts,
per-file deltas that cannot cancel, original occurrence anchors, absent and
unselected sources, parse failure, source modes, root/platform context, embedded
delivery and aggregate limits. The unchanged Buffer call plus the existing
value-budget fixture must produce incomparable coverage, never a fake encoding
delta. `WebPackageBehaviorDiffSDK.*` repeats that counterexample through the
public API, resolves records with an empty independent Node behavior cache,
pages 600 occurrences, checks cache/revision invalidation and the shared
16-source limit, and runs directory/archive CLI composition with an unusable
PATH. Worker/MCP check shared summary/page equality and strict schemas.
`NEVERD_CLAUDE_CODE_BEHAVIOR_DIFF_DIR` optionally supplies the two hash-pinned
`claude-code-2.1.295.tgz` and `claude-code-2.1.296.tgz` wrappers for
`WebPackageBehaviorDiffReal.*`. Follow with complete Web regression, parser and
backend omission checks and restoration ON. See the
[comparison profile](web-package-behavior-diff-profile.md).

`WebDesktopManifest.*` checks explicit NW.js/VSIX profiles, documented directory
layouts, manifest-relative files, URL/command-line boundaries, malformed JSON
and independent metadata/work bounds. `WebDesktopSDK.*` checks distinct profile
caches, revision replacement, directory/ZIP entries, Bun/SEA asset isolation,
redaction canaries and actual CLI selection in single-file/directory/ZIP modes.
The optional pinned VSIX test shares the ZIP environment variable below.
Run these with and without the JS parser, plus C ABI availability and worker/MCP
parity. See the [profile](web-desktop-manifest-profile.md).

`WebZip.*` checks native ZIP32 records, independently preserved libarchive
fixtures, framing/CRC/path/expansion refusals and unavailable-member evidence.
`WebZipNoDeflate.*` compiles the payload owner without its decoder macro;
it is not a whole-engine zlib-free build. `WebPackageArchiveSDK.*` also checks
ZIP consumer coordinates, nested Bun/SEA/HTML origins, atomic failure and CLI
processes with an unusable PATH. Supply `NEVERD_CLAUDE_CODE_21296_VSIX_LINUX_X64`
for the hash-pinned Marketplace ZIP body (after HTTP Content-Encoding decoding),
as documented in the [qualification record](web-claude-code-qualification.md).
These tests never download, install or execute a supplied extension/helper.
Shared path/origin changes require the whole owning Web directory and affected
worker/MCP parity tests. See the [ZIP profile](web-zip-profile.md).

The optional `NEVERD_BUILD_WEB_MCP=ON` target builds a native C++ transport.
Run `ctest --test-dir build/tools/neverd-web-mcp --output-on-failure` for its
four C++ suites: protocol, catalog, real-backend evidence parity and actual
stdio process tests with an unusable PATH. The process suite explicitly skips
on Windows. An additional `NeverDWebMCPClaude` process case requires the pinned
`NEVERD_CLAUDE_CODE_21296_ELF` artifact and otherwise explicitly skips.
Repeat against parser-off and backend-off builds; the latter must
retain capabilities while omitting analysis tools. The runtime-free engine
profile also sets `NEVERD_ENABLE_PYTHON_PLUGINS=OFF`. Shared JSON/client changes
require worker protocol/Web and affected mock transport regressions. These
MCP suites own catalog qualification separately from the existing Python
native capability checker. See the [profile](web-mcp-profile.md).

The worker build also builds `neverd-mcp`, NeverD's native MCP server, which
runs the worker's engine in process. `ctest -L '^mcp$'` runs `NeverDMCP`, the
protocol, tool, result-shape, reading-view and workbench-attachment cases
against a recording backend and a stand-in workbench socket, and
`NeverDMCPProcess`, which launches the executable on the test's own binary with
the real engine and skips on Windows. Changes to the worker engine or its
operations also run `NeverDMCPProcess`.

`WebNexe.*` covers pinned EOF framing, strict first-shim JSON, malformed binary64
lengths, first-sentinel window boundaries, private paths, overlap/alias coverage
and independent member/work/private-metadata budgets. `WebNexeSDK.*` exercises
publication and cache atomicity, stored/deflate ZIP nonzero source coordinates,
two identical occurrences with isolated package namespaces, outer delivery-only
joins, source/bootstrap/native consumers and CLI operation with an unusable
PATH. Set `NEVERD_NEXE_4RC7_CORPUS` for three optional producer-built full images;
missing samples explicitly skip and tests never generate/download/execute them.
Worker/MCP suites verify both operations through the shared C API. Check
parser-off extraction and backend-off ABI/discovery, then restore both options.
See the [nexe profile](web-nexe-profile.md).

`WebPkg.*` independently builds fixed loader/prelude envelopes and covers LF/
CRLF framing, snapshot roots, aliases, partial overlaps, opaque caches, private
metadata, every truncated prefix, compressed-profile refusal and independent
byte/node/count/work budgets. `WebPkgSDK.*` covers root namespace uniqueness,
source/cache and symlink uncertainty, metadata-alias privacy through consumers,
cache/revision atomicity, stored/deflate ZIP source coordinates, package/native
handoff and CLI operation without external tools. Set `NEVERD_PKG_581_CORPUS`
to the pinned six-platform corpus root, including the supplemental
`outputs-stripped` and `stripped-inputs` directories. Tests verify original
container/input hashes and separately preserved BOM/shebang storage evidence;
they never build/download/execute samples. Worker/MCP tests own
direct/framed parity. Check parser-off extraction and backend-off ABI/discovery,
then restore both options. See the [pkg profile](web-pkg-profile.md).

`WebSEA.*` covers independent Node SEA serialization, x64/ARM64 ELF/Mach-O/PE
resource location, malformed names/tables, duplicate mappings, cumulative work,
opaque V8 evidence and exact compiler-generated blobs. `WebSEASDK.*` and
`NeverDWorkerWeb` cover publication/revocation, private-key redaction,
source/map/package/native consumers, nested compressed anchors and CLI/framed
parity. Set `NEVERD_NODE_SEA_22150_CORPUS` for the optional pinned snapshot
directory and `NEVERD_NODE_SEA_22150_IMAGES` for injected full-image fixtures.
Tests never download or execute them. See the [profile](web-sea-profile.md).
After shared artifact/session/package boundary changes, run the owning web
directory, C API availability and five core worker cases. Verify parser-off
extraction and backend-off ABI behavior separately.

`WebStreams.*` covers explicit JSONL/SSE/JSON-RPC/MCP/log profiles, byte spans,
EOF limits, duplicate/missing context, original numeric tokens and aggregate
JSON work including failures. `WebStreamSDK.*` and `NeverDWorkerWeb` cover
preview gates, profile/hash/policy receipts, redaction canaries, cache/revision
revocation and transport parity. Run the owning web directory after changing
shared JSON admission or session boundaries; readers remain enabled without JS.
See the [stream profiles](web-stream-profile.md).

`WebInterfaces.*` and `WebSourceInterfaces.*` cover passive HAR admission,
fixed redaction classifications, missing/malformed fields, source anchors,
shadowing/eval, options inheritance, method normalization, credential and
WebSocket exclusions, exact origin/path candidate joins and fanout budgets.
The v3 cases add non-computed member-fetch receiver anchors and optional calls
without wrapper/API attribution or HAR eligibility. Any direct/member-fetch or
WebSocket spread withholds argument-role nodes and default-method claims. The
optional cross-platform evidence root also selects the exact NeverD-exported
Claude macOS ARM64 module 928 by SHA-256 and verifies its unresolved member
spread at byte 150,734 for 36 bytes. It does not execute the client. Run source,
SDK and worker/MCP cases, the owning Web regression, JS/backend omission and
enabled restoration when changing this shared profile.
`WebSourceWrappers.*` adds v4 single-return lexical fetch forwarding, reordered
caller/parameter anchors, const arrows/functions, shared option/method refusal,
missing/spread arguments, duplicate declarations, shadowing, writes, unsupported
parameters/bodies/aliases, unchanged HAR eligibility and atomic record limits.
SDK and worker/MCP cases check the same metadata-only anchors and canaries.
`WebSourceXHR.*` covers direct/const receiver and argument anchors, shadowing,
aliases/writes, dynamic scope, all-call spread refusal, callback value syntax,
compound-assignment exclusions, missing/forbidden methods and atomic budgets.
XHR remains excluded from the unchanged fetch/HAR correlation profile.
`WebInterfaceRelations.*` adds exact function/return/await/consumer anchors,
lexical alias/shadow/write/computed exclusions, member/then spread uncertainty,
malformed/foreign navigation and independent exact/short graph budgets. The
same optional pinned Claude module retains function and direct-return links.
`WebInterfaceSDK` checks fresh/cached navigation equality, revision revocation,
closed metadata, 128-record pages and 2,050 retained requests whose relation
fanout exceeds 4,096 without destroying request pages. Run the owning Web
directory without a name-prefix filter so parameterized platform cases also
run, then worker/MCP/catalog, both omission profiles and enabled restoration.
See the [relationship profile](web-interface-relations-profile.md).
`WebInterfaceSDK.*` and `NeverDWorkerWeb` verify explicit preview/commit,
hash-bound CLI receipts, revoked tokens/revisions, immutable capture bytes,
cache limits, canary exclusion and direct/framed parity. Run the owning web
directory after changes to these shared session boundaries. HAR remains
available without the parser; source/interface comparison requires it.
See the [interface profile](web-interface-profile.md).

`WebPackageArchive.*`, `WebPackageIntegrity.*` and `WebPackageDigest.*` in
`NeverDWebArtifactTests` cover bounded tar/PAX/gzip admission, shared storage,
strongest SRI declarations, original compressed-byte scope and independent
SHA-384/512 vectors. `WebPackageArchiveSDK.*`, `WebPackageIntegritySDK.*` and
`NeverDWorkerWeb` cover consumer identity, nested coordinates, cache/revision
revocation, native storage lifetime and offline CLI/transport parity. Optional
hash-pinned official Claude Code npm artifacts are described in the
[archive/integrity profile](web-package-archive-profile.md). Run the owning
web directory and `WebBlobStore.*` when changing these shared storage paths.

`WebPackages.*` in `NeverDWebArtifactTests` covers npm v1/v2/v3 placement,
hidden locks, aliases/workspaces, optional/peer rules, declaration conflicts,
field evidence coverage, two-root diffs and metadata budgets. The optional
`NEVERD_NPM_CLI_1190_LOCK` path enables the pinned full npm CLI v11.9.0 lockfile
case described in the [package profile](web-package-profile.md). It performs
no download or installation. `WebPackageSDK.*` and `NeverDWorkerWeb` exercise
metadata pages, stale IDs, private canaries and direct/framed adapter parity.
Run the owning web CTest directory after changes to shared artifact/error or
session boundaries; unavailable parser/compression/host profiles remain explicit.

The Bun profile's C++ cases add preserved compiler graph hashes, synthetic
hostile ELF/graph layouts, source encoding and immutable range checks. Set
`NEVERD_BUN_142_CORPUS` to the pinned full-image corpus to qualify complete
compiler outputs; that case explicitly skips without the corpus. The tests
never build or execute their inputs. SDK/CLI/worker tests verify decoded source
identities and an unusable external-tool PATH. See the
[profile](web-bun-profile.md) and [corpus recipe](../unittests/web/fixtures/bun/README.md).

`WebBun.ClaudeCode21296FullContainerWhenSupplied` checks an independently
downloaded official Linux x64 artifact when `NEVERD_CLAUDE_CODE_21296_ELF` is
set. It pins the original SHA-256/size, extended layout, module/region counts,
entry index and all decoded source totals. No target code is redistributed or
downloaded by tests. Missing evidence is an explicit skip. Synthetic extension
tests cover malformed prelinked indices, counts, aliases and alignment.
`NpmPlatformInputs/WebClaudeDistribution.*` additionally uses
`NEVERD_CLAUDE_CODE_21296_CROSS_DIR` to select eight separately captured npm
platform scopes. Each pins original/member hashes and sizes, compares the
captured registry SHA-512 declaration, checks graph geometry and strictly
decoded source totals. See the
[cross-platform qualification](web-claude-code-cross-platform-qualification.md)
for its exact local evidence layout. The cases do not download, install or
execute packages and do not require the optional JS parser.
`WebPackageArchiveSDK.CLIExport*` checks the explicit archive-to-Bun export
consumer over tar/gzip/stored and deflate ZIP, retained original/expanded
origins, exact recovered bytes, unavailable/non-Bun refusal, existing-output
preservation and metadata canaries with an unusable external-tool PATH.
`WebSourceFeatures` checks the fixed client syntax vocabulary, exact callback
source/conditional-test links, unresolved names/shapes, privacy and independent
feature exhaustion. `WebSDK.ClientFeaturePagesKeepConfidenceAnchorsAndPrivateValues`
checks paging, shared function IDs, revision invalidation and private-value
canaries. Worker/MCP compare the same feature metadata through the existing
navigation C APIs. `WebSourceFeatures.CapturedClaudeClientUnitsWhenSupplied`
uses the cross-platform environment above to inspect all 2,364 JS units in the
pinned macOS ARM64 archive sequentially and records parser/feature coverage,
including exact refusal reasons. See the
[actual client qualification](web-client-feature-qualification.md); it does not
claim complete feature attribution or run a target runtime.
`WebConfigurationFiles.*` checks minified lexical source candidates, exact
file/layer literal and switch-case anchors, unknown array elements, ambiguous
bindings, foreign evidence and atomic record/term exhaustion. The SDK, worker
and MCP checks compare `config_files` paging and private-value canaries through
the existing navigation operation. Both captured-client cases also record this
independent profile's sites, calls and layer sequences for every admitted unit;
all admitted units must complete the inventory. See the
[file navigation profile](web-configuration-file-profile.md).
`WebSourceRecovery` checks comments/literals/ASI, reparse equality, large-input
recovery without raising interactive budgets and invalid-source refusal.
`WebReadableExportSDK.*` checks immutable input deletion, exact original and
readable hashes, explicit source goals, invalid syntax/encoding without fake
source, stale/unknown/oversize refusal before writes, 9 MiB chunked selection,
independent interactive caches, nested compressed Wails origins and exclusive
local destinations. Its optional pinned Wails bundle case uses the same
`NEVERD_WAILS_ZENTRO_030_ELF` capture below. CLI tests use an unusable external-tool
PATH. Parser omission retains the new ABI with explicit refusal before writes;
backend omission checks the declaration independently. Worker/MCP exclude the
raw operation from their capability subset. See the
[readable-export profile](web-readable-export-profile.md).
`WebWails.*` checks fixed Go/Wails declarations, native pointer ownership,
non-filesystem slice exclusion, directory order/ancestors, portable path
collisions, content/metadata overlap, complete hash verification and exact/short
independent budgets. Its compiler digest cases cover empty files and the
1,024/1,025-byte algorithm boundary. PE cases deliberately separate RVAs from
file offsets and reject header/raw/virtual overlaps, invalid alignment,
address overflow, PE32/DLL/foreign ISA, conflicting platform declarations,
virtual tails, raw padding and mutable pointers. `WebWailsSDK.*` checks metadata canaries,
pagination, failed-publication rollback, cache/revision invalidation, separate
HTML table namespaces, ELF/PE compressed-parent source anchors, exclusive raw export
and overwrite/symlink refusal. CLI cases use an unusable external-tool PATH.
Worker/MCP compare the same extraction/pages and reject extra execution fields.
`NEVERD_WAILS_ZENTRO_030_ELF` and `NEVERD_WAILS_ZENTRO_030_PE` select the exact
bare Linux and Windows x64 Zentro v0.3.0 releases named in the
[Wails profile](web-wails-profile.md). Positive optional
cases verify all stored assets, exact JS SHA/offset/length, an unresolved
root-relative HTML reference without guessing the runtime mount, the existing
whole-source byte refusal and original native handoff/lifetime. The Windows
case additionally exports all assets and verifies the complete independent
same-tree readable output against its own hash and physical origin. Explicit
profile selection also survives metadata pages, raw manifests and readable
origins; cached extraction never answers a different profile. CLI tests cover
both profiles with an unusable external-tool PATH. The pinned binaries remain
offline corpus inputs, rather than runtime execution fixtures.
They do not download, install or launch that application. Build every owning
Web binary before running the complete owning Web directory without a prefix
filter; the eight parameterized Claude platform cases must also remain enabled.

`NativeConstantStores.*` independently checks x64 full-width LowIR value/address
roles, instruction/recorded-function boundaries and atomic unknown-effect
refusal. The optional actual native case uses
`NEVERD_TAURI_CHATGPT_110_X64_NATIVE`. `WebTauri.*` checks the upstream
SipHash13-128 known vector, complete PHF row agreement, mapping/range/path/hash
guards and bounded exact Brotli streams. `WebBrotliNoDecoder.*` compiles the
owning codec without its private SDK macro and checks truthful capability
omission and fixed refusal, without an external fallback.
`WebTauriSDK.*` checks native initializer projection, private pages, nested
archive coordinates, import revocation, exclusive raw/readable exports and
original native handoff. `NEVERD_TAURI_CHATGPT_110_X64_TGZ` enables the pinned
real lencx/ChatGPT v1.1.0 old-GNU tar/gzip case, all 15 decoded assets and all
nine same-parser-tree JS members. These optional tests use local captured
bytes only; see the [Tauri profile](web-tauri-profile.md). Worker/MCP tests
compare direct and framed results, reject caller-supplied table descriptors
and check canaries. The archive consumer/CLI fixtures also cover both uniform
old-GNU tar and old-GNU tar/gzip, including Bun/source/native consumers.

`WebDesktopBridge.*` checks exact direct Tauri/Wails shapes, optional syntax,
shadowed/dynamic roots, spread/getter/duplicate/computed/arity limitations,
record-budget atomic refusal and call/child agreement. Its optional captured
case uses `NEVERD_TAURI_CHATGPT_110_X64_JS` for the fixed decoded member 6.
`WebSDK.*DesktopBridge*` checks private values, paging, anchors, import revocation
and the actual CLI with an unusable PATH. `WebTauriSDK` additionally joins the
real decoded IPC call range to its exact compressed frame and old-GNU parent.
Worker/MCP compare the same bridge pages and reject extra execution fields.
Run the complete owning Web directory and affected transports, then parser
omission and restore both Web/JS options ON. These checks qualify source syntax,
not native command registration or runtime activation. See the
[desktop bridge profile](web-desktop-bridge-profile.md).

`WebConfigurationOrder.*` checks source loading/merge relationships, exact
unknown-key barriers, duplicate/accessor/computed definitions, redeclarations,
reassignment, dynamic scope and independent object/term exhaustion. Optional
fixed units use `NEVERD_CLAUDE_CODE_21296_CONFIG_JS` (module 1676) and
`NEVERD_CLAUDE_CODE_21296_CONFIG_UNKNOWN_JS` (module 6); the existing whole
Claude client corpus adds order-object/term/unknown-field coverage receipts.
`WebSDK.ConfigurationOrder*`, the offline navigation CLI and worker/MCP tests
check private values, shared anchors, cached/paged identity and import revocation.
Run the complete owning Web directory and affected transports, then parser
omission and restoration. See the [profile](web-configuration-order-profile.md).

`WebSourceRewrite` separately checks closed primitive substitutions, UTF-16
surrogates, signed zero/NaN/infinities, BigInt, distinct passes, comment and
directive preservation, tagged-template refusal, ASI outer-tree mismatches,
exact inverse and tampered receipts. Lexer, record and output-growth budgets
must refuse atomically. Its optional pinned Claude module 1897 case checks 5
string-recovery and 201 constant-folding substitutions, each retaining 1,496
refused candidates, with exact original recovery. This does not qualify every
module for semantic rewriting. `WebRewriteExport` checks candidate/receipt
failure rollback, foreign files, symlinks and renamed/replaced output paths.
`WebRewriteSDK` checks private literals, cache/revision and parse-goal bindings,
immutable captured bytes and receipt-bound CLI export with an unusable PATH.
Worker/MCP tests compare both passes and record pages against the shared API;
MCP excludes raw export and arbitrary output paths.
`WebSourceRename` checks whole-binding replacement, shadowing/closures, decoded
identifier collisions, unchanged property keys/labels, name inference, shorthand,
non-simple parameter environments, writes, global/module names, dynamic scope,
budgets and inverse tampering. Its optional pinned Claude case uses original
Bun modules: module 1 retains excluded rest parameters and exports while module
228 refuses the record ceiling without publishing a partial candidate.
`WebRenameSDK` checks the distinct lexical profile, private original/generated
names, shared cache limits, revision-bound receipts and CLI export without an
external-tool PATH. Worker/MCP check the same profile and record evidence.
`WebRewriteSDK.PrimitiveV1ReceiptsRemainExactlyCompatible` compares both older
passes against fixed pre-rename export bytes, IDs, tokens and SHA-256 hashes.
`WebSourceBranch` supplies independent expected text and retained-tree oracles
for Boolean `?:`/`&&`/`||`, method/optional/tag calls, indirect eval, typeof/delete,
selected sequences, chosen effects/exceptions, lexical projection, await and
scope exclusions, ASI/comments, tampered receipts and byte/record/work/lexer
ceilings. `WebBranchSDK` checks private operand metadata, distinct evidence,
shared cache/revision receipts and CLI export with an unusable PATH. Worker/MCP
compare the same branch result and records. `WebRenameSDK` additionally pins
the complete pre-branch rename receipt; existing primitive goldens still apply.
Its optional original Claude test accepts one branch and refuses six in module
1047, leaves all 1,007 module 1897 candidates unchanged, and checks module 1613's
source-byte refusal through the same Bun decoder. No target is executed.
Async-rest source spans and illegal trailing commas have parser regressions.
Resource-declaration tests cover contextual keywords, line terminators,
initialization, loops, source types, immutable lexical bindings and conservative
disposal effects. With the same official-artifact environment variable,
`WebSourceRecovery.ClaudeCode21296AllJavaScriptWhenSupplied` checks all 2,345
modules for reparse-verified readability and preservation of every original
byte in order; it never executes the target. Allow several minutes on a busy
host. The worker's C++ code-edit regression includes 9,000 long comment rows,
real asm linkage and fake linkage inside comments. Existing source-cache
transport checks exercise cache reuse and byte-budget eviction without raising
their response deadlines.
`WebSDK.BunExport*` and `WebSDK.CLIBunExport*` check captured-byte preservation,
all region contents, raw/readable/index output, stale revisions, destination
and symlink refusal, metadata canaries and an unusable external-tool PATH.
The full practical command and result boundary are in the
[Claude Code qualification](web-claude-code-qualification.md).

ASAR cases in `NeverDWebArtifactTests` combine fixed upstream writer archives
and an independent original member with C++-constructed malformed inputs.
They cover complete truncation, Pickle padding, overlaps/overflow, Unicode
aliases, inherited unpacked directories, missing/wrong-sized/changed bytes,
whole/block integrity (including empty terminal blocks), and admission ceilings.
`WebAsarSDK` checks source/map/native identity, exact packed/unpacked anchors,
virtual relative-file candidates, unavailable-member refusal, cache/revision
boundaries and CLI commands with no external tools. The C++ worker compares
direct and framed extraction, source anchors and unpacked native handoff.
The fixed native ICU policy is required; omission tests check an explicit
`archive_path_policy_unavailable` response. See the
[ASAR profile](web-asar-profile.md) and
[fixture manifest](../unittests/web/fixtures/asar/README.md).

`NeverDWebArtifactTests` covers artifact identity, immutable import publication,
input links/collisions, aggregate budgets and bounded JSON admission. Shared
`WebPathPolicy` owner cases check portable device names and
NFC/full-fold equivalence; captured Unicode aliases refuse before publication
without changing original evidence names. Both owner cases and the portable
capture case are mandatory in the Windows product receipt. Its blob
tests additionally check immutable disk snapshots, shared descriptor lifetime,
range/overflow limits, concurrent reads and a C++ child with a lowered file-size
limit to inject storage-write failure. The maximum-size case streams two 256 MiB
members, checks hashes and records peak-RSS growth; it creates about one GiB of
temporary source/spool data and removes it through the fixture's lifetime.
`NeverDWebSourceTests` checks embedded C++ parsing, original byte spans, exact
UTF-16 values, lexical binding relationships, dynamic lookup, primitive arithmetic,
coercion and refusal budgets, immediate/deferred effect boundaries, ESM module
entries, require uncertainty, confined admitted-file comparisons, and bundle
partition spans/identity/uncertainty. Bundle tests verify an archived upstream
example against its source document/manifest plus C++-constructed inert cases;
source-view tests additionally cover regex/template rescans, keyword property
names, private/escaped identifiers, shadowing/dynamic aliases, reviewed ranges,
UTF-8/CRLF boundaries, immutable source coverage and the retained-token ceiling.
Navigation tests cover nested functions, parameter defaults, direct expressions,
shadowed/dynamic/reassigned callees, tagged/optional/constructor/import syntax,
unavailable lexical evidence and malformed models. Bun range cases distinguish
Latin-1, UTF-16 surrogate pairs, client UTF-8, CRLF and excluded terminators.
`NeverDWebSourceMapTests` checks map deltas, index offsets and UTF-16 coordinates,
plus Bun serialized-map windows, rare/duplicate anchors, preserved compiler
source hashes, complete truncation boundaries and bounded Zstd frames. Bun map
cases explicitly skip when both LLVM Zstd and the exact native static SDK
1.5.7 are absent; SDK capability and unavailable-path checks remain active.
With Zstd-free LLVM, `-DCMAKE_DISABLE_FIND_PACKAGE_zstd=ON` tests that omission.
No external decoder is used.
`NeverDWebSDKTests` exercises C ABI ownership, revisions, private source/binding/semantic/map
evidence and the CLI with an unusable external-tool `PATH`, including large
originals refused by the smaller source/map limits before materialization.
Source-view SDK cases check metadata-only previews, single-use publication,
policy revocation, revision invalidation, exact chunks and source-view cache
limits. The C++ worker compares direct and framed results and refuses text from
an uncommitted projection or an undeclared disclosure field.
Navigation/anchor cases compare ordinary, Bun and compressed-map storage
precision, committed-view coverage, revisions and direct/framed query parity.
CLI cases also exercise `bun-navigate`, `bun-view` and `bun-anchor` with an
unusable external-tool PATH.

The optional `NEVERD_BUN_142_CROSS_CORPUS` is documented in the
[Bun cross-container corpus](../unittests/web/fixtures/bun/cross/README.md).
`WebBun.CrossPlatformFullCompilerContainersWhenSupplied` validates twenty pinned
ELF/Mach-O/PE compiler images for the additional ARM64/x64 targets; absent full
images are a skip. Preserved graph tests use synthetic wrappers and cannot
replace this qualification. All reads, negative mutations and golden member
checks are C++, and the generated targets are never run.

`WebBunSourceMapCorpus.OptionalSixPlatformImagesRecoverIndependentOriginalSources`
requires both `NEVERD_BUN_142_CORPUS` and `NEVERD_BUN_142_CROSS_CORPUS` plus the
native decoder. It checks twelve full Linux ELF, thin macOS Mach-O and Windows
PE x64/ARM64 asset/cache images against their recorded original container hashes,
recovers the independently authored original source hashes, validates generated
and original coordinate boundaries, and retains exact stored-frame anchors.
Absence of either corpus or the decoder is an explicit skip. This fixture
does not qualify other producer versions or bytecode recovery.

`NeverDWebNativeTests` compares file and immutable-buffer loading of synthetic
ELF/PE/thin Mach-O, verifies direct native bytes and static decompilation after
web/input destruction, rejects truncated images and implicit universal slices,
and checks sidecar/patch refusal and transactional file reload. It also covers
directory occurrences, Bun native-asset provenance and four offline CLI commands.
The full Bun compiler-container native load case uses `NEVERD_BUN_142_CORPUS`
and explicitly skips when that corpus is absent. It does not execute the target
or claim full-runtime native decompilation. The Windows product lane requires
the eight self-contained native handoff cases without skips. The supplied full
Bun compiler case remains part of the separate producer gate. Worker checks retain
a handoff across
failed replacement, compare direct/framed static analysis, revoke it on import
and preserve the separate ordinary native project.
All new fixture-generation and test logic is C++; archived target JavaScript
is inert input text.

Electron cases add bounded manifest entry comparison, module-origin candidates,
window/preload/renderer/IPC/bridge source ranges and refusal of shadowed, written,
cyclic or dynamically resolved origins. C++ tests check falsy default entries,
ESM extension/type rules, unsafe/missing paths, duplicate JSON, option getters,
spreads and duplicate keys, alias/record budgets and mismatched evidence.
SDK/CLI tests join an ASAR manifest to an explicit unpacked main, retain source
anchors, refuse stale/cache/page requests and keep canary values private with
external tools unavailable. The C++ worker compares actual framed results with
the direct C API, including parser omission and unknown-field refusal. See the
[Electron evidence profile](web-electron-profile.md); these tests do not
establish runtime API identity, distribution detection or a complete app graph.

`WebElectronIPC` additionally checks cross-source candidate grouping, exact
UTF-16/NUL/surrogate comparisons, normalization distinctions, missing-main and
wrong-source-type refusal, namespace isolation, unavailable origins, stable
selection order and aggregate record/string/comparison limits. SDK/CLI cases
cover ASAR occurrence isolation despite equal backing bytes, scoped pages and
storage anchors, cache/revision refusal, private channels and original/ASAR CLI
requests with unusable PATH. The framed worker compares both IPC operations
against the same C++ API; no Electron runtime is part of these tests.

`WebSourcePaths` checks CommonJS and ESM roots, app/source-base separation,
join/resolve/concatenation differences, file URL object/string/directory cases,
shadowing/writes/eval/cycles, unsafe paths and deterministic request selection.
Parsed inert fixtures independently exhaust path work and allocated-string
budgets and verify that no partial paths escape. `WebElectronEntries` adds
exact-file/selected-source links, missing/directory/outside-scope refusals and
aggregate boundary admission. A repeated long alias separately exhausts entry
normalization work even when expression values are cached. SDK/CLI cases cover
source anchors, immutable
capture after deletion, bounded pages/caches, revision invalidation, private
path canaries, ASAR occurrence isolation, unavailable unpacked targets and
original/ASAR CLI forms with unusable PATH. The framed worker compares entry
summaries/pages with direct API results and checks omission/unknown fields.

`WebHTML` in `NeverDWebArtifactTests` checks the full pinned named-reference table
and code point arrays, token/type/escape states, body spans, local URL/base
association and real scan/link budget exhaustion. `WebHTMLSDK` checks C ABI and
CLI inventory, source parsing, original/ASAR anchors, cache/revision failures and
private-output canaries with an unusable PATH. `WebHTMLModules` in
`NeverDWebSourceTests` checks inline import/re-export/dynamic-import candidates,
preceding bases, URL metadata, import-map boundaries, malformed contexts and
real work/count exhaustion. Unavailable inventories and early exhaustion cannot
claim that import-map declarations are absent. SDK cases separate equal inline
bytes under different documents/bases, preserve packed/unpacked ASAR occurrence identity and verify
that external-file contexts cannot be rebased by HTML inspection. CLI checks
require request pages after a partial module summary. The framed worker compares
both HTML operations and inline source/anchor/module results with direct API
results. All
new fixture recording and test logic is C++; the [profile](web-html-profile.md)
records deliberate context and import limitations.

`WebImportMap` checks native URL normalization, exact/prefix/scoped precedence,
null and invalid-address blocking, prefix backtracking, separate declaration
and script bases, first-definition composition, percent/query/fragment identity,
duplicate/normalized-key refusals and independent admission/resolution budgets.
HTML module cases add timing/origin/root confinement and aggregate map limits.
SDK/CLI/worker cases compare private map pages and request evidence, preserve
ASAR member identity and read captured maps after the original files are deleted.
No JavaScript runtime, browser, URL CLI or external generator is used. HTML map
inventory must also be checked with the JS parser disabled; backend-omission
coverage retains its existing ABI cases.

Build the owning target, then run
`ctest --test-dir build-release/unittests/web --output-on-failure`. Parser tests require
`NEVERD_ENABLE_WEB_JAVASCRIPT`; the backend requires
`NEVERD_ENABLE_WEB_ANALYSIS`. Native lifting suites are not substitutes for
this source-domain coverage. Host/profile limitations are recorded in the
[web support matrix](web-support-matrix.md); source inventory does not qualify
container extraction, full JavaScript semantics or complete epic acceptance.
`NeverDWebAvailabilityTests` remains available when the backend is disabled;
run its test directory at `build-release/unittests/web-api`. With the worker
enabled, `NeverDWorkerWeb` in `build-release/tools/neverd-worker` checks the real
framed process and isolation from native-session revisions using a C++ client.

## Library recognition

The feature repository retains the original compiler objects, truth, source
profiles and digests. `NeverDLibraryRecognitionTests` consumes those archives
and checks all 59 rules in the five supported libc++/MSVC STL/ATL/musl profiles.
It includes standalone and inline positives, wrong layouts/types/call slots,
COM ordering and exception near misses, cross-pack conflicts, budgets, unchanged
IR and mapped HighC/default LLVMC/NoOpt LLVMC source. `NeverDSessionCAPITests`
checks shared identities, authoritative names, template distinctions and cache
invalidation. The worker fixture exercises actual paged engine responses.

```sh
cmake --build build-release --target NeverDLibraryRecognitionTests \
  NeverDSessionCAPITests NeverDSignatureTests NeverDDebugInfoTests NeverDPDBIdentityTests \
  NeverDObjCSourceCallTests NeverDLLVMCValueTests neverd-worker --parallel 4
build-release/bin/NeverDLibraryRecognitionTests
build-release/bin/NeverDSessionCAPITests
build-release/bin/NeverDSignatureTests
build-release/bin/NeverDDebugInfoTests
build-release/bin/NeverDPDBIdentityTests
build-release/bin/NeverDObjCSourceCallTests
build-release/bin/NeverDLLVMCValueTests
ctest --test-dir build-release -R '^NeverDWorkerLibraryFeatures$' --output-on-failure
```

MSVC consumer fixtures require Clang and `lld-link`. CMake links the archived
`/Z7` objects into a matching PE/PDB pair and creates real runtime import thunks
from fixture `.def` files. These ATL images are analysis-only: unresolved platform
calls are retained and never executed. Unsupported native exception rewriting
does not become supported by recognizing a library operation. Missing linkers
omit these tests and must be reported as skipped profile coverage.

The optional GUI targets `neverd-gui-library-view-tests` and
`neverd-gui-text-position-tests` cover fold/unfold, original text copying,
disjoint regions, page completeness, Unicode positions and revision changes.
Run the broader `check-neverd` target before review because the source observers
cross both C backends. Pack schema/probe tests in `signatures/scripts/tests`
validate data production separately from these engine tests.

The worker source-synthesis integration also covers a missing-corner product
of degree 24. CLI single-function and whole-program HighC/LLVMC, plus both GUI
worker source routes, recompile and execute the result against an independent
formula at `-O0` and `-O2`, preserving the high-bit remainder and toggle/cache
isolation.

## Modular MBA simplification

`SymSimplifyFinite.*` covers complete two-valued slices at widths from 8 to 512 bits, shared-use profitability, every supported poison-generating annotation, independent volatile reads and freezes, explicit undef/poison, deep iterative traversal, budgets and the obfuscation stamp. Original and simplified IR execute against an independent oracle over every byte input and randomized full-width inputs at O0/O2. Translation-object tests require distinct cache identities for distinct finite-value budgets.

Merge-domain tests cover nested selects, diamond PHIs and copy cycles at 8–512 bits; conflicting backedges, undefined conditions, unanchored components, independent PHI/freeze observations and retained flagged producers; and exact node, edge and work boundaries. O0/O2 runtime oracles exhaust every byte-input pair and vary full-width operands for selections, joins and bounded state loops.

The finite-value tests additionally cover nested conjunction masks at 8–512 bits, commuted operands, OR/undef refusal, bounded deep discovery, standalone work/stamp policy and the exact first-rewrite budget boundary. Runtime oracles exhaust all byte-input pairs and vary unrelated 64-bit data, comparing original and simplified IR at O0/O2.

`SymSimplifyPredicates.*` exhausts four-bit offsets, signs and inputs, checks Boolean interval composition and disconnected sets, and executes independent byte/full-width oracles at O0/O2. It covers poison annotations, hidden undefined join inputs, independent reads/freezes, retained loop PHIs, shared-use profitability, cumulative work, high fan-out, recursion limits and the obfuscation stamp. Translation-object tests distinguish predicate budgets in both cache keys.

`SymExpr.*` checks constant non-low windows with exhaustive four-bit inputs and masks, wide carriers, nested structural operations, mixed known/unknown byte reassembly and arithmetic carry counterexamples. Budget regressions combine a wide node with the recursion boundary and reject copying an oversized constant. Unknown windows must remain symbolic without expanding the expression DAG. `SymState.*` also distinguishes derived scalar constants from literal-only region facts in both byte orders, without growing the DAG or changing complete stored words.

`SymReadability.*` covers subtraction and complement spelling, n-ary operator
cost, one-bit and wide literals, shared-tree saturation, budgeted selection,
and exhaustive three-bit equivalence with sampling disabled. `SymMBASample.*`
compares narrow and arbitrary-width verification against the AP evaluator,
including all operators, deterministic assignments, and unused wide inputs.
Candidate quality
comparisons across scoring revisions must recount both outputs with the same
metric; the SDK's version-specific size counters are diagnostic only.

`SymMBAMinimumTests` checks proved binary-leaf minima with only traversal work,
exhausts cheaper functions at small widths, and covers wide words, resource
refusals and related or opaque inputs that still require full simplification.

Build the symbolic engine, LLVM safety guards, HighIR bridge tests, and source
roundtrips together when changing shared bitvector simplification:

`HighSymSimplify.ParallelSegments*` compares serial and parallel rewriting
over shared DAGs with distinct branch, switch and exception-body definitions.
Independent arithmetic oracles check the rewritten results. Calls, labels,
exception continuations, PHI copies, address escapes and ordered effects must
still kill definitions; ordinary stores must retain them.

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
Boolean identities as ELF, COFF, and Mach-O. Ordinary source keeps MBA, so
every case decompiles with the explicit `--symbolic-simplify`, whole-program
or for one function. It checks both HighC and LLVMC for
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
`HighCDefinitionIndex.*` in `NeverDHighCStoreForwardingTests` checks all six
definition-identity fields, definition order, repeated and differently sized
definitions, preceding versus conditional/later aliases, missing definitions
and cyclic references. Address keys and private-frame eligibility must retain
their existing behavior.
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

`HighControlFlowDAG.*` in `NeverDHighControlFlowTests` checks single-use
condition folding on large shared expression graphs. Shared reads retain
their multiplicity without expanding every path, and substitution reaches a
unique read after an unrelated shared subgraph. Folding preserves its depth
limit and rejects effects even after a large pure prefix.

`HighBoundPrivateFrameCopies.*` in `NeverDHighControlFlowTests` checks copies through nonescaping private frame slots after call ABI binding, including branches, slot reuse and agreement across guard contexts. For x64 and AArch64, the emitted C runs at `-O0` and `-O2` with undefined-behavior traps and independent arithmetic checks. Negative cases preserve the original function for frame escape, unknown call ABIs, missing or inconsistent frame aliases, reassigned entry inputs, overlapping accesses, ordered or atomic memory, malformed statements, cycles and exhausted budgets. Ordinary value conversions must not become PHI copies.

`HighIntegerSignedness.*` in `NeverDHighControlFlowTests` checks the late pass that declares each register or temporary local signed or unsigned by what most of its uses read. Wrapping arithmetic, logical shifts and unsigned comparisons favor unsigned; signed comparisons, signed division, arithmetic shifts and sign extension favor signed; a conversion to an integer as wide as its operand favors the signedness it converts to; a shift count reads unsigned, so its masking arithmetic prints without a signed view; a local with any non-integer use keeps its types. The emitted C runs at `-O0` and `-O2` with undefined-behavior traps against independent reference arithmetic, including a signed comparison of a local that became unsigned.

`HighNarrowLocals.*` in `NeverDHighControlFlowTests` checks the late pass that declares a register or temporary local only as wide as its reads take: every read takes at most its low N bytes and every definition extends a value of at most N bytes or is a constant that keeps its meaning in N bytes. A constant definition keeps only its low bytes; a wide read, an incoming register, a join, a call result or a definition that computes all register bytes keeps the declaration. The emitted C runs at `-O0` and `-O2` with undefined-behavior traps against reference arithmetic.

`HighValueForward.*` in `NeverDHighControlFlowTests` checks when the HighC writer may fold a single-use value into its use. A loop condition keeps a value whose variables the loop assigns, since one name can denote several SSA values; a reloaded frame slot keeps its value across a store to that slot and folds past a store to another slot. A copy keeps its value when its source is reassigned before the use. Each case runs at `-O0` and `-O2` with undefined-behavior traps.

`HighCIntegerConversion.*` in `NeverDHighControlFlowTests` checks the integer conversions the HighC writer leaves to C. A conversion inside an operand that keeps the bytes an outer conversion keeps prints no cast of its own; an assignment to a declared integer local and a return convert implicitly, and a literal is spelled as the value it converts to, while a pointer keeps its explicit conversion. Stores convert like assignments, and a zero-extended argument to a typed wider parameter keeps its extension. A callee whose debug symbol has no type, or that nothing declares while the code reads more than an int of its result, returns its whole register, so a 64-bit pointer keeps its upper half. Each case runs at `-O0` and `-O2` with undefined-behavior traps against reference arithmetic.

Source projection also revalidates variadic object lists after this cleanup: empty instruction anchors are accepted, while hidden effects or control transfers are rejected. Synchronized cleanup accepts a single `int64_t` or `uint64_t` view of the same saved receiver; narrowing, floating conversions, address arithmetic and reassignment remain rejected. Foundation object sets and normal/exceptional unlock traces run at both `-O0` and `-O2`.

```sh
cmake --build build-release --target NeverDARM32InterworkingTests --parallel 4
build-release/bin/NeverDARM32InterworkingTests
```

## Finite native dispatch

`AffineFrameState.*` in `NeverDJumpTableTests` checks balanced and nonzero
cycles, unknown roots, conflicting anchors, intermediate overflow, budget
exhaustion, incremental graph growth and cache reset. A seeded independent
backward path-constraint oracle checks cyclic equation results. Repeated
diamond graphs check linear evidence growth rather than a wall-clock cutoff.

`HighEntryStackOffsets.*` in `NeverDHighControlFlowTests` checks the shared
affine solver's SSA adapter against independent path constraints and the prior
recursive algorithm on its supported small domain. Coverage includes shared
diamonds, balanced and unanchored cycles, conflicting paths, checked overflow,
SSA ambiguity, exception entries, pointer widths and query-order-independent
depth refusal.

`ResolverFrameModes` checks that value-query mode switches retain independent
frame equations, partial stack writes reject stale frame identities and an
incomplete query cannot poison a later query. `ResolverLocalAddress` checks
that unrelated arithmetic does not multiply LOAD-address proof work, missing
or failed local proofs retain full discovery, overwritten producers are
rejected, and every evidence-budget boundary preserves transactional
publication. Both belong to `NeverDJumpTableTests`.

`ResolverGraphCache.*` in `NeverDJumpTableTests` compares hot and cold answers
and remaining budgets at every small-fixture budget boundary. Equal-size
instruction, edge, root and ownership changes must replace the graph; rolled
back and temporary override payloads cannot leave borrowed pointers behind.
Resource failure and value-analysis incompleteness remain distinct.
`ResolverPointLookup.*` checks exact instruction coordinates across cache
collisions, missing points, high address bits and distinct operation sequences.
A small colliding working set, including an absent point, must stop repeating
ordered searches after warming the cache.
Switching or moving immutable indexes must preserve their own coordinates, and
later queries cannot change an already returned location. The graph/cache
regressions retain the existing proof and evidence-budget checks.
`ResolverMemoryDefinitionCache.*` checks every cold/warm scan-budget boundary,
potential writers with malformed operands, collisions and graph replacement.
`ResolverGraphCache.MemoryScansKeepExactWritersBarriersAndBudgets` covers long
memory scans across four architectures and three formats: a claimed memcpy at
a pure operation remains invalid, and adding a call invalidates a cached scan.
Changes to provisional CFG storage should run both `NeverDJumpTableTests` and
`NeverDLowInstructionBoundaryTests` to cover stale targets, root ownership and
the final published operation/boundary ranges.
`ResolverLoadAddress.*` checks that one opaque load retains its identity when
its unused address exceeds the proof depth. Across four architectures and
three formats, stores, calls, ordered reads and incompatible widths prevent
load reuse. Compatible reloads and ARM ELF literals still require complete
address reconstruction; warm queries retain the same result and budget.
`ResolverPendingCycles.*` checks nested loops whose late definitions must reach
every batched query, in both query orders and in isolated queries. Register and
private-frame variants cover x86, x64, ARM and AArch64 across ELF, COFF and
Mach-O. Reachable values cannot disappear from feasible masks or certify a false
upper bound; real arithmetic updates cannot become transparent backedges. Warm
and cold queries must agree at evidence-budget and resolver-depth boundaries.
`ResolverPendingWalks.*` compares recursive dependency lifetimes against an
independent set-of-activation-IDs model, including sparse ancestors, completed
memo hits, reused depths and the fixed bitset's word and capacity boundaries.
Unrelated returns must preserve an exact snapshot; retiring any actual
dependency must invalidate it, including beyond the exact tracking capacity.
`ResolverPredicateDemand.*` places a deep operand behind conditions that the
current query cannot use. The independent constant candidate must still be
proved across four architectures and exact COPY/NOT transports. Needed
conditions, guarded/partial definitions and cross-block producers retain their
depth refusals; policy switches and evidence budgets agree cold and warm.
`ResolverQueryScope.*` compares scoped and ordinary queries at every small-fixture
budget boundary, including completion flags and masks. It covers owned temporary
overrides, equal-sized edge changes, roots, different builders, nested leases,
new invocations after instruction/storage changes, depth refusals, oversized
keys and the legacy null-budget path.
`ResolverConditionQueryCache.*` checks shared value/control graph ownership,
ordered control queries, temporary certified edges, roots and storage-owner
changes, and every small-fixture budget boundary. An independent graph-walk
oracle covers cyclic, disconnected and multiple-entry control flow, including
edges leaving the graph, with cold and repeated queries.
`ResolverValueQueryCache.*` checks actual complete-batch reuse, exact remaining
budgets at every small-fixture boundary, ordered mixed results and feasible
masks, all query fields, proof limits and graph-independent relocation context.
Shared match-work failures clear all outputs, including earlier feasible masks.
Cached match/depth refusals preserve per-query incomplete status, independent
results and cold work charges for every requested output shape. Changed limits,
graphs, query ordering and relocation context require fresh analysis. An
independent unknown prevents refusal retention. Other incomplete proofs and
oversized records cannot be retained; count and byte
limits are checked independently.
The long-predecessor guard fixture separates a shallow comparison from its
value's CFG history. It checks 128 table slots reaching four exact case targets,
unrelated guards and partial-register changes, while retaining the independent
deep-syntax refusal. The cache case checks rejection beyond the expanded value
depth, that a complete expanded-depth answer cannot satisfy a default-depth
query, and that repeated expanded queries preserve results and work charges.
`PipelineOutcome.ParallelCalleeFrontiersPreserveEveryRegisterSummary` compares
serial and parallel summaries across two 40-function callee frontiers.

`ResolverLaneViews.*` in the same target checks query-local register metadata
caching across widths, high-byte registers, architectures, temporary values
and deliberate cache collisions. Shuffled architectural views must retain
their cold-query answers after eviction; value and frame proofs remain outside
this fixed-size cache.

The two-table linear-copy cases cover more than sixteen address/base copies,
partial address clobbers and exhausted shared evidence. Both C routes, including
optimized and unoptimized LLVMC, execute every byte selector and four table-select
inputs at O0/O2 with undefined-behavior traps against the fixture formula.
The existing deep selector-copy refusals remain covered independently.

`ResolverCountLane.*` checks that scalar POPCOUNT/LZCOUNT results and their
zero-extended low bytes identify the same value on x86, x64 and AArch64 LowIR.
Malformed operands, wider inputs, different byte lanes, unrelated operations
and exhausted work remain negative controls; cached answers retain cold work
charges. `JumpTableCountLane.*` uses actual x64 TZCNT/POPCNT/LZCNT instructions
with 32/64-bit inputs and byte/full-width guards. High-bit arithmetic and
partial-register writes must prevent recovery. Relative-table HighC and
absolute-table HighC, optimized LLVMC and unoptimized LLVMC execute zero,
all bit positions and deterministic mixed inputs at O0/O2 with UB traps.
The existing deep-arithmetic narrow guards must retain their exact-definition
proof before the batched extension-range fallback is considered.

`ResolverGuardPredicates.*` checks compound entry/backedge predicates on x86,
x64, ARM32 and AArch64, including changed lanes, independent roots, unrelated
flags, wide indices and exhausted evidence. Query-mode changes must retain
separate caches, and a logical union cannot claim CFG predecessor ownership.
`JumpTableJoinedGuard.*` checks separate inclusive/exclusive loop guards in
ELF, COFF and Mach-O. Its ELF source
roundtrips execute HighC and both LLVMC modes at O0/O2 against an independent
selection oracle. Small joined domains reuse the finite-coordinate proof;
unchecked paths and sparse or incomplete domains cannot grant a dense table.

`GuardExpression.*` independently enumerates every byte offset and boundary
for signed/unsigned comparisons in both operand orders and affine directions.
Wide nested affine expressions and Boolean combinations retain wrap, sign and
hole boundaries; periodic masks, shifts and width changes require full replay.
The native guard suites above still cover CFG/lane identity, sparse tables,
all-domain proof rejection and exhausted evidence. Replay partitions retain the
original whole-window evidence charge and the final solver proof.

`ResolverValueQueryCache.MalformedScalarRecordsCannotPublishProofs` verifies
missing ADD/SELECT inputs, input-array overflow, zero widths and a memory-space
scalar input across x86/x64/ARM/AArch64 and ELF/COFF/Mach-O metadata. Each case
first populates a valid cache, mutates an equally sized record, and requires
incomplete results on warm and cold queries. Valid input must still prove its
value; malformed input cannot become a completed negative proof.

`ResolverValueQueryCache.EntryMemoryCannotBorrowAStoreFromALaterIteration`
checks first-iteration frame-memory identity and authenticated writers across
the same architecture/format matrix. A back-edge STORE cannot initialize the
entry LOAD. A preceding definite STORE is a positive control, and masking the
unknown LOAD still proves its independent numeric bound. Both query orders and
cold/warm caches must agree.

`ResolverValueQueryCache.FiniteJointReuseRequiresCurrentSourceAndRoles` supplies
a cached numerical result directly to the production reuse gate. Equal-size
source changes, altered index/LOAD/target use points and changed address roles
must refuse; exact/one-short work boundaries remain deterministic. This fixture
tests result binding, not construction of a numerical theorem. The existing
i386 GOTOFF joint-loop and four/six-consumer adjacent-table fixtures separately
exercise construction, restricted selector/address/target replay and atomic
group publication.
`ResolverLoadRoles.RestrictedReplayDoesNotBorrowTheGroupHypothesis` includes an
extra H edge that can corrupt both an address and a target producer. Both role
queries refuse H and accept its independently supplied restricted map; ordinary
group mode still uses H. Cold/warm graph reuse must preserve this distinction.
`ResolverFiniteControl.*` supplies a provisional nonmember target subset that
hides a STORE of an out-of-range index. The subset admits an apparent finite
domain, but cannot supply the joint owner's control completeness; exposing the
STORE independently rejects both selector domains. Direct control, ordinary
indirect RET and modeled additional roots are positives. Unknown/interior/guard
roots, missing block boundaries and malformed nonterminal branches refuse.
Conditional code-root input also refuses this narrow owner, including a target
silently omitted by the ordinary graph because its model is missing.
Exact/one-short work checks require no stale completeness output on exhaustion.
These controls exercise the shared graph gate, not a new table admission route.

`COFFTableBase.*` in `NeverDJumpTableTests` checks image-base definitions across
0, 32 and 160 unrelated lexical blocks, mapped decoy definitions, single- and
two-level RVA tables, conflicting predecessors, scalar provenance, calls and
full/partial clobbers. A serial dispatch discovers new blocks and a second
switch before the SEH source-frame gate is checked. Tiny aggregate budgets
cannot publish partial tables; a non-image-base relative table remains valid.
`ResolverPEImageBase.*` checks exact/one-short query budgets, incomplete graphs
and freshness after definition/provenance/root changes. `COFFTableBaseCLI.*`
writes a minimal linked PE, verifies its decoded targets and invokes HighC and
LLVM-to-C through the CLI, without depending on a platform linker.

`ResolverConstantAddress.*` checks address discovery across four architectures
and three formats, equal and conflicting predecessor values, scalar/fragment
collisions, uninitialized arms, loop backedges, cache output identity, every
small-fixture budget boundary, changed roots and shared failure clearing.
Scoped address discovery and its legacy-fold wrapper must preserve every
budget boundary, completion state and address across all four architectures
and three formats. New scopes revalidate changed instructions; roots are
checked on each query. The PE image-base helper has the same budget checks.
`Native/JumpTableAddress.*` assembles
x86, x64, AArch64 and Thumb ELF objects and x64/AArch64 Mach-O objects. It checks
0/32/160 unrelated blocks, mapped decoys, agreeing joins, clobbers, calls and
tiny budgets against independent case-label offsets. It also covers absolute
tables on x64/AArch64 and a two-level x64 ELF table. With LLD it checks
the linked ELF and Mach-O images. These are loader/LowIR checks on the host;
they do not claim native execution on each target operating system.

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
The independent ARM64 frame fixture also distinguishes restored native tail
calls from live or unknown SP values, changed link words, full/partial return
slot writes, unknown aliases, borrowed frame pointers, independent roots and
conflicting loop backedges. HighC and both LLVMC routes must retain an explicit
failure path for the unresolved live-frame branch, and their C must compile.
Zero/short work budgets, incomplete instruction boundaries, unknown roots and
invalid graph edges grant no frame candidate. The focused indirect-call
suite includes the existing four-architecture tail-call runtime variety.
An unselected prefix pointer to a separate function must not prevent recovery
of local cases. A foreign target selected immediately or after a reached
backedge must remain outside the local switch. The prefix case also runs
through all three C routes at both optimization levels.

```sh
cmake --build build-release --target NeverDJumpTableTests --parallel 4
build-release/bin/NeverDJumpTableTests
cmake --build build-release --target NeverDIndCallXformTests --parallel 4
build-release/bin/NeverDIndCallXformTests --gtest_filter='*TailVarietyRT*'
```

`NeverDMachOPointerRelocationBoundaryTests` checks sparse dispatch origins
before the owned runtime slots. Missing maps, fixups or ownership, added filler
slots, unindexed reads, malformed strides, address overflow and exhausted
evidence must retain the ordinary load path.

`NeverDJumpTableTests` checks repeated field loads at nonzero offsets on x64
and AArch64. Independently computed addresses may share a switch guard only
when their full-width pointer, displacement, load width and memory history
agree. Borrowed RBP fields, negative displacements and both signed and unsigned
index extensions are covered; changed bases/offsets, partial reloads, stores,
calls and memory barriers must not recover a table from the earlier guard.

`JTE_X86_32.StageAbortKeepsAnUnreachedTableShapeOpaque`
(`test_i386_gotoff_stage_abort.s`) exhausts every proposal stage right after
the first of two GOTOFF dispatches resolves. A completed stage rejects the
second, whose GOT register one path replaced, and lowers it as a tail call; an
aborted one must keep it an unsafe `INDIR_BR`. The test fails without the
stage-abort marker.

`JTE_X86_32.GetPcThunkCallSeedsTheGOTModel` assembles GCC's object-file
switches (`test_i386_gotpc_thunk.s`): a get-PC thunk call followed by the GOTPC
add, with a separate or fused GOTOFF table read. Each must publish one GOT-base
model and its exact cases. `GetPcThunkSeedsOnlyTheRegisterItLoads` adds the GOT
to a register the thunk did not load and must publish neither.

`LinkedI386GOTTable.*` in `NeverDJumpTableTests` links an i386 shared library
with ld.lld (`test_pic_switch_i386.s`) and requires the exact cases of GCC's
thunk with a separate or fused add, clang's call/pop, and a GOT register kept
in `%ebx` across a call on one path; `gcc_fused_switch` must decompile as a
`switch`. A GOT+4 anchor and a register replaced on one path must publish no
table. The tests skip without ld.lld.

`JTE_AArch64.GCCCompactSignedEntriesMayPrecedeTheAnchor` assembles GCC's
compact switch (`test_jumptable_gcc_compact_a64.s`): the entry loaded back into
the index register, the table base register reused for the anchor, and signed
byte and halfword entries (`sxtb #2`, `sxth #2`) with a case before the anchor.
It requires the exact targets and the lifted switch cases, and no table for a
halfword entry the dispatch truncates to its low byte.
`JTE_AArch64.GCCCompactGuardBoundsTheStorageItReads` is libgcc's
`size_of_encoded_value`: a guard bounds the index below bytes that follow the
table in its data owner, and one case calls `abort`. It requires the five
guarded targets. These functions have their own object: a case that calls a
function with the computed target still in X0 passes a pointer of unknown
origin, which makes module arbitration treat every writable table in the same
object as possibly modified.
`JTE_AArch64.ZlibInflateDispatchReachesEveryMode` assembles zlib's `inflate`
as GCC 15.2 compiles it for AArch64 (`test_jumptable_inflate_gcc_a64.s`). Its
31-mode state loop dispatches through `ldrh w0, [x0, w1, uxtw #1]`, and its
cases keep updating a 64-bit bit buffer. The test requires every target the
halfword table encodes. It needs two resolver rules. Structural value matches
must not resolve edge predicates: once the table's own targets are in the
graph, the predicates of the loop's branches exhaust the candidate budget. The
address role must follow the `uxtw` extension from the recorded index to its
scale; the fallback search over every scale in the loop also exhausts it.

`ModuleJumpTableBudget.*` uses a real ELF table inventory to separate the
400000-operation input boundary from scan, retained-source, queue and cumulative
transfer limits. It checks both function orders, unrelated code occurrences,
same-field independent contexts with no memory consumer, missing PC-relative
sources, repeated long blocks, final recording and cross-function exhaustion.
Every exhaustion must protect the nonempty request set. Existing cross-target
jump-table and returned-code tests exercise the shared collector's semantics.

`ModuleIntrinsicEffects` validates scalar value-only shapes across x87, BMI2,
ARM and AArch64. `ModuleJumpTableBudget` also checks a pure instruction after a
real call with clobbered input lanes, unused address inputs, later result/input
escapes, explicit temporary address operands of memory intrinsics and malformed
contracts. These tests retain real loader-backed relocation occurrences and
must protect table slots whenever an observable use remains.

`ModuleAdjacentTable.*` uses real ELF32/ELF64 adjacent absolute tables in both
function orders. It checks final x87 returns, independent roots, an unsplit
interior branch that bypasses the mask, physical fallthrough gaps, cloned TOP
contexts, malformed address operands, changed LOAD widths/addresses and FS/GS
spaces. Escaped temporaries and provenance aliases, duplicate sources and a
source-only context retain the preceding object. The source-only control removes
only its occurrence, leaving its LowOps unchanged. A three-table positive and
cumulative exhaustion control verify withdrawal of earlier grants. The expanded
physical-envelope test checks end-to-end preservation, including the later
LOAD/alias veto; it does not isolate the envelope comparison. Actual CLI HighC
and optimized/unoptimized LLVMC output execute all masked indices at host O0/O2.
These fixtures do not establish support for exceptional graphs or other
architectures in the new instruction-local query. The real PE32 CFI cases require
`i686-w64-mingw32-ld`; they assert an actual FDE, nonempty recovered tables and
exact field occurrences before testing ordinary-unwind admission and incomplete
augmentation/indirect-LSDA refusals. Metadata negatives cover outer records,
fragments, explicit entries, malformed identifiable starts, reversed order and
cleared final annotations. Per-slot inventories must completely partition the
fresh runtime extent; gaps, duplicates, extra slots and changed strides refuse.

`ItaniumEHDriver` and `DarwinUnwindSymbols` check section-wide structural failure
propagation across x86/x64/AArch64, mixed good/bad record orders, unknown CIE
augmentation, unresolved indirect LSDA, malformed tails and language refresh.
Darwin covers standalone and both compact/FDE attachment paths, unrelated
compact-only records, missing personality arrays, unknown providers, direct and
bound-slot conflicts (including equal nonzero file values, equal names and
unnamed relative relocation targets),
LSDA conflicts, invalid ranges and retained foreign/oversized FDEs. Complete
frame sections add no language-refresh provenance. These are decoding/source
proof checks; they do not certify native exception execution.

The same target's `NarrowGuard*` tests use long arithmetic prefixes before x86
and x64 byte/word guards and AArch64 W-register guards. Low/high byte identity,
disjoint writes and explicit index widening must retain exactly three slots
through normal LLVM emission and verification. Changed lanes, other bytes,
unbounded upper bits, call clobbers and exhausted budgets must not borrow the
earlier bound. The fourth physical slot is deliberately outside that bound.
An unrelated deep return expression exercises incomplete escape auditing:
the proven table survives only with every relocation root retained.

`LowToMedSelectorOccurrence.*` checks distinct SSA selectors for copied
instruction addresses on x64 and AArch64 across ELF, Mach-O and COFF. Changing
one copy's operand role must invalidate only that copy, and the complete
LLVM module must verify. Pointer-boundary regressions retain composite table
loads whose dispatch copies lose or change a recipe, while accepting a load
in their shared predecessor when every recipe survives.

The pointer-boundary target checks closed scalar graph retries on x64 and
AArch64 across ELF, Mach-O and COFF. Arithmetic deeper than the initial
recursive walk can succeed; address seeds, forbidden dependencies, mixed code
slots and chains beyond the independent graph depth limit must still fail.

The pointer-boundary target's `LLVMFrameSlotProof` cases exercise shared PHI
diamonds and loop-carried SELECT DAGs for x86, x64, ARM and AArch64. Deterministic work
counters bound proof expansion independently of machine speed. Conflicting
coordinates, nonzero recurrences, unanchored cycles, lossy pointer casts and
query-local budget exhaustion must fail closed. Exact slots, affine recurrences,
scalar ranges and frame intervals consume one saturating budget, including
nested queries. Interval regressions reject independent rootless cycles and
mutually seeded recurrences that eventually grow into another stack slot;
reversing PHI-arm order must preserve the result.
`LLVMFrameSlotProof.ParallelStackPointerPhisAnchorOnlyInOneBlock` accepts an
i386 back edge that returns ESP through the RSP PHI of the same header, and
refuses the same RSP PHI placed in another block.
Repeated reaching-store iterations and successor edges reuse an unbound alias
proof only for the identical address and write width within that one reload
query. Overlapping and differently sized writes retain their independent
checks. Shared scalar LOAD/SELECT graphs also have deterministic expansion
counts across all four architectures and ELF, Mach-O and COFF: completed scalar
and exact frame reload proofs are reused without exhausting the path budget.
Opaque loads of pointer width, address-owned arms and forbidden values remain
rejected; recurrence and frame-memory assumptions cannot escape their scope.
An earlier scalar load from a slot cannot authorize a later pointer load from
that same slot, in either operand order.

`NeverDLLVMCValueTests` additionally checks relocated bytes that resemble
strings, generic builtins compiled for a different source ISA, and dead image
address calculations. The latter must retain volatile/atomic loads and
observable calls while leaving the input LLVM module unchanged.
Integer-initializer regressions give scalars, arrays and records values that
match known image string addresses, then compile and execute their generated C
at O0/O2 with x64 and AArch64 writer options. Symbolic global references retain
their pointer relocations. The MinGW whole-module regression separately checks
that the runtime's integer switch tables produce compilable LLVM C.
Return-only join tests compare 6144 full-status cases at each O0/O2 level against
independently compiled LLVM, checking call counts and volatile output effects.
They also require direct returns in the C and unchanged input IR. Effectful
joins, exception personalities and address-taken return blocks are refused.
Concise GEP tests exercise 4096 address calculations per width and optimization
level, including negative narrow indices, scaled wide indices and wraparound
in both 32- and 64-bit address domains, with undefined-behavior traps.
Single-bit condition tests compare 28,672 cases per O0/O2 level against
independently compiled LLVM at 8/16/32/64 bits, including signed and unsigned
comparisons, inverted flags, narrow sign bits, shifts and boolean truncation.
Volatile word reads/writes and intervening observer calls must retain their
values and counts, and caller IR must stay unchanged. Negative and budget cases
retain freeze, variable/oversized shifts, arithmetic, independent bit sources,
poison, multi-bit masks and overly deep graphs; shared DAGs remain bounded.

## Interpreter recovery checks

`LLVMCValues.RepeatedParameterFieldAddressesPreserveMemoryObservations`
compares x64 and ARM64 C projections against independently compiled LLVM at
O0/O2. Each projection checks 2,048 complete-buffer cases with unaligned and
overlapping accesses, negative field offsets, branch-dependent stores and an
opaque call that changes the observed memory. Undefined-behavior traps remain
enabled; repeating an address must not reuse the value previously read there.

`NeverDBytecodeObservation` covers input-bound call-site source evidence through
HighC, LLVMC and optimized LLVMC, compiling and executing successful and failed
state gateway calls. It rejects stale reports and mismatched call occurrences.
Symbol identities are tested through complete memory transport, partial stores
and opaque-call invalidation with 32- and 64-bit results. `SymValueIdentity.*`
also checks both byte orders, constants, arithmetic and conflicting identities.
`CSourceEvidence.*` exercises the shared Windows/ELF source model, mismatched
functions, ambiguous spans and hostile comment text. It does not claim native
Windows process execution coverage.

`StateSource.*` checks contract rejection before mutation, exact pointer
round trips, Windows x64/ARM64 ELF call reconstruction and unknown-site
fallback. `StateSourceCompiled.*` compares the complete unaligned state bank,
guest writes, signed arguments/results and nonzero statuses against independent
gateway implementations at `-O0`/`-O2` with undefined-behavior traps. Partial-selector tests cover both
byte orders, byte/halfword/word slices and an intervening overwrite. These checks use synthetic inputs;
they do not authenticate a producer's isolation or host-service assertions.
`LocalBankPreservesEveryByteAndOpaqueBoundaries` compares independently
compiled original LLVM and emitted C at O0/O2 with UBSan traps: 2,048 inputs per
level vary unaligned banks, overlapping partial writes, loops, every-byte opaque
mutations, pointer identity, guest effects, call traces and full-width failure
returns. Unsupported selected bank addresses retain the original body. These
checks cover the implementation under the explicit private-bank assertion;
they do not prove that arbitrary callers satisfy that assertion.
`BankEffectsPreserveCallsLoopsAndCompleteState` adds three independent
LLVM-versus-C cases at O0/O2 with UBSan traps (2,048 inputs per configuration):
recursive callees, conflicting loop/backedge writes, partial result overlap,
full-bank gateway reads, bounded and unbounded fallback writes, fixed fallback
widths, arbitrary opaque mutation, guest effects and nonzero statuses. Separate
refusal tests cover weak/interposable and indirect callees. Schema checks reject
malformed fallback assertions before mutation. LLVMC unsigned saturation tests
cover 1/8/16/32/64/128-bit add/subtract, boundaries, operand evaluation, assigned
and inline expressions, name collisions and malformed shapes.
The Python native integration suite checks static/callback producer parity,
strict schema admission and reconstruction counts; the CLI retains existing
output after a rejected source contract.
String cases cover indirect bank capture, both byte orders, missing assertions,
unknown/out-of-range offsets, unknown keys and missing terminators. The compiled
string oracle varies bytes (including the terminator), normalization modes,
module sentinels and bank alignment at O0/O2 with undefined-behavior traps,
comparing complete bank bytes, guest effects, native-call traces and failure
statuses. Native Python tests exercise static/callback parity and reject
malformed images, string declarations and exclusions through the shared C ABI.
String spelling also rejects missing call-site contracts, writable globals and
externally initialized globals, preserving ordinary address identity.
Pointer cases reject mixed selector modes, incomplete/duplicate source slots,
narrow loads and unknown offsets. Windows x64 and ARM64 IR retain address and
arity checks even after opaque bank calls. The compiled pointer oracle compares
2048 cases at O0/O2, including target replacement, null pointers, changed cache
contents after capture, signed results, unaligned full banks, guest effects and
nonzero gateway statuses. Python tests check static/callback parity, underscore
symbols and strict pointer-contract parsing. These are synthetic ABI cases, not
native Windows import-table or whole-program execution coverage.
The pointer oracle also supplies an unsigned header overload for its signed
import, so address guards and call operands must preserve the declared signature.
The variadic compiled oracle checks 1024 complete-bank cases at O0/O2 using
actual `va_arg` consumers, signed/unsigned 32- and 64-bit operands, a small
64-bit constant and a pointer. It checks guest effects and return values;
native Python parsing rejects zero, fractional and oversized fixed prefixes and preserves
explicit promoted types at guarded pointer call sites.
Void-capture cases exercise full and partial overwrites on both byte orders,
branch coverage, intervening observers, mixed bank/guest addresses, cycles and
early exits. The independent compiled oracle compares 2048 unaligned complete
banks, guest effects and statuses at O0/O2 while varying the gateway's otherwise
unspecified captured bits. It exercises both the native void arm and failed
arity fallback. Native Python tests verify static/callback parity, the legacy
zero policy, proof failure and strict void-policy schema admission.
Scalar-selector cases cover both byte orders, partial slices, multiple bank
transports, unlisted candidates and unknown writers. The compiled oracle checks
4096 full-bank cases at O0/O2 with undefined-behavior traps, changing the selector
across an opaque call, varying upper bits and argument counts, and comparing
guest effects, signed results and full-width failure statuses. Native Python
tests verify static/callback parity and preservation of the runtime guard and
original fallback. PHI/select candidates include conflicting arms and loop
backedges. The compiled oracle executes both straight-line and variable-trip
loops, preserving complete banks and per-iteration effects across native and
fallback paths.

Narrow-signature cases check signed and unsigned 8- and 16-bit results on both
byte orders. The compiled default-argument oracle compares 8192 cases per
optimization level, varying all four result types, 1/2/4/8-byte destinations,
supplied counts, signed operands, null pointer defaults and bank alignment.
Full banks, guest effects, native traces and failure statuses must agree at
O0/O2 with undefined-behavior traps; both accepted and fallback paths execute.
Native Python tests check static/callback parity, zero supplied operands,
out-of-range counts and rejection of malformed or overflowing defaults,
non-null pointer defaults and unpromoted narrow variadic operands.
Forwarded-target cases reject conflicting identities, malformed target indices,
missing required slots and target normalization before mutation. Windows x64
and big-endian ARM64 IR check target removal at different operand positions and
signed indirect signatures. An independent compiled oracle compares 8192 cases
per O0/O2 level with undefined-behavior traps: replaced and null targets,
captured operands whose slots are overwritten, optional pointers, padding beyond
the available bank slots, signed narrow results, all four store widths, full
unaligned banks, guest effects and failure statuses. Python static/callback
producers share padding and target admission through the native C API.
Context-forwarding cases reject absent, non-pointer, variadic and excess gateway
parameters before mutation, including conflicting signed declarations after
receiver insertion. Windows x64 and ARM64 IR cover named, pointer-candidate,
forwarded-target and string calls. The compiled oracle compares 6144 cases at
each O0/O2 level with undefined-behavior traps and uninitialized-variable errors:
distinct and null receivers, receiver/guest aliases, mutations before the call,
reentrant calls using another receiver, signed arguments/results, defaults,
variadics, full unaligned banks and fallback status/effects. Python static and
callback producers share strict boolean parsing and missing-context rejection.
These tests establish explicit receiver forwarding, not original object layouts
or native Windows runtime behavior.
Guest-selector tests check endian-aware slices and unrelated offsets. Their
4096-case independent LLVM/C oracle at O0/O2 changes the bank-held base, guest
bytes and argument count across an opaque call. Both successful guarded calls
and full-status fallbacks preserve unaligned banks, guest bytes and call traces
with undefined-behavior traps.
`LLVMCValues.PreservedIndirectChainsKeepLoadsStoresAndCapturedTargets` checks
1024 nested-pointer cases at each O0/O2 level against independently compiled
LLVM, with undefined-behavior traps. It preserves observable target snapshots
and calls the captured value after another call replaces or nulls the slot.
The compiler also rejects any emitted read of an uninitialized target.
`LLVMCVoidAnalysis.FreezeOfIntegerCall*` covers joins between an integer call
result and a memory load. Its 1024-case C oracle checks both branches, the
stored/returned bits and exact call counts at O0/O2 with undefined-behavior
traps. Unproved shift, undef and poison arguments still refuse emission.

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

The profile/callback source matrix also exercises loops, guest stores and nested
status-propagating calls through a synchronous external decoder. Malformed
callback recipes share the profile's bank, width, temporary, overlap and budget
checks. Layout and instruction-reply JSON reject ambiguous or unknown fields.

The optional source-context contract is exercised on both AArch64 and x64
carriers through both C routes: nested calls, an entry back-edge, full-width
failure status, reentrant execution, null context and context/state aliases.
The public C API also checks aliases on an unaligned bank through the optimized
LLVM route at O0/O2 with undefined-behavior traps. CLI checks retain an unused
context parameter in leaf functions. C/Python tests validate the option and
response contract in check and emission modes, including rejection before
decoder invocation and Python compatibility with older unary-ABI responses.

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
Their callback modes exercise actual C and embedded Python trampolines.
The C API callback tests cover PC/context-dependent opcodes, variable sizes,
function-bounded input windows, reply-copy lifetime, missing/repeated replies,
and early rejection of invalid buffers and requests. Python tests cover native
reentrancy/concurrency, 64-bit PCs and exceptions (including `BaseException`)
re-raised only after the owned native response is released.

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

Additional regressions preserve pure bit-operation intrinsics in both memory modes and byte orders, while retaining barriers for ordinary calls, operand bundles, convergence, lifetime, memory effects and traps. Original and rewritten code run at O0/O2 with undefined-behavior traps against an independent oracle that checks the numeric return address and every buffer byte.

Byte-liveness regressions cover disjoint reads, partially observed writers and coverage assembled from multiple later stores. Guarded-address cases cover AND/OR/XOR, 32/64-bit widths, both byte orders, wrong roots, incomplete masks, bypassing joins and every address-budget boundary. O0/O2 oracles exercise both branch outcomes, all sixteen address residues and every output-buffer byte.

Address-relation regressions exercise integer and pointer PHIs/selects, both byte orders and pointer widths, stable and changing backedges, undefined/frozen edges, unanchored cycles, adjacent address/output budgets and address-only analysis invalidation. O0/O2 loops compare every return and every buffer byte with an independent per-iteration oracle, including a moving address that must not be treated as constant.

Numeric-memory cases check full and contained single-writer forwarding, undef/poison without added snapshots, both byte orders, 32/64-bit representations, modular negative offsets, partial overlap, aliases between different roots and allocas, throwing calls, loops, adjacent work/use budgets and store-only analysis invalidation. O0/O2 execution compares original and transformed IR against an independent oracle for both the return value and every byte of an aliased buffer.

`NeverDMedMutableSourceTests` and `NeverDLLVMCValueTests` execute independent loops, reordered blocks, entry backedges, runtime stack arithmetic, earlier reads, branch joins, partial aliases, Boolean truth values and zero-inclusive bit counts at O0/O2. Negative cases reject malformed inputs, truncated targets, ambiguous carriers and exhausted budgets before emission. A CLI fixture above the SSA limit requires executable LLVMC output and explicit HighC refusal. Repeated updates and stored-expression chains across blocks also check C output size and execution.

LLVMC aggregate tests link generated C with independently compiled LLVM
callers and callees at O0/O2, checking both words of array results and exactly
one observable call. They also execute nested insert/extract, PHI/select,
array storage and unaligned copies with whole-buffer comparisons. Raw array
call boundaries currently admit one/two i64 words on Linux/Darwin AArch64 or
SysV x64, with array arguments restricted to i64/pointer signatures that
fit the argument registers. Other layouts, Windows, custom conventions,
variadic array arguments and register exhaustion fail explicitly. Native ABI
execution runs on the host target; unsupported hosts skip that comparison.
Generated size/field-offset assertions reject incompatible C object layouts.

Additional regressions bound private loads/stores before LLVM promotion and C output size. They execute long mixed arithmetic chains, reordered SSA blocks, overlapping guest writes and zero returns at O0/O2, repeat key checks through the actual LLVM optimization pipeline, and reuse an emitter after rejected module generations.

Compound-condition regressions execute conjunctions and disjunctions with nonzero equality constants, unsigned comparisons, signed comparisons in both operand orders, widened Boolean inputs and every Boolean-negation combination. C emission must preserve the complete truth table at O0/O2 and must not dereference a missing zero-comparison operand. Integer-address stores cover aligned and unaligned 32/64/128-bit carriers; byte backing arrays retain explicit alignment and exact base/partial accesses without scalar array assignments or incompatible typed aliasing.

`NeverDLowIRRefinementTests` checks actual recovered residuals, differently structured finite loops, zero iterations, distinct dynamic producers, guarded witnesses, shared overlapping input views, correlated copies and spills, both-sided immutable-read evidence, mandatory system flags and return-slot preservation. Wrong candidates, extra writes, incomplete or infinite paths, stale evidence, scratch collisions and exhausted shared budgets must refuse a certificate. Existing independence tests continue to reject observable arbitrary values.

`CompleteModel`, `CompletedTargetFacts`, `ConditionalImplication` and `PartitionedCoverage` cases in `NeverDLowIRRefinementTests` use exhaustive small-domain oracles and malformed-input, stale-cache, incomplete-enumeration and exact/short-budget controls. Actual LowIR and binary refinement fixtures check conditional products and all original/recovered terminal arms at fixed gate limits, rejecting changed final observations, unrelated domains and missing targets. `FiniteValues` tests distinguish encoding failure from search, value-count and global-budget refusal.

`LowIRLoopRefinement.*` and `BinaryLowIRLoopRefinement.*` in the same target exercise arbitrary 64-bit counts, nested lexicographic ranks, actual native residuals, entry-prefix templates, overlapping views and correlated spills. Negative controls reject incorrect bodies, narrowed entry domains, nondecreasing ranks, unsigned wraparound, forgotten prior writes, missing cuts, malformed templates and exhausted shared budgets. Successful finite siblings never authorize an incomplete induction proof.

`LowIRLoopInference.*` and `BinaryLowIRLoopInference.*` use independently authored counters, spills, early returns, native calls and packed flags. Regressions cover narrow arithmetic widening and semantically equal flags with different expressions. Malformed graphs, absent or forged origins, nonterminating/wrapping loops and exhausted inference or proof budgets must never yield a certificate.

Zero-prefix regressions check concatenation grouping, unusual widths and exhaustive byte pairs while retaining unknown and nonzero bits. Independently authored frame loops cover separate low-value/high-zero stores in both byte orders, narrow widths, wrong arithmetic and padding, exact/short inference limits and a separate complete-proof query budget.

Shared-header and shared-latch regressions cover zero-extended 32-bit and full 64-bit counters, non-unit scalar ranks, wrong results, stuttering and wrapping paths, and exact or exhausted budgets across scalar and tuple search. `LowIRLoopInference.SharedHeaderAndLatchNeedLexicographicRanks`. Additional increment/reset regressions require convergence without unfolding one counter bit per round and reject missing progress and unsigned wraparound. Scheduling regressions cover carried non-unit accumulators, wrapping unit noise beside a valid non-unit scalar rank, and three counters whose successful tuple lies beyond the early window. Exact and one-short rank budgets check deterministic continuation without repeated proposals.

`LowIRLoopPlanPairing.*` in the same target checks renamed registers, different arithmetic bodies, side-specific prefix snapshots, retained predicates, shared frame inputs, nested cut coverage and fresh proof budgets. Missing relations, incorrect writes, malformed temporary bindings, incomplete pairings and exhausted metadata limits must not establish a certificate.

`LowIRLoopAlignment.*` checks independently authored ordinary and rotated frame-counter loops: both default self-plans prove individually, their first pairing fails, and another candidate cut proves the relation. Regressions cover multiple-cut permutations, wrong results and frame writes, missing/stale original records, explicit undefined witnesses, nondecreasing/wrapping counters, malformed graphs, cumulative failed-attempt queries, exact total budgets and exhausted search limits. Every refusal must lack a certificate. Additional cases cover separate reset/progress phases, equivalent relocated exit guards requiring cross-family pairing, shared caching without repeated inference, and combined metadata exhaustion. A trailing independent cycle checks complete feedback coverage with an explicit 16384-query inference limit. Empty and duplicate families use no symbolic queries; too few cuts refuse. Exact and one-short global budgets, wrong results, missing progress, original evidence and undefined witnesses remain checked. Filtered-family regressions cover neutral arithmetic diamonds, local versus boundary-only joins, and a reachable join that can be bypassed to an exit or loop boundary. They check a filtered candidate even when the original filtered family is duplicate, reuse of a filtered plan before a later broad attempt, exact/one-short/zero `MaxCutSelectionWork`, cumulative failed `CutSelectionWork`, and no symbolic inference after global graph work is exhausted. Complete cycle coverage is checked for both branch families; the diamond relation uses explicit inference and proof query limits.

`LowIRLoopAlignment.DirectRanks*` should check renamed register/frame carriers, compatible frame unions, conflicts at either binding endpoint, and conservative refusal of constant or expression ranks and actual 8-byte versus 4-byte frame inputs. The accounting oracle must derive optional scan, comparison and metadata costs from the public self-plans before testing exact and one-short limits, rather than copying the successful search's total. Duplicate proposals must consume attempts and work without new checker queries; exhausting the optional cap must still allow a later ordinary pairing to prove. Wrong results, frame or preserved-state changes, missing progress, and stale original evidence must remain rejected with complete observations and the selected witness intact. `InterpreterLLVMRefinement.PreservationRestoresEntryAcrossLoopCutpoints` should discover the source plan through public search without manual cut addresses or rank bindings, then prepare fresh models and recheck both native and LLVM premises in the composite checker. These requirements do not assert a test-run result or ordinary ABI certification.

`LowIRLoopAlignment.PlanCutCap*` checks zero, short and exact plan caps, zero symbolic queries for oversized families, complete cycle coverage, malformed graphs and later filtered proofs in both directions. Standalone inference remains compatible. `LLVMModel.RejectedBodyTemplateDoesNotHideAValidGuardedHeader` also checks that a one-cut alignment cap permits multiple inference attempts. Wrong results must still refuse a certificate.

Partial-counter regressions cover frame and register lanes, both directions, low/interior/high positions, unusual widths, both byte orders and three-byte represented frame words. They check delayed lane discovery, preserved-bit mutations, nonprogress and unguarded wrap, invalid additional entries, exact/short inference budgets, and existing single-cut search.

Leading-phase regressions cover two and three sequential loops reusing one countdown word, composition with existing nested-loop phases, exact and one-short rank/query budgets, nonprogressing loops and resets back to an earlier phase. Same-width corrupted phase constants, wrong results and frame writes must fail the complete checker without a certificate; missing original evidence remains unsupported.

`InterpreterMachineStateModel.*` in `NeverDLowIRRefinementTests` uses independent LowIR examples to check raw entry flags, status versus guest RAX, all 17 state words, partial register lanes, packed flags, sticky dynamic rejection, guest frame writes, both branch arms and cyclic inference followed by a fresh proof. Wrong outputs, lost status, changed memory, stale instruction records, malformed inputs and exhausted generation budgets must fail. Existing machine-source tests also exercise both C routes at O0/O2; model tests alone do not certify compiled C.

`NeverDLLVMInterpreterModelTests` checks independently written LLVM against full-state LowIR oracles: widths, parallel PHIs, switches, guest memory, separate status, poison guards, intrinsic ranges, rejected contracts and all four construction budgets. Constant state GEPs retain 8/16/32/64-bit element allocation strides, chained negative offsets and complete byte observations; wrapped scaling, object escapes, dynamic indices and guest-pointer GEPs remain refused. It checks a complete arbitrary-word countdown proof and rejects changed status. Independently written C compiled at O1/O2 must match the same observations. These tests validate the admitted model; automatic invariant discovery and compiler correctness remain separate obligations. Variable-shift cases cover all four widths, masked and branch-bounded counts, boundary and oversized counts, no-wrap/exact flags, strict poison rejection and compiled C at O1/O2.

`LLVMGuestAlignment.*` compares loads and stores with independent byte-memory oracles: aligned and misaligned domains, free high address bits, parsed default alignment, partial widths, unused or overwritten accesses, unreachable branches and exact/one-short construction budgets. `InterpreterLLVMRefinement.GuestAlignmentRequiresBothFreshPremises` checks native stack stores, matching entry congruences and altered source effects through both fresh relations.

`LLVMByteSwap*`, `LLVMScalarByteSwap.*` and `InterpreterLLVMRefinement.ByteSwapRequiresBothFreshPremises` check independent byte-copy and shift/mask oracles, preserved upper bytes, cross-block values, retained poison, strict call contracts and independently counted exact/one-short budgets. Clang O1/O2 fixtures require actual byte-swap intrinsics; small native byte-exchange/BSWAP fixtures check both fresh premises and reject changed values or lost upper-word clearing.

`NeverDLLVMScalarEquivalenceTests` checks complete loop domains, zero iterations, simultaneous PHI swaps, switches, high input bits, last-partition counterexamples, poison-producing extra updates, return ranges, unsupported contracts and exact/short/zero budgets. Independent double-width and overflow oracles cover funnel endpoints and guarded products at every admitted word width; independent nested-loop C at O1/O2 checks the compiler input profile. The state-model suite also checks funnel endpoints. `SymExpr.ConstantWindowSharesActualWorkWithoutRelaxingQueryCeilings` checks shared query accounting and unchanged local limits.

`LLVMScalarDecision.*` covers deep exact-shift/extension obligations, constant branch decisions, both loop backedges, retained high data bits, late undefined operations, nontermination, edits to previously checked functions and exact/short/local budgets. `LLVMScalarDecisionCompiled.DeepOneAndTwoBackedgeOracles` compares independently authored one- and two-backedge recurrences with an unsigned C oracle at O0/O2 over 32,768 calls. These are scalar-model checks, not native ABI or whole-binary recovery coverage.

`LLVMScalarDemand.*` proves separate nonlinear loop bodies within a fixed work budget, retains all sixteen control partitions and free high input bits, rejects last-partition output/definedness failures, and checks exact/short work and node limits. `LLVMScalarDemandCompiled.*` compares both bodies with an independent unsigned arithmetic oracle at O0/O2 over 262,144 calls. These tests exercise query scheduling without removing source operations or narrowing the input domain.

`SymKnownBitsTests` checks facts against all byte-input pairs and arbitrary-precision boundary values, including extension identity, nonwrapping sums, distinct roots and total shift semantics. It covers exact/short budgets, charged cache hits, bounded storage, separate contexts, depth, fan-in and unsupported widths. `SymExprExtensionTests` checks high constants and full-width shift counts. Scalar equivalence regressions retain symbolic data while discharging range guards, reject last-partition differences and executed poison, and account for the complete proof under exact/short budgets. `SymMBAExtensionTests` requires derivation with sample verification disabled, checks all byte-input pairs, and preserves signed, narrow-carry, complement and exhausted-work boundaries.

`NeverDLLVMScalarLoopRecoveryTests` covers prefix and predecessor-carrier recovery, zero-trip alternatives, self-latch rotation, affine states, wrapping equality fallback, extra-update poison, differing high data bits and unsupported input contracts. Exact/short cumulative budgets check atomic refusal. Independent arithmetic oracles execute original and recovered LLVM at O0/O2, including all byte control inputs. These tests do not establish native ABI recovery or default C output.

Loop-predicate regressions cover 8/16/32/64-bit carriers, swapped orderings and branch polarities, signed/unsigned widening, distinct external seeds, conflicting entry observations, multiple backedges, hidden symbolic-data counterexamples, refused truncation, original poison/nontermination, and atomic construction/proof/candidate/transform limits. Independent unsigned oracles compare original and recovered LLVM at O0/O2 over 458,752 calls with undefined-behavior traps. Source and parent module remain unchanged.

Exit-bound regressions cover modular guards at 8/16/32/64 bits, descending steps, both polarities, zero trips, shared exits, wrong neighboring bounds that agree only on zero data, unreachable step boundaries, oversized slices, poison, unavailable same-width leaves and atomic budgets. Forty unused arguments before a live prefix seed must not displace it from the bounded search. Independent unsigned O0/O2 oracles execute original and recovered LLVM over 458,752 calls with undefined-behavior traps.

The same target also checks `recoverLLVMScalarSource`: preparation before loop search, cleanup without loop changes, full-width unused state, dead overflow/exact-shift/division and assume obligations, unsupported effects, exact/one-short cumulative budgets and bounded continuation. Independent arithmetic oracles execute original and prepared LLVM at O0/O2 for every byte control and deterministic full-width state. Source and parent module must remain unchanged on success and refusal.

Source-preparation guard tests cover annotated modular identities, equivalent expressions in distinct predecessors, retained differing branch values, original overflow/exact-shift/truncation/extension refusals, and exact/short cumulative budgets. Independent unsigned oracles compare original and prepared bodies at O0/O2 over 131,072 calls with undefined-behavior traps. Standalone predicate rejection rules remain covered by the existing semantic-pass tests.

Mask regressions cover commuted operands, zero fields, retained high input bits, a screened alternative after full-data failure, every backedge, wrap/overflow rejection, batches larger than 32 and exact/short atomic budgets. Independent LLVM and emitted-C arithmetic oracles at O0/O2 check composition with width recovery. Self-query regressions retain complete control domains, poison/undef and unsupported-contract rejection, nontermination, local ceilings and exact/short work accounting; editing the same function invalidates any earlier outcome. These checks remain scalar LLVM coverage, not native ABI certification.

Shared mask-containment tests exhaust every byte-input pair, cover noncontiguous masks and widths through 128 bits, retain unknown/high bits, and bound node growth beyond the fan-in/width ceilings. A symbolic loop with two backedges must prove its masked-XOR recurrence against an independent closed form. Control-discovery tests charge normalized shift-count storage while preserving exact/short budgets and the wide-value fallback.

Width regressions cover nonzero literals, unchanged signatures, wider inputs and intrinsics, observable high bits, signed order, newly overflowing updates and exact/short budgets. The same scalar candidates run under x86-64, AArch64, big-endian AArch64 and ARM32 target triples; this is LLVM-level coverage, not native ABI certification. Independent original/recovered LLVM and emitted-C arithmetic oracles run at O0/O2, with C undefined-behavior traps.

Seed regressions cover agreement at every external entry, multiple backedges, hidden high data, poison, parallel swaps, split batches, more than 32 carriers and atomic budgets. Scalar proof tests distinguish completed unknown queries from global work exhaustion and prove safe shifts without enumerating data bits. Symbolic tests exhaust byte values, masks and counts, retaining observable high bits, source identity and large-count semantics. Cross-width shift views must share the full numeric count. These checks do not certify native ABI recovery.

Additional loop regressions cover narrow wrapped last indices, separate body/latch blocks, both guard polarities, swapped equality operands, reordered and descending unit-step carriers, zero-trip high-data differences, newly executed poison and wrong boundary guesses. Exact/short construction and proof budgets plus candidate exhaustion preserve atomic refusal. Original LLVM and emitted C run at O0/O2 against independent arithmetic oracles.

`NeverDLLVMCScalarLoopRecoveryTests` checks default whole-module and selected-function output, continued recovery after return-path cleanup, function identity and attributes, caller bindings, existing/new intrinsics and symbol collisions, shared budgets, effectful calls, missing input-definedness, metadata, image projections, external block addresses and foreign selections. Independent arithmetic and rotation oracles execute generated C at O0/O2 with undefined-behavior traps; arithmetic also compares independently compiled original LLVM across every byte control, boundary words and deterministic full-width data.
The memory-region tests compare generated C with original LLVM at O0/O2 under
UBSan. They cover aliased memory, indirect observer calls, pointer PHI swaps,
acyclic diamonds, zero-trip loops and shared failure-return PHIs. The observer
trace and complete buffers are compared, not only the final scalar return.


`SymSimplifyPredicates.*` also compares standalone and full-pass policy, reported work, exact/one-short budgets, a disabled phase and stamped functions. Scalar source regressions cover arithmetic-encoded loop stops at 8/32/64 bits, signed-overflow refusal and construction limits shared across rounds and functions. Failed publication retains the original IR; emitted C runs at O0/O2 against independently compiled original LLVM and an arithmetic oracle.

`SymKnownBits.*` checks lossless masks and signed shift roundtrips against exhaustive byte-pair arithmetic, including negative values, changed roots, discarded unknown bits, mismatched factors/counts and wide overshifts. Widths through 128 bits retain exact/short query budgets and no DAG growth. Scalar decision tests add positive and negative updates on separate backedges, stale-input and overflow refusals, symbolic high data bits, and 16,384 O0/O2 calls against an independent unsigned oracle.

`SymKnownBits.*` also exhausts byte pairs for scaled order and cross-width products, rejecting wrap, incorrect coefficients or factor multiplicities, and moving narrow overflow into a wider word. Queries through 128 bits retain exact/one-short work and no DAG growth. Scalar tests prove repeated addition and multiplication without enumerating data bits, retain every loop overflow obligation, and execute 16,384 O0/O2 calls against an independent oracle.

`LLVMScalarAssume*` checks complete loop domains, last-partition failures, unreachable versus reached false conditions, sticky definedness across all byte inputs, exact/short budgets, changed IR and unsupported call contracts. Four target triples exercise shared modeling; 8,192 O0/O2 calls check an independent unsigned oracle. The state-model suite independently checks the same obligation and operand-bundle rejection.

`LLVMScalarProjection.*` covers nested fields, bit windows, retained unused arguments, multiple returns, backedges, unselected overflow/shift/assume obligations, last-partition failures, nontermination, unknown contracts, changed input and exact/short budgets. Four target triples exercise shared semantics. `LLVMScalarProjectionCompiled.*` compares the original aggregate through an LLVM array bridge and projected windows against independent unsigned arithmetic at O0/O2. `SymExpr.RightShift*` exhausts byte pairs and checks sign extension, carries, retained high bits, full counts and bounded discovery.

`LLVMScalarInputs.*` checks ordered mixed-width mappings, zero/unnamed interfaces, dead arithmetic and assume demand, mutated outputs, unknown contracts, packaging refusal and exact/short cumulative budgets. Proof tests restore the complete original signature instead of fixing omitted inputs. `LLVMScalarInputsCompiled.*` executes original and reduced loop interfaces against an independent unsigned oracle at O0/O2 while varying all omitted arguments.

`NeverDLLVMScalarStateProjectionTests` covers overlapping and unaligned windows, all 8/16/32/64-bit cells, loops, entry masks, changed sources, status ranges, retained poison, external-memory refusals and exact/short budgets. Original memory bodies and LLVM aggregate bridges execute 172,032 O0/O2 comparisons against independent byte/arithmetic oracles. Scalar proofs remain separate. `SymKnownBits.*` exhausts byte pairs and checks 128-bit, changed-factor, wider-mask, wrapping-sum and budget boundaries. `LLVMCIntrinsicSemantics.AssumeEvaluatesItsConditionAndRefusesBundles` checks single predicate evaluation at O0/O2 and explicit bundle refusal.

Loop-metadata regressions compare counted loops with an independent formula across all control partitions and exact/short budgets. Large or zero peeling-history counts cannot hide wrong results, nontermination or poison. API-constructed malformed metadata exercises importer rejection separately from LLVM assembly parsing; machine-state tests retain state effects and input limits.

Initialization-contract regressions cover partial and separated byte ranges, fixed aliases, both branch arms, every return, first-iteration loop reads, and stores before loop reads. Read-before-write, missing writes, guest writes, unknown aliases, special memory accesses, out-of-object ranges and exhausted input/work budgets must fail. An independent output-only C fixture compiled at O1/O2 retains the exact LLVM attributes and passes fresh native-to-LLVM composition.

Guarded countdown coverage checks retry after a rejected body template, a complete arbitrary-word header proof, preserved shared cutpoint/query budgets, and immediate refusal of a real entry-contract violation.

`NeverDInterpreterLLVMRefinementTests` checks fresh native-to-LLVM composition, exact text/function binding, independent budgets, full observations and deliberately broader source domains. Changed bytes, residuals, results, flags, status, frame writes, poison and false/stale loop plans must refuse a composite receipt. Arbitrary-word countdowns require both inductive premises; independent C fixtures compiled at O1/O2 exercise actual serialized LLVM input. State-model regressions reject hidden entry backedges and bound roots without copying ancillary provenance.

`InterpreterLLVMRefinement.Preservation*` covers partial/overlapping ranges, malformed requests, independently counted preparation work, identical final clobbers, entry save/restore across loops, fresh opaque evidence and late refusal. Rebuild `NeverDPEFixedImageTests` as another API consumer. Compare omitted-request outcomes, counters and digests with the baseline separately.

`InterpreterLLVMRefinement.Collection*` checks required retention and deferral in finite and inductive proofs, reachable bad arms, late source refusal, entry preservation and all four native policy identities. Compile omission faults in the composition owner to verify that each required choice reaches the native checker.

```sh
cmake --build build-release --target NeverDLLVMCScalarLoopRecoveryTests --parallel 4
build-release/bin/NeverDLLVMCScalarLoopRecoveryTests
cmake --build build-release --target NeverDLLVMScalarLoopRecoveryTests --parallel 4
build-release/bin/NeverDLLVMScalarLoopRecoveryTests
cmake --build build-release --target NeverDLLVMScalarEquivalenceTests --parallel 4
build-release/bin/NeverDLLVMScalarEquivalenceTests
cmake --build build-release --target NeverDLLVMScalarResultProjectionTests --parallel 4
build-release/bin/NeverDLLVMScalarResultProjectionTests
cmake --build build-release --target NeverDLLVMScalarStateProjectionTests --parallel 4
build-release/bin/NeverDLLVMScalarStateProjectionTests
cmake --build build-release --target NeverDLLVMScalarInputProjectionTests --parallel 4
build-release/bin/NeverDLLVMScalarInputProjectionTests
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

`FreshChoiceProjection.*` belongs to `NeverDInterpreterSpecializationTests`. Independent symbolic contexts check exact node order against ordinary substitution; byte-input oracles check projected values and retained choices. Regressions cover context and leaf admission, zero and exact capacities, fallback, and total traversal work on shared incremental chains. Timing thresholds are not used.

`NeverDLiftTemporaryTests` checks complete temporary byte allocation, unchanged
scalar/placeholder spacing and exhaustion. Strictly decoded x64 multiplication,
AArch64 high multiplication and ARM vector accumulation run through `SymExec`
against independent limb arithmetic and lane results. CF/OF and high product
bits remain observable. `InterpreterSpecialization.WideInteger*` uses an
independent byte-vector executor for signed/unsigned products, both byte orders,
unknown high bits controlling both branches, numeric byte slices, malformed
shapes and exact/short budgets. `BinaryLowIRRefinement.WideInteger*` checks raw
binary completion while retaining native-proof and both source-layout refusals.

`InterpreterSpecialization.WideIntegerConcat*` checks both byte orders, high/low words, constant inputs, overlapping output, high-word control, malformed shapes, exact/short budgets and retained unknown-memory provenance. Strictly decoded `PSHUFD` covers all 256 selectors with aliased and distinct destinations against independent dword indexing. `BinaryLowIRRefinement.WideIntegerConcatCompletesOnlyRawRecovery` retains native-proof and both source-layout refusals.

`InterpreterSpecialization.Timestamp*` checks complete samples, zero upper halves, independent and repeated reads, preserved registers/flags/memory/segment bases, exact and insufficient budgets, malformed bundles and unknown address origins with independent value/state oracles. `BinaryLowIRRefinement.TimestampCompletesOnlyRawRecovery` checks raw completion and continued native-proof/source-layout refusals. Existing `X86_64_Intrinsics` timestamp cases check source emission and single-read full-bit results; `DriverTimestamp` separately checks the shared Unicorn/KVM/WHP clock policy, state preservation, budgets and overflow. Platform skips do not establish backend execution.

`InterpreterSpecialization.CPUQuery*` checks original selectors, all four
outputs and upper-half clearing, independent repeated queries, exact/short
budgets, malformed bundles and unknown-address stores. Independent value and
complete-state oracles cover the recovered operations and preserve unrelated
registers, flags, memory and segment bases. Timestamp regressions exercise the
same architecture dispatch. `BinaryLowIRRefinement.CPUQueryCompletesOnlyRawRecovery`
checks raw completion while retaining native-proof and both source-layout
refusals. These checks establish no CPU-feature profile or backend execution.

`InterpreterSpecialization.SavedFrameAddress*` checks indexed and conditional
writes against an independent complete-byte-memory oracle, modular addresses,
partial overlap, later overwrite, disjoint saved cells with a writable gap,
projection and exact/short work and query budgets.
`BinaryLowIRRefinement.SavedFrameAddressRetainsIndexedProofBoundary` checks
strictly decoded native bytes. Unrestricted symbolic indexing still refuses a
native certificate; separately bound selector controls prove complete frame
writes and reject a corrupted byte store. Run the full specialization, original
binary independence, PE fixed-image, LowIR independence and LowIR refinement
owners after changing this shared preservation boundary.

`SymMaterializedRanges.*` in `NeverDSymMemoryKnowledgeTests` checks bounded extent projection without new nodes or bytes, exact and short budgets, modular wrap, holes, adjacent partial writes, invalid/noncanonical bases and post-havoc history. `InterpreterSpecialization.SavedScalar*` checks arbitrary narrow/full-width spills, indexed and conditional writes against complete byte-memory oracles, partial overlap, later overwrite, constant projection and exact work/query limits. Its external-store case must still refuse without a separate return-slot alias contract. These tests supplement the saved-address and writable-gap checks above.

`InterpreterSpecialization.SupervisorRead*` checks ECX selectors with nonzero
upper RCX bits, full-width results, zero upper destination halves, independent
and repeated reads, exact/short budgets, malformed bundles, required supervisor
domain and unknown-address stores. Independent value and complete-state oracles
preserve unrelated registers, flags, memory and segment bases.
`BinaryLowIRRefinement.SupervisorReadCompletesOnlyRawRecovery` includes a proved
supervisor return control before checking raw RDMSR completion and continued
native-proof and both source-layout refusals. User-profile reads still refuse.
These tests do not establish runtime MSR accessibility or backend execution.

The following behavior uses the default strict audit contract. `NeverDOriginalBinaryUndefinedIndependenceTests` uses independent fixed-map x64 bytes to test physical native CALL/RET, modified return targets, exhaustive finite indirect targets and immutable loads. The same target checks complete direct-branch collection, exact byte/effect/mapping/read-witness binding, outer-return preservation of entry RSP and its return slot, and image-disjoint frame feasibility. Missing or overlapping instructions, unaudited arms outside the exact trap and explicit profile-projection rules, nonterminating or over-budget loops, incomplete target enumeration, profile/contract mismatches and exhausted budgets must refuse without a certificate or residual code. Success requires every feasible native path to finish. This opt-in gate does not certify loop invariants, exception dispatch, CET-enabled execution or native-to-C equivalence; ordinary recovery remains separate. The target also checks strictly lifted `INT3`/`UD2` terminal boundaries and binding of their complete bytes and operation digests. A `Missing` undefined-output sidecar must remain `Missing`; only symbolically unreachable traps may appear in a certificate, while any feasible trap path must return `ContractViolation` without a certificate or residual code. Trap fallthrough and exception recovery are not modeled, `codeFollowsTrap` is not used, and static LowIR API support remains unchanged.

Physical-return regressions cover direct and indirect calls that skip invalid inline bytes, feasible bad continuations, complete target enumeration and exact/short budgets. Full-state refinement rejects altered results. Independently compiled C at O1/O2 must also preserve the complete frame write; matching return values cannot hide a changed return-slot byte.

Explicit native-overlap tests check actual x64 branches into immediate operands, both feasible branch outcomes and indirect return entries inside earlier instructions. Synthetic provider tests check contained overlaps in both collection orders, conflicting bytes on an untaken direct arm, code/read consistency in both orders and candidate reads. Exact and short byte budgets count duplicate overlap bytes across indirect transfers. Changed branch results, static or loop use, and contradictory evidence must refuse certificates; enabling the option or changing its limit changes the digests.

Opt-in audit-boundary tests cover dead RCL, LOCK memory XADD and REP MOVS, symbolic path contradictions, arbitrary-controlled branches and exact refusals on entry, indirect, CALL and RET arrivals. They check independent access to a reachable suffix, candidate/native address collisions, malformed or partial evidence, resource exhaustion, static/loop API rejection and all three refinement digest layers. Changing an unreachable instruction or enabling the option with no retained boundaries changes the certificate digests. These tests establish the declared finite proof scope, not semantics for the unaudited instructions.

Packed-flags tests cover all scalar entry-flag combinations, privilege masks, both-execution TF/AC guards, distinct undefined producers, correlated copies, native calls, sibling state, mandatory final system-state observation, malformed evidence and charged resource limits. Finite loops must exhaust every feasible input path; a safe sibling cannot hide an infinite or truncated path. RDSSPD/RDSSPQ checks cover all 16 general-purpose registers and both widths, unchanged high bits, retained `Missing` evidence and rejection of forged projections. Machine-state tests compare both C routes at O0/O2 with undefined-behavior traps against an independent user-mode flags oracle and check sticky profile failure. INCSSPD/INCSSPQ tests cover both widths and every general-purpose register, unreachable-boundary retention, feasible traps after a completed sibling, zero operands and forged trap evidence.

`NeverDX86DecodeDetailTests` covers all three decode routes, both x64 address widths, signed displacement boundaries, mandatory prefixes, genuine i386 disp16, moffs, truncation and detail-free reuse. Exact relocation occurrences bind; wrong widths, offsets and values do not. Native independence and refinement tests additionally retain full frame writes, reject observed arbitrary flags and reject an altered shift candidate.

`NeverDLowUndefinedDigestTests` checks independent SHA-256 vectors, every stored field, signed sequence bits, order, padding exclusion and unchanged inputs. Cases cover inline-buffer growth, the 199/200-operation boundary and larger streaming spans. `LowIRRefinement.StaleUnusedInputRefusesAcrossDigestStorageBoundaries` checks real stale-evidence rejection and rebinding across both paths. Keep existing `InputDigest.*` coverage in `NeverDLiftTests`, and rebuild affected callers when changing the outlined owner. Report actual sanitizer, portable-path and host coverage; digest microbenchmarks alone do not certify native equivalence.

```bash
cmake --build build-release --target NeverDLowUndefinedDigestTests --parallel 4
build-release/bin/NeverDLowUndefinedDigestTests
```

`NeverDX86UndefinedEffectsTests` checks undefined-bit metadata, defined/preserved flags and stale-certificate refusal. `NeverDX86CarryArithmeticFlagTests` checks ADC/SBB auxiliary carry for register and memory forms against an arithmetic oracle. `NeverDX86LogicIdentityTests` checks that AND with identical operands still clears bits 63:32 of the enclosing 64-bit register for a 32-bit destination in 64-bit mode while preserving unwritten bits for narrower writes.

`X86RotateUndefinedEffects.*` covers every raw count, operand width, CL overlap, high-byte alias and memory destination against a scalar arithmetic oracle. `X86BitTestUndefinedEffects.*` covers register/immediate indexes, source/destination overlap, extended registers, defined flags and upper-register writes. Metadata controls reject changed operands, encodings and unsupported forms. Native proofs distinguish correlated reads from independent fresh flags, enforce exact/short producer budgets and reject observable undefined overflow. Full-state refinement checks accept the selected witness and reject zero-bit witnesses or altered candidates.

`X86XaddAudit.*` checks all 65,536 byte operand pairs, wider flag boundaries, register/high-byte overlap, both writebacks, REX byte-width limits and whole-register preservation against an unsigned arithmetic oracle. Native checks require zero new fresh bits without losing earlier dependencies; both witnesses accept unchanged XADD, while altered sum, exchanged source or defined flag outputs refuse. The `/6` alias uses the complete shift-count matrix, with changed group/decoded-ID controls rejecting semantic relabeling. Memory tests additionally exhaust byte pairs and cover address overrides, extensions, IP-relative and i386 16-bit addressing, signed displacements, surrounding bytes and stale detail refusals. Full-frame native checks retain earlier arbitrary dependencies and exact/short limits; altered store addresses violate the return-slot contract.

`X86DoubleShiftUndefinedEffects.*` checks every raw byte count at 16/32/64-bit widths against independent single-bit transfers, including source/destination/CL aliases and exact producer guards. Native checks isolate flags after discarding RAX, distinguish count 16 from 17, retain earlier dependencies and enforce exact/short budgets. Full-state relations reject changed defined slices and a zero-bit witness; arbitrary low words never authorize clearing defined upper bits. Malformed forms publish no partial evidence.

`*Deferred*` cases exercise dead taken edges, dead fallthrough, missing or malformed code, symbolic contradictory guards, arbitrary control, selected witnesses and full-state mutations. Synthetic providers verify that dead successors are never fetched and reached malformed metadata still refuses. Exact/short instruction, operation, visit and query limits, feasible bad alternatives and nonterminating cycles cannot certify prefixes. Policy changes alter certificate digests, and static/loop APIs reject the option.

`NeverDPEFixedImageTests` uses independently constructed PE files to check relocated instructions and immutable data, import write footprints, malformed headers/tables, aliases and changed provenance. Native-to-LowIR and exact LLVM proofs accept matching candidates and reject changed results, status or native bytes. Preparation exhaustion remains distinct and permits an explicit retry with larger limits; ordinary loading also accepts a valid 40000-record relocation table beyond the default analysis budget.

`TerminalImportSpecialization.*` checks physical external-call target ordering,
one stack push and the original return address with an independent scalar
oracle, including an aliasing state used solely to test expansion semantics.
Normal siblings and exact/short budgets remain required. Changed bytes, complete
lifted operands, domains, receipts and synthetic return-address substitutions
refuse. `PEFixedImageTest.TerminalImport*` checks authenticated named identity,
mutable display metadata, writable/ordinal/relocated/partial/overlapping slots,
bound IATs without a lookup table, both sides of the 31-bit name-RVA boundary,
ordinary sibling paths and proof/source refusal, including unused declarations.
`InterpreterMachineStateTest.PipelineRefusesUnusedTerminalImportDomain` checks
the source gate. Run their full owner executables and the loader contract suite
when changing this shared import identity boundary.

`FrameOffsets.*`, `NativeStackSpecialization.*` and `OriginalBinaryUndefinedIndependence.*` check all residues for alignments 2/4/8/16/32, free high bits, spills across calls, countdown loops, alias corruption, wrong dispatch, irrelevant wide masks, necessary partition upgrades and exact/one-short budgets. Separate native controls check guarded alignment, internal unsigned return cleanup, incorrect cleanup and prefixed returns. These tests do not establish automatic native-to-LLVM proof coverage for partitioned loops.

Native frame-cache regressions cover 64 repeated aligned loads under the one-load query budget, one-short query refusal, changed out-of-frame addresses and the same address under distinct incoming predicates after one path has returned. Existing full-state mutation and cache key/capacity tests remain required.

Frame-offset regressions cover 558 split-width/alignment/residue/bias combinations under a gate budget too small for whole-root subtraction, mismatched slice roots and biases, unconstrained sparse masks, modular carries and wraparound, nested masks and node/query exhaustion. Native tests verify exact stores through partially aligned pointers, reject missing alignment and out-of-frame accesses, and reject modified store values in full-state refinement.

Repeated-feasibility regressions retain all 130 native instructions while using the same query budget as a two-instruction straight line, reject one-short query/instruction budgets, keep feasible traps across changed branches and entry domains, refuse exhausted solver gates and reject modified candidate state.

Entry-congruence proof tests cover every residue at alignments 1/2/4/16, two distinct root registers, unconstrained higher bits, malformed domains, contradictory entry constants, nonwrapping bounds, exclusion gaps, preservation and exact/one-short query budgets. Inductive loop templates retain the original root predicate. Fresh native independence/refinement and native-to-LLVM checks reject mismatched domains and changed status, while certificate digests bind both domains. These are independently authored fixtures; no alignment is inferred from an ABI or a concrete execution. An independent guarded C function is compiled unchanged at O1/O2 and proved against the actual native instruction sequence under two residues; the same compiled artifacts must fail for a different residue.

Register-case regressions cover both byte orders, high frame roots, overwritten and overlapping fields, later edges, widened predecessors, native CALL/RET and exact/one-short budgets. Both C routes execute all four memory cases at O0/O2. Native refinement checks bind two selector values independently; they are not an unrestricted-input proof. A separate LLVM fixture checks that moving the false arm before a shared join cannot make it execute after the true arm, including PHI copies and stores.

Entry-alignment regressions cover finer partitions, every allowed residue, different high root bits, late failing cases and exact/short budgets. Source checks exercise both C backends at O0/O2 with inaccessible rejected guest addresses and invalid flags: status 2 must preserve all state bytes. Model refinement checks the same rejection semantics; C/Python tests cover v5 layout, ownership and old/future tails. Native proof controls reject unbound alignment domains.

`StringTransfer.*` and repeated-copy regressions check overlap, zero count, scratch isolation, capacity/budget limits and pointer invalidation. `MachineStringSourceTests.cpp` compares native execution and both C routes at O0/O2 against independent full-register, flags and stack observations for all four widths and both directions.

`ControlDiscovery.*` and `NativeStackSpecialization.*` cover low-bit root guards, high-bit and whole-root dependencies, incomplete walks, exact and short traversal budgets, and preserved finite immutable-address witnesses.

`ControlDiscoveryCache.*` checks completed-walk reuse, exact and short work
budgets, changed or missing input births, later load history, context and byte
order isolation, incomplete retries and storage saturation. The sibling
producer regression executes both byte orders, retains its fixed discovery
budget and rejects an unresolved sibling. Run the complete specialization,
PE fixed-image, native/LLVM refinement and undefined-independence test owners
when changing this shared dependency boundary. Reuse grants no larger proof
or source-publication contract.

Unresolved-target guard tests cover an unreachable invalid phase, an unknown target reachable at entry or after a backedge, exact refinement limits, and adjacent successful/exhausted discovery budgets. Refusal publishes no residual code, origins or read witnesses. Optional discovery work after complete recovery is not a required-budget lower bound.

Demanded frame-projection regressions cover automatic and existing register carriers, high roots and modular wraparound, incomplete low-byte guards, conflicting or missing sibling facts, adjacent query-budget outcomes and solver unknown. A narrow demand never certifies only part of a full pointer; refused recovery publishes no residual code or witnesses.

Finite-subword regressions cover selectors in either half, observable arbitrary payloads, register and frame carriers, both byte orders, narrow carriers, later domain expansion, disjoint masks and missing facts. Missing reachable targets, free selectors, exhausted shared query budgets and solver unknown must publish no residual code or witnesses. Automatic discovery with whole-word demand, unused manual hints, root-derived payloads and constant leading windows are covered too.

Public work-budget tests cover default and maximum 32-bit limits, independent evaluation/discovery exhaustion, both source ABIs and C backends, and malformed CLI values. C/Python layout checks cover v7, inherited validation and v1–v6 ignoring future tails. Budget refusal must leave source and recovery witnesses unpublished; node-evaluation charging must not wrap at the maximum counter value.

Optional repeated-destination chain tests compare direct and nested loops under one fixed operation budget, execute the residual loops, distinguish decode modes, and check that zero chaining is unchanged. A correlation counterexample must still succeed by default and refuse publication with the option. Repeated native calls/return slots, dependency replay and late predecessors run with both settings. Public flag tests cover both source ABIs/backends, default and unknown flags, old APIs ignoring the extension, and the reported boolean.

`NativeStackSpecialization.NarrowAddressDemandRetainsCompletePointer` checks guarded pointer joins in registers and frame slots, both byte orders, modular wraparound and high roots, restored stack, and 120 frame bytes. Companion controls reject corrupted pointers and exercise exact/short query, operation, evaluation and refinement budgets, plus discovery and context exhaustion.

`InterpreterSpecialization.StackControlDiscoversSavedRootAddresses` covers
ordinary returns and native CALL/RET after joined saved-pointer loads, with
register and frame carriers in both byte orders. Independent byte oracles check
complete memory effects, zero/wrapped/high roots and both branches; unknown
saved values and exhausted refinement budgets still refuse.

`InterpreterSpecialization.WideIntegerConcat*` covers intermediate widths from
nine through sixteen bytes, every valid split, both byte orders, output aliases,
malformed extents and exact/short operation budgets. Decoded `PACKSSDW` and
`PACKSSWB` cases use independent signed-saturation oracles at both boundaries.
`BinaryLowIRRefinement.WideIntegerConcatCompletesOnlyRawRecovery` separately
checks that these instructions do not acquire source or native proof support.

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

`Win64SourceCallsKeepTheirDeclaredCarrierWidths` checks declared scalar carriers
against whole-register scans. `NeverDWorkerCallSiteTypes` also checks explicit
Win64 argument counts from zero through five, both C routes and partial-stack
refusal. `RegisterJoinsRespectEntryValuesInEitherOrder` covers x64/AArch64 PHI
order, entry widths, nested joins, exceptional inputs and missing predecessors.
The joined-format SysV tests check compatible and conflicting formats, unknown
paths, clobbers and independent entries on ELF and Mach-O. Their compiled C
checks both paths with a full-width number and a real string pointer.

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

The same suite checks shared-expression liveness with independent return-value
oracles, multiple branch definitions, dead cycles and nested observable effects.
Calls, stores, ordered memory and non-default address spaces remain observable
even when several assignments share an expression. Cyclic expression graphs
exercise effect propagation in both encounter orders; cleanup must terminate
without dropping an effect or its inputs.

Cleanup progress regressions distinguish rejected copy candidates from actual
replacements, including simplification that makes a previously unstable candidate
safe on the next round. Shared-node operator changes, child replacements and
in-place operand rewrites must report progress and preserve independent
interpreter results. Refused ordered-memory rewrites report no progress and
retain the access.

`CProjectionIdentifiers.*` in `NeverDHighControlFlowTests` checks first-free
source names after dense and explicit suffix collisions, canonicalization,
writer copies, moves and resets. Both C emitters share this allocator.

`HighCPointerAddresses.Required*` / `UnknownConditionsFailOnlyWhenRead` checks that inferred required register arguments retain unknown trailing slots. Evaluating an unknown required argument or condition must trap explicitly; omitted, null and nested operands must not silently become zero. Known values and proven unread extra operands remain executable. A trap is a diagnostic boundary, not evidence of equivalent recovered behavior.

## CPU execution checks

`UnicornAArch64.CheckedRegisterPacketsPreserveEveryLaneAndRestoration` checks
distinct scalar and SIMD values across repeated checked user/supervisor steps
and context restoration. Linux's `NeverDUnicornStateTransferTests` injects each
scalar/vector read failure, partial batch installation, cancellation and expiry;
normal captures use the real batch API. `NeverDUnicornMachineControlTests`
retains its pre-entry and RAM rollback controls at the batch capture boundary.
These Linux linker-wrapper injections require a Linux build; other platforms'
successful state-transfer tests do not establish that fault-injection coverage.

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

`CMPXCHG8B` and `CMPXCHG16B` execute their original encodings on KVM, WHP and checked Unicorn in driver and user profiles. Successful and failed comparisons both require read/write access; faults are classified as writes. `CMPXCHG16B` checks 16-byte alignment before memory access and reports `#GP(0)`. Its two result observations share one RAM transaction: stopping or throwing in either publishes no registers or RAM. Unlocked `CMPXCHG8B` may cross pages; locked operands retain the natural-alignment contract. `X64WideAtomicTests.cpp` compares original host results and direct native faults, aliases, prefixes, address rules, repair and cancellation. Original Windows driver and ring3 PE fixtures exercise both widths; the WDK fixture also executes `_InterlockedCompareExchange128`. The CPU model must support `CMPXCHG16B`.

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
observer failures and software HLT without a workload-completion claim. The
software deadline regression spends the budget inside an instruction observer
and verifies that the following unmapped load never executes and a fresh run
can stop normally. Keep native Windows repetition for the short-slice timer
regression; a pass on another host does not qualify its timing behavior.
The driver context fixture checks all 1296 guarded bytes at each of twelve
captures. Constant-width copies and unrolled fixed loops reduce checked native
instruction overhead while retaining every comparison and the existing budget.
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

For hosted macOS HVF diagnosis, `run_native_cpu_ci.py --execution-methods`
executes the same complete CTest inventory in serial GoogleTest method
processes. It retains every parameter and validates the original XML against
CTest identities; unsupported properties fail explicitly. Method deadlines
are capped at 120 seconds with bounded process-group retirement. This mode
uses aggregate method timeouts instead of separate parameter timeouts, and
records that execution policy in its summary. Normal self-hosted validation
keeps CTest's per-case isolation. See [macOS HVF](macos-hvf.md) for evidence.

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

`NeverDParallelExecutionTests` forces overlapping processor calls, cancellation while a writer waits, independent CPU state, physical-alias atomic contention and private transport staging. `NeverDRunControlTests` checks that independent WHP bindings can retain two resource leases while the shared binding serializes them. `NeverDMMIOAtomicTests` compares device and RAM results for original x64 atomic/update encodings and all ARM64 LSE cases, and checks both wide write observations, stale previews, provider failures and commit/stop races. `KernelMMIOFailure` covers aliases, identical writes, repeated commits, power changes, unmapping and retired owners. Platform-unavailable cases are explicit skips; a wrapper rendezvous proves concurrent processor calls, not simultaneous hardware retirement. Native ARM64 KVM/WHP needs the corresponding host.

```bash
cmake --build build-cpu --target NeverDParallelExecutionTests NeverDMMIOAtomicTests NeverDRunControlTests --parallel 4
ctest --test-dir build-cpu/unittests/emulation -L '^NeverD(ParallelExecution|MMIOAtomic|RunControl)Tests$' --output-on-failure
```

## Process emulation checks

`AndroidJNI.BuildVersion*` compares JNI version fields with native property
reads across O0/O2, relocation packing and available CPU backends. It checks
property/default string identity, signed SDK values, field IDs surviving
detach/reattach, wrong accessor/write refusal and unknown fields.
`AndroidVersion.*` in `NeverDDalvikTests` checks shared DEX/direct-field identity,
Android integer parsing boundaries, noninterned UTF-16 values, rejected platform
replacement, malformed inputs and terminal heap exhaustion. Run the complete
Dalvik and Android JNI suites when changing this shared field owner.
`ProcessReport.DexWireBudgetPreservesTheMetadataAndDecodedByteBounds` checks the
full 1 MiB DEX allowance, split images, both wire limits and malformed inputs.
`ProcessPublic.DexWireAllowanceReachesSharedValidation` checks that the C API
admits a large request to the DEX validator while retaining the non-DEX text
limit. The wire allowance never bypasses DEX container validation.
`AndroidJNI.DexClassCapacityIsSeparateAndAggregate` uses generated DEX035
containers to cross the former 16-class limit, resolve the last class at the
4096-class boundary in single and split inputs, and reject oversized combined
inventories, too many images, duplicate classes and corrupted bytes. The input
generator is shared with the independent mobile inventory benchmarks; no JDK,
phone or copied application DEX is needed.

`AndroidSleepTests.cpp` compiles independent O0/O2 callers with ordinary, APS2 and
RELR relocations. It checks raw, named and variadic relative sleeps, complete
observed bytes, request/remaining aliases, malformed inputs, multiple deadlines,
saved registers/TLS/errno, join/mutex/once continuations, provider lifetime and
pending events at instruction limits. Available native AArch64 transports also
match Unicorn's complete reports, including instruction and thread traces.
Direct and dynamically resolved `usleep` callers check zero, subsecond and
multi-second durations, full unsigned 32-bit range, high register-bit narrowing,
clock rollover, errno preservation, idle wake completion and a runnable peer.
Disabled time policy, clock overflow and closed providers retain incomplete
calls and unchanged output memory.
`LinuxClockTests.cpp` checks shared elapsed
time and atomic refusal on clock/deadline overflow. The x64/ARM64 Linux process
fixtures verify raw sleep errors, unchanged input and both updated clock layouts.

`AndroidNative.FamilyFallbacksPreserveDiagnosticsAndProviderPrecedence` checks
unknown pthread attribute and mutex members, disabled thread calls and generic
unknown imports through real dynamic lookups. The same calls after library close
must report the inactive provider before interpreting their invalid arguments.
Dispatch refactors also require the Android, Linux, Darwin, process API and
execution session/configuration suites, preserving complete reports across the
public surfaces and available backends.

`DriverKernelFramework.CallAdmissionPreservesValidationOrderAndIRQL` checks
competing argument, globals, IRQL and object failures, preserves allocation state
on rejection, and executes the unrestricted context accessors. Windows call
routing changes require the complete driver, public driver API, native driver and
guard metadata suites with driver emulation enabled. Request lifetime, queue
ownership, cancellation, interrupt and power tests exercise the domain handlers;
execution tests still report unavailable native hosts and optional WDK fixtures
explicitly.

`DriverKernelModel.SpinLockFailuresPreserveOwnershipAndValidationOrder` checks
competing release-variant, saved-IRQL and corrupted-storage errors through public
kernel call admission. Failed releases preserve ownership, storage and IRQL;
repairing the word permits a valid release with unspecified upper argument bits.

PoFx call routing also preserves live-handle and framework-ownership admission
before component-index or flag errors. `KernelPoFxAPI` and
`KernelFrameworkPoFxBridge` check those competing failures alongside blocking
continuations, callback completion, registration lifetime and component hints.

`AndroidFinalizerTests.cpp` runs independent C ABI callers at O0/O2 with ordinary,
APS2 and RELR relocations. It checks constructor registrations, exact callback
order and arguments, DSO filtering, duplicate and NULL registrations, recursive
finalization, callbacks that register callbacks, nested `pthread_once`, stack
preservation, dynamic provider lifetime and registry isolation. Invalid guest
targets, callback failures, raw exits, capacity and instruction/event limits
must not complete a suspended finalize event. C API/CLI and the real Python SDK
verify the same guest effects and dynamic symbol identities.

`AndroidSyscallTests.cpp` executes independent C fixtures at O0/O2 with ordinary,
Android-packed and RELR relocations. It compares named, raw SVC and variadic
identity calls; verifies errno, six-argument memory calls, full-width pointers,
binary vectored output, budget stops and nonreturning exits; and rejects unknown
services and closed dynamic providers. The shared C API/CLI and actual Python
wrapper also check `dlsym` provider identity for `syscall`.

`LinuxMemory.*` in `NeverDLinuxProcessTests` checks raw anonymous-memory syscall
rules, partial protection before a hole, page reclamation, transactional budget
failure and program-break preservation. Original x64/ARM64 ELF fixtures verify
those services through actual instructions and repeated process continuations;
the x64 fixture additionally calls rewritten executable memory. Protection
failures use real guest stores. `MemoryLifecycle` also verifies that virtual
layout snapshots expose RAM/device rights without pinning retired allocations.
`NeverDProcessPublicTests` runs the memory fixture through the shared SDK/CLI.
Merge-advice cases check range splitting, rejoining, partial revocation,
`PROT_NONE`, both sides of holes, argument narrowing, range overflow and policy
retirement without retaining page allocations. The x64/ARM64 memory fixtures
also execute raw `madvise` calls. Android fixtures compare named imports, raw
SVC and `syscall` errno behavior across all relocation formats, verify dynamic
provider names and lifetime, and require unmodeled content advice to stop.

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

Checked x64 also admits masked legacy `ADD`, `SUB`, `MUL`, `DIV`, `SQRT`, `MIN` and `MAX` in `SS`, `SD`, `PS` and `PD` forms. `X64SSEInstructions.def` owns operand widths, alignment and admission. `MaskedSSEArithmeticMatchesIndependentHostExecution` compares register and RAM forms against an independent host CPU oracle, including all four rounding modes, FTZ, signed zero, subnormal inputs and NaNs; `SSEMemoryObserverStopsBeforeResultAndStatusChanges` verifies cancellation before effects. This does not admit unmasked exceptions, x87 or AVX.

`X64PackedIntegerTests.cpp` uses original encodings and 180 literal vectors in `X64PackedIntegerCases.def`, checked independently against native x64 compiler intrinsics. Register and aliased page-end RAM cases preserve other XMM registers, integer sentinels, FLAGS, MXCSR and source bytes. Observer stops/failures and recoverable read faults preserve state; repair permits a single retry. Misalignment raises `#GP(0)`; MMX, LOCK and MMIO reject before callbacks. Both WHP privileges are mandatory in native CI.

`X64PackedShiftTests.cpp` and original `X64PackedShiftCases.def` compare ten shifts against independent scalar calculations and native SSE2 intrinsics at 16 immediate and 21 variable counts. Coverage includes count/destination aliasing, ignored high bits, alignment, observers, recoverable faults and device rejection. `X64VectorTestSupport.h` shares register and RAM assertions with packed arithmetic tests. Both WHP privilege modes are mandatory in native CI.

`X64VectorMaskTests.cpp` checks independent raw encodings against scalar bit extraction and native SSE intrinsics, including every source bit and all 16 GPR × 16 XMM combinations with both REX.W values. Complete public register snapshots, RAM and data observers verify zero extension and preservation; instruction stops, callback failures and unsupported forms cannot publish effects. Native KVM/WHP acceptance requires both privilege modes.

`X64ShuffleTests.cpp` uses independent encodings in `X64ShuffleCases.def` and scalar lane selection checked against native intrinsics. It covers all 256 controls with register, self-source and page-end alias operands, all XMM register pairs, full public CPU state and RAM, observer stops/failures, permissions, alignment faults and retries. MMX, VEX/EVEX, LOCK and device operands must reject without effects. Native KVM/WHP acceptance requires these cases at both privileges; unavailable host/ISA pairs remain explicit skips. The `UNPCKLPS`, `UNPCKHPS`, `UNPCKLPD` and `UNPCKHPD` cases reuse the same state/fault matrix and independent scalar/native oracles. Every XMM destination is also checked with a memory source.

`X64PartialMoveTests.cpp` and `X64PartialMoveCases.def` compare independent scalar/native load and store oracles, all 16 XMM registers and raw NaN/subnormal bits. Full CPU state and both RAM pages are checked for unaligned accesses, aliases, cross-page faults, permission repair, observer stops/failures and retries. Page-end operands need only eight bytes, and stores need no read permission. Register aliases, rejected forms and device callbacks are checked separately; native KVM/WHP cases are mandatory at both privileges.

`X64IntegerFloatTests.cpp` uses independent `X64IntegerFloatCases.def` encodings, `APFloat` expectations and original native instructions with saved/restored FP state. Both integer widths, four rounding modes, sticky precision status, FTZ, every GPR/XMM pair and full CPU/RAM preservation are checked. Unaligned, cross-page and page-end sources, permission repair, observer stops/failures and retries retain exact footprints. Native KVM/WHP cases are required at both privileges.

`X64FloatIntegerTests.cpp` checks rounded and truncating scalar conversions using independent `X64FloatIntegerCases.def` encodings, `APFloat` and original native register/memory instructions. Signed limits, halfway values, NaNs, infinities, subnormals, all rounding modes, sticky status and FTZ cover both integer widths. Every GPR/XMM pair, full CPU/RAM state, exact page-end reads, recoverable cross-page faults and observer cancellation/retry are checked. Native KVM/WHP outcomes at both privileges are mandatory.

`X64SSEComparisonTests.cpp` uses independent `X64SSEComparisonCases.def` encodings, `APFloat` ordering and original native instructions with saved/restored host FLAGS and FP state. All pairs of 21 raw inputs cover NaN/denormal priority, zeros, infinities and adjacent values. Tests check every XMM pair and alias, sticky MXCSR, rounding independence, DF preservation, full CPU/RAM state, precise page-end/cross-page reads and observer cancellation/retry. Native KVM/WHP outcomes at both privileges are mandatory.

`X64SSEPredicateTests.cpp` uses independent predicates in `X64SSEPredicateCases.def`, shared raw inputs in `X64SSEComparisonCases.def`, `APFloat` ordering and original native instructions. Tests cover every input pair, mixed-lane exception priority, scalar upper lanes, XMM aliases, full CPU/RAM state, page-end/cross-page reads, alignment priority and observer/fault retries. Direct Capstone checks cover all control bytes, both syntaxes, both decode APIs and 32/64-bit modes. Native KVM/WHP outcomes at both privileges are mandatory; reserved controls, VEX/EVEX and device operands remain excluded. Value matrices are split by opcode and predicate; rounding/control matrices are split by opcode. Every original combination remains required in `NativeCPUTests.def`, with unchanged guest and 15-second CTest deadlines. Compile-time checks require each opcode/predicate pair exactly once.

`X64SSEPrecisionTests.cpp` combines independent `X64SSEPrecisionCases.def` encodings, `APFloat` precision rounding and original native instructions. Range checks use rounding with an unbounded exponent, including directed finite overflow and tiny results that round to normal. Tests cover both signs, NaN payloads, all rounding/FTZ/sticky settings, packed lane aggregation, XMM aliases, complete CPU/RAM state, exact source widths, alignment priority, page faults and observer cancellation/retry. KVM/WHP cases at both privileges are mandatory.

`X64PackedFloatTests.cpp` uses independent `X64PackedFloatCases.def` encodings, signed `APFloat` expectations and original native instructions. Every input pair, integer limits, precision halfway values, rounding modes, sticky status and FTZ are checked. Register aliases, all XMM pairs, complete CPU/RAM state, every m64 page split, exact page tails, alignment priority and observer/fault retries cover both privileges. All KVM/WHP cases are mandatory.

`X64PackedFloatIntegerTests.cpp` combines independent `X64PackedFloatIntegerCases.def` encodings, shared `X64FloatIntegerCases.def` payloads, `APFloat`/`APSInt` and original native instructions. Every pair of 43 raw inputs covers signed32 limits, adjacent halfway boundaries, NaNs, infinities and subnormals. Separate lane matrices independently vary exact, inexact, invalid and subnormal inputs. All rounding/FTZ/sticky settings, XMM aliases, full CPU/RAM state, aligned page tails, alignment priority and observer/fault retries are checked. KVM/WHP cases at both privileges are mandatory.

`DAZBackends` extends the comparison, predicate, scalar integer/float, packed integer/float and precision-conversion matrices with DAZ enabled. `X64DAZTestSupport.h` supplies independent `APFloat` input normalization and checks the original host `MXCSR_MASK` before native oracle execution. Tests compare signed zeros, subnormal and NaN inputs, mixed lanes, all rounding modes, FTZ and sticky status while preserving source bytes, unrelated registers, FLAGS and complete RAM. Register, alias and page-boundary operands retain their existing access observations. KVM/WHP require every DAZ case at both privileges and all 17 original-host oracle cases; unsupported hosts remain explicit skips in portable runs. Existing DAZ-disabled cases and deadlines are retained.

`X64AlignmentTests.cpp` checks that misaligned operands of admitted aligned SSE instructions report recoverable or terminal `#GP(0)` before data observers, permission checks or device callbacks. Faults retain the complete public x64 register context, PC and RAM; address-size wrapping precedes FS/GS addition, and repairing the address retries the original instruction. Direct KVM/WHP machine cases independently verify the hardware boundary. Windows ring3 delivers classified `operand_alignment` faults; other `#GP` causes remain unsupported.

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

`X64SIMDExceptionTests.cpp` bypasses checked admission to verify native KVM/WHP `#XM` transport at both privileges. Eight original cases in `X64SIMDExceptionCases.def` cover all six exception types, including exact tiny results and overflow with exact unbounded precision. Register and RAM forms preserve the complete GPR, XMM, x87, FLAGS, FS/GS and guest memory state on faults, apart from the specified MXCSR status. Masking the exception retries the original instruction; repairing operands while retaining sticky status verifies that old flags do not retrigger it. The public checked and driver contracts also run those original fault/retry cases. `WindowsSIMDExecutionTests.cpp` executes a real native fault, guest VEH/VCH instructions and skip, masked-retry or operand-repair continuation; live handler controls and saved context are checked separately. Negative startup tests reject missing faults, wrong vectors and changed destinations without publishing capabilities.

`check_windows_simd.py` builds an independent original Windows x64 executable from `WindowsSIMDCases.def` and the scalar case inventory. Its 6,144 observations cover register/RAM operands, every exception-mask combination, clear/all-set sticky flags and three continuations: skip, mask and retry, or repair operands and retry without changing the masks. Assembly entries record live VEH/VCH MXCSR and x87 controls separately from the saved `CONTEXT`. The fixture checks exact fault PCs, preserved state, repaired context and retry results before restoring host state. CI retains raw records and source hashes. `--build-only` is compilation evidence only. These observations do not enable checked unmasked SIMD or claim native ARM64 execution.

`WindowsSIMDStatusCases.def` freezes all 63 nonempty active-status combinations observed on native Windows. `WindowsSIMDMappingTests.cpp` verifies their exact codes and parameters, rejects inconsistent faults and invalid controls, and injects the fault boundary to check exception records, both CONTEXT controls and a masked continuation. The original Windows CI executable independently enforces the frozen results. The injected test does not establish Unicorn exception delivery or enable checked unmasked SIMD.

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

For changes to shared kernel API dispatch or its call adapters, include
`NeverDNativeDriverTests` and `NeverDDriverGuardMetadataTests` with the two
driver emulation suites above. These checks cover common validation, strict
execution policy, request ownership and backend-visible outcomes as well as
individual API behavior. Unavailable native backends and external WDK images
remain explicit skips.

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
`DriverKernelRegistry` also covers Basic/Node/Full/Name/Cached key layouts,
explicit and unknown timestamps, overflow before mutation, identical-value
writes, retained maxima and live subkey indices. Original
`driver_registry_values.c` and `driver_registry_keys.c` execute query,
enumeration and deletion through Unicorn/KVM/WHP/HVF with legacy/checked-x64
contracts at preferred/rebased addresses; unavailable host backends skip
explicitly.
`driver_cstring_writes.c` uses the same backend/contract/base matrix for eight
C string writers. Model tests cover copy padding versus append termination,
zero-count narrow/wide differences, exact source boundaries, full-width counts,
overlap, write preflight, paged IRQL restrictions and expired pool storage.
`driver_lookaside.c` exercises the public x64 inline cache paths, default pool
callbacks, cache hits/misses and full unload across that matrix.
`KernelLookaside` checks registration links and reserved bytes, cached-pool
lifetimes, corruption, IRQL, missing exports, MDL borrowing in either order,
and deletion preflight with held locks, pinned pages and read-only storage.
`driver_fast_mutex.c` covers both WDK initialization forms, all eleven fast/guarded
mutex exports, failed Try calls, APC_LEVEL entry, Unsafe regions and unload
across the same backend/contract/base matrix. `KernelFastMutex` checks logical
thread ownership, nested holds and callbacks, IRQL/region restoration, padding,
corruption, read-only and paged storage, MDL borrowing in either order and
failure before mutation. Contended waits remain explicit unsupported paths.
`driver_eresource.c` executes all 22 legacy resource exports across that matrix,
including three system threads holding shared ownership while parked on an event,
table growth, recursive acquisition, downgrade, owner-pointer transfer and unload.
`KernelExecutiveResource` covers table placement for different shared-acquisition
forms, registration unlink preflight, APC/IRQL and mutex restoration, foreign
release, owner-pointer lifetime, MDL borrowing, read-only/corrupt storage and
thread/unload guards. Blocking contention is rejected before registering a waiter.
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

`DriverThreadPriorityTests.cpp` runs the original `driver_thread_priority.c` fixture on explicit Unicorn/KVM/WHP in driver and checked contracts. Cases verify queued and blocked reprioritization, mid-quantum event/timer wakes, equal-priority rotation, DISPATCH_LEVEL masking and timer progress while lower-priority work is starved. A paired counter-loop experiment proves that priority preemption preserves the exact remaining quantum. Model tests cover signed ABI arguments, rejected mutations, retained exited objects, nested identity and detached stack reuse. Native cases are mandatory in `NativeDriverTests.def`; unavailable transports remain explicit local skips.

`DriverMutexThreadTests.cpp` executes four original WDK modes from `driver_seh_mutex.def`: recursion from an SEH filter, ownership acquired by a filter or exceptional finally, and a blocked filter resumed after another system thread releases the mutex. Unicorn/KVM/WHP driver and checked contracts cover normal/active-CFG images, preferred/rebased addresses and cooperative/1/17-instruction quanta. Model tests also check APC suppression after nested stack retirement, wrong-thread release and the outermost return guard; KVM/WHP outcomes are mandatory in `NativeDriverTests.def`.

`KernelWaitSetTests.cpp` runs sixteen portable model cases without Unicorn: partial `WaitAll`, first-ready `WaitAny`, captured indices, timeout cleanup, rejected late objects/storage, 64-object boundaries, IRQL, retained exited threads and two synchronization timers. `DriverMultipleWaitTests.cpp` executes seven original WDK modes from `driver_wdm_multiple_wait.c` and `DriverMultipleWaitCases.def` across Unicorn/KVM/WHP, both driver contracts, normal/CFG images, relocation and cooperative/1/17-instruction quanta. The 30 model/native outcomes are mandatory in `NativeDriverTests.def`. Regressions also cover stale completion after success or timeout, altered captured state and repeated delay completion.

`KernelMultipleWait.DispatcherScalarParametersIgnoreUpperRegisterBits` checks poisoned high bits, signed bounds, overflow and unchanged object state. Naked tail wrappers in `DriverMultipleWaitCases.def` exercise the same valid ABI calls through real WDK imports in `driver_wdm_multiple_wait.c`, including active CFG, relocation and instruction preemption.

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

The same genuine-WDK fixture acquires an executive spin lock through the actual `KeAcquireSpinLock` macro, checks `DISPATCH_LEVEL`, observes a failed try-acquire while held, releases it, and checks the original IRQL. Native normal/active-CFG and preferred/rebased runs, C API/CLI, and Python cover that path. `KernelRequestOwnershipTests.cpp` checks resident storage, exact ownership and release variant, saved IRQL, repeated acquisition, and rejected return/free while held. This CPU0 model rejects contended blocking acquisition; cross-CPU lock progress and in-stack queued locks remain unsupported.

The WDK fixture also initializes a `KSEMAPHORE` at count zero, releases two units, consumes them through two actual zero-timeout waits, confirms a third timeout, and releases up to its limit. `KernelDispatcherTests.cpp` checks count and limit validation, one-unit wait consumption, priority/Wait restrictions, IRQL limits and a typed `STATUS_SEMAPHORE_LIMIT_EXCEEDED` exception without count mutation. Native normal/active-CFG and preferred/rebased, C API/CLI and Python paths exercise the same count transitions.

The WDK fixture's inline `KeRaiseIrqlToDpcLevel`, `KeRaiseIrqlToSynchLevel` and `KeRaiseIrql` use actual `KfRaiseIrql` / `KeLowerIrql` imports. It observes CR8 through `KeGetCurrentIrql` after nested DISPATCH, APC and synchronization-level transitions in normal/active-CFG preferred/rebased images, C API/CLI and Python. Model tests reject lower without a saved raise, wrong LIFO order, cross-execution restoration, a held spin lock and return with an unmatched raise. No instruction-level interrupt preemption is inferred.

The same genuine WDK fixture uses a resident `KMUTEX` through `KeInitializeMutex`, `KeWaitForMutexObject` (the WDK `KeWaitForSingleObject` macro), `KeReadStateMutex` and `KeReleaseMutex`, checking initial signaled state, recursive acquisition, signed previous-state returns and final release. A second mode catches `STATUS_MUTANT_NOT_OWNED` from an unowned release. Native normal/active-CFG preferred/rebased, C API/CLI and Python paths run both modes. Model tests check owner isolation, incorrect IRQL, waiting-thread ownership, a held object's storage and callback-return lifetime, and unsupported `Wait=TRUE` handoff.

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
| `unittests/loader` | `NeverDRawISATests` | Binary files: instruction-set identification from the bytes (data, the test's own code, code two bytes in, 32- versus 64-bit encodings per family, files that start with zeros) and the Cortex-M vector table. `scripts/validate_isa_model.py --engine build/bin/libneverd.so` checks the model on 180 real programs and libraries it never saw, downloaded by hash; it needs the network and is not part of CTest |
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

`AArch64_FP.SeparateArithmeticKeepsItsRoundingThroughBothCRoutes` uses original
half/single/double multiply-and-add instructions and fixed bit-pattern oracles.
Both C routes execute at O0/O2 on the host and cross-compile to ARM64 IR that
must retain separate operations. This exercises half bitcasts and intermediate
rounding on hosts without native FP16, alongside the native half-arithmetic CI
case. It does not certify nondefault floating environments.

`PLTImportsRequireSymbolAddressRelocations` checks REL and RELA slot roles for
x86, x64, ARM and AArch64, retaining TLS descriptor runtime semantics without
turning descriptor symbols into callable imports. `PLTUnwindRangesDoNotHideIndividualVeneers`
checks all four architectures, including explicit function ownership and
partial-range counterexamples. Session C API regressions cover TLS calls,
variadic veneers and lazy-binding source pages.

`ExternalFunctionPointersNeedAnExactAbsoluteSymbol` checks the four absolute
relocation types with REL and RELA addends, and that GOT bindings, data,
untyped, weak and defined symbols, conflicting names and relocatable objects
yield no external function pointer. `PointerVariablesStartAtExternalFunctions`
builds, for each architecture, a library whose two pointer variables start at a
function another library defines. One is volatile, so x64 calls the other
straight from memory (`call [rip+disp]`) and loads this one into a register
first. Its LLVM C must not trap and must compile for all four targets. On an
x64 host the program runs and must call the initial function, then the one
stored over it.

`CallsThroughPointerVariablesPassTheirArguments` builds, for each architecture,
a library whose pointer variables start at prototyped routines (`exit`, `puts`,
`strcmp`, which a stub library defines), at a static function and at a routine
nothing describes, and checks HighC and LLVM C. Each caller takes as many
parameters as the routine its variable starts at and passes them on in order,
in tail and ordinary calls alike, and the undescribed call passes no invented
zero or return address. `call_self` passes its variable as the fourth argument
of a routine that takes four, though the same register holds the call target.
`PointerVariableCallsRunWithTheirArguments` runs the x64 LLVM C against libc:
each call reaches its routine with its arguments, a stored function replaces
the initial one, and the call through `exit` ends the program with the code it
passed. `IndirectCallsPassNoTargetOrSavedRegisters` assembles ARM calls whose
first argument register holds only the target, loaded through a table slot,
which must pass nothing in either backend (not the slot's address as a virtual
call's object), and a call whose outgoing stores leave a gap below a
`push {r4, r6, r7, lr}`, whose HighC must not pass the saved link register.

`UndescribedCalleesKeepEveryCallsArguments` builds an x86-64 and an i386
function that calls an import nothing describes with one argument, then
with three; its LLVM C must keep each call's arguments.

`VariadicCallsPassOnlyWhatTheirCallerSets` builds, for x86-64, i386, ARM and
AArch64, a library whose functions call a variadic routine (a `vprintf`
wrapper with one named parameter) and an undescribed variadic import after a
join of two calls, and call the routine with every argument register set (a
tail call on the register ABIs, after an alignment `push` on i386). HighC and
LLVM C must pass exactly the format and the arguments each caller sets: no
trap on a register earlier calls left behind, and no incoming stack slot taken
for the routine's open arity. `MedCallLeftovers.JoinsOfCallClobbersOnly`
checks which clobbers and joins `ValuesLeftByCalls` names, loops and exception
entries included.
`CallsTakeNoIncomingRegisterAnEarlierCallChanged` links, for x86-64, i386,
ARM and AArch64, a routine that reads four argument registers with callers
that declare it with one, calling it after another call, after a join of two
calls and in a destructor run on a normal and an exceptional path: no caller
takes a parameter for a register an earlier call may have changed, HighC and
LLVM C alike. `VariadicCallsTakeNoSpillAboveAnUnsetRegister` keeps an AArch64
-O0 spill above an unset register out of a variadic call's arguments.
`ItaniumRuntimeCopiesTakeTheirDeclaredParameters` links an image's own copy
of `_Unwind_Resume` for x86-64, ARM and AArch64: it takes its one declared
parameter, and its callers pass it one argument.

`ELFARM32ModeTest.MixedImagePairsItsPLTWithTheImports` links a mixed
ARM/Thumb shared library and requires the PLT entry a Thumb `bl puts` reaches
to be paired with `puts`. `LinkerThunkToAnImportStandsForTheImport` links a
stripped Thumb library whose tail call to `puts` goes through lld's thunk: the
thunk and the PLT entry must both stand for `puts`, and the decompiled caller
must call `puts` with its string. `LinkerThunk.*` checks each long-branch
form's target, state and size, and that a direct jump is never a long branch.

`LLVMCValues.ImageStringsInACodeSegmentFollowTheirSection` requires a string
in `.rodata` inside an R+X segment to print, its bytes in `.text` not to, and
the segment rule without section headers.

`LinkedLiteralAddsToTheDataItNames` links an ARM getter whose
`ldr r0, [pc, r0]` reads its literal right after `bx lr`. It requires the
`ARMLinkedLiteral` certificate for the static variable at the ADD output,
LowToMed's exact constant in its place, and a lifted getter that loads the
variable itself. A relocation on the literal word withdraws the certificate.

`IndirectTailFrame.UnwindOnlyFrameDescriptionKeepsTheTailCall` assembles AArch64
functions with plain CFI into a shared library: a restored frame's `br x0`
becomes `INDIR_CALL + RETURN` under its FDE, and a live frame keeps its
indirect branch, as does a computed jump into the function's own code
(`adr x1, 1f; add x0, x1, w0, sxtw #2; br x0`), while a tail call through the
function's own reloaded address stays one. `ExceptionFunctionUnwindOnly`
checks that an LSDA, a personality routine, handler data or a partial parse
each count as a handler.

`IndirectTailFrame.RestoredNativeFrameRetainsItsFunctionPointerTailCall` also
covers libc++'s `__shared_weak_count::__release_weak` (an acquire load and an
`ldaxr`/`stlxr` decrement loop before a virtual tail call), an exclusive pair
loop, LSE atomics (`ldaddal`, `swpal`, `casal`, `ldclral`, `stadd`) and a
value-only `rbit`. `ActiveOrUnknownTailFrameRetainsItsIndirectBranch` keeps
the branch when an exclusive store or an atomic swap may overwrite the saved
link word, when one publishes a frame address that a later callee could use
though no argument register holds it, when `ldaxr` loads the link register,
in a live frame, and after an `svc`.
`IndirectTailFrame.ExclusiveReleaseCallsItsVirtualSlotInEveryRoute` decompiles
the `__release_weak` shape through HighC, LLVM-C and unoptimized LLVM-C: none
keeps the unresolved-branch trap, each calls through the loaded vtable slot,
and the HighC compiles for AArch64.
`LowIntrinsicMemoryOperands.DecodeOnlyTheLiftedShapes` rejects width, ordering
and segment mismatches and intrinsics without a memory contract.

The restored-frame list also covers libc++'s `__release_shared` (a call, then
an exclusive decrement through the object while the link word is saved), a
plain store through an incoming pointer (`tail_unknown_alias`), a `brk` path,
`udf` padding and a thread-pointer `mrs`. The retained list keeps the branch
for a store through a frame-derived address, after a frame address leaves
through an untracked store, after one is read back through an address the
spills cannot follow, after half of one is stored where no spill records it,
after one is stored above the frame, and when `mrs` writes the link register.
`LowIntrinsicEffects.OutputOnlyReadsAndTraps` checks the two predicates.
`IndirectTailFrame.ItaniumTableForwardsOnlyFromCoveredBranches` links a
shared library whose C++ tables cover one branch with a call site that names
no landing pad (a tail call) and leave another uncovered, as a `noexcept`
destructor's empty table does (kept).

`LocalUnwindTargetRequiresExclusiveSameFrameUses` includes decoded x64 calls
whose SP save and load occupy different blocks, stale producer changes, opaque
calls, partial writes, foreign address uses and invalid module/runtime facts.
Its runtime-only continuation has no ordinary incoming edge; the test requires
both the protected-scope proof and the exact established SP in shared SSA.
An additional decoded 4096-NOP block checks that complete-CFG work does not
consume the address-use receipt budget; an explicitly small frame budget still
refuses the frame proof while preserving the separate address-use analysis.
`LocalUnwindFrameProofIntersectsEveryPredecessor` checks diamonds, stable and
changing loops, block ordering, independent/EH roots, expired temporaries,
released stack storage, partial/atomic writes and exhausted budgets. No opaque
callee memory preservation or mixed PE32 callback activation is implied.
`LocalUnwindFrameProofInspectsCalleeWritesAndReturnPC` uses decoded caller/helper
instructions to check a neighbouring parent-frame write, partial saved-SP
overwrite, unknown write address, return-PC corruption, wrong return SP,
nested calls, malformed/incomplete bodies, and missing or stale callee evidence.
Two- and three-body call chains and repeated invocations retain a proved cell;
opaque calls, direct/mutual recursion, callee-save register clobbers, nested
one-arm writes and exhausted budgets do not. Both explicit writes and a
stack-switched CALL must preserve every active return PC. Missing or changed
inner bodies invalidate the entire receipt, and a repeated body is checked
again with the second invocation's actual argument addresses.
`LocalUnwindCalleeDepthAndWorkLimitsDoNotPublishPartialFacts` checks shallow
chains, the 16-body limit, deeper chains and an explicitly exhausted budget.
Missing instruction slices in either caller or callee, a symmetrically removed
callee branch edge, and a wholly removed caller branch arm cannot hide the path
that overwrites a saved cell. Complete caller diamonds retain their frame proof.

`RegistrationConditionalFrame` checks the pinned PE32 xcpt4 helper's incoming
counter, search filter and finally effects. Its controls reject truncated object
bounds, aliases to return-PC or argument storage, pointer-bearing object cells,
uninitialized private reads, unknown nested calls, exhausted work, writable code,
far returns and a missing ordinary return. A filter or finally return alone
cannot establish a returning helper. `WindowsRegistrationNative` also checks
that source helper facts cannot mutate native IR without a replacement call
receipt. These focused checks do not establish full xcpt4 or nonlocal-jump
support.

`RegistrationJumpRuntime` mutates each capture/resume/unwind binding in direct
and indirect forms, including loader storage, DLL identity, delay imports,
architecture, veneer opcode/slot and writable code. `RegistrationJumpCapture`
checks physical PC and frame values, argument/admin overlap, wrong protocol,
unknown callback roles, detached operations, source slices, caller ownership and
work exhaustion. Its pinned xcpt4 CFG check requires all seven physical capture
PCs to begin ordinary fallthrough blocks without becoming independent roots.
`RegistrationJumpState` checks solver-local capture/resumption, partial buffer
writes, unused unwind-data writes, unknown intervening calls, altered IAT bytes,
missing authentication and absent physical continuations. Buffer tests retain the
six untouched DWORDs and mutate every initialized byte. Native controls reject
both capture facts and unconsumed resumption receipts without changing IR. These
checks do not establish full xcpt4, recursive nonlocal finally effects or shared
CFG/SSA resumption support.

CRT source regressions use synthetic tables and the pinned Windows corpus:

```bash
cmake --build build-release --target \
  NeverDImageAnalysisBoundaryTests NeverDMedEntryFrameTests \
  NeverDMachOPointerRelocationBoundaryTests \
  NeverDNativePointerRelocationBoundaryTests \
  NeverDLLVMCValueTests NeverDLLVMCVectorTests NeverDLLVMCVoidAnalysisTests \
  NeverDWindowsEHCorpusTests NeverDLiftTests --parallel 4
build-release/bin/NeverDImageAnalysisBoundaryTests
build-release/bin/NeverDMedEntryFrameTests
build-release/bin/NeverDMachOPointerRelocationBoundaryTests
build-release/bin/NeverDNativePointerRelocationBoundaryTests
build-release/bin/NeverDLLVMCValueTests
build-release/bin/NeverDLLVMCVectorTests
build-release/bin/NeverDLLVMCVoidAnalysisTests
build-release/bin/NeverDWindowsEHCorpusTests
build-release/bin/NeverDLiftTests --gtest_filter='ExceptionCFGSeed.*:COFFException*'
```

`MedImmutableTableScanTests` checks exact sentinel exit counts, integer widths,
mutable/truncated storage, independent roots and budget refusal. Pointer tests
cover nested recurrences, shared expression DAGs, role-neutral address leaves
and independently materialized SELECT arms. Both suites retain ambiguous
relocatable comparisons and distinguish adjacent objects' one-past addresses.
`MedConstantPropagation` also checks full SSA identities at maximum integer
IDs/versions and atomic budget refusal on reverse-ordered width chains.
`HighCSourceCalls.ReusedConverterRebuildsClobberIdentityAndOrder` checks first
record precedence and clearing/restoring clobbers between conversions of the
same function, against a fresh converter. The worker code-edit checks cover
every C++ raw-string prefix while renaming surrounding identifiers.
C value tests compile and execute
relocated address arithmetic and freeze projections at O0/O2; vector and FP
helper tests cover indirect calls and emitted Windows analysis bodies.

`NativeImages/MedImmutableTableScanMatrix` runs the same scan, address-width,
PHI-completeness and refusal contracts across x86, x64, ARM and AArch64 in each
of PE/COFF, ELF and Mach-O. It rejects narrow address tags, unmarked address
bits, duplicate/missing predecessor arms, volatile reads and independent
entries. `DefinitionInventorySharesTheEvaluationBudget` prevents uncharged
definition indexing from bypassing the bounded evaluator. These are shared
MedIR/storage checks, not execution of each platform's runtime.
The matrix also retains graphs owned by source-call certificates; scalar
equivalence alone does not authorize erasing their original LOAD occurrences.
`ImmutablePEPointer.ScalarReadsRespectTheTargetPointerWidth` verifies that a
32-bit relocation cannot claim its adjacent null sentinel. Function discovery
checks debug storage and FDE extents on all twelve native target/format cells,
including Thumb-tagged pointers and explicitly named interior entries.
`ImmutablePEPointer.CodeTargetsNeedImmutableAlignedInstructionStorage` retains
x86 unaligned entries but rejects unaligned AArch64 targets, writable code,
truncated instructions and targets lacking function evidence.
`ImmutableImageBytes.ARMLiteralReadsStayInsideTheirDataIsland` rejects reads
that cross from an ARM literal island into instructions in all three formats.
`PreservesPureRematerializedTableBaseRecurrence` exercises both untagged and
role-neutral address leaves in PE, ELF and Mach-O. Native ELF/PE fixtures also
require AArch64 ADRP/ADD and x64 LEA recurrences to resolve through the same
table-address model.

Pointer boundary tests also cover bounded countdown traversal, masked and
guarded word offsets, nullable callback slots and runtime/native callback
choices across the same twelve target/format cells. Refusal cases retain
partial pointers, mismatched SSA guards, mutable tables and independent loop
entries. Linux x64 native execution compares callback choice and descending
constructor order against independent C observations at O0/O2. Immutable
scan tests additionally check lossless address carriers and folding a scan's
exit count into successor PHI operands.

`ExactIndirectTargetsReuseTheirRecoveredCallSignature` checks all twelve
target/format cells: a function identity or immutable slot uses the recovered
callee signature, including narrow parameters and surplus caller registers.
Mutable and atomic loads retain runtime dispatch, and missing required
arguments fail explicitly. A consumed void-call result remains unknown, and
immutable table observations survive target resolution. The native pointer
test compiles LLVM, default C and exact-type C at O0/O2 and checks the narrow
argument's actual value after the indirect call. Function-address initializers
also require declarations matching the emitted definitions before the tables.
`ImportCallSignaturesDoNotDependOnFirstDirectCallOrder` adds 228 cases across
those twelve cells: zero, integer, FP, variadic and va_list imports, both call
orders, indirect-only use, narrow aliases and missing required arguments.
The shared ABI test adds 90 register-forwarding cases across x64, ARM and
AArch64, including local setup, predecessor flow, intervening calls, alternate
roots and conflicting/addended/name-only import identities. A Win64 machine
fixture checks the `_popen` incoming argument at both optimization settings.
The native pointer oracle executes real `getpid` and `labs` imports with 1024
inputs through LLVM and both C modes at O0/O2 on Linux x64. Pipeline outcome
tests reconcile late exact targets and reject missing arguments after shard
linking even when the LLVM verifier accepts each input module. A narrow-return
case checks scalar bit preservation, source identity and obsolete range
metadata. Same-type calls with conflicting caller/callee stack-cleanup
conventions are refused even when the LLVM verifier accepts them.
`TargetAggregateLayoutCompilesAcrossArchitecturesAndFormats` checks each
target's pointer width and compiles LLVM-derived record size/offset assertions
with Clang for all twelve target/format combinations, without host headers.
`OptimizedSwitchTablesPreservePhysicalAndTwoLevelSelectors` executes original
x64 machine code and both optimized/unoptimized lifted LLVM at O0/O2, checking
1024 selectors plus the two-level default boundary against independent results.

`MedABIPass.SplitStackArgumentSetupRequiresEveryIncomingPath` checks all twelve
architecture/format cells, block-order changes, conflicting or missing stores,
partial overwrites, opaque calls, aliases, provenance and independent/EH roots.
`I386CallContract.SelectingTheCallerPreservesTheCalleeArgumentContract` compares
selected and whole-function output, cdecl and register arguments, direct and
forwarding callees, HighIR and LLVM, and default/NoOpt in all three formats.
Runtime stack-probe audits require the exact platform and stated name evidence.

`X86_64_DebugRecords.RecordsCCannotSpellKeepMachineTypes` compiles and executes
the generated C at O0/O2: opaque record pointers retain their names, by-value
arguments keep their carriers, and the callee returns both words used by its
caller. `RegistrationState.DeadFrameValuesDoNotAccumulateAcrossJoins` checks
bounded sparse state and overlapping pointer taint in all three frame spaces.
`LanguageRuntimeDetection.ExportsProvideTheSameLanguageEvidenceAsSymbols`
checks exact runtime names, C++ decorations and Rust prefixes in exports.

`MedCxxContinuationFrame` verifies that catch and normal paths address the same
local, rejects inconsistent unwind/stack effects and ordinary roots, and checks
the public MSVC nested catch's parameter count and resume labels. The broader
Windows corpus includes Clang FH3 state maps. `ExceptionCFGSeed` covers exact
return-PC state lookup and an IP boundary inside an instruction. These source
checks do not establish native execution of regenerated C++ exceptions.
For MinGW performance comparisons, use Release and time a fresh full EXE load
as well as `decompile --llvm --func`; retain the binary, command and output
outside the repository. Successful output alone is not semantic equivalence.

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

PE32 registration changes also require the focused state, frame and native
targets. Changes to runtime entry stacks also require
`NeverDMedStackAlignmentTests`, `NeverDNoReturnTests` and the `MedSSAMultiRoot`,
`MedSEHEstablisherFrame`, `MedSEHHandlerEntry`, `MedTempIdentity` and
`Win64Forwarder` regressions in `NeverDLiftTests`. These cover independent
ordinary entries, restored versus private callback stacks, malformed root
carriers, and existing x64 handler-frame behavior.
`RegistrationCallABI` in the native target checks immutable scalar ThrowInfo,
the exact CRT import, private object initialization and width, caller-PC
observations and metadata mutation. The leaf-callee matrix also checks separate
private-stack/object spills, pointer escape, bounds, unknown addresses and
nonvolatile-register preservation. These proofs do not enable native source
C++ reconstruction on their own.
`StackCleanupRequiresEveryRestoredNearReturn` separately checks balanced
caller/callee cleanup, nonvolatile restoration after an opaque call, dynamic
alignment, disagreeing return pops, missing restoration, volatile anchors,
memory-only saved pointers, far/tail returns, writable code, wrong targets and
exhausted work. `StackCleanupIncludesCheckedNestedImportCleanup` verifies
provider and exact-slot identity before a nested stdcall can balance its caller.
`NestedReturningStackProofIsBoundedAndNonCircular` checks multi-level callee
cleanup, mismatched pops, recursive dependencies, the depth boundary and shared
work exhaustion. It does not admit dispatcher-entered parent returns through
an ordinary subgraph. `SEHReturningStackUsesCompleteParentFrameEvidence`
separately checks complete registration state and lifetime, corrupted EBP,
missing ESP restoration, incompatible ordinary/exceptional return pops,
writable code, stale installation metadata and an opaque helper overwriting
a saved stack value, both directly and through an outer helper.
`StackCleanupDoesNotGrantMemoryBorrowAuthority` checks exact
direct/indirect target identity, registration-overlapping pops and duplicate
contracts while retaining the independent call-frame refusal.

The Windows nonlocal-call regressions also check `_setjmp3`, `_setjmpex`,
`__intrinsic_setjmp`, `__intrinsic_setjmpex` and MinGW's ARM wrappers through
the shared returns-twice/no-return tables. PE32 import calls retain their LLVM
attributes for IAT and register-carried targets at both optimization settings,
and the CFG retains ordinary continuations only for returning entries. The
native pointer target adapts these spellings with tail veneers to the host's
independent `setjmp`/`longjmp` runtime and checks 512 live values on each calling
route at `-O0` and `-O2`. Pointer mirrors honor the loader's post-relocation
read-only record as well as section names; the four-architecture, three-format
signature matrix checks this alongside mutable and atomic counterexamples.
These control-flow properties follow
the [Microsoft setjmp contract](https://learn.microsoft.com/en-us/cpp/c-runtime-library/setjmp3)
and [MinGW-w64 runtime declarations](https://github.com/mingw-w64/mingw-w64/blob/master/mingw-w64-headers/crt/setjmp.h);
they do not prove the contents or lifetime of a caller's jump buffer.

`OnlyCanonicalWaitPreservesLocalFinallyFrameEffects` checks nine intrinsic shapes, the finally's observed parent-frame write, restored argument ESP, registration teardown and retained exception-dispatch roots. Only the canonical FWAIT passes; all eight malformed or different intrinsic forms retain the refusal.

Cleanup-relay tests cover both EBP displacement widths, exact thiscall object
reads, PE32 relative-branch wrapping, nonwrapping storage, writable/overlapping
code, fixups, call substitutions, callee stack pops and FS-dependent effects.
The independently loaded MSVC fixture also checks its two actual relays and
their destructor footprints.
The state target also checks source-call identity, initialized ECX object
borrows, registration/SavedESP separation, partial stores, pointer taint,
conflicting predecessors and preserved catch resumption after a private throw.
`ExactLocalAccessesDoNotRescanTheWholeFrame` proves installation and unlink
after thousands of exact loads from a populated frame under the unchanged
cumulative work limit. `IndexedFrameAccessesKeepPartialAndUnknownAliases`
checks unaligned overlapping writes, unknown-address reads, disjoint cells and
signed-offset endpoints in parent, entry and callback coordinates.
`CanonicalNoReturnKeepsExceptionalFlowWithoutBorrowing` checks direct and
indirect canonical no-return calls without a callee memory contract, retaining
catch dispatch/resumption and rejecting conditional, unmarked, mismatched or
unbound instruction evidence.
`LocalUnwindIdentityRequiresCurrentProviderAndVeneer` checks PE32 EH3 runtime
identity across exact IAT and immutable jump forms, including wrong providers,
conflicting slots, changed/writable code and other architectures or formats.
`LocalUnwindRetiresLevelsAfterCheckedFinallyEffects` checks argument binding,
zero-step unwinds, finally writes, preserved caller argument ESP, invalid or
non-ancestor levels, cyclic scope tables, unknown callback calls, runtime-slot
clobbers, nonlocal stack changes, return pops, conditional exits, trailing
return effects, atomic writes and all-path joins with agreeing or conflicting
finally effects. It retains the
independent memory/native-output refusal.
`LocalFinallyOwnsOnlyItsAllocatedCallbackStack` checks a saved parent EBP,
a private callback frame, restoration and an observable parent-local write.
Negative cases cover uninitialized and partial spills, reads below the
allocation or at the runtime return PC, freed/reallocated storage, an
unbalanced return, partial EBP restoration, runtime-slot writes, escaping
callback pointers and opaque calls. `LocalUnwindSplitsCurrentMachineStateAtCall`
also decodes the corresponding PE32 push/frame/pop sequence and mutated
return-stack, return-PC, frame-register and pointer-escape cases. These checks
do not admit nonlocal finally exits or infer a native memory-borrow contract.
`SEHCallbacksMustReturnToTheirCurrentInvocation` covers filters and finally
handlers under EH3 and EH4: restored and unbalanced stacks, changed EBP,
return-pop immediates, conditional returns, trailing effects and missing or
mismatched instruction identities. The C++ catch-return suite applies the
same unconditional-return rule before publishing a continuation.
`SharedSEHCallbackTailsKeepEachInvocationStack` checks distinct EH3/EH4 filter
and finally entries sharing an epilogue, with reversed discovery order. Each
invocation owns its initialized spills and restored EBP/ESP. Uninitialized
storage in one invocation cannot borrow another's facts, and a shared source
value that is scalar in one invocation cannot claim the other's exact frame
offset. Return-PC writes, an unbalanced stack, return-pop immediates and an
independent ordinary entry remain rejected.
`SEHActivationsShareTheCumulativeWorkBudget` checks that many callback entries
cannot restart the state budget; exhaustion publishes no block or frame facts.
The native call target checks cumulative failed-proof budgets and fresh-image
callee indices.
Catch-return tests require the pre-dispatch SavedESP snapshot to survive catch
writes and subsequent continuation reads; unknown snapshots remain rejected.
The native fixture independently edits or removes the writeback, changes its
value/address, and moves or duplicates it before installation. Run
`check_windows_registration_cxx_rewrite.py --saved-stack-probe` with the same
MSVC inputs and tools as the ordinary source oracle. It derives a bounded
machine-code probe that overwrites SavedESP with 7 and then reads the
initialized inner guard through the restored pointer. Its report distinguishes the derived input from its
hashed native MSVC baseline. CI runs both capture forms through all six
preferred/rebased routes in Wine and replays those identical files on Windows.
Cleanup projection tests check every reachable unwind state, initialization
before state activation, missing or mismatched contracts, registration and
SavedESP overlap, released storage, pointer taint and predecessor conflicts.
Physical-return tests separately cover entry-EAX pass-through, computed
scalars, object loads, caller-PC results and distinct return predecessors.
Reachability tests distinguish empty pre-install/post-remove states from dead
blocks, retain runtime catch/resumption roots after a private throw, and reject
incomplete proofs, changed block ranges and missing/mismatched call receipts.
The native call index memoizes relay contracts under the same shared budget;
a descriptor alone grants no initialized object borrow.
C++ runtime-object tests cover value/reference homes, exact type/width, private
spills, bounds, scalar writes, partial pointers, ordered/FS accesses, escapes
and stale catch identities. Loader tests also cover adjacent catch labels and
independent function boundaries. Genuine MSVC value and reference images must
agree in whole-module and selected-function state replay; the reference fixture
checks all four runtime object occurrences.
Scope mutation tests cover both SEH3 and EH4, exact exclusive table ends,
partial overlap, PE32 overflow and cookie separation. The real source fixture
also rejects an edited LLVM write to either format's scope table.
C++ metadata tests cover all three FuncInfo versions, exception-spec records,
reparsed graph equality, PE32 exclusive ends, distinct record overlap and
writes from ordinary calls and cleanup relays. A private-throw closure test
requires the exact checked CRT import and retains caller-PC write/read guards.
The genuine MSVC fixture also rechecks all four record extents and its throwing
helper and cleanup relays. These component facts do not imply source native
C++ installation.
PE32 C++ personality tests distinguish the original FuncInfo-loading thunk
from the CRT entry, checking exact relocation operands, immutable storage,
import identity and conflicting pointer records. Native-source classification
tests reject changed registration, language and object contracts, unsupported
dispatch shapes and missing compiler receipt capabilities. Genuine value/reference fixtures additionally
verify typed catch and cleanup IR, compile it through the patch code generator,
and authenticate each indexed catch row against its exact child funclet range.
Changed range ownership must reject the machine-code receipt. These checks do
not execute a reconstructed C++ source PE.
With `LLVM_NEVERD_X86_CXX_FUNCTION_RECEIPTS`, the same source fixture also checks
the complete FuncInfo/unwind/try/handler bytes through the public COFF table
validator. It requires all nonempty cleanup rows, original RTTI identity,
physical catch subfields and exact absolute pointer fixups. Thirty mutations
cover raw headers and graph edges, missing cleanup receipts, overlapping
sections, missing/overlapping fixups, width, addend and target changes. The LLVM
MC regression separately closes table extents, generated indices and object
bounds. These receipts still do not authenticate edited IR effects or final PE
installation.
Native installation needs the LLVM fork exposing
`LLVM_NEVERD_X86_REGISTRATION_EH` and, for EH4,
`LLVM_NEVERD_X86_REGISTRATION_COOKIES`; GS source frames additionally need
`LLVM_NEVERD_X86_REGISTRATION_GS`. The pinned r5 packages and their matching
source commit provide these receipts, including the C++ catch-subfield,
function-range and handler receipts needed by native C++ reconstruction.

```bash
cmake --build build --parallel 4 --target neverd \
  NeverDRegistrationStateTests NeverDRegistrationEHTests \
  NeverDWindowsRegistrationFrameTests NeverDWindowsRegistrationNativeTests \
  NeverDNoReturnTests
build/bin/NeverDRegistrationStateTests
build/bin/NeverDRegistrationEHTests
build/bin/NeverDNoReturnTests
build/bin/NeverDWindowsRegistrationFrameTests
build/bin/NeverDWindowsRegistrationNativeTests \
  --gtest_filter='-WindowsRegistrationNative.InputPE32PreservesItsCheckedSourceContract:WindowsRegistrationCxxSource.*:RegistrationEntryABI.InputPE32*:RegistrationRelift.InputPE32*'
for registration_case in filter nested-finally continue-search continue-execution normal-finally cdecl-parameter cdecl-parameter-write eh4-filter; do
  python scripts/check_windows_registration_rewrite.py \
    --test-binary build/bin/NeverDWindowsRegistrationNativeTests \
    --patch-binary build/bin/neverd --case "$registration_case" \
    --output "build/evidence/registration-${registration_case}"
done
```

The SEH rewrite script also runs `longjmp-finally`, `longjmp-zero`,
`longjmp-filter`, `longjmp-parameter`, `longjmp-callback`,
`longjmp-unwind-callback`, `longjmp-callback-zero`, `longjmp-search-filter`
`longjmp-search-filter-zero`, `longjmp-retired-scope` and
`longjmp-retired-inner-scope`, `longjmp-callee`,
`longjmp-returning-callee` and `longjmp-scalar-callee`. The helper fixtures
execute cross-frame resumption, ordinary returning object borrows and scalar
argument projection. Independent mutations reject frame observers, changed
buffers/objects, ABI attributes, observed incidental results and scalar reads
moved before source writes. Shared frame tests retain exception-scalar alias taint across partial
spills and reject mixing it with physical pointer bits. These fixtures check a resumed
nonvolatile register and try level, the requested jump value (including zero
becoming one), finally's
private-frame write, a subsequent exception, and FS:[0] restoration on four
successive invocations. Original and generated caller PCs are distinguished,
and each generated image is executed at preferred and forced-rebased addresses.

`RegistrationFrameScalar` checks that exact PE32 frame differences and equality
preserve their results under relocation, including integer wraparound. Distinct
frame roots, partial stores, ambiguous joins and ordered comparisons retain
their conservative provenance. `LowScalarConstant` compares constant multiply
and shift facts with concrete LowIR execution at 1/2/4/8-byte widths, including
overflow, zero shifts and saturating counts. Unknown operands and malformed
widths supply no fact. The native MSVC input owner also checks chain
observations in `nonlocal_finally_reenter` across `/Od`/`/O2`, `/GS-`/`/GS` and
raw/optimized projection. The focused Windows EH job runs the complete native
input owner with its verified MSVC images. Its exact source state does not discharge the separate
CRT guard retirement obligation; native reconstruction remains rejected while
that proof is incomplete.

The parameter case reads the real caller argument in both the resumed parent
and its filter, including the caller-frame slot introduced during outlining.
The callback cases resume from ordinary finally calls and runtime unwind
dispatch, recover their parent's native buffer, and exercise private argument
slots and zero-to-one conversion. Mutation controls reject a different parent
frame, escape index, unused recovery, escaped buffer and redirected value load.
CLI coverage includes both patch modes from emitted IR and directly from the
original EXE, with default optimization and `--no-opt`. Both pipeline paths
must preserve `optnone` runtime callback roots and register snapshots.
`longjmp-runtime-raise` and `longjmp-runtime-continue` call the original
`RaiseException` import after resumption and exercise handler entry and filter
continuation. `longjmp-control-fp` observes both `_controlfp_s`'s scalar result
and private output before capture. Sixteen mutations per fixture check the
runtime ABI, provider/IAT identity, exact argument values, import storage,
attributes and argument-read ordering. The continuation fixture also checks
that a helper immediately following a closed registration body does not
inherit its EH ownership. `RegistrationInstructionRange.*` covers all runtime
roots, unresolved/escaping edges, overlapping instructions and budget refusal.
Use `--runtime-root` with the captured `runtime-libs` directory to bind SEH and
jump imports to the authenticated Microsoft x86 CRT. This is required under
Wine for the retired-inner-scope case: Wine 9's builtin CRT unwinds below the
saved scope differently. The JSON report retains the provider manifest and
the output directory contains the exact DLL. Focused CI captures this runtime
from MSVC, runs all source cases in Wine, then replays the same 20 images per
case on Windows with `replay_windows_registration.py --source-only`. Replay
verifies every image hash, runtime DLL hash, code owner and relocation base.
Dynamic-scope mutations alter selectors, state cases, restore values, source
stores and overlapping aliases; each must fail installation. Dead-finally-result
tests reject observable, aliased, volatile and atomic spill chains.
`longjmp-nested-finally-call` calls a returning finally from an ordinary finally
invocation before resuming the parent. `longjmp-nested-finally-unwind` enters
the outer finally through CRT unwind. Both distinguish the inner/outer frame
writes and then raise another exception. Sixteen installer mutations per
ordinary finally call check its source owner, target, frame, abnormal flag,
ABI and attributes. The clone regression uses two calls sharing unobserved
result storage and checks atomic refusal when a volatile load observes it.
The ordinary-entry and runtime-unwind source tests share direct-call and
folded tail-jump controls, including effects hidden after a CALL.

Callback TryLevel coverage is split across `RegistrationSEHTryLevel.*`
(state/frame correlation, retirement, narrow writes and source receipts),
`RegistrationSEHStateStore.*` (exact MedIR store binding, preserved high bytes
and retained scopes),
and `RegistrationSEHStateNative.*` (EH3/EH4 lowering, source replay, callback ABI
retention and physical write mutations). `CompiledSEHStateWrites.*` checks
receipt transport through layout changes and final ownership/range closure;
`CompiledSEHStateStoreBytes.*` checks every architectural address register,
displacements and rejection of extra instructions or indexed addresses.
The native receipt tests require
`LLVM_NEVERD_X86_REGISTRATION_SEH_STATE_WRITES`, supplied by the pinned r5
packages. Older r4 builds retain analysis and refusal coverage but cannot run
these compiler-protocol tests. The explicit `r5` source replay profile covers
these 64 scenarios. The complete `all` profile additionally requires the
dynamic-call capability described below.

`registration_state_dispatch.s` adds `longjmp-state-byte-high-bits`: the same
filter byte store produces complete states 0 and 256. Four alternating invocations
require one or two cleanup visits, respectively, and check the restored EAX/EBX,
final state and registration chain. Intermediate sibling rows have ordinary
activation stores to isolate the result domain from deep nesting and unrelated
reachability limits. Two separate nonlocal call sites keep each parent call's
active level unambiguous. The 20 runtime images cover source and generated PE,
both CLI modes, IR/EXE input, optimization on/off and rebasing.
`RegistrationSEHStateDispatchTests` replays 20 verifier-clean edits to the actual
reconstructed IR: changed read address/width/ordering, missing or extra uses,
changed domains, swapped or bypassed cases, altered or duplicated updates,
hoisted reads and escaped case/continuation addresses all refuse installation.
Reordering the physical case blocks remains accepted. MedIR tests separately
cover EH3/EH4 sentinel and positive outcomes of the same byte store.

`longjmp-finally-incidental-result` uses the same state-selection fixture but
returns directly after `RaiseException`. The frozen pre-fix emitter refuses
this source. `RegistrationSEHRuntimeReturn.*` checks that only a runtime-only
finally can discard the incidental scalar result: the source body, all entry
kinds, ordinary call/branch edges and exact physical near returns must agree.
An ordinary caller, shared filter/handler entry, parent return, memory write or
other scalar use remains observable. This proof is shared by emission and COFF
source replay through the MedIR call-result binder.

`longjmp-deep-scope-chain` uses 259 nested scope rows and walks 254 search-only
rows during nonlocal unwind. Its 20 runtime images check both cleanup counts,
resumed EAX/EBX, final state and FS:[0]. Distinct call sites isolate scope depth
from the separate parent-call state-selection contract.
`RegistrationSEHTryLevel.DeepUnwindKeepsTheDefaultProofBudget` and
`RegistrationDispatch.SharedFinallyRetainsEveryLevelWithinDefaultBudget` cover
1,024 levels without changing the work cap, including reversed block order.
`RegistrationDispatch.ManyDistinctFramesStayWithinDefaultBudget` checks 1,024
different parent cells with otherwise equal registers, in both block orders.
The corpus filter-thunk HighC test guards readable `xcpt4` output against
snapshot-index budget regressions.
Companion tests preserve distinct source snapshots, converging sibling paths,
recurrent-path rejection and observable finally writes.

`longjmp-shared-parent-call` reaches one nonlocal call through two active levels
of the 259-row table. `shared-parent-throwing-call` raises from one ordinary
helper call and must select different handlers on alternating invocations.
Each fixture checks 20 original/generated executions, both patch modes,
optimization settings and rebasing. They require
`LLVM_NEVERD_X86_REGISTRATION_SEH_DYNAMIC_CALLS`. Source-IR mutations remove or
alter call-site markers, noinline and notail, or move authority to a callee,
scope intrinsic or function. `RegistrationSEHDynamicScope` checks that PHIs
retain original predecessor/value pairs across scope selection.
Deleted or duplicated source-call receipts and changed operation identities
also fail both the generic exception counters and PE32 source replay. LLVM
regressions check live setjmp state, SEH3/EH4 emission, unmarked calls and rejection
of hoisting a later state reset before an inherited call. A separate register
liveness regression checks a value observed only after handler entry, including
a nonreturning call and optimized IR. The runtime handler-selection fixture
reproduced a stale frame address in EDI before this compiler fix.

`longjmp-private-parent-call` and `longjmp-private-parent-graph` place a private
leaf or a two-level helper before the shared nonlocal jump. The live capture,
one helper effect, finally count, restored state, registers and FS chain must
agree in all 20 original/generated executions per fixture.
`RegistrationSEHPrivateCall` rejects caller-frame access, object borrowing,
callee cleanup, code/table/import writes, recursive calls, changed instruction
bytes/slices and caller-PC readback. `RegistrationPrivateCallState` checks that
stack-only facts cannot preserve a capture, deallocated buffers refuse and a
frame-preserving call supplies no scalar result authority.

The EH4 jump matrix repeats fifty-nine capture/resumption scenarios with and
without GS cookies: parent and callback jumps, zero values, filter search, incoming
parameters, retired scopes, private leaf/graph calls with a live buffer, and
registered EH3/EH4 helpers borrowing the parent buffer and a scalar object, and
correlated filter searches while a finally invocation is suspended.
The helper cases require exactly one observed helper effect after resumption.
`RegistrationSEHPrivateCall.EH4CalleeGraphsPreserveTheImageCookie` rejects
transitive cookie/header writes, partial cookie overlap and invalid cookie
addresses. Each case compares the original and nine generated
variants at two bases under the authenticated native CRT. The source and MedIR
mutation tests reject wrong unwind protocols, cookie addresses, argument extents
and cookie overlap; callback scratch tests retain rejection of cookie/table writes
and unknown addresses. The EH4 corpus regression separately requires a CFG block
boundary at each physical `_setjmp3` return PC. Older replay profiles retain their
historical case sets and exclude these new EH4 jump cases.

`RegistrationMultiStateJump` checks a shared capture reached with two source
states, including identical later active states, changed buffers, invalid stores,
clobbered capture bytes and reversed block order. `RegistrationSEHJumpABI` checks
that the captured bound disambiguates identical visible unwind states.
`longjmp-multistate-capture` and its EH4/GS variants alternate the two bounds on
four calls, unwind the same finally, check the restored state and nonvolatile
register, then raise another exception. The native mutation tests alter the saved
state slot, read address, volatile accesses, placement, use closure and captured
projection key while retaining verifier-clean IR. The full source profile now
contains 216 scenarios; older compiler profiles keep their original membership.

`longjmp-finally-{register,frame,private}-loop` and the corresponding
`longjmp-filter-*` fixtures add 18 EH3/EH4/EH4+GS scenarios. Three iterations
contribute observable trace digits before a suspended finally continues;
register, parent-cell and callback-stack counters must all survive reconstruction.
The complete profile has 4,320 original/generated executions at two bases.
`RegistrationCallbackLoop` rejects recurrent or unknown counters, bad branch
identities, live-buffer writes, unbalanced stacks and exhausted work, while
accepting reordered blocks and successors. `RegistrationSEHPHICopy` checks exact
source-edge and callback ownership. `RegistrationRecoveredCopy` authenticates
parent escape indices, both callback frame recipes and all scalar-slot aliases;
wrong parents, unknown recoveries, address escapes, atomic or mistyped accesses
and observable frozen values refuse installation. Ten verifier-clean mutations
per actual outlined copy block also reject changed recovery, storage, operations
and control flow when replaying the linked source fixtures.

The EH4/GS `longjmp-callee`, `longjmp-returning-callee` and
`longjmp-scalar-callee` variants retain the source helper's registration while
mapping its caller's generated buffer and object. `RegistrationBoundCallState`
checks full/partial cookie overlap, registration storage, unused object-address
arguments and allocation bounds. Runtime source mutations retarget the actual
object-address instruction into both cookie slots; source replay and installation
must reject them. The MedIR binder also rejects missing cookie proof or invalid
cookie addresses before lowering the preserved call.

The corresponding `longjmp-eh4-callee`, `longjmp-eh4-returning-callee` and
`longjmp-eh4-scalar-callee` variants use a separately registered EH4 helper.
Their `-mixed-gs` variants invert only the helper's GS configuration, covering
all four caller/helper cookie combinations without sharing their local proofs.
`RegistrationSEHBoundCookieTests` mutates its actual cookie load width, global
address, access direction, frame XOR, encoded scope pointer, and full/partial
cookie stores. It also changes the live GS check parameter and requires source
replay and native installation to reject it. A changed check in the fixture's
unreachable except body remains harmless for that proved invocation; the live
ordinary-return check retains its independent obligation.

`RegistrationCopyEdge` checks unavailable handler-register PHIs through cyclic
private register/snapshot transports. Frozen poison is permitted only when its
entire bounded use graph has no observer. Return values, call arguments, global
stores, address escapes, arithmetic, atomic accesses, over-wide slots and budget
exhaustion refuse; C++ copy edges keep their stricter nonvolatile slot rule.
`RegistrationSEHDynamicScope.ProvesUnreachedSourceAddressExpressions` checks
all-arm frame-coordinate PHIs in source blocks without execution receipts.
Reordered equivalent arms and checked runtime handler roots pass. Exceptional
arms also require the same coordinate; malformed runtime definitions refuse.
Differing coordinates, missing provenance,
numeric addresses, narrow/wide aliases, duplicate definitions and cycles grant
no logical-state write authority.

`RegistrationFilterState.*` checks paired dispositions and states at shared
returns, captured search cursors, arbitrary negative dispositions, continuation
inside a finally block and reordered edges for EH3, EH4 and EH4+GS. Missing or
wrong image cookie inputs, full/partial EH and GS slot writes refuse EH4
resumption; a disjoint scalar write remains valid. Selected handlers abandon
only their own suspended paths, including a filter-activated cleanup. Unknown
results, escaping searches, bad stacks, recurrent selection and incomplete
instruction boundaries refuse. MedIR rejects unordered/duplicate selected scope
indices, finally rows, missing handlers, unknown roots and incorrect entry
states. Skipping one or several filter scopes records the last callback state
and its final runtime state separately. `RegistrationSEHJumpABI` checks complete
state projections and rejects missing, invalid, unordered or contradictory
pairs. Native installation mutations change the reload address, volatility,
atomicity, alignment, selector, projection keys/results, uses and placement
before the capture. They also remove a projection or compare another value.

`registration_filter_state.s` supplies thirty runtime cases. Five handler-selection
cases (`longjmp-handler-selected`, `longjmp-handler-cleanup`,
`longjmp-handler-captured-cursor`, `longjmp-handler-nested-finally` and
`longjmp-handler-mixed-resumption`) check handler entry state, cleanup/filter
order, actual caller PC, return value and restored FS chain. Handler-only cases
require the captured continuation to remain unreachable; the mixed case alternates
selection and continuation, checking distinct traces and restored registers.
Nested selection abandons both suspended callback invocations. They run with
EH3, EH4 and EH4+GS. Two additional cases, `longjmp-handler-cleanup-jump` and
`longjmp-handler-nested-cleanup-jump`, supersede selection from a direct or nested
cleanup. They require traces 236/2367, capture value 42, restored EBX and state 0;
the selected handler and suspended callback tails must not execute. The six
protocol variants also cover out-of-line EH4 callbacks. Source mutations reject
changed buffers, escaping frame values, incomplete CALLs and parent continuations
outside their range. Four more cases, `longjmp-handler-cleanup-value`,
`longjmp-handler-nested-cleanup-value`, `longjmp-handler-cleanup-zero` and
`longjmp-handler-nested-cleanup-zero`, replace the pending value 42 with 43 or
zero. All twelve protocol variants check the actual resumed EAX (43 or 1),
cleanup order, restored registers and registration state. The two
`-dynamic-value` cases load the current iteration from mutable image data and
subtract one, covering negative, zero and positive values without a constant
proof. Their six protocol variants check EAX on every invocation. State
mutations reject unauthenticated image reads, dereferencing loaded pointer bits,
and mixing them with a physical frame address. The two `-other-capture` cases
capture distinct buffers with different EBX, ESP and unwind bounds. Cleanup
replaces the pending second capture with the first and value 42 with 43; only
the first continuation may execute. All six protocol variants check EAX, the
restored state and registers
at both bases. `RegistrationReplacementJump` rejects unknown, overlapped or
modified destination buffers, including writes during subsequent cleanup and
writes to unused buffer padding; disjoint writes and reordered blocks pass.
`RegistrationSEHJumpABI` rejects fabricated callback locations, targets,
duplicate transfers and cyclic or incomplete replacement graphs. MedIR checks
exact nested invocation identity and independent ownership
of unreached callback tails; installation rejects removed noreturn attributes
and newly inserted normal returns. Eight `longjmp-search-filter-*` and
`longjmp-nested-search-filter-*` cases jump directly from the search filter:
the original capture, another live capture, or a branch that alternates jumping
with continue-execution or positive handler selection. All twenty-four protocol
variants distinguish EAX 43 on
replacement from the pending value 42, traces 23/263 on abandoned paths and
235/26375 on returning paths, and restore the destination's registers, state and
FS chain. `RegistrationFilterJump` checks pure and mixed exits across all three
protocols. Changed buffers, frame-address values, clobbered capture bytes,
incomplete CALLs and nondefault memory spaces refuse; disjoint writes and block
reordering remain valid. Shared filter fixtures live in their own test helper
files. The other seven cases are:
`longjmp-filter-correlated-state`, `longjmp-filter-shared-state`,
`longjmp-filter-captured-cursor`, `longjmp-filter-negative-disposition`,
`longjmp-filter-skipped-scope`, `longjmp-filter-two-resume-mappings` and
`longjmp-filter-callback-activation`.
The skipped-scope case writes state 1 in a filter; after
the suspended finally returns, the CRT skips that filter scope and resumes at
state 0. Its other branch retires to -1, requiring a variable projection.
`longjmp-filter-two-resume-mappings` activates its scope rows before capture and
checks both 2-to-1 and 3-to-0 resumption. It also exposes a numeric-bound bug:
LLVM's old depth-first order 0,3,4,1,2 skipped the finally when unwinding from
source state 4 to captured state 1. The compiler must preserve source order,
and `NonlocalUnwindPreservesSiblingStateOrder` independently rejects a sibling
permutation whose bytes, fixups and enclosing graph are otherwise valid.
`longjmp-filter-callback-activation` omits the ordinary activation of scope 2;
its distinct filter and handler acquire their entry proofs only from the
continued callback state. Unit controls require both roots and reject complete
proof publication when a filter path has an unknown disposition.
The genuine `CorpusFuncLoadX86FilterThunksPrintTheirValue` regression retains
structured filters when an ordinary finally's raise selects a handler: that
path must not be treated as a failed nonlocal resumption.
Each execution alternates both filter paths across four invocations, checks the
resumed TryLevel and nonvolatile register, and requires the exact cleanup trace
and restored registration chain. The source reconstruction and independent
replay scripts include these cases and their EH4/GS variants in the current
compiler profile. EH4 variants use the native CRT's cookie-aware unwind routine,
eleven initialized buffer words and the -2 frame sentinel. All seven variants
retain the exact trace, correlated filter state and restored nonvolatile value.

`registration_state.s` adds six EH3 runtime cases:

| Case | Checked callback state effect |
|------|-------------------------------|
| `longjmp-state-activate-call` (30) | An ordinary finally activates a callback-only scope before resumption |
| `longjmp-state-activate-unwind` (31) | A CRT-dispatched finally activates that scope before nested resumption |
| `longjmp-state-retire` (32) | A returning unwind callback retires a pending inner cleanup |
| `longjmp-state-sentinel` (33) | A callback leaves no active scope; the resumed parent later activates one |
| `longjmp-state-byte` (34) | A callback activates a scope with a byte store before nonlocal resumption |
| `longjmp-state-word` (35) | The same transition preserves the high half of a word store |

The six cases have passed local Wine execution against the captured Microsoft
x86 CRT: 120 original and rewritten image executions cover both patch modes,
IR and EXE input, optimization on/off and forced rebasing. Each execution checks
four invocations, the cleanup trace, resumed registers/state and FS:[0]
restoration. To run the complete script, use NeverD built against a compiler
with the capability above and the captured Microsoft x86 CRT. `--compiler` and
`--linker` select fixture tools, not the LLVM library linked into NeverD:

```bash
for registration_case in longjmp-state-activate-call longjmp-state-activate-unwind longjmp-state-retire longjmp-state-sentinel longjmp-state-byte longjmp-state-word; do
  python scripts/check_windows_registration_rewrite.py \
    --test-binary build/bin/NeverDWindowsRegistrationNativeTests \
    --patch-binary build/bin/neverd \
    --compiler /path/to/neverd-llvm/bin/clang \
    --linker /path/to/neverd-llvm/bin/lld-link \
    --wine wine --runtime-root /path/to/runtime-libs \
    --case "$registration_case" \
    --output "build/evidence/registration-${registration_case}"
done
```

`RegistrationSEHJumpABI` checks exact stack argument order and arity despite
incidental register values, extra private spills and conflicting inferred
prototypes; missing arguments and changed source receipts remain rejected.
The source-input unit rejects edits to capture/resume identities, the private
buffer, register snapshots, restored state and argument slot. Its unwind-adapter
controls change the copy extent, physical registration field, virtual EBP,
original-buffer isolation, runtime identity and call attributes. All mutations
must remain LLVM-verifier-clean and fail the independent installer proof.

With genuine MSVC value/reference fixtures, run the source IR and machine-code
checks once per image. Missing input is a skip and cannot count as verification:

```bash
NEVERD_REGISTRATION_INPUT_CXX_PE32=/absolute/path/to/original.exe \
  build/bin/NeverDWindowsRegistrationNativeTests \
  --gtest_filter='WindowsRegistrationCxxSource.*'
```

The genuine C++ input unit also checks original image storage when an ordinary
helper is emitted first, an emitter is reused and a helper-only shard masks the
native parent. Its independent source/control validator rejects 24 edits to
block and operation identities, call ABI and source targets, semantic pads,
catch subfields, unwind/resumption edges, chain reads and localescape. Terminal
no-return boundaries remain indexed after unreachable-code pruning. These
control and compiler-table checks still do not establish source PE installation.
The full C++ IR validator additionally replays actual affine frame addresses,
scalar destructor borrows, definite byte initialization on normal and exceptional
edges, typed runtime reference accesses, and original image storage. Its frame
mutations keep the independent control proof valid while changing object bases,
initialization width, undefined values, pointer-bearing scalar objects, private
global storage, runtime reference homes and immutable metadata/runtime writes.
Both value and reference inputs must pass the complete proof; a table receipt
alone does not authenticate these effects or enable final PE installation.
With `LLVM_NEVERD_X86_CXX_HANDLER_RECEIPTS`, the fixture additionally checks the
actual generated registration handler's private owner, parent range and physical
node. Thirty-three changes to its opcodes, FuncInfo/runtime targets, exact
fixups, decoded parent store and SafeSEH metadata must reject while the complete
language-table proof still succeeds. MC checks changed handler identities,
cross-row agreement and bounded node layouts separately. These checks do not
execute an installed source C++ PE. The unit also composes these proofs through
prepare/apply/final validation, merges original/generated SafeSEH handlers and
checks all nine dispatch HIGHLOW fields. It covers fixed images and disabled
SafeSEH without activating a partial handler table. Nine rejected preparation
changes leave generated bytes unchanged. Nine final-image mutations reject,
including missing/redirected entry bytes, a changed machine class and original
entry permissions. Four changed entry receipts (missing, duplicate or shifted
source/target RVA) reject independently of the generated section hash.
The compiler table receipt also normalizes the generated FuncInfo with the
shared COFF decoder and closes all four indexed table extents. Final validation
reparses that graph from actual PE sections, rejecting missing graph/encoding
receipts and changed normalized try/unwind edges. A changed FuncInfo with a
freshly recomputed section hash still rejects. This verifies the installed
language graph. The genuine input test also reloads each public output through
the ordinary COFF loader and requires the same complete generated FuncInfo
graph. It also checks the separate ESI anchor, allocation/alignment and biased
state observations. Changed prologue bytes cannot retain the checked anchor.
Canonical metadata and source digests retain every coordinate. The realigned
state proof checks independent callback allocation, initialized spills, exact
parent-frame recovery, preserved callback calls and balanced runtime returns;
HighIR requires a closed callback CFG and current runtime-root/continuation
bindings before it moves the full body into a clause. Native lowering needs
the additional physical-layout and callback-stack proofs described below.
`NeverDWindowsRegistrationRealignedTests` emits a 64-byte-aligned frame with a
catch that calls a checked thiscall leaf on its parent local. Run
`check_windows_registration_realigned.py --test-binary <test-binary>
--runtime-libs <native-msvc-library-directory> --output <directory>` to compile
its driver, exercise four caller stack layouts in Wine,
and relift preferred/rebased PE32 images through LowIR and structured HighIR.
The continuation reads its local through the actual restored ESP. The driver
checks that value, caller bytes and FS chain restoration; the
wrong-result control must exit with 1 at both bases. Machine-code mutations,
independent roots, mixed ECX/EDX link publication, uninitialized or released
callback slots, missing call contracts and saved-entry-EBP borrows must reject.
The same PE verifies distinct MedIR runtime roots and HighIR/LLVM address
expressions at every ABI-compatible alignment residue, including PE32 address
wraparound. Missing, changed or incomplete no-return receipts cannot remove a
normal edge to create a continuation root. Runtime COPY definitions cannot
alias the ordinary incoming register in pointer and frame-slot proofs.
HighIR restores SavedESP before either an explicit continuation jump or its
folded body. Incomplete lifetime/callback proofs, changed return receipts and
unproved frame geometry cannot synthesize that writeback.
`WindowsRegistrationHighCallback` checks the distinct entry-register identity,
one capture at the callback entry, complete clause extraction, and HighC's
explicit runtime-input spelling. Root/state/return mutations reject the
projection. Splitting the callback into multiple ordinary blocks keeps all of
them in the region; an independent predecessor invalidates the whole region.
Call-identity and callee-contract mutations withdraw the exact argument ABI;
a changed ECX definition must remain the current argument instead of reverting
to the historical frame address. The runner also invokes the public `neverd`
CLI (the test binary's sibling, or `--neverd`) in C and C++ modes at both bases.
It checks the complete body and single object argument, and syntax-checks the
explicit C view with Clang. C++ output remains structured analysis pseudocode.
Replay requires both output hashes and the C syntax-check evidence.
Capture and replay require all three emission/identity checks and all three
analysis checks at each base, with no failures or skips.
`replay_windows_registration_realigned.py` authenticates the captured file
matrix and runs those identical four images on native Windows in the EH CI job.
The first Windows fixture job captures the selected x86 MSVC release link
libraries with `check_windows_registration_cxx.py --capture-runtime-libraries`.
The cross-linker verifies their manifest and hashes before linking the probe;
the capture records that library provenance alongside the generated object.
Use these native libraries for CRT RTTI definitions: a Wine-only import stub
can admit exports that the native CRT does not provide.
This is generated-frame analysis/runtime evidence, not source reconstruction.
`check_windows_registration_realigned_rewrite.py` separately emits aligned
value/reference, unbound value/reference and catch-all parents. Independent
assembly fixtures use the canonical direct MSVC EBP frame for the unbound forms.
LLVM fixed-frame parents cover all five forms, unsigned catch-all and a larger
allocation requiring an imm32 stack adjustment. Their additional proof checks
exact prologue fields, paired chain registers, displaced runtime roots, callback
EBP restoration and saved-register separation.
Catch-all also executes with an unsigned throw. Four additional direct-frame
profiles cover value/reference/catch-all with parent and catch argument writes,
and catch-all with read-only arguments. Their assembly caller checks both
physical argument words after return, with four signed input pairs and stack
layouts. The runner lifts real PE32
instructions and reconstructs
through the public patcher and both CLI modes. Pass `--test-binary`,
`--patch-binary`, `--runtime-libs` and `--output`. Every source and control
requires source reconstruction; LLVM fixed and aligned profiles additionally
require both callback HighIR checks. Displaced fixed frames also require the source-coordinate
and prologue mutation checks. Runtime probes
use four caller stack layouts to check the catch value, reference effect,
caller PC and restored FS chain.
All forty-two source/control profiles and four installation routes execute at two
forced bases (336 executions). CLI bytes must equal the checked public output;
the throw caller must lie in that output's recovered generated owner.
Incoming-frame tests also verify transactional rollback and reject unobserved
entry words, register ABI attributes, missing/changed access receipts, wrong
physical offsets, frame-depth changes, hidden slot writes, unindexed caller
memory operations and private-frame pointer leaks. Replay requires the source
read/write counts and both executed entry/rollback checks.
Twelve frame/stack edits plus eight continuation edits reject independently,
including valid control receipts with uninitialized scratch reads, accesses
after catch return and callback pointers outside the SavedESP bridge.
Unbound catches additionally reject fabricated homes, contradictory null-object
metadata, changed RTTI/adjectives, uninitialized parent reads and literal-field
relocations (including overlaps and unknown widths). Replay binds both fixture
emitters and requires each profile's exact reconstruction/HighIR test count.
The separate layout tests cover all supported alignments, source residues,
signed displacement bounds and under-aligned allocations.
`replay_windows_registration_realigned_rewrite.py` authenticates the source,
objects, IR, checked installation receipts, executed tests and exact PE matrix
before native Windows executes those same 336 files. It does not relink them.
Real MSVC directory-size64/declared-size192 load-configs must remain supported
with complete section bounds.
Final generic PE validation also checks valid and invalid CF/EH continuation
tables after the 64-byte compatibility directory and rejects a declared
load-config extent that leaves its image or raw backing.
Checked callee extents exclude unreachable padding and include the exact throw import thunk. Interior callee/thunk patches
reject, while a separately compiled unrelated replacement may coexist with the
C++ parent. The same fixture exercises the public COFF patcher and a preserved
callee renamed to an actual import, requiring the exact checked generated
section, SafeSEH and relocation closure and the committed original-entry receipt.
Set `NEVERD_REGISTRATION_OUTPUT_CXX_PE32` to save the manual transaction's EXE;
`NEVERD_REGISTRATION_OUTPUT_CXX_PRODUCT_PE32` and
`NEVERD_REGISTRATION_OUTPUT_CXX_COLLISION_PE32` save the public variants.
Structural success is not runtime evidence.

`check_windows_registration_multiple_catch.py` uses the same test binary and
captured CRT libraries for ordered value/reference/catch-all clauses. Its fixed
and aligned compiler parents each have a wrong-result control. The 32-image
matrix checks three throws with four caller stack layouts per execution,
including both CLI patch modes and forced relocation. The source test also
rejects reordered pads, cross-clause homes/stacks/continuations, malformed
emitted tables and incomplete HighIR scope proofs. Public C/C++ output must
retain all callback bodies and resume labels; C receives a syntax check.
`replay_windows_registration_multiple_catch.py` validates exact source/object,
IR, installation, decompilation and PE identities before replaying the same
files on Windows. Its Python admission suite rejects incomplete, stale or
substituted evidence.

`check_windows_registration_entry.py` uses the native registration test binary
and captured CRT libraries for cdecl, stdcall, thiscall and fastcall parents at
O0/O1. Word and narrow integer profiles include high unsigned-byte bits and
negative signed shorts, so sign and zero extension are observable. Four changing
arguments, four caller stack layouts and three catch paths plus a normal return
exercise register/stack ordering, caller-visible results,
ESP balance and FS-chain restoration. Its 256 executions cover source and
wrong-result controls, the checked installation, both CLI modes and two bases.
The C++ proof rejects altered source RET boundaries, incomplete continuations,
changed calling conventions and missing or misplaced fastcall `inreg` attributes;
it also decodes the generated parent RET cleanup count. Both C and C++ output
must retain two tries, three callback bodies and their exact resume labels and
pass syntax checks. `replay_windows_registration_entry.py` authenticates every
source, proof, receipt, object, IR, output and executable before repeating the
same 256 images with the Windows CRT. Python tests reject missing cases, skipped
proofs, stale identities and substitutions of the ABI, runtime or rebased image.

`check_windows_registration_relift.py` consumes the authenticated entry capture
and treats each first-generation product as fresh PE32 input. Its native test
reproves the frame, callback states, incoming ABI and LLVM privacy contract before
a second rewrite; both CLI modes must reproduce the API transaction byte for byte.
The 32 cases produce 256 executions across four routes and two image bases, with
16 calls each. Caller PCs must move into the second generated owner, and ESP,
FS:[0], argument writes and results must still match. The independent
`replay_windows_registration_relift.py` validates both complete capture identities,
source/IR/receipt hashes, both trampolines, native CRT and exact rebased bytes
before replaying those 256 images on Wine or Windows. Mutation tests reject missing
cases, changed identities, stale proofs, wrong ABI and forged caller ownership.
The re-lift proof also validates every LowIR instruction boundary. A focused
private-throw regression places a dead branch inside a runtime callback's
instruction, checks that authenticated nonreturning calls cut that path, and
retains the independent callback root. Unknown imports and returning callees
must preserve their ordinary fallthrough.
`NeverDMedLLVMDeadSlotsTests` checks transitive unused PHI scratch, live return
values, escaping pointers, volatile reads, atomic/volatile stores and source
observations. The narrow O1 entry re-lift also catches an unused ECX PHI that
previously became an uninitialized native frame read on catch return. The
source-frame initialization validator must still reject that old generated code.
`RegistrationFrameStores` and `RegistrationFrameTaint` test bounded LLVM spill
proofs, joins, backedges, partial writes and exhausted budgets. Loader tests keep
independent SafeSEH/code references and real function boundaries while excluding
padding bytes inside proved instructions. `COFFTrampolineRegion` tests unique
raw/virtual entry storage and instruction-mode widths on x86, x64, Thumb and ARM64.

`check_windows_registration_nested_try.py` compiles independent Clang `-O0`
and `-O1` parents and links the captured CRT libraries. Inner reference catches
and outer value/catch-all clauses exercise all three continuations under four
caller stack layouts, including a new throw inside the inner catch. The
288-image source/control matrix covers ordinary searches, secondary throws,
true helper and direct rethrows, and a try with or without local cleanups inside
a live reference catch through
both CLI patch modes and forced rebasing.
The C++ test rejects changed prologue saves,
personality argument reads, search edges, handler order, continuation ownership,
and emitted try/unwind state rows. Secondary-search mutations also reject
searching the exited inner try or losing the active catch token. HighIR must
contain every try and callback body; C and C++ output receive syntax checks.
The catch-internal profile has three tries and four catches. Its inner catch
updates the still-live outer reference, then resumes the suspended outer stack.
`WindowsRegistrationCatchContext` rejects changed catch ancestry, missing
suspended owners, and verifier-clean edits to resume values, source identities,
offsets and required memory properties. State tests separately distinguish
outer/inner object sizes, ended lifetimes and unbalanced private stacks.
The cleanup variant independently records two destructors in reverse construction
order (`53`). Clang O0 uses two unwind actions; O1 combines two calls into one
action. The wrong-order control expects `35` and must fail while retaining the
actual `53` trace. Each caller layout checks all three choices and resets the
trace. Thirteen verifier-clean edits reject missing, reordered, redirected or
misattributed calls, changed optimizer promises, skipped actions and forged
callback chunks. ABI tests bind every relay instruction and reject changed
frame adjustments, calls, returns, fixups and work exhaustion. State tests check
every borrow in a combined action and partial writes overlapping live references.
Replay binds the cleanup proof and shared receipt writer to their current digests.
`windows_registration_runtime.py` captures the selected MSVC
x86 redistributable DLL alongside the link libraries. Each image runs with
that exact app-local runtime, with a native-only Wine override, and replay
authenticates its provider and digest. A missing or changed runtime fails;
Wine's built-in catch-guard behavior cannot silently replace the Windows CRT.
Rethrow cases modify an unsigned object in the inner reference catch and require
the outer reference catch to observe that same modified value. ABI tests reject
partially null arguments, borrowed or unknown pointers and changed runtime
imports. State tests require a live catch and reject invented new-object fields,
missing initialization and callback arguments that overlap the runtime return PC.
Direct-call tests change each current MedIR argument and verifier-clean LLVM
arguments, parameter attributes, calling convention, target and noreturn
properties. HighC requires
both null runtime arguments before rendering a bare `throw;`.
Direct scalar profiles additionally throw int, unsigned and float through the
same runtime entry, including a fresh throw from a catch and a mixed typed-throw /
rethrow function. A separate returning marker retains an actual generated caller
PC witness. Source-state tests reject missing/partial initialization, pointers in
scalar bytes, changed table arguments, duplicate candidates, oversized object
reads and callback return-PC overlap. The native proof rechecks each actual
object argument against its original parent or callback coordinate, including
byte initialization and the exact relocated ThrowInfo address.
Callback-boundary mutations distinguish decoded straight-line instructions from
stated entries, exports, returns, traps and jumps. A padding-byte guess inside an
instruction cannot truncate a table-owned callback, while a real terminating
instruction preserves the next function boundary.
`RegistrationCxxUnwind` tests nested, disjoint, three-level and cleanup search
graphs. `replay_windows_registration_nested_try.py` authenticates the source,
rejection tests, compiler objects, IR, decompilation and installed PE bytes
before executing the identical images on Windows. Its Python admission tests
reject missing routes, stale proofs, altered images and lost nested regions.


The focused Windows EH workflow first builds and executes genuine MSVC x86
value/reference source fixtures on Windows, then transfers those exact inputs
to Linux. With the pinned compiler's handler receipts, Linux runs the complete
manual and public transactions and executes original/reconstructed images at preferred
and forced bases under Wine. The serialized compiler contract binds the parent
code range, private handler, nine pointer fields and source/final image hashes.
The final Windows job executes the same hashed images and reparses their code
owners, SafeSEH closure and pointer relocation values; previous runner success
flags cannot substitute for the actual runtime observations.

```bash
python scripts/check_windows_registration_cxx_rewrite.py \
  --input-root /absolute/path/to/native-msvc-inputs \
  --test-binary build/bin/NeverDWindowsRegistrationNativeTests \
  --patch-binary build/bin/neverd \
  --output build/evidence/source-cxx --wine-prefix /absolute/path/to/wine32
```

On Windows, replay with `scripts/replay_windows_registration_cxx.py
--evidence-root build/evidence/source-cxx --output build/native-cxx-replay.json`.
Both profiles require `value=7`, cleanup `trace=213`, four iterations, restored
FS chain, and `caught=7` by value or `caught=18` by reference. The observed caller PC
must lie in the indexed generated parent rather than an original helper or
another part of the generated section. Schema3 requires nine routes at both
bases: original, manual, public COFF patcher, called-helper/import-name collision,
CLI section and CLI inplace, followed by a second public transaction and its two
CLI modes. The second generation reproves generated ESI cleanup relays, their
saved entry EBP, two ordered destructor calls and complete callback state. It
binds both trampoline destinations, the full callback owner and the new SafeSEH
handler. Each CLI route must reproduce its generation's complete transaction
byte for byte. The source and re-lift test digests, both LLVM inputs and the
captured Microsoft x86 CRT are bound to replay. Both ordinary and SavedESP
writeback profiles require all 18 executions per value/reference case.
Schema2 replay remains first-generation public/CLI evidence; schema1 remains
manual-only. Neither older format establishes cleanup re-lifting.

The shared re-lift runner also accepts `--profile catch-cleanup` and a complete,
authenticated nested-try capture. Clang O0/O1 plus wrong-result controls require
32 executions and 32 independent replays, including ordered local destruction
inside an active reference catch. O0 preserves pointer bits in a register's high
bytes while replacing its low byte with a scalar condition. The independent
LLVM frame proof tracks demanded bits through masks and unaliased spills;
truncating retained pointer bits, overlapping writes, aliases and poison flags
remain rejected. `RegistrationFrameBitsTests.cpp` covers those boundaries.

`RegistrationRealignedCleanupABITests.cpp` checks short and wide displacements,
ordered multiple calls, restored EBP, preserved leaf ESI, immutable code,
relocations, coordinate overflow and work limits. Cached relay proofs are tested
against matching and mismatching parents in both lookup orders. The independent
caller ABI check and LLVM emitter also bind each relay to its current parent.
Plain C table-owned callbacks retain checked native labels; normal-flow execution
must skip those bodies on x64 and AArch64.

The runtime runner builds actual SEH3, no-GS EH4 and initialized-GS EH4 source
images, including explicit source exit checks, lifts their protected
function and executes the original, manual installer, public COFF patcher,
import-name collision and both CLI modes. Every image runs at its preferred
base and at a forced relocated base. Assertions cover return values, ordered
filter/finally traces, actual generated caller PCs, four repeated calls,
restored FS:[0], SafeSEH and a newly installed Guard CF table-pointer relocation.
The cdecl cases additionally read and write the actual caller-owned parameter
slot through exceptional callbacks; synthetic frame initialization alone cannot
satisfy those observations.
State tests cover narrow C++ state writes, nonzero high bytes, path unions,
sentinels, unknown prior states and scanner/lifter width disagreement. The
canonical metadata and source semantic receipts retain the exact store width.
Continuation tests require a decoded plain catch return, an exact code pointer
and a reaching saved stack value. Nested catches must restore the enclosing
catch context and its search minimum. CFG tests decode a previously missing
continuation and reject instruction-interior, non-code, callee-cleanup return
and independently owned function targets. A candidate with no decoded target
retains diagnostic evidence but cannot grant native authority.
State tests mutate the GS decode expression, target, width and instruction
identity. Native input tests remove, duplicate and alter the indexed source
check event and require the public patcher to reject it. Strict generated-only
cookie probes vary caller stack alignment and corrupt EH and GS slots
independently; they remain separate evidence from source reconstruction.
Original-corpus and callback-only runs remain distinct evidence. Missing Wine,
compiler receipts or skipped reconstruction cannot count as a pass.

The `ci.yml` `windows_eh_only` profile runs the x86 original/callback probes
and ARM32 cross-target PE checks. Supplying `windows_eh_llvm_artifact_run` adds
all source reconstruction runs using a successful compiler build whose commit,
archive checksum and BUILDINFO match the checked-out LLVM submodule. It also
executes strict EH4 cookie probes with and without GS, requiring rejection after
cookie corruption. A dependent Windows job replays the same hashed EXEs with
the native CRT. Wine callback execution and native Windows replay are recorded
as separate evidence. ARM32
cross-target codegen and PE validation do not establish Windows ARM32 runtime
execution.

The native Windows job also builds `registration_cxx_runtime.cpp` with MSVC
and its actual runtime libraries. Value and reference catches must preserve
the caught object, destructor trace `213`, repeated calls and FS:[0] at both
load bases. When the compiler provides checked catch subfields, the frame unit
suite emits `NEVERD_REGISTRATION_CXX_RUNTIME_OBJECT`. The native runner links
that exact object with the same real MSVC RTTI, throw helper and destructor to
check value/reference catches in ordinary and 64-byte-aligned generated frames.
Caller stack padding varies on repeated calls, and the recorded throw caller
must belong to the selected parent according to its export and linker map.
`check_windows_registration_cxx.py` labels original-program and generated-frame
ABI evidence separately; neither establishes source C++ reconstruction.

The nested PE32 runner also accepts `--profile objects`. It builds 8-byte and
12-byte trivial exception records with Clang O0/O1, private/direct throws,
reference updates and rethrows into value catches. The 16 source/control cases
cover 128 images across both patch modes and forced bases. The extra object
proof and exact fixture type declarations are hashed into capture admission;
C++ syntax checks use those declarations rather than inventing record layouts.
`replay_windows_registration_nested_try.py` selects the authenticated profile
and replays identical files with the Microsoft CRT. `RegistrationPHICopy` checks
emission ownership and independently rejects nonprivate scalar copy edges.

The object profile also feeds `check_windows_registration_relift.py --profile
objects`. All sixteen original/control cases are loaded again from their first
installed PE32 bytes, reconstructed a second time, installed through both CLI
modes, and executed at both bases (128 runs). Native Windows replays the same
files with `replay_windows_registration_relift.py`; source proofs, both image
generations, Microsoft CRT identity, field effects and throw caller ranges are
mandatory evidence. Second-generation direct throws run the normal call ABI
recovery pass before native LLVM emission.

The `destructors` profile uses the same sixteen O0/O1 source/control cases with
observable exception-object and catch-copy destructors. Each execution checks
twelve destructions and a rolling trace of their order and object contents,
alongside catch results, reference effects, caller ownership and FS:[0]. Negative
controls change only the expected destruction count. The shared source driver,
record declarations and independent destructor proof are hashed into admission.
Both `check_windows_registration_nested_try.py --profile destructors` and
`check_windows_registration_relift.py --profile destructors` produce 128 images;
their replay tools validate the identical files with Wine or native Windows.
`RegistrationThrowObject.*` rejects out-of-bounds effects, escapes, CRT caller
observations, mutable code, metadata writes, unchecked calls and exhausted
budgets. It also retains helper and destructor code in either address order.

The separate `destructor-graphs` profile keeps both observable destructor
operations behind two real out-of-line member calls. Its sixteen O0/O1 cases
and controls each exercise both installation routes and forced bases (128
images per generation); capture, replay, re-lift and replay retain the exact
source, declarations and transitive lifetime proof hashes. The existing leaf
destructor profile remains a control. `RegistrationObjectGraph.*` checks
same-object and subobject effects, conditional may-writes, private scalar
arguments, exact cleanup, child-result privacy, pointer escapes, metadata and
code writes, recursion, depth, distinct-node and work exhaustion, and changed
transitive code. The input-PE32 test requires complete runtime object and cleanup
state, both destructor identities, two actual child calls and closure of their
code ranges in the parent's current contracts.
The separate `destructor-results` profile consumes a returned object-field
calculation when updating the same independent lifetime trace. Its sixteen
O0/O1 cases and controls produce 128 images per generation, with identical-file
replay on Wine and native Windows.
`RegistrationScalarCallState.*` additionally checks parent and catch calls,
physical and instruction-local return values, unproved or wide results,
changed output identities, other volatile register escapes and separate object
initialization. Authentic source tests also flip the result proof and require
native emission to refuse the stale contract. O1 corpus inputs expose ordinary scalar calls
after destructor inlining and require complete source state before rewriting.
`RegistrationRuntimeMemberState.*` checks distinct runtime allocation identity,
subobject offsets, reordered CFGs, bounds, partial pointers, constructor
authority and expired catches. `RegistrationRuntimeMemberFrame.*` independently
checks the actual LLVM pointer and lifetime, rejecting parent-frame or constant
receivers, changed offsets, wide effects and invented initialization or stack
arguments. The `reference-members` profile consumes a real out-of-line member
call on a catch reference before normal completion or rethrow. Its sixteen
O0/O1 cases and controls produce 128 images per generation and retain source,
declaration, callback and receiver-proof hashes for Wine and native Windows
replay. Its input-PE32 test also rejects six changed source receiver contracts,
three missing, extra or constant receiver argument projections and three
post-emission LLVM pointer edits. O1 keeps a live EDX value beside ECX; the
checked member contract must exclude that scratch value from the call ABI.
Pure graph tests require an independent
scalar on every return, reject borrowed pointers and entry-EAX alternatives,
and retain the private scalar-argument proof when a child returns an argument.
`NeverDRegistrationCallResultTests` independently checks source result-use
proofs and LLVM deletion plans. SSA copies, extensions and PHIs may transport
an unused result; returns, branches, source stores, implicit call arguments,
preserved call inputs, ambiguous definitions and exhausted budgets reject.
LLVM plans remove only integer transports into exact emitter-owned store-only
slots, retaining the call and rejecting loads, escapes, metadata, volatile and
atomic stores. O1 destructor graph cases exercise the ordinary void member
calls exposed when the compiler inlines the outer destructor.
`SourceDialect.CxxUnknownExternalParametersKeepDefaultPromotions` checks the
frontend IR argument types for the C/C++ views on every host: a C declaration
with unknown parameters cannot become a zero-argument C++ prototype. Both views
execute against an independent C callee where the physical ABIs agree. Darwin
AArch64 executes the C view and checks the C++ view's promoted IR types, because
its fully variadic and fixed C ABIs differ. The provider uses the exact assembler
label on Mach-O. The source ellipsis does not grant a native variadic ABI contract.
The `member-arguments` profile adds a nonzero 32-bit stack argument to those
member calls and separately authenticates its source, type and scalar-word proof.
`RegistrationMemberCallee.*` keeps CRT callback and ordinary member policies
separate; `RegistrationMemberArgumentState.*` checks one through three words in
parent, value-catch and reference-catch contexts, including missing, partial,
pointer-bearing, unbalanced and retired storage.
`RegistrationRuntimeMemberFrame.RechecksDefinedScalarStackWords` and authentic
source mutants reject pointer, undef and poison arguments after LLVM emission.
Capture, replay and a second reconstruction cover both optimizations, negative
controls, both patch modes, rebasing and lifetime count/order/content.

`WindowsSEHFallback.*` checks complete native clause annotations, narrow state
stores, pure and side-effecting filters, refusal to discard an unbound handler
body, native continuations after an ordinary return, and irreducible C++ views.
`MedSEHHandlerEntry` checks table-owned x86
frame recovery after state-budget exhaustion, including independent ordinary
roles, predecessors, mismatched personalities, frame offsets and entry ranges.
The large original `xcpt4` corpus regression must keep its frame-reading filter
expression in the native annotation without claiming lexical protection.

`NeverDLiftTest.HostHalfFMAProviderMatchesIndependentIEEEOracle` checks the
test-only binary16 FMA provider against APFloat for 10,936 edge and randomized
triples at O0/O2. `AArch64_FP.HalfPrecisionArithmeticExecutesThroughHighC` uses
that provider when an older host library lacks `fmaf16`; its expected values
also come from APFloat. This fixture proves default nearest-even execution,
without granting a product floating-environment or other rounding-mode contract.

The `copies` source and relift profiles extend that matrix with nontrivial CRT
copy callbacks for both records. Each run checks four copies and twelve
destructions with separate count and object-content traces. Negative controls
change the expected copy count. The sixteen source/control cases produce 128
executions per capture or replay, including both patch modes and forced bases.
`RegistrationCopy.*` separately rejects source writes, bounds violations,
partial destination initialization on any return, pointer escapes, caller
storage observations and incorrect `RET 4` cleanup. Replay admission binds the
copy proof and source declarations and rejects a trivial-object or destructor
capture in their place. C/C++ syntax checks also cover records with no default
constructor; RTTI alone must not invent a constructor or remove initialization.

The `initializers` source and relift profiles keep ordinary scalar-argument
constructors out of line in the parent, its catch callback and private throw
helpers before entering the CRT. Twenty O0/O1 direct/private nested/rethrow and
direct-secondary source/control cases produce 160 images per capture or replay;
the complete two-generation sequence executes 640 images. The source
profile binds the initializer proof, the constructor-selection wrapper, both
shared drivers and the actual type declarations. It retains the independent
four-copy/twelve-destruction oracle for nested catches and rethrows. A new
object constructed from a catch's modified fields requires four copies and
sixteen destructions with a distinct content/order trace. Copy-only evidence
cannot replace ordinary-constructor evidence. `RegistrationInitializer.*` checks
callee stack cleanup, exact returned destination, private spills, all-path initialization,
source closure and the nearest pointer/argument/caller-storage refusals.
`RegistrationInitializerState.*` checks exact stack cleanup and return identity
without borrowing uninitialized arguments. `RegistrationInitializerFrame.*`
checks disjoint must-writes, exceptional-edge isolation and scalar argument
privacy. Direct-source mutations also challenge the installed constructor ABI,
object pointer and optimizer promises at every call occurrence, including a
constructor shared by parent and callback. `RegistrationCallbackInitializer.*`
checks callback argument privacy, stack cleanup and parent-object initialization
independently of block order. `RegistrationCallbackStackFlow.*` checks cross-block
SSA copies, loops, bypassed definitions and runtime resumes without treating the
parent's ordinary entry as the callback's dominator root.
Second-generation receipts retain disjoint callback ranges. Replay checks each
range and rejects gaps, overlap and chunks from the previous generation.
`RegistrationInstructionRange.*` distinguishes instruction operands containing padding
bytes from real entries after branches and calls, including loops, unreachable
bytes, conflicting branch targets and overlapping instruction decodes.
`RegistrationState.APossibleCalleeWriteCannotLaunderTheObjectPointer` checks
that both partial and full-width may-writes preserve possible frame-pointer
provenance until a definite replacement has been proved.

The separate `delegating` source and relift profiles retain both ordinary
constructor bodies and require a current-machine transitive construction proof.
Their twenty O0/O1 source/control cases cover private helpers, direct parent
calls, catch construction, rethrows and secondary exceptions, producing 160
executions per capture or replay and 640 across both generations. Native Windows
replays the identical files. Ordinary initializer evidence cannot replace the
constructor-graph proof or its source dependencies. `RegistrationInitializerGraph.*`
checks transitive image/code effects, independent subobjects sharing one callee,
subobject return identity, partial must-writes, scalar privacy, exact cleanup,
recursion and exhausted depth/work budgets. The source proof verifies actual
child calls on ordinary entry paths, independently of other address-taken roots.

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
With `NEVERD_SEMANTIC_HIGHC=1` in the environment, step 4 decompiles the object
to C on the HighC route instead and builds that C for the original target at
`-O2` with `-fno-strict-aliasing`; the run then checks the decompiled C itself
by execution. Each function gets its own section, and the link places the
tested function first, where emulation starts. A case whose C calls a routine
the freestanding image cannot link (a rounding instruction printed as
`nearbyint`), or does not define the tested function, is skipped with that
reason. Every round-trip test runs this way, so compare the failing names of
two such runs to judge a HighC or HighIR change. `NEVERD_SEMANTIC_LLVMC=1`
does the same with the LLVM-C route's C, the check for an LLVM-C printer
change.
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

The same job first executes its focused emulation and mobile contracts. The
`reuse` command retains only their passed results with matching inventory
identities, so the main profile does not run those cases twice. Failed, skipped
and incomplete cases remain in the main execution. A name registered by several
owners moves only when every selected owner has passed. The final audit combines
the retained JUnit cases with fresh results against the unchanged complete
profile, rejects overlaps and missing identities, and keeps the original exit
status. Reuse names, JUnit and input hashes accompany the outcome artifact.
This avoids repeated native work within the hosted job's time limit without
reducing its required coverage.

The original WHP file-transfer matrix contains eighteen separately bounded
five-second guests, so its complete CTest method has a 120-second allowance.
The original integer-formatting matrix contains four thirty-second guests and
has a 150-second CTest allowance. The policy matches only those two methods and
their four WHP contract/base variants; other driver methods retain their
existing limits. The created-regular-attributes, created-directory-filesystem
and attribute-name Darwin differentials use the same twenty-second guest
allowance as the other semantic batches, including complete filesystem record
copies. Their whole-method CTest allowances include host setup and teardown:
180 seconds for eight directory guests, 420 seconds for twenty attribute guests,
and 90 seconds for three attribute-name guests. The policy applies to these
three methods across their transports.
Instruction bounds, public defaults and all result assertions remain unchanged.

The four `SessionCAPIKernelInputs` checks require developer-supplied kernel/PDB
files. Their exact missing-input reasons are recorded as external-input skips;
they do not count as passed. Once their required environment variables are
configured, a skip fails the audit. Other Session checks remain mandatory.

The required execution policy follows existing CI ownership:

- Linux must execute all selected `NeverDSemanticTests` cases; macOS must
  execute all selected `NeverDPatchFullTests` cases.
- Every host must execute the mandatory safety, native example plugin,
  concolic, binary corpus, and failure-integrity suites. The integrity suites
  cover worker exception transport, SDK session state, exact native LLVM
  function identity and verified caching, the semantic fixture, and pipeline
  outcome publication.
  `NeverDImageAnalysisBoundaryTests` is also mandatory on all three hosts:
  removing, excluding, skipping or failing a discovered native-image proof
  case fails the CI evidence audit.
- Linux must also execute the SBF external oracle, upstream conformance, and
  Agave conformance suites because that leg installs their pinned dependencies.
- Every host must execute `NeverDSourceDialectTests` and
  `NeverDSourceAnchorTests`. Missing compilers, missing generated corpus inputs,
  skipped checks, or omitted suites fail the audit. The CTest evidence artifact
  includes the compiler versions, native ELF inputs, emitted C and CLI logs.
- Platform exceptions within these required suites include
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

`KvmHandoffPolicy` bounds each polling wait to 8 μs, uses blocking waits after two consecutive misses, and retries after 256 handoffs. Caller and worker adapt independently; the caller also observes the original deadline and stop token. Atomic readiness flags are only scheduling hints: the mutex still owns packets, callback lifetimes and cancellation acknowledgement. `NeverDKvmRunTests` checks bounded unproductive polling, recovery, changing peer latency and cancellation before packet reuse.

KVM x64/ARM64 uses `KvmRunControl` to prepare state, enter `KVM_RUN` and capture state on one private vCPU worker. Preparation runs once across `EINTR` retries; cancelled entry or failed capture cannot publish. `KvmAArch64Machine.cpp` performs translation maintenance and complete scalar/vector transfers on this worker under one step deadline. The caller publishes only after acknowledgement; ISA decoding, RAM transactions, OS policy and observers remain on the caller thread. Native ARM64 runtime evidence is still pending.

`ReusesCapturedStateAndInstallsHostChangesAcrossFaultsAndStops` checks continued execution and independent CPU stores after host changes to general registers, both edge XMM lanes, MXCSR and x87 control. Actual `FXSAVE64` bytes verify all physical 80-bit registers, TOP, tags, opcode and pointers after a stopped entry; repeated divide faults also invalidate reuse. These machine-boundary tests do not admit additional x87 instructions into checked profiles.

`NeverDKvmStateTransferTests` injects a failed register or XSAVE read after real KVM execution, then retries the unchanged input. Independent integer and packed-byte results prove that failed captures cannot reuse advanced native state. Only this executable wraps `ioctl`; unavailable native hosts skip explicitly.

`NeverDKvmStateTransferTests` also exercises absent, individually available and combined `KVM_CAP_SYNC_REGS` sets plus failed capability queries on real KVM. `SynchronizedCapturesRemoveOnlySupportedReadIoctls` counts actual reads and checks complete CPU state after consecutive steps. `CancelledWarmEntryRequiresFreshSpecialStateOnRetry` requires a fresh special-register read after cancellation. Capture failures, integer/SIMD retries, speculative RAM rollback and exception priority run through the same capability matrix; unavailable native coverage is an explicit skip.

Checked ARM64 has one complete state boundary. `Registers.def` defines 39 scalar fields and 32 128-bit vectors; `captureAArch64State` stages every read, applies declared widths and NZCV normalization, then publishes once. Unicorn, KVM, WHP and HVF transfer the same inventory, including TPIDR_EL0, TPIDRRO_EL0, TPIDR_EL1, FPCR and FPSR. Native adapters enable FP/SIMD through CPACR_EL1. Any scalar/vector read failure or cancelled entry preserves all caller state.

ARM64 KVM/WHP/HVF startup executes the private `AArch64MachineProbe.def` program: NOP, FP32 addition rounded toward positive infinity, a two-lane SIMD addition, A/B return-address signing/authentication with the keys disabled, and all four BTI forms on unguarded pages. Each step compares all 39 scalar fields and 32 vectors, including TLS, NZCV, cleared upper destination bits and retained/cumulative FPCR/FPSR state. The probe uses supervisor monitor storage and one overall deadline. The probes establish bounded initialization only. Linux ARM64 KVM and Windows ARM64 WHP workload validation remains pending; native macOS results are recorded in the [HVF guide](macos-hvf.md).

The probe also executes `MRS CTR_EL0` twice, `DC CVAU`, `DSB ISH`, `IC IVAU` and `ISB`, checking stable cache geometry and complete state. Checked EL0/EL1 execution admits the original words, all named baseline DSB options and only ISB SY. CTR comes from the selected virtual CPU and may differ between transports. Cache targets must identify readable ordinary RAM with the current privilege, including unaligned addresses and aliases; other targets fail as unsupported. Maintenance produces no data read/write observer events. The projection keeps instruction execution coherent; it does not model private cache contents or parallel hardware SMP. `NeverDAArch64CacheTests` checks full state, read-only page ends, rejected forms, stops, contexts, budgets and guest code updates through cross-page RW/RX aliases. Unavailable KVM/WHP hosts remain explicit skips.

Native x64 KVM/WHP/HVF initialization executes `X64MachineProbe.def` in private supervisor pages. One deadline covers NOP, rounded FP32 addition, two-lane SIMD addition, FS/GS loads and CS/SS/CR8 reads; every step compares the complete scalar, XMM, physical x87 and control state. x64 and ARM64 probes require the exclusive physical-memory execution lease. `MemoryProjection` owns cache identity (ISA, address space, mapping generation, privilege and monitor variant) and committed root history per ISA. Builders invalidate before rewriting private bytes; failed replacement cannot reuse partially written tables, and callers cannot supply stale roots. The probes establish bounded initialization only. Linux ARM64 KVM and Windows ARM64 WHP workload validation remains pending; native macOS results are recorded in the [HVF guide](macos-hvf.md).

`NeverDAArch64PAuthTests` runs both checked privilege profiles on available
Unicorn/KVM/WHP/HVF transports. Independent encodings cover all 128 HINT selectors,
the twelve admitted disabled-key forms, four unguarded BTI forms,
zero and high-bit return addresses, and original `XPACLRI` with unsigned low
48-bit return addresses. The latter are fixed points of PAC stripping in the
checked translation regime; authentication being disabled alone does not make
stripping a NOP. `XPACLRI` with higher address bits, non-HINT authentication and
stripping, authenticated branches and key/control register access remain
unsupported. Every attempt checks the complete scalar/vector inventory, PC and
unchanged code/data bytes. Run it together with `NeverDAArch64StateTests`,
`NeverDAArch64FPTests` and the checked CPU/session regressions after changing
admission or startup probes. Native platform cells without hardware remain
explicit skips; no active-authentication or real Android process claim follows.

`NeverDAArch64BTITests` executes independently encoded direct calls, indirect
calls/jumps and returns to all four original BTI words on unguarded pages.
EL0/EL1 checks preserve the branch trace, link register, following memory effects,
observer stops, saved contexts and shared instruction budgets. The exhaustive
HINT-selector test checks complete scalar/vector state and neighboring rejection;
the startup probe also executes all four BTI words. Projection tests walk the
actual leaves after permission, privilege and alias changes to verify GP stays
clear. Run the BTI, PAuth, state, FP, memory, projection, checked CPU and session
targets together. These checks do not establish guarded-page enforcement or
native Android equivalence; unavailable hardware cells remain explicit skips.

`AArch64ExclusiveTests.cpp` checks scalar/pair widths, acquire/release forms, register overlaps, aliases, alignment and permission faults, snapshots, observer cancellation/failure and competing CPUs. `RAMReservationTests.cpp` checks identical-value writes, ABA, allocation reuse, rollback and KVM/Unicorn string and `ENTER` interference. Original ARM64 Windows process fixtures execute exclusive loops. `scripts/check_aarch64_exclusives.py` runs the original instructions and captures alignment exception records on Windows ARM64 CI. Native instruction observations do not establish ARM64 KVM/WHP backend execution; unavailable profiles remain explicit skips. The same exclusive cases cover software Unicorn, cross-contract interference, identical-value and ABA writes, same-run executable aliases, and `DC ZVA` writes with observer cancellation. `windows-alignment-oracle.yml` also runs the ARM64 probe: 1,320 observations cover every misaligned offset, four load/store sequences, and writable, read-only, inaccessible and split-page memory. The probe retains full-width register seeds and records any committed fault prefix. `WindowsExclusiveProcessTests.cpp` checks 1,320 original Windows ARM64 observations against the native digests in `WindowsExclusiveNative.def`, retaining register values, exception metadata and RAM effects while normalizing only code/data placement.

`AArch64AtomicTests.cpp` covers 168 independently assembled LSE encodings, register aliases, signed comparisons, permissions, cancellation, physical reservations and NZCV transfers. `scripts/check_aarch64_atomics.py` collects 1,100 original Windows ARM64 records with complete operand results, exception context and RAM footprints; parser tests reject missing or inconsistent evidence. Native KVM/WHP execution remains a separate validation requirement. The checked process regression is `WindowsAtomicProcessTests.cpp`, with complete record digests in `WindowsAtomicResults.def`.

The shared XSAVE decoder distinguishes standard and compacted initial SSE state. With XSTATE_BV[1] clear, both forms initialize XMM registers; standard format still reads and validates MXCSR, while compacted format initializes MXCSR. `X64XsaveCases.def` supplies independent packet layouts and original host XRSTOR programs. `X64XsaveTests.cpp` checks rejected-state atomicity and compares both formats with actual host execution, preserving the caller’s FP/SSE state. The host oracle skips explicitly when the architecture or required instruction feature is unavailable.

`X64FPState.def` declares compacted AVX, AVX-512, CET_U/CET_S and AMX transport layouts, including 64-byte component alignment. Present extension payloads must be architectural zero init state; absent payloads and alignment padding do not define state. Layout bits determine offsets, and unknown layouts, non-initial payloads or incorrect lengths fail before publication. `CompactedOffsetsFollowLayoutRatherThanPresentBits`, `WideLayoutIgnoresAbsentComponentsAndAlignmentPadding`, `InitialCETComponentsDoNotHideFPState` and `InitialWideComponentsDoNotHideFPState` cover 872-byte and 10752-byte WHP packets. This transport support does not admit those extension instructions.

`WhpXsaveRegisters.def` supplements complete XSAVE packets with named x87/SSE control registers. Last opcode and instruction/data pointers are written explicitly and captured from the host; zero packet slots may be supplemented, while conflicting nonzero metadata or inconsistent shared controls fail before publication. `NamedMetadataRestoresOmittedPacketFields` checks the omitted-field case without dropping any FP payload.

Native `FOP/FIP/FDP` follow the host’s x87 save/restore rules. AMD may clear these fields without a pending unmasked exception; snapshots retain observed values. `X64MachineProbe.def` and exact NOP/context tests seed a coherent pending exception so every field remains valid and is compared without masking. The host-process FXRSTOR64/FXSAVE64 oracle checks both states; backends never substitute input metadata for host results.

`NativeGuestRAMDistinguishesEntryFromCaptureLoss` compares complete FP/SSE state saved by actual guest FXSAVE64 with the host XSAVE capture. Both API variants test direct installation and in-guest FXRSTOR64, with the default pointer-save feature and the explicitly selected host-supported setting. This isolates entry, guest execution and capture without repairing values in the test; a mismatch remains a failure. The boundary matrix also covers a pending unmasked x87 exception and records a host-process FXRSTOR64/FXSAVE64 reference plus processor vendor, to distinguish conditional pointer-save semantics from WHP transport behavior.

The shared `encodeX64XsaveState` / `decodeX64XsaveState` codec owns standard/compacted FP/SSE packets, physical TOP rotation, absent-component init state and atomic validation. WHP uses complete XSAVE APIs, preferring `WHvGetVirtualProcessorState` / `WHvSetVirtualProcessorState` with the older XSAVE APIs as a compatibility path. Legacy individual x87 registers cannot replace complete packets. Non-initial extended components, malformed headers, invalid controls and truncated captures fail explicitly. WHP mapping failures retain HRESULT, GPA and size for diagnosis.

`CheckedX64Instructions.def` admits unsigned `MUL` at 8/16/32/64 bits and `CBW/CWDE/CDQE/CWD/CDQ/CQO` through the existing processor transport. `NeverDX64IntegerTests` uses independent `X64IntegerCases.def` encodings and expected values at both privilege levels: partial-register preservation, 32-bit zero extension, both product halves, defined CF/OF results and unchanged flags for sign extension. Ordinary-RAM multiplication retains whole-span permission checks and read observers; a fault or observer stop preserves implicit output registers and PC. Device operands remain unsupported. These cases also run on checked Unicorn; unavailable native transports skip explicitly.

The `X64Integer.BitScan*` regressions use independent BSF/BSR encodings and scalar expected indexes. They cover both directions at 16/32/64 bits, zero and nonzero sources, upper-register preservation, 32-bit zero extension, aliased extended registers, defined/preserved flags, register and cross-page RAM reads, observer stops, unmapped/protected fragments, invalid LOCK prefixes and rejected device operands. Zero-source destinations and undefined flags are deliberately not assigned portable expectations. Seven cases run at both privilege levels on each checked transport; the native KVM/WHP gate requires all fourteen matching-host outcomes. Genuine unavailable transports remain explicit skips.

`X64BitInstructions.def` admits register and ordinary-RAM `BT/BTS/BTR/BTC` at 16/32/64 bits. A register bit index is signed at the operand width and selects a complete word; an immediate stays within the base word. Address-size wrapping occurs before FS/GS base addition. The processor supplies CF and written values; `RAMTransaction` keeps the result private until observers accept it. Whole-span permission checks cover separate page allocations and aliases. Stops, callback failures and denied pages preserve the original CPU and RAM. LOCK is limited to naturally aligned modifying memory forms; MMIO and parallel hardware SMP remain unsupported. `X64BitStringTests.cpp` compares independent encodings with actual x64 host execution and checks negative indices, width truncation, cross-page accesses, cancellation and invalid LOCK forms. See the [Intel instruction reference](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).

`X64StringInstructions.def` owns ordinary-RAM `MOVS/STOS/LODS` at 8/16/32/64 bits; `CLD/STD` controls direction without changing other flags. Each REP element validates the entire operand before observations and commits at one restart boundary. Earlier completed elements survive a later fault; cancellation or observer failure leaves the current element untouched. FS/GS applies only to the source, after address-size truncation. AL/AX loads preserve upper bits and EAX loads zero-extend. Zero-count address-size-32 REP requires zero upper count bits and, for MOVS/STOS, zero upper participating address bits: real CPU implementations differ otherwise. REPNE on MOVS/STOS/LODS and STOS/LODS device operands remain unsupported. `X64StringTransferTests.cpp` uses independent host instructions for widths, direction, overlap and zero counts, with separate checks for permissions, aliases, wraparound, faults and resumption. The original WDK resource driver executes all four STOS/LODS widths through `driver_resource_strings.def`.

`X64StringInstructions.def` also owns ordinary-RAM `CMPS/SCAS` at 8/16/32/64 bits with `REPE/REPNE`. Every element validates both complete read operands before observers, updates all six arithmetic flags, and stops on the first matching termination condition. A data fault restores the flags from entry to this uninterrupted REP while retaining completed pointer/count changes; a public resume starts from the published CPU state. Stops and observer exceptions leave the current element untouched. Early termination never reads the next element. FS/GS affects only the CMPS source; SCAS leaves the accumulator and unused source register unchanged. Device operands and ambiguous inactive 32-bit upper halves remain excluded. `X64StringComparisonTests.cpp` compares independent host instructions, flags, direction, aliases, wrapping, permissions and recovery; its Linux x64 signal oracle checks actual fault-time registers. The original WDK resource driver executes both conditional-repeat forms at all four widths through `driver_resource_strings.def`. See the [Intel instruction reference](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html). The Linux native oracle checks faults before and after the first element. It distinguishes Intel entry-flag restoration from the last-comparison flags observed on AMD EPYC 7763 under Hyper-V ([native observations](https://github.com/NeverSight/NeverD/actions/runs/37202522130)); unknown CPU vendors fail explicitly. Checked guests keep entry-flag restoration on every backend.

Live WHP CPUs share one native partition. Its final close and replacement creation use the same registry lock. `WhpResourceCache.h` reuses cooperative VP 0; switching its logical owner first retires that VP and its mappings. Parallel bindings retain separate VPs and private GPA windows. Every register transfer, XSAVE operation and cancellation targets its own VP. x64 preserves the host’s default XSAVE feature set and validates the effective partition with `WHvGetPartitionProperty`. Cooperative scheduling remains the default.

`NeverDX64FPTests` checks all 79 startup corruption positions and executes independently assembled `X64ProbeCases.def` instructions on native transports with one deadline and unchanged guest RAM. `NeverDProjectionCacheTests` checks caller changes, ISA order, root history, privilege/monitor variants, mapping generations, address-space identity and failed replacement. `NeverDRunControlTests` includes `WhpXsaveTests.cpp` for modern/legacy API packets, every TOP, size bounds and unchanged state on failure; these in-memory protocol tests do not establish native WHP evidence. Unavailable native transports skip explicitly.

XSAVE validation diagnostics distinguish size queries, local packet preparation and captured-packet decoding. They retain the API name, returned byte count, capacity and bounded header/control metadata from `WhpHostFailureCases.def` expectations; guest register payloads are not printed. `InvalidInputReportsPreparationWithoutHostMutation` also checks that rejected input never calls the host or changes its packet. The shared ISA codec remains the sole validation authority.

WHP host failures during capability queries, partition/virtual-CPU setup, register/XSAVE transfer and execution preserve the HRESULT and failing API name declared in `WhpProtocol.def`; capability-query failures retain the typed unavailable result. `WhpHostFailureCases.def` provides independent expectations for host failure concurrent with cancellation and modern/legacy XSAVE query, install and capture failures. The focused Windows dispatch requires 210 native passes: 16 mapping cases, two startup cases, ten FP/context cases, seven shared-CPU cases, eight integer cases and both `NativeInstallRetainsFPStateBeforeAnyGuestExecution` API variants. The latter compare complete FP/SSE and independently queried metadata before running guest code. Missing registrations, skips, disabled tests and not-run outcomes fail the native evidence audit. The additional 26 checks cover all `X64BitStringTests.cpp` cases at both privilege levels. Windows PE64 requires 67 WHP process cases and eleven independent native Windows oracle cases.

`NeverDMemoryLifecycleTests` is built independently of Unicorn, including native-only configurations. Its software-specific projection/device cases skip explicitly when Unicorn is disabled; matching-host shared-CPU tests remain registered. `WhpMemoryTests.cpp` isolates the native memory API with 16 cases in `WhpMemoryCases.def`: page/projection-sized backing, shared/independent allocations, untouched/resident bytes and a first virtual processor present/absent. Each case keeps two logical owners alive, switches their mapped partition repeatedly, retires the inactive owner and verifies that the surviving mapping stays usable without recreation. Genuine mapping failures retain HRESULT and fail the test; this is memory-API evidence, not instruction-execution proof.

`X64MachineProbe.def` names the failing startup instruction and every differing scalar, TLS, privilege, x87 control, physical FP lane and XMM word, retaining expected and observed values. `DiagnosticIdentifiesStepFieldAndBothValues` checks independent golden messages. State comparison remains exact; these diagnostics distinguish transport loss from instruction execution without certifying a failed native probe.

`WhpResourceTests.cpp` covers cache reuse, retirement before replacement, failure recovery and deadline/stop races. `LogicalCPUSwitchingRestoresPhysicalFPAndTLS` alternates two live machines in both privilege modes, checks independent physical x87/XMM and FS/GS state, then resumes the survivor after peer destruction. Windows CI requires both WHP privilege cases.

`NEVERD_ENABLE_SEMANTIC_TESTS` defaults to `ON` and controls the test group in `unittests/semantic`, including its aggregate runners. To build native CPU tests without Unicorn, keep `BUILD_TESTING=ON` and set both `NEVERD_ENABLE_SEMANTIC_TESTS=OFF` and `NEVERD_EMULATION_BACKEND_UNICORN=OFF`. The native KVM/WHP tests remain available, including Windows ARM64/MSVC builds with suitable SDK headers. Enabling Unicorn on Windows ARM64 still requires an ARM64 LLVM-MinGW toolchain. This build separation does not establish native ARM64 runtime coverage.

The native-only CI checkout initializes pinned Capstone sources and uses the verified prebuilt LLVM package. With `NEVERD_ENABLE_SEMANTIC_TESTS=OFF` and the Unicorn adapter disabled, its CPU owners configure, build and link without Unicorn sources. Signatures and the external corpus are not dependencies of these owners. Default CI keeps the complete semantic test group enabled.

The `ci.yml` manual profile `native_cpu_only` selects Windows x64 with `native_cpu_backend=whp` (the default), or Ubuntu x64 with `native_cpu_backend=kvm`. `NativeCPUTests.def` shares CPU/process requirements while declaring transport-specific owners and cases separately. `run_native_cpu_ci.py --require-whp` or `--require-kvm` validates the host, builds every owner before CTest and retains inventory, JUnit, logs and outcome counts. Missing or skipped mandatory cases fail even when CTest exits successfully. CI disables Unicorn; `--with-drivers` requires the same original/rebased corpus on the selected transport. Compilation and setup probes do not establish guest execution or ARM64 acceptance. The Ubuntu profile uses the signed upstream Clang/LLD 21 packages; Clang 18/19 CR8 declarations conflict with the pinned WDK headers. Native Linux acceptance uses CMake 4.2.3. `NeverDNativeDriverTests` selects `NO_PRETTY_VALUES` so CTest retains declared case names independently of diagnostic parameter rendering.

Before building, native CI probes `sccache --zero-stats`. A failed probe clears both compiler launchers and retains the same compiler configuration and mandatory test inventory. Configure/build failures remain fatal.

The KVM gate requires real non-exiting vCPU cancellation and 48 state-transfer outcomes from `KvmStateTransferCases.def`, including ioctl capture and failed optional-capability queries. Additional synchronized-register modes execute when supported and report explicit skips otherwise. Stable parameter names avoid dependence on ioctl numbers or tuple formatting. Protocol tests supplement native execution; they do not replace it.

`native-host-probe.yml` runs the standalone `probe_native_host.py` on Linux and Windows x64/ARM64 hosted runners. `NativeHostProbe.def` declares the ordered capability, VM/vCPU creation and cleanup evidence. Reports retain source/binary hashes, native host ISA and every host status code. `setup_ready` proves setup only; no guest instruction is executed. Missing API/device capabilities are `unavailable`, while build, setup, cleanup, timeout and malformed-evidence failures fail the job. Hosted ARM64 availability must be observed per run; this probe does not establish ARM64 workload acceptance. Both Linux workflows use `prepare_kvm_ci.py` to grant only the current hosted runner account access to an existing KVM character device. It records the device identity and permissions; local and self-hosted machines are rejected, and missing devices are not created.

`windows-alignment-oracle.yml` uses `check_windows_alignment.py` and `WindowsAlignmentCases.def` to collect 72 original x64 Windows exception observations: nine aligned SSE forms across seven misaligned address/permission cases and an aligned inaccessible-page control. It retains exception codes, parameters, fault PCs, saved contexts, raw output and source/binary hashes, and verifies unchanged inputs and RAM. These observations establish OS behavior only; they neither certify KVM/WHP execution nor add SEH support.

With `native_cpu_only=true`, `native_driver_tests=true` enables `NeverDNativeDriverTests` without Unicorn. Before configuring, `build_wdk_driver_fixtures.py` verifies the complete SHA-256 of the official Microsoft WDK/SDK 10.0.26100.6584 packages and rebuilds 68 original normal/CFG/DBG driver images. `WDKDriverFixtures.def` owns package identities, compiler/linker arguments and fixture bindings. Unmodified Microsoft inputs and their licenses remain in the local build/cache directories; CI uploads only build metadata and logs. The manifest records tool versions, commands, source/header hashes and output image hashes.

`NativeDriverTests.def` requires 270 WHP outcomes from all 135 workloads in `DriverBuiltinImages.def` and `DriverBackendParityCases.def`: 27 built-in images, 68 WDK images and 40 request scenarios, each at original and rebased addresses. The complete mandatory inventory is `5085 CPU + 270 WHP + 32 SEH + 77 scheduling + 30 wait sets + 29 driver UNPACK + 6 clock reads + 4 image memory checks + 6 CPUID + 7 kernel snapshots + 10 pool captures + 4 DriverEntry caller checks + 126 driver environment checks + 116 driver API checks = 5802`. The 30 wait-set checks comprise sixteen portable model cases and fourteen original native driver cases. `run_native_cpu_ci.py --with-drivers` retains exact inventory/JUnit evidence with Unicorn disabled. Missing or skipped required fixtures fail the opt-in gate; ordinary builds keep external fixtures optional. Fixed images retain their expected rebase rejection. ARM64 native guest execution remains unverified.

The genuine WDK inventory also builds `driver_device_queries.c`, `driver_file_objects.c` and `driver_file_handles.c` in normal and CFG forms. `DriverDeviceQueryTests.cpp` and `DriverFileObjectTests.cpp` reuse the portable callers’ assertions for retained deletion, failed CREATE, process identity, last-handle CLEANUP and deferred CLOSE across both contracts, bases and scheduling policies. All three families have mandatory native gates; inventory validation independently discovers sources with `NEVERD_WDK_FIXTURE` so a dual-mode source cannot silently lose its WDK build.

`InterruptionRetainsPhaseCauseDeadlineAndLease` injects deadline, stop and combined interruptions before two different startup instructions. It checks the exact phase diagnostic, owned message lifetime, preserved error type and cause bits, one unchanged deadline across steps and released memory ownership. Existing real transport failures and state mismatches remain distinct. The native x64 startup validation budget is `5 s`; ordinary guest deadlines and single-step allowances are unchanged.

`WhpResourcePolicy.def` gives WHP x64 and ARM64 resource creation a separate `30 s` allowance before ISA validation. Synchronous host setup is checked against that deadline before publishing the resource. Instruction probes and ordinary guest deadlines retain their own limits. `WhpResourceTests.cpp` checks typed initialization interruptions, cause diagnostics, disposal after cancellation, host-error priority and unchanged ordinary execution deadlines.

`X64PopFlagsTests.cpp` checks both privileges and `driver-strict`: all 256 admitted flag images against two initial states, nine encodings, all 64 input bits, read-only/executable aliases, cross-page faults and repair, observer cancellation/failure, rejected device stacks and subsequent native instruction boundaries. `X64PopFlagsOracle` executes original instructions independently on x64 hosts, checking CPL3/IOPL0 and exact stack consumption. `driver_resource_flags.def` makes the original WDK resource driver set, clear and restore flags with both operand widths. These tests preserve complete integer/control/x87/SSE state; they do not admit guest TF/NT/AC/ID or establish native ARM64 execution.

`X64StatusFlagsTests.cpp` checks `CLC/STC/CMC`, `LAHF/SAHF`, all 256 AH inputs and admitted flag combinations, every REX prefix, complete CPU state, unchanged memory, observer stops/errors, saved contexts and native ADC/store continuation. Invalid LOCK forms reject without effects. An independent host-instruction oracle checks 24 prefix sequences after verifying CPUID support. Original WDK resource drivers exercise all five instructions. These cases add 22 mandatory native outcomes; unavailable host/ISA cells remain explicit skips. The portable Unicorn profile runs the same seven cases; direct dependency tests cover 16/32/64-bit AH and LOCK behavior, explicit REX registers and long-mode feature rejection.

`X64DoubleShiftTests.cpp` covers every admitted imm8/CL count, aliased and extended registers, defined flags, exact cross-page RAM observations, cancellation, permission/unmapped/device faults, rejected LOCK/undefined counts, contexts and native ADC continuation. An independent host oracle checks 5,184 original executions. Twelve original WDK probes cover register and RAM forms. The native gate adds 145 mandatory outcomes.

`X64ScalarShiftTests.cpp` checks all byte counts, both carry inputs, zero/all-one and signed operands, implicit-one forms, AH/SPL and count-register aliases, full CPU state, exact RAM spans, observer rollback, faults and context continuation. An independent native oracle checks 65,536 executions. The WDK resource driver adds 72 original probes. The KVM/WHP gate requires 769 outcomes for this family. Only a zero masked count guarantees that all flags are preserved. A nonzero full `RCL/RCR` carry-ring rotation preserves the operand and CF but leaves OF undefined; the oracle excludes only that undefined bit.

`X64LoopTests.cpp` covers 21 original encodings, wrapping counters, signed targets above 4 GiB, unchanged full CPU/RAM state, observer cancellation, split instruction backing, target-fetch faults and context restoration. Incomplete instruction bytes reject before execution; a fault fetching the branch target retains the retired counter and PC. The WDK resource driver adds 12 original probes. KVM/WHP require 379 outcomes for this family across supervisor, user and driver contracts. The original-host oracle runs up to 1,008 cases and reports its count. Taken AMD `66H` branches target low memory reserved by the host OS, so those forms run in the guest matrix with explicitly mapped low targets instead. Intel and AMD target widths, including REX.W precedence, are checked separately.

`X64BranchTests.cpp` checks all 16 Jcc conditions and relative JMP, nine prefix sequences, short/near forms, signed targets above 4 GiB, complete CPU/RAM state, observer stops/errors, split-page decoding, target-fetch faults and context restoration. The independent Intel-host oracle executes 9,792 original instructions; AMD low-target forms stay in guest tests. Both decoder models have complete/truncated-byte tests, and native probes test publication failure. Four original WDK resource probes exercise driver policy. This adds 274 mandatory outcomes per KVM/WHP gate. AMD software-model coverage does not establish native AMD or ARM64 execution.

`X64StackTests.cpp` covers 42 encodings across nine families: widths, addressing, complete state, ordered observers, cancellation, permissions, split pages, physical aliases, fault repair, device rejection and context restoration. An independent host oracle checks original instructions; six WDK resource probes exercise the driver path. The native KVM/WHP gates each add 1135 mandatory outcomes. Unavailable transports remain explicit skips outside their required native gate.

`X64FrameExitTests.cpp` covers 14 `LEAVE` encodings, effective prefix order, full RBP addressing, complete register state, read-only aliases, split-frame faults and repair, privilege checks, observers, invalid addresses, device rejection and context replay. An independent host oracle executes 42 original instructions; five WDK resource probes check driver execution. Both native gates add 379 mandatory outcomes.

`X64FrameEntryTests.cpp` covers 14 encodings, nesting and overlap, physical aliases, write-only probes, cross-page faults and repair, observer cancellation, privilege, rejected prefixes and context replay. Independent host oracles execute 882 successful instructions and, on Linux x64, 84 faults with exact stack-byte and register checks. Two injected-transport tests distinguish cancellation/failure rollback from architectural fault publication. Six WDK resource probes exercise original driver instructions. Native gates add 508 mandatory KVM outcomes and 507 WHP outcomes.

`DriverSIMDSEHTests.cpp` runs eight original SSE fault cases through four supported dispositions plus an x87-edit rejection, both native contracts, normal/CFG WDK images and two load addresses. Ten backend-specific outcomes and three pure kernel SSE record checks are mandatory. `driver_seh_simd.def` owns fixtures and modes; asynchronous unwind tables cover the faulting helper. Microsoft kernel 10.0.26100.9549 supplied independent classification and restoration evidence: 107,744 isolated instruction-path classifications and 8,192 restorations. These observations are not live Windows kernel-driver execution; ARM64 native KVM/WHP remains unverified.

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

`AArch64WidenCases.def` records 36 independently assembled SSHLL/USHLL and
SSHLL2/USHLL2 forms, including zero-shift SXTL/UXTL aliases. Independent integer
oracles check signed extension, unsigned high bits, both source halves, all
8/16/32-bit source widths, boundary shifts and overlapping registers. Complete
scalar/vector state and guard memory are compared at both privileges; reserved
element-size and scalar encodings fail without mutation.
Lane-move tests cover UMOV/SMOV, their MOV aliases, all legal element/result
widths, first/last lanes, sign extension, W-register upper clearing and XZR.
Capstone's `HasNEONorSME` group is satisfied by baseline NEON; SME-only feature
groups and unsupported register views remain rejected.

`AArch64ReductionCases.def` records 34 independently assembled baseline forms of
CMEQ (register/zero and scalar/vector), UZP1, XTN/XTN2 and UMAXV. Integer lane
oracles cover all admitted widths, unsigned high bits, narrowing truncation,
upper clearing/preservation and source/destination aliases; the full vector,
scalar and guard-memory state is checked at EL0 and EL1. Each malformed or SVE
counterexample has a fresh CPU because a refused instruction terminates execution.
Run the `NeverDAArch64FPTests` label after changing this admission boundary.
The semantic reference is Arm's [instruction guide](https://documentation-service.arm.com/static/6245c734b059dc5ff9a8bdab),
sections D6.15–16, D6.246 and D6.279–281.


`AArch64InstructionEffects` owns scalar and FP/SIMD single/pair RAM footprints, including operands up to 128 bits. The shared address space validates every page before CPU entry; `RAMTransaction` commits only complete declared physical writes. A 128-bit write observer receives two ordered 64-bit words before effects. Stops and faults preserve RAM, vectors and writeback. Numeric Xn/Vn overlap is valid; wrapping pair footprints are rejected. `NeverDAArch64MemoryTests` uses independent `AArch64CrossPageCases.def` and `AArch64VectorMemoryCases.def` encodings. `AArch64StructureMemoryCases.def` adds independently assembled NEON lane, register-list, interleaved and replicate forms, plus reserved encodings. Tests cover B/H/S/D elements, 64/128-bit vectors, register wrap, SP, immediate/negative-register writeback, every crossing, exact ordered observers, stopping at every element, denied final bytes, adjacent inaccessible memory, shared physical aliases and restored contexts. All 39 scalar fields and 32 vectors are compared, including retained lanes and cleared upper halves. The same cases run on checked EL0/EL1 Unicorn and available KVM/WHP/HVF transports; unavailable platforms are explicit skips.

`NeverDAArch64StateTests` checks all 71 complete-state read positions at both privileges, including width normalization, missing readers and retry. `NeverDAArch64FPTests` executes original `AArch64FPCases.def` instructions: all vector lanes, packed arithmetic, scalar/vector floating results, four rounding modes, FZ/DN, cumulative FPSR, context state and rejected extensions/control bits. `NeverDAArch64MemoryTests` tests every crossing offset, ordered observers, stops, denied pages, aliases and restored vector inputs. `NeverDUnicornStateTransferTests` (`UnicornStateTransferCases.def`, `CapturesDeclaredWidthsWithoutStaleUpperBits`) injects every scalar/vector read failure after real execution. Unavailable native transports skip explicitly; these tests and cross-compilation do not replace native ARM64 KVM/WHP evidence.

`NeverDAArch64MemoryTests` covers 18 scalar/pair forms at both privilege levels with Unicorn, KVM and WHP. It checks every crossing offset, sign/width results, observer ordering, denied/unmapped second pages, explicit fault consumption and retry, repeated physical aliases, and context restoration after alias replacement. The former valid crossing-load rejection is reproduced before the change. Unavailable transports skip explicitly; portable Unicorn verification and cross-compilation do not replace native ARM64 KVM/WHP evidence.

`NeverDDriverGuardMetadataTests` (`DriverGuardCases.def`) checks zero-flag dormant CFG metadata, unchanged fallback pointers at both load addresses, invalid slots/targets and missing relocations. Its execution cases use `driver-strict` and `checked-x64-v1` on explicit Unicorn/KVM/WHP transports; unavailable transports skip separately. `DriverPublicCLICases.def` selects `--backend unicorn` for the CLI comparisons with the compatible v1 C API. Native and `auto` selection retain their separate public coverage and never silently fall back when the host API is unavailable.

Checked Unicorn uses `MachineRunControl`: one allowance covers ARM64 maintenance, guest execution and complete state capture. `UC_HOOK_CODE` checks the borrowed stop token and deadline at instruction entry; the synchronous engine call retires its hook borrow before returning, while the machine step retains control through publication. Unicorn and WHP stage complete CPU state and check the same control before publishing a successful step. WHP creates its allowance once before preparation. An authenticated x64 CPU exception takes precedence over a stop arriving during capture. The checked RAM transaction discards speculative stores when capture is cancelled; the unrestricted software contract is unchanged. `MachineInterruptedError` distinguishes acknowledged cancellation from host or capture failure. The shared checked CPU returns `Stopped` or `Deadline`, preserves CPU/RAM and permits retry; genuine failures remain `BackendFailure` even with a simultaneous stop.

Capture regressions: `NeverDUnicornStateTransferTests`, `NeverDUnicornMachineControlTests`: `StopDuringCaptureCannotPublishAndAllowsRetry`, `ExpiredCaptureCannotPublishAndAllowsRetry`, `CompletedStoreCannotPublishCancelledCapture`, `StopDuringCaptureCannotHideRealGuestException`. `UnicornPublicCapture.CancellationKeepsTypedExitStateAndRAMConsistent`; `NeverDKvmStateTransferTests`: `PublicCancellationRetainsStateRAMAndFailurePriority`.

`NeverDUnicornMachineControlTests` uses original store instructions from `UnicornMachineControlCases.def` on real x64 and ARM64 engines at both privileges. `RejectedEntryPreservesStateAndRAMAndAllowsRetry` checks cancellation before stepping, stop/deadline expiry at the actual guest entry, unchanged complete input and RAM, and one successful subsequent store. Its test-only entry wrapper does not require a hypervisor and does not establish native ARM64/WHP evidence.

`RunDeadline::invoke` rejects a stopped or expired WHP entry before calling the host, retains an actual host result during cancellation, and acknowledges interrupt callbacks before releasing the borrowed token. KVM and WHP validate a successfully captured private packet on the owning caller before classifying a concurrent stop or deadline. Genuine host/capture failures and authenticated x64 CPU exceptions retain priority. Ordinary successful state stays private until cancellation checks finish; an acknowledged interruption discards speculative CPU/RAM effects and permits retry. Preparation, native execution and capture share one step allowance. These controls provide cooperative cancellation, without a hard wall-clock guarantee.

`NeverDRunControlTests` includes portable `NativeEntryTests.cpp` and Windows WHP-enabled `WhpEntryControlTests.cpp`. In-memory host callbacks check rejected entry, retry, late cancellation, retained failures, completion priority and acknowledged callback lifetime without requiring Hyper-V. `NeverDKvmRunTests` checks caller-thread completion, error priority and reentry rejection. Real `NeverDKvmStateTransferTests` executes `KvmStateTransferCases.def` instructions; `ActualCPUExceptionOutranksStopDuringCapture` and `PublicCPUExceptionOutranksStopDuringCapture` stop after actual register/XSAVE reads and preserve divide exceptions, original context, RAM and explicit recovery. Portable tests executed with the Windows ABI under Wine are threading/control evidence only; they do not establish native WHP execution. Unavailable native transports remain explicit skips.

`NeverDInstructionFetchTests` executes x64/ARM64 checked programs through Unicorn, KVM and WHP at supervisor and user privilege. `InstructionFetchCases.def` covers changing operand forms and relative branches, guest and host writes through code aliases, context restoration, permission revocation, separate page backing, page-tail lookahead, invalid/truncated encodings and recursive entry rejection. Native Windows CI requires every x64 WHP case to pass. Unavailable host/ISA pairs are explicit skips; portable ARM64 execution does not establish native ARM64 support.

`RepeatedOperandsSurviveCodeWorkingSetEviction` repeats operand, memory-hook and relative-branch checks before and after a larger code working set. Checked execution retains bounded, owned Capstone metadata only while the current instruction bytes match; every step still fetches current bytes and checks execute permissions. Full operand details and decoder aliases remain available on cache hits.

`HostOperandWriteRefreshesAnInstructionAtThePageTail` changes only operand
bytes through another mapping, including a separately backed x64 page tail.
`HostWriteAfterStopRefreshesAnUnexecutedInstruction` changes a potentially
translated successor before its first execution. `RepeatedStepsRetainExactLoopAndStopState`
checks loop results and the precise stopped PC under repeated checked entries.
Together with the user-projection, RAM-transaction, code-alias and direct-run
suites, these guard bounded Unicorn translation invalidation. Test duration
is useful for local comparisons, not a portable throughput guarantee.

`WhpStateTransferTests.cpp` checks both XSAVE API generations with injected register transfers: exact changed groups, complete capture, ignored padding, partial failures, cancellation, exception priority and partition replacement. `ContinuedStepsReuseCapturedRegistersAndFP` counts avoided installs; `PartialTransferFailuresPreserveStateAndForceFullRetry` requires complete restoration. These are protocol checks, not native execution evidence; existing native FP, state-transition, driver and ring3 suites remain required.

`CancelledDirectRunPublishesACompleteBoundary` verifies complete state at an acknowledged direct-run cancellation. `FailedDirectCapturePreservesStateAndForcesFullRetry` requires unchanged caller state when register, XSAVE or metadata capture fails during cancellation, followed by a complete retry; neither API generation may publish a partial register prefix.

`WhpStateTransferCases.def` also covers every partial prefix of the combined 32-register capture and conflicts in all seven metadata fields. Both XSAVE API generations must preserve caller state and force a complete retry. The same suite checks one register read per step and recovers omitted XSAVE metadata from that read.

### Android native workloads

`AndroidAuxvTests` compares imported and named `getauxval` calls at O0/O2 with
ordinary, Android-packed and RELR relocations on each available checked ARM64
transport. It covers complete output buffers and canaries, 64-bit type/value
preservation, absent versus empty input, present zero versus missing-key errno,
constructor/finalizer queries, duplicate/null/oversized input rejection, the
128-entry boundary, closed provider refusal and per-thread errno isolation.
Malformed JSON shape and unsigned ranges are checked separately. These are
authored inputs; the tests do not inherit host auxiliary-vector values.

`AndroidCalendarTests` exercises imported and named calendar calls at O0/O2
with ordinary, Android-packed and RELR relocations on available ARM64
transports. Independent civil dates cover negative epochs, 1900/2000/2100
leap rules, post-2038 seconds and years 1/9999. Full-buffer comparisons cover
LP64 fields, padding, zone bytes, aliasing, fixed offsets in both directions,
shared process storage across APIs/threads, caller storage and errno isolation.
Missing zones, excluded ranges, closed providers, invalid memory and malformed
zone input remain visible failures. Tests never read the host time zone.
Inverse cases use independent expected timestamps and complete buffer images
for signed carries, Gregorian centuries, ignored derived fields, padding,
fixed offsets, endpoint dates, static-object aliasing and the API 28 valid
`-1` errno rule. Positive DST and null input check the exact failure effects,
including read-only storage when `tm_isdst` is already one. Direct model tests
check an `mktime`-only zone arena, cross-page write refusal, exact memory-limit
admission and expired-budget atomicity against all original guest bytes.

`AndroidObservation.*` checks complete workload equality with and without
selected PC observations across ordinary/APS2/RELR images and Unicorn/HVF.
Exact instruction bytes provide independent before-store register and memory
expectations. Single- and multi-thread runs use quanta as short as one
instruction; truncation, unknown imports, unreadable memory and output-budget
admission are covered. `AndroidObservationOptions.*` checks strict JSON.
Register-relative cases compare complete reports against unobserved execution,
follow distinct pointers in interleaved threads, check signed offsets and SP
against independent instruction bytes, and reject wraparound, inaccessible
tails, invalid registers and excessive output before guest effects.
`RuntimeSession.CheckedQuantumEndsBeforeTheNextExecutionWatch` covers the
shared watch/quantum boundary on checked x64 and AArch64 transports.

`AndroidLog.*` checks the five liblog reader operations with independent O0/O2
callers, ordinary/APS2/RELR relocations and Unicorn/HVF. It compares complete
v4 message bytes, PID/time/tail selection, exact-time exclusion, separate cursors,
reopening, provider identity, handle retirement, errno and unsupported inputs.
`AndroidLogOptions.*` checks strict JSON admission; the same validator admits
C++ snapshots before execution. See [Android log readers](android-log-reader.md).

`AndroidDevices.*` runs independent O0/O2 callers with ordinary, APS2 and RELR
relocations. Raw, named, variadic and dynamic ioctl routes check full buffers,
errno, scalar argument width, independent opens, descriptor reuse and provider
lifetime. Rejected requests, nested pointers, later input/output faults and
read-only buffers must preserve bytes; alias cases resolve every input before
outputs. `AndroidDeviceMemory.*` checks retry without sequence consumption.
Linux x64/ARM64 callers use the same synthetic inputs and independent byte
oracle. Process JSON and Python integration cover strict schema, shared limits
and public API parity. These tests validate explicit device transactions, not
real hardware or driver execution.

`LinuxMemory.Device*` checks device snapshot alias identity, partial retirement,
maximum permissions across coalesced ranges, refused requests and allocation
budgets. `AndroidDevices.DeviceMapping*` executes raw, Bionic, variadic and dynamic
callers at O0/O2 with each relocation packing, including access faults and mappings
that survive descriptor close. The Linux device fixture runs the same lifecycle
checks on x64 and ARM64. `ProcessReport.LinuxDeviceMappings*` checks strict JSON
and C++ admission; the Python process integration test transports the same public
contract through the C ABI. Run the shared Linux process, Android native,
execution-session and process-public suites when changing this memory boundary.

`AndroidDevices` and the shared O0/O2 x64/ARM64 device fixture cover duplicated
file cursors, ioctl positions and output sinks; context ownership and VMA-retained
lifetimes; consumed/retired timestamps; equal-time, cross-page producer writes;
PROT_NONE aliases, partial unmapping and address reuse. Context refusal cases
check unchanged output bytes for unsupported flags, copy-out and busy retirement.
A pure guest polling loop requires the explicit instruction-time input; a runnable
child and sleeping/joining parent share that clock and the updated physical page.
`LinuxClock.InstructionTime*` tests cumulative accounting, idle coexistence and
atomic overflow. `ProcessReport.StatefulDevices*` checks strict JSON and C++
admission. The Python device integration case exercises the same contracts through
the C ABI. These are deterministic model checks, not vendor GPU or SDK equivalence.
Run the Linux process, Android native and public process suites when changing these
shared owners; report unavailable transports separately.

`AndroidResidencyTests.cpp` compares raw SVC, variadic and named `mincore`
calls using independent O0/O2 fixtures with ordinary, APS2 and RELR relocations.
It checks ordered range/alignment errors, empty queries, unchanged output bytes,
errno and mapped-page refusal. `LinuxMemoryTests.cpp` checks precise user-range
boundaries, PROT_NONE and a mapped prefix before a hole without changing bytes,
mapping generation or allocation counts. `LinuxProcessTests.cpp` executes the
corresponding x64/ARM64 raw syscall callers and verifies the full output vector.

`AndroidSnapshotTests.cpp` uses independent O0/O2 C fixtures with all three
relocation packings to allocate and fill memory during a native call. It checks
explicitly deferred snapshots against full byte expectations, preserves the
default entry-mapping requirement, and exercises unmapping, permission removal,
unsupported imports, terminal faults, instruction limits and pre-execution
size/output budgets. The same cases run on every available AArch64 backend.
`ProcessReportTests.cpp` checks the per-read JSON Boolean and rejects it on
memory initialization regions.

`AndroidSignalTests.cpp` uses independent O0/O2 C declarations with ordinary,
Android-packed and RELR relocations. It compares full guarded buffers for
Bionic/kernel layouts, padding, 64-bit observations, signed flags, reserved
masks, overlapping objects and disposition changes surviving failed copy-out.
It also covers missing observations, validation order, errno, provider lifetime
and API 28's indeterminate old output after an error. A direct model case checks
earlier field stores and retained state after a later user-space pointer fault.
`LinuxProcessTests` executes original x64/AArch64 syscall fixtures at O0/O2;
`ProcessReportTests` checks lossless input, malformed entries and profile gates.
These tests validate action bookkeeping, not signal delivery or handler frames.

`AndroidNativeTests.cpp` checks device API queries against absent, empty,
positive, nonpositive, prefix and overflowing property values through direct
imports and dynamic providers. Repeated results, errno preservation and closed
provider rejection are checked with independently compiled O1 callers across
ordinary, APS2 and RELR relocations. The O0/O2 decimal conversion tests cover
the shared parser's guest-memory behavior.
An external device query must be replayed from explicit captured properties;
its API level alone does not establish compatibility with that Android release.

`ProcessReport.LinuxDeviceMappingBase64PreservesCompleteBinaryPages` compares
every decoded byte of a 32 KiB binary page input while retaining the 64 KiB JSON
limit. It rejects ambiguous encodings, malformed padding/alphabet, noncanonical
spare bits, partial pages and unaligned offsets. The Python process tests execute
the same mapping lifecycle through raw, named, variadic and dynamic calls with
the compact input. Base64 changes transport only; shared Linux mapping and
catalogue bounds remain authoritative.

`AndroidNativeTests.cpp` compares both Android `sysconf` page-size selectors
with `getpagesize`, page-aligned allocation and its final valid byte. Compiled
callers check selector width, errno, dynamic provider lifetime and explicit
refusal of other configuration queries. Host page sizes and limits are not
used as an oracle.

`AndroidScanningTests.cpp` uses independently compiled O0/O2 callers with
ordinary, APS2 and RELR relocations. It checks integer widths, register/stack
arguments, `va_copy`, explicit save areas, matching versus input failures,
field widths, prefix rollback, numeric buffer limits, suppression, `%n`,
destination aliases, thread errno and dynamic provider lifetime. Full memory
comparisons cover canaries and refusals. Formatting regressions exercise the
same GP argument reader. These tests establish the bounded Android model;
they do not establish behavior on an Android device.

`AndroidSearchTests.cpp` executes independently compiled character-search
callers at O0/O2 with ordinary, APS2 and RELR relocations on each available
ARM64 backend. It checks first/last matches, NUL and unsigned characters,
64-bit bounds, read-only page tails, Fortify-before-read ordering, unchanged
guest bytes and errno, and dynamic provider lifetime. Direct model checks
separate object bounds from memory and deadline limits. C API/CLI and Python
cover successful named calls and non-returning Fortify failures. Pinned API 28
source defines the contract; these checks do not establish device equivalence.

`AndroidTimeTests.cpp` executes independent O0/O2 C fixtures with ordinary,
APS2 and RELR relocations. It checks explicit fixed clocks, post-2038 and
negative 64-bit seconds, exact timeval/timespec/timezone bytes, errno, dynamic
provider lifetime, absent inputs, fault ordering and unsupported partial
copies. Linux process fixtures execute the raw x64/ARM64 service ABIs at O0/O2
through available backends. Process JSON tests cover malformed and lossless
inputs; C API/CLI and Python tests preserve dynamic names and exact output.
Released GKI CPU clock cases compare raw and Bionic calls across all eight
source pins, including current-process aliases, distinct PROF/VIRT/SCHED
observations, low-32-bit arguments, target errors before pointer faults and
unchanged canaries. The cooperative syscall fixture checks that the current
nonleader TID still names its group's CPU sample. Linux raw callers verify that
idle advancement changes wall clocks while leaving CPU samples fixed.
These are deterministic model checks, not an Android device or native Linux
clock comparison. Unsupported native transports remain explicit skips.

With CPU emulation enabled, build `NeverDAndroidNativeTests`,
`NeverDLinuxProcessTests`, `NeverDExecutionSessionTests`, and
`NeverDProcessPublicTests`. The Android fixtures are independently authored
freestanding ARM64 C, linked with Clang/LLD as ordinary, APS2, and RELR shared
libraries. They exercise constructors, sectionless linking, stack arguments,
TLS/stack guards, explicit properties, memory allocation, raw versus Bionic
error returns, output and execution limits, unsupported imports, and SDK/CLI
report parity. No Android device, NDK sysroot, or proprietary fixture is used.
File-initialized memory tests copy more than 64 KiB through C++, C/CLI and
Python, check a guest-computed hash, exact bytes, zero padding, guest-only
mutations and fresh reads on a subsequent workload. They reject conflicting
initializers, missing/non-regular/oversized inputs and aggregate memory-limit
overflow. A deterministic expired deadline leaves guest bytes unchanged.
The dynamic lookup fixtures exercise explicit library catalogues, provider
identity, repeated opens and NOLOAD, missing symbols, stale handles, null and
invalid names, TLS slot 6 consume-once errors, and named calls through guest
traps. Default-scope cases distinguish unknown and explicitly empty scopes,
reverse duplicate-provider priority, skip providers without an export, retain
resident functions and handles across opens/closes, and prevent local opens
or a previous workload from widening the scope. Unknown implementations and
unsupported lookup scopes must stop. C/CLI and Python integration tests check the same lookup names and
returned addresses; no host library supplies those functions.
Identity fixtures compare Bionic imports, named dynamic calls and raw ARM64
services, retain errno, and reject calls after the provider closes. Linux
x64/ARM64 fixtures compare real/effective UID/GID queries with startup auxv;
credential mutation must still stop as unsupported.
Page-size fixtures check the fixed 4096-byte guest layout, unchanged errno,
direct and named dynamic calls, explicit catalogue membership, closed-provider
rejection and resident-provider calls after close across all three relocation
packings. The result does not depend on the host's page size.
Independent `pthread_once` fixtures at O0/O2 and all three relocation packings
check constructor initialization, nested callbacks, imports inside callbacks,
repeated calls and live stack locals.
Default-scope callbacks also retain their resident provider after the explicit
handle closes; an absent scope and an explicit empty scope remain distinct.
Negative cases cover invalid controls and callback addresses, read-only
memory, recursion, unsupported imports,
unbalanced callback stacks and exhaustion of the original instruction budget.
C/CLI and Python tests check callback effects and ordered nullable call results
through the real shared engine. This is modeled API 28 evidence, not a native
Android device comparison or proof of parallel SMP initialization semantics.
Cooperative thread fixtures also check multiple once waiters, nested controls,
join/once cycles, dynamic symbol lookup, retained registers and TLS, callback
write ordering, and cumulative budgets. Control mutation or unmapping before
waiter resumption must leave the original call incomplete. Complete reports
from the same independent programs are compared between Unicorn and available
native ARM64 HVF execution.
`AndroidPropertyTests.cpp` runs independently authored O0/O2 callers with
ordinary, APS2 and RELR linking. Full-buffer expectations cover stable handles,
empty/missing values, maximum short values, full callback names versus legacy
truncation, overlapping output order, page-tail names and unsigned nth indices.
Nested foreach/read/once callbacks and cooperative threads check live stack
locals, full-width cookies, separate errno and ordered pending call results.
Invalid handles, callback addresses, output permissions, mapping retirement,
changed property bytes and aggregate memory exhaustion have explicit outcomes.
Unsupported callbacks and instruction/event exhaustion cannot complete the
parent call. Available HVF execution is compared with Unicorn on the same
nested/thread programs. Public C/CLI and Python checks reuse the dynamic
property fixture and provider-lifetime controls. These are immutable API 28
model checks; property-service updates, SELinux contexts, long values and native
Android-device equivalence are not established. Run the full Android native,
execution-session and process-public suites after callback-owner changes.

`AndroidStdio` executes independent O0/O2 callers with ordinary, APS2 and RELR
relocations on Unicorn, and O0/O2 RELR callers on HVF when available. It checks
complete output buffers, raw FD offsets, partial element reads, EOF retries,
seek/position aliases, fdopen/dup ownership, buffer replacement, allocation
fallback, direct-read EFAULT versus buffered-pointer faults, descriptor closure,
opaque handles, errno isolation and dynamic provider retirement. C API/CLI and
Python cases use the same fixture and compare exact returned memory. All file
contents and metadata are synthetic explicit inputs; no device or host stdio
is an oracle for the Android ABI. Run `ctest -R AndroidStdio` after building
`NeverDAndroidNativeTests`; unsupported backends remain skips, not passes.

The same `AndroidStdio` fixture covers full/line/unbuffered writes, read/write
transitions, append with competing opens, flush failures, raw-versus-buffered
pointer faults, dynamic proc FD reuse and directory buffering/cookies.
`LinuxFileNamespace` in `NeverDLinuxProcessTests` independently checks both
LP64 architectures, complete defined dirent bytes and untouched padding,
partial source-page writes, closed FD slots, EOF cookies and budget refusal.
C/CLI and Python exercise fresh mutable-file and proc workloads through the
public JSON contract. None of these fixtures needs a phone or a host filesystem
oracle. Live maps/task/status/fdinfo and opt-in timestamp/allocation maintenance
have separate `LinuxProcTests` and `AndroidProcTests` coverage; see
[live proc](linux-live-proc.md). These bounded contracts do not establish
creation or complete Android filesystem equivalence.

`LinuxFileNamespace.RepeatedSeparators*` and `Separator*` check repeated guest
path separators in both LP64 ABIs: shared bytes and metadata, write visibility,
directory and link boundaries, and the original 4096-byte import limit.
`AndroidFiles.RepeatedSeparatorsUseTheSharedFileNamespace` runs independent raw
syscall and Bionic callers at O0/O2 with all relocation packings; successful
aliases preserve errno and missing/non-directory paths retain exact errors.
Catalogue inputs still require canonical names, and dot walks remain explicit
unsupported boundaries.

Independent tokenization fixtures at O0/O2 and all three relocation packings
check changed delimiters, interleaved contexts, unsigned bytes, final/empty
tokens, exact cursor width, input mutations and errno preservation. Read-only
inputs and cursors, invalid pointers, scan limits and stale dynamic providers
have explicit outcomes. C/CLI and Python integration compare named calls and
guest effects through the shared engine. The API 28 cursor contract is checked
against [pinned AOSP source](https://android.googlesource.com/platform/bionic/+/android-9.0.0_r1/libc/upstream-openbsd/lib/libc/string/strtok.c);
this is model evidence, not a native Android device comparison.

Independent thread-attribute fixtures run at O0/O2 with ordinary, APS2 and
RELR packing. They check the complete 56-byte LP64 object, retained padding,
destruction fill, integer versus pointer widths, historical inheritance rules,
full-width stack values, invalid-value precedence, ignored scope pointers,
overlapping outputs, inaccessible unused tails and provider lifetime. C/CLI
and the actual Python wrapper exercise named dynamic calls through the shared
library. The default single-thread contract still rejects creation and
thread-state queries. These are pinned API 28 model checks, not an Android
device comparison.

`AndroidThreadTests.cpp` opts into guest scheduling with independent O0/O2
ordinary/APS2/RELR fixtures. It checks scalar/vector/flags/FP state, separate
TLS/errno/dlerror/stack, common imported/raw/variadic identity, recursive mutex
owners, nested once/finalize continuations, join/detach lifetime, thread versus
process exit, instruction/report limits, guard faults and rejected state.
Repeated software reports must match exactly. Available ARM64 HVF execution
is compared separately with Unicorn; unavailable transport is an explicit
skip. C API/CLI and Python checks preserve thread attribution and dynamically
resolved API names. These tests do not establish concurrent SMP or native
Android device equivalence.

Independent mutex fixtures run at O0/O2 with ordinary, APS2 and RELR packing.
They check eight-byte attributes, four-byte getter outputs, complete 40-byte
initialization, overlapping attributes, static initializers, all three lock
types, errno preservation, recursive exhaustion, foreign owners, dynamic
provider names and explicit contention stops without scheduling. With
scheduling enabled they check three waiting threads, shared/private state,
recursive final release, unlocker reacquisition, a woken contender waiting
again, original event identity, TLS errno, deadlock, and instruction limits.
Destroyed/changed objects and revoked permissions are rechecked on resume.
Cross-page tests retain unused read-only bytes and reject a denied owner
write without publishing half a transition. Cases run on Unicorn and
available KVM/WHP/HVF transports, with unavailable hosts reported as skips.
These are API 28 model checks, not native Android device or SMP equivalence.

Linux regression tests guard the shared kernel-service boundary. Native KVM
and WHP cells may be unavailable on the host; report their skips separately.

`windows-pe64-v1` supports bounded Windows x64/ARM64 console processes with PEB/TEB, static and dynamic TLS, `DllMain`, named Win32 APIs and explicit acyclic DLL graphs. Guest modules support named/ordinal code and data imports, DIR64 rebasing, forwarded exports and actual loader-list identities. `LoadLibraryA` / `LoadLibraryW`, `FreeLibrary` and `GetProcAddress` use the configured module catalogue. CRT/GUI, ARM64 frame-based user SEH, threads and general Windows application compatibility remain unfinished; native ARM64 KVM/WHP evidence is still pending.

Input-file bytes and aggregate image extents each share `memory_limit`; runtime mappings also consume the image budget. Preparation shares a 65,536-record and 64 MiB metadata-read allowance, bounded names and the workload deadline. Blocking host I/O has no hard time guarantee. The original EXE→DLL→DLL fixture checks rebased pointers, ordinal calls, shared data, API pointer identity, `MEM_IMAGE`, loader lists and executable TLS attach/detach. `NeverDWindowsProcessTests` owns these checks and the direct native Windows oracle; `NeverDPEProgramExportsTests` checks malformed metadata and work accounting, and `NeverDProcessPublicTests` checks C ABI/CLI catalogue parity. Unavailable transports are explicit skips.

`WindowsProcess.ClockServicesUseConsistentUnitsAndPreserveLastError` runs original x64/ARM64 PE calls for `QueryPerformanceFrequency`, the monotonic counter, FILETIME, wrapping tick count and a relative delay. `WindowsProcess.UnmodeledDelaysStopWithoutClaimingCompletion` checks alertable, positive absolute and INT64_MIN intervals before completion. `NativeWindowsOracleRunsTheSameExecutable` also executes the successful clock scenario directly on Windows; both guest regressions are mandatory in Unicorn-free KVM/WHP acceptance. The original fixture explicitly poisons unused `BOOLEAN` register bits and uses typed 64-bit constants, preserving the epoch and INT64_MIN under the Windows ABI.

`ARM64 native backend build` compiles `NeverDEmulationNative` with Unicorn disabled on `ubuntu-24.04-arm` (KVM) and `windows-11-arm` (WHP), using the pinned LLVM sources. `audit_native_backend_build.py` checks every declared native source, its active backend definition, compile recipe, ARM64 ELF/COFF object and hashes. `probe_native_host.py` records setup availability and resource cleanup; unavailable facilities are explicit and setup failures fail the job. This verifies compilation and host setup; guest execution remains unverified. `NeverDCapstoneCompilerOptions.inc` scopes the qualifier diagnostic option to Clang C compilations; GCC and MSVC retain their own warning policies. `native_arm64_only=true` selects these ARM64 component builds and setup probes without the full x64 CPU acceptance profile. Build audits canonicalize source and build roots before matching paths, including Windows 8.3 aliases. The ARM64 component job explicitly enables its selected KVM or WHP backend before the build audit.

`WindowsTestExecution.def` selects the Unicorn ARM64 `WindowsExclusive` comparison for `RUN_SERIAL`. The CTest policy avoids contention with other guest workloads while retaining the original 60 s guest deadline and all result, register, permission and native-digest checks.

`run_native_cpu_methods.py` validates the boolean `RUN_SERIAL` property from `NativeMethodExecution.def` and retains it in method grouping and cross-run execution contracts. Methods run one at a time; an unretired child prevents the next method. Unknown properties and changed shard contracts still fail.

`WindowsProcessLifetime` runs dependency DLL TLS callbacks then `DllMain`, followed by EXE TLS and entry, on one CPU under the same execution budget. Each module gets an independent TLS index and aligned block copied from the relocated, linked image within a shared 64 KiB arena. TLS reserved arguments are zero; startup/process-detach `DllMain` receives an opaque non-null value. Explicit process exit detaches successfully initialized DLLs in reverse loader-list order, then EXE TLS, even if EXE initialization had not run. Startup `DllMain(FALSE)` exits with `0xc0000142` without detach notifications. Faults and exhausted budgets do not invent cleanup. Returning from the PE entry with guest DLLs requires unsupported thread termination and stops explicitly. Nonzero `SizeOfZeroFill` remains unsupported; zero-initialized bytes in the actual TLS template are supported. DLLs without entry points receive TLS attach but no process-detach notifications.

`WindowsProcessExports` resolves static imports and `GetProcAddress` through the same named/ordinal identities, including code, data, aliases and chained forwarders. Only demanded startup forwarders add catalogue modules and initialization dependencies; unused forwarders do not load files. Export names are case sensitive; a missing name returns NULL/error 127, a direct missing ordinal (including a hole) returns NULL/error 182, and a null query argument returns error 87, while success preserves LastError. Unknown module handles remain unsupported. Exact provider/name API gates are reserved once from the bounded registry. Resolution checks every queried image’s live PE headers and export metadata, rejects changes or unreadable bytes, limits chains to 64 entries, and shares remaining preparation metadata credits and the workload deadline. A forwarder targeting a hole returns the target image base and preserves LastError; forwarding to ordinal zero returns error 87. The returned base is a data address, not permission to execute image headers. Runtime forwarders can load configured catalogue modules and complete initialization before returning a lookup result. Live export-table rewriting remains unsupported.

`WindowsProcessLoader` loads ASCII DLL basenames from `windows.modules` and owns explicit references, shared dependencies and startup retention. Repeated forwarded queries do not acquire extra references. Catalogue slots carry a new resident generation on reload. TLS and `DllMain` execute on the same CPU below suspended API frames; restoring registers preserves guest writes and uses the live return slot. Dynamic attach/detach reserved pointers are zero. Failed attach during explicit loading returns error 1114 after cleanup, retaining successful independent nested loads. Unload releases image mappings and TLS; reload restores original image contents. Loader-list or TLS-pointer changes outside the model fail explicitly. File, image and metadata work credits remain cumulative across failures and reloads. System providers use their mapped PE bases as module handles. Filesystem search, non-ASCII paths, `LoadLibraryEx` flags, cyclic imports and reentrant transitions of an already initializing or unloading module remain unsupported.

`GetEnvironmentVariableW`, `SetEnvironmentVariableW`, `GetEnvironmentStringsW`, `FreeEnvironmentStringsW`, `ExpandEnvironmentStringsW` share the live guest environment block in PEB process parameters. Names are ASCII and case insensitive; values are UTF-16. Updates validate inputs, capacity and writable memory before publication. Snapshots remain independent after changes and release their guest backing on free. The block has a 64 KiB model limit; strings and expansions are bounded and check the workload deadline. Unknown pointer ownership, malformed blocks, ANSI code pages and overlapping expansion buffers remain unsupported. `WindowsEnvironmentTests.cpp` compares original x64/ARM64 fixtures across available backends and requires an independent native Windows oracle in CI.

`WindowsProcessHeap` now owns allocation, `HeapReAlloc`, free and size queries in one process heap. Resizing preserves the retained bytes; `HEAP_ZERO_MEMORY` clears newly exposed bytes, and `HEAP_REALLOC_IN_PLACE_ONLY` forbids movement. Failed resizing preserves the old block and returns NULL with `ERROR_NOT_ENOUGH_MEMORY` (8), matching the native observations. Separate page backing lets shrink/free return capacity, while staged growth and bounded copies observe the workload deadline. Custom heaps, exception-generating flags, unknown ownership and inaccessible copy/zero spans stop explicitly. `WindowsHeapTests.cpp` covers both ISAs, forced movement, budget reuse and failure atomicity; CI also runs the original EXE against native Windows. Batch PE cases use the common 120-second CTest harness timeout; each guest workload retains its own finite budget. The heap fixture permits 20 seconds per process for complete data checks on WHP.

`WindowsSystemModules` builds bounded PE64 model images for `ntdll.dll`, `kernelbase.dll` and `kernel32.dll` on both ISAs. Their mapped bases are shared by ASCII `GetModuleHandleA` / `GetModuleHandleW`, `LoadLibraryA` / `LoadLibraryW` and `GetProcAddress`; PEB/LDR and `MEM_IMAGE` describe those same images. Static imports, named queries and guest forwarders use the same API gates and export resolver. Providers stay pinned, have no guest initialization callbacks and do not prevent entry return after ordinary guest DLLs unload. Changed headers or export metadata stop lookup. Unknown system export names and nonzero system ordinal queries stop explicitly; case-only mismatches of modeled names and empty names return error 127, while a null query returns 87. Generated bytes and addresses are model policy; Windows DLL version layouts, native ordinals and cross-provider aliases are not reconstructed. `WindowsSystemTests.cpp` compares original x64/ARM64 executables with native Windows, including eight independent initial-thread returns.

`WindowsSectionFixture.inc` checks two independent image views, handle reuse, closing before reading, view isolation after a write, unmapping and preservation of the resident provider. Negative cases cover namespaces, access rights, fixed addresses and offsets. `WindowsThreadFixture.inc` checks affinity separately from hiding state, exact lengths and dirty upper argument bits. The original fixtures also run in the native Windows oracle; Wine with no `KnownDlls` namespace cannot validate the section scenario.

`WindowsProcessExceptions` implements `AddVectoredExceptionHandler`, `RemoveVectoredExceptionHandler` and `RaiseException` on one CPU with the process budget. Ordered handlers may register or remove handlers, raise nested exceptions, call modeled APIs, load DLLs and exit the process. x64/ARM64 data-access violations and x64 integer divide faults can resume after validated guest edits to `CONTEXT`; general registers, SIMD and supported FP state are preserved. Software exceptions resume through a real return instruction in the modeled provider. The model bounds registrations to 128 retained entries and nesting to 16 frames. Invalid dispositions, changed exception pointers, unsupported context fields and exhausted bounds fail explicitly. ARM64 frame-based SEH/unwinding, debugger delivery and execute/guard faults remain unsupported. `WindowsExceptionTests.cpp` compares original EXE/DLL scenarios against native Windows; native ARM64 KVM/WHP evidence remains pending. Software exception records carry `EXCEPTION_SOFTWARE_ORIGINATE` (`0x80`), independently of the caller’s noncontinuable flag; the original Windows executable checks the exact software and hardware flag values.

`WindowsProcessContext` retains each dispatch frame’s origin. Admitted x64 data-access and divide faults expose RF (`0x10000`) in `CONTEXT.EFlags`; `RaiseException`, including software access-violation codes, retains the current context. The origin survives VEH/VCH and SEH search/unwind. Valid continuation restores logical CPU flags without RF; guest edits to RF are rejected before state publication. This bounded profile does not model instruction breakpoints or guest-controlled RF. `WindowsExceptionTests.cpp` checks saved records, restoration and unchanged CPU/RAM on rejection.

Windows ring3 maps checked x64 `operand_alignment` faults to `STATUS_ACCESS_VIOLATION` with parameters `[read, UINT64_MAX]`, including stores, following independent native observations. The CPU layer supplies the cause; Windows does not guess it from vector 13 or decode the instruction again. `WindowsAlignmentProcessTests.cpp` runs original PE instructions across 72 fault scenarios and 9 address-repair retries (`72 + 9`), checking PC, RF, XMM state and RAM. Unclassified or inconsistent faults remain rejected. Process and driver fault reports preserve nullable `cause` and hexadecimal `error_code`; absence stays distinct from zero. This delivery applies to the checked x64 user profile. After every fault or repaired retry, the fixture exports the complete 4096-byte page; the host verifies all 81 snapshots and the actual completion counters within the unchanged guest deadline. Native process and initial-thread observations use `CREATE_DEFAULT_ERROR_MODE`: GoogleTest enables the inherited `SEM_NOALIGNMENTFAULTEXCEPT` flag, which can make Windows repair the faults being measured. The oracle therefore observes the system default instead of the test harness policy.

`AddVectoredContinueHandler` and `RemoveVectoredContinueHandler` maintain a separate ordered list, sharing the 128 retained registration limit with exception handlers. Continue callbacks run after a vectored exception handler accepts continuation; they see the same mutable exception record and `CONTEXT`. Final context validation happens after these callbacks, including nested exceptions and DLL notifications. Handles cannot be removed through the other handler family. `WindowsContinuationTests.cpp` compares original executables for ordering, short-circuiting, mutation, context repair, nested dispatch, loader callbacks and process exit against native Windows. The tested Windows x64 vectored path permits continuation with `EXCEPTION_NONCONTINUABLE` set; this does not establish frame-based SEH behavior. Native ARM64 execution remains unverified.

`RtlCaptureContext` is available through `kernel32.dll` and `ntdll.dll` for x64 and ARM64. Shared `WindowsProcessContext` and `IntegerABI` record the caller’s PC/SP without changing CPU state or LastError. Native Windows observations establish x64 flags `0x10000f`, preservation of untouched home/debug/vector storage, and the legacy 32-bit x87 address fields; ARM64 records PC from LR and clears the saved X0/LR. Register values, SIMD and FP controls come from the guest; x64 selectors and the MXCSR capability mask follow the configured guest CPU. Invalid, unaligned or partly inaccessible destination records fail before publication. `WindowsContextTests.cpp` covers direct imports, provider lookup, VEH callbacks, cross-page output and failure atomicity. `scripts/check_windows_context.py` runs the original executable on Windows x64 and ARM64, with a separate native nonempty-x87 oracle. These ARM64 API observations do not establish native KVM/WHP execution. Context restore, stack walking and dynamic function tables remain separate work. `WindowsProcessServices.def` declares exact module restrictions: the modeled `kernelbase.dll` lookup returns `ERROR_PROC_NOT_FOUND` (127), matching native observations rather than creating an extra export. [RtlCaptureContext](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtlcapturecontext).

`WindowsProcessSEH` uses shared `X64SEH` in `os/windows/exception/` (`NeverDEmulationWindowsException`, available without drivers) for x64 `__C_specific_handler` and UNWIND_INFO V1. After VEH search it supports filters, finally callbacks, nonlocal handler transfer, nested/collided dispatch and rebased EXE/DLL frames, preserving nonvolatile GPR/XMM state. Filter continuation runs VCH with the same `CONTEXT`. `WindowsSEHTests.cpp` compares 23 original scenarios with native Windows; KVM/WHP/Unicorn share these semantics. Dispatch rechecks image generations, headers, unwind/scope bytes, personality code regions and IAT bindings under the process budget. Changed metadata or unloaded retained images fail explicitly. ARM64 frame SEH, C++ EH, dynamic function tables, general RtlUnwind/NtContinue, and unwinding across loader/VEH/VCH callback boundaries remain unsupported.

For `EXCEPTION_NONCONTINUABLE`, an x64 filter returning `EXCEPTION_CONTINUE_EXECUTION` dispatches `STATUS_NONCONTINUABLE_EXCEPTION` (`0xc0000025`, flags `0x81`, null linked record) with a fresh context. VEH runs again before search restarts on the retained logical stack, preserving finally order and EXE/DLL frame identity under the same depth and execution budgets. The 23 native scenarios include 21 successful executions and two terminations: accepting this secondary exception in VEH/VCH remains unhandled even after restoring the original `CONTEXT`. The model reports that outcome as a runtime failure. Software exception addresses equal their saved PC; internal dispatcher addresses and register layout are model policy. [Windows x64 CI](https://github.com/NeverSight/NeverD/actions/runs/37141166235).

`WindowsDynamicTests.cpp` compares original x64/ARM64 DLLs and EXEs with independent native Windows observations: reference counts, shared dependencies, nested loading, failed attach cleanup, forwarded lookup, process exit, no-entry DLLs and fresh TLS on reload. Additional regressions reject modified loader metadata and stale code pointers, preserve cumulative preparation budgets and keep interrupted API results incomplete. Windows CI requires the native oracle and WHP cases; cross-compilation and Unicorn ARM64 do not establish native ARM64 execution.

A missing library anywhere in a `GetProcAddress` forwarder chain returns error 127; an explicit `LoadLibrary` of a missing catalogue module returns 126. The native oracle and each available backend assert all 41 declared loader scenarios; return after unloading every DLL is observed 16 times per DLL variant on Windows. Failed initialization in a `GetProcAddress` forwarder also returns 127 after cleanup. Process-detach callbacks preserve the exiting caller’s stack contents.

`WindowsExportTests.cpp` uses original x64/ARM64 DLLs and an EXE to check forwarded code/data/ordinal calls, aliases, initializer queries, rebasing, case-sensitive misses, LastError, cyclic and nonresident targets, invalid pointers and metadata changes after successful queries. The same EXE has an independent native Windows oracle; WHP cases are mandatory in native CI. C ABI/CLI tests compare complete reports. Native ARM64 hardware evidence remains pending. Variants with and without EXE exports cover both dependency graphs, PEB-list order, detach order and name/ordinal/null error codes.

`WindowsLifetimeTests.cpp` compares frozen traces with independent native Windows processes and KVM/WHP/Unicorn execution: normal exit, entry return, both DLL initialization failures, four early exits and DLLs without entry points. It separately checks callback faults, shared budgets, relocated TLS fields and aggregate TLS capacity. The native entry-return probe retains the initial thread handle and checks its exit code and exact thread/process notification sequence in 64 repetitions. Remaining child threads are terminated after observation; their process exit is not treated as the entry return value.

`NeverDUnpackTests`, `NeverDUnpackExecutionTests` and `NeverDUnpackPublicTests` cover packed-image recovery; see [unpacking](unpack.md). `UnpackGeneratedTests.cpp` checks the entry rules on x86-64 and ARM64 with a program the test packs itself. `X64ReturnPrefixTests.cpp` checks the two-byte near return on every transport and that every other prefixed return stays rejected. `WindowsDeferredTests.cpp` checks opaque entries and stopped-process observation; `ExecutionSessionTests.cpp` checks execution watches. `DirectX64Tests.cpp` checks partial-page watches, cross-page instructions, one-instruction resumption, service traps, invalid instructions and deadline state on Unicorn/KVM/WHP; native CI requires the matching KVM/WHP cases.

`DirectX64.UnobservedStoresInvalidatePhysicalWriteWitnesses` checks changed, same-value and restored writes without a write watch. Direct execution invalidates old physical write histories conservatively; invalid-instruction and deadline exits do too. Stopped code can create a fresh history, while execution watches and committed-write notifications keep their existing behavior.

`LiveHeapReferencesCannotPublishAnOrdinaryRecoveredImage`, `ExplicitSnapshotsKeepExternalHeapDependenciesVisible` and `ReleasedHeapStateDoesNotBlockRecovery` compare independently compiled startup/entry behavior on available checked and direct backends. `HeapReferencesInCapturedTLSCannotBeDiscarded` covers TLS-only dependencies. Public tests require refusal to preserve existing output files and explicit snapshots to agree across C API and CLI. Address matches remain conservative evidence, not a native-execution certificate.

`DirectServiceBindingsRequireAnExplicitSnapshot` covers direct service bindings first used before and after the captured entry; C API and CLI checks also preserve existing output on rejection.

`PrivateHeapDestructionReleasesOnlyOwnedBlocks` checks moved allocation ownership, cross-heap refusal, retired handles and reuse of live-heap capacity.

`UnpackLibraryTests.cpp` packs independent x64/ARM64 DLLs and checks dependency order, ordinary and generated TLS callbacks, failed attach cleanup, input/host identity, self-file access, export names/ordinals/data/forwarders, and absence of self-imports. Native Windows loads original and rebuilt DLLs through a separate EXE and calls their declared exports; checked and direct WHP cases are mandatory. `CompletedGeneratedTLSCallsRequireTheAttachABI` rejects changed entry/arguments; `GeneratedCallsNeedTheirReturnedStackAtTheContinuation` rejects a wrong return stack. These tests establish unpacking behavior, without devirtualization.

`ExportObserver` also watches executable exports of resident guest dependencies, while modeled providers keep their service-dispatch observation. Input-image exports are excluded. Module changes refresh the watches, and live export identity still authorizes each repair. Discovery retains at most the declared import limit. DLL fixtures require repair of both a system API helper and a guest dependency helper; native loading verifies that neither retains an emulated address.

`WrappedEntriesRequireExplicitTransferEvidence` covers a DLL wrapper calling its restored entry on a deeper stack. The default remains `no_entry`; selecting that observed call with `transfer` rebuilds a loadable DLL. A deeper call alone cannot distinguish an entry from an initializer.

`WindowsDeferred.EarlierTLSCallbackMayGenerateALaterCallback` requires an isolated `.gentls` section with `IMAGE_SCN_CNT_UNINITIALIZED_DATA`, exactly the declared buffer extent, and zero raw-data size and pointer. `WindowsDeferredCases.def` owns the storage and assembly; ordinary `.data` remains separate. Both generated callback and entry cases retain strict rejection and deferred execution checks on x64/ARM64.

`ExtendedRegistersLoadOrdinaryImportsAgain` executes compact and padded R8-R15 import loads through checked and direct x64 execution. Low-register cases cover a preceding REX-shaped byte and CALL-only address helpers. Padded call helpers skip arbitrary bytes after CALL. `ImportCallHelpersCannotDiscardPersistentEffects` requires persistent helper effects to remain observable. `PERebuildTests.cpp` rejects missing start/result evidence and overlapping starts, and preserves the exact API return address for six- to eight-byte call windows.

`OpaqueExportCallsAreRepairedBeforeTheExplicitStop` restores a pure call without bypassing an unknown API. `ExportObservationIncludesTheOpaqueBoundary` covers static, dynamic and ordinal exports without changing execution or service logs. `OpaqueExportObservationPreservesAnUnreadableReturn` requires missing return metadata to remain absent.

`ExportIdentitySurvivesRebindingAndLateResolution` changes opaque-export binding order and resolves an export after entry. Checked/direct x64 cases require the correct API identity and preserve the explicit unsupported-service stop.

Windows virtual memory adds `VirtualAlloc`, `VirtualFree`, `VirtualProtect`, `VirtualQuery` and current-process `FlushInstructionCache`. The OS layer owns reservations; `AddressSpace` remains the authority for committed pages, permissions and backing. Tests cover dynamic code rewriting, access faults and memory-budget reuse.

`WriteProcessMemory` follows the observed x64/ARM64 committed-page contract for current-process writes up to 4 KiB. It preserves each region's protection, copied prefixes, byte counts and LastError, including `ERROR_NOACCESS`, `ERROR_PARTIAL_COPY` and RX-prefix success. `WindowsMemoryWriteTests.cpp` checks all 25 protection pairs; `check_windows_memory_write.py` verifies the same original executable on native Windows CI. Uncommitted destinations remain explicitly unsupported.

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

The repeated `ImmediateSelectPreservesBitmapsCopiesAndProviderPremises` and
`SymbolicFileSystemsKeepOwnSnapshotAndFollowedTargetSeparate` semantic batches
allow 20 seconds per guest run. Their complete CTest cases allow 120 and 240
seconds respectively, covering four select runs or nine filesystem runs.
The complete service and memory assertions still apply to every repetition.
Native cancellation checks retain their short deadlines; production limits,
instruction credit and event credit are unchanged.

## macOS HVF backend

Hypervisor.framework supplies the `hvf` native transport. `auto` selects it for
a matching macOS host ISA; x64 driver execution on Apple Silicon continues to
use Unicorn. The existing ISA and OS contracts remain authoritative. See
[HVF ownership, signing and hardware tests](macos-hvf.md). ARM64 hardware
evidence and Intel runtime coverage are reported separately.

`NeverDHvfTests` authenticates the non-stepped ARM64 maintenance exit separately
from the guest single-step. Fault injection at the end of maintenance covers UDF,
wrong HVC immediates and a correct immediate at a wrong PC. Deadline and stop
tests require an actual native store before cancellation, preserve all caller
registers/vectors at both privileges, then rewrite guest code and retry. A public
CPU test rejects guest branches into the private maintenance area before fetching
or observing those bytes. The HVF inventory also includes `NeverDInstructionFetchTests`
for host/guest writes through aliases, context restoration and permission changes.

### Reproduce checked ARM64 CPU measurements

In a Release CPU build, `neverd-cpu-bench` links directly to the CPU components
and checks every execution result. It emits JSON lines for initialization,
integer/branch loops, RAM, TLS/calls and two alternating CPUs. Initialization is
measured separately; ordinary execution excludes setup and verification. The
two-CPU workload includes the intervening public API calls and checks. These are
checked-execution microbenchmarks, not full OS throughput or cross-ISA comparisons.
Explicit HVF requires an ARM64 macOS host; Unicorn must be enabled for its comparison.

```bash
cmake --build build-cpu --target neverd-cpu-bench --parallel 4
build-cpu/bin/neverd-cpu-bench --backend hvf --samples 7 --warmup 1
build-cpu/bin/neverd-cpu-bench --backend unicorn --samples 7 --warmup 1
```

Preserve a baseline executable **before** rebuilding after a change. The target
statically links NeverD/Unicorn; inspect `otool -L` to confirm its dependencies.
Use Python 3.11+ and independent saved executables for paired measurements:

```bash
python3 scripts/benchmark_cpu.py \
  --baseline /path/to/before --baseline-label BEFORE_COMMIT \
  --candidate /path/to/after --candidate-label AFTER_COMMIT \
  --pairs 15 --output /path/to/new-comparison.json
```

The driver alternates process order, warms each workload, verifies the complete
sample inventory, records binary hashes, labels, load and raw samples, and refuses
to overwrite evidence. Both backends default to HVF; use `--baseline-backend unicorn`
to compare the same candidate binary's software and native transports. Source labels
are caller-supplied; retain compiler/configuration details alongside the results.
Avoid running builds or other tests while measuring. Report spread and paired
results; one host and these workloads do not establish a universal ranking.

Native entry counting is a separate opt-in diagnostic. It adds instrumentation
overhead, so discard its elapsed times. The count includes startup probes.

```bash
cmake --build build-cpu --target neverd-hvf-entry-counter --parallel 4
DYLD_INSERT_LIBRARIES="$PWD/build-cpu/bin/libneverd-hvf-entry-counter.dylib" \
  build-cpu/bin/neverd-cpu-bench --backend hvf --samples 1 --warmup 1
```

The library prints its whole-process `hv_vcpu_run` count to stderr. The timing
driver rejects injected libraries, preventing accidental use of instrumented
timings as normal performance results.

Finite-dispatch regressions cover register and frame phases, both byte orders, reachable invalid arms, later predecessors, exhausted inner guards and adjacent discovery limits. Destination-cap tests cover retained arithmetic correlations, nested loops, decode modes, fallthrough counting, total work limits and legacy precedence. CLI tests execute both C routes and source ABIs at O0/O2; C/Python v8 tests check layouts, invalid fields and ignored future tails. Regressions also keep discovery work available for native guards behind large unrelated finite selectors, and reserve the last permitted refinement for an already nominated producer.

`NeverDLLVMCPhiTests` executes independent and cross-dependent loop updates at O0/O2 over zero-trip loops, iteration boundaries and randomized full-width seeds. Readability assertions require no snapshot locals for independent updates and only the needed snapshot for compound exchanges. Existing branch, switch, moved-arm and swap-loop cases continue to check the selected edge and simultaneous assignment semantics.

`LLVMCInternalExitRegions` adds five independent tests covering both exit polarities, zero iterations, parallel exit/backedge swaps, all four combinations of nested header/internal exits and ordered header/body/latch observations. Shared-destination exits and early continuations verify executable fallback; a valid earlier loop followed by an unsupported region must publish no partial structure. Whole-module and selected-function output must agree without changing LLVM. Original LLVM and emitted C run against independent unsigned oracles at O0/O2 over 294,912 calls, with undefined-behavior traps on C.

`NeverDLLVMCPhiTests` also checks common loop exits with distinct successor PHI pairs, ordered observer calls, output memory and unchanged caller IR. Whole-module and selected-function C execute against independent O0/O2 oracles. Different exit comparisons and extra arm predecessors exercise conservative handling.

`NeverDLLVMCPhiTests` covers multi-backedge loop-expression factoring with independent O0/O2 oracles, ordered observers, memory snapshots before modifying calls, narrow wrap and signed extension. It checks whole-module and selected-function output without changing caller IR, conflicting edges, shared roots, poison annotations, undefined operands, variable shifts, constrained intrinsics, exception functions and complete budget refusal. Rotate calls must collapse only when every incoming operation agrees.

`NeverDLLVMCPhiTests` also executes structured scalar regions at O0/O2: nested loops with shuffled block layout, diamonds, zero iterations, narrow wrap, header observers, PHI swaps, live outer carriers, shared steps and funnel-shift endpoints. It checks source-IR preservation, three-local coalescing, and executable fallback for multi-exit, irreducible and oversized graphs. These are independent synthetic fixtures; source rendering does not certify native recovery.

`LLVMCScalarLoopRecovery.EffectfulGuardLaddersKeepSharedFailureAndEdgeValues`
and `DistinctLoopExitTailsKeepEffectsAndCommonPhis` compare generated C with
independently compiled LLVM at O0/O2 with UBSan traps. They vary branch polarity,
skipped null-pointer reads, aliased memory, indirect observer order, early
returns, shared exit tails and parallel pointer PHIs over 2,048 inputs per
level each. Selected-function and module bodies agree without mutating LLVM.
`NestedOptionalCallsKeepTheirOuterContinuation` retains structured outer
joins around optional effectful calls and distinct early returns, with the
same independent O0/O2 comparison of return values, memory and observer order.

`NeverDLLVMCValueTests` compares typed scalar-loop C directly with independently compiled LLVM at O0/O2, with undefined-behavior traps enabled for the generated C. Boundary and deterministic full-width inputs cover narrow multiplication and wrap before shifts, widened multiplication and right shifts, wide-to-boolean truncation, signed comparisons/extensions, precedence, conditional expressions, boolean arithmetic, unsupported-operation fallback and deep materialized expressions. The tests also assert unchanged caller IR and removal of redundant casts.

`NeverDLLVMCPhiTests` and `NeverDLLVMCValueTests` cover scoped byte counters across increment/decrement wrap, coalesced exit values, inlined uses after a loop, live outer carriers and PHI snapshots. Independent O0/O2 checks with undefined-behavior traps verify compound additions, reversed-subtraction refusal, narrow multiplication and boolean masks. Nested-region tests require loop-local counter declarations and a distinct result carrier without changing source LLVM. An executable naming regression makes external callees collide with the initially generated result/counter identifiers and checks that both calls and observer effects survive.

`NeverDLLVMCValueTests` checks both neutral-select polarities for add, subtract and bitwise updates, unchanged bases, inline old-value dependencies, materialized condition snapshots, narrow truth tests, nonidentity arms and shared selections. Generated C executes against independently compiled LLVM at O0/O2 with undefined-behavior traps. `NeverDLLVMCPhiTests` additionally checks parallel old-value snapshots and branch-dependent initializers that must stay in shared scope. Caller IR remains unchanged.

`NeverDUnicornDecodeTests` checks reserved EVEX register fields on AVX-512/APX CPU models and ROUND memory-fault priority, retained state and resumption. A Linux x64 host probe independently confirms legacy alignment faults and scalar/VEX page faults. These engine tests do not extend checked ISA admission or establish native APX execution.

`NeverDLLVMPrivateFrameTests` checks overlapping writes, all entry paths and loop backedges, retained numeric addresses, aliased external outputs, uninitialized/ordered/unknown memory, metadata and transactional exact/short budgets. Independent O0/O2 oracles compare complete return values, external objects and frame bytes with undefined-behavior traps. Four target compilations cover x86-64, AArch64, big-endian AArch64 and ARM32; they do not imply native recovery on those architectures. Re-run `NeverDByteMemoryForwardingTests` when changing the shared address/effect helpers.

```sh
cmake --build build-release --target NeverDLLVMPrivateFrameTests NeverDByteMemoryForwardingTests --parallel 4
build-release/bin/NeverDLLVMPrivateFrameTests
build-release/bin/NeverDByteMemoryForwardingTests
```

`NeverDByteCellScalarizationTests` covers overlapping words, two entry paths and two backedges, wide and odd-width accesses, both byte orders, retained store choices/poison obligations, partial poison overwrites, complete-use rejection and exact/short budgets across multiple objects. The normal Thin/Deep pipeline must remove the residual arrays. Independent O0/O2 oracles compare all 24 output bytes, surrounding guards and the return value for 8,192 inputs in three versions (49,152 calls), with undefined-behavior traps. x86-64, AArch64, big-endian AArch64 and ARM32 compilation checks are separate from native execution coverage. Run this target together with the byte-forwarding and private-frame targets when changing shared memory contracts.

```sh
cmake --build build-release --target NeverDByteCellScalarizationTests --parallel 4
build-release/bin/NeverDByteCellScalarizationTests
```

`HighControlFlowSemantics.DeepStableContainersPreserveEveryReturnPath` checks 48-level block, loop, switch and exception bodies with an independent interpreter over entered and bypassed paths. A generous runtime bound catches repeated recursive traversal. This is structured HighIR coverage; whole-image method recovery still requires its separate complete inventory and dependency checks.

`AndroidFileTests.cpp` and the original `linux_files.c` fixture exercise O0/O2 code, ordinary/APS2/RELR Android relocation formats and x64/AArch64 Linux processes. Cases cover binary bytes, independent opens, cross-thread/raw/Bionic cursor sharing, errno, lowest descriptor reuse (including standard streams), capacity/error precedence, signed seek boundaries, page-fault prefixes, EOF and page-tail path termination. Dynamic bindings preserve provider identity and closed-provider refusal. `ProcessReportTests.cpp` checks strict catalogue fields, limits, conflicting paths and profile restrictions. C API/Python/CLI checks use the same explicit inputs. Unavailable transports remain skips; these memory-file cases do not establish host filesystem or procfs equivalence.

`AndroidFileStatusAtTests.cpp` checks `fstatat` and `fstatat64`, variadic syscall
and raw SVC against the same explicit observations. Independent full-structure
and byte expectations cover padding, wide timestamps, unaligned and overlapping
path/output buffers, terminated page tails, untouched output on errors, low-word
arguments, descriptor lifetime and unchanged cursors. Unmodeled directory/CWD
metadata, version-specific NULL and synchronization flags, and mixed output
permissions remain pending calls. The shared fixture also runs as O0/O2 x64 and
AArch64 Linux executables; C API, Python and CLI execute its success and missing
path cases. Available native ARM64 transports compare complete reports with
Unicorn, normalizing only backend identity and its selection reason.

Trailing-separator cases distinguish regular files, implicit directories,
root-only slash paths, absent names and file ancestors across open, access,
status and mkdir. They check creation's final-name `EEXIST`, query `ENOTDIR`,
first-NUL faults, flag and descriptor-limit priority, untouched status bytes
and unchanged cursors. Relative, dot-component and internally repeated-slash
forms stay unsupported; directory state and permissions are not inferred.

`AndroidFileSystemStatusTests.cpp` verifies `statfs`/`fstatfs` pathname and
descriptor errors through both Bionic aliases, variadic syscall and raw SVC.
Complete byte comparisons cover invalid and overlapping outputs, trailing
slashes, missing names and file ancestors. Live files with stat metadata,
directories and standard streams remain pending without filesystem observations;
absent catalogue input takes priority over invalid arguments. Dynamic providers
retain identity and closed-provider refusal. Raw fixtures also cover low-word
descriptors, close/reuse, cursor preservation and page-tail NUL/fault ordering on
x64/AArch64 O0/O2 Linux. Available native ARM64 transports compare full reports
with Unicorn, excluding only backend identity and its selection reason. C API,
CLI and Python exercise the same bounded query-error cases.

File-existence cases cover raw x64 `access`, x64/AArch64 `faccessat`, Bionic
imports and variadic syscalls, shared catalogue queries from another guest
thread, private errno, dynamic provider lifetime, implicit directories,
missing paths, file ancestors, invalid-mode precedence, integer high bits,
descriptor exhaustion and unchanged cursors. Path imports include empty,
unmapped, overlong and page-tail strings. Permission checks and noncanonical
paths remain unsupported. Raw syscall extra registers are ignored; the API 28
Bionic wrapper rejects nonzero flags.

Directory cases cover existing files/root/prefix directories, missing parents,
file ancestors, pathname faults, descriptor exhaustion and unchanged cursors
through raw x64/AArch64 traps, named Bionic calls and variadic syscalls. Existing
parents with absent targets remain unsupported. Independent guest threads
retain private errno, and dynamic providers retain identity and lifetime.

Status cases use independently declared x64 and AArch64 `stat` structures, checking
every field, zero padding, canaries, unaligned output, full-width inode/timestamps,
size independent from bytes and unchanged cursors. Raw/Bionic/variadic calls and
another guest thread share observations while keeping errno private. Unknown
metadata, stream identity, both mixed-access directions and closed dynamic
providers remain incomplete; rejected copies preserve the entire backing buffer.
C++ and JSON inputs share range/path validation. Run the clock cases as well when
changing their shared fixed-copy or lossless integer helpers.

The file fixture also takes an imported function’s address while calling it directly. `CallableImportEvidenceBelongsToTheExactSymbol` checks GOT-before-PLT binding across ordinary/APS2/RELR inputs, removes the matching call-slot evidence, changes the declaration to object/function types, and verifies one thunk per admitted symbol. Existing thread-address imports exercise the same linker rule.

Memory-file reads cover empty ranges at the user limit and original-count signed overflow before transfer clamping or EOF. Raw calls and Bionic check address-error precedence, unchanged cursors and errno across failures and successful empty reads.

`SwiftOnceSources.EarlyReturnsKeepExactObjCOnceThunkProofs` covers ARM64/x64 thunks with combined or separate retain calls. `EarlyOnceCopyReturnsRequireTheSameCompleteTail` rejects changed stores, missing or reordered retains, ordered loads, changed results and external entries. `IgnoredNestedReturnCopiesDoNotObserveOnceContext` checks both feasible void callback exits and repeats source-flow validation after projection.

`HighControlFlowSemantics.ReturnTailCopyKeepsTheOuterLabelOwner` compares entered and bypassed paths with an independent interpreter when an address recurs inside a nested block. `ReturnTailCopyIncludesTheFirstChildOfItsLabel` keeps the valid parent/first-child case and its assignment. These focused checks do not replace complete method and native dependency comparisons.

`JumpTailCopyKeepsTheOuterLabelOwner` checks the same ownership rule for jump tails.

`EarlyStringGetterReturnsKeepOnlyInertOnceAnchors` checks empty instruction anchors between the early return and once call, and rejects intervening calls or stores.

`SwiftOnceSources.ObjCThunkRootsShareTheNestedCallbackProof` checks the shared root proof and rejection after a leaf starts observing its context.

`SourceABI.SwiftPointActionRequiresTwoDoublesAndContext` / `ObjCCallHints.CoreGraphicsPointActionsKeepSwiftFloatingCarriers` checks both architectures and rejects changed providers, weak imports, conflicting storage and stale ABI carriers. `HighCSourceCalls.SwiftCoreGraphicsPointActionsKeepCoordinatesContextAndOrder` executes generated C at O0/O2 with independent Swift carrier oracles, checking coordinate bits including signed zero, subnormals and NaNs, receiver identity, call order and guards. These checks establish the call ABI, not complete upper-method recovery.

`NativeSourceHints.CGContextCGRectMethodKeepsOrdinaryAndSwiftContextInputs` checks the complete method tree and exact carriers, including private members and rejected signatures. `SwiftFieldReceiver.CGRectMethodSelfKeepsItsLogicalParameterIdentity` and `CGRectMethodRejectsChangedEntryAndReceiverParameter` exercise the pipeline and publication replay, rejecting a changed self index, entry or argument type. Compiler records cover four macOS/Mac Catalyst targets; the supported entry declaration remains arm64 only. `HighCSourceCalls.SwiftCGRectMethodKeepsContextReceiverAndAllCoordinateBits` executes generated C at O0/O2 against an independent scalar Swift carrier oracle, retaining all four coordinate bit patterns, distinct context/self pointers, one call and storage guards.

`NeverDLowInstructionBoundaryTests` runs the LowIR instruction-provenance suite independently of the aggregate lift fixtures. `BackwardSharedReturnEpilogueKeepsReturnAndCallerFrame` checks both aligned ADD and post-indexed LDP stack releases, including a link-register restore performed in the caller; the original RET X30 and shared entry remain independently represented. `BackwardSharedReturnEpilogueRejectsChangedReturnAndOwnership` rejects another return register, BR X30, missing or unaligned release, narrow restores, interior entries, fixups, writable or ambiguous mappings, relocatable input and another format. Decoding a shared tail does not prove a native ABI: missing caller saves or allocation still fail the existing frame proof.

`NeverDOwnInteriorCallTests` covers direct x86 and x86-64 calls to a label inside the caller's own unwind range, under a Microsoft x64 `.pdata` entry, a System V x86-64 DWARF FDE and an i386 DWARF FDE. A call made only for the return address it pushes is lifted as a push and a jump, and the emitted C for the straight and looped x86-64 cases runs at `-O0` and `-O2` under AddressSanitizer and undefined-behavior traps. A target whose returns pop that call's own return address stays an ordinary call; a return after a stack switch or below the entry stack pointer is refused. A range recovered from an i386 registration chain does not bound the body, so its call stays a call.

Return-slot checks reject overlapping, unknown, atomic and opaque-callee writes before treating a target as an ordinary subroutine. PE, ELF and Mach-O cases cover partial-width PC corruption and i386 widened memory addresses. Private writes, discarded return addresses and terminating branches remain valid.

Mixed exits from a single internal call site retain both outcomes: a checked bare near RET pops its own intact return PC and resumes the exact continuation, while an exit that discards that PC keeps its original control flow. `MixedExitsResumeOnlyTheMatchingInvocation` covers PE/ELF/Mach-O x64 and ELF i386, verifies optimized and unoptimized LLVM, and executes x64 C with sanitizers. Companion tests cover repeated invocations and reject multiple sites sharing a RET, independent or exceptional roots, new predecessors exposed by the continuation, open indirect exits, corrupted PCs, immediate-cleanup returns and language-runtime metadata. Each rebuilt CFG must revalidate the invocation; this does not authorize shared SEH callback returns. Nonlocal exits also prove the parent return slot separately: the outer RET must consume the original entry PC at the exact entry SP. Over-release and writes that corrupt or may alias that PC are refused; disjoint private writes remain valid.

`NeverDSysVCallContractTests` covers x86-64 System V call contracts on the shapes of QtXml's `QDomNode::save` and `QDomNode::isDocument`. A direct callee whose summary reads an argument register receives the caller's value of it, including an incoming `this` passed through untouched, in a Mach-O image as in an ELF one; a virtual call takes the object a dominating block loaded into `RDI`; a method that returns without writing `RAX` on one path while only passing callee results on the others is void; and a byte written to `AL` before a comparison chain is the value returned on every path. The emitted programs run at `-O0` and `-O2` under AddressSanitizer and undefined-behavior traps.

`ObjCCallHints.CIImageAffineValueKeepsProviderAndPhysicalCopyCarrier` checks the CoreImage provider, CIImage factory, complete 48-byte logical record and x2 pointer, rejecting missing or wrong providers, x86_64 and conflicting declarations. `ObjCImageValueCopy.OriginalFrameAndCompleteBodyAuthorizePublication` retains the original call independently of result assignments at the same machine address. `RejectsChangedCopyCallBodyAndCurrentImage` rejects 24 edits to receipts, arguments, stores, frames, metadata, imports, duplicate calls and saved IR, including consistent edits to both MedIR and HighIR. `GeneratedCExecutesAgainstIndependentPhysicalCopyABI` executes unchanged generated C at O0/O2 on ARM64 against a callee accepting the independently compiler-observed x2 pointer, checking all six floating bit patterns, selector/receiver identity, one evaluation, returned object, legal copy writes, unchanged inputs and boundary guards. Other hosts skip this physical ABI execution test.

`ObjCCallHints.CurrentMethodEncodingMustAgreeWithCachedDeclaration` rejects a cached ABI that disagrees with the current nonempty method encoding or selector; declaration-only clients retain their existing contract.

`DarwinIndirectRecordCalls.MatrixFrameEffectsRequireExactCurrentContract` covers the current matrix/affine contracts and 22 rejected contract mutations. `ObjCAffineImageValueCopy.CurrentProducerInitializesThePublishedCopy` proves the SDK result reaches the CoreImage copy and independent publication replay. `RejectsWrongProducerFrameAndSavedIR` checks 12 mutations per producer, including missing input writes, out-of-frame results, wrong providers/carriers and reuse of a consumed concat input. `GeneratedCMatchesOriginalMachineAndSDKResults` executes the unchanged generated C and original ARM64 fixture words against native CoreGraphics at O0/O2 on Apple ARM64: 1000 cases per producer compare all 48 result bytes, two input records, selector/receiver identity, one call, returned objects, private-copy writes and boundary guards. Other hosts skip this native SDK execution test.

`MatrixFrameEffectsRequireExactCurrentContract` also checks the CGRect consumer and its 22 rejected mutations. `ObjCAffineImageValueCopy.CGRectInputUsesTheSameCurrentFrameOwner` verifies rotation → CGRect borrow → rotation reinitialization → CoreImage publication. `CGRectBorrowRejectsExpiredInputsAndChangedABI` rejects eight edits to initialization, input bounds, imports and carriers. `GeneratedCMatchesOriginalMachineAndSDKResults` additionally executes this full sequence at O0/O2 for 1000 cases against the original fixture words and native SDK, checking saved angle, all 48 final bytes, objects and guards.

`FrameMetadataAccessorUsesCurrentCatalogAndABI` checks the shared metadata declaration, both response carriers and current frame-witness publication. `FrameMetadataAccessorRejectsChangedImportAndBytes` rejects weak imports, changed providers/names/addends, private-address requests, partial spills, wrong reloads and changed original calls. The original ARM64/generated-C witness oracle also calls the real Foundation URL metadata accessor: both variants run 2048 cases at O0/O2 with dynamic witness selection, exact output bytes, input preservation, call counts and guards. This coverage does not establish dynamic stack-allocation or witness memory effects.

Structural constant native targets use the existing feasibility-gated scheduler directly. A symbolic singleton retains the incoming predicate only after exhaustive enumeration. Regressions check 128 literal transfers within the straight-line query budget and 32 computed transfers with two enumeration queries per transfer, preserving free high address bits and branch domains. Changed full-state results, missing alignment, zero target limits and short query/instruction budgets refuse. Existing multiple-target and incomplete-enumeration refusals remain required.

A native branch retains its incoming domain on one edge only after a completed UNSAT proof excludes the other edge. Tests verify 32 guarded transfers in both orientations within 512 solver gates, exact/short query budgets and exhausted gates. Changed or missing alignment, reversed comparisons and changed terminal state must refuse; existing arbitrary-control and two-feasible-edge tests remain required.

Bit-blast caches and traversal storage track only reached expression nodes and variables. `NeverDSolverTests` checks late sparse identifiers, context growth between incremental assertions, cached bit reuse, model extraction and changing assumptions. Unrelated wide expressions remain unencoded; reached width violations, malformed roots and exhausted gate budgets still refuse.

`SourceFrameAnalysis.CallStorage*` checks exact occurrences, reaching definitions, initialization, padding, escapes, bounds and cycles without granting a source gate. `ObjCFrameBlockBorrows.*` checks descriptor-bounded synchronous borrows and 19 changed-import, header, ABI and machine cases. These tests retain padding as unproved bytes and reject missing ownership initialization; final block construction, capture reads and callback closure remain separate publication checks.

The block/copy publication cases additionally cover two disjoint 48-byte ranges, descriptor overlap, changed callback bodies, stale machine and IR, detached call occurrences and the exact projection order. `MixedWidthFrameCopiesMeetEveryInitializedByte` and `FrameCoverageCannotHideMissingBytesOrPointerJoins` exercise 8/16-byte stores in both join orders, missing bytes, writable invalidation and pointer identities retained after partial overwrites.

Loop refinement tests cover fixed and changing function-temporary values over arbitrary iteration counts, separate side offsets, paired plans, partial and unaligned ranges, both byte orders and temporary-based ranks. Native composition keeps new storage candidate-only. Missing prefixes, undeclared or undefined bytes, wrong projections, omitted assignments, changed program behavior and inconsistent defined-byte sets must refuse certification. Exact execution, query and observation budgets pass and one-short budgets fail; inference cannot fund a later proof. Lifetimes remain digest-bound. These checks do not establish an ordinary native ABI.

Native loop refinement exercises deferred conditional collection and retained unaudited refusal boundaries under arbitrary iteration counts. Manual and inferred plans must recheck the complete entry and induction domains; live bad arms, changed native updates and exhausted query/instruction budgets refuse certificates. Tests bind changed unreachable boundary bytes, preserve strict defaults and malformed-plan rejection, check both witness policies and combined collection options, and retain static-API and overlapping-instruction refusals. Proof semantic schema 17 binds this admission; ordinary native ABI and source composition remain separate obligations.

`ObjCSuperGetterSources` covers four-carrier CGRect getters, ten publication mutations and mixed Boolean/CGRect callers sharing one machine body. The execution oracle checks exact return bits (including signed zero, infinity and a NaN payload), receiver/class identity and selector loading after the metadata call at O0 and O2. Apple ARM64 runs the original compiler thunk as well as generated C; other hosts exercise the generated C with their native record ABI.

`LowIRLoopInference` covers projected 8-, 24- and 32-bit counters with arbitrary initial upper bits, both directions, registers, frames, function temporaries and both byte orders. Complete self-proofs pass; changed results, stuttering, narrow wraparound and skipped equality exits refuse. Exact operation, query, path, rank-candidate and widening budgets pass and one-short budgets fail. Final proof operation, query and observation limits are checked separately.

`ObjCCallHints.SDKRecordData*` checks both external records, every double offset, both Darwin architectures and provider aliases, plus changed imports, weak linkage, missing libraries, conflicting fixups, writable storage and incomplete ranges. `python3 -m unittest scripts.tests.test_generate_darwin_record_data_declarations scripts.tests.test_generate_darwin_data_declarations` checks profile conflicts, alternative layouts, invalid sizes/alignment, TLS and architecture-specific exports. Reproduce the record catalog with `generate_darwin_record_data_declarations.py` using the pinned SDK, libclang, output path and `--check`; this declaration check does not establish native indirect-result initialization or method recovery.

`LowIRLoopInference.ProjectedBounds*` covers narrow equality exits with arbitrary upper bits in counters and bounds, 8/24/32-bit lanes, all three storage kinds and both byte orders. Complete proofs reject changed bounds, stuttering, skipped exits, wrapping and changes to observed upper input bytes. Inference and proof budgets remain separate, with exact and one-short checks.

`LowIRLoopInference.LateCounter*` covers constant initialization that reveals 8/24/32-bit lanes only after generalization, across registers, frames, function temporaries, both byte orders and arbitrary upper bound bits. Complete proofs pass for equality exits and reject stuttering, skipped exits, changing bounds and observed upper-byte mutations. Exact and one-short inference and final-proof budgets remain separate.

`LowIRLoopInference.ProjectedComparisonBits*` covers cached narrow equality at loop headers with arbitrary upper bound bits: 8/24/32-bit counters, three storage kinds, both byte orders and constant or padded initialization. Complete proofs reject changed cache values, observable upper bytes, moving bounds, stuttering and skipped exits. Exact and one-short inference and final-proof budgets are checked independently.

`LowIRLoopInference.OrderedComparisonBits*` covers cached unsigned-order exits in both Boolean encodings across 8/24/32-bit counters, three storage kinds and both byte orders. Full proofs reject nonterminating updates, changed comparisons, moving bounds and observed upper-byte mutations. Exact and one-short inference/proof budgets are separate. Fixtures that change their Boolean encoding rebind the original operation digest before proof.

`LowIRLoopInference.MutablePrefixBounds*` covers derived 8/24/32-bit bounds with changing upper bits, three counter storage kinds, both byte orders and direct/cached exits. It checks nontermination, moving equality bounds, observed upper-bit/cache mutations and exact/one-short budgets. A moving unsigned-order bound can terminate on wraparound and has its own complete proof regression.

`LowIRLoopInference.SharedTemplatesFitIndependentOperationBudgets` proves nested partial counters within 1,024 inference operations and 640 independent proof operations. Exact pure expressions and immutable prefix reads are shared only within one cut rebuild, retaining operand identity, output widths and location spaces. Tests observe full counter and bound words, reject a changed prefix computation and refuse one-short operation budgets. Existing register, frame, function-temporary, byte-order and nontermination cases remain required.

`LowIRLoopInference.CompletedEntailments*` checks session isolation across terminating and nonterminating frame loops in both byte orders, solver/node exhaustion and independent proof budgets. Mutable-bound regressions verify exact and one-short logical query limits with cache hits; native repeated-context proofs check domain-sensitive reuse.

`LowIRLoopInference.IncrementalEntailmentsKeepRollingBudgets` checks encoder reuse within one constraint domain. Switching domains discards the encoder; exhausting accumulated gate capacity retries once with a fresh encoder and charges another query. Full counter and bound words remain observed in both byte orders, with exact and one-short query limits, gate/width/search refusal, and independent final-proof budgets.

`LowIRLoopInference.RebuiltCounterLanes*` covers exact projected updates when other counter bits are reconstructed from prefix values or change independently. Register, frame and function-temporary cases span both byte orders, 1/3/4-byte counters and direct/cached exits, with complete counter, bound and tag observations. Removing an observed high tag, nonterminating updates, missing exit guards and one-short inference/proof budgets must still be refused. Structural recurrence matching only proposes widening and ranks; complete transition and final proofs remain required. Fixed operation and query budgets also cover symbolic projected counters without expanding incidental constant-prefix matches into relations.

`LowIRLoopInference.ProjectedCounterCopies*` covers nested loops that copy a counter lane through a separately tagged word before incrementing it. The 144 complete-state cases span registers, frames, function temporaries, both byte orders, 1/3/4-byte lanes, direct/cached exits and whole-word controls. Projected equalities must hold on saved arrivals and every incoming transition; high bits remain independent. Transition-domain entailment can identify an additive recurrence through distinct parameters. Dropped high tags, invalid updates, missing guards and one-short inference/final-proof budgets still refuse.

`LowIRTransferredCounterState.*` and `LowIRLoopInference.TransferredCountersKeepProofBoundaries` cover a counter that moves between storage locations at different cuts while another loop can bypass each cut. Exact symbolic unit transfers propose source-counter guards and a fallback rank with one alternate cut location; every guard and rank still needs full transition proof. The 192 complete-state cases span registers, frames, function temporaries, both byte orders, 1/3/4/8-byte counters, independent tags and stationary controls. Changed results/tags, invalid rank maps, nonterminating updates, missing guards and short inference/final-proof budgets refuse. Counter traversal uses the existing symbolic-node allowance; optional graph-selector work can remain zero. Cases include decrementing to zero and incrementing to an input bound. When a new counter is discovered, bound and lane-guard pruning waits until every cut template has been rebuilt; ordinary widening and independent proof still run. The complete-state matrix has twelve named storage/direction/transfer groups with sixteen combinations each; CTest reports and bounds each group separately.

`LowIRLoopRefinement.GuardedCuts*` and `BinaryLowIRLoopRefinement.GuardedCuts*` cover repeated PCs, register/frame/profiled-system selectors, both byte orders, unmatched finite and cyclic paths, overlap and wrong-side refusal, prefix generalization, selected undefined-value witnesses, malformed metadata, digest binding and shared budgets. Independent native tests prove both R10 contexts at one original loop address and keep unaudited boundaries ahead of selectors. Ordinary ABI certification remains separate.

`BinaryLowIRLoopInference.NativeSelectors*` covers two register contexts, frame-only contexts, three-domain conjunctions, inseparable templates, origin/native-body mutations and exact/one-short independent inference and proof budgets. The tests use arbitrary loop counts and introduce no entry constants.

`NativeSelectorsGeneralize*` checks automatic recovery and complete native proofs for alternating register and frame phases, including byte masks with symbolic upper bits. `NativeSelectorState*` checks wrong rank/body, outside-mask corruption, malformed assignments and exact/one-short work and metadata budgets. Explicit valid plans additionally compose the production builder with complete native checks before and after mixed overlap/disjoint cuts, retain original-prefix provenance, charge fallback scans and reject temporary overflow. These explicit-plan checks are distinct from automatic inference coverage.

`DarwinIndirectRecordCalls` checks the current MakeScale contract and its 22 import/ABI mutations, then consumes a complete 48-byte private result through the shared by-value-copy proof. Misaligned, displaced, overlapping or out-of-frame result ranges refuse. Removing the definite-write effect also refuses, even with the complete return ABI retained.

`SourceFrameAnalysis.IncomingResultAddressNeedsCompleteEntryIdentity` rejects ten entry/carrier/write mutations and a missing entry ABI. `NativeSourceHints.IndirectResultTailCallRetainsExplicitOutputAddress` re-lifts a direct tail and checks the explicit output parameter, six stores and publication gate.

`NativeSourceHints.FourDoubleCallerDemandNeedsEveryUnchangedCarrier` checks all four low lanes, independent upper-lane writes and nine declaration/control mutations. `FourDoubleReturnRequiresEveryComputedLowLane` rejects twelve incomplete-result or stale-contract cases. `DarwinNativeRecordReturns.FourComputedDoublesExecuteAtO0AndO2` compares all 32 result bytes with an independent arithmetic oracle for 2048 cases at each optimization level.

`DarwinIndirectRecordCalls.AffineInvertSnapshotsItsCompleteAliasedInput` runs 2560 cases at O0 and O2, including equal, overlapping and disjoint input/output placements. It checks input bit patterns, one call, all 48 result bytes and the entire guarded storage. This is a physical-copy/snapshot oracle, not original-machine or native SDK execution. The current matrix/affine contract test retains 22 rejected mutations per contract.

`DarwinIndirectRecordCalls.AffineTranslatePreservesScalarBitsAndSnapshotsAliasedInput` runs 2560 cases at each of O0 and O2, checking both scalar bit patterns, all six input fields, one call, complete output bytes and guarded storage across equal, overlapping and disjoint placements. Four scalar ABI mutations and the shared 22 import/ABI mutations refuse. The bitwise stub checks physical argument and snapshot behavior; it is not a mathematical translation oracle or original-machine execution.

`NativeFloatingReturnProof.HFAResultFieldsNeedTheExactCompleteDefinedCall` covers nine field selections and seventeen rejected call, carrier, width, offset or SSA mutations. `HFAFieldExtractionNeedsADominatingCall` rejects a sibling-path producer. `NativeSourceHints.HFAFieldTypeRequiresCurrentCallAndFrameProofForPublication` lifts a five-instruction ARM64 caller, re-lifts the inferred scalar result and checks the source gate; a wrong provider or missing LR/SP restoration refuses. These checks establish source type/projection behavior, not execution of the original machine body.

Explicit CPU0 preemption, clock semantics and current limits are described in [driver scheduling](driver-scheduling.md).

`SwiftOnceSources.FoldedObjCGetterTailsExecuteOnceAndRetainsAtO0AndO2` folds actual ARM64/x64 return tails, with combined or separate retain calls and standalone or inlined predicates, then executes emitted C against runtime stubs at O0/O2. It checks result bits, one initialization, call order and a changed cached value. `FoldedObjCGetterTailsRevalidateCurrentStorageAndCalls` refuses width, ordering, intrinsic, storage, result, call and current-import mutations even with a saved plan. These are controlled source/runtime-stub checks, not original WMF machine or native Swift runtime execution.

`SourceFrameAnalysis.CompleteOutput*` covers complete and short prefixes, every-return joins, SDK tail writes, missing bytes, pointer escapes and carrier restrictions. Caller cases reject missing or short certificates, misalignment, frame-boundary and saved-register overlap, aliases, later write invalidation and live opaque-value overlap. `NativeSourceHints.CompleteNativeOutput*` replays assembled ARM64 producers and consumers, requires the complete SDK input range and rejects stale code, CFG, ABI, audit, provider and call-occurrence evidence. These are byte-initialization and source-gate checks; they do not execute the original machine body or certify a native logical return.

`MedCallingConvValueFlow.FPInputsFollowOnlyAuthenticatedCallPrefixes` / `NativeSourceHints.PreservedFPPrefixesRetainAllEntryInputsAfterSDKCalls`: The preserved-prefix regressions check three valid reads and nineteen refused carrier, prefix, ownership and unused-value cases. An assembled ARM64 caller retains all four incoming double lanes across an SDK call; fresh lifting checks its complete parameter ABI and source gate. The unchanged accepted WMF `0x36350` method also passes 2048 cases at each of O0 and O2 against an independent native CoreGraphics flip/translate/normalize/concat/apply expression, checking all 32 result bytes, receiver/selector and input guards, including zero, negative, infinite and NaN dimensions. The original WMF machine body is not executed.

`SourceABI.SwiftEntryCapturesAndReturnsTheErrorRegisterOnEveryPath` checks ARM64/x64 entry capture, both return paths and rejection of missing or changed transport proofs. `NativeSwiftCallsKeepBothResultsAndPostCallErrorBranches` retains the call result, updated error register and following conditional, including private-name collisions and both emission orders. Generated C executes at O0/O2 on the native Darwin target against independent success/failure oracles. `SwiftFunctionSymbols.RegularExpressionInitializerRetainsContextAndError` rejects aliases, changed complete symbols, non-code entries and unsupported image formats. These checks do not execute the original WMF constructor or establish complete upper-method recovery.

`NativeSourceHints.SwiftErrorDeclaration*` re-lifts assembled ARM64/x64 entries after replacing an observed scalar ABI with the compiler declaration. Missing or incomplete current audits prevent replacement; explicit source contracts in options, MedIR or HighIR retain their authority. The entry tests also reject a missing MedIR error-output marker or an incorrect operand width before HighIR conversion.

The entry capture remains a liveness root when every path overwrites the error register, including final source cleanup after once binding. `NativeSourceHints.SwiftErrorCallsRequireCurrentDirectNativeProjection` rejects absent, null, changed or unproved callees and changed targets, indirect calls, incomplete results, missing operands and incompatible effects.


`SwiftFunctionSymbols.RepeatedDeclarationsKeepEveryRecordField` checks identical duplicate records and rejects changed names, sizes, boundary provenance or origins. `NativeSourceHints.SwiftErrorCallResults*` exercises automatic ARM64/x64 caller inference and rejects missing current callees, altered machine operations, stale audits, incomplete ABIs and missing, narrowed or unrelated result extracts. Entry source execution also covers conflicting optional debug declarations while retaining the bound convention and error/context roles.

The Swift witness generators verify both `CurrentValueSubject: Publisher` and `Range<Bound: Comparable>: RangeExpression` on ARM64/x86-64 macOS and Mac Catalyst. `scripts.tests.test_generate_swift_witness_contracts` rejects changed generic inputs, metadata response types or members, prototypes, export providers and incomplete flow. `ObjCSourceBindings.SwiftWitnessUndefRequiresExactDescriptorContract` and `SwiftWitnessUndefRejectsUnprovedInputAndABI` check both descriptors on both architectures, including 33 mutations per descriptor and architecture for runtime/import identity, weak or conflicting storage, ABI and effects. These catalogs grant no frame layout or borrowing contract.

`scripts.tests.test_generate_swift_data_declarations` checks the complete `String.Index` descriptor query and rejects changed symbolic cells, recipe bytes or lengths, metadata/cache flow, runtime ABI and duplicate or missing definitions. `ObjCSourceBindings.SwiftRangeIndexDescriptorKeepsItsCompleteRecipe` checks the nonleading descriptor at offset 3 on both architectures; `SwiftRangeIndexDescriptorRejectsStaleIdentityAndRecipe` rejects 20 mutations per architecture and revalidates previously published address hints and helper emission.

`ObjCSourceBindings.PrivateFramePointerTailRequiresExactStoreOnEveryPath` covers cyclic object locals after PHI coalescing and a private-frame seed through the same cycle. The object cycle preserves an exact pointer spill; the frame-carrying cycle, partial clobbers and unknown frame escapes reject it.

`ObjCCallHints.FoundationGenericNSRangeKeepsSixPointersAndTwoWords` checks both architectures and exported providers, all argument and result carriers, and rejects weak imports, addends, foreign providers, stale symbols and invented borrowing effects.

The string witness reader in `scripts.tests.test_generate_swift_witness_contracts` checks complete cache/query flow, rejects 28 storage, ABI and flow mutations plus seven ambiguous declarations, and enforces its input budget. The existing Swift witness binding tests cover all four descriptors on both architectures, including 33 mutations per descriptor and architecture. The descriptor identity supplies no frame layout or borrowing permission.

`PreparedFiniteKeys.*` checks context destruction and renaming, projection order and limits, explicit invalidation after moves, malformed and incomplete results, empty and nonunique domains, and exact capacity boundaries. Existing cache and frame regressions also cover the prepared-key route.

`SourceABI.SwiftPointTransformKeepsTwoFloatingInputsAndResults` rejects changed carriers, layouts, context roles and indirect results on both architectures. `SourceABI.SwiftPointForwardingPreservesBothIEEECarriers` compiles and runs forwarded source at -O0/-O2, checking signed zero, subnormals, infinities and NaN payloads in both fields. The declaration test accepts compiler and named-argument forms and rejects altered method signatures and ambiguous image identities.

The MainActor fixture checks the complete fixed metadata/static-table flow and rejects changed storage, ABI, metadata extraction, table identity, extra effects, missing or duplicate declarations and exhausted input budgets. Both catalogs require all three paired SDK exports in every target. Witness binding tests cover all four descriptors on arm64/x64 with 33 input, import and ABI mutations per descriptor and architecture; the host runtime oracle compares the public table for five instantiation-argument bit patterns.

`BitVectorEncodingClone.WatchMigrationAndGrowthOutliveTheSource` grows literal tables and shared watch lists after copying and destroying the source, mutates a sibling independently, interrupts propagation at one watch visit, and checks resumed full models against original clauses and independent Boolean relations.

`ObjCCallHints.SwiftPublishedAccessorsKeepOpaqueValueAndAllKeyPaths` verifies both accessors on ARM64/x86-64 and both canonical Combine providers, rejecting nine ABI mutations and eight import-identity mutations for each combination. Independent SDK validation executes generated C for both source architecture configurations at O0/O2 on ARM64, comparing all 24 payload bytes, input/output guards and two owner identities over 128 accessor calls. Eight cross-compilation configurations check both architectures on macOS and Mac Catalyst. The runtime oracle preserves the setter’s consumed references; it grants no production borrowing or ownership shortcut.

`ObjCCallHints.SwiftMainActorSharedKeepsObjectAndMetatypeContext` verifies the complete result and swiftself carriers on ARM64/x86-64, rejecting ten ABI mutations and ten import-identity mutations per architecture. Independent SDK validation executes unchanged generated C for both source architecture configurations at O0/O2 on an ARM64 host: 128 calls preserve singleton and metatype identity with balanced reference ownership. Eight cross-compilation configurations cover macOS and Mac Catalyst on both architectures; native x86-64 execution remains separate coverage.

`BitVectorEncodingClone.RootQueuePreservesDecisionsAcrossGrowthAndBudgets` checks mixed root and undecided variables, nondecision roots, copying a copy, destroyed sources, later variable growth, both default phases, bounded interruption and resume, conflicts and restarts. Complete models and all search counters must match a fresh encoding.

`ContextFiniteProofs.*` checks context and owner isolation, owner replacement, token moves, exact predicates and ordered projections, append-only growth, completed and incomplete results, storage ceilings and LRU eviction. Frame tests require the final uniqueness query before caching and preserve symbolic-node limits on hits.

`CompletedQueryCache.*` checks full byte-domain answers, every packed slot, growth, context and owner isolation, invalid and incomplete inputs, and exact storage limits. Native branch regressions retain fixed logical query costs and exact/one-short budgets even when complete answers avoid backend work.

`BinaryLowIRRefinement.NativeTargetDomainsKeepIndependentProjections` / `FrameOffsets.FrameAndJointTargetProjectionsKeepIndependentSearches` check repeated native target chains with branch changes and symbolic frame stores, fixed logical costs, exact/one-short query budgets, invalid target limits, gate exhaustion and wrong terminal observations. Interleaved frame and correlated target projections also preserve complete tuples, observer order and incomplete-result refusal across predicate replacement.

`LinuxPriorityTests.cpp` checks explicit task state, thread isolation, missing
observations, malformed JSON, profile admission and refusal effects.
Independent x64/AArch64 raw callers at O0/O2 verify nice clamping, 32-bit syscall
argument narrowing, CAP_SYS_NICE/RLIMIT_NICE permission boundaries, and kernel
getpriority encoding. Run the `LinuxPriority.*` and
`Backends/LinuxPriorityProcess.*` cases in `NeverDLinuxProcessTests`, then the
complete Linux process, Android native and process public suites for shared
kernel/JSON changes. Absent optional native transports remain explicit skips.

`AndroidThread.NiceState*` executes independent O0/O2 pthread callers with all
three relocation encodings. A gated child observes the parent's nice value at
creation despite a later parent change; its own grandchild inherits the child's
updated value. Raw traps and Bionic `syscall` check independent changes, denied
priority increases, and ESRCH after child exit. Missing parent observations stay
unmodeled. `LinuxPriority` unit tests also check rollback, identity collisions,
unknown versus exited tasks and lifetime-capacity refusal without mutation.

`LinuxKernelAvailability.*` validates explicit absence inputs and profile
admission. `Backends/LinuxKernelProcess.*` uses independent x64/AArch64 O0/O2
raw callers to verify ENOSYS before argument validation and continued refusal
for unspecified or unrelated calls. The Android syscall fixture compares raw
SVC with Bionic `syscall`, preserving distinct raw return/errno effects.
Run these focused tests, the full Linux process and public process suites,
and Android syscall, native-entry and signal suites for availability changes.

`AndroidTestExecution.def` gives `FiniteRegistryRejectsBeforeSuccessAndCanBeReused` an aggregate CTest timeout of 120 seconds and `RUN_SERIAL`. Each of its two workloads retains its own finite 30-second runtime budget; serialization prevents capacity stress cases from timing out through contention. All six O0/O2 and relocation variants use this policy.

`LinuxPIDFD.*` checks released GKI branch parsing and rejects invalid enum values
or an absent-pidfd observation combined with GKI before image loading. It also
rejects malformed, excessive and contradictory task catalogues before loading.
`Backends/LinuxPIDFDProcess.*` runs independent O0/O2 x64/AArch64 callers for all
eight branches, including flag differences, shared file/pidfd allocation,
limits, close/reuse, closed task lookup, versioned nonleader errors and
scalar/vector error ordering. Task cases test missing targets and nonleaders
before FD exhaustion, thread flags and an empty catalogue with implicit self.
The versioned vector cases
contrast an early negative length with inaccessible later metadata, and a single
buffer whose original extent crosses the user limit while its capped extent fits.
They check pidfds and both captured streams, with O0/O2 callers. Android's
`ReleasedGKIProcessDescriptorsShareRawAndBionicOwnership` cases repeat the
shared descriptor and errno behavior across all six compiled relocation
profiles; `ReleasedGKIVectorImportRetainsRawAndBionicErrors` repeats the vector
ordering and cap differences through raw and Bionic transports.
`ReleasedGKICatalogueRetainsRawAndBionicLookupErrors` repeats the catalogue
and exhaustion cases while checking raw errors and Bionic's preserved errno.
`ProcessCPUClocksRetainIdentityAndIdleSeparation` and
`ProcessCPUClocksKeepMissingObservationBoundaries` check encoded CPU identities,
unknown observations, target validation, aliases and idle behavior. The
`LinuxClock.ProcessCPUObservationsShareAliasesAndRemainFixedWhileIdle` unit case
checks the shared observation owner, and input cases reject aliases,
unobserved/nonleader targets and negative CPU time before loading.
`ZeroTimeoutPollRetainsReadinessAndOrderedCopies` executes O0/O2 raw callers
for every released GKI branch. It checks live/negative/closed entries, duplicate
ready counts, argument narrowing, timeout/mask ordering, read-only zero
timespecs, metadata-before-readiness admission and earlier revents before a
later write fault. `ZeroTimeoutPollKeepsUnobservedBoundaries` retains unknown
kernel, limit, masks, waits and descriptor readiness. Android's
`ReleasedGKIZeroTimeoutPollSharesRawAndBionicResults` checks the same table and
errno ownership across all six packing profiles.
`MemoryFileOpenFlagsRetainObservedPathErrors` checks both raw architectures
and all released GKI branches for directory/direct bit normalization, known
lookup failures, pathname-before-capacity ordering and unchanged cursors.
`ReleasedGKIOpenFlagsSharePathErrorsAndErrno` repeats raw and the four Bionic
open imports across packing profiles. The corresponding boundary cases keep
existing directories and unobserved direct I/O unsupported.
`ReleasedGKIXAttrsPreserveNameAndTargetErrorOrder` checks O0/O2 callers on all
eight releases for versioned name/target precedence, empty and overlong names,
page-edge imports, descriptor narrowing, unchanged cursors and value canaries.
Android’s `ReleasedGKIXAttrsShareRawAndBionicErrorOrder` repeats raw/named calls
and independent errno checks across packing profiles. Corresponding boundary
cases retain missing GKI and existing-object attribute uncertainty.
Run these first,
then the complete Linux process, Android native and
public process suites when changing shared kernel or descriptor semantics.
These tests execute the model; they do not boot the eight pinned GKI kernels.
See [released GKI contracts](android-gki-kernels.md).

`FrameOffsets.Cached*` covers translated addresses with arbitrary high root bits, unsigned wrap, sum-shape changes, both cache modes, predicate separation, zero capacity, query/node budget refusals, and distinct empty/nonunique domains. Cold requests retain complete solver proofs; later translations may use an already completed proof with no remaining query budget.


### Explicit Darwin extended-attribute reads

`DarwinFileTest.Xattr*` exercises both4096/16384-byte pages, full/short/query buffers, name and carrier bounds, complete ordered list prefixes, aliases and inaccessible tails, transport/budget errors, CWD/link policy, dup/removal/name reuse, independent stat validity and content invalidation. `DarwinFileOptions.Xattr*` bounds names/count/bytes and implicit-directory entry costs; `ProcessReport.DarwinXattr*` rejects malformed records without losing UTF8/order/opaque bytes. `DarwinProcess.ExtendedAttributesPreserveValuesNamesAndObjectLifetime` is required for every available ARM64 HVF profile. The three original modes also run through C/CLI and the unchanged Python SDK integration method across five thin profiles.

`extended-attributes` is the only new native case. The private runner seeds ordinary attributes on its own file after resetting bytes, then the original raw-call workload derives list boundaries from the provider’s actual names, including automatic provenance. Literal and unknown modes are virtual-only. Native5s, newguest/Python5,000,000us/quantum1024 and public10s remain unchanged. Negative buffers on an explicitly empty list are unsupported because the private clear operation never produced a verified native empty list. Native Intel/physical iOS remain unverified. Root-only serial epochs retain source, products, controller inputs, failures, timeout captures and reap evidence.

## Bounded bulk directory attributes

The bulk-attributes workload checks whole groups, name/type membership, guarded unused bytes, low32 FD, bitmap words, native errors, dup/shared progress, independent opens, cached EOF and zero rewind. Literal and unknown modes are virtual-only. Model tests also cover full stat, invalidation, NFD/255-byte names, request/output aliasing, transport/budget failures, held moves/SWAP/removal/reuse and explicit authorization. Required native inventory is 65 workloads per platform: 195 matching ARM64 cases and 130 Intel cases. Only matching ARM64 HVF execution is locally verified. Native5s, guest/Python5,000,000us/quantum1024 and public10s remain unchanged.


## Darwin ordinary attribute mutations

`XattrMutation*` model tests check both4KiB/16KiB pages, guarded literal values, low carriers, import/existence precedence, permission separation, name/value aliases, transport and budget rollback, reserved initial slots, shared growth and orphan/mapping lifetime. Strict JSON tests cover all three initial object kinds. The original `xattr-mutations`, `xattr-mutations-values` and `xattr-mutations-unsupported` workload modes cover all five thin Mach-O profiles through guest, C, CLI and the complete Python method. Native inventories retain every existing case and require the new workload on each available matching transport. Native Intel/physical iOS and complete Apple frameworks are separate unavailable coverage.

`MaterializedRuntimePreservesOwnedObjectsOnNativeWindows` checks original modeled execution, restoration, section permissions and original/restored native Windows execution. It covers private-heap reallocation/free, encoded interior pointers, FLS callback rearming, recursive locks, LastError and reserved/committed/protected virtual pages. `MaterializationRequiresKnownSupportedState` rejects absent version inputs and dynamic TLS. `RuntimeRestorationHasTheSameCAPIAndCLIContract` compares exact bytes and reports. Linux construction checks and Wine observations do not replace native Windows lifecycle evidence.

`NeverDUnpackDriverTests` covers packed DriverEntry recovery, static/dynamic kernel imports, retained kernel resources, entry ABI/control state, malformed exports, scheduling, request/unload lifecycle, C API/CLI parity and PE checksums. The native driver inventory requires matching KVM/WHP cases; Windows also checks ImageHlp. This does not certify a native kernel load. See [unpacking](unpack.md).

`DriverEntryCaller.*` checks nonzero incoming values for all eight nonvolatile
GPRs, both halves of XMM6–15, volatile MXCSR status versus preserved controls,
FPCSR and the returned direction flag. The independent machine-code control
`UnpackDriver.CheckedDriverEntryRejectsAClobberedCallerBeforePublishingStatus`
keeps a balanced stack and STATUS_SUCCESS while corrupting R13; its companion
restores R13. Checked execution must reject the corruption before publishing
NTSTATUS or scheduling lifecycle work. Matching KVM/WHP cases are required.

`NeverDKernelSnapshotTests` checks diagnostic object snapshots after terminal memory faults and unsupported instructions. Committed device and dispatch state remains observable without clearing the first fault or permitting further CPU access. Corrupted ownership, cyclic device lists, invalid driver extensions and partial dispatch pointers still fail. Native KVM/WHP acceptance requires the matching backend cases.

`KernelUnpackStateTests.cpp` adds nine portable cases to `NeverDKernelSnapshotTests` covering pool allocation contracts, copied contents, released lifetimes, missing backing and remaining dependencies. Terminal-fault cases also capture live pools. `UnpackDriver.StoppedViewOwnsLivePoolContentsAndContracts` verifies the public observer state after session destruction. These cases are mandatory in the native driver gate and do not certify native kernel restoration.

The committed-write observer regressions verify visibility before the next instruction, exact instruction accounting without write logs, and propagation of observer failures. Both legacy and checked contracts are mandatory on matching KVM/WHP backends.

`UnpackDriver.CommittedWriteWatchesSuspendBeforeTheNextInstruction`; `UnpackDriver.CommittedWriteObserverFailuresRemainAPIErrors`.

`scripts/check_windows_nonlocal_finally.py` compiles and executes original MSVC x86/x64 programs at O0/O2 with GS enabled and disabled. Its six cases distinguish ordinary and unwinding finally invocations, conditional jumps to the parent, and returns that stop unwinding. Repeated observations check local guards and the x86 registration chain; a deliberately changed cleanup trace must fail the original oracle. The Windows EH fixture job retains the source/image hashes, compiler output, linker maps and runtime observations. Run it in the matching MSVC environment with `--arch x86` or `--arch x64` and `--output`. This supplies an original-runtime baseline, not evidence that NeverD supports these nonlocal transitions. Four additional cases raise another exception immediately after parent resumption, and two exercise a scalar input plus a guarded caller-owned counter. The x86 chain is checked after every call and at the resumed label. Schema 2 observations are required; an older final-chain-only record cannot establish this coverage.

`scripts/check_windows_native_seh_resume.py` separately tests an experimental Windows x64 LLVM/CRT continuation at O0/O2. `_local_unwind` receives the captured physical SP and an address-taken parent continuation, separate from escaped source cells. The target is the ordinary call continuation, which reads an explicit escaped resume token before selecting the suffix. A bare address-taken join can acquire predecessor-only PHIs or a sunk suffix under O2; address retention alone does not authenticate hidden entry into LLVM control flow. A MASM caller checks eight preserved GPRs, XMM6–XMM15, returned SP and guards. Normal paths execute the call suffix; abandoning paths skip it, and every path raises a second exception. A later ordinary finally call in the same parent activation must execute its own suffix after reinitializing the token. These fixed-frame observations do not authenticate arbitrary LLVM live values at hidden entry or EHCONT/CET compatibility. Reports retain toolchains and hashes. Native success grants no NeverD reconstruction receipt and does not solve PE32 exits, escaped-memory authority, arbitrary nesting or the two C routes. Use `--output` in the native x64 MSVC environment. The `windows_eh_protocol_only` workflow input runs these native experiments without rebuilding NeverD; it is separate from engine regression coverage.

`scripts/check_windows_native_seh_bridge.py` adds a separate fixed-frame x64 experiment: assembly owns both continuation labels and unwind metadata, then calls optimized LLVM through ordinary ABI entries. O0/O2 each exercise eight paths over 256 iterations, including a second nonlocal call. Independent observations retain each raw AbnormalTermination byte, exception-event counts, actual codes and a persistent unexpected-event flag. Six compiled negative controls cover exclusive scope ends, unwind flags, repeated suffixes, colliding abnormal bytes, overwritten exception observations and shifted physical frames. Reports hash every compiled source, retain object/final-image unwind and load-config output, and capture the actually resolved CRT providers. This is feasibility evidence only; source-memory authority, arbitrary nesting, concurrency, EHCONT/CET and production reconstruction remain unproved.

`RegistrationTryLevel.EH4Publication*` and `EH4CookieStoresMustOwnDistinctPrivateBytes` check direct PE32 EH4 frames with ADD/SUB allocation, saved registers, pushed cookies and disp8/disp32 cookie stores. Changed sources, offsets, overlapping cookies and writes into saved registers or runtime administration must be rejected; shared LowIR independently verifies values and lifetime.

`RegistrationSEHControl` checks actual immutable PE32 near-call/return bytes and
complete folded return-jump paths, including short and near jumps. Far controls,
writable or partial code, changed targets, cycles and incomplete jump prefixes
must not provide physical continuation evidence. `RegistrationState` exercises
ordinary finally returns and SavedESP parent jumps independently, including
unowned callback interiors, CALL blocks that include a tail jump, and folded
continuation receipts. The SavedESP-copy cases require the exact adjacent load
producer, source slot, width and micro-operation sequence. Changed temporaries,
sources or address spaces cannot supply a restore. The WAIT cases retain the
original intrinsic and dispatch eligibility in both invocation analysis and
returning-body inspection; altered identities or operand shapes refuse. Budget
exhaustion must withdraw exit completeness and any published exit records.

`RegistrationState.SEHRuntimeFieldsBelongToTheSearchFilterInvocation` checks
the EH3 runtime slot, record link, exception code/flags and a private pointer
spill. Wrong fields, partial pointers, altered widths, FS accesses, writes,
ordinary roots, opaque calls, changed instruction slices, guards and ordering
refuse. The source micro-operation sequence may restart at each instruction;
it must not be confused with the operation's block index. The lifetime test
checks callback exit and reentry across registers and all retained frame spaces.
An exception code reused as an unbounded store address must still refuse.
Partial temporary aliases must not preserve a full runtime pointer, including
when the truncated value feeds address arithmetic in the same instruction.
`WindowsRegistrationNative.RuntimeExceptionFieldsRequireAReplacementObjectReceipt`
checks atomic refusal of incomplete or unconsumed runtime access evidence.
The `exception-code` and `exception-flags` reconstruction cases, including their
`eh4-` and `eh4-gs-` variants, execute original and reconstructed PE32 files in
both CLI modes and at a changed image base. Each source replay also rejects
wrong field offsets, pointers and widths, unindexed and duplicated reads,
runtime writes, overwritten pointer cells and pointer escapes (including an
extra unindexed pointer-slot read). These are native reconstruction checks;
they do not grant arbitrary context access or a nonlocal continuation contract.

PE32 LowIR branch
targets use 64-bit code-address constants; they must still fit the 32-bit address
space. Data-pointer width is not the branch-address representation.

`RegistrationCallABI.TerminationOnlyStackProofDoesNotGrantAnObjectBorrow` checks
an incoming-object conditional finally separately from its returning-stack
contract. Far returns, unknown stack restoration, malformed metadata, writable
code, changed frame registers and nested calls that only have a pop summary
refuse. This regression does not certify the helper's object-memory effects.
The public xcpt4 PE32 and PE64 issue tests remain separate required acceptance
checks; passing these focused tests does not close either issue.

`RegistrationConditionalFrame.XCPT4CompletesWithinTheDefaultClosureBudget`
checks the original PE32 xcpt4 main, including filters, ordinary and runtime
finally invocations, local unwinds and jump-buffer resumptions. The shared
registration work cap is 4,194,304. A Release observation used 1,144,583 state
work units and 2,613,031 units for the complete decoded-callee closure. The
original filter-output test also passes when built independently from its
unchanged test body and helper. These work counts are deterministic accounting,
not a throughput benchmark. PE64 source projection and native-frame proofs
have separate acceptance gates.
Unknown-address frame accesses still charge every potentially aliased cell.
Their budget regression scales its load count with the configured limit and
must withdraw all complete output when exhausted.
Memory extent tests continue to reject a borrow beyond 1 MiB and an oversized
C++ throw object. Their shared source-call storage limit is independent of
the registration propagation work cap.

The borrowed-capture, asynchronous-filter-write and nested-finally cases
require actual buffer contents, disjoint object extents and each invocation's
own return slot. Import rebinding, partial buffer writes, changed CALL bytes,
missing capture evidence and unproved continuations refuse. The returning
RaiseException case checks a call before a finally return; conditional and
guarded instructions and an absent memory contract still refuse.
CRT regressions check the exact `_controlfp_s` binding and output DWORD,
floating-state intrinsic shapes and import cleanup without granting memory
effects. Scalar comparison tests cover signedness, input widths and unknown
values. Distinct direct registration prologues inside one guessed function
range must retain separate owners; a damaged prefix supplies no new entry.

`PipelineOutcome.SourceEHRefusalsAgreeAcrossSingleAndShardedEmission` checks optimized and unoptimized source projection across PE32, ELF and Mach-O on x86, x64 and AArch64. Single-function and sharded LLVM emission retain the same explicit incomplete native EH contract; binary rewriting still rejects it.

`RegistrationCallNoReturn` rejects changed instruction extents, targets, operation ranges, conditional calls and foreign architecture/format use. The realigned PE32 probe checks both an independently terminal decoded call and a retained fallthrough requiring a current receipt; invalid memory evidence cannot recreate an already absent ordinary edge.

`SafeSEHReference.*` checks immutable SafeSEH membership, malformed or ambiguous storage, relocations, foreign architectures/formats and declaration order across shards. `PipelineOutcome.NativeFinallyInputsRetainSourceLLVM` uses `NEVERD_NONLOCAL_FINALLY_INPUTS` to test 32 LLVM combinations of the native MSVC x86/x64 O0/O2 and GS fixtures with source optimization enabled and disabled. It verifies LLVM and resolved call signatures while requiring unsupported x86 native EH reconstruction to remain rejected. The focused Windows EH job downloads the native fixture artifact and runs this matrix; without the input directory the test is explicitly skipped.

`RegistrationPrologueSaves.*` and `PipelineOptNone.*` cover retained source memory, protected callback arguments/selectors and continued optimization of ordinary code. The native MSVC matrix checks the x86 `/Od` and `/O2` (`/GS-`, `/GS`) `nonlocal_finally_return` and `nonlocal_finally_goto` exports, including independent COFF validation. `check_windows_finally_rewrite.py --form return` and `--form goto` compare raw/optimized IR through both patch modes at two forced bases, with chain/stack guards and a negative control. The header-only EXE-to-DLL adapter requires a TLS-free input and retains original code; an outer SEH observer supplies exception search. This does not establish support for partial local-unwind bounds or reentry inside active enclosing scopes.

`RegistrationEH4Wrapper.*` covers both bases, mutable/immutable runtime data, MXCSR branches, every-path definitions, caller-frame isolation, overlapping instructions/storage and exact PE relocations. The two MSVC profiles each include an independent negative control.

`LocalUnwindInsideCheckedSEHHandlerCarriesTheParentFrame` checks local-unwind calls from a checked Windows x64 SEH handler and from shared ordinary/exceptional entry paths. Negative cases cover mismatched unwind allocation, unstable or partial SP values, independent roots, incomplete input, absent or forged exception edges, work exhaustion, loads from unknown stack memory, foreign target relocations and untrusted runtime providers. The LowIR call proof and module receipt both feed SSA verification; ordinary returned-continuation tests retain their separate root rules.

`OrdinaryInternalReturnsRetainTheParentsReturnPC` and `OrdinaryInternalWritesCanUseTheParentsPrivateAllocation` check parent-PC corruption and disjoint parent-local writes on PE, ELF and Mach-O x64 and ELF i386. An intact callee PC alone cannot authorize an ordinary call.

`LLVMFrameSlotProof.RepeatedClosedQueriesDoNotRepeatProofWork` bounds repeated positive and negative query work across x86, x64, ARM and AArch64. `SharedBudgetProofsReplayTheirNodes` checks that a replayed shared-budget proof leaves the same answer and remaining budget as a fresh walk, for an exactly sufficient budget, one node short and half the nodes, and that a walk that ran out replays only for budgets no larger than its own; `BoundQueriesReuseOnlyTheirBindings` keeps answers for different bindings apart. The context tests retain bound-value, entry-SP, width and zero-budget refusals, and discard answers across provisional/rebuilt feasible graphs and function changes.

`RegistrationState.LocalUnwindRetiresLevelsAfterCheckedFinallyEffects` also checks early stopping below a bound, sentinel exits, enclosing links that skip the requested index and joins with distinct stopping levels. Missing finally bodies must still be rejected when actually traversed; a skipped cleanup must not lend a frame-write effect.

`RegistrationState.LocalUnwindRetiresLevelsAfterCheckedFinallyEffects` also covers constant bounds outside the table, the high unsigned half and -2. No-op exits retain the original level and frame-memory facts; dynamic bounds and invalid current levels still reject.

`RegistrationState.MalformedSEHScopeGraphsPublishNoStateFacts` checks wrong sentinels, cycles, forward/out-of-range links, invalid initial levels and handler/filter roles despite a complete parser status. Initial-active-scope and empty-table controls preserve valid frame sentinels in both EH3 and EH4. Over-width handler and filter addresses also reject without publishing facts.

The source registration solver uses one administration-boundary check for object borrows, catch-object construction and local-finally writes. `RegistrationState.SEHObjectBorrowsExcludeSavedStackAndExceptionPointers` and `SEHObjectWritesCannotBorrowWholeOrPartialCookieCells` cover EH3/EH4 reads, writes and partial overlaps with SavedESP, exception pointers and cookie cells. `RegistrationCallback.RealignedCatchObjectsCannotReplaceRuntimeAnchors` protects the saved entry frame independently of later reloads. Adjacent ordinary objects remain valid.

`NeverDRegistrationCalleeStackTests` runs the decoded PE32 call/return ABI
controls, including a cycle between a registration parent and an ordinary
wrapper. An unbalanced recursive wrapper cannot borrow its own pending ESP
answer. `RegistrationCallABI.ReturningStackProofDoesNotNormalizeTheX87Stack`
keeps the stack proof for bodies whose x87 normalization alone is incomplete
or splits blocks by TOP, and for FXAM with a proved or unproved tag.
`RegistrationCallABI.ScalarReturnDescribesEAXBesideAnX87Result` pins the leaf
contract's EAX result for a leaf that also returns an x87 value.
`RegistrationCallABI.RecordedStackPopsChargeTheirFreshWork` checks that a
recorded stack-pop answer charges the work of the fresh proof: the answer and
an exhausted caller budget match a fresh proof, answers of another SEH
analysis level stay apart, and another image object never reads the record.
`NeverDX87CallGraphTests` provides the focused `X87CallStack.*` and
`X87CallGraphCache.*` checks for the shared-cache and invocation boundaries.

`MedStackDelta.*` (in `NeverDLiftTests`) resolves the stack-pointer offset
behind 64 joins in a row, each merging two arms; every path reaches the live-in
stack pointer exactly at the depth limit, and one more join exceeds it.
`MedABIPass.FrameLocalLeafSummary*` (in `NeverDFPParameterMaskTests`) checks
the indexed leaf summary against duplicate definitions and entries, PHIs,
value widths and identities, authenticated stack live-ins, the depth limit,
and reuse across functions or invocations. The index belongs to one immutable
function; a rejected function must still count toward duplicate-entry rejection.
The `Native pipeline compatibility` workflow runs the complete focused ABI,
pipeline-outcome, thread and HighIR control-flow owners on Linux, macOS and
Windows x64 (MSVC and clang-cl), together with actual SDK allocation and native
worker requests. It requires registered cases to execute without failures or
skips and asserts that the normal build enables mimalloc by default. This
focused profile does not replace CPU-emulation or full semantic qualification.
`NeverDGraphCyclesTests` checks component grouping and a million-node chain.

`MedCxxContinuationFrame.ProtectedSourcesFollowCompactedBlockIdentities` uses
native jump blocks removed before SSA. Both asynchronous and synchronous
catches must retain the parent's local slot and the nonvolatile value written
before the throwing call. The original FH3/FH4 GS/no-GS O0/O2 ABI-probe CLIs
remain the corpus-level check for this projection.

`HighCParamUsage` checks shared expression DAGs, repeated callee queries and
recursive argument uses. Its default and compacted parameter lists must agree
with the operands that C prints. The two `WindowsEHModesCorpus.Decompiles*`
regressions run the original FH3 O2 GS/no-GS probes and the five PE32 xcpt4 and
xframe inputs through the full source CLI, including output generation. These
checks also run in the Windows EH CI lane.

`NeverDSEHRuntimeMemoryTests` checks nested authenticated UCRT calls, actual
home-area writes, immutable formats and receipt freshness. Wrong providers,
delay imports, incomplete or conflicting PE32/PE64 storage, `%n` variants,
unproved writable formats, saved-slot overwrites, IAT rebinding, forged import veneers,
parent-return-PC overlaps with either CALL's push or the home area, saved cells
below SP and encoded frame escape
must refuse. These focused contracts do not establish xcpt4 support; retain the
real `DecompileMatrixFailsClosedWithAuditableExceptionMetadata` acceptance test.

`SEHRuntimeMemory.WindowsFormatsDoNotBorrowForeignDialectContracts` checks the
decoded CALL through the complete frame proof. Ordinary integer/float formats
preserve the saved cell; positional/grouping/q dialects, unproved dynamic
widths, overflowing decimal fields, pointer conversions and multibyte formats
must not. Both segment and section extents cover each fixture's actual bytes,
so rejection tests syntax rather than an accidentally truncated mapping.

`SourceFrameFormatBytesOwnTheRuntimeContract`,
`MissingOrChangedFrameBytesWithdrawFormatAuthority` and
`FrameFormatsRequireLiveStorageAndEveryReachingByte` in the same target decode
source stores and calls. Byte/DWORD initialization, matching branch stores and
an inspected helper filling a missing byte preserve saved-frame evidence.
Changed or unknown bytes, missing NUL, conflicting branches, released storage,
runtime home/scratch areas and active return PCs refuse. The same Windows
grammar applies to image and frame formats. These tests do not establish
initial mutable-image bytes, handler-memory restoration or native execution.

`SEHImageFormat.*` in the same target decodes stores into writable PE image
storage. Complete stores, agreeing branch arms and an inspected callee filling
one byte preserve the saved frame across authenticated UCRT calls. File-only
bytes, missing bytes/NUL, conflicting branches, unknown byte writes, `%n`,
callee changes and saved-frame overwrites refuse. Truncated and ambiguous image
storage supplies no destination authority. The original xcpt4 matrix remains
the acceptance check for initial writable literals and exception activations.

`MedSEHEstablisherFrame.SourceIndependentEntriesKeepUnknownStackValues`
checks ordinary and shared exception entries without borrowing the prologue's
stack cells. Strict conversion still refuses, source conversion retains
unknown memory operands, and direct LLVM emission rejects the result.
`SourceSwitchRequiresEveryResolvedTarget` checks the resolver-to-frame CFG
boundary, including absent, duplicated, changed and unsafe target sets.
`SourceRequiresExplicitIndependentEntry` rejects disconnected protected paths
without a published source entry and accepts the same graph once that entry
can receive explicit unknown values.
`TheX87ContextHandleIsNotAFrameValue` proves the frame of a handler that starts
with `fninit`, whose x87 context handle the CFG builder begins at each root.
`TheHandlerBeginsTheX87ContextBeforeUsingIt` and
`OnlyTheX87ContextHandleIsAdmitted` refuse a handler that reads the handle
before beginning it and a handle copied into an ordinary register.
`MedSEHHandlerEntry.AHandlerResumingAtANopEndsTheArmThere` ends an `__except`
arm that resumes at a `nop` without a jump, and
`X86_64_PipelineE2E.DecompileC_MinGWStartupSEHWithX87Callee` decompiles
MinGW-w64-shaped startup code (`test_mingw_startup_seh.s`) whose guarded callee
resets the x87 state.
`COFFExceptionIR.HighCEmptyExceptArmResumingAfterItsTryPrintsNoJump` prints
MSVC's nested -O0 levels with empty arms, and
`HighCLeftOutTryTailKeepsTheLabelAJumpLandsOn` keeps the label that switch
cases leave a try body for. `HighCSEHFilterIsNamedAsTheFunctionListNamesIt`
names a filter by its function symbol over a `.text` section symbol, and by
an export. `StaticSymbolTableTest.MinGWImageNamesNoSectionOrFileRecord` reads
`unittests/loader/fixtures/mingw_sections.exe` (its Makefile rebuilds it with
MinGW-w64 GCC): GNU ld's section symbols stay as bounds that name nothing, its
`.file` record is dropped, and a static function's own record names it.
`StaticSymbolTableTest.MinGWDataAmongCodeIsData` reads `mingw_text_data.exe`
and its i386 build `mingw_text_data32.exe`, which keep a -1-headed pointer
table in `.text` as GNU ld keeps `__CTOR_LIST__`: the table is data, the
pointer to it in `.data` points to data, and its entry is a slot that holds
code.

`NeverDRelocatedFunctionEntryTests` checks LLVM's shared entry evidence across
x86, x64, Thumb and AArch64 with PE, ELF and Mach-O images. It covers bounded
relocation proofs, complete CFG tables, malformed metadata and cache reuse.
`CFGMetadataAuthenticatesEntriesWithoutRelocations` checks that metadata alone
identifies a code address, while its absence still refuses that address.
`RelocationsDoNotAuthenticateContinuationStarts` and
`CFGMetadataDoesNotAuthenticateContinuationStarts` reject independent function
identity for continuation code through both the verifier and LLVM emission.
The `FunctionEntryAdmission` cases exercise runtime, verified, symbol, unwind,
known-range and import evidence against the same data, continuation and
alignment rejections. They also check Thumb mode aliases, primary entries
preceding their unwind frames, index rebuilds and next-entry bounds. A named continuation jump
must retain its parent CFG and return; LLVM must keep interior frame addresses
relative to the primary function. CFG targets reject virtual tails, incomplete
instruction units and overlapping owners while admitting writable and
execute-only code. On-demand x86 import-thunk decoding cannot join code bytes
with adjacent data. Run `NeverDFunctionAdmissionTests`,
`NeverDNativePointerRelocationBoundaryTests`,
`NeverDMachOPointerRelocationBoundaryTests` and `NeverDSourcePatchLayoutTests`
when changing this shared entry boundary.

`PointerRelocationBatch.PointerRunsAmongCodeAreData` records an i386 jump
table's three relocated slots in `.text` as data slots and the displacement
naming it as a data operand, and keeps the two fields of one
`mov dword ptr [disp32], imm32` as operands.
`TwoAdjacentEightBytePointersAreDataAmongX64Code` takes two eight-byte fields
as data, and `PointerRunsAmongOtherCodeStayOperands` leaves an ARM literal pool
and an object file's relocations alone.
`X86_32_FunctionDiscovery.SwitchTableAmongCodeBelongsToItsFunction` builds a
linked i386 image whose table follows its function two bytes past a four-byte
boundary, with an aligned relocated `.rdata` pointer an instruction names past
it: the cases stay the switch's labels rather than entries, and the CFG
builder recovers all three. It fails without the run bound or the label rule.
`X86_32_FunctionDiscovery.CallbackStoredByARelocatedImmediateIsAFunction`
stores a callback with `mov [ecx+0x10], offset cb` and recovers the callback
as a function of its own.
`X86_32_FunctionDiscovery.SwitchTableAmongCodeLiftsPastTheMirrorLimit` lifts
such a switch from a `.text` larger than the LLVM emitter mirrors as one
global; it refuses the table's address without the table-base witness or the
elided address operations. A variant with no arbitration budget, which leaves
no slot suppressible as a large module's arbitration does, refuses it without
the exception for a table among the code. `APointerSpilledAcrossALoopsCallsKeepsItsTable`
walks an integer array in `.rdata` beside a code pointer with a pointer kept
in a frame slot across a call (`add dword ptr [ebp-4], 4`), and fails as an
ambiguous pointer table without the frame-slot recurrence.
`ANestedLoopsSpilledIndexStaysAnOffset` keeps an outer loop's index in a frame
slot and an inner loop's record pointer `&table[i]` in another, around a call
with a pushed argument; it is refused the same way without the affine frame
keys, as the inner loop's merge on the outer back edge leaves its pushes
unkeyed.
`ARuntimeIndexedReadOnlyTableResolves` reads an `.rdata` int table at a live-in
object pointer's field scaled by four, as GOF2's `sub_4028D4` does. Without the
run-mirror redirect it is refused as an ambiguous read-only table base; with it
the read is a GEP into a read-only global and the table VA survives nowhere.
`InCodePointerTableMirrorsPastTheLimit` reads a function-pointer table MSVC
lays among its code as data, in a `.text` over the 1 MiB single-global limit;
the read resolves through the table run's own mirror rather than refusing that
the code segment cannot mirror.
`ANarrowLoadBaseAmongCodeIsData` decodes a two-level switch's `movzx r,
byte [sel + table]` and its pointer-wide jump-table load; the byte table's
base in `.text` names data, the jump table's names code. Without the
narrow-load rule the byte-table base is a code address with no lifted
identity.
`SwitchCasesStayLabelsPastAnEntryBeforeTheTable` puts an aligned prologue
match in dead code between a switch's seven-byte `mov eax, N; jmp tail` cases
and its table among the code; the cases stay labels only because the
instruction reading the table, not the table's slots, names them.
`PrologueMatchesInsideAFunctionAreItsCode` falls into an aligned `push esi;
push edi` and branches back to an aligned loop head, both prologue matches the
entry owns, while a padded function and a tail jump's target stay entries;
`AFramePrologueAfterACallStaysAnEntry` keeps a frame prologue right after a
call that may not return. `APushOfEbpWithoutTheFrameMoveIsNoUnalignedEntry`
finds no entry at the 55 8B C6 inside `push 55h; mov eax, esi`, which the
unaligned scan took for a frame setup. `AnInt3ByteInsideAnInstructionIsNoPadding`
runs the loader's padding scan over a loop whose `jne` displacement is 0xcc:
the entry falls into the start guessed after it, which it owns, while the start
guessed after real padding past the entry's return stays an entry.
`CallbacksPushedPastTheReturnAreFunctions` pushes a constructor and a
destructor that sit right after the pushing function's return, with no call,
prologue or padding to mark them, and recovers both as functions; a catch
block's `mov eax, offset continuation; ret` keeps its continuation a label.
`AVtableSlotConfirmsAPaddingGuess` runs the loader over a destructor after
int3 padding that a relocated `.rdata` vtable slot names: it is no boundary
guess, while a padded start nothing names stays one.
`LLVMImportVeneerBoundary.APushedVeneerAddressNamesItsImport` stores an
exact import veneer's code-address constant, as `push offset` does; the
module names the import rather than refusing that the veneer has no code
identity.
`ARuntimeOrStaticPointerReadsThroughTheMirror` reads `p = arg ? arg : &empty`
with the default in a `.data` that holds a relocated code pointer, the shape
GOF2's merges take; `AMergeOfMirroredAndReadOnlyStaticsResolvesEach` adds a
relocated `.rdata` string arm, and both lift without an image address. Without
the merge rule they refuse as an ambiguous pointer-table address.
`AMergeWithAnUnprovedImageAddressKeepsItsProof` drops the string's relocation,
so its number lands in the image unproved: the read keeps the refusal, and
without that guard it would keep `0x402000` raw.
`I386CallContract.ABlockPhiIsNoArgumentPastAnEarlierCallInItsBlock` joins a
callback's address and an import call's leftover in EDX, calls the import
again in the join block, then a routine reading ECX and EDX: the argument is
the second call's leftover, while without the rule it is the join.
`SessionCAPITest.LLVMCReadsAPointerTableAmongCodeAsData` emits LLVM-C for the
function that reads the table through `.data`, which was refused before.
`NativePointerRelocationBoundary.MinGWPointerTableAmongCodeMirrorsItsFunction`
finds the entry of both fixtures' tables as the function's address in the
`.text` pointer-table mirror, and
`MinGWPointerTableAmongCodeTooLargeToMirrorIsRefused` pads `.text` past the
embedding limit with MinGW-w64 GCC, when installed, and expects the refusal.
`LLVMCValues.AGlobalAnEarlierInitializerNamesIsDeclaredFirst` compiles and runs
a whole module whose `.data`-like global points into a constructor list
defined after it.
`HighControlFlowSemantics.SlotCallGoesThroughThePointerItHolds` calls through
MinGW's `__imp_X` variables, which hold the image's own code, relocated or in
a fixed image, rather than naming an import by their spelling.
`StaticSymbolTableTest.MinGWImportVariableNoCodeChangesIsConstant` reads
`mingw_imp_slots.exe`, which calls through two such variables: the one nothing
changes is constant, the one its lazy binding rewrites is not.
`SessionCAPITest.MinGWImportVariableCallsReachTheirFunction` decompiles its
caller: HighC and LLVM C call the constant one's function directly and the
lazy one through its variable.
`HighControlFlowSemantics.AShiftByZeroIsItsOperand` reads AArch64's bit-0
`tbz` test as `x & 1`.
`HighControlFlowSemantics.ATruthValueSelectingOneOrZeroIsItself` reads -O0
AArch64's `cset w8, eq; and w8, w8, #1` as the comparison itself, or its
negation, and keeps a selection by a value that may hold another number.
`NativePointerRelocationBoundary.MinGWProgramWholeModuleLLVMCCompiles` links
a program against the MinGW-w64 runtime, when its compiler is installed, and
compiles the program's whole-module LLVM C for x86-64 MinGW. The LLVMCValues
cases it rests on compile and run on the host:
`ACallerWithUnwindInformationSeesItsCalleeDeclared` declares a callee whose
caller carries an unwind record;
`ALibraryRoutineCalledThroughItsImportTakesItsDeclaration` calls `fputc`,
`getenv` and `free` through import symbols with their headers' types and
`EnterCriticalSection` with the runtime tables' prototype;
`ARuntimeRoutineReturningAFunctionPointerIsDeclared` declares `_onexit`, and
`ARuntimeRoutineCalledDirectlyTakesItsPrototype` declares it for a direct
call, which passes only its parameter;
`ACastBetweenAnIntegerAndAPointerConverts` passes a volatile read's integer
to a pointer parameter and adds to a pointer it read;
`AWeakImportIsDeclaredWeak` links without `__gmon_start__`;
`ADefinitionOfALibraryRoutinesNameTakesItsAddress` renames the image's own
`atexit`, `AMinGWRoutineTheProgramLinksTakesItsAddress` a Windows program's
`__mingw_printf` and no other format's, and
`AWholeModulesMainTakesItsAddress` a whole module's `main`; and
`ACalleeThatReturnsNothingLeavesItsResultUndefined` calls a definition
settled as returning nothing alone, or reads zero for its result.
`LLVMCIntrinsicSemantics.RepMovsForwardBackwardDynamicAndOverlap` runs
`rep movs` copies of each width and direction, overlapping ones too, and
`RepMovsRejectsUnprovedContracts` refuses other shapes.
`LibCResult.ReadsWhetherTheDeclaredResultIsAPointer` reads pointer results
from headers, typedefs included, and from the runtime tables.
`MinGWHeaderFunctions.NamesWhatTheHeadersDeclare` reads the routines the
mingw-w64 headers declare, its own, the runtime's and the C library's.

`NeverDItaniumHandlerTests` spells Itanium landing pads as C++ handlers.
`ItaniumHandlerBodies.ACleanupOnlyPadBecomesACatchAllThatRethrows` prints a
pad that only cleans up as `try { … } catch (...) { cleanup; throw; }` and
keeps it labelled in C. `ACleanupScopeHoldsNoCallItsPadDoesNotCover` keeps a
call that reaches no pad out of the scopes, and
`ACopiedCallKeepsTheCoverOfItsInstruction` does so for a tail copy whose
statement took a covered jump's address.
`ABranchWhoseArmsCallThroughThePadIsOneScope` wraps the whole branch, and
`ACopyWithoutAnAddressKeepsItsCallsInstruction` places calls copied into both
arms by their instruction. `AnExceptionSpilledThroughAFrameAddressIsResumed`
follows the exception through the slot address clang keeps for a large frame.
`AnExceptionSpilledOffACopiedFrameBaseIsResumed` follows it through a copy of
the base clang reaches the far slots from,
`ASpillAndAReloadThroughTwoBaseCopiesMatch` through one copy to the spill and
another to the reload, and
`AReadThroughABaseRewrittenAfterTheSpillKeepsThePad` keeps the pad when the
base's slot changes between the spill and the copy.
`ACallOfNoInstructionKeepsTheCleanupPad`,
`AJumpIntoACleanupScopeKeepsThePad` and
`ACoveredCallOnlyAJumpReachesKeepsThePad` keep the pad where a scope cannot
be proved. `NothingFallsOutOfAnEndlessLoop` prints no dead statement after a
`while (1)` that only returns. `HighControlFlowSemantics.AMergedCallKeepsAnInstructionOnlyIfItHadOne`
checks that merging two branches' equal calls forgets their instructions.
Malformed unwind/prologue and converted-SP checks run with source projection
enabled too. `WindowsEHModesCorpus.DecompileMatrixFailsClosedWithAuditableExceptionMetadata`
runs the original eleven-case matrix, including the complete x64 xcpt4 image.
`DecompilesRegistrationCalleeProofCycles` gives its full PE32 applications a
240-second child-process allowance, within the corpus's 300-second CTest
deadline. This accommodates slower hosts producing the large source body;
the static proof work limits and output assertions do not change.

`RegisterPairReturnTest` checks x86 and ARM32 high-word evidence across
independent returns, transparent predecessors, clobbering calls, PHI inputs and
tail calls. `RegisterPairReturnPipeline` keeps a partial-register callee and a
`memset` veneer scalar despite callers reading EDX. The shared
`MedSSAMultiRoot.UnwrittenPhiEdgeCannotBorrowASiblingDefinition` checks x86,
x64, ARM32 and AArch64 merges. The PE32 xcpt4 corpus gate also requires the
three affected EH entries and the memset veneer to retain their bodies without
an unknown-return skeleton. These are source/IR checks, not native EH execution
certificates.

Run the original x64 executable locally and verify the separate source result:

```bash
python scripts/check_windows_xcpt4_source.py --neverd build-release/bin/neverd \
  --output /tmp/neverd-xcpt4-source --wine-prefix /tmp/neverd-xcpt4-wine64
```

The script accepts `wine`, `wine64` or `wine64-stable` (or `--wine`), uses an
isolated 64-bit prefix and checks all 90 reported cases even when the EXE exits
zero. An unhandled exception or page fault on either output stream rejects the
observation, including when Wine's debugger returns zero after all cases print
success. Routine Wine prefix diagnostics are allowed. The x64 fixture reports
89 successes and one explicit skip: test 29's
alignment check is compiled only for ARM. Unexpected skips fail. It then runs
the original source CLI and checks main's body, language/GS
metadata and explicit unresolved-frame annotation. JSON keeps original runtime
and source-analysis results separate. Neither successful original execution
nor readable source is evidence that a replacement PE can execute. The Windows
EH CI lane runs this check and the full analysis matrix; source/patch refusal
and saved-slot overwrite controls remain required independently.

The pinned VS2019 PE32 GS/O2 xcpt4 fixture has five compile-time skips: test 29
requires ARM alignment handling, tests 58/61 require x64 nested-finally support,
and tests 87/89 exclude i386. Its original-runtime baseline therefore requires
85 successes, those five skips and no unhandled exception. Wine 9's built-in
UCRT can report a floating-point overflow while restoring `_controlfp_s` state
even though all 90 case reports appear and the launcher returns zero. For a
local comparison, use a dedicated `WINEARCH=win32` prefix with the Windows SDK's
x86 redistributable `ucrtbase.dll` in that prefix's `drive_c/windows/system32`,
and `WINEDLLOVERRIDES='ucrtbase=n'`. A `WINEDEBUG=-all,+loaddll` observation must
show that UCRT loaded as native. Preserve the original image hash, SDK version,
DLL hash, all case results and both output streams. This setup passes the
original PE32 fixture with Wine 9; it does not establish reconstructed-image
equivalence or replace native Windows replay.

`X64CRTFrame` in that target and `NeverDLiftTests` checks `_controlfp_s` output
extents with adjacent, partial-overlap, home-area, released-storage and return-PC
controls. Wrong providers, delay imports, missing or conflicting import storage,
unknown masks, unsupported precision control and previously escaped frames
refuse. Decoded WAIT and both register/memory scalar SSE arithmetic retain saved
frame identities, while malformed effects and GPR outputs (including R8 and
APX R16) do not. The shared PE32 floating-state tests cover the same policy.
Adding unproved language callbacks withdraws the Win64 FP preservation result.
These contracts leave the PE64 xcpt4 writable-format and runtime-activation
proofs open. Their native reconstruction coverage is separate from #858's
source-analysis acceptance above.

`RegistrationState.OrdinaryFinallyReturnsItsCheckedFrameEffects` checks actual
ordinary finally returns, private push/pop storage, incoming register values,
nonvolatile register changes and joins across both return arms. Missing branch
arms, return-PC corruption, administration writes, unknown aliases, opaque
calls, malformed boundaries, missing near-return evidence and stack-pop returns
reject. The check supplies no native callable contract and does not model the
nonlocal xcpt4 finally exits.

`RegistrationSEHReturn` checks the native near-return consumer against changed
CALL, callback, return occurrence and continuation identities, duplicate exits,
and multiple callers sharing one return. `RegistrationSEHControl` checks that
EH3 and EH4 both partition the physical CALL continuation before later source
instructions. The `normal-finally`, `eh4-normal-finally` and
`eh4-gs-normal-finally` reconstruction cases execute the generated callback,
public section/inplace installation and rebased images. Their independent
source replay also rejects replacing the generated finally return with
`unreachable`.

The nine `finally-parent-*` reconstruction cases and their EH4/EH4+GS variants
cover ordinary and runtime SavedESP branches, multiple parent destinations,
mixed near-return/nonlocal exits and another exception after resumption.
The parent-only ordinary and unwind cases resume code with no ordinary incoming
edge before joining an established parent epilogue. Their original and rebased
executions must pass independently. `FinallyOnlyParentTailsRequireEveryPathToAnAnchor`
checks single-block, chained and conditional tails in both block orders, with
cycles, missing/duplicate edges, foreign callbacks, decoded-target mismatches,
mode changes, guards and unanchored returns rejected.
They also check the current EBX/ESI/EDI values written by the finally and restore
the original caller's nonvolatile registers before returning. The register-input
case computes from the ordinary caller's values while retaining a scope that
is never activated by source try levels. Reentry checks run before the second
exception, whose handler has a separate runtime register contract.
Each case requires the original, independently installed outputs, both CLI
patch modes, IR/EXE inputs, optimization on/off and rebased images (20 runtime
observations). The reentry sources explicitly reconnect their live registration;
the unwind case additionally builds a control without this repair and requires
`RegistrationSEHParentExit.InputPE32RejectsAnAbandonedUnwindGuard` to reject it.
The invalid control is never counted or executed as a passing runtime fixture.
`RegistrationSEHParentExit` checks 37 changed source/invocation facts and complete
module rollback after failed outlining. `RegistrationSEHParentExitIRMutations`
checks 36 verifier-clean changes to capture, private storage, aliases, FS:[0],
selectors, source destinations and retained callback ABI. Callback-to-parent
PHI copies require the exact authenticated edge, including recovered scalar
slots shared by multiple destinations. The complete source replay profile
includes all 243 reconstruction cases and requires 4,860 runtime observations.
`RegistrationSEHRegisterIRMutations` checks 30 edits to register slots, aliases,
source order, runtime-entry selection and constant outputs.
`RegistrationCookieABI` checks ECX-only recovery despite incidental EDX/XMM/stack
values and conflicting inferred signatures, cross-block cookie PHIs, and 18
invalid source occurrences that must not acquire the cookie-call protocol.
`RegistrationConditionalFrame.XCPT4SelectedFunctionRetainsItsScalarFilterRange`
checks that focused loading preserves the LowIR protected range and MedIR finally
returns even when the native parent-chain gate fails. The parent-exit tests require
both native preparation and independent COFF binding to reject that unsafe chain.

`RAMReservationTests.cpp` checks public write witnesses across identical-value and ABA stores, retained slices, address reuse, sibling mappings, rollback, cancellation, fault-prefix commits and opaque execution. `UnpackDriver.RelocationFactsRequireUninterruptedLoaderWrites` checks owned loader facts at preferred, downward and kernel load addresses: restored bytes do not restore invalidated provenance. `DriverGuardExecution.SessionFixupsInvalidateOriginalLoaderFacts` distinguishes active CFG pointer replacement from dormant fallback retention.

`UnpackDriver.SnapshotPreservesOnlyUnmodifiedLoaderRelocationFacts` checks PE output, an independent DIR64 reader, model rebasing and exclusion of rewritten debug records. `UnpackDriver.SnapshotRelocationFactsBindSourceAndFormatWrites` rejects source/base/order mismatches and excludes formatting replacements even when bytes match. These checks certify diagnostic metadata, not native recovery.

`RegistrationStackCopy.*` authenticates equal-displacement copies from both callback predecessors and rejects changed displacements, unknown or undefined sources, missing paths, external definitions, reads before definition, volatile stores and uninitialized self-copies.

## Source and native object replacement tests

The current expansion inventory is checked in as `unittests/Inputs/source-patch-expansion.json`: 283 CPU matrix cases, 14 layout cases, 16 callback cases, 265 exception-contract cases and the exact host-specific C API list. `scripts/qualify_source_patch_expansion.py` requires the complete inventory with zero failures or skips at a clean, exact source commit. `scripts/check_source_patch_expansion_runtime.py` separately executes callback, dependency and combined IR/object images through the native OS loader: 84 functional and 84 exception observations plus two missing-library/symbol controls per host. It checks real old/new shared libraries, floating/stack arguments, repeated saved-body calls, exactly-once cleanup, loader metadata and publication hashes. CPU, compatibility and native OS process evidence remain separate.

Six selected native OS/ISA jobs passed with zero failed or skipped cases: 3669 GoogleTest executions, 1008 native process observations (504 functional and 504 C++ exception observations), 12 real loader rejections, 96 images and 84 publication receipts.

| Host | Source | Harness | CI job | Tests | Observations / refusals |
|---|---|---|---|---|---|
| Linux x64 | [61fb1d3e759](https://github.com/NeverSight/NeverD/commit/61fb1d3e759f6796d23341782e7f4d624295f27c) | [197126208b6](https://github.com/gmh5225/test_mac_intel/commit/197126208b665bec0db5c6dc74278931300229af) | [116845056048](https://github.com/gmh5225/test_mac_intel/actions/runs/38861651227/job/116845056048) | 626 | 168 / 2 |
| Linux ARM64 | [61fb1d3e759](https://github.com/NeverSight/NeverD/commit/61fb1d3e759f6796d23341782e7f4d624295f27c) | [197126208b6](https://github.com/gmh5225/test_mac_intel/commit/197126208b665bec0db5c6dc74278931300229af) | [116845055863](https://github.com/gmh5225/test_mac_intel/actions/runs/38861651227/job/116845055863) | 600 | 168 / 2 |
| macOS Intel | [b7a180c7ac3](https://github.com/NeverSight/NeverD/commit/b7a180c7ac35fc2f05be3568a86104d42fc32800) | [cba0906a114](https://github.com/gmh5225/test_mac_intel/commit/cba0906a11461d6e29bbe68f9337515fe084b610) | [117185095063](https://github.com/gmh5225/test_mac_intel/actions/runs/38963782346/job/117185095063) | 624 | 168 / 2 |
| macOS ARM64 | [61fb1d3e759](https://github.com/NeverSight/NeverD/commit/61fb1d3e759f6796d23341782e7f4d624295f27c) | [f6311a611e0](https://github.com/gmh5225/test_mac_intel/commit/f6311a611e095bcffb7537c7ae3b40c826d1e2e1) | [116879179194](https://github.com/gmh5225/test_mac_intel/actions/runs/38872352550/job/116879179194) | 624 | 168 / 2 |
| Windows x64 | [76ce83e138d](https://github.com/NeverSight/NeverD/commit/76ce83e138de639b8d75206c78bd5ccbb9cfd8dd) | [a42b117681f](https://github.com/gmh5225/test_mac_intel/commit/a42b117681f81d922e09d98ccb1a1c4525055947) | [116908799030](https://github.com/gmh5225/test_mac_intel/actions/runs/38881414688/job/116908799030) | 598 | 168 / 2 |
| Windows ARM64 | [76ce83e138d](https://github.com/NeverSight/NeverD/commit/76ce83e138de639b8d75206c78bd5ccbb9cfd8dd) | [cba0906a114](https://github.com/gmh5225/test_mac_intel/commit/cba0906a11461d6e29bbe68f9337515fe084b610) | [117014878954](https://github.com/gmh5225/test_mac_intel/actions/runs/38913475295/job/117014878954) | 597 | 168 / 2 |

These artifacts qualify exactly the source and harness commits listed below. Later dev integration and documentation commits have separate local and documentation checks. Earlier failed or cancelled jobs remain failures. The historical 216 supplementary observations, PE ARM32 and older 32-bit Apple CPU/compatibility results do not establish new native hardware or C++ exception coverage.

The Windows ARM64 qualification uses a native ARM64 Unicorn C DLL built from the pinned submodule with the verified LLVM-MinGW toolchain, an MSVC import library and an explicit CMake target override. Its isolated build copy authenticates three host fixes: the non-SEH jump pair, an ARM64 page-commit callback with instruction-cache synchronization, and 64-bit immediate masks for Windows LLP64. The submodule source and revision stay unchanged. This qualifies the test host for the CPU matrix; it does not establish default MSVC ARM64 JIT support. The five host-recovery checks are separate from native patched-process and C++ exception evidence.

`NeverDSourcePatchMatrixTests` compiles and links real fixtures, installs source
or supplied IR, reloads the result and invokes the patched original entry in
Unicorn. Four architectures (x86, x64, ARM32, ARM64) and three formats (ELF,
PE, Mach-O) require successful source, supplied-IR and native-object installation,
reload and exact execution. The 283-case inventory includes both ARMv7k watchOS
and ARMv7 iOS, mutable/readonly state, original floating-point and stack-argument
bindings, and source/object entries at two nonzero image-base slides. Every
invocation must reach the return address within a finite instruction/time budget.
This CPU matrix does not certify operating-system loaders or C++ unwinders.
Legacy Mach-O 32-bit containers retain compiler-produced instructions,
relocated CFI and genuine symbols; their CPU execution is not an OS runtime
qualification. Native object ranges never receive synthetic MC identities.
On Apple Silicon, `NEVERD_TEST_DARWIN_ARCH=x86_64` runs the C API/CLI and
seven exception scenarios through Rosetta 2 using x64 originals. Historical
Intel fixtures reserve file/virtual padding with `-Wl,-segalign,0x4000`.
The new tight-layout cases omit header/unwind reservations and require the
admitted format-owned layout expansion described in the source patch guide.
The exact reference regressions cover both x64 ELF PLT branches and Mach-O GOT
data words. `NEVERD_TEST_REQUIRE_FIXED_LLVM=1` requires both corrected producers.
See the [source patch matrix](source-patching.md).

`NEVERD_TEST_MATRIX_NEVERC=/path/to/neverc` selects the optional ordinary NeverC
producer while originals remain compiled by Clang as independent ABI oracles.
For NeverC 3389.1.5 use
`--gtest_filter=AllTargets/*.*/*_X64:AllTargets/*.*/*_ARM64`: its 66 checks cover six supported
profiles. This compiler does not implement x86/ARM32; an unfiltered run must
fail those source cases rather than silently use Clang. The adapter disables
NeverC's automatic PE default libraries with `-fno-autolink`. The same matrix
still rejects externally supplied `.drectve` contracts without publication.

The matrix also checks `target` receipts against an independent ABI inventory for compiled and supplied native objects. Malformed-object tests must then install and execute the unmodified object as a positive control.

The dedicated `source-patch-windows.yml` workflow uses Clang 22.1.6, the native
MSVC CRT and the Windows SDK. `NEVERD_TEST_REQUIRE_WINDOWS_RUNTIME=1` makes missing
runtime inputs fail; the workflow also rejects skipped PE/object/matrix cases.
It retains XML evidence and does not cancel an active qualification when an
unrelated dev push arrives. Check the actual run result before claiming native
Windows qualification.

`scripts/check_source_patch_pe_runtime.py prepare` builds three freestanding
PE originals and 27 replacements: x86/x64/ARM64, three C scenarios, and source
IR/compiled-object/supplied-object routes. The scenarios exercise original
nonleaf calls, mutable pointers and floating-point/stack argument bindings.
`run` requires 30 process observations per architecture, exact commit/file
identity and a complete inventory; failures retire any previous success report.
The Windows x64 job executes x86 and x64, then `windows-11-arm` downloads the
same hashed images and executes ARM64. All originals and replacements run three
times. The jobs retain images, publication receipts and runtime JSON. A runner
override records external execution and cannot claim native Windows coverage.
These C process checks do not qualify ARM64 C++ EH or an ARM64 engine build.
The verifier's evidence regressions run with
`python -m unittest scripts.tests.test_check_source_patch_pe_runtime -v`.

`NeverDSourcePatchTests` checks C API/CLI address selection, compiler isolation,
atomic publication, original data bindings, native state and C++ exceptions.
Its object cases use cross Clang plus LLD for malformed-input, relocation and
byte-preservation checks even on a non-Linux host. Native ELF runtime cases run
on Linux x64/ARM64; Mach-O runtime cases run on Darwin ARM64/x64. Set `NEVERD_TEST_NEVERC`
to an optional NeverC executable to include its compiler cases. GCC C++ runtime
cases additionally require a matching Linux x64/ARM64 `g++`.
Configure `NEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE` with a compatible Clang
path; the verified source profile uses Clang 22.1.6. This setting is separate
from the compiler that builds NeverD and from other suites' fixture compilers.

```sh
cmake --build build-source --target NeverDSourcePatchTests \
  NeverDNativeExceptionRewriteContractTests NeverDCOFFExceptionPatchTests --parallel 4
ctest --test-dir build-source -L \
  '^(NeverDSourcePatchTests|NeverDNativeExceptionRewriteContractTests|NeverDCOFFExceptionPatchTests)$' \
  --output-on-failure --parallel 4
```

Use the corrected LLVM source described in [source patches](source-patching.md)
for x64 PLT capability validation, with `NEVERD_TEST_REQUIRE_FIXED_LLVM=1`.
Without this environment variable the regression probes the backend separately
and requires an exact branch or fail-closed refusal. A refusal with the r3/r4
prebuilt is a passing safety test, not a passing runtime capability test.
The ELF IR exception-runtime case is skipped with the known r3/r4 prebuilt unless
that capability-validation variable is set; native-object cases do not need
this LLVM correction.
The exception fixture's original target may throw, so the original compiler
retains caller catch/cleanup paths. A no-throw original can erase those paths.
The noexcept case installs a termination handler and checks its exact exit code.

`SourcePatchPE` always cross-checks original/new unwind rows, separate writable
storage, base-relocation publication, and refusal of active guard profiles or
forged lifted EH metadata. Its FH3 runtime test additionally uses
`NEVERD_TEST_WINDOWS_CRT_LIB`, `NEVERD_TEST_WINDOWS_UCRT_LIB` and
`NEVERD_TEST_WINDOWS_SDK_LIB` for x64 import-library directories. On a non-Windows
host, `NEVERD_TEST_WINDOWS_RUNNER` must name an executable accepting the PE path
and its arguments, returning the program exit code. Use a matching OS unwinder
(the development run uses Wine 11); a CPU-only emulator is insufficient.
These optional SDK files and runners are test inputs, not build dependencies.

Before reviewing these changes, also build/run `NeverDLiftTests` and
`NeverDPatchFullTests`, and run the Python SDK checks in
[CONTRIBUTING.md](../CONTRIBUTING.md). Report platform/toolchain skips separately
from passed tests. Object identity checks or cross-compilation alone do not
establish operating-system unwind behavior.

`NeverDNativeFinallyInputTests` separately owns the MSVC-input matrix. The portable `NeverDPipelineOutcomeTests` owner remains mandatory on every CI platform; its execution cannot depend on optional native fixtures. The focused Windows EH job builds and executes the native-input owner with its verified images. macOS CI explicitly selects Homebrew Clang 22 for source-patch fixtures; the compiler used to build NeverD is independent. Unsupported replacement stack-probe methods remain rejected.

MSVC unit test targets use `/bigobj` to allow the larger COFF section counts produced by GoogleTest and LLVM template instantiations. Other compiler profiles are unchanged. Configuration checks verify the generated test compile command; native Windows CI still verifies compilation and execution.

Checked double-shift rejection reports the masked count and operand width when its destination and flags are undefined. `X64DoubleShift` verifies that typed exits and the legacy error API retain the reason without register or RAM changes or memory observations; the driver fixture verifies that the same reason survives before scenario dispatch.

`OriginalBinaryUndefinedIndependence.KernelPackedFlags*` checks CPL0 IF clear/set against a CPL3 countercontrol, profile matching and digest separation, TF/AC/IOPL refusals, mandatory final IF dependence even with ordinary observations disabled, restoration and exact/one-short work limits. `InterpreterMachineStateModel.GenerationRefusesUnsupportedOrMalformedInput` also checks that this native kernel profile cannot silently select the user source ABI. The production undefined-instruction admission and driver recovery guards are unchanged.

Readlink coverage in `linux_files.c` checks raw x64/AArch64 and Android O0/O2
ordinary/APS2/RELR calls, direct and dynamic Bionic bindings, signed low-word
sizes, error ordering, binary targets without NUL, truncation, aliasing,
page-tail import/output and unchanged descriptor cursors. Unsupported traversal,
metadata and mixed-copy cases retain pending calls and unchanged output bytes.
`ProcessReportTests` rejects conflicting namespaces, malformed targets and
combined limits; C API/CLI and Python reuse the same query fixture.

Android decimal-prefix tests use independent callers and literal expectations
for all six C whitespace characters, signs, empty/nondigit inputs, decimal-only
prefixes and signed 32/64-bit endpoints. They check errno preservation, dynamic
provider lifetime, first-nondigit/page-tail access and fail-loud overflow under
the same O0/O2 and ordinary/APS2/RELR fixture matrix.

Driver write-watch controls independently encode changed-value and repeated same-value stores with raw write tracing disabled. Both legacy and checked x64 contracts must suspend after each watched commit, withdraw the unchanged-memory guarantee, and restore it at a later pure execution watch. Lifecycle observation verifies that new invocations and kernel export callbacks withdraw that guarantee. The same-value case is mandatory in native KVM/WHP inventory.

`KernelUnpackState.TypedPoolsCannotHideOtherRetainedObjects` checks mixed pool/device ownership before and after freeing the pool. Legacy and modern pool controls require the exact pool-only dependency inventory. The mixed-object case is mandatory in native driver CI; typed pool capture still requires runtime reconstruction.

Checked KVM x64 execution delegates the complete instruction loop through `withExecutionOwner` to its private vCPU thread. Admission, RAM transactions, hooks and budgets stay in that loop; individual native entries retain their controls and exception priority. The caller monitors active entries and acknowledges cancellation before returning. CPU hooks execute on this private owner, so they must not require the caller’s thread-local state. Standalone native-entry completion still runs on its caller. `ExecutionOwnerKeepsAdmissionAndTransfersOnOneThread` and `OwnedLoopDeadlineAcknowledgesAnActualNonExitingVCPU` cover ownership and real cancellation; this transport change does not certify a recovered driver.

The optional `NeverCProfiles/*` matrix adds 30 tests across six x64/ARM64 profiles: C IR, C objects, C hosted DynCode, noninline helpers/recursion and C++ rejection. `SourcePatch.NeverCRejectsCppBeforeStartingTheCompiler` also verifies rejection without an installed NeverC. `NeverDSourcePatchDynCodeTests` rejects forged artifact digests, entries, targets, state and final verification. `SourcePatchException.NeverC*` executes C results and a throwing callback through two hosted C frames. The separate Clang Windows C++ checker requires nine patched scenarios, original controls, three attempts, matching machine/commit/digests and native x64/ARM64 hosts.

```sh
NEVERD_TEST_MATRIX_NEVERC=/path/to/neverc build-source-full/bin/NeverDSourcePatchMatrixTests --gtest_filter="NeverCProfiles/*"
```

Native producer qualification pins NeverC revision `182fd2c5dc`, independent originals and the current NeverD installer. `check_source_patch_hosted_pe_runtime.py` requires 36 native Windows observations per architecture for C IR/object/hosted results and a throwing callback through two hosted C frames. `check_source_patch_elf_runtime.py` requires 120 native Linux observations per architecture for C IR/objects/hosted, hosted propagation and nine Clang C++ exception scenarios. Receipts, artifact hashes and the actual host architecture must match. These checks install real C producer outputs; they do not claim a NeverC executable ran on the CI host. The Linux inventory includes nine exception scenarios for each of Clang IR, Clang native objects and GCC native objects on x64 and ARM64. NeverC remains C-only.

The earlier 204-observation translation-profile receipt is historical. Current fixtures require C-only NeverC producers; Clang C++ FH3, native-host identity and emulated CPU profiles retain separate evidence. `HighSwiftEmitter` executes register and memory scalar conversions at both optimization levels, observing result bits and MXCSR. The mobile C parser accepts only the backend's exact pragma-only rounding prefix and continues to reject conditional declarations and mutated directives.

`InterpreterSpecialization.HighAddress*` and `BinaryLowIRRefinement.HighAddress*` cover upper-half entries and transfers, overlapping native extents, original CALL return bytes, stack restoration, data equal to label values, malformed or missing code, and exact/short budgets. The independent byte oracle and a changed-return-word negative control distinguish runtime values from CFG labels.


`NeverDRawEnvironmentRequirementTests` checks copied LowIR, exact and malformed marker shapes, strict/non-strict emulation, symbolic step/run, Low-to-Med conversion, direct C/LLVM emission, machine wrappers and re-specialization. Positive ordinary controls and unreachable-marker negatives cover static independence, both sides of refinement and original-binary candidate refinement; rejected proofs publish no certificate.

`PEFixedImageTest.RawCode*` covers returning supplemental code in both address halves, original return words, full registers and byte memory, unknown writable globals, full-slot import reads, missing code, full image exclusion, malformed/unused declarations, refinement, explicit alias contracts and receipt mutations. Exact/short preparation, result and operation budgets include many mappings, multiple bindings and mixed terminal exits. `PipelineRefusesUnusedRawEnvironmentDomains` verifies source refusal before recovery.

`RawCodeNativeReturn*` checks two physical saved return words, an aliasing gap, a discarded unproved cell, narrow-address refusal, complete register/memory effects and exact proof budgets. `SymStorePreservation.Additional*` and `SymStorePreservation.AbsentAdditional*` check range unions in both byte orders, post-store epochs, absent bytes, malformed ranges and failure without mutation at insufficient work limits.

`SymStorePreservation.Independent*`, `RepeatedRegionsFormAUnionAndChargeEveryScan`, `EmptyIndependentRegionsKeepOriginalWorkAccounting` and `ExecutorUsesIndependentRegionsForOnlyOneStore` cover a single STORE preserving two or three independent current roots, both byte orders, partial ranges across relative zero, overlapping declarations, untouched gaps and post-store unknown epochs. Exact and one-short budgets check the complete charge, while absent regions must stay unmaterialized. Invalid and out-of-range references, narrow or constant bases, empty/reversed intervals and the actual store's own base reject without state mutation. Existing single-root and `AdditionalRanges` costs remain unchanged when `AdditionalRegions` is empty. The executor case checks one event, its neighboring same-region bytes and later ordinary-store invalidation. `SymAtomicCmpxchg.IndependentRegionsShareTheWritePhaseAndItsBudget` checks matching and mismatching comparisons with exact and one-short preservation budgets, two protected roots, the old-value output and the existing fail-closed refusal path.

`FrameOffsets.*Normalization*` covers exact identities, extra inputs, root identity, infeasible predicates without cached witnesses, default behavior, prefix/measurement charges and post-call node refusal. `FrameNormalizationPreservesStoresAndNativeReturns` and `RawCodeFrameNormalization*` exercise original memory effects and physical return words with independent byte oracles, both byte orders where applicable, exact/short operation budgets and receipt mutation. `FrameNormalizationStrategyBindsIndependentProof` checks both binary proof identities.

`NormalizationRetryReusesOriginalTranslatedQuery` · `IncompleteNormalizationRetryNeverSuppliesCachedEvidence` · `NormalizationTheoremsNeverPopulateFiniteDomains`.

`NormalizationFeasibilityCachesExactTranslatedTheorems` · `NormalizationFeasibilityRejectsIncompleteAndUnrelatedProofs` · `NormalizationFeasibilityRetainsStorageAndWorkLimits`.

`MachineExitRetReadsAliasedBytesBeforePhysicalPop` · `MachineExitCallCapturesTargetBeforeAliasingPush` · `MachineExitJumpRetainsDynamicAndOriginalDirectTargets` · `MachineExitKeepsInternalCallsAndReturnProofs` · `MachineExitRequiresExactDomainBytesAndTransferKind` · `MachineExitIntegrityBudgetsAndOrdinaryConsumers` · `MachineExitProvesLiveInternalReturnCells`.

`ConstantAddressesReuseOnlyCompletedPredicateProof` · `ConstantPredicateReuseStillRequiresEachReadCertificate` · `ConstantPredicateReusePreservesInfeasibleControlEdges`.

`NormalizationChargesOnlyTheUncachedReadabilityPrefix` / `ReadabilityPrefixInspectionPreservesLazyAndCopiedCaches` checks that cached history is not charged again, appended wide constants still cost admission, short work and node budgets still refuse, and copied readability caches remain independent.

## Native Lift qualification

The `Lift native qualification` workflow owns the 108 previously unqualified cases (62 failures and 46 skips). It installs Clang 23 or newer, GNU GCC and the i386 development libraries on a native Linux x86-64 runner, and initializes the pinned binary corpus. `unittests/Inputs/lift-qualification.json` retains every original test identity. `scripts/run_lift_qualification.py` rejects missing, duplicate, disabled, skipped or failed cases and a nonzero process exit; `qualification.json` records individual passes, source/corpus revisions, compiler versions and binary hashes.

The profile executes native i386/x86-64 controls, x87 extended precision, and generated Swift SSE result, state and unmasked-trap checks at both optimization levels. The AArch64 parity controls decode the actual `.text` of a pinned Rust runtime. Cross-target decoding does not establish native execution on that target; use the separate source-patch native process jobs for Windows/Linux x64/ARM64 and the local macOS checks.

```bash
python3 scripts/run_lift_qualification.py \
  --binary build-release/bin/NeverDLiftTests \
  --neverd build-release/bin/neverd \
  --output build-release/lift-qualification
```

The native profile also requires the LLVM-C wide-integer constant/store checks and native i386 synthesized-table execution. Swift signal observers use independent raw storage, with positive controls and separate negative controls for premature state and result writes at both optimization levels. Observing an active Swift `inout` variable from the signal handler would instead trigger an exclusivity failure and invalidate the exception observation.

The native Linux x64 job also requires three C++ exception replacement checks: Clang IR, Clang native objects and GCC native objects. Their XML and process exit are audited separately, and any failure or skip fails qualification; NeverC is not used for these C++ inputs.

The native qualification job builds the pinned LLVM fork from source, including the scoped x64 PLT/AArch64 specifier correction. The r4 prebuilt alone does not qualify the Clang IR exception route.

## Ordinary Darwin whole-file lock coverage

`DarwinAdvisoryLockOptions` and `DarwinAdvisoryLockTest` check complete unique initial objects, shared byte budgets, both page sizes, low carriers, action priority, readonly EX, reentry, failed upgrades, downgrade, missing versus empty providers, FD/kind ordering, duplicate/fcntl/dup2 lifetime, descriptor reuse, hardlinks, rename, held zero-name unlink and creation refusal. Real VM mappings and retained Contents cannot keep a closed description’s lock; mode/ownership/data/xattr changes keep independent authority and metadata validity. A contended blocking request stops before model mutation. Strict C/CLI/Python and the original SDK-free77-record workload cover five software and three matching HVF profiles, including empty-provider boundaries. Native-common64 and mandatory79 per platform require all237 matching ARM64 native identities to execute successfully. The720 original ARM64 O0/O1/O2 literal observations comprise522 action and198 namespace/descriptor/VM-lifetime controls. Intel native, physical iOS, external peers and wait lifecycle remain separate unverified coverage. Native5s, guest/Python5,000,000us/quantum1024 and public10s remain unchanged.


## Ordinary Darwin file synchronization coverage

`DarwinSynchronizationOptions` and `DarwinSynchronizationTest` check complete initial grants, shared byte budgets, 4KiB/16KiB pages, three upper carriers, readonly/writeonly/readwrite descriptions, missing versus empty providers, FD/kind ordering, dup/fcntl/dup2 and FD reuse, runtime links/rename/held zero-name unlink, recreated-object refusal, real private mappings and retained Contents. Write/truncate, mode/ownership, invalid stat/xattr observations and advisory locks retain their independent state. Both immutable query domains close all57 other logical routes. The original SDK-free68-record workload and C/CLI/Python preserve20 guarded full stat observations and separate main40/stdout S across five software and three matching ARM64 HVF profiles. Native-common68 and mandatory83 per platform require all249 matching ARM64 identities to execute successfully. Original O0/O1/O2 native captures contain504 literal observations with guarded176-byte stat records, cursors, raw/libc paths and three high carriers. Directory/pipe captures do not expand the ordinary provider contract. Native5s, guest/Python5,000,000us/quantum1024 and public10s are unchanged. Native Intel, physical iOS, durability, full cancellation, provider failures and asynchronous waits remain unverified or unsupported.


## Ordinary Darwin data synchronization coverage

`DarwinDataSynchronizationOptions` and `DarwinDataSynchronizationTest` execute complete initial admission/shared budgets at4KiB/16KiB, low FD carriers, readonly/writeonly/readwrite, FD/kind/provider ordering, missing/empty environments, dup/fcntl/dup2/reuse, runtime names/held zero-name/recreation, real VM and retained Contents, committed write/truncate records/cursors, invalid stat/xattrs, mode/ownership and locks. The existing `DarwinSynchronizationTest.DataGrantCannotSupplyFileCompletionProvider` and new `FileGrantCannotSupplyDataCompletionProvider` cases execute file-only/data-only/both/empty providers. Neither purpose restores observations or supplies the other's grant. Both query domains close all57 other logical routes. SDK-free41 records/11 guarded full stat observations/main41/stdout D use five software and three matching HVF profiles plus C/CLI/Python. Native-common68/mandatory83 require all249 matching ARM64 identities to actually pass. Original O0/O1/O2 controls retain252 literal observations with176-byte guarded native stat/cursors. Native5s, guest/Python5,000,000us/quantum1024 and public10s remain unchanged. Native Intel, physical iOS, physical durability, general cancellation, provider failures and asynchronous waits remain unverified or unsupported.


## Created ordinary Darwin completion coverage

DarwinCreatedSynchronizationOptions and both4096/16384-page DarwinCreatedSynchronizationTest cases execute complete policy prerequisites, independent purposes/defaults, initial/creation separation, readonly/dirty data, carriers, literal created metadata, write/truncate state, runtime links/rename/held removal/recreation, dup/fcntl/dup2/reuse, real VM retention, failed descriptor/entry/byte/inode admission and unchanged unrelated authorities. CreatedRegularSynchronizationKeepsIndependentProvidersAndState is mandatory on every available matching ARM64 transport. ProcessReport strict flag/policy cases run in NeverDLinuxProcessTests; C/CLI and the complete Python method preserve147 records/33 guarded full stat observations/main42/C through five software profiles. Common68/mandatory83 per platform require all249 matching ARM64 identities to actually pass. The702 original O0/O1/O2 controls observe each fresh unflushed target itself. The SDK-free first native failure is retained: raw created F_GETFL65538 exposed an invalid hard-coded2; corrected checks capture the initial flags and verify access bits and preservation. The first model process failure is also retained: its existing FileWasWritten0x10000 owner produced post-write65538. C++/C/CLI/Python expectations include that existing write history and preserve whole flags; no new status-bit representation is introduced. Native5s, guest/Python5000000us/quantum1024 and public10s remain unchanged. Intel native, physical iOS, physical durability, full cancellation and provider/wait lifecycle stay unverified or unsupported.


## Created ordinary Darwin advisory lock coverage

DarwinCreatedAdvisoryLockOptions and both4096/16384-page DarwinCreatedAdvisoryLockTest cases cover complete policy prerequisites, shared byte budgets, independent defaults/initial objects, existing O_CREAT and replacements, persistent status on initial/created descriptions, failed/unsupported actions, F_SETFL history roundtrip, carriers, complete dirty state, dup/fcntl/dup2/FD reuse, runtime names, actual mapping and final-description release, failed FD/entry/byte/inode admission, directory/link exclusion and independent authorities/unknown observations. CreatedRegularAdvisoryLocksKeepProviderAndDescriptionStatus is mandatory on available matching ARM64 transports. ProcessReport strict tests run in NeverDLinuxProcessTests; C/CLI and the complete Python method verify131 literal records/twelve guarded stat observations/main43/L through five software profiles. Native-common68/mandatory83 require all249 matching ARM64 identities to actually pass. Existing SymbolicDescriptorTruncateAndControlShareOnlyDescriptions runs both pages with ENOTTY25 for recognized history, unchanged actual history/stat/cursors, and genuine unsupported flags. Opaque stream requests return zero without acquiring history. Separate162 native symbolic/pipe controls prove these exact results. Original486 flock observations plus72 fcntl checks at O0/O1/O2 prove persistent FWASLOCKED0x4000, original bounds, whole176-byte stat/content/cursors and real mapping lifetime. Retain the first native assumption failure and repair the existing status owner; success sets history, unlock keeps it, and errors/refused waits do not manufacture it. Skips remain unexecuted; full timing stability, Intel native, physical iOS, external peers and wait/provider lifecycle remain unverified.


## Created ordinary Darwin attribute coverage

New ordinary regular files can receive independent mode and ownership providers through an explicit declaration. Each flag defaults to false and requires a complete creation policy when true. Existing objects keep separately declared grants; reused names transfer no authority. Successful attribute changes use mutation_time as virtual ctime. Both inactive ownership IDs still require a provider and preserve every byte. Unknown metadata or group membership remains unsupported.

DarwinCreatedAttributeOptions and DarwinCreatedAttributeTest register three admission/default cases and thirteen cases on each of 4096/16384-byte pages: 29 identities. They check independent flags/initial times, existing O_CREAT, fresh replacements, dirty bytes/cursors/status, three high carriers and all mode/owner routes, readonly/duplicate/independent descriptions, inactive ownership and active equal IDs, errors/unknown security or groups, names/held unlink, private mappings, failed FD/entry/byte/inode admission, directory/link exclusions, independent authorities and invalidated metadata. CreatedRegularAttributesKeepIndependentProvidersAndObjectState is mandatory on available matching ARM64 transports. Strict JSON runs in NeverDLinuxProcessTests. C/CLI and one whole Python SDK method check five software profiles: 235 authored common services/main44/stdout A/137 guarded stat observations and nine virtual services/main45/320 independent literal guarded bytes. Process and SDK success cases each use two fresh common and virtual runs. Virtual GID31 explicitly declares effective groups20/31; real GID31 alone proves nothing. Common native69 and mandatory84 per platform require all252 matching ARM64 identities to pass. The complete Darwin owner must discover3098 identities (2090 runnable/1008 unavailable), with broad C++ owners requiring1808 passes; actual receipts determine qualification. Fresh O0/O1/O2 native controls record450 mode and720 ownership observations at native5s with whole176-byte guarded stat records. Native timestamps are not literal virtual ctime; SDK-free native execution proves main44/A without a captured service trace. Preparation/compile failures and older HVF time-limit failures remain evidence. Whole-method revalidation, if required, is an explicit aggregate without a timing-stability claim. Guest/Python5000000us/quantum1024 and original bounds stay unchanged. Intel native, physical iOS, full frameworks, root privilege, general DAC and host provider lifecycle remain unqualified.

The first process expectation failure is retained:136 assertions omitted upper raw syscall or FD carrier bits. Fresh expectations derive the complete raw carriers from the unchanged workload; optional FD expectations also check literal UINT64_MAX instead of conflating it with an absent check. Runtime decisions, scalar/errno results, guarded records and timing bounds are unchanged.

The complete-owner first round retained two x64 stack-budget failures. Compiler inlining reserved13688 bytes on every main branch, so the unchanged four-page stack test selected an unmapped page below SP and could not release budget. Only the two new workload helpers now use noinline; main reserves56 bytes, and all eight runnable original budget cases pass without a larger budget. Later guest time-limit failures remain retained. Five fixed-length loops in the shared stat helpers use compile-time full unrolling: every original guard, mask, scalar/error check, all137 complete observations and all235 raw calls remain. The runtime implementation, guest5s, method/owner bounds and scheduling policy remain unchanged. Failure diagnostics retain mode/repeat, instructions, PC and completed service count. Isolated entitled CLI completion does not establish timing stability or prove the cause of another host timeout.


## Ordinary Darwin full and barrier completion coverage

Explicit full and barrier synchronization providers independently complete operations on ordinary regular objects. Initial object grants and newly created object grants are separate; creation flags default to false and require a complete creation policy when true. Completion preserves every model observation and ignores the unused argument. Barrier support is direct, without mount promotion or fallback. Neither declaration flushes host storage or proves physical durability.

DarwinFullSynchronizationOptions and DarwinFullSynchronizationTest register three admission cases and twelve parameterized cases over4096/16384-byte pages and commands51/85:51 identities. They cover initial/created defaults and policy prerequisites, shared reference budgets, access modes, raw upper carriers, ignored invalid pointers, bad/closed FDs and kind/scope ordering, missing/empty/other-purpose grants, duplicate/fcntl/dup2/reuse, links/rename/held removal/recreation, real VM retention, dirty bytes/truncate/mode/ownership/locks/xattrs, invalid metadata, directory/link exclusion and failed admission. FullAndBarrierSynchronizationKeepIndependentObjectState is mandatory on each available matching ARM64 transport. The SDK-free common workload has148 authored services,38 guarded full stat observations and separate main46/stdout F. Its native execution establishes46/F without a captured service trace. Virtual-only creation uses ten services, separate main47 and320 independent literal guarded bytes; this is not a native timestamp oracle. Process and Python success paths use two fresh common and virtual runs across five software profiles; C/CLI checks strict independent success and missing/empty providers. New strict JSON cases run in NeverDLinuxProcessTests. Common native70 and mandatory85 per platform require all255 matching ARM64 native identities to execute successfully. The complete Darwin inventory is3169 identities; expected2149 runnable and1020 unavailable identities must be reconciled against actual execution receipts. Broad public/Linux/service/budget/safety/integration owners and nineteen complete Python methods are separate acceptance checks. Fresh O0/O1/O2 native controls record216 raw calls and648 guarded176-byte whole-stat comparisons, including known native pipe EBADF9, low carriers and ignored arguments. Those controls establish observed completions, not whether a native mount used direct barriers or promotion, and not physical durability. Retain initial test-only failures: write access had no explicit write grant, and exhausted virtual inode admission is unsupported rather than a guessed ENOSPC28. Original guest/Python5000000us/quantum1024, native5s, public10s, registered method and owner bounds remain unchanged. Older stack and HVF timing failures remain evidence. Native Intel, physical iOS, default-fatal child cleanup, provider/wait lifecycle and complete Apple frameworks remain unqualified.


## Explicit ordinary timestamp mutations

Explicit timestamp providers independently authorize ordinary regular objects. Initial grants and grants for successful new regular creation are separate. The creation flag defaults to false; true requires a complete creation policy. NULL requests use only a separately declared current-time sample. Requested time storage and fixed virtual ctime are distinct observations. No host clock or filesystem is consulted, and the complete macOS/iOS framework environment remains incomplete.

DarwinTimestampMutationOptions and DarwinTimestampMutationTest cover initial/created grants, shared catalogue limits, explicit and NULL requests, signed microseconds and seconds saturation, nonnegative birth boundaries, request/error ordering, effective ownership and known/unknown group permissions, whole guarded stat records, dirty bytes, offsets, descriptor flags, lock/xattr independence, unknown metadata, hardlinks/rename/held unlink, failed creation and real VM retention. C++ process, strict JSON, C/CLI and one whole Python method cover five software profiles and three required matching ARM64 HVF profiles. Earlier batch counts are historical observations; the timestamp batch required71 common workloads and86 mandatory methods per platform. Every discovered matching ARM64 native identity must actually pass for complete owner acceptance. Native captures are evidence on their test filesystem, not a universal storage rule. Intel native, physical iOS, general setattr lifecycle, dyld/Mach IPC/ObjC/Swift/Foundation/UIKit and full OS execution remain unqualified.

## Explicit filesystem observations

Explicit filesystem snapshots independently supply capacity, type and mount-name observations for regular and directory objects. Zero values and empty names are valid. No host filesystem is consulted. Initial bindings follow actual objects through duplicate descriptors, moves and held unlink; replacement names inherit nothing. Successful new regular creation receives its actual bound parent’s snapshot only through the explicit creation flag and complete creation policy. This provides a usable file-environment query surface; the full macOS/iOS framework environment remains incomplete.

DarwinFileSystemOptions and DarwinFileSystemTest cover exact wide/signed fields, UTF8/empty/maximum names, full guarded records, low carriers, missing/empty/other-purpose declarations, path/FD/output order, partial output without effects, independent target metadata and access flags, effective known/unknown group search, root/special/unknown ancestors, implicit directories and followed links, CWD, separate opens/dup/fcntl, same-ID noninheritance, hardlinks/rename/held unlink/reuse, explicit/default/unbound creation, directory moves/default-created-directory exclusion, dirty bytes/locks/xattrs and real VM retention. C++ process, strict JSON, C/CLI and one whole Python method execute both authored common and independent virtual modes across five thin profiles; all three matching ARM64 HVF identities are mandatory. Full owner acceptance reconciles discovered identities and actually passed native requirements. All prior timing and test-input failures are retained in immutable receipts. Source/products are pinned and heavy work is serial under the existing acceptance lock. Old batch totals remain historical; current native registration is72 common workloads and87 mandatory methods per platform. Native Intel, physical iOS, default-fatal child cleanup, provider wait lifecycle and complete Apple frameworks remain unqualified.

`DarwinCreatedAttributesPublic.PreserveProvidersAcrossCAndCLI` registers five platform cases separately, preserving all 180 C API/CLI executions under the original 30-second case limit. `DarwinCreatedRegularAttributesRejectMalformedProvidersBeforeLoading` retains the ten invalid inputs. Mobile policy regressions check unchanged text and reject altered directives; scalar, calls/Blocks and Swift recovery must rebuild and execute all twelve variants, followed by `test_mobile_ios_single_session.py`.

`SourceABI.SourceCallsReplaceGenericRegisterArgumentSummaries` in `NeverDObjCSourceCallTests` checks source argument order, narrow lanes and Swift context carriers on x86-64 and ARM64 after generic call summaries are available. `SourceABI.UnboundSourceCallsRetainGenericArguments` checks disabled, missing and invalid bindings plus explicit LowIR and formatted-call operands. `SourceABI.PrototypeArgumentCountsDoNotReplaceExplicitOperands` retains explicit COFF ARM64 arguments even when import prototype counts match the source declaration. The mobile matrix must still recover, compile and independently execute every original/rebuilt variant; complete method counts alone do not establish matching behavior.

## Declared mount enumeration

Declared mount enumeration now provides ordered cached observations and explicitly supplied successful refreshes. Each process keeps its own cache. Duplicate records remain separate, and an empty table is distinct from an unknown table. This supports another common filesystem query used by Apple applications; dyld, Mach IPC and the full Objective-C/Swift and Foundation/UIKit environment remain incomplete.

DarwinMountEnumerationOptions and DarwinMountEnumerationTest exercise both 4KiB and 16KiB pages, the shared entry/byte budgets, exact independent record bytes, duplicate order, signed and high size carriers, unknown versus empty tables, NULL and short outputs, flag precedence, partial records, late missing refresh and complete faults, earlier committed effects, cache reset and independent statfs/fstatfs observations. Strict JSON tests reach both nested snapshot parsers and preserve the separate 64KiB envelope. The SDK-free common workload checks count, truncation, flags, guarded records and native error ABI; the virtual workload emits 19609 independently authored bytes across three snapshots plus M. C++ process, C/CLI and Python exercise five thin profiles and process reuse, with three matching ARM64 HVF identities mandatory. The new common native registration is73 and required method registration88 per matching platform. Native Intel hardware, physical iOS, default-fatal child cleanup and full Apple frameworks remain unqualified.

## Swift scalar instruction completion

Recovered x86_64 Swift supports scalar ADD/SUB/MUL/DIV in register and memory forms, signed 32/64-bit conversion to Float32/Float64, and Float32/Float64 conversion in both directions, including VEX forms. Numerical bits and complete incoming/outgoing MXCSR remain paired. Packed arithmetic, SQRT/MIN/MAX, FMA, rounding and approximate operations remain unsupported; the full macOS/iOS framework environment remains incomplete.

Mixed legacy/generic and legacy/VEX cases check one helper per numerical/state contract. Independent original SSE observers and literal results compare generated Swift at Onone/O for rounding, sticky flags, DAZ/FTZ, subnormals, signed zeros and NaNs. Register/memory controls also cover all scalar conversion directions. Unmasked arithmetic and conversion traps require native Linux x64 or Intel macOS and unchanged result/state sentinels; ARM64 hosts explicitly skip those trap checks because Rosetta masks exceptions.

## Explicit ordinary directory modes

Explicit mode providers now support ordinary directories as well as regular files. Initial directory grants and the separate opt-in for newly created directories remain independent of namespace mutation and child objects. The complete dyld, Mach IPC, Objective-C/Swift and Foundation/UIKit environments and physical iOS verification remain unfinished.

DarwinModeTest covers both4KiB and16KiB pages with complete independent stat records, changed/equal permissions, strict authority, alias admission, independent initial/created grants, high carriers, errors, cursors, direct moves/exchanges and both namespace/mode orders. The focused61 checks passed. Earlier complete-owner rounds retained two obsolete directory-target expectations and intermittent legacy mount-enumeration/created-attribute HVF guest timeouts. Final required method revalidation uses the original complete method, unchanged source and binary hashes, and original method/guest deadlines; earlier failures remain evidence. DirectoryModesPreserveIndependentObjectAndNamespaceState is mandatory on all three available matching ARM64 HVF profiles. Five thin software profiles also pass process, C/CLI and Python, including fresh runs in a reused public session. Native common inventory74 and mandatory inventory89 require267 matching ARM64 executions; the complete Darwin owner reconciled3419 identities with2351 passed and1068 unavailable hardware identities across four disjoint serial shards plus full-method revalidation of retained guest timeouts. This is a composite qualification, not a single unsharded timing-stability claim. A retained HVF run reached the unchanged guest5-second limit during complete stat verification under high observed host load; load is contextual evidence, not a proven sole cause. Equivalent word comparisons and fully unrolled fixed-length fixture loops retain all33 records, complete fields and guards, raw calls, errors and independent virtual times. The qualified fixture reduces checked guest instruction work without changing runtime semantics, scheduling or deadlines. Earlier invalid-fixture, registration-controller and build-target errors remain retained. The first captured-dev Lift rebuild reached its original1200-second deadline; the incomplete attempt is retained and the final incremental build is separately qualified under the same bound. Intel hardware, physical iOS and the complete Apple framework environment remain unqualified.

## Explicit ordinary directory timestamps

Explicit directory timestamp providers now support utimes and futimes on independently granted initial and newly created directory objects. They preserve namespace, mode and ownership authority. Complete Apple runtimes and frameworks, and physical iOS verification remain unfinished.

The97 initial private/strict JSON checks passed before route addition. Directory timestamp cases cover4KiB/16KiB, full literal stat144 with guards, initial/created grants, all time/namespace orders, aliases, effective owner/nonowner decisions, dup/cursors, direct move/exchange, held removal/reuse and permanent invalidation. Five thin software process/C/CLI/Python profiles and all three matching ARM64 HVF profiles passed with fresh executions; foreign hardware is unavailable coverage. The complete Darwin owner reconciles3469 identities with2389 passed and1080 unavailable, including all270 mandatory matching ARM64 executions, across four disjoint serial bounded shards. This is not a single unsharded timing-stability claim. Shared public/Linux/service/budget/safety owners passed1845 cases, with existing timestamp and filesystem Python regressions. The native common inventory is75 and mandatory inventory is90 per matching platform. The preparation controller's ignored-plan assertion, one isolated test's retained movable capability without mutable parent, and a stale regular-object diagnostic expectation are preserved failures corrected in fresh epochs. Native5s, compiler120s, component1200s/parallel2, original method bounds, guest5000000us/quantum1024, public30s and other established limits remain unchanged. Physical Intel/iOS, nonowner/special native security and complete Apple runtime/framework execution remain unqualified.

## Explicit ordinary directory ownership

Explicit ownership providers now support chown, fchown and fchownat on independently granted initial and newly created ordinary directories. Group changes update the actual parent identity used by child creation. Namespace, mode and timestamp authority remain independent. Complete Apple runtimes and frameworks, and physical iOS verification remain unfinished.

Initial focused private and strict JSON checks passed103 identities before route addition. New directory cases cover4KiB/16KiB, complete literal stat144 with guards, independent initial/created grants, actual current parent group inheritance for all3routes and all3child kinds, aliases, active/inactive IDs, authorization and group knowledge, high carriers, errors, dup/cursors, moves/exchanges, held removal/reuse and permanent invalidation. Five software process/C/CLI/Python profiles and all three matching ARM64 HVF profiles passed fresh executions. The complete Darwin owner reconciles3511 identities with2419 passed and1092 hardware-unavailable, including all273 mandatory matching ARM64 executions, across four disjoint serial bounded shards plus37 fresh isolated replays of the initial guest-time-limit failures. Every replay passed on the same source/products and original bounds. This is not a clean full-round or unsharded timing-stability claim. Shared public/Linux/service/budget/safety owners passed1852 cases, together with affected Python attribute/filesystem regressions. Native common inventory is76 and mandatory inventory91 per matching platform. The earlier complete Darwin round retains guest-time-limit failures during high observed host load; the final complete-inventory qualification uses unchanged source, products and bounds, combining the original disjoint shards with focused replays. Native5s, compiler120s, component1200s/parallel2, original method bounds, guest5000000us/quantum1024 and public30s remain unchanged. The new negative-membership fixture correction and a guest-deadline failure from redundant end-to-end Cartesian snapshots are retained in immutable epochs; the final workload keeps independent full typing proof and unchanged bounds. Physical Intel/iOS, native nonowner/special security and complete Apple runtimes/frameworks remain unqualified.

## Symbolic ownership regression

`DarwinSymbolicOwnershipTestData.h` supplies independently authored complete stat144 records with two8-byte guards. `DarwinOwnershipTests.cpp` covers exact initial/created authority, effective credentials and known/unknown groups, complete-record preservation/commit, descriptor versus SEARCH error order, flags/carriers, dangling/cyclic targets, alias/dup/move/exchange/held-unlink/reuse and permanent metadata invalidation. Strict JSON checks preserve purpose-specific Booleans, defaults, creation prerequisites and lossless timestamp import.

The SDK-free `darwin_process.c` ownership modes use59/60 authored common calls and67/68 literal calls, return58/W and emit1281 bytes in each literal mode. The eight full guarded records distinguish original link, unchanged/changed target, two-name/renamed/held link and fresh replacement. Process tests repeat positives from fresh state and retain early/late unsupported outcomes with exact raw service/scalar argument checks. Native catalogue seeding explicitly creates a symbolic input and a separate binary target. Original owned nonroot O0/O1/O2 probes retain318 observations, and common initial/created native workloads execute at all three levels. Common native checks capture no service trace and assert no fixed native ctime.

`SymbolicOwnershipKeepsVnodeAndTargetGrantsSeparate` is required on every matching native platform through `scripts/NativeDarwinTests.def`; inventory now contains94 methods per platform and282 matching ARM64 identities. `DarwinNativeCases.def` contains81 common cases. Execute all five software profiles, all three matching ARM64 HVF identities and C/CLI/Python routes; skips do not count as native acceptance. Preserve guest5000000us, quantum1024, native5s, compiler120s, component1200s/parallel2 and public30s limits. Full Darwin qualification uses the existing disjoint serial method shards, with any bounded unchanged-source recovery reported as a composite. Intel hardware, physical iOS and complete Apple runtimes remain unqualified.

## Filesystem snapshots for created directories

A separate creation flag now binds a new directory to its actual parent’s declared filesystem snapshot, if present. Open descriptors retain that binding through moves and deletion. Complete Apple runtimes, frameworks and physical iOS verification remain unfinished.

Initial focused private and strict JSON checks passed125 identities before route addition. Final cases cover both page sizes/routes, full2200 guarded literal records, independent flags, zero versus unbound parents, initial/symbolic exclusion, actual parent binding after moves/exchanges, child moves, held rmdir/name reuse, failed/exhausted-inode creation, descriptor flags, full stat preservation, group changes and permanent metadata invalidation without path SEARCH repair. Five software process/C/CLI/Python profiles and three matching ARM64 HVF profiles pass. C API successes repeat in reused sessions; CLI reports are compared completely.

Owned nonroot ARM64 O0/O1/O2 probes passed102 observations with complete2168-byte records and guards, primary/secondary/carry, high carriers, error order, CWD, child kinds, moves and held deletion/name reuse. Common native main56/D passes with33 authored calls; virtual-only35 calls emit4400 independent literal guarded bytes plus D. Native comparisons exclude only five dynamic uint64 counters; no native service trace or universal capacity is claimed.

The complete Darwin owner reconciles3565 identities: 2461 passed, 1104 hardware-unavailable, zero failures; all276 mandatory matching ARM64 identities actually pass in four disjoint serial bounded shards. This is not an unsharded timing-stability claim. Shared public/Linux/services/budget/safety owners pass1858 C++ cases. All20 products rebuilt; inventories77 common native and92 mandatory per matching platform verified. Original fixture/discovery failures stay immutable: explicit creation/swap/empty-provider prerequisites and a regular-only stat helper were corrected, without changing production semantics or the33/35-call sequence. Native5s, compiler120s, component1200s/parallel2, guest5000000us/quantum1024, public30s and original method bounds remain unchanged. Apple runtimes/frameworks, Intel native and physical iOS remain unfinished.

## Filesystem snapshots for symbolic vnodes

Explicit filesystem snapshots now support symbolic vnode descriptors, including dangling and cyclic targets. Path queries still follow the target. Created symbolic links have an independent opt-in parent binding. Complete Apple runtimes/frameworks and physical iOS verification remain unfinished.

Initial private/strict JSON checks passed166 identities before route addition. Final coverage uses both4KiB/16KiB pages and both path/FD routes: full2200-byte literal guarded snapshots, link-versus-target declarations with identical IDs, unknown/empty/zero snapshots, dangling/cyclic/nonUTF8 targets, complete namespace prerequisites and all8 independent creation-flag combinations, no target metadata/stat/credential dependence, permanent invalidation, dup/flags/target bytes, moves/exchanged parents, held unlink/replacement, failed creation/inode exhaustion and exact reference budgets/partial copyout.

Owned nonroot ARM64 O0/O1/O2 native probes passed114 observations, full2168 payloads/guards, raw primary/secondary/carry, high carriers, readonly/dup, target errors, parent moves, held deletion/name reuse and unchanged stat/flags/cursor. The common initial32 and created33 authored services pass main57/Y natively; virtual35 emits6600 independent literal bytes plus Y, separating link owner17 from target/replacement owner4294967294. Native comparisons mask only five dynamic uint64 counters and capture no native service trace. Five software process/C API/CLI/Python profiles and three matching ARM64 HVF profiles pass fresh executions; C API successes reuse sessions and CLI reports compare completely.

Complete Darwin composite: 3624 registered, 2508 passed, 1116 hardware-unavailable, zero remaining failures; all279 required matching ARM64 identities actually pass in four disjoint serial bounded shards plus1 fresh isolated replay of the original resource-usage HVF guest-time-limit failure. The initial full round and all failures remain retained. Identical source/products and unchanged limits qualify the composite only; no clean full-round or unsharded timing-stability claim. Shared public/Linux/services/budget/safety owners pass1866 C++ cases. All20 products rebuilt; common native79/mandatory93 per matching platform independently asserted. Earlier compile enum typo, mutable-parent fixture omissions and failed repair matching/controller syntax attempts remain immutable; no production semantics were weakened to pass them. Native5s/compiler120s/component1200s parallel2/guest5000000us quantum1024/public30s and original method bounds are unchanged. Intel native, physical iOS and complete Apple runtimes/frameworks remain unqualified.


### Declared physical memory inventory

`KernelPhysicalInventoryTests` verifies byte units, terminator, independent allocations, admission without effects, declaration copying, capture dependencies, pins, physical identity and shortage. `DriverPhysicalInventoryTests` covers strict transport and full preferred/rebased lifecycle, dynamic lookup and freed reads on Unicorn/KVM/WHP. The unused-declaration UNPACK control preserves refusal. All 13 cases per native backend are mandatory; unavailable platform execution remains skipped locally.

### Declared kernel VA shadow query

`KernelVaShadowQueryTests` checks independent flags, exact/short/cross-page outputs, ULONG truncation, both destinations, overlap, permissions, aliases, objects, context, IRQL and retained dependencies. `DriverVaShadowQueryTests` checks strict transport and imported/dynamic queries through rebased Unicorn/KVM/WHP lifecycles. Unused declarations still refuse UNPACK recovery. All 19 identities per native backend are mandatory; unavailable execution remains an explicit skip. Existing version-query cases cover the shared buffer admission.

## Source patch qualification after integration

The native Linux qualification also runs the complete `HighSwiftEmitter` owner, including register and memory conversions, legacy/VEX helper sharing, original SSE comparisons and unmasked traps. Use the corrected LLVM source build: r4 can misencode i386 PIC GOT anchors; publication now rejects an invalid anchor displacement. Ten Linux/ELF-specific `SessionCAPITest` cases retain explicit platform skips on macOS and Windows. The audit matches their exact names, owner labels and original reasons, continues to require native Linux execution, and never counts these skips as passes. The host-compiled no-return fixture accounts for Mach-O symbol spelling and the recovered native argument width.

### Bounded kernel caller-context capture

`KernelContextCaptureTests` covers all record/guard bytes, stack scratch, flags, aliases, overlaps, objects, permissions, IRQL and injected collector/setter failures. `DriverContextCaptureTests` checks live GPR/FPCW/MXCSR/XMM values and imported/dynamic calls through rebased entry/request/unload lifecycles, including rejected outputs and retained dependencies. Its bytewise twelve-record oracle has an explicit 400000-instruction test budget. The fixture uses the checked initial FPCW; separate collector cases vary it. All 17 identities per native backend are mandatory; unavailable WHP execution remains an explicit Linux skip. Existing x64/ARM64 user context tests protect unchanged behavior.

### Symbolic region coordinates across zero

`SymRegionWrap.*` checks both byte orders, complete and partial accesses across relative zero, nonwrapping input origins, copied unknown epochs, exact and insufficient preservation budgets, and rejection of absolute/scalar overflow. `InterpreterSpecialization.NativeReturnCellProofKeepsStoreAcrossRelativeZero` asserts the symbolic `base - 1` address shape and checks complete STORE effects, native return words and restored stack state with an independent byte oracle. Missing separation proof or entry alignment must still refuse.

### Sequential raw compare-and-exchange

`SymAtomicCmpxchg.*` and `InterpreterSpecialization.AtomicCmpxchg*` cover complete byte effects, both byte orders, old-value provenance, input/output aliases, native return cells, affine-spill invalidation across joins, retained ordered occurrences, strict admission and exact/short budgets. The independent byte oracle models sequential values only. Source-wrapping controls require ordered-memory refusal for initialized, structurally valid graphs. Independent LowIR controls also bind complete sidecars and require no certificate. Native binary proofs may refuse earlier because LOCK CMPXCHG lacks a complete architecture-undefined-output audit. Raw-receipt checks bind ordering, address space and desired operands. Run the owning suites before claiming support; test definitions alone are not execution evidence.

`KernelObjectTests.cpp` checks known-ID lookup, type/access/mode failures, optional handle information, failed outputs without leaked references, unload, and retained identities across stack reuse. `DriverObjectTests.cpp` executes original `driver_objects.c` on available Unicorn/KVM/WHP/HVF backends, Legacy/CheckedX64, preferred/rebased images and cooperative/17-instruction scheduling. KVM/WHP outcomes are mandatory in `NativeDriverTests.def`; missing or skipped execution is a gap. These fixtures do not certify ACLs, general process exit or unimplemented object classes.

`KernelDataExportTests.cpp` checks object identity, availability, thunk capacity, read-only/NX permissions, undeclared debugger bytes and recovery dependencies. `DriverDataExportTests.cpp` runs original `driver_data.c` with real DATA imports across available backends, both contracts and image bases, including unload and rejected writes/execution. Scenario, C ABI, CLI and Python tests cover strict debugger booleans and report transport. Native outcomes are required by `NativeDriverTests.def`; unavailable backends remain coverage gaps.

### Live proc and maintained file metadata

`LinuxProc` checks x64 and AArch64 stat bytes, sparse page allocation, truncation,
clock failure, live FD offsets/CLOEXEC, proc buffering, task disappearance and
mapping-label retirement. `AndroidProc` runs independent O0/O2 callers with
ordinary/APS2/RELR relocation packing on every available transport, including
real cooperative thread creation/join and libc DIR/FILE reads. Run
`ctest -R 'LinuxProc|AndroidProc|AndroidLiveProc'` after building the Linux,
Android and process-public test targets, then the four OS/session suites for
shared-owner changes. `test_android_live_proc_and_maintained_stat` checks Python
against the built SDK; unavailable backends are skips, not successful coverage.

### Native JNI and Binder contracts

`AndroidJNI` executes independent O0/O2 callers with ordinary/APS2/RELR
relocations, validating complete string/array bytes, copy-back modes, exception
cleanup, local/global lifetime, thread attachment and invalid identity/borrow
controls. `AndroidBinder` covers control records, looper responses, shared-open
ownership and per-thread state. `LinuxBinder` checks both LP64 kernel ABIs,
including full records, guards, output aliases and rejected model combinations.
Process-public tests compare complete C API/CLI reports, and Python integration
runs the same guest functions. Build the Android, Linux, process-public and
execution-session targets, run `ctest -R 'AndroidJNI|Binder'`, then their complete
owner labels for changes to shared models. GPU context fixtures also check known
empty queues, unknown external queues and invalid timestamp copy-out. Missing
native transports remain skips, not execution evidence.

`KernelNamespaceTests.cpp` checks typed names, root-relative and case-sensitive lookup, buffer sizing/copy order, captured link tails, permissions, process scope, IRQL, failed-output reference safety and held deletion. `DriverNamespaceTests.cpp` runs original `driver_namespace.c` on available Unicorn/KVM/WHP/HVF, both contracts, both load addresses and cooperative/17-instruction scheduling. `NativeDriverTests.def` requires native outcomes. Unavailable backends and unsupported information/security classes remain coverage gaps.

`KernelRegistryTests.cpp` covers independent opens, typed references, cloned handles, process scope, generic mapping, deleted identities, odd/short name buffers, aliased output ordering and failure/capacity boundaries. Original `fixtures/driver_registry_objects.c` runs through `DriverRegistryObjectTests.cpp` on each available backend, both execution contracts and image bases, with cooperative and 17-instruction scheduling. A deliberate pointer leak verifies unload rejection. Native acceptance requires all four contract/base outcomes.

`KernelDeviceStackTests.cpp` checks current neighbors, borrowed versus retained results, deletion of either query input or neighbor, last-reference retirement, invalid/corrupted inputs and IRQL rejection before retention. Original `fixtures/driver_device_queries.c` runs through `DriverDeviceQueryTests.cpp` on available backends, both contracts and image bases, with cooperative and 17-instruction scheduling. It verifies held storage after detach/delete and rejects unload with a deliberately leaked reference; all four native contract/base outcomes are required.

KernelFileObjectTests and KernelObjectTests cover typed pointer opens, output clearing on type mismatch, generic access, process/global handle counts, callback reopening rejection, failed-route retry, requestor exit, repeated cleanup and pending final close. KernelPnpRequestTests preserves a delayed CLEANUP provider response; KernelFrameworkFileTests checks callback process ownership and LOCK_CONTROL admission. Original driver_file_objects.c keeps its pointer-lifetime coverage. Original driver_file_handles.c checks synchronous/pending CLEANUP and CLOSE, system-worker release, identifier reuse and deliberate leaks with cooperative and 17-instruction scheduling. Genuine WDK driver_kmdf_file_handles.c checks system-process cleanup, restored caller close and callback-free cleanup; configure NEVERD_KMDF_FILE_HANDLES_FIXTURE and optional NEVERD_KMDF_FILE_HANDLES_CFG_FIXTURE. DriverFileObjectTests runs available backends, both contracts and bases. Missing WDK artifacts or backends skip visibly. Filesystem operations require separate coverage. Scenario-originated KMDF CLEANUP/CLOSE retain the calling process, thread and PreviousMode; the F fixture mode closes a retained handle from another file’s CLEANUP and checks restoration afterward.

### Android method, transaction and command execution

Build the Android/Linux/process-public/execution-session targets in Release.
`AndroidJNI` validates ordinary/V/A intrinsic calls, UTF-16 results, constructor
copies, virtual/nonvirtual identity, cached names, exception ordering and invalid
IDs/types. `AndroidBinder` executes guest service replies on cooperative threads;
`LinuxBinder` separately checks full LP64 records and read-only buffer ownership
on x64 and AArch64. `AndroidDevices` and `LinuxProcess` run original PM4 callers,
including retained physical memory after CPU address reuse and refused programs
with no earlier writes. Run focused `ctest -R 'AndroidJNI|Binder|GPUCommands|RejectedGPU|StatefulDescriptorsPages|StatefulDevicesValidate'`,
then the four owning labels. Process-public C API/CLI and Python integration
exercise the same paths; transport unavailability is a skip, never evidence of
execution. These authored workloads do not establish arbitrary application,
Android service or shader coverage.

`KernelFileQueryTests.cpp` covers borrowed and native buffered packets, immediate/pending publication, IOSB aliases and padding, worker completion, retained storage, local classes, handle scope, IRQL/APC admission and unsupported routes. `KernelFileCreateTests.cpp` and `DriverFileSystemTests.cpp` cover six declared metadata classes, exact sizes, bounded names and retained open identity. Original `driver_file_queries.c` and `driver_filesystem_provider.c` run across available backends, contracts, bases and cooperative/17-instruction scheduling; genuine normal/CFG images use `NEVERD_WDM_FILE_QUERIES_FIXTURE`, `NEVERD_WDM_FILE_QUERIES_CFG_FIXTURE`, `NEVERD_WDM_FILESYSTEM_FIXTURE` and `NEVERD_WDM_FILESYSTEM_CFG_FIXTURE`. C API/CLI/Python check origins, full output bytes and handled failure statuses. Bounded original-kernel probes cover 156 producer cases and 18 completion cases with explicit allocator, dispatcher and notification boundaries; they do not certify a complete filesystem or general APC runtime. Missing hardware and external fixtures remain explicit skips.

`KernelIRPStack.ReadWriteCompletionPreservesStoredFilePosition` checks synchronous/asynchronous files, immediate/pending completion, success/warning/error statuses and zero/nonzero lengths against an independent 64-bit position. The genuine normal/CFG `driver_wdm_stack.c` fixture reads the position again across READ, WRITE and cleanup.

## Supplied DEX method execution

`NeverDDalvikTests` checks the decoded executor's uninitialized/torn registers,
overlapping wide moves, division exceptions/overflow, budget/stack stops,
result-use rules, per-workload static state, initialization failures, terminal
resource stops, field widths/final stores, handler/throwable identity and refusal
of malformed handlers or unknown opcodes.
`NeverDMobileTests` must also run when changing the shared DEX reader or models;
mobile recovery and JNI execution share those owners.

For an independent Java compiler/runtime oracle, prepare the JNI fixtures and
`neverd-process-probe`, then run:

```sh
python3 scripts/test_android_dex_runtime.py --build build-release \
  --jdk /path/to/jdk --d8 /path/to/build-tools/lib/d8.jar \
  --backend unicorn --output /path/to/new-dex-run
```

Repeat with `--backend hvf` on supported macOS hosts. The original Java is
compiled to class files for the JVM oracle and to DEX for NeverD. Cases cover
overflow, negative/oversized shifts, signed division/remainder, narrow returns,
wide arguments, branches, packed/sparse switches, loops, recursion and
cross-class calls, through ordinary/V/A JNI forms. Negative workloads cover
division exceptions, loops exhausting the shared native budget, recursion,
unknown Object methods, corrupt/duplicate DEX and JNI-disabled admission.
The formerly unsupported arithmetic handler now has a positive result oracle. Logs and
request/report files stay in the explicit output directory. Missing JDK/D8 or
CPU transports are unavailable evidence, not a passing substitute. The runtime
itself does not launch these preparation tools or use a device.

Run `scripts/test_android_dex_state.py` with the same arguments for persistent
class state. The original Java oracle emits every sequential return value and
exception identity; the native fixture records the same sequence through JNI.
Cases include encoded/default values, all integral field widths, dynamic final
initialization, both orders of cyclic initialization, ordinary division failure
without rollback, failed initializer effects without retry, nested initializer
errors and subsequent NoClassDefFoundError. Each sequence runs with retained JNI
attachment, repeated detach/reattach, and alternating joined guest threads.
Volatile fields and unsupported or looping initializers retain explicit
failure boundaries. These are supplied synthetic programs, not evidence for
unavailable application DEX, Binder services or shaders.

Run `scripts/test_android_dex_objects.py` with the same arguments for supplied
objects and arrays. Its independent JVM oracle covers every integral JNI field
width, object/static aliases, native-to-DEX and DEX-to-native mutation, constructor
and method call forms, nested arrays, primitive payloads, global references and
field IDs across reattachment, and five recoverable Java exception classes.
Separate negative calls check accessor mismatch, final writes, bad receivers,
forged field IDs and model heap limits. Decoded-model unit cases independently
check forged register references, constructor ordering/aliasing, array payload
bounds, component types, cyclic field storage and terminal resource stops.

Run `scripts/test_android_dex_language.py` with the same JDK/D8/backend flags
for strings, library execution, handlers and inheritance. Original class files
run on the JVM; D8 supplies the distinct instruction stream executed by NeverD.
Cases compare exact UTF-16 units and hashes, literal/intern identities, message
identity, ordered catch/rethrow/finally side effects and repeated unwinding.
JNI bridge cases compare complete output arrays for virtual/interface/nonvirtual
calls, inherited fields, class identity, Throwable round trips and references
after reattachment. Unknown library and synchronized calls inside catch-all
regions must stop rather than return a guessed success. The separate sequence
oracle resets its explicit side-effect input between calls for adapter reuse;
its preparation is not itself a NeverD execution claim.


Processor regressions cover output boundaries, NULL, nested restores, invalid IRQL and import identity. Original `driver_processors.c` exercises entry, worker and DPC contexts across backends, contracts, bases and scheduling. Optional `NEVERD_WDM_PROCESSORS_FIXTURE` and `NEVERD_WDM_PROCESSORS_CFG_FIXTURE` verify genuine WDK inline calls and dynamic exports with normal/active CFG images; missing transports remain explicit skips.

Integer formatting regressions verify full-width values, all admitted radices, exact capacity, invalid-base ordering, output aliases and terminator faults. Original `driver_int64_strings.c` checks imports and dynamic lookup in entry, worker and unload, plus elevated-IRQL rejection. Optional `NEVERD_WDM_INT64_STRINGS_FIXTURE` and `NEVERD_WDM_INT64_STRINGS_CFG_FIXTURE` cover genuine WDK normal/CFG images across available backends, contracts, bases and scheduling.


`DriverFileSystemTests.cpp` and `KernelFileSystemTests.cpp` cover admission limits, required zero values, exact owner snapshots, pre-entry multi-volume identity, simultaneous PnP devices, route rejection before IRP adoption, protected projection and unload leak detection. `DriverFileSystemExecutionTests.cpp` runs original and genuine WDK normal/CFG fixtures across available backends, contracts, bases and scheduling; configure `NEVERD_WDM_FILESYSTEM_FIXTURE` and `NEVERD_WDM_FILESYSTEM_CFG_FIXTURE`. `DriverScenarioPublic` and `test_driver_filesystem_integration.py` check C API/CLI/Python state on success and stop. The Python test uses `NEVERD_TEST_DRIVER_FILESYSTEM_FIXTURE` and `NEVERD_TEST_LIBNEVERD`. `UnusedFileSystemDeclarationCannotAuthorizeRuntimeRecovery` checks unused empty declarations. Unavailable backends remain explicit skips.

`KernelFileCreateTests.cpp` covers packets retained until producer return, sharing failures, relative roots held across pending work, explicit cancellation, failure/aliased publication and DriverUnload teardown. `DriverFileSystemExecutionTests.cpp` executes original and genuine WDK normal/CFG callers across both contracts, bases and available backends, with cooperative and instruction-clock scheduling. C API/CLI/Python preserve the same file identities. Unavailable backends remain skips. Original kernel completion probes distinguish retained CREATE/cancel packets from ordinary consumed IRPs; their event and APC boundaries do not establish a complete filesystem or kernel.

### KGSL object storage and lifetime

Build `NeverDMemoryLifecycleTests`, `NeverDLinuxProcessTests`,
`NeverDAndroidNativeTests`, `NeverDProcessPublicTests` and `neverd-process-probe`.
Run `ctest -R 'UnmappedBacking|GPUObject|GPUCommands|RejectedGPU|StatefulDevices|StatefulDescriptorsPages'`.
The original C fixture checks zeroed allocation, unaligned-size rounding, full
query records, shared independent opens/dup, CPU/PM4 coherence, remapping,
pending free, last-FD closure, final-unmap retirement and zeroed reuse on both
LP64 kernel ABIs and Bionic call routes. Refusal checks preserve complete
records and data for metadata, flags, read-only output, split VMAs and async
free. The shared RAM test verifies capacity and identity without a virtual
mapping. C API/CLI and Python integration use the same authored guest.
Broaden to the memory-lifecycle, Linux-process, Android-native, execution-session
and process-public labels before publishing changes to these shared owners.
The raw `GPUObjectStorage` case and Android object lifecycle/address-range cases
have a 30-second per-execution wall budget for word-by-word O0 page checks on
contended hosts; their guest instruction and event budgets are unchanged.
Existing device cases retain their deadlines.
`LegacyGPU*` adds old/new allocation, query and release mixing, normalized
alignment and FORCE_32BIT flags, address collision recovery, ID-before-address
lookup, interior data versus guard queries, complete reserved fields and pending
frees. Its mixed PM4 case allocates the command buffer through GPUOBJ and writes
into GPUMEM_ID storage. Cross-page records with a read-only trailing page must
preserve the complete input and live RAM; imported objects without a modeled
kernel ID stop explicitly. Raw errno and Bionic errno adapters are checked by
independent callers. Run these cases alongside the existing GPUObject tests;
the same public C API/CLI/Python case now includes both allocation interfaces.
The kernel-owner test also observes allocation capacity, ID/address reuse and
unmap-after-refused-free state, so unchanged output bytes alone cannot hide a
partially committed allocation or pending free.
These fixtures do not supply an Android system-service implementation or shader.

`GPUCommandsWritePinnedRAMAndRetireOwnedTimestamps` also executes both A3xx
indirect-buffer forms, repeated child calls, parent continuation and pinned
child fetches after CPU address reuse. The raw x64/AArch64, C API/CLI and Python
paths use that same independent caller. `RejectedIndirectGPUProgramsPreserveAllFetchedRAM`
checks child and parent failures, recursive/deeper calls, aggregate budget,
physical aliases, empty/truncated/unmapped/misaligned buffers and permissions.
It compares complete fetched pages with pre-submission copies, destination RAM,
the result timestamp and both memstore timestamps, plus the refusal location.

`AndroidBinder.GuestServicesExecuteSynchronousTransactionsAndReplies` runs
ordinary and TF_ACCEPT_FDS requests with both data and guest-computed
TF_STATUS_CODE replies, including exact guards and buffer release. The LP64
`LinuxBinder` test checks rejected flags/status sizes/objects, unchanged arenas
and consumed counts, and a successful retry of the same pending call. These
checks validate message transport, not any Android service's response policy.
`LinuxBinder` also checks page-rounded arenas through 4 MiB, both ends' read-only
permissions, failed protection upgrades, dup/close/unmap lifetimes, oversized
refusal without allocation and retry after memory-budget exhaustion.
`AndroidBinder.LargeArenasRetainMultipleCopiedRequestsAndReplies` keeps two
64 KiB requests live in one larger arena, verifies the second buffer beyond
64 KiB, and executes guest-produced replies and buffer release at O0/O2 across
all Android relocation forms and available CPU backends.
Run `ctest -R 'Binder|GPUCommands|RejectedIndirectGPU|RejectedGPU|StatefulDescriptorsPages'`,
then the five OS/memory/session owner labels described above.

`KernelFileTransfers` covers packet fields, offset sentinels, append-only handles, zero lengths, aliased outputs, immediate and pending statuses, worker notification and refusal before publication. `OriginalBufferedNativeFileTransfers` and `GenuineWdkBufferedNativeFileTransfers` execute imported and dynamically resolved calls across backends, contracts, load bases and scheduling. Configure `NEVERD_WDM_FILE_TRANSFERS_FIXTURE` and `NEVERD_WDM_FILE_TRANSFERS_CFG_FIXTURE` for genuine normal/CFG images; unavailable backends remain explicit skips.

Checked CRT regressions cover every byte and UTF-16 unit, carrier narrowing, readonly/no-change buffers, count/capacity boundaries, truncation, null ordering, sequential aliases, partial faults, freed storage and the scan bound. Original `driver_crt_strings.c` and genuine `NEVERD_WDM_CRT_STRINGS_FIXTURE` / `NEVERD_WDM_CRT_STRINGS_CFG_FIXTURE` exercise imports and dynamic lookup in entry, worker and unload, at passive/dispatch IRQL and both scheduling modes. The native gate requires both contracts and load bases; unavailable host backends remain explicit skips.

Checked memory regressions distinguish full-capacity clearing from unchanged error output, zero-count pointers, full-width counts, overlap, paged/freed/read-only storage and operation limits. Header probes cover unsigned DOS offsets, 64-bit results, the address fence, signature-only acceptance and fault ordering. The original and genuine WDK CRT fixtures exercise imported/dynamic calls in entry, worker and unload. The WDK fixture uses a local ABI declaration for `RtlImageNtHeader` because the pinned headers omit that export; PE types and import libraries remain from the genuine kit. The pinned WDK ntoskrnl.lib links memcpy_s as guest code. The original fixture verifies its direct import, while both fixtures verify dynamic export calls.

`KernelFileTransfers` checks native flush access masks, bufferless packets on every device transfer mode, full-width Information, publication and disposal before dispatch returns, worker/guarded notification and rejected async/concurrent calls. The existing original and genuine WDK file-transfer fixtures also execute imported and dynamically resolved flushes, including a user IOSB and the nine completion modes.

`KernelFileTransfers` checks native control packet fields, ULONG truncation, zero-length carriers, copied read-only input, IOSB aliases, separate input/output bounds, deferral and eligible-thread notification. The existing original and genuine WDK transfer fixtures call both control APIs through imports and dynamic lookup across four buffer shapes and nine completion modes.

### Explicit Darwin dylibs and TLS

Build `NeverDDarwinProcessTests`, `NeverDProcessPublicTests` and `neverd` in the
CPU-emulation profile. `DarwinDylib.*` checks graph identity/version/platform,
shared input accounting, resident TLS budgets, cycles, malformed layouts and
preparation timeout. `DarwinLinking.ExportTries*` and
`ThreadLocalExportMustNameAnAdmittedDescriptor` cover original export facts;
`DarwinLibSystemTest.TLV*` checks descriptor/frame refusal, live-copy lifetime
and full GP/vector preservation on both ISAs. `DarwinMemoryTest.MappedTLS*`
checks actual reservation charging and mapping/protection refusal.

`DarwinProcess.StartupDylibsShareDataAndKeepThreadLocalInstances` compiles the
original three-module C workload for all five profiles with classic and chained
fixups. It checks shared dependencies, duplicate export spellings, relocated data,
zero-only and initialized TLS, live template mutation and fresh repeated runs.
The mandatory native inventory now has 102 methods per matching profile, or
306 matching ARM64 identities; the separate common native catalogue remains 82.
Unavailable transports remain explicit skips. `DarwinNative.StartupDylibsAndTLSMatchOriginalNativeImages`
compares a system-linked executable and its original libraries with real macOS
execution. Independent O0/O1/O2 classic/chained controls complement that test.

`DarwinLibSystemPublic.StartupDylibsAndTLSKeepIdentityAcrossCAndCLI` checks the
same explicit catalogue and reports through C and CLI. With
`NEVERD_TEST_LIBNEVERD` and `NEVERD_TEST_DARWIN_FIXTURES` set, the real Python
`test_darwin_startup_dylibs_share_data_and_isolate_tls` repeats every profile.
Run these alongside DarwinImage, linking, memory, existing libSystem calls,
raw startup and shared process/API regressions before publishing. Guest deadlines
and event limits are unchanged; native Intel and physical iOS need other hosts.

### Darwin constructors and TLS termination

`DarwinCallbackFixtures.cmake` builds the original header-free C/C++ dependency
graph for all five profiles in classic/chained formats. `DarwinCallbacks.*`
checks original constructor forms, malformed storage/routines and executable
target bounds. `DarwinLibSystemTest.Callback*`, `NormalExit*` and
`StartupCallbacks*` check void registration, opaque values, nested LIFO,
capacity/nulls, current-stack preservation, live permissions and pending tables.
`DarwinProcess.DylibConstructorsAndTLSCallbacksKeepStartupAndExitOrder` repeats
all exit paths, fresh processes and shared instruction/event limits. It adds
the constructor/TLS method in the mandatory native inventory.
`DarwinNative.ConstructorsAndTLSCallbacksMatchOriginalNativeImages` compares
both system-linked formats with actual macOS execution; independent original
O0/O1/O2 controls disable stack protection, whose guard imports remain outside
this provider. `DarwinLibSystemPublic.ConstructorsAndTLSCallbacksKeepLifetimeAcrossCAndCLI`
and genuine Python `test_darwin_constructors_and_tls_callbacks_keep_process_lifetime`
check all five profiles, void JSON results and repeated session isolation.
Native Intel and physical iOS remain unqualified. No guest budget is increased.

### Darwin C/C++ global exit callbacks

`DarwinExitFixtures.cmake` builds a real two-dylib C++ graph with compiler-emitted
`__cxa_atexit` and local-static initialization using `-fno-threadsafe-statics`.
`DarwinLibSystemTest.Global*` checks exact arity/int results, errno, mixed LIFO,
quota retirement, full-width objects, original targets and active-frame permissions.
`DarwinLinking.CallbackStorage*` and `RegistrationOnly*` protect the old base/TLS
footprint and conditional global metadata charge; raw and constructor-only
minimum-budget cases remain separate. Guard imports from libSystem and selective-finalization imports
remain refused.

`DarwinProcess.DylibGlobalsAndLocalStaticsFinalizeAfterTLS` covers eight native
outcomes, global raw exit, explicit reentrant-exit refusal, fresh processes and
shared terminal event/instruction limits. It raises the mandatory inventory to
103 methods per profile and 309 matching ARM64 HVF identities.
`DarwinNative.GlobalAndStaticDestructorsMatchOriginalNativeImages` compares both
system-linked formats with original macOS execution. Independent O0/O1/O2
controls cover all eight outcomes. C/CLI
`GlobalAndStaticDestructorsKeepLifetimeAcrossCAndCLI` and genuine Python
`test_darwin_global_and_static_destructors_keep_process_lifetime` check all five
profiles and existing JSON call records. Native Intel and physical iOS remain
unqualified; all guest budgets remain unchanged.

### Imported Darwin data and TLS pointer offsets

`DarwinAddendFixtures.cmake` builds the original SDK-free C/assembler alias
workload for all five platform/ISA cells with classic/chained fixups. The +8
array reference, -4 tail reference and initialized volatile TLS pointer share
provider storage; the TLS pointer remains changed at exit to check fresh state.
`DarwinDylibAddends.*` checks original format facts, signed extremes, main-image
and runtime targets, declared-span versus page padding, and one-byte pointer
membership. `DarwinDylib.OrdinaryAddendsKeepProviderChoiceWithRepeatedExportSpellings`
checks ordinal ownership; `NonzeroTLVDescriptorAndLibSystemAddendsRemainExplicitlyRefused`
retains the exact-entry boundaries.

`StartupDylibAddendsPreserveDataAndTLSAliasing` is required on each matching
native profile. `DarwinNative.StartupDylibAddendsAndTLSTemplatesMatchOriginalImages`
compares both system-linked formats with original macOS execution; independent
O0/O1/O2 controls complement it. `DylibAddendsAndTLSPointersKeepIdentityAcrossCAndCLI`
and the genuine Python `test_darwin_dylib_addends_initialize_data_and_tls_pointers`
repeat all five profiles. Unavailable hardware remains a skip, with no native
Intel or physical iOS claim. Common native cases remain 82; required methods
are 101 per profile, or 303 matching ARM64 identities.

### Imported Darwin memory calls

`DarwinLibSystemTest.Memory*` checks C argument widths, original carriers, early
errno, transport failure before effects and resident gate reservations on both
ISAs. `DarwinProcess.LibSystemMemoryCallsPreserveMappingsAndCErrorOrder` runs
ordinary C through classic/chained bindings on all five profiles; matching
ARM64 HVF identities are mandatory. Public C/CLI and real Python tests repeat
the same workload. `DarwinNative.LibSystemMemoryCallsMatchNativeMappingsAndCErrorOrder`
compares the system-linked object with original native file bytes, anonymous
zeroing, protection transitions, private mapping lifetime and EOF tails.

Counted runtime regressions cover hash-output aliases, low-width carriers, odd lengths, reloaded descriptors, sequential overlap, allocation failure, NLS refusal, partial faults, lifetime and APC_LEVEL admission. Time tests cover subsecond epoch edges, the ULONG limit and input/output aliases. The original and genuine WDK CRT fixtures call all three exports statically and dynamically in entry, worker and unload; entry also exercises APC_LEVEL.

Scalar uppercase regressions exhaust ASCII and the NLS-independent wide range with high carrier bits, and check extended-value refusal, exact arity and APC_LEVEL. Original and genuine WDK CRT fixtures exercise all three static imports and dynamic lookups in entry, worker and unload. The WDK ctype.h declarations retain 16-bit wint_t; no host C runtime provides the expected values.

Debug regressions compare repeated-short rules across all three wrappers, scalar loads at guest boundaries, growing prefix attempts, embedded NUL publication, freed/paged inputs and IRQL. The original and genuine WDK CRT fixtures exercise static and dynamic prefix calls in entry, worker and unload, on both scheduling modes, contracts and load bases. Unavailable backends remain explicit skips.

Context-query tests cover retained and stale threads, requestor/worker/attached contexts, module boundaries and holes, exact output width, lifetime, rights and IRQL. Original and genuine WDK CRT fixtures exercise static/dynamic KernelMode queries; the file-query fixtures also check UserMode callers. Both WDK families include normal/CFG images. Two omitted query declarations use local ABIs with genuine kit types. Unavailable backends remain skips.

Legacy MDL mapping checks cover static and dynamic genuine WDK calls, an attached worker, original image aliases, mode narrowing, IRQL, process switching, stale descriptors, duplicate system mappings and injected alias-budget exhaustion. Kernel shortage retains the MDL and reports unsupported bugcheck processing; user shortage remains catchable. Original wrapper probes verify argument forwarding and return width, not the injected mapper.

`KernelFileQueries.BaseDevice*` checks base versus two attached filters, unchanged references, ignored extension pointers, create/cleanup/close admission, corrupt records and IRQL. Original and genuine WDK normal/CFG file-query fixtures execute static and dynamic base queries across the existing backend, relocation, scheduling and caller-lifetime matrix.


## Ioctl memory evidence

`AndroidDevices.IoctlTrace*` checks raw/named/variadic/dynamic routes against
unchanged guest instruction/event counts, pointer payload bytes and errno
outcomes. It also checks unreadability, address overflow and trace exhaustion.
`LinuxFileNamespace.IoctlTraceKeepsEntryPointerAndRefusedCallBytes` covers both
LP64 architectures, a pointer overwritten by the ioctl and an unsupported
retry with complete RAM equality. `ProcessReport.IoctlTrace*` checks strict
JSON admission and lossless evidence serialization. The Python device
integration exercises the same report through the public C SDK.

`NeverDHighCSupervisorCallTests` verifies that an unmodeled live ARM/AArch64
SVC result returns a recoverable emission error. Unused results retain the
side-effecting statement without acquiring a Linux syscall contract.

`RegistrationSEHScalarSpill.*` checks exact block-local runtime DWORD spills and rejects changed widths, overlapping stores, unknown effects, duplicate definitions and exhausted proof budgets. `RegistrationLocalUnwindNative.SavedStackReadsBindTheExactLoadAndCallback` separates ordinary source preflight from committed volatile reads. `check_windows_finally_rewrite.py --form goto` checks all four MSVC profiles through raw/optimized LLVM and both patch modes at two bases; the external observer verifies four execution paths, chain restoration, stack guards and a changed-finally negative control.

`KernelDevicePointer.*` checks retained references, overlapping outputs, failed and informational opens, caller/APC restoration across pending CREATE and CLEANUP, argument widths, sharing, output-fault store order, admission and unload. The original and genuine WDK filesystem fixture adds immediate, worker-completed CREATE, worker-completed CLEANUP and unload-held pointer modes, including dynamic lookup. `KernelFileSystemExecution.*OpensExistingFiles` runs both scheduling modes, contracts and image bases; unavailable transports remain explicit skips.

`KernelMDLUserMapping.ProcessProbe*` checks carrier widths, operation normalization, IRQL, live-process admission, thread identity during probing, nested restoration, retry after faults and attachment/restoration shortages. The genuine WDK user-mapping fixture executes static and dynamic calls, a real exception handler after detachment, same-process calls while attached, shared physical pages and complete lock release. Original wrapper/termination-funclet probes establish control flow across explicitly injected helper boundaries, not whole-kernel behavior.

`KernelObjects.FilterVersion*` checks exact arity, caller-independent results and the pageable IRQL boundary. Original and genuine WDK CRT fixtures call the query through static imports and dynamic lookup in entry, worker and unload, with entry at APC_LEVEL. Original two-instruction probes verify the full return carrier and lack of data-memory effects without injected helpers.

`KernelPhysicalRAM.AddressQueries*` and `KernelMDLUserMapping.PhysicalQuery*` cover aliases, address-space identity, VA reuse, exact bounds, full-width PFNs, IRQL, page locks and revoked mappings. The genuine WDK user-mapping fixture compares static/dynamic `MmGetPhysicalAddress` calls with MDL PFNs across read-only user views, DPC-level system views and rebinding while an earlier locked alias survives.

`KernelMDLUserMapping.PrimaryQuery*` covers exact allocation bytes, adjacent owners on one page, full-width addresses, IRQL, retirement, aliases, replaced mappings, read protection and unsupported primary projections. The genuine WDK fixture exercises static and dynamic `MmGetVirtualForPhysical`, including a DPC-level system view and rebinding a user VA while an earlier locked alias survives.

`DriverKernelModel.StackQuery*` / `KernelMDLUserMapping.StackQuery*` checks activation, retirement, nested context restoration, full-width bounds, IRQL, process attachment, output overlap and high-before-low fault ordering. The original/genuine WDK CRT fixture calls both stack queries statically and dynamically in entry, worker and unload, checks a local variable against the returned bounds, and queries limits again at DISPATCH_LEVEL.

`KernelMDLChain.CreateMdl*` and `KernelMDLUserMapping.CreateMdl*` cover full-width guards, untouched bytes, zero/wrapping metadata, pool and stack lifetime, locks, aliases, failed release, partial reuse, IRP refusal and IRQL. The genuine WDK user-mapping fixture exercises static/dynamic creation, owned pool, interior caller pool and caller stack records, physical identity and complete unload. Original probes distinguish wrapper writes, pool flag translation and injected allocator/free boundaries.

`KernelMDLChain.AdvanceMdl*` and `KernelMDLUserMapping.AdvanceMdl*` check repeated page crossings, PFN tails, exact-end refusal, narrow arguments, stable mapping extents, failed teardown, permissions, storage ownership, process context and unsupported dependencies. The genuine WDK user-mapping fixture uses static/dynamic advance, explicit unmap and automatic unlock. Original-code probes separately cover the entire advance routine, restoration helper and unmap wrapper with explicit mapping-kind/PTE-release injection; they do not execute the operating system page database.

### Darwin default C++ static guards

`DarwinGuardFixtures.cmake` preserves default thread-safe static code generation
in two dylibs and main. A separate exact libc++ tbd exports only the three guard
operations; native controls link real libc++. `DarwinLibSystemBindings.Guard*`
and the four guard methods in `DarwinLibSystemTest` cover complete state words,
independent high/low port values, errno, void returns, first-byte-only readonly
acquire and failure ordering. `DarwinLinking.Guard*` independently writes both
fixup forms and checks provider closure, compatibility, shared pages, individual
fixup/gate charges, exact minimum admission and one byte below it.

`DarwinProcess.DefaultStaticGuardsKeepDylibTLSAndExitLifetime` covers constructor,
TLS and global callbacks, repeated static mutation, abort/retry, fresh state,
recursive/foreign/absent-port failures and shared event/instruction cuts. It
raises the required native inventory to 104 methods/profile, 312 matching ARM64
HVF identities. `DarwinNative.DefaultStaticGuardsMatchOriginalNativeImages`,
C/CLI `DefaultStaticGuardsKeepLifetimeAcrossCAndCLI` and genuine Python
`test_darwin_default_static_guards_keep_process_lifetime` check the same original
programs. O0/O1/O2 classic/chained native controls include x64 through Rosetta.
ARM64 compilation specifies both `-march=armv8-a -mcpu=generic`; inspect every
final image for LDARB, absence of LDAPR and real guard imports. Preserve failed
modern-host and march-only LDAPRB controls. Their refusal happened before the
first guard call with no output; it does not prove unchanged memory. Native
Intel, physical iOS, RCpc and concurrent initialization remain separate coverage.

## Offline Android resolver and Linux stream contracts

Build `NeverDAndroidNativeTests` and `NeverDLinuxProcessTests` with CPU emulation
and Android fixture compilation enabled. Filter `*AndroidNetwork*` covers
resolver result ordering, LP64 layout, null/error ordering, ownership,
IPv4/IPv6 formatting, named/raw/dynamic calls, short transfers, alias lifetime,
refusal and unknown-input stops. Original C fixtures run at O0/O2 with ordinary,
APS2 and RELR relocations on available checked AArch64 transports; unavailable
backends are skips. `*LinuxNetwork*` checks x64/AArch64 shared descriptors,
failed-admission atomicity, full byte streams and strict JSON. Tests never use
host networking. Run the complete affected owners before review.

The network fixtures also exercise TCP failure followed by connected UDP,
IPv4/IPv6, named/raw/dynamic calls, FD reuse, packet truncation, peek, empty
datagrams and explicit error completions. The original C receive caller makes
its own protocol decision for valid and malformed synthetic replies. Kernel
tests cover event ordering, send prerequisites, retry after interrupted route
selection, absent readiness, failed output admission and strict wire limits.
The same native fixture checks static/dynamic fortified memory calls, overlap,
zero size, unknown extent, failure-before-pointer ordering and byte guards.

`NeverDAAPCS64CallContractTests` checks bounded breadth-first callee discovery
and preserves unknown operands across call/exception boundaries without
inventing forwarded entry parameters. `NeverDLLVMImportVeneerBoundaryTests`
checks exact unwind-owned code pointers on ELF/COFF/Mach-O for x64/AArch64,
adjacent symbol ends, and rejection of unproved/continuation/data addresses.


### Buffered streams and poll/epoll

Build `NeverDLinuxProcessTests` and `NeverDAndroidNativeTests` with CPU emulation
and semantic fixtures enabled. Run `*LinuxPoll*`, `*LinuxPIDFDProcess*`,
`*AndroidStreamWait*`, `*AndroidPoll*` and `*AndroidReceive*` first. The new tests
cover x64/AArch64 kernel ABIs, retained inputs and output faults, alias lifetime,
ONESHOT rearming, timeout copyback, and original libc callers over all six
O0/O2 ordinary/APS2/RELR fixtures and available checked ARM64 backends. Buffered
cases assert partial objects, newline/read-ahead, multiple suspensions, EOF,
per-read timeout, thread-local errno and incomplete imports at unknown input.
Run affected Linux and Android owner suites before review; unavailable backend
skips do not count as executed coverage. All fixtures use explicit offline data.


### JNI lookup and native registration boundaries

`AndroidJNI.NativeRegistrationsRetainPartialRebindingsAndDeclaringOwners` runs
original O0/O2 native callers under every relocation packing and available CPU
transport. An authored DEX declares inherited native methods and a class whose
initializer throws. Tests compare complete retained registration inventories
through replacements, partial failure, class-local unregistration and detach;
lookup/registration must not initialize, while method lookup must initialize.
Malformed inputs preserve the already published prefix. Explicit loader-failure
cases verify exception identities, clear/retry and rejection of declarations
that shadow known classes. ProcessReport tests cover the JSON request and full
64-bit targets. Run the JNI, Dalvik and process-public suites; runtime tests for
existing DEX state/language workloads protect initialization order. These tests
do not claim native Java method execution or custom loader support.

## Legacy aligned vector moves

`NeverDX86LegacyAlignedMoveTests` checks MOVDQA, MOVAPS and MOVAPD register preservation and 16-byte alignment before memory access, including address-width wrapping and explicit FS/GS bases. Misaligned accesses must preserve destination registers and memory. Missing segment bases remain rejected. This is lifting/emulator coverage; exact symbolic recovery must separately prove the retained alignment guard.

```sh
cmake --build build-release --target NeverDX86LegacyAlignedMoveTests
ctest --test-dir build-release -L '^NeverDX86LegacyAlignedMoveTests$' --output-on-failure
```

`InterpreterSpecialization.Alignment*` and `PEFixedImageTest.RawAlign*` check independent remainder proofs, invalid inputs, exact budgets, retained native guards and receipt tampering. Legacy move audits bind the complete operation span, including the alignment guard; they do not certify preservation of opaque machine-state banks.

```sh
cmake --build build-release --target NeverDInterpreterSpecializationTests NeverDPEFixedImageTests NeverDX86UndefinedEffectsTests
ctest --test-dir build-release -L "^(NeverDInterpreterSpecializationTests|NeverDPEFixedImageTests|NeverDX86UndefinedEffectsTests)$" --output-on-failure
```

`NeverDX86NativeStateAuditTests` checks strict native-byte and decoder-detail admission for canonical x64 REP MOVSQ and legacy LDMXCSR. Complete operation spans remain bound to their undefined-output receipts. Independent full-state and ordered-copy oracles cover overlap, zero counts, address forms, missing bytes and reserved bits. Execution still requires the existing count/direction, memory, effective MXCSR-mask and validity proofs; this audit does not certify partial-fault restart or opaque-state preservation.

```sh
cmake --build build-release --target NeverDX86NativeStateAuditTests
ctest --test-dir build-release -L "^NeverDX86NativeStateAuditTests$" --output-on-failure
```

`NeverDX86ScalarMoveAuditTests` checks strict legacy MOVSS/MOVSD admission against original bytes and complete decoder detail. Independent byte oracles cover register merge, memory-load zero extension, scalar stores, every alignment, 16/32/64-bit addressing and complete register/flag/MXCSR preservation. Missing memory and read-only stores must not commit architectural outputs. Negative cases retain closed string, VEX/EVEX, segmented and malformed forms, and receipts bind the complete emitted span without granting opaque-state preservation.

```sh
cmake --build build-release --target NeverDX86ScalarMoveAuditTests NeverDX86LegacyAlignedMoveTests NeverDX86UndefinedEffectsTests
ctest --test-dir build-release -L "^(NeverDX86ScalarMoveAuditTests|NeverDX86LegacyAlignedMoveTests|NeverDX86UndefinedEffectsTests)$" --output-on-failure
```

`NeverDSymConditionalStoreTests` covers conditional effects on current bytes across independent roots, 128-bit writes, both byte orders, partial overlaps, symbolic relative wrapping and absolute memory. It checks own-bank neighbors, current versus historical facts, post-write unknown epochs, unsupported inputs, and exact versus one-short budgets. Executor cases cover STORE with existing segment-base handling, strict CMPXCHG success/failure and delayed Old publication, refusal havoc, and other operations leaving the policy Invalid. Ordinary store and preservation defaults retain their existing behavior. These tests validate bounded memory semantics; they do not certify access permissions or complete program recovery.

## Runtime module resources and persistent buffers

The [runtime module reference](runtime-modules.md#verification) lists the focused
resource, mapping, pause/resume, execution, C SDK and CLI suites. Qualify the
CPU-on/Driver-off configuration as well as disabled execution. The Windows
adapter remains a separate integration and is rejected when unregistered.

`DalvikSnapshot.*` checks read-only declaration/initialization reporting, forged
source labels, unknown and explicit phases, identity materialization, mutable
Instrumentation, erroneous/terminal initializers and unchanged heap/work limits.
`AndroidJNI.JavaState*` compares complete native reports with observation enabled
and disabled across relocation forms and available backends, including unknown
lifecycle stops and native registration without class initialization.
`ProcessReport.JavaStateObservation*` checks strict opt-in JSON and unknown/null
state serialization. These checks do not establish execution of ART startup.
`DalvikSnapshot.ExplicitApplicationLoader*`,
`AndroidJNI.ApplicationLoaderIsIndependentOfApplicationCreation` and
`ProcessReport.AndroidLoaderMayBeEstablishedBeforeApplication` cover independent
loader setup, unchanged null Application, exact lookup outcomes, retained loader
identity after detach and contradictory/unknown lifecycle inputs.

## Java library loading and retained DEX/native calls

`DalvikResume.*` checks single-use owner-bound continuations, nested stack order,
register/field effects without replay, initialization reentry and failure,
original DEX catch handlers, resource exhaustion and synchronous refusal.
`AndroidJNI.JavaLibraryLoadResumesInitializersAndNestedNativeCalls` runs authored
DEX and original AArch64 ELF bodies through `System.loadLibrary`, `JNI_OnLoad`,
registration and nested DEX/native calls. Cases cover method/field lookup and
allocation during initialization, recursion and repetition, constructor and
object returns, implicit frame cleanup, exception/class-error propagation,
accepted/rejected versions, guest sleep and missing/wrong ELF bindings.
The same fixture covers O0/O2 and ordinary/APS2/RELR relocations on available
CPU transports. `ProcessReport.JavaLibraryBindingRequiresInitializedJNIGuestImage`
checks typed explicit input and invalid names/lifecycle combinations.

Run these with all Dalvik and Android JNI tests, existing callback/thread/
property/finalizer tests and ProcessReport after changing this shared boundary.
Unsupported CPU transports are platform skips, not successful qualification.


`AbsoluteMemoryFactsTests.inc` checks written absolute targets across nodes, conflicting joins, partial and unknown alias writes, and proved stack separation in both byte orders with chaining enabled or disabled. Independent byte execution checks retained effects; missing exclusions and short budgets must refuse. `SymStorePreservation.Absolute*` and `SymState.AbsoluteProjections*` check absolute/symbolic coexistence, upper addresses, pure bounded projections, post-write unknown epochs and transactional refusal. Run `NeverDInterpreterSpecializationTests`, `NeverDSymbolicIntegerIntrinsicTests` and `NeverDSymMemoryKnowledgeTests`, then the native independence and LowIR refinement suites.

Absolute-memory regressions cover bounded contiguous and sparse batches, complete write/cell containment, and concrete aliases at both frame and exclusion boundaries. Ordered and reversed range unions must preserve the same cells, discard gaps and leave state unchanged when preservation runs one work unit short.

## Finite ordinary RAM read regression tests

`LowIRReadTrace.*` checks shared inputs, both byte orders, changed addresses/widths/order/counts, every branch, partial overlap with prior frame writes, undefined producers, independent repeated reads, exact/short budgets and loop refusal. `OriginalBinaryUndefinedIndependence.OrdinaryReadTracePreservesNativeMemoryInputs` checks original native independence, native-to-LowIR refinement and a changed-pointer rejection. Run `NeverDLowIRRefinementTests`, `NeverDLowIRUndefinedIndependenceTests`, `NeverDOriginalBinaryUndefinedIndependenceTests`, `NeverDPEFixedImageTests` and `NeverDInterpreterSpecializationTests`. These proofs do not by themselves qualify compiled C or a concrete CPU backend.

## Native scalar query regression tests

`NativeQueryTrace.*` covers independent query answers, all four zero extensions, selector/count/order mutations, undefined selectors, ordinary-read interleaving, every branch, unaffected state, strict byte/detail audits, malformed bundles, budgets, digest binding and static/inductive refusal. `NativeScalarQueryDescriptor.*` tests the generic descriptor limits and the actual receipt binding.

`NeverDOriginalBinaryUndefinedIndependenceTests`, `NeverDInterpreterSpecializationTests`, `NeverDLowIRRefinementTests`, `NeverDLowIRUndefinedIndependenceTests`, `NeverDX86NativeStateAuditTests`, `NeverDX86LegacyGSRecoveryTests`, `NeverDPEFixedImageTests`.
