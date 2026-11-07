# Legacy protected-mode operands

`ir/X86/X86LegacySegments.h` owns operand checks against an authenticated,
already loaded 16/32-bit protected-mode segment cache. The cache has a base,
effective byte limit, selector, access rights and an independent unusable bit.
It does not infer hidden cache contents from a selector or read the GDT/LDT
again for each operand. Loading a descriptor, checking privilege/presence,
writing accessed bits and invalidating caches belong to the instruction that
installs the cache.

The owner checks read/write/code permissions and the complete offset extent
before adding the base modulo 2^32. Expand-down data uses D/B to select the
16- or 32-bit upper bound. Invalid ordinary segment accesses report #GP(0);
invalid SS accesses report #SS(0). An invalid externally supplied cache is a
binding error, rather than a fabricated processor exception. The effective
offset is supplied after instruction-specific address-size truncation; later
bytes do not independently wrap at the address-size boundary.

Intel SDM volume 3A sections 6.3 and 6.4 specify these bounds and permissions.
Section 6.3 permits an operand crossing a full FFFFFFFFh expand-up limit either
to fault or to wrap; the choice may vary even between executions. The model
therefore requires `X86FullSegmentLimitChoice` on that particular crossing.
A sampled host result does not establish a universal processor policy.
[Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).

`X86PagedMemory::loadSegment`, `prepareSegmentStore` and `storeSegment` compose
these checks with the existing physical RAM/page-walk owner. Segment failures
precede all page-table observations and A/D writes. After a successful segment
check, a linear range crossing FFFFFFFFh continues at zero. A subsequent #PF
retains the translated fault address and earlier A/D effects without publishing
a partial load result or ordinary store. Missing physical backing remains an
embedding failure. These entry points require CR0.PE, no EFER.LMA, and no VM86;
they supply no guessed real-mode or compatibility-mode environment. Alignment,
instruction commit, event delivery and partial-progress instruction policy
remain separate owners.

`NdOpEmulator::stepX86LegacySegment` applies an explicit cache to one i386
CS/ES/SS native-memory operation: legacy RAO, compact compress/expand, or
four-source memory. That binding expires when the call returns. It cannot
silently survive a callee, selector reload or another instruction. Normal
`step` still refuses an active unbound CS/ES/SS access. Zero masks suppress
address/cache observations. Four-source instructions check their whole m128;
compact instructions check only the packed active extent. Code-segment writes
remain forbidden. The image-backed evaluator preserves its existing ordered
compact-store partial effects on a backing failure; it is not a paged fault
handler or a model of concurrent hardware atomicity.

The original-instruction KVM oracle runs byte/word/dword MOV in legacy paging
and PAE with separate code, handlers, supervisor stacks and data backing. It
compares read results, operand bytes, fault vector/error code and CR2 across
all six segments, type and expand-down bounds, nonzero/wrapping bases and
competing page permissions. This validates the common segment address/rights
rules. It does not certify RAO/AVX-512 physical execution or full i386 event
frame delivery. The tests separately exercise the instruction integrations,
mask suppression, byte wrap, fault preservation and expired bindings.

The cached-operand layer does not complete the legacy instruction dispatcher,
task gates, real/VM86 modes, software SMM, enclave
execution/AEX/EPCM, CET/FRED or the remaining partial-progress and device
contracts. LLVM and NeverC already transport these native instruction controls;
this change adds no compiler builtin, LLVM release, SDK pin or submodule update.

The [November 2 qualification](evidence/x86-legacy-segments-2026-11-02.json)
records 528 passing tests and two explicit LA57 skips across the native LLVM
SDK and pinned r6. The new MOV matrix performs 12,864 comparisons across both
SDKs and the local P/E core classes. Both source pipelines and the LLVM object
route check 2,340 i386 objects. Separate Clang ASan/UBSan and NeverC recorders
complete 3,744 source runs plus 504 NeverC packed-constant comparisons. These
recorder results qualify operand transport, not physical RAO/AVX-512 behavior.

Each SDK exports 821 C/header files. Of these, 677 are byte-identical; 144
LLVMC files differ only by r6 retaining an unused result declaration/copy.
Their native-operation text is identical, and both variants pass independent
source execution. The evidence retains the input hashes, commands, build
receipts and exact distinction instead of claiming all exports are identical.


## Explicit selector instructions

`translate/X86/X86LegacySegmentExecution` adds the protected 16/32-bit
instruction-transition boundary for selector MOV in both directions, PUSH/POP
and LDS/LES/LSS/LFS/LGS. It accepts complete instruction bytes, an explicit
GPR/segment/GDTR/LDTR/CPL snapshot, owned paged RAM and a target profile. A
strict prefix/ModRM/SIB grammar retains independent code, operand, address
and stack sizes, all six segment overrides, signed displacements and captured
source registers. It refuses malformed forms, real/VM86/IA-32e state, and
missing or invalid profiles before any RAM access.

`ir/X86/X86SegmentDescriptorRules` is the shared owner of data/stack type,
privilege, present checks and descriptor field extraction. The existing x64
continuation templates emit these rules as IR; the legacy executor evaluates
them directly. Neither source renderer acquires a second descriptor algorithm.
The x86 alignment-priority type also has one owner, with the existing x64
name retained as an alias.

The `WordStackOffsetFirst` profile reads a selector word for MOV/POP, and
writes a word for PUSH even with a four-byte stack adjustment. MOV r32,Sreg
clears its high word. A far pointer reads its offset and then its selector;
each field's effective address is truncated separately before segment-base
addition. Ordinary bytes within a field retain the full offset bound check.
GDT/LDT lookup checks the complete descriptor extent against the table limit,
then reads and conditionally writes eight bytes with implicit supervisor
permissions. Linear table-base addition and descriptor bytes can wrap at
2^32. A descriptor spanning two pages checks write permission on both pages
when A=0, including a read-only first page whose bytes do not contain A.
An already-accessed descriptor skips that write. Null data selectors skip table
access; null SS reports #GP(0).

Segment/page checks precede publication. Successful loading publishes the
visible selector, hidden cache and coupled GPR or stack update together;
a failed explicit operand, descriptor check or accessed write preserves all
of them. POP uses the old SS.D/B for its stack update, including preserving
ESP's high word with a 16-bit stack. A/D page effects remain visible, and a
missing physical backing is separate from a guest exception. Active alignment
checking requires an explicit before/after-paging choice; a misaligned
store never publishes data. The qualified legacy selector operands report
#AC before a competing #PF on both local core classes. The original CPU
matrix also distinguishes a four-byte far-pointer offset from word-sized
MOV/POP/PUSH selector accesses at addresses aligned to two but not four bytes.
This observation is specific to these instructions and cannot select the
priority of an unrelated scalar instruction. MOV/POP SS report the inhibition
request to the scheduler, while LSS does not create that request.

This is an instruction-transition interface, matching the scope of the x64
far-pointer transition helper. Its caller owns original-byte fetch and
authentication, EIP/RF completion, persistent interruptibility, asynchronous
event arbitration and fault delivery. It is not a complete i386 dispatcher or
legacy task/call-gate engine. Null-loaded unusable caches retain opaque old
fields; those fields are not observable as a valid segment or evidence for a
later mode switch. Reserved descriptor state outside the loaded-cache contract
fails explicitly. Processor profiles are bound by the caller, never guessed
from the host.

The original-instruction tests independently author GDT/LDT entries and raw
selector instructions in KVM. They compare every GPR, all usable segment
cache fields, selectors/unusable bits, explicit operand and descriptor bytes,
fault vector/error/CR2 and the old fault-stack pointer/selector. On faults,
data caches and same-ring SS remain directly observable and are checked
against hardware. Handler CS and, on outer-ring entry, replacement SS are
excluded. A following NOP passes the MOV/POP SS debug-inhibition boundary
without changing registers. The
exception handler itself remains an oracle fixture; these results do not
qualify a software i386 event-delivery implementation. Hidden fields of null
unusable caches and unrelated exception-frame memory are excluded explicitly.
The matrix records 174,368 comparisons across legacy paging and PAE, both SDKs,
and the local P/E core classes. Its final runs include the cross-page
accessed-write and preserved-cache fault cases; superseded diagnostic runs
are excluded from the totals. The three shared paged/continuation suites also
export 1,032 files per SDK; all are byte-identical across the two toolchains.
The qualification totals 1,536
passing tests and 34 explicit skips, including LA57, absent native AMX,
NeverC's i386 frontend and older SDK interfaces. See
[the selector-load qualification](evidence/x86-legacy-segment-loads-2026-11-02.json).

After integrating `553445bc43`'s C annotation-keyword change, both SDKs pass
204 additional type-parser and paged-source checks, with two optional kernel
type-corpus skips. All 704 paged-source exports per SDK still match the earlier
qualification. These integration checks are recorded separately from the
primary totals; the paired parser component check retains its raw measurements
locally and makes no whole-decompilation performance claim.

## Protected instruction and event execution

`translate/X86/X86LegacyExecution` connects the selector-transition owner to
an internal protected-mode execution state: EIP, EFLAGS, CR2, IDTR, loaded TR,
and persistent SS/STI/NMI inhibition accompany the existing GPR/segment state.
That state owns flags; execution binds the paging provider to it before
accesses and again after completion. External event selection and APIC state
remain the embedding caller's responsibility; the optional instruction-step
binding below owns TF/ICEBP causes and DR6 publication.

A step authenticates each original instruction byte through CS and paging.
It stops at the first changed byte, preserving earlier page-walk effects and
requiring the caller to decode the new code. A fetch or operand exception
retains the original EIP and enters the IDT. Successful execution publishes
fallthrough, clears RF, and advances SS/STI inhibition. The admitted byte
grammar includes the segment instructions, NOP, register-immediate MOV,
CLI/STI with IOPL/PVI checks, INT, INT3, INTO, INT1, UD2 and the protected
IRET/RETF returns described below. An unimplemented instruction returns an
error; guest #UD is generated only for explicitly owned invalid encodings,
such as authenticated UD2 or LOCK on the string forms described below.
Sequential EIP is not truncated solely because CS.D is zero. Code-base and
limit checks still apply to each fetched byte.
INT1/ICEBP completes before its debug trap: it clears RF, advances EIP and
ends the STI window even when the subsequent #DB entry fails. Its gate DPL
and EXT handling remain those of a debug event, not an INT n software entry.

Event entry supports 16/32-bit interrupt and trap gates, conforming code,
GDT/LDT targets, all four CPLs, inner-ring stacks from 16/32-bit TSS caches,
and frame/error-code widths selected by the gate. The gate DPL check belongs
to software entry; an IRQ to vector 14 does not push a page-fault error code.
Faults set saved RF, software entry clears saved RF, and live flags receive
interrupt/trap-gate updates. Descriptor-table accesses are implicit supervisor
accesses; frame writes use the destination CPL. Table and frame permissions
flow through the same page walker as operands.

