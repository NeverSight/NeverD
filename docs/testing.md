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

## Modular MBA simplification

`SymReadability.*` covers subtraction and complement spelling, n-ary operator
cost, one-bit and wide literals, shared-tree saturation, budgeted selection,
and exhaustive three-bit equivalence with sampling disabled. `SymMBASample.*`
compares narrow and arbitrary-width verification against the AP evaluator,
including all operators, deterministic assignments, and unused wide inputs.
Candidate quality
comparisons across scoring revisions must recount both outputs with the same
metric; the SDK's version-specific size counters are diagnostic only.

Build the symbolic engine, LLVM safety guards, HighIR bridge tests, and source
roundtrips together when changing shared bitvector simplification:

```sh
cmake --build build-release --target NeverDSymbolicTests NeverDSolverTests \
  NeverDSymSimplifyGuardTests NeverDLiftTests NeverDMBASourceTests \
  NeverDHighCStoreForwardingTests NeverDMetadataJSONTests --parallel 4
build-release/bin/NeverDSymbolicTests
build-release/bin/NeverDSolverTests
build-release/bin/NeverDSymSimplifyGuardTests
build-release/bin/NeverDLiftTests \
  --gtest_filter='HighSymSimplify.*:HighFrameStoreForwarding.*:ELFARM32ModeTest.*'
build-release/bin/NeverDHighCStoreForwardingTests
build-release/bin/NeverDMBASourceTests
build-release/bin/NeverDMetadataJSONTests --gtest_filter='ELFARM32ModeCAPITest.*'
```

`NeverDMBASourceTests` assembles generic 8/16/32/64-bit arithmetic and shared
Boolean identities as ELF, COFF, and Mach-O. It checks both HighC and LLVMC for
remaining MBA, recompiles their C at `-O0` and `-O2` with undefined-behavior
traps, and compares results with unsigned arithmetic, parity, and memory-store
oracles. A full-product counterexample ensures low-word rules do not discard
observable upper bits. The symbolic and HighIR tests additionally check signed
extension, carry boundaries, shared-DAG traversal, and effect preservation.
Missing cross-target Clang is a skip, not evidence for that format.
Nested source expressions additionally compile at `-O0` for x86-32/64,
ARM32, Thumb-1/2 and AArch64. Both C routes must remove the MBA, recompile at
`-O0` and `-O2`, and match unsigned arithmetic on byte pairs, edge values and
random words. A pipeline guard checks that exact synthetic-frame accesses can
be promoted while a dynamic store that may alias the frame remains observable.
The 32-bit register-pair fixture separately checks that both C routes preserve
an observed 64-bit return through a call on i386 (including PIC call/pop),
ARM32, Thumb-1, and Thumb-2. Its edge and random-word oracles check the complete
64-bit result and a caller that consumes both halves. Three independent
Boolean forms exercise wide addition and subtraction; two more add a nonzero
64-bit offset after the Boolean form. A three-input parity/majority form checks
carry recovery across the two words, with a matching native 64-bit check on
x86-64 and AArch64. A four-input carry-save form checks the same matrix with
independently randomized operands and the corresponding native 64-bit paths.
An independent five-input carry-save form extends the split-word matrix;
Thumb-2 HighC also checks a frame-pointer alias established after earlier
spills. Native x86-64 and AArch64 repeat the five-input check through both C
routes. The output must remove their residual
XOR, AND, and complement operations, while retaining the shifts and OR needed
to assemble input halves. Thumb-1 compilers may place these offsets in a
read-only literal island inside executable code; mapping and relocation
evidence must authorize constant reads in both C routes. The shared symbolic
candidate generator infers signed coefficients from basis responses and uses
mixed samples only to discard candidates. `NeverDSolverTests` checks that an
exact proof accepts equivalent arithmetic and rejects rare counterexamples or
an incomplete proof.

ARM, Thumb-1 and Thumb-2 Mach-O objects additionally run a complete
two-function five-input case through both C routes. The check includes
function-size recovery, a call to the first function at object address zero,
source MBA elimination, and host-recompiled `-O0`/`-O2` execution. Dedicated
MedIR tests cover exact stack-alignment offsets and reject dynamic, ambiguous,
and stronger-than-ABI masks.
One mixed ARM/Thumb Mach-O object checks mode recovery across direct calls in
both directions. Both C routes remove its arithmetic MBA and execute all five
functions against edge and randomized modular-addition inputs at `-O0`/`-O2`.
Loader tests separately require a Thumb symbol at object address zero to retain
its mode and verify every reachable mixed-mode function entry.
An additional Mach-O object has uncalled ARM and Thumb MBA functions whose
first instructions carry mode-specific relocations, without Thumb nlist mode
flags. Both C routes must recover and execute their simplified arithmetic.
Loader tests check that ARM branch, Thumb branch and halfword relocations record
only their exact instruction modes. Homogeneous ARM-only and Thumb-only objects
must keep a uniform mode when relocation evidence supplies the first entry;
contradictory symbol and relocation modes must fail loading.
Thumb-only ARMv6-M, ARMv7-M, ARMv7E-M, ARMv8-M Base/Main, and ARMv8.1-M Main
Mach-O objects have unmarked MBA functions without instruction relocations.
Both C routes must recover their arithmetic and execute it at `-O0`/`-O2`;
loader tests require the subtype to prove Thumb mode, reject an ARM instruction
relocation in a Thumb-only image, and fail loading on malformed subtype
capability bits.

The frame-spill source matrix also covers x86-32 (ELF/COFF/Mach-O), ARM32
(ARM, Thumb-2 and Cortex-M Thumb-1 ELF), and AArch64 (ELF/COFF/Mach-O)
through both C backends. Repeated private-frame reloads must reduce to
addition/subtraction and execute correctly
for all byte pairs, word-boundary pairs and deterministic random words at both
optimization levels. Clang AST checks inspect the complete spill functions for
residual MBA operators, including temporary assignments, while distinguishing
valid address and pointer expressions. `HighFrameStoreForwarding.*` checks exact
access widths,
source-local mutation, memory-home writes, prefix aliases, partial overlaps,
ordered memory, malformed/cyclic graphs and rendered expansion bounds. Its
narrow-complement regression executes after semantic simplification.
`HighCStoreForwarding.RetainsDefinitionsUsedByForwardedValues` keeps the cached
store-value dependencies live across all four architectures, including floating
reinterpretations and additional direct uses. `SymSimplifyGuard.OpaqueLoad*`
checks load identity/order, volatile/atomic state and poison boundaries.
`ELFARM32ModeTest.*` checks authenticated ARM/Thumb selection, normalized
function addresses, mapping-only objects, mixed-image decoding, cross-mode
calls with modular MBA arithmetic, wide Thumb branches, HighC/LLVMC
execution, forwarded argument chains, halfword-aligned interworking targets,
preserved conditional ARM calls, and rejection of contradictory modes or
malformed call relocations. A sectionless and symbolless linked ELF checks both
directions of direct ARM/Thumb calls, conditional Thumb fallthrough, unknown
unreached bytes, and executable HighC/LLVMC output before and after rewriting.
A mapped ELF checks generated executable bytes outside its section table,
Thumb-to-ARM call repair, and both C backends at `-O0` and `-O2`. An aligned
Thumb stack frame passed to an ARM callee checks live pointer preservation in
both C backends before and after rewriting; a MedIR contract test also checks
multi-hop pointer forwarding and rejects a redefined argument register.
`ELFARM32ModeCAPITest.*` checks that mixed-mode SDK disassembly and both C
backends work after replacing a Thumb image in the same session, then reloads
Thumb to verify decoder recovery. `ARM32InterworkingPatchRT.*` links a generic
mixed ARM/Thumb ELF and executes all four entries in Unicorn before and after
both section and in-place rewriting. `InstructionMode.*` covers the decoder,
code-pointer, direct-branch and code-generation boundaries.
`COFFRelocatableAbsoluteRelocation.MachOARM32*` checks ARM32 Mach-O object
relocations, including Thumb BL, B.W and BLX across sections, halfword-aligned
call sites, backward calls, malformed encodings and jumps that need a veneer.

```sh
cmake --build build-release --target NeverDARM32InterworkingTests --parallel 4
build-release/bin/NeverDARM32InterworkingTests
```

## Interpreter recovery checks

```sh
cmake --build build-release --target NeverDInterpreterSpecializationTests \
  NeverDDevirtualizationSourceTests NeverDInterpreterMachineStateTests --parallel 4
ctest --test-dir build-release -L '^NeverD(InterpreterSpecialization|DevirtualizationSource|InterpreterMachineState)Tests$' \
  --output-on-failure
```

Core tests check context splitting, fixed-point joins, dynamic loops, overlapping registers, alias invalidation, finite dispatch, and refusal without a partial replacement. Source tests assemble original register, stack and finite-address x64 machines, recover both C routes, compile at O0/O2 with undefined-behavior traps, and compare execution with independent unsigned arithmetic and memory oracles. Finite-address fixtures exercise input-selected records and related cursor/key controls; native checks cover SysV and Win64 calling conventions. The suite also exercises the public CLI, recovery budgets and unsupported-input reports. Cross-target Clang and LLD are required; original ELF execution additionally requires an x64 Linux host. Missing tools or a nonmatching host are skipped coverage, not a pass.

`VMShapeSourceTests.cpp` adds three original shapes recovered without manual
control hints: direct-threaded pointer bytecode, a bounded software CALL/RET
stack with nested virtual calls, and a loop with rotating opcode-decoder state.
The tests compare native SysV/Win64 execution and both recovered C backends at
O0/O2 against independent mathematical oracles, including returned values,
output stores and canaries. They also assemble ELF and COFF variants. COFF
coverage is assembly-only; the Win64 calling-convention oracle runs as Linux
ELF with `ms_abi`, not as a Windows PE executable. Unknown
virtual return cursors, unconstrained decoder keys and an exhausted recovery
budget must refuse source publication.

`MachineControlSourceTests.cpp` exercises the explicit machine-state ABI with
finite register-indirect native CALL and finite internal RET dispatch. Its
separate native observer compares all 16 general registers, defined RFLAGS and
every byte of the tested guest stack with HighC and LLVMC at O0/O2. Callees read
and overwrite the target register; checks require one actual fallthrough-address
store, preserved flags and restored RSP. Expected code addresses come from ELF
symbols. Unknown targets, finite sets containing a missing or nonexecutable
destination and an unproved target at entry `[rsp]` must fail without source.
These original fixtures have been validated locally on x64 Linux;
their coverage does not establish support for arbitrary virtual machines.

`MachineMemoryCallSourceTests.cpp` adds four zero-hint, default-budget runtime
cases: an immutable RIP-relative pointer slot, an input-selected read-only
two-target table, an initialized guest `[rsp]` slot with two possible values,
and `call *-8(%rsp)` whose target slot is overwritten by the call's push. Native
execution and both recovered C backends at O0/O2 must match the independent
17-word register/flag oracle and every byte of the tested guest stack. The
callee observes the pushed return address; ELF symbols supply expected code
addresses. The oracle also requires a table address left in a register to retain
its original numeric guest value, rather than the address of a generated C
global; the caller supplies any required guest mappings. Unproved external or
writable slots, unknown or alias-clobbered stack values, invalid targets and
default-source-ABI use must refuse publication. Local x64 Linux validation
checks each of the four forms with 1,024 states through native execution, HighC
and LLVMC at O0/O2, comparing all 17 state words and 128 guest-stack bytes.
The lower-level memory-call tests also require canonical unsegmented `r/m64`
forms, optional `addr32` and REX, and rejection of FS/GS, far calls and extra or
noncanonical prefixes. Effective-address calculation and the target LOAD must
precede the native return-address push.

`MachineSourceUsesOriginalWritableMappingsThroughBothRoutes` and the wrapper's
`FixedGuestAddressesRemainNumericThroughLoadsStoresAndState` regression supply
writable guest memory at its original address. Both C backends at O0/O2 must
use that mapping for runtime reads and writes, retain numeric guest addresses
in register outputs and preserve adjacent canaries. Generated global objects
cannot substitute for these mappings. The fixed-mapping runtime coverage is
local to x64 Linux.

The flag-state fixture retains `PUSHFQ` and `POPFQ` across finite indirect
dispatch, then executes recovered HighC and LLVMC at O0/O2. Core negative cases
reject malformed flag intrinsics, snapshots of unbound entry flags, flag-derived
writes without return-slot nonalias evidence, and unbounded flag-derived targets.
They also reject direct reads of unbound flags after algebraic cancellation or
a control-flow join where only one predecessor defines the flag.
Temporary-definition tests require every byte to be written earlier in the
same lifted native instruction, even when an undefined value cancels
algebraically or reuses the previous instruction's temporary offset.

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

Explicit nested user-memory tests cover strict JSON/native graph validation,
shared and cyclic references, unaligned pointer slots, page rights, request and
process revocation, and WDF caller-context ownership. Genuine WDK WDM/KMDF tests
follow two-level structures and interior aliases in normal/active-CFG images at
preferred/rebased addresses; a WDM worker completes through locked aliases after
unmap or requestor exit. C API/CLI and Python run
`docs/examples/driver-nested-user-scenario.json` and inspect `backing_hex` without
mistaking revoked memory for caller-visible output. Backend tests prove terminal
snapshots preserve the original fault and reject MMIO, running CPUs and invalid
spans without destination changes.

`KernelMDLChainTests` and `KernelMDLUserMappingTests` verify partial-MDL
capacity, reuse, physical identity, mapping ownership, process permissions and
failure-atomic retirement. Optional genuine WDK user-view fixtures use
`NEVERD_WDM_USER_MAPPING_FIXTURE` and
`NEVERD_WDM_USER_MAPPING_CFG_FIXTURE`; missing artifacts are explicit skips.
The [user mapping scenario](examples/driver-user-mapping-scenario.json) also
runs through the CLI and Python C API binding. It checks shared bytes, caught
read-only faults, process attachment and mapping ownership. Group-retirement
tests release several relocked views before their request backing and verify
that external pins reject completion without changing IRP or MDL state.

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

`DriverKMDFControl.StopCompletionCallbackRunsBeforeQueueRestart` uses a
genuine WDK control driver to verify that a stop-completion callback receives
its context only after the previously delivered request finishes, without
waiting for a second request still queued. The callback runs before the worker
restarts the queue, and the waiting request is then delivered from Start.
Normal and active-CFG images run at preferred and relocated bases. Model tests
also cover immediate notification on an idle queue, forwarding away the final
delivered request, and rejection of a second pending registration; the C API
exercises the callback path.

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

`KernelDriverIRPAllocationTests.cpp` checks real packet headers, bounded stack counts, quota rejection, exhaustion, exact-base release and independent caller storage. `KernelDriverIRPOwnershipTests.cpp` checks adoption preflight, immutable lower routes, completion ownership, MPR/free ordering, suspended callback protection and independent kernel output snapshots. `DriverWDMOwnedIRPTests.cpp` executes the genuine WDK `driver_wdm_owned_irp.c` with optional `NEVERD_WDM_OWNED_IRP_FIXTURE` / `NEVERD_WDM_OWNED_IRP_CFG_FIXTURE`: unsent allocation, inline free-before-dispatch-return, worker completion, held MPR, cancellation and nested outer completion, with or without the caller stack slot. Normal/active-CFG images run at preferred/rebased addresses. `DriverScenarioTests.cpp` checks kernel-only input rejection and origin/report parity; `DriverScenarioPublic.CAPIAndCLIReportIndependentDriverAllocatedIRP` verifies actual worker completion and independent rows in both public paths. The [caller-owned IRP scenario](examples/driver-owned-irp-scenario.json) uses that fixture and deliberately cancels one child: CLI exit code 2 / `scenario_success: false` are expected with a returned stop reason, clean unload and zero fixture failures. Missing artifacts skip explicitly; execution evidence remains Linux-only.

`DriverPnpTopologyTests.cpp` checks forward-declared parents, independent roots, strict native/JSON graph validation and stable nullable report fields. `KernelPnpTopologyTests.cpp` checks resolved parent PDO identity, independent device stacks, retained relationship observations after failed AddDevice, powered-parent START admission and explicit bottom-up STOP, SurpriseRemoval and REMOVE ordering.

`DriverPnpScenarioTests.cpp` covers strict JSON/native parity, explicit initial facts, ID/count limits, forbidden field combinations, final bus statuses and nullable observed reports. `KernelPnpDeviceTests.cpp`, `KernelPnpRequestTests.cpp` and `KernelPnpCompletionTests.cpp` cover provider ownership, AddDevice success/failure/leaks, initial IRPs, file admission, lifecycle rollback, delayed completion, MPR/nested/waiting continuations and failure atomicity. The original genuine-WDK `driver_wdm_pnp.c` uses optional `NEVERD_WDM_PNP_FIXTURE` / `NEVERD_WDM_PNP_CFG_FIXTURE` paths. `DriverWDMPnpTests.cpp` exercises normal/active-CFG rebased AddDevice, file I/O, orderly removal, delayed start/removal, failed start/query and clean/leaking AddDevice failure. Missing artifacts skip explicitly. Execution evidence is Linux-only and establishes only the documented resource-free PnP subset. `DriverScenarioPublicTests.cpp` also exercises seven-request delayed PnP reports through the C API and CLI with normal/active-CFG images.

V9 schema tests round-trip all eight minor spellings and share final-status validation with lifecycle completion; QueryStop 0x119 is rejected before image loading. Expanded model and genuine fixture checks cover query-stop rollback, cancel-stop, stop/restart, surprise removal, exact-success failures, software I/O while stopped or remove-pending, guest rejection after surprise removal, device identity and mixed AddDevice outcomes. `DriverScenarioPublicTests.cpp` runs a 16-request stop/restart/surprise sequence through both C API and CLI with normal/active-CFG fixtures, preserving successful software IOCTL bytes, the guest's failed surprise-removal IOCTL and final cleanup/close/remove. Public execution is serial: a held IRP without a currently available producer cannot wait for a later scenario request to start or clean up the device. Remove-drain restrictions are profile boundaries, not a general Windows I/O admission policy. Evidence remains Linux-only.

`KernelRemoveLocksTests.cpp` checks independent lock/device identity, NULL/repeated Tags, exact retail/DBG sizes, immediate and delayed drain, failed-acquire obligations, failure atomicity, capacity and storage retirement. `KernelRemoveLockBridgeTests.cpp` and `DriverWDMRemoveLockTests.cpp` cover pre-attachment initialization, opaque extension storage, IRQL boundaries, release after packet retirement, provider completion after lock drain, final-release readiness before callback return, waiting workers and clean AddDevice failure. The original genuine-WDK `driver_wdm_remove_lock.c` is built in retail/DBG and normal/active-CFG forms through optional `NEVERD_WDM_REMOVE_LOCK_FIXTURE`, `NEVERD_WDM_REMOVE_LOCK_CFG_FIXTURE`, `NEVERD_WDM_REMOVE_LOCK_DBG_FIXTURE` and `NEVERD_WDM_REMOVE_LOCK_DBG_CFG_FIXTURE` paths. Public C API/CLI tests use the existing PnP schema and preserve distinct bus receipt/completion and final teardown observations. Missing artifacts skip explicitly; execution evidence is Linux-only and does not establish full Driver Verifier or general concurrent request draining.

`DriverResourceScenarioTests.cpp` checks explicit JSON/native facts, integer widths, counts, physical/register overlap, alignment, IDs, empty banks and configuration serialization. `KernelMMIOTests.cpp`, `KernelMMIOFailureTests.cpp`, `KernelResourceBridgeTests.cpp` and `UnicornMMIOTests.cpp` cover bank/mapping ownership, aliases, epochs, packed-list lifetime, provider timing, restart persistence, surprise/power accessibility, exact CPU/API transactions and failure atomicity. The original genuine-WDK `driver_wdm_resources.c` uses `NEVERD_WDM_RESOURCE_FIXTURE` / `NEVERD_WDM_RESOURCE_CFG_FIXTURE`; `DriverWDMResourceTests.cpp` executes real scalar and REP accessors, normal/active-CFG rebasing, subrange aliases, page-tail mappings, STOP/restart and invalid accesses. C API/CLI tests reject invalid facts before image loading and execute the same 14-request restart scenario with persistent IOCTL output and exact map/unmap counts. The shared [driver-register-bank-scenario.json](examples/driver-register-bank-scenario.json) needs this fixture's register/IOCTL protocol. Missing artifacts skip explicitly, and evidence is Linux-only; no host physical memory or general device backend is exercised.

`DriverInterruptScenarioTests.cpp` covers explicit raw/translated descriptors, mixed and interrupt-only assignments, strict event fields/counts, source identity and independent BOOLEAN observations. `KernelInterruptsTests.cpp`, `KernelInterruptBridgeTests.cpp` and `SchedulerInterruptTests.cpp` cover exclusive tuple matching, opaque tokens, epoch/connection capture, event lifetime, exact selected Ex fields, shared lock/IRQL restoration, callback ownership, same-time ISR priority and capacity failure before mutation. `KernelFrameworkRequestTests.cpp` checks pure cancellation previews and batch token capacity without publishing calls or consuming references. The original genuine-WDK `driver_wdm_interrupts.c` uses `NEVERD_WDM_INTERRUPT_FIXTURE` / `NEVERD_WDM_INTERRUPT_CFG_FIXTURE`; `DriverWDMInterruptTests.cpp` exercises normal/active-CFG rebasing, the legacy eleven-argument ABI, Ex versions 1/2/4, actual ISR→DPC completion, low-AL FALSE, synchronization/manual locks, independent PDOs, restart epochs and invalid hardware facts. C API/CLI tests reject invalid declarations before image loading and execute the seven-request [driver-interrupt-scenario.json](examples/driver-interrupt-scenario.json), checking the pending IOCTL bytes and separate delivery observations. Missing images skip explicitly; execution evidence remains Linux-only and does not establish instruction-level preemption. Explicit message and passive-ISR coverage is described below. Shared-line tests verify every latched handler runs even after a claim, matching cross-PDO resource tuples, caller-provided nonpaged locks and assigned-DIRQL scheduling distinct from synchronization IRQL. Level tests cover source OR, same-time assert/deassert, repeated sampling without fabricated edges or acknowledgements, delivery limits and source lifetime. Genuine normal/CFG images at both bases execute shared ISR chains and level assertion/repetition/deassertion.

`KernelFrameworkInterruptTests.cpp` checks the WDF interrupt lifecycle, exact
configuration failures, excess unassigned objects, synchronization return width,
resource lifetime, enable-failure rollback and deferred-callback draining before
D0 exit. `KernelInterruptsTests.cpp` checks WDF service arguments on the original
connection, descriptor-local message selection and waitable passive-lock
ownership. `KernelSchedulerTests.cpp` checks coalescing, namespace separation,
retention while suspended and separate passive framework continuations.
The genuine WDK `driver_kmdf_pnp.c` includes `driver_kmdf_interrupt.h`;
`NEVERD_KMDF_PNP_FIXTURE` / `NEVERD_KMDF_PNP_CFG_FIXTURE` cover real line/MSI and
passive callbacks, creation in PrepareHardware, all eleven interrupt table
slots, enable failure and STOP/restart reconnection through
`DriverKMDFPnpTests.cpp`. Normal and active-CFG images run at preferred and
rebased addresses. Missing external images skip explicitly; execution evidence
remains Linux-only. `KernelInterruptActivityTests.cpp` checks inactive connections,
shared-line eligibility, retained wake pulses, power failure and D0 admission.
`KernelFrameworkInterruptTests.cpp` checks report-active/inactive lifecycle and
wake callback ordering, including failed D0Entry cleanup.

`KernelFrameworkLockTests.cpp` covers real thread ownership, waiter references,
IRQL restoration, APC state, relative timeouts and parent deletion. Dispatcher
and interrupt tests cover shared external locks and recursive callback locks.
The original genuine-WDK `driver_kmdf_locks.c` uses optional
`NEVERD_KMDF_LOCK_FIXTURE` / `NEVERD_KMDF_LOCK_CFG_FIXTURE`;
`DriverKMDFLockTests.cpp` executes two-thread contention, zero/relative timeouts,
spin IRQL restoration, scheduled and foreground APC_LEVEL lock waits, and
unload at preferred/rebased addresses with normal
and active-CFG images. Missing images skip explicitly. The PnP fixture also
checks device/queue automatic callback serialization and explicit
`WdfObjectAcquireLock` / `WdfObjectReleaseLock` across real callback waits.

`DriverPowerScenarioTests.cpp` checks D2 native/JSON admission, exact reports and unsupported-state rejection. `KernelPowerRequestTests.cpp` checks exact D2 packet values, independent PoSetPowerState history and IRQL; `DeviceLifecycleTests.cpp` exercises all distinct low-power transition pairs, pure preflight, ticket preservation and the required completed D0 between them. Resource/MMIO/DMA/interrupt tests prove that D2 blocks hardware access while preserving provider assignments and caller-owned common buffers; D3cold tests prove D2 does not reset registers or advance the cold generation.

`DriverDeviceD2Tests.cpp` checks real synchronous/delayed PoRequestPowerIrp FIFO ownership, Query versus notification independence and rejection without FIFO consumption. Its genuine `driver_kmdf_child_wake.c` tests use `NEVERD_KMDF_CHILD_WAKE_FIXTURE` / `NEVERD_KMDF_CHILD_WAKE_CFG_FIXTURE` for D0/D2 callback order/counts, reported power packet states, separate D3 cycles, rejected direct D2→D3 and real D0 preparation before system sleep even without wake settings. The [D2 power scenario](examples/driver-d2-power-scenario.json) and public C API/CLI tests preserve these observations. Framework model tests separately inspect exact callback state arguments and cover explicit S0/Sx D2 settings, reassignment, child wake target selection and PoFx permission; this does not claim genuine WDF-settings coverage for those paths. Normal/active-CFG images run at preferred/rebased addresses; absent artifacts skip explicitly and runtime evidence remains Linux-only.

`KernelPowerCompletionTests.cpp`, `KernelProviderWaitWakeTests.cpp`, `KernelWdmWakeEventTests.cpp`, `DriverWdmWaitWakeTests.cpp`: The [native WAIT_WAKE scenario](examples/driver-wdm-wait-wake-scenario.json) uses the genuine WDK `driver_wdm_wait_wake.c` fixture via `NEVERD_WDM_WAIT_WAKE_FIXTURE` / `NEVERD_WDM_WAIT_WAKE_CFG_FIXTURE`. Tests cover actual START issuance, callback arguments, wake without implicit D0, rearm, cancellation, MPR, DPC cancellation with worker-issued D0, exact event capture and independent providers. Normal/active-CFG and preferred/rebased execution evidence is Linux-only; unavailable artifacts skip explicitly.

`KernelUsbIdleTests.cpp` verifies protocol ownership, exact callback/D2 identity, borrowed storage and sticky completion causes; `KernelUsbIdleBridgeTests.cpp` and `KernelUsbIdleReceiptTests.cpp` exercise actual caller IRPs, malformed admission, queued cancellation, composite capacity and nested receipt/completion timing. `DriverUsbIdleScenarioTests.cpp` checks native/JSON parity and evidence-limited reports. Genuine `driver_wdm_usb_idle.c` uses `NEVERD_WDM_USB_IDLE_FIXTURE` / `NEVERD_WDM_USB_IDLE_CFG_FIXTURE`; `DriverWdmUsbIdleTests.cpp` covers retained idle, cancellation, D0/D3, real remote wake, exact rearm/restart and independent/composite membership, including direct PDO versus FDO forwarding and allocator-stack ownership. `DriverWdmUsbIdlePublicTests.cpp` runs the [USB scenario](examples/driver-wdm-usb-idle-scenario.json) through C API/CLI, normal/active-CFG and preferred/rebased images. Missing artifacts skip explicitly; runtime evidence remains Linux-only. This suite does not establish KMDF USB selective-suspend support.

`KernelFrameworkUsbIdleTests.cpp`, `KernelFrameworkUsbIdleStorageTests.cpp` and `KernelFrameworkUsbIdleBridgeTests.cpp` verify policy, real storage and typed scheduling. Genuine `driver_kmdf_usb_idle.c` never submits its own USB idle packet. `DriverKMDFUsbIdleTests.cpp` covers no permission, delayed D2/D0 with managed I/O, StopIdle before/during the callback, failed arm, explicit Maximum capability, remote wake and composite membership. `DriverKMDFUsbIdlePublicTests.cpp` runs the [KMDF USB scenario](examples/driver-kmdf-usb-idle-scenario.json) through C API/CLI using `NEVERD_KMDF_USB_IDLE_FIXTURE` / `NEVERD_KMDF_USB_IDLE_CFG_FIXTURE`, normal/active-CFG and preferred/rebased. Missing artifacts skip explicitly; execution evidence remains Linux-only. After a successful wake-arm callback, modeled allocation exhaustion executes the real disarm callback and cancels WAIT_WAKE without consuming a D2 response.

`DriverKMDFUsbPoFxTests.cpp` exercises initial SystemManaged/WithHint assignment, independent PoFx/USB grants, cancellation in D0, delayed D2/D0 and a real worker-delayed F0 acknowledgement, activity and StopIdle, wake recovery before READ, failed arm, removal and restart. `DriverKMDFUsbPoFxPublicTests.cpp` runs the [USB PoFx scenario](examples/driver-kmdf-usb-pofx-scenario.json) through C API/CLI with both service modes, normal/active-CFG and preferred/rebased images. The direct READ cases in `DriverKMDFUsbIdleTests.cpp` and its public suite retain the original forwarding regressions and verify D0Entry before delivery without entering the router. `KernelFrameworkRequestTests.cpp` separately checks mapping, caller-context ownership, manual/stopped queues and IRQL. Model USB/PoFx bridge tests provide allocation-failure and independent-gate evidence; the genuine fixture does not exhaust the arena. Missing external fixtures skip explicitly; execution evidence remains Linux-only. `KernelFrameworkUsbPoFxBridge.RemovalPowerUpFailureAcknowledgesRequiredWithoutReleasingIdleWait` separately covers real D0Entry failure during RemovePending: the exact Required acknowledgement and quiescence permit failed-IRP/hardware cleanup without F0/ActiveCondition. It does not establish general present-device SET_POWER failure or autonomous surprise removal.

`KernelPowerCompletionTests.cpp` covers APC/DPC admission, full-capacity atomic retry, provider-only synchronous/delayed completion with and without callbacks, captured-route lifetime and MPR. The genuine `DriverWdmWaitWakeTests.cpp` extension checks direct D0 issuance from a DPC cancellation callback, independent APC/DPC Query/Set, unchanged caller IRQL/CR8, caller return before PASSIVE dispatch/completion and the still-rejected elevated WAIT_WAKE. The [elevated power scenario](examples/driver-wdm-elevated-power-scenario.json) runs via `DriverWdmWaitWakePublicTests.cpp` through C API/CLI, normal/active-CFG and preferred/rebased images; runtime evidence remains Linux-only.

`KernelFrameworkPowerPolicyTests.cpp` checks arm/disarm continuations, nested idle references and real D0 waits. `DriverKMDFPowerPolicyTests.cpp` uses the genuine `driver_kmdf_pnp.c` fixture with `driver_kmdf_power_policy.h`, via `NEVERD_KMDF_PNP_FIXTURE` / `NEVERD_KMDF_PNP_CFG_FIXTURE`, to cover D3hot idle, S0/Sleeping3 wake, retained and canceled WAIT_WAKE IRPs, arm failure, StopIdle waits, queue-triggered D0 and explicit system resume. The [KMDF power-policy scenario](examples/driver-kmdf-power-policy-scenario.json) uses the same optional fixture. `DriverScenarioPublic.CAPIAndCLIExecuteFrameworkIdleWake` runs its policy events and retained WAIT_WAKE observations through the C API and CLI. Missing images skip explicitly; normal/active-CFG and preferred/rebased execution evidence remains Linux-only.

`KernelFrameworkChildWakeTests.cpp` checks the independent own/child arm reasons, recursive opt-in propagation, batch preflight, captured epochs, late-arm rejection, arm failures and exactly-once disarm after the last child cancellation. `KernelFrameworkChildWakeBridgeTests.cpp` verifies actual retained IRPs, failure-atomic batch preflight and nullable causal report fields through the kernel host bridge. `DriverKMDFChildWakeTests.cpp` executes the genuine WDK `driver_kmdf_child_wake.c` through optional `NEVERD_KMDF_CHILD_WAKE_FIXTURE` / `NEVERD_KMDF_CHILD_WAKE_CFG_FIXTURE`. It checks own-only and child-only reasons, real retained WAIT_WAKE completion with causal source fields, disabled siblings, per-hop propagation opt-in, failed child or parent arming, cancellation and STOP/restart generations. The [child-wake scenario](examples/driver-kmdf-child-wake-scenario.json) runs the same fixture via CLI. Missing images skip explicitly; normal/active-CFG and preferred/rebased execution evidence remains Linux-only.

`ExecutionPolicyTests.cpp` checks exact 8-byte GS current-thread reads, including compiler MOV/CMP forms, and rejects neighboring, indexed, partial-width and store forms. Original instructions retain their architectural register and flag effects. `KernelPoFxAPITests.cpp` checks stable current-thread identity through nested callbacks, independent threads, reuse of modeled system-thread objects and opaque-object access rejection. `DriverSessionTests.cpp` exercises the default fixture’s current-thread read, real MOV/CMP flag effects, exact instruction budgets and rejection of ordinary-address access to the private processor view. The genuine PoFx fixture uses the original WDK current-thread helper to verify blocking callback identity.

`KernelPoFxTests.cpp` checks that framework quiescence cancels unissued decisions, retains pending callbacks and preserves driver ownership. `KernelPoFxTests.cpp` and `KernelPoFxAPITests.cpp` also check copied PoFx v1 descriptions, balanced references, explicit Fx/device decisions, hint constraints, callback admission/acknowledgement/return ownership, blocking thread continuations and release failures. `DriverPoFxScenarioTests.cpp` checks strict action fields and JSON/native report parity. The genuine-WDK `driver_wdm_pofx.c` uses optional `NEVERD_WDM_POFX_FIXTURE` / `NEVERD_WDM_POFX_CFG_FIXTURE`; `DriverWDMPoFxTests.cpp` covers synchronous/asynchronous callbacks, delayed acknowledgements and explicit Fx/power decisions at preferred/rebased addresses with normal/active-CFG images. The [PoFx scenario](examples/driver-pofx-scenario.json) uses its F1 command. Missing artifacts skip explicitly; execution evidence remains Linux-only.

`KernelFrameworkPoFxTests.cpp` checks single-component custom settings at `WdfDeviceWdmAssignPowerFrameworkSettings`, copied state, managed-idle prerequisites, one-time admission before first START completion, unsupported directed/PEP fields, Post/Pre callback ordering, callback drain before D0Exit across STOP/restart, failed-Post self-managed I/O suspension and independent physical-D0/component-F0 readiness. `DriverKMDFCustomPoFxTests.cpp` uses the genuine `driver_kmdf_pofx.c` with optional `NEVERD_KMDF_POFX_FIXTURE` / `NEVERD_KMDF_POFX_CFG_FIXTURE`. The [custom KMDF PoFx scenario](examples/driver-kmdf-pofx-scenario.json) uses this fixture. Tests exercise guest Fx callbacks and framework defaults, held requests until F0/D0 recovery, request-driven F0 restoration without another D-state IRP, balanced registration across STOP/restart, and failed-Post cleanup through normal/active-CFG images at preferred/rebased addresses. Missing images skip explicitly; execution evidence remains Linux-only.

`KernelFrameworkPoFxPolicyTests.cpp` checks framework policy integration. `DriverKMDFPoFxTests.cpp` uses the existing genuine KMDF PnP fixtures to distinguish idle/hints from explicit OS power-down permission, verify real D3/D0 IRPs and release/re-register across STOP, restart and REMOVE. `KernelD3ColdResourceTests.cpp` checks dedicated power capability, cold wake, assignment versus power generations, alias restoration and access failure. `DriverD3ColdScenarioTests.cpp` checks strict native/JSON facts. `DriverKMDFD3ColdTests.cpp` and `DriverD3ColdPublicTests.cpp` execute genuine WDF ExcludeD3Cold policy, cold register restoration, D3hot fallback, repeated SET D3 while already cold, and S0/Sx wake through native and public C APIs. DMA unit tests distinguish live transactions/channels from retained idle adapters and common RAM buffers. These fixtures use `NEVERD_KMDF_PNP_FIXTURE` / `NEVERD_KMDF_PNP_CFG_FIXTURE`; unavailable images skip, and the evidence remains Linux-only.

`DriverDMAScenarioTests.cpp` validates explicit capabilities, logical domains, byte/count/time limits, strict event directions and separate configuration/observations. `KernelPhysicalMemoryTests.cpp` and `BackendBackingTests.cpp` check shared-page allocation boundaries, pins, unchanged CPU permissions, MMIO/reentry exclusion and whole-span failure atomicity; `KernelRequestMDLTests.cpp` checks built descriptor aliases against the same physical identities. `KernelDMATests.cpp`, `KernelDMABridgeTests.cpp` and `SchedulerDMATests.cpp` exercise actual RAM bytes, adapter-bound table calls, inline/queued FIFO ownership, separate callback/map lifetimes, page fragments, wrong directions, release preflight, independent PDO domains and epoch/power failures. The original genuine-WDK `driver_wdm_dma.c` uses `NEVERD_WDM_DMA_FIXTURE` / `NEVERD_WDM_DMA_CFG_FIXTURE`; `DriverWDMDMATests.cpp` and C API/CLI coverage execute real adapter pointers, common/SG storage and separately configured DMA/interrupt events. The shared [driver-dma-scenario.json](examples/driver-dma-scenario.json) requires that fixture's protocol. Missing artifacts skip explicitly; execution evidence is Linux-only and does not establish real host DMA, PCI or a general device engine. `pluginsdk/python/tests/test_driver_dma_integration.py` exercises the existing owned JSON binding with `NEVERD_TEST_LIBNEVERD`, `NEVERD_TEST_WDM_DMA_FIXTURE` and `NEVERD_TEST_WDM_DMA_CFG_FIXTURE`, including bytes, callback order and reported failures.

`KernelSEHTests.cpp` checks pure unwind plans, scope order, nonvolatile GPR restoration, bounded stacks and explicit unsupported metadata; `KernelExceptionTests.cpp` checks exact API arity, low-32-bit statuses, typed exceptions, IRQL limits and unchanged model/CPU state. The genuine-WDK `/GS-` `driver_wdm_seh.c` uses optional `NEVERD_WDM_SEH_FIXTURE` / `NEVERD_WDM_SEH_CFG_FIXTURE`; `DriverWDMSEHTests.cpp` runs normal/active-CFG/rebased images through direct and helper raises, nested handlers, rethrows, actual filters and unwind finally callbacks, search ordering, stable guest exception records, valid CPU-fault continuation and full flags/SIMD restoration. Foreground, unrelated worker and attached-worker filters check inherited process identity and probe/MDL authority. Nested filters and collided finally callbacks execute on linked logical stacks; unrelated CPU faults remain explicit negative cases. C API/CLI executes [driver-seh-scenario.json](examples/driver-seh-scenario.json) and verifies null API results with actual guest handler messages. `pluginsdk/python/tests/test_driver_seh_integration.py` uses `NEVERD_TEST_LIBNEVERD`, `NEVERD_TEST_WDM_SEH_FIXTURE` and `NEVERD_TEST_WDM_SEH_CFG_FIXTURE`. Missing external images skip explicitly; evidence remains Linux-only and does not establish general SEH support.

