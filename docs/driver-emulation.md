**Languages**: [English](driver-emulation.md) | [简体中文](zh-CN/driver-emulation.md) | [繁體中文](zh-TW/driver-emulation.md) | [日本語](ja/driver-emulation.md) | [한국어](ko/driver-emulation.md) | [Français](fr/driver-emulation.md) | [Deutsch](de/driver-emulation.md) | [Español](es/driver-emulation.md) | [Italiano](it/driver-emulation.md) | [Русский](ru/driver-emulation.md) | [العربية](ar/driver-emulation.md)

[← Documentation index](README.md)

# Windows driver emulation

NeverD's optional driver emulator executes the PE entry point of a supported
x64 WDM driver and optionally exercises an explicit request
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
| KMDF 1.33 PnP FDO | `EvtDriverDeviceAdd`, FDO/PDO identity, default or explicit power-managed I/O queues, common PnP minors, hardware/D0, self-managed I/O and request stop/resume callbacks, and framework cleanup | Unmodeled resource types, idle/wake policy and general KMDF PnP |
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
may come from `ntoskrnl.exe`, `ntkrnlmp.exe` or `WDFLDR.SYS`.
The execution loader supports validated x64 `DIR64` base relocations and a
limited security-cookie load configuration, initialized before the entry wrapper
with a deterministic guest cookie. Other unmodeled load-configuration
fields, TLS, delayed/bound imports, ordinal imports, and managed images are
rejected. Images must also pass strict range and alignment checks.

Active Control Flow Guard (CFG) validates PE flags, pointer slots and sorted executable target entries. Check and dispatch helpers admit only declared image entries or registered API thunks, preserve the Win64 call state and reject undeclared targets. Instrumentation without active CFG preserves the original guest fallback pointers. Active XFG, export suppression and other unmodeled guard policies remain rejected; executable memory alone does not make an address a valid target.

WDM stacks can contain several device objects owned by the same guest driver. `IoAttachDeviceToDeviceStack` attaches an isolated source above the target's current top and returns that previous top; it sets `StackSize` and `AlignmentRequirement` without changing the driver's `NextDevice` list or copying buffer flags. `IoDetachDevice` takes the saved lower device and requires `PASSIVE_LEVEL`; attachment permits IRQL up to `DISPATCH_LEVEL`. Opening a named lower device dispatches to the current top, while `FILE_OBJECT.DeviceObject` and the report retain the named device identity. READ/WRITE buffering uses the selected top's flags. Captured request routes retain every device through dispatch return, including after detach/delete; internal holds do not increase the open-handle `ReferenceCount`.

`IofCallDriver` and the `IoCallDriver` helper invoke the exact target on that retained route. Real inline `IoCopyCurrentIrpStackLocationToNext`, `IoSkipCurrentIrpStackLocation` and `IoSetCompletionRoutine` operate on the original guest IRP; cursor, count and control flags are validated. Lower dispatch returns its actual status independently of `IoStatus` and completion-routine return values. Completion advances the cursor, selects callbacks using success/error/cancel flags and carries pending state upward; a completion routine owns its pending propagation, including after an earlier `STATUS_PENDING` dispatch return. `STATUS_MORE_PROCESSING_REQUIRED` stops unwinding without retiring the IRP, MDLs or buffers; later completion resumes it. Nested completion is supported with the required outer stop result, and terminal unwinding retires storage once. Owner-tagged continuations preserve nested WDM/WDF caller frames and inherited IRQL. Each consumed lower stack location is cleared before the upper completion callback runs.

Driver-allocated IRPs, additional PnP minors and other hardware/resource models remain unsupported. Other WDF target forwarding, attaching to stacks with live files or callbacks, detaching an intermediate layer, changing the forwarded major function and targeting devices outside the captured route fail explicitly. Optional genuine-WDK `driver_wdm_stack.c` images use `NEVERD_WDM_STACK_FIXTURE` and `NEVERD_WDM_STACK_CFG_FIXTURE`; native and C API/CLI coverage includes relocation, with missing artifacts explicitly skipped. Execution evidence remains Linux-only.

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

Ordinary CREATE/READ/WRITE/IOCTL/CLEANUP/CLOSE requests reach real guest dispatch while the device exists outside Removing/Removed. The model does not synthesize a failure from Stopped, StopPending, RemovePending or power state: a driver can complete software I/O, reject a request or hold it according to its own code. Pending WDM transfers can overlap only through explicit batching across files or on one asynchronous file; a held IRP with no available producer still cannot be released by a later scenario start or cleanup request, and stops as stalled `model_error`. The closed-file and drained-request requirements before Remove are profile restrictions. `query_stop` with final `STATUS_RESOURCE_REQUIREMENTS_CHANGED` (0x119) is rejected in both scenario preflight and final guest completion because it requests unmodeled resource requery; see [Microsoft’s QUERY_STOP contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/irp-mn-query-stop-device). Stop/restart and surprise-removal do not implement resource rebalance or KMDF power policy.

Resource-free PnP uses the original genuine-WDK `driver_wdm_pnp.c`, optional `NEVERD_WDM_PNP_FIXTURE` / `NEVERD_WDM_PNP_CFG_FIXTURE`, and native plus C API/CLI tests. Missing artifacts skip explicitly; execution evidence remains Linux-only.

WDM remove locks execute through the actual `IoInitializeRemoveLockEx`, `IoAcquireRemoveLockEx`, `IoReleaseRemoveLockEx` and `IoReleaseRemoveLockAndWaitEx` exports; the unsuffixed WDK names are macros. Each lock belongs to the exact guest DEVICE_OBJECT whose extension contains its full aligned storage, independently of PDO lifecycle or Tag shape. Initialization works before attachment. Retail 32-byte and DBG 120-byte locks are accepted only with the matching separate size argument, and their entire registered storage is opaque. NULL and repeated Tags are counted independently per lock; Tags are never dereferenced, so releasing after IRP completion remains valid. Initialization and AndWait require `PASSIVE_LEVEL`; acquire/release permit `DISPATCH_LEVEL`.

AndWait closes admission, releases one matching acquisition and suspends the actual guest frame until all remaining acquisitions drain. Later acquire returns `STATUS_DELETE_PENDING` without adding an obligation. The final release latches readiness before the releasing callback returns; a worker can release and then wait for the resumed REMOVE path to signal it. There is no synthetic callback, timeout or successful result without a producer. This profile requires an associated active REMOVE route containing the owner and actual provider receipt (`bus_received_at_100ns` may be zero); lower completion is not required. A lower driver that queues REMOVE before provider receipt remains outside this bounded check, which does not claim full OutsideRemoveDevice/Driver Verifier enforcement. The public runner still requires closed files and drained earlier requests before REMOVE, but permits outstanding callbacks that release the locks. The retained REMOVE route survives drain, guest detach/delete and any pending lower completion; remaining callback frames must return before final route retirement.

