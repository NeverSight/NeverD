**Languages**: [English](testing.md) | [简体中文](zh-CN/testing.md) | [繁體中文](zh-TW/testing.md) | [日本語](ja/testing.md) | [한국어](ko/testing.md) | [Français](fr/testing.md) | [Deutsch](de/testing.md) | [Español](es/testing.md) | [Italiano](it/testing.md) | [Русский](ru/testing.md) | [العربية](ar/testing.md)

[← Documentation Index](README.md)

# Testing NeverD

NeverD's tests cover three different questions: whether a representation has
the expected shape, whether a full pipeline route works for a binary fixture,
and whether generated code preserves behavior. Choose the smallest suite that
answers the question behind a change, then run the broader aggregate before a
high-risk pull request.

## Configure a test build

Tests are disabled unless `BUILD_TESTING` is enabled. A Release build is the
normal choice for the full suite; Debug preserves assertions and stepping but
is intentionally unoptimized and is not representative for decode benchmarks.

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build-release --parallel 4
```

The full fixture set needs `clang` for cross-target compilation and LLVM's
linkers (`ld.lld` and `lld-link`) on `PATH`. CMake builds many relocatable
fixtures unconditionally and linked ELF/PE fixtures when the matching linker is
available. A test skipped because the host cannot compile or link its fixture
is unexecuted coverage, not a pass for that target.

The SBF source differential suite additionally needs `rustc`; it executes both
generated C and generated Rust. Treat a missing compiler skip as missing
backend evidence, not as semantic success.

See [CONTRIBUTING.md](../CONTRIBUTING.md) for clone, build-profile, and macOS
prebuilt-LLVM guidance.

## Driver emulation checks

Enable `NEVERD_ENABLE_DRIVER_EMULATION=ON` together with `BUILD_TESTING=ON`
to build the focused execution suite and the shared C API/CLI checks:

```bash
cmake --build build-release --target \
  NeverDDriverEmulationTests NeverDDriverEmulationPublicTests --parallel 4
