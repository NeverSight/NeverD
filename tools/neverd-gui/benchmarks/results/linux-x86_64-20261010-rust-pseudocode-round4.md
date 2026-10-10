# Rust pseudocode performance, fourth pass — 2026-10-10

The largest function's median first complete-source request changes from
**66.243 s to 26.993 s**
(59.3% less waiting). **It still does not overtake IDA.**

[Raw samples, hashes, phase traces and test results](linux-x86_64-20261010-rust-pseudocode-round4.json) accompany this
follow-up to the [third pass](linux-x86_64-20261010-rust-pseudocode-round3.md).
The frozen worker and engine control is `c663bb37cf717838d8c70e1ea0588c6f01bb6161`.
The tested candidate starts from `c663bb37cf717838d8c70e1ea0588c6f01bb6161`; its
implementation diff SHA-256 is `4b41634b98915ea16359e0b38243355dc1fc1569bb51b86bab888dda243ff2bd`.
Both timed sides include the later upstream x87-call proof, narrow switch-guard,
x86 exception-frame, GUI text-selection, web URL and unpack integration changes
through `c663bb37c`. These shared upstream changes are not attributed to this
performance patch. Earlier value-query diagnostic runs below used the frozen
`bf3c7a142` tree; their counts are explanatory evidence from that snapshot, not
claimed to be final-build counts.

## Evidence and changes

Two diagnostic runs separated graph preparation, value analysis and diagnostic
bookkeeping on the supplied Rust sample. In one invocation, **4,725 completed
batches containing 673,149 queries repeated exactly on the same owned graph**.
Their results and evidence consumption all matched. Their value analysis took
3.966 cumulative seconds, against 12.981 seconds of all value analysis and
1.981 seconds of graph preparation. These are overlapping, inclusive worker
measurements, not wall-time savings. Diagnostic serialization alone added
1.603 seconds and is absent from the production build.

The earlier hypothesis that repeated candidate-depth failure was the major
cost did not hold: its trial cache recorded 666 candidates and **zero hits**.
That implementation and its tests were removed before this release.

A later instrumented run at `22793fc9d` exposed another repeated cost in x87
call analysis. A complete-graph-only prototype had 38 cache hits, but 27
expensive projections still repeated and then refused a block with no known
successor, return or stop. Those builds took 42.511 cumulative seconds; 71
exception-edge refusals took another 2.617 cumulative seconds. Again these
timers overlap and are not claimed wall-time savings. Fixed graph-size limits
and no-return recursion were not the observed cause of these expensive misses.

The final changes are:

- Retain completed value-query batches inside the validated immutable graph.
  Full ordered query fields, context, both relocation inventories, effective
  proof limits and output shape must match. Hits deduct the complete original
  value-work charge. Smaller allowances run the original proof path. Incomplete
  proofs remain incomplete; proposal rounds and rollback are never skipped.
  Retention is bounded to 64 batches and a separate 8 MiB payload allowance.
  Pointer-named symbolic values/merges are excluded because their allocation
  addresses can affect the charged string length. The anonymous-merge exclusion
  is defensive: current production merge constructors reject that shape.
- Share immutable x87 machine graphs across independent CFG builders. Exact
  construction context, immutable image/index lifetime, and bounded 128-item /
  8 MiB retention prevent cross-context reuse. Also retain compact refusal
  markers for exceptional edges and missing terminators after complete,
  nontruncated instruction lifting. These markers still refuse the proof; they
  never grant a stack effect. Incomplete lifts, query budget/depth failures and
  caller-specific answers are not shared. Every successful graph hit pays the
  original proof charges. Graph construction runs outside cache locks.
- Expand the parallel callee scheduling window from 8 to 32 bodies, still with
  at most four independent CFG builders. Serial execution retains eight bodies.
  Full-frontier BFS admission and ordered summary publication are unchanged.
  This overlaps slow callees across former small-batch barriers at a bounded
  memory cost; it adds no concurrent mutation of a Session.
- Add `neverd_prepare_function` so the worker can prepare analysis without
  generating and discarding a complete C document. It shares the existing ARM
  mode, exception-handler and function-scope logic. Older engines retain the
  decompiler fallback. A failed switch clears the prior prepared-entry marker.

- Preserve the latest requested or selected graph instruction while an async
  layout refresh is pending. Background revision/generation refreshes previously
  fell back to the function entry, and a navigation on retained graph nodes could
  be overwritten by the replacement layout. Deterministic regressions reproduce
  those paths before the fix and cover programmatic, keyboard and mouse changes.
  A separately reproduced race let an older function-lookup callback overwrite
  a newer in-graph jump. One owned single-shot connection now cancels that
  obsolete callback and its hidden-listing jump when navigation changes.

## Matched measurements

Input: `rust_eh_probe-x86_64-unknown-linux-gnu-unwind-o2`, SHA-256
`ff7b02957d3c10e9e6d409c62312d0ddcc261a9f69e3807be089ec319a53e538`. Release Clang 23.1.2 / LLVM r4, GCC 15.2 worker,
Intel i9-13900H, Linux x86-64. CPU affinity **0, 2, 4, 6** selects four separate
performance cores; `NEVERD_THREADS=4`. Each entry starts a fresh worker and input
copy with warm OS caches. No task builds or tests run during timing. Other host
activity is observed rather than stopped; raw loads are retained.

