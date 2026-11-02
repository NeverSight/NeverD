# Bun standalone extraction profiles

The C++ offline reader supports these Bun 1.4.2 container contracts. Invoke
`neverd web bun FILE`, or call
`neverd_web_bun_extract_json` on an admitted original artifact. Pages use
`neverd_web_bun_records_json` with `modules` or `regions`. The worker exposes
the corresponding `web_bun_extract` and `web_bun_records` operations.

`bun-71d0d439-prelinked-linux-x64-elf-v1` additionally admits the prelinked graph
and runtime-options extensions observed in the official Claude Code 2.1.296
Linux x64 artifact. Selection follows validated graph flags, not the filename.
Existing Linux x64 profiles and extraction identities are unchanged.

| Target | Container | Baseline layout profile |
|---|---|---|
| Linux x64 | ELF64 little-endian | `bun-1.4.2-linux-x64-elf-v1` |
| Linux ARM64 | ELF64 little-endian | `bun-1.4.2-linux-arm64-elf-v1` |
| macOS x64 | Thin Mach-O 64 little-endian | `bun-1.4.2-macos-x64-macho-v1` |
| macOS ARM64 | Thin Mach-O 64 little-endian | `bun-1.4.2-macos-arm64-macho-v1` |
| Windows x64 | PE32+ | `bun-1.4.2-windows-x64-pe-v1` |
| Windows ARM64 | PE32+ | `bun-1.4.2-windows-arm64-pe-v1` |

All six support inventory, JS/assets extraction and raw map/cache preservation.
Map decoding requires the separate native Zstd capability; bytecode decoding is
unsupported. The shared graph reader also recognizes the prelinked/runtime
extensions under corresponding `bun-71d0d439-prelinked-<target>-v1` profiles.
The [eight captured Claude npm platform packages](web-claude-code-cross-platform-qualification.md)
qualify real extension graphs, original-byte integrity and all shipped JS on
the six targets, including both supplied Linux libc variants. The hostile
extension cases remain synthetic. Metadata reports container format, target OS and
architecture separately from the analyzer's host. Universal Mach-O, 32-bit and
big-endian containers, ARM64e, FreeBSD/Android-specific contracts and stripped
ELF section tables are not qualified by this matrix. These are compatible
layouts, not authenticated compiler versions or libc/runtime ABI claims.

This is a selected layout profile, not authentication of the producer or its
version. Its response always reports `producer_version_verified:false`.
It does not launch Bun, run the executable, load JSC caches, resolve virtual
paths, inspect a user's installation or download missing artifacts.

## Authoritative layout references

