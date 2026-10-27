# Structural binary comparison

`neverd diff --structural` compares decoded native functions, basic blocks and
instructions. The desktop exposes the same operation under **File → Compare
binaries…**. Both use the shared comparison C API; the GUI runs it in a separate
worker and does not replace the open project. The original name-based `diff`
command remains available without `--structural`.

Both inputs may be NDDB saved projects, including a project compared with an
ordinary binary. Select the `.nddb` file in the desktop, pass it to `-a`/`-b`,
or use the same worker input `path`. Auto format uses the saved loader,
processor, platform and placement; an explicit load choice takes precedence.
The SDK extracts the embedded binary and known project files into a private
session-owned directory, retained through comparison-owned validation. It never
reuses the desktop's editing or crash-recovery directory.

The shared reader holds one read-only SQLite snapshot and checks chunk sequence,
exact compression framing, lengths, filenames and input SHA-256. Input extraction
streams through fixed buffers without a total-size policy ceiling. Aggregate
metadata/sidecar/state budgets fail explicitly and are configurable through the
shared reader. Cancellation removes unfinished extraction and preserves the
previous comparison. A malformed database is an input error, never raw code.

Provenance keeps `path` as the original database and `sha256` as the embedded
binary's digest. Optional `project` metadata records `format:"nddb-v1"`,
`input_name` and `state_sha256`. Saved analysis project files bind the analysis
revision independently of SQLite layout, compression, temporary paths, desktop
state and undo history. Source decorations and bookmarks do not become native
matching evidence. External debug files are not embedded: saved choices use the
normal debug loaders, missing files fail explicitly, and unchecked selections
require their recorded file stamps. Relative companion paths resolve beside the
NDDB file. Worker freshness checks include WAL and rollback-journal changes.

This is an experimental foundation for issue #1462. The default
`native-same-isa-v1` selects the qualified x86/x64 or 64-bit AArch64 reader.
Explicit `native-x86-v1` and `native-aarch64-v1` restrict that selection. Both
inputs must have the same ISA, bitness and ABI; unsupported pairs fail clearly.
The x86 normalization and saved identity are unchanged by profile selection.
The desktop adds **Raw AArch64** alongside automatic format detection.

AArch64 qualification covers authored raw instructions and compiled ELF
relocatable, COFF object and Mach-O object fixtures through the real loaders,
CLI, worker, source views, SSA/slice/region inspection and saved comparison
binding. It does not establish
qualification for linked applications, every relocation/transformation family
or large production corpora. Mach-O AArch64 identity preserves the complete CPU
subtype, including ARM64e ABI bits; differing subtypes and raw Darwin inputs
without a subtype are not silently paired with them.

The AArch64 reader retains operand widths, register identities, conditions,
shifts/extensions, writeback, vector/predicate modifiers and typed system fields.
Branch destinations are bound to their operand occurrence, so TBZ/TBNZ bit
indices remain scalar evidence. Literal reads use the ordinary lift's full
address, including addresses above 4 GiB. ADRP keeps an image-relative page
fragment with the exact base offset; it does not invent an ADRP+ADD object
correspondence. Other immediates/addends remain scalar unless their architectural
address role is established. Illegal or unsupported instructions retain their
original bytes with incomplete analysis. AArch64 matching does not broaden the
separately qualified x86 scalar-validation and import-effect proof profiles.

## Command line

```sh
neverd diff --structural -a original.exe -b patched.exe
neverd diff --structural -a original.exe -b patched.exe --json --save review.nddiff
neverd diff --structural -a original.exe -b patched.exe --reopen review.nddiff --json
neverd diff --structural -a original.exe -b patched.exe --list changed --limit 200
neverd diff --structural -a original.exe -b patched.exe --func 0x140001000 --json
neverd diff --structural -a original.exe -b patched.exe --ignore-names --timeout-ms 30000
neverd diff --structural -a original.nddb -b patched.nddb --json --save review.nddiff
```

Progress goes to stderr. `--list` supports `all`, `matched`, `changed`,
`ambiguous` and `unmatched`, with `--offset` and `--limit`. `--func` selects a
primary entry address or an unambiguous name and returns paired original bytes,
operands, instruction alignment, CFG and matching evidence.

Ctrl+C cooperatively stops input loading as well as comparison work. Loading
reports per-side stages on stderr. A stop before comparison publication exits
with code 3, writes no JSON result, and leaves an existing `--save` file intact.
Individual file reads, allocations and metadata helpers can delay the next stop
checkpoint; this is not a hard stop deadline. Ordinary load failures exit with
code 1.

Function inventories support `--list-side primary|secondary`, `--list-search`
(name or hexadecimal address), `--list-sort` (address, name, size, similarity,
confidence or changes), and `--list-descending`. To inspect every retained
ambiguity candidate for a primary function, select the secondary inventory:

```sh
neverd diff --structural -a original.exe -b patched.exe --list all \
  --list-side secondary --ambiguous-with 0x140001000 --limit 200
```

Function projections retain the extracted session's display name in `name`,
including analyst renames and listing suffixes. `evidence_name` identifies the
underlying name used by the comparison. Search accepts either name; sorting by
name uses the display name. These presentation annotations are owned by the
comparison and do not alter matches, snapshot revisions or saved report bytes.
When the names differ, the desktop's function tooltips and Evidence tab show the
analyzed name alongside the display name, including the counterpart's names.
The read-only Evidence pane wraps long identities and supports scrolling and
copying the complete text.

Continue with `--offset` using the response's `next_offset`. `ambiguous_with`
always names a function on the opposite side; an absent source is an error,
while a valid source with no retained ambiguity group returns an empty page.
The `ambiguity` metadata identifies the source, group explanation and unfiltered
candidate count. Search and sort apply to the returned candidate functions.
The usual inventory's short `candidates` array remains a preview; it does not
limit this query's total. These are the engine's retained hypotheses, not all
possible correspondences or accepted matches.

In the desktop, select a function and choose **Pair with…**. The resizable
picker pages all retained ambiguity candidates and can switch to **All
functions** for manual pairing. Search accepts names and hexadecimal addresses,
including zero. Rows show current pairings; a selection explains which pairs
will be replaced. **Pair functions** records the edit and **Undo** restores the
previous pairs. Searching, browsing and cancelling do not edit the mapping.
Changed comparison revisions invalidate the picker until it is reopened.

`--options FILE` accepts an object containing:

| Option | Meaning |
| --- | --- |
| `profile` | `native-same-isa-v1` (default), `native-x86-v1` or `native-aarch64-v1` |
| `discover`, `use_names`, `approximate` | Enable discovery, stated names, approximate candidates |
| `min_similarity`, `min_context_similarity`, `ambiguity_margin` | Heuristic thresholds in `[0,1]`; exact score ties stay undecided even with zero ambiguity margin |
| `max_candidates` | Best quick-scored candidates per unmatched function proposed for exact measurement (default 16, maximum 256); proposals from both sides are combined |
| `max_instructions`, `max_functions`, `max_memory_bytes` | Snapshot extraction allowances |
| `timeout_ms` | Comparison deadline; zero disables it |
| `primary`, `secondary` | Optional `{begin,end,function}` filters |
| `overrides` | Ordered analyst decisions |
| `regions` | Boolean or bounded native region settings object |
| `dataflow` | Boolean or bounded native SSA collection settings object |
| `flow_regions` | Boolean or bounded verified SSA neighborhood settings object |

Unknown fields and wrong types fail. Policy defaults and ceilings live in
`include/neverd/diff/Limits.h`. The memory allowance accounts for decoded/model
storage; it is **not a process RSS limit** and does not include caller-owned
loaded images. A budget-limited snapshot is rebuilt as a deterministic prefix in
address order before publication. Cancelled and deadline-limited prefixes can
differ according to when the stop was observed.

`--overrides FILE` applies a JSON array of decisions after comparison/reopen:

```json
[
  {"kind":"match","primary":"0x140001000","secondary":"0x140002000"},
  {"kind":"confirm","primary":"0x140001000","secondary":"0x140002000"}
]
```

`match` replaces conflicting pairs, `unmatch` dissolves a pair, and `confirm`
records an analyst's decision. The SDK and GUI support undo/redo. These actions
change correspondence, not input bytes or project annotations.
When a pairing consumes a group's last candidate on either side, that group
is removed and any remaining functions become unmatched. Groups with candidates
on both sides stay unresolved, including a single remaining pair. Undo restores
the original groups and candidate lists; every intermediate edit can be saved
and reopened.

| Exit | Meaning |
| --- | --- |
| 0 | No observed non-movement changes or unresolved functions, with complete coverage |
| 1 | Load, option, comparison, saved-result binding or output failure |
| 2 | Changes or unresolved functions with complete coverage |
| 3 | Stopped work or incomplete analysis/discovery coverage |

