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
every site retains dynamic fallback. Schema 3 covers complete function CFGs,
nested parameters, return values and typed scratch effects. It retains prior
entry-prefix evidence under a separate scope and records generalized inputs,
summary dependencies and resource/recursion barriers. It also records projected
inputs, complete bounded value alternatives, parameter-relative write intervals,
return expressions, and reclaimed solver-only summaries. A finite list of
remaining CALLX values is not counted as a unique target proof. Unknown targets
include the first
missing incoming register or load fact when available. A successful query with
zero targets is an analysis pass, not a devirtualization pass. Neither these
conditional facts nor the global unresolved CALLX count can stand in for the
other.

Source dispatcher regions use the shared `SBFSourcePartition` layout: every raw
slot occurs once, including unreachable and continuation slots. HighIR function
ownership determines grouping; shared tails and unowned ranges remain explicit.
`MaximumDispatcherRegionSlots` bounds each region's compiler CFG work. The
monolithic layout remains an opt-out used by source execution differential
checks. This is a compiler scalability boundary, not a completed function source
view or a business-logic recovery claim.

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
