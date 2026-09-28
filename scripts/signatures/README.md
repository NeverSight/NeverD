# Signature generation

Tooling that builds the `.pat` signatures in the `signatures` submodule
(NeverSight/signatures). Two pipelines use it:

- the exception-runtime databases, so that NeverD can locate a personality
  routine in an image that names nothing (most of this page), and
- the MSVC and Windows SDK databases under `pe/`, which name the runtime,
  STL, ATL/MFC and SDK code statically linked into Windows programs (see
  [MSVC and Windows SDK signatures](#msvc-and-windows-sdk-signatures)).

## What a signature line says

Every line is produced by `neverd-sigmaker` from a real library, and states
the bytes of one function with a wildcard wherever a relocation rewrites
them. The tail starts where the CRC span ends. A function whose line would
state fewer than sixteen bytes exactly is not written at all. `neverd::sigs::PatternGenerator` owns those rules, including how wide
each COFF relocation is. The name on a line is the linkage name the
library's symbol table spells, byte for byte: `?Close@CFile@@UEAAXXZ`,
`_ZNSt6thread4joinEv`, x86 `_memcpy`. It is never demangled, sanitized,
prefixed, or truncated. One public name sits at offset 0 of each line.

## Why the exception-runtime databases exist

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
whole-function coverage and at least sixteen bytes stated exactly. The
signature maker never writes a line that states fewer than sixteen bytes
(`SignatureMatcher::MinStatedBytes`), since such a line agrees with a great
deal of unrelated code. A line that only falls short of whole-function coverage
is still written, because it remains useful for renaming a function, but it
will never be allowed to decide a personality. The run fails outright
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

## MSVC and Windows SDK signatures

`build_msvc_signatures.py` builds `pe/<x86|arm>/<32|64>/vs<year>.pat` and
`winsdk.pat` from the library archives that the signatures repository's
`msvc-libraries.yml` workflow collects on GitHub-hosted Windows images and
keeps in a release:

- Visual Studio 2026 (its default toolset and 14.50) for x86, x64 and ARM64.
- Visual Studio 2022 (v143), 2019 (v142) and 2017 (v141) for x86, x64, ARM32
  and ARM64.
- Visual Studio 2015 (v140) for x86, x64 and ARM32.
- Windows SDK 10.0.17763 through 10.0.26100: the Universal CRT and the
  user-mode libraries.
- Visual Studio 2005 through 2013 for x86 and x64, and 2012 and 2013 for
  ARM32, unpacked from Microsoft's installation media together with the
  Windows SDK libraries those media install.
- Static libraries that belong to no Visual Studio release, such as the MASM32
  SDK's. Such a `library` asset names its own file (`masm32.pat`), and its
  `format` (`pe` by default, or `elf`) puts the file under `pe/` or `elf/`:
  the signatures repository files the packages its ELF files came from, such
  as Ubuntu's `libc6-dev` builds, as `elf` library assets.

```bash
cmake --build build --target neverd-sigmaker
gh release download <tag> --repo NeverSight/signatures --dir assets
python3 scripts/signatures/build_msvc_signatures.py \
    --sigmaker build/bin/neverd-sigmaker \
    --assets assets --output signatures
```

Each asset runs through the signature maker with `--machine` set to its
architecture, which keeps the COFF objects of that machine and the ELF
objects of its class and machine, and a tail that covers every function to
its end. Lines from
every servicing toolset of one Visual Studio year go into the same file, and
so do lines from every SDK version.

A file is rebuilt from its assets alone, so the lines it held before are
replaced; download every release before running the script, as the
signatures repository's `msvc-signatures.yml` does. The one other input is a
`<name>.imported` file next to the output, which the signatures repository
keeps for a release whose libraries it collects only in part: the lines an
earlier import holds for routines no collected library defines, renamed to
the linkage names the libraries spell. Its lines join the generated ones and
every rule below applies to them alike; its comment lines, which hold what
could not be renamed, are not read, and neither does the loader read the file.

ELF libraries give one routine several symbols -- glibc's `puts` is also
`_IO_puts` -- and the signature maker writes them as one line with each name
at offset 0. Lines that state the same bytes under names they share are one
routine's, and become one line with every name any of them gives it; the
matcher shows the name with the fewest leading underscores
(`preferredAliasOrder`). When two lines state the same bytes under names with
nothing in common, all of them are dropped and the count is reported: the pattern cannot tell those routines
apart, and picking one name would be a guess. The loader applies every file
of a directory together, so the rule spans the directory. Bytes that one
file's libraries give several names are dropped from every file there, and so
are bytes that two files name differently. Without this, a debug-CRT wrapper
that is unique among the SDK's libraries would still name the byte-identical
template instantiations that `vs2026.pat` had dropped as ambiguous.

Each line also names, as `^offset name`, the routines its function branches
to directly (`neverd-sigmaker --references`): the rel32 field of an x86 or x64
`call` or `jmp`, and the ARM64 `B`/`BL` or Thumb-2 `B.W`/`BL`/`BLX`
instruction. The matcher follows each branch in the image. A target that the
other matches name differently drops the match; a target they name the same,
or that the named routine's own pattern matches, confirms it; a target nothing
names, such as an import thunk, a veneer or a routine the program replaced,
decides nothing. Lines of one file that state the same bytes under different
names are therefore kept when, for every two of them, one offset holds a
reference in both to different routines: at a match only one of them can be
confirmed. Bytes that two files name differently are still dropped from both,
because the Rich header loads only one of them, where the line would have no
rival to be told apart from.
Copies of one routine whose builds call different routines, such as the
debug CRT's `_free_dbg` where the release CRT calls `free`, keep the
references they share. A loader older than the references rejects such lines.

The matcher compares a line only as far as the line's own length and accepts
any byte where the line has a wildcard, so a line is dropped as well when a
routine of another name, at least as long, states every byte the line states.
An ARM64 catch funclet that is nothing but a prologue is the typical case: its
line would name every function that begins with the same prologue. So is a
short routine whose relocated operands are wildcards in its own line but
fixed in a longer routine's.

A file larger than 50 MB (`--max-file-bytes`), which GitHub warns about and
at 100 MB refuses, is written as parts of even size: `<name>.pat`, then
`<name>.part2.pat` and so on. The loader reads the parts as one library:
their matches name it as theirs, and the Rich header chooses a release's
parts together.

Every file written is read back through `neverd-sigmaker --verify`, which uses
the loader's parser, because one bad line makes the loader reject its whole
directory. A `<name>.sources.json` file next to each `.pat` records the assets
it was built from, with their archive digests and toolset or SDK versions, and
the NeverD revision and release tag that produced it.
