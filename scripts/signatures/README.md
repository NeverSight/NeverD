# Exception-runtime signature generation

Tooling that builds `.pat` signatures for the exception-handling runtimes, so
that NeverD can locate a personality routine in an image that names nothing.

## Why this exists

Locating a personality routine is a name lookup everywhere else in NeverD. A
CIE points at an address; `resolveRoutineName` asks the symbol table, the
import table, the export table, the relocations, and the `DW.ref.` slot what
lives there; `classifyPersonalityName` classifies whatever they answer.

A stripped, statically linked image answers nothing. The routine is an address
and nothing else, so every frame that installs it reports an unknown
personality. Schema-independent bytes may still have a provisional reading,
but personality-specific forms cannot be trusted yet.

Signatures close that gap, but only for the routines the personality table
already knows: a name NeverD cannot classify buys nothing at that address. So
the databases these scripts produce are deliberately narrow. See
`eh_runtime_symbols.py` for the list and why each family is on it.

## How a signature becomes a classification

```
.pat database ──▶ SignatureDB::identifyPersonalityRoutines(Img)
                        │
                        │  candidates: collectUnnamedPersonalityRoutines(Img)
                        │  — only addresses a frame installs as its personality
                        │    and that the image cannot name
                        ▼
                  SignatureMatcher::scanAtAddresses  (candidates only)
                        │
                        │  gate: isFullyVerified(Mod)      whole-function match
                        │        fixedByteCount(Mod) >= 16 not mostly wildcards
                        │        FuncRef::Offset == 0      names this routine
                        │        one name per address      no silent tie-break
                        ▼
                  neverd::adoptPersonalityRoutineName(Img, VA, Name)
                        │
                        │  refuses: a name the personality table does not know
                        │           an address the image already names
                        │           an address no frame installs
                        ▼
                  Img.Symbols += {Name, VA}
                  every frame with that PersonalityVA reclassified and its
                  language data refreshed from retained native provenance,
                  each carrying a diagnostic saying the name was inferred;
                  any incomplete refresh leaves the image unchanged
```

Nothing an image says is overwritten. A named binary behaves exactly as it did
before, so the pass runs unconditionally wherever a signature database is
loaded: `neverd sigs --auto`, `--sig-dir`, and `--sig-file`, and the three C
API entry points behind them.

It runs *after* the general match rather than before. `apply` begins by
clearing the match list, so the other order reports nothing; running second
also puts an adopted name in front of `buildNameMap`, which is what lets the
routine be renamed in the function listing and not only in the frames that
installed it.

## Running it locally

Build the signature maker, then point the driver at whatever the host has:

```bash
cmake --build build --target neverd-sigmaker
python3 scripts/signatures/build_eh_signatures.py \
    --sigmaker build/bin/neverd-sigmaker \
    --from-toolchain gcc --from-toolchain clang \
    --output signatures --name host-eh
```

To see what would be read without generating anything:

```bash
python3 scripts/signatures/eh_signature_inputs.py --compiler gcc
```

To see the coverage list and the reason each family is on it:

```bash
python3 scripts/signatures/eh_runtime_symbols.py
```

Archives can also be named explicitly, which is what CI does after building an
unwinder from source:

```bash
python3 scripts/signatures/build_eh_signatures.py \
    --sigmaker build/bin/neverd-sigmaker \
    --archive build-libunwind/lib/libunwind.a \
    --archive build-libcxxabi/lib/libc++abi.a \
    --name llvm-unwind --output signatures
```

Output lands in `<output>/<format>/<arch>/<bitness>/<name>.pat`, with the
triple read from the archive's own object headers rather than from a flag.
`elf`, `pe`, and `macho` crossed with `x86`/`arm` and `32`/`64` are the
directories the loader looks in.

### Reading the summary

The driver reports four numbers, and the last one is the one that matters:

```
6 archives, 4812 signatures generated, 96 exception-runtime signatures kept,
93 of them strong enough to name a personality
```

"Strong enough" means the line passes the same gate the loader applies —
whole-function coverage and at least sixteen bytes stated exactly. A line that
fails it is still written, because it remains useful for renaming a function,
but it will never be allowed to decide a personality. The run fails outright
when *no* line passes, because a database like that cannot do the job it was
built for; raise `--tail` if that happens.

## Installing the CI workflow into the signatures repository

`ci/build-eh-signatures.yml` is written for the external signatures
repository, not for this one. It is staged here so it can be reviewed
alongside the code that consumes what it produces, and it is inert where it
sits — this repository's own CI is owned elsewhere and does not read it.

To install it:

1. Copy the file to `.github/workflows/build-eh-signatures.yml` in the
   signatures repository.
2. Give that repository's Actions a `contents: write` token, or a deploy key,
   so the publish job can commit. The workflow requests write permission on
   the publish job only.
3. Set `NEVERD_REF` in the workflow's `env` to the NeverD revision whose
   `neverd-sigmaker` should generate the database. Pinning it is what keeps a
   regenerated database attributable to a known generator.

The workflow follows the producer/publisher shape the binary corpus uses. A
`sigmaker` job builds the generator once against the published prebuilt LLVM
package, so a signature run does not rebuild the compiler fork. A matrix of
`produce` jobs then builds each toolchain's unwinder — GCC's, LLVM's, the
mingw cross runtime, Rust's — runs the driver, and uploads one `.pat` tree per
leg as an artifact. A final `publish` job merges the trees and commits them,
and only on `main`; every other trigger stops after the artifacts, so a pull
request shows what would change without changing it.
