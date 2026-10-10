# Web artifact and query schema, version 1

`lib/web` owns this schema, identity construction and publication rules.
CLI/SDK adapters must not reconstruct identities or infer additional analysis
claims. This schema describes metadata inspection and explicit local Bun
evidence export; findings, transform receipts and observations require separate
contracts.

## Identity and immutable evidence

Hashes are lowercase hexadecimal SHA-256. A blob hash covers exact bytes.
An occurrence/artifact ID also identifies its provenance: identical bytes
in different members remain different artifacts. Relocating the same admitted
tree does not change its identities; the selected host's absolute path is not
an identity input. A standalone root filename is likewise not part of identity.

The `web-identity-v1` hash preimage consists of UTF-8/byte fields, each preceded
by its unsigned 64-bit big-endian byte length: first
`neverd.web.identity.v1`, then the domain, then the ordered domain fields.
There are no separators, terminators or implicit JSON canonicalization.
Numeric identity fields use their canonical unsigned decimal representation.
Domain labels and field order are defined in
[`ArtifactStore.cpp`](../lib/web/ArtifactStore.cpp),
[`InputRoot.cpp`](../lib/web/InputRoot.cpp), and the owning source/map reader.

| Identity | Bound evidence |
|---|---|
| Snapshot/project | Sorted admitted tree, member bytes and kinds |
| Artifact occurrence | Snapshot/provenance, relative member identity and blob |
| Source unit | Artifact occurrence, parser-input blob hash (raw or explicit decoded projection), parser profile, explicit source type |
| Bun extraction | Original artifact occurrence/hash and explicit layout profile |
| Bun region | Extraction ID (already bound to the full original hash), region ordinal/kind and original offset/size; separate raw range hash retained |
| Bun module | Extraction ID and module-table ordinal |
| Bun source projection | Module ID, raw content region ID and strict Unicode decoding profile |
| ASAR extraction | Snapshot, original archive occurrence, explicitly selected unpacked directory or empty selection, ASAR profile and path-policy profile |
| ASAR member | Extraction identity and exact private logical member path; byte hash is separate |
| Electron manifest | Captured namespace, manifest occurrence/hash and entry profile; exact candidate identity is separate from runtime resolution |
| Source module origins | Source/binding/module analysis identities and origin profile; private member chains never become runtime object values |
| Electron source / boundary | Source/module/origin/value identities and profile; each boundary binds its original node and fixed operation kind |
| Electron IPC scope / channel / endpoint | Captured namespace, manifest, sorted source/evidence selection and profile; channel IDs bind scope plus first occurrence ordinal, endpoints bind original boundary IDs; no standalone channel hash |
| Captured path analysis / Electron entries | Source/binding/module/origin/value IDs, captured occurrence context, profile and canonical requested nodes; entry scope binds namespace/manifest plus sorted source/evidence/path IDs, each entry binds its original boundary |
| HTML document / script / base | Captured occurrence/hash plus HTML profile; script and base records bind tag offsets; reference values remain private |
| HTML inline source / reference links | Script identity, raw body hash and source type; links bind document and captured namespace; anchors preserve original parent storage and exact body offset |
| HTML inline module context / links | Context binds HTML document, script, source and HTML namespace-link identities plus module-link profile; link analysis adds module inventory and namespace IDs |
| Syntax node | Source ID, syntax kind, original byte start/length, traversal ordinal |
| Primitive value analysis | Source ID and fixed primitive-evaluator profile |
| Effect analysis | Source ID, binding/value analysis IDs and fixed effect profile |
| Module inventory | Source ID, binding analysis ID and module-evidence profile |
| Module occurrence | Module inventory ID, fixed kind, syntax node ID and traversal ordinal |
| Admitted module-file comparison | Module inventory ID, snapshot ID and file-comparison profile |
| Bundle analysis | Source ID, binding analysis ID and structural bundle profile |
| Bundle/partition/dependency/region occurrence | Bundle analysis ID, fixed record kind, original node ID and ordinal |
| Source map | Artifact occurrence, map blob hash, decoder profile |
| Mapped source | Map ID, nested section path and source-array index |
| Bun mapped source | Map ID and serialized source-array index, in the `bun-map-source` identity domain |
| Map segment | Map ID and stable generated-position-sorted ordinal |

