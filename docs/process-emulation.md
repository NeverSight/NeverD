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
unsupported signal delivery, Linux process threads, general file systems and unsupported services fail
explicitly. The [Android native profile](android-native-emulation.md),
`android-aarch64-api28-v1`, separately supports bounded API 28 ARM64 shared-library
function calls and Bionic models. The Windows PE64 process profile is described
below. The [Darwin profiles](darwin-emulation.md) add bounded macOS, iOS device
and iOS Simulator Mach-O processes. Android managed runtimes and guest kernel
workloads remain separate work.

<!-- i18n-section: cli-sdk -->

## CLI and SDK

```bash
neverd emulate guest.elf --profile=linux-elf64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"],"instruction_limit":100000}'
```

Matching Linux hosts select KVM, matching Windows hosts select WHP, and
matching macOS hosts select HVF. Other
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

Options are a JSON object with at most 64 KiB of metadata, excluding two
hexadecimal characters per validated `android.dex_images` byte and per
validated `linux_files.files[].bytes_hex` byte. The total wire ceiling is
34880 KiB: up to 1 MiB of decoded DEX, up to 16 MiB of decoded Linux
regular-file bytes, and the metadata allowance. The existing aggregate
Linux file budget still charges paths, directories, links, devices and
write reservations, so regular-file content cannot consume that entire
16 MiB by itself. Paths, other binary fields, whitespace and JSON escape
overhead remain metadata. These transport allowances do not enlarge any
decoded field budget or grant access to host files.
Unknown fields, null field values,
invalid types, embedded NULs in strings and nonpositive limits are rejected.

| Option | Default | Contract |
|--------|---------|----------|
| `backend` | `auto` | `auto`, `unicorn`, `kvm`, `whp` or `hvf` |
| `arguments` | Input filename | Complete argv, including argv[0]; empty selects the default |
| `environment` | `[]` | Explicit guest strings; never inherits the host environment |
| `instruction_limit` | 100000 | Shared admitted instruction attempts |
| `event_limit` | 10000 | System-call events; charged before OS service handling |
| `timeout_microseconds` | 5000000 | One monotonic deadline starting after process setup |
| `memory_limit` | 67108864 | Physical/mapped memory budget |
| `stack_size` | 1048576 | Page-aligned stack within the memory budget |
| `output_limit` | 1048576 | Combined captured stdout/stderr bytes |
| `instruction_quantum` | 1024 | Admission interval before yielding to the runtime |
| `linux_network` | Absent | Explicit offline TCP attempts and byte streams; requires `linux_files`; see [contracts](linux-network-contracts.md) |
| `linux_time` | Absent | Explicit clock observations and optional idle advancement for Linux ELF64 and Android native workloads |
| `linux_files` | Absent | Explicit guest file catalogue, bounded write grants and optional live FD links |
| `linux_signals` | Absent | Explicit dispositions, initial thread mask and disabled alternate-stack state; bounded AArch64 handlers |
| `linux_priority` | Absent | Explicit per-task nice values and caller authority for raw Linux priority services |
| `linux_kernel` | Absent | Explicit released GKI branch, fixed guest task catalogue or observed absent guest kernel interfaces |

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

`writev` shares those sinks on x64/ARM64 and through Android Bionic. It imports up to 1024 guest `iovec` entries before output, rejects negative lengths with `EINVAL`, validates user ranges, and applies Linux’s page-aligned transfer cap. An invalid descriptor returns `EBADF` before vector access; inaccessible metadata returns `EFAULT` without output. A later payload fault preserves the copied prefix. The output budget covers the whole vector before publication, across both streams. `write` and `writev` use the low 32 descriptor bits; vector count also follows Linux’s 32-bit import. Bionic alone converts raw negative errors to `-1` and `errno`. `LinuxOutputNativeTests` runs ten original cases on host Linux with regular-file redirects; modeled x64/ARM64 cases also check budgets. Explicit GKI selection retains its released version's metadata and transfer-cap order; see [released GKI contracts](android-gki-kernels.md). See the [Linux vector import contract](https://github.com/torvalds/linux/blob/v6.12/lib/iov_iter.c).

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

