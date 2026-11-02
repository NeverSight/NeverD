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
RF clears on successful scalar instruction completion. Wider IRET forms instead
restore RF from the return frame.

Existing status identities 0..16 remain unchanged. Internal status 17 is
consumed by the executor. Status 18 reports physical backing failure, 19 reports
shutdown, and 20 reports changed code requiring relift. A completed instruction
or an architectural fault attempt consumes one instruction-budget unit;
native/invalid/changed-code stops and scalar operand backing failures do not.
Once IRET begins, its NMI and descriptor effects prevent that rollback rule;
even a return-frame backing failure consumes its attempt. RETF retains NMI
inhibition and consumes an attempt on backing failure only after a descriptor
write has committed. Page-walk A/D effects alone do not charge an attempt.
A fault whose IDT entry subsequently hits a backing failure still consumes its attempt.
`InstructionAttempted` and `Delivery.InputAccepted` retain these distinctions
independently of the final status. Full fault and backing details
are available through the context's `lastResult()` and `lastError()`.

## Faultable LSS, LFS and LGS

The paged V6 binding executes 16/32/64-bit far-pointer loads through the shared
segment owner. Bind `X64PagedExecutionOptions::FarPointerProfile` to the target
processor's qualified `OffsetFirst` order. It reads the offset, then the
selector, then reads and validates the descriptor before its conditional
qword accessed-bit update. The profile has been compared with original bytes
on both local Intel hybrid core types. It is an explicit target binding, not a
claim about every processor. Missing binding stops after instruction fetch and
before operand access. The usual alignment-priority binding also applies.

Operand reads use the current CPL and the original stack/data address role;
descriptors use implicit supervisor accesses, including SMAP restrictions.
SP/BP select #SS for noncanonical stack addresses, while R12/R13 remain data
defaults. FS/GS overrides select data access. Single CS/DS/ES/SS overrides are
admitted and ignored in 64-bit mode; they do not change the original default
fault class. Duplicate prefix groups, forbidden LOCK/REP forms, register
operands and incomplete encodings remain refused by the shared byte grammar.
Each address32 pointer field wraps before adding the old FS/GS base, including
EIP-relative addressing. Source registers and bases are captured before either
destination changes.

Validation covers CPL/RPL/DPL, data and readable/conforming code types, GDT/LDT
bounds, descriptor canonicality, presence and accessed writes. Null SS is
accepted at CPL0..2 only with matching RPL; CPL3 gets #GP(0). Null FS/GS uses
the export's existing `null_segment_base_policy` (`clear` or `preserve`). An
unbound policy refuses a null load after operand reads; it does not invent a
guest exception. Null loads never consult the descriptor tables. Inactive
cache attributes are not asserted equal to KVM's normalized inactive values.

Both the GPR destination and segment cache commit only on success. A word GPR
write preserves its upper bits; a dword write clears the upper half using the
shared register definition. Faults retain the original PC and use the common
IDT/#DF/shutdown owner. Ordinary physical backing stops leave both destinations
and the instruction budget unchanged; an architectural fault still consumes
an attempt if its handler entry then encounters missing backing. LSS does not
start or renew MOV/POP-SS inhibition. The existing scheduler consumes incoming
inhibition after the attempt and can deliver IRQ/NMI immediately afterward.

The original-CPU matrix covers competing faults, cross-page pointer and
descriptor accesses, accessed writes to read-only pages, aliased pointer/table
storage, null selectors, privilege combinations and both hybrid core classes.
Additional generated programs repair a missing page in a #PF handler, remove
the error code, IRET and retry each of LSS/LFS/LGS. Address-wrap and pending-event
programs compare complete usable caches, register state, fault frames and
ordinary RAM. The portable runtime path uses scalar IR and callbacks. The
existing LLVM fork intrinsics and NeverC native builtins remain separate native
execution capabilities and need no source change for this runtime extension.

