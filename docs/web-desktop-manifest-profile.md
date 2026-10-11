# Captured NW.js and VS Code extension manifests

`nwjs-manifest-file-candidates-v1` and
`vscode-extension-manifest-file-candidates-v1` read an explicitly selected
captured manifest using in-process C++. They complement the
[ZIP container](web-zip-profile.md), [Electron](web-electron-profile.md) and
[package](web-package-profile.md) profiles; each retains its own semantics.
Selecting a profile does not authenticate a framework, publisher or version.

## Declarations and entries

NW.js records the shapes of `name`, `version` and `nodejs`, and compares `main`,
`node-main`, `bg-script`, `inject_js_start` and `inject_js_end`. Plain captured
roots, `package.nw` / `app.nw` directories and
`Contents/Resources/app.nw/package.json` have named layout candidates. A ZIP
`package.nw` uses the same manifest root and preserves its distinct ZIP origin.
Executable-appended ZIPs are outside the container profile.

VS Code extension analysis records the shapes of `name`, `publisher`, `version`,
`engines.vscode`, `extensionKind`, `activationEvents` and `contributes`, and
compares `main` and `browser`. A root `extension/package.json` gives an extension
path candidate; a root `package.json` gives a captured extension-root candidate.
Other placement remains unrecognized. A contribution-only extension may have no
entry declarations. Object-valued npm `browser` mappings are not VSIX entry
strings and remain unsupported entry types.

All matches are exact available files relative to the selected manifest's
parent directory. There is no extension/index/package fallback. URLs, scheme or
authority syntax, percent encoding, query/fragment syntax, whitespace and
nonportable paths do not become filename candidates. `node-main` arguments or
quotes receive a separate command-line-syntax status; nothing strips arguments
and guesses a script. These are deliberately narrower profiles than runtime
resolution. Missing, empty, invalid-type, directory and unmatched declarations
remain distinct.

No target code, runtime, extension, installer or native addon is launched.
Runtime activation, version compatibility, permissions and helper-version
association remain explicitly unanalyzed. `nodejs=false` is a declaration, not
an execution or permission result. Application/extension version shapes never
establish the version of an embedded native helper. Helper hashes and container
origins can independently connect to the [Bun](web-bun-profile.md) or
[SEA](web-sea-profile.md) profiles.

## Identity, selection and publication

Manifest identity binds the captured namespace, artifact, original hash and
explicit profile. Entry identities also bind the fixed manifest field. Different
containers and NW.js/VSIX interpretations cannot reuse one result. An available
ZIP/ASAR member uses only its container namespace. An individual Bun/SEA asset
has one document and no directory namespace; private asset keys never manufacture
paths or neighboring files.

`desktop/Manifest` owns bounded JSON/hash/name admission and exact path
comparison. `desktop/NWManifest` and `desktop/VSIXManifest` own their field rules.
`SessionDesktop` owns publication and sixteen revision-bound cached results;
reimport revokes them. The original bytes and nested storage origin remain
available through the existing artifact model. Candidate `entries[].artifact_id`
values select the same source, HTML, package and native consumers, with their
existing budgets. `entries[].entry_id` identifies declaration evidence and is
not a consumer selection ID.

One manifest is at most 1 MiB, depth 32 and 20,000 JSON nodes. Namespace admission
is at most 10,001 root/member records with 8 MiB path metadata; one path is at
most 4,096 bytes. Replies contain a fixed set of declaration statuses and at
most five entries. They disclose no raw target string, path, version, dynamic
contribution key or activation value. Fixed field names and kinds are NeverD's
vocabulary. No raw export or reviewed-display permission is implied.

## Interfaces and qualification

The C API is `neverd_web_desktop_manifest_analyze_json`, with input kind `nwjs`
or `vsix`. Both the compiled worker and MCP expose
`web_desktop_manifest_analyze` using the same client and backend. Capabilities
advertise both complete profile names, bounds and unverified runtime claims.
The API remains available without the JavaScript parser; backend omission
retains its ABI symbol with explicit unavailability.

```console
neverd web desktop-manifest CAPTURED_DIRECTORY nwjs MANIFEST_ARTIFACT_INDEX
neverd web desktop-manifest package.json vsix
neverd web archive-desktop app.nw zip 0 MANIFEST_MEMBER_INDEX nwjs
neverd web archive-desktop extension.vsix zip 0 MANIFEST_MEMBER_INDEX vsix
```

`WebDesktopManifest.*` covers framework rules, scoped candidates, URL/command-line
boundaries, missing/invalid entries, malformed JSON, hashes, names and budgets.
`WebDesktopSDK.*` covers captured layouts, directory/ZIP consumers, individual
assets, cache/revision lifetime, canaries and actual CLI processes. The optional
pinned real VSIX test requires `NEVERD_CLAUDE_CODE_21296_VSIX_LINUX_X64`; it never
downloads or executes the sample. Executed checks and host limitations are in
the [implementation ledger](web-analysis-implementation.md).

Field semantics follow the primary [NW.js manifest reference](https://github.com/nwjs/nw.js/blob/main/docs/References/Manifest%20Format.md)
and [VS Code extension manifest reference](https://code.visualstudio.com/api/references/extension-manifest).
No upstream implementation was copied. These profiles do not establish full
distribution detection, VSIX XML/signature validation, activation or safe export.
