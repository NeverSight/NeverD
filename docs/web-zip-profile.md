# Native ZIP32 evidence profile

`zip32-local-central-v1` extends the existing archive API with explicit format
`zip`. It reads immutable captured originals, retains member evidence and
provides available member bytes to source, maps, HTML, package, Bun, SEA and
native analysis. It does not run a ZIP program, target, runtime or installer.
NW.js and VSIX manifest/entry adapters are separate from this container profile.

## Admission

The reader validates a single-disk ZIP32 end record, exact central directory,
local/central field agreement, all member ranges and data descriptors. Local
records must cover the interval from byte zero to the central directory without
gaps or overlaps, although central entries may be in a different order. The
single terminal end record may have a bounded private comment. Stored and raw
deflate payloads must consume their exact input, produce their declared size
and match CRC32 before publication. CRC32 does not authenticate a publisher.

The profile accepts creator families DOS, Unix and macOS; extraction versions
1.0–2.0; ASCII names or explicit UTF-8; and bounded UT, UID v1 and NTFS timestamp
extra records with validated internal framing. Unknown extras, alternate-name
extensions, ZIP64, split disks, signatures, executable prefixes, unsupported
flags and newer versions refuse the entire archive. Original bytes remain in
the captured input after refusal. This profile does not include APK/IPA signing
or alignment rules and does not change the separate mobile archive reader.

Inside an otherwise admitted envelope, ordinary encryption and unknown
compression methods produce metadata-only members with a fixed reason. Deflate
without the linked decoder behaves the same way. These members still consume
declared size/name/metadata budgets and participate in all path collision checks.
They expose no decoded Blob/hash and cannot become source/package/native inputs.
Newer BZIP2/AES/strong-encryption envelopes are refused by version/extension
admission; their mere presence does not establish a supported opaque layout.

The existing ICU portable path policy checks original names and directory
prefixes. Absolute/traversal/device names, symlinks/special files, duplicate
members, Unicode/case aliases and conflicting file/directory uses refuse the
archive. No member name is used as a filesystem path. The only derived write is
one private, unlinked spool, sealed after every readable member validates.

## Evidence and budgets

Archive identity includes the explicit profile and original artifact/hash.
Member identity includes its ordered occurrence, so equal bytes do not erase
container provenance. Pages retain original local-header/payload/frame ranges,
expanded offsets, compression method, declared size, availability reason and
CRC status. Private names, comments, metadata values and raw bytes are omitted.
Source anchors for stored members map directly to original payload bytes.
Deflate anchors retain decoded-stream coordinates plus the entire member frame
(local header, payload and descriptor), without a fabricated byte mapping.
Nested Bun/SEA/HTML consumers retain the container-origin chain.

ZIP shares the four-entry archive cache and 512 MiB aggregate derived/pending
spool allowance with tar/tgz. Each file is limited to 256 MiB; archives have at
most 10,000 members, depth 64 and 8 MiB metadata including copied path prefixes.
Declared expansion is checked before decoding, with a maximum ratio of 1,000:1
against stored size (zero uses a denominator of one). Actual output is checked
during decoding and the whole archive shares an iteration budget. Import
replacement revokes caches and derived selections. No recursive ZIP expansion
or raw filesystem export is part of this profile.

## Interfaces and qualification

Use the existing `neverd_web_package_archive_extract_json` with format `zip`,
then its paged records and member IDs. Capabilities advertise the ZIP profile,
format and deflate availability separately from the tar/gzip profile. The
worker and compiled MCP use the same C API adapter and retain output redaction.

```console
neverd web archive INPUT.zip zip
neverd web archive-packages INPUT.zip zip 0 MEMBER_INDEX package-json
neverd web archive-bun INPUT.zip zip 0 MEMBER_INDEX
```

`WebZip.*` covers C++ hostile fixtures and two independent preserved libarchive
3.8.0 outputs, with no recorder dependency during tests. The
[fixture manifest](../unittests/web/fixtures/zip/README.md) pins their bytes.
SDK tests check package/source/native consumers, nested storage anchors,
revocation and actual CLI processes with an unusable PATH. Worker/MCP parity
uses the same public archive operations. Executed checks and host limitations
belong in the [implementation ledger](web-analysis-implementation.md).

The record layout follows [PKWARE APPNOTE 6.3.10](https://pkware.cachefly.net/webdocs/casestudies/APPNOTE.TXT).
No upstream reader code was copied. Supported ZIP records do not establish
framework detection, application activation, extension installation or native
backend recovery.
