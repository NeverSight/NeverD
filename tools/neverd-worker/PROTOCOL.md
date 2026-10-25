# NeverD worker protocol 1.1

`neverd-worker` is a local child process, owns a native C ABI Session and an
independent optional web session, and has no Qt
or LLVM linkage of its own. The GUI must launch the bundled worker by explicit
path, continuously drain stdout and stderr, and validate `protocol_major` before
opening a binary. It does not listen on a network port or execute target code.

## Transport and lifecycle

Each message is a **4-byte unsigned big-endian byte length**, followed by that
many bytes of UTF-8 JSON. Length is 1 through 8 MiB. Input nesting is capped at
64. Decoded duplicate object keys are rejected. Empty, truncated and oversized frames terminate the connection with a
`fatal` message; malformed JSON produces an error response and preserves framing.
stdout is a protocol-only channel. Before Session creation, the worker duplicates
the output handle and redirects ordinary stdout to stderr, including C stdio,
C++ streams, and embedded Python output. Engine dynamic-library initializers run
before `main` and must remain quiet, as with the existing linked CLI boundary.

The first unsolicited message is:

```json
{"type":"hello","protocol_major":1,"protocol_minor":1,"engine_version":"...","project_schema":1,"revision":"0","project_id":"","max_frame_bytes":8388608,"capabilities":["metadata","functions","disasm","bytes","decompile","cfg","xrefs","strings","analyze","resolve","annotations","annotation_set","rename","save","reload","segments","read_only","heartbeat","cancel"],"storage":"existing_json_sidecars","web":{"schema_version":1,"status":"error","error":{"code":"capability_unavailable"}}}
```

`project_schema:1` refers to this sidecar adapter and versioned edit history, not
the proposed SQLite project format. No analysis cache is persisted. Edit history
is bound to the input SHA-256 supplied by the public dashboard C API. The engine
version must match the shipped engine. `hello` is also
accepted as a request and returns this object in its response payload.
The example shows an engine without web support. When available, `web` contains
the actual web C API capability response plus `operation_prefix:"web_"` and
`payload_schema_version:1`. A worker linked to an older engine remains usable
for native requests; it resolves the optional web API only from the already
loaded engine library.

Requests:

```json
{"protocol_major":1,"request_id":"42","operation":"disasm","project_id":"project-1","expected_revision":"1","payload":{"address":"0xffff800012340000","limit":128}}
```

`request_id` is a nonempty string (up to 128 bytes), unique among pending
requests. `project_id` and `expected_revision` are optional; when supplied they
must match at execution time. An absent payload means `{}`. An empty project ID
does not constrain the project. A stale request returns `stale_project` or
`stale_revision` and performs no action. Clients should refresh after a stale
response and should not attach one old expected revision to a batch containing
an analysis or edit that changes revision.

Responses:

```json
{"type":"response","protocol_major":1,"request_id":"42","operation":"disasm","project_id":"project-1","revision":"1","status":"ok","payload":{"items":[],"next_address":null,"complete":true}}
```

Statuses are `ok`, `error`, `cancelled`, and `budget_exceeded`. Error responses
include `error:{code,message}`. While a request loads an image, the worker may
first send responses with the same `request_id` and `status:"progress"`, whose
payload is `{phase,done,total,detail}`; they are not the request's result, and
a client keeps waiting for one of the final statuses. Display messages are diagnostics; use stable
`code` values for localization. A successful empty page differs from errors,
unsupported representations and analysis failures. Every VA is a `0x`-prefixed
lowercase hexadecimal string, including VAs above 2^53. Revisions and project IDs are opaque
strings. CFG node/edge IDs are strings, not addresses or floating-point values.
Byte offsets inside a requested page and bounded counts are JSON integers.

Responses from the Session executor also include
`analysis_state:"not_analyzed"|"complete"`, even when the requested view is
unsupported after successful analysis. This lets clients refresh recovered
function lists once when a lazy query completes analysis. Ordinary edit
revisions do not imply a new analysis. Administrative replies may omit this
additive field.

Native executor replies also carry the published `background` object and
`background_sequence`, a decimal uint64 string that increases on every state
publication within one worker process. Heartbeats carry the same sequence.
A client must validate request correlation and project/revision before using a
reply's state, and apply it after installing the reply (opening resets the
client's session). Discard older or duplicate sequences, including a heartbeat
written after a newer reply. Cached responses must not republish their snapshots.
Reset the accepted sequence on session/transport retirement. Older workers can
omit these additive fields; once a sequenced snapshot has been accepted, an
unsequenced heartbeat cannot supersede it. Administrative and web replies do
not publish native executor snapshots.

Session calls run serially on an execution thread; a separate input loop admits
requests and cancellation while the synchronous engine runs. The queue holds at
most 32 waiting requests and 16 MiB of request bodies; excess receives
`queue_full`. Replies and heartbeat only expose published revisions. The current
adapter serializes every engine query; it does not claim concurrent immutable
engine snapshots. Query results are bounded before IPC, and models must still
bound their own cached pages.

Every second the worker sends
`{type:"heartbeat",protocol_major:1,project_id,revision,active_request_id:string|null,cancellation_requested:bool}`.
The same heartbeat envelope is also sent immediately when background function
discovery completes, including a failed discovery whose diagnostic is published
as `background.state:"unavailable"`. `background.discovery_complete` states
that the SDK discovery result has been consumed; it does not promise that the
derived listing or reference index has been rebuilt. The executor admits
requests for up to its existing 150 ms idle grace at that transition, waking
as soon as one arrives, before continuing derived background work.
Stepped SDK discovery is opt-in through `open.step_discovery:true`; it defaults
to false so older clients do not acquire an extra image snapshot. Only clients
that coordinate large-image replica startup with `discovery_complete` should
enable it. SDKs without the optional polling export retain synchronous discovery.
Heartbeat is liveness, not percent progress. `cancel` takes
`{request_id:"target"}`. A queued target returns `cancelled` and the cancel reply
has `{accepted:true,stopped:true,state:"stopped",requires_restart:false}`.
For an active synchronous call it has
`{accepted:true,stopped:false,state:"pending",requires_restart:true}`. The active
call continues until the C ABI returns or the parent terminates the process.
On natural completion its original truthful status and committed result are
preserved, with `cancellation_requested:true`, `calculation_stopped:true`, and
`completed_before_cancellation:true` (the computation completed before a
cooperative cancellation could stop it). Never show “stopped” from the initial
cancel acknowledgement. `shutdown` cancels queued requests, acknowledges
`{stopped,requires_restart}`, waits for an active call, and exits. EOF follows the
same drain policy. A parent can terminate and restart an unresponsive worker.

## Native binary comparison

`comparison_regions` and `comparison_region_page` serve bounded transformation
hypothesis, anchor and original instruction pages when `options.regions` was
requested. They preserve the accepted function mapping and expose their own
analysis state and abstention counts. The same evidence is saved in `.nddiff`.
`comparison_regions` accepts `all_anchors:true` to inspect unclassified anchors
from a partial analysis. `comparison_region_page` accepts the global `anchor`
index, with optional `region_id` to require membership in a hypothesis.

