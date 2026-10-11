# Offline web analysis

Explicitly selected SSE, JSONL, JSON-RPC/MCP-shape and diagnostic-log inputs
use `neverd web stream-preview` and receipt-bound `stream-import`.
See the [stream profiles](web-stream-profile.md) for framing, redaction,
recorded-context joins and limitations.

Passive HAR/source interface commands and their explicit redaction preview
workflow are documented in the [interface profile](web-interface-profile.md).
Use `neverd web har-preview` before hash-bound `har-import` or
`interface-correlate`; `neverd web interfaces` supports source-only candidates.

The `neverd web` command and independent C API inspect admitted files without
executing them. The current implementation provides immutable artifact
inventory, JavaScript syntax and lexical-binding queries, bounded primitive
values, conservative effect metadata, module evidence, qualified bundle partitions,
reviewed source views and source-map decoding. Versioned Bun ELF/Mach-O/PE profiles preserve source, asset,
map and cache ranges and feeds decoded source into the same analysis API.
The ASAR reader admits packed members and explicitly associated captured
unpacked files, with shared source/map/native selection and integrity checks.
The [HTML source profile](web-html-profile.md) inventories captured script/base
declarations and local file candidates, and supplies exact inline byte slices
to the shared source and storage-anchor APIs. Inline module requests use explicit
document/base candidates for captured-file comparison. DOM construction, runtime
activation, import maps and full application import closure remain unverified.
The wider
JavaScript, package, desktop and protocol work remains in progress; see the
[implementation ledger](web-analysis-implementation.md) and
[support matrix](web-support-matrix.md).

```console
neverd web capabilities
neverd web packages ./package-lock.json npm-lock
neverd web packages ./package.json package-json
neverd web archive ./package.tgz tgz
neverd web bun-export ./standalone-elf ./new-recovery-directory
neverd web inspect ./input-directory
neverd web source ./bundle.js script
neverd web source ./entry.mjs module
neverd web source ./entry.cjs commonjs
neverd web bindings ./bundle.js script
neverd web semantics ./bundle.js script
neverd web modules ./entry.mjs module
neverd web bundles ./webpack-bundle.js script
neverd web view ./bundle.js script
neverd web navigate ./bundle.js script
neverd web anchor ./bundle.js script 0 16
neverd web map ./bundle.js.map
neverd web bun ./standalone-linux-x64
neverd web bun-map ./standalone-linux-x64 0
neverd web bun-navigate ./standalone-linux-x64 0 module
neverd web bun-view ./standalone-linux-x64 0 module
neverd web bun-anchor ./standalone-linux-x64 0 module 0 16
neverd web native ./standalone-linux-x64
neverd web native-analyze ./selected-helper
neverd web bun-native ./standalone-linux-x64 2
neverd web bun-native-analyze ./standalone-linux-x64 2
neverd web asar ./app.asar
neverd web asar-source ./app.asar 0 - 0 module
neverd web asar-native ./app.asar 0 - 2
neverd web html ./index.html 0
neverd web html-source ./captured-app 2 0
neverd web html-modules ./captured-app 2 0
neverd web asar-html ./app.asar 0 - 1
neverd web asar-html-modules ./app.asar 0 - 1 0
```

