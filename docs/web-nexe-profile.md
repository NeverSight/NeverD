# Offline nexe resource extraction

`nexe-4.0.0-rc.7-eof-resources-v1` reads the fixed nexe resource envelope
entirely in C++. It extracts stored JavaScript, manifests and binary resources
into immutable occurrences that feed the existing source, package, HTML and
native consumers. It never starts Node, nexe, a target, a script or another
analysis program. Selecting this profile is explicit; it is not a generic
native CLI detector or proof of a producer/runtime version.

## Ownership and framing

`nexe/Reader` owns EOF framing and byte partition hashes. `nexe/Index` owns the
fixed first-shim grammar, strict bounded JSON index, path admission, unique
ranges and alias accounting. `archives/PathIndex` remains the authoritative
portable namespace policy. `SessionNexe` owns revision-bound publication,
selection and caches; `ArtifactView` composes original/expanded byte origins.
Transport adapters only select and serialize those same results.

The profile requires a 32-byte EOF footer: `<nexe~~sentinel>` followed by two
little-endian binary64 lengths for bootstrap and resources. Lengths must be
finite, nonnegative, integral and bounded before conversion/subtraction;
negative zero, empty bootstrap, empty prefix, truncation and trailing data
refuse. The producer's boot code chooses the **first** sentinel in the last
16,000 bytes. An earlier visible sentinel refuses even when an apparent footer
exists at EOF. A sentinel starting one byte before that window is not visible
to the upstream search and does not cause an invented ambiguity.

The UTF-8 bootstrap must start with the exact first wrapper around
`process.__nexe = {"resources": ...};`. The bounded JSON object has exactly one
`resources` object; each entry has `[offset,size]` relative to the resource
region. No expression evaluation or general JavaScript parsing participates in
container extraction. Duplicate decoded JSON keys, invalid Unicode, unknown
first-shim layouts, excessive structure and out-of-bounds ranges refuse.
Subsequent bootstrap text is preserved, not interpreted by this reader.

## Regions, names and evidence

Four regions partition every captured byte: `opaque_prefix`,
`bootstrap_storage`, `resource_storage`, and `footer`. The prefix is deliberately
unclassified: this reader does not validate PE/ELF/Mach-O headers or instructions.
`native_prefix_validation`, `signature_status` and `runtime_activation` remain
`not_checked`. Footer-appended signatures fail EOF framing; signatures already
inside the prefix are not inspected. Accepted input is not necessarily unsigned.

Resource paths require the fixed `./` prefix and the existing pinned ICU 77.1
portable slash policy. Only that prefix is removed for namespace selection.
Absolute, parent, device, drive, backslash, invalid UTF-8, case-fold/NFC aliases
and file/directory conflicts refuse. Windows backslash-key layouts require a
separate qualification; keys are never converted to host paths or written to
disk. Public pages contain IDs, hashes, offsets, sizes and fixed categories,
not private paths, source text or binary payloads.

Exact `(offset,size)` aliases are permitted, share a range ID/hash and retain
distinct member occurrence IDs. Partial overlaps refuse. Empty members cover
no bytes; unreferenced resource bytes remain in `resource_storage` with an
explicit byte count/coverage state. Records are sorted by private UTF-8 path,
independent of JSON object's iteration order. Partition hashes and the parent
hash are calculated together; each distinct member range is read for hashing
only once. Thousands of aliases cannot multiply large-member reads.
Public member IDs use the extraction, sorted ordinal and range identity; the
private path itself is not an additional public identity-hash input.

Members and the bootstrap region expose explicit `selection_id` values.
Source consumers require an explicit `script`, `module` or `commonjs` goal.
File suffixes do not authenticate content, source-map associations, bytecode
semantics or entry execution. V8 caches/snapshots remain opaque bytes.
No raw export is provided by this first profile.

Every extraction has its own private namespace, even when two containers have
identical bytes and member paths. A selected inner manifest can inventory only
that namespace. The package behavior consumer follows retained typed nexe
ancestry to an outer inventoried file as **delivery** evidence; it does not
inherit source ownership or declared entries from the container's package.
Nested stored ZIP offsets remain original-storage coordinates; deflate ZIP
selections retain expanded coordinates and the containing compressed frame.

## Bounds and surfaces