`KernelSEHTests.cpp` checks GS fixed/aligned/negative cookie offsets, frame-pointer
encoding, wrapped-handler flags, search/unwind rechecks after filter or finally
mutation, prologue/epilogue exclusion and malformed metadata without stack reads.
`driver_seh_gs.h` first calls the linked genuine WDK cookie checker with each
original frame, then raises through `__GSHandlerCheck_SEH` or standalone
`__GSHandlerCheck`; normal/CFG images
at both load addresses must handle intact cookies and stop before handlers or
unload for corrupted ones. The C API and Python binding preserve that outcome. Standalone checks
must not invent C scope records. `COFFExceptionGSTests.cpp` verifies exact
standalone identities, truncated aligned-cookie payloads and rejection of
anonymous cookie-shaped metadata; `KernelSEHTests.cpp` verifies search and
unwind-only checks, outer-filter order and malformed standalone payloads.


`BackendBackingTests.cpp` also checks exact alias retirement, surviving derived
aliases and shared backing, same-address reuse with saved CPU contexts, and
whole-batch replacement at the mapping budget. Invalid duplicate, partial,
overlapping or retiring-source replacements preserve every original alias;
running, callback-reentrant and terminal-fault boundaries reject changes.
`KernelPhysicalMemoryTests.cpp` verifies immutable physical cache attributes.
`KernelMDLUserMappingTests.cpp` checks requested-address reuse, independent pins,
per-process view ownership, same-address process switching, wrong-process
unmapping/exit and failure-atomic attachment. The genuine user-mapping fixture
retains two processes' same-address views simultaneously across four requests,
checking distinct physical identities and read/write permissions after each
switch in normal/CFG and preferred/rebased images.

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

`DriverGuardTests.cpp` and four original `driver_guard.c` variants cover active/inactive CFG, rebasing, check/dispatch ABI and malformed targets. `KernelFrameworkTests.cpp`, `KernelFrameworkControlTests.cpp`, `KernelFrameworkQueueTests.cpp` and `KernelFrameworkRequestTests.cpp` cover bindings, validated registry-path snapshots, transactional device creation, queue routing, logical buffer lengths and cleanup/IRP/context lifetimes. The original `driver_kmdf_lifecycle.c` and `driver_kmdf_control.c` are optionally compiled against genuine WDK 1.33 headers and linked through the real `FxDriverEntry` library. Set `NEVERD_KMDF_FIXTURE` / `NEVERD_KMDF_CFG_FIXTURE` for lifecycle images and `NEVERD_KMDF_CONTROL_FIXTURE` / `NEVERD_KMDF_CONTROL_CFG_FIXTURE` for normal/active-CFG control-device images. Missing external artifacts are explicitly skipped. `DriverKMDFLifecycleTests.cpp`, `DriverKMDFControlTests.cpp` and C API/CLI cases in `DriverScenarioPublicTests.cpp` cover actual callbacks, buffered/direct I/O, pending worker completion, failure status, unload and rebased CFG execution. `DriverKMDFControlTests.cpp` also runs genuine-WDK buffered/direct/neither caller-context callbacks in normal/active-CFG images at preferred/rebased bases, verifying explicit enqueue before sequential queue delivery, completion within caller context, rejection of a callback that returns without either action, request-owned WDFMEMORY aliases for neither IOCTL/READ/WRITE, and no-access/read-only probe failures; `DriverScenarioPublicTests.cpp` executes positive caller-context and neither paths through the C API. KMDF cancellation coverage includes strict transfer-only virtual deadlines and report fields, completion-first and already-canceled paths, mark/unmark outcomes, queued-versus-delivered completion ownership, callback waits and internal-reference lifetimes. Scheduler tests independently verify DPC/cancellation/worker order, capacity, identity separation and suspend/resume. WDM cancellation has the separate genuine-WDK cases above. The original WDK-linked `driver_kmdf_pnp.c` fixture uses `NEVERD_KMDF_PNP_FIXTURE` / `NEVERD_KMDF_PNP_CFG_FIXTURE`; `DriverKMDFPnpTests.cpp` checks resource-free AddDevice, FDO/PDO and WDM/WDF handle identity, start, queue I/O, removal, automatic failed-Add cleanup and optional unload through normal/active-CFG images. The same genuine driver registers D0Entry/D0Exit and PrepareHardware/ReleaseHardware callbacks in service modes; tests cover delayed START, STOP/restart, final removal, empty resource-list getters, failed PrepareHardware and D0Entry with guaranteed ReleaseHardware, and unsupported callback rejection. Its default and explicit power-managed queue modes verify `WdfIoQueuePnpHeld` before D0 entry and during D0 exit, then normal I/O after each successful START, on normal/active-CFG images at preferred/rebased bases. `DriverScenarioPublicTests.cpp` exercises power-managed queue I/O through the C API and the hardware lifecycle through C API and CLI. A register-bank mode exercises read-only WDF resource descriptors, `MmMapIoSpace` and `MmUnmapIoSpace` across STOP/restart, with mutation and unreleased-mapping failure cases; the C API runs the positive resource lifecycle. Additional modes verify `EvtIoStop` completion, `WdfRequestStopAcknowledge` requeue and retained-request `EvtIoResume` across normal/active-CFG images, surprise-removal purge flags, worker completion after the callback returns without action, and an explicit stalled error when neither a callback nor a worker can complete the request. Native and C API tests also cover a power-managed queue without EvtIoStop that waits for a real worker completion, including a device with no registered hardware power callbacks. A separate genuine WDK mode verifies that `WdfDeviceInitSetDeviceType` reaches the FDO `DEVICE_OBJECT` on normal/active-CFG images at preferred/rebased bases; WDM unit coverage checks opaque vendor device types and control initializer rejection. Separate genuine control and PnP modes verify that `WdfDeviceInitSetExclusive` reaches `DO_EXCLUSIVE` on the created device at both image bases, while two opens through a nonexclusive named PDO remain valid. WDM stack tests check that the named lower object, rather than an exclusive unnamed upper object, determines open admission and that closing the first file releases exclusive access. A queued request submitted while stopped runs after restart, including the no-hardware-callback path; the C API covers both acknowledgment modes. Evidence remains Linux-only and does not establish full KMDF or PnP/power support.

Additional genuine `driver_kmdf_pnp.c` modes execute QueryStop, QueryRemove and
SurpriseRemoval notifications, including suspended callbacks before bus receipt,
query veto without lower side effects, later successful query and invalid
callback statuses. `AbsoluteSendTimeoutUsesCurrentTimeAndPreservesImmediateWins`
checks synchronous/asynchronous absolute deadlines from zero and advanced
virtual time, earlier timeouts, exact ties and immediate lower completion.

`KernelFrameworkFileTests.cpp` covers configured file identities, live WDM name and flags, FsContext/FsContext2 handle storage and retirement, optional file-object identities, request association, file-filtered manual and sequential retrieval, failed CREATE disposal, and CLOSE callback/context order. The genuine WDK `driver_kmdf_control.c` modes f/g/h/i/j exercise those paths in normal/active-CFG images at preferred/rebased bases, while the default no-file-object mode remains covered by request-accessor tests.


`KernelFrameworkRequestTests.cpp` checks cached input/output WDFMEMORY aliases, their original buffer pointers and logical lengths, zero-length and neither-I/O rejection, and invalidation after completion. The genuine WDK `driver_kmdf_control.c` modes b/c exercise both APIs and `WdfMemoryGetBuffer` over buffered/direct IOCTLs in normal/active-CFG images at preferred/rebased bases. `DriverScenarioPublicTests.cpp` runs the same buffered alias path through the C API.
The genuine WDK `driver_kmdf_pnp.c` file modes cover filter-default, explicit enabled and explicit disabled forwarding over a direct FDO/PDO route on normal/active-CFG images at preferred/rebased bases. `DriverKMDFPnpTests.cpp` verifies lower CREATE/CLEANUP/CLOSE status and callback order, failed lower CREATE disposal, and errors for missing or unused bus responses. `AutomaticFileForwardingWaitsForLowerCompletion` checks delayed automatic CREATE/CLEANUP/CLOSE; `OwnedCreateWaitsForDelayedLowerCompletion` and `OwnedCreateTimeoutCompetesWithLowerCompletion` cover callback-based asynchronous and synchronous CREATE, preservation of the suspended guest caller, Boolean send results, final lower NTSTATUS and lower-first deadline ties. Delayed send-and-forget remains independently covered. `DriverKernelFrameworkFileSend.PendingSynchronousSendRetainsOwnershipUntilLowerCompletion` rejects completion and request mutation while the caller is suspended, then restores driver ownership after the lower response. `DriverWDMPnp.DelayedFileForwardingRunsActualCompletionCallbacks` exercises delayed WDM CREATE/CLEANUP/CLOSE and failed CREATE through the genuine WDK completion routine, pending propagation and virtual timing on normal/active-CFG images at preferred/rebased bases. `DriverPnpScenarioTests.cpp` checks file-response schema constraints, while `DriverScenarioPublicTests.cpp` runs the forwarded lifecycle through the C API.

`KernelMDLChainTests.cpp` checks primary replacement, secondary append including an empty head, manual guest relinking, detached descriptor ownership, automatic completion-time unlock/release and surviving pool storage. Invalid cycles, dangling or shared links and removal of the original direct-I/O MDL fail before retirement; `KernelRequestMDLTests.cpp` also rejects private WDF descriptors inserted into WDM chains. `DriverMDL.GuestChainLinksSupportAssociationReplacementAndUnlinking` executes the genuine WDK macros and APIs at preferred/rebased addresses, while `DriverMDL.MalformedChainsAndOriginalDirectReplacementFailExplicitly` covers rejected guest chains. These cases do not establish arbitrary user mappings or hand-built MDLs.

Legacy cancellation tests retain the API continuation across cancellation, nested cleanup and final destruction; Ex still returns cancellation without callback delivery for an already canceled request. `KernelFrameworkRequestAccessorTests.cpp` and `KernelRequestMDLTests.cpp` check shared 64-bit Information, completion-time length validation, originating queue/IRP identity, NULL WDF file handles, retained-handle getter results, buffered MDL caching and first-direction ByteCount, direct descriptor identity and deferred mapping, completion retirement and rejected WDM-completion bypass. Genuine control-fixture modes L, M, D and C exercise legacy cancellation, buffered MDL/information, direct READ/WRITE MDLs and accessors after completion in normal/active-CFG images.

## Test layout

For optional independent bitvector oracles, configure `NEVERD_ENABLE_Z3=ON`
and run `NeverDSolverTests`; see [solver validation](solver.md) for the
cross-backend benchmark and replayable query export. Validate an OFF build too
when changing this boundary, so the default dependency-free backend remains
usable and explicit unavailable-backend requests are rejected.

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
| `unittests/corpus` (submodule) | `NeverDWindowsEHCorpusTests`, `NeverDRustEHCorpusTests`, `NeverDGoEHCorpusTests`, `NeverDCxxItaniumEHCorpusTests`, `NeverDObjCEHCorpusTests`, `NeverDAdaDEHCorpusTests` | Exception and runtime metadata read out of 545 pinned real binaries, each declared in a manifest with the floors its recovery must clear |

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
`check-neverd-cxx-itanium-eh-corpus`, `check-neverd-objc-eh-corpus`, and
`check-neverd-ada-d-eh-corpus` run one each. All three CI hosts configure with
the flag and run all six lines: the bytes are identical everywhere, but what
reads them is not. A corpus run on one host proves nothing about the other two.
`scripts/audit_ci_test_inventory.py` refuses an inventory that is missing any of
the six labels, because a build that quietly stopped reading the corpus is a
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

For Android fixture workspaces, SDK staging and multi-step local verification,
follow [Android validation artifacts](android-validation-artifacts.md). The CI
shard driver and local request driver retire owned outputs after their readers
finish and recover interrupted attempts on the next startup.

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

## Mobile SDK export evidence

The manual `Mobile SDK Export Evidence` workflow runs `collect_mobile_ios_sdk_declarations.py --exports-only` against the pinned Xcode SDKs. It retains the exact Foundation, CoreFoundation, and UIKit linker maps for both the iOS device and simulator SDKs, with target, SDK version, SDK settings hash, file size, and SHA-256. The normal declaration collector retains these maps too. Missing, empty, oversized, or SDK-external files fail collection while preserving completed evidence. Linker maps establish symbol export evidence; they do not prove a call ABI or method recovery.

## Mobile Swift String ABI evidence

The manual `Mobile Swift String ABI Evidence` workflow compiles fixed Swift equality/ordering probes and a C `swiftcall` probe with Xcode 26.5 for arm64 iOS devices and simulators. `collect_mobile_swift_string_abi.py` retains source, LLVM IR, assembly, compiler identity, SDK settings, and `libswiftCore.tbd` with hashes. Both languages must show the exact five-argument comparison import returning `i1`; C must explicitly extend that result to a byte. Wrong targets, changed signatures, failed commands, and timeouts preserve partial evidence and fail collection. This compiler evidence neither installs a runtime declaration nor establishes method recovery. Test the collector without an SDK using `python3 -m unittest scripts.tests.test_mobile_swift_string_abi`.

The driver gap regression suites additionally cover:

- `DriverKernelSEH` validates chained records, partial prologues, complete XMM
  restores and canonical epilogues at each partially restored instruction.
  Return-address bias and invalid/truncated epilogues cannot trigger speculative
  stack reads. Genuine WDK SEH tests exercise nested filters and collided cleanup.
- `DriverWDMInterrupt` executes independent and unified MSI messages, line
  fallback, and passive ISR waits with repeated and independent arrivals.
  `DriverScenarioPublic` verifies message observations through the C API.
- `KernelMDLChain` checks independent page allocation, contiguous chunks,
  partial ownership, alias reuse, all six PAGE protection modes and pool pins.
  Genuine WDK user-mapping modes exercise allocation/protection/free and
  KernelMode pool locks at the documented IRQLs in normal/CFG/rebased images.
- `DriverKMDFPnp` checks self-managed callback ordering, failed initialization,
  ordinary D3/D0 resource retention, surprise-removal queue ordering, independent
  system-policy Query/Set children and
  missing/mismatched explicit responses. The public C API checks child origins
  and response indices without assuming result rows are grouped by origin.

The constructor-loop-prefix profiles compile original Java 25 prologues with
two argument loops before `this`, recover their DEX, and compile every source.
Frozen ordinary, reflective and method-handle callers check repeated and zero
iterations, null identities, complete long words, exception identity and partial
final initialization. Both owned and dependency Trace declarations retain exact
original entry and metadata inventories. Native coverage checks loop receiver
origins independently and rejects receiver escapes and invalid result entries.

Constructor-loop-allocation profiles extend that original compiler witness with
a fresh object before effectful argument evaluation, both before and inside the
loops. They observe original allocation/argument/initializer order and partial
objects on failure, rather than only checking the final returned reference.

Bounded-carrier-seed-result profiles retain an invariant carrier of the method's
sole nominal formal beside direct formal inputs and a long auxiliary. Original
compiler metadata and frozen observers check the actual virtual result, nulls,
full-width time arithmetic, each seed identity, effects and thrown identity in
both owned and SDK declaration profiles.

Nominal-constructor-overload profiles verify that an exact fixed interface
argument excludes an incompatible class-formal sibling. The original compiler
witness keeps private this selection, nulls, raw float bits, input identity,
ordered exceptions and partial final fields through frozen public callers.
Native coverage rejects incomplete ancestry and applicable sibling bounds.

Allocation-retained-call profiles compile an original switch-expression witness
whose constructor preparation retains a reference for a later statement while
four float arguments reuse the original scalar. Independent observers check
class initialization, single call evaluation, raw bits, identities and partial
objects under failure, with owned and dependency declarations.

Paired-array-carrier profiles cover fixed-arity invocation of a generic varargs
method that consumes a covariant binary carrier array and returns an invariant
binary carrier. They retain the original array, element slots, provider writes,
actual result and failure order, and compare all original metadata through
owned and dependency declarations. Native checks reject changed rank, bounds,
variance, slot correlation and unavailable declarations.

Inherited-unwritten-boolean profiles retain a never-written instance final by
assigning its allocation default in a Java 25 prologue before the original
immediate superclass call. Frozen observers check superclass virtual callbacks,
reflection mutations, constructor delegation, partial objects and exact failure
identity. Original descriptors, metadata and absence of ConstantValue remain
unchanged, with owned and dependency observer variants. Native cases also cover
external parents, reordered declarations and rejected writes through aliases.
The `--package-owner` variants retain a package class and a private field; frozen
same-package callers and reflection check that source accessibility does not
change the before-super default proof or overwrite a superclass mutation.

Recursive-range-result profiles preserve a direct seed, an invariant range and
the returned formal under one complete `Comparable<? super T>` bound. The common
recursive carrier proof retains the original Comparable result without a
predicate receiver adaptation. Frozen callers check endpoint identity, ordered
reads and comparisons, long extrema, partial writes and exact failures, including
nulls and wrong endpoint types. Owned and imported variants compile all Java and
compare original metadata; five independent wrong programs test the observers.

Producer-predicate-completion `--fixed-completion` profiles keep the producer
formal in the callback input while both nested and final completion carriers use
one identical closed nominal value type. The shared full input proof checks the
complete relation and retains the actual Object result. Frozen callers distinguish
produced values from completion values, nulls, partial writes, failed reads and
late result casts. Both declaration modes compare all original metadata and
compile every recovered source; wrong completion payloads are independently tested.

Scoped-completion `--leading-context` profiles retain a separate closed nominal
reference before the existing scope/callback/completion relation. Static and
instance entries share the same complete nested type proof. Frozen callers
check exact context identity before completion effects, including nulls, failures
and deferred callback execution. Owned and imported declarations compile all
sources and retain metadata; the prior leading-word profile is checked too.

Same-bound-key `--static-context` profiles pass the original receiver as an
explicit closed nominal parameter. The same invariant key declaration must bind
the method's identical sole class or interface result bound. Frozen callers
check the actual context's state, null context and key failures, skip paths,
malicious raw providers and original bound casts. Owned and imported declaration
variants preserve all original metadata; native cases also cover static members
and reject changed key bounds, variance, scope and method flags.

Seeded-element-callback profiles bind a direct Object-erased seed to complete
callback input trees and the identical result formal. The shared recursive proof
checks closed nominal slots and lower wildcards without adding a callback or a
result cast. Frozen callers exercise member-class forwarding, two callback
invocations, null and arbitrary values, provider overrides, partial effects and
failure identity, including an original late cast. Owned and imported provider
variants compile every source and compare all original entries and metadata.

Inherited-fixed-marker profiles cover a nongeneric class whose raw
compiler access constructor forwards to a private constructor with a closed
generic callback parameter. The complete plain ancestry and closed carrier
arguments must be known. Frozen observers retain superclass effects, callback
identity, wide and boolean parameters, partial objects and exceptions; native
counterexamples reject missing declarations, variance and forwarding changes.
The original synthetic flag difference remains explicitly reported.
The `--open-owner` variants cover a concrete public class, retaining both direct
construction and subclass construction. Frozen ordinary, reflective and
method-handle callers check actual virtual callback selection, complete long
words, partial fields and exceptions. The same private target, complete known
ancestry and invariant generic argument proof applies to open owners.
The `--final-owner` variants retain a package-private final class with a plain
parent and interface. Frozen same-package callers, reflection and method handles
check private constructor delegation, complete long words, callback order,
partial fields and failure identity. Native cases cover both public and package
final owners with owned or imported declarations; missing ancestry and changed
generic bindings remain rejected.
The closed-parent-marker `--plain-parent` variants give a final class a plain
parent and a callback with a closed lower wildcard. The existing complete
argument proof applies to both parent forms. Frozen callers retain superclass
publication, scalar bits, partial fields, callback results and the original late
cast, with owned and imported contracts. Native cases cover both public and
package final owners and reject unavailable wildcard bounds.

Intersection callback `--branch-entry` profiles use an original Kotlin/D8 branch
to the argument copy. The copy's complete incoming state must retain one nominal
type satisfying both bounds; the original cast, callback preparation and call
must then remain a single linear region without handlers. Frozen callers check
both paths, exact failure order and callback identity with owned and dependency
providers, and a same-code wrong-bound program preserves its original later cast.

Scalar interface-super profiles retain one primitive parameter, its complete
incoming words, the original direct-interface qualifier and the actual primitive
result. The common bridge receipt checks the complete plain interface DAG and
one most-specific concrete default, excluding redundant qualifiers and abstract
or ambiguous selection. Virtual name planning consumes that receipt. Authored
Java with a diamond, two direct interfaces, an abstract owner, a final owner and
a child override checks both dispatch forms through frozen callers, reflection
and method handles. Name/bridge-flag encoding keeps original Code unchanged;
raw float bits, receiver identity, ordered effects and exact failures survive
recovery, with owned and dependency Trace variants. Native checks cover wide
inputs and results as well as unchanged reference getter dispatch.

Scoped completion `--instance-provider` profiles retain the original interface
receiver beside a three-slot callback with a fixed nominal scope, a nested
completion and an unchanged Object result. The callback and final completion
must share the sole Object-bounded method formal through the common recursive
carrier proof. Frozen ordinary, reflective and method-handle callers check
receiver identity, null-receiver failure before the provider body, deferred
callback capture, arbitrary results, suspension sentinels and exact failures.
Owned and dependency input declarations preserve every original metadata field;
native class/interface, range and reordered cases share the same call receipt.
The `--leading-word` variants retain an additional primitive argument before
that callback/completion pair. The original long is recorded before provider
effects, including on failure; frozen callers check both extrema and high words,
and a fifth wrong program truncates it to 32 bits. The complete generic carrier
proof and original Object result remain unchanged. Native cases cover static,
class and interface dispatch with long, double and int prefixes and reject
changed word order, arrays, bounds, variance and runtime descriptors.

Protected-bounded-receiver profiles exercise a final protected generic method
with one nominal or interface bound and matching direct input/result formals.
Cross-package calls require the original lexical self receiver and a complete
source-name lookup without competing overloads. Frozen callers verify the
actual returned object, including a different implementation or null, original
receiver/input identity, ordered callbacks and exact failures. Both owned and
dependency declarations retain their original metadata and method inventory.

Protected-fixed-mangled profiles cover a protected final method on an abstract
class, including an unchanged contravariant callback signature. Equal-length
constant-pool name encoding retains all original bytecode and attributes.
Frozen callers compare cross-package self calls, same-package receiver calls,
reflection, wide values, callback effects, arbitrary results and exact failures.
The original method name is the only disclosed metadata projection. Native
checks reject incomplete ancestry, illegal callers, changed receivers, name
collisions and unproved nullary calls before superclass construction.

The protected identifier runner's `--enclosing` variants retain an owned
anonymous allocation inside the renamed method. Frozen reflection observers
check the projected enclosing method and original captured owner identity;
complete class and method inventories admit only the disclosed local-name,
outer-constructor and synthetic capture differences. External anonymous and
named-local metadata still block identifier projection.

Protected-abstract-names profiles retain a complete protected abstract class
entry and two different overriding implementations under one disclosed source
name. Frozen callers, reflection, method handles, subclass self calls and
same-package callers compare 120 cases twice. They observe original receiver,
wide arguments, boolean results, callback order, partial publication and exact
exceptions. Four incorrect programs fail the frozen observers. Equal-length
name encoding preserves original bytecode; all class and method metadata match
except the three disclosed method names, with owned and dependency base cases.

The abstract naming runner's `--public-family` profiles cover a public abstract
class root and an independent interface root bearing the same JVM method name.
Each declaration retains its own root receipt while all five entries share one
Java spelling. All generated Java compiles, with 120 frozen behavior cases and
separate reflection checks of interface dispatch. Complete metadata comparisons
permit only the five disclosed method names. Native cases reject scoped or
external roots, abstract super calls and unrelated nonfinal protected entries.

Bounded-void-carrier profiles preserve a nominally bounded factory, three
contravariant consumers, plain auxiliary references and primitive flags. Frozen
callers, reflection and method handles compare 96 cases twice, including
arbitrary subtype or null results, failed factory and callback result casts,
ordered effects, partial publication and exact failure identity. Four wrong
programs fail the observers. Owned and dependency contract cases retain every
original class/method entry, full generic Signature, flags and metadata. Native
checks include wide auxiliaries, declaration order and rejected missing bounds,
carrier scopes, formal drift, varargs and checked exceptions.

Pure-allocation-receiver-copy profiles use an independently authored smali
client and immutable Java constructor/trace observers. They cover plain late
copies and early alias chains, wide and boolean argument values, reuse of the
original object word as a wide local, one returned object, partial final fields
and exact constructor exceptions. Each compares 36 cases twice through direct,
frozen, reflection and method-handle calls; four wrong programs fail observers.
All original client entries and metadata match. This authored layout is
explicitly distinguished from compiler output. Native checks also cover owned
constructors, order variants and rejected uninitialized uses or lost origins.

Closed-parent-marker profiles cover a package final class whose fixed parent
argument is an owned package type and whose private constructor takes a closed
contravariant callback. Both owned and dependency contract variants retain the
complete class/constructor Signatures and original metadata, apart from the
reported synthetic marker flag. Each compares 64 cases twice through frozen,
reflection and method-handle constructors, observing long/float values, callback
identity, parent effects, partial final fields and exact failures. Four wrong
programs fail the observers; native cases reject foreign/dependency-only private
types, changed bounds, forwarding words and unsupported constructor contracts.

Recursive-range-result profiles retain one complete recursive Comparable bound
across the seed, invariant range and original result. Owned and dependency
variants compare 80 cases twice through frozen callers, reflection and method
handles, including ordered endpoint reads, comparisons, nulls, failed casts,
partial writes and exact failure identity. All original entries and metadata
match. The inherited-owner variants additionally verify that the original DEX
calls through a subclass and that inherited static calls do not initialize that
subclass. Six wrong programs fail their observers. Native checks cover two-hop
lookup, declaration order, missing ancestry and invalid invocation styles.

Contextual-bidirectional-factory profiles preserve two Object-bounded result
formals through opposite callback directions, with one additional fixed context
input on the forward callback. Owned and dependency factories compare 68 cases
twice through frozen callers, reflection and method handles. The observers check
context identity and null, arbitrary intermediate objects, mutable inputs,
partial fields, callback direction, actual result identity and exact failures.
Five wrong programs fail the observers. Every original entry and metadata field
matches, and all recovered Java compiles. Native cases reject missing or generic
contexts, changed variances, bounds, formal directions and callback arity.

Private-static-field-name profiles use two explicitly encoded illegal Java field
names while preserving all original Code, flags, descriptors and other class
attributes. Owned and dependency observer variants compare 56 cases twice,
including static initialization order and sticky failure, mutable callbacks,
volatile long extrema, nulls, partial writes and exact results or exceptions.
Five wrong programs fail frozen callers. Every original entry and metadata field
matches apart from the two reported names. Native checks require exact local
access and reject inherited aliases, foreign access and unrelated field kinds.

Closed-allocating-constructor profiles retain an original box allocation from a
wide input before private same-owner constructor delegation. Complete callback
and carrier arguments are fixed nominal types. Owned and dependency inputs
compare 64 cases twice, including initialization and sticky failures, nulls,
wide extrema, callback argument and result identity, partial final fields and
exact exceptions. Five wrong programs fail frozen observers. All original
entries and metadata match; every recovered Java source compiles at the declared
minimum release. Native cases reject unresolved scopes, bounds and overloads.

Overloaded-scalar-interface-super profiles distinguish `(F)I`, `(I)F` and `(J)D`
defaults under one source name. The int overload selects a different ancestor.
Owned and dependency observers run 128 cases with two receivers across all three
overloads, including raw NaN bits, signed zero, integer precision, long words,
effect order and exact failures. Seven wrong programs fail frozen observers.
All recovered Java compiles, and original entries and metadata match except
explicit name and bridge-flag receipts. Native tests cover input order, range
calls, synthetic bridges, SDK declarations and ambiguous or changed contracts.

Nested-parent-mask profiles retain a package owner's complete
`Parent<Local<T>>` edge and its raw default constructor ABI. A shared access
check limits package carrier types to their original owned package; the common
signature resolver proves nested nominal bounds. Owned and SDK declarations
compare 256 cases twice through frozen callers, reflection and method handles,
covering mask selection, long values, lazy and sticky initialization failures,
partial parent/final fields, nulls and callback identity. Seven wrong programs
fail their observers. Every recovered Java source compiles and all original
entries and metadata match apart from the reported synthetic constructor flag.

