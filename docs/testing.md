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

The native source dialect suite also uses `rustc` and `go` from `PATH`, or
`NEVERD_TEST_RUSTC` and `NEVERD_TEST_GO`. It executes 32 Rust/C observations and
12 Go/C observations covering integer promotion, narrowing, signed/unsigned
comparison, arithmetic shifts and loops. The Go check compiles its generated
scalar functions unchanged; it does not establish that every Go pseudocode
construct is directly compilable. Missing compilers remain explicit local
skips and are failures in the main CI outcome audit.

See [CONTRIBUTING.md](../CONTRIBUTING.md) for clone, build-profile, and macOS
prebuilt-LLVM guidance.

## GUI function lists and navigation

`SessionCAPITest.PrepareFunction*` checks analysis without source emission,
restricted and whole-image scope, repeated and switched entries, address zero,
unloaded sessions and unknown ARM mode. `NeverDWorkerSourceCache` verifies
preparation does not render discarded C and that a failed function switch
requires real preparation before the preceding function's CFG is read again.

With the optional Qt desktop targets enabled, `NeverDGuiController` exercises
the production views against the deterministic worker. Its chooser regressions
replace queued filters and scroll a 20,000-function fixture across more pages
than the dispatcher admits. Functions activation covers Pseudocode, explicit C,
LLVM C and IR windows, multiple windows, pinned views and back/forward history.
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
`NeverDWorkerAnalysisSnapshot` verifies read-only replica state,
unchanged owner files, staged comments, signature replay and stale-input
rejection. Database restore coverage keeps pseudocode closed until requested.

`NeverDWorkerCodeEdits` checks declared local targets, image-name replacements,
UTF-8 span remapping, comments and raw string isolation, and rejection of stale,
colliding or shadowed names. The controller edits source, C and LLVM C with a
pinned view and a different hidden assembly location, then checks undo/redo,
save/restart and instruction-mapped comments. Snapshot coverage also verifies
that read-only replicas load these edits without changing durable owner files.

`NeverDWorkerSourceCache` counts actual preparation and source-page calls across
A-to-B-to-A navigation, including a subsequent graph or IR request. It checks
edits, undo, replica restore, project replacement, representation separation and
both document-count and retained-byte eviction. Timing-independent cache tests
complement `source_revisit_bench.py`, which measures complete real source pages.

`NeverDSourceAnchorTests` checks byte-identical HighC/LLVMC emission and refuses
changed-kind, synthetic, ambiguous and mismatched-function observations.
`SourceDialect.Navigation*` checks distinct statement ranges in C++, Rust and
Go, partial slices, conflicting spans and functions shown as C after a dialect
refusal. `SessionCAPITest.DialectNavigationMapsOnlyTheReturnRowAcrossPages`
checks real x64/AArch64 source and explicit dialect pages, unmapped headers and
single-line paging without spreading return addresses across the function.
`SessionCAPITest.CSourcePagesRetainCanonicalReturnAnchors` and the real worker's
native mapping test verify x64 and AArch64 high-VA source rows through small
pages, including return instruction addresses distinct from the function entry.
The controller's `tabFromCodeUsesSelectedInstruction` verifies independent
click/arrow navigation and both Tab directions; its optional native rows use the
same PE/PDB and environment variables described below. Unmapped declarations
exercise the explicit entry fallback. `tabFromAssemblyWaitsForPagesAndUnfoldsTarget` checks
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

## Source dialects and DWARF ingestion

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
calls and handlers. `NeverDSessionCAPITests` checks detected defaults, explicit
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

## Scalar x86 floating-point state

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

`X87CallGraphCache` compares independent builders with and without shared
graphs, including exact remaining proof budgets, recursion-depth failures,
changed image and analysis contexts, concurrent queries and bounded eviction.
Complete instruction graphs rejected for exceptional edges or unresolved
terminal dispatch retain only an immutable rejection marker: tests require
actual warm hits, identical cold/warm budget consumption and a fresh successful
proof when the same address belongs to a different valid image. Truncated
instruction streams remain unretained.

The concurrent graph tests hold a builder at a real prover callback and observe
registered waiters before releasing it. They check one construction for matching
inputs, independent proof budgets, exception and incomplete-lift wakeups,
different contexts, and reentrant callbacks across threads and caches. These
overlaps are coordinated by conditions rather than assumed from timing.

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