Unknown/mismatched storage, unmatched releases, duplicate drain, reinitialization and deleting an extension with acquisitions or an unconsumed drain wait fail before mutation. An initialized unused lock can be deleted during clean AddDevice failure. Lock acquisitions do not replace real device/work-item references; lock storage unregisters only at physical extension retirement. Valid debug metadata does not enable verifier timeout/high-water behavior. The original genuine-WDK `driver_wdm_remove_lock.c` uses optional `NEVERD_WDM_REMOVE_LOCK_FIXTURE` / `NEVERD_WDM_REMOVE_LOCK_CFG_FIXTURE` / `NEVERD_WDM_REMOVE_LOCK_DBG_FIXTURE` / `NEVERD_WDM_REMOVE_LOCK_DBG_CFG_FIXTURE`; missing artifacts skip explicitly. Native and C API/CLI evidence remains Linux-only and does not establish a complete remove manager or general concurrent I/O draining.

The synthetic `bus: "register_bank"` adds explicit fixed memory resources to a PDO. When present, its `resources` array contains `id`, `raw_start`, `translated_start`, `length` and `registers`; every register requires `offset`, `width`, `access` (`read_only` or `read_write`) and initial `value`. `DriverResources.h` / `DriverResources.def` define the shared C++ and JSON contract. Resource IDs follow the bounded ASCII identifier rules and are unique within each PDO. Limits are 8 resources per PDO / 32 total, 256 registers per resource / 4096 total and 1–1048576 bytes per resource. Only naturally aligned exact 1/2/4-byte register accesses are supported; values must fit that width. Both physical intervals must be overflow-free; raw ranges cannot overlap within a PDO, and translated ranges cannot overlap globally. Empty `registers` explicitly leaves the whole bank inaccessible. Addresses and initial values are declarations, never host hardware or default zero-filled memory. `resource_free` retains an omitted resource inventory and null START pointers.

START receives separate read-only raw and translated `CM_RESOURCE_LIST` allocations, with corresponding ordered Memory descriptors: one full descriptor, Internal interface, bus 0, version/revision 1, DeviceExclusive sharing and READ_WRITE range flags. Per-register RO remains independent. A successful lower START makes the assignment available before upper completion callbacks; each START from NotStarted/Stopped creates a new resource epoch with the same fixed assignment. Register values initialize once per PDO and survive unmap, STOP and restart. Failed START and successful STOP/REMOVE require the driver to release mappings before terminal IRP completion; upper completion callbacks can unmap first, and mappings are never silently cleaned up. Surprise removal immediately prevents new maps and register access, while existing mappings can still be unmapped. Actual successful provider device-SET completion changes hardware accessibility: D3 blocks accesses, D0 permits them only with an available assignment; D3 still permits mapping without register access, and power changes do not discard mappings or reset values.

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
an event. Due events run at supported callback boundaries; time advances only
while idle, without instruction-level preemption. Provider hardware publication
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
APIs or outlive their storage. DIRQL waits, floating-state save and instruction-level preemption remain
unsupported. Failed START and successful
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

`DriverDMA.h` / `DriverDMA.def` add an optional `dma` object to a `register_bank` PDO, alongside its memory/interrupt assignments; DMA alone does not replace either resource list. All seven fields are explicit: `address_bits` (32 or 64), `maximum_length` (1–1048576 bytes), `map_registers` (1–256), `alignment` (power of two from 1–4096), `logical_base` (nonzero and page aligned), `logical_length` (page aligned, 4096–1073741824 bytes), and Boolean `scatter_gather`. The overflow-free aperture must fit the address width. Each PDO has an independent logical domain, so equal addresses on different devices do not alias. Translated MMIO resources must avoid the reserved model RAM range `[0x1000000000, 0x1000100000)`. These declarations describe a coherent synthetic bus master, never host physical memory or a PCI device.

`IoGetDmaAdapter` accepts the historical `DEVICE_DESCRIPTION` version 0/1 fields for an Internal bus master and publishes a version-one `DMA_ADAPTER` with its actual 104-byte `DMA_OPERATIONS` table. Version 2/3 probes return NULL without reading a modern description tail. Each indirect method is bound to that exact live adapter, independently of kernel imports. Implemented methods are `AllocateCommonBuffer`, `FreeCommonBuffer`, `GetDmaAlignment`, `GetScatterGatherList`, `PutScatterGatherList`, `PutDmaAdapter`, `AllocateAdapterChannel`, `MapTransfer`, `FlushAdapterBuffers` and `FreeMapRegisters`. `FreeAdapterChannel` and `ReadDmaCounter` remain named traps because this profile has no subordinate/system DMA controller. Common-buffer allocation/free and alignment queries require PASSIVE_LEVEL; Get/PutScatterGatherList require DISPATCH_LEVEL, and adapter release permits IRQL through DISPATCH_LEVEL. x64 ignores `CacheEnabled`. Unsupported version probes, incompatible declared capabilities and documented allocation shortages return NULL; malformed or unmodeled interface selections and backend failures remain explicit errors.

`KernelPhysicalMemory` assigns at most 256 model physical pages of 4096 bytes to existing RAM. CPU virtual addresses, physical page identities and device logical addresses are distinct. Built MDL PFN arrays expose these shared identities read-only; unbuilt descriptors have no usable PFNs. Small neighboring allocations may share a PFN but retain separate byte ranges and lifetimes. Common buffers, pool storage and request buffers use the same bytes already owned by `GuestMemory`, without a second DMA copy. A live SG mapping pins its exact data range and descriptor. Completion, pool/MDL free and teardown reject live dependencies before retirement. Unmapping a direct MDL only revokes its CPU system mapping; DMA remains able to reach its locked backing. `DmaWritable` records the write-lock contract separately from CPU mapping permissions: device writes require direct READ/OUT_DIRECT or writable nonpaged storage; WRITE/IN_DIRECT do not gain that permission merely because their CPU mapping is writable.

`GetScatterGatherList` validates CurrentVa/Length against the MDL's original range and creates logical page fragments over its existing backing. Available map registers allow the real four-argument void `AdapterListControl` to run inline before the API returns; otherwise admission retains the data/descriptor and reserves a callback in the PDO's FIFO until resources are released. This profile has no StartIo ownership, so the callback's second IRP argument is NULL. Callback return does not release a mapping. CPU access to a live SG data range requires Put first; a callback waiting for map registers has not yet granted the device ownership of those bytes. `PutScatterGatherList` may run inside the callback; after Put, the driver may complete the request and release the last adapter while its callback continuation and device hold survive until return. Common buffers require their original adapter, length, logical address and CPU address when freed. Logical addresses are never reused during the session, including restart. A resource wait without a real producer stalls explicitly; it does not fabricate a completion or deadline.

