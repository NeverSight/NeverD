# Bun 1.4.2 fixed corpus

These inert inputs were authored for NeverD. The `*.graph.bin` files are exact
graph ranges from real compiler outputs, captured on 2026-10-10. They contain
source/asset/map/cache data and possible Bun-generated JavaScript scaffolding,
not a Bun native runtime. Upstream source and license references are recorded
in [the profile](../../../../docs/web-bun-profile.md) and
[third-party notices](../../../../THIRD_PARTY_NOTICES.md).

`*.manifest.json` contains the whole original ELF hash/size, graph hash/offset,
and six original pointer ranges for each module, in this order: name, content,
serialized map, bytecode, module-info, bytecode-origin path. A zero-length
optional pointer is absent. `Record.cpp` made these records by independent
little-endian field reads; it does not call NeverD's extractor.

## Pinned development toolchain

Official [bun-v1.4.2 release](https://github.com/oven-sh/bun/releases/tag/bun-v1.4.2),
commit `744846f844374847c902b5e7fd59b4342a51ef99`:

| Artifact | SHA-256 |
|---|---|
| `bun-darwin-aarch64.zip` | `90987a3a16d7db556d886ac3d551e7b6d3edf0a1cf43acaed622e8676be1d12f` |
| Host compiler binary | `35d20dd0263e5c950194434b925454fdfa9ba6e4467da960410fa05b08a7a5b5` |
| `bun-linux-x64-baseline.zip` | `c678040f14fe0440eb839d37cbd0ce4c051a32da72806ac97de6a6aab6bf728f` |
| Target runtime template binary | `a83d263767d839e4d2649ca8e35d07159c7afc99afdc96d731ced29e056dda0c` |

Fixture creation is a separate, manual development activity permitted by
#718's fixture contract. Only the trusted compiler reads these self-authored
inputs; the produced executables are never run. The C++ analyzer, tests and
production build neither invoke nor need Bun, Node, package managers or this
recorder. No fixture-generation script is supplied.

## Exact inputs and build recipe

Use an empty directory with these exact UTF-8 files (each ends in a newline):

`plain.js`:

```javascript
export const message = "NEVERD_BUN_FIXTURE_CANARY";
export function increment(value) { return value + 1; }
```

`unicode.js`:

```javascript
export const message = "NeverD 中文 🌱";
export function increment(value) { return value + 1; }
```

`utf16.js`:

```javascript
/*! NeverD 中文 🌱 — self-authored UTF-16 storage fixture */
export const message = "NeverD 中文 🌱";
export function increment(value) { return value + 1; }
```

The retained comment forces this output into actual UTF-16 storage. Merely
using Unicode literals does not: Bun escapes those in the emitted source.

`asset.js`:

```javascript
import resource from "./asset.bin" with { type: "file" };
export { resource };
export const message = "NEVERD_BUN_FIXTURE_CANARY";
```

`asset.bin` is exactly `NEVERD_BUN_ASSET_CANARY` followed by one LF.

| Input | SHA-256 |
|---|---|
| plain.js | `dae4f14862158ace4ba0cfb51115157698a5286192f7883ff89123a17b4ad687` |
| unicode.js | `8081e11d7f51ba486a0c2b85c9d4003bf972cbed7497771c98406a347e7919a3` |
| utf16.js | `99a53185e8e5ad73629e5a8840ddcc4c7e2abbdd917c4d855ca099e58762eb70` |
| asset.js | `ada8ba82b03b7bd85876cc812e0108150d30465e0945ac4b0798ecdb7db31433` |
| asset.bin | `9979487eb24cc637fd385f64dcfd4adee34b3b5b117a8ea60c925971422a2fd6` |

All invocations use the verified host compiler with the same common arguments:

```console
env -i PATH=/usr/bin:/bin /absolute/pinned/host/bun build --compile --target=bun-linux-x64-baseline --compile-executable-path=/absolute/pinned/target/bun --env=disable --no-compile-autoload-dotenv --no-compile-autoload-bunfig --no-compile-autoload-tsconfig --no-compile-autoload-package-json --root=. --outfile=plain.elf ./plain.js
```

For the remaining cases, replace only the output/input and append these flags:

| Output | Input | Additional flags |
|---|---|---|
| unicode.elf | ./unicode.js | None |
| utf16.elf | ./utf16.js | None |
| asset-map.elf | ./asset.js | `--minify --sourcemap` |
| cache-map.elf | ./plain.js | `--bytecode --sourcemap` |

The original working directory was `/tmp/neverd-bun-qualification-1.4.2` on
macOS arm64. The first plain invocation additionally set HOME to that empty
directory; later invocations left it absent. Cache bytes can depend on compiler
environment/ABI; compare manifests rather than assuming bit reproducibility
across hosts. Template selection prevents automatic cross-target downloads.

Build the optional recorder explicitly and run it on each fixed output:

```console
cmake --build build-release --target neverd-web-bun-fixture-record
build-release/bin/neverd-web-bun-fixture-record /path/to/plain.elf /path/to/plain
```

It writes `.graph.bin` and `.manifest.json`. Review new hashes before replacing
the golden corpus. The five graph sizes total only 8,249 bytes. Full ELF files
are held separately; do not silently substitute the synthetic test wrapper
for the full-container test. Set `NEVERD_BUN_142_CORPUS` to the exact full corpus
directory to run that test; absence is reported as a skip.