## x86 invalid encodings and instruction boundaries

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

## Inline C memory accesses

`NeverDCMemoryCopyTests` executes HighC and LLVMC output at `-O0` and `-O2`
with undefined-behavior traps. It checks unaligned byte copies, adjacent-byte
preservation, aligned accesses with mixed effective types, pointer copies,
signed narrow loads, expression-store results, single evaluation, guarded and
short-circuit loads, loop reloads, and temporary-name collisions. It repeats the
checks with `UseUnalignedPointers`. `HighCIntegerWidths.*`,
`HighCPointerAddresses.*`, `HighCStoreForwarding.*`, `LLVMCValues.*`, and the
frame-memory, atomic and segmented-memory suites cover the surrounding paths.

`LLVMCValues.SelectedFunctionKeeps*SynthesizedTableStorage` compiles and executes
selected lookup-table output at O0/O2 for x86, x64, ARM and AArch64 source
profiles across ELF, COFF and Mach-O. Execution uses host scalar C for 64-bit
layouts and freestanding i386 C for 32-bit layouts on Linux x86 hosts; other
hosts explicitly skip the i386 execution test. Initializer-dependency coverage
retains forward-referenced storage and function providers while excluding
unrelated globals. `HighCPointerAddresses.ExceptTailUsesOnlyItsPrintedContinuation`
checks except fallthrough, live intervening effects and finally transfers;
the x86 SEH probe also checks shared result-slot identity after frame projection.
`LLVMCValues.WideConstantsPreserveEveryStoredByte` executes unaligned i256/i512
loads and stores at O0/O2 with undefined-behavior traps, checking every byte and
the surrounding sentinels. Widths beyond the supported C carrier fail explicitly.

```sh
cmake --build build-release --target NeverDCMemoryCopyTests \
  NeverDLLVMCValueTests NeverDLLVMCFrameMemoryTests \
  NeverDHighCStoreForwardingTests --parallel 4
ctest --test-dir build-release \
  -L '^NeverD(CMemoryCopy|LLVMCValue|LLVMCFrameMemory|HighCStoreForwarding)Tests$' \
  --output-on-failure
```

## Offline web analysis

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
`WebSourceRecovery` checks comments/literals/ASI, reparse equality, large-input
recovery without raising interactive budgets and invalid-source refusal.
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
input links/collisions, aggregate budgets and bounded JSON admission. Its blob
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
cases explicitly skip when LLVM lacks native Zstd support; SDK capability and
unavailable-path checks remain active. No external decoder is used.
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
`NeverDWebNativeTests` compares file and immutable-buffer loading of synthetic
ELF/PE/thin Mach-O, verifies direct native bytes and static decompilation after
web/input destruction, rejects truncated images and implicit universal slices,
and checks sidecar/patch refusal and transactional file reload. It also covers
directory occurrences, Bun native-asset provenance and four offline CLI commands.
The full Bun compiler-container native load case uses `NEVERD_BUN_142_CORPUS`
and explicitly skips when that corpus is absent. It does not execute the target
or claim full-runtime native decompilation. Native handoff cases explicitly skip
on Windows while web input capture is unavailable there. Worker checks retain
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

`ResolverGraphCache.*` in `NeverDJumpTableTests` compares hot and cold answers
and remaining budgets at every small-fixture budget boundary. Equal-size
instruction, edge, root and ownership changes must replace the graph; rolled
back and temporary override payloads cannot leave borrowed pointers behind.
Resource failure and value-analysis incompleteness remain distinct.
`ResolverValueQueryCache.*` checks actual complete-batch reuse, exact remaining
budgets at every small-fixture boundary, ordered mixed results and feasible
masks, all query fields, proof limits and graph-independent relocation context.
Incomplete proofs and oversized records cannot be retained; count and byte
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

`FrameOffsets.*`, `NativeStackSpecialization.*` and `OriginalBinaryUndefinedIndependence.*` check all residues for alignments 2/4/8/16/32, free high bits, spills across calls, countdown loops, alias corruption, wrong dispatch, irrelevant wide masks, necessary partition upgrades and exact/one-short budgets. Separate native controls check guarded alignment, internal unsigned return cleanup, incorrect cleanup and prefixed returns. These tests do not establish automatic native-to-LLVM proof coverage for partitioned loops.

