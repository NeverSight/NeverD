# Explicit Linux device transactions

Linux ELF64 and Android API 28 workloads can supply device sessions under
`linux_files.devices`. This is an input contract for analysis, not a kernel
driver, inferred hardware behavior or access to the host's devices. An absent
or mismatched transaction stops execution; it never supplies implicit success.
Recorded and synthetic inputs must be distinguished by the caller.

```json
{"linux_files":{"files":[],"devices":[{
  "path":"/fixture/control","access_mode":2,"open_errno":0,
  "ioctls":[{"request":2701131777,"result":0,"memory":[
    {"offset":0,"expected_hex":"3132","output_hex":"5253"}
  ]}]
}]}}
```

C++ uses `LinuxFileOptions::Devices` and the types in
[`LinuxDeviceOptions.h`](../include/neverd/emulation/LinuxDeviceOptions.h).
The C ABI, CLI and Python use the same process JSON parser and Linux owner.

Each device requires `path`, `access_mode`, `open_errno` and `ioctls`. Paths
share the existing closed file/link namespace and cannot overlap another
entry or serve as its parent. Access mode is the exact admitted `O_ACCMODE`
(0, 1 or 2). Opens admit optional `O_CLOEXEC` and the architecture's
`O_LARGEFILE`, without creation, truncation, nonblocking or direct-I/O behavior.
`open_errno:0` explicitly admits a descriptor; 1–4095 specifies a guest error.
The shared descriptor ceiling and lowest-free allocation still apply. Regular
files remain immutable. Closing retires the descriptor; device mappings can
retain its open description. No host driver's release callback executes.

`dup`, `dup2` and `dup3` share the original open description: file cursor,
device transaction position, context ownership and captured output destination.
`dup` selects the lowest free descriptor; replacement is atomic in this
single-core model. `dup2(fd,fd)` validates then retains the descriptor;
`dup3(fd,fd,flags)` returns `EINVAL`. `dup3` admits zero or `O_CLOEXEC`; execution
and `fcntl` inspection of close-on-exec flags remain unsupported. ARM64 has no
raw `dup2` syscall, but its Bionic import shares the same operation.

Every open starts its own ordered ioctl sequence. Other opens, even to the
same path, have independent positions; this contract does not infer shared
hardware state. Threads using the same descriptor share its position. A
successful transaction consumes exactly one entry. Duplicated descriptors share
that position. Exhaustion, a request
mismatch or a failed input check stops before outputs and consumption. Closing
may abandon unconsumed entries; reopening starts a fresh sequence.

Each ioctl requires `request` (unsigned 32-bit), `result` (signed 64-bit,
at least -4095), and `memory` (possibly empty). Optional `argument` matches
the complete unsigned 64-bit third argument. Wide integers may use decimal
strings. Raw x64/ARM64 ioctl, Android `ioctl`, variadic `syscall` and named
dynamic calls share descriptor lookup and the low-32-bit fd/request values.
Invalid descriptors return `EBADF` before examining a transaction. Bionic
alone converts kernel errors to `-1` and thread errno. Dynamic-provider
lifetime checks remain in force.

