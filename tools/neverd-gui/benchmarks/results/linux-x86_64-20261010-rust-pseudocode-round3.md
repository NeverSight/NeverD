# Rust pseudocode performance, third pass — 2026-10-10

The largest measured Rust function falls from **31.671 s to
21.946 s** in matched fresh-session medians. Returning to
it after another function now reuses the completed source document. **First
decompilation still trails IDA.**

[Raw samples, hashes and phase traces](linux-x86_64-20261010-rust-pseudocode-round3.json) contain three alternating
old/new runs for each entry, a separate one-thread identity check and a
single-worker A → A → B → A sequence. The control is commit
`42c166cc06208085c4368db1eb1940a9c9ca8e70`; both the worker and engine are frozen
for each side. This pass follows the [second pass](linux-x86_64-20261009-rust-pseudocode-round2.md).

## Measurements

Input: `rust_eh_probe-x86_64-unknown-linux-gnu-unwind-o2`, SHA-256
`ff7b02957d3c10e9e6d409c62312d0ddcc261a9f69e3807be089ec319a53e538`. Release Clang 23.1.2 / LLVM r4, GCC 15.2 worker,
Intel i9-13900H, Linux x86-64. CPU affinity is **0, 2, 4, 6**, four separate
performance cores; `NEVERD_THREADS=4`. Each entry has a fresh worker and input
copy, with warm OS caches. No task builds or tests run during the paired timing
window. Other host activity is not stopped; raw load observations are retained.

Times include the first 256 source lines and all following worker pages, after
open, functions and listing. They exclude Qt painting and GUI cache hits.
IDA 9.4 is measured in a separate fresh idalib process/database for each entry,
after auto-analysis, returning complete text through its in-process API.
The APIs and output volumes differ: these are useful latency observations,
not equal-work microbenchmarks or a native GUI comparison.

| Entry | Previous worker + engine | New worker + engine | Less waiting | IDA after analysis |
|---|---:|---:|---:|---:|
| `0x19b24` | 31.671 s | 21.946 s | 30.7% | 3.180 s |
| `0x34901` | 15.801 s | 13.280 s | 16.0% | 1.564 s |
| `0x309d0` | 7.864 s | 6.662 s | 15.3% | 0.759 s |
| `0x54174` | 7.94 ms | 8.18 ms | -3.1% | 2.11 ms |

Values are medians of three repetitions. The tiny `_init` result cannot
establish a meaningful speed difference. IDA separately prepays a median
**4.588 s** of auto-analysis for the largest entry; do not
compare its F5 time to NeverD's complete open-and-view time without that cost.
Even with this preparation included, this pass does not overtake IDA there.

All **12 old/new source pairs** and the final **four one-thread checks** have
identical line counts, byte counts and SHA-256 hashes. The largest remains
4,637 lines / 346,651 bytes. Hashes establish output stability on these fixtures,
not semantic equivalence of the whole binary.

The largest function's median HighIR phase drops from
**3.936 s to 0.921 s** and LowIR from
**25.168 s to 19.114 s**. Phase timings
exclude worker paging and some source emission work; they are not the complete
request latency.
Median worker peak RSS is recorded below. This is per worker; the GUI can
retain two analysis workers plus its owner.

| Entry | Previous peak RSS | New peak RSS |
|---|---:|---:|
| `0x19b24` | 190.9 MiB | 192.5 MiB |
| `0x34901` | 144.2 MiB | 170.1 MiB |
| `0x309d0` | 127.7 MiB | 135.9 MiB |
| `0x54174` | 80.1 MiB | 81.8 MiB |

## Returning to a function

This separate sequence uses one fresh worker with background analysis disabled,
as in a GUI analysis replica. A is `0x19b24`, B is `_init`. Both runs read every
page at each step; neither uses the GUI response cache. These are one sequence
per build, not medians, and must not be pooled into the fresh-session table.