`AllocateAdapterChannel` requires DISPATCH_LEVEL and reserves an opaque non-NULL map-register token. Common buffers, SG lists and channel reservations share the same per-PDO quota and FIFO. Success accepts a real immediate or queued `AdapterControl`; an excessive requested count returns `STATUS_INSUFFICIENT_RESOURCES` without a callback. One unfinished allocation callback is allowed per guest device, and calling AllocateAdapterChannel from AdapterControl is rejected, including through a nested callback. The callback's four arguments contain the actual `DEVICE_OBJECT.CurrentIrp` snapshot taken at registration. That exact eight-byte field is writable on guest devices; zero or a live IRP routed to the device is accepted. A queued callback retains that packet until entry, after which the callback may complete it. The profile still has no StartIo; the separate SG callback's unused IRP argument remains NULL.

`AdapterControl` returns a 32-bit `IO_ALLOCATION_ACTION`; high RAX bits are ignored, and the action never replaces AllocateAdapterChannel's `STATUS_SUCCESS`. `DeallocateObject` releases unused or fully flushed registers on callback return. `DeallocateObjectKeepRegisters` retains them until `FreeMapRegisters` uses the exact adapter, token and original count. Returning `KeepObject` requires an unmodeled system controller and fails explicitly. The newly delivered allocation cannot be freed as retained before its callback returns; a different previously retained allocation may be freed when its own contract permits. Callback identity, retained registers and active mapped bytes have separate lifetimes, all checked before adapter or device teardown.

`MapTransfer` and `FlushAdapterBuffers` permit IRQL through DISPATCH_LEVEL; channel allocation and register free require DISPATCH_LEVEL. MapTransfer takes an MDL-relative index, reads and updates an actual ULONG length and returns the logical address by value. The bounded SG profile returns one backing-page fragment per call; contiguous subsequent indices in the same MDL and direction extend one operation. Non-SG maps the entire requested extent in one call if it fits the reserved count, without shortening it. The first map reserves a nonreused logical aperture sized to the register reservation; other allocations can interleave without overlapping it. All fragments share one growing physical pin. Device transactions can span the full currently mapped operation, while CPU access, MDL free and completion that retires pinned storage remain blocked until aggregate flush. Flush must match the initial index, MDL, direction and total actual mapped length. It releases mapped bytes without releasing registers, so the retained token can support another operation. Partial flushes, mixed-MDL operations and other MapTransfer patterns are outside this profile, rather than assumed invalid on every Windows system.

`KeFlushIoBuffers` validates a live locked/nonpaged MDL. The modeled platform is coherent, so either ReadOperation or DmaOperation value needs no separate cache copy; this call does not release DMA ownership or replace FlushAdapterBuffers. The [channel scenario](examples/driver-dma-channel-scenario.json) runs original `driver_wdm_dma_channel.c` through two MapTransfer calls, one cross-page device transaction, a separately declared IRQ/DPC, aggregate flush and exact register release. Genuine normal/CFG images use `NEVERD_WDM_DMA_CHANNEL_FIXTURE` and `NEVERD_WDM_DMA_CHANNEL_CFG_FIXTURE`.

Only READ/WRITE/IOCTL requests accept `dma_events`. Each event requires `after_100ns`, `device_id`, `logical_address`, `direction` and `length`; `write_memory` additionally requires exact-length `data_hex`, while `read_memory` rejects that field. Directions are from the device's perspective. Limits are 64 events per request, 1024 total, 16 MiB of total transaction bytes and 1 MiB per transaction; delay is nonnegative through INT64_MAX. Submission captures the PDO's current assigned epoch and anchors virtual time, but does not require a mapping that the forthcoming dispatch has yet to create. Delivery resolves the complete live logical range and direction, requires physical D0, and validates all backing bytes before any transaction effect. Source IRP completion does not cancel an event. Missing/released mappings, stale epochs, surprise removal or D3 record a failure and stop; events neither rebind nor invent an interrupt, register protocol or IRP completion. At one scheduled boundary, provider hardware publication precedes DMA bytes, which precede independently declared interrupt pulses. Timing remains cooperative, not instruction-level preemption.

Reports retain `configuration.pnp_devices[].dma` and flattened `configuration.dma_events`. Root `dma_transfers` rows identify `source_request_index`, `event_index`, `device_id`, `epoch`, `logical_address`, `direction`, `length` and `due_at_100ns`, with nullable `occurred_at_100ns`, `completed_at_100ns`, `mapping`, `adapter` and `failure_reason`; `data_hex` contains actual transferred bytes. Every declared transaction must finish without failure for `scenario_success`. The [DMA scenario](examples/driver-dma-scenario.json) uses original genuine-WDK `driver_wdm_dma.c` with optional `NEVERD_WDM_DMA_FIXTURE` / `NEVERD_WDM_DMA_CFG_FIXTURE`, exercising a common buffer through actual adapter pointers and a separately declared ISR→DPC completion. C/Python still use `scenario_json` without changing `neverd_driver_options_v1`. Missing artifacts skip explicitly; evidence is Linux-only. Subordinate controllers, V2/V3 methods, hardware descriptor engines, general KMDF DMA and other device models remain unsupported.

WDM power execution uses `kind: "power"` with a configured `device_id`. Every packet explicitly requires `minor` (`query` or `set`), `power_type` (`device` or `system`), `power_state` (`D0`/`D3` or `working`/`sleeping3`), `power_action` (`none` or `sleep`), `system_context` (a 32-bit integer or hexadecimal string) and `bus_completion`. System Query to Working is unsupported. Power packets reject file, transfer and cancellation fields. The complete `system_context` value is retained as an opaque packet fact; it does not select a parent or imply hibernate/fast-startup support. Routes require `DO_POWER_PAGABLE` without `DO_POWER_INRUSH`, and this profile executes power dispatch and `PoRequestPowerIrp` at `PASSIVE_LEVEL`. `PoCallDriver` forwards the same owned power IRP; `PoStartNextPowerIrp` follows the Vista+ contract without an extra serialization handshake. General power policy, WAIT_WAKE, other states/actions, shutdown/hibernate, inrush, nonpageable routes, general hardware and idle/wake policy remain unsupported.

