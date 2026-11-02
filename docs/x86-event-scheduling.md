# Instruction-boundary x64 IRQ/NMI scheduling

`scheduled_events: true`, together with `event_delivery: true` in
`kernel-events-v6`, exports `neverd_x64_continue_events_v6`. It polls an explicit
producer before each selected original instruction, delivers eligible IRQs or
NMIs through the existing LowIR event owner, and stops at handler entry. Both
HighC and LLVM-to-C use the same instruction bodies, entry guards and scheduler.
The original instruction-only drivers remain available.

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

This optional scheduler uses indexed instruction dispatch for either export
dispatch setting. It does not poll inside REP, native services, HLE or other
partial instructions. Native RTM region bindings are rejected: their checkpoint
cannot be split into these source boundaries. Selected steps retain their
existing mode/privilege/flags domains. The event function's compatibility-entry
domain does not create an i386 dispatcher. MOV/POP SS remain native boundaries;
tracking supplied inhibition does not implement their descriptor-load execution.
APIC/devices, restartable page faults, nested faults/#DF, SGX AEX, SMI entry and
CET/FRED routing remain separate, unfinished boundaries.

`ScheduledEvents*` checks persistent shadows, budgets, invalid entry, producer
failure, acknowledgment, calls and synchronous exceptions. Its independent KVM
fixture executes original STI/NOP, STI/STI, STI/CLI/STI, STI/JMP and STI/CALL,
plus supplied STI/MOV-SS states and a blocked NMI. Each compiler profile compares
all state and 4 KiB of RAM over 1,024 cases, including interrupt/trap gates,
all stack alignments and varied flags. KVM's actual interrupt-window exit
selects the injection point, independently of the candidate scheduler.

Capture artifacts with `NEVERD_ISA_REGRESSION_ARTIFACT_DIR` and run
`scripts/isa/x86/verify_event_delivery.py --family schedule --sources DIR
--compiler NEVERC --llvm-bin LLVM_BIN --keep OUTPUT`. This checks NeverC and
both full LLVM backends and authenticates source hashes against manifests.

References: [Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html),
revision 090, STI and Volume 3A interrupt inhibition;
[KVM run interface](https://www.kernel.org/doc/html/latest/virt/kvm/api.html#the-kvm-run-structure),
`request_interrupt_window` and `ready_for_interrupt_injection`.
