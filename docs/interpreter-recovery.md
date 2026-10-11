**Languages**: [English](interpreter-recovery.md) | [简体中文](zh-CN/interpreter-recovery.md) | [繁體中文](zh-TW/interpreter-recovery.md) | [日本語](ja/interpreter-recovery.md) | [한국어](ko/interpreter-recovery.md) | [Français](fr/interpreter-recovery.md) | [Deutsch](de/interpreter-recovery.md) | [Español](es/interpreter-recovery.md) | [Italiano](it/interpreter-recovery.md) | [Русский](ru/interpreter-recovery.md) | [العربية](ar/interpreter-recovery.md)

[← Documentation Index](README.md)

# Interpreter source recovery

The experimental interpreter specialization stage removes statically resolved
dispatch from a linked x64 function while retaining its runtime inputs, memory
effects, branches, and loops. It uses instruction semantics rather than handler
signatures or a particular protector's opcode table.

```sh
neverd decompile program --func vm_entry --devirtualize \
  --recovery-report recovery.json -o recovered.c
neverd decompile program --func vm_entry --devirtualize \
  --llvm -o recovered-llvm.c
```

Optional `--vm-control` selects full general registers that distinguish interpreter
contexts. It can be repeated, and it supplies no concrete values. For example,
select a bytecode cursor whose value is established by the entry stub. A runtime
input used as a counter must remain dynamic. Missing context separation can
cause recovery to stop when distinct cursor values meet; the engine must not
guess a dispatch target to compensate. For a spilled cursor,
`--vm-control-stack=-16:8` selects eight bytes at entry-RSP minus 16. The offset
is relative to the function entry, not the current adjusted stack pointer.

The C API is `neverd_devirtualize_source_v1()` in
`neverd/sdk/NeverDCAPIDevirtualize.h`. It runs a separate transaction without
modifying the session's ordinary decompilation cache. Failure returns no source
and can still return a JSON diagnostic. Both owned strings use
`neverd_free_string()`.

<!-- i18n-section: control-discovery -->

## Automatic control-state discovery

The CLI and all source-recovery C API versions enable automatic discovery by
default.
Provider-neutral C++ callers opt in with `SpecializationOptions::DiscoverControlState = true`;
its default is `false`. Manual `--vm-control` and `--vm-control-stack` hints remain
optional context keys. Ordinary automatically discovered fields preserve bounded
joint finite-value relations without creating context keys or binding runtime
inputs to sampled values. Ordinary counters remain dynamic unless their proven
constants are needed for the selective memory refinement below.

Discovery follows unresolved control and address dependencies through
structured node-entry register inputs and memory-input creation origins,
including narrow byte slices. A frame slot is nominated only when its creation
origin identifies untouched node-entry memory at an exact entry-frame-relative
range. Later loads of equal or forwarded values do not create additional input
dependencies, and unknown bytes created after memory clobbering are not
treated as entry slots. Historical load records remain available to other
analyses. Missing dependencies trigger refinement from the function entry.
Every retained relation still needs a complete finite-value proof for the bits
it constrains, and aliasing writes still invalidate memory facts. Arbitrary
external memory and unbounded correlated values are not made finite by discovery.

Producer requests preserve bit masks across node boundaries without rounding
them to full bytes; arithmetic still conservatively includes the lower-bit
prefix that can carry or borrow into a demanded bit.

If already tracked memory dependencies repeatedly prevent an exact address
proof, refinement may additionally use their proven incoming constants as
context keys. Complete 64-bit register values or spilled pointers proved equal
to the entry frame root plus an exact displacement can also distinguish these
contexts. The key retains the displacement, not a guessed numeric invocation
address. Partial or potentially aliasing writes invalidate the pointer fact.

After ordinary discovery stalls, recovery may partition one existing register context field per non-entry native cursor and instruction mode. Its incoming domain must be complete, varying and cover all declared field bits. Every actual predecessor independently proves its current complete domain, compares the live physical field and reprojects each case. Later predecessors and widened nodes are checked again; chaining stops at nominated entries. Comparisons have synthetic provenance and share node, operation, context, query and refinement budgets. Partial masks, undefined flags and frame-only fields do not qualify. No guest-memory read or caller assumption is added. Incomplete domains retain the conservative edge, and publication still requires every reachable case to finish.

Conditional guards may nominate dependencies for bounded refinement before the
final control proof. When an imprecise join exposes an unsupported successor,
the candidate search selects the nearest undecided predecessor guards; it is
not a complete backward slice. A nomination never proves a guard false.
Reachability and every retained target still require the complete proof; a
remaining reachable unsupported operation or exhausted required budget prevents
publication.

Backward producer demands identify a native node entry, instruction mode, and
field kind and byte range. Only an edge whose successor demands that field
expands its producer dependencies, including finite domains that remain too
imprecise. This may trigger bounded restarts from entry without assigning one
global role to a reused physical register. Dependency discovery is bounded and
incomplete; recovery without manual hints is not guaranteed for every
interpreter.

Projection of ordinary automatic fields is also scoped to the destination
node's demands. A nonconstant field enters an edge's joint finite-value
relation only when the destination demands it. Manual fields and
automatically promoted context-key fields continue to be projected globally.
Known constant bytes, exact pointers relative to the entry frame, and
provenance facts are preserved independently of demand. This prevents
unrelated fields in different handler phases from multiplying the relation's
value combinations. The configured budgets and the required proofs for control
targets, memory addresses, and return state remain unchanged.

For ordinary automatic fields, each tuple column constrains only the demanded
bits and carries the corresponding mask. Joins retain only bits constrained on
every incoming path. Other bits in the same byte remain runtime values: a
field's storage range does not certify a complete finite domain over that
entire range. A byte becomes constant only when all eight bits are proved
constant.

Manual fields and promoted context fields still try full-width relations
globally. If that proof is inconclusive, they may instead retain a relation
for demanded bits; this never supplies unproved bytes to a context key or
bypasses an exhausted global budget.

At a fresh-graph restart, ordinary automatic storage ranges fully contained
in other fields can share those wider carriers. Original phase locations and
bit demands remain unchanged. Manual fields, context fields, and fields
nominated directly by an unresolved memory address retain their exact ranges.
`MaxControlFields` limits the actual retained fields after this normalization.
`DiscoveredControlFields` remains cumulative and can exceed the active count;
the field limit and global work budgets are not raised.

An ordinary automatic field with a complete finite-domain proof does not
immediately add all of its producers as control fields. Discovery records
those candidate dependencies and activates them only if recovery remains
blocked and immediate refinement produces no candidates. Fields promoted to
context keys continue to expand their producers immediately. Discovery visits,
restarts, and proof work retain their existing cumulative budgets.

After a failed attempt and the usual candidate selection, a bounded backward
worklist propagates pending exact bit demands through observed native
transfers. It reuses the shared scalar evaluator only for ranges carried by
current control fields, adding no fields or contexts. The old graph routes
candidates; its values and path feasibility are not imported as new facts.
Predecessor traversal, replay, and dependency discovery share the cumulative
work budgets. This can pass demands through long chains without one full
restart per native phase; publication still requires a fresh complete fixed
point.