ctest --test-dir build-release -L '^NeverDDriverEmulation' --output-on-failure
```

Fixtures exercise guest initialization, returned success and failure,
unsupported behavior, memory faults, strict scenario parsing, bounded execution,
and buffered/direct I/O, READ/WRITE, independent file lifetimes,
MDL permissions, dynamic export resolution, guest varargs, and structured CPU
faults through create, transfers, cleanup, close and unload.
Use the
[`emulate-driver` CLI](driver-emulation.md) to verify JSON and process exit
codes. Production builds may enable this feature with `BUILD_TESTING=OFF`;
test-only Unicorn configuration must not be required by `libneverd`.

Additional fixtures cover driver-owned nonpaged pool MDLs, independent descriptor
and buffer lifetimes, registry query layouts and short buffers, handle rights,
deletion and leaks, and full-width `information_hex` for zero-output IOCTLs.
External acceptance also covers Zero synchronous direct reads/writes and
statistics queries.

Backend tests verify full CPU contexts (registers, flags, SIMD, FPU and CR8),
shared memory and rejection of foreign or faulted contexts. Compiled
`driver_dispatcher.c` fixtures execute real DPC and worker callbacks, timer
boundaries, notification/synchronization events and timers, nonalertable
`KernelMode` waits with reason `Executive`, timeout/delay, multiple blocked
stacks, set/reset wake latching, callback arguments and invalid
IRQL/lifetimes. Worker tests retain pending/completion, queue, stall and
shared-budget coverage. These cases establish the documented subset, not full
Windows asynchronous support.

`DriverAsyncTests.cpp` submits two pending WDM IOCTLs on independent file
objects with `defer_callback_drain`, then checks that both dispatches precede
either worker and that each IRP completes with its own output. A second batch
cancels one IRP while the other completes and releases each work item and
request independently. C API/CLI tests run both eight-request scenarios.
Synchronous dispatch and same-file overlap are rejected explicitly; arbitrary
thread races are not covered by this deterministic batch boundary. The genuine
WDK neither-I/O fixture also verifies that another requestor can complete a
synchronous request before the first exited requestor's locked-MDL worker runs,
in normal/active-CFG images at preferred and relocated bases.

`DriverKMDFControl.ParallelQueueDeliversTwoPendingRequestsBeforeWorkers`
uses a genuine WDK control driver with an unlimited parallel default queue.
Two IOCTLs on an asynchronous file enter separate work items before either
worker runs; a third request completes synchronously, then both workers
complete their own IRPs. The case runs normal/active-CFG images at preferred
and relocated bases. Model-level queue tests verify parallel overlap and
sequential exclusion; another native case batches two independent synchronous
file objects. The C API test checks the asynchronous-file batch and output bytes.

`DriverKMDFControl.SequentialQueuePresentsWaitingRequestAfterWorker` submits
two IOCTLs to a default sequential queue while the first worker remains
pending. The queue accepts the second request and presents it only after the
first worker completes, in normal/active-CFG images at preferred and relocated
bases. A companion genuine-driver case cancels an undelivered waiter. Model
tests cover FIFO promotion, cancellation before delivery, and framework-only
completion when a handler is absent or a zero-length transfer is disabled. The
C API covers both successful waiting and cancellation.

`DriverKMDFControl.StoppedSequentialQueueResumesWaitingRequestAfterWorker`
stops delivery from a genuine WDK default queue while its first request is
driver-owned. The second request remains queued; after the worker completes
the first, `WdfIoQueueStart` presents the second before returning. The driver
checks queue state bits and queued/delivered counts at each boundary. Normal and
active-CFG images run at preferred and relocated bases. Model tests also cover
paused explicit retrieval, state counts, forwarding to a stopped nondefault
automatic queue, and resuming all waiters on an unlimited parallel queue; a C
API test covers the public sequential scenario.

`DriverKMDFControl.BoundedParallelQueueWaitsForPresentedCompletion` uses
`NumberOfPresentedRequests=1`; the second handler runs only after the first
worker completes. Normal/active-CFG and preferred/rebased images are covered.
Another genuine-driver case cancels a request before presentation. Model tests
exercise limits of one and two, FIFO handoff and queued cancellation; the C API
runs both unlimited and finite modes.

`DriverKMDFControl.NondefaultAutomaticQueuesForwardAndRespectPresentationLimits`
uses genuine WDK sequential and two-presented nondefault queues. Requests
forwarded from a sequential default queue enter the destination callbacks in
FIFO order, while excess requests wait until a worker completes. Normal and
active-CFG images run at preferred and relocated bases; the C API runs both
modes. Model tests cover destination ownership, cancellation before delivery,
completion when no destination callback matches, explicit retrieval from a
sequential queue, bounded parallel presentation and release of a bounded source
queue. A genuine-driver case cancels an automatic waiter before delivery. A
separate model test covers incoming
requests in a manual default queue, FIFO retrieval and queued cancellation.

`DriverKMDFControl.ManualQueueReleasesSequentialSourceAndRetrievesInWorker`
uses a genuine WDK control driver with a nondefault manual queue. Its first
IOCTL is forwarded from a sequential default queue; a second IOCTL on the
same asynchronous file completes before a work item retrieves and completes
the first. Normal/active-CFG images run at preferred and relocated bases. Model
tests cover FIFO retrieval, ownership transfer, invalid forwards and deletion
of a queue with live requests; a C API case checks the public scenario. A
second genuine mode checks queued cancellation at virtual time zero or while
the worker waits, with model cases for request cleanup ordering.
`DriverKMDFControl.ManualRequestRequeueReturnsSameRequestToWorker` exercises
`WdfRequestRequeue` against both WDK images and load bases. Model cases verify
head insertion, ownership transfer, cancelable-request rejection and cleanup
when cancellation precedes requeue; the C API runs the same request flow.

An `asynchronous_file` CREATE case checks that the guest FILE_OBJECT and IRP
omit synchronous flags, two pending IOCTLs on the same file complete with
distinct output, canceling one overlapping IRP leaves the other live, and
CLEANUP cannot overtake an active transfer. A model-level
READ test completes two same-file IRPs out of order without advancing an
implicit current byte offset. Genuine WDK
neither-I/O runs a second same-file transfer before its first worker across
normal/active-CFG and relocated images. The C API/CLI use the same public
schema. These cases do not model completion ports or implicit file position.

`driver_context_limits.c`: API IRQL ceilings come from `KernelAPIIRQL.def`,
with argument-dependent checks in the owning model. DPCs cannot call registry
APIs or allocate, free or access paged pool; Unicode `DbgPrint` conversions
require `PASSIVE_LEVEL`, while supported ANSI output and nonpaged operations
remain usable at `DISPATCH_LEVEL`. Callback stacks have bounded ranges; an
escaping stack pointer cannot enter another blocked worker’s stack. Armed
timers in a device extension prevent premature device retirement. Explicit
IRQL raises/restores use the actual x64 WDK imports, pair on one execution and
update guest CR8; arbitrary transitions outside those pairs remain unsupported.

`KernelDeviceStackTests.cpp` checks independent ownership/attachment graphs, top selection, invalid-operation atomicity, stack capacity, opaque fields, open-handle counts, work-item/request retention across detach/delete and named-file versus dispatch-top identity. The original `driver_wdm_stack.c` uses genuine WDK headers and inline Copy/Skip/SetCompletion helpers; configure optional `NEVERD_WDM_STACK_FIXTURE` / `NEVERD_WDM_STACK_CFG_FIXTURE` paths for normal/active-CFG images. `DriverWDMStackTests.cpp` exercises rebased execution, exact lower status, completion ordering and flags, delayed pending propagation, worker/DPC completion, waits, `STATUS_MORE_PROCESSING_REQUIRED`, direct-MDL retention, nested completion and malformed cursors/control. `DriverScenarioPublicTests.cpp` covers C API/CLI forwarding and C API retained/nested completion, including configured CFG images. Missing artifacts skip visibly. These Linux-only checks establish the same-driver stack subset, not PDO/PnP/power support. `KernelIRPStackTests.cpp` checks counted cursors, full inline Copy prefixes, consumed-slot clearing, status/pending propagation, MPR and nested completion, continuation-owner checks and retained routes. Genuine READ/WRITE and file-lifecycle coverage also uses inline Copy.

`DriverPnpScenarioTests.cpp` covers strict JSON/native parity, explicit initial facts, ID/count limits, forbidden field combinations, final bus statuses and nullable observed reports. `KernelPnpDeviceTests.cpp`, `KernelPnpRequestTests.cpp` and `KernelPnpCompletionTests.cpp` cover provider ownership, AddDevice success/failure/leaks, initial IRPs, file admission, lifecycle rollback, delayed completion, MPR/nested/waiting continuations and failure atomicity. The original genuine-WDK `driver_wdm_pnp.c` uses optional `NEVERD_WDM_PNP_FIXTURE` / `NEVERD_WDM_PNP_CFG_FIXTURE` paths. `DriverWDMPnpTests.cpp` exercises normal/active-CFG rebased AddDevice, file I/O, orderly removal, delayed start/removal, failed start/query and clean/leaking AddDevice failure. Missing artifacts skip explicitly. Execution evidence is Linux-only and establishes only the documented resource-free PnP subset. `DriverScenarioPublicTests.cpp` also exercises seven-request delayed PnP reports through the C API and CLI with normal/active-CFG images.

V9 schema tests round-trip all eight minor spellings and share final-status validation with lifecycle completion; QueryStop 0x119 is rejected before image loading. Expanded model and genuine fixture checks cover query-stop rollback, cancel-stop, stop/restart, surprise removal, exact-success failures, software I/O while stopped or remove-pending, guest rejection after surprise removal, device identity and mixed AddDevice outcomes. `DriverScenarioPublicTests.cpp` runs a 16-request stop/restart/surprise sequence through both C API and CLI with normal/active-CFG fixtures, preserving successful software IOCTL bytes, the guest's failed surprise-removal IOCTL and final cleanup/close/remove. Public execution is serial: a held IRP without a currently available producer cannot wait for a later scenario request to start or clean up the device. Remove-drain restrictions are profile boundaries, not a general Windows I/O admission policy. Evidence remains Linux-only.

`KernelRemoveLocksTests.cpp` checks independent lock/device identity, NULL/repeated Tags, exact retail/DBG sizes, immediate and delayed drain, failed-acquire obligations, failure atomicity, capacity and storage retirement. `KernelRemoveLockBridgeTests.cpp` and `DriverWDMRemoveLockTests.cpp` cover pre-attachment initialization, opaque extension storage, IRQL boundaries, release after packet retirement, provider completion after lock drain, final-release readiness before callback return, waiting workers and clean AddDevice failure. The original genuine-WDK `driver_wdm_remove_lock.c` is built in retail/DBG and normal/active-CFG forms through optional `NEVERD_WDM_REMOVE_LOCK_FIXTURE`, `NEVERD_WDM_REMOVE_LOCK_CFG_FIXTURE`, `NEVERD_WDM_REMOVE_LOCK_DBG_FIXTURE` and `NEVERD_WDM_REMOVE_LOCK_DBG_CFG_FIXTURE` paths. Public C API/CLI tests use the existing PnP schema and preserve distinct bus receipt/completion and final teardown observations. Missing artifacts skip explicitly; execution evidence is Linux-only and does not establish full Driver Verifier or general concurrent request draining.

`DriverResourceScenarioTests.cpp` checks explicit JSON/native facts, integer widths, counts, physical/register overlap, alignment, IDs, empty banks and configuration serialization. `KernelMMIOTests.cpp`, `KernelMMIOFailureTests.cpp`, `KernelResourceBridgeTests.cpp` and `UnicornMMIOTests.cpp` cover bank/mapping ownership, aliases, epochs, packed-list lifetime, provider timing, restart persistence, surprise/power accessibility, exact CPU/API transactions and failure atomicity. The original genuine-WDK `driver_wdm_resources.c` uses `NEVERD_WDM_RESOURCE_FIXTURE` / `NEVERD_WDM_RESOURCE_CFG_FIXTURE`; `DriverWDMResourceTests.cpp` executes real scalar and REP accessors, normal/active-CFG rebasing, subrange aliases, page-tail mappings, STOP/restart and invalid accesses. C API/CLI tests reject invalid facts before image loading and execute the same 14-request restart scenario with persistent IOCTL output and exact map/unmap counts. The shared [driver-register-bank-scenario.json](examples/driver-register-bank-scenario.json) needs this fixture's register/IOCTL protocol. Missing artifacts skip explicitly, and evidence is Linux-only; no host physical memory or general device backend is exercised.

`DriverInterruptScenarioTests.cpp` covers explicit raw/translated descriptors, mixed and interrupt-only assignments, strict event fields/counts, source identity and independent BOOLEAN observations. `KernelInterruptsTests.cpp`, `KernelInterruptBridgeTests.cpp` and `SchedulerInterruptTests.cpp` cover exclusive tuple matching, opaque tokens, epoch/connection capture, event lifetime, exact selected Ex fields, shared lock/IRQL restoration, callback ownership, same-time ISR priority and capacity failure before mutation. `KernelFrameworkRequestTests.cpp` checks pure cancellation previews and batch token capacity without publishing calls or consuming references. The original genuine-WDK `driver_wdm_interrupts.c` uses `NEVERD_WDM_INTERRUPT_FIXTURE` / `NEVERD_WDM_INTERRUPT_CFG_FIXTURE`; `DriverWDMInterruptTests.cpp` exercises normal/active-CFG rebasing, the legacy eleven-argument ABI, Ex versions 1/2/4, actual ISR→DPC completion, low-AL FALSE, synchronization/manual locks, independent PDOs, restart epochs and invalid hardware facts. C API/CLI tests reject invalid declarations before image loading and execute the seven-request [driver-interrupt-scenario.json](examples/driver-interrupt-scenario.json), checking the pending IOCTL bytes and separate delivery observations. Missing images skip explicitly; execution evidence remains Linux-only and does not establish shared/level/MSI interrupts or instruction-level preemption.

`DriverDMAScenarioTests.cpp` validates explicit capabilities, logical domains, byte/count/time limits, strict event directions and separate configuration/observations. `KernelPhysicalMemoryTests.cpp` and `BackendBackingTests.cpp` check shared-page allocation boundaries, pins, unchanged CPU permissions, MMIO/reentry exclusion and whole-span failure atomicity; `KernelRequestMDLTests.cpp` checks built descriptor aliases against the same physical identities. `KernelDMATests.cpp`, `KernelDMABridgeTests.cpp` and `SchedulerDMATests.cpp` exercise actual RAM bytes, adapter-bound table calls, inline/queued FIFO ownership, separate callback/map lifetimes, page fragments, wrong directions, release preflight, independent PDO domains and epoch/power failures. The original genuine-WDK `driver_wdm_dma.c` uses `NEVERD_WDM_DMA_FIXTURE` / `NEVERD_WDM_DMA_CFG_FIXTURE`; `DriverWDMDMATests.cpp` and C API/CLI coverage execute real adapter pointers, common/SG storage and separately configured DMA/interrupt events. The shared [driver-dma-scenario.json](examples/driver-dma-scenario.json) requires that fixture's protocol. Missing artifacts skip explicitly; execution evidence is Linux-only and does not establish real host DMA, PCI or a general device engine. `pluginsdk/python/tests/test_driver_dma_integration.py` exercises the existing owned JSON binding with `NEVERD_TEST_LIBNEVERD`, `NEVERD_TEST_WDM_DMA_FIXTURE` and `NEVERD_TEST_WDM_DMA_CFG_FIXTURE`, including bytes, callback order and reported failures.

`KernelSEHTests.cpp` checks pure unwind plans, scope order, nonvolatile GPR restoration, bounded stacks and explicit unsupported metadata; `KernelExceptionTests.cpp` checks exact API arity, low-32-bit statuses, typed exceptions, IRQL limits and unchanged model/CPU state. The genuine-WDK `/GS-` `driver_wdm_seh.c` uses optional `NEVERD_WDM_SEH_FIXTURE` / `NEVERD_WDM_SEH_CFG_FIXTURE`; `DriverWDMSEHTests.cpp` runs normal/active-CFG/rebased images through direct and helper raises, nested handlers, rethrows, unhandled exceptions and explicit filter/finally/CPU-fault rejection. C API/CLI executes [driver-seh-scenario.json](examples/driver-seh-scenario.json) and verifies null API results with actual guest handler messages. `pluginsdk/python/tests/test_driver_seh_integration.py` uses `NEVERD_TEST_LIBNEVERD`, `NEVERD_TEST_WDM_SEH_FIXTURE` and `NEVERD_TEST_WDM_SEH_CFG_FIXTURE`. Missing external images skip explicitly; evidence remains Linux-only and does not establish general SEH support.

`KernelRequestOwnershipTests.cpp` checks the distinct `METHOD_NEITHER` input/output pointers, zero-length and failing probes, actual write protection, independent user MDL pins and aliases, completion bytes and failure isolation. It also rejects raw user pointers without caller context while permitting a previously locked kernel alias. `BackendFaultTests.cpp` checks in-context recoverable user faults while preserving terminal kernel faults; `BackendBackingTests.cpp` checks that user and kernel aliases share one RAM authority. The original genuine-WDK `driver_wdm_neither.c` uses optional `NEVERD_WDM_NEITHER_FIXTURE` / `NEVERD_WDM_NEITHER_CFG_FIXTURE`; `DriverWDMNeitherTests.cpp` executes six successful paths and four selected page-protection failures in normal/active-CFG images at preferred/rebased addresses. C API/CLI and `pluginsdk/python/tests/test_driver_neither_integration.py` run [driver-neither-scenario.json](examples/driver-neither-scenario.json), verify plain and locked-alias output bytes, and observe guest access violations from configured `no_access` input or `read_only` output. Python takes `NEVERD_TEST_LIBNEVERD`, `NEVERD_TEST_WDM_NEITHER_FIXTURE` and optional `NEVERD_TEST_WDM_NEITHER_CFG_FIXTURE`. Missing images skip explicitly; evidence remains Linux-only and does not establish arbitrary process address spaces.

The same genuine-WDK fixture acquires an executive spin lock through the actual `KeAcquireSpinLock` macro, checks `DISPATCH_LEVEL`, observes a failed try-acquire while held, releases it, and checks the original IRQL. Native normal/active-CFG and preferred/rebased runs, C API/CLI, and Python cover that path. `KernelRequestOwnershipTests.cpp` checks resident storage, exact ownership and release variant, saved IRQL, repeated acquisition, and rejected return/free while held. This single-CPU cooperative model stops on contended blocking acquisition; it does not model cross-CPU progress or in-stack queued locks.

The WDK fixture also initializes a `KSEMAPHORE` at count zero, releases two units, consumes them through two actual zero-timeout waits, confirms a third timeout, and releases up to its limit. `KernelDispatcherTests.cpp` checks count and limit validation, one-unit wait consumption, priority/Wait restrictions, IRQL limits and a typed `STATUS_SEMAPHORE_LIMIT_EXCEEDED` exception without count mutation. Native normal/active-CFG and preferred/rebased, C API/CLI and Python paths exercise the same count transitions.

The WDK fixture's inline `KeRaiseIrqlToDpcLevel`, `KeRaiseIrqlToSynchLevel` and `KeRaiseIrql` use actual `KfRaiseIrql` / `KeLowerIrql` imports. It observes CR8 through `KeGetCurrentIrql` after nested DISPATCH, APC and synchronization-level transitions in normal/active-CFG preferred/rebased images, C API/CLI and Python. Model tests reject lower without a saved raise, wrong LIFO order, cross-execution restoration, a held spin lock and return with an unmatched raise. No instruction-level interrupt preemption is inferred.

The same genuine WDK fixture uses a resident `KMUTEX` through `KeInitializeMutex`, `KeWaitForMutexObject` (the WDK `KeWaitForSingleObject` macro), `KeReadStateMutex` and `KeReleaseMutex`, checking initial signaled state, recursive acquisition, signed previous-state returns and final release. A second mode catches `STATUS_MUTANT_NOT_OWNED` from an unowned release. Native normal/active-CFG preferred/rebased, C API/CLI and Python paths run both modes. Model tests check owner isolation, incorrect IRQL, waiter-frame ownership, a held object's storage and callback-return lifetime, and unsupported `Wait=TRUE` handoff.

The genuine WDK neither fixture also creates a system thread with `PsCreateSystemThread`, references its opaque object with `ObReferenceObjectByHandle`, closes the handle with `ZwClose`, and waits for `PsTerminateSystemThread` to signal it. The thread observes system PID 4, kernel mode and `PASSIVE_LEVEL`; code after termination does not execute. Native normal/active-CFG preferred/rebased, C API/CLI and Python paths run this case. A model test checks that closing the handle does not retire a referenced object, a normal start-routine return is rejected, and waiting/dereferencing retire the object in order. APC delivery and non-system process handles are outside this profile.

The same WDK fixture checks the system thread's initial critical region, then leaves and reenters it; a separate IOCTL nests critical and guarded regions and checks `KeAreApcsDisabled` and `KeAreAllApcsDisabled` before and after each transition and an APC_LEVEL raise. The KMUTEX mode checks implicit normal-APC suppression while held. Native normal/active-CFG preferred/rebased, public C API/CLI and Python cases cover the transitions. Model tests reject unmatched leaves and returning with an open region. These state queries do not establish APC queue or delivery support.

The same genuine-WDK neither fixture also exposes READ/WRITE on a device with neither buffering flag. Native normal/active-CFG and preferred/rebased tests verify actual `IRP.UserBuffer` bytes, no implicit SystemBuffer or MDL, successful caller-context probe/access and catchable read-only/no-access failures. A model test rejects explicit user rights on a buffered READ before guest dispatch. C API/CLI and Python execute the public READ/WRITE scenario and check returned bytes and `configuration.user_page_access` request indices. This does not imply deferred raw pointer access is safe; a worker must use a locked MDL alias.
The independent `driver_direct.c` stream fixture now reads `IRP.UserBuffer` for its neither device; its READ/WRITE test checks byte and count results alongside the genuine-WDK paths.
Additional genuine-WDK pending-worker modes lock both user buffers during dispatch, use only mapped kernel aliases after `STATUS_PENDING`, unlock/free both MDLs, free the work item and complete the IRP. A raw-user-pointer worker stops as `model_error` with a requesting-process diagnostic. Native normal/CFG and preferred/rebased cases plus C API/CLI and Python cover the successful pending route; native and C API/CLI also cover the rejected raw route. WDM cancel cases exercise a scheduled cancellation callback with locked MDLs, completion before a deadline, a wrong saved IRQL, synchronous `IoCancelIrp` with and without a registered routine, and scheduler ordering. Public C/CLI and Python cases exercise the canceled pending IRP and report time. Explicit `user_unmap_after_dispatch` cases revoke the original user VAs before queued work, verify that locked aliases still complete or cancel, that raw pointer access faults, and that successful completion has no caller-visible output buffer after unmapping.
The same genuine WDK worker now compares `IoGetRequestorProcessId` in dispatch and deferred work, and checks that `PsGetCurrentProcessId` changes to the modeled system worker thread. Unit tests cover explicit requestor IDs, cross-process VA protection, process-exit revocation, retained MDL aliases, and rejection of new I/O from an exited identity. Native normal/CFG and rebased, C API/CLI and Python cases verify successful completion or cancellation after the bounded exit event; cleanup/close remain explicit scenario requests rather than inferred handle rundown.

`KernelRequestOwnershipTests.cpp` also checks process-object identity, opaque storage, attachment-only user VA access across two nested process contexts, exact APC-state pairing, return and wait rejection while attached, and restoration to the system process. The genuine WDK neither fixture executes `IoGetRequestorProcess`, `KeStackAttachProcess`, `IoGetCurrentProcess`, `PsGetProcessId` and `KeUnstackDetachProcess` in a work item through normal/active-CFG and preferred/rebased images, including inaccessible pages and exited requestors. Public C API/CLI and Python scenarios verify the returned bytes. `PsGetCurrentProcessId` continues to identify the work item's creator, PID 4, while the attached process is read from its APC state.

`KernelDMAChannelTests.cpp`, `KernelDMAChannelBridgeTests.cpp` and the shared `SchedulerDMATests.cpp` check mixed allocation FIFO, callback return widths, pure admission/release checks, register reuse, contiguous page fragments, whole-operation flush, CurrentIrp snapshots and packet/MDL/device lifetimes. The original genuine-WDK `driver_wdm_dma_channel.c` uses optional `NEVERD_WDM_DMA_CHANNEL_FIXTURE` / `NEVERD_WDM_DMA_CHANNEL_CFG_FIXTURE`; `DriverWDMDMAChannelTests.cpp` executes normal/active-CFG/rebased drivers with actual MapTransfer and FlushAdapterBuffers calls, shared common/SG/channel quota, explicit device transactions, IRQ/DPC completion, sequential operations, two PDOs and failure cases. C API/CLI runs the seven-request [driver-dma-channel-scenario.json](examples/driver-dma-channel-scenario.json), including a single transaction spanning both mapped fragments. `pluginsdk/python/tests/test_driver_dma_channel_integration.py` uses `NEVERD_TEST_LIBNEVERD`, `NEVERD_TEST_WDM_DMA_CHANNEL_FIXTURE` and `NEVERD_TEST_WDM_DMA_CHANNEL_CFG_FIXTURE` for the same public JSON interface. Missing artifacts skip explicitly; Linux evidence does not establish system DMA controllers or arbitrary HAL mapping/flush patterns.

`DriverPowerScenarioTests.cpp` verifies strict power packet facts, JSON/native parity, opaque 32-bit context, response FIFO limits and independent child reports. `KernelPowerRequestTests.cpp` and `KernelPowerCompletionTests.cpp` check real packet layout, route flags, lifecycle versus per-object notification, FIFO matching, terminal callback ownership, MPR, waits and release boundaries. The original genuine-WDK `driver_wdm_power.c` uses optional `NEVERD_WDM_POWER_FIXTURE` / `NEVERD_WDM_POWER_CFG_FIXTURE` paths. `DriverWDMPowerTests.cpp` covers normal/active-CFG rebasing, direct and nested Query/Set, delayed independent completion, S0-before-D0 ordering, five-argument callback snapshots across waits, worker-originated children, null callbacks, query rejection, independent PDO seeds/FIFOs and explicit missing-fact failures. `DriverScenarioPublicTests.cpp` adds malformed-power preflight and a six-scenario/three-child sleep/wake sequence through both C API and CLI. Missing genuine artifacts skip explicitly; execution evidence remains Linux-only and establishes only the documented pageable resource-free power subset.

`DriverGuardTests.cpp` and four original `driver_guard.c` variants cover active/inactive CFG, rebasing, check/dispatch ABI and malformed targets. `KernelFrameworkTests.cpp`, `KernelFrameworkControlTests.cpp`, `KernelFrameworkQueueTests.cpp` and `KernelFrameworkRequestTests.cpp` cover bindings, transactional device creation, queue routing, logical buffer lengths and cleanup/IRP/context lifetimes. The original `driver_kmdf_lifecycle.c` and `driver_kmdf_control.c` are optionally compiled against genuine WDK 1.33 headers and linked through the real `FxDriverEntry` library. Set `NEVERD_KMDF_FIXTURE` / `NEVERD_KMDF_CFG_FIXTURE` for lifecycle images and `NEVERD_KMDF_CONTROL_FIXTURE` / `NEVERD_KMDF_CONTROL_CFG_FIXTURE` for normal/active-CFG control-device images. Missing external artifacts are explicitly skipped. `DriverKMDFLifecycleTests.cpp`, `DriverKMDFControlTests.cpp` and C API/CLI cases in `DriverScenarioPublicTests.cpp` cover actual callbacks, buffered/direct I/O, pending worker completion, failure status, unload and rebased CFG execution. `DriverKMDFControlTests.cpp` also runs genuine-WDK buffered/direct/neither caller-context callbacks in normal/active-CFG images at preferred/rebased bases, verifying explicit enqueue before sequential queue delivery, completion within caller context, rejection of a callback that returns without either action, request-owned WDFMEMORY aliases for neither IOCTL/READ/WRITE, and no-access/read-only probe failures; `DriverScenarioPublicTests.cpp` executes positive caller-context and neither paths through the C API. KMDF cancellation coverage includes strict transfer-only virtual deadlines and report fields, completion-first and already-canceled paths, mark/unmark outcomes, queued-versus-delivered completion ownership, callback waits and internal-reference lifetimes. Scheduler tests independently verify DPC/cancellation/worker order, capacity, identity separation and suspend/resume. WDM cancellation has the separate genuine-WDK cases above. Evidence remains Linux-only and does not establish full KMDF or PnP/power support.

Legacy cancellation tests retain the API continuation across cancellation, nested cleanup and final destruction; Ex still returns cancellation without callback delivery for an already canceled request. `KernelFrameworkRequestAccessorTests.cpp` and `KernelRequestMDLTests.cpp` check shared 64-bit Information, completion-time length validation, originating queue/IRP identity, NULL WDF file handles, retained-handle getter results, buffered MDL caching and first-direction ByteCount, direct descriptor identity and deferred mapping, completion retirement and rejected WDM-completion bypass. Genuine control-fixture modes L, M, D and C exercise legacy cancellation, buffered MDL/information, direct READ/WRITE MDLs and accessors after completion in normal/active-CFG images.

## Test layout

`add_neverd_unittest` creates one GoogleTest executable and assigns every
discovered case a CTest label equal to that executable's target name.

| Source area | Target and CTest label | What it covers |
|-------------|-------------------------|----------------|
| `unittests/TestProcessTests.cpp` | `NeverDTestProcessTests` | Cross-platform child-process invocation, quoting, redirects, and exit codes |
| `unittests/libc` | `NeverDLibCTests` | Known libc names and classification |
| `unittests/safety` | `NeverDSafetyTests`, `NeverDSafetyIntegrationTests` | Sink catalog, identity precedence, argument prefilter, copy-overflow hunt, heap-lifetime audit, and the mandatory six-cell PE/ELF/Mach-O × x86-64/AArch64 matrix |
| `unittests/lift` | `NeverDLiftTests` | Decoder/lifter LowIR shapes, IR stages, loaders, relocations, format fixtures, decompilation, and representative patch flows |
| Most files in `unittests/semantic` | `NeverDSemanticTests` | Instruction, ABI, control-flow, C-expression, and lift/recompile differential semantics |
| `unittests/evm` | `NeverDEVMOpcodeTests`, `NeverDEVMBytecodeTests`, `NeverDEVMLoaderTests`, `NeverDEVMABITests`, `NeverDEVMAnalyzerTests`, `NeverDEVMDecoderPropertyTests`, `NeverDEVMProxyTests`, `NeverDEVMCallTests`, `NeverDEVMSemanticTests`, `NeverDEVMEmitterTests`, `NeverDEVMIntegrationTests` | Hardfork metadata, input normalization, ABI and signature ambiguity, CFG/SSA/recovery, exhaustive decoder boundaries and hostile inputs, proxy/call facts, interpreter semantics, LLVM/C/Solidity differential execution, and public API routing |
| `unittests/sbf` | `NeverDSBFMetadataTests`, `NeverDSBFProgramImageTests`, `NeverDSBFLoaderTests`, `NeverDSBFAnalyzerTests`, `NeverDSBFVerifierTests`, `NeverDSBFISAConformanceTests`, `NeverDSBFAgaveConformanceTests`, `NeverDSBFSemanticTests`, `NeverDSBFEmitterTests`, `NeverDSBFLLVMEmitterTests`, `NeverDSBFLLVMDifferentialTests`, `NeverDSBFSourceDifferentialTests`, `NeverDSBFMalformedCorpusTests`, `NeverDSBFUpstreamConformanceTests`, `NeverDSBFExternalOracleTests`, `NeverDSBFSolanaModelTests`, `NeverDSBFIntegrationTests` | v0-v4 metadata/layouts, strict verifier and loader behavior, 23 pinned ELF artifacts, official-process oracle, exhaustive opcode availability, hostile inputs, CFG/recovery, and executed LLVM/C/Rust differential behavior |
| `unittests/plugin` | `NeverDPluginRuntimeTests`, `NeverDPythonRuntimeTests`, `NeverDPluginTests`, `NeverDPythonPluginTests` | Native/Python loading, metadata, duplicates, lifecycle, GIL handoff, stale sessions, tracebacks, mixed discovery, and public C routing |
| `PatchFullSubstRTTests.cpp` | `NeverDPatchFullTests` | Rewrite/obfuscation equivalence across four ISAs and three object formats |
| Focused transform files in `unittests/semantic` | `NeverDSwitchXformTests`, `NeverDIndCallXformTests`, `NeverDCFGLoopXformTests`, `NeverDTwoTableXformTests`, `NeverDAvxUpperXformTests` | Small, fast-to-relink probes split out of the large semantic binary |
| `unittests/corpus` (submodule) | `NeverDWindowsEHCorpusTests`, `NeverDRustEHCorpusTests`, `NeverDGoEHCorpusTests`, `NeverDCxxItaniumEHCorpusTests`, `NeverDObjCEHCorpusTests` | Exception and runtime metadata read out of 317 pinned real binaries, each declared in a manifest with the floors its recovery must clear |

The source of truth for registration is
[`unittests/CMakeLists.txt`](../unittests/CMakeLists.txt),
[`unittests/lift/CMakeLists.txt`](../unittests/lift/CMakeLists.txt), and
[`unittests/semantic/CMakeLists.txt`](../unittests/semantic/CMakeLists.txt),
[`unittests/evm/CMakeLists.txt`](../unittests/evm/CMakeLists.txt),
[`unittests/sbf/CMakeLists.txt`](../unittests/sbf/CMakeLists.txt),
[`unittests/plugin/CMakeLists.txt`](../unittests/plugin/CMakeLists.txt), and
[`unittests/safety/CMakeLists.txt`](../unittests/safety/CMakeLists.txt).

### The pinned binary corpus

Every other suite builds what it tests. The corpus does not: it is a submodule
of binaries that real toolchains produced, on hosts and for targets this
repository cannot reach, and each one is pinned by digest beside a manifest
stating the floors its recovery has to clear. That is the only place a claim
about what NeverD reads out of, say, a `-O2` stripped `armv7` shared object is
answerable rather than argued.

The suites are built only when the configure step is told to look for them, so
the flag is what keeps them under test:

```bash
cmake -S . -B build-corpus -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_BINARY_CORPUS_TESTS=ON
cmake --build build-corpus --target check-neverd-corpus --parallel 4
```

`check-neverd-corpus` runs every line; `check-neverd-windows-eh-corpus`,
`check-neverd-rust-eh-corpus`, `check-neverd-go-eh-corpus`,
`check-neverd-cxx-itanium-eh-corpus`, and `check-neverd-objc-eh-corpus` run one
each. All three CI hosts configure with the flag and run all five lines: the
bytes are identical everywhere, but what reads them is not, and a corpus run on
one host proves nothing about the other two.
`scripts/audit_ci_test_inventory.py` refuses an inventory that is missing any of
the five labels, because a build that quietly stopped reading the corpus is a
regression no test can catch — the test is what went missing.

The EVM opcode audit always runs `git fetch` against the official
`https://github.com/ethereum/go-ethereum.git` default branch's remote `HEAD`
with `--depth=1 --force`, resolves and reports the exact SHA, and probes that
object in a detached temporary worktree. Each run uses an unpredictable
private temporary bare repository,
holds the fetched authority ref and its resolved exact SHA through the detached
worktree lifetime, and then destroys the repository and worktree together.
There is no shared persistent Git repository or cache. A
`local_docs` checkout, existing source tree, or submodule is never an audit
path; a pinned submodule would go stale instead of detecting live drift:

