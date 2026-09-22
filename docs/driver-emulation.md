**Languages**: [English](driver-emulation.md) | [简体中文](zh-CN/driver-emulation.md) | [繁體中文](zh-TW/driver-emulation.md) | [日本語](ja/driver-emulation.md) | [한국어](ko/driver-emulation.md) | [Français](fr/driver-emulation.md) | [Deutsch](de/driver-emulation.md) | [Español](es/driver-emulation.md) | [Italiano](it/driver-emulation.md) | [Русский](ru/driver-emulation.md) | [العربية](ar/driver-emulation.md)

[← Documentation index](README.md)

# Windows driver emulation

NeverD's optional driver emulator executes the PE entry point of a supported
x64 WDM driver and optionally exercises an explicit synchronous request
scenario before unloading it. It uses Unicorn for CPU execution and NeverD's
own bounded Windows environment model. It does not load
the driver into the host kernel or forward guest API calls to host OS services.

## Build and run

The feature is opt-in and independent of `BUILD_TESTING`:

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEVERD_ENABLE_DRIVER_EMULATION=ON
cmake --build build-release --target neverd --parallel 4
build-release/bin/neverd emulate-driver path/to/driver.sys \
  --instruction-limit 100000 > driver-report.json
```

The report is always JSON on stdout; request and setup diagnostics go to
stderr. The default limits are 100000 guest instructions, 64 MiB of guest
memory, 10000 recorded events, and 5000 milliseconds. The instruction limit
must be positive. An exhausted budget stops execution with partial observations.

| Exit code | Meaning |
|-----------|---------|
| `0` | Initialization and every requested completed operation succeeded |
| `1` | Invalid input/options, setup failure, or feature disabled at build time |
| `2` | Initialization or a completed request returned a failing `NTSTATUS` |
| `3` | Execution stopped before the scenario completed, such as an unsupported API, fault, or budget limit |

A returned failure status is a completed observation of that operation. A
successful return only describes this modeled run; it does not establish that
the driver works under Windows.

## Driver compatibility

Compatibility is determined by the executed code path and its dependencies,
not by the `.sys` extension. The current acceptance evidence covers original
freestanding fixtures and the buffered, in-direct and out-direct paths of
Microsoft's SIOCTL WDM sample, including its debug logging build.
It does not establish compatibility with arbitrary third-party drivers.

| Driver class or requirement | Current scope | Missing environment |
|-----------------------------|---------------|---------------------|
| x64 software WDM driver using the listed APIs | Initialization and synchronous file lifecycles | Each additional executed API must have a defined model |
| `METHOD_BUFFERED` IOCTL | Supported, with independent file identities and interleaved requests | Asynchronous completion is unavailable |
| `METHOD_IN_DIRECT`, `METHOD_OUT_DIRECT` | Request-owned MDLs and system mappings | Driver-allocated MDLs, physical page identities, DMA and user mappings |
| Synchronous READ/WRITE | Buffered or direct according to device flags | Neither I/O, implicit file-position selection and asynchronous completion |
| `METHOD_NEITHER` | Rejected | User address-space context, access probing and guest exception handling |
| KMDF / UMDF driver | Unsupported | Framework binding, objects, queues, callbacks, and the appropriate host runtime |
| PnP bus/function/filter driver | Initialization may run within the API subset; device-stack lifecycle is unsupported | Device attachment, lower-driver dispatch, PnP and power IRPs |
| Storage, network, display, filesystem and minifilter drivers | Unsupported subsystem contracts | Port/class/miniport frameworks, NDIS/WFP, graphics or filesystem services |
| Driver using worker threads, timers, DPCs, APCs, waits or cancellation | Unsupported | Scheduling, IRQL transitions, synchronization and asynchronous ownership |
| Driver using process/thread callbacks, handles, registry/file operations or kernel-module discovery | Unsupported outside the listed APIs | Object manager, system state and callback/event producers |
| Hardware, DMA, PCI, interrupt or virtualization driver | Unsupported environment | Device models, physical memory, buses, interrupts and privileged CPU state |
| x86 or ARM64 Windows driver | Rejected | Architecture-specific loading, ABI and execution model |
| x64 image requiring CFG, unsupported load configuration, TLS or other rejected PE features | Rejected at load time | Explicit loader/runtime semantics for those requirements |

An unused unsupported import can remain bound. Reaching an unsupported operation
stops with a diagnostic and the observations collected so far. A successful
DriverEntry alone does not prove that later dispatch, hardware or framework
paths are supported. The API table below is the authoritative supported subset.

## Execution contract

The profile models one single-threaded x64 WDM lifecycle at `PASSIVE_LEVEL`.
Execution begins at the PE entry point, retaining a compiler's entry wrapper
when present. DriverEntry must return `STATUS_SUCCESS` to initialize; a
nonzero successful or pending status stops as an unsupported initialization
contract. A failing status is retained as a completed initialization result.
All objects, strings, stacks, function pointers, and allocations live in guest memory. The model provides a `DRIVER_OBJECT` and a registry path
for the configured service name (default `NeverDDriver`).
The adapter uses Unicorn’s virtual TLB mode to preserve guest virtual addresses,
including canonical high kernel addresses, without synthesizing Windows page
tables. The initial RFLAGS value is `0x202`; the software-device profile uses a
fixed 64-byte cache line. These are explicit properties of this execution scenario.
Inline x64 CR8 reads observe the same `PASSIVE_LEVEL`; CR8 writes and other
control-register operations remain unsupported.

Unknown imports bind to lazy traps. An unused import does not prevent execution;
executing its thunk or reading an unmodeled exported data value stops with
`unsupported_api`. Unsupported CPU environment effects also stop explicitly.
NeverD does not replace unimplemented calls with success values. Malformed
images or unsupported loading requirements fail before execution.

This profile does not implement a complete Windows kernel, KMDF runtime,
PnP/power lifecycle, asynchronous or pending IRPs, neither-method IOCTLs,
interrupts, or multi-thread scheduling. Callbacks execute only when explicitly
requested by the scenario; initialization alone still stops after DriverEntry.

Images use their preferred base unless a scenario selects a valid relocated
address, and must be PE32+ x64 executables with the native subsystem. Imports
may come from `ntoskrnl.exe` or `ntkrnlmp.exe`.
The execution loader supports validated x64 `DIR64` base relocations and a
limited security-cookie load configuration, initialized before the entry wrapper
with a deterministic guest cookie. CFG and other unmodeled load-configuration
fields, TLS, delayed/bound imports, ordinal imports, and managed images are
rejected. Images must also pass strict range and alignment checks.

The initial API model deliberately has a finite contract:

| APIs | Modeled behavior and restrictions |
|------|-----------------------------------|
| `RtlInitUnicodeString` | Builds a guest `UNICODE_STRING` for a bounded NUL-terminated source |
| `RtlCopyUnicodeString`, `RtlCompareUnicodeString`, `RtlEqualUnicodeString` | Counted UTF-16 copy and case-sensitive comparison; case-insensitive comparison requires a Windows case table and stops |
| `ExAllocatePool2` | Paged/nonpaged NX allocations, zeroed by default; uninitialized and cache-aligned flags modeled; invalid required flags return NULL, quota/executable pools and raised allocation exceptions stop |
| `MmGetSystemRoutineAddress` | Resolves a counted guest name through the shared export inventory |
| `MmMapLockedPagesSpecifyCache`, `MmGetSystemAddressForMdlSafe`, `MmUnmapLockedPages` | Request-owned MDLs, cached KernelMode system mappings, explicit permissions and lifetime; existing safe mappings are reused |
| `ExAllocatePoolWithTag`, `ExFreePoolWithTag`, `ExFreePool` | Data allocations for pool types `0`, `1`, and `512`; positive size/tag, matching tagged frees, no address reuse |
| `IoCreateDevice`, `IoDeleteDevice` | Device type `0x22`, characteristics `0` or `0x100`, bounded extensions, ASCII `\Device\Name` names |
| `IoCreateSymbolicLink`, `IoDeleteSymbolicLink` | ASCII `\DosDevices\Name` or `\??\Name` within one session namespace, targeting `\Device\Name` |
| `DbgPrint`, `DbgPrintEx` | Checked Win64 variadic formatting, at most 512 output bytes; all debugger filters enabled |
| `IoGetCurrentIrpStackLocation` | Returns the stack location of the active modeled IRP; normal compiled WDM macros read the same guest field |
| `KeGetCurrentIrql` | Returns `PASSIVE_LEVEL` |
| `IofCompleteRequest`, `IoCompleteRequest` | Completes the active synchronous modeled IRP with `IO_NO_INCREMENT`; a completed IRP or buffer cannot be accessed again |
| `memcpy`, `memmove`, `memset`, `memcmp`, `RtlCopyMemory`, `RtlMoveMemory`, `RtlFillMemory`, `RtlZeroMemory`, `RtlCompareMemory` | Bounded guest buffer operations, at most 1 MiB per call; non-overlapping copy APIs reject overlaps |

`DbgPrint` formatting supports integer `d/i/u/o/x/X`, pointer `p`, text `s/c`,
`%%`, counted Unicode `wZ/lZ`, wide `ls/ws`, flags, width/precision including
`*`, and Windows integer length modifiers. At most 32 variable arguments and
1024 format bytes are read. Width and precision are limited to 512. Floating
point, `%n`, unknown combinations and non-ASCII text conversions stop explicitly;
the model does not guess a Windows code page or call host printf on guest data.

The original RegistryPath record and buffer expire when DriverEntry returns.
Drivers that need the string later must copy it during initialization.

The object/pool arena is 1 MiB. Uninitialized pool bytes use deterministic
`0xCD` contents; freed pool bytes use `0xDD`. This is one concrete execution
scenario. CPU accesses and modeled buffer APIs reject freed pool allocations,
deleted devices, unallocated arena bytes, opaque object fields, and writes to
read-only object fields. These checks cover this model's object lifetimes;
they are not a general driver memory-safety analysis. Unwritten dispatch-table
slots report zero as "unregistered". A scenario request for an unregistered
major function completes through the modeled default handler with
`STATUS_INVALID_DEVICE_REQUEST`; the failure remains visible in both dispatch
and I/O statuses. The model does not invent a guest function address for that
handler, and guest reads of an unwritten slot remain unsupported. Explicitly
registering a null callback is an error.

## Request scenarios

Pass a JSON file with `--scenario` to select requests and optional unload:

```bash
build-release/bin/neverd emulate-driver path/to/driver.sys \
  --scenario scenario.json > driver-report.json
