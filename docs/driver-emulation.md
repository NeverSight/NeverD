**Languages**: [English](driver-emulation.md) | [简体中文](zh-CN/driver-emulation.md) | [繁體中文](zh-TW/driver-emulation.md) | [日本語](ja/driver-emulation.md) | [한국어](ko/driver-emulation.md) | [Français](fr/driver-emulation.md) | [Deutsch](de/driver-emulation.md) | [Español](es/driver-emulation.md) | [Italiano](it/driver-emulation.md) | [Русский](ru/driver-emulation.md) | [العربية](ar/driver-emulation.md)

[← Documentation index](README.md)

# Windows driver emulation

NeverD's optional driver emulator executes the PE entry point of a supported
x64 WDM driver and optionally exercises an explicit serial request
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
Acceptance also covers Pavel Yosifovich's unmodified Zero WDM sample, including
direct READ/WRITE, atomic statistics and a statistics IOCTL. This does not
establish compatibility with arbitrary third-party drivers.

| Driver class or requirement | Current scope | Missing environment |
|-----------------------------|---------------|---------------------|
| x64 software WDM driver using the listed APIs | Bounded x64 WDM initialization, serial buffered/direct requests, work items, timers, DPCs, events and waits, with behavior reports and limits | Each additional executed API must have a defined model |
| `METHOD_BUFFERED` IOCTL | Serial buffered/direct I/O with work-item or DPC completion | Only the API subset below; no concurrent scenario-submitted IRPs or WDM request cancellation |
| `METHOD_IN_DIRECT`, `METHOD_OUT_DIRECT` | Request-owned MDLs and system mappings | physical page identities, DMA and user mappings |
| Driver-allocated MDLs | Standalone descriptors over modeled nonpaged pool, with shared original buffer addresses | IRP association, MDL chains, probing/locking, physical pages and user mappings |
| READ/WRITE | Serial buffered/direct I/O with work-item or DPC completion | Only the API subset below; no concurrent scenario-submitted IRPs or WDM request cancellation; `METHOD_NEITHER` and implicit file position |
| `METHOD_NEITHER` | Rejected | User address-space context, access probing and guest exception handling |
| KMDF 1.33 non-PnP driver | Binding, objects/contexts, named control devices, sequential default queues and buffered/direct requests with executed callbacks | No PnP devices, general queue scheduling, class extensions or UMDF |
| PnP bus/function/filter driver | Explicit resource-free PDOs, guest AddDevice and eight common PnP lifecycle minors | Other PnP operations, general power policy, hardware/resources and KMDF PnP |
| Storage, network, display, filesystem and minifilter drivers | Unsupported subsystem contracts | Port/class/miniport frameworks, NDIS/WFP, graphics or filesystem services |
| Work items, timers, DPCs, events and waits | The current execution IRQL is `PASSIVE_LEVEL` for dispatch and workers, and `DISPATCH_LEVEL` for DPCs | Only the API subset below; no concurrent scenario-submitted IRPs or WDM request cancellation |
| Driver using process/thread callbacks, handles, registry/file operations or kernel-module discovery | Configured registry supported; other behavior limited to the listed APIs | Object manager, system state and callback/event producers |
| Hardware, DMA, PCI, interrupt or virtualization driver | Unsupported environment | Device models, physical memory, buses, interrupts and privileged CPU state |
| x86 or ARM64 Windows driver | Rejected | Architecture-specific loading, ABI and execution model |
| x64 CFG | Validated target tables and check/dispatch calls; inactive instrumentation retains guest fallbacks | XFG, export suppression, unsupported load configuration and TLS remain rejected |

An unused unsupported import can remain bound. Reaching an unsupported operation
stops with a diagnostic and the observations collected so far. A successful
DriverEntry alone does not prove that later dispatch, hardware or framework
paths are supported. The API table below is the authoritative supported subset.

## Execution contract

The profile models an x64 WDM lifecycle on CPU0 with deterministic cooperative scheduling. Execution begins at the PE entry point, retaining a compiler's entry wrapper
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

