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
This refinement does not partition multivalue tuples into new edges or emit
extra guest-memory reads: a finite-value proof alone does not establish that an
added load is safe. Dynamic or unbounded memory-dependent state may therefore
still stop recovery within the configured limits.

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

The JSON report adds `discoverControlState`, `maxControlRefinements`,
`maxDiscoveryVisits`, `discoveredControlFields`, `discoveredContextFields`,
`controlRefinements`, and `discoveryVisits`. These record enabled behavior,
limits, and analysis work; field discovery alone does not establish successful
recovery.

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

Only complete file-backed, read-only ranges without overlapping mappings or
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
An indirect call captures the original target register before changing RSP and
stores the actual fallthrough address exactly once. Memory-indirect CALL remains
unsupported, including read-only pointer slots and `[rsp]`: a slot address must
not be treated as a callee address.

An internal near RET may select from a completely proved finite target set.
The residual code retains one guest stack read, captures its value before the
stack increment, and dispatches on that captured value. Reaching the preserved
entry return slot remains an outer exit, even after discarding an internal
frame. Unknown targets, sets containing missing or nonexecutable destinations,
callee-pop returns and arbitrary stack pivots remain unsupported. Exact frame
pointers survive complete spills, with partial or potentially aliasing writes
invalidating the facts. Active stack positions and return-slot values separate
contexts and are budgeted.

Exception handler metadata may be traversed only under this explicit normal
execution profile; exception dispatch and unwind equivalence are not certified.
The report records `sourceABI` and `executionProfile`. The default API retains
its stricter call, entry-flag and exception rejection. The machine-state wrapper
remaps guest registers and uses the existing LowIR/MedIR/HighC/LLVMC scalar
pipeline. It does not introduce a second instruction evaluator.

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
sets and memory-indirect calls must fail without source. These are coverage
requirements for original fixtures, satisfied by local x64 Linux validation;
they are not a guarantee for arbitrary virtual machines or protection products.

See [testing.md](testing.md) for the focused targets.