Each `pnp_devices` entry may include `initial_reported_device_power: "D0"` or `"D3"`, independent of the required initial lifecycle facts D0/working. The provider PDO and each newly associated guest DEVICE_OBJECT receive separate notification state; `PoSetPowerState` returns and updates only the calling device's previous value. Without this explicit seed, a reached `PoSetPowerState` fails instead of assuming D0. Optional `requested_device_power` contains device-type packet templates, with the same six required power facts. At most 64 templates are accepted across all PDOs. A real `PoRequestPowerIrp` or framework power-policy transition with matching PDO, minor and target consumes that PDO's FIFO head; missing or mismatched entries fail, unused entries create no requests. No parent is guessed from the callback context. Each child owns an independent IRP and result row with `origin: "PoRequestPowerIrp"` and zero-based `response_index`; scenario rows use `origin: "scenario"` and null response index. Synchronous child completion can run its five-argument void callback before `PoRequestPowerIrp` returns `STATUS_PENDING`; callbacks may wait, and system S0 can complete before the independent D0 child. The callback's IO_STATUS_BLOCK snapshot remains valid through its return.

Request reports add nullable `power` alongside `pnp`; power rows have `file: null`. `power` records the explicit packet facts, `device_state_before`/`device_state_after`, `system_state_before`/`system_state_after`, nullable `requested_device_object` and actual `bus_status`/`bus_received_at_100ns`/`bus_completed_at_100ns`. Final PnP-device snapshots add `device_power` and `system_power`; live device snapshots add nullable `reported_device_power`. `scenario_success` counts scenario-origin rows against configured requests and requires every actual child and scenario row to complete successfully; unused FIFO templates do not fail the scenario. Genuine `driver_wdm_power.c` images use `NEVERD_WDM_POWER_FIXTURE` / `NEVERD_WDM_POWER_CFG_FIXTURE`, with normal/active-CFG native and C API/CLI coverage. Missing artifacts skip explicitly; execution evidence is Linux-only.

The [complete power scenario](examples/driver-power-scenario.json) runs the genuine power fixture through start, system query/sleep/wake and removal, with three explicit child responses; pass it with `--scenario`.

KMDF 1.33 support uses the exact 1.33.0 ABI: 458 function slots have stable guest identities, while the 88 APIs below have execution semantics. `WdfVersionBind` and `WdfVersionUnbind` manage guest bindings around the genuine WDK `FxDriverEntry` wrapper. `WdfGetDriver` reads the public driver globals. Non-PnP drivers, generic objects, control devices, queues and incoming requests share typed contexts, reference counts and executed cleanup/destroy/unload callbacks. All modeled framework calls and callbacks currently require `PASSIVE_LEVEL`; adding references after completed cleanup remains outside this profile. Unmodeled function slots, `WdfLdrQueryInterface`, class extensions and UMDF stop explicitly.

KMDF PnP drivers may register `EvtDriverDeviceAdd` through `WdfDriverCreate`. For each configured PDO, the callback receives a framework-owned `WDFDEVICE_INIT`; `WdfDeviceCreate` consumes it and creates a direct FDO/PDO pair. `WdfFdoInitWdmGetPhysicalDevice` and `WdfDeviceWdmGetPhysicalDevice` return the configured PDO. `WdfDeviceWdmGetAttachedDevice` returns the direct lower WDM object, `WdfWdmDeviceGetWdfDeviceHandle` maps a framework-owned WDM object back to its WDF handle, and `WdfDeviceGetDriver` returns the owning WDFDRIVER. On AddDevice failure the framework deletes any created FDO and runs guest cleanup/destroy callbacks before the provider retires; successful AddDevice clears `DO_DEVICE_INITIALIZING`. PnP IRPs forward to the configured bus response, and completed Remove deletes the WDF queue/device before the captured route and PDO retire. `WdfDeviceInitSetPnpPowerEventCallbacks` accepts the exact KMDF 1.33 structure with `EvtDevicePrepareHardware`, `EvtDeviceD0Entry`, `EvtDeviceD0EntryPostInterruptsEnabled`, `EvtDeviceD0ExitPreInterruptsDisabled`, `EvtDeviceD0Exit`, `EvtDeviceReleaseHardware`, `EvtDeviceQueryStop`, `EvtDeviceQueryRemove` and `EvtDeviceSurpriseRemoval`, the five self-managed I/O callbacks described below; other non-null event callbacks fail explicitly. Query callbacks run before forwarding to the bus and may wait. A rejected query completes with the callback status without a bus observation or power transition; `STATUS_PENDING` and `STATUS_NOT_SUPPORTED` are invalid callback results. SurpriseRemoval is a void notification before lower forwarding; its return register is ignored. After successful provider START, PrepareHardware receives distinct raw and translated `WDFCMRESLIST` handles, then D0Entry runs at `PASSIVE_LEVEL` with `WdfPowerDeviceD3Final` before the original IRP completes. Empty lists return zero from `WdfCmResourceListGetCount` and NULL from `WdfCmResourceListGetDescriptor`. For a configured `register_bank` provider, both lists expose the assigned read-only `CM_PARTIAL_RESOURCE_DESCRIPTOR` entries; descriptor pointers remain valid through ReleaseHardware and are retired afterward. A driver may map the translated memory assignment with `MmMapIoSpace` and must unmap it before final STOP or Remove completion. A failed PrepareHardware or D0Entry status becomes the START status; ReleaseHardware still runs after either failure, while D0Exit is skipped. Successful STOP, surprise removal and direct removal from D0 invoke D0Exit with `WdfPowerDeviceD3Final`, then ReleaseHardware, before IRP completion. A later START prepares hardware and enters D0 again. The public scenario must explicitly remove a device after failed START. PnP queues accept `WdfUseDefault` and `WdfTrue` for automatic power management, or `WdfFalse` to keep dispatch under driver control. A managed queue reports `WdfIoQueuePnpHeld` until D0 entry finishes and again before D0 exit; it resumes request delivery after entering D0. Before a managed queue leaves D0, a registered `EvtIoStop` runs for each driver-owned request with Suspend for STOP or Purge for surprise removal. The driver may complete the request, call `WdfRequestStopAcknowledge`, or leave it to an existing completion producer. Without `EvtIoStop`, the framework also waits for every delivered request to complete. A true requeue flag returns the request to framework ownership for delivery after restart. A false flag retains driver ownership and requires `EvtIoResume` after D0 entry. Queued requests remain held during STOP and are presented after restart, including when no hardware callback is registered. The D0 exit and PnP completion wait for the real request completion; if no producer remains, the scheduler reports a stalled `model_error`. A retained request without a resume callback remains unsupported. Unmodeled resource types, resource mutation, WDF interrupt objects, general KMDF power policy and other PnP event callbacks remain unsupported. The optional genuine WDK `driver_kmdf_pnp.c` fixture covers normal and active CFG images through native and public C API/CLI tests using `NEVERD_KMDF_PNP_FIXTURE` / `NEVERD_KMDF_PNP_CFG_FIXTURE`.

