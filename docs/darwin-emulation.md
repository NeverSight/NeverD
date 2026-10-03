# macOS and iOS guest process environments

NeverD's Darwin environments run bounded freestanding Mach-O processes. They
are guest OS models under `lib/emulation/os/darwin/`, separate from the host
CPU transport. Enable `NEVERD_ENABLE_CPU_EMULATION`; Windows driver emulation
is not required.

| Profile | Mach-O platform | Guest architectures | OS page size |
| --- | --- | --- | --- |
| `macos-macho64-v1` | macOS | x86-64, baseline ARM64 | 4 KiB x64; 16 KiB ARM64 |
| `ios-macho64-v1` | iOS device | baseline ARM64 | 16 KiB |
| `ios-simulator-macho64-v1` | iOS Simulator | x86-64, baseline ARM64 | 4 KiB x64; 16 KiB ARM64 |

Platform selection is explicit. An iOS device binary is not a simulator image,
and macOS host execution does not change the guest's profile. Matching macOS
host/guest ISAs can use [HVF](macos-hvf.md); cross-ISA execution uses Unicorn
under `auto`. The CPU's mapping granule remains 4 KiB even when the guest OS
requires 16 KiB alignment and allocation units.

```sh
neverd emulate guest.macho --profile=ios-macho64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"]}'
```

