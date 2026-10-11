**Languages**: [English](darwin-emulation.md) | [简体中文](zh-CN/darwin-emulation.md) | [繁體中文](zh-TW/darwin-emulation.md) | [日本語](ja/darwin-emulation.md) | [한국어](ko/darwin-emulation.md) | [Français](fr/darwin-emulation.md) | [Deutsch](de/darwin-emulation.md) | [Español](es/darwin-emulation.md) | [Italiano](it/darwin-emulation.md) | [Русский](ru/darwin-emulation.md) | [العربية](ar/darwin-emulation.md)

[← Documentation index](README.md)

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
BSD services add an `error` Boolean to their records: a positive errno in
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

BSD calls on ARM64 use X16, X0–X5 and `svc #0x80`; x64 uses the BSD class `0x02000000`
with RAX and RDI/RSI/RDX/R10/R8/R9. Successful calls clear carry and supply the
documented scalar result; errors set carry and return positive errno. ARM64
clears X1; x64 clears RDX on success and preserves it on error. X64 SYSCALL
return clobbers are explicit. These rules come from the XNU
[ARM64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/arm/systemcalls.c)
and [x64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/i386/systemcalls.c)
entry paths; no Apple implementation code is incorporated.

The core service inventory is `exit`, `write`, `getpid`, `getppid`, `getuid`,
`geteuid`, `getgid`, `getegid`, `getgroups`, `mmap`, `mprotect` and `munmap`. PID is deterministically 1000 and parent PID is 1. UID/GID default to 1000;
optional credentials below explicitly select distinct real/effective IDs. Output descriptors 1 and
2 are initially captured byte sinks, including NUL and non-UTF8 bytes. Their
duplicates retain the original sink; writes through closed or read-only
descriptors return EBADF. A partial readable prefix is captured, but a subsequent copy
fault retains EFAULT, consistent with XNU's
[write error propagation](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/sys_generic.c).
The raw write length is limited to `INT_MAX`; larger requests return EINVAL
before descriptor lookup, guest-pointer access or output-budget admission.

Memory services support private anonymous data mappings with descriptor -1
and offset zero (`flags=0x1002`). Lengths and nonfixed hints round up to the OS
page size; an occupied hint searches upward before falling back. The raw
legacy zero-length mmap succeeds with address zero without allocating;
`MAP_UNIX03` (`0x40000`) additionally permits the standard flag spelling
and rejects zero length with EINVAL. Unmap/protect addresses must be aligned.
NONE, READ and WRITE protections are supported, and WRITE
implies READ. Physical owners are per OS page, so partial unmap releases its
budget and subsequent mappings are zeroed. Maximum protections are distinct
from current rights. A failed protect across a hole or maximum-rights boundary
leaves the complete range unchanged. The rules are grounded in XNU's
[BSD VM services](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/kern_mman.c).

Shared/fixed/JIT mappings, executable mappings, other Mach traps,
indirect system calls, threads, signals, host filesystem/network access, dyld
linking, Objective-C/Swift runtime and Foundation/UIKit are outside this
profile. They stop explicitly. This is neither a full Apple OS compatibility
layer nor the iOS Simulator application.

## Explicit file inputs and descriptors

`darwin_files` adds a closed catalogue of initially read-only regular files to all three
profiles. `files` is required; each entry has a canonical absolute guest `path`
and hexadecimal `bytes_hex`. Optional `stdin_hex` supplies a finite input stream:
omitted input is unknown and stops on a nonzero read; an empty string is EOF.
Neither guest paths nor standard input consult the host. Missing catalogue
configuration stops `open`; an explicit empty catalogue still contains root, while absent paths return ENOENT.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839"}],"stdin_hex":"00ff78","descriptor_limit":32}}
```

The file services include `open`, `read`, `pread`, `write`, `pwrite`, `truncate`,
`ftruncate`, `lseek`, `close`, `dup`, `dup2`, `fcntl`, `rename`, `renameat` and `renameatx_np`. The `read`, `write`,
`open`, `close`, `fcntl`, `pread` and `pwrite` nocancel entries use the same owners.
`open` supports O_RDONLY, O_WRONLY, O_RDWR, O_NONBLOCK, O_APPEND, O_TRUNC, O_CREAT, O_EXCL, O_CLOEXEC, O_DIRECTORY, O_NOFOLLOW and O_NOFOLLOW_ANY.
`fcntl` supports F_DUPFD, F_DUPFD_CLOEXEC, F_GETFD, F_SETFD, F_GETFL, bounded F_SETFL and F_GETPATH.
Separate opens have independent cursors; duplicated descriptors share a cursor
but have independent close-on-exec flags. `pread` never changes the cursor.
Closing/replacing descriptors 0, 1 or 2 affects subsequent I/O, and a duplicated
output descriptor keeps its capture sink and the shared output budget.

Configuration allows at most 256 specified file/directory/link paths (including
metadata/contents-only ancestor paths), 16 MiB of combined paths/NUL/file/input/CWD/link-target
and encoded directory-record bytes, paths shorter than 1024 bytes and components of at most 255 bytes.
`descriptor_limit` is an exclusive ceiling from 3 to 4096, default 256.
JSON retains the existing 64 KiB request limit. Invalid configuration is rejected
before image loading, including a file that is another file's ancestor.

Raw read lengths above INT_MAX return EINVAL before FD lookup. EOF does not
touch the destination; invalid writable addresses return EFAULT. A partially
writable destination stops before copying or advancing the cursor, because
partial filesystem copyout effects are outside this model. Seek supports
SET/CUR/END, preserving the cursor on negative-position or overflow errors.
Legacy stat metadata and other fcntl operations remain unsupported.
Path prefixes describe implicit directories; a regular file used as an ancestor
returns ENOTDIR. The ABI is grounded in XNU's
[read path](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/sys_generic.c),
[open/seek path](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/vfs/vfs_syscalls.c)
and [descriptor operations](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/kern_descrip.c).

## Mutable existing files

A file entry can opt into process-local mutation with the strict Boolean
`"writable": true`; C++ uses `DarwinFileOptions::WritableFiles`. Omission or false
keeps the initial read-only contract. Unknown write authorization stops explicitly.
No host file is read or changed, and the caller's initial bytes remain unchanged.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233","writable":true}]}}
```

`write` (4/397), `pwrite` (154/415), `truncate` (200), `ftruncate` (201) and
O_TRUNC change a shared file node. Separate opens share bytes but retain independent
cursors; dup shares its open description. Closing the last descriptor does not
reset the file. Growth zero-fills gaps; truncation preserves every cursor.
O_RDONLY|O_TRUNC also truncates. F_SETFL changes only O_APPEND|O_NONBLOCK after native flag conversion, preserves access,
close-on-exec and the observed FWASWRITTEN bit; unsupported flags stop explicitly.
F_GETFL exposes FWASWRITTEN=0x10000 after nonzero transferred bytes, including
positioned writes and captured output. Successful ftruncate also sets the bit,
even at unchanged size, on the calling open description and its duplicates.
O_TRUNC sets it on the newly opened description, including O_RDONLY; path
truncate leaves existing description flags unchanged. pwrite ignores append
and preserves the cursor. Count above INT_MAX returns EINVAL before FD lookup; pwrite offset -1
returns EINVAL even earlier. Offset INT64_MAX returns EFBIG before zero-count
success; length is clipped at that boundary before append chooses EOF.

Fully readable input commits its bytes and cursor once. A partially readable
input stops before effects because native prefix writes and extension rollback
are filesystem-dependent. Whole EFAULT preserves bytes; nonempty append still
moves its cursor to EOF. Transport failures preserve contents and cursor.
Without an explicit mutation policy, successful nonzero writes, truncation and
nonzero whole-EFAULT attempts invalidate the complete stat observation: native failed append can change mtime/ctime.
Subsequent stat calls stop as unknown before output instead of returning stale
size/timestamps/block allocation. Zero writes leave contents and metadata intact.

The aggregate logical storage cap stays 16 MiB: paths/NUL, input, directory
records, CWD and current file sizes count together; writable path references also
count. Admission precedes allocation. Shrinking replaces the backing vector and
reclaims capacity. Caller-owned initial bytes and one bounded replacement buffer
are additional storage, not claimed to fit within that logical cap. Known writable
inode aliases and immutable/append-only metadata flags are rejected until their
semantics are modeled.

Private mappings retain file leases until all their ranges are unmapped, including
PROT_NONE mappings and mappings whose FDs have closed. Writes and truncation,
including O_TRUNC, stop before effects while a lease exists; failed or legacy
zero-length mappings retain no lease. A fresh mapping sees current file bytes.
O_WRONLY mmap with READ (including normalized WRITE) returns EACCES; PROT_NONE
is allowed and later mprotect may grant read/write, matching XNU's private
maximum-permission normalization.

The original `writable-files` and `writable-files-nocancel` workloads compare
actual bytes, offsets, flags and error order with the native kernel. Unit tests
cover 4 KiB/16 KiB pages, budget reclamation, backend failure, metadata invalidation
and mapping lifetime; C/CLI/Python exercise all five profile/ISA combinations.
Permission enforcement, arbitrary initial-directory deletion, rename between distinct initial directory domains, hard links, native filesystem metadata updates, coherent vnode/COW
or shared mappings and EOF SIGBUS remain unfinished. This does not complete the
macOS/iOS environment or establish physical iOS or Intel HVF verification.

References: [XNU write](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/sys_generic.c),
[vnode offsets](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c),
[file flags](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/fcntl.h),
[mmap permissions](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mman.c).

## Explicit mutable metadata

A file may add `mutation_policy` next to `writable: true` and complete `metadata`;
C++ uses `DarwinFileOptions::MutationPolicies`. This selects a virtual sparse-unit
filesystem contract. It does not infer APFS allocation or sample the host clock.
Omitting it preserves the unknown post-mutation metadata contract above.

```json
{"mutation_policy":{"allocation_unit":4096,"mutation_time":{"seconds":-7,"nanoseconds":123456789}}}
```

Both fields and both time members are required, with the existing lossless integer
rules. The allocation unit must be a power of two from 512 bytes to 16 MiB,
independent of `block_size` and VM pages. Admission requires ordinary regular-file
permissions (no set-id/sticky), `flags=0`, `link_count=1`, and dense initial
allocation: `blocks=ceil(size/allocation_unit)*(allocation_unit/512)`. Dense is
an explicit assertion even for zero bytes; the model never infers holes from
their values. The policy path reference also counts toward the 16 MiB logical
storage limit. Block accounting is a separate virtual ledger, not an ENOSPC model.

Writes allocate every touched unit, including writes of zero into a hole. Truncate
growth adds zero bytes without allocating units; shrink discards whole units beyond
rounded-up EOF and retains an allocated final partial unit. Regrowth does not restore
discarded allocation. Successful nonzero writes and every successful truncate,
including same-size truncate and empty O_TRUNC, update size/blocks and set mtime/ctime
to the supplied fixed time. All other metadata stays unchanged; reads do not advance
atime. Stat by path, independent opens, dup and reopen share the same node state.
Caller input observations remain unchanged.

Zero writes, budget or mapping refusals, partial-input refusal and backend failure
preserve known metadata. Nonempty whole-EFAULT still makes it unknown; later
successful writes or truncation cannot reconstruct it. Failed stat copyout preserves
the node. The `virtual-file-metadata` guest workload checks the complete 144-byte
record through all five profiles and C/CLI/Python; its allocation results are policy
tests, not native APFS comparisons. Existing native writable workloads verify the
kernel flags, cursors and error ordering separately. Further namespace operations, native
filesystem coherence, Mach services and dynamic runtime loading remain unfinished.

## Sparse file positioning

`lseek` accepts `SEEK_HOLE=3` and `SEEK_DATA=4` on regular files with an explicit
mutation policy whose allocation state is still known. It reads the same unit
ledger as stat; before the first mutation the admitted initial file is dense,
including zero-valued bytes. Within the requested kind of unit it returns the
input offset, otherwise the next matching unit's start. The terminal hole begins
at EOF. Negative offsets return EINVAL; offsets at or beyond EOF, including an
empty file, return ENXIO=6 for either mode. No later data also returns ENXIO.
Errors preserve the cursor; success changes only the open description shared
by dup. Independent opens keep their own cursors, and reopen sees current allocation.
Metadata, flags and bytes are unchanged. High whence carrier bits are ignored.

Missing policies, directories and permanently unknown allocation after whole
EFAULT remain unsupported. No holes are inferred from zero bytes, and rejected
writes or growth cannot invent allocation. The original `sparse-file-seek` program
checks native/guest errors, occupied bytes, EOF and descriptor lifetime without
assuming earlier filesystem-specific extent boundaries. `virtual-file-metadata`
separately checks exact policy geometry; C/CLI/Python cover all five profiles.
[XNU lseek](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

## Removing regular-file names

Per-directory `mutable:true` (C++ `DarwinFileOptions::MutableDirectories`) explicitly authorizes immediate namespace changes, independently of `writable` file contents. The example below permits unlink of a read-only `/work/data`. Missing grants stop as unsupported; permission bits do not invent credentials or EACCES. Admission rejects nonzero known flags on mutable parents or immediate regular children, special parent permission bits, child link_count other than one, and known identity aliases of either the parent or child. Identity checks include stat and directory-snapshot inode observations; distinct explicitly known devices stay distinct. Grants count toward the existing path/entry budgets.

`unlink(10)` and `unlinkat(472)` remove existing regular names; regular-file unlinkat accepts flags zero or `AT_SYMLINK_NOFOLLOW_ANY=0x800`. Unknown flag bits return EINVAL before path/FD checks; AT_REMOVEDIR uses the bounded directory-removal contract below; DATALESS and SYSTEM_DISCARDED remain unsupported. Low 32 flag bits apply. The shared resolver preserves pathname-fault/dirfd order, relative CWD/dirfd use and absolute-path independence. Missing names return ENOENT, file/trailing-slash paths ENOTDIR, ordinary directory targets EPERM and a slash-only guest root EISDIR; root paths ending in `.` or `..` return EBUSY. Original native probes also check terminal `.`/`..` directory paths.

A live namespace owns file objects separately from open descriptions. Unlink preserves existing FD/dup/independent-open bytes, offsets and status flags. Later opens fail; implicit parent directories and CWD remain present. F_GETPATH retains the last linked path, matching the native reference even after name removal. The file's writable grant belongs to its object. Unlinked objects retain their current-byte budget while any description or mapping lease remains; close/dup2 and the next mutation reclaim only unreachable objects, after the final mapped range is unmapped. Initial path/reference costs remain charged. Rename between distinct initial directory domains and hard links remains unfinished; initial-directory removal uses the explicit grant described below.

A successful unlink invalidates only its parent's stat and enumeration observations across old/new FDs, dup and path queries. stat/readdir/SEEK_END stop before output or cursor changes; ordinary read/pread still return EISDIR, while SET/CUR, F_GETPATH, fchdir and relative lookup continue. With a still-known file mutation policy, unlink sets nlink=0 and ctime to its fixed time, preserving mtime/atime/bytes/allocation. Later writes or truncation cannot restore nlink=1. Without a policy, or after whole-EFAULT, complete file metadata remains unknown. Rejected unlinks preserve all state. The original `unlinked-file` program compares native kernel name/descriptor behavior; policy timestamps and directory invalidation are explicit model rules.

```json
{"darwin_files":{"files":[{"path":"/work/data","bytes_hex":"00"}],"directories":[{"path":"/work","mutable":true}]}}
```

[XNU unlink / unlinkat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [F_GETPATH](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_descrip.c).

## Creating regular files

`O_CREAT=0x200` creates an empty regular file only in an explicitly mutable immediate parent. Ordinary/nocancel open and openat share this behavior. The namespace grant makes new objects writable; existing objects retain their separate `WritableFiles` authority. A read-only descriptor can create but cannot write. Without an explicit creation policy, new stat64 and sparse-seek observations remain unknown, including when an old configured name is reused. New objects never inherit that old name's metadata or mutation policy.

`O_EXCL=0x800` with O_CREAT returns EEXIST for an existing file or directory before truncation or writable/mapping checks; alone it has no effect. Existing read-only directory opens with O_CREAT succeed. After the openat prefix check described below, invalid access mode precedes descriptor availability, then O_CREAT|O_DIRECTORY returns EINVAL before pathname access. Only a missing final original component can be created: missing ancestors, trailing `/`, `//`, `/.` and `/..` remain ENOENT. Relative CWD/dirfd and absolute-path rules share the existing resolver. New O_CREAT|O_TRUNC does not set FWASWRITTEN; truncating an existing object does.

Only successful insertion invalidates the immediate parent's stat/enumeration observations. New and old unlinked objects at the same name retain independent bytes, descriptors, metadata and mapping leases. The 256-entry cap includes fixed initial non-file entries plus live and retained orphan file objects. Each new object charges its canonical path and NUL alongside current bytes within 16 MiB. Unlink reclaims these dynamic charges only after the last description and mapping lease; closing a still-named object releases neither its entry nor bytes. Initial input/reference charges remain reserved. Exhausted model budgets, including a resolved canonical path of 1024 bytes or more, stop explicitly without inventing ENOSPC or a native pathname error. Rejected creation publishes no name or descriptor.

The original `created-file` workload compares native macOS and all five guest profiles; 4K/16K tests independently cover exact capacity, rollback and mapped name reuse. Permission enforcement, rename between distinct initial directory domains, links and further directory operations remain separate work.

[XNU open](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c).

## Explicit creation metadata and process umask

Optional `darwin_files.umask` (C++ `InitialUmask`) declares the initial process mask, from 0 through octal 07777. It works independently of file creation authority. `umask(60)` returns the previous mask and stores the low 07777 bits, without guest-memory access or a free descriptor. Omission remains unknown; no host/default mask is inferred. The mask initializes once, changes only future creations, and never mutates caller input. The example below supplies decimal 18, or octal 0022.

Without `namespace_policy`, optional `darwin_files.creation_policy` (C++ `CreationPolicy`) enables complete metadata for new regular files. Its strict object has exactly `first_inode`, `block_size`, `generation`, `creation_time` and `mutation_policy`; time and mutation objects use the existing strict formats. It requires an explicit umask, at least one mutable parent, and complete metadata for every mutable parent. `block_size` is positive and at most INT32_MAX; generation is uint32. The allocation unit is a power of two from 512 through 16 MiB, independent of block size and VM pages; nanoseconds must be in [0, 1000000000). `first_inode` is a nonzero uint64 greater than every supplied stat/snapshot inode, including other devices. Decimal strings preserve values beyond JSON's exact integer range.

Only a successful new insertion consumes the next global inode. A successful UINT64_MAX exhausts the sequence permanently; close, unlink, name reuse, umask and later namespace lookups cannot reset it. Exclusive, FD, path, entry and byte-budget refusals publish no name, descriptor or inode increment. Existing O_CREAT opens consume none.

New stat64 records use the direct parent's supplied device and GID, the selected guest effective UID (default 1000), mode `S_IFREG | (mode & 0777 & ~umask)`, nlink 1, and zero size/blocks/flags. Block size and generation come from policy; all four timestamps equal the fixed creation time. Device/GID remain known after a parent's complete stat/enumeration observation becomes invalid, so later creation can use those fields without restoring that full record. Each new node owns its metadata and allocation state; it never borrows an old same-name object's record. Subsequent write/truncate/unlink use the supplied mutation policy, preserve inode/mode/birthtime and an unlinked nlink=0, and retain permanent whole-EFAULT invalidation. Existing nodes are unaffected by the creation policy.

The original `created-file-metadata` workload compares native modes, umask return bits, effective UID, parent device/group and identity lifetime across five guest profiles. `virtual-created-metadata` separately compares the entire 144-byte policy record. Native creation timestamps need not all be equal; the fixed values and sparse allocation policy describe the declared virtual filesystem. Permission enforcement, credential switching, ACLs and native APFS metadata behavior remain outside this contract.

```json
{"darwin_files":{"files":[],"umask":18}}
```

[XNU creation](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c), [XNU umask](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c). [XNU rename / renameat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

## Regular-file rename

`rename(128)`, `renameat(465)` and `renameatx_np(488)` move or replace a regular file within one mutable initial directory and its process-created descendants. Both immediate parents require namespace authority, even for a same-name no-op. The low 32 flag bits accept `RENAME_EXCL=0x4` or `RENAME_SWAP=0x2`, optionally combined with `RENAME_NOFOLLOW_ANY=0x10`; flags 0 or 0x10 perform ordinary rename. Unknown bits or EXCL+SWAP return EINVAL before reading paths; SECLUDE remains unsupported. The EXCL and SWAP contracts follow below. The component walker preserves source-before-target errors, directory-FD rules and original trailing-slash/dot checks. Removed directory sources stop explicitly. Live process-created directories and declared movable initial directories use the subtree contracts below. A resolved terminal target dot/dotdot returns EINVAL before mount or grant checks; missing or non-directory ancestors keep their earlier errors. An ordinary regular-file rename to a directory within the admitted namespace returns EISDIR.

All existing source descriptions, including independent opens and dup, share the current `F_GETPATH` name. A removed or replaced target keeps its own last linked path, bytes, offsets, flags and mapping lifetime, even when the source moves again. Replacement never transfers the target's write grant or metadata to the source. Each object's known mutation policy updates only its ctime, plus nlink=0 for the replaced target; identity, ownership, birthtime, bytes and allocation remain attached to that object. Without a policy or after whole-EFAULT, complete metadata stays unknown. An actual move invalidates both parents' complete stat/enumeration observations; a no-op does not.

Rename consumes neither a creation inode nor a new entry, and needs no free FD. The destination canonical path/NUL replaces the source's dynamic path charge. Initial input/reference costs stay reserved. Replaced bytes and dynamic path costs can fund admission only if no old description or mapping retains that target, and are reclaimed once. Partial unmap retains the full object's byte charge until its final range is gone. Paths of 1024 bytes or more and insufficient aggregate 16 MiB capacity stop before name, metadata or descriptor changes.

Moves between distinct initial directory domains and contradictory known device observations remain unsupported: equal stat device numbers do not establish shared mount identity, so the model does not invent EXDEV. Initial-directory moves require the movable declaration below; initial-directory SWAP, SECLUDE and permission enforcement remain future work. The original `renamed-file` program compares native and guest identity, path, replacement and mapping behavior; policy timestamps and budget rules are explicit virtual behavior.

## Directories and relative paths

Optional `directories` entries contain a canonical absolute `path` and optional
complete `metadata`. Root and ancestors are implicit; empty directories can be
explicit. Metadata may describe root or an implicit ancestor, but never creates
a missing path. Directory mode is `0x4000` plus permissions; its `size` is an
explicit observation in [0, INT64_MAX]. Missing size or timestamps are not inferred.
`working_directory` must name an existing canonical directory. Omitting it leaves
CWD unknown; it never inherits the host directory.

`openat` (463), `openat_nocancel` (464), `chdir` (12), `fchdir` (13) and
`fstatat64` (470) share the `DarwinFiles` component walker with open and stat64.
Relative paths use the directory FD or `AT_FDCWD=-2`. Absolute paths ignore the
FD. Repeated separators, `.`, `..` and trailing slashes retain ancestor checks:
`/file/..` returns ENOTDIR and `/missing/..` returns ENOENT. Failed CWD changes
preserve the previous directory; closing, reusing or replacing its original FD
does not change CWD. `F_GETPATH=50` copies the canonical path and NUL, including
through duplicated descriptors, with no writes after the terminator.

Directory read/pread returns EISDIR, even for zero length; a negative positioned
offset takes EINVAL precedence. SET/CUR seek uses a shared cursor; END requires
explicit metadata. Directory mmap returns EINVAL. `fstatat64` accepts zero,
`AT_SYMLINK_NOFOLLOW=0x20`, `AT_SYMLINK_NOFOLLOW_ANY=0x800`, and
`AT_FDONLY=0x400` (ignore the pathname entirely). Invalid flag bits return EINVAL;
`AT_REALDEV=0x200` stops because real-device metadata is unmodeled. Stream path
and directory identities remain unknown. Permission bits are observations,
not an access-control model; directory creation and bounded removal use the explicit grants described below.

The original `directories` workload compares these services with the native
macOS kernel and all five guest combinations; native stat records cover both
files and directories. Intel HVF Actions remains suspended. ABI references:
[XNU VFS](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c),
[XNU flags](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/fcntl.h).

Directory validation (2026-10-05, Release): 467 registered Darwin cases, 227 passed, 240 skipped, zero failed; all 60/60 required ARM64 HVF cases executed. The 10 native macOS workloads, 37 public C/CLI/report tests without skips, five Python guest combinations and 66 evidence-runner tests passed. Counts overlap. Evidence: `build-hvf-arm64/darwin-directory-verified-evidence/`. Other native transports and physical iOS remain unvalidated for this increment.

```json
{"darwin_files":{"files":[{"path":"/work/data","bytes_hex":"3031"}],"directories":[{"path":"/work/empty"}],"working_directory":"/work"}}
```

## Explicit directory snapshots

`getdirentries64` (344) enumerates an optional immutable `contents` snapshot on
an existing `directories` entry. C++ uses `DarwinFileOptions::DirectoryContents`.
Supply the complete ordered `entries`, including `.` and `..` and every immediate
catalogue child. Missing snapshots remain unknown, even for an empty directory.
Snapshots create neither paths nor stat metadata and never consult host files.

Each entry requires `name`, nonzero `inode`, `type` (0 unknown, 4 directory,
8 regular file), `next_offset` and `seek_offset`. Type must agree with the path;
inodes must agree for the same resolved path across snapshots and metadata.
`next_offset` is a nonzero cookie up to INT64_MAX, unique within that directory,
with no ascending-order requirement. Zero rewinds. `seek_offset` is the separate
unsigned 64-bit d_seekoff observation; duplicate zero values are valid. Integer
fields use the same lossless decimal-string rules as stat metadata.

`contents.minimum_buffer_size` is a required positive payload minimum up to
128 MiB, including at EOF. An entry's optional `minimum_buffer_size` (default 0)
adds a constraint when a call starts at that entry. The example records the
observed APFS minimum of 64 bytes for the initial dot pair, then 1 byte at EOF;
other positions must fit at least one complete record. Records use the native
LP64 layout and eight-byte alignment, with size `roundUp(25 + nameBytes, 8)`.
At most 4096 records are admitted across all snapshots. Encoded records join
the 16 MiB input budget; metadata/contents-only ancestor paths count once toward
the 256-path limit. The 64 KiB JSON request limit still applies.

Independent opens have independent cursors; dup shares them. Enumeration resumes
at zero or a supplied cookie; unknown positions stop explicitly. Each call emits
a maximal whole-record prefix. Counts >=1024 reserve the last four requested
bytes for EOF flags (1 at EOF, 0 otherwise); only the record payload is capped at
128 MiB. The suffix address retains original unsigned count arithmetic, including
wrap. Data is copied first, the cursor advances, the pre-read position is copied,
then flags are written. Later EFAULT preserves earlier effects; EOF skips the
empty data copy. A partially writable individual copy stops unsupported before
that copy, retaining any preceding copies and cursor effects.

The original `directory-entries` workload compares record fields, dup/rewind,
small reads, EOF and copy ordering with the native macOS kernel. A separate test
compares captured native record bytes, including long names, against the SDK
layout. Snapshot cookies remain fixed on rewind; this does not reproduce APFS's
dynamic cookie generations. Legacy `getdirentries` (196), enumeration after namespace mutation,
other native transports and physical iOS remain outside this acceptance.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"3031"}],"directories":[{"path":"/empty"},{"path":"/","contents":{
  "minimum_buffer_size":1,"entries":[
    {"name":".","inode":41,"type":4,"next_offset":11,"seek_offset":0,"minimum_buffer_size":64},
    {"name":"..","inode":41,"type":4,"next_offset":22,"seek_offset":0},
    {"name":"empty","inode":42,"type":4,"next_offset":7,"seek_offset":0},
    {"name":"data","inode":73,"type":8,"next_offset":99,"seek_offset":0}]}}]}}
