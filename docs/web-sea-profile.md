# Offline Node SEA extraction

This independent C++ reader implements the Node **22.15.0** little-endian,
64-bit SEA serialization layout. It does not use the Bun graph reader, link
Node/V8, execute the selected program, or invoke a package manager or external
analysis tool. An explicit profile is required; layout compatibility does not
authenticate the producer version or establish that a native runtime will
activate the resource. The fuse, signatures, permissions and runtime behavior
are not checked.

## Profiles and ownership

| Profile | Selected resource |
|---|---|
| `node-sea-22.15.0-blob-le64-v1` | Entire preparation blob; architecture is not encoded |
| `node-sea-22.15.0-elf-x64-v1`, `node-sea-22.15.0-elf-arm64-v1` | ELF64 little-endian PT_NOTE, exact `NODE_SEA_BLOB\0` name |
| `node-sea-22.15.0-macho-x64-v1`, `node-sea-22.15.0-macho-arm64-v1` | Thin Mach-O64 `NODE_SEA/__NODE_SEA_BLOB` |
| `node-sea-22.15.0-pe-x64-v1`, `node-sea-22.15.0-pe-arm64-v1` | PE32+ `RT_RCDATA/NODE_SEA_BLOB`, exactly one language leaf |

`sea/Container` owns explicit selection; the three format readers own bounded
resource locations and file/virtual mapping checks. These checks establish
resource location, not complete native-loader validity. ELF section headers
and unrelated Mach-O command payloads are not interpreted. PE traverses only
the selected resource type/name path; it does not validate every unrelated
resource subtree. Duplicate or conflicting selected mappings refuse.
Universal Mach-O, 32-bit targets, big-endian layouts and other SEA versions
need separate profiles.

ELF checks the bundled postject runtime's raw eight-byte name prefix as well
as the complete declared name. Short names crossing into following bytes and
competing prefixes refuse. Every traversed PT_NOTE has complete file backing
and one consistent PT_LOAD mapping; notes are charged cumulatively even when
program headers repeat a range. Mach-O rejects duplicate SEA segments,
duplicate selected sections, zero-fill resources and overlapping selected
file/virtual mappings. Noncanonical Mach-O name padding that the runtime would
match also refuses. PE checks directory key ordering, exact duplicate keys,
revisited directories, directory/data bounds and unique file backing. Multiple
languages refuse instead of guessing the runtime's language fallback.

## Stored bytes and public evidence

`sea/Blob` reads the fixed magic, flags, two 64-bit-length-prefixed fields,
optional V8 cache and optional counted key/value assets. Unknown flag bits,
snapshot plus cache, duplicate asset keys, truncation, overflow and trailing
bytes refuse atomically. Asset order is preserved. Keys are private byte
strings, never output filenames or an inferred filesystem/module namespace.

The retained regions partition the entire selected container: native prefix
and suffix, serialization fields, code path, main JavaScript or V8 snapshot,
optional cache, asset keys and assets. Every region has an occurrence ID and
exact offset/size; public byte regions carry SHA-256. Code paths, asset keys
and their individual hashes are withheld. No URL, source text or asset payload
appears in ordinary JSON.

Only stored JavaScript and assets expose `selection_id`. The main source also
has `source_artifact_id` and the CommonJS hint. This identifies stored bytes,
not syntax validity or original project reconstruction. Existing explicit
source, map, package and native consumers accept selections through the
shared `ArtifactView`; sources keep exact original storage anchors, including
a containing compressed frame when nested in tgz. Maps require explicit
selection and are not authenticated by an asset filename. V8 snapshots/caches
remain opaque and do not expose source selections.

`SessionSEA` owns publication, revision checks, four cached extractions and
metadata-only pages of at most 128 records. Import revokes the cache and
derived selections. C API/CLI/worker adapters share this implementation.

## Bounds

| Resource | Limit |
|---|---|
| Selected input | 256 MiB, immutable existing storage |
| Assets | 4,096 |
| One private code path/key | 32 KiB |
| Aggregate private code path/keys | 1 MiB per extraction |
| ELF program headers / cumulatively visited notes | 1,024 / 8,192 |
| Mach-O commands / total sections / command bytes | 4,096 / 4,096 / 1 MiB |
| PE sections / cumulatively visited selected directory entries | 512 / 4,096 |
| PE header offset / optional-header bytes | 1 MiB / 4 KiB |
| Native resource-name bytes, charged on every visit | 1 MiB; one name 32 KiB |
| Retained extractions / page records | 4 / 128 |

Payload hashing streams from the existing spool. No full native image is
materialized by the extractor. Source/map/native consumers keep their own
smaller admission or materialization limits.

## Interfaces

```text
neverd web sea FILE_OR_ROOT PROFILE [ARTIFACT_INDEX]
neverd web sea-source FILE_OR_ROOT PROFILE [ARTIFACT_INDEX]
```

`sea` emits metadata and all region pages. `sea-source` analyzes only the
stored main script as CommonJS; a snapshot-only resource reports
`sea_source_not_stored`. These commands do not print raw source.

C API: `neverd_web_sea_extract_json`, `neverd_web_sea_records_json`.
Worker: `web_sea_extract` (`revision`, `artifact_id`, `profile`) and
`web_sea_records` (`revision`, `extraction_id`, optional `offset`/`limit`).
Extraction remains available without the JS parser; backend-off C ABI stubs
return `capability_unavailable`.

## References and qualification

The version pin is Node commit
`b009466555c360513b8012ce549f716501090ee5`.
Primary layout references:
[serializer](https://github.com/nodejs/node/blob/b009466555c360513b8012ce549f716501090ee5/src/node_sea.cc),
[primitive framing](https://github.com/nodejs/node/blob/b009466555c360513b8012ce549f716501090ee5/src/blob_serializer_deserializer-inl.h),
[runtime resource lookup](https://github.com/nodejs/node/blob/b009466555c360513b8012ce549f716501090ee5/deps/postject/postject-api.h)
and [versioned SEA documentation](https://nodejs.org/download/release/v22.15.0/docs/api/single-executable-applications.html).
The upstream [license and notices](../LICENSES/node/LICENSE) are preserved.

The [fixture recorder and manifest](../unittests/web/fixtures/sea/README.md)
separate real compiler-produced blobs from synthetic native-container fixtures.
Real injected-image qualification and final integration results are recorded
in the implementation ledger. None of this closes #718 or the other four
JavaScript epics; pkg/nexe and broader runtime/host profiles remain independent
work.