## Native region hypotheses

`--regions` adds the experimental `native-runs-v1` search below function
boundaries. In the desktop, enable **Settings → Region hypotheses** before
comparing, then open **Regions…**. The separate review window pages hypotheses,
shared spans and their paired original instructions; it does not modify accepted
function pairs. Full-function inspection remains available from each span.

```sh
neverd diff --structural -a before.o -b after.o --regions --save regions.nddiff
neverd diff --structural -a before.o -b after.o --list regions --region-kind split
neverd diff --structural -a before.o -b after.o --region region-ID --limit 200
neverd diff --structural -a before.o -b after.o --region region-ID --region-anchor 0 --limit 200
neverd diff --structural -a before.o -b after.o --reopen regions.nddiff --list region-anchors
neverd diff --structural -a before.o -b after.o --reopen regions.nddiff --region-anchor 0
```

Rare normalized instruction windows grow into exact shared runs, retaining scalar
changes outside those runs. A bipartite graph groups their function memberships.
One-to-many, many-to-one and many-to-many components retain alternative split,
merge, clone and redistribution explanations. Observed calls among participating
functions support inline/outline hypotheses; shared terminal runs and stated
module ownership support shared-tail and module-movement hypotheses. These are
inferences about transformations, not recovered source facts. Overlapping copies
remain ambiguous and do not replace the one-to-one mapping.

Coverage counts decoded non-transfer instructions in participating functions.
Calls, branches and returns are excluded from seed growth and remain visible in
the ordinary function delta. Memory effects, register renaming, scheduling,
source-level dataflow and cross-ISA semantics are not validated by this profile.
An unchanged shared run does not hide a changed constant elsewhere in the body.

Repeated runs shorter than the default eight instructions are omitted unless
they cover at least half the eligible instructions of either function. This
limits components formed from common short sequences; it is a heuristic
abstention, not evidence that a transformation is absent. Unique seeds retain
the ordinary minimum run length.

`regions` may be an object with `enabled`, `min_instructions`,
`min_repeated_instructions`, `max_occurrences`, `max_seeds`, `max_work`,
`max_anchors` and `max_hypotheses`. Work/storage admission
and defaults have one owner in `diff/Limits.h`. Frequent seed groups and oversized
components and weak candidates are counted as explicit abstentions. Exhausting a budget, cancellation
or deadline retains completed evidence and publishes the actual stop state.
No feature hash, score or region explanation is a semantic certificate.
The **All shared spans (including unclassified)** filter and `--list region-anchors`
also page anchors retained before grouping could finish. Use the saved report
when revisiting a partial result: anchor indices belong to that exact result.

## Dependency evidence and slices

`--dataflow` captures the existing pipeline's verified MedIR as detached SSA/CFG
observations. Enable **Settings → Dependency evidence** in the desktop, choose a
function allowance, compare, and open **Dependencies…**. The window pages both
inputs' function availability, operations, native instruction addresses, widths,
operands and gaps. **Trace inputs** and **Trace uses** request backward/forward
slices with optional control dependencies. Original-function navigation retains
the ordinary native delta alongside these observations.

```sh
neverd diff --structural -a before.o -b after.o --dataflow --save flow.nddiff
neverd diff --structural -a before.o -b after.o --list dataflow --limit 200
neverd diff --structural -a before.o -b after.o --flow-function 0x1000 --limit 200
neverd diff --structural -a before.o -b after.o --flow-function 0x1000 --flow-kind blocks
neverd diff --structural -a before.o -b after.o --flow-function 0x1000 --slice-node 12
neverd diff --structural -a before.o -b after.o --flow-function 0x1000 --slice-node 12 --slice-direction forward --slice-data-only
neverd diff --structural -a before.o -b after.o --reopen flow.nddiff --list dataflow
```

`--flow-side secondary` selects the second input. Node IDs and function entry
addresses come from that side's pages; an ID belongs to one saved graph. The
collection visits decoded bodies in address order, subject to the input filters.
The default allowance is 16 functions per input, 4096 native instructions per
function, 65536 SSA nodes per graph, and 262144 retained nodes per input. The
`dataflow` settings object accepts `enabled`, `profile`, `max_functions`,
`max_native_instructions`, `max_total_nodes`, and `graph`; `graph` accepts
`max_nodes`, `max_blocks`, `max_work`, and `rounds`. Limits have one owner in
`diff/Limits.h`. Collection summaries distinguish eligible, processed,
unprocessed, available, unsupported, failed and resource-limited functions.
Increasing one allowance does not remove the other bounds.

SSA capture uses the machine ABI preparation shared with native lifting and
verifies its final MedIR, without emitting HighIR or LLVM. Source presentation
hints do not supply its argument contracts. Existing bounded omitted-callee
summary recovery may inspect bodies outside the selected function.

The selected native pipeline invocation is synchronous. Stop/deadline checks
occur between functions and during graph projection/slicing; pipeline temporary
memory and work are not hard-limited by retained-node bounds. Native instruction
admission prevents starting an oversized decoded body, but this is not a process
RSS or real-time deadline guarantee. The GUI uses a separate worker and bounded
page caches. The last exact slice request is cached for paging; the C API accepts
`refresh:true` to recompute a stopped slice.

SSA names/register identities are not correspondence evidence. Scalar bits,
widths, address provenance, PHI predecessor edges and native origins are retained.
Synthetic operations have block provenance rather than an invented instruction.
The `med-ssa-v2` profile and dependency schema 2 also retain recovered call
arguments. Each call's `call_argument_begin`/`call_argument_count` identifies
their exact range in the input array, after raw operation inputs and before
preserved call-clobber inputs. An absent record (`call_argument_begin:null`)
remains an unknown input boundary; it differs from an observed zero-argument record. Stale or
duplicate call-record owners are refused. These records add caller-side data
dependencies, not callee/formal parameter or return bindings. Earlier development
schema-1 graphs require fresh extraction rather than silent reinterpretation.
Slices traverse explicit SSA edges and optional postdominator-derived control
dependencies on paths to observed exits. Aliases, opaque calls, exceptional flow,
unknown inputs and missing exits remain visible gaps. These are pipeline
observations with inferred calling conventions, not qualified machine-state or
whole-program equivalence. The internal finite-neighborhood fingerprints expose
open data frontiers; hash equality never marks code unchanged.

Saved dependency graphs stream into the shared document. Reopen reconstructs
the bounded native projection and checks the recipe, coverage, origins and every
dependency fact before publishing it. A saved stopped collection may retain a
verified prefix of fresh evidence. CLI and worker restart/reopen reuse saved
comparison settings; original input load/discovery/filter settings still need to
match. These dependency pages alone do not qualify compiler transformations or
cross-ISA profiles.

Choose **Settings → SSA representation → Optimized SSA** or pass
`--dataflow-profile=llvm-ssa-v1` to compare the ordinary machine pipeline's
optimized LLVM integer SSA. This can expose shared computations hidden by O0
stack spills and register moves. It uses the existing LLVM optimizer without
source-only rewrites and keeps the native delta available. `med-ssa-v2` remains
the default; existing reports retain their original profile and encoding.

```sh
neverd diff --structural -a unoptimized.o -b optimized.o --ignore-names \
  --flow-regions --dataflow-profile=llvm-ssa-v1 --save optimized.nddiff
```

The optimized profile initially admits scalar integers up to 64 bits, integer
operations, comparisons, casts, selects, PHIs, direct branches/switches/returns
and selected integer intrinsics (including funnel shifts). Exact bit widths,
flags, predicates, intrinsic immediates and argument/return/function attributes
remain correspondence evidence. Memory, ordinary calls, other types, native EH
contracts and unmodelled metadata produce a visible unavailable reason for that
function. Use Original SSA to inspect its retained machine dependencies. These
boundaries do not affect native comparison or source inspection.

Dependency schema 3 adds an exact `semantics` string to each optimized node and
an eighth type/attribute field to each operand. A sorted `origins` array retains
all direct emission observations as `{address, sequence}`. Optimized nodes keep
legacy `address:0x0` and `origin_sequence:-1`; no arbitrary representative is
chosen. No origins means no retained direct instruction attribution. Library
expression ancestry is excluded. Region node pages add `originals`, one decoded
instruction per distinct origin address, while `original` remains null. Native
origin thresholds count unique instruction addresses, not operation sequences.
Slices and GUI dependency pages retain these plural origins. Optimized regions
use `llvm-neighborhoods-v1`; saved profile/metadata/origin edits are checked
against a fresh projection, including empty and stopped collections.

