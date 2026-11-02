# Live Linux process observations

Linux ELF64 and Android native workloads can enable a bounded proc view over
their current modeled state:

```json
{
  "linux_files": {
    "files": [],
    "dynamic_proc": {"name": "example", "parent_pid": 1}
  }
}
```

The name is an explicit 1–15 byte task name without control characters or
backslashes. The parent PID is an explicit nonnegative signed 32-bit identity.
PID/TGID and UID/GID remain the profile's existing 1000 identities. This option
also enables live FD links; the older `dynamic_proc_fds` option retains its
smaller, compatible scope. Conflicting static objects under `/proc/self`,
`/proc/thread-self` and `/proc/1000` are rejected. Other proc snapshots can remain
explicit catalogue inputs. No host process, device or filesystem is consulted.

## Shared state

| Path | Authoritative state |
| --- | --- |
| `/proc/self/maps` | Current AddressSpace ranges and permissions, Linux mapping identities |
| `/proc/self/task` | Live cooperative guest threads; finished children disappear before join |
| `/proc/self/status`, `task/TID/status` | Current thread state, model identity, FD capacity and mapped size |
| `/proc/self/fdinfo/FD` | Current shared open-description offset, status flags and per-FD CLOEXEC |
| `/proc/self/fd/FD` | Current descriptor membership and its catalogue pathname |

`/proc/1000` names the same process. `/proc/thread-self` resolves to the calling
thread's task directory at open time. Task subdirectories expose maps, status,
fd and fdinfo over the same process owners. Directory cursors and buffered
records belong to shared open descriptions; dup does not create an independent
cursor. Seeking a task directory resets its saved next-thread identity when
the position changes. Closing/reusing a numbered FD changes later fdinfo
refills; an already buffered record remains readable.

Status emits only fields with modeled meaning: Name, State, Tgid, Pid, PPid,
TracerPid, Uid, Gid, FDSize, VmSize and Threads. The model has no guest tracer.
Waiting means a cooperative join, once, mutex or sleep wait, and a finished
group leader is retained as a zombie while children remain. RSS, capabilities,
signal masks, cgroups and resource statistics are **not** inferred or printed
as zeros. A missing field is not evidence about that unmodeled subsystem.

FD info includes pos, octal flags, mnt_id and ino. Mount IDs 1 (catalogue) and
2 (live proc) and unspecified inode numbers are workload-local identities,
consistent across directory and fdinfo queries. Complete supplied file metadata
retains its inode. Read-only device contracts expose the common fields with
their live open flags and zero cursor; driver-specific fdinfo extensions remain
outside that contract. Opaque captured streams and pidfds stop on fdinfo because
their complete file-operation observations are not supplied. Dup clears CLOEXEC
on the new FD; dup3 uses its explicit flag. Creation/truncation request flags
do not become persistent open-description flags.

## Mapping identities and reads

Anonymous model RAM is private and initially unnamed. Linux program-break and
main-stack ranges are labeled `[heap]` and `[stack]`; device mappings are shared,
with their actual catalogue path and mapping offset. Synthetic loaded image RAM does not
implicitly acquire a host filename or a native inode. Optional initial guest
backing identities can be declared under `dynamic_proc.mappings`:

```json
{"name":"example","parent_pid":1,"mappings":[
  {"address":"0x20000000","size":8192,"offset":16384,
   "device":2049,"inode":91,"shared":false,"path":"/guest/module.so"}
]}
```

Every row requires all seven mapping fields. Ranges must be disjoint, aligned
to the guest page size and fully mapped before execution. Paths are empty or
canonical absolute **guest** paths. At most 256 initial identities are accepted.
Permissions always come from the address space; inaccessible mapped guards
remain visible as `---p` and count towards VmSize. Mprotect splits visible ranges;
munmap retires affected backing labels and adjusts surviving offsets. Mapping
new anonymous RAM at that address cannot revive the old name or inode. Device
aliases retain their shared backing identity independently of descriptor close.
The observation is bounded to 65,536 live range fragments.

Proc reads follow record-buffer semantics: the first nonzero read generates a
record, short reads and dup retain its suffix, and later refill observes current
owners. Maps resumes at the next VMA address, including intervening splits or
merges. A seek to a different byte position traverses current records; rewind
refreshes the view. SEEK_END is invalid. Zero-size reads do not generate data.
User copies preserve committed page prefixes and cursor movement on EFAULT.
Libc sees the proc filesystem's 1024-byte preferred I/O block size; the kernel
record buffer starts at one guest page and grows for long records, retaining
its capacity across seek and refill.
An exited task or closed descriptor is checked when generating new records,
not by rewriting bytes already in a buffer.

