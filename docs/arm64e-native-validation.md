# Native arm64e validation

The [2026-11-09 native execution record](evidence/arm64e-native-installation-2026-11-09.json)
records twelve successful installed-image cases on each of the macOS 26 and
Xcode 27 runners, with zero failures or skips. It identifies the locally built
Release test host, exact source revision, binary/fixture hashes and each
case's PAC, compiler-original, migration and unwind properties. The execution
hosts compile their own replacement objects and controls. This qualifies the
documented dylib installation matrix. Later sections record default startup
and the canonical-IR producer separately. The
[generated-image record](evidence/arm64e-generated-image-2026-11-10.json)
qualifies the canonical source pipeline on both hosts; arbitrary lifted-image
rebuilding retains its separate ABI reconstruction boundary.

The same record includes independent Release/r6
[cold source builds on both runners](https://github.com/gmh5225/test_mac_intel/actions/runs/42641592355).
Each built NeverD with its own Apple compiler and passed all twelve native
installation cases without skips. Probe, build, runtime XML and test-binary
identities are recorded separately from the delivered test-host run. Both
controllers removed their owned build directories after saving diagnostics.

Run the compiler-original environment gate before building NeverD:

```sh
python3 scripts/verify_arm64e_native.py probe --output native-evidence/probe
```

The probe uses the same `.def` compiler inputs, flags and markers as
`MachOArm64eNativeObjectInstallation`. Both O0/O2 controls must produce nontrivial
IA/IB/DA/DB signatures, authenticate valid pointers and fault on corrupted
signatures. Each key first authenticates and reads a valid signed pointer in a
child process, then repeats that path with one changed signature bit. Both
hardware faults and Clang's checked-authentication `BRK`/`SIGTRAP` rejection
are supported; an externally killed child does not pass. Compact and DWARF
compiler-original dylib controls then exercise signed calls, authenticated data,
typed catches and propagated destructor cleanup through the same `dlopen`
loading contract as the installed image. The report identifies this loading
contract and hashes both the library and its arm64e loader.
Successful compilation or a process that runs with PAC disabled cannot pass.

`report.json` records the source revision and hashes, compiler/SDK, effective
boot arguments, SIP status, binary subtype and each process result. Exit 0 means
the complete original control passed; exit 2 means the environment is unavailable;
exit 1 means a failed check or tool operation. Both nonzero exits stop qualification.
A crash after the flushed probe entry marker fails. Each invocation needs a fresh
output directory; sources and diagnostics remain, while generated executables
are journaled before compilation and removed on success or failure.

On macOS 15.6.1, the matching
[XNU loader](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_exec.c)
explicitly refuses non-platform preview-ABI arm64e executables unless the
`-arm64e_preview_abi` boot argument is enabled. Its dyld policy also restricts
preview libraries. A dedicated test Mac must have the appropriate administrator
boot-policy setup and restart before running this profile. Read `kern.bootargs`
to inspect the effective boot arguments. These tools perform no NVRAM, SIP or
boot-policy writes and do not reboot a runner. Changing a Mach-O subtype cannot
establish a compatible ABI or enabled authentication.

After the probe passes, configure a Release build with `BUILD_TESTING=ON` and
build the native test owner through `scripts/build.py`:

```sh
python3 scripts/build.py build-release --target NeverDArm64eAnalysisTests --parallel 4
python3 scripts/verify_arm64e_native.py run \
  --build-dir build-release \
  --probe-report native-evidence/probe/report.json \
  --output native-evidence/installation
```

The runner requires the probe's exact current revision and source hashes. It
checks the twenty registered CTest commands before execution, records the owning
test binary hash and runs with the registered CTest deadlines. Every chained and
threaded-input O0/O2 case must execute successfully: authenticated calls and signed
compact unwind, native DWARF catches/cleanup, and relocated original signed
language frames. Missing, duplicate, foreign, failed or skipped cases fail the
qualification even if CTest itself returns success. Temporary fixture storage
is private to the invocation and is removed afterward.

The matrix additionally requires four `PublishedEntriesExecute` cases at O0/O2
with compact/DWARF language records. Four additional publication cases start
with original threaded images and migrate them to chained metadata inside the
publication transaction. These use the native publication API,
check failed-receipt rollback and exclusive output creation, and enter the new
authenticated call/catch/cleanup functions through the original addresses.
`native_object_publication_qualified` names this additional native object
contract. The matrix now requires twenty cases with no failures or skips.

Publication passes the admitted input CodeDirectory page size explicitly to
`codesign`. Host signing defaults can differ from the compiler linker's defaults;
the transaction keeps its exact page-size postcondition. The signer regression
uses real `codesign` to check both 4 KiB and 16 KiB signatures without executing
the fixture.

`native_installer_call_unwind_qualified` covers this native installation matrix.
General whole-image publication keeps its separate false qualification field;
neither this profile nor the controlled FVP call matrix establishes that broader
contract. Keep the configured LLVM r6 package until a separately qualified fork
release replaces it.

The [cold-build publication run](evidence/arm64e-native-publication-2026-11-10.json)
qualifies the native dylib transaction on both macOS 26 and Xcode 27: each host
passes 33 signer/signature tests, 16 installed-call/unwind cases and both sets of
six prerequisite controls, without failures or skips. The reports bind source
revision and prerequisite hashes and confirm removal of their owned build.
That run predates the four migrated-threaded publication cases and the separate
main executable gate; it does not establish those additions.

Native macOS 26 and Xcode 27 runners execute the four-key PAC controls, including
checked-authentication traps. The compiler-original call/unwind control passes
when loaded through `dlopen`. Its equivalent startup executable leaves static
B-key chained fixups unresolved on those runners and fails before unwinding;
this profile does not qualify startup loading or hide that failure behind a
retry. See the [native runner experiment](https://github.com/gmh5225/test_mac_intel/actions/runs/42626899000).

Run the separate startup prerequisite with the same compiler-original sources:

```sh
python3 scripts/verify_arm64e_native.py probe-startup --output startup-evidence
```

This command first requires the O0/O2 four-key PAC controls, then executes the
O0/O2 compact/DWARF controls as main executables. It records `control_loading`
as `exec` and sets `compiler_original_startup_qualified` only when all controls
pass. A failing executable never retries through `dlopen`. The installed-dylib
runner requires its matching `dlopen` probe, so a startup report cannot substitute
for it. Neither probe grants whole-image publication qualification; that also
requires a qualified producer and complete rebuild/signing transaction. The
canonical-IR path below uses native MC objects from a compatible producer; the raw MC rewrite callback
and arbitrary lifted modules retain separate requirements.

The [separate startup run](https://github.com/gmh5225/test_mac_intel/actions/runs/42646758724)
passed both PAC controls and all four `dlopen` controls on each native host.
Both startup probes failed with `SIGTRAP` at the first O0 compact
compiler-original executable. Those are failed startup prerequisites; neither
job reports success. The report and product manifests retain the failure and
confirm removal of the owned executables.

The [page-in linking comparison](evidence/arm64e-startup-pagein-2026-11-10.json)
localizes this failure to the system startup fixup path. Both hosts stop at the
DB-authenticated `native_external_data` access, where `AUTDB` is followed by
Clang's `BRK #0xc473` and the pointer retains chain encoding. Each exact signed
executable fails under default loading, passes with userspace dyld fixups, then
fails under default loading again. All four O0/O2 compact/DWARF controls pass
under that explicit profile, and the four-key valid/corrupt PAC controls pass
under both profiles. Binary hashes are checked after every invocation.

Use the qualified userspace startup profile explicitly:

```sh
python3 scripts/verify_arm64e_native.py probe-startup \
  --startup-fixups userspace --output startup-userspace-evidence
```

This profile derives `SYS_map_with_linking_np` from the execution host's SDK and
uses a process sandbox denying only that syscall. Apple's
[dyld syscall delegate](https://github.com/apple-oss-distributions/dyld/blob/fd8d0c4d52320ebf64db34f3cb280310d905c5ae/dyld/DyldDelegates.cpp)
checks that denial, and its
[process configuration](https://github.com/apple-oss-distributions/dyld/blob/fd8d0c4d52320ebf64db34f3cb280310d905c5ae/dyld/DyldProcessConfig.cpp)
selects userspace fixups. Every PAC and language control runs under the declared
launcher. The report records `fixup_loader`, the SDK syscall number, profile and
launcher; an unsuccessful or ambiguous SDK query fails before execution.
The [maintained verifier run](https://github.com/gmh5225/test_mac_intel/actions/runs/42659921245)
passed all six controls in each of the system/dlopen and userspace/exec profiles
on both hosts, with exact source/report identities in the page-in evidence.
The unchanged compiler-original `system` startup profile retains its observed failure. The installer still
requires its matching system/dlopen probe, and neither profile qualifies a
rebuilt image or arbitrary lifted-image publication.

The [compiler ABI comparison](evidence/arm64e-compiler-abi-2026-11-10.json)
records nine compiler-only observations on each current native host. Both Apple
Clang 21 toolchains produce the following headers from the same source:

| Compiler profile | Object subtype | Dylib subtype | Executable subtype |
| --- | --- | --- | --- |
| Default or explicit ABI0 | `0x80000002` | `0x80000002` | `0x81000002` |
| Explicit ABI1 | `0x81000002` | `0x80000002`, entry discarded | Link failure |

In the explicit ABI1 linked cases, the linker reports that it ignored the
object. A successful dylib link therefore does not establish retained code or
ABI compatibility. Reports bind the compiler, command traces, disassembly and
product hashes; all produced objects and images were removed. No product was
executed by that compiler comparison. The native installer now admits exactly
this ABI0-object/ABI1-macOS-main handoff when both platform commands name macOS.
Other subtype differences remain unsupported. This matches LLVM's
[Darwin linker subtype test](https://github.com/llvm/llvm-project/blob/b1dc6f680e37cbb19b2ee5ee4195635229c4f6c3/lld/test/MachO/arm64e-cpu-subtype.s);
the executable test matrix separately establishes execution, not the header alone.

Run the four executable publication cases with the successful same-source
userspace startup report:

```sh
python3 scripts/verify_arm64e_native.py run-executable \
  --build-dir build-release \
  --probe-report native-evidence/startup/report.json \
  --output native-evidence/executable
```

Main executable publication retains one `SG_READ_ONLY` segment. The native
installer declares new authenticated/ordinary pointer storage in its unowned,
zero-filled tail, preserving existing addresses and adjusting symbol section
ordinals. Existing dyld slots remain occupied even when their encoded words are
zero and their storage lies beyond the original sections. Insufficient capacity,
occupied bytes and `LC_NOTE` owners reject the transaction.
[XNU rejects a second read-only segment](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/mach_loader.c#L2499)
for main executables; the installer preserves RELRO rather than removing flags.

The verifier independently derives the SDK syscall restriction and compares it
to that report before launching CTest inside the declared profile. It rejects
missing, skipped or failed cases. The result is separately named
`native_executable_publication_qualified`; default startup uses the separate
`run-default-executable` gate described below. General lifted-image publication
retains its own unqualified boundary.

The [main-executable cold-build record](evidence/arm64e-native-executable-publication-2026-11-10.json)
binds source `3e5fb93c816902d4b9893c1a54ffb353e3e3f117` and the environment revision
to independent Release/r6 builds on macOS 26 and Xcode 27. Each host passed 33
signature/signer tests, all twenty dylib cases and all four executable cases,
without failures or skips. Both prerequisite profiles passed all six controls.
The reports verify source hashes, prerequisite-report hashes and test-binary
identity, and confirm removal of each owned build. This qualifies the native
object transaction under its declared profiles; system-default startup and
the post-r6 lifted-image producer remain separate requirements.

The same record separately identifies the later loader-slot fix at
`6eb5e0af5333e7419f402fa3704a6c2f523dcec9`. A legal zero-valued rebase beyond the
section inventory reproduced overlapping allocation before the fix; all four
offline executable cases pass afterward. Its 143-case local Mach-O regression
has 118 passes, 25 explicit environment/optional-interface skips and no failures.
The [delivered-host replay](https://github.com/gmh5225/test_mac_intel/actions/runs/42704012121)
passes all twenty dylib and four main cases, plus both prerequisite profiles,
on each native host without skips. Its separate source, binary and report hashes
retain the distinction from the earlier cold source build.

A supplemental linker diagnostic did not stop when `ld-classic` lookup failed
and therefore does not establish legacy threaded execution. Xcode 27 no longer
ships that linker. Produce the four original legacy inputs on a host with a real
`ld-classic`, then transfer the output directory to the execution host:

```sh
python3 scripts/verify_arm64e_native.py legacy --output legacy-fixtures
python3 scripts/verify_arm64e_native.py run \
  --build-dir build-release --probe-report native-evidence/probe/report.json \
  --legacy-fixtures legacy-fixtures --output native-evidence/installation
```

The producer fails on an unsuccessful linker lookup and records its hash, the
compiler version, exact command lines, signatures and image hashes. The consumer
requires the same source revision, source hashes and four exact arm64e binaries.
It passes only those explicit inputs through `NEVERD_ARM64E_LEGACY_FIXTURES`;
without that argument the tests continue to invoke the host linker. Replacement
objects and execution controls are always compiled by the execution host. No
Apple tool binaries are redistributed.

The full run also exposed two installer problems: moving unwind metadata left
section descriptors out of address order, and a Clang personality `POINTER_TO_GOT`
placeholder was decoded before relocation. Metadata placement now sorts section
descriptors while retaining symbol section identity. Object ingestion normalizes
the unresolved personality field only in its logical decoding view; final
relocation still requires the exact CIE field and installs the authenticated GOT
slot. Native qualification requires successful execution after these repairs.

The local `NeverDArm64eAnalysisTests` owner also exercises CPA2 source emission.
Its eighteen narrow aliased-index words run as separate CTest cases, each keeping
both C routes and O0/O2 execution. This avoids charging 72 compiler invocations
to one 120-second test deadline and identifies an individual failing word without
raising the deadline or reducing coverage.

The Xcode 27 runner in the [independent legacy comparison](https://github.com/gmh5225/test_mac_intel/actions/runs/42639296315)
binds an authenticated `data + 1` to `data` even in an unmodified library produced
by Apple's `ld-classic`. The same original and installed images retain the addend
on macOS 26. Native installation qualification therefore explicitly migrates
legacy inputs with `convertMachOThreadedFixupsToChains` before signing. This
qualifies legacy input migration, not unchanged threaded output on Xcode 27.
Offline cases retain and inspect the original threaded encoding.

The converter retains the ARM64E pointer-word format and every target, addend,
signing recipe, symbol and export. It replaces the old dyld-info command with
chained-fixup and, when needed, export-trie commands inside the existing header
space. Existing section VAs and file data remain fixed. Legacy non-weak-definition
import flags cannot be expressed by the modern import table and fail without
mutation. Already chained images are unchanged. Final code signing belongs to
the caller; conversion does not authorize general whole-image rebuilding.

Each native child now receives freshly truncated diagnostic output, preventing
an earlier child's success marker from satisfying a later run. Pair owned-image
checks also run as 215 individual words, retaining all range/provenance checks
and both source routes at O0/O2 for the original execution subset.

Default startup publication now prepares authenticated userland chains in the
shared signing transaction. B-key chains are repartitioned into valid 4 KiB
chain pages, preserving their targets, signing recipes, code, RELRO and the
original CodeDirectory page size. On 16 KiB hosts the default dyld fallback
applies all four keys; no sandbox launcher is used by this publication gate.
`run-default-executable` requires a successful same-source system `probe`,
then all four O0/O2 compact/DWARF executable publication cases. Each also runs
its unchanged original main before the replacement. The result is named
`native_default_executable_publication_qualified`, independently of the
compiler-original B-key startup failure and the userspace diagnostic profile.

The [default-startup comparison](evidence/arm64e-default-startup-2026-11-10.json)
records four O0/O2 compact/DWARF controls on each native host: the original
SIGTRAPs, the same code with 4 KiB chain pages succeeds, and the byte-identical
original SIGTRAPs again. Only chain page-size fields change before re-signing;
the original signature page size is retained. This isolates the format/loader
boundary from code generation and authentication semantics. The Xcode 27 host
reports `FEAT_CPA=0` and `FEAT_CPA2=0` on an Apple M1 virtual CPU. The separately
recorded FVP EL0/EL1 controls qualify that model environment, not native Apple
CPTA-on or new NeverD output.

The same record binds the [production publication replay](https://github.com/gmh5225/test_mac_intel/actions/runs/42746949630)
at `e5a0fddc68b2fd03c2f25445aca674cef32ab3e6`. Each host passes twenty dylib,
four userspace-profile main and four default-system main cases, with no failures
or skips. The default command starts CTest directly; its successful system PAC
and original-dylib report is separately hashed from the userspace prerequisite.
Both hosts use the same identified local Release/r6 test binary, which is
removed after execution. This is a delivered-host replay, not a cold source
build. The ordinary original main, authenticated replacement calls, RELRO,
compact/DWARF catches and cleanup all execute under the default loader.

```sh
python3 scripts/verify_arm64e_native.py run-default-executable \
  --build-dir build-release \
  --probe-report native-evidence/probe/report.json \
  --output native-evidence/default-executable
```


## Canonical IR through native MC objects

`installSourcePatch` admits canonical arm64e replacement IR for classic macOS
images and ABI1 macOS main executables. It verifies the exact arm64e target,
platform and layout, then emits a native Mach-O object with the
`Codegen::compile` backend. The native object installer owns final placement,
authenticated and ordinary pointer relocations, new providers, RELRO, signed
language frames and the publication transaction. The SDK preserves provider
and original binding symbols through this handoff instead of first replacing
them with ordinary pointer loads. Layout planning checks section and loader ownership, then reuses
available TEXT padding before requesting prefix growth, including creation of a missing
`__eh_frame` section in a dylib. Deferred authenticated function providers do
not reserve ordinary import-thunk frames.

Use a frontend that emits LLVM's standard
[pointer-authentication constants and call bundles](https://llvm.org/docs/PointerAuth.html),
such as the tested Clang 22 frontend, selected explicitly with `--compiler-path`.
Apple's older synthetic `llvm.ptrauth` globals remain unsupported IR input;
its native object artifact uses the separately qualified object installer.
Compiler selection never silently changes. The default prebuilt package remains
r6. Its explicit classic signed-return CFI is incompatible with the macOS 26
system unwinder, so publication rejects those objects transactionally. The
Apple-native object route remains available. Canonical generation requires the
[Darwin CFI correction](https://github.com/NeverSight/llvm-project/pull/8)
committed directly to the NeverSight LLVM fork; it does
not strip assembly or patch LLVM during the NeverD build. A separate source
build validates that correction without changing the prebuilt release selection.
The prebuilt audit checks the tag, release commit, all archive digests and checksum
sidecars against the r6 snapshot. The source gitlink may advance only when the
fork's authoritative comparison proves that snapshot is its ancestor; older,
divergent or unavailable ancestry fails the audit. The report prints both
identities. A source fix does not qualify the unchanged prebuilt package.
The native object route does not require the raw MC final-address callback.

The generated fixtures cover O0/O2, four authentication keys, address diversity,
local and new external data/code pointers, ordinary pointer addends, new writable
storage, three original-entry replacements, C++ catches, cleanup/resume and
transaction rollback. A compatible producer exercises both compact language
frames and explicit DWARF language frames. The latter forces a GNU_args_size
row, including creation of previously absent EH storage. A deliberate generic
RA-state toggle is a negative producer control: both the native installer and
SDK must refuse publication without changing the original image.
The offline checks compare native object/final unwind metadata and Apple's
`dwarfdump` output; only execution on a qualified host can certify runtime use.

The [producer diagnosis](https://github.com/gmh5225/test_mac_intel/actions/runs/42756197099)
retains an independent Apple-linker control: r6 and Clang 22 objects failed to
catch an exception on macOS 26 even without NeverD installation. Removing only
the generic RA-state directives in the diagnostic assembly restored the catch
and cleanup. This isolated the producer ABI defect; the diagnostic ablation is
not a production transformation. Xcode 27 accepting the original bytes does
not establish compatibility with the earlier deployment target.

The [fixed-fork direct-link controls](https://github.com/gmh5225/test_mac_intel/actions/runs/42761893719)
pass all four O0/O2 compact/DWARF call/catch/cleanup cases on each host, along
with all six compiler-original prerequisites. LLVM's 68 return-signing,
pointer-authentication and arm64e regressions also pass. These direct-link
controls establish the producer correction independently of NeverD installation.

The [cold source run](https://github.com/gmh5225/test_mac_intel/actions/runs/42762192123)
then builds NeverD `eabb96c44b7abaac2f628851968c05a090de5b5d` with the merged
LLVM fork `5676029e378c9e37adc16c8216a8e8453ffafcae` on both hosts. Each passes
all 36 native cases (20 dylib, four userspace-main, four default-main, four
generated-dylib and four generated-main), 33 signing tests and two groups of
six preflight controls, without failures or skips. The generated cases use
Clang 22.1.8 as the explicit IR frontend and NeverD's source-built LLVM backend.
Source, frontend, test-binary, preflight and runtime XML identities are bound in
the record; both controllers remove their owned builds. These runs qualify the
bounded canonical source pipeline, including real authenticated calls and C++
catch/cleanup through newly generated signed frames.

The [integrated replay](https://github.com/gmh5225/test_mac_intel/actions/runs/42772199612)
uses the local Release host built from `0d3511620291254bffcaa90a356870016f2741f4`
after merging dev and recording the corrected LLVM gitlink. Both hosts again
pass all 36 native cases and both six-control prerequisites, with no failures
or skips. The temporary delivery binds source-file, manifest, archive and
test-binary hashes; it is recorded separately from the cold builds.

A source build also exercises `MCProducerCapturesRecipesThroughFinalPlacement`
for IR and assembly producers with chained and threaded installers, plus
nested signing constants. This proves the bounded raw MC signing-recipe
handoff offline. r6 lacks that callback. Neither this handoff nor canonical
replacement IR supplies the missing ABI reconstruction for arbitrary
`neverd.arm64e-analysis` lifted modules.

Run the new matrices using a source build of the corrected fork and a successful
same-source system preflight:

```sh
python3 scripts/verify_arm64e_native.py run-generated \
  --build-dir build-arm64e-native --probe-report native-evidence/probe/report.json \
  --ir-compiler "$(brew --prefix llvm@22)/bin/clang++" \
  --output native-evidence/generated
python3 scripts/verify_arm64e_native.py run-generated-executable \
  --build-dir build-arm64e-native --probe-report native-evidence/probe/report.json \
  --ir-compiler "$(brew --prefix llvm@22)/bin/clang++" \
  --output native-evidence/generated-executable
```

Each gate requires its exact four unskipped cases, records frontend version/hash,
NeverD test-binary hash, source identities and preflight/XML hashes, and removes
owned temporary products. The executable gate uses the system loader without
a userspace launcher profile. Qualification flags are
`native_generated_ir_publication_qualified` and
`native_generated_executable_publication_qualified`; neither changes
`whole_image_publication_qualified` or `native_apple_cpta_qualified`.

`neverd.arm64e-analysis` lifted modules, foreign platforms, unqualified ABI
subtypes (including x1), shared-cache publication, TLS and runtime constructors
keep explicit refusal paths. Native CPTA-on additionally requires actual CPA/CPA2
hardware and a controlled enabling/readback contract. The system probe records
both feature sysctls independently of PAC execution; FVP results do not qualify
an Apple native execution environment.

## Lifted source entry reconstruction

The [entry ABI validation record](evidence/arm64e-entry-abi-2026-11-10.json)
records 886 passed and 46 skipped broader regression cases, followed by all
11 final entry tests passing. It distinguishes the two build/test phases,
identifies the final source and binary, and retains each unavailable check.

`MedLLVMEmitter::setReconstructAArch64SourceEntries(true)` projects a complete
fixed Darwin C declaration onto each emitted body's recovered carriers. The
shared ABI owner must reproduce exactly the declared register and stack layout;
an arbitrary observed-register list cannot become a native C signature. Missing
declarations, incompatible result banks, variadic entries and uncovered input
bytes fail before the entry is installed. Narrow register values retain their
declared extension; unobserved upper bytes require the existing MedIR demand
proof. Incoming stack slots require complete byte coverage.
Unused synthetic parameters can receive zero only when they have no parameter
attributes; a contract such as `noundef` still requires the declared carrier.
Non-scalar physical result fields fail before any entry is changed.

The public entry groups i128 values, ordinary records, HFA and HVA values and
binds incoming stack pieces by byte offset. An indirect input retains its
original pointer, and an indirect result uses an explicit `sret` parameter in
x8 without inventing `noalias` or making a second result object. The private
body keeps its physical parameter interface. Direct physical calls, recursion
and invokes retain that body; calls marked by the canonical source-call producer
and stored function pointers use the public entry. Interior block addresses
remain in the body, and pointer-authentication constants retain their signing
schema while changing the entry target. Function signing and unwind attributes
remain with the body and its generated entry.

The carrier rules follow [Apple's documented ARM64 ABI](https://developer.apple.com/documentation/xcode/writing-arm64-code-for-apple-platforms),
including odd-register i128 pairs and packed stack arguments. The owning
`AArch64EntryABI` tests include independent Clang O0/O2 callers and a decoded
machine-code-to-MedIR-to-entry case. These tests distinguish ordinary native
Darwin ABI execution from native arm64e signing and CPTA execution.
The HVA case also executes a MedIR-emitted three-vector result. Records return
reordered members, so an empty identity stub cannot satisfy the comparison.
Codegen's narrow-return widening preserves an explicit `signext` contract;
zero-extending every narrow result had changed signed byte returns.

Entry reconstruction does **not** remove `neverd.arm64e-analysis`. A declaration
does not prove the original image's prototype, physical-frame observations,
exception coverage, address ownership or publication closure. Arbitrary lifted
images still need those proofs, including variadic and non-C entries. The
previous canonical-IR installation qualification and pinned r6 package remain
unchanged; source-fork-only producer features still require the source build.

## Publishing position-independent lifted dylibs

The ordinary segment patch pipeline can publish a classic macOS arm64e image
whose instruction section consists of verified frameless bodies.
It matches lifted definitions to original entries, then checks the relocated
machine instructions and compact unwind coverage for every body. An unshifted
MOVZ with immediate zero and a move from the zero register can both clear the
same GPR. Both W and X forms write all 64 destination bits and preserve NZCV.
The original entries forward to the emitted RX addresses. The private native emission uses
the classic ABI version. The input IR retains its analysis marker.

`NeverDArm64eLiftedImageTests` compiles an unreserved Apple Clang dylib,
lifts it through the normal patch pipeline and compares the result against
the original image. A missing definition or a changed arithmetic result fails
before an output file is created. Set `NEVERD_ARM64E_LIFTED_NATIVE=1` on a
host that executes arm64e to run the independently compiled authenticated
caller against both images. The strict instruction check accepts
register-only integer arithmetic, logical operations, moves, and scalar
floating-point add/subtract ending in RET. Plain LDR and STR can use a live
base register and displacement. A typed caller compares loaded values and
checks both neighboring slots after stores. Literal or register-indexed
addressing, writeback and SP-based memory operands require separate proofs.

A frameless body can end with an unconditional direct tail branch to another
installed entry. The lifter preserves simple argument forwarding with the same
calling convention and prototype. The installer independently decodes the two
branch destinations at their actual PCs and matches the original callee to its
compiled owner. The regression reverses the two emitted functions and checks
the relocated branch with Capstone. Redirecting the tail call to a different
entry fails before publication. A lifted tail call may also re-enter a
loader-identified import stub at its published address. An image-relative
data declaration binds to the original data section. The verifier decodes
ADRP at both PCs and checks the destination register and original data page,
then checks the memory operation and displacement. The global/import fixture
changes an exported 64-bit variable between calls and compares its second
entry with the independent caller's own `getpid()`.

Unassigned incoming reads of X20 and X22 use LLVM's reserved physical
registers. These carry Swift self and the asynchronous context, respectively.
The lifter preserves the observed register even when the image omits its
source calling convention. Apple Clang builds the independent typed callers
and callees with `swiftcall` and `swiftasynccall`. The latter verifies the
context passed to a leaf before any suspension or continuation switch.
These register assignments are documented in the
[Swift ARM64 ABI](https://github.com/swiftlang/swift/blob/main/docs/ABI/CallingConventionSummary.rst#arm64).
The floating-point caller checks the exact result bits, including signed zero.

A Darwin Swift error result has both an incoming and an outgoing value in
X21. Analysis preserves each actual instruction write and the physical entry
read. For a standalone writer the private native entry takes a Swift
`swifterror` pointer, so instruction selection returns the new error in X21.
The ordinary result can simultaneously return the previous error in X0.
The compiler-typed tests exercise replacement and write-only clearing with
null and four live object addresses. Removing the X21 write while preserving
the ordinary return must prevent publication. Two adjacent clears of distinct
admitted registers may be scheduled in either order. Both leave flags, memory,
SP and the return address intact. Direct internal tail callers pass the same
`swifterror` operand through to the writer. Publication checks the emitted
branch against the relocated writer. A direct signed call with a continuation
uses a local Swift error-result slot. The caller copies the incoming X21 into
the slot, calls the relocated writer, then propagates the outgoing X21 while
returning the ordinary result in X0.
See the [Clang Swift error result attribute](https://clang.llvm.org/docs/AttributeReference.html#swift-error-result).

On macos-26 and xcode-27, each typed caller executed all 25 input/output
pairs on the compiler-original and NeverD-installed image. Both hosts also
executed the independent compiler-only O0 and O2 controls. The default r6
CLI published both fixture variants locally and their ad-hoc signatures
passed strict verification. The [native run](https://github.com/gmh5225/test_mac_intel/actions/runs/42984546707)
and [validation record](evidence/arm64e-swift-error-2026-11-11.json)
identify the exact images, compiler, source-built NeverD test host and
default r6 CLI results.

Frameless bodies may also preserve a matching entry PACIBSP and terminal
RETAB. SP and LR remain untouched between those instructions. The relocated
body must pass the instruction comparison and retain its frameless compact
unwind coverage. Removing the return-signing policy refuses publication.
For standard X29 compact frames, the publisher also checks the PACIBSP,
saved X29/X30 pair, X29 setup, pair restore and RETAB at their final PCs.
A direct BL in such a frame must still reach its original dyld stub or the
installed owner of its source callee. A compact-only input retains its compact
unwind mode. The optimizer and publisher share the same bounds check when
they turn accesses to the lifter's private stack into LLVM frame slots. The
backend can then remove an unobservable spill before creating its native
frame. A separate FDE for the same frame requires its own replay.
The Swift call fixtures use both Clang frame-pointer settings. One saves X29
and X30 in a 32-byte frame with compact unwind. The other saves LR on the
stack with an indexed DWARF record. Publication compares the latter's CIE,
CFA and register rules at their original instruction offsets. With
`-fasynchronous-unwind-tables`, both images restore LR at offset 16, just
before RETAB. The synchronous Clang record ends at the call-site row. The
negative control changes the saved-LR slot in the FDE, re-signs the input,
and requires publication to fail. Both the compact and indexed signed Swift
callers ran on macos-26 and xcode-27, including the asynchronous epilogue
control. The [native run](https://github.com/gmh5225/test_mac_intel/actions/runs/43029704710)
and [validation record](evidence/arm64e-swift-signed-call-2026-11-11.json)
include both image hashes for each case, the LR boundary and compiler identities.
The default r6 CLI refused all three signed non-tail Swift inputs. Its
backend emitted an explicit return-address state toggle. The compact-only input
lacked a registered DWARF section, and the indexed inputs had different CFI.
Each refusal preserved the input and left no output. The
[LLVM fork fix](https://github.com/NeverSight/llvm-project/commit/8b74c310a8140953b96df2464b8d091b46bd4fd8)
is in the source build used by both runners.
The framed import fixture calls
`getpid`, reads an exported variable after the call and returns their sum.
The caller changes that variable between invocations. Other stack layouts
and PC-dependent signing need their own instruction and unwind coverage.

The [native lifted-image run](https://github.com/gmh5225/test_mac_intel/actions/runs/42957645426)
executed the original and installed images on both macos-26 and xcode-27.
Each caller checked both entries for 25 ordered argument pairs. The
[evidence record](evidence/arm64e-lifted-leaves-2026-11-11.json) identifies the
source-bound Release host, local LLVM fork, original and published image hashes,
and the runner artifacts.

The two Darwin executable tests take the host's `/usr/bin/true` and
`/usr/bin/false` as inputs. Each test extracts its arm64e slice, removes the
platform signature from a temporary copy, then publishes it through the
ordinary patch pipeline. Apple codesign signs the baseline and published
copies explicitly. The tests verify both signatures and the new owner of
the main entry. With `NEVERD_ARM64E_LIFTED_NATIVE=1` they also require the
two original and two published processes to return their expected exit codes.

The [native executable run](https://github.com/gmh5225/test_mac_intel/actions/runs/42985655565) qualified both system-image copies on
macos-26 and xcode-27. On each runner, both baselines and both
installed copies returned their expected exit statuses. Each runner also
executed eight original/installed image pairs, verified a reordered direct tail caller,
recovered ten interrupted frames and their signed callers, and passed
40 native object installation cases. Those cases include authenticated
indirect calls, language exceptions, legacy threaded fixups and default
executable startup. The unmodified compiler-original B-key startup control
failed on both hosts before emitting a control marker. The separate diagnostic
runs recorded SIGTRAP on both and a SIGSEGV on one Xcode 27 repeat.
Their reports and fixup disassemblies accompany the installed-default-startup
results. Independent HighC and LLVM C projections were also
checked against all four pointer-signing keys and PACGA on each host. The default r6 CLI also installed both system
copies locally and verified the relocated entries and final signatures. The
[execution record](evidence/arm64e-darwin-executables-2026-11-11.json)
identifies the source-built Release hosts, the IR frontend and the image
hashes for each runner.

## Interrupted return-address signing

The exact-PC unwind query reports an unknown DWARF state when a classic
arm64e frame relies on Darwin's implicit signing ABI and supplies no rule
for the signing column. A compact prologue recipe can still be available.
Explicit DWARF rows retain their instruction-boundary signing state.
Mach-O metadata relocation also preserves the CIE signal-frame marker `S`
and its exact-PC interpretation alongside the signing rows. This accepts
register and offset rules that retain their meaning when the table moves.
Register-relative signal-context expressions can dereference a saved context
pointer and add a field offset. Absolute addresses still need relocation. The
fixture decodes the original and installed CFA and LR stack rules through LLVM
and checks both against the interrupted stack, including the saved slot.

`AArch64AsyncUnwind.PublishedFrameRecoversCallerAtFiveSignalInterruptions`
compares an original indexed-DWARF function with its installed native object.
The original carries explicit signing transitions and the replacement uses
Darwin-compatible implicit CFI. Both functions have identical instructions.
With `NEVERD_ARM64E_ASYNC_NATIVE=1`, a second thread interrupts the active
function before and after PACIBSP, with LR spilled, after restoring SP, and
after AUTIBSP. Before execution, the test host decodes CFA and LR locations
from that image's FDE. The handler selects the row by interrupted PC and
recovers the return address from the Darwin thread state's LR or the saved
stack slot. When the address carries the test's nonzero signature, the handler
authenticates it with the derived CFA. The caller preserves a signed parent
LR in its standard frame record. The handler uses the interrupted FP to
recover and authenticate this second return. Both addresses must equal
independent assembly return sites. The test also checks the original's
explicit signing rows and executes a changed companion entry in the installed
image. The two-frame hardware check covers this controlled call chain.

Both the [native publication and interruption run](https://github.com/gmh5225/test_mac_intel/actions/runs/42969858410)
and the [compiler-original frame controls](https://github.com/gmh5225/test_mac_intel/actions/runs/42967877615)
ran on macos-26 and xcode-27. Each runner executed all six lifted original
and installed image pairs. Five signals reached each of the original and
installed unwind bodies. All ten samples recovered the interrupted return
and authenticated the saved parent return. The
[evidence record](evidence/arm64e-interrupted-frames-2026-11-11.json)
includes the per-PC rows, caller checks, host and source revisions, and image
hashes. The two Apple M1 virtual runners replayed a locally built Release
host from the pinned LLVM fork.

The default r6 CLI lifted six fixture images, published 5 of them, and
verified the signed outputs and complete relocated entry bodies. For the
compact-only signed input, r6 emitted DWARF and reported that the original
contained no registered eh_frame section. The Release test suite also
exercised the arm64e slice of the system dyld and decoded 138,671 real
instruction words. The command-line symbolization regression separately
covers unused function addresses and scalar values in a zero-based dylib.

An independent [system cursor control](https://github.com/gmh5225/test_mac_intel/actions/runs/42967112065)
starts Darwin's public `unw_init_local` in the signal handler. It reaches
`_sigtramp` for all four compiler-original combinations of frame pointers
and signing CFI. Before PAC or after authentication it skips the interrupted
PC and reaches the caller. At the three signed positions it traps while
stepping from the trampoline. A general walk from that signal handler
remains unqualified on the two measured systems.

## Reconstructed entries through the native compiler

`scripts/verify_arm64e_entry.py` separately validates the entry reconstruction
IR with an explicitly selected native compiler. It uses the actual
`AArch64EntryABI.ExportSignedNativeInputs` producer, including the MedIR HVA
case, rather than a handwritten replacement IR fixture. A same-source system
PAC and compiler-original unwind preflight is mandatory for execution:

```sh
python3 scripts/verify_arm64e_native.py probe --output entry-evidence/probe
python3 scripts/verify_arm64e_entry.py \
  --test-binary build-release/bin/NeverDArm64eAnalysisTests \
  --compiler "$(xcrun --find clang++)" \
  --probe-report entry-evidence/probe/report.json \
  --output entry-evidence/native
```

The eight combinations cover O0/O2, IA/IB address-diverse pointer storage and
compact/forced-DWARF unwind. The independent caller checks the logical values,
C++ catch and destructor cleanup through scalar, i128, HVA128 and indirect
aggregate/x8 entries, including their normal values after catching. Every
generated exception entry and its private carrier body must retain signed
return sequences and separate calls; the
caller must contain authenticated branches. A valid signed pointer to the
reconstructed scalar entry executes first; flipping one observed signature bit
must then fault in a child process. Missing signing, skipped export tests,
stale preflight identities and nonzero execution outcomes fail qualification.
The exported fixture explicitly declares classic `ptrauth.abi-version`, as
Apple's frontend does; without it the native IR reader produces `arm64e.old`.
The verifier requires the exact classic object/dylib subtype and performs no
Mach-O header conversion. Its explicit indirect-value loads/stores avoid
version-specific `memcpy` intrinsic attributes in older native IR readers.
The fixture sets `frame-pointer=all` and the native frontend's `uwtable(sync)`
contract in the IR itself. Frontend flags alone do not establish the IR frame
policy, and asynchronous epilogue CFI forced DWARF in the nominal compact
profile. Final per-function encoding checks reject that mismatch; asynchronous
unwinding remains outside this synchronous C++ matrix. The verifier
also records and passes the selected macOS SDK, preserving the `clang++`
invocation name when it is a symlink to `clang`.

The DWARF profile keeps `__unwind_info` and forces per-function FDEs with
`DW_CFA_GNU_args_size`. The verifier checks the final indexed encoding for the
entries, carrier bodies, thrower and catching caller. Merely retaining `__eh_frame`
does not establish a usable Darwin lookup path: the compiler-only controls in
[run 42849216672](https://github.com/gmh5225/test_mac_intel/actions/runs/42849216672)
passed compact and indexed DWARF on both native hosts, but removing the index
with `-no_compact_unwind` caused an uncaught-exception abort on both. That
unsupported link profile is retained as diagnostic evidence, not counted as a
DWARF execution test.

`--compile-only` checks objects, signatures and disassembly without claiming
execution. Reports bind the source, exported IR, caller, selected compiler,
test host, loader and each object/dylib. Owned executable products are removed
on success and failure. `native_reconstructed_entry_qualified` refers only to
the selected compiler's execution of this fixed entry matrix. It does not
qualify NeverD's LLVM backend, original prototype recovery, arbitrary lifted
image publication or native CPTA-on; these remain separate report fields and
requirements. The r6 package selection does not change.

[Run 42859085649](https://github.com/gmh5225/test_mac_intel/actions/runs/42859085649)
passed all eight entry combinations and six prerequisite PAC/unwind controls
on each of macos-26 and xcode-27. The source-bound Release/r6 test host was
delivered to the runners; this was not a cold source build there. The local
12-entry, 31-script and eight offline checks also passed. The
[recorded evidence](evidence/arm64e-native-entry-2026-11-11.json) binds commit
`2201fbe2f1291e61ebca8856b3f726d314cdda21`, compiler/SDK identities, exports and
case hashes. Both hosts reported Apple M1 (Virtual); CPA/CPA2 remained absent
or zero, and no self-hosted runner was registered, so native CPTA-on remains
unqualified.

## Reconstructed entries through NeverD codegen and publication

Use `--producer neverd` with `scripts/verify_arm64e_entry.py` to export the
reconstructed entry objects through `Codegen::compile`. The independent Apple
compiler builds the caller and links the image. The exported objects must
already have the classic arm64e subtype and compatible signed-return CFI;
neither the verifier nor linker repairs those identities. In particular, r6
is rejected when it emits `DW_CFA_AARCH64_negate_ra_state`. Use a separate
source build of the pinned NeverSight LLVM fork for this qualification; the
default r6 selection stays unchanged.

Add `--publish` to exercise `bindSourcePatch` and `installSourcePatch` on the
same reconstructed fixed-C entry matrix. Each original dylib contains 26
public trapping entries and the independently compiled native caller/thrower.
Native validation first requires that original image to enter its caller and
then trap before the completion marker; a startup trap is insufficient. It
installs all 26 reconstructed entries, preserving the original caller and its
address-diverse signed entry pointers. The installed image must pass every
value, catch, cleanup, post-exception and corrupted-pointer check. This is a
complete published fixture image, not automatic admission of arbitrary lifted
modules.

The linker receives `-Wl,-headerpad,0` in all eight cases. The four compact
and two O2 indexed-DWARF cases use the default zero preferred base and fit in
existing TEXT storage. The two O0 DWARF cases set a nonzero preferred TEXT
address so NeverD can extend the front of the segment while preserving code,
data and authenticated slot VAs. The verifier reads each actual Mach header
base and resolves the compact index's relative offsets against the installed
base. With `--publish --zero-base`, the same eight IA/IB and compact/DWARF
cases require both preferred header addresses to remain zero. This profile
uses O2 for the original trapping entries and a fixed `@rpath` install name.
The separately compiled caller still runs at O0 and O2.
Add `--match-original-optimization` to compile both the caller and the
original entries at each optimization level. The original fully O0 reproducer
had 1,192 bytes of DWARF and 864 bytes of free TEXT with its fixed install
name. Adding the two independently dereferenced pointer slots reduced the
free TEXT to 440 bytes on the same compiler. The publisher can grow TEXT by
inserting aligned pages ahead of the original sections in a zero-base
arm64e dylib with image-relative
chained fixups. The preferred header stays at zero. The output symbols, stubs
and original data move together. The installation receipt records the slide
and preserves each original entry VA. If the existing RX segment cannot grow,
the installer can retry with a TEXT prefix. An occupied gap that prevents the
slide produces a relocation error before the output is written.
The verifier resolves each trampoline
from the signed output's own symbols. In the dense profile the caller also
reads an ordinary pointer and an address-diversified DA-signed pointer to a
local variable, writes through one and reads the result through the other.
Apple's `dyld_info` decodes both images so the verifier can compare each
original bind and rebase slot, its target and its signing recipe.
The installer measures LSDA index demand from the native compact entries
and FDEs. It can retain the compact table at its original VA, move
the adjacent DWARF records into audited free TEXT storage and reuse their
former bytes for the enlarged compact table. Each case also
fills the remaining TEXT tail of a zero-base input copy and requires refusal
before producing an output. A separate contract fixture covers a zero-base
image with exactly one free section descriptor and an aligned free range
between occupied bytes. DWARF-only growth also exercises prefix relocation
without a compact registry and replays the signed return-address rows.

The [zero-base native run](https://github.com/gmh5225/test_mac_intel/actions/runs/42989932233)
executed the eight zero-base images on macos-26 and xcode-27. On each host,
both O0 caller / indexed DWARF cases kept compact unwind at `0x3230` as it
grew from 168 to 1,096 bytes. The installer moved the 136 bytes of original
DWARF from `0x32d8` to `0x3678` and installed a 2,240-byte frame section.
Each installed caller exercised its 26 public entries and passed. The
[validation record](evidence/arm64e-zero-base-unwind-2026-11-11.json) includes
the source-built test host, per-image hashes, the fixed-name fully O0 refusal
and a zero-filled range referenced by `LC_NOTE` on that earlier revision.

The fully O0 IA and IB images on macos-26 and xcode-27 kept a preferred base
of zero. Their original sections moved by `0x4000`. All 26 installed entries
passed their independent caller checks, and `dyld_info` recovered the same 40
authenticated binds, two ordinary binds, one authenticated rebase and one
ordinary rebase in each image. Both runners also executed the lifted import and
global-access fixtures, the Swift X21 tail caller and the five asynchronous
interruption points on both original and published code. The
[native run](https://github.com/gmh5225/test_mac_intel/actions/runs/43020688049)
and [validation record](evidence/arm64e-dense-publication-2026-11-11.json)
identify the source revision, compilers and per-host results.
The default r6 CLI published the global and Swift tail cases and installed
an independent 151-function native object into the dense image. For the
framed `getpid` input, r6 generated indexed DWARF while the original
registered only compact unwind. The CLI refused the image before writing
an output. Both native runners used the pinned LLVM source fork for this call.

The publication test also checks rejection before the receipt commits,
unchanged input bytes, exact installed entry receipts and refusal to overwrite
an existing output. Analysis-marked modules remain rejected. The verifier
uses the installed function addresses from the receipt to check both signed
frames and final compact/DWARF lookup encodings. Installed code occupies an RX
segment without a section descriptor, so ordinary object disassembly only
shows the original sections. The verifier extracts the loader-validated final
text range, hashes it, and places those exact bytes in a temporary
inspection-only object for the independent disassembler. That object is never
executed; the signed published image remains the runtime input. Inspecting the
original symbol table would otherwise check the old code. All 26 public identities and
all exceptional carrier frames must be present.

The matrix remains eight combinations per route: caller O0/O2, IA/IB stored
entry pointers and compact/indexed-DWARF frames. NeverD's current
`Codegen::compile` uses `CodeGenOptLevel::Less`; the caller optimization axis
must not be reported as two NeverD optimization levels. Reports record that
scope, the producer, the original trap status, installed addresses and image
hashes. `--compile-only` exercises emission and publication checks without
claiming native execution. Executable products are removed on success and
failure, including partially exported backend objects.

`neverd_codegen_qualified` requires native execution with the NeverD producer.
`reconstructed_entry_publication_qualified` additionally requires the complete
publication matrix. These fields concern the declared fixed-C fixture. They
do not set `whole_image_publication_qualified` or `native_apple_cpta_qualified`:
prototype recovery, asynchronous interruption through prologues/epilogues,
variadic/non-C entries and general lifted-image admission still require their
own implementation and native evidence.

The [native backend/publication run](https://github.com/gmh5225/test_mac_intel/actions/runs/42877451388)
passed on both macos-26 and xcode-27. Each host executed eight direct backend
cases and eight installed-image cases, with six independent compiler-original
PAC/unwind controls. All eight original images per host reached the caller
marker and trapped; all eight unreserved layouts were refused by that revision.
The [evidence record](evidence/arm64e-backend-publication-2026-11-11.json)
binds source `3f5513c8e936cc057326213abfe976748c98ee50`, the source LLVM fork
`5676029e378c9e37adc16c8216a8e8453ffafcae`, the local Release host and both
runner reports. This was source-bound replay of a locally built host, not a
cold runner build. The fork checkout was clean and prebuilt mode was disabled
only in that separate build. The runners still reported no CPA/CPA2 feature;
native Apple CPTA-on remains unqualified.

The [compact layout run](https://github.com/gmh5225/test_mac_intel/actions/runs/42946894626)
executed the eight direct cases and eight installed images on both macos-26
and xcode-27. Per host, all eight original callers reached their trap and all
eight occupied inputs failed the TEXT capacity check. Each installed image ran
the native caller against its 26 replaced public entries. Six images per host
linked with zero header reservation. The
[run record](evidence/arm64e-compact-layout-2026-11-11.json) binds the source
revision, Release host, LLVM fork, GitHub artifacts and signed image hashes.
Both runners reported Apple M1 (Virtual).

The [header relocation run](https://github.com/gmh5225/test_mac_intel/actions/runs/42951431144)
and the [compact storage replay](https://github.com/gmh5225/test_mac_intel/actions/runs/42954296548)
executed all eight published images with zero linker header reservation on
each Apple runner. In both O0 indexed-DWARF cases the preferred header moved
from `0x100000000` to `0xffffc000`. The six default-base images retained
their zero preferred address. Each host also passed eight original-caller
trap controls, eight occupied-layout refusals and six compiler controls. The
[evidence record](evidence/arm64e-header-relocation-2026-11-11.json)
identifies the Release host, both artifacts and their measured image bases.

### Darwin variadic entry stacks

For a recovered Darwin arm64 variadic entry without materialized overflow
parameters, the LLVM emitter takes the anonymous argument cursor from
`llvm.sponentry`. Locals and recovered named argument homes keep their own
storage. LLVM IR, LLVM-to-C and structured HighC retain the callee boundary
when the cursor depends on its incoming SP. HighC keeps all ABI positions
before a used stack argument, including unused registers and alignment slots.
Stack-parameter recovery uses the target's entry alignment proof for masked
pointers, including the upper word
of a 16-byte-aligned argument after an 8-byte argument. An OR in a cursor
walk can set only known low bits of its entry-SP displacement. In particular,
setting an already-set bit in a negative local offset keeps the cursor local.

`AArch64DarwinVariadic` compiles O0 and O2 Apple arm64e inputs and calls the
lifted LLVM IR, LLVM-to-C and structured HighC from a separately compiled
variadic caller. It covers a variable count, a named stack prefix, a copied cursor,
double and a 128-bit value. The IR control enables stack protection to displace
the private frame. Setting `NEVERD_ARM64E_LIFTED_NATIVE=1` runs the compiler
original and all three replacements as arm64e.

An exported variadic entry can be called by another image even when its only
local callers use direct branches. The export keeps its open ABI. An optimized
sentinel loop can read the first stack word directly and store an advanced
cursor made with a pointer OR. The recognizer and both projections use the
entry alignment to prove that cursor's displacement. The caller control calls
the same entry through dyld with different tail lengths and through a function
inside the image.

The image publisher compares non-writeback SP accesses at their effective
incoming byte offsets. It can remove a compact leaf's unused stack reservation
when the exact SP adjustment and restoration agree with its unwind row, the
surviving loads retain their widths and destinations, and every arithmetic
instruction and the return agree. A pre-indexed SP load can restore the entire
frame while reading the first anonymous argument. The verifier tracks the
resulting SP for later loads and requires restoration by return. Tests change
the incoming slot or switch the compiler's pre-indexed load to post-indexing;
the latter restores the same SP but reads the callee's private frame. Both
inputs must be refused. Register-walk bodies with other instruction
changes, frame-local memory effects and additional DWARF rows need their own
publication proof.

The two Apple runners each executed 14 native images across five variadic
tests with prebuilt r6. The signed installation and default executable checks
also passed on each host. The pinned LLVM fork passed all 24 lifted-image
cases on each runner, including 50 ordinary image executions, 10 interrupted
frames and 10 authenticated parent returns. The
[verification record](evidence/arm64e-darwin-variadic-2026-11-12.json)
identifies the tested sources, host binaries and Actions artifacts.

The nine-function mixed variadic fixture exercises a custom caller that reads
X8 and X9 after return. The earlier O0 `leaf_sum` preserved the C result
109 while changing X8 from 100 to entry SP + 8. The earlier O2
`leaf_difference` returned 77 while changing X9 from 23 to 85.

For these stack argument entries and their local callers, patch-mode lifting
retains the source register operands in an LLVM native body. Immediates and
local function targets are explicit LLVM operands. MC assembles each body
at its new address, including local branches and unwind directives. Before
writing the output, the installer checks the final machine instructions, call
destinations and compact or indexed DWARF frame rows. The O0 and O2 tests
require all nine relocated owners and reject changes to X9, a stack
displacement or the called function. The native observer compares X0, X8,
X9, D0 and SP immediately after each call across three input vectors.

`python3 scripts/verify_arm64e_native.py probe-cpta --output <directory>`
builds thin arm64, arm64e and arm64e.x1 controls. Each executable records
the results of ADDPT on a non-overflowing pointer, a top-byte carry, a borrow
and an existing poison bit. The report identifies the exact executed slice,
an unavailable instruction or slice, and effective checked EL0 arithmetic.
The privileged EL1 controls have a separate verification record.

A local direct caller can also establish a non-variadic X8/X9 result contract.
The register reader checks physical reads and writes after the call, including
W-register definitions. The callee and its supported callers use the same
native lowering. Local tail forwarders retain the complete register result,
and the installer resolves their B targets against the published entry map.
The custom-caller test consumes both results and checks a
matrix of ordinary, wrapping and high-bit scalar values.

`DarwinArm64eSignalCursor.h` provides a signal-context cursor for classic
arm64e frames. A caller supplies the exact-PC CFA and saved-register row and
the interrupted thread's mapped stack bounds. The cursor verifies an IA- or
IB-key signature against the recovered CFA before returning the canonical PC.
Unknown signing state, a mismatched signature and an out-of-range saved slot
leave the cursor unchanged. The native interruption test reaches the system
`_sigtramp`, recovers the interrupted return and its signed parent, and
cross-checks the parent through Darwin's frame-pointer backtrace.

In the original qualification, the installed implicit-signing fixture used the
signing rows of the explicitly described original after checking identical
machine code. The [system signal qualification below](#system-signal-trampoline-and-implicit-signing-rows)
recovers the installed rows independently and records the behavior of an
unmodified system `unw_step` at `_sigtramp`.

Release verification at `7bf3d8571ab87cb8de5f20145e786621d3dc4659` uses the
tracked Capstone and Unicorn revisions and LLVM fork
`5676029e378c9e37adc16c8216a8e8453ffafcae`. The fork passes all 28 lifted-image
cases locally and on both Apple runners. Each runner executes 58 ordinary
images, compares 54 register input cases across the O0/O2 nine-function
fixtures, and completes 10 interrupted-frame returns, 10 authenticated parent
returns and 10 signal-context handoffs. Prebuilt r6 passes its 23 locally
supported cases on both runners, with 48 ordinary image executions per host.
The five r6 failures match the recorded baseline. Both local configurations
pass all 87 shared Pipeline and variadic regressions. The public `neverd patch`
entry point publishes both nine-function images and their signatures verify.
The verification record contains source and host hashes, dependency revisions,
native image properties and runner artifacts.
[Verification record](evidence/arm64e-register-results-and-signal-2026-11-12.json)

The native ADDPT controls report an undefined instruction in both arm64 and
arm64e processes on both M1 runners. Xcode 27 emits an arm64e.x1 slice whose
launch fails with `EBADARCH`. The local M4 control also reports an undefined
ADDPT instruction. Native CPTA-on remains unavailable on these environments.
That qualification covers unsigned and IA/IB-with-CFA signal rows. The
instruction-address recipes added below have separate hardware qualification.
At that revision, continuing the system cursor beyond `_sigtramp` had not
been demonstrated. The [later bridge qualification](#system-signal-trampoline-and-implicit-signing-rows)
covers the caller handoff. Additional image instruction and unwind profiles
still require qualification.

## Instruction-address signal recipes and remembered frames

`DarwinArm64eSignalCursor.h` now accepts IA/IB signatures diversified by CFA
and the resolved signing instruction address. Explicit PAuth_LR and PACM
remain distinct recipes. `SigningPC = 0` is a valid modifier, not an unknown
state. The caller resolves the row before entering a signal handler and
supplies the interrupted thread's keys and enable controls. The existing
`ndDarwinArm64eStepFrame` entry point refuses explicit PAuth_LR;
`ndDarwinArm64eStepFrameWithPAuthLR` accepts it only when the caller has
established instruction availability. Both compare a fresh hardware signature,
so malformed PACs do not execute a potentially trapping authentication.
Every refusal leaves the cursor unchanged.

The runtime signers reuse the instruction contracts in
`AArch64PAuthLR.def` and `AArch64PAuthM.def`. The independent native oracle
uses SP/PC signing instructions, including the actual address of the signing
instruction; the cursor instead materializes the two modifiers in registers.
It exercises both keys, saved and live returns, SP/FP CFA bases, bounds and
overflow, wrong PACs, wrong signing recipes and unavailable explicit
instructions. C11 and C++20 compile at O0/O2 in the lifted-image test suite.
Run the standalone C11 qualification with:

```bash
python3 scripts/verify_arm64e_native.py probe-cursor --output cursor-evidence
```

The report separates `native_cursor_qualified`, `pacm_pc_qualified` and
`explicit_pauth_lr_qualified`. PACM hints can execute on old CPUs while
ignoring the PC modifier, which cannot qualify PC-sensitive recovery.
Both hosted Apple profiles pass 1,058 baseline checks at each optimization
level, including signatures whose signing-address modifier is zero. Both report
no PC sensitivity and unavailable explicit PAuth_LR.
[Native cursor and image run](https://github.com/gmh5225/test_mac_intel/actions/runs/43176528918)

Native register-result image publication also retains FP/SP CFA changes,
remembered states across multiple epilogues, saved/same-value register rules,
GNU argument-size rows and the explicit B-key CIE augmentation. Final linked
CFI is compared against the original at every instruction boundary. Generic
RA toggles remain rejected by the existing classic-Darwin compatibility
boundary; this extension does not authorize them.

Release verification at `06d091c34b8fa1fa84016c528982aa55452ba874` passes all
30 lifted-image tests with the tracked LLVM fork, locally and on both Apple
runners. Each runner executes 60 ordinary images, compares 54 custom-register
input cases, completes 10 interrupted-frame and authenticated-parent returns,
and runs four C/C++ cursor profiles. The new B-key multiple-epilogue image
executes both the original and published bodies and rejects a changed LR spill
rule before publication. The shared return-protection and PAuth suites pass
51 cases; three native capability checks remain explicitly unavailable on the
local host. All 190 script regressions pass.

Prebuilt r6 remains unchanged. Its 25 supported lifted-image tests pass locally
and on both runners, with 50 ordinary image executions per runner. Its five
failures match the earlier recorded LLVM r6 baseline; the native r6 replay
explicitly excludes those cases. The
[verification record](evidence/arm64e-signing-recipes-and-remembered-frames-2026-11-12.json)
binds source, dependencies, host binaries, native reports and exclusions.
At that revision the installed rows still depended on the original. The
following qualification supersedes that part of the earlier result.

### System signal trampoline and implicit signing rows

Resolve a Darwin frame with `recoverDarwinAArch64Unwind` outside the signal
handler. For an implicit FDE, its CFA and register columns select the actual
live LR or saved stack slot. For a compact-only frame the analysis instead
tracks the incoming SP, register values and active frame saves. It follows
reachable direct branches, joins agreeing facts, and accounts for call
clobbers, stack writeback, classic IA/IB, PACM and explicit PAuth_LR. A failed
proof has no unsigned default. Opaque control flow, alternate entries,
unrecognized personalities and exceeded budgets cannot authorize a native
handoff.

Select the row at the interrupted instruction. `prepareDarwinArm64eSystemCaller`
binds its signing PC to the loaded address and checks that the caller's integer
register locations can be represented. With the system cursor positioned at
the real signal trampoline, `ndDarwinArm64eCrossSignalTrampoline` consumes the
matching kernel `ucontext_t` and mapped stack bounds. It verifies the return
signature, restores SP, FP and X19–X28, and positions that same system cursor
at the caller. Invalid or duplicate saves fail before changing the cursor.
Discard the cursor on a system API failure or if the return PC selects a
different function's unwind record.

For an ordinary call return through the live Darwin signal trampoline, pass
the same prepared frame, integer saves and `Plan.Vectors` to
`ndDarwinArm64eResumeSignalCaller`. After success the handler supplies the
call's result, if required, and returns normally. XNU consumes its original
signal frame, restores the signal mask and alternate-stack state, and resumes
at the authenticated call fallthrough. This operation returns from the
interrupted call directly. A language exception that must run cleanups still
requires its personality and landing pad. See the
[XNU signal return implementation](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/dev/arm/unix_signal.c)
and [thread-state pointer accessors](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/mach/arm/_structs.h).

The resume plan requires all eight ABI-preserved D8–D15 lanes. Recovery
tracks both halves of a Q spill, the low half of a D spill and overlapping
writes at instruction boundaries. A DWARF SIMD register column supplies its
low 64 bits. Upper halves with a proven save are restored from it. Other
upper halves and volatile vectors retain their interrupted bits, as do FPCR
and FPSR. An incomplete low lane, an out-of-bounds saved lane or a rejected
return signature leaves the signal context untouched. The register contracts
are specified in [AAPCS64](https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst)
and [AADWARF64](https://github.com/ARM-software/abi-aa/blob/main/aadwarf64/aadwarf64.rst).

The public frame query exposes a separate `instruction_state_at_pc` where
applicable. It leaves `dwarf_state_at_pc` and `compact_recipe` unchanged. The
GUI presents the instruction result separately. Resolve once and reuse the
row table when sampling; the public exact-PC query performs bounded analysis.

The [compiler comparison](https://github.com/gmh5225/test_mac_intel/actions/runs/43209395729)
produced identical instructions and compact encodings on the two hosts.
Apple's newer linker put the last lookup sentinel at `0x305`, one byte past
the end of `__text` at `0x304`; the older linker used `0x304`. The loader
retains the raw sentinel for rewriting and bounds the decoded instruction
range at the proven end of the code section. Interior entries and partial
instructions inside a code section are not rounded down.

Two independent [system controls](https://github.com/gmh5225/test_mac_intel/actions/runs/43196724884)
found the same behavior on both hosted Apple profiles: unmodified system
stepping at the trampoline skipped the interrupted PC for unsigned LR and
raised SIGTRAP at all three signed stages. Seeding the proved caller let the
system walk to the correct grandparent in all five stages of both frame
profiles. The [in-place bridge test](https://github.com/gmh5225/test_mac_intel/actions/runs/43202351334)
also checked a clobbered X19 restored from the frame. This is observed
behavior on those Darwin runtimes; it is not a guarantee that a future system
libunwind will permit setting IP across functions.

Native register-result publication now emits local TBZ/TBNZ targets as labels,
keeps the tested bit as an LLVM operand, and preserves indexed memory operands
including LSL, UXTW, SXTW and SXTX. Final linked bytes still have to match the
source at each instruction. For indexed SP accesses the generic LLVM emitter
declares the function with its recovered prototype. Before exposing the module,
the patch pipeline requires its verified, complete native register body: its
machine instructions use the physical SP. A partial body, an unsupported
operation or a changed tested bit prevents publication. This does not qualify
an ordinary LLVM function using a synthetic stack. No LLVM overlay or new
prebuilt package is needed.

At `a6ffeb9bd9e3a9799c56d60418ed05abf8b01a1b`, the
[source LLVM replay](https://github.com/gmh5225/test_mac_intel/actions/runs/43210663619)
passed all 31 tests on each runner. Each executed 60 original or published
images, continued the system cursor at all 10 interruptions across the two
images, and made three native calls through the compiler-generated compact
fixture. The nine-function variadic images each compared 27 register inputs
at O0 and again at O2. Neither runner qualified explicit PAuth_LR,
PC-sensitive PACM or native CPTA-on.

The call-ABI suite also lifts Apple-linked arm64 and arm64e copies of a C
variadic wrapper. Its context and format are fixed parameters. Apple Clang
passes the third actual argument on the stack. The regression parses NeverD's
LLVM output and checks the types at the definition and the call, including
the single variadic operand. All 13 tests in the call-ABI suite passed.

The unchanged [r6 replay](https://github.com/gmh5225/test_mac_intel/actions/runs/43215695747)
passed its 26 supported tests on each runner, with 50 image executions, the
same 10 system-caller continuations and three compact-fixture calls. The
local unfiltered run executed all 31 tests and reproduced exactly the five
previous r6 failures; the compatibility replay explicitly excludes those
five. No LLVM patch, fork revision change or replacement prebuilt is used.
The [verification record](evidence/arm64e-system-signal-and-implicit-unwind-2026-11-12.json)
includes the per-runner signal samples, test-host hashes, local suite results
and hardware capabilities.

The [integrated source replay](https://github.com/gmh5225/test_mac_intel/actions/runs/43271432365) repeated the 31-test native suite on both Apple profiles after merging `dev` at
`cee06981844f5597004da9be6bcc778db813d6b2`. Each profile again recorded
60 image executions and 10 system-caller continuations. A separate Apple
Clang fixture with a typed C++ catch has a compact frame and LSDA. Its
implicit signing rule was unavailable at entry and at the next two
instructions. The verification record includes the source and all three
query results for that revision.

For Darwin C++ frames using `__gxx_personality_v0`, instruction recovery
follows the decoded LSDA call-site ranges to their catch and cleanup landing
pads. The handler receives only surviving callee-saved registers and stack
slots. For a protected non-call instruction the analysis intersects the
states on both sides. A call to a known non-returning function can still
reach its handler. An unaligned, overlapping or out-of-frame range, a missing
LSDA or a different personality prevents this proof. At a protected PC with
a DWARF outgoing-argument size, the exceptional edge advances SP and drops
the released stack slots. Ordinary return retains the argument area. See the
[Itanium exception ABI](https://itanium-cxx-abi.github.io/cxx-abi/abi-eh.html#base-personality)
and [libunwind context installation](https://github.com/llvm/llvm-project/blob/main/libunwind/src/libunwind.cpp#L205-L212).

Native Mach-O object publication can reuse a dyld-bound RTTI slot in a
non-lazy pointer section sealed after fixups. The import table must select
one provider and weak-import policy, and the binding must have zero addend.
An ordinary RTTI reference and an authenticated personality reference keep
their respective pointer encodings. A writable source slot requires separate
GOT storage for the published reference.

Apple Clang can leave a synchronous DWARF row in force through part of the
prologue or epilogue. At an interrupt where its CFA cannot be proved to equal
the incoming SP, recovery derives the CFA and integer register locations from
the reachable instructions. Registers the producer marked undefined stay
undefined.

The [signed C++ execution run](https://github.com/gmh5225/test_mac_intel/actions/runs/43290127091) passed all 32
publication tests on each Apple M1 runner. Each recorded 34 successful C++ outcomes and six system
PAC traps across five compiler profiles, exercising the original and redirected
entry. The four conventional profiles returned the expected values, propagated
unmatched exceptions and ran cleanup for a new exception thrown inside a catch.
With a nonzero DWARF outgoing-argument size, both untouched and published
arm64e code returned normally on the nonthrowing path. On the throwing paths
the system unwinder trapped before installing the handler. The
[debugger control](https://github.com/gmh5225/test_mac_intel/actions/runs/43285706120) located the same authenticated-pointer
`BRK #0xc471` in system libunwind on both hosts. Ordinary arm64 completed
the corresponding controls. The system signal suite also crossed ten interrupted
frames on each runner. The [verification record](evidence/arm64e-system-signal-and-implicit-unwind-2026-11-12.json)
records the source revision, host digest, per-runner observations and local
regressions.

Darwin Objective-C uses an address-based LSDA for typed catches and
`@finally` cleanup. The signing analysis follows those edges, uses the
instruction's access width to identify overlapping stack saves, and stops at
compiler-generated terminating traps. Apple's Objective-C runtime delegates
selection of these landing pads to the C++ personality after running its
alternative handlers. [Apple Objective-C exception runtime](https://github.com/apple-oss-distributions/objc4/blob/main/runtime/objc-exception.mm)

An Objective-C replacement with the same image-info version and flags reuses
the original runtime declaration. Foundation supplies the type descriptor for
`NSException`. For an object that contributes its own weak fallback, such as a
typed `NSString` catch, the publisher can use the existing sealed GOT slot
with dyld's weak lookup ordinal. This requires an exported data fallback, a
separate weak atom and indirect references through that slot. Conflicting
image flags, additional live atoms in the same storage and direct references
to a discarded fallback cause publication to fail.

Each Apple M1 runner passed all 33 publication tests in the
[Objective-C execution run](https://github.com/gmh5225/test_mac_intel/actions/runs/43294345635). On each runner, the original and redirected
entries produced 32 expected results across both catch types at O0 and O2.
The driver checked return values and the type and value of each propagated
exception. It also verified one cleanup on every path. Both runners repeated
ten system signal-frame crossings. The
[verification record](evidence/arm64e-system-signal-and-implicit-unwind-2026-11-12.json)
includes the compiler versions, test-host digest and hardware probe results.

The native CPTA probe signs the x1 slice with the Enhanced Security and
checked-pointer entitlements, verifies the signature and reads back the
embedded plist before launch. Its report records the final binary digest,
the signed entitlement values and the scalar ADDPT, SUBPT, MADDPT and MSUBPT
results. Equivalent signed offsets and unit products exercise tag carry, borrow
and already-poisoned pointers across all four forms. A second invocation
executes post-indexed LD1 and ST1 on a mapped byte. It verifies the data and
the updated base for an in-range offset and for a carry into the top byte.
This ordinary-memory control also runs on older CPUs when ADDPT is undefined.
A passing x1 run requires both explicit checked arithmetic and checked
load/store writeback. [Arm LD1 operation](https://support.arm.com/documentation/111108/2026-03/SIMD-FP-Instructions/LD1--multiple-structures---Load-multiple-single-element-structures-to-one--two--three--or-four-registers-).
Apple documents the required [Enhanced Security entitlement](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.security.hardened-process.checked-allocations.enforce-checked-pointer-arithmetic-overflow)
and [supported x1 devices](https://developer.apple.com/documentation/security/preparing-your-app-to-work-with-pointer-authentication).

For an external thread-local variable, the arm64e object publisher resolves
TLVP page relocations to the original strong import's sealed descriptor
slot. The compiler's authenticated descriptor call still selects storage for
the current thread. This path requires an existing provider and accepts the
ordinary ADRP and pointer-sized LDR forms. For a strong per-thread export
in the original image, the publisher checks the descriptor's section and
authenticated bootstrap, then relaxes the load to an ADD at the relocated
descriptor address. Both paths reuse the original per-thread storage.
The native object path below also registers storage contributed by a replacement.

Both Apple M1 runners passed all 35 publication cases in the
[native signal and TLS run](https://github.com/gmh5225/test_mac_intel/actions/runs/43366275284). Each runner completed eight signal
returns through original and redirected entries on both stack types. Across
the two TLS layouts it performed 32 accesses at O0 and O2, checking both
threads' independent values. The [verification record](evidence/arm64e-signal-resume-and-tlv-2026-11-13.json)
includes the code and binary digests, exception results and CPTA observations.

To repeat the interrupted-frame test on a Mac that can launch arm64e
executables, build the test host against the tracked LLVM fork and require
native execution:

```bash
python3 scripts/build.py build-release --target NeverDArm64eLiftedImageTests
NEVERD_ARM64E_ASYNC_NATIVE=1 build-release/bin/NeverDArm64eLiftedImageTests \
  --gtest_filter='AArch64AsyncUnwind.PublishedFrameRecoversCallerAtFiveSignalInterruptions:AArch64AsyncUnwind.SignalReturnResumesCallerWithRecoveredVectorLanes' \
  --gtest_output=xml:signal-results.xml
```

The XML properties `native_system_caller_steps` and
`native_signal_caller_resumptions` must be 10 and 8 respectively. Without the
environment variable the test can check publication and the recovered rows
without performing those native steps.

### Signal delivery, language cleanup and new thread-local storage

Resolve the unwind rows and call-site table while the image is loaded.
`prepareDarwinArm64eExceptionSite` intersects the recovered row with the
personality's guarded range. At a protected call, it requires a saved LR and
an actual call instruction. A frame with no local action can forward to its
authenticated caller. The SA_SIGINFO handler passes this plan and a throw
or rethrow callback to `ndDarwinArm64eDeliverSignalException`, then returns.
The kernel restores the interrupted thread's signal mask and stack. The
callback runs there, with the original call's return address, so the system
personality selects the landing pad and executes cleanup. A protected
instruction other than a call needs separate evidence for the live values
expected by its landing pad. An ordinary unmanaged throw out of a handler
still depends on the system unwinder's signal-trampoline support.

For TLS contributed by a native object, the publisher emits dyld descriptor
and template sections, an authenticated bootstrap bind, and offsets for
initialized data, zero-fill and compiler guards. The original template forms
the prefix of the combined template, preserving references from existing
descriptors. Strong exported definitions receive per-thread export-trie
entries. Reading the C or C++ variable resolves the descriptor for the
calling thread. Compiled first-use code and the authenticated libSystem
terminator call supply C++ initialization and thread-exit destruction.
Address-diversified pointers in copyable template storage require an
instance-specific signing contract. An old template can contain an ordinary
pointer or an address-independent authenticated function pointer. Copying its
dyld recipe preserves the target and signing schema while the new slot gets
its own chain successor. [Apple's dyld TLS implementation](https://github.com/apple-oss-distributions/dyld/blob/main/libdyld/ThreadLocalVariables.cpp)

Image constructors have a different lifetime. The publisher resolves each
native module-initializer relocation to a local function and emits a
read-only array of 32-bit offsets from the Mach header for dyld. Existing
image constructors retain their order. [Apple's initializer reader](https://github.com/apple-oss-distributions/dyld/blob/main/common/MachOAnalyzer.cpp)

For register-only integer leaves the publisher also accepts multiply,
multiply-add, multiply-subtract and scalar shifts whose emitted instruction
words and register operands match the source. LLVM instruction simplification
removes the generic oversized-shift check when the lifted AArch64 count mask
proves it redundant. The native control covers wraparound products and shift
counts at and above the register width.

Both M1 runners passed all 39 cases using the Release host built from
`3b6fc93c6c800290c3ff02a98f15f287cd589ee4`. Each executed 16 C++ signal
deliveries and 16 Objective-C deliveries with typed catches, cleanup and
rethrow. Each ran eight compiler-linked or published TLS lifecycle controls,
including image initialization, independent threads and thread-exit callbacks.
The arithmetic image ran once before and once after publication. Both runners
reported unchecked ordinary-memory writeback. Xcode 27 built and signed the
x1 slice, and the M1 loader returned Darwin error 86 on launch.
[Native Actions run](https://github.com/gmh5225/test_mac_intel/actions/runs/43387041250)
and [verification record](evidence/arm64e-signal-cleanup-and-new-tls-2026-11-13.json).
