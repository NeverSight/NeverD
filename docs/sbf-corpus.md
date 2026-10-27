# Pinned Solana SBF corpus qualification

`scripts/prepare_sbf_corpus.py` authenticates the 72 program artifacts in
`unittests/sbf/corpus/solana-dex-2026-10-24.json`. The manifest pins the collector
commit, snapshot release, asset identities, stored and padded SHA-256 values,
loader ownership, and observed ELF layout. The binaries remain in a local cache.

The collector's deployment slot is not an execution slot. The manifest explicitly
uses the selected NeverD revision's mainnet-beta execution profile with an unknown
historical execution slot. It does not reconstruct a historical chain feature
snapshot, account state or transaction input.

## Build and run

Configure an independent Release build as described in CONTRIBUTING.md, then:

```sh
python3 scripts/build.py build-sbf-qualification --target neverd-sbf-corpus \
  NeverDSBFAnalyzerTests NeverDSBFSolanaModelTests --parallel 4

# Cache-only verification is the default. Add --fetch to acquire missing assets.
python3 scripts/prepare_sbf_corpus.py --cache /path/to/sbf-assets

python3 scripts/run_sbf_corpus.py \
  --worker build-sbf-qualification/bin/neverd-sbf-corpus \
  --cache /path/to/sbf-assets \
  --output build-sbf-qualification/corpus-results \
  --jobs 2 --timeout 60
```

The output directory must be new, so a failed request cannot inherit an older
success. `--kinds analysis` runs the analysis census without emitter requests.
`--retain-sources` preserves generated output; otherwise its size and SHA-256
are recorded before deletion. `--compile` additionally generates native objects
from C11 and Rust with warnings as errors, under the same request budgets. Rust
appends `sbf_corpus_native_probe.rs` to a separate copy: its concrete v2 host
forces monomorphization, while unresolved Rust-ABI callbacks preserve opaque
host effects. This compile-only adapter is never executed. Reports distinguish
native objects from older syntax-only/metadata checks and retain source/probe
and object hashes. Emission or compilation alone does not establish behavioral
verification.

`summary.native_compilation` counts C and Rust object results independently
from source emission. A successfully exported file with a compiler timeout is
an emission pass and a native-compilation failure. Cancellation retains the
full inventory with explicit cancelled/not-run entries.

`--kinds callx-contexts --retain-sources` explicitly runs the optional
caller-context query and retains `output.callx-contexts.json` for every sample.
It uses the same loader, execution profile, process isolation and request
budgets. This query is not added to the default export kinds. Target evidence
is conditional on the recorded summary/caller and recognized entry model;
every site retains dynamic fallback. Schema 6 covers complete function CFGs,
nested parameters, return values and typed scratch effects. It retains prior
entry-prefix evidence under a separate scope and records generalized inputs,
summary dependencies and resource/recursion barriers. It also records projected
inputs, complete bounded value alternatives, parameter-relative write intervals,
return expressions, and reclaimed solver-only summaries. Complete sets of two
to four literal raw targets now apply every alternative's call summary and join
all returning outcomes. `proven_target_set` rows and their site/context counters
stay separate from singleton proofs; `conditional_target_sets` preserve the
complete address alternatives. Summary edges record `callx_target_address`,
including distinct unaligned addresses mapping to one slot. Unknown, symbolic,
overflowed and unsupported alternatives retain conservative effects and explicit
barriers. A finite list of remaining CALLX values is not counted as a unique
target proof, and reclassifying addresses present in an older report is not
new address recovery. Unknown targets include the first missing incoming
register or load fact when available. A successful query with
zero targets is an analysis pass, not a devirtualization pass. Neither these
conditional facts nor the global unresolved CALLX count can stand in for the
other.

Generic full-width stored-value summaries keep complete two-to-four alternatives
across CFG joins and nested wrappers. Their keys must still be present on every
returning path; actual argument substitution must retain every alternative and
stay within the same four-value bound. Aliases, partial overwrites, unknown
values and overflow retain conservative invalidation. These are memory-transfer
facts, separate from global CALLX closure or executed-handler coverage.