Implemented calls are `write`, `writev`, `exit`, `exit_group`, `getpid`, `gettid`,
`getuid`, `geteuid`, `getgid`, `getegid`,
`mmap`, `mprotect`, `munmap`, `madvise`, `brk`, `gettimeofday`, `clock_gettime` and
bounded `rt_sigaction`, with
separate [x64](https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl)
and [asm-generic ARM64](https://github.com/torvalds/linux/blob/master/include/uapi/asm-generic/unistd.h)
numbers. Returning x64 SYSCALL applies its RCX/R11 clobbers as well as RAX and
the next PC. ARM64 uses x8 for the number and x0 for the result. Unknown calls
stop as `unsupported_service`; they never execute host syscalls.
Real/effective identity queries agree with the corresponding auxv entries.
They use the deterministic model identity above; credential-changing services
such as `setuid` remain unsupported.

The optional `linux_kernel` input records explicitly observed absent kernel
interfaces. For example, a fixture with no `pidfd_open` implementation uses:

```json
{"linux_kernel":{"unavailable_syscalls":["pidfd_open"]}}
```

The selected raw call returns -ENOSYS before argument validation, as a missing
kernel entry does, and creates no descriptor or guest-memory effect. Bionic's
`syscall` wrapper retains its normal -1/errno translation. Missing input and an
empty list retain the existing unsupported boundary for this interface;
other unknown calls are not converted to ENOSYS. Currently only `pidfd_open`
is admitted; unknown names, duplicates and wrong types are rejected. This
input does not infer a kernel version, host availability or a working pidfd
implementation. See the [kernel missing-call implementation](https://github.com/torvalds/linux/blob/master/kernel/sys_ni.c).

An explicit `linux_kernel.gki` selects a released Android common kernel branch
from 5.10 through 6.18. Its current implemented subset includes `pidfd_open`
for the live model process and explicitly catalogued guest tasks, versioned
vector import, observed process CPU clocks and extended-attribute name/target
errors, with the same descriptor table used by
`linux_files`; Bionic and raw traps share ownership and error ordering.
Selecting GKI together with an absent `pidfd_open` observation is rejected.
An optional `linux_kernel.tasks` array supplies a closed, fixed catalogue of
additional live tasks as `{ "id": 2000, "group_leader": true }` entries.
An omitted array leaves foreign target lookup unsupported; an empty array
observes no foreign tasks. A PID outside both the declared catalogue and the
local task owner returns ESRCH.
Entries and priority observations must be consistent. With cooperative Android
threads the fixed catalogue describes foreign tasks; it must not occupy any
identity reserved for child creation. Live local children come from the shared
thread owner and are rejected as nonleaders according to the selected GKI.
Exited children are absent from an explicit closed catalogue. Thread-bound
pidfds for local cooperative threads remain unsupported, including their exit
readiness; static foreign observations do not establish that lifetime.
Android API levels do not select a kernel. See the
[released GKI contracts](android-gki-kernels.md) for all eight source pins,
descriptor behavior, tests and the remaining kernel coverage.
`poll`/`ppoll` and the supported level-triggered epoll subset use the shared
`linux_files` descriptor owner without requiring GKI selection. They preserve
ordered `revents` output, duplicate identities and explicitly scheduled TCP/UDP
readiness; finite waits use virtual idle time. Observed live pidfds remain
unready, with no invented exit notification, and a closed descriptor reports
`POLLNVAL`. See [poll/epoll](linux-network-contracts.md#poll-and-epoll-waits)
for timeout, signal-mask, one-shot and unsupported-mode boundaries.

The optional `linux_priority` input declares nice state for fixture-owned tasks
with the caller's UID. Raw `setpriority` and `getpriority` share this state across
Linux ELF64 and Android native workloads; they never change host priorities.

```json
{"linux_priority":{"tasks":[{"id":1000,"nice":0}],
                   "cap_sys_nice":false,"rlimit_nice":0}}
```

Task IDs are distinct positive signed 32-bit values, initial nice values are
-20 through 19, and `rlimit_nice` is 0 through 40. `cap_sys_nice` and
`rlimit_nice` default to false and zero; task state is always explicit. Missing
input, unlisted tasks, and PRIO_PGRP/PRIO_USER selection stop with an unsupported
service. This includes newly created threads whose nice state was not declared;
the model does not guess inheritance or another task's ownership. PRIO_PROCESS
with who zero selects the current guest task, otherwise the named task.
Invalid selectors return raw -EINVAL. Set requests clamp their signed 32-bit
nice argument to -20..19. Lowering nice requires CAP_SYS_NICE or a sufficient
RLIMIT_NICE limit; denial returns raw -EACCES without changing state.
Raw get requests return `20 - nice`, preserving the kernel's 40..1 encoding;
this is not libc's translated `getpriority` result. See the
[Linux priority interface](https://man7.org/linux/man-pages/man2/setpriority.2.html).

`linux_signals` supplies process-wide initial kernel actions. A missing entry
is unknown; it does not imply `SIG_DFL`. An explicit empty action list enables
installing actions without observing an unknown predecessor. For example:

```json
{"linux_signals":{"actions":[
  {"signal":11,"handler":0,"flags":0,"restorer":0,"mask":0}
]}}
```

Signal numbers are 1–64; each entry requires all five fields. The four action
words are unsigned 64-bit values, with decimal strings for values beyond the
exact JSON integer range. Duplicate entries, unknown fields, blocked
SIGKILL/SIGSTOP bits, and nonzero actions for those two signals are rejected.
Inputs describe the kernel layout: handler, flags, restorer and eight-byte mask.
They are not read from the host or inferred from a selected native function.

`rt_sigaction` requires `sigsetsize == 8`, copies the new 32-byte LP64 record
before validating the signal, then observes/replaces the shared disposition.
It removes SIGKILL/SIGSTOP from new masks. Common Linux action flags are retained,
including Bionic's sign-extended 32-bit flags; unknown flag extensions stop.
An invalid old-output pointer returns EFAULT after a successful installation.
Mixed-access kernel copies stop before unmodeled partial bytes are written.
These ordered effects follow the [Linux signal implementation](https://android.googlesource.com/kernel/common/+/16a2d602244f/kernel/signal.c).

`kill` shares signed 32-bit PID/signal decoding across x64 and AArch64 raw
syscalls, Android imports and Bionic `syscall`. Invalid signal numbers return
EINVAL before target lookup. `kill(getpid(), 0)` succeeds without a disposition
snapshot; `kill(getpid(), SIGKILL)` terminates the whole guest workload with
`stop_reason: "signaled"` and `termination_signal: 9`. `exit_status` remains null
and the terminating call has no result. Bionic alone converts raw errno to
`-1`/thread-local errno; successful queries preserve errno. No host process is
looked up or signaled. Other PIDs and process-group selectors stop as unsupported.
Unknown targets are not reported as ESRCH. See the
[Linux kill contract](https://man7.org/linux/man-pages/man2/kill.2.html).

Standard self-process signals use one coalesced pending set and per-thread
blocked sets. Supply `initial_mask` explicitly (an unsigned 64-bit integer or
decimal string); an omitted mask remains unknown. New Android threads inherit
their creator's mask. Runnable threads receive eligible signals at guest
execution boundaries, with lower signal numbers selected first. `rt_sigprocmask`
uses an eight-byte set, captures input before output, filters SIGKILL/SIGSTOP,
and commits a valid mask change before a failed old-mask copy. `rt_sigpending`
copies the blocked pending subset, accepting output lengths from zero to eight.
Installing SIG_IGN, or SIG_DFL for SIGCHLD/SIGURG/SIGWINCH, discards pending
instances. Default terminating actions produce the same typed `signaled`
outcome as SIGKILL; core-dumping actions remain unsupported.

AArch64 handlers additionally require `alternate_stack_disabled: true`, meaning
the alternate stack is explicitly cleared and disabled. They execute original
guest instructions with a Linux `rt_sigframe` containing mutable `siginfo`,
`ucontext`, integer registers, NZCV and FPSIMD state. SA_SIGINFO, SA_NODEFER,
SA_RESETHAND and a supplied SA_RESTORER are supported. Otherwise a workload-local
RX mapping executes the real `rt_sigreturn` syscall. Return restores guest
frame bytes, including supported handler changes to PC, registers and mask;
there is no host callback that bypasses the handler. Frame padding is preserved.
Malformed frames and unsupported context extensions stop explicitly.
Nesting is bounded to 32 live frames.

Android API 28 adapters include `sigprocmask[64]`, `sigpending[64]`, `signal`,
and `sigemptyset`, `sigfillset`, `sigaddset`, `sigdelset`, `sigismember` (also their
64 variants). They share the Linux state while preserving Bionic's reserved
signals, wrapper copy ordering and errno. `signal` installs SA_RESTART.

An eligible pending signal also makes an Android cooperative waiter schedulable.
Ignored signals leave its operation pending; default termination ends the
process without completing that operation. A caught signal interrupts a
relative `nanosleep` or `usleep` before its deadline, even with SA_RESTART.
The shared sleep owner copies remaining time before entering the handler and
supplies raw `-EINTR` (or `-EFAULT` for an inaccessible remainder). An already
expired sleep completes normally. Partially accessible output stops explicitly.
Bionic wrappers defer errno conversion until `rt_sigreturn`, so a handler sees
the previous errno. Nested interrupted wrappers retain separate continuations.
The resumed x0 is authoritative: handler edits affect the eventual wrapper
result. Redirecting ucontext PC past the synthetic import's post-trap return
boundary bypasses its adapter; the model preserves that redirect and leaves
the native-call event incomplete instead of inventing a normal return.

Caught signals also interrupt cooperative `poll/ppoll`, `epoll_wait/epoll_pwait`
and raw `pselect6`; SA_RESTART does not retry these operations. Poll copies zero
revents before returning EINTR, while pselect6 leaves the input sets unchanged.
Raw ppoll/pselect6 update their remaining timeout; Bionic ppoll preserves the
caller's const timespec. Epoll leaves event storage, rotation and one-shot state
unchanged. A wholly inaccessible poll output returns EFAULT before EINTR;
inaccessible timeout output is ignored, and partial-field access stops explicitly.
These rules follow [Linux poll/select](https://github.com/torvalds/linux/blob/v6.12/fs/select.c),
[epoll](https://github.com/torvalds/linux/blob/v6.12/fs/eventpoll.c) and
[API 28 wrappers](https://github.com/aosp-mirror/platform_bionic/blob/android-9.0.0_r1/libc/bionic/poll.cpp).

Direct socket read/recv/recvfrom waits have made no transfer before suspension.
Without SA_RESTART they return EINTR without consuming input or touching the
buffer/peer output. With SA_RESTART and no receive timeout captured on entry,
the wait is released and the original trap is restored in the signal frame.
After the handler, the guest re-executes that trap using its restored registers,
so changed arguments, descriptor reuse and PC redirection remain observable.
The interrupted attempt has no completed result; re-entry records a new attempt.
Modeled imports restart at their synthetic trap and expose their import argument
ABI, including the leading number for `syscall`. They do not manufacture the
internal registers/stack of a real libc implementation.
An entry-time SO_RCVTIMEO disables restart even if changed while suspended.
An already ready transfer completes before handler delivery, including short
reads. See [socket interruption](https://github.com/torvalds/linux/blob/v6.12/include/net/sock.h)
and [ARM64 restart](https://github.com/torvalds/linux/blob/v6.12/arch/arm64/kernel/signal.c).
An indefinite cooperative descriptor wait can be woken by another guest thread;
without a runnable owner, eligible signal or scheduled event it still stops
explicitly instead of consulting a host clock or transport.

Real-time queues, thread-directed sends, alternate stacks, job control,
synchronous fault delivery, core dumps, x64 handler frames and interruption
of synchronization/stdio waits remain unsupported. Temporary wait masks and
TCP WAITALL partial-progress restart are also outside this contract. A caught signal directed
to such a waiter stops explicitly. No host signals, devices or
network services participate. ABI references:
[Linux signal semantics](https://man7.org/linux/man-pages/man7/signal.7.html),
[AArch64 signal frames](https://github.com/torvalds/linux/blob/v6.1/arch/arm64/kernel/signal.c),
[AArch64 context layout](https://github.com/torvalds/linux/blob/v6.1/arch/arm64/include/uapi/asm/sigcontext.h),
[Bionic masks](https://android.googlesource.com/platform/bionic/+/android-9.0.0_r1/libc/bionic/sigprocmask.cpp)
and [Bionic signal helpers](https://android.googlesource.com/platform/bionic/+/android-9.0.0_r1/libc/bionic/signal.cpp).

`mincore` validates alignment, query range and numerical vector range in that
order (EINVAL, ENOMEM, EFAULT). The vector has one byte per rounded-up page.
Zero-length queries succeed; an unmapped first query page returns ENOMEM before
checking output mappings. These paths leave the vector and observations alone.

An optional `linux_kernel.residency_observations` array supplies ordered
`{ "address": "4294967296", "pages": [0, 1, 0] }` records. Addresses use the
shared unsigned decimal integer wire contract. Records describe query-time page
states, not initial RAM contents: the start and entire rounded query length must
match exactly. The bounded LP64 contract uses 4 KiB pages and admits 1,024
records, 65,536 pages per record and 1,048,576 observation bytes in total. Shared
JSON metadata limits still apply. Missing, exhausted or mismatched input stops
explicitly; neither host residency nor allocated NeverD RAM supplies a default.

Only live anonymous allocations made by the shared Linux `mmap`/`brk` owner
qualify, including PROT_NONE pages. Names alone never establish backing
provenance. Unmapping retires provenance, so an unrelated mapping at a reused
address cannot inherit it. Query output advances by complete live VMA chunks,
limited to one page of vector bytes. A later hole returns ENOMEM with completed
prefixes intact. An inaccessible first output byte returns EFAULT; an incomplete
chunk with a writable prefix remains unsupported because exact partial-copy
progress is architecture-specific. Device query backing and device output are
unsupported. Unrelated device mappings are not accessed. Observations are
consumed once a query reaches known backing, before output validation, including
EFAULT. Unknown first backing does not consume a record.

Raw Linux syscalls, Bionic `mincore`, Bionic `syscall` and dynamic imports use the
same state; Bionic alone translates kernel errors to errno. Temporary kernel
allocation failure (EAGAIN), host paging and unknown backing types are outside
this model. The ordering and chunking contract follows Android common kernel
4.4.115 `aa856bd83c43969dc3e8f8c0e97e6105e91aef76`,
[`mm/mincore.c`](https://android.googlesource.com/kernel/common/+/aa856bd83c43969dc3e8f8c0e97e6105e91aef76/mm/mincore.c).
This selects a bounded semantic contract, not a device kernel image.

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
a zero-count write still validates the user address range, but does not
require a mapped page or read any payload.
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

`madvise` admits `MADV_MERGEABLE` and `MADV_UNMERGEABLE`. It records KSM
eligibility over guest RAM, including read-only and `PROT_NONE` pages. The
deterministic profile keeps the background scanner stopped: advice does not
deduplicate backing allocations or change bytes or access permissions. Partial
revocation splits the eligible range; `munmap` and heap shrink retire the
removed policy, and new allocations start ineligible. Alignment and rounding
overflow return `EINVAL`; an aligned empty range succeeds. Holes return
`ENOMEM` while still applying advice to mapped portions on either side. The
behavior argument uses its low 32 bits, matching the Linux `int` ABI. These
rules follow Linux's [advice dispatch](https://github.com/torvalds/linux/blob/v6.8/mm/madvise.c)
and [KSM policy](https://github.com/torvalds/linux/blob/v6.8/mm/ksm.c).
`MADV_DONTNEED` additionally admits owned private anonymous mappings, with the
page-discard contract below. Other advice, including `MADV_FREE`, remains
unsupported; discarding content is not modeled as a content-preserving hint.

Explicit `linux_files.devices[].read_only_mappings` additionally admit
read-only shared device RAM snapshots, with shared backing across opens,
mapping lifetime independent of descriptor close and bounded permission
changes. See [device contracts](linux-device-contracts.md) for the supplied
page format and supported operations.

Ordinary file, anonymous shared and fixed mappings, grow-down, huge pages, memory locking, protection
keys, execute-only/write-only policy and other flags remain explicit unsupported
service contracts. They stop before publishing effects or inventing a syscall
return. Ordinary range/length/alignment errors within the admitted subset return
guest errors and allow execution to continue. No memory service forwards a
guest pointer or mapping request to the host OS.

<a id="windows-pe64-profile"></a>

**Private anonymous page discard.**

`madvise(MADV_DONTNEED)` shares the LinuxMemory implementation across raw LP64
syscalls and Android Bionic direct, syscall and dynamic-import routes. It admits
only private anonymous mmap and brk ranges allocated by that owner. Ownership
is recorded when pages are mapped, independently of optional VMA-name and
residency observations. Initial image/input RAM, file mappings, devices, locked
or huge pages and unmodeled advice values are not inferred from generic RAM.

The low 32 bits select the advice. Alignment, rounded length and unsigned end
overflow are checked before mapping lookup; a valid zero length returns zero.
Every covered page receives fresh zero backing with its previous permissions.
Virtual addresses, VMA names, program break and KSM eligibility are retained.
Previously pinned views and other physical aliases retain the original bytes;
this operation does not memset shared backing. AddressSpace remains the mapping
and generation authority, so CPU projections refresh before execution resumes.

Mapped pages on both sides of a hole are discarded before ENOMEM is returned.
Unknown backing stops explicitly at that range, preserving earlier completed
page effects. A temporary replacement allocation is prepared before removing
each old mapping. Exhausting the model's backing budget stops without claiming
a kernel allocation result or removing the current page; previous page effects
remain. Unexpected mapping failures are terminal. OS mutation ownership must
remain exclusive throughout the stopped operation.

This is the byte-visible contract for the restricted anonymous domain, based on
Android common 4.4.115 `mm/madvise.c` at
`aa856bd83c43969dc3e8f8c0e97e6105e91aef76`. It does not model lazy page faults,
RSS, swap or host reclamation. Mincore still requires its separate explicit
query-time observations; no residency bit is inferred from this host storage.

<!-- i18n-section: linux-clocks -->

## Explicit guest clocks

The optional `linux_time` input supplies observations, fixed by default, shared by raw
Linux services and Android Bionic. It never reads the host clock or supplies a
default epoch. Instruction time is frozen unless explicitly configured. For example:

```json
{"linux_time":{"clocks":[
  {"id":0,"seconds":"4294967297","nanoseconds":987654321},
  {"id":1,"seconds":123,"nanoseconds":456789}],
  "timezone":{"minutes_west":-60,"dst_time":0}}}
```

Clock IDs include Linux's static IDs 0–9 and 11: realtime, monotonic, process CPU,
thread CPU, monotonic raw, realtime coarse, monotonic coarse, boottime,
realtime alarm, boottime alarm and TAI. Each is independent; absent clocks are
unknown, including coarse variants. A selected released GKI contract or an
explicit `linux_kernel.tasks` catalogue also admits negative encoded process
CPU clock IDs with explicit live group observations;
see [released GKI clocks](android-gki-kernels.md#implemented-process-cpu-clock-subset).
PROF, VIRT and SCHED retain independent observations; aliases of the current
process share one observation. Duplicate identities,
unknown input IDs and more than 16384 clock observations are errors.
Seconds are signed 64-bit and must be nonnegative for CPU clocks;
nanoseconds must be in `[0, 1000000000)`. Integer
JSON numbers are accepted within `±9007199254740991`; decimal strings preserve
the full signed range. Timezone fields are signed 32-bit integers. C++ callers
use `ProcessOptions::LinuxTime`, with the same value validation. Other process
profiles reject this option.

`"advance_on_idle": true` explicitly enables relative `nanosleep` on x64 and
ARM64, including Android's named and variadic wrappers and scalar `usleep`.
Clock IDs 0, 1, 4 and 7 advance from their independent initial values by elapsed
virtual time. Monotonic raw (4) never borrows the epoch of monotonic (1).
Process CPU clock
observations may coexist with this policy and stay fixed during idle advancement.
Other clock inputs remain excluded from this policy.
A sleeping Android thread retains its original pending service;
other runnable threads execute first. When all threads are blocked, time advances
to the earliest sleep deadline. A single-threaded workload advances directly.
Without an instruction-time input, a busy runnable thread can leave a sleeper
pending until the workload budget expires.

`"nanoseconds_per_instruction":100` optionally assigns a deterministic virtual
wall-time rate to admitted guest instructions. Zero (the default) disables it.
The unsigned 64-bit rate may also be a decimal string. A workload-wide cumulative
instruction count is accounted exactly once at successful service and quantum
boundaries, across all Android threads. Realtime, monotonic, monotonic raw and
boottime share that elapsed time with idle advancement; supplied process CPU samples stay fixed.
Unsupported clock kinds are rejected just as for idle advancement. Multiplication,
elapsed-time and epoch overflow stop before changing any clock.

This rate is a declared analysis input, not measured hardware timing or CPU-time
accounting. It lets busy guest polling loops observe scheduled device-page changes
and lets sleepers wake while another guest thread runs. Updates remain quantized
by actual execution boundaries and `instruction_quantum`; reports should retain
the rate, quantum and producer trace. No host clock or sample PC participates.

The request is copied before checking nonnegative seconds and normalized
nanoseconds. Bad input addresses return `EFAULT`; invalid values return `EINVAL`.
Normal completion leaves the remaining-time pointer and Bionic errno untouched.
Zero duration returns without advancing. Saturated or overflowing deadlines and
clock overflow stop explicitly; no partial clock advancement is committed.
Relative sleep interruption in cooperative Android threads follows the signal
contract above; no SA_RESTART retry is applied. This deterministic policy excludes
absolute sleeps,
timer slack, CPU-time accounting and host scheduling. Disabled policy retains
an explicit unsupported-service stop for sleep.
The copy/validation/completion boundary follows Linux's
[relative nanosleep implementation](https://github.com/torvalds/linux/blob/v6.12/kernel/time/hrtimer.c);
Android's error conversion follows the API 28
[AArch64 Bionic wrapper](https://github.com/aosp-mirror/platform_bionic/blob/android-9.0.0_r1/libc/arch-arm64/syscalls/nanosleep.S).

`clock_gettime` consumes the low signed 32 bits of its clock ID, returns
`EINVAL` for invalid positive IDs before accessing the destination. Admitted
process CPU clocks validate their target before the destination; other encoded
and dynamic clocks stop explicitly. A known clock without input stops as
`unsupported_service`. `gettimeofday` writes seconds and truncated microseconds
as two ordered 64-bit fields, followed by the optional pair of 32-bit timezone
fields. `gettimeofday(NULL, NULL)` needs no input. Missing timezone input stops
after any completed timeval writes. The x64 `time` syscall returns realtime
seconds and optionally stores one 64-bit value; ARM64 has no such syscall.
These follow the Linux [time service](https://github.com/torvalds/linux/blob/v6.12/kernel/time/time.c)
and [clock dispatch](https://github.com/torvalds/linux/blob/v6.12/kernel/time/posix-timers.c)
contracts for the modeled subset.

Fully writable outputs, including unaligned ones, are supported. A wholly
inaccessible output returns `EFAULT`; earlier completed gettimeofday fields
remain written. Mixed accessibility within a single 8-byte store or 16-byte
timespec copy stops before that operation because architecture-specific partial
fault writes are not modeled. Adjustment, resolution queries, scheduling,
sleep and real device clocks remain unsupported.

<!-- i18n-section: linux-files -->

## Explicit memory files

`linux_files` supplies initial file bytes to Linux ELF64 and Android native
workloads. C++ uses `ProcessOptions::LinuxFiles`; C, Python and CLI share this
strict JSON contract:

Optional `devices` entries add [explicit descriptor-owned ioctl transactions and read-only mapping snapshots](linux-device-contracts.md)
in the same namespace. Device responses are declared inputs; regular file
bytes remain immutable unless granted a `write_limit`; device mappings are not implied.


Optional `symbolic_links` entries use the same canonical absolute namespace:
`{"path":"/links/item","target_hex":"2e2e2f746172676574"}`. Targets are
nonempty NUL-free bytes and share the catalogue's count and byte limits with
regular files. A link cannot also name a file or an ancestor directory.
Raw x64 `readlink`, x64/AArch64 `readlinkat`, Android imports and `dlsym` bindings
copy the observed target prefix without adding NUL. Absolute queries ignore
`dirfd`; the kernel's signed low-32-bit size check precedes pathname access.
Link queries preserve file cursors and do not resolve the target, including
relative or dangling targets. Traversal through links, link metadata,
relative queries and unknown descriptor-backed links stop explicitly; other
file operations cannot silently treat these links as missing regular files.
Mixed-permission output copies remain unsupported before any bytes are written.
The error ordering follows the [Linux readlink contract](https://github.com/torvalds/linux/blob/v6.12/fs/stat.c#L491-L540).

```json
{"linux_files":{"files":[
  {"path":"/fixture/data","bytes_hex":"00ff410a805a"}],
  "descriptor_limit":256}}
```

`files` is required and may be empty. Every entry requires `path` and
`bytes_hex`, with optional `metadata` and `write_limit`; empty bytes describe an empty file. Paths must be distinct,
absolute and canonical, with no NUL, empty, `.` or `..` components or trailing
slash. Paths have fewer than 4096 bytes and components at most 255 bytes.
A file cannot be another file's parent directory. The catalogue holds at most
256 files and 16 MiB total content and NUL-terminated path bytes. The existing
64 KiB JSON request ceiling also applies. `descriptor_limit` defaults to 256,
accepts 3–4096, and is the exclusive descriptor ceiling, including 0, 1 and 2.
Other process profiles reject these options before loading an image.

The catalogue is closed: absent paths return `ENOENT`. No host file, process
command line, environment or implicit `/proc` content is consulted. Without
`linux_files`, file services remain unsupported. Supplied bytes are explicit
workload facts; naming a proc path does not model procfs or its dynamic content.

Raw x64 `open`, x64/ARM64 `openat`, `read`, `close` and `lseek` share one
process-owned descriptor table. Android `open`, `open64`, `openat`, `openat64`,
`read`, `close`, `lseek`, `lseek64`, `fstat`, `fstat64` and `syscall` use that same state, including
across guest threads. Bionic alone maps kernel errors to `-1` and thread-local
`errno`; success preserves errno. Absolute paths ignore `dirfd`; mode is unused
without creation. Ordinary file opens admit `O_RDONLY`, optional `O_CLOEXEC`
and the architecture's `O_LARGEFILE`. `O_DIRECTORY` also admits known pathname
failures and requires the final object to be a directory. `O_DIRECT` admits
pathname and descriptor-exhaustion failures before reaching an opened file;
the catalogue does not infer its backing filesystem's direct-I/O support.
ARM64's directory/direct bits are `0x4000`/`0x10000`, while x64 uses
`0x10000`/`0x4000`. These stable source-pinned rules do not require GKI selection.
Unlisted flags, cwd-relative paths, symlink traversal and unsupported
descriptor-control operations stop explicitly. Repeated separators and
interior `.` components use the live namespace. `openat` also walks `.` and
`..` and resolves relative names from a live directory FD; file creation needs
the separate policy below. Exec is unmodeled, so close-on-exec flags have
no observable transition in this subset.

Guest queries accept canonical absolute names followed by one or more `/`
characters. Trailing separators require the final object to be a directory:
queries of a regular file with a trailing slash return `ENOTDIR`, and missing
names still return `ENOENT`. Paths consisting only of slashes name the root.
Directory opens are modeled; directory stat metadata needs the explicit
directory provider. This parsing does
not relax the canonical keys required by the catalogue configuration. The
directory requirement follows Linux
[pathname lookup](https://www.kernel.org/doc/html/latest/filesystems/path-lookup.html).

Existence queries use the same pathname import and closed catalogue. Raw x64
`access` (21), x64/ARM64 `faccessat` (269/48), and Android `access`/`faccessat`
support `F_OK`: files, their ancestor directories and `/` exist; missing paths
return `ENOENT`, and a file used as an ancestor returns `ENOTDIR`. The explicit
catalogue admits traversal of its directory prefixes; it supplies no general
directory permission model. Queries allocate no descriptor and preserve all
cursors, including when the descriptor table is full. Invalid low 32-bit mode
bits return `EINVAL` before pathname access. `R_OK`, `W_OK` and `X_OK` on an
existing entry remain unsupported: observed metadata does not establish
credentials, ACLs or mount policy. Raw `faccessat` consumes three arguments;
the [API 28 Bionic wrapper](https://github.com/aosp-mirror/platform_bionic/blob/android-9.0.0_r1/libc/bionic/faccessat.cpp)
rejects nonzero flags with `EINVAL` before entering the kernel. The kernel
validation order follows [Linux access](https://github.com/torvalds/linux/blob/v4.9/fs/open.c).

Each open starts an independent cursor and chooses the lowest unused
nonnegative descriptor. Exhaustion returns `EMFILE` after pathname import;
close releases the descriptor. Slots 0–2 initially reserve unknown stdin and
the existing captured stdout/stderr streams. Closing those slots enables
ordinary file reuse; writes through a closed slot or read-only file return
`EBADF`. Reading stdin remains unsupported; it is never assumed to be empty.
Descriptors, open flags and seek modes consume their low 32 bits.

**Directories and live FD links**

Catalogue parents and `/` are enumerable directories. `directories` optionally
adds canonical empty directory paths. They share the 256-entry catalogue limit.
`getdents64` (x64 217, AArch64 61) emits LP64 records with 8-byte alignment,
opaque continuation cookies, type, inode, name and NUL; padding remains
unchanged. `.` and `..` precede byte-sorted catalogue children. Supplied file
inodes are retained; other inodes are deterministic workload-local identities,
not captured host values. At most 8,448 such identities are admitted. A device
whose char/block type is unknown has `DT_UNKNOWN`. `dup` shares the directory
cursor; independent opens do not. Small buffers, EOF, invalid descriptors,
non-directories and wholly inaccessible outputs have explicit errno behavior;
mixed-permission record writes stop as unsupported. Directory reads return
`EISDIR`. Directory `SEEK_SET`/`SEEK_CUR` accepts nonnegative cookies.

`dynamic_proc_fds: true` reserves `/proc/self/fd` for live links and enumeration.
Opening, duplicating, replacing, closing and reusing an FD immediately changes
that namespace; no stale static link overrides it. Known file, directory and
device paths come from their actual open description. Standard captured streams
have no inferred tty/pipe pathname, so their readlink query stops explicitly.
`/proc/self` reports the modeled process ID. Conflicting catalogue entries are
rejected. The virtual FD table preallocates `descriptor_limit` rounded up to
64 slots: proc cookies include closed slots and reach that table boundary at
EOF. Rewind is required to revisit reused slots below the cursor. These rules
follow the kernel [record ABI](https://github.com/torvalds/linux/blob/v5.10/fs/readdir.c)
and [FD iteration](https://github.com/torvalds/linux/blob/v5.10/fs/proc/fd.c).
This option does not supply dynamic maps, task, status or fdinfo files, nor
PID aliases, proc mount metadata or general proc symlink traversal. The separate
[`dynamic_proc` contract](linux-live-proc.md) supplies maps, live tasks, supported
status fields and fdinfo, including process/thread-self aliases.

**Bounded writes to existing files**

An entry's `write_limit` grants mutable content with that maximum extent.
The full reservation counts against the 16 MiB file budget. The grant does
not assert real DAC/SELinux permissions or disk capacity. `O_WRONLY`, `O_RDWR`,
`O_TRUNC` and `O_APPEND` use one mutable object per file identity, shared by
all opens; cursor and flags remain per open description. `dup` shares both.
`write` overwrites, zero-fills holes, preserves readable-page prefixes on a
partial input fault, and chooses the current object end for append. Kernel
validation checks the original signed cursor and count before append selects
EOF, following the [VFS write contract](https://github.com/torvalds/linux/blob/v5.10/fs/read_write.c).
Reads and newly opened descriptors observe the resulting bytes. Missing write
grants and exhausted reservations stop explicitly; they are not fabricated
EACCES/ENOSPC results. `O_CREAT` can open an existing entry; `O_EXCL` returns
EEXIST before truncation. Creating absent objects requires the separate grant
below. `O_CREAT | O_DIRECTORY` also stops
explicitly because its behavior depends on the Linux release.

Without an explicit `track_write_metadata` provider, any write or truncation
invalidates captured stat observations. A later stat
stops rather than returning stale size, block counts or timestamps. The known
filesystem block size remains usable for stdio buffer selection. Vectored
mutable-file writes, regular file mappings, truncate syscalls and general
permission changes remain unmodeled. An explicit
[metadata provider](linux-live-proc.md#metadata-after-regular-file-writes) can
maintain complete size, sparse allocation and virtual timestamps after writes.
Captured stdout/stderr keep their existing bounded output semantics.

**Explicit file lifecycle**

Three independent `linux_files` grants enable bounded namespace changes:

| Field | Input and admitted operation |
| --- | --- |
| `file_creations` | Array of `{ "path": "/work/data", "write_limit": 8192, "count": 2 }`. `count` defaults to one; all counts together admit at most 64 future regular-file objects. |
| `file_renames` | `{ "parents": ["/work"], "limit": 2 }`. Up to 64 parents and 64 successful same-directory moves from a single-link tracked file to an absent name. |
| `file_unlinks` | `{ "parents": ["/work"], "limit": 2 }`. Up to 64 parents and 64 successful removals of closed, single-link tracked regular files. |

Each parent needs the [explicit noatime tmpfs directory provider](android-directory-creation.md)
and a realtime clock. Creation and unlink also require a persistent entry in
`directories`. Creation applies the provider's umask and parent setgid group;
it reserves every future object's name and maximum bytes against the common
file budget. A failed operation consumes no grant. A successful creation does
not regain its reservation after unlink or rename, and the original file's
write grant never becomes a grant for a replacement.

Names bind to stable objects. Rename retains open descriptions, bytes, cursors,
write limits and inode identity. Recreating a retired name assigns a fresh
object and inode. Status, directory enumeration, proc FD links and fdinfo use
the same live namespace. Successful operations update the file and parent
metadata using the supplied clock. An open parent-directory description,
captured mapping identity, replacement destination, cross-directory rename,
hard-link removal or exhausted grant stops explicitly before mutation.

Raw x64/AArch64 calls, Bionic `rename`/`renameat` and `unlink`/`unlinkat`, and
variadic `syscall` share this owner. Bionic alone translates negative Linux
results into `-1` and thread errno. Absolute unlink with zero flags also models
known missing/non-directory paths and shared pathname-import errors without a
mutation grant. Relative rename/unlink, symlink traversal and other unlink
flags remain unsupported.

Interior `.` components preserve the same live pathname. Open also resolves
`.` and `..` one visited directory at a time, including relative paths from an
open directory FD. Missing, non-directory and known search-denied components
cannot disappear through lexical normalization. A directory FD starts the
walk at that object; relative `AT_FDCWD` paths and general link traversal
remain unsupported. Final-dot handling outside open keeps its prior boundary.

Without its explicit provider, directory creation supports failures already
determined by pathname lookup. Raw x64 `mkdir` (83), x64/ARM64 `mkdirat` (258/34), and Android
`mkdir`/`mkdirat` share the catalogue: a missing parent returns `ENOENT`, a file
ancestor returns `ENOTDIR`, and an existing file or directory returns `EEXIST`.
The final name's `EEXIST` result also applies to a regular file followed by
trailing slashes; directory creation checks the parent separately.
They preserve the catalogue, descriptors and cursors. An absent final name
under an existing directory remains unsupported; mode, umask, write permission
and successful creation have no invented defaults. The boundary follows
[Linux parent and final-component lookup](https://github.com/torvalds/linux/blob/v4.9/fs/namei.c)
and the [API 28 Bionic wrapper](https://github.com/aosp-mirror/platform_bionic/blob/android-9.0.0_r1/libc/bionic/mkdir.cpp).

Reads validate the original user range before the page-aligned Linux transfer
cap or EOF, copy only available bytes, and advance by exactly the copied
prefix. A later inaccessible page preserves that prefix; a fault before any
byte returns `EFAULT`. Empty reads and EOF do not probe payload mappings.
A zero-length user range may start exactly at the user limit. After validating
that range, a file position plus the original count above `INT64_MAX` returns
`EINVAL`, including at EOF; neither failure advances the cursor.
Ordinary `SEEK_SET`, `SEEK_CUR` and `SEEK_END` keep a signed 64-bit nonnegative
cursor, permit seeking beyond EOF, and reject negative/overflowing positions
without changing it. `SEEK_DATA` and `SEEK_HOLE` remain unsupported. These are
bounded memory-file semantics based on Linux's [open lifecycle](https://github.com/torvalds/linux/blob/v6.6/fs/open.c),
[read/seek contracts](https://github.com/torvalds/linux/blob/v6.6/fs/read_write.c)
and [AArch64 flags](https://github.com/torvalds/linux/blob/v6.6/arch/arm64/include/uapi/asm/fcntl.h),
not general filesystem compatibility.

File status requires explicit observations. The optional `metadata` object has
exactly these fields; all are required when the object is present:

| Fields | Admitted values |
| --- | --- |
| `device` | Linux encoded unsigned 32-bit device number |
| `inode` | Unsigned 64-bit integer |
| `mode` | `S_IFREG` (32768) plus permission/special bits (0–4095) |
| `link_count`, `uid`, `gid` | Unsigned 32-bit integers |
| `size`, `blocks` | 0–`INT64_MAX`; blocks are 512-byte units |
| `block_size` | 0–`INT32_MAX` |
| `access_time`, `modification_time`, `change_time` | Objects with signed 64-bit `seconds` and `nanoseconds` from 0 to 999999999 |

All observation integers accept decimal strings for their full width. Numeric
JSON values must be integral and within ±(2^53−1), as for `linux_time`. Unknown
fields, missing members and invalid ranges fail before execution. C++ stores
these observations in `LinuxFileOptions::Metadata`, keyed by a path already in
`Files`; a key without file bytes is rejected. For example, this is an explicit
virtual-file observation with nonempty bytes and a reported size of zero:

```json
{"linux_files":{"files":[{"path":"/fixture/virtual","bytes_hex":"616263",
  "metadata":{"device":1,"inode":"18446744073709551615","mode":33060,
    "link_count":1,"uid":1000,"gid":1000,"size":0,"block_size":4096,"blocks":0,
    "access_time":{"seconds":0,"nanoseconds":0},
    "modification_time":{"seconds":0,"nanoseconds":0},
    "change_time":{"seconds":0,"nanoseconds":0}}}]}}
```

These immutable observations do not change access policy or byte-vector read
and seek behavior, and are never inferred from host metadata. Raw `fstat`
(x64 5, AArch64 80), Bionic `fstat`/`fstat64` and variadic `syscall` use the same
descriptor state without advancing its cursor. The ABI structures are 144 bytes
on x64 and 128 bytes on AArch64, with zero `rdev` and padding for regular files.
An invalid descriptor returns `EBADF` before accessing the output. A file
without metadata or a standard stream with unknown identity stops explicitly.
With known metadata, a wholly inaccessible or out-of-range output returns
`EFAULT`. Mixed writable/inaccessible output stops without changing bytes;
architecture-specific partial `copy_to_user` effects are not modeled. This is
the same fixed-copy policy used by Linux clock services. See the Linux
[status conversion](https://github.com/torvalds/linux/blob/v6.6/fs/stat.c),
[AArch64 layout](https://github.com/torvalds/linux/blob/v6.6/include/uapi/asm-generic/stat.h)
and [x64 layout](https://github.com/torvalds/linux/blob/v6.6/arch/x86/include/uapi/asm/stat.h).

`newfstatat` (x64 262, AArch64 79), Bionic `fstatat`/`fstatat64` and variadic
`syscall` query those same observations. Absolute paths in the supported form ignore dirfd;
missing entries return `ENOENT`, and traversal through a regular file returns
`ENOTDIR`. `AT_SYMLINK_NOFOLLOW` and `AT_NO_AUTOMOUNT` are accepted for supported
regular-file queries; link metadata, link traversal and automount behavior
remain unmodeled. A non-NULL empty pathname
with `AT_EMPTY_PATH` queries an existing descriptor without changing its cursor;
without that flag it returns `ENOENT`. Closed descriptors return `EBADF`.
The directory and flags arguments use their low 32 bits. Invalid flag bits
return `EINVAL` before path errors; the complete pathname is imported before
status output, so input and output may overlap.

Relative paths, directory metadata, the current working directory, NULL with
`AT_EMPTY_PATH`, and stat synchronization flags remain unsupported. The last two
have changed across kernel releases; the model does not infer a kernel version.
Path queries use the same fixed-copy policy and explicit metadata requirement
as descriptor queries. Contracts were checked against Linux
[v4.9](https://github.com/torvalds/linux/blob/v4.9/fs/stat.c) and
[v6.12](https://github.com/torvalds/linux/blob/v6.12/fs/stat.c), and the LP64
[Bionic service aliases](https://github.com/aosp-mirror/platform_bionic/blob/android-9.0.0_r1/libc/SYSCALLS.TXT).

`statfs` (x64 137, AArch64 43) and `fstatfs` (x64 138, AArch64 44)
resolve names and descriptors in that same explicit catalogue. Path imports
preserve `EFAULT` and `ENAMETOOLONG`; missing names return `ENOENT`, and a regular
file used as a directory returns `ENOTDIR`. Closed or unknown descriptors return
`EBADF`, using the low 32 bits of the descriptor argument. These errors precede
output access and leave every output byte unchanged. Queries of existing files,
implicit directories, root or live standard streams remain unsupported because
file metadata does not provide filesystem observations. No successful filesystem
status or mount information is inferred. Bionic `statfs`/`statfs64`,
`fstatfs`/`fstatfs64` and variadic `syscall` share these error paths and the usual
errno conversion. See Linux's [filesystem query order](https://github.com/torvalds/linux/blob/v4.9/fs/statfs.c)
and the [API 28 LP64 wrappers](https://github.com/aosp-mirror/platform_bionic/blob/android-9.0.0_r61/libc/bionic/statvfs.cpp).

<!-- i18n-section: windows-pe64 -->

## Windows PE64 profile

`windows-pe64-v1` supports bounded Windows x64/ARM64 console processes with PEB/TEB, static and dynamic TLS, `DllMain`, named Win32 APIs and explicit acyclic DLL graphs. Guest modules support named/ordinal code and data imports, DIR64 rebasing, forwarded exports and actual loader-list identities. `LoadLibraryA` / `LoadLibraryW`, `FreeLibrary` and `GetProcAddress` use the configured module catalogue. CRT/GUI, ARM64 frame-based user SEH, threads and general Windows application compatibility remain unfinished; native ARM64 KVM/WHP evidence is still pending.

`WindowsProcessTime.cpp` owns host-backed `GetSystemTimeAsFileTime`, `GetTickCount`, `QueryPerformanceCounter`, `QueryPerformanceFrequency` and `ZwDelayExecution`. FILETIME uses 100 ns units from 1601; the performance counter uses a monotonic clock and its reported 10 MHz frequency, while tick count wraps at 32 bits in milliseconds. Nonalertable relative delays up to 500 ms and a zero interval complete; alertable, positive absolute and longer delays stop explicitly without shortened waits or a successful result. These services preserve LastError and use the same model across CPU backends. `ZwDelayExecution` reads only the low 8 bits of `BOOLEAN`; unused argument-register bits do not change the wait policy.

[Windows x64 ABI](https://learn.microsoft.com/cpp/build/x64-calling-convention), [QueryPerformanceFrequency](https://learn.microsoft.com/windows/win32/api/profileapi/nf-profileapi-queryperformancefrequency), [FILETIME](https://learn.microsoft.com/windows/win32/api/minwinbase/ns-minwinbase-filetime).

Windows virtual memory adds `VirtualAlloc`, `VirtualFree`, `VirtualProtect`, `VirtualQuery` and current-process `FlushInstructionCache`. The OS layer owns reservations; `AddressSpace` remains the authority for committed pages, permissions and backing. Tests cover dynamic code rewriting, access faults and memory-budget reuse.

`WriteProcessMemory` follows the observed x64/ARM64 committed-page contract for current-process writes up to 4 KiB. It preserves each region's protection, copied prefixes, byte counts and LastError, including `ERROR_NOACCESS`, `ERROR_PARTIAL_COPY` and RX-prefix success. `WindowsMemoryWriteTests.cpp` checks all 25 protection pairs; `check_windows_memory_write.py` verifies the same original executable on native Windows CI. Uncommitted destinations remain explicitly unsupported.

Private allocations support `MEM_RESERVE`, `MEM_COMMIT`, `MEM_DECOMMIT`, `MEM_RELEASE` and `MEM_TOP_DOWN`, with 64 KiB reservation alignment and 4 KiB pages. Reserve-only regions consume no guest RAM. Recommit preserves bytes and updates protection; decommit returns individual page backing. Whole-range validation and staged allocation prevent partial changes on ordinary allocation/protection failures. Query returns the 48-byte x64/ARM64 memory-information layout and coalesces forward within one allocation. The initial image, environment, heap arena, API gates and stack margins participate in placement. Stack allocation identity agrees with TEB. If a successful `VirtualProtect` makes its old-protection output read-only, the new protection remains installed, the output bytes stay unchanged, and the call still succeeds. An uncommitted-range protection failure returns `ERROR_INVALID_ADDRESS`, writes `PAGE_NOACCESS` to the old-protection output and leaves page permissions unchanged.

Supported protections are `PAGE_NOACCESS`, `PAGE_READONLY`, `PAGE_READWRITE`, `PAGE_EXECUTE_READ` and `PAGE_EXECUTE_READWRITE`. Guard pages, execute-only and copy-on-write policies, cache modifiers, large pages, reset/write-watch/placeholders and modification of model-owned runtime mappings remain explicit unsupported operations. Only private virtual allocations can be decommitted or released.

[VirtualAlloc](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc), [VirtualFree](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualfree), [VirtualProtect](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualprotect), [VirtualQuery](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualquery), [MEMORY_BASIC_INFORMATION](https://learn.microsoft.com/windows/win32/api/winnt/ns-winnt-memory_basic_information).

```bash
neverd emulate guest.exe --profile=windows-pe64-v1 \
  --options='{"backend":"auto","arguments":["guest.exe","argument"],"environment":["MODE=test"]}'
```

The single-thread profile keeps the PE32+ EXE at its preferred base and admits explicitly supplied DLLs with optional entry points and static TLS. `WindowsProcessOptions::Modules`, or JSON `windows.modules`, supplies up to 64 guest basenames and host input paths through `name` and `path`; there is no host DLL search or execution. ASCII names compare without case; duplicate names and overrides of modeled system providers fail. Only reachable files are read. Named/ordinal function and data imports bind to actual mapped exports; holes, missing symbols, cycles, bound/delay imports and unsupported load configuration/CFG fail explicitly. DIR64 records rebase colliding movable DLLs; fixed collisions and relocation writes into linker metadata fail before publishing the affected image.

`readPEProgramExports` owns original export identities and bounded metadata footprints. `WindowsProcessModules` owns the guest graph and one exact provider/name API gate per process. `VirtualMemory` reserves every image before mapping; `AddressSpace` owns pages and permissions. PEB/LDR lists contain real images, with loader registration order in the initialization list. Registration and dependency-based attach order are tracked separately. `GetModuleHandleW` accepts NULL or ASCII basenames, compares without case, and appends `.dll` when no extension is supplied; paths, non-ASCII lookup and trailing-dot rules remain unsupported. Missing names return error 126; success preserves LastError. API models are not installed system DLLs.

Input-file bytes and aggregate image extents each share `memory_limit`; runtime mappings also consume the image budget. Preparation shares a 65,536-record and 64 MiB metadata-read allowance, bounded names and the workload deadline. Blocking host I/O has no hard time guarantee. The original EXE→DLL→DLL fixture checks rebased pointers, ordinal calls, shared data, API pointer identity, `MEM_IMAGE`, loader lists and executable TLS attach/detach. `NeverDWindowsProcessTests` owns these checks and the direct native Windows oracle; `NeverDPEProgramExportsTests` checks malformed metadata and work accounting, and `NeverDProcessPublicTests` checks C ABI/CLI catalogue parity. Unavailable transports are explicit skips.

`WindowsProcessLifetime` runs dependency DLL TLS callbacks then `DllMain`, followed by EXE TLS and entry, on one CPU under the same execution budget. Each module gets an independent TLS index and aligned block copied from the relocated, linked image within a shared 64 KiB arena. TLS reserved arguments are zero; startup/process-detach `DllMain` receives an opaque non-null value. Explicit process exit detaches successfully initialized DLLs in reverse loader-list order, then EXE TLS, even if EXE initialization had not run. Startup `DllMain(FALSE)` exits with `0xc0000142` without detach notifications. Faults and exhausted budgets do not invent cleanup. Returning from the PE entry with guest DLLs requires unsupported thread termination and stops explicitly. Nonzero `SizeOfZeroFill` remains unsupported; zero-initialized bytes in the actual TLS template are supported. DLLs without entry points receive TLS attach but no process-detach notifications.

`WindowsProcessExports` resolves static imports and `GetProcAddress` through the same named/ordinal identities, including code, data, aliases and chained forwarders. Only demanded startup forwarders add catalogue modules and initialization dependencies; unused forwarders do not load files. Export names are case sensitive; a missing name returns NULL/error 127, a direct missing ordinal (including a hole) returns NULL/error 182, and a null query argument returns error 87, while success preserves LastError. Unknown module handles remain unsupported. Exact provider/name API gates are reserved once from the bounded registry. Resolution checks every queried image’s live PE headers and export metadata, rejects changes or unreadable bytes, limits chains to 64 entries, and shares remaining preparation metadata credits and the workload deadline. A forwarder targeting a hole returns the target image base and preserves LastError; forwarding to ordinal zero returns error 87. The returned base is a data address, not permission to execute image headers. Runtime forwarders can load configured catalogue modules and complete initialization before returning a lookup result. Live export-table rewriting remains unsupported.

`WindowsProcessLoader` loads ASCII DLL basenames from `windows.modules` and owns explicit references, shared dependencies and startup retention. Repeated forwarded queries do not acquire extra references. Catalogue slots carry a new resident generation on reload. TLS and `DllMain` execute on the same CPU below suspended API frames; restoring registers preserves guest writes and uses the live return slot. Dynamic attach/detach reserved pointers are zero. Failed attach during explicit loading returns error 1114 after cleanup, retaining successful independent nested loads. Unload releases image mappings and TLS; reload restores original image contents. Loader-list or TLS-pointer changes outside the model fail explicitly. File, image and metadata work credits remain cumulative across failures and reloads. System providers use their mapped PE bases as module handles. Filesystem search, non-ASCII paths, `LoadLibraryEx` flags, cyclic imports and reentrant transitions of an already initializing or unloading module remain unsupported.

A DLL supplied as the process input uses `WindowsLibraryHost.cpp`: a modeled PE EXE executes `LoadLibraryA` → `FreeLibrary` → `ExitProcess` on the same guest CPU. The input consumes one of the 64 catalogue slots, retains its basename, and shares preparation and execution budgets with the host and dependencies. `GetModuleHandle(NULL)` identifies the host; `GetModuleFileNameA` accepts resident module handles. Self-file opens still access only the explicit input file. Failed attach follows ordinary error 1114 and cleanup; no export signature is guessed.

`GetEnvironmentVariableW`, `SetEnvironmentVariableW`, `GetEnvironmentStringsW`, `FreeEnvironmentStringsW`, `ExpandEnvironmentStringsW` share the live guest environment block in PEB process parameters. Names are ASCII and case insensitive; values are UTF-16. Updates validate inputs, capacity and writable memory before publication. Snapshots remain independent after changes and release their guest backing on free. The block has a 64 KiB model limit; strings and expansions are bounded and check the workload deadline. Unknown pointer ownership, malformed blocks, ANSI code pages and overlapping expansion buffers remain unsupported. `WindowsEnvironmentTests.cpp` compares original x64/ARM64 fixtures across available backends and requires an independent native Windows oracle in CI.

`WindowsProcessHeap` now owns allocation, [`HeapReAlloc`](https://learn.microsoft.com/en-us/windows/win32/api/heapapi/nf-heapapi-heaprealloc), free and size queries in one process heap. Resizing preserves the retained bytes; `HEAP_ZERO_MEMORY` clears newly exposed bytes, and `HEAP_REALLOC_IN_PLACE_ONLY` forbids movement. Failed resizing preserves the old block and returns NULL with `ERROR_NOT_ENOUGH_MEMORY` (8), matching the native observations. Separate page backing lets shrink/free return capacity, while staged growth and bounded copies observe the workload deadline. Custom heaps, exception-generating flags, unknown ownership and inaccessible copy/zero spans stop explicitly. `WindowsHeapTests.cpp` covers both ISAs, forced movement, budget reuse and failure atomicity; CI also runs the original EXE against native Windows.

`windows.peb_version` explicitly supplies the PEB `major`, `minor`, `build` and `platform` fields. All four unsigned integer fields are required; `build` is 16 bits and the other fields are 32 bits. Omission preserves the profile’s zero-filled version fields. `WindowsProcessEnvironment` owns their initialization on x64 and ARM64. This input does not choose native service numbers or certify compatibility with that Windows release. Capture and native replay must agree on any environment bytes on which retained code depends.

`WindowsSystemModules` builds bounded PE64 model images for `ntdll.dll`, `kernelbase.dll` and `kernel32.dll` on both ISAs. Their mapped bases are shared by ASCII `GetModuleHandleA` / [`GetModuleHandleW`](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getmodulehandlew), `LoadLibraryA` / `LoadLibraryW` and [`GetProcAddress`](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getprocaddress); PEB/LDR and `MEM_IMAGE` describe those same images. Static imports, named queries and guest forwarders use the same API gates and export resolver. Providers stay pinned, have no guest initialization callbacks and do not prevent entry return after ordinary guest DLLs unload. Changed headers or export metadata stop lookup. Unknown system export names and nonzero system ordinal queries stop explicitly; case-only mismatches of modeled names and empty names return error 127, while a null query returns 87. Generated bytes and addresses are model policy; Windows DLL version layouts, native ordinals and cross-provider aliases are not reconstructed. `WindowsSystemTests.cpp` compares original x64/ARM64 executables with native Windows, including eight independent initial-thread returns.

`WindowsProcessExceptions` implements `AddVectoredExceptionHandler`, `RemoveVectoredExceptionHandler` and `RaiseException` on one CPU with the process budget. Ordered handlers may register or remove handlers, raise nested exceptions, call modeled APIs, load DLLs and exit the process. x64/ARM64 data-access violations and x64 integer divide faults can resume after validated guest edits to `CONTEXT`; general registers, SIMD and supported FP state are preserved. Software exceptions resume through a real return instruction in the modeled provider. The model bounds registrations to 128 retained entries and nesting to 16 frames. Invalid dispositions, changed exception pointers, unsupported context fields and exhausted bounds fail explicitly. ARM64 frame-based SEH/unwinding, debugger delivery and execute/guard faults remain unsupported. `WindowsExceptionTests.cpp` compares original EXE/DLL scenarios against native Windows; native ARM64 KVM/WHP evidence remains pending. [AddVectoredExceptionHandler](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-addvectoredexceptionhandler), [RemoveVectoredExceptionHandler](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-removevectoredexceptionhandler), [RaiseException](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-raiseexception), [CONTEXT x64](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-context), [ARM64_NT_CONTEXT](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-arm64_nt_context). Software exception records carry `EXCEPTION_SOFTWARE_ORIGINATE` (`0x80`), independently of the caller’s noncontinuable flag; the original Windows executable checks the exact software and hardware flag values. [EXCEPTION_RECORD](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-exception_record).

`WindowsProcessContext` retains each dispatch frame’s origin. Admitted x64 data-access and divide faults expose RF (`0x10000`) in `CONTEXT.EFlags`; `RaiseException`, including software access-violation codes, retains the current context. The origin survives VEH/VCH and SEH search/unwind. Valid continuation restores logical CPU flags without RF; guest edits to RF are rejected before state publication. This bounded profile does not model instruction breakpoints or guest-controlled RF. `WindowsExceptionTests.cpp` checks saved records, restoration and unchanged CPU/RAM on rejection.

Windows ring3 maps checked x64 `operand_alignment` faults to `STATUS_ACCESS_VIOLATION` with parameters `[read, UINT64_MAX]`, including stores, following independent native observations. The CPU layer supplies the cause; Windows does not guess it from vector 13 or decode the instruction again. `WindowsAlignmentProcessTests.cpp` runs original PE instructions across 72 fault scenarios and 9 address-repair retries (`72 + 9`), checking PC, RF, XMM state and RAM. Unclassified or inconsistent faults remain rejected. Process and driver fault reports preserve nullable `cause` and hexadecimal `error_code`; absence stays distinct from zero. This delivery applies to the checked x64 user profile.

`AddVectoredContinueHandler` and `RemoveVectoredContinueHandler` maintain a separate ordered list, sharing the 128 retained registration limit with exception handlers. Continue callbacks run after a vectored exception handler accepts continuation; they see the same mutable exception record and `CONTEXT`. Final context validation happens after these callbacks, including nested exceptions and DLL notifications. Handles cannot be removed through the other handler family. `WindowsContinuationTests.cpp` compares original executables for ordering, short-circuiting, mutation, context repair, nested dispatch, loader callbacks and process exit against native Windows. The tested Windows x64 vectored path permits continuation with `EXCEPTION_NONCONTINUABLE` set; this does not establish frame-based SEH behavior. Native ARM64 execution remains unverified. [AddVectoredContinueHandler](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-addvectoredcontinuehandler), [RemoveVectoredContinueHandler](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-removevectoredcontinuehandler).

`RtlCaptureContext` is available through `kernel32.dll` and `ntdll.dll` for x64 and ARM64. Shared `WindowsProcessContext` and `IntegerABI` record the caller’s PC/SP without changing CPU state or LastError. Native Windows observations establish x64 flags `0x10000f`, preservation of untouched home/debug/vector storage, and the legacy 32-bit x87 address fields; ARM64 records PC from LR and clears the saved X0/LR. Register values, SIMD and FP controls come from the guest; x64 selectors and the MXCSR capability mask follow the configured guest CPU. Invalid, unaligned or partly inaccessible destination records fail before publication. `WindowsContextTests.cpp` covers direct imports, provider lookup, VEH callbacks, cross-page output and failure atomicity. `scripts/check_windows_context.py` runs the original executable on Windows x64 and ARM64, with a separate native nonempty-x87 oracle. These ARM64 API observations do not establish native KVM/WHP execution. Context restore, stack walking and dynamic function tables remain separate work. `WindowsProcessServices.def` declares exact module restrictions: the modeled `kernelbase.dll` lookup returns `ERROR_PROC_NOT_FOUND` (127), matching native observations rather than creating an extra export. [RtlCaptureContext](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtlcapturecontext).

`WindowsProcessSEH` uses shared `X64SEH` in `os/windows/exception/` (`NeverDEmulationWindowsException`, available without drivers) for x64 `__C_specific_handler` and UNWIND_INFO V1. After VEH search it supports filters, finally callbacks, nonlocal handler transfer, nested/collided dispatch and rebased EXE/DLL frames, preserving nonvolatile GPR/XMM state. Filter continuation runs VCH with the same `CONTEXT`. `WindowsSEHTests.cpp` compares 23 original scenarios with native Windows; KVM/WHP/Unicorn share these semantics. Dispatch rechecks image generations, headers, unwind/scope bytes, personality code regions and IAT bindings under the process budget. Changed metadata or unloaded retained images fail explicitly. ARM64 frame SEH, C++ EH, dynamic function tables, general RtlUnwind/NtContinue, and unwinding across loader/VEH/VCH callback boundaries remain unsupported.

For `EXCEPTION_NONCONTINUABLE`, an x64 filter returning `EXCEPTION_CONTINUE_EXECUTION` dispatches `STATUS_NONCONTINUABLE_EXCEPTION` (`0xc0000025`, flags `0x81`, null linked record) with a fresh context. VEH runs again before search restarts on the retained logical stack, preserving finally order and EXE/DLL frame identity under the same depth and execution budgets. The 23 native scenarios include 21 successful executions and two terminations: accepting this secondary exception in VEH/VCH remains unhandled even after restoring the original `CONTEXT`. The model reports that outcome as a runtime failure. Software exception addresses equal their saved PC; internal dispatcher addresses and register layout are model policy. [Windows x64 CI](https://github.com/NeverSight/NeverD/actions/runs/37141166235).

Before dynamic detach callbacks, the module leaves the initialization list while its mapping, name lookup and load/memory list membership remain available. Entry-return oracles observe the initial thread independently of system worker threads.

`WindowsDynamicTests.cpp` compares original x64/ARM64 DLLs and EXEs with independent native Windows observations: reference counts, shared dependencies, nested loading, failed attach cleanup, forwarded lookup, process exit, no-entry DLLs and fresh TLS on reload. Additional regressions reject modified loader metadata and stale code pointers, preserve cumulative preparation budgets and keep interrupted API results incomplete. Windows CI requires the native oracle and WHP cases; cross-compilation and Unicorn ARM64 do not establish native ARM64 execution.

A missing library anywhere in a `GetProcAddress` forwarder chain returns error 127; an explicit `LoadLibrary` of a missing catalogue module returns 126. The native oracle and each available backend assert all 41 declared loader scenarios; return after unloading every DLL is observed 16 times per DLL variant on Windows. Failed initialization in a `GetProcAddress` forwarder also returns 127 after cleanup. Process-detach callbacks preserve the exiting caller’s stack contents.

`WindowsExportTests.cpp` uses original x64/ARM64 DLLs and an EXE to check forwarded code/data/ordinal calls, aliases, initializer queries, rebasing, case-sensitive misses, LastError, cyclic and nonresident targets, invalid pointers and metadata changes after successful queries. The same EXE has an independent native Windows oracle; WHP cases are mandatory in native CI. C ABI/CLI tests compare complete reports. Native ARM64 hardware evidence remains pending. Variants with and without EXE exports cover both dependency graphs, PEB-list order, detach order and name/ordinal/null error codes.

`WindowsLifetimeTests.cpp` compares frozen traces with independent native Windows processes and KVM/WHP/Unicorn execution: normal exit, entry return, both DLL initialization failures, four early exits and DLLs without entry points. It separately checks callback faults, shared budgets, relocated TLS fields and aggregate TLS capacity. The native entry-return probe retains the initial thread handle and checks its exit code and exact thread/process notification sequence in 64 repetitions. Remaining child threads are terminated after observation; their process exit is not treated as the entry return value.

```json
{"windows":{"modules":[{"name":"middle.dll","path":"inputs/middle.dll"},{"name":"leaf.dll","path":"inputs/leaf.dll"}]}}
```

GS on x64 and x18 on ARM64 point to TEB. Supported state includes stack bounds, self pointer, PID/TID, PEB, process parameters, LastError and TLS. Inputs are strict UTF-8 converted to UTF-16; argv is quoted for Microsoft CRT parsing. Environment names are ASCII, case-insensitive duplicates are rejected, values may be Unicode, and the sorted environment is double-NUL terminated. No host environment or filesystem is inherited. Static TLS copies its template, zeroes BSS and writes a 32-bit index; dynamic TLS uses separate TEB slots. Attach and detach read live callback arrays in order, with all instructions and named calls sharing one deadline and resource account. Normal process exit runs detach callbacks. Entry return is supported only with no resident guest DLLs; a second `ExitProcess` during process-exit cleanup remains unsupported.

The exact API inventory is `WindowsProcessServices.def`: `ExitProcess`, `RtlExitUserProcess`, standard-output handles and synchronous `WriteFile`, LastError, process/thread identifiers and pseudo-handles, `GetCommandLineW` / `HeapReAlloc`, process heap allocation/free/size, dynamic TLS and `LoadLibraryA` / `LoadLibraryW` / `FreeLibrary` / `GetModuleHandleA` / `GetModuleHandleW` / `GetProcAddress`. Provider names are restricted to `kernel32.dll`, `kernelbase.dll` and `ntdll.dll` with exact export identity. Direct syscalls and forged callback gates cannot select API models. Heap backing is owned by the process and reclaimed on free. Writes capture binary bytes; Win32 argument errors remain distinct from unsupported asynchronous I/O or user exception dispatch. Pointer aliasing observes the initial completion-count write and the live call-return slot.

The `windows.native_calls` report preserves module/function names, declared scalar arguments and nullable result bits. It does not invent native NT syscall numbers. `NeverDWindowsProcessTests` covers real x64/ARM64 PE startup, compiler TLS, live callback changes, heap/LastError, aliasing, invalid metadata, privilege faults and limits across available backends. `NeverDProcessPublicTests` checks the same PE through CLI/C ABI. Native Windows CI runs the original EXE as an independent behavioral oracle and requires WHP cases; native ARM64 runtime evidence still requires a suitable machine.

`WriteFile` with a nonempty unreadable input buffer returns `ERROR_INVALID_USER_BUFFER` (1784), zeros the completion count and publishes no bytes.

`windows.defer_unmodeled` loads an image whose loader facts the model does not implement and stops only if execution depends on one. Exports outside the API inventory and modules outside the catalogue bind to opaque entries: each identity resolves to one address, and executing it stops as `unsupported_service` naming `module!export`. Directories the model does not interpret stay uninterpreted, metadata the file does not back is not read at load, and frame-based exception dispatch through such an image stops. `observeProcess` adds a `ProcessObserver` that reads the stopped process at its start and at each execution watch; it cannot change guest state, and ending the run reports `observer`. [Unpacking](unpack.md) is built on both.

[PE/COFF](https://learn.microsoft.com/windows/win32/debug/pe-format), [ARM64 ABI](https://learn.microsoft.com/cpp/build/arm64-windows-abi-conventions), [WriteFile](https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-writefile), [TLS](https://learn.microsoft.com/windows/win32/api/processthreadsapi/nf-processthreadsapi-tlsgetvalue), [Wine 10.0 loader](https://github.com/wine-mirror/wine/blob/wine-10.0/dlls/ntdll/loader.c). [GetProcAddress](https://learn.microsoft.com/windows/win32/api/libloaderapi/nf-libloaderapi-getprocaddress).

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

Android `atoi`, `atol` and `atoll` accept bounded C-locale decimal prefixes,
including leading whitespace and one sign. The first nondigit ends the scan;
representable 32-bit/64-bit results preserve errno. Out-of-range results stop
explicitly without choosing libc-specific wrapping or saturation. This subset
follows [C11 section 7.22.1.2](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n1570.pdf);
it does not add general `strtol` bases, end pointers or locale mutation.