**Optimized SSA with parameter views** (`--dataflow-profile=llvm-parameter-ssa-v1`)
also recognizes an i64 native parameter whose every actual use is an unflagged,
origin-free truncation to i32. It observes the same low 32 bits as an i32
parameter at the same argument position and recorded native register. Empty
argument attributes and an unambiguous, non-stack native register range are
required. High-bit uses, mixed widths, truncation flags, direct origins,
missing/overlapping sites or unusable source indices retain the raw parameter
and all its truncations. This is a read-only observation; no input or LLVM body
is rewritten. Exact function attributes, PHI edges and remaining native origins
are preserved.

Schema 4 records every source argument in `parameter_views`: `source_bits`,
`view_bits`, `attributes`, optional `native_site:{register,index}`, and
`elided_truncations`. Site values are hexadecimal bit strings; null means no
record, while register `0xffffffffffffffff` denotes a recorded stack site.
The MedIR index is local provenance, not a cross-build ABI ordinal, and is
checked on reopen without becoming a correspondence key. Metadata pages expose
`parameter_count` and `projected_parameter_count`; node/slice/region pages add
only the parameter records used by that node, with `input` and `parameter`
positions. Qt shows the original and observed widths on projected inputs.
Parameter rows and retained nodes share the node/storage allowance, including
`max_total_nodes`; public node counts and coverage still count operations only.
Elided nodes also remain subject to the original per-graph node/work budget.
Existing schemas and saved encodings are unchanged.

This profile uses `llvm-parameter-neighborhoods-v1` or, with shared fragments,
`llvm-parameter-fragments-v1`. Owned x64 ELF/COFF/Mach-O O0/O2 branch fixtures
exercise the added correspondence. A changed final constant is excluded from
matching; changed control beyond a finite neighborhood remains an open frontier,
and a deeper inspection checks that condition. These fixtures do not establish
general optimization coverage, equivalent behavior, or loop correspondence.

**Optimized SSA with loop analysis** (`--dataflow-profile=llvm-progress-ssa-v1`)
includes parameter views and independently derives return-progress contracts.
This lets supported O0 loop bodies correspond to computations in O2 unrolled
code while retaining every original function attribute. The source LLVM body
and machine ABI are unchanged. Only the detached observed contract gains
`mustprogress` and `willreturn`, and only after proof. LLVM's `willreturn` allows
undefined behavior or returning control; this does not promise a normal return
for invalid inputs or establish equivalent results.

The proof first admits finite scalar operations. It accepts acyclic control
flow and single-block counter loops with a direct PHI-to-update recurrence,
a signed or unsigned nowrap constraint, and any nonzero fixed integer step.
The branch must directly consume a scalar integer comparison of that update;
all integer predicates, either operand order and either exit polarity are
supported. The other operand need not be constant. A skipped exit value still
exhausts the nowrap domain, producing poison consumed by the branch; this
proves progress without promising a normal return. No iteration simulation,
potentially overflowing iteration count or fixed choice for `undef` is assumed.
Removing the proved self edges must leave the entire represented CFG acyclic. Wrapping-only
recurrences, zero or variable steps, frozen or reset counters, unrelated exit
conditions, multi-block and nested cycles remain **unproven**, with their exact source contracts and
available graphs. Unproven does not mean nonterminating.

Schema 5 adds `progress:{kind,source_contract,observed_contract,loop_count,loops}`.
Kinds are `unproven`, `acyclic` and `counter_loops`; each loop witness records
`block`, `counter`, `update` and `compare` IDs. Step, width, flags and exit value
remain exact in those nodes. Graph metadata pages omit the loop array; each
paged node includes only its block's `progress_loop` or null. The proof root
and witness rows share retained-item limits with nodes and parameter views.
All analysis and contract text work use the original projection budget and
cancellation mechanism. Structural reading rechecks finite operations and the
proof; fresh source binding additionally authenticates the attribute change.
Qt exposes the proof status, original/observed contracts and restored selection.

The recipes are `llvm-progress-neighborhoods-v1` and
`llvm-progress-fragments-v1`. Shared loop/unrolled computations retain open
control/data frontiers and ambiguity between repeated copies. Changed iteration
bounds, changed body arithmetic and native deltas remain visible. Progress is
not equality of loop counts, whole-function behavior or patch presence. All
previous profiles, schemas and saved encodings keep their existing behavior.

**Optimized SSA with multi-block loop analysis**
(`--dataflow-profile=llvm-progress-ssa-v2`) extends that proof to branched loop
bodies. Each cyclic component must have one entry header and exactly one
internal edge back to it. The latch directly tests the fixed nowrap update of
that header's counter PHI, computed in the header or latch; no internal edge may
supply a fresh counter seed.
Removing those proved backedges must leave the whole graph acyclic. Multiple
external seed edges into the same header, early exits and sequential loops are
supported. Nested cycles, extra backedges, irreducible entries and unreachable
cycles without an entry remain unproven. This adds evidence without guessing a
loop bound or changing native behavior.

Schema 6 adds `header` to each witness; `block` identifies the latch, while
`counter`, `update` and `compare` identify operations. Metadata remains paged:
only latch-block rows carry the witness, and the GUI shows its header, latch,
counter update and guard when such a row is selected. The recipes are
`llvm-progress-neighborhoods-v2` and `llvm-progress-fragments-v2`. All v1 proof
rules, work charges and schema 5 report bytes retain their existing contract.
The SCC traversal is iterative and linear in graph size, with the existing
work, storage and cancellation allowances. Both proof versions retain the
same finite-operation admission and source-binding requirements.

**Optimized SSA with nested loop analysis**
(`--dataflow-profile=llvm-progress-ssa-v3`) also proves eligible loops nested
inside other counter loops. The inner counter may receive a fresh seed on each
outer iteration. Each layer retains the unique-entry, single-backedge, fixed
nowrap update and direct consuming guard requirements. Header updates, early
exits and multiple sequential inner loops are supported. An unproved inner or
outer cycle leaves the whole function unproven; no partial proof changes its
source contract. Multiple backedges and irreducible entries remain outside this
proof family.

The analysis repeatedly checks residual components while retaining original
entry edges, then requires an acyclic residual graph. It uses the existing work
and cancellation allowance, without a fixed nesting-depth limit. Deep nesting
can consume quadratic work and may therefore report resource exhaustion.
Schema 7 exposes every layer's header, latch, counter, update and guard on its
latch-node rows; witnesses are ordered by latch rather than proof round. Saved
validation derives the proof again from the graph and authenticates it against
fresh source evidence. The recipes are `llvm-progress-neighborhoods-v3` and
`llvm-progress-fragments-v3`. All v1 and v2 profiles remain available with their
published contracts and saved encodings.

**Optimized SSA with branching loop analysis**
(`--dataflow-profile=llvm-progress-ssa-v4`) also handles several continuation
paths that advance the same header counter. Different fixed steps are allowed
when every path strictly advances in one common signed or unsigned nowrap
domain. An increasing path paired with a decreasing path, a reset, an unguarded
continuation, or incompatible signed/unsigned guarantees remains unproven.

The same proof handles compiler-generated unconditional latches: all incoming
paths must have a direct consuming guard, and the latch must feed the header
with either one common update or a PHI selecting each path's exact update.
Guards can test the current counter or their own update. Overflow poison may
cross one backedge before a current-counter guard observes it on the next
iteration; it cannot sustain defined infinite continuation. Nested components
still require a final acyclic residual graph before any progress is published.
Only the stated direct update/one-PHI forms are admitted. Irreducible entries,
duplicate predecessor edges and more complex recurrence expressions remain
unproven. Ordinary optimization is unchanged, so some compiler outputs may
lose the required nowrap evidence or acquire an unsupported control-flow shape.

Schema 8 distinguishes `loop_count`, `backedge_count`, and `guard_count`.
Witness `block` now identifies the consuming guard; `latch` identifies the
physical backedge source. Two guards may justify one backedge. `header`,
`counter`, `update`, and `compare` retain their exact graph identities. Node
pages expose witnesses on guard-block rows; all pages retain the authoritative
totals. The interface shows the selected guard and its actual latch separately.
Saved validation recomputes every count and witness before fresh source binding.
The recipes are `llvm-progress-neighborhoods-v4` and
`llvm-progress-fragments-v4`; published v1/v2/v3 bytes and behavior are retained.
All scans, grouping and ordering use the existing work/storage/cancellation
allowances, without a fixed limit on loop depth or the number of backedges.

**Optimized SSA with wrapping loop analysis**
(`--dataflow-profile=llvm-progress-ssa-v5`) also handles fixed-step counters
without an overflow prohibition. For example, an unsigned counter adding 3
eventually visits zero from every initial value. Adding 2 cannot reach zero
from an odd initial value, but it does reach the exit range `0..1` from every
initial value. The proof checks this reachability mathematically, without
executing or counting iterations.