Finite symbolic parameter origins also survive a join before a common store
or return. Summaries publish complete `return_input_alternatives` separately
from concrete `return_value_alternatives`; private formal origins are never
serialized as VM addresses. Unknown and fifth alternatives still absorb the
fact, and memory effects retain conservative alias handling. A generic CALLX
can bind an `entry_input_alternatives` expression from those origins, including
common full-width loads through selected pointers while the caller's memory
facts survive all preceding writes;
its proof includes every leaf and the final complete union, with the same
four-value, work, retained-memory and output-element limits.

Completed specialized summaries may reuse their proven conditional input
dependencies without taking another variant slot. Reports retain the original
static input masks and cache inputs alongside the separate conditional reuse
mask and memory-dependency flag. Edges identify equivalent specialized-input
reuse, and `equivalent_specialized_summary_reuses` counts it. Unknown call
boundaries remain conservative; different read scratch cannot borrow another
caller's facts. Definition-mask joins participate in the solver even when all
register values and origins are unchanged. No numeric qualification budget
changes with this reuse policy.

An actual caller can also instantiate a generic summary's target expression
from its own parameters and entry-memory snapshot. Such rows retain their
generic-summary and budget reason and carry the separate
`instantiated_generic_summary_inputs` scope and input/load witnesses. The
generic return/memory result and four-specialization limit remain in force.
Equivalent unary transfers share an expression across instruction positions
and physical carriers when their version-qualified opcode, normalized operand
and input and memory guard agree. Reported slots identify representative
transfers, not an
executed branch. Differing offsets, operations and unpreserved entry-memory
loads do not acquire equality from this rule.
Generic helper returns expose `return_value_expression` and can compose into
their generic caller's target template. Deferred loads retain representable
preceding writes in `input_memory_guards`. Instantiation rebases those footprints
to the actual caller and uses canonical memory-effect invalidation before
reading its full-width facts, including across nested helper calls. Unknown
write addresses, overlapping fields and uncertain absolute/frame relationships
still invalidate facts; heap or account types imply no disjointness. Captured
values can survive a later write when their carrier survives the ABI. Complete
canonical loaded origins are reused before constructing a guarded expression.
Full finite target proofs from this path do not by themselves establish
complete candidate-callee summaries or execution coverage. Expression nodes
and proof steps consume the existing output, retained-state and work budgets.

Source dispatcher regions use the shared `SBFSourcePartition` layout: every raw
slot occurs once, including unreachable and continuation slots. HighIR function
ownership determines grouping; shared tails and unowned ranges remain explicit.
`MaximumDispatcherRegionSlots` bounds each region's compiler CFG work. The
monolithic layout remains an opt-out used by source execution differential
checks. This is a compiler scalability boundary, not a completed function source
view or a business-logic recovery claim.

### Lifinity-v1 concrete field trace

An offline diagnostic now reaches slots 1960/1963 in the pinned, unpatched
`lifinity-v1` binary (SHA-256
`897a5b450f22dfa1fed6398e25fb5bfbaabc7d6bbc2ad2411cb840e7601af601`).
Its aligned input has 11 distinct account keys, a 621-byte Amm record, a Config
record, initialized token/mint records, matching packed Amm key fields and an
authority PDA. The quantity arguments, reserve token amounts and mint supply
are all one; decoded Amm field+0x250 is `10^18`. The earlier 605-byte Amm data
failed deserialization. Packed
field positions were checked against the actual decoder's byte movements.

| Curve byte | Raw steps | Slot 1960 field | Loaded vtable | Slot 1963 target |
| --- | ---: | --- | --- | --- |
| 0 | 24,061 | `0x200006DC8` | `0x100087C68` | `0x100003EB8` (slot 1971) |
| 1 | 24,063 | `0x200006DC8` | `0x100087CF8` | `0x100003EB8` (slot 1971) |