Control devices require a copied printable-ASCII name and the exact SDDL `D:P(A;;GA;;;WD)`. This grants universal access and avoids inventing a caller token; other security descriptors, unnamed devices and automatic names are unsupported. Device initialization owns one WDM device. Requests can select it through the existing session namespace using `\DosDevices\Name` or `\??\Name` symbolic-link aliases; reports retain the canonical device name. Successful creation consumes and clears the initializer; failed creation rolls back partial device ownership. `WdfControlFinishInitializing` gates I/O delivery. Deletion removes the device and its links only when modeled files, work items and requests permit it; cancellation or draining during deletion is unsupported.

The 96-byte `WDF_IO_QUEUE_CONFIG` supports manual, sequential and parallel default queues and nondefault queues of each dispatch type, all with explicit passive execution and no framework synchronization. A parallel queue uses `Settings.Parallel.NumberOfPresentedRequests = -1` for unlimited delivery or a positive finite limit. A default sequential queue accepts additional incoming requests while one is presented; each waits in FIFO order until that request completes or leaves the queue. Queued requests may be canceled without calling the driver. Control-device queues are not power managed. `WdfIoQueueStop` pauses delivery but continues accepting requests; `WdfIoQueueStart` resumes queued delivery, and `WdfIoQueueGetState` reports queued and delivered counts. Retrieval while stopped returns `STATUS_WDF_PAUSED`. A stop-completion callback runs with the supplied context after every request already delivered to the driver has completed or left the queue; waiting requests do not delay it. A second callback registration while one is pending fails explicitly. Specific READ/WRITE/IOCTL callbacks take precedence over the default callback. Accepted requests return `STATUS_PENDING` even when completed synchronously; a void callback's return register does not complete its request. A manual default queue accepts incoming requests without invoking an I/O callback. `WdfRequestForwardToIoQueue` transfers an owned request to another queue on the same device, freeing its source presentation slot. An automatic destination delivers through its own matching callback when a slot is available and otherwise retains the request; if there is no matching callback, it completes the queued request with `STATUS_INVALID_DEVICE_REQUEST` when a slot becomes available. The framework owns the request while queued. `WdfIoQueueRetrieveNextRequest` transfers a pending request from a manual or sequential queue to the driver in FIFO order, and returns `STATUS_NO_MORE_ENTRIES` with a null output when empty; parallel queue retrieval returns `STATUS_INVALID_DEVICE_STATE`. `WdfRequestRequeue` returns a retrieved, unmarked request to the head of the same manual queue. If cancellation wins before a request is first delivered to the driver, the framework removes and completes the queued request with `STATUS_CANCELLED`, running its cleanup callbacks before retiring the IRP; a later retrieval observes an empty queue. Deferred completion uses the existing scheduler, including explicit `defer_callback_drain` batches on asynchronous files or separate files after forwarding. For an incoming request to an automatic default queue, a missing handler completes it with `STATUS_INVALID_DEVICE_REQUEST`; zero-length READ/WRITE completes without delivery unless enabled. The default file package completes CREATE/CLEANUP/CLOSE successfully with Information=0. Broader PnP and power behavior remains outside this profile.

`WdfIoQueueReadyNotify` registers one `EvtIoQueueState` callback for a manual queue. At `PASSIVE_LEVEL`, it receives `(WDFQUEUE, WDFCONTEXT)` when the queued count changes from zero to nonzero, even if the driver still owns earlier retrieved requests. Registering on an already nonempty queue may notify immediately; a stopped queue waits until `WdfIoQueueStart`. Duplicate registration and deregistration before stopping return `STATUS_INVALID_DEVICE_REQUEST`. Passing NULL after `WdfIoQueueStop` deregisters the callback.

`WdfIoQueueFindRequest` scans a manual queue without transferring request ownership and adds one request reference on success. The driver releases that reference with `WdfObjectDereference`; `WdfIoQueueRetrieveFoundRequest` transfers ownership of a still-queued request, while a request removed by cancellation returns `STATUS_NOT_FOUND`. Optional request parameters use the same layout as `WdfRequestGetParameters`. A live framework file object filters `WdfIoQueueFindRequest`; `WdfIoQueueRetrieveRequestByFileObject` takes the next matching request from a manual or sequential queue and leaves the output unchanged when none matches.

A configured `EvtIoCanceledOnQueue` receives `(WDFQUEUE, WDFREQUEST)` only for a request that the driver previously received and then forwarded or requeued, or one that the caller-context callback explicitly enqueued. A never-delivered queued request is completed by the framework with `STATUS_CANCELLED` without that callback. Cancellation transfers the notified request back to the driver, which must complete it inside the callback or later and cannot requeue it. Queue purge and state completion wait for the callback to return and for the driver-owned request to complete.

`WdfIoQueueDrain` stops accepting new requests with `STATUS_INVALID_DEVICE_STATE` while delivering requests already queued; its completion callback runs only when both queued and driver-owned counts reach zero. Forwarding into a drained queue returns `STATUS_WDF_BUSY`. `WdfIoQueueStart` restores acceptance. A pending drain callback must finish before another queue-state change.

`WdfIoQueuePurge` also rejects new arrivals and cancels every request still queued by the framework with `STATUS_CANCELLED`, running request cleanup before IRP retirement. For already delivered requests marked cancelable, it records cancellation on the original IRP and invokes each driver cancel callback. Its optional state callback waits until queued and driver-owned requests, including active cancel callbacks, have finished. Requests not marked cancelable remain driver-owned until the driver completes them. Start restores acceptance after purge.

The synchronous forms `WdfIoQueueStopSynchronously`, `WdfIoQueueDrainSynchronously`, and `WdfIoQueuePurgeSynchronously` suspend their PASSIVE_LEVEL caller until the relevant requests retire. Stop keeps accepting but pauses delivery and waits for delivered requests; drain rejects arrivals while delivering queued requests and waits for both queued and delivered requests; purge cancels queued and marked delivered requests and waits through cancellation callbacks. A wait without a completion producer reports a stalled model error.

`WdfIoQueueStopAndPurge` and `WdfIoQueueStopAndPurgeSynchronously` cancel requests already queued and marked driver-owned requests, then keep accepting new requests without delivering them until `WdfIoQueueStart`. The asynchronous state callback and synchronous wait finish after the original delivered requests and their cancellation callbacks; requests added after the stop-and-purge call remain queued and do not delay notification.