```bash
python3 scripts/audit_evm_opcode_metadata.py
```

Every Git command first clears all inherited `GIT_*`, including
`GIT_CONFIG_*`, then installs only audited settings. `GIT_CONFIG_NOSYSTEM`
and `GIT_CONFIG_GLOBAL` disable system/global configuration;
`GIT_ATTR_NOSYSTEM` and command-scoped `core.attributesFile` disable
system/global attributes, and `core.hooksPath` disables hooks. Unexpected
private-repository configuration, grafts,
`objects/info/alternates`, and `refs/replace` fail validation, while
`GIT_NO_REPLACE_OBJECTS` disables replacement lookup.

CI runs the same live audit for pushes to `dev`, pull requests, manual
dispatch, and once per day, so upstream drift is detected even when NeverD does
not change. The public CLI exposes only `--manifest-output`; it cannot select a
remote, ref, checkout, or toolchain. The emitted `schema 3` manifest is closed.
`EVMUpstreamOpcodePolicy.def` owns the closed name-alias plus
historical and unscheduled-EOF exclusion policy. The orthogonal
`EVMUpstreamSemanticsPolicy.def` owns the closed reflected boolean inventory of
`params.Rules`, maps forks, and declares exceptional stack prechecks and
dynamic-immediate families. The Go probe calls `LookupInstructionSet`, scans all
256 byte slots at each mapped fork, and decides allocation from geth's
`operation.undefined`. `HasCost` is only a cost cross-check because defined
zero-cost operations also return false: every `defined && !HasCost` slot must
match `EVM_GETH_ACTIVE_WITHOUT_COST` exactly at its declared activation fork.
An undefined slot with cost, an unreviewed defined slot, or loss of the marker
fails closed. The manifest verifies activation, byte/name identity,
`base_min_stack`, and `net_stack_delta`. Typed historical
and unscheduled-EOF exclusions must satisfy their declared overlap or inactive
invariant; unknown or duplicate schema fields, rules, forks, names, or bytes
fail. Missing, out-of-range, and syntactically unconsumed declarations fail too:
every `.def parser` rejects partial policy input. A failed CI run uploads the
exact geth revision, manifest, and log as an artifact. Parser and drift
diagnostics have independent Python unit coverage:

`EVMUpstreamSemanticsPolicy.def` assigns every exported boolean `params.Rules`
field exactly one `EVM_GETH_RULE_FIELD` category: `MappedForkSelector`,
`NoOpcodeAllocation`, or `ExcludedSelectorExpectedError`. The probe enables
each field alone through `LookupInstructionSet`; the first two categories must
return nil error, the third must return error, and every returned complete
256-slot opcode/stack fingerprint must equal `ExpectedFork`. Current
no-allocation fields `IsEIP155`, `IsEIP2929`, `IsEIP4762`, and `IsPetersburg`
fingerprint as Frontier; `IsUBT` must error and fingerprint as Cancun.

`EVMUpstreamSemanticsPolicy.def` declares the EIP-8024 dynamic opcode families,
operation kinds, and valid stack deltas; `EVMEIP8024Immediates.def` separately
owns immediate decoding and explicitly classifies all 256 bytes in both its
single- and pair-operand inventories. Production uses direct lookup. With
`go -overlay`, the live audit obtains the real private `operation.execute`
handlers and covers the `canonical fork jump tables` plus the
`mainnet active/scheduled jump tables` one table at a time. It records an
`inactive` family explicitly and rejects a `partial` family. Every active table
runs `DUPN`, `SWAPN`, and `EXCHANGE` over every immediate (`3x256`) plus
`3 missing-operand cases`, checking acceptance, PC delta, marker-derived
operands and stack mutation, exact valid underflow, and missing operand `0x00`.
Python compares the observations with the same declarative inputs without
restating the formula.