Slot 22063 stores the vtable at `FP-568`; slots 22815..22817 pass the descriptor
at `FP-576` to helper 1959. Slot 1960 loads its field+8, slot 1961 loads vtable
word zero, and slot 1963 dispatches r2. The separate r1 value one is the object
carrier, not the target address. In both runs, slots 22176..22179 follow the
parent descriptor+96 through the boxed Amm/account record; slot 22188 updates
the distinct heap address `0x300007DD0` from one to two. Slot 22973 is not
executed by these two inputs. These finite observations do not establish
nonaliasing on other paths.

A complete checked-store trace confirms that 22063 is the last overlapping
writer before the field load at 1960 in both diagnostic runs. Earlier copies
at 67100/67115 use the same stack address before the descriptor's lifetime;
the address does not establish a permanent source-language type. The observer
leaves the guest results and instruction traces byte-identical. Temporary
host/observer sources are restored before rebuilding the qualification worker.

The static query still needs all-path field and alias proofs. Its global
`specialization_budget_exhausted` flag can remain false while individual edges
reach `per_entry_specialization_limit`. These are separate precision limits;
report the edge reasons alongside missing field/register facts. A finite
nonoverlapping trace cannot authorize a heap/stack NoAlias assumption or
remove the dynamic fallback.

The complete store trace also links the separate pointer chain used by 22188:
the parent descriptor+96 names the boxed Amm, its first word names an account
record, and record+8 names a separately allocated header. Slot 49845 stores
that header pointer in the record, and 49770 initializes its first word to one
before 22188 increments it to two. The copies into the boxed record and parent
descriptor are included in the byte-write audit. This identifies the observed
source without treating a source-language type as an alias proof.

The separate `withdraw_all_token_types` path needs a 12-account envelope:
the LP source, reserve tokens, mint and destination tokens have different
positions, and a fee token account is required. Its key must match the packed
Amm field at input bytes 318..349; merely adding an initialized token record
returns `IncorrectFeeAccount` before the later CALLX. With those roles and
nonzero quantities bound, both curve inputs run through
22973 → 23890 → 1960 → 1963 → 1971. Slot 22955 writes the vtable at
`0x200006DB8`, while 22973 increments the distinct header at `0x300007DA0`.
These observations extend the finite diagnostic paths, not the static query's
all-path alias proof.

The diagnostic host supplies a zeroed 32-KiB heap, checked nonoverlapping memcpy
and PDA derivation checked against solders 0.29.0. Logging returns zero, and
three CPI calls explicitly return zero without token or account-state
transitions. Both runs return zero under those synthetic effects. They are not
mainnet transaction replay, business verification or static CALLX closure.
Temporary host/test overlays are separate from the qualification worker.
The retained input JSON hashes to
`3d83dc29c262ad116ad64b41d5319e31ab84e3316ffcfc6e5abe04b257aee54f`;
the trace JSON hashes to
`273f6ed43cb4c79be99b19927eca0b8a15fd231f876f1c70338b15a1075522a5`.

Rust's partition handlers share one non-generic table and borrow the existing
v2 host as a trait object. The public generic v1/v2 entry points remain intact;
host callbacks and their error/feature behavior are unchanged. This avoids
replicating the region graph for each concrete host type.

Private Rust registers use statically named fields. Partitioned memory accesses
share callback/result handling and check errors before the next instruction;
the final-slot check preserves memory-fault precedence over execution overrun.
C shares callback validation/status conversion and retains every case label
when using sequential fallthrough. These changes reduce compiler work without
removing arbitrary raw CALLX entries or changing the qualification budgets.

Every artifact and output kind runs in its own child process. Each worker
flushes stage-start and stage-completion JSON lines. The parent preserves those
lines and stderr after a fatal signal, marks the active stage as crashed or
budget-exhausted, and atomically publishes the artifact result. Later stages
remain `not_run`. Ctrl+C cancels owned children and records the unattempted work.

The owning host policies are in `scripts/sbf_corpus_common.py`. The parent
records the request timeout, sampled resident-memory ceiling, concurrency and
output-byte ceiling. Resident memory is sampled with `ps` on POSIX for the
request's owned process group, including compiler children. Shared pages are
charged to each process; short-lived peaks can be missed. Reports identify the
observation scope so older parent-only measurements are not silently compared
with group measurements. These are host qualification budgets, not Solana protocol
limits or compute units. LLVM text is budget-checked after generation; process
memory and time limits also bound its construction.

