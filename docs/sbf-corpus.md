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
every site retains dynamic fallback. Schema 10 covers complete function CFGs,
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
Full-width output fields can also carry a captured loaded expression across
generic wrappers and complete copies. The actual caller binds its parameters
and pre-call scratch before applying the resulting typed store. Summary
`output_field_value_expressions` and
`instantiated_stored_value_input_expressions` count this propagation separately
from target-site coverage; they do not establish tag/payload correlation or
path feasibility at a join.
Schema 10 also preserves complete unions of loaded expressions at register,
return and full-width output-field joins, capped at the existing four
alternatives. Each member retains its own preceding-write guard. A bound
loaded return is evaluated against the actual pre-call parameters and memory
before applying callee writes; a later overwrite cannot change an already
captured value. Edges expose `instantiated_return_input_expression`, and
`instantiated_return_input_expressions` counts these successful computations,
including solver contexts subsequently reclaimed. Neither counter is target
coverage. An unknown member, a possibly aliased preceding write or a fifth
alternative still loses the complete proof.
Complete scratch spills and reloads can retain a captured loaded expression
across blocks. They use canonical full-width and alias invalidation; partial
overwrites and unknown writes still lose it. Resolved syscalls clobber the r0
expression carrier and preserve r1-r10;
guest and legacy mixed-dispatch calls retain their wider ABI clobbers.
Validated automatic-frame translation bounds can exclude an absolute write
from a complete relative cell, in either direction, through the shared
typed-cell owner. Reports expose the bound in `frame_coordinates`; manual
frames remain unbounded. A translated interval that can overlap still loses
its facts, including frame-relative arithmetic reaching heap addresses.
Summary-local frame inputs can be normalized at a known singleton entry r10,
with literal bits and captured identities unchanged. Returned frame-valued r0
and typed cells are rebound to the actual caller. Relative typed stores and
copies share their range validation, including representable coordinates below
the numeric stack region; wrapped windows and frame-based image reads remain
unsupported. `frame_coordinates` declares the summary and instantiated-proof
namespaces, and `canonicalized_frame_inputs` counts normalization work.
Opaque cached recursion cuts can be reconsidered when their non-self callee is
no longer active, with the same transfer/target retention checks and four-variant
limit. `unblocked_recursion_retries` counts successful work selections, not
target recoveries. Unknown members of a repeated literal-image binding cluster
do not prevent exploring a new literal binding; they still supply no target
or alias proof.
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

The static field loss is distinct from enumerating the two vtables. Both
candidates remain present immediately before 22188/22973; the unknown header
write then removes the field. In a specialized deposit context, 22177 names
the parent descriptor field but has no full-width pointer value for it. The
constructor now retains a nullable bounded allocator result at 32470 and its
captured identity along the successful 624-byte copy. At 32493 that path joins
an error-helper path whose reloaded payload and tag remain unknown; r8 becomes
unknown before 32926 stores it into output+96. The account-header pointer stored
at 49845 needs the same complete parent/object chain. Allocator 30171 reads an
unbound heap cursor; its captured return carries numeric bounds and equality,
not a guessed literal address or allocation freshness. The extra copy and
field facts do not establish an all-path field at 32926. Automatic-frame
translation bounds alone cannot authorize preserving the vtable across an
unresolved header write.

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

## 2026-10-28 P3 frame and recursion qualification

Qualified code: `0fb3e597f288101f74d3166b4e4f5ec9627206a6`; worker SHA-256
`010c2739bce749cf08994deb8fa948fbb00f0bab6a59ed8b1d659074e9b78dd2`.
The baseline is the complete second #1931 round at
`d024084e01c2d045220f38c214c1d2cd9e212e8e`; its production worker is
byte-identical to publication commit `bb21384252cd0ffb6c2de2f16983721207796758`.
The new report SHA-256 is
`ce3ab40e2385c7660d7de45ab131c9629d3133e2a2ea5021b4324af58236d15d`.

