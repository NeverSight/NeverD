# Claude Code 2.1.296: offline NeverD qualification

## VSIX container and embedded CLI equality — 2026-10-11

The official [Marketplace extension](https://marketplace.visualstudio.com/items?itemName=anthropic.claude-code)
version `2.1.296`, target `linux-x64`, was downloaded through its
[versioned package endpoint](https://marketplace.visualstudio.com/_apis/public/gallery/publishers/anthropic/vsextensions/claude-code/2.1.296/vspackage?targetPlatform=linux-x64).
The HTTP response used `Content-Encoding: gzip`. The initial wire body was
preserved and correctly refused as ZIP; downloading with HTTP content decoding
produced the actual VSIX body. NeverD did not infer or unwrap HTTP encoding.

| Evidence | Bytes | SHA-256 |
|---|---:|---|
| HTTP gzip wire body | 118,756,941 | `e7039fcac6d32b4632f9eda4a24a56bc813cd4a48fde23d2c594561f4deb0995` |
| Decoded VSIX ZIP body | 118,957,454 | `31176c4a2a29144673170dfb9affc2589af997d01532f1f7f97f8b93999a3d11` |
| Embedded native helper | 257,068,216 | `24972e3bc859fab2b46ed4c1e51f7d6130f06d3bd550811a114640de3370d0de` |

The C++ ZIP reader validated 29 available regular members totaling 268,754,709
decoded bytes. The captured `extension/package.json` declares version `2.1.296`
and produces package entry evidence. Member index 15 matches the previously
qualified standalone CLI byte-for-byte and yields 2,589 Bun modules and 10,005
regions. Extension and helper identities remain separate; equality here comes
from bytes, not matching version labels. This verifies container extraction and
downstream analysis, not extension activation or publisher authentication.

The real C++ test is enabled only by
`NEVERD_CLAUDE_CODE_21296_VSIX_LINUX_X64`. The CLI also completed:

```console
PATH=/neverd-no-external-tools /absolute/path/to/neverd web archive-bun /path/to/decoded.vsix zip 0 15
```

No VS Code, extension, helper, installer or external extraction program ran.
Framework manifest analysis is a separate phase from the
[ZIP container profile](web-zip-profile.md).

The subsequent native C++ VSIX manifest profile selected member index 4,
the 24,030-byte manifest with SHA-256
`e3f2ffc5e475f38f29db1568112b7c3a312b6f0fe53fca387d32a004b136101c`.
Its `main` declaration links to available member artifact
`b93f9a5471ca812b01e7f03cd7a49fed889712966b77ce8279e4979f6e7f5ff3`
within the same ZIP; `browser` is absent. Required field shapes are present.
Compatibility, permissions, activation and helper-version association remain
explicitly unanalyzed. No manifest strings or contribution/event names appear
in ordinary output. The original compressed member frame and expanded-byte
coordinates remain attached to the result.

The C++ SDK case tries all 29 available members through the bounded profile and
finds exactly one admitted manifest. The actual CLI also passed with an
unusable external-tool PATH:

```console
PATH=/neverd-no-external-tools /absolute/path/to/neverd web archive-desktop /path/to/decoded.vsix zip 0 4 vsix
```

Indices 4 and 15 are observations for this exact pinned archive, not defaults
for other releases. Logs are `/tmp/neverd-desktop-fixed-tests.log` and
`/tmp/neverd-desktop-real-cli.log`; see the
[desktop manifest profile](web-desktop-manifest-profile.md) for the narrower
file-candidate contract.

## npm originals and standalone equality — 2026-10-11

NeverD's C++ tar/gzip reader admitted the official
[wrapper npm artifact](https://registry.npmjs.org/@anthropic-ai/claude-code/-/claude-code-2.1.296.tgz)
and [Linux x64 npm artifact](https://registry.npmjs.org/@anthropic-ai/claude-code-linux-x64/-/claude-code-linux-x64-2.1.296.tgz).
Both original compressed files matched SHA-512 declarations in their separately
captured registry JSON responses through the C++ SRI verifier and CLI. No npm,
tar, gunzip, Node, Bun runtime or target script was invoked.

| Evidence | Bytes | SHA-256 |
|---|---:|---|
| Wrapper original tgz | 29,001 | `f6c375d51d4c22a7a850e185a0d7ddda85173f89dbbeb3262aa7971e3bd670b5` |
| Wrapper registry JSON | 3,531 | `0fb79dc0c21d02fc14feeb7f9a86fa15ca3937598f9f3a6a03816bb15e7fa6e4` |
| Linux x64 original tgz | 114,864,541 | `eef5a2e2b09a5e7d2ce784d1b3ba335ae7fc6c7f6256c802a0ebf2c21e4fd0e1` |
| Linux x64 registry JSON | 2,560 | `2b5c7ee2ad1174d6e3abdb6bdce80684c3cec54d3ef3edad5fe2bf9c4c161db0` |
| Native member, equal to standalone original | 257,068,216 | `24972e3bc859fab2b46ed4c1e51f7d6130f06d3bd550811a114640de3370d0de` |

The wrapper expands to 194,560 tar bytes and seven regular members. Its actual
captured `package.json` produces one package, eight optional dependency
declarations, two scripts and one bin entry linked to a captured member. The
Linux x64 package expands to 257,073,152 tar bytes and four regular members.
The native member again produces 2,589 Bun modules. Its exact byte equality
connects this npm distribution to the full standalone recovery documented below.
It does not establish equality of any other platform's distribution.

The pinned registry SRI strings are:

```text
wrapper: sha512-OX/k/rpcthMqnFKOWcpNcbiLsMKtAjLOpQNJHlYiDCFUznWCIwdRTyl/p9SdHVrsrzZuPMCNqZjnpakIni6upw==
linux-x64: sha512-+m01urgelnI+VJxPaKPEK0wixgq42uZ8PeCaUk5tX/8YoDBvSmC/REVQG2jQfJAr8dxunpjJ600FvLrcj1WWTw==
```

The verifier hashes the original tgz, not the expanded stream or binary member.
Registry/declaration association is explicitly selected evidence; matching SRI
does not authenticate publisher identity or establish benignness. C++ optional
tests and repeatable commands are in the
[archive/integrity profile](web-package-archive-profile.md).

## Standalone source recovery

Updated on 2026-10-11 on macOS arm64, Release, using prebuilt NeverD LLVM
23.0.0 r4. This is a real artifact extraction and source-recovery qualification,
not a claim to reconstruct the publisher's original repository.

## Input and provenance

The official download channel and npm metadata both selected **2.1.296**.
The input was downloaded from the official
[Linux x64 release URL](https://downloads.claude.ai/claude-code-releases/2.1.296/linux-x64/claude),
using the coordinates from the
[installer](https://claude.ai/install.sh) and
[release manifest](https://downloads.claude.ai/claude-code-releases/2.1.296/manifest.json).
Neither the installer nor the target executable was run. Downloading the
artifact is separate from NeverD's offline analysis.

| Evidence | Value |
|---|---|
| Release | `2.1.296`, `linux-x64` |
| Manifest commit | `fdb6c17a97b6e0e3cf15e3566663fca58e7c43a9` |
| Manifest build date | `2026-10-09T14:46:35Z` |
| Original size | `257068216` bytes |
| Original SHA-256 | `24972e3bc859fab2b46ed4c1e51f7d6130f06d3bd550811a114640de3370d0de` |
| NeverD layout | `bun-71d0d439-prelinked-linux-x64-elf-v1` |
| Graph offset / length | `89120776` / `167890143` bytes |
| Graph flags | `7167` (`0x1bff`) |
| Entry / startup count | `6` / `7` |

NeverD independently computed the original SHA-256 and matched the official
manifest. Publisher signatures were not verified. The input, recovered target
code and old comparison source remain outside the NeverD repository; tests
do not download or redistribute them.

## Practical recovery

```console
PATH=/neverd-no-external-tools /absolute/path/to/neverd web bun-export /path/to/claude-linux-x64 /path/to/new-output-directory
PATH=/neverd-no-external-tools /absolute/path/to/neverd web bun-navigate /path/to/claude-linux-x64 6 module
PATH=/neverd-no-external-tools /absolute/path/to/neverd web bun-navigate /path/to/claude-linux-x64 228 module
```

These operations use NeverD's C++ C API/backend in process. There is no Bun,
Node, npm, Python, external extractor or formatter on the analysis path. The
target's `require`, `eval`, imports, configuration and executable bytes remain
data. Source recovery also ran successfully with an unusable external-tool PATH.

| Result | Count |
|---|---:|
| Declared modules | 2,589 |
| Exact exported regions | 10,005 |
| Strictly decoded JS modules | 2,345 |
| Decoded UTF-8 JS bytes | 44,768,763 |
| Unavailable source projections | 0 |
| Reparse-verified readable modules | 2,345 |
| Modules with parser/readability rejection | 0 |
| Asset modules | 244 |
| Module JSC cache regions | 2,343 |
| Source-map regions | 0 |
| Output files | 14,698 |
| Verified output bytes | 615,294,413 |

The complete original, every declared region and all 2,345 decoded sources are
present. `index.html` maps generated filenames to captured virtual names and
identifies module 6 as the entry. Every exported file is reread and hashed;
`manifest.json` also carries original offsets, storage/source/readable hashes,
projection and parser status, and bounded diagnostic offsets.

The complete run produced manifest SHA-256:
`8b9dc8bbdb20a533c39dc708497ac9f500ea48a3d1921ab32303f30c2b9e25e3`.
It took 170.44 seconds wall time, 55.60 seconds user CPU and
8.29 seconds system CPU, with 1,796,014,080 bytes maximum RSS as reported by
macOS `time -l`. Other builds/tests shared the host; this is an observation,
not a controlled throughput benchmark or resource guarantee.

The previous run's 42 resource-declaration rejections are all resolved by the
private C++ [resource-management extension](web-resource-management-profile.md).
The 4,762,637-byte module then exposed the old one-million-node recovery limit:
it needs 1,165,398 retained nodes. The explicit recovery profile now allows
two million nodes; interactive limits are unchanged. Every source in this
artifact now has empty parse diagnostics and a reparse-verified readable copy.
No guessed source or substituted empty module is emitted.

No source maps are shipped in this artifact. Original TypeScript, erased types,
deleted comments/names and pre-bundle file boundaries cannot be recovered from
these bytes with the same fidelity as the older source-map-based reference.
Native machine code and JSC caches are preserved, not decompiled. Readable
copies retain original bytes and insert whitespace, then compare retained
syntax trees; source-text reflection and positions change.

## Failures that drove implementation

1. The old reader refused flags 11/12. The new independently versioned layout
   validates prelinked graph tables and runtime-option storage against the
   pinned upstream writer, retaining linked-bytecode flag 13 as unsupported.
2. One 4,762,637-byte JS module exceeded the old storage materialization cap.
   The shared C++ Unicode decoder now reads in bounded chunks; explicit export
   uses a separate limit without raising interactive parser limits.
3. Nine sources exposed missing parser locations for async-arrow rest
   parameters. The pinned C++ parser conversion now preserves original ranges.
   A regression also exposed an illegal rest trailing comma being accepted;
   parser admission checks the parser-owned following token. Valid spread-call
   commas and comments remain admitted.
4. Whole-module readability needed a separate sequential parser budget and
   reparse comparison. Unsupported syntax remains explicit, with raw evidence
   and diagnostics retained.
5. Resource declarations needed contextual grammar, async/source-type checks,
   immutable lexical bindings and conservative disposal effects. Declaration
   kinds survive the retained AST and readable reparse; they are never lowered
   into ordinary variable declarations. Lexer lookahead now restores newline
   state, and node-count/depth budget diagnostics are distinct.

Module 228 additionally passed NeverD's existing binding/navigation consumer:
55,522 syntax nodes, 1,169 functions, 4,468 calls and 10,980 references, with
`binding_status:ok` and `navigation_status:ok`. These are source relationships,
not proven runtime call targets. Old-source phrase matches supply navigation
hints only; no old source was substituted into recovered files.

## Repeatable checks

```console
cmake --build build-release --target neverd NeverDWebArtifactTests NeverDWebSourceTests NeverDWebSDKTests
NEVERD_CLAUDE_CODE_21296_ELF=/path/to/claude-linux-x64 build-release/bin/NeverDWebArtifactTests --gtest_filter=WebBun.ClaudeCode21296FullContainerWhenSupplied
NEVERD_CLAUDE_CODE_21296_ELF=/path/to/claude-linux-x64 build-release/bin/NeverDWebSourceTests --gtest_filter=WebSourceRecovery.ClaudeCode21296AllJavaScriptWhenSupplied
build-release/bin/NeverDWebSourceTests --gtest_filter='WebSource.AsyncArrowRest*:WebSourceRecovery.*'
build-release/bin/NeverDWebSDKTests --gtest_filter='WebSDK.BunExport*:WebSDK.CLIBunExport*'
```

The full web suite registered **291 cases: 284 passed, seven skipped, zero
failed**. This revision adds seven cases, including an optional full-artifact
recovery check that reparses all
2,345 modules and verifies every original byte survives in order. That batch
has a 600-second CTest deadline; ordinary unit tests and per-parser cooperative
budgets are unchanged. Both the five pinned Bun 1.4.2 full images and this Claude
artifact were supplied. Six source-map cases require unavailable LLVM Zstd and
one availability case only applies when ASAR is disabled. The full-artifact
case passed in 145.59 seconds. Local logs are
`/tmp/neverd-erm-web-qualified.log` and `/tmp/neverd-erm-worker-focused.log`.
The native/source/backend boundaries retain their independent budgets and
qualification limits. This case advances #714 and #718; it does not close the
five JS epics or qualify other container platforms.

The worker SourceCache timeout was traced to whole-document regex scanning of
long comments. C++ code now finds asm linkage through the existing comment and
literal-aware token stream; image-marker checks use an anchored scan. A C++
9,000-comment-row regression preserves real linkage, rejects fake linkage in
comments and checks row anchors. The existing SourceCache transport suite
passed in 7.15 seconds under parallel testing, without raising its deadlines.
The macOS snapshot fixture now receives a canonical temporary directory from
CMake, matching the worker's canonical path contract.

Broader worker testing also exposed a libc++ fixture signature mismatch
(`NeverDWorkerLibraryFeatures`, zero expected whole-function matches), which
persists with the signature cache disabled. `NeverDWorkerNativeMapping` failed
once in parallel and passed on serial retry. These native-analysis checks are
outside the JS recovery path; full worker/repository success is not claimed.

The implementation and full web qualification are at `cb0a3d5ad`. Integration
with upstream `dev` (`5f0fb5ebb`) produced `58bdfa7e7`; the web implementation,
headers, parser integration, worker and web tests are unchanged by that merge.
The focused source/mock-worker build reports no work needed. This qualification
does not rerun a full native-backend build for the independently merged x86 EH
changes. The recovered output remains the qualified C++ export recorded above.
