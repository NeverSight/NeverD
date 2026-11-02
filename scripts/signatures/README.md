# Signature generation

Tooling that builds the `.pat` signatures in the `signatures` submodule
(NeverSight/signatures). The byte-signature pipelines use it:

- the exception-runtime databases, so that NeverD can locate a personality
  routine in an image that names nothing (most of this page), and
- the MSVC and Windows SDK databases under `pe/`, which name the runtime,
  STL, ATL/MFC and SDK code statically linked into Windows programs (see
  [MSVC and Windows SDK signatures](#msvc-and-windows-sdk-signatures)).

## Library feature packs

`library_features.py` dispatches to the pinned signature repository's producer
and validator. That repository owns the feature schema, source manifests,
compiler probes, archived evidence and repeat-build checks; the engine does
not keep a competing copy. The producers reuse the existing MSVC header
collector and NeverD signature maker for gated byte rules.

```sh
python3 scripts/signatures/library_features.py validate
python3 scripts/signatures/library_features.py probe --help
python3 scripts/signatures/library_features.py libcxx --help
python3 scripts/signatures/library_features.py msvc --help
python3 scripts/signatures/library_features.py musl --help
python3 scripts/signatures/library_features.py repeat --help
```

`--signatures /path/to/checkout` selects an authoring checkout before the command.
Producer arguments pass through unchanged; retain the actual compiler/header
manifest and any required independent repeat build. Data validation does not
claim consumer success. Run the engine's `NeverDLibraryRecognitionTests` and
source/session tests separately, and record the tested NeverD and feature SHAs
in the feature coverage receipt. See [library recognition](../../docs/library-recognition.md).

## What a signature line says

Every line is produced by `neverd-sigmaker` from a real library, and states
the bytes of one function with a wildcard wherever the linker may rewrite
them. The tail starts where the CRC span ends. A function whose line would
state fewer than sixteen bytes exactly is not written at all. `neverd::sigs::PatternGenerator` owns those rules, including the bytes
each COFF, ELF and Mach-O relocation may rewrite, and the arm64 instructions
a Mach-O linker optimization hint names. The name on a line is the
linkage name the library's symbol table spells, byte for byte:
`?Close@CFile@@UEAAXXZ`, `_ZNSt6thread4joinEv`, x86 `_memcpy`, Mach-O
`_deflate`. It is never demangled, sanitized, prefixed, or truncated. One
or more public names sit at offset 0 of each line. COFF function symbols at
the same section and offset retain their source-proven aliases together;
equal code at different addresses is not alias evidence. References to any
of a function's own aliases are recursion, not external constraints.

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

For incremental additions, `supplement_patterns.py` compares candidate patterns
with **unpruned** sigmaker outputs for every available library/version in the
same format and architecture, then with the existing packs. Supply the loader's
library stem for each input; repeat `--evidence` when several assets belong to
one stem. Keep full archive/file hashes and generation commands with the raw
inputs. For example:

```sh
python3 scripts/signatures/supplement_patterns.py \
  --candidate vs2022=candidates/vs2022.pat \
  --evidence vs2022=raw/vs2022.pat \
  --evidence vs2019=raw/vs2019.pat \
  --evidence winsdk=raw/winsdk.pat \
  --existing signatures/pe/x86/64 --output supplement/
```

The example's evidence list must be expanded to the full collected source set.
Supplying only retained `.pat` packs loses the counterexamples discarded during
their original build and can reintroduce known ambiguities. The supplement
retains source-proven aliases and reference distinctions, checks covering
patterns, skips claims already owned by an existing pack, and writes new
numbered parts plus input/output hashes. It never replaces an existing part.
Verify the output with `neverd-sigmaker --verify`, then evaluate ordinary
`sigs --auto --no-debug` recognition, candidate errors and performance before
updating the submodule. Source-corpus collision checks do not establish
universal uniqueness or replace held-out program evaluation.

To constrain existing records after collecting additional raw source collisions:

```sh
python3 scripts/signatures/refine_colliding_patterns.py \
  --existing signatures/pe/x86/64 --machine x64 \
  --evidence raw/templates-release.pat --evidence raw/templates-debug.pat \
  --output refined/ --report refinement.json
```

Supply unpruned `!evidence` records and a fresh output directory. The report must
be a new file outside that directory. Complete conflicting records move to
mandatory-reference or incoming-only native records with the same library stem;
incomplete conflicting records are omitted. Existing mandatory-reference claims
become incoming-only queries when a new source defeats their complete context. Existing
mandatory fields can be reduced only with an exact unpruned source witness and
a sufficient import/RTTI clause that also distinguishes loaded required rivals.
The report labels these changes `reduced`; call-only constraints remain intact.
Same-library cohorts with
identical typed references, incoming queries and metadata remain intact. The
tool never invents names or aliases. It writes pattern files only: update the
affected source manifests with the report and qualified output hashes before publication. Verify native
parsing, report lost/gained correct candidates as well as errors, and measure
the changed packs with the same consumer and input entries on both sides.
Proposed single-site clauses preserve alternatives and are audited against all
raw rivals and the existing byte cohort. An independently sufficient import/RTTI
clause may be retained, preferring fewer alternatives and the earliest source
field. Call-only conditions keep all original sites because the callee name may
itself be an ambiguous byte claim. Each old record still contributes at most
one global pattern, bounding byte-index work.
The whole loaded cohort is constrained together, preventing a newly unique
unqualified survivor. Separate complete incoming declarations
retain only outgoing fields shared across raw source versions; a pruned clause
does not supply omitted calls. Jointly necessary sites remain in a full clause.

For primary x64 COFF table context, generate separate unpruned declarations:

```sh
neverd-sigmaker runtime.lib --machine x64 --vtable-evidence -o runtime.tables.jsonl
python3 scripts/signatures/supplement_vtable_context.py \
  --candidate runtime.tables.jsonl --evidence runtime.tables.jsonl \
  --existing signatures/pe/x86/64/runtime.npat \
  --name '??_7Widget@@6B@' --output runtime.part2.npat
```

Repeat `--evidence` for every collected source version and `--existing` for all
packs in the intended selected profile. Incomplete declarations veto publication
of their type; raw weak bindings preserve both source names. The selector keeps
existing metadata, rejects conflicting layouts, requires an existing complete
strong queried body, and writes input/output hashes. `!vtable` records never
participate in function folding or metadata refinement. Verify, evaluate and
measure the supplement under the same identity and performance rules above.

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
signatures repository's `msvc-signatures.yml` does.

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

On COFF x86/x64, `--references` also retains import-pointer relocations in
unprefixed `FF 15` calls and `FF 25` jumps. The reference names `__imp_<export>`
at the four-byte address field (absolute on x86, RIP-relative on x64). The
consumer confirms an exact export spelling at the addressed, mapped IAT slot;
missing, ordinal-only, differently spelled or duplicate slot metadata remains
unknown. It never scans that data slot as code. Prefixed forms are unsupported,
including a prefix-looking byte immediately before the opcode when no decoded
boundary proves otherwise. This evidence can resolve candidates; it cannot
authorize a caller's automatic name or type.

For complete native outputs, `--data-references` can accompany `--contextual`,
`--caller-context` or `--collision-evidence`. It emits `&offset name` for
supported zero-addend x64 COFF external-data operands, including the exact
REL32_N bias of immediate instructions. Static/local symbols are excluded.
These fields require independently frozen full-function anchors and mapped
readable nonexecuting storage. They cannot become code references, bootstrap
other data candidates or authorize automatic names. The native parser rejects
mixed kinds at one field and ordinary records containing data references;
cache format 4 retains the kind without growing packed name records. See
[signature quality](../../docs/signature-quality.md) for evidence and collision
requirements. Existing ordinary generation is unchanged unless this option is
explicitly selected with a complete contextual mode.

Each line also names, as `^offset name`, the routines its function branches
to directly (`neverd-sigmaker --references`), in COFF and ELF objects alike:
the rel32 field of an x86 or x64 `call` or `jmp`, the ARM64 `B`/`BL` or
Thumb-2 `B.W`/`BL`/`BLX` instruction, or one byte past an ARM-state
`B`/`BL`/`BLX` instruction, an odd offset. Several references at one offset
name the routines that one branch may reach, any of which confirms it: the
maker states the alternate name an `/alternatename` directive of the inputs
gives a symbol beside the symbol itself. The matcher follows each branch in
the image. A target that the other matches name as none of those routines, or
in an ELF image the PLT stub of another import, drops the match; a target they
name as one of them, or that one's own pattern matches, confirms it; a target
nothing names, such as a COFF import thunk or a routine the program replaced,
decides nothing, and a routine that only jumps on -- a linker's thunk, or a
routine that tail-calls another -- confirms what it reaches but contradicts
nothing. Lines of one file that state the same bytes under different names are
therefore kept when, for every two of them, one offset holds references in
both and no routine the branch there may reach is one both name: at a match
only one of them can be confirmed. Bytes that two files name differently are
still dropped from both, because the Rich header loads only one of them, where
the line would have no rival to be told apart from.
Copies of one routine whose builds call different routines, such as the
debug CRT's `_free_dbg` where the release CRT calls `free`, name every
routine they call at a branch they all state, and state no reference at a
branch some copy states none for. A loader older than the references rejects
such lines, and one older than NeverSight/NeverD#214 reads the references at
one offset as separate branches.

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
the NeverD revision and release tag that produced it. A Homebrew asset also
records its formula and each bottle's tag and SHA-256, by which Homebrew's
registry serves the bottle.

### Source runtime pointer defaults

`neverd-sigmaker --runtime-pointer-evidence --evidence-symbol NAME` emits offline
JSONL configuration, pointer and actual function definitions for x64 COFF. Repeat
`--evidence-symbol` for every selected pointer and proposed default function, and
include all unpruned source versions as evidence. Undefined declarations do not
establish a function type; unsupported definitions remain incomplete counterclaims.

`supplement_runtime_pointers.py` joins these declarations and requires existing
complete caller bodies. `--pointer` selects source variables; `--updated-dir`
receives copied packs with proved outgoing references added to their existing
records. `--output` receives only missing `!loadcfg` metadata. Review those copies
and run `neverd-sigmaker --verify` before publication. Conflicting or incomplete
source declarations veto selection. Ordinary caller strength limits are unchanged.
Runtime matching uses loader-recorded immutable relocated slots and validates every
body wildcard against independently bound storage. These are candidates, not
source identity or a prediction of a pointer's value after the OS loads an image.


### Complete source code contributions

`neverd-sigmaker --code-layout-evidence --machine x64 LIBRARY -o source.jsonl`
collects complete non-COMDAT COFF code contributions and explicit refusals.
The source must own every function offset and canonical import/data relocation;
no target program or evaluation names participate. COMDAT fragments cannot
establish fixed relative positions across functions.

```sh
python3 scripts/signatures/supplement_code_layouts.py \
  --evidence source.jsonl --existing signatures/pe/x86/64 \
  --producer build-signatures/bin/neverd-sigmaker \
  --output library.npat --report layout-selection.json
```

The selector removes exact duplicates and preserves different nominal claims.
Native validation is mandatory and completes before output replacement. Retain
input archives, evidence, generator identity and the report hashes for review.
Qualify the selected tree on held-out images and measure matching, loading and
memory. Layouts use the name index, add no global byte-index records and retain
the independent automatic-name gate. See
[signature quality](../../docs/signature-quality.md) for the complete runtime
requirements and frozen-evidence policy.
