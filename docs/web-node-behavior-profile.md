# Offline Node source behavior evidence

`node-22.15-static-behavior-v1` is a bounded C++ consumer of NeverD's captured
source, bindings, module requests, module origins and primitive values. It
classifies import-rooted call syntax and optionally joins selected sources to
an existing package inventory. It never installs packages, runs scripts,
decodes payloads, invokes another analyzer or contacts a service.

## Rule contract

Rules use the Node **22.15.0** documented API names. A matching source expression
does not authenticate the runtime module, prove callability or reachability,
establish successful I/O, or supply a malicious/benign verdict. Zero matches is
partial coverage, not a clean result. Unavailable upstream analysis produces
an explicit unavailable or budget-exceeded result with no retained findings.

Both exact `node:` and bare builtin requests are admitted. Shared origin
evidence supports literal require, ESM imports, unwritten aliases, object
destructuring and static member chains. `javascript-module-origin-candidates-v2`
retains the exact ESM import row. Only a documented default import, including
`{default as name}`, removes its first `default` projection. CJS `.default`,
namespace `.default`, repeated defaults, call-return aliases and constructed
instances are not normalized into a builtin module.

The rule vocabulary is closed:

| Import and exact member | Record kind |
| --- | --- |
| `child_process`: `exec`, `execSync`, `execFile`, `execFileSync`, `spawn`, `spawnSync`, `fork` | `process_creation_call` |
| `fs`: `write`, `writev`, `writeFile`, `appendFile`, `mkdir`, `rename`, `copyFile`, `cp`, `rm`, `unlink`, `truncate`, `chmod`, `chown`, each with its documented `Sync` form; also `createWriteStream` | `filesystem_mutation_call` |
| `fs.promises` or `fs/promises`: `writeFile`, `appendFile`, `mkdir`, `rename`, `copyFile`, `cp`, `rm`, `unlink`, `truncate`, `chmod`, `chown` | `filesystem_mutation_call` |
| `http` or `https`: `get`, `request` | `outbound_request_call` |
| `process.dlopen` | `native_load_call` |
| `buffer.Buffer.from` | `buffer_conversion_call`, or the qualified encoded-data case below |

The promise table is independent: it contains neither module-level `write`
nor `createWriteStream` nor synchronous methods. Generic `open` is not a write
finding. This profile does not follow global `process`/`Buffer`, arbitrary
wrappers, FileHandle methods, shell commands or response dataflow.

`Buffer.from` receives `encoded_data_conversion_call` only with exactly two
nonspread constant primitive strings and an ASCII case-insensitive encoding
of `base64`, `base64url` or `hex`. Otherwise the general call record keeps
`overload_unresolved`, `encoding_unresolved` or `outside_profile`. The payload
is never decoded and does not establish executable content.

Rules retain call/callee/receiver, request/import and argument node identities,
original byte ranges, optional syntax, argument count and spread presence.
Argument anchors are ordinals, not overload-resolved parameter roles. For
example, `execFile(file, options)` does not imply that the second argument is
an argument vector; `http.request(options)` does not imply a URL value.
At most 16 argument anchors are published with an explicit truncation flag.
Commands, paths, URLs, payloads, arbitrary names and per-value hashes are not
included in metadata output. API and encoding classes are fixed vocabulary.

Primary contracts: [child_process](https://nodejs.org/download/release/v22.15.0/docs/api/child_process.html),
[fs](https://nodejs.org/download/release/v22.15.0/docs/api/fs.html),
[builtin ESM modules](https://nodejs.org/download/release/v22.15.0/docs/api/esm.html#built-in-modules),
[HTTP](https://nodejs.org/download/release/v22.15.0/docs/api/http.html),
[HTTPS](https://nodejs.org/download/release/v22.15.0/docs/api/https.html),
[process.dlopen](https://nodejs.org/download/release/v22.15.0/docs/api/process.html#processdlopenmodule-filename-flags),
and [Buffer.from](https://nodejs.org/download/release/v22.15.0/docs/api/buffer.html#static-method-bufferfromstring-encoding).

## Package connection and ownership

Package analysis takes one existing package-analysis ID and 1–16 explicit
source IDs. Every selected source retains a row, including zero-match and
unavailable results. A direct artifact must match an inventoried file's exact
ID, hash and size. Unrecorded installed-package ownership stays null; no root
or longest-prefix fallback is invented. Exact direct declared-entry matches
retain up to 16 entry IDs, the full count and truncation status.

For Bun/SEA/nexe sources, the session builds ancestry only from its retained typed
extractor results. It can follow at most 16 edges to an inventoried file,
including a native member of a captured archive. The resulting
`embedded_source` relation retains the **delivery package** and origin evidence,
but keeps source ownership null and does not inherit the container's declared
entry matches. Caller-supplied origin JSON cannot create a relation. Duplicate,
cyclic, excessive and foreign ancestry refuses the whole aggregation.

`selection_complete:false`, `runtime_entry_verified:false` and
`install_entry_propagation:not_analyzed` remain explicit. Manifest lifecycle
commands are declarations owned by package metadata; this source inspection
does not parse shell commands or prove which files run during installation.
Advisory/provenance imports, reachability, analyst dispositions and human reports
remain separate #715 work.

## Ownership, limits and surfaces

`SourceModel::indexSourceDeclarations` owns the unique declaration projection
used by origins, paths, navigation and bundles. A class name may legitimately
have distinct outer and inner bindings on one AST node. That node's unique
projection stays unknown, without invalidating unrelated declarations; the
full binding declaration model remains intact and order independent.

`packages/behavior/Rules` owns the table; `Source` traverses shared models;
`Package` owns inventory joins; `Publication` emits fixed metadata.
`SessionNodeBehavior` and `SessionPackageBehavior` own explicit selections,
caches and revision invalidation. CLI, worker and MCP call the same C API.

Per source: existing 1 MiB/100,000-node parser admission, at most 4,096 records,
16 argument anchors per record and 2,000,000 additional inspection steps.
Each upstream semantic owner also enforces its own limits. Per aggregate:
16 sources, 8 MiB selected text, 1,000,000 selected nodes, 8,192 records and
8,000,000 inventory/join/copy steps. Existing session source/node budgets also
apply. Four source results and two aggregates are cached; aggregate analysis
uses temporary source results when necessary, so its 16-source selection is
not constrained by the four-result cache. Pages admit at most 512 rows.
Overflow discards the incomplete result. Reimport revokes all dependent caches.

The additive C API is `neverd_web_node_behavior_analyze_json`,
`neverd_web_node_behavior_records_json`,
`neverd_web_package_behavior_analyze_json` and
`neverd_web_package_behavior_records_json`. Worker/MCP use the corresponding
`web_` operations; package selection supplies `package_analysis_id` and
`source_ids`, and package pages select `sources` or `records`.
Parser/backend omission keeps the ABI with explicit `capability_unavailable`
and removes these operations from capabilities and MCP discovery.

```console
neverd web node-behavior ./captured-root 1:commonjs
neverd web bun-node-behavior ./standalone 0 1897:module
neverd web package-behavior ./captured-root 17 package-json 1:commonjs 2:module
neverd web archive-package-behavior ./package.tgz tgz 0 3 package-json 0:commonjs 1:commonjs
```

Indices select previously inventoried artifacts/members, not paths guessed
from a target string. Archive source and manifest indices share the selected
archive namespace. Each source's parse goal is explicit. The Bun command's
two indices select the captured native artifact and its module.