`EVM_HARDFORK_LATEST` has exactly one canonical target, while the closed
`EVMUpstreamForkAliases.def` maps Prague to Pectra, Osaka and BPO1 through BPO5
to Fusaka, and Paris/Shanghai/Cancun/Amsterdam/Bogota to themselves. Unknown
names fail closed. One recorded `audit_unix_time` drives both
`MainnetChainConfig.LatestFork(time)` (which must equal NeverD latest) and the
`LatestFork(max uint64)` alias/inventory check; both resulting instruction sets
receive a complete table comparison. The manifest fixes
`authority=official-fresh-fetch`, official URL, requested `HEAD`, and resolved
SHA. The probe uses `GOTOOLCHAIN=local`.

The Go request/response and the Python controller enforce
`input/collection/string hard limits` before allocating hostile metadata.
Oversized input, arrays, or strings fail closed. They separately enforce
`bounded diagnostic output`: an overlong display includes a full-content
`digest` and an `explicit truncated marker`. Bounded child output and a shared
deadline cover every command; a timeout or output-limit violation kills the
entire `process group`/process tree and drains its pipes.

The current schema-3 live receipt records `schema_version=3`,
`audit_unix_time=1787534659`, `authority=official-fresh-fetch`,
`remote=https://github.com/ethereum/go-ethereum.git`, `ref=HEAD`, revision
`02b73d4ea7181464175e0a6cbecc0a3a2655a562`, local `Go 1.24.0`,
`stack_limit=1024`, and `diagnostics=[]`. It covers `21 fork tables` and
`20 Rules probes` with `15 mapped/4 no-op/1 expected-error`. Both
`mainnet active/scheduled` records report `upstream BPO2`, closed-mapped to
`NeverD Fusaka`. EIP-8024 has `23 table targets`; only `Amsterdam/Bogota` are
active, yielding `1536 candidate executions` and `6 missing-operand cases`.
The `three handler symbols` agree across the two active targets. Python audit
is `67/67`, and `C++ Opcode 10/10`. The real macOS run succeeded under
`sandbox-exec` with network disabled for the final `go run`; the Linux workflow
mandates `bubblewrap`.

All Go stages—`go env`, `go mod init`, `go mod edit`, `go mod tidy`,
`go mod download`, and `go run`—must pass through the capability-root filesystem
sandbox. Its read capabilities contain only the private probe, fresh geth,
validated `resolved GOROOT`, and exact required system runtime roots; only
isolated environment roots are writable. Network is granted only to dependency
stages that need it and the final run is offline. Tests place sentinels in the
`host HOME/workspace`, require access denial, and require their contents to be
absent from every output. Linux exercises the isomorphic `bubblewrap` policy
without a `/` broad bind.

```bash
python3 -m unittest -v scripts.tests.test_audit_evm_opcode_metadata
```

For EVM control-flow work, run the fixed-point and height-domain contract first:

```bash
cmake --build build --target NeverDEVMAnalyzerTests --parallel 4
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.StackHeightDomain*:EVMAnalyzer.WholeProgram*'
```

These cases cover cross-block internal returns, finite multi-target merges,
loop convergence and deterministic edge ordering, path-dependent whole-stack
lanes, correlation preservation, unknown jumps, exact invalid targets,
fail-loud analysis budgets including `MaxAbstractInstructionTransfers`, and
strict versus relaxed stack faults. Strict rejects unknown or inactive opcodes
only on proven `Reachable` lanes; a
`MayReachable` edge remains a CFG candidate and cannot produce a definite
semantic fact.

All eleven registered EVM test executables are:

```text
NeverDEVMOpcodeTests
NeverDEVMBytecodeTests
NeverDEVMLoaderTests
NeverDEVMABITests
NeverDEVMAnalyzerTests
NeverDEVMDecoderPropertyTests
NeverDEVMProxyTests
NeverDEVMCallTests
NeverDEVMSemanticTests
NeverDEVMEmitterTests
NeverDEVMIntegrationTests
```

Run the complete registered family plus the live upstream audit after CFG,
decoder, ABI, proxy, call, or emitter changes. In particular,
`NeverDEVMDecoderPropertyTests` exhaustively compares complete decoding and
exact `JUMPDEST` boundaries for every two-byte input at each decoder-changing
fork, then exercises deterministic hostile byte strings through every fork
with a bounded input size.

For MedIR/HighIR dataflow changes, also run the constant-phi, selector,
typed-operand, malformed-graph, and deep-chain contracts:

```bash
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.MediumIR*:EVMAnalyzer.HighIR*:EVMAnalyzer.*Selector*:EVMAnalyzer.*MedIR*:EVMAnalyzer.RecoversStorageAndEventFactsFromTypedOperands:EVMAnalyzer.RecoversComputedCalldataArgumentOffset:EVMAnalyzer.*Return*:EVMAnalyzer.*Receive*'
```

These cases prove equal and conflicting cyclic phis, non-adjacent and
cross-block selector expressions, both equality operand orders, exact ABI
width checks, typed storage/event/calldata operands, root-constrained selector /
receive / fallback walks, shared-selector standard ambiguity, per-standard
`KnownFunctionVariantInfo` selection, successful-terminal return-shape
agreement, deterministic malformed MedIR handling, and an iterative
16,384-value producer walk.

Python plugin changes also have an exact C/Python/workflow drift audit:

```bash
PYTHONPATH=pluginsdk/python python3 -m unittest discover \
  -s pluginsdk/python/tests -v
PYTHONPATH=pluginsdk/python python3 -m unittest \
  scripts.tests.test_check_python_plugin_sdk -v
python3 -m mypy --config-file pluginsdk/python/pyproject.toml \
  pluginsdk/python/neverd_plugin
PYTHONPATH=pluginsdk/python python3 scripts/check_python_plugin_sdk.py
```