Every recurrence arm must use the same nonzero modular step. Guards may compare
the current counter, its exact update, or one fixed add/sub translation against
a literal or another such observation of the same counter. This includes
compiler-generated wrap tests such as `counter + 4 < counter + 2`.
All possible continuation paths must share enough exit values to
cover every counter cycle. Different modular steps, incompatible exits, resets,
freezes in the recurrence and more complex observations remain unproven. The
existing monotone nowrap proof is preferred when available; it also admits a
single affine guard and can still use nonliteral bounds or different steps.
Unique-entry, complete-PHI and final residual-acyclicity requirements remain.
This does not prove an iteration count or equivalence between functions.

Schema 9 adds `proof: "nowrap"` or `proof: "modular"` to each witness. The
selected operation explains this basis alongside its guard, counter and latch.
Totals, paging, slicing and fresh-source report validation use the same core
model. Recipes are `llvm-progress-neighborhoods-v5` and
`llvm-progress-fragments-v5`. Existing profiles and report bytes stay supported.
Ordinary native optimization is unchanged: compiler transformations can still
produce unsupported shapes or require seed/bound relationships outside this
proof, which remain explicitly unproven.

**Optimized SSA with parameter-bound loop analysis**
(`--dataflow-profile=llvm-progress-ssa-v6`) also handles equality exits relative
to an input parameter. For example, repeatedly adding 3 to a 32-bit unsigned
counter eventually reaches any input target from any initial counter value.
Adding 2 can miss that target and remains unproven without additional evidence.
The analysis checks counter cycles mathematically; it does not run the loop.

Every continuation arm must use the same nonzero modular step. Its direct
equality/inequality guard compares the counter or one fixed add/sub observation
against the same parameter or one fixed add/sub translation of that parameter.
The analysis expresses all guards relative to that parameter and requires a
shared exit in every possible counter cycle. This also admits qualifying
even-step loops that continue only while equal. Different parameters, mixed
literal/parameter exit domains, changing bounds, reversed parameter subtraction
and freezes on the recurrence or guard path remain unproven. A fresh choice
between guarded paths is allowed when every path satisfies the common proof.
Nested-loop and complete-PHI checks still apply. Existing nowrap and fixed-bound
proofs take priority; no parameter attributes are invented.

Schema 10 adds the proof basis `parameter_modular`; it retains all counts,
witness identities, parameter provenance, paging and source authentication.
The interface explains that undefined behavior may occur before the exit: this
is a finite-execution contract, not a promise of normal return, an iteration
count or equivalence. Recipes are `llvm-progress-neighborhoods-v6` and
`llvm-progress-fragments-v6`. Earlier modes retain their exact contracts and
saved encodings. General invariant expressions and seed/bound relationships
remain outside this version's proof.

**Optimized SSA with expression-bound loop analysis**
(`--dataflow-profile=llvm-progress-ssa-v7`) also handles a shared bound computed
from inputs. For example, a counter advancing by 3 can reach
`(limit ^ (limit >> 3)) + (start & 255)` from any initial 32-bit counter value.
The expression may use supported integer arithmetic, masks, shifts, casts,
comparisons, selects and finite integer intrinsics. Several guards can use the
same expression with constant additions or subtractions; all must have a common
exit in every counter cycle.

The analysis traces the complete expression back to inputs and constants.
Changing PHIs, freezes and dependency cycles remain unproven, including a hidden
select arm. Independently computed expressions are not assumed equal merely
because they look alike. Prior proofs take priority, and every residual loop
must still be discharged. General invariant PHIs, frozen bounds and seed/bound
relationships remain outside this version's proof.

The interface explains the expression-relative proof and its undefined-behavior
limit beside the selected guard. Schema 11 records `invariant_modular`, with
recipes `llvm-progress-neighborhoods-v7` and `llvm-progress-fragments-v7`.
Paging, slicing and saved restore retain complete witness identities and source
provenance. Restore both rederives the proof and authenticates the expression
against the original binary. This remains a finite-execution contract, not a
normal-return guarantee or whole-function equivalence.


Reopening LLVM reports also tolerates changes in PHI numbering when every
operation, input, source observation and proof still matches exactly after an
authenticated node mapping. The saved IDs and report contents stay intact.
Different constants, origins, contracts or ordinary instruction order are not
accepted through this path; ambiguous unanchored PHIs remain a refusal.

Native-to-SSA change associations and call-effect contracts currently require
the original MedIR profile. Change review explicitly reports an unsupported
association profile for LLVM evidence while retaining native changes. Local
LLVM correspondences remain finite structural observations with open control
boundaries; they do not prove equivalence or patch presence. Extraction still
checks cancellation between bounded function jobs, not inside LLVM optimization.

## SSA region hypotheses

`--flow-regions` enables dependency extraction and `med-neighborhoods-v1` by default.
Selecting optimized SSA uses `llvm-neighborhoods-v1`. In the
desktop, enable **Settings → SSA region hypotheses**, compare, then choose
**Regions → SSA dependency regions**. The separate native instruction comparison
continues to show residual changes and uncovered code.

```sh
neverd diff --structural -a before.o -b after.o --flow-regions --save ssa.nddiff
neverd diff --structural -a before.o -b after.o --reopen ssa.nddiff --list flow-regions
neverd diff --structural -a before.o -b after.o --reopen ssa.nddiff --flow-region flow-region-ID
neverd diff --structural -a before.o -b after.o --reopen ssa.nddiff --flow-region-anchor 0 --limit 200
neverd diff --structural -a before.o -b after.o --reopen ssa.nddiff --flow-region-anchor 0 --flow-region-coordinates phi_blocks
neverd diff --structural -a before.o -b after.o --reopen ssa.nddiff --list flow-region-anchors
```

Finite fingerprints nominate native-origin expression roots. Exact checks then
preserve node sharing, operation/operand widths, scalar bits, address provenance
and PHI incoming branch-arm context within the admitted depth. Distinct tied
definitions abstain. Candidates must contain at least four verified nodes and
two distinct original instruction addresses by default. Overlapping anchors keep
their node pairs and original byte records; their group coverage counts each
node once. Different counterparts for the same node add a clone alternative.
Function topology and observed native calls can suggest split, merge,
redistribution, inline, outline or module movement. These explanations are not
accepted function mappings, calibrated probabilities, semantic validation or
patch-presence findings. Control beyond PHI context remains open in every anchor.

`flow_regions` accepts `enabled`, `fragments`, `min_nodes`, `min_origins`, `max_occurrences`,
`max_seeds`, `max_work`, `max_anchors` and `max_node_pairs`. Defaults admit eight
occurrences of a seed per side, 262144 indexed roots per side, 67108864 work
units, 20000 anchors and 1048576 retained node/PHI-block pairs. Dependency
collection and graph limits also apply. Repeated seeds, weak or ambiguous
candidates, limited graphs and oversized groups have separate counters. Scope
flags describe whether extraction and fingerprinting covered the decoded
function inventory; they do not assert complete semantic coverage. A work stop
keeps completed anchors even when no group was published. The raw-anchor view
can inspect them. Save before revisiting a partial result, since anchor indices
belong to that result.

**Shared SSA fragments** is an explicit optional mode (`--flow-fragments`, or
`flow_regions:{"fragments":true}`). It enables dependency/region extraction and
uses `med-fragments-v1` or `llvm-fragments-v1`. The desktop setting has the same
name; the region view shows the recipe, depth, matched operations and external
inputs. A depth of one verifies one operation and its immediate constants,
parameter identities and types. Larger depths include more producer operations,
up to the graph's configured rounds. PHIs, branches, switches, memory/call effects
and unknown operands are excluded inside the fragment. Return-value observations
can terminate a fragment. Excluded producers can
remain outside it: this can retain an identical return computation after an O0
PHI becomes an O2 select. Exact function attributes and instruction semantics
are still required; the mode does not remove inconvenient contracts.

Each fragment records separate `nodes` and `frontier` pairs, `depth` and
`frontier_count`. Both sets share one consistent definition mapping. Frontier
producers have **not** been compared and contribute neither matched-node counts,
native-origin admission nor coverage. Inspect them with **External inputs** or
`--flow-region-anchor 0 --flow-region-coordinates frontier`; pages report
`boundary_only:true` and retain original instructions. Combined conflicting
bindings mark a hypothesis ambiguous without inventing extra clone evidence.
All depth nominations consume the seed allowance; all retained node and frontier
pairs consume the pair allowance. Deeper candidates are preferred. The existing
four-node/two-origin defaults apply. Loops remain open even when every represented
node has a binding. This local correspondence does not establish equal return
values, branch behavior or patch presence. Legacy recipes and report fields are
unchanged when the mode is off.

