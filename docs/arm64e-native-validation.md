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
an actual call instruction. The same rule covers a prefix in its guarded
range and a call's own return edge that loops back to the call. Every
instruction on that path must preserve the row, memory and the callee-saved
registers. A stack reload must fit inside the recovered live frame.
A frame with no local action can forward to its
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
When the template contains an address-diverse pointer or the object supplies
per-thread initializer pointers, the publisher registers a private descriptor
with dyld. On first access the public descriptor obtains the calling thread's
block, authenticates each nonnull pointer against its template address and
signs it for the corresponding address in that block. The resolver uses
the original IA, IB, DA or DB key and discriminator. It runs the original
initializer sections before the added sections, traversing each in reverse
entry order. Recursive TLS lookups from callbacks use the same initialized
pointers. The resolver preserves Darwin's TLV argument and SIMD registers.
[Apple dyld TLS allocation](https://github.com/apple-oss-distributions/dyld/blob/main/libdyld/ThreadLocalVariables.cpp)
and [legacy initializer ordering](https://github.com/apple-oss-distributions/dyld/blob/dyld-852.2/src/threadLocalVariables.c).

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

The per-thread resolver and protected-call path tests ran in the 40-case
Release suite on macOS 26.7.2 and 27.0.1. Each M1 runner executed 24 C++
and 24 Objective-C signal deliveries and eight reference or published TLS
lifecycles. Xcode 27 built and signed the x1 probe. Its loader rejected
the slice on this hardware.
[Native Actions run](https://github.com/gmh5225/test_mac_intel/actions/runs/43462907219)
and [per-host results](evidence/arm64e-tls-resolver-and-call-setup-2026-11-14.json).

Register-only lifted leaves can also publish integer REV, RBIT and CLZ.
The object producer uses LLVM's bit-provenance matcher to recognize the
lifter's shifts and masks. The publication check still compares the emitted
instruction words and their operands. The byte-reversal and trailing-zero
fixture compares both entries with independent loops, including a zero input.

Lifted integer leaves also publish signed and unsigned selects, register
division and remainder, sign- or zero-extending loads, and local conditional
loads and stores. The producer recognizes division only after proving the
zero and signed-overflow guards carry the architecture's results. Conditional
branches must keep their displacement and land inside the same function.
Native drivers check null and nonnull paths, adjacent memory, signed extrema,
and zero divisors on the original and published libraries.

The Windows CI analysis and authenticated-call fixtures use the same checked
Clang 22.1.6 frontend as the Windows source-patch tests. Clang 20.1.8 accepts
the arm64e target while leaving function-pointer authentication off by default.
Explicit flags enable C function-pointer signing in that version, but its
Objective-C class and method metadata still lacks the authentication relocations
exercised by the loader tests.

The Release host passed all 46 cases on macOS 26.7.2 and 27.0.1. Each M1
runner executed 74 original or published images and verified 24 C++ and 24
Objective-C signal cleanups. The Windows runner independently compiled the
signed C and Objective-C controls at O0 and O2. Both M1 runners reported
that native checked writeback remained unavailable.
[Native and Windows Actions run](https://github.com/gmh5225/test_mac_intel/actions/runs/43545498327)
and [per-host verification record](evidence/arm64e-scalar-publication-2026-11-14.json).

Ordinary register-indexed accesses and non-SP writeback accesses now enter
the same audited native register-body producer as physical stack cursors.
The producer retains each original memory instruction and its base update,
including the runtime checked-pointer behavior. This covers signed and
unsigned GPR loads, scalar and SIMD stores, signed word pairs, and B/H/S/D/Q
memory registers. Publication continues to verify the relocated
instruction words, control-transfer targets and unwind rows.

Direct-call X8/X9 result discovery follows the function's control-flow edges.
Backward liveness reaches reads across conditional, forward and backward
branches. W writes kill the complete corresponding X value, and another call
ends the previous result. Functions without direct calls skip this analysis.
The two-bit worklist processes each edge at most twice and emits results in
instruction order.

The additional publication controls compare indexed reads and signed-offset
stores, zero and nonzero loop counts, pre/post-indexed scalar and pair
accesses, and all five SIMD memory widths. Narrow SIMD loads are subsequently
stored as full 128-bit registers, checking that their upper lanes are cleared.
Independent drivers compare complete destination buffers, untouched guards
and updated pointers on the original and published libraries. A separate
negative case verifies that intervening W8/W9 writes end both old call results.

Both M1 runners passed all 57 cases with the Release host built from
`7991be5e74e7c671ce0e30b5fdfba002df64f72b`. Each executed 94 original or
published images, 24 C++ and 24 Objective-C signal cleanups, and eight
per-thread resolver lifecycles. The Windows frontend controls also passed.
Both hosts still reported native CPTA-on unavailable.
[Native Actions run](https://github.com/gmh5225/test_mac_intel/actions/runs/43581342838)
and [per-host verification record](evidence/arm64e-memory-and-register-publication-2026-11-14.json).

### Fixed-width SIMD data in published images

The complete native register-body producer also admits audited scalar FP and
Advanced SIMD data-processing instructions. It identifies their
[A64 encoding class](https://support.arm.com/documentation/ddi0602/2023-12/Index-by-Encoding),
which has no PC-relative operands, and supplies the original instruction word
to LLVM MC. This preserves vector arrangements, lane operands, modified
immediates, table-register lists and floating-point controls. Functions must
still pass the existing semantic audit and complete-body/unwind checks.
Memory and control transfers retain their separate relocation rules; SVE/SME
instructions do not enter this encoding class.

Publication regressions exercise all integer ADD/SUB lane widths in 64/128-bit
registers, vector arguments and results, logical operations, permutations,
table lookups, lane insertion/extraction and modified immediates. Independent
scalar drivers also check widening/narrowing, multiplication, shifts,
reductions, comparisons and saturation, including FPSR.QC. The fused float and
double lane controls compare every result and exception flag under all four
rounding modes. A mixed SIMD/physical-SP case retains signed LR, remembered
CFI states and multiple epilogues. Changing an encoded SIMD destination is
rejected before publication. This extends the admitted image contract; it
does not qualify arbitrary image layouts or native CPTA-on.

Both M1 runners passed all 70 cases with the Release host built from
`a726549412997a5be4b257148ece6cc57916ed65`. Each executed 120 original or
published images, including all 13 added SIMD cases, and retained the signed
signal cleanup and TLS lifecycle controls. The Windows frontend controls
also passed. Neither host reported native CPTA-on.
[Native Actions run](https://github.com/gmh5225/test_mac_intel/actions/runs/43599335828)
and [per-host verification record](evidence/arm64e-simd-publication-2026-11-14.json).

### Preserved registers in whole lifted frames

The native register-body producer retains DWARF offset, restore and same-value
rules for X19-X28 and V8-V15, alongside FP/LR. Register admission reuses the
Darwin unwind ABI definitions. The final image must still match the original
instruction words, control transfers, B-key augmentation and complete CFI
rows; changing an X19 or V8 spill to another valid stack slot is refused.
CFA bases and unsupported CFI operations retain their existing restrictions.

Four publication fixtures save and overwrite every preserved GPR and vector,
use FP-based CFA rows, and retain two signed epilogues with remembered CFI
state. Independent callers check normal returns over zero and wrapping integer
inputs. The D fixtures check the low 64-bit lanes required by the Darwin call
ABI; the Q fixtures additionally check the upper lanes explicitly saved by
their instructions. D upper lanes are checked separately: reloads clear them,
whereas signal resumption retains their interrupted values. Signal fixtures
interrupt the fully established original
or published frame, recover its caller from the linked metadata and machine
instructions, and resume through the system signal frame. The resumed caller
checks every saved register. These checks do not imply that ordinary language
unwind tables preserve caller-clobbered vector upper lanes.

This closes the indexed-DWARF preserved-register publication restriction. That
qualification did not reconstruct compact saved-pair frames, qualify arbitrary
image layouts or arbitrary non-call language cleanup, or supply native CPTA-on
evidence. The current GitHub [standard macOS ARM runners](https://docs.github.com/en/actions/reference/runners/github-hosted-runners)
list M1 processors, and [larger ARM runners](https://docs.github.com/en/actions/reference/runners/larger-runners)
list M2. Neither test repository had a registered self-hosted runner during
this qualification; hardware features remain subject to the native probe.

Both M1 runners passed all 74 cases using the Release host built from
`8223da42b74cd5a00df267cfc71a7dac8b9c607e`. Each executed 128 original or
published images. The added cases made 104 normal calls with complete
register observations and 24 authenticated caller resumptions across signal
frames per host. All original signed exception, signal cleanup and TLS
controls remained enabled, and the Windows frontend checks passed. Local
checks passed 74 publication cases, 18 unwind-recovery cases and 137 ABI/format
cases; 42 native-gated ABI/format cases were skipped locally. Neither native
host reported CPTA-on.
[Native Actions run](https://github.com/gmh5225/test_mac_intel/actions/runs/43610978871)
and [per-host verification record](evidence/arm64e-saved-register-publication-2026-11-14.json).

### Compact saved frames in published images

The native register-body producer also reconstructs classic ARM64
frame-pointer compact recipes from the loader's normalized saved-register
slots. MC receives the canonical FP-based CFA, LR/FP record and descending
GPR/D offsets; the final image must retain the original compact encoding and
instruction words without introducing a DWARF fallback. The slot ordering
follows Apple's [compact unwinder](https://github.com/apple-oss-distributions/libunwind/blob/main/libunwind/src/CompactUnwinder.hpp).
These MC directives describe the compact recipe; exact-PC asynchronous
recovery still analyzes the original or published instructions.

When an ordinary lifted callee needs a Swift error-result prototype, verified
native callers keep their physical X21 call boundary and symbolic target.
Only LLVM callers receive rewritten prototypes and call operands. The existing
signed Swift forwarding case checks this mixed boundary and still refuses a
callee whose X21 write was removed.

Nine cases cover all nine individual saved pairs, no additional pair, all
pairs, sparse masks, an additional stack reservation below FP and two signed
epilogues. The fixtures overwrite the live LR after saving it, so signal
resumption must authenticate the saved stack copy. Independent callers check
every preserved GPR and vector low lane. Saved D upper lanes are checked
against the appropriate reload or interrupted value, and unsaved upper
lanes must remain unchanged. Mutating any emitted saved-register offset
refuses publication and creates no output.

This qualification uses one function per compact fixture. Linkers can coalesce
adjacent identical compact recipes into one range; recovering an individual
function from that range still needs a proved function boundary. Frameless
saved-pair layouts and PAuth_LR compact publication also remain separate gaps.
These cases do not qualify arbitrary lifted images, arbitrary protected
non-call language cleanup or native CPTA-on.

Both M1 runners passed all 83 cases using the Release host built from
`b54eed139a515af9042314863daf30ad193f78b7`. Each executed 164 original or
published images; the added compact cases made 364 normal calls with complete
register observations and 24 signed caller resumptions across signal frames
per host. The mixed native/Swift call case, original authentication and
exception controls, existing signal cleanup and TLS cases remained enabled.
Windows frontend checks passed. Local checks passed 83 publication cases,
18 unwind-recovery cases and 137 ABI/format cases; 42 native-gated ABI/format
cases were skipped locally. Both native CPTA probes remained unavailable.
The LLVM fork revision and r6 prebuilt pin were unchanged.
[Native Actions run](https://github.com/gmh5225/test_mac_intel/actions/runs/43617563664)
and [per-host verification record](evidence/arm64e-compact-frame-publication-2026-11-15.json).


### Coalesced, frameless and PAuth_LR compact frames

Compact recovery now seeds each authenticated function entry in a coalesced
range with its own incoming register state. Mach-O typed function symbols,
including `LC_FUNCTION_STARTS`, supply roots without being reclassified as
loader/CRT entry points. Guessed boundaries and continuation labels do not
supply roots. Rows keep offsets relative to the original unwind range; calls
and tail transfers to another proved entry do not merge its state. Fallthrough
across a boundary and transfers into another function's interior are refused.
DWARF, language tables and explicit signing-PC rules still require one owner.

The native producer reconstructs frameless saved-pair recipes with the complete
stack size and normalized slots. These preserve live LR, omit an invented
FP/LR spill record and support an additional stack reservation. Coalesced
fixture pairs must share the same original unwind record before publication.
Independent native callers compare all preserved registers and signal-driven
caller resumptions for both functions.

PAuth_LR publication preserves PACIBSPPC, AUTIBSPPC and RETABSPPC words and their
function-relative positions. The LLVM fork emits a canonical B-key mode-5
recipe with the signing-PC offset. Ordinary compact mode cannot authorize
these PC-signed words. The final verifier checks the complete emitted recipe
and rejects removal of the signing CFI.

The Xcode 27 diagnostic reproduced a transport difference: the input object
contained `0x05002515`, but the Apple-linked image contained `0x04000515`.
Regular LLVM object emission therefore retains DWARF for nonzero signing-PC
offsets. The final-image writer and explicitly marked native-object installer
can consume the full compact payload directly. Tests independently construct
the single final-image encoding slot in an unsigned structural fixture after
linking; they record the linker word before construction and also require
rejection of an ordinary-FP record with PC-signed instructions. This does not
qualify Apple's external linker as a mode-5 nonzero-offset producer.

PAuth_LR cases report structural images separately from native execution.
Neither a structurally correct compact record nor a passing classic PAC signal
resumption qualifies execution on PAuth_LR hardware, arbitrary lifted images,
arbitrary protected non-call language cleanup, or native CPTA-on.

Both native runners passed all 96 cases from
`941203f5d2036ad3541a2e797938ccba01a898ed`. Each executed 202 original or
published images. The eleven added classic compact cases contributed 442
register-observed calls and 48 signed signal-caller resumptions per host.
The two PAuth_LR cases contributed four structural original/published images
and zero native PAuth_LR executions per host. Windows frontend checks passed.
The Xcode 27 linker word before fixture construction was recorded as
`0x04000515` for the nonzero-offset case; the final installed image retained
`0x05002515`.

Local Release validation selected 909 cases: 862 passed, 47 native-gated cases
were skipped, and none failed. Six LLVM MC/FileCheck invocations passed, as
did the pinned 22.1.2 format check for all thirteen changed source files and
the documentation inventory check. The r6 prebuilt configuration is unchanged.
The encoder and consumer-boundary changes were merged directly into the LLVM
fork in [PR #10](https://github.com/NeverSight/llvm-project/pull/10) and
[PR #11](https://github.com/NeverSight/llvm-project/pull/11).
Both M1 environments still reported native CPTA-on unavailable, and neither
repository had a registered self-hosted runner at the recorded observation.
[Native Actions run](https://github.com/gmh5225/test_mac_intel/actions/runs/43628073491)
and [per-host verification record](evidence/arm64e-compact-range-publication-2026-11-15.json).


### System unwinding of frameless saved pairs

The previous frameless qualification compared native returns and NeverD's
instruction-derived signal resumption. An independent public libunwind cursor
exposed a different system consumer behavior: the compact frameless walker
loads its first spill at CFA and leaves SP reduced by the saved-pair bytes.
A two-register control restored the wrong X19/X20 values and SP. Apple's
[compact walker source](https://github.com/apple-oss-distributions/libunwind/blob/main/libunwind/src/CompactUnwinder.hpp)
contains the same slot and SP calculations.

Publication now gives these bodies indexed DWARF with explicit actual stack
size and CFA-relative saved slots. It retains the original instructions and
B-key return ABI. The final-object verifier checks the complete CIE/FDE recipe;
existing offset-mutation controls must still refuse publication. This is a
complete-prologue synchronous recipe. Instruction-derived recovery remains
the owner of interrupted prologue and epilogue states.

The native signal driver additionally seeds a fresh system cursor from the
genuine interrupted context, steps the published frame and compares SP,
authenticated return PC, X19-X28 and D8-D15 with independent caller inputs.
All-pair, sparse, coalesced and each individual-pair cases exercise this path.
These checks do not qualify native PAuth_LR or CPTA-on, nor arbitrary non-call
language cleanup or arbitrary image publication.


### Explicitly protected non-call exception delivery

An address-based C++ or Objective-C LSDA can now authorize delivery at a
non-call instruction without finding a later throwing call. Its entire
instruction must belong to the guarded range, have a saved original LR,
preserve live frame registers, and keep the same complete recovery row at
the virtual return edge. The existing signal-return bridge runs the supplied
throw thunk outside the handler; the original system personality selects and
executes the interrupted frame's cleanup. Missing LSDA ownership, partial
instruction coverage, changed CFA/signing/spills and preserved-register
writes remain rejected.

Independent assembly fixtures protect exactly one load in a loop with no
calls. Their C++ and Objective-C landing pads record the original or changed
published cleanup before resuming the exception. The driver checks that
cleanup, the outer cleanup, exception identity and the restored signal mask
on both ordinary and alternate stacks, for original and installed images at
O0 and O2. This qualifies explicitly guarded points with a proved frame;
it does not infer missing asynchronous lifetime information in arbitrary
compiler output.


Both native runners passed all 99 cases from
`c316fe83a8d07880aa6c851274ae0be62a8ea8ff`. Each reported 220 executed
original/published images, 39 independent system frameless steps, eight C++
non-call cleanup deliveries and eight Objective-C non-call cleanup deliveries.
The latter cover both optimization settings, both image forms and both stacks.
Windows frontend checks passed. The independent LSDA fixture includes a
no-handler entry for `_Unwind_Resume`; omitting it was reproduced as a
second-phase termination in a local ordinary-arm64 control before correction.

Local validation passed 99 publication cases and 26 recovery cases, with the
four affected cleanup cases rerun after the fixture correction. The broader
1084-case ABI/language selection passed 1036 cases and explicitly skipped 48,
with no failures. All twelve changed source files passed clang-format 22.1.2,
and the documentation inventory check passed. LLVM and r6 pins are unchanged.
Both M1 runners still report native CPTA unavailable; PAuth_LR execution,
arbitrary lifted-image contracts and missing asynchronous lifetime information
remain unqualified.
[Native Actions run](https://github.com/gmh5225/test_mac_intel/actions/runs/43675185998)
and [per-host verification record](evidence/arm64e-system-frames-and-noncall-2026-11-15.json).


### Lifted indexed language frames

Automatic whole-image publication can retain a complete indexed C++ or
Objective-C frame whose original read-only LSDA uses an omitted LPStart and
ULEB128 function-relative call-site offsets. The native register-body producer
keeps every instruction at its original function offset, including the cleanup
landing pad, direct imported calls and terminal BRK. It emits the original
personality and LSDA CFI references. The installer binds the LSDA to original
storage and resolves the personality through the original authenticated GOT.
A bare RET entry also retains its incoming physical result registers.

The native-object reader accepts an external LSDA difference relocation only
at a decoded PC-relative LSDA field. Original-image bindings and final
relocations remain mandatory. The publication verifier parses linked CFI
against the relocated image, compares the complete CIE/FDE rules, checks
LSDA/personality slot addresses after any image slide, and compares every
source instruction and direct transfer. A changed LSDA operand is rejected
without publishing an output.

`LiftedNonCallRunsInterruptedAndOuterCleanups` and its Objective-C companion
run the complete Pipeline/PatchMode/MachOPatcher route at O0/O2. Their native
driver checks interrupted and outer cleanup, exception identity, restored
signal masks and ordinary/alternate signal stacks for original and published
images. It separately calls the bare RET entry with nonzero 64-bit seeds.
Native-object replacement remains an independent control.

This contract requires unchanged instruction offsets and complete original
language metadata. Explicit LPStart, SJLJ tables, compact frameless language frames,
unsupported native body operations and missing asynchronous lifetime tables
remain outside this profile. It does not qualify arbitrary lifted-image
publication or native PAuth_LR/CPTA-on execution.


Both macOS runners passed all 101 cases from
`efa88237c8a6e6078e5c73b6674efc0abf87406f`. Each executed eight C++ and eight
Objective-C cleanup deliveries through the new lifted-image route, in addition
to the independent native-object controls and 220 existing original/published
image checks. The Windows frontend job also passed. Both hosts remain M1
virtual machines: explicit PAuth_LR and CPTA-on were unavailable, including
the signed arm64e.x1 launch attempt on the newer environment.

Local publication passed 101 cases. The 1408-case exception/ABI selection
finished with 1359 passes, 48 skips and one 120-second parallel timeout. That
unchanged immediate-memory test passed alone in 22.60 seconds, leaving 1360
passed cases and 48 environmental skips after retry. Both logs are retained;
the initial timeout is not counted as a pass. Eight source files passed
clang-format 22.1.2 and the documentation inventory passed. LLVM and r6 are
unchanged. The temporary native replay release and asset were removed, and
the CI branch was returned to its original tree.
[Native Actions run](https://github.com/gmh5225/test_mac_intel/actions/runs/43687902578)
and [per-host verification record](evidence/arm64e-lifted-language-frames-2026-11-15.json).

### Compact language frames and explicit LPStart decoding

Automatic publication also accepts complete compact FP C++ and Objective-C
language frames without a DWARF FDE. The compact reader retains the selected
personality pointer slot; exception resolution, native-object verification and
whole-image verification consume that identity. The generated object supplies
canonical personality and LSDA references. Verification compares the linked
slot and LSDA after any image slide, while retaining the original compact
frame recipe and every instruction offset. A language frame must cover exactly
one published function; an unwind-only coalesced range does not establish this
LSDA contract.

For a compact FP frame, asynchronous recovery can select the saved LR even
while the live register still contains it. One exact frame-record lookup must
prove an active, full-width copy with the same return and signing state.
Partially overwritten or released saves and a different live signing recipe
cannot authorize exception delivery through that copy. Declared DWARF columns
keep their existing authority.

`CompactNonCallRunsInterruptedAndOuterCleanups` and its Objective-C companion
exercise native-object replacement. Their `LiftedCompactNonCall` counterparts
exercise the normal lift/publication pipeline. Each uses O0/O2 original and
published images, ordinary and alternate signal stacks, nested cleanup and
exception-identity checks. The input must actually have a compact FP record
and no FDE; a changed LSDA operand must still fail publication.
The compact recovery tests separately cover overwritten, released, misplaced
and differently signed LR copies.

The shared Itanium reader now keeps explicit LPStart separate from the
function-relative guarded ranges and preserves an explicitly encoded zero.
This follows the independent address bases used by
[LLVM libc++abi's personality](https://github.com/llvm/llvm-project/blob/main/libcxxabi/src/cxa_personality.cpp)
and [GNU libsupc++](https://github.com/gcc-mirror/gcc/blob/master/libstdc%2B%2B-v3/libsupc%2B%2B/eh_personality.cc).
`ItaniumLSDA.ExplicitLPStart*` checks ELF/Mach-O and x64/AArch64 coordinates,
including guarded-range overflow. At that milestone, publication still required
an omitted LPStart and a compact FP or indexed frame. The next section records
explicit-base rebuilding and compact frameless language publication. Unsupported
body operations, missing lifetime tables and native PAuth_LR/CPTA-on qualification
remain separate gaps.


Both macOS runners passed all 105 cases from
`2a837b5adaa629e91477df909e66cb010f850707`. Each executed 32 new compact-frame
cleanup deliveries across C++/Objective-C, native-object/lifted publication,
O0/O2 and ordinary/alternate signal stacks, in addition to the existing
220 original/published image checks. The Windows frontend job passed.
Local publication passed 105 cases, implicit recovery passed 29, and the
focused LSDA/compact-reader checks passed three. Seventeen source files passed
clang-format 22.1.2; the documentation inventory passed.

Both hosts remain M1 virtual machines. The probes did not qualify explicit
PAuth_LR or CPTA-on, including the signed arm64e.x1 launch attempt on the
newer environment. LLVM and prebuilt r6 are unchanged. The temporary replay
release and asset were deleted with verified HTTP 404 responses, and the
test repository was restored to its original tree.
[Native Actions run](https://github.com/gmh5225/test_mac_intel/actions/runs/43708062786)
and [per-host verification record](evidence/arm64e-compact-language-frames-2026-11-15.json).


### Explicit LPStart rebuilding and frameless language frames

The native register producer now rebuilds explicit-base C++ and Objective-C
LSDAs in generated read-only exception storage. Guarded ranges remain relative
to the function; landing pads are rebased independently. The usual output omits
LPStart. If a handler is at the function entry, an explicit base one instruction
before it preserves a nonzero handler offset, since zero denotes no handler.
Action links, selectors and exception-specification list offsets retain their
original bytes. Type references use relocated PC-relative fields and preserve
indirection through the original binding cells. The shared reader also resolves
types referenced only by negative exception-specification filters.

Publication validates the installed private candidate before making the output
visible. It compares normalized guarded ranges, handler coordinates, action
chains, type identities and specification lists with the source, in addition to
the existing instruction, CFI, personality and dependency checks. Native object
installation also reserves load-command capacity for new writable and zero-fill
data mappings before assigning addresses; type-pointer cells no longer fail
late because their segment commands lack header space. The original
omitted-base table may still be reused when its function-relative contract
holds. Truncated tables, handlers outside the copied function and incomplete
metadata cannot authorize publication.

Compact frameless language frames retain their personality and LSDA. Frames
with saved pairs use indexed DWARF for the system walker's established spill
layout, just as unwind-only frameless publication does. The regression suite
separately checks ordinary signed returns, preserved registers and system walks
of these installed language frames. SP-based indexed frames with a saved LR
exercise actual C++ and Objective-C cleanup delivery at a protected non-call
instruction, at O0 and O2 and on ordinary and alternate signal stacks.

The typed explicit-base fixture executes a nonmatching catch followed by cleanup;
its additional specification-only type tests reconstruction without invoking an
unexpected-exception path. The entry-handler case qualifies address serialization
only. At that milestone, frames whose caller return exists solely in live LR
were not admitted to protected exception delivery: the direct throw thunk
would overwrite that return address.
Absent lifetime tables cannot establish arbitrary asynchronous cleanup scopes.

Both macos-26 and xcode-27 passed all 119 source-bound publication cases from
`42786bcb1fc38597a31cb79a04b4f434e635eb24`, with no failed or skipped cases. Each
host executed 80 new language-cleanup deliveries, 52 new frameless preserved-
register calls, six signal resumptions and three system frameless steps. The
complete replay includes 226 original/published image checks per host. The
Windows frontend job passed. Local publication passed 119 cases; related CTest
coverage passed 1,067 with 48 explicitly skipped native/environment cases; all
11 focused LSDA checks passed. Thirteen C++/header/definition files passed
clang-format 22.1.2 and the documentation inventory passed.

The fresh hardware reports still identify both runners as M1 virtual machines.
Explicit PAuth_LR, PACM PC dependence and CPTA-on remain unqualified, including
the arm64e.x1 launch attempt. LLVM and prebuilt r6 are unchanged. The temporary
release and asset were deleted with verified HTTP 404 responses, and the replay
branch was returned to its baseline tree after saving the results.
[Native Actions run](https://github.com/gmh5225/test_mac_intel/actions/runs/43733530036)
and [per-host verification record](evidence/arm64e-lsda-frameless-2026-11-16.json).


### Live-LR asynchronous language delivery

Protected indexed-DWARF frames can retain their caller return in live x30.
`prepareDarwinArm64eExceptionSite` requires the original system CFA and live-LR
rule to match the recovered frame at the instruction and its virtual return
edge. The live return must use the ordinary IB/CFA signing recipe. An inferred
interrupt row cannot replace incompatible synchronous unwind metadata; compact
FP records with a different saved return remain rejected.

`ndDarwinArm64eDeliverSignalException` preserves the genuine signal context's
SP and LR and schedules a small native bridge. The bridge runs after sigreturn,
reserves the interrupted frame's 128-byte red zone and saves two independent
returns. DWARF return column x17 supplies the signed virtual protected edge;
the x30 column restores the original caller return. The language throw thunk
then runs outside the handler, with the original signal mask and alternate-
stack state restored. Stack capacity and authentication are checked before
mutating the context. No bridge slots are written over an active kernel signal
frame. Its assembly, register indices and frame size belong to
`DarwinArm64eSystemUnwind.def`.

The bridge personality advances the system cursor to the original language
frame and delegates matching and cleanup selection to that frame's personality.
A local catch retains the bridge's identity across the two unwind phases.
Darwin's landing-pad resume itself consumes x30, so a separate authenticated
entry restores the caller LR before the original handler can spill it. Its CFI
retains the interrupted frame's virtual return, original SP and LR; a walker
can still apply the original frame's saved-register rules. The resumed virtual
return uses x12 because system resume does not restore x16/x17. Neither transfer
disables return authentication or copies an opaque system cursor.

The eight `AArch64AsyncUnwind.*LiveLR*` cases cover C++ and Objective-C,
replacement objects and NeverD-generated images, O0/O2, and normal/alternate
signal stacks. They verify red-zone contents, nested handler exceptions,
exception identity, interrupted/outer cleanup ordering, the original SP/LR,
insufficient-stack rejection, and the emitted bridge's independent return
columns. Four cases execute a local catch and authenticated normal return;
the others spill live LR before calling `_Unwind_Resume`, with matching
PC-specific CFI. The Objective-C catch uses the compiler's `_OBJC_EHTYPE_id`
GOT reference to obtain the original object through `objc_begin_catch`.
Hardware breakpoints also interrupt each resume instruction. A fresh public
cursor walks through the interrupted frame to its
caller and checks SP, LR, FP and the preserved general registers before signal
return continues the landing pad. `LDUR` and `STUR` now participate in native
register body generation and the existing exact-instruction publication checks.

This admission requires an owned language lifetime table. It does not infer
cleanup scopes for arbitrary faulting instructions. At this milestone,
compact-only live-LR delivery, additional signing recipes, and native
PAuth_LR/PACM-PC/CPTA-on qualification remained separate coverage requirements.
The LLVM fork revision and prebuilt r6 were unchanged.

Both macos-26 and xcode-27 passed all 127 source-bound publication cases from
`ec26d6dfa35fc2ae4e945a3bdfcfb07a034248e1`, with no failed or skipped cases.
Each host completed 64 live-LR deliveries: 32 cleanup paths and 32 local catches,
covering original/replacement/lifted images, O0/O2 and both signal stacks.
All 64 also checked the resumed cursor against the original caller. The Windows
frontend job passed. Local publication passed 127 cases, unwind recovery passed
31, and related CTest coverage passed 142 with four explicit native/environment
skips. C11/C++20 multi-translation-unit linking, clang-format 22.1.2 and the
documentation inventory passed.

Fresh reports still identify both runners as M1 virtual machines. Explicit
PAuth_LR, PACM PC dependence and CPTA-on remain unqualified, including the signed
arm64e.x1 launch attempt. The temporary release and asset were deleted with
verified HTTP 404 responses; the replay repository was returned to its baseline
tree. [Native Actions run](https://github.com/gmh5225/test_mac_intel/actions/runs/43746760233)
and [per-host verification record](evidence/arm64e-live-lr-2026-11-16.json).

### Compact live-LR and adapted signing recipes

Protected compact frameless scopes can now use live LR when their actual system
recipe agrees with the instruction-derived frame: SP plus the encoded stack
size, no saved register pairs, and unchanged preserved registers. The scope's
owned address-based language table must protect the interrupted instruction.
A mismatched stack size, compact FP record or saved-pair encoding cannot
authorize this route. The saved-pair consumer discrepancy described above still
requires publication to indexed DWARF.

Live-LR delivery first authenticates the recovered original recipe. It then
signs a separate caller return using Darwin's implicit IB/CFA unwind schema.
The bridge preserves this value, the virtual protected edge, and the original
LR independently, below the interrupted red zone. Its personality and resume
entry expose the adapted LR to the system walker while restoring the original
LR for the landing pad's instructions. Thus unsigned and IA-signed interrupted
states need not masquerade as an IB signature. The source still owns its
synchronous calling ABI: the indexed fixtures authenticate or sign their live
return before spilling it for calls; the compact fixtures restore their stack
and authenticate before tail-calling a language resume or catch helper.

The recovered row also retains explicit PAuth_LR and PACM IA/IB recipes and
their relocated signing PCs. These use the existing hardware-capability and
signature checks before any signal-context mutation. Static admission across
both metadata forms does not qualify executing explicit PAuth_LR or the
PC-dependent behavior of PACM on unsupported hardware. The bridge does not
repair arbitrary synchronous CFI, invent language lifetimes, or replace the
signature in a saved source stack slot.

The regression matrix covers C++/Objective-C, compact/indexed frames,
unsigned/IA/IB live returns, replacement objects and NeverD-generated images,
cleanup/local catch, O0/O2, and normal/alternate signal stacks. Each native
delivery also walks a cursor from the resume entry, checks both the original
and adapted returns, and verifies refusal of an unknown recipe or unavailable
explicit PAuth_LR without modifying the kernel context. Runtime assembly and
ABI constants, fixture source, and admitted native hint aliases remain in
their owning `.def` tables. LLVM and prebuilt r6 are unchanged.

Once a proved native register body supplies every instruction and CFI row, it
owns the return-signing policy. Its naked LLVM wrapper drops inferred compiler
prologue attributes and retains the PAuth instruction feature. This permits
an IA or unsigned protected scope to convert its return recipe before making
ordinary Darwin calls without also requesting a conflicting compiler prologue.

The compact Objective-C catch helper also exposed an overly broad shared frame
classification: `objc_begin_catch` and other ordinary runtime-call annotations
were treated as local unwind handlers. `ExceptionFunction::describesUnwindOnly`
now distinguishes these annotations from landing pads, fragile setjmp state and
MSVC tables; personality, LSDA, parse completeness and other language-table
requirements remain intact. The narrower scalar comparison profile continues
to reject runtime-call annotations it cannot project.

Both macos-26 and xcode-27 passed all 167 source-bound publication cases from
`9fa1dda6876d960bac97bd0a69c6cb4a8c457861`, with no failed or skipped cases.
Each host completed 384 live-LR deliveries and resume-cursor checks: 192
cleanup paths and 192 local catches, including 192 compact-only deliveries.
Unsigned, IA/CFA and IB/CFA each contributed 128 deliveries per host. This
includes original, replacement and NeverD-generated images at O0/O2 on normal
and alternate signal stacks. Windows frontend checks passed as well.

Both fresh hardware reports still identify M1 virtual machines. Explicit
PAuth_LR, PACM PC dependence and CPTA-on remain unqualified, including the
signed arm64e.x1 launch attempt. The extended PC-dependent recipes have
recovery/admission coverage here; that does not complete their native
publication and language-delivery matrices. The temporary release, asset and
tag were deleted with verified HTTP 404 responses, and the replay repository
was returned to its baseline tree.
[Native Actions run](https://github.com/gmh5225/test_mac_intel/actions/runs/43792618472)
and [per-host verification record](evidence/arm64e-compact-live-lr-2026-11-16.json).

Local Release checks passed all 167 publication cases, all 33 unwind-recovery
cases, 321 language-EH cases with one explicit sample skip, and 144 related
cases with four native/environment skips. All 324 binary-diff cases and 12
tail-frame cases passed, including the scalar profile's nine focused checks.
The latter target's CLI dependency was built with the real shared SDK enabled.
C11/C++20 multi-translation-unit checks, clang-format 22.1.2 and the documentation
inventory also passed. Generated products were removed after recording hashes;
the active Release build and r6 were retained.


### PC-signing publication and live-LR language delivery

The native register producer retains PACM prefixes, IA/IB explicit SP/PC
signing and authentication, and instruction-relative returns as exact words.
A local ADR can materialize a signing point only inside the same copied body;
external addresses still require their relocation owner. Instruction offsets
and final linked words are verified, preserving PACM adjacency and signing-PC
distances. Explicit authentication displacements cannot point before the body.
Classic compact frames still cannot publish saved PC-signed returns. Compact
frameless bodies with no calls or saved register pairs can retain a live LR;
the signal bridge authenticates its recovered recipe before presenting its
separate IB/CFA return to the system unwinder.

The language matrix adds IA/IB PACM and explicit PAuth_LR across indexed and
compact frames, replacement objects and NeverD-generated images, C++ and
Objective-C, cleanup and local catch, O0/O2 and both signal stacks. The runtime
fixture binds the serialized signing offset to the actual loaded owner before
reading or delivering the interrupted context. Every case reports four
structural original/published images. Explicit native execution is conditional
on an independent instruction probe. PACM native delivery and PC-modifier
qualification are recorded separately; hint-compatible execution cannot prove
that the PC contributes to the signature.

Both macos-26 and xcode-27 passed all 231 source-bound cases from
`4140bf3243b1ce37d57bc308409f175288649515`, with no failed or skipped cases.
Each host completed 640 live-LR deliveries and resume-cursor checks: 320
cleanups and 320 local catches, including 320 compact-only deliveries. The
new PC-recipe cases checked 256 original/published image layouts per host;
PACM supplied 256 additional native deliveries. The 32 explicit PAuth_LR
cases completed structural publication and recovery with zero native deliveries.
The Windows frontend checks passed as well.

Both hosts still report M1 virtual CPUs. Their independent probes qualify
baseline signing and PACM hint behavior, while explicit PAuth_LR, PACM PC
dependence and CPTA-on remain unavailable. The repository has no self-hosted
runner providing those missing features. This result does not establish full
native arm64e.x1 coverage.
[Native Actions run](https://github.com/gmh5225/test_mac_intel/actions/runs/43910496915)
and [per-host verification record](evidence/arm64e-pc-signing-2026-11-17.json).

Local Release checks passed all 231 publication cases and 690 related recovery,
language-EH, return-protection, relocation and rewrite-contract cases, with one
explicit real-binary sample skip. clang-format 22.1.2, whitespace and the
localized documentation inventory passed. The temporary release, asset and
tag were removed with verified HTTP 404 responses; the replay repository was
restored to its baseline tree. The 42,325,529-byte local replay package was
removed after recording its hash. The active build and prebuilt r6 are retained.

### Native x1 runner qualification

Apple specifies M6 or later Macs and A20 Pro or later iPhones for arm64e.x1;
the simulator cannot execute this ABI. Standard GitHub macOS ARM runners use
M1, and larger ARM runners use M2. Changing their image to Xcode 27 therefore
does not provide the required hardware. The local M4 Max also does not qualify.
See [Apple's pointer-authentication requirements](https://developer.apple.com/documentation/security/preparing-your-app-to-work-with-pointer-authentication),
[standard runners](https://docs.github.com/en/actions/reference/runners/github-hosted-runners)
and [larger runners](https://docs.github.com/en/actions/reference/runners/larger-runners).

On an M6 Mac with an arm64e.x1-capable Apple SDK and operating system, run:

```bash
python3 scripts/verify_arm64e_native.py qualify-x1 --output native-x1
```

The probe compiles fresh O0/O2 signing/cursor controls, checks actual signatures
and PC sensitivity, signs an arm64e.x1 slice with the required Enhanced Security
entitlements, and measures checked scalar arithmetic and ordinary writeback.
It also requires the original authenticated call and language-unwind controls.
The top report binds all three component reports by SHA-256 and qualifies the
environment only when every required native claim is true. Exit 2 records an
unavailable environment; exit 1 records failure. CPU names, successful assembly,
PACM hint execution and a green negative-control workflow do not qualify x1.
Temporary executables and entitlements are removed; reports remain available.

A registered GitHub Actions runner with labels `self-hosted`, `macOS`, `ARM64`
and `neverd-arm64e-x1` can run the manual **Arm64e x1 native qualification**
workflow. It has read-only repository permissions, requires the same strict
probe to return success and uploads the component diagnostics on failure too.
It is not dispatched until a suitable runner is available. With no device
available, `gmh5225/test_mac_intel` uses its hosted runners to exercise the
same gate as a negative control and continue publication regression tests.
An unavailable aggregate report remains unqualified even if that regression
workflow succeeds.

After the environment qualifies, build `NeverDArm64eLiftedImageTests` through
`scripts/build.py` using the integrated LLVM fork and execute it with both
`NEVERD_ARM64E_LIFTED_NATIVE=1` and `NEVERD_ARM64E_ASYNC_NATIVE=1`. The four compact
PAuth_LR publication cases now probe capability before execution, retain the
actual signing recipe/PC in signal plans and exercise both original and
published signed images. They explicitly sign execution copies after publication,
strictly verify them and recover the execution plans again from those copies;
the unsigned source fixture remains unchanged through publication. They separately count normal calls, caller resumptions
and public system-unwinder steps. Six classic compact signal controls also
exercise the saved-LR system-step path on ordinary arm64e hosts. Unsupported
hardware still reports zero
native PAuth_LR executions. The r6 prebuilt remains unchanged and skips these
four cases because its MC producer lacks mode-5 support.

The aggregate probe qualifies only the checked environment controls. It does
not establish complete arm64e instruction, arbitrary-image publication or
asynchronous language-exception coverage.

At `a2e577a8791bc0c8e7d6ab8cebae1ce7317574fb`, local Release verification passes
all 233 publication cases and 83 script regressions. The
[hosted replay](https://github.com/gmh5225/test_mac_intel/actions/runs/44003787806)
passes all 233 cases on both macos-26 and xcode-27, with 48 saved-LR public
system-unwinder steps per host and successful Windows frontend checks.
Each Mac contributes eight signed structural PAuth_LR images, while its
PAuth_LR execution, call, resumption and system-step counters remain zero.
Both machines report Apple M1 (Virtual). The strict aggregate reports
`unavailable`: baseline PAC, compiler language unwinding and the cursor pass,
but explicit PAuth_LR, PACM PC dependence, x1 scalar CPTA and checked writeback
do not qualify. The Xcode 27 x1 slice is signed and verified but cannot start
(`EBADARCH`, errno 86). The
[source-bound record](evidence/arm64e-x1-qualification-2026-11-17.json)
binds the reports, execution-copy hashes and exact test package to this result.


### Ad-hoc inputs and repeated native publication

The shared Mach-O signature policy accepts both a linker's bare ad-hoc
CodeDirectory and codesign's exact empty requirements/CMS shape. The latter
must have exactly the known slots, contiguous blobs, a zero first special
hash and a SHA-256 second special hash matching the empty requirements blob.
All identity, architecture, flags, version, page-size and executable-segment
checks still apply. Nonempty requirements, entitlements, developer identities
and unknown metadata require an explicit signing workflow.

`CodesignSignedLiftedOutputAllowsRepeatedNativePublication` signs a compiler
arm64e input with a fixed identifier, publishes its lifted module, then performs
two native-object replacements on the signed output. It checks all four images
with strict codesign verification, preserves each previous input and compares
the signature policy at each generation. Native-enabled execution uses an
independent authenticated caller against the original and all three outputs.
`AdHocRequirementsRefusePublicationWithoutMutation` uses a valid codesign
signature with a nonempty designated requirement; publication must refuse it
without writing the destination or changing the input.

This sequence qualifies one automatic lifted publication followed by native
object publication. At that revision, automatically relifting the appended executable regions
was unsupported: the installer required one original instruction section, and
a diagnostic relift exposed a compact-unwind gap promoted to a function plus an
omitted first appended definition. Those failure diagnostics are retained
separately; they were not successful arbitrary-image coverage.

At `1f1bd9eec18406c0567efab3fa743de986ff08d0`, the local Release suites pass
235 publication, 28 signature-policy, 7 signer and 42 sanitizer C API tests,
with zero failures or skips. The [hosted replay](https://github.com/gmh5225/test_mac_intel/actions/runs/44053487204)
passes all 235 publication cases on both macos-26 and xcode-27. Each host
executes the original and three published images through the authenticated
caller, while the nonempty-requirement control remains refused. Windows
frontend checks pass. Both Macs report Apple M1 (Virtual); the strict x1 gate
still reports `unavailable` for explicit PAuth_LR, PACM-PC, scalar CPTA and
checked memory writeback. The [source-bound evidence](evidence/arm64e-adhoc-publication-2026-11-17.json)
records the exact package, report and per-generation image hashes. The temporary
release, asset, tag and replay files were retired after verification, and unused
local packages and compiled probe copies were removed. The active source build
and archived diagnostics remain available; r6 and all fork pins are unchanged.

### Automatic relifting of installed code

The Mach-O reader retains encoding-zero compact lookup intervals in its raw
unwind view without turning them into primary functions or code ranges. An
independent FDE retains its own exact extent. Symbols and other format entry
evidence remain independent of an empty lookup recipe. This matches the
[upstream unwinder's encoding-zero handling](https://github.com/llvm/llvm-project/blob/main/libunwind/src/UnwindCursor.hpp).

Function detection excludes the linked Mach header, rather than every segment
base. An authenticated function at the start of an appended RX segment is
therefore available to the ordinary patch pipeline. The register producer and
final-byte publisher share one source-extent query: original instruction
sections bound their code; sectionless installed functions require a complete
file-backed span bounded by their compact range and the next mapped entry.
Data sections cannot be consumed as part of such a span.

An entry-level unconditional branch to another authenticated function is a
four-byte forwarding body. Its relocated branch must preserve the exact
installed callee, with no register, SP or LR changes, and its generated unwind
record must describe a zero-stack frameless body. Unreachable instructions
following an old entry trampoline are not emitted as its new body. The native
register producer and final-machine verifier both check this contract.
Sectionless forwarding bodies share the data-overlap check with ordinary
bodies: an adjacent data range is outside the four-byte branch, while a range
covering even its last byte refuses the source extent. The repeated-publication
fixtures exercise both boundaries on installed forwarding entries.

The tight-layout publisher may reclaim `LC_FUNCTION_STARTS` command space.
Republishing uses the independently authenticated symbol/unwind inventory and
checks that every original callable entry has a definition; it does not require
a function-start command which the previous publication legitimately removed.
The signed-leaf, vector-memory, SIMD-register, indexed-frame and global/import
fixtures each exercise three automatic publication generations. They check the
complete 4-entry and 8-entry relift inventories, reject redirected forwarding
operands and missing definitions, verify each signature and preserve every prior input.
Native-enabled runs execute the original and all three outputs through an
independent authenticated caller.

The first native replay exposed a signature boundary error on both macOS
hosts: codesign's strictly verified global/import input had nonzero reserved
bytes after its SuperBlob. `LC_CODE_SIGNATURE.datasize` bounds the reservation;
the embedded length bounds signature contents, as in Apple's
[Mach-O signature reader](https://github.com/apple-oss-distributions/Security/blob/main/OSX/libsecurity_codesigning/lib/machorep.cpp)
and [blob reader](https://github.com/apple-oss-distributions/Security/blob/main/OSX/libsecurity_utilities/lib/blob.cpp).
The shared policy now ignores unused reserved bytes without admitting indexed
slots outside the embedded length. Internal gaps, unknown metadata and strong
signing policy retain their existing checks. Synthetic linker/codesign profiles
and out-of-range slot controls pin this distinction independently of the native
fixture.

This extends repeated publication within the verified native instruction and
frame contracts. Arbitrary additional instruction sections, unsupported source
operations, data-in-code and unrepresented entries still require explicit
support. Interruptions at redirected original entry instructions are not
exercised. It does not qualify native PAuth_LR, PACM-PC or CPTA-on execution.

Local Release validation passes all 240 publication cases, 30 signature-policy
cases, 7 signer cases, 323 language-EH cases, 161 image-boundary cases and 68
relocated-entry cases. Two unrelated checks remain unavailable locally: the
optional real-binary unwind sample and
the native i386 reference. Formatting, whitespace and documentation checks
pass. No LLVM fork changes or prebuilt revision changes are needed.

After integrating dev `371c66be9d562081a53425f2e568379cb4799905`, Release
validation also passes all 8 call-argument and 4 indirect-call source cases,
and repeats all 240 publication cases. At
`6ec2c81bc85b9c7015ba9de8566eddf1782880e6`, the
[integrated native replay](https://github.com/gmh5225/test_mac_intel/actions/runs/44074026861)
passes 240 cases on each of macos-26 and xcode-27, with zero failures or GoogleTest
skips, plus the Windows frontend checks. Each Mac executes 20 images across the
five new scenarios and records 48 saved-LR public system-unwinder steps in the
retained frame suite. Both report Apple M1 (Virtual). The strict x1 gate remains
`unavailable`: baseline PAC, compiler call/unwind and cursor checks pass, but
explicit PAuth_LR, PACM-PC, scalar CPTA and checked writeback do not qualify.
The [source-bound record](evidence/arm64e-automatic-relifting-2026-11-17.json)
binds the source, build receipts, test package, report and per-generation image
hashes, including the preceding signature failure and its successful replay.
Temporary releases, assets, tags and replay files were retired. Unused test
executables, object files and packages were removed; diagnostic logs remain in
a compressed archive with verified decompressed hashes and a recovery index.

### Signals at original entries and discontiguous executable sections

Publishing an arm64e function replaces its first instruction with a direct
branch to the installed body. At the address of that branch, SP still has its
incoming value and the return address is live in LR. The loader supplies the
same four-byte extent and callable target to the publisher and to instruction
unwind recovery. The signal exception planner forwards that instruction to its
caller. The displaced prologue and its LSDA describe a scope that the branch
never entered. The original metadata remains available to metadata queries.
Within a shared compact range, each independent function keeps its own recovery
rows. Bytes following a forwarded entry have an unknown instruction state up
to the next independently established entry.

Whole-image publication accepts multiple regular pure-instruction sections
with file-backed, nonwritable executable mappings. It checks the address and
file coverage of each section and requires a definition at every section start
and independently authenticated callable entry. A data export can sit between
two code sections covered by one compact unwind range. Its section and symbol
identity continue to describe data.

Two fixtures use an empty standard text section, aligned discontiguous code
and an exported read-only marker. One places both functions in `__TEXT`, the
other places the second in `__TEXT_EXEC`. The tests verify the marker bytes and
strict code signature in the input and three consecutive publications. Relifting
recovers four definitions after the first publication and eight after the second.
The native caller reads the exported data and calls both public functions in
each image.

The signal fixture samples an authenticated call loop. It accepts only a
SIGUSR1 context whose actual PC equals the original branch address, recovers
the caller's live LR, SP and X19, crosses the system signal trampoline, and
steps Darwin libunwind to the outer caller. It then returns normally through the
published image. This exercises NeverD's instruction-level signal recovery.
The original synchronous unwind table retains its own semantics.

Both Actions hosts reported Apple M1 (Virtual). Each passed all 242 Release
cases, recorded an exact old-entry signal and one public libunwind step to the
outer caller, and executed eight images across the two new layouts. Windows
frontend checks also passed. The strict native x1 probes reported unavailable
for explicit PAuth_LR, PACM-PC, CPTA-on and checked writeback. Local Release
passed all 242 publication and 36 unwind cases. The broader local analysis and
unwind run recorded 760 passes and 47 skips. A separate parity run passed on
653 instructions from the system `/usr/bin/wc` arm64e text section. The
[validation record](evidence/arm64e-original-entry-and-sections-2026-11-18.json)
includes the identities of the sources, binaries, fixtures, hosts and reports
from the [Actions run](https://github.com/gmh5225/test_mac_intel/actions/runs/44153408139).

Mixed code/data sections, inputs with missing entry or machine ABI facts, and
asynchronous language cleanup without lifetime metadata still need additional
evidence. Platform-wide asynchronous recovery through the original tables
alone also remains unqualified. LLVM r6 stays pinned.

### Embedded data in published instruction sections

The Mach-O loader retains the half-open byte range and kind of each
`LC_DATA_IN_CODE` entry. Function discovery and code ownership exclude those
bytes, including an instruction-sized query crossing a record boundary. The
object installer translates assembler section addresses to final VAs and
writes both the retained and generated records into the signed image.

The publication fixture uses a SOME-only instruction section with a literal
whose bits encode a valid PAC instruction, followed by adjacent 8-, 16- and
32-bit jump-table records. Its return value depends on loading that literal.
The fixture checks the original and three successive publications for identical
data bytes and kinds, verifies their signatures and exercises both exported
functions. The register producer requires complete immutable local storage for
a copied PC-relative literal load. Branches and fallthrough into an island
abort publication. Absolute jump tables, runtime-relocated literals and
unproved indirect dispatch still need additional relocation or control-flow
evidence.

### Original system tables at a forwarding entry

`ndDarwinArm64eInitIndexedEntryCursor` initializes a Darwin stack-walking
cursor from the signal's kernel register state at the first instruction of a
known indexed function. It preserves the incoming SP, FP, live LR, callee-saved
general registers and low halves of D8 through D15. The public system unwinder
looks up the loaded image's indexed FDE and takes both steps to the caller and
its caller. The original and published fixtures compare the original FDE
instructions and require the sampled PC to equal the exported entry.

The independent handler walk reaches the caller directly on the two measured
systems. Its cursor retains the signal-frame SP there. Starting from the
kernel's interrupted state recovers the actual caller SP. On the tested
macOS 26 runner the exact entry lookup returned an index with an empty FDE.
The first instruction's second byte returned the registered FDE for that same
function. The Xcode 27 runner resolved the FDE at the entry itself. The cursor
reports the lookup offset so consumers can retain the exact interrupted PC.
[Compiler-original native controls](https://github.com/gmh5225/test_mac_intel/actions/runs/44217430465)

The full Release replay passed all 243 cases on each of macos-26 and xcode-27
with zero skips. Windows passed its frontend checks. Each Mac executed the
original mixed image and three signed publications. The fourth image contained
16 data-in-code records. A separate forced header expansion preserved all four
input records and the bytes they designate. Both systems reproduced the indexed
old-entry result in the published image. The FDE lookup offset within the first
instruction was one byte on macos-26 and zero on xcode-27. Both reported Apple
M1 (Virtual). Explicit PAuth_LR, PACM-PC, scalar CPTA-on and checked writeback
still lack a qualifying native execution. [Actions run](https://github.com/gmh5225/test_mac_intel/actions/runs/44220600796).

The local six-suite Release run passed 1,080 of 1,132 cases and skipped 52.
Supplying the system arm64e shared cache and the text section of `/usr/bin/wc`
exercised six of those skips. All six passed. The cache checks covered 3,257
image projections and 7,981,837 authenticated slide recipes. The classifier
matched the independent decoder on all 653 system text instructions.
[Source, image hashes and test results](evidence/arm64e-mixed-data-system-unwind-2026-11-18.json).

### Executing embedded relative switches and unwinding compact leaf entries

The arm64e Mach-O publisher accepts a register-indirect switch when the source
CFG proves the selector domain, the load reads a declared local jump table,
and PC-relative instructions materialize both the table and the local target
anchor. Each reachable table slot and case label must stay inside the copied
body. Local block-address references can move with their owner. References that
escape to another function, global or metadata preclude replacing the body.
Installation repeats the control-flow proof using the input Mach-O.

The classic linker fixture grows into a separate executable segment. The
Xcode 27 fixture grows its existing `__TEXT` segment beyond the end of its
declared sections. The loader now treats each file-backed `LC_DATA_IN_CODE`
record as the storage boundary for its table in either layout. Reanalysis of
the first Xcode 27 publication recovers the same three bounded switches.
Bytes between records still need evidence of an owner.
[Xcode 27 compiler input](https://github.com/gmh5225/test_mac_intel/actions/runs/44715360325).

The executable fixture covers unsigned 8- and 16-bit offsets in instruction
units and signed 32-bit byte offsets including backward targets. Its byte table
starts at an odd address, has an unreachable tail and shares one target across
two slots. Calls exercise all slots, the default path, high selector bits and
64-bit arithmetic wraparound. Removing the upper guard, substituting a scalar
for the moving anchor, targeting literal data or changing the declared slot
width each invalidates the installation.

Darwin's system unwinder can also start at the interrupted first instruction
of a compact leaf with zero stack adjustment and no saved registers. The
original and published signal controls verify the exact PC, incoming SP,
callee-saved registers and two caller PCs. The forwarding branch retains the
original compact record. For a leaf with a C++ personality and an LSDA,
the language metadata bits in that record leave the entry register rule
unchanged. Both linkers emitted encoding `0x52000000`. The old observer
rejected that record on both hosts. The source fixture also exercises the
plain compact leaf and original indexed DWARF entry.
[Compiler-original controls](https://github.com/gmh5225/test_mac_intel/actions/runs/44710899294).
[Language leaf before the fix](https://github.com/gmh5225/test_mac_intel/actions/runs/44718387160).

A dynamically rebased pointer literal in executable text has a separate loader
constraint. The macOS 26 classic linker produced the rebase, and the original
dylib run terminated with SIGBUS. The current linker on each runner rejected
the text relocation. Both toolchains also rejected the test's 32-bit absolute
symbol relocation in an arm64e dylib. Publication of those forms needs a
working original and a relocation contract for the destination slot.
[Linker and original-execution controls](https://github.com/gmh5225/test_mac_intel/actions/runs/44708601726).

Both hosted Macs passed all 244 Release cases with zero skips. Each host ran
288 switch comparisons on the original and three signed publications from
its own toolchain. The CLI replay used compiler originals from the classic
and Xcode 27 linkers. Default and `--no-opt` publication each produced three
successive generations from both originals. All passed strict code-signature
verification. The three generations carried 8, 12 and 16 data-in-code records
respectively.
An independent arm64e caller checked 72 results on each of the 14 images,
for 1,008 comparisons per host. Windows also passed its Darwin frontend
signing checks at O0 and O2. [Native replay](https://github.com/gmh5225/test_mac_intel/actions/runs/44719924950).

On both systems a walk initialized at the interrupted PC recovered the caller
and its caller using only the image's registered unwind tables. Starting in
the handler skipped the old entry and yielded a different SP at the caller.
The compact entry result covers a zero stack adjustment and an incoming
live LR, including the variant with a personality and LSDA. A compact
recipe describing a later stack allocation or spills requires a rule for
the exact interrupted instruction.

The local Release run passed 244 publication cases, 792 of 798 jump-table
cases, ten code-owner cases and seven block-address replacement checks.
Six jump-table cases skipped for compiler-shape or cross-runtime reasons.
The hosted machines both reported Apple M1 (Virtual). Native authentication,
compiler-original authenticated calls and signed exception unwinding passed.
Explicit PAuth_LR, PACM-PC, scalar CPTA-on and checked writeback remain
unqualified. The test repository had zero registered self-hosted runners.
[Source revisions, image hashes and measurements](evidence/arm64e-switch-dispatch-2026-11-20.json).

### Recovering missing Mach-O data-in-code records

For functions with local relative switches, the arm64e publisher can recover
a missing or incomplete `LC_DATA_IN_CODE` table. It combines machine
instruction boundaries from the source CFG, the exact bytes read by local
AArch64 literal loads, the finite slots read by proved switch dispatches,
and any records already in the image. The ranges must agree on type and stay
disjoint from live instructions. An alignment fragment that cannot contain
one AArch64 instruction is preserved as ordinary data. A gap large enough for
a whole aligned instruction still requires a source of evidence.

Installation reconstructs these reads from the input image before consuming
the emitted object. It carries the ranges through header growth and a
zero-base image slide, records both the old and copied data in the published
image, and checks their bytes and types after placement. The first publication
therefore supplies native data-in-code records for later generations.

The regression fixtures include 8-, 16- and signed 32-bit relative dispatch,
an eight-byte literal that changes the result, and a byte table at an odd
address adjacent to padding. One input declares only its 16-bit table. Another
has no data records. A literal retargeted into a live instruction invalidates
the source proof before installation.

A separate fixture reads six literal forms through the integer and SIMD
register files. It compares zero-extended W, full X, sign-extended LDRSW,
the bit pattern of a single-precision NaN, double-precision negative zero
and both lanes of a 128-bit Q load. Repeated publication checks the original
and copied pools against independently sized extents.

The macOS 26 and Xcode 27 runners each passed all 247 Release publication
tests, plus 20 native installer cases and four default-startup cases.
Each ran 4,032 independent return-value comparisons on 56 signed CLI inputs
and outputs. These came from eight originals, two publication modes and
three successive generations per mode. The original tables were absent,
empty or partially populated. One local control clears
`S_ATTR_PURE_INSTRUCTIONS` on a linked copy, retains
`S_ATTR_SOME_INSTRUCTIONS`, and signs that copy before execution.
[Hosted native run](https://github.com/gmh5225/test_mac_intel/actions/runs/44731143048).

The signal tests sampled the original and published old entries with the
handler on its regular stack and on a registered alternate stack. Starting
Darwin libunwind at the kernel's interrupted PC recovered the incoming SP
and both caller PCs in six alternate-stack runs per host. These covered an
indexed FDE, a compact leaf, and a compact leaf with a personality and LSDA.
The handler-initiated walk skipped the interrupted entry and reported a
different SP in the caller on both systems, on both regular and alternate
signal stacks.
[Compiler-original alternate-stack controls](https://github.com/gmh5225/test_mac_intel/actions/runs/44728953139).

The five local Release targets accounted for 2,129 cases. Of these, 2,076
passed and 53 had explicit skip conditions. One count-lane case reached the
parallel timeout and passed the normal serial CTest in 24.40 seconds. A
separate run supplied the arm64e text of `/usr/bin/wc` to the optional
classifier test. Both hosted Apple M1 virtual machines qualified native PAC,
authenticated calls and signed language unwinding. The strict PAuth_LR,
PACM-PC and CPTA-on hardware checks reported unavailable.
[Source, image hashes and per-host measurements](evidence/arm64e-inferred-data-2026-11-20.json).

### Literal loads crossing Mach-O data-in-code records

An AArch64 literal load can span several consecutive records or a gap in
the source table. The publisher walks the records intersecting the decoded
read and keeps their original extents. It fills only the bytes within the
read that lack a record. The installer requires complete coverage of the
read in the resulting layout. A conflicting record kind invalidates the read,
including a conflict confined to the high half of a vector.

The signed fixture marks two bytes in the middle of a 64-bit integer, leaves
a gap inside a double, and describes a 128-bit vector with two adjacent
eight-byte records. The comparison program reads all six integer and SIMD
literal forms and compares both halves of the vector. The test checks the
five source records, four recovered extents and the bytes and types in three
successive publications.
