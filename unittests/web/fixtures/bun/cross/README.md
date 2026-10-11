# Bun 1.4.2 cross-container corpus

These 20 inert graph ranges were recorded from complete compiler outputs on
2026-10-11. Each target has `plain`, `utf16`, `asset-map` and `cache-map` cases.
Inputs and their SHA-256 values are the exact self-authored files in the
[parent recipe](../README.md). The native runtime images are held outside Git;
the graph data and independent C++ member manifests are the portable corpus.

The trusted compiler is the same pinned `bun-v1.4.2` macOS arm64 binary as the
parent corpus: SHA-256
`35d20dd0263e5c950194434b925454fdfa9ba6e4467da960410fa05b08a7a5b5`.
All templates came from that release and their archives matched its published
SHA-256 digests. Template selection is explicit: fixture creation does not
download a target implicitly. Neither the recorder nor NeverD executes any
generated program, loads its runtime/cache or invokes the compiler.

| Corpus prefix | Compiler target | Release template archive SHA-256 | Template binary SHA-256 |
|---|---|---|---|
| linux-arm64 | bun-linux-arm64 | `54328bbc2d9c8e0c9f892c544d66c57a83b84139e34909e5ee81758f1ac8fda7` | `616f267a34278ff5ac282df37ffdfba1d7141f4f6926bca99af2cd6ef3ad32b1` |
| macos-x64 | bun-darwin-x64-baseline | `bad5bbd6cf14d0980d115f5954c9ff904df619d5e994d2da1ffccd3f316300b0` | `2fa513af22ac59e03aae640cad302e73cb1ddb0f6398501e2ddccf7dcd613596` |
| macos-arm64 | bun-darwin-arm64 | `90987a3a16d7db556d886ac3d551e7b6d3edf0a1cf43acaed622e8676be1d12f` | `35d20dd0263e5c950194434b925454fdfa9ba6e4467da960410fa05b08a7a5b5` |
| windows-x64 | bun-windows-x64-baseline | `78c221c2376f79731ccf4e4af0b3bb46d81fefa3296c5abee09ad8a1b21e68c6` | `15277c59ccd6c6c20f8dc9716c2b59c1776320d606b6a8658f70be8799519ca4` |
| windows-arm64 | bun-windows-arm64 | `a7a16b876a305fd1029c66dbd27007b4f6112ae896532f675878731a21e50cfd` | `3d7e98d3201c55c3bde6c069a5dfa0d34da9edc9145187c76594f685f8aa54c6` |

Archive asset names use `aarch64` where the compiler target above uses `arm64`.
Each manifest records the supplied target, complete container hash/size, exact
graph offset/hash, and every member pointer's offset/size/hash. Its module
encoding/loader/format/side values are independent reads of the producer bytes.
Cache contents depend on the compiler host/ABI and are opaque evidence; their
presence is not a claim that a foreign target can execute this cache.

The development working directory was
`/tmp/neverd-bun-cross-1.4.2/inputs`. The shared invocation, replacing the named
paths and `TARGET` explicitly, was:

```console
env -i PATH=/usr/bin:/bin /absolute/pinned/host/bun build --compile --target=TARGET --compile-executable-path=/absolute/pinned/template/bun --env=disable --no-compile-autoload-dotenv --no-compile-autoload-bunfig --no-compile-autoload-tsconfig --no-compile-autoload-package-json --root=. --outfile=/absolute/output/PREFIX-plain ./plain.js
```

For `utf16` use `utf16.js`; for `asset-map` use `asset.js` and append
`--minify --sourcemap`; for `cache-map` use `plain.js` and append
`--bytecode --sourcemap`. Windows outputs get an `.exe` suffix. Record each
output with the separately built, development-only C++ recorder:

```console
build-release/bin/neverd-web-bun-fixture-record /absolute/output/PREFIX-plain /absolute/corpus/PREFIX-plain TARGET
```

`WebBun.CrossPlatformCompilerGraphsMatchIndependentGoldenMemberRanges` always
checks the preserved payloads using explicitly synthetic container wrappers.
`WebBun.CrossPlatformFullCompilerContainersWhenSupplied` additionally requires
the exact complete images under `NEVERD_BUN_142_CROSS_CORPUS`, verifies their
recorded hashes, and reads their real headers. An absent full corpus is a skip.
This separates malformed-layout testing from actual compiler qualification.