Times include the first 256 source lines and all remaining pages, after open,
functions and listing. They exclude Qt painting and GUI response caches.
IDA 9.4 uses a separate fresh idalib process/database per entry and returns text
after auto-analysis. APIs and source output volumes differ, so these are latency
observations rather than equal-work microbenchmarks.

| Entry | Control | Candidate | Less waiting | IDA after analysis |
|---|---:|---:|---:|---:|
| `0x19b24` | 66.243 s | 26.993 s | 59.3% | 5.042 s |
| `0x34901` | 33.232 s | 21.949 s | 34.0% | 1.500 s |
| `0x309d0` | 11.152 s | 7.436 s | 33.3% | 0.737 s |
| `0x54174` | 6.21 ms | 5.75 ms | 7.4% | 2.02 ms |

Values are medians of three alternating old/new repetitions. The tiny `_init`
measurement does not establish a meaningful speed difference. IDA separately
prepays median **5.012 s** auto-analysis for the largest entry.

All 12 old/new complete source pairs and four one-thread candidate checks match
in line count, byte count and SHA-256. The largest contains 4,637 lines
and 346,651 bytes. Hash equality checks output stability, not native semantic
correctness by itself. The largest median LowIR phase falls from
**62.975 s to 24.596 s**.

| Entry | Control peak RSS | Candidate peak RSS |
|---|---:|---:|
| `0x19b24` | 223.1 MiB | 252.3 MiB |
| `0x34901` | 164.8 MiB | 182.4 MiB |
| `0x309d0` | 138.3 MiB | 136.0 MiB |
| `0x54174` | 82.1 MiB | 82.5 MiB |

Peak RSS is per worker. A GUI can keep two analysis workers and its project owner;
the cache allowance is retained payload, not a cap on process or graph memory.

A separate one-worker sequence checks that complete document reuse survives the
new preparation path (A = largest function, B = `_init`):

| Step | Complete source | First page |
|---|---:|---:|
| A_first | 30.036 s | 29.995 s |
| A_repeat | 40.82 ms | 2.40 ms |
| B_first | 9.28 ms | 9.26 ms |
| A_revisit | 44.66 ms | 2.97 ms |

## Verification

| Suite | Passed | Skipped | Failed |
|---|---:|---:|---:|
| `NeverDJumpTableTests` | 413 | 0 | 0 |
| `NeverDPipelineOutcomeTests` | 27 | 0 | 0 |
| `NeverDSessionCAPITests` | 102 | 0 | 1 |
| `NeverDHighControlFlowTests` | 382 | 0 | 0 |
| `NeverDSysVCallContractTests` | 23 | 0 | 0 |
| `NeverDWin64CallContractTests` | 9 | 0 | 0 |
| `NeverDI386CallContractTests` | 10 | 0 | 0 |
| `NeverDLanguageEHTests` | 253 | 1 | 0 |
| `NeverDSourceAnchorTests` | 4 | 0 | 0 |
| `NeverDSourceDialectTests` | 17 | 0 | 0 |

The only failure in the ten core suites is `SessionCAPITest.StubsOfVariadicImportsPassTheirArgumentsOn`: it observes
zero generated import stub forms instead of six. The unchanged frozen
`c663bb37c` control reproduces the same failure in
`control-c663-session-failure.log`; it is not introduced by these changes.
The optional language/EH scratch input remains skipped when absent.

The current `c663bb37c` Qt offscreen GUI/worker suite passes
**19/19**. Current real-worker engine, native x86/AArch64 mappings,
import-name, Unicode-path and listing-data scripts also pass. Earlier targeted
GUI diagnostics passed 30 repetitions of four Tab-navigation cases (120 cases)
and a focused Qt run reporting 13 passed cases (including test setup/cleanup).
The recorded pre-fix runs fail three refresh cases and one stale-lookup case.
Those earlier diagnostics are distinct from the current full CTest run. The current worker also passes the native
source/IR/CFG script with the frozen `c663bb37c` control engine, exercising the
missing-preparation-symbol fallback.
Python ABI tests pass 12/12 and the SDK declaration audit passes. Optional audit self-tests pass 9/10: the
existing packaging assertion rejects `copy_directory` anywhere in SDK CMake,
including the newly integrated web-license staging. An earlier unchanged `44966cccb`
archive separately reproduced that assertion; no packaging changes are part of this pass.

The isolated x86/x64 x87 and Med calling-convention suites pass
**192/192**. Their cache tests verify actual shared
hits, cold/warm proof budgets, context invalidation, concurrent readers,
retention bounds, and unchanged refusal behavior. A 32-wrapper pipeline fixture
checks serial/parallel x87 call and return state, including callers with no
local x87 instruction.

New regressions exercise every small-fixture budget boundary, ordered results
and feasible masks, all query fields, context changes, incomplete proofs,
eviction and oversized inputs. A two-frontier 40-callee fixture compares all
serial/parallel register summaries. SDK/worker checks cover preparation without
emission, whole-image/restricted scope, zero-address entries, ARM mode failure
and A → failed B → A → retried B state transitions.

Native GUI paint/scroll timing, Z3 and native-emulation profiles are outside this
run. Remaining first-request time is still dominated by LowIR proof work; this
pass does not claim parity with IDA or relax NeverD's evidence requirements.