A memory entry requires `offset`, `expected_hex` and `output_hex`. Empty
expected/output bytes omit that direction; both cannot be empty. The address
is `argument + offset`. Optional `pointer_offset` first reads one little-endian
64-bit pointer at `argument + pointer_offset`, then adds `offset`. No structure
layout or transfer size is derived from the request's `_IOC` bits; ioctl
arguments can also be scalars, as the
[kernel interface documentation](https://docs.kernel.org/driver-api/ioctl.html)
describes.

All pointers and expected bytes refer to memory at transaction entry. The
model validates every input and output span before writing, then applies
outputs in listed order. Overlapping outputs therefore have explicit order,
while an earlier output cannot change the pointer or expected bytes used by
a later entry. Missing permissions, overflowing addresses and mismatched
bytes stop; they do not invent a driver's partial copy or error code.
An explicit negative result may still have declared outputs, if that is the
provided observation. Backend failures remain terminal errors.

The combined namespace is limited to 256 entries, with 16 MiB of path and
file/link/transaction/update payload bytes. At most 1,024 ioctl entries and
scheduled updates combined are admitted
across the catalogue, with 16 memory regions per entry. The process JSON
request limit and execution budgets also apply. Both C++ and JSON inputs use
the same validation. Unknown fields and malformed hex or numeric values fail
before execution.

## Read-only shared mapping snapshots

An optional `read_only_mappings` array supplies complete RAM pages, independently
of ioctl transactions. Each entry has `offset` (unsigned byte offset), one of
`bytes_hex` or `bytes_base64`, and optional `updates` described below.
Base64 uses canonical padded RFC 4648
encoding; whitespace, URL-safe variants and nonzero padding bits are rejected.
The decoded bytes have identical validation and mapping semantics. This compact
encoding permits a 32 KiB snapshot plus options within the unchanged 64 KiB
JSON request limit; larger complete requests still fail at that boundary.
Offsets and nonempty payload lengths must be multiples of
4,096; their sum must fit a signed 64-bit offset. Entries cannot overlap.
At most 16 entries per device are admitted, and their bytes count against the
same 16 MiB catalogue budget. Omitting the array preserves the unsupported
mapping boundary. No ioctl result creates a mapping input implicitly.

The admitted operation is non-fixed `MAP_SHARED` with `PROT_READ` or `PROT_NONE`.
The page-rounded request must fit wholly within one supplied interval. Missing
pages, private/fixed mappings, unknown flags and other devices remain explicit
unsupported boundaries. A matching read-only interval rejects writable or
executable protection, and a write-only descriptor, with `EACCES`. Empty lengths
and unaligned offsets return `EINVAL`; invalid descriptors return `EBADF`.
Bionic alone converts raw errors to `MAP_FAILED` and errno. Address hints and
allocation limits use the same Linux placement owner as anonymous mappings.

All live mappings of a catalogue entry and offset share the same physical page,
including mappings made through independent opens. Closing the descriptor leaves
its mappings valid, consistent with the [Linux mmap contract](https://man7.org/linux/man-pages/man2/mmap.2.html).
`mprotect` can switch between read and no access but cannot add write or execute
permission. A permission change across adjacent mappings retains any successful
prefix before reaching a denied interval. Partial `munmap` retires exactly the
affected aliases and protection policy. Physical pages are released when the last
alias or pinned observer releases them; a later fresh mapping reads the current
device snapshot, including committed model/producer writes. Virtual aliases and physical pages retain their separate
memory-budget charges.

Without model writes or supplied updates these remain static RAM snapshots.
They do not implement register side effects, cache coherency or GPU execution.
Device advice, read/write, seek, metadata and polling still require their own
semantics. Reports must retain the inputs, actual native/service events and first
remaining boundary; a supplied snapshot does not establish original-device
equivalence or execute callbacks.

## Finite external producer updates

A mapping's optional `updates` array supplies deterministic external byte writes:

```json
"updates": [
  {"after_nanoseconds":1000000,"offset":40,"bytes_hex":"07000000"},
  {"after_nanoseconds":2000000,"offset":48,"bytes_hex":"07000000"}
]
```

Time is elapsed virtual time from workload start; `offset` is relative to the
mapping's initial bytes. Both are unsigned 64-bit integers or decimal strings.
Each patch is nonempty, at most 4,096 bytes, wholly within its mapping. Updates
must be ordered by nondecreasing time; equal-time writes keep array order.
Cross-page writes are permitted within that size bound. Unknown fields fail.

`LinuxClock` advances through admitted sleeps and, optionally, the explicit
[`nanoseconds_per_instruction` policy](process-emulation.md#explicit-guest-clocks).
Before guest execution resumes, due writes update the authoritative device
snapshot and live physical aliases, including `PROT_NONE` mappings. They never
grant guest write permission. Partial unmapping and address reuse cannot redirect
a later update into anonymous RAM; no live mapping is required for the snapshot
to advance. Instructions within one execution interval observe the preceding
boundary's bytes, so `instruction_quantum` remains part of the experiment.
Host time, sample PCs and inferred hardware events do not trigger writes.

## Explicit KGSL context subset

A device may opt into `"kgsl_contexts":{"memstore_offset":4096,"context_limit":3}`.
The offset must select one declared mapping whose 40-byte slots fit IDs 0 through
the inclusive limit (1–1,024). This is an LP64 Adreno protocol subset, not a GPU
driver. Every context starts idle. Command execution requires the separate explicit
profile below; externally submitted queued timestamps, fault recovery, secure
contexts, hardware scheduling and original SDKs are absent.
An external producer trace is a declared workload assumption, not command execution.

The implemented requests are `0xc0080913` (create), `0x40040914` (destroy), and
`0xc00c0916` (read context timestamp). Create admits flags `0x12`
(`PREAMBLE|NO_GMEM_ALLOC`) or the returned default `0x8052`; missing required
flags return `EINVAL`, other flags stop. It allocates the lowest free device-wide
ID, writes back default priority/per-context timestamp flags, and zeros only
the consumed and retired words. Finite ID exhaustion returns `ENOSPC`.

Contexts belong to the creating open description, shared by duplicated FDs and
retained by that description's mappings. Another independent open cannot query
or destroy them (`EINVAL`), even though its mapping sees the same device bytes.
Timestamp types 1 and 2 read the consumed/retired words at slot offsets 0 and 8;
type 3 requires the queue proof below, and invalid types copy zero with EINVAL.
Read-only or partial result buffers stop before allocation or output effects. Static ioctl entries cannot shadow these three modeled requests.

Destroy or final-reference retirement admits only equal consumed and retired
timestamps under the declared quiescent producer contract. An unequal pair
stops before closing/replacing the last FD or unmapping the last retained VMA;
the model cannot infer a hardware wait or the eventual completion value.
Closing an FD while a VMA retains the description preserves its contexts.
Once all references are gone, IDs become available to other opens.

The ABI facts and narrow lifecycle rules were independently implemented from
the pinned AOSP msm revision `12d6459848fcd3bf073765e61299d8d87ec14fdf`:
[UAPI](https://android.googlesource.com/kernel/msm/+/12d6459848fcd3bf073765e61299d8d87ec14fdf/include/uapi/linux/msm_kgsl.h),
[ioctl and ownership](https://android.googlesource.com/kernel/msm/+/12d6459848fcd3bf073765e61299d8d87ec14fdf/drivers/gpu/msm/kgsl.c),
[Adreno context initialization](https://android.googlesource.com/kernel/msm/+/12d6459848fcd3bf073765e61299d8d87ec14fdf/drivers/gpu/msm/adreno_drawctxt.c).
Selecting this model does not identify a captured device's kernel version or
establish equivalence to all vendor variants.

### Queued timestamp admission

A newly created context has a known empty queue only if no declared producer
update can touch its 40-byte memstore slot. Type 3 then reports zero. A producer
for that context makes queued state unknown: consumed/retired page words do not
supply a queued timestamp. Unknown timestamp types return EINVAL and copy out a
zero timestamp. This follows the read/copy-out contracts in the pinned
[KGSL ioctl owner](https://android.googlesource.com/kernel/msm/+/973f4134d9deb396415846f902848f0a32cb4cfa/drivers/gpu/msm/kgsl.c)
and [Adreno timestamp owner](https://android.googlesource.com/kernel/msm/+/a65478df351382d255d293d745aee9bae3a3a741/drivers/gpu/msm/adreno.c).
Blocking waits and busy-context retirement remain unmodeled. The explicit
command profile below owns its own queued and retired timestamps.

## Binder control and looper protocol

An explicit device entry can set `"binder":true`, with `access_mode`,
`open_errno` and an empty `ioctls` array as usual. For example:

```json
{"path":"/dev/binder","access_mode":2,"open_errno":0,
 "ioctls":[],"binder":true}
```

The LP64 protocol-8 subset implements VERSION, SET_MAX_THREADS, THREAD_EXIT and
WRITE_READ with REGISTER/ENTER/EXIT_LOOPER commands. Every independent open owns
its control state and per-native-thread loopers; dup aliases share that owner.
O_NONBLOCK is admitted for this device model. Fresh Binder threads can receive
BR_NOOP or a thread-pool spawn request; an empty nonblocking read thereafter
returns EAGAIN while preserving the kernel's observable BR_NOOP buffer write.
Complete write/read records, consumed counts, guards and overlapping output
copy order are retained. A zero-length stream does not dereference its pointer.

There are no inferred service-manager entries, Parcel values, remote objects,
death notifications or Android system-service replies. Guest-owned synchronous
transactions are described below. Other commands and blocking reads that need
work stop explicitly. Captured ioctl
replies, read-only mappings and KGSL cannot be combined with this model. Command
streams are bounded to 4096 bytes and 4096 thread identities; partial command or
record faults, short read buffers and resumed read buffers are not admitted.
The state machine follows the pinned
[Binder driver contract](https://android.googlesource.com/kernel/common/+/cf21a9ac5ee4f57c6f45fe2b09d197bdc038f39d/drivers/android/binder.c),
without executing or copying its implementation or contacting a host driver.


### Guest-owned Binder services

SET_CONTEXT_MGR registers one context-manager open per declared device. A second
registration returns EBUSY. Separate opens have distinct Binder process owners,
even within this one emulated process. A read-only MAP_PRIVATE (optionally
MAP_NORESERVE), offset-zero arena of at most 4 MiB supplies receiver buffers;
only one mapping per open is admitted. The memory owner retains each buffer's
mapping identity, permissions and lifetime.
Lengths are page-rounded before the limit check, and receive pages share the
process memory budget. Allocation refusal leaves the open available for a
smaller retry. A larger mapping does not raise the 64 KiB limit on each payload
and offset table.
Requests beyond 4 MiB still stop: the driver's oversized-VMA truncation is not
modeled. The arena bound follows the pinned driver's `binder_mmap` limit above.

BC_TRANSACTION to handle zero copies a payload into the registered peer's
arena; an owned nonzero strong handle routes to its node's live owner. An entered/registered guest looper receives BR_TRANSACTION, runs
its native service code, and sends BC_REPLY. The original caller receives
BR_TRANSACTION_COMPLETE and BR_REPLY; no service result is supplied by the model.
Complete LP64 records retain code, sender identity, data size, aligned offsets,
consumed counts and untouched guards. BC_FREE_BUFFER releases only a delivered,
live buffer belonging to that open. Descriptors created with dup share ownership.
A missing context manager yields queued BR_DEAD_REPLY; unowned nonzero handles
and a reply without an incoming call yield BR_FAILED_REPLY. Target failure does
not dereference the payload. The kernel's full-record read reservation is retained,
even for four-byte completion/error work.
An unread return error stops command consumption; reading it allows that thread
to resume from write_consumed. Later commands are decoded only when reached,
so an error does not inspect an unknown or incomplete command after it.
A delivered transaction can request a new looper
through the leading BR_SPAWN_LOOPER word, under the same thread-pool rules as
control reads. Private thread work and process work retain kernel read eligibility.

Synchronous requests also accept TF_ACCEPT_FDS, as emitted by libbinder's
ordinary `transact`. The bit is delivered intact; descriptor translation is
still unsupported.
Replies accept TF_STATUS_CODE only with exactly four guest-provided bytes.
They preserve the flag and payload in the caller's read-only arena; no service
error or success value is invented. Other request/reply flags remain unsupported.
Rejected flags and status sizes retain command/target/code/flag/length diagnostics.
These unsupported flags and status sizes stop before allocating a reply buffer
or consuming the call.
These transport conventions follow the pinned
[Binder UAPI](https://android.googlesource.com/kernel/msm/+/a80b2e0035e75c0460b54e2217ca0104631d547d/include/uapi/linux/android/binder.h)
and [libbinder producer/consumer](https://android.googlesource.com/platform/frameworks/native/+/2d5e05b44248e445af986ca8410c6e98c6ba3533/libs/binder/IPCThreadState.cpp).

Strong LP64 flat Binder objects carry a validated, four-byte-aligned,
non-overlapping uint64 offset table. The shared kernel translates a sender's
pointer/cookie node into a receiver-local handle, reuses handles for the same
node, and restores the original pointer/cookie when an object returns to its
owner. Forwarding to a third open allocates that open's own handle. Payload and
offset bytes share the arena allocation; buffers retain their object references
and the transaction target independently of userspace references.
BC_INCREFS/ACQUIRE/RELEASE/DECREFS and the two *_DONE acknowledgments share node
and handle counters. BR_INCREFS/ACQUIRE/RELEASE/DECREFS preserve native ordering
and pointer/cookie fields. Exported-node notifications stay deferred until a
reply/error activates the synchronous caller's thread queue. Final open retirement
drops its handle references;
remote handles do not keep a dead owner alive. A call through such a handle
returns BR_DEAD_REPLY. Buffer views carry allocation identity, so address and
size reuse do not make a released view live again.

Malformed offset tables, misaligned/overlapping/out-of-bounds objects, unknown
tags, truncated objects, node-cookie mismatches and unowned strong object handles
produce BR_FAILED_REPLY. Copy-in bytes and a successfully translated prefix stay
observable in the arena, but cleanup releases the target followed by the prefix
references and retires the failed allocation. No failed buffer lease is delivered.
A failed request queues only the sender's return error. A failed reply retires
both sides of the original call, queues BR_TRANSACTION_COMPLETE in the server's
return-error slot and completes the caller with BR_TRANSACTION_COMPLETE followed
by BR_FAILED_REPLY. The caller's separate reply-error slot permits later writes;
an unread failure is not overwritten by a subsequent failed reply. The original
received request buffer still requires BC_FREE_BUFFER. Valid weak/FD/pointer
objects remain unsupported; malformed reference commands and over-release also
stop explicitly.
These semantics follow the Android P
[Binder driver](https://android.googlesource.com/kernel/common/+/01d7587c4f03aa97c8d618a3e067923f932e0c60/drivers/android/binder.c).

This subset admits at most one synchronous transaction per write batch, 64 live
buffers/transaction work items per queue, 64 KiB per payload/offset table and 4096 nodes and
handles per open. Node-control work is bounded by retained nodes. Weak-object,
FD/pointer/scatter-gather translation, one-way or nested calls, security policies,
death-recipient notification and blocking raw-driver scheduling remain explicit gaps. Partial
copies, exhausted model budgets and missing peer arenas stop rather than
fabricating data. Pending work prevents final owner retirement. Existing ioctl
observations cannot replace these stateful operations.

### Explicit A3xx memory commands

Add `"synchronous_a3xx":true` inside `kgsl_contexts` to opt into a deterministic
PM4 subset. It defaults to false and rejects non-Booleans. A memstore with
external producer updates cannot enable it: there must be one timestamp owner.
This setting selects a command format and a synchronous, coherent-RAM execution
contract; it does not identify a physical GPU or claim its timing/cache behavior.

MAP_USER_MEM admits page-aligned ordinary user-address memory, zero offset,
1–8 pages, and zero/GPUREADONLY flags. Up to 64 retained allocations share a
process GPU address space across independent opens; returned flags include
USERMEM_ADDR. Physical pins survive CPU unmapping and virtual-address reuse.
SHAREDMEM_FREE releases a registered base address.

Legacy RINGBUFFER_ISSUEIBCMDS admits one flags-zero root IB. A shared 4096-word
budget counts the root and every executed child, including repeated visits.
The type-3 CP_NOP payload is skipped according to its packet extent;
one-word CP_MEM_WRITE writes its actual operand into a registered writable GPU
allocation. CP_INDIRECT_BUFFER_PFE/PFD contain a 32-bit GPU address and a word
count. One indirect level is admitted; execution returns to the parent, and
repeated sibling calls execute again. Their prefetch distinction has no timing
effect under this synchronous coherent-RAM contract.

Admission checks the entire reachable program before any write. It retains
physical fetch ranges, then rejects every store overlapping any of them,
including a child discovered after the store or a different GPU address alias.
Recursive/overlapping active buffers, deeper nesting, empty/unmapped ranges,
predicate/reserved bits, unknown opcodes and truncated packets fail explicitly.
Refusals identify the buffer, word offset and packet header when available.
These are model limits, not claimed hardware limits. There is no shader,
register, draw, submission-list or type-7 implementation.

An admitted command buffer drains at the stopped CPU boundary. Its owned queued,
consumed and retired timestamps advance together; mapped memstore aliases observe
the same bytes. A wait already satisfied by that queue returns immediately;
other waits stop. Static ioctl observations cannot shadow owned command requests.
Public raw x64/AArch64 and Bionic fixtures validate the pinned-memory effects,
CPU-address reuse, guarded outputs, timestamp queries and refusal before effects.

These ABI/packet facts were independently implemented from the pinned
[KGSL UAPI](https://android.googlesource.com/kernel/msm/+/973f4134d9deb396415846f902848f0a32cb4cfa/include/uapi/linux/msm_kgsl.h),
[submission and memory owner](https://android.googlesource.com/kernel/msm/+/973f4134d9deb396415846f902848f0a32cb4cfa/drivers/gpu/msm/kgsl.c),
[ringbuffer packet producer](https://android.googlesource.com/kernel/msm/+/973f4134d9deb396415846f902848f0a32cb4cfa/drivers/gpu/msm/adreno_ringbuffer.c)
and [PM4 encoding](https://android.googlesource.com/kernel/msm/+/a65478df351382d255d293d745aee9bae3a3a741/drivers/gpu/msm/adreno_pm4types.h).
The indirect forms also follow the pinned
[legacy IB encoding](https://android.googlesource.com/kernel/msm/+/2db776e4f994f895799dd7cb47397db6bfe1e616/drivers/gpu/msm/adreno_pm4types.h).

### Owned GPU memory objects

`"memory_objects":true` in `kgsl_contexts` separately opts into LP64
GPUOBJ_ALLOC/INFO/FREE, GPUMEM_ALLOC_ID/GET_INFO/FREE_ID and SHAREDMEM_FREE.
It requires an O_RDWR device;
`synchronous_a3xx` is needed only to execute PM4. Static ioctl observations
cannot shadow these requests. Unknown/non-Boolean settings are rejected.

Objects own zero-initialized physical RAM in NeverD, charged to the workload
memory budget. GPUREADONLY, FORCE_32BIT, alignment hints and zero metadata
length are admitted. The 4 KiB page profile normalizes the alignment exponent
to 12–25 and returns the effective flags. Addresses honor that alignment even
when skipping explicit device mappings. Nonzero sizes through eight 4 KiB
pages are rounded up; a GPU-only
4 KiB guard contributes to the returned footprint. IDs and GPU addresses are
process-scoped across independent opens and descriptor aliases; allocation
choices are deterministic model policy, not captured hardware addresses.
At most 64 objects/imports share the address space. Query returns live size,
flags, GPU address, footprint and current CPU mapping address, preserving the
caller's padding. Unknown IDs return EINVAL; unsupported flags, metadata or
resource limits stop explicitly.

The legacy LP64 requests use 48-byte allocation, 72-byte query and 8-byte
release records (`0xc0300934`, `0xc0480936`, `0xc0080935`). They address the
same objects as GPUOBJ, including mappings and pending frees. Legacy allocation
implicitly sets FORCE_32BIT; its 32-bit flags field omits that upper bit, while
GPUOBJ_INFO reports it. All modeled addresses currently fit below 4 GiB.
GPUMEM_GET_INFO prioritizes a nonzero ID, otherwise accepts an address within
the object's data and returns the base. It does not search by address after an
invalid ID, or admit the guard as data. Reserved fields survive copyout,
including FREE_ID's padding. Complete writable output is required before any
state mutation; unsupported copyout stops without partial effects. Querying a
legacy user-address import through GET_INFO stops explicitly because its
kernel allocation ID is not modeled. Other legacy structure layouts are not
inferred from matching request numbers.

MAP_SHARED accepts one complete data mapping by `id * 4096` or GPU base,
with PROT_NONE, PROT_READ or PROT_READ|PROT_WRITE. CPU and admitted PM4 writes
use the same retained bytes. GPUREADONLY restricts GPU writes, not CPU writes.
A second active mapping returns EBUSY; an incorrect size returns ERANGE.
Interior GPU-address offsets, mapping the guard, executable mappings, VMA
splitting and advice stop rather
than inventing fault delivery or driver VMA callbacks. Whole-range mprotect
and munmap are supported. Dynamic proc maps labels use the same mapping owner.

An immediate free while mapped marks the object pending; query and existing
mapping access remain valid, and a second free returns EBUSY. Final unmap
retires a pending object; unmapping a live object preserves its contents for
remapping. VMAs retain their open and process owner after FD closure. Freed
storage is zeroed before reuse. Asynchronous timestamp/fence frees, imports
through GPUOBJ_IMPORT, cache maintenance, secure/SVM allocations and shaders
remain unsupported. The resource model is not a physical GPU/cache model.

The independently written implementation uses the pinned
[GPU object UAPI](https://android.googlesource.com/kernel/msm/+/25423157b70a8f2250860e72e63b29e28872220c/include/uapi/linux/msm_kgsl.h)
and [allocation, query and VMA lifetime owner](https://android.googlesource.com/kernel/msm/+/2db776e4f994f895799dd7cb47397db6bfe1e616/drivers/gpu/msm/kgsl.c).
The LP64 legacy layouts use the same revision's
[UAPI](https://android.googlesource.com/kernel/msm/+/2db776e4f994f895799dd7cb47397db6bfe1e616/include/uapi/linux/msm_kgsl.h),
with the page-alignment floor defined by its
[shared-memory allocator](https://android.googlesource.com/kernel/msm/+/2db776e4f994f895799dd7cb47397db6bfe1e616/drivers/gpu/msm/kgsl_sharedmem.c).

## Optional ioctl memory evidence

`linux_files.ioctl_trace` observes the shared raw/Bionic ioctl boundary. It
records narrowed FD/request, argument, PC, thread, raw Linux result bits and,
for Android wrappers, the guest return address (a tail call retains its
caller's return address). It neither interprets an unknown ioctl nor supplies
a reply. A null result is an unfinished/refused boundary, not success.

```json
"ioctl_trace": {
  "event_limit": 1024,
  "byte_limit": 65536,
  "requests": [{
    "request": 2701131777,
    "memory": [
      {"offset": 0, "size": 24},
      {"pointer_offset": 8, "offset": 0, "size": 16}
    ]
  }]
}
```

Each range is an explicit observation extent. `pointer_offset` reads one
little-endian LP64 pointer at argument+offset at entry; both snapshots read
the resulting address plus `offset`, even if the service changes the pointer
cell. Sizes are not inferred from ioctl number bits, pointer values or guest
allocation extents. GPU virtual addresses are not CPU pointers. This is not
a recursive Binder parcel decoder or a GPU/shader program extractor.

The optional top-level `ioctl_trace` report includes `memory` rows with
`address`, `size`, `before_hex`, `after_hex`, `before_status`, and `after_status`.
Only `complete` means the entire declared range was captured. `unreadable`,
`address_overflow`, `backend_error`, `byte_limit`, and
`unresolved_entry_address` distinguish absent evidence from an empty payload.
Unknown request numbers still have a metadata row with an empty range list.
No trace fields are emitted when the option is absent.

Limits are 4096 events, 1 MiB attempted read bytes (including pointer cells and
both phases), 64 unique requests, 16 ranges per request and 4096 bytes per
range. Whole ranges are retained or omitted; no zero padding or partial-read
success is reported. `ioctl_trace_truncated` marks event/byte exhaustion.
Capture failure/exhaustion does not change guest results, device transaction
consumption, writes or virtual time. Only ordinary guest RAM is observed;
these snapshots do not authorize host/device reads or provide general memory
provenance. Bounded observations are evidence for the executed input, not an
all-path proof.