Reopen recomputes the regions from freshly bound dependency observations.
Completed/resource-limited records must reproduce their mappings and coverage;
cancelled/deadline records may retain only a verified completed prefix.
Work counters are bounded diagnostics and may differ on recomputation. SSA
numbering and source addresses are coordinates, not correspondence features.
The current profile requires the same admitted ISA, ABI and normalization and
has not been qualified across compiler versions, optimization/obfuscation
families or cross-ISA builds.

## Call paths and possible impact

`native-call-paths-v1` traces observed native callers or callees from one or more
function entries. It uses admitted call relations, including tail transfers and
resolved import slots, and needs no SSA extraction. Select a function in the GUI
and choose **Call paths…** to inspect functions, observed program entries/exports,
one shortest witness per function, all traversed call sites, unresolved scope and
known edges beyond the depth/function allowance. Site rows retain original bytes
and navigate to the caller. An absence of callers never establishes an entry.

```sh
neverd diff --structural -a before.o -b after.o --impact-function 0x401000
neverd diff --structural -a before.o -b after.o --impact-function 0x401000,0x402000 --impact-side secondary
neverd diff --structural -a before.o -b after.o --impact-function 0x401000 --impact-direction callees --impact-kind sites
neverd diff --structural -a before.o -b after.o --impact-function 0x401000 --impact-kind path --impact-witness 1
neverd diff --structural -a before.o -b after.o --impact-function 0x401000 --impact-depth 0 --impact-kind frontier
```

`--impact-kind` also accepts `entrypoints` and `boundaries`; default `functions`
is in breadth-first discovery order. `--impact-witness` selects a global function
row, independent of the current page/filter. `--offset`/`--limit` page every view.
Witness ties use stable reverse native instruction order; recursive and repeated
call sites remain in the full site view without enumerating every possible path.
Default bounds are depth 32, 4096 reached functions, 262144 indexed call/boundary
sites and 16777216 work units. `--impact-max-functions`, `--impact-max-sites`,
`--impact-max-work` and `--timeout-ms` can adjust them within core ceilings.
Indexing is limited to 1048576 native functions. A resource stop retains completed
observations and reports partial indexing/traversal; depth/function omissions
also have explicit frontier sites. Cancellation is cooperative.

The result is **possible call reachability**, not proven interprocedural data
impact. Path feasibility, parameter/return bindings, aliases and external callers
are not modeled. The global boundary inventory includes unresolved transfers
outside the reached set, so missing indirect callers are never treated as proof
that a change is isolated. Native coverage, discovery, exception and external-body
gaps remain visible. This operation establishes neither patch presence nor
vulnerability elimination and performs no semantic validation.

`neverd_comparison_impact_json` and worker `comparison_impact` share the same
query: `side`, `seeds` (address strings), `direction`, `kind`, optional `witness`,
`max_depth`, `max_functions`, `max_sites`, `max_work`, `timeout_ms`, `offset`,
`limit`, `refresh`. One exact analysis is cached across ordinary page kinds. A refresh
recomputes a stopped result. Every page binds its input hash/revision and query;
saved comparison reopening freshly reconstructs the native evidence, then the
query can be repeated. Call-path queries are not stored as analyst confirmations.

### Qualified import-call contracts

In **Call paths…**, choose **Call contracts in reached functions**. Select a row
and open **Selected call** to inspect its original instruction, SSA node,
argument roles, assumptions and remaining boundaries. **Dependency evidence**
must have been enabled when comparing to qualify contracts; without it, calls
remain listed with explicit missing-SSA status. Double-click a site to inspect
the original caller.

```sh
neverd diff --structural -a before.so -b after.so --dataflow \
  --impact-function 0x401000 --impact-direction callees --impact-kind effects
```

`native-import-effects-v1` inspects every indexed transfer in a reached caller,
including unresolved sites and known targets beyond the traversal frontier.
Only a bodiless native import with a unique original-instruction SSA call and
recovered arguments can receive a contract. Safety's shared exact-name, arity,
format and ABI rules own the contract and argument roles. Internal functions,
similar names, missing or ambiguous SSA and unsupported signatures stay explicit.
For example, a copy contract can identify the length argument without knowing
destination capacity. Fallible release remains distinct from successful release.
The stated assumption is that the import resolves to its catalog contract.

These are applicability-checked call contracts, not observed executions or
security findings. Parameter/return propagation, aliases, must-release, capacity,
authorization, path feasibility, patch presence and semantic validation remain
unproved. Qualifying a contract never clears SSA or native coverage gaps.
The current real-binary fixture covers Linux x64 ELF imports and PLT transfers;
authored ABI/arity cases additionally test refusal and fallible-release metadata.
This does not qualify whole-program lifetime or cross-format security analysis.

The shared impact API/worker `kind:"effects"` adds `max_effect_records` and
`max_effect_work`; CLI flags are `--effect-max-records` and `--effect-max-work`.
Defaults are 4096 records and 16777216 review work units, with ceilings of 65536
and 1073741824. Native indexing keeps its independent impact budgets. One query
deadline covers fresh native and review work, with cooperative polling; graph
validation and output serialization are bounded, not hard real-time deadlines.
The effect query has its own cache recipe; pages reuse that result and refresh
retries stopped work. Inspection completeness is separate from native scope.
Reopen rederives contracts from freshly bound native/SSA facts; it does not trust
saved effect verdicts. The output records content/revision, SSA recipe, profile,
contract authority and assumptions.

## Local change review

Select an accepted function pair and choose **Change review…**. The ranked table
keeps original instructions and bytes, local SSA operations, dependency context
and association evidence. Select an input to trace a retained node's inputs or
uses, inspect its original function, or open its native call paths. Filtering and
paging reuse a bounded cached calculation; **Refresh review** explicitly retries
stopped work. SSA is optional: enable **Dependency evidence** before comparing to
collect it; missing or unsupported graphs retain native change rows.

```sh
neverd diff --structural -a before.o -b after.o --changes-function 0x401000
neverd diff --structural -a before.o -b after.o --dataflow \
  --changes-function 0x401000 --changes-context predicate_dependency --limit 200
```

The `native-ssa-changes-v1` profile observes literal bits, local operand/result
widths, operation changes within compatible families, signed/unsigned integer
order or extension changes, and recovered call-argument counts. It uses the
accepted native alignment, including unchanged instructions whose SSA operands
changed after rewriting or constant propagation. Compatible SSA families must be
unique at both native origins. A unique direct synthetic arithmetic operand can
also be associated through that native anchor. Such records explicitly carry
`association:"direct_synthetic_operand"`, both anchor node IDs and the retained
SSA nodes. A synthetic address never becomes an original instruction address.
Multiple node operands, PHIs, deeper synthetic expressions and incompatible
families are not recursively paired. Ambiguous native families abstain and are
counted. Native changed rows remain visible when no finer fact is available.

Contexts are syntactic dependencies to predicates, returns, recovered call
arguments or stores. They do not establish feasible paths, buffer capacity,
authorization, error handling, lifetime effects or external callee behavior.
Call target/clobber operands are excluded from local argument comparison; a
missing argument record differs from a known empty record. Known address
provenance is excluded from literal comparisons. Graph-wide uncertainty stays
separate from individual local observations. An empty filtered page or completed
calculation does not prove unchanged behavior or absence of a vulnerability.

Priority is a review heuristic: base 10, literal 40, width/return 60, call argument
70, signedness/memory write 80 and predicate 90; the highest applicable value wins.
It is neither probability nor severity. Responses explicitly report
`semantic_validation:"not_performed"`, `security_inference:"not_performed"` and
`fix_presence:"not_assessed"`. Matching evidence and analyst confirmation remain
separate from these observations.

The query accepts `side`, `address`, `context`, `max_rows`, `max_observations`,
`max_work`, `timeout_ms`, `refresh`, `offset` and `limit`. Contexts are `all`,
`predicate_dependency`, `return_dependency`, `call_argument_dependency` and
`memory_write_dependency`. CLI selectors use `--changes-side`, `--changes-context`,
`--changes-max-rows`, `--changes-max-observations` and `--changes-max-work`, with the
shared `--timeout-ms`, `--offset` and `--limit`. Defaults admit 65536 native rows,
4096 observations and 16777216 units of indexing/local work; maxima are 1048576,
65536 and 1073741824. Shared native alignment has its own admission allowance.
One cooperative cancellation/deadline covers delta acquisition and observation
work. Bounded output sorting and page serialization are not hard deadlines.
Stop state, visited rows, work, per-side SSA availability/indexing and graph gaps
remain visible. CLI stopped review returns partial exit code 3.

The cache binds the accepted pair and query recipe to immutable snapshots and
retained SSA recipes. Context/page selectors reuse that cache; mapping edits,
undo or restore invalidate it. Derived review rows are recomputed after reopening
instead of trusted from a saved report. Broader security-effect classification and engine patch-presence assessment
remain future work. Manual assessments use the independent history below.

## Bounded scalar return validation