When the linked engine exports the comparison C ABI, `hello.capabilities` includes
`comparison_start` and the related operations documented in
[Structural binary comparison](../../docs/binary-diff.md#shared-api-and-worker).
The worker holds independent comparison inputs and a published snapshot; neither
start nor mapping edits replace the native project's Session or revision.
Comparison replies carry `comparison_revision`, and the payload's optional
`expected_comparison_revision` rejects obsolete reads and edits. Failed starts
preserve the previous comparison. File stamp changes invalidate interactive reads;
explicit validation/reopening also re-reads and checks content identity.
`comparison_start.options.profile` defaults to `native-same-isa-v1`; the SDK
selects qualified x86/x64 or 64-bit AArch64 inputs with matching ISA and ABI.
Explicit `native-x86-v1` and `native-aarch64-v1` restrict admission. The worker
does not infer a profile from filenames. AArch64 Mach-O subtype/ABI bits remain
part of input identity. This structural profile does not extend the separate
x86 scalar-validation or import-effect qualification.

`comparison_source_page` is advertised when the SDK exposes the optional source
page API. It accepts `side`, `address`, `representation` (`c`, `llvmc`, `source`,
`cpp`, `rust`, `go`), `offset`, `limit` (1–2048), `max_native_instructions`
(default 4096; zero removes this extra allowance), `timeout_ms` and `refresh`.
It returns `neverd.bindiff.source.page` version 1, with input revision/hash,
availability, stop state and the ordinary source page in `document`. Rows add
`comparison_anchors` bound to the captured native function. Existing completed
native alignment supplies optional `listing_row` and `native_change` navigation;
source-to-source alignment and semantic validation are not performed. Synthetic
or unmapped lines stay unmapped. The page uses the comparison's owned sources,
requires current input stamps and accepts `expected_comparison_revision`.
Cancellation discards the page, but function analysis and rendering are
indivisible steps; `hard_deadline` is false. Failed or unsupported source views
leave the native comparison intact. Older SDKs retain ordinary comparison
operations without advertising this capability.

`comparison_source_difference` is independently advertised when its optional SDK
entry point is present. It accepts the source-page fields plus `max_work`,
`max_source_bytes` and `include_prelude`. It requires an accepted function pair,
collects both complete renderings, and pages a cached exact textual alignment.
The `neverd.bindiff.source.difference` version 1 result exposes availability,
computation state, input identities, source coordinates, original anchors and
textual row kinds. Missing sides are null. Adjacent insertions/deletions may be
paired as `changed` for display; they do not prove statement correspondence.
Default additional allowances are 50,000,000 work units and 64 MiB of text plus
serialized row evidence across both inputs; zero removes the selected allowance.
Declarations are excluded unless `include_prelude:true`. Stop/limit results have
no classified rows and remain cached until refresh or changed options. Mapping
edits invalidate the cache. The operation uses the same input-stamp and expected
revision checks and cooperative cancellation contract as source inspection.

`comparison_dataflow` pages retained SSA collections (`side`, `kind` of
`functions`, `nodes` or `blocks`, `address` for a graph, `offset`, `limit`).
`comparison_slice` accepts `side`, `address`, `seeds` (node IDs), `direction`
(`backward`/`forward`), `control`, `timeout_ms`, `offset`, `limit`, and optional
`refresh`. Both require `dataflow:true` or a bounded settings object at start.
The settings object accepts `profile:"med-ssa-v2"` (default) or
`profile:"llvm-ssa-v1"` (optimized machine integer SSA), or
`profile:"llvm-parameter-ssa-v1"` (eligible native low-bit parameter views), or
`profile:"llvm-progress-ssa-v1"` (parameter views and independently proved
return progress). Schema 5 adds `progress` with `kind`, `source_contract`,
`observed_contract` and `loop_count`; full reports also contain `loops` witnesses
with block/counter/update/compare IDs. Metadata pages omit that array and node
rows include only their block's `progress_loop` or null. Proof rows share retained
storage limits. Unproved loops remain available with their original attributes;
progress does not establish equal iteration counts or results. Fresh restore
binds the original contract and proof to the actual source projection.

`profile:"llvm-progress-ssa-v2"` also proves single-entry multi-block loop
components. Schema 6 adds a `header` block ID to witnesses; `block` is the latch.
Only latch-node rows carry that bounded witness. The recipes use
`llvm-progress-neighborhoods-v2` / `llvm-progress-fragments-v2`. Header PHIs must
receive the exact update from the latch edge with no internal reset. The update
is computed in the header or latch; after cutting proved backedges the complete
CFG must be acyclic. Nested/irreducible or bypass cycles remain unproven.
Published v1 proofs and schema 5 encodings are unchanged.

`profile:"llvm-progress-ssa-v3"` extends the same proof to nested components.
Each layer must meet the single-entry/backedge recurrence rules, with original
entry edges retained during residual analysis. Every residual cycle must be
discharged before any progress is published. Schema 7 witnesses use the same
explicit header/latch/counter/update/compare fields and remain sorted by latch;
paged node rows expose only their own latch witness. Recipes use
`llvm-progress-neighborhoods-v3` / `llvm-progress-fragments-v3`. Repeated SCC
analysis and witness ordering share the existing work/cancellation limits;
there is no separate depth cap. v1/v2 contracts and encodings are unchanged.

`profile:"llvm-progress-ssa-v4"` proves groups of guarded continuations of a
common counter, including several direct backedges and compiler-merged latches.
Every update must advance in a common signed/unsigned nowrap direction; every
recurrence arm and guard must qualify before the group is admitted. An
unconditional latch can carry one common update or one PHI selecting the exact
guarded updates. Residual cycles still leave all progress unproven.
Schema 8 publishes distinct `loop_count`, `backedge_count`, and `guard_count`.
Witness `block` is the guard's block; new `latch` is the physical backedge source.
Node rows expose their own guard witness, sorted by guard block. Totals are
complete even on page size one and are independently recomputed on restore.
Recipes use `llvm-progress-neighborhoods-v4` / `llvm-progress-fragments-v4`.
The shared proof owner retains existing resource/cancellation allowances and
all published v1/v2/v3 contracts. Complex, irreducible, duplicate-edge or
missing-nowrap recurrences remain explicit unproven cases.

`profile:"llvm-progress-ssa-v5"` adds fixed modular counter cycles and one
affine guard observation. The preferred nowrap proof retains its common-domain
rules. Modular fallback requires identical nonzero width-normalized steps,
literal bounds or two affine observations of the same counter, and common
exits covering every residue class of the step.
All residual cycles still require proof; no iteration limit or native argument
attributes are invented. Schema 9 retains the three counts and witness IDs,
adding `proof:"nowrap"` or `proof:"modular"`. Each guard-block node page exposes
its own witness; replay recomputes the canonical proof kind before fresh source
authentication. Recipes are `llvm-progress-neighborhoods-v5` and
`llvm-progress-fragments-v5`. All v1-v4 contracts and encodings remain exact.

`profile:"llvm-progress-ssa-v6"` also admits shared parameter-relative EQ/NE
exit guards. Every recurrence arm uses the same nonzero modular delta; every
guard compares an affine counter observation with the same parameter, directly
or as P+K, K+P or P-K. The core reuses common-exit residue coverage in coordinates
relative to that parameter. Mixed absolute/relative domains, different parameter
identities and freeze/reset barriers refuse. Schema 10 accepts the additional
canonical `proof:"parameter_modular"`, with unchanged witness fields and counts.
The preferred nowrap and v5 modular bases remain distinct. This establishes
finite execution or undefined behavior, not an iteration count or normal return.
Recipes are `llvm-progress-neighborhoods-v6` and `llvm-progress-fragments-v6`.
Earlier schemas never accept the new marker. Replay independently rederives
the proof and binds the original parameter identities to fresh native evidence.

`profile:"llvm-progress-ssa-v7"` additionally admits shared pure input-derived
expression bounds. Every dependency must end in parameters, constants or typed
undef/poison leaves; PHIs, freeze and cyclic definitions remain barriers.
Supported arithmetic, casts, selects and finite integer intrinsics reuse the
core finite-operation model. Guards must share the same basis node, optionally
with affine constant offsets, and cover every modular counter cycle. Legacy
proofs for every candidate are attempted before allocating the expression cache.
Schema 11 adds canonical `proof:"invariant_modular"`; earlier schemas reject it.
Recipes are `llvm-progress-neighborhoods-v7` and `llvm-progress-fragments-v7`.
Pages and slices expose the same witness fields/counts. Restore rederives the
basis and authenticates the full expression against fresh native SSA, even if
a changed expression would still prove progress. This proves finite execution
or undefined behavior, not normal return or whole-function equivalence.

Fresh binding can authenticate a complete same-block PHI renumbering while
retaining saved node IDs. Ordinary instruction order, source observations,
ordered inputs and all proof metadata must still agree exactly; this does not
weaken edited-report rejection.

Schema 4 retains source/view bits, argument attributes, optional native site
and elided truncation counts. Parameter rows consume the shared retained-item
allowance; public node counts and coverage still count operations. Metadata
pages expose parameter counts, and each node page exposes only its referenced
parameter rows with input and parameter positions. Reopen binds all source
records to fresh extraction, including snapshot-local MedIR indices.
LLVM dependency schema 3
adds typed `semantics` and plural `origins:[{address,sequence}]`; operands have an
eighth exact type/attribute field. Its region profile is `llvm-neighborhoods-v1`.
Optimized region nodes return `originals` for every distinct native address and
leave singular `original` null. Unsupported effects/types/metadata remain
unavailable with reasons. Med-only change associations report
`availability:"unsupported_profile"` explicitly. Saved settings select the same
profile on fresh reopen, including early-cancelled reports.
Unavailable records, graph origins, effect gaps and actual slice stop states are
retained. Neither operation validates behavior or vulnerability elimination.
`comparison_restore` is included in `comparison_cancellable_operations` only
when the linked SDK exports `neverd_comparison_restore_json_with_progress`.
Its `restore:*` progress covers chunked file reads, JSON parsing, validation and
measurement. A stop/failure retains the previous result and edit history without
advancing `comparison_revision`. Older engines keep synchronous restoration and
do not advertise cooperative cancellation for that operation. Reading settings
and the JSON DOM parser are indivisible checkpoints, not hard deadlines.

Successful standalone restoration and `comparison_start` with `restore_path`
return `restore_completed:true`. The payload's `state` still describes the saved
computation; a historical `cancelled` state is not a stop of the new restore.
`job.restore_state` describes the latter, and SDK status exposes
`previous_result_retained` for an admitted restore. A successful standalone
commit advances the revision even if cancellation arrives afterward; the
terminal reply then reports `completed_before_cancellation:true`. A replacement
handle remains disposable until the old session is swapped. Saved timeout
settings do not become a deadline for restore validation.

Restart with `restore_path` reuses the saved comparison settings before fresh
projection. Input load/discovery/filter settings must still match the snapshot.

`comparison_flow_regions` requires `flow_regions:true` or a bounded settings
object at start (implicitly requests dependency extraction when `dataflow` is
omitted). Query fields are `offset`, `limit`, optional `kind` relationship filter,
`region_id` to page that hypothesis's anchors, or `all_anchors:true` for every
retained anchor. With a global `anchor` index it returns paired nodes and original
instruction records; `coordinates:"phi_blocks"` pages verified PHI context pairs.
An optional `region_id` checks anchor membership. Conflicting selectors fail.
Replies expose scope, abstention/limit counters and open dependency boundaries.
These finite correspondences neither certify behavior nor hide native deltas.
The optional start setting `flow_regions:{"fragments":true}` selects
`med-fragments-v1` or `llvm-fragments-v1`. Anchors expose `depth` and
`frontier_count`; `coordinates:"frontier"` pages their external definition
bindings with `view:"frontier"` and `boundary_only:true`. Producers in those
rows have not been compared. They share a consistent mapping with internal
nodes but are excluded from matched-node counts, origin admission and coverage.
Original bytes remain inspectable. Legacy anchors reject frontier coordinates.
The mode defaults off; saved settings retain it even on early cancellation.

`comparison_impact` accepts `side`, nonempty address-string `seeds`, `direction`
(`callers`/`callees`), `kind` (`functions`/`entrypoints`/`sites`/`boundaries`/
`frontier`/`path`/`effects`), optional `witness` for path rows, `max_depth`,
`max_functions`, `max_sites`, `max_work`, `timeout_ms`, `offset`, `limit` and
`refresh`. It independently indexes native admitted call/tail/import relations
and retains shortest witnesses and global unresolved scope. One exact query
shares its cached result across page kinds. Input hash, native and comparison
revisions bind every page. These are possible call paths; data effects, aliases,
parameter/return binding, external callers and path feasibility remain unmodeled.
No SSA opt-in is required for paths. Stale input/revision checks still apply.
The `effects` page has an independent cache recipe and accepts
`max_effect_records`/`max_effect_work`. It joins each indexed transfer in a
reached caller to an exact native import and unique SSA call, then uses Safety's
applicability-checked contract. Missing SSA, unknown/ambiguous calls and unsupported
signatures remain rows. Contract facets, zero-based argument roles, original
bytes/node IDs, fallible release and SSA gaps accompany the evidence. This requires
`dataflow` at comparison start to qualify contracts. `effect_review` records
inspection state/work/scope and the catalog-resolution assumption independently
of native traversal coverage. It does not propagate data across calls, prove
successful release or assess security. Existing cooperative cancellation covers
fresh native and effect stages under one deadline; refresh retries a stopped query.

`comparison_observations` accepts `side`, accepted function `address`, `context`,
`max_rows`, `max_observations`, `max_work`, `timeout_ms`, `refresh`, `offset` and
`limit`. It returns the shared `neverd.bindiff.observations` schema, original
instruction anchors, local SSA facts, heuristic priorities and explicit scope.
SSA extraction is optional; missing or ambiguous evidence retains native rows.
Direct synthetic operands remain labeled synthetic and bind through native
anchor nodes. Context/page changes reuse one cached calculation; mapping edits
or restore invalidate it. It reports no security inference or fix assessment.

`comparison_reviews` pages independently recorded analyst history using `case`,
optional `side`/`address`, `latest`, `offset` and `limit`. It returns newest-first
records, global `head`, current pair binding and an explicit manual/unvalidated
source. `comparison_review_record` accepts `primary`, `secondary`, `case`,
`author`, `note`, optional `reference`, `assessment` and required `expected_head`.
The current pair must be accepted; stale heads fail atomically. Successful append
returns the updated summary and advances `comparison_revision`, without changing
matching or its undo stack. Save/export persists the schema-3 journal. These
bounded text operations do not claim cooperative cancellation or authentication.

For an active operation in the advertised `comparison_cancellable_operations`,
including `comparison_observations` and `comparison_scalar_validation`,
`cancel` returns `requires_restart:false` and
`stopped:false` while its cooperative request is pending. Only the target's
terminal reply reports `calculation_stopped:true`. An interrupted comparison has
`completed_before_cancellation:false`, `partial_result_retained:true` and payload
`state:"cancelled"`. Cancellation before or during input loading instead fails with
`error.code:"cancelled"` and no new partial result. The previous comparison and
its mapping revision remain usable. Current engines forward load stages as
`primary:load:<stage>` / `secondary:load:<stage>` (including `read`, `hash`,
`copy`, `parse`, `load-discovery`, `debug`, `ready`); `done`/`total` belong to that
stage, not to the entire comparison. Older engines without the additive
cancellable-load entry point finish their synchronous load before stopping. The payload's `job` object
separates `cancel_requested`, `computation_stopped` and
`partial_result_retained`. Loading and native discovery cooperate with the stop
probe; file I/O, allocations and individual metadata/parser helpers can still
delay acknowledgement. There is no hard wall-clock deadline.
The preceding legacy cancellation contract still applies to other operations.

`comparison_scalar_validation` accepts `side`, an accepted function `address`,
optional `max_native_instructions`, `max_work` and `timeout_ms`. It retains no
proof cache: every request performs fresh selected-function machine-ABI lifting
and checks the bounded `lifted-scalar-returns-v1` domain. Initial `comparison_start`
transfers its private input sessions to the C API comparison, which owns them
until replacement/close. The owned query avoids complete input decoding on each
request; it reports `input_source_binding:owned_sessions`, `inputs_bound` and
`inputs_revalidated:false`. Existing source-path change checks still reject stale
worker requests. Fresh reopen captures the inputs again. `status` is `proved`,
`unsupported`, `unproved`, `budget_exceeded` or
`cancelled`; `state` independently distinguishes cancellation, timeout and
resource limits. `proved` covers only lifted integer returns and defined
termination for defined input bitvectors at matching GPR locations. It admits
only absent exception records or complete source-bound canonical unwind-only
metadata; personality routines, handlers and incomplete records are unsupported.
This does not establish native unwind behavior. It assumes
lift correctness and proves neither complete native state nor security;
`unproved` is not an inequivalence verdict. Reports contain input revisions,
native entries, lift audits/model hashes, argument bindings and finite limits.
Saved comparisons never contain a trusted proof. Cancellation/deadline polling
covers model/proof work; a bounded native pipeline job and
LLVM copy/verification steps can delay observation of the stop.

Interactive clients use `comparison_summary`, `comparison_functions` and
`comparison_function_page`; `comparison_export` is subject to transport limits,
so large results should be written with `comparison_save`. CFG summary/viewport
operations use the existing layout and bounded spatial indexes, with optional
node/edge comparison evidence. A `comparison_cfg_focus` request identifies a
block by its start address and returns its geometry for counterpart navigation.

## Operations

Manual PDB loading uses `pdb_inspect` with `{path}` first. It returns
`{path,stamp,verified,name_count,supports_unverified_analysis}` without changing
the project. `pdb_load` takes `{path,stamp,mode}`: `verified` requires a matching
PE identity; `analysis` records explicit user confirmation to use the PDB for
analysis when that identity is missing. Types, signatures and symbol ranges
use the same supported debug-context path as a matching PDB. Machine, section
layout and any present identity still must agree. Older `display` selections
retain their display-only behavior. A successful load changes the revision,
invalidates old analysis and returns the mode, verification status, name count
and function count. Analysis replicas require the owner's mode and file stamp;
a changed PDB is rejected rather than silently reused. `open` takes a saved
selection as `pdb`: a `verified` one is checked again by its identity, an
`analysis` one by its stamp, and one that no longer loads leaves the file open
without it, with a warning.

`pdb_reference {path}` needs no open binary: it returns the SDK's
`neverd_pdb_reference_json` for the input, `{pdb:null}` or
`{pdb:{file,key,recorded,found,rejected,servers}}` — the PDB the PE names, the
first local copy whose build identity matches (`{path,location}` with location
`recorded`, `beside`, `directory` or `store`), the copies of other builds, and
`[{url,path}]`, each symbol server of the search path with the store path it
keeps the PDB at. The GUI asks before an `open` and passes the answer as `pdb`:
a `{path,mode:"verified"}` selection, or `{mode:"none"}`, which opens the PE
without the PDB it names and is saved with the project like a selection. With a
`pdb` value the open searches for no PDB itself, so the selection is the only
one that loads. `pdb_reference` is listed in `capabilities` when the engine has
it.

`local_types {filter?,origin?,language?}`, `local_type {name,spelling?}`
and `local_types_source {language?,spelling?,offsets?,hexOffsets?,asserts?,
types?,dependencies?,prototypes?}` read the open program's local types
through the SDK's `neverd_local_types_json`, `neverd_local_type_json` and
`neverd_local_types_source`: the list with import statistics, counts by
origin and the header libraries joined (`headers`), one type laid out member
by member, and `{language,source}`, the types rendered as C, C++, Rust or Go.
The engine reads them from the program's PDB the first time it is asked, and
joins the system headers NeverD ships for the image, such as the Windows
kernel's. A name the program has no type for, or the PDB's types failing to read,
is a `local_types_failed` error. The three are listed in `capabilities` when
the engine has them.

`local_types_fit {text?,records?,shifts?,limit?,spelling?,accesses?,
function?,address?,parameter?}` ranks the program's structures and unions by
how the code's accesses through a pointer land on their members, through the
SDK's `neverd_local_types_fit_json`: the accesses `[{offset,size,use}]` the
request lists, and with `address` and `function` those the instruction at
`address` makes through its memory operand's pointer and every other access
of the function through the same pointer, or with `parameter` and
`function` those the function makes through its parameter of that position
(as `function_type` lists it), from the register it arrives in.  `text` matches type names and their
typedefs as an editor's quick open does, `Type.member` and `.member` member
paths too.  It returns the SDK's `{accesses,target?,candidates,total}`; an
instruction that reaches no memory through a pointer is a
`local_types_failed` error.  It is listed in `capabilities` with
`operand_struct` when the engine has it.

`local_types_declare {source,language?}` and `local_types_forget {names}` are
writer commands: the first reads C, C++, Rust or Go declarations into the
program's local types through `neverd_local_types_declare` and returns
`{types,prototypes,problems}`, each problem with its `line` and `column`, and
nothing changed when there is one; the second removes the named types through
`neverd_local_types_forget` and returns `{removed,usedBy}`. Each change is
kept at once in the project folder's `types.json`, which the engine reads again with
the binary, so an analysis replica and the next session see the same types.
Each is a step of undo history, kind `types`: the file lists the declarations
in the order made, a removal as `{forget}`, and undo takes the last back off
the file and the program's types. Like a rename, both refuse staged comments
(`unsaved_changes`), and a change the file cannot take is undone and the
command fails. A `types.json` changed outside the worker is a foreign edit,
as the other tables are. Both are listed in `capabilities` when the engine has
them.

`type_libraries` returns the type libraries NeverD ships, as IDA's type
libraries window lists them, through `neverd_type_libraries_json`:
`{libraries:[{name,dataModel,recordABI,compatible,image,joined,types?,
prototypes?,conflicts?,error?}],added,removed}`. `image` says whether the
program's kind and architecture join a library, `compatible` whether its
records lay out as the program's, and `joined` whether its types join the
program's now: the image's but those `removed`, then those `added`, in that
order, each where no type before it declares the name, so order decides which
library gives a name two declare. The libraries joined are listed first, in
the order they join, then the others. `type_libraries_set {added?,removed?}` is a
writer command that sets the choice whole, each a list of library names: an
added library must be compatible and none of the image's, a removed one the
image's. The worker writes it to the project folder's `libraries.json` under
the writer lock it holds, and the engine reads that again with the binary, so
an analysis replica and the next session join the same libraries; it is no
step of undo history, as a debug file's choice is none. The program's types are read anew and the analysis starts
over. `project_sync` reads a `libraries.json` changed outside the worker and
says so as `type_libraries`. Both are listed in `capabilities` when the
engine has them.

`dwarf_reference {path}` needs no open binary either: it returns the SDK's
`neverd_dwarf_reference_json`, `{dwarf:null}` or `{dwarf:{path,location}}` --
the file that holds an ELF or Mach-O image's DWARF, with location `input` (the
image's own sections), `dsym` (a .dSYM bundle beside a Mach-O image),
`build-id` (the file a debug directory keeps under the image's build ID),
`debuglink` (the file the image's .gnu_debuglink names, of the CRC it records)
or `beside` (a split .debug file beside the image, matched by name), the first
in the order the open reads them, which is GDB's. A PE, an image without DWARF
and an SBF program answer null. The GUI asks before an `open`, as IDA asks
"Load DWARF debug information?", and passes a declined answer as
`dwarf:{mode:"none"}`: the open reads no DWARF, though other debug information
still loads, and the answer is saved with the project and kept by read-only
replicas. `open` and `metadata` return it as `dwarf`, null when the DWARF was
read, and what the load read as `debug_info:{enabled,kind,path,files}`, kind
`dwarf`, `pdb`, `map` or `none`, and `files` every file read, the PDB or DWARF
first: `[{kind,path,names,dialect?,match?,functions?}]`. `dwarf_reference` is
listed in `capabilities` when the engine has `neverd_dwarf_reference_json` and
`neverd_session_set_dwarf_search`.

`dwarf_load {path,stamp?}` reads the DWARF of the open ELF or Mach-O image from
a file the user chose -- a split .debug file, an unstripped copy or a .dSYM
bundle -- through `neverd_session_load_dwarf`, in place of the DWARF it has. Its
machine must be the image's, and a build ID or UUID both record must agree.
The engine reads the image again, so the DWARF's names replace a MAP's. A
successful load changes the revision and returns `{path,stamp,loaded,
function_count,debug_info}`; the GUI saves `{path,stamp}` as the project's
`dwarf`, which `open` loads again once the image is, leaving the file open
without it and with a warning when it no longer loads. `dwarf_load` is listed
in `capabilities` when the engine has it.

`map_reference {path}` needs no open binary: it returns the SDK's
`neverd_map_reference_json`, `{map:null}` or `{map:{path,dialect,functions,
data,outside,match,reason?,stamp?,section?,names}}` -- the linker MAP beside
the input (`<stem>.map` or `<name>.map`), its linker format, its names and how
it holds against the image: `verified` (it records the image's build stamp),
`consistent` (its sections are where the image has them), `unverified` or
`mismatch`, with `reason` `stamp`, `section` or `outside`. The open reads the
MAP only when no PDB or DWARF describes the image, and never one that
mismatches it, so the GUI asks only then, after the PDB and DWARF questions,
and passes a declined answer as `map:{mode:"none"}`, or a mismatched MAP the
user still wants as `map:{path,mode:"unchecked",stamp}`. `map_inspect {path}`
reads a MAP the user chose against the open image, the same object with its
`stamp`, and `map_load {path,mode,stamp?}` loads it through
`neverd_session_load_map`: `checked` refuses a mismatched MAP with
`map_mismatch`, `unchecked` records the user's confirmation. Its names fill
only what a PDB or DWARF leaves unnamed, and replace an earlier MAP's. A
successful load changes the revision and returns the inspection with
`{mode,loaded,function_count,debug_info}`; the GUI saves `{path,mode,stamp}`
as the project's `map`, which `open` loads again after its PDB. With a `pdb`
selection, a `dwarf` file or a `map` value the open reads no MAP itself.
`map_reference`, `map_inspect` and `map_load` are listed in `capabilities`
when the engine has them.

`symbol_table_reference {path}` needs no open binary either: it returns the
SDK's `neverd_symbol_table_reference_json`, `{symbols:null}` or
`{symbols:{table,count,names}}` -- the image's static symbol table, which
strip removes: `.symtab` for ELF and SBF, `COFF symbol table` for a PE, and
`LC_SYMTAB` for the symbols a Mach-O image does not export, with the defined
names a load reads from it and its first function names, at most eight,
global ones first. An image without one, an object file and a format without
one answer null. Anyone can write names into the table, so the GUI asks
before an `open`, ahead of the PDB and DWARF questions, and passes a declined
answer as `symbols:{mode:"none"}`, the only `symbols` value there is: the
open reads the image as if stripped, keeping the symbols dynamic linking
needs, and the answer is saved with the project and kept by read-only
replicas. `open` and `metadata` return it as `symbols`, null when the table
was read. `symbol_table_reference` is listed in `capabilities` when the
engine has `neverd_symbol_table_reference_json` and
`neverd_session_set_symbol_table`.

`open` and `metadata` also return `pdb_download`: either null or `{file,key}`
derived by the SDK from a unique valid PE CodeView record. A candidate is offered
only with debug loading enabled, no loaded, selected or declined PDB and
PDB-loading support. It grants no network permission. The GUI may ask for
consent to download from the servers `pdb_reference` names, then use strict
`pdb_load` (`mode:"verified"`). For a freshly downloaded file, `stamp` may be
omitted on `pdb_load`; the worker inspects that final path and returns its
stamp. The GUI saves this returned selection for project reopening. SDK headers
JSON supplies `dynamic.pdb_symbol` and `debug_info:{enabled,kind,path}`; clients
using older engines see no candidate. The GUI includes the original `{file,key}`
as `download` on `pdb_load`. The worker rejects the command if it is no longer
the current missing-symbol candidate, including when another client has loaded a
PDB while the download was running.

Unless stated otherwise, operations require an open binary. Standard table pages
take `{offset:0,limit:128,filter:""}`, with limit 1–512, and return
`{items,total,offset,next_offset:integer|null,complete:boolean}`. `complete` means
no further pages remain after this page. Filtering is a substring match that
ignores case in any script, by the engine's Unicode simple case folding
(`neverd_fold_case`; only ASCII letters fold with an engine without it), so
"ПРИВЕТ" finds "привет"; the rows keep their original text.

`functions`, `strings`, `names`, `imports`, `exports`, `segments` and `regions`
also accept `seek_address:"0x..."`. The worker locates the address in the complete
filtered, sorted order and returns the containing page instead of the requested
offset, adding `selected_index` (global row index) and `selected_address` (the
canonical row address). Segment and region queries locate the containing range;
the other tables require an exact row address. Missing rows return `not_found`.
Selection cannot be combined with a `from` cursor and requires a positive limit.
Filtering, preview limits, sort direction and ordinary page bounds still apply.

Metadata also includes `loader_diagnostics:[{code,message}]`. An older engine
without the additive query exposes an empty array. The `open` reply includes
these messages in `warnings`; they describe the currently loaded image without
repairing its bytes or authorizing inferred semantics. Invalid PE entry metadata
exposes `entry_address:"0x0"`; clients can choose a mapped browsing position
without changing that entry. Invalid ordinary import descriptors publish no
guessed identities/storage bindings, while independent valid descriptors remain
available. Complete legacy WORD-packed relocation blocks are retained with a
diagnostic; fixed-image authentication still requires its stricter evidence.

| Operation | Payload and result |
|---|---|
| `identify` | Available before opening a file. `{path}`. Lists the ways the engine can load the file without loading it, from `neverd_identify_json`: `{rows:[{loader,text,processor,bits,endian,loadable,by_name?,reason?}]}` in the load dialog's order (rows a loader asks to lead, the others, then `Binary file`); `reason` says why a row cannot be loaded, and `by_name` marks a row only the file's name suggests, which loads only when chosen. For a file no header describes, the binary file row also carries what its bytes show: `guesses:[{isa,name,processor,share}]` (the likeliest instruction sets, `processor` empty for one NeverD cannot decode), `code_share` (the windows that look like code), `status` (`settled` when the bytes name one set, `width_unclear` when its family's instructions do not tell its 32-bit set from its 64-bit one, `unclear`, or `no_code`), `code_unit` and `code_offset` (the bytes the code's instructions align to, and where in the file they start modulo that), `wide_share` for a family of two widths (the share of its instructions only the 64-bit set has), `detected` (the processor an `open` without one reads the file as; empty unless `settled` names one NeverD decodes), `evidence`, and `fingerprint:{name,processor,entry,base}` when the file starts with a structure such as a Cortex-M vector table. A path that names no regular file is `invalid_request`. Capability present only with an engine that identifies files. |
| `open` | `{path:string,read_only:false,debug_info:true,analysis:true,warm:false,loader?,processor?,base?,offset?,size?,entry?,platform?}`. Loads a regular file into a new Session, keeping the old Session on failure; returns metadata plus `warnings:[]`. `debug_info:false` loads the image alone, ignoring the PDB, DWARF or linker map beside it; `analysis:false` turns off idle-time analysis (function discovery and the reference index; a cross-reference request still builds the index). `warm:true` adds a last step to that idle-time analysis: preparing each function's analysis for its first decompile (see Workbench names and background work). `loader` chooses how a file that names no format itself is read: `"evm"` as EVM bytecode, or `"binary"` as code of `processor` (`x86`, `x86_64`, `arm`, `thumb`, `aarch64`; absent or `auto` reads it from the bytes, and fails naming what they look like when they name no processor NeverD decodes clearly, or the file offset to read from when the code starts mid-word) mapped at `base`, from file `offset` on for `size` bytes (0 for the rest), starting at `entry` (the base when absent); the numbers are hexadecimal strings. `platform` names the platform whose conventions a binary file's code follows -- `sysv`, `windows` or `darwin` -- and `auto`, the default, has the engine read it from the code. The engine keeps a chosen loader, with the detected platform, in the project folder's `load.json`, which later opens without `loader` read, unless the session is read-only. Metadata carries how the file was read as `load_options`, the engine's load options JSON: for a binary file its `processor` with `processor_source` (`detected` or `user`) and `processor_evidence`, its placement, and `platform` with `platform_source` and `platform_evidence`. Without the engine's load options API, `loader` is `unsupported`. Project ID changes and revision increases on success. |
| `metadata` | Returns `path,architecture,format,bitness,file_size` (decimal string), `sha256` (the input's, in lowercase hexadecimal, empty when it cannot be read), `base_address,entry_address,function_count,segment_count,section_count,import_count,export_count,symbol_count,language,analyzed,analysis_state,read_only,dirty`. `function_count` is null before EVM/SBF analysis where a quick list is unavailable. `language` is `neverd_headers_json`'s: `{runtime,version?,secondary:[],evidence:[],pseudocode:[]}`, the runtime that built the image (`c`, `c++-itanium`, `c++-msvc`, `rust`, `go`, `objective-c`, `swift`, `delphi`, `ada`, `d` or `unknown`) and the languages its pseudocode reads in (`c`, and `rust` or `go` for an image with that language's code). |
| `functions` | Standard page in address order, filtered by display name, engine name, the name demangled or hex address and optionally sorted by `{sort:field,descending}`. Items carry `{name,address,size,library,thunk,exported}` under the workbench display name (see Listing below) plus `engine_name` when that differs, and may add `display_name,linkage_name,name_origin,display_origin,linkage_origin,recognition_state,library_annotations` from the shared session identity. A function the workbench names, such as a PLT entry `__ZN10QByteArrayC1EPKcx`, shows its name demangled (`neverd_demangle`) as `display_name`. The order is cached per filter, sort and listing generation; a table beyond one million functions is refused explicitly. Optional `from` is an exclusive address cursor for adjacent-function navigation, requires address order, and combines with `descending`; offsets and totals are relative to the portion after that cursor. The cache key includes direction. |
| `resolve` | `{query:"0x..."}` or exact symbol name. Returns `{address,function_address:hex|null,name,comment,import}`. Optional display/linkage/evidence fields share the function-list identity. For an interior VA, containing function discovery uses engine sizes. An unmapped numeric VA remains an exact navigation address. An import's name, as its symbol or as C calls it (`c_name` from the engine), resolves to its thunk where it has one and otherwise to its slot; `import` says whether the address is an import's slot or thunk. |
| `unwind_frame` | `{address:"0x..."}` only. Requires a loaded image and an engine exporting `neverd_unwind_frame_json`; advertised when that optional export exists. Returns the complete public C ABI frame object or `null` when no frame contains the exact requested address. It does not resolve the address to a function entry, subtract a return-address bias, analyze code or change revision. AArch64 frames may add `return_address:{query_pc,dwarf_state_at_pc,compact_recipe}`. `dwarf_state_at_pc` describes the exact PC; `compact_recipe` describes the completed prologue and never establishes the state at that PC. Source `null`, all typed statuses, diagnostics and a present `signing_pc:"0x0"` are preserved. An older engine may return a legacy frame without `return_address`. All address fields use the protocol's lowercase hexadecimal spelling. The listing's function-entry `__unwind` markers remain boundary annotations, not state queries. |
| `disasm` | `{address,limit:128}`; limit 1–512 instructions. Returns `{items,address,next_address:hex|null,complete}`. Rows `{address,size,bytes,mnemonic,operands,comment}` may include backend decode status. `next_address` is computed from the last decoded instruction's size, with overflow checks. A full page's successor is a continuation candidate; an empty following page means decoding ended. |
| `bytes` | `{address,size:256,text_encoding?}`; size 1–65536. Returns `{address,encoding:"hex",data,bytes_read,requested_size,mapping_status,next_address}`. Status is `mapped`, `partial`, or `unmapped_or_unmaterialized`; reading stops at the first unmapped byte. The legacy C ABI cannot distinguish BSS from unmapped bytes. Initial v1 carries bounded hex data in JSON, not separate binary attachments. With `text_encoding` (a `string_encodings` name) the reply adds `cells`, one per byte read, from the engine's `neverd_decode_text_json`: a character's text at its first byte, `""` at its later bytes, and `null` where no character is shown. The bytes decode from the first one, so a character cut by the range's ends shows as `null`. An unknown name is `unsupported_encoding`; an engine without the decoder returns `unsupported`. |
| `analyze` | Computes the synchronous full-image pipeline once, publishes a new revision, invalidates adapter caches, and returns metadata. The listing then reads the jump tables the pipeline recovered (`neverd_switches_json`): a table is named `jpt_<dispatch>` and, in the form the engine verified, laid out a slot per line; its load and dispatch are commented, and every target is referred to from the dispatch (`j`) and the table (`o`). No function-level incremental analysis or cooperative cancellation is claimed. |
| `signatures_load` | `{path,mode:"file"}` loads one `.pat` or library feature `.json`; `mode:"auto"` selects from a signature tree including `features/rules`. Returns `{loaded:true,byte_matches}` and advances the analysis revision. `byte_matches` counts candidates, not renamed functions; library classification requires the SDK's admitted `auto_name`. Invalid packs preserve the previous evidence and revision. Available on read-only images; it does not write annotations or the binary. |
| `decompile` | `{address,representation:"c",offset:0,limit:512}`; representation `c/llvmc/low/med/high/llvm/source/cpp/rust/go`, line limit 1–2048. Triggers analysis when needed. A known native chunk selects its recovered owner before analysis; `address` in the result is that entry. Explicit entries retain their identity, and unowned addresses retain the ordinary entry request. Returns `{address,representation,text,offset,total_lines,next_offset,complete,mapping_status,provenance_complete,rows,project_id,revision}`. Native Low/Med/High/LLVM/C/LLVMC use the optional bounded C ABI page API when present; other views retain one legacy function representation up to 32 MiB. C pages may also carry `byte_offset,function_identity,library_regions,prelude,recognition_budget_exhausted`. `cpp`, `rust` and `go` spell the HighC page in that language, for an image whose `language.pseudocode` lists it (otherwise `unavailable` with the engine's reason), and `source` in the function's own (C++, Rust, Go or C); they come only from the page API and add `dialect` (`c/cpp/rust/go`, what the page reads in), `unread` (why a declaration is shown as C) and `source_names` (`{begin_byte,end_byte,identifier,symbol,address?}` for each source name on the page, such as `core::fmt::write`, which splitting the text into identifiers would not find whole). A wire code page is capped at 2 MiB. See source mapping below. |
| `cfg` | `{address}`. Uses the same function-owner selection as `decompile`. Triggers analysis, returns `{name?,entry?,nodes,edges,complete:true}`. Nodes have `{id:string,start,end,insn_count,disasm:[string],address:start,label:start,lines:disasm}`; edges `{from:string,to:string|null,type}`. More than 500 nodes or 2000 edges returns `budget_exceeded`, with no partial graph. |
| `cfg_summary` | `{address,metrics?}`. Uses the same function-owner selection as `decompile`; layout keys and returned `address` use that entry. Builds one immutable graph snapshot on the executor; returns counts, bounds and an opaque `layout_revision`, without node/edge arrays. Up to 20000 nodes / 100000 edges. Optional `metrics:{char_width,line_height,padding,...}` size each node to its formatted listing rows; the layered layout routes edges around nodes. See viewport contract below. |
| `cfg_viewport` | `{address,layout_revision,x,y,width,height,scale:1,node_offset:0,edge_offset:0}`. Queries the current snapshot's spatial indexes; returns at most 256 nodes and 512 edges. No engine call or relayout per pan. A missing/mismatched snapshot returns `stale_layout`. |
| `xrefs` | `{address,direction:"to",offset,limit:128}`, limit 1–512. Direct references from each instruction's own lift, without whole-program analysis: rows `{address,from,to,kind,type,text,function,function_address?}` where `kind` is call/jump/cjump/read/write/offset and `type` its classic letter (p/j/r/w/o). "to" reads the parallel reference index, finishing it first when the background build has not; "from" decodes the one instruction. Engines without `neverd_code_refs_json` return `unsupported`. `{source:"ir"}` selects the previous IR-constant references instead: it triggers analysis, preserves engine `from/to,func,block/opcode` and adds `{address,function,kind:"IR constant reference"}`; EVM/SBF return `unsupported` there. |
| `call_paths` | `{waypoints:[...],exclude?,max_depth?,shortest_only?,data_refs?,max_paths?,max_listed?}`: the engine's `neverd_call_paths_json` over the graph that functions' direct references make, without analysis. A waypoint or exclusion is a `0x` address, or a name: one the listing shows, such as a synthesized `main`, resolves to its address first, and the engine resolves its own function and import names. Returns the engine's `{segments,unconnected,nodes,max_depth,data_refs}` with addresses as the protocol spells them and functions under the names the workbench shows. A malformed request or unknown waypoint is `invalid_request`; an engine without the query, `unsupported`. Listed in `capabilities` when the engine has it. |
| `call_tree` | `{root,direction:"callees"|"callers",max_depth?,max_listed?,data_refs?,exclude?,expand_library?}`: `neverd_call_tree_json` on the same graph, `root` resolved as a waypoint is. Returns `{root,direction,max_depth,nodes,edges,depth_limited,complete,bottom_up?}`, named as `call_paths` names them. |
| `strings` | Standard page; the filter matches the text, the address or the type. Strings in the encodings and minimum length of `string_options` (engines without `neverd_strings_ex_json` find ASCII strings of at least 4 characters). Rows `{address,text,value,length,chars,encoding,type}`: `length` counts bytes without the terminator, `text` is UTF-8, `type` is `C` for ASCII or the encoding's listing spelling (`UTF-8`, `UTF-16LE`). Parsed and cached per revision. Sorting by `address`, `length`, `type` or `text` covers the complete filtered index in the requested direction. Optional `text_limit` (1–1024 UTF-8 bytes) returns a display preview with an ellipsis and `text_truncated:true` when shortened; sorting, filtering and byte length retain the full value. Without it, `text` remains complete. The GUI requests 400-byte previews to keep pages containing multi-megabyte literals within the frame budget. |
| `string_encodings` | Available before opening a file. Returns `{items:[{name,spelling,unit,default,legacy?}]}`, the engine's string encodings; `legacy:true` marks a code page (GBK, Big5, Shift-JIS, Windows-1252, ...). Engines without `neverd_string_encodings_json` return `unsupported`. |
| `string_options` | Available before opening a file. `{encodings?:[name],preferred?:name\|null,min_length?:1-1024}` replaces the options the listing and `strings` search with, increments revision and invalidates both. C strings that are not UTF-8 are read in every code page among the encodings; `preferred` names the code page that reads them first and so wins where others read them as text too, and is searched whether or not the encodings name it. Without it, a code page wins that reads a string in the scripts it is made for and that no code page for another script contests. `min_length` counts display columns, a wide East Asian character two. An unknown name is `unsupported_encoding` and a preferred encoding that is not a code page `invalid_request`. Without fields it only reads. Returns `{encodings,preferred,min_length}`. The options outlive the open file. |
| `string_references` | `{offset,limit:128,filter?,sort:"address",descending:false}`, limit 1–512; sort `address`, `text`, `function` or `type`. Every instruction that refers to a string under `string_options`, from the engine's `neverd_string_refs_json` (listed once per string options and function list): rows `{address,string_address,text,type,encoding,kind,disasm,function,function_address?,via?}`. A reference into a string reads its text from the first character that starts at or after that byte, when at least `min_length` columns remain; `string_address` is the referenced byte. An instruction that reads a relocated slot holding such an address refers to the string through it, and `via` names the slot. `disasm` is the instruction as the listing shows it. The filter matches the text, the function name or the address, ignoring case in any script; each query's filtered and sorted rows are kept for its pages. An engine without the query returns `unsupported`. Returns `{items,total,offset,next_offset,complete}`. |
| `listing` | `{address,sub:0,before:0,after:100,opcode_bytes:0}`; before/after 0–2000 lines, opcode bytes 0–12. The classic text listing of the whole image around an address: rows `{item,item_size,address,sub,cls,kind,prefix,text,spans,target?,flow?,function?,function_address?}` with `[byte_offset,byte_length,role,address?]` spans into the UTF-8 text (roles from `ListingRoles.def`, address classes `cls` from `AddressClasses.def`). Returns `{lines,anchor,at_start,at_end,generation}`. `item_size` is the authoritative item byte extent, encoded as a hexadecimal string; clients must not infer an extent from neighboring displayed rows. No analysis is started; functions decode lazily and the `generation` changes when labels or names can change. |
| `overview` | `{buckets:1024}`, 1–16384. The address space as equal linear buckets, each the dominant address class digit: `{buckets:string,total,regions:[{name,start,end,initialized_end,linear,exec}],generation}`. |
| `names` | Standard page by name, sortable: every function (kind `function`, `library` or `thunk`), data and string label and import slot as `{name,address,kind}`. |
| `regions` | Standard page by name, sortable: sections or segments as `{name,address,start,end,size,initialized_end,flags,alignment,class}`. |
| `imports` | Standard page by name, sortable: `{address,name,module,ordinal}` at each import's data slot. |
| `exports` | Standard page by name, sortable: exports `{address,name,ordinal,kind:"export"}` and entry points with their engine type. |
| `search` | `{kind:"text"|"bytes",pattern,case_sensitive:false,limit:256,from?:address,backward:false}`, limit 1–4096. Bytes are hexadecimal pairs with optional spaces. `from` is an exclusive image-address cursor; matches are ordered by address in the requested direction. Returns `{items:[{address,...}],complete,next_address}`. `complete:false` means another match exists; pass `next_address` as `from` to continue. Without `from`, starts at the first/last initialized image byte. Segments are searched independently, including overlapping matches within a segment. A malformed pattern or cursor is `invalid_request`. Requires the matching engine's `neverd_search_page` C API; older engines report `unsupported`. |
| `segments` | Standard page by name; rows `{name,address,size,flags}`. `size` is the engine's hex string. |
| `annotations` | Standard page by text; rows `{address,text}`. |
| `annotation_set` | `{address,text}`; an empty string removes the comment. Stages an edit, increments revision, returns `{address,text,dirty:true,saved:false}`. Requires the writer lock. |
| `save` | Commits staged annotations and their command-history cursor through the recovery journal; returns `{saved:true,dirty:false}` after flushing. Requires the writer lock. |
| `rename` | `{address,name}`; any address: a function, data or a label. The name is not empty, has no spaces or control characters, is not an automatic form such as `sub_1234`, and is not used at another address. Requires clean annotations; never autosaves staged notes. Commits the rename plus history through the journal, reloads through the public C ABI, invalidates caches and increments revision. Returns `{address,name,saved:true}`. Requires the writer lock. |
| `code_edit` | `{address,representation,kind}` for a function's source view (`c/llvmc/source/cpp/rust/go`). `kind:"name"` adds `{original,name,identity}` from a current local `code_names` target; an empty name or the original name clears the alias. `kind:"comment"` adds `{line,anchor,text}` from a current source row; empty text removes it. `kind:"number"` adds `{line,anchor,index,radix}`: the `index`-th integer literal of that row's line, counted outside comments and strings as `include/neverd/support/IntegerLiterals.h` counts them, reads in `radix`, a base of `include/neverd/LiteralRadixes.def` (`hex`, `decimal`, `octal`, `binary`, `char`; `number` gives the number back to the decompiler). A C literal keeps its type, taking the suffix that keeps it; a base without such a spelling is `invalid_request`. Pages show the chosen bases of a row's numbers as `code_number_radixes` `{index:radix}`. Refuses obsolete/ambiguous targets, colliding or invalid local names, dirty annotations and read-only sessions. Commits the presentation edit and history, invalidates caches and increments revision. Returns `{address,saved}`, `saved:false` on no change. Image names use `rename` and mapped instruction comments use `annotation_set`. |
| `function_create` / `function_delete` | `{address}`. Capability present only with an engine that keeps function edits. Creates a function at the address, or deletes the one that starts there, as the engine allows (the project folder's `functions.json`). Requires clean annotations; commits the edit plus history, restarts function-level analysis and the reference index, and increments revision. Returns `{address,created,saved:true}`. Requires the writer lock. |
| `function_attributes` | `{address}`. Capability present with `function_edit`. Returns the SDK's `neverd_func_attributes_json`: what the Edit function dialog shows of the function that starts at the address, each attribute the user's statement, else analysis' answer, `type` the user's C declaration or null, and `stated` the user's statements. Reads only. |
| `function_edit` | `{address,attributes}`. Capability present only with an engine that keeps function edits. `attributes` states one key of `include/neverd/sdk/FunctionAttributes.def` or more: `noreturn`, `library`, `static` (true or false), `color` (`#RRGGBB`) and `type`, the function's C declaration as IDA's Y sets it, which the engine reads against the local types and refuses when it does not read or bind; null hands a key back to analysis. Commits the row (the project folder's `functions.json`) plus one history command and increments revision; a `type` drops only the decompiled text. Returns `{address,row,saved:true}`. Requires clean annotations and the writer lock. |
| `function_type` | `{address,declaration?}` or `{address,parameter,type}`. Capability present when the engine gives functions declarations. Returns the SDK's `neverd_func_type_json`: the declaration the user edits (the stated one, else the signature the decompiler prints) with its parameters' `name`, `type` and `location` (`rcx`, `stack+0x28`); with `declaration`, that one read instead, its `problems` by line and column; with `parameter` and `type`, the declaration Convert to struct * would state, an `invalid_request` when the function's declaration or the type does not read. Reads only: state a declaration with `function_edit`. |
| `item_define` | `{address,action,size?}`. Capability present only with an engine that keeps data items. `action:"code"` decodes native file-backed executable bytes from the exact address up to a basic block boundary or existing code, without creating a function. Calls fall through; branches, returns and unmodelled flow end the block. At most 256 instructions; invalid or truncated bytes, ambiguous modes, and overlaps with another typed item fail atomically. Stores one validated `kind:"code"` row per instruction, with its actual size. `"data"` makes the item a value of `size` 1, 2, 4 or 8 bytes, or without a size the next of byte, word, dword and qword after the current one; `"string"` makes the string that starts there an item, read with the `string_options` encodings; `"undefine"` shows the item's bytes as undefined bytes. Data/string edits are refused on automatic function instructions. Undefine hides an automatic or manually defined instruction without deleting its function; undo restores the prior item, and the override survives reopen. Manually defined code can also be replaced. Commits the items (the project folder's `items.json`) plus one history command, invalidates caches and increments revision. Returns `{address,kind,size,saved}`; code's size is the entire converted block. `saved:false` for already defined code or undefined bytes. Requires clean annotations and the writer lock. |
| `operand_format` | `{address,operand?:0-7,action}`. Capability present only with an engine that keeps operand formats. The address must start an instruction whose listing shows formats (x86 so far; other dialects are `unsupported`). A format changes an operand that is a plain number: `operand` when it is one, otherwise (or without `operand`) the instruction's last number; an instruction without one is `invalid_request`. `action` is a base of `include/neverd/OperandFormats.def` (`number`, `hex`, `decimal`, `binary`, `char`, `offset`, `struct`; `number` also clears both flags), or `negate`/`invert` to toggle the sign change or the bitwise complement. `struct` (capability `operand_struct`) shows a structure offset, as IDA's T does, and takes `type`, a structure or union of the local types, and `delta?`, how many bytes into it the register points: it also changes a displacement from a base register, which it takes first when `operand` names none, and the listing shows `[rcx+_KTHREAD.Process]` or `[rcx+(_KAPC.Type-10h)]`, the number again where the type has no member; a structure offset changes neither sign nor bits. A type the program lacks is `invalid_request`. Commits the formats (the project folder's `operands.json`) plus one history command; listing lines show the change without the listing being built again. Increments revision and returns `{address,operand,format:{base,negate,invert,type?,delta?},saved}` with the operand that took the format, `saved:false` when nothing changed. Requires clean annotations and the writer lock. |
| `reload` | Explicitly reloads the sidecars, discards staged annotations, clears dirty state, invalidates caches and increments revision. Returns metadata. |
| `history` | Standard `offset/limit` page. Returns `{schema_version:1,items,total,cursor,offset,next_offset,complete,can_undo,can_redo,available,blocked_reason,source_sha256}`. Items contain `{kind:"annotation"|"rename"|"function"|"item"|"operand"|"code"|"types"|"patch",address,before,after,index,applied}`; a `types` item's address is the declaration's place in `types.json`. Hashing is lazy and runs on the Session execution thread; the existing dashboard API can require VM analysis. |
| `undo` / `redo` | No payload. Annotation changes/cursor movement remain dirty until Save. Rename, function, item, operand, code, types and patch history changes require clean annotations and commit immediately. Return the history listing plus `{dirty,saved,address}`. All operations require the writer lock. |
| `history_reset` | Explicitly starts empty history from the current sidecars, preserving comments/renames. Requires clean annotations and the writer lock, then reloads their state. It never resolves a pending recovery journal or maps comments to a changed binary. |
| `contributions` | Available before opening a file. Returns `{schema_version:1,registry_revision:string,revision:string,items,complete:true}`. Each item has `{id,title,kind,namespace,version,query}` and optional table `columns`. The payload revision belongs to the registry, independently of the envelope's image revision. |
| `contribution_register` | `{path}`. Validates a local manifest completely before adding or replacing its namespace; returns the new contributions listing. Registration never executes a query or loads scripts/QML. |
| `contribution_unregister` | `{namespace}`. Removes that namespace and returns the new listing. The built-in `neverd` namespace is reserved. |
| `contribution_execute` | `{id,address?:hex}`. Executes that registered, whitelisted read-only query, replacing its exact `${address}` token with the supplied address or image entry. Returns `{contribution_id,operation,result}`. Arbitrary query/payload overrides are not supported. |

## Offline web operations

The shared JSON reader rejects duplicate decoded object keys through a bounded
SAX preflight before normal DOM construction. It avoids DOM-filter callbacks,
which can repeatedly scan large parent arrays in the pinned JSON library.
The same admission applies to native frames and persisted history; the byte
and depth limits remain unchanged.

Web requests use the same framing, serialized executor and cancellation
contract, with an operation prefixed by `web_`. They do not require an open
binary. Every web payload requires `schema_version:1`; unknown fields are
rejected. The payload is passed to the shared C API without independent
parsing, extraction or semantic inference in the adapter.

Responses carry `domain:"web"`. Envelope `project_id`, `revision`, and optional
request guards belong to the web session for these operations. Native requests
retain the native session's state. Successful web import does not change the
native revision or dirty state. Web inspection has
`analysis_state:"not_analyzed"|"partial"`, never the native `complete` claim.
Heartbeat retains its native fields and adds
`web:{revision,project_id}` for the published web snapshot.

| Operation | Payload fields after `schema_version` |
|---|---|
| `x87_capabilities` | Empty object; returns the C API's static x87 mode/ABI/state/consumer/backend matrix unchanged. Available before loading a binary; does not probe the host, execute code or change the project revision. Advertised only when the linked engine exports `neverd_x87_capabilities_json`; older engines return `unsupported`. Numerical and architectural-state support, profile requirements, unsupported reasons and test-source references retain their distinct meanings. Test-source references do not establish a passing run or a proof. |
| `web_capabilities` | None; returns actual compiled capabilities, including unavailable status |
| `web_import_preview` | `path`, optional `options` object using the C API import schema |
| `web_import_commit` | `preview_token` |
| `web_metadata` | None |
| `web_electron_manifest_analyze` | Exact `revision`, captured `artifact_id` (original file or available ASAR member); explicit manifest profile, exact admitted entry candidate, no runtime or framework verification |
| `web_desktop_manifest_analyze` | Exact `revision`, captured `artifact_id`, and `input_kind` (`nwjs` or `vsix`); bounded manifest declarations and exact parent-directory candidates, without runtime/activation/permission inference. Archive members retain their namespace; individual Bun/SEA assets have no neighbors |
| `web_electron_source_analyze` | Exact `revision`, analyzed `source_id`; lexical module-origin candidates and source-visible window/IPC/bridge boundaries; requires the JS parser |
| `web_electron_source_records` | Exact `revision`, `source_id`, optional `offset:0,limit:128`; exact source node/range links and fixed categories, private values omitted |
| `web_electron_ipc_analyze` | Exact `revision`, analyzed `manifest_artifact_id`, `source_ids` array of 1–16 analyzed sources including the main candidate; explicit captured directory scope, private exact UTF-16 channel comparison, no runtime routing proof |
| `web_electron_ipc_records` | Exact `revision`, `electron_ipc_id`, `record_kind:sources/channels/endpoints`, optional `offset:0,limit:128`; scope-bound IDs and candidate counts, no channel text/hash |
| `web_electron_entries_analyze` | Exact `revision`, analyzed `manifest_artifact_id`, `source_ids` array of 1–16 analyzed sources including the main candidate; captured preload/renderer paths compared with exact available members, no runtime or HTML analysis |
| `web_electron_entry_records` | Exact `revision`, `electron_entries_id`, `record_kind:sources/entries`, optional `offset:0,limit:128`; path evidence and optional target artifact/source IDs, private paths omitted |
| `web_artifacts` | Exact `revision`, optional `offset:0,limit:128` |
| `web_html_analyze` | Exact `revision`, captured `artifact_id`; UTF-8 script/base candidates, no DOM/fetch/execution; works without the JS parser |
| `web_html_records` | Exact `revision`, `html_id`, `record_kind:scripts/bases`, optional `offset:0,limit:128`; original ranges and eligible source artifact IDs, private reference values omitted |
| `web_asar_extract` | Exact `revision`, original `artifact_id`, optional captured `unpacked_directory_id`; no host companion lookup, member integrity/availability explicit |
| `web_asar_records` | Exact `revision`, `extraction_id`, optional page fields; private names, actual storage identities/ranges, usable `member_id` selections |
| `web_bun_extract` | Exact `revision`, original `artifact_id`; explicit qualified Bun ELF/thin Mach-O/PE profiles; no execution or producer authentication |
| `web_nexe_extract` | Exact `revision`, original or available member `artifact_id`, and explicit `profile`; bounded resource envelope, unverified opaque native prefix/signatures/runtime |
| `web_nexe_records` | Exact `revision`, `extraction_id`, `record_kind:regions/members`, optional `offset:0,limit:128` (max 512); private paths/payloads omitted, explicit selection IDs for shared consumers |
| `web_pkg_extract` | Exact `revision`, original or available member `artifact_id`, explicit `profile`; typed uncompressed VFS stores, opaque caches and unverified native/runtime evidence |
| `web_pkg_records` | Exact `revision`, `extraction_id`, `record_kind:regions/members/links`, optional `offset:0,limit:128` (max 512); private paths/metadata digests omitted, only stored content/prelude selections available |
| `web_bun_records` | Exact `revision`, `extraction_id`, `record_kind: modules/regions`, optional page fields; original offsets/hashes and private decoded source IDs |
| `web_source_analyze` | Exact `revision`, `artifact_id`, explicit `source_type` |
| `web_source_nodes` | Exact `revision`, `source_id`, optional page fields |
| `web_package_behavior_diff_analyze` | Exact `revision`, retained `package_diff_id`, optional matching `before_package_behavior_id` / `after_package_behavior_id`; missing/degraded coverage cannot become zero behavior |
| `web_package_behavior_diff_records` | Exact `revision`, `package_behavior_diff_id`, `record_kind:pairs/patterns/occurrences`, optional page fields (max 512); original occurrence anchors and fixed syntax counts per file candidate |
| `web_package_install_analyze` | Exact `revision`, `package_analysis_id`, optional matching `package_behavior_id`; captured lifecycle entry and selected module/behavior candidates, with explicit absent evidence and no execution |
| `web_package_install_records` | Exact `revision`, `package_install_id`, `record_kind:scripts/sources/requests/associations`, optional `offset:0,limit:128` (max 512); command byte offsets and opaque evidence chains, no command/path/argument disclosure |
| `web_package_advisory_import` | Exact `revision`, `artifact_id` and `descriptor_artifact_id`; captured OSV/source/time declarations only, no feed retrieval |
| `web_package_advisory_records` | Exact `revision`, `package_advisory_id`, `record_kind:affected/severities`, optional page fields (max 512); original anchors and fixed coverage, no private name/score/source label |
| `web_package_advisories_match` | Exact `revision`, `package_analysis_id`, optional `advisory_ids` (0..16); exact declared npm identity and admitted version selectors; unknown coverage is explicit |
| `web_package_advisories_match_records` | Exact `revision`, `package_advisory_matches_id`, optional page fields (max 512); original package/affected-entry anchors, no installed-content or exploitability proof |
| `web_package_attestation_import` | Exact `revision`, `artifact_id`, `descriptor_artifact_id`, `input_kind` (`registry-dist`, `intoto-statement`, `dsse-intoto`); bounded captured metadata without signature verification |
| `web_package_attestation_records` | Exact `revision`, `package_attestation_id`, `record_kind` (`signatures`, `subjects`, `digests`), optional page fields (max 512); no raw keys, signatures, payloads, URIs or digests |
| `web_package_attestation_digest_match` | Exact `revision`, original `artifact_id`, `package_attestation_id`, `digest_id`; one subject digest compared through the existing integrity owner; no inventory or publisher authentication |
| `web_package_attestation_digest_result` | Exact `revision`, `package_attestation_digest_match_id`; retained original-byte receipt |
| `web_source_bindings_analyze` | Exact `revision`, analyzed `source_id`; returns lexical-binding status and counts |
| `web_source_binding_records` | Exact `revision`, `source_id`, `record_kind: scopes/bindings/declarations/references`, optional page fields |
| `web_source_semantics_analyze` | Exact `revision`, `source_id`; returns bounded primitive/effect analysis status and counts |
| `web_source_semantic_records` | Exact `revision`, `source_id`, optional page fields; private value kinds/status and immediate/deferred effect categories only |
| `web_source_rewrite_analyze` | Exact `revision`, `source_id`, explicit `pass: string_recovery/constant_folding/scope_renaming/boolean_branches`; private candidate and metadata-only profile-specific receipt |
| `web_source_rewrite_records` | Exact `revision`, `rewrite_id`, optional page fields; fixed reasons and original/candidate ranges, no literal bytes or per-patch hashes |
| `web_source_modules_analyze` | Exact `revision`, `source_id`; module inventory and admitted-file comparison; derived HTML inline sources retain explicit document/script/base context, without runtime resolution |
| `web_source_module_records` | Exact `revision`, `source_id`, `record_kind: requests/imports/exports/attributes`, optional page fields; reported link profile/context and nullable URL presence flags, no specifier/name/path values |
| `web_source_bundles_analyze` | Exact `revision`, `source_id`; fixed structural loader/table recovery, without execution or producer authentication |
| `web_source_bundle_records` | Exact `revision`, `source_id`, `record_kind: bundles/modules/dependencies/regions`, optional page fields; original ranges and hashes, no module keys/source text |
| `web_source_view_preview` | Exact `revision`, `source_id`, optional schema-1 `options` object; metadata only, default structural policy |
| `web_source_view_commit` | Exact `revision`, `preview_token`; consumes the current preview attempt and publishes only a matching candidate |
| `web_source_view_records` | Exact `revision`, `view_id`, optional page fields; metadata mapping original/projected regions, also available before commit |
| `web_source_view_chunk` | Exact `revision`, committed `view_id`, canonical decimal-string `byte_offset`, optional integer `byte_limit:65536` (1–65536); UTF-8/CRLF-aligned inert text |
| `web_source_navigation_analyze` | Exact `revision`, `source_id`; source containment index with explicit lexical/runtime uncertainty |
| `web_source_navigation_records` | Exact `revision`, `source_id`, `record_kind: functions/calls/references/features/desktop_bridges/config_merges/config_files`, optional page fields; source spans and identity links, no raw names |
| `web_source_anchor` | Exact `revision`, `source_id`, canonical decimal-string `byte_offset` and `byte_length`, optional committed `view_id`; source/storage/display range metadata only |
| `web_native_open` | Exact `revision`, `selection_id` for an admitted original, Bun asset or available ASAR member; opens one independent snapshot-backed native session and returns metadata only |
| `web_native_metadata` / `web_native_analyze` | Exact `revision`, current `handoff_id`; query captured provenance/counts or explicitly run static native analysis; no target execution |
| `web_source_map_analyze` | Exact `revision`, `artifact_id`; also accepts an available ASAR `member_id` or extracted Bun `source_map_region_id` when native Zstd support is available |
| `web_source_map_sources` / `web_source_map_segments` | Exact `revision`, `map_id`, optional page fields |
| `web_source_map_lookup` | Exact `revision`, `map_id`, `generated_source_id`, canonical decimal-string `byte_offset` |

The web native handoff is distinct from the worker's ordinary native project.
It has no host path, debug-companion discovery or sidecar persistence. Failed
replacement leaves it usable; a successful web import or handoff replacement
destroys the previous handle. `pipeline_status` describes the selected native
pipeline only. Input bytes are capped at 256 MiB; native loader/pipeline resource
contracts remain separate from JavaScript parser budgets. Universal Mach-O
selection is unavailable. Other native APIs are not exposed through this table.

Required payload revisions bind source/artifact queries even when the optional
envelope guard is absent. Page limits are 1–512. Payloads preserve the backend's
`page_complete`, map association status, coordinate validation and fixed
diagnostic codes. Neither page completion nor valid map coordinates establish
semantic completeness or producer provenance. The metadata policy omits target
names, source text, URL references and literal values on ordinary metadata
operations. Explicit source-view chunks use a structural display policy instead.
Original regions require `locally_reviewed:true` and exact `reviewed_ranges` in
the preview's options, followed by commit. This is a local caller assertion,
not authorization for network/model disclosure. Replacing a source's policy
revokes its prior view; import commit clears all projections. View operations
do not authorize semantic rewrites. The separate rewrite operations apply
profile-specific complete-lexer, local value/effect or lexical-binding checks,
retained-tree validation and exact inversion. Boolean expression selection
retains chosen effects and adds explicit GetValue scaffolding. All profiles
publish metadata only and make no whole-program or source-reflection equivalence
claim. Raw rewrite export remains local C API/CLI only.
Ordinary semantic results explicitly deny rewrite permission. An unavailable effect
record is null rather than an empty list. These additive entry points are
resolved optionally, so an older web-capable engine still supports its existing
operations and reports the new ones unavailable.
Module request candidates retain `runtime_target_verified: false`; dynamic
imports and unverified require callees are never followed by the transport.
Bundle partitions likewise retain original source identities and unverified
runtime-target/producer status. An unmatched layout leaves the source unit intact.
See the [web schema](../../docs/web-artifact-schema.md).

`NeverDWorkerWeb` is a C++ adapter and framed-process test against the real
engine. It compares published artifacts/maps with direct C API-backed queries,
checks web/native revision isolation and runs the worker with an unusable
external-tool `PATH`. Windows input is still unavailable; its web test returns
the explicit CTest skip status until that reader is qualified.
For a standalone worker using an imported engine, enable
`NEVERD_WORKER_WEB_ENGINE_TESTS` only when that library exports the web C API;
the ordinary worker itself continues to support older engines without it.

Bun serialized maps reuse the map operations above. Responses preserve
container/compressed/decoded evidence identities and explicitly report
`retained_mapped_anchors_only`: Bun discarded symbol names and unmapped
boundaries. A module association is an unverified `container_assertion`,
not runtime/producer authentication. The worker forwards the backend's profile,
fidelity, storage and redaction fields without reinterpreting them.

## Variable associations

`source_variables` takes `{address,representation,source_options?}` for `c`,
`source` or `llvmc` and returns `{variables,snapshot,associations,results}`.
The inventory and fingerprint are engine-owned and route-specific. Associations
are ordered `map`/`split` policies; each result reports `{applied,refusal,outputs}`.

`code_edit` with `kind:"variables"` takes the same view arguments and its current
`snapshot`. `action:"split"` needs `variable_id`; `action:"map"` also needs the
destination `to` identity. The worker previews and proves the complete proposed
chain before committing. Failure returns `variable_edit_refused` without state,
revision or history changes. `unmap`/`unsplit` remove the matching zero-based
`association` index; `clear` removes every association for that function/route.
The existing code history, replicas and database sidecar transport own this state.
Reopening checks the image hash and every input fingerprint; stale dependent
policies remain diagnosable instead of rebinding by display names. Read-only
replicas can inspect and replay policies but cannot commit them.

## Per-call declarations

`call_sites` takes `{address,representation,source_options?}` for `c`, `source`
or `llvmc` and returns `{calls,snapshot,types}`. Each original LowIR occurrence
has an engine-owned `id`, `instruction`, `sequence`, `indirect`, and optional
`target`. A source page publishes the same `source_calls` and `call_snapshot`,
plus `call_type_results` with `{id,applied,refusal}` for persisted policies.

`code_edit` with `kind:"call_type"` takes those view arguments, `call_id`, the
current `snapshot`, and a complete C function `declaration`. For a variadic
declaration, `variadic_types` lists the actual trailing types for this call,
after default argument promotions; the fixed prefix comes from the declaration.
The declaration's function name is a placeholder, never a replacement target.
`reset:true` removes the selected occurrence; `action:"clear"` resets all call
declarations of the function. Policies are shared by both C routes, stored in
the function's `call_types` in `code.json`, image-hash and graph checked, and use
the existing transaction/history/replica/database transport. A failed preview
returns `call_type_refused` without changing state, revision or history.

The engine places scalar and pointer operands with its ordinary x64 PE/ELF/Mach-O
or AArch64 ELF/Mach-O C ABI. Aggregates, special calling conventions, Windows
ARM64 and unproved Win64 duplicate floating variadic carriers are refused.
System V x64 variadic floating arguments also require the original `AL` vector
count to be defined and sufficient in that call's basic block.
Stack arguments require complete writes in the call's own basic block, with no
intervening call or unknown memory write. Register inputs must have actual
defined SSA carriers. A private Low-to-Med rebuild retains those operands before
argument pruning; native IR, other call sites, callee prototypes and native
rewrite/safety admission keep their original analysis.

## IR and C source mapping

`code_edit_prepare` is an optional read with the same fields as a local-name
`code_edit`. It prepares and validates the target without saving, incrementing
revision or writing history; the GUI can run it while the name dialog is open.
A successful local-name commit adds `local_names_only:true` to its response.
Other code edits continue through full invalidation.

`analysis_update_names` accepts an owner `analysis_snapshot` only in an existing
readonly replica created by `analysis_restore`. Every input, option and
non-name state must match the restored snapshot. The worker also checks input
stamps, committed user tables, loaded type declarations and library choices
before replacing display-name state. A pending recovery transaction also
refuses reuse.
Success returns `{updated:true}` and advances the replica's local revision;
retained source is redecorated without rerunning the pipeline. This operation
never writes sidecars or history. A changed snapshot or racing sidecar is
`stale_snapshot`; a changed input is `input_changed`. The GUI uses the capability
only for an idle replica after a confirmed local-name commit and otherwise
keeps the existing fresh-process restore path.

Source pages with edit metadata carry `code_names`, half-open UTF-8 spans in
the complete displayed document. An address target has `kind:"address"`,
`address` and `original`; a local target has `kind:"local"`, `original` and
`identity`. The worker recognizes declared, unambiguous locals conservatively;
member spellings and text in strings/comments supply no inferred local target.
Rows add `code_anchor` (the original physical line) and `code_comment`.
Presentation aliases replace identifier occurrences, preserving literals and
remapping source names, library spans and prelude offsets. Notes render inline
without adding physical lines. Existing image annotations appear on mapped
source rows, and a function annotation appears at its definition.

Local aliases and unmapped notes belong to a function and representation in
the project folder's `code.json`. An alias's complete source identity is a portable
presentation hash, not semantic evidence: a different engine rendering suppresses
the alias and sets `code_edits_stale`. A note, and the bases picked for a line's numbers, require its line and original
anchor to match. These edits do not change engine variables, types or instruction
semantics. They share the normal journal, undo/redo, replica validation and `.nddb`
packing. An older page API that rejects `source` as an unsupported representation
may supply its HighC page instead, reported as `representation:"source",dialect:"c"`.

A worker linked to an engine exporting the additive `neverd_ir_view_json`
operation uses its native Low/Med/High/LLVM/C/LLVMC pages directly. The symbol is resolved from the
already loaded engine; older matching engines remain usable with
`mapping_status:"unavailable_engine_api"`. Native mapped pages contain
`mapping_status:"instruction_anchors"`, `provenance_complete:false`, and rows
`{line,object_id,kind,mapping_status,addresses:[hex],origin_seq?}`. `line` is the
absolute zero-based physical rendered line. Instruction address arrays are
canonical lowercase hex strings; opaque IDs may contain the engine's own casing.
The payload and envelope both bind the page to its `project_id` and `revision`.

An `instruction_anchor` identifies the original instruction for that surviving
IR operation. Low anchors are checked against canonical instruction boundaries
and the operation's address/sequence. A Med anchor additionally requires its
retained `(Addr, OriginSeq)` to match a Low occurrence. This supports selecting an
IR line to navigate to its exact instruction, or highlighting matching loaded
IR rows for an instruction address. It does not claim a complete set of all
instructions contributing to an expression after propagation or transformation.

Headers, block labels and successor rows are `unmapped`; PHIs and synthetic Med
operations are `synthetic`. Those rows have empty address arrays. HighIR
statement rows use the same surviving-expression index as C emission and must
match canonical LowIR occurrences. Braces, labels and handler metadata remain
unmapped. VM mappings are `unsupported_architecture`. Their text remains available through the existing
backend, with empty mapping rows. No source address is guessed from textual line
numbers, names, or the location of another representation's row.

The mapped native renderer is shared with the legacy Low/Med/High text APIs, so its
paged text remains byte-for-byte equivalent. Physical line counts come from that
same emission, including embedded newlines in display names. Page extraction
formats the function on the executor to count lines but retains only the bounded
selected page; it does not maintain a full mapping index or provide reverse
lookup across unloaded pages. IDs include stage/function/block, operation slot,
address and retained sequence and are stable only within the analysis revision.

Native C and LLVMC pages use `mapping_status:"instruction_anchors"` when
surviving source statement anchors pass the canonical LowIR boundary/sequence
check; otherwise they retain `mapping_status:"library_regions"`. Source rows
with these anchors have `kind:"source"`, `mapping_status:"instruction_anchor"`
and original instruction addresses. The first address belongs to the smallest
surviving statement span intersecting the row. Headers, declarations and
unbound or synthetic statements remain unmapped. The source map is not a
complete set of expression dependencies. Tab uses this explicit row address;
unmapped lines report that the GUI is showing the function entry.
Library-only mapped rows retain `mapping_status:"library_region"`. All pages have
`provenance_complete:false`. Their text is the ordinary, fully expanded C;
`byte_offset` locates the page in that full UTF-8 document. Each region includes
`id,function,scope,family,operation,display_name,linkage_name,receiver_type`,
`identity_evidence,rule_id,rule_revision,pack_id,pack_sha256,profile_sha256`,
`evidence_sha256,source_origin,source_revision,isolated,foldable,mapping_status`,
`occurrences:[{address,origin_seq}]` and `spans:[{begin_byte,end_byte}]`.
Scope is `whole-function`, `inline-expression`, `inline-region` or `call-site`.
A call-site region adds the original `callee` address and `callee_identity`; an
inline region does not invent a callee. Pages add `function_identity`, with
raw/display/linkage names and evidence identical to the function list.
Spans are half-open byte offsets in the full C document, not page-relative
character positions. Region metadata repeats on every page. Source provenance
describes the rule, not the exact library version used by the target.

Only a `mapped`, `isolated`, `foldable` region with all spans inside the loaded
text may collapse. Unknown, overlapping or partial regions remain expanded.
Rows intersecting a mapped span carry `mapping_status:"library_region"` and
the region's instruction addresses; other C rows are unmapped. A region may
map to several disjoint spans, without hiding intervening text. Clients retain
the full source for copying and export and retire folds on revision changes.
`recognition_budget_exhausted:true` reports a bounded stage that withheld its
incomplete candidates. Independently completed stages may still supply
annotations; no partially proved region becomes foldable.

A C page also carries `prelude:{lines,end_byte}` when the emitter recorded where
the function's definition begins: the whole lines before it (includes, support
types and declarations) and the byte in the full document where the definition
starts. Renamed identifiers move `end_byte` as they move region spans. Clients
may fold the prelude to one line; it is never a library region.

## CFG viewport contract

`cfg_summary` returns `{address,layout_revision,bounds:{x,y,width,height},
node_count,edge_count,resolved_edge_count,unresolved_edge_count,node_limit:256,
edge_limit:512,layout:"scc-layered-v1",snapshot_complete:true,complete:true}`.
The address is the exact requested function entry. Counts include the complete
validated snapshot; unresolved/dangling edges have no invented endpoint geometry.
The legacy `cfg` preview operation remains compatible and keeps its smaller cap.

The layout condenses strongly connected components, ranks the resulting DAG,
and orders each layer by exact numeric instruction address and stable node ID.
Layers wrap after eight nodes; nodes are 300 × 180 logical units with 80-unit
horizontal and 70-unit vertical gaps. Edges use deterministic orthogonal routes.
This basic layout preserves all CFG relations and supports loops; it does not
claim optimal crossings or a general graph layout quality benchmark. Coordinates
are ordinary finite logical values, never encoded instruction addresses.

Viewport rectangles are in those unscaled graph coordinates. `scale` is the
screen-to-graph zoom factor and only controls detail (instruction preview lines
are omitted below 0.6); it does not transform the rectangle. Each reply includes
summary fields plus `viewport,scale,nodes,edges,visible_node_count,
visible_edge_count,node_offset,edge_offset,next_node_offset,next_edge_offset,
nodes_truncated,edges_truncated,complete`. `snapshot_complete:true` means the
worker holds all supported graph objects. `complete:true` means neither viewport
cursor has another page. It does **not** mean all graph nodes are visible.
Truncation flags remain true on later pages if earlier visible objects were
omitted. A GUI should show a zoom/visibility indication when the requested view
exceeds the tile budget rather than materialize every page at overview zoom.

Nodes are `{id,address,start,end,label,lines,lines_truncated,insn_count,x,y,width,
height}`. `label` is the exact start address; previews have at most six lines and
2048 bytes, with explicit `lines_truncated`. Select a node and use bounded
`disasm` paging to inspect the complete block. Edges are `{id,from,to,type,
points:[{x,y},...]}`; endpoints may be offscreen, so render the supplied points
without looking up node objects. `type` retains the engine value (`true`,
`false`, `unconditional`, `indirect`, etc.). Node/edge IDs are opaque strings.

The two cursors page independently over a stable sorted list of intersecting
objects. Reuse the exact viewport, scale, function address and layout revision
while paging; use the corresponding total count as an exhausted cursor when
only the other cursor continues. `x/y` must be within ±1e9, width/height within
0.001..1e9, and scale within 0.000001..1000. Every image edit invalidates the
snapshot; refreshing `cfg_summary` obtains its new revision. Frame output is
bounded independently of total graph size. AABB indexes hold O(nodes + edges)
storage, including long back edges; exact orthogonal segment intersection avoids
false visible edges from an overlapping route bounding rectangle.

The existing C ABI still constructs and decodes a whole CFG JSON document before
this adapter sees it. Its 32 MiB result cap applies first; a 10k graph with unusually
large blocks can exceed that cap and receives an explicit budget error. The GUI
never receives this whole JSON result. Fully paged engine extraction would need
an additive metadata/block-text C ABI and remains separate work.

## Storage and practical limits

The binary is never modified. Its project is the folder `<binary>.neverd/`
(`neverd/support/ProjectFiles.def` names its files): `annotations.json` and
`renames.json` stay compatible with the CLI, as do the optional
`functions.json`, `items.json` and `operands.json` of engines that keep
function edits, data items and operand formats (`neverd function-edits`,
`neverd items`, `neverd operands`). An older engine's files beside the binary
(`<binary>.neverd-annotations.json` and its siblings) move into the folder as
the engine loads the binary. The engine records in `project.json` which build
of the binary the files describe, and the worker asks it to after each commit
(`neverd_project_record`); `metadata` reports it as `project`
(`neverd_project_json`). The worker also owns `local/history.json` and the
temporary recovery record `local/journal.json`, each user's own, which the
folder's `.gitignore` leaves out of version control. History contains schema version, source SHA-256,
engine version, command cursor, before/after values and committed sidecar state.
The most recent 128 commands are retained (also bounded by serialized size), and
a new edit removes the discarded redo branch.

Before changing durable files the worker writes and flushes a journal containing
the known before/after sidecar and history states. Each target is then replaced
atomically and flushed; the journal is removed after history publication. On a
writable reopen, recovery only completes this known transaction if the input
hash matches and every current file matches its before or after state. A third
state, source change or malformed journal refuses recovery without overwriting
sidecars. Read-only opens report pending recovery. An interrupted failed commit
must be recovered before another save. New CLI/C ABI annotation and rename writers use the same shared advisory
lock. Older engine/CLI binaries and external editors can bypass it, so semantic
foreign-state checks remain necessary; this is not general multi-writer ACID
isolation.

Successful Save acknowledges the staged annotations and command cursor; a
successful Rename acknowledges durable rename/history plus Session reload.
Unsaved annotations and annotation undo/redo are kept in memory and are lost on
termination. Rename refuses dirty annotations rather than implicitly saving
them. The GUI must prompt/save before explicitly discarding dirty work. History
is disabled on input-hash or foreign-sidecar mismatch; `history_reset` is an
explicit decision to preserve the current sidecars and discard the old command
chain. Size/mtime checks also refuse edits after a live input change; reopen the
binary before continuing.

Writable opens take an OS advisory lock (`flock` / `LockFileEx`) on
`<canonical-binary>.neverd/local/writer.lock`. The persistent lock file is not unlinked;
process exit releases the kernel lock, including after a crash. Other workers
must open read-only or wait. CLI builds predating this shared guard do not take the lock,
so concurrent CLI sidecar editing is outside this guarantee. Read-only opens
skip the lock and reject all edit/save/history mutation operations. History JSON
is capped at 8 MiB and the write-ahead journal at 24 MiB, both compact.

`project_sync` (writer only) takes the project's files as they are on disk after
something outside this worker changed them: git, another tool or a person. When
the edit tables differ from what the worker last committed, or the history no
longer describes them, it resets the history to the files, reloads every table
and keeps staged annotations staged on top of them, except those the files now
hold as staged (`comments_kept` counts the rest); a changed local types file
is reloaded and its analysis restarts. It answers `{changed, tables,
local_types, comments_kept, dirty}`, and `changed:false` for files the worker
wrote itself. Until a client syncs, an edit after such a change is refused with
`foreign_edits`. A table holding git's conflict markers fails with
`merge_conflict` naming the file. The tables' rows are ordered by address as
numbers, as the command line orders them. The
project tables (annotations, renames, functions, items, operands, code) are
written for version control: two-space indentation, sorted keys, `\u0008` and
`\u000c` for backspace and form feed, non-ASCII text as UTF-8 and one final line
break. These are the CLI's bytes for the same table, so a project kept in git diffs
only where its edits differ. Each such table is capped at 16 MiB as written.
Addresses stay strings
through history and recovery. Unknown command kinds and duplicate state addresses
are rejected, and recovery can only write the fixed sidecar paths.

Legacy functions returning whole JSON/text may allocate inside the engine
before this adapter sees them. The adapter rejects backend strings over 32 MiB
before parsing and retains only a strings snapshot, matching function indices,
one code representation and one bounded indexed graph. These limits do not constrain LLVM pipeline memory
or the backend's temporary result allocation. Further engine paging, bounded
engine graph extraction, priority scheduling, SQLite project storage and incremental
analysis remain separate work. JSON DOM overhead may exceed serialized length.

## Declarative contribution manifests

The GUI's **Analysis queries → Import queries…** action can register a JSON
file such as:

```json
{
  "schema_version": 1,
  "namespace": "sample",
  "version": "1.0",
  "contributions": [
    {
      "id": "sample:selected-instructions",
      "kind": "panel",
      "title": "Selected instructions",
      "query": {
        "operation": "disasm",
        "payload": {"address": "${address}", "limit": 128}
      }
    }
  ]
}
```

Namespace and local IDs are lowercase identifiers, and every ID is namespaced.
Kinds are `command`, `panel`, or `table`; a table may specify up to 16
`columns:[{key,title}]`. A manifest is at most 64 KiB and has 1–32 contributions.
The registry holds at most 16 namespaces and 128 total items. Built-in
`neverd` descriptors cannot be replaced or removed.

Allowed query operations are only `metadata`, `functions`, `strings`, `disasm`,
`decompile` (C/LowIR/MedIR/HighIR/LLVM IR) and `xrefs`. Their parameters use the
same operation budgets; unexpected keys, script/QML fields and mutation/nested
contribution operations are rejected. The only template substitution is an exact
`${address}` in the address field. The GUI renders titles and result data as
plain text and invokes `contribution_execute`; it does not evaluate manifest
strings as code. Registry changes have their own monotonic revision. Registry
contents last for one worker lifetime; the GUI may remember explicitly imported
manifest paths and import them again after restart. Registration does not
authorize file writes or autonomous query execution.

Analysis queries are distinct from executable plugins. The CLI exposes these
analysis categories through its existing commands and can reuse saved argument
files; see [Analysis queries from the CLI](../../docs/gui.md#analysis-queries-from-the-cli).
Those argument files and these JSON manifests are different formats.

## Building and tests

As part of the repository use the optional worker target and existing
`neverd_shared`. To build this directory alone against a matching engine:

```sh
cmake -S tools/neverd-worker -B build-worker -G Ninja \
  -DNEVERD_ENGINE_LIBRARY=/absolute/path/to/libneverd.dylib -DBUILD_TESTING=ON
cmake --build build-worker
ctest --test-dir build-worker --output-on-failure
```

Windows also requires `NEVERD_ENGINE_IMPLIB`. The JSON-only dependency is pinned
to nlohmann/json 3.11.3 by SHA-256; no LLVM or Qt is discovered by this standalone
project. Tests include a deterministic fake C ABI linked only into a test
executable, testing framing/fragmentation, UTF-8, high VAs, paging, queue limits,
heartbeat during blocking analysis, truthful cancellation, writer exclusion,
atomic sidecar replacement and restart persistence. A separate suite covers
history branches, source/foreign-edit refusal, partial-commit recovery and
declarative manifest validation. Graph tests cover 10k nodes with loops,
independent cursors, stable geometry, high addresses, viewport culling and 100
actual framed IPC pans; mutation invalidates old layout tokens. Mapped Low/Med
pages and the old-engine fallback are checked separately. The real-engine smoke verifies EVM decoding,
all IR stages, CFG and the public SHA-256 against Python's digest. The shipped
target always links the real engine. `NEVERD_WORKER_REAL_ENGINE_TESTS=OFF` is for
CI that deliberately supplies a deterministic fixture engine, not for claiming
real-engine validation.

## Workbench names and background work

The `listing`, `functions`, `names` and `resolve` operations share one naming
layer for the workbench. Functions the engine leaves generic take classic
names: executable import veneers (ELF PLT entries and `.plt.got` stubs that
jump through an import slot) are thunks named after their import (`_` +
import on ELF), the image entry is `start`, and a function at the start of
`.init` or `.fini` is `_init_proc` or `_term_proc`. The engine names `main`
itself: the function the image's entry passes `__libc_start_main` (or another
start routine of `libc/LibCStartup.h`) as main, which its C also declares as
the C runtime calls it, `int main(int argc, char **argv, char **envp)`.
Engine, symbol and user names always take precedence, and `resolve` accepts
the workbench names. Import data slots, including ELF `GLOB_DAT` entries
from `neverd_import_slots_json`, are named `<import>_ptr` on ELF. Code text
from the engine (C and IR) keeps the engine's spellings.

Heartbeats carry
`background:{state,done,total,references,generation,functions,discovery_complete}`
for the work the worker does while idle. It first lets the engine add the
functions its detector finds without lifting, polling
`neverd_session_discover_functions_step` between foreground requests. Discovery
owns one image snapshot and publishes its result on the executor; the owner
continues serving reads while that snapshot computes. Older engines use the
synchronous `neverd_session_discover_functions` fallback.
`discovery_complete` lets clients defer large analysis replica startups until
that snapshot is released. `functions` is the listed function count, which
clients compare to refresh function lists. It is absent until the listing is
first built, because discovery does not build the listing. The worker then
builds the reference index: `pending`, `building`, `ready` or `unavailable`.
Its generation changes when finished references can add labels and
cross-reference comments to listing text; clients refresh visible lines then.
Function-level analysis runs only for
the function a decompile, CFG or IR request names; `analyze` remains the
explicit whole-program pipeline. Discovery failures carry `error` and an
`unavailable` state rather than claiming a complete reference index.

With `open`'s `warm:true`, the worker then prepares functions in entry order,
a few per idle step, building the control-flow graphs their first decompile
would otherwise build (`neverd_warm_functions_json`); decompiles return the
same results either way. `background.warm` reports `{state,prepared}`: `off`,
`pending`, `running`, `complete`, `full` (the engine refused more without
discarding what it holds) or `unavailable` (an engine or image whose
decompiles keep no such state, such as one that is not x86-64), and
`prepared` counts the functions it holds. Every revision change starts the preparation over, since
the engine may have dropped it.


### Interactive byte and source patches

These operations appear in `hello.capabilities` only when the linked SDK
exports their implementation. Byte overlays, source proofs and whole-function
replacement are advertised independently.

| Operation | Request and result |
|---|---|
| `patch_context` | `address`; current bytes/instruction, applicable action names, and `revert_ranges:[{address,size}]` for canonical changed runs intersecting that instruction (or the addressed byte when no instruction is available). Pass these ranges to `patch_revert` to restore operand-only changes too. |
| `patch_preview` | A patch request below; returns `address`, `file_off`, `size`, `original`, `current`/`hex`, `covers`, `decoded`, `covers_truncated`, `decoded_truncated` and `warnings` without mutation. Instruction arrays are bounded for display; validation still checks the complete range. |
| `patch_apply` | The same request, optionally `expected` original hex; applies and journals one edit, persists `patches.json` and invalidates analysis. Requires the writer and no staged unsaved annotations. |
| `patch_revert` | `address` and optional `size` (zero restores the containing changed run), or `ranges:[{address,size}]`; restores immutable input bytes. A batch is one history command. |
| `patches` | Optional `offset`, `limit`, or `ranges_only`; returns canonical changed runs, their original/current bytes and kind, with ordinary page metadata. `ranges_only:true` returns all `{address,size}` ranges without byte payloads; current SDKs encode only these ranges, while older SDKs retain the full-row compatibility path. |
| `patch_export` | `path` and optional `allow_unsigned`; exclusively publishes a new binary by replaying the overlay. Never overwrites an existing destination. |
| `patch_source_target` | `function`, `representation` (`c`/`llvmc`), zero-based `line`, and `literal` (zero-based integer index, or -1 for a statement); returns only proven actions and the exact `source_anchor`. |
| `patch_source_preview` | A proven source patch request; returns `before` and `after` C, or `after_error`, using an isolated session. |
| `source_patch_context` | Function `address`; compilable HighC `source`, replacement `symbol` and explicit original `bindings`. Requires an empty byte overlay. |
| `source_patch` | `address`, C `source`, `symbol`, output `path`, `compiler` (`clang`/`neverc`/`gcc`), optional `compiler_path`, `timeout_seconds` and `bindings`; returns the source publisher's receipt and produced `path`. |

A patch request contains `address` and `kind`: `bytes`, `assemble`, `nop`,
`fill_nops`, `branch_always`, `branch_never`, `branch_invert`, `skip_call`,
`skip_call_value` or `constant`. `bytes` holds hex pairs; `text` holds one
assembly instruction; `value` is an unsigned address-form string; `size` is a
byte count. Assembly accepts `dialect` (`intel`/`att`), `fill_nops` (default true),
`allow_overlap` (default false), and a `symbols` object of signed integer
bindings. Referenced frame-variable names receive the listing's explicit
frame displacements; ambiguous or unresolved names are never guessed.

A source edit also supplies `function`, `representation`, `line`, `literal`
and the returned `source_anchor`. Preview and apply repeat the source proof;
changed text or bytes refuse the edit. Source navigation addresses alone are
insufficient. Literal proof currently requires a direct x86 MOV scalar value,
possibly reaching a return through local copies and zero extensions. Arithmetic,
partial writes, casts, predecessor merges and multiple literals remain refused.

Whole-function `bindings` are `{symbol,kind,address,size}` rows, with kind
`function`, `data`, `const` or `original`. The last kind explicitly names the
saved original function body. A whole-function rewrite uses the existing
source publisher and produces a new file; export and reopen any byte overlay
before requesting it. Neither source preview nor successful publication adds
an in-memory byte patch.

Overlay admission rejects relocation fields, loader-rewritten or zero-fill
bytes, ambiguous mappings and load headers. Signed PE/Mach-O export requires
explicit unsigned consent. History stores the full durable overlay, while
history listing responses expose `changed_rows` and `details_omitted` instead
of repeating potentially large byte payloads. Patch history items and undo/redo
responses expose the virtual program address of the first changed run;
durable history retains its canonical file-offset key for replay.