Request parameters use the 40-byte `WDF_REQUEST_PARAMETERS` layout. Input/output accessors return the logical lengths, preserving buffered aliases and existing direct-I/O MDL mappings; direct IOCTL input remains buffered. Wrong directions and insufficient buffers return documented statuses. Completion runs request cleanup while buffers are live, completes the IRP and releases request-owned pins, then destroys child objects and the request when references permit. New buffer and parameter accessors are rejected once completion begins; already obtained buffer pointers remain usable during cleanup. An external object reference preserves context, not completed IRP access.

Cancellation is modeled for requests on the control-device queues described above. `WdfRequestMarkCancelableEx` returns `STATUS_CANCELLED` without invoking a callback if cancellation already occurred. Successful `WdfRequestUnmarkCancelable` removes the callback; a later cancellation records the canceled state without delivering that callback. `WdfRequestIsCanceled` observes that state on an unmarked live request. After successful marking, completion requires successful unmarking or delivery of the cancellation callback: a merely queued callback does not authorize completion. Once delivery begins, the callback and a worker may coordinate completion, including when the callback waits. A separate internal reference retains the request through cancellation-callback return; completion still retires the IRP first, and the final request-destroy continuation may itself wait. DPCs take priority, followed by cancellation callbacks in FIFO order, then ordinary workers; queued cancellation callbacks also precede resuming ready passive waiters.

For an already canceled request, legacy void `WdfRequestMarkCancelable` executes a synchronous guest cancellation callback before returning. This child continuation can wait, complete the request through nested cleanup, and run final destruction before the original API resumes. If cancellation occurs after registration, delivery uses the scheduled cancellation path above. This reproduces the public source behavior at `PASSIVE_LEVEL` with `WdfSynchronizationScopeNone`; [Microsoft](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfrequest/nf-wdfrequest-wdfrequestmarkcancelable) directs drivers without automatic synchronization to use Ex. Legacy execution here is compatibility behavior, not a recommendation to use it in that configuration.

`WdfRequestGetInformation` and `WdfRequestSetInformation` share the original 64-bit `IRP.IoStatus.Information`, including direct guest writes. Set assigns the value; transfer-length validation occurs at completion. `WdfRequestCompleteWithInformation` writes the same field before cleanup; changes through a previously saved IRP during cleanup determine the final Information even though GetInformation already returns zero in that phase. `WdfRequestGetIoQueue` returns the delivering queue while the driver owns the request, including the manual destination after retrieval; queued requests are framework-owned and cannot be inspected by the driver. With the default file configuration, `WdfRequestGetFileObject` returns NULL: no WDF file object is invented from the WDM FILE_OBJECT. `WdfRequestWdmGetIrp` returns the same IRP; guest `IoCompleteRequest`/`IofCompleteRequest` cannot bypass WDF completion. While a request handle remains valid during or after completion, GetInformation/GetIoQueue return zero, and MDL retrieval first clears a valid output slot to NULL then returns `STATUS_INTERNAL_ERROR`. SetInformation/GetFileObject/WdmGetIrp remain rejected then. Existing buffer and parameter accessor restrictions are unchanged.

`WdfRequestRetrieveInputWdmMdl` and `WdfRequestRetrieveOutputWdmMdl` lazily describe the existing SystemBuffer for buffered WRITE input, READ output and IOCTL input/output. Each requested direction must be valid and nonempty before using the request’s single cached descriptor; the first successful retrieval fixes its ByteCount even when the other direction has a different logical length. `MmGetSystemAddressForMdlSafe` returns the original VA for this descriptor; additional system mapping, system unmapping and driver freeing are rejected. Direct READ output, WRITE input and IOCTL output instead return the existing `IRP.MdlAddress` without mapping it merely by retrieval; direct IOCTL input uses the SystemBuffer cache. Descriptors, IRP and buffers retire at completion. Cancellation-internal or external references retain only WDF context, not completed I/O storage. Built physical PFNs are read-only; WDF MDL retrieval for `METHOD_NEITHER` remains unsupported; request-owned WDFMEMORY uses a separate locked user-page mapping.

For buffered, direct and neither KMDF transfers, `WdfDeviceInitSetIoInCallerContextCallback` runs a prequeue callback in the requestor process at `PASSIVE_LEVEL`. It must complete the request or call `WdfDeviceEnqueueRequest` once before delivery to the default queue. For `METHOD_NEITHER` IOCTL and neither READ/WRITE, `WdfRequestRetrieveUnsafeUserInputBuffer` and `WdfRequestRetrieveUnsafeUserOutputBuffer` expose the original user VAs only in that callback. `WdfRequestProbeAndLockUserBufferForRead` and `WdfRequestProbeAndLockUserBufferForWrite` check page rights and pin request-owned memory; `WdfMemoryGetBuffer` returns a system alias that remains usable in the queue callback outside the requestor context. Completion releases the pins and aliases. The original scenario buffers and explicitly declared `user_buffers` are eligible, including buffers reached through embedded pointers. Locking remains restricted to the current request in caller context; arbitrary user mappings remain unsupported.

`WdfRequestRetrieveInputMemory` and `WdfRequestRetrieveOutputMemory` expose request-owned WDFMEMORY views over existing buffered or direct input/output buffers. Repeated retrieval of one direction keeps its handle; `WdfMemoryGetBuffer` returns the original buffer and logical length. Zero-length or invalid-direction requests fail with their WDF statuses, and neither I/O still requires the caller-context probe-and-lock path. These borrowed views add no MDL pin and expire when the request completes.

The framework sequences `EvtDeviceSelfManagedIoInit`,
`EvtDeviceSelfManagedIoSuspend`, `EvtDeviceSelfManagedIoRestart`,
`EvtDeviceSelfManagedIoFlush` and `EvtDeviceSelfManagedIoCleanup` around its
hardware and queue transitions. Init runs once, restart follows a successful
resume, and cleanup runs once before object destruction. Surprise removal
drains power-managed requests before suspending self-managed I/O; orderly
removal suspends self-managed I/O first. Downward callbacks
precede provider power-off; D0 entry follows provider power-on. Ordinary D3/D0
transitions retain hardware resource handles and mappings.
`WdfDeviceInitSetPowerPolicyOwnership` controls ownership (FDO default true,
filter default false). The default policy maps Sleeping3 to D3 and Working to
D0, consuming explicit `requested_device_power` responses. Each generated child
has its own IRP, `origin: "framework_power_policy"` and `response_index`.
System queries wait for a separate matching device query and inherit its
status without changing power state. The S3 SET parent waits for D3 completion;
the S0 parent may finish after D0 is issued. Idle/wake policy, WDF interrupt objects and automatic failed-device
reenumeration remain outside this profile.

Modeled KMDF APIs: `WdfDriverCreate`, `WdfDriverGetRegistryPath`, `WdfDriverWdmGetDriverObject`, `WdfWdmDriverGetWdfDriverHandle`, `WdfWdmDeviceGetWdfDeviceHandle`, `WdfObjectGetTypedContextWorker`, `WdfObjectAllocateContext`, `WdfObjectContextGetObject`, `WdfObjectReferenceActual`, `WdfObjectDereferenceActual`, `WdfObjectCreate`, `WdfObjectDelete`, `WdfControlDeviceInitAllocate`, `WdfDeviceInitFree`, `WdfDeviceInitAssignName`, `WdfDeviceInitSetDeviceType`, `WdfDeviceInitSetExclusive`, `WdfDeviceInitSetFileObjectConfig`, `WdfDeviceInitSetIoType`, `WdfDeviceInitSetIoInCallerContextCallback`, `WdfDeviceInitSetPnpPowerEventCallbacks`, `WdfDeviceInitSetPowerPolicyOwnership`, `WdfCmResourceListGetCount`, `WdfCmResourceListGetDescriptor`, `WdfFdoInitWdmGetPhysicalDevice`, `WdfFdoInitSetFilter`, `WdfDeviceCreate`, `WdfDeviceEnqueueRequest`, `WdfDeviceCreateSymbolicLink`, `WdfControlFinishInitializing`, `WdfDeviceWdmGetDeviceObject`, `WdfDeviceWdmGetAttachedDevice`, `WdfDeviceWdmGetPhysicalDevice`, `WdfIoQueueCreate`, `WdfDeviceGetDefaultQueue`, `WdfDeviceGetDriver`, `WdfDeviceGetIoTarget`, `WdfIoQueueGetDevice`, `WdfIoQueueGetState`, `WdfIoQueueStop`, `WdfIoQueueStopSynchronously`, `WdfIoQueueStart`, `WdfIoQueueDrain`, `WdfIoQueueDrainSynchronously`, `WdfIoQueuePurge`, `WdfIoQueuePurgeSynchronously`, `WdfIoQueueStopAndPurge`, `WdfIoQueueStopAndPurgeSynchronously`, `WdfIoQueueReadyNotify`, `WdfIoQueueRetrieveNextRequest`, `WdfIoQueueRetrieveRequestByFileObject`, `WdfIoQueueFindRequest`, `WdfIoQueueRetrieveFoundRequest`, `WdfRequestForwardToIoQueue`, `WdfRequestRequeue`, `WdfRequestStopAcknowledge`, `WdfRequestComplete`, `WdfRequestCompleteWithInformation`, `WdfRequestFormatRequestUsingCurrentType`, `WdfRequestSend`, `WdfRequestGetStatus`, `WdfRequestSetCompletionRoutine`, `WdfRequestGetCompletionParams`, `WdfRequestGetParameters`, `WdfRequestRetrieveInputBuffer`, `WdfRequestRetrieveOutputBuffer`, `WdfRequestRetrieveInputMemory`, `WdfRequestRetrieveOutputMemory`, `WdfRequestRetrieveUnsafeUserInputBuffer`, `WdfRequestRetrieveUnsafeUserOutputBuffer`, `WdfRequestProbeAndLockUserBufferForRead`, `WdfRequestProbeAndLockUserBufferForWrite`, `WdfMemoryGetBuffer`, `WdfRequestRetrieveInputWdmMdl`, `WdfRequestRetrieveOutputWdmMdl`, `WdfRequestSetInformation`, `WdfRequestGetInformation`, `WdfRequestGetFileObject`, `WdfFileObjectGetFileName`, `WdfFileObjectGetFlags`, `WdfFileObjectGetDevice`, `WdfFileObjectWdmGetFileObject`, `WdfRequestGetIoQueue`, `WdfRequestWdmGetIrp`, `WdfRequestMarkCancelable`, `WdfRequestMarkCancelableEx`, `WdfRequestUnmarkCancelable`, `WdfRequestIsCanceled`.

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
| `MmMapLockedPagesSpecifyCache`, `MmGetSystemAddressForMdlSafe`, `MmUnmapLockedPages` | MDL system aliases and process-owned user views retain physical cache attributes and permissions; nonpaged pool MDLs reuse the original pool mapping through the safe helper |
| `IoAllocateMdl`, `MmBuildMdlForNonPagedPool`, `MmProbeAndLockPages`, `MmUnlockPages`, `IoFreeMdl` | Standalone or IRP-associated nonpaged-pool/user descriptors, mutable chain links, independent locks and shared system aliases; no quota |
| `ZwOpenKey`, `ZwCreateKey`, `ZwQueryValueKey`, `ZwSetValueKey`, `ZwDeleteValueKey`, `ZwDeleteKey`, `ZwClose` | Explicit session registry, per-handle rights and lifetime, query buffer sizing and mutations; no host registry access |
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
| `PoSetPowerState`, `PoRequestPowerIrp` | Independent per-device notification state and real device-power children from explicit per-PDO response FIFOs; bounded pageable profile above |
| `IoCreateSymbolicLink`, `IoDeleteSymbolicLink` | ASCII `\DosDevices\Name` or `\??\Name` within one session namespace, targeting `\Device\Name` |
| `DbgPrint`, `DbgPrintEx` | Checked Win64 variadic formatting, at most 512 output bytes; all debugger filters enabled |
| `IoGetCurrentIrpStackLocation` | Returns the stack location of the active modeled IRP; normal compiled WDM macros read the same guest field |
| `KeGetCurrentIrql` | Reads current guest IRQL/CR8, including explicit raises and restores; dispatch and workers start at `PASSIVE_LEVEL`, DPCs at `DISPATCH_LEVEL` |
| `KfRaiseIrql`, `KeLowerIrql` | Actual x64 WDK raise/lower imports, including inlined `KeRaiseIrqlToDpcLevel` and `KeRaiseIrqlToSynchLevel`; saved IRQL values must be restored in LIFO order on the same execution before return or suspension. CR8 reads observe each change. This does not simulate instruction-level interrupt preemption. |
| `KeEnterCriticalRegion`, `KeLeaveCriticalRegion`, `KeEnterGuardedRegion`, `KeLeaveGuardedRegion`, `KeAreApcsDisabled`, `KeAreAllApcsDisabled` | Nested per-thread APC-disable state. Critical regions and held KMUTEX objects disable normal kernel APCs; guarded regions and IRQL >= APC_LEVEL disable all APCs. New system threads begin inside one critical region. Unmatched leaves and unbalanced callback returns fail; APC delivery is not modeled. |
| `KeInitializeSpinLock`, `KeAcquireSpinLockRaiseToDpc`, `KeReleaseSpinLock`, `KeAcquireSpinLockAtDpcLevel`, `KeReleaseSpinLockFromDpcLevel`, `KeTryToAcquireSpinLockAtDpcLevel` | Resident, aligned executive locks on cooperative CPU0; exact owner and acquire/release pairing, saved IRQL restoration, and nonblocking try-acquire. Contended blocking acquisitions stop explicitly because the scheduler cannot make progress while spinning. |
| `IoAllocateWorkItem`, `IoQueueWorkItem`, `IoFreeWorkItem` | Device-owned opaque work items; `DelayedWorkQueue` only, callbacks receive the device and context at `PASSIVE_LEVEL`; queued items cannot be freed |
| `PsCreateSystemThread`, `PsTerminateSystemThread`, `ObReferenceObjectByHandle`, `ObfDereferenceObject`, `ZwClose` | Bounded system-process threads run at `PASSIVE_LEVEL` with a separate guest stack. Kernel handles and referenced opaque thread objects have independent lifetimes. Termination does not return to the guest, signals the thread object, and a normal start-routine return stops explicitly. NULL process/client IDs and NULL or kernel-handle-only object attributes are supported; APCs, thread priorities and typed object references are not. |
| `KeInitializeDpc`, `KeInsertQueueDpc`, `KeRemoveQueueDpc`, `KeSetImportanceDpc`, `KeSetTargetProcessorDpc` | Opaque DPC storage, four guest callback arguments, `DISPATCH_LEVEL`, duplicate/remove semantics and importance; target CPU0 only |
| `KeInitializeTimer`, `KeInitializeTimerEx`, `KeSetTimer`, `KeSetTimerEx`, `KeCancelTimer`, `KeReadStateTimer` | Notification/synchronization timers; relative/absolute 100 ns deadlines, periodic milliseconds, rearm/cancel and signal queries in virtual time |
| `KeInitializeEvent`, `KeSetEvent`, `KeResetEvent`, `KeClearEvent`, `KeReadStateEvent` | Notification/synchronization events with distinct signal consumption; `KeSetEvent` accepts Increment=0 and Wait=FALSE only |
| `KeInitializeSemaphore`, `KeReleaseSemaphore`, `KeReadStateSemaphore` | Resident counting semaphore with a positive limit, nonnegative initial count, and one count consumed per successful wait; release accepts Increment=0 and Wait=FALSE, and an excess adjustment raises `STATUS_SEMAPHORE_LIMIT_EXCEEDED` |
| `KeInitializeMutex`, `KeReleaseMutex`, `KeReadStateMutex` | Resident KMUTEX with execution-owned recursive acquisition; KeReleaseMutex returns the previous signed signal state, requires the owner and matching DISPATCH_LEVEL acquisition context, and accepts Wait=FALSE only. Held mutexes block return, reinitialization and storage release. A non-owner release raises `STATUS_MUTANT_NOT_OWNED`. |
| `KeWaitForSingleObject` | One initialized event, timer, semaphore or mutex, or a referenced thread object; nonalertable `KernelMode`, reason `Executive`; zero polling, finite relative/absolute or infinite waits; nonzero/infinite waits require IRQL <= APC_LEVEL |
| `KeDelayExecutionThread` | Nonalertable `KernelMode` relative/absolute delay at IRQL <= APC_LEVEL; resumes the saved guest frame after virtual time advances |
| `IoMarkIrpPending` | Marks the live active IRP; the equivalent WDM macro's stack-control write is also modeled; dispatch must return `STATUS_PENDING` |
| `IoSetCancelRoutine`, `IoAcquireCancelSpinLock`, `IoReleaseCancelSpinLock`, `IoCancelIrp` | Live WDM IRP cancel-routine exchange, nonrecursive system cancel lock with saved IRQL, and synchronous driver-initiated cancellation; the WDK inline helper uses the same IRP field |
| `IofCompleteRequest`, `IoCompleteRequest` | `IO_NO_INCREMENT`; executes completion unwinding, supports stopped/resumed completion and retires IRP/MDL/buffer storage only at its terminal boundary |
| `memcpy`, `memmove`, `memset`, `memcmp`, `RtlCopyMemory`, `RtlMoveMemory`, `RtlFillMemory`, `RtlZeroMemory`, `RtlCompareMemory` | Bounded guest buffer operations, at most 1 MiB per call; non-overlapping copy APIs reject overlaps |

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