| Step | Previous complete source | New complete source | New first page |
|---|---:|---:|---:|
| A_first | 26.147 s | 23.497 s | 23467.83 ms |
| A_repeat | 0.253 s | 29.25 ms | 1.66 ms |
| B_first | 13.45 ms | 8.38 ms | 8.36 ms |
| A_revisit | 26.291 s | 32.44 ms | 2.12 ms |

The four results within each sequence and across builds preserve complete
source identity. A cached source hit does not claim that the mutable Session
contains that function; a subsequent graph or IR request prepares it normally.

## What changed

- HighIR entry-stack queries share one SSA affine graph and the existing
  checked affine solver instead of recursively expanding PHI predecessors.
  A diagnostic invocation previously made 78,669,547 recursive queries while
  collecting 173 stack slots. All incoming values must agree; unknown,
  unanchored, conflicting, malformed-width and overflowing inputs refuse.
- A CFG builder retains one successfully constructed proof graph with its own
  immutable payload. Reuse compares complete graph inputs, including effective
  targets, roots and storage ownership, and deducts the exact original graph
  budget. Value proofs, one-shot hooks, incomplete results and fixed-point
  rollback still follow their existing paths. Retention caps the **input
  payload** at 8 MiB and separately caps vertices/owners; this is not an 8 MiB
  cap on all derived graph/arena memory.
- Each worker retains up to eight completed documents within a conservative
  32 MiB size allowance. Revision, listing generation and representation remain
  part of identity. Edits/restore/project changes invalidate documents. Paging
  copies the requested text and rows rather than copying the full document
  before trimming it. Oversized documents are readable without retention.

Existing independent callee builders remain bounded to four threads and eight
bodies per batch, with deterministic admission and publication. This pass
reduces repeated work; it does not add concurrent mutation of a Session.

The depth certificate uses two linear conservative bounds, including a separate
non-PHI starting prefix. Tests cover 31/32/33-layer boundaries and wide shared
chains. General multi-PHI graphs can still be refused even when a more expensive
path proof might fit the depth bound. Some mutually dependent, fully anchored
PHIs become provable where the old recursive heuristic depended on query start;
an independent path-constraint oracle checks that case. Do not describe the
adapter as matching every old heuristic acceptance decision.

## Verification

| Suite | Passed | Skipped |
|---|---:|---|
| High control flow | 382 | None |
| Jump tables | 398 | None |
| C API | 99 | None |
| SysV ABI | 23 | None |
| Win64 ABI | 9 | None |
| i386 ABI | 10 | None |
| Source anchors | 4 | None |
| Source dialects | 17 | None |
| Language/EH metadata | 253 | DumpRealBinary |

The Qt offscreen GUI/worker CTest suite passes **18/18**. Five real-worker
scripts pass: engine smoke, native x86/AArch64 source/IR mapping, import names,
Unicode paths and listing data. New checks cover cache eviction, edits/undo,
replica restore, project replacement, EVM/SBF first analysis, and cold Low/Med
paging. Verification caught and fixed the first-IR-request name changing from
`sub_400078` to `start` between whole and partial pages.

The jump-table suite was run after the graph-cache/shared-math change; the final
HighIR-only depth refinement was followed by the full HighIR, C API, ABI and
source suites. `Scratch.DumpRealBinary` needs an optional user-selected file.
Z3 and native-emulation build profiles were not enabled. No native GUI/manual
scrolling frame-time claim is made.

## Remaining work

LowIR is still the main wait. Successful graph reuse does not eliminate repeated
value proofs or the conservative rollback stages. The prior diagnostic found
88 additional callee CFGs and 11,970 table-value query batches for the largest
entry. Further reuse needs exact proof-input identity, mode, resource charges
and failure semantics; treating repeated stage counts as a fixed point would
be unsafe. The next architectural target is demand-driven callee evidence and
reuse of completed value proofs, followed by eliminating redundant source
renders while preserving byte-identical mapping checks.