```

For a driver that creates `\Device\NeverDIO` and accepts buffered IOCTL
`0x222000`, an example `scenario.json` is:

```json
{
  "requests": [
    {"kind": "create", "device": "\\Device\\NeverDIO"},
    {"kind": "ioctl", "code": "0x222000", "input": "00112233", "output_size": 4},
    {"kind": "cleanup"},
    {"kind": "close"}
  ],
  "unload": true
}
```

The device name and IOCTL code must match the driver. On create, omitting
`device` selects the sole live device; ambiguous selection fails. Later requests
use their file's device unless an explicit matching name is given. Optional
`file` is an unsigned 32-bit scenario identity, defaulting to zero. Each identity
has its own FILE_OBJECT and FsContext and requires create, transfers, cleanup,
and close in that order. Requests for independent files may be interleaved.
Exclusive devices reject a second open. These identities represent file objects,
not duplicated handles. Buffered and both direct IOCTL methods are supported. Dispatch must synchronously complete
each IRP; returning `STATUS_PENDING`, failing to complete, invalid output
lengths, and accessing a completed IRP fail explicitly. Requested unload must
leave no live device, symbolic link, pool allocation, or file object.

The optional root field `"load_address": "0x190000000"` requests rebasing;
omission or `"0x0"` uses the preferred address. Relocation requirements must be
satisfied by the image. No scenario is implied by the original initialization
command or C API.

Only `load_address`, `requests`, `unload`, and `kernel_exports` are accepted at
the root. All requests accept `kind`, optional `device` and optional `file`.
IOCTLs require `code` and accept `input`, `output_size`, and `direct_input`.
A `read` accepts `output_size` and `byte_offset`; a `write` accepts `input` and
`byte_offset`. Offsets default to zero, accept integers or hex strings, and must
fit a nonnegative signed 64-bit value. Lifecycle requests reject transfer fields.
Unknown or duplicate fields are rejected.
`code` accepts an unsigned 32-bit JSON integer or a `0x` hexadecimal string.
`input` is an even-length hexadecimal byte string without a prefix or spaces;
omission means empty input. `output_size` is an unsigned JSON integer; omission
means zero. Numeric fractions and floating-point spellings are rejected.

For direct IOCTLs, `input` initializes the first system buffer, while
`direct_input` initializes the separate MDL-described second buffer, padded
with zeros to `output_size`. `METHOD_IN_DIRECT` requires read access; it does
not imply a read-only system mapping. Both methods use readable/writable
scenario buffers. `MdlMappingNoWrite` removes mapping write access and
`MdlMappingNoExecute` removes execute access. Unmapping revokes the system VA;
remapping retains the same locked data. Completion expires the MDL and mapping.
The public MDL fields used by WDM macros are modeled; process/PFN fields,
hand-built MDLs, user mappings and direct access through raw UserBuffer are
rejected. A zero-length direct buffer has a null MDL.

For READ/WRITE, `DO_BUFFERED_IO` or `DO_DIRECT_IO` selects the transfer method.
Neither or conflicting flags stop. Information is checked against the transfer
length; writes return a count and reads return bytes.

`kernel_exports` maps routine names to explicit availability booleans, for
example `"kernel_exports": {"OptionalRoutine": false}`. Modeled exports and
static imports receive stable addresses shared with `MmGetSystemRoutineAddress`.
An explicitly absent export resolves to NULL and cannot satisfy a static import.
A declared present export without an API model resolves to a lazy trap. An
unknown dynamic name stops with an unspecified-availability diagnostic; absence
is never inferred from missing implementation. Names are bounded printable
ASCII and resolution is case-sensitive. The inventory is a concrete scenario
property, not a claim to match every Windows release.
`IoGetCurrentIrpStackLocation` and `MmGetSystemAddressForMdlSafe` are modeled WDM header helpers; this does not declare them exported by default, so their export availability requires a static import or an explicit `kernel_exports` declaration.

Scenario text is limited to 2 MiB, with at most 64 requests, at most 65536 bytes
per input or output buffer, and at most 512 KiB total requested bytes, including
`direct_input` contents.
Instruction, observation, guest-memory, and time budgets apply across the
entire scenario. The 1 MiB arena also holds objects and metadata, so an image
can exhaust model memory before consuming the maximum scenario buffers.

## Microsoft sample acceptance check

The opt-in [validation script](../scripts/validate_windows_driver_sample.py)
downloads Microsoft's SIOCTL source at the revision pinned in the
[validation manifest](../unittests/emulation/fixtures/sioctl-validation.json),
verifies SHA-256 hashes, and compiles the unmodified source against MinGW-w64
DDK headers. It retains upstream license/provenance, build commands, scenario,
and reports in the selected output directory. It requires network access,
Clang, `lld-link`, `nm`, and MinGW-w64's DDK headers:

```bash
python3 scripts/validate_windows_driver_sample.py \
  --neverd build-release/bin/neverd \
  --output build-release/driver-validation/sioctl