Unknown imports bind to lazy traps. An unused import does not prevent execution;
executing its thunk or reading an unmodeled exported data value stops with
`unsupported_api`. Unsupported CPU environment effects also stop explicitly.
NeverD does not replace unimplemented calls with success values. Malformed
images or unsupported loading requirements fail before execution.

Queued `DelayedWorkQueue` workers execute at `PASSIVE_LEVEL`; guest DPC
callbacks execute at `DISPATCH_LEVEL` with the documented four arguments.
Scheduling is deterministic and cooperative on CPU0, at returned-call and
blocking-wait boundaries. Relative, absolute and periodic timers use virtual
time, advancing to the next timer, wait or cancellation deadline when no frame
can run.
Notification and synchronization events/timers retain their distinct
signal-consumption behavior. Each callback has a separate guest stack;
multiple blocked frames retain their locals and complete CPU contexts while
guest memory remains shared. Win64 callback entry places the first four
arguments in registers and additional arguments on the stack. Requests remain
serial: a dispatch that marks an IRP pending must return `STATUS_PENDING`, and
it must complete before the next request starts. No available producer for a
pending request or infinite wait causes a stalled `model_error`. Shared
instruction, memory, observation and wall-clock budgets still apply.

This is a bounded scheduling model, not full Windows asynchronous support.
Alertable or user-mode waits, system threads, APCs, WDM request cancellation,
spinlocks, concurrent scenario-submitted IRPs, general IRQL transitions, `METHOD_NEITHER`,
UMDF, KMDF PnP devices and general queue scheduling, full PnP/power, hardware, DMA and interrupts remain unsupported.
Initialization-only calls execute explicitly queued callbacks without
inventing requests or unload.

Images use their preferred base unless a scenario selects a valid relocated
address, and must be PE32+ x64 executables with the native subsystem. Imports
may come from `ntoskrnl.exe`, `ntkrnlmp.exe` or `WDFLDR.SYS`.
The execution loader supports validated x64 `DIR64` base relocations and a
limited security-cookie load configuration, initialized before the entry wrapper
with a deterministic guest cookie. Other unmodeled load-configuration
fields, TLS, delayed/bound imports, ordinal imports, and managed images are
rejected. Images must also pass strict range and alignment checks.

Active Control Flow Guard (CFG) validates PE flags, pointer slots and sorted executable target entries. Check and dispatch helpers admit only declared image entries or registered API thunks, preserve the Win64 call state and reject undeclared targets. Instrumentation without active CFG preserves the original guest fallback pointers. Active XFG, export suppression and other unmodeled guard policies remain rejected; executable memory alone does not make an address a valid target.

WDM stacks can contain several device objects owned by the same guest driver. `IoAttachDeviceToDeviceStack` attaches an isolated source above the target's current top and returns that previous top; it sets `StackSize` and `AlignmentRequirement` without changing the driver's `NextDevice` list or copying buffer flags. `IoDetachDevice` takes the saved lower device and requires `PASSIVE_LEVEL`; attachment permits IRQL up to `DISPATCH_LEVEL`. Opening a named lower device dispatches to the current top, while `FILE_OBJECT.DeviceObject` and the report retain the named device identity. READ/WRITE buffering uses the selected top's flags. Captured request routes retain every device through dispatch return, including after detach/delete; internal holds do not increase the open-handle `ReferenceCount`.

`IofCallDriver` and the `IoCallDriver` helper invoke the exact target on that retained route. Real inline `IoCopyCurrentIrpStackLocationToNext`, `IoSkipCurrentIrpStackLocation` and `IoSetCompletionRoutine` operate on the original guest IRP; cursor, count and control flags are validated. Lower dispatch returns its actual status independently of `IoStatus` and completion-routine return values. Completion advances the cursor, selects callbacks using success/error/cancel flags and carries pending state upward; a completion routine owns its pending propagation, including after an earlier `STATUS_PENDING` dispatch return. `STATUS_MORE_PROCESSING_REQUIRED` stops unwinding without retiring the IRP, MDLs or buffers; later completion resumes it. Nested completion is supported with the required outer stop result, and terminal unwinding retires storage once. Owner-tagged continuations preserve nested WDM/WDF caller frames and inherited IRQL. Each consumed lower stack location is cleared before the upper completion callback runs.