A complete finite domain can also prove individual byte lanes constant even
when the whole word varies. For example, the domain `{0, 0x100}` has a constant
low byte. Only bytes equal in every enumerated tuple are retained, using the
target byte order; other lanes remain dynamic. Partial enumeration, unknown
solver results, or exhausted proof budgets supply no such facts.

Finite proofs can be reused within one recovery run, including refinement
restarts. A bounded cache compares the complete ordered expression DAG modulo
consistent renaming of free variables; variable sharing, widths, constant
bits, operator payloads, and the projection limit remain part of the key.
Only complete domains and proofs that a domain exceeds its limit are cached.
Unknown or partial results are not cached; cache misses or capacity limits use
the ordinary proof path. All global budgets remain enforced, and
`solverQueries` counts actual solver calls.

Under the same predicate, one complete varying-column domain and proved
singleton values for every other column determine the exact joint relation,
provided reachability is established. Multiple varying columns still require
a joint proof. A masked column covering its entire bit domain can be omitted
only after proving its input bits independent of the predicate and every other
column. Shared inputs, unknown results, and partial enumeration never justify
assuming a Cartesian product.

Defaults are `MaxControlFields = 16` for manual and automatic fields together,
`MaxControlRefinements = 16`, and `MaxDiscoveryVisits = 65536`. Restarts share
global node (including synthetic nodes), operation, evaluation, solver-query,
and discovery-visit budgets. The reported `contexts` counts native contexts;
`contexts`, `evaluatedOperations`, `nodeEvaluations`, and `solverQueries` accumulate
across attempts. Per-address contexts, active native return slots, control-field
counts, and tuple counts remain structural limits within each attempt. The
default solver-query limit remains 4096. Exhaustion publishes no partial result;
`residualBlocks` describes only the final residual graph.

The CLI option `--vm-max-refinements=N` requires a positive integer and
defaults to 16. C callers can select the same bound through
`neverd_devirtualize_source_v2()` or
`neverd_devirtualize_machine_source_v2()`: zero-initialize
`neverd_devirtualize_options_v2`, set
`base.struct_size = sizeof(neverd_devirtualize_options_v2)`, and set
`max_control_refinements` (zero keeps the default of 16). The embedded `base`
holds the v1 options; both reserved members must remain zero. Existing v1
layouts and entry points are unchanged and ignore extension tails. Other
work and proof budgets still apply.

`--vm-max-fields=N` and `--vm-max-queries=N` expose the control-field and solver-query limits, with unchanged defaults of 16 and 4096. These options and `--vm-max-refinements` require positive 32-bit decimal integers and `--devirtualize`; zero, signs, hexadecimal, trailing text and overflow are rejected. Both source ABIs and backends use the same limits. Larger budgets permit more analysis work; they supply no control hints or runtime values and provide no new proof guarantee. Exhaustion still publishes no C.

C callers use `neverd_devirtualize_source_v3()` or `neverd_devirtualize_machine_source_v3()`. Zero-initialize `neverd_devirtualize_options_v3` and set `base.base.struct_size = sizeof(neverd_devirtualize_options_v3)`. Set `max_control_fields`, `max_solver_queries` and optionally `base.max_control_refinements`; zero selects the corresponding unchanged default. All three reserved fields must be zero. v1/v2 entry points ignore the v3 tail, including its reserved member, while v3 ignores future tails. The report records effective `maxControlFields`, `maxControlRefinements` and `maxSolverQueries` alongside actual work.

Recovery also exposes `--vm-chain-transfers=N` (default 0) and `--vm-no-control-discovery`. Chaining retains symbolic correlations across proved singleton transfers; its limit returns to ordinary CFG boundaries. Machine-state recovery can declare unchecked, nonwrapping entry-RSP offsets with `--vm-entry-frame=begin:end`. The exact numeric premise accompanies generated C and the report; it grants no memory access or equivalence proof.

Large recovered functions that exceed the SSA construction limit can use `--llvm` through a bounded scalar mutable-storage contract. Entry inputs, loop-carried values and earlier reads retain their meaning. Unsupported implicit state, vector-register parameters, image relocation, ambiguous storage and malformed control fail explicitly; HighC rejects this fallback. Source output still uses the existing machine-state contract and adds no equivalence certificate.

The mutable subset accepts 8/16/32/64/128-bit scalar storage; bit-count inputs are limited to 64 bits. Nonstandard widths and wider storage require a separate source contract.

For validated mutable bodies, LLVM forwards exact private-slot values within an append-only basic block and keeps its final stores. Entry setup and block, function and module boundaries remain separate. Guest memory accesses stay explicit. C emission bounds scalar expression expansion and immediate-fold work, retaining named intermediates. Foldable constant expressions use LLVM’s target layout; unsupported expressions fail explicitly. Designated mutable return values, including zero, cannot become void returns.

The compatible v4 APIs are `neverd_devirtualize_source_v4()` and `neverd_devirtualize_machine_source_v4()`. Zero-initialize `neverd_devirtualize_options_v4` and set `base.base.base.struct_size` to its full size. Chaining accepts a nonnegative 32-bit decimal CLI count, including zero to disable it. Bounds use signed 64-bit decimal endpoints with `begin < end`, require `--vm-machine-state`, and constrain the physical entry RSP rather than its adjusted value. In C, the bounds flag requires the machine-state API; without it both endpoints must be zero. Unknown flags and old reserved fields are rejected. v1/v2/v3 ignore the entire v4 tail; v4 ignores future tails. Null options retain old defaults. The report adds `maxChainedTransfers` and `entryFrameBounds`; `discoverControlState` records the effective switch. All CLI options require `--devirtualize`. The numeric premise is not checked at runtime and establishes no accessibility, initialization or nonalias guarantee. These options do not enable a native proof policy.

```c
neverd_devirtualize_options_v4 options = {0};
options.base.base.base.struct_size = sizeof(options);
options.base.base.base.use_llvm = 1;
options.max_chained_transfers = 64;
options.flags = NEVERD_DEVIRTUALIZE_V4_DISABLE_CONTROL_DISCOVERY |
                NEVERD_DEVIRTUALIZE_V4_HAS_ENTRY_FRAME_BOUNDS;
options.entry_frame_begin = -256;
options.entry_frame_end = 8;
const char *report = NULL;
const char *source = neverd_devirtualize_machine_source_v4(
    session, entry, &options, &report);
neverd_free_string(source);
neverd_free_string(report);
```

Machine-state recovery accepts `--vm-entry-alignment=A:R` as an explicit, checked entry-RSP domain. `A` must be a positive power of two and `R < A`. Other roots return status 2 before guest accesses or state writes. High root bits remain free; defaults assume no alignment. This option does not provide native equivalence certification.