The fixed source revision is
[`744846f844374847c902b5e7fd59b4342a51ef99`](https://github.com/oven-sh/bun/tree/744846f844374847c902b5e7fd59b4342a51ef99),
the commit referenced by the `bun-v1.4.2` tag and release:

- [ELF section writer](https://github.com/oven-sh/bun/blob/744846f844374847c902b5e7fd59b4342a51ef99/src/exe_format/elf.rs), Git blob `9d231ec56e0acfbbc0d880512f3e6628919df842`.
- [Mach-O section writer](https://github.com/oven-sh/bun/blob/744846f844374847c902b5e7fd59b4342a51ef99/src/exe_format/macho.rs), Git blob `59a66dabea863b1838fa52e8342e5ecb52d883f2`.
- [PE section writer](https://github.com/oven-sh/bun/blob/744846f844374847c902b5e7fd59b4342a51ef99/src/exe_format/pe.rs), Git blob `ab1a03d072a519c410c24d41f49b43437dea48cc`.
- [Standalone graph writer and records](https://github.com/oven-sh/bun/blob/744846f844374847c902b5e7fd59b4342a51ef99/src/standalone_graph/StandaloneModuleGraph.rs), Git blob `9ab6dbbf0f8dfaef469bab851b19989a65ff474c`.
- [Loader discriminants](https://github.com/oven-sh/bun/blob/744846f844374847c902b5e7fd59b4342a51ef99/src/ast/loader.rs), Git blob `cf748647a6d85cf7d2cfbab3d132db0795e42cab`.
- [StringPointer ABI](https://github.com/oven-sh/bun/blob/744846f844374847c902b5e7fd59b4342a51ef99/src/bun_core/util.rs), Git blob `28006517b5d1e9f0c9a8541b95581b3e03b6d0bd`.
- [StringBuilder empty-string terminators](https://github.com/oven-sh/bun/blob/744846f844374847c902b5e7fd59b4342a51ef99/src/bun_core/string/StringBuilder.rs), Git blob `21641fb924e053b1822d42090497f7b7d94c2185`.
- [Internal source-map windows, writer and `from_vlq`](https://github.com/oven-sh/bun/blob/744846f844374847c902b5e7fd59b4342a51ef99/src/sourcemap/InternalSourceMap.rs), Git blob `fbf1e5b1c52265b9c507f733cf00a490e5ce0a84`.

The reader is a NeverD C++ implementation of these data formats. No Bun Rust
code, Bun runtime or JavaScriptCore library is linked. The upstream license
statement is preserved under [LICENSES/bun](../LICENSES/bun/LICENSE.md).

The extension layout is pinned to Bun revision
[`71d0d439973bb327e57ff90032bfb72ba30c34ef`](https://github.com/oven-sh/bun/blob/71d0d439973bb327e57ff90032bfb72ba30c34ef/src/standalone_graph/StandaloneModuleGraph.rs).
Flag 11 adds a 128-byte-aligned opaque prelinked blob and a bounded table of
unique module indices after the earlier graph tables. Flag 12 adds an opaque
eight-byte runtime-options record. Counts, ranges, alignment and overlap are
validated before publication; runtime options never configure the host.
Flag 13's linked-bytecode layout is a different unsupported contract and fails
with `bun_unsupported_graph_flags`. This reader does not authenticate the Bun
version that built an otherwise compatible artifact.

## Admission and evidence

The ELF reader accepts bounded ELF64 little-endian x86-64/AArch64 ET_EXEC/ET_DYN images
with a unique `.bun` PROGBITS section. The section must have one consistent
file/virtual-address owner in a writable, non-executable PT_LOAD, with equal
file/memory sizes. Headers, section names, tables, section overlap and range
arithmetic are checked before reading graph records. Bun virtual starts follow
the pinned writer's 4-KiB x64 and 64-KiB AArch64 alignment.

The Mach-O reader bounds load commands/sections and selects one `__BUN,__bun`
section in a read/write, non-executable segment. Section and segment mappings
must agree, other segments cannot alias it, and segment padding must be zero.
The writer expands the segment to 16 KiB multiples while retaining the template
start: x64 permits 4 KiB starts, ARM64 requires 16 KiB. The section's recorded
alignment alone does not prove a 16-KiB x64 start.
Typed load-command sizes, contained strings, symbol/relocation/linkedit file
ranges and the entry offset are checked. Referenced metadata cannot overlap
the Bun segment. One macOS build/minimum-version declaration is required;
missing, duplicate, foreign-platform and unknown command contracts fail
explicitly. Field layouts follow Apple's
[Mach-O format declarations](https://github.com/apple-oss-distributions/xnu/blob/main/EXTERNAL_HEADERS/mach-o/loader.h);
the analyzer neither loads native libraries nor verifies a code signature.

The PE reader bounds DOS/COFF/optional headers, sections and directories. One
read-only initialized-data `.bun` section must have a complete file-backed
graph, checked file/section alignment and zero file padding. Other sections and
data directories cannot alias the selected file/virtual range. The security
directory uses file offsets rather than RVAs. Unsupported machine types and
ambiguous mappings fail before any graph content is published.

The section begins with an eight-byte graph length. The graph ends with a
32-byte offsets record and the fixed 16-byte trailer. Every module record is
52 bytes: six offset/length pointers followed by encoding, loader, format and
side bytes. The profile requires this writer's contiguous-source, source-hash,
builtin-table and startup-count flags; unknown flags fail. It validates the
optional builtin/shared-string tables in flag order, entrypoint less than
module count, startup count, NUL terminators, encoding, alignment and unique
module names. Payloads cannot overlap metadata or each other. Hashing starts
only after this validation, preventing pointer aliases from multiplying work.

The extraction also retains the ordered prelinked-file mapping and eight opaque
runtime-option bytes when their respective flags are present. Compile-time
arguments have a dedicated exact range, including empty arguments, appended
after existing regions to preserve their indices. These fields never configure
the analyzer's runtime. Local evidence manifests expose `startup_count`,
`flags`, `side_id`, `prelinked_module_files`, `runtime_options_bytes` and
`compile_exec_argv_file`; ordinary pages expose only counts, presence and a
private argument-region reference.

Limits are 256 MiB per original, 4,096 modules, 4,096 builtins, 1 MiB total
private name bytes (32 KiB per name), 4,096 section headers, 1,024 program
headers and four cached extractions per session. Original storage and source
parser ceilings remain independent. Original range hashing reads the container
partition plus the bounded compile-argument subrange of graph metadata; parsing
additionally reads bounded metadata.
There is no cancellation or whole-process memory guarantee for extraction.

Module pages link to raw content, name, source-map, cache, module-info and
cache-origin ranges. Region pages contain original container offsets, sizes,
SHA-256 and occurrence identities. Name/origin range hashes stay private to
avoid disclosing a standalone dictionary-checkable name digest; their IDs
bind the container identity, range and role rather than a separate name hash.
Prefix/suffix regions preserve the native
container remainder without claiming native-code recovery. Graph alignment
gaps are covered by the original artifact hash rather than separate regions.
Names, source, compile arguments and cache bytes are absent from ordinary
JSON output. Virtual `/$bunfs/` and Windows `B:/~BUN/` keys are opaque evidence, never host paths;
even a stored `..` segment cannot cause a filesystem lookup or export.

## Local evidence export

`neverd web bun-export FILE NEW_DIRECTORY` uses
`neverd_web_bun_export_json(session, revision, extraction_id, output_directory)`
with the normal C API length arguments. `SessionBunExport` consumes the already
captured original and validated extraction; CLI code does not extract bytes.
This is explicit local raw disclosure, including names and any embedded secrets,
and grants no network-upload permission. The worker has no export operation.

`neverd web archive-bun-export ROOT FORMAT ARTIFACT_INDEX MEMBER_INDEX NEW_DIRECTORY`
selects one available member of an explicitly admitted `tar`, `tgz` or `zip`
archive and uses the same extraction/export APIs. Obtain both indices from the
current inventories. It preserves the selected executable as `original.bin`
and retains its archive/member identities, enclosing archive hash and original
or expanded locators in `original_origin`. The enclosing archive remains in the
capture; this command does not copy it into the output directory. Compressed
byte offsets remain unavailable rather than being interpolated. Unsupported or
unavailable members cannot create an export directory. No archive path becomes
an output filename and no package entry or installer is invoked.

The C++ local writer creates a private directory and uses generated ordinal
filenames with exclusive no-follow writes. Existing destinations, including
symlinks, are refused. Virtual member names occur only as manifest data and
HTML-escaped index labels, never filesystem paths. `original.bin` retains the
whole original; `rNNNNN.bin` retains each exact region; `mNNNNN.js` contains
strictly decoded UTF-8 source. Storage hashes/offsets remain distinct from
decoded hashes. Every output is reread and SHA-256 checked. `manifest.json` is
published without replacing an existing file only after it is verified;
failed exports may leave a partial directory or `manifest.pending`. No crash
durability guarantee is made. Output is bounded by 1 GiB and 40,000 files.
The [Windows product contract](web-windows-product.md) records the shared
writer's separate Windows Server 2025 x64 fixed-NTFS qualification.

The shared decoder streams stored Latin-1/UTF-16LE in 64 KiB chunks, including
surrogate pairs across chunk boundaries. Explicit export permits at most
64 MiB decoded source per module. Interactive parser limits remain unchanged.
Unsupported projections preserve raw bytes and report a failure code; an empty
substitute is never written.

When the embedded parser is enabled, the separate sequential recovery profile
`hermes-602befee-recovery-js-v2` admits at most 32 MiB of source, 2,000,000 nodes,
2,000,000 lexemes, 16,777,216 decoded UTF-16 code units, a 256 MiB parser arena and
1,600,000 work units with a cooperative 30-second check. It does not enlarge
interactive caches or claim a process-wide memory/CPU bound. This uses the
same parser and semantic admission, including the async-rest source-location
fix and parser-token check for forbidden rest trailing commas. The ordinary
parser profile is now `hermes-602befee-js-v3`.

`mNNNNN.readable.js` only inserts whitespace at parser-owned token boundaries.
All original bytes, including comments and licenses, remain in order. Before
publication NeverD reparses the candidate and compares node kinds, strictness,
child roles/ordinals and complete retained attribute values. The status is
`verified_same_parser_tree`, not a runtime equivalence proof: source text
reflection and source positions necessarily change. Failed parsing, budgets or
tree mismatches produce no readable file and retain diagnostic codes and
original UTF-8 offsets in the manifest. The private C++ parser extension retains
explicit resource-management declarations (`using` / `await using`); see the
[resource-management profile](web-resource-management-profile.md).

The index contains no JavaScript and declares `default-src 'none'`. Export
does not run target code, install packages, invoke a formatter or restore
deleted names, comments, types, original TypeScript modules or native/JSC
machine code. Raw maps and caches remain preserved regions. See the
[Claude Code qualification](web-claude-code-qualification.md) for a full real
artifact run and its measured recovery boundary.

## Source and native consumers

A JavaScript module also has a distinct `source_artifact_id`. The existing
source API can decode Latin-1 or UTF-16LE into strict UTF-8, or validate stored
UTF-8 client code, within its ordinary source budget. Lone UTF-16 surrogates
refuse projection; original bytes remain intact. The source's hash is over
decoded bytes and its spans refer to that projection. The linked raw region
retains the original hash and container offset. `source_anchor` now translates
UTF-8 selections through that same decoder into exact original Latin-1/UTF-16LE
ranges, or identical client UTF-8 bytes. Endpoints must be scalar/CRLF boundaries;
surrogate pairs cannot be split and the storage terminator is excluded.
Storage hashes and decoded-source hashes remain distinct. A source view can be
joined through its committed view ID; hidden interiors select whole labels.
The `bun-navigate`, `bun-view` and `bun-anchor` CLI commands select an explicit
module index and source type without an external extractor or temporary JS file.

Module-info, builtin/module JSC caches and shared tables remain opaque hashed
ranges; no JSC cache ABI or V8 bytecode interpretation is claimed. The original
container and native asset members can enter an independent native session
through [explicit native handoff](web-analysis.md#explicit-native-handoff).
The container hash or exact member range remains attached to that session;
encoded source, maps, cache and runtime-prefix regions cannot be selected as
standalone native images. The independent SEA/pkg/nexe adapters have their own
[versioned contracts and qualification](web-native-javascript-matrix.md).
Broader runtime-version coverage requires separate evidence. Native-loader
support alone does not qualify a new Bun layout; the six targets above have
separate compiler-container and member-hash evidence.

## Serialized source maps

`bun-1.4.2-serialized-source-map-v1` decodes an extracted module's private map
on explicit request. Pass its `source_map_region_id` to the existing
`source_map_analyze` API, then use ordinary map sources/segments/lookup queries.
The CLI equivalent is `neverd web bun-map FILE MODULE_INDEX` (zero-based).
Extraction itself does not require map decoding: a malformed map can remain
original evidence while the decode request fails without publishing a map.

The format begins with two little-endian u32 values: source count and internal
mapping length. Two arrays of u32 offset/length pairs locate UTF-8 filenames
and individual Zstd-compressed UTF-8 contents. The mapping blob follows those
tables; all names, then all frames, partition the remaining bytes exactly.
Pointers are relative to this serialized map and carry no NUL terminator.
NeverD rejects aliases, gaps, overlaps, invalid UTF-8 and trailing bytes.

The internal mapping header carries length, count, input-line count, sync count
and stream offset. Each 24-byte sync entry seeds a window of at most 64 anchors;
32-byte window headers select bounded zigzag-varint lanes and equality masks.
The reader checks every window offset/count, consumed lane byte, mask bit,
optional exception index, signed-coordinate accumulation, source index and
generated-position ordering, including duplicate anchors. It never casts
input bytes to native structs or uses Bun's trusted-memory lookup routines.

Zstd decompression uses LLVM Support's linked native library in the same
process. If LLVM was built without Zstd, CMake optionally selects the exact
Zstandard 1.5.7 static SDK (`zstd::libzstd_static`). The header and library
versions are checked, no decoder is downloaded, and no command-line utility
is invoked. `compression/Zstd` owns this selection; the format reader retains
the C++ preflight against the
[Zstandard frame specification](https://github.com/facebook/zstd/blob/v1.5.7/doc/zstd_compression_format.md):
one ordinary frame, known content size, bounded window, no dictionary field,
reserved bits, skippable/concatenated frames or trailing bytes. Every frame is
preflighted against the cumulative output budget before decompression begins.
The native decoder checks compressed blocks/checksums; failures are fixed
diagnostics, never empty-source substitutions.
Capabilities report `available_on_request` or `zstd_unavailable`; a missing
native decoder produces `bun_source_map_zstd_unavailable`, never a subprocess.
To check that omission with a Zstd-free LLVM build, configure
`-DCMAKE_DISABLE_FIND_PACKAGE_zstd=ON`. To select an installed SDK, set
`zstd_DIR` to its CMake package directory. Static linking avoids a new runtime
library lookup; the [BSD notice](../LICENSES/zstd/README.md) is distributed
beside the binaries and in the SDK/install tree, including parser-disabled builds.

Limits are 8 MiB stored map bytes, 10,000 sources, 100,000 anchors, 4 MiB decoded
bytes per source, 8 MiB decoded bytes total, 8 MiB window and 100,000 blocks per
frame. Names share a 1 MiB budget with a 32 KiB per-name ceiling. Decoded maps
share the existing four-map/200,000-segment cache budget. JavaScript parsing
retains its smaller 1 MiB limit; accepting source content does not imply that
the parser accepts it. No process-wide memory or hard CPU deadline is claimed.

The producer drops symbol names and one-field unmapped segments while building
this format, and does not serialize `sourceRoot`, `file` or ignore lists.
Responses explicitly report these losses. `mapping_coverage` is
`retained_mapped_anchors_only`; `name_present` and `ignored_claim` are null,
not assertions that those properties were absent. A preceding anchor does not
establish the mapped/unmapped status of intervening text. Lookup reports
`mapping_scope: anchor_only_intervening_boundaries_unknown`.

The map summary links the module, container and exact raw map/mapping ranges.
Source rows keep compressed range/hash and decoded UTF-8 hash/identity separate.
Path bytes and path hashes remain private. Embedded source IDs feed the same
source parser; decompression itself never executes or fetches them. Association
with the module's decoded source is a `container_assertion`, with
`provenance_verified:false`. Passing another source to lookup is explicitly a
`caller_assertion`. Format checks do not authenticate producer version or
whole-map coordinates; lookup checks returned anchor boundaries only.

## Qualification

The self-authored corpus records and regeneration inputs are in
[`unittests/web/fixtures/bun`](../unittests/web/fixtures/bun/README.md).
Five real Linux x64 baseline executables were built with the pinned official
compiler on macOS arm64, then read without execution. They cover ordinary
source, Unicode literals, actual UTF-16 source storage, minification plus a
binary asset and map, and CommonJS source plus JSC cache and map. Full-image
hashes and original member hashes are recorded independently of the reader.

The small checked-in fixtures preserve the exact graph bytes, not the native
runtime. Default C++ tests use explicitly synthetic container wrappers for these
graphs plus hostile mutations. The full-container test requires the separately
supplied corpus. Ordinary local runs explicitly skip absent inputs; the
[required producer qualification](web-producer-corpus-qualification.md) turns
missing inputs or consumers into failures. A successful small fixture test
alone must not be reported as full-container qualification.

The [cross-container corpus](../unittests/web/fixtures/bun/cross/README.md) adds
20 complete compiler images: four cases for each additional target. The C++
recorder captured member ranges independently. All 20 extraction tests passed
with both full corpora and the official Claude Code 2.1.296 ELF supplied on
2026-10-11; this includes 25 self-authored full images, cross-container hashes,
UTF-16, assets/maps/caches and malformed-container refusal. Tests run with an
unusable external-tool PATH. This does not count raw map preservation as map
decompression or bytecode execution.

```console
cmake --build build-release --target NeverDWebArtifactTests NeverDWebSourceMapTests NeverDWebSDKTests
NEVERD_BUN_142_CORPUS=/path/to/pinned-corpus NEVERD_BUN_142_CROSS_CORPUS=/path/to/cross-images build-release/bin/NeverDWebArtifactTests --gtest_filter='WebBun.*'
build-release/bin/NeverDWebSDKTests --gtest_filter='WebSDK.Bun*'
build-release/bin/NeverDWebSourceMapTests --gtest_filter='WebBunSourceMap.*'
ctest --test-dir build-release/tools/neverd-worker -R NeverDWorkerWeb --output-on-failure
```

The corpus compiler and development recorder are never prerequisites for
analysis, ordinary builds or tests. The CLI/worker tests deliberately use an
unusable external-tool PATH. The complete producer corpus is qualified on the
macOS arm64 Release reader. The later [Windows Server 2025 x64 product](web-windows-product.md)
separately qualifies native ownership and selected Bun source/map/export
consumers; its required selection does not run every full producer image.
Other host coverage requires independent evidence.

The preserved minified asset and cached CommonJS graphs also exercise actual
serialized maps and raw/compressed Zstd blocks. Their recovered source hashes
match the independently recorded input hashes, and returned anchors stay within
the original/generated texts. C++ synthetic fixtures exercise Unicode, empty
maps/sources, rare deltas, multiple windows, duplicate anchors, malformed lanes,
truncation at every byte, frame checksums and exact/over-budget decompression.

### Native static SDK qualification — 2026-10-12

The prebuilt LLVM 23.0.0 r4 package omits Zstd. The optional native static
Zstandard 1.5.7 SDK decoded twelve complete pinned Bun 1.4.2 images on the
macOS arm64 Release host: asset-map and cache-map for each baseline target.
The C++ corpus test verifies the independently recorded full-container hashes,
original source hashes, every original/generated coordinate and stored frame
anchors. The asset input SHA-256 is
`ada8ba82b03b7bd85876cc812e0108150d30465e0945ac4b0798ecdb7db31433`;
the cache input SHA-256 is
`dae4f14862158ace4ba0cfb51115157698a5286192f7883ff89123a17b4ad687`.
Container hashes remain in the existing Linux and cross-target manifests.

The actual `neverd web bun-map <image> 0` command succeeded for all twelve
images with `PATH=/neverd-no-external-tools`. Each metadata-only JSONL report
retains the raw container/map/source anchors and recovered source hash:

| Target | Asset report SHA-256 | Cache report SHA-256 |
|---|---|---|
| Linux x64 ELF | `7365cce8c5ebba6587b9e26659bcfde7c95784760c0d1492034307b115ab43be` | `cd26f142b25c8d1f3b1155ab4cdb79ae50b6ed0014b3cfbd59b2a56d0daf6fde` |
| Linux ARM64 ELF | `e304668e81ded92bac7d9befd4dcfc6a3494e4264a0b833bb093fcf461ddaa2e` | `7feab79d84f4675ff0bcdcdd88b041156584700e9271796138d0fb1543da707d` |
| macOS x64 thin Mach-O | `2f1355a7e96410336bdb0133a53b8b8a1c17c9f48be88949a5644efa457616a9` | `eeb2fe048c590897563a2e186d3398bbce93a9ec28ddc44da19c0e0f8e242d2f` |
| macOS ARM64 thin Mach-O | `04118f65dab7a336032248cc6fdb75fbb02f2f614eaeafb7d5acb2cf3681f200` | `216affa3b1b44bc83b9d4c745a33672d55cf911b50b49fdbeb48f659d56822da` |
| Windows x64 PE | `ef6af698fee8b7c6171fc7d2b32ccc504d100ffced4506816e120bb2693a315b` | `1366f6b52973246bf0722413de8747d201c842ba61cefe41114c81574ad2c54b` |
| Windows ARM64 PE | `c1e7e1857c5526d98bb0d42a31270e43e9db312b79ddf4a8e9d961b920c82b35` | `b042bd4005b83828e31c1124ac22627fdbfdc1ff0fa3c5d0ea609cc03dcfde88` |

Reports are preserved with the local recovery evidence. The Linux x64 module,
decoded map and decoded source pages are byte-identical to the earlier LLVM
decoder reports; the extraction summary additionally has the already published
container/platform/architecture fields. These tests execute NeverD only.
They do not execute the extracted target or recover opaque caches. The official
Claude Code 2.1.296 container has zero source-map regions; these producer
fixtures do not imply original TypeScript recovery for that separate artifact.
