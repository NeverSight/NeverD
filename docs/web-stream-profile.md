# Passive stream evidence profiles

These native C++ readers implement the offline input-adapter part of
[#717](https://github.com/NeverSight/NeverD/issues/717). Select the interpretation
explicitly. No reader executes a target, launches a runtime, connects to an
endpoint, replays a request or supplies missing traffic.

## Selected input interpretations

| Profile | Framing and interpretation |
|---|---|
| `jsonl-metadata-v1` | UTF-8 JSON values, one per LF/CRLF line; no protocol inference |
| `sse-utf8-metadata-v1` | SSE line/block framing and withheld payload counts |
| `jsonrpc-2.0-jsonl-v1` | JSON-RPC object shapes over LF/CRLF; direction/session absent |
| `mcp-2025-06-18-stdio-jsonl-v1` | Explicit MCP base-message shape subset and fixed known-method classes over LF/CRLF |
| `recorded-jsonrpc-2.0-jsonl-v1` | NeverD's recorded envelope below, with optional session/direction evidence |
| `diagnostic-lines-v1` | Opaque UTF-8 lines; object/array-looking lines may supply JSON shape metadata, never protocol inference |

Original hashes include every byte, including delimiters and a BOM. Each
record retains its original byte offset/length and physical line location.
Capture identity includes original artifact identity, original hash, selected
profile and policy. Record identity derives from capture identity and occurrence
ordinal, never an unredacted recorded ID or payload.

JSON profiles reject a BOM. Blank lines and malformed JSON remain located
limitations. A valid final JSON value without LF remains a value with
`terminated=false`; an invalid final fragment is `possibly_truncated_json`,
not proof that truncation caused the error. CR alone is not a line delimiter.
Strict shared JSON admission rejects duplicate decoded keys and malformed
number spelling. No batch is expanded: arrays are explicit unsupported
protocol records. Diagnostic noise remains opaque.

SSE follows the line/block rules of the
[WHATWG HTML snapshot dated 2026-10-09](https://html.spec.whatwg.org/multipage/server-sent-events.html#parsing-an-event-stream):
one leading BOM, CR/LF/CRLF delimiters, blank-line dispatch, data-line joining
and persistent ID state. An empty data field still permits a dispatched event.
Comments, unknown fields, retry fields and invalid NUL-containing IDs have
separate counts. An empty ID resets the ID buffer. Only buffer presence is
retained; ID values are discarded. The selected event name is classified as
default/message or custom-withheld. EOF without a final blank line preserves
an unfinished block; it never becomes a dispatched event. Joined payload byte
counts account for intervening LF bytes, but payloads are neither emitted nor
interpreted as JSON-RPC. Invalid UTF-8 is refused rather than replaced as in a
browser decoder. This is framing evidence, not evidence of a live EventSource,
HTTP headers, reconnects, message delivery or a complete session.

## Protocol shapes and relationships

The [JSON-RPC 2.0 specification, updated 2013-01-04](https://www.jsonrpc.org/specification),
defines the selected request/notification/response shapes. We retain fixed
shape classifications and field presence only. Error-code integrality is
checked from original decimal spelling independently of the ID join rules;
DOM rounding cannot turn a fractional error code into an integer.

The MCP profile uses the
[2025-06-18 base message shape](https://modelcontextprotocol.io/specification/2025-06-18/basic#messages)
and [newline stdio framing](https://modelcontextprotocol.io/specification/2025-06-18/basic/transports#stdio).
It admits object params/results and non-null string or canonical safe-integer
IDs. Other numeric ID spellings/ranges are explicitly unsupported. Method names
come from a fixed allowlist; extensions stay `other_redacted`. Selection does
not prove version negotiation, validate method-specific schemas, establish
initialization, authenticate the capture or demonstrate permission enforcement.
Absent initialization and a supplied different protocol version do not change
that claim. Content-Length framing, HTTP transport, cross-file sessions and
protocol-specific diagnostic formats are outside these profiles.

The recorded-envelope profile is an **analysis input format defined by NeverD**,
not an MCP wire format:

```json
{"session":"example-session","direction":"client_to_server","timestamp":"optional recorded text","message":{"jsonrpc":"2.0","id":1,"method":"example","params":{}}}
{"session":"example-session","direction":"server_to_client","message":{"jsonrpc":"2.0","id":1,"result":{}}}
```

Session names, recorded IDs and timestamps stay private. Timestamp output
reports presence/type only; no date validation or time-based ordering is
claimed. Unknown envelope fields are ignored. A link requires one request and
one later response with the same nonempty recorded session, opposite recorded
directions and exact typed ID. String `"1"` differs from number `1`.
Numbers must have original plain-integer spelling and be within
[-9007199254740991, 9007199254740991]; fractional/exponent spellings never
silently inherit a rounded DOM value. Null IDs never link.

Repeated requests or responses for the same session/ID/request direction make
that group ambiguous, including sequential ID reuse. Response-before-request
groups never link. Malformed/unsupported protocol records, unjoinable numeric
IDs or requests/responses lacking recorded context set `coverage_gap` and
block all links in the capture, because omitted evidence could conceal a
duplicate. Raw JSON-RPC/MCP profiles have no recorded context and do not link.
SSE IDs never become request IDs. Links are recorded-ID **candidates**, not
authenticated delivery or evidence of source execution. Zero links does not
prove absent communication.

## Redaction and publication

`passive-stream-metadata-v1` withholds every raw payload, log line, arbitrary
property/method/event name, ID, session and timestamp. Normal pages expose
only fixed classifications, counts, original coordinates and occurrence IDs.
No sensitive-value recognizer is needed to decide whether these classes of
data are withheld. Private correlation keys exist only during inspection;
retained capture models contain metadata alone. Original bytes remain in the
shared immutable private spool.

The three C API exports are `neverd_web_stream_preview_json`,
`neverd_web_stream_commit_json` and `neverd_web_stream_records_json`.
C++ worker operations use the same names without `neverd_`/`_json`.
A dispatched current-revision preview revokes the preceding pending preview,
even if lookup or parsing fails. The new token binds capture/revision/policy
and sequence. Every same-revision commit attempt consumes it. Stale revisions
cannot disturb current pending state. Reimport revokes pending and published
captures. Records are unavailable until commit.

```sh
neverd web stream-preview capture.jsonl recorded-jsonrpc-2.0-jsonl-v1
neverd web stream-import capture.jsonl recorded-jsonrpc-2.0-jsonl-v1 0 \
  passive-stream-metadata-v1 PREVIEW_RECEIPT
```

Each CLI invocation creates a fresh session. `redaction_receipt` binds the
previewed original SHA-256, selected profile and policy. Import repeats the
preview and requires matching receipt/policy. Changed bytes or a different
interpretation invalidate the receipt. Single-file artifact index is zero;
an inventoried directory root occupies index zero before its members.

## Bounds and qualification

| Resource | Limit |
|---|---|
| Original | 8 MiB |
| One physical line or SSE block, including delimiters | 256 KiB |
| Records | 4,096 |
| JSON depth / aggregate preflight nodes, including failed records | 32 / 200,000 |
| Private recorded session/ID material | 1 MiB |
| Retained captures / pending previews | 4 / 1 |
| Page size | 1..128 |

Byte, encoding and budget failures abort the analysis before publication.
JSON budget failures are never downgraded to ordinary malformed records.
Budgets constrain this analysis, not all SDK sessions or whole-process RSS.
Readers remain available when the JavaScript parser is disabled. All three
ABI symbols return unavailable when the whole web backend is disabled.

C++ fixtures cover split SSE events, sticky/reset/invalid IDs, unfinished EOF,
unknown events/methods, duplicate JSON keys, mixed diagnostic noise, exact
numeric ID counterexamples, duplicate/absent context, out-of-order responses,
shared successful/failed JSON work, canary exclusion and immutable previews.
SDK/CLI/worker checks exercise receipts, revocation, bounded caches and direct/
framed parity with an unusable external-tool PATH.

No real Claude Code transcript was supplied or invented. These readers can
inspect supplied files alongside recovered source, but do not establish a
Claude-specific stream format or source-to-message correlation. Such evidence,
full C++ MCP server transport, schema exports and broader host qualification
remain separate acceptance work.
