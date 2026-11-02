# Released Android GKI kernel contracts

NeverD prioritizes the released Android GKI 5.10–6.18 branches before broader
Linux kernel variants. Select a branch explicitly in a process request:

```json
{"linux_kernel":{"gki":"android17-6.18"},"linux_files":{"files":[],"descriptor_limit":16}}
```

The Android native profile's API 28 contract describes Bionic imports and does
not select a Linux kernel version. GKI selection currently controls only the
implemented `pidfd_open`, vectored-output, encoded process CPU clock and
extended-attribute query error contracts. Descriptor polling uses the shared
Linux owner independently of GKI selection.
It does not certify an
entire kernel, load a kernel image, or infer device, namespace, credentials or
process inventory.
Existing unsupported services still stop explicitly. See the official
[GKI release policy](https://source.android.com/docs/core/architecture/kernel/gki-releases).

## Source pins

The catalogue in `LinuxGKIKernels.def` follows these formal `r1` release tags,
checked on 2026-10-07. Each linked immutable commit supplies `kernel/pid.c`,
`include/uapi/linux/pidfd.h`, `arch/arm64/configs/gki_defconfig`,
`kernel/fork.c`, `lib/iov_iter.c`, `fs/read_write.c` and the CPU clock sources
linked below. Extended-attribute ordering uses each pin’s `fs/xattr.c` and
`include/uapi/linux/limits.h`; syscall numbers use the generic UAPI and x64
syscall table. Flag values come from the UAPI and the syscall's validation,
rather than an Android API level or the host kernel.

| Request branch | Formal release tag | Pinned source commit | Admitted flags | Iovec import |
| --- | --- | --- | --- | --- |
| `android12-5.10` | `android12-5.10-2026-07_r1` | [b14525331e0d](https://android.googlesource.com/kernel/common/+/b14525331e0d5d335b037d6ed17d40424ed47b0a/kernel/pid.c) | `PIDFD_NONBLOCK` (`0x800`) | [Copy all metadata first](https://android.googlesource.com/kernel/common/+/b14525331e0d5d335b037d6ed17d40424ed47b0a/lib/iov_iter.c) |
| `android13-5.10` | `android13-5.10-2026-07_r1` | [b9c8cb19d426](https://android.googlesource.com/kernel/common/+/b9c8cb19d426ec591e0a34dcc2d8638147e4ebc1/kernel/pid.c) | `PIDFD_NONBLOCK` | [Copy all metadata first](https://android.googlesource.com/kernel/common/+/b9c8cb19d426ec591e0a34dcc2d8638147e4ebc1/lib/iov_iter.c) |
| `android13-5.15` | `android13-5.15-2026-09_r1` | [0b6028f1f30d](https://android.googlesource.com/kernel/common/+/0b6028f1f30da3c2143bb40eee912c4974108e5c/kernel/pid.c) | `PIDFD_NONBLOCK` | [Copy all metadata first](https://android.googlesource.com/kernel/common/+/0b6028f1f30da3c2143bb40eee912c4974108e5c/lib/iov_iter.c) |
| `android14-5.15` | `android14-5.15-2026-07_r1` | [9938d39e2fe9](https://android.googlesource.com/kernel/common/+/9938d39e2fe99593abe347df3b82ea2f85c83d1d/kernel/pid.c) | `PIDFD_NONBLOCK` | [Copy all metadata first](https://android.googlesource.com/kernel/common/+/9938d39e2fe99593abe347df3b82ea2f85c83d1d/lib/iov_iter.c) |
| `android14-6.1` | `android14-6.1-2026-09_r1` | [79480508eb1e](https://android.googlesource.com/kernel/common/+/79480508eb1eed09620f1cd5484dbfa4a677d0a9/kernel/pid.c) | `PIDFD_NONBLOCK` | [Copy all metadata first](https://android.googlesource.com/kernel/common/+/79480508eb1eed09620f1cd5484dbfa4a677d0a9/lib/iov_iter.c) |
| `android15-6.6` | `android15-6.6-2026-07_r1` | [5556e039c32f](https://android.googlesource.com/kernel/common/+/5556e039c32fa02b239611dc8e5ebb958a7f12e1/kernel/pid.c) | `PIDFD_NONBLOCK` | [Single-buffer path](https://android.googlesource.com/kernel/common/+/5556e039c32fa02b239611dc8e5ebb958a7f12e1/lib/iov_iter.c) |
| `android16-6.12` | `android16-6.12-2026-09_r1` | [894a317b5382](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/kernel/pid.c) | `PIDFD_NONBLOCK` and `PIDFD_THREAD` (`0x80`) | [Single-buffer path](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/lib/iov_iter.c) |
| `android17-6.18` | `android17-6.18-2026-09_r1` | [bab5f6aca819](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/kernel/pid.c) | `PIDFD_NONBLOCK` and `PIDFD_THREAD` | [Single-buffer path](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/lib/iov_iter.c) |

These pins define the current contract. A later release or backport needs source
verification and regression coverage before changing it. A selected released
GKI branch and an explicit absent `pidfd_open` observation contradict each
other and are rejected before image loading.

## Implemented process-descriptor subset

Raw x64/AArch64 traps and Android Bionic's `syscall` share `LinuxServices` and
one workload-owned descriptor table. The model consumes the low 32 PID/flag
bits and rejects unknown flag bits or nonpositive signed PIDs with `EINVAL`
before reserving a descriptor. The current process is a known live group
leader; it can open a pidfd with the selected flags. Other positive PIDs require
an explicit guest task observation. An omitted catalogue retains the unsupported
lookup boundary, without inferring absence from missing host information.

The optional `tasks` array is a closed, fixed catalogue of additional live tasks
in this workload's guest PID namespace:

```json
{"linux_kernel":{"gki":"android17-6.18","tasks":[{"id":2000,"group_leader":true},{"id":3000,"group_leader":false}]},"linux_files":{"files":[],"descriptor_limit":16}}
```

The running process (PID 1000, group leader) is implicit, including for an empty
array. A valid positive PID outside a declared catalogue returns `ESRCH` before
descriptor allocation. A live nonleader without `PIDFD_THREAD` returns `EINVAL`
on the pinned 5.10–6.12 releases, and `ENOENT` on
[the pinned 6.18 release's `pidfd_prepare`](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/kernel/fork.c).
With the accepted thread flag, the 6.12 and 6.18 releases can open the declared
live nonleader. Flag validation still precedes all target lookup.

Every entry requires an integer `id` in 1..2147483647 and a Boolean
`group_leader`. At most 4096 entries are admitted. Duplicate IDs, extra fields,
an observed nonleader PID 1000, and a catalogue without GKI selection are
rejected. Explicit priority observations must name tasks present in this
catalogue or the running process. This fixed catalogue cannot be combined with
Android's cooperative thread mode (`thread_limit > 1`); creation, reaping,
credentials, namespace translation and other task lifetime changes need their
own supported ownership before that combination can be admitted.

A request must declare `linux_files` so the same descriptor limit and ownership
apply to regular files and process descriptors. Allocation uses the lowest
free number; exhaustion returns `EMFILE`, `close` releases it, and a second
close returns `EBADF`. Closing a standard stream permits that number to be
reused. No host pidfd, filesystem or process lookup runs.

On a valid pidfd, scalar `read` and `write` return `EINVAL` before payload
access, and `lseek` returns `ESPIPE` after validating the seek origin. `writev`
imports and validates its vector first, so invalid metadata or user extents
can return `EFAULT` before the missing write operation's `EINVAL`. It does not
read payload mappings or capture output for a pidfd. The shared importer applies
the selected version to pidfds and captured stdout/stderr alike. This follows the
pinned pidfd/pidfs operations and [VFS read/write ordering](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/fs/read_write.c).

The pinned 5.10, 5.15 and 6.1 branches copy the entire iovec array before
rejecting negative lengths. An earlier negative length followed by inaccessible
metadata returns `EFAULT`. They validate original buffer extents before the
transfer cap, including for a single vector. The pinned 6.6, 6.12 and 6.18
branches read and validate descriptors sequentially, so that combination
returns `EINVAL`. Their single-buffer path caps the length before checking the
user extent; their multi-vector path still validates every original extent.
These are `copy_iovec_from_user`, `__import_iovec` and `import_ubuf` observations
from the linked immutable source pins. Missing GKI selection keeps the existing
single-buffer policy; it does not infer a kernel release.

Bionic translates raw negative errors to `-1` and thread-local `errno`; a
successful call preserves `errno`. `fstat` needs file metadata not supplied by
this subset. Exit notifications, signal delivery through pidfds,
`pidfd_getfd`, `fcntl`, pidfs ioctls and unobserved task lookup remain unsupported.
Scheduling and process lifetime are not inferred from a pidfd.

## Process-descriptor polling

Raw x64/AArch64 `ppoll`, x64 `poll` and Bionic named/variadic wrappers use
one descriptor owner. Polling does not require GKI selection; creating pidfds
still follows the selected kernel's contract. `linux_files.descriptor_limit`
supplies the guest RLIMIT_NOFILE bound. The low unsigned 32-bit `nfds` count
may not exceed it. Observed live pidfds have no readiness, negative descriptors
are ignored, and closed descriptors report `POLLNVAL` (`0x20`). Every nonzero
entry is counted separately, including repeated descriptors.

Regular-file readiness, scheduled TCP/UDP input, finite waits, raw ppoll timeout
copyback and Bionic timeout preservation are documented with the
[shared poll/epoll contract](linux-network-contracts.md#poll-and-epoll-waits).
Missing observations and unsupported descriptor kinds remain explicit stops.

The timeout is copied and validated before the mask and descriptor arguments.
A nonnull mask has its full-width size and readable extent checked before the
unsupported temporary-mask boundary. Null masks ignore the size argument.
All descriptor metadata is imported before readiness selection or output.
Only each 16-bit `revents` field is written, in entry order; a later fault
retains earlier stores. Mixed access within one field retains the shared
unsupported partial-copy boundary. The final user-range check also applies to
an empty array. A zero timeout is never written back, so a readable, read-only
zero timespec can succeed. The call does not read or advance any wall clock.

These rules were checked at all eight immutable release pins in `fs/select.c`:
`ppoll`, `do_sys_poll`, `do_pollfd`, `poll_select_set_timeout` and
`poll_select_finish`. See the pinned
[5.10 poll implementation](https://android.googlesource.com/kernel/common/+/b14525331e0d5d335b037d6ed17d40424ed47b0a/fs/select.c)
and [6.18 poll implementation](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/fs/select.c).
Live pidfd readiness follows `pidfd_poll` in the first six releases'
[fork implementation](https://android.googlesource.com/kernel/common/+/5556e039c32fa02b239611dc8e5ebb958a7f12e1/kernel/fork.c)
and the last two releases' pidfs implementation:
[6.12](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/fs/pidfs.c),
[6.18](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/fs/pidfs.c).
Exit and reaping notifications differ across those versions and are outside
the fixed-live-task subset. Temporary masks and signal delivery still require
further contracts. Finite waits can expire using the shared virtual clock;
this does not invent task exit or reaping events.

## Architecture-specific open flags and known path errors

The closed `linux_files` catalogue also admits `O_DIRECTORY` and `O_DIRECT`
when their result is determined before opening an observed file. ARM64 uses
`0x4000` for directories and `0x10000` for direct I/O; x64's generic ABI uses
the opposite values. All eight pinned ARM64 and generic UAPI headers agree:
see the [5.10 ARM64 header](https://android.googlesource.com/kernel/common/+/b14525331e0d5d335b037d6ed17d40424ed47b0a/arch/arm64/include/uapi/asm/fcntl.h),
[6.18 ARM64 header](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/arch/arm64/include/uapi/asm/fcntl.h)
and [6.18 generic header](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/include/uapi/asm-generic/fcntl.h).

The shared descriptor owner imports the pathname, checks descriptor capacity,
and then resolves the catalogue and any directory requirement. Known absent
paths return ENOENT; a regular file used as a directory returns ENOTDIR.
An opened file's direct-I/O support follows pathname resolution in the pinned
[5.10 open implementation](https://android.googlesource.com/kernel/common/+/b14525331e0d5d335b037d6ed17d40424ed47b0a/fs/open.c)
and [6.18 implementation](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/fs/open.c).
The catalogue does not declare a backing filesystem's direct-I/O capability,
so existing regular files with that flag retain an explicit unsupported
boundary. Existing directory opens remain unsupported. These stable rules
also apply without a GKI selection; they do not infer procfs content or mounts.

## Extended-attribute query import and target errors

With explicit query import order and `linux_files` selection, raw x64/AArch64 `getxattr`,
`lgetxattr` and `fgetxattr` share the file owner with their three named Bionic
imports. The model admits errors determined by importing the attribute name
or resolving a declared target. Empty names and names with no terminator in
256 bytes return ERANGE; a valid name can contain at most 255 bytes. Import
stops at the first NUL. An unreadable name returns EFAULT.

The first seven released pins resolve the pathname or descriptor before
importing the name. The 6.18 pin imports the name first. Thus a closed FD and
empty name return EBADF on 5.10–6.12 and ERANGE on 6.18; a known missing path
and unreadable name return ENOENT and EFAULT respectively. The order is
source-pinned in [5.10 `path_getxattr` and `fgetxattr`](https://android.googlesource.com/kernel/common/+/b14525331e0d5d335b037d6ed17d40424ed47b0a/fs/xattr.c),
[6.12](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/fs/xattr.c)
and [6.18 `path_getxattrat`](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/fs/xattr.c).

GKI selection supplies this order from the release table. To declare only the
observed order without selecting the other GKI contracts, use:

```json
{"linux_kernel":{"xattr_query_import":"target-before-name"},"linux_files":{"files":[]}}
```

The other accepted value is `"name-before-target"`. The values are exact and
case sensitive. Supplying `gki` together with `xattr_query_import` is rejected,
even when they agree. Without either input, these queries remain unsupported.
This declaration
does not supply attribute values, permission or namespace policy, link
traversal, task observations or any other syscall's semantics. Explicit absence
observations such as `unavailable_syscalls:["pidfd_open"]` remain independent.
The name limit comes from each pin's
[UAPI limits](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/include/uapi/linux/limits.h).

Known absent paths return ENOENT and a regular file used as a directory returns
ENOTDIR in the selected order. Closed descriptors return EBADF after consuming
their low 32 bits. These failures do not access the value buffer or change file
cursors. Bionic alone converts raw negative errors to `-1` and `errno`.
For an existing object and valid name, the catalogue does not supply attributes,
credentials, security policy or filesystem capability; execution stops explicitly.
New `getxattrat` entry points, symlinks, listing and attribute mutations require
additional contracts.

## Implemented process CPU clock subset

With explicit GKI selection or an explicit `linux_kernel.tasks` catalogue,
`clock_gettime` admits negative encoded process CPU clock IDs for PROF, VIRT and SCHED. It uses the low signed 32-bit argument;
the encoded PID and clock kind identify an explicit sample in `linux_time`.
The current process is implicit. Other processes must be declared live group
leaders in the closed task catalogue before a clock sample can be supplied:

```json
{"linux_kernel":{"gki":"android17-6.18","tasks":[{"id":2000,"group_leader":true}]},"linux_time":{"advance_on_idle":true,"clocks":[{"id":1,"seconds":10,"nanoseconds":0},{"id":2,"seconds":3,"nanoseconds":4},{"id":-16006,"seconds":7,"nanoseconds":9}]}}
```

Omit `gki` when only the task catalogue and process CPU clock subset are
observed. An explicitly empty `tasks` array admits the current process and
observes foreign absence. This does not enable pidfd release semantics or
supply any clock samples.

Here `-16006` is PID 2000's SCHED clock. PROF and VIRT are independent
observations. For the current process, SCHED IDs 2, -6 (encoded PID zero) and
-8006 (PID 1000) share one sample. The corresponding PROF aliases are -8 and
-8008, and VIRT aliases are -7 and -8007. Duplicate aliases are rejected even
when their values agree. CPU seconds must be nonnegative and nanoseconds
normalized. Idle advancement leaves every process CPU sample fixed while
advancing only declared wall clock IDs 0, 1, 4 and 7 from their own epochs. Instruction execution does not infer
CPU usage.

The current task's own TID also identifies its process group, including in
cooperative Android thread mode without a foreign task catalogue. For foreign
targets, a closed catalogue's missing PID or live nonleader returns `EINVAL`
before destination access. An omitted catalogue leaves the lookup unsupported.
A known group without an explicit clock sample also stops unsupported before
accessing the destination. Invalid CPU clock kinds return `EINVAL`; valid
samples reach the shared user copy and can return `EFAULT`. Raw traps retain
negative errors, and Bionic alone updates errno and returns -1 on those errors.

These target and clock-kind rules follow `pid_for_clock`,
`posix_cpu_clock_get`, the clock dispatcher and clock-ID definitions at every
immutable release pin:

| Request branch | Process CPU clock source |
| --- | --- |
| `android12-5.10` | [b14525331e0d](https://android.googlesource.com/kernel/common/+/b14525331e0d5d335b037d6ed17d40424ed47b0a/kernel/time/posix-cpu-timers.c) |
| `android13-5.10` | [b9c8cb19d426](https://android.googlesource.com/kernel/common/+/b9c8cb19d426ec591e0a34dcc2d8638147e4ebc1/kernel/time/posix-cpu-timers.c) |
| `android13-5.15` | [0b6028f1f30d](https://android.googlesource.com/kernel/common/+/0b6028f1f30da3c2143bb40eee912c4974108e5c/kernel/time/posix-cpu-timers.c) |
| `android14-5.15` | [9938d39e2fe9](https://android.googlesource.com/kernel/common/+/9938d39e2fe99593abe347df3b82ea2f85c83d1d/kernel/time/posix-cpu-timers.c) |
| `android14-6.1` | [79480508eb1e](https://android.googlesource.com/kernel/common/+/79480508eb1eed09620f1cd5484dbfa4a677d0a9/kernel/time/posix-cpu-timers.c) |
| `android15-6.6` | [5556e039c32f](https://android.googlesource.com/kernel/common/+/5556e039c32fa02b239611dc8e5ebb958a7f12e1/kernel/time/posix-cpu-timers.c) |
| `android16-6.12` | [894a317b5382](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/kernel/time/posix-cpu-timers.c) |
| `android17-6.18` | [bab5f6aca819](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/kernel/time/posix-cpu-timers.c) |

The FD-clock discriminator and CPU clock routing also follow the pinned
[6.18 dispatcher](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/kernel/time/posix-timers.c)
and [clock-ID definitions](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/include/linux/posix-timers_types.h).
The pins' `init/Kconfig` enables POSIX timers by default, and their GKI
defconfigs do not disable it; see the pinned
[6.18 configuration default](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/init/Kconfig).
FD-backed clocks and encoded per-thread CPU clocks remain unsupported. The
catalogue is a fixed guest observation; permission, namespace, process lifetime
and CPU-time accounting extensions require their own supported contracts.

## Validation and next coverage

`LinuxPIDFDTests.cpp` executes independent x64/AArch64 ELF fixtures at O0/O2
across the available transports, for all eight branches. It checks versioned
flags, descriptor reuse/limits, regular-file coexistence, scalar/vector error
ordering, inaccessible later metadata versus earlier negative lengths,
single-vector cap versus original-extent checks, closed and omitted catalogues,
live nonleaders, absent targets before FD exhaustion, and retained unsupported
boundaries.
Its CPU clock cases check identity and output ordering, independent clock kinds,
explicit observation validation and separation from wall-clock idle advancement.
`AndroidSyscallTests.cpp` repeats ownership and raw/Bionic error encoding over
O0/O2 Android fixtures with ordinary, Android-packed and RELR relocations,
including the versioned vector import, task lookup and errno differences.
`AndroidTimeTests.cpp` checks named/raw process CPU clock output and canaries,
and the cooperative syscall fixture verifies the current nonleader TID alias.
Zero-timeout poll fixtures check live group and admitted nonleader pidfds,
negative/closed descriptors, low-32-bit counts, timeout and mask error ordering,
read-only timespecs, complete metadata import, ordered fault prefixes and
unchanged wall clocks. Android repeats readiness and raw/Bionic errno behavior
over all six relocation profiles.
Open-flag fixtures check architecture-specific directory/direct bits, combined
flags, narrow arguments, pathname faults, descriptor exhaustion and unchanged
cursors on all eight branches. Android repeats raw and all four named open
imports with independent errno checks, and retains existing-file boundaries.
Extended-attribute fixtures check all eight import orders, empty and overlong
names, name and pathname faults, page-edge terminators, low descriptor bits,
unchanged cursors and value-buffer canaries. Raw/Bionic callers retain unknown
kernel and existing-object boundaries across all packing profiles.

Source pins and these model executions are evidence for the specified syscall
subset. Native tests booting every pinned GKI image are not yet available.
Extend the released GKI matrix service by service, retaining version and
configuration differences and explicit workload observations, before treating
normal Linux distributions as an additional compatibility target.
