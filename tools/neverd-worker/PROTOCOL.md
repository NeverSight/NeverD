# NeverD worker protocol 1.0

`neverd-worker` is a local child process, owns one C ABI Session, and has no Qt
or LLVM linkage of its own. The GUI must launch the bundled worker by explicit
path, continuously drain stdout and stderr, and validate `protocol_major` before
opening a binary. It does not listen on a network port or execute target code.

## Transport and lifecycle

Each message is a **4-byte unsigned big-endian byte length**, followed by that
many bytes of UTF-8 JSON. Length is 1 through 8 MiB. Input nesting is capped at
64. Empty, truncated and oversized frames terminate the connection with a
`fatal` message; malformed JSON produces an error response and preserves framing.
stdout is a protocol-only channel. Before Session creation, the worker duplicates
the output handle and redirects ordinary stdout to stderr, including C stdio,
C++ streams, and embedded Python output. Engine dynamic-library initializers run
before `main` and must remain quiet, as with the existing linked CLI boundary.

The first unsolicited message is:

```json
{"type":"hello","protocol_major":1,"protocol_minor":0,"engine_version":"...","project_schema":1,"revision":"0","project_id":"","max_frame_bytes":8388608,"capabilities":["metadata","functions","disasm","bytes","decompile","cfg","xrefs","strings","analyze","resolve","annotations","annotation_set","rename","save","reload","segments","read_only","heartbeat","cancel"],"storage":"existing_json_sidecars"}
```

`project_schema:1` refers to this sidecar adapter and versioned edit history, not
the proposed SQLite project format. No analysis cache is persisted. Edit history
is bound to the input SHA-256 supplied by the public dashboard C API. The engine
version must match the shipped engine. `hello` is also
accepted as a request and returns this object in its response payload.

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
include `error:{code,message}`. Display messages are diagnostics; use stable
`code` values for localization. A successful empty page differs from errors,
unsupported representations and analysis failures. Every VA is a `0x`-prefixed
lowercase hexadecimal string, including VAs above 2^53. Revisions and project IDs are opaque
strings. CFG node/edge IDs are strings, not addresses or floating-point values.
Byte offsets inside a requested page and bounded counts are JSON integers.

Session calls run serially on an execution thread; a separate input loop admits
requests and cancellation while the synchronous engine runs. The queue holds at
most 32 waiting requests and 16 MiB of request bodies; excess receives
`queue_full`. Replies and heartbeat only expose published revisions. The current
adapter serializes every engine query; it does not claim concurrent immutable
engine snapshots. Query results are bounded before IPC, and models must still
bound their own cached pages.

Every second the worker sends
`{type:"heartbeat",protocol_major:1,project_id,revision,active_request_id:string|null,cancellation_requested:bool}`.
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

## Operations

Unless stated otherwise, operations require an open binary. Standard table pages
take `{offset:0,limit:128,filter:""}`, with limit 1–512, and return
`{items,total,offset,next_offset:integer|null,complete:boolean}`. `complete` means
no further pages remain after this page. Filtering is an ASCII-case-insensitive
substring match; original non-ASCII bytes are preserved.