Map-provided filenames and source text remain untrusted private evidence.
Equal source names never merge occurrences. An embedded source has a blob hash
and can be parsed by its derived artifact ID. A source reference without
`sourcesContent` has no content artifact and cannot be read through a host path.
Derived map sources are enumerated through `source_map_sources`, separately
from the original imported artifact inventory.

## Response rules

Every response has `schema_version: 1` and `status: "ok" | "error"`.
Ordinary failures contain a fixed `error.code`, never target filenames,
source text, parser messages or host exception text. A null C result means
allocation failure. All nonnull C results are owned by the caller.

Revisions and arbitrary byte sizes/offsets use decimal strings to avoid
loss in JSON clients. Bounded counts, page offsets and 32-bit map coordinates
use JSON integers. Hash IDs are opaque strings, not native addresses.

Every page requires the exact published revision, including its first page.
`offset` counts items, `limit` is 1–512, `next_offset` is an integer or null,
and `page_complete` indicates pagination only. A complete page does not mean
complete source semantics, map provenance or whole-project analysis.

| State | Meaning |
|---|---|
| `inventory_complete: true` | All admitted original entries were read consistently; any rejected entry fails the import |
| `analysis_status: not_analyzed` | No source/map/container/manifest result exists in this revision |
| `analysis_status: partial` | Some source/map/container inspection exists; no whole-program completeness claim |
| `parse_status: parsed` | The fixed parser's admitted checks succeeded |
| `validation: parser_checks` | Not full ECMAScript semantic validation |
| `semantic_analysis: not_analyzed` | No binding/effect/value analysis has been published |
| `semantic_analysis: partial` | Separate semantic-profile results exist; no complete semantics claim |
| `binding_status: ok` | Declared lexical-binding profile completed without a reported limitation; no runtime or whole-language validation claim |
| `binding_status: partial` | Known dynamic, compatibility or conflicting-declaration cases are retained explicitly |
| `binding_status: unsupported/budget_exceeded/unavailable` | No binding records are published |
| `value_status: ok/partial` in a summary | Finite primitive profile completed, possibly with per-node refusals; does not imply all expressions are constant |
| `effect_status: ok/partial` | Conservative may-effects were computed; not a reachability or rewrite proof |
| `value_status/effect_status: budget_exceeded/unsupported/unavailable` in a summary | That analysis publishes no per-node records |
| `module_status: ok/partial` | Syntactic inventory completed, possibly with explicit export/assertion conflicts; no runtime linking or reachability claim |
| `module_status: unsupported/budget_exceeded/unavailable` | No module inventory records are published |
| `link_status: ok` in a module summary | Every request received an admitted-file comparison or explicit refusal/boundary classification |
| `link_status: partial` in an HTML inline module summary | Explicit source/base/import-map candidates were compared; browser preparation, activation/history and runtime bases remain unverified |
| `exact_admitted_file_candidate` in a request | A literal allowed by the reported profile matches an admitted member; runtime module target is still unverified |
| `bundle_status: ok/partial` | Qualified structural partitions exist; partial retains explicit refused candidates or binding limitations |
| `bundle_status: not_detected` | No qualified layout found; original source is still available |
| `bundle_status: unsupported/budget_exceeded/unavailable` | No recovered bundle records are published |
| `format_status: decoded` | The map satisfies the admitted decoder profile |
| `association_status: unbound` | No generated-source association has been established |
| `association_kind: caller_assertion` | Lookup uses the caller's chosen source, not authenticated producer evidence |
| `association_status/association_kind: container_assertion` | A Bun module record names this map and generated source; provenance is still unverified |
| `mapping_coverage: retained_mapped_anchors_only` | Bun discarded unmapped boundaries; preceding anchors cannot establish intervening mapping scope |
| `checked_scope: returned_anchors` | Position checks cover returned anchors only |