The same RSS sample retains live child names and raw `ps` CPU times. This is a
last observation, not total request CPU: exited children are absent. It helps
diagnose timeouts on a busy host without changing a timeout into a pass or
claiming exclusive-machine latency.
On macOS `ps -g` selects the owned group before collecting usage, avoiding a
full-machine process inspection for each sample. Other POSIX hosts retain the
original all-process query and explicit group filter. The report records the
observer implementation; RSS scope, thresholds and request budgets stay fixed.

## Stage meaning

`SBFAnalysisStages.def` owns the engine stage names. Both the C++ worker and
Python runner consume it. `AnalyzeOptions::Observer` receives immutable engine
state, while `StopAfter` makes the requested inspection boundary explicit.
Ordinary callers retain the complete analysis path by default.

| Stage | Evidence |
| --- | --- |
| load | Production ELF loader admission and declared executable range |
| runtime_image | Canonical relocated ProgramImage and resolved runtime profile |
| decode | Requisite instruction validation and lossless LowIR, including continuations |
| control_flow | Initial call resolution and function-entry evidence |
| verification | Selected verifier policy; the worker uses the product's requisite policy and records that local preflight is not requested |
| cfg_medir | CFG, MedIR, register propagation and CALLX refinement fixed point |
| highir | Functions, ownership, regions and whole-program structured-plan disposition |
| solana | Optional discriminator, account, CPI, PDA and advisory recovery |
| llvm | LLVM emission, module verification and bounded text export |
| c / rust | Source emission; compilation is a separately recorded optional action |

CFG and MedIR are one convergence boundary because a newly proven CALLX target
can change function boundaries and invalidate earlier register facts. The report
does not claim an earlier CFG is final. Coverage counts distinguish raw slots,
logical instructions, continuations, reachable slots, ambiguous ownership and
unresolved calls. Unreachable code stays in the input denominator.

`report.json` retains the inventory denominator and per-request results, including
exact commands, output sizes, diagnostics, timeout and signal disposition.
`identity.json` records the HEAD, hashes of changed sources, worker binary,
manifest, dependency receipt and host. Comparison runs must retain identical
asset hashes, profiles and budgets. Preserve the configured immutable dependency
exports during a comparison; changes to dependency inputs require a new baseline.

## Recovery safety and header queries

Opaque binary words can equal `UINT64_MAX` or `UINT64_MAX-1`. Scalar DenseMap
keys reserve these values, so the Solana dictionaries, read-only scan indexes
and public-key prefix buckets use `SBFWordKey.h` to represent the complete word
domain. The additional key lane separates real words from container sentinels.
Neither valid input words nor supplied IDL discriminators are skipped.

SBF `neverd_headers_json` returns the available image inventory without forcing
optional recovery. `func_count_status` is `image_inventory` until recovered
functions have been synchronized, then `recovered`. An explicit analyze/functions
request still runs analysis and reports its failures. Native C-main discovery
does not decode SBF instructions.

## Checks

```sh
python3 -m unittest scripts.tests.test_sbf_corpus
build-sbf-qualification/bin/NeverDSBFAnalyzerTests
build-sbf-qualification/bin/NeverDSBFSolanaModelTests
```

The runner tests cover corrupt and truncated inputs, unsafe names, stale URLs,
no implicit network fetch, signal evidence and deadlines. Engine tests cover
early stops, failure boundaries, raw-slot accounting, reserved binary words,
supplied IDLs, and public-key prefix collisions. The integration suite checks
header availability before an explicit instruction-analysis failure.

Stage completion is not a claim of readable business-source recovery, execution
equivalence or security. Whole-program dispatcher output and unresolved account
or call facts remain visible; official VM and behavioral qualification retain
their separate gates described in `docs/sbf.md`.

## 2026-10-26 P3 qualification

