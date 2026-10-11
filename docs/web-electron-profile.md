# Electron manifest and source evidence

These are in-process C++ evidence consumers for explicitly selected captured
artifacts and parsed source units. They complement the [ASAR reader](web-asar-profile.md);
they do not authenticate an Electron distribution, infer its runtime version,
execute application code or establish a complete application graph.

## Manifest entry candidates

`electron-40-package-entry-candidates-v1` reads bounded JSON from an original
file or an available ASAR member. Its entry-selection reference is Electron
[v40.0.0 browser initialization](https://github.com/electron/electron/blob/v40.0.0/lib/browser/init.ts).
Absent, null, false, zero or empty-string `main` selects the documented
`index.js` fallback. A nonempty string is a declaration; other main value types
remain explicitly unsupported. `.mjs`, `.cjs` and the `type` field provide a
source-type candidate under that reference rule, never a verified runtime mode.

The selected manifest hash must match its captured occurrence. A main path is
compared only with exact available members under the manifest's directory in
the same captured namespace. ASAR members use the extraction's virtual
namespace, including explicitly associated unpacked files. A single original
file provides no directory namespace. An exact match reports its artifact ID;
directories, missing exact files and unsupported paths have distinct statuses.
There is no extension/index/package lookup, host fallback or remote retrieval.
Parent components and absolute/device/unsafe paths never authorize another read.

Name, version and product-name fields are recorded by presence only. A declared
Electron dependency is metadata, not evidence that a particular runtime is
installed. Values, dependency ranges and main filenames remain private. Manifest
selection explicitly reports `framework_evidence: caller_selected_profile`.

## Source-visible boundaries

`electron-static-boundaries-v1` consumes the shared syntax, binding, module and
finite primitive-value owners. `javascript-module-origin-candidates-v1` owns
syntactic provenance from ESM imports and literal require candidates through
unwritten variable initializers, object destructuring and static member access.
It retains module-request identities and distinguishes constructed instances
from their constructor. Aliasing never establishes initialization, reachability,
object contents, getter behavior or a runtime function value.

Shadowed require, reassigned bindings, unbound require writes and dynamic `with`
references cannot provide those links. A possible direct eval refuses the origin
analysis. Alias cycles, default/rest/positional patterns, computed dynamic members,
call results and interprocedural propagation do not invent origins. Source names
alone cannot identify an Electron API. Budgets fail atomically; origin/value
status remains visible in the Electron summary.

The Electron consumer matches explicit `electron`, `electron/main` or
`electron/renderer` module syntax, including renamed imports and require
destructuring. Its fixed categories cover:

- `BrowserWindow` construction, uniquely declared preload options, and
  constructed-window `loadFile` / `loadURL` calls;
- constructed-window `webContents.send` / `postMessage` candidates;
- `ipcMain` handle/listen/removal and `ipcRenderer` invoke/send/listen/removal
  candidates;
- `contextBridge.exposeInMainWorld` and `exposeInIsolatedWorld` candidates.

API shape references are the pinned Electron v40.0.0
[BrowserWindow](https://github.com/electron/electron/blob/v40.0.0/docs/api/browser-window.md),
[ipcMain](https://github.com/electron/electron/blob/v40.0.0/docs/api/ipc-main.md),
[ipcRenderer](https://github.com/electron/electron/blob/v40.0.0/docs/api/ipc-renderer.md)
and [contextBridge](https://github.com/electron/electron/blob/v40.0.0/docs/api/context-bridge.md)
documentation. The fixtures are independently authored inert C++ test strings;
no Electron runtime is a build, test or analysis dependency.

Each boundary retains the original source node/range, module request index and
analysis ID, construction node, value node and callback/API node. These can be
joined to the existing module pages, source navigation, source-storage anchors
and reviewed source views. Channels, URLs, paths and exposed API keys are not
ordinary output. Proven finite string expressions stay private; dynamic values
remain explicit. Getters, spreads, duplicate keys or unknown computed keys in
window options prevent selecting a unique preload value. The construction
record retains the options node even when its properties cannot be resolved.

All successful source summaries report `analysis_status: partial` and
`runtime_targets_verified: false`. A zero boundary count means no match under
this profile, not absence of IPC. Property mutation, wrappers and runtime
module replacement remain possible. No permission, security-default,
exploitability, callback execution or runtime message-flow verdict is made.

## Interfaces and budgets

The C API adds `neverd_web_electron_manifest_analyze_json`,
`neverd_web_electron_source_analyze_json` and
`neverd_web_electron_source_records_json`. Worker operations use the same names
without `neverd_` / `_json`. Unknown fields are refused. Results use exact web
revisions, owned C strings and the shared `metadata-only-v1` output policy.
Import replacement clears both caches. Manifest analysis remains available
without the JavaScript parser; source operations return `capability_unavailable`.

```console
neverd web electron-manifest ./captured-app 2
neverd web electron-manifest ./package.json
neverd web electron-source ./main.js commonjs
neverd web asar-manifest ./app.asar 0 - 1
neverd web asar-electron ./app.asar 0 - 0 commonjs
neverd web bun-electron ./standalone 0 module
neverd web electron-ipc ./captured-app 2 1:commonjs 3:commonjs
neverd web asar-ipc ./app.asar 0 - 1 0:commonjs 2:commonjs
neverd web electron-entries ./captured-app 2 1:commonjs 3:commonjs
neverd web asar-entries ./app.asar 0 - 1 0:commonjs 2:commonjs
```

Indices are illustrative. Select the actual captured artifact/member or Bun
module position; commands reuse the same source and container owners in one
process. `-` leaves ASAR unpacked resources unassociated. No temporary JavaScript
file, external executable or target launch occurs.

| Resource | Ceiling |
|---|---:|
| Manifest bytes / JSON depth / JSON nodes / one string | 1 MiB / 32 / 20,000 / 4 KiB |
| Namespace entries including a virtual ASAR root | 10,001 |
| Cached manifests | 16 |
| Origin steps / cumulative allocated UTF-16 units | 2,000,000 / 1,048,576 |
| Origin traversal / destructuring depth / member chain | 64 / 32 / 32 |
| Electron steps / boundary records / retained string units | 2,000,000 / 10,000 / 1,048,576 |
| Boundary page | 1–512 |
| IPC selected sources / cached scopes | 16 / 4 |
| IPC cumulative selected boundary records / private channel UTF-16 units | 10,000 / 1,048,576 |
| IPC work including examined comparison units | 2,000,000 |
| Entry selected sources / cached scopes / aggregate selected boundaries | 16 / 4 / 10,000 |
| Entry association work, including path work | 4,000,000 |
| Per-source path work / traversal depth / requested expressions | 2,000,000 / 64 / 10,000 |
| One private path / cumulative allocated path text | 4,096 / 1,048,576 UTF-16 units per source |

One source result shares each admitted source's existing cache/revision lifetime.
Original capture, parser, binding, module and primitive-value budgets also apply.
These are per-analysis limits, not a hard whole-process memory or time guarantee.

## Explicit source selection and channel correlation

`electron-scoped-channel-candidates-v1` accepts an already analyzed manifest and
1–16 explicitly selected source/evidence pairs. Every source must be an exact
captured member under the manifest's directory in the same occurrence namespace.
The exact-file main candidate is required with its manifest source-type candidate.
An original unpacked file and its ASAR occurrence remain different identities,
even when their bytes match. Source-map-derived or other-container sources do
not acquire membership from matching content or names. A standalone file cannot
invent a directory namespace. The analyzer does not discover or import sources.

Private constant channels use exact UTF-16 equality. Embedded NULs, lone
surrogates, case and Unicode normalization differences remain distinct as in
JavaScript strings. Channels receive identities from the whole selected scope
and first occurrence ordinal, not a separately reusable hash of their value.
Changing the manifest, namespace or selected evidence changes those identities;
reordering the same selection does not. Source order is canonicalized by ID.

The result groups source-visible IPC candidates and reports compatible pair
counts for renderer invoke/main handle, renderer send/main listen and
webContents send/renderer listen. It does not construct every possible pair,
so many equal-channel records cannot allocate a quadratic edge list. Removal
calls are retained without assuming their execution order or subtracting
handlers. Dynamic, absent and non-string arguments retain unresolved endpoints.
Sources whose origin analysis refused remain visible with their status and no
invented endpoints. ContextBridge keys are not IPC channel names and are not
merged into these groups.

The C ABI operations `neverd_web_electron_ipc_analyze_json` and
`neverd_web_electron_ipc_records_json` share the worker's
`web_electron_ipc_analyze` / `web_electron_ipc_records`. Analysis options are
`{"schema_version":1,"source_ids":[...]}` plus the separate manifest artifact
ID. Unknown options and decoded duplicate JSON keys refuse. Manifest/source
analyses must exist first. Queries page `sources`, `channels` or `endpoints`;
endpoints retain the original source, artifact, boundary and node/range IDs for
navigation/storage anchors. Four immutable selections are cached per revision;
an identical selection remains queryable at the limit. Import replacement
revokes every scope. Failure publishes no partial correlation result.

All results remain partial: `scope_evidence` is
`caller_selected_manifest_and_sources`, `selection_complete` is false and
`runtime_routing_verified` / `process_roles_verified` are false. Equal strings
do not prove that the calls run, their APIs are unmodified, both ends inhabit
compatible processes, an active handler exists or messages reach it. Entry
association below is a separate consumer; it does not grant IPC routing proof.

## Captured preload and renderer entry candidates

`electron-captured-entry-candidates-v1` reuses the manifest/source admission
owner used by IPC. It requires cached manifest and source evidence, but no IPC
analysis. It compares only exact available members of that captured namespace.
There is no basename search, extension fallback, filesystem lookup, automatic
source/HTML analysis or import closure. ASAR members retain their occurrence
IDs; a separately captured file with identical bytes cannot replace them.
Unavailable ASAR members cannot supply candidate targets.

`javascript-captured-portable-path-candidates-v1` owns private path expressions.
The admitted roots are syntactic candidates: unwritten CommonJS wrapper
`__dirname`/`__filename`, `app.getAppPath()`, and module `import.meta.url`,
`.dirname` and `.filename`. Unwritten simple initializer aliases, string `+`,
templates and primitive constant conditionals preserve those roots. Native
`path`/`node:path` join, resolve, normalize and dirname calls use finite rules.
Module-origin evidence also admits `url`/`node:url` fileURLToPath and URL
constructors; an unbound, unwritten global URL constructor is a candidate.
URL objects and URL strings remain distinct; href and toString expose captured
URL-string candidates. Relative file URL references preserve directory bases;
only captured file URL strings can link loadURL.

The reference API contracts are Node.js v24.0.0
[path](https://github.com/nodejs/node/blob/v24.0.0/doc/api/path.md),
[CommonJS](https://github.com/nodejs/node/blob/v24.0.0/doc/api/modules.md),
[ES modules](https://github.com/nodejs/node/blob/v24.0.0/doc/api/esm.md) and
[URL](https://github.com/nodejs/node/blob/v24.0.0/doc/api/url.md), together with
Electron v40.0.0 [webContents](https://github.com/electron/electron/blob/v40.0.0/lib/browser/api/web-contents.ts)
and [web preferences](https://github.com/electron/electron/blob/v40.0.0/docs/api/structures/web-preferences.md).
These are independent C++ rules and inert fixtures, not copied runtime code or
evidence that the inspected application uses those releases.

The path domain is a virtual captured-root namespace with a portable component
subset, independent of the analyzer host. LoadFile's relative strings use the
manifest application directory; dirname-based expressions use their source
member directory. Preload requires a captured absolute-base candidate already
in canonical form. Raw concatenation does not normalize dot components;
explicit path operations do. Resolve needs an admitted root and can reset to
a later admitted root. Unknown arguments still refuse, even when an actual
runtime might ignore them to the left of its last absolute argument.

External absolute literals, unknown working directories, namespace-root
escapes, invalid UTF-16, NUL, backslashes, unsafe components and overlong paths
refuse. Explicit posix/win32 variants require target context and are currently
unsupported. URL percent encoding, query/fragment references, absolute URL
references and directory URL targets are outside this profile. Shadowed or
written roots, dynamic lookup, direct eval, cyclic aliases, arbitrary calls,
destructured path values and interprocedural results do not supply paths.
Property mutation, temporal initialization, symlinks at runtime and host
replacement of APIs are not proved absent. The namespace is a caller-selected
capture: actual runtime path bases and reachability remain unverified.

The C ABI operations `neverd_web_electron_entries_analyze_json` and
`neverd_web_electron_entry_records_json` correspond to worker operations
`web_electron_entries_analyze` and `web_electron_entry_records`. Analysis takes
the same options and manifest ID as IPC. Shared selection errors retain their
existing `electron_ipc_*` names. Page kinds are `sources` and `entries`, with
limits 1–512. Sources retain path-analysis ID/status/reason. Entries retain
boundary/node/range, path-root/operation and construction/value node links,
fixed refusal or exact-member status, optional target artifact ID and optional
explicitly selected target source ID. Paths and URLs never appear in output.

Analysis IDs bind the namespace, manifest, canonical source/evidence selection
and path-analysis identities, including requested expression nodes. Caches
hold four immutable selections per revision; replacement import revokes them.
Path budget failure clears that source's entire path result. Scope-wide budget
or admission failure publishes no entry cache. The association work ceiling
includes its path computations, repeated target normalization and examined
entry-index path comparison units; prerequisite module/origin/value analyses
retain their separate limits. These are logical counters, not process RSS or
wall-clock promises. All summaries remain partial, with
`runtime_entries_verified`, `runtime_path_bases_verified` and
`selection_complete` false and `html_analysis: not_analyzed`.

## Required remaining work

HTML script entries and source import closure, runtime channel routing,
wrappers and broader dataflow, distribution detection and runtime-version
evidence still require implementation and qualification. This
increment does not finish Electron, safe export, NW.js, VSIX, Tauri, Wails,
CEF/WebView or issue #716. Their requirements remain in the
[implementation plan](superpowers/plans/2026-10-10-javascript-analysis-design-zh.md)
and [ledger](web-analysis-implementation.md).