The v5 C APIs `neverd_devirtualize_source_v5()` and `neverd_devirtualize_machine_source_v5()` extend v4 with `entry_frame_alignment` and `entry_frame_residue`. Zero-initialize `neverd_devirtualize_options_v5` and set `base.base.base.base.struct_size` to its full size. Alignment zero disables the option and requires residue zero. Nonzero alignment is accepted only by the machine-state API. v1–v4 ignore the new tail; v5 ignores future tails. The report records `entryFrameAlignment`; generated C records the same checked condition. Invalid alignment takes precedence over invalid entry flags and leaves all state words unchanged. Existing numeric frame bounds remain unchecked. C++ residual consumers must enforce the declared RSP domain themselves; source and model wrappers share the guard generator. Native proof APIs reject this domain until their contracts can bind it.

`--vm-max-symbolic-nodes=N` and v5 `max_symbolic_nodes` expose the per-node symbolic DAG budget (default 262144). The CLI requires a positive 32-bit decimal count; zero in C selects the default. The same limit applies to both source ABIs. `maxSymbolicNodes` records the effective budget; increasing it supplies no semantic assumption or proof.

`--vm-external-stores-disjoint-frame` adds an explicit, unchecked precondition for machine-state recovery: every external STORE extent must avoid `--vm-entry-frame`. Only existing facts inside that range survive such writes. It does not constrain LOADs or aliasing between external pointers; the default remains conservative. Native proof APIs reject this domain.

The v6 APIs `neverd_devirtualize_source_v6()` and `neverd_devirtualize_machine_source_v6()` embed the unchanged v5 options. Zero-initialize `neverd_devirtualize_options_v6`, set `base.base.base.base.base.struct_size` to its full size, and enable `NEVERD_DEVIRTUALIZE_V6_EXTERNAL_STORES_DISJOINT_ENTRY_FRAME` in `flags` only with machine source and v4 entry-frame bounds. Ordinary source rejects the flag. v1–v5 ignore the extension; v6 ignores future tails. The report records `externalStoresDisjointEntryFrame` and the unchecked contract; generated C states the same premise. Frame-derived or unknown-origin writes receive no exemption. Complete affine spills must fit entirely inside the protected range. Scanning, saving and restoring existing facts consumes the shared operation budget; exhaustion publishes no C.

The v7 APIs `neverd_devirtualize_source_v7()` and `neverd_devirtualize_machine_source_v7()` add `max_node_evaluations` and `max_discovery_visits` to the unchanged v6 prefix. Zero selects 16384 evaluations and 65536 dependency visits. Zero-initialize `neverd_devirtualize_options_v7` and set `base.base.base.base.base.base.struct_size` to its full size. v1–v6 ignore the extension; v7 ignores future tails and retains inherited validation. Python exposes the matching `abi.NeverDDevirtualizeOptionsV7` layout and typed functions. The CLI flags `--vm-max-evaluations=N` and `--vm-max-discovery-visits=N` require `--devirtualize` and positive 32-bit decimal values, including 4294967295. Reports retain the effective `maxNodeEvaluations` and `maxDiscoveryVisits`. These cumulative work limits do not change semantic premises; budget refusal publishes no C or recovery witnesses.

The v7 `flags` field also accepts `NEVERD_DEVIRTUALIZE_V7_STOP_CHAIN_AT_REPEAT` (Python: `DevirtualizeFlagsV7.STOP_CHAIN_AT_REPEAT`), exposed as `--vm-chain-stop-at-repeat`. It ends an ordinary proved-singleton chain before a repeated native destination, identified by address and decode mode, and uses ordinary edge projection. It can reduce loop unrolling but lose correlations required to resolve later control, so an otherwise successful recovery may refuse output. Flags default to zero; a zero chain limit makes this option a no-op. Dependency replay follows committed occurrences unchanged. Both source ABIs report `stopChainingAtRepeatedDestination`; unknown v7 flags are rejected. This strategy adds no semantic premise or permission to publish unresolved control.

`--vm-chain-visits=N` adds a nonnegative 32-bit cap per native address and decode mode within a chain. It counts the initial cursor and chained transfer destinations, not sequential fallthrough. Zero adds no cap; `--vm-chain-stop-at-repeat` still enforces the stricter limit of one. A zero total chain limit makes both controls inactive. The v8 C APIs use `neverd_devirtualize_options_v8`; zero-initialize it, set `base.base.base.base.base.base.base.struct_size` to its full size, and set `max_chained_visits_per_destination`. `reserved` must be zero. v1–v7 ignore this tail; v8 ignores future tails. Python exposes `NeverDDevirtualizeOptionsV8`. Reports retain `maxChainedVisitsPerDestination` and `effectiveChainedVisitsPerDestination`. This option requires `--devirtualize` and changes analysis precision/work without adding semantic premises.

Finite multi-target indirect dispatch also retains deferred guard dependencies: a join can keep a finite target set while admitting infeasible arms. Failed-attempt reverse search skips guards that cannot nominate a new field, context or producer bit, allowing useful outer guards to be considered. Selection and activation share one rule and discovery budget. These nominations never remove edges; publication still requires fresh proofs for the complete reachable graph. The finite-dispatch dependency walk starts only after existing precision refinements stall. Enabling it consumes one fresh refinement under the same cumulative work limits, so unrelated selectors cannot preempt an existing producer or native-guard refinement. Exhausted guard nominations still leave deferred producers and other precision fallbacks eligible; only a new proof can close the failure.

Control and guard refinement precedes optional frame-partition retries; necessary finer partitions remain available. Recovery finishes each residue’s fixed point before starting the next, but publication requires every allowed residue to complete. Explicit entry alignment intersects the partition domain, and dispatch compares actual residues. Context, operation, node and solver budgets remain bounded and shared across retries.

An unresolved indirect target can result from a widened guard. After direct target and deferred producer refinements stop adding candidates, recovery examines the nearest undecided guards that reach the failure. Selection and retries share the discovery and refinement budgets. Only a fresh complete proof can rule out an arm; reachable unknown targets still fail without publishing residual code or witnesses.

A control demand can also require an entry-relative pointer relation. For a complete 64-bit register carrier demanded by the successor, recovery may prove its displacement from the entry root under that edge’s condition even without an alignment mask. Only a unique full-width result enters that edge’s projection; unknown results add no fact, and joins discard conflicting or missing offsets. Physical root values remain free.

For sums up to 64 bits, bounded structural masks can prove that all addends have pairwise-disjoint possible-one bits. Only then can an upper control slice exclude lower carry dependencies. The check includes constants and every addend across the full word; unknown shapes, overlapping bits and wider sums keep the conservative low-prefix rule. Mask inspection consumes discovery work, and exhaustion publishes neither candidates nor a root-independence claim. This does not replace finite-domain or target proofs.

When a selected control word mixes a finite selector with a large payload or address domain, recovery can try 32-, 16- and 8-bit windows within the requested mask. Automatic discovery waits for an active consumer; manual hints also participate when discovery is disabled. The search skips already projected bytes and candidates whose independence from the entry frame root cannot be established. Each optional dependency scan has its own `MaxSymbolicNodes` work bound, separate from cumulative `DiscoveryVisits`; all finite proofs share the existing query budget. Proved constant bytes are retained, and the search continues when they cover the complete selected mask. The first remaining exhaustive finite domain is kept in its original bit positions, while other bits remain runtime data. Original producer demands stay immediate after partial proofs. Later predecessors invalidate incompatible or missing facts. This bounded search does not guarantee every useful subfield is found.