The first two runtime targets do not depend on the decompiler core and are the
fastest way to isolate loader, CPython, GIL, traceback, and capsule-lifetime
failures. `NeverDPluginTests` and `NeverDPythonPluginTests` then exercise the
same behavior through the exported `libneverd` C API.

## How fixtures are produced

### Lift and format fixtures

`unittests/lift/CMakeLists.txt` cross-compiles C and assembly sources during the
build. Clang target triples produce x86-64, i386, AArch64, and ARM32 ELF
objects, PE/COFF objects and linked images, and PIC/no-PIC Mach-O i386 objects.
When LLD is available, selected objects are also linked into executables for
patch tests. `NeverDLiftTests` depends on the `lift-test-objects` target, so a
normal build of that test binary refreshes its generated fixtures.

Most lift tests use `NeverDLiftFixture.h` to invoke the built `neverd` CLI and
inspect LowIR, MedIR, HighIR, LLVM IR, generated C, or a rewritten binary. The
`NEVERD` environment variable can override the CLI path for a focused manual
experiment; ordinary CTest runs use the executable embedded by CMake.

### Memory-safety fixtures

`unittests/safety/fixtures/binaries` contains checked-in PE, ELF, and Mach-O
images for x86-64 and AArch64, together with the PDB or dSYM companion each
format supplies and a linker MAP for every image. The MAP is what a stripped
build still ships, so each cell is also analysed with the MAP named explicitly,
which pins what a finding may claim once no types and no source lines are left.
`NeverDSafetyIntegrationTests` runs all six cells on every host; configure fails
if any required image or companion is absent, and the suite has no
host-toolchain skip path.

The equivalent binaries come from one source file. Rebuild the host-native
smoke fixture with `make`, or regenerate the complete checked-in matrix with:

```bash
make -C unittests/safety/fixtures matrix
```

The matrix recipe needs Clang's Linux and Windows cross targets, LLD's COFF
tools, both Darwin architectures, and `dsymutil`. Its debug paths are remapped
and CodeView command-line recording is disabled so checked-in companions do not
capture a developer's absolute workspace path.

### Windows exception reconstruction

Windows table-based exception changes need both representation tests and a
linked-PE patch test. The focused lift-suite filter covers the normalized
unwind/SEH/C++ model, corrupt-input handling, exceptional CFG edges, HighIR,
LLVM WinEH generation, exception-directory replacement, and Guard CF/EH
continuation reconstruction:

```bash
cmake --build build --target NeverDLiftTests --parallel 4
build/bin/NeverDLiftTests \
  --gtest_filter='COFFException*:*PatchCOFF_X64.ReconstructsGuardedSEHAndContinuationTable:*PatchCOFF_X64.ReconstructsNativeFH3StateGraph:*PatchCOFF_X64.RejectsInteriorExceptionDirectoryPadding:*PatchCOFF_X64.RebuildsSortedExceptionDirectoryInAppendedSection'
```

The guarded x64 assembly fixture requires Clang's Windows target and
`lld-link`; its CMake link uses `/guard:cf` and `/guard:ehcont`. A skip caused
by a missing cross-linker is not evidence for the final-image path. A passing
integration case proves that the rewritten PE can be reloaded and that its
runtime-function, unwind, load-config, Guard CF, and Guard EH continuation
tables remain sorted, file-backed, and executable-target valid.

The linked FH3 fixture covers the native C++ closure independently: fixed
state tables, HighC annotations, personality preservation, generated catch
targets, and the reloaded IP-to-state graph.

See [Windows Exception Reconstruction](windows-exception-reconstruction.md)
for the analysis/native support matrix and the fail-closed patch contract.

### Language exception models

Everything that is not the Windows table model lives in one focused target.
`NeverDLanguageEHTests` covers the DWARF frame chain, the Itanium
language-specific data area, ARM EHABI, Darwin compact unwind, the Go runtime's
frame metadata, Rust's panic machinery, and the three Objective-C runtimes:

```bash
cmake --build build --target NeverDLanguageEHTests --parallel 4
build/bin/NeverDLanguageEHTests --gtest_filter='ObjC*'
```

The tables in this suite are assembled byte by byte rather than compiled,
because the point of most of them is a combination no single toolchain emits.
Objective-C is the clearest case: all three runtimes emit an Itanium LSDA and
differ only in what a type-table slot holds, and they differ completely rather
than in degree. Apple's slot addresses an `objc_typeinfo` whose first two
fields imitate `std::type_info`, GNUstep's Objective-C++ slot addresses a real
`std::type_info` subclass, and the GNU runtime's slot is not a pointer at all
but the class name string itself. Applying one runtime's convention to
another's table does not fail; it reports a class name read out of the middle
of something else, which is why the runtime is established from the frame's
personality before any slot is read.

The same suite pins two distinctions that are easy to collapse and wrong to.
`@catch(id)` and `@catch(...)` are different handlers — the first takes any
Objective-C object and lets a foreign exception continue past it — and every
runtime spells them differently, so a decoder that reports both as a catch-all
puts a handler on exceptions that would in fact have flown by. And a
setjmp/longjmp call-site table indexes call sites rather than addresses, so a
reader that fails to recognize one of the SJLJ personalities does not error
out; it invents guarded ranges and landing pads the program never named.

Recognizing that form is not the same as refusing it. An SJLJ entry is a pair
of ULEB128 values — a dispatch selector and an action offset — and the action
offset means there what it means in the address form, so the action chain, the
catch types, and the exception specifications all read out of a table that
names no code at all. Only the region each entry guards stays unknown, because
the function's own stores into its call-site slot are what say it. The suite
also pins the byte that must not be trusted here: GCC writes `DW_EH_PE_uleb128`
as the call-site encoding and LLVM writes `DW_EH_PE_udata4`, both then emit
ULEB128 regardless, and no personality ever reads it — so neither may a
decoder.

Personality identity is pinned alongside, because it decides how every table
above is read. GNAT spells its routine the three ways GCC spells every front
end's — `_v0`, `_sj0`, `_seh0` — and on Windows registers one symbol while
forwarding to another, so all four spellings have to land on Ada. D is the
mirror image: three compilers, three names for one routine, one set of tables
behind them.

### Unicorn differential roundtrips

The semantic fixture tests behavior rather than textual shape:

1. Write a small C/assembly case or construct LLVM IR.
2. Compile it for the requested target with Clang/LLVM.
3. Execute the original machine code in Unicorn and capture the expected
   return value or other fixture-defined state.
4. Load and lift it through NeverD, emit LLVM IR, and compile the result back to
   machine code.
5. Execute the regenerated code with the same ABI, inputs, memory layout, and
   CPU model.
6. Compare the observable results.

The main implementation is
[`SemanticRoundTripFixture.h`](../unittests/semantic/SemanticRoundTripFixture.h).
The patch-full fixture uses `Codegen::compileForRewrite`, the same rewrite
backend as patch operations, then compares baseline and transformed code across
the full 4 x 3 ISA/format grid.

A deterministic NeverD semantic failure should be a failed test. Reserve skips
for an explicit external capability boundary, and read the skip reason: a green
summary with a missing cross-linker does not prove that format path ran.

### EVM differential backends

EVM interpreter tests provide a deterministic 256-bit oracle. The emitter
suite compiles and runs generated LLVM directly, lowers generated C23 through
Clang and executes it against the same host harness, and—when `solc`, `anvil`,
`cast`, and `jq` are installed—deploys a generated Solidity harness to a local
Anvil node. It compares status, storage, and instruction trace counts rather
than relying only on output text. A separate raw-bytecode corpus executes the
pre-Fusaka scalar ALU, calldata/memory copying, overlapping `MCOPY`, Keccak,
and return-data paths directly in Anvil and compares them with the interpreter,
guarding against a lowering mistake shared by all generated backends.

The oracle performs typed stack preflight before any opcode-specific side
effect. `EVMForkSemantics.def` defines byte `0x44` as `DIFFICULTY` before Paris
and `PREVRANDAO` from
Paris, and checks transaction rollback for `REVERT`, faults, step limits, and
`ExecutionFaultKind::ResourceExhausted`. Resource exhaustion that prevents the
entry snapshot is explicitly non-committable through
`HasPersistentStateSnapshot`; it is not reported as an ordinary semantic
success.

### EVM public-boundary and budget regressions

Public-API tests tamper independently with canonical
`Code`/`Fork`/`Instructions`/`JumpDestinations` and with every LowIR table,
range, ID, lane, and edge reference. `execute` must return `llvm::Error` before
instruction lookup, and `lowerToMedIR` must reject the complete malformed or
over-budget LowIR before building indexes or allocating proportional output.
For `lowerToMedIR`, tests enforce option validation, resource validation, and
structure validation before a field-by-field `canonical decode replay` and
before `lowerCanonicalLowToMedIR`. Public HighIR recovery replay-checks external
LowIR/MedIR; `analyze` alone may use `lowerCanonicalLowToMedIR` and
`recoverCanonicalHighIR` for its own canonical IR, avoiding recursive or
duplicate replay while still enforcing every HighIR option/resource budget.
The interpreter then tests exact-boundary and one-past-boundary behavior for
all limits declared by `EVMInterpreterLimits.def`: `MaxSteps` keeps the
dedicated `StepLimit`, while `MaxMemoryBytes`, `MaxTraceEntries`,
`MaxLogEntries`, aggregate `MaxLogDataBytes`, and runtime
`MaxPersistentStateEntries` exhaustion return `ResourceExhausted` and roll back
transactional effects. Oversized initial aggregate `MaxHostReturnDataBytes` or
persistent state is an API error. Initial `MaxCalldataBytes`, aggregate
`MaxHostEnvironmentEntries` across `BlockHashes`, `Balances`, `CodeHashes`,
`ExternalCode`, and `BlobHashes`, and aggregate `MaxExternalCodeBytes` are also
API errors. The `const execute preflight` rejects them before environment,
snapshot, or result copying. Return-data `ArrayRef` views and sorted-table
`lower_bound` lookup are covered without requiring a copied buffer or PC map.

