# Captured HTML script candidates

`html-utf8-script-candidates-v1` is a bounded C++ source scanner for explicitly
selected captured HTML. It inventories script/base declarations, compares local
references with captured occurrences and supplies eligible inline byte slices
to the existing JavaScript source APIs. It never constructs a DOM, fetches a URL,
launches a browser/tool or executes target code. Successful inventory and link
summaries remain `partial`.

## Source and context contract

The caller selects a UTF-8 profile; valid bytes alone do not establish a
browser's encoding choice. NUL, invalid UTF-8, conflicting charset declarations
and legacy `http-equiv=content-type` declarations refuse the document. No encoding
sniffing, transcoding or HTML input preprocessing is claimed. Inline bodies are
exact original bytes, including CRLF and entity spellings, rather than a
reconstruction of browser-processed source text.

Tags and attributes use ASCII case folding. Quoted values preserve original
ranges; duplicate attributes keep the first value and set a flag. The C++
attribute decoder uses all 2,231 pinned WHATWG named references, numeric scalar
replacement and legacy attribute ambiguity rules. The immutable
[fixture and recorder](../unittests/web/fixtures/html/README.md) document the
snapshot hash and [license](../LICENSES/whatwg/LICENSE).

Script scanning distinguishes data, escaped and double-escaped states. Closing
HTML script tags terminate bodies regardless of JavaScript quoting. Comments,
raw-text/RCDATA containers, `noscript` under the scripting-enabled candidate
assumption, and `plaintext` do not invent nested scripts. Template and foreign
records are ineligible for inline-source recovery or entry linking. Foreign
integration points and browser tree construction remain unverified. Unsupported
token structures clear the inventory; an unterminated script retains its range
and refusal status but no inline ID.