These independently implemented rules use Linux 6.6's
[proc records](https://github.com/torvalds/linux/blob/v6.6/fs/proc/array.c),
[task enumeration](https://github.com/torvalds/linux/blob/v6.6/fs/proc/base.c),
[maps iterator](https://github.com/torvalds/linux/blob/v6.6/fs/proc/task_mmu.c),
[fdinfo](https://github.com/torvalds/linux/blob/v6.6/fs/proc/fd.c) and
[sequential reader](https://github.com/torvalds/linux/blob/v6.6/fs/seq_file.c)
and [proc superblock](https://github.com/torvalds/linux/blob/v6.6/fs/proc/root.c)
as platform references. This is a selected virtual namespace, not a complete
native procfs mount: unknown entries stop explicitly. Proc inode stat metadata,
foreign processes, arbitrary symlink traversal, smaps and maps ioctl extensions
remain outside the model.

### Explicit legacy Android anonymous VMA names

`linux_kernel.anonymous_vma_names = "android-user-pointer"` enables
`prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, address, length, name)` on anonymous
model RAM. The raw x64 and ARM64 syscalls, Bionic `prctl` and Bionic `syscall`
all use LinuxMemory's shared implementation. The option is independent of
Android API level, GKI and whether the live proc namespace is enabled. Missing
selection and other nonnegative outer `prctl` operations stop as unsupported.

The shared Linux owner narrows the outer option to a signed 32-bit kernel
`int`. Negative options return EINVAL before reading the remaining arguments
or requiring an optional VMA-name provider. They change no guest memory or
mapping state. Raw x64 and ARM64 traps return `-EINVAL`; Bionic callers return
`-1` and set their thread errno. High bits outside the kernel `int` do not
change the interpretation of a nonnegative option. Dynamic calls still require
an active provider before reaching this owner. Custom security hooks and
vendor-defined negative commands are outside this contract.

This contract borrows the **guest pointer**, without copying, validating or
pinning the name at the call. A null pointer clears the name. Pointer identity
controls name boundaries; equal strings at different addresses remain distinct.
The outer option uses its low 32 bits, while the suboption, address, length and
name retain all 64 bits. Start alignment and length/end overflow produce EINVAL;
a zero length succeeds before suboption validation. Length rounds up to pages.
Holes produce ENOMEM while mapped intervals on both sides receive the change.
These metadata operations do not change memory bytes, permissions or allocation
ownership. Unmapping, including Android thread retirement, removes affected
names before address reuse. Metadata is bounded by the existing 65,536-fragment
proc mapping budget; exhaustion stops explicitly before publishing a new state.

When maps generates a record, it reads at most 255 bytes from the live guest
pointer, stopping at NUL. An unreadable page appends `<fault>` after any readable
prefix. The result is `[anon:NAME]`; legacy anonymous names preserve newline and
backslash bytes. Existing heap/stack labels take precedence. Buffered record
suffixes and dup retain already generated bytes; a refill or rewind observes
later name writes, protection changes or unmapping. A status read does not
dereference names. Declared backing identities, device/MMIO mappings and other
VMA classes remain explicit unsupported boundaries for naming.

This is an independently implemented, selected legacy contract checked against
Android common revision `aa856bd83c43969dc3e8f8c0e97e6105e91aef76`:
[`prctl_set_vma` and its anonymous-name walk](https://android.googlesource.com/kernel/common/+/aa856bd83c43969dc3e8f8c0e97e6105e91aef76/kernel/sys.c)
and [`seq_print_vma_name`](https://android.googlesource.com/kernel/common/+/aa856bd83c43969dc3e8f8c0e97e6105e91aef76/fs/proc/task_mmu.c).
It does not select modern Linux's copied-name representation or claim a full
Android kernel/VMA implementation.

## Metadata after regular-file writes

An existing file may opt into complete stat maintenance with
`"track_write_metadata": true` alongside `write_limit` and its complete initial
`metadata`. This selects a noatime, page-backed memory-file provider, independently
of pathname or Android version. The initial size must match the bytes, the initial
allocation must be dense (`blocks = ceil(size / 4096) * 8`), and set-ID bits must
be absent. CLOCK_REALTIME must be explicitly supplied in `linux_time.clocks`
before a write or truncation needs its timestamp.

Successful writes update size and count only allocated 4 KiB pages. A seek past
EOF followed by a write creates zero-filled holes without charging their pages;
overwriting an allocated page does not charge it twice. O_TRUNC clears content
and allocation, updates mtime/ctime and leaves other open cursors intact. All
opens, dup aliases, fstat and pathname fstatat observe the same complete object.
Device/inode, mode, ownership, link count, block-size and noatime atime are kept;
mtime/ctime use the same virtual realtime owner as clock_gettime.

A nonempty admitted write updates timestamps before copying data, including
an ensuing source-memory fault. Zero counts and preceding VFS extent errors do
not. An unavailable clock stops before changing bytes or metadata. Resource
reservations still bound storage; they are not simulated disk errors.
Without this opt-in provider, writes retain the previous explicit invalidation
of captured stat observations. No timestamp or allocation policy is inferred
from a read-only capture. The implementation follows the ordered update and
copy boundary in Linux's [generic writer](https://github.com/torvalds/linux/blob/v6.6/mm/filemap.c)
and the explicitly selected memory-page allocation policy. It does not model
on-disk durability, quota, ACL, set-ID removal or filesystem compression.

`LinuxProcTests` checks both LP64 wire layouts and changes between reads.
`AndroidProcTests` executes original O0/O2 native callers through the supported
backends, including actual pthread creation/exit, DIR/FILE buffering, raw reads
and metadata writes. C API, CLI and Python use the same JSON declarations and
fixture. No phone is needed for these checks.