Qualified code: `48b34933ca43b6d7760bb8b30da08de48ec68f20`; worker SHA-256 `5a40173becae26bbbf8931b4d43a97a894c5fa3f063a7c0c525d7817d18abc6a`. The pinned snapshot contains 72 SOs. This local macOS arm64 Release run uses the official LLVM r6 prebuilt package, pinned SBPF/Agave gates, and the original profiles and budgets: 60 seconds/request, sampled process-group RSS 8 GiB, output 512 MiB, jobs 2. Context limits remain 8,388,608 instruction visits, 64 MiB retained-state estimate, and four specialized summaries per entry.

The historical comparison below uses the frozen qualification shipped in #1802. NeverD has since moved to dev, including its Capstone pin; the five managed fork inputs and corpus profiles match. All 216 LLVM/C/Rust exports remain byte-identical. The separate V2/V3 cache-policy comparison uses the same dev and dependency baseline.

| Stage | #1802 frozen run | This run |
| --- | ---: | ---: |
| `load` | 72/72 | 72/72 |
| `runtime_image` | 72/72 | 72/72 |
| `decode` | 72/72 | 72/72 |
| `control_flow` | 72/72 | 72/72 |
| `verification` | 72/72 | 72/72 |
| `cfg_medir` | 72/72 | 72/72 |
| `highir` | 72/72 | 72/72 |
| `solana` | 72/72 | 72/72 |
| llvm export | 72/72 | 72/72 |
| c export | 72/72 | 72/72 |
| rust export | 72/72 | 72/72 |
| LLVM module verification | 72/72 | 72/72 |
| c native object compilation | 72/72 | 72/72 |
| rust native object compilation | 71/72 | 68/72 |

Conditional target sites: **173 → 142**; sites beyond the entry block: **166 → 135**. There are 13 newly recorded sites. All 44 lost sites have raw-SO witnesses excluding every old positive caller context; there are no unclassified site losses. These counts measure conditional facts, not executed handlers, globally closed indirect calls, or monotonically growing coverage.

For example, boop-fun constructors store zero at FP−72 and FP−56 before passing FP−72 to slot 149805. Its first two loads read those zeros; OR produces zero and JEQ at 149809 selects 149862. The old positive contexts for slots 149928, 149946, 149955 and 149966 therefore cannot execute. The audit follows all old positive callers, checks real ELF `.text` bytes and ABI-preserved zero definitions, and refuses unknown/relocated values or legacy-runtime mutation. Other runtime inputs remain open.

The cache-policy control restores Phoenix slots 143914/143953: V2 reaches the unchanged 8,388,608-visit bound; pressure-triggered reclamation takes 6,816,216 visits and retains both sites. Whole-corpus visits are 196,169,086 → 193,054,066; resource-widened samples are 5 → 4. Relative to #1802, resource-widened samples fall from 12 to 4.

Remaining resource-widened samples: `byreal, meteora-dbc, meteora-dlmm, pancakeswap`. Lifinity-v1 CALLX 1963 remains unproven: slot 1960 lacks an all-path value for the loaded field after possible aliasing writes; slot 1971 remains a constructor-derived candidate. Unknown pointer writes are not assumed disjoint from the stack.


Remaining full-scan native failures:

| SO | Stage | Disposition | Elapsed | Sampled peak RSS |
| --- | --- | --- | ---: | ---: |
| `jupiter-perps` | `rust_native` | timeout | 60.066 s | 3.40 GiB |
| `pump-fun` | `rust_native` | timeout | 60.047 s | 2.51 GiB |
| `raydium-clmm` | `rust_native` | timeout | 60.104 s | 1.49 GiB |
| `raydium-launchlab` | `rust_native` | timeout | 60.061 s | 1.52 GiB |

All use `rustc 1.91.1 (ed61e7d7e 2025-11-07)`. Emitted sources, compiler versions and probe templates match the earlier run. No isolated retry replaces a full-scan failure.

Validation: Release **411/411**, no failures, skips or disabled cases. Both C and Rust cover all **5,899,345 raw slots exactly once**, with matching partitions. Sanitizers, other host platforms and real business-handler replay were not requalified in this run. P3 and the broader semantic-recovery goal remain open.