## Explicit local Bun export

`bun-local-evidence-export-v1` returns counts, hashes, the selected layout
profile, entry index and fixed verification claims. `status:ok` means export
completed; `unavailable_source_count`, `parsed_source_count` and
`readable_source_count` independently describe projections and parser coverage.
It never means that original TypeScript or native code has been recovered.
`readable_source_profile` is null when the parser is disabled.

The explicitly requested local `manifest.json` contains raw virtual names,
generated relative filenames, exact original offsets and storage hashes, plus
separate decoded/readable hashes. `source_status:decoded_exact` describes the
strict encoding projection; `source_status:asset` retains opaque data.
Other fixed source failure codes retain raw storage without a substitute JS
file. `readable_status:verified_same_parser_tree` means whitespace insertion
passed a comparison of the retained parser trees. A parser failure preserves
`parse_status` and bounded `parse_diagnostics` with fixed codes and
`original_utf8_byte_offset` as a decimal string or null. Those offsets belong
to decoded original JS, not the native container or the readable file.

The local manifest and HTML index are deliberate raw disclosure and are not
ordinary metadata-only responses. Original virtual names never select output
paths. The index escapes names, executes no script and records its own hash
in the manifest. A manifest hash in the API response binds the exact completed
manifest bytes; it does not authenticate publisher signatures.

## Private metadata policy

`metadata-only-v1` excludes raw member names, identifiers, literals, URLs,
source roots and contents from ordinary metadata operations. `name_redacted` records
that omission. Hashes, sizes, counts and syntax shapes remain visible; this
policy is not an anonymity guarantee or automatic secret detector.