Select an accepted pair and choose **Validate returns…**, or request:

```sh
neverd diff --structural -a before.so -b after.so \
  --validate-function 0x1000 --timeout-ms 5000
```

`--validation-side primary|secondary` selects the address's input. The optional
`--validation-max-instructions` bounds each selected native body (default 2,048,
ceiling 16,384); `--validation-max-work` controls the shared scalar proof budget
(default 16,777,216, ceiling 1,073,741,824). These flags require
`--validate-function`. Model construction, projection, control partitions,
block visits and symbolic nodes have separate finite limits listed in each
report. A result other than `proved` gives partial-analysis exit code 3;
ordinary comparison coverage and change exit rules still apply to proved models.

The `lifted-scalar-returns-v1` profile requires x64 native inputs with the same
ABI, complete strict machine-ABI lifts, equal integer return widths and identical
ordered GPR parameter locations/widths. It checks all **defined integer input
bitvectors**. This is an explicit precondition; no `noundef` annotation is added
to the source IR. Native calls, unknown transfers, exception handlers, memory/pointer
models and unsupported contracts are refused. Different recovered input widths
remain unsupported even when both native functions return the same width.
Ordinary unwind-only records are admitted only when complete, bound to the
entire selected native body, and identical to the emitter's canonical projection
of that lift's normalized source record. Records with a personality, LSDA,
handler or incomplete parse remain unsupported. The original unwind information
and IR stay intact; this admission does not prove native unwind behavior.

| Status | Meaning |
| --- | --- |
| `proved` | Equal integer returns and defined termination in the supplied lifted models over the complete admitted input domain |
| `unsupported` | A binding, operation, effect or contract lies outside this profile |
| `unproved` | The bounded algebra did not establish equality or definedness; **not** evidence of inequivalence |
| `budget_exceeded` | A finite model/proof allowance prevented completion |
| `cancelled` | A cooperative stop was observed; `state` distinguishes cancellation from timeout |

A proved result assumes lift correctness. It does not prove full native register
state, flags, memory, faults, call effects, vulnerability elimination or fix
presence. Native input hashes/revisions, original entries, verified lift counts,
LLVM content digests, argument bindings, work and partitions remain visible in
the report. It is an inspection result, not a signed or persistent certificate.
The UI shows assumptions beside the outcome and clears results when input or
mapping revisions change. Time, native-instruction and proof-work controls allow
bounded retries; changing any limit also clears the old result. **Check again**
always starts a fresh query.

CLI and GUI validate against the comparison's retained input sources. Only the
selected pair is freshly lifted and proved, with NoOpt and scaffold mem2reg;
there is no proof cache. The report says `input_source_binding:owned_sessions`,
with `inputs_bound:true` and `inputs_revalidated:false`. Reopening builds a fresh
capture. The worker also checks whether the source paths have changed and asks
for a new comparison if they have. Direct API clients inspecting an owned capture
continue to inspect its originally loaded bytes even if those paths are replaced.

The C API offers two lifetime contracts, with the same query fields `side`,
`address`, `max_native_instructions`, `max_work`, `timeout_ms`:

- `neverd_comparison_run` borrows live input sessions. Its
  `neverd_comparison_scalar_validation_json(comparison, primary, secondary, query)`
  revalidates both complete native revisions on every query, without recapturing
  optional SSA. The report identifies `revalidated_sessions`.
- `neverd_comparison_run_owned(comparison, &primary, &secondary, …)` takes two
  distinct sessions in distinct slots. Ownership admission nulls both slots;
  the comparison then owns them, including if analysis subsequently fails.
  Invalid ownership arguments or a reused comparison leave the slots unchanged.
  Transferred handles must never be used or destroyed by the caller. With both
  native captures complete, `neverd_comparison_owned_scalar_validation_json`
  uses these exclusive sources and avoids full input decoding per query.

The shared scalar prover owns definedness, arithmetic and complete finite control
domains. Neither input code nor its LLVM body is modified or executed. Comparison
destruction releases transferred sessions; ordinary snapshot queries can still
outlive borrowed sessions. No proof verdict is stored in comparison files or
trusted on reopen. Cancellation covers live rebinding and model/proof work;
a native lift, LLVM copy/verification or serialization step can delay it, so
`timeout_ms` is a cooperative deadline rather than a hard wall-clock bound.

## Source inspection

The **Pseudocode** tab loads the selected functions from the comparison's
retained inputs. Accepted pairs default to **Align source lines**, with original
line numbers, `+` / `−` / `~` change markers, highlighting and synchronized
vertical scrolling. The selector pages aligned rows; the gutter and selected-line
status retain each input's original source coordinates. Turn alignment off for
independent pages and source-line selection. C and LLVM C use the same emitters as ordinary
decompilation; Input language, C++, Rust and Go retain their existing dialect
admission and unavailable reasons. Unmatched functions can be inspected too.
Selecting a mapped line lists its original instruction anchors; **Show instruction**
opens the native listing at that anchor. Unmapped and synthetic lines remain
explicit. Source line numbers do not imply correspondence between the panes.

```sh
neverd diff --structural -a before.o -b after.o \
  --source-function 0x1000 --source-representation c --limit 200
neverd diff --structural -a before.o -b after.o \
  --source-function 0x2000 --source-side secondary \
  --source-representation llvmc --offset 200 --limit 200
```

`neverd_comparison_source_page_json` accepts `side`, `address`, `representation`,
`offset`, `limit`, `max_native_instructions`, `timeout_ms` and `refresh`.
It requires complete native captures from `run_owned`; it never reopens a path.
The versioned `neverd.bindiff.source.page` response identifies the input revision,
content hash, recipe, availability and actual stop state. Its `document` preserves
the ordinary source page, including prelude metadata, dialect, text and line
records. Each row adds `comparison_anchors` checked against the captured native
function. A completed native detail query also supplies `listing_row` and
`native_change`; the source query does not force alignment work.

Pages contain up to 2048 lines. The additional admission allowance defaults to
4096 native instructions; `--source-max-instructions 0`, or **Options → Remove
instruction allowance**, removes that extra allowance while retaining the
ordinary pipeline's limits. Cancellation and deadlines are checked around
function analysis, rendering and anchor projection; an analysis step can delay
cancellation. Stopped requests discard their source page. Reloading retries the
view; mapping changes clear old navigation. The worker rejects stale source
paths and obsolete comparison revisions, as for other interactive reads.

Source inspection does not change saved native evidence and is not persisted as
an equivalence result. Independent pages retain `source_alignment:not_performed`;
`semantic_validation:not_performed` and `provenance_complete:false` apply to both
views. Equal source spelling does not establish equivalent behavior.

```sh
neverd diff --structural -a before.o -b after.o \
  --source-function 0x1000 --source-difference --limit 200
```

`neverd_comparison_source_difference_json` and the optional worker operation
`comparison_source_difference` collect both complete selected-function renderings
before aligning exact text lines. They never align independently selected page
windows. The `neverd.bindiff.source.difference` version 1 response has original
line records and checked native anchors on each side of `rows`, plus a textual
`kind`: `unchanged`, `changed`, `inserted` or `deleted`. A missing side is null.
Changed rows pair adjacent removals and additions for display; this does not
establish statement or instruction correspondence. Exact line LCS uses a
bidirectional shortest-edit-path search with linear scratch space. Sparse edits
avoid a full line-by-line Cartesian search. Ties are deterministic; repeated text
can have other valid pairings. Recipe `sdk-source-difference-v2` records this
search and its work accounting; the response schema remains version 1.

Queries accept the source-page fields plus `max_work`, `max_source_bytes` and
`include_prelude`. Declarations are excluded by default; enable **Include
declarations in alignment** or `--source-include-prelude` to compare whole
emissions. One retained pair serves subsequent pages, keyed by function pair,
representation, scope and calculation settings. Mapping edits retire the cache.
Stopped results retain no source text or classified rows; `refresh` or **Reload
source** retries them. Independent source pages remain available.

Default alignment allowances are 50,000,000 charged work units and 64 MiB of
source text plus serialized row evidence across both inputs. These are additional
admission/accounting allowances, not process RSS limits or hard deadlines. Use
`--source-max-work 0` / `--source-max-bytes 0`, the matching JSON fields, or
**Options** to remove them. Cancellation is checked during collection and line
alignment and around synchronous analysis/rendering. Allowance and stop outcomes
remain explicit; they never manufacture unchanged rows.

## Analyst cases and assessment history

**Analyst review…** opens a paged case history for the selected function, or the
whole comparison when no function is selected. An accepted pair can receive an
explicit case identifier, author, assessment, reference and note. **Append
assessment** adds an event; it never edits earlier statements. **Save comparison…**
uses the comparison's existing Save/export flow. Appending in memory alone does
not write a report to disk.