One independent complete run of all 72 pinned SOs passes every analysis stage
(`load`, `runtime_image`, `decode`, `control_flow`, `verification`, `cfg_medir`,
`highir`, `solana`), LLVM/C/Rust export, CALLX-context queries, and C/Rust native
object compilation: **72/72 before and after**. No crashes, timeouts or native
compile failures occurred in this round. The earlier #1931 first round's five
Rust timeouts remain historical failures; no isolated retry substitutes for a
full-run result, and timing stability is not established.

Profiles, asset hashes, compiler versions/flags, dependency inputs and budgets
match: 60 seconds/request, sampled process-group RSS 8 GiB, output 512 MiB,
jobs 2; internally 8,388,608 instruction visits, 64 MiB retained-state estimate,
524,288 output elements, four specializations per entry, and four complete
value alternatives. Release SBF checks pass **467/467**, with no skips or
disabled cases, including required official SBPF, Agave and protocol-scale
gates. Four new tests cover typed atomic frame translation, equivalent frames,
previously blocked recursive calls, and unknown variants beside new image
bindings; their 31 synthetic raw CALLX observations are not corpus bank replay.

| Conditional-analysis measure | Before | After |
| --- | ---: | ---: |
| Singleton target sites | 184 | 194 |
| Complete finite target-set sites | 31 | 31 |
| Sites with either conditional address proof | 215 | 225 |
| Sites with no conditional address proof | 7,359 | 7,349 |
| Instruction visits | 183,686,634 | 169,908,145 |
| Function summaries | 141,400 | 120,986 |

Singleton site/address relations gain 13 and caller-slot relations gain 17,
with no losses. Caller-entry relations gain 28 but lose 381 singleton and four
finite-set relations in Raydium CPMM/LaunchLab. An independent raw-ELF audit
checks all **1,170 old positive contexts** for these 385 removed relations:
literal vtable construction, relocated immutable function pointers, all raw
return paths setting r0 to zero, and conservative caller CFG propagation
exclude every old call position. Without the certified zero return, all 1,170
call positions become reachable again. In-memory mutations to nonzero returns
and overwritten vtables are rejected; original SO hashes remain unchanged.
For example, CPMM's calls at 18864/18927 into wrapper 24242 dispatch to slot
5116, whose returns are zero; the JNE branch into the panic tail at 18961 cannot
be taken. Calls otherwise may return, clobber caller-saved registers and expose
unknown memory; no nonreturning host-callback assumption is introduced.

Serialized expression witnesses are not all preserved. After normalization by
the owning caller's known R10, 2,656 forms become 2,279: 383 removed, six added.
Of the removed forms, 381 correspond to the audited pruning above. The other
two Aquifer forms, from caller entries 30832/31055 through call 31699 to CALLX
17792, now use direct function summaries. Three old positive contexts retain
the same normalized known caller registers and complete target set
`{17898, 17998, 18086, 18141}`. Queries do not serialize complete input memory,
so this comparison does not establish all-memory equivalence. Raw witness
counts 9,002 → 2,279 also include coordinate deduplication.

All 216 LLVM/C/Rust exports remain byte-identical to the baseline. C and Rust
each represent all 5,899,345 raw slots exactly once, with matching partitions.
Whole-program structured recovery is still **0/72**; all 7,574 raw CALLX sites
retain dynamic fallback. There are 71 samples with remaining unproven sites;
`meteora-dbc` and `meteora-dlmm` still reach a global analysis budget bound.

Lifinity-v1 CALLX 1963 remains unproven. Its unknown summary contexts decrease
from 24 to 16 (ten missing full-width scratch values, six unknown entry
registers); this deduplication is not a new target proof. Error-helper tag and
payload propagation, potentially aliasing writes, and the join at 32493 still
prevent carrying allocation/field facts through 32926 to 1960/1963. Finite
instrumented inputs observe slot **1971**, address `0x100003EB8`; this does not
prove all inputs. Sanitizers, other host platforms, real bank/CPI replay and
byte-identical source reconstruction were not requalified in this round.

