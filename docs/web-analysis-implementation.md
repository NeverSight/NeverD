# Offline web analysis implementation ledger

This work implements issues [714](https://github.com/NeverSight/NeverD/issues/714),
[715](https://github.com/NeverSight/NeverD/issues/715),
[716](https://github.com/NeverSight/NeverD/issues/716),
[717](https://github.com/NeverSight/NeverD/issues/717), and
[718](https://github.com/NeverSight/NeverD/issues/718).
It is in progress. A foundation milestone does not close the five epics.

## Authoritative implementation constraint

The user approved implementation on 2026-10-10 and required **all new
implementation code to be C++, without external scripts or external tools**.
This supersedes the draft's Node/Babel helper, JavaScript analyzer, Python
binding additions, Python MCP changes, and script-based fixture generators.
The new feature must operate through compiled, in-process C++ libraries and
NeverD's C++ SDK/CLI/worker. It must not shell out to Node, Bun, package
managers, archive utilities, parsers, or a separate analyzer executable.
Development builds still use the repository's normal CMake/compiler tools.

A pinned embedded C++ JavaScript parser is integrated. Hermes' parser
and AST libraries are used without its VM, runtime execution, bytecode
compiler or command-line tools. Semantic rules, source mapping, evaluation,
rewrites, adapters and protocol consumers remain C++. New tests use C++.
The existing Python MCP adapter is not the implementation path for the new
feature: web MCP transport/tool dispatch must be implemented in C++ as well.

The independent source-domain C API is included explicitly through
`neverd/sdk/NeverDCAPIWeb.h`. It is installed beside the existing SDK headers,
but is not part of the native-session umbrella consumed by the existing
Python binding. That binding and its complete native-API drift check retain
their existing scope; no Python support for web APIs is claimed.

## Required work and current evidence

| Work | Current state | Evidence required before completion |
|---|---|---|
| P0 identity/schema/profile contract | [Schema](web-artifact-schema.md), [support matrix](web-support-matrix.md) and pinned parser documented | Container layout dependencies and fixture manifests remain |
| P1 artifact store and safe input | 12 identity/input, eight blob/storage and five JSON admission tests passed on macOS arm64 Release; 512 MiB disk-backed snapshot qualified | Broader host qualification and redaction/export remain; Windows pending |
| P2 SDK/CLI/worker | Thirty-four SDK/CLI tests passed, including navigation/storage/view anchors and direct Bun navigation/view/anchor commands; C++ worker adapter/framed-process and all nine worker regressions passed | Broader host, cancellation/resource and distribution qualification remain |
| P3 C++ JS parser/scope/query/maps | Nine parser/model, thirteen binding, eleven primitive-value, ten effect, eleven module, eight bundle, nine source-view, five navigation, nine standard coordinate/map and six Bun map tests passed | Full syntax matrix, feature-specific navigation/attribution, runtime dataflow, foreign export linking, broader bundle profiles and verified map association remain |
| P4A Bun extraction | Fixed Linux x64 extraction, decoded-source navigation/view/storage anchors, bounded serialized-map decoding and explicit native handoff implemented; five full compiler outputs, ten extraction/range and six map cases passed | Other Bun platforms remain; JSC caches stay opaque |
| Shared native handoff | Nine native handoff tests and 65 existing Session C API tests passed; immutable-buffer loader, independent native SDK session and metadata-only CLI/worker entry points; parser/backend omission checked | Other hosts, explicit universal slices and bounded native pipeline work remain |
| P4B desktop/VSIX extraction | C++ ASAR packed/unpacked extraction; Electron manifest entries, source-visible boundaries, scoped IPC comparison, captured preload/renderer files and HTML/import-map candidates implemented; qualification recorded below | External-source HTML contexts and full import closure, runtime/window routing, distribution detection, safe export, NW.js/VSIX and broader host/release qualification remain |
| P4C package graph/diff | Pending | npm lock v1/v2/v3, integrity/provenance, dispositions, platform artifacts |
| P4D passive interfaces/HAR | Pending | Static-only, HAR-only, correlation, uncertainty and redaction tests |
| P5 reversible source projections | Reviewed display projection and original/projected ranges implemented; semantic transforms pending | Per-pass receipts, local semantic preconditions, undo and refusal tests |
| P6 stream/log consumers | Pending | SSE/JSONL/JSON-RPC/MCP versioned records, truncation and canary tests |
| P7 Tauri/Wails/Node-family | Pending | Actual asset extraction and bridge/native links for each named profile |
| P8 C++ MCP, optional presentation, distribution | Pending | No external executable/script dependency, bounded outputs and shipped notices |
| P9 full acceptance audit | Pending | Issue-by-issue fixture and cross-platform evidence, documented gaps |

Original bytes now use `posix-unlinked-spool-v1`: a private, immediately unlinked
file with one close-on-exec descriptor per snapshot. Import streams and hashes
64 KiB chunks, seals before publication and exposes only bounded immutable ranges
internally. Admission is capped at 512 MiB aggregate / 256 MiB per original file,
10,000 entries and depth 64. Preview retains only its candidate digest, so one
session uses at most its published spool plus one pending capture (1 GiB).
This is not a global quota across arbitrary SDK sessions, nor a whole-process
RSS bound. Source/map materialization remains limited to 1 MiB / 8 MiB.

The local maximum-size C++ test captured two 256 MiB members and compared their
hashes; the 2026-10-10 macOS arm64 Release run recorded 65,536 bytes of additional
process peak RSS during capture and bounded reads. This is a high-water-delta
measurement for that test, not total resident memory or a promise for other
hosts. The test enforces a less-than-32-MiB growth bound and also checks refusal
before materializing an over-limit range. Other cases cover concurrent reads,
slice lifetime after input deletion, shared-descriptor cleanup and write failure
in a C++ child with a lowered file-size limit. SDK/CLI cases admit an original
above the old 64-MiB ceiling, then verify smaller parser budgets still refuse it.
Current POSIX directory traversal retains descriptor-relative no-follow opens.
Windows input remains unavailable until a handle-based implementation passes tests.

Ordinary inventory returns hashes, sizes, parent identities and fixed kinds;
target-derived names and contents are omitted. That is the current
`metadata-only-v1` output policy, not universal secret recognition. Explicit
source-view policy is implemented below; broader consumer redaction and export
remain pending.

Before Bun extraction, 115 web cases passed through their owning CTest directory, with
the compiled-capability check and three worker cases also passing. A root
CTest invocation was blocked during unrelated semantic-test discovery by an old
`NeverDSemanticTests` executable referencing a missing `BinaryPatcher` symbol in
the current shared library. It was not counted as a successful whole-repository
run. Worker checks passed both directly and through CTest after enabling testing
before configuring tool subdirectories. With the JS parser disabled, all 41
remaining web cases (including the new disk-storage and large-original cases),
the compiled-capability check and three worker checks passed; the
backend-disabled ABI case was explicitly skipped in that profile.
With the whole backend disabled, both ABI availability tests passed and the CLI
returned `capability_unavailable` with exit status 1. Existing protocol/graph
checks passed; the actual web worker case explicitly skipped. This disabled
backend check includes binding, semantic, module and both new bundle ABI entry
points. The ABI omission case is inapplicable and explicitly skipped in enabled
profiles. The current 115-case run also includes private primitive metadata,
deferred effects and a lexical-budget exhaustion case
that retains values while returning null effect fields.
Module cases additionally cover class inner/outer binding identity, exported
destructuring, duplicate names/assertions, dynamic import boundaries, shadowed
or reassigned require, confined relative member comparison, UTF-16/path refusal,
whole-inventory budget failure, immutable cache links and output canaries.
Bundle cases verify the pinned upstream document, derived fixture and hashes;
object/sparse-array keys, false removed slots, exact loader structure, lexical
alpha matching, writes/dynamic lookup, partial bundles and whole-analysis limits
are covered. The upstream example does not establish the version that originally
produced it; producer verification remains false.
Both build options are restored to ON. After restoration, all 115 web cases,
the compiled-capability check and three worker cases passed again; the
backend-omission-only ABI case was explicitly skipped in that enabled profile.
`git diff --check` and formatting checks passed.

The storage regression exposed this LLVM revision's mismatch between the
temporary-file header's claimed `0600` mode and its actual `0666` creation mode
(observed as `0644` under the host umask). Storage now requests `0600` explicitly
through exclusive creation before unlinking, and validates permissions and
close-on-exec before copying any input bytes. The private/unlinked descriptor
case and write-failure case passed after that correction.

Hermes and webpack notices are staged beside the binaries and in the SDK.
Their install rules now belong to `lib/web`, because the embedded parser's
`EXCLUDE_FROM_ALL` directory does not participate in normal installation.
The generated Web install rules were exercised in an isolated prefix; every
installed Hermes license and the webpack MIT license matched the preserved
source files byte-for-byte. This verifies those notice rules, not a complete
product distribution or installation of all targets.
No new Python, JavaScript or shell implementation/test scripts were added.

## Next implementation boundary

The C++ source model now retains field roles, list ordinals and exact UTF-16
values privately. Thirteen binding cases cover shadowing/hoisting, parameter
default environments, destructuring, loops/switch/catch, class/function names,
imports/exports, CommonJS/arguments, dynamic lookup, declaration conflicts,
Unicode names and deterministic budget refusal. The two-million-step limit
was exercised with a parsed 40,000-reference source; no partial graph escaped.
SDK/CLI/worker queries expose only metadata and stable relationships.

The finite primitive evaluator now uses LLVM APFloat for binary64 operations,
bounded APInt arithmetic for BigInts, exact private UTF-16 strings and explicit
short-circuit rules. Number formatting uses the pinned embedded Hermes support
library in a separate translation unit. No LLVM/LLVH headers are mixed.
Parser/formatter calls reject an unqualified host rounding mode; APFloat
arithmetic on an admitted model retains its explicit rounding mode.
Number exponentiation and mixed BigInt comparison remain explicit refusals.

Effect summaries separate immediate evaluation from deferred function bodies,
default parameters and instance-field initializers. Static class work, computed
keys, getter/proxy uncertainty and declaration preservation have focused tests.
The source-tree validator is shared by bindings, values, effects and modules. SDK, CLI
and worker expose only fixed categories and evidence identities; computed
primitive values remain private and never grant rewrite permission.

The module profile now inventories ESM requests/entries and dynamic import /
require candidates with lexical evidence. Static relative specifiers can match
only exact admitted directory members; those candidates remain unverified runtime
targets. Source-map names and single-file imports never supply a trusted host
directory base. Every output surface keeps specifiers, names, paths and assertion
values private. Static `with` import attributes, require aliases, CommonJS export
inference and foreign export linkage are explicitly unimplemented.

The fixed `webpack5-basic-loader-v1` now recovers original source partitions,
their wrapper/body hashes and candidate dependency edges without reparsing away
outer closure context. This qualifies one basic loader shape, not arbitrary
webpack output, execution, original filenames or an entrypoint reconstruction.
An archived upstream document and its exact code-fence derivation are recorded
under `unittests/web/fixtures/webpack-manifest.json`; MIT notices are shipped.

The fixed Bun Linux x64 profile now uses bounded disk-backed ranges. Five
self-authored compiler outputs cover plain code, Unicode, actual UTF-16
storage, minified assets/maps and CJS with JSC cache/maps. Their pinned compiler
and template hashes, exact graph bytes and independent member manifests are
preserved under `unittests/web/fixtures/bun`; the native runtime is not checked
in. Full-container qualification passed locally with the original image hashes;
that test explicitly skips when the separately supplied corpus is absent.
Eight C++ core cases cover original offsets/hashes, immutable ownership,
malformed footer/ELF/pointers, aliasing, counts, encoding and refusal. Three
SDK/CLI cases and worker checks cover paging, revisions, private names and
decoded source identities reaching the C++ parser/binding analysis. Ordinary
output also hides isolated name/origin hashes. No runtime is invoked by the
analyzer or tests. Manual fixture preparation used the pinned trusted compiler
as a development activity; the optional independent recorder is C++.

The enabled profile passed all 126 owning web tests, including the full corpus
check, and the compiled-capability check. The backend-omission-only case was
explicitly skipped in that enabled configuration. Bun reference notices were
checked byte-for-byte beside binaries, in the SDK and via the generated install
rules in an isolated prefix. This does not claim a full product install.

With only the JavaScript parser disabled, all 52 remaining web cases, the
compiled-capability check and the three C++ worker protocol/graph/web cases
passed. Thus Bun extraction does not depend on the parser being enabled;
source-analysis requests correctly report unavailable.

The broader worker regression exposed a performance defect in the previously
added duplicate-key check: nlohmann 3.11.3's DOM filter callback scans the entire
parent array at every object end, even when the callback discards nothing.
This made native history save/read operations intermittently exceed their
existing request timeout. A separate optimized C++ experiment on the same
7,209,891-byte / 29,000-object input measured 45 ms for ordinary parsing and
6,135 ms for the callback path. The reader now uses the public SAX interface
for decoded-key admission followed by ordinary DOM parsing. The same local
experiment measured 43 ms ordinary / 150 ms strict after the change, with equal
results. These are local measurements, not a hard latency guarantee.
A C++ regression preserves the large array and rejects late/nested duplicate
keys. All nine worker tests then passed, including the native history case;
neither its timeout nor the history implementation was changed.

With the entire web backend disabled, both ABI availability cases passed,
including the new Bun functions. The CLI Bun command returned
`capability_unavailable` with exit status 1; protocol/graph worker tests passed
and the actual web worker case explicitly skipped. These disabled configurations
do not establish extraction support on another host or target platform.

Both build options have been restored to ON. All 126 owning web cases passed
again after restoration, including the separately supplied full Bun corpus,
and the compiled-capability case passed; the backend-omission case skipped as
expected. Formatting checks and `git diff --check` passed.

The next increment adds `bun-1.4.2-serialized-source-map-v1`: an independent
C++ reader for the pinned window/varint format and its per-source Zstd frames.
It reuses the in-process LLVM Support decoder after strict frame/size/window
preflight. No new runtime, external tool, script or package installation is
involved. The original map region, compressed source range/hash and derived
UTF-8 source identity remain separately queryable. A malformed map refuses
without losing the original extraction or publishing a partial decoded map.

The existing map C API/worker operations accept a module's
`source_map_region_id`; the CLI adds `web bun-map FILE MODULE_INDEX`.
The producer drops names, unmapped segments, source roots, file metadata and
ignore lists, so replies expose that loss rather than inferring absent data.
Container association remains unverified, and lookup never projects a retained
anchor into an intervening mapped span.

All 134 owning web tests passed with the full corpus supplied, and all nine
worker regressions passed. Six new C++ decoder cases cover both preserved
compiler maps, independently recorded original-source hashes and text anchors,
rare/multiwindow deltas, duplicate anchors, every truncation boundary, malformed
varints/masks/indexes and native frame corruption. Decompression admits exactly
8 MiB total (two 4 MiB sources) and refuses one additional byte before decoding.
Two new SDK/CLI cases check source parsing, association distinctions, cache
publication/revisions and private output with an unusable external-tool PATH.
The worker compares direct and framed map/source/segment results.

With `NEVERD_ENABLE_WEB_JAVASCRIPT=OFF`, all 60 remaining web cases passed,
including both real serialized-map fixtures and the separate full-container
corpus. Compiled-capability and three C++ worker protocol/graph/web checks also
passed; the backend-omission-only ABI case skipped as expected. A separate build
with LLVM Zstd disabled has not been qualified in this increment; the capability
and fixed unavailability path are explicit. No new public C entry point was
needed for map decoding.

The JavaScript parser is restored to ON; the web backend and LLVM Zstd are ON.
All 134 web cases, the compiled-capability case and the actual web worker case
passed after restoration; the backend-omission-only case skipped. The new
`bun-map` CLI also decoded both complete pinned `asset-map.elf` and
`cache-map.elf` files with an unusable external-tool PATH, zero exit status and
empty stderr. Their raw map hashes match the preserved manifests and storage
offsets refer to the original 81 MB containers. Run logs and these metadata-only
JSONL outputs are preserved under `/tmp/neverd-bun-qualification-1.4.2/` with
the `serialized-map-` prefix. Formatting and diff-whitespace checks passed.

This checkpoint preceded source-to-storage coordinates and source navigation/
redaction, implemented in the increments below. Other container/consumer
milestones remain in scope.
Binding identities do not establish runtime values, initialization, reflection
safety or API attribution. Annex B cases remain partial; unknown semantics fail
closed. Ordinary queries retain metadata-only output; explicit source views
follow their separate preview/publication policy.
No source transformation or runtime module loader is claimed. Bun extraction
does not authenticate the producer/version; PE/Mach-O and independent Node
family profiles remain unavailable. The five epics remain open implementation work.

## Reviewed source display increment — 2026-10-10

The C++ parser now owns a bounded lexer/comment inventory separately from syntax
validity. `javascript-source-view-v1` preserves fixed syntax and ASCII layout,
uses lexical/occurrence aliases for identifiers and masks values, comments,
hashbangs, regex/template text and unclassified regions by default. Keyword
property names are treated as target names. Originals and licenses remain in
immutable source evidence; the view is inert display text, not transformed JS.

Source-view preview publishes only counts, categories, hashes and exact review
ranges. Segment pages are available for review before publication. A matching
single-use token commits the candidate; only then can callers read UTF-8/CRLF
aligned chunks. Nonempty original ranges require an explicit local-review
caller assertion, whole lexical/layout regions and a distinct policy identity.
Policy replacement revokes a source's previous view, and import clears all views.
The two published views plus one pending candidate have independent text/record
limits. SDK and worker share the backend contract; `neverd web view` uses only
the structural default policy, with no raw-disclosure option or subprocess.

Eight new C++ core cases and four SDK/CLI cases passed. They cover private
canaries, regex/template rescans, keyword properties, private/escaped names,
dynamic/shadowed bindings, the lexer retention boundary, exact range coverage,
explicit local review, preview consumption, policy/revision invalidation,
Unicode/CRLF chunks and the published-view cache limit. The worker matches
direct/framed preview, record, commit and chunk results and refuses uncommitted
text and undeclared disclosure fields.

All 146 web cases passed with the pinned full Bun corpus enabled, including
70 source, 28 SDK/CLI, 15 map and 33 artifact/storage cases. All nine worker
regressions passed. The compiled-capability check passed; the omission-only ABI
case skipped in the enabled profile. Logs use `/tmp/neverd-source-view-*.log`.
With the JS parser disabled, all 60 remaining web cases passed, including the
new source-view unavailable-path checks. The compiled-capability check and
three C++ worker checks passed; the backend-omission ABI case skipped as expected.
With the entire web backend disabled, both ABI availability cases passed,
including all four new public entry points. CLI capabilities returned
`capability_unavailable` with exit status 1. Worker protocol/graph checks passed;
the actual web worker case explicitly skipped in that profile.

Both web options and LLVM Zstd are restored to ON. All 146 web cases passed
again with the pinned full Bun corpus, including decoded Bun source/hash to
reviewed-view integration. The compiled-capability and actual framed web worker
checks passed after restoration; the omission-only ABI case skipped as expected.
Formatting and diff-whitespace checks passed. This qualification is local macOS
arm64 Release; broader host qualification remains pending.

Feature-specific navigation/tool attribution, semantic transformation receipts,
export and consumer-wide redaction remain pending. Basic navigation and Bun
storage coordinates are implemented in the next increment.
This increment does not complete #714 or the five-epic goal.

## Source navigation and storage anchors — 2026-10-10

The bounded C++ navigation index now retains functions, calls and references,
source containment, body/name nodes, declaration/initializer binding links and
syntactic direct-function-expression links. Runtime callees remain unproven;
member access, reassignment, initialization and dynamic environments cannot be
resolved by source-name matching. Dynamic-import source arguments are not
misclassified as callees. Unavailable bindings preserve syntax while omitting
lexical links; failed navigation publishes no partial records.

The metadata-only anchor query validates UTF-8/CRLF boundaries and returns
line/UTF-16 coordinates, original storage and optional committed-view coverage.
Bun member offsets use the same Latin-1/UTF-16LE/client-UTF-8 decoder as source
extraction. Stored terminators are excluded. Compressed map sources identify
their entire original Zstd frame; standard JSON sources retain map/source IDs
with unavailable encoded-member offsets. Hidden display interiors map to whole
labels, never invented interior positions. Parse status, storage precision,
runtime uncertainty and producer authenticity remain separate.

SDK/worker operations and `navigate`/`anchor` CLI commands share these owners.
`bun-navigate`, `bun-view` and `bun-anchor` select a module index and explicit
source type in the same process. No target execution, external extractor,
script, temporary JS file or raw-source export is involved.

Five new core navigation cases, one display-location case, two Bun range cases
and six SDK/CLI cases passed. Existing preserved compiler-graph cases now also
check navigation/storage/view integration. Actual framed worker results match
direct navigation and anchor queries, including canonical-offset and unknown-
field refusal. All 160 web cases passed with the full pinned Bun corpus:
76 source, 34 SDK/CLI, 35 artifact/storage and 15 map cases. All nine worker
regressions and the compiled-capability check passed. The omission-only ABI
case skipped in the enabled profile. Logs use `/tmp/neverd-source-navigation-*.log`.
With the JS parser disabled, all 62 remaining web cases passed, including the
navigation/anchor unavailable-path checks and Bun range tests. The compiled-
capability check and three C++ worker cases passed; the backend-omission ABI
case skipped as expected. With the entire web backend disabled, both ABI
availability cases passed, including all three new entry points. CLI
capabilities returned `capability_unavailable` with exit status 1. Worker
protocol/graph checks passed; the real web worker case explicitly skipped.
Both web options and LLVM Zstd are restored to ON. All 160 web cases passed
again with the pinned full Bun corpus, and the compiled-capability and actual
framed web worker checks passed. The omission-only ABI case skipped as
expected. Formatting and diff-whitespace checks passed. This remains local
macOS arm64 Release qualification; other hosts need separate evidence.

Tool registration/configuration/permission/MCP/hook attribution, reversible
semantic transforms, other formats/platforms and the remaining
package/desktop/protocol consumers are still required for the five epics.

## Immutable native handoff increment

`immutable-native-handoff-v1` selects an original artifact or Bun asset region
by captured revision and occurrence ID. The SDK copies that immutable selection
into a bounded buffer, checks the loader's complete input SHA-256 and publishes
a new native handle only with its successful metadata response. The origin
retains either artifact/parent IDs or exact Bun container, module and byte-range
IDs. No input path is reopened. Encoded source, maps, caches and partial native
regions cannot stand in for an independent native image. Universal Mach-O is
refused until explicit slice selection exists.

ELF, COFF and Mach-O file/buffer entry points now share their format parser.
The SDK likewise shares decoder preparation and image publication. Memory
sessions omit debug-companion discovery and sidecars, reject path-dependent
persistence/patching, and retain their captured file size and hash after source
deletion. Successful ordinary file reload clears handoff provenance; failed
reload leaves the existing image usable. Explicit `native_analyze` uses the
existing static pipeline and returns metadata, not recovered original source.
CLI adds `native`, `native-analyze`, `bun-native` and `bun-native-analyze`;
the worker owns an independent handoff without replacing its native project.

The enabled-profile build passed. All 169 web cases passed with the pinned
Bun corpus, including nine new handoff cases; all 65 existing Session C API
cases passed. Handoff cases cover x64 ELF/PE/thin Mach-O and AArch64 ELF,
a PE signature beyond a 4-KiB prefix, malformed/truncated inputs, source and
web-session lifetime, sidecar/patch refusal, transactional reload, directory
occurrence identity, Bun native assets and metadata-only offline CLI output.
The full pinned Bun container was loaded and hash-checked without execution;
its complete native pipeline was not run. Both SDK and CLI static-analysis
checks use the small synthetic fixture. All new tests and fixture construction
are C++.

The first parallel worker run had four eight-second startup-handshake timeouts
before any requested operation; the new C++ web worker and the other native
operation cases passed. Without code or timeout changes, all nine worker cases
then passed sequentially and in a repeated four-way parallel run. The initial
startup cause has not been isolated; these results do not establish a hard
startup-latency bound. Logs are retained as
`/tmp/neverd-native-handoff-*-tests.log` and
`/tmp/neverd-native-handoff-worker-parallel-recheck.log`.

With only the JavaScript parser disabled, all 71 registered web cases passed,
including the nine handoff cases and full Bun corpus. The compiled-capability
case and three C++ worker cases passed; the backend-omission ABI case skipped
as expected. With the entire web backend disabled, both availability cases
passed, including all three new native entry points and output-handle clearing.
CLI `web capabilities` and `web native` returned `capability_unavailable` with
exit status 1. Worker protocol/graph cases passed and its real web case skipped.

The web backend, JavaScript parser and LLVM Zstd options were restored to ON.
The restored build passed all 169 web cases with the full Bun corpus and all
nine worker cases in parallel. Its compiled-capability case passed; the
backend-omission-only case skipped as expected. Native handoff cases explicitly
skip on Windows, where the web input reader is not implemented; that is missing
host coverage, not a successful native-import qualification. The root native
aggregate was not run for this increment; the 65-case Session C API suite and
the cross-format handoff cases are the native regression evidence.

Native input materialization is capped at 256 MiB. Existing native loader and
pipeline work/allocations have their own contracts, not the JS parser's budgets
or a new hard process bound. Standard native SDK queries retain their existing
raw-name/text disclosure contracts; the web handoff report is metadata-only.
Other hosts, universal-slice selection and broader native resource qualification
remain pending. This increment does not complete desktop/package extractors,
other Bun layouts or any of the five epics.

## ASAR and captured unpacked members — 2026-10-10

`asar-pickle-json-v1` reads explicitly selected captured occurrences entirely
in C++. It validates Pickle framing, bounded JSON, safe Unicode path identities,
all ranges and non-overlap before reading member payloads. Optional SHA-256
declarations verify the whole member and every block, including the pinned
writer's empty terminal block. Unknown integrity algorithms, mismatches,
missing unpacked files and unfollowed links remain distinct unavailable states.
Available files without a declaration retain `integrity_status: missing`.
Extraction completeness covers declared members; unreferenced payload bytes
are counted separately and preserved in the original archive. No integrity
state authenticates an application publisher.

An unpacked root must be selected from the same immutable input snapshot.
There is no host companion discovery. Exact relative names select captured
occurrences; Unicode comparison keys only reject collisions and never rename
evidence. The cumulative path allowance charges both archive declarations and
every entry indexed under the selected root, including unreferenced entries.
Fixed upstream archives, original member bytes and their hashes are recorded
in the [fixture manifest](../unittests/web/fixtures/asar/README.md).

`ArtifactView` is the shared direct-byte/origin owner for original files, Bun
assets and available ASAR members. Source, map, source-storage anchors and
native handoff consume it. Source module comparisons use the extraction's own
virtual namespace; unavailable members cannot become candidate file bytes.
Packed anchors point to archive ranges; unpacked anchors and native reports
retain the actual captured file together with its archive declaration.
Native handles keep their immutable bytes independently of source lifetime.
Session metadata and artifact pages use one analysis-status owner.

The C ABI, CLI and actual framed C++ worker expose extraction and member pages.
Queries retain revisions, cache ceilings and metadata-only output. Explicit
CLI selection also supports source, views, navigation, anchors, maps and native
handoff. The C++ CLI tests make external analysis tools unavailable through
`PATH`. Failed structural extraction publishes no cache entry; reimport
invalidates old extraction identities and four cached selections are bounded.

The native Unicode policy requires ICU 77.1 and Unicode 16.0. ASAR and ICU
licenses are preserved, staged and installed alongside existing notices.
ICU runtime distribution/static-link closure is still a release requirement;
the current host supplies its native ICU libraries. This does not qualify a
portable release, Windows capture, general archive export or the remaining
desktop adapters. See the [ASAR profile](web-asar-profile.md).

With ASAR alone disabled, the SDK exercised an admitted capture and received
`archive_path_policy_unavailable` without creating an extraction. The compiled
capability check and three C++ worker cases passed; the backend-omission ABI
case skipped as expected. With the JavaScript parser disabled and ASAR enabled,
87 web cases passed and the ASAR-omission-only case skipped. This included the
new selected-index path-budget regression, all fixed ASAR archives and the
full pinned Bun corpus. The compiled-capability and three C++ worker checks
passed; the backend-omission-only ABI case skipped in that profile.
With the whole backend disabled, both ABI availability checks passed, including
the two new ASAR exports. CLI `web capabilities` and `web asar` returned
`capability_unavailable` with exit status 1. Worker protocol/graph cases passed;
the real web worker case explicitly skipped. Logs use
`/tmp/neverd-asar-no-parser-*.log` and `/tmp/neverd-asar-no-backend-*.log`.

The backend, parser, ASAR and LLVM Zstd options are restored to ON. The restored
build passed 185 web cases, with one expected ASAR-omission-only skip out of
186 registered cases. This includes 11 ASAR core cases, five ASAR SDK/CLI cases,
the selected unpacked CLI flow, and the full pinned Bun corpus. All 65 Session
C API cases and all nine worker cases passed; the compiled-capability case
passed and its backend-omission-only counterpart skipped. The final worker run
was sequential and followed the web/native checks. Logs use
`/tmp/neverd-asar-restored-*.log`.

An earlier full worker run in this increment failed the transport mock's
eight-second initial hello deadline before any requested operation; its other
eight cases passed. The same unchanged test then passed three isolated runs,
and the final full worker run passed. Neither code nor timeout was changed to
obtain these results. The startup cause remains unisolated, consistent with
the earlier native-handoff observation; it is not reported as fixed. The
original and isolated evidence remains in
`/tmp/neverd-asar-full-worker-tests.log` and
`/tmp/neverd-asar-transport-isolated-tests.log`.

ASAR/ICU notices matched the preserved originals byte-for-byte beside binaries,
in the staged SDK and after exercising the owning Web install rules in an
isolated prefix. This qualifies those notice rules, not a full product install
or ICU runtime redistribution. Formatting and diff-whitespace checks passed.
Qualification remains local macOS arm64 Release. The existing root semantic
test discovery limitation recorded above was not bypassed or counted as a
whole-repository success. Electron manifest/entry/IPC analysis, safe export,
NW.js/VSIX and the other four epics remain required work; this increment does
not complete issue #716 or the active five-epic goal.

## Electron manifest and source boundaries — 2026-10-10

`SourceOrigins` now owns finite syntactic module provenance over the existing
binding and module owners. ESM imports, literal require candidates, unwritten
initializers, object destructuring and static members retain their module request.
Construction stays distinct from its constructor. Shadowing, writes (including
unbound require writes), dynamic references, direct eval, alias cycles, unsupported
patterns, call results and budgets never supply invented origins. This does not
establish runtime module values, initialization, reachability or call targets.

`ElectronSource` consumes those private origins and the existing finite value
analysis. Its records cover window construction, uniquely declared preload
options, constructed-window renderer/webContents operations, ipcMain/ipcRenderer
and contextBridge candidates. Each record keeps original node/range and evidence
IDs, value and callback/API nodes. Constants stay private; failed origin/value
analysis remains explicit. Getter/spread/duplicate/computed option cases cannot
invent a unique preload. Runtime targets, permissions and channel correlation
are unverified. The result is always partial source evidence, including when
no supported candidate is found.

`ElectronManifest` is independent of the JS parser. It checks the captured hash,
parses bounded JSON and compares the main entry only with exact available members
of its manifest directory. It supports the pinned Electron v40.0.0 main fallback
and extension/type entry-mode rule without selecting or executing a runtime.
ASAR manifests can link to explicitly captured unpacked main members with the
same occurrence IDs; single files have no implicit host directory. Declared
names/versions/dependencies remain private metadata, not runtime authentication.

The three C ABI exports, worker operations and direct/ASAR/Bun CLI consumers use
the shared C++ owners. Manifest caches are bounded at 16; source results share
the existing source-cache lifetime. Import replacement clears both. All new
implementation and fixture/test logic is C++; targets remain inert strings.

The enabled build passed 201 web cases with one expected ASAR-omission-only
skip out of 202 registered cases. Four new origin, four Electron source, four
manifest and four SDK/CLI cases passed, including exact anchor joins, canaries,
source/manifest lifetime, cache/revision/page refusal, explicit unpacked main
and offline CLI operation. The existing Bun CLI case also exercised the new
consumer through decoded source identities. All nine worker checks passed;
its C++ client compared actual framed manifest/source/record results with direct
calls and rejected unknown fields and stale requests. The compiled-capability
case passed; the backend-omission-only ABI case skipped in that enabled profile.
Logs use `/tmp/neverd-electron-full-*.log`.

With the JS parser disabled, 95 web cases passed and the ASAR-omission-only
case skipped, out of 96 registered cases. This included manifest comparison
and the full pinned Bun corpus; source-boundary API/CLI refusal was exercised
explicitly. The compiled-capability check and three C++ worker checks passed;
the backend-omission-only ABI check skipped as expected. Logs use
`/tmp/neverd-electron-no-parser-*.log`.

With the whole backend omitted, both ABI availability cases passed, including
all three Electron exports. Manifest/source CLI commands returned
`capability_unavailable` with exit status 1; worker protocol/graph checks passed
and the actual web check explicitly skipped. The backend, parser, ASAR and LLVM
Zstd settings were then restored to ON. The restored build again passed 201 of
202 web cases (one expected ASAR-omission-only skip), the compiled-capability
check and all nine worker checks. Logs use
`/tmp/neverd-electron-no-backend-*.log` and
`/tmp/neverd-electron-restored-*.log`. These local results do not qualify another
host or a complete Electron distribution.

Application-scoped preload/renderer association, HTML entries, IPC channel
correlation, broader dataflow/wrappers and distribution/version qualification
remain required. The existing archive/export and other desktop adapters are
also incomplete. See the [profile](web-electron-profile.md); these candidates
do not complete Electron, #716 or any of the five epics.

## Explicit Electron IPC scope — 2026-10-10

`ElectronIPC` now compares private channels from explicitly selected source
evidence in one captured manifest directory namespace. The declared exact-file
main candidate and its candidate source type are required. Original and ASAR
occurrences remain distinct even for equal bytes. Inputs are canonicalized by
source ID; no host discovery, implicit imports or source execution occurs.
Sources with unavailable origin analysis remain visible with their refusal.

Channel equality preserves exact UTF-16 including NUL, lone surrogates and
normalization distinctions. Group IDs bind the complete evidence scope and
occurrence ordinal; no channel text or isolated channel hash is output.
Endpoint records join the original source/artifact/boundary/node/range. The
three compatible direction categories report candidate pair counts instead
of allocating a quadratic edge list. Removal order, runtime roles, routing and
selection completeness remain explicitly unverified. Dynamic arguments retain
unresolved endpoints. ContextBridge keys do not become IPC channels.

The C++ C ABI, CLI and worker expose analysis plus source/channel/endpoint pages.
Limits cover 16 sources, 10,000 aggregate selected boundary records, 1,048,576
channel code units and 2,000,000 work steps including comparison units. Four
immutable scopes may be cached per revision; failure cannot publish partial
correlation state and import replacement clears them.

Five new core cases and all six Electron SDK/CLI cases passed after fixing the
CLI request's source-ID lifetime: LLVM JSON `StringRef` values borrowed each
temporary source response, so the aggregate now stores owned string copies.
Both original-directory and ASAR CLI paths pass with an unusable PATH. The
three C++ worker checks passed, including actual framed two-source correlation,
all three page kinds, private-output canaries and unknown-field refusal. Logs
use `/tmp/neverd-electron-ipc-focused-tests.log` and
`/tmp/neverd-electron-ipc-worker-tests.log`.

The fully enabled local Release build passed 208 of 209 registered web cases,
with one expected ASAR-omission-only skip. This includes the separately supplied
full pinned Bun corpus, all web source/map/native cases and the new IPC increment.
All nine worker regressions and the compiled-capability check passed; the
backend-omission-only ABI case skipped as expected. Logs use
`/tmp/neverd-electron-ipc-full-*.log`.

With the parser disabled, all five applicable Electron SDK/CLI cases, the
compiled-capability check and three C++ worker checks passed. Manifest inspection
remained available; both new IPC operations and CLI analysis returned explicit
unavailability. This focused omission run did not rerun unrelated web cases.
Logs use `/tmp/neverd-electron-ipc-no-parser-*.log`.

With the entire backend disabled, both availability cases passed, including
the two new IPC exports. The original/ASAR IPC CLI forms returned
`capability_unavailable` with exit status 1. Worker protocol/graph cases passed;
the web case explicitly skipped. Logs use
`/tmp/neverd-electron-ipc-no-backend-*.log`.

The backend, parser, ASAR and LLVM Zstd options are restored to ON. The restored
build again passed 208 web cases with one expected skip out of 209 registered,
the compiled-capability case and all nine worker checks. The backend-omission
ABI case skipped in that enabled configuration. Logs use
`/tmp/neverd-electron-ipc-restored-*.log`. Focused formatting and
`git diff --check` passed. This remains local macOS arm64 Release qualification,
not a whole-repository or other-host success claim.

Preload/renderer/HTML entry association, source import closure, window-specific
and runtime routing, broader dataflow, distribution/version qualification and
the other adapters/export consumers remain required. This is an explicit
source selection contract, not a complete application graph or completion of
any epic.

## Captured Electron entry candidates — 2026-10-10

This increment is implementation progress toward #716. `ElectronSelection`
now owns shared manifest/source admission for IPC and entry consumers.
`SourcePaths` owns finite path-expression analysis in a captured namespace;
`ElectronEntries` compares only exact available member occurrences. Relative
loadFile values use the manifest application directory, while dirname-based
expressions use their captured source directory. Relative preload literals
stay unresolved. No host filesystem lookup or target code execution occurs.

The path profile covers unwritten CommonJS wrapper roots, app.getAppPath(),
ESM import.meta roots, simple initializer aliases, string concatenation and
templates, constant conditionals, finite native path operations and captured
file URL conversion/serialization. Raw concatenation, join, resolve and
normalization retain separate rules. Unsupported path flavors, unknown cwd,
external absolute paths, unsafe names/encoding, namespace escapes, cycles,
dynamic bindings and unsupported URL references refuse. All roots, callees
and target links remain unverified runtime candidates; source provenance does
not prove initialization order, immutable properties or runtime reachability.
The complete contract and pinned API references are in the
[Electron profile](web-electron-profile.md#captured-preload-and-renderer-entry-candidates).

C ABI, CLI and framed worker expose summaries and source/entry pages, including
original node/range links, captured target artifact IDs and optionally selected
target source IDs. Private paths and URLs remain absent. ASAR occurrence
membership is retained despite equal bytes in another captured file; unavailable
unpacked members cannot supply links. Four immutable scopes may be cached per
revision; import replacement revokes them. Path work/allocation failures clear
the entire source path result, and aggregate admission failure publishes no
entry cache. Requested nodes are sorted and deduplicated for both identity and
evaluation, so reordering or repeating an admitted request cannot change its
budget outcome.

The initial focused run passed seven C++ path cases, four entry-association cases
and all nine Electron SDK/CLI cases. They cover distinct roots, ESM file URLs, unknown/refused entries,
actual independent work/string budget exhaustion, canonical request order,
scope and cache failures, source anchors, immutable capture after deletion and
private-output canaries. Original/ASAR CLI requests pass with an unusable PATH.
The direct/framed worker checks both new operations, links/refusal states,
pages and unknown-field rejection. No new scripts or external analyzers were
introduced. Logs use `/tmp/neverd-electron-entry-core-tests.log`,
`/tmp/neverd-electron-entry-sdk-tests.log` and
`/tmp/neverd-electron-entry-worker-tests.log`.

After canonicalization and URL href support, the enabled macOS arm64 Release
run passed 222 of 223 registered web cases, with one expected ASAR-omission-only
skip. It includes the separately supplied full pinned Bun corpus, 100 source
cases, all 49 SDK cases and the nine native cases. All nine worker regressions
and the compiled-capability check passed; the backend-omission-only ABI case
skipped as expected. Logs use `/tmp/neverd-electron-entry-full-*.log`.

With the JS parser disabled, all seven applicable Electron SDK/CLI cases and
three C++ worker cases passed. Manifest inspection remained available while
both entry operations and original/ASAR entry CLI forms returned explicit
unavailability. The compiled-capability check passed and the backend-omission
ABI case skipped as expected. Logs use
`/tmp/neverd-electron-entry-no-parser-*.log`.

With the entire backend disabled, both ABI availability cases passed, including
the new entry exports. Both original/ASAR entry CLI forms returned
`capability_unavailable` and exit status 1. Worker protocol/graph passed; the web
case explicitly skipped. Logs use `/tmp/neverd-electron-entry-no-backend-*.log`.

The final review added charging for repeated target normalization even when a
path expression is memoized, and for entry-index path comparison units. Its
additional long-alias aggregate-budget fixture passed in the restored build.
The enabled configuration is restored: backend, parser, ASAR and LLVM Zstd are
ON. The restored run passed 223 of 224 registered web cases, with one expected
ASAR-omission-only skip. This includes all 101 source cases, all 49 SDK cases
and the supplied full pinned Bun corpus. All nine worker regressions and the
compiled-capability case passed; the backend-omission-only ABI case skipped in
this enabled configuration. Logs use `/tmp/neverd-electron-entry-restored-*.log`.
Focused C++ formatting and `git diff --check` passed. Qualification remains
local macOS arm64 Release; no whole-repository or other-host success is claimed.

HTML script entries, source import closure, runtime/window routing, distribution
and release qualification, safe export and the remaining adapters/consumers
are still required. This does not complete Electron or any of the five epics.

## Captured HTML script candidates — 2026-10-10

This C++ increment advances #716 with a bounded UTF-8 HTML source scanner,
pinned named-character-reference data and captured local URL comparisons.
Script/base metadata keeps original ranges, first-attribute and preceding-base
evidence, fixed type/context categories and explicit refusals. It does not
construct a DOM, authenticate browser encoding or establish runtime activation.
The full contract and primary references are in the
[HTML profile](web-html-profile.md).

Eligible inline IDs select immutable raw body slices through `ArtifactView`.
Existing source parsing, navigation, reviewed display and anchors share those
bytes. Anchors compose the body and parent storage offsets, preserving ASAR
occurrence identity. Inline IDs do not authorize recursive HTML or native-image
analysis. Local external references compare only with exact available members;
unsupported URLs/bases, absent directories and namespace escapes remain explicit.
No script, browser, subprocess analyzer or remote retrieval is introduced.

Both HTML C ABI operations, worker operations and four CLI forms use the same
owners. The cache holds at most four documents per revision and import replacement
revokes derived IDs. Scan failures clear declarations; link-budget failure clears
links while retaining bounded raw inventory. Repeated inline base copies are
charged against the link work budget. The 2,231-entry WHATWG snapshot and its
license are pinned by SHA-256; its optional development recorder and all new
test logic are C++. Binary, SDK and local install notice copies match exactly.

The first focused run passed all 16 new core/SDK/CLI cases and all three C++
worker checks. Tests include every named reference and scalar array, HTML
escape/type/comment/text states, malformed inputs, real scan/link exhaustion,
portable URL refusals, source-order base candidates, immutable capture after
deletion, private-output canaries, packed ASAR offsets and namespace isolation.
CLI cases use an unusable PATH. Logs use `/tmp/neverd-html-focused-tests.log`
and `/tmp/neverd-html-worker-tests.log`.

The enabled local macOS arm64 Release run passed 239 of 240 registered web
cases, with one expected ASAR-omission-only skip. This includes the supplied
full pinned Bun corpus, 62 artifact, 101 source, 15 map, 9 native and 53 SDK
cases. All nine worker regressions and the compiled-capability check passed;
the backend-omission ABI case skipped as expected. Logs use
`/tmp/neverd-html-full-{web,api,worker}.log`.

The subsequent unpacked-HTML case checks explicit captured-directory admission,
missing-member refusal and the actual unpacked storage origin. With the JS parser
disabled, all 17 HTML core/SDK/CLI cases and three C++ worker checks passed.
Inventory and local file comparison remain available; inline parsing and source
CLI forms explicitly report `capability_unavailable`. The compiled-capability
case passed and the backend-omission ABI case skipped as expected. Logs use
`/tmp/neverd-html-no-parser-{tests,api,worker}.log`.

With the backend disabled, both availability cases passed, including the two
new public exports. Worker protocol/graph passed and the web case explicitly
skipped. All four HTML CLI forms returned `capability_unavailable` with exit
status 1. Logs use `/tmp/neverd-html-no-backend-{api,worker}.log`.

Final review added an invalid end-tag opener regression: its bogus-comment
content cannot manufacture a script candidate. The inline module integration
case also verifies that the existing file comparator reports unavailable
directory origin rather than fabricating an HTML member path.

Backend, JS parser, ASAR and LLVM Zstd are restored to ON. The final enabled
macOS arm64 Release run passed 240 of 241 registered web cases with one expected
ASAR-omission-only skip: 62 artifact, 101 source, 15 map, 9 native and 54 SDK
cases, including the supplied full pinned Bun corpus. All nine worker cases and
the compiled-capability check passed; the backend-omission ABI case skipped in
this enabled configuration. Logs use `/tmp/neverd-html-restored-{web,api,worker}.log`.
Focused C++ formatting and `git diff --check` passed. No implementation scripts
or external analyzers were introduced.

Base-aware inline module links, import-map semantics, full application import
closure, runtime/window routing, distribution qualification, safe export and
the remaining framework/package/protocol consumers are still required. No epic
is complete and no cross-host or whole-repository success is claimed.

## HTML inline module file candidates — 2026-10-10

This C++ increment advances #716 with
`html-inline-module-file-candidates-v1`. `HTMLModules` compares literal static
imports, re-exports and dynamic imports from eligible raw inline sources with
exact captured members. It shares `HTMLFiles`, declared-base selection and the
local URL owner with HTML entry linking. The inline source retains its own
occurrence identity; its document supplies explicit context without inventing
a member path. Packed/unpacked ASAR files remain in their extraction namespace.

Summary context binds HTML/script/base/source and captured namespace evidence;
query/fragment presence is nullable metadata and values remain private. Equal
inline bytes under different document/base occurrences stay distinct. HTML
selection never rebases an ordinary external file's existing analysis or cache.
Import-map declarations currently refuse association, including relative imports
that a map could replace or block. Nonliteral/unsupported requests, remote or
missing bases and source-type mismatches remain explicit. Results are partial
file candidates, not browser module identity, activation or runtime routing.
The full contract is in the [HTML profile](web-html-profile.md#inline-module-file-candidates).

Existing source-module C ABI and worker operations select this profile for
derived inline sources. `html-modules` and `asar-html-modules` select a script
record and emit all module page kinds. The first focused run exposed a CLI
status mismatch: the shared adapter accepted only `ok`, so a completed HTML
`partial` summary stopped before request pages. The adapter now accepts both
completed states; the CLI regression requires actual dynamic-import records.

The corrected focused run passed all 36 HTML/module cases and three C++ worker
checks. Tests cover preceding/later bases, UTF-8 percent-encoded paths, nullable
URL metadata, literal/nonliteral requests, map boundaries, invalid context/cache
evidence, independent work/count exhaustion and comparison after a separately
exhausted HTML entry budget. SDK cases cover context isolation, immutable capture,
packed/unpacked occurrence and anchor preservation, and external-file cache
stability. Both CLI forms run with an unusable PATH. Direct/framed worker results
agree and omit private reference canaries. Logs use
`/tmp/neverd-html-modules-focused-tests.log` and
`/tmp/neverd-html-modules-worker-tests.log`.

The first enabled local macOS arm64 Release run passed 248 of 249 registered web
cases with one expected ASAR-omission-only skip: 62 artifact, 108 source, 15 map,
9 native and 55 SDK cases, including the supplied full pinned Bun corpus.
All nine worker cases and the compiled-capability case passed; the backend-only
ABI case skipped in the enabled configuration. Logs use
`/tmp/neverd-html-modules-full-{web,api,worker}.log`.

With the JS parser disabled, 17 of 18 HTML cases passed; the new inline-context
case explicitly skipped because it requires parsing. Inventory, captured entry
links and original/ASAR CLI inspection remained available. Inline source/module
commands returned `capability_unavailable` with exit status 1. All three C++
worker checks and the compiled-capability case passed; the backend-omission
ABI case skipped as expected. Logs use
`/tmp/neverd-html-modules-no-parser-{tests,api,worker}.log`.

With the backend disabled, both availability cases passed. Worker protocol and
graph checks passed; the web case explicitly skipped. Both new CLI forms returned
`capability_unavailable` with exit status 1 before input lookup. Logs use
`/tmp/neverd-html-modules-no-backend-{api,worker}.log`.

Final review found that an unavailable inventory or early link-budget failure
could incorrectly report no import-map declaration before inspecting declarations.
The initial status is now `not_analyzed`; absence is reported only after a
complete check. A malformed inline source with a captured import map and an
early request-count failure cover this distinction. This parser-only correction
was added after the omission-profile runs above.

The backend, JS parser, ASAR and LLVM Zstd options are restored to ON, with the
existing ICU 77.1 selection. All eight inline-module core cases passed. The final
enabled local macOS arm64 Release run passed 249 of 250 registered web cases,
with one expected ASAR-omission-only skip: 62 artifact, 109 source, 15 map,
9 native and 55 SDK cases, including the supplied full pinned Bun corpus.
All nine worker regressions and the compiled-capability case passed; the
backend-omission ABI case skipped in this enabled configuration. Logs use
`/tmp/neverd-html-modules-restored-{focused,web,api,worker}.log`.
Focused C++ formatting and `git diff --check` passed. All new implementation and
test logic is C++; no subprocess analyzer, target execution or external script
was introduced.

Import-map semantics, external-source document contexts, complete application
import closure, runtime/window routing, distribution qualification, safe export
and the remaining framework/package/protocol consumers are still required.
This increment does not complete an epic or establish whole-repository or
cross-host qualification.

## Integration with current dev

The web checkpoint was integrated with `dev` at `22793fc9d`, retaining its
loader-choice/identification APIs, COFF object view, unwind discovery ordering,
UTF-8 file paths, transactional debug loading, user edits and worker background
indexing. File and captured-buffer loading continue through the same native
format readers. Rich Header and object metadata parsing consume the selected
buffer, and immutable-buffer loading explicitly refuses file-loader overrides.

The newer function, data-item, operand-format and load-option persistence APIs
now enforce the existing file-backed-session boundary. Snapshot loads skip all
sidecar discovery. The standard input-SHA query uses the captured digest and
never falls back to opening a snapshot path. C++ regression assertions cover
these additions alongside the existing snapshot/file parity and lifetime cases.

The integration build uses macOS arm64 Release, the matching prebuilt NeverD
LLVM 23.0.0 r4 package, pinned Capstone, Hermes and ICU 77.1. Web analysis, the JS
parser and ASAR are enabled; emulation, semantic tests, GUI and the Python
plugin host are disabled. This LLVM package omits Zstd, so compressed Bun-map
decoding reports its explicit unavailable capability. Earlier integrated-LLVM
Zstd-enabled results above remain separate evidence.

The clean-worktree run caught an omitted 182-byte preserved ASAR member fixture:
the original checkout contained it under an ignore rule, but the checkpoint did
not. The fixture is now tracked with its existing independently checked SHA-256
`cc402b796dc92b2b1f3a6d09515003d8400e63d8acaffc967e49c0cf015fcffe`.
All 11 ASAR core cases then passed. The repeated full web run passed 243 of
250 registered cases: six Bun-map cases require unavailable Zstd and one tests
ASAR omission only. The full pinned Bun container corpus was supplied. All nine
native handoff cases passed, including the newer persistence/SHA assertions.
The loader-choice regression and compiled-capability test passed; the
backend-omission-only ABI test skipped in this enabled build.

The current native Session C API suite passed 91 of 99 cases; eight explicitly
require ELF/glibc/System V fixtures unavailable on this macOS host. Logs are
`/tmp/neverd-js-dev-{web-fixed,asar-fixed,buffer,api,session}-tests.log`.

The first parallel worker run failed a temporary-path equality check and timed
out on a large source-cache request. The path check compares its supplied path
with the worker's canonical path; macOS's default temporary directory traverses
a symlink. With `TMPDIR=/private/tmp`, both focused cases passed in a serial
run. The complete serial worker suite then passed 21 of 24 cases; the remaining
three explicitly require a glibc host compiler/strip. No timeout or assertion was
relaxed. The retained logs are `/tmp/neverd-js-dev-worker-tests.log`,
`/tmp/neverd-js-dev-worker-focused.log` and
`/tmp/neverd-js-dev-worker-serial-tests.log`.

Build logs are `/tmp/neverd-js-dev-configure.log` and
`/tmp/neverd-js-dev-build-current.log`. The original first build was deliberately
stopped when `dev` advanced, before the incremental current-dev build completed.
Scoped formatting and the complete integration diff's whitespace check passed.
This is local integration evidence, not a whole-repository, GUI, cross-host or
Zstd-enabled qualification of the new dev baseline. The five epics remain open.

Before publication, `dev` advanced to `710b8368c` with opt-in native-to-LLVM
state-preservation proofs. That revision was merged without conflicts. The same
build targets required no recompilation. The repeated web run passed 243 of 250
cases with the same seven skips; Session C API passed 91 of 99 with the same
eight skips. Native-buffer loading and the compiled-capability check passed,
with the omission-only ABI check skipped. All five affected worker smoke suites
(Web, RealEngine, PEBrowse, NativeMapping and FunctionEdits) passed serially with
`TMPDIR=/private/tmp`. The complete worker result above remains separate from
this final smoke run. Logs are `/tmp/neverd-js-dev-final-build.log`,
`/tmp/neverd-js-dev-final-web-tests.log` and
`/tmp/neverd-js-dev-final-session-tests.log`. The newly merged proof suites were
not part of this web integration profile.

The checkpoint and integration evidence were pushed to GitHub `dev` at
`44966cccb1cf3faf31df07d4af26aa68c4a75b01`; the remote ref was checked after
the successful fast-forward push. The original working tree's unrelated changes
and dirty signature checkout were preserved.

## Captured import maps — 2026-10-10

`ImportMap` now owns bounded JSON/URL normalization and exact, prefix, scoped and
blocking resolution. It embeds unchanged Ada 4.0.0 C++ sources at
`b12a893a45809da8103bb4f1e2f6f5ee13f9100b`, with a verified archive hash and
preserved Ada/IDNA/Unicode notices. Only the library sources are compiled;
upstream generators, tools, URLPattern and external runtimes are excluded.
The dependency's symbols use hidden visibility. Its generated notice-install
rules were exercised under `/tmp/neverd-import-map-license-install`; every
installed Ada notice matched the preserved source directory byte for byte.
This checks notice installation, not a complete product distribution.

`HTMLImportMaps` retains each declaration's captured body, preceding base and
refusal state. Maps preceding all ordinary classic/module declarations compose
in source order with first definitions retained. Late/interleaved maps, unknown
absolute key origins, unsupported local key contexts, malformed maps and
exhausted budgets cannot silently fall back to an ordinary relative file.
Map and script declarations have independent bases. URL normalization preserves
query, fragment and percent spelling before a separate confined file projection.
Private context-bound URL groups distinguish several URLs naming one captured
file; no isolated URL/string hash is published. Raw references are checked before
normalization so origin-root clamping cannot erase traversal evidence. Absolute
requests cannot alias the synthetic capture origin.

The HTML module profile is now `html-inline-module-file-candidates-v2`.
SDK/CLI/worker use the same owners and publish `import_maps` pages, declaration
IDs/body ranges, counts, refusal states and private request-mapping identities.
Integrity declarations are inventoried without validating a fetch or digest.
No browser execution, activation/history equivalence or per-entry byte span is
claimed. External-source contexts and full application closure remain pending.

The first focused run passed 45 HTML/import-map cases and all three C++ worker
checks. Review then added explicit absolute/escaping-request refusal, invalid
UTF-8 admission and CLI failure for exhausted map comparisons. A compiler warning
had identified temporary JSON-key string views; they were replaced with owned
strings before the test runs. All new implementation and test logic is C++.

With JS parsing omitted, 28 of 31 HTML/import-map cases passed; three source-only
SDK cases explicitly skipped. Map inventory and native URL processing remained
available. All three worker checks and the compiled-capability check passed;
the backend-omission-only ABI case skipped. With the complete web backend omitted,
both ABI cases passed; protocol/graph checks passed and the web worker case
explicitly skipped. `neverd web html` returned `capability_unavailable` with exit
status 1 before input lookup. Logs use
`/tmp/neverd-import-maps-no-parser-{tests,api,worker}.log` and
`/tmp/neverd-import-maps-no-backend-{api,worker,cli}.log`. The final UTF-8 and
parser-dependent budget corrections were made after these omission runs.

Both web build options are restored to ON, with the same prebuilt LLVM/ICU
profile described above. The complete restored web run passed 264 of 271 cases:
72 artifact, 116 source, 15 map, nine native and 59 SDK registrations, with six
Zstd-dependent map skips and one ASAR-omission-only skip. The pinned Bun corpus
was supplied. The compiled-capability case and three C++ worker checks passed;
the backend-omission ABI case skipped as expected. Logs use
`/tmp/neverd-import-maps-restored-{build,web,api,worker}.log`.

Final link verification passed all 47 focused cases and three worker checks.
Two further parsed-fixture tests independently exhaust normalization with long
bases and repeated module/map comparison work; both clear candidate results and
pass. The final complete run passed 266 of 273 registrations with the same seven
skips (73 artifact, 117 source, 15 map, nine native and 59 SDK cases). All three
worker checks passed. Logs use `/tmp/neverd-import-maps-final-focused.log`,
`/tmp/neverd-import-maps-budget-tests.log` and
`/tmp/neverd-import-maps-publication-{build,web,worker}.log`.
Ada notices are also staged beside the shared library and inside the SDK; both
staged directories matched `LICENSES/ada` byte for byte. Scoped formatting and
`git diff --check` passed. Existing duplicate-library linker warnings remain;
there were no new compiler diagnostics in the final build.

Publication also incorporated `dev` at `aea8fd619`, including GUI text selection
and realigned x86 native EH work, through a conflict-free merge. The affected
native libraries rebuilt successfully. All 59 selected HTML/import-map/native
and ASAR CLI cases passed, including the pinned Bun native handoff. Session C API
passed 91 of 99 cases with its same eight platform skips; the native-buffer
loader case and seven web/native worker smoke suites passed. Logs use
`/tmp/neverd-import-maps-dev-{build,tests,session,loader,worker}.log`.
The upstream GUI and dedicated realigned-EH suites were not part of this web
integration profile; their qualification is not inferred from these checks.

The five epics remain open. This increment qualifies captured import-map
candidates locally; package graphs/diffs, passive observations, other desktop
and native JS formats, semantic rewrites, C++ MCP and cross-host distribution
still need their planned implementation and evidence.