| Resource | Limit |
|---|---|
| Selected immutable input | 256 MiB |
| Bootstrap / index | 8 MiB / 1 MiB |
| Members / JSON preflight nodes / JSON depth | 4,096 / 16,391 / 4 |
| Private/path-index accounting | 4 MiB per extraction, 8 MiB retained per session |
| Reader structural steps | 2,000,000 |
| Bytes read for hashing | Input length plus unique nonoverlapping member lengths, at most twice input length |
| Bytes scanned for envelope/bootstrap | At most 16,000 plus bootstrap length |
| Cached extractions / page records | 4 / 512 |

The JSON node ceiling admits one extra member so the independent member-count
diagnostic remains testable. Shared path admission has its own 8 MiB ceiling;
the nexe owner checks its stricter 4 MiB ceiling before subtracting remaining
budget. Namespace validation completes before publication; failures and cache
limits preserve earlier results. Reimport revokes all derived selections.
Extraction needs the pinned native namespace policy but works with JavaScript
parsing disabled. Backend omission retains unavailable C ABI entry points.

C API: `neverd_web_nexe_extract_json` and `neverd_web_nexe_records_json`.
Worker/MCP: `web_nexe_extract` and `web_nexe_records`; record kinds are `regions`
and `members`. MCP prefixes these with `neverd_` and advertises them only when
the compiled policy is available.

```text
neverd web nexe ROOT PROFILE [ARTIFACT_INDEX]
neverd web nexe-source ROOT PROFILE ARTIFACT_INDEX MEMBER_INDEX SOURCE_TYPE
neverd web archive-nexe ROOT PROFILE tar|tgz|zip ARCHIVE_INDEX ARCHIVE_MEMBER_INDEX
neverd web archive-nexe-source ROOT PROFILE tar|tgz|zip ARCHIVE_INDEX ARCHIVE_MEMBER_INDEX MEMBER_INDEX SOURCE_TYPE
```

## Producer qualification

On 2026-10-11, controlled plain/minified CommonJS, asset and package-manifest
inputs were packaged with npm `nexe@4.0.0-rc.7`, Node 24.1.0 and npm 11.3.0 on
macOS arm64. Lifecycle scripts were disabled during isolated producer setup.
The three original bases came from nexe's `v3.3.3` release: Node 14.15.3
`linux-x64`, `mac-x64`, and `windows-x64`. Those targets are producer asset
labels; this envelope reader does not independently authenticate them.

All three full outputs were extracted through NeverD with an unusable PATH.
They contain the same 16,044-byte bootstrap and four original files totalling
365 bytes, each matched byte-for-byte/hash-for-hash against independent inputs.
Native header/ARM64 prefix fixtures additionally test prefix-independent
arithmetic; **ARM64 producer outputs and target runtime activation are not
qualified**. No sample was executed. Native prefixes, complete outputs, producer
metadata and a dependency lock are retained locally, not redistributed.

Exact artifact/base/input hashes and producer identity are recorded in
[`fixtures/nexe/manifest.json`](../unittests/web/fixtures/nexe/manifest.json).
`WebNexe.OptionalPinnedProducerImagesPreserveIndependentInputs` and
`WebNexeSDK.OptionalProducerSourcesBootstrapAndManifestUseSharedConsumers` use
`NEVERD_NEXE_4RC7_CORPUS`; missing artifacts explicitly skip. Tests never install,
download or execute a producer. The manual C++ fixture-input writer is excluded
from ordinary builds and tests. See the [fixture recipe](../unittests/web/fixtures/nexe/README.md).

The implementation is independent of upstream source; no nexe code is linked
or copied into the product. Format evidence is pinned to commit
[`c32a79a`](https://github.com/nexe/nexe/tree/c32a79a684011d7ac28a236593c514be441b6c27):
[`compiler.ts`](https://github.com/nexe/nexe/blob/c32a79a684011d7ac28a236593c514be441b6c27/src/compiler.ts),
[`bundle.ts`](https://github.com/nexe/nexe/blob/c32a79a684011d7ac28a236593c514be441b6c27/src/fs/bundle.ts),
[`boot-nexe.ts`](https://github.com/nexe/nexe/blob/c32a79a684011d7ac28a236593c514be441b6c27/src/patches/boot-nexe.ts),
and [`util.ts`](https://github.com/nexe/nexe/blob/c32a79a684011d7ac28a236593c514be441b6c27/src/util.ts).
Other nexe layouts, pkg, native loader validation and full project reconstruction
remain separate work; this increment does not complete #714–#718.