Driver-allocated IRPs, additional PnP minors and hardware/resource models remain unsupported. WDF attachment/forwarding, attaching to stacks with live files or callbacks, detaching an intermediate layer, changing the forwarded major function and targeting devices outside the captured route fail explicitly. Optional genuine-WDK `driver_wdm_stack.c` images use `NEVERD_WDM_STACK_FIXTURE` and `NEVERD_WDM_STACK_CFG_FIXTURE`; native and C API/CLI coverage includes relocation, with missing artifacts explicitly skipped. Execution evidence remains Linux-only.

A scenario can explicitly configure `pnp_devices` (at most 64). Every entry requires `id`, `bus: "resource_free"`, `initial_device_power: "D0"` and `initial_system_power: "working"`; omitted facts are not guessed. IDs are case-sensitive ASCII, 1–64 bytes, beginning with an alphanumeric character and otherwise containing only alphanumerics, `_`, `-` or `.`. Ordinary requests may select a configured `device_id` instead of `device`; the two selectors are exclusive. A `kind: "pnp"` request requires `device_id`, `minor` and `bus_completion`. Supported minors are `start`, `query_remove`, `cancel_remove`, `remove`, `query_stop`, `stop`, `cancel_stop`, and `surprise_removal`. `bus_completion.status` is required as a 32-bit integer or hexadecimal string; optional `delay_100ns` is a nonnegative integer at most INT64_MAX, measured from actual provider receipt. `STATUS_PENDING` is not a final bus status; stop/cancel-stop/surprise-removal/cancel-remove/remove require exactly `STATUS_SUCCESS` (0). PnP requests reject file, transfer and cancellation fields, even when zero. The C++ API applies the same preflight.

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

After successful DriverEntry, `AddDevice` runs once per configured PDO with a separate provider-owned `DRIVER_OBJECT`; the guest cannot delete or impersonate provider objects. PnP IRPs are `KernelMode`, file-free and resource-free, initially `STATUS_NOT_SUPPORTED`. The bus response is consumed only if forwarding reaches that PDO; delayed completion uses the shared virtual clock and existing completion continuations. Final upper completion commits or rolls back lifecycle state independently of the bus status. Normal removal from Started requires a successful query, closed files, drained prior requests/callbacks and guest detach/delete. Clean AddDevice failure retires only the provider; leaked new guest devices cause `model_error`, including detached devices. All providers must be absent before unload. This does not implement other PnP minors, hardware/resources or KMDF PnP. PnP success requires actual provider completion; an early START/QUERY_STOP/QUERY_REMOVE failure may retain null bus observations. Device/file lifecycle identity persists after detach.

Reports retain the initial inventory in `configuration.pnp_devices`. Observed `pnp_devices` entries contain `id`, `pdo`, nullable `add_device_status`, current `attached`, `pnp_state` and `provider_present`; removal leaves `attached` false. AddDevice phases are `add_device:<ID>`, and failures contribute to `scenario_success` without replacing DriverEntry `nt_status`. Each request adds nullable `device_id` and `pnp`; PnP requests have `file: null`. The `pnp` object records `minor`, `state_before`, `state_after`, nullable `bus_status`, `bus_received_at_100ns` and `bus_completed_at_100ns`. Configured status becomes an observation only at actual bus completion; receipt time is independent. Existing request field types are unchanged.

Ordinary CREATE/READ/WRITE/IOCTL/CLEANUP/CLOSE requests reach real guest dispatch while the device exists outside Removing/Removed. The model does not synthesize a failure from Stopped, StopPending, RemovePending or power state: a driver can complete software I/O, reject a request or hold it according to its own code. The public runner remains serial; a held IRP with no available producer cannot be released by a later scenario start or cleanup request, and stops as stalled `model_error`. The closed-file and drained-request/callback requirements before Remove are profile restrictions. `query_stop` with final `STATUS_RESOURCE_REQUIREMENTS_CHANGED` (0x119) is rejected in both scenario preflight and final guest completion because it requests unmodeled resource requery; see [Microsoft’s QUERY_STOP contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/irp-mn-query-stop-device). Stop/restart and surprise-removal support do not add resources or KMDF PnP.