The JSON report adds `discoverControlState`, `maxControlRefinements`,
`maxDiscoveryVisits`, `discoveredControlFields`, `discoveredContextFields`,
`controlRefinements`, and `discoveryVisits`. These record enabled behavior,
limits, and analysis work; field discovery alone does not establish successful
recovery.

When a guard narrows an address dependency to a byte slice, refinement also retains enclosing, already tracked eight-byte direct-address fields as context candidates. The original narrow field and producer mask remain unchanged; unrelated wider fields are not promoted. Constants and entry-relative offsets still require proof, and all contexts share the existing limits.

Optional immutable-address enumeration stops when a feasible address lacks a certificate. The original runtime read remains. Observed values and certificates are used only after the complete domain is proved; cache hits still recheck the current read extent, and an observed malformed certificate remains an error.

Recovery can skip optional enumeration of a full 64-bit `root + constant` control value or immutable-read address when the current predicate provably permits more values than the actual `MaxControlTuples` or `MaxImmutableReadAddresses` limit. The proof accounts for the declared nonwrapping frame interval: every fixed low-32-bit residue must have more than that many roots. Only exact top-level conjuncts matching those bounds are omitted from the dependency walk; all remaining conjuncts must leave the fresh root's high 32 bits free. Stricter restrictions, unsupported dependencies and exhausted work fall back to normal solving.

Each proof shares one `MaxSymbolicNodes` work budget across bound matching and dependency walks. A changed path predicate requires a fresh proof. This refusal neither fixes inputs nor proves reachability; runtime reads, narrow producer masks, immutable-read certificates and final feasibility checks remain required.

Finite projection can also retain independent bits inside a partially constrained symbolic input. For variables up to 64 bits, the shared dependency walker unions the demands of the current predicate and every original `RelatedValues` output. Only distinct source bits preserved by exact output-bit mappings and absent from that union count toward the cardinality lower bound. Carry dependencies and repeated sign bits remain conservative; related outputs are never replaced by a Boolean conjunction. Completely absent variables retain the existing fast path, including narrow views of wider sources. All roots, operand passes and bit walks share `MaxSymbolicNodes`; incomplete proofs fall back to normal solving. This pure-expression mode nominates no machine locations and leaves state-aware provenance checks unchanged.

At capacity, finite-proof reuse replaces the least recently used records with eligible proofs that fit individually. Successful lookups refresh recency; duplicate stores, misses and rejected candidates do not. Evicted proofs may need to be established again. Only complete domains or proved domain-limit excesses are retained, and every new solver call still consumes the shared query budget.

Recovery also retains complete finite domains for individual masked control fields. When a joint relation exceeds `MaxControlTuples`, these independent domains can still constrain a target without asserting correlations between fields. Joins union the masked values; an absent or overflowing domain is discarded in full. A changed domain reschedules its node even when the joint relation has already widened. Partial enumeration supplies no facts, and the existing field, tuple, symbolic-node and solver limits remain in force.

If a target remains unresolved after producer, guard, register and frame refinements have no further candidates, recovery can inspect the failed node’s observed predecessors and nominate one already tracked register or frame carrier with committed demanded bits at that entry. The bounded fallback skips manual and existing context fields and function-entry inputs; successful attempts incur no additional scan. A fresh graph must prove every complete constant or exact frame displacement used as a context key. Partial carriers and later unknown predecessors remain conservative, and immutable reads still require certificates. Discovery visits and refinement, node and context budgets remain shared.

`MaxChainedTransfers` is an opt-in C++ limit (default `0`) for consecutive control transfers whose target or Boolean outcome is proved unique. It preserves full symbolic state, native origins and cumulative budgets; multiple outcomes use ordinary CFG edges. Backward discovery replays committed instruction occurrences. Chaining may duplicate loop origins and limit automatic cutpoint inference.

`EntryFrameBounds` explicitly declares a nonwrapping `[Begin, End)` range around the entry value of `FrameBaseRegister`. It grants no memory-access or nonalias facts. Without it, roots remain modular. Native proof requires matching caller frame bounds and binds them into its receipt; LLVM proof retains that frame's existing domain. Recovery alone is not an equivalence certificate.

<!-- i18n-section: execution-contract -->

## Default execution contract

The binary adapter currently accepts linked x64 ELF and PE images at their
mapped addresses. Mappings, bytes, and permissions must remain fixed, with no
concurrent mutation. Strict on-demand lifting follows reachable machine code;
unsupported instructions, calls, opaque operations outside the flag forms
described below, ordered memory, unresolved control, and language exception
handling stop recovery.

PE recovery requires full image metadata, including image-wide relocations and
exception records. The CLI loads that metadata before applying `--func`; C API
callers must not first restrict the session with
`neverd_session_restrict_function()`. The binary adapter rejects images loaded
with a restricted function work-set because omitted metadata cannot prove the
absence of fixups or exceptional edges.

Without the PE evidence below, only complete file-backed, read-only ranges without overlapping mappings or
loader fixups can supply constant image reads. Writable tables, unresolved
relocations, and a sampled runtime snapshot are not immutable-read evidence.
COPY relocations and structurally incomplete exception directories are refused.
A PE can contain an undecoded handler in an unrelated function when the
directory and function ranges are complete; reaching that handler still stops
recovery.
The ordinary source ABI reconstructs invocation-private frame storage. Every
external-origin LOAD/STORE range, including addresses computed from external
integers, must be disjoint from both the native private frame and its
reconstructed source storage. This is an explicit environment
precondition for source-frame relocation, not an alias fact inferred from a run.
The shared frame proof rejects escaping frame addresses, frame-dependent
scalar outputs and branches, and reads of uninitialized private bytes. The
explicit machine-state ABI retains original guest addresses and does not use
this private-frame precondition.

`PEFixedImageView` authenticates complete x64 PE images at their preferred base. It checks raw headers, unique mappings, complete DIR64 fields and ordinary import writes; IAT bytes remain excluded. TLS, load-config, delayed/bound imports, CLR and unknown writers are refused. Recovery and native/LLVM proofs bind the same snapshot. The C++ options `MaxImagePreparationBytes` and `MaxImagePreparationRecords` default to 64 MiB and 65536; exhaustion reports `BudgetExceeded`. The image must remain unchanged. This does not establish ASLR, initialization or unpacking equivalence.

Aligned-frame recovery partitions all low entry-root residues while leaving high bits free. It emits an explicit entry dispatch and proves each constant displacement before sharing the original root's symbolic memory bank. Spills, alias invalidation and context joins retain the partition identity. Candidates are tried in increasing size; unrelated wide masks cannot force the largest partition. Context and refinement limits bound partitions and retries; nodes, operations, evaluations and solver queries are cumulative. This recovery support does not yet provide automatic native/LLVM proof aggregation across partitions or duplicated loop origins. Existing checkers may prove offsets implied by their actual path predicates. With explicit physical stack semantics, canonical internal `RET imm16` reads the old return slot and advances RSP by eight plus the unsigned cleanup; outer returns still require zero cleanup. Prefixed return encodings are refused by this projection.

