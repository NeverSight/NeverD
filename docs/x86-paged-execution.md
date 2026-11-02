# Generated x64 instructions over paged RAM

The V6 continuation export accepts `paged_memory: true`. Both HighC and
LLVM-to-C emit the actual selected instruction semantics with explicit guest
memory calls. `X64PagedExecution` binds those calls to `X86PagedMemory`, verifies
original instruction bytes through execute permissions, and commits scalar
instructions or delivers their synchronous faults. Guest addresses never become
host pointers.

```json
{
  "profile": "kernel-events-v6",
  "mxcsr_mask": "0xffff",
  "paging_address_bits": 48,
  "paged_memory": true,
  "instructions": [4096, 4099]
}
```

The exported entry is:

```c
uint64_t neverd_x64_run_paged_v6(neverd_x64_state_v6 *state,
                                uint8_t *context, uint64_t *budget);
```

`context` points to a live C++ `analysis::X64PagedExecution` constructed with
the embedding application's `translate::X86PagedMemory`. Link the
`NeverDX64PagedEventDelivery` component for the source-only C bridges declared
in `X64PagedExecution.h`. The generated bodies, validators and original bytes
must come from the same export. Their adapters preserve actual function types
across both C routes. This internal embedding contract is not a stable C SDK
runtime-construction interface.

When CPL3 alignment checking is active (`CR0.AM` and `RFLAGS.AC`), construct
the context with an explicit `X64PagedExecutionOptions::AlignmentPriority`.
`BeforePaging` checks scalar alignment after canonicality but before page walks;
`AfterPaging` checks it after successful translation and backing access. An
unspecified priority refuses that entry before fetch or RAM effects. The bound
CPU supplies this choice; it is never inferred from the host. The local KVM CPU
qualifies `BeforePaging`, including unaligned absent pages, noncanonical
operands and page-crossing operands. The other order has a separate software
contract check. Intel's boundary-event priority table does not prescribe the
order of competing faults during instruction execution.

## Instruction and fault ownership

The existing decoded LowIR owns arithmetic, flags, register slices, original
addresses and branch targets. The paged source adapter retains original stack
reference and read-modify-write identities before register-state rewriting.
Private state accesses stay separate from guest operands. Imported callback
types and SSA input lifetimes are explicit throughout both source pipelines.

The runtime validates the entry and IA-32e controls, then fetches the expected
instruction bytes. It stops at the first changed byte, before using later stale
bytes; the caller can relift at that PC. A complete instruction runs against a
staged state. Its final scalar store retains a prepared physical translation
plan, and publishes data only when all remaining checks succeed. Page-table
A/D effects survive failure. There are no external RAM writers during a step.

The current admission covers GPR/flags operations with zero or one scalar read
and zero or one final scalar write, at 1/2/4/8 bytes. It includes near CALL/RET,
indirect branches, stack operands, FS/GS bases, address-size overrides and
full-dividend DIV/IDIV. A read-modify-write operand checks write permissions even
before arithmetic. False CMOV conditions still perform their memory access.
CALL checks its stack write before a noncanonical target: a missing stack page
therefore produces #PF, while successful preflight followed by #GP publishes
neither the return address nor register changes.

Canonical INT n and both INT3 encodings also enter the shared paged IDT owner.
Successful software entry saves the original fallthrough and never infers an
error code from the vector number. A gate, descriptor or memory fault while
executing the instruction instead saves its original PC. The common delivery
layer owns this distinction, including recursive faults. Service-shaped INT
vectors such as 0x29, 0x2d and 0x80 remain physical IDT transfers in this explicit
machine-state domain. They do not invoke host OS services.
The last canonical instruction byte is also valid for INT/INT3: the saved
fallthrough may be noncanonical, because entry does not execute it. The shared
faultable owner validates the handler target independently. Original CPU tests
cover this boundary through both C routes and compilers. More generally, saved
PC is always frame data for paged entry: a sequential fetch beyond canonical
addresses or a compatibility CS limit must retain that faulting PC, and an IRQ
can preempt the bad fetch. Native NOP-boundary and POPFQ/IRQ cases verify these
paths; the old successful-RAM event API retains its narrower input contract.
POPFQ constructs the original CPU's interrupt window in that fixture; its
execution is not newly admitted by the paged source binding. The compatibility
cases qualify event entry, not a complete legacy instruction dispatcher.

#PF, #GP, #SS, #AC, #DE and canonical UD2's #UD retain the original restart PC.
The [shared paged event owner](x86-paged-memory.md) performs IDT entry, including
nested faults, #DF, CR2 updates, partial handler-frame writes and shutdown.
Execution stops at handler entry. Correcting a mapping and restoring the
interrupted state permits a retry without repeating a committed operand write.
RF clears only on successful instruction completion.

Existing status identities 0..16 remain unchanged. Internal status 17 is
consumed by the executor. Status 18 reports physical backing failure, 19 reports
shutdown, and 20 reports changed code requiring relift. A completed instruction
or an architectural fault attempt consumes one instruction-budget unit;
native/invalid/changed-code stops and operand backing failures do not. A fault
whose IDT entry subsequently hits a backing failure still consumes its attempt.
`InstructionAttempted` and `Delivery.InputAccepted` retain these distinctions
independently of the final status. Full fault and backing details
are available through the context's `lastResult()` and `lastError()`.

## Paged IRQ/NMI scheduling

Add `scheduled_events: true` to the paged binding to export:

```c
uint64_t neverd_x64_continue_paged_events_v6(
    neverd_x64_state_v6 *state, uint8_t *memory_context,
    uint64_t *instruction_budget, neverd_x64_interruptibility_v1 *shadows,
    uint64_t *poll_budget, neverd_x64_peek_interrupt_v1 peek,
    neverd_x64_accept_interrupt_v1 accepted, void *producer_context);
```

The [shared scheduler](x86-event-scheduling.md) owns polling, budgets and
persistent STI/MOV-SS inhibition for both memory bindings. The paged variant
uses the runtime's instruction validator before invoking a producer and the
same faultable event owner as synchronous exceptions. It does not emit or call
the successful-host-RAM event body. `memory_context` is the paged executor;
`producer_context` belongs to the external event producer. State, both contexts,
shadows and budgets must be disjoint from each other and guest RAM.

An eligible IRQ/NMI precedes the next instruction's fetch, including an absent
code page. An inhibited candidate remains pending while that instruction may
execute or fault. The paged event result records acceptance before the first
IDT access. The scheduler calls `accepted` exactly once when that event was
accepted, even if entry produces a secondary exception, shutdown or a physical
backing failure. These events consume no instruction budget. The callback
acknowledges producer storage; it does not signal APIC EOI. Invalid or inhibited
events are not acknowledged. Partial frame writes, CR2 and NMI blocking remain
owned by the common event algorithm.

Completed instructions and architectural fault attempts expire supplied
inhibition; an architectural fault whose delivery fails is still an attempt.
Code changes and operand backing failures preserve inhibition and instruction
budget. Polls are charged independently. Call/return following retains the
export's explicit policy and can poll at the selected callee's entry.

CLI/STI check CPL against IOPL. At CPL3 with CR4.PVI and IOPL below 3, they
update VIF instead of IF; virtual STI faults when VIP is set. Only an actual
IF transition from zero to one starts STI inhibition. The runtime also owns
the instruction-entry #GP when CPL3/PVI find both VIF and VIP set, regardless of
IOPL or opcode. It occurs before fetch, after eligible external event selection.
The shared flags lowering performs the authorized IF/VIF update. Paged STI
requires the scheduling binding so its inhibition can persist across calls.

## Verification and limits

Build `NeverDX64PagedExecutionTests` through `scripts/build.py`, together with
`NeverDX86PagingTests`, `NeverDX64ContinuationSourceTests` and
`NeverDX64ContinuationExportTests`. The generated execution fixture qualifies
Linux x64; other hosts explicitly skip it. Supply `NEVERD_TEST_NEVERC` to
compile each source route with NeverC as well as Clang, at O0/O2. Set
`NEVERD_ISA_REGRESSION_ARTIFACT_DIR` to preserve actual C and manifests.

An independent KVM CPU executes 38 scalar forms, all 256 INT n vectors and
single-byte INT3 across fifteen input and memory configurations for every
compiler profile. Software cases execute without host debug interception.
Checks compare all GPRs,
RFLAGS, PC, CR2, CS/SS and physical RAM outside paging-table pages, including
fault frames. Only architectural undefined arithmetic flags use witnesses from
that original CPU run. Fault-time A/D updates are not equated because their
publication can depend on the CPU. Additional checks cover self-modification,
backing failure, invalid binding, write-fault restart and refusal boundaries.
The software-vector matrix avoids deliberately recursive #NP/#GP gates whose
#DF saved program state is undefined; the separate paging suite qualifies #DF
and shutdown with its explicit undefined-state exclusions.

The [November 1 qualification](evidence/x86-paged-execution-2026-11-01.json)
records 534 passing tests across the native-intrinsic and pinned r6 SDKs, with
two explicit LA57 skips. Generated execution performs 6,080 original-CPU
comparisons, including 3,120 fault cases; the paging/event regression adds 1,800
comparisons. All 298 C/header/manifest artifacts match across SDKs, and the old
source/header outputs remain unchanged.

The [software-entry qualification](evidence/x86-paged-software-events-2026-11-01.json)
extends this to 538 passing tests and the same two LA57 skips. It records 72,632
CPU comparisons across both SDKs: 70,832 through generated C and 1,800 through
the paging/event regression. The generated matrix includes 29,872 fault cases.
All 330 exported artifacts match across SDKs; the earlier no-fault C/header
outputs remain unchanged. Retained counterexamples distinguish wrong software
restart PCs, wrong alignment/page-fault ordering and rejection of a valid
software entry at the last canonical instruction byte.

This is the scalar instruction commit domain. REP, gather/scatter, XSAVE and
other partial-completion instructions need their own progress models. LOCK,
native system providers, TF, SMI/AEX, legacy i386 dispatch,
CET/FRED, MMIO, TLB and concurrent writers remain outside this binding.
Intra-instruction events, device arrival and interrupt-controller selection
remain separate. Combining paged memory with `event_delivery`, direct dispatch
or RTM regions is explicitly rejected. The [no-fault scheduler](x86-event-scheduling.md)
retains its separate successful-RAM contract. LLVM release/pin completion is not implied.

References: [Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html),
volume 2 CALL/DIV/IDIV/CMOV/CLI/STI, volume 3A paging, exception priority
and interrupt inhibition, and volume 3B protected-mode virtual interrupts;
[KVM API](https://www.kernel.org/doc/html/latest/virt/kvm/api.html).
