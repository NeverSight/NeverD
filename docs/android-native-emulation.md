# Android native emulation

`android-aarch64-api28-v1` runs one native function in an Android 9 / API 28
AArch64 shared library using NeverD's CPU, address space, AAPCS64 call frames,
execution sessions, and Linux syscall models. This is a bounded native analysis
environment, with explicit inputs and observable failures. It does not boot an
Android system or supply a Java bytecode runtime, system-service peers, signal
delivery, a general filesystem, or a network. JNI and Binder have explicit
bounded models described below. It never calls host functions to satisfy a guest import.

Build with `NEVERD_ENABLE_CPU_EMULATION=ON`. The host transport is selected
independently of the guest profile; `unicorn` works without an Android device.

```sh
neverd emulate example.so --profile=android-aarch64-api28-v1 --options='{
  "backend": "unicorn",
  "instruction_limit": 100000,
  "event_limit": 10000,
  "android": {
    "entry_symbol": "inspect_buffer",
    "arguments": ["0x20000000", 4],
    "properties": {"test.device": "sample"},
    "memory": [{"address": "0x20000000", "size": 4096,
                "bytes_hex": "01020304"}],
    "read_memory": [{"address": "0x20000000", "size": 16}],
    "trace_limit": 4096
  }
}'
```

The same request works with `neverd_emulate_process_json` and the Python
`emulate_process` wrapper. C++ callers use `ProcessOptions::Android` and
`emulateProcess(..., ProcessProfile::AndroidNativeAArch64, ...)`.

## Native input contract

Choose exactly one `entry_symbol` (a defined dynamic function symbol) or
`entry_address` (a **link-time virtual address**, before `load_bias`). The
load bias defaults to `0x40000000`; it must satisfy the ELF load alignment.
Addresses and scalar arguments accept unsigned JSON integers or decimal/`0x`
strings. Results use hexadecimal strings without a prefix, like Linux reports.

Arguments are non-variadic 64-bit scalar AAPCS64 values. Values beyond x0–x7
are placed on the guest stack by `IntegerABI`. Floating-point arguments,
aggregates, and variadic classification are not inferred. Callers explicitly
supply extension of narrow values. Top-level Linux `arguments` and
`environment` are invalid for this profile.

Memory regions are explicit page-aligned address/size pairs. Initial
`bytes_hex` are followed by zeros. They are readable/writable; `executable`
defaults to false. Overlaps and reserved runtime addresses are rejected.
`read_memory` snapshots must fit the output budget and, by default, initially
readable memory. Each read can explicitly set `require_mapped_at_entry: false`
to observe memory allocated during the workload. The setting only defers the
mapping check; it does not create memory, retain freed mappings, or change guest
allocation and execution. Size, overflow and output-budget checks still apply
before execution, and the complete range must be readable at the final stop.
A snapshot that is unmapped or unreadable then fails explicitly. Terminal CPU
or model faults do not trigger snapshots. No host addresses are exposed as guest
memory.
If a final read fails after earlier reads completed, those earlier snapshots
remain in the report alongside the failure.

```json
{"address":"0x24000000","size":64,"require_mapped_at_entry":false}
```

For input larger than the 64 KiB options JSON limit, supply `path` instead of
`bytes_hex` in a `memory` region. C++ callers set `NativeMemoryRegion::File`.
The path names an explicit regular file on the caller's host; relative paths
use the caller's working directory and JSON paths are UTF-8. The complete file
must fit the region. Preparation copies its bytes into already budgeted guest
memory and leaves the remaining bytes zero. Guest writes never modify the
file, and another workload reads its current contents again.

```json
{"address":"0x20000000","size":131072,"path":"input.bin"}
```

`path` and `bytes_hex` are mutually exclusive, including empty `bytes_hex`.
Missing files, non-regular files, oversized files and detected size changes
during copying fail before CPU execution. Keep the input stable while it is
read; copying is not an atomic filesystem snapshot. The region shares the
image/stack memory limit, and bounded reads check the preparation deadline.
Blocking host filesystem I/O itself has no hard deadline guarantee. This
explicit preparation input does not expose host files through guest APIs.

By default, DT_INIT and DT_INIT_ARRAY constructors execute in order before the
selected function, with an explicit empty argc/argv/envp. All calls share the
same instruction, event and deadline budget. `initialize: false` explicitly
requests analysis of an **uninitialized** library; the report records this
choice. A normal function return has `stop_reason: "returned"`, a
`return_value`, and CLI status 0 regardless of its integer return value.
In the default single-thread mode, raw Linux `exit`/`exit_group` retain
process-exit semantics. With guest threads enabled, `exit` ends the current
thread and `exit_group` ends the workload. Destructors are not run when the
observation ends at the selected function's return. The libc
`exit` import and automatic FINI-array execution remain unsupported.

### Selected execution observations

`android.observe_execution` records state immediately before explicit absolute
guest PCs, without patching code or supplying call results:

```json
{"pcs":["0x40001000","0x40001004"],"limit":32,
 "read_memory":[{"address":"0x20000000","size":64}]}
```

The object requires 1–64 unique, four-byte-aligned `pcs` and a positive `limit`.
Each retained record contains `pc`, `thread_id`, the admitted instruction count
before that point, `registers` in x0–x30 order, `sp`, `nzcv`, and the requested
memory bytes. These are scalar observations, not complete CPU/OS checkpoints.
Addresses and register bits use hexadecimal strings in reports. The same
memory ranges are copied at every point. `require_mapped_at_entry: false`
allows a range created during execution; it must be readable at every captured
point. An unreadable range fails before executing that watched instruction.

The complete retained capacity shares `output_limit` with ordinary snapshots,
thread records and PC tracing. `execution_observations_truncated` becomes true
only when another point matches after the retained limit; execution continues
with observation disabled. Unreached addresses produce no records. Observations
consume no guest instruction or service event and preserve cooperative quantum
boundaries. Their host work still counts against the existing wall-time limit.
They neither prove coverage beyond supplied inputs nor implement an unknown
provider's memory, IPC or external effects.

## Linking and Android contracts

The loader reads original PT_LOAD/PT_DYNAMIC file bytes, independently of
section tables and analysis patches. It decodes ordinary RELA, Android APS2
RELA and RELR records, including dynamic symbols bounded by SysV or GNU hash
tables. The Android model handles AArch64 RELATIVE, ABS64, GLOB_DAT and JUMP_SLOT,
then applies segment permissions and GNU RELRO. Unsupported relocations,
IFUNC/TLS symbols, ELF TLS templates, text relocations, interpreters, malformed
tables and unmodeled data imports are errors. Sectionless shared libraries are
supported.

Dependencies are **not loaded transitively**. Undefined function imports are
bound to identified trap slots. Calling an unmodeled import produces
`unsupported_service` with its name and arguments; it never returns invented
success. This is selective native analysis, not a complete dynamic linker:
all such function imports have non-null trap addresses, including weak imports.
Code that tests availability of an optional provider requires a fuller linker
model. Symbol versions are not used to select alternate implementations.

The supported Bionic subset is:

- `memcpy`, `memmove`, `memset`, `memcmp`, `strlen`, `strnlen`, `strcmp`, `strncmp`.
- `__FD_SET_chk`, `__FD_CLR_chk`, `__FD_ISSET_chk`, using the Bionic LP64
  1024-bit descriptor bitmap. The signed low 32-bit descriptor must be 0..1023,
  then the full-width object extent must be at least 128 bytes. Invalid values
  stop with a non-returning FORTIFY `runtime_failure` before touching the bitmap;
  signal delivery is not modeled. Valid calls access only the selected eight-byte
  word, preserve other bits and errno, and do not require an open descriptor.
  Queries return 0 or 1 and accept read-only memory; mutations require read/write
  access. Guest-pointer and wrapping-address failures remain explicit. These
  rules follow the pinned API 28 [checks](https://github.com/aosp-mirror/platform_bionic/blob/android-9.0.0_r1/libc/private/bionic_fortify.h)
  and [bitmap ABI](https://github.com/aosp-mirror/platform_bionic/blob/android-9.0.0_r1/libc/include/sys/select.h).
  Raw `pselect6` and its variadic `syscall` route use the shared Linux readiness
  subset below; Bionic `select`/`pselect` wrappers are not yet modeled.
- LP64 raw `pselect6`, for immediate completion within the initial 64-descriptor
  word. It captures all input sets before ordered output, counts ready bits
  across sets, checks unopened descriptors, and preserves the raw errno/libc
  errno boundary. Regular files and declared live pidfds share the existing
  descriptor owner. Connected offline sockets can be readable from supplied
  data, queued empty UDP datagrams or explicit TCP EOF; remaining declared sends
  can establish immediate write readiness. Queries never consume a packet,
  completion error or stream byte. Missing input, gated input, exceptional data
  and exhausted send contracts retain an unsupported observation boundary.
  Zero timeouts do not change time. A nonzero timeout requires the explicit
  virtual idle-time policy and can complete only when immediately ready; its
  remaining-time copy follows bitmap output, including aliases and ordinary
  errors. Entirely read-only timeout output is ignored as in Linux. Partial
  output faults, larger descriptor-table history, temporary signal masks and
  actual blocking waits remain explicit unsupported cases. No host polling is
  used. Admission and copy ordering follow [Linux v4.14](https://github.com/torvalds/linux/blob/v4.14/fs/select.c).
- `strchr`, `strrchr`, `__strchr_chk`, `__strrchr_chk`, with byte-width character
  conversion, NUL matches, full-width object extents and unchanged errno. The
  first search can return before reaching a terminator; reverse search must
  reach NUL. Fortified calls check the remaining extent before each read.
  Exhaustion produces a non-returning `runtime_failure` with a Fortify diagnostic,
  consistent with the model's abort boundary; signal delivery is not modeled.
  Invalid pointers and memory/deadline limits remain distinct failures. Only
  visited guest bytes are read, including a match at a readable page's last byte.
  These rules follow the pinned [API 28 implementation](https://github.com/aosp-mirror/platform_bionic/blob/android-9.0.0_r1/libc/bionic/fortify.cpp).
- `strtok_r`, scanning guest byte strings and updating the caller's eight-byte,
  aligned save pointer. Delimiters may change between calls; separate contexts
  remain independent. API 28 clears the saved pointer at the final token or an
  empty remainder. A null saved cursor returns NULL without reading delimiters
  or writing memory. Only a terminating delimiter is overwritten; a final
  token with no separator can reside in read-only memory. Both write spans are
  checked before either effect, and errno is preserved. Invalid pointers and
  exhausted scan/deadline bounds stop explicitly. No host tokenizer or hidden
  process cursor supplies state; `strtok` remains unsupported.
- `snprintf`, `vsnprintf`, `sprintf`, `vsprintf`, for the bounded integer and
  byte-string formatting subset described below.
- `sscanf`, `vsscanf`, for bounded integer scanning as described below.
- `sigaction`, `sigaction64`, sharing process-wide dispositions with raw
  `rt_sigaction` and the variadic `syscall` wrapper. Initial actions come from
  the explicit [`linux_signals` input](process-emulation.md). API 28 LP64 has
  four-byte flags, four untouched padding bytes, then eight-byte handler, mask
  and restorer fields. New masks omit Bionic's reserved signals 32–35; kernel
  normalization separately removes SIGKILL/SIGSTOP. Input is captured before
  output, including overlapping objects. Successful calls preserve errno.
  Output fields are written in order, preserving padding and completed stores
  if a later user-space access fails. A failed kernel operation with a non-null
  old-action pointer stops explicitly: API 28 copies an uninitialized temporary
  on this path, so the model cannot invent its bytes. See the pinned
  [wrapper](https://android.googlesource.com/platform/bionic/+/android-9.0.0_r61/libc/bionic/sigaction.cpp)
  and [reserved mask](https://android.googlesource.com/platform/bionic/+/android-9.0.0_r61/libc/private/sigrtmin.h).
  Registering an action does not enable signal delivery or execute its handler.
- `mincore`, for the shared Linux validation, empty-range and unmapped-first-page
  subset. Direct imports, variadic `syscall` and raw SVC share the same error
  priority and leave the output vector untouched. Only Bionic converts negative
  kernel errors to `-1` and errno; success preserves errno. Mapped-page residency
  remains an explicit unsupported observation; guest backing memory does not
  justify inventing residency bits. See the [Linux contract](process-emulation.md).
- `malloc`, `calloc`, `realloc`, `free`, with live allocation tracking and
  bounded anonymous guest memory. Zero-size allocations may return a unique
  pointer; allocation failure returns NULL and sets ENOMEM.
  `android.malloc_fill_byte` (0..255, default 0) selects concrete contents for
  the requested bytes of `malloc` and the growth tail of `realloc`. Use
  different values to check whether an analysis depends on initial heap bytes.
  `calloc`, internal model allocations and anonymous `mmap` remain zeroed;
  page padding is excluded. This input neither models a real allocator history
  nor establishes the validity of uninitialized reads.
- `__errno`, `getpid`, `gettid`, `getuid`, `geteuid`, `getgid`, `getegid`.
  Identity queries share the Linux
  process model: PID/TID and real/effective UID/GID are 1000, including raw
  `svc #0` and calls resolved through the explicit dynamic catalogue. They
  preserve errno and never query host credentials. Credential changes such
  as `setuid` remain unsupported.
- `android_get_device_api_level`, reading the explicitly supplied
  `android.properties["ro.build.version.sdk"]`. Missing, empty, nonnumeric or
  nonpositive values return signed `int` -1; positive decimal prefixes use the
  same bounded parser as `atoi`, including whitespace and a leading sign.
  Out-of-range conversion stops explicitly. The query preserves errno and
  observes immutable guest input, never host properties. Direct imports and
  declared dynamic providers share this behavior. The reported device version
  does not change the profile's pinned API 28 Bionic structures, supported
  services or selected kernel contract. See the
  [Bionic query](https://android.googlesource.com/platform/bionic/+/android-13.0.0_r1/libc/include/bits/get_device_api_level_inlines.h).
- `getpagesize`, returning the Android profile's 4096-byte guest page size
  without changing errno. It uses the same layout as guest mappings,
  independently of the host page size. Direct imports and explicit dynamic
  catalogue entries share this behavior; provider lifetime rules still apply.
  See the pinned Android 9
  [Bionic implementation](https://android.googlesource.com/platform/bionic/+/android-9.0.0_r61/libc/bionic/getpagesize.cpp).
- `ioctl`, for [explicit Linux device transactions](linux-device-contracts.md).
  Imported, dynamic and raw calls use the shared descriptor owner. Inputs must
  declare the ordered request, scalar/byte expectations, result and outputs;
  missing transactions stop. This does not provide a driver or device mapping.
- `localtime`, `localtime_r`, `gmtime` and `gmtime_r`, converting signed 64-bit
  timestamps to API 28's 56-byte LP64 `struct tm` for civil years 1..9999.
  Local conversion requires `android.fixed_time_zone`, for example
  `{"name":"FIX+08","offset_seconds":28800}`. This is a complete fixed zone
  with no transitions or leap-second corrections: the offset is seconds east
  of UTC, within ±86400, and the abbreviation is 1..32 printable non-space
  ASCII bytes. It is not a tzdb name or a host-zone lookup. GMT conversion uses
  zero offset and the `GMT` abbreviation without requiring that input.
  The non-reentrant APIs share one process-owned guest object across threads;
  `_r` writes caller storage, preserves ABI padding, and leaves that static
  object alone. Zone strings have stable, read-only guest storage. Supported
  conversions preserve errno and read the timestamp before writing output,
  including overlapping input/output. Missing local-zone input and dates
  outside the admitted range stop explicitly; wider-year overflow effects,
  tzdata transitions, leap seconds, `tzset` and calendar formatting remain
  unsupported. No host clock, locale or environment is consulted. The ABI and
  storage contract follow [API 28 Bionic's calendar implementation](https://raw.githubusercontent.com/aosp-mirror/platform_bionic/android-9.0.0_r1/libc/tzcode/localtime.c).
- `getauxval`, reading the complete immutable `android.auxiliary_vector`
  input. Each entry has unsigned 64-bit `type` and `value` fields; numeric
  strings, including hexadecimal values, are accepted. At most 128 unique,
  nonzero types are admitted; the `AT_NULL` terminator is implicit. For example,
  `[{"type":16,"value":0},{"type":26,"value":0}]` explicitly advertises zero
  HWCAP/HWCAP2 for this workload, not the host's capabilities. Omitting the
  vector stops the query as unknown; `[]` is an explicitly empty vector.
  A missing key returns zero and sets the calling thread's errno to `ENOENT`;
  a present zero leaves errno unchanged, matching
  [API 28 Bionic](https://raw.githubusercontent.com/aosp-mirror/platform_bionic/android-9.0.0_r1/libc/bionic/getauxval.cpp).
  Constructors, finalizers and named dynamic calls use the same input.
  Pointer-valued entries are guest values; callers must provide their backing
  memory separately. No host auxv is consulted, no Linux process startup stack
  is changed, and advertised features never relax checked CPU admission.
- `sysconf`, for Android's `_SC_PAGESIZE` (`0x27`) and `_SC_PAGE_SIZE` (`0x28`)
  selectors. Both return the same guest page size as `getpagesize`, mappings
  and allocation rounding, without changing errno. The selector uses its
  declared 32-bit `int` width. Other selectors stop as unsupported, including
  invalid names; the model does not substitute host limits or invent errno
  responses. Direct and dynamic calls share provider lifetime checks. Android
  9 defines [distinct selector numbers](https://android.googlesource.com/platform/bionic/+/android-9.0.0_r61/libc/include/bits/sysconf.h)
  and [one page-size result](https://android.googlesource.com/platform/bionic/+/android-9.0.0_r61/libc/bionic/sysconf.cpp).
- `__system_property_get`, `__system_property_find`,
  `__system_property_find_nth`, `__system_property_read`,
  `__system_property_serial`, `__system_property_read_callback` and
  `__system_property_foreach` share the explicit immutable property dictionary.
  Missing get values return zero and write NUL; missing find/nth entries return
  NULL. Empty values retain a valid handle. Values fit 91 bytes plus NUL.
  Legacy read accepts a NULL name and truncates names to 31 bytes plus NUL;
  the read callback receives the complete name. Value output precedes name
  output, including overlapping buffers.
  [The API 28 implementation](https://github.com/aosp-mirror/platform_bionic/blob/android-9.0.0_r61/libc/system_properties/system_properties.cpp)
  defines these read and callback contracts.
  Stable, read-only guest records use the
  [short `prop_info` layout](https://github.com/aosp-mirror/platform_bionic/blob/android-9.0.0_r61/libc/system_properties/include/system_properties/prop_info.h).
  The snapshot starts at generation zero: serial is value length shifted by 24.
  This is a newly constructed model state, not a captured device serial.
  Enumeration is deterministic name order; Android's property-area traversal
  order is not inferred. The reserved property arena starts at `0x7010000000`,
  is bounded by 256 MiB and the workload memory limit, and charges mapped bytes
  to that same memory budget. Invalid handles, missing mappings and changed
  record bytes fail clearly instead of reading cached original values.
  Both callbacks execute guest code with full-width cookies on the suspended
  stack. Nested enumeration, once callbacks and cooperative threads retain
  independent continuations and share execution budgets. Callback strings are
  const snapshot storage. No host property or callback is used. Property
  mutation/waits, long values, SELinux property contexts and property-service
  IPC remain unsupported.
- `time`, `gettimeofday`, and `clock_gettime`, using the shared explicit
  [`linux_time` inputs](process-emulation.md#explicit-guest-clocks). Imports,
  named dynamic calls and raw ARM64 services read the same fixed observations.
  Successful calls preserve errno; kernel errors become `-1` and errno in
  Bionic alone. API 28's `time` fallback stores through the caller's `time_t*`
  in user space, so a bad destination is a runtime failure rather than a
  returning kernel `EFAULT`. Signed timestamps retain all 64 bits, including
  negative values; they are not mistaken for raw errno bits. No guest vDSO or
  host clock is invoked. See the pinned Android 9
  [Bionic wrappers](https://github.com/aosp-mirror/platform_bionic/blob/196632fb3c59ebbf1184d791a3e7124dd0c3f22b/libc/bionic/vdso.cpp).
- `pthread_once`, executing the guest initializer on the current guest stack.
  An API 28 four-byte control transitions from 0 to 1 before the callback and
  to 2 only after normal return. A completed control returns zero without
  invoking its initializer. Nested initialization of different controls and
  modeled imports inside callbacks share the caller's execution budget.
  With cooperative threads enabled, a caller waits when another live guest
  thread owns the initializer. All waiters become runnable only after that
  callback returns and its completion word is written. Resumption restores
  the original CPU/TLS/stack and rechecks the control before completing the
  original import. Recursive initialization and running controls without a
  known owner stop as unsupported. Resetting an active control also stops. Callback
  faults and exhausted budgets leave the call result incomplete. Unwinding,
  cancellation and fork recovery are unsupported.
- `__cxa_atexit` stores the guest callback, opaque argument and DSO token;
  registration does not execute the callback or dereference those values.
  `__cxa_finalize(dso)` invokes matching registrations in reverse order;
  a NULL DSO selects all. Each registration is retired before invocation,
  including during recursive finalization. Registrations made by a callback
  are considered before older remaining entries. NULL callbacks are skipped.
  Guest callbacks share the suspended caller's CPU, stack and budgets, including
  nested `pthread_once`. A failed callback leaves the finalize event incomplete.
  The registry has a model limit of 4096 pending slots, including NULL entries;
  exceeding it stops explicitly. Finalizing all entries releases unused slots.
  Callback alignment and execute permission are checked at invocation. A DSO
  token is an opaque identity, independent of the catalogue's `dlopen` handles.
  Catalogue `dlclose` does not unload guest code or automatically finalize it.
  FILE cleanup, atfork handlers, exception unwinding and concurrent destruction
  remain outside this model. No host callback is invoked.
- `dlopen`, `dlsym`, `dlclose`, `dlerror`, using an explicit local catalogue
  described below. Function availability and implementation are separate:
  an available symbol whose call is unmodeled still stops explicitly.
- `write`, `writev`, `mmap`/`mmap64`, `mprotect`, `munmap`, `madvise`, delegated to the shared Linux
  service implementation. Bionic wrappers translate negative kernel error
  values to -1 and thread-local errno; raw `svc #0` preserves negative errno
  bits and does not update TLS errno.
  `madvise` supports the shared [KSM eligibility contract](process-emulation.md#linux-elf64-profile)
  for `MADV_MERGEABLE` and `MADV_UNMERGEABLE`; other advice stops explicitly.
- `open`/`open64`, `openat`/`openat64`, `read`, `close`, `lseek`/`lseek64`
  and `fstat`/`fstat64`, `fstatat`/`fstatat64`
  use the explicit [`linux_files` catalogue](process-emulation.md#explicit-memory-files).
  Raw traps, imported calls and guest threads share descriptors and cursors;
  only Bionic converts negative errors into `-1` and TLS errno. No host files or
  implicit proc data are visible. Explicit write grants allow bounded writes;
  known directory opens, canonical relative `openat` and opt-in live FD links
  follow the same Linux catalogue contract. Other path forms and flags stop explicitly.
  File status requires complete explicit `metadata`; it writes the 128-byte
  AArch64 layout and preserves the open cursor. Missing observations and mixed
  writable/inaccessible outputs stop without invented values or partial bytes.
- `syscall(number, ...)` uses the same Linux service table and effects. The
  AArch64 wrapper takes the number from x0 and six arguments from x1–x6;
  x7 is unused. It preserves full-width results and applies Bionic's -1/errno
  conversion only to kernel errors. Successful calls preserve errno, and
  exit services terminate the workload without returning to the caller.
  Unknown numbers and unsupported memory or networking services stop explicitly.
  Static and dynamic calls retain their native import event; only an actual
  `svc #0` produces a raw service event.
- `nanosleep` shares the explicit
  [idle clock advancement policy](process-emulation.md#explicit-guest-clocks)
  with raw and variadic calls. It retains the thread's original continuation
  while other guest threads run, and preserves remaining-time storage and errno
  on success. The policy is disabled by default; signal interruption and
  absolute sleeps remain unmodeled.
- `usleep` uses the same policy and thread wait ownership. The argument is
  unsigned 32-bit microseconds, including values of one second or more;
  successful calls preserve errno. Direct imports and dynamic lookups share
  provider lifetime checks. The scalar conversion follows Android 9's
  [Bionic contract](https://github.com/aosp-mirror/platform_bionic/blob/196632fb3c59ebbf1184d791a3e7124dd0c3f22b/libc/upstream-freebsd/lib/libc/gen/usleep.c)
  and [32-bit type](https://github.com/aosp-mirror/platform_bionic/blob/196632fb3c59ebbf1184d791a3e7124dd0c3f22b/libc/include/sys/types.h).

TLS uses the API 28 Bionic layout: TPIDR_EL0 points to a guest TLS block,
`__errno` addresses slot 2, and the stack guard occupies slot 5. The guard is a
deterministic analysis value, not host randomness. `__stack_chk_guard` aliases
that value; `__stack_chk_fail` and `abort` fail explicitly. The known `__sF`
stdio data symbol has an opaque, inaccessible guest address: field reads fail
instead of receiving fabricated FILE contents. FILE I/O operations remain
unmodeled; string formatting does not expose a FILE object.

These ABI choices are based on the pinned AOSP Android 9 definitions:
[Bionic TLS slots](https://android.googlesource.com/platform/bionic/+/refs/tags/android-9.0.0_r1/libc/private/bionic_tls.h),
[`__errno`](https://android.googlesource.com/platform/bionic/+/refs/tags/android-9.0.0_r1/libc/bionic/__errno.cpp),
[system property declarations](https://android.googlesource.com/platform/bionic/+/refs/tags/android-9.0.0_r1/libc/include/sys/system_properties.h),
[`pthread_once`](https://github.com/aosp-mirror/platform_bionic/blob/android-9.0.0_r1/libc/bionic/pthread_once.cpp),
[`__cxa_atexit` and `__cxa_finalize`](https://github.com/aosp-mirror/platform_bionic/blob/196632fb3c59ebbf1184d791a3e7124dd0c3f22b/libc/stdlib/atexit.c),
and the [AArch64 syscall wrapper](https://github.com/aosp-mirror/platform_bionic/blob/android-9.0.0_r1/libc/arch-arm64/bionic/syscall.S).
The model is independently implemented; these sources specify the ABI.

### String formatting

The four string formatting imports share one Android implementation. It reads
ordinary GP arguments from x0–x7 and then the guest stack. The `v` forms consume
the caller's actual 32-byte AAPCS64 `va_list`, including its GP save area and
overflow stack; they never use the host's variadic ABI. The caller's list is
not modified. Narrow integer arguments undergo the guest's integer promotions;
`long`, `long long`, `intmax_t`, `size_t`, pointers and `ptrdiff_t` have LP64 widths.

Supported conversions are `d i u o x X p s c %`, with `hh h l ll j z t` integer
lengths, signs, alternate form, left/zero padding, and fixed or `*` width and
precision. Android's `q D O U` integer aliases and ignored grouping flag are
accepted. `%s` operates on bytes; precision bounds its memory reads. NULL strings
use `(null)`. Pointer output includes `0x`, including NULL; API 28's zero-value
`%#.0o` emits no digits. Embedded NUL from `%c` counts toward the result.

Successful calls preserve errno, write a terminating NUL when capacity is
nonzero, and return the complete required length excluding that terminator.
`snprintf(NULL, 0, ...)` still validates and counts its input. Truncation needs
only the actual destination prefix to be writable. Padding is counted without
materializing discarded bytes; input scans and retained output share the
process's existing memory and deadline limits.

Floating-point, wide text, positional arguments, `%n`, unknown conversions,
`snprintf` capacities above INT_MAX, and lengths exceeding INT_MAX stop as
`unsupported_service` before publishing output. This boundary does not simulate
Bionic's version-specific overflow or fortify error side effects. Invalid guest
memory or malformed GP `va_list` state fails explicitly. Guest strings are never
passed to host printf functions. Resolving these names with `dlsym` uses the
same implementation and provider lifetime checks as direct imports.

The independently authored implementation and compiled O0/O2 callers follow
[AAPCS64's variadic layout](https://github.com/ARM-software/abi-aa/blob/2025Q1/aapcs64/aapcs64.rst#the-va_list-type),
Android 9's [formatting entrypoints](https://github.com/aosp-mirror/platform_bionic/blob/196632fb3c59ebbf1184d791a3e7124dd0c3f22b/libc/stdio/stdio.cpp)
and [conversion behavior](https://github.com/aosp-mirror/platform_bionic/blob/196632fb3c59ebbf1184d791a3e7124dd0c3f22b/libc/stdio/vfprintf.cpp).

### Byte streams

`fopen`/`fopen64` and `fdopen` admit byte read, write and update modes.
Writing regular files requires an explicit `linux_files.files[].write_limit`. `fread`, `fgets`,
`fgetc`/`getc`, their unlocked forms, `fileno`, `feof`, `ferror`, `clearerr`,
`fclose`, `fseek`/`fseeko`/`fseeko64`, `ftell`/`ftello`/`ftello64`,
`fgetpos`/`fgetpos64`, `fsetpos`/`fsetpos64`, `rewind`, `setvbuf`, `setbuf`,
`setbuffer` and `setlinebuf` share one workload's stream state. Direct imports
and explicit dynamic providers use the same owner. FILE tokens are opaque:
field access, forged tokens, retired tokens and detected changes to their
mapping fail; a returned token is not a materialized Bionic FILE structure.

Supply regular bytes and metadata in `linux_files`. Default buffer capacity
comes from that live descriptor's explicit `metadata.block_size`; zero selects
API 28's 1024-byte BUFSIZ. Missing metadata remains unknown, not a fabricated
failed fstat or default block size. `setvbuf(..., _IONBF, ...)` needs no metadata
and uses a one-byte buffer. Model-owned allocations count against the guest
memory limit; buffer allocation failure falls back to one-byte reads and
retains ENOMEM. Caller-supplied buffers are actual guest memory. Rebuffering
discards unread bytes at the underlying FD position without rewinding it.

Refills advance the kernel's shared open-description cursor; `ftell` subtracts
unread bytes. A read larger than the stream buffer bypasses that buffer.
`fread` returns complete element counts while retaining any final partial
object's bytes. Exact-length reads do not infer EOF. In this API 28 contract,
EOF remains set until cleared, but later reads still retry the descriptor.
`fseek` clears EOF on success and preserves the error indicator; `rewind`
clears both. `fflush`/`fflush_unlocked` on a read-only FILE return EOF/EBADF
without discarding buffered input. A null argument flushes all output streams
and retains a failure while continuing with the remaining streams.

`fdopen` adopts the existing FD without duplicating or resetting it. `dup`
aliases and raw syscalls see the same kernel cursor. Closing the FD does not
erase already buffered bytes; a later refill reports EBADF. `fclose` retires the
stream even when closing its FD fails. Thread calls share FILE state, complete
as indivisible model events, and retain separate TLS errno. Explicit
`flockfile` locking and concurrent unlocked operations are not modeled.

Transfers above the API 28 callback's signed-int extent, fdopen's close-on-exec
or append-flag mutation, `ungetc`, wide streams, `freopen`, custom callbacks
and standard FILE objects remain unsupported. Buffered copies validate consumed guest memory;
invalid pointers stop the workload. Direct reads preserve the kernel model's
partial-transfer/EFAULT behavior. This is a bounded byte-stream model, not
an implementation of the entire Bionic stdio ABI.

These independently authored models and synthetic callers follow the pinned
Android 9 [stdio entrypoints](https://github.com/aosp-mirror/platform_bionic/blob/android-9.0.0_r61/libc/stdio/stdio.cpp),
[refill contract](https://github.com/aosp-mirror/platform_bionic/blob/android-9.0.0_r61/libc/stdio/refill.c),
[buffer selection](https://github.com/aosp-mirror/platform_bionic/blob/android-9.0.0_r61/libc/upstream-openbsd/lib/libc/stdio/makebuf.c),
[mode parsing](https://github.com/aosp-mirror/platform_bionic/blob/android-9.0.0_r61/libc/upstream-openbsd/lib/libc/stdio/flags.c)
and [setvbuf behavior](https://github.com/aosp-mirror/platform_bionic/blob/android-9.0.0_r61/libc/upstream-openbsd/lib/libc/stdio/setvbuf.c).
Host stdio and host filesystem calls do not satisfy guest reads.

`fwrite`, `fputs` and their unlocked aliases share these FILE objects. Full,
line and unbuffered modes retain actual guest bytes and flush through kernel
`write`; `fclose` flushes before closing, even when the flush fails. A failed
flush discards its pending count and sets the error indicator. Direct writes
preserve partial transfer/EFAULT semantics; invalid buffered memcpy inputs stop
instead of inventing a kernel errno. Read/write transitions follow API 28:
refilling flushes pending output, while switching to writing discards read-ahead
at the current FD offset. Use a seek when the logical read position must be
preserved. `ftell` flushes output before querying the cursor, including append
streams; it does not report a stale pre-append position. Refilling a line or
unbuffered stream flushes other line-buffered writers. Large `fread` requests
use the API 28 direct FD reader, bypassing these refill transitions. `fdopen` also works for
captured output FDs with explicit unbuffered mode; it does not materialize
`stdin`, `stdout` or `stderr` FILE objects.

The output transition contracts are pinned to AOSP's
[write setup](https://github.com/aosp-mirror/platform_bionic/blob/android-9.0.0_r61/libc/upstream-openbsd/lib/libc/stdio/wsetup.c)
and [byte writer](https://github.com/aosp-mirror/platform_bionic/blob/android-9.0.0_r61/libc/upstream-openbsd/lib/libc/stdio/fvwrite.c),
with independent model code and synthetic tests.

### Live process observations

The optional [live proc and file metadata contract](linux-live-proc.md) supplies
maps, task/status, fdinfo and FD links from the shared Linux owners. Bionic
FILE/DIR buffering observes the same records as raw calls. Explicit tracked
memory files maintain complete stat metadata using the virtual realtime clock.
Unknown status fields, proc inode metadata and broader filesystem policies
remain explicit boundaries; a live view does not imply complete Android procfs.

### Directory streams

`opendir`, `fdopendir`, `readdir`/`readdir64`, `closedir`, `dirfd`, `telldir`,
`seekdir` and `rewinddir` use the shared Linux directory descriptions described
in [the file contract](process-emulation.md#directories-and-live-fd-links).
DIR tokens are opaque, and their guest record buffer holds 15 LP64 `dirent`
slots, matching the pinned [API 28 entrypoints](https://github.com/aosp-mirror/platform_bionic/blob/android-9.0.0_r61/libc/bionic/dirent.cpp).
The kernel cursor advances on buffer refill; `telldir` returns the last
consumed record's cookie. `fdopendir` adopts the FD without resetting it.
Seek/rewind discard the Bionic cache at their specified success boundaries;
EOF preserves errno. Closing the underlying FD does not erase buffered records.
Raw `getdents64`/`__getdents64` and `syscall` use the same kernel owner.
Scandir, reentrant directory APIs, general symlink traversal and a public DIR
layout remain unmodeled. Enumeration order and unspecified inode identities
belong to the virtual workload, not to the host filesystem.

### Integer scanning

`sscanf` and `vsscanf` share the formatting model's GP argument reader and
Android LP64 `va_list` rules. The scanner accepts `d i o u x X p n`, literal
characters, `%%`, ASCII whitespace, assignment suppression, decimal field
widths, and `hh h l ll j z t q` integer lengths. Android's `D` and `O` aliases
use `long`. `%n` records consumed input bytes without increasing the assignment
count. Suppression and widths on `%n`, and lengths on `%p`, are unsupported.

The input is a bounded NUL-terminated guest byte string. A numeric field consumes
at most 512 bytes, including its sign and radix prefix. An incomplete `0x`
prefix consumes only the zero (and optional sign). Matching failures return the
assignment count; input exhaustion returns EOF when no assignment has occurred.
Earlier assignments survive either normal failure. Destination widths and
truncation follow Android LP64, and successful conversions preserve thread errno.

Floating-point, character/string/scanset conversions, allocation, positional
arguments, malformed formats, and 64-bit conversion overflow stop explicitly.
Output that aliases input, format, or consumed argument storage also stops;
destinations may alias each other and are written in conversion order. All
formats, consumed pointers, and supported conversions are checked before
publishing writes. Invalid memory and unsupported calls do not publish partial
assignments or a successful return. This does not model libc's fault or overflow
side effects. Direct imports and dynamic symbols share implementation and
provider lifetime checks. Host scanf functions are never called.

The independently authored scanner follows Android 9's
[integer scanning behavior](https://android.googlesource.com/platform/bionic/+/android-9.0.0_r61/libc/stdio/vfscanf.cpp)
and [string input setup](https://android.googlesource.com/platform/bionic/+/android-9.0.0_r61/libc/upstream-openbsd/lib/libc/stdio/vsscanf.c).

### Explicit dynamic symbol catalogue

Supply exact library names and their available function symbols in the native
options. The same object works through C, CLI and Python:

```json
{"android": {
  "entry_symbol": "inspect_buffer",
  "libraries": {"libc.so": ["strlen", "memcmp"]}
}}
```

`dlopen("libc.so", RTLD_NOW)` now obtains an opaque guest handle. A subsequent
`dlsym(handle, "strlen")` returns a named guest trap that dispatches to the
existing bounded Bionic model. `android.native_calls` records the requested
`library` and `symbol` on the lookup, its returned address, and the same address
and provider on the later named call. Two providers have distinct traps even
when their function names agree. These addresses and handles are local model
identities, not host addresses or recovered original device addresses.

The catalogue defaults to empty. Library names match exactly, with no filesystem
search, dependency loading, aliases, constructors or host `dlopen`/`dlsym`.
There are at most 256 libraries and 4096 total function entries, with nonempty,
NUL-free names no longer than 1024 bytes and no duplicate functions per library.
Only functions are represented; data symbols and symbol versions need a fuller
linker model.

Supported flags are any combination of LP64 `RTLD_LAZY` (1), `RTLD_NOW` (2)
and `RTLD_NOLOAD` (4), including zero and NOLOAD alone, as accepted by API 28.
Unknown bits return a linker error; recognized GLOBAL/NODELETE requests stop
as unsupported. Repeated opens share a live handle and increment its reference
count. The last close invalidates that handle; a later open receives a new one.
Calls through a provider's trap while that provider is closed stop explicitly.
Missing libraries/symbols, closed or invalid handles, and a null symbol name
return a lookup error. `dlerror()` consumes the pending guest error pointer in
API 28 TLS slot 6 once; successful lookups do not clear an earlier error, and
`errno` is independent. Error message wording belongs to the model.

An optional `default_scope` supplies the complete ordered provider list for
`dlsym(RTLD_DEFAULT, name)` (handle 0 on LP64):

```json
{"android": {
  "entry_symbol": "inspect_buffer",
  "libraries": {"libfirst.so": ["strlen"], "libsecond.so": ["strlen", "memcmp"]},
  "default_scope": ["libsecond.so", "libfirst.so"]
}}
```

The first provider containing the requested function wins; reports retain its
library identity. Entries must be distinct exact names in `libraries`. An
omitted scope is unknown and stops explicitly; `[]` asserts an empty scope and
produces ordinary lookup errors. Merely listing a library in the catalogue or
opening it with `RTLD_LOCAL` does not add it to this search.

Scope providers are explicitly resident for the whole workload. Their functions
can be called before `dlopen`; `RTLD_NOLOAD` succeeds, and balanced opens/closes
preserve their handles and callable traps. A close without an outstanding open
returns a modeled error. Other providers retain the ordinary unload behavior
above. Each workload starts with fresh handles and linker errors.

This is a caller-supplied, fixed search snapshot. It must already incorporate
the applicable namespace, global/local group visibility and search order for
the workload; the model does not derive them from the input ELF, target SDK,
dependencies or caller PC. It supplies neither actual provider binaries nor
new function implementations. ELF import binding is unchanged by this scope.

`dlopen(NULL)`, `RTLD_NEXT`, `RTLD_GLOBAL`, `RTLD_NODELETE`,
Android linker namespaces and `android_dlopen_ext` remain unsupported. They
stop with a diagnostic rather than selecting a guessed process-wide scope.
`RTLD_NEXT` additionally requires an authenticated caller position.

ABI references: Android 9 [`dlfcn.h`](https://android.googlesource.com/platform/bionic/+/refs/tags/android-9.0.0_r1/libc/include/dlfcn.h),
[`dlsym`/`dlclose`](https://android.googlesource.com/platform/bionic/+/refs/tags/android-9.0.0_r1/linker/linker.cpp),
and [`dlerror`](https://android.googlesource.com/platform/bionic/+/refs/tags/android-9.0.0_r1/linker/dlfcn.cpp).

## API 28 pseudo-random sequences

`rand`/`random` share one guest process sequence; `srand`/`srandom` reset it
using the low 32 bits of the seed. The default 31-word additive generator,
signed seed expansion and 310 warm-up steps follow the pinned
[Android 9 random implementation](https://github.com/aosp-mirror/platform_bionic/blob/android-9.0.0_r1/libc/upstream-netbsd/common/lib/libc/stdlib/random.c)
and [Bionic aliases](https://github.com/aosp-mirror/platform_bionic/blob/android-9.0.0_r1/libc/bionic/rand.cpp).
An unseeded process starts with seed 1; seed 0 has its own sequence. Calls are
atomic within the guest scheduler and never use host entropy or host libc state.

`rand_r` updates exactly four guest bytes using the separate
[Android 9 recurrence](https://github.com/aosp-mirror/platform_bionic/blob/android-9.0.0_r1/libc/upstream-netbsd/lib/libc/stdlib/rand_r.c),
retains the high bit in the stored seed and returns the low 31 bits. Invalid or
read-only seed memory stops explicitly; successful calls preserve errno.
`initstate` and `setstate` remain unsupported.
These deterministic sequences reproduce the pinned ABI; they are not entropy.

## API 28 mutexes

The Bionic model implements `pthread_mutexattr_init`, `destroy`, `gettype`,
`settype`, `getpshared`, `setpshared`, `getprotocol`, and `setprotocol`, plus
`pthread_mutex_init`, `destroy`, `lock`, `trylock`, and `unlock`. LP64 attributes
occupy eight bytes. Mutex objects occupy 40 bytes with four-byte alignment;
their guest bytes are authoritative, including static initializers. Ordinary
operations retain padding and reserved storage. Initialization clears all
40 bytes before interpreting an overlapping attribute, including when an
invalid type returns EINVAL.

Normal, recursive and error-checking mutexes retain their distinct behavior.
Error-checking self-lock returns EDEADLK; busy try-lock returns EBUSY;
recursive depth exhaustion returns EAGAIN; a non-owner unlock of a recursive
or error-checking mutex returns EPERM. These are direct pthread return values
and do not change errno. Ownership uses the same guest TID as `gettid`.
Destroyed-object use stops explicitly, following the API 28 target behavior;
older app-target compatibility is not modeled.

With guest scheduling enabled, contended locks suspend the original import
and mark the guest object contended. Unlock wakes one matching waiter in
round-robin order; a wake permits another acquisition attempt. The waiter
reloads guest state and permissions after restoring its CPU context, and
returns through its original event only after acquiring the lock. A competing
acquirer can make it wait again. Resumed acquisitions retain the contended
bit, so subsequent releases do not strand other sleepers. Recursive unlock
wakes only when its final level is released. Normal locks never use the
owner field. State snapshots are confined to a single modeled operation.

Without guest scheduling, blocking and wake transitions stop explicitly.
The process-shared bit is retained within the workload's single address
space; it does not introduce an external process. Priority inheritance, timed
operations and nonzero futex padding that would require an EAGAIN retry loop
remain unsupported. Invalid state, changed mutex attributes while suspended,
and denied memory fail without completing the pending lock. Complete write
spans are checked before changing state or ownership. No host mutex or
futex supplies guest behavior.

ABI references: Android 9 [`pthread_mutex.cpp`](https://github.com/aosp-mirror/platform_bionic/blob/android-9.0.0_r1/libc/bionic/pthread_mutex.cpp)
and [`pthread_types.h`](https://github.com/aosp-mirror/platform_bionic/blob/android-9.0.0_r1/libc/include/bits/pthread_types.h).
These declarations and state rules inform an independent implementation.

## API 28 unnamed semaphores

`sem_init`, `sem_destroy`, `sem_getvalue`, `sem_post`, `sem_trywait` and
`sem_wait` share the guest count word. The LP64 object occupies 16 bytes;
these operations access only its first aligned 32-bit word and preserve the
reserved bytes. The low bit records process sharing. The signed upper 31 bits
hold the count or the contended state. Initialization rejects values above
`0x3fffffff` before accessing the object; posting at that limit returns
`-1/EOVERFLOW`. An empty try-wait returns `-1/EAGAIN`. Success preserves TLS
errno. API 28 destroy returns zero without accessing the supplied object.

A blocking wait requires enabled guest-thread scheduling. Posting to a
contended word wakes all modeled waiters; each reloads shared RAM before
competing for a token. A runnable caller may consume a token first, leaving
a woken waiter asleep again with its original import still pending. Invalid
state, changed sharing or inaccessible storage on resumption fail explicitly.
A wait with no possible producer reports the existing guest-thread deadlock.
Shared semaphores support participants in this modeled process; this does not
create external process participants. Timed waits, named semaphores, signal
interruption and raw futex interoperability remain unsupported.

This independently authored model follows Android 9
[`semaphore.cpp`](https://android.googlesource.com/platform/bionic/+/android-9.0.0_r1/libc/bionic/semaphore.cpp)
and [`semaphore.h`](https://android.googlesource.com/platform/bionic/+/android-9.0.0_r1/libc/include/semaphore.h).

## API 28 thread attributes

`pthread_attr_init`, `destroy`, and the get/set pairs for `detachstate`,
`inheritsched`, `schedpolicy`, `schedparam`, `stack`, `stacksize`, `guardsize`
and `scope` operate on guest LP64 storage. The 56-byte, eight-byte-aligned
object keeps its padding and reserved bytes during initialization: only its
six fields are assigned. Defaults include a 1 MiB minus 16 KiB stack and a
4 KiB guard. Destruction fills all 56 bytes with `0x42`.

Getters write four-byte integers or eight-byte pointers/sizes as declared.
Inheritance flags take precedence over API 28's historical scheduling-policy
fallback. Stack-size-only assignment accepts any size at least 16 KiB;
assignment of a stack address and size additionally requires page alignment.
Stack addresses are opaque guest values. Guard sizes and scheduling policies
retain the supplied values without inventing validation absent from Bionic.
Invalid enums and sizes return EINVAL before accessing the attribute. Scope
operations ignore the attribute: system scope succeeds and process scope
returns ENOTSUP. These direct pthread error returns preserve errno.

The model checks all affected spans before publishing writes, and preserves
ordered reads/writes when `getstack` outputs overlap the attribute. Invalid
memory or alignment stops explicitly. Attribute assignment does not create a
thread or authorize a scheduling policy. The shared symbol catalogue also supplies
named dynamic calls through `dlsym` and enforces provider lifetime.

This independently implemented model follows pinned AOSP
[`pthread_attr.cpp`](https://github.com/aosp-mirror/platform_bionic/blob/196632fb3c59ebbf1184d791a3e7124dd0c3f22b/libc/bionic/pthread_attr.cpp),
[`pthread_types.h`](https://github.com/aosp-mirror/platform_bionic/blob/196632fb3c59ebbf1184d791a3e7124dd0c3f22b/libc/include/bits/pthread_types.h),
[`pthread.h`](https://github.com/aosp-mirror/platform_bionic/blob/196632fb3c59ebbf1184d791a3e7124dd0c3f22b/libc/include/pthread.h)
and [`pthread_internal.h`](https://github.com/aosp-mirror/platform_bionic/blob/196632fb3c59ebbf1184d791a3e7124dd0c3f22b/libc/bionic/pthread_internal.h).
`AndroidThreadAttributes.def` owns layout constants; the existing Android
symbol and diagnostic tables retain their respective vocabularies.

## Cooperative guest threads

The default `android.thread_limit: 1` preserves the single-thread contract.
Values 2 through 256 enable a deterministic guest scheduler and bound the
total identities created, including the entry thread. Retired identities are
not reused. `pthread_create`, `pthread_join`, `pthread_detach`, `pthread_self`,
`pthread_equal`, `pthread_exit`, `pthread_getattr_np`, and `pthread_gettid_np`
share the imported and dynamically resolved call paths.

Threads have separate saved CPU contexts, stacks, TPIDR_EL0, errno, linker
errors and callback continuations. They share guest memory, allocations,
library handles and the process destructor registry. Scheduling rotates at
`instruction_quantum`, a blocking join, or thread completion. Native calls
consume the current quantum instead of restarting it. The shared execution
budget is never replenished. This is cooperative single-core execution, not
SMP or a host pthread implementation. Normal, raw and variadic `gettid` and
mutex ownership use the same current identity; `getpid` remains process-wide.

Creation admits normal scheduling and a model-allocated stack. Inherited and
historical API 28 flags retain their policy interpretation. Explicit stacks,
unknown flags and scheduling policies requiring unmodeled behavior stop.
Identity or memory capacity exhaustion returns EAGAIN without changing errno
or publishing a handle. Pointer faults remain failures. Stack and guard sizes
round to 4 KiB; each identity reserves a model address slot below a 64 MiB
ceiling. A PROT_NONE guard precedes writable stack bytes, and a separate TLS
page follows. `pthread_getattr_np` reports that actual model span, including
the guard, and preserves copied reserved attribute bytes. Handles are opaque
unmapped identities, distinct from TIDs; this does not reconstruct Bionic's
private TCB or its allocator-dependent placement. Changed TLS identity slots
or TPIDR_EL0 stop Bionic calls explicitly.

A join retains its original service continuation until the target finishes,
then publishes the full pointer result and releases its storage. Detached
threads release storage on completion. Invalid non-null or retired handles
fail at the API 28 target boundary; NULL lookup returns ESRCH, self-join
returns EDEADLK, and detached or already claimed targets return EINVAL.
If no thread is runnable, an admitted sleep can advance virtual time to its
deadline. With no such deadline, outstanding join, once, mutex or semaphore
waits produce an explicit unsupported stop.
`pthread_exit` returns its pointer to a joiner; raw `SYS_exit` terminates only
the current thread and leaves its pthread result zero. When the last thread
exits without an entry return, its low eight status bits become the workload
exit status. `exit_group` terminates
the workload. Guest cleanup handlers, TLS keys/destructors, exit during a
once/finalize callback, timers, cancellation,
signals and clone remain unsupported. No cleanup or completion is invented.

By default the selected entry's return stops observation, even with live
children. `android.drain_threads: true` continues those children under the
same limits. The entry's `return_value` remains visible if a later child stops
with an error. `android.threads` records identity, placement, waiting,
completion and retirement; completion of its initial row means the selected
invocation ended, not that the process exited. Named calls and raw services
include `thread_id`. `android.trace_threads` partitions the existing PC trace
using zero-based `trace_index` and `thread_id`. Each new run consumes 16 bytes
of the remaining report allowance; insufficient allowance truncates trace
retention, not execution. Thread metadata reserves 128 bytes per allowed
identity in addition to the trace and memory snapshots.

These independently implemented API rules use AOSP Bionic revision
`196632fb3c59ebbf1184d791a3e7124dd0c3f22b`:
[`pthread_create.cpp`](https://github.com/aosp-mirror/platform_bionic/blob/196632fb3c59ebbf1184d791a3e7124dd0c3f22b/libc/bionic/pthread_create.cpp),
[`pthread_join.cpp`](https://github.com/aosp-mirror/platform_bionic/blob/196632fb3c59ebbf1184d791a3e7124dd0c3f22b/libc/bionic/pthread_join.cpp),
[`pthread_detach.cpp`](https://github.com/aosp-mirror/platform_bionic/blob/196632fb3c59ebbf1184d791a3e7124dd0c3f22b/libc/bionic/pthread_detach.cpp),
and [`pthread_exit.cpp`](https://github.com/aosp-mirror/platform_bionic/blob/196632fb3c59ebbf1184d791a3e7124dd0c3f22b/libc/bionic/pthread_exit.cpp).
They establish a bounded model contract, not native Android equivalence.

## Evidence and limits

`android.native_calls` records import arguments and nullable results.
`android.trace` records **admitted instruction attempts**, including service
instructions; it does not claim every attempt retired. `trace_limit` bounds
retention, and `trace_truncated` distinguishes an incomplete trace. Trace
storage plus requested memory snapshots must fit `output_limit`. Output streams
also obey the existing process output limit.

A trace demonstrates behavior for its explicit inputs. It is not proof of
unvisited branches or arbitrary input equivalence. Unknown services and CPU
instructions, invalid pointers, stack-check failures, and exhausted budgets
remain visible stops. No unsupported instruction is replaced by NOP.

## JNI native values and thread attachment

Set `"android":{"jni":true,...}` to enable the JNI 1.6 LP64 interface. The
strict Boolean defaults to false. `JNI_GetCreatedJavaVMs` exposes one guest VM;
`GetEnv`, attachment and detachment use the cooperative native thread ID.
Threads initially have no JNIEnv. A successful attachment creates a distinct
identity; an environment from another thread or a previous attachment fails.
The function tables are immutable guest storage, checked before each call.
Internal JNI entry names are not accepted as arbitrary ELF imports; calls must
enter a thunk owned by the prepared interface table.

This native value model supports intrinsic class identity, local/global
references, local frames, pending exceptions, UTF-16 and modified UTF-8 strings,
primitive arrays of all eight widths, and Object/String object arrays. Array
region copies preserve complete element bits; borrowed element copies obey
release, commit and abort modes. Borrowed strings require matching releases.
Pending-exception cleanup preserves both exception identity and borrowed values.
Slots not implemented stop with their standard JNI table index in the diagnostic.

The supported class set comprises Object, Class, String, Throwable, Exception,
Error, RuntimeException, NullPointerException, IllegalArgumentException, IndexOutOfBoundsException,
ArrayIndexOutOfBoundsException, StringIndexOutOfBoundsException,
NegativeArraySizeException, ArrayStoreException and OutOfMemoryError, plus the
primitive and Object/String array classes. Unknown classes stop rather than
inventing an application class or assuming ClassNotFoundException. `ThrowNew`
admits only a null message for these intrinsic throwable classes.

Java execution includes the intrinsic methods and explicit integral DEX bodies
below. There is no field execution, general class loader, native-method registration, monitor, weak-reference,
critical/direct-buffer, GC or JIT model.
Attachment names/groups and VM destruction also remain unsupported. The model
retains at most 4096 objects and 4096 live references/borrows, with object storage
bounded by half the memory limit and 512 unique attachment identities per
workload. Exhaustion fails explicitly; it is not a claim about ART heap pressure.
Native child-thread exit requires detachment and all borrowed copies released.
Stopping observation at the selected main function's return is not a thread exit.

Invocation ordering and return statuses follow the pinned
[API 28 ART implementation](https://android.googlesource.com/platform/art/+/android-9.0.0_r1/runtime/java_vm_ext.cc).
In particular, zero-capacity VM enumeration copies no VM and reports zero, and
unsupported GetEnv versions leave its output untouched. Native values follow
[JNI functions](https://docs.oracle.com/en/java/javase/17/docs/specs/jni/functions.html)
and the [invocation interface](https://docs.oracle.com/en/java/javase/17/docs/specs/jni/invocation.html).
These are bounded protocol models implemented independently of ART, not a booted
Android runtime. See [device contracts](linux-device-contracts.md) for Binder and
GPU ownership shared with raw Linux syscalls.

ART-specific JNI value boundaries follow the pinned [API 28 JNI owner](https://android.googlesource.com/platform/art/+/refs/tags/android-9.0.0_r1/runtime/jni_internal.cc)
and [UTF conversion contract](https://android.googlesource.com/platform/art/+/refs/tags/android-9.0.0_r1/libdexfile/dex/utf.cc).
`NewStringUTF` accepts null and valid four-byte Unicode sequences; a complete
surrogate pair encodes as four bytes, while an isolated UTF-16 surrogate unit
encodes as three. `GetStringUTFChars` on null leaves `isCopy` untouched. Malformed
encodings still fail explicitly. Negative array construction and an incompatible
object-array initializer are JNI abort boundaries, represented by `runtime_failure`,
not recoverable Java exceptions. Array-region bounds and `SetObjectArrayElement`
type mismatches produce pending Java exceptions. Negative local-frame capacity
returns `JNI_ERR` without pushing a frame. These distinctions are tested separately.


### Intrinsic Java method execution

GetMethodID/GetStaticMethodID resolve opaque, VM-owned method IDs. Ordinary,
`V` and `A` calls share the same descriptor, receiver, invocation-kind and
return-type checks. GP varargs reuse the Bionic AAPCS64 reader, including stack
arguments; `A` calls read eight-byte jvalue carriers and consume only declared
primitive bits. Method IDs survive deletion of the lookup reference. A zero-arg
method does not read its argument pointer. Unknown method bodies stop with the
class, method and signature; they are not evidence for NoSuchMethodError.

The independently implemented, value-based subset is:

- Object.getClass/equals, including String's virtual equals override and explicit
  nonvirtual Object identity; Class.getName/isArray, with cached name identity.
- String.length/isEmpty/charAt/hashCode/equals/toString, concat, substring with
  one or two integer indices, compareTo(String), startsWith with optional offset,
  endsWith, indexOf(String) with optional starting index, getChars and toCharArray.
- String constructors for empty, String and char[] (whole array or offset/count),
  plus static valueOf for int, long, boolean, char and char[].

The operations compute UTF-16 values from the actual arguments and preserve
copy/alias behavior, 32-bit hash overflow, bounds and null exception ordering.
Boolean and minimum-integer literal identities, and Android's cached int strings
for -99 through 99, are retained separately from newly allocated value strings.
getChars validates its destination before writing and preserves it on exception.
Allocation shares the JNI object budget. String search also has a finite work
budget. This intrinsic catalogue does not infer application methods or arbitrary
class initializers. Explicit supplied DEX bodies use the separate executor below.

Semantic references are the pinned Android 9
[String](https://android.googlesource.com/platform/libcore/+/android-9.0.0_r1/ojluni/src/main/java/java/lang/String.java),
[Class](https://android.googlesource.com/platform/libcore/+/android-9.0.0_r1/ojluni/src/main/java/java/lang/Class.java),
[Integer](https://android.googlesource.com/platform/libcore/+/android-9.0.0_r1/ojluni/src/main/java/java/lang/Integer.java),
[Long](https://android.googlesource.com/platform/libcore/+/android-9.0.0_r1/ojluni/src/main/java/java/lang/Long.java)
and JNI invocation contracts linked above. No host Java runtime is invoked.


## Explicit DEX methods and object state

With JNI enabled, `android.dex_images` accepts an array of complete hex-encoded
DEX images. This is an explicit classpath for this workload; the model never
searches the host, phone, APK store or network. The Android API 28 profile admits
versions 035, 037, 038 and 039, at most 16 images and 1 MiB of input, further
limited by the process memory allowance. The shared DEX reader checks container
checksums, metadata, instruction boundaries and code flow before native guest
constructors run. Duplicate definitions and platform class replacements fail.
The reader's existing annotation and format restrictions also apply.

`FindClass`, method lookup and field lookup bind supplied declarations and exact descriptors,
including inherited members.
Ordinary, V and A calls share the same JNI descriptor/argument owner as the
intrinsics. Method identities belong to this VM and survive local frame or
thread teardown. The execution contract supports static/instance methods,
constructors and nonvolatile integral/reference fields on finite supplied
class hierarchies, including public abstract interface declarations. Supported bodies include integer/wide constants and moves,
32/64-bit arithmetic and shifts, narrowing/sign extension, signed comparisons,
branches, packed/sparse switches, loops, recursion and static calls to supplied
methods. Integral arguments/results include boolean, byte, char, short, int and
long. All arithmetic uses Java wrapping and masked shift distances; minimum
integer divided by -1 wraps, and division by zero raises a pending
ArithmeticException for JNI to observe and clear.

Each workload retains its own class state across JNI calls, local-reference
deletion, detach/reattach and sequential calls from different guest threads.
DEX encoded field values (or zero defaults) are installed before `<clinit>`.
FindClass, static method lookup, static invocation and static field instructions
initialize the declaring class once. Recursive initialization on the executing
thread can observe prepared or partially written values; it does not rerun the
initializer. `sget`/`sput` integral variants check the declared field width;
stores narrow byte/char/short values, and final stores are restricted to the
declaring initializer. Cross-class nonpublic members remain unsupported.

A modeled Java exception escaping `<clinit>` produces a pending
ExceptionInInitializerError; subsequent use of that erroneous class produces
NoClassDefFoundError. Nested initialization Errors propagate. Effects already
performed by Java code remain observable. An unsupported instruction, invalid
execution or exhausted resource budget terminates the workload rather than
being converted into a Java exception or silently restarting initialization.
Modeled throwables retain heap identity and explicit constructor messages,
without a complete ART cause/stack-trace object graph. Initialization follows the synchronous subset of
[Java class initialization](https://docs.oracle.com/javase/specs/jls/se25/html/jls-12.html#jls-12.4.2)
and [JNI initialization triggers](https://docs.oracle.com/en/java/javase/25/docs/specs/jni/functions.html#findclass).

Every DEX instruction consumes the existing native workload instruction budget
and deadline. Calls do not reset either budget. Recursion is bounded to 64
frames and a conservative frame-storage share of the stack/memory allowance;
resource exhaustion stops explicitly. Uninitialized registers, torn wide
values and incompatible results fail. Wide moves read both halves before an
overlapping write. Diagnostics name the method and DEX code-unit address.
DEX bodies execute synchronously inside their JNI call; this contract does not
preempt Java threads or equate bytecode steps with physical CPU/GPU timing.
Native scheduling and explicitly configured virtual clocks retain their owners.

DEX and JNI share a bounded heap, object identities, field values and array
storage. `new-instance`, direct/private calls, virtual/interface dispatch,
`invoke-super`, `iget`/`iput` and reference static fields execute supplied bodies.
Objects include inherited fields and methods resolve from declared and actual
types. Superclasses initialize before subclasses; static members retain the
state of their declaring class. JNI ordinary calls select the actual override,
while nonvirtual calls validate and execute the named class's resolved method.
Direct `this`/super constructor chains keep one object identity, with initialization
completed only after the outer constructor returns. Uninitialized objects can
be moved but cannot escape through calls, fields or returns; arbitrary
constructor self-escape remains outside the profile. JNI `AllocObject` explicitly creates
zeroed storage without running the constructor; `NewObject` ordinary/V/A runs it.

Reference registers remain distinct from integers (only literal zero can also
serve as null). `new-array`, `filled-new-array`, `fill-array-data`, array length,
all integral/reference `aget`/`aput` variants, casts, instance tests and reference
equality share the same type rules. Arrays may contain supplied instances or
other arrays; primitive arrays are not coerced across component types. Null,
bounds, negative DEX lengths, array-store and cast failures produce their Java
exceptions. JNI negative-length allocation retains its existing fatal contract.
JNI array borrows still copy back only under their requested release mode.

JNI GetFieldID/GetStaticFieldID and integral/reference accessor families resolve
opaque, workload-owned IDs against supplied declarations. They validate the
receiver, storage kind and accessor type; field lookup initializes the class.
JNI changes are visible to DEX and vice versa, including after detaching a native
thread. Final-field mutation through JNI is deliberately rejected. Object storage
is retained until workload teardown with finite byte/object limits; reaching a
model limit stops explicitly, without claiming that a real Java VM would throw
OutOfMemoryError. There is no garbage-collection or ART layout claim.

DEX string constants (including encoded static String values), reference class literals,
and the same bounded String/Object/Class value methods available through JNI
now execute in the shared runtime. UTF-16 preserves embedded NUL, supplementary
characters and isolated surrogates. Literal strings and `String.intern` share a
workload pool; class objects share identity with FindClass/GetObjectClass.
Supported throwable constructors accept no argument or a String message;
`Throwable.getMessage` returns that actual message identity.

Ordered typed catches, catch-all handlers, `move-exception`, explicit throws,
rethrows and compiled finally paths execute decoded control flow. Exceptions
from modeled arithmetic, arrays and library operations follow the same handler
selection. The original throwable survives JNI ExceptionOccurred/Throw and
catch/rethrow. Non-Java failures and budget stops cannot be swallowed by a catch.

This is a bounded interpreter, not complete ART. Float/double execution,
other encoded reference constants, default/static interface bodies, interface
fields, synchronization, volatile fields, cross-class nonpublic member access,
and external/native Java methods remain unsupported. The finite library catalogue
is explicit; an unknown method never supplies a guessed Java result.
Binder system-service implementations and GPU shaders are independent inputs
and are not supplied by admitting DEX.

The instruction contract follows the Android
[Dalvik bytecode specification](https://source.android.com/docs/core/runtime/dalvik-bytecode)
and [DEX format](https://source.android.com/docs/core/runtime/dex-format).
`test_android_dex_runtime.py` compiles original Java fixtures with javac and D8,
then compares the JVM's class-file results with JNI calls into those DEX bodies.
`test_android_dex_state.py` compares every value and exception type in repeated
calls, reversed cyclic initialization, retained state after failure, reattachment
and sequential guest-thread execution against independently run Java class files.
`test_android_dex_objects.py` compares JVM values with constructors, instance
calls, typed field access, reference identity, primitive/object/nested arrays,
copy-back, global references across reattachment and recoverable Java exceptions.
It also tests invalid field IDs/accessor types, final writes and heap limits.
`test_android_dex_language.py` adds independent JVM checks for UTF-16 operations,
interning, thrown-object identity, ordered handlers, finally effects, repeated
unwinding, inherited fields and class/interface/super/JNI nonvirtual dispatch.
The JVM and D8 prepare/check fixtures only; NeverD execution uses neither.

## Offline address resolution and TCP

Explicit `android.resolver_queries` and `linux_network` inputs allow bounded
`getaddrinfo`/`freeaddrinfo`, `inet_ntop`/`inet_pton`, socket creation, timeout
options, connect and scalar
stream I/O. See [offline network contracts](linux-network-contracts.md) for the
API 28 layout, ownership checks, errno, descriptor aliases and unsupported
boundaries. No host DNS or sockets are used.