Native frame-cache regressions cover 64 repeated aligned loads under the one-load query budget, one-short query refusal, changed out-of-frame addresses and the same address under distinct incoming predicates after one path has returned. Existing full-state mutation and cache key/capacity tests remain required.

Frame-offset regressions cover 558 split-width/alignment/residue/bias combinations under a gate budget too small for whole-root subtraction, mismatched slice roots and biases, unconstrained sparse masks, modular carries and wraparound, nested masks and node/query exhaustion. Native tests verify exact stores through partially aligned pointers, reject missing alignment and out-of-frame accesses, and reject modified store values in full-state refinement.

Repeated-feasibility regressions retain all 130 native instructions while using the same query budget as a two-instruction straight line, reject one-short query/instruction budgets, keep feasible traps across changed branches and entry domains, refuse exhausted solver gates and reject modified candidate state.

Entry-congruence proof tests cover every residue at alignments 1/2/4/16, two distinct root registers, unconstrained higher bits, malformed domains, contradictory entry constants, nonwrapping bounds, exclusion gaps, preservation and exact/one-short query budgets. Inductive loop templates retain the original root predicate. Fresh native independence/refinement and native-to-LLVM checks reject mismatched domains and changed status, while certificate digests bind both domains. These are independently authored fixtures; no alignment is inferred from an ABI or a concrete execution. An independent guarded C function is compiled unchanged at O1/O2 and proved against the actual native instruction sequence under two residues; the same compiled artifacts must fail for a different residue.

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

`AndroidSleepTests.cpp` compiles independent O0/O2 callers with ordinary, APS2 and
RELR relocations. It checks raw, named and variadic relative sleeps, complete
observed bytes, request/remaining aliases, malformed inputs, multiple deadlines,
saved registers/TLS/errno, join/mutex/once continuations, provider lifetime and
pending events at instruction limits. Available native AArch64 transports also
match Unicorn's complete reports, including instruction and thread traces.
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

Cleanup-relay tests cover both EBP displacement widths, exact thiscall object
reads, PE32 relative-branch wrapping, nonwrapping storage, writable/overlapping
code, fixups, call substitutions, callee stack pops and FS-dependent effects.
The independently loaded MSVC fixture also checks its two actual relays and
their destructor footprints.
The state target also checks source-call identity, initialized ECX object
borrows, registration/SavedESP separation, partial stores, pointer taint,
conflicting predecessors and preserved catch resumption after a private throw.
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
`LLVM_NEVERD_X86_REGISTRATION_GS`. The pinned r4 packages and their matching
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
  --gtest_filter='-WindowsRegistrationNative.InputPE32PreservesItsCheckedSourceContract:WindowsRegistrationCxxSource.*'
for registration_case in filter nested-finally continue-search continue-execution normal-finally cdecl-parameter cdecl-parameter-write eh4-filter; do
  python scripts/check_windows_registration_rewrite.py \
    --test-binary build/bin/NeverDWindowsRegistrationNativeTests \
    --patch-binary build/bin/neverd --case "$registration_case" \
    --output "build/evidence/registration-${registration_case}"
