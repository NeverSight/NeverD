**Languages**: [English](process-emulation.md) | [简体中文](zh-CN/process-emulation.md) | [繁體中文](zh-TW/process-emulation.md) | [日本語](ja/process-emulation.md) | [한국어](ko/process-emulation.md) | [Français](fr/process-emulation.md) | [Deutsch](de/process-emulation.md) | [Español](es/process-emulation.md) | [Italiano](it/process-emulation.md) | [Русский](ru/process-emulation.md) | [العربية](ar/process-emulation.md)

# Guest process emulation

`neverd emulate` executes an image under an explicit guest OS profile. CPU
transport, image parsing, process entry and OS services have separate owners.
Build with `NEVERD_ENABLE_CPU_EMULATION=ON`; driver emulation also includes it.

The first profile, `linux-elf64-v1`, runs bounded little-endian x64 and AArch64
ELF `ET_EXEC` and self-relocating static `ET_DYN` programs at CPL3 or EL0.
It loads real ELF segments, constructs
the initial stack, resumes instruction quanta and handles explicit Linux
system-call requests. It is a freestanding process model, not a full Linux
distribution or a promise to run arbitrary libc binaries. Dynamic linking,
signals, threads, file systems and unsupported services fail
explicitly. The [Android native profile](android-native-emulation.md),
`android-aarch64-api28-v1`, separately supports bounded API 28 ARM64 shared-library
function calls and Bionic models. The Windows PE64 process profile is described
below. Android managed runtimes, Darwin and other kernel workloads remain separate work.

<!-- i18n-section: cli-sdk -->

## CLI and SDK

```bash
neverd emulate guest.elf --profile=linux-elf64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"],"instruction_limit":100000}'
```

Matching Linux hosts select KVM and matching Windows hosts select WHP. Other
host/guest ISA combinations use Unicorn. An unavailable selected backend is an
error, with no silent fallback. An ELF guest still uses the Linux process model
when executed on Windows. See [CPU execution](cpu-execution.md) for the checked
instruction inventory, native availability and cancellation limitations.
Instruction support follows the selected checked CPU contract; vector register
storage does not imply unrestricted SIMD execution.

The CLI emits one JSON report. Its exit code is 0 for a guest exit status of
zero, 2 for another guest exit status, 3 for incomplete execution (including
faults and limits), and 1 for invalid setup/API failure. The actual guest status
is in `exit_status`; it is not substituted for the CLI exit code.

The additive C entry point is
[`neverd_emulate_process_json`](../include/neverd/sdk/NeverDCAPIProcess.h).
Pass a session, nonempty input path, explicit profile and optional options JSON.
Free its result with `neverd_free_string`. A null result is a setup failure;
read `neverd_last_error`. A guest fault or resource stop returns a report. The
session's loaded analysis image is neither required nor changed.

Python exposes the same native validation:

```python
report = session.emulate_process(
    "guest.elf", "linux-elf64-v1",
    '{"backend":"unicorn","arguments":["guest"],"environment":[]}',
)
output = bytes.fromhex(report["stdout_hex"])
```

<!-- i18n-section: options-results -->

## Options and results

Options are a JSON object of at most 64 KiB. Unknown fields, null field values,
invalid types, embedded NULs in strings and nonpositive limits are rejected.

| Option | Default | Contract |
|--------|---------|----------|
| `backend` | `auto` | `auto`, `unicorn`, `kvm` or `whp` |
| `arguments` | Input filename | Complete argv, including argv[0]; empty selects the default |
| `environment` | `[]` | Explicit guest strings; never inherits the host environment |
| `instruction_limit` | 100000 | Shared admitted instruction attempts |
| `event_limit` | 10000 | System-call events; charged before OS service handling |
| `timeout_microseconds` | 5000000 | One monotonic deadline starting after process setup |
| `memory_limit` | 67108864 | Physical/mapped memory budget |
| `stack_size` | 1048576 | Page-aligned stack within the memory budget |
| `output_limit` | 1048576 | Combined captured stdout/stderr bytes |
| `instruction_quantum` | 1024 | Admission interval before yielding to the runtime |