## 2026-10-28 P3 loaded branch and return qualification

At four specialized caller contexts, a getter selecting two loaded fields
previously lost its complete expression at the join. A generic fallback also
composed a return expression into generic callers without evaluating it for
a concrete caller. Complete loaded-expression unions now survive register,
return and full-width stored-field joins. The caller evaluates a live generic
return using its pre-call memory before applying callee writes. Every member's
write guard is retained; unknown members and fifth alternatives remain unknown.

The qualified code is `3abf679e6f5854bd77674321c03e0f701ee37ba1`, compared
with the complete `0fb3e597f288101f74d3166b4e4f5ec9627206a6` frame-context
round above. Its `fc8ce09b5` publication changed only documentation. The new
frozen worker SHA-256 is
`0ea2c4d9ad8c381deb38a41b8a63b5f5ffd2d771554942411c394a4167b0829f`;
the complete report SHA-256 is
`92f4d17c114aeecce96a6c6dae70e228aba1d0d891928493316c2934d28b94bc`.

Both complete rounds use the same 72 asset hashes, loader/cluster profiles,
compiler versions/flags, dependency revisions, jobs and budgets: 60 seconds per
request, 8 GiB sampled process-group RSS, 512 MiB output, two concurrent assets;
8,388,608 context visits, 64 MiB retained estimate, 524,288 output elements,
four specializations per entry and four complete alternatives. The new round
contains every request and both native compilations, with no isolated retry
substitution.

| Stage | Before | After |
|---|---:|---:|
| Load | 72/72 | 72/72 |
| Runtime image | 72/72 | 72/72 |
| Decode | 72/72 | 72/72 |
| Control flow | 72/72 | 72/72 |
| Requisite verification | 72/72 | 72/72 |
| CFG / MedIR | 72/72 | 72/72 |
| HighIR processing | 72/72 | 72/72 |
| Solana metadata | 72/72 | 72/72 |
| LLVM export | 72/72 | 72/72 |
| C export | 72/72 | 72/72 |
| Rust export | 72/72 | 72/72 |
| CALLX-context query | 72/72 | 72/72 |
| C native object | 72/72 | 72/72 |
| Rust native object | 72/72 | 72/72 |
| Required SBF tests | 467/467 | 468/468 |

There were no crashes, timeouts, compiler failures or skipped required tests.
The final test run required the official SBPF oracle, Agave conformance and
protocol-ceiling scale. The new regression exercises nine modes and 138 raw
CALLX observations, including two wrapper layers, generic fallback, aliasing
writes before/after the captured load, unknown sources and five alternatives.
This is synthetic execution, not corpus transaction replay.

| Conditional recovery metric | Before | After |
|---|---:|---:|
| Singleton sites | 194 | 194 |
| Finite-set sites | 31 | 31 |
| Either conditional address proof | 225 | 225 |
| Sites with no conditional address proof | 7,349 | 7,349 |
| Output field expression records | 1,733 | 1,849 |
| Generic return expression instantiations | 0 | 2 |
| Serialized expression witness forms | 2,279 | 2,290 |
| Context instruction visits | 169,908,145 | 170,102,731 |
| Function summaries | 120,986 | 120,986 |

No address, caller-slot or caller-entry target relation changed. All old
serialized expression witnesses remain. Both actual return instantiations occur
in Obsidian. The additional field records and 11 witness forms do not constitute
new target-site coverage. All 216 LLVM/C/Rust exports are byte-identical to the
baseline; C and Rust each cover 5,899,345 raw slots exactly once, with all 72
partitions agreeing. Whole-program structuring remains 0/72; every one of the
7,574 raw CALLX sites retains dynamic fallback. The two global-budget-limited
samples remain meteora-dbc and meteora-dlmm.