done
```

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
another part of the generated section. Schema2 requires all six routes at both
bases: original, manual, public COFF patcher, called-helper/import-name collision,
CLI section and CLI inplace. All five generated routes must reproduce the entire
checked manual transaction byte for byte, including preserved helpers, before
runtime observation can count. Native replay validates every hash and the same
complete route matrix. Schema1 replay remains explicitly manual-only for older
evidence and cannot count as public/CLI runtime verification.

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
two such runs to judge a HighC or HighIR change.
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
  `NeverDImageAnalysisBoundaryTests` is also mandatory on all three hosts:
  removing, excluding, skipping or failing a discovered native-image proof
  case fails the CI evidence audit.
- Linux must also execute the SBF external oracle, upstream conformance, and
  Agave conformance suites because that leg installs their pinned dependencies.
- Every host must execute `NeverDSourceDialectTests` and
  `NeverDSourceAnchorTests`. Missing compilers, missing generated corpus inputs,
  skipped checks, or omitted suites fail the audit. The CTest evidence artifact
  includes the compiler versions, native ELF inputs, emitted C and CLI logs.
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
zero and high-bit return addresses,
and rejected non-HINT authentication, stripping, branches and system-register
access. Every attempt checks the complete scalar/vector inventory, PC and
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

With `native_cpu_only=true`, `native_driver_tests=true` enables `NeverDNativeDriverTests` without Unicorn. Before configuring, `build_wdk_driver_fixtures.py` verifies the complete SHA-256 of the official Microsoft WDK/SDK 10.0.26100.6584 packages and rebuilds 48 original normal/CFG/DBG driver images. `WDKDriverFixtures.def` owns package identities, compiler/linker arguments and fixture bindings. Unmodified Microsoft inputs and their licenses remain in the local build/cache directories; CI uploads only build metadata and logs. The manifest records tool versions, commands, source/header hashes and output image hashes.

`NativeDriverTests.def` requires 230 WHP outcomes from all 115 workloads in `DriverBuiltinImages.def` and `DriverBackendParityCases.def`: 27 built-in images, 48 WDK images and 40 request scenarios, each at original and rebased addresses. The complete mandatory inventory is `5068 CPU + 230 WHP + 25 SEH + 77 scheduling + 30 wait sets + 11 driver UNPACK + 6 clock reads + 4 image memory checks = 5451`. The 30 wait-set checks comprise sixteen portable model cases and fourteen original native driver cases. `run_native_cpu_ci.py --with-drivers` retains exact inventory/JUnit evidence with Unicorn disabled. Missing or skipped required fixtures fail the opt-in gate; ordinary builds keep external fixtures optional. Fixed images retain their expected rebase rejection. ARM64 native guest execution remains unverified.

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

`WhpStateTransferTests.cpp` checks both XSAVE API generations with injected register transfers: exact changed groups, complete capture, ignored padding, partial failures, cancellation, exception priority and partition replacement. `ContinuedStepsReuseCapturedRegistersAndFP` counts avoided installs; `PartialTransferFailuresPreserveStateAndForceFullRetry` requires complete restoration. These are protocol checks, not native execution evidence; existing native FP, state-transition, driver and ring3 suites remain required.

`CancelledDirectRunPublishesACompleteBoundary` verifies complete state at an acknowledged direct-run cancellation. `FailedDirectCapturePreservesStateAndForcesFullRetry` requires unchanged caller state when register, XSAVE or metadata capture fails during cancellation, followed by a complete retry; neither API generation may publish a partial register prefix.

`WhpStateTransferCases.def` also covers every partial prefix of the combined 32-register capture and conflicts in all seven metadata fields. Both XSAVE API generations must preserve caller state and force a complete retry. The same suite checks one register read per step and recovers omitted XSAVE metadata from that read.

### Android native workloads

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

`NeverDSysVCallContractTests` covers x86-64 System V call contracts on the shapes of QtXml's `QDomNode::save` and `QDomNode::isDocument`. A direct callee whose summary reads an argument register receives the caller's value of it, including an incoming `this` passed through untouched; a virtual call takes the object a dominating block loaded into `RDI`; a method that returns without writing `RAX` on one path while only passing callee results on the others is void; and a byte written to `AL` before a comparison chain is the value returned on every path. The emitted programs run at `-O0` and `-O2` under AddressSanitizer and undefined-behavior traps.

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

`LowIRLoopInference.TransferredCounters*` covers a counter that moves between storage locations at different cuts while another loop can bypass each cut. Exact symbolic unit transfers propose source-counter guards and a fallback rank with one alternate cut location; every guard and rank still needs full transition proof. The 192 complete-state cases span registers, frames, function temporaries, both byte orders, 1/3/4/8-byte counters, independent tags and stationary controls. Changed results/tags, invalid rank maps, nonterminating updates, missing guards and short inference/final-proof budgets refuse. Counter traversal uses the existing symbolic-node allowance; optional graph-selector work can remain zero. Cases include decrementing to zero and incrementing to an input bound. When a new counter is discovered, bound and lane-guard pruning waits until every cut template has been rebuilt; ordinary widening and independent proof still run.

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
