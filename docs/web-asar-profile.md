# ASAR extraction profile

`asar-pickle-json-v1` is an offline, in-process C++ reader of explicitly
selected original artifacts. Its format reference is
[`electron/asar` e4fb057678562b7b6170699a046d983ae6d31cb8](https://github.com/electron/asar/tree/e4fb057678562b7b6170699a046d983ae6d31cb8).
Compatibility with this layout does not establish the producer's release,
Electron identity or publisher authenticity.

## Admission and integrity

The reader validates both Pickle headers, signed string-length bounds,
four-byte alignment, zero padding and the JSON extent before parsing. Packed
offsets are canonical unsigned decimal strings relative to `8 + headerSize`.
Every file range is checked with subtraction before addition. Nonempty packed
ranges cannot overlap; empty files may share a boundary. Unknown structural
metadata and duplicate decoded JSON keys are rejected. All structure/ranges
are validated before reading member payloads; failure publishes no extraction.

Directories, files and links have separate records. Link targets are preserved
privately and never followed. Input permissions are metadata only. Member paths
use `icu-77.1-nfc-casefold-portable-v1`: strict UTF-8 and portable components,
NFC/full case fold/NFC comparison keys, plus Windows device-name rules.
Comparison keys never rename evidence. The selected unpacked subtree is checked
under the same policy, including extra entries. Original directory capture and
future export have their own boundaries; this does not qualify Windows capture
or a general exporter.

SHA-256 integrity declarations are optional. For the supported algorithm, both
the whole-file digest and every declared block must match. This pinned writer
always flushes a final block, including an empty final block for an empty file
or an exact block-size multiple. Malformed declarations reject the extraction.
`verified_bytes` means equality with an archive claim, not authentication.

| Member condition | Availability | Integrity state |
|---|---|---|
| Readable bytes, no declaration | `available` | `missing` |
| Supported declaration matches | `available` | `verified_bytes` |
| Supported declaration differs | `integrity_mismatch` | `mismatch` |
| Present unknown algorithm and otherwise available bytes | `unsupported_integrity_algorithm` | `unsupported_algorithm` |
| No selected unpacked root | `unpacked_not_selected` | Declaration remains unverified |
| Absent/wrong type/wrong size | `missing_member` / `member_type_mismatch` / `size_mismatch` | Declaration remains unverified |
| Logical link | `link_not_followed` | No usable payload |
| Directory | `metadata_only` | No usable payload |

Unavailable members cannot supply bytes to source, map or native operations.
Per-member limitations remain visible in a successful metadata result with
`extraction_status:partial`. `complete` covers declared members only, and may
include files without integrity declarations. Unreferenced payload gaps/trailer
bytes are counted separately and remain preserved in the original archive.

## Captured unpacked association and consumers

The archive and optional unpacked directory are opaque occurrence IDs in the
same immutable snapshot. The backend does not search for `app.asar.unpacked`
or reopen a host path. Exact relative names select captured entries, without
normalization-based substitution or link resolution. A different directory
selection produces a different extraction identity. Import replacement clears
all extraction IDs; stale-revision requests fail.

Packed members are immutable slices of the original spool; unpacked members
reference the selected captured file. Neither is written out as an executable.
Source/map/native calls accept available `member_id` values directly. Source
anchors report archive byte ranges for packed members and actual captured-file
ranges for unpacked members, together with the archive declaration identity.
Source module comparisons use only the same extraction's admitted virtual
namespace; missing members remain unresolved. This is not runtime resolution.

Raw ASAR map members use the standard source-map parser and remain `unbound`
until an explicit association is made. Native handoff uses the existing
immutable-buffer loader; its independent handle owns bytes and provenance after
the web session or host inputs are destroyed. No sample or native addon is run.

## Interfaces and limits

The C ABI adds `neverd_web_asar_extract_json` and
`neverd_web_asar_records_json`; worker operations are `web_asar_extract` and
`web_asar_records`. Ordinary output omits member names, link targets and source
text. All consumers retain the same IDs and hashes.

```console
neverd web asar ./app.asar
neverd web inspect ./captured-app
neverd web asar ./captured-app 1 2
neverd web asar-source ./app.asar 0 - 0 module
neverd web asar-view ./app.asar 0 - 0 module
neverd web asar-navigate ./app.asar 0 - 0 module
neverd web asar-anchor ./app.asar 0 - 0 module 7 5
neverd web asar-map ./app.asar 0 - 1
neverd web asar-native ./app.asar 0 - 2
```

Indices in these examples are illustrative. Select the archive and unpacked
directory by their actual zero-based positions in `inspect` pages, then select
a member by `member_index` in ASAR pages. `-` means no unpacked association.
Names stay private. Inputs must remain the same between independent CLI runs;
SDK/worker callers can reuse a revision-bound capture. `asar-native-analyze`
additionally requests the existing static native pipeline.

| Resource | Ceiling |
|---|---:|
| Header / JSON depth / JSON nodes / one string | 8 MiB / 64 / 200,000 / 4 KiB |
| Declared members / directory nesting | 10,000 / 24 |
| One path / cumulative private paths (declarations plus selected unpacked index) | 4 KiB / 8 MiB |
| One file / cumulative declared file bytes | 256 MiB / 512 MiB |
| Integrity block size / block count per file | 8 MiB / 10,000 |
| Cached extractions / record-page entries | 4 / 512 |

These add to original capture, source, map and native budgets; they do not
increase those budgets. Available packed bytes are streamed through 64-KiB
reads. No full-member copy is needed for inventory or integrity verification.

`NEVERD_ENABLE_WEB_ASAR=ON` requires native ICU4C 77.1 (`ICU_ROOT` can locate a
prepared installation); runtime checks require Unicode 16.0. Without this
exact policy, capabilities mark ASAR unavailable, with no helper fallback.
ICU's tools are never invoked. ICU binaries are currently supplied by the
build/deployment environment; they are not downloaded or bundled by this
feature. Redistribution must provide their matching runtime libraries or a
qualified static link, in addition to the staged license notices.

Local qualification and missing host/release coverage are tracked in the
[support matrix](web-support-matrix.md). Preserved upstream fixtures have an
independent [manifest](../unittests/web/fixtures/asar/README.md). Electron
manifest/IPC inference, NW.js/VSIX extraction and safe raw export remain
separate work; this reader alone does not complete issue #716.