Assessments include investigating, relevant/unrelated change, candidate fix,
patch present/absent/partial/reverted and inconclusive. These are manual statements,
including statuses that mention patches. They do not change computed deltas,
matching, confidence, observation ranking or engine fix-presence/validation status.
Authors are self-described; NeverD does not authenticate them. A reference is
stored as text and is not fetched or executed.

Each event binds to the exact content and analysis identities and both function
entries. It has a sequence, previous-event digest and the previous event for its
case/function pair. These digest links detect inconsistent content and order;
they are not signatures or independent proof of authorship. The latest event and
whether its original pair is currently accepted are distinct fields. Unmatching,
remapping and mapping undo retain the complete history. Revising a judgment means
appending a new event, not rewriting or undoing an old one. Draft text survives
navigation and changed inputs, but cannot silently move to another input/pair;
**New draft for selected pair** explicitly starts that draft.

```sh
neverd diff --structural -a before.o -b after.o --reopen review.nddiff --list reviews
neverd diff --structural -a before.o -b after.o --reopen review.nddiff \
  --review-record record.json --save reviewed.nddiff --list reviews
neverd diff --structural -a before.o -b after.o --reopen reviewed.nddiff \
  --list reviews --review-case local-case-42 --review-latest
```

A record file has `primary`/`secondary` hexadecimal function entries, `case`,
`author`, `note`, optional `reference`, `assessment` (the names above with
underscores) and required `expected_head`. Use the `head` returned by the history
query; the first event uses an empty string. A stale head fails without appending.
The CLI requires `--save` when recording, so successful annotations are preserved.
`--review-function` and optional `--review-side` filter history by function;
`--offset` and `--limit` use the shared page limits. History is newest first.

A journal admits 4096 events and 4 MiB of combined user text. Per-event UTF-8 limits
are 256 bytes each for case and author, 16384 for note and 2048 for reference.
Required fields cannot be blank. Notes allow newline/tab; unsupported control
characters and invalid UTF-8 fail. Each input's semantic identity strings must
also be valid UTF-8 and total at most 4096 bytes before canonical digest encoding.
Schema 3 persists this journal independently
of rederived native/SSA facts. Schema 2 reports are read with no assessment history;
a version-2 document claiming a journal is rejected. Input changes, inconsistent
links/digests, missing functions and unsupported proof/authentication claims fail
before replacing the current comparison.

## Interpretation and persistence

Saved matches must agree with the final ordered mapping decisions. An effective
`unmatch` cannot coexist with the same accepted pair, including in a historical
cancelled, timed-out or resource-limited report. A later `match` of that pair
clears its rejection; `confirm` does not. Later remapping may replace earlier
matches, so historical match entries are not all required to remain active.
Contradictory reports fail before replacing the current comparison, preserving
its edits, undo/redo and inspection state.

Scores describe heuristic structural correspondence. They do not prove semantic
equivalence, vulnerability removal or patch presence. Identical normalized code
with incomplete decoding is never reported as normalized unchanged. Ordinary
scalar immediates remain meaningful even when their value happens to equal an
image address. Only decoded control-flow operands, exact relocation fields and
known address references receive address normalization.

Instruction alignment uses the same weighted objective for small and large
paired basic blocks: equal normalized instructions score 4, a shared skeleton
with changed constants scores 3, and a shared mnemonic scores 2. Large blocks
use exact divide-and-conquer alignment with linear score frontiers and bounded
dense tables; crossing the table threshold no longer switches to a greedy
approximation. Equal ends and provably optimal constant-change diagonals avoid
quadratic work for those cases. General changed interiors can still require
quadratic time; cancellation and query deadlines remain available. An optimal
alignment is structural correspondence, and repeated instructions may admit
several equally scoring alignments.

The approximate function matcher honors the requested `max_candidates` allowance
when selecting candidates for exact measurement; it no longer silently clamps
that allowance to four. Raising it can recover candidates whose sketch score is
lower than structurally misleading alternatives. Candidate generation remains
bounded, and a larger allowance does not guarantee that every true pair enters
the candidate set. The ambiguity margin and one-to-one pairing rules still apply.
A zero ambiguity margin admits any positive score lead; it does not break exact
ties by function address or candidate order.

Call-position context uses only anchors shared by every longest alignment of
already corresponding callees. Repeated calls with competing anchor positions
remain uncertain. Between these anchors, equally sized intervals may propose
new pairs when all call targets are known and unmatched. All occurrences must
propose a consistent one-to-one relationship before the similarity threshold
can accept a pair. Unresolved calls retain their positions and prevent this
positional inference across their interval; they are not invented call-graph
edges. Other matching features can still resolve those functions.

There is no 1,024-call cutoff. Large call sequences retain exact anchor selection
with linear score frontiers and bounded dense tables; identical known-call
sequences have a linear fast path. General differing sequences can still take
quadratic time and honor comparison cancellation/deadlines. Call positions are
heuristic structural evidence, not a proof of behavioral correspondence.

Matcher version 4 identifies the call-context and exact-tie corrections; version
3 introduced the instruction alignment and candidate-allowance changes above.
Fresh binding recomputes saved measurements: older reports with agreeing
measurements can reopen with a version warning, while obsolete scores or tallies
are refused without replacing the current result. Reopening preserves the
saved pair choices; run a new comparison to obtain current matching decisions.
Use the offline report workflow to inspect preserved old evidence.