The original Kotlin synthetic-name fixture's `--private-forwarder
--package-default` profile exercises the compiler-generated package-visible
static default-argument entry without changing class flags or Code. Both owned
and declaration-only `Box` variants retain every original method and field,
compile all recovered Java, and compare 60 fresh JVM cases with two attempts
per case. Wide values, object identity, initialization order and sticky failures
use the frozen original caller. A separately compiled wrong default must fail
that oracle. Native cases require the exact declaring class and runtime package
for renamed package-static calls, preserve access and metadata disclosures,
and reject inherited aliases, foreign callers, absent bodies and unsupported
contracts. Public and private static naming retain their existing rules.

The allocation-handler-gap runner's `--retry-loop` profile keeps the original
javac/D8 code for bounded catch-and-retry loops, including split handler copy
adapters. Its frozen caller checks one or two failures followed by success,
repeated caught failures, uncaught constructor failures and a throwing cleanup,
with exact exception identity, argument aliases, partial fields and effect
order. Owned and declaration-only constructor variants compile all recovered
Java and preserve every original definition and metadata record. Independent
wrong programs must expose lost retries, lost cleanup, wrong inputs and changed
field timing. Native tests exercise fixed-point handler validation with fresh
allocation entries and handler-local loops; unavailable deferred values and
entries into the middle of an allocation remain rejected.

The same runner's `--long-cleanup` profiles retain unchanged javac/D8 code with
160 ordered cleanup callbacks and a protected finally body. Frozen callers
exercise all four dispatch routes, nulls, constructor publication, caught and
uncaught failures, late callback exceptions and errors, and exactly-once
finally effects. All recovered Java compiles and original metadata remains
unchanged. Nine independently compiled wrong programs must fail the observers.
Native tests check long normal and exceptional continuations, late deferred
reads, invalid entries and exhaustion of the shared mobile work budget.

The erased-super-constructor runner's `--null-tail nominal|carrier` profiles
combine an original generic callback input with a closed null argument in the
same superclass call. Unmodified javac/D8 code supplies the zero literal and
exact constructor target. Owned and declaration-only variants compile every
recovered Java unit, retain metadata and use frozen direct, wrapper, reflective
and method-handle callers to check callback identity, overload choice, partial
fields, exceptions and repeated initialization failures. Separately compiled
wrong overloads and non-null arguments must fail the observer. Native cases
retain zero provenance through copies alongside intact wide words, and reject
nonzero/wide constants, unknown or incomplete types and changed generic
contracts. No class or method formal is inferred from null.

Private unary factory profiles exercise owned static and instance methods whose
covariant producer and invariant result retain the same method formal. Original
compiler output and frozen callers check callback counts, carrier identity,
initialization, exceptions and later casts, with carrier declarations also loaded
from a dependency JAR. Complete method/class metadata and inventories must survive
recovery and compilation; foreign private callers and incomplete type relations
remain rejected.

Inherited unwritten-Boolean profiles also cover static preparation of a closed
generic superclass argument. Allocation's false value must precede every original
prefix effect and remain unchanged by generated code after a superclass callback
mutates it reflectively. Frozen observers exercise prefix failures, partial fields,
wide values, constructor delegation, public/package owners and dependency observers.
Only the witnessed final field flag is encoded; compiler Code remains unchanged.

Stateless anonymous method-contract profiles retain original bridge bodies with
method-owned formals and closed parameterized declarations. Genuine Kotlin/D8
anonymous members exercise ordinary and synthetic bridge flags against owned and
dependency interface declarations. Five frozen dispatch routes check original
callbacks, effects, aliases and ordered failures across 80 fresh cases twice.
Full metadata and inventories must match after disclosed source name, annotation
retention and bridge-flag differences. Same-erasure contract drift and three
independently compiled wrong providers must fail their respective checks.

Invariant carrier-array result profiles cover inherited interface calls whose
Class<T> argument binds a T[] result. Original arrays and later caller casts
remain observable through 72 fresh cases twice, including nulls, differing
runtime array types, extreme integer inputs, mismatching/primitive class tokens,
callbacks and partial stores. Owned/dependency interface declarations keep full
metadata and definition inventories. Three same-erasure signature changes and
five wrong programs must fail; native checks also cover other invariant carriers,
static/virtual dispatch, wide arguments, range forms and incomplete relations.

The carrier-array runner's `--deferred-provider` profiles additionally retain
method-owned generic obligations through abstract generic class chains, with an
original abstract redeclaration and concrete implementation. The same 72-case
observer checks both source/dependency interfaces. Native sibling-interface
counterexamples compare complete unused formal bounds even when erasures match.

Conditional nested-copy profiles compile independent Java sources to DEX and
require both long and double methods to contain the original reference copy and
two wide copies between the selected inner allocation and range initializer.
Owned and immutable dependency providers each compare 96 fresh JVM cases twice,
including both branches, null tags, extreme longs, raw NaN payloads, signed zero,
partial constructor publication and first/repeated class initialization failures.
Five wrong programs cover reordered initialization, eager allocation, swapped
wide arguments, lost reference identity and canonicalized NaNs. Every recovered
Java file compiles; original entries, metadata and Exceptions match exactly.

The scoped-mask runner's `--fixed-nominal --private-target` profiles use genuine
Kotlin private constructors and their package-visible synthetic mask entries.
Direct and inherited owners run with owned or immutable dependency defaults.
Each profile compares 160 fresh cases twice, including null incoming callbacks,
mask selection, lazy provider initialization, partial fields and exact failures.
Reflection checks both original visibilities and the raw/generic signatures.
All Java compiles and all original definitions and metadata are accounted for;
four independently compiled wrong bodies must fail the frozen observer. Native
cases also cover public mask entries, nested closed carrier types, declaration
order, unknown ancestry, access drift, bad masks and uninitialized-this aliases.

Reference-array-store profiles require the independently compiled DEX to contain
five original erased-value stores, including incoming String[] and int[][] views,
fresh arrays, constructor arguments and a loop. Owned and immutable dependency
providers each compare 140 fresh JVM cases twice. Frozen direct/reflection
callers check null/bounds/component exception precedence, callback and class
initialization order, original identities and partial stores. Six independently
compiled wrong programs must fail. All generated Java compiles; original class
and method entries, metadata and Exceptions remain unchanged. Native checks cover
six array component shapes across ordinary and constructor-argument stores,
handlers and source/dependency linking, plus malformed operand counterexamples.

The inherited-fixed-marker runner's `--variant-input` profiles retain nested
wildcards in a two-parameter callback contract on abstract and open owners.
Owned and immutable dependency declarations compare 56 or 60 fresh JVM cases
twice respectively. Frozen reflection checks the complete nested wildcard tree;
other observers retain subclass dispatch during construction, partial fields,
wide/boolean arguments, markers, callback identity and exact failures. Six wrong
programs, including a changed nested generic contract, must fail. Original entries and all metadata match except the reported
synthetic flag; every recovered Java source compiles. Native matrices include
Object and inherited parent paths, regular/range calls, declaration order and
missing/inaccessible types, arity, bounds and forwarding counterexamples.

Guarded-scalar-conversion profiles independently compile int-to-float and
double-to-long defaults into DEX, requiring the read, conversion, provider call
and same-owner delegation in their original guarded order. Owned and immutable
dependency providers compare 104 fresh JVM cases twice, including both branches,
rounding boundaries, NaNs, infinities, signed zero, long extrema, lazy and failed
initialization, exact body failures and partial final fields. Six wrong programs
alter selection, conversion, widths, reads or initialization order. All Java
compiles and original definitions, metadata and Exceptions match. Native tests
cover all 15 conversions, regular/range calls, dependency declarations, source
order, malformed operands and implicit wide overlaps with both receivers.

The Kotlin guarded-constructor-chain profiles request a 600-second analysis
allowance, matching their existing CI profile deadline. This also raises the
bounded deterministic work allowance for the full compiler dependency catalog;
the default work ceiling was independently exhausted by the preceding binary.

Linear-integer-prefix profiles require an independently compiled boolean getter,
XOR and superclass initializer in that exact DEX order. Owned and immutable
dependency providers each compare 32 fresh JVM cases twice through frozen direct
and reflection callers. The observer checks getter identity/count, superclass
initialization, callbacks seeing allocation defaults, later provider mutation,
partial final fields and exact first/repeated failures. Six wrong programs must
fail. Every generated Java file compiles; original entries, metadata and
Exceptions remain unchanged. Native cases cover all 22 admitted integer opcode
forms, primitive and getter inputs, source/dependency declarations, regular/range
invocations, declaration order, guarded bitwise paths and malformed operands.

The raw-receiver runner's `--dependent-bounds` profiles retain method formals
bounded by the declaring class formal. A Signature-only edit places that single
type-variable bound in the interface slot while proving unchanged Code bytes;
the source spelling change must be reported exactly. Owned and immutable
dependency providers retain the 224-call, 64-platform-call, 192-failure matrix,
including virtual overrides, four caller routes, nulls, wide bits and effect
order. Frozen reflection checks the complete bound/input/result relation and
rejects a same-Code bound drift. Three wrong programs and an erased-descriptor
counterexample remain required. Every Java file compiles; all entries and
metadata match except the reported bound positions. Native matrices also cover
dependent owner bounds, chained method formals, cycles, unknown variables,
additional bounds, throws, static dispatch and typed inherited aliases.

Raw-reference-Unit profiles preserve a raw callback interface, its original erased
entry and its typed void overload. An independently authored access-only change
marks the erased entry bridge/synthetic without altering Code. Owned and immutable
dependency providers each compare 67 fresh JVM cases twice through direct, raw
interface, frozen, reflection, MethodHandle and typed-void callers. Observers check
cast precedence, null receivers, argument and receiver identity, partial effects,
exact target exceptions and first/repeated Unit initialization failures. Six wrong
programs must fail, and an incomplete bridge flag encoding must be rejected.
Every generated Java file compiles; original entries, raw ancestry, metadata and
Exceptions match except the reported bridge/synthetic flags. Native cases cover
one, three and 22 arguments, register copies, regular/range invokes, raw class
Signatures, owned/external declarations, declaration order and malformed bridges.

Default-scoped-covariance profiles compile an original Kotlin generic method with
a default callback. Its stateless raw callback retains both return descriptors;
the target's erased parameter reuses the shared synthetic-default scope receipt.
Dependency runtime declarations and compiler-distributed runtime DEX input each
compare 54 fresh JVM cases twice, including frozen direct default callers, Java
interface callers, reflection and MethodHandles for both callback descriptors.
Observers check method bounds, original bridge flags, mask selection, singleton
identity, null checks, factory/target exception identity and partial effects.
Four wrong programs must fail. All Java compiles and original metadata/Exceptions
match except reported member placement, default synthetic flag and parameter
scope erasure; D8 annotation retention is independently checked. Native cases
also cover an enclosing class formal shadowed by the method, declaration order,
regular/range calls, source/dependency ancestors and incomplete scope evidence.

Both Kotlin covariance runners use the CI profile's 600-second analysis allowance
for the complete compiler dependency catalog. The preceding binary also exhausts
the smaller default work allowance on the unchanged class-lambda fixture.

Raw-indexed-scope profiles retain an explicit raw callback bridge and its original
Number.intValue conversion, composing the shared default-method parameter receipt.
Both original descriptors remain; bridge/synthetic flag differences are reported.
Dependency runtime declarations and original compiler runtime DEX input each run
94 fresh JVM cases twice through direct, frozen, reflection and MethodHandle
entries. Long truncation, floating boundaries/NaN/infinities, custom Number effects,
null and cast precedence, singleton identity, partial stores and exact failures
are checked. Five wrong Kotlin programs and a separately narrowed Integer class
constant must fail. Every Java file compiles; original entries, complete metadata
and Exceptions match except the recorded source projections and helpers. Native
cases vary parameter position/count, register copies, regular/range invocations,
source/dependency declarations and order, with changed conversion/scope negatives.

Closed-default-access profiles keep complete closed parameterized method Signatures
on the original default and its one private body helper. Independently compiled
Kotlin originals and frozen callers compare 104 fresh JVM cases twice for owned
and immutable dependency contracts. They check direct default versus virtual
override dispatch, receiver and argument identity, wide values, partial fields,
nulls and exact target failures. Four behavioral counterexamples and a separate
Signature-only drift with unchanged Code must fail. Every generated Java file
compiles; original entries, metadata and Exceptions match except the reported
accessor synthetic flag and listed helpers. Native cases cover parameter count,
nominal/parameterized results, declaration order, owned/external contracts and
regular/range calls; changed bounds, formals and dispatch are rejected.

Synthetic-interface-name profiles apply an independently authored synthetic flag
to a Kotlin interface implementation, verifying unchanged Code and descriptors.
The whole owned interface family retains one disclosed Java name and complete
generic contracts; the synthetic method flag loss is explicitly recorded.
Plain and anonymous-enclosing-method variants each compare 98 fresh JVM cases
twice, including frozen interface/concrete callers, reflection, MethodHandles,
super dispatch, nulls, identity and exact failures. Changed superclass effects,
repeated final effects and a flag-only reflection drift must fail; the enclosure
variant also rejects changed enclosing metadata. Every Java file compiles and
all original entries, metadata and Exceptions are checked against the recorded
projections. Native cases vary generic scope, which overrides are synthetic,
regular/range calls, source/dependency marker ancestry and declaration order.
Synthetic roots, external interface contracts and incomplete bodies still fail.

Generic-abstract-name profiles preserve the complete public abstract class root,
its fixed generic superclass and wildcard parameter Signatures under one source
method name. Independently authored classfile name encoding keeps Code unchanged.
Owned and dependency superclass variants each compare 120 fresh JVM cases twice
across authored, encoded and rebuilt stages, using frozen callers, reflection,
MethodHandles and both implementations. Generic reflection, wide argument bits,
receiver identity, partial publication, callback effects and exact failures are
checked. Four behavioral counterexamples and an unchanged-Code Signature drift
must fail. All Java, original entries, metadata and Exceptions are verified;
only the disclosed method names differ. Native cases also cover separate class
and method contracts, unrelated interface roots, inherited symbolic calls,
source/dependency declarations and order; invalid scopes and access still fail.

Bounded-seed-factory profiles retain one complete method formal shared by an
invariant input/result container and a closed-input producer callback. The raw
call keeps its original erasures without inferring provider behavior. Owned and
immutable dependency providers each compare 64 fresh JVM cases twice, using
direct, frozen, reflection and MethodHandle callers. Observers check complete
generic reflection, bound casts, wide values, null/cast precedence, callback
counts, partial list mutations, publication and exact failure identities. The
returned list can be separate from the seed or null. Four behavioral variants
and an unchanged-Code Signature drift must fail; unproved callback variance is
rejected. Every Java file compiles, and all original entries, metadata and
Exceptions remain unchanged. Native cases also cover class/interface bounds,
input order, invariant/variant callbacks, regular/range calls, owned/dependency
declarations and order, with malformed contract and dispatch negatives.

Bounded-covariant-seeds profiles use the same invocation proof with two covariant
input containers and no callback parameter. The authored provider explicitly
casts before its original mutation and returns the second input, including null.
Owned and dependency providers each compare the same 64 fresh JVM cases twice;
callbacks come from original provider state and are not inferred from its method
contract. Frozen observers check both wildcard Signatures, distinct input/result
identities, effects, bound casts and partial failures. Five wrong programs and
an invalid contravariant input test retain independent checks. All Java, original
entries, metadata and Exceptions are verified. Native coverage adds one, two and
four containers, invariant/covariant inputs, class/interface bounds and source/
dependency declaration order; unmatched formals and result variance are rejected.

Moved-receiver-final profiles extend the typed-default fixture with independently
witnessed receiver-register placement before Object preparation. The original
classfile Code is unchanged; only two DEX instructions change layout, with an
exact byte receipt and SDK disassembly checks. Owned and dependency interface
profiles compare 32 fresh JVM cases twice across authored, final-flag encoded
and rebuilt stages. Frozen callbacks observe allocation defaults, modify a final
slot during construction and publish the partial object before an exact failure.
Original calls, arguments, field reads, raw bits, identities and metadata are
checked, alongside eight behavioral counterexamples. Native coverage varies all
ten field types, chained aliases, range calls, source scope and declaration order;
wrong origins, argument overwrites, effects and constructor re-entry must fail.

Closed-nominal-marker profiles use class and interface bounds on an original
constructor callback, with a concrete actual type satisfying that declaration.
Source-owned abstract and dependency-backed final owners each compare 56 fresh
JVM cases twice per bound kind. Frozen direct, reflection and MethodHandle
observers check private constructor access, generic bounds, wide/boolean inputs,
super/subclass dispatch, nulls, callback ordering, partial publication and exact
failures. Five behavioral counterexamples and an unchanged-Code bound Signature
drift must fail. All Java, original entries, metadata and Exceptions are checked
against the sole disclosed synthetic constructor flag difference. Native cases
also cover self arguments, class/interface bounds, all three explicit variances,
abstract/open/final owners, source/dependency declarations and order; incomplete
bounds, scope and original ABI drift remain rejected.

Synthetic-default-body profiles apply a witnessed access-only flag to an original
Kotlin default, keeping Code, descriptors, annotations and generic contracts
unchanged. Owned and dependency variants each compare 104 fresh JVM cases twice,
including frozen direct/accessor/reflection/MethodHandle callers, override versus
default dispatch, callback effects, wide bits, partial construction and exact
failures. All generated Java and original inventory/metadata/Exceptions are
checked; only the reported helper and synthetic flags differ. Six counterexamples
include unchanged-Code Signature and synthetic-flag drift. Native cases cover
void/scalar/wide/reference returns, closed contracts, source/dependency declarations,
range calls and order. A valid virtual forwarder must receive no default receipt;
bridge/abstract/native targets, wrong receivers and unproved scopes remain rejected.

Raw-index-result profiles use a nongeneric Kotlin default callback with original
int/Object target parameters and a Void result. The unmodified compiler emits
both the raw interface entry and Number.intValue bridge. External-runtime and
input-runtime variants each compare 94 fresh JVM cases twice, with frozen
interface, reflection and MethodHandle callers of both overloads. Long values,
floating boundaries/NaN, custom Number effects, null/cast precedence, reference
identity, partial stores and exact failures are observed. Six counterexamples
include narrowing Number to Integer. Every Java file, original entry, metadata
and Exceptions is checked with explicit placement/flag disclosures and no
invented generic Signature. Native cases add one, two and four slots, index
position, receiver/argument copies, range calls, dependency declarations and
order; changed conversion, receiver, target, result and captures remain rejected.

Nested-bidirectional-factory profiles extend the contextual callback relation
with a shared List payload and a fixed Object result slot. Owned and dependency
providers each compare 76 fresh JVM cases twice through frozen direct, reflection
and MethodHandle callers. Observers check complete nested generic contracts,
context/input/callback/result identity, mutable list contents, nulls, partial
fields, both original casts and exact exception precedence. Five behavioral
changes, a removed nested cast and unchanged-Code Signature drift must fail.
Every recovered Java file, original entry, metadata field and Exceptions
attribute is checked. Native cases exercise invariant and variant callbacks
and containers, class/interface contexts, range calls, dependencies and order;
unbound formals, unknown carriers, inconsistent variance and altered result
contracts remain rejected.

Scalar-field-copies profiles retain volatile boolean reads and range-constructor
copies through the shared allocation graph. Owned and dependency targets compare
80 fresh JVM cases twice through frozen direct, reflection and MethodHandle
callers of static and instance field paths. Class initialization toggles the
source before the read; the constructor publishes final fields and changes the
source afterward. Null and initialization/body failure precedence, live aliases,
wide values and raw floating bits are observed. Five wrong programs must fail.
All Java, entries, metadata and Exceptions are checked. Native cases add all
eight primitive field types, chained copies, static/instance fields, range
calls, dependencies and order; invalid types, words, receivers, access and
control entries remain rejected.

Anchored-array-void profiles retain private static and instance array consumers
with source and dependency carrier declarations. Each configuration compares
88 fresh JVM cases twice through frozen direct, private reflection and private
MethodHandle callers. Observers check the complete invariant/array Signature,
private flags, null and initialization/failure precedence, partial additions,
ignored false callback results, covariant arrays, repeated references and
callback mutation of visited and upcoming elements. Six wrong programs and
unchanged-Code Signature drift must fail. Every Java file, original entry,
metadata field and Exceptions attribute is checked. Native cases also cover
public/package/private calls, dependency providers, one/two array ranks, class
and interface carriers, parameter order, range calls and declaration order;
foreign or inherited private calls and incomplete bounds/contracts stay rejected.

Array-formal-result profiles extend array-seed factories with an original
T[] -> T contract. Owned and dependency provider variants each compare 136
fresh JVM cases twice through frozen direct, reflection and MethodHandle
callers. Observers check the complete generic Signature, inherited static
dispatch, initialization failure, partial stores and covariant-array failures.
Actual null, arbitrary independent, aliased and mutable result objects remain
observable; checked callers cast only after the original publication/effect.
Four wrong programs and unchanged-Code result Signature drift must fail, and
three unsupported or malformed contracts must fail recovery clearly. Every
Java file, original entry, metadata field and Exceptions attribute is checked.
Native cases add range calls, final declarations, dependencies, inheritance
and order; unknown formals, altered bounds, scopes and dispatch remain rejected.

Nested-private-scalar profiles retain a private integer result with a complete
Cell<Inner<T>> anchor. Static/instance and source/dependency carrier variants
each compare 64 fresh JVM cases twice through frozen direct, reflection and
MethodHandle callers. Observers check nested Signature, original receiver,
wide/scalar bits, mutable getters, callback inputs/results, arbitrary integer
extrema, partial fields and exact null/cast/failure precedence. Four behavior
changes and unchanged-Code nested Signature drift must fail. All Java, entries,
metadata and Exceptions are checked. Native coverage extends the shared anchor
to all eight primitive results and fixed nominal results, three nested depths,
source/dependency declarations, static/instance dispatch, range calls and order;
variant, unknown or unbound nested slots and foreign private calls stay rejected.

Member-formal-names profiles encode only Signature tokens in an owned Java
class to reproduce a class-formal/member-name collision. Code and every other
attribute are verified unchanged. Source/dependency trace and reserved-name
variants each compare 64 fresh JVM cases twice across original, encoded and
rebuilt classes. Frozen observers check member binary identity, exact formal
declaration/bound, independent method-formal shadowing, partial stores, reads,
raw/checked results and exception precedence. Four wrong programs include a
same-Code nominal-member substitution for a formal reference. All Java, entries,
metadata and Exceptions must match only the four complete scoped name receipts.
Native cases cover unused versus used formals, fresh-name conflicts, method
shadowing and order; missing/duplicate members and unresolved variables fail.

Direct-seeded-nominal profiles retain T -> Object calls on generic classes
whose class and method formals share a name. Object/Number class bounds and
source/dependency provider configurations each compare 36 fresh JVM cases
twice. Frozen callers and reflection check distinct generic declarations and
bounds, null/arbitrary/aliased/mutable results, partial array mutation and
exact initialization/body/cast failures. Four wrong programs include returning
the seed instead of the actual result and unchanged-Code parameter Signature
drift. Three malformed/unsupported contracts fail recovery clearly. Every
Java file, original entry, metadata and Exceptions is checked. Native cases
add plain Object or nominal class results, package owners, range calls and
order; unknown bounds, result declarations, scopes and dispatch stay rejected.

Closed-owner-delegation profiles retain an allocation before this(...) when
the owner implements View<Box<Value>> and a plain marker interface. Public/private
selected constructors and source/dependency declarations each compare 64 fresh
JVM cases twice through frozen direct, reflection and MethodHandle callers.
The observer checks zero owner formals and both complete interface edges, then
allocation/class initialization, wide bits, callbacks, partial final fields and
exact failures. Five wrong programs must fail. All Java, entries, metadata and
Exceptions remain unchanged. Native cases add simple/nested interface arguments,
private targets and order; owner formals, unknown/mismatched bounds/edges and
unproved constructor contracts remain rejected.

Projected-owner-delegation profiles combine a fixed generic constructor with an
independently authored terminal throwing bridge. Only the class Signature and
bridge/synthetic flags are encoded; all Code bytes and descriptors stay fixed.
The original complete View<Box<Value>> contract and separate Marker<Box<Value>>
edge have zero owner formals. Four public/private and source/SDK variants each
compare 64 fresh JVM cases twice, retaining constructor order, callbacks, wide
bits, partial final fields and failure identity. Frozen direct, interface,
reflection and MethodHandle callers test incompatible/null/typed bridge inputs
without an inserted cast. Six wrong programs include adding that bridge cast.
Every Java file, entry, metadata and Exceptions is checked against exactly the
existing terminal interface and bridge receipts; constructors gain no metadata
projection. Native cases cover simple/nested interface arguments and order, and
retain malformed/open owner, target, bound, bridge, varargs and throws failures.

Constant-cleanup-adapters profiles add independently authored narrow/wide
constant assignments to both protected allocation handler adapters. A frozen
JVM observer checks the raw stored constant, input identities, partial fields,
original catch versus uncaught exception identity, and allocation entry frame.
Four zero, integer sign-bit and wide NaN-payload configurations each compare
24 fresh JVM cases twice, including direct/reflection/MethodHandle callers.
All Java, original entries, metadata and Exceptions must match. Six wrong
programs include changing the constant and storing it before allocation.
Native cases compare decoded constants across opcode encodings, retain input
origins, and admit differing dead constants only after a real overwrite. Wrong
bits/halves, observed input drift, malformed or throwing assignments, bad handler
entries and cycles remain rejected. The owned smali adapters retain the original
Java reference program's runtime descriptors and independent observer contract.

Conditional type/interface-test profiles widen an independently authored
provider's results to Object and guard a repeated getter with instance-of.
Class/interface and owned/SDK provider-target configurations each compare
84 fresh JVM cases with two calls per case. Frozen callers distinguish null,
first incompatible objects, later incompatible objects, changed getter results,
wide argument bits and exact initializer/body/cast failures. Four wrong programs
cache the getter, lose the type test, cast before the test, or evaluate an
argument before target class initialization. Every Java file, original entry,
metadata and Exceptions is checked. Native cases add reference/primitive arrays,
wide and range frames, dependency and class order; malformed tests, receiver
use, foreign entries, cycles, handlers and private constructors stay rejected.

Captured-closed-delegation profiles use an independently authored Java member
with a private generic primary constructor and a public wide delegating entry.
Private/public member and owned/SDK callback-value variants each compare
68 fresh JVM cases twice through frozen direct, reflection and MethodHandle
callers. They cover exact captured outer identity, null outer values in raw
reflective calls, class initialization, generic callbacks, arbitrary/null results,
wide bits, partial fields and every constructor failure stage. All Java, original
entries, metadata and Exceptions must match only the existing captured-member
and omitted-parameter disclosures. Six wrong programs change callback, value,
wide, index or publication order. Native cases also cover private callers,
complete versus omitted outer Signature slots, range calls and class order;
open outer scopes, broken capture origins, unknown types, bounds, synthetic
callers, varargs, throws and nonfinal owners remain rejected.

Captured-access-delegation profiles author a raw forwarding entry to a private
wide constructor, then encode only that entry's synthetic flag. All Code,
descriptors, other flags and attributes stay byte exact; original and encoded
classes both execute against the frozen JVM observers before recovery. Four
private/public member and source/SDK configurations each compare 68 fresh cases
twice per original/encoded/rebuilt stage. Generic constructors keep their exact
implicit-capture mapping; the raw marker entry gains no Signature. The existing
capture disclosure must name the synthetic constructor flag difference. Seven
wrong programs include forwarding the unused marker as the callback. Every Java
file, original entry, metadata and Exceptions is checked. Native cases add
leading locals, range/ordinary calls and full/omitted target Signatures; flags,
word order, marker declarations/bounds, extra work and target drift fail clearly.

A separate captured primitive-marker regression keeps ordinary int/long entries
recoverable without the new nominal-marker receipt. An owned smali reproducer
also compares the previous binary, the observed new rejection and the fixed
binary, then compiles every fixed Java source. These cases protect existing
scalar constructors from a reference-Signature parser being used on primitives.

Comparison-carrier profiles use an independently authored opaque int provider
with List<? extends T>, two int bounds and Comparison<? super T,Integer>.
Package owners and public owners with a same-named Number-bounded class formal,
each as source/SDK declarations, compare 72 fresh JVM cases twice. Frozen
reflection checks the exact method/class formal identity, Object/Number bounds
and both complete carriers. Runtime cases retain mutable upcoming list values,
null/incompatible callback results, integer overflow, partial stores, arbitrary
provider results and exact initialization/callback/cast/index failures. Six
wrong programs include caching an element, replacing the actual provider result
and unchanged-Code callback Signature drift. All Java, entries, metadata and
Exceptions are checked. Native cases add variance, range, source/order and
scope combinations; missing declarations and malformed/open contracts fail.

Filled-int-constructor profiles use independently authored Java 25 constructors
with distinct/shared int arrays, a this delegation and a retained array local.
Each source/SDK profile compares 56 fresh JVM cases twice through direct,
compiled, reflection and MethodHandle callers. Frozen observers check array
identity, element order, signed overflow, partial final-field stores, callback
mutation, retained postlude writes and exact initialization/seed/callback/body
failures. Both original class versions and version-only D8 inputs execute with
all Code/attribute bytes unchanged. Six independently compiled wrong programs
alter aliasing, elements, seed results, final-store timing or postlude values.
All emitted Java, original entries, metadata and Exceptions are checked. Native
cases cover ordinary/range arrays and delegation, aliases, linked/direct entry
and continuation; undefined/self/primitive slots and malformed pairs fail.

Conditional-packed profiles combine reference equality or ordered int input
comparison with raw float-to-long shift/mask expressions inside an original
allocation gap. Owned/SDK Provider and Target profiles each compare 64 fresh
JVM cases twice using frozen direct, compiled, reflection and MethodHandle
callers. Both branches preserve nominal identity, signed zeros, NaN payloads,
infinities, masked shift distances, arbitrary seed results and exact ordered
class-init/provider/constructor failures. Six wrong programs alter condition,
shift, mask, float input, seed or tail effects. All Java, entries, metadata and
Exceptions are checked. Native cases cover all six int relations, reference
eq/ne, six long bit operators, ordinary/two-address forms and source/SDK
entries; malformed relations, word/type drift, receiver aliases and escaping
or looping edges fail clearly.

Multiple-final-exits profiles use independently authored Java 8 constructor and
class initializer bodies with two wide final stores and three original linear
throw blocks after the sole return. Normal/reversed guard layouts with
source/SDK Trace each compare 64 fresh JVM cases twice through frozen direct,
compiled, reflection and MethodHandle entries. An external IntConsumer checks
every partial final-field state, including reentrant static reads. Original
throw identity, null-throw NPEs, observer failures and sticky failed class
initialization match. Six independently compiled wrong programs change store
values/timing, exception identity or guard/effect selection. All Java, entries,
metadata and Exceptions are checked. Native cases cover two/three exits and
instance/static linked/direct paths; interior entries, return edges, rewrites,
loops, missing terminal throws and protected multi-exit layouts fail clearly.

Raw-zero-reference profiles use independently authored raw Producer<R> owners
with unchanged covariant Value/Void methods and compiler-generated Object
bridges. Each source/SDK profile compares 32 fresh JVM cases twice through
typed, compiled raw-interface, reflection and MethodHandle entries. Frozen
reflection checks the raw class header, exact result formal, typed method and
synthetic Object bridge. Opaque nonnull/null result identity, partial static
mutations and exact class-init/body failures match. Six wrong programs include
invented owner type arguments; Value profiles also substitute the opaque
result. Every Java file, original entry, metadata and Exceptions is checked.
Native cases add range, final typed methods, raw explicit/missing headers,
source/SDK declarations and reversed order; invalid bridge bodies, scopes,
bounds, result classes and captured storage fail clearly.

Constructor-private-getter profiles use independently authored Java 25 masked
prologues calling a private value method on an existing static receiver before
a pure-return this delegation. Boolean/int results with source/SDK Defaults
each compare 48 fresh JVM cases twice through frozen direct, compiled,
reflection and MethodHandle callers. Both original and version-only D8 inputs
execute with unchanged Code/attributes. Receiver/result snapshots, conditional
class initialization, mutable static replacement, partial final stores and
exact getter/target failures match. Seven wrong programs alter getters, result
or receiver snapshots, mask bits, static mutation or final-store timing. All
Java, entries, metadata and Exceptions are checked. Native cases cover scalar,
wide/reference/boolean results, linear/guarded prefixes, range forms and SDK
fields; foreign private calls, uninitialized/primitive receivers, missing
members, result kinds, word counts and exception contracts fail clearly.

The constructor-parameter-read Kotlin fixture requests the existing bounded
600-second/40M-work analysis profile for its complete standard-library
classpath. The baseline binary also exhausts the default 20M ceiling on this
owned input. Global analysis limits and semantic checks remain unchanged; the
fixture still compiles every source and compares its full frozen JVM oracle.

Raw zero-input reference regeneration also rejects inherited Object source
slots and additional unrelated return-only overloads without a separate alias
receipt. Native cases vary declaration order and linked/direct entry. An
independently authored smali getClass covariance example is accepted by the
previous profile but javac rejects every-source compilation; the fixed profile
must reject it before publishing source. The unrelated return-slot example
retains the common exact-dispatch rejection.

The allocation-retained-int fixture covers verified integer forwarding wrappers
inside nested constructor arguments. Its Java 8 input is separately copied with
only ACC_BRIDGE changed; every Code, descriptor and attribute byte is unchanged,
and both originals execute against the same frozen caller. Four source/SDK and
base/derived profiles each compare 64 fresh JVM cases with two attempts through
direct, compiled, reflection and MethodHandle routes. Mutable repeated getter
snapshots, wrapper stack frames, virtual overrides, null receivers, ordered
class initialization, partial final stores and exact failures are observed.
Six legal wrong programs bypass or cache wrappers, move calls, change receivers
or snapshots, or write finals early. Every Java source is compiled; original
entries, complete metadata and Exceptions match except the disclosed bridge
flag Java cannot spell. Native positives vary synthetic/range flags, API source
and SDK declarations and traversal order; missing or changed forwarding proofs
fail clearly.

The private-seeded-result fixture preserves two independent method formals on
private static and instance providers. Source and SDK API variants each run
80 fresh JVM cases with two attempts through direct, compiled, reflection and
MethodHandle routes. Frozen implementations produce opaque results and mutate
state; observers check full generic reflection, receiver/seed/result identity,
callback order, nullable values, exact failures and checked, raw and discarded
result timing. Six legal wrong programs change results or snapshots, insert an
early cast, skip a callback, change its seed or repeat the producer. Every Java
source, original entry, complete metadata and Exceptions table is checked.
Native cases also cover range forms and traversal order; foreign or inherited
private references, mixed formals, wrong variance, extra bounds or formals,
missing declarations, altered flags, result kinds and checked throws reject.

Allocation-retained-int also has four plain API profiles: no input class
Signature, source/SDK interfaces and base/derived getters. They preserve the
same complete 64-case, two-attempt frozen JVM matrix, byte-exact bridge encoding,
wrong programs, all-source compilation, metadata and Exceptions checks. Native
cases vary open/final owners, synthetic flags, repeated/range calls, API origin
and traversal order. Both owner discovery and empty-argument edge discovery
must retain the common proof; malformed wrappers and contracts still reject.

The constructor-literal-allocation fixture creates an argument object with
three null slots and an integer mask before private same-owner delegation.
Positive/negative masks and source/SDK Token declarations each run 64 fresh
JVM cases with two attempts through four immutable caller routes. Observers
check class initialization, fresh references, two partial final-field objects,
input identity, raw wide words, nulls, exact callback/constructor failures and
sticky initialization failure. All Java, inventory, metadata and Exceptions
match. Seven wrong programs alter literal slots, mask, input or wide values,
or move stores. Native cases additionally vary local/reference-parameter
destinations, wide literals and range/order forms; effects inside the bounded
literal region, receiver overlaps, undefined words, malformed operands and
changed initializer contracts fail clearly.

Allocation-instance-calls has inherited-provider variants that move the
original read implementation to a new Base and retain Provider's self caller.
The original frozen callers still check base/derived virtual dispatch, allocation
initialization, mutable receiver/result identity and exact failures. The DEX
witness must retain the Provider spelling of its inherited read call. Source
and SDK providers cover protected and package access; every Java, entry,
metadata and Exceptions table is checked, private-flag edits reject, and wrong
hoisted or overridden programs fail the independent observer. Native reference
and scalar cases cover copies, range forms, package classes and traversal order;
foreign scopes, generic/unknown chains and altered calls or frames reject.

Allocation-array-lengths also covers a total integer prefix before the array
read, with positive/negative biases and source/SDK providers. The original DEX
must retain arithmetic between new and array-length, and an independently
compiled observer checks the actual prefix field alongside initialization,
mutable array selection, null failures, nested allocation and retained length.
Every profile compares its complete 160-case, two-attempt JVM matrix, all Java,
entries, metadata and Exceptions; a wrong bias joins the existing four ordering
counterexamples. Native cases vary array kinds, arithmetic forms and live prefix
results, while throwing/unknown operations, invalid types, receiver writes and
external entries keep their rejection guards.

The `test_mobile_android_conditional_array_lengths.py` source and SDK-target
profiles compile every emitted Java source and preserve original definitions,
Code presence, access roles and Exceptions metadata. Each profile compares
64 fresh JVM cases with two calls per case through direct, previously compiled,
reflective and method-handle callers. The observers check selected and skipped
length reads, class initialization, captured integer arguments, partial
constructor effects and exact checked/runtime/Error failures. Eight independent
wrong programs exercise the observers. Native cases cover primitive/reference
arrays, live length results, range frames, link order and rejected receiver,
type, payload and CFG changes.


Constructor-choice uses an independent bytecode builder to keep one allocation
before a null branch and two original overload initializers. The builder emits
Java 8 classfiles through the JDK's bundled ASM API; its output and an ordinary
Java reference both pass the same frozen caller before D8 or NeverD runs.
Four source/SDK and branch-polarity profiles each compare 128 fresh JVM cases
with two attempts, including sticky class-initialization errors, partial final
fields, input identity, observer mutations and exact handler exceptions. The
DEX witness must retain one new before the branch and both constructor calls.
All Java, original entries, metadata and Exceptions are checked; seven wrong
programs must fail the independent observer. Native cases cover copies, range
forms, traversal order and handler visibility, while changed control-flow,
constructor contracts, input types or exception contexts reject.

Monitor-copy-cleanup pairs an independently authored immutable smali Gate with
an independently compiled Java reference and frozen callers; it does not rewrite
or execute smali as a JVM classfile. Four narrow/wide and source/SDK profiles
compile every recovered source, compare all original entries, metadata and
Exceptions, and run the same observer before and after recovery. Each checks
6000 concurrent updates, reentrancy, null locks, partial construction, all three
failing protected stages, Exception/Error identity and release to another
thread; five wrong programs must fail. Native original-CFG cases cover all copy
forms, handler coverage and class traversal order; 32 mutations reject changed
locks/exceptions, observable cleanup, backward or external entries and invalid
frames or operands. Existing continuous and sparse monitor profiles remain
independent regression checks.

The complete Kotlin-declaration sparse-monitor fixture explicitly requests the
bounded 600-second analysis profile. Its default work ceiling also fails on
the pre-change binary; the original compiler inputs remain unchanged. Shared
monitor-return fixtures use the CI Android SDK D8; newer D8 versions may emit a
different cleanup layout before the fixture's structural witness runs.

Private-binary-carrier compiles untouched Java 8 originals and frozen callers
before D8 or recovery. Four static/instance and source/SDK profiles compare 128
fresh JVM cases with two attempts, all output Java, original entries, metadata
and Exceptions. Observers check both input callbacks, unrelated opaque elements,
null inputs/receivers, changing result sources, captured return identity, sticky
class-initialization failures and later caller casts. Seven wrong programs must
fail. Native cases cover all mixed input variances, range forms, raw synthetic
accessors and either returned input; 64 mutations reject foreign/aliased private
calls and incomplete bounds, carriers, scopes or dispatch contracts.

Protected-self-final retains untouched Java 8 compiler originals with two final
stores inside a witnessed D8 catch-all interval, alongside earlier and later
final stores. Four narrow/wide and source/SDK profiles compare 64 fresh JVM
cases with two attempts, every recovered Java source, all original entries,
metadata and Exceptions. Frozen observers inspect the published partial object
at every getter and cleanup, allocation defaults, changing getter sources, raw
long words, original/replacement failures, Error cleanup and sticky class
initialization. Eight wrong programs must fail. Native cases vary provider
static/instance dispatch, source/SDK declarations, range calls, this aliases,
word widths and traversal order; 32 mutations reject identity, store-count,
handler, frame and normal-exit changes.

Raw-scalar-tail-bridges uses independently authored Java 8 originals whose erased
entry casts and invokes an effectful final carrier getter before forwarding to a
differently named scalar target with a complete closed generic tail. Only the
entry's bridge/synthetic access bits are encoded; every Code attribute remains
unchanged and both original forms execute before recovery. Four source/SDK and
32/64-bit profiles each compare 80 fresh JVM cases with two attempts through
interface, frozen caller, reflection, method handle and direct routes. Observers
check exact scalar bits, mutable getter state, callback aliases, changing return
identity, all throwing stages and null/cast failures. Every Java source, original
definition, full Signature and Exceptions are checked; only the disclosed source
flag difference is permitted. Two complete-tail metadata mutations reject and
eight independent wrong programs fail the same observers. Native cases also
cover all eight scalar types, range forms, invalid source target spellings and
reversed declaration order; 28 mutations reject changed conversions or contracts.

Binary-callback-carrier compiles untouched Java 8 originals and frozen callers
before recovery. Four invariant/covariant and source/SDK profiles each compare
96 fresh JVM cases with two attempts, every generated Java source, all original
entries, complete metadata and Exceptions. The observer checks two input reads,
both callback arguments, mutable aliases, partial publication, null/cast errors,
Boolean unboxing, provider-selected result identity, later caller casts and
sticky initialization/Exception/Error behavior. Eight wrong programs fail the
same observers and six metadata-only signature mutations must reject recovery.
Native cases vary all input/callback variances, fixed result declarations, range
forms and traversal order; 32 mutations reject incomplete scoped relations.

The binary-callback-carrier runner's `--key-selector` profile instead preserves
two independent method formals and an invariant carrier of the original input.
Four source/SDK and invariant/covariant profiles each compare 100 fresh JVM
cases with two attempts, every Java source, original entry, full metadata and
Exceptions. An unrelated opaque key must reach the original late Boolean cast
after publication and the provider's postlude; the same observers also check
the original input reads, selected result, aliases, sticky initialization and
throwing stages. Eight wrong programs fail and six metadata-only role changes
reject. Native cases vary declaration order, formals order, input/callback
variances, range calls and SDK declarations; the original mapped-result proof
is checked independently, and 32 incomplete scope/role mutations reject.

Nested-conditional-tail uses untouched Java 8 compiler originals with an outer
new, an inner conditional/fallback argument program, then an opaque wide-return
call and another volatile outer argument. Four source/SDK and branch-polarity
profiles compare 112 fresh JVM cases with two attempts, all emitted Java and
every original entry, complete metadata and Exceptions. Frozen observers check
class initialization before field reads, dynamic fallback values, partial
objects around final stores, captured wide values before later mutations,
argument order, original throwable identity and sticky initialization failure.
Ten independent wrong programs must fail. Native cases cover all scalar and
reference tail types, range frames, inner word widths, SDK declarations, argument
positions and traversal order; 32 mutations reject receiver, CFG, access, entry,
handler, word and metadata changes. Existing conditional and nested-allocation
regressions remain separate checks.


`test_mobile_android_reference_array_super.py` compiles owned Java 25 constructor
prefixes that allocate and fill reference arrays before a generic superclass
call. Source and SDK declaration profiles each compare 92 fresh JVM cases with
two attempts, compile every emitted Java unit and retain complete original
metadata and Exceptions. Frozen observers witness distinct ordered elements,
array component identity, zero and negative lengths, null receivers, partial
final stores, exact errors and failed class initialization. Eight signature-only
mutants and ten independent wrong programs challenge the contract and observer.


`test_mobile_android_covariant_array_super.py` retains a nested
`Map.Entry<? extends K, ? extends V>` component in the same original reference
array construction loop and superclass call. Source and SDK profiles each
compare 92 fresh JVM cases with two attempts, all generated Java25 units,
original definitions and full metadata/Exceptions. Eight signature-only mutants
reject mismatched scope, variance, raw components and unused formals. Ten
independent wrong programs challenge ordered allocations, reference identities,
array effects, partial stores and exact failures using unchanged observers.


`test_mobile_android_allocation_int_shifts.py` compiles all output for source
and SDK declaration profiles with register or post-initialization field inputs.
Each profile compares 64 fresh JVM cases with two attempts and complete original
metadata/Exceptions. A frozen bit oracle uses repeated unsigned-long arithmetic
to check signed-int left, arithmetic-right and logical-right shifts with masked
negative and large distances. Nine independently compiled wrong programs
challenge distance width, sign, input snapshots, literal values and ordering.


`test_mobile_android_inherited_generic_allocation.py` retains descriptor-only
protected final array/scalar calls through a complete unchanged generic class
chain in the same runtime package. Source and SDK profiles each compile every
Java8 output and compare80 fresh JVM cases with two attempts, full metadata and
Exceptions. Frozen observers witness two original array snapshots, receiver and
index mutation, allocation/class initialization order, partial final fields and
exact errors. Six signature-only mutants and eight independent wrong programs
challenge access admission, contract completeness and runtime observations.


`test_mobile_android_paired_plain_array.py` retains a private static method's
independent Object-bounded seeds beside a plain Object array and scalar context,
returning the original plain array descriptor. Source and SDK profiles each
compile every Java8 output and compare72 fresh JVM cases with two attempts,
original metadata and Exceptions. Frozen observers witness input/result aliases,
independent/null seed values, ordered partial array writes, target initialization
and exact throws. Six signature-only mutants and eight independent wrong
programs challenge the binding and observer.


`test_mobile_android_paired_array_indices.py` retains independent scalar
auxiliaries beside both generic seeds and the closed array context/result.
Source and SDK profiles each compile all Java8 output and compare72 fresh JVM
cases with two attempts, full original metadata and Exceptions. The frozen
observer witnesses both index roles, partial writes, arrays and throwable
identity. Six signature-only mutants and eight wrong programs include reusing
the first index for the second store. Native cases cover zero, two, three and
eight scalar auxiliaries, all scalar word types, declaration scopes and frames.


`test_mobile_android_concrete_super_resolution.py` retains a lexical superclass
call through two nearer classes with abstract interface slots and a farther
concrete class declaration. Source and SDK profiles each compile all Java8
output and compare56 fresh JVM cases with two attempts, complete original
metadata/Exceptions and helpers. Frozen observers witness receiver, argument
and boolean-result snapshots, initialization, partial commits and exact throws.
Eight independent wrong programs challenge virtual recursion, receiver changes,
field-write order and delayed argument/result reads. Native controls retain the
nearest abstract override and reject malformed ancestry, descriptors and frames.


`test_mobile_android_independent_array_seeds.py` retains a unary direct formal
next to a plain Object-array input and result. Source and SDK profiles compile
all Java8 output and compare72 fresh JVM cases with two attempts, complete
metadata, Exceptions and helper inventories. Frozen observers distinguish
input/result aliases, null seeds, short arrays, ordered writes, partial effects,
class initialization and exact throws. Six signature mutants and eight
independent wrong programs challenge the binding and observations. Native
coverage includes one, three, four and eight independently bound formals, zero
or multiple scalar arguments, wide frames and source/SDK declaration order.


`test_mobile_android_scoped_constructor_delegation.py` retains an original
owner-formal constructor argument beside two mutable static field snapshots.
Source and SDK profiles compile all Java25 output and compare64 fresh JVM
cases with two attempts, complete original metadata, Exceptions and helpers.
Frozen observers witness direct, separately compiled, reflective and method
handle entry, nulls/aliases, initialization order and sticky initialization
failures, partial final fields and exact throwable identity. Six signature
controls and eight wrong programs challenge binding and snapshot preservation.
Native coverage includes multiple owner formals, closed scalar/reference/array
contexts, nominal upcasts, wide frames, field order and SDK declarations.


`test_mobile_android_formal_getter_super.py` retains a receiver-formal getter
result passed to a generic superclass constructor beside a separate key and
nested Map contract. Source and SDK profiles compile all Java25 output and
compare72 fresh JVM cases/two attempts, full original metadata, Exceptions and
helpers. Frozen observers witness four entry routes, null keys/results/receiver,
mutable getter state and result snapshots, initialization failures, partial
final fields and exact throwable identity. Eight signature controls and eight
wrong programs challenge receiver substitution, getter execution and snapshots.
Native cases retain exact formal identities across receiver arity, selected
formal, input order, source/SDK, invocation frame and linking order.


`test_mobile_android_closed_getter_upcast.py` compiles an original Java25
constructor that snapshots two getters and checks a generic interface cast
before its superclass call. Source/SDK profiles compile every generated unit
and compare88 fresh JVM cases/two attempts with full metadata, Exceptions and
helper inventories. Frozen observers witness getter order, result and receiver
identity despite mutable fields, nulls, class initialization, partial final
fields and exact errors. Eight signature controls and eight wrong programs
challenge generic ancestry and snapshot retention. Native coverage varies
receiver formals, superclass depth, read order, SDK and linking/invocation modes.


`test_mobile_android_scalar_number_bridges.py` uses unmodified Kotlin compiler
output for an AtomicInteger subclass with thirteen original methods and ten
scalar bridges. It compiles every Java8 output and compares80 fresh JVM cases
with two attempts through separate callers, Number, reflection and method
handles. Frozen independent oracles check mutable values, signed byte/short
conversions, long returns and float/double bits at rounding boundaries. Every
original method and full metadata/Exceptions remain except the explicitly
reported bridge flag. Eight wrong programs challenge recursion, stale values,
wide words, signed conversion and floating-point precision.

`test_mobile_android_inherited_descriptor_default.py` compiles a Kotlin parent
in `all` mode and its child in `all-compatibility` mode. The original child
accessor invokes an inherited default through its own symbolic owner; the
parent has no static compatibility entry. Source/SDK profiles compile every
generated Java unit and compare104 fresh JVM cases with two attempts through
virtual dispatch, DefaultImpls, reflection and MethodHandle calls. Immutable
observers check receiver/input/result identity, nulls, partial effects and
exact failures. Eight wrong programs challenge default selection and effects;
native cases also vary range words, intermediate interfaces, initialization
and input order, and reject incomplete or changed declaration contracts.

`test_mobile_android_array_consumer_carrier.py` checks an original array,
contravariant comparator and invariant result carrier through five call routes.
Source/SDK profiles compile every generated Java unit and compare115 fresh JVM
cases twice, including covariant array stores, mutable callback state, class
initialization and exact partial failures. Frozen observers also check the
result's snapshot fields, scalar value and reference identity. Eight same-Code
Signature controls and twelve wrong programs challenge the complete relation.

`test_mobile_android_closed_interface_super.py` uses unmodified Java8 compiler
output for a named static member override with a conditional allocation and a
nested-interface default call. Source/SDK profiles compile every generated Java
unit and compare80 fresh JVM cases twice across five call routes. Immutable
observers check self/input/result identity, snapshots, partial constructor
publication and exact runtime/error failures. Nine wrong programs challenge
lexical dispatch and evaluation order. Native cases add nominal and wide
parameters, complete interface diamonds, range calls and static member scopes.

`test_mobile_android_generic_default_fold.py` uses genuine Kotlin compatibility
output with a method-owned Object formal, a direct seed and a complete
mixed-variance binary callback. Source/SDK profiles compile every generated
Java unit and compare104 fresh JVM cases twice through virtual, DefaultImpls,
reflection and MethodHandle calls. Frozen observers distinguish overriding
receivers, track original seed/self/result snapshots and check exact partial
runtime/error failures. Nine wrong programs challenge dispatch and effects;
native controls reject missing contracts, wrong scopes and changed word frames.

`test_mobile_android_private_nested_carrier.py` compiles an original generic
owner with a shadowing private static method and an invariant private nested
carrier. Source/SDK profiles compile every generated Java unit and compare75
fresh JVM cases twice. Frozen observers inspect distinct class/method formals,
opaque payloads, original allocation identity, callback mutations, result
selection and exact partial failures. Four same-Code Signature controls and
twelve wrong programs challenge scope, casts, snapshots and effects.

`test_mobile_android_captured_closed_super.py` uses genuine Kotlin value-class
and suspend-lambda output with three captures before a closed generic parent
constructor. Debug/release D8 profiles compile every emitted Java unit and
compare60 fresh JVM cases twice through four invocation routes. Frozen observers
check receiver, capture and result identity, holder snapshots, continuation
ownership, suspension/resumption and exact runtime/error failures. Complete
definition and metadata accounting checks each disclosed local/method rename
and generic projection; eight wrong programs challenge the observer.

`test_mobile_android_interface_seed_visitor.py` checks a direct interface-bounded
seed and a complete visitor returning a public nested enum. Public source/SDK
and owned private-body profiles
compile every generated Java unit and compare64 fresh JVM cases twice across
four call routes. Frozen observers check overload selection, class initialization,
input/result snapshots, invalid raw callback results and partial runtime/error
failures. Eight same-Code Signature controls and twelve wrong programs challenge
the relation. Original declarations and metadata are independently accounted;
the reported enum compiler boundary separately records its exact backing-array
helper and source synthetic-flag differences.

`test_mobile_android_trailing_allocation_abort.py` uses original Java14 nested
constructor and switch output with terminal throwing arms and reused abandoned
object words. Source/SDK profiles compile all generated Java and compare64
fresh JVM cases twice across four routes. Frozen observers check class
initialization, getter/style snapshots, exact float bits, partially published
constructor objects and runtime/error identities. Twelve wrong programs
challenge ordering and snapshots; native controls add both physical throw-tail
layouts, wide words, register frames, reentry and uninitialized-value escapes.

`test_mobile_android_open_scoped_mask.py` uses genuine Kotlin output from a
public open generic owner with a raw synthetic mask entry. Source/SDK profiles
compile every recovered Java unit and compare96 fresh JVM cases twice across
four constructor routes. Frozen observers check NaN and negative-zero bits,
opaque/null/array payload identity, scoped reflection types, partially published
fields and exact runtime/error failures. Four same-Code Signature controls and
twelve separately compiled wrong programs challenge the proof and observer;
complete metadata accounting isolates the disclosed synthetic flag and D8's
independently verified CLASS annotation loss.

`test_mobile_android_captured_owner_super.py` uses genuine Kotlin inner-class
output with two distinct enclosing formals and a shadowing parent formal.
Source/SDK profiles compile every recovered Java unit and compare72 fresh JVM
cases twice across four routes. Frozen observers inspect the original enclosing
capture during the parent constructor, null enclosing references, float bits,
payload identity and partial runtime/error failures. Four same-Code Signature
controls and twelve wrong programs challenge scope and behavior. Complete
metadata accounting checks the static member, synthetic capture flag, omitted
constructor parameter and each disclosed Object projection independently.

`test_mobile_android_constructor_instance_reads.py` uses unmodified Java8 copy
constructors reading private instance fields and public SDK-declared fields.
Source/SDK profiles compile every generated Java unit and compare56 fresh JVM
cases twice across four routes. Getters and delegated constructors mutate source
objects and caller globals; frozen observers check original snapshots, every
primitive width, raw floating bits, null receivers, array/reference identity
and partial final-field initialization. Thirteen wrong programs challenge the
observer. All definitions, declaration metadata and Exceptions remain exact;
native controls reject malformed fields, inaccessible declarations and stale
uninitialized-self aliases in wide destination words.

`test_mobile_android_constructor_comparisons.py` uses original Java8 long,
float and double comparisons plus effectful conditional allocations in a
constructor chain. Source/SDK profiles compile all generated Java and compare64
fresh JVM cases twice across four routes. Explicit golden tables check long
boundaries, NaNs, signed zeros and infinities; frozen observers check input bits,
getter counts, lazy allocation identity and partial runtime/error failures.
Thirteen wrong programs challenge the observer. Native controls check all five
comparison opcodes, six zero relations, malformed operands and hidden wide
uninitialized-self words using the same scalar receipt as ordinary statements.

`test_mobile_android_final_diamond_aborts.py` uses original Java8 constructors
with two final-assignment arms and earlier/later explicit exception paths.
Source/SDK profiles compile all generated Java and compare128 fresh JVM cases
twice across direct, reflection and two MethodHandle routes. Frozen observers
check partial final fields, original getter counts and snapshots, volatile long
bits, nulls, aliases and exact runtime/error identity. Fourteen wrong programs
challenge the observer. All definitions, metadata and Exceptions stay exact.
Native controls cover all ten field types, inline and trailing throws, static
initialization, exact tail entries and repeated writes on an abrupt path.

`test_mobile_android_allocation_prefix_copies.py` uses genuine Java8/D8 copies
inside two constructor allocations for all ten input types. Source/SDK profiles
compile every emitted Java25 unit and compare64 fresh JVM cases twice across
direct, reflection and two MethodHandle routes. Explicit bit patterns and frozen
callers inspect independent original inputs, both allocation identities, partial
final fields and exact runtime/error identity. Fourteen wrong programs challenge
copy origin, snapshots and ordering. Native controls cover all nine copy forms,
wide overlaps, hidden uninitialized words, SDK declarations, captured scopes,
malformed metadata and inaccessible constructor targets.

`test_mobile_android_padded_constructor_allocation.py` uses independently authored
smali with six exact empty instructions before or between six reference copies.
It compares48 fresh JVM cases twice against frozen owned Java, including nulls,
aliases, callback array mutation, partial construction and exact failure identity.
Ten wrong programs must fail before recovery. Both layouts compile every emitted
Java25 unit and retain all input definitions and metadata except the reported
class-file version and throw helper. Native cases cover all copy types/forms,
linked declarations, malformed padding, word overlaps and illegal entry edges.

`test_mobile_android_producer_consumer_factory.py` compares48 fresh JVM cases
twice through direct, reflection and MethodHandle routes. Owned Java uses a
public inherited static method on a package-private provider, covariant input,
contravariant comparator and invariant list result. Source/SDK profiles compile
every Java8 unit and compare complete original metadata and Exceptions except
reported helpers. Frozen observers check iterator reads, nulls, aliases, callback
counts, mutation between reads, actual result mutation and exact failures;
ten wrong programs run before recovery. Native cases also cover all four input
variance combinations, SDK declarations, facade ownership, register frames,
formal renaming and rejection of incomplete or malformed relations.

`test_mobile_android_discarded_reference_result.py` encodes an owned final void
adapter's JVM name/synthetic flag and its caller's constant-pool reference while
checking every compiler Code attribute is byte-identical. Both source/SDK paths
compare80 fresh JVM cases twice, run10 wrong programs before recovery, and compile
every Java8 unit. Frozen observers cover owned calls, reflection, MethodHandles,
interface dispatch, char code units, null receivers, adapter frames, partial
writes and exact returned/failure identities. Original metadata and definitions
are compared except the disclosed void name/synthetic flag and reported helpers.
Native coverage lives in `MobileDalvikDiscardedResultTests.cpp`, including scalar
word layouts, SDK contracts, nested scopes and malformed forwarding/ancestry.

`test_mobile_android_nested_unary_carrier.py` compares60 fresh JVM cases twice
through owned calls, reflection and MethodHandles. A private generic carrier
contains the public static factory class; its instance method has an independent
formal shared by a public input carrier and private result. Source/SDK input
profiles compile all Java8 units and compare complete metadata and Exceptions
except reported helpers. Ten wrong programs challenge getter snapshots,
allocations, partial final fields, actual/null/replacement results, runtime casts,
null receivers and exact failures. Native controls keep class/method scope
ownership and access boundaries distinct, including the separately supported
raw receiver on an unused generic owner. Foreign-only and mixed caller sets in
the same or another package cannot borrow the internal lexical access receipt.

The source wrapper overloads additionally exercise ordinary numeric conversions
before this-delegation. Native linear controls check all15 conversion opcodes,
narrow/boolean input promotion, SDK getter continuation, metadata and implicit
wide receiver words using the ordinary cast renderer and shared origin proof.

`test_mobile_android_floating_constructor_prefix.py` witnesses all ten original
float/double arithmetic families and both D8 address forms. Source/SDK profiles
compile every emitted Java25 unit and compare64 fresh JVM cases twice across
direct, reflection and two MethodHandle routes. Frozen original numeric outputs
retain finite values, overflow, zero signs, infinities and explicit float rounding;
arithmetic NaN payloads are normalized. External callers also assert getter and
exception ordering, context/name identity and partial final-field state. Fourteen
wrong programs challenge operator selection, integer-zero division, narrowing,
reassociation and effects. Native controls verify physical source/destination
words against canonical and aliased uninitialized receivers in prefix/allocation
regions, plus exact opcodes, metadata, SDK declarations and register frames.

`test_mobile_android_synthetic_interface_roots.py` compiles unmodified Kotlin
abstract/default synthetic interface entries, two implementations and real
interface/subinterface/concrete callers. The source and SDK-callback profiles
compile every recovered Java unit, compare every definition and metadata/
Exceptions against the exact reported name/flag changes, and verify D8 CLASS
annotation loss independently. Frozen callers compare96 fresh JVM cases twice
using original and reported spellings, checking full long/float word positions,
raw NaN/zero bits, dynamic/default dispatch, nulls, identity and exception timing.
Fourteen wrong callbacks challenge the observer; an unchanged SDK root cannot
obtain a source spelling. Native controls retain each former valid input and
check abstract/default roots, flag combinations, full generic scope, unknown
ancestors, malformed bodies, bridge/native/access flags and enclosure metadata.

`test_mobile_android_named_default_accessors.py` composes genuine Kotlin parent
and compatibility-child output with non-Java method names. Both source and
SDK-callback profiles compile every generated Java unit, compare all original
definitions and metadata/Exceptions against reported name/flag/helper changes,
and independently check D8 CLASS annotation loss. Frozen observers compare96
fresh JVM cases twice across genuine virtual calls, compatibility forwarders,
reflection and exact MethodHandle static dispatch, checking wide/scalar words,
receiver/token identity, raw float bits, nulls and partial failures. Fifteen wrong
callbacks and a virtual-redispatch counterexample challenge the observer. Native
controls compose name and lexical-super receipts across return carriers, range
forms, interface chains, initializers and input order, retaining all22 rejection
cases with both original and non-Java names.

`test_mobile_android_closed_callback_default_accessors.py` adds an unchanged
closed contravariant callback Signature to genuine Kotlin parent/compatibility
output. Both profiles compile all Java and retain every definition and complete
metadata/Exceptions except individually reported names, flags and helpers.
The frozen96-case observer runs twice and checks virtual versus lexical default
dispatch, all wide/scalar words, raw float bits, receiver/callback identity,
partial trace effects and callback failure identity. Twenty independent wrong
programs must fail. Native controls check source/SDK callback declarations,
interface chains, initializers, range forms and input order; open formals,
unknown types, changed Signatures, throws, access and forwarding remain rejected.
The shared inheritance selector owns admission; Body consumes its exact
original-call to declaration receipt without inferring another type contract.

`test_mobile_android_encoded_static_prefix.py` uses genuine javac/D8 encoding to
retain explicit typed preparation zeros before runtime final-field writes. Both
source and SDK-callback profiles compile every Java unit and compare the full
definition inventory and metadata/Exceptions, accounting for the reported
non-inlining helper of the encoded anchor constant. Frozen observers run96 fresh
JVM cases twice across mask combinations and compiled, reflective, MethodHandle
and no-argument entry, checking static initialization/reentrancy, typed defaults,
raw float/double bits, token identity and partial failures. Sixteen independent
wrong programs challenge the observer. Native controls compose512 type, field
owner, original write, guard, range, frame and order cases; SDK ConstantValue,
nondefault/negative-zero/NaN bits, access violations and malformed original
writes remain rejected. Initialization emission and constructor planning share
the same typed preparation-default predicate.

`test_mobile_android_local_static_prefix.py` retains two genuine Kotlin anonymous
callback singletons in a synthetic constructor's exact lexical scope. Source and
SDK-callback profiles compile every Java unit and compare all original entries,
complete metadata/Exceptions and individually reported shared-member, Signature
and synthetic-flag changes. Frozen observers run128 fresh JVM cases twice across
all masks, reflection, MethodHandle and genuine Kotlin calls, checking singleton
identity across distinct constructed objects, partial fields, callback inputs,
return identity and runtime/error failures. Fifteen independent wrong programs
challenge the observer. Native controls cover64 guard/range/frame/order/entry/synthetic-flag
variants and9 foreign/private/changed-scope cases. Constructor planning consumes
the common lexical visibility proof while source projection owns the class name.

`test_mobile_android_long_constructor_prefix.py` retains genuine Kotlin default
size packing from two interface getters. Source and SDK-interface profiles compile
all emitted Java and compare original definitions, complete metadata/Exceptions,
reported synthetic flag loss and independently inventoried D8 annotation losses.
Frozen observers run 132 fresh JVM cases twice through reflection, two MethodHandle
routes and genuine Kotlin callers. Signed boundary bits, getter order, original
snapshots, partial fields, null receivers and runtime/error identity are checked;
16 independent wrong programs must fail before recovery. Native controls cover 576
operation/address/owner/range/guard/frame/entry cases, six adjacent int shift
controls, 25 malformed or hidden-wide-word cases and three allocation receiver
controls. The exact long opcode/exception receipt and physical operand-word proof are
shared by statements, allocation and constructor preparation; long
division/remainder remain outside the total family.

`test_mobile_android_nominal_super_constructor.py` retains genuine Java8 generic
superclass arguments when the child constructor has only a nominal descriptor.
Source and SDK-parent profiles compile every emitted Java unit and compare every
original definition and complete metadata/Exceptions without adjusting Signature
attributes. Frozen observers run 144 fresh JVM cases twice through reflection,
two MethodHandle routes and a compiled caller. Distinct subclasses, nulls, long
boundaries, reference identity, escaped partial parent/child fields and exact
runtime/error timing remain observable; 16 independently compiled wrong programs
must fail before recovery. Native controls cover 384 declaration, source/SDK,
frame, copy, width, overload and order variants plus 96 unsupported type/origin
cases. The shared generic planner proves the original class formal and exact
incoming descriptor for one fixed-arity target; it invents no method Signature.

`test_mobile_android_guarded_closed_delegation.py` retains a descriptor-only
masked constructor that delegates to two closed wildcard lists, a long and a
float. Its class retains a complete nominal interface header and a no-argument
constructor chain. Source and SDK-parent/interface profiles compile every
emitted Java25 unit and compare all original definitions, complete supported
declaration metadata and Exceptions without adjusting signatures. SDK declaration
classfiles target Java8 from the same sources. Frozen callers run 180 fresh JVM
cases twice through reflection, two MethodHandle routes and direct Java. Every
mask, null/shared/unchecked list identity, subclasses, long/float bits, getter
reads, escaped partial fields and exact runtime/error timing are observed;
16 independent wrong programs must fail before recovery. Native controls cover
512 source/SDK, header, caller-Signature, synthetic, word, range, frame and order
variants, eight raw ancestor controls and 104 open, malformed or unsupported cases. The shared control-flow
receipt owns the prefix; only complete closed parameter and overload contracts
supply source casts, without inventing a caller Signature.

Exact former negative inputs without a synthetic flag or with an original raw
superclass remain positive controls. The complete own constructor consumes no
ancestor formal; the original absent class/constructor Signatures stay absent.
An independently compiled former raw-parent output retains both absences and
the selected closed target Signature. Wildcard inheritance, missing declarations
and unknown bounds remain rejected, and raw ancestry cannot bind a method's
undeclared type variable.

`test_mobile_android_scoped_suspend_forwarder.py` retains genuine Kotlin 2.0.21
JVM8 default interface wrappers and synthetic static bodies with receiver plus
long/double prefixes. Source and SDK-context profiles compile all emitted Java9
(the reported private-interface-helper minimum) and independently compare original definition inventories, supported declaration
metadata and Exceptions. Only reported synthetic flag loss and inventoried D8
CLASS annotation loss are allowed. Four caller routes run 168 fresh JVM cases
twice per profile, observing long/double bits, receiver/context/completion/result
identity, null paths, subclasses, partial effects and exact runtime/error timing.
Sixteen separately compiled wrong programs must fail before original recovery.

`test_mobile_android_closed_nominal_overload.py` preserves unmodified Kotlin JVM8
raw primary/mask constructors and a same-arity closed Function2 overload. External
Java reflection, two MethodHandle routes and a separately compiled Kotlin invoker
exercise all low mask bits, ignored high/sign bits, null/shared/covariant arrays,
objects implementing both interfaces, factory snapshots, escaped partial fields
and exact runtime/error timing. Source and SDK-handler/context profiles compare
every original definition, supported metadata and Exceptions, compile every emitted
Java unit at the reported minimum, and run 240 fresh JVM cases twice. Eighteen
independent wrong programs must fail before recovery; synthetic constructor flag
loss and independently inventoried D8 CLASS annotation loss remain explicit.

`test_mobile_android_fixed_completion_factory.py` retains original Java8 unary
and seeded factory declarations with one/two independent Object-bound method
formals and a fixed completion result. Source and SDK-factory/carrier profiles
independently compare all original definitions, supported metadata and Exceptions,
compile all emitted Java8, and run 168 fresh JVM cases twice through four routes.
Eighteen wrong programs validate callback/receiver/completion identity, independent
returned carriers, exact unary/binary dispatch, optional callback/completion paths,
opaque callback answers, partial effects and return snapshots. A separate genuine
Kotlin compiler study covers both coroutine factory API signatures.

`test_mobile_android_scoped_owner_completion.py` retains a genuine Kotlin private
captured inner class with an unused class formal and a separate method formal.
Source and SDK-interface profiles preserve the original callback/completion
Signature, explicit outer field and constructor descriptor. A frozen observer
checks 184 fresh JVM cases twice through four routes, including sessions created
by another outer object, raw long values, null and unrelated sessions, callback
and completion identity, suspension/aliased results, partial effects and exact
runtime/error timing. Seventeen wrong owned programs must fail before recovery.
All emitted Java25 and original definitions, supported metadata and Exceptions are
checked; existing captured-inner bindings and D8 annotation losses are explicit.

`test_mobile_android_protected_overload_identifiers.py` retains one protected
abstract callback overload with a fixed wildcard Signature and a concrete token
overload on the same declaring class. Source and SDK-base profiles compare 368
fresh JVM cases twice through four routes, both before/after a spelling-only JVM
name encoding and after recovery. Dual callback/token objects, inherited and super
calls, raw long/float values including NaN payloads, null inputs, partial fields,
and exact runtime/error identities expose overload and dispatch mistakes.
Eighteen wrong programs include a Signature-only drift with unchanged Code.
Every original definition, supported metadata and Exceptions is independently
checked, all emitted Java8 compiles, and each overload has its exact root receipt.

`test_mobile_android_package_owner_constructor.py` reads a same-package non-public
provider before a checked cast and primary-constructor delegation. Source and
SDK-provider profiles compile every emitted Java25 unit, compare every original
definition, supported metadata and Exceptions, and run 300 fresh JVM cases twice
through reflection, two handles and direct bytecode. Eighteen wrong programs
validate one-time class initialization and cached failures, cast timing, public
factory controls, a provider mutation after its read, escaped partial final fields,
callback snapshots, constructor continuations and exact runtime/error identity.
A separate genuine Kotlin private-object study retains the original concrete
provider-to-interface cast. Foreign, private and nested owner scopes stay guarded.

`test_mobile_android_generic_terminal_void.py` retains genuine Kotlin 2.0.21
Nothing lowering: a method-owned Object formal couples Key<T> and T on a Void
terminal target, with a raw void forwarding bridge. Source and SDK-key/base
profiles compare all definitions, supported metadata and Exceptions, compile
every emitted Java8 unit, and run 128 fresh JVM cases twice through four routes.
Eighteen wrong programs verify normal-return rejection, exception type/message/
cause/freshness, exact target/bridge stack structure, raw bridge generic metadata,
operand probes and a Signature-only drift whose Code remains unchanged. The
original target Signature and raw bridge Signature stay intact; the source name
and irreducible bridge flags have explicit receipts.

The allocation statement runner's `--instance-marker` profiles verify instance
void calls inside constructor arguments with source and SDK declarations. They
retain class initialization and failure ordering, frozen independent callers,
all emitted Java compilation, metadata/Exceptions, and five wrong-program
counterexamples. The existing static and discarded-result profiles remain in
the catalog.

The inherited closed default profiles compile a genuine generic compatibility
child against a separately compiled parent. Their frozen observers cover
boolean results, callback boxing/nulls, receiver aliases, lexical default
selection, partial effects and exception identity across six call routes.
Source and SDK-trace variants compile all generated Java and reject sixteen
independent wrong programs before exercising recovery.

The inherited seeded default profiles retain original method-owned seed/result
formals and both Function2 argument orders across generic compatibility children.
They compare every emitted Java definition, metadata and Exceptions and exercise
240 fresh JVM cases per profile through four frozen routes, plus twelve wrong
programs covering effects, aliases, result casts and virtual redispatch.

The inherited fluent default profiles retain an argument and result on the exact
nongeneric ancestor interface descriptor. Source and SDK-trace variants cover
null, aliased, fresh and invalid results, cast timing, partial failures, override
traps and complete metadata/Exceptions across eighty frozen process cases and
ten independently compiled wrong programs.

`test_mobile_android_allocation_long_division.py` compiles all generated Java in
source and SDK-provider profiles and compares every original runtime entry,
class/method metadata and Exceptions attribute. Frozen compiled callers and
observers run 120 fresh JVM cases twice. A BigInteger oracle checks signed wide
quotients/remainders, minimum-long overflow, zero divisors, narrowed int divisors,
callback snapshots, class initialization order, partial constructors and exact
exception/cleanup identity. Eighteen incorrect programs must fail before
recovery, including discarded zero-divisor operations. Native checks exercise
both forms, source/SDK constructors, high and overlapping words, field reads,
conversions, unused/overwritten results, malformed instructions and the existing
total-only constructor/conditional boundaries.

`test_mobile_android_inherited_static_prefix.py` retains inherited mutable
protected fields referenced through a subclass before this/super delegation.
Source and SDK-ancestor profiles compile every generated Java unit and compare
all original definitions, metadata and Exceptions attributes. Frozen observers
run 112 fresh JVM cases twice and reject 17 wrong programs before recovery.
Signed and wide snapshots, reference identity, reentrant callbacks, class
initialization failures, erroneous-class reuse, partial constructors and exact
runtime/error identity remain observable. Native controls cover scalar/reference
widths, original qualifiers, high/range frames, source/dependency declarations,
hidden fields, incomplete ancestry, access boundaries and malformed instructions.

`test_mobile_android_nullary_nested_carrier.py` verifies complete nested result
bindings in source and SDK-provider profiles, including standard `toSet()` and
`toList()` calls. Ninety-six fresh JVM cases run twice, with 16 independent wrong
programs and 11 Signature-only controls whose Code stays unchanged. The observer
checks initialization and erroneous reuse, callback order and failures, ignored
results, casts, reference identity and standard collection contents. All emitted
Java compiles; original definitions, metadata, runtime roles and Exceptions must
match. Native controls also cover renamed formals, canonical unbounded slots,
source/interface carriers, inherited qualifiers, register frames and ordering.

`test_mobile_android_synthetic_instance_names.py --generic-owner` retains two
class formals and an invariant interface binding while D8 generates its genuine
package-synthetic instance helper. Source and SDK-provider profiles compile all
recovered Java, inventory every original and generated runtime entry, and check
all class/method metadata against the exact disclosed name/flag changes. Eighty
fresh JVM cases run twice with frozen callers and observers; five wrong programs
must fail before recovery. Native checks retain the former class-formal negatives
as positive cases and cover renamed scopes, raw and parameterized children,
source/SDK interfaces, shifted/range frames, ordering, malformed metadata,
unknown ancestry and the original package/declaration/access boundaries.

`test_mobile_android_allocated_nested_carrier_delegation.py` retains a primitive
constructor caller, a fresh nullary allocation and a nested invariant target
parameter. Source and SDK-provider profiles compile every recovered Java unit
and compare original definitions, metadata, roles and Exceptions. Ninety-six
fresh JVM cases run twice with frozen callers; 18 wrong programs must fail
before recovery, and eight Signature-only controls retain unchanged Code.
Callbacks can mutate the collection and external state or throw; observers check
wide argument snapshots, partial construction, class initialization and erroneous
reuse, raw contents, later element casts and actual standard-library allocation.
Native controls cover source/dependency declarations, intermediate ancestors,
renamed formals, range/copy frames, reused incoming words, stale wide reads,
overloads, access and malformed origins.

The same runner's `--superclass` profiles pass the actual new object to an
immediate generic parent. They reuse the frozen caller and behavioral observer,
including the 96-case matrix, 18 wrong programs and eight Code-preserving
Signature controls. Original parent fields and getters remain inherited; source
and SDK-parent profiles inventory every definition and compile all generated
Java. Native tests exercise different and swapped parent/child formal names,
source/SDK parents, register copies and reused scalar or wide input words. Both
original and specialized bounds, access, initializers and overloads must be
proved; malformed superclass edges and stale wide reads remain rejected.

`test_mobile_android_seed_context_throws.py` covers direct generic seeds beside
fixed contexts and actual arrays, with closed declared exceptions. Source and
SDK-provider profiles compile every emitted Java unit and compare all original
definitions, metadata, runtime roles and Exceptions. Eighty fresh JVM cases run
twice; 14 wrong programs must fail before recovery, and eight Signature-only
controls preserve Code. The frozen observer checks unrelated provider results,
ignored calls, late casts, array identity and failures, callbacks, reentrant
writes and exact thrown-object identity. Native checks retain the producer
profile's shared scope and exception guards across fixed/varargs arrays,
source/SDK declarations, renamed formals, register frames and class ordering.

`test_mobile_android_recursive_selector_factory.py` checks single-selector
factories with recursive Comparable keys and invariant carrier results. Source
and SDK-provider profiles compile every emitted Java8 unit and retain all
original definitions, metadata, runtime roles and Exceptions. Ninety fresh JVM
cases run twice through owned eager factories and actual standard-library
`Comparator.comparing` calls, including ignored results. Frozen observers check
initialization and erroneous reuse, callback and comparison timing, reentrant
writes, actual result identity, nulls, casts and exact thrown-object identity.
Fourteen wrong programs must fail before recovery; eight Signature-only controls
preserve Code. Native controls cover independent class/method scopes, source/SDK
carriers, interface owners, inherited qualifiers, variance, renamed formals,
fixed/range frames, ignored results and malformed contracts.

`test_mobile_android_scoped_interface_token.py` checks interface-bounded token
results while generic owners and methods shadow formal names. Source and SDK
profiles compile every emitted Java8 unit and compare all original definitions,
metadata, runtime roles and Exceptions. Thirty fresh JVM cases run twice with
frozen external implementations and observers, checking raw Class tokens,
protected virtual dispatch, private adapters, reentrant context/result changes,
ignored calls, explicit casts and exact failures. Ten wrong programs fail before
recovery; eight Signature-only controls retain original Code. Native controls
cover class/interface bounds, source/SDK declarations, independent outer bounds,
renamed scopes, inherited qualifiers, shifted/range frames and invalid bindings.

`test_mobile_android_reifiable_context_throws.py` checks result-only Object
formals with fixed receiver/Method/array contexts and two closed exception
declarations. Source and SDK-provider profiles compile all emitted Java8 units
and compare original definitions, metadata, runtime roles and Exceptions. Eighty
fresh JVM cases run twice; 14 wrong programs fail before recovery and eight
Signature-only controls preserve Code. Frozen observers check actual results,
context and array identity, callbacks, ignored calls, reentrant writes, later
casts and exact partial failures. Native checks reuse the canonical reifiable
proof across nullary, scalar, nominal, star-carrier and array inputs, while
malformed scopes, throws, access, varargs and physical frames remain rejected.

`test_mobile_android_wildcard_carrier_nominal.py` checks one invariant formal and
one unbounded carrier slot with an independent owned nominal result. Source
and SDK-provider profiles compile every emitted Java8 unit and preserve original
definitions, metadata, runtime roles and Exceptions. Eighty fresh JVM cases run
twice with frozen external implementations and observers, covering optional
callback execution, null inputs, initialization and erroneous reuse, arbitrary
results, reentrant writes, ignored calls, result casts and exact partial failures.
Sixteen wrong programs fail before recovery; eight Signature-only controls keep
Code unchanged. Native checks cover both slot orders, canonical wildcards,
source/SDK and class/interface carriers, inherited qualifiers, renamed scopes,
shifted/range frames and invalid bounds, variance, access and declarations.

`test_mobile_android_closed_covariant_result_context.py` checks a result-only
Object formal beside an opaque `Class<? extends Annotation>` context and original
receiver/array inputs. Source and dependency profiles compile all emitted Java8
units and compare complete definitions, metadata, roles and Exceptions. Eighty
fresh JVM cases run twice with frozen observers checking actual results, context
and array identity, callbacks, late writes, ignored calls and partial failures.
Fourteen wrong programs fail before recovery; eight Signature-only controls keep
Code unchanged. Native controls cover nominal class/interface arguments, source
and SDK declarations, custom carriers, multiple contexts, original throws, alpha
renaming, array/varargs boundaries, lexical scopes and malformed physical frames.

`test_mobile_android_reifiable_array_result.py` checks result-only arrays with
opaque closed contexts and an actual Class/length reflective allocation entry.
Source and dependency profiles compile every emitted Java8 unit and preserve all
definitions, metadata, runtime roles and Exceptions. Frozen observers run 122
fresh JVM cases twice, covering arbitrary and null arrays, primitive/reference
components, original provider and caller casts, ignored calls, negative sizes,
null/void tokens, array stores, callbacks, late writes and exact partial failures.
Twenty wrong programs fail before recovery; eight Signature-only controls keep
Code unchanged. Native controls cover ranks, nullary/scalar/array contexts,
source/SDK entries, inherited static qualifiers, closed throws, varargs, scopes
and invalid descriptors or physical call frames.

`test_mobile_android_array_consumer_array_result.py` checks one shared formal in
array input, unary consumer and actual array result. A generic Number-bounded
owner and Object-bounded methods shadow formal names; both parameter orders are
used. Source and dependency profiles compile every emitted Java8 unit and retain
all definitions, metadata, roles and Exceptions. Frozen observers run 120 fresh
JVM cases twice, including null shortcuts, opaque callbacks, unrelated arrays,
ignored calls, casts, reentrant writes, array-store failures and exact partial
failures. Twenty wrong programs fail before recovery and eight Signature-only
controls preserve Code. Native controls retain the existing void/boolean consumer
proof across source/SDK class/interface carriers, invariant/lower slots, scalar
contexts, owner shadowing, inherited qualifiers and physical frames.

`test_mobile_android_closed_carrier_parent_formal.py` checks an incoming closed
List substituted for a parent Object formal beside an independent generic context.
The child forwards a subset of eight inputs. Source and dependency profiles
compile all emitted Java8 units and preserve definitions, metadata, roles and
Exceptions. Forty fresh JVM cases run twice, checking opaque/aliased/null carriers,
unchecked contents, virtual reads of child defaults, callback and constructor
order, partial initialization, scalar origins and exact failures. Twenty wrong
programs fail before recovery; eight Signature-only controls preserve Code.
Native controls cover nested class/interface carriers, source/SDK declarations,
renamed parent formals, wide words/copies/range frames and competing constructors.

`paired_mapper_collector` independently exercises two mappers and an opaque
three-slot result containing a pair, plus an original standard collector factory
call. Source and dependency profiles compile every emitted Java8 unit, preserving
all definitions, generic metadata, roles and Exceptions. Seventy-four fresh JVM
cases run twice, covering optional callbacks, snapshots, aliases, nulls, ignored
calls, replacement results, original casts and exact failures. Twenty independent
wrong programs fail before recovery; eight Signature-only controls preserve Code.
Native cases cover all three scoped formals, alpha renaming and formal order,
carrier declarations and variance, source/SDK dependencies and original frames.

`generic_interface_final_field` independently initializes three generic interface
finals through opaque providers, including a nested wildcard and a self nominal
argument. Source and dependency profiles compile all emitted Java8 units and
preserve definitions, metadata, roles and Exceptions. Sixty-four fresh JVM cases
run twice, checking ordered partial writes, initialization reentry, aliases,
nulls, unchecked contents, callback result replacement, exact failures and
erroneous-class reuse. Twenty wrong programs fail before recovery; eight
Signature-only controls retain Code. Native tests exercise closed variance and
nesting, source/SDK declarations, static scope, raw signatures, field roles,
effect ownership and original frames. Recheck ordinary generic final fields
and the existing interface initializer families after changing the shared cast.

`interface_constructor_arguments` independently constructs four interface field
values using mixed Z/I/J and F/D arguments, with a different-arity sibling. Both
source and dependency profiles compile every emitted Java8 unit and preserve
definitions, roles, metadata and Exceptions. Sixty-four fresh JVM cases run
twice, checking class initialization, scalar and raw floating bits, partial
objects, receiver escape, callback changes, exact failures and class/interface
reuse. Twenty wrong programs run before recovery; eight metadata controls keep
Code unchanged. Native coverage retains prepared/reused argument registers,
range/high frames, constructor PCs, canonical booleans, scope, roles and words.
Recheck existing interface, boolean and ordinary allocation families after
changing the shared expression preparation grammar.

`paired_array_comparison` independently exercises ordinary and variable-arity
array boolean operations, including a method formal that shadows a different
class bound. Source and dependency profiles compile all emitted Java8 units
and preserve definitions, metadata, roles and Exceptions. Eighty fresh JVM
cases run twice, checking null/empty/aliased/covariant arrays, original array
identities, snapshots, callbacks, mutations, array-store failures and opaque
results. Twenty wrong programs fail before recovery, including varargs packing
and equality shortcuts; eight Signature-only controls preserve Code. Native
cases cover complete method scopes, renaming, varargs/fixed declarations, array
ranks and erasures, source/SDK declarations, used/ignored results and frames.

`direct_formal_transfer` verifies direct own formal inputs/results with original
checked and unchecked exceptions, without auxiliary contexts. Seven routes and
sixteen modes run twice in fresh JVMs for source and SDK profiles. They retain
opaque result replacements, nulls/aliases, callbacks, checked exception timing,
exact errors, array effects, original narrow casts and ignored result behavior.
An escaping constructor stores its original class token after the call; its
callback observes the default field. Twenty wrong programs fail before recovery,
and eight Signature-only controls preserve Code and descriptors. Compile every
emitted Java8 unit and audit all definitions, metadata, roles and Exceptions.
Native cases cover complete formal ownership, closed exception ancestry,
renaming, source/dependency declarations, frames and class order. Recheck direct
context seeds and reifiable result-only exception profiles.

`binary_carrier_array_transfer` verifies two invariant carrier slots and an
explicit array of either own formal, with ordinary and variable arity entries.
Five routes and twenty modes run twice per fresh JVM in source and SDK profiles.
Check null/empty/covariant arrays, original identities, snapshots, callbacks,
carrier mutations, array stores, exact exceptions, ignored results and returned
replacements. Twenty wrong programs fail before recovery, including packing,
copying and returned-input shortcuts. Eight Signature-only controls retain Code
and descriptors while changing scopes, order, ranks or complete carrier arity.
Compile every Java8 unit and preserve definitions, metadata, roles and Exceptions.
Native cases cover method/formal renaming, declaration order, both array slots,
class/interface carriers, source/SDK roles, frames and class order. Recheck paired
array boolean operations and paired mapper collector factories.

`scalar_array_factory` verifies an explicit final generic array after a boolean
and optional wide scalar. Ordinary and variable arity source/SDK profiles compile
all Java8 units and preserve definitions, generic metadata, roles and Exceptions.
Five routes and twenty modes run twice, retaining scalar/array snapshots,
covariant stores, callbacks, exact errors, ignored results and opaque
HashSet/LinkedHashSet/null replacements. Twenty wrong programs fail before
recovery, including packing, copying and invented collection construction;
eight Signature-only controls preserve Code/descriptors. Native cases cover
closed configuration objects, wide words, inherited static aliases, element and
carrier results, frames and malformed final parameter or declaration roles.
Recheck existing array seed and array context factory families.

`reifiable_seed_context` verifies an all-star map beside a direct own formal,
boolean and nongeneric configuration, including a relay without the scalar.
Seven routes and twenty modes run twice in fresh JVMs for source/SDK profiles.
Check original references, nulls, replacements/aliases, map/configuration
snapshots and callback mutations, exact errors, narrow casts and ignored calls.
Twenty-two wrong programs fail before recovery; eight Signature-only controls retain
Code/descriptors. Compile all Java8 units and audit every definition, Signature,
role and Exceptions attribute. Native cases vary seed positions, carrier arity,
context arrays, direct/array results, renaming, scopes, class order and frames.
Recheck reifiable result-only and direct-context closed-exception families.
The direct seed also binds its array result without inspecting an element.
Runtime cases retain Object[]/String[] checks and returned array identities.
Retain earlier direct-seed, reifiable and closed-exception native inputs as
compatibility successes where the shared proof applies. The seeded covariant
auxiliary fixture also compiles and executes four same-Code metadata
controls (result-only, all-star, own-formal and closed invariant tokens) across all 144 observer cases, twice
per variant and profile. Undeclared seed and token formals
remain independent same-Code rejection controls.

`iterable_function_context` retains an invariant `Iterable<T>`, a complete
`Function<? super T, ? extends R>` and a `List<R>` result beside boolean and
wide inputs. Seven routes and twenty modes run twice in fresh JVMs for both
source and SDK profiles. Check iterator/mapper/action identities and order,
nulls, mutations, failures, independent and aliased results, original casts,
ignored calls and inherited static lookup. Twenty-two wrong programs and
eight same-Code Signature controls precede acceptance. Compile all emitted
Java8 units and audit every original definition, role, Signature and Exceptions
attribute. Native matrices vary formals, order, dispatch, contexts and frames;
retain existing covariant producers beside invariant output-formal contexts.

`mapped_array_token` checks a closed Object input, invariant `Class<R>`,
`Function<? super T, ? extends R>` and `R[]` result. Seven routes and twenty
modes run twice in fresh JVMs in source/SDK profiles: opaque input and token
snapshots, callbacks, nulls, actual independent/aliased/covariant/nested arrays,
primitive-array failures, erased and narrow casts, ignored calls and static
aliases. Twenty-two wrong programs and eight same-Code Signature controls
precede acceptance. Compile every emitted Java8 definition and audit roles,
Signatures, Exceptions and helper accounting. Native matrices vary declaration
order, renamed formals, class/interface tokens, all parameter permutations,
call frames and class order; crossed token/callback/result relations stay rejected.

`closed_invariant_seed_context` retains an own formal beside `Context<String>`,
boolean and nongeneric configuration. Seven routes and twenty modes run twice
in fresh JVMs for source and SDK profiles, including array results, snapshots,
mutations, ignored and narrowed calls, relays and exact failures. Twenty-two
wrong programs and eight same-Code metadata controls exercise the observer;
compile every emitted Java8 unit and audit all definitions, Signatures, roles
and Exceptions. Native matrices vary class/interface contexts, seed positions,
formal names, closed nominal arguments, direct/array results and call frames.
The existing seeded covariant auxiliary also compiles and executes its exact
closed invariant token input as another same-Code compatibility variant.

`guarded_constructor_unchecked_getter` retains a public generic varargs factory
with a custom RuntimeException declaration before original this/super delegation.
Six routes and sixteen modes run twice in fresh JVMs for source and SDK profiles.
Observe literal and supplied arrays, opaque/null/class/array results, callbacks,
exact runtime/Error identities, mutations, constructor order and final stores.
Twenty wrong programs precede recovery. Same-Code metadata controls change
exception declarations, ancestry or generic contracts without changing runtime
descriptors. Compile every emitted unit at its reported Java release and audit
all original roles, Signatures, Exceptions and helper accounting. Native tests
vary guarded/range paths, getter result types and closed unchecked hierarchies;
checked, mixed, missing, cyclic and generic exception contracts remain rejected.

`constructor_object_overload` reuses the owned unchecked factory behavior oracle
with a separate Class<?> sibling constructor source. Both source and SDK modes
retain the selected Object delegation, opaque values and callback/constructor
order across ninety-six twice-run JVM cases and twenty wrong programs. Eleven
same-Code controls cover throws plus sibling scope and arity. Audit every
emitted Java unit, original roles, Signatures, Exceptions and generated helpers;
native cases vary Object root metadata, nominal/formal siblings, declaration
order, range frames and null views.

`instance_reifiable_result` keeps generic instance reads, an alpha-renamed
override, a generic Exception-catching entry and a zero-input forwarding entry
in a closed self-carrier owner. Source and SDK profiles run 144 twice-repeated
fresh-JVM cases, twenty-one wrong programs and ten same-Code controls. Observe
receiver/key identity, volatile late results, override/catch order, caller casts,
ignored calls and exact RuntimeException/Error behavior; compile every emitted
Java8 unit and audit all original definitions, roles, Signatures and Exceptions.

The instance-result observer records the actual caught object and checks its
identity. This keeps a move-exception entry after D8 and exercises the same
handler receipt as the targeted fixed-input call, including the null catch path.

`raw_nominal_superclass_constructor` retains Base<Object,Map> argument identities
while a separate Type input is stored after the superclass returns. Source and
SDK profiles repeat sixty fresh-JVM cases twice, checking nulls, aliases,
untouched maps/types, callback mutation and exact constructor failures. Twenty
wrong programs and six same-Code metadata controls precede acceptance. Compile
every emitted Java8 unit and audit all classes, including the methodless marker
interface, plus original method roles, Signatures, Exceptions and helpers.

The `wildcard_carrier_nominal` declared-unchecked source and SDK variants retain
an explicit unchecked exception on the original provider. They compile every
emitted Java8 unit, compare definitions, roles, Signatures and Exceptions, run
80 twice-repeated fresh-JVM cases and sixteen wrong programs, and reject ten
same-Code metadata controls including checked and generic throws.

The array_mapped_carrier source and SDK profiles retain original arrays, mapper
arguments and opaque Set results through ordinary parents and static facades.
They compile all emitted Java8 units and audit roles, Signatures, Exceptions and
helpers; 132 twice-repeated fresh-JVM cases cover read/store snapshots, covariance,
late global/index changes, callbacks, result casts and exact partial failures.
Twenty-four wrong programs precede recovery; ten metadata controls preserve Code.


The raw_nominal_superclass_constructor any/closed wildcard-target source and SDK
variants place a scoped child formal beside an all-star or closed upper/lower Map.
All emitted Java8 definitions, roles, Signatures and Exceptions are compared;
sixty twice-repeated fresh-JVM cases and twenty wrong programs retain untouched
maps, alias/null identities, constructor callbacks and later type stores. Ten
same-Code controls preserve scope ownership and constructor overload guards.


The repeated_array_tokens source and SDK profiles combine Collection<T> and
Class<T> with an opaque volatile T[] result, ordinary parent, static facade and
reversed parameter order. All Java8 units, original definitions, roles,
Signatures, Exceptions and helpers are checked. The 132 twice-repeated fresh-JVM
cases cover Class/collection null order, size failures, late callbacks/results,
mismatched tokens, opaque array identities and ignored/caller casts. Twenty-two
wrong programs precede recovery; ten metadata controls preserve Code.


The seeded_reducer_carrier source and SDK profiles retain opaque three-slot
factory results and owned calls to the actual SDK reducing overload. They
compile all Java8 units and audit definitions, roles, Signatures, Exceptions and
helpers. The 108 twice-repeated fresh-JVM cases cover nullable seeds/operators,
late results, ignored-result casts, no invented operator calls, fresh collector
states, exact accumulator/combiner operands and partial failures. Twenty wrong
programs precede recovery; ten metadata controls preserve Code.


The mapped_collector_carrier source and SDK profiles retain opaque mapped
factories and owned calls to actual SDK mapping. All Java8 units and original
roles, Signatures, Exceptions and helpers are checked. The 126 twice-repeated
fresh-JVM cases observe downstream accessor order, captured functions and
characteristics, mapper/accumulator sequencing, nullable opaque states, late
results and exact failures. Twenty wrong programs precede recovery; eleven
same-Code controls reject crossed inputs, accumulator and result slots.


The grouped_collector_carrier source and SDK profiles compile all Java8 units
and check original roles, Signatures, Exceptions and helper identities. Their
144 twice-repeated fresh-JVM cases include actual SDK grouping, original input
and key identities, downstream accessor order, nullable states, merge/finisher
callbacks and captured identity finish. Twenty wrong programs precede recovery;
twelve same-Code controls reject crossed nested keys, values and outer roles.


The object_array_constructor source and SDK profiles retain all Java8 units,
original constructor roles, Signatures, Exceptions and helpers. Their 108
twice-repeated fresh-JVM cases cover array/Object constructor chains, original
references, late fields and markers, nullable/covariant arrays, partial failures
and exact exceptions. Twenty wrong programs precede recovery and five same-Code
metadata controls reject changed bounds, missing scopes and method-owned arrays.


The guarded_integer_division source and SDK profiles compile all Java8 originals
and every recovered Java25 unit while retaining original roles, Signatures and
Exceptions. Their 108 twice-run JVM cases observe guarded and linear division,
remainder, zero and overflow, original list calls, superclass callbacks, partial
fields, exact failures and prefix words used by the suffix. Twenty wrong programs
precede recovery; six same-Code controls reject unproved generic scopes/arity.

Guarded division also exercises a generic parent in both provider modes. Native
cases retain complete class-formal carrier identities beside computed scalars
and reject changed reference words, unrelated formals and ambiguous targets.


The scoped_null_constructor source and SDK profiles compile every Java8 unit
and retain original definitions, Signatures, Exceptions and helpers. Their 108
twice-run JVM cases check class-scoped null, original scalar/callback references,
late fields, constructor effects, ignored results and exact failures. Twenty
wrong programs precede recovery; six same-Code controls reject missing bounds,
borrowed or method-owned formals and unproved throws.

The scoped null originals also retain the compiler's checked unused copy.
Recovered Java25 prologues preserve that operation; native variants check both
consumed and unused checked nulls, class-formal variances, alpha names and wide
primitive inputs. Original Java8 and recovered class versions are audited apart
from unchanged source metadata, runtime roles and exception declarations.


The raw_binary_carrier_allocation profiles compile every original Java8 and
recovered Java25 unit in source and SDK modes. Their 108 twice-run JVM cases
check raw reference capture, exact wrapper/parent order, partial state, nulls,
original failures, delayed fields and ignored results. Twenty-six independent
wrong programs run before recovery; six same-Code controls reject unproved
bounds, missing formals and borrowed scopes. Native cases vary variance, wide
words, copies, range calls and declaration order, and reject changed origins.


The keyed_map_carrier source and SDK profiles compile all Java8 originals and
every recovered unit while comparing original Signatures, roles, Exceptions and
reported helpers. Their 120 twice-run JVM cases observe ordered callback inputs,
partial effects, opaque result identity, nulls, exact failures and delayed casts.
Twenty wrong programs precede recovery; six same-Code controls reject crossed
collection/mapper/result roles, unbound formals and variant nested result edges.
Native cases vary provider mode, invariant/variant mapper slots, alpha names,
range calls, ignored results, nesting and declaration order.


The nullary_binary_formal source and SDK profiles compile every original Java8
unit and every recovered unit, comparing Signatures, roles, Exceptions and helpers.
Owned private interface helpers require recovered Java9; SDK mode retains Java8.
Their 96 twice-run JVM cases check opaque results, provider actions, original
casts, callback dispatch, exact failures, ignored results and actual JDK identity
functions. Twenty wrong programs precede recovery; six same-Code controls reject
borrowed/variant slots, unknown formals, missing arity and unsupported bounds.
Native variants keep alpha-renamed method scope separate from both owner formals
across class/interface declarations, source/SDK headers and call frame shapes.

Nullary binary native checks also keep a legal unused formal beside a raw result.
It supplies no element binding and does not borrow a class formal; its original
Signature and complete declaring interface remain unchanged.

Legacy closed-tail coverage keeps its repeated method-formal result as a positive
contrast in all source/SDK declaration orders. Other malformed, missing, variant,
inaccessible and mismatched-bound result slots still require rejection.


The merged_mapper_collector source and SDK profiles compile every original and
recovered Java8 unit while comparing Signatures, method roles, Exceptions and
helpers. Their twice-run JVM cases check original mapper/merge references, opaque
provider actions, exact failures, ignored results and late casts. Actual standard
collectors cover duplicate keys, merged values and null removal. Same-Code
controls reject crossed mapped roles, missing declarations and unproved bounds;
independent wrong programs must fail the frozen observer before recovery.


The grouped_map_context source and SDK profiles compile every original and
recovered Java8 unit, comparing Signatures, roles, Exceptions and helpers. Their
100 twice-run JVM cases observe original collection/classifier/downstream
references, scalar flags, opaque actions, downstream state identity, callback
order, failures and delayed map casts. Twenty wrong programs precede recovery;
six same-Code controls reject crossed roles and unproved accumulator bindings.
Native cases vary alpha names, variance, canonical wildcard encodings, provider
modes, ignored results, range frames and declaration order.


The classified_map_context source and SDK profiles compile every original and
recovered Java8 unit and compare Signatures, roles, Exceptions and helpers. Their
140 twice-run JVM cases check direct/unary result context, exact provider choice,
original references, scalar flags, opaque callbacks, partial failures and delayed
casts. Twenty wrong programs precede recovery; six same-Code controls reject
crossed collection/classifier/result roles, variance and unsupported bounds.
Native cases vary source/SDK modes, names, variance, scalar presence, frame shape,
result use and declaration order while retaining the prior map-context profiles.


The iterator_map_context source and SDK profiles compile all original and
recovered Java8 units, comparing Signatures, roles, Exceptions and helpers. Their
100 twice-run JVM cases check null-safe iterable forwarding, original iterator
identity/state, opaque provider actions, exact failures, ignored results and
late map casts. Twenty wrong programs precede recovery; six same-Code controls
reject lost or variant elements, missing nominal declarations and wrong bounds.
Native variants preserve both existing context arguments and the unary form.


The enum_set_token_factory source and SDK profiles compile every original and
recovered Java8 unit and compare metadata, roles, Exceptions and helper inventory.
Their 96 twice-run JVM cases retain exact tokens and opaque provider effects,
including ignored calls and failures, and exercise real JDK EnumSet.noneOf with
valid, empty, null, non-enum and constant-subclass tokens. Twenty wrong programs
precede recovery; six same-Code controls reject broken recursive bounds or
variant slots. Native cases separate same-name owner and method formals.

An already supported instance call remains a positive receiver/metadata contrast.


The unbound_pair_context source and SDK profiles compile all original and recovered
Java8 units and compare Signatures, roles, Exceptions and helper definitions. Their
100 twice-run JVM cases retain exact fixed-context references, iterator actions,
partial failures, ignored results and late casts. Twenty wrong programs precede
recovery; six same-Code controls reject linked inputs or lost independent result
formals. Native cases vary input position, context count and result arity.


The iterator_result_context source and SDK profiles compile every original and
recovered Java8 unit and compare complete metadata, roles, Exceptions and helpers.
Their 100 twice-run JVM cases retain exact input/context references, iterator and
opaque provider actions, failures, ignored results and delayed casts. Twenty wrong
programs precede recovery; six same-Code controls reject variant inputs or lost
independent result formals. Native cases anchor either result slot; the previous
unbounded-input profile retains its newly supported invariant-input contrast.


The iterable_callback_forwarder source and SDK profiles compile every original and
recovered Java8 unit and compare Signatures, roles, Exceptions and helpers. Their
100 twice-run JVM cases retain exact inputs/callbacks, iterator actions, partial
failures, ignored results, provider casts and delayed Collection casts. Twenty
wrong programs precede recovery; six same-Code controls reject lost or variant
interface/element dependencies. Native cases retain private caller restrictions.

Full native validation also retains the bounded converter and pair-consumer
positive groups, guarding dispatch to existing bounded profiles.


The iterator_indexed_result source and SDK profiles compile all original/recovered
Java8 units, preserving Signatures, roles, Exceptions and helper accounting. Their
100 twice-run JVM cases check signed index extremes, exact iterator/provider
calls, declared index failure identity, opaque results, ignored calls and late
String casts. Twenty wrong programs precede recovery; six same-Code controls
reject unproved input/result formals. Native cases include narrow/wide scalars,
closed checked/unchecked exceptions, parameter order and package access scopes.

The already supported all-star result-only case remains a positive contrast.


The collection_list_mapper source and SDK profiles compile all original/recovered
Java8 units and compare metadata, roles, Exceptions and helpers. Their 125 twice-
run JVM cases retain opaque outer/inner lists, exact inputs/classifiers, iterator
actions, partial failures, ignored calls and delayed outer/inner/element casts.
Twenty wrong programs precede recovery; six same-Code controls reject variant,
missing and unscoped result layers. Native cases retain existing direct results.


The recursive_comparable_extrema source and SDK profiles compile all original and
recovered Java8 units and compare complete metadata, roles, Exceptions and helpers.
Their 40 twice-run JVM cases check provider identity/counts, real comparison order,
empty/null inputs, runtime/Error identity, duplicate maximum identity and singleton
non-Comparable objects under the original Object erasure. Ten wrong programs
precede recovery; six same-Code controls reject lost Object or recursive bounds.
Native cases preserve both input variances and the original result descriptor.

The collection_filter_collector source and SDK profiles retain two dependent
collection seeds and a lower callback. Fresh JVM observers check distinct and
aliased seed references, nulls, callback/iteration/addition order, partial
runtime and Error identity, opaque results and delayed List casts. Metadata,
roles, Exceptions and helpers are compared after compiling every output unit;
independent wrong programs and same-Code Signature controls guard the proof.
Native checks include slot order, alpha-renaming, formal order, ignored calls,
range frames and malformed scopes, bounds, carriers and callback variance.

The sorted_entry_map source and SDK profiles compile every recovered unit and
compare metadata, roles, Exceptions and helpers. Frozen JVM observers check
entry/comparator identity, iterator/comparison/key/value order, partial runtime
and Error identity, null/alias cases, ignored calls and the original result
cast. Independent wrong programs and same-Code Signature controls cover lost
calls, swapped pair slots, missing layers, variants and unknown scopes; native
coverage includes inherited facades, range frames, alpha-renaming and input
order, alongside the existing nested pair and envelope transfer groups.

The indexed_list_transfer source and SDK profiles compile every output and
compare metadata, roles, Exceptions, deprecated declarations and helpers.
Fresh JVM observers check exact primitive extrema and seed references, size/get
order, partial runtime/Error identity, nested aliases, null/opaque results,
ignored calls and delayed ArrayList casts. Independent wrong programs and
same-Code Signature controls guard lost calls, scalar changes and both type
layers. Native cases include wide/range frames, scalar position, multiple
primitive contexts, facade lookup, alpha-renaming and source/link order.

The iterable_map_result source and SDK profiles compile every recovered unit
and compare metadata, roles, Exceptions and helpers. Frozen JVM observers check
iterable/key/value identity, iterator/next/map-get order, partial runtime/Error
identity, null results, nested aliases, opaque maps, ignored calls and delayed
HashMap casts. Independent wrong programs and same-Code Signature controls
guard lost calls and incomplete/variant/swapped result layers. Native coverage
includes either/both wrapped result slots, complete producer variance, formal
order, alpha-renaming, facade/range calls and source/link order.

The iterator_map_callback source and SDK profiles cover a map seed beside an
iterator and either one key callback or distinct key/value callbacks. Every
output unit compiles with metadata, roles, Serializable/checked callback
contracts, Exceptions and helpers compared. Frozen JVM observers check exact
references, callback/mutation order, runtime/Error/checked identity, nulls,
opaque results and ignored calls. Independent wrong programs and same-Code
Signature controls guard crossed roles and seed slots. Native variants cover
source/SDK declarations, both callback forms, variance, boolean context,
alpha-renaming, range/ignored calls and source/link order.

The map_unary_result source and SDK profiles compile every recovered unit and
compare metadata, roles, Exceptions and helpers. Frozen JVM observers check
map/key/value identity, get/iterator/next order, partial runtime/Error identity,
nulls, nested aliases, opaque results, ignored calls and delayed ArrayList casts.
Independent wrong programs and same-Code Signature controls cover lost calls,
variance, erased layers and crossed pair slots. Native checks cover either/both
input wrappers, both producer variance levels, formal order, alpha-renaming,
facade/range calls and source/link order, retaining the inverse result tests.

The paired_array_map source and SDK profiles compile every output and compare
metadata, roles, Exceptions and helpers. Frozen JVM observers check exact
array/flag references, covariant and aliased arrays, null/bounds/store failures,
partial writes, runtime/Error identity, opaque results and ignored calls.
Independent wrong programs and same-Code Signature controls guard component
casts, swapped arrays/slots, missing scopes and flag changes. Native coverage
includes inherited static facades, nongeneric parents, declaration/argument
order, alpha-renaming, range/ignored calls and source/link order, retaining
the original array comparison and binary carrier/array transfer groups.

The tail_loop_final source and SDK profiles exercise alternative constructor
final stores followed by a branch-tail loop. Frozen callbacks observe escaped
allocation defaults, committed fields during cloning/loop steps, partial arrays,
late reads, mutations and checked/Error identity through direct, reflection
and method-handle construction. Every output compiles with unchanged final
flags, definitions, metadata and Exceptions; no field-restoration utility is
allowed. Independent wrong programs include delayed stores and dropped final
flags. Native cases cover reference/wide/array fields, shifted PCs, range
calls and source/link order, rejecting skipped/repeated/foreign assignments.

The legacy diamond-tail contrast also checks the now-proven pending-store
constructor entry, preserving final metadata and exactly one source commit per
field. Actual rewrites before abrupt exit and the static-tail contrast remain
negative cases.

The SDK covariance fixture exercises an unchanged two-formal parent returning
a carrier of its first formal through an interface view. Source and dependency
profiles preserve all original class/method roles, scoped Signatures, Exceptions
and helper accounting, and compile every emitted unit at the reported release.
Direct typed, erased interface and method-handle calls cover null/alias results,
late reads, mutations, callbacks and checked/runtime/Error identity. Independent
wrong programs qualify the observer; same-Code Signature controls change the
result or ancestor slots without changing executable behavior. Native contrasts
cover method order, linking, visibility, bridge flags, missing declarations,
competing providers and complete consumer substitutions.

The iterator closed-list-context regression retains one invariant Iterator or
Iterable formal beside an exact String and an independent List<Object> result.
Source and SDK profiles compare all definitions, roles, Signatures and Exceptions,
compile every emitted unit at the reported release, and check five caller routes
including ignored and delayed checked reads. Null/alias results, context identity,
iterator order, late mutations and runtime/Error identities use a frozen observer
qualified by independent wrong programs and same-Code Signature contrasts.

A raw-input contrast leaves the method formal completely unused and preserves
that existing valid invocation profile. Missing, foreign, variant, bounded and
repeated formal inputs remain explicit negative cases for the new relation.

The seeded iterable-map fixture places an exact nested Map seed first and retains
one invariant Iterable plus key or key/value callbacks. Frozen source/SDK observers
check iterator creation and its null/runtime/Error failures before next, callback
and mutation effects. Opaque returned maps may differ from the seed. Native cases
cover both argument orders, flat/nested seeds, scoped alpha names, ignored results,
linking and incomplete/crossed nested or callback slots.

The flag-pair factory fixture preserves boolean and integer/boolean overloads,
exact scalar snapshots, null/invalid/subtype/late HashMap results, ignored calls,
empty-key failure and delayed subtype/key/value casts. Source and SDK profiles
compare every original role, Signature, Exception and helper, compile every emitted
unit, and qualify RuntimeException/Error identity with a frozen observer and
independent wrong programs. Same-Code Signature contrasts retain exact erasures.
Native cases retain the earlier integer profile and check mixed argument order,
facades, ignored calls, complete result slots, scopes and ABI descriptors.

The collection primitive-result fixture retains invariant Collection/List and
unary callback inputs beside an opaque int[] result. Source and SDK observers
check exact references, null and incompatible array types, late results, ignored
calls, original load/store boundaries, writes and RuntimeException/Error identity.
All emitted units compile and preserve original metadata, roles and Exceptions.
Wrong programs qualify the observer; same-Code controls isolate scope and arity.
Native cases cover all eight primitive components, direct/linked/facade calls,
alpha names, ignored results, array ABI mismatches and unproved input contracts.

The collection constructor-choice fixture compares a Java reference, independent
ASM original and recovered units with the same generic API. One allocation
precedes a null-selected empty or Collection constructor. Frozen source/SDK
observers cover class initialization and sticky failures, exact input/result
identity, partial final stores, mutable reads and constructor/handler failures.
An opaque collection rejects invented traversal. Every original entry, role,
Signature and Exception is compiled and compared. Native cases retain both branch
polarities, range/copy frames, ignored results and exact generic declarations.
The former self-typed constructor contrasts for an Object overload and an unused
class formal are valid exact-erasure cases; method formals, projections, foreign
entries, wrong input frames and mismatched exception regions remain rejected.

Constructor-choice scope and argument-count contrasts also run against lazy SDK
metadata. They require the validator's complete constructor input receipt rather
than a parsed Signature. Private constructor access is tested at the member;
class visibility continues to follow the existing package and member rules.

The scoped pair-transfer fixture adds two unrelated class formals to the owned
provider while retaining independent same-named method formals. Source and SDK
observers retain getter mutations, partial effects, opaque map identity and late
casts. Same-Code controls change pair metadata only. Native cases include
self-typed results and reject borrowed, missing, crossed or bounded slots.

The pair-constructor-transfer fixture covers an `int`/`float` allocation followed
by a same-name method-scoped `Map<K,V> -> Box<K,V>` transfer into a selected
`this(Box, Function)` overload. Both source and SDK profiles retain every
original entry and compile all generated Java. Frozen callers observe allocation
failures, opaque factory results, nulls, partial final fields, exact callbacks,
map reads and late casts across direct, reflection and method-handle routes.
Six incorrect programs must fail independently; six metadata-only controls
preserve all original Code bytes. Native checks also cover typed incoming pairs,
wide primitive words, declaration ordering and changed overload boundaries.

The boolean-constructor-default fixture checks false and true delegation with
a complete two-slot class-scoped callback beside an integer overload. Source
and SDK callback profiles preserve original methods, fields and metadata and
compile every recovered unit. Four independent entry routes observe defaults,
direct boolean/integer calls, partial final fields, exact callback/failure
identities and late casts. Six wrong programs and four Code-preserving
Signature controls protect the proof; native checks reject other bit patterns,
wide constants, lost copies, receiver leaks and crossed generic scopes.

The scoped-scalar-pair-factory fixture exercises boolean and mixed I/Z static
factories on a generic class with independently shadowed method formals and an
opaque self result. Source and SDK profiles compile all recovered units and
retain original compiler entries and metadata. Frozen callers observe flag and
capacity snapshots, callback effects/failures, replacement/null results and late
map operations/casts. Six wrong programs and six Code-preserving Signature
controls protect scope and complete-result boundaries. Native cases include
self/other carriers, swapped result order, facades, SDK declarations and ranges.

The scalar-factory-constructor fixture composes incoming boolean, mixed I/Z and
canonical false defaults with an opaque same-scope pair factory before a typed
constructor overload. Source and SDK profiles retain original definitions and
metadata and compile all recovered units at release 25. Frozen independent
callers observe configuration, factory and constructor effects, partial final
fields, exact callback/result/failure identities and delayed map/callback casts.
Six wrong programs and six Code-preserving Signature controls protect complete
factory and target contracts. Native checks cover literal/copied/incoming
configuration, ranges, self/other results and changed scope/ABI boundaries.

The ancestor-super-call fixture verifies D8 keeps the compiler's original
Object.clone ancestor reference through a generic parent. Source and SDK-parent
profiles compile all Java and compare original compiler entries and metadata.
A future Cloneable subclass checks actual runtime cloning, copied final fields
and back references, constructor counts, partial updates, exact failures,
exception causes and delayed casts; a virtual clone override remains bypassed.
Six wrong programs protect dispatch and effects. Dedicated native tests cover
complete generic/descriptor ancestry, native roots and range forms, and reject
nearer overrides or invalid bindings.

The object-pair-scalar-result fixture preserves two direct Object-formal inputs,
a boolean configuration value and arbitrary int results. Source and SDK-provider
profiles compile every Java unit and compare original entries, metadata and
Exceptions. Frozen callers check nulls, aliases, exact input identities, partial
writes, callbacks and exact failures across four entry routes. Six wrong
programs and six Code-preserving Signature controls guard the contract; native
cases include SDK models, alpha renaming, ranges, link order and invalid scope,
access, scalar, result and register-word boundaries.

The allocated-constructor-context fixture delegates initialized generic
collection and context objects to a nested class-formal List parameter and a
fixed nominal slot. Source and SDK-provider profiles compile every recovered
unit and retain original definitions and metadata. Frozen callers check fresh
identities, allocation order, partial final fields, mutable contents, exact
constructor/callback failures and late casts. Six wrong programs and six
Code-preserving Signature controls guard the proof. Dedicated native cases
cover incoming/allocated contexts, generic ancestry, scope, ranges and broken
initialization or register words.

The computed-scalar-allocation fixture places List.size inside a new context
argument before a complete nested class-scoped constructor. Source and SDK
profiles compile every unit and compare original entries and metadata. Frozen
callers observe class initialization before the getter, opaque integer
snapshots, mutations, fresh references, partial fields, exact failures and late
casts. Six wrong programs and six Code-preserving Signature controls guard
behavior and scope. Native cases cover virtual/interface getters, SDK models,
ranges, alpha renaming, copies and rejected declaration/result/live-word changes.

The nested-carrier-factory fixture shadows a class formal with a static method
formal in a complete List/Comparator input and opaque self result. Source and
SDK profiles compile every unit and compare original entries and metadata.
Frozen callers check input identity without inspection, callback order, future
subclass/null/replacement results, allocation counts, exact failures and late
reads/casts. Six wrong programs and six Code-preserving Signature controls guard
the proof. Native cases include allocating bodies, marker ancestry, SDK models,
ranges, alpha renaming and rejected scopes, variants, bounds and incomplete slots.

The nested factory controls also remove the static method formal while keeping
its class formal and all Code bytes. Source and SDK profiles must reject that
borrowed scope at the referenced declaration boundary.

The bounded-pair-scalar fixture keeps Comparable-super-self inputs, a boolean
context and arbitrary int results through source and SDK-provider profiles.
Every recovered unit compiles and original entries and metadata match. Frozen
callers exercise nulls, aliases, exact input identities, callbacks, partial
writes and exact failures while inputs trap comparison and inspection. Six
wrong programs and six Code-preserving controls guard the proof. Native checks
retain two-argument behavior and cover SDK binding, ranges, alpha renaming and
rejected recursive bounds, flags, results, scopes and platform declarations.

The callback-array-factory fixture covers callback-output arrays and invariant
input-formal results, including boolean and default-false varargs overloads.
Source and SDK profiles compile every unit and retain original entries and
metadata. Frozen callers observe function/array identity, nulls, cyclic and
covariant arrays, mutations, opaque results and exact late failures across
144 fresh JVM cases. Six wrong programs and six Code-preserving controls guard
the relation. Native cases cover fixed/variadic flags, SDK declarations, alpha
renaming, ranges and rejected slot, bound, rank, context and word changes.

The typed-variadic-constructor fixture forwards a complete Class<T> token and
the same String array to a boolean constructor overload. Source and SDK-marker
profiles compile every recovered unit and retain original entries, fields,
varargs flags and metadata. Frozen callers check default/explicit flags, exact
type/array identities, null/empty arrays, mutations, partial final fields,
constructor counts, exact failures and late casts across 144 fresh JVM cases.
Six wrong programs and six Code-preserving Signature controls guard the proof.
Native cases include fixed/variadic callers and targets, owner-formal arrays,
ranges, copies, generic siblings and rejected scope or array-word changes.

The scoped-capture-super fixture creates a raw callback from an original
Class<T> token and String array before calling a generic parent constructor.
Source and SDK-parent profiles compile every unit and retain original entries,
fields, metadata and Exceptions. Frozen callers exercise 144 fresh JVM cases
twice through four routes, checking capture/parent order, identities, nulls,
mutations, fresh objects, partial final fields and exact constructor and late
callback failures. Six wrong programs and six Code-preserving Signature
controls guard scope and ABI. Native cases cover synthetic captures, variadic
callers, ranges, alpha renaming, link order and rejected scopes or word origins.

The scoped-null-super fixture passes null to a complete generic parent
parameter before storing a child's boolean and Function-to-Comparable fields.
Source and SDK-parent profiles compile every unit and retain original entries,
fields, flags, metadata and Exceptions. Frozen callers exercise 144 fresh JVM
cases twice, checking exact identities, scalar state, partial final fields,
initialization order, opaque callback results and exact late failures. Six
wrong programs and six Code-preserving Signature controls guard scope and ABI.
Native cases cover scalar widths, null copies, wildcard forms, alpha renaming,
ranges, fixed siblings and rejected origins, bounds, scopes and target flags.

The array-object-scalar fixture calls an E[]/Object/int declaration directly
and through a generic instance's private field-reading helper. Source and
SDK-provider profiles compile every unit and retain original metadata and
Exceptions. Frozen callers run 144 fresh JVM cases twice, observing input
identities, aliases, cyclic/covariant/null/empty arrays, mutations, opaque int
extremes and exact failures. Six wrong programs and six Code-preserving
Signature controls guard the proof. Native cases cover source/SDK declarations,
field and direct inputs, ranges, alpha renaming and rejected bounds, ranks,
independent-Object substitutions, declaration roles and call words.

The closed-bound-super fixture forwards a descriptor-only service input to a
protected interface-bounded generic parent, retaining an independent loader
input. Source and SDK-parent profiles compile every unit and preserve original
entries, fields, metadata and Exceptions. Frozen callers run 48 fresh JVM cases
twice through four routes, checking identities, fresh children, construction
order, partial final fields, opaque callback results and exact late failures.
Six wrong programs and six Code-preserving Signature controls guard the proof.
Native cases cover package access, unused child formals, input order, copies,
ranges, fixed siblings and rejected scope, bound, descriptor and word changes.

Closed-bound superclass controls also reject SDK constructor Signatures whose
parameter or return erasure differs from the original descriptor. The original
Code remains unchanged, and the declaration boundary owns the rejection.

The instance-formal-context fixture retains one method formal in an instance
input and opaque result beside Type/Object contexts and an unchecked Exceptions
attribute. A generic static wrapper retains its original Exception handler and
soft/default-false overloads. Source and SDK-provider profiles compile every
unit and preserve original metadata and Exceptions. Frozen callers exercise
144 fresh JVM cases twice, including virtual overrides, identity/alias/null
inputs, mutations, original handlers and exact late casts/failures. Six wrong
programs and six Code-preserving Signature controls guard the relation.
Native cases cover seed order, ranges, alpha renaming, absent/empty/unchecked
Exceptions and rejected scopes, context erasures and declaration roles.

Existing instance-result regressions retain newly supported direct formal
seeds, including independent all-star contexts, as positive cases with their
original closed parent scope, metadata and unchecked exception declarations.

The enum-token-default fixture retains E extends Enum<E> across Class<E>, an
optional E default and the result beside an Object context. Source and SDK
profiles compile every unit and retain original metadata. Frozen callers run
144 fresh JVM cases twice, including null and mismatched tokens, aliases,
different enum classes, opaque results, mutations and exact late failures.
Six wrong programs and six Code-preserving Signature controls guard recursive
bounds and erasures. Native cases cover source/SDK declarations, independent
platform/method alpha renaming, unused owner formals, ranges and rejected
self bounds, token variance, context substitutions and declaration roles.

The paired-raw-token-context fixture binds Map<K,V> through distinct Class<K>
and Class<V> inputs beside Class<? extends Map> and Object contexts. Its default
overload retains dynamic getClass/HashMap token selection. Source and SDK
profiles compile every unit and preserve original metadata. Frozen callers run
144 fresh JVM cases twice, checking exact identities, nulls, aliases, opaque
maps, callback mutations and late get/cast failures. Six wrong programs and six
Code-preserving controls guard the relation. Native cases cover context and
result slot order, class/interface carriers, ranges, alpha renaming and rejected
raw bounds, arities, duplicate/foreign tokens and declaration roles.

The object-primitive-array fixture retains an Object-bounded input and opaque
byte-array result. Source and SDK profiles compile every unit and preserve
original metadata. Frozen callers run 96 fresh JVM cases twice, checking exact
identities, nulls, aliases, signed bytes, empty arrays, callback mutations,
caller writes and exact late failures. Six wrong programs and six Code-preserving
controls guard scope and erasure. Native cases cover every primitive array
kind, ranges, alpha renaming and rejected input/result scopes, ranks, physical
descriptors, declaration roles and call/result words.

The enum-token-scalar fixture retains Class<E>/E self bounds beside arbitrary
int values. Source and SDK profiles compile every unit and preserve metadata.
Four immutable routes run 96 fresh JVM cases twice with null, mismatched and
non-enum tokens, signed int extremes, opaque enum results and exact late
failures. Six wrong programs and six Code-preserving controls guard the proof.
Native cases retain ignored results, independent owner/platform/method scopes,
ranges and rejected self bounds, token slots, scalar erasures and call roles.

The enum-closed-context fixture compiles every recovered source and preserves
original entries and metadata in source and SDK-provider profiles. Four immutable
routes run 96 fresh JVM cases twice: an opaque Class<E>/String/E provider checks
String identity, null and mismatched tokens, callback writes and exact late
failures, while the real SDK Enum.valueOf checks valid, missing, null, empty,
padded, lowercase, NUL and Unicode names. Six wrong programs and six
Code-preserving controls guard behavior and erasure. Native cases include the
SDK Enum owner, independent recursive class/method formals, ignored results,
ranges and rejected context descriptors, nominal scopes and declaration roles.

The bounded-token-primitive-array fixture retains byte[], Class<N>, ByteOrder,
a Number-bounded result and original IllegalArgumentException declaration.
Source and SDK-provider profiles compile all output and preserve metadata and
Exceptions. Four immutable routes run 96 fresh JVM cases twice, checking array,
token and order identities, nulls, empty arrays, signed bytes, callback writes,
opaque results and exact late failures. Six wrong programs and six
Code-preserving controls guard behavior, scope and erasure. Native cases cover
all primitive array kinds, argument positions, absent/empty/unchecked throws,
method scopes, ignored results, ranges and rejected declarations/call frames.

The inherited-protected-allocation fixture calls a protected instance member of
a generic parent in another package while constructing a real SDK URI. Source
and SDK-parent profiles compile every output unit and preserve original metadata
and Exceptions. Four frozen routes run 96 fresh JVM cases twice with ordinary
and overriding receivers, input traps, null receivers, callbacks and exact
constructor/late failures. Six wrong programs and six Code-preserving controls
guard virtual dispatch, erased descriptors and scoped inheritance. Native cases
cover complete parent chains, caller qualifiers, static caller bodies, ignored
owner formals, ranges and rejected access, receiver and generic edge changes.
Foreign nonpublic superclass edges remain rejected. Two older protected-member
controls now retain valid nonfinal calls with unchanged class/method metadata;
package-only access, rewritten qualifiers and incomplete edges stay negative.

The constructor-static-nominal fixture retains two opaque String factory results
before a captured raw callback and a Base<String> constructor. Source and
SDK-factory profiles compile all output and preserve metadata and Exceptions.
Four frozen routes run 96 fresh JVM cases twice, checking input/result identity,
aliases, nulls, captures, signed indices, flags, partial constructor state and
exact callback/late failures. Six wrong programs and six Code-preserving
controls guard the proof. Native cases reject unknown static return types,
unproved formal scopes, overloads, access changes and argument/result words.

The interface-seed-varargs fixture retains a CharSequence seed/result, independent
String context, original Object[] varargs array and IllegalArgumentException
contract. Source and SDK-provider profiles compile all output and preserve
metadata and Exceptions. Four frozen routes run 96 fresh JVM cases twice with
opaque CharSequences, nulls, covariant/empty/cyclic arrays, aliases, callback
writes and exact late failures. Six wrong programs and six Code-preserving
controls guard scope, erasure and fixed array identity. Native cases cover
plain/varargs entries, ignored results and absent/empty/unchecked throws;
checked throws, borrowed formals, changed arrays and call roles remain negative.

The reused-allocation-read fixture retains a shared string constant and a shared
volatile read. A later argument's class initializer changes that volatile field
and checks that the allocated class initialized first. Source and SDK-target
profiles compile every output and preserve metadata and Exceptions. Four frozen
routes run 192 fresh JVM cases twice, checking identities, nulls, class-init order,
partial constructor state, fresh objects, distinct argument slots and exact
failures. Ten independent wrong programs guard actual behavior. Native cases
cover shared constants,
static/instance volatile reads and invalid producer/access/receiver frames.
All six orders of three distinct string producers are checked independently of
constructor argument order, including jumbo strings, range calls, source and
SDK constructors, retained registers and static final initialization. An
independent D8 witness retains a single string followed by a CharSequence cast
before construction; native cases also cover reference copies and cast targets.
The source profile uses Java 25 for constructor delegation; an optional compiler
JDK also runs its observers while D8 retains its separately selected JDK.
Malformed
values, access, constructor frames and external entries remain rejected.

The recursive-carrier-factory fixture keeps nullary recursive Comparable
factories and the actual SDK reverse comparator. Source and SDK-provider modes
compile every output and preserve metadata and Exceptions. Four frozen routes
run 96 JVM cases twice, observing opaque comparator identities, argument order,
nulls, callback writes and exact early/late exceptions. Six wrong programs and
six Code-preserving Signature controls guard behavior and complete scope,
bound, carrier and erasure proofs. Native cases cover class/method shadowing,
SDK-owner factories, ignored results, range calls and rejected access/roles.

The private-interface-context fixture retains a Calendar reference beside one
private Appendable-bound seed/result. Source and SDK-support profiles compile
all output, preserve metadata/Exceptions and run 96 cases twice through four
frozen entry routes. Observers distinguish receiver/context identities, opaque
replacement results, callback writes and the cast location in two wrappers.
Six wrong programs and six Code-preserving Signature controls reject changed
behavior or relations. Native cases retain either seed position, scalar/wide,
array/all-star contexts, range frames and exact same-class private access.

The binary-closed-context fixture retains two invariant input/output slots and
an independent closed exception mapper. Source and SDK-provider profiles compile
all output and preserve metadata/Exceptions. Four frozen routes run 96 cases
twice with either parameter order, nulls, opaque replacement outputs, mapper
identities and exact partial-effect failures. Six wrong programs and six
Code-preserving Signature controls guard behavior, scopes and complete erasures.
Native cases include covariant producers, separate method scopes, inherited
carrier contracts and rejected dependent/variant contexts or reordered slots.

The array-callback-index fixture retains an opaque int result, unary consumer
and exact formal varargs array, with or without a scalar context. Source and
SDK-provider profiles compile all output and preserve metadata/Exceptions.
Four frozen routes run 96 cases twice, observing null/empty/cyclic/covariant
arrays, unchanged scalar values, callback order, partial stores and exact
ArrayStoreException or callback failures. Six wrong programs and six
Code-preserving Signature controls reject copies, guessed indices and missing
relations. Native cases include invariant/lower consumers and wide range frames.

The enum-interface-key fixture covers inherited default forwarding and an abstract generic interface selector. Source and SDK-contract profiles compile every recovered unit and compare 96 fresh JVM cases twice through direct, separately compiled, reflection and method-handle callers. Receiver, key, token and fallback identity, effects, partial writes and late failures remain observable. Six wrong programs and six Code-preserving signature controls distinguish scope and behavior; native coverage varies class/method scopes, declaration roles, SDK ownership and register frames.

The array-boolean-varargs fixture checks opaque boolean results for the original null, empty, cyclic and covariant arrays through four immutable caller routes. Source and SDK-provider profiles compile every recovered unit and run 96 fresh JVM cases twice, including array-store timing, callback effects, partial writes and exact exceptions. Six wrong programs and six Code-preserving metadata controls guard behavior and scope. Native tests vary static method/class scopes, source and SDK ownership, frame encodings and invalid declarations.

The inherited-static-field enum profile exposes two distinct enum field results through four frozen caller routes, compiles all recovered enum units and rejects three wrong programs. It preserves original input and caller bytes, and retains the existing enum initialization, values-array, order, reflection and dispatch checks. Native coverage includes anonymous and named constant bodies, primitive, wide, reference and volatile fields, local-name collisions and invalid field roles or subtype escapes.

The carrier-nominal-context fixture retains an invariant collection beside File, String and boolean contexts, a File result and a closed unchecked exception. Source and SDK-provider profiles compile every recovered unit and run 96 fresh JVM cases twice through four frozen caller routes. Collection identity and mutation, context values, exact exceptions, partial writes and late File dispatch remain observable without filesystem I/O. Six wrong programs and six Code-preserving metadata controls guard behavior and scope; native coverage varies nominal names, physical words, invariant bindings and exception hierarchies.

The instance-nominal-throws fixture adds an invariant Iterable, SDK enum context and closed unchecked exception to the nominal-result matrix. Four frozen caller routes check 96 fresh JVM cases twice, including null receivers, a derived override, original receiver/argument identity, partial writes and late File dispatch without filesystem I/O. Source and SDK-provider profiles compile every recovered unit with original metadata; six wrong programs and six Code-preserving signature controls remain independent. Existing native static/instance cases share the input-tree authority and cover both legal call frames.

The producer-throws fixture isolates an unchecked declaration from an otherwise identical producer call and covers used and ignored Object results through four immutable caller routes. Source and SDK-provider profiles compile every recovered unit and run 96 fresh JVM cases twice, including null receivers/handlers, derived dispatch, checked callback wrapping, partial writes and late casts. Six wrong programs and six Code-preserving controls guard behavior and scope. A diagnostic twin omits only provider/caller unchecked declarations; native tests cover instance/static exception policies and exact producer scopes. Only owned in-memory reader fixtures execute.

The parameterized-interface-seed fixture preserves a T extends Collection<String> input/result across instance and static calls. Four frozen caller routes compare 96 fresh JVM cases twice, including static-null versus instance-null receivers, derived dispatch, collection identity, unmodifiable writes, stage failures and the provider Collection cast versus the caller subtype cast. Source and SDK-provider profiles compile every recovered unit with original metadata. Six wrong programs and six Code-preserving controls guard scope and behavior; native variants change closed type arguments, interface declarations, physical frames and exception claims. Only owned collections execute.

The interface-token-context fixture keeps a CharSequence-bound Class<T>, independent File context and original nominal varargs array with closed IOException declarations. Source and SDK-provider profiles compile every recovered unit and compare four frozen caller routes across 96 cases twice. Cases distinguish checked failures, array covariance and writes, provider bound casts and later caller casts; six wrong programs and six Code-preserving Signature controls test the observer and binding boundary. Only owned values execute; no filesystem operation runs.

The enum-direct-seed fixture compares 96 cases twice across six owned carrier/JDK caller routes, including enum constant subclasses, other enum families, null, ignored results, argument/result casts and exact effects. Source and dependency-provider profiles preserve compiled metadata; twenty wrong programs and six Code-preserving Signature controls test observer sensitivity and recursive binding. Native variants cover independent owner scopes, dependency declarations and result interfaces.

The array-overload-separation fixture retains selected String[] superclass calls beside Collection<File> constructors. Four frozen callers compare 96 cases twice for source and SDK Base declarations, including fresh/forwarded null or empty arrays, writes, construction order, exact failures and late child fields. Six wrong programs and six Code-preserving Signature controls guard semantics; native variants cover primitive/reference ranks and assignable Object/Cloneable/Serializable contrasts. No filesystem operation runs.

The protected-static-finals fixture compares 96 cases twice across four frozen entry routes for source and SDK-provider recovery. Same-thread callbacks inspect partial static writes; observers verify null results, exact checked/runtime/Error causes, first initialization failure and cached failure on the next access. Six wrong programs and native malformed-CFG/field/frame controls guard the original order and field metadata. Only owned initializer code executes.

The reifiable-array-delegation fixture keeps File-to-Path virtual preparation and Class<?>[] constructor arguments across overloaded File, String and Path entries. Four frozen constructor caller routes compare 96 cases twice with source and SDK Trace declarations. Cases cover null receiver/array/result, aliases and writes, original dispatch, failures during preparation/construction and late casts. Six wrong programs, six Code-preserving Signature controls and native rank/arity/frame/overload cases guard the complete contract. No filesystem operation runs.

The nominal-super-result fixture retains narrowed superclass inputs and independent FileVisitResult outputs. Four frozen caller routes compare 96 cases twice for source and SDK Base declarations, including future-subclass dispatch, original casts, nulls, exact checked/runtime/Error identity and staged effects. Every recovered unit and original metadata entry is compiled and audited. Six wrong programs and six Code-preserving Signature controls guard the observer and binding boundary; native cases cover source/link order, interface inputs/results and physical frames. Only owned code executes.

The throwable-carrier-scalar fixture compares 96 cases twice through four frozen callers for source and SDK providers. It retains three double contexts, opaque double results, lazy Supplier execution and an identical Throwable formal in input and throws metadata. Cases cover checked/runtime/Error identities, null and invalid provider values, original casts, partial writes and raw floating-point bits. Six wrong programs and six Code-preserving Signature controls test observation and scope; native tests cover primitive descriptors, frames, carrier variance and exception metadata. Only owned in-memory code executes.

The throwable-carrier-context fixture keeps two Object contexts, invariant Supplier<X> and a void result with the identical Throwable formal in throws metadata. Four frozen caller routes compare 96 cases twice for source and SDK providers, including nulls, aliases, ordered mutations, original casts, lazy callbacks and exact checked/runtime/Error identity. Every recovered unit and original metadata entry is compiled and audited. Six wrong programs, six Code-preserving Signature controls and native context/variance/frame contrasts guard the shared proof. Only owned in-memory values execute.

The array-transfer-context fixture keeps one scoped T[] input/result beside String and Object[] contexts, including a fixed varargs tail and a short generic relay with an original empty-array allocation. Four frozen callers compare 96 cases twice for source and SDK providers. Observers distinguish input/context aliases, array store failures, opaque or null returned arrays, late component casts, staged writes and exact failures. Every recovered unit and original metadata entry is compiled and audited; six wrong programs, six Code-preserving Signature controls and native scope/frame/context variants guard the proof. Only owned arrays execute.

The throwable-array-carrier fixture keeps independent Object-array and Throwable formals across typed and checked callers. Source and SDK-provider profiles compile every recovered unit and compare four frozen caller routes across 96 cases twice. Cases cover array identity and covariance, opaque/null results, lazy suppliers, exact checked/runtime/Error failures, input casts and late component casts. Six wrong programs, six Code-preserving Signature controls and native formal-order/scope/frame contrasts guard the composed proof. Only owned in-memory code executes.

The throwable-bound-seed fixture keeps a CharSequence-bound direct input/result alongside the independent Throwable producer and throws slot. Source and SDK-provider profiles compile every recovered unit and compare four frozen callers across 96 cases twice. Cases cover mutable input identity, opaque/null results, later subtype casts, lazy/null callbacks and exact checked/runtime/Error identity. Six wrong programs, six Code-preserving Signature controls and native class/interface-bound, formal-order and physical-frame contrasts guard the proof. Only owned in-memory values execute.

The dependent-seed-bound fixture keeps E inside T extends Iterable<E> across context and independently throwing entries. Four frozen caller routes compare 96 cases twice for source and SDK providers, including mutable identity, opaque/null results, lazy callbacks, original exceptions, late subtype casts and fresh context arrays. Unexpected element traversal fails the observer. Every recovered unit and metadata entry is compiled and audited; six wrong programs, six Code-preserving Signature controls and native slot/arity/order/frame contrasts guard the common bound proof. Only owned in-memory code executes.

The closed-map-super fixture retains String/Object parent bindings across a null-selected original generic DefaultMap allocation and the Map constructor beside an int overload. Four frozen constructor routes compare 96 cases twice for source and SDK parents, checking allocation/parent/child ordering, partial writes, aliases, exact failures and later key casts. Every recovered unit and metadata entry, including a methodless marker, is compiled and audited. Six wrong programs, six Code-preserving Signature controls and native closed-edge/overload/CFG/frame variants guard the proof. Only owned maps execute.

The direct-seed-nominal fixture checks one direct method-owned Object seed with an opaque fixed result on a closed generic owner. Source and SDK providers compare 96 cases twice through four frozen callers, including a future override, early and late casts, null receiver/input/result, partial writes and exact checked/runtime/error failures. All generated units and original metadata entries, including a methodless marker, are compiled and audited. Six wrong programs and six Code-preserving method Signature controls guard the boundary.

The boolean-super-result fixture closes two parent formals and forwards a private key-normalization result with two original value references into invoke-super. Four frozen routes compare 96 cases twice with source and SDK parents, observing both opaque boolean values, exact receiver/argument identities, nulls, aliases, normalization and parent partial writes, stage failures and exact checked/runtime/error identity. A future subclass traps virtual redispatch; six wrong programs, six Code-preserving Signature controls and native range/lookup/frame variants guard the proof. All generated sources and original metadata entries, including a methodless marker, are compiled and audited.

The class-throwable-context fixture separates a receiver-owned Object result from a method-owned Throwable factory formal. Source and SDK receivers compare 96 cases twice through four frozen direct/delegated and typed/generic routes, including a covariant future override, lazy producers, nulls, opaque wrong values, context and receiver identity, partial writes, late casts and exact checked/runtime/error objects. All original entries and metadata are compiled and audited. Six wrong programs and six Code-preserving method Signature controls guard both scopes.

The scoped-null-delegation fixture forwards one class-formal value and invariant carrier, supplies an original null for a second class-formal slot, and preserves a subsequent boolean constructor delegation. Four frozen routes compare 96 cases twice with source and SDK carrier declarations for default nulls, identities, aliases, partial writes, constructor/callback order, late casts and exact runtime/error/checked failures. All original entries and metadata, including the methodless carrier, are compiled and audited; six wrong programs, six Code-preserving Signature controls and native zero/nonzero/scope/frame variants guard the proof.

The array-nominal-context fixture forwards one method-owned formal array with a CharSequence context and null String auxiliaries to an opaque fixed String result. Four frozen routes compare 96 cases twice with source and SDK providers, checking original arrays and component types, ArrayStoreException, callback mutation, element/delimiter conversion order, partial writes and exact checked/runtime/error identity. Every original entry and metadata item is compiled and audited; six wrong programs, six Code-preserving Signature controls and native scope/array/word variants guard the input proof.

The comparable-map-transfer fixture retains an invariant Map input and result with independent Object and recursive Comparable method formals plus a boolean argument. Four frozen routes compare 96 cases twice with source and SDK providers, including raw non-Comparable values, null and distinct maps, callback mutation, late result casts, partial writes and exact checked/runtime/error identity. Original compiled entries and metadata are audited; six wrong programs, six Code-preserving Signature controls and native scope/slot/erasure/role/word variants guard the binding.

The direct-seed-raw-carrier fixture separates a generic class formal from a static factory method formal, retaining a direct seed and an independent fixed context alongside the invariant result carrier. Source and SDK profiles each compare 96 cases twice through four frozen routes, checking constructor effects, fixed context identity, nulls, opaque distinct results, late casts, partial writes and exact checked/runtime/error identity. Original entries and metadata are compiled and audited. Six wrong programs, six Code-preserving Signature controls and native scope/erasure/slot/role/word variants guard the proof.

The nested-seed-transfer fixture retains a direct method seed beside List<Source<T>> and an opaque List<Target<T>> result; Target has a closed generic superclass. Source and SDK profiles each compare 96 cases twice through four frozen routes, including raw input contents, callback mutation, distinct and null results, late element/value casts, partial writes and exact checked/runtime/error identity. All original compiled entries and metadata are audited. Six wrong programs, six Code-preserving Signature controls and native declaration/scope/variance/erasure/role/word variants guard both layers.

The seeded-nested-callback fixture retains distinct input and seed formals, a fixed context, a binary void callback and an opaque two-layer result whose inner carrier has a closed generic parent. Source and SDK profiles each compare 96 cases twice through four frozen routes, covering raw/null inputs, callback bridge casts, mutations, partial writes, distinct results, late value casts and exact checked/runtime/error identity. Original compiled entries and metadata are audited. Six wrong programs, six Code-preserving Signature controls and native scope/declaration/variance/erasure/role/word variants guard the ordered roles.

The seeded-carrier-callback fixture returns an opaque invariant seed carrier and then invokes its virtual List view. Source and SDK profiles compare 96 cases twice through four frozen routes, including a future view override, exact view receiver identity, null carriers and Lists, callback bridge casts, mutations, partial writes, late value casts and exact checked/runtime/error identity. Original entries and metadata are compiled and audited. Six wrong programs, six Code-preserving Signature controls and native direct-result scope/declaration/variance/erasure/role/word variants guard the shared proof.

The reversed-carrier-slots fixture retains Map<K,V> and an opaque Map<V,K> result through complete invariant method slots. Source and SDK profiles each compare 96 cases twice through four frozen routes, covering raw/null inputs, callback mutation, partial writes, actual returned Map identity, late casts and exact checked/runtime/error identity. Original compiled entries and metadata are audited. Six wrong programs, six Code-preserving Signature controls and native scope/slot/declaration/erasure/role/word variants guard the reversed relation.

The primitive-map-super fixture passes a generic map allocated with int/float inputs to a generic parent Map constructor beside an int overload. Source and SDK parents each compile and audit every original entry, including a methodless marker. Four frozen routes exercise two input orders and 96 cases twice: exact float bits, invalid configuration, competing allocation overloads, allocation/parent/child order, partial writes, callbacks, exact failures and later key casts. Six wrong programs and six Code-preserving Signature controls guard the observer and scope proof. Native variants cover source/SDK declarations, invariant/covariant inputs, alpha-renaming, narrow/wide pairs, register overwrites, ranges and link order. Only owned maps execute.

The floating-constructor-default fixture forwards float and double constants beside covariant class-scoped map inputs through original this calls, with competing int/long overloads. Four frozen routes compare 96 cases twice for source and SDK parents: exact bits and negative zero, original references, parent/target/wrapper order, callback mutations, partial writes, exact failures and later key casts. All source units and metadata entries compile and match, including a methodless marker. Six wrong programs and six Code-preserving Signature controls guard the observer and scope. Native cases cover canonical zeros, subnormals, ordinary values, NaN payloads, infinities, both word widths, copies, ranges, input orders, SDK declarations and link order; opposite-width, integer-input/field and stale-pair controls remain rejected. Existing integer-default cases retain their runtime and scope validation. Only owned classes execute.

The scoped-builder-callback-super fixture retains a virtual generic Map result and a fresh raw Function/Serializable allocation through both original check-casts before super. Four frozen constructor routes compare 96 cases twice with source and SDK builders/parents, including a future override with an unrelated map result. They audit all original source, method, bridge and metadata entries, a methodless marker, input/result snapshots, exact receiver/callback identity, ordering, partial writes, exact failures and later key casts. Six wrong programs and six Code-preserving Signature controls guard observation and scope. Native variants cover source/SDK parents, builders and interfaces; synthetic allocation flags; ranges, alpha-renaming and link order; primitive-separated siblings and complete upward cast sequences. Missing ancestry, reference siblings and unproved result scopes remain rejected. Only owned code executes.

The scoped-comparator-allocation fixture forwards the exact incoming Comparator<? super K> to an original generic map initializer beside a Map overload. The unused allocation value formal supplies no data binding. Four frozen routes compare 96 cases twice for source and SDK allocations/parents, auditing all recovered units and metadata including a methodless marker. Observers check stored comparator identity, null policy, real callback arguments/order, partial writes, exact failures and later key casts. Native variants cover complete input scopes with all three supported variances, source/SDK declarations, ranges, alpha-renaming, link order, unused formals and covariant parent views. Stars, borrowed scopes, method formals, stale origins and unproved initializer overloads remain rejected. Only owned maps and comparators execute.

The scoped-map-initializer-upcast fixture supplies an unchanged SortedMap<? extends K,? extends V> reference to an original Map initializer beside a more specific SortedMap overload. Four frozen routes compare 96 cases twice for source and SDK allocations/parents, compiling and auditing every original unit and metadata entry including a methodless marker. They check physical overload selection, source identity, exact copy reads, callbacks, snapshots, partial writes, exact failures and later key casts. Six wrong programs, six Code-preserving Signature controls and native subtype/scope/overload/origin variants guard the proof. Only owned maps execute.

The scoped-supplier-delegation fixture invokes an incoming Supplier<Map<K,V>> before a same-owner Map constructor and retains independent Function<Object,K> and Function<Object,V> inputs. Four frozen routes compare 96 cases twice with source and SDK parents, compiling and auditing every original source/metadata entry and a methodless marker. Observers check single dynamic invocation, cached results despite callback mutation, null/invalid results, parent/constructor/wrapper effects, partial writes, callback identities, later casts and exact failures. Six wrong programs, six Code-preserving Signature controls and native source/SDK/interface/virtual/copy/range/scope variants guard the proof; missing casts and incomplete result declarations remain rejected. Only authored fixtures execute.

The carrier-nominal-varargs fixture retains Map<K,V>, two independent String contexts, the exact String[] tail and an opaque String result across static calls. Four frozen routes compare 96 cases twice with source and SDK providers, including the original empty-array convenience call, null/raw/hostile/unmodifiable maps, array identity/content mutations, input/result identity, callbacks, partial writes, late failures and exact thrown objects. Every original source, metadata and Exceptions entry compiles and matches, including a methodless marker. Six wrong programs, six Code-preserving Signature controls and native binary scopes, fixed/varargs calls, array ranks, input order, alpha names, source/SDK declarations and link-order variants guard the proof. Only owned fixtures execute.

The carrier-scalar-varargs fixture adds an original boolean context before the exact reference-array tail of a two-formal Map method. Four frozen routes compare 96 cases twice for source and SDK providers, checking true/false words, all original entries and metadata, raw/null/hostile/unmodifiable maps, array and context identity, callback mutations, partial writes, opaque results and exact failures. Six wrong programs and six Code-preserving Signature controls guard observation and scope. Native variants cover every primitive descriptor, wide pairs, fixed and varargs calls, multiple array ranks, input order, renamed scopes and link order; changed scalar erasures and missing or stale physical words remain rejected. Only owned code executes.

The paired-seed-element fixture combines Map<K,V>, K and Function<? super K,? extends V> while keeping the actual stored callback result independent of the opaque returned object. Four frozen routes compare 96 cases twice for source and SDK providers, checking every original entry and metadata, null and raw inputs, callback argument and identity, ordering, partial map writes, exact failures and later caller casts. Six wrong programs guard those observations; six Code-preserving Signature controls alter the shared forward/adapt contract. Native checks cover renamed and reordered formals, source and SDK carriers, callback variance, range calls and link order, and reject incomplete scopes, reversed roles, altered bounds, access/Code roles and invalid physical words. Only owned fixtures execute.

The paired-seed-scalar fixture adds an original boolean context to a two-formal Map.Entry factory. Four frozen routes compare 96 cases twice for source and SDK providers while auditing every original entry and metadata. Mutable and immutable entry construction, exact key/value and allocation identities, scalar branch selection, callback writes, partial effects, opaque results and exact failures remain observable. Six wrong programs and six Code-preserving Signature controls protect the observations and contract. Native variants cover all primitive descriptors and widths, leading/trailing contexts, renamed/reordered formals, independently swapped input/result slots, source/SDK carriers and link order. Repeated or missing seeds, incomplete results, variance, altered erasures and invalid physical words remain rejected. Only owned fixtures execute.

The token-seed-unchecked fixture combines Class<T>, an independent Object context and a T fallback beside a declared RuntimeException subtype, then delegates through an unchanged Type input. Four frozen routes compare 96 cases twice for source and SDK providers, compiling all output and auditing every original entry and metadata. Null/raw tokens and inputs, hostile Type objects, callback argument identity and mutation, opaque results, partial effects, exact unchecked and hidden checked failures, and late caller casts remain observable. Six wrong programs and six Code-preserving Signature controls guard the behavior and relation. Native variants cover renamed scopes, input order, complete source/SDK tokens, fixed class/interface contexts and RuntimeException/Error hierarchies; foreign formals, token variance, checked or unknown declarations, invalid roles and physical words are rejected. Only owned fixtures execute.

The paired-key-consumer fixture retains Map<K,V>, Comparator<? super K> and TreeMap<K,V> through the same original call. Four frozen routes compare 96 cases twice for source and SDK providers, compiling all output and auditing all original entries and metadata. Exact comparator inputs/results, callback changes observed by later field reads and map writes, null/raw arguments, partial writes, opaque results and exact callee/caller failures remain visible. Six wrong programs and six Code-preserving Signature controls protect the relation and observations. Native variants cover independent carrier declarations, source/SDK inputs, alpha-renamed and reordered formals, reordered parameters and pair slots, producer inputs, invariant/contravariant consumers, range calls and link order. Missing, foreign, reversed or variant slots and invalid declaration roles or words remain rejected. Only owned fixtures execute.

The comparable-pair-factory fixture covers a nullary Comparator<Map.Entry<K,V>> factory with V extends Comparable<? super V>, including the real SDK value-comparator factory and a separate opaque provider result. Four frozen routes compare 96 cases twice for source and SDK providers, compile every generated source, and audit all original entries and metadata. Exact factory effects, result identity, getter and comparison order, null/raw values, hidden checked failures and late caller casts remain observable. Six wrong programs and six Code-preserving Signature controls protect the binding. Native variants cover source/SDK carrier declarations, method/owner scope, reordered and alpha-renamed formals and slots, discarded results, class/interface owners, range calls and link order. Missing or variant nested slots, changed recursive bounds and invalid invocation roles remain rejected. Only owned fixtures execute.

The scoped-static-field-super fixture forwards ConcurrentMap<Reference<K>,V> and an enum constant or mutable volatile marker to a generic parent. Four frozen routes compare 96 cases twice for source and SDK parents, compile every generated source, and audit every original class, method and metadata entry. Callback changes to the field, original argument identity, partial parent writes, null/raw inputs, late casts and exact unchecked/hidden checked failures remain visible. Six wrong programs and six Code-preserving Signature controls protect the observations. Native cases cover source/SDK parents and fields, alpha-renamed/reordered owner formals, nested carrier declarations, copied registers, final/volatile fields, range calls and link order; missing or generic fields, wrong scopes, altered nested slots and malformed constructor roles remain rejected. Only owned fixtures execute.

The fixture explicitly checks the five disclosed enum projections and the one compiler-generated backing-array helper. It restores only the original synthetic access bit after compilation, verifies every Code attribute and all other classfiles are unchanged, then requires exact original class/method metadata and runtime behavior.

The dependent-carrier-allocation fixture retains C extends Collection<V> while a primitive-initialized container reaches Parent<K,C>, beside a Supplier<Map<K,C>> constructor overload. Four frozen routes compare 96 cases twice for source and SDK parents, compile every source and audit all original entries and metadata. Capacity and float bits, original allocations, callbacks, partial writes, exact constructor failures and later caller casts remain observable. Six wrong programs and six unchanged-Code Signature controls protect the contract. Native cases cover source/SDK declarations, owner/formal names, invariant and covariant input slots, reversed scalar order, wide words, range calls and link order; missing, foreign, recursive or variant dependent bounds and malformed constructor roles remain rejected. Only owned fixtures execute.

The synthetic-nullary-allocation fixture creates a raw callback through an exact public synthetic nullary initializer before same-owner constructor delegation. Nested covariant map arguments, float bits, original allocation and callback identities, selected overloads, snapshots, partial writes and exact failures remain observable through 96 cases twice in four frozen routes for source/SDK interface declarations. All generated output compiles; exact class and constructor synthetic bits are restored with unchanged Code before auditing every original class/method entry and metadata. Six wrong programs and six Code-preserving Signature controls protect the relation. Native source/SDK variants cover original flags, names, nested scopes, range and wide words and link order; changed initializer roles, generic bounds, capture scopes and invalid words remain rejected. Only owned fixtures execute.

Synthetic-nullary native variants include the explicit interface upcast retained by D8 for the owned source fixture. Unknown targets and unrelated checked conversions remain rejected; the same runtime observers require the original casted object identity and all allocation/callback effects.

A separate positive Signature control binds the callback to another already-declared Object-bound owner formal. Its unchanged original Code is decompiled, fully compiled, checked for exact original entries/metadata and compared with the same 96-case runtime baseline. The raw allocation supplies no relation between independent owner formals; an undeclared callback element remains rejected.

The scoped-builder superclass regression now checks that a synthetic nullary initializer preserves the same fully casted overload choice as its ordinary initializer beside an Object sibling. Both source/SDK declarations and both flag states retain their original selected target and metadata. A sibling that introduces its own constructor formal remains rejected because it needs a separate inference proof.

Synthetic nullary admission keeps its declaring implementation raw. A concrete parameterized interface header cannot supply an unrelated owner-formal target binding; the original scoped-builder counterexample and the new direct-delegation counterexample both retain this rejection.

The scoped-integer-getter-delegation fixture captures the original dynamic size() result before a same-owner constructor call. Four frozen routes compare 96 cases twice for source/SDK interface declarations, compile every generated source and audit all original entries and metadata. Mutating getter callbacks, negative values, float bits, null receivers, raw results, overload selection, partial writes and exact failures remain observable. Six wrong programs and six Code-preserving Signature controls protect the proof. Native cases cover virtual/interface and source/SDK declarations, alpha-renamed nested scopes, copies, range calls and link order; changed getter roles, result kinds, undeclared formals and integer-to-float reinterpretation remain rejected. Only owned fixtures execute.

The captured-owner profile of synthetic_instance_names uses an original javac inner member and its D8-generated lambda entry. Source and SDK-provider profiles compile every recovered source and compare 80 cases twice across direct, precompiled, reflection and method-handle callers. The observer checks outer identity, subclass dispatch, callback timing, cleanup and exact failures, with five incorrect programs. Native cases exercise canonical captured scopes, nested outer formals, alpha renaming, range calls and link order; changed access, method/own formals, incomplete families and absent capture evidence remain rejected. Only owned fixtures execute.

The captured-owner compiler witness also checks the independent D8 lambda class can access its captured private member. It requires the exact package-access caller receipt and compiler metadata difference. Native coverage distinguishes independent callers from calls inside the same enclosing source class.

The covariant allocation constructor profile extends the synthetic nullary allocation fixture with a compiled `Maker<? extends Collection<V>>` target. Source and SDK declarations use the same frozen observer: all generated Java and original identities/metadata are checked, 96 cases run twice through direct, precompiled, reflection and method-handle routes, and six wrong programs expose allocation, input, target and exception changes. Six Code-preserving Signature controls retain rejection of missing formals, lower bounds and incompatible slots; an independent declared owner formal remains a positive control. Native cases additionally cover nested covariance, alpha-renamed scopes, wide inputs, range calls and link order.

The existing allocated-constructor-context and nested-carrier delegation regressions also retain outer and nested covariant allocation views as positive native cases. Their compiled Signature controls use lower bounds for unsupported variance, while the source/SDK runtime profiles continue to check the unchanged allocation and initialization behavior. The separate superclass binding proof keeps its original covariance rejection controls.

The synthetic-primitive-allocation profile uses an owned boolean capture and a covariant callback carrier. Four frozen call routes exercise both capture values, allocation and target failures, partial effects and opaque callback results across 96 cases twice; all generated Java, original identities and metadata are checked. Eight wrong programs include reversed capture input and field storage. A constructor-flag-only control keeps all Code bytes and other metadata identical and demonstrates the former synthetic-arity restriction. Native cases cover boolean, integer, long and double captures, exact wide pairs, range calls, source/SDK declarations and retained owner scope; incomplete arguments, changed constructor roles and generic owners remain rejected.

The instance-array-transfer profile recovers a generic provider through a generic derived receiver. Four frozen call routes cover 104 cases twice, including precompiled subclass overrides, monitor ownership, null receivers, callback failures, partial array writes and opaque result casts. Every original class and method, generated helper, Signature and Exceptions attribute is audited after compiling all emitted Java. Eight wrong programs and six Code-preserving Signature controls distinguish receiver, component, result and synchronization mistakes. Native cases also cover lexical shadowing, renamed formals, direct/inherited lookups, source/SDK providers, range frames and reversed link order.

The mixed-star-constructor profile uses a closed covariant callback slot beside an unbounded wildcard. Four frozen call routes cover 160 cases twice, including both boolean defaults, integer overload separation, partial final-field initialization, callback identity and late checked results. All generated Java, original entries, metadata and Exceptions are compared for source and SDK declarations. Six wrong programs and six Code-preserving Signature controls retain failure sensitivity. Native coverage includes invariant, upper and lower nominal bounds, reversed slots, nested carriers, range aliases, zero/one defaults and malformed scope, declaration and word origins.

The inherited marker fixture also verifies three closed mixed-star access profiles: a public target with the original synthetic marker, an ordinary marker in a plain owner, and an ordinary marker in an inherited owner. Both source and SDK providers retain 60 frozen JVM cases twice, reflection checks, initialization and callback effects, all original compiled identities and six wrong programs per profile. These are positive complete-contract cases; malformed arguments, bounds and physical frames remain negative tests.

The dependent-collector fixture verifies chained interface bounds using two mapper inputs and two invariant suppliers. Source and SDK providers compile every emitted Java source and compare all original identities, metadata and Exceptions. Four frozen call routes run 96 cases twice, checking evaluation order, a single input read, nulls, callback exceptions, partial effects, aliasing and delayed Collection, Map and Collector casts. Ten wrong programs and eight Code-preserving Signature controls verify failure sensitivity. Native cases vary declaration order, formal names, provider source, raw invocation encoding and result use, with incomplete constraints, roles, declarations and physical frames rejected.

The produced-collector profile verifies a single supplier through owned factories and the standard collection factory. Both source and SDK providers compile all emitted Java and compare every original class, method, metadata and Exceptions entry. Four frozen call routes run 64 cases twice, covering laziness, collection identity and contents, partial writes, callbacks, nulls, exact exceptions and late Collection/Collector casts. Ten wrong programs and eight Code-preserving Signature controls retain failure sensitivity. Native tests vary formal names and order, declaration source, result use and normal/range invokes, while rejecting incomplete bounds, carrier roles, access, scopes and physical frames.

The grouped-collector source and SDK profiles compile every emitted Java source and compare all original class, method, metadata and Exceptions entries. Four frozen caller routes run 96 cases twice across classifiers, map suppliers and downstream supplier/finisher accessors and callbacks, checking order, identity, partial writes, nulls, exact exceptions and late Map/Collector and callback bridge casts. Ten wrong programs and eight Code-preserving Signature controls establish failure sensitivity. Native matrices retain distinct input, key, result, state and bounded carrier roles across names, formal order, variance, dependency source, result use and invoke encodings; malformed bounds, roles, declarations and physical frames remain rejected.

The dependent-operator source and SDK profiles compile all emitted Java and compare every original class, method, metadata and Exceptions entry. Four frozen caller routes run 96 cases twice, preserving operand identities, callback and action order, partial writes, nulls, exact errors, late result casts and subsequent calls through the returned operator. Ten wrong programs and eight Code-preserving Signature controls retain failure sensitivity. Native matrices cover either Object dependency as the input element, formal renaming and order, source/SDK declarations, result use and normal/range calls; malformed bounds, variance, carrier declarations, scopes and physical frames are rejected.

The configured-collector source and SDK profiles retain both the short delegating factory and its fully configured overload. They compile all emitted Java and compare every original class, method, metadata and Exceptions entry. Four frozen caller routes run 96 cases twice across configuration identities and aliasing, callback-to-interface casts, partial integer writes, exact exceptions, configuration replacement and late result casts. Ten wrong programs and eight Code-preserving Signature controls cover both overloads. Native matrices vary one or three contexts, parameter order, formal names, callback variance, dependency origin, result use and invoke encoding, while rejecting incomplete nominal declarations, carrier roles, scopes and physical frames.

The nested-produced-collector source and SDK profiles compile all emitted Java and compare every original class, method, metadata and Exceptions entry. Four frozen caller routes run 96 cases twice, covering opaque producer results, input identity, partial effects, callback mutation, late result casts and later Map/List callback casts. Ten wrong programs include an invented eager bound cast; eight Code-preserving Signature controls vary nested edges and ordered roles. Native matrices vary wrapper position, pair order, formal names/order, dependency origin, result use and invoke encoding; 43 controls reject lost edges, incomplete declarations, scopes and physical frames.

The mapped-reducer source and SDK profiles compile all emitted Java and compare all original classes, methods, metadata and Exceptions. Three owned caller routes cover immediate mapper/operator calls, state identity, partial writes, bridge casts, callback replacement, exceptions and late result casts. A fourth route exercises the standard three-parameter reducing collector with delayed callbacks, empty inputs, null callbacks, intermediate state identity and failures on either input. The 96 cases run twice, with ten wrong programs and eight Code-preserving Signature controls. Native matrices vary six parameter orders, variance, names, formal order, source/SDK declarations and invoke forms, plus 43 rejection controls; the original single-formal reducer tests remain in the focused gate.

The merging-collector source and SDK profiles compile every emitted Java source and compare all original class, method, metadata and Exceptions entries. Three owned caller routes retain immediate callbacks, captured inputs, Map casts, partial effects, exact errors and an opaque result. A fourth route verifies standard four-argument toMap laziness, supplied Map identity, duplicate-key merging, removal on a null merge result, prepopulated values, null keys and callbacks, raw values and later bridge failures. The 96 cases run twice, with ten wrong programs and eight Code-preserving Signature controls. Native matrices vary names, formal order, pair roles, variance, dependency origin, result use and invoke encoding, with 46 rejection controls and the existing five-formal factory regressions.

The contextual-collector source and SDK profiles exercise result-only method formals with short and fully configured factories. All emitted Java and every original metadata entry are checked. Four frozen routes run 96 cases twice, including context aliasing and replacement, captured versus reread configuration, partial writes, exact failures and late opaque result casts. Ten wrong programs and eight Code-preserving Signature controls verify the observers and rejection boundary. Native matrices cover one through four closed contexts and explicitly admit the former callback-removal control while preserving malformed declaration, variance, scope, erasure and frame rejections.

The prefix-loop-final source and SDK profiles publish a constructor receiver, commit a prefix final, execute a loop and then choose alternative boolean and object final stores before a wide final. Frozen direct, reflection and MethodHandle callers run 150 cases twice, observing zero/one/multiple iterations, exact checked exceptions and Errors, partial final visibility, mutable token/configuration capture and wide values. Fourteen wrong programs qualify the observers; all emitted Java, metadata, roles and Exceptions are verified without field projections or postprocessing. Native tests vary references/wide scalars, an optional prefix store, invoke encoding, varargs metadata and provider origin, and reject skipped, repeated, foreign, mistyped or reordered stores and invalid edges.

The same profile includes a published constructor that stays in an assignment-free prefix loop. A frozen daemon caller uses a volatile callback signal, a bounded observation and exact partial-field checks; the original and recovered constructor must both remain active without completing or running later callbacks. A separate encoded CFG proof checks the non-returning cycle and ordered writes on every completing path. The two earlier shared-guard back-edge cases are explicit positive native coverage; malformed skips, repeats, operands and fields retain their negative coverage.

The guarded-delegation-tail source and SDK profiles retain a varargs constructor with conditional argument computation, exact this delegation and an initialized array loop. Frozen direct, reflection, fixed-arity MethodHandle and subclass callers run 192 cases twice. They check checked exceptions and Errors by identity, overflow and wide values, null arrays, mutation during delegation, array/context capture and virtual dispatch. Twelve independent wrong programs qualify the observer; every emitted Java source, original definition, role, metadata record and Exceptions attribute is checked. Native coverage also exercises zero and raw floating scalar exports, range frames and reversed linking, and rejects prefix reentry, bypassed/repeated initialization, final rewrites and unproved continuation words.

Prefix callback declarations in guarded-delegation-tail remain empty; the independent frozen harness still injects checked exceptions and Errors by identity. Declared checked prefix exceptions retain their separate rejection boundary. The formerly rejected linear allocation argument followed by a dead local assignment is now an explicit positive native case, after compiling all three raw/linked/reversed outputs and comparing array allocation and failure behavior against an independent reference.

The interface-scope variant of inherited-static-prefix retains the original owned program and adds explicit known marker interfaces to both classes between the symbolic field owner and its declaration. Source and SDK-provider profiles still run 112 cases twice with 17 independently compiled wrong programs, checking initialization failures, exact exception identities, wide/reference snapshots, mutation during delegation and partial final stores. All original metadata and every generated Java source are verified. Native tests cover shared interface diamonds on one or several class steps and reject same-name fields of either type, missing or cyclic interface ancestry, noninterface nodes and the existing class/access failures.

The nested-task-transfer source and SDK profiles call both ExecutorService.invokeAll overloads and an independent Worker interface with the same nested relation. Frozen direct, separate-caller, reflection and MethodHandle paths run 256 cases twice, checking volatile receiver snapshots, opaque collections and Future lists, wide timeout values, exact context identities, nulls, checked/runtime/Error propagation, interrupt flags and deferred consumer casts. Twelve independently compiled wrong programs and eight same-Code Signature controls qualify these checks. Every emitted Java source and original definition, role, Signature and Exceptions attribute is verified. Native coverage varies concrete/interface dispatch, declaration origin, alpha-renamed formals, range frames and closed contexts, with negative variance, scope, bound, role and descriptor controls.

The nested-task-result source and SDK profiles retain the direct own-formal result from both ExecutorService.invokeAny overloads and an independent Worker declaration. Frozen callers run 256 cases twice across direct calls, a separately compiled generic caller, reflection and MethodHandle invocation. They check opaque input collections, receiver and result identities, wide timeout contexts, volatile fields, interrupt state, checked exceptions and cause identity, and delayed caller casts. Thirteen independently compiled wrong programs and eight same-Code Signature controls qualify the observers. Every emitted source, definition, role, Signature and Exceptions attribute is checked; native controls vary instance dispatch, origin, alpha-renaming, range frames and closed checked contexts.

The deferred-field-string source and SDK profiles independently witness D8 emitting allocation, a volatile field read, a string and a constructor whose string argument comes first. Frozen direct, separate-caller, reflection and MethodHandle routes run 128 cases twice, covering initialization effects and poisoning, null priority, field snapshots, callback mutations, checked failures, actual result identity and live receiver continuation. Fourteen independently compiled wrong programs qualify the observers; every emitted source, original definition, role and metadata attribute is checked. Native coverage includes field and call predecessors, declaration origins, jumbo strings, argument order, retained values and range frames, with malformed producer, access, frame and entry controls.

The array-nominal-varargs source and SDK profiles preserve an explicit method-owned array beside closed nominal/scalar contexts and an opaque nominal result. Frozen direct, separately compiled caller, reflection and fixed-arity MethodHandle observers run 104 fresh cases twice, covering null/empty/covariant arrays, original array mutations, context identity, extreme scalar values, exact checked and unchecked failures, delayed casts and result identity. Fourteen independently compiled wrong programs and eight same-Code Signature mutations qualify the observers and rejection boundary. Every emitted source, definition, role, Signature and Exceptions attribute is checked. Native controls vary alpha-renaming, source/SDK origin, range frames, result declarations, wide contexts and link order.

The array-object-context source and SDK profiles retain additional scalar contexts beside an Object-bounded method array, independent Object input and integer result. Frozen direct, separately compiled caller, reflection and MethodHandle observers run 192 fresh cases twice, checking array identity and mutations, covariance failures, long extremes, raw NaN payloads, negative zero, Boolean flags, callback failure identity and deferred result casts/unboxing. Fifteen independently compiled wrong programs and eight same-Code Signature controls qualify the observers. Every emitted source, definition, role, Signature and Exceptions attribute is checked; native controls vary origin, alpha-renaming, scalar widths, range frames and link order.

The comparable-array-predicate source and SDK profiles verify a first-rank array bounded by Comparable<? super T> and an opaque Boolean result. Frozen direct, separately compiled caller, reflection and MethodHandle observers run 160 fresh cases twice, checking null/empty/covariant arrays, inherited lower-bound implementations, comparison identities and mutations, raw bridge casts, exact callback failures and deferred Boolean conversions. Fifteen independently compiled wrong programs and eight same-Code Signature controls qualify the observers. Every emitted source, definition, role, Signature and Exceptions attribute is checked; native controls vary origin, both independent formal names, range frames, link order, recursive variance and malformed Comparable declarations.

The comparable-array-result source and SDK profiles retain a recursively bounded array, optional invariant Comparator context and direct own-formal result. Frozen direct, separately compiled caller, reflection and MethodHandle observers run 216 fresh cases twice, checking original array/comparator identities, comparison argument snapshots, callback mutations and exact failures, opaque returned objects, original bounded casts and later consumer casts. Sixteen independently compiled wrong programs and eight same-Code Signature controls qualify the observers. Every emitted source, definition, role, Signature and Exceptions attribute is checked; native controls vary both declaration origins, array-only or contextual shape, alpha-renaming, class/interface contexts, range frames and link order.

The array-star-context source and SDK profiles preserve a method-owned array transfer beside Class<?> and scalar contexts. Four frozen invocation paths run 216 fresh cases twice, retaining array/token identities, wide integers, raw floating-point payloads, callback snapshots and exact throwable identity, covariant stores, opaque result arrays and delayed caller casts. Sixteen wrong programs and eight same-Code Signature controls qualify the observers. Every emitted source, original class/method role, Signature and Exceptions attribute is checked. Native controls vary both declaration origins, class/interface carriers, canonical unbounded syntax, scalar widths, fixed varargs tails, alpha-renaming and link order.

The enum-token-map source and SDK profiles retain recursive enum tokens and opaque LinkedHashMap<String,E> results. Four frozen invocation paths run 192 fresh cases twice, checking map identity, subclass query dispatch, callback mutations and throwable identity, raw non-enum or null tokens, deferred enum initialization and delayed value casts. Fifteen wrong programs and eight same-Code Signature controls qualify the observers. Every emitted source, definition, role, Signature and Exceptions attribute is checked. Native coverage varies provider/result-carrier origins, public inherited entry points, class/interface results, independent platform/method alpha-renaming, closed result heads, invocation frames and link order.

The enum-carrier-key source and SDK profiles preserve a recursive enum formal beside an independent key in a binary input carrier. Four frozen invocation paths run 192 fresh cases twice, checking exact pair/key/default identity, null inputs, raw enum defaults, original input/result casts, callback and interface probe timing, mutations and throwable identity. Fifteen wrong programs and eight same-Code Signature controls qualify the observers. All emitted sources compile with original definitions, roles, Signature and Exceptions retained. Native coverage varies declaration origins, class/interface carriers, alpha-renaming, public inherited lookup, optional defaults, invocation frames and link order.

The enum-token-predicate source and SDK profiles preserve recursive enum tokens, closed string contexts and opaque Boolean results. Four frozen invocation paths run 192 fresh cases twice, covering null and raw non-enum tokens, primitive/array tokens, exact string identity, deferred enum initialization, callback writes, exact throwable identity, input casts and negated consumers. Fifteen wrong programs and eight same-Code Signature controls qualify the observers. All emitted sources compile with original definitions, roles, Signature and Exceptions retained. Native coverage varies declaration origins, public inherited lookup, independent alpha-renaming, invocation frames and link order.

The array-input-mapped-token source and SDK profiles retain input-array components, mapper variance and independently selected result-array components. Four frozen invocation paths run 192 fresh cases twice, checking input/result identity, null or primitive tokens, null/empty arrays, missing callbacks, callback mutations, exact array and mapper failures, delayed narrow-array casts and covariant stores. Fifteen wrong programs and eight same-Code Signature controls qualify the observers. All emitted sources compile with original definitions, roles, Signature and Exceptions retained. Native coverage varies provider/carrier origins, class/interface carriers, alpha-renaming and formal order, all six argument orders, ordinary/range calls, public inherited lookup and link order.

The token-star-array-factory source and SDK profiles retain Class<T>, Class<?>[] and Constructor<T> with a competing broad overload. Four frozen invocation paths run 192 fresh cases twice, checking exact member selection, input and result identity, null arrays and tokens, primitive tokens, callbacks and partial effects, deferred construction and value casts. Fifteen wrong programs and eight same-Code Signature controls qualify the observers. Every emitted source compiles with original definitions, roles, Signature and Exceptions retained. Native coverage varies provider/carrier origins, class/interface tokens, alpha-renaming, canonical all-star syntax and already supported raw arrays, original varargs roles, ordinary/range calls, public inherited lookup and link order.

The comparable-carrier-result source and SDK profiles preserve Collection<T> inputs and opaque results bounded by Comparable<? super T>. Four frozen invocation paths run 192 fresh cases twice, checking identity, null inputs, absent traversal, callback failures and partial effects, original bounded casts, delayed concrete casts and deferred comparisons. Fifteen wrong programs and eight same-Code Signature controls qualify the observers. All emitted sources compile with original definitions, roles, Signature and Exceptions retained. Native coverage varies provider/carrier origins, class/interface carriers, alpha-renaming, invariant/covariant inputs, used/discarded results, ordinary/range calls, public inherited lookup and link order; existing Object-erased results retain their independent regression coverage.

The fixed-sdk-interface-bridge source and SDK profiles compile all emitted sources and compare every original class/method definition, bridge/synthetic role, Signature and Exceptions attribute. Four invocation paths run 192 fresh cases twice, including a special method handle that enters the original parent bridge on an overriding subclass. Observers check virtual dispatch, cast/null ordering, wide arguments/results, callback effects, delayed reads and exception identity. Fifteen wrong programs and eight same-Code class Signature controls qualify those observations. Native coverage varies root/fixed/plain interface origins, direct/transitive graphs, alpha-renaming, lexical and link order, closed slots and ordinary/range bridge calls, with incomplete declarations and changed bodies rejected.

The nested-prefix-allocation source and SDK profiles compile every emitted source and preserve original definitions, method roles, Signature, Exceptions and final fields. Four frozen constructor paths run 192 fresh cases twice, covering this/super delegation, outer-before-inner class initialization, erroneous-class reuse, callback failure identity, partially escaped objects, original argument identities and initialized continuations. Fifteen wrong programs include inner-first construction with otherwise identical constructor calls. Native coverage varies independent class origins, nesting depth, wide words, ordinary/range calls, moved frames, this aliases and link order; open lifetimes, malformed initializers, receiver exposure, hidden wide overlaps and unsupported control flow remain rejected.

The filled-reference-prefix source and SDK profiles compile all emitted units and compare original definitions, method roles, Signature, Exceptions and final fields. Four frozen caller routes cover Object[] and Object[][] through both this and super delegation, with 256 fresh cases twice. The observer checks element and array identity, component type, callback mutation, substitution, class initialization failure/reuse, partial construction and exact unchecked/checked failure identity. Fifteen wrong programs guard the observer. Native checks cover nominal/interface/array components, ordinary/range encodings, empty arrays, overwritten incoming slots, guarded prefixes, delegation chains and link order; invalid types, uninitialized receivers, malformed results and unsupported metadata remain rejected.

The reference-array profiles also retain D8 matrix-array allocation/store sequences. These overwrite a no-longer-used incoming word before super delegation. Native controls require later reads of discarded words to fail through direct calls, reference copies and array length; a previous parameter value cannot reappear at the initialized boundary.

The contextual-carrier-transfer source and SDK profiles compile every recovered Java unit and preserve original definitions, method roles, Signature and Exceptions. Four frozen call routes compare 192 cases twice, including nulls, original or alternate carrier objects, fixed context and class-token identity, callback mutations, initialization failure and reuse, exact checked/runtime/error identities and delayed result casts. Fifteen wrong programs and eight Code-preserving Signature controls verify failure sensitivity. Native cases vary formal names/order, context positions, declaration source, ordinary/range calls and declared exceptions, including the existing two-seed callback transfer; incomplete scope, bounds, declarations, exceptions and physical words remain rejected.

The closed-carrier-producer source and SDK profiles compile every recovered unit and compare original definitions, method roles, Signature and Exceptions. Four frozen caller routes run 192 cases twice across receiver reuse and overrides, nulls, closed contexts with opaque contents, callback mutation, partial effects, exact checked/runtime/error identity and delayed result casts. Sixteen wrong programs and eight Code-preserving Signature controls establish failure sensitivity. Native matrices cover input positions, formal names, source and SDK declarations, static/instance and ordinary/range calls and closed exceptions; incomplete declarations, variance, exception hierarchies and frames remain rejected. Existing sole-producer cases also cover valid instance checked exceptions and reject non-Throwable declarations.

The instance-producer-varargs source and SDK profiles compile all emitted units and preserve definitions, method roles, Signature and Exceptions. Four frozen invocation routes compare 192 cases twice, covering overrides and receiver reuse, null/empty arrays, original array identity, callback mutations, partial array writes, exact checked/runtime/error identities and delayed result casts. Twenty wrong programs and eight Code-preserving Signature controls qualify these observers. Native coverage varies declaration origins, formal names, fixed/varargs declarations, reference/multidimensional/primitive/all-star arrays, ordinary/range calls, exceptions and link order; dependent or incomplete arrays, unbound producers, invalid exceptions and changed frames remain rejected.

The instance producer matrix also retains an opaque nominal component when its optional declaration is absent. Separate source and SDK qualification compiles the complete emitted sources against that original nominal dependency and compares all definitions and metadata. An all-star array control isolates the input/result proof from the Exceptions attribute; both empty and closed exception contracts retain the same producer scope.

The closed-context-producer source and SDK profiles compile all emitted sources and preserve definitions, method roles, Signature and Exceptions. Four frozen caller routes run 240 cases twice through static forwarding and virtual dispatch, checking nulls, overrides, both callbacks, original closed-result casts, input mutation, partial effects, returned references and exact checked/runtime/error identity. Twenty wrong programs and eight Code-preserving Signature controls qualify the observers. Native cases vary one, two and three closed slots, declaration origins, formal names, input order, static/instance and ordinary/range calls and declared exceptions; open variables, mismatched arity, invalid bounds, variance and frames remain rejected.

The instance-seed-nominal source and SDK profiles verify a direct method seed with two booleans and two plain ancestors above a closed generic base and marker. Four frozen routes run 96 cases twice, checking each primitive value, input and receiver identity, overrides, opaque or aliased results, nulls, callbacks, partial effects, delayed casts and exact failures. Nine wrong programs and eight Code-preserving Signature controls qualify the observers. Every emitted source and every original definition, role, Signature and Exceptions entry is checked. Native cases also vary parameter positions, wide primitive words, formal names, result owners, declaration origins, link order and static/instance ordinary/range calls; malformed scopes, erasures, roles, exceptions and register frames remain rejected.

The method-seed-bridge source and SDK profiles retain all original class bridge slots and their reported source-projection status, then compile every source and compare complete definitions, access roles, Signature and Exceptions. Four frozen routes run 128 cases twice through leaf, middle and base descriptors and a future subclass override. They check seed and receiver identity, booleans, callbacks, partial effects, super dispatch, each original result cast, nulls, opaque and aliased values and exact failures. Thirteen wrong programs and eight same-Code Signature controls qualify the observers. Native cases retain the full method formal in each ancestor witness and reject changed scopes, missing slots, altered forwarding words and effects.

The unused-scalar-throws source and SDK profiles retain unused method formals on a public instance call with a closed nominal context and IOException contract. Four frozen routes compare 128 cases twice through original and future subclass dispatch. They verify receiver and context identities, opaque scalar values, early casts, callback order and partial writes, null timing, checked/runtime/error identity and late consumer casts. Ten wrong programs and eight Code-preserving Signature controls qualify the observers. Every generated unit compiles and retains every original class/method definition, role, Signature and Exceptions. Native cases retain static calls, namesake owner scopes, one or two unused formals, primitive widths, arrays and closed throws while rejecting malformed declarations and call words.

The unused-scalar-throws fixture also retains the original public generic scalar forwarder and its closed IOException declaration. Its frozen callers, all-source compilation and exact original inventory prevent an outer forwarding entry from disappearing while fixing the inner unused-formal call. Direct-seed native checks cover all eight primitive results, source/SDK and static/instance declarations, ordinary/range words, independent primitive contexts and alpha-renamed seed positions; malformed scopes, throws, declarations and original operands remain rejected.

The nominal-seed-context-throws source and SDK profiles retain a direct method-owned Object input, independent String and boolean contexts, a closed nominal result and IOException declarations. Four frozen routes compare 128 cases twice through original and future subclass dispatch. They verify receiver, input, context and result identities, context snapshots before receiver callbacks, primitive values, callback order and partial writes, early and late casts, null timing and exact checked/runtime/Error objects. Thirteen wrong programs and eight Code-preserving Signature controls qualify the observers. Every generated unit compiles with every original class/method definition, role, Signature and Exceptions. Native checks share the input proof across nominal/scalar results and covariant class bridge slots, preserving original physical words, scopes, exception compatibility and forwarding validation.

The nominal-seed-context-throws negative source profiles independently decode the exact method Signature with the Android SDK before testing rejection. This prevents D8 normalization from silently turning malformed metadata into a valid input. The fixed nominal context control remains encoded unchanged; original descriptors, Code and runtime observers remain frozen.

Earlier Object-scalar and instance-formal-context mutation controls now require positive direct-seed receipts when only one parameter remains generic and the other inputs are fixed Object or reflection Type contexts. Real source and SDK classfiles compile with exact original definitions and metadata; all other scope, role, erasure, exception and physical-word rejection controls remain active.

The closed-class-interface-bridge source and SDK profiles retain original checked bridges on a Signature-free child and its closed parent. Five frozen call routes include generic and raw interface dispatch plus special entry into both original bridge slots. Each profile checks 160 fresh JVM cases twice, including future subclasses, receiver/key/result identity, callback order, partial writes, early/late casts, null timing and exact runtime, Error and checked exceptions. Seventeen wrong programs and eight Code-preserving class Signature controls qualify the observers; source controls require the exact SDK-decoded annotation. All emitted Java units compile with every original definition, role, Signature and Exceptions. Original generic-interface-bridge projection status remains unchanged. Native checks cover source/SDK ownership, class-formal alpha-renaming, ordinary/range words, classpath order, conflicting slots and changed forwarding bodies. Earlier complete covariance receipts retain ownership of their method scopes.

The mixed-bridge-contract-ownership source and SDK profiles place checked parameter bridges and method-owned covariant result bridges on the same class. Each of 240 fresh JVM cases runs twice through both bridge families, with typed/raw/generic callers, original special bridge entry and frozen future subclass overrides. Observers check original receivers and arguments, opaque generic and final result identities, callback order, partial writes, null timing, late casts and exact checked/runtime/Error objects. A callback can replace a too-wide generic result before the original checked conversion. Twenty-eight wrong programs and eight Code-preserving class/method Signature controls qualify the observers. All generated Java units compile with every original definition, role, scoped Signature and Exceptions; original bridge projection statuses remain unchanged. Native controls preserve both families independently across source/SDK ownership, method-formal alpha-renaming, ordinary/range wide words, link order and changed scopes, slots, roles or either forwarding body.

The inherited-fixed-parameter-bridge source and SDK profiles retain a parent that implements two closed interface methods while the child overrides only one. The inherited method keeps its original erased and typed parent declarations and produces no child definition. Each of 160 fresh JVM cases runs twice through typed, raw, generic and special inherited bridge entries, including frozen future subclass redispatch. Observers verify receiver and argument identity, checked parameter casts, callback order, partial writes, null timing, result identity, late casts and exact checked/runtime/Error objects. Fourteen independent wrong programs and eight Code-preserving class Signature controls qualify the observers. Every emitted Java unit compiles with exact original class/method definitions, roles, Signature and Exceptions. Native controls cover source/SDK ownership, final inherited targets, alpha-renaming, scalar/reference results, ordinary/range wide words and class/method order; SDK checks validate declarations without claiming an external body proof. Original bridge projection statuses remain unchanged.

The inherited-method-parameter-bridge source and SDK profiles preserve a parent method with its own recursive enum bound and a fixed interface key parameter, while the child overrides only an unrelated method. Each of 240 fresh JVM cases runs twice through typed, erased, generic and special inherited bridge entries, including frozen future subclass overrides. Observers verify opaque token/result identity, receiver effects, callback order, partial writes, null timing, the original parent enum cast, later caller casts and exact checked/runtime/Error objects. A callback can replace an incompatible result before the original cast; a token cannot invent a different result cast. Nineteen wrong programs and eight Code-preserving class/method Signature controls qualify the observers. Every generated Java unit compiles with the original definitions, roles, full method scopes and Exceptions. Native controls cover independent Object and recursive Enum bounds, formal alpha-renaming, final inherited implementations, source/SDK ownership, ordinary/range wide words and link order. No child method or bridge is invented, and original projection statuses remain unchanged.

The inherited-method-parameter-bridge runtime profiles also verify concrete instance enum-token calls. A separate static/instance and plain/closed-owner source/SDK matrix isolates invocation admission, while native cases retain all four existing enum context shapes, descriptor aliases and ordinary/range call words. Invalid owner headers, changed method scopes, roles and call words remain rejected.

The closed-covariant-slot-ownership source and SDK profiles combine checked key bridges with literal covariant clone slots over a closed standard collection superclass. Each of 240 fresh JVM cases runs twice; independent checks also exercise six typed, erased and special clone entries, future overrides, null results and late result casts. Frozen observers preserve original effects, callbacks, partial writes, receiver/result identity, null timing and exact exception objects. Twenty-eight wrong programs and four Code-preserving class Signature controls qualify the observers. Every emitted Java source compiles with the original class/method definitions, access roles, Signature and Exceptions; original bridges stay explicitly unverified until recompilation. Native controls include wide/range words, class order, protected generic ancestors with narrowed throws, final/static conflicts and incompatible erased parameter slots inherited from SDK declarations.

The interface-token-result-throws source and SDK profiles preserve an inherited abstract interface method with a method-owned invariant Class<T> input, T result and checked exceptions. A field-loaded receiver forwards the same contract. Each of 192 fresh JVM cases runs twice through frozen direct, reflective and method-handle callers; observers cover future overrides, opaque/null/mismatched tokens, callback changes, late result casts and exact exception objects. Seventeen wrong programs and seven Code-preserving method Signature controls qualify the checks. Every emitted source compiles with exact original definitions, access, Code-presence roles, Signature and Exceptions; abstract interface methods remain declaration-only. Native checks also cover fixed contexts, wide/range call words, alpha-renamed scopes, inherited aliases and source/SDK order.

The literal-token-constructor source and SDK profiles keep a string constant, an opaque Class token and a closed context in the original superclass constructor call. Each profile runs 36 fresh JVM cases twice through frozen direct, reflective and method-handle callers. Observers cover class resolution without initialization, missing token classes, erroneous initialization, argument identity, prefix effects, wide bits, negative zero and exact exception objects. Twelve wrong programs and five Code-preserving constructor Signature controls qualify the checks. All emitted Java units compile with exact original definitions, roles, Signature and Exceptions. Native controls also cover copied origins, all closed Class variances, class/interface bounds, ordinary/range words and source/SDK order.

The closed-null-super-binding source and SDK profiles retain a null argument in a complete covariant superclass constructor slot without inventing a caller Signature. Each profile compiles all original definitions and runs 36 fresh JVM cases twice through frozen direct, compiled, reflective and method-handle callers. Observers check null and context identity, initialization and exception order, missing unrelated classes, wide bits and negative zero. Twelve wrong programs and five Code-preserving Signature controls qualify the checks. Native tests cover all closed variances, class/interface bounds, copied null origins and wide/range frames, while rejecting nonzero words, missing bounds, malformed metadata and changed overload or call roles.

The closed-interface-seed-throws source and SDK profiles retain checked exception declarations on static methods whose direct seed and result share a complete invariant interface bound. Each compiles all original definitions and runs 96 fresh JVM cases twice through direct, frozen compiled, reflective and method-handle callers. Eight wrong programs check context identity, callbacks, virtual delegation, partial writes, opaque results, exact checked failures and delayed casts. Six Signature controls retain all other DEX and classfile metadata and Code. Native checks include empty, unchecked, checked and mixed exception declarations, source/SDK dependencies, seed positions, wide/range frames, malformed exception descriptors, incomplete bound scopes and changed dispatch.

The closed-class-seed-throws source and SDK profiles retain checked exception declarations on static methods whose direct seed and result share a known non-generic class bound with a closed generic parent. Each compiles all original definitions and runs 96 fresh JVM cases twice through direct, frozen compiled, reflective and method-handle callers. Nine wrong programs check context identity, exact wide scalar values, callbacks, virtual delegation, partial writes, opaque results, exact checked failures and delayed casts. Six Signature controls retain all other DEX and classfile metadata and Code. Native checks include empty, unchecked, checked and mixed exception declarations, source/SDK dependencies, seed positions, wide/range frames, malformed exception descriptors, incomplete bound scopes and changed dispatch.

The nested-array-result source and SDK profiles verify a public static Object-bounded T[][] parameter and T[] result. Each compiles every emitted source with all original class identities, method descriptors, Signatures, access/Code roles and Exceptions. Four immutable caller routes cover 196 fresh JVM cases twice, including direct matrices and explicit row packing, covariance failures at both ranks, null and empty rows, partial writes, callbacks, class initialization, opaque or aliased results and delayed casts. Eight wrong programs and six exact same-Code Signature controls prevent copied matrices, guessed results, reordered effects and incomplete generic bindings from passing. Native checks cover fixed and variable arity, inherited declarations, range frames, copied and packed arguments, alpha-renaming, closed headers and malformed scope/rank/access metadata.

The closed-nominal-array-constructor source and SDK profiles verify fixed constructor selection beside variable-arity overloads. Arrays retain every rank and their original object identities even when a nongeneric component has a closed generic inheritance header. Each profile compiles every emitted source and retains all original class and method identities, Signatures, access and Code roles, and Exceptions. Four frozen caller routes cover 128 fresh JVM cases twice, including covariance failures at both matrix ranks, partial array writes, null and empty inputs, callbacks, class initialization, exact exception identities and delayed result casts. Eight wrong programs and six exact same-Code Signature controls guard the independent observer and incomplete contracts. Native checks cover source and SDK declarations, three array ranks, closed headers, copied range frames and malformed scope, erasure and access metadata.

The private-context-callback-throws profiles cover source and SDK declarations with both present and absent caller Exceptions attributes. Each compiles every emitted source and compares original class/method identities, access and Code roles, Signatures and Exceptions. Four frozen caller routes exercise 112 fresh JVM cases twice per profile, including static and instance calls, null arguments, late casts, callback failures, cleanup overriding an earlier failure, and mutated aliases. Nine wrong programs validate the independent observer; six same-Code Signature mutations test rejected bindings. Native checks also retain the former closed Exception case as a positive while rejecting missing or invalid exception hierarchies.

The bound-nominal-super-results source and SDK profiles verify exact incoming references and static factory results against a nominal superclass formal bound. Each compiles every emitted Java unit and compares original class and method identities, access and Code roles, Signatures and Exceptions. Four frozen caller routes cover 112 fresh JVM cases twice, including nulls, aliases, wide scalar arguments, partial construction, exact failures, provider mutation and delayed result casts. Nine independent wrong programs and six same-Code Signature controls retain observer sensitivity. Native cases also cover class and interface bounds, closed nominal headers, source and SDK declarations, renamed formals, copied range frames and unproved origins, scopes and overloads.

The protected-single-final-store source and SDK profiles retain a final assignment inside a constructor try region, including checked construction failures and effects after assignment. Four frozen caller routes cover 128 fresh JVM cases twice, null inputs, wide arguments, partially constructed objects, exact checked/runtime/Error identities, callback order and the original wrapped cause. Every emitted Java unit compiles with original class/method identities, Signatures, access and Code roles, and Exceptions. Ten independent wrong programs and an exact same-Code constructor-access control guard the observer and private scope. Native coverage includes one or several ordered fields, source and SDK constructors, typed/catch-all handlers, copied range frames, illegal skipped/repeated writes and valid same-owner private construction.

The closed-covariant-bound-result source and SDK profiles retain a result-only formal bounded by a unary covariant carrier whose element has a plain interface bound. Four frozen caller routes cover 216 fresh JVM cases twice, an independently scoped all-star caller result, direct checked propagation and wrapping, null inputs/results, virtual overrides, returned identity, callback mutation and exact checked/runtime/Error identities. All emitted Java compiles with original definitions, Signatures, Code/access roles and Exceptions. Ten wrong programs challenge the observer; eight same-Code Signature controls challenge incomplete or incompatible bindings. Native coverage also checks static calls, plain class bounds, range frames, absent Exceptions and source/SDK declaration order.

The closed-context-result source and SDK profiles compile every emitted Java unit with all original definitions, roles, Signatures and Exceptions. Four frozen caller routes exercise 320 fresh JVM cases twice across virtual, static, wrapped, independently generic and closed-token calls. They observe opaque results, context aliasing and mutation, future overrides, partial effects, null timing, delayed casts and exact checked/runtime/Error objects. Thirteen wrong programs and six exact same-Code classfile Signature controls qualify the observer; source controls independently decode the DEX annotation, and SDK controls retain the identical original DEX. Native checks cover unary/binary carriers, declaration order, independent scopes, wide/range arguments and malformed bounds, access, exceptions or operands. Formerly unsupported closed Class<String> contexts and independent Object inputs retain explicit positive coverage; frozen runtime callers check that a class token cannot invent a result cast.

The closed-head-member source and SDK profiles compile every emitted Java unit and compare all original definitions, Code/access roles, Signatures and Exceptions. Four frozen caller routes exercise 224 fresh JVM cases twice across interface, concrete, casted-result and checked-handler calls. They observe token and result identity, null timing, future overrides, callback mutation, replacement results and exact checked/runtime/Error objects. Nine wrong programs qualify the observer, while six same-Code Signature controls reject incomplete input/result bindings without altering unrelated metadata. Native cases include custom carriers, all three closed head forms, independent method scopes, direct/link/reversed order, range calls and invalid declarations, exception ancestry or operands.

The static-final-resource source and SDK profiles independently witness a protected static store and branching suppressed-exception path in the original DEX. Each compiles every emitted Java source with exact definitions, Code/access roles, Signatures and Exceptions, then compares 72 JVM cases with two fresh class-loader attempts per case. Four frozen caller routes check reentrant field reads before and after assignment, null resources and values, callback dispatch, resource identity, exact checked/runtime/Error failures, self-suppression, suppressed-list order and sticky initialization failure. Nine wrong programs qualify the observer. Native cases cover one or two static finals, primitive/wide/reference values, two cleanup encodings, range calls, link order and rejected skipped, repeated or invalid writes.

Dependency-reader regressions construct classfiles across the Java 1.1–17 version boundary and test both class-header and InnerClasses roles. Legacy interfaces with implicit or explicit abstract flags link without invented methods. Concrete classes remain concrete; modern missing-abstract and final-interface guards remain covered independently.

The array-star-nested-member source and SDK profiles compile every emitted Java unit with exact original class/method identities, access and Code roles, Signatures and Exceptions. Four frozen caller routes exercise 384 JVM cases twice across interface, concrete, fixed-context, checked-handler/finally and static call chains. The observer checks opaque nested results, array, context and seed identity, null/empty arrays, array mutation, virtual overrides, delayed result selection and exact checked/runtime/Error failures. Seventeen wrong programs qualify the observer; eleven same-Code Signature controls reject unproved contexts, variance, scopes, ranks and missing declarations. An independent DEX witness confirms the original interface invocation. Static chains retain both the generic producer and the same-owner private nested conversion, including the final result container and its input identity. Native coverage also includes custom carriers, fixed-arity declarations, range calls, declaration order and missing or malformed metadata and operands.

The protected-provider variant of reifiable-context-throws uses fixed-array protected static entries in the original compiler input. Source and SDK profiles retain every declaration, access flag, generic signature, exception and runtime role, compile all emitted Java, and run the same 80 JVM cases twice, 14 wrong programs and eight Code-preserving signature controls. Native cases also cover Type/Object/configuration inputs, independent caller formals, interface default callers, cross-package subclasses, range frames, absent/empty/checked exception declarations and inaccessible foreign callers.

The closed-throws variant of null-constructor-arguments compiles source and dependency-provider recovery and compares all original class/method metadata, generic signatures and Exceptions attributes. Four frozen entry routes cover initialization, successful construction and exact checked/runtime exception identities in 64 fresh JVM cases, each repeated twice. Six independently compiled wrong programs exercise changed overloads, non-null arguments, missing/repeated effects and replaced/swallowed checked failures. Native controls retain nested closed input trees, canonical exception ancestry, caller coverage, null origins and original register frames.

The protected-provider variant of context-carrier-token compares every emitted Java unit and original definition/metadata in source and SDK profiles. Seventy-five fresh JVM cases, repeated twice, preserve context and token identity, callback timing, live field reads, opaque carrier identity, ignored results, delayed element casts and exact failures. Thirteen independently compiled wrong programs must fail the immutable observer. Native coverage includes standard Class/List carriers, independent caller formals, closed owner/context headers, accessible subclasses and interface callers, and rejects invalid variance, arity, scopes, access, roles and frames.

The scoped-default variant of interface-token-result-throws verifies generic interface inheritance, default delegation and independent class/method formals in source and SDK profiles. All emitted Java compiles with exact original definitions, Code/access roles, Signatures and Exceptions. Four frozen caller routes exercise 192 fresh JVM cases twice, including future overrides, opaque tokens, context and boolean identity, nulls, callback mutation, late result casts and exact checked/runtime/Error failures. Twenty independently compiled wrong programs qualify the observer; ten same-Code Signature controls reject broken scopes and relations. Native cases add abstract/default declarations, inherited references, renamed formals, range frames, closed exception variants and malformed metadata.

Scoped default source recovery uses Java 9 for the existing private interface exception helpers; dependency-provider recovery uses Java 8. Signature controls change the child interface entry actually called by emitted code in both modes, retaining all original Code bytes.

The linear-prefix variant of instanceof-constructor-prefix witnesses a branch-free DEX sequence with a static factory, type test, virtual selector and original constructor delegation. Source and SDK profiles compile all emitted Java 25 units and compare original identities, access/Code roles, Signatures and Exceptions. Sixty fresh JVM cases run twice, including nulls, subtype matches, opaque virtual results, partial final stores and exact failures before initialization. Twenty-one independent wrong programs check type-test values, receiver dispatch and missing or repeated effects. Native checks cover both this and super, copied/overlapping operands, reference and primitive-array tests, range frames and invalid receiver origins, type descriptors, roles and arguments.

The object-root-covariance narrowed-child source and SDK-parent profiles retain
both parent-return and Object-return clone bridges over a generic parent. Every
emitted Java source compiles with exact original definitions, generic metadata,
Exceptions and bridge roles. Eighty cases run twice in fresh JVMs through typed
calls, reflection and method handles, including an independently compiled future
override, null results and exact checked failures before and after the parent
call. Ten wrong programs qualify these observers. Native tests include closed
and open parent bindings, alpha-renamed scopes, high range frames, declaration
order and missing or malformed forwarding and exception proofs.

The constructor-overload-argument nominal-sibling source and SDK-provider
profiles keep String arguments beside an original Class<?> overload. Every
emitted Java source compiles with exact original metadata and Exceptions. Sixty
cases run twice in fresh JVMs and fifteen wrong programs qualify overload
selection, nulls, evaluation order, escaped receivers and exact failures. Native
controls cover closed generic interface headers, this/super delegation, checked
throws, range frames and open, missing or overlapping declarations.

The variant also retains both original superclass constructors, so the String
view is checked on direct super delegation as well as same-owner delegation.

SDK argument headers and same-arity constructor siblings are resolved through
the canonical scope and erasure validator before this proof is consumed.

The constructor-overload-argument ancestor-input profiles check source and SDK
class chains beside unrelated Class<?> constructor siblings. Seventy-two fresh
JVM cases run twice and twenty-three wrong programs qualify the frozen observers.
Direct, reflective and method-handle calls retain parent selection, input aliases,
partial initialization, null failures and field-versus-virtual dispatch. Native
controls retain checked throws and reject missing, cyclic, open or applicable
ancestor chains. Every emitted Java source compiles with exact original metadata.

The closed-super-reference-result variants of the nominal-super-result runner
cover closed generic argument and result substitution through source and SDK
parents. Four frozen caller routes execute 192 fresh JVM cases twice, including
consumed and discarded polluted results, future-subclass dispatch, original
casts, aliases, staged effects and exact checked/runtime/Error objects. Every
Java unit and original definition/Code/access/Signature/Exceptions entry is
checked. Eleven wrong programs and eight same-Code Signature controls qualify
the observers; native tests also cover transitive parents, nested carriers,
range frames, input order and refusal of incomplete scopes or unproved words.

The same closed-super-reference fixture also compiles a later parent with a
more specific overload while retaining the original child/caller class files.
Its original runtime matrix must remain identical; recovery refuses the
competing source binding instead of changing the invoked descriptor.

The enum-token-default string-context variants cover static and instance
providers as source and SDK declarations. Four frozen caller routes verify
144 static or 156 instance fresh JVM cases twice, with every recovered Java
unit compiled and original metadata/Code/access/Exceptions entries compared.
Six same-Code Signature controls retain recursive-bound, token, default,
context and result ownership checks. Six wrong static programs and eight
instance programs qualify observers, including a future override, null receiver,
opaque results, mismatched enum classes and delayed failing result casts.

The anchored-seed-nominal source and SDK profiles retain an abstract generic
method, concrete implementation and static callers with checked exceptions.
Four frozen caller routes exercise 156 fresh JVM cases twice, including future
overrides, null tokens and receivers, discarded results, original value casts,
staged writes and exact exception objects. All emitted Java compiles with exact
original definitions, Code/access roles, Signatures and Exceptions. Ten wrong
programs and eight same-Code Signature controls qualify the observer. Native
controls also cover alpha-renamed scopes, high range frames, input order and
incomplete carrier, method-role and exception proofs.

The closed-access-marker stateful variants retain a compiler-generated switch
table and an owned enum dependency. Frozen observers check constructor forwarding
before and after successful or failed marker initialization, while every emitted
source compiles and original method roles are accounted. Native controls cover
static fields, initialization and utility bodies, range words, declaration order,
and rejection of instance state or a changed enclosure.

The stateful marker observer requires the original synthetic static-final field
flag. Its generated metadata utility must preserve every method body and all
other class bytes, be idempotent, and reject wrong names, types, duplicate rows
and a final-only recipe without changing compiled classes.

The closed-access-marker runner's `--checked-throws` profiles preserve original
IOException metadata on the private target, synthetic constructor, builder and
callback. Empty and stateful marker variants compile every Java source, compare
all original definitions and metadata, and run 64 or 72 fresh JVM cases twice
per source/SDK profile. Frozen observers check exact exceptions, partial fields,
raw collection identity and marker initialization. Ten or fifteen wrong programs
must fail. Native cases cover multiple closed carriers, wide frames, broader
caller exception declarations, late binding receipts and rejected open or
malformed throws and argument words. Both runtime variants retain the owner's
original nongeneric interface implementation; native cases require complete
parent-interface declarations and preserve the existing ordinary constructor
overload-separation negatives.

The `--nested-owner --stateful-marker` profiles use a private static member
whose access constructor shares the outer owner's anonymous switch marker.
Both original and rebuilt programs use frozen builder and reflection callers;
reflection verifies private/static nesting and reads the partially initialized
member's fields. Checked and unchecked source/SDK variants compile every Java
unit and compare all original definitions, Code roles, Signatures and Exceptions.
They run 72 fresh JVM cases twice with fourteen or sixteen independently
compiled wrong programs, including changed private-member visibility. Native
cases cover high/wide frames, empty or stateful markers and checked contracts;
missing or external enclosing owners, foreign markers, open scopes, instance
state and changed forwarding words remain rejected.

The `--argument-tail` source and SDK profiles retain an unconditional forward
jump from allocation to a tail containing a filled reference array and nested
StringBuilder construction. Independently authored Java and ASM inputs execute
against the same frozen caller; D8 must retain the jump, unrelated switch sibling,
array and nested-allocation witnesses. All Java compiles with exact original
class/method/Code/access/Exceptions accounting. The observer checks class
initialization before argument effects, first and repeated failures, captured
array elements, later argument mutation, partial stores and exact catch/cleanup
failures. Ten independently compiled wrong programs must be rejected. Native
cases cover jump encodings, range/wide words, direct and linked order, and
reject extra entries, receiver aliases and malformed array results.

The `--seed-comparator` profiles add one method-owned value and a complete
contravariant Comparator input. Source and SDK modes compile all emitted Java,
compare every original definition and metadata value, and retain the existing
144-case matrix twice. Frozen direct, compiled, reflection and MethodHandle
callers also exercise both SDK binarySearch overloads with null/empty/covariant
arrays, insertion points, natural ordering and exact comparator exceptions.
Opaque owned providers must retain callback identity without inspecting or
invoking it. Eight independently compiled wrong programs and seven same-Code
Signature controls qualify these checks. Native controls cover alpha-renaming,
wide/range words, ignored results and rejection of open or unrelated seeds.

The interface-token-context `--instance-member` source and SDK profiles compare
128 fresh JVM cases twice through direct, compiled, reflection and MethodHandle
callers. An opaque interface context and separately compiled future provider
check receiver/token/result identity, dispatch, callback writes, original bound
casts, later casts and exact checked/runtime/Error failures. Every emitted Java
unit compiles with exact original class/method/Code/access/Signature/Exceptions
accounting. Nine wrong programs and seven same-Code Signature controls must fail.
Native cases cover nominal class/interface bounds, alpha names, wide high frames,
used or ignored results, final/nonfinal methods, declaration order and rejection
of incomplete scopes, exception hierarchies, method roles and argument words.

Bounded-scalar-transfer source and SDK profiles compile all Java and account for
original definitions, Code roles, Signatures and Exceptions. Five frozen callers
run 160 fresh JVM cases twice, including a same-owner forwarding overload, full
integer and boolean values, opaque/null/empty/aliased lists, callback writes,
exact checked/runtime/Error failures and delayed result and element casts.
Nine independently compiled wrong programs and eight same-Code Signature
controls must fail. Native cases cover class/interface bounds, scoped names,
carrier position, wide high frames, used/ignored results and incomplete bounds,
carrier declarations, exception hierarchies, method roles and invocation words.

Private-sibling-constructor profiles retain an owned private context with a
closed generic interface header and a List of another private sibling. Source
and SDK callback variants run 64 fresh JVM cases twice through direct, compiled,
reflection and MethodHandle callers. All Java compiles with original definitions,
Code/access, Signatures and Exceptions accounted, including individually
disclosed marker-name and synthetic access projections. Observers check partial
fields, raw identities, exact failures and delayed casts; ten wrong programs and
four same-Code Signature controls must fail. Native cases cover nested carriers,
wide high frames, checked and unchecked constructors, declaration order and
rejection of external, foreign, missing or incomplete lexical contexts.

Token-carrier-throws source and SDK profiles compile all emitted Java and account
for original definitions, Code roles, access, Signatures and Exceptions. Four
frozen callers run 144 fresh JVM cases twice, including null and mismatched
tokens, opaque and empty lists, mutable buffer contexts, exact failures and
later element casts. Eleven wrong programs and eight same-Code Signature
controls qualify the observers. Native cases cover standard and owned carriers,
closed context headers, alpha names, token positions, wide high frames, used or
ignored results, source/SDK declaration order and incomplete scopes or frames.

The token-carrier-throws `--private-member` variants retain a Java 8 compiler
synthetic accessor between a nested caller and its private static target. Source
and SDK profiles keep the 144-case matrix twice, eleven wrong programs and eight
same-Code Signature controls. Every original definition and metadata value is
accounted, allowing only the individually disclosed accessor synthetic flag.
Native cases cover ordinary generic and raw synthetic callers, declaration
order, standard/source/SDK carriers, wide frames and rejection of foreign or
mixed caller owners, unavailable bodies, open scopes and changed roles.

Exception-only-monitor source and SDK profiles compare 64 fresh JVM cases twice
through direct, compiled, reflection and MethodHandle callers. Independent D8
inspection requires exactly one physical release for each exceptional-only
monitor. Every emitted Java unit compiles with original definitions, Code roles,
access flags, Signatures and Exceptions accounted. Bounded contention verifies
the original lock and owner; callbacks check held locks, partial effects and
exact checked/runtime/Error identities. Twelve wrong programs must fail. Native
cases cover high frames, copied lock aliases, checked source/SDK callbacks and
reject uncovered exceptions, changed aliases, held returns, malformed captures,
bypasses, repeated releases and unreachable operations. An unlocked suffix must
remain outside any source synchronized method modifier.

Array-length-constructor-prefix profiles compile original Java 8 inputs and
all recovered Java 25 source. Source and SDK variants run 80 fresh JVM cases
twice through four frozen caller routes, with exact original definition,
Code/access, Signature and Exceptions accounting. Independent D8 inspection
qualifies the callback, length read, this-call and continuation order. Observers
check input identity, empty/null arrays, partial final fields, callback changes
and exact checked/runtime/Error failures. Fourteen wrong programs must fail.
Native cases cover primitive/reference/multidimensional arrays, high and wide
frames, reused length destinations, source/SDK declarations and rejection of
uninitialized receiver use, malformed operands and changed delegation words.

Conditional-array-arguments source and SDK-provider profiles compile all emitted
Java 14 units and account for original definitions, flags, Code, Signature and
Exceptions. Four frozen caller routes run both branches in sixteen modes, twice
per fresh JVM. Independent D8 inspection requires ordinary array creation and
two reference stores inside the pending allocation. Observers retain class
initialization, receiver replacement, partial writes, final fields, opaque array
results and exact checked/runtime/Error identities. Sixteen wrong programs must
fail. Native cases cover primitive/reference/multidimensional allocation, high
and wide frames, reused length words and declaration order; malformed operands,
receiver exposure, invalid store types and incomplete control flow are rejected.

Unary fixed-result callback variants extend the owned binary-callback-carrier
fixture before compilation and freeze its resulting classfiles and callers.
Each source/SDK and invariant/covariant profile runs 96 fresh JVM cases twice,
compiles every emitted Java unit and compares complete original metadata,
including Code presence and MethodParameters. Eight wrong programs and six
same-Code Signature changes must be detected. Native cases cover closed nominal
completions, inherited callback contracts, public facades over package owners,
high range frames and declaration order; incomplete scopes, reversed variance,
changed arity, unproved roles and malformed call words remain rejected.

Recursive-pair-result profiles compile all source/SDK output units and compare complete class/method/Code/access/Signature/Exceptions/MethodParameters metadata. Four frozen calling routes cover raw and checked results in 128 fresh JVM cases twice, including null/aliased inputs, independent results, provider and later caller casts, partial effects and exact runtime/Error identity. Ten wrong programs and eight Code-preserving Signature controls exercise the observer and generic boundary. Native tests cover direct, linked and reversed input order, source/SDK declarations, public facades over package-private providers, renamed formals, ignored results and high register frames with unused wide arguments.

Bounded-static-self-result profiles preserve independent namesake class/method formals, a generic static nested base and a methodless interface. Source and SDK variants compile every emitted unit and compare complete original class/method/Code/access/Signature/Exceptions/MethodParameters facts. Four immutable calling routes exercise raw and later interface-checked results in 96 fresh JVM cases twice, with ten wrong programs and eight Code-preserving method/class Signature controls. Native checks cover source/SDK scope resolution, generic inheritance, facades, renamed method formals, ignored results, high registers and unused wide input words.

Recursive-array-carrier-result source and SDK profiles compile every emitted unit and compare complete original class/method/Code/access/Signature/Exceptions/MethodParameters facts. Four frozen caller routes compare raw container returns and later checked element reads in 128 fresh JVM cases twice, covering null/empty/covariant arrays, callback stores, opaque results and exact failures. Ten wrong programs and eight Code-preserving Signature controls exercise behavior and binding. Native checks cover both declaration origins, inherited facades over package-private providers, renamed formals, ignored results, high registers with unused wide inputs and declaration order.

Nullary-token-super-constructor profiles preserve a private no-argument child, two independent closed superclass slots and an original Class token. Source/SDK layouts compile every emitted unit and compare original definitions and metadata. Four frozen routes run 48 fresh cases twice, checking class resolution without initialization, missing token classes, partial parent/child state, callbacks and exact failures. Ten wrong programs and eight Code-preserving Signature controls exercise behavior and scope. Native cases vary visibility, declaration origin, formal names, move aliases, high range frames and declaration order.

Bounded-formal-super-call profiles retain a protected static nested child, independent formal scopes and an inherited method through a package-private intermediate parent. Source/SDK variants compile all emitted units and compare complete original metadata. Four frozen routes and a separately compiled future subclass run 96 fresh JVM cases twice, including raw bridge failures, null values, partial writes, callback mutation and exact checked/runtime/Error identity. Ten wrong programs and eight Code-preserving Signature controls qualify behavior and scope. Native variants cover declaration origins, ancestor qualifiers, alpha-renaming, ignored results, null words, wide incoming frames and link order; broad unproved words, shadowed formals and competing overloads remain rejected. The single reported compiler-regenerated bridge retains its original status and must have the exact normalized cast, virtual invocation and return bytecodes after recompilation.

Nested-visibility-super-bridge source and SDK profiles compile every output unit and compare complete original class/method/Code/access/Signature/Exceptions/MethodParameters metadata. Seven compiler-generated bridges must retain exact normalized loads, superclass invocation and returns. Four frozen routes and future overrides exercise 230 fresh JVM cases twice, including wide integers, raw floating-point bits, opaque references, callback state and exact checked/runtime/Error identity. Raw ancestor, reflection and method-handle routes also distinguish unchanged broad erased arguments from the future subclass bridge's original checked narrowing. Twelve wrong programs and eight Code-preserving Signature controls qualify the independent observer and scope boundary. Native cases cover source/SDK ancestors, near/inherited declarations, scalar/reference/void results, high range frames and input order; lexical ambiguity, malformed forwarding, shadowed scopes and altered metadata remain rejected.

Nested-generic-super-arguments source and SDK profiles compile all output units and compare complete original metadata. Four frozen routes and a future override run 96 fresh JVM cases twice, preserving class initialization before superclass evaluation, wide argument bits, original receiver and result identities, partial construction and exact failures. Twelve wrong programs and eight Code-preserving Signature controls cover order and scope. Native tests vary source/SDK declarations, high range frames and input order; uninitialized receiver exposure, malformed wide words, unsupported method formals and changed dispatch remain rejected.

Owned-package-element access constructor profiles reuse the closed-access-marker
source and SDK callback layouts. They compile every emitted unit and account for
all original definitions, access, Code, Signatures and Exceptions, including
existing marker projections. Four immutable routes run 68 fresh JVM cases twice,
checking raw carrier identity, delayed poisoned-element casts, exact lexical
visibility, partial fields and checked/runtime/Error identity. Eleven wrong
programs and four Code-preserving Signature controls exercise the observer and
binding boundary. Native variants cover package/public owners, package/private/
protected static elements, source/SDK callbacks, high frames and link order;
foreign or dependency-only scope and malformed argument words remain rejected.

Scalar-intersection-argument profiles compile all source and SDK output units
and compare complete original compiler and exception metadata. Four immutable
routes and a future override run 128 fresh JVM cases twice, covering wide words,
raw floating-point bits, nulls, callback field replacement, partial effects and
exact checked/runtime/Error identity. Twelve wrong programs and eight same-Code
Signature controls must fail. A Code-preserving witness-interface change first
qualifies the original late provider failure, then requires static rejection
without introducing an earlier cast. Native cases vary formal position, scalar
width, static/virtual dispatch, inherited qualifiers, high frames, declaration
origin and link order; missing bounds, overloads and malformed words are rejected.

Abrupt-blank-final-constructor source and SDK profiles compile every emitted
unit and compare all original definitions, Code presence, access, fields and
exception metadata. Four frozen routes run 144 fresh JVM cases twice, observing
partial construction, reflective final-field mutations, wide stores, allocated
failures and exact checked/runtime/Error identity. Twelve wrong programs qualify
the observer. Native variants cover absent, partial and complete writes, reversed
store order, high frames, range invocations, source/SDK superclasses and link
order. Returning paths with missing writes, duplicate stores, wrong receivers,
malformed words and unsupported exceptional entries remain rejected.

Mapped-consumer-factory source and SDK profiles compile every output unit and
compare complete original compiler metadata. Four frozen routes run 112 fresh
JVM cases twice, covering an opaque owned factory and the real platform
comparing overload. They observe original inputs, callback order, mutable
snapshots, future implementation bridges, delayed casts and exact failures.
Twelve wrong programs and eight Code-preserving Signature controls qualify the
observer and binding. Native variants cover generic class/interface owners,
independent or shadowed method scopes, invariant/variant arguments, high frames,
unused wide parameters, ignored results, declaration origins and link order;
incomplete relations, class-formal borrowing and malformed words remain rejected.

Stateless-access-marker source and SDK profiles compile every output unit and
compare all original definitions, Code roles, access, Signatures and Exceptions.
Four frozen calling routes run 64 fresh JVM cases twice, observing actual unused
marker instances, private context identity, partial fields, later element casts
and exact failures. They also invoke the same anonymous implementation before
and after construction, preserving callback order, mutation, opaque results and
checked/runtime/Error identity. Sixteen wrong programs and four same-Code
Signature controls qualify the observer and binding boundary. Native checks
cover source/SDK dependencies, high and wide frames, generic implementation
methods and link order; capture state, missing scope and malformed words remain
rejected. The original anonymous constructor must not become a generated helper.

Independent-carrier-factory source and SDK profiles compile every output unit
and compare complete original compiler metadata and Code roles. Four frozen
routes run 112 fresh JVM cases twice across full and null-tail overloads, with
extreme integers, aliased contexts, deferred generic seed reads, opaque results,
partial state and exact checked/runtime/Error identity. Sixteen wrong programs
and eight same-Code Signature controls qualify behavior and scope. Native cases
vary dependency layouts, independent/shadowed formals, generic superclass
contracts, seed position, high/wide frames, ignored results and link order;
missing or cross-scoped formals, reversed variance and malformed words fail.

The integer-constructor-choice fixture compares an independent Java reference
and an ASM original with one allocation before a signed zero comparison. Source
and SDK declarations run in both branch polarities. Each layout compiles all
emitted Java and checks complete original metadata, Code roles, Exceptions and
exact helper accounting. Four frozen call routes run 128 fresh JVM cases twice,
covering integer extremes, cold and warm initialization, sticky failures,
captured inputs despite global mutation, partial final stores and exact handler
identities. Ten wrong programs must fail the observer. Native checks cover all
six unary comparisons, both constructor orders, copied and high registers,
protected and unprotected allocations, source and SDK declarations and all
three linking routes; changed types, words, scopes and interior edges fail.

The final-superclass-parameter-bridge fixture retains a final typed consumer and
its non-final synthetic bridge through an interface-bound generic root and
literal superclass edges. Source and SDK parents each run 216 fresh JVM cases
twice across four frozen routes. All emitted Java compiles; complete original
metadata, Code roles, Exceptions, final and bridge flags, and exact helpers
match. Seven wrong programs fail before recovery. Runtime checks cover inherited
final dispatch, null receivers, cast ordering, long values, class initialization,
identity, effects and exact failures. Native checks also cover class bounds,
final owners, ancestor depth, copied/range words and three linking routes;
final ancestors, invalid roles, changed casts and broken substitutions fail.

The package-carrier-factory fixture exercises two original package-visible
static overloads through public clients in the same package, with source and
SDK declarations. Four frozen routes run 112 fresh JVM cases twice per layout;
seventeen wrong programs fail before recovery, including a publicized factory.
Eight same-Code Signature mutations fail. All emitted Java compiles and complete
original metadata, Code roles, Exceptions and visibility match. Native checks
cover independent method scopes, short/full contexts, high and wide words,
used/discarded results and dependency layouts. A foreign-package call must fail
even after the declaration receives its generic binding receipt.

The seeded-star-property source and SDK profiles cover a direct method seed,
String context and invariant/star binary property through an abstract contract,
concrete implementation and frozen callers. Each compiles every emitted unit,
compares complete original compiler metadata, Code roles and Exceptions, and
runs 192 fresh JVM cases twice. Null and aliased contexts, future overrides,
discarded results, staged effects and exact checked/runtime/Error objects remain
observable. A frozen property implementation delays its first-slot bridge until
a caller reads it after provider completion; its wildcard result is independent.
Fourteen wrong programs and ten same-Code Signature controls qualify the oracle.
Native cases also cover static and concrete/abstract instance declarations,
source/SDK mixtures, class/interface properties, high range words, reordered
input and alpha-renamed scopes. Incomplete bounds, slots, erasures, method roles
and exception declarations must fail before source output.

The anonymous-closed-parent-bridge source and SDK profiles retain an anonymous
subclass of a plain class with a closed generic interface. Two checked arguments
and a float context reach the original typed target through a compiler bridge.
All emitted units compile; complete original compiler facts and method roles
match except the exact reported local-name and capture-constructor mapping.
Five frozen routes exercise 160 fresh JVM cases twice, including future dispatch,
nulls, both cast positions, partial effects, opaque/aliased results, exact
throwables, NaN payloads and signed zero. Sixteen wrong programs qualify the
observer before recovery; eight same-Code class Signature controls retain the
ancestor proof. Native cases cover static and captured anonymous scopes, source
and SDK parents, copied/high words, independent generic base edges and input
order. Open enclosing formals, broken forwarding and changed ancestry must fail.

`test_mobile_android_intersection_constructor_witness.py` checks source and SDK
constructor declarations with a method-owned class/interface intersection. It
compiles all emitted Java and compares complete original metadata, then runs
132 fresh JVM cases twice across direct, reflective, method-handle, nested,
external generic and constructor routes. It covers cold/warm/failed class
initialization, future overrides, nulls, partial effects and exact thrown objects.
Fourteen wrong programs qualify the observer; eight same-Code Signature controls
and a separately executed missing-witness control require rejection. Native
cases vary source/SDK scope, original self/argument words, erasure casts, register
width, range calls, nested allocations, formal names and input order, including
competing constructor overloads and complete-bound failures.

`test_mobile_android_seeded_carrier_constructor.py` validates source and SDK
constructor layouts with a recursive parent, separate method scopes and scalar
overload. Its frozen observer covers direct, reflective, method-handle, nested,
generic-caller and original-receiver entries, future carrier overrides, class
initialization that replaces a static argument, partial construction, float bits
and exact failures. Independent wrong programs qualify those observations;
same-Code Signature controls must retain their exact original constructor metadata.
Every emitted unit compiles, with original class and method identities, Code
roles, access, Signature, Exceptions and parameter metadata compared independently.
Native cases additionally cover seed/carrier order, wide scalars, high/range
registers, alpha-renamed formals, link order and malformed word/contract inputs.

`test_mobile_android_package_bound_scalar.py` checks package abstract bounded
inputs and primitive results in source and SDK layouts. Frozen future overrides
and generic callers exercise original virtual dispatch, receiver capture before
argument evaluation, nulls, private branch/arithmetic paths and exact exceptions.
Wrong programs qualify the observer, and same-Code Signature controls must retain
their exact metadata. Every emitted unit compiles with full original identities,
Code roles, access, Signature, Exceptions and parameter facts preserved. Native
cases also cover all primitive result kinds, range/high registers, inherited
references, alpha-renaming, link order and malformed contracts/access/frames.

`test_mobile_android_closed_visibility_parameter_bridge.py` checks source and
SDK layouts for recursive closed superclass visibility bridges. Its frozen
observer covers direct, reflective, method-handle and generic caller routes,
future overrides, erased inputs, nulls, partial effects and exact exceptions.
Independent wrong programs qualify the observer; exact same-Code Signature
controls must be rejected. Every emitted unit compiles with all original class
and method metadata accounted for, and the compiler-regenerated bridge must
retain its exact normalized forwarding bytecodes and original roles. Native
cases vary result kinds, scalar contexts, register words, link order and formal
names, and reject incomplete bounds, conflicting final or abstract ancestors,
cycles, changed dispatch and lost bridge metadata.
The native matrix includes ordinary callers and rejects an erased caller input
without the original narrower register proof; source recompilation validates the
same parameter view across ordinary and superclass invocation paths.

`test_mobile_android_nested_anonymous_allocation.py` verifies source and SDK
constructor arguments containing an original anonymous callback. A frozen
observer checks initialization order, argument and callback identities, partial
construction, failure wrapping, repeated access and calls on a saved callback
after its enclosing initialization fails. Independent wrong programs qualify
the observer. All emitted Java compiles, and every original class and method,
including anonymous binary/enclosing identity and complete metadata, is compared
independently. Native matrices vary recursive owner scope, argument order,
reference preparations, scalar widths, range calls and lookup order; invalid
ownership, constructor bodies and register frames remain rejected.
The conditional variant keeps a branch and a reference alias between the
anonymous constructor and the outer constructor. Independent DEX witnesses
verify the allocation order, and the same frozen observer verifies that source
recovery creates one original anonymous class and object. The former native
exclusion of these valid literal bodies becomes a positive inventory and unique
allocation check as this source shape gains support.

`test_mobile_android_final_nested_visibility_bridge.py` verifies source and SDK
layouts of a final static member inheriting from a protected static member in a
different enclosing type. Frozen callers outside the defining package cover
direct, public-base, reflective and method-handle dispatch, mutable receiver
and argument suppliers, nulls, wide values, partial state and exact checked,
runtime and Error objects. Independent wrong programs qualify the observer,
including explicit source overrides that lose bridge roles and changes to
final flags, parent visibility and enclosing generic contracts. All emitted
Java compiles; original class and method metadata and normalized original and
regenerated bridge instructions are compared independently. Native cases vary
finality, enclosing scope, plain and generic ancestry, SDK provenance, return
types, wide/range register words and lookup order while rejecting unproved
scopes, metadata and forwarding.

`test_mobile_android_instance_array_carrier_void.py` covers a constructor calling
a public instance method with one formal array and an invariant member carrier.
Source and SDK layouts compile every emitted unit and compare all original
class identities, method Code roles, access, generic contracts and Exceptions.
Frozen external callers cover direct construction, a factory, reflection and
method handles, future subclasses, receiver and argument snapshots, nulls,
partial writes, array-store failures and exact checked/runtime/Error objects.
Wrong programs and unchanged-Code Signature mutations qualify the observer
before recovery. Native checks vary array rank and order, source/dependency
providers, class/interface dispatch, member/ordinary carriers, range calls and
lookup order, while rejecting incomplete contracts and changed register frames.

`test_mobile_android_unused_private_intersection.py` verifies a private method
whose unused formal has two ordered interface bounds, reached through an original
compiler accessor. Frozen callers exercise direct, reflective and method-handle
entries, future subclasses, opaque inputs that implement neither bound, nulls,
receiver snapshots, two ordered virtual calls and exact checked/runtime/Error
objects. Wrong programs and unchanged-Code bound controls run before recovery.
All emitted Java compiles and every original class/method/Code role is compared.
Only the existing ordinary accessor synthetic-bit loss is admitted by its exact
reported identity and flag pair; visibility, static/bridge roles, private generic
metadata and all other contracts remain checked independently.

The `intersection_super_forwarding` owned fixture runs in source and SDK-provider
layouts. Frozen direct callers, reflection and method handles enter an override
through ordinary and future receiver classes. Nulls, invalid erased arguments,
ordered partial effects and exact checked/runtime/Error identity are observed
independently. Qualified wrong programs and unchanged-Code Signature mutations
must fail those observers before recovery. Every emitted Java unit compiles and
all original class/member identities, Code roles, access, Signature, Exceptions
and parameter metadata are compared. Native tests cover separate lexical formal
names, inherited aliases, register and range forms, unused source overrides, and
rejection of lost origins, reentry, missing bounds and competing overloads.

The `captured_intersection_witness` fixture combines an owned private captured
member, a closed generic superclass and an intersection consumer. Source and SDK
layouts retain frozen callers and check original outer identity even during the
superclass constructor, ordered getter/field/array arguments, virtual dispatch,
partial effects and exact throwable identity. All emitted Java compiles at the
existing Java 25 capture boundary. Exact established capture-field synthetic and
member static metadata differences are disclosed; all other class/member/Code,
access, Signature, Exceptions and parameter metadata must match. Wrong programs
and unchanged-Code class Signature controls independently qualify the observer.
The captured witness fixture also fixes the existing private capture constructor's
implicit-parameter disclosure to its exact four original unnamed records and absent
compiled attribute. Every other constructor or parameter attribute stays exact.

The `private_prefix_constructor_factory` fixture checks two private result-producing
calls before a protected interface-bounded superclass constructor. Frozen direct
constructor callers, reflection and method handles observe future returned
subclasses, exact float bits, nulls, opaque result snapshots, partial construction,
later boolean stores and exact throwables. Both source and SDK layouts compile all
Java and compare every original identity, Code role, access, Signature, Exceptions
and parameter record. Wrong programs and unchanged-Code parent Signature controls
qualify observers before recovery; native cases retain all scope, bound, dispatch,
origin and overload rejection checks.

The `closed_visibility_result_bridge` fixture checks a public final child of a
package generic parent with an interface-bounded result. Source and SDK layouts
compile every Java unit, compare all original metadata, and independently verify
both original and regenerated bridge bytecodes. Frozen external callers,
reflection, method handles and rebuilt callers distinguish an erased result from
caller-side narrowing, including polluted fields, callback mutation, reentry,
future result dispatch and exact throwable identity. Qualified wrong programs
and unchanged-Code class/method Signature controls run before recovery.

The `branched_interface_super_accessor` fixture uses original Kotlin compiler
output with two direct parents and a default on the second branch. Frozen
callers, reflection, method handles and a rebuilt caller observe original static
compatibility entries, explicit default dispatch and virtual calls inside the
default, wide argument words, result values, reentry and exact throwable identity.
Source and SDK trace layouts compile all emitted Java and check every original
entry and Code role, with exact established annotation and helper disclosures.
Native cases cover both direct-parent orders, inherited declaration depth and
rejection of conflicting, incomplete or unqualified graphs.

The `nominal_interface_super_forwarding` fixture retains original Java forwarding
Code while encoding only its bridge flag. Source and SDK trace layouts compile
all Java and check every original class, method, Code role, access, Signature and
Exceptions record. Frozen callers, reflection and method handles distinguish the
original direct interface default, virtual callbacks inside it, argument/result
identity, raw input bits, reentry, partial construction and exact throwables.
Independent wrong programs qualify the observer before recovery; native cases
cover original qualifiers and reject unproved nominal slots and graph changes.

The `mixed_interface_super_forwarding` fixture adds a package final child of a
named static parent, shared interface ancestry and reference/scalar overloads.
Source and external-provider layouts compile every emitted unit and compare all
original class, method, Code-role, Signature and Exceptions records. Frozen
callers, reflection and method handles observe reference aliases, wide bits,
virtual callbacks, reentry, partial effects and exact throwable identity.
Independent wrong programs qualify those observations; compiler controls reject
inherited interface qualifiers and final superclass overrides. Native cases
cover multiple result kinds, call encodings, visibility and complete ancestry.

The `void_interface_super_forwarding` fixture covers the receiver-only void
frame, a default on the second direct interface, and a named static superclass.
Frozen callers, reflection and method handles observe wide state changes,
callback order, reentry, partial effects and exact throwables. Both source and
external-provider layouts compile all emitted Java and preserve every original
class, declared method and Code role with the exact bridge-bit disclosure.
Independent wrong programs and superclass compiler controls run before recovery;
native cases retain the original call even when the default body is empty.

The `object_prefix_defaults` fixture combines a closed contravariant callback
parameter, a linear static guard before Object preparation, an original callback
store and unwritten private boolean, long and reference-array final fields. Only
the three original final bits are encoded; every Code byte and other attribute
is preserved. A separate D8 input copy changes only the Java 25 version header
and is checked independently. Source and external-guard layouts each run 60
fresh JVM cases through authored, final-bit encoded, D8-compatible and rebuilt
stages, with two attempts and 18 independent wrong programs. An independent
javac control rejects a foreign private guard. Frozen direct
callers, reflection and method handles check exact failures, prefix order,
reentry, partial state, opaque callback identity and reflective final-field reads.
All emitted Java and original metadata are checked. Native coverage includes 96
positive and 120 negative combinations over linking order, source/SDK guards,
receiver aliases, scopes, uninitialized-this escapes and known/unresolved stores.

The earlier copied-receiver preparation matrix now checks the canonical
prologue receipt and exact nonconstant default for its local `const/4` case.
That valid linear prefix is newly supported; every other original malformed
receiver, operand, dispatch and control-flow case retains its rejection.

The `paired_seed_completion` fixture checks two ordered method input formals
and a nested completion through an inherited public static qualifier. Source
and external-provider layouts run 80 fresh JVM cases at original and rebuilt
stages with two attempts each, eighteen wrong programs and three unchanged-Code
Signature controls. Frozen direct callers, reflection and method handles verify
nulls, aliases, opaque results, deferred completion, reentry and exact checked,
runtime and Error identities. All emitted Java, original metadata and Code roles
are checked. Native coverage has 96 positive and 150 negative combinations over
source/SDK declarations, inherited qualifiers, method-formal renaming, linking
order, generic bounds, ordered arguments, variance, scope and access.

The `private_instance_field_names` fixture uses a Code-preserving constant-pool
name encoding for a final reference, mutable generic callback and volatile long.
Source and imported-observer layouts run 80 fresh JVM cases through authored,
encoded and rebuilt stages, twice each. Frozen direct, reflection and method-handle
routes check two independent receivers, constructor partial effects, nulls, wide
extrema, aliases, reentry and exact checked/runtime/Error identities. Eighteen
independent wrong programs must fail. All generated Java and original metadata
are checked, permitting only three disclosed field names. Native checks cover
24 source-scope/link-order combinations and 48 malformed access/storage cases;
all earlier static-field naming tests remain unchanged.

Scoped mask constructor profiles with `--scoped-interfaces` add an invariant
interface diamond and a nominal marker to the original Kotlin owner. Source and
external-default layouts retain all compiler-generated constructor Code, run the
128-case frozen observation matrix twice, compile every emitted Java unit and
check all original identities, generic headers, metadata and Exceptions. Four
independent wrong Kotlin programs run before decompilation. Native variants use
an erased mask entry that leaves a covariant callback unchanged and replaces its
ordinary reference context, checking 48 public/package, source/SDK, direct/range
and linking combinations plus 144 incomplete graph or delegation cases.

The fixed bounded-mask regression also retains the previously rejected package-visible final owner as a positive proof: the canonical delegation and generic call agree on the original target, the owner Signature is unchanged, and every method recovers. All other malformed-header and invocation-role cases keep their original rejection assertions.

Wide scalar getter naming is covered by the genuine Kotlin carrier-accessor
runner for long and double, with source and imported-trace layouts. Frozen
observers check raw values, both words, NaN payloads, signed zeros, comparison
calls, side effects and exact failures. All generated Java files and original
class/method/Exceptions metadata are checked after only the existing explicit
name, synthetic flag and checked Comparable projection disclosures; D8 CLASS
annotation loss is proved separately. Native cases cover direct and linked
class maps, reversed input order, private/public fields, unrelated dependency
name collisions, malformed words and receiver slots, field/result disagreement,
and missing scope or field proofs. Existing float profiles remain regression
coverage, including the imported-trace layout.

Double comparison cases also verify the canonical carrier bridge and optional getter spelling together. Register-pair swaps, missing words, receiver overlap, scalar type disagreement, changed scope and access all remain rejected. The genuine double Kotlin profile independently exercises its comparison bridge before the accessor naming proof.

The inlined-accessor-names fixture retains byte, long and double getter Code beside a complete erased comparison body. Only explicit class Signature, method flags and UTF8 names encode the JVM witness. Frozen observers compare authored, encoded and rebuilt programs, including return-descriptor overloads, raw floating bits, unsigned comparisons, two-word values, null/type failures and exact runtime/error identities. Source and imported Api/Trace layouts compile every Java unit and audit all original declarations and Exceptions. Native tests cover all eight scalar types, known scope receipts, input order, SDK declarations, unrelated name collisions and incomplete field, scope or frame evidence.

The terminal-array-accessor-names fixture freezes original byte, short, int and long array getters beside a parameter-independent throwing generic consumer. Explicit input Signature, flag and UTF8 edits preserve every original Code attribute. Authored, encoded and rebuilt programs must match through direct, precompiled, reflection and MethodHandle callers, preserving aliases, null/empty arrays, payloads, mutation visibility, effect order and exact failures. Every Java source and original declaration/Exceptions entry is checked in source and imported Api/Marker/Trace layouts. Independent wrong programs and native neighboring scope, field, frame and reference-array cases guard the admission boundary.

The terminal-scalar-entry-names fixture covers nongeneric scalar methods beside an erased terminal consumer: unsigned byte results, both words of long values, raw double bits, ordinary effectful bodies and parameter-independent throws. Four frozen caller routes compare authored, exactly encoded and rebuilt programs, including null receivers and exact runtime/error identities. Source and imported-support layouts compile all Java and check original metadata and Exceptions. Native cases cover all scalar kinds, one/two parameters, terminal and returning bodies, unrelated dependency names, order independence and unproved reference or generic neighbors.

The boxed generic interface result runner's mangled-target profile changes only the scalar provider's UTF8 spelling and its original symbolic references, retaining every Code attribute. Existing direct, interface, reflection and MethodHandle observers check original scalar words, factory result identity, state, callback order and exact failures. Eight independently compiled wrong programs must fail before recovery. Source and imported-root layouts compile all Java and audit exact method-name disclosures, access, fields, Exceptions and helper inventory. Native coverage includes all scalar kinds, effectful providers, package and private static member owners, dependency collisions, input order and incomplete provider/bridge/scope proofs.

The array-fixed-reference-result fixture preserves a generic array input and fixed Object[] result through an inherited static provider. Four frozen caller routes exercise 88 fresh JVM cases twice, with returned, narrowed and ignored results. Observers check runtime component types, copied versus reused arrays, nullable replacement identities, callback writes, declaring-class initialization and exact runtime/error failures. Sixteen independently compiled wrong programs and five same-Code Signature controls guard the oracle and binding boundary. Source and imported-provider layouts compile all Java and audit every original definition, Signature, field and Exceptions entry. Native cases cover fixed Object/String components, all scalar widths, source/SDK providers, public/package declarations, inherited lookup, direct/range calls and ordering, plus unproved components, scopes and frames.

The array-closed-context-void fixture retains an original private generic recursive helper with a method-owned array, a fixed builder and an invariant fixed-array carrier. Four frozen caller routes exercise 96 fresh JVM cases twice, covering cycles, shared children, reference identities, callbacks, array-store failures, string conversion, partial builder/history state and exact runtime/error instances. Sixteen independently compiled wrong programs and five same-Code Signature controls validate the observer and admission boundary. Source and imported-context layouts compile every emitted Java file and compare complete original metadata, Exceptions and helper inventories. Native variants cover public/private entries, package owners, SDK contexts and public providers, scalar word pairs, parameter order and complete fixed components; unproved bindings and foreign/private alias calls remain rejected.

The package-array-factory fixture keeps a package method and package owner with a generic array input and invariant collection result. Four frozen caller routes run 80 fresh JVM cases twice across used, narrowed and ignored results. The observer checks array and result identities, backing-array writes, package access, callback order, partial effects, null and cast failures, and exact checked/runtime/error propagation. Sixteen independent wrong programs and six same-Code Signature mutations test the observer and binding boundary before and after recovery. Source and imported-provider layouts compile all emitted Java and audit every original definition, Signature, field, Exceptions entry and generated throw helper. Native cases cover source/SDK providers and carriers, formal renaming, owner visibility, direct/range calls and class/method order; foreign-package and aliased calls remain rejected even beside a valid local call.

The destination bounds fixture recompiles all emitted Java with both source
and imported providers. It checks array and iterable writes, checked exception
identity after a partial write, and the runtime component type and range
errors of a copy controlled by a separate array type token.

The destination-bounds fixture also calls static methods through a subclass
of a package-private provider. It checks recursive comparable bounds, a
broader generic array overload, a partially written array and a checked exception in both
the program and dependency layouts.