The [November 2 qualification](evidence/x86-paged-far-pointer-2026-11-02.json)
records 604 passing test runs, two unavailable LA57 oracle skips and 768,246
original-CPU comparisons across the native-intrinsic and pinned r6 SDKs. The
far-pointer subset is 580,440 comparisons, including 360,096 fault cases;
these subsets are included in the total. The 99-form matrix exercises 181
input/fault cases through both C routes, Clang/NeverC and O0/O2 on P/E cores.
There are also 96 policy/backing checks, including failures after the offset
read and at descriptor publication. All 556 primary and 124 additional E-core
exports match across SDKs; all 484 earlier outputs are unchanged. The existing
544 IRET #AC VMX/NMI discrepancies remain separately recorded. Five compiler
interface tests and 33 new-pass-manager commands separately verify the existing
native intrinsics; they are not added to the CPU totals.

## Faultable selector MOV and POP

The paged V6 path owns `MOV r/m16,Sreg`, register-width selector reads,
`MOV Sreg,r/m16`, and long-mode `PUSH/POP FS/GS`. Bind
`X64PagedExecutionOptions::SegmentMoveProfile` to `SelectorFirst`. Memory
MOV and POP selector operands access exactly two bytes. On the qualified processor, even
the default eight-byte POP reads only that word; its successful RSP increment
is eight bytes, or two with an operand-size override. REX.W selects eight.
PUSH writes the full zero-extended selector at its operand width, two or eight
bytes, then publishes the matching RSP decrement. It does not read the GDT/LDT,
even for an unusable segment cache. Address-size and segment overrides do not
redirect either stack instruction's implicit 64-bit RSP access. The original
RSP survives faults; a cross-page backing stop cannot publish a partial store.
Native compiler builtins remain a separate ambient-machine execution path.

The original-byte parser shares prefix, ModRM, SIB and displacement ownership
with LSS/LFS/LGS. MOV reads all six segment selectors and writes all except CS.
Word register reads preserve upper bits, dword reads clear the upper half, and
qword reads zero-extend. Memory MOV uses the original address size, old FS/GS
base and stack/data fault class. The shared descriptor owner validates
privilege, type, presence and accessed updates before publishing the new cache.
Null DS/ES selects an unusable cache; null SS retains the CPL/RPL restrictions
above. FS/GS null loads require the existing explicit clear/preserve policy.
Register operands and RSP remain unchanged when selector/descriptor access
faults. Physical backing failure is distinct from an architectural exception.

`MOV SS` requires `scheduled_events: true` and the generated
`neverd_x64_continue_paged_events_v6` entry. Its shadow persists across budget
stops and separate calls. The instruction-only `neverd_x64_run_paged_v6` refuses
MOV SS without effects even when the export also provides a scheduler. A
successful MOV SS starts inhibition unless an incoming MOV-SS window is already
active; an incoming STI window does not prevent that start in this qualified
profile. A following MOV SS consumes the existing window without renewing it.
Faults consume it, and unattempted backing/changed-code/admission stops preserve
it. LSS keeps its distinct behavior and never starts MOV-SS inhibition.

The original-CPU checks include all privilege pairs, present/type violations,
GDT/LDT and accessed-bit faults, operand/store faults, exact word page bounds,
FS/GS and address32 wrapping, pending IRQ/NMI, consecutive MOV SS and a following
RSP switch. Generated fault handlers repair the page table, discard the error
code, IRET and retry MOV loads/stores and both PUSH/POP targets. Complete usable
segment caches, GPRs, flags, CR2, restart frames and ordinary RAM are compared.
Page-table A/D effects are excluded from hardware byte equality; the software
alignment-order check separately proves a post-translation #AC cannot publish
a selector store. Duplicate prefixes, LOCK/REP, invalid segment registers and
incomplete encodings remain explicit refusals. This is a long-mode contract;
legacy PUSH CS/DS/ES/SS, POP DS/ES/SS and i386 segment/task dispatch are not added here.

The [selector-move qualification](evidence/x86-paged-segment-moves-2026-11-02.json)
records 636 passing broad regression runs and two unavailable LA57 skips across
both SDKs, including 1,103,686 CPU comparisons. Selector MOV/POP contributes
335,440 comparisons, with 183,360 fault cases, and 608 policy/backing checks.
The 1,080 captured C/header/manifest files match across SDKs. A final paged
rerun additionally checks the unbound MOV-SS native status and unique dynamic
library names; its results and repeated comparisons are recorded separately
from those broad totals. Existing IRET #AC VMX/NMI discrepancies remain explicit.