Lifinity-v1 slot 1963 remains unproven in 16 contexts: ten lack a full-width
scratch fact and six lack an entry register. The successful allocation/copy at
32470 still loses its complete origin at the 32493 join before output+96 at
32926. Helper 182's saved tag/payload encounter unknown effects through
60926 -> 59757 -> 3034 -> 3157. Allocator helper 3070's success/error output
fields still lack their cross-field relation, and unknown writes cannot be
assumed disjoint. Earlier finite offline runs reaching slot 1960/1963 observed
target slot 1971; they do not close these static contexts. Real bank/CPI replay,
arbitrary-input semantic equivalence and byte-identical source reconstruction
remain outside the validated result.

## 2026-10-28 P3 matched guest return qualification

A mandatory guest recursion cycle previously acquired an invented returning
continuation when context recursion was cut. Schema 11 computes matched-return
reachability from canonical MedIR calls and CFG edges. A call requires both its
callee and continuation to return; a branch admits either successor. A cycle
cannot bootstrap a return from an EXIT after its mandatory recursive call.
Host callbacks and unknown indirect calls remain potentially returning,
including terminal-catalog syscalls. Callee prefixes retain their CALLX
observations even when the callee cannot return.

The isolated change is `22fde6821211fa04243a734d7d39bb41e5ce15eb`, compared
with the complete `3abf679e6f5854bd77674321c03e0f701ee37ba1` round above.
The new worker SHA-256 is
`ff67c026d280432a5301f2221cc8f03aa44f4ee76d7d522961060f82ffb59b65`;
report SHA-256 is
`4daaf2fbb685161864e10990a075b98cc751c948a72c2b2886ea91ceda7c1cad`.
All asset hashes, loader/cluster profiles, compiler versions/flags, dependency
revisions and budgets match that baseline. The complete run uses 60 seconds
per request, 8 GiB sampled process-group RSS, 512 MiB output and two concurrent
assets. Internal limits remain 8,388,608 visits, 64 MiB retained estimate,
524,288 output elements, four specializations and four complete alternatives.
No isolated sample retry is substituted.

| Stage | Before | After |
|---|---:|---:|
| Load | 72/72 | 72/72 |
| Runtime image | 72/72 | 72/72 |
| Decode | 72/72 | 72/72 |
| Control flow | 72/72 | 72/72 |
| Requisite verification | 72/72 | 72/72 |
| CFG / MedIR | 72/72 | 72/72 |
| HighIR processing | 72/72 | 72/72 |
| Solana metadata | 72/72 | 72/72 |
| LLVM export | 72/72 | 72/72 |
| C export | 72/72 | 72/72 |
| Rust export | 72/72 | 72/72 |
| CALLX-context query | 72/72 | 72/72 |
| C native object | 72/72 | 72/72 |
| Rust native object | 72/72 | 72/72 |
| Required SBF tests | 468/468 | 471/471 |

There were no crashes, timeouts, compiler failures or skipped required tests.
Official SBPF, Agave and protocol-ceiling scale checks were mandatory. The three
new raw-execution regressions retain a real recursive base return, a CALLX
executed before nontermination, and a returning sibling's memory facts.
All 216 LLVM/C/Rust exports match the baseline byte-for-byte. C and Rust each
cover 5,899,345 raw slots exactly once; all 72 source partitions agree.

| Conditional recovery metric | Before | After |
|---|---:|---:|
| Singleton sites | 194 | 590 |
| Finite-set sites | 31 | 49 |
| Either conditional address proof | 225 | 639 |
| Sites with no conditional address proof | 7,349 | 6,935 |
| Global-budget-limited samples | 2 | 0 |
| Context instruction visits | 170,102,731 | 106,683,295 |
| Function summaries | 120,986 | 123,992 |
| Generic return expression instantiations | 2 | 116 |

This is a net gain, not monotonic proof retention: 419 sites gain a conditional
proof and five lose one. The lost sites are boop-fun 149476 and spl-token-swap
31501, 31530, 32233, 32261. The last two have raw-unreachable old prefixes.
An independent raw ELF matched-return audit verifies all 1,385 nonreturning
edges. Of 3,196 removed caller-entry target relations, 3,185 have a proven
unreachable prefix. The other 11 remain precision losses under changed
specialization/input propagation; they are not counted as justified pruning:

