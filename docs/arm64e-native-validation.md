# Native arm64e validation

The [2026-11-09 native execution record](evidence/arm64e-native-installation-2026-11-09.json)
records twelve successful installed-image cases on each of the macOS 26 and
Xcode 27 runners, with zero failures or skips. It identifies the locally built
Release test host, exact source revision, binary/fixture hashes and each
case's PAC, compiler-original, migration and unwind properties. The execution
hosts compile their own replacement objects and controls. This qualifies the
documented dylib installation matrix. Later sections record default startup
and the canonical-IR producer separately; arbitrary lifted-image rebuilding
and the post-r6 MC rewrite interface retain their own qualification boundary.

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
rebuilt image or the missing post-r6 MC publication path.

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
controls establish the producer correction; NeverD's generated publication
matrices remain a separate qualification.

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
