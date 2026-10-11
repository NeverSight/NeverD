# Web analysis support and qualification

This table records implemented behavior separately from plans. No completed
foundation milestone closes issues #714–#718. Qualification below is local
macOS arm64 Release. The integrated-LLVM full, parser-disabled and
backend-disabled configurations were built and checked on that host. The
separate current-dev integration uses prebuilt LLVM 23.0.0 r4 without Zstd;
its checks and omissions are recorded in the
[implementation ledger](web-analysis-implementation.md).
Other hosts require separate evidence.

| Surface | Implemented profile | Qualification / outstanding work |
|---|---|---|
| Artifact import | `posix-descriptor-v1` | Immutable file/directory capture, revision checks, links/special files, aggregate limits and basic name collisions tested; Windows reader unavailable |
| Original-byte storage | `posix-unlinked-spool-v1` | One private unlinked spool per snapshot, bounded range reads, immutable slices, descriptor lifetime and write-failure checks; 512 MiB input qualified locally without whole-file materialization |
| JavaScript syntax | `hermes-602befee-js-v3` | Script/module/CommonJS, byte spans, async-rest location/comma regressions, `using`/`await using`, module await, selected modern syntax and malformed/deep input tested; [resource profile](web-resource-management-profile.md), complete syntax/semantic coverage not claimed |
| Source maps | `source-map-v3-offline-v1` | Basic and inline indexed maps, UTF-16/bytes, malformed offsets/VLQ, budgets and private embedded content tested; no automatic association or URL retrieval |
| C API and CLI | Source/binding/semantic/module/bundle/map/view/navigation/anchor/native/ASAR/Electron/package/interface/stream operations | Owned responses, revision/cache checks, offline CLI, explicit passive-import previews and output canaries tested |
| Lexical scope and binding identities | `javascript-lexical-bindings-v1` | Source roles, hoisting/shadowing, default parameters, imports/exports, classes and dynamic cases have C++ checks; no runtime values or complete semantic validity claim |
| Finite primitive values | `javascript-primitive-values-v1` | C++ binary64, UTF-16, bounded BigInt, coercion, branch and refusal checks; no runtime identifier/object/call evaluation |
| Conservative effects | `javascript-conservative-effects-v1` | Immediate/deferred boundaries, classes, getters, declarations, dynamic bindings and unavailable-result checks; no rewrite permission |
| Module evidence | `javascript-module-evidence-v1` | ESM requests/entries, dynamic boundaries, require callee evidence, private assertions, duplicate exports and budgets have C++ checks; static `with` attributes and CommonJS export inference pending |
| Admitted file comparison | `admitted-relative-file-v1` | Confined exact static relative paths only; candidates are never runtime resolution; packages/aliases/foreign export linking remain pending |
| Runtime module resolution and dataflow | Pending | Binding identities and literal values alone cannot authorize rewrites or API attribution |
| Bundle partitions | `webpack5-basic-loader-v1` | Fixed basic runtime with object/sparse-array tables; archived upstream example plus inert C++ boundary fixtures; producer version remains unknown, no generic webpack/minifier coverage |
| Reviewed source display | `javascript-source-view-v1` | Structural default masking, explicit local-review ranges, preview/commit/revocation, UTF-8 chunks and original/projected range mapping; no semantic rewrite or network permission claim |
| Source navigation | `javascript-source-navigation-v1` | Functions, calls, references, source containment and lexical/initializer links; runtime call targets and feature-specific attribution remain unproven |
| Storage/view anchors | `source-storage-anchor-v1` | UTF-8/UTF-16 positions, exact original/ASAR/Bun encoding boundaries, whole compressed-frame locators and committed display coverage; JSON encoded-member offsets remain unavailable |
| Semantic source transforms | Pending | Per-pass receipts, semantic refusal and undo remain required |
| Bun standalone | Bun 1.4.2 ELF/Linux, thin Mach-O/macOS and PE/Windows, each x64 and ARM64; prelinked graph extensions | 25 self-authored full compiler outputs qualify the six baseline container layouts; the official Claude Code 2.1.296 ELF additionally qualifies real prelinked/runtime-options data. Other containers' extensions have synthetic coverage; linked-bytecode refused, caches opaque, version unauthenticated. [Exact matrix](web-bun-profile.md) |
| Explicit Bun local export | `bun-local-evidence-export-v1`, `hermes-602befee-recovery-js-v2` | C++ CLI/C API preserve original/regions, decoded JS and reparse-verified readable copies in a fresh private directory with an inert index; no worker export, original TypeScript recovery or native/cache decompilation. [Real artifact qualification](web-claude-code-qualification.md) |
| Bun serialized maps | `bun-1.4.2-serialized-source-map-v1` | C++ windows/varints and bounded in-process Zstd; preserved compiler maps and hostile C++ fixtures; retained anchors only, producer-discarded names/unmapped boundaries stay unknown; requires LLVM Zstd |
| Explicit native handoff | `immutable-native-handoff-v1` | C++ snapshot/file parity for x64 ELF/PE/thin Mach-O and AArch64 ELF, snapshot lifetime, Bun native assets and a full pinned Bun container; 9 cases passed locally. Universal slices and other host qualification pending; native loading does not qualify an extractor |
| ASAR | `asar-pickle-json-v1` | Fixed upstream archives and C++ malformed fixtures, explicit packed/unpacked association, integrity, source/map/anchor/native consumers and direct/framed parity qualified locally; ICU 77.1 runtime deployment and other hosts remain unqualified. [Profile](web-asar-profile.md) |
| Archive name comparison | `icu-77.1-nfc-casefold-portable-v1` | Native NFC/full folding with Unicode 16.0, device/path/collision checks for ASAR and selected unpacked members; original input capture and general export are not yet unified under this policy |
| Module-origin source candidates | `javascript-module-origin-candidates-v1` | ESM/require, lexical aliases, object destructuring and construction boundaries; writes/dynamic lookup/eval/budgets refuse links. No runtime module/object value proof |
| Electron manifest / source boundaries | `electron-40-package-entry-candidates-v1` / `electron-static-boundaries-v1` | Captured exact-file main candidates and source-visible window/preload/renderer/IPC/bridge records; framework/version/target verification and app-level entry graph remain pending. [Profile](web-electron-profile.md) |
| Electron selected-source IPC | `electron-scoped-channel-candidates-v1` | Exact private UTF-16 channel equality inside an explicitly selected manifest/source scope, unresolved endpoints, evidence-linked pages and compatible pair counts; no process-role/runtime-routing or complete application graph claim |
| Captured source paths / Electron entries | `javascript-captured-portable-path-candidates-v1` / `electron-captured-entry-candidates-v1` | Distinct source/app bases, finite path/URL operations and exact available-file association inside the explicit manifest scope; HTML/import closure and runtime path bases remain unverified |
| NW.js / VSIX | Pending | Each needs its own qualified container profile; ASAR does not establish this support or safe raw export |
| Captured HTML | `html-utf8-script-candidates-v1` | Script/base inventory, captured local URL candidates and inline raw-byte sources/anchors; DOM and runtime activation remain unverified. [Profile](web-html-profile.md) |
| HTML inline module files | `html-inline-module-file-candidates-v2` | Literal requests use explicit document/base evidence and exact captured members; early captured maps add scoped/prefix/null rules and private URL candidates; browser activation/history and full closure remain pending |
| HTML import maps | `import-map-url-evidence-v1`, embedded Ada 4.0.0 | Bounded C++ JSON/URL processing and metadata pages remain available without JS parsing; absolute key origins, later/interleaved maps and order-dependent JSON remain explicit HTML-profile refusals |
| npm metadata and supplied-directory diff | `node-package-evidence-v1` | npm lock v1/v2/v3, hidden locks, captured manifests, unresolved/conditional placements, exact file hashes and evidence coverage comparisons; [profile](web-package-profile.md). Behavior/advisory/provenance, dispositions and reports remain pending |
| Package archives and original SRI | `ustar-pax-single-gzip-v1`, `npm-original-sri-v1` | Native tar/local-PAX/single-gzip, metadata-only links, shared source/package/Bun/native consumers and explicit registry/lock declaration checks over original bytes. [Profile and limits](web-package-archive-profile.md). Recursive archives, full tar dialects and publisher authentication are unsupported |
| Passive interface evidence | `har-1.2-metadata-v1`, `direct-fetch-websocket-syntax-v1`, `absolute-http-method-origin-path-v1` | HAR redaction preview/commit, fixed metadata and source-node links; explicit candidate joins preserve inference/observation classes. XHR/wrappers, dynamic URLs, WebSocket frames and protocol reconstruction remain unsupported. [Profile](web-interface-profile.md) |
| Passive stream/log records | Six explicitly selected JSONL/SSE/JSON-RPC/MCP-shape/recorded-envelope/log profiles | Bounded native framing, redaction preview/commit and recorded-context candidate joins; missing/ambiguous evidence refuses links. No complete MCP schema/negotiation, source correlation or live capture claim. [Profiles](web-stream-profile.md) |
| Node SEA | Node 22.15.0 LE64 preparation blob and explicit ELF64/Mach-O64/PE32+ x64/ARM64 resource profiles | Separate C++ reader, stored JS/assets and opaque V8 cache/snapshot, shared consumers; [qualification and limits](web-sea-profile.md). Runtime activation/version and other versions/hosts are unverified |
| Tauri/Wails/pkg/nexe | Research/profile qualification pending | Detection is insufficient; each named profile must extract known assets |
| C++ worker | `web_` operations in protocol 1.1 | Real framed-process, direct-query parity, web/native revision isolation and private-output checks passed locally |
| C++ MCP transport | Pending | Must use the same C API and enforce an explicit host input scope |

