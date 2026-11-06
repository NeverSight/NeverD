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
It also admits audited legacy `XORPS` register forms and `MOVUPS`/`MOVDQU`
register or memory forms. Their existing LowIR preserves the upper vector
lanes, flags and MXCSR; each memory operand is one 16-byte access. A store
prepares both pages before publishing either half, and a failed load retains
the complete destination. These steps require an SSE/SSE2-capable long-mode
target. After authenticated fetch and before operands, `CR0.EM` or cleared
`CR4.OSFXSR` produces #UD; otherwise `CR0.TS` produces #NM. The shared event
owner delivers these faults with the original restart state.

For these unaligned moves with CPL3/AM/AC active, bind
`X64PagedExecutionOptions::SseAlignment` to `NoCheck`, `Qword` or `FullOperand`
according to the target CPU. This chooses the #AC alignment requirement;
`AlignmentPriority` independently chooses its order relative to paging.
Absence refuses the operand before data access instead of guessing a target
policy. Register forms and inactive alignment checking require no such binding.
See Intel SDM Vol. 2, `MOVUPS` (including its varying #AC treatment) and
`MOVDQU`, and Vol. 3A Table 15-1 for SSE availability
([Intel SDM](https://cdrdv2-public.intel.com/868137/325462-089-sdm-vol-1-2abcd-3abcd-4.pdf)).
AVX, FP arithmetic, aligned-vector memory operations and unaligned FS/GS forms
without complete lifter audits retain explicit native boundaries. This is not
a general SIMD executor or a concurrent-memory atomicity guarantee.

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
POPFQ constructs the original CPU's interrupt window in that fixture. The
separate `paged_debug` binding below now admits its generated execution. The compatibility
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

## Instruction-step debug events

Add `"paged_debug": true` to a paged V6 instruction-region export and bind a
live `X64PagedDebugState` through `X64PagedExecutionOptions::Debug`. The bank
contains DR0–DR3, DR6, DR7 and IA32_DEBUGCTL and stays disjoint from V6 state,
guest RAM and the execution context. Its lifetime covers every call. V6's wire
layout does not change. Debug exports use distinct `neverd_x64_paged_debug_*_v1`
bridges, so linking an older runtime cannot silently omit debug delivery.
The scheduled bridge validates the bank before producer polling or fetch and
passes its current MOV-SS shadow to each step.

The default `DebugProfile = InstructionStep` is nontransactional with DR7=0x400
and DEBUGCTL=0. `ExecuteBreakpoints` additionally enables the hardware execution
comparators described below; `DebugRegisters` also owns MOV-DR and GD.
Data/I/O watchpoints, BTF, task traps and other
debug producers require their own state; incompatible controls refuse before
effects. DR6's high half and reserved bits are checked. Historical BLD/RTM status
inputs assume a target that can represent them. The local native oracle uses canonical BLD=RTM=1
inputs because its CPU lacks those optional facilities.

The executor samples TF before the original instruction, commits its register
and memory effects, publishes the #DB cause, and enters the existing IDT owner.
RF does not suppress this single-step cause. IRET and RETF use entry TF even
when the return clears TF; IRET retains restored RF. Setting TF with IRET or
POPFQ starts stepping with the next instruction. Canonical PUSHFQ/POPFQ use
the original stack and entry CPL/IOPL, including silent privilege masking of
IF/IOPL and preservation of VIF/VIP. Their writable-mask rule is shared with
the protected legacy executor. The word flag-stack forms retain their existing
native boundary in this source binding.

MOV SS suppresses its own TF trap when it starts an inhibition window. It
does not defer BS; a consecutive MOV SS consumes the old window. LSS creates
no such window. ICEBP (`F1`) has its own completed trap, including after MOV SS.
Both producers clear inactive B0..B3, set RTM and preserve sticky status;
only TF sets BS. The status rule is shared with i386. Software INT 1, INT3,
faults before completion and externally selected IRQ/NMI do not synthesize
DR6 causes. External producers remain responsible for their own cause banks.

Each completed REP element can single-step. An unfinished element saves the
original PC and RF; the terminal or zero-count element saves fallthrough.
The existing REP flags/progress policy remains authoritative. Synchronous #DB
delivery completes before the scheduler can poll another external event.
Execution stops at handler entry, or at the existing IRET privilege-return
boundary; the embedding may continue selected handler/return-target bodies.

Instruction completion and DR6 survive failed #DB entry, including secondary
faults and physical backing stops. `Delivery.Continuation` retains the actual
accepted event and its original fault PC. After repairing backing, pass that
record to `X64PagedExecution::resume` or `resumeX64PagedEvent`; do not rerun the
instruction or acknowledge the external event again. The record retains
completed memory reads and skips completed writes. This matters when a partial
exception frame overwrites a descriptor or TSS input. Accepted NMI also retains
its blocker without treating it as a new eligibility check on resume. Preserve
the state and record while repairing backing. This continuation is for the
confined ordinary-RAM binding, not device replay or concurrent mutation.

`function_context` is currently refused with `paged_debug`: function completion
across an intervening debug handler needs its own activation contract. Default
paged and no-fault exports keep their existing TF admission. Hardware watchpoints,
branch stepping, general compatibility/i386 source dispatch, SMM/SGX and
transactional debug events remain separate work. Native LLVM/NeverC ICEBP and
flags instructions already exist; this extension adds the software execution
and event contract without changing the production LLVM pin.

See the [qualification record](evidence/x86-x64-paged-debug-2026-11-06.json).
The original-byte matrix disables KVM guest debugging and uses real handlers
ending in HLT. It compares DR6/DR7, represented GPR/segment state, flags, CR2,
inhibition and complete RAM. Page A/D bits are preseeded so the final handler
HLT fetch adds no unmatched page-table effects. Original handlers count #DB
events, IRET through REP elements, and repair a #PF before retrying the load.
Both source routes compile through Clang and NeverC at O0/O2. These results
qualify the tested processor bindings, not every x86 implementation.

References: [Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html),
revision 093, volume 3 §§7.8.3, 20.2.3 and 20.3.1.4;
[KVM debug-register API](https://docs.kernel.org/virt/kvm/api.html).

### Hardware execution breakpoints

Select `X86InstructionDebugProfile::ExecuteBreakpoints` through
`X64PagedExecutionOptions::DebugProfile`, retain `paged_debug`, and bind the
four original linear instruction addresses in `X64PagedDebugState::DR`.
The [protected i386 executor](x86-legacy-segments.md#protected-instruction-step-debug-events)
uses the same profile and matching/status owner through its existing debug
option. This is an explicit processor binding; the runtime does not infer it
from its host.

All four DR7 local/global enable pairs are supported. LE/GE are retained but
ignored on this profile; bit 10 stays one, every RW/LEN field must be zero, and
the remaining bits and DEBUGCTL must be zero. An enabled comparator matches
only the first byte of an instruction, including its first prefix. i386
matches the wrapped 32-bit `CS.base + EIP`; x64 compares the full original
linear address. Physical aliases and addresses inside an instruction do not
match. After IRET/RETF selects compatibility code, next-PC single-step matching
uses the returned CS base plus the IP offset, with 32-bit linear wrap. The
saved exception PC remains that original IP offset. Noncanonical DR addresses
themselves are legal values, not an admission error; they cannot equal an
admitted canonical instruction address.

Eligible external IRQ/NMI selection and PVI's pending-interrupt #GP precede
the execute fault. The fault precedes instruction fetch, its page/segment
faults, byte authentication and execution. RF or an incoming MOV/POP-SS window
suppresses matching. This suppression does not defer a cause. A normal completed
instruction clears RF; IRETD/IRETQ use the restored RF and unfinished REP retains
it. A single-step trap can also report enabled comparators at the next PC when
RF does not inhibit them. ICEBP retains its separate cause. The bound processor
reports only enabled matching B0–B3 slots, preserves sticky DR6 status and does
not set BS for a pure execute fault. Disabled-slot status is not a universal
x86 guarantee; it is qualified here on the recorded processors.

An execute fault saves the original instruction PC and the entry RF image
(zero), without executing the instruction or automatically setting saved RF.
The handler must set saved RF or change its breakpoint before retrying.
The existing IDT continuation retains the published DR6 cause across secondary
faults and missing physical backing; resuming it neither refetches the original
instruction nor generates another cause. The old non-debug bridge refuses an
enabled comparator even with TF clear, so it cannot silently omit this check.

The [execution-breakpoint qualification](evidence/x86-execution-breakpoints-2026-11-06.json)
compares original CPU DR6/DR7, registers, caches, frames and complete RAM with
the executor. It covers local/global/all-slot matches, RF/TF, SS inhibition,
fetch/operand/PVI faults, failed debug gates/stacks, external events and IRET
targets, including nonflat compatibility code and all three return widths.
Original handlers set RF, clear TF, count entry and return; a separate
case repairs a subsequent page fault before retry. Both C routes compile
through Clang and NeverC at O0/O2. Data/I/O watchpoints,
BTF, task traps and transactional debug production remain separate contracts.
No new LLVM intrinsic, SDK release or pin change is needed for this runtime
binding. Reference: Intel SDM revision 093, volume 3 §§7.9, 20.2.1–20.2.5 and
20.3.1.1.

Native debug comparisons use the shared Intel host gate. The evidence records
the complete P/E regression and the subsequent `dev` integration separately,
including rebuilt runtime tests and byte-for-byte generated-source comparison.
The gate does not change software execution or infer its profile from the host.

### Debug-register transfers and general detect

Select `DebugProfile = X86InstructionDebugProfile::DebugRegisters` and retain
`paged_debug`. This explicit nontransactional target preserves MOV-DR's six
architecturally undefined arithmetic flags, fixes DR6.BLD/RTM to one and has
no RTM, bus-lock, BTF, Intel PT breakpoint-trigger tracing or data/I/O
watchpoint producer. `NativeDebugRegisterAccess`
remains the separate native execution option; paged exports call the bound
runtime bank instead of executing a host MOV-DR.

Paged debug exports import `neverd_x64_paged_debug_access_v1` for their operand
callback. This distinct symbol requires a runtime with the debug-register
operation; an older runtime fails to link instead of interpreting the new
operation as an older memory request. Existing ordinary paged exports retain
`neverd_x64_paged_access_v1`.

The shared byte owner accepts 0F 21/23, every GPR and debug register index,
all four ModR/M mod values, one prefix from each legacy group and one final
REX. Mod does not describe memory: only the r/m register is accessed. REX.B
selects R8–R15; 66, REX.W and address-size overrides do not alter the 64-bit
width. LOCK and REX.R are retained as faulting encodings. Duplicate prefix
classes and nonfinal REX remain explicit unsupported source boundaries.

After original-byte authentication, LOCK/REX.R and CR4.DE with DR4/DR5 produce
#UD before CPL's #GP; a remaining CPL0 access checks GD before any transfer or
value validation. CR4.DE=0 aliases DR4/DR5 to DR6/DR7. Writes to DR6/DR7 with
nonzero high halves fault without publication; DR0–DR3 accept all 64 bits,
including noncanonical values. The target normalizes reserved DR6/DR7 bits.
A valid DR7 write requesting RTM or unowned comparators refuses the environment
without manufacturing #GP or modifying the bank. Successful writes feed the
same instruction-step and next-PC comparator owner immediately.

GD publishes DR6.BD and the original PC before entry, clears GD on accepted
#DB and retains both effects through secondary faults and backing stops.
The instruction is incomplete and can be retried after the original handler's
IRET. RF does not suppress GD. A synchronous GD access consumes the incoming
SS window. TF/ICEBP/execution-breakpoint and external architectural #DB entry
also clear GD; software INT 1 leaves it unchanged.

An actual GD cause additionally requires `GeneralDetectFrame`. `FaultResume`
uses the SDM's fault-class saved RF=1 rule. `ClearResume` binds the saved RF=0
observed on the recorded Intel VMX/KVM host. This difference is explicit:
the oracle's saved frame is never masked or silently rewritten. Native
qualification uses `ClearResume`; software backing/resume tests cover both
choices. Missing or invalid choices refuse before publishing a GD cause.
The VMX oracle executes original bytes with guest debugging disabled; KVM can
intercept debug accesses and reinject #DB, so these runs do not establish an
unvirtualized general-detect frame rule.

The [MOV-DR qualification](evidence/x86-debug-registers-2026-11-06.json) records
both C routes, Clang/NeverC, O0/O2, SDK/core combinations and full state/RAM
comparisons, including original handler/IRET/retry. No new intrinsic or LLVM
release is needed for this runtime binding. References: Intel SDM revision 093,
volume 2 MOV to/from Debug Registers; volume 3 §§20.2 and 20.3.1.3, and the
[Linux VMX debug-access owner](https://github.com/torvalds/linux/blob/master/arch/x86/kvm/vmx/vmx.c).

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
it does not add a legacy-mode instruction dispatcher. Restored TF remains an
explicit boundary unless the instruction-region export binds `paged_debug`.

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
and port-device iterations below. Gather/scatter, XSAVE and other
partial-completion instructions need their own progress models. LOCK,
native system providers, unbound debug events, SMI/AEX, legacy i386 dispatch,
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
Arrival and eligibility remain producer-owned. INS/OUTS use the
[explicit port device binding](#explicit-port-devices); physical devices and
read-ahead retain separate native boundaries.

The [REP qualification](evidence/x86-paged-rep-2026-11-02.json) records 759
passing checks across the fork and r6 SDKs, with two unavailable LA57 checks
and one r6-only native-interface check explicitly skipped. Its 827,562
original-CPU comparisons include 201,312 REP comparisons, 59,776 REP fault
cases and 14,208 partially completed fault cases; the latter two are subsets.
The 1,280 REP policy/backing checks are separate. Both SDKs emit identical
680-file paged captures; all 112 REP captures also match across P/E cores,
including 56 source files without inline assembly. Existing IRET #AC VMX/NMI
differences remain separately counted. These results qualify the declared
profiles, not every x86 environment or physical interrupt arrival schedule.
After integrating `dev`'s shared HighC return-tail optimization, another 20
REP test executions pass across both SDKs and P/E cores, with 201,312 CPU
comparisons and all 112 exports identical to the primary captures. The evidence
keeps this later integration gate separate from the broader primary snapshot.

References: [Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html),
volume 2 CALL/RET/DIV/IDIV/CMOV/CLI/STI/IRET/LSS/LFS/LGS/MOV/PUSH/POP/REP,
volume 3A paging, exception priority
and interrupt inhibition, and volume 3B protected-mode virtual interrupts;
[KVM API](https://www.kernel.org/doc/html/latest/virt/kvm/api.html).

## Explicit port devices

`translate/X86/X86PortIO` now owns device effects and bitmap reads for this
adapter and the [protected legacy binding](x86-legacy-segments.md#protected-port-devices).
Existing `X64PagedPortIO*` type names remain aliases of the common types.
The mode adapters still own addressing, TSS admission and register progress.

Paged V6 exports admit long-mode scalar `IN/OUT` (DX or imm8) and single or
F3-repeated `INS/OUTS`, at byte/word/dword widths and 32/64-bit address widths.
REX.W overrides 66 but the transfer remains capped at a dword; without
REX.W, 66 selects a word. OUTS applies FS/GS after address truncation; INS always uses ES, whose
base is ignored in long mode. The strict shared prefix grammar rejects LOCK,
F2 repeats, duplicate groups and misplaced REX. Ordinary source contracts
retain their existing native requirements.

Bind `X64PagedExecutionOptions::PortIO` to a synchronous synthetic device and
one of `X64PagedPortIOProfile`'s explicit element policies. No host port is
accessed. Permissions precede even a zero-count repeat: CPL at or below IOPL
bypasses the TSS, otherwise the runtime reads the I/O-map offset and two-byte
permission window as implicit supervisor memory. Bitmap limits include the
extra terminator byte; a wide access at port 65535 never wraps its permission
check to port zero. Permission denial raises #GP(0); paging and physical
backing failures keep their distinct existing meanings.

Each successful element commits its RAM/register changes, updates the index
according to DF, and consumes one attempt. Addr32 writes zero-extend the
updated index and count. Zero count clears RCX's upper half after permission
checks but does not touch either index. A fault before element completion
preserves its entry registers; earlier completed elements remain visible.
The scheduler polls at each published element boundary, including restart at
the original PC. This is an element execution contract, not a claim about
physical bus cycles, APIC arrival timing or device prefetch.

`InputBeforeWrite` retains one completed input transfer even when the
corresponding destination write faults. `InputAfterWriteChecks` checks the
complete destination before consuming input. `InputBeforeWriteFragments` uses
prepared translations and retains earlier
4 KiB write fragments if a later page faults. `PortInputBytesWritten` reports
these committed bytes even though the element's index/count remain unchanged.
All three profiles check OUTS memory before output. Fragment qualification
uses nonaliasing paging structures; it does not establish a hardware policy
for self-modifying page tables during an operand. `lastResult().DeviceTransferCompleted` identifies that irreversible
device effect separately from `InstructionCompleted`; a faulting INS can set
the former while leaving the latter false. Fault repair and IRET retry the
uncompleted element and may consume the next input value. A discarded input
must not silently be reused after a guest exception.

A device callback returns an optional value: a value completes exactly one
transfer; empty returns status 22 (`DeviceUnavailable`) without charging an
attempt; an error reports a binding failure with no transfer. Output success
ignores the returned value. Callbacks cannot throw, reenter execution, access
guest state/RAM, or change the paging binding. The runtime can therefore
preflight physical backing before invoking the device. An operand or permission backing stop never
consumes input, even under `InputBeforeWrite`; retry after that repair is safe.
Backing failure during later fault delivery is different: the attempted INS
and its device read have already happened. Preserve the reported exception,
repair backing and call `deliver` to finish that exception entry; replaying
INS would consume a second value. Tests cover this separate recovery path. Page-table A/D effects can remain after a stop. There is no private
pending-input buffer: architectural state/RAM plus the caller's device state
are sufficient to recreate the execution context and resume.

The Intel SDM explicitly allows an INS read to happen before a destination
fault. It does not define one portable prefetch or fault-priority policy for
all processors. Qualification with KVM uses original instruction bytes and
an independent synthetic device tape, but intercepted I/O is emulated by KVM;
these counts are **not bare CPU equivalence results**. KVM can read multiple
INS values ahead of a faulting store, so fault trace comparisons use one
requested element. Multi-element success, OUTS fault progress, and the bound
one-element device policies have separate tests. Physical devices, read-ahead
batching, MMIO and concurrent writers are outside this binding.

The port emulator also omits #AC and incorrectly reports #SS for an SS-prefixed
OUTS with a noncanonical RSI. The runtime retains #AC and reports #GP for that
data reference: long mode ignores legacy CS/DS/ES/SS overrides, including fault
classification (Intel SDM Vol. 1, 3.3.7.1). Tests assert both discrepancies
separately and exclude them from KVM equivalence counts. Independent CPU MOV
probes confirm that SS/DS overrides preserve #GP for RSI and #SS for RBP.

References: [Intel SDM, INS/OUTS and I/O permission maps](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html),
[KVM I/O exit API](https://www.kernel.org/doc/html/latest/virt/kvm/api.html).
