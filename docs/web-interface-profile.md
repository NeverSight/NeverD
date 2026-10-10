# Passive interface evidence profiles

This increment of [#717](https://github.com/NeverSight/NeverD/issues/717) supports
HAR-only, source-only and explicitly selected source/HAR comparison. All
implementation and fixtures are C++; the already embedded Ada 4.0.0 parser
handles URL serialization. No input code, URL, body, vendor extension or
declared script is executed, requested, replayed or uploaded.

## HAR admission and redaction

`har-1.2-metadata-v1` accepts UTF-8 JSON, including a leading UTF-8 BOM,
with `log.version = "1.2"` and an entries array. This is a documented subset of
the [HAR 1.2 format](https://github.com/ahmadnassri/har-spec/blob/master/versions/1.2.md),
not the abandoned W3C HAR draft or a claim of full HAR validation.

Original artifact identity and SHA-256 cover every supplied byte, including
the BOM. Entry order is preserved. Observation, request and response IDs use
the capture identity and occurrence ordinal; they never reuse vendor IDs or
hash private endpoint values. A request/response ID can identify an absent
side, so consumers must inspect its `*_present` field.

The reader retains only declared recognized methods, HTTP status 0 or 100..599,
field presence/counts and timestamps in a narrow validated RFC3339 subset.
Timestamps require a valid Gregorian date, seconds 00..59, optional 1..9
fractional digits and Z or a numeric offset. Missing/invalid timestamps remain
distinct. Capture creator names/versions, arbitrary methods, unknown header
names, URLs, query names/values, cookies, bodies, redirects, IP addresses,
comments and vendor values are withheld from normal output. Only a fixed
allowlist of header-name classifications can be displayed. This blanket
policy also covers credentials or personal data embedded in paths and names;
it does not depend on recognizing a token pattern.

Headers/cookies/query/parameter lists report absent versus present-empty and
incomplete entries. Missing requests, responses and body text are supported
partial evidence. Wrong types, duplicate decoded JSON keys, invalid status,
conflicting postData text/params, truncated JSON or budget excess fail
atomically with fixed diagnostics. Bodies, including base64 content, are
never decoded. Custom fields are counted as unsupported; known
`_webSocketMessages` presence is reported, but no frames are imported.
This profile does not validate timings, HTTP sizes or capture authenticity.

HAR admission has two explicit phases. A current-revision preview that passes
the API/transport argument boundary revokes any earlier pending preview before
lookup or parsing, including if the new preview then fails. It returns only
redaction/coverage counts and a token bound to the original artifact/hash,
revision, profile, policy and preview sequence. Observation pages become
readable only after commit. Every same-revision commit attempt consumes the
pending token. A stale-revision request cannot disturb the current preview.
Reimport clears pending and published analyses and comparisons.

## Source candidates

`direct-fetch-websocket-syntax-v1` consumes the shared parser, lexical binding
and primitive-value models. It recognizes direct external `fetch(...)` and
`new WebSocket(...)` syntax. A lexically external name is **not proof of a
platform intrinsic**: every result remains static inference with unknown
runtime identity and reachability. Shadowed/written names are excluded;
possible direct eval refuses the analysis. Member callees, aliases, wrappers,
XHR and interprocedural request flow are unsupported in this profile.

Literal strings, supported primitive concatenation and fully evaluated template
literals provide private URL candidates. Unresolved template expressions,
identifiers and other dynamic expressions retain source node/range links and
unknown values. Only omitted fetch init supplies a default GET candidate.
An explicit object must contain distinct non-computed ordinary data properties;
spread, getters, methods, duplicates and `__proto__` prevent selecting options.
Absent own `method` is unknown because inherited properties can participate
in [Web IDL dictionary conversion](https://webidl.spec.whatwg.org/#es-dictionary).
Only the six methods specified by
[Fetch normalization](https://fetch.spec.whatwg.org/#concept-method-normalize)
are uppercased; lowercase `patch` is not promoted to `PATCH`.
CONNECT/TRACE candidates carry a forbidden-method classification.

Records link call, URL, options, method, headers and body syntax nodes to exact
source byte coordinates. No source text or private endpoint strings are
published by these APIs. Existing source navigation and explicitly reviewed
source views supply the separate local inspection path. Response handling,
parameter schemas, wrapper ownership, serialization/compression/crypto chains
and semantic behavior remain unimplemented.

## Explicit correlation

`absolute-http-method-origin-path-v1` compares one selected source analysis
with one committed HAR capture. Only fetch candidates with recognized methods
and absolute HTTP(S) URLs qualify. Ada-serialized full origin and path must
match exactly; no suffix, wildcard, route-template or inferred-base matching
occurs. Special URL forms without explicit authority, such as
`https:host/path` and `https:/host/path`, remain base-dependent; successful
standalone URL parsing cannot substitute for the unknown environment base.
Query strings, fragments, headers, bodies and timing are explicitly
excluded from comparison. Distinct queries can therefore yield multiple
candidate pairs; this is never evidence that the source caused an observation.

Credentials and fragment presence are classified before private values are
discarded. Credential-bearing fetch URLs retain their rejection under the
[Request constructor](https://fetch.spec.whatwg.org/#dom-request); they cannot
become eligible after redaction. WebSocket URLs with fragments, including an
empty fragment, retain the
[constructor limitation](https://websockets.spec.whatwg.org/#dom-websocket).
WebSocket source records never join ordinary HAR HTTP observations.

All matching occurrences remain separate, with pair IDs derived from public
source/observation evidence IDs. Results preserve static inference and imported
observation classes, never assert execution, authenticated capture, analyst
confirmation or server completeness. Zero pairs means no match under this
rule, not proof of absent behavior. Relative/dynamic URLs stay unresolved.

## Budgets and interfaces

| Resource | Bound |
|---|---|
| HAR original | 8 MiB |
| JSON depth / nodes / one string | 64 / 200,000 / 4 MiB |
| HAR entries or source interface records | 4,096 |
| HAR header/cookie/query/post-parameter records | 32,768 aggregate |
| One raw or serialized URL / private origin+path bytes per analysis | 16 KiB / 2 MiB |
| Source/HAR analysis steps | 2,000,000 |
| Correlation pairs | 32,768; excess fails atomically |
| Retained captures / source analyses / comparisons | 4 each |
| Pending capture previews | 1 |
| Page records | 1..128 |

The public C API adds `neverd_web_har_{preview,commit,records}_json`,
`neverd_web_interfaces_{analyze,compare}_json`,
`neverd_web_interface_records_json` and
`neverd_web_interface_correlation_records_json`. C++ worker operations use
the same names without `neverd_`/`_json` and reject unknown request fields.
HAR works with the JS parser disabled; source analysis and comparison report
`capability_unavailable`. All symbols retain unavailable stubs when the whole
web backend is omitted.

CLI examples for a single captured file (artifact index 0):

```sh
neverd web har-preview capture.har
neverd web har-import capture.har 0 passive-interface-metadata-v1 <previewed-sha256>
neverd web interfaces client.js module
```

For an explicitly inventoried root:

```sh
neverd web interface-correlate ROOT module SOURCE_INDEX HAR_INDEX \
  passive-interface-metadata-v1 PREVIEWED_HAR_SHA256
```

Each CLI invocation creates a new session. The second invocation explicitly
accepts the displayed policy and original SHA-256, repeats the bounded preview,
and commits only if both match; it never attempts to reuse another session's
token. Changed original bytes or a different policy refuse publication.

## Qualification scope

`WebInterfaces`, `WebSourceInterfaces`, `WebInterfaceSDK` and the C++ worker
regression use self-authored inert fixtures. They exercise secret canaries in
URLs, names, headers, cookies, query, bodies, errors and extensions; preview
revocation, immutable storage, missing fields, duplicate occurrences,
shadowing/eval, prototype/method/credential exclusions and fanout refusal.
CLI and framed worker checks use an unusable external-tool PATH.

This does not complete #717. XHR/request wrappers, response and transformation
tracing, schema exports, reviewed endpoint displays, C++ MCP transport and
broader host qualification remain required. Separately selected
[SSE/JSONL/JSON-RPC/MCP-shape/log adapters](web-stream-profile.md) now provide
passive framing and recorded-context candidates; source-to-stream correlation
remains unsupported.
No Claude Code traffic or transcript is invented.

With the optional `NEVERD_CLAUDE_CODE_21296_ELF` input, a C++ regression
re-extracts module 1,897 from the hash-pinned original using NeverD's Bun
reader. It checks one fetch candidate at source byte 65,473 with length 52,
retaining its dynamic URL and unresolved options as unknown. This qualifies
the container→source→interface path; it does not recover that runtime endpoint
or supply traffic observations.
