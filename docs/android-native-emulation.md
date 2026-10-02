# Android native emulation

`android-aarch64-api28-v1` runs one native function in an Android 9 / API 28
AArch64 shared library using NeverD's CPU, address space, AAPCS64 call frames,
execution sessions, and Linux syscall models. This is a bounded native analysis
environment, with explicit inputs and observable failures. It does not boot an
Android system or supply ART, JNI, Binder, threads, signals, a filesystem, or a
network. It never calls host functions to satisfy a guest import.

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
`read_memory` snapshots must fit the output budget and initially readable
memory. A snapshot that becomes unreadable fails explicitly. Terminal CPU or
model faults do not produce snapshots. No host addresses are exposed as guest
memory.

By default, DT_INIT and DT_INIT_ARRAY constructors execute in order before the
selected function, with an explicit empty argc/argv/envp. All calls share the
same instruction, event and deadline budget. `initialize: false` explicitly
requests analysis of an **uninitialized** library; the report records this
choice. A normal function return has `stop_reason: "returned"`, a
`return_value`, and CLI status 0 regardless of its integer return value.
`exit`/`exit_group` retain Linux process-exit semantics. Destructors are not run
when the observation ends at the selected function's return.

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
- `malloc`, `calloc`, `realloc`, `free`, with live allocation tracking and
  bounded anonymous guest memory. Zero-size allocations may return a unique
  pointer; allocation failure returns NULL and sets ENOMEM.
- `__errno`, `getpid`, `gettid`, `android_get_device_api_level` (28).
- `__system_property_get`, backed only by the explicit property dictionary.
  Missing properties return length zero and write NUL; values must fit 91 bytes
  plus NUL. Host properties are never inherited.
- `dlopen`, `dlsym`, `dlclose`, `dlerror`, using an explicit local catalogue
  described below. Function availability and implementation are separate:
  an available symbol whose call is unmodeled still stops explicitly.
- `write`, `mmap`/`mmap64`, `mprotect`, `munmap`, delegated to the shared Linux
  service implementation. Bionic wrappers translate negative kernel error
  values to -1 and thread-local errno; raw `svc #0` preserves negative errno
  bits and does not update TLS errno.

TLS uses the API 28 Bionic layout: TPIDR_EL0 points to a guest TLS block,
`__errno` addresses slot 2, and the stack guard occupies slot 5. The guard is a
deterministic analysis value, not host randomness. `__stack_chk_guard` aliases
that value; `__stack_chk_fail` and `abort` fail explicitly. The known `__sF`
stdio data symbol has an opaque, inaccessible guest address: field reads fail
instead of receiving fabricated FILE contents. stdio operations are not modeled.

These ABI choices are based on the pinned AOSP Android 9 definitions:
[Bionic TLS slots](https://android.googlesource.com/platform/bionic/+/refs/tags/android-9.0.0_r1/libc/private/bionic_tls.h),
[`__errno`](https://android.googlesource.com/platform/bionic/+/refs/tags/android-9.0.0_r1/libc/bionic/__errno.cpp),
and [system property declarations](https://android.googlesource.com/platform/bionic/+/refs/tags/android-9.0.0_r1/libc/include/sys/system_properties.h).
The model is independently implemented; these sources specify the ABI.

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

Supported flags are LP64 `RTLD_LAZY` (1) or `RTLD_NOW` (2), optionally with
`RTLD_NOLOAD` (4). Repeated opens share a live handle and increment its reference
count. The last close invalidates that handle; a later open receives a new one.
Calls through a provider's trap while that provider is closed stop explicitly.
Missing libraries/symbols, closed or invalid handles, and a null symbol name
return a lookup error. `dlerror()` consumes the pending guest error pointer in
API 28 TLS slot 6 once; successful lookups do not clear an earlier error, and
`errno` is independent. Error message wording belongs to the model.

`dlopen(NULL)`, `RTLD_DEFAULT`, `RTLD_NEXT`, `RTLD_GLOBAL`, `RTLD_NODELETE`,
Android linker namespaces and `android_dlopen_ext` remain unsupported. They
stop with a diagnostic rather than selecting a guessed process-wide scope.
Existing ELF import binding is unchanged by the catalogue.

ABI references: Android 9 [`dlfcn.h`](https://android.googlesource.com/platform/bionic/+/refs/tags/android-9.0.0_r1/libc/include/dlfcn.h),
[`dlsym`/`dlclose`](https://android.googlesource.com/platform/bionic/+/refs/tags/android-9.0.0_r1/linker/linker.cpp),
and [`dlerror`](https://android.googlesource.com/platform/bionic/+/refs/tags/android-9.0.0_r1/linker/dlfcn.cpp).

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