Module summaries and pages carry `link_profile` and nullable `link_context`.
For `html-inline-module-file-candidates-v2`, the context includes `context_id`,
`kind: html_inline_source_candidate`, `html_id`, `html_script_id`, nullable
`preceding_base_id`, `base_status`, `import_map_status` and
`import_map_analysis_id`. It always reports `runtime_base_verified: false`.
`module_url_identity` is `not_modeled` without maps, or
`capture_context_url_candidate` for the admitted source-order map profile.
Requests add nullable `query_present` and `fragment_present`; null means no
admitted URL comparison supplied that metadata. Values stay private. An import
map refusal/budget/timing/origin boundary has no candidate artifact ID.
Requests additionally carry nullable `module_url_candidate_id`, `import_map_id`,
`import_map_entry_id`, `import_map_scope_id` and `import_map_match`
(`exact`, `prefix`, `default_url`). URL IDs group private normalized URLs within
one source/context and are not runtime module identities. Unadmitted file URLs
have no URL ID. Ordinary external-file analyses retain null
context and presence fields. Equal target artifact IDs establish a shared
captured file occurrence, not equivalent runtime module URLs. See the
[HTML profile](web-html-profile.md#inline-module-file-candidates).

HTML summaries add `import_map_analysis_id`, `import_map_profile`,
`import_map_analysis`, `import_map_count`, `import_map_steps`,
`import_map_reason` and `import_map_activation_verified: false`.
`html_records` accepts `import_maps` and retains normal pagination/revision rules.
Each item reports a declaring `script_id`, whole-body range, fixed status/reason,
base status, record/import/scope/ignored-key counts, integrity presence/count,
origin-dependent-key presence and normalization work. Raw map text, addresses,
keys, scopes and integrity values remain private; integrity/activation flags are
always false. Individual-map refusal remains visible in a partial inventory.

ASAR summaries use `profile:asar-pickle-json-v1`,
`path_policy:icu-77.1-nfc-casefold-portable-v1`,
`layout_status:compatible` and `coverage:declared_members`.
`extraction_status:complete/partial` describes availability of declared
non-directory members, not full application analysis or publisher trust.
`unreferenced_payload_bytes` counts gaps/trailing data outside packed members;
the original archive preserves those bytes. Counts are bounded integers;
`data_offset`, member `byte_offset/byte_length`, and unreferenced byte counts
are decimal strings. `unpacked_directory_id` is null when none was selected.

`asar_records` returns `member_id,parent_id,member_index,kind,availability,
storage,byte_length,storage_artifact_id,byte_offset,blob_sha256,integrity_status,
executable_claim,name_redacted,link_target_redacted`. The root parent ID is the
extraction ID. Directory/link records provide no usable bytes; a file with
`availability:available` is an accepted source/map/native selection. Storage
offsets refer to the captured archive for packed files and the actual captured
file for unpacked files. Missing storage has null location/hash fields.
Integrity states and per-member refusal reasons are in the
[profile](web-asar-profile.md). A mismatched member may retain the hash of its
actual bytes but is still unavailable to consumers.

ASAR source-anchor/native origins include the archive/extraction/member IDs,
actual `storage_artifact_id`, packed/unpacked kind and selected directory when
applicable. Member SHA-256 refers to actual bytes; `authenticates_publisher`
remains false. Relative source imports compare only available files from that
same extraction, with no host lookup or runtime-resolution claim.

Navigation identity binds source ID, `javascript-source-navigation-v1`, binding
analysis ID and binding status. Function/call identities additionally bind their
syntax node IDs. Reference identities are reused from the binding analysis.
Function containment and declaration/initializer links are source relationships;
`runtime_call_graph: not_analyzed` and `runtime_target: not_proven` prohibit treating
them as resolved runtime calls. Failed navigation publishes no records. Partial
navigation may retain complete syntax with unavailable lexical evidence.

`source-storage-anchor-v1` identities bind the source and requested UTF-8 span.
`start/end` use zero-based line/UTF-16 coordinates; `source_parse_status` does not
affect the separate claim of coordinate validity. `storage` identifies original
bytes (`byte_identity`), exact Bun encoding-boundary conversion
(`unicode_boundary_conversion`), a containing Zstd frame
(`containing_compressed_frame`) or a decoded JSON source without encoded-member
positions (`encoded_member_not_located`, offsets null). Hash fields distinguish
decoded source, stored member and compressed frame. A committed `view` contributes
its own view/policy identities and a byte-identity, exact-boundary or region-cover
mapping. `source_cover_byte_offset/length` records any hidden-region expansion;
raw source text is never part of an anchor response.

Explicit source views use `structural-with-reviewed-ranges-v1` and the
`javascript-source-view-v1` display profile. Policy identity hashes the sorted
reviewed byte ranges and profile. View identity additionally binds the source,
lexer profile and binding analysis ID/status. Preview tokens bind the view,
published revision and session preview sequence; they are confirmation tokens,
not authentication credentials. Segment identity binds the view and ordinal.
Previews and segment pages contain no text. Only a committed view's chunk
operation returns `text`, marked `content_role: untrusted_target_source` and
`semantic_rewrite: false`. Nonempty reviewed ranges are a `caller_assertion` of
local review. Default views expose only fixed syntax/layout and replacement labels.
`source_byte_offset/length` and `view_byte_offset/length` are decimal strings;
`mapping: byte_identity` is exact while `whole_region` forbids interpolation.
Source coordinates and `source_sha256` belong to the analyzed UTF-8 artifact,
not the original encoded Bun member or compressed source-map frame.
`first_segment/last_segment_exclusive` select the metadata records intersecting
a returned chunk. A policy replacement revokes the old view within that session.

Decoded Bun map summaries use profile `bun-1.4.2-serialized-source-map-v1`.
`artifact_id` refers to the raw map region; `storage` links the containing
artifact, extraction/module IDs and original map/mapping offsets and sizes.
`generated_artifact_id` identifies the module's decoded UTF-8 projection.
Map sources carry `origin_kind: bun_source_map_zstd_content` and a `storage`
record with original name/content offsets/sizes and compressed content hash.
Their ordinary `blob_sha256` covers decoded UTF-8 bytes. Name hashes stay
redacted. All storage offsets/sizes are decimal strings in the original
container coordinate system, never offsets in the decoded source.

Bun's dropped name/unmapped/root/file/ignore metadata is reported as
`discarded_by_producer`. Source `ignored_claim` and segment `name_present`
are null for this profile. `mapping_scope` on lookup is
`anchor_only_intervening_boundaries_unknown`; exact and preceding selection
still describe anchor selection only. `provenance_verified` remains false.

Binding identities are derived from the source ID and
`javascript-lexical-bindings-v1`. Scope, binding and reference IDs use distinct
identity domains; declaration rows link node IDs to binding IDs. Binding names
are private UTF-16 data and never JSON fields. The four record kinds are paged
separately; declarations are not embedded as an unbounded list inside a binding.
`node_id: null` denotes an implicit binding's declaration, and `binding_id: null`
on a reference denotes absent or uncertain resolution. Root scopes have null
parents. Every record is tied to one source and one published revision.

Semantic pages are indexed by syntax occurrences. Each row links a `node_id`
and original byte span to `value_status`, a fixed `value_reason`, optional
`value_kind`, and `value_redacted: true`. No scalar value is serialized.
`immediate_effects` and `deferred_effects` contain analyzer-owned category names;
`contains_declaration` is a syntactic preservation flag, independent of branch
selection. If values exist but effects are unavailable, effect fields are null
rather than empty lists or false. Summary diagnostics and step/storage counts
are separate for the two analyses. `page_complete` still means pagination only.

Module pages use `record_kind: requests/imports/exports/attributes`, `record_id`,
`node_id`, original byte spans and `names_redacted: true`. Imports/exports link
lexical binding IDs and request IDs; foreign exports stay unlinked. Request
rows preserve a private specifier node, literal/dynamic/missing classification,
callee identity/evidence, optional-call flag, dynamic-options node and a
candidate artifact ID or null. `runtime_target_verified` is always false.
Static import/export requests have no callee or runtime argument list;
their `argument_count` is zero. Request link status is `unavailable` with null
candidate ID when link analysis exhausted its budget. Inventory and link
budgets are independent. No module record exports a source name or path.

Bundle pages use `record_kind: bundles/modules/dependencies/regions` and expose
only IDs, kinds, original byte spans, wrapper/body hashes and fixed uncertainty
categories. Module records are partitions of the source unit, not new source
files; `module_key_redacted: true` suppresses table keys. `removed_slot` has no
body and reports null body hash/positions. A dependency's `caller_module_id`
is null outside a recovered factory; this never establishes an entry module.
Unknown, absent, dynamically resolved, reassigned or removed targets have null
`target_module_id`. Even `table_member_candidate` retains
`runtime_target_verified: false`. Summary `producer_verified` and
`authorizes_source_rewrites` are false in this profile.

## Electron evidence

Electron manifest results retain `artifact_id`, `blob_sha256`, immutable `origin`
and `main_artifact_id` only for an exact admitted file candidate. Main paths,
package names and declared versions remain private. `framework_verified`,
`runtime_version_verified` and `runtime_entry_verified` are false. Default,
declared, unsupported-type, no-directory, missing-exact-file and unsafe-path
states remain distinct. The selected profile does not detect a distribution.

Electron source results bind `electron_source_id` to their source/module/origin/
value evidence and publish `analysis_status: partial/budget_exceeded/unavailable`.
Origin and value statuses are separate. Boundary pages retain exact source
node/range IDs, module request index and its analysis ID, construction/value/
callback-or-API nodes, fixed operation kind, optionality and private value status.
`runtime_target_verified` is false and reachability is not analyzed. There is
no exported channel value/hash or implicit correlation across applications.
Budget failure publishes no boundary records. Caches share revision invalidation.

`electron_ipc_id` binds an explicitly selected manifest directory namespace and
the sorted source/evidence selection, including its main candidate. All selected
sources must be exact captured members in that subtree. Scope membership is a
caller assertion, not a verified runtime process or module graph. `sources`
pages preserve each source's analysis status; `endpoints` join boundary and source
node/range IDs, with a nullable scope-specific `channel_id`. `channels` count
fixed operation categories and compatible candidate pairs. Equality is exact
UTF-16, retaining NUL and lone surrogate distinctions without Unicode folding.
No plaintext or isolated hash of the channel is emitted. Removal calls remain
evidence and never subtract handlers without execution-order proof. Dynamic
values have null channels. Budget failure publishes no correlation cache;
`selection_complete`, `runtime_routing_verified` and `process_roles_verified`
remain false. See the [profile](web-electron-profile.md).

`electron_entries_id` uses the same explicit manifest/source scope as IPC but
does not require an IPC result. Sources publish path-analysis identities,
status and fixed reasons. Entry records retain their boundary, construction,
value, path-root and path-operation node links and original byte ranges.
`target_artifact_id` identifies an exact captured file candidate;
`selected_target_source_id` is non-null only if that occurrence was explicitly
included in the source selection. Missing, unsafe, dynamic and external paths
retain fixed refusal states. No path, URL or isolated path hash is exposed.
Import replacement invalidates all entry scopes. `runtime_entries_verified`,
`runtime_path_bases_verified` and `selection_complete` remain false;
`html_analysis` remains `not_analyzed`. Path-source budget failure publishes
its status with no partial path nodes; aggregate failure publishes no cache.

## Native handoff

`immutable-native-handoff-v1` binds a `handoff_id` to project ID, captured web
revision, selection ID and complete blob hash. `origin` records an original
artifact/parent occurrence, a Bun container/extraction/module/region and exact
asset byte range, or an ASAR extraction/member/declaration and its actual byte
storage occurrence. Packed ASAR offsets identify archive bytes; unpacked offsets
identify the explicitly selected captured file. Unavailable members cannot be
selected. Byte length and base/entry addresses are decimal strings.
Format, architecture, instruction mode, bitness and native counts come from the
shared native loader. `platform: not_inferred` and
`producer_version_verified: false` retain uncertainty independently.

Opening is explicit, metadata-only and does not run the full pipeline.
`native_analyze` is a separate static operation; `pipeline_status` is
`not_run`, `succeeded` or `failed`, and never attests whole-application semantics.
The separate native handle owns its image independently of the web session.
`host_path` is null; companion discovery, sidecar persistence and target execution
are false. Ordinary native file reload clears the handoff on successful
publication. Worker import replacement additionally revokes its retained handle.
Snapshot sessions reject path-based annotation, rename, function, data-item,
operand-format and load-option persistence. The standard input-SHA query uses
the captured digest and cannot reopen a missing snapshot path. File-specific
loader overrides and implicit universal Mach-O slice choices are unavailable
at the immutable-buffer loading boundary.
Standard native SDK queries keep their existing disclosure contracts; the
handoff report never embeds those queries' source text, names or diagnostics.

## Transactions and bounds

A preview captures candidate bytes without replacing the published project,
then releases the candidate spool and retains its digest for comparison.
A new preview invalidates an old token, including when the new preview fails.
Commit consumes its token, recaptures, then publishes only an identical snapshot.
Failure preserves the prior revision and its caches. Successful commit increments
the revision and discards prior source/map caches. Stored evidence never follows
later edits to the selected host file.

`capabilities.artifact_storage` is `posix-unlinked-spool-v1` on an admitted
POSIX host and `unavailable` on Windows. Storage location, descriptors and spool
offsets never affect content/occurrence IDs. `limits.max_blob_read_bytes`,
`capture_buffer_bytes` and `max_session_spool_bytes` are decimal strings; the
last bounds original-byte storage for one session's published and pending
captures only. A blob slice retains its owner's lifetime, while its range is
checked with subtraction before allocation or offset arithmetic. No API returns
a live view of an input file or exposes a raw blob-read operation.

Inputs and queries have admission limits reported by `capabilities`; there is
no silent truncation. JSON structural preflight bounds depth, nodes, strings
and bytes before DOM creation, and rejects decoded duplicate object keys.
The current POSIX reader rejects links, special files, unsafe names and detected
changes. It does not yet provide export-path normalization or Windows admission.