Successful commands emit newline-delimited JSON. A source command with rejected
syntax emits the diagnostic summary and returns a nonzero exit status. Import
errors preserve any previously published SDK project. Ordinary output contains
identities, hashes, sizes, syntax kinds and positions. Paths, identifiers,
literal values, map URLs and embedded source text are omitted under
`metadata-only-v1`. The explicit `view` command instead emits a structural
display projection: target names, values and comments are replaced with labels.
It does not emit executable transformed JavaScript. The separate `bun-export`
command explicitly discloses raw source and names to a new local directory;
its console response remains metadata-only. It writes exact captured bytes,
strictly decoded JS, verified readable copies where supported, a manifest and
an inert HTML index. See [local Bun export](web-bun-profile.md#local-evidence-export).

All new analysis and adapter code is C++. JavaScript parsing embeds a pinned
Hermes parser/AST subset. There is no JavaScript VM, target evaluation, package
installation, plugin loading, subprocess analyzer or automatic network/file
resolution. Input bytes are data, including `eval`, `require`, imports,
source-map references and executable configuration files.

The [package evidence profile](web-package-profile.md) adds explicitly selected
npm v1/v2/v3 lockfile and manifest inspection, bounded graph pages and comparisons
of two supplied roots. Placement candidates, integrity declarations, missing
evidence and version/platform uncertainty remain separate; it neither installs
packages nor issues a benignness verdict.
The [archive and integrity profiles](web-package-archive-profile.md) admit
bounded tar/local-PAX/single-gzip members and compare explicitly selected
original artifacts with captured registry or npm-lock SRI declarations.
Source, package, Bun and native consumers share the immutable member selection.
SHA equality, archive validity and publisher authenticity are separate claims.

Bun extraction is explicitly selected and follows
[versioned layouts](web-bun-profile.md). Module/region pages preserve original
container offsets and hashes; derived source IDs select strict UTF-8 decoding
for `source_analyze`. The profile does not authenticate the compiler version.
Bun serialized maps use the existing map API on explicit request, with a
bounded native Zstd decoder. Their retained anchors, compressed ranges and
decoded-source identities remain separate. The producer's lost names/unmapped
boundaries are reported explicitly; JSC caches remain opaque. The original
container and native asset members can be selected for the explicit native
handoff described below. Raw export is available through the CLI and C API;
the worker does not expose filesystem export. Bun input profiles cover Linux,
macOS and Windows on x64/ARM64; the [matrix](web-bun-profile.md) distinguishes
input formats from host qualification and remaining unsupported layouts.

## SDK lifecycle

ASAR commands use an original artifact index, an optional captured-directory
index (`-` for none), and a member index. Indices must come from the current
input's inventory, not an assumed filename order. The C API and worker instead
use revision-bound opaque IDs. Missing unpacked data, unverified claims, links,
unreferenced payload bytes and integrity failures remain explicit; unavailable
members cannot be parsed or handed to the native loader. See the
[ASAR profile](web-asar-profile.md) for syntax, states and limits.

Include [`NeverDCAPIWeb.h`](../include/neverd/sdk/NeverDCAPIWeb.h) explicitly.
The opaque `neverd_web_session_t` is independent of the native binary session.
Return strings use `neverd_free_string()` from the included SDK session header.
No Python binding for this domain is currently claimed.

The C++ worker exposes the same operations with the `web_` prefix and keeps web
and native project state separate. Its payload and envelope contract is in the
[worker protocol](../tools/neverd-worker/PROTOCOL.md#offline-web-operations).
The optional native C++ [web MCP adapter](web-mcp-profile.md) exposes the
supported offline tools through the shared worker/C API adapter. Its launch
configuration supplies an explicit input scope; tool discovery follows the
loaded backend capabilities. It does not expose raw export or reviewed source
ranges. Build with `NEVERD_BUILD_WEB_MCP=ON`.

1. Create a session and inspect `neverd_web_capabilities_json()`.
2. Preview an explicitly selected file/directory. Empty options use the
   reported limits; nonempty options require `schema_version: 1`.
3. Commit the preview token. The reader recaptures the input and compares the
   snapshot identity before publication. A commit attempt consumes the token.
4. Query artifact pages using the exact decimal-string revision. Submit an
   artifact ID and explicit source type to `source_analyze` or submit a map
   artifact ID to `source_map_analyze`.
   For a Bun module, use its `source_map_region_id` after `bun_extract` and
   check the reported Zstd capability. This is a container association claim,
   not verified provenance.
5. Query source nodes or map sources/segments in pages of 1–512 entries.
   Embedded map sources have derived artifact IDs that `source_analyze` accepts.
   For an analyzed source ID, call `source_bindings_analyze`, then
   `source_binding_records` with `record_kind` equal to `scopes`, `bindings`,
   `declarations` or `references`.
   `source_semantics_analyze` computes finite values and immediate/deferred
   effects, admitting bindings if needed. `source_semantic_records` pages the
   per-node metadata; it never serializes the computed values.
   `source_modules_analyze` inventories module evidence and compares static
   relative specifiers with the published directory snapshot, admitting bindings
   if needed. Derived HTML inline sources automatically use their explicit
   document/base profile, including literal dynamic-import candidates and import-map
   refusal. `source_module_records` pages `requests`, `imports`, `exports` or
   `attributes`. It never opens a referenced path.
   `source_bundles_analyze` recovers the supported loader/table profile;
   `source_bundle_records` pages `bundles`, `modules`, `dependencies` or `regions`.
6. For source viewing, preview a policy, inspect its range metadata, commit its
   token and read bounded text chunks as described below.
7. Free every owned response. Destroy the session after outstanding calls finish.

Session calls are synchronous and serialized. This interface currently has no
active cancellation operation. A parser allocation hook limits its arena and
checks a cooperative deadline; it does not enforce a whole-process RSS limit.
All caches are scoped to the published revision and cleared on successful import.

## Reviewed source views

`javascript-source-view-v1` uses the embedded parser's retained token/comment
spans (`hermes-602befee-lexemes-v1`). The default policy preserves fixed syntax
and ASCII whitespace only. Identifier names, property names (including keyword
spellings), numeric/string/regex/template literals, booleans, null, comments and
hashbangs are replaced. Unclassified tokens and unusual layout are replaced too.
Unambiguous lexical binding occurrences share `binding_N` labels; unresolved,
dynamic or unsupported binding evidence uses occurrence labels. These are
display aliases, not semantics-preserving renames. Source structure, counts,
positions and hashes remain visible; this is not an anonymity guarantee.

The four C API operations also exist as `web_` worker operations:

1. `source_view_preview(revision, source_id, options)` returns metadata only:
   source/hash/policy/view identities, region classes, hidden-region count,
   reviewed byte count/ranges and a preview token. Empty options select the
   default structural policy. A new admitted preview attempt replaces the
   pending candidate; a rejected policy cannot leave its earlier token valid.
2. `source_view_records(revision, view_id, offset, limit)` returns original and
   projected byte ranges for review, including before commit. `byte_identity`
   permits bytewise mapping. `whole_region` maps a replacement to its entire
   original range and must not be interpolated. Source ranges refer to the
   analyzed UTF-8 source artifact, including decoded Bun sources; they are not
   encoded container offsets. Use `source_anchor` for storage coordinates.
3. `source_view_commit(revision, preview_token)` consumes the pending candidate
   on every current-revision attempt. Only a correct token publishes it.
   Replacing a source's policy makes its previous view unavailable unless that
   policy is explicitly published again. Failed commits preserve the current view.
4. `source_view_chunk(revision, view_id, byte_offset, byte_limit)` reads only a
   committed projection. Chunks are at most 65,536 bytes, end on UTF-8/CRLF
   boundaries and identify the segment interval needed to map their positions.
   A limit too small to fit the next scalar or CRLF returns a fixed error.

To disclose original regions locally, pass a schema-1 policy with
`locally_reviewed: true` and `reviewed_ranges`, each containing canonical
decimal-string `byte_offset` and `byte_length`. Up to 64 nonoverlapping ranges
are accepted. Ranges must cover complete lexical or layout regions; partial
tokens, partial layout regions, UTF-8 scalars and CRLF pairs are refused.
The preview does not disclose these bytes; a subsequent commit is required.
The SDK treats local review as a **caller assertion**, not proof of a human's
approval or authorization to send that text to a model or network service.
Returned text is marked `untrusted_target_source` and must remain inert data.
The CLI selects only the default structural policy and provides no raw reveal flag.

The original source and comments/licenses remain immutable. Views are neither
exports nor transformation receipts. Each view is limited to 4 MiB and 400,001
segments; each session retains at most two published source views and one
pending candidate. Import commit clears all views/tokens. Token retention is
bounded separately from AST validity (200,000 lexer entries including EOF and
comments at admission, 400,000 retained entries across source caches).
A source can remain parsed while `lexeme_status` refuses source viewing.
These are per-session retained-data limits, not whole-process RSS guarantees.

## Source navigation and storage anchors

`source_navigation_analyze(revision, source_id)` builds the bounded
`javascript-source-navigation-v1` index, admitting lexical bindings if needed.
`source_navigation_records` pages three record kinds: `functions`, `calls` and
`references`. Each row retains its AST identity and original UTF-8 byte range.
Functions expose source containment, body/name nodes, parameter count and
declaration/initializer binding links. Calls expose the syntactic callee and
its lexical reference/binding when available. Direct function expressions have
a `syntactic_function_id`. Dynamic imports expose their source argument rather
than mislabeling that expression as a callee. References link back to the same
binding/reference IDs as the binding API and to their containing source function.

Containment describes source syntax, including parameter defaults and deferred
class fields; it does not describe runtime activation. Initializer links remain
source evidence after reassignment. `runtime_target: not_proven` and
`runtime_call_graph: not_analyzed` are explicit. There is no property-call,
tool-registration or API attribution based only on a name. If binding analysis
is unavailable, syntax records remain available with partial status and absent
lexical links. A navigation failure publishes no partial index. Queries are
cached once per source/revision and share the source/node limits.

`source_anchor(revision, source_id, byte_offset, byte_length, optional_view_id)`
returns metadata only: zero-based line/UTF-16 columns, original storage and an
optional projection into a committed view. Endpoints must be UTF-8 scalar and
CRLF boundaries; zero-length selections and EOF are valid. The returned parse
status remains distinct from coordinate validity. No source-map association or
producer authenticity is inferred from this query.

| Storage mapping | Meaning |
|---|---|
| `byte_identity` | Exact bytes in the original admitted source, an available ASAR member, or a Bun client member validated as UTF-8 |
| `unicode_boundary_conversion` | Exact original range using the same Latin-1/UTF-16LE decoder as Bun source extraction; surrogate pairs are indivisible and terminators are excluded |
| `containing_compressed_frame` | Complete Bun source-map Zstd frame; there is no per-character compressed offset |
| `encoded_member_not_located` | A decoded JSON `sourcesContent` member; map/source identities are retained, raw encoded offsets are null |

A supplied view must be committed for the same source. Its `byte_identity`
range is exact, `exact_boundary` identifies a caret position, and `region_cover`
selects whole replaced regions at hidden endpoints. A caret inside a hidden
literal therefore selects its label, not an invented character within that label.
The returned source-cover range records any expansion. Replaced view policies
and stale revisions cannot be used for navigation.

`navigate` emits the three metadata inventories. `anchor` additionally creates
the default structural view and emits at most 64 KiB of its selected range.
`bun-navigate`, `bun-view` and `bun-anchor` select an explicit zero-based module
index and source type through the same in-process pipeline; no temporary JS
file or external extractor is involved. SDK/worker queries support the same
operations and precision distinctions. Runtime dataflow and feature-specific
tool/configuration/permission attribution remain separate work.

## Electron evidence

Electron manifest and source queries are independent, metadata-only consumers.
`electron-manifest` compares an explicit manifest's entry with available members
in its captured directory or ASAR namespace. `electron-source`, `asar-electron`
and `bun-electron` inspect source-visible window, renderer, IPC and contextBridge
candidates through the shared C++ binding/module/value owners. Source names do
not authenticate APIs; runtime targets and whole-application coverage remain
unproven. See the [Electron profile](web-electron-profile.md) for commands,
limits, refusal cases and required remaining application-level work.

`electron-ipc <root> <manifest-index> <source-index:source-type>...` and
`asar-ipc <root> <archive-index> <unpacked-index|-> <manifest-member-index>
<source-member-index:source-type>...` compare channels across explicitly selected
sources under one manifest. Include its main candidate with the manifest's
source-type candidate. The C API/worker accept existing source IDs, after
manifest and source evidence analysis. Results provide source, channel and
endpoint pages with exact original evidence links. They compare private UTF-16
constants, preserve unresolved calls and never prove runtime routing, process
roles, reachability or completeness. No package entry or source is executed.

`electron-entries` and `asar-entries` use the same positional selection as
their IPC counterparts. They compare preload and renderer path expressions
with exact captured members and page sources/entries. Relative loadFile paths
use the selected application directory; dirname-based paths retain the source
member's directory. A relative preload literal remains unresolved. Candidate
target IDs and original evidence links are visible; paths and URLs are private.
No runtime path-base verification or automatic HTML/import analysis is implied.

## Explicit native handoff

`native_open(revision, selection_id)` selects an original file occurrence,
an extracted Bun asset region, or an available ASAR member. Packed ASAR members
retain their exact archive range; unpacked members retain both their archive
declaration and actual captured-file occurrence. A Bun runtime must be selected as its entire
original container; native prefixes, JSC caches, encoded source and map regions
are not independent native images. The selected bytes are copied from the
immutable spool into a bounded native input buffer and the loader's complete
SHA-256 is checked against the captured occurrence. No host path is reopened.
PE/COFF, ELF and thin Mach-O use the same format readers as ordinary native
loads. Universal Mach-O is refused until the caller can select a slice explicitly.
Format and architecture do not authenticate a producer or establish an OS.

The C ABI returns a new owned `neverd_session_t` through an output parameter
only with a successful, allocated JSON response. Destroy it with
`neverd_session_destroy`. It remains usable after the web session changes or
is destroyed. The handoff ID binds the source project/revision, selected
occurrence, bytes and handoff profile; it denotes that evidence, not a process
instance. `native_metadata` reports this provenance and native counts without
names or raw strings. `native_analyze` explicitly runs the existing static
native pipeline. Its `pipeline_status` is separate from whole-app coverage;
decompiled C is not claimed as recovered original Rust, Go or JavaScript.

Snapshot-backed native sessions have no file path. They do not discover debug
companions or load sidecars. Path-dependent annotations/rename persistence and
binary patching fail explicitly. An explicit successful ordinary native file
reload switches back to the normal file contract and clears handoff provenance;
a failed reload preserves the snapshot. Native SDK queries can expose original
symbols and strings, so callers must apply their disclosure policy when using
those queries. Web handoff reports themselves remain metadata-only.

The worker retains one separate handoff alongside its web session, without
replacing its ordinary native project. Failed opens preserve that handoff;
successful replacement/import closes it. Queries require the current web
revision and handoff ID. `native`/`bun-native`/`asar-native` CLI commands load and report only;
their `-analyze` variants explicitly request static analysis. All use C++ in
process. No target execution, extractor executable or temporary native file is
involved. Native input materialization is capped at 256 MiB; native loader and
pipeline allocations/work use their existing contracts, not JS parser budgets
or a claimed hard process memory/time bound.

## Original byte storage

POSIX imports stream into one private temporary spool per snapshot. The file
is exclusively created with mode `0600`, unlinked before target bytes are
copied, sealed after capture and closed when its last owned view is released.
Its descriptor is close-on-exec. No temporary path or descriptor is exposed by
the SDK. Original-byte slices keep their storage alive and do not reopen the
selected input, so later edits, replacement or deletion cannot alter evidence.

The default and maximum admission limits are 512 MiB per snapshot and 256 MiB
per original member. Capture uses 64 KiB transfers; a single materialized blob
read is capped at 8 MiB, with callers allowed to impose lower limits. JavaScript
and source-map requests check their 1 MiB / 8 MiB limits before reading bytes
into a string. Importing a large opaque executable therefore does not imply
that its source, container format or bytecode can be analyzed.

Preview retains only its digest after producing the review metadata. A session
can hold at most its published spool plus a replacement capture, totaling at
most 1 GiB of original bytes. This is per session and excludes derived models;
it is not a whole-process memory or disk quota. Storage creation/read/write
failures return fixed codes and preserve a previously published revision.
The snapshot is ephemeral, not an export or a persistent evidence archive.
Windows admission remains unavailable pending a handle-based reader.

## Lexical bindings

The C++ source model retains named AST child roles and exact UTF-16 identifier
and literal values privately, including escaped lone surrogates in JS strings.
The binding profile resolves source declaration identities through script,
module, CommonJS, function, block, loop, catch, switch and class scopes. Import
specifier names, noncomputed property names, labels and private names do not
become ordinary variable reads. CommonJS explicitly models its five wrapper
parameters; it does not verify the runtime producer of an input.

The separation of parameters, body variables, implicit `arguments` and lexical
body declarations follows
[FunctionDeclarationInstantiation](https://tc39.es/ecma262/multipage/ordinary-and-exotic-objects-behaviours.html#sec-functiondeclarationinstantiation).
Default-parameter expressions cannot see body variables. A scope denotes a
static family, so separate invocations and loop iterations do not share one
runtime cell merely because they have the same scope identity.

The profile has explicit limitations:

- `lexical_binding` links an identifier occurrence to a declaration family.
  It proves no runtime value, initialization, reachability or lack of exceptions.
  `immutable_binding` describes rebinding only, not deep immutability.
- `external` means the name is absent from the supplied source. It does not
  establish that the name exists in a host or identifies a built-in API.
- `with` lookup and possible non-strict direct-eval declarations yield dynamic
  reference classifications. Even a local named `eval` can hold the intrinsic;
  call syntax is conservatively flagged without evaluating it. Strict eval is
  flagged as well, although it does not introduce caller-visible declarations.
- Annex B block-function and catch-var compatibility are reported as partial;
  outer references affected by possible block-function bindings stay uncertain.
  Labelled function declarations are outside this profile. See
  [Annex B](https://tc39.es/ecma262/multipage/additional-ecmascript-features-for-web-browsers.html#sec-block-level-function-declarations-web-legacy-compatibility-semantics).
- Conflicting declarations remain explicit. Private-name resolution, temporal
  initialization, module loading and reflection remain separate work. The
  effect/value profile below does not infer initialized runtime bindings.
  Parser acceptance plus binding status `ok` is not full
  ECMAScript validation or authorization to rewrite the source.

Unknown node semantics or a binding budget failure publish no partial binding
graph. Known dynamic cases retain classified facts with `binding_status: partial`.
At most 32 diagnostic records are retained; `diagnostic_count` and
`diagnostics_complete` expose the full count and any omission.
The CLI emits those diagnostics and pages with exit status 0; unavailable,
unsupported or budget-exceeded binding analysis returns exit status 1.

## Finite primitive values and effects

`javascript-primitive-values-v1` evaluates a bounded whitelist over literals:
undefined produced by `void`, null, Boolean, binary64 Number, UTF-16 String and
BigInt. It supports primitive unary/coercion operations, arithmetic/bitwise
operations, comparisons in the qualified subset, string concatenation,
untagged templates, sequences, and known logical/conditional branches.
Identifiers (even `undefined`, `NaN` or `Math`), properties, calls, objects,
runtime binding propagation and target constructors are not evaluated.

Number arithmetic uses LLVM APFloat with explicit nearest/ties-even rounding.
It retains negative zero and uses truncating
[ECMAScript remainder](https://tc39.es/ecma262/multipage/ecmascript-data-types-and-values.html#sec-numeric-types-number-remainder),
not IEEE remainder. Number formatting uses the pinned embedded Hermes support
library across a standard-C++ boundary. BigInt arithmetic has a 1,024-bit
magnitude ceiling; mixing it with Number arithmetic or dividing by zero returns
a fixed exception classification. Number exponentiation and mixed BigInt
equality/relational comparisons remain explicit `unsupported` cases.
The embedded parser and formatter require the host thread's nearest-rounding
mode; another mode yields `unsupported_host_rounding_mode` without changing the
host environment. APFloat arithmetic on an already admitted model does not
inherit the host rounding mode.

Per-node value status is `constant`, `unknown`, `would_throw`, `unsupported`,
`budget_exceeded` or `not_expression`. `would_throw` describes evaluation of
that node under the profile, not proof that execution reaches it. Unchosen
branches have their own records but do not change the selected expression's
value. Per-value limits refuse that value; aggregate work/storage exhaustion
discards the incomplete value inventory. All values remain private.

`javascript-conservative-effects-v1` reports may-effects for immediate
evaluation separately from deferred function bodies/default parameters and
instance-field initializers. Class heritage, computed keys, static fields and
static blocks contribute immediate effects. Getter bodies stay deferred when
creating their containing object. Property access, coercion of unknown values,
global object bindings, `with`, iteration and calls retain conservative
call/throw/unknown flags. This is not a call graph, flow-sensitive reachability
analysis, or proof that any listed effect occurs.
`unknown` can include effects outside the listed categories, so an absent
`call` flag alongside `unknown` is not a no-call proof. Script-root declaration
instantiation retains this uncertainty, including declarations inside branches
that statement evaluation will not select. Global `var` initialization can
invoke an existing global accessor and retains property/call effects.

Known short-circuit guards can omit an unselected branch's immediate effects,
but `contains_declaration` is retained even for `if (false) { var x; }`.
Unavailable effect records use JSON null, never an empty effect list. Results
always state `authorizes_source_rewrites: false`; no rewrite or source-reflection
safety follows from a constant value or zero immediate effect flags.

The CLI returns 0 for completed/partial profile results and 1 when the value or
effect analysis is unavailable, unsupported as a whole, or globally exhausted.
Per-node exceptions/refusals are inspection findings, not CLI execution errors.

## Module evidence and admitted file candidates

`javascript-module-evidence-v1` records static ESM imports, local/default exports,
named/star/namespace re-exports, dynamic import expressions and syntactic
`require(...)` / `require.resolve(...)` candidates. Each occurrence retains its
own ID, syntax node, original byte span and lexical links. Equal specifier text
does not merge requests. Exported destructuring visits only binding positions;
function parameters, initializer locals and a class's inner name binding do not
become exports. Duplicate or unbound exported names remain explicit findings.
This profile does not yet infer CommonJS export assignments or require aliases.

Require evidence distinguishes external/dynamic lookup, another lexical binding
and a caller-selected CommonJS parameter. Even the latter can be reassigned and
never proves a Node loader value. No request establishes execution or reachability.
Dynamic import, including a literal specifier, retains an unresolved runtime boundary.
Its options node remains evidence; no properties are evaluated. The pinned
parser accepts legacy static `assert { ... }` syntax and retains private key/value
evidence, including duplicate-key conflicts. Static `with { ... }` is currently
rejected by that parser; current Node import-attribute support is not claimed.

`admitted-relative-file-v1` compares only literal static ESM `./` or `../`
specifiers with exact, case-sensitive names already captured in a directory
snapshot. It checks root confinement and valid UTF-16, and refuses URL schemes,
bare package specifiers, absolute paths, backslashes, percent encoding,
query/fragment syntax, whitespace/control characters and directory shorthand.
It does not add extensions, search indexes or read package exports. A match is
an `exact_admitted_file_candidate`, with `runtime_target_verified: false`.
This deliberately limited evidence is separate from
[Node's URL and package resolution rules](https://nodejs.org/api/esm.html#import-specifiers).
It does not establish loader configuration, module format, import-attribute
validity or successful linking. Foreign export names and star conflicts remain
unlinked. A single-file input or derived map source has no trusted directory base.

Specifier/export/import/assertion text stays private. Summary `module_status`
describes inventory completion, while `binding_status` and `link_status` retain
their own coverage. Exhaustion discards the entire affected inventory or link
set; unavailable links have null candidate IDs. The CLI returns 0 for completed
or partial inventory and 1 for unavailable/unsupported/exhausted analyses. Its
single-file `modules` command reports the absent directory base explicitly;
directory candidate links are available through the SDK and worker lifecycle.

HTML inline IDs instead select `html-inline-module-file-candidates-v2`, which
compares literal imports/re-exports/dynamic imports using the document and
preceding base candidate. It supports the bounded HTML local URL subset and
retains document/script/base IDs in `link_context`; normal file contexts remain
null. Captured early import maps add exact/prefix/scoped candidate resolution,
blocking entries, first-definition composition and private URL identity.
Unknown origin/history, malformed maps and budget failures refuse association.
The
`html-modules` and `asar-html-modules` CLI forms emit those summaries and pages,
including completed `partial` results. They never reinterpret an external file
under a document context. See the [HTML contract](web-html-profile.md).

## Bundle source partitions

`webpack5-basic-loader-v1` recognizes an anonymous, synchronous, argument-free
top-level IIFE with a table/cache/loader prefix. An optional initial strict
directive is accepted. The prefix is matched as a syntax tree with consistent
lexical identities, so renaming its locals or changing whitespace does not
break recognition. Variable spelling alone is insufficient. The fixed loader
must check its cache, create `{exports:{}}`, call the selected factory with
`(module, module.exports, loader)`, and return `module.exports` in the qualified
shape. It follows the basic branch of webpack's
[fixed renderer source](https://github.com/webpack/webpack/blob/6c9f912af2dfbb3e0e1a2a3ecdc3881c96363432/lib/javascript/JavascriptModulesPlugin.js).
Interception, `.call` dispatch, exception-handling variants, split chunks,
concatenated array tables and general minifier rewrites are outside this profile.

Object tables retain literal string/integer keys; sparse arrays retain original
ordinals. Numeric and equivalent string keys cannot create two modules.
Duplicate keys, special `__proto__` entries, spread/computed/accessor properties
and unsupported factory signatures refuse that bundle. Anonymous synchronous
factories have zero to three plain, distinct parameters and a block body.
Literal false slots are explicit `removed_slot` records, not recovered functions.

Factory records point to original wrapper/body nodes, byte ranges and SHA-256
digests. They are source partitions inside the unchanged parsed unit: no original
filename, independent executable module or rewritten source is invented.
Statements after the loader prefix remain `startup_or_runtime` regions. An
inlined entry is not automatically reconstructed as a named module. The rest of
the source remains available through its original source inventory.

Direct calls to the lexical loader or a factory's third parameter can link to a
`table_member_candidate`. Shadowed callees do not inherit that role. Dynamic
lookup, binding writes, optional calls, unsupported arguments, absent IDs and
removed targets retain separate statuses. Such links never verify runtime loads:
reachability, aliases, property mutation and module-cache contents are not proved.
Module keys and all target text remain private in SDK/CLI/worker responses.

The corpus includes a hash-pinned
[upstream CommonJS example](https://github.com/webpack/webpack/blob/6c9f912af2dfbb3e0e1a2a3ecdc3881c96363432/examples/commonjs/README.md)
and C++-constructed inert boundary cases. Its
[manifest](../unittests/web/fixtures/webpack-manifest.json) records the original
document, exact code-fence derivation and expected sparse factories/edges.
The repository tag is `v5.99.9`; the README replaces the generating version with
`X.X.X`, so the actual producer version stays unknown. This is documented-output
qualification for the structural profile, not a locally regenerated producer
matrix or universal webpack support. The MIT license is staged beside the SDK.

`bundle_status` is `ok`, `partial`, `not_detected`, `unsupported`,
`budget_exceeded` or `unavailable`. A malformed candidate publishes none of its
partitions; valid separate bundles may remain with `partial`. A global budget
failure clears the entire inventory. `not_detected` leaves an ordinary source
unit intact. The CLI returns 0 for completed/partial/not-detected inventory and
1 for unsupported/unavailable/exhausted analysis. No bundle result authorizes a
source rewrite or authenticates a producer.

## Source maps and positions

The decoder follows the admitted subset of
[ECMA-426](https://tc39.es/ecma426/): version 3 basic maps and bounded inline
index sections. It decodes signed Base64 VLQ state, preserves coincident
anchors, and handles section line/column offsets. External section URLs are
rejected. `sourceRoot`, `file`, `sources`, names and unknown extension fields
never authorize I/O. Unsupported or malformed maps return a fixed error code;
no partial map is published.

Byte ranges refer to unchanged UTF-8 input. Map positions use zero-based lines
and UTF-16 columns. CRLF is one line break; CR, LF, U+2028 and U+2029 are also
recognized. Queries reject positions inside a UTF-8 scalar, surrogate pair or
CRLF rather than rounding them. Lone surrogate escapes in JSON are outside
the admission profile because replacing them would change evidence.

Decoding a map proves its admitted format only. `source_map_lookup` accepts an
explicit generated source ID and byte position; its association remains a
`caller_assertion` with `provenance_verified: false`. It returns every exact
or nearest preceding anchor on the same line, without interpolation. Only the
returned anchors are checked against supplied generated/embedded source bytes.
An invalid anchor, missing original content, and an unmapped segment remain
distinct. No filename match establishes provenance.

## Build and qualification

`NEVERD_ENABLE_WEB_ANALYSIS` controls the backend; its
`NEVERD_ENABLE_WEB_JAVASCRIPT` suboption controls the embedded parser. The C API
keeps unavailable stubs when disabled. Runtime capabilities report the actual
compiled operations. CMake pins the parser archive digest; offline builds can
set `FETCHCONTENT_SOURCE_DIR_NEVERD_HERMES` to that pinned source checkout.
Hermes and transitive notices are installed with the SDK.
`NEVERD_ENABLE_WEB_ASAR` separately requires a prepared ICU4C 77.1 installation
and Unicode 16.0 data; `ICU_ROOT` may locate it. If absent or disabled, ASAR is
unavailable without an external-tool fallback. This feature stages notices,
but deployment still must provide matching ICU native libraries or a qualified
static link; it does not yet bundle those runtimes.

```console
cmake --build build-release --target NeverDWebArtifactTests NeverDWebSourceTests NeverDWebSourceMapTests NeverDWebSDKTests --parallel 4
ctest --test-dir build-release/unittests/web --output-on-failure
```

The SDK tests launch the C++ CLI with an unusable `PATH`, exercise target code
that must never run, and check that sensitive canaries do not enter ordinary
output. These tests do not qualify every host, container, syntax feature or
resource ceiling. See the [support matrix](web-support-matrix.md) for limits.