Separate LowIR exact-boundary tests cover the aggregate diagnostic limits
`MaxLowDiagnostics` and `MaxLowDiagnosticBytes`: linear decode and CFG
construction both precharge exact count/final bytes, and zero is rejected.
HighIR safety tests exercise the sorted per-lane `Any/Exact/Excluded` domain,
equality match/exclusion, raw `XOR(selector, constant)` false-edge match and
true-edge mismatch, zero-word/calldata-size/call-value refinement, and
fail-closed unknown conditions. Their exact-boundary and one-less cases cover
`MaxHighDispatchCandidates`, aggregate `MaxHighRecoveredArguments`,
`MaxHighDiagnostics`, `MaxHighDiagnosticBytes`, `MaxHighReferenceVisits`,
`MaxHighMemoryTransferCells`, and `MaxHighMemoryValueVisits` from
`EVMAnalysisLimits.def`. They require every emitted diagnostic—including the
fixed malformed diagnostic—to charge count and final bytes before allocation.
The LowIR and HighIR diagnostic budgets are therefore tested independently, and construction
of the default root CFG region must charge `MaxHighRegionBlockReferences`
before reserve or block-PC copy.
Function-scope regressions cover both `EQ` and `raw XOR` back-jumps into a
shared dispatcher. They verify that another function cannot contaminate the
recovered `arguments`, `mutability`, `return shape`, or `region`, while shared
bodies and tail calls remain reachable.
External CALL/CREATE results are tested as nondeterministic host outcomes with
both precise CFG edges, preserving ERC-1167 fallback recovery; an unreadable
selector condition remains Unknown and cannot manufacture fallback or function
facts.

Control-flow tests derive `InvalidJumpDestination` from
`EVMLowFaultKinds.def` for an `end-of-code JUMPI`: definitely true with an
invalid target has no successful tail and is a definite fault; definitely false
succeeds; unknown retains the possible successful false path without marking
the whole lane definitely faulting.

ABI tests apply the grammar boundaries from `EVMABIParserLimits.def` and the
public-table cardinality/text boundaries from `EVMABITableLimits.def` at the
exact limit and one beyond. They also reject invalid kind/standard/evidence
enums, mismatched metadata, noncanonical signatures and return lists, shared
independent selectors, dangling or duplicate variants, and a non-word-sized
event-topic `APInt` before indexed selector or sorted topic lookup.

`NeverDEVMOpcodeTests` also enforces the metadata architecture: every assigned
opcode round-trips between byte encodings and typed values, family helpers are
checked at their boundaries, hardfork aliases resolve through the shared
database, and the complete stack-contract and host-argument maxima remain
derived rather than duplicated in backends.

### Solana SBF differential backends

SBF metadata tests validate every version feature, opcode collision boundary,
Murmur3 syscall hash, relocation, syscall source/availability, ELF machine,
register, and VM-address constant. Loader fixtures generate both legacy v0-v2
section layouts and sectionless strict v3/v4 program-header layouts without
vendored binaries. The hostile corpus probes overflowed ELF tables and
segments, overlapping runtime regions, malformed optional metadata, invalid
registers/branches/LDDW continuations, and immediate-domain violations.

`NeverDSBFISAConformanceTests` checks every byte encoding for each v0-v4 version
against an independently audited typed manifest. `NeverDSBFExternalOracleTests`
then compares the activation and boundary decisions with a separately built
official Anza process. `NeverDSBFUpstreamConformanceTests` assigns explicit
outcomes to all 23 ELFs at the pinned Anza revision.
`NeverDSBFSemanticTests` executes verified instruction bytes directly and does
not consume MedIR, so changing or corrupting normalized IR cannot make the
source oracle agree accidentally with a backend. It covers non-monotonic v2
semantics, memory, syscalls, internal call frames, faults, traces, and resource
limits.

The ORC suite executes lifted LLVM against that raw oracle. The source suite
compiles and runs generated C with warnings as errors and Rust with
`-D warnings`; both compare return/fault state, writable-memory hashes, and
syscall traces. Public API tests traverse every IR stage, disassembly, CFG,
metadata, LLVM, C, and Rust from a generated strict SBF ELF.

#### Audited SBF evidence snapshot

The reproducible gate was audited on 2026-08-24 and pins Anza `sbpf`
`2510663bb8d894e8e3094be351e4bb4b604f1f84`, Agave
`ef210d67f2fabeee1730498188fa78854260c679`, and the Solana SDK
`122f32e571ce39face4beffaccea733e37c207fd`. The Firedancer test-vectors corpus
is pinned at `68bb4af40235562e8852fa23d5727e49c2a0b862`. The pinned official ELF
manifest passes 23/23. `NeverDSBFAgaveConformanceTests` authenticates that Git
tree and matches all 1,955 `sol_compat_elf_loader_v1` fixtures (1,399 accepts,
556 rejects), including `entry_pc`, `text_off`, `text_cnt`, `rodata_hash`, and
`calldests_hash` for every accepted ELF. This loader-only gate deliberately
does not run the later instruction verifier. The independent official-process
gate checks 1,411 opcode and verifier-boundary cases through
`SBFOfficialOracleProtocol.def` and
`SBFOfficialVerifierCases.def` and `SBFOfficialExecutionConstants.def`;
`SBFOfficialELFMutations.def` names malformed
ELF dimensions, so no changing malformed-corpus total is frozen here.
Separately, the `41-case strict ELF differential` runs the complete deterministic
strict-v3 mutation table through the official `verify-elf-batch` process and
NeverD; its 41 cases are not included in the 1,411 opcode/verifier total.

The official additional execution matrix is separate: it has exactly 508 active (Version,Opcode)
cases plus 58 boundary cases = 566 exact execution cases. It
does not replace or count toward the 1,411 verifier probes or the 41-case strict
ELF differential.
Linux Release CI obtains the exact source/corpus pins and Rust toolchain with
`--print-pinned-revision`, `--print-test-vectors-revision`, and
`--print-toolchain`, builds the official process, authenticates the sparse
fixture checkout, and exports `NEVERD_SBPF_ORACLE` plus
`NEVERD_AGAVE_CONFORMANCE_ROOT`, making both external gates mandatory.
Ordinary local runs without the explicit oracle/corpus environment variables
still discover these cases and may skip them rather than downloading or
building upstream implicitly.

The default `RuntimeVersionPolicy::ChainProfile` uses `SBF_RUNTIME_VERSION` to
advance a slot-qualified cluster maximum from V0 through the official V1, V2,
and V3 enable-feature activations; the current maximum is V3. Explicit v4
analysis uses `RuntimeVersionPolicy::UpstreamToolchain` against pinned `sbpf`;
it is an offline capability, not a chain activation claim. The current 10 MiB
cap is exactly `10'485'760` bytes. 65,536 is historical provenance/test data
only and is not enforced. Execution faults have stable explicit values in
`SBFFaultCodes.def`; generated-source host status values remain a separate ABI
in `SBFSourceStatuses.def`.

Scale gates cover dependency worklists, per-function ownership, shared tails,
and multi-latch loops with 10,000-scale fixtures without asserting a
machine-specific duration. Runtime feature rows also support an
`RPC activation audit` of cluster account/slot evidence while ordinary tests
remain deterministic and offline.

### Solana SBF sanitizer profile

Use a separate build directory so sanitizer flags cannot contaminate the
normal integrated-LLVM build. The prebuilt NeverD LLVM package has RTTI
disabled, so standalone consumers must add `-fno-rtti` as well as the sanitizer
flags:

```bash
cmake -S . -B build-sbf-asan-ubsan -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DNEVERD_BUILD_PLUGINS=OFF \
  -DNEVERD_ENABLE_PYTHON_PLUGINS=OFF \
  -DLLVM_DIR=/path/to/neverd-llvm/lib/cmake/llvm \
  -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-rtti' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined' \
  -DCMAKE_SHARED_LINKER_FLAGS='-fsanitize=address,undefined'
```

Build and run the focused SBF targets listed below with
`ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:strict_string_checks=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. macOS ASan does not support
LeakSanitizer, hence the explicit `detect_leaks=0`; use a Linux sanitizer shard
for leak coverage. The pinned, revisioned prebuilt package includes the NeverD
LLVM fork's `llvm/MC/BinaryRewrite.h`, so `NeverDSBFIntegrationTests` runs in
the same fail-fast ASan/UBSan profile. Release evidence records named targets
and their results rather than a brittle aggregate case count.

```bash
cmake --build build-sbf-asan-ubsan --parallel 4 --target \
  NeverDSBFMetadataTests NeverDSBFProgramImageTests NeverDSBFLoaderTests \
  NeverDSBFAnalyzerTests NeverDSBFISAConformanceTests \
  NeverDSBFVerifierTests NeverDSBFAgaveConformanceTests \
  NeverDSBFSemanticTests NeverDSBFEmitterTests NeverDSBFLLVMEmitterTests \
  NeverDSBFLLVMDifferentialTests NeverDSBFSourceDifferentialTests \
  NeverDSBFMalformedCorpusTests NeverDSBFUpstreamConformanceTests \
  NeverDSBFSolanaModelTests NeverDSBFIntegrationTests

ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:strict_string_checks=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
NEVERD_SBPF_ROOT=/path/to/sbpf \
NEVERD_AGAVE_CONFORMANCE_ROOT=/path/to/firedancer-test-vectors \
NEVERD_AGAVE_CONFORMANCE_REVISION=68bb4af40235562e8852fa23d5727e49c2a0b862 \
ctest --test-dir build-sbf-asan-ubsan --output-on-failure --parallel 4 \
  -L '^NeverDSBF'