Repeated transfers with 64-bit addresses support 1/2/4/8-byte elements after complete count and direction proofs. Zero count accesses no memory and needs no direction proof under explicit machine state; the ordinary ABI still rejects unbound flags. Each scalar access retains ordinary alias and return-slot checks. Complete copied frame pointers are re-proved from actual memory before edge projection. Unknown counts, exhausted budgets, segmented accesses and narrower address sizes are refused. Native undefined-effect certification for these original REP instructions remains unsupported.

The source domain requires ordinary ABI returns: every external-origin store's
target range is disjoint from the entry return-address slot. This is an explicit
caller/environment precondition, including addresses computed from external
integers; absence of frame provenance does not prove numeric disjointness.
Frame-derived store addresses must prove disjointness from it, and the original
stack pointer must be restored at return. Origin information survives spills and
joins; losing an affine expression does not turn it into an external pointer.
Stack pivots, callee-pop returns, and RET-based dispatch are currently refused.
The binary adapter enforces x64 little-endian semantics.

This produces source and IR for analysis. It does not establish relocation,
unwind, asynchronous-exception, or binary replacement safety. Patch mode rejects
this option. There is no claim that every interpreter or protection
configuration is supported.

The exact x64 `PUSHFQ`/`POPFQ` forms remain in the residual program. Analysis treats each machine flag snapshot as an unknown runtime value; the lifter merges its separately modelled arithmetic flags. Restoring flags remains a runtime effect. A flag-derived address cannot borrow the external-pointer return-slot contract, and an unbounded flag-derived dispatch still fails.

Before a full flag snapshot, every modelled arithmetic or direction flag must have a definition within the recovered function. Any direct flag read also needs a definition on every reachable predecessor, even when symbolic simplification cancels its value. Otherwise recovery refuses instead of emitting an unknown-register trap in C.

LowIR temporaries are local to one lifted native instruction. Every byte read must have been defined earlier in that instruction; a reused offset from a prior instruction or an algebraically cancelled undefined value is not source evidence. Entry constants may bind physical registers only.

<!-- i18n-section: machine-state -->

## Explicit machine-state recovery

```sh
neverd decompile program --func entry --devirtualize --vm-machine-state \
  --recovery-report machine-report.json -o machine.c
```

The separate C entry point is `neverd_devirtualize_machine_source_v1()`.
This opt-in source ABI takes a pointer to 17 aligned `uint64_t` words:
RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8 through R15, then RFLAGS.
Only architecturally defined flag observations are equivalence obligations;
undefined flags are not evidence of a particular processor's behavior.
Programs that feed undefined flags into control flow, addresses, or otherwise
defined outputs need a separate noninterference proof. The current recovery
report does not provide that proof or certify such processor-dependent behavior.
It returns an unsigned 64-bit status. Only zero certifies successful execution;
nonzero means the flag profile was violated and does not undo memory effects.
The output state is captured immediately before the final native RET pops its
return address. State storage must be disjoint from all guest memory. Guest
addresses use the original fixed mappings on a little-endian 64-bit host.
The entry return-address slot must remain disjoint from all external-origin
STORE ranges, including computed addresses, as in the default return contract.

Original guest code and data addresses retain their exact numeric values,
including addresses observed in output registers. Source generation must not
replace them with addresses of globals in the generated program. The caller
supplies the required original guest mappings; generating C does not relocate
the guest address space. Proofs and recovery reports continue to refer to the
original input image.