Resource-free PnP uses the original genuine-WDK `driver_wdm_pnp.c`, optional `NEVERD_WDM_PNP_FIXTURE` / `NEVERD_WDM_PNP_CFG_FIXTURE`, and native plus C API/CLI tests. Missing artifacts skip explicitly; execution evidence remains Linux-only.

Resource-free WDM power execution uses `kind: "power"` with a configured `device_id`. Every packet explicitly requires `minor` (`query` or `set`), `power_type` (`device` or `system`), `power_state` (`D0`/`D3` or `working`/`sleeping3`), `power_action` (`none` or `sleep`), `system_context` (a 32-bit integer or hexadecimal string) and `bus_completion`. System Query to Working is unsupported. Power packets reject file, transfer and cancellation fields. The complete `system_context` value is retained as an opaque packet fact; it does not select a parent or imply hibernate/fast-startup support. Routes require `DO_POWER_PAGABLE` without `DO_POWER_INRUSH`, and this profile executes power dispatch and `PoRequestPowerIrp` at `PASSIVE_LEVEL`. `PoCallDriver` forwards the same owned power IRP; `PoStartNextPowerIrp` follows the Vista+ contract without an extra serialization handshake. General power policy, WAIT_WAKE, other states/actions, shutdown/hibernate, inrush, nonpageable routes, hardware and KMDF PnP remain unsupported.

Each `pnp_devices` entry may include `initial_reported_device_power: "D0"` or `"D3"`, independent of the required initial lifecycle facts D0/working. The provider PDO and each newly associated guest DEVICE_OBJECT receive separate notification state; `PoSetPowerState` returns and updates only the calling device's previous value. Without this explicit seed, a reached `PoSetPowerState` fails instead of assuming D0. Optional `requested_device_power` contains device-type packet templates, with the same six required power facts. At most 64 templates are accepted across all PDOs. Only a real `PoRequestPowerIrp` with matching PDO, minor and target consumes that PDO's FIFO head; missing or mismatched entries fail, unused entries create no requests. No parent is guessed from the callback context. Each child owns an independent IRP and result row with `origin: "PoRequestPowerIrp"` and zero-based `response_index`; scenario rows use `origin: "scenario"` and null response index. Synchronous child completion can run its five-argument void callback before `PoRequestPowerIrp` returns `STATUS_PENDING`; callbacks may wait, and system S0 can complete before the independent D0 child. The callback's IO_STATUS_BLOCK snapshot remains valid through its return.

Request reports add nullable `power` alongside `pnp`; power rows have `file: null`. `power` records the explicit packet facts, `device_state_before`/`device_state_after`, `system_state_before`/`system_state_after`, nullable `requested_device_object` and actual `bus_status`/`bus_received_at_100ns`/`bus_completed_at_100ns`. Final PnP-device snapshots add `device_power` and `system_power`; live device snapshots add nullable `reported_device_power`. `scenario_success` counts scenario-origin rows against configured requests and requires every actual child and scenario row to complete successfully; unused FIFO templates do not fail the scenario. Genuine `driver_wdm_power.c` images use `NEVERD_WDM_POWER_FIXTURE` / `NEVERD_WDM_POWER_CFG_FIXTURE`, with normal/active-CFG native and C API/CLI coverage. Missing artifacts skip explicitly; execution evidence is Linux-only.

The [complete power scenario](examples/driver-power-scenario.json) runs the genuine power fixture through start, system query/sleep/wake and removal, with three explicit child responses; pass it with `--scenario`.

KMDF 1.33 support uses the exact 1.33.0 ABI: 458 function slots have stable guest identities, while the 38 APIs below have execution semantics. `WdfVersionBind` and `WdfVersionUnbind` manage guest bindings around the genuine WDK `FxDriverEntry` wrapper. `WdfGetDriver` reads the public driver globals. Non-PnP drivers, generic objects, control devices, queues and incoming requests share typed contexts, reference counts and executed cleanup/destroy/unload callbacks. All modeled framework calls and callbacks currently require `PASSIVE_LEVEL`; adding references after completed cleanup remains outside this profile. Unmodeled function slots, `WdfLdrQueryInterface`, class extensions and UMDF stop explicitly.

Control devices require a copied printable-ASCII name and the exact SDDL `D:P(A;;GA;;;WD)`. This grants universal access and avoids inventing a caller token; other security descriptors, unnamed devices and automatic names are unsupported. Device initialization owns one WDM device. Requests can select it through the existing session namespace using `\DosDevices\Name` or `\??\Name` symbolic-link aliases; reports retain the canonical device name. Successful creation consumes and clears the initializer; failed creation rolls back partial device ownership. `WdfControlFinishInitializing` gates I/O delivery. Deletion removes the device and its links only when modeled files, work items and requests permit it; cancellation or draining during deletion is unsupported.

The 96-byte `WDF_IO_QUEUE_CONFIG` supports a sequential default queue with explicit passive execution and no framework synchronization. Control-device queues are not power managed. Specific READ/WRITE/IOCTL callbacks take precedence over the default callback. Accepted queued requests return `STATUS_PENDING` even when completed synchronously; a void callback's return register does not complete its request. Deferred completion uses the existing scheduler. Without a handler the request completes with `STATUS_INVALID_DEVICE_REQUEST`; zero-length READ/WRITE completes without delivery unless enabled. The default file package completes CREATE/CLEANUP/CLOSE successfully with Information=0. Parallel/manual queues, file callbacks, PnP devices and full PnP/power remain unsupported.

Request parameters use the 40-byte `WDF_REQUEST_PARAMETERS` layout. Input/output accessors return the logical lengths, preserving buffered aliases and existing direct-I/O MDL mappings; direct IOCTL input remains buffered. Wrong directions and insufficient buffers return documented statuses. Completion runs request cleanup and child destruction before retiring the IRP/buffers, then destroys the request when references permit. New buffer and parameter accessors are rejected once completion begins; already obtained buffer pointers remain usable during cleanup. An external object reference preserves context, not completed IRP access. User-mode `METHOD_NEITHER` still requires unimplemented caller-context/probe/lock support.

Cancellation is modeled for requests on the control-device queues described above. `WdfRequestMarkCancelableEx` returns `STATUS_CANCELLED` without invoking a callback if cancellation already occurred. Successful `WdfRequestUnmarkCancelable` removes the callback; a later cancellation records the canceled state without delivering that callback. `WdfRequestIsCanceled` observes that state on an unmarked live request. After successful marking, completion requires successful unmarking or delivery of the cancellation callback: a merely queued callback does not authorize completion. Once delivery begins, the callback and a worker may coordinate completion, including when the callback waits. A separate internal reference retains the request through cancellation-callback return; completion still retires the IRP first, and the final request-destroy continuation may itself wait. DPCs take priority, followed by cancellation callbacks in FIFO order, then ordinary workers; queued cancellation callbacks also precede resuming ready passive waiters.

For an already canceled request, legacy void `WdfRequestMarkCancelable` executes a synchronous guest cancellation callback before returning. This child continuation can wait, complete the request through nested cleanup, and run final destruction before the original API resumes. If cancellation occurs after registration, delivery uses the scheduled cancellation path above. This reproduces the public source behavior at `PASSIVE_LEVEL` with `WdfSynchronizationScopeNone`; [Microsoft](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfrequest/nf-wdfrequest-wdfrequestmarkcancelable) directs drivers without automatic synchronization to use Ex. Legacy execution here is compatibility behavior, not a recommendation to use it in that configuration.

`WdfRequestGetInformation` and `WdfRequestSetInformation` share the original 64-bit `IRP.IoStatus.Information`, including direct guest writes. Set assigns the value; transfer-length validation occurs at completion. `WdfRequestCompleteWithInformation` writes the same field before cleanup; changes through a previously saved IRP during cleanup determine the final Information even though GetInformation already returns zero in that phase. `WdfRequestGetIoQueue` returns the originating queue. With the default file configuration, `WdfRequestGetFileObject` returns NULL: no WDF file object is invented from the WDM FILE_OBJECT. `WdfRequestWdmGetIrp` returns the same IRP; guest `IoCompleteRequest`/`IofCompleteRequest` cannot bypass WDF completion. While a request handle remains valid during or after completion, GetInformation/GetIoQueue return zero, and MDL retrieval first clears a valid output slot to NULL then returns `STATUS_INTERNAL_ERROR`. SetInformation/GetFileObject/WdmGetIrp remain rejected then. Existing buffer and parameter accessor restrictions are unchanged.

`WdfRequestRetrieveInputWdmMdl` and `WdfRequestRetrieveOutputWdmMdl` lazily describe the existing SystemBuffer for buffered WRITE input, READ output and IOCTL input/output. Each requested direction must be valid and nonempty before using the request’s single cached descriptor; the first successful retrieval fixes its ByteCount even when the other direction has a different logical length. `MmGetSystemAddressForMdlSafe` returns the original VA for this descriptor; additional mapping, unmapping and driver freeing are rejected. Direct READ output, WRITE input and IOCTL output instead return the existing `IRP.MdlAddress` without mapping it merely by retrieval; direct IOCTL input uses the SystemBuffer cache. Descriptors, IRP and buffers retire at completion. Cancellation-internal or external references retain only WDF context, not completed I/O storage. `METHOD_NEITHER` and physical PFN access remain unsupported.

Modeled KMDF APIs: `WdfDriverCreate`, `WdfDriverGetRegistryPath`, `WdfDriverWdmGetDriverObject`, `WdfWdmDriverGetWdfDriverHandle`, `WdfObjectGetTypedContextWorker`, `WdfObjectAllocateContext`, `WdfObjectContextGetObject`, `WdfObjectReferenceActual`, `WdfObjectDereferenceActual`, `WdfObjectCreate`, `WdfObjectDelete`, `WdfControlDeviceInitAllocate`, `WdfDeviceInitFree`, `WdfDeviceInitAssignName`, `WdfDeviceInitSetIoType`, `WdfDeviceCreate`, `WdfDeviceCreateSymbolicLink`, `WdfControlFinishInitializing`, `WdfDeviceWdmGetDeviceObject`, `WdfIoQueueCreate`, `WdfDeviceGetDefaultQueue`, `WdfIoQueueGetDevice`, `WdfRequestComplete`, `WdfRequestCompleteWithInformation`, `WdfRequestGetParameters`, `WdfRequestRetrieveInputBuffer`, `WdfRequestRetrieveOutputBuffer`, `WdfRequestRetrieveInputWdmMdl`, `WdfRequestRetrieveOutputWdmMdl`, `WdfRequestSetInformation`, `WdfRequestGetInformation`, `WdfRequestGetFileObject`, `WdfRequestGetIoQueue`, `WdfRequestWdmGetIrp`, `WdfRequestMarkCancelable`, `WdfRequestMarkCancelableEx`, `WdfRequestUnmarkCancelable`, `WdfRequestIsCanceled`.

Optional genuine-WDK validation compiles `driver_kmdf_lifecycle.c` and `driver_kmdf_control.c` separately with the real KMDF entry library. `NEVERD_KMDF_FIXTURE` / `NEVERD_KMDF_CFG_FIXTURE` select lifecycle images; `NEVERD_KMDF_CONTROL_FIXTURE` / `NEVERD_KMDF_CONTROL_CFG_FIXTURE` select normal/active-CFG control-device images. Missing external artifacts produce explicit skips. See [testing](testing.md) for native and C API/CLI coverage. Current execution evidence is limited to Linux hosts.

The initial API model deliberately has a finite contract:

| APIs | Modeled behavior and restrictions |
|------|-----------------------------------|
| `RtlInitUnicodeString` | Builds a guest `UNICODE_STRING` for a bounded NUL-terminated source |
| `RtlCopyUnicodeString`, `RtlCompareUnicodeString`, `RtlEqualUnicodeString` | Counted UTF-16 copy and case-sensitive comparison; case-insensitive comparison requires a Windows case table and stops |
| `ExAllocatePool2` | Paged/nonpaged NX allocations, zeroed by default; uninitialized and cache-aligned flags modeled; invalid required flags return NULL, quota/executable pools and raised allocation exceptions stop |
| `MmGetSystemRoutineAddress` | Resolves a counted guest name through the shared export inventory |
| `MmMapLockedPagesSpecifyCache`, `MmGetSystemAddressForMdlSafe`, `MmUnmapLockedPages` | Request-owned MDLs with cached KernelMode mappings and permissions; nonpaged pool MDLs reuse the original pool mapping through the safe helper |
| `IoAllocateMdl`, `MmBuildMdlForNonPagedPool`, `IoFreeMdl` | Standalone descriptors, complete ranges in one live nonpaged pool allocation, independent descriptor/buffer lifetimes; no IRP association, MDL chains or quota |
| `ZwOpenKey`, `ZwCreateKey`, `ZwQueryValueKey`, `ZwSetValueKey`, `ZwDeleteValueKey`, `ZwDeleteKey`, `ZwClose` | Explicit session registry, per-handle rights and lifetime, query buffer sizing and mutations; no host registry access |
| `ExAllocatePoolWithTag`, `ExFreePoolWithTag`, `ExFreePool` | Data allocations for pool types `0`, `1`, and `512`; positive size/tag, matching tagged frees, no address reuse |
| `IoCreateDevice`, `IoDeleteDevice` | Device type `0x22`, characteristics `0` or `0x100`, bounded extensions, ASCII `\Device\Name` names |
| `IoAttachDeviceToDeviceStack`, `IoDetachDevice` | Same-driver attachment; attach returns the previous top, detach consumes the saved lower device; explicit topology/lifetime limits above |
| `IofCallDriver`, `IoCallDriver` | Exact-target dispatch on the retained route; validated guest stack cursor and distinct lower NTSTATUS |
| `PoCallDriver`, `PoStartNextPowerIrp` | Forward an owned power IRP; Vista+ start-next validation without a serialization handshake |
| `PoSetPowerState`, `PoRequestPowerIrp` | Independent per-device notification state and real device-power children from explicit per-PDO response FIFOs; bounded pageable profile above |
| `IoCreateSymbolicLink`, `IoDeleteSymbolicLink` | ASCII `\DosDevices\Name` or `\??\Name` within one session namespace, targeting `\Device\Name` |
| `DbgPrint`, `DbgPrintEx` | Checked Win64 variadic formatting, at most 512 output bytes; all debugger filters enabled |
| `IoGetCurrentIrpStackLocation` | Returns the stack location of the active modeled IRP; normal compiled WDM macros read the same guest field |
| `KeGetCurrentIrql` | The current execution IRQL is `PASSIVE_LEVEL` for dispatch and workers, and `DISPATCH_LEVEL` for DPCs |
| `IoAllocateWorkItem`, `IoQueueWorkItem`, `IoFreeWorkItem` | Device-owned opaque work items; `DelayedWorkQueue` only, callbacks receive the device and context at `PASSIVE_LEVEL`; queued items cannot be freed |
| `KeInitializeDpc`, `KeInsertQueueDpc`, `KeRemoveQueueDpc`, `KeSetImportanceDpc`, `KeSetTargetProcessorDpc` | Opaque DPC storage, four guest callback arguments, `DISPATCH_LEVEL`, duplicate/remove semantics and importance; target CPU0 only |
| `KeInitializeTimer`, `KeInitializeTimerEx`, `KeSetTimer`, `KeSetTimerEx`, `KeCancelTimer`, `KeReadStateTimer` | Notification/synchronization timers; relative/absolute 100 ns deadlines, periodic milliseconds, rearm/cancel and signal queries in virtual time |
| `KeInitializeEvent`, `KeSetEvent`, `KeResetEvent`, `KeClearEvent`, `KeReadStateEvent` | Notification/synchronization events with distinct signal consumption; `KeSetEvent` accepts Increment=0 and Wait=FALSE only |
| `KeWaitForSingleObject` | One initialized event or timer; nonalertable `KernelMode`, reason `Executive`; zero polling, finite relative/absolute or infinite waits; nonzero/infinite waits require IRQL <= APC_LEVEL |
| `KeDelayExecutionThread` | Nonalertable `KernelMode` relative/absolute delay at IRQL <= APC_LEVEL; resumes the saved guest frame after virtual time advances |
| `IoMarkIrpPending` | Marks the live active IRP; the equivalent WDM macro's stack-control write is also modeled; dispatch must return `STATUS_PENDING` |
| `IofCompleteRequest`, `IoCompleteRequest` | `IO_NO_INCREMENT`; executes completion unwinding, supports stopped/resumed completion and retires IRP/MDL/buffer storage only at its terminal boundary |
| `memcpy`, `memmove`, `memset`, `memcmp`, `RtlCopyMemory`, `RtlMoveMemory`, `RtlFillMemory`, `RtlZeroMemory`, `RtlCompareMemory` | Bounded guest buffer operations, at most 1 MiB per call; non-overlapping copy APIs reject overlaps |

API IRQL ceilings come from `KernelAPIIRQL.def`, with argument-dependent
checks in the owning model. DPCs cannot call registry APIs or allocate, free
or access paged pool; Unicode `DbgPrint` conversions require `PASSIVE_LEVEL`,
while supported ANSI output and nonpaged operations remain usable at
`DISPATCH_LEVEL`. Callback stacks have bounded ranges; an escaping stack
pointer cannot enter another blocked worker’s stack. Armed timers in a device
extension prevent premature device retirement. These checks do not expose
general IRQL transitions.

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

Only READ/WRITE/IOCTL requests accept optional `cancel_after_100ns`, a JSON integer from 0 through `INT64_MAX` (9223372036854775807). It schedules cancellation relative to request submission in virtual 100 ns units, not wall-clock time. Zero applies after framework routing and before the guest I/O callback; if routing already completed the request, completion wins. For positive delays, time advances to timer, wait or cancellation deadlines only when no callback/frame is ready. Configuring cancellation on a WDM request stops with `model_error`; general queue and PnP cancellation remain unsupported. Each request report includes `cancel_requested_at_100ns`, either the actual absolute virtual cancellation time or null if cancellation never occurred, including when completion won first. A cancellation request alone does not complete an IRP or prescribe its final status.

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

`IoAllocateMdl` allocates standalone metadata for a nonempty, nonoverflowing
buffer of at most 1 MiB; it does not probe or lock that buffer. `Irp` must be
NULL and `SecondaryBuffer` and `ChargeQuota` must be FALSE. Arena exhaustion
returns NULL. `MmBuildMdlForNonPagedPool` requires the entire described range
to belong to one live nonpaged pool allocation. The safe helper and ordinary
WDM macro reuse its original address, preserving aliases and existing
permissions even when new no-write/no-execute flags are supplied. Additional
system mappings and unmapping are rejected. `IoFreeMdl` expires only the
descriptor; the pool buffer has its own lifetime. Both release orders are
supported when freed storage is not used afterward. All modeled MDL fields are
read-only; process/PFN access, descriptor chains and manual field changes
remain unsupported. Unload must release every driver-owned descriptor.

When an IOCTL has a nonzero `output_size`, `Information` must not exceed that
size, even when the input buffer is larger. An IOCTL without an output buffer
may return a driver-defined result in this field, and no output bytes are
copied. `information_hex` preserves its exact raw64-bit value.

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
The profile is `wdm-x64-scheduled-v10`. `nt_status` remains the DriverEntry
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
Request fields are `kind`, `device`, `device_id`, `pnp`, `file`, `byte_offset`, `code`, `irp`, `completed`,
`cancel_requested_at_100ns`, `dispatch_status`, `io_status`, `information`, `information_hex`, and `output_hex`.
`information_hex` preserves the raw64-bit `IoStatus.Information` as an exact
hexadecimal string; the existing numeric `information` field remains available.

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