The explicit `OrderedFrame` target profile uses qword accessed writes,
TSS offset reads before the selector, and ordered SS/ESP/flags/CS/EIP/error
pushes as applicable. TSS reads cover the actual 16/32-bit offset and selector,
not a padded eight-byte field. On a 16-bit target stack, ESP retains its old
high word while the low word comes from the TSS; a 32-bit target stack uses
the TSS value. Gate width, TSS width and stack address size remain independent.
Earlier descriptor and frame stores survive a later entry failure, while
registers, caches and EIP commit together only after the full entry succeeds.
Target EIP-limit validation follows the complete frame, including its error
code; a competing frame-page fault wins and retains the preceding writes.
Recursive #DF preserves the failed fault entry's saved RF image and uses the
original saved PC and register/cache bank. The architecture leaves #DF program
state undefined: these are qualified profile choices, not portable guarantees
for every x86 processor. The IA-32e oracle retains its separate #DF exclusions.
CR2 survives a subsequent #DF or shutdown. NMI blocking starts when the event
is accepted, including when entry subsequently faults. A missing physical
backing remains a host binding failure, never a synthesized guest #PF.

The profile also binds consecutive MOV/POP SS not to extend an incoming SS
window. LSS does not establish that window. External IRQ, NMI and debug-event
eligibility is checked before table reads. The STI/NMI relationship requires
an explicit policy when it affects eligibility; it is not inferred from the
host. TF instruction stepping and ICEBP while an SS window is active require
the explicit debug binding below. Hardware breakpoint producers and delayed
data-breakpoint state are not silently approximated.

The architecture basis is Intel SDM revision 093, volume 2 INT/MOV/POP/STI/CLI
and volume 3A chapter 7. Target-specific access ordering and stack high-bit
behavior are qualified against original instructions in KVM, not inferred
from the pseudocode's textual evaluation order.
[Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).
The [November 3 evidence](evidence/x86-legacy-event-entry-2026-11-03.json)
records commands, source hashes, SDK inputs, CPU affinity, independent native
matrices, corrected probes and regression results.

This is a bounded internal execution interface. The scalar binding below
extends its ordinary instruction execution. The explicit source dispatcher
below connects selected original instructions to this owner. Ordinary i386
function projection, task transitions, real/VM86, CET/FRED, software
SMM, enclave/AEX/EPCM and the remaining device/partial-progress contracts are
not completed by it. No new LLVM or NeverC intrinsic is required for these
runtime state transitions. The final LLVM release, SDK pin and NeverD LLVM
gitlink remain unchanged pending global completion.

## Protected source dispatch

`--x86-legacy-source` exports a bounded C dispatcher over the protected
execution owner. The SDK entry is `neverd_x86_legacy_source_v1`; its C++ owner
is `exportX86LegacySource`. A request declares exact original instruction
extents in an i386 image:

```json
{
  "profile": "protected-legacy-v1",
  "instructions": [
    {"address": "0x1000", "code_bits": 32, "size": 2},
    {"address": "0x2000", "code_bits": 16, "size": 1}
  ]
}
```

```sh
neverd decompile input.elf --x86-legacy-source=request.json \
  --continuation-report=manifest.json -o protected.c
```

The declared address is the **32-bit linear instruction address**. The
dispatcher selects by `uint32(CS.base + EIP)` together with CS.D. Two code
modes may select the same address with different complete extents. Live
segment bases, offsets, limits and privileges remain in the runtime, including
nonflat code and linear wrap. The image extent itself cannot wrap past 4 GiB.
The shared executor grammar validates each complete byte sequence; this export
does not ask a flat 32-bit decoder to guess a 16-bit extent. Duplicate
address/mode pairs, extra fields, missing bytes and unsupported instructions
fail explicitly. Selection order is normalized for deterministic output.

Generated `neverd_x86_legacy_continue_v1(void *context, uint64_t *budget)`
borrows a `translate::X86LegacySourceExecution`. Construct that C++ context
with the complete `X86LegacyExecutionState`, `X86PagedMemory` and explicit
`X86LegacyExecutionOptions`. Retain the state, RAM, borrowed device/debug
callbacks and their contexts across pauses. Link the private bridge from the
matching NeverD source build. The budget is a separate host cell, disjoint from
state/RAM and never modified by device/debug callbacks. There is no serialized
C++ object layout or stable C state-construction ABI. The SDK exports source
and a manifest; it
does not capture a native CPU, infer a service ABI or construct a guest machine.

This is one **runtime-backed C dispatch route**: instruction arithmetic,
fetch, paging, descriptor, debug and fault semantics stay in the existing
execution owner. It is not a new HighC/LLVMC arithmetic projection. `--llvm`
and ordinary function/source optimization options are therefore refused.
Clang and NeverC compile the same generated C without embedded guest assembly.
The manifest records the exact original bytes/modes, source hash, bridge
identity, budget and event policies, and all status values.

Completed selected near/far/system transfers, CALL and RET continue at the
live target, including a selected IRET target. No ordinary function completion
is inferred from RET. Each attempted instruction consumes one budget unit;
REP consumes one per completed element, including its zero-count completion.
Faulting attempts consume a unit even when fault entry subsequently stops.
Changed code, pre-attempt fetch backing stops and refused steps consume none.
An ordered stack instruction consumes one unit when first attempted, including
when an operand subsequently stops on host backing. Resuming `PendingStack`
does not consume another unit; an architectural fault followed by handler
retry is a new attempt. IRET retains its existing attempted-frame semantics.
Zero budget returns 258
without selection or guest accesses; a null context/budget returns 256.

Every delivered event stops before executing its handler. A caller can then
invoke the dispatcher to execute a selected original handler, including a PTE
repair, error-code removal, IRET and retry. The source-specific status values
are 256 invalid/unbound state, 257 unselected address/mode, 258 instruction
budget exhausted and 259 pending event delivery. Values 0–8 retain
`X86LegacyExecutionStatus`; these are independent of the x64 continuation
status namespace. `lastResult()` retains detailed progress, exceptions,
device effects and self modifications; `lastError()` explains a refusal.
Changed code requires a replacement selection. Shutdown is terminal for that
CPU activation; a producer-owned machine reset is required before executing
again. Neither condition is an automatic retry.

When delivery is pending after a backing or event-work stop, selection returns
259. Repair the physical backing if needed, call `context.resume()`, inspect
its result, then continue. Previously consumed descriptor/TSS inputs, frame
writes, debug causes and completed device input remain owned by the retained
event. The dispatcher never replays the initiating instruction. The host may
call `context.deliver(event)` between dispatch calls; eligibility, accepted
events and inhibition remain owned by the existing event implementation.
No asynchronous event producer or polling interval is inferred.

This provides source selection/continuation for selected, admitted protected
16/32-bit instructions within complete image extents. It does not supply
unimplemented instruction banks, ordinary i386 function C, IA-32e compatibility
mode, task/real/VM86 execution, software SMM/SGX, or native device qualification.
Global LLVM release remains gated on those separately documented contracts.

The [November 7 source qualification](evidence/x86-legacy-source-2026-11-07.json)
records 2,075 source, semantic and interface executions across qualification
and subsequent `dev` integrations: 2,074 pass, with one explicit KVM LA57
hardware skip and no failures. The new 41-case source suite runs on both SDKs
and P/E core classes in the full matrices, giving 164 passing executions and
1,024 original branch comparisons per full matrix. Every retained set of 184
source, manifest, shared-library and i386-object artifacts is byte-identical,
including after the later integrations. Its
paging test binaries are also unchanged, retaining the exact 527-case native
legacy and 570-case r6 full-paging qualification.

The full integration matrices separately pass 88 x64 exporter checks, four
real Python source-ABI checks and seven native plugin adapter checks per SDK.
The Android Parcel merge changes only the r6 shared SDK among the recorded
executables and libraries; its updated ABI and export checks pass. A later
C type lexer merge changes both shared SDKs while retaining those executables.
Both updated SDKs pass another 41 source and four Python ABI cases. Additional
parser checks each pass 56 cases and skip one optional Windows kernel PDB
round-trip because `NEVERD_TYPES_NTKRNLMP` is unset. The initial
two NeverC i386 failures came from omitting its existing explicit full LLVM
backend option; the corrected invocation passes and the failed run remains a
diagnostic, outside qualification counts. Embedded Python plugin-host tests
are unavailable in these build profiles. i386 object generation does not
qualify execution of a 32-bit host runtime bridge. LLVM and NeverC need no
implementation change for this source route; the r6 pin and LLVM gitlink
remain unchanged.

## Protected instruction-step debug events

