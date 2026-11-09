# Native arm64e validation

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
threaded O0/O2 case must execute successfully: authenticated calls and signed
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