| Operation | Payload and result |
|---|---|
| `open` | `{path:string,read_only:false}`. Loads a regular file into a new Session, keeping the old Session on failure; returns metadata plus `warnings:[]`. Project ID changes and revision increases on success. |
| `metadata` | Returns `path,architecture,format,bitness,file_size` (decimal string), `base_address,entry_address,function_count,segment_count,section_count,import_count,export_count,symbol_count,analyzed,analysis_state,read_only,dirty`. `function_count` is null before EVM/SBF analysis where a quick list is unavailable. |
| `functions` | Standard page filtered by name or hex address. Items `{name,address,size}`. Filtering caches matching indices; an index beyond one million functions is refused explicitly. Unfiltered pages read only requested functions. |
| `resolve` | `{query:"0x..."}` or exact symbol name. Returns `{address,function_address:hex|null,name,comment}`. For an interior VA, containing function discovery uses engine sizes. An unmapped numeric VA remains an exact navigation address. |
| `disasm` | `{address,limit:128}`; limit 1–512 instructions. Returns `{items,address,next_address:hex|null,complete}`. Rows `{address,size,bytes,mnemonic,operands,comment}` may include backend decode status. `next_address` is computed from the last decoded instruction's size, with overflow checks. A full page's successor is a continuation candidate; an empty following page means decoding ended. |
| `bytes` | `{address,size:256}`; size 1–65536. Returns `{address,encoding:"hex",data,bytes_read,requested_size,mapping_status,next_address}`. Status is `mapped`, `partial`, or `unmapped_or_unmaterialized`. The legacy C ABI cannot distinguish BSS from unmapped bytes. Initial v1 carries bounded hex data in JSON, not separate binary attachments. |
| `analyze` | Computes the synchronous full-image pipeline once, publishes a new revision, invalidates adapter caches, and returns metadata. No function-level incremental analysis or cooperative cancellation is claimed. |
| `decompile` | `{address,representation:"c",offset:0,limit:512}`; representation `c/low/med/high/llvm`, line limit 1–2048. Triggers full analysis when needed. Returns `{address,representation,text,offset,total_lines,next_offset,complete,mapping_status,provenance_complete,rows,project_id,revision}`. Native Low/Med use the optional bounded C ABI page API when present; other views retain one legacy function representation up to 32 MiB. A wire code page is capped at 2 MiB. See instruction mapping below. |
| `cfg` | `{address}`. Triggers analysis, returns `{name?,entry?,nodes,edges,complete:true}`. Nodes have `{id:string,start,end,insn_count,disasm:[string],address:start,label:start,lines:disasm}`; edges `{from:string,to:string|null,type}`. More than 500 nodes or 2000 edges returns `budget_exceeded`, with no partial graph. |
| `cfg_summary` | `{address}`. Builds one immutable graph snapshot on the executor; returns counts, bounds and an opaque `layout_revision`, without node/edge arrays. Up to 20000 nodes / 100000 edges. See viewport contract below. |
| `cfg_viewport` | `{address,layout_revision,x,y,width,height,scale:1,node_offset:0,edge_offset:0}`. Queries the current snapshot's spatial indexes; returns at most 256 nodes and 512 edges. No engine call or relayout per pan. A missing/mismatched snapshot returns `stale_layout`. |
| `xrefs` | `{address,direction:"to",offset,limit}`. Triggers analysis. Rows preserve engine `from/to,func,block/opcode` and add `{address,function,kind:"IR constant reference"}`. These are existing IR-constant references, not a stronger claim of exact calls/data references. EVM/SBF return `unsupported`, because this C ABI scans native LowIR only. |
| `strings` | Standard page, filter matches string content, minimum length 4. Rows `{address,text,value,length}`. One legacy result is parsed and cached per revision. |
| `segments` | Standard page by name; rows `{name,address,size,flags}`. `size` is the engine's hex string. |
| `annotations` | Standard page by text; rows `{address,text}`. |
| `annotation_set` | `{address,text}`; an empty string removes the comment. Stages an edit, increments revision, returns `{address,text,dirty:true,saved:false}`. Requires the writer lock. |
| `save` | Commits staged annotations and their command-history cursor through the recovery journal; returns `{saved:true,dirty:false}` after flushing. Requires the writer lock. |
| `rename` | `{address,name}`; address must be a function entry. Requires clean annotations; never autosaves staged notes. Commits the rename plus history through the journal, reloads through the public C ABI, invalidates caches and increments revision. Returns `{address,name,saved:true}`. Requires the writer lock. |
| `reload` | Explicitly reloads both sidecars, discards staged annotations, clears dirty state, invalidates caches and increments revision. Returns metadata. |
| `history` | Standard `offset/limit` page. Returns `{schema_version:1,items,total,cursor,offset,next_offset,complete,can_undo,can_redo,available,blocked_reason,source_sha256}`. Items contain `{kind:"annotation"|"rename",address,before,after,index,applied}`. Hashing is lazy and runs on the Session execution thread; the existing dashboard API can require VM analysis. |
| `undo` / `redo` | No payload. Annotation changes/cursor movement remain dirty until Save. Rename history changes require clean annotations and commit immediately. Return the history listing plus `{dirty,saved,address}`. All operations require the writer lock. |
| `history_reset` | Explicitly starts empty history from the current sidecars, preserving comments/renames. Requires clean annotations and the writer lock, then reloads their state. It never resolves a pending recovery journal or maps comments to a changed binary. |
| `contributions` | Available before opening a file. Returns `{schema_version:1,registry_revision:string,revision:string,items,complete:true}`. Each item has `{id,title,kind,namespace,version,query}` and optional table `columns`. The payload revision belongs to the registry, independently of the envelope's image revision. |
| `contribution_register` | `{path}`. Validates a local manifest completely before adding or replacing its namespace; returns the new contributions listing. Registration never executes a query or loads scripts/QML. |
| `contribution_unregister` | `{namespace}`. Removes that namespace and returns the new listing. The built-in `neverd` namespace is reserved. |
| `contribution_execute` | `{id,address?:hex}`. Executes that registered, whitelisted read-only query, replacing its exact `${address}` token with the supplied address or image entry. Returns `{contribution_id,operation,result}`. Arbitrary query/payload overrides are not supported. |

## IR instruction mapping

A worker linked to an engine exporting the additive `neverd_ir_view_json`
operation uses its native Low/Med pages directly. The symbol is resolved from the
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
operations are `synthetic`. Those rows have empty address arrays. C, High and
LLVM mappings are `unsupported_representation`; VM Low/Med mappings are
`unsupported_architecture`. Their text remains available through the existing
backend, with empty mapping rows. No source address is guessed from textual line
numbers, names, or the location of another representation's row.

The mapped native renderer is shared with the legacy Low/Med text APIs, so its
paged text remains byte-for-byte equivalent. Physical line counts come from that
same emission, including embedded newlines in display names. Page extraction
formats the function on the executor to count lines but retains only the bounded
selected page; it does not maintain a full mapping index or provide reverse
lookup across unloaded pages. IDs include stage/function/block, operation slot,
address and retained sequence and are stable only within the analysis revision.

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

The binary is never modified. Existing `<binary>.neverd-annotations.json` and
`<binary>.neverd-renames.json` stay compatible with the CLI. The worker also owns
`<binary>.neverd-history.json` and the temporary recovery record
`<binary>.neverd-journal.json`. History contains schema version, source SHA-256,
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
`<canonical-binary>.neverd-gui.lock`. The persistent lock file is not unlinked;
process exit releases the kernel lock, including after a crash. Other workers
must open read-only or wait. CLI builds predating this shared guard do not take the lock,
so concurrent CLI sidecar editing is outside this guarantee. Read-only opens
skip the lock and reject all edit/save/history mutation operations. History JSON
is capped at 8 MiB and the write-ahead journal at 24 MiB. Addresses stay strings
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

An explicit GUI import can register a JSON file such as:

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
