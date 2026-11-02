# Instruction-boundary x64 IRQ/NMI scheduling

`scheduled_events: true`, together with `event_delivery: true` in
`kernel-events-v6`, exports `neverd_x64_continue_events_v6`. It polls an explicit
producer before each selected original instruction, delivers eligible IRQs or
NMIs through the existing LowIR event owner, and stops at handler entry. Both
HighC and LLVM-to-C use the same instruction bodies, entry guards and scheduler.
The original instruction-only drivers remain available.
`paged_memory: true` instead of `event_delivery: true` selects the
[faultable paged scheduler](x86-paged-execution.md#paged-irqnmi-scheduling),
which reuses this scheduling owner with explicit paged runtime bridges.

```json
{
  "profile": "kernel-events-v6",
  "mxcsr_mask": "0xffbf",
  "paging_address_bits": 48,
  "event_delivery": true,
  "scheduled_events": true,
  "follow_internal_calls": true,
  "instructions": [4096, 4097, 4098]
}
```

The generated interface is:

```c
uint64_t neverd_x64_continue_events_v6(
    neverd_x64_state_v6 *state, uint64_t *instruction_budget,
    neverd_x64_interruptibility_v1 *shadows, uint64_t *poll_budget,
    neverd_x64_peek_interrupt_v1 peek,
    neverd_x64_accept_interrupt_v1 accepted, void *context);
```

`shadows` holds Boolean `STIBlocked`, `SSBlocked`, `NMIBlockedBySTI`, and five
zero reserved bytes. These cells persist across calls and budget stops.
`State.NMIBlocked` retains its existing architectural owner. A supplied STI
shadow requires IF=1. Both callbacks are required:

- `peek(const state *, const shadows *, pending *, context)` returns zero for
  no pending event or one with an interrupt `Kind` and `Vector`. Kind 2 is an
  IRQ; kind 3 is an NMI and requires vector 2. Other results are rejected.
- `accepted(const pending *, context)` acknowledges the event exactly once
  after successful delivery. It cannot fail. A blocked event, rejected event
  or instruction-generated exception never calls it.

The producer owns arrival, priority, APIC/PIC/CR8 eligibility and coalescing. A
blocked candidate remains pending there; a later poll can select a different
eligible event. `peek` may update its own bookkeeping but cannot change state,
shadows, budgets, instruction bytes or guest RAM. `accepted` only acknowledges
the selected event in producer storage. Mutable producer storage, state,
shadows and budgets are disjoint from each other and guest memory.

Each poll consumes one poll-budget unit, including no-event and inhibited
responses. Status 16 means another poll is required but its budget is exhausted;
the boundary has executed nothing. Status 9 retains instruction-budget meaning.
The instruction's actual entry guard runs before polling. An invalid entry,
unselected PC or exhausted instruction budget never invokes a callback.
Invalid callback data returns status 5 without instruction/state/RAM effects;
the already performed poll remains charged. Delivery retains the existing
successful descriptor/TSS/stack RAM contract.

Saved PC comes from the next original instruction. Successful delivery returns
7 without charging an instruction. A synchronous exception charges its attempted
instruction, returns 7, and leaves a deferred external event unacknowledged.
CALL/RET use original RAM and the export's existing internal-call policy.

Canonical STI starts inhibition only when its incoming IF was zero. Completion
of the next original instruction expires it; another STI with IF already one
does not extend it. CLI in that slot keeps IRQs blocked through IF. Successful
exception entry clears the shadow. A supplied MOV-SS shadow likewise expires on
completion or delivery. Invalid or unsupported steps expire neither shadow.
The NMI/STI policy is explicit because processors may inhibit NMIs after STI;
MOV-SS inhibition and architectural NMI blocking are checked independently.

LSS does **not** create MOV/POP-SS inhibition. It loads its pointer and SS as a
single instruction; an event arriving after a successful LSS may be delivered
before the following instruction. An incoming MOV-SS/STI shadow expires on that
LSS completion without being renewed. A refused load in the successful-RAM
domain preserves the incoming shadow and budget. Intel SDM volume 3A revision
093, section 7.8.3 explicitly distinguishes LSS from MOV/POP SS; creating an
extra LSS shadow also fails the original-CPU interrupt-window comparison.

This optional scheduler uses indexed instruction dispatch for either export
dispatch setting. It does not poll inside REP, native services, HLE or other
partial instructions. Native RTM region bindings are rejected: their checkpoint
cannot be split into these source boundaries. Selected steps retain their
existing mode/privilege/flags domains. The event function's compatibility-entry
domain does not create an i386 dispatcher. MOV/POP SS remain native boundaries
in this successful-RAM interface. The [paged MOV SS binding](x86-paged-execution.md#faultable-selector-mov-and-pop)
owns long-mode descriptor loads and persistent inhibition through this shared
scheduler; legacy POP SS is still outside that binding.
This successful-RAM interface does not own restartable page faults or nested
delivery faults; the paged binding now owns those interactions with scheduling.
APIC/devices, SGX AEX, SMI entry and CET/FRED routing remain unfinished boundaries.

`ScheduledEvents*` checks persistent shadows, budgets, invalid entry, producer
failure, acknowledgment, calls and synchronous exceptions. Its independent KVM
fixture executes original STI/NOP, STI/STI, STI/CLI/STI, STI/JMP and STI/CALL,
plus supplied STI/MOV-SS states, a blocked NMI and LSS at each operand width
with pending IRQ/NMI. Each compiler profile compares all state and 4 KiB of RAM
over 2,176 cases, including interrupt/trap gates, all stack alignments and varied
flags. The LSS subset has 1,152 cases. KVM's actual interrupt-window exit
selects the injection point, independently of the candidate scheduler.

Capture artifacts with `NEVERD_ISA_REGRESSION_ARTIFACT_DIR` and run
`scripts/isa/x86/verify_event_delivery.py --family schedule --sources DIR
--compiler NEVERC --llvm-bin LLVM_BIN --keep OUTPUT`. This checks NeverC and
both full LLVM backends and authenticates source hashes against manifests.
For scheduling, capture the `.oracle.json` count/hash sidecar with each harness.
The verifier reads those fixture-owned counts and checks the harness hash; it
must not reuse the former hard-coded 1,024-case total for an expanded matrix.

The [2026-11-01 qualification](evidence/x86-event-scheduling-2026-11-01.json)
records 686 passing regressions across the full fork and pinned r6 SDK, with
no failures or skips. Their 36 captured source, harness, linker and manifest
files are byte-identical. New scheduler and memory-division fixtures perform
72,192 original-CPU comparisons through Clang; NeverC and both full LLVM
backends add 108,288 comparisons across 24 profiles. This qualifies the
declared instruction-boundary and successful-RAM domains above.

References: [Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html),
revision 090, STI and Volume 3A interrupt inhibition;
[KVM run interface](https://www.kernel.org/doc/html/latest/virt/kvm/api.html#the-kvm-run-structure),
`request_interrupt_window` and `ready_for_interrupt_injection`.

The [paged qualification](evidence/x86-paged-scheduling-2026-11-02.json) covers
faultable IRQ/NMI scheduling through both C routes, Clang/NeverC at O0/O2 and
both SDKs. It includes all IRQ vectors, NMI, gate faults, inhibited and blocked
inputs, PVI entry faults and multi-instruction inhibition windows. The 186
prior no-fault C/header artifacts remain byte-identical.

The [LSS boundary qualification](evidence/x86-lss-event-boundary-2026-11-02.json)
records the expanded scheduler matrix and its source, runtime and paged-return
regressions after integrating current dev: 594 passing tests, two LA57 skips,
24 additional compiler/backend profiles and 257,422 CPU comparisons overall.
The LSS subset contributes 46,080 comparisons. All 484 captured C/header/JSON
exports match between SDKs; eight historical scheduling samples intentionally
expand their selected instruction set, and the other 178 earlier outputs remain
unchanged. The previously documented 544 IRET #AC VMX/NMI discrepancies remain
separate from matching hardware fields. This historical qualification validates
LSS event semantics; the later paged binding now also owns faultable LSS/LFS/LGS
and selector MOV/POP, as documented in the paged execution contract.