```

## One-shot targets

The custom targets build their dependencies and then run CTest with parallelism
derived from the host CPU:

| CMake target | Selection |
|--------------|-----------|
| `check-neverd` | Every registered test |
| `check-neverd-semantic` | `NeverDSemanticTests` only |
| `check-neverd-sbf` | Every `NeverDSBF*Tests` target/case |
| `check-neverd-patch-full` | `NeverDPatchFullTests` only |
| `check-neverd-switch-xform` | `NeverDSwitchXformTests` only |
| `check-neverd-cfgloop-xform` | `NeverDCFGLoopXformTests` only |
| `check-neverd-twotable-xform` | `NeverDTwoTableXformTests` only |

```bash
cmake --build build-release --target check-neverd
cmake --build build-release --target check-neverd-semantic
cmake --build build-release --target check-neverd-sbf
```

`NeverDIndCallXformTests` and `NeverDAvxUpperXformTests` currently have no
`check-neverd-*` convenience target. Build and select them by label as shown
below. `check-neverd-semantic` also does not include the separate transform or
patch-full binaries; use `check-neverd` for the complete aggregate.

## Incremental CTest workflow

Build the owning executable first, then select its label. This avoids relinking
unrelated large semantic targets.

```bash
# Lifter, loader, and format tests
cmake --build build-release --target NeverDLiftTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDLiftTests$' --output-on-failure --parallel 4

# Main semantic binary
cmake --build build-release --target NeverDSemanticTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDSemanticTests$' --output-on-failure --parallel 4

# A label-only focused transform binary
cmake --build build-release --target NeverDIndCallXformTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDIndCallXformTests$' --output-on-failure --parallel 4

# Every focused EVM target/case
cmake --build build-release --target \
  NeverDEVMOpcodeTests NeverDEVMBytecodeTests NeverDEVMLoaderTests \
  NeverDEVMABITests NeverDEVMAnalyzerTests NeverDEVMDecoderPropertyTests \
  NeverDEVMProxyTests NeverDEVMCallTests NeverDEVMSemanticTests \
  NeverDEVMEmitterTests \
  NeverDEVMIntegrationTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -R 'EVM' --output-on-failure --parallel 4

# Every focused Solana SBF target/case
cmake --build build-release --target check-neverd-sbf --parallel 4
```

Use a GoogleTest-derived CTest name for a single regression:

```bash
ctest --test-dir build-release --build-config Release -N \
  -L '^NeverDLiftTests$'
ctest --test-dir build-release --build-config Release \
  -R '^COFFARMPipeline\.ARM32ThumbLiftAndDecompile$' \
  --output-on-failure
```

Useful selectors:

| Command | Purpose |
|---------|---------|
| `ctest --test-dir build-release -N` | List discovered cases without running them |
| `ctest --test-dir build-release -L '<regex>'` | Select a test-binary label |
| `ctest --test-dir build-release -R '<regex>'` | Select case names |
| `ctest --test-dir build-release --output-on-failure` | Show diagnostics only for failures |
| `ctest --test-dir build-release --stop-on-failure` | Stop after the first failing case |
| `ctest --test-dir build-release --parallel 4` | Run up to four cases concurrently |

GoogleTest discovery uses `DISCOVERY_MODE PRE_TEST`, so the corresponding test
binary must exist before CTest can enumerate it. Per-case timeouts and the
separate discovery timeouts are defined in `cmake/AddNeverD.cmake` and may be
widened only for suites with measured heavy cases.

## Which tests should change with code?

| Change area | Start with | Then consider |
|-------------|------------|---------------|
| Architecture lifter or decode | Named case in `NeverDLiftTests` | Matching ISA semantic roundtrip |
| LowIR CFG, function detection, jump tables | Lift CFG/switch cases | `NeverDSwitchXformTests`, `NeverDCFGLoopXformTests`, or `NeverDTwoTableXformTests` |
| MedIR, ABI, flags, types, SSA | MedIR/calling-convention lift cases | Cross-ISA `NeverDSemanticTests` cases |
| HighIR or structured C | HighIR/decompile cases | `NeverDCFGLoopXformTests` and generated-C compilation checks |
| PE/ELF/Mach-O loader or input relocation | Matching `unittests/lift` format fixture | All-stage load/decompile test for that cell |
| Rewrite codegen or output relocation | `RewriteCodegenRTTests` cases | `NeverDPatchFullTests` and a linked patch fixture where available |
| LLVM IR transform used by patch | Focused transform binary | `NeverDPatchFullTests` composed-pass grid |
| C API or CLI | Direct SDK/query test and `unittests/semantic/CLIEndToEndTests.cpp` | Relevant pipeline/format suite |
| EVM loader, opcode, IR, or backend | Smallest owning `NeverDEVM*Tests` target | All EVM targets plus generated C/Solidity compilation |
| SBF loader, ISA, IR, or backend | Smallest owning `NeverDSBF*Tests` target | All SBF targets plus generated C/Rust compilation |
| Libc recognition | `NeverDLibCTests` | Semantic call/ABI cases if behavior changes |
| Heap-lifetime audit or copy-overflow hunt | `NeverDSafetyTests` | All six cells in `NeverDSafetyIntegrationTests` |
| Process execution or quoting | `NeverDTestProcessTests` | One affected CLI/semantic case on each supported host |

Tests should express the contract at the lowest stable boundary. A LowIR shape
test is useful for lifter attribution; a semantic roundtrip is required when
two plausible IR shapes could behave differently. Avoid golden dumps of whole
functions when a small opcode, CFG, or observable-state assertion is enough.

## CI relationship

CI builds Release with tests enabled on Linux, macOS, and Windows, then audits
the discovered inventory before applying platform-specific label exclusions.
Those profiles are defined in `.github/workflows/ci.yml` and
`scripts/audit_ci_test_inventory.py`. `NeverDSafetyTests` and
`NeverDSafetyIntegrationTests` are required on every matrix host, and every
such run reads the same checked-in PE, ELF, and Mach-O fixtures for both native
architectures. Because no single matrix shard represents every expensive
suite, a local `check-neverd` remains the clearest complete pre-merge signal
when the machine has all required cross tools.

Discovery is not execution evidence. CI writes a CTest JUnit report and the
original CTest exit status, then `scripts/audit_ci_test_results.py` reconciles
every selected `(test name, labels)` identity with its result. Passed, skipped,
disabled, infrastructure-not-run, failed, and missing results remain separate.
In particular, CTest can encode a missing executable as a JUnit `<skipped>`;
that is an execution failure, not an optional-backend skip. A stopped run can
omit unstarted tests entirely, so matching only the XML summary count is not
sufficient either. The audit runs even after CTest fails and preserves that
failure. Each matrix leg uploads its inventory, JUnit, exit status, outcome
JSON, and log as `ctest-evidence-<profile>`.

The required execution policy follows existing CI ownership:

- Linux must execute all selected `NeverDSemanticTests` cases; macOS must
  execute all selected `NeverDPatchFullTests` cases.
- Every host must execute the mandatory safety, native example plugin,
  concolic, binary corpus, and failure-integrity suites. The integrity suites
  cover worker exception transport, SDK session state, exact native LLVM
  function identity and verified caching, the semantic fixture, and pipeline
  outcome publication.
- Linux must also execute the SBF external oracle, upstream conformance, and
  Agave conformance suites because that leg installs their pinned dependencies.
- The only platform exceptions within these required suites are
  `ObjCEHCorpus.HonorsHostMachORewriteContractForEveryVariant` and
  `CxxItaniumEHCorpus.HonorsHostMachORewriteContractForEveryProbeVariant` on
  non-macOS hosts. Both execute native Mach-O probes; their exact identities
  and existing skip reasons are checked. Portable corpus-reading cases remain
  required everywhere.

A missing toolchain still appears as skipped, but a skip in a required suite
fails the CI evidence gate: the promised execution was not obtained. Optional
suite skips, such as an unavailable external Solidity toolchain, stay visible
in the JSON and JUnit artifacts and never contribute to the passed count.
Disabled tests and infrastructure-not-run results are never optional success.

The unit being audited is a **CTest registration**. A passed Python aggregate
runner does not prove that every child unittest executed; the separate
capability-evidence audit remains responsible for its more specific contracts.
Neither report is a claim of complete code coverage.

CI outcome auditing requires CTest 3.28 or newer for JUnit labels. This is a
CI-only tooling requirement; the project's minimum build version is unchanged.
The parser, policy, actual CTest outcomes, and workflow failure handling can be
verified without compiling NeverD:

```bash
python3 scripts/audit_ci_test_results.py check-tool
python3 -m unittest scripts.tests.test_audit_ci_test_inventory \
  scripts.tests.test_audit_ci_test_results scripts.tests.test_ci_configuration -v
```