| Sample | Caller entry | Call slot | CALLX slot |
|---|---:|---:|---:|
| boop-fun | 147703 | 147756 | 149476 |
| heaven | 134053 | 134053 | 149441 |
| meteora-damm-v1 | 3733 | 3733 | 162124 |
| meteora-damm-v2 | 3537 | 3537 | 160763 |
| meteora-dlmm | 239194 | 239194 | 247880 |
| perena | 85783 | 86385 | 31035 |
| pump-fun | 5484 | 5484 | 220358 |
| raydium-clmm | 7667 | 7667 | 169724 |
| raydium-launchlab | 118937 | 118937 | 127781 |
| spl-token-swap | 31833 | 31837 | 31501 |
| spl-token-swap | 31833 | 31837 | 31530 |

Serialized expression witnesses change from 2,290 to 601: 1,700 removed and
11 added. Of the removals, 1,695 map to raw-unreachable prefixes and two to the
precision losses above. Three have no established condition equivalence
(obric-v2 twice and pump-fun once). They remain audit gaps rather than a claim
that equal target addresses imply equal conditional evidence.

Lifinity-v1 singleton sites increase from 5 to 19, but slot 1963 is still
unproven. Its unknown contexts decrease from 16 to ten: six missing full-width
fields at 1960 and four unknown entry-register contexts. The successful
32470 allocation and 32478 copy retain the captured identity. The 32487 error
helper still falls back to a generic summary; its missing tag/payload at 32488
prevents preserving r8 at the 32493 join and output+96 at 32926. Returning
reallocation/copy paths and parameter-dependent aliasing remain open. A temporary
inline-copy experiment improves some helper summaries but adds no target site;
it was restored and is not part of this qualification.

Whole-program structuring remains 0/72. All 7,574 raw CALLX sites retain dynamic
fallback, and 71 samples still contain sites without any conditional proof.
Bank/CPI business replay, other host platforms and byte-identical source
reconstruction remain outside this result.

The subsequent merge of dev `967a75a12` is separately qualified at
`051d38a42cef8f371eeed8b23ecb8d0f91cfa29d`. This includes dev's Capstone update
from `45dcfb3f8` to `3c00e8faf`, so it is not relabelled as the same-dependency
comparison above. SBF sources/protocol and all other dependency inputs match.
A clean rebuild passed the same 471 mandatory tests and another independent
complete 72-SO run under the original budgets. Every stage and both native
compilations passed. All 288 LLVM/C/Rust/query outputs match the isolated fix
byte-for-byte, as do all 72 analysis stage details. The integrated worker
SHA-256 is
`08aaadcee3b97b8f2ca498ea0c72ea85929034077ce400f8f171ff817d1f756a`;
report SHA-256 is
`1b69b0633d61e5e88029c6a71ca122b30533f13ed42ec143bb45aa50bdb39267`.
These local results do not claim all GitHub platform checks passed.

## 2026-10-28 P3 inline memory-transfer qualification

Small copies in lifinity-v1's formatter execute guest load/store loops rather
than the wrapper's memcpy syscall branch. A generic wrapper previously erased
captured fields at that boundary. Schema 12 replays a proven scalar path through
canonical memory transfers without allocating a fifth specialization. Unknown
branches, other guest calls and repeated block/register states cannot establish
a returning path. Temporary snapshots, effects and instructions use the existing
budgets; store effects cover complete destination alternatives, with no heap
freshness or non-aliasing assumption. Inline edge counters remain distinct from
actual copy-syscall slots.

