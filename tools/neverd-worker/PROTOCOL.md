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

Responses from the Session executor also include
`analysis_state:"not_analyzed"|"complete"`, even when the requested view is
unsupported after successful analysis. This lets clients refresh recovered
function lists once when a lazy query completes analysis. Ordinary edit
revisions do not imply a new analysis. Administrative replies may omit this
additive field.

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
no further pages remain after this page. Filtering is a substring match that
ignores case in any script, by the engine's Unicode simple case folding
(`neverd_fold_case`; only ASCII letters fold with an engine without it), so
"ПРИВЕТ" finds "привет"; the rows keep their original text.

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
| `open` | `{path:string,read_only:false,debug_info:true,analysis:true,loader?,processor?,base?,offset?,size?,entry?,platform?}`. Loads a regular file into a new Session, keeping the old Session on failure; returns metadata plus `warnings:[]`. `debug_info:false` loads the image alone, ignoring the PDB, DWARF or linker map beside it; `analysis:false` turns off idle-time analysis (function discovery and the reference index; a cross-reference request still builds the index). `loader` chooses how a file that names no format itself is read: `"evm"` as EVM bytecode, or `"binary"` as code of `processor` (`x86`, `x86_64`, `arm`, `thumb`, `aarch64`; absent or `auto` reads it from the bytes, and fails naming what they look like when they name no processor NeverD decodes clearly, or the file offset to read from when the code starts mid-word) mapped at `base`, from file `offset` on for `size` bytes (0 for the rest), starting at `entry` (the base when absent); the numbers are hexadecimal strings. `platform` names the platform whose conventions a binary file's code follows -- `sysv`, `windows` or `darwin` -- and `auto`, the default, has the engine read it from the code. The engine keeps a chosen loader, with the detected platform, in `<input>.neverd-load.json`, which later opens without `loader` read, unless the session is read-only. Metadata carries how the file was read as `load_options`, the engine's load options JSON: for a binary file its `processor` with `processor_source` (`detected` or `user`) and `processor_evidence`, its placement, and `platform` with `platform_source` and `platform_evidence`. Without the engine's load options API, `loader` is `unsupported`. Project ID changes and revision increases on success. |
| `metadata` | Returns `path,architecture,format,bitness,file_size` (decimal string), `base_address,entry_address,function_count,segment_count,section_count,import_count,export_count,symbol_count,language,analyzed,analysis_state,read_only,dirty`. `function_count` is null before EVM/SBF analysis where a quick list is unavailable. `language` is `neverd_headers_json`'s: `{runtime,version?,secondary:[],evidence:[],pseudocode:[]}`, the runtime that built the image (`c`, `c++-itanium`, `c++-msvc`, `rust`, `go`, `objective-c`, `swift`, `delphi`, `ada`, `d` or `unknown`) and the languages its pseudocode reads in (`c`, and `rust` or `go` for an image with that language's code). |
| `functions` | Standard page in address order, filtered by display name, engine name, the name demangled or hex address and optionally sorted by `{sort:field,descending}`. Items carry `{name,address,size,library,thunk,exported}` under the workbench display name (see Listing below) plus `engine_name` when that differs, and may add `display_name,linkage_name,name_origin,display_origin,linkage_origin,recognition_state,library_annotations` from the shared session identity. A function the workbench names, such as a PLT entry `__ZN10QByteArrayC1EPKcx`, shows its name demangled (`neverd_demangle`) as `display_name`. The order is cached per filter, sort and listing generation; a table beyond one million functions is refused explicitly. |
| `resolve` | `{query:"0x..."}` or exact symbol name. Returns `{address,function_address:hex|null,name,comment,import}`. Optional display/linkage/evidence fields share the function-list identity. For an interior VA, containing function discovery uses engine sizes. An unmapped numeric VA remains an exact navigation address. An import's name, as its symbol or as C calls it (`c_name` from the engine), resolves to its thunk where it has one and otherwise to its slot; `import` says whether the address is an import's slot or thunk. |
| `disasm` | `{address,limit:128}`; limit 1–512 instructions. Returns `{items,address,next_address:hex|null,complete}`. Rows `{address,size,bytes,mnemonic,operands,comment}` may include backend decode status. `next_address` is computed from the last decoded instruction's size, with overflow checks. A full page's successor is a continuation candidate; an empty following page means decoding ended. |
| `bytes` | `{address,size:256,text_encoding?}`; size 1–65536. Returns `{address,encoding:"hex",data,bytes_read,requested_size,mapping_status,next_address}`. Status is `mapped`, `partial`, or `unmapped_or_unmaterialized`; reading stops at the first unmapped byte. The legacy C ABI cannot distinguish BSS from unmapped bytes. Initial v1 carries bounded hex data in JSON, not separate binary attachments. With `text_encoding` (a `string_encodings` name) the reply adds `cells`, one per byte read, from the engine's `neverd_decode_text_json`: a character's text at its first byte, `""` at its later bytes, and `null` where no character is shown. The bytes decode from the first one, so a character cut by the range's ends shows as `null`. An unknown name is `unsupported_encoding`; an engine without the decoder returns `unsupported`. |
| `analyze` | Computes the synchronous full-image pipeline once, publishes a new revision, invalidates adapter caches, and returns metadata. The listing then reads the jump tables the pipeline recovered (`neverd_switches_json`): a table is named `jpt_<dispatch>` and, in the form the engine verified, laid out a slot per line; its load and dispatch are commented, and every target is referred to from the dispatch (`j`) and the table (`o`). No function-level incremental analysis or cooperative cancellation is claimed. |
| `signatures_load` | `{path,mode:"file"}` loads one `.pat` or library feature `.json`; `mode:"auto"` selects from a signature tree including `features/rules`. Returns `{loaded:true,byte_matches}` and advances the analysis revision. Invalid packs preserve the previous evidence and revision. Available on read-only images; it does not write annotations or the binary. |
| `decompile` | `{address,representation:"c",offset:0,limit:512}`; representation `c/llvmc/low/med/high/llvm/source/cpp/rust/go`, line limit 1–2048. Triggers analysis when needed. Returns `{address,representation,text,offset,total_lines,next_offset,complete,mapping_status,provenance_complete,rows,project_id,revision}`. Native Low/Med/C/LLVMC use the optional bounded C ABI page API when present; other views retain one legacy function representation up to 32 MiB. C pages may also carry `byte_offset,function_identity,library_regions,prelude,recognition_budget_exhausted`. `cpp`, `rust` and `go` spell the HighC page in that language, for an image whose `language.pseudocode` lists it (otherwise `unavailable` with the engine's reason), and `source` in the function's own (C++, Rust, Go or C); they come only from the page API and add `dialect` (`c/cpp/rust/go`, what the page reads in), `unread` (why a declaration is shown as C) and `source_names` (`{begin_byte,end_byte,identifier,symbol,address?}` for each source name on the page, such as `core::fmt::write`, which splitting the text into identifiers would not find whole). A wire code page is capped at 2 MiB. See source mapping below. |
| `cfg` | `{address}`. Triggers analysis, returns `{name?,entry?,nodes,edges,complete:true}`. Nodes have `{id:string,start,end,insn_count,disasm:[string],address:start,label:start,lines:disasm}`; edges `{from:string,to:string|null,type}`. More than 500 nodes or 2000 edges returns `budget_exceeded`, with no partial graph. |
| `cfg_summary` | `{address,metrics?}`. Builds one immutable graph snapshot on the executor; returns counts, bounds and an opaque `layout_revision`, without node/edge arrays. Up to 20000 nodes / 100000 edges. Optional `metrics:{char_width,line_height,padding,...}` size each node to its formatted listing rows; the layered layout routes edges around nodes. See viewport contract below. |
| `cfg_viewport` | `{address,layout_revision,x,y,width,height,scale:1,node_offset:0,edge_offset:0}`. Queries the current snapshot's spatial indexes; returns at most 256 nodes and 512 edges. No engine call or relayout per pan. A missing/mismatched snapshot returns `stale_layout`. |
| `xrefs` | `{address,direction:"to",offset,limit:128}`, limit 1–512. Direct references from each instruction's own lift, without whole-program analysis: rows `{address,from,to,kind,type,text,function,function_address?}` where `kind` is call/jump/cjump/read/write/offset and `type` its classic letter (p/j/r/w/o). "to" reads the parallel reference index, finishing it first when the background build has not; "from" decodes the one instruction. Engines without `neverd_code_refs_json` return `unsupported`. `{source:"ir"}` selects the previous IR-constant references instead: it triggers analysis, preserves engine `from/to,func,block/opcode` and adds `{address,function,kind:"IR constant reference"}`; EVM/SBF return `unsupported` there. |
| `strings` | Standard page; the filter matches the text, the address or the type. Strings in the encodings and minimum length of `string_options` (engines without `neverd_strings_ex_json` find ASCII strings of at least 4 characters). Rows `{address,text,value,length,chars,encoding,type}`: `length` counts bytes without the terminator, `text` is UTF-8, `type` is `C` for ASCII or the encoding's listing spelling (`UTF-8`, `UTF-16LE`). Parsed and cached per revision. |
| `string_encodings` | Available before opening a file. Returns `{items:[{name,spelling,unit,default,legacy?}]}`, the engine's string encodings; `legacy:true` marks a code page (GBK, Big5, Shift-JIS, Windows-1252, ...). Engines without `neverd_string_encodings_json` return `unsupported`. |
| `string_options` | Available before opening a file. `{encodings?:[name],preferred?:name\|null,min_length?:1-1024}` replaces the options the listing and `strings` search with, increments revision and invalidates both. C strings that are not UTF-8 are read in every code page among the encodings; `preferred` names the code page that reads them first and so wins where others read them as text too, and is searched whether or not the encodings name it. Without it, a code page wins that reads a string in the scripts it is made for and that no code page for another script contests. `min_length` counts display columns, a wide East Asian character two. An unknown name is `unsupported_encoding` and a preferred encoding that is not a code page `invalid_request`. Without fields it only reads. Returns `{encodings,preferred,min_length}`. The options outlive the open file. |
| `string_references` | `{offset,limit:128,filter?,sort:"address",descending:false}`, limit 1–512; sort `address`, `text`, `function` or `type`. Every instruction that refers to a string under `string_options`, from the engine's `neverd_string_refs_json` (listed once per string options and function list): rows `{address,string_address,text,type,encoding,kind,disasm,function,function_address?,via?}`. A reference into a string reads its text from the first character that starts at or after that byte, when at least `min_length` columns remain; `string_address` is the referenced byte. An instruction that reads a relocated slot holding such an address refers to the string through it, and `via` names the slot. `disasm` is the instruction as the listing shows it. The filter matches the text, the function name or the address, ignoring case in any script; each query's filtered and sorted rows are kept for its pages. An engine without the query returns `unsupported`. Returns `{items,total,offset,next_offset,complete}`. |
| `listing` | `{address,sub:0,before:0,after:100,opcode_bytes:0}`; before/after 0–2000 lines, opcode bytes 0–12. The classic text listing of the whole image around an address: rows `{item,address,sub,cls,kind,prefix,text,spans,target?,flow?,function?,function_address?}` with `[byte_offset,byte_length,role,address?]` spans into the UTF-8 text (roles from `ListingRoles.def`, address classes `cls` from `AddressClasses.def`). Returns `{lines,anchor,at_start,at_end,generation}`. No analysis is started; functions decode lazily and the `generation` changes when labels or names can change. |
| `overview` | `{buckets:1024}`, 1–16384. The address space as equal linear buckets, each the dominant address class digit: `{buckets:string,total,regions:[{name,start,end,initialized_end,linear,exec}],generation}`. |
| `names` | Standard page by name, sortable: every function (kind `function`, `library` or `thunk`), data and string label and import slot as `{name,address,kind}`. |
| `regions` | Standard page by name, sortable: sections or segments as `{name,address,start,end,size,initialized_end,flags,alignment,class}`. |
| `imports` | Standard page by name, sortable: `{address,name,module,ordinal}` at each import's data slot. |
| `exports` | Standard page by name, sortable: exports `{address,name,ordinal,kind:"export"}` and entry points with their engine type. |
| `search` | `{kind:"text"|"bytes",pattern,case_sensitive:false,limit:256}`, limit 1–4096. Bytes are hexadecimal pairs with optional spaces. Returns `{items:[{address,...}],complete:true}`; a malformed pattern is `invalid_request`. |
| `segments` | Standard page by name; rows `{name,address,size,flags}`. `size` is the engine's hex string. |
| `annotations` | Standard page by text; rows `{address,text}`. |
| `annotation_set` | `{address,text}`; an empty string removes the comment. Stages an edit, increments revision, returns `{address,text,dirty:true,saved:false}`. Requires the writer lock. |
| `save` | Commits staged annotations and their command-history cursor through the recovery journal; returns `{saved:true,dirty:false}` after flushing. Requires the writer lock. |
| `rename` | `{address,name}`; any address: a function, data or a label. The name is not empty, has no spaces or control characters, is not an automatic form such as `sub_1234`, and is not used at another address. Requires clean annotations; never autosaves staged notes. Commits the rename plus history through the journal, reloads through the public C ABI, invalidates caches and increments revision. Returns `{address,name,saved:true}`. Requires the writer lock. |
| `code_edit` | `{address,representation,kind}` for a function's source view (`c/llvmc/source/cpp/rust/go`). `kind:"name"` adds `{original,name,identity}` from a current local `code_names` target; an empty name or the original name clears the alias. `kind:"comment"` adds `{line,anchor,text}` from a current source row; empty text removes it. Refuses obsolete/ambiguous targets, colliding or invalid local names, dirty annotations and read-only sessions. Commits the presentation edit and history, invalidates caches and increments revision. Returns `{address,saved}`, `saved:false` on no change. Image names use `rename` and mapped instruction comments use `annotation_set`. |
| `function_create` / `function_delete` | `{address}`. Capability present only with an engine that keeps function edits. Creates a function at the address, or deletes the one that starts there, as the engine allows (`<binary>.neverd-functions.json`). Requires clean annotations; commits the edit plus history, restarts function-level analysis and the reference index, and increments revision. Returns `{address,created,saved:true}`. Requires the writer lock. |
| `item_define` | `{address,action,size?}`. Capability present only with an engine that keeps data items. `action:"code"` decodes native file-backed executable bytes from the exact address up to a basic block boundary or existing code, without creating a function. Calls fall through; branches, returns and unmodelled flow end the block. At most 256 instructions; invalid or truncated bytes, ambiguous modes, and overlaps with another typed item fail atomically. Stores one validated `kind:"code"` row per instruction, with its actual size. `"data"` makes the item a value of `size` 1, 2, 4 or 8 bytes, or without a size the next of byte, word, dword and qword after the current one; `"string"` makes the string that starts there an item, read with the `string_options` encodings; `"undefine"` shows the item's bytes as undefined bytes. Data/string/undefine are refused on automatic function instructions; manually defined code can be replaced or undefined. Commits the items (`<binary>.neverd-items.json`) plus one history command, invalidates caches and increments revision. Returns `{address,kind,size,saved}`; code's size is the entire converted block. `saved:false` for already defined code or undefined bytes. Requires clean annotations and the writer lock. |
| `operand_format` | `{address,operand?:0-7,action}`. Capability present only with an engine that keeps operand formats. The address must start an instruction whose listing shows formats (x86 so far; other dialects are `unsupported`). A format changes an operand that is a plain number: `operand` when it is one, otherwise (or without `operand`) the instruction's last number; an instruction without one is `invalid_request`. `action` is a base of `include/neverd/OperandFormats.def` (`number`, `hex`, `decimal`, `binary`, `char`, `offset`; `number` also clears both flags), or `negate`/`invert` to toggle the sign change or the bitwise complement. Commits the formats (`<binary>.neverd-operands.json`) plus one history command; listing lines show the change without the listing being built again. Increments revision and returns `{address,operand,format:{base,negate,invert},saved}` with the operand that took the format, `saved:false` when nothing changed. Requires clean annotations and the writer lock. |
| `reload` | Explicitly reloads the sidecars, discards staged annotations, clears dirty state, invalidates caches and increments revision. Returns metadata. |
| `history` | Standard `offset/limit` page. Returns `{schema_version:1,items,total,cursor,offset,next_offset,complete,can_undo,can_redo,available,blocked_reason,source_sha256}`. Items contain `{kind:"annotation"|"rename"|"function"|"item"|"operand"|"code",address,before,after,index,applied}`. Hashing is lazy and runs on the Session execution thread; the existing dashboard API can require VM analysis. |
| `undo` / `redo` | No payload. Annotation changes/cursor movement remain dirty until Save. Rename, function, item, operand and code history changes require clean annotations and commit immediately. Return the history listing plus `{dirty,saved,address}`. All operations require the writer lock. |
| `history_reset` | Explicitly starts empty history from the current sidecars, preserving comments/renames. Requires clean annotations and the writer lock, then reloads their state. It never resolves a pending recovery journal or maps comments to a changed binary. |
| `contributions` | Available before opening a file. Returns `{schema_version:1,registry_revision:string,revision:string,items,complete:true}`. Each item has `{id,title,kind,namespace,version,query}` and optional table `columns`. The payload revision belongs to the registry, independently of the envelope's image revision. |
| `contribution_register` | `{path}`. Validates a local manifest completely before adding or replacing its namespace; returns the new contributions listing. Registration never executes a query or loads scripts/QML. |
| `contribution_unregister` | `{namespace}`. Removes that namespace and returns the new listing. The built-in `neverd` namespace is reserved. |
| `contribution_execute` | `{id,address?:hex}`. Executes that registered, whitelisted read-only query, replacing its exact `${address}` token with the supplied address or image entry. Returns `{contribution_id,operation,result}`. Arbitrary query/payload overrides are not supported. |

## IR and C source mapping

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
`<binary>.neverd-code.json`. An alias's complete source identity is a portable
presentation hash, not semantic evidence: a different engine rendering suppresses
the alias and sets `code_edits_stale`. A note requires its line and original
anchor to match. These edits do not change engine variables, types or instruction
semantics. They share the normal journal, undo/redo, replica validation and `.nddb`
packing. An older page API that rejects `source` as an unsupported representation
may supply its HighC page instead, reported as `representation:"source",dialect:"c"`.

A worker linked to an engine exporting the additive `neverd_ir_view_json`
operation uses its native Low/Med/C/LLVMC pages directly. The symbol is resolved from the
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
operations are `synthetic`. Those rows have empty address arrays. High and
LLVM mappings are `unsupported_representation`; VM mappings are
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

The binary is never modified. Existing `<binary>.neverd-annotations.json` and
`<binary>.neverd-renames.json` stay compatible with the CLI, as do the optional
`<binary>.neverd-functions.json`, `<binary>.neverd-items.json` and
`<binary>.neverd-operands.json` of engines that keep function edits, data items
and operand formats (`neverd function-edits`, `neverd items`,
`neverd operands`). The worker also owns
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

## Workbench names and background work

The `listing`, `functions`, `names` and `resolve` operations share one naming
layer for the workbench. Functions the engine leaves generic take classic
names: executable import veneers (ELF PLT entries and `.plt.got` stubs that
jump through an import slot) are thunks named after their import (`_` +
import on ELF), the image entry is `start`, a function at the start of `.init`
or `.fini` is `_init_proc` or `_term_proc`, and the first argument the start
routine passes to `__libc_start_main` is `main`. Engine, symbol and user names
always take precedence, and `resolve` accepts the workbench names. Import data
slots, including ELF `GLOB_DAT` entries from `neverd_import_slots_json`, are
named `<import>_ptr` on ELF. Code text from the engine (C and IR) keeps the
engine's spellings.

Heartbeats carry `background:{state,done,total,references,generation,functions}`
for the work the worker does while idle. It first lets the engine add the
functions its detector finds without lifting
(`neverd_session_discover_functions`); `functions` is the listed function count,
which clients compare to refresh function lists. It then builds the reference
index: `pending`, `building`, `ready` or `unavailable`. Its generation changes
when finished references can add labels and cross-reference comments to listing
text; clients refresh visible lines then. Function-level analysis runs only for
the function a decompile, CFG or IR request names; `analyze` remains the
explicit whole-program pipeline.