```

[XNU getdirentries64](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [dirent ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/dirent.h), [extended flags](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/dirent_private.h).

Enumeration validation (2026-10-05, Release): 498 Darwin registrations, 246 passed, 252 unavailable-backend skips, zero failures; all 63/63 required ARM64 HVF cases executed. All 11 original native macOS workloads, 40 public C/CLI/report checks without skips, five Python guest combinations with eight file workloads, and 66 runner tests passed. Counts overlap. Evidence: `build-hvf-arm64/darwin-dirents-merged-evidence/`. Intel HVF Actions remain suspended; other native transports and physical iOS are unvalidated.

## Private file mappings

`mmap` accepts `MAP_PRIVATE` regular catalogue-file mappings (`flags=0x2`
or `0x40002` with `MAP_UNIX03`). Offsets must be OS-page aligned. The complete
mapped pages contain the current file bytes, including bytes beyond a short
requested length; the remaining part of the final EOF page is zero. Private
writes change only that mapping. Independent mappings, original file bytes,
fixed metadata and shared descriptor cursors remain independent. Closing or
reusing the descriptor does not retire an existing mapping. Read-only and
PROT_NONE mappings are initialized before their guest protections apply;
`mprotect` can subsequently grant WRITE, which implies READ.

File-end overflow and UNIX03 zero-length/alignment errors return EINVAL before
FD lookup; an invalid FD returns EBADF before memory-budget admission. Legacy
zero length still validates the FD and returns address zero without allocating.
Legacy unaligned offsets, stream descriptors, empty-file pages and any complete
page beyond EOF stop as unsupported before allocation. Native macOS accepts
such EOF mappings but faults with SIGBUS on access; this model does not invent
zero-filled readable pages or claim to deliver that signal. Shared, fixed,
executable and JIT mappings remain outside the contract.

`DarwinFiles` alone owns shared file contents and resolves descriptor kind and bytes.
`DarwinMemory` owns placement, page allocation, maximum protection, mapping leases
and rollback; mapping bytes are
charged to the existing memory limit. All file data comes from `darwin_files`,
with no host-file passthrough. The independent `file-mapping` workload checks
private writes, close lifetime, cursor preservation, errors and anonymous page
reuse. A separate native comparison checks a nonzero file offset, every byte
of the final page and the real SIGBUS boundary. ABI reference:
[XNU mmap](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mman.c).

## Explicit file metadata

A file entry may additionally contain `metadata`. When present, every field
shown below is required. Decimal strings preserve full-width integers; numeric
JSON is limited to exactly represented integers within ±(2^53−1). Device IDs
are signed 32-bit; mode and link count are unsigned 16-bit; inode is unsigned
64-bit; UID, GID, flags and generation are unsigned 32-bit. `size` must match
`bytes_hex`, blocks fit signed 64-bit, and block size fits nonnegative signed
32-bit. Times use signed 64-bit seconds and nanoseconds in [0, 999999999].
The mode must match the catalogue kind, with optional permission bits.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839","metadata":{
  "device":1,"inode":"18364758544493064720","mode":33188,"link_count":1,
  "uid":1000,"gid":1000,"size":10,"block_size":4096,"blocks":8,
  "flags":0,"generation":0,
  "access_time":{"seconds":-1,"nanoseconds":1},
  "modification_time":{"seconds":2,"nanoseconds":3},
  "change_time":{"seconds":4,"nanoseconds":5},
  "birth_time":{"seconds":6,"nanoseconds":7}
}}]}}
```