The subsequent [PUSH qualification](evidence/x86-paged-segment-push-2026-11-02.json)
adds both FS/GS targets at word/qword widths, empty/unusable selectors, ignored
prefixes, alignment and cross-page faults, canonical boundary wrap, PUSH/POP
round trips and mapping repair/retry. Across both SDKs and core classes, 104
test runs pass with 781,210 CPU comparisons; 12,864 exercise PUSH directly,
including 3,712 fault cases. The 568 captured source/header/manifest files match
between SDKs, and all 280 selector exports also match between core classes.
The 272 inherited IRET #AC VMX/NMI discrepancies are recorded separately.

## Faultable IRET, IRETD and IRETQ

Canonical `66 cf`, `cf` and `48 cf` use the same descriptor, return-frame,
flag and compatibility-state owner as the successful-RAM source path. The
faultable evaluator supplies architectural #GP/#NP/#SS/#PF/#AC outcomes and
preserves their original IRET restart PC. Both C routes call this owner through
the existing paged bridge; no host pointer or inline assembly executes the
return. Source admission authenticates the original bytes and LowIR shape.

Bind `X64PagedExecutionOptions::IretAccessOrder` to the target processor's
qualified order. The currently qualified `FlagsFirst` profile reads FLAGS,
CS and IP, validates and updates CS's descriptor, then reads SS and SP,
validates and updates SS's descriptor, and finally validates the target IP.
Descriptor reads precede RPL validation, so a missing descriptor page wins over
a simultaneous RPL error. A compatibility target is truncated before the final
canonical-address check. Accessed writes
use descriptor qwords. This order determines CR2 and the writes preceding a
later fault; it is an explicit CPU binding, not a universal inference from
Intel's pseudocode. Missing binding stops after byte authentication but before
return-frame access or NMI unblocking. Active user alignment checks additionally
require the normal `AlignmentPriority` binding.

IRET unblocks NMI after successful fetch, including when NT, a frame access,
a descriptor or the returned PC subsequently faults. The old registers and
segment caches remain restartable on failure, while completed descriptor and
page-table writes remain visible. Each stack field receives its own canonical
and paging checks: preflighting the whole frame would incorrectly suppress a
CS accessed write before a later noncanonical SS field. A null SS performs no
speculative descriptor read. Same-CPL returns preserve data-segment caches;
only an outer return invalidates inaccessible DS/ES/FS/GS selectors. IF, IOPL,
VIF and VIP restoration respects the original CPL/IOPL, and 16-bit IRET retains
the defined upper flag bits. Compatibility returns retain explicit upper-half
GPR witnesses; these bits are not treated as a native equality requirement.

Success returns the existing `PrivilegeReturn` status 6. The caller can resume
selected code with a new driver call. Event entry still returns status 7 at the
handler boundary. Tests compose these calls through IRQ/NMI entry, generated
IRET and resumed code, and execute a generated #PF handler that repairs the
page table, removes the error code, returns to the faulting IRET and retries it.
Returning to compatibility mode produces the correct state and explicit exit;
it does not add a legacy-mode instruction dispatcher. Restored TF likewise
remains an explicit boundary before later instructions without a debug owner.