The existing [process C, Python and CLI APIs](process-emulation.md#cli-and-sdk)
share this implementation and the same limits and report schema. Returning
Darwin services add an `error` Boolean to their records: a positive errno in
`result` with `error=true` represents BSD carry, not a Linux negative result.
Nonreturning or unsupported requests have no result or error field.

## Image and startup contract

`MachOExecutionImage` uses LLVM's Mach-O parser and the shared loader entry
reader. It preserves original file bytes without analysis relocation patches.
Only thin little-endian `MH_EXECUTE` images with one unambiguous platform and
entry are admitted. Universal images require an explicitly extracted slice;
the execution loader does not choose one from the host.

The complete input file, including unmapped metadata and trailing bytes, must
fit `memory_limit` before parsing or copying. The loader reads a bounded private
snapshot from a regular file and rejects embedded-NUL paths, short reads and
size changes. It does not parse through a live file mapping. This input limit
and the mapped guest-memory limit are separate ceilings with the same value;
host filesystem I/O has no hard wall-clock guarantee.

Segments retain their permissions, zero-fill and maximum protection.
`__PAGEZERO` reserves addresses without allocating its often multi-gigabyte
extent. File and virtual ranges, OS-page alignment, rounded overlap, header
ownership, executable entry and memory budget are checked before the private
address space becomes executable. The Mach header's segment must be readable
and executable. A final file page retains its original bytes through the page
boundary (or EOF); subsequent complete VM pages are zeroed, following XNU's
[Mach-O loader](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/mach_loader.c).
Guard pages and the private main-return gate remain reserved.

`LC_MAIN` receives `argc`, `argv`, `envp` and the apple vector as four integer
arguments under the platform's scalar entry convention. A return produces the
low eight-bit process exit status. The bounded startup model accepts the
standard `/usr/lib/dyld` command only for import-free `LC_MAIN`; it implements
that entry handoff without loading or running host dyld. Nonzero `stacksize`
requests are rejected, since the caller's explicit `stack_size` owns the budget.

`LC_UNIXTHREAD` accepts exactly one complete native 64-bit general-register
record with only its PC populated. The initial stack contains argc, terminated
argv, terminated envp, and a terminated apple vector containing
`executable_path=<input filename>`. Custom SP, flags, other register state,
extra flavors and conflicting entries are rejected. No host environment or
auxiliary Linux vector is inherited. Startup follows the distinction described
in Apple's [dyld architecture](https://github.com/apple-oss-distributions/dyld/blob/main/doc/dyld4.md).

External dylibs, imports, rebases/chained fixups, constructors/destructors,
thread-local sections, arm64e/PAC and other nonbaseline CPU subtypes,
encrypted payloads and unmodeled load commands fail before execution. PIE
images without fixups use their preferred addresses; this is deterministic
placement, not ASLR. Code-signing blobs are metadata, not an implementation
of AMFI or entitlement policy.

## Darwin services

ARM64 uses X16, X0–X5 and `svc #0x80`; x64 uses the BSD class `0x02000000`
with RAX and RDI/RSI/RDX/R10/R8/R9. Successful calls clear carry and supply the
documented scalar result; errors set carry and return positive errno. ARM64
clears X1; x64 clears RDX on success and preserves it on error. X64 SYSCALL
return clobbers are explicit. These rules come from the XNU
[ARM64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/arm/systemcalls.c)
and [x64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/i386/systemcalls.c)
entry paths; no Apple implementation code is incorporated.

The service inventory is `exit`, `write`, `getpid`, `getppid`, `getuid`,
`geteuid`, `getgid`, `getegid`, `mmap`, `mprotect` and `munmap`. PID, UID and
GID are deterministically 1000, and parent PID is 1. Output descriptors 1 and
2 are captured byte sinks, including NUL and non-UTF8 bytes; other descriptors
return EBADF. A partial readable prefix is captured, but a subsequent copy
fault retains EFAULT, consistent with XNU's
[write error propagation](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/sys_generic.c).
The raw write length is limited to `INT_MAX`; larger requests return EINVAL
before descriptor lookup, guest-pointer access or output-budget admission.

Memory services support private anonymous data mappings with descriptor -1
and offset zero (`flags=0x1002`). Lengths and nonfixed hints round up to the OS
page size; an occupied hint searches upward before falling back. The raw
legacy zero-length mmap succeeds with address zero without allocating;
`MAP_UNIX03` is outside this profile. Unmap/protect addresses must be aligned.
NONE, READ and WRITE protections are supported, and WRITE
implies READ. Physical owners are per OS page, so partial unmap releases its
budget and subsequent mappings are zeroed. Maximum protections are distinct
from current rights. A failed protect across a hole or maximum-rights boundary
leaves the complete range unchanged. The rules are grounded in XNU's
[BSD VM services](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/kern_mman.c).

File/shared/fixed/JIT mappings, executable anonymous mappings, Mach traps,
indirect system calls, threads, signals, filesystem/network services, dyld
linking, Objective-C/Swift runtime and Foundation/UIKit are outside this
profile. They stop explicitly. This is neither a full Apple OS compatibility
layer nor the iOS Simulator application.

## Verification

`NeverDDarwinProcessTests` builds original freestanding C fixtures with Clang
and `ld64.lld`, without an Apple SDK or proprietary binary. It exercises all
five platform/ISA combinations through Unicorn and available native transports.
Independent Mach-O records test direct thread entry and malformed metadata;
memory tests exercise 4 KiB and 16 KiB behavior, including partial initial
stack/data unmap under a full physical budget. `NeverDProcessPublicTests`
checks C API/CLI report parity for every profile and ISA. Python's
`test_process_integration.py` covers the same five Mach-O fixture combinations
when `NEVERD_TEST_LIBNEVERD` and `NEVERD_TEST_DARWIN_FIXTURES` are configured.

```sh
cmake --build build-hvf --target NeverDDarwinProcessTests NeverDProcessPublicTests --parallel 8
ctest --test-dir build-hvf -L '^(NeverDDarwinProcessTests|NeverDProcessPublicTests)$' --output-on-failure
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/native-evidence --require-hvf
```

The native gate requires matching HVF cases to execute, including Darwin
fixtures; missing `ld64.lld` cannot turn the required suite into a skip. Each
transport must be verified on its own host; the results below distinguish
Apple Silicon HVF, Linux KVM and Windows WHP. Intel HVF has passed its ten-case native transport gate; complete CPU and
Darwin acceptance remains pending. See the
[HVF validation record](macos-hvf.md#implementation-validation-2026-10-02-to-2026-10-03).

The focused workload gate additionally requires **every** Darwin process case
on each platform supported by the host ISA: 39 cases on ARM64, or 26 on x64.
Both `LC_MAIN` and independent raw `LC_UNIXTHREAD` programs are required on
every supported platform. A source-inventory regression ensures each new
Darwin process test joins this required set.
On a native macOS build, `DarwinNativeTests.cpp` also runs the same authored
object against the host kernel. A separate host executable links libSystem
only for dyld's real main handoff; guest images remain import-free. Return,
exit, memory protection/reuse and oversized-write error ordering must match
the fixture's exit status and exact output. This native reference is required
by the HVF gate; it does not establish native iOS execution.
The independent [native kernel workflow](../.github/workflows/darwin-kernel-reference.yml)
runs these same cases on both Intel and Apple Silicon macOS without building
NeverD or LLVM. `DarwinNativeCases.def` owns the modes and expected byte output
for both runners. Wrong host architectures, Rosetta, timeouts and any result
mismatch fail the reference gate; its JSON records the source, OS and compiler.
Run it locally with `python3 scripts/run_darwin_kernel_reference.py
--architecture arm64 --evidence build-kernel-reference` (use `x86_64` on Intel).
The focused workload gate preserves the full inventory and JUnit results, and fails on missing or
skipped native workloads even when loader-only tests pass:

```sh
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/darwin-workload-evidence --require-darwin-backend hvf
```

Use `kvm` on Linux or `whp` on Windows with the same command. The manual
[`Native Darwin workloads` workflow](../.github/workflows/darwin-native.yml)
builds without Unicorn and runs both x64 transports on hosted runners. It
also accepts a single `kvm` or `whp` selection for focused reruns. It
requires actual virtualization and `ld64.lld`; unavailable hardware or fixture
tools fail explicitly. The result validates the bounded guest OS model and
shared CPU contracts, not Intel HVF or native iOS hardware.

### Local verification, 2026-10-03

Release evidence on Apple M4 Max / macOS 15.6.1, source
`36e11ca8a3d80aecf585d3328018839ce7fdb989`:

| Check | Passed | Failed | Skipped | Required native cases |
| --- | ---: | ---: | ---: | ---: |
| Full HVF gate, Unicorn disabled, 20 owners | 834 | 0 | 5,903 | 13 / 13 |
| Every Darwin workload, Unicorn disabled | 65 | 0 | 221 | 39 / 39 |
| Darwin and public C API/CLI, with Unicorn | 138 | 0 | 156 | — |

The rows overlap and are not additive. Native summaries record clean source,
no missing registrations and no unexecuted required cases. Skips belong to
foreign architectures, unavailable transports or the disabled software backend.
The 65 Darwin checks include loader/VM tests, every ARM64 process workload and
the original host-kernel reference. With the later native-interruption inventory,
the full HVF gate requires 16 checks on ARM64 or 14 on Intel, including the native reference; the focused
Darwin gate separately requires all 39 or 26 process workloads. The later full
ARM64 gate at clean source `561ebf37b9eaaec08043ac5816b2e083ecccaf68` passed 841
checks, failed none, skipped 5,939 and executed all 16 required outcomes. Its
evidence is in `build-hvf-native/hvf-cancellation-full-evidence/`.

Evidence is under `build-hvf-native/hvf-release-evidence/`,
`build-hvf-native/darwin-release-evidence/` and
`build-hvf/verification/darwin-release-public.xml`. Regressions cover whole-file
admission including unmapped trailing bytes, direct thread entry on every
platform, oversized-write error ordering and formatted native-case inventories.
The native inventory/result/CI regression suite passed 106 tests. Capability,
localized-documentation, provenance and formatting checks also passed.

After later `dev` integration, clean source
`f4bf8dde5cbc33d18ce053cb0722a54047e5a2d0` repeated the independent ARM64
Darwin gate: 65 passed, zero failed, 221 skipped and all 39 required native
workloads executed. Evidence is in
`build-hvf-native/hvf-final-dev-darwin-evidence/`. This rerun validates the
Darwin owner; it does not claim a new complete CPU gate for unrelated Windows
process changes merged in the meantime.

Earlier integration checks exercised all five platform/ISA combinations through
the Python SDK. The packaged engine matched 18 no-Unicorn CLI reports across
all three ARM64 profiles, and the bundle passed dependency/signature checks for
186 Mach-O images plus Cocoa startup. With both HVF and Unicorn disabled, the
Darwin/HVF/configuration targets built and passed 38 checks, with 231 backend
cases skipped and no Hypervisor.framework linkage. Those are integration and
build-isolation checks, not additional native guest executions. The separate
[desktop GUI workflow](https://github.com/NeverSight/NeverD/actions/runs/37053518872)
passed on macOS, Windows and Ubuntu at `e078b129c`.

### Hosted native verification, 2026-10-03

Both x64 transports executed the latest focused Darwin workload gate with
Unicorn disabled, including the file-budget, direct-thread-entry and oversized
write regressions. All 26 required process cases across macOS and iOS Simulator
passed at `36e11ca8a3d80aecf585d3328018839ce7fdb989`:

| Host / transport | Passed | Failed | Skipped | Required native cases |
| --- | ---: | ---: | ---: | ---: |
| [Windows / WHP](https://github.com/NeverSight/NeverD/actions/runs/37062839703/job/111023370601) | 51 | 0 | 235 | 26 / 26 |
| [Ubuntu 24.04 / KVM](https://github.com/NeverSight/NeverD/actions/runs/37062839703/job/111023370857) | 51 | 0 | 235 | 26 / 26 |

The artifacts `darwin-native-whp-x64` and `darwin-native-kvm-x64` contain the
inventory, JUnit results, CTest log and source/host summary. Both summaries
record clean source trees and zero missing or unexecuted required cases. Skips
include the macOS-only kernel reference, foreign ISAs and other transports.
Downloaded artifact hashes were verified against GitHub's SHA-256 digests.

These runs establish the bounded Darwin model on the indicated transports.
Intel Mac HVF and broader backend CPU behavior require their own gates.

### Independent native macOS reference, 2026-10-03

At `e727d3eab7086063bb392444bd55014ac48d43c3`, the original programs passed
directly on both macOS 15.7.9 kernels, compiled with Apple Clang 17.0.0:

| Host architecture | Native workloads passed |
| --- | ---: |
| [Apple Silicon ARM64](https://github.com/NeverSight/NeverD/actions/runs/37064795867/job/111029848093) | 4 / 4 |
| [Intel x86-64](https://github.com/NeverSight/NeverD/actions/runs/37064795867/job/111029847805) | 4 / 4 |

Both clean-source summaries record exit status 37 for `return`, `exit`,
`memory` and `write-length`, with exact expected output and empty stderr.
The artifact digests were verified. This independently checks the original
workloads' kernel contracts on both ISAs; the emulated executions remain
covered by the separate HVF/KVM/WHP results above.