Duplicate names and indistinguishable bodies remain explicit ambiguities.
Address-generated labels do not anchor matches, even after native function
discovery. Name evidence retains its producer origin: a real symbol or export
named `sub_1234` remains eligible, as do names admitted by recognition. Display
renames do not replace those names or change native snapshot revisions. Numeric
suffixes added to distinguish equal names in the listing do not manufacture
unique name evidence; literal suffixes in the original symbols remain intact. The
`sdk-native-decode-v2` extraction identity records this contract; historical
reports keep their original evidence and require a new comparison for fresh
binding under the revised extraction profile.
Use the [offline report workflow](binary-diff-lineage.md#open-a-report-from-an-older-analysis-profile)
to inspect those reports without reanalyzing their binaries.
Unmatched functions are not automatically additions/deletions when discovery is
partial. Imports, thunks, libraries and incomplete bodies carry separate traits.
Pairs with no body on either side report `identity_only`, with no body similarity
claim, and do not count as normalized unchanged. Pairing a body with a bodiless
symbol exposes the observed structural differences and incomplete coverage.
The GUI Evidence tab shows the matching step, scores, change categories, analyst
confirmation and stable identities; change text accompanies semantic colors.

The versioned `neverd.bindiff` JSON document stores both input hashes, format,
ISA, bitness, endian, ABI, image base, entry point, normalization/analysis/content
revisions, settings, inventories, pairs, ambiguity groups, coverage, timings and
overrides, plus requested region evidence. Function pairs use entry addresses.
Region spans use coordinates in the exact revision-bound snapshots; reopening
checks their bounds, normalized instructions and derived hypothesis fields.
Positive unique-seed claims are checked against both complete seed indexes;
saved hypothesis membership must agree with the connected anchor components.
Schema version 2 introduced identity-only pairs; version 3 adds analyst history. Reopening recomputes each
measured function delta under the saved correspondence and rejects inconsistent
changes, body coverage, scores or instruction/block/edge tallies. Analyst pairing
and confirmation decisions remain distinct from those decoded facts.
Reopening requires the original input content and extraction settings. A stale
identity or reversed direction is rejected without replacing the current result.
Save uses atomic replacement. A saved document retains inventories and mapping
evidence; it does not embed both binaries or their full decoded snapshots.

## Shared API and worker

`NeverDCAPIDiff.h` provides an opaque comparison handle. `run` captures two
borrowed loaded sessions; callers retain ownership and must grant exclusive
access for that call. Its snapshots outlive the sessions. `run_owned` instead
transfers both sessions through handle slots that it clears on admission, keeping
the loaded inputs for fresh scalar validation. Use `status_json` concurrently for progress and `cancel` to request a stop. Destroy
the handle only after its running call and other concurrent queries have joined.
All returned strings use `neverd_free_string`.

Use `summary_json` for bounded overview metadata, `functions_json` for filtered,
sorted inventory pages, and `function_page_json` for aligned instruction pages.
`regions_json` pages hypotheses, a selected hypothesis's anchors (`region_id`),
or every retained anchor (`all_anchors:true`). `region_page_json` pages one
global anchor index's original instructions; optional `region_id` checks its
membership. `flow_regions_json` shares hypothesis/anchor selectors and, with
`anchor`, pages paired SSA nodes plus original bytes; `coordinates:"phi_blocks"`
selects the verified PHI context pairs. Fragment anchors additionally accept
`coordinates:"frontier"` for uncompared input producers; legacy anchors reject
that selector. `graph_json` accepts
explicit node/edge allowances. Full export and legacy full
function detail are intended for batch use; interactive consumers should page.

Function pages and CFG queries also accept `timeout_ms` and `refresh`. The shared
delta calculation cooperates with cancellation and reports `state` separately
from `computation_complete`. A stopped delta retains no fabricated unchanged
rows; a direct CFG query keeps native topology with `not_compared` annotations.
The exact stopped query stays cached until a refresh or a different query.
The GUI clears stopped comparison graphs and offers **Retry difference**.
Full legacy detail uses the same delta cache and an instruction allowance;
its JSON serialization and the worker's bounded graph layout are not hard
deadline guarantees. CLI full-detail cancellation returns partial exit code 3.

The Qt-free worker advertises these operations only when the linked engine has
the required additive C ABI:

- `comparison_start`: `primary` and `secondary` each contain `path` and optional
  `load_options`; accepts comparison `options` and optional `restore_path`.
- `comparison_summary`, `comparison_functions`, `comparison_function_page` and
  `comparison_function`: overview, inventories, paged and full details.
- `comparison_cfg_summary`, `comparison_cfg_viewport`, `comparison_cfg_focus`:
  deterministic shared layout, bounded viewport pages and counterpart navigation.
- `comparison_regions`, `comparison_region_page`: hypothesis/anchor pages and
  paired native instruction spans; requires requested region analysis.
- `comparison_dataflow`, `comparison_slice`: bounded SSA function/node/block
  pages and dependency slices; requires requested dependency extraction.
- `comparison_flow_regions`: verified SSA hypothesis, anchor and node/PHI-context
  pages; requires requested SSA region analysis.
- `comparison_impact`: observed caller/callee witnesses, entries and global
  unresolved scope; independent of SSA extraction and cancellable.
- `comparison_observations`: ranked local native/SSA changes, original-code
  anchors and scope; optional retained SSA, bounded paging and cancellation.
- `comparison_scalar_validation`: fresh input binding, audited native lifting
  and bounded conditional scalar return validation, with cooperative stop.
- `comparison_reviews`: paged manual history, latest/current-pair binding and
  the expected head for optimistic updates.
- `comparison_review_record`: append an explicit assessment; changes the
  comparison revision and preserves independent mapping undo history.
- `comparison_override`, `comparison_undo`, `comparison_redo`: mapping edits.
- `comparison_save`/`comparison_restore`: a local `path`;
  `comparison_export`: the shared document in a bounded response.
- `comparison_validate`, `comparison_close`: re-read/check inputs or release them.

Responses carry `comparison_revision`, separate from the main project's
revision. Clients may send `expected_comparison_revision` to reject obsolete
reads or edits. Input file stamp changes invalidate interactive review; explicit
validation and reopening re-read content and verify its identity. Large exports
should use `comparison_save` instead of a transport frame.

Saved-report restoration is transactional and cooperatively cancellable through
`neverd_comparison_restore_json` and the additive
`neverd_comparison_restore_json_with_progress`. Progress identifies reading,
parsing, evidence validation and measurement stages. On a stop or invalid report,
the previous result, analyst edits, undo/redo and inspection caches remain
available. Successful restoration replaces them only after validation completes.
SDK status separates `restore_state` from the saved computation's `state` and
reports `previous_result_retained`; opening an old cancelled, timed-out or
resource-limited report can therefore complete successfully. The saved
`timeout_ms` is historical and does not impose a restore deadline.

The worker advertises standalone `comparison_restore` cancellation only when
the linked engine provides the new progress entry point. Older engines retain
the synchronous restore path. A successful restore/reopen reply includes
`restore_completed:true`; a late cancellation cannot undo its commit or leave
the revision unchanged. A cancelled `comparison_start` replacement is discarded
before the existing session is swapped. Qt preserves the existing selection and
review on a stopped restore and allows retry. CLI SIGINT during `--reopen`
returns exit code 3 without publishing stdout or overwriting a `--save` target.

File I/O checks bounded chunks, record readers and validators check their loops,
and parallel measurement workers propagate a typed stop after joining. SDK
restoration constructs one JSON DOM. LLVM JSON parsing, the CLI/worker settings
parse, individual allocation/copy and full graph equality remain indivisible
checkpoints; cancellation is cooperative, without a hard latency guarantee or
unsafe thread termination. Report schemas, evidence profiles and allowances are
unchanged.

The standard worker `cancel` operation acknowledges the request first. Only the
terminal comparison reply reports computation stopped and whether a partial
result was retained. The worker advertises `comparison_cancellable_operations`;
the GUI uses that list for queued/running analysis requests and Cancel state.
Input loading, native discovery, decoding, feature extraction and delta
measurement cooperate with cancellation. `neverd_session_load_cancellable`
returns 1 for a completed load, 0 for failure and -2 for an observed stop before
publication. Stopped preparation preserves the previous session's image,
analysis and edits. Worker replacement during either input's loading preserves
the old comparison and its revision, without publishing a new partial result.
The GUI displays readable per-side loading stages, formats byte counts in the
user's locale, keeps Cancel pending until the terminal acknowledgement and
allows another attempt. A stopped replacement leaves the old review available.
Native byte hashing and copying check bounded batches without capping file size;
format parsing, load-time scans and PDB progress share the loading control. Discovery checks its metadata, architecture scan, verification
and overlap walks, joins its workers and publishes no partial candidate inventory.
Stopped extraction reports `stopped_phase:"discovery"` or `"decode"`; retry starts
fresh discovery, and closing a session stops its owned discovery job. The additive
`neverd_session_discover_functions_cancellable` returns -2 for cancellation,
separately from -1 for failure and nonnegative completed function counts.
File I/O, allocations, individual metadata helpers and sorting can still delay
a stop. Cancellation remains cooperative, with no hard deadline. Session
publication and sidecar restoration after final preparation finish normally if
a stop arrives after the final checkpoint; old engines lacking the additive load
API finish loading before observing a comparison stop.

## Current boundaries and verification

Owned x64 and AArch64 pipeline fixtures exercise SSA projection, native origins,
entry live-ins and deep constant dependencies. Public comparison admission
supports the same-ISA profiles described above; these fixtures do not qualify a
cross-ISA comparison profile. SDK coverage exercises dependency lifetime after
closing inputs, slices, resource refusals and saved-fact tamper rejection.
An owned unsigned-expression regression also retains regions across Clang/GCC
optimization levels and Windows COFF, with names disabled. A changed final
constant remains visible, saved evidence rebinds, and incompatible ABIs fail.
This is one small source family; broad compiler/optimization qualification and
LTO/PGO/obfuscation coverage remain outstanding.

The GUI uses bounded function/instruction page caches and worker-owned CFG
viewport indexes. Its graph viewport reports when a display allowance is reached.
Comparison-owned inputs also provide paged source inspection with checked native
anchors and exact textual line alignment. Semantic source correspondence, full
reference provenance and broader profile qualification remain work in progress.

Native runs and verified finite SSA neighborhoods are parts of the required
transformation work. Broader CFG/IR region matching, compiler/O-level/LTO/PGO/obfuscation
qualification, cross-ISA profiles, security-fix triage, interprocedural data effects,
broader semantic validation beyond the pure scalar-return profile, release-aware
lineage beyond the offline snapshot graph, native/game package
adapters and competitive qualification remain outstanding. No comparative
performance superiority is established.
The [advanced acceptance map](binary-diff-acceptance.md) tracks the required
end-state areas separately from individual completed increments.
Observed native caller/callee reachability is available as review context;
parameter/return/alias propagation remains separate unfinished work.

Focused targets are `NeverDBinaryDiffTests`, `NeverDBinaryDiffSDKTests`,
`neverd-worker`, `neverd`, and `neverd-gui-comparison-tests`. The
`NeverDBinaryDiff` CTest label includes CLI, real worker and GUI integration
checks. SDK/core suites have their respective test labels. Shared traversal
changes also require the session discovery and worker/CLI instruction-flow tests.
Set `NEVERD_DIFF_SCREENSHOTS` while running the GUI test to retain real light/dark
Qt captures; run separately with `QT_SCALE_FACTOR=1` and `1.5` for DPI review.

## Multi-version review

The [offline version graph](binary-diff-lineage.md) combines retained comparisons,
traces function associations across branches and cycles, exposes conflicting
paths and keeps manual assessments attached to their original evidence. CLI,
C ABI, Python, worker and desktop share this model and exact source persistence.
This first profile makes no chronology or automatic patch-presence inference.

Restoring a saved comparison through the worker reads its settings under the
saved-file byte allowance, not the smaller backend-reply allowance. Reports
larger than 32 MiB remain local during both restore and start-with-restore;
interactive responses keep their ordinary bounded pages.