The same-dependency baseline is `051d38a42cef8f371eeed8b23ecb8d0f91cfa29d`;
the isolated fix is `57ff53aa4b698cafe96b7a3975846a3bd0e64c5f`. Its worker SHA-256 is
`76961ae9a2566b8e19f43425ed73d57668b8495d59e16e6466238dae9b3f6a5f`;
report SHA-256 is
`c7676f05c1606832171dbc087c044b0ee6b8c01515b950fbb68988096de8f120`.
The full independent 72-SO run retains the original 60-second request limit,
8 GiB sampled process-group RSS, 512 MiB output, two concurrent assets and all
internal limits above. Input hashes, profiles, compilers/flags and dependencies
match. No isolated retry is substituted.

All fourteen stages in the preceding table remain 72/72: load, runtime image,
decode, control flow, requisite verification, CFG/MedIR, HighIR processing,
Solana metadata, LLVM/C/Rust export, context queries and both native object
compilations. Required SBF tests remain 471/471, with zero skipped/disabled
tests; official SBPF, Agave and protocol-ceiling checks are mandatory. Additional
raw controls exercise a two-iteration copy after four live specializations,
an unknown selector, aliased allocation addresses and a nonreturning loop that
must not publish provisional stores. The integrated fixture restores the known
source after warm-up so rejection controls reach the intended replay boundary.

| Metric | Before | After |
|---|---:|---:|
| Singleton CALLX sites | 590 | 590 |
| Finite-set CALLX sites | 49 | 49 |
| Either conditional address proof | 639 | 639 |
| Sites without a conditional address proof | 6,935 | 6,935 |
| Global-budget-limited samples | 0 | 0 |
| Unknown-write summaries | 91,801 | 90,942 |
| Output parameterized fields | 48,841 | 50,253 |
| Applied stored values | 445,717 | 447,074 |
| Instantiated inline memory-path edges | 0 | 2,219 |
| Context instruction visits | 106,683,295 | 107,082,728 |

No target-address, caller-slot or caller-entry relation is added or lost.
All 601 serialized expression witness forms remain unchanged, also after frame
normalization. These field improvements are not new target-site coverage.
All 216 LLVM/C/Rust files remain byte-identical to the baseline. Each source
backend covers 5,899,345 raw slots exactly once, with all 72 partitions agreeing.
There are no crashes, timeouts or native compiler failures.

Lifinity-v1 retains 19 singleton sites. Helpers 182, 60926 and 3034 each reduce
unknown-write summaries from five to two; helper 182 gains six parameterized
output fields. Slot 1963 remains unproven in ten contexts: six lack the full
field at 1960 and four lack an incoming register. A same-output state observer
confirms that 32470/32478 preserve the allocation/copy, while the generic helper
at 32487 loses tag/payload facts before the 32493 merge and out+96 at 32926.
The constructor's five summaries still all contain unknown writes. A separate
raw slice identifies a narrow-tag boundary: 4334 stores one byte at out+621,
and 32465 loads FP-3. Typed narrow facts and tag/payload correlations remain
unimplemented; that observation does not prove the final target.

The latest-dev integration is `ca361fed2c2c0676fbad9da9cb5a7f8f555b81b9` against
`f7d924bd0fc802cdf74099c3463ea2d37bafa05d`. Dev updates Capstone from
`3c00e8faf` to `8e962575d`; this is separately qualified rather than relabelled
as the same-dependency comparison. Production SBF/protocol sources match the
isolated fix, while the rejection fixture is strengthened. A rebuild passes
471 mandatory tests and another independent full72 under the original budget.
All 288 LLVM/C/Rust/query outputs and all 72 analysis stage details match the
isolated fix. Integrated worker SHA-256:
`105cb716fdacfb8f966e199b93df784c2cd3fb28d20cb2508e317119cf2796cf`;
report SHA-256:
`23d4104a5edb39d23c7b8550c07d537720e5a73a45653b5605dde2f91a8473c0`.

Whole-program structuring remains 0/72, and all 7,574 raw CALLX sites keep
dynamic fallback. Seventy-one samples retain unproven sites. The previous
11 caller-entry precision losses and three unestablished witness equivalences
remain open. Real bank/CPI replay, all-platform GitHub CI and byte-identical
source reconstruction are not established by these local tests.