`IoAllocateMdl` allocates metadata for a nonempty, nonoverflowing buffer of at
most 1 MiB; it does not probe or lock that buffer. `Irp` may be NULL or a live
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
The profile is `wdm-x64-scheduled-v74`. `nt_status` remains the DriverEntry
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

Filter callbacks receive stable `EXCEPTION_POINTERS`, exception-record and
`CONTEXT` storage on a separate bounded stack. Callback execution preserves the
original full CPU state, including floating-point/SIMD registers and flags.
The exposed `CONTEXT` supports integer/control fields; a negative filter may
change supported GPRs, RIP/RSP and arithmetic/direction flags to resume an
in-context user read/write CPU exception. Invalid context changes fail before
resume. Exception pointers, exception-record fields and unsupported context
fields are validated after every filter; mutating them remains unsupported.
Continuing a modeled API raise remains unsupported. Callback identity
inherits the parent thread, process and user-access authority without giving
an unrelated worker access to the requesting process.

Nested filter exceptions retain a linked `EXCEPTION_RECORD` and revisit the
suspended protected scopes; collided finally unwinding resumes after the
already entered cleanup. The planner restores nonvolatile GPRs and full
XMM6–XMM15 values, checks chained V1 records, and unwinds partial prologues.
Canonical epilogues are decoded from current executable guest bytes: only
remaining stack adjustments, nonvolatile pops and the return are simulated.
C++ personalities and incomplete metadata still fail explicitly. An uncaught API or supported user-memory
exception stops with `model_error`; other CPU memory, interrupt and
invalid-instruction faults remain terminal.

`__GSHandlerCheck_SEH` checks the live image security cookie before language
handler search and again during unwind, including frames without finally
callbacks. Fixed and dynamically aligned cookie slots use checked signed
frame offsets; cookie encoding uses the original frame pointer. The wrapped
handler flags control C scopes independently of the wrapper's cookie checks.
Prologue and epilogue unwinding does not read an unestablished cookie. A
mismatch stops before the affected filter, cleanup or handler can run. The
standalone `__GSHandlerCheck` and GS/C++ wrappers remain unsupported. See
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