The ASAR increment restored the backend, JavaScript parser, ASAR and LLVM Zstd
options to ON. Its final local run passed 185 of 186 registered web cases;
the ASAR-omission-only case was explicitly skipped. All 65 Session C API and
nine worker cases passed. ASAR-disabled, parser-disabled and backend-disabled
behavior was also checked, including explicit refusal through SDK/CLI/worker.
An earlier worker initial-handshake timeout remains unisolated despite three
unchanged isolated passes and the final full-worker pass. See the
[implementation ledger](web-analysis-implementation.md#asar-and-captured-unpacked-members--2026-10-10)
for the per-profile counts, skipped coverage and failure evidence. Fixed native
ICU runtime packaging and other host qualification remain outstanding.

The subsequent manifest/source increment passed 201 of 202 web cases after
restoring all options, with one expected ASAR-omission-only skip, and all nine
worker cases. Parser/backend omission explicitly refused the unsupported
Electron operations. The selected-source IPC increment has separate evidence
in the [ledger](web-analysis-implementation.md#explicit-electron-ipc-scope--2026-10-10);
its enabled run passed 208 of 209 web cases (one expected skip) and all nine
worker cases. This does not imply application entry closure or runtime routing.

The captured-entry increment restored all four feature options and passed 223
of 224 web cases, with one expected skip, and all nine worker cases. The new
entry APIs and CLI forms also passed focused parser/backend omission checks.
Its [qualification record](web-analysis-implementation.md#captured-electron-entry-candidates--2026-10-10)
covers source/app roots, private paths, ASAR occurrence isolation and work/cache
limits. HTML/import closure and runtime path bases remain unverified.

The HTML increment restored all four options and passed 240 of 241 web cases,
with one expected skip, and all nine worker regressions. Seventeen HTML cases
also passed in the focused parser-omission run; backend omission retained both
new ABI symbols with explicit unavailability. The final enabled run additionally
checks bogus end-tag comments, inline import-base refusal and unpacked storage
anchors. See the [qualification record](web-analysis-implementation.md#captured-html-script-candidates--2026-10-10).
These are source-visible candidates; DOM and application import closure remain
unverified.

The inline-module increment restored the four options to ON and passed 249 of
250 registered web cases, with one expected skip, and all nine worker cases.
Its eight core cases include malformed-source and early-budget status coverage;
SDK/CLI/worker cases retain document/base and packed/unpacked occurrence evidence.
Parser and backend omission were checked, including both new CLI forms. The
[qualification record](web-analysis-implementation.md#html-inline-module-file-candidates--2026-10-10)
distinguishes the final parser-only correction from the earlier omission runs.
Import-map resolution, browser module identities and complete application import
closure remain outstanding.

## Current admission ceilings

| Resource | Ceiling |
|---|---:|
| Original input bytes per snapshot | 512 MiB |
| One original file/member | 256 MiB |
| Capture transfer buffer / one blob read | 64 KiB / 8 MiB |
| Original spool bytes per session, including a pending capture | 1 GiB |
| Archive expanded bytes across retained and pending streams per session | 512 MiB |
| Total conservative original/expanded spool budget per web session | 1.5 GiB |
| Package archive members / depth / cached archives | 10,000 / 64 / 4 |
| Package archive PAX/name/prefix metadata | 8 MiB |
| SRI declaration bytes / tokens / cached verifications | 64 KiB / 256 / 16 |
| HAR original / observations / field records | 8 MiB / 4,096 / 32,768 |
| Source interface records / correlation pairs | 4,096 / 32,768 |
| HAR captures / pending previews / source analyses / correlations | 4 / 1 / 4 / 4 |
| Stream original / physical line or SSE block | 8 MiB / 256 KiB |
| Stream records / aggregate JSON nodes / JSON depth | 4,096 / 200,000 / 32 |
| Stream private recorded ID/session bytes / captures / pending previews | 1 MiB / 4 / 1 |
| Original entries / directory depth | 10,000 / 64 |
| JavaScript source / syntax nodes | 1 MiB / 100,000 |
| Lexer tokens plus comments at admission / retained cached lexemes | 200,000 (including EOF) / 400,000 |
| Source view text / region records / local-review ranges | 4 MiB / 400,001 / 64 |
| Published source views / pending source views per session | 2 / 1 |
| Source view text chunk | 65,536 bytes |
| Private decoded JS string units per source | 2,097,152 UTF-16 code units |
| Binding-analysis steps per source | 2,000,000 |
| Source-navigation steps per source | 2,000,000 |
| Module-inventory steps / admitted-file comparison steps | 2,000,000 / 2,000,000 |
| Private module-name units / relative specifier units | 1,048,576 / 4,096 UTF-16 units |
| Bundle analysis steps / private module-key units | 4,000,000 / 1,048,576 UTF-16 units |
| Bundles / recovered module slots / dependency records per source | 64 / 4,096 / 50,000 |
| Bun modules / builtins / cached extractions | 4,096 / 4,096 / 4 |
| Bun private names total / one name | 1 MiB / 32 KiB |
| SEA selected input / assets / cached extractions | 256 MiB / 4,096 / 4 |
| SEA private names total / one name / page records | 1 MiB / 32 KiB / 128 |
| Bun section / program headers | 4,096 / 1,024 |
| Bun serialized map / decoded content total | 8 MiB / 8 MiB |
| Bun decoded source / Zstd window | 4 MiB / 8 MiB |
| Bun map sources / anchors / Zstd blocks per frame | 10,000 / 100,000 / 100,000 |
| Selected native input materialization | 256 MiB |
| ASAR header / JSON depth / nodes / one string | 8 MiB / 64 / 200,000 / 4 KiB |
| ASAR members / directory depth / cached extractions | 10,000 / 24 / 4 |
| ASAR one path / cumulative path bytes (declarations plus selected unpacked index) | 4 KiB / 8 MiB |
| ASAR one file / cumulative declared payload | 256 MiB / 512 MiB |
| ASAR integrity block bytes / blocks per file | 8 MiB / 10,000 |
| Electron manifest bytes / JSON depth / nodes / one string | 1 MiB / 32 / 20,000 / 4 KiB |
| Electron manifest namespace entries / cached manifests | 10,001 / 16 |
| Source-origin steps / cumulative allocated UTF-16 units | 2,000,000 / 1,048,576 |
| Source-origin traversal / pattern depth / member chain | 64 / 32 / 32 |
| Electron source steps / records / retained private string units | 2,000,000 / 10,000 / 1,048,576 |
| Electron IPC selected sources / cached scopes | 16 / 4 |
| Electron IPC cumulative selected boundaries / private channel units / work | 10,000 / 1,048,576 UTF-16 units / 2,000,000 including comparisons |
| Electron entries selected sources / cached scopes / aggregate selected boundaries | 16 / 4 / 10,000 |
| Electron entry association work / per-source path work / path traversal depth | 4,000,000 / 2,000,000 / 64 |
| One path / cumulative allocated path text per source | 4,096 / 1,048,576 UTF-16 units |
| HTML bytes / cached documents | 4 MiB / 4 |
| HTML scan work / entry link work / inline module link work | 16,777,216 / 4,194,304 / 4,194,304 |
| HTML combined script/base records / inline module requests | 10,000 / 10,000 |
| Value-analysis steps / effect-analysis steps per source | 4,000,000 / 2,000,000 |
| One value string / aggregate allocated string units | 65,536 / 2,097,152 UTF-16 units |
| BigInt magnitude / numeric-conversion text | 1,024 bits / 4,096 code units |
| Retained binding/module/bundle diagnostics per analysis | 32; total count and completeness reported |
| Cached source units / total nodes | 16 / 200,000 |
| Parser arena / allocation calls | 32 MiB / 200,000 |
| Parser recursion / inventory traversal depth | 128 / 256 |
| Map JSON / JSON depth / JSON nodes | 8 MiB / 32 / 200,000 |
| Map string / coordinate-source bytes | 4 MiB / 4 MiB |
| Map sources / names / segments | 10,000 / 100,000 / 100,000 |
| Cached maps / total segments | 4 / 200,000 |
| Nested index sections / sections per array | 4 / 1,024 |
| Coordinate lines | 200,000 |
| Page entries / coincident lookup anchors | 512 / 512 |

Cache admission reserves space for a maximum-sized analysis before parsing.
It can refuse a new analysis before every slot is occupied. No cache eviction
is implemented yet. Original bytes are streamed into a private, immediately
unlinked file and read only through bounded ranges. Preview keeps the candidate
digest, not another retained byte store. The one-GiB ceiling covers a published
snapshot plus its replacement capture within one session, not aggregate usage
across arbitrarily many SDK sessions. Maps and ASTs have separate memory budgets;
disk-backed admission does not increase the source/map parser limits.
Native handoff materializes its selected input for the existing format reader.
Its byte ceiling is not an aggregate SDK-handle quota or a hard bound on native
loader/pipeline allocations or time. The worker retains one native handoff;
SDK callers own and dispose each returned independent handle.
The parser's cooperative
five-second deadline is checked during arena allocation; it is neither a
hard wall-clock deadline nor a process memory limit.
Binding analyses are cached at most once per admitted source and share its
revision lifetime; retained records are bounded by the source/node ceilings.
Value/effect analyses use the same cache lifetime, with one result per source.
Module inventory and file comparison also retain at most one result per source;
no recursive module loading or speculative host lookup occurs.
Bundle partitions share that revision/cache lifetime. Counts cover all recovered
bundles in the source, not a fresh allowance for each table. Digest work is charged
by the source bytes read, and a global budget failure publishes no partial result.
The string-allocation budget includes temporary concatenation results and is
conservative; a refused subexpression never turns into a truncated constant.

## Parser provenance

Hermes is pinned to
[`602befee340da188ea1560cde7a9cc33e6b180dc`](https://github.com/facebook/hermes/tree/602befee340da188ea1560cde7a9cc33e6b180dc).
Its archive SHA-256 is
`6c615757374850ccb99991c4536aaf86ac0973661b1ae18aac45d2ca85143153`.
Only the parser/AST and required support dependencies are built; TypeScript,
Flow and JSX are disabled. The NeverD allocation-budget overlay is disclosed
in [third-party notices](../THIRD_PARTY_NOTICES.md). Upstream license material
is preserved under [`LICENSES/hermes`](../LICENSES/hermes).
