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