Classic MIME types, modules, import maps, speculation rules and data blocks are
distinct. MIME parameters do not match a JavaScript MIME essence. Whitespace is
stripped only for the classic MIME check: `type=" module "` is a data block.
`type` takes precedence over `language`. These rules reference the
[script element](https://html.spec.whatwg.org/multipage/scripting.html) and
[tokenization specification](https://html.spec.whatwg.org/multipage/parsing.html).
DOCTYPE support is a strict `html`/PUBLIC/SYSTEM subset; unsupported identifiers
refuse instead of hiding markup inside guessed quotes.

## Captured local reference candidates

Each script retains the first preceding eligible `base href` from the source
scan. That base resolves against the captured document location; later bases do
not replace it. This is a source-order candidate, not the browser's eventual
frozen base or async/module preparation order. DOM edits, insertion modes,
CSP, integrity verification, activation and execution timing are not evaluated.
Import maps have the separate candidate profile below. See the
[base element reference](https://html.spec.whatwg.org/multipage/semantics.html#the-base-element).

The local URL subset supports relative components, dot segments, UTF-8 percent
decoding, directory bases and query/fragment presence. Query/fragment values
stay private and do not participate in file comparison. Encoded separators,
invalid encoding, absolute/device/network URLs, backslashes, unsafe portable
names, internal controls, empty path components and traversal above the capture
root refuse. Unsupported bases do not silently fall back to a local directory.
An empty `src` has a separate refusal status.

Only exact available members in the same namespace can supply a target. No host,
extension/index/package or basename lookup occurs. ASAR members use their
extraction namespace, including explicitly associated unpacked files. Equal
original bytes do not replace an ASAR occurrence. A standalone HTML file or Bun
asset has no captured directory base; eligible inline bytes remain usable.

Inline identities bind the document occurrence/profile, tag position, body hash
and source type. `ArtifactView` supplies an immutable slice. Source anchors add
the body offset to the original or ASAR/Bun storage offset and retain the parent
origin. Inline IDs do not authorize native handoff or recursive HTML inspection.

Source parsing, bindings, navigation, anchors and reviewed display reuse their
existing C++ owners. The inline source remains a derived occurrence; its HTML
document supplies context without manufacturing a captured member path.

## Inline module file candidates

`html-inline-module-file-candidates-v2` reuses the local URL and exact captured
member rules above. Literal static imports, re-exports and dynamic imports in
an eligible inline source use its document location and preceding base candidate.
The source must have been parsed with the script record's declared source type.
Classic inline sources can supply dynamic-import candidates; `require` calls
remain unverified. Without an eligible import map, bare names, absolute URLs,
nonliteral imports and unsupported local URLs do not acquire a target.
Missing or remote bases cannot fall back to
the capture root. No extension, package, host-file or recursive source lookup
occurs.

The context binds the HTML document, script occurrence, source and captured
namespace. Equal inline bytes under different HTML/base occurrences retain
different source/context IDs and can select different captured files. Original
and ASAR namespaces remain separate, including explicitly associated unpacked
members. Selecting an external script through HTML does not attach document
context to that file's existing module analysis; it retains
`admitted-relative-file-v1`.

These are file comparisons, with `link_status: partial` and
`runtime_base_verified: false`. Without import maps,
`module_url_identity` remains `not_modeled`.
Query/fragment presence is reported only after an admitted local URL comparison;
their private values do not participate in file matching. Two URL spellings
that match the same file do not prove the same runtime module identity. The
[HTML module resolver](https://html.spec.whatwg.org/multipage/webappapis.html#resolve-a-module-specifier)
uses a script base and import-map state; this profile does not reconstruct
browser preparation order or execute imports.

## Captured import-map candidates

`import-map-url-evidence-v1` parses captured JSON in C++ and normalizes URLs with
the embedded, hash-pinned [Ada 4.0.0 source](../LICENSES/ada/README.md). The build
compiles existing C++ sources directly; no upstream generator, CLI, browser or
JavaScript runtime runs. The parser is independent of the JS syntax backend.

The owner validates the top-level `imports`, `scopes` and `integrity` object
shapes. Exact definitions precede matching prefixes; the most specific scope
precedes less specific scopes and top-level imports. Non-string/invalid addresses
and invalid prefix addresses become blocking entries. A blocking match or prefix
backtracking terminates resolution, with no fallback. Empty keys and unknown
top-level members are counted. Integrity declarations are inventoried without
verifying a fetch or digest. Duplicate decoded JSON keys, lone surrogates and
keys/scopes that collide after URL normalization explicitly refuse the map;
this profile never guesses an ordering lost by the JSON representation.

For HTML consumption, all ordinary-context map declarations must precede every
ordinary classic/module script declaration. Maps compose in source order with
the first definition winning and an explicitly assumed empty prior resolved
module set. A later/interleaved declaration yields
`html_import_map_timing_unverified`; an invalid, external or unclosed map cannot
silently disappear. Inert template/foreign records remain visible but do not
participate. This establishes a source-order candidate only: DOM construction,
CSP, modulepreload, event handlers and dynamic insertion can change real browser
activation/history and are not reconstructed.

Map keys and addresses use the map declaration's preceding base; a request uses
its own script's preceding base. Private synthetic capture URLs preserve percent
spelling, query and fragment while comparing normalized keys. An absolute,
network or root-relative key/scope could shadow a relative request under the
unknown real origin, so it refuses HTML association with
`html_import_map_origin_unverified`. Unsupported local keys/scopes also refuse
association. Absolute/root-relative mapped addresses remain nonlocal candidates
and never become captured files, even if they spell the private synthetic origin.
Absolute/network/root-relative requests likewise remain unresolved because the
real document origin is unknown. Raw relative requests, keys and addresses must
pass capture-root checks before their normalized
URL can supply a file candidate; origin-root clamping cannot erase an escape.

Only a URL that separately passes portable captured-file projection receives a
`module_url_candidate_id`. IDs are assigned to distinct private serialized URLs
in request order and bound to the containing source/context; they do not hash
isolated secret values. Equal URLs share an ID within that context. Different
queries, fragments or percent spellings can have different IDs while naming the
same captured file. They are not browser module identities. Such contexts report
`module_url_identity: capture_context_url_candidate`; matched declarations,
scope and entry IDs preserve the mapping evidence without publishing strings.

External-source document contexts, actual browser merge history and complete
application import closure remain follow-up work.

## Interfaces

`neverd_web_html_analyze_json(session, revision, artifact_id)` and
`neverd_web_html_records_json(session, revision, html_id, kind, offset, limit)`
use the existing length-bearing C ABI and owned-string conventions. Record kinds
are `scripts`, `bases` and `import_maps`. Pages contain identities, ranges, fixed categories,
presence flags and candidate artifact IDs. Raw attributes, paths, URLs and body
text are absent under `metadata-only-v1`. Import-map pages expose the declaring
script/body range, parse/refusal status, counts, base status and presence flags.
They never expose raw keys, scopes, addresses or integrity values. A map record
anchors the complete body; per-entry byte spans are not fabricated.

Worker operations are `web_html_analyze` and `web_html_records`; unknown fields
are refused. Inventory works without the JS parser, while parsing an inline ID
then returns `capability_unavailable`. With the backend omitted, both ABI symbols
remain available and return that fixed failure.

After parsing an inline ID, the existing `source_modules_analyze` and
`source_module_records` operations select the HTML profile automatically. Their
`link_context` carries document/script/base IDs and fixed statuses, never paths
or import-map text. Ordinary file contexts remain JSON null. The same C ABI and
worker operations are used; module analysis still requires the JS parser.

```console
neverd web html ./captured-app 2
neverd web html ./index.html 0
neverd web html-source ./captured-app 2 0
neverd web html-modules ./captured-app 2 0
neverd web asar-html ./app.asar 0 - 1
neverd web asar-html-source ./app.asar 0 - 1 0
neverd web asar-html-modules ./app.asar 0 - 1 0
```

Indices are illustrative: choose the captured artifact/member ordinal, then a
zero-based script-record ordinal. Inventory commands print all three page kinds.
Their `-source` forms parse one eligible inline or exact external candidate with
its reported type in the same session. Unresolved selections fail. `-` leaves
unpacked resources unassociated. The `-modules` forms additionally emit the
module summary and request/import/export/attribute pages. Completed partial
comparisons return exit status 0; unavailable or exhausted analyses return 1.

## Budgets and qualification

| Resource | Ceiling |
|---|---:|
| HTML bytes / cached documents per revision | 4 MiB / 4 |
| Scan/attribute work / link work units | 16,777,216 / 4,194,304 |
| Combined script and href-base records | 10,000 |
| Examined markup starts / context depth | 100,000 / 64 |
| One tag / attributes per tag / one attribute | 64 KiB / 256 / 16 KiB |
| Total decoded attribute bytes | 1 MiB |
| Local path / namespace occurrences including virtual root | 4 KiB / 10,001 |
| Eligible inline body / record page | 1 MiB / 1–512 |
| Inline module link work / requests | 4,194,304 / 10,000 |
| Import-map declarations / aggregate body bytes | 64 / 1 MiB |
| Aggregate import-map records / URL or encoded JSON string bytes | 10,000 / 4 KiB |
| Import-map JSON depth / normalization work | 16 / 4,194,304 |

Scan failures discard all declarations. Link-budget failure discards all links
but preserves separately bounded source inventory. Repeated base-path copies and
comparisons count against aggregate work, including inline scripts. These are
cooperative logical ceilings, not whole-process CPU/RSS guarantees. Import
replacement revokes HTML records and derived inline IDs.
Inline module linking has its own work budget for namespace construction,
base/specifier text and comparisons. Exhaustion clears all its candidate links;
the separately bounded module inventory remains available. An exhausted HTML
entry-link result can still supply raw inline evidence, but the module consumer
must independently validate and budget its context.
An unavailable inventory or early budget failure does not claim that import-map
declarations are absent; that status is published only after a complete scan.
Map-inventory exhaustion clears its declarations and prevents default-URL
fallback; script inventory remains independently available. Each map has bounded
JSON admission and normalization; document aggregate accounting includes the
completed map work, with at most one bounded in-flight map before exhaustion.
Module comparisons use their separate charged work budget. These limits do not
claim exact internal Ada/JSON instruction counts or whole-process RSS limits.

C++ tests check the full named-reference table against the pinned JSON and its
independently decoded scalar arrays, token/type/escape states, raw ranges,
malformed inputs (including bogus end-tag comments), real work/count exhaustion,
local URLs and base selection.
SDK/CLI/worker cases cover immutable capture, private-output canaries, nested
packed/unpacked ASAR offsets and explicit association, occurrence isolation,
pages, cache/revision failures and an
unusable PATH. Build-profile results appear in the
[implementation ledger](web-analysis-implementation.md).
Inline module tests additionally cover preceding-base selection, Unicode URLs,
query/fragment metadata, literal/nonliteral imports, map boundaries, source and
cache mismatches, context isolation and real work/count exhaustion. SDK/worker
tests compare original and packed/unpacked ASAR candidates without publishing
private specifiers; CLI cases verify that request pages survive partial status.
Import-map cases add exact/prefix/scoped/null resolution, independent bases,
first-definition composition, normalized-key refusal, native Unicode URL
normalization, query/fragment/percent identity, origin and timing boundaries,
aggregate exhaustion and ASAR occurrence isolation. SDK/CLI/worker checks cover
private mapping metadata, immutable bodies and the additional page kind.

This does not establish complete Electron entry/import closure, DOM equivalence,
runtime reachability or completion of #716.