The IRET CPU matrix compares CS/SS caches, data selectors, unusable bits,
NMI blocking, flags, GPRs, restart frames and ordinary RAM. It covers all operand
sizes, privilege and IOPL combinations, LDT/conforming descriptors, null stacks,
misaligned descriptor tables, descriptor/frame aliases, split stack fields and
the canonical-address boundary. KVM normalizes unusable SS attributes, so those
inactive attribute bits are not used as a hardware cache witness. Linux VMX also
intercepts #AC and `vmx_recover_nmi_blocking` restores NMI inhibition before
reinjection. That observed VMM discrepancy is counted separately; the model
retains Intel's architectural IRET unblocking rule rather than copying it.
See the [Linux VMX source](https://github.com/torvalds/linux/blob/master/arch/x86/kvm/vmx/vmx.c)
and Intel SDM volume 2, IRET/IRETD/IRETQ.

## Faultable RETF and parameter cleanup

Canonical `CB` and `CA imm16`, with no prefix, `66` or `48`, use the same
faultable return owner. Bind `X64PagedExecutionOptions::FarReturnProfile` before
executing them. Both profiles read CS before IP, validate and update the CS
descriptor, then read SS before SP only for an outer return. Descriptor fetch
precedes RPL validation; descriptor accessed writes precede late target faults.
A same-CPL return never speculatively accesses an outer frame. NT is retained,
RF clears on successful completion, and NMI inhibition remains unchanged.

The two profiles differ in the final compatibility target check:

| Profile | Canonical-address input | Locally qualified CPU |
| --- | --- | --- |
| `SelectorFirst` | Original saved IP | Core i9-13900H performance core, CPUID core type `0x40` |
| `SelectorFirstTruncatedIP` | Truncated compatibility EIP | Core i9-13900H efficient core, CPUID core type `0x20` |

This difference was reproduced by executing the same original RETFQ bytes on
fixed cores. It is a target binding, not a claim about every processor with that
core type, and runtime code never derives it from the machine running NeverD.
The test oracle pins its thread before reading
[CPUID's hybrid core type](https://www.intel.com/content/www/us/en/developer/articles/guide/12th-gen-intel-core-processor-gamedev-guide.html)
and restores its prior affinity afterwards. Qualification runs exercise both
classes explicitly. The original unpinned failure is retained as evidence.

The unsigned immediate is applied to the current stack's parameters before
locating an outer frame and to the returned stack after restoration. Operand
width, target code mode and SS.B retain their separate roles. Outer returns
revalidate data selectors; same-CPL returns preserve their caches. Compatibility
GPR upper halves use explicit witnesses. Success publishes `PrivilegeReturn`
without executing the target. A generated #PF handler can repair a mapping,
IRET back to RETF, and retry the original instruction.

Physical backing failures are embedding stops, not synthetic guest page
faults. An early RETF backing stop preserves budget and interrupt inhibition;
a stop after a descriptor write retains that write and charges an attempt.
The caller must inspect retained state rather than assume transactional rollback.
Architectural faults still use the common IDT/#DF/shutdown owner and original
restart PC. Missing profile, changed code and fetch faults do not access the
return frame. The [RETF qualification](evidence/x86-paged-retf-2026-11-02.json)
also records the added competing-fault IRET cases and both hybrid core profiles.

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
Code changes and scalar operand backing failures preserve inhibition and
instruction budget. Return backing stops use the retained-effect rules above. Polls are charged independently. Call/return following retains the
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

The [paged scheduling qualification](evidence/x86-paged-scheduling-2026-11-02.json)
records 558 passing tests and two explicit LA57 skips across both SDKs. It
includes 118,510 original-CPU comparisons: 45,856 new scheduling/virtual-flags
comparisons and 22 saved-PC boundary comparisons extend the earlier matrix.
All 422 captured exports match across SDKs, and 186 prior no-fault C/header
outputs remain unchanged. Retained failing runs demonstrate the missing
pre-instruction PVI fault and rejection of valid saved event-frame PCs.

The [IRET qualification](evidence/x86-paged-iret-2026-11-02.json) records
566 passing tests and two explicit LA57 skips across both SDKs, after integrating
current `dev`. The 130,290 CPU comparisons include 11,780 new return/handler
comparisons, with 4,624 IRET fault cases. The 272 #AC VMX/NMI discrepancies
above remain separately reported; they are not counted as matching NMI bits.
All 450 captured exports match across SDKs, and 186 prior no-fault C/header
outputs remain byte-identical. Retained failures demonstrate the wrong frame
order/CR2, lost descriptor writes, eager whole-frame canonical checks and
incorrect same-CPL data-cache invalidation.

The [RETF qualification](evidence/x86-paged-retf-2026-11-02.json) records
570 primary dual-SDK passing tests and eight additional efficient-core test
runs, with the same two LA57 skips: 578 passes and 170,382 original-CPU
comparisons overall. The RETF subset contains 27,648 comparisons, including
15,312 fault cases. Expanded IRET matrices exercise both core classes and count
544 #AC VMX/NMI discrepancies separately. All 474 primary exports match across
SDKs; each SDK's 44 efficient-core exports also match its performance-core
outputs. The 186 earlier no-fault C/header outputs remain byte-identical.

This binding owns scalar commits, long-mode selector MOV/PUSH/POP and far-pointer
loads, faultable long-mode interrupt/far returns, and the bound REP memory-string
iterations below. Port strings, gather/scatter, XSAVE and
other partial-completion instructions need their own progress models. LOCK,
native system providers, TF, SMI/AEX, legacy i386 dispatch,
CET/FRED, MMIO, TLB and concurrent writers remain outside this binding.
Intra-element events, device arrival and interrupt-controller selection
remain separate. Combining paged memory with `event_delivery`, direct dispatch
or RTM regions is explicitly rejected. The [no-fault scheduler](x86-event-scheduling.md)
retains its separate successful-RAM contract. LLVM release/pin completion is not implied.

## Restartable REP memory strings

Long-mode REP MOVS/STOS/LODS and REPE/REPNE CMPS/SCAS use an explicit
`RepeatProfile` binding. Each
generated step executes one element through the ordinary scalar lifter and
paged operand transaction. Its data, pointers and count commit together.
Earlier completed elements survive a later architectural fault or missing
physical backing. While repetition continues, PC stays at the original REP and RF is
set for restart; final completion advances PC and clears RF. The memory runtime
does not retain a hidden iteration cursor, so architectural state and owned RAM
can be resumed with a new execution context.

The instruction budget counts completed elements and faulting attempts. A
zero-count instruction consumes one completion attempt without an operand
access. A failed element's physical backing stop consumes none; earlier
elements have already been charged. Status 21 reports an intermediate element
from the direct runtime `execute` interface. Both generated region drivers
handle that status and continue within the supplied budget. The scheduler
polls between elements, consumes the incoming SS/STI window after an attempted
element, and saves the original REP PC and RF for an accepted IRQ/NMI.

The two profiles explicitly select the observed addr32 LODS entry behavior:
`ClearLodsCountHigh` clears RCX's high half (qualified P-core behavior),
and `PreserveLodsCountHigh` preserves it (qualified E-core behavior).
Both retain RSI and RAX for empty LODS and first-element operand faults. Addr32
MOVS/STOS zero-extend the count and their applicable index registers before
the first operand, including zero counts and first-element guest faults.
Addr32 CMPS/SCAS clear the count's high half at entry; their index high halves
remain until a successful element writes them. A failed fetch precedes this
entry normalization. Physical backing/admission stops discard the failed
element's staged changes. Address32 wraps the implicit
indexes before the optional source FS/GS base; destinations retain ES's zero
long-mode base. Direction and operand widths come from the shared single
element lifter. The source parser permits one prefix per group and a final
REX, and requires F3 with MOVS/STOS/LODS or F2/F3 with CMPS/SCAS.

CMPS/SCAS evaluate the comparison condition after each element, independent of
the incoming ZF. Both profiles read the CMPS destination before its source,
matching the qualified P/E cores across widths and address forms. The shared
single-element lifter keeps this order, including the CMPSD string/SSE
disambiguation path. Unfinished comparisons retain the pre-instruction flags for
fault, interrupt, backing and budget boundaries; terminal comparisons publish
their result flags. A zero count leaves arithmetic flags unchanged. This
requires no hidden flags checkpoint in the execution context.

Native IRQ/NMI comparisons replay the observed interruption index. Some fast
string forms complete multiple elements before accepting the pending event;
this elementwise binding does not predict a physical CPU's internal grouping.
Arrival and eligibility remain producer-owned. INS/OUTS still require a
device-side-effect contract and retain explicit native boundaries.

References: [Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html),
volume 2 CALL/RET/DIV/IDIV/CMOV/CLI/STI/IRET/LSS/LFS/LGS/MOV/PUSH/POP/REP,
volume 3A paging, exception priority
and interrupt inhibition, and volume 3B protected-mode virtual interrupts;
[KVM API](https://www.kernel.org/doc/html/latest/virt/kvm/api.html).