The profile is 64-bit user mode at CPL3/IOPL0, shadow stacks disabled, normal
nonfaulting execution without asynchronous events. Entry flag images must be
canonical with TF, RF, VM, AC, VIF and VIP clear; executed POPFQ images must keep TF and
AC clear. Generated source checks these flag conditions. PUSHFQ/POPFQ use the
explicit guest state rather than the compiler's live flags. IF, IOPL and
reserved bits follow the user-mode preservation rules. RDSSP preserves its
destination when shadow stacks are disabled; reached INCSSP is unsupported.
These rules follow the [Intel instruction reference](https://cdrdv2-public.intel.com/671110/325383-sdm-vol-2abcd.pdf).

Under this machine-state ABI, the adapter certifies direct near CALL and
register-indirect near CALL with an exhaustively proved finite target set.
A register-indirect call captures the original target register before changing
RSP and stores the actual fallthrough address exactly once.

Memory-indirect near CALL is also supported under this explicit machine-state
ABI with shadow stacks disabled and normal nonfaulting execution. Its memory
operand must use a canonical unsegmented `r/m64` encoding, optionally with
address-size override (`addr32`) and REX. The shared x64 effective-address and
LOAD semantics read the target before the call changes RSP or pushes the actual
fallthrough address. A slot
address is never substituted for the loaded callee; this order also applies
when the push overwrites the target slot.

Accepted target proofs include immutable read evidence for read-only pointer
slots or finite tables, and initialized guest-stack values whose complete finite
target set can be proved. A writable slot's initial image bytes or a runtime
snapshot are not immutable evidence. Unproved external loads, clobbered stack
facts and invalid targets still refuse recovery. FS/GS or other segment
overrides, far calls, extra prefixes and noncanonical prefix sequences remain
unsupported. The default source ABI still refuses native calls.

An internal near RET may select from a completely proved finite target set.
The residual code retains one guest stack read, captures its value before the
stack increment, and dispatches on that captured value. Reaching the preserved
entry return slot remains an outer exit, even after discarding an internal
frame. Unknown targets, sets containing missing or nonexecutable destinations,
outer callee-pop returns and arbitrary stack pivots remain unsupported. Exact frame
pointers survive complete spills, with partial or potentially aliasing writes
invalidating the facts. Active stack positions and return-slot values separate
contexts and are budgeted.

Exception handler metadata may be traversed only under this explicit normal
execution profile; exception dispatch and unwind equivalence are not certified.
The report records `sourceABI` and `executionProfile`. The default API retains
its stricter call, entry-flag and exception rejection. The machine-state wrapper
remaps guest registers and uses the existing LowIR/MedIR/HighC/LLVMC scalar
pipeline. It does not introduce a second instruction evaluator.

`modelInterpreterMachineStateX64` returns an `InterpreterMachineStateModel` through the same generator as the source wrapper. Register bytes `[0, 136)` represent the raw 17-word state object; `RETURN` carries status separately from guest RAX, and guest memory stays memory. Invalid entry flags and rejected dynamic flag writes retain sticky failure. The abstraction requires accessible, aligned state storage disjoint from guest accesses. `MaxOperations` bounds input metadata and generated operations. Its deterministic LowIR records are not architecture-undefined-output evidence. Callers must supply observations, entry and frame contracts, and a fresh `checkLowIRLoopRefinement` or finite refinement proof; neither model generation nor these records certify compiled C.

`modelLLVMInterpreterMachineStateX64` models a verified scalar LLVM function with one state pointer and an i64 status result. `llvmInterpreterMachineStateContract` observes all 17 words and requires the separate `LLVMInterpreterDefinednessOffset` byte to start and remain zero. Keep these obligations when adding the entry domain and guest frame. PHI copies are parallel; integer overflow, exact shifts, range attributes and related conditions become checked guards. Every executed admitted operation must be non-poison, even when dead code or select could mask poison under [LLVM semantics](https://llvm.org/docs/UndefinedBehavior.html). `MaxInputItems`, `MaxBlocks`, `MaxOperations` and `MaxWork` bound construction. Unsupported types, pointer escapes, memory modes, calls, attributes and metadata fail explicitly. Variable `shl`, `lshr` and `ashr` on i8/i16/i32/i64 require an unsigned count below the source width without ISA masking; `nuw`, `nsw` and `exact` remain separately checked. Division and freeze are not admitted. A fresh complete finite or inductive check must establish definedness, memory safety, observations and termination; automatic loop proposals remain incomplete. Bind the exact source, module and compiler inputs separately. The parser/compiler remain trusted, and this API does not certify C, generated machine code or physical CPU undefined choices. See the [LLVM language reference](https://llvm.org/docs/LangRef.html) for source contracts. Entry bytes read must be initialized, and guest accesses must designate live storage in the declared frame; flat LowIR memory does not establish LLVM object lifetime or pointer provenance.

`initializes` is accepted only for separated byte ranges inside the state object. A bounded must-dataflow analysis requires ordinary stores to initialize every claimed byte before any read of that byte and before each normal return. Partial stores and fixed pointer aliases share the same byte facts; guest stores receive no credit. Entry starts with no initialized claims, so backedge stores cannot justify a first-iteration read. Atomic/volatile accesses and unknown state aliases remain unsupported. Range-list elements count toward `MaxInputItems` before verification, and fixed-point work shares `MaxWork`. This conservative check may reject valid path-correlated code; it never removes the attribute to obtain a proof.

`prepareInterpreterLLVMRefinement` snapshots exact LLVM text and the selected function name, verifies the module, and prepares both state models with an entry-only canonical flag projection. `checkBinaryLLVMRefinement` rebuilds these inputs and proves the actual native image against the identical residual, then the residual model against that LLVM artifact. Both finite and inductive checks retain all state words, actual status, definedness and frame writes, and require preserved RSP and return slot. Native entry constants are not copied into the source contract: the source relation deliberately covers the larger arbitrary-GPR/canonical-flags domain and may conservatively reject a result valid only under native restrictions. Loop plans are untrusted proposals. Only two successful fresh checks produce a composite receipt binding exact IR bytes, function name, both proof receipts, profile version and limits.

`MaxIRBytes` bounds text and function-name sizes before copying or parsing; `MaxMachineStateOperations` and `MaxPreparationItems` bound model/preparation work. Native and LLVM proof budgets are independent and never raised automatically. The trusted LLVM parser/verifier has no hard CPU, stack or allocation limit here. The shared frame must contain the entry return slot and use the raw RSP word as its root. Initialized live frame storage, accessible aligned disjoint state storage and valid guest-pointer provenance remain required. This C++ API proves the admitted native-to-LLVM relation, not C compilation, generated machine code, faults or physical CPU undefined-bit choices.

Structured machine-state C can share the sixteen GPR writeback stores across ordinary return blocks. Packed flags stay on each return path; alignment rejection still performs no writes. State is captured before the native RET pop, and invalid-profile status remains sticky. LLVM output keeps inline return tails for its own optimization. Existing C++ wrapper/model overloads retain `InlineReturns`; the explicit `InterpreterMachineStateLayout::SharedGPRExit` layout uses the same semantic generator for both. Invalid layout values are rejected. If a complete ordinary join or fresh address space is unavailable, compaction keeps the original inline form. Independent model checks and both C backends at O0/O2 cover branch-specific flags, partial register writes, loops, memory canaries and alignment rejection. Source compaction adds no ordinary-ABI or native-equivalence certificate.

`LowFunc::FunctionTemporaries` declares sorted, disjoint byte ranges that start unbound. Recovery captures a temporary indirect target once into a reserved function-local slot before finite dispatch crosses instruction boundaries. The shared relation checker retains only bytes actually defined on the current path; ordinary native temporaries still expire at each instruction. Range metadata and snapshot construction remain budgeted, and certificate semantic schema 13 binds these lifetimes. The raw-state model preserves byte semantics; source lowering requires exact whole-slot operands and refuses byte aliases. Inductive loop proof and inference currently reject these declarations because cutpoint states do not bind them. This adds no entry assumption or native ABI certificate.

<!-- i18n-section: limits -->

## Current limits

Input-dependent bytecode addresses and decoder-state relationships are supported
only when the required finite domains and correlations can be proved within the
configured limits. This does not establish support for arbitrary indirect
decoding schemes. Dynamic branches and loops may be recovered when every
dispatch target is proved; ordinary branch coverage is not evidence for all such
schemes. Unresolved control and exhausted required proof budgets are failures,
with no recovered source or partial replacement published. External calls, exception/reentry execution, mutable code, and other
architectures remain unsupported. The default source ABI also refuses native
helper calls; the explicit machine-state profile covers only the forms above.

<!-- i18n-section: implementation -->

## Shared implementation

`SpecializationProvider` supplies complete lifted instructions and immutable
read witnesses. `NeverDInterpreterSpecialization` uses the existing `SymExec`
semantics to partially evaluate integer and control operations. The binary
adapter owns mapping and instruction decoding; it does not implement a second
instruction evaluator. The narrow flag-snapshot rule in the specializer is an
overapproximation, not an evaluator for the machine's system flags.

For a finite symbolic read address, the built-in bitvector solver enumerates
candidate addresses under the current constraints. The set is accepted only
after a final UNSAT result proves that no other address is possible, and every
address must have a complete immutable, nonfaulting-read certificate. Such a
certified LOAD can be replaced in LowIR by capturing its address and selecting
the exact value with a SELECT chain; ordinary uncertified loads remain dynamic.
A sampled address is never a substitute for the full set. Selected control
registers and entry-frame slots can retain bounded joint tuples across nodes,
preserving relationships such as a cursor and its decoder key. Joins and
widening remain conservative. Partial SAT samples or unknown solver results are
not proofs of a complete address or target set. This mechanism does not require
the optional Z3 backend.

A node is identified by its native cursor and instruction mode, active native
return-stack state, and proven constants or complete entry-frame-relative
pointer displacements in manual or selectively promoted context fields. Other
byte-level facts meet by intersection. When an incoming fact weakens, the node
is evaluated again. Ordinary business values remain
dynamic; promoted state remains subject to context limits. All reachable values
of an indirect target must belong to a bounded, exhaustively proved set;
selected targets become explicit residual comparisons and CFG edges.

Dynamic operations and ordinary loads/stores stay in LowIR. Scalar constants,
affine entry-frame pointers, and proven constant frame bytes can cross nodes;
other expressions are discarded rather than expanded without bound. Frame memory
uses the existing symbolic state's conservative alias invalidation. A write
through an unknown potentially aliasing pointer invalidates conflicting facts.
This does not assume that a stack slot is private or erase it using an unproved
no-alias contract.

Unique synthetic instruction labels distinguish cloned contexts. The original
instruction boundaries remain in a separate origin map; original relocation,
exception, or jump-table certificates are not copied onto new occurrences. The
recovered LowIR enters the ordinary LowIR-to-MedIR conversion before the
HighC/LLVM route split, sharing register, stack, CFG, SSA, and ABI handling.
Recovery also requires successful MedIR verification on the HighC route.

Node, per-address context, operation, node-evaluation, and finite-target budgets
bound analysis. Budget exhaustion and unsupported semantics publish no residual
function. A complete control graph is distinct from successful source emission;
the public API checks both and reports the distinction.

Finite read-address sets, joint control tuples and control-field counts also
have explicit limits. Global solver-query limits and per-query limits on gates,
conflicts, propagations and watched-literal visits bound proof work;
symbolic-node limits bound expression growth. The JSON report includes these
budgets together with `solverQueries` and `relationalWidenings`.

`AllowOverlappingNativeInstructions` is a separate, default-off option for finite native independence and native-to-LowIR refinement. Each entry is decoded and checked independently; intersecting instruction bytes must agree with all earlier instruction and immutable-read evidence, including candidate reads. Candidate LowIR addresses remain labels, not byte evidence. `MaxNativeInstructionBytes` defaults to 1048576 and charges the full size of every newly fetched entry, including repeated overlapping bytes, before comparison. Exhaustion or conflicting bytes refuses a certificate. The option and limit bind the proof digest. Static LowIR, inductive loop proofs and inference reject the option, even with an empty loop plan; the exact LLVM API and CLI retain their existing defaults.

<!-- i18n-section: evidence -->

## Evidence and tests

The optional local JSON report includes the input hash, selected controls,
budgets, status, work counters, residual block count, original instruction
locations, and immutable bytes used during recovery. It contains input-derived
information and is written only to the requested local path.

The public tests use original register-threaded and stack-dispatch machines,
each with arithmetic, branch/join, and runtime-loop programs. An independent
unsigned oracle checks returns, memory stores, carry/borrow results, and output
canaries. Recovered HighC and LLVMC are compiled at O0/O2 with
undefined-behavior traps and executed against that oracle. Negative cases
exercise unresolved and writable dispatch, incompatible byte order, exception
metadata, and budgets.

Additional original finite-address fixtures use input-selected read-only
records, related cursor/key control fields and a shared handler at different
virtual positions. They cover branch/join behavior and loops whose record choice
depends on live program state. Their independent native oracle uses both SysV
and Win64 calling conventions; both recovered C routes are checked at O0/O2 with
undefined-behavior traps and output canaries. Missing read certificates and
insufficient proof budgets must not publish a partial result.

The extended test matrix requires three further independent shapes to recover
without manual control hints: direct-threaded pointer bytecode, a bounded
software CALL/RET stack with nested virtual calls, and a loop with rotating
opcode-decoder state. Their native SysV/Win64 executions and recovered HighC
and LLVMC at O0/O2 are compared with independent mathematical oracles, including
output canaries. Unknown virtual return cursors, unconstrained decoder keys and
exhausted budgets must refuse publication.

A separate machine-state matrix requires native and both recovered C routes at
O0/O2 to agree on all 16 general registers, defined flags and every byte of the
tested guest stack. Register-indirect CALL callees both read and overwrite the
target register; return-threaded dispatch selects between known destinations.
The oracle checks the actual fallthrough store, unchanged flags and restored
RSP. Symbol lookup supplies expected code addresses. Unknown or invalid target
sets and unproved memory targets must fail without source. These are coverage
requirements for original fixtures, satisfied by local x64 Linux validation;
they are not a guarantee for arbitrary virtual machines or protection products.

The public memory-call fixtures cover immutable RIP-relative pointer slots,
finite input-selected read-only tables, initialized finite guest stack slots,
and a target slot overwritten by the call's own push. Local x64 Linux validation
checks each of these four forms with 1,024 states through native execution,
HighC and LLVMC at O0/O2. The independent oracle compares all 17 state words and
128 guest-stack bytes, including the return address observed by the callee.
Unproved external or writable slots, alias-clobbered stack values, invalid
targets and use through the default source ABI must fail without source.

A separate fixed-address runtime regression supplies writable guest memory at
its original image address. Both C backends at O0/O2 must read and write that
mapping, preserve the original numeric address in register outputs and leave
memory canaries unchanged. Rebinding to a generated global object cannot satisfy
this oracle. These local checks do not establish arbitrary VM support.

See [testing.md](testing.md) for the focused targets.

<!-- i18n-section: loop-proposals -->

## Automatic loop proof proposals

The prefix-predicate preservation requirement below applies when `GeneralizeEntryPrefix = false`.

`inferLowIRLoopRefinementPlan` proposes bounded templates using the shared symbolic executor. Feedback cutpoints cover every CFG cycle; widening retains proved fixed bits and prunes unsigned prefix bounds. Observed unit counters and inferred phases form lexicographic ranks for nested ascending or descending loops. `OriginalPrefix` and `CandidatePrefix` require `UseEntryPrefix`. A cut behind earlier cuts can use a separate bounded replay from the real entry to establish a feasible paired prefix witness. This witness does not cover the entry domain: every actual arrival must imply its predicate, and complete entry and transition coverage remain mandatory. `inferAndCheckBinaryLowIRLoopRefinement` requires complete recovery and unique native origins, then independently reruns the full original/candidate checker. Proposals and origin mappings are untrusted; only `Refinement` can contain a certificate. Inference and proof keep separate explicit budgets. This C++ API does not run automatically with `--devirtualize`. Unreachable prefixes, arbitrary control alignment, rank families outside the search, C-backend equivalence and physical CPU choices for undefined bits remain unsupported.

Unit-counter discovery also recognizes byte-aligned subword updates that preserve every outside bit, after trying the existing full-word and zero-extended forms. For multiple cuts, lane endpoint exclusions are proposed only after proof on saved concrete arrivals and all current incoming states. Widening removes failed guards without reseeding them and can discover another lane after its word is already known. Masks use the existing whole-word parameter; full-word ranks and state observations remain intact. All proposals retain shared node/query limits and require independent complete refinement.

For multiple cuts, each failed observed counter tuple is followed by a variant with a leading 64-bit constant phase, before advancing to the next tuple. Both variants consume separate `MaxRankCandidates` attempts and share the same query budget. The leading phase must be nonincreasing on every feasible transition; if the remaining tuple is not proved strictly decreasing, that transition requires a strict phase decrease. Nonnegative difference constraints reject positive cycles. This allows sequential loops to reuse a counter after reinitialization while preserving ordinary progress obligations inside cycles. Existing between-counter phases can compose with the leading phase. Every complete tuple is checked on the original full transition predicates, and the final refinement checker independently rechecks the proposed rank. Single-cut search does not add this ineffective constant prefix.

Single-cut candidates must cover every cycle among all originally reachable blocks, including cycles disconnected by removing the cut. Each coverage check consumes `MaxCutpointAttempts` before symbolic execution. Scalar counters with unit progress on every returning edge retain priority. Then up to eight observed unit-counter tuple proposals alternate with individual broader scalar guard/bound hypotheses. Remaining scalar hypotheses run before tuple search resumes without replay. This order is independent of the budget cap. Each tuple attempt restores the saved complete stable template and transitions; a failed tuple-domain check disables only that family. Failed scalar guards cannot narrow its domain. `MaxRankCandidates`, query, operation and path budgets remain cumulative. A proposal still needs the complete refinement checker. An additive returning arm can trigger structural widening even when sibling arms stutter or reset; the proposed template must still pass all entry and transition checks.

Single-cut search retries another eligible cut when executing a generalized template violates a preservation or memory contract. For example, a body cut may lose a guarded counter relation while the loop header retains it. A rejected hypothesis does not reject every other cut. Real-entry prefix failures, malformed input, unsupported execution and exhausted budgets still stop. Attempts retain all shared accounting, including `MaxCutpointAttempts` and solver queries; a new proposal still requires a fresh complete proof. This fallback does not search alternative nested feedback sets.

`GeneralizeEntryPrefix` defaults to `false` and requires `UseEntryPrefix`. In the default mode, every arrival must preserve the captured path predicate. The explicit generalized mode instead treats the prefix state expressions as total, untrusted template functions outside that path. It still requires a feasible paired witness, complete real-entry and segment coverage, full register/frame equality, input projections, native guards and strict rank decrease over the expanded induction domain. Inference expands a cut only when an incoming state exceeds the witness domain, then rebuilds and rechecks every general transition. A zero-iteration first witness cannot hide another entry arm that loops. The policy is bound to the inductive certificate digest.

Nested inference may seed strict and non-strict unsigned bounds between observed unit counters and unchanged prefix values referenced in control predicates, including equality exits. It checks concrete entry/incoming states first and monotonically prunes these candidate relations on general incoming transitions. All predicate traversal and solver work uses the existing explicit inference limits; the final original/candidate checker independently proves the resulting templates.

Nested inference also proposes equality between a cached operand and a counter or unchanged control input when their prefix expressions match. It checks every concrete incoming state, retains candidates for caches discovered during later widening, and removes a relation when any general incoming state violates it. Each represented word keeps its own recoverable parameter; the equality is a checked predicate. Copy recurrences may discard spurious fixed bits faster, but never supply assumed semantics.

Nested inference also searches fields with at most 16 varying bits for cached counter equality or inequality. Each relation uses current counter and bound values, with a separate recoverable state parameter for the cache. A guard or constant-folded initializer may hide the comparison until later widening; new tuples can then be proposed, but rejected or pruned tuples are never reseeded. Candidates must hold on all saved concrete arrivals and current incoming transitions. Variable DAG scans are cached per cut and charged to the shared predicate budget. Complete native coverage, full state equality and strict rank checks remain independent requirements. Counter discovery continues after general transitions: an outer counter hidden by the initial inner-loop witness still receives checked bounds and operand-copy candidates. No rejected relation is restored, and unit-step probes obey the symbolic-node budget.

Entry-prefix replays visit queued branches before extending an earlier loop again. This lets a short reachable witness be found when another branch admits an arbitrary number of iterations. Inference and the original/candidate checker share this scheduling rule. All work still consumes the existing budgets; a prefix witness does not replace complete entry coverage, invariant preservation or termination checks.

`pairLowIRLoopRefinementPlans` combines two independently proposed self-relation plans using explicit `LowIRLoopCutpointPair` entries. Every cut must be paired exactly once. `SharedInputs` adds equality predicates between equal-width induction inputs, retaining both bindings and projections for the checker. Other temporaries remain independent, candidate prefix inputs use `CandidatePrefix`, both predicates are retained, and the original rank is selected. Both plans must agree on `UseEntryPrefix`; `GeneralizeEntryPrefix` is enabled if either requires it. Temporary definitions must not overlap and uses must have exact widths. `MaxMetadata` defaults to 65536 and bounds construction before copying. The result is an untrusted proposal: `checkLowIRLoopRefinement` must independently check complete coverage, state/frame equality, observations and strict progress. This helper supplies no certificate and changes no CLI defaults.

`inferAndCheckLowIRLoopRefinement` searches a checked relation between LowIR loops. It tries default, broad branch-arm and filtered branch-arm plans in that order, pairing the same families before all cross-family pairs and then individual cyclic candidate cuts. Broad proposals select successors inside a cyclic component that follow a branching block and have one outgoing edge. The filtered family omits branches only when every path reconverges at a nonterminal node before reaching a DFS backedge target. All successors, including exits and boundaries, participate; a join only at a boundary does not remove the branch. Greedy additions still cover every originally reachable cycle. These proposals preserve action phases without requiring default inference to succeed. `MaxCutSelectionWork` separately bounds the optional common-path analysis, including set construction and comparisons; `CutSelectionWork` reports work even on failure and also consumes the remaining global `MaxSearchWork`. Default and broad selectors are unchanged. Successes and failures are cached, with up to three plans per side and lazy cut permutations. Optional families skip cut sets already retained on that side, even if the existing plan was inferred by a later-numbered family. Empty or duplicate families use no symbolic queries but still consume a candidate attempt. All six retained plans share one `MaxMetadata` pool; each pairing construction is bounded separately. By default, `MaxRankPairingAttempts = 0` preserves the existing proposal order and accounting, proposing equality only for same-width frame inputs at identical offsets. Opting in adds two proposals after each ordinary pairing fails: direct rank bindings, then those bindings plus compatible frame bindings. Each nonempty rank tuple must have the same length on both sides; every component must exactly name a unique `Original` input by full `NdVar` identity, with distinct locations and equal nonzero widths. Constants, expressions, prefix inputs and width conversions are not guessed. The union retains rank bindings and skips frame bindings that conflict at either endpoint. Binding sets duplicating the ordinary proposal or the rank-only proposal are skipped before another complete check. Entered optional attempts, including ineligible and duplicate proposals, consume both `MaxRankPairingAttempts` and global `MaxPairingAttempts`; scans, comparisons and storage consume `MaxSearchWork`, and both working pairing vectors share one per-attempt `MaxMetadata` allowance. Exhausting only the optional cap still permits later ordinary proofs within the remaining global budgets. Affine relations and arbitrary feedback sets still require explicit pairing. The authoritative pairer and complete checker retain the caller's original audited records, witness, entry domain, frame observations and termination obligations. `LowIRLoopAlignmentLimits` shares `MaxSolverQueries` across every inference and proof, including failures; each invocation receives at most its stage limit and the remaining total. `MaxSearchWork`, `MaxMetadata`, `MaxCandidateAttempts`, `MaxPairingAttempts` and `MaxCuts` bound search construction and enumeration. Per-attempt exhaustion may retry; global exhaustion stops. `Unsupported` means no relation was found, not that the programs differ. Only a fresh successful `Refinement` contains a certificate. CLI defaults are unchanged.


Alignment forwards `MaxCuts` to the shared inference owner. After input validation and complete feedback-cut selection, an oversized set fails before symbolic entry exploration or widening. This per-plan cap does not reduce `MaxCutpointAttempts` or legal single-cut retries. Filtered families retain their charged selection work. Standalone and native inference keep their existing limits; every accepted relation still requires the unchanged complete checker.