# SEA fixture provenance

`Record.cpp` is a manual C++ development tool, excluded from normal builds.
It writes trusted fixture source/configuration data and records exact upstream
compiler outputs. It never executes a compiler, target, or downloaded script.
No product or automated test depends on Node, postject or a package manager.

Node 22.15.0 commit `b009466555c360513b8012ce549f716501090ee5` generated
five preparation blobs on macOS arm64. The official compiler archive
`node-v22.15.0-darwin-arm64.tar.gz` matched the SHA-256 in the captured official
[release checksums](https://nodejs.org/dist/v22.15.0/SHASUMS256.txt):
`92eb58f54d172ed9dee320b8450f1390db629d4262c936d5c074b25a110fed02`.
This checksum comparison is not publisher-signature authentication.

The committed `manifest.json` records exact original sizes and hashes,
independent expected source strings and asset hashes. Plain, minified, asset
and code-cache blobs total 1,220 bytes and are preserved unchanged. The
5,644,095-byte snapshot stays in the optional local corpus because it includes
runtime snapshot material; only its hash/size is committed. V8 caches and
snapshots are never loaded by tests.

Reproduction, using an explicitly supplied trusted compiler:

```text
cmake --build build-web-dev --target neverd-web-sea-fixture-record
build-web-dev/bin/neverd-web-sea-fixture-record write /tmp/sea-cases
cd /tmp/sea-cases
/path/to/node-22.15.0 --random-seed=42 --experimental-sea-config plain.json
/path/to/node-22.15.0 --random-seed=42 --experimental-sea-config minified.json
/path/to/node-22.15.0 --random-seed=42 --experimental-sea-config assets.json
/path/to/node-22.15.0 --random-seed=42 --experimental-sea-config cache.json
/path/to/node-22.15.0 --random-seed=42 --experimental-sea-config snapshot.json
/path/to/neverd-web-sea-fixture-record record /tmp/sea-cases /tmp/sea-recorded
```

Generating the snapshot runs only the recorder's trusted fixture builder,
which registers an empty deserialize-main function. The extraction/tests do
not execute that function or any selected input. Captured hashes identify
these particular compiler outputs; byte-identical output on another build or
host is not promised.

`SEAFixture.h` supplies independent, deliberately synthetic ELF64, Mach-O64
and PE32+ headers for x64 and ARM64 plus hostile mutation cases. Those headers
prove reader boundary behavior, not that Node or postject produced the images.
`NEVERD_NODE_SEA_22150_CORPUS` opts into the captured snapshot fixture directory.

The reader is an independent implementation; Node and V8 implementation code
is not linked or copied into NeverD. The original upstream
[license](../../../../LICENSES/node/LICENSE) accompanies the fixture/profile
references.

## Injected native images

The optional `images.json` records six complete official Node 22.15.0 binaries
after injecting the same cache-free `assets.blob`. The recorder locates the
unique complete original 250-byte blob to establish independent expected
offsets; the production reader instead traverses native resource metadata.
The original downloads matched these official release checksums:

| Distribution | SHA-256 |
|---|---|
| `node-v22.15.0-linux-x64.tar.gz` | `29d1c60c5b64ccdb0bc4e5495135e68e08a872e0ae91f45d9ec34fc135a17981` |
| `node-v22.15.0-linux-arm64.tar.gz` | `c3582722db988ed1eaefd590b877b86aaace65f68746726c1f8c79d26e5cc7de` |
| `node-v22.15.0-darwin-x64.tar.gz` | `f7f42bee60d602783d3a842f0a02a2ecd9cb9d7f6f3088686c79295b0222facf` |
| `node-v22.15.0-darwin-arm64.tar.gz` | `92eb58f54d172ed9dee320b8450f1390db629d4262c936d5c074b25a110fed02` |
| `win-x64/node.exe` | `77bdff912b1c569b3e693fe126f619337c3e9d73dafbc4d0bf1d4f1f6a145761` |
| `win-arm64/node.exe` | `ebdab2adc9ff5fb09f852b665875af35138b9ee5725a3927bb85d7cfd36e059f` |

Trusted upstream postject `1.0.0-alpha.6` with commander `9.5.0` was used
manually only to construct these corpus files. Their npm archive SHA-512
matched the captured version metadata; archive SHA-256 values are in
`images.json`. No dependency was added to NeverD. The target binaries were
copied before modification, Mach-O signatures removed from those copies,
and the images were never executed. The postject ELF section-name and PE
signature warnings remain in local generation logs; runtime/signature validity
is not a qualification claim.

For each image, the resource argument was `NODE_SEA_BLOB`, the input was
`assets.blob`, and the fuse option was
`--sentinel-fuse NODE_SEA_FUSE_fce680ab2cc467b6e072b8b5df1996b2`.
Mach-O additionally used `--macho-segment-name NODE_SEA`. Image filenames are
`elf-x64`, `elf-arm64`, `macho-x64`, `macho-arm64`, `pe-x64`, `pe-arm64`.
Given sibling `cases` and `images` directories, record with:

```text
neverd-web-sea-fixture-record record-images /tmp/sea/images /tmp/images.json
```

`NEVERD_NODE_SEA_22150_IMAGES` enables full-image tests; only the golden
manifest, not the native runtimes, is redistributed here.