Bind both `X86LegacyExecutionOptions::Debug` and
`X86LegacyExecutionState::Debug` to use `InstructionStep`. This is an explicit
nontransactional processor profile: DR7 is 0x400, DEBUGCTL is zero, and DR6
has the profile's reserved-bit image. Incoming sticky BLD/RTM status asserts
a processor capable of representing it; this profile does not generate those
historical causes. Incompatible or incomplete bindings
refuse before instruction fetch. This profile has no enabled address/I/O
breakpoints, GD, branch stepping, bus-lock detection or task-trap producer.
Those controls cannot be supplied and silently ignored. The additional
`X86LegacyDebugProfile::ExecuteBreakpoints` profile owns four canonical
execution comparators in `X86LegacyDebugState::DR`; its control and priority
rules are shared with [paged x64](x86-paged-execution.md#hardware-execution-breakpoints).

`X86LegacyDebugProfile::DebugRegisters` additionally owns MOV-DR and GD,
sharing [the x64 register rules and explicit GD frame choices](x86-paged-execution.md#debug-register-transfers-and-general-detect).
The protected executor always transfers full dwords, independently of CS.D,
SS.B and 66; non-64-bit writes zero the architecture's upper halves. Both legacy
and PAE paging preserve original fetch priority and handler frame widths.
All GPRs, ignored mod bits, aliases, DE, CPL, RF/TF, SS inhibition and original
GD handler/IRET/retry have dedicated software and original-byte regressions.
The register profile fixes BLD/RTM and preserves the undefined arithmetic
flags; it does not infer data/I/O watchpoint or transactional debug semantics.

The executor samples TF before the instruction. A successful instruction
produces #DB even when POPF or IRET clears TF; setting TF takes effect on the
following instruction. RF does not suppress instruction stepping. MOV/POP SS
that establishes an inhibition window suppresses its own TF trap, without
retaining a postponed BS cause. The following instruction can trap, including
a consecutive MOV SS under the existing window profile. LSS does not create
that window. ICEBP after MOV SS delivers at its own fallthrough and retains
its own cause even when TF was set.

An instruction-step cause clears inactive DR6.B0..B3, sets RTM and BS, and
retains other sticky status. ICEBP has the same status update without setting
BS. A software `INT 1` leaves DR6 unchanged, as do other software entries and
faults that prevent instruction completion. Causes publish before IDT entry:
a missing #DB gate, a later frame fault or an accepted-event backing stop
retains the completed instruction and its debug status. Event resume does
not execute that instruction again. `PendingEvent.CompletedAccesses` retains
consumed table/TSS inputs and skips already committed frame/descriptor stores.
This matters when a partial event frame aliases and overwrites its own TSS
stack pointer or selector before a backing stop. Architectural secondary
faults and newly generated data traps start fresh access records while keeping
the previous RAM effects. An external debug-event producer remains
responsible for its own DR6 cause; event delivery does not infer one.

Each completed REP element can trap. Remaining iterations save the original
instruction PC and RF=1, while completion or a zero count saves fallthrough
with RF cleared. IRET restores the frame's RF before any entry-TF trap;
SYSENTER/SYSEXIT trap from their committed target context. Existing IDT,
frame-return, paging and partial-progress owners perform these transitions.
This adds no separate exception engine or compiler intrinsic.

`LegacyDebug/*` executes original instructions with KVM guest debugging
disabled and stops in real IDT handlers through HLT. It compares DR6/DR7,
all eight GPRs, all six segment caches, EIP/EFLAGS/CR2, inhibition and complete
RAM, including page-table and partial-frame effects. Code, stack and gate
widths vary independently under legacy and PAE paging. Original #DB handlers
count traps and IRET to the next instruction or remaining REP elements; a #PF
handler repairs the missing PTE, discards its error slot and retries before
the instruction-step event. No host repair occurs between those steps. Unit
cases cover unbound controls, backing continuation and instruction authentication.
The [debug qualification](evidence/x86-legacy-debug-2026-11-05.json) records
actual runs and retained diagnostics. The architectural reference is Intel
SDM revision 093, volume 3 sections 7.8.3, 20.2.3 and 20.3.1.4, plus volume 2 INT1.
The recorded KVM CPU exposes neither RTM nor bus-lock detection; its native
matrix uses canonical BLD=RTM=1. Incoming historical BLD=0/RTM=0 preservation
has software coverage only. Raw KVM debug-register injection can retain an
image that an architectural register read later normalizes; those exploratory
runs are diagnostics, not native qualification.

`LegacyExecutionBreakpoint/*` adds enabled local/global DR0–DR3 matches, RF/TF
combinations, MOV/POP-SS inhibition, nonzero CS bases, pending virtual
interrupts, original fetch/operand faults and failed debug entry across both
paging modes and independent code/stack/gate widths. It also compares original
handlers that set saved RF, count #DB and IRETD before executing the interrupted
instruction once. Unit checks cover linear-address wrap, competing CS limits,
malformed controls and accepted-event backing continuation. See the shared
[execution-breakpoint evidence](evidence/x86-execution-breakpoints-2026-11-06.json).

`X86LegacyDebugProfile::Watchpoints` adds shared data/I/O comparators and
MOV/POP-SS delayed causes. It observes committed linear operands and event
accesses, excluding page walks, and retains accepted-entry causes across
backing stops. The same profile owns protected MOV-DR control writes. Read the
[shared contract and oracle limitations](x86-paged-execution.md#data-and-io-watchpoints)
for matching, software-entry status, recursive #DB and device receipts.
`X86LegacyWatchpointTest.*` covers backing/fault priority and synthetic I/O;
its event-work cases retain pending software/debug and secondary-fault chains
across `EventBudgetExhausted`. `Options.EventDelivery.MaxAttempts` must be
positive; resume `PendingEvent` without repeating the original instruction.
`LegacyDebug/*OriginalDataWatchpoint*` and `*OriginalStackOperandsAndEventTables*`
compare original 16/32-bit code, stack and gate combinations in legacy/PAE RAM.
Repeated memory strings also require `Options.RepeatDebug`: the shared explicit
group decision accumulates MOVS/STOS causes across host pauses, preserves them
on backing stops, and discards them on an accepted ordinary fault. A default
binding selects each element; fast-string grouping is supplied by the target,
not inferred from the host. TF and final completion still force publication.

`X86LegacyDebugProfile::BranchWatchpoints` adds the shared
[BTF contract](x86-paged-execution.md#branch-single-stepping). The scalar owner
returns its actual taken decision, including zero-displacement Jcc and LOOP;
near/far transfers and frame returns retain their existing state owners.
Raw TF and DEBUGCTL remain separate REP group inputs. Hardware debug causes
clear BTF before entry and retain that effect across backing/event stops;
software INT 1 has no such effect. Original ptrace comparisons cover selected
branch/GPR/stack outcomes in protected16/32 user code, while raw debug-bank
and frame effects have separate software checks.

This internal protected executor still does not provide general i386 source
dispatch, task switching, real/VM86, or the other open
machine-environment contracts. The LLVM release, SDK pin and gitlink remain
unchanged until global completion.

## Protected scalar execution

`X86LegacyExecutionOptions::Scalars = OrderedRAM` binds the original-byte
executor to one CPU and ordinary paged RAM for these protected-mode families:

| Family | Admitted forms |
| --- | --- |
| MOV, MOVSX, MOVZX, LEA | Byte/word/dword GPR and memory operands, high byte registers, immediate and moffs moves, independent 16/32-bit addresses and operands. LEA consumes the effective offset without consulting segment or paging permissions. |
| ARPL | Word register or segmented memory destination and word register source, independent code/address widths, conditional RPL adjustment and ZF-only publication. Operand-size prefixes do not widen the word. |
| ADD, ADC, SUB, SBB, CMP | Register/memory, accumulator-immediate and group-immediate forms, including signed imm8 and all six arithmetic flags. |
| AND, OR, XOR, TEST | The same exact operand widths, with an explicit `LogicalAuxiliaryCarry` value for architecturally undefined AF. An absent choice refuses before fetch. |
| INC, DEC, NEG, NOT | Register and memory forms. INC/DEC preserve CF; NOT preserves flags. |
| SHL/SAL, SHR, SAR, ROL, ROR, RCL, RCR | Byte/word/dword registers and memory, implicit one, imm8 and CL counts, masked counts and independent code/address widths. Undefined CF/AF/OF choices are explicit. |
| SHLD, SHRD | Word/dword destinations and register source, imm8/CL counts, explicit undefined state for narrow out-of-range counts. |
| BT, BTS, BTR, BTC | Word/dword register or memory bit base, register or imm8 index. Memory register indices are signed word offsets; immediate indices stay within the base word. LOCK on BTS/BTR/BTC requires the exclusive-RAM binding below. |
| XCHG, XADD, CMPXCHG | Byte/word/dword registers or segmented memory, every high-byte alias, accumulator-short XCHG forms and all six arithmetic flags for XADD/CMPXCHG. Memory XCHG always requires the atomic binding. |
| CMPXCHG8B | Eight-byte segmented memory, full EDX:EAX comparison and ECX:EBX replacement, failed writeback and ZF-only flag changes; 66 does not shrink the operand. |
| AAA, AAS, DAA, DAS, AAM, AAD | Implicit AX/AL adjustment, every immediate base, explicit undefined flag slices, and restartable AAM base-zero #DE. Operand/address/segment prefixes do not change the operation. |
| BSF, BSR | Word/dword register or memory source, partial GPR publication and architectural ZF. Zero-source destination and non-ZF CPU choices are explicit. |
| CBW, CWDE, CWD, CDQ | Implicit AL/AX/EAX sign extension with independent code/operand widths; other register bits and all flags are preserved. |
| LAHF, SAHF | Exact AH/status-bit transfer, reserved bits, and preservation of OF, DF and privileged flags. |
| CLC, STC, CMC, CLD, STD | CF or DF alone; nonprivileged execution at every CPL. |
| BSWAP, XLAT, LEAVE | Dword byte reversal, unsigned AL table lookup through address-sized (E)BX, and an independently sized stack/frame pop. Flags are preserved. |
| SETcc, CMOVcc | All sixteen conditions; exact byte destinations for SETcc and word/dword register destinations for CMOVcc. CMOVcc reads its source even when the condition is false. |
| LOOP, LOOPE, LOOPNE, JCXZ, JECXZ | Independent address-sized CX/ECX and operand-sized targets, signed short displacements, unchanged flags and restartable target-limit faults. |
| PUSH, POP | GPR, memory and PUSH immediate, with independent SS.B stack size. PUSH ESP observes old ESP; POP ESP publishes the popped value after advancement; POP memory uses incremented ESP for its explicit address. |
| PUSHF/PUSHFD, POPF/POPFD | Exact word/dword flag images through SS, with independent stack size, original CPL/IOPL permissions and staged flag publication. |
| MUL, IMUL | Byte/word/dword full products and word/dword truncated/immediate signed products; architectural CF/OF and an explicit undefined-flags profile. |
| DIV, IDIV | Byte/word/dword register or segmented memory divisors, full double-width dividend, signed truncation toward zero, coupled quotient/remainder, and restartable zero/overflow #DE. |
| Near CALL, JMP, Jcc, RET | Relative and indirect transfers, all sixteen condition codes, short branches and RET imm16. Taken targets use the operand width and code limit. Untaken conditional branches do not consult the target. |

Register-immediate MOV preserves its earlier interface without the new scalar
profile. Other scalars require that explicit binding. Original instruction
fetch, pending virtual interrupts, IDT faults, RF, CR2 and persistent shadows
continue through the existing execution owner. Malformed, unsupported or
unbound instructions fail before memory access. No decoder name alone grants
an execution contract.

Register and flag writes are staged until completion. Stores preflight the
whole operand before writing data. Unconditional read/modify/write instructions
request write permission first, preserving the write bit in #PF, and read
through that same physical plan. ARPL instead reads before checking whether
its conditional write is needed. Page-table A/D effects survive later faults and are
visible if the operand aliases its own PTE. Physical read or write denial
remains a backing stop, distinct from an architectural exception. Segment
checks precede paging and the selected alignment priority. Logical undefined
AF choices become observable only on successful completion.

ARPL compares the original low two RPL bits, raises the destination RPL only
when needed, and preserves every other destination bit. A false condition
clears ZF without requesting write permission: read-only segments and pages
remain valid sources. A true condition checks the complete word store before
publishing ZF; a write fault retains the original flags and operand. Read
faults therefore precede conditional write-permission faults. The word remains
two bytes in either code width, including with 66; 67 still selects addressing.
Original handlers can repair the page and IRET/retry. See the
[P/E-core qualification](evidence/x86-legacy-arpl-2026-11-04.json), including
successful operand A/D effects and independent native ELF32 fault probes.
The existing native LLVM ARPL interfaces retain their separate compiler ABI
contract; this executor extension adds no NeverC i386 frontend or real/VM86 mode.

PUSHF/PUSHFD clear RF/VM only in the stored snapshot; ordinary completion
then clears live RF. POPF/POPFD read the whole stack operand before changing
flags or ESP. Original CPL/IOPL govern IF, and only CPL0 can replace IOPL.
Word POPF preserves AC/ID; neither width changes VM/VIF/VIP. CR4.PVI does not
redirect these instructions to VIF. Old AC governs operand alignment faults.
Successful POPF ends existing SS/STI inhibition without starting an STI window;
NMI blocking is retained. Newly restored TF does not trap on the POPF itself,
and the next step uses the bound instruction-step debug producer.

The word native LLVM and C projections preserve this width through
`llvm.x86.flags.{read,write}.u16` and the matching builtin. Extending a word
write to POPFD/POPFQ would incorrectly replace AC/ID. Native compiler functions
own their stack and ABI; they do not replace this original-state executor.
The [FLAGS qualification](evidence/x86-flags-2026-11-04.json) distinguishes
the original protected-mode CPU matrix from source-function ABI comparisons.

`Atomics = ExclusiveRAM` additionally binds a step to exclusive ownership of
the ordinary-RAM image, with no competing CPU, DMA, MMIO or split-lock/bus-lock
trap producer. It admits memory XCHG's implicit locking and legal memory LOCK
forms of ADD/ADC/SUB/SBB/AND/OR/XOR, INC/DEC/NEG/NOT, BTS/BTR/BTC,
XCHG/XADD/CMPXCHG and CMPXCHG8B. Register destinations, read-only operations,
HLE prefixes and unsupported LOCK combinations refuse before fetch. An absent
or unknown binding also refuses before fetch; this model supplies no
concurrent shared-memory synchronization contract.

Exchanges snapshot source values and authenticate the destination before
publishing registers. XADD's sum wins when both operands name the same
register. CMPXCHG and CMPXCHG8B always prepare and commit a write, including
when comparison fails; a failed comparison on read-only RAM therefore faults.
Their original memory plan survives changes to an implicit accumulator or
explicit source that also participates in addressing. CMPXCHG uses accumulator
minus original destination for all six arithmetic flags. CMPXCHG8B compares
both dwords, updates EDX:EAX together on failure and preserves all flags except
ZF. Complete eight-byte bounds and permissions precede publication. The
existing page-table A/D model remains observable when operands alias a PTE.
These rules follow the XCHG, XADD, CMPXCHG, CMPXCHG8B and LOCK sections of
[Intel SDM revision 093](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).

Decimal adjustments require `DecimalUndefinedFlags`: OF/SF/ZF/PF for AAA/AAS,
OF for DAA/DAS, and OF/AF/CF for AAM/AAD. Other bits are rejected before fetch.
Defined flags are calculated from the original AL, AH, CF and AF as applicable;
unrelated flags and register bits are retained. AAA/AAS include carry/borrow
between AL and AH before masking the low nibble. DAS retains its first-stage
borrow even when the second adjustment is not taken. AAM/AAD use the full
unsigned immediate byte, independently of operand and code widths.
AAM zero delivers #DE without publishing AX or new arithmetic flags. AAD zero
is valid: it keeps AL and clears AH. Instruction-fetch faults precede #DE.
An original handler can repair the immediate and IRET to retry with the new
authenticated bytes. LOCK, REP, duplicate prefixes and malformed extents are
refused. These operations reuse the protected scalar transaction; they add no
real/VM86 mode or general i386 source-dispatch contract.

DIV/IDIV additionally require `DivisionArithmeticFlags`, an explicit EFLAGS
image containing only CF/PF/AF/ZF/SF/OF (`kX86LegacyArithmeticFlagsMask`).
These flags are architecturally undefined after successful division; the
runtime does not infer a host-specific result. Divide-by-zero, quotient
overflow and operand faults preserve the incoming flag image and dividend in
the restart state. The operand is a read even on a read-only page; #PF write
permission is never requested. Segment, alignment and page faults precede #DE.
AL/AH and AX/DX aliases are captured before either result changes. An original
handler can change the divisor, IRET and retry without host register repair.
The native suite requires `NEVERD_TEST_LEGACY_DIVIDE_FLAGS=preserve` to bind
the separately measured local CPU choice.

MUL and all one/two/three-operand IMUL forms require the explicit `Multiply`
profile. Full products publish AX, DX:AX or EDX:EAX; truncated IMUL forms write
only their selected destination. Byte immediates are sign-extended before the
signed product, and CF/OF report representability at the operand width. The
source is read before any result or flag publication, including high-byte and
implicit-result aliases, read-only pages and original #PF/IRET/retry.

SF/ZF/AF/PF are architecturally undefined. Both admitted profiles clear AF and
compute SF/PF from the low product. `ProductSignParity` clears ZF;
`ProductSignParityZero` sets it exactly when the low product is zero. The
local 13900H P/E counterexample requires these distinct choices; neither is
a vendor-wide guarantee or selected from the executing host. Bind the native
suite with `NEVERD_TEST_LEGACY_MULTIPLY_FLAGS=sign-parity` on the qualified P
core or `sign-parity-zero` on its E core. An absent or unknown binding fails
before instruction fetch in the software executor.

Single shifts and rotates require `ShiftUndefinedFlags`, an image containing
only CF/AF/OF (`kX86LegacyShiftUndefinedFlagsMask`). It supplies only bits
that are undefined for that particular instruction, width and masked count.
All defined result/flag bits are calculated independently. Nonzero shifts
consume the AF choice; counts above one consume OF. SHL/SHR consume CF only
when the masked count reaches or exceeds the operand width; SAR's carry
remains defined. Rotates preserve SF/ZF/AF/PF. A nonzero whole RCL/RCR carry
cycle preserves the operand and CF but still consumes undefined OF.

All counts are masked to five bits before narrow rotate reduction. Only a
zero masked count guarantees preservation of all flags. Memory destinations
still require a writable, in-limit, mapped and appropriately aligned operand
at zero count,
using the same read/modify/write plan and restart handling as arithmetic.
Input CL is captured before changing any destination, including ECX or CH.
The producer must supply its chosen undefined image; the executor does not
infer one from the host. Native tests observe it separately with the same
count encoding and register/memory form because either can affect undefined
OF. Memory observation uses independent scratch storage with the actual input.
The undocumented group-two `/6` alias remains explicitly refused.
The shared page walker retains its ordered accessed-bit publication on a
later permission fault. Exact faulting-walk A/D identity with a particular
processor is outside this scalar binding's native qualification; successful
operand A/D updates are compared.

SHLD/SHRD accept word/dword register or memory destinations, a register source
and imm8/CL counts. `DoubleShiftUndefined` supplies an explicit low-word result
and arithmetic-flags image. For a masked count from 1 through the operand
width, only undefined AF and (above one) OF consume that image. Count equal
to the width remains defined. A masked word count above 16 consumes the
supplied low-word result and all six arithmetic flags; destination upper bits
and unrelated flags remain unchanged. A masked count of zero preserves the
operand and flags but still requires a writable, accessible memory operand.
The executor never derives an undefined result from a host-specific formula.
All source, destination and count aliases use their original inputs. These
operations reuse the staged scalar owner and its original fault/restart path;
missing or invalid undefined-state bindings refuse before instruction fetch.

BT/BTS/BTR/BTC require `BitTestUndefinedFlags`, containing only OF/SF/AF/PF.
The selected original bit determines CF; ZF and unrelated flags are preserved.
Register bit bases mask the index to the operand width. For memory, a signed
register index selects a preceding or following whole word/dword before the
final address-sized offset wraps and the segment base is added. An imm8 index
never extends the memory address. BT performs a read; the modifying forms
prepare a writable operand even when the selected bit would not change.
Complete word/dword permissions, bounds and alignment apply, including a
page boundary beyond the selected bit's byte. Faults retain original flags,
registers and operand bytes for the ordinary-RAM profile.

BSF/BSR require `BitScanChoices`. `ZeroResult` supplies the operand-sized
destination only when the source is zero; `Flags` contains only CF/PF/AF/SF/OF.
The executor computes every nonzero bit index and ZF independently. The
producer binds its target CPU: current Intel SDM 093 documents preservation
of the zero-source destination and distinguishes older flag behavior; older
architectural contracts also leave more state undefined. This interface does
not infer a vendor profile or choose one from the host. Source faults precede
destination and flag publication. Missing or malformed bindings fail before
fetch. REP-prefixed TZCNT/LZCNT and LOCK variants are separate instructions
or execution contracts and are not silently interpreted as these forms.

CBW/CWDE sign-extend AL/AX into AX/EAX; CWD/CDQ fill DX/EDX from the sign of
AX/EAX without changing the low half of the dividend. Word operations retain
the destination's upper word. All four preserve flags apart from enclosing
instruction completion clearing RF. Code and operand width remain independent.

LAHF writes only AH, with reserved bits fixed to the architectural image.
SAHF changes only SF/ZF/AF/PF/CF and preserves OF, DF, IF and IOPL. CLC/STC/CMC
own CF alone, and CLD/STD own DF alone. These nonprivileged operations use the
existing explicit scalar binding and require no undefined-flag choice. Their
admitted operand/address/segment prefixes do not cause memory access or change
the operation; malformed/group-one prefixes are refused by the scalar grammar.

BSWAP reverses one dword GPR, including ESP, without accessing memory. Its
undefined word form is refused explicitly. The scalar binding assumes BSWAP
availability; it does not model pre-486 processors lacking the instruction.
XLAT reads exactly one byte at the address-sized sum of (E)BX and unsigned AL,
then writes AL alone. Truncation precedes segment-base addition; a segment
override replaces DS. Operand size does not affect its byte access, and the
usual segment/page checks precede publication.

LEAVE copies EBP or BP to the stack pointer according to SS.B, then reads a
word/dword frame according to operand size. A word stack retains the original
high ESP bits and wraps the updated low SP. The popped word preserves the high
EBP bits. Address-size and segment prefixes do not replace the SS-owned frame
access. Both pointer changes are staged, so a fault exposes the original ESP
and EBP to the handler. All three operations preserve flags apart from normal
instruction completion clearing RF.

SETcc evaluates the shared condition and stores one byte without reading the
destination. Its ModRM reg field is ignored. CMOVcc always reads the complete
source operand first; a false condition preserves the destination but does not
suppress segment, page or alignment faults or the source page's accessed bit.
Both preserve flags and use the existing ordinary-RAM operand owner.
The scalar binding assumes CMOV availability on its bound CPU; it does not
model a processor lacking that feature.

LOOP/LOOPcc decrement CX or ECX according to address size, preserving the upper
word for CX. LOOPE/LOOPNE additionally consume ZF without changing any flags.
JCXZ/JECXZ inspect the same address-sized counter without decrementing it.
Taken targets independently truncate according to operand size before checking
CS.limit. Untaken branches retain the full fallthrough and do not check the
target; a taken target fault restarts with the original counter and EIP.

CALL additionally requires `X86LegacyExecutionOptions::Calls`. Both profiles
preflight the stack before checking the target code limit, so a competing stack
#PF takes precedence and page-table effects survive a later target #GP.
`StoreBeforeTargetLimit` publishes the return address before that limit check;
`CheckBeforeTargetLimit` commits it only after the check succeeds. The qualified
Intel i9-13900H P core requires the first choice and its E core the second.
In both cases the fault frame retains original ESP/EIP. Missing or invalid
choices refuse before fetch; neither the runtime nor the test infers a choice
from the current host. The CPU matrix checks direct and indirect CALL, stack
data and A/D bits. This differs from reading SDM pseudocode as a required
access order. Word-stack POP memory uses incremented low SP while retaining
high ESP; its wrapping cases have separate CPU comparisons.
[Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).

The native handler test executes original MOV to repair a missing PTE,
ADD ESP to remove the error code, IRET to restore the interrupted context,
and retries the original load. Neither side receives a host-side PTE repair
or register adjustment between these instructions. This covers legacy paging
and PAE, CPL0..3, and independent code/stack sizes. It closes that earlier
handler boundary for the admitted scalar instructions; it does not supply a
general i386 source exporter, a TLB/cache model, devices, concurrent shared-memory
synchronization, HLE, task gates, real/VM86 state or other unlisted instructions.
Data/I/O causes use the separate watchpoint profile above. Exclusive-RAM LOCK
forms use the additional binding above.

The [November 3 scalar qualification](evidence/x86-legacy-scalars-2026-11-03.json)
records both CALL counterexamples, 85,120 scalar CPU comparisons across two
SDKs and both core types, 908 passing checks and two explicit LA57 skips.
All 1,032 retained exports per SDK agree byte for byte. The record includes
the exact source patch, binary hashes, build inputs and raw diagnostic hashes;
it does not certify the remaining instruction or machine-state domains.

The [November 3 division qualification](evidence/x86-legacy-division-2026-11-03.json)
records 185,856 DIV/IDIV CPU comparisons across both SDKs, paging modes and
P/E cores. All 428 available checks pass; two LA57 checks remain explicit
skips. Separate regression counts are 85,120 existing scalar comparisons and
9,648 fast-system comparisons. The retained source patch, frozen binaries,
build receipts and raw results identify exactly what was tested.

The [November 3 multiplication qualification](evidence/x86-legacy-multiply-2026-11-03.json)
records 138,240 MUL/IMUL CPU comparisons across both SDKs, paging modes and
P/E cores. All 476 available checks pass; two LA57 checks remain explicit
skips. The independent flag probe passes 2,700 cases per core and detects
748 mismatches per core when the P/E profiles are exchanged. Division,
existing scalars and fast-system instructions retain their separate regression
counts. Source snapshots, frozen binaries and build receipts are retained.

The [November 3 implicit-state qualification](evidence/x86-legacy-status-2026-11-03.json)
records 88,064 CPU comparisons for sign extensions, LAHF/SAHF and carry/direction
controls across both SDKs, paging modes and P/E cores. All 514 available checks
pass; two LA57 checks remain explicit skips. The same runs retain 138,240
multiplication, 185,856 division, 85,120 existing-scalar and 9,648 fast-system
regression comparisons. Executable source hashes and frozen/live binary hashes
were checked after completion.

The [November 3 conditional qualification](evidence/x86-legacy-conditions-2026-11-03.json)
records 100,864 SETcc/CMOVcc and LOOP/LOOPcc/JCXZ/JECXZ CPU comparisons across
both SDKs, paging modes and P/E cores. All 582 available checks pass; two LA57
checks remain explicit skips. The same runs retain 88,064 implicit-state,
138,240 multiplication, 185,856 division, 85,120 existing-scalar and 9,648
fast-system regression comparisons. The shared fixture's new instruction-PC
parameter is exercised at ordinary and high offsets, with independent count
and target widths and original target-fault restart frames.

The [November 3 implicit-operand qualification](evidence/x86-legacy-implicit-operands-2026-11-03.json)
records 60,928 BSWAP/XLAT/LEAVE CPU comparisons across both SDKs, paging modes
and P/E cores. All 658 available checks pass; two LA57 checks are explicitly
skipped. The same runs retain 100,864 conditional, 88,064 status, 138,240
multiplication, 185,856 division, 85,120 existing-scalar and 9,648 fast-system
regression comparisons. The original-byte oracle covers unsigned table
indices, exact byte accesses, nonflat segments, word-stack wrap and original
page-handler repair/IRET/retry. Executable sources and frozen/live binaries
are hashed and checked after all runs.

The [November 3 shift/rotate qualification](evidence/x86-legacy-shifts-2026-11-03.json)
records 218,400 CPU comparisons across both SDKs, paging modes and P/E cores.
All 716 available checks pass; two LA57 checks remain explicit skips. The
oracle covers every raw byte count, all count encodings, register aliases,
segmented memory, zero-count faults and original handler repair/IRET/retry.
Defined bits are calculated independently; only architecturally undefined
bits use the separately observed image. Successful operand page-table A/D
effects are compared; exact faulting-walk A/D identity remains outside this
qualification. Source and binary hashes are verified after all four runs.

The [November 3 double-shift qualification](evidence/x86-legacy-double-shifts-2026-11-03.json)
records 154,624 SHLD/SHRD CPU comparisons across both SDKs, paging modes and
P/E cores. All 772 available full-suite checks pass, with two explicit LA57
skips. Independent arbitrary undefined images and single-bit unit calculations
distinguish the defined word count 16 from the undefined count 17. Native
checks cover source/destination/CL aliases, address-base aliases, segmented
memory, zero-count faults and original page-handler repair/IRET/retry.

A subsequent test-only consolidation shares the actual initial operand and
matches register versus memory observation, using independent scratch RAM.
The recorded P core produces different undefined OF for those forms; E does
not for the reproduced ROL case. The original failures and independent probe
are retained. All 292 final affected checks pass across both SDKs and cores,
including all legacy unit checks on each SDK and an added nonflat SS matrix.
These replays perform 154,624 double-shift and 223,776 single-shift/rotate CPU
comparisons. Production source and both translate-library archives remain
identical to the full-suite qualification; final test sources and binaries
are separately hashed.
After merging the subsequent dev loader/pipeline changes, both rebuilt paging
test executables remain byte-identical to those final qualified binaries.

The same record checks NeverC's existing x64 Microsoft-compatible
`__shiftleft128`/`__shiftright128` interfaces at O0/O2: their LLVM funnel-shift
intrinsics produce native SHLD/SHRD without inline assembly. Those are
return-value code-generation checks, not a complete flag-state API or a
general i386 frontend.

## Protected memory strings

Bind `X86LegacyExecutionOptions::Strings` to `ElementwiseDestinationFirst`
for MOVS/STOS/LODS/CMPS/SCAS over ordinary paged RAM. The original-byte owner
accepts byte/word/dword operands in 16/32-bit code with independent 16/32-bit
address sizes. REP applies to moves, stores and loads; REPE/REPNE applies to
comparisons. Unprefixed forms execute exactly one element without changing
CX/ECX. Source overrides select the loaded CS/DS/ES/SS/FS/GS cache; the
destination always uses ES. Address-sized indices wrap before adding the
segment base, and word updates preserve the high halves of ECX/ESI/EDI.

Each call commits at most one complete element. `RepeatContinue` reports an
unfinished instruction with the remaining count and indices, original EIP,
RF set and the SS/STI shadows ended. The caller can stop for its own budget,
deliver a producer-selected IRQ/NMI through the existing IDT owner, or execute
the next element. Zero count completes without inspecting either data
operand. The original instruction bytes must remain immutable until completion
or event entry; each call still authenticates fetch and reports changed code.
This profile does not infer fast-string grouping or interrupt-arrival timing.

The element operation delegates to the scalar memory and arithmetic owner.
Segment bounds/permissions, explicit alignment priority, page walks, prepared
stores and comparison flags therefore have one implementation. MOVS reads its
source before preparing the destination store. The bound CMPS profile reads
the ES destination first. Each store preflights the entire element; an element
fault publishes neither its data bytes nor its register changes. Completed
earlier elements and page-table effects survive later faults or missing backing.
Comparison repeats retain entry flags until the terminating comparison, so
fault/IRQ/NMI frames preserve the restart image. Existing handlers can repair
the PTE, discard a fault error code and IRET to the remaining count.

LOCK on an otherwise admitted string raises #UD after original-byte fetch,
including when CX/ECX is zero. Repeated/conflicting prefixes and F2 on a
non-comparison remain explicit refusals. Port strings use the separate binding
below, and data/I/O causes use the watchpoint profile above. MMIO, physical
read-ahead, real/VM86 and a general i386 source dispatcher remain outside this
memory-string contract. LLVM and NeverC already expose
native string interfaces; no new compiler intrinsic is needed for this runtime
state transition. The final SDK release, pin and LLVM gitlink stay unchanged.

The architecture basis is the Intel SDM REP/CMPS/SCAS/MOVS/STOS/LODS entries
and exception/interrupt restart rules. Operand access order and intermediate
flags are explicit processor choices, qualified against original instructions
under KVM. See the [qualification record](evidence/x86-legacy-strings-2026-11-05.json).
It records 142,464 string CPU comparisons across the native and pinned-r6 SDKs
on both local P/E core types. Each SDK's full paging suite passes 447 tests;
the one LA57 oracle skip is unrelated to protected legacy paging. The wider
qualification also runs both C routes through Clang/NeverC, paged execution,
continuation source and export regressions. Final test-only include/profile
checks retain byte-identical production translate archives; the record keeps
the distinct test-binary and source identities.

## Protected port devices

Bind `X86LegacyExecutionOptions::PortIO` to an `X86PortIOBinding` for scalar
IN/OUT (DX or imm8) and single/F3 INS/OUTS. Original-byte decoding accepts
byte/word/dword operands in either protected code width, independent 16/32-bit
address sizes, and source segment overrides. INS always uses ES. Word index
and count updates preserve their upper halves and wrap before segment-base
addition. Scalar input preserves the unaffected accumulator bits. LOCK raises
#UD after fetch; F2, repeated prefixes and REP scalar port forms refuse.

The common `X86PortIO` owner also serves the existing x64 paged source binding.
It checks the two-byte I/O permission window as implicit supervisor memory,
including its terminator and unwrapped port extent. CPL <= IOPL bypasses the
TSS; otherwise a 16-bit TSS raises #GP because it has no bitmap. Legacy table
addresses wrap at 32 bits. Permission checks precede zero-count completion;
zero count leaves high count/index halves intact and performs no operand or
device access. Segment checks precede the explicitly bound #AC/#PF order.

Each call commits one element. `RepeatContinue` retains the original EIP and
sets RF; completion advances EIP and clears RF. `DeviceUnavailable` retains
registers, flags and shadows, with no transfer. `InputBeforeWrite` may retain
one input on a destination exception, `InputAfterWriteChecks` preflights the
whole destination, and `InputBeforeWriteFragments` additionally retains earlier
4 KiB store fragments. `DeviceTransferCompleted` and `PortInputBytesWritten`
report effects independently of instruction completion. Faulted elements do
not advance indices/count; handler repair and IRET may consume a new input.
Backing stops before the transfer consume nothing. Callback errors also
guarantee no device effect; callbacks cannot access guest state or RAM.

An accepted event retains `PendingEvent` while its entry is stopped on missing
backing, including secondary fault/#DF state. Repair backing and call
`resumeX86LegacyEvent`; new instructions and events refuse while it is pending.
Resume retries the current entry against retained ordinary RAM and preserves
earlier descriptor/frame stores. It does not replay the triggering device
operation or reapply NMI eligibility. Completion/shutdown clears the pending
entry. While pending, the caller may repair backing but must not execute guest
code or change the architectural state. This continuation is an internal C++
binding, not a serialized source ABI or an instruction-level debugger.

Qualification distinguishes **KVM-intercepted device emulation** from bare CPU
execution. Native byte INS and all OUTS widths qualify original-handler repair;
software checks qualify the elementwise input policies at all widths. On the
recorded KVM, a forward two-element word/dword REP INS across a missing page
reads four values over fault/retry and replaces the first element, while the
explicit elementwise model reads three and retains the completed first element.
A dedicated regression independently checks both images and records this
discrepancy separately from equivalence counts. KVM may also expose an input
batch-completion interrupt before final EIP/RF publication. Output IRQ/NMI
boundaries have native checks; both directions have explicit software
delivery/IRET/resume checks. Single-step injection changes saved TF and is not
used to disguise these differences. KVM's missing port #AC checks remain
separate from architectural alignment unit checks.

The basis is the [Intel SDM](https://cdrdv2-public.intel.com/835781/325462-sdm-vol-1-2abcd-3abcd-4.pdf)
IN/OUT/INS/OUTS, REP, I/O bitmap and 16-bit TSS contracts. Historical
[KVM emulator code](https://github.com/torvalds/linux/blob/v6.17/arch/x86/kvm/emulate.c)
documents input read-ahead and separate writeback/progress publication; it is
not the source identity of the measured host kernel. See the actual runtime
observations in the [qualification record](evidence/x86-legacy-ports-2026-11-05.json).
The profiles bind ordinary RAM and an isolated synchronous device. They do
not predict device read-ahead, physical bus or interrupt timing, MMIO,
self-modifying page-table fragment behavior, real/VM86, or general i386 source
dispatch. LLVM/NeverC already own native port APIs; this change adds runtime
state ownership and does not publish a new LLVM SDK or alter its pin/gitlink.

## Protected SYSENTER and SYSEXIT

Bind `X86LegacyExecutionOptions::FastSystem` to `IntelSEP` and supply
`X86LegacyExecutionState::Sysenter` with the low 32-bit CS, ESP and EIP MSR
inputs. This binding asserts an Intel processor supporting SEP; it does not
derive feature or MSR state from the host. Existing protected-state admission
continues to exclude CET/FRED, real/VM86 and IA-32e. TF requires the separate
instruction-step binding described above.

SYSENTER can enter from CPL0..3. It installs EIP/ESP from those MSRs, selects
CPL0 and clears IF/VM/RF. SYSEXIT requires CPL0, takes its full 32-bit EIP/ESP
from EDX/ECX, selects CPL3 and clears RF while preserving other admitted flags.
Both install fixed flat 32-bit CS/SS caches and retain every other loaded cache
and GPR. Selectors wrap at sixteen bits, including usable null-index selectors.
Neither instruction looks up descriptors or reads either stack. The runtime
does not fetch the destination until the next requested step; a missing target
page therefore faults from the newly installed context on that next step.
NMI blocking survives; completion ends incoming SS/STI inhibition.

The shared strict prefix grammar accepts one prefix from each legacy group.
Operand/address-size, segment and REP prefixes do not narrow these targets.
LOCK delivers #UD after original-byte authentication and before MSR or
privilege checks. An MSR CS value with bits 15:2 clear, or SYSEXIT above CPL0,
delivers #GP(0) with the original instruction's restart state. Fetch failures
precede these instruction faults. Duplicate groups, malformed extents and a
missing profile remain explicit errors before fetch.

The CPU comparison retains complete registers, segment caches, defined flags,
CR2, frame bytes and ordinary RAM. It exercises nonflat entry and absent tables
or destination stacks, in addition to privilege, selector, width and prefix
matrices. `X86FastSystemRules` supplies the fixed cache/selector/flag decisions
to both this runtime and the x64 continuation source owner. This does not add
a general legacy source dispatcher or an implementation of OS services. The
existing LLVM/NeverC native SYSENTER and SYSEXIT interfaces remain separate
terminal compiler operations with their declared ambient-state contracts.
[Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).

The [November 3 transfer qualification](evidence/x86-legacy-fast-system-2026-11-03.json)
records 19,296 original SYSENTER/SYSEXIT comparisons across both SDKs and core
types, 85,120 scalar regression comparisons, 946 passing tests and two LA57
skips. The retained 1,032 exports per SDK agree byte for byte. The full LLVM
fork now independently qualifies native code16 SYSEXIT32, IRET and far-return
compiler transport in [the code16 evidence](evidence/x86-native-code16-transfers-2026-11-03.json).
NeverC's absent i386 frontend target remains separate from this software interface.
After merging the subsequent `dev` source-address changes, 154 focused tests
and 9,648 original CPU comparisons pass on the resulting tree. The evidence
records this integration separately and retains both SDK build inputs.

## Protected IRET and far returns

Canonical `CF`, `CB` and `CA imm16`, optionally prefixed with `66`, execute
16/32-bit protected returns through `X86LegacyFrameReturn`. The original-byte
executor owns fetch, restart EIP, fault delivery and completion. The return
owner stages GPRs, caches and flags and stops at the destination before executing
it. `ir/X86/X86ReturnRules` shares return-code privilege checks, data-cache
invalidation and IRET flag restoration with the x64 continuation/source owner.

Bind `X86LegacyExecutionOptions::Returns` to `OrderedFrame`. IRET reads
FLAGS, CS and IP; RETF reads CS and IP. Both fetch and validate the code
descriptor, publish its qword accessed update, then read SS before SP on an
outer return. SS descriptor validation and its accessed write precede the
final target IP-limit check. A same-CPL return never accesses an outer frame.
Each field uses the current SS base, bounds and D/B-controlled stack offset;
the frame slot width remains independent of CS.D and SS.D/B. CS and SS each
read only their two-byte selector, including in four-byte slots. Their padding
does not trigger page, segment-limit or alignment faults. IP, SP and flags
retain their full operand width. These access-order
choices bind the qualified processor, rather than generalizing the SDM's
pseudocode order. Active CPL3 alignment checking needs the explicit alignment
priority, independently of the return profile.

The target code segment may be 16 or 32 bits, conforming or nonconforming,
and selected from the GDT or LDT. The operand width controls truncation of
the returned IP. A 16-bit destination CS alone does not truncate a 32-bit EIP.
An outer return replaces SP/ESP according to the target SS.D/B, retaining the
old ESP high word for a 16-bit stack. RETF's unsigned immediate skips current
parameters before locating an outer frame and also adjusts the destination
stack, with the applicable stack-width wrap. An outer return invalidates
inaccessible ES/DS/FS/GS caches; a same-CPL return preserves them. Null-loaded
unusable cache fields remain opaque.

Original CPL and IOPL govern IF/IOPL/VIF/VIP restoration. Word IRET retains
AC/ID/VIF/VIP and clears RF; IRETD restores RF/AC/ID, with VIF/VIP writable at
CPL0. RETF clears RF and preserves the other flags. Successful returns end
SS/STI inhibition. IRET unblocks NMI after byte authentication even if a later
frame or descriptor access faults; RETF retains NMI blocking. Ordered RAM and
page-table effects survive later faults and backing failures. Missing physical
backing never manufactures a guest #PF, and retry does not duplicate an ESP
increment. The fault path enters the existing IDT owner with the original EIP.

Nested-task IRET is refused before execution because task-switch state is
not owned. A CPL0 frame requesting VM86 is refused after reading its flags,
retaining NMI unblocking; it is not misreported as an ordinary protected
return. Restored TF is preserved; the instruction-step binding samples entry
TF independently of the restored frame.
Reserved legacy descriptor bits, CET/FRED and unbound profiles remain explicit
errors. The error holders in selector, event and return transitions preserve
LLVM's checked-error contract, including these dynamically discovered refusals.

`LegacyReturns/*` executes original IRET/RETF bytes in KVM under legacy paging
and PAE. It checks all privilege levels, code/operand/stack width combinations,
nonflat code, GDT/LDT, descriptor and frame faults, parameter cleanup, high
ESP, wrapping, flag/IOPL combinations and descriptor/frame aliases. Page and
segment boundaries distinguish selector bytes from slot padding,
including an actually inaccessible second selector byte. Word-aligned but
dword-misaligned frames distinguish CS access from IP/flags accesses. Two adjacent
execution breakpoints independently witness restored RF: it suppresses the
first stop only. INT/IRQ/NMI entry followed by IRET resumes the interrupted
code. Page-repair cases supply a repaired PTE and consumed error code at the
handler boundary, then execute original IRETD and retry the original return.
The repair itself is an explicit embedding operation, not a claim that this
legacy subset executes a general page-fault handler.

The oracle compares all eight GPRs, usable caches, selectors/unusable bits,
flags, NMI state, CR2, restart frames and retained descriptor RAM. Debugger RF
and inactive cache fields are not used as direct cache/flag equality witnesses.
[Linux VMX's #AC reinjection](https://github.com/torvalds/linux/blob/master/arch/x86/kvm/vmx/vmx.c)
can restore NMI blocking after IRET; this known VMM
discrepancy is counted separately while the model keeps architectural NMI
unblocking. See the [return qualification](evidence/x86-legacy-returns-2026-11-03.json)
for the two SDKs, both hybrid core profiles, commands, hashes and exclusions.
The final matrix records 46,008 new return comparisons, with 96 VMX #AC/NMI
reinjection observations reported separately. The 22 regression runs contain
1,794 passes and 42 explicit skips for unavailable hardware, SDK interfaces
or optional type providers. The additional selector-boundary probes exposed
and corrected four-byte CS/SS overreads after the earlier matrix had passed.
Superseded runs do not enter these totals. Each SDK exports 1,032 shared
paged/continuation artifacts, all byte-identical across the SDKs.
After integrating `dc0f9db8f1`'s supplemental no-return lookup change, both
SDKs pass 784 additional pipeline, no-return, source/export, paged execution
and CLI checks without skips. The legacy return/event/segment/page inputs
remain unchanged, and all 1,032 exports per SDK still match the primary
qualification. These integration results are recorded separately; the
original qualified binaries are retained by hash.
The architectural reference is Intel SDM revision 093, volume 2 IRET and RET,
available from the [Intel manual page](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).

## Protected BOUND and native source projection

The original-byte scalar executor admits word/dword BOUND in protected
16/32-bit code with independent 16/32-bit address sizes. It reads the signed
lower bound, compares the unchanged index, and only reads the upper bound
when the lower comparison succeeds. Each field adds and wraps its own offset
at the instruction's address width before segment and paging checks; bytes
within a field do not wrap around a segment limit. The comparison is inclusive
at both ends, without adding the element size to the upper value.

A below-lower index therefore raises #BR even when the upper field would cause
#GP/#SS/#PF. A lower-field access fault precedes the comparison. Alignment is
that of the individual word/dword, and competing #AC/#PF uses the existing
explicit alignment profile. Neither the index nor arithmetic flags change;
ordinary completion and fault-frame RF follow the shared executor's rules.
Original #BR and upper-field #PF handlers can repair the index or page, execute
IRET, and retry through the same dispatcher.

`X86LegacyExecutionState::Mpx` optionally binds the existing `X86MpxContext`.
BOUND derives CPL and CR4.OSXSAVE from the executing machine, then requires a
known, admitted MPX enable state. Without a context, CR4.OSXSAVE=0 proves MPX
is disabled; an unknown enabled configuration is refused before fetch.
An explicitly active transaction is also refused before fetch, even when MPX
is disabled: the dispatcher has no transaction checkpoint/abort continuation.
On #BR, the existing MPX owner clears enabled BNDSTATUS before IDT delivery.
This effect survives a delivery fault or shutdown. Bounds, disabled status,
successful checks and operand faults leave that status unchanged. This binding
does not extend the scalar dispatcher to other MPX instructions or transactions.

The full LLVM fork provides `llvm.x86.bound16`/`bound32` with index and pointer
inputs, and `.address32` forms with index, 32-bit offset, constant address width
and segment-prefix inputs. Matching Clang builtins use the names
`__builtin_ia32_bound16`/`bound32` and the `_address32` suffix. All return void;
unused calls and memory ordering remain observable. Code16 and i386 use native
machine instructions with a read-only index; long mode and invalid address
controls are rejected. The pointer form uses a 32-bit DS offset even in code16.

NeverD's LowIR/MedIR contract keeps a normalized eight-byte offset and explicit
address/segment controls. Both C routes render the same native builtin and
retain an exact GNU assembly alternative for the pinned SDK/compiler. The
shared returned-value rule preserves a propagated full-width parameter or
register value when a later narrow register view exists. This also applies to
explicit integer/pointer source returns. Reading AX with BOUND must not turn a
function returning its input EAX into a signed sixteen-bit return; storing AX
must likewise preserve a declared x64 RAX return.

`LegacyBound/*` checks original CPU instruction/state/RAM behavior, fault order,
16-bit and 32-bit address wrap, and original handler retry under both legacy
paging and PAE. `X86BoundSource.*` exports 168 image/configuration combinations
through both C routes, LLVM IR and object code, and rejects malformed contracts.
The NeverC repository's `verify_bound.py`, `verify_bound_source.py` and
`verify_protected_bound.py` retain independent byte witnesses, compiler/tool
hashes and deliberate wrong-input/return controls. See the
[BOUND qualification](evidence/x86-legacy-bound-2026-11-04.json) for actual
profiles, counts, failures repaired during development and exclusions.

These interfaces do not create a NeverC i386 frontend or general original-state
execution for arbitrary generated C. MPX status has software-owner tests on
this host; physical MPX hardware is unavailable. Global instruction/transfer
completion and the final LLVM release/pin remain separate unfinished work.
The architectural reference is Intel SDM revision 093, Volume 2A BOUND,
from the [Intel manual page](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).

## Protected far transfers

`X86LegacyExecutionOptions::FarTransfers=OrderedFrame` adds protected far
CALL/JMP to the original-byte execution boundary. It accepts immediate
ptr16:16/32 and memory m16:16/32 forms, with independent operand/address sizes
and segment overrides. The memory pointer's offset is read before its selector;
each field's starting offset wraps at the address size. Complete fields still
obey the common segment extent checks. Effective addresses are captured before
any call-frame write, including ESP-based pointers that alias the frame.

The owner validates null selectors, GDT/LDT extents, code type, conforming and
nonconforming privilege rules, gate DPL/RPL, presence and target limits. Direct
branches retain CPL. A call gate takes its destination and width from the gate,
ignoring the operand's offset and operand-size prefix. JMP through a gate cannot
enter a more privileged nonconforming segment. CALL can select a 16/32-bit TSS
stack for an inner ring and copy the gate's zero-to-31 parameters. Target SS.B
controls stack address size independently of gate/TSS width; word stacks retain
the old ESP high word.

The explicitly bound ordinary-RAM profile retains code/stack descriptor accessed
writes and ordered frame stores on later faults. It saves old SS:SP before
reading the highest parameter, then reads and pushes each parameter downward,
followed by old CS and the return offset. Overlapping stacks therefore observe
earlier writes. The target limit is checked after frame stores. Registers and
caches publish together on success; faults enter the existing IDT owner with
the original instruction PC. Missing backing can stop the transfer or a pending
event; the latter resumes through `resumeX86LegacyEvent`. RETF imm16 composes
with gate entry and removes parameters from both stack contexts.

LOCK delivers #UD only after authenticated fetch. Duplicate prefixes, REP/BND
prefixes and register forms of FF /3 or /5 refuse explicitly. Task-gate/TSS
transfers, reserved L-bit targets, VM86 and CET/FRED remain outside this
boundary. TF requires the separate instruction-step debug binding. They do not become guessed near calls or NOPs.

The ordinary lifter previously mapped far calls/jumps to near control flow,
discarding the selector and possible stack/privilege transition. It now refuses
them before publication in both strict and permissive modes. This includes
Capstone's CALL/JMP aliases for FF /3 and /5. Native LLVM/NeverC far-transfer
intrinsics are independently qualified in the
[native compiler evidence](evidence/x86-native-far-transfers-2026-11-05.json).
Their declared compiler ABI does not supply general original-state i386 source
execution, which still requires its complete dispatch boundary.
No LLVM release, SDK pin or submodule change is made for this boundary.

Architectural references are Intel SDM Volume 2A CALL/JMP and Volume 3A's
protection and call-gate chapters, from the
[Intel manual page](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).

The [November 5 far-transfer qualification](evidence/x86-legacy-far-transfers-2026-11-05.json)
records 1,700 tests across the native LLVM SDK and pinned r6: 1,698 passed,
two explicit LA57 skips and no failures. The new far-transfer matrix performs
10,912 original-CPU comparisons across both SDKs and qualified P/E cores.
The source/export suites are regression checks for existing execution paths;
they do not qualify a new i386 far-transfer source compiler. Diagnostic logs
retain the parameter-copy ordering mismatch and the Capstone alias probe.

## Ordered PUSHA, POPA and ENTER

The bound `OrderedRAM` scalar profile executes original PUSHA/PUSHAD,
POPA/POPAD and ENTER bytes in protected 16/32-bit code. Operand prefixes select
word/dword data independently of CS.D and SS.B. Segment/address overrides do
not change their implicit SS references. Malformed/trailing bytes, duplicate
prefixes and LOCK/REP forms are explicit grammar errors before fetch.

PUSHA pushes registers in encoding order and saves the entry SP/ESP value.
POPA loads them in reverse order, discarding the saved SP value. In the
qualified ordered-RAM model that discarded slot is still read: original P/E
cores produce both its page fault and its data breakpoint. Treating the SDM's
discard operation as an absent memory access gives the wrong CR2/debug result.
Completed PUSHA/ENTER stores survive later faults. Completed POPA register loads
also survive; ESP remains the entry value until the whole instruction succeeds.
The existing event owner delivers faults with the original instruction PC and
entry stack pointer, preserving those partial effects.

ENTER masks nesting to five bits. It copies the previous display in order,
including aliases of its earlier pushes. Display addresses subtract at the
SS.B stack width. The saved frame value uses operand-width subtraction of
entry ESP, including a borrow across bit 16 on a word stack; final word BP
publication preserves the original EBP high word. Allocation adjusts SP/ESP
at the stack width. The final pointer receives an operand-sized write-permission
and alignment check: this can set page A/D bits or fault, but stores no data and
does not trigger a data-write comparator. The entire frame is not preflighted.
These order/partial-publication choices are explicit target bindings, measured
on the qualified Intel cores, not inferred from the executing host.

A physical backing failure retains `PendingStack`, independently of the guest
exception mechanism. Keep the same architectural state, profiles and RAM;
repair unavailable backing, then call the same original instruction again.
Completed reads retain their values and completed stores are skipped. The
original authenticated bytes also remain retained when a push has overwritten
them. Resumption cannot accept different bytes, register/cache state or external
events. Debug matches survive a backing stop and are published only at normal
completion; an architectural fault discards the speculative cause. A fault
clears `PendingStack` before IDT delivery, so handler repair/IRET starts a new
attempt. The generated protected C dispatcher uses this same owner and charges
the retained instruction only once.

The ordinary lifter separately fixes word PUSHA/POPA to use a complete flat
i386 stack address. Its ENTER display cursor now subtracts at pointer width,
while final word BP publication preserves the original high bits. These
operations already lower through ordinary LLVM IR and compile with the pinned
r6 and current native SDK; no new inline-assembly fallback, LLVM intrinsic or
NeverC builtin is needed for this change. This successful-memory projection
does not acquire the protected executor's fault or continuation contract.

The architectural reference is the ENTER/PUSHA/POPA entries in
[Intel SDM Volume 2](https://cdrdv2-public.intel.com/789581/325383-sdm-vol-2abcd.pdf).
The [stack qualification](evidence/x86-legacy-stack-2026-11-07.json) retains
the original-CPU experiments and the final test/build receipts. The initial
CR2, discarded-slot breakpoint and frame-borrow mismatches are kept as
diagnostics, separate from passing qualification.

The final native-SDK/r6 matrix passes all 976 tests without skips across the
qualified P/E cores. It includes 14,144 original stack/debug comparisons,
12 original-versus-rewritten program pairs and 48 new generated-source cases.
A separately retained, earlier executable passes 592 of 593 full paging tests;
its one LA57 test is unavailable on this host. The evidence records that
executable separately from the final code and preserves the actual commit,
compiler, binary and exported-artifact hashes for each qualification lane.

Real/VM86, compatibility execution, task switching, other unbound i386 banks,
software SMM/SGX transitions and general external-event production remain
outside this boundary. Global completion is still open; LLVM release, SDK pin
and the LLVM submodule stay at r6.

## Protected integer extensions

The `OrderedRAM` scalar binding admits exact legacy POPCNT, LZCNT, TZCNT,
ADCX and ADOX bytes. Count operands use the code default and independent 66
override. ADX always uses dword operands in protected code16/code32; 66 selects
ADCX, and ADOX ignores an additional 66 in either prefix order. The common
address parser retains 16/32-bit addresses, segment overrides and implicit SS.
Duplicate prefixes, LOCK/REPNE selectors, absent mandatory selectors and
truncated/trailing forms remain explicit grammar refusals before fetch.

`X86LegacyExecutionOptions::IntegerExtensions` supplies target POPCNT, LZCNT,
BMI1 and ADX availability. These are explicit producer inputs, never inferred
from host CPUID. Missing POPCNT/ADX produces guest #UD after original-byte
fetch and before operand access. Missing LZCNT/BMI1 selects BSR/BSF, including
their existing `BitScanChoices` for zero results and non-ZF flags. The enabled
LZCNT/TZCNT path instead requires `BitCountUndefinedFlags` containing only
OF/SF/AF/PF. Its result, source-zero CF and result-zero ZF are always computed.
POPCNT computes all six arithmetic flags. ADCX/ADOX update only their chosen
CF/OF chain, and both use unsigned carry; the other chain and flags survive.

Memory operands use the shared segment, paging and alignment owner. Faults
preserve the destination and entry flags, while retaining page-table effects.
The existing original fault handler can repair a PTE, IRET and retry. Completed
reads participate in the common data-breakpoint owner. Generated protected C
passes through this same execution boundary with its existing attempt budget.
Ordinary lifting and LLVM/NeverC already provide these integer computations;
this addition requires no new compiler intrinsic or inline-assembly fallback.

The reference is the POPCNT/LZCNT/TZCNT/ADCX/ADOX entries in
[Intel SDM Volume 2](https://cdrdv2-public.intel.com/789581/325383-sdm-vol-2abcd.pdf).
Original-CPU checks require those features and cover the current Intel host.
Feature-disabled aliases and #UD priority are separately qualified software
contracts, not physical execution on a processor missing those features.

The [November 7 integer qualification](evidence/x86-legacy-integer-2026-11-07.json)
passes 1,098 tests across the native SDK, pinned r6 and qualified P/E cores,
including 33,696 original integer-state comparisons, 960 original data-debug
comparisons and 48 new compiled-source cases. Decoder and ordinary-lift checks
also cover the integrated Capstone revision and the preceding stack fixes.
After a later Android-only merge, both rebuilt shared libraries pass another
130 source checks; the paging executables remain byte-identical. The archive
retains each actual source commit, tool/binary hash and generated source/library,
with the pre-integration receipts recorded separately.

The other state banks and the global LLVM release gate remain open.

## Protected BMI1/BMI2

The `OrderedRAM` scalar binding admits the thirteen VEX GPR forms ANDN,
BEXTR, BLSI, BLSMSK, BLSR, BZHI, MULX, PDEP, PEXT, RORX, SARX, SHLX and SHRX.
All use dword operands in both code16 and code32. The shared address reader
retains code-default addressing, 67, segment overrides, ModRM/SIB, displacement
wrap and implicit SS. Non-64-bit W, B and the high encoded register bit do not
extend operands. RORX requires its reserved vvvv field. C4 with a legacy
memory ModRM remains LES. Reserved maps/selectors/L, legacy 66/F2/F3/LOCK
prefixes, duplicate prefixes and truncated/trailing extents remain explicit
grammar refusals before fetch, rather than guessed instruction semantics.

`IntegerExtensions` now also binds BMI2. BMI1 selects ANDN/BEXTR/BLSI/BLSMSK/BLSR;
BMI2 selects the other eight forms. Missing features deliver guest #UD after
the original instruction fetch and before any operand access. No AVX state,
XCR0 or host CPUID is inferred. Enabled flag-writing forms require
`BMIUndefinedFlags`: AF/PF for the logic/lowest-bit/BZHI forms, and SF/AF/PF
for BEXTR. Defined flags cannot be supplied by that image. BEXTR preserves
control flags, computes ZF and clears CF/OF. BZHI uses the low eight index bits;
an out-of-range index preserves the source and sets CF. The seven remaining
BMI2 forms preserve the complete entry flag image.

MULX captures EDX and the source before either destination changes. Its low
half is committed before the high half, so the high half wins when both
destinations name the same register. A source fault or backing stop publishes
neither destination. All forms use the existing segmented, paged, aligned
source-read boundary and data-debug owner. Original guest handlers can repair
the page and IRET to retry the same bytes. Generated C uses this same owner
and attempt budget; ordinary LLVM and NeverC BMI computations already existed.

The reference is the VEX GPR section, Type 13 exception table and corresponding
instruction entries in [Intel SDM Volume 2](https://cdrdv2-public.intel.com/789581/325383-sdm-vol-2abcd.pdf).
The reference has contradictory prose for BEXTR W and BLSI carry; the operation
definitions, non-64-bit width rules and independent original CPU comparisons
determine the admitted behavior. Original comparisons cover the available
Intel BMI1/BMI2 host. Disabled-feature #UD is a separate software contract,
not physical qualification on a processor missing BMI.

The [November 7 BMI qualification](evidence/x86-legacy-bmi-2026-11-07.json)
passes 1,084 checks with no failures or skips across the native SDK, pinned
r6 and qualified P/E cores. It includes 131,264 original BMI state comparisons,
2,496 original data-debug comparisons and 48 new compiled-source cases, plus
shared scalar/integer/segment, decoder and ordinary BMI/BEXTR regressions.
The retained archive binds the actual clean source commit, dependency inputs,
compiler and executable hashes, generated sources, libraries and raw results.

Global completion remains open, including other unbound state banks, task
switching, real/VM86 and general compatibility execution, software SMM/SGX
transitions and external-event producers. LLVM release and pin remain at r6.

## Protected descriptor queries

`DescriptorQueries = X86LegacyDescriptorProfile::TableImage` enables LAR,
LSL, VERR and VERW under `OrderedRAM`. Code and address widths are independent;
66 changes LAR/LSL destination width and 67 changes memory addressing. The
selector is always a word, including the low word of a register source. LAR
and LSL preserve the full destination on failure; a successful word result
preserves its high half. Only ZF changes. Register aliases capture the selector
before publishing a destination.

Null selectors, an unusable LDT, descriptors extending past the table limit,
invisible privilege levels and disallowed types clear ZF. Conforming code
bypasses the CPL/RPL versus DPL comparison. LAR admits data/code, LDT, 16/32-bit
TSS and call/task gates; LSL excludes gates. VERR admits readable code/data,
while VERW admits only writable data. Presence does not affect these queries,
and no query sets the descriptor accessed bit or changes a segment cache.

The explicit source word uses ordinary segment, page and alignment checks.
Table reads are implicit supervisor accesses and remain faultable, even when
a later type/privilege check would fail. An architectural fault enters the
shared event owner with the original PC/GPR/flags; a physical backing stop
preserves them for retry. Completed page-walk effects remain observable.
Generated protected C uses the same dispatcher and attempt budget, including
original handler repair followed by IRET and retry.

The table profile returns the original descriptor limit nibble in LAR bits
19:16. Intel defines these four result bits as undefined; this profile is an
explicit CPU choice rather than an ISA-wide claim. LSL computes the granular
byte limit before destination truncation. Prefix and exact-extent admission
remains strict: LOCK/F2/F3, duplicate prefixes, truncated and trailing bytes
are rejected before fetch. Unbound query profiles also refuse before fetch.

The architectural reference is LAR, LSL and VERR/VERW in
[Intel SDM Volume 2](https://cdrdv2-public.intel.com/789581/325383-sdm-vol-2abcd.pdf).
This query owner does not load LDTR/TR or implement task switching. Existing
native LLVM descriptor interfaces are independent of this owned-RAM execution
path. NeverC now exposes 28 previously x64-only descriptor/machine builtin
forms to i386 source through the common registry, preserving seven x64-only
wide forms. Its explicit full LLVM backend emits the original instructions;
privileged forms are checked for code generation, and unprivileged queries
also execute in original Linux i386/x64 processes at O0/O2.

The [November 7 descriptor qualification](evidence/x86-legacy-descriptors-2026-11-07.json)
records 1,154 passing checks, 196,608 original descriptor-state comparisons,
1,792 original data-debug comparisons and 48 new compiled-source cases across
both SDKs and qualified P/E cores. Another 566 post-integration checks pass,
including both complete type suites with the existing kernel PDB and all four
generated-C suites. The paging executables remain byte-identical. Initial
type runs without the PDB binding are retained as superseded records, with
their skips excluded from the final counts. NeverC separately passes 10,560
native query comparisons and 252 code-generation form checks across P/E cores.
The archive binds actual source commits, compiler/dependency inputs, binaries,
generated sources/libraries and raw result hashes. The LLVM release gate
remains open and the pin remains r6.

## Protected system selectors

`SystemSelectors = X86LegacySystemSelectorProfile::ExclusiveTable` binds
SLDT, STR, LLDT and LTR to one CPU with exclusive ordinary RAM. This is not a
synchronization primitive for concurrent CPUs, DMA or MMIO. SLDT/STR write
only a selector word to memory; word registers preserve their upper half and
dword registers zero-extend. This is the modern target choice for SLDT's
historically undefined upper register half. Stores do not read either table,
and CR4.UMIP with CPL greater than zero raises #GP(0) before operand access.

LLDT/LTR require CPL0 before reading their source word; operand-size prefixes
do not widen the selector. GDT-only selection, table bounds, type and presence
checks retain their architectural fault classes and order. Null LLDT preserves
the supplied selector and marks the cache unusable; its hidden fields remain
opaque. Null LTR faults. LLDT admits LDT descriptors; LTR admits available
16/32-bit TSS descriptors and refuses busy TSS descriptors. Neither instruction
checks DPL against RPL, accesses the TSS contents or changes EFLAGS.

Implicit descriptor reads use supervisor access and 32-bit linear wrap. LTR
then prepares a whole-qword write, reads the retained physical fragments and
sets only the busy bit. The loaded TR cache uses that updated image, including
page-walk A/D bits when the descriptor aliases its own page table.
Read, write-protection and physical backing failures
remain distinct. The original core/cache state is retained until the complete
operation succeeds, while completed page-walk effects remain observable.
TR publication precedes post-instruction debug/event delivery. Loading TR does
not perform task switching or set CR0.TS. LLDT retains the raw descriptor flag
nibble; its unused L bit must not cause the next instruction's state validation
to reject that cache.

The shared scalar owner provides segment/address decoding, exact instruction
extent, alignment, data-watchpoint observation and generated-C dispatch. Missing
profiles or malformed prefixes fail before fetch. A guest page-fault handler
can make the original table writable, return with IRET and retry LTR without
publishing a premature cache or busy bit. The generated-C tests also consume a
newly loaded LDT through LSL and read back both system selectors.

The references are SLDT, STR, LLDT and LTR in
[Intel SDM Volume 2](https://cdrdv2-public.intel.com/789581/325383-sdm-vol-2abcd.pdf).
The existing LLVM intrinsic lowering and NeverC's common i386/x64 registry
already emit these native forms. This owned protected-state work does not
advance the LLVM release or r6 pin. Other state banks, task switching,
real/VM86/general compatibility execution, software SMM/SGX transitions and
unowned external-event producers remain outside the completed contract.
