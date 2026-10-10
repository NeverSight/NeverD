# Offline package evidence profile

`node-package-evidence-v1` inspects explicitly selected `package-json` or
`npm-lock` metadata in an immutable captured namespace. It is a partial
implementation of #715. No package manager, shell, target JavaScript or network
request runs. A lifecycle script remains data.

## Input and graph ownership

`lib/web/packages/PackageReader.cpp` owns bounded metadata admission, supplied
manifest association and installation-location candidates. `PackageDiff.cpp`
compares that model. `SessionPackages.cpp` owns revision-bound caches, pagination
and redaction; C API, CLI and worker adapters do not parse metadata or repeat
dependency rules. `include/neverd/web/Error.h` keeps the shared fixed diagnostic
contract independent of the session layer.

The caller selects a metadata occurrence and its input kind. A directory input
also supplies exact sibling/descendant evidence; a standalone JSON input has
no implicit directory inventory. Available ASAR members can be selected through
the same session API. Other package manifests within `node_modules` are
installation instances only at a complete package root, including scoped names;
example/test manifests nested inside an installed package are ordinary files.
An unrecorded installed package blocks file attribution to a known ancestor.

The npm adapters follow the [documented version contracts](https://docs.npmjs.com/cli/v11/configuring-npm/package-lock-json/):

| Input | Interpretation |
|---|---|
| npm v1 | Nested `dependencies` records describe installation placement. Only `requires` describes package dependency requests. Root direct dependencies require a supplied root manifest. |
| npm v2/v3 | `packages` is the instance table. Dependency kinds, optional overrides, peer metadata, aliases, platform conditions and link declarations retain separate evidence. |
| npm v2 legacy tree | Retained for comparable declaration conflicts; it never overrides the instance table. Fetch specifications in its version field are distinct from package versions. |
| npm hidden v3 lock | A selected `node_modules/.package-lock.json` uses the containing project's root for instance paths. A supplied project manifest can fill missing root evidence. |
| package.json | Root declarations and exact captured installed-package manifests, without registry lookup or installation. |

Missing versions stay missing unless a supplied manifest provides one. Name,
version and OS/CPU/libc fields record whether the selected evidence came from
the lockfile or manifest; conflicting supplied platform declarations remain
visible. A manifest's optional dependency overrides are compared with npm's
normalized production declarations without treating the dropped duplicate as
a conflict. Conditions accept a string or a string array, following
[npm's platform checks](https://github.com/npm/cli/blob/v11.9.0/node_modules/npm-install-checks/lib/index.js).

Nearest captured installation candidates do not prove semver satisfaction,
runtime resolution or selected-host compatibility. Namespace ancestry is not
npm's physical installation parent. A workspace target and an installed
package have different peer placement contexts. Links retain their target
location; missing targets, escapes and link chains have explicit states and
never become filesystem traversal.

Supplied manifests contribute lifecycle/script declarations and explicit
`main`, `module` and `bin` entries. Entry matching is confined and exact;
`exports`, `browser`, extension/index inference and conditional runtime loading
are outside this profile. Script names/commands, bin aliases, dependency names,
specifications, paths, origins and integrity text stay private.

## Comparisons and integrity

Two analyses in the same published revision can be explicitly selected as
before/after roots. Comparisons retain both analysis IDs and record IDs. They
compare location-aligned declarations, script bodies, bin names/entry paths and
exact supplied file hashes. Different names at one location are replacement
candidates; missing supplied files are not asserted to have been deleted.
Manifest coverage changes do not become empty-script comparisons. Different
lock versions retain an input-contract boundary instead of comparing legacy
fetch specifications with real package versions or inventing dependency changes.
Version and platform alignment remain analyst responsibilities.

SRI metadata is classified as missing, malformed, unsupported or
declared-unverified. A legacy Git commit is a separate unverified declaration.
No archive integrity, publisher signature, provenance, advisory freshness,
reachability, benignness or exploitability is established by this increment.
Tarball input, source behavior findings, external evidence imports, analyst
dispositions, readable reports and the C++ MCP surface remain pending.

## Surfaces and limits

```console
neverd web packages ./package-lock.json npm-lock
neverd web packages ./package.json package-json
neverd web inspect ./two-captured-roots
neverd web package-diff ./two-captured-roots npm-lock BEFORE_INDEX AFTER_INDEX
```

For directory input, append the selected metadata artifact index to `packages`.
Use indices from `inspect`; the examples do not assume filename ordering.
The four additive C functions are `neverd_web_packages_analyze_json`,
`neverd_web_package_records_json`, `neverd_web_packages_compare_json` and
`neverd_web_package_diff_records_json`. Worker operations use the corresponding
`web_` names. Pages contain fixed categories, hashes, relationships and opaque
IDs under `metadata-only-v1`; no individual private name is hashed into a public
record ID. Whole-artifact hashes remain part of the shared evidence contract.

Metadata admission is limited to 8 MiB aggregate, 4,096 instances, 32,768 edges,
32,768 script/entry/change records and two million analysis steps. Shared JSON
admission rejects duplicate decoded keys, depth above 64, invalid types and
over-budget strings/nodes. Sessions retain at most four package analyses and
four comparisons; page limits are 1–512. A new import revokes both caches.
An error cannot publish a partial graph or silently relax these limits.

## Qualification inputs

C++ fixtures cover all three npm lock versions, nested/hoisted instances,
aliases, workspaces, optional/peer rules, platform fields, hidden locks,
conflicting/missing manifests, metadata budgets, exact file attribution and
two-root comparisons. C API/CLI/worker checks cover stale revisions, bounds,
private-output canaries and an unusable external-tool PATH.

An optional real-input test accepts `NEVERD_NPM_CLI_1190_LOCK`, the unmodified
[npm CLI v11.9.0 lockfile](https://github.com/npm/cli/blob/v11.9.0/package-lock.json):
400,837 bytes, SHA-256
`b07e0fba031168ad2f8923c3b15d188f23b50799229f9ba456ed0abdfb427ab4`.
Its expected graph has 1,230 instances and 2,426 dependency declarations.
The data is supplied locally and never fetched by the test or analyzer.
