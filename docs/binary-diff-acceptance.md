# Advanced binary DIFF acceptance

This tracks the end-state requirements of
[#1462](https://github.com/NeverSight/NeverD/issues/1462), including the required
advanced areas. A working increment is not completion of the epic. “Complete”
in a comparison or bounded query describes that operation and its declared
profile; it is not a statement that all code, behaviors or security properties
were understood.

## Capability and evidence map

| Requirement | Implemented evidence | Remaining acceptance work |
| --- | --- | --- |
| Reproducible same-ISA native comparison | Shared `lib/diff`, input/analysis identities, strict saved binding, x86/x64 and AArch64 readers, ambiguous candidate inventories, exact weighted large-block alignment, caller-selected candidate measurement and conservative call-position anchors without a size cutoff | Broader linked-application, real-world stripped and duplicate-heavy corpora; measured error rates |
| CLI and GUI workflow | C ABI and Qt-free worker; bounded pages, CFG/source inspection, edits and undo/redo, save/reopen, input/restore/query cancellation, NDDB project inputs | Continue scenario-based usability and large-input latency qualification; support advanced areas below through the same surfaces |
| A: transformations and regional matching | Native instruction-run and finite SSA region hypotheses; split/merge/clone/inline/outline/tail evidence; qualified loop progress profiles through v7 | Broad CFG/dataflow transformations and cross-module movement; independent compiler, LTO, PGO and obfuscation qualification; explicit qualified cross-ISA profiles |
| B: security-fix triage and review | Local change observations, native call-path context, bounded import-effect context, input-bound analyst cases/history and references | Ranked security-fix datasets, interprocedural parameter/return/alias effects, path feasibility, automated patch/backport/revert evidence and reviewer report qualification |
| C: optional semantic validation | Bounded pure scalar-return profile with explicit models, budgets, non-success outcomes and native provenance | Additional memory/call/loop profiles, precondition and coverage qualification; no general equivalence or vulnerability-elimination claim |
| D: version and branch lineage | Offline snapshot graph, direct/indirect/parallel/cyclic associations, grouped conflicts, exact-byte reuse, report-local assessments, CLI/Python/GUI persistence, release aliases with declared chronology/parents, focused integration coverage, and a reproducible synthetic scale harness | Broader integration failure follow-up, package-wide graphs, automated patch/backport/revert evidence and large real-corpus quality/latency; annotation application remains #99 |
| E: software/game packages | Native loaders, module resources and a separate qualified offline web-package behavior comparison exist | One package manifest workflow routing native/managed/metadata/script/config/resource changes, repacking distinctions, qualified Unity/Unreal/Godot adapters and unknown/encrypted coverage |
| F: competitive qualification | Owned native/compiler regressions, pinned local diagnostic runs, a [reusable fixture generator and mapping evaluator](binary-diff-benchmarks.md) for NeverD, BinDiff, Diaphora and Ghidra exports, and automated fresh-input pipelines for all four tools with completion and input-identity checks | Licensed real-world ground truth, qualified common-input and end-to-end tracks, held-out corpora, broader runtime/profile qualification, per-category quality/resource metrics, frozen numeric gates and statistical evidence |

Each advanced area remains required. An absent native finding is not evidence
that a patch is absent, and an analyst's patch-present assessment is not a
machine proof that a vulnerability has been eliminated. Ambiguous and unsupported
results must remain inspectable; adding profiles must not silently broaden the
meaning of an existing proof.

The external-runner increment was exercised on all three owned ELF diagnostic
cases with Ghidra 12.1.3, IDA 9.4/BinDiff 8 and Diaphora tag 3.5.0, including a
separate Diaphora assembly-only profile. All twelve case pipelines completed;
scored mismatches remain retained evidence. New adapter checks cover native
input/direction binding, partial exports, false-success exits, cancellation,
analysis timeouts, source archiving and replay after moving retained databases.
The first Diaphora adapter attempt rejected its native MD5 BLOB representation;
the corrected reader admits both that representation and hexadecimal text while
still rejecting wrong input identities. Its initial failed receipts remain
separate. These diagnostic runs do not close F's real-corpus, fairness,
performance or statistical acceptance requirements.

## Verification record boundaries

Manual correspondence edits retain valid two-sided ambiguity groups throughout
save, reopen and undo/redo. Core regressions cover unequal candidate counts,
cross-group edits and exact group restoration; native SDK and worker fixtures
verify remaining-function classification, inventory invalidation and file
reopen after the last candidate is consumed. These qualify editing and
persistence, independently of matching accuracy or broader corpus coverage.

Matcher version 4 preserves exact similarity ties at zero ambiguity margin and
checks repeated call-position proposals before accepting pairs. It retains
unresolved calls as positional uncertainty and uses only anchors shared by all
optimal alignments of known corresponding callees. The former 1,024-call cutoff
is removed; 1,200- and 70,000-call fixtures retain contextual correspondence.
Independent exhaustive and full-table oracles check mandatory anchors, including
repetition and unequal sequence sizes. Six core behavioral fixtures and two
native SDK checks fail against the preceding implementation and pass after the
repair; the original failure receipts remain retained. This does not qualify
arbitrary call reordering or establish semantic equivalence.

Matcher version 3 removes two reproduced quality ceilings. A long-block
insertion no longer triggers greedy instruction alignment; completed alignments
use the same weighted objective at every size. A separate fixture shows that
raising `max_candidates` now reaches a correct pair below four misleading sketch
candidates, with the remaining decoys still ambiguous. Core regressions compare
large repeated/unequal alignments against an independent edit-cost oracle and
cover sparse insertions, 100,000-instruction constant edits, stops and stale
saved measurements. A native x86 SDK fixture checks the original bytes through
full/paged inspection and save/reopen. These qualify the stated structural
behaviors, not transformation completeness, semantic equivalence or competitive
superiority. General weighted alignment remains quadratic in time in the worst
case; cancellation and deadlines remain available.

Final matcher-v4 matching/alignment/feature objects linked against frozen
`fd59ee8336` remaining components passed 339 C++ cases without skips, seven
CLI/worker groups and 120 Python checks. Three selected Qt comparison workflows
passed in a disposable native Wayland desktop, including candidate selection,
editing and cancelled-restore retry. The compiler group includes 527 fixture
comparisons; these are not additional CTest cases. All 23 owned native mapping
pairs remain correct.
Separately, 89 historical reports reopen byte-exactly offline and 38 older
extraction-profile reports retain their intentional live-binding refusal.
These receipts qualify the named objects and frozen profile; subsequent upstream
native pipeline changes need their own runtime coverage.

The SBF Rust warning failure described below has a focused repair: the monolithic
and structured generators borrow their private register bank as partitioned
handlers already do. The original failure was reproduced, and an isolated
relink against frozen `fd59ee8336` archives passed all 20 emitter checks and both
C/Rust source differential tests, with no skips. Four added terminal-slot cases
cover scalar writes, successful/faulting division and syscall effects. The Rust
differential compiled and executed 104 generated modules with `-D warnings`.
This closes that reproduced failure at the frozen boundary; it does not replace
the earlier full-suite receipt or qualify subsequent upstream SBF changes.

The v7 input-expression progress increment was locally qualified at
`7ae9cbf363e11944cb365cdfd49d50312014f067`: 1,427 selected CTests passed,
four optional/environment-dependent tests skipped, 527 compiler comparisons,
89 byte-exact historical report reopens, and Python/Qt integration checks.
Those are distinct types of checks and must not be summed into one benchmark.
The finite-domain oracle's proof calls are not native executions or performance
measurements.

The publication commit `ac381aaef710b436ecc10d27d9386b3c3fba8edb` also contains
concurrent upstream changes. Its exact-source local integration selected 2,521
CTests: initially 2,497 passed, one failed and 23 skipped. The failure was a
Windows HFA execution harness passing vector carriers to projected scalar
parameters. Fixture repair `35826752e1fec446c30e1d83b56301b58c8a24ed` uses the
actual projected argument types while retaining the independent byte oracle.
The repaired test passed, producing a composite 2,498 passed / 23 skipped
qualification; this was not a clean rerun of all 2,521 tests. Original failures
and repair receipts are retained separately.

Saved-decision consistency was subsequently qualified at
`dc6e83e8185931aa0d35935985480cdf7048aa7c`: all 300 selected CTests passed,
including the 527-comparison compiler fixture; 89 historical reports reopened
byte-exactly, and Python/Qt integration passed. These are local Release results
using the selected LLVM 23r6 prebuilt and Capstone
`88f3e3966565fb9383cd4fbb20c051c9aa22f309`, with Z3, emulation and embedded
Python plugins disabled. Native-backed Python bindings were tested separately.
Neither source-file equality nor cancelled CI runs qualify an entire merge.
Future receipts must name tested and delivered commits, dependency inputs,
build profile, selected tests and failures/skips separately.

See [the comparison profiles](binary-diff.md) for exact evidence meanings and
[the test guide](testing.md#structural-binary-comparison) for owning checks.
No comparative performance or “best in class” claim is established by the
current regressions.

The preceding fixed integration `183c8acd77b6cbece352b8c7b2dbb70fc97064b7`
selected 5,439 CTests: 5,376 passed and 63 skipped, with no failures. Its compiler,
historical-report, Python and Qt checks passed. The normal push delivered
`32a3acca81cd4850b0fbb50f5ae5f2369bbbfb5c` after additional concurrent upstream
merges; those later deltas need their own integration coverage.

The first complete lineage integration at
`f9b2ab69915218c25624262056141fcf43ac3665` selected 5,744 CTests: 5,638 passed,
four failed and 102 skipped. The failures exposed a source global-type lookup
that overlooked shared original-image storage, an appearance-test oracle that
misidentified antialiased text, and two growing x86 compilation/execution
matrices exceeding their 120-second per-test deadlines. The global-type repair
is covered by ELF/COFF/Mach-O unit cases and worker tests that compile both C
routes at O0/O2, change the pointer cell between calls, and preserve native IR.
The appearance oracle retains the actual layout tolerances. System-instruction
and descriptor matrices retain all original cases in separate tests.

The source-frozen rerun at `a90cd50056338b67938332b453c5b0b73a68ea1b`
selected 5,843 CTests: 5,734 passed, two failed and 107 skipped. All four earlier
failures were repaired; the remaining failures were two aggregate AMX matrices
exceeding the unchanged 120-second deadline. This run also passed the
527-comparison compiler fixture, 89 byte-exact historical report reopens,
native-backed Python checks, desktop comparison/appearance/lineage checks at
150% scaling, and lineage checks in private GNOME Wayland sessions at both
100% and 150%. Matrix splitting increases the CTest count without adding
semantic coverage; optional NeverC cases account for the additional skips.
Original failure and diagnostic receipts remain separate from subsequent
qualification. The AMX matrices were subsequently split by encoding, preserving
their original cases and the 120-second per-case deadline.

The preliminary delivery merge `4155ff7a180ff069918f48b431b0093047829f40`
also exposed a stale test caller of the shared stack-projection interface during
the full build. The fixture now supplies its matching LowIR source; the original
stack-operation assertions remain intact. Inspection of the newly integrated
byte-patch workflow then reproduced a data-only overlay reusing the unpatched
DIFF identity and collapsing two states into one lineage snapshot. SDK and
Python regressions first failed against that source. Repair
`5a9b57dffa5f2ce9455e4ead210671b4adc85e85` binds effective overlay bytes in the
shared native snapshot adapter, including bytes outside decoded functions.
Focused tests passed for data-only identity, refusal of an older saved binding,
sidecar replay, revert, and identical bytes with different edit kinds/row splits.
The source-frozen qualification of this repaired source selected 5,777 CTests:
5,707 passed, 70 skipped and none failed. A separate, disjoint selection of all
126 split AMX cases passed without skips. The compiler fixture's 527 comparisons
are included in the selected CTests, not additional CTest cases. All 89 historical
report reopens remained byte-exact, and the five Python groups passed 111 tests.
Comparison, appearance and lineage checks passed at 150% offscreen scaling;
native compositor gestures are explicitly skipped on that platform. Private
GNOME Wayland sessions at both 100% and 150% each passed all ten lineage cases,
including real resizing, restored geometry, replacement navigation and bounded
long-label rendering. Source hashes remained unchanged throughout qualification.

The same source passed three fresh-process scale samples at each of 1,000 and
10,000 functions per snapshot, with 16 snapshots and 19 source comparisons.
The larger workload retains 160,000 function nodes and 190,000 association edges.
The reusable harness verifies authored identities, complete graph counts,
first/middle/last-function paths and original edges, current cache budgets,
cancelled replacement and exact report export. Hashed inputs, native library,
bindings and harness, original failures, per-sample timings and process RSS are
retained in local qualification artifacts. These are synthetic scale checks;
they do not qualify real-corpus matching quality, desktop latency or competitive
superiority.

This qualification uses Release, LLVM 23r6, Capstone
`88f3e3966565fb9383cd4fbb20c051c9aa22f309` and signature export
`fe9a373c020fd48f68802353497bf8e315f7e178`. Z3, emulation and embedded Python
are disabled; native Python bindings are qualified separately. Skips include
optional NeverC, Windows/PDB and exception corpora, platform-specific native
execution and newer LLVM intrinsics. Subsequent upstream integration and the
delivered commit require separately identified coverage.

Integration `3dc5881e6dcc1f9607965adf83995db8ae0bc68a` includes upstream
`62c40a36f5c4dbe5dd7d8e1f50738566728ba58d`. Its complete build initially
refused a missing libc++ fixture archive; installing the exact upstream signature
gitlink `39c99bbade0bf21c00cc0d70df7c577d98e34a7c` supplied the retained
compiler artifacts. The subsequent frozen-source selection passed 6,429 CTests,
skipped 76 and failed none, out of 6,505 selected. The disjoint 126 AMX cases,
both three-sample scale workloads, 89 historical reopens, five Python groups,
150% Qt checks and both ten-case native Wayland runs passed. Seventy-three
signature-generation/metadata script tests also passed against unchanged script
contents. This uses the same Release/LLVM/Capstone profile and disabled optional
backends described above; it does not imply emulation qualification.

Desktop recovery was subsequently qualified at
`833507adcd4130db7da6b838687be27514048a79`. A saved collection with 200
snapshots and 48 KiB labels reproduces valid admission followed by oversized
default pages. User-adjustable page rows recover the complete inventory without
changing labels, collection identity or budgets. A separate 200-report diagram
reproduces Fit shrinking below the manual zoom range; Zoom in now recovers a
readable scale. Both regressions failed on the original desktop code and passed
after repair. All twelve lineage cases passed in each native Wayland scale;
offscreen runs passed eleven and explicitly skipped the compositor gesture.
CLI, worker and shared-library hashes remained identical to the qualified
integration. Later layout and upstream changes still need their own coverage.

The frozen integration `9db401cc32e2872d3e6ce0af96084a9f005a6a25`
includes upstream `e4bac38ad97853d86c75cd67d8a107c86c1f1357` and signature
gitlink `4ad4e052e78666a1ac20541535878133e3a3ca8d`. Its complete selected
build succeeded. Of 6,565 selected CTests, 6,486 passed, 79 skipped and none
failed. The three additional skips require an unconfigured Windows exception
corpus; they are not successful executions. All 126 disjoint AMX cases, both
three-sample synthetic scale workloads, 89 byte-exact historical reopens,
111 native-backed Python checks and 73 signature-script checks passed.
Comparison, appearance and lineage checks passed at 150% offscreen scaling,
with compositor gestures explicitly skipped there. Each private Wayland run
at 100% and 150% passed all twelve lineage cases without skips. Source hashes
remained unchanged throughout this qualification. The Release/LLVM/Capstone
profile and disabled optional backends remain as described above. Subsequent
localization, right-to-left navigation and upstream integration have separate
qualification records.

The localized integration `5aaa254c7524c21c98ef9a4f27060cda505cc4f9`
selected 6,764 CTests: 6,684 passed, 79 skipped and one failed. The failure
was the complete workbench group: its private runtime path exceeded the Unix
socket limit, reference fixtures tried to focus hidden code tabs, and hiding a
completed code document discarded its exact secondary instruction selection.
The latter was a product defect; cancellation now preserves that selection
until the document is replaced. Original failures remain in separate receipts.
The 126 disjoint AMX cases, both synthetic scale workloads, 89 historical
reopens and 111 Python checks passed. The localized lineage suite passed all
23 cases in each private Wayland scale; the offscreen gesture remained skipped.

Repair `9987eccaa4692d98826e730d4ac41a163176c070` passed all 16 selected
worker/desktop CTests, including the complete workbench group. Its 14-case
focused workbench run passed offscreen and in private Wayland sessions at
100% and 150%. The compositor explicitly supplies native focus; Qt's ordinary
activation request alone cannot provide that contract on Wayland. All 23
lineage cases passed at each native scale; offscreen passed 22 and skipped the
native gesture. Eleven locales also exercise invalid-file messages and retention
of the previous selection and collection. CLI and shared-core bytes remained
identical to the preceding integration. This is a composite repair qualification,
not a clean rerun of all 6,764 CTests. Later row-visibility checks have their own
qualification below.

Integration `a005079b26e26f2ac467a3ebc1af41054c7a6182` includes upstream
`0e24efef94a1e8c51188d3c7233066545733c070`. All 418 selected CTests passed
without skips, covering the new BEXTR owner, shared prefix/undefined-effect
boundaries, source pipelines and DIFF owners, including the compiler comparison
fixture. The 89 historical reopens and 111 Python checks passed again. Desktop
binaries remained identical to the qualified repair. The public-interface ledger
now accounts for the lineage C, Python, CLI and JSON surfaces; its 116 script
tests and source validation passed. The unchanged Release profile still disables
Z3, emulation and embedded Python. Merged Android changes therefore have no
runtime qualification in this profile.

Native layout qualification at `f902f29464b4bb18ca113ab794122b651919239c`
adds visible-row and non-overlap checks for narrow localized windows. Arabic
font metrics first reproduced a clipped function row, and a growing error
message partly hid the selected snapshot. Font-derived table minima, retained
selection visibility and the dialog's content minimum fix those cases. Screenshot
review also caught paging status overlapping a table despite valid viewport
coordinates; the stronger checks cover containment and separation. Multi-line
metadata is rendered on one table line while original tooltips and evidence
remain complete. All 23 lineage cases passed at each native scale. The three
selected desktop CTests passed; the 150% offscreen run passed 22 and skipped
the compositor gesture. CLI, worker and shared-core bytes remained identical
to integration `a005079b26e26f2ac467a3ebc1af41054c7a6182`.

Naming-provenance and declared-release integration at
`bc4931f92d9ed6fcd1847668fe5baa5d37ec3f28` selected 14,163 CTests:
13,971 passed, 21 failed and 171 skipped. All 326 selected core/CLI DIFF cases passed
without skips, including the shared reader and release contracts. The overall
integration remains failed; those broader failures are retained separately from
successful DIFF qualification. Fifteen failing CLI cases also fail with the
earlier frozen `4e02ca99499cf10c4f654c8fbbed10c6df97e0cc` CLI. That comparison
does not classify the remaining direct-component failures or establish that all
failure causes are identical.

The same frozen integration passed 116 native-backed Python checks, real legacy
worker/library compatibility, and both three-sample 10,000-release workloads.
At each native Wayland scale, lineage passed 27 cases, the selected workbench
workflows passed 19, and layout passed eight. All 89 historical reports remained
byte-exact through the offline workflow. Fresh live binding intentionally refused
38 reports from older native naming profiles: `sdk-native-decode-v2` excludes
generated address names and display-only duplicate numbering from name anchors.
This is a profile boundary, not 89 successful old-report live authentications.

This local Release profile uses the frozen LLVM
`03270359c2ed8f6806533d3832a947bf883a6f69` SDK, including its separately built
JIT/driver archives, Capstone `88f3e3966565fb9383cd4fbb20c051c9aa22f309`, and
retained dependency fixture overrides. Z3, emulation and embedded Python plugins
remain disabled. Native Windows/ARM64 and CI coverage are separate. Subsequent
GUI name presentation, header sizing, test-fixture repairs and upstream merges
require their own receipts; none converts this failed aggregate into a clean run.

Name-presentation integration at
`b6612fe61e1d4299d450c8540034141180a253ce` selected 15,367 CTests:
15,079 passed, 31 failed and 257 skipped. All 330 selected DIFF-owned tests
passed without skips: 326 core/CLI checks and four worker/GUI checks. An earlier
name-based count of 333 included three other-component entries mentioning
"Lineage"; the owning-target count supersedes that classification. Its separate
capability-manifest check failed on duplicate
byte-overlay claims and exclusions; the aggregate remains failed. The frozen
source and dependency hashes were checked after all independent checks finished.
The same 116 Python checks, real legacy worker/library compatibility, 89
byte-exact historical reopens and 38 intentional old-profile live-binding
refusals passed. At both native Wayland scales, lineage passed 27 cases, saved
comparison passed three, the selected workbench workflows passed 19, and layout
passed eight, all without skips. Later fixes require their own qualification.

The normal push delivered `5c21d2664f393788f752357ae79a9235fe70b231` to
`dev`. Frozen-source follow-up at `fd59ee8336e6b8e353f787b7413a71803524d67c`
selected 1,478 CTests: 1,464 passed, one failed and 13 skipped. All 330
DIFF-owned checks passed without skips. The remaining selected failure is
`SBFSourceDifferential.GeneratedRustMatchesTheRawOracle`, whose generated Rust
still triggers unused-assignment diagnostics. This is a composite follow-up,
not a clean rerun of all 15,367 earlier checks. The delivered commit also
contains later upstream source-dialect changes; those changes do not acquire
the frozen-source runtime qualification.

The follow-up passed 120 native-backed Python checks, real legacy worker/library
compatibility, 89 byte-exact historical reopens and 38 intentional older-profile
binding refusals. Both native Wayland scales again passed 27 lineage, three
saved-comparison, 19 selected workbench and eight layout cases. The optional
10,000-function GUI probe passed separately, including three fresh validation
requests for its selected two-instruction function; it does not prove equivalence
of all 10,000 functions or establish latency under concurrent test load.

The pair-memory formatting and malformed-contract fixture repairs passed 11
focused checks at `997dfd44b1a9a249833c391b422e6db608fe5a18`. The ordered-memory
fixture repair `ccca9397ba` passed all 14 cases in an isolated test executable
against the frozen `fd59ee8336` archives and CLI. These Linux x64 checks preserve
original instruction encodings and compile the AArch64 projections; they are
not native ARM64 execution qualification. None of these partial repair receipts
waives the unresolved failures outside the selected follow-up.
