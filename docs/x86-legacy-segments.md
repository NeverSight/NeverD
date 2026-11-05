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

This is a bounded internal execution interface. The scalar binding below
extends its ordinary instruction execution. General i386 source
dispatch, task/call-gate transitions, real/VM86, CET/FRED, software
SMM, enclave/AEX/EPCM and the remaining device/partial-progress contracts are
not completed by it. No new LLVM or NeverC intrinsic is required for these
runtime state transitions. The final LLVM release, SDK pin and NeverD LLVM
gitlink remain unchanged pending global completion.

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
but the next step still requires the separate debug-event producer.

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
synchronization, HLE, task gates, real/VM86 state, debug producers or other
unlisted instructions. Exclusive-RAM LOCK forms use the additional binding above.

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
non-comparison remain explicit refusals. Port strings INS/OUTS, MMIO, physical
read-ahead, debug producers, real/VM86 and a general i386 source dispatcher
remain outside this memory-string contract. LLVM and NeverC already expose
native string interfaces; no new compiler intrinsic is needed for this runtime
state transition. The final SDK release, pin and LLVM gitlink stay unchanged.

The architecture basis is the Intel SDM REP/CMPS/SCAS/MOVS/STOS/LODS entries
and exception/interrupt restart rules. Operand access order and intermediate
flags are explicit processor choices, qualified against original instructions
under KVM. See the [qualification record](evidence/x86-legacy-strings-2026-11-05.json).

## Protected SYSENTER and SYSEXIT

Bind `X86LegacyExecutionOptions::FastSystem` to `IntelSEP` and supply
`X86LegacyExecutionState::Sysenter` with the low 32-bit CS, ESP and EIP MSR
inputs. This binding asserts an Intel processor supporting SEP; it does not
derive feature or MSR state from the host. Existing protected-state admission
continues to exclude CET/FRED, real/VM86, IA-32e and TF debug production.

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
