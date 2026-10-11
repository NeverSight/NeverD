# Offline package archive and integrity profiles

The C++ package pipeline admits an explicitly selected captured tar or tgz,
publishes immutable members, and can separately compare an original artifact
with a captured integrity declaration. No package manager, target, external
archive utility, hash utility, script or network request runs.

## Archive admission

`ustar-pax-single-gzip-v1` uses native linked zlib for a **single** gzip stream.
CRC and ISIZE must validate, all compressed bytes must be consumed, and a
concatenated stream or trailing data is refused. The bounded expanded stream
lives in a private, immediately unlinked spool. No member path becomes a host
filename. Sealing this private intermediate does not publish it: the whole tar
must validate before the session publishes any member.

The tar reader accepts POSIX USTAR headers with `ustar\0`/`00`, unsigned header
checksums and octal sizes/modes. Regular files, directories, symbolic links,
hard links and local PAX `x` headers are recognized. Link entries expose
metadata only; they have no content/hash and cannot supply a consumer's bytes.
Ownership and timestamps are not interpreted. Supported local PAX keys are
`path`, `linkpath`, `size`, plus inert `mtime`, `atime`, `ctime`, `uid`, `gid`,
`uname` and `gname`. Length framing is checked; duplicate/empty keys or values,
repeated pending PAX headers, unsupported keys and a terminal pending header
are refused. A local header applies only to its following member.

Global PAX, GNU long-name/base-256/sparse extensions, device/FIFO types and
other tar dialects are unsupported. Padding and the terminal stream must be
zero, with at least two terminal 512-byte blocks. This fixed profile is
narrower than general tar utilities; unsupported archives fail.
The final effective member names pass the same ICU 77.1 portable path policy
as ASAR. Traversal, absolute paths, separators, reserved names, normalized
Unicode/case aliases, duplicate entries and file/link ancestors are refused,
including conflicts involving implicit directories and either member order.

Limits include the entire expanded stream, including PAX, padding and trailing
zeros: 512 MiB per archive and across the session's archive cache; 256 MiB per
file; 10,000 members; depth 64; and 8 MiB aggregate PAX/name/prefix metadata.
There are at most four cached archives. Remaining cache bytes bound pending
decompression too. Only captured original artifacts may be archive inputs;
recursive archive expansion is not part of this profile. Imports revoke all
archive members and consumers.

`PackageArchive` owns framing and member evidence. `BlobStore` owns private
derived storage and sealed ranges. `ArtifactView` and `memberNamespace` supply
the same members to package, source, HTML, Bun and native consumers. Bun
selection/export accepts these immutable members without another reader.
Direct tar offsets refer to original bytes; gzip member offsets refer to the
expanded stream. Anchors retain its hash and the original containing frame,
and never invent compressed per-character offsets. Nested Bun assets retain
both the selected Bun container offset and the root storage coordinate space.
Native sessions retain their borrowed immutable storage after import replacement
or destruction of the originating web session.

## Original-artifact integrity

`npm-original-sri-v1` binds two explicit selections in one revision:

- A captured original artifact, never an extracted archive member.
- A captured registry JSON artifact's `dist.integrity`, or a package instance
  in an already admitted `npm-lock` analysis.

The caller chooses this association; filenames, URLs and package names do not
prove it. The result retains both original and declaration artifact hashes,
the package/registry selection and the comparison outcome. Neither declarations
nor raw private names, URLs, paths or script commands appear in ordinary output.
Registry metadata uses the bounded duplicate-key-rejecting JSON reader.

The graph classifier and verifier share one declaration parser. It admits
lowercase `sha1`, `sha256`, `sha384` and `sha512` with canonical padded base64,
at most 64 KiB and 256 tokens. Options, malformed digests, invalid padding and
excessive input remain explicit outcomes. Unsupported algorithms alone cannot
produce a match. Among supported algorithms, SHA-512 takes priority over
SHA-384, SHA-256 and legacy SHA-1; any candidate of that strongest algorithm
may match. A matching weak digest cannot hide a stronger mismatch.
Legacy SHA-1 results are labeled. Git commit declarations are distinct and
are never compared as archive digests.

SHA-1/256 use LLVM. The bounded C++ SHA-384/512 implementation follows
[FIPS 180-4](https://doi.org/10.6028/NIST.FIPS.180-4); independent empty, `abc`,
112-byte and million-`a` vectors cover padding and varied chunk boundaries.
Strongest-algorithm selection follows the
[SRI model](https://www.w3.org/TR/SRI/), with the stricter parsing subset above.
This is not a browser SRI implementation or cryptographic-module certification.
The verifier hashes **original compressed bytes**, independent of decompression.
There are at most 16 cached verifications, invalidated on import.

A match establishes byte equality with the selected declaration. It does not
authenticate a publisher, verify a signature/provenance attestation, establish
archive safety, or give a benignness/reachability verdict. A valid archive can
have a mismatching declaration; matching bytes can contain a refused archive.

## Public surfaces

Use `neverd web inspect ROOT` to choose original artifact indices, and archive
member pages to choose member indices:

```console
neverd web archive INPUT tgz
neverd web archive-packages ROOT tgz ARTIFACT_INDEX MEMBER_INDEX package-json
neverd web archive-bun ROOT tgz ARTIFACT_INDEX MEMBER_INDEX
neverd web integrity ROOT ORIGINAL_INDEX registry-dist REGISTRY_INDEX
neverd web integrity ROOT ORIGINAL_INDEX npm-lock LOCK_INDEX PACKAGE_INDEX
```

`integrity` exits zero only for a matching declaration; other outcomes retain
their metadata with exit status 1. C API functions are
`neverd_web_package_archive_extract_json`,
`neverd_web_package_archive_records_json`, and
`neverd_web_package_integrity_verify_json`; C++ worker operations use the
corresponding `web_` names. Empty `PackageID` selects registry metadata;
nonempty `PackageID` selects an existing lock analysis via `DeclarationID`.
Archive pages have a 1–512 limit. Extraction names and link targets remain
private. Native zlib and the pinned ICU path policy report independent
availability; unsupported analyzer hosts are not inferred from target formats.

## Qualification

Inert C++ fixtures cover framing, gzip corruption/concatenation, tar/PAX
mutation, expansion and metadata limits, path aliases/ancestors, sealed-storage
lifetime, no publication on failure, captured package/source/Bun/native
consumers, nested source coordinates and stale revisions. SDK/CLI and direct/
framed worker checks exercise the same owners with private canaries and an
unusable external-tool PATH.

Optional tests consume local copies only:

```console
NEVERD_CLAUDE_CODE_21296_NPM_DIR=/path/to/originals build-release/bin/NeverDWebArtifactTests --gtest_filter=WebPackageIntegrity.OfficialClaudeCodeTarballsWhenSupplied
NEVERD_CLAUDE_CODE_21296_NPM_LINUX_X64=/path/to/claude-code-linux-x64-2.1.296.tgz build-release/bin/NeverDWebArtifactTests --gtest_filter=WebPackageArchive.ClaudeCode21296NPMPayloadMatchesStandaloneWhenSupplied
```

The directory contains the two `.tgz` and corresponding `.registry.json`
files named in `PackageIntegrityTests.cpp`. Exact sizes, SHA-256 values,
independent registry declarations and the native-payload comparison appear in
the [Claude Code qualification](web-claude-code-qualification.md). Proprietary
inputs and recovered code are outside the repository.
