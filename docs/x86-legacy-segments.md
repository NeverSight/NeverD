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

This layer does not complete the legacy instruction dispatcher, segment-load
state transitions, task/call gates, real/VM86 modes, software SMM, enclave
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