`stat64` (338), `fstat64` (339) and `lstat64` (340) return one 144-byte LP64
record on ARM64 and x64. Path queries share `open`'s component resolver, while
FD queries follow descriptor duplication and closure. The regular-file rdev,
padding and reserved fields are zero. Inputs supply the initial metadata; the
optional mutation policy governs changes. Reads do not change timestamps, and
mode bits do not change catalogue access.
Missing metadata, stream status, legacy stat layouts and
extended-security variants remain unsupported. Missing paths and
bad descriptors precede output-pointer checks; partial output stops before any
bytes are written. Status queries neither allocate FDs nor change cursors.
The ABI is described by XNU's [stat records](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/sys/stat.h).
The native test compares all record bytes with a real filesystem observation
and checks offsets and widths against the macOS SDK. The authored raw-call
fixture independently checks all three services against the native kernel.

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
Apple Silicon HVF, Intel HVF, Linux KVM and Windows WHP. The historical Darwin
inventories below passed on their respective matching hosts. Those results do
not validate subsequently added file services on Intel HVF, KVM or WHP. Intel
HVF runtime remains unvalidated and its Actions testing stays suspended. See the
[HVF validation record](macos-hvf.md#implementation-validation-2026-10-02-to-2026-10-03).

The focused workload gate additionally requires **every** Darwin process case
on each platform supported by the host ISA: 111 cases on ARM64, or 74 on x64.
Both `LC_MAIN` and independent raw `LC_UNIXTHREAD` programs are required on
every supported platform. A source-inventory regression ensures each new
Darwin process test joins this required set.
On a native macOS build, `DarwinNativeTests.cpp` also runs the same authored
object against the host kernel. A separate host executable links libSystem
only for dyld's real main handoff; guest images remain import-free. Return,
exit, memory protection/reuse and oversized-write error ordering must match
the fixture's exit status and exact output. The file and nocancel cases reuse
the same authored object with an isolated host file containing the guest's
exact bytes. Descriptor replacement/redirect cases also run on the host.
Memory-only tests cover both OS page sizes, denied and partial copyout,
configuration admission, absent/empty input and FD exhaustion. Public C/CLI
and Python checks cover file, nocancel, binary stdin, output redirection and
stat64 observations.
on every platform/ISA combination. This native reference is required
by the HVF gate; it does not establish native iOS execution.
The independent [native kernel workflow](../.github/workflows/darwin-kernel-reference.yml)
runs these same cases on both Intel and Apple Silicon macOS without building
NeverD or LLVM. `DarwinNativeCases.def` owns the modes and expected byte output
for both runners. Wrong host architectures, Rosetta, timeouts and any result
mismatch fail the reference gate; its JSON records the source, OS and compiler.
Run it locally with `python3 scripts/run_darwin_kernel_reference.py
--architecture arm64 --evidence build-kernel-reference` (use `x86_64` on Intel).
The focused workload gate preserves the full inventory and original test XML, and fails on missing or
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

### Metadata verification, 2026-10-05

After adding stat64, the Release gate reconciled 409 unique registrations:
193 passed, 216 skipped and zero failed. All 54 required ARM64 HVF outcomes
ran, and Unicorn covered all five guest combinations. Native SDK layout and
full-record comparisons passed, along with all eight original macOS programs,
36 public/report cases without skips, Python on all five combinations and
66 evidence-runner tests. Counts overlap. The native harness now uses a separate
output file per case, fixing stale trailing bytes after shorter outputs.
These additions still have no native Intel HVF/KVM/WHP or physical iOS evidence.

### Private mapping verification, 2026-10-05

The Release Darwin gate reconciled 438 unique registrations: 210 passed,
228 skipped and zero failed. All 57 required ARM64 HVF cases executed;
Unicorn covered all five guest platform/ISA combinations. All nine original
native macOS programs passed. A separate native test compared every byte of
a nonzero-offset EOF page and confirmed SIGBUS in an isolated child for the
next complete page. All 36 public/report checks ran without skips, and Python
executed all five combinations, including `file-mapping`. The 66 evidence-runner
and 38 provenance regression tests passed. Counts overlap. Evidence is under
`build-hvf-arm64/darwin-mmap-verified-evidence/`; this adds no Intel HVF/KVM/WHP
or physical iOS acceptance. Intel HVF Actions remain suspended.

### Remaining environment work

1. Extend the bounded writable-file model with permission enforcement, rename between separate undeclared directory domains and remaining directory operations,
   shared mappings and EOF fault delivery. Keep native acceptance for cursor,
   mapping lifetime and error-order interactions as the supported set grows.
2. Extend fixed time inputs with advancing clocks and additional system observations,
   and add required Mach/thread services,
   then Mach-O dependency loading, rebases/binds, initializers and TLS. Validate
   small real executables at each boundary before admitting general libraries.
3. Add Objective-C/Swift and Foundation/UIKit behavior with executable native
   references. Physical iOS comparison needs an iOS SDK and device environment;
   Intel HVF remains unvalidated and its Actions workflows remain suspended.

### File services verification, 2026-10-05

The Release Darwin gate on Apple Silicon, with HVF and Unicorn enabled,
reconciled 381 registrations: 177 passed, 204 skipped and zero failed. All
51 required ARM64 HVF workloads executed. The software backend exercised
all five platform/ISA combinations. The seven original native macOS cases,
35 process-report/public C/CLI tests, Python's five Darwin combinations and
66 evidence-runner tests passed. Counts overlap and must not be added.
The new file services have no native Intel HVF, KVM or WHP evidence; earlier
results below refer to their recorded inventories. The host lacks an iOS SDK,
and neither guest execution nor macOS references establish iOS device results.

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

The later clean-source method run at
`d5864c055116a687546320e4acf0788ef4a4e735` again passed all 39 ARM64 Darwin
workloads (65 passed, 221 skipped), with all 286 CTest identities reconciled
against original GoogleTest XML. The full twenty-owner run at that source
passed 849 cases, failed none and skipped 5,993 across all 6,842 registrations.
Its sixteen native requirements all passed. These runs include the subsequent
Windows environment changes and explicitly record method-level process
isolation. Evidence is in `build-hvf-native/hvf-method-clean-{full,darwin}-evidence/`.

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

The [Intel HVF Darwin gate](https://github.com/NeverSight/NeverD/actions/runs/37106013999)
at clean source `8dcc74c59da303176801b99747a60339161b824b` passed every required
x64 workload: **26/26**, across macOS and iOS Simulator. All 286 CTest
registrations were reconciled against original GoogleTest XML: **52 passed,
0 failed, 234 skipped**, with no missing, duplicate or unexecuted required
results. The 234 skips are 65 disabled-Unicorn cases, 39 foreign ARM64
guests and 130 foreign-host backends. All 32 method processes exited
successfully. The native macOS kernel reference also passed; these results do not establish iOS device-kernel or
broader Intel CPU acceptance.

Artifact `11267489438` was downloaded and verified against SHA-256
`cd8fabbd7d031ac4ad7b891b8e5a52f3e3abe3c39306d9c4a1893e40912e78ef`.
Evidence is retained under `build-hvf/verification/hvf-intel-darwin-accepted/`.
Execution uses the documented serial method policy on hosted Intel, including
all parameters and the CTest environment. The runner was macOS x86-64 with
four logical CPUs and Darwin 24.6.0. The same run also passed its ten-case
transport preflight, 100 interruption/recovery repetitions and isolated CR8
regression; these overlapping checks are not added to the Darwin totals.

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

## Explicit time observations

`ProcessOptions::DarwinTime` / `darwin_time` supplies fixed observations for raw `gettimeofday` (116), including its third `mach_absolute_time` output, on every Darwin profile. Each of `time_of_day`, `timezone` and `mach_absolute_time` is optional: absence means unknown, while explicit zero is a value. An empty object does not supply default clocks. The model never reads host clocks, infers timezone, advances time or converts absolute ticks.

Every member of a supplied record is required. `seconds` is unsigned 32 bits, `microseconds` is in [0, 999999], and `minutes_west` / `dst_time` are signed 32 bits. Absolute ticks are unsigned 64 bits. JSON uses the shared lossless integer rules: quote decimal values outside the safe JSON integer range. Unknown fields, invalid ranges and use on non-Darwin profiles fail before image loading.

The LP64 `timeval` is 16 bytes: zero-extended seconds at offset 0, 32-bit microseconds at 8, and four zero padding bytes at 12. The timezone is two signed 32-bit fields; ticks occupy eight bytes. Requested calendar and absolute values form one initial sample, so both requested observations must exist before any output copy or pointer check. Copies then occur in order: timeval, timezone, absolute ticks. Missing timezone is detected at its phase, preserving an earlier timeval write. A later EFAULT also preserves earlier writes, and aliases follow the same order. An individually partially writable output stops unsupported before that copy, retaining prior copies. All-null arguments succeed without configuration; selective queries require only requested values.

The original `time` workload checks native behavior; guest `time-values` emits the exact configured 32 bytes through C/CLI/Python on all five guest combinations. A separate SDK oracle compares every byte against three outputs captured in one raw native call. These fixed observations do not implement advancing clocks, clock conversion, commpage counters, timers or Mach clock objects/IPC. Dyld, threads, Objective-C/Swift and Foundation/UIKit remain separate environment work. Intel HVF Actions remain suspended; this adds no native Intel or physical iOS acceptance.

```json
{"darwin_time":{"time_of_day":{"seconds":4045620583,"microseconds":654321},"timezone":{"minutes_west":-480,"dst_time":-1},"mach_absolute_time":"18364758544493064720"}}
```

[XNU gettimeofday](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_time.c), [time ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/time.h).

Time validation (2026-10-06, Release): 538 Darwin registrations, 274 passed, 264 unavailable-backend skips, zero failures; all 66/66 required ARM64 HVF cases executed. All 12 original native macOS workloads and the independent single-sample SDK byte comparison passed. Public C/CLI/report: 43/43, no skips. Python passed all five guest combinations, including exact time bytes and the eight existing file modes. All 66 runner tests, localization, capability and format checks passed. Counts overlap. Evidence: `build-hvf-arm64/darwin-time-verified-evidence/`, `darwin-time-native-first/` and `darwin-time-public.xml`.

## Mach time and return conventions

`darwin_time.timebase` supplies `numerator` and `denominator`, both nonzero unsigned 32-bit integers. The exact ratio is retained, without reduction or conversion. Mach `mach_timebase_info_trap` uses index 89: ARM64 X16=-89 or x64 RAX=0x01000059. It writes eight little-endian bytes (numerator, denominator) and returns zero. As in XNU, a wholly invalid output address still returns zero; an individually partially writable output stops before copying because its prefix effects remain unmodeled. Transport failures propagate. Missing timebase configuration stops before pointer checks, including a null pointer.

ARM64 special traps X16=-3 and X16=-4 return the complete unsigned 64-bit `mach_absolute_time` and `mach_continuous_time` observations. Each requires only its own value; explicit zero is valid. These slots are unsupported on x64, where the corresponding native Mach table entries raise EXC_SYSCALL. This does not implement advancing clocks, commpage counters, timers or Mach clock objects/IPC.

Service lookup uses only the low 32 bits of the number register; reports retain all original 64 bits. ARM64 negative numbers select Mach; x64 uses Mach class 0x01000000 and BSD class 0x02000000. Namespaces remain distinct, including BSD read/write numbers 3/4. Unknown numbers and foreign classes stop explicitly. The resolved binding owns the return convention: Mach preserves incoming flags and X1/RDX, while BSD retains its documented carry/secondary rules. X64 still updates RCX/R11 for SYSCALL return. Returning Mach records contain `result` and omit `error`, even when incoming carry is set.

The original `mach-time` workload compares flag/secondary preservation, high number bits, invalid pointers and transitions back to BSD against the ARM64 host kernel. `mach-timebase-values` emits exact configured bytes on all five guest combinations; `mach-clock-values` does so on ARM64. An independent SDK oracle checks layout and a captured native ratio. Intel HVF Actions remain suspended; x64 software tests and syntax checks do not establish native Intel or physical iOS acceptance.

```json
{"darwin_time":{"timebase":{"numerator":125,"denominator":3},"mach_absolute_time":"18364758544493064720","mach_continuous_time":"18446744073709551615"}}
```

[XNU clock traps](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/kern/clock.c), [ARM64 entry](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/bsd_arm64.c), [ARM64 special traps](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/sleh.c), [x64 entry](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/x86_64/idt64.s).

Mach validation (2026-10-06, Release): 569 unique Darwin registrations, 293 passed, 276 unavailable-backend skips, zero failures; all 69/69 required ARM64 HVF cases executed. All 13 native workloads and both time SDK oracles passed in the final run. Public C/CLI/report: 100/100, no skips; Python covered all five guest combinations. Public comparisons run separately by platform and workload with an explicit 10-second guest budget; product defaults and deadline regressions are unchanged. Counts overlap.

Initial cold native launches exceeded the existing five-second reference limit: one independently measured 6.056 seconds, then 0.010 seconds on reuse. The unchanged native binary subsequently passed all 13 cases under the original limit; failed summaries remain retained. Separate serial verification passed after earlier wall-clock timeouts under host load. Evidence: `build-hvf-arm64/darwin-mach-time-final-evidence/`, `darwin-mach-time-native-recheck/existing-binary-recheck.json`, `darwin-mach-time-public-accepted.xml`. Evidence describes the pre-commit working tree. At that revision, ARM64 MRS/MSR NZCV were outside the checked CPU contract, so the fixture used integer instructions to observe flags. The increment below addresses that CPU gap.

## ARM64 condition flag register

The shared checked ARM64 contract admits exact `MRS Xt, NZCV` and `MSR NZCV, Xt` encodings at EL0 and EL1. Reads return only bits 31–28; writes take only those input bits and ignore all others. Reading into `XZR` discards the result; writing from `XZR` clears the four flags without reading SP. The original instructions run through each transport. The host register setter's validation and the bounded FPCR/FPSR policy are unchanged; neighboring unlisted system registers remain unsupported.

`NeverDAArch64NZCVTests` compares all flag combinations against original host instructions and checks complete scalar/vector state, memory, register boundaries, observer stop/failure, saved-context retry and shared instruction budgets. The ARM64 `mach-time` fixture now uses real MSR/MRS around SVC, covering preservation through Mach and transitions to BSD. Native HVF requirements include all six methods at both privileges plus the host oracle. ARM64 KVM/WHP and physical iOS remain unvalidated. Writable files, system information, advancing clocks, Mach IPC/threads, dyld/runtime/frameworks and device acceptance remain separate environment work.

[Arm NZCV (DDI0601, 2025-06)](https://developer.arm.com/documentation/ddi0601/2025-06/AArch64-Registers/NZCV--Condition-Flags).

## Mutable-file validation (2026-10-06)

The Release Darwin gate passed 322 of 610 registrations, with 288 unavailable-backend skips and no failures; all 72 required ARM64 HVF identities executed. The final focused run passed 102 of 114, with 12 unavailable skips, including the subsequently added EFAULT-metadata assertions. All 15 independent native workloads and 111 public C/CLI/report tests passed. These counts overlap. Evidence is retained in `build-hvf-arm64/writable-darwin-evidence/`, `writable-native-final/`, `writable-focused-final.xml` and `writable-public.xml`. The first native writable run exposed FWASWRITTEN and is retained separately; it was corrected before the passing runs. No deadlines changed. Full GitHub CI and physical iOS remain separate; Intel HVF Actions remain suspended.

Python's initial full integration run timed out in directory enumeration on three ARM64 profiles. An unchanged-argument observation passed all ten new writable scenarios, but one iOS directory call consumed 5.005 seconds wall time for 1.263 seconds CPU time and timed out. Isolated directory rechecks with the same five-second bound then passed all three ARM64 profiles in 2.43–3.17 seconds, each retiring 10,941 instructions and emitting `65`. Host load was 54–70 on 16 logical CPUs. These observations support scheduling pressure; they do not establish stable latency or erase the failed runs. Evidence: `writable-python-observation.json` and `writable-python-directory-recheck.json`.

The final unmodified Python integration method passed all five profile/ISA combinations in 41.118 seconds under the original five-second per-process limit. The earlier failed and diagnostic runs remain separate evidence.


### Mutable metadata verification, 2026-10-06

Release focused coverage: 148 registrations, 124 passed, 24 unavailable-backend skips,
zero failures. Full Darwin run: 645 registrations, 343 passed, 300 unavailable skips,
and two existing ARM64 HVF directory-enumeration timeouts. The unchanged 20-case
method recheck passed 8 with 12 unavailable skips; affected cases took 3.818/3.949s
under the original 5s limit. All 75 required ARM64 HVF identities have passing
observations across these runs; the first gate remains recorded as failed.
Public C/CLI/report 117/117 passed, including 73 Darwin comparisons; the unmodified
Python integration method passed all five profiles in 27.359s. Native original
workloads 15/15 and runner tests 66/66 passed. New allocation results are explicit
virtual-policy tests, not APFS observations. Evidence:
`build-hvf-arm64/mutation-metadata-validation-summary.json` and
`mutation-metadata-darwin-evidence/`, `mutation-metadata-directory-recheck/`,
`mutation-metadata-focused.xml`, `mutation-metadata-public.xml`.
No deadlines changed. Full GitHub CI, native Intel and physical iOS remain separate
acceptance requirements; the full macOS/iOS goal is still incomplete.

### Sparse-seek verification, 2026-10-06

The Release Darwin gate reconciled 671 registrations: 359 passed, 312 unavailable-backend skips, zero failures. All 78 required ARM64 HVF identities executed; Unicorn covered all five guest profiles. Focused file/memory/sparse-seek coverage passed 123 of 147 with 24 unavailable skips. All 16 original native programs and 122 public C/CLI/report checks passed, including 78 Darwin comparisons. The unmodified Python integration method passed all five profiles in 12.344 seconds. Evidence-runner tests passed 66/66. Counts overlap; no deadlines changed. Evidence: `build-hvf-arm64/sparse-seek-validation-summary.json`, `sparse-seek-darwin-evidence/`, `sparse-seek-native/`, `sparse-seek-focused.xml` and `sparse-seek-public.xml`. Earlier failed runs remain preserved. Allocation geometry is an explicit virtual policy, not APFS equivalence. Full GitHub CI and physical iOS remain separate; Intel HVF Actions stay suspended.

### Unlink verification, 2026-10-06

Release Darwin reconciled 708 registrations: 384 passed, 324 unavailable-backend skips, zero failures; all 81 required ARM64 HVF identities executed. Focused coverage passed 137 of 156 with 19 unavailable skips. All 17 original native programs and 128 public C/CLI/report checks passed, including 83 Darwin comparisons. The unmodified Python method passed five profiles in 16.268 seconds; runner tests passed 66/66. Independent design and implementation review found no remaining blocker. Counts overlap, deadlines are unchanged and no recheck was needed. Evidence: `build-hvf-arm64/unlink-validation-summary.json`, `unlink-darwin-evidence/`, `unlink-native/`, `unlink-focused.xml` and `unlink-public.xml`. Directory invalidation and fixed policy times are explicit model behavior; these results do not establish full filesystem/runtime or physical iOS compatibility. Intel HVF Actions remains suspended; complete GitHub CI is separate.

### Creation verification, 2026-10-06

Release Darwin: 748 registrations, 412 passed, 336 unavailable-backend skips, zero failures; all 84 required ARM64 HVF identities executed. Focused: 162 registrations, 150 passed, 12 unavailable skips. Public C/CLI/report: 133/133, including 88 Darwin comparisons. Python passed all five profiles in 9.982 seconds; original native programs 18/18 and evidence runners 66/66 passed. Initial ARM64 creation runs correctly rejected pointer-table rebases introduced by the fixture; replacing the table with inline bytes removed them without changing loader admission. The original failures and binaries remain preserved. The runner inventory expectation was updated from 27 to 28 workloads. Independent review found no remaining blocker, including added parent-observation rollback coverage. Counts overlap and deadlines are unchanged. Full GitHub CI and physical iOS remain separate; Intel HVF Actions stay suspended.

`build-hvf-arm64/create-validation-summary.json`, `create-darwin-evidence/`, `create-focused.xml`, `create-public.xml`, `create-native-final/`, `create-initial-evidence/`.

### Creation metadata verification, 2026-10-06

Release Darwin reconciled 787 registrations: 439 passed, 348 unavailable-backend skips, zero failures; all 87 required ARM64 HVF identities executed. Focused coverage passed 139 of 151 with 12 unavailable skips. Public C/CLI/report passed 145/145, including 98 Darwin input comparisons; the unmodified Python method passed five profiles in 12.211 seconds. Original native programs passed 19/19 and evidence runners 66/66. Independent review found no remaining blocker; added cases cover cross-parent device/group and global inode allocation, unlink before the first write, and umask with no free FD or usable guest input. Counts overlap; deadlines are unchanged and no failure recheck was needed. Fixed creation/mutation times and allocation remain explicit virtual policy, not native APFS observations. Full GitHub CI and physical iOS remain separate; Intel HVF Actions stay suspended.

`build-hvf-arm64/creation-metadata-validation-summary.json`, `creation-metadata-darwin-evidence/`, `creation-metadata-focused.xml`, `creation-metadata-public.xml`, `creation-metadata-native/`.

### Rename verification, 2026-10-06

Release Darwin reconciled 835 registrations: 474 passed, 360 unavailable-backend skips and one existing macOS ARM64 HVF virtual-metadata timeout (5.087s). The unchanged 20-case method recheck passed 8 with 12 unavailable skips; the affected identity took 0.113s under the original 5s limit. All 90 required ARM64 HVF identities have passing observations across these runs; the first final gate remains failed. Rename-focused coverage passed 42 of 54, with 12 skips. Public C/CLI/report passed 150/150, including 103 Darwin input comparisons; the unchanged Python method passed five profiles in 18.478s. Original native programs passed 20/20 and evidence runners 66/66. Independent review caught a nested-dot target classification error; its 4K/16K regression failed before the fix and passed afterward. An earlier test-only readonly-ftruncate expectation was corrected to EINVAL. All failures and probe revisions remain preserved, counts overlap and deadlines are unchanged. Full GitHub CI and physical iOS remain separate; Intel HVF Actions stay suspended.

`build-hvf-arm64/rename-validation-summary.json`, `rename-focused-final.xml`, `rename-darwin-final-evidence/`, `rename-metadata-recheck.xml`, `rename-public.xml`, `rename-native-final/`, `rename-review-initial-evidence/`.

## Explicit system observations

`ProcessOptions::DarwinSystem` / `darwin_system` supplies fixed observations for `sysctl(202)` and raw `sysctlbyname(274)` on every Darwin profile. Each field is optional; an omitted value or unlisted key stops as unsupported. There are no host queries or inferred version/model defaults. Strict JSON and typed validation reject malformed values and non-Darwin profiles before image loading.

`os_revision` is signed 32-bit; `cpu_count` is 1 through INT32_MAX; `memory_size` retains all unsigned 64 bits; `max_files_per_process` is 0 through INT32_MAX and has a four-byte int encoding. The remaining scalar fields are strings of at most 1023 bytes (255 for `hostname`) without embedded NUL; explicit empty strings are valid and results include the terminating NUL. These observations do not change scheduling or allocation/descriptor budgets.

| JSON field | sysctl name | MIB |
| --- | --- | --- |
| `os_type` | `kern.ostype` | `1,1` |
| `os_release` | `kern.osrelease` | `1,2` |
| `os_revision` | `kern.osrevision` | `1,3` |
| `kernel_version` | `kern.version` | `1,4` |
| `os_version` | `kern.osversion` | `1,65` |
| `machine` | `hw.machine` | `6,1` |
| `model` | `hw.model` | `6,2` |
| `cpu_count` | `hw.ncpu` | `6,3` |
| `memory_size` | `hw.memsize` | `6,24` |
| `max_files_per_process` | `kern.maxfilesperproc` | `1,29` |
| `hostname` | `kern.hostname` | `1,10` |

`hw.pagesize` comes from the existing guest memory policy: normally eight bytes, or four when a nonnull output has capacity exactly four. The legacy MIB `[6,7]` and name `hw.pagesize_compat` always return four bytes. The dynamic numeric OID for `hw.pagesize` remains unsupported. `hw.memsize` also narrows at exact capacity four only when its 64-bit pattern is a sign extension of a signed 32-bit value; otherwise ERANGE34 preserves output and length.

MIB counts use the low 32 bits and must be 2–12; named lengths use all 64 bits and must be below 1024. Every supplied name byte is checked before interpreting the first NUL and removing one final dot. An empty name returns ENOENT. Partial input remains unsupported. A nonnull `oldlenp` must be fully readable and writable for eight bytes before effects; faulting native length probes did not return within their deadlines, so those pointers remain an explicit unsupported boundary. Null `oldlenp` means capacity zero; null `oldp` requests only the size. For keys other than `kern.hostname`, a short buffer returns ENOMEM12, leaves data untouched and writes length zero. Data EFAULT preserves the old length. Input and capacity are captured before data, with the final length copy last, including aliases and retained earlier copies after transport failure.

`hostname` declares the bytes visible to this guest caller, without host lookup, a mobile `localhost` default or inferred entitlements. Missing is unknown; empty returns one NUL. For `kern.hostname`, a nonnull output with positive short capacity succeeds with exactly that many bytes, ending in NUL, and reports that capacity. Zero capacity still returns ENOMEM12 with length zero and no data; a null output reports the complete NUL-inclusive length. Only the actual output span is checked. A partially writable span remains unsupported without prefix publication; native partial-copy behavior is outside this model. This adds the raw observation used by libc uname/gethostname, not their dylib imports or a complete runtime.

Only nonzero `newp` together with nonzero `newlen` is a write request. After the original name/MIB and complete oldlenp read/write preflight, default or explicit non-root EUID returns EPERM1 before observation lookup or data-output access. With explicit EUID0, privileged-writable `kern.osversion / kern.maxfilesperproc / kern.hostname` stops unsupported because privileged writes are not modeled; real UID does not decide this branch. Native read-only selected nodes retain EPERM1 even for root. A pointer with zero new length is ignored. Unknown keys, other trees and dynamic OIDs do not acquire guessed ENOENT results.

The original `system-info` program checks native macOS and guest ABI behavior; `virtual-system` compares exact configured bytes through C++, C/CLI and Python. A separate SDK oracle captures all nine host observations as explicit test inputs and compares both named and numeric output. This does not provide iOS device or Intel HVF acceptance.

```json
{"darwin_system":{"hostname":"guest-node","os_type":"Darwin","os_release":"24.test","os_revision":0,"kernel_version":"Virtual kernel","os_version":"V42","machine":"virtual64","model":"VirtualModel","cpu_count":4,"memory_size":"17179869184"}}
```

[XNU sysctl](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_newsysctl.c), [XNU hardware MIB](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mib.c), [Apple sysctl(3)](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man3/sysctl.3.html).

### System-query verification, 2026-10-06

Release Darwin reconciled 881 registrations: 509 passed, 372 unavailable-backend skips and zero failures; all 93 required ARM64 HVF identities executed. Focused checks passed 37 of 49 with 12 unavailable skips. Public C/CLI/report passed 163/163, including 113 Darwin input comparisons. The unchanged Python method passed all five profiles in 15.302s; original native programs passed 21/21 and evidence runners 66/66. Independent review found no blocker; its extra error-priority combinations and the separate SDK capture oracle passed. One new oracle compilation failed for a missing StringExtras include and passed after adding it; that log and source are preserved. Faulting native length-pointer probes remain preserved and outside the admitted contract. Only two file-header comments were normalized after the passing runs, followed by a successful rebuild. Counts overlap, deadlines are unchanged, and no runtime recheck was needed. Full GitHub CI and physical iOS remain separate; Intel HVF Actions stay suspended.

`build-hvf-arm64/sysctl-validation-summary.json`, `sysctl-darwin-final-evidence/`, `sysctl-focused-final.xml`, `sysctl-public.xml`, `sysctl-native-final/`, `sysctl-sdk-build-failure/`, `sysctl-initial-probe-evidence/`.

## Vectored file and capture I/O

`readv`/`writev` and `preadv`/`pwritev`, including nocancel entries, share the scalar file and capture owners. No new input options or host access are introduced. LP64 iovec records contain an eight-byte address and eight-byte length. The signed low 32 bits of iovcnt must be 1–1024. The complete array is copied before descriptor lookup, so later output aliases cannot rewrite the request. An individually partial array remains unsupported.

Descriptor access and positioned-stream checks precede length validation. Each length and their sum must fit INT64_MAX; regular files/directories additionally require total <= INT_MAX. Streams have no vnode limit: finite stdin clips to available bytes, and capture retains its output budget. Every negative pwritev offset is rejected before the array; preadv checks offsets after descriptor/length admission. Zero spans ignore their addresses, while descriptor, directory and offset rules still apply. EOF skips all unused tails. Positioned calls preserve the open cursor and pwritev ignores append. Ordinary append clips the entire request against the original cursor once before selecting EOF.

Copies follow vector order. A later wholly invalid span returns EFAULT while retaining earlier completed bytes, ordinary cursor progress and FWASWRITTEN after nonzero written bytes. An admitted nonzero file write that returns data-buffer EFAULT invalidates complete metadata even with a virtual success policy; argument errors, model admission refusals and backend failures preserve it. A partially writable read span stops with UnsupportedService before copying that span; earlier copies remain. An individually partial file-write source remains unsupported before any file effects. File authorization, mapping leases and aggregate storage admission precede writes; backend preflight/read failures publish no file or capture bytes.

Capture uses the combined stdout/stderr allowance before vector data access. A span crossing the user address limit contributes no bytes; earlier vectors remain. Other partial readable capture spans retain their checked prefix with EFAULT. Scalar range-error priority over the output budget is unchanged. Duplicated and redirected descriptors retain their original sink. The original `vectored-io` workload exercises all eight entries on native macOS and all five guest combinations, including public C/CLI/Python. This does not add cancellation, pipes, threads or physical iOS acceptance.

`readv`: 120/411; `writev`: 121/412; `preadv`: 540/542; `pwritev`: 541/543.

[XNU vector calls](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/sys_generic.c), [XNU iovec lengths](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_subr.c), [XNU vnode I/O](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c).

### Vector I/O verification, 2026-10-06

Release Darwin reconciled 937 registrations: 553 passed, 384 unavailable-backend skips, zero failures; all 96 required ARM64 HVF identities executed. Focused checks passed 45/57 with 12 unavailable skips. Public C/CLI/report passed 168/168, including 118 Darwin input comparisons; Python covered all five combinations in 20.397s. Original native workloads passed 22/22, and runner tests 66/66. Independent review added a sparse positioned-write fault test that verifies cursor, actual EOF, metadata refusal and exact remaining storage capacity. Initial compilation still referenced a removed internal query in an old test; that test now checks actual captured output. An optional<bool> mistake in the new service-event assertion initially failed eight otherwise successful guest runs; it was corrected and all affected checks passed. Both failures retain source and logs. Counts overlap; deadlines are unchanged. Full GitHub CI and physical iOS remain separate, and Intel HVF Actions stay suspended.

`build-hvf-arm64/vector-validation-summary.json`, `vector-darwin-final-evidence/`, `vector-focused-final.xml`, `vector-public.xml`, `vector-native-initial/`, `vector-initial-build-failure/`, `vector-initial-assertion-evidence/`.

## File existence queries

Without `darwin_files.authorization`, `access(33)` and `faccessat(466)` query the current virtual catalogue without allocating a descriptor or changing file bytes, cursors, flags or metadata. F_OK proves that a name exists under the existing catalogue traversal contract. Metadata alone does not grant or revoke catalogue access; these legacy queries do not establish native ancestor-search, ACL or MAC authorization. Missing or invalidated stat observations do not prevent an existence query. Deleted names return ENOENT even while old descriptors or mappings retain the object; creation, name reuse and rename use the current namespace. The explicit static owner-query environment below has its own SEARCH and supported-operation rules.

Mode uses the low 32 bits. Native authorization actions come from R/W/X (bits 0–2) or extended rights (bits 9–21). When `(mode & 0x003ffe07) == 0`, the request is an existence query; other bits, including the sign bit, are ignored rather than rejected as EINVAL. Without the explicit owner-query environment, requested permissions remain UnsupportedService after successful lookup, even if metadata or mutation grants appear permissive. Known pathname/descriptor errors occur first. No permission result is guessed.

Faccessat accepts low-bit flags AT_EACCESS (0x10), AT_SYMLINK_NOFOLLOW (0x20) and AT_SYMLINK_NOFOLLOW_ANY (0x800), in any combination. Other flags return EINVAL before pathname or FD access, including when the catalogue is absent. AT_EACCESS selects explicit effective credentials in the owner-query environment; legacy existence queries do not authorize either identity. Fixed link lookup uses the policies described below. Absolute paths ignore dirfd; relative lookup retains configured CWD/directory-FD requirements. Non-AT_FDCWD nameiat routes import one byte and check a relative directory FD before full string import; a slash skips that FD check. An inaccessible first byte is EFAULT14; a later fault follows EBADF9/ENOTDIR20 for relative bad/file FDs. Empty relative paths still check the FD: EBADF for unknown, ENOTDIR for regular files, otherwise ENOENT. Missing catalogue and unknown stream-directory identity remain explicitly unsupported.

The independent `file-access` workload compares both raw calls, ignored mode bits, flag combinations and lookup order on native macOS and five guest combinations through C++, C/CLI/Python. Native NOFOLLOW_ANY checks use a relative directory FD so host `/tmp` or `/var` symlinks do not alter the reference. Direct tests cover live namespace changes, mixed permission bits, descriptor exhaustion, metadata independence and failed guest-memory access.

[XNU access and faccessat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU access mode bits](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/unistd.h).

### File-existence verification, 2026-10-06

Release Darwin reconciled 971 registrations: 575 passed, 396 unavailable-backend skips, zero failures; all 99 required ARM64 HVF identities executed. Focused checks passed 23/35 with 12 unavailable skips, including all 14 direct cases. Public C/CLI/report passed 173/173, including 123 Darwin input comparisons. Python passed all five combinations in 16.235s; original native workloads passed 23/23 and evidence runners 66/66. Independent design and implementation review found no blocker. Native probes preserve the initial NOFOLLOW_ANY result caused by a host /tmp symlink and the subsequent canonical-path comparison; the shared workload uses a relative directory FD. Counts overlap, deadlines are unchanged, and no runtime failure recheck was needed. Full GitHub CI and physical iOS remain separate; Intel HVF Actions stay suspended.

`build-hvf-arm64/access-validation-summary.json`, `access-darwin-final-evidence/`, `access-focused-final.xml`, `access-public.xml`, `access-native-initial/`, `access-evidence/`.

## Creating and removing directories

`mkdir(136)` and `mkdirat(475)` create directories in an explicitly mutable immediate parent. New directories inherit namespace mutation authority; initial directories retain their own grants. They inherit only the parent's known device/group identity, never complete stat records, directory size, allocation, timestamps or enumeration cookies. A new directory shadows all old observations at a reused file name. `creation_policy` still applies only to regular files: nested regular files use the inherited parent identity and the existing global inode sequence; mkdir consumes no regular-file inode. Permission enforcement and native directory metadata remain outside this contract.

All calls share the component walker. Mkdir admits a missing final name followed only by slashes; missing ancestors followed by dot/dotdot still return ENOENT, and a regular-file ancestor gives ENOTDIR. Known existing names give EEXIST. Relative paths retain FD/CWD requirements, absolute paths ignore dirfd, and string faults precede relative descriptor errors. Creation needs no available descriptor. Rejected lookup, grant, byte/entry budget or memory transport publishes no name or parent invalidation.

`rmdir(137)` and `unlinkat(472)` with AT_REMOVEDIR(0x80), optionally AT_SYMLINK_NOFOLLOW_ANY(0x800), remove empty directories created by this process or explicitly admitted initial directories as described below. Unknown low32 flags return EINVAL before input; DATALESS and SYSTEM_DISCARDED remain unsupported. Known path/type/root errors remain; initial-directory removal without a removable grant remains UnsupportedService. For admitted directories, final dot gives EINVAL and dotdot from a linked directory or a nonempty target gives ENOTEMPTY. Directory FDs, duplicates and CWD retain the original directory object and no longer prevent its removal. Open unlinked regular files and their mappings do not count as names: native comparisons confirm their bytes, inode and last linked F_GETPATH survive parent removal and name reuse.

Every created directory charges its canonical path plus NUL and one entry to the shared 16 MiB/256-entry catalogue limits. An orphan regular file retains its parent directory object through an open description or mapping lease. Removing every name and closing directory FDs does not refund those ancestors while that file remains retained. File reclamation precedes fixed-point directory reclamation; each unreachable created directory refunds its current path and entry exactly once. Successful namespace changes invalidate the direct parent's complete stat/enumeration observations; rejected changes preserve them. New directory metadata and snapshots remain unknown even with a regular-file creation policy. The original `directory-mutations` workload exercises nested creation, rename, unlink, deletion and orphan reuse across native macOS and all five guest combinations through C++/C/CLI/Python.

[XNU mkdir/rmdir](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU directory creation lookup](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_lookup.c).

### Directory mutation verification, 2026-10-06

Release Darwin: 1,017 registered, 609 passed, 408 unavailable-backend skips, zero failures; all 102 required ARM64 HVF cases executed. Focused checks: 58 passed, 12 unavailable skips, including 26 new direct 4K/16K cases. Public C/CLI/report: 178/178, including 128 Darwin input comparisons. Python passed five combinations in 17.255s; original native workloads 24/24 and runner tests 66/66 passed. Independent review checked admission, name reuse, parent identity, retained file leases and rollback. An extra native probe exposed a gap after the initial passing gate: slash-only root removal returns EISDIR, while root paths ending in dot/dotdot return EBUSY. The shared removal decision and common native/guest fixture now cover both; the earlier gate and source/binary snapshot remain preserved. Counts overlap; deadlines are unchanged. Intel HVF Actions remain suspended, with full GitHub CI and physical iOS outside this local acceptance.

`build-hvf-arm64/directory-mutation-validation-summary.json`, `directory-mutation-darwin-final-evidence/`, `directory-mutation-focused-final.xml`, `directory-mutation-public-final.xml`, `directory-mutation-native-final/`, `directory-mutation-before-root-fix/`.

## Retained directory identities

A removed directory keeps its original parent chain through FD/CWD references, including when parent names are removed and reused. Dot opens preserve that object with an independent cursor; dup shares its cursor. Dotdot follows the original parent, while ordinary child lookup in a removed directory returns ENOENT and cannot see a replacement namespace. LOOKUP may traverse retained removed parents; create/delete/rename lookup refuses that traversal with ENOENT. A terminal rename dot/dotdot returns EINVAL before that component is traversed, after earlier ancestor errors. F_GETPATH follows the retained object: moving a live created ancestor updates its descendant paths, including removed directories and orphan files; replacement/name reuse does not attach old objects to the new directory. Complete removed-directory stat and enumeration remain unknown. Each removed created directory keeps its current path+NUL and entry charge until no FD, CWD, retained child directory or held orphan file/mapping references it. Close, dup2, CWD changes and mutation admission reclaim unreachable file-then-directory chains; initial input costs stay reserved. The original `deleted-directories` workload compares deletion with held descriptors, parent/name reuse, lookup intent and CWD-only retention on native macOS and five guest profiles.

### Directory lifetime verification, 2026-10-06

Release: 1,051 Darwin registrations, 631 passed, 420 unavailable-backend skips, zero failures; all 105 required ARM64 HVF cases executed. Focused: 98 passed and 12 unavailable skips; initial direct checks: 64/64, including 14 new cases and updated held-removal coverage. Public C/CLI/report: 183/183, including 133 Darwin input comparisons. Python covered five combinations in 18.691s; native workloads 25/25, evidence runners 66/66. An additional native rename probe corrected terminal-dot error order after the initial focused pass; original sources/results and the earlier snapshot remain preserved. Primary source/evidence audit was completed; final independent review was unavailable. Counts overlap, deadlines are unchanged, and full GitHub CI and physical iOS remain separate. Intel HVF Actions stay suspended.

`build-hvf-arm64/directory-lifetime-validation-summary.json`, `directory-lifetime-darwin-final-evidence/`, `directory-lifetime-focused-final.xml`, `directory-lifetime-public.xml`, `directory-lifetime-native/`, `directory-lifetime-before-rename-fix/`.

## Removing explicitly admitted initial directories

A directory entry can opt into removal with strict Boolean `"removable": true`; C++ uses `DarwinFileOptions::RemovableDirectories`. This declares an ordinary non-mount directory with one namespace identity. It must be an explicit initial `directories` entry other than root, with an explicitly mutable immediate parent. Known special mode/flags, inode aliases (including directory snapshots), and conflicting known parent/target device numbers reject admission. Equal device numbers alone do not prove the absence of a mount. Omission or false retains the unsupported boundary; other JSON types are invalid. This grants no general permission or mount model.

```json
{"darwin_files":{"files":[],"directories":[{"path":"/","mutable":true},{"path":"/empty","removable":true}],"working_directory":"/empty"}}
```

Removal requires an empty live namespace. Initial implicit child directories remain present after their last original file is unlinked. Successful removal invalidates the object's and immediate parent's full stat/enumeration observations, while old FD/dup/CWD references retain the original object and parent chain. Removed initial names never reappear from immutable inputs. A new file or directory at that name has separate identity and cannot recover the old metadata or snapshot. Caller input remains unchanged.

Each removable reference charges its path plus NUL to the fixed initial 16 MiB footprint. Initial directory entries, paths, references and snapshots stay reserved after removal and final close; deleting an initial object never refunds these fixed costs or the initial entry within the 256-entry limit. New objects continue using their own dynamic accounting. The original `initial-directory-removal` workload removes the native harness's existing empty directory while held, reuses its name for a file and then a directory, checks CWD-only retention, and restores the empty directory. The same workload runs through C++/C/CLI/Python on all five guest combinations.

### Initial-directory removal verification, 2026-10-06

The final Release source reconciled 1,089 Darwin registrations: 657 passed, 432 unavailable-backend skips, zero failures; all 108 required ARM64 HVF cases executed. Focused checks passed 27/39 with 12 unavailable skips; the additional snapshot-only alias check passed. Public C/CLI/report passed 191/191, Python covered five combinations in 76.276s, original native workloads passed 26/26, and evidence runners passed 66/66. An initially inconsistent inode/snapshot test fixture was corrected; its failing results remain preserved.

Two earlier complete runs had one and three timeouts in existing file/rename cases; an instrumented run reproduced one file timeout at 5.008s wall time and 0.171s process CPU time. Exact-method and baseline-program comparisons passed, but the latency cause remains unresolved. The successful final run does not establish timeout stability. Temporary diagnostics were removed, fixture hashes restored, and the original 5s guest limits retained. Primary source/evidence audit was completed; independent review was unavailable. Counts overlap; full GitHub CI, physical iOS and suspended Intel HVF Actions remain outside this local acceptance.

`build-hvf-arm64/initial-directory-validation-summary.json`, `initial-directory-darwin-restored-evidence/`, `initial-directory-public.xml`, `initial-directory-native-evidence/`, `initial-directory-timeout-probe/`.

## Exclusive regular-file rename

After source and target lookup, RENAME_EXCL returns EEXIST for a distinct existing file or directory before mount and namespace-mutation checks. Earlier path errors still win, including terminal dot/dotdot EINVAL. An absent target uses the same bounded rename transaction, preserving held descriptions, cursors, flags, mapping leases and configured metadata transitions. Same-object exclusive rename remains explicitly unsupported: its native result depends on filesystem case sensitivity, which exact catalogue keys do not establish. Case folding, initial-directory-source rename and SECLUDE remain outside this contract. Created-directory EXCL moves use the subtree contract below. The existing original `renamed-file` workload now compares refusal without metadata changes and successful EXCL|NOFOLLOW_ANY moves through native macOS and C++/C/CLI/Python.

Verification, 2026-10-06 (Release): 1,097 Darwin registrations, 665 passed, 432 unavailable-backend skips, no failures; all 108 required ARM64 HVF cases executed. Focused: 44 passed and 12 unavailable skips, including eight new direct cases. Public C/CLI/report: 191/191; Python: five combinations in 19.241s. The independent raw-call probe passed 26 checks. The first complete native attempt timed out on the existing return case; the other 25, including renamed-file, passed. Three unchanged-binary return rechecks completed in 0.014–0.034s, then all 26 native cases passed with the original 5s bound. The initial failure remains preserved and unexplained; neither this result nor the previous final HVF pass establishes latency stability. Primary audit completed; independent review was unavailable. Counts overlap; physical iOS, full GitHub CI and suspended Intel HVF Actions remain outside local acceptance.

`build-hvf-arm64/exclusive-rename-validation-summary.json`, `exclusive-rename-darwin-evidence/`, `exclusive-rename-public.xml`, `exclusive-rename-native-evidence/`, `exclusive-rename-native-rechecked-summary.json`, `exclusive-rename-probe/`.


## Cross-parent rename in created directories

An initial directory and all descendants created from it by this process share one virtual namespace domain. `rename`, `renameat` and `renameatx_np` may move a regular file or a live process-created directory subtree between those parents; no additional JSON field is needed. For example, after creating `/work/left` and `/work/right` with mkdir/mkdirat under a mutable initial `/work`, `/work/data` can move into either child, and files can move between the children. A separately supplied initial `/work/left` is its own domain unless a movable or exchangeable declaration supplies its non-mount parent relation, as described below; matching devices alone do not join domains. General mount topology is unknown, so movement between separate undeclared domains remains explicitly unsupported.

Both immediate parents require namespace authority. Created directories inherit that authority and known device/group observations; rename preserves the file's own identity, owner/group, write grant and allocation. Known device conflicts still refuse. An actual move invalidates both parents' full metadata and enumeration. Removed initial directories and reused paths remain different objects; held old directory FDs/CWD do not acquire the replacement's domain.

The existing bounded replacement transaction, mapping retention, path/NUL charges and error precedence apply. EXCL against a distinct existing target still returns EEXIST before domain/grant checks. The original `renamed-file` workload now creates a child and moves into it, replaces a file back in the initial parent, then moves into the child again through C++/C/CLI/Python and native macOS. Permission enforcement, initial-directory moves, hard links, dynamic symbolic links and native APFS metadata remain separate work.

### Cross-parent verification, 2026-10-06

The final Release gate reconciled 1,115 Darwin registrations: 683 passed, 432 unavailable-backend skips, zero failures; all 108 required ARM64 HVF identities executed. Direct checks passed 56/56, including 18 new 4K/16K cases. Public C/CLI/report passed 191/191; the Python method covered five profiles in 22.254s. Original native workloads passed 26/26, the separate raw-call probe passed 34 checks, and documentation/capability/evidence-runner script tests passed 296/296. Counts overlap. All ten acceptance binary hashes remained unchanged after the MSVC-only CMake adjustment.

Two earlier complete gates retained three and two timeouts in the existing HVF file method. Exact-method, working-directory and session controls passed but did not establish their cause; the final pass does not prove latency stability. The probe initially compared lexical /tmp with canonical /private/tmp; obtaining the root FD path corrected four expectations. The first extended native workload used mkdir(136) for cleanup; rmdir(137) corrected its exit150 failure. Initial sources and failures remain preserved, and the original 5s guest bounds are unchanged.

Four LP64 test initializer-list conflicts found by full Linux CI now use explicit uint64_t values; NeverDJumpTableTests now receives /bigobj under MSVC. Actual Linux/Windows compilation awaits CI. Prior full CI also reported separate Windows EH corpus and closed-PR cancellation failures. Primary source/evidence review was completed; no independent review, physical iOS or suspended Intel HVF acceptance is claimed.

`build-hvf-arm64/cross-parent-rename-validation-summary.json`, `cross-parent-rename-darwin-synced-evidence/`, `cross-parent-rename-darwin-evidence/`, `cross-parent-rename-darwin-rechecked-evidence/`, `cross-parent-rename-public-synced.xml`, `cross-parent-rename-python-synced.log`, `cross-parent-rename-native-fixed-evidence/`, `cross-parent-rename-probe/`.

## Atomic exchange of file and created-directory names

RENAME_SWAP=0x2 exchanges two existing regular files, live process-created directories, declared exchangeable initial roots, or mixed file/directory pairs through renameatx_np, optionally with RENAME_NOFOLLOW_ANY. Initial roots require the separate declaration below. An explicit initial directory providing the filesystem capability must declare both mutable:true and swap_rename:true. DarwinFileOptions::SwapRenameDirectories supplies the C++ declaration. Created descendants inherit the original directory object's capability; deleting and reusing a path does not transfer the removed object's declaration. False or omission leaves support unknown. Matching devices and namespace grants alone do not establish support, and distinct undeclared initial directory domains remain unsupported.

Source and target use the existing component walker. For an admitted source root, a missing target returns ENOENT before source-dot/dotdot, domain, parent-grant or filesystem-capability checks, including a missing target with trailing slashes. Undeclared initial roots or removed directory operands remain explicitly unsupported. Either ancestor order, including a directory and its child file, returns EINVAL in the admitted domain. A regular-file target with trailing slashes retains ENOTDIR. An authorized ordinary-component same-object swap is a no-op, even without the capability declaration; same-object source-dot/dotdot still needs an unknown filesystem case-sensitivity property. EXCL+SWAP and unknown flags retain EINVAL before path input; SECLUDE remains unsupported.

All exchanged roots stay linked. Regular files retain their own identity, owner/group, bytes, write grants, descriptions, cursors, flags and mapping leases. Configured virtual policies update each file's own ctime; absent or invalidated policies leave full metadata unknown. Both parents' full metadata and enumeration become unknown after an actual exchange. No creation inode, entry or FD is consumed, and caller input stays unchanged.

Each capability reference reserves path+NUL in the fixed 16 MiB input budget. The transaction preflights both complete dynamic name charges, retaining linked bytes and leases without replacement credit, before publishing either name. This also excludes an unopened target's bytes. An initial file's first move acquires a dynamic path charge; swapping back does not erase it, and repeated swaps reuse it. Permission enforcement, mount topology, case folding and initial-directory moves remain separate work.

Directory exchanges follow parent objects in both subtrees, including nonempty live children, removed child directories and file orphans retained by FD or mapping. Mixed exchanges also move the exact regular-file root; an old unlinked file with the same last name stays attached to its original parent. Source/target FDs, duplicates, CWD and dotdot follow each object and its new parent. Descendant regular-file observations, bytes, write authority, offsets, flags and leases stay intact; moved roots and both immediate parents receive their existing namespace metadata updates.

All live and retained paths in both directions are prepared and checked against the 1024-byte canonical-path limit and shared 16 MiB budget before effects. Opposing names are withdrawn together before either subtree is published. No root is unlinked or credited by a swap, and no FD, entry or creation inode is needed. Failure preserves both namespaces, parents, cursors, observations and mappings. The original SDK-free `swapped-directory` program compares directory/directory and both mixed orders against native macOS and all five C++/C/CLI/Python profiles.

[Apple volume swap capability](https://developer.apple.com/documentation/foundation/urlresourcevalues/volumesupportsswaprenaming?changes=__1_2), [XNU rename flags and lookup order](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

```json
{"darwin_files":{"files":[],"directories":[{"path":"/work","mutable":true,"swap_rename":true}]}}
```

### Swap verification, 2026-10-06

Release Darwin reconciled 1,133 registrations: 701 passed, 432 unavailable-backend skips, zero failures; all 108 required ARM64 HVF cases executed. Direct rename checks passed 68/68, including 18 new option and 4K/16K cases. Public C/CLI/report passed 192/192; the Python method covered five profiles in 16.153s. Original native workloads passed 26/26 and the separate raw-call probe passed 45 checks. Counts overlap; guest limits are unchanged.

Two initial direct-test instances used incorrect expectations for unknown write authority and unknown post-mutation metadata; correcting the expectations preserved the existing semantic owner. An initial JSON filter selected zero tests and is excluded from acceptance; the actual owner and full public gate subsequently passed. Initial sources and results remain preserved. Earlier HVF/native latency remains unexplained; this pass does not establish stability. Primary source/evidence self-review was completed; no independent review, physical iOS, full GitHub CI or suspended Intel HVF acceptance is claimed.

`build-hvf-arm64/swap-rename-validation-summary.json`, `swap-rename-darwin-evidence/`, `swap-rename-public.xml`, `swap-rename-python.log`, `swap-rename-native-evidence/`, `swap-rename-probe/`.


## Process-created directory rename

Ordinary `rename`, `renameat` and `renameatx_np` move a live process-created directory and its subtree within one initial directory object's domain. Both immediate parents require namespace authority. A missing final directory target may have trailing slashes; a regular-file target returns ENOTDIR, a nonempty directory target ENOTEMPTY, and moving into a descendant EINVAL. An authorized ordinary same-name rename is a no-op. EXCL still returns EEXIST for a distinct existing target before type, cycle, domain or grant checks. Target lookup errors precede source-dot/dotdot rejection. A source dot/dotdot naming the same target object stays UnsupportedService because filesystem case sensitivity can change the native result. Without the initial-directory declarations below, initial directory sources and replacement targets remain unsupported. Removed sources, separate undeclared domains, links, permission enforcement and SECLUDE remain explicitly unsupported.

The transaction follows parent objects rather than matching text prefixes. Live children, FD-held removed child directories and unlinked files retained by descriptions or mapping leases acquire the moved ancestor's new canonical paths. Source directory FDs, duplicates, CWD and dotdot follow that same object and its new parent. A replaced empty created target keeps its own last path and original parent; its ordinary children stay absent, while dot/dotdot and CWD retain the removed object. If the new source later moves again, old target orphans at identical path strings stay attached to the old target.

Moving the subtree preserves descendant file bytes, identity, metadata, write authority, shared/independent cursors, descriptor flags and mapping leases. The moved directory and both immediate parents lose complete metadata/enumeration observations; child regular-file observations do not change merely because an ancestor moved. Created descendants retain namespace authority for future mkdir, file creation and admitted regular-file rename. No new catalogue entry, FD or creation inode is needed.

Before effects, the bounded transaction allocates all replacement keys and paths and preflights every live and retained descendant's canonical path+NUL against 1024-byte paths and the shared 16 MiB budget. Each previous dynamic name charge is replaced once. Only an empty created target with no FD, CWD or retained descendant ownership can fund replacement credit; final reclamation applies that credit once. Failure preserves every name, object parent, file observation, cursor and mapping. Mapping-only orphan files also retain their removed parent directory path and entry charges until the last lease is released.

The original SDK-free `renamed-directory` workload compares native macOS with all five guest profiles through C++, C, CLI and Python. Direct 4K/16K checks cover object/name reuse, whole-subtree rollback, exact capacity, long canonical descendants, target credits, mapping-retained ancestor reclamation and exhausted entries/descriptors/inode sequences.

[Apple rename contract](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/rename.2.html), [XNU lookup and same-object ordering](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).


### Created-directory verification, 2026-10-06

The final Release Darwin gate reconciled 1,175 registrations: 731 passed, 444 unavailable-backend skips, zero failures; all 111 required ARM64 HVF cases executed. Focused checks passed 32/44 with 12 unavailable skips, including 22 new direct 4K/16K cases; final error-boundary/sibling checks passed 4/4. Public C/CLI passed 165/165 and JSON/report passed 32/32, with no skips. Python covered five profiles in 21.759s. Original native programs passed 27/27; the independent raw-call probe passed 79 checks on this case-insensitive macOS filesystem. Counts overlap and deadlines are unchanged.

Independent plan and implementation review identified a root-path separator bug in the original fixture and the same-object source-dot filesystem-property boundary; both were corrected and reviewed. The initial fixture passed natively but exited124 at its first path assertion on all eight available guest transports. The first full gate retained two old direct expectations that still refused every created directory source; these now assert target EFAULT, and the final complete gate passed. Initial sources, binaries, failures and probes remain preserved. Prior unrelated HVF/native timeouts remain unexplained; this pass does not establish latency stability. Intel HVF Actions remain suspended, and physical iOS and full GitHub CI are separate acceptance boundaries.

`build-hvf-arm64/directory-rename-validation-summary.json`, `directory-rename-darwin-final-evidence/`, `directory-rename-public.xml`, `directory-rename-reports.xml`, `directory-rename-python.log`, `directory-rename-native-evidence/`, `directory-rename-probe/`, `directory-rename-initial-fixture/`, `directory-rename-darwin-evidence/failed-source/`.

### Directory and mixed-swap verification, 2026-10-07

The Release Darwin gate reconciled 1,219 registrations: 763 passed, 456 unavailable-backend skips, zero failures; all 114 required ARM64 HVF cases executed. Focused checks passed 32/44 with 12 unavailable skips, including all 24 new direct 4K/16K cases. The complete file owner passed 317/317. Original native workloads passed 28/28; a separate raw-call probe recorded 32 successful observations on this case-insensitive macOS filesystem. Counts overlap and guest deadlines are unchanged.

Independent plan and implementation review checked the two-way transaction, initial-file charges, both retained subtrees, mapping-only orphans and exact refunds. The first red fixture omitted its explicit root swap declaration and is excluded from acceptance; the corrected baseline failed all six selected cases at the old directory-swap refusal. Two early mixed-file assertions incorrectly discarded configured metadata; they now compare the full record with only ctime changed. A local constant pointer table introduced ARM64 rebases and caused six admission failures. Four scalar assertions removed that table; the fixed fixture has no classic rebases, and the image loader retains its unsupported-fixups boundary.

The first fixed-fixture focused run retained three five-second HVF timeouts. Individual and three-profile controls passed, followed by the complete gate above. Their cause remains unknown and this pass does not establish latency stability. Initial sources, binaries, results and controls remain preserved. Initial-directory moves, separate initial domains, permission/mount/case declarations, dynamic dependencies and framework runtime remain unfinished; physical iOS, suspended Intel HVF and full GitHub CI are separate acceptance boundaries.

`build-hvf-arm64/directory-swap-research/`, including `darwin-final-evidence/`, `darwin-evidence/`, `focused-v5.xml`, `file-owner-v5.xml`, `native-workload-fixed-evidence/`, `native-probe-v2-results.json`, `fixture-fixups-before/` and `hvf-controls.json`.

The final linked binaries repeated the complete Darwin gate successfully. Frozen dev 0a9a1d28d integration checks recorded 4,515 cases across 20 shared owners: 4,489 passed, six optional Z3 and 20 unavailable Windows EH corpus cases skipped, zero failures. Public C/CLI passed 170/170 and reports 32/32, included in that count. Python exercised all five profiles in 30.780s. All 12 native mobile architecture/fixup variants matched 4,532 original-program observations; single-session and whole Swift metadata parity passed 12/12. The Swift witness generator also reproduced its catalogue with the recorded SDK/compiler after a comment-indentation repair. This is local Release LLVM 23/Apple Clang 17 acceptance, not acceptance of later dev revisions or Linux Clang 18.


## Ordinary moves of declared initial directory subtrees

A strict Boolean `"movable": true` on an explicit non-root initial directory
authorizes ordinary rename of that root and declares its entire initial subtree
to contain ordinary non-mount directories with unique namespace identities.
C++ uses `DarwinFileOptions::MovableDirectories`, appended after existing aggregate
fields. The immediate parent must already be mutable. Omission or false keeps
moving that initial root unknown; other JSON types fail admission. Descendants
retain their own mutable, removable, movable and byte-write grants. The declaration
does not authorize an initial directory as either SWAP operand root, general
permission checks or mounts. Swapping created ancestors may carry previously
moved initial descendants; object state and dynamic charges follow the same
subtree transaction. Earlier initial-directory restrictions apply when the
required declaration is absent.

Known flags, special directory modes, regular-file link counts other than one,
and inode aliases from stat records or directory snapshots reject admission.
Each non-mount component joined by the declaration must have at most one known
device number, including observations on sibling subtrees and files under an
ancestor without stat metadata. Matching devices alone never join another initial
domain. Both immediate parents still need namespace authority at the actual
rename. SWAP capability remains separate: initial objects keep their direct
`swap_rename` declaration, mkdir copies its parent's capability into the created
object, and movement does not replace it. Both SWAP parents require support.

The live directory graph owns names and parent identities. Old input paths never
reconstruct moved or removed names. Unopened descendants, FD/dup/CWD references,
regular-file mappings and removed descendants follow their original objects.
Untouched descendants keep their original full stat, enumeration snapshots,
cookies and SEEK_END observations after an ancestor moves. The moved root and
changed parents invalidate full stat/enumeration. Reusing an old path supplies
no old metadata, snapshot or grant. Initial input is a complete declaration for
one synchronous execution; it is not a runtime catalogue-replacement API.

An empty initial replacement target needs its separate `removable` grant.
All current canonical paths must fit 1023 bytes, and the entire transaction
preflights the shared 16 MiB budget before publishing any names. Each movable
reference reserves its original path plus NUL, without another entry. Fixed
initial directory paths, references, snapshots and entries remain reserved after
removal. Initial directory names start with zero dynamic PathCharge; a move
charges the current path once for every linked or retained member. Only an
immediately releasable replacement's existing dynamic charge supplies credit.
FD/CWD/child/orphan-mapping retention supplies no credit, and final reclamation
refunds each dynamic charge once. Implicit initial ancestors add no new entries
to the existing 256-entry cap; initial regular-file entry reclamation is unchanged.

```json
{"darwin_files":{"files":[],"directories":[{"path":"/","mutable":true},{"path":"/work","mutable":true,"movable":true}]}}
```

The original `initial-directory-move` workload moves the native harness's existing
empty directory, retains subdirectories, shared/independent cursors, descriptor
flags, CWD and a private file mapping, replaces a created empty target and restores
the original names. It uses no SDK or libSystem calls in the guest. Native macOS
comparison is separate from physical iOS and other unavailable transports.

Validation on native macOS ARM64 after the final independent review records
1,264 Darwin registrations: 796 passed, 468 unavailable-backend skips, zero
failures. All 117 required ARM64 HVF workloads executed. The file-owner subset
passed 342/342, including 22 new 4K/16K behavior instances and three admission
checks. CreationPolicy inherits the moved parent object's original Device/GID;
name reuse, inode sequencing and current umask remain independent. At the exact
16 MiB limit, 16 forward/reverse created-ancestor exchanges preserve initial
descendants and release only the actual six-byte delta when moved back.
Public C/CLI passed 175/175, report/parser checks 33/33, and the original native
kernel workloads 29/29. The separate original probe recorded 19 successful
observations. These counts overlap.

Python integration passed all five guest configurations in 27.865 seconds; the
pure API suite passed 71 cases, SDK drift passed, and native inventory/reference
runners passed 49 unit checks. New sources, binaries, attempts and final results
are preserved under `build-hvf-arm64/initial-directory-move/` with hashes bound
to the committed revision. This establishes local guest-profile behavior, not
physical iOS or suspended Intel HVF acceptance. Initial operand SWAP, permissions,
undeclared mounts/case rules, shared-map EOF faults, Mach/thread/dyld dependencies
and framework runtime remain separate unfinished work.

## Atomic exchange of declared initial directory roots

A strict Boolean `"exchangeable": true` on an explicit non-root initial directory
authorizes that object as a `RENAME_SWAP` operand. C++ uses the appended
`DarwinFileOptions::ExchangeableDirectories` field. Its immediate initial parent
must be mutable. Omission/false keep that root unsupported; other JSON types fail.
It declares an ordinary non-mount initial subtree with unique namespace identities,
using the same flags, special-mode, alias, hard-link and connected-device admission
as movable. The union of these declarations supplies topology, not root authority.
Each movable and exchangeable reference reserves its own path plus NUL, even when
both name one root; neither adds an entry. Matching devices alone do not join domains.

Ordinary/EXCL initial sources still require movable. An ordinary initial replacement
target still requires removable. Exchangeable grants neither of those, nor mutable
descendant names, file writes, permission checks or general mount knowledge. Both
actual parents of a distinct exchange must be mutable and separately support
`swap_rename`. An authorized ordinary-component same-name SWAP checks parent
authority/device compatibility, then does nothing without a first dynamic charge
or a distinct-object filesystem capability. Same-object dot/case behavior remains
unknown. Missing target and source-dot ordering stay unchanged.

Two initial roots, initial/created roots, and initial directory/file pairs exchange
complete nonempty subtrees in either direction. Initial descendants may travel
without gaining direct root authority. Original FD/dup/CWD/cursors, parent object
relationships, leases and object grants remain attached. Untouched descendants
retain stat/snapshots; root namespace metadata follows the existing invalidation
and regular-file policy. Reused paths cannot reconstruct input objects. Both old
and new same-named removed descendants remain separate objects.

The existing transaction preflights all linked and retained members of both trees
before withdrawing every moving live name and inserting either tree. Both roots
stay linked. There is no replacement credit, file-byte refund or new descriptor,
inode or entry. Initial paths start with zero dynamic charge; first exchange
charges current paths once while all fixed input costs remain reserved. Subsequent
exchanges replace the two old dynamic charges rather than accumulate them. Path
or byte-budget failure leaves both trees and their observations unchanged.

```json
{"darwin_files":{"files":[],"directories":[{"path":"/","mutable":true,"swap_rename":true},{"path":"/left","exchangeable":true},{"path":"/right","exchangeable":true}]}}
```

The original SDK-free `initial-directory-swap` workload exchanges the native
harness's pre-existing empty directory with its regular file and then exchanges
them back. It checks subtree movement, private mappings, CWD/parent identity,
shared and independent cursors, descriptor flags, creation under the moved
original directory, retained removed children and restoration of original names.
The earlier ordinary-move acceptance at f98068c07 is a separate frozen result;
its initial-root SWAP limitation is extended only by this explicit declaration.


Validation on native macOS ARM64 (Release) reconciles 1,303 Darwin registrations:
823 passed, 480 unavailable-backend skips, zero failures; all 120 required ARM64
HVF workloads executed. File-owner checks pass 361/361, including 16 new 4K/16K
behavior instances and three admission checks. At exact capacity, no-op and 16
round-trip exchanges retain the original objects and charges. A first exchange
requiring six bytes with only five available rejects both orientations without
changing either path/cursor or the remaining creation budget.

Public C/CLI passes 180/180, report/parser 34/34, original native kernel workloads
30/30, and the separate original probe records 35 successful observations. Python
integration covers five guest configurations in 33.894 seconds; 71 pure API and
49 inventory/reference runner checks, SDK drift, pinned formatting, capability,
provenance and documentation checks pass. Counts overlap. Independent plan and
final source reviews found no remaining blocker. Sources, binaries, attempts and
actual results are frozen under `build-hvf-arm64/initial-directory-swap/` and bound
to the committed revision. An early build was interrupted to correct a known
invalid report field; an accidentally overlapping report run is excluded and was
repeated serially. Stale translation markers were corrected before final checks.
No deadline or semantic negative control was weakened.

This completes the declared initial-root exchange contract. Permissions,
undeclared mounts/case behavior, coherent shared maps/EOF faults, advancing
clocks, Mach/thread/dyld dependencies and framework runtime remain unfinished.
Physical iOS, suspended Intel HVF and remote merge CI are separate acceptance.

## Explicit read-only resource-limit observations

`getrlimit(194)` reads caller-supplied `DarwinSystemOptions::ResourceLimits`
(`darwin_system.resource_limits`) on all five Darwin guest configurations. Each
canonical resource 0..8 has a `DarwinResourceLimit` pair: `Current` at byte 0 and
`Maximum` at byte 8, both little-endian uint64, in one complete 16-byte output.
The resources are CPU, FSIZE, DATA, STACK, CORE, AS/RSS, MEMLOCK, NPROC and NOFILE.
Values require `0 <= current <= maximum <= 9223372036854775807`; zero is explicit
and INT64_MAX is Darwin's infinity. These fixed observations never query host
limits and do not change existing FD, VM, storage or execution budgets.
`setrlimit`, limit enforcement, related signals and scheduling remain unfinished.

Strict JSON accepts at most nine uniquely keyed objects with exactly `resource`,
`current` and `maximum`. Exact integers or unsigned decimal strings preserve the
full values; malformed, duplicate, noncanonical or reversed inputs fail before
image loading. Empty or omitted arrays declare no limits. Configuration keys
remain 0..8: syscall-only flag/truncation rules never normalize input keys.

```json
{"darwin_system":{"resource_limits":[{"resource":8,"current":256,"maximum":"9223372036854775807"}]}}
```

The syscall uses the low 32 selector bits and clears only `_RLIMIT_POSIX_FLAG`
(`0x1000`). An invalid selector returns EINVAL before lookup or memory access;
a valid absent observation stops unsupported before touching output. A wholly
unwritable buffer returns EFAULT; an individually partially writable pair stops
unsupported before publishing either word. A full unaligned or cross-page copy
preserves surrounding bytes. Memory-backend errors remain transport errors.
The original ARM64 native probe passed 23 selector/value/guard checks and four
separate fault checks; this host's untouched partial prefix is not generalized
into a portable partial-copy guarantee. [Apple getrlimit](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/getrlimit.2.html),
[XNU resource selector and copy order](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_resource.c),
[XNU resource constants](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/resource.h).

Native macOS ARM64 Release validation reconciles 1337 registrations:
845 passed, 492 unavailable-backend
skips and zero failures; all 123 required ARM64 HVF cases executed. The new
query has twelve 4K/16K behavior instances, typed and strict-JSON admission
controls, an SDK capture oracle and required SDK-free native/guest workloads.
Public C/CLI passes 190/190, report/parser 37/37 and original native workloads
31/31. Actual Python integration covers five configurations in 71.978 seconds;
71 pure API and 49 runner/reference checks, SDK drift, pinned formatting,
capability, provenance and documentation checks pass. Counts overlap. Evidence
is frozen under `build-hvf-arm64/resource-limit-observations/` and bound to the
committed source. A test-only profile enum typo was corrected after the preserved
first build failure. The initial owner filter missed typed admission checks;
they were run separately and included in the complete gate. Source-wiring
attempts remain recorded; deadlines and negative controls are unchanged.

This closes the explicit read-only query gap. Resource enforcement, permissions,
directory metadata/enumeration after mutation, coherent shared maps/EOF faults,
advancing clocks, Mach/thread/dyld dependencies and framework runtime remain
unfinished. Physical iOS, suspended Intel HVF and remote merge CI remain separate
acceptance requirements.

## Explicit read-only resource-usage observations

`getrusage(117)` reads independently optional
`DarwinSystemOptions::ResourceUsageSelf` and `ResourceUsageChildren` on all five
Darwin guest configurations. The strict JSON members are
`darwin_system.resource_usage.self` and `.children`. Each `DarwinResourceUsage`
requires signed int64 `user_seconds` / `system_seconds`, uint32
`user_microseconds` / `system_microseconds` below 1000000, and exactly fourteen
signed int64 `counters`. Exact integers or signed decimal strings preserve the
full range; malformed, missing or unknown fields, invalid microseconds and
counter lengths fail before image loading. Omitted peers stay unknown and do
not prevent querying the supplied peer; explicit zero snapshots are valid.

The complete little-endian output is 144 bytes. Timevals start at 0 and 16:
seconds occupy eight bytes, microseconds four, then four zero padding bytes.
Counters start at byte 32, eight bytes each; public indices 0..13 are
`ru_maxrss`, `ru_ixrss`, `ru_idrss`, `ru_isrss`, `ru_minflt`, `ru_majflt`, `ru_nswap`, `ru_inblock`, `ru_oublock`, `ru_msgsnd`, `ru_msgrcv`, `ru_nsignals`, `ru_nvcsw`, `ru_nivcsw`. Preserve Darwin's raw values and units, including
`ru_maxrss`; do not apply Linux KiB conversion. These observations remain fixed
across calls. They neither sample host performance nor implement runtime
accounting, fork/wait, scheduling or resource enforcement.

```json
{"darwin_system":{"resource_usage":{"self":{"user_seconds":"-9223372036854775808","user_microseconds":999999,"system_seconds":0,"system_microseconds":0,"counters":["9223372036854775807",0,0,0,0,0,0,0,0,0,0,0,0,0]}}}}
```

Only the low 32 selector bits matter: 0 is SELF, -1 is CHILDREN. Unlike
`getrlimit`, `0x1000` is invalid and no POSIX flag is removed. Invalid selectors
return EINVAL before lookup or memory access; missing selected observations stop
unsupported before output access. One complete unaligned or cross-page copy
changes exactly 144 bytes and preserves guards. Wholly unwritable output gives
EFAULT; individually partially writable output stops unsupported before any
byte. Backend errors remain transport errors. The SDK oracle uses one capture
per selector, including every field and padding, because sequential SELF samples
can change. The original native probe passed thirteen layout/selector/value
checks and four independent fault checks at the unchanged five-second bound.
On this host a partial SELF output published 64 bytes before EFAULT; this is
preserved as an observation, without generalizing a portable prefix rule.
[Apple getrusage](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/getrusage.2.html), [XNU](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_resource.c), [SDK layout](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/resource.h).

Native macOS ARM64 Release validation reconciles 1371 Darwin
registrations: 867 passed, 504
unavailable-backend skips, zero failures; all 126 required ARM64 HVF
cases executed. File-owner coverage remains 361/361. C/CLI 200/200, JSON 40/40, System/SDK 53/53, native 32/32 pass.
The new behavior has twelve 4K/16K instances plus typed/JSON admission and exact
single-capture SDK comparison. Actual Python integration passes all five guest
configurations in 42.865 seconds; 71 pure API and 49 inventory/reference
checks, SDK drift, pinned format, capability, provenance and documentation
checks pass. Counts overlap. Independent source review found no blocker.

Evidence is frozen under `build-hvf-arm64/resource-usage-observations/` and bound
to the committed source. The first guest fixture incorrectly required zero
secondary return on x64 EINVAL despite a nonzero RDX input; the corrected
assertion checks preserved RDX on x64 and cleared X1 on ARM64. The original
failure, source/binaries and zero-match filter attempt remain separate evidence.
The runtime return owner, deadlines and negative controls were unchanged.
The first complete gate recorded 866 passed, one existing iOS ARM64 HVF rename
five-second timeout and 504 unavailable skips. The same unchanged binary passed
that exact case in 386 ms, then the complete serial gate passed. Both runs remain
preserved; the timeout cause is unestablished and no latency guarantee is inferred.

This completes the declared query. Resource enforcement, permissions,
directory metadata/enumeration after mutation, coherent shared maps/EOF faults,
advancing clocks, Mach/thread/dyld and framework runtime remain unfinished.
Physical iOS, suspended Intel HVF and remote merge CI need separate acceptance.


## Explicit credentials, group access and coherent creation ownership

Optional C++ DarwinSystemOptions::Credentials holds RealUID, EffectiveUID,
RealGID, EffectiveGID and independently optional GroupAccessList and
GroupMembershipUID. Omission keeps
the historical four scalar getters at1000. An explicit record is complete;
zero/root is a valid declaration. Missing groups remain unknown and are never
inferred from host membership or EGID. The four scalar IDs and group entries support0..INT32_MAX;
the independent membership UID has its separate range below. Groups contain1..16 entries, preserve order and duplicates, and begin
with EffectiveGID. Strict darwin_system.credentials requires exactly real_uid,
effective_uid, real_gid, effective_gid and independently optional groups and
group_membership_uid. Lossless integer
decoding and the central validator reject malformed shapes, fields, ranges,
group counts or first-group mismatch before image loading; non-Darwin profiles
still reject the option.

getuid(24), geteuid(25), getgid(47), getegid(43) and getgroups(79) share one system
owner. The process passes the selected effective UID once to the file owner.
New regular files use that UID while inheriting device/GID from their direct
parent; rename, retained descriptors and old-name reuse preserve object identity.
Existing input stat records remain independent. Root does not grant file writes,
mutable directories, permissions or ACLs. Credential mutation, setuid/setgid/
setgroups and process/session creation remain absent. Credentials alone do not
authorize filesystem operations; the separate static owner-query environment
below is the sole supported permission-query exception.

getgroups interprets the low32 capacity bits as signed int. Negative capacity
returns EINVAL before observations or memory. Missing groups stop unsupported
before output. Known zero capacity returns count without accessing any pointer;
positive short capacity returns EINVAL before memory. Sufficient capacity copies
only4*count little-endian bytes once, then returns count. Capacity0x1000 is
positive and performs copying or pointer-fault handling; no POSIX flag is removed.
Unaligned/cross-page copies preserve full guards. Wholly unwritable output gives
EFAULT; individually partial output stops unsupported before any byte. Backend
errors remain transport errors. BSD error return preserves x64 RDX and clears
ARM64 X1; successful secondary results clear on both. Reports preserve full raw
arguments. Explicit EUID0 changes only the declared kern.osversion / kern.maxfilesperproc / kern.hostname write boundary
described above, after the existing input/length preflight.


~~~json
{"darwin_system":{"credentials":{"real_uid":101,"effective_uid":202,
 "real_gid":303,"effective_gid":404,"groups":[404,0,"2147483647",7,7]}}}
~~~


[Apple getgroups contract](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/getgroups.2.html), [pinned XNU credential/group ordering](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_prot.c).

### Credential verification, 2026-10-07

Release macOS ARM64 registrations/passed/unavailable-skipped/failed: 1413/897/516/0;
all 129 required ARM64 HVF cases executed. System/SDK72/72, file365/365,
report43/43 and public C/CLI210/210 passed without skips. Focused guests8 passed/
12 unavailable; original native workloads33/33; Python all five configurations
in 82.918s, Python API71 and runner/reference49 checks passed. SDK drift,
pinned changed-source/full-PR formatting, capabilities, provenance and all11
guides plus localization negative controls passed. Counts overlap. Independent
source review passed. Evidence is frozen under
`build-hvf-arm64/credential-observations/` and bound to the committed revision.

The independent raw/SDK probe passed21 value checks and5 separate fault
processes at unchanged 5 s. This host has16 groups, so positive short-capacity
precedence was exercised natively. Native partial copy wrote32 bytes before
EFAULT here; this observation is preserved separately and is not a portable
prefix guarantee or a modeled partial copy.

The first focused run retained8 failures (five instruction limits and three
5s deadlines) and12 unavailable skips: scanning two complete pages in the new
fixture exceeded unchanged bounds. The fixture now verifies all132 guard bytes
around the same cross-page output (64 preceding, up to64 data, at least4
following); direct owner tests still verify both complete pages. No service
parameters, budgets or fault controls changed. Sources, binaries and both runs
are preserved. Review also corrected the direct transport test's cross-page
geometry before execution. Public option selection now compares string contents
rather than literal pointer identities.

Permissions/ACLs, links, complete directory observations after mutation,
coherent shared maps/EOF, advancing clocks, Mach/thread/dyld and frameworks
remain unfinished. Physical iOS, suspended Intel HVF and remote merge CI remain
separate acceptance boundaries.

## Descriptor-table size from declared process and kernel limits

`DarwinSystemOptions::MaxFilesPerProcess` / `darwin_system.max_files_per_process` declares an optional nonnegative int observation. Missing is unknown; explicit zero is valid. Named `kern.maxfilesperproc` and numeric MIB `[1,29]` read the same four-byte value, independently of resource limits. Strict lossless integer parsing and central validation reject malformed, negative or oversized values before image loading; non-Darwin profiles reject this configuration.

BSD `getdtablesize(89)` requires this cap and `ResourceLimits[8].Current`, then returns their minimum. It clips the complete 64-bit Current before its int return: Current `0x100000001` with cap 64 returns 64, and infinite Current safely clips. Maximum, host values, live FD counts and `DescriptorLimit` are not substitutes. Missing either observation stops unsupported even when the known peer is zero. All six live arguments are ignored; the service performs no user-memory operation and uses the existing BSD carry/secondary-register convention. Mach timebase trap 89 remains separate.

The existing sysctl copy phases apply. Declared effective UID 0 cannot infer privileged writes to `kern.maxfilesperproc`: a true write stops unsupported after name/MIB and oldlenp preflight, before observations or output. Non-root returns EPERM; a new pointer with zero length stays a read. No limits or write authorization are inferred. The required resource workload compares both cap queries and low/high-number getdtablesize while keeping its original `l` / 144-byte output; a later unsupported query preserves bytes already emitted. A separate scalar fixture proves missing peers, zero, wide Current and independence from DescriptorLimit=3.

```json
{"darwin_system":{"max_files_per_process":64,"resource_limits":[{"resource":8,"current":"4294967297","maximum":"9223372036854775807"}]}}
```

[XNU getdtablesize](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_descrip.c), [XNU proc_limitgetcur_nofile](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_resource.c), [XNU MIB constants](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/sysctl.h).

Checks: Darwin (registered/passed/unavailable/failed) 1441/913/528/0; ARM64 HVF 132/132; System/SDK 80/80; File 365/365; C/CLI 211/211; ProcessReport 45/45; native 33/33; Python 5 (42.254s); API 71; runner 44 + reference 5. clang-format 22.1.2; capabilities/provenance; docs 286 / locales 10 / negative controls 3.

Attempt history: the first runner check failed in the ARM64 and x86-64 inventory subtests because the new scalar method lacked mandatory registration. Registration was added without relaxing the method-equality rule. The initial 129-required gate and failed runner records are retained; the fresh final ARM64 gate requires 132 cases.

Counts overlap. Evidence preserves the actual execution baseline plus exact source/binary/log hashes under `build-hvf-arm64/descriptor-table-observations/`; prior observation evidence stays immutable. Native probe: five disposable ARM64 macOS processes, 40 checks, five-second deadline each. Captured default Current=1048575/cap=245760 returned 245760; child Current=0/1/32/245777 returned 0/1/32/245760. Parent and system limits were unchanged. Physical iOS, suspended Intel HVF and remote merge CI remain separate. Permissions, mutable directory observations, coherent shared maps/EOF, advancing clocks, Mach/thread/dyld and framework runtime remain unfinished.

## Explicit process observations

`DarwinSystemOptions::ProcessGroupID`, `SessionID` and `ProcessTainted` are independent optional inputs: JSON `process_group_id`, `session_id` and `process_tainted`. IDs must be positive, at most INT32_MAX; taint accepts only JSON Boolean `true`/`false`. Missing stays unknown, while explicit `false` is a known zero. No value comes from the host, PID1000, credentials or another observation.

Raw `getpgrp(81)` reads the process group; `getpgid(151)` and `getsid(310)` use signed low32 `pid_t`, accepting zero or the fixed current PID1000. For example, `0xffffffff000003e8` still names self. A negative low32 PID returns ESRCH3 before observation lookup, as confirmed by read-only native probes and XNU process allocation/lookup. An unknown positive peer, including `0x1000`, stops with UnsupportedService without a guessed ESRCH or flag masking. A missing selected self value also stops unsupported. `getpgrp` and `issetugid(327)` ignore all arguments, and all four scalar queries access no guest memory. Existing BSD carry and secondary-register rules apply.

`process_tainted` supplies the fixed `P_SUGID` observation independently of equal or unequal real/effective IDs. It does not change EUID, file ownership, sysctl write authority, entitlements, leadership or terminal state. `setpgid`, `setsid` and credential mutation remain unsupported. The original read-only `process-observations` workload captures its own PID and checks high carriers, negative errors and return state; `virtual-process-observations` emits configured group/session/taint bytes. Model-only peer, missing-value and setter cases are excluded from native execution. Native macOS references do not establish physical iOS acceptance.

```json
{"darwin_system":{"process_group_id":7,"session_id":16909060,"process_tainted":false}}
```

[XNU process queries](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_prot.c), [XNU service numbers](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/syscalls.master), [XNU PID allocation](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_fork.c), [XNU PID lookup](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_proc.c).


## Explicit session login buffer

`DarwinSystemOptions::LoginNameBytes` is an independent optional observation of all 255 raw session bytes (`MAXLOGNAME`). JSON `login_name_hex` requires exactly 510 ASCII hexadecimal digits; both cases are accepted. Embedded NUL and nonzero bytes after a terminator remain valid. Missing is unknown; an explicit all-zero record is known. The model neither pads a short name nor obtains bytes from the host, credentials, process group, session ID or taint. This observation grants no login or filesystem authority.

Raw `getlogin(49)` takes unsigned low32 `u_int` length and copies exactly min(length,255) bytes, without string decoding, an added NUL or a required-size output. Zero length succeeds without an observation or guest-memory access even for invalid pointers; `0xffffffff00000000` therefore selects zero. A nonzero request requires the full declared buffer before destination checks. A wholly unwritable destination returns EFAULT14. Partial writable ranges stop unsupported before copying; preflight errors publish no bytes. Backend write errors propagate through the existing user-copy owner without a general rollback guarantee. BSD carry and secondary-register rules are retained, including original x64 RDX on errors.

`setlogin(50)` stays unsupported even with explicit root credentials or an all-zero buffer. The original read-only `login-buffer` workload checks full-carrier lengths, prefixes, adjacent bytes, zero-length pointers and EFAULT; `virtual-login-buffer` emits the declared 255 bytes. Model-only missing/setter cases never enter native execution. macOS ARM64 references do not establish physical iOS or Intel native acceptance. The example explicitly declares all 255 zero bytes; it is not an inferred empty login.

The guest verifier initializes all 265 output bytes before every copy and checks them in three ascending, disjoint ranges: [0,3), [3,3+n), [3+n,265), where n is the same clamped copy length. The first and last ranges check every guard byte; the middle compares every copied byte to the original snapshot. All 12 length carriers, 21 zero-length calls, 42 EFAULT calls, carry/secondary checks and raw/virtual routes remain covered under the existing execution limits. This reduces the authored verifier workload while preserving its byte coverage and first-error order; production runtime performance requires separate measurements.

```json
{"darwin_system":{"login_name_hex":"000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"}}
```

[XNU getlogin / MAXLOGNAME](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_prot.c#L1561).


## Explicit current-process priority

`DarwinSystemOptions::ProcessNice` / JSON `darwin_system.nice` declares an independent fixed signed nice value from -20 through 20. Missing is unknown; explicit zero and -1 are known. Lossless integer numbers and decimal integer strings use the existing parser; malformed types, fractions, exponent strings, whitespace and out-of-range values fail before image loading. Exact integer numeric JSON remains valid. Non-Darwin profiles reject the field. No host, credentials, group/session/taint/login, resource or CPU observation supplies it; it changes no scheduling, permissions or execution budget.

Raw `getpriority(100)` uses low32 for the int selector and unsigned `id_t` target. A target above INT32_MAX first returns EINVAL22. Unknown selectors, including GPU5 and 0x1000, return EINVAL; thread selector3 with nonzero low32 target also returns EINVAL before observations. `PRIO_PROCESS`0 supports only zero or current PID1000, requiring the declared nice value after selection. Positive other PIDs remain unsupported without guessed ESRCH. Group1, user2, thread3/target0 and extended selectors4,6,7,8 remain unsupported even with related observations; high-half-only thread targets select unknown thread state, not EINVAL. The result is signed-extended to the full 64-bit carrier: -1 returns UINT64_MAX successfully with carry clear. The query accesses no guest memory and ignores unused arguments; existing BSD secondary-register rules retain x64 RDX on errors and clear it on success, while ARM64 X1 is cleared on both paths.

`setpriority(96)` remains unsupported even with explicit root credentials and nice. Original read-only `process-priority` checks self carriers, guaranteed-invalid arguments and success/error/success return transitions; `virtual-process-priority` emits the configured eight signed bytes. Peer, aggregate, missing and setter routes are model-only. The fresh ARM64 macOS probe passed 191 checks with nice0; that nonnegative sample does not establish negative hardware extension, which uses pinned signed entry declarations and independent model boundaries. Physical iOS and Intel native acceptance remain separate.

```json
{"darwin_system":{"nice":-1}}
```

[XNU getpriority](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_resource.c), [XNU signed INT entry](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/dev/arm/systemcalls.c).


## No-follow open flags and directory preflight

Ordinary and nocancel `open` / `openat` accept unsigned low32 O_NOFOLLOW=0x100 or O_NOFOLLOW_ANY=0x20000000 in the closed file catalogue. These lookup flags never appear in F_GETFL and do not change access, append, truncation, creation or descriptor-local CLOEXEC behavior. Combining both returns EINVAL22 after FD capacity admission, before importing the full pathname; a full descriptor table returns EMFILE24 first. Unknown flags remain unsupported.

For non-AT_FDCWD `openat`, importing exactly the first pathname byte precedes access-mask and FD-capacity checks. An inaccessible first byte returns EFAULT14. A relative prefix, including NUL, first checks the held directory object: unknown FD gives EBADF9, regular file gives ENOTDIR20, and an unknown stream vnode kind remains unsupported. A slash skips dirfd validation. Only afterward does the existing open sequence import the full pathname. Ordinary `open` and AT_FDCWD skip this prefix phase. Other nameiat routes use the same first-byte/relative-FD preflight before full import; AT_FDONLY bypasses it. Rejected opens reserve no FD or new inode; transport errors propagate without namespace mutation.

The original `file-access` workload uses relative directory FDs for NOFOLLOW_ANY to avoid native `/var` or `/tmp` aliases. Direct tests distinguish first/later byte faults, slash/NUL, user/page boundaries, exhausted descriptors and retained removed directories. The separate ARM64 macOS raw probe passed 30 cases with its unchanged five-second deadline; its final exhausted absolute-path row uses a valid directory FD despite its historical label. Physical iOS and Intel native acceptance remain separate.

[XNU open1at / open1](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c), [XNU vn_open_auth](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_vnops.c), [XNU open flags / FMASK](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/sys/fcntl.h).

## Fixed initial symbolic links

`DarwinFileOptions::SymbolicLinks` and JSON `darwin_files.symbolic_links` declare fixed links. Each entry requires canonical absolute `path` and raw hexadecimal `target_hex`; optional `metadata` observes the link itself. Targets contain 1..1023 non-NUL bytes, preserve non-UTF8 bytes, repeated slashes and dots, and need not exist. `files` remains required, including an empty array. Link names cannot collide with files/directories or be another declared name’s ancestor. Targets and path/NUL charges share the existing 256-entry/16 MiB limits. Link metadata requires S_IFLNK and size equal to the raw target length; complete directory snapshots include DT_LNK=10 and coherent link inodes. Configured CWD must name an actual directory.

The file owner expands links before reducing dots. Relative targets start at the actual containing directory; absolute targets restart at the guest root. Every expansion reparses terminal slashes: consumed input slashes do not become target slashes. Up to 32 expansions are allowed; the next returns ELOOP62. Expanded target plus remaining suffix plus NUL must fit 1024 bytes or return ENAMETOOLONG63. Open descriptions, CWD, F_GETPATH and mmap retain the resolved file/directory object.

`stat64`, ordinary open, access, truncate and chdir follow final links. `lstat64` and readlink retain the final link. O_NOFOLLOW retains it and returns ELOOP; O_DIRECTORY with that retained link returns ENOTDIR20 first. O_NOFOLLOW_ANY refuses required expansion. O_CREAT|O_EXCL retains an existing terminal link and returns EEXIST17, including dangling/cyclic links. AT_SYMLINK_NOFOLLOW (0x20) retains final links; AT_SYMLINK_NOFOLLOW_ANY (0x800) also retains them and rejects any required intermediate/trailing expansion. Both AT flags may be combined. Fstatat AT_FDONLY still ignores the pathname after flag validation.

Raw `readlink(58)` takes a signed low32 count; `readlinkat(473)` retains the full size_t count. Both return int and reject counts above INT32_MAX before path/FD access. They copy only min(count,target length) raw bytes, add no NUL and preflight only that prefix. Zero length still resolves the path and checks link type, then ignores the output pointer. Non-links return EINVAL22. EFAULT14 means no writable prefix; a partially writable prefix stops unsupported before any bytes are copied. Transport and memory-budget errors propagate. The shared nameiat prefix order is confirmed separately by 26 additional ARM64 macOS controls, including critical repeated-slash expansion lengths.

Initial raw targets remain immutable; initial names are protected unless the explicit mutable grant below is supplied. A MutableDirectories entry cannot be the root or a segment ancestor of any ungranted initial link name; /work does not contain /workspace/link. Separate mutable domains may contain link targets, including names created, moved, deleted or replaced during execution. Existing parent, mount, alias, flag, swap-support and creation-policy validation still applies; fresh creation inodes must exceed all metadata/snapshot inodes, including protected links. Fixed-name WritableFiles/MutationPolicies may still mutate the resolved regular file. Unlink/rename of ungranted retained links stop explicitly before effects. Runtime link creation is documented below; hard links, ACL authorization and ungranted initial-link mutations remain unsupported. The independent ARM64 macOS probe passed 189 observations and 115 full-buffer checks with its original five-second deadline; that reference alone does not establish physical iOS, Intel HVF or full OS compatibility.

The additional 60-case ARM64 macOS DELETE/RENAME matrix captures complete stat buffers, before/after namespace state and retained FD/CWD identities under the original five-second deadline. Terminal slashes can expand a fixed link and mutate its actual target; required expansion with NOFOLLOW_ANY returns ELOOP. The SDK-free symbolic-link-mutations workload also checks creation, dangling targets, rename/removal/replacement, retained CWD parents and all ten original file bytes before descriptor closure, with all ten mapped bytes still checked after closure. These host observations do not establish physical iOS or native Intel coverage.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"3031"}],"symbolic_links":[{"path":"/link","target_hex":"64617461"}],"working_directory":"/"}}
```

[XNU namei](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_lookup.c), [XNU readlink / AT](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c), [XNU open authorization](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_subr.c).

## Runtime symbolic-link creation

Raw `symlink(57)` and `symlinkat(474)` create process-local links in admitted mutable directories. Both return int; symlinkat uses the low32 directory FD, and an absolute destination ignores that FD. Target import precedes all destination checks: the first NUL terminates an opaque 0..1023-byte target, including empty, non-UTF8, dot and repeated-slash bytes. An unterminated 1024-byte target returns ENAMETOOLONG63; an earlier inaccessible byte returns EFAULT14. Initial JSON link targets still require 1..1023 bytes.

The current link table owns the actual name, parent and raw bytes. Existing terminal entries return EEXIST17. Consumed terminal slashes can expand a dangling link and create at its target name; the original link remains unchanged. Expanding an empty target returns ENOENT2. Readlink of an empty target returns zero without accessing the output pointer, even with positive capacity; count/path/kind checks still run first.

Created names/NUL and target bytes share the 256-entry/16 MiB budget, charged once alongside initial links, files and directories. Refusals publish no node or parent change and consume no FD or regular-file creation inode. Without `namespace_policy`, complete new-link metadata remains explicitly unknown; the regular-file CreationPolicy cannot supply it, and reused names cannot inherit old observations. Parent stat/snapshots become unknown after successful creation. Resolved target FDs, CWD and mappings retain their original objects after target removal/replacement.

Rmdir and directory replacement detect created-link descendants. Directory movement or SWAP containing a protected initial link stops before effects. Ordinary directory/link replacement retains its native type errors; hard links, permissions/ACLs and ungranted initial-link mutations remain unsupported. Creation through an alias requires authority at the actual parent.

Native ARM64 macOS references retain the original 150-case epoch, including four observer failures that examined an existing dangling link instead of the new target. A separate 10-case supplement observes the actual target and empty-link copy/follow boundaries without rewriting those failures. The SDK-free `symbolic-link-creation` workload independently checks both entry points, raw targets/canaries, retained parents, replacement files and all ten old FD/mapping bytes. Native references alone do not establish physical iOS, native Intel or complete OS compatibility.

[XNU symlink / symlinkat](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c), [XNU empty-link expansion](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_lookup.c).

## Removing runtime-created symbolic links

`unlink(10)` and `unlinkat(472)` remove a runtime-created link when its actual parent admits mutation; protected initial links remain immutable. Bare `AT_SYMLINK_NOFOLLOW_ANY` retains the final link and permits its removal, including dangling, cyclic and empty-target links. Required intermediate or trailing-slash expansion with that flag returns ELOOP62. Without it, the existing resolver chooses the actual entry: for `a → b → target`, `unlink("a/")` removes `b`, retaining `a` and the final target. Existing flag, pathname and dirfd error ordering still applies.

Successful removal refunds exactly one dynamic entry and its current path/NUL/raw-target charge, and invalidates only the actual parent's complete stat/enumeration observations. No free FD or creation inode is needed. Target bytes, mutation policy, open descriptions, shared cursors, CWD and mapping leases keep their existing objects; name reuse cannot restore the removed link. Rejected operations preserve model state and refund nothing. Complete runtime-created-link metadata remains unknown. Ordinary directory/link replacement retains its native type errors; moving/SWAP of subtrees that contain protected initial links remains unsupported.

The independent 40-case ARM64 macOS reference records 28 successful deletions and 12 native errors under the unchanged five-second deadline, including 17 second-removal ENOENT results and retained FD/CWD/private-mapping checks. These observations do not establish guest, physical iOS or native Intel acceptance. The SDK-free `symbolic-link-unlink` workload and public API cases exercise the same removal boundaries separately.

## Renaming runtime-created symbolic links

`rename(128)`, `renameat(465)` and `renameatx_np(488)` support ordinary and `RENAME_EXCL=4` leaf moves involving a runtime-created link: link to missing name, link/link, link/file and file/link replacement. Both actual parents must admit mutation in one established mount domain; protected initial links remain immutable. The shared resolver selects the actual entries. Raw target bytes are unchanged, including empty, dangling, cyclic and non-UTF8 targets; moving a relative link makes it resolve from its new parent. Bare `RENAME_NOFOLLOW_ANY=16` retains terminal links, while required intermediate expansion returns ELOOP62. Distinct existing EXCL destinations return EEXIST17; same-object EXCL remains unsupported without a filesystem case-sensitivity contract.

The transaction reserves the new path/NUL charge, bounded by 1024 bytes including NUL, before publishing either name. Replacing a runtime link refunds its entire current path/NUL/target charge once, independently of referent FDs or mappings. Replaced regular-file bytes/path charges remain leased until all descriptions and mapping leases release; only an immediately reclaimable regular target supplies reservation credit. A move requires no extra entry, FD or file-creation inode. Refusals retain both entries; success invalidates the actual parents' complete stat/enumeration observations. Without `namespace_policy`, complete runtime-link metadata remains unknown. link/directory replacement, hard links and moving/SWAP of subtrees containing protected initial links remain unsupported.

The independent 19-case ARM64 macOS reference records 14 successful moves/replacements and five EEXIST/ELOOP errors with unchanged five-second deadlines, link inode/raw-target checks, relative rebinding and retained FD/dup/cursor/CWD/private-mapping controls. The SDK-free `symbolic-link-rename` workload and public SDK/CLI cases check these paths separately. Native references alone do not establish physical iOS, native Intel or full OS compatibility.

[XNU rename / namei](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c).

## Exchanging runtime-created symbolic links

`renameatx_np(488)` also supports `RENAME_SWAP=2` leaf exchange for runtime link/link, link/file and file/link pairs. Both actual parents must grant mutation and SWAP support in one established mount domain. Exact same-name link SWAP succeeds without effects or a SWAP capability declaration. Missing targets return ENOENT2; `SWAP|NOFOLLOW_ANY=18` retains terminal links but required intermediate expansion returns ELOOP62; `SWAP|EXCL=6` returns EINVAL22 before path input. Opaque target bytes remain unchanged and relative links bind from both new parents.

The existing transaction reserves both path/NUL charges before publishing either name. Both objects stay linked: their bytes and mapping leases provide no replacement credit. An initial regular file acquires a dynamic path charge on its first exchange; swapping back reuses that charge. No entry, FD or creation inode is consumed. Both parents' full stat/enumeration observations become unknown after a real exchange, while file identity, nlink, descriptions, shared cursors, CWD and mappings retain their objects. Complete runtime-link metadata requires namespace_policy; subtree moves/SWAP containing protected initial links remain unsupported.

The separate 22-case ARM64 macOS probe records 14 exchanges, two same-object successes and six ENOENT/ELOOP/EINVAL errors with the original five-second deadlines. The extended SDK-free `symbolic-link-rename` workload checks both mixed orders, opaque targets, relative rebinding and retained mappings; C++/public SDK/CLI/Python cases require actual SWAP flags and error events. These references do not establish physical iOS, native Intel or full OS compatibility.

[XNU rename / namei](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c).

## Moving trees containing runtime-created symbolic links

Ordinary/EXCL directory moves and SWAP now include runtime-created link descendants in the existing directory transaction, including directory/file exchange. Every directory, file and link destination is preflighted against the shared namespace and the terminator-inclusive 1024-byte path limit. Link target bytes stay charged and unchanged; only current path/NUL charges are replaced. SWAP supplies no replacement credit. All three tables are extracted before any new key is published, preserving colliding file/link child suffixes. No additional entry, FD or creation inode is consumed.

Links retain their actual parent objects as those directories move. Relative targets resolve from the new paths, while existing descriptions, duplicate cursors, CWD, orphan nodes and mapping leases retain their objects. Protected initial links and their containing trees remain immutable; full runtime-link metadata requires namespace_policy, while hard links and ACL enforcement remain unsupported. Separate native ARM64 macOS controls record 25 cases: five moves, ten exchanges, two same-object successes and eight unchanged-namespace refusals, with the original five-second deadline. Cross-parent controls verify unchanged raw targets and changed relative bindings. The extended `symbolic-link-rename` workload exercises relocation through C++, SDK, CLI and Python; these observations do not establish physical iOS, native Intel or full OS compatibility.

[XNU rename / namei](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c).

## Metadata for process-created links and directories

Optional `creation_policy.namespace_policy` (C++ `DarwinFileCreationPolicy::Namespace`) extends the five required creation-policy fields with one strict object: `symbolic_link_allocation_unit`, `directory_entry_size` and `directory_blocks`. The link unit is a power of two from 512 through 16 MiB, entry size is positive and at most 16 MiB, and blocks is uint64 at most INT64_MAX. Decimal strings preserve large integers. Parent metadata, initial umask and fresh-inode requirements remain unchanged. Without the extension, earlier unknown link/directory metadata and regular-file-only inode behavior remain the defaults.

Enabled regular-file, symlink and mkdir insertions share one inode sequence, including permanent UINT64_MAX exhaustion; refusals and existing-name opens consume none. Device/GID belong to the actual parent and UID to the effective guest identity. Link mode is S_IFLNK plus `0777 & ~umask`, nlink is 1, size is exact raw-target bytes (including empty/non-UTF8), and blocks rounds those bytes to the declared allocation unit in 512-byte blocks. Directory mode is S_IFDIR plus `mode & 0777 & ~umask`; nlink is two plus all immediate linked names, size is nlink times the declared entry size, and blocks stays declared, including for held empty removed directories. These size/allocation/count rules are an explicit virtual contract, not an inferred APFS policy.

Initial timestamps use creation_time. Successful child namespace changes update created-parent mtime/ctime; direct link/directory moves update only their ctime using mutation_time. Ancestor moves preserve descendant metadata. Full records belong to objects across dup, CWD, replacement, SWAP, removal and name reuse. The creation extension alone does not preserve initial-parent full stat or fixed snapshots; the separate directory policies below provide stat and live enumeration authority. ACLs and ungranted initial-link mutations remain unsupported. Native `created-namespace-metadata` checks common mode/owner/identity/lifetime observations; `virtual-created-namespace-metadata` checks literal complete 144-byte directory and link records through C++/SDK/CLI/Python. The focused epoch passed 11 model/admission checks, one strict-JSON check, 43 native workloads under the unchanged five-second bound, eight guest-profile/backend instances (12 unavailable-backend skips, all three required HVF instances executed) and ten public cases. The fixture pointer-table failure and corrected static ARM64 envelope are retained. Native Intel and physical iOS remain unvalidated.

```json
{"namespace_policy":{"symbolic_link_allocation_unit":512,"directory_entry_size":32,"directory_blocks":7}}
```

## Virtual enumeration after namespace changes

An optional `directories[].enumeration_policy` enables live `getdirentries64` views. C++ uses `DarwinFileOptions::DirectoryEnumerationPolicies` and `DarwinDirectoryEnumerationPolicy`. The strict object requires exactly `minimum_buffer_size`, `initial_minimum_buffer_size` and `seek_offset`: the first is positive, both minima are at most 128 MiB, and seek_offset is lossless uint64. Initial directories require explicit own nonzero-inode metadata; a directory cannot also declare immutable `contents`. Each policy reference charges its path plus NUL once. mkdir inherits the actual parent's policy; existing descendants retain their own declarations. This declaration supplies enumeration authority independently of mutation grants.

The explicit virtual order is `.`, `..`, then linked immediate names sorted by unsigned bytes. Record inodes come from observed or process-created objects; `..` follows the actual retained parent. Missing or zero child/parent identity stops before output. Inode identity can remain known when a complete initial stat record becomes unknown; no other stale stat field is reused. Cookies are local ordinals from one, and d_seekoff is the declared constant. Dup shares the bound cursor; opens are independent. Committed membership changes and direct directory moves invalidate nonzero cursors until zero rewind; refusals and same-object no-ops retain them. Ancestor moves retain descendant cursors. Version exhaustion stops explicitly instead of wrapping. Held removed empty directories emit zero records, including after name reuse.

The existing record encoder, whole-record packing, buffer minima, payload cap, EOF suffix and ordered data/cursor/position/flags effects remain authoritative. Initial-parent full stat invalidates without the separate stat policy below; immutable snapshots still invalidate. Omitting the new policy retains the earlier unsupported behavior. These virtual order/cookie rules do not reproduce APFS generations. The independent ARM64 preparation records 25 events and 16 views within the unchanged five-second deadline. `directory-enumeration-mutations` checks common native names/types/inodes and retained objects; `virtual-directory-enumeration` checks a literal 160-byte view through guest, C/CLI and Python routes. Native Intel, physical iOS, ACL enforcement, hard links and full OS/framework execution remain unvalidated or unsupported.

[XNU directory ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/dirent.h), [XNU VFS copy ordering](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

## Explicit stat mutation for initial directories

An optional `directories[].mutation_policy` keeps an admitted initial directory’s full stat record after namespace changes. C++ uses `DarwinDirectoryMutationPolicy` and `DarwinFileOptions::DirectoryMutationPolicies`. The strict object has exactly `directory_entry_size` and `mutation_time`. Entry size is positive and at most 16 MiB; time uses lossless signed 64-bit seconds and nanoseconds in [0, 1000000000). The directory must have its own complete metadata with a nonzero inode. Each policy reference charges its path plus NUL once; the projected size does not allocate file bytes. The policy grants no namespace or permission authority and requires neither a creation policy nor an enumeration policy.

The complete observed record remains unchanged until the first genuine committed change, which copies its scalar fields into the directory object without allocation. Child-name changes set nlink to two plus all linked immediate names of every kind, size to nlink times directory_entry_size, and mtime/ctime to mutation_time. Direct moves, SWAP or removal change only ctime; moving an ancestor preserves descendant records. Refusals and same-object no-ops change nothing. Device, inode, mode, owner, blocks, block size, flags, generation, atime and birthtime retain their observed values. These are declared virtual rules, not APFS allocation, link-count or clock inference.

The record and policy follow the original object through dup, held descriptors, CWD, replacement, removal and name reuse. A new mkdir object uses the separate creation policy, if supplied, and never inherits an initial-directory stat policy from its parent or an old object at the same name. Immutable directory snapshots still become unknown after changes; an independently declared enumeration_policy can provide live views. Without this stat policy, changed initial-directory full metadata remains unknown.

An independent native ARM64 preparation preserved 27 guarded raw-stat views and 15 operations under the unchanged five-second deadline, checking the 144-byte SDK ABI and retained identity without generalizing native timestamps or allocation. The original `initial-directory-metadata` workload checks common native observations; `virtual-initial-directory-metadata` compares a literal complete 144-byte record across guest, C/CLI and Python routes. Models also cover first removal, missing mutation grants, creation-policy omission and snapshot/enumeration independence. Native Intel, physical iOS, ACL enforcement, hard links, ungranted initial-link mutations and full OS/framework execution remain unvalidated or unsupported.

```json
{"mutation_policy":{"directory_entry_size":17,"mutation_time":{"seconds":-11,"nanoseconds":321}}}
```

## Explicit mutation of initial symbolic links

Initial `symbolic_links[].mutable:true` (C++ `DarwinFileOptions::MutableSymbolicLinks`) grants namespace mutation of that original object, with a separately mutable actual parent. Known flags, special mode bits, link_count other than one, identity aliases and conflicting parent devices reject admission. Omission keeps the name protected and excludes ungranted links from mutable ancestor domains. Each grant reserves a fixed path/NUL reference; it creates no entry and consumes no creation inode. Initial raw targets remain immutable.

The existing unlink, ordinary/EXCL rename, leaf link/file/link SWAP and admitted directory-subtree transactions apply without relaxing their actual-parent, mount and SWAP requirements. Raw targets remain unchanged and relative lookup rebinds at the actual new parent. Referent FD/dup offsets, CWD and mapping leases retain their own objects. Initial target/name/reference costs stay reserved after replacement or removal; first rekey reserves a separate dynamic name. Replacement and unlink refund only dynamically owned name/created-target costs. Only newly created links count as dynamic entries. Refusals and same-object no-ops publish nothing.

Optional `symbolic_links[].mutation_policy` uses `DarwinSymbolicLinkMutationPolicy` and `DarwinFileOptions::SymbolicLinkMutationPolicies`. Its strict one-field object requires `mutation_time`, with lossless signed 64-bit seconds and nanoseconds in [0, 1000000000). It requires the explicit grant and complete observed nonzero-inode metadata. The first direct move/SWAP copies scalar observations without allocation and changes only ctime to that fixed time. Every other field, including blocks, remains observed; ancestor moves, refusals and no-ops preserve the full record. Without the policy, a direct move makes complete stat unknown, while inode identity remains available to live enumeration and known device contradictions still refuse. Each policy reserves one fixed path/NUL reference. Reused names and new symlink objects never inherit an initial record or policy; creation uses its separate namespace policy.

The original private ARM64 preparation retains 14 guarded raw-stat views, 11 operations and the independent 144-byte SDK ABI under compile120s/native5s/drain1s/reap1s bounds, with recorded private cleanup. `mutable-initial-links` checks common native identity, target rebinding and held referents; `virtual-mutable-initial-links` checks a literal complete stat through five guest profiles, C/CLI and Python. Model checks cover both page sizes, subtree SWAP, exact fixed/dynamic costs, entry/inode exhaustion and metadata/enumeration independence. Native Intel and physical iOS remain unvalidated; hard links, ACL enforcement and full OS/runtime/framework execution remain separate gaps.

```json
{"mutable":true,"mutation_policy":{"mutation_time":{"seconds":-13,"nanoseconds":456}}}
```

## Directory and symbolic-link root transactions

`renameatx_np(RENAME_SWAP)` admits an actual directory and a symbolic link in either order, including nonempty directory trees. Existing authority remains required: initial directories need `exchangeable`, initial links need `mutable`, and both actual parents need mutation/SWAP grants in one established mount. Protected initial descendants still refuse. Under the existing ordinary-move grants, directory-to-link returns ENOTDIR20, link-to-directory EISDIR21, and EXCL against either existing distinct name EEXIST17. Both directory/descendant-link cycle directions return EINVAL22 before effects. The operation exchanges the link itself, so a successful exchange can create a self-referent link whose later follow returns ELOOP62.

The existing three-map transaction preflights both roots, all linked or held descendants, canonical paths and dynamic storage before publication. Initial fixed names/targets/references, file bytes and mapping leases supply no SWAP credit. Each first initial-name rekey owns a separate dynamic path/NUL charge; repeated exchanges replace that charge once. Both roots stay linked without consuming an entry, descriptor or creation inode. Membership follows actual object parents; a removed object with a reused spelling cannot join an unrelated new tree.

Raw targets stay unchanged and relative lookup rebinds at the new actual parent. Held FD/dup cursors, CWD, file/directory referents and orphan mapping leases survive. Direct directory/link roots apply their own existing stat policies to ctime; ancestor moves preserve descendant records. Omitted policies retain unknown full stat, while known inodes remain available to live enumeration. Committed parent changes/direct directory moves invalidate the relevant enumeration version; rewind uses the existing contract. No new JSON fields or API authority are added.

The original private ARM64 preparation records 36 guarded 144-byte stat views and 16 raw rename operations under compile120s/native5s/drain1s/reap1s, with actual reap and private cleanup. Original `directory-link-roots` and `virtual-directory-link-roots` cover common native identity and literal complete stat through five guest profiles, C/CLI and Python. Both-page model checks cover exact budgets, unopened overflow, retained orphans, missing grants and descriptor/entry/inode exhaustion. This extends the preceding directory/link exclusions only within these grants. Native Intel, physical iOS, hard links, ACLs and complete OS/runtime/framework execution remain separate coverage or implementation gaps.

## Fixed kernel pathconf queries

Raw `pathconf(191)` and `fpathconf(192)` implement the fixed XNU vnode queries: selectors 15/16/17 return 1,19/25 return 0,20/22/23 return 4096,21 returns 65536 and 24 returns 255. These are _PC_2_SYMLINKS, _PC_ALLOC_SIZE_MIN, _PC_ASYNC_IO, _PC_PRIO_IO, _PC_SYNC_IO, transfer recommendations and _PC_SYMLINK_MAX. They are query results; asynchronous execution and filesystem authorization remain separate unsupported services. The values do not follow guest page size, allocation policy or catalogue limits.

Pathname import and full follow lookup precede selector handling, retaining CWD, terminal/intermediate links and native EFAULT/ENOENT/ENOTDIR/ELOOP ordering. fpathconf first looks up signed-low32 FD ownership; unknown/closed FDs return EBADF before selector handling. Known regular-file/directory descriptions retain these queries across dup, rename, unlink and name reuse without stat observations. Input/capture descriptions have unknown native kind and stop explicitly. Low32 int selectors and the existing BSD int/carry/secondary-register return contract apply. Queries do not copy output, advance cursors, change metadata/enumeration or reserve an entry/FD/inode. Filesystem-dependent selectors, including NAME_MAX, case sensitivity and unknown values, remain unsupported after lookup; neither the host filesystem nor a guessed EINVAL supplies them.

Original kernel-pathconf checks both calls and carrier/error transitions; kernel-pathconf-values emits an independent 80-byte literal across guest/C/CLI/Python, while kernel-pathconf-unsupported preserves prior output and reports no scalar/error result. The private native ARM64 preparation passed 250 queries (249 SDK cross-checks) in 0.262 s under the unchanged 5 s deadline, including held removed objects and cursor preservation. Native Intel/physical iOS, ACLs, hard links and complete runtime/framework execution remain unvalidated or unsupported.

[XNU vn_pathconf](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_vnops.c), [XNU pathconf](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU fpathconf](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/kern/kern_descrip.c).

`pathconf`: `file/` → ENOTDIR; `alias/`, `alias//` → 1 (selector15); `alias/.`, `alias/child` → ENOTDIR.

## Fixed common attribute lists

getattrlist(220), fgetattrlist(228) and getattrlistat(476) query eleven fixed common fields in the explicit catalogue: device, object type, four timestamps, owner/group, full mode, flags and file ID. They share the complete-stat validity decision with stat64; a missing or invalidated observation stays unknown. Object type and an empty selection need no stat record. Root/mount NAME, volume, directory/file/fork-specific masks, ACLs and unknown options stay explicitly unsupported, even when a returned mask was requested. No host metadata or mount label is inferred.

The 24-byte request is imported before path/at lookup; fgetattrlist validates the low32 FD and native kind first. The reserved word is ignored. Shared CWD, relative-FD and link resolution preserve native errors before the size/bitmap checks. Known fixed records use little-endian, four-byte alignment, full st_mode and signed seconds. A returned-mask record is 120 bytes; the corresponding plain record is 100. Short user buffers receive only the requested prefix but still report the full required size. Partially accessible copies stop before that copy; unrepresentable signed-uio sizes give EINVAL only after a valid supported request. Queries preserve cursors, metadata, enumeration and entry/FD/inode budgets. Held objects keep the existing dup/removal/name-reuse lifetime.

The three workload modes check native common behavior, independent configured bytes and an unsupported ATTR_CMN_EXTENDED_SECURITY query preserving previous output through guest, C, CLI and Python routes. Private ARM64 preparation passed 187 raw queries and 176 typed SDK path/FD cross-checks within the unchanged 5-second native bound. SDK15.5 has no getattrlistat declaration; raw476 controls are separate. New guest/Python workloads keep 5,000,000us/quantum1024 and existing public tests keep 10s. Native Intel, physical iOS, filesystem-specific facts, hard links, permissions/ACL enforcement and full runtimes/frameworks remain unverified or incomplete.

```text
ATTR_CMN_RETURNED_ATTRS=0x80000000
FSOPT_NOFOLLOW=1, FSOPT_REPORT_FULLSIZE=4
FSOPT_PACK_INVAL_ATTRS=8 (requires ATTR_CMN_RETURNED_ATTRS)
FSOPT_NOFOLLOW_ANY=0x800
common-attributes
common-attributes-values
common-attributes-unsupported
```

[XNU attrlist](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_attrlist.c), [XNU attr.h](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/sys/attr.h).


## Explicit extended-attribute reads

Raw getxattr(234), fgetxattr(235), listxattr(240) and flistxattr(241) read complete ordered ordinary observations from `darwin_files`. Each file/directory/link can declare `extended_attributes` as an array of strict `{name,bytes_hex}` records. Omission stays unknown; [] declares a known empty list. Names are UTF-8, including slash, with 1..127 bytes; values are opaque, duplicates are rejected and order is preserved. The common admission owner bounds 4096 attributes and charges names plus NUL and values in the existing 16 MiB budget, without charging an existing object path twice.

Observations follow objects through dup, namespace moves/removal, CWD and name reuse independently of full stat or directory-enumeration validity. New objects start unknown. Content writes, successful truncate and ambiguous nonempty copyin failures invalidate attributes; a partial-buffer preflight refusal preserves them. Queries preserve offsets, input bytes, metadata and entry/FD/inode budgets.

ABI: low32 FD/options/position, full64 size, BSD user_ssize_t/carry/secondary. NULL queries ignore position; path nonNULL size0 gives ERANGE for a nonempty value, FD size0 queries its length. Only path get sizes UINT32_MAX/UINT64_MAX are legacy queries; FD get clamps to INT32_MAX. Positive short lists can publish complete-name prefixes before ERANGE; negative full64 nonNULL list lengths give ERANGE for nonempty lists. No verified native empty-list control was obtained: negative lengths on declared-empty lists remain UnsupportedService. Required inaccessible output stops before copying. NOFOLLOW1 and NOFOLLOW_ANY64 remain independent;8/16 reject before lookup and FD1/64 before FD/name. CREATE2/REPLACE4 are ignored for these reads; SHOWCOMPRESSION32 and unknown bits remain unsupported. Protected com.apple.system.*, ResourceFork, FinderInfo, decmpfs, setters/removers, permission/ACL enforcement and filesystem inference remain outside this read contract.

extended-attributes / extended-attributes-values / extended-attributes-unsupported: original shared workload, literal virtual bytes, unknown-observation stop preserving prior output. Guest/Python5,000,000us/quantum1024; public10s; native5s unchanged. Private ARM64 preparation:320 raw/typed SDK crosschecks with all288 guarded bytes, carry and secondary. Automatic com.apple.provenance is an observation, not a default empty list. Native Intel and physical iOS remain unverified; full dyld, Mach IPC, Objective-C/Swift and frameworks remain incomplete.

Sources: [XNU syscall ABI](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/kern/syscalls.master), [XNU xattr calls](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU ordinary attributes](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_xattr.c), [XNU xattr definitions](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/sys/xattr.h). Original code/probes; Apple implementation is not copied.

## Bounded object names

ATTR_CMN_NAME=1 is now admitted by getattrlist220/fgetattrlist228/getattrlistat476 for a uniquely named non-root catalogue object. Its legal UTF-8 leaf has 1..255 bytes. The shared object path used by F_GETPATH supplies the last linked spelling through dup, CWD, moves, SWAP, removal and name reuse; caller aliases never replace it. Name and type need no stat; selected stat fields still require valid complete observations. Root/mount labels, malformed names, hard-link or case-fold aliases, normalization and full-path attributes remain unknown.

The 8-byte attrreference_t precedes other common fields; attr_dataoffset is relative to the reference, attr_length includes NUL, and the terminal name area is padded to four bytes. Short output keeps the full required length and exact prefixes, including partial UTF-8. attribute-names / attribute-names-values / attribute-names-unsupported check native behavior, independent literal bytes and a root-name stop preserving prior output through guest/C/CLI/Python. Private ARM64 controls passed 601 raw queries, 453 complete guarded typed SDK comparisons and 384 prefix checks. SDK15.5 has no typed raw476 declaration. Native5s, guest/Python5,000,000us/quantum1024 and existing public10s remain unchanged. Native Intel, physical iOS and full runtimes/frameworks remain unverified or incomplete.

## Bounded bulk directory attributes

getattrlistbulk(461) requires explicit enumeration_policy.bulk_attributes=true; omission or false grants nothing. This virtual TYPE contract returns live immediate child names in unsigned byte order, without dot entries, using local ordinals rather than native filesystem cookies. Initial directory objects retain authorization across dup, moves, SWAP, removal and name reuse; newly created directories do not inherit bulk authorization. The existing minimum_buffer_size, initial_minimum_buffer_size and seek_offset apply only to getdirentries64.

NAME|OBJTYPE|RETURNED_ATTRS (0x80000009) is required, with the existing eleven common fields available when selected observations remain valid. Options 0/8 are admitted; bulk ignores the two 16-bit bitmap/reserved words independently of standalone attrlist validation. One authoritative attribute codec retains attrreference_t and stat64 validity. Only complete records are returned: groups use 8-byte padding when it fits, with a final 4-byte-sized group admitted otherwise. Too-small first groups return ERANGE without output or cursor changes; partial required output stops explicitly before copying. Only bytes actually returned require writable memory.

Dup shares progress; separate opens are independent. A nonzero completed traversal retains EOF after namespace changes and bypasses size/output checks after request validation. Initially empty offset 0 rechecks the view. Zero lseek resets iteration; changed membership before EOF, arbitrary nonzero seeks and mixed getdirentries64/bulk iteration stop explicitly. NAME-only fallback, ERROR-bearing entries, snapshots, ACL/permission decisions, host order and other masks/options remain unsupported. bulk-attributes / bulk-attributes-values / bulk-attributes-unsupported cover native common behavior, literal virtual bytes and an unsupported selection preserving prior output. Private ARM64 preparation passed 728 guarded raw/SDK comparisons. Native5s, guest/Python5,000,000us/quantum1024 and public10s are unchanged. Native Intel, physical iOS and full runtimes/frameworks remain unverified or incomplete.

Sources: [XNU bulk ABI](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/man/man2/getattrlistbulk.2), [XNU attribute definitions](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/sys/attr.h). Original implementation and probes; no Apple implementation copied.

## Explicit ordinary extended-attribute mutations

setxattr(236), fsetxattr(237), removexattr(238) and fremovexattr(239) use an independent initial-object grant: C++ `MutableExtendedAttributes`, strict JSON `mutable_extended_attributes=true`. Each granted file/directory/link must declare a complete ordinary `extended_attributes` list, including a known empty list. Writable content and mutable namespace declarations do not confer this authority. Grants and values follow retained objects through dup, move, removal and mapping leases; created objects and reused names start unknown. Known aliases, conflicting metadata flags, protected system attributes, ResourceFork, FinderInfo and compression semantics remain excluded.

Replacement keeps its virtual list position, removal deletes it, and creation appends. This is a declared process-local order, not an inferred APFS order. Initial attribute costs/counts and grant path references stay reserved. Runtime excess shares the existing 16 MiB/4096 admission owner; removal, content invalidation and final object release reclaim only that excess. Capacity, transport and deadline failures never publish scratch state. A required input with no readable bytes returns EFAULT; partially readable input stops as unsupported before publication. Successful attribute mutation invalidates complete stat without inventing times, while preserving identity, directory membership, enumeration version/snapshot and cursors.

The mutation ABI uses low32 FD/options/position and full64 size. Early privileged/FD link options precede name import; mutation name import precedes object lookup. Set rejects nonempty NULL before oversized VFS input (E2BIG7); lookup precedes ordinary name/position/conflict validation. Set imports the complete value before CREATE-existing EEXIST17 or REPLACE-missing ENOATTR93. Both CREATE and REPLACE give EINVAL; removers ignore those two bits. Zero-size set does not read the value pointer. Other flags, unknown permissions and unobserved provider behavior stop explicitly.

xattr-mutations / xattr-mutations-values / xattr-mutations-unsupported share original native/guest controls, independent literal virtual bytes and a missing-grant stop preserving prior output. Private ARM64 preparation checked726 raw/SDK calls, full544-byte guarded observations and complete readable pages. Native5s/compile120s/drain1s/reap1s, guest/Python5,000,000us/quantum1024 and public10s remain unchanged. Native Intel, physical iOS, dyld, Mach IPC, threads/signals, Objective-C/Swift runtimes and full frameworks remain unverified or incomplete.

Primary ABI references: [XNU syscall declarations](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/kern/syscalls.master), [xattr definitions](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/sys/xattr.h). Original code and probes; Apple implementation is not copied.

## Bounded Darwin hard links

Raw link follows the terminal symbolic target; linkat with flags 0 links the symbolic object and AT_SYMLINK_FOLLOW follows its target. Only low32 flags 0/0x40 are admitted; other low32 bits give EINVAL before import. Source import/lookup and directory EPERM precede destination import. An existing destination gives EEXIST. Destination mutation authority and the same explicitly established mount domain are required; source-parent mutability is not required. Known conflicting devices, flags, special modes and initial identity aliases remain unsupported. Input metadata never coalesces separate initial objects.

A successful alias consumes one namespace entry and its path/NUL charge, with no new inode or duplicate object bytes/attributes. Bytes, independent attribute/content grants, metadata validity and mapping leases remain object-owned. Link counts and ctime use the existing explicit metadata policies; omitted policy leaves complete post-mutation stat unknown. Attribute mutation invalidates complete stat; content mutation invalidates ordinary attribute observations. Removed names retain their cost while held descriptions own them, and the final name/object cost survives mapping-only retention. Rename replacement credits only immediately releasable ownership. Subtree moves/SWAP select exact identities and actual parents; tree-external aliases remain in place and relative symbolic targets use each selected entry parent.

After an object has acquired multiple names, F_GETPATH and vnode ATTR_CMN_NAME remain unsupported even after one or zero names remain. Private native ARM64 controls show lookup-sensitive APFS name observations, with different path/name cache behavior; no general cache model is claimed. Bulk directory NAME uses the selected live entry, independently of vnode-name inference. Same-object ordinary rename/SWAP preserve both entries, while case-insensitive EXCL remains outside the bounded contract. Native Intel HVF, physical iOS, permissions/ACLs, coherent file mappings/EOF signals, dyld, Mach IPC, threads and complete frameworks remain separate gaps. This section extends earlier hard-link exclusions only within this contract.

```text
link(9), linkat(471), AT_SYMLINK_FOLLOW=0x40
DarwinFiles, FileEntry, LinkEntry, NameIdentity, Contents, LinkNode
LinkedNames, HadMultipleNames, DetachedNames
F_GETPATH, ATTR_CMN_NAME, getattrlist(220), fgetattrlist(228), getattrlistat(476)
getattrlistbulk(461), O_SYMLINK
hard-links
hard-links-values
hard-links-name-unsupported
hard-links-attributes-unsupported
HardLink*, HardLinksShareObjectsAndRetainExplicitNameBoundary
native5s / compile120s / drain1s / reap1s
guest/Python5,000,000us / quantum1024 / public10s
34 model cases / 20 guest cases / 20 public cases / 5 Python profiles
66 mandatory workloads per platform / ARM64 198 / Intel 132
```

## Bounded O_SYMLINK descriptors

O_SYMLINK=0x00200000 retains the final symbolic object, including broken or cyclic links, for read-only, write-only and read/write access. It does not grant writable target content. O_CREAT still follows the final target; NOFOLLOW keeps its ELOOP precedence, exclusive creation keeps EEXIST, and O_DIRECTORY rejects a retained link with ENOTDIR. Required intermediate or trailing-slash expansion uses the existing resolver and NOFOLLOW_ANY boundary. F_GETFL omits the selection bit.

Descriptions share the actual LinkNode and selected NameIdentity. Dup shares status flags and cursor; independent opens keep their own descriptions. Held links retain their original object through rename, removal, name reuse and parent removal. Unique F_GETPATH and ATTR_CMN_NAME use the selected retained name; multiple-name history permanently refuses vnode name inference even after all names are removed. Initial fixed reservations remain fixed; dynamic names, targets, entries and attribute growth wait for their actual final owner. Replacement cannot credit a last alias while another description still owns its object or selected name.

Symbolic I/O never exposes raw target bytes as file contents. After the existing scalar/vector import, access and count checks, negative offsets give EINVAL. Reads at INT64_MAX return zero; other admitted offsets give EPERM, including zero-length requests. Writes at INT64_MAX give EFBIG; other admitted offsets give EPERM before zero-length success, APPEND or payload access. The existing early negative pwrite/pwritev rules remain authoritative. DATA/HOLE seek returns ENXIO for nonnegative positions and EINVAL for negative positions, without advancing the cursor.

Admitted open TRUNC, including read-only symbolic selection, only sets WasWritten. Nonnegative ftruncate does the same for write-only/read/write descriptions. Both leave target bytes, full stat, xattrs, cursor, storage admission and inode allocation untouched. Read-only ftruncate and negative lengths give EINVAL. F_SETFL with admitted arguments changes APPEND|NONBLOCK before returning ENOTTY25; dup observes the change and independent opens do not. Unknown arguments stop before effects.

Fixed fpathconf, fgetattrlist and independently declared ordinary FD-xattr authority use the symbolic object. Relative directory-FD lookup and fchdir return ENOTDIR. Attribute mutations keep their existing stat invalidation; truncate cannot restore metadata. Legacy aligned, non-executable private/shared mmap selections reach the symbolic-kind EINVAL refusal without a mapping or lease. Ordinary shared mappings, unknown flags, executable protection and other existing unsupported boundaries remain unchanged. The native mapping controls cover length16384, offset0 and protections1/2/3, not every mmap variant.

Original ARM64 preparation records complete guarded 144-byte stat equality for15 truncate controls and isolated selection cases. Additional O0/O2 observations cover scalar/vector extreme offsets and counts, sparse seeks,18 mapping refusals and failed F_SETFL effects. The SDK-free common workload also compiles at O0/O1/O2 and compares actual descriptor parent paths, avoiding native temporary-root spelling aliases. Virtual routes compare an independent complete stat/type literal or stop explicitly at a multi-name query while preserving output. Native Intel HVF, physical iOS, ACL/permission enforcement, coherent mapped EOF/signals, dyld, Mach IPC, threads and full runtimes/frameworks remain unverified or incomplete.

Primary interpretation: [matching XNU mapping boundary](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/kern/kern_mman.c). Implementation and probes are original; no Apple implementation is copied.

```text
O_SYMLINK=0x00200000; ENOTTY=25
open O_RDONLY|O_SYMLINK|O_TRUNC: WasWritten only
LinkNode, NameIdentity, HadMultipleNames, DetachedNames
INT64_MAX read=0 / write=EFBIG; other admitted offsets=EPERM
SEEK_DATA/SEEK_HOLE nonnegative=ENXIO / negative=EINVAL
F_SETFL: APPEND|NONBLOCK effect before ENOTTY; dup shares / independent open separate
symbolic-descriptors
symbolic-descriptors-values
symbolic-descriptors-name-unsupported
SymbolicDescriptor*, SymbolicDescriptorsRetainObjectsAndNativeErrorOrder
native5s / compile120s / drain1s / reap1s
guest/Python5,000,000us / quantum1024 / public10s
34 model cases / 20 transport parameters / 15 public cases / 5 Python profiles
66 mandatory workloads per platform / ARM64 198 / Intel 132
```

## Bounded nonblocking descriptor state

O_NONBLOCK=4 is accepted for regular, directory and O_SYMLINK opens and retained by F_GETFL. Dup shares the status and cursor; independent opens retain their own descriptions. Existing access, close-on-exec, WasWritten, metadata, bytes and cursor rules remain authoritative. Finite declared stdin retains EOF and pointer-error behavior; omitted input remains unknown. Captured output retains its copy faults and shared output budget.

F_SETFL validates supported low32 arguments before effects, converts the native open-flag word by adding one, then changes only APPEND|NONBLOCK. High32 bits are ignored; access and input WasWritten bits cannot grant access or fabricate a write. The literal native controls for requests3/7/11/15 select status4/8/12/0. Symbolic descriptions commit these status changes before ENOTTY25. Unknown flags such as ASYNC0x40 stop before effects.

The original ARM64 macOS preparation records122 observations, including16 low-bit requests for each valid object/access combination, held duplicates, independent opens, clearing, real writes and per-FD CLOEXEC. The SDK-free shared program runs at O0/O1/O2 and compares bytes, status, cursor and raw BSD carry/error ABI. Guest, C/CLI and Python routes also require the unknown-flag refusal; all three ARM64 HVF profiles are mandatory. This adds no readiness waits, pipes, networking, kqueue, asynchronous signals or host I/O. O_EVTONLY process policy remains unsupported. Native Intel HVF, physical iOS and the full macOS/iOS environment remain unverified or incomplete.

```text
O_NONBLOCK=4; F_SETFL raw low32 mask=0x1000f
requests3/7/11/15 -> APPEND|NONBLOCK status4/8/12/0
nonblocking-descriptors / nonblocking-flags-unsupported
Nonblocking*, NonblockingDescriptorsKeepNativeControlState
native5s / compile120s / drain1s / reap1s
guest/Python5,000,000us / quantum1024 / public10s
8 model cases / 20 transport parameters / 10 public cases / 5 Python profiles
67 mandatory workloads per platform / ARM64 201 / Intel 134
```

## Explicit finite getentropy observations

Raw BSD getentropy500 accepts an ordered `DarwinSystemOptions::EntropyReads` observation queue, encoded as JSON `darwin_system.entropy_reads`. Records are nonempty even-length hexadecimal strings: at most256 records, each1..256 bytes. These are finite model limits; the existing65536-byte JSON transport limit is unchanged. Omission is unknown; `[]` is explicitly exhausted. Strict native/JSON validation runs before image or backend mutation, and other OS profiles reject Darwin options.

The owner checks the full64-bit length first: values above256 return EINVAL22 without memory access or consumption; zero succeeds for every pointer without accessing memory or requiring input. A nonzero request admits the next exact-length record before copyout. Missing, exhausted or mismatched observations stop UnsupportedService before effects, including for a bad address; that ordering describes replay admission. A successful copy or wholly inaccessible EFAULT14 consumes exactly one admitted record. Partial writable destinations stop before any bytes or cursor changes; memory transport errors remain errors without advancing. A later return-register failure preserves completed copy/consumption effects. Every public process execution starts a fresh cursor, even when the same options object is reused.

One DarwinEntropy instance per run owns replay position; supplied bytes stay immutable. Existing BSD dispatch and returnService own both ISA bindings, carry and secondary registers. The SDK-free replay fixture verifies zero/non-UTF8 bytes, canaries, whole faults and refusal modes across five guest profiles and three actual ARM64 HVF profiles. Fixed replay bytes do not enter the native deterministic RNG inventory. Original native ARM64 probes at O0/O1/O2 cover594 calls; changed sentinel-byte counts are not exact copy lengths. Host randomness, cryptographic quality, /dev/random, libc imports and frameworks are not provided. Native Intel HVF, physical iOS and full OS compatibility remain unverified or incomplete.

```text
BSD getentropy500 / DarwinEntropy / EntropyReads / darwin_system.entropy_reads
full64 size>256 -> EINVAL22; zero ->0; whole EFAULT14 consumes one record
1..256 bytes per record / at most256 records / JSON transport65536 bytes
entropy-replay / entropy-missing / entropy-exhausted / entropy-mismatch / entropy-partial
15 model cases / 20 transport parameters / 26 public cases / 5 Python profiles
68 mandatory workloads per platform / ARM64 204 / Intel 136
native5s / compile120s / drain1s / reap1s
owner/build1200s / guest/Python5,000,000us / quantum1024 / public10s
```

[XNU getentropy ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/syscalls.master), [XNU generation/copyout boundary](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/dev/random/randomdev.c).

## Explicit current-thread identity

Raw BSD thread_selfid372 reads the immutable optional `DarwinSystemOptions::ThreadID` / JSON `darwin_system.thread_id`. Every supplied uint64 bit pattern, including zero, is known; omission stops UnsupportedService. Decimal strings preserve all64 bits; numeric JSON is limited to exact integers up to2^53-1. No ID is inferred from the host, PID or a Mach port. The no-argument call ignores all six carriers and accesses no memory. Existing low32 syscall resolution preserves the complete raw number in events; the shared BSD return owner preserves full64 values, clears carry and clears RDX/X1. Mach spellings remain unsupported.

Repeated executions reuse the supplied observation; independent options remain independent. This does not allocate IDs, guarantee uniqueness, create scheduler event identities, or model thread lifecycle, pthreads, TLS or Mach IPC. Original ARM64 O0/O1/O2 probes retain24 calls comparing raw results with the SDK current pthread ID, arbitrary argument seeds and high32 numbers. The stable common fixture compares relationships within one native process; literal supplied-ID bytes are excluded from the deterministic native reference inventory. Native Intel HVF, physical iOS and full OS compatibility remain unverified or incomplete.

```text
BSD thread_selfid372 / Wide / ThreadID / darwin_system.thread_id
known uint64 including0 / missing -> UnsupportedService / no memory
full64 return / low32 resolution / carry clear / RDX-X1 zero / raw event number
thread-identity / thread-identity-value / thread-identity-missing
4 model cases / 20 transport parameters / 16 public cases / 5 Python profiles
69 mandatory workloads per platform / ARM64 207 / Intel 138 unverified
original ARM64 O0/O1/O2 probes24 / native5s / compile120s / drain1s / reap1s
owner/build1200s / guest/Python5,000,000us / quantum1024 / public10s
```

[XNU thread_selfid ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/syscalls.master), [libpthread current-thread owner](https://github.com/apple-oss-distributions/libpthread/blob/42d026df5b07825070f60134b980a1ec2552dfee/kern/kern_support.c).

## Explicit Mach self-port observations

Raw Mach thread_self_trap27, task_self_trap28 and host_self_trap29 read independent optional uint32 observations: DarwinSystemOptions::ThreadSelfPort, TaskSelfPort and HostSelfPort, encoded by darwin_system.thread_self_port, task_self_port and host_self_port. Each query requires only its selected field. Missing is unknown and stops UnsupportedService; zero, equal names and every32-bit pattern are explicit values. Inputs accept exact integers or decimal strings through UINT32_MAX. They do not inherit the positive pid_t bounds of process_group_id/session_id.

The system owner converts the name through the signed int32 kernel result into the raw64 carrier: 0x80000001 becomes0xffffffff80000001 and UINT32_MAX becomesUINT64_MAX. The existing Mach binding preserves flags and X1/RDX, keeps x64 RCX/R11 syscall clobbers, ignores all arguments and accesses no memory. Low32 resolution retains the full raw number in events. Mach query events omit the BSD error field and never synthesize a scheduler ThreadID. Reusing options preserves the observation; independent options remain independent.

Original ARM64 O0/O1/O2 preparation retains432 raw observations, all16 NZCV combinations, high32 number prefixes, seeded carriers and SDK agreement. No native bit31 port name was observed; high-bit sign extension is a pinned XNU return-path contract with independent literal model/guest/public tests. The common native fixture checks only within-process relationships. Literal virtual names and missing-input modes do not enter native deterministic references. These observations do not allocate names or send references, authenticate live rights, infer uniqueness or implement IPC/lifetime/thread scheduling. Permission/ACL enforcement, readiness, advancing clocks, real Mach IPC/threads, dyld/TLS and full runtimes/frameworks remain incomplete. Native Intel HVF and physical iOS remain unverified.

```json
{"darwin_system":{"thread_self_port":2147483649,"task_self_port":0,"host_self_port":"4294967295"}}
```

```text
Mach thread_self_trap27 / task_self_trap28 / host_self_trap29
ThreadSelfPort / TaskSelfPort / HostSelfPort / uint32 / signed-int32 -> raw64
known0 / missing -> UnsupportedService / no arguments or memory
low32 resolution / complete raw number / flags and RDX-X1 preserved / no BSD error
mach-self-ports / mach-self-port-values / mach-self-port-missing
MachSelfPortsPreserveExplicitBitsAndIndependentRuns
6 model cases / 20 transport parameters / 26 public cases / 5 Python profiles
70 mandatory workloads per platform / ARM64 210 / Intel 140 unverified
original ARM64 O0/O1/O2 probes432 / no native bit31 name observed
native5s / compile120s / drain1s / reap1s
owner/build1200s / guest/Python5,000,000us / quantum1024 / public10s
```

[XNU Mach trap table](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/kern/syscall_sw.c), [self-port name owners](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/kern/ipc_tt.c), [host-port owner](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/kern/ipc_host.c), [ARM64 return](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/bsd_arm64.c), [x64 return](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/i386/bsd_i386.c).


## Static ordinary-owner permission queries

The exact opt-in `darwin_files.authorization="static-owner-queries"` (C++
DarwinFileAuthorization::StaticOwnerQueries) declares an immutable ordinary
local permission environment: no ACL, MAC, additional kauth listener,
entitlement or other permission bypass; a writable, executable, nonopaque local
mount with ownership enabled; supplied flags=0 and no special mode bits.
Metadata and credentials alone never make this declaration. It supplies static
access/faccessat permission queries; normal file workloads need a later, broader
authorization environment.

An actual check requires explicit darwin_system.credentials and the actual
object's metadata. Access selects real_uid; AT_EACCESS selects effective_uid for
both pathname SEARCH and final R/W/X. The selected UID must be nonzero and match
the object's UID. Every requested owner permission bit must be present; a known
denial returns EACCES13 with the existing BSD carry convention. Missing
credentials/metadata, selected UID0, nonowner, extended actions and R/W/X on a
retained terminal link stop UnsupportedService. An unselected UID0 remains a
valid observation. No host identity, UID1000 fallback, group/world permissions
or root exemption is inferred.

SEARCH uses the current parent directory's X bit before child lookup, including
missing children, applicable dot/dotdot reductions and every symlink restart.
F_OK and ignored mode bits require only those actual SEARCH checks, with no final
owner check. Slash-only root LOOKUP and root-clamped dotdot perform no SEARCH;
consumed trailing separators do not add a final-directory SEARCH. Invalid flags,
path copying, relative dirfd admission and empty-name errors keep their original
order. After known allowed SEARCH, a component beyond Name255 stops Unsupported:
that resource bound does not prove filesystem ENAMETOOLONG. Denied SEARCH still
returns EACCES13 first; unknown SEARCH stops before the child lookup. The
separate whole copied-path bound is unchanged.

All other dispatched file operations, including read-only open, O_WRONLY/TRUNC,
stat, chdir, readlink, pathconf, attributes, enumeration and namespace mutation,
stop before effects. Only established typed Input/Output/Error standard streams
and their dup aliases retain read/write/vector/positioned preflight, close,
dup/dup2, lseek and existing fcntl controls. Numeric FD0/1/2 does not grant this
exception. File-backed mmap and direct mappingSource are closed; anonymous
memory remains independent. Shared C++/JSON admission rejects mutation/creation
policies and grants, known flags/special bits and known inode/device aliases.
Missing metadata remains unknown. The declaration adds no path/entry charge and
keeps the existing256-entry/16MiB limits.

The example supplies actual root SEARCH facts as well as a0400 owner file;
queries can read-check /data and return EACCES for its write-check. It does not
open that file:

```json
{"darwin_files":{"authorization":"static-owner-queries","files":[{"path":"/data","bytes_hex":"00","metadata":{"device":7,"inode":2,"mode":33024,"link_count":1,"uid":501,"gid":20,"size":1,"block_size":4096,"blocks":0,"flags":0,"generation":0,"access_time":{"seconds":0,"nanoseconds":0},"modification_time":{"seconds":0,"nanoseconds":0},"change_time":{"seconds":0,"nanoseconds":0},"birth_time":{"seconds":0,"nanoseconds":0}}}],"directories":[{"path":"/","metadata":{"device":7,"inode":1,"mode":16832,"link_count":2,"uid":501,"gid":20,"size":0,"block_size":4096,"blocks":0,"flags":0,"generation":0,"access_time":{"seconds":0,"nanoseconds":0},"modification_time":{"seconds":0,"nanoseconds":0},"change_time":{"seconds":0,"nanoseconds":0},"birth_time":{"seconds":0,"nanoseconds":0}}}],"working_directory":"/"},"darwin_system":{"credentials":{"real_uid":501,"effective_uid":501,"real_gid":20,"effective_gid":20}}}
```

Original native ARM64 O0/O1/O2 evidence retains2472 raw/SDK pairs, including2439
independent literal checks and33 capture-only observations, under the original
compile120s/native5s limits. The private held-dirfd tree records actual owner,
mode, flags, statfs and fstatx/filesec ACL-property absence. The initial probe's
NULL/ENOENT was an ACL-absence protocol mismatch, before any permission queries;
its failed record is retained. Native real/effective IDs are equal on this host;
distinct identity selection is a pinned-source/model contract. Absence of every
global security hook and opaque mount internal is not independently observed.
The SDK-free owner-queries workload is supplied-model evidence and stays out of
the58 deterministic native-common workloads. Group/root/ACL/MAC authorization,
dynamic credentials, authorized open/creation/namespace operations, readiness,
advancing clocks, Mach IPC/threads, dyld/TLS and complete runtimes/frameworks
remain unfinished. Native Intel HVF and physical iOS remain unverified.

```text
DarwinFileAuthorization::StaticOwnerQueries / authorization=static-owner-queries
access33 / faccessat466 / real_uid / effective_uid / AT_EACCESS0x10
owner R/W/X / all requested bits / directory SEARCH / EACCES13
no-action root LOOKUP / root-clamped dotdot / consumed terminal separators
unknown credentials-metadata-root-nonowner -> UnsupportedService
all other vnode routes closed / typed standard streams and dup aliases only
anonymous memory independent / file-backed mmap and mappingSource closed
Name255 availability stop after allowed SEARCH / no guessed filesystem errno
owner-queries / owner-query-stop / owner-query-open / owner-query-map
OwnerQueriesKeepPermissionAndUnknownBoundaries
73 mandatory workloads per platform / ARM64 219 / Intel 146 unverified
original ARM64 O0/O1/O2 pairs2472 / literal2439 / capture-only33
native5s / compile120s / owner-build1200s / guest-Python5,000,000us
```

[XNU access and subject selection](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [real credential copy](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_credential.c), [owner authorization](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_subr.c), [pathname SEARCH](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_lookup.c), [cached lookup](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_cache.c), [Libc ACL properties](https://github.com/apple-oss-distributions/Libc/blob/Libc-1698.140.3/gen/filesec.c), [fstatx ACL absence](https://github.com/apple-oss-distributions/Libc/blob/Libc-1698.140.3/sys/statx_np.c).

## Static ordinary queries with partial group knowledge

The exact opt-in `darwin_files.authorization="static-ordinary-queries"` (C++
DarwinFileAuthorization::StaticOrdinaryQueries) extends the preceding immutable
ordinary environment to nonowners. It keeps the same declared mount/security
assumptions, metadata admission, pathname SEARCH points, default-closed file
routes, typed standard-stream exceptions and independent anonymous VM. The old
static-owner-queries spelling and matching-owner-only behavior are unchanged.
Actual checks still require explicit credentials, metadata and a nonzero
selected UID. Selected root, extended actions and terminal-link R/W/X remain
unsupported.

A matching owner uses all requested owner bits. For a nonowner, compare the
**whole requested mask** against group and world permissions. Equal outcomes
supply a known success or EACCES13 without any group lookup; different bit sets
can produce equal denials. When the outcomes differ, a known member selects
group bits and a proved nonmember selects world bits. Unresolved membership
stops UnsupportedService before lookup or effects; permissions from different
classes are never combined.

The explicit credentials.groups field is the ordered in-credential group list,
with EffectiveGID at index0 and duplicates retained. It is not the SDK's
extended resolver-backed getgroups list. The selected primary GID is a known
member, even when additional groups are omitted. A positive entry is also known;
a missing entry generally leaves external membership unknown. An omitted list
never proves nonmembership.

Access/faccessat without AT_EACCESS use the pinned real-credential copy. If both
UID/GID pairs agree, it preserves the original context. Otherwise, the copy
replaces index0 with RealGID and swaps the old EffectiveGID into the first
matching supplementary RealGID entry, when present. With no supplementary match,
it displaces the old primary and disables memberd. That proved displacement or explicit original KAUTH_UID_NONE, together with an
explicit complete in-credential list, makes missing groups known negative.
Unequal UIDs with equal GIDs still take this path; a duplicate primary in a
supplementary position can preserve unknown external membership. AT_EACCESS
always uses the original effective context. The supplied record never changes.

The example supplies a nonowned group-readable/writable file. Its real query
is denied because GID20 was displaced, while AT_EACCESS uses known primary GID20
and succeeds. Root-directory SEARCH succeeds from group/world agreement; this
does not authorize a selected UID0 or open the file:

```json
{"darwin_files":{"authorization":"static-ordinary-queries","files":[{"path":"/data","metadata":{"device":7,"inode":2,"mode":32816,"link_count":1,"uid":700,"gid":20,"size":1,"block_size":4096,"blocks":0,"flags":0,"generation":0,"access_time":{"seconds":0,"nanoseconds":0},"modification_time":{"seconds":0,"nanoseconds":0},"change_time":{"seconds":0,"nanoseconds":0},"birth_time":{"seconds":0,"nanoseconds":0}},"bytes_hex":"00"}],"directories":[{"path":"/","metadata":{"device":7,"inode":1,"mode":16895,"link_count":2,"uid":0,"gid":0,"size":0,"block_size":4096,"blocks":0,"flags":0,"generation":0,"access_time":{"seconds":0,"nanoseconds":0},"modification_time":{"seconds":0,"nanoseconds":0},"change_time":{"seconds":0,"nanoseconds":0},"birth_time":{"seconds":0,"nanoseconds":0}}}],"working_directory":"/"},"darwin_system":{"credentials":{"real_uid":501,"effective_uid":502,"real_gid":30,"effective_gid":20,"groups":[20,40]}}}
```

Read-only native ARM64 O0/O1/O2 preparation retains270 raw/SDK pairs for
actual nonowned files/directories, SEARCH, errors, held-object identity and
independent ACL-property absence. Raw BSD79 observed16 in-credential groups;
SDK getgroups observed17 extended groups. The initial SDK-length rejection
stopped before permission queries and remains preserved. Real/effective IDs
are equal on this host: distinct identity transforms and complete membership
rules use pinned-source and independent model evidence. The native subset does
not establish absence of every external hook. Five software and three ARM64
HVF profiles execute the supplied guest plus C/CLI/Python controls and unknown
identity/metadata/root/membership stops. This workload stays outside the58
native-common deterministic references. Full group resolution, root, ACL/MAC,
general vnode authorization, dynamic credentials, readiness/networking,
advancing clocks, Mach IPC/threads, dyld/TLS and complete frameworks remain
unfinished; native Intel HVF and physical iOS remain unverified. All deadlines
remain unchanged.

```text
DarwinFileAuthorization::StaticOrdinaryQueries / authorization=static-ordinary-queries
owner bits / whole-mask group-world outcomes / EACCES13
credentials.groups / in-credential16 / EffectiveGID index0 / duplicates retained
real credential copy / first supplementary match / displacement disables memberd
missing membership usually unknown / original NONE or displaced real plus complete list proves negatives
all40 other file routes and direct/file-backed mappings closed / typed streams only
ordinary-queries / ordinary-query-unknown / ordinary-query-open / ordinary-query-map
OrdinaryQueriesPreserveGroupKnowledgeAndSelectedSearch
73 mandatory workloads per platform / ARM64 219 / Intel 146 unverified
original ARM64 O0/O1/O2 nonowner pairs270 / raw-groups16 / SDK-extended-groups17
native5s / compile120s / guest-Python5,000,000us / quantum1024 / public10s
```

[XNU ordinary mode authorization](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_subr.c), [real credential and group membership](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_credential.c), [raw in-credential getgroups](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_prot.c), [SDK extended getgroups](https://github.com/apple-oss-distributions/Libc/blob/Libc-1698.140.3/sys/getgroups.c).

## Explicit original group membership context

Optional `DarwinCredentials::GroupMembershipUID` / `darwin_system.credentials.group_membership_uid` declares the original `cr_gmuid`, independently of the four scalar IDs and `groups`. It accepts 0..INT32_MAX or exactly KAUTH_UID_NONE=4294967195 (0xffffff9b, UINT32_MAX minus100). Lossless decimal strings and exact numeric integers use the existing decoder; malformed, fractional, negative and other out-of-range inputs fail before loading. The sentinel remains invalid in scalar IDs and group entries. Omission supplies no external membership knowledge, and another admitted UID does not enable a resolver.

A selected primary or positive in-credential entry remains known first. Original NONE plus an explicit complete list proves a missing entry is a nonmember; NONE with an omitted list cannot prove that. The real-credential copy preserves original NONE even when its first supplementary match retains the old effective primary. A proved displacement also disables external membership. These inputs do not change getuid/geteuid/getgid/getegid, raw getgroups, creation ownership or the legacy authorization modes. The same ordinary-query owner selects world permissions and checks pathname SEARCH before child lookup; all other vnode operations remain closed.

The example declares a known nonmember of GID50: both identities may read `/data`, while a write-permission query returns EACCES13. Omitting group_membership_uid leaves the differing group/world decision unsupported.

```json
{"darwin_files":{"authorization":"static-ordinary-queries","files":[{"path":"/data","bytes_hex":"00","metadata":{"device":7,"inode":2,"mode":32772,"link_count":1,"uid":700,"gid":50,"size":1,"block_size":4096,"blocks":0,"flags":0,"generation":0,"access_time":{"seconds":0,"nanoseconds":0},"modification_time":{"seconds":0,"nanoseconds":0},"change_time":{"seconds":0,"nanoseconds":0},"birth_time":{"seconds":0,"nanoseconds":0}}}],"directories":[{"path":"/","metadata":{"device":7,"inode":1,"mode":16895,"link_count":2,"uid":0,"gid":0,"size":0,"block_size":4096,"blocks":0,"flags":0,"generation":0,"access_time":{"seconds":0,"nanoseconds":0},"modification_time":{"seconds":0,"nanoseconds":0},"change_time":{"seconds":0,"nanoseconds":0},"birth_time":{"seconds":0,"nanoseconds":0}}}],"working_directory":"/"},"darwin_system":{"credentials":{"real_uid":501,"effective_uid":501,"real_gid":20,"effective_gid":20,"groups":[20],"group_membership_uid":4294967195}}}
```

Local O0/O1/O2 SDK executions verify only the sentinel and four-byte uid_t carrier. They do not observe the host's cr_gmuid, disable its resolver or replace the earlier270 actual raw/SDK nonowner pairs. The supplied `ordinary-queries-closed-groups` workload checks173 events, effective nonmember denials and allowed/denied search through missing children, dot/dotdot and links across five software profiles and three mandatory ARM64 HVF profiles, with C/CLI/Python admission checks. It remains outside the58 native-common deterministic references. Root, ACL/MAC, full group resolution, general vnode authorization, dynamic credentials, readiness/networking, advancing clocks, Mach IPC/threads, dyld/TLS and complete frameworks remain unfinished. Native Intel HVF and physical iOS are unverified; original deadlines are unchanged.

```text
GroupMembershipUID / group_membership_uid / original cr_gmuid
0..INT32_MAX or KAUTH_UID_NONE=4294967195 / 0xffffff9b / not UINT32_MAX
positive entries first / original NONE plus complete list proves negatives
omitted list unknown / first-match real copy preserves original NONE
ordinary-queries-closed-groups / 173 events / stdout GN
OrdinaryQueriesUseExplicitMembershipUIDWithoutResolver
73 mandatory workloads per platform / ARM64 219 / Intel 146 unverified
SDK constant O0/O1/O2 only / prior actual nonowner pairs270 remain separate
58 native-common references unchanged / Intel and physical iOS unverified
native5s / compile120s / guest-Python5,000,000us / quantum1024 / public10s
```

[XNU KAUTH_UID_NONE](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/kauth.h), [XNU credential membership](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_credential.c).
