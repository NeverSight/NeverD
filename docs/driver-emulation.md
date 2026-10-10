**Languages**: [English](driver-emulation.md) | [简体中文](zh-CN/driver-emulation.md) | [繁體中文](zh-TW/driver-emulation.md) | [日本語](ja/driver-emulation.md) | [한국어](ko/driver-emulation.md) | [Français](fr/driver-emulation.md) | [Deutsch](de/driver-emulation.md) | [Español](es/driver-emulation.md) | [Italiano](it/driver-emulation.md) | [Русский](ru/driver-emulation.md) | [العربية](ar/driver-emulation.md)

[← Documentation index](README.md)

# Windows driver emulation

NeverD's optional driver emulator executes a supported x64 WDM driver's PE entry point and can exercise an explicit request scenario before unloading it. The CLI selects KVM on matching Linux hosts and WHP on matching Windows hosts through `auto`; the original C++ defaults and V1 API retain Unicorn. Every transport uses NeverD's bounded Windows environment model. Guest API calls are handled by that model, and the driver remains isolated from the host kernel.

## Execution backends

`driver-strict` supports KVM on matching Linux x64 hosts and WHP on matching Windows x64 hosts; `auto` selects that native transport, and cross-ISA execution selects Unicorn. Explicit Unicorn and the original V1 API retain the portable software profile. Native execution checks canonical addresses and instruction effects before entry; unavailable hardware fails without fallback. Unsupported instructions and OS behavior remain explicit errors. Native Windows x64 CI with Unicorn disabled passes all 359 required checks: 131 CPU checks, 224 driver outcomes from 26 built-in images, 46 WDK images and 40 scenario cases at both preferred and relocated bases, plus four SEH boundary checks ([`9d4c130c`](https://github.com/NeverSight/NeverD/actions/runs/36981864458)). Native ARM64 runtime evidence is still pending, and this does not establish arbitrary-driver or Android/Darwin compatibility.

The native acceptance above covers the declared driver entry points and published scenarios. Detailed per-feature regressions and C API/CLI/Python checks described below retain their Linux-only evidence scope unless Windows execution is explicitly recorded; passing the native corpus does not extend evidence to every test variation.

`DriverImage.def` declares strict PE size/alignment limits and diagnostic text; pointer widths come from `DriverProfile.def`. `DriverImage.cpp` owns validation and relocation, with unchanged accepted images and error messages.

The checked profile validates each instruction and its memory accesses before
single stepping. It preserves Windows object access checks and write observers,
shared RAM aliases, and CPU-only contexts. Scalar memory arithmetic, naturally
aligned locked arithmetic, SETcc and `BT/BTS/BTR/BTC` retain read/write checks;
native execution owns their flags. Legacy SSE/SSE2 moves, logical operations,
MOVLHPS/MOVHLPS and masked scalar CVTTSS2SI/CVTTSD2SI/SUBSS/SUBSD are admitted.
All sixteen XMM registers and MXCSR survive entry and context restoration. Unmasked SIMD faults require native KVM/WHP `precise_simd_exceptions`.
Full-width XMM stores offer two ordered eight-byte write observations before
either word changes. Ordinary RAM operands may cross mapped pages, including
aliases of nonconsecutive physical pages. The whole operand must pass permission
checks before any architectural store; a failing page is reported at its first
inaccessible byte. Misaligned aligned-vector forms remain unsupported.

Supervisor MMIO admits one aligned 1/2/4-byte scalar move transaction. Device
pages never enter native RAM mappings. MOVS/REP MOVS executes one checked element
at a time, with stop/deadline/budget checks at restart boundaries. A device
source must offer a pure prepared read so a destination observer can stop before
the device read commits. Windows register banks implement this preparation;
other devices without it reject string reads before effects. RAM elements may
cross pages; faults preserve completed elements and the faulting element's
restart registers without a partial store. One element cannot mix RAM and device
pages or split an indivisible device transaction. Device RMW, wide
MMIO, port I/O and unmodeled CPU effects remain unsupported. Register-bank REP
instruction counts can differ from Unicorn's extra zero-count termination hook;
request bytes, device state and write events are compared independently.
This supervisor contract does not provide a user-process environment.
Timeout and cancellation are checked between admitted, bounded instructions;
this path does not provide a general asynchronously preemptible VM runner.
No session is restarted on another backend after guest execution begins.

`NEVERD_EMULATION_BACKEND_UNICORN`, `NEVERD_EMULATION_BACKEND_KVM` and
`NEVERD_EMULATION_BACKEND_WHP` control the adapters. Windows APIs are loaded
dynamically from the system DLL. KVM requires access to `/dev/kvm`; the emulator
does not change host permissions.
Cross-compilation is not runtime evidence. The C API adds `neverd_emulate_driver_backend_json`;
the existing v1 structure and entry points remain unchanged. New selection
reports identify the requested/selected backend, execution contract and reason.

KVM transports complete x87 and XMM/MXCSR state using the standard XSAVE interface, including
the FP/SSE presence bits. The older FPU register interface is insufficient for
this state contract. See the [KVM API](https://docs.kernel.org/virt/kvm/api.html)
and [WHP register API](https://learn.microsoft.com/en-us/virtualization/api/hypervisor-platform/funcs/whvvirtualprocessordatatypes).

Checked x64 also admits masked legacy `ADD`, `SUB`, `MUL`, `DIV`, `SQRT`, `MIN` and `MAX` in `SS`, `SD`, `PS` and `PD` forms. `X64SSEInstructions.def` owns operand widths, alignment and admission. `MaskedSSEArithmeticMatchesIndependentHostExecution` compares register and RAM forms against an independent host CPU oracle, including all four rounding modes, FTZ, signed zero, subnormal inputs and NaNs; `SSEMemoryObserverStopsBeforeResultAndStatusChanges` verifies cancellation before effects.

```bash
build-release/bin/neverd emulate-driver path/to/driver.sys \
  --backend auto --execution-contract checked-x64-v1
```

ARM64 CPU execution is available through the independent [C++ CPU interface](architecture.md#cpu-execution).
This driver CLI still requires the x64 Windows ABI and rejects ARM64 driver images.

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
must be positive. An exhausted execution budget stops with partial observations; allocation APIs retain their documented shortage return contracts, including MMIO mapping NULL below.

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
Acceptance also covers Pavel Yosifovich's unmodified Zero WDM sample, including
direct READ/WRITE, atomic statistics and a statistics IOCTL. This does not
establish compatibility with arbitrary third-party drivers.

| Driver class or requirement | Current scope | Missing environment |
|-----------------------------|---------------|---------------------|
| x64 software WDM driver using the listed APIs | Bounded x64 WDM initialization, buffered/direct/neither requests, work items, timers, DPCs, events and waits, with behavior reports and limits | Each additional executed API must have a defined model |
| `METHOD_BUFFERED` IOCTL | Buffered/direct I/O with work-item or DPC completion; pending WDM requests may overlap across files or on one asynchronous file | Only the API subset below; synchronous files remain serial |
| `METHOD_IN_DIRECT`, `METHOD_OUT_DIRECT` | Request-owned MDLs, system mappings and shared physical page identities | User mappings and DMA interfaces outside the subset below |
| Driver-allocated MDLs | Standalone or IRP-associated descriptors, partial MDLs, shared user/system mappings, per-process VA reuse and completion-time release | Quota, hand-built MDLs and arbitrary virtual-memory allocation |
| READ/WRITE | Buffered/direct/neither I/O with work-item or DPC completion; pending WDM requests may overlap across files or on one asynchronous file | Only the API subset below; synchronous files remain serial and no implicit file position is modeled |
| WDM `METHOD_NEITHER` | Separate user input/output VAs, access probes, catchable user-memory faults, driver-created user MDLs, synthetic requestor identities, bounded work-item process attachment, post-dispatch VA revocation or process exit and cancellation | User mappings and address reuse are limited to the documented MDL view arena |
| KMDF 1.33 non-PnP driver | Binding, objects/contexts, named control devices, manual, sequential and parallel default queues, nondefault manual and automatic queues, and buffered/direct/neither requests with executed callbacks | No PnP devices, queue power transitions, class extensions or UMDF |
| KMDF 1.33 PnP FDO | `EvtDriverDeviceAdd`, FDO/PDO identity, default or explicit power-managed I/O queues, common PnP minors, hardware/D0, self-managed I/O and request stop/resume callbacks, and framework cleanup | Unmodeled resource types, multi-component KMDF policy and general KMDF PnP |
| PnP bus/function/filter driver | Explicit resource-free/register-bank PDOs, guest AddDevice and eight common PnP lifecycle minors | Other PnP operations, general power policy and other hardware/resources |
| Storage, network, display, filesystem and minifilter drivers | Unsupported subsystem contracts | Port/class/miniport frameworks, NDIS/WFP, graphics or filesystem services |
| Work items, timers, DPCs, events and waits | The current execution IRQL is `PASSIVE_LEVEL` for dispatch and workers, and `DISPATCH_LEVEL` for DPCs and WDM cancel callbacks | Only the API subset below; concurrent requests require explicit bounded batching |
| Driver using process/thread callbacks, handles, registry/file operations or kernel-module discovery | Configured registry supported; other behavior limited to the listed APIs | Object manager, system state and callback/event producers |
| Hardware, DMA, PCI, interrupt or virtualization driver | Explicit register banks, MMIO, exclusive/shared latched/level interrupts and bounded coherent common/SG/channel DMA | Other device models, host physical RAM, PCI, ports, other DMA interfaces and privileged CPU state |
| x86 or ARM64 Windows driver | Rejected | Architecture-specific loading, ABI and execution model |
| x64 CFG | Validated target tables and check/dispatch calls; inactive instrumentation retains guest fallbacks | XFG, export suppression, unsupported load configuration and TLS remain rejected |

An unused unsupported import can remain bound. Reaching an unsupported operation
stops with a diagnostic and the observations collected so far. A successful
DriverEntry alone does not prove that later dispatch, hardware or framework
paths are supported. The API table below is the authoritative supported subset.

## Execution contract

The profile models an x64 WDM lifecycle on CPU0 with deterministic cooperative scheduling by default. Execution begins at the PE entry point, retaining a compiler's entry wrapper
when present. DriverEntry must return `STATUS_SUCCESS` to initialize; a
nonzero successful or pending status stops as an unsupported initialization
contract. A failing status is retained as a completed initialization result.
All objects, strings, stacks, function pointers, and allocations live in guest memory. The model provides a `DRIVER_OBJECT` and a registry path
for the configured service name (default `NeverDDriver`).
The adapter uses Unicorn’s virtual TLB mode to preserve guest virtual addresses,
including canonical high kernel addresses, without synthesizing Windows page
tables. The initial RFLAGS value is `0x202`; the software-device profile uses a
fixed 64-byte cache line. These are explicit properties of this execution scenario.
Inline x64 CR8 reads observe the current execution IRQL; CR8 writes and other
control-register operations remain unsupported.

Only an absolute, read-only 8-byte access to `GS:[0x188]` exposes the current logical thread’s borrowed opaque identity. Compiler forms such as `MOV` and `CMP` execute as the original backend instruction, with their original register and flag effects. Nested callbacks on that thread see the same object; unrelated threads remain distinct, and modeled system threads use their existing object. The private processor view supplies this field without modeling a complete KPCR or granting thread-structure access or reference ownership. Other GS offsets, all FS accesses, indexed or partial-width reads and stores remain unsupported.

Unknown imports bind to lazy traps. An unused import does not prevent execution;
executing its thunk or reading an unmodeled exported data value stops with
`unsupported_api`. Unsupported CPU environment effects also stop explicitly.
NeverD does not replace unimplemented calls with success values. Malformed
images or unsupported loading requirements fail before execution.

Queued `DelayedWorkQueue` workers execute at `PASSIVE_LEVEL`; guest DPC
callbacks execute at `DISPATCH_LEVEL` with the documented four arguments.
By default, scheduling is deterministic and cooperative on CPU0, at returned-call and
blocking-wait boundaries. Relative, absolute and periodic timers use virtual
time, advancing to the next timer, wait or cancellation deadline when no frame
can run.
Notification and synchronization events/timers retain their distinct
signal-consumption behavior. Each callback has a separate guest stack;
multiple blocked frames retain their locals and complete CPU contexts while
guest memory remains shared. Win64 callback entry places the first four
arguments in registers and additional arguments on the stack. A dispatch that
marks an IRP pending must return `STATUS_PENDING`. By default it completes
before the next request starts. A WDM request or a request on a supported
parallel KMDF default queue may set
`defer_callback_drain: true` to submit another request on an independent file
or the same asynchronously opened file before queued callbacks run. The next request without this flag drains all
queued callbacks and finalizes the batch; a final flagged request drains at
the end of the scenario. The flagged dispatch must actually leave its IRP
pending. A sequential KMDF queue does not deliver another request while its
previous request remains driver-owned. Synchronous-file overlap, arbitrary preemption and external
request arrival remain unsupported. No available producer for a
pending request or infinite wait causes a stalled `model_error`. Shared
instruction, memory, observation and wall-clock budgets still apply.

This is a bounded scheduling model, not full Windows asynchronous support.
Alertable or user-mode waits, APCs,
arbitrary concurrent scenario-submitted IRPs,
UMDF, general KMDF PnP/power and queue scheduling, full PnP/power, general hardware, other DMA interfaces and other interrupt modes remain unsupported.
Initialization-only calls execute explicitly queued callbacks without
inventing requests or unload.

Images use their preferred base unless a scenario selects a valid relocated
address, and must be PE32+ x64 executables with the native subsystem. Imports
may come from `ntoskrnl.exe`, `ntkrnlmp.exe`, `HAL.dll` or `WDFLDR.SYS`.
The execution loader supports validated x64 `DIR64` base relocations and a
limited security-cookie load configuration, initialized before the entry wrapper
with a deterministic guest cookie. Other unmodeled load-configuration
fields, TLS, delayed/bound imports, ordinal imports, and managed images are
rejected. Images must also pass strict range and alignment checks.

Active Control Flow Guard (CFG) validates PE flags, pointer slots and sorted executable target entries. Check and dispatch helpers admit only declared image entries or registered API thunks, preserve the Win64 call state and reject undeclared targets. Instrumentation without active CFG preserves the original guest fallback pointers. Active XFG, export suppression and other unmodeled guard policies remain rejected; executable memory alone does not make an address a valid target.

WDM stacks can contain several device objects owned by the same guest driver. `IoAttachDeviceToDeviceStack` attaches an isolated source above the target's current top and returns that previous top; it sets `StackSize` and `AlignmentRequirement` without changing the driver's `NextDevice` list or copying buffer flags. `IoDetachDevice` takes the saved lower device and requires `PASSIVE_LEVEL`; attachment permits IRQL up to `DISPATCH_LEVEL`. Opening a named lower device dispatches to the current top, while `FILE_OBJECT.DeviceObject` and the report retain the named device identity. READ/WRITE buffering uses the selected top's flags. Captured request routes retain every device through dispatch return, including after detach/delete; internal holds do not increase the open-handle `ReferenceCount`.

`IofCallDriver` and the `IoCallDriver` helper invoke the exact target on that retained route. Real inline `IoCopyCurrentIrpStackLocationToNext`, `IoSkipCurrentIrpStackLocation` and `IoSetCompletionRoutine` operate on the original guest IRP; cursor, count and control flags are validated. Lower dispatch returns its actual status independently of `IoStatus` and completion-routine return values. Completion advances the cursor, selects callbacks using success/error/cancel flags and carries pending state upward; a completion routine owns its pending propagation, including after an earlier `STATUS_PENDING` dispatch return. `STATUS_MORE_PROCESSING_REQUIRED` stops unwinding without retiring the IRP, MDLs or buffers; later completion resumes it. Nested completion is supported with the required outer stop result, and terminal unwinding retires storage once. Owner-tagged continuations preserve nested WDM/WDF caller frames and inherited IRQL. Each consumed lower stack location is cleared before the upper completion callback runs.

`IoAllocateIrp` and `IoFreeIrp` support caller-owned packets through `DISPATCH_LEVEL` with `ChargeQuota=FALSE`. First `IoCallDriver` validates the packet and captures the exact lower device route. This bounded path supports kernel `IRP_MJ_INTERNAL_DEVICE_CONTROL` with `METHOD_NEITHER` buffers owned by the driver; it does not create a user file or copy user buffers. The guest executes real completion and cancel routines. Freeing the packet inside its completion requires `STATUS_MORE_PROCESSING_REQUIRED`; packet access ends immediately, while dispatch and callback metadata remain until their frames return. An unsent packet can be freed directly; leaked, double-freed, foreign or still-owned packets fail explicitly. Arbitrary packet reuse, other caller-created request formats, `IoBuildDeviceIoControlRequest` remain unsupported.

Observed caller requests use `kind: "internal_ioctl"`, `origin: "driver_allocated_irp"` and `file: null`, preserving `code`, `irp`, dispatch/I/O status, cancellation time and exact `information_hex`. Native and JSON scenarios reject `internal_ioctl` before image loading; only actual guest allocation and dispatch create these rows.

Outside the USB idle protocol below, the lower target must be a live guest WDM device; submit with a completion routine enabled for success, error and cancellation. The genuine `driver_wdm_owned_irp.c` fixture uses `NEVERD_WDM_OWNED_IRP_FIXTURE` / `NEVERD_WDM_OWNED_IRP_CFG_FIXTURE`. The [caller-owned IRP scenario](examples/driver-owned-irp-scenario.json) exercises delayed completion. Normal/active-CFG images run at preferred/rebased addresses; C API/CLI checks preserve independent kernel-origin rows. Missing artifacts skip explicitly; execution evidence remains Linux-only.

The example deliberately cancels one child: CLI exit code 2 and `scenario_success: false` are expected with `stop_reason: "returned"`, clean unload and zero fixture failures. Completed kernel output may be snapshotted in `output_hex`; the buffer remains owned by the driver.

Additional PnP minors and other hardware/resource models remain unsupported. Other WDF target forwarding, attaching to stacks with live files or callbacks, detaching an intermediate layer, changing the forwarded major function and targeting devices outside the captured route fail explicitly. Optional genuine-WDK `driver_wdm_stack.c` images use `NEVERD_WDM_STACK_FIXTURE` and `NEVERD_WDM_STACK_CFG_FIXTURE`; native and C API/CLI coverage includes relocation, with missing artifacts explicitly skipped. Execution evidence remains Linux-only.

A scenario can explicitly configure `pnp_devices` (at most 64). Every entry requires `id`, `bus: "resource_free"` or `bus: "register_bank"`, `initial_device_power: "D0"` and `initial_system_power: "working"`; omitted facts are not guessed. IDs are case-sensitive ASCII, 1–64 bytes, beginning with an alphanumeric character and otherwise containing only alphanumerics, `_`, `-` or `.`. Ordinary requests may select a configured `device_id` instead of `device`; the two selectors are exclusive. A `kind: "pnp"` request requires `device_id`, `minor` and `bus_completion`. Supported minors are `start`, `query_remove`, `cancel_remove`, `remove`, `query_stop`, `stop`, `cancel_stop`, and `surprise_removal`. `bus_completion.status` is required as a 32-bit integer or hexadecimal string; optional `delay_100ns` is a nonnegative integer at most INT64_MAX, measured from actual provider receipt. `STATUS_PENDING` is not a final bus status; stop/cancel-stop/surprise-removal/cancel-remove/remove require exactly `STATUS_SUCCESS` (0). PnP requests reject file, transfer and cancellation fields, even when zero. The C++ API applies the same preflight.

```json
{
  "pnp_devices": [
    {"id": "sensor0", "bus": "resource_free", "initial_device_power": "D0", "initial_system_power": "working"}
  ],
  "requests": [
    {"kind": "pnp", "device_id": "sensor0", "minor": "start", "bus_completion": {"status": "0x0", "delay_100ns": 10}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "query_stop", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "cancel_stop", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "query_stop", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "stop", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "start", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "query_remove", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "cancel_remove", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "surprise_removal", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "remove", "bus_completion": {"status": "0x0"}}
  ],
  "unload": true
}
```

After successful DriverEntry, `AddDevice` runs once per configured PDO with a separate provider-owned `DRIVER_OBJECT`; the guest cannot delete or impersonate provider objects. PnP IRPs are `KernelMode` and file-free, initially `STATUS_NOT_SUPPORTED`. The bus response is consumed only if forwarding reaches that PDO; delayed completion uses the shared virtual clock and existing completion continuations. Final upper completion commits or rolls back lifecycle state independently of the bus status. Normal removal from Started requires a successful query, closed files, drained prior requests and guest detach/delete. Clean AddDevice failure retires only the provider; leaked new guest devices cause `model_error`, including detached devices. All providers must be absent before unload. This does not implement other PnP minors, general hardware/resources or the broader KMDF PnP contract. PnP success requires actual provider completion; an early START/QUERY_STOP/QUERY_REMOVE failure may retain null bus observations. Device/file lifecycle identity persists after detach.

Reports retain the initial inventory in `configuration.pnp_devices`. Observed `pnp_devices` entries contain `id`, `pdo`, nullable `add_device_status`, current `attached`, `pnp_state` and `provider_present`; removal leaves `attached` false. AddDevice phases are `add_device:<ID>`, and failures contribute to `scenario_success` without replacing DriverEntry `nt_status`. Each request adds nullable `device_id` and `pnp`; PnP requests have `file: null`. The `pnp` object records `minor`, `state_before`, `state_after`, nullable `bus_status`, `bus_received_at_100ns` and `bus_completed_at_100ns`. Configured status becomes an observation only at actual bus completion; receipt time is independent. Existing request field types are unchanged.

Optional `parent_id` declares a provider parent by its configured ID. Parents may appear later in the array; omission declares an independent root. Native and JSON preflight reject unknown parents, self-links, cycles and invalid IDs before creating PDOs. This graph does not create WDM attachment or WDF object-parent relationships. Observed `parent_id` and `parent_pdo` retain the declared identity after either provider retires. Child START requires a present, Started parent in physical D0 with no pending lifecycle or power transition. Parent STOP and SurpriseRemoval require children to leave active PnP states and drain pending transitions and WAIT_WAKE; parent REMOVE requires every child provider to retire first. No implicit cascade occurs.

Ordinary CREATE/READ/WRITE/IOCTL/CLEANUP/CLOSE requests reach real guest dispatch while the device exists outside Removing/Removed. The model does not synthesize a failure from Stopped, StopPending, RemovePending or power state: a driver can complete software I/O, reject a request or hold it according to its own code. Pending WDM transfers can overlap only through explicit batching across files or on one asynchronous file; a held IRP with no available producer still cannot be released by a later scenario start or cleanup request, and stops as stalled `model_error`. The closed-file and drained-request requirements before Remove are profile restrictions. `query_stop` with final `STATUS_RESOURCE_REQUIREMENTS_CHANGED` (0x119) is rejected in both scenario preflight and final guest completion because it requests unmodeled resource requery; see [Microsoft’s QUERY_STOP contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/irp-mn-query-stop-device). Stop/restart and surprise-removal do not implement resource rebalance.

Resource-free PnP uses the original genuine-WDK `driver_wdm_pnp.c`, optional `NEVERD_WDM_PNP_FIXTURE` / `NEVERD_WDM_PNP_CFG_FIXTURE`, and native plus C API/CLI tests. Missing artifacts skip explicitly; execution evidence remains Linux-only.

WDM remove locks execute through the actual `IoInitializeRemoveLockEx`, `IoAcquireRemoveLockEx`, `IoReleaseRemoveLockEx` and `IoReleaseRemoveLockAndWaitEx` exports; the unsuffixed WDK names are macros. Each lock belongs to the exact guest DEVICE_OBJECT whose extension contains its full aligned storage, independently of PDO lifecycle or Tag shape. Initialization works before attachment. Retail 32-byte and DBG 120-byte locks are accepted only with the matching separate size argument, and their entire registered storage is opaque. NULL and repeated Tags are counted independently per lock; Tags are never dereferenced, so releasing after IRP completion remains valid. Initialization and AndWait require `PASSIVE_LEVEL`; acquire/release permit `DISPATCH_LEVEL`.

AndWait closes admission, releases one matching acquisition and suspends the actual guest frame until all remaining acquisitions drain. Later acquire returns `STATUS_DELETE_PENDING` without adding an obligation. The final release latches readiness before the releasing callback returns; a worker can release and then wait for the resumed REMOVE path to signal it. There is no synthetic callback, timeout or successful result without a producer. This profile requires an associated active REMOVE route containing the owner and actual provider receipt (`bus_received_at_100ns` may be zero); lower completion is not required. A lower driver that queues REMOVE before provider receipt remains outside this bounded check, which does not claim full OutsideRemoveDevice/Driver Verifier enforcement. The public runner still requires closed files and drained earlier requests before REMOVE, but permits outstanding callbacks that release the locks. The retained REMOVE route survives drain, guest detach/delete and any pending lower completion; remaining callback frames must return before final route retirement.

Unknown/mismatched storage, unmatched releases, duplicate drain, reinitialization and deleting an extension with acquisitions or an unconsumed drain wait fail before mutation. An initialized unused lock can be deleted during clean AddDevice failure. Lock acquisitions do not replace real device/work-item references; lock storage unregisters only at physical extension retirement. Valid debug metadata does not enable verifier timeout/high-water behavior. The original genuine-WDK `driver_wdm_remove_lock.c` uses optional `NEVERD_WDM_REMOVE_LOCK_FIXTURE` / `NEVERD_WDM_REMOVE_LOCK_CFG_FIXTURE` / `NEVERD_WDM_REMOVE_LOCK_DBG_FIXTURE` / `NEVERD_WDM_REMOVE_LOCK_DBG_CFG_FIXTURE`; missing artifacts skip explicitly. Native and C API/CLI evidence remains Linux-only and does not establish a complete remove manager or general concurrent I/O draining.

The synthetic `bus: "register_bank"` adds explicit fixed memory resources to a PDO. When present, its `resources` array contains `id`, `raw_start`, `translated_start`, `length` and `registers`; every register requires `offset`, `width`, `access` (`read_only` or `read_write`) and initial `value`. `DriverResources.h` / `DriverResources.def` define the shared C++ and JSON contract. Resource IDs follow the bounded ASCII identifier rules and are unique within each PDO. Limits are 8 resources per PDO / 32 total, 256 registers per resource / 4096 total and 1–1048576 bytes per resource. Only naturally aligned exact 1/2/4-byte register accesses are supported; values must fit that width. Both physical intervals must be overflow-free; raw ranges cannot overlap within a PDO, and translated ranges cannot overlap globally. Empty `registers` explicitly leaves the whole bank inaccessible. Addresses and initial values are declarations, never host hardware or default zero-filled memory. `resource_free` retains an omitted resource inventory and null START pointers.

START receives separate read-only raw and translated `CM_RESOURCE_LIST` allocations, with corresponding ordered Memory descriptors: one full descriptor, Internal interface, bus 0, version/revision 1, DeviceExclusive sharing and READ_WRITE range flags. Per-register RO remains independent. A successful lower START makes the assignment available before upper completion callbacks; each START from NotStarted/Stopped creates a new resource epoch with the same fixed assignment. Register values initialize from the PDO configuration and survive unmap, STOP, restart and D3hot. Failed START and successful STOP/REMOVE require the driver to release mappings before terminal IRP completion; upper completion callbacks can unmap first, and mappings are never silently cleaned up. Surprise removal immediately prevents new maps and register access, while existing mappings can still be unmapped. Actual successful provider device-SET completion changes hardware accessibility: D2 and D3 block accesses, D0 permits them only with an available assignment; Both low-power states still permit mapping without register access, and power changes retain mappings. An explicitly supported D3cold cycle restores configured register values before the next D0 access.

`MmMapIoSpace` supports NonCached; `MmMapIoSpaceEx` supports PAGE_NOCACHE with PAGE_READONLY or PAGE_READWRITE. Both accept only a declared translated subrange within one assignment, preserving its page offset. Aliases share one bank, retain independent permissions and require exact original base/length for `MmUnmapIoSpace`. Mapping/window or configured memory-budget exhaustion returns NULL; backend faults remain explicit failures. Unmapped addresses never regain access through a later mapping. Scalar and real REP register-buffer instructions execute through CPU MMIO checks; holes, wrong widths, misalignment, RO writes, cross-mapping accesses and execution fail before register effects. Modeled API memory accesses must also fit one aligned 1/2/4-byte transaction; larger bulk spans fail instead of being split into register accesses. This does not implement arbitrary physical RAM, resource rebalance, ports, other interrupt modes, other DMA interfaces or general hardware behavior.

The runnable [register-bank scenario](examples/driver-register-bank-scenario.json) uses the original genuine-WDK `driver_wdm_resources.c` through optional `NEVERD_WDM_RESOURCE_FIXTURE` / `NEVERD_WDM_RESOURCE_CFG_FIXTURE`. It executes 14 requests across delayed START, file I/O, STOP, restart and removal, and observes persistent values through the driver's IOCTL. `configuration.pnp_devices[].resources` records initial facts with lossless hexadecimal physical addresses; it is not a second bank-state report. C API and Python continue using the existing `scenario_json` entry and unchanged `neverd_driver_options_v1`. Missing genuine artifacts skip explicitly; current execution evidence is Linux-only.

The same `register_bank` provider can declare `interrupts` alone or alongside
`resources`; at least one list must be nonempty. `resource_free` rejects an
explicit `interrupts` field, including `[]`. Each interrupt requires `id`,
`raw_vector`, `raw_level`, `raw_affinity`, `translated_vector`,
`translated_level`, `translated_affinity`, `mode` (`latched` or
`level_sensitive`) and `share` (`device_exclusive` or `shared`). A level source
also requires positive `retrigger_after_100ns`; latched sources reject that
field. `DriverInterrupts.h` / `DriverInterrupts.def` own the spellings and
limits: 8 interrupts per PDO / 32 total, per-PDO unique bounded ASCII IDs, raw
level 0–65535 and translated level 0 (passive) or DIRQL 3–12. Both affinity masks must be 1, making
CPU0/group0 explicit. Vectors retain all 32 bits without a guessed IRQL
relation. A translated vector can be shared only with matching level,
affinity, mode and sampling period. Memory descriptors precede ordered
interrupt descriptors, whose sharing and level/latched flags reflect the
assignment. This models declared synthetic lines, not a PCI controller.

READ/WRITE/IOCTL requests may declare `interrupt_events`, each with explicit
`after_100ns`, `device_id` and `interrupt_id`. Optional `action` defaults to
`pulse` for existing latched scenarios. Level sources require `assert` or
`deassert`; latched sources require `pulse`. Other request kinds reject the
field even when empty. At most 64 events per request / 1024 total are accepted,
with nonnegative delay through INT64_MAX. Successful submission captures the
live connections and PDO resource epoch. Source IRP completion does not cancel
an event. By default, due events run at supported callback boundaries and time
advances only while idle. The `scheduling` policy also advances deadlines during
instruction execution; arbitrary interrupt nesting remains unsupported. Provider hardware publication
precedes eligibility, and ISRs precede DPCs/workers according to assigned DIRQL.
A lost connection, stale/unavailable epoch or physical D3 records an
`undelivered_reason` and stops without rebinding the event.

A shared latched pulse invokes every captured ISR in registration order, even
when an earlier handler claims it. Level lines retain one asserted state per
PDO/source/epoch; the shared physical line is the OR of those sources. All
same-time source changes apply before sampling. A still-asserted line is
sampled again after its declared interval, so a repeated assertion does not
invent an edge. Claiming a level interrupt stops that sample's handler search
but does not acknowledge the device or clear any source. Only explicit
`deassert` changes that source's state. A session permits at most 4096 delivery
batches, in addition to shared callback and instruction budgets; exhaustion
fails before another delivery. Disconnect and device retirement reject a
still-asserted source. Register writes never infer an enable or acknowledgement
protocol.

`IoConnectInterrupt` uses its eleven arguments and the exact translated
assignment. `IoConnectInterruptEx` supports FullySpecified (1), LineBased (2,
one assigned line on the explicit PDO) and FullySpecifiedGroup (4, group 0);
disconnection requires the matching version/context. Registration and
disconnection require PASSIVE_LEVEL. A private lock or an initialized
caller-provided nonpaged lock is supported. Multiple connections can share
the caller's lock with the same synchronization IRQL, which must be at least
each assigned DIRQL; it does not change scheduler priority. LineBased zero
selects the assigned level. CPU0/group0 and no floating-state save remain
required. `KINTERRUPT` is opaque. The real ISR receives
`(Interrupt, ServiceContext)` and returns BOOLEAN from AL; FALSE is a valid
unclaimed observation. `KeSynchronizeExecution` runs the actual callback under
the same lock at synchronization IRQL, restoring caller IRQL/CR8 afterward.
Manual acquire/release enforce nonrecursive ownership, original execution and
saved IRQL. Connected locks cannot be accessed through executive spin-lock
APIs or outlive their storage. DIRQL waits, floating-state save for ISRs and arbitrary interrupt nesting
remain unsupported. Failed START and successful
STOP/REMOVE require disconnection before terminal completion; disconnect never
silently discards queued DPCs.

Message assignments use a nonempty `messages` array with explicit
`message_address`, `message_data`, `translated_vector`, `translated_level`,
`translated_affinity` and `polarity` for each message. The descriptor's first
translated tuple must match its first message; messages within one descriptor
share an address and use latched mode with raw level zero. Separate descriptors
may use different addresses. A pulse selects descriptor-local `message_id`;
`CONNECT_MESSAGE_BASED` (3) registers the PDO's full message set and passes the
PDO-wide message index as the ISR's third argument. The returned read-only
`IO_INTERRUPT_MESSAGE_INFO` exposes actual per-message interrupt identities.
Absent messages use the explicit line fallback and write back version 2, or
return `STATUS_NOT_FOUND` without one. Independent locks retain each assigned
DIRQL; an explicit shared lock or synchronization level uses a validated common
level. No PCI configuration or message arrival is inferred.

`CONNECT_MESSAGE_BASED_PASSIVE` (5) and passive line registrations execute at
`PASSIVE_LEVEL`. Their synchronization event retains execution ownership across
supported waits, serializes repeated arrivals for the same interrupt, and lets
independent passive handlers progress separately. DIRQL handlers run before
DPCs, then passive handlers and workers. Arrival time is recorded separately
from delivery time when synchronization delays a callback. Disconnect validates
the complete message group before retiring any member.

Reports retain declared resources in `configuration.pnp_devices[].interrupts`
and flatten events in `configuration.interrupt_events`. Observed `interrupts`
rows retain `source_request_index`, `event_index`, `device_id`, `interrupt_id`,
`action`, `epoch`, `due_at_100ns`, nullable `occurred_at_100ns`,
`delivered_at_100ns`, `returned_at_100ns`, `interrupt_object`, `return_value`,
`claimed` and `undelivered_reason`. `handlers` records each actual callback,
including its `delivery_index` for repeated samples. Each source retains its
first assertion; a sample is attached to the first currently asserted source
in provider order. Repeated assertions or an assert/deassert pair at one
boundary may have no ISR records. `delivered_at_100ns`
is the first handler entry, while the top-level return fields summarize the
latest delivery batch. A deassertion records its occurrence without fabricating
an ISR return. An unclaimed interrupt remains valid. The runnable
[interrupt scenario](examples/driver-interrupt-scenario.json) uses genuine-WDK
`driver_wdm_interrupts.c` through optional `NEVERD_WDM_INTERRUPT_FIXTURE` /
`NEVERD_WDM_INTERRUPT_CFG_FIXTURE`. Normal/CFG and preferred/rebased tests cover
shared pulses, caller-provided locks and level assertion/repetition/deassertion.
The existing C/Python `scenario_json` boundary and `neverd_driver_options_v1`
layout are unchanged. Missing artifacts skip explicitly; execution evidence
remains Linux-only.

`DriverDMA.h` / `DriverDMA.def` add an optional `dma` object to a `register_bank` PDO, alongside its memory/interrupt assignments; DMA alone does not replace either resource list. All seven fields are explicit: `address_bits` (32 or 64), `maximum_length` (1–1048576 bytes), `map_registers` (1–256), `alignment` (power of two from 1–4096), `logical_base` (nonzero and page aligned), `logical_length` (page aligned, 4096–1073741824 bytes), and Boolean `scatter_gather`. The overflow-free aperture must fit the address width. Each PDO has an independent logical domain, so equal addresses on different devices do not alias. Translated MMIO resources must avoid the reserved model RAM range `[0x1000000000, 0x1004000000)`. These declarations describe a coherent synthetic bus master, never host physical memory or a PCI device.

`IoGetDmaAdapter` accepts the historical `DEVICE_DESCRIPTION` version 0/1 fields for an Internal bus master and publishes a version-one `DMA_ADAPTER` with its actual 104-byte `DMA_OPERATIONS` table. Version 2/3 probes return NULL without reading a modern description tail. Each indirect method is bound to that exact live adapter, independently of kernel imports. Implemented methods are `AllocateCommonBuffer`, `FreeCommonBuffer`, `GetDmaAlignment`, `GetScatterGatherList`, `PutScatterGatherList`, `PutDmaAdapter`, `AllocateAdapterChannel`, `MapTransfer`, `FlushAdapterBuffers` and `FreeMapRegisters`. `FreeAdapterChannel` and `ReadDmaCounter` remain named traps because this profile has no subordinate/system DMA controller. Common-buffer allocation/free and alignment queries require PASSIVE_LEVEL; Get/PutScatterGatherList require DISPATCH_LEVEL, and adapter release permits IRQL through DISPATCH_LEVEL. x64 ignores `CacheEnabled`. Unsupported version probes, incompatible declared capabilities and documented allocation shortages return NULL; malformed or unmodeled interface selections and backend failures remain explicit errors.

System MDL aliases backed by loader-owned image spans admit the known image bytes within the described 4 KiB pages. Access to padding outside the declared image span fails, even when the underlying page is mapped. `ByteOffset` and `ByteCount` retain the logical buffer extent; they are not subpage CPU permission boundaries. Image bytes outside that extent remain shared with the original view, while protection and unmapping apply to the complete mapping. Object-backed owners retain the model's bounded padding restrictions.

`KernelPhysicalMemory` assigns at most 16384 model physical pages of 4096 bytes to existing RAM. CPU virtual addresses, physical page identities and device logical addresses are distinct. Built MDL PFN arrays expose these shared identities read-only; unbuilt descriptors have no usable PFNs. Small neighboring allocations may share a PFN but retain separate byte ranges and lifetimes. Common buffers, pool storage and request buffers use the same bytes already owned by `GuestMemory`, without a second DMA copy. A live SG mapping pins its exact data range and descriptor. Completion, pool/MDL free and teardown reject live dependencies before retirement. Unmapping a direct MDL only revokes its CPU system mapping; DMA remains able to reach its locked backing. `DmaWritable` records the write-lock contract separately from CPU mapping permissions: device writes require direct READ/OUT_DIRECT or writable nonpaged storage; WRITE/IN_DIRECT do not gain that permission merely because their CPU mapping is writable.

`GetScatterGatherList` validates CurrentVa/Length against the MDL's original range and creates logical page fragments over its existing backing. Available map registers allow the real four-argument void `AdapterListControl` to run inline before the API returns; otherwise admission retains the data/descriptor and reserves a callback in the PDO's FIFO until resources are released. This profile has no StartIo ownership, so the callback's second IRP argument is NULL. Callback return does not release a mapping. CPU access to a live SG data range requires Put first; a callback waiting for map registers has not yet granted the device ownership of those bytes. `PutScatterGatherList` may run inside the callback; after Put, the driver may complete the request and release the last adapter while its callback continuation and device hold survive until return. Common buffers require their original adapter, length, logical address and CPU address when freed. Logical addresses are never reused during the session, including restart. A resource wait without a real producer stalls explicitly; it does not fabricate a completion or deadline.

`AllocateAdapterChannel` requires DISPATCH_LEVEL and reserves an opaque non-NULL map-register token. Common buffers, SG lists and channel reservations share the same per-PDO quota and FIFO. Success accepts a real immediate or queued `AdapterControl`; an excessive requested count returns `STATUS_INSUFFICIENT_RESOURCES` without a callback. One unfinished allocation callback is allowed per guest device, and calling AllocateAdapterChannel from AdapterControl is rejected, including through a nested callback. The callback's four arguments contain the actual `DEVICE_OBJECT.CurrentIrp` snapshot taken at registration. That exact eight-byte field is writable on guest devices; zero or a live IRP routed to the device is accepted. A queued callback retains that packet until entry, after which the callback may complete it. The profile still has no StartIo; the separate SG callback's unused IRP argument remains NULL.

`AdapterControl` returns a 32-bit `IO_ALLOCATION_ACTION`; high RAX bits are ignored, and the action never replaces AllocateAdapterChannel's `STATUS_SUCCESS`. `DeallocateObject` releases unused or fully flushed registers on callback return. `DeallocateObjectKeepRegisters` retains them until `FreeMapRegisters` uses the exact adapter, token and original count. Returning `KeepObject` requires an unmodeled system controller and fails explicitly. The newly delivered allocation cannot be freed as retained before its callback returns; a different previously retained allocation may be freed when its own contract permits. Callback identity, retained registers and active mapped bytes have separate lifetimes, all checked before adapter or device teardown.

`MapTransfer` and `FlushAdapterBuffers` permit IRQL through DISPATCH_LEVEL; channel allocation and register free require DISPATCH_LEVEL. MapTransfer takes an MDL-relative index, reads and updates an actual ULONG length and returns the logical address by value. The bounded SG profile returns one backing-page fragment per call; contiguous subsequent indices in the same MDL and direction extend one operation. Non-SG maps the entire requested extent in one call if it fits the reserved count, without shortening it. The first map reserves a nonreused logical aperture sized to the register reservation; other allocations can interleave without overlapping it. All fragments share one growing physical pin. Device transactions can span the full currently mapped operation, while CPU access, MDL free and completion that retires pinned storage remain blocked until aggregate flush. Flush must match the initial index, MDL, direction and total actual mapped length. It releases mapped bytes without releasing registers, so the retained token can support another operation. Partial flushes, mixed-MDL operations and other MapTransfer patterns are outside this profile, rather than assumed invalid on every Windows system.

`KeFlushIoBuffers` validates a live locked/nonpaged MDL. The modeled platform is coherent, so either ReadOperation or DmaOperation value needs no separate cache copy; this call does not release DMA ownership or replace FlushAdapterBuffers. The [channel scenario](examples/driver-dma-channel-scenario.json) runs original `driver_wdm_dma_channel.c` through two MapTransfer calls, one cross-page device transaction, a separately declared IRQ/DPC, aggregate flush and exact register release. Genuine normal/CFG images use `NEVERD_WDM_DMA_CHANNEL_FIXTURE` and `NEVERD_WDM_DMA_CHANNEL_CFG_FIXTURE`.

Only READ/WRITE/IOCTL requests accept `dma_events`. Each event requires `after_100ns`, `device_id`, `logical_address`, `direction` and `length`; `write_memory` additionally requires exact-length `data_hex`, while `read_memory` rejects that field. Directions are from the device's perspective. Limits are 64 events per request, 1024 total, 16 MiB of total transaction bytes and 1 MiB per transaction; delay is nonnegative through INT64_MAX. Submission captures the PDO's current assigned epoch and anchors virtual time, but does not require a mapping that the forthcoming dispatch has yet to create. Delivery resolves the complete live logical range and direction, requires physical D0, and validates all backing bytes before any transaction effect. Source IRP completion does not cancel an event. Missing/released mappings, stale epochs, surprise removal or non-D0 power record a failure and stop; events neither rebind nor invent an interrupt, register protocol or IRP completion. At one scheduled boundary, provider hardware publication precedes DMA bytes, which precede independently declared interrupt pulses. The default clock is cooperative; `scheduling` enables instruction-driven deadlines and thread preemption.

Reports retain `configuration.pnp_devices[].dma` and flattened `configuration.dma_events`. Root `dma_transfers` rows identify `source_request_index`, `event_index`, `device_id`, `epoch`, `logical_address`, `direction`, `length` and `due_at_100ns`, with nullable `occurred_at_100ns`, `completed_at_100ns`, `mapping`, `adapter` and `failure_reason`; `data_hex` contains actual transferred bytes. Every declared transaction must finish without failure for `scenario_success`. The [DMA scenario](examples/driver-dma-scenario.json) uses original genuine-WDK `driver_wdm_dma.c` with optional `NEVERD_WDM_DMA_FIXTURE` / `NEVERD_WDM_DMA_CFG_FIXTURE`, exercising a common buffer through actual adapter pointers and a separately declared ISR→DPC completion. C/Python still use `scenario_json` without changing `neverd_driver_options_v1`. Missing artifacts skip explicitly; evidence is Linux-only. Subordinate controllers, V2/V3 methods, hardware descriptor engines, general KMDF DMA and other device models remain unsupported.

WDM power execution uses `kind: "power"` with a configured `device_id`. Every packet explicitly requires `minor` (`query` or `set`), `power_type` (`device` or `system`), `power_state` (`D0`/`D2`/`D3` or `working`/`sleeping3`), `power_action` (`none` or `sleep`), `system_context` (a 32-bit integer or hexadecimal string) and `bus_completion`. System Query to Working is unsupported. Power packets reject file, transfer and cancellation fields. The complete `system_context` value is retained as an opaque packet fact; it does not select a parent or imply hibernate/fast-startup support. Routes require `DO_POWER_PAGABLE` without `DO_POWER_INRUSH`, actual pageable power dispatch runs at `PASSIVE_LEVEL`. `PoCallDriver` forwards the same owned power IRP; `PoStartNextPowerIrp` follows the Vista+ contract without an extra serialization handshake. General power policy, other states/actions, shutdown/hibernate, inrush, nonpageable routes, general hardware remain unsupported.

Query/Set `PoRequestPowerIrp` accepts callers through `DISPATCH_LEVEL`. At APC or DISPATCH it returns `STATUS_PENDING` with an independently owned IRP and reserved power transaction; actual device state remains unchanged until completion. The caller's IRQL and CR8 remain unchanged. The scheduler later runs the real pageable DispatchPower at `PASSIVE_LEVEL`; a DPC wake callback can therefore request D0 directly without allocating a guest work item. PASSIVE callers retain inline dispatch and early completion. Failed admission preserves the response FIFO, result rows, packet storage and scheduler capacity. Completion callbacks retain the IRQL of the actual completing path. WAIT_WAKE issuance still requires `PASSIVE_LEVEL`. See the [elevated power scenario](examples/driver-wdm-elevated-power-scenario.json).

Native WDM drivers may issue `PoRequestPowerIrp(IRP_MN_WAIT_WAKE)` with system `Working` or `Sleeping3`. Issuance requires `PASSIVE_LEVEL`, stable physical D0, no active device/system power transaction and a real successful lower START acknowledgement; it may occur before that START returns to the upper driver. The optional output `PIRP` is written before guest dispatch, and the completion callback is independently optional. Preflight failure preserves the output, result rows and allocation budget. A sent request returns `STATUS_PENDING`, including inline provider rejection. It reports `origin: "PoRequestPowerIrp"` and `response_index: null` without consuming `requested_device_power`. Public scenario power packets remain Query/Set only; native WAIT_WAKE through a framework-owned route is rejected before allocation.

One provider slot retains either a native or framework WAIT_WAKE. Missing/all-false `wake_capabilities` completes `STATUS_NOT_SUPPORTED`; an unsupported system wake limit completes `STATUS_INVALID_DEVICE_STATE`; a duplicate completes `STATUS_DEVICE_BUSY`. Actual provider completion unwinds the ordinary IoCompletion chain, including `STATUS_MORE_PROCESSING_REQUIRED`, before the terminal five-argument void `REQUEST_POWER_COMPLETE` callback. Its independent `IO_STATUS_BLOCK` survives through callback return. Successful upper completion cannot fabricate a provider wake. `IoCancelIrp` calls the installed provider cancel routine under the real cancel-lock protocol and returns TRUE only when that routine ran. Successful delivery separately checks the provider’s independent S0/Sx capability against the actual Working/Sleeping3 state; a Sleeping3 IRP limit does not grant S0 wake.

A native `wake` policy event captures an already retained IRP and its exact successful START identity. Cancel/rearm and STOP/restart cannot retarget that event. Wake records `wake_source_device_id` / `wake_source_pdo` but does not change D0/D2/D3 or Working/Sleeping3; the driver must issue separate power requests. The originating driver cancels and drains WAIT_WAKE and its callbacks before incompatible teardown. Only actual provider-cancelled native WAIT_WAKE with a recorded cancellation counts as expected control flow. Raw WDM child-wake propagation remains unsupported.

Native WDM USB selective idle uses an explicit `usb_idle` provider configuration, independent of `bus`: `role` is `independent_function`, `composite_parent` or `composite_function`. A composite function's immediate `parent_id` must name a composite parent. `remote_wake` defaults to false; true additionally requires declared generic `wake_capabilities`, and an actual USB wake requires D2. Generic D3hot wake facts alone do not enable USB remote wake. A `usb_idle_permission` action in `power_policy_events` targets an independent function or the composite parent, rejects `component`/`state`, and captures every member's retained IRP and successful START identity. Every configured composite function must have a registration; a missing member rejects the whole grant. Cancel/rearm and restart cannot retarget captured permission.

The driver allocates and sends a real `IOCTL_INTERNAL_USB_SUBMIT_IDLE_NOTIFICATION` using kernel `METHOD_NEITHER`, exactly 16 bytes of callback info and no output, at `PASSIVE_LEVEL` in stable S0/D0. It may target the explicit provider directly or forward through its guest FDO. The provider borrows the info until completion; its captured context is opaque and may be NULL. Permission executes the one-argument void callback at PASSIVE. The callback issues one real SET D2 and waits for that request's terminal completion; an observed allocation failure permits cancellation and return. The idle packet then remains pending. Cancellation before entry withdraws the queued callback; after entry it waits for callback return. D0 receipt completes idle successfully before the separate D0 acknowledgement; D3 receipt completes it with `STATUS_POWER_STATE_INVALID`, and S3/removal receipts cancel it. Final IoCompletion uses the existing free/`STATUS_MORE_PROCESSING_REQUIRED` ownership contract.

Request `usb_idle` observations preserve `start_epoch`, actual receipt, callback entry/return, `d2_irp` and D2 outcome, `completion_cause`, claim time and final completion time; unobserved facts remain null. Permission reports `usb_idle_members` with exact device/PDO/IRP/START identities, without claiming a power transition. Only matching actual provider control-flow outcomes count as expected cancellation/D3 invalidation; a duplicate `STATUS_DEVICE_BUSY` remains a scenario failure. The [USB idle scenario](examples/driver-wdm-usb-idle-scenario.json) uses `NEVERD_WDM_USB_IDLE_FIXTURE` / `NEVERD_WDM_USB_IDLE_CFG_FIXTURE`. USB descriptors, URBs, pipes and transfer targets remain unsupported.

KMDF `IdleUsbSelectiveSuspend` supports `DriverManagedIdleTimeout` with explicit D2. `PowerDeviceMaximum` resolves only from an explicit function-level `usb_idle.device_wake: "D2"`; omission remains unknown. The independent `remote_wake` Boolean does not supply that bus fact, and `device_wake` does not enable remote wake. Composite parents reject `device_wake`. USB timeout zero uses the 5000 ms default; user override is disabled and `PowerUpIdleDeviceOnSystemWake` must be `WdfUseDefault`.

Timeout retains a real framework-owned idle IRP in D0. `usb_idle_permission` then drives framework callbacks and a real D2 acknowledgement, including the complete composite group. Remote wake requires both explicit `remote_wake=true` and generic S0 capability, and uses a separate real `WAIT_WAKE`. Managed I/O and StopIdle cancel the old idle packet first; its `framework_usb_idle` row records `cancel` / `STATUS_CANCELLED`, while managed delivery still waits for the independent D0 acknowledgement and D0Entry. Wake without earlier cancellation instead completes idle at D0 receipt. Reports use null file/response-index fields and preserve the same receipt/callback/D2 evidence as WDM. The [KMDF USB scenario](examples/driver-kmdf-usb-idle-scenario.json) uses `NEVERD_KMDF_USB_IDLE_FIXTURE` / `NEVERD_KMDF_USB_IDLE_CFG_FIXTURE`.

USB also supports `SystemManagedIdleTimeout` and `SystemManagedIdleTimeoutWithHint`, assigned before the first D0 entry finishes. The first ignores the driver's timeout; WithHint supplies a PoFx lower bound, not another local timer. `idle` alone grants no device power-down. An explicit `power_not_required` decision retains a real USB idle IRP and acknowledges PoFx while still physically in D0. Separate `usb_idle_permission` is required before the actual callback and D2 request. Activity before that permission cancels idle in D0 without a redundant D0 IRP. After D2, managed I/O and StopIdle wait for real D0 acknowledgement/D0Entry and subsequent F0/ActiveCondition completion. Remote wake restores the component before later I/O. Failed arm or allocation restores activity after real cancellation/disarm; STOP/REMOVE retire the owners and restart obtains new identities. The [USB PoFx scenario](examples/driver-kmdf-usb-pofx-scenario.json) demonstrates the two decisions and independent device/component recovery. If a real `EvtDeviceD0Entry` fails during `RemovePending`, the pending Required acknowledgement may be settled only to complete failed-IRP and hardware cleanup; F0/ActiveCondition are not invoked. This does not permit SET_POWER failure for a present device or add autonomous framework surprise removal.

`WdfDeviceConfigureRequestDispatching` maps READ, WRITE or external IOCTL to a live nondefault queue on the same device at IRQL <= DISPATCH_LEVEL. Automatic queues need the matching callback or `EvtIoDefault`; manual queues use existing retrieval semantics. A duplicate mapping returns `STATUS_WDF_BUSY`. Unmapped requests keep the default queue, presented requests keep their owner, and a mapped queue cannot be deleted independently. `WdfDeviceEnqueueRequest` captures the selected queue; later mappings cannot move an already enqueued request. CREATE and internal IOCTL queue mappings remain explicitly unsupported in this profile; file callbacks and typed USB provider dispatch retain their existing owners. Genuine tests cover both direct READ routing and `WdfRequestForwardToIoQueue`, with delivery after actual D0 readiness.

The [native WAIT_WAKE scenario](examples/driver-wdm-wait-wake-scenario.json) uses the genuine WDK `driver_wdm_wait_wake.c` fixture via `NEVERD_WDM_WAIT_WAKE_FIXTURE` / `NEVERD_WDM_WAIT_WAKE_CFG_FIXTURE`. Tests cover actual START issuance, callback arguments, wake without implicit D0, rearm, cancellation, MPR, DPC cancellation with worker-issued D0, exact event capture and independent providers. Normal/active-CFG and preferred/rebased execution evidence is Linux-only; unavailable artifacts skip explicitly.

D2 preserves its own device-state value throughout dispatch and completion. Native/JSON power packets, `requested_device_power` and `initial_reported_device_power` accept D0/D2/D3; `PoSetPowerState` changes only the calling object's notification history, and D2/D3 notifications require IRQL <= APC_LEVEL. `DeviceLifecycle::validateDevicePowerRequest` rejects a SET directly between different low-power states without changing the pending operation or consuming a ticket. A framework S3 target that differs from the current Dx state uses a real D0 child followed by the selected Dx child, with separate explicit FIFO responses and actual completion before the next step. D0 callbacks receive the exact D2 previous/target state.

S0 idle and Sx wake settings accept explicit `DxState` D2 or D3 and retain the selected target across policy execution; reassignment updates that target. This provider's existing `wake_capabilities` promise wake from D3hot and also cover D2; they do not invent a bus `DeviceWake` value. D1 and non-USB `PowerDeviceMaximum` remain unsupported. D2 blocks MMIO, DMA and ordinary interrupt delivery. The register-bank provider retains its configured registers, mapping aliases, assignments and common RAM through D2; this is a modeled provider contract, not a claim about arbitrary hardware context retention. D2 cannot enter D3cold or advance its reset generation. An idle `DxState` of D2 cannot authorize D3cold even if a later system transition selects D3. Repeating SET D3 for an already cold provider preserves the existing cold generation while completing the real power request.

The [D2 power scenario](examples/driver-d2-power-scenario.json) uses `NEVERD_KMDF_CHILD_WAKE_FIXTURE` / `NEVERD_KMDF_CHILD_WAKE_CFG_FIXTURE`. Native and genuine WDK tests cover D2/D0 packets and callback order/counts, forbidden direct Dx transitions and explicit D0 preparation before system sleep; C API/CLI preserve the same observations. Normal/active-CFG images run at preferred/rebased addresses. Missing fixtures skip explicitly; evidence remains Linux-only. Framework model tests separately inspect exact callback state arguments.

Each `pnp_devices` entry may include `initial_reported_device_power: "D0"` or `"D2"` or `"D3"`, independent of the required initial lifecycle facts D0/working. The provider PDO and each newly associated guest DEVICE_OBJECT receive separate notification state; `PoSetPowerState` returns and updates only the calling device's previous value. Without this explicit seed, a reached `PoSetPowerState` fails instead of assuming D0. Optional `requested_device_power` contains device-type packet templates, with the same six required power facts. At most 64 templates are accepted across all PDOs. A real `PoRequestPowerIrp(Query/Set)` or framework power-policy transition with matching PDO, minor and target consumes that PDO's FIFO head; missing or mismatched entries fail, unused entries create no requests. No parent is guessed from the callback context. Each child owns an independent IRP and result row with `origin: "PoRequestPowerIrp"` and zero-based `response_index`; scenario rows use `origin: "scenario"` and null response index. Synchronous child completion can run its five-argument void callback before `PoRequestPowerIrp(Query/Set)` returns `STATUS_PENDING`; callbacks may wait, and system S0 can complete before the independent D0 child. The callback's IO_STATUS_BLOCK snapshot remains valid through its return.

Request reports add nullable `power` alongside `pnp`; power rows have `file: null`. `power` records the explicit packet facts, `device_state_before`/`device_state_after`, `system_state_before`/`system_state_after`, nullable `requested_device_object` and actual `bus_status`/`bus_received_at_100ns`/`bus_completed_at_100ns`. Final PnP-device snapshots add `device_power` and `system_power`; live device snapshots add nullable `reported_device_power`. `scenario_success` counts scenario-origin rows against configured requests and requires every actual child and scenario row to complete with success or an explicitly described expected WAIT_WAKE cancellation; unused FIFO templates do not fail the scenario. Genuine `driver_wdm_power.c` images use `NEVERD_WDM_POWER_FIXTURE` / `NEVERD_WDM_POWER_CFG_FIXTURE`, with normal/active-CFG native and C API/CLI coverage. Missing artifacts skip explicitly; execution evidence is Linux-only.

The [complete power scenario](examples/driver-power-scenario.json) runs the genuine power fixture through start, system query/sleep/wake and removal, with three explicit child responses; pass it with `--scenario`.

KMDF 1.33 support uses the exact 1.33.0 ABI: 458 function slots have stable guest identities, while the APIs listed below have execution semantics. `WdfVersionBind` and `WdfVersionUnbind` manage guest bindings around the genuine WDK `FxDriverEntry` wrapper. `WdfGetDriver` reads the public driver globals. Non-PnP drivers, generic objects, control devices, queues and incoming requests share typed contexts, reference counts and executed cleanup/destroy/unload callbacks. Except for lock APIs, automatically serialized callbacks, interrupt DDIs and the request/object operations with explicit IRQL support described below, modeled framework calls and callbacks require `PASSIVE_LEVEL`; adding references after completed cleanup remains outside this profile. Unmodeled function slots, `WdfLdrQueryInterface`, class extensions and UMDF stop explicitly.

Interrupt APIs: `WdfInterruptCreate`, `WdfInterruptQueueDpcForIsr`, `WdfInterruptQueueWorkItemForIsr`, `WdfInterruptSynchronize`, `WdfInterruptAcquireLock`, `WdfInterruptReleaseLock`, `WdfInterruptEnable`, `WdfInterruptDisable`, `WdfInterruptWdmGetInterrupt`, `WdfInterruptGetInfo`, `WdfInterruptGetDevice`.

Framework interrupt objects use assigned line or MSI resources and internal interrupt locks. ISR and synchronized callbacks run at the assigned DIRQL, or at `PASSIVE_LEVEL` for passive handling. `WdfInterruptGetInfo` reports passive IRQL correctly. DPCs run at `DISPATCH_LEVEL`; work items run at `PASSIVE_LEVEL`. Repeated queueing coalesces while queued, and callback ownership survives suspension until return. Power-up connects and enables ordinary interrupts after D0 entry; power-down disables them and drains deferred callbacks before D0 exit. `ReportInactiveOnPowerDown` retains the connection inactive until D0 reactivation; `WdfInterruptReportInactive` and `WdfInterruptReportActive` share that authority. Final hardware release disconnects it. Explicit enable/disable calls execute the driver callbacks without fabricating hardware pulses. A passive `CanWakeDevice` interrupt requires an assigned `wake_capable: true` resource and armed device wake policy. It remains enabled during Dx; an explicit pulse requests wake and retains ISR delivery until physical D0 and successful D0Entry. Wake interrupts are not reported inactive. Automatic failed-device reenumeration remains outside the profile.

Lock APIs: `WdfSpinLockCreate`, `WdfSpinLockAcquire`, `WdfSpinLockRelease`, `WdfWaitLockCreate`, `WdfWaitLockAcquire`, `WdfWaitLockRelease`, `WdfObjectAcquireLock`, `WdfObjectReleaseLock`. External `WDFSPINLOCK` and `WDFWAITLOCK` objects share the executive/dispatcher lock authority with interrupt objects; deletion validates holders, waiters and parent references. Spin locks raise to `DISPATCH_LEVEL` and restore the saved IRQL. Wait locks belong to the real thread, disable normal kernel APCs while waiting/held, and support infinite, relative, absolute and zero timeouts. A zero timeout requires IRQL below `DISPATCH_LEVEL`; other waits require `PASSIVE_LEVEL`. A spin lock assigned to an interrupt must use the interrupt lock APIs. Device/queue synchronization scopes and inherited execution levels serialize I/O, file, cancel and interrupt deferred callbacks using the selected parent lock, preserving ownership across suspension. Passive callback locks wait; dispatch callback locks raise IRQL and reject contention that cannot block. Automatic callbacks may reenter on the same thread; explicit object-lock acquisition is nonrecursive. Interrupt DPC/work-item serialization requires a compatible parent execution level. ISR locking remains independent of the parent callback lock.

Power-policy APIs: `WdfDeviceInitSetPowerPolicyEventCallbacks`, `WdfDeviceAssignS0IdleSettings`, `WdfDeviceAssignSxWakeSettings`, `WdfDeviceStopIdleNoTrack`, `WdfDeviceResumeIdleNoTrack`, `WdfDeviceStopIdleActual`, `WdfDeviceResumeIdleActual`. Driver-managed idle policy supports an explicit positive timeout in milliseconds, `IdleCannotWake`/`IdleCanWake` and no user override. `SystemManagedIdleTimeout` and `SystemManagedIdleTimeoutWithHint` default to a framework-owned PoFx registration with one F0 component; idle alone does not invent an OS power-down decision. STOP/REMOVE retire that registration; restart creates a fresh owner. S0 and Sleeping3 wake execute the registered arm, triggered and disarm callbacks around a real retained `WAIT_WAKE` IRP. StopIdle/ResumeIdle hold balanced, nestable power references; `StopIdle(TRUE)` requires `PASSIVE_LEVEL` and suspends the actual calling frame until D0 completion, while FALSE and ResumeIdle permit IRQL through `DISPATCH_LEVEL`. Synchronous StopIdle from a managed-queue callback or during power-down fails as a deadlock. The S0 idle and Sx wake assignment APIs permit IRQL through `DISPATCH_LEVEL`; callback registration requires `PASSIVE_LEVEL`. Sx wake records a signal and still requires an explicit system Working request. `PowerUpIdleDeviceOnSystemWake` governs whether a non-wake-capable idle device remains in Dx until activity. General PEP/platform policy and automatic failed-device reenumeration remain unsupported.

Declared `parent_id` relationships support KMDF Sx child wake. `ArmForWakeIfChildrenAreArmedForWake` and `IndicateChildWakeOnParentWake` are independent Boolean settings. `EvtDeviceArmWakeFromSxWithReason` receives separate own-device and child reasons, including `(FALSE, TRUE)` when only children require wake. Only successfully armed Sx children with a real retained `WAIT_WAKE` contribute; the parent captures their PDO and START epoch. Parent wake completes those retained IRPs after validating the whole batch and propagates recursively only through devices that opt in. Disabled, disarmed, failed and restarted children do not receive an invented wake. Successful WAIT_WAKE reports record the original source in nullable `wake_source_device_id` and `wake_source_pdo`; each device still needs explicit Working and D0 requests through its own response FIFO.

Arm children before their parent enters Sx/Dx. New child arming is rejected before creating WAIT_WAKE when its direct parent is already in Sx/Dx with either child-aware setting enabled. Canceling the last captured child also cancels a child-only parent's retained WAIT_WAKE and executes its disarm callback once; a parent with its own wake reason or a completed wake keeps normal D0 disarm ordering. This profile uses declared provider topology; guest bus PDO enumeration and raw WDM child-wake callbacks remain unsupported.

The genuine WDK `driver_kmdf_child_wake.c` fixture uses optional `NEVERD_KMDF_CHILD_WAKE_FIXTURE` / `NEVERD_KMDF_CHILD_WAKE_CFG_FIXTURE`. The [child-wake scenario](examples/driver-kmdf-child-wake-scenario.json) runs it through the CLI. Missing images skip explicitly; normal/active-CFG and preferred/rebased execution evidence remains Linux-only.

Scenarios explicitly declare PDO `wake_capabilities: {s0, sx}` as Booleans. READ/WRITE/IOCTL requests may provide `power_policy_events: [{device_id, after_100ns, action}]`, with framework actions `idle`, `active` or `wake` and the explicit PoFx decisions described below. Idle starts the driver’s timeout; active requests D0 when needed, and wake requires an armed source. Reports retain `source_request_index`, `event_index`, `device_id`, captured `device_epoch`, `action`, `due_at_100ns` and `occurred_at_100ns`; events cannot bind to a later restart. D0/D2/D3 requests consume `requested_device_power` responses and report `origin: "framework_power_policy"`. The independent `origin: "framework_wait_wake"` IRP remains pending until wake or disarm and does not consume that FIFO. A completed cancellation of this WAIT_WAKE is a valid expected outcome. See the [complete KMDF idle/wake scenario](examples/driver-kmdf-power-policy-scenario.json).

`WdfDeviceWdmAssignPowerFrameworkSettings` (KMDF 1.33 slot 425) supports custom Fx states for one component. Assign system-managed idle settings successfully first, then call once at `PASSIVE_LEVEL` before the first START completes. Component, idle-state and callback settings are copied. Guest active-condition, idle-condition and idle-state callbacks run through real continuations; omitted callbacks use framework defaults. This profile requires `PoFxDeviceFlags=0`, `DirectedPoFxEnabled=WdfFalse` and no PEP power-control callback. See Microsoft's [settings contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfdevice/nf-wdfdevice-wdfdevicewdmassignpowerframeworksettings).

After registration, `EvtDeviceWdmPostPoFxRegisterDevice` runs before management starts. Its failure fails START and suspends running self-managed I/O before hardware release. The handle remains valid until `EvtDeviceWdmPrePoFxUnregisterDevice` returns. STOP/REMOVE drain callbacks and retire the handle; restart creates a new handle from the copied settings. The framework handle permits `PoFxCompleteIdleCondition`, `PoFxCompleteIdleState` and component latency/residency/wake hints. The framework owns activity references, device-power acknowledgement and registration lifetime; raw PoFx Activate/Idle/Start/Unregister and other ownership-changing calls on that handle stop explicitly. This is a profile restriction, not a WDK prohibition. Power-managed queues stay held until the component reaches active F0.

The genuine `driver_kmdf_pofx.c` fixture uses `NEVERD_KMDF_POFX_FIXTURE` / `NEVERD_KMDF_POFX_CFG_FIXTURE`. Missing images skip explicitly; execution evidence remains Linux-only. Run the [custom KMDF PoFx scenario](examples/driver-kmdf-pofx-scenario.json) with this fixture.

This profile rejects STOP/REMOVE before hardware teardown while a PoFx device-power acknowledgement is pending; complete the actual D-state transaction first. Quiescence cancels only unissued decisions and drains existing component callbacks before D0Exit. It never fabricates device-power completion. This serialization limit belongs to the model, not to the general WDK contract.

WDM PoFx v1 APIs: `PoFxRegisterDevice`, `PoFxUnregisterDevice`, `PoFxStartDevicePowerManagement`, `PoFxActivateComponent`, `PoFxIdleComponent`, `PoFxCompleteIdleCondition`, `PoFxCompleteIdleState`, `PoFxCompleteDevicePowerNotRequired`, `PoFxReportDevicePoweredOn`, `PoFxSetComponentLatency`, `PoFxSetComponentResidency`, `PoFxSetComponentWake`, `PoFxSetDeviceIdleTimeout`. Registration copies component/idle-state descriptions and retains the device until unregister. Component references, callback entry, acknowledgement and return have separate ownership; acknowledgements cannot precede delivery or release a still-running callback. Blocking activation/idle continues on the calling thread. Latency, residency, wake and idle-timeout hints constrain explicit decisions; they do not synthesize Windows policy. A `component_idle_state` event requires `component` and `state`; `power_not_required` accepts neither. Both require the current registered device and captured START epoch. The existing `idle`/`active`/`wake` actions drive framework policy. PoFx v2/v3, directed power management, and arbitrary platform power-control requests remain unsupported.

The genuine `driver_wdm_pofx.c` fixture uses optional CMake paths `NEVERD_WDM_POFX_FIXTURE` / `NEVERD_WDM_POFX_CFG_FIXTURE` for normal/active-CFG images. Run the [complete PoFx scenario](examples/driver-pofx-scenario.json) with `--scenario`; its F command requests explicit F1 then returns to F0. The P command accepts `power_not_required` instead. Missing fixtures skip explicitly; execution evidence is Linux-only.

Optional PDO `d3cold: {supported, enabled_by_default, wake_s0, wake_sx}` contains four required Booleans describing a dedicated supply. Cold wake requires the matching `wake_capabilities` fact. `ExcludeD3Cold=True` keeps D3hot, False permits supported cold entry, and Default follows `enabled_by_default`; unavailable cold wake keeps an armed device in D3hot. Only successful D3hot→D3cold entry advances the power generation and restores configured register values on D0 access. Assignment epochs and MMIO aliases survive; MMIO/DMA remain unavailable before physical D0. Cold entry rejects outstanding DMA transactions, mappings or channel ownership while allowing idle adapters and common RAM buffers. Shared power rails and inferred firmware capabilities remain unsupported. See Microsoft’s [device sleeping states](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/device-sleeping-states) and [idle settings contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfdevice/ns-wdfdevice-_wdf_device_power_policy_idle_settings).

The optional scenario-root `service_name` overrides the base configuration; omission preserves the caller’s service name. It must contain 1–128 ASCII letters, digits, `_` or `-`, and applies equally to CLI, C and Python scenario JSON.

The following request/object APIs support IRQL through `DISPATCH_LEVEL`: `WdfObjectDereferenceActual`, `WdfRequestComplete`, `WdfRequestCompleteWithInformation`, `WdfRequestRetrieveInputBuffer`, `WdfRequestRetrieveOutputBuffer`, `WdfRequestRetrieveInputMemory`, `WdfRequestRetrieveOutputMemory`, `WdfRequestRetrieveInputWdmMdl`, `WdfRequestRetrieveOutputWdmMdl`, `WdfRequestGetInformation`, `WdfRequestSetInformation`, `WdfRequestGetIoQueue`, `WdfRequestGetFileObject`, `WdfRequestWdmGetIrp`, `WdfRequestGetParameters`, `WdfRequestGetStatus`, `WdfRequestMarkCancelable`, `WdfRequestMarkCancelableEx`, `WdfRequestUnmarkCancelable`, `WdfRequestIsCanceled`, `WdfMemoryGetBuffer`.

`WdfObjectGetTypedContextWorker`, `WdfObjectContextGetObject`, `WdfObjectReferenceActual` also support the modeled interrupt IRQLs; context access and reference acquisition do not invoke passive callbacks.

Request completion or final object dereference above `PASSIVE_LEVEL` schedules real cleanup, destruction and subsequent framework delivery at `PASSIVE_LEVEL`. Other APIs keep their existing IRQL restrictions.

KMDF PnP drivers may register `EvtDriverDeviceAdd` through `WdfDriverCreate`. For each configured PDO, the callback receives a framework-owned `WDFDEVICE_INIT`; `WdfDeviceCreate` consumes it and creates a direct FDO/PDO pair. `WdfFdoInitWdmGetPhysicalDevice` and `WdfDeviceWdmGetPhysicalDevice` return the configured PDO. `WdfDeviceWdmGetAttachedDevice` returns the direct lower WDM object, `WdfWdmDeviceGetWdfDeviceHandle` maps a framework-owned WDM object back to its WDF handle, and `WdfDeviceGetDriver` returns the owning WDFDRIVER. On AddDevice failure the framework deletes any created FDO and runs guest cleanup/destroy callbacks before the provider retires; successful AddDevice clears `DO_DEVICE_INITIALIZING`. PnP IRPs forward to the configured bus response, and completed Remove deletes the WDF queue/device before the captured route and PDO retire. `WdfDeviceInitSetPnpPowerEventCallbacks` accepts the exact KMDF 1.33 structure with `EvtDevicePrepareHardware`, `EvtDeviceD0Entry`, `EvtDeviceD0EntryPostInterruptsEnabled`, `EvtDeviceD0ExitPreInterruptsDisabled`, `EvtDeviceD0Exit`, `EvtDeviceReleaseHardware`, `EvtDeviceQueryStop`, `EvtDeviceQueryRemove` and `EvtDeviceSurpriseRemoval`, the five self-managed I/O callbacks described below; other non-null event callbacks fail explicitly. Query callbacks run before forwarding to the bus and may wait. A rejected query completes with the callback status without a bus observation or power transition; `STATUS_PENDING` and `STATUS_NOT_SUPPORTED` are invalid callback results. SurpriseRemoval is a void notification before lower forwarding; its return register is ignored. After successful provider START, PrepareHardware receives distinct raw and translated `WDFCMRESLIST` handles, then D0Entry runs at `PASSIVE_LEVEL` with `WdfPowerDeviceD3Final` before the original IRP completes. Empty lists return zero from `WdfCmResourceListGetCount` and NULL from `WdfCmResourceListGetDescriptor`. For a configured `register_bank` provider, both lists expose the assigned read-only `CM_PARTIAL_RESOURCE_DESCRIPTOR` entries; descriptor pointers remain valid through ReleaseHardware and are retired afterward. A driver may map the translated memory assignment with `MmMapIoSpace` and must unmap it before final STOP or Remove completion. A failed PrepareHardware or D0Entry status becomes the START status; ReleaseHardware still runs after either failure, while D0Exit is skipped. Successful STOP, surprise removal and direct removal from D0 invoke D0Exit with `WdfPowerDeviceD3Final`, then ReleaseHardware, before IRP completion. A later START prepares hardware and enters D0 again. The public scenario must explicitly remove a device after failed START. PnP queues accept `WdfUseDefault` and `WdfTrue` for automatic power management, or `WdfFalse` to keep dispatch under driver control. A managed queue reports `WdfIoQueuePnpHeld` until D0 entry finishes and again before D0 exit; it resumes request delivery after entering D0. Before a managed queue leaves D0, a registered `EvtIoStop` runs for each driver-owned request with Suspend for STOP or Purge for surprise removal. The driver may complete the request, call `WdfRequestStopAcknowledge`, or leave it to an existing completion producer. Without `EvtIoStop`, the framework also waits for every delivered request to complete. A true requeue flag returns the request to framework ownership for delivery after restart. A false flag retains driver ownership and requires `EvtIoResume` after D0 entry. Queued requests remain held during STOP and are presented after restart, including when no hardware callback is registered. The D0 exit and PnP completion wait for the real request completion; if no producer remains, the scheduler reports a stalled `model_error`. A retained request without a resume callback remains unsupported. Unmodeled resource types, resource mutation, general KMDF power policy and other PnP event callbacks remain unsupported. The optional genuine WDK `driver_kmdf_pnp.c` fixture covers normal and active CFG images through native and public C API/CLI tests using `NEVERD_KMDF_PNP_FIXTURE` / `NEVERD_KMDF_PNP_CFG_FIXTURE`.

Control devices require a copied printable-ASCII name and the exact SDDL `D:P(A;;GA;;;WD)`. This grants universal access and avoids inventing a caller token; other security descriptors, unnamed devices and automatic names are unsupported. Device initialization owns one WDM device. Requests can select it through the existing session namespace using `\DosDevices\Name` or `\??\Name` symbolic-link aliases; reports retain the canonical device name. Successful creation consumes and clears the initializer; failed creation rolls back partial device ownership. `WdfControlFinishInitializing` gates I/O delivery. Deletion removes the device and its links only when modeled files, work items and requests permit it; cancellation or draining during deletion is unsupported.

The 96-byte `WDF_IO_QUEUE_CONFIG` supports manual, sequential and parallel default queues and nondefault queues of each dispatch type, with execution levels and synchronization scopes configured by object attributes. A parallel queue uses `Settings.Parallel.NumberOfPresentedRequests = -1` for unlimited delivery or a positive finite limit. A default sequential queue accepts additional incoming requests while one is presented; each waits in FIFO order until that request completes or leaves the queue. Queued requests may be canceled without calling the driver. Control-device queues are not power managed. `WdfIoQueueStop` pauses delivery but continues accepting requests; `WdfIoQueueStart` resumes queued delivery, and `WdfIoQueueGetState` reports queued and delivered counts. Retrieval while stopped returns `STATUS_WDF_PAUSED`. A stop-completion callback runs with the supplied context after every request already delivered to the driver has completed or left the queue; waiting requests do not delay it. A second callback registration while one is pending fails explicitly. Specific READ/WRITE/IOCTL callbacks take precedence over the default callback. Accepted requests return `STATUS_PENDING` even when completed synchronously; a void callback's return register does not complete its request. A manual default queue accepts incoming requests without invoking an I/O callback. `WdfRequestForwardToIoQueue` transfers an owned request to another queue on the same device, freeing its source presentation slot. An automatic destination delivers through its own matching callback when a slot is available and otherwise retains the request; if there is no matching callback, it completes the queued request with `STATUS_INVALID_DEVICE_REQUEST` when a slot becomes available. The framework owns the request while queued. `WdfIoQueueRetrieveNextRequest` transfers a pending request from a manual or sequential queue to the driver in FIFO order, and returns `STATUS_NO_MORE_ENTRIES` with a null output when empty; parallel queue retrieval returns `STATUS_INVALID_DEVICE_STATE`. `WdfRequestRequeue` returns a retrieved, unmarked request to the head of the same manual queue. If cancellation wins before a request is first delivered to the driver, the framework removes and completes the queued request with `STATUS_CANCELLED`, running its cleanup callbacks before retiring the IRP; a later retrieval observes an empty queue. Deferred completion uses the existing scheduler, including explicit `defer_callback_drain` batches on asynchronous files or separate files after forwarding. For an incoming request to an automatic default queue, a missing handler completes it with `STATUS_INVALID_DEVICE_REQUEST`; zero-length READ/WRITE completes without delivery unless enabled. The default file package completes CREATE/CLEANUP/CLOSE successfully with Information=0. Broader PnP and power behavior remains outside this profile.

`WdfIoQueueReadyNotify` registers one `EvtIoQueueState` callback for a manual queue. At `PASSIVE_LEVEL`, it receives `(WDFQUEUE, WDFCONTEXT)` when the queued count changes from zero to nonzero, even if the driver still owns earlier retrieved requests. Registering on an already nonempty queue may notify immediately; a stopped queue waits until `WdfIoQueueStart`. Duplicate registration and deregistration before stopping return `STATUS_INVALID_DEVICE_REQUEST`. Passing NULL after `WdfIoQueueStop` deregisters the callback.

`WdfIoQueueFindRequest` scans a manual queue without transferring request ownership and adds one request reference on success. The driver releases that reference with `WdfObjectDereference`; `WdfIoQueueRetrieveFoundRequest` transfers ownership of a still-queued request, while a request removed by cancellation returns `STATUS_NOT_FOUND`. Optional request parameters use the same layout as `WdfRequestGetParameters`. A live framework file object filters `WdfIoQueueFindRequest`; `WdfIoQueueRetrieveRequestByFileObject` takes the next matching request from a manual or sequential queue and leaves the output unchanged when none matches.

A configured `EvtIoCanceledOnQueue` receives `(WDFQUEUE, WDFREQUEST)` only for a request that the driver previously received and then forwarded or requeued, or one that the caller-context callback explicitly enqueued. A never-delivered queued request is completed by the framework with `STATUS_CANCELLED` without that callback. Cancellation transfers the notified request back to the driver, which must complete it inside the callback or later and cannot requeue it. Queue purge and state completion wait for the callback to return and for the driver-owned request to complete.

`WdfIoQueueDrain` stops accepting new requests with `STATUS_INVALID_DEVICE_STATE` while delivering requests already queued; its completion callback runs only when both queued and driver-owned counts reach zero. Forwarding into a drained queue returns `STATUS_WDF_BUSY`. `WdfIoQueueStart` restores acceptance. A pending drain callback must finish before another queue-state change.

`WdfIoQueuePurge` also rejects new arrivals and cancels every request still queued by the framework with `STATUS_CANCELLED`, running request cleanup before IRP retirement. For already delivered requests marked cancelable, it records cancellation on the original IRP and invokes each driver cancel callback. Its optional state callback waits until queued and driver-owned requests, including active cancel callbacks, have finished. Requests not marked cancelable remain driver-owned until the driver completes them. Start restores acceptance after purge.

The synchronous forms `WdfIoQueueStopSynchronously`, `WdfIoQueueDrainSynchronously`, and `WdfIoQueuePurgeSynchronously` suspend their PASSIVE_LEVEL caller until the relevant requests retire. Stop keeps accepting but pauses delivery and waits for delivered requests; drain rejects arrivals while delivering queued requests and waits for both queued and delivered requests; purge cancels queued and marked delivered requests and waits through cancellation callbacks. A wait without a completion producer reports a stalled model error.

`WdfIoQueueStopAndPurge` and `WdfIoQueueStopAndPurgeSynchronously` cancel requests already queued and marked driver-owned requests, then keep accepting new requests without delivering them until `WdfIoQueueStart`. The asynchronous state callback and synchronous wait finish after the original delivered requests and their cancellation callbacks; requests added after the stop-and-purge call remain queued and do not delay notification.

Request parameters use the 40-byte `WDF_REQUEST_PARAMETERS` layout. Input/output accessors return the logical lengths, preserving buffered aliases and existing direct-I/O MDL mappings; direct IOCTL input remains buffered. Wrong directions and insufficient buffers return documented statuses. Completion runs request cleanup while buffers are live, completes the IRP and releases request-owned pins, then destroys child objects and the request when references permit. New buffer and parameter accessors are rejected once completion begins; already obtained buffer pointers remain usable during cleanup. An external object reference preserves context, not completed IRP access.

Cancellation is modeled for requests on the control-device queues described above. `WdfRequestMarkCancelableEx` returns `STATUS_CANCELLED` without invoking a callback if cancellation already occurred. Successful `WdfRequestUnmarkCancelable` removes the callback; a later cancellation records the canceled state without delivering that callback. `WdfRequestIsCanceled` observes that state on an unmarked live request. After successful marking, completion requires successful unmarking or delivery of the cancellation callback: a merely queued callback does not authorize completion. Once delivery begins, the callback and a worker may coordinate completion, including when the callback waits. A separate internal reference retains the request through cancellation-callback return; completion still retires the IRP first, and the final request-destroy continuation may itself wait. DPCs take priority, followed by cancellation callbacks in FIFO order, then ordinary workers; queued cancellation callbacks also precede resuming ready passive waiters.

Legacy void `WdfRequestMarkCancelable` supports IRQL through `DISPATCH_LEVEL`. For an already canceled request, it invokes `EvtRequestCancel` synchronously only when the queue execution level permits the current IRQL and the current thread does not already own the same callback lock. Otherwise it queues a worker or DPC and returns; normal cancellation after registration is also scheduled. Queued cancellation is marked delivered only when the real guest callback enters after acquiring its callback lock. Request completion cannot substitute a queued callback for delivered cancellation. Without automatic synchronization (`WdfSynchronizationScopeNone`), Microsoft recommends Ex. [Microsoft](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfrequest/nf-wdfrequest-wdfrequestmarkcancelable).

`WdfRequestGetInformation` and `WdfRequestSetInformation` share the original 64-bit `IRP.IoStatus.Information`, including direct guest writes. Set assigns the value; transfer-length validation occurs at completion. `WdfRequestCompleteWithInformation` writes the same field before cleanup; changes through a previously saved IRP during cleanup determine the final Information even though GetInformation already returns zero in that phase. `WdfRequestGetIoQueue` returns the delivering queue while the driver owns the request, including the manual destination after retrieval; queued requests are framework-owned and cannot be inspected by the driver. With the default file configuration, `WdfRequestGetFileObject` returns NULL: no WDF file object is invented from the WDM FILE_OBJECT. `WdfRequestWdmGetIrp` returns the same IRP; guest `IoCompleteRequest`/`IofCompleteRequest` cannot bypass WDF completion. While a request handle remains valid during or after completion, GetInformation/GetIoQueue return zero, and MDL retrieval first clears a valid output slot to NULL then returns `STATUS_INTERNAL_ERROR`. SetInformation/GetFileObject/WdmGetIrp remain rejected then. Existing buffer and parameter accessor restrictions are unchanged.

`WdfRequestRetrieveInputWdmMdl` and `WdfRequestRetrieveOutputWdmMdl` lazily describe the existing SystemBuffer for buffered WRITE input, READ output and IOCTL input/output. Each requested direction must be valid and nonempty before using the request’s single cached descriptor; the first successful retrieval fixes its ByteCount even when the other direction has a different logical length. `MmGetSystemAddressForMdlSafe` returns the original VA for this descriptor; additional system mapping, system unmapping and driver freeing are rejected. Direct READ output, WRITE input and IOCTL output instead return the existing `IRP.MdlAddress` without mapping it merely by retrieval; direct IOCTL input uses the SystemBuffer cache. Descriptors, IRP and buffers retire at completion. Cancellation-internal or external references retain only WDF context, not completed I/O storage. Built physical PFNs are read-only; WDF MDL retrieval for `METHOD_NEITHER` remains unsupported; request-owned WDFMEMORY uses a separate locked user-page mapping.

For buffered, direct and neither KMDF transfers, `WdfDeviceInitSetIoInCallerContextCallback` runs a prequeue callback in the requestor process at `PASSIVE_LEVEL`. It must complete the request or call `WdfDeviceEnqueueRequest` once. Enqueue captures the queue selected by `WdfDeviceConfigureRequestDispatching` for that request type, falling back to the default queue when no mapping exists; later mapping changes do not move the accepted request. For `METHOD_NEITHER` IOCTL and neither READ/WRITE, `WdfRequestRetrieveUnsafeUserInputBuffer` and `WdfRequestRetrieveUnsafeUserOutputBuffer` expose the original user VAs only in that callback. `WdfRequestProbeAndLockUserBufferForRead` and `WdfRequestProbeAndLockUserBufferForWrite` check page rights and pin request-owned memory; `WdfMemoryGetBuffer` returns a system alias that remains usable in the queue callback outside the requestor context. Completion releases the pins and aliases. The original scenario buffers and explicitly declared `user_buffers` are eligible, including buffers reached through embedded pointers. Locking remains restricted to the current request in caller context; arbitrary user mappings remain unsupported.

`WdfRequestRetrieveInputMemory` and `WdfRequestRetrieveOutputMemory` expose request-owned WDFMEMORY views over existing buffered or direct input/output buffers. Repeated retrieval of one direction keeps its handle; `WdfMemoryGetBuffer` returns the original buffer and logical length. Zero-length or invalid-direction requests fail with their WDF statuses, and neither I/O still requires the caller-context probe-and-lock path. These borrowed views add no MDL pin and expire when the request completes.

The framework sequences `EvtDeviceSelfManagedIoInit`,
`EvtDeviceSelfManagedIoSuspend`, `EvtDeviceSelfManagedIoRestart`,
`EvtDeviceSelfManagedIoFlush` and `EvtDeviceSelfManagedIoCleanup` around its
hardware and queue transitions. Init runs once, restart follows a successful
resume, and cleanup runs once before object destruction. Surprise removal
drains power-managed requests before suspending self-managed I/O; orderly
removal suspends self-managed I/O first. Downward callbacks
precede provider power-off; D0 entry follows provider power-on. Ordinary D2/D3 and D0
transitions retain hardware resource handles and mappings.
`WdfDeviceInitSetPowerPolicyOwnership` controls ownership (FDO default true,
filter default false). The default policy maps Sleeping3 to D3 and Working to
D0, consuming explicit `requested_device_power` responses. Each generated child
has its own IRP, `origin: "framework_power_policy"` and `response_index`.
System queries wait for a separate matching device query and inherit its
status without changing power state. The S3 SET parent waits for D3 completion;
the S0 parent may finish after D0 is issued. Automatic failed-device
reenumeration remains outside this profile.

Modeled KMDF APIs: `WdfDriverCreate`, `WdfDriverGetRegistryPath`, `WdfDriverWdmGetDriverObject`, `WdfWdmDriverGetWdfDriverHandle`, `WdfObjectGetTypedContextWorker`, `WdfObjectAllocateContext`, `WdfObjectContextGetObject`, `WdfObjectReferenceActual`, `WdfObjectDereferenceActual`, `WdfObjectCreate`, `WdfObjectDelete`, `WdfObjectAcquireLock`, `WdfObjectReleaseLock`, `WdfControlDeviceInitAllocate`, `WdfDeviceInitFree`, `WdfDeviceInitAssignName`, `WdfDeviceInitSetDeviceType`, `WdfDeviceInitSetExclusive`, `WdfDeviceInitSetFileObjectConfig`, `WdfDeviceInitSetIoType`, `WdfDeviceInitSetIoInCallerContextCallback`, `WdfDeviceInitSetPnpPowerEventCallbacks`, `WdfDeviceInitSetPowerPolicyOwnership`, `WdfCmResourceListGetCount`, `WdfCmResourceListGetDescriptor`, `WdfDeviceCreate`, `WdfDeviceEnqueueRequest`, `WdfDeviceCreateSymbolicLink`, `WdfControlFinishInitializing`, `WdfDeviceWdmGetDeviceObject`, `WdfDeviceWdmGetAttachedDevice`, `WdfDeviceWdmGetPhysicalDevice`, `WdfWdmDeviceGetWdfDeviceHandle`, `WdfDeviceGetDriver`, `WdfDeviceGetIoTarget`, `WdfFdoInitWdmGetPhysicalDevice`, `WdfFdoInitSetFilter`, `WdfIoQueueCreate`, `WdfDeviceGetDefaultQueue`, `WdfDeviceConfigureRequestDispatching`, `WdfIoQueueGetDevice`, `WdfIoQueueGetState`, `WdfIoQueueStop`, `WdfIoQueueStopSynchronously`, `WdfIoQueueStopAndPurge`, `WdfIoQueueStopAndPurgeSynchronously`, `WdfIoQueueReadyNotify`, `WdfIoQueueStart`, `WdfIoQueueDrain`, `WdfIoQueueDrainSynchronously`, `WdfIoQueuePurge`, `WdfIoQueuePurgeSynchronously`, `WdfIoQueueRetrieveNextRequest`, `WdfIoQueueRetrieveRequestByFileObject`, `WdfIoQueueFindRequest`, `WdfIoQueueRetrieveFoundRequest`, `WdfRequestForwardToIoQueue`, `WdfRequestRequeue`, `WdfRequestStopAcknowledge`, `WdfRequestComplete`, `WdfRequestCompleteWithInformation`, `WdfRequestFormatRequestUsingCurrentType`, `WdfRequestSend`, `WdfRequestGetStatus`, `WdfRequestSetCompletionRoutine`, `WdfRequestGetCompletionParams`, `WdfRequestGetParameters`, `WdfRequestRetrieveInputBuffer`, `WdfRequestRetrieveOutputBuffer`, `WdfRequestRetrieveInputMemory`, `WdfRequestRetrieveOutputMemory`, `WdfRequestRetrieveUnsafeUserInputBuffer`, `WdfRequestRetrieveUnsafeUserOutputBuffer`, `WdfRequestProbeAndLockUserBufferForRead`, `WdfRequestProbeAndLockUserBufferForWrite`, `WdfMemoryGetBuffer`, `WdfRequestRetrieveInputWdmMdl`, `WdfRequestRetrieveOutputWdmMdl`, `WdfRequestSetInformation`, `WdfRequestGetInformation`, `WdfRequestGetFileObject`, `WdfFileObjectGetFileName`, `WdfFileObjectGetFlags`, `WdfFileObjectGetDevice`, `WdfFileObjectWdmGetFileObject`, `WdfRequestGetIoQueue`, `WdfRequestWdmGetIrp`, `WdfRequestMarkCancelable`, `WdfRequestMarkCancelableEx`, `WdfRequestUnmarkCancelable`, `WdfRequestIsCanceled`, `WdfInterruptCreate`, `WdfInterruptQueueDpcForIsr`, `WdfInterruptQueueWorkItemForIsr`, `WdfInterruptSynchronize`, `WdfInterruptAcquireLock`, `WdfInterruptReleaseLock`, `WdfInterruptEnable`, `WdfInterruptDisable`, `WdfInterruptWdmGetInterrupt`, `WdfInterruptGetInfo`, `WdfInterruptReportActive`, `WdfInterruptReportInactive`, `WdfInterruptGetDevice`, `WdfSpinLockCreate`, `WdfSpinLockAcquire`, `WdfSpinLockRelease`, `WdfWaitLockCreate`, `WdfWaitLockAcquire`, `WdfWaitLockRelease`, `WdfDeviceInitSetPowerPolicyEventCallbacks`, `WdfDeviceAssignS0IdleSettings`, `WdfDeviceWdmAssignPowerFrameworkSettings`, `WdfDeviceAssignSxWakeSettings`, `WdfDeviceStopIdleNoTrack`, `WdfDeviceResumeIdleNoTrack`, `WdfDeviceStopIdleActual`, `WdfDeviceResumeIdleActual`.

For a PnP FDO, `WdfDeviceInitSetDeviceType` stores the supplied 32-bit type in the WDM `DEVICE_OBJECT`; otherwise `FILE_DEVICE_UNKNOWN` remains the default. Type-dependent I/O priority boosts are not modeled.

`WdfDeviceInitSetExclusive` sets `DO_EXCLUSIVE` on the WDM device created from the initializer. A named control device admits one independent open until its file closes. On a PnP FDO, this flag alone does not make the named PDO or the whole stack exclusive; INF-specified PDO exclusivity is outside this profile.

`WdfDeviceInitSetFileObjectConfig` registers `EvtDeviceFileCreate`, `EvtFileCleanup` and `EvtFileClose` and copies optional file-object context attributes before device creation. This profile supports `WdfFileObjectNotRequired`, `WdfFileObjectWdfCanUseFsContext`, `WdfFileObjectWdfCanUseFsContext2` and `WdfFileObjectWdfCannotUseFsContexts`. The selected WDM context slot holds the WDF handle until failed CREATE or CLOSE and must be initially empty. For the three classes that require file objects, `WdfFileObjectCanBeOptional` permits I/O with no matching WDM file identity; `WdfRequestGetFileObject` then returns NULL. CREATE, CLEANUP and CLOSE still require a WDM file object. `WdfFdoInitSetFilter` enables filter-default file forwarding; `WdfTrue` forwards explicitly and `WdfFalse` keeps requests local. Forwarding requires a direct FDO/PDO route and an explicit `bus_completion` for each file request. A positive `delay_100ns` is supported for automatic CREATE/CLEANUP/CLOSE forwarding and for synchronous, callback-based asynchronous or `SEND_AND_FORGET` CREATE sends. CLEANUP and CLOSE callbacks run before lower dispatch. Automatic forwarding retains the WDM IRP and file through cleanup and destruction callbacks required during completion. External WDF references preserve context but do not extend the lifetime of a completed WDM file. A CREATE callback with `WdfFileObjectNotRequired` can obtain its device-owned local target through `WdfDeviceGetIoTarget` and forward the original request with `WdfRequestSend` using only `WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET`. The retained PDO consumes the explicit bus response and owns terminal completion, including when the lower response is delayed. A CREATE callback with a framework file object can use `WDF_REQUEST_SEND_OPTION_SYNCHRONOUS`, read the lower result through `WdfRequestGetStatus`, and complete the original request with that status. A delayed synchronous send suspends the actual guest caller frame until completion and then resumes with the Boolean sent result; the final NTSTATUS is available through `WdfRequestGetStatus`. A default asynchronous send first formats the request with `WdfRequestFormatRequestUsingCurrentType` and registers `WdfRequestSetCompletionRoutine`. Its callback receives `WDF_REQUEST_COMPLETION_PARAMS`, can read the lower result through `WdfRequestGetCompletionParams` or `WdfRequestGetStatus`, and completes the original request with its own chosen status. For a callback-based send, a delayed PDO response leaves the request outstanding until the virtual deadline, then queues its completion callback with the final lower status. A `WDF_REQUEST_SEND_OPTION_TIMEOUT` on a synchronous or callback-based asynchronous CREATE send accepts a negative relative interval or a positive absolute deadline on the virtual 100 ns clock. An earlier timeout delivers `STATUS_IO_TIMEOUT`; an equal deadline or an immediate lower completion wins. A past absolute deadline has no remaining interval. Zero disables the timeout; other send options remain unsupported. A configured file object keeps the WDM `FILE_OBJECT` and WDF handle distinct. `WdfRequestGetFileObject`, `WdfFileObjectGetDevice` and `WdfFileObjectWdmGetFileObject` expose that identity while it is live. `WdfFileObjectGetFileName` returns the WDM file-name record and `WdfFileObjectGetFlags` reads its current flags. Failed CREATE deletes the WDF file object without file cleanup or close callbacks; successful CLEANUP and CLOSE run their callbacks before context cleanup and destruction.

WDM CREATE/CLEANUP/CLOSE forwarded to a configured PDO accept the same explicit delayed `bus_completion`. The real guest completion routine runs after the lower deadline and retains the usual pending propagation and final-status ownership rules.

Optional genuine-WDK validation compiles `driver_kmdf_lifecycle.c`, `driver_kmdf_control.c` and `driver_kmdf_pnp.c` separately with the real KMDF entry library. `NEVERD_KMDF_FIXTURE` / `NEVERD_KMDF_CFG_FIXTURE` select lifecycle images; `NEVERD_KMDF_CONTROL_FIXTURE` / `NEVERD_KMDF_CONTROL_CFG_FIXTURE` select normal/active-CFG control-device images. Missing external artifacts produce explicit skips. See [testing](testing.md) for native and C API/CLI coverage. Current execution evidence is limited to Linux hosts.

The initial API model deliberately has a finite contract:

| APIs | Modeled behavior and restrictions |
|------|-----------------------------------|
| `RtlInitUnicodeString` | Builds a guest `UNICODE_STRING` for a bounded NUL-terminated source |
| `RtlCopyUnicodeString`, `RtlCompareUnicodeString`, `RtlEqualUnicodeString` | Counted UTF-16 copy and case-sensitive comparison; case-insensitive comparison requires a Windows case table and stops |
| `ExRaiseStatus`, `ExRaiseAccessViolation`, `ExRaiseDatatypeMisalignment` | Raise a guest exception for supported C `__except` handlers and filters, with unwind `__finally`; no normal API return |
| `ProbeForRead`, `ProbeForWrite`, `ExGetPreviousMode` | User-mode request context with numerical read probing, page-touching write probing and explicit access/misalignment exceptions |
| `ExAllocatePool2` | Paged/nonpaged NX allocations, zeroed by default; uninitialized and cache-aligned flags modeled; invalid required flags return NULL, quota/executable pools and raised allocation exceptions stop |
| `MmGetSystemRoutineAddress` | Resolves a counted guest name through the shared export inventory |
| `NtQuerySystemInformation`, `ZwQuerySystemInformation` | Class `11` (`SystemModuleInformation`) at PASSIVE_LEVEL: zero-length size queries and complete Win64 module records for modeled providers and the input driver. Partial buffers and unknown classes stop explicitly. |
| `MmMapLockedPagesSpecifyCache`, `MmGetSystemAddressForMdlSafe`, `MmUnmapLockedPages` | MDL system aliases and process-owned user views retain physical cache attributes and permissions; nonpaged pool MDLs reuse the original pool mapping through the safe helper |
| `IoAllocateMdl`, `MmBuildMdlForNonPagedPool`, `MmProbeAndLockPages`, `MmUnlockPages`, `IoFreeMdl` | Standalone or IRP-associated nonpaged-pool/user descriptors, mutable chain links, independent locks and shared system aliases; no quota |
| `ZwOpenKey`, `ZwCreateKey`, `ZwQueryValueKey`, `ZwSetValueKey`, `ZwDeleteValueKey`, `ZwDeleteKey`, `ZwClose` | Explicit session registry, per-handle rights and lifetime, query buffer sizing and mutations; no host registry access |
| `ExAllocatePool` | Legacy two-argument data allocation for pool types `0`, `1`, and `512`; shared alignment, uninitialized-byte model, size/IRQL checks and NULL on exhaustion. Free through `ExFreePool` or a zero-tag `ExFreePoolWithTag`; retained allocations remain kernel dependencies. |
| `ExAllocatePoolWithTag`, `ExFreePoolWithTag`, `ExFreePool` | Data allocations for pool types `0`, `1`, and `512`; positive size/tag, matching tagged frees, no address reuse |
| `IoCreateDevice`, `IoDeleteDevice` | Device type `0x22`, characteristics `0` or `0x100`, bounded extensions, ASCII `\Device\Name` names |
| `IoAttachDeviceToDeviceStack`, `IoDetachDevice` | Same-driver attachment; attach returns the previous top, detach consumes the saved lower device; explicit topology/lifetime limits above |
| `IofCallDriver`, `IoCallDriver` | Exact-target dispatch on the retained route; validated guest stack cursor and distinct lower NTSTATUS |
| `IoGetDmaAdapter` | Explicit Internal bus-master version 0/1 description and bound version-one operations table at PASSIVE_LEVEL; newer versions probe NULL |
| `AllocateCommonBuffer`, `FreeCommonBuffer`, `GetDmaAlignment`, `PutDmaAdapter` | Adapter-table methods over shared coherent RAM, exact allocation identity and independent adapter lifetime |
| `GetScatterGatherList`, `PutScatterGatherList` | Adapter-table methods at DISPATCH_LEVEL; real inline or resource-queued list callback, pinned MDL view and explicit mapping release |
| `AllocateAdapterChannel`, `MapTransfer`, `FlushAdapterBuffers`, `FreeMapRegisters` | Translated bus-master channel callbacks, shared register quota, contiguous MDL fragments, aggregate flush and exact retained-register release |
| `KeFlushIoBuffers` | Coherent CPU-cache flush over a live locked/nonpaged MDL, without releasing DMA mappings |
| `MmMapIoSpace`, `MmMapIoSpaceEx`, `MmUnmapIoSpace` | Declared translated register-bank subranges; NonCached / NOCACHE RO or RW aliases, exact unmap and DISPATCH_LEVEL ceiling |
| `IoConnectInterrupt`, `IoDisconnectInterrupt`, `IoConnectInterruptEx`, `IoDisconnectInterruptEx` | Assigned latched/level lines, exclusive/shared handlers and locks, legacy and Ex versions 1/2/4 at PASSIVE_LEVEL; precise epoch lifetime |
| `KeSynchronizeExecution`, `KeAcquireInterruptSpinLock`, `KeReleaseInterruptSpinLock` | Real BOOLEAN synchronization callback and the same nonrecursive lock at synchronization IRQL; original caller IRQL and ownership restored |
| `IoInitializeRemoveLockEx`, `IoAcquireRemoveLockEx` | Exact extension-owned 32/120-byte opaque locks; initialization at PASSIVE_LEVEL and NULL/repeated Tag acquisition through DISPATCH_LEVEL |
| `IoReleaseRemoveLockEx`, `IoReleaseRemoveLockAndWaitEx` | Matching opaque Tag release; PASSIVE_LEVEL resumable REMOVE drain after actual bus receipt, with separate callback/device lifetime |
| `PoCallDriver`, `PoStartNextPowerIrp` | Forward an owned power IRP; Vista+ start-next validation without a serialization handshake |
| `PoSetPowerState`, `PoRequestPowerIrp` | Independent per-device notification state, Query/Set children from explicit per-PDO response FIFOs, and native WAIT_WAKE with optional PIRP/callback; bounded pageable profile above |
| `IoCreateSymbolicLink`, `IoDeleteSymbolicLink` | ASCII `\DosDevices\Name` or `\??\Name` within one session namespace, targeting `\Device\Name` |
| `DbgPrint`, `DbgPrintEx` | Checked Win64 variadic formatting, at most 512 output bytes; all debugger filters enabled |
| `IoGetCurrentIrpStackLocation` | Returns the stack location of the active modeled IRP; normal compiled WDM macros read the same guest field |
| `KeGetCurrentIrql` | Reads current guest IRQL/CR8, including explicit raises and restores; dispatch and workers start at `PASSIVE_LEVEL`, DPCs at `DISPATCH_LEVEL` |
| `KfRaiseIrql`, `KeLowerIrql` | Actual x64 WDK raise/lower imports, including inline helpers; each execution restores saved IRQL values in LIFO order before return. Legal waits at IRQL <= APC_LEVEL preserve their saved raises and resume at the waiting IRQL. Dispatch-level holds cannot suspend. CR8 observes each change; configured CPU0 event preemption uses `scheduling`; arbitrary nested interrupts remain unsupported. |
| `KeEnterCriticalRegion`, `KeLeaveCriticalRegion`, `KeEnterGuardedRegion`, `KeLeaveGuardedRegion`, `KeAreApcsDisabled`, `KeAreAllApcsDisabled` | Nested per-thread APC-disable state. Critical regions and held KMUTEX objects disable normal kernel APCs; guarded regions and IRQL >= APC_LEVEL disable all APCs. New system threads begin inside one critical region. Unmatched leaves and unbalanced callback returns fail; APC delivery is not modeled. |
| `KeInitializeSpinLock`, `KeAcquireSpinLockRaiseToDpc`, `KeReleaseSpinLock`, `KeAcquireSpinLockAtDpcLevel`, `KeReleaseSpinLockFromDpcLevel`, `KeTryToAcquireSpinLockAtDpcLevel` | Resident, aligned executive locks on CPU0; exact owner and acquire/release pairing, saved IRQL restoration, and nonblocking try-acquire. Contended blocking acquisitions stop explicitly because the scheduler cannot make progress while spinning. |
| `IoAllocateWorkItem`, `IoQueueWorkItem`, `IoFreeWorkItem` | Device-owned opaque work items; `DelayedWorkQueue` only, callbacks receive the device and context at `PASSIVE_LEVEL`; queued items cannot be freed |
| `PsCreateSystemThread`, `PsTerminateSystemThread`, `ObReferenceObjectByHandle`, `ObfDereferenceObject`, `ZwClose` | Bounded system-process threads run at `PASSIVE_LEVEL` with a separate guest stack. Kernel handles and referenced opaque thread objects have independent lifetimes. Termination does not return to the guest, signals the thread object, and a normal start-routine return stops explicitly. NULL process/client IDs and NULL or kernel-handle-only object attributes are supported; APC delivery, process priority classes and typed object references are not. |
| `KeSetPriorityThread`, `KeQueryPriorityThread` | Runtime priorities at `PASSIVE_LEVEL`; setters accept 1..31 and return the previous priority. The deterministic initial priority is 8. Known thread objects are required. Priority scheduling is enabled by `scheduling`; dynamic boosts and process priority classes remain unsupported. [driver-scheduling.md](driver-scheduling.md) |
| `KeQueryActiveProcessors`, `KeSetSystemAffinityThread`, `KeRevertToUserAffinityThread` | One processor in group 0: `KeQueryActiveProcessors` returns mask `1` at any valid IRQL. The legacy affinity pair accepts mask `1` at IRQL <= DISPATCH_LEVEL, tracks logical-thread ownership across parked/nested calls, and requires restoration before the outer return. Other masks and unmatched restores fail explicitly. These environment-sensitive calls retain a UNPACK dependency. |
| `KeInitializeDpc`, `KeInsertQueueDpc`, `KeRemoveQueueDpc`, `KeSetImportanceDpc`, `KeSetTargetProcessorDpc` | Opaque DPC storage, four guest callback arguments, `DISPATCH_LEVEL`, duplicate/remove semantics and importance; target CPU0 only |
| `KeInitializeTimer`, `KeInitializeTimerEx`, `KeSetTimer`, `KeSetTimerEx`, `KeCancelTimer`, `KeReadStateTimer` | Notification/synchronization timers; relative/absolute 100 ns deadlines, periodic milliseconds, rearm/cancel and signal queries in virtual time |
| `KeInitializeEvent`, `KeSetEvent`, `KeResetEvent`, `KeClearEvent`, `KeReadStateEvent` | Notification/synchronization events with distinct signal consumption; `KeSetEvent` accepts Increment=0 and Wait=FALSE only |
| `KeInitializeSemaphore`, `KeReleaseSemaphore`, `KeReadStateSemaphore` | Resident counting semaphore with a positive limit, nonnegative initial count, and one count consumed per successful wait; release accepts Increment=0 and Wait=FALSE, and an excess adjustment raises `STATUS_SEMAPHORE_LIMIT_EXCEEDED` |
| `KeInitializeMutex`, `KeReleaseMutex`, `KeReadStateMutex` | Resident KMUTEX with thread-owned recursion across nested callbacks and SEH; KeReleaseMutex returns the previous signed signal state, requires the owner and matching DISPATCH_LEVEL acquisition context, and accepts Wait=FALSE only. Held mutexes block outermost return, reinitialization and storage release. A non-owner release raises `STATUS_MUTANT_NOT_OWNED`. |
| `KeWaitForSingleObject` | One initialized event, timer, semaphore or mutex, or a referenced thread object; nonalertable `KernelMode`, reason `Executive`; zero polling, finite relative/absolute or infinite waits; nonzero/infinite waits require IRQL <= APC_LEVEL |
| `KeWaitForMultipleObjects` | `WaitAll`/`WaitAny` over 1..64 objects under nonalertable `KernelMode`/`Executive`; more than three require nonpaged `KWAIT_BLOCK` storage. Atomic acquisition, array-index results, thread references and timeout release follow [driver scheduling](driver-scheduling.md). |
| `KeDelayExecutionThread` | Nonalertable `KernelMode` relative/absolute delay at IRQL <= APC_LEVEL; resumes the saved guest frame after virtual time advances |
| `IoMarkIrpPending` | Marks the live active IRP; the equivalent WDM macro's stack-control write is also modeled; dispatch must return `STATUS_PENDING` |
| `IoSetCancelRoutine`, `IoAcquireCancelSpinLock`, `IoReleaseCancelSpinLock`, `IoCancelIrp` | Live WDM IRP cancel-routine exchange, nonrecursive system cancel lock with saved IRQL, and synchronous driver-initiated cancellation; the WDK inline helper uses the same IRP field |
| `IofCompleteRequest`, `IoCompleteRequest` | `IO_NO_INCREMENT`; executes completion unwinding, supports stopped/resumed completion and retires IRP/MDL/buffer storage only at its terminal boundary |
| `memcpy`, `memmove`, `memset`, `memcmp`, `RtlCopyMemory`, `RtlMoveMemory`, `RtlFillMemory`, `RtlZeroMemory`, `RtlCompareMemory` | Bounded guest buffer operations, at most 1 MiB per call; non-overlapping copy APIs reject overlaps |

`MmProbeAndLockPages` accepts `KernelMode` ranges within one contiguous loader-owned span of the input driver image at IRQL <= APC_LEVEL. Image pages must be readable and use the existing physical-page quota. The model already owns private resident image backing: `IoWriteAccess` and `IoModifyAccess` permit writable MDL aliases even when the original image view is read-only; `IoReadAccess` retains a read-only alias contract. Original image protections stay unchanged. Image holes and unrelated mappings are excluded, and ownership also identifies images below the user-address cutoff. Locks require unlock and descriptor release. Temporary MDLs over loader-owned image pages no longer add a recovery dependency after every alias is unmapped, every lock is released and every descriptor is freed. This contract admits kernel-mode cached aliases without a requested address. Live MDLs, other ownership or mapping kinds, and guest reads of physical PFN identities remain dependencies; reads by modeled copy, move and compare services count too.

`KernelDispatcher` decodes semaphore `Count`, `Limit` and `Adjustment` as signed 32-bit `LONG`, mutex `Level` as 32-bit `ULONG`, and `Wait` as 8-bit `BOOLEAN`, ignoring undefined register bits under the [Windows x64 ABI](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention?view=msvc-170). Invalid low-width values and semaphore overflow fail before object state changes.

API IRQL ceilings come from `KernelAPIIRQL.def`, with argument-dependent
checks in the owning model. DPCs cannot call registry APIs or allocate, free
or access paged pool; Unicode `DbgPrint` conversions require `PASSIVE_LEVEL`,
while supported ANSI output and nonpaged operations remain usable at
`DISPATCH_LEVEL`. Callback stacks have bounded ranges; an escaping stack
pointer cannot enter another blocked worker’s stack. Armed timers in a device
extension prevent premature device retirement.

Timer expiry satisfies already registered waits before a DPC can reset or
rearm the timer. Queued DPCs run before awakened `PASSIVE_LEVEL` frames
resume. Completing an IRP rejects release of request storage that still
contains a queued DPC; the failure precedes completion and buffer
invalidation.

`DbgPrint` formatting supports integer `d/i/u/o/x/X`, pointer `p`, text `s/c`,
`%%`, counted Unicode `wZ/lZ`, wide `ls/ws`, flags, width/precision including
`*`, and Windows integer length modifiers. At most 32 variable arguments and
1024 format bytes are read. Width and precision are limited to 512. Floating
point, `%n`, unknown combinations and non-ASCII text conversions stop explicitly;
the model does not guess a Windows code page or call host printf on guest data.

The original RegistryPath record and buffer expire when DriverEntry returns.
Drivers that need the string later must copy it during initialization.

A worker is removed from the queue before its callback begins, so that callback
may free its own work item. Freeing an item that is still queued, duplicate
queueing, stale handles and callback targets outside executable guest memory
fail explicitly. The device reference survives until the callback returns.
Requested unload requires all work items to be freed and queued work to finish.
CPU context save/restore includes general, SIMD, FPU and control state; guest
memory stays shared and faulted CPUs cannot be resumed through a saved context.
Device deletion is deferred while file objects or queued/running work-item
references remain. Work-item allocation returns NULL when the object arena
is exhausted.

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
not duplicated handles. Buffered and both direct IOCTL methods are supported.
Dispatch must either complete synchronously or use the pending callback
contract above. Invalid output lengths and access to a completed IRP fail
explicitly. Requested unload must
leave no live device, symbolic link, pool allocation, or file object.

The optional root field `"load_address": "0x190000000"` requests rebasing;
omission or `"0x0"` uses the preferred address. Relocation requirements must be
satisfied by the image. No scenario is implied by the original initialization
command or C API.

Only `load_address`, `requests`, `unload`, `kernel_exports`, `registry`, and `pnp_devices` are accepted at
the root. Ordinary file requests accept `kind`, optional `device` or `device_id` (mutually exclusive), and optional `file`.
IOCTLs require `code` and accept `input`, `output_size`, and `direct_input`.
A `read` accepts `output_size` and `byte_offset`; a `write` accepts `input` and
`byte_offset`. Offsets default to zero, accept integers or hex strings, and must
fit a nonnegative signed 64-bit value. Lifecycle requests reject transfer fields.
Unknown or duplicate fields are rejected.
`code` accepts an unsigned 32-bit JSON integer or a `0x` hexadecimal string.
`input` is an even-length hexadecimal byte string without a prefix or spaces;
omission means empty input. `output_size` is an unsigned JSON integer; omission
means zero. Numeric fractions and floating-point spellings are rejected.

A CREATE request may set Boolean `asynchronous_file: true`; omission or false
keeps the existing synchronous file object. The asynchronous open clears
`FO_SYNCHRONOUS_IO` on its guest `FILE_OBJECT` and does not set
`IRP_SYNCHRONOUS_API` on subsequent file IRPs. It allows overlapping
READ/WRITE/IOCTL requests on that file when an earlier transfer explicitly
defers callback draining. CLEANUP and CLOSE still wait for every earlier IRP
on the file to complete and finalize. The model does not maintain an implicit
current byte offset for an asynchronous file; READ/WRITE `byte_offset` remains
an explicit per-request fact with a default of zero. Non-CREATE requests reject
`asynchronous_file`, even when false.

Only READ/WRITE/IOCTL requests accept optional `cancel_after_100ns`, a JSON integer from 0 through `INT64_MAX` (9223372036854775807). It schedules cancellation relative to request submission in virtual 100 ns units, not wall-clock time. For KMDF, zero applies after framework routing and before the guest I/O callback. With caller-context preprocessing, it applies after that callback enqueues the request and the queue routes it; if routing already completed the request, completion wins. For WDM, zero applies after dispatch returns. When the live IRP has a registered cancel routine, the scheduler clears that field and invokes the routine at `DISPATCH_LEVEL` with the cancel spin lock held. The routine must release the lock using `Irp->CancelIrql` before completion or return. Without a registered routine, cancellation sets `Irp->Cancel` but does not complete the request. The WDK inline `IoSetCancelRoutine` exchange, `IoAcquireCancelSpinLock`, `IoReleaseCancelSpinLock` and driver-initiated `IoCancelIrp` use the same IRP and lock state; `IoCancelIrp` calls a registered routine synchronously and returns whether it did so. For positive delays, time advances to timer, wait or cancellation deadlines only when no callback/frame is ready. General queue and PnP cancellation remain unsupported. Each request report includes `cancel_requested_at_100ns`, either the actual absolute virtual cancellation time or null if cancellation never occurred, including when completion won first. A cancellation request alone does not complete an IRP or prescribe its final status.

For a nonempty WDM neither-I/O READ/WRITE or `METHOD_NEITHER` IOCTL, optional Boolean `user_unmap_after_dispatch` revokes the request’s original and declared user virtual addresses after dispatch returns and before queued work or cancellation runs. A previously locked MDL and its system alias retain the same physical bytes until the driver unlocks them; raw user-pointer access and new locks fail. If the output user address is revoked, `output_hex` is empty because no caller-visible output buffer remains, even if the driver completes successfully with a nonzero Information value. The backing is retained for existing MDL pins and is never reused in this bounded scenario. Remapping these original scenario allocations and arbitrary unmap timing are not modeled. Independent MDL views have the separate reuse rules below.

File requests may set `requestor_process_id` to a synthetic integer from 5 through `UINT32_MAX`; the default is 4096. `IoGetRequestorProcessId` returns that identity for a live file IRP and zero for an IRP without a requestor thread. `PsGetCurrentProcessId` returns the process that created the current thread: the requestor during foreground file dispatch and 4 in a modeled system work item. `IoGetRequestorProcess` returns an opaque process object for a live IRP; `IoGetCurrentProcess` and `PsGetProcessId` identify the process in the current APC state. A system work item may use `KeStackAttachProcess` with aligned writable `KAPC_STATE` storage on its own stack or in nonpaged pool while a request from that process remains live, access that process's original user VAs, then call `KeUnstackDetachProcess` with the same state before returning. Attachments to exited processes, unmatched detach, waits, IRP completion and other complex API calls while attached fail explicitly. `PsGetCurrentProcessId` remains 4 during attachment because the work item was created by the system process. Other callback thread identities and process attachment outside work items remain unsupported. Switching dispatch to another requestor makes the first requestor's original user VAs inaccessible while locked MDL system aliases remain valid. For a nonempty WDM neither-I/O transfer, Boolean `requestor_exit_after_dispatch` revokes all original and declared user VAs belonging to that requestor after dispatch returns. Later CREATE/READ/WRITE/IOCTL requests from the exited identity are rejected; explicit CLEANUP and CLOSE requests remain available for teardown. Process exit does not imply cancellation, and the two post-dispatch revocation fields cannot be combined. This bounded event does not model automatic handle rundown, reuse of original scenario allocation addresses or arbitrary exit timing.

For direct IOCTLs, `input` initializes the first system buffer, while
`direct_input` initializes the separate MDL-described second buffer, padded
with zeros to `output_size`. `METHOD_IN_DIRECT` requires read access; it does
not imply a read-only system mapping. Both methods use readable/writable
scenario buffers. `MdlMappingNoWrite` removes mapping write access and
`MdlMappingNoExecute` removes execute access. Unmapping revokes the system VA;
remapping retains the same locked data. Completion expires the MDL and mapping.
The public MDL fields and built physical PFN arrays used by WDM macros are
modeled read-only except for `MDL.Next`. Process fields, unbuilt PFN access,
hand-built MDLs and direct access through raw UserBuffer are rejected. User
mappings follow the process-owned MDL contract below. A zero-length direct buffer has a null MDL.

`IoAllocateMdl` allocates metadata for a nonempty, nonoverflowing buffer without
probing or locking it. The modeled descriptor, including its PFN array, must
fit its 16-bit size field; page offsets count toward PFN capacity. Descriptor
storage is independent of the described buffer size. `Irp` may be NULL or a live
modeled IRP. A primary descriptor replaces the current driver-owned chain head;
the detached descriptors remain driver-owned. A secondary descriptor appends
to the current chain, or becomes the head when the chain is empty. The original
request-owned direct-I/O MDL must remain reachable and cannot be replaced or
freed by the driver. `ChargeQuota` must be FALSE. Arena exhaustion returns NULL.
`MmBuildMdlForNonPagedPool` requires the entire described range
to belong to one live nonpaged pool allocation. The safe helper and ordinary
WDM macro reuse its original address, preserving aliases and existing
permissions even when new no-write/no-execute flags are supplied. Additional
system mappings and unmapping are rejected. `IoFreeMdl` expires only the named
driver-owned descriptor: it neither unlinks it nor follows `Next`. A manually
freed user MDL must first be unlocked. The pool buffer has its own lifetime;
both release orders are supported when freed storage is not used afterward.

Drivers may edit `MDL.Next` and `IRP.MdlAddress` to insert or detach modeled
descriptors. Final IRP completion validates the current chain before mutation,
unlocks attached user descriptors, revokes their aliases and retires all
attached descriptors. Detached descriptors and pool backing remain driver-owned.
Cycles, unknown or freed links, descriptors shared between live IRPs and private
WDF descriptors spliced into WDM chains fail explicitly. Live DMA and dispatcher
dependencies prevent premature retirement. Other modeled MDL fields and built
PFNs remain read-only; process fields, unbuilt PFN access and hand-built
MDLs remain unsupported. Unload must release every
remaining driver-owned descriptor.

`MmAllocatePagesForMdl` and `MmAllocatePagesForMdlEx` allocate independent pages
from the bounded physical RAM arena, honoring Low/High/Skip constraints,
partial allocation, `MM_ALLOCATE_FULLY_REQUIRED` and aligned contiguous chunks.
Legacy pages acquire cache identity on first mapping; Ex fixes it at allocation.
`MmFreePagesFromMdl` releases pages and their system mapping after checking live
user, partial and DMA dependencies; `ExFreePool` separately releases the MDL.
An IRP may own a partial view, while the independent source remains driver-owned.
`MmProtectMdlSystemAddress` applies documented PAGE permissions to an existing
private system mapping without changing PFNs, cache type or independent user
view permissions. Unmapped or invalid protection requests return their NTSTATUS.
Shared canonical pool pages cannot be reprotected through an MDL. System alias
addresses can be reused after unmapping. `MmProbeAndLockPages` also supports
KernelMode pool ranges: paged memory requires at most APC_LEVEL to lock, while
nonpaged memory permits DISPATCH_LEVEL. Locked physical pages remain resident
at DISPATCH_LEVEL; freeing their pool owner before unlocking fails.

`IoBuildPartialMdl` describes a nonempty subrange of a built source MDL;
length zero selects the remainder. The target must have enough PFN capacity.
Partial descriptors share physical pages without taking another page lock.
They either inherit a live source system mapping or create an independent
mapping through `MmGetSystemAddressForMdlSafe`. Freeing or preparing a partial
MDL for reuse releases only a mapping it owns. The real WDK inline
`MmPrepareMdlForReuse` can then precede another `IoBuildPartialMdl` call.
A nonpaged source descriptor can be freed independently; a root user lock or
borrowed system alias cannot retire while a dependent partial MDL remains.
Completion checks the entire chain before releasing dependencies, regardless
of descriptor order. Active DMA prevents rebuild and premature retirement.

`MmMapLockedPagesSpecifyCache` accepts `UserMode` in a live
requestor context, including the bounded attached work-item context, at or
below `APC_LEVEL`. Each call creates a separate process-owned view of the same
physical backing without changing `MappedSystemVa`. Views are always
non-executable; `MdlMappingNoWrite` makes a view read-only. Pool backing must
be nonpaged and occupy complete private pages. Mapping a user view again with
`MmProbeAndLockPages` retains the original physical identity. A requested
address must preserve the MDL page offset and lie in the dedicated user-view
arena; exhaustion raises a catchable `STATUS_INSUFFICIENT_RESOURCES`.
`MmNonCached`, `MmCached` and `MmWriteCombined` requests retain the cache
attribute already assigned to the physical pages. Existing ordinary RAM is
cached; mapping a second view does not change that attribute. Reserved cache
values and conflicting physical-page registrations fail explicitly.

`MmUnmapLockedPages` requires the exact user address/MDL pair and the creating
process. Unmapping an original scenario VA leaves independent MDL views and
system aliases intact. Process exit revokes only that process's user views;
locked physical pages and system aliases survive. Live views prevent descriptor,
root-lock, pool and request-storage retirement. Unmapping retires the exact
view and releases its address and mapping budget for reuse. Independent pins
and system aliases continue to identify the original pages after that address
is reused for different backing or permissions.

Different modeled processes can simultaneously own views at the same numeric
address. A process switch replaces the complete active view set after pure
preflight; switching back restores that process's backing and permissions.
Unmapping or exiting another process cannot remove the active process's view.
A live opaque dispatcher object in a retiring view prevents switching until
the object is retired. This bounded view model does not implement arbitrary
virtual-memory allocation, page tables or a general process manager.

For a WDM `METHOD_NEITHER` IOCTL, the input pointer in
`Type3InputBuffer` and the output pointer in `IRP.UserBuffer` refer to separate
user allocations. Neither pointer creates `SystemBuffer` or `IRP.MdlAddress`.
The request runs in its modeled requesting-process context. `ProbeForRead`
checks the user range and alignment but does not touch pages; a later CPU read
can still raise a catchable `STATUS_ACCESS_VIOLATION`. `ProbeForWrite` checks
the range and alignment and touches each page. Zero-length probes return
without inspecting the pointer or alignment. A supported C `__except`
handler can catch these exceptions and in-context user CPU read/write faults;
kernel-address, interrupt and invalid-instruction faults stay terminal.

A driver can describe one live user allocation with `IoAllocateMdl`, then
call `MmProbeAndLockPages` in `UserMode` at or below `APC_LEVEL` for read or
write access. This pins the allocation's modeled physical pages independently
of the request packet. `MmGetSystemAddressForMdlSafe` returns a shared kernel
alias; writes through it change the user buffer. `MmUnlockPages` revokes that
alias and unpins the pages, and `IoFreeMdl` requires the MDL to be unlocked.
The modeled user VA arena, process context, one-allocation locking rule and
mapping budget are explicit limits. A `METHOD_NEITHER` IOCTL with a nonempty
buffer may set `user_input_access` or `user_output_access` to `read_write`
(the default), `read_only` or `no_access`. The two protections are independent.
`no_access` retains a non-NULL user pointer whose pages fail CPU access and
locking. `ProbeForRead` still performs only its range/alignment check, while
`ProbeForWrite` and `MmProbeAndLockPages` can raise an access violation. For
neither READ/WRITE, `user_input_access` is valid only for WRITE and
`user_output_access` only for READ. These fields are rejected for buffered or
direct transfers and empty buffers. Arbitrary
process address spaces and arbitrary user unmap timing remain unmodeled. WDM cancellation is
covered only for file READ/WRITE/IOCTL IRPs with a registered driver cancel
routine; arbitrary concurrent queue races remain outside this profile.
The report's `configuration.user_page_access` lists only explicit protection
facts, keyed by zero-based `source_request_index`; omitted directions use
`read_write`.

A METHOD_NEITHER IOCTL or neither READ/WRITE can also declare `user_buffers`
and `user_pointers`. Each buffer has a request-local `id`, a nonzero `size`,
optional initial `input` hex bytes (zero-padded), and optional `access` with the
same three page rights. Each pointer has `source` and `target` references:
`{"buffer":"input"|"output"|"memory", "offset":N}`; only `memory` requires an
`id`. The source names one complete eight-byte x64 pointer slot. The target
may point inside a buffer or exactly at its end. Slots need not be aligned,
but overlapping source slots, unknown IDs and out-of-range references fail
before execution. Repeated targets share bytes; cyclic pointer graphs are
valid. No private IOCTL layout is inferred and no host addresses are exposed.

The scenario permits at most 64 additional buffers and 256 pointer slots in
total. IDs are 1–64 ASCII bytes matching `[A-Za-z0-9][A-Za-z0-9_.-]*`. Each buffer
is at most 64 KiB, and its full size counts toward the existing 512 KiB
scenario buffer budget. Initial
bytes and pointer fixups are written before dispatch without relaxing final
page rights. Neither READ accepts only the original `output` direction and
neither WRITE only `input`; buffered/direct transfers reject these declarations.

Declared regions use the existing requestor process, page rights and MDL
backing. Completion does not revoke user allocations. Request unmap revokes
all its original and declared VAs, while process exit revokes every allocation
of that PID, including completed requests; already locked aliases retain the
same physical bytes until unlock. WDF can probe and lock declared regions in
`EvtIoInCallerContext`, then use their WDFMEMORY aliases in queue callbacks.
Cross-request IDs, arbitrary process mappings and remapping the original declared scenario allocations remain unsupported.

`configuration.user_memory` preserves declarations and pointer slots by
`source_request_index`. Each request's `user_buffers` reports `id`, hexadecimal
`address`, `size`, `access`, `revoked` and `backing_hex`. The latter is a diagnostic
RAM snapshot at termination, including after revocation or faults; it grants
no guest access and does not replace caller-visible `output_hex`. Snapshotting
never invokes MMIO or clears a fault. The
[nested-user example](examples/driver-nested-user-scenario.json) exercises a
shared embedded pointer, interior offsets and a locked worker after unmap
through the genuine WDK fixture. C API, CLI and Python use this same JSON.

When an IOCTL has a nonzero `output_size`, `Information` must not exceed that
size, even when the input buffer is larger. An IOCTL without an output buffer
may return a driver-defined result in this field, and no output bytes are
copied. `information_hex` preserves its exact raw64-bit value.

For READ/WRITE, `DO_BUFFERED_IO` or `DO_DIRECT_IO` selects buffered or direct
transfer. With neither flag, the original user VA appears only in `IRP.UserBuffer`:
WRITE holds the input and READ holds the output. No SystemBuffer or MDL is
created implicitly. A driver must probe and access it in caller context or
lock it before deferring work. `user_input_access` applies to neither WRITE and
`user_output_access` to neither READ; explicit rights on buffered/direct
READ/WRITE stop before dispatch. Conflicting flags stop. Information is checked
against the transfer length; writes return a count and reads return bytes.

`kernel_exports` maps routine names to explicit availability booleans, for
example `"kernel_exports": {"OptionalRoutine": false}`. Modeled exports and
static imports receive stable addresses shared with `MmGetSystemRoutineAddress`.
An explicitly absent export resolves to NULL and cannot satisfy a static import.
A declared present export without an API model resolves to a lazy trap. An
unknown dynamic name stops with an unspecified-availability diagnostic; absence
is never inferred from missing implementation. Names are bounded printable
ASCII and resolution is case-sensitive. The inventory is a concrete scenario
property, not a claim to match every Windows release.
`IoGetCurrentIrpStackLocation`, `IoMarkIrpPending` and `MmGetSystemAddressForMdlSafe` are modeled WDM header helpers; this does not declare them exported by default, so their export availability requires a static import or an explicit `kernel_exports` declaration.

Scenario text is limited to 2 MiB, with at most 64 requests, at most 65536 bytes
per input or output buffer, and at most 512 KiB total requested bytes, including
`direct_input` contents.
Instruction, observation, guest-memory, and time budgets apply across the
entire scenario. The 1 MiB arena also holds objects and metadata, so an image
can exhaust model memory before consuming the maximum scenario buffers.

## Registry scenarios

The optional `registry` array defines a concrete session-local registry tree.
Each key has a required `path` and an optional `values` array; each value has
`name`, unsigned integer `type`, and hexadecimal `data`. An empty value name
selects the default value. For example, a DWORD value is
`{"name":"Mode","type":4,"data":"01000000"}`. Value bytes are preserved exactly;
the model does not repair string terminators or expand environment variables.

Paths must be absolute ASCII below `\Registry\Machine` or `\Registry\User`.
Key ancestors are created implicitly. Key and value identities are compared
case-insensitively using ASCII rules; non-ASCII names and duplicate identities
are rejected. Omission leaves registry availability unspecified and registry
calls stop. `"registry": []` explicitly describes an empty namespace. No key,
value, host registry data, or service configuration is inferred from the driver.

`ZwOpenKey` and `ZwCreateKey` return independent opaque handles, with per-handle
access checks for query, set, child creation and deletion. The configured tree
grants supported `KEY_ALL_ACCESS` bits, including ordinary `KEY_READ` and
`KEY_WRITE` masks. This is an explicitly accessible test tree, without Windows
ACLs or privilege evaluation. Generic rights, `MAXIMUM_ALLOWED`, alternate
registry views, custom security descriptors, classes and symbolic links are
unsupported. Relative creation requires a direct parent handle with
`KEY_CREATE_SUB_KEY`. Input keys are nonvolatile; newly created keys may be
volatile, and a nonvolatile child of a volatile key is rejected. There is no
reboot or disk persistence model.

`ZwQueryValueKey` implements Basic, Full, Partial and their defined Align64
information classes, including exact lengths, aligned data, partial output and
distinct `STATUS_BUFFER_TOO_SMALL` / `STATUS_BUFFER_OVERFLOW` results.
`ZwSetValueKey` and `ZwDeleteValueKey` change only this session's tree.
`ZwDeleteKey` rejects a key with live children; handles to a deleted key return
`STATUS_KEY_DELETED` until closed. `ZwClose` releases a handle independently of
the key, and requested unload fails while registry handles remain open.

Limits are 256 keys including ancestors, 1024 total values, 65536 bytes per
value, 512 KiB total value data, 1024 ASCII bytes per key path, 256 bytes per
value name, and 256 simultaneously open handles. Creation and mutation enforce
the same limits as scenario preflight. The report's `configuration.registry`
retains the original input; `registry` lists final live key paths and values,
including mutations observed before a stop. Unspecified registry state reports
as null. Volatility and handle identities are not part of that value snapshot.

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

## Zero sample acceptance check

The additional [Zero validation script](../scripts/validate_zero_driver_sample.py)
builds Pavel Yosifovich's unmodified public Zero WDM sample from the revision
and hashes in [its manifest](../unittests/emulation/fixtures/zero-validation.json).
Run `python3 scripts/validate_zero_driver_sample.py` with the same toolchain
requirements. Source, MIT license, commands, scenario and report are retained
under `build-release/driver-validation/zero` by default. Nine requests exercise
direct reads across page boundaries, write counts, guest atomic statistics and
a buffered statistics IOCTL. The sample's zero-length read failure and missing
CLEANUP handler remain visible; expected CLI exit is 2, followed by successful
close and unload. This validation script succeeds only when those exact results
and all output bytes agree.

## Reports and SDK

The JSON report distinguishes `stop_reason`, nullable `nt_status` and
`nt_success`, the stopping PC, and instruction count. It preserves the API
calls and observable state collected before a stop, including device objects
and driver callback addresses. Guest addresses are hexadecimal strings so
JSON consumers do not lose 64-bit precision.
The `configuration` object records the run's limits, service name,
`kernel_exports` overrides and original `registry` input.
The profile is `wdm-x64-scheduled-v99`. `nt_status` remains the DriverEntry
result, while `scenario_success` describes initialization and completed
requests together. `phase`, `requests`, and `unload_completed` identify which
parts of the requested lifecycle ran. Each API call and CPU write also records
its phase (`driver_entry`, `add_device:<ID>`, `request:N`, `callback:N`, or `unload`). Each request
reports dispatch and I/O statuses, completion, information length, and returned
`output_hex` bytes.
Pending requests retain `STATUS_PENDING` in `dispatch_status`; the callback's
final completion status is reported separately in `io_status` and determines
the request's contribution to `scenario_success`.
`preferred_image_base` describes the original PE base. `security_cookie` is the
guest address of the initialized cookie, or `"0x0"` if none was required.
Request fields are `kind`, `device`, `device_id`, `pnp`, `file`, `requestor_process_id`, `byte_offset`, `code`, `irp`, `completed`,
`cancel_requested_at_100ns`, `dispatch_status`, `io_status`, `information`, `information_hex`, and `output_hex`.
`information_hex` preserves the raw64-bit `IoStatus.Information` as an exact
hexadecimal string; the existing numeric `information` field remains available.

`ExRaiseStatus` passes the low 32-bit NTSTATUS to the guest exception handler; `ExRaiseAccessViolation` and `ExRaiseDatatypeMisalignment` raise `STATUS_ACCESS_VIOLATION` and `STATUS_DATATYPE_MISALIGNMENT`. The profile follows the individual Microsoft DDI pages: ExRaiseStatus permits `APC_LEVEL`, while the two no-argument routines require `PASSIVE_LEVEL`. Some WDK SAL annotations permit APC_LEVEL for the wrappers; this profile retains the documented stricter limit. A raised call keeps `result: null` and records the code in `detail`; it never reports a successful API return.

Exception delivery uses the image's decoded x64 version-one unwind tables and
`__C_specific_handler` scopes. Search executes real guest filters in table
order: zero continues searching, a positive result selects the handler, and a
negative result requests continuation. Constant `EXCEPTION_EXECUTE_HANDLER`
scopes select their handler directly. Once a handler is selected, unwind runs
the exited `__finally` callbacks before the actual handler body. Cleanup does
not run during search or when a filter resumes the faulting execution. Ordinary
helper-frame unwinding restores saved nonvolatile general registers and stays
within the original execution's stack. `GetExceptionCode()` observes the raised
code, and handlers can raise into an enclosing supported scope.

C SEH ranges remain half-open. A valid `__C_specific_handler` landing pad may lie inside its protected range: [LLVM 20.1.8](https://github.com/llvm/llvm-project/blob/llvmorg-20.1.8/llvm/lib/CodeGen/AsmPrinter/WinException.cpp#L600-L608) emits `EndLabel + 1` as the scope end. The Windows OS model preserves the raw endpoints and independently validates executable targets, function ownership and continuation identity, including after rebasing. `KernelSEHContinuationCases.def` retains the original fixture layout; `ScopeEndLabelMayOverlapTheHandlerLandingPad` checks constant handlers and filters. Companion tests preserve the exclusive end and reject invalid targets without consuming the dispatch state. These pure checks run in `NeverDNativeDriverTests` with Unicorn disabled.

Target unwind also uses the raw scope end: a `finally` whose protected range still contains the handler target is not exited. `FinallyRespectsRawScopeEndAtHandlerTarget` tests both sides of that boundary and, on Windows x64, compares them directly with `ntdll.dll!__C_specific_handler`. NeverD does not repair compiler-generated ranges. The Clang 20/21 builds of the original fixture return guest failure in modes `T` and `J` because their biased end includes the selected target; Clang 23 builds execute both cleanups. [LLVM change #144745](https://github.com/llvm/llvm-project/pull/144745) removes the old `+1` bias. These compiler-specific outcomes are distinct from backend failures.

Filter callbacks receive stable `EXCEPTION_POINTERS`, exception-record and
`CONTEXT` storage on a separate bounded stack. Callback execution preserves the
original full CPU state, including floating-point/SIMD registers and flags.
The exposed `CONTEXT` supports integer/control fields; a negative filter may
change supported GPRs, RIP/RSP and arithmetic/direction flags to resume an
in-context user read/write CPU exception or integer division exception.
Processor `#DE` maps to `STATUS_INTEGER_DIVIDE_BY_ZERO`, including quotient
overflow; it enters the same image-owned SEH search and unwind path on explicit
KVM/WHP and Unicorn transports. A negative filter may repair the divisor and
retry the original instruction. Invalid context changes fail before
resume. Exception pointers, exception-record fields and unsupported context
fields are validated after every filter; mutating them remains unsupported.
Continuing a modeled API raise remains unsupported. Callback identity
inherits the parent thread, process and user-access authority without giving
an unrelated worker access to the requesting process.

Native x64 KVM/WHP delivers actual `#XM` faults to driver C SEH through `X64SIMDException`. Hardware-fault `CONTEXT` records preserve XMM0–15 and MXCSR. Filters, exception-unwind finally callbacks and selected handlers run with MXCSR `0x1f80` and DF cleared. A negative filter can edit XMM registers and top-level `CONTEXT.MxCsr`, masked by the guest CPU profile, before retrying the original instruction; `FltSave.MxCsr` does not control kernel restoration. Modeled API raises retain integer/control records; x87/AVX context edits remain unsupported.

Nested filter exceptions retain a linked `EXCEPTION_RECORD` and revisit the
suspended protected scopes; collided finally unwinding resumes after the
already entered cleanup. The planner restores nonvolatile GPRs and full
XMM6–XMM15 values, checks chained V1 records, and unwinds partial prologues.
Canonical epilogues are decoded from current executable guest bytes: only
remaining stack adjustments, nonvolatile pops and the return are simulated.
C++ personalities and incomplete metadata still fail explicitly. An uncaught API or supported user-memory
exception or integer division exception stops with `model_error`; other CPU memory, interrupt and
invalid-instruction faults remain terminal.

`__GSHandlerCheck_SEH` checks the live image security cookie before language
handler search and again during unwind, including frames without finally
callbacks. Fixed and dynamically aligned cookie slots use checked signed
frame offsets; cookie encoding uses the original frame pointer. The wrapped
handler flags control C scopes independently of the wrapper's cookie checks.
Prologue and epilogue unwinding does not read an unestablished cookie. A
mismatch stops before the affected filter, cleanup or handler can run. Standalone `__GSHandlerCheck` also checks cookies during search and unwind without inventing C scopes. Recognition requires an exact symbol or import identity; anonymous code is not guessed from instruction patterns. GS/C++ wrappers and C++ exception personalities remain unsupported. Microsoft distinguishes SEH from C++ exceptions and states that the Windows kernel does not support C++ exceptions; this is a kernel contract, not an unimplemented WDK exception facility ([Handling Exceptions](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/handling-exceptions)). See
[Microsoft's /GS contract](https://learn.microsoft.com/en-us/cpp/build/reference/gs-buffer-security-check).

The original `driver_wdm_seh.c` fixture uses genuine WDK headers with `/GS-` for its C routines and explicit original GS-protected assembly in `driver_seh_gs.h`. Both GS forms execute the linked WDK cookie checker before raising, and negative variants corrupt only the stored cookie. Configure `NEVERD_WDM_SEH_FIXTURE` and `NEVERD_WDM_SEH_CFG_FIXTURE` for normal and active-CFG images. The [driver-seh-scenario.json](examples/driver-seh-scenario.json) example rebases the image, catches an API exception in DriverEntry and unloads. The separate genuine-WDK `driver_wdm_neither.c` fixture exercises request pointers, probes, in-context CPU exceptions and locked user MDLs in normal/active-CFG and preferred/rebased images through `NEVERD_WDM_NEITHER_FIXTURE` and `NEVERD_WDM_NEITHER_CFG_FIXTURE`. The [driver-neither-scenario.json](examples/driver-neither-scenario.json) example checks both plain and locked-alias output bytes through the public scenario interface.

The same fixture can mark a neither IOCTL pending after locking input and
output pages in caller context. A work item at `PASSIVE_LEVEL` uses the kernel
aliases, unlocks and frees both MDLs, frees its work item and completes the
IRP. A separate work item that dereferences the raw user VA stops with an
explicit requesting-process diagnostic; queued work does not inherit the
caller's address context.

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

The optional scenario boolean `trace_memory_writes` defaults to true. False leaves `writes` empty and charges the event budget only for API calls. Memory validation, committed-write observers, and instruction/time limits remain active. The report records the selection in `configuration.trace_memory_writes`; C++ uses `DriverOptions::TraceMemoryWrites`.

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
existing loader; Windows object/API behavior belongs to `lib/emulation/os/windows`;
CPU state and execution belong to the Unicorn adapter. The adapter and model
use the same guest-memory interface. No Windows API behavior belongs in the
Unicorn fork.

## x64 floating-point context state

The shared architecture layer retains x87 control/status, TOP, physical
nonempty tags, opcode, instruction/data pointers and all eight 80-bit payloads
in KVM, WHP and both Unicorn paths. `FP0`–`FP7` use `RegisterValue`; scalar
access rejects truncation, and `FPTag` is the physical abridged mask.
`NeverDX64FPTests` verifies every TOP, exact host FXSAVE/FXRSTOR comparisons and
context restoration. This state coverage does not admit checked x87
instructions or certify all software rounding semantics.

Use `executionCapabilities(Contract, ISA, Backend)` to query the selected profile. `NativeLegacyX64` describes native x64 driver execution. `NeverDNativeDriverTests` validates the original corpus and can run with Unicorn disabled.

Checked ARM64 has one complete state boundary. `Registers.def` defines 39 scalar fields and 32 128-bit vectors; `captureAArch64State` stages every read, applies declared widths and NZCV normalization, then publishes once. Unicorn, KVM and WHP transfer the same inventory, including TPIDR_EL0, TPIDRRO_EL0, TPIDR_EL1, FPCR and FPSR. Native adapters enable FP/SIMD through CPACR_EL1. Any scalar/vector read failure or cancelled entry preserves all caller state.

ARM64 KVM/WHP startup executes the private `AArch64MachineProbe.def` program: NOP, FP32 addition rounded toward positive infinity and a two-lane SIMD addition. Each step compares all 39 scalar fields and 32 vectors, including TLS, NZCV, cleared upper destination bits and retained/cumulative FPCR/FPSR state. The probe uses supervisor monitor storage and one overall deadline. Success verifies this bounded initialization program; independent native ARM64 workload validation remains outstanding. The program also executes A/B return-address signing/authentication with the keys disabled and all four BTI forms on unguarded pages.

The probe also executes `MRS CTR_EL0` twice, `DC CVAU`, `DSB ISH`, `IC IVAU` and `ISB`, checking stable cache geometry and complete state. Checked EL0/EL1 execution admits the original words, all named baseline DSB options and only ISB SY. CTR comes from the selected virtual CPU and may differ between transports. Cache targets must identify readable ordinary RAM with the current privilege, including unaligned addresses and aliases; other targets fail as unsupported. Maintenance produces no data read/write observer events. The projection keeps instruction execution coherent; it does not model private cache contents or parallel hardware SMP. `NeverDAArch64CacheTests` checks full state, read-only page ends, rejected forms, stops, contexts, budgets and guest code updates through cross-page RW/RX aliases. Unavailable KVM/WHP hosts remain explicit skips.

Native x64 KVM/WHP initialization executes `X64MachineProbe.def` in private supervisor pages. One deadline covers NOP, rounded FP32 addition, two-lane SIMD addition, FS/GS loads and CS/SS/CR8 reads; every step compares the complete scalar, XMM, physical x87 and control state. x64 and ARM64 probes require the exclusive physical-memory execution lease. `MemoryProjection` owns cache identity (ISA, address space, mapping generation, privilege and monitor variant) and committed root history per ISA. Builders invalidate before rewriting private bytes; failed replacement cannot reuse partially written tables, and callers cannot supply stale roots. These probes certify bounded initialization only; independent native ARM64 workload validation remains outstanding.

The shared XSAVE decoder distinguishes standard and compacted initial SSE state. With XSTATE_BV[1] clear, both forms initialize XMM registers; standard format still reads and validates MXCSR, while compacted format initializes MXCSR. `X64XsaveCases.def` supplies independent packet layouts and original host XRSTOR programs. `X64XsaveTests.cpp` checks rejected-state atomicity and compares both formats with actual host execution, preserving the caller’s FP/SSE state. The host oracle skips explicitly when the architecture or required instruction feature is unavailable.

`X64FPState.def` declares compacted AVX, AVX-512, CET_U/CET_S and AMX transport layouts, including 64-byte component alignment. Present extension payloads must be architectural zero init state; absent payloads and alignment padding do not define state. Layout bits determine offsets, and unknown layouts, non-initial payloads or incorrect lengths fail before publication. `CompactedOffsetsFollowLayoutRatherThanPresentBits`, `WideLayoutIgnoresAbsentComponentsAndAlignmentPadding`, `InitialCETComponentsDoNotHideFPState` and `InitialWideComponentsDoNotHideFPState` cover 872-byte and 10752-byte WHP packets. This transport support does not admit those extension instructions.

`WhpXsaveRegisters.def` supplements complete XSAVE packets with named x87/SSE control registers. Last opcode and instruction/data pointers are written explicitly and captured from the host; zero packet slots may be supplemented, while conflicting nonzero metadata or inconsistent shared controls fail before publication. `NamedMetadataRestoresOmittedPacketFields` checks the omitted-field case without dropping any FP payload.

Native `FOP/FIP/FDP` follow the host’s x87 save/restore rules. AMD may clear these fields without a pending unmasked exception; snapshots retain observed values. `X64MachineProbe.def` and exact NOP/context tests seed a coherent pending exception so every field remains valid and is compared without masking. The host-process FXRSTOR64/FXSAVE64 oracle checks both states; backends never substitute input metadata for host results.

The shared `encodeX64XsaveState` / `decodeX64XsaveState` codec owns standard/compacted FP/SSE packets, physical TOP rotation, absent-component init state and atomic validation. WHP uses complete XSAVE APIs, preferring `WHvGetVirtualProcessorState` / `WHvSetVirtualProcessorState` with the older XSAVE APIs as a compatibility path. Legacy individual x87 registers cannot replace complete packets. Non-initial extended components, malformed headers, invalid controls and truncated captures fail explicitly. WHP mapping failures retain HRESULT, GPA and size for diagnosis.

`CheckedX64Instructions.def` admits unsigned `MUL` at 8/16/32/64 bits and `CBW/CWDE/CDQE/CWD/CDQ/CQO` through the existing processor transport. `NeverDX64IntegerTests` uses independent `X64IntegerCases.def` encodings and expected values at both privilege levels: partial-register preservation, 32-bit zero extension, both product halves, defined CF/OF results and unchanged flags for sign extension. Ordinary-RAM multiplication retains whole-span permission checks and read observers; a fault or observer stop preserves implicit output registers and PC. Device operands remain unsupported. These cases also run on checked Unicorn; unavailable native transports skip explicitly.

`X64BitInstructions.def` admits register and ordinary-RAM `BT/BTS/BTR/BTC` at 16/32/64 bits. A register bit index is signed at the operand width and selects a complete word; an immediate stays within the base word. Address-size wrapping occurs before FS/GS base addition. The processor supplies CF and written values; `RAMTransaction` keeps the result private until observers accept it. Whole-span permission checks cover separate page allocations and aliases. Stops, callback failures and denied pages preserve the original CPU and RAM. LOCK is limited to naturally aligned modifying memory forms; MMIO and parallel hardware SMP remain unsupported. `X64BitStringTests.cpp` compares independent encodings with actual x64 host execution and checks negative indices, width truncation, cross-page accesses, cancellation and invalid LOCK forms. See the [Intel instruction reference](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).

`X64StringInstructions.def` owns ordinary-RAM `MOVS/STOS/LODS` at 8/16/32/64 bits; `CLD/STD` controls direction without changing other flags. Each REP element validates the entire operand before observations and commits at one restart boundary. Earlier completed elements survive a later fault; cancellation or observer failure leaves the current element untouched. FS/GS applies only to the source, after address-size truncation. AL/AX loads preserve upper bits and EAX loads zero-extend. Zero-count address-size-32 REP requires zero upper count bits and, for MOVS/STOS, zero upper participating address bits: real CPU implementations differ otherwise. REPNE on MOVS/STOS/LODS and STOS/LODS device operands remain unsupported. `X64StringTransferTests.cpp` uses independent host instructions for widths, direction, overlap and zero counts, with separate checks for permissions, aliases, wraparound, faults and resumption. The original WDK resource driver executes all four STOS/LODS widths through `driver_resource_strings.def`.

`X64StringInstructions.def` also owns ordinary-RAM `CMPS/SCAS` at 8/16/32/64 bits with `REPE/REPNE`. Every element validates both complete read operands before observers, updates all six arithmetic flags, and stops on the first matching termination condition. A data fault restores the flags from entry to this uninterrupted REP while retaining completed pointer/count changes; a public resume starts from the published CPU state. Stops and observer exceptions leave the current element untouched. Early termination never reads the next element. FS/GS affects only the CMPS source; SCAS leaves the accumulator and unused source register unchanged. Device operands and ambiguous inactive 32-bit upper halves remain excluded. `X64StringComparisonTests.cpp` compares independent host instructions, flags, direction, aliases, wrapping, permissions and recovery; its Linux x64 signal oracle checks actual fault-time registers. The original WDK resource driver executes both conditional-repeat forms at all four widths through `driver_resource_strings.def`. See the [Intel instruction reference](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).

Live WHP CPUs share one native partition. Its final close and replacement creation use the same registry lock. `WhpResourceCache.h` reuses cooperative VP 0; switching its logical owner first retires that VP and its mappings. Parallel bindings retain separate VPs and private GPA windows. Every register transfer, XSAVE operation and cancellation targets its own VP. x64 preserves the host’s default XSAVE feature set and validates the effective partition with `WHvGetPartitionProperty`. Cooperative scheduling remains the default.

`CheckedAArch64Instructions.def` and `AArch64InstructionEffects` admit bounded baseline FP32/FP64 arithmetic, comparisons, moves and fixed-width SIMD operations at EL0/EL1. FPCR supports four rounding modes, FZ and DN; FPSR retains cumulative status and QC. Unsupported control/status bits are rejected before mutation. FP16 arithmetic, SVE/SME, unmasked exceptions, optional extensions and unlisted forms fail explicitly. This CPU support does not add Windows ARM64 driver loading or another OS environment.

`AArch64InstructionEffects` owns scalar and FP/SIMD single/pair RAM footprints, including operands up to 128 bits. The shared address space validates every page before CPU entry; `RAMTransaction` commits only complete declared physical writes. A 128-bit write observer receives two ordered 64-bit words before effects. Stops and faults preserve RAM, vectors and writeback. Numeric Xn/Vn overlap is valid; wrapping pair footprints are rejected. `NeverDAArch64MemoryTests` uses independent `AArch64CrossPageCases.def` and `AArch64VectorMemoryCases.def` encodings.

KVM x64/ARM64 uses `KvmRunControl` to prepare state, enter `KVM_RUN` and capture state on one private vCPU worker. Preparation runs once across `EINTR` retries; cancelled entry or failed capture cannot publish. `KvmAArch64Machine.cpp` performs translation maintenance and complete scalar/vector transfers on this worker under one step deadline. The caller publishes only after acknowledgement; ISA decoding, RAM transactions, OS policy and observers remain on the caller thread. Native ARM64 runtime evidence is still pending.

KVM compares general registers and the complete FP/SSE state against the last acknowledged debug capture using `X64HostRegisters.def` and `X64FPState.def`, and reinstalls changed input. Host writes and context restoration participate in this comparison; exceptions, cancellation and failures invalidate reuse. Stepping is armed and actual general/FP state is read back for every instruction.

Hardware execution alone does not guarantee lower end-to-end latency. Current native execution performs instruction admission, observation, state transfer and a VM exit for each step. Compare the same original images and scenarios with identical instruction/event budgets and report outcome parity alongside timings; include CLI startup and loading when measuring CLI latency.

Dormant CFG pointer slots remain valid when `GuardFlags` is zero. The loader validates their storage, executable fallback targets and complete `DIR64` coverage on rebasing, and preserves the original guest pointers. Active CFG still requires the image enablement bit and its instrumentation/function-table flags. `DriverGuardCases.def` and `NeverDDriverGuardMetadataTests` cover these cases through Unicorn, KVM and WHP, including builds without Unicorn.

Checked Unicorn uses `MachineRunControl`: one allowance covers ARM64 maintenance, guest execution and complete state capture. `UC_HOOK_CODE` checks the borrowed stop token and deadline at instruction entry; the synchronous engine call retires its hook borrow before returning, while the machine step retains control through publication. Unicorn and WHP stage complete CPU state and check the same control before publishing a successful step. WHP creates its allowance once before preparation. An authenticated x64 CPU exception takes precedence over a stop arriving during capture. The checked RAM transaction discards speculative stores when capture is cancelled; the unrestricted software contract is unchanged. `MachineInterruptedError` distinguishes acknowledged cancellation from host or capture failure. The shared checked CPU returns `Stopped` or `Deadline`, preserves CPU/RAM and permits retry; genuine failures remain `BackendFailure` even with a simultaneous stop.

`RunDeadline::invoke` rejects a stopped or expired WHP entry before calling the host, retains an actual host result during cancellation, and acknowledges interrupt callbacks before releasing the borrowed token. KVM and WHP validate a successfully captured private packet on the owning caller before classifying a concurrent stop or deadline. Genuine host/capture failures and authenticated x64 CPU exceptions retain priority. Ordinary successful state stays private until cancellation checks finish; an acknowledged interruption discards speculative CPU/RAM effects and permits retry. Preparation, native execution and capture share one step allowance. These controls provide cooperative cancellation, without a hard wall-clock guarantee.

## macOS HVF backend

Hypervisor.framework supplies the `hvf` native transport. `auto` selects it for
a matching macOS host ISA; x64 driver execution on Apple Silicon continues to
use Unicorn. The existing ISA and OS contracts remain authoritative. See
[HVF ownership, signing and hardware tests](macos-hvf.md). ARM64 hardware
evidence and Intel runtime coverage are reported separately.

Explicit CPU0 preemption, clock semantics and current limits are described in [driver scheduling](driver-scheduling.md).

## HAL exports and performance counter

`HAL.dll` is a separate, case-insensitive import provider. Static imports and `MmGetSystemRoutineAddress` share exact, case-sensitive export identities across the kernel and HAL; conflicting live identities are refused. `kernel_exports` overrides known HAL routines in their HAL namespace; other explicit declarations remain kernel exports. Unknown HAL imports retain lazy traps, without acquiring a kernel API contract merely from their name.

`KeQueryPerformanceCounter` returns the shared scheduler time in 100 ns ticks with a fixed frequency of 10,000,000 ticks per second. Its optional output pointer is checked for the complete eight-byte write and object lifetime. The call is available at every valid x64 IRQL. Cooperative mode advances time only at existing scheduling boundaries; instruction-clock mode retains its configured timing. Counter reads never create a second clock or advance time themselves. This is a deterministic profile, not a measurement of host hardware. The independently compiled runtime fixture checks static/dynamic identity, frequency and monotonicity at preferred and rebased addresses on native CPU backends.

`RDTSC` and `RDTSCP` read the same 10 MHz scheduler clock as `KeQueryPerformanceCounter`. `RDTSCP` returns zero in ECX for the single modeled processor. EAX/EDX (and ECX for RDTSCP) zero their upper halves; other registers and flags are preserved. Cooperative reads do not advance time. With explicit instruction scheduling, the read observes time after its own admitted instruction is charged, independently of the quantum. Overflow stops before publishing register results. Instruction limits and observer stops still apply. This profile does not measure host TSC frequency or expose host processor identity; MSR access, RDPMC and other unmodeled CPU queries remain unsupported.

`KernelModuleImages` derives readable PE headers and export tables from `KernelExportRegistry`; static imports, dynamic lookup and module enumeration share the same addresses. Provider code remains opaque, and the inventory describes the modeled environment, not the host kernel. Both outputs are checked before writes, including pool lifetimes and overlap. Nt queries require a known kernel previous-mode; Zw queries use the kernel contract. A complete module query retains an explicit recovery dependency, even after its buffer is freed; provider-image pointers are also tracked as borrowed state. `KernelExportTests.cpp` checks PE parsing, permissions, ABI fields, refused writes and recovery dependencies; the original compiled runtime fixture walks these export tables on the CPU backends.