`schema_version` is 1. Results include profile, architecture, selected backend
and its selection reason, `stop_reason`, nullable `exit_status`, diagnostic,
entry/current PC, instruction/event counters, service records and the last
typed CPU exit. Addresses, syscall numbers, argument registers and raw return
bits are hexadecimal strings **without** a `0x` prefix; they never lose bits
through a JSON floating-point consumer. `stdout_hex` and `stderr_hex` are
lowercase byte encodings, preserving NUL and invalid UTF-8. A null syscall
result denotes no modeled return, including process exit or an unsupported
request; it is distinct from a successful zero return.

<!-- i18n-section: linux-semantics -->

## Linux profile semantics

The existing ELF loader supplies decoded program headers. OS policy validates
ABI tags, segment alignment, mapped program-header tables and user-address
bounds before execution. A generic mapping plan checks extents, permissions,
overlap and budget before allocation; the model publishes only a fully prepared
private address space. Linux mappings preserve file-page prefix/tail bytes,
zero BSS, honor segment permissions and reserve stack guard gaps. Page-overlap
layouts and contradictory headers are rejected rather than guessed.

Static PIE uses a deterministic load bias of at least `0x40000000`, increased
to honor larger `PT_LOAD` alignment. Every mapped segment, entry PC and
`AT_PHDR`/`AT_ENTRY` uses that same bias; the file's program-header values stay
unmodified. `AT_BASE` remains zero because no interpreter is loaded. This is a
reproducible placement policy, not Linux ASLR. Startup follows the direct-entry
path of the [Linux ELF loader](https://github.com/torvalds/linux/blob/v6.8/fs/binfmt_elf.c).

The mapping source is explicitly the original file, so analysis-time pointer
fixups cannot leak into guest execution. Guest startup must perform its own
relocations and initialization. The loader decodes `PT_DYNAMIC` from bounded
original file records, independently of optional sections; the process model
requires a readable, terminated table of at most 4096 entries when present. `PT_INTERP`
and external dependency/filter/audit tags are rejected. The model does not
silently provide a dynamic linker, symbol resolver or constructor runner.

The initial stack contains aligned argc/argv/envp/auxv, mapped PHDR/PHENT/PHNUM,
entry/page-size values and identity entries. Model PID/TID/UID/GID are 1000.
`AT_RANDOM` contains the first 16 bytes of the input's SHA-256 for reproducible
execution; this is explicitly a deterministic model policy, not cryptographic
entropy. HWCAP/HWCAP2 are zero; there is no vDSO. Startup conventions follow the
[Linux ELF loader](https://github.com/torvalds/linux/blob/master/fs/binfmt_elf.c).

Implemented calls are `write`, `exit`, `exit_group`, `getpid`, `gettid`,
`mmap`, `mprotect`, `munmap` and `brk`, with
separate [x64](https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl)
and [asm-generic ARM64](https://github.com/torvalds/linux/blob/master/include/uapi/asm-generic/unistd.h)
numbers. Returning x64 SYSCALL applies its RCX/R11 clobbers as well as RAX and
the next PC. ARM64 uses x8 for the number and x0 for the result. Unknown calls
stop as `unsupported_service`; they never execute host syscalls.

Static ELF TLS templates (`PT_TLS`) are validated as loader-owned facts, with
one template, bounded file/memory extents, alignment congruence and readable
initialized bytes. Guest startup allocates and initializes each TLS block and
installs its thread pointer; the Linux model does not invent a libc-specific
TCB or DTV. This permits compiler-generated local-exec TLS in freestanding
programs. Dynamic TLS, a dynamic linker and OS thread creation remain separate
work.

On x64, `arch_prctl` supports `ARCH_SET_FS`, `ARCH_GET_FS`, `ARCH_SET_GS` and
`ARCH_GET_GS`. Set accepts an unmapped user-range base; a later dereference still
checks user permissions. Kernel-range bases return guest `EPERM`; invalid get
destinations return guest `EFAULT` without faulting the CPU. Other operations
stop as unsupported, never as a success stub. The behavior follows the
[Linux arch_prctl implementation](https://github.com/torvalds/linux/blob/master/arch/x86/kernel/process_64.c).
ARM64 startup installs `TPIDR_EL0` with the architecturally unprivileged `MSR`
instruction. The corresponding `MRS`, x64 FS/GS memory accesses, and CPU context
restoration preserve each thread pointer across execution quanta and backend
entries. This does not itself implement a thread scheduler.

Descriptors 1 and 2 are virtual byte sinks. `write` validates readable user
pages, returns a readable prefix when a later page is inaccessible, and returns
guest `EFAULT` when no bytes are readable. A bad descriptor returns `EBADF`;
a zero-count write on a valid descriptor succeeds without pointer access.
These sinks do not model Linux pipe atomicity or file objects. Native fixture
comparison agrees on ordinary output and on partial writes to regular files;
Linux pipes can reject that same small cross-page write completely. Output
limits stop before publishing an over-budget write.

Anonymous memory services use the same process address space and physical
memory budget as the loaded image and stack. `mmap` accepts exactly
`MAP_PRIVATE | MAP_ANONYMOUS`, with ordinary `PROT_NONE`, `PROT_READ`,
`PROT_READ | PROT_WRITE`, `PROT_READ | PROT_EXEC` and readable RWX permissions.
Free, page-aligned hints are honored; otherwise placement searches gaps from
`0x100000000`, then from the minimum user address, reserving the stack guards.
This deterministic policy does not emulate Linux ASLR. New anonymous pages
are zero-filled and independently owned, so partial unmapping can reclaim
unpinned page allocations. A CPU projection or retained backing view may keep
a retired allocation alive until its own lifetime ends.

Lengths round to pages. `munmap` tolerates holes and repeated removal;
`mprotect` changes the mapped prefix before returning `ENOMEM` at a hole.
`PROT_NONE` preserves the allocation and bytes while denying guest access.
The raw `brk` call returns the requested byte boundary on success and the old
boundary on failure; it is not the libc wrapper's zero/minus-one convention.
The initial break is the page-aligned image end. Growth respects other mappings
and the memory budget; shrinking preserves bytes in its remaining partial page.
Rules and error precedence for the supported subset follow the Linux
[mapping](https://github.com/torvalds/linux/blob/v6.8/mm/mmap.c) and
[protection](https://github.com/torvalds/linux/blob/v6.8/mm/mprotect.c) services.

File/shared/fixed mappings, grow-down, huge pages, memory locking, protection
keys, execute-only/write-only policy and other flags remain explicit unsupported
service contracts. They stop before publishing effects or inventing a syscall
return. Ordinary range/length/alignment errors within the admitted subset return
guest errors and allow execution to continue. No memory service forwards a
guest pointer or mapping request to the host OS.

<a id="windows-pe64-profile"></a>

<!-- i18n-section: windows-pe64 -->

## Windows PE64 profile

`windows-pe64-v1` supports bounded Windows x64/ARM64 console processes with PEB/TEB, executable TLS, named Win32 APIs and explicit acyclic startup DLL graphs. Guest DLLs support named/ordinal code and data imports, DIR64 rebasing and actual loader-list identities. DLL entry points/TLS, dynamic loading, forwarded exports, CRT/GUI, user SEH and threads remain unfinished; native ARM64 KVM/WHP evidence is still pending.

Windows virtual memory adds `VirtualAlloc`, `VirtualFree`, `VirtualProtect`, `VirtualQuery` and current-process `FlushInstructionCache`. The OS layer owns reservations; `AddressSpace` remains the authority for committed pages, permissions and backing. Tests cover dynamic code rewriting, access faults and memory-budget reuse.

Private allocations support `MEM_RESERVE`, `MEM_COMMIT`, `MEM_DECOMMIT`, `MEM_RELEASE` and `MEM_TOP_DOWN`, with 64 KiB reservation alignment and 4 KiB pages. Reserve-only regions consume no guest RAM. Recommit preserves bytes and updates protection; decommit returns individual page backing. Whole-range validation and staged allocation prevent partial changes on ordinary allocation/protection failures. Query returns the 48-byte x64/ARM64 memory-information layout and coalesces forward within one allocation. The initial image, environment, heap arena, API gates and stack margins participate in placement. Stack allocation identity agrees with TEB. If a successful `VirtualProtect` makes its old-protection output read-only, the new protection remains installed, the output bytes stay unchanged, and the call still succeeds. An uncommitted-range protection failure returns `ERROR_INVALID_ADDRESS`, writes `PAGE_NOACCESS` to the old-protection output and leaves page permissions unchanged.

Supported protections are `PAGE_NOACCESS`, `PAGE_READONLY`, `PAGE_READWRITE`, `PAGE_EXECUTE_READ` and `PAGE_EXECUTE_READWRITE`. Guard pages, execute-only and copy-on-write policies, cache modifiers, large pages, reset/write-watch/placeholders and modification of model-owned runtime mappings remain explicit unsupported operations. Only private virtual allocations can be decommitted or released. This does not add user exception dispatch or native ARM64 hardware evidence.

[VirtualAlloc](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc), [VirtualFree](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualfree), [VirtualProtect](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualprotect), [VirtualQuery](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualquery), [MEMORY_BASIC_INFORMATION](https://learn.microsoft.com/windows/win32/api/winnt/ns-winnt-memory_basic_information).

```bash
neverd emulate guest.exe --profile=windows-pe64-v1 \
  --options='{"backend":"auto","arguments":["guest.exe","argument"],"environment":["MODE=test"]}'
```

The single-thread profile keeps the PE32+ EXE at its preferred base and admits explicitly supplied DLLs with zero entry point and no TLS directory. `WindowsProcessOptions::Modules`, or JSON `windows.modules`, supplies up to 64 guest basenames and host input paths through `name` and `path`; there is no host DLL search or execution. ASCII names compare without case; duplicate names and overrides of modeled system providers fail. Only reachable files are read. Named/ordinal function and data imports bind to actual mapped exports; holes, missing symbols, cycles, forwarding, bound/delay imports and unsupported load configuration/CFG fail explicitly. DIR64 records rebase colliding movable DLLs; fixed collisions and relocation writes into linker metadata fail before CPU creation.

`readPEProgramExports` owns original export identities and bounded metadata footprints. `WindowsProcessModules` owns the guest graph and one exact provider/name API gate per process. `VirtualMemory` reserves every image before mapping; `AddressSpace` owns pages and permissions. PEB/LDR lists contain real images, with DLLs in dependency order in the initialization list. `GetModuleHandleW` accepts NULL or ASCII basenames, compares without case, and appends `.dll` when no extension is supplied; paths, non-ASCII lookup and trailing-dot rules remain unsupported. Missing names return error 126; success preserves LastError. API models are not installed system DLLs.

Input-file bytes and aggregate image extents each share `memory_limit`; runtime mappings also consume the image budget. Preparation shares a 65,536-record and 64 MiB metadata-read allowance, bounded names and the workload deadline. Blocking host I/O has no hard time guarantee. The original EXE→DLL→DLL fixture checks rebased pointers, ordinal calls, shared data, API pointer identity, `MEM_IMAGE`, loader lists and executable TLS attach/detach. `NeverDWindowsProcessTests` owns these checks and the direct native Windows oracle; `NeverDPEProgramExportsTests` checks malformed metadata and work accounting, and `NeverDProcessPublicTests` checks C ABI/CLI catalogue parity. Unavailable transports are explicit skips.

```json
{"windows":{"modules":[{"name":"middle.dll","path":"inputs/middle.dll"},{"name":"leaf.dll","path":"inputs/leaf.dll"}]}}
```

GS on x64 and x18 on ARM64 point to TEB. Supported state includes stack bounds, self pointer, PID/TID, PEB, process parameters, LastError and TLS. Inputs are strict UTF-8 converted to UTF-16; argv is quoted for Microsoft CRT parsing. Environment names are ASCII, case-insensitive duplicates are rejected, values may be Unicode, and the sorted environment is double-NUL terminated. No host environment or filesystem is inherited. Static TLS copies its template, zeroes BSS and writes a 32-bit index; dynamic TLS uses separate TEB slots. Attach and detach read live callback arrays in order, with all instructions and named calls sharing one deadline and resource account. Entry return and normal process exit both run detach callbacks; reentrant exit during detach stops explicitly.

The exact API inventory is `WindowsProcessServices.def`: `ExitProcess`, `RtlExitUserProcess`, standard-output handles and synchronous `WriteFile`, LastError, process/thread identifiers and pseudo-handles, `GetCommandLineW`, process heap allocation/free/size, dynamic TLS and `GetModuleHandleW`. Provider names are restricted to `kernel32.dll`, `kernelbase.dll` and `ntdll.dll` with exact export identity. Direct syscalls and forged callback gates cannot select API models. Heap backing is owned by the process and reclaimed on free. Writes capture binary bytes; Win32 argument errors remain distinct from unsupported asynchronous I/O or user exception dispatch. Pointer aliasing observes the initial completion-count write and the live call-return slot.

The `windows.native_calls` report preserves module/function names, declared scalar arguments and nullable result bits. It does not invent native NT syscall numbers. `NeverDWindowsProcessTests` covers real x64/ARM64 PE startup, compiler TLS, live callback changes, heap/LastError, aliasing, invalid metadata, privilege faults and limits across available backends. `NeverDProcessPublicTests` checks the same PE through CLI/C ABI. Native Windows CI runs the original EXE as an independent behavioral oracle and requires WHP cases; native ARM64 runtime evidence still requires a suitable machine.

`WriteFile` with a nonempty unreadable input buffer returns `ERROR_INVALID_USER_BUFFER` (1784), zeros the completion count and publishes no bytes.

[PE/COFF](https://learn.microsoft.com/windows/win32/debug/pe-format), [ARM64 ABI](https://learn.microsoft.com/cpp/build/arm64-windows-abi-conventions), [WriteFile](https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-writefile), [TLS](https://learn.microsoft.com/windows/win32/api/processthreadsapi/nf-processthreadsapi-tlsgetvalue), [Wine 10.0 loader](https://github.com/wine-mirror/wine/blob/wine-10.0/dlls/ntdll/loader.c).

<!-- i18n-section: verification -->

## Verification

```bash
cmake --build build-cpu --target NeverDLinuxProcessTests NeverDExecutionSessionTests NeverDX64MemoryUpdateTests NeverDThreadPointerTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(LinuxProcess|ExecutionSession|X64MemoryUpdate|ThreadPointer)Tests$' --output-on-failure
# In a shared-library/CLI build:
cmake --build build-cpu --target NeverDProcessPublicTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDProcessPublicTests$' --output-on-failure
```

Tests compile original freestanding ELF entry assembly and C for both ISAs.
They exercise data/BSS, actual startup metadata, syscall errors, binary output,
permission faults, partial writes, unsupported services and budget preservation
across quanta. Local-exec TLS fixtures initialize independent aligned blocks,
zero TLS BSS, install thread pointers and check preserved values across switches.
On x64 they also verify `arch_prctl` errors without losing the previous base.
Backend cells report unavailable transports as skips. The public
suite traverses the shared C ABI and CLI and checks report/exit-code agreement.
The static PIE fixtures check relocated auxiliary values and raw zero RELA
slots before performing their own data/function-pointer relocations. Mapping
tests separately retain analysis fixups when that byte source is selected;
dynamic-table tests cover sections being absent and malformed/dependent input.
Anonymous-memory fixtures execute allocation, protection, holes, remapping,
heap growth/shrink and handled syscall errors on both ISAs. Separate actual
guest stores fault after ordinary and partial protection changes. The x64
fixture also rewrites code between RW and RX transitions and calls both
versions at the same address. The same x64 ELF runs natively on Linux as an
independent result/fault oracle. Pure memory tests cover budget exhaustion,
reclamation and authoritative mapping snapshots without retaining RAM.
Cross-compilation and Unicorn ARM64 results are not native ARM64 KVM/WHP evidence.
