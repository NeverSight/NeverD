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
task/call gates, real/VM86 modes, software SMM, enclave
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
accesses and again after completion. Event selection and APIC/debug producer
state remain the embedding caller's responsibility.

A step authenticates each original instruction byte through CS and paging.
It stops at the first changed byte, preserving earlier page-walk effects and
requiring the caller to decode the new code. A fetch or operand exception
retains the original EIP and enters the IDT. Successful execution publishes
fallthrough, clears RF, and advances SS/STI inhibition. The admitted byte
grammar includes the segment instructions, NOP, register-immediate MOV,
CLI/STI with IOPL/PVI checks, INT, INT3, INTO, INT1, UD2 and the protected
IRET/RETF returns described below. An unimplemented
instruction returns an error; only an authenticated UD2 generates its #UD.
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
a debug producer and are refused by this step interface. Native debug
registers and delayed data-breakpoint state are not silently approximated.

The architecture basis is Intel SDM revision 093, volume 2 INT/MOV/POP/STI/CLI
and volume 3A chapter 7. Target-specific access ordering and stack high-bit
behavior are qualified against original instructions in KVM, not inferred
from the pseudocode's textual evaluation order.
[Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).
The [November 3 evidence](evidence/x86-legacy-event-entry-2026-11-03.json)
records commands, source hashes, SDK inputs, CPU affinity, independent native
matrices, corrected probes and regression results.

This is a bounded internal execution interface. General i386 scalar/source
dispatch, task/call-gate transitions, real/VM86, CET/FRED, software
SMM, enclave/AEX/EPCM and the remaining device/partial-progress contracts are
not completed by it. No new LLVM or NeverC intrinsic is required for these
runtime state transitions. The final LLVM release, SDK pin and NeverD LLVM
gitlink remain unchanged pending global completion.

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
return. Restored TF is preserved, and the next step requires a debug producer.
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