```

Use `--headers` for a non-default MinGW-w64 include directory. The script
generates an MS COFF import library from the compiled object’s dependencies.
The check runs separate buffered, in-direct and out-direct scenarios through
DriverEntry, create, IOCTL, cleanup, close, and unload. Add `--debug` and select
a separate output directory to compile with `DBG=1` and verify guest log messages. The upstream sample does not register a cleanup handler;
the modeled default therefore completes cleanup with
`STATUS_INVALID_DEVICE_REQUEST` (`0xC0000010`). The driver still closes and
unloads, and the successful IOCTL returns the expected bytes. For this complete
scenario, the CLI's expected exit code is **2** and `scenario_success` is false.
The script itself succeeds only when all those outcomes match, including the
visible cleanup failure; it does not rewrite the sample to hide that result.

## Reports and SDK

The JSON report distinguishes `stop_reason`, nullable `nt_status` and
`nt_success`, the stopping PC, and instruction count. It preserves the API
calls and observable state collected before a stop, including device objects
and driver callback addresses. Guest addresses are hexadecimal strings so
JSON consumers do not lose 64-bit precision.
The `configuration` object records the run's limits, service name and
`kernel_exports` overrides.
The profile is `wdm-x64-synchronous-v2`. `nt_status` remains the DriverEntry
result, while `scenario_success` describes initialization and completed
requests together. `phase`, `requests`, and `unload_completed` identify which
parts of the requested lifecycle ran. Each API call and CPU write also records
its phase (`driver_entry`, `request:N`, or `unload`). Each request reports
dispatch and I/O statuses, completion, information length, and returned `output_hex` bytes.
`preferred_image_base` describes the original PE base. `security_cookie` is the
guest address of the initialized cookie, or `"0x0"` if none was required.
Request fields are `kind`, `device`, `file`, `byte_offset`, `code`, `irp`, `completed`,
`dispatch_status`, `io_status`, `information`, and `output_hex`.

The nullable `fault` object preserves the first backend fault. Its `kind`, `pc`,
nullable `address`, `size`, `access` and `interrupt` distinguish unmapped or
protected memory, invalid ranges, invalid instructions and CPU exceptions.
Addresses use hex strings; sizes and interrupt vectors use integers. Observation
reads cannot replace the original fault. A faulted backend cannot resume, and
this record does not imply guest SEH handling.

`instructions` counts admitted guest instruction attempts. An instruction
rejected by the execution policy is not counted; an admitted instruction that
faults in the CPU is counted. Synthetic API dispatch and the return sentinel
do not increment this counter.

Each `writes` entry has `semantics: "attempted_guest_write"`: it records a CPU
write attempt outside the stack, including an attempt that may subsequently
fault or be stopped by a budget. It does not guarantee that the write completed
and does not include writes made by API models. Device and driver-object
snapshots describe the state observed when execution stops.

Include `neverd/sdk/NeverDCAPIEmulation.h` (or the C API umbrella), create a
session, and call `neverd_emulate_driver_json(session, path, options)`.
An explicit nonempty path enters strict execution preflight directly, without
first loading through the general analysis API. The CLI uses this route.
Passing `NULL` options selects
the defaults. Explicit `neverd_driver_options_v1` options require the exact
`struct_size` and positive instruction, memory, event, and timeout budgets.
Free the result with `neverd_free_string`.

Passing `NULL` for the path instead requires a loaded session and reparses its
file independently of IR analysis and function-restricted loading. Both routes
preserve the session image. Keep the input file available and unchanged for the
duration of the call. Request/setup failures return `NULL`
and set `neverd_last_error`; execution stops return JSON. The API remains
available in disabled builds and reports how to enable the feature.

`neverd_emulate_driver_scenario_json(session, path, scenario_json, options)`
uses the same v1 options and ownership rules while adding strict scenario
input. A non-NULL NUL-terminated JSON string is required. The original
`neverd_emulate_driver_json` ABI remains unchanged and initialization-only.
The C++ parser `driverOptionsFromScenarioJSON` supplies the same scenario
validation to callers of `emulateDriver`.

The internal C++ entry point is `neverd::emulation::emulateDriver` in
`include/neverd/emulation/DriverSession.h`. Format parsing belongs to the
existing loader; Windows object/API behavior belongs to `lib/emulation/windows`;
CPU state and execution belong to the Unicorn adapter. The adapter and model
use the same guest-memory interface. No Windows API behavior belongs in the
Unicorn fork.
