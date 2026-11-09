# Native arm64e validation

The [2026-11-09 native execution record](evidence/arm64e-native-installation-2026-11-09.json)
records twelve successful installed-image cases on each of the macOS 26 and
Xcode 27 runners, with zero failures or skips. It identifies the locally built
Release test host, exact source revision, binary/fixture hashes and each
case's PAC, compiler-original, migration and unwind properties. The execution
hosts compile their own replacement objects and controls. This qualifies the
documented dylib installation matrix; whole-image rebuilding, startup loading
and the post-r6 LLVM MC interface remain separate publication requirements.

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
checks the twelve registered CTest commands before execution, records the owning
test binary hash and runs with the registered CTest deadlines. Every chained and
threaded-input O0/O2 case must execute successfully: authenticated calls and signed
compact unwind, native DWARF catches/cleanup, and relocated original signed
language frames. Missing, duplicate, foreign, failed or skipped cases fail the
qualification even if CTest itself returns success. Temporary fixture storage
is private to the invocation and is removed afterward.

`native_installer_call_unwind_qualified` covers this native installation matrix.
General whole-image publication keeps its separate false qualification field;
neither this profile nor the controlled FVP call matrix establishes that broader
contract. Keep the configured LLVM r6 package until a separately qualified fork
release replaces it.

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
requires the post-r6 MC producer and complete rebuild/signing transaction.

The [separate startup run](https://github.com/gmh5225/test_mac_intel/actions/runs/42646758724)
passed both PAC controls and all four `dlopen` controls on each native host.
Both startup probes failed with `SIGTRAP` at the first O0 compact
compiler-original executable. Those are failed startup prerequisites; neither
job reports success. The report and product manifests retain the failure and
confirm removal of the owned executables.

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
