//===- JumpTableResolver.cpp - Jump table detection and resolution --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Jump table resolution from indirect branch patterns and metadata
/// extraction into LowFunc::JumpTables.
///
/// The resolver uses a multi-strategy approach with fallback:
///
///   1. **ARM-family detectors** — the architecture-gated recognizers for
///      the ARM TBB/TBH table-branch and the AArch64 compact byte/halfword
///      table.  These are the only target-specific strategies and live in
///      JumpTableResolverARM.cpp; every strategy below is architecture-neutral.
///
///   2. **PIC-relative tables** — handle the common x64 pattern where
///      each table entry is a 32-bit signed offset from the table base:
///        target = base + (int32_t)table[index]
///
///   3. **Symbolic dispatch decomposition** — execute the dispatch with each
///      register input symbolic and recover an exact linear table shape.
///
///   4. **Backward slicing** — trace data flow from the INDIR_BR input
///      through INT_ADD, INT_MULT, LOAD, INT_ZEXT, INT_LEFT, INT_RIGHT,
///      INT_ASHR, INT_SEXT, SUBBYTES, and COPY to identify the base
///      address and entry layout.  Cross-instruction base recovery lives in
///      JumpTableResolverSource.cpp, stack-materialized sources in
///      JumpTableResolverStack.cpp, and composite layouts in
///      JumpTableResolverShapes.cpp.
///
///   5. **Guard analysis** — scan preceding instructions *and* CFG
///      predecessor blocks for comparison/mask ops (INT_LESS,
///      INT_LESSEQUAL, INT_SUB, INT_AND) that bound the switch
///      variable, giving a precise entry count.  Lives in
///      JumpTableResolverGuards.cpp.
///
///   6. **Multi-format entries** — read 1, 2, 4, or 8 byte entries,
///      both signed and unsigned, with tolerance for sparse invalid
///      entries in bounded tables.
///
///   7. **Sanity validation** — each target is checked for executable
///      segment membership, data availability at the target address,
///      reasonable distance from the function, and duplicate-run limits.
///
///   8. **Multi-stage fallback** — when the primary strategy produces
///      too few entries, retry with alternative entry sizes to recover
///      tables that use an unexpected format.  Path collection and dispatch
///      emulation fallbacks live in JumpTableResolverEmu.cpp.
///
/// This file holds the strategy dispatch itself.  The framework pieces it
/// drives are split by responsibility across sibling translation units:
/// backward slicing in JumpTableResolverSlice.cpp, table-base constant folding
/// in JumpTableResolverFold.cpp, index normalization and stride in
/// JumpTableResolverNorm.cpp, guard-free entry-count bounds in
/// JumpTableResolverBounds.cpp, entry decoding and target validation in
/// JumpTableResolverTargets.cpp, and case-label / metadata construction in
/// JumpTableResolverExtract.cpp.
///
/// See CFGBuilder.cpp for the main CFG construction logic.
///
//===----------------------------------------------------------------------===//

#include "JumpTableResolverDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/symbolic/SymDispatch.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#define DEBUG_TYPE "neverd-cfg-builder"

namespace neverd {

//===----------------------------------------------------------------------===//
// ARM-family target detectors (tryARMTableBranch, tryAArch64CompactTable) live
// in JumpTableResolverARM.cpp.  They are the only architecture-gated table
// recognizers; every other strategy -- the guard/bounds analysis in
// JumpTableResolverGuards.cpp, source/stack/shape detectors in their dedicated
// JumpTableResolver*.cpp files, and the framework below -- is pattern-based and
// architecture-neutral, so there is no corresponding x86 detector to split
// out (LLVM target-dispatch pattern).
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// resolveJumpTable — top-level multi-strategy resolution
//===----------------------------------------------------------------------===//

std::vector<va_t> CFGBuilder::resolveJumpTable(const BinaryImage &Img,
                                               const InsnRecord &Rec) {
  JumpTableInfo Info;

  // Strategies are tried most-specific first, falling back to the generic
  // pattern matchers.  Strategies 1 and 1b are the architecture-gated
  // ARM-family detectors (defined in JumpTableResolverARM.cpp); every strategy
  // from 1c onward is architecture-neutral pattern matching that covers x86,
  // x64, ARM32, and AArch64 alike — which is why there is no x86-specific
  // detector to dispatch to here.

  // Strategy 1: ARM32 TBB/TBH table-branch (most specific, check first).
  bool Recovered = tryARMTableBranch(Img, Rec, Info);

  // Strategy 1b: AArch64 compact byte/halfword table (separate entry base and
  // code anchor, scaled entries) — must precede the generic relative resolver,
  // which would otherwise latch onto the entry base as the (wrong) target base.
  if (!Recovered)
    Recovered = tryAArch64CompactTable(Img, Rec, Info);

  // Strategy 1c: runtime-selected table base (`base = cond ? A : B; jmp
  // *base[idx]`) — two adjacent code-pointer tables merged into one.  Must
  // precede the generic relative/cross-instruction resolvers, which would fold
  // only one arm of the select and recover half the table.
  if (!Recovered)
    Recovered = tryTwoTableSelect(Img, Rec, Info);

  // Strategy 1d: two-level index-byte table (`jmptab[idxtab[switchvar]]`, the
  // classic MSVC sparse-switch lowering).  Must precede the generic
  // relative/cross-instruction resolvers, which would otherwise recover only
  // the inner address table (jmptab) and dispatch on the intermediate table
  // index instead of the real switch variable — collapsing the case set and
  // losing the true labels.  It composes the per-case targets into
  // ExplicitTargets, so it short-circuits the single-base machinery below.
  if (!Recovered)
    Recovered = tryTwoLevelIndexTable(Img, Rec, Info);

  // Strategy 2: PIC-relative table (architecture-neutral; common on x64).
  if (!Recovered)
    Recovered = tryRelativeTable(Img, Rec, Info);

  // Strategy 2b: PIC-relative table whose base register is materialised in
  // a preceding instruction (x86 `lea table(%rip),%reg` / ARM32 ADR).  The
  // per-record slice above cannot see the base, so fold it across
  // instructions by emulating the dominating prefix.
  if (!Recovered)
    Recovered = tryCrossInstrRelativeTable(Img, Rec, Info);

  // Strategy 2c: constant-base absolute table whose load is decoupled from the
  // branch by an -O0 spill/reload relay (`... mov tab(,idx,W),%r; mov
  // %r,[slot];
  // ... mov [slot],%r; jmp *%r`), including a shared multi-site computed-goto
  // dispatch where several goto-site predecessors feed one common table.  The
  // cross-instruction resolver above only reaches a load in the branch's own
  // block or a single-predecessor path, so a many-predecessor shared dispatch
  // reaches none; this recovers the table from the code-pointer relocation run
  // at its constant base regardless of how many goto sites share it.
  if (!Recovered)
    Recovered = tryConstBaseAbsoluteTable(Img, Rec, Info);

  // Strategy 3: execute the dispatch once with each register input left as the
  // one whole-word unknown.  A successful decomposition is exact: the loaded
  // address itself states the table base, entry width and index stride.  Keep
  // this behind the specialised forms above, whose multi-table and
  // architecture-specific layouts intentionally carry more metadata than one
  // linear expression can describe.
  if (!Recovered) {
    std::set<std::pair<uint64_t, uint16_t>> Candidates;
    for (const LowOp &Op : Rec.Ops)
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        if (Op.Inputs[I].isReg() && Op.Inputs[I].Size != 0)
          Candidates.emplace(Op.Inputs[I].Offset, Op.Inputs[I].Size);

    std::optional<symbolic::DispatchShape> Shape;
    uint64_t ShapeIndex = InvalidVA;
    bool Ambiguous = false;
    for (const auto &[Reg, Bytes] : Candidates) {
      symbolic::SymContext SymCtx;
      std::optional<symbolic::DispatchShape> Candidate =
          symbolic::analyzeDispatch(SymCtx, Rec.Ops, Reg, Bytes);
      if (!Candidate || Candidate->EntryStride == 0 ||
          Candidate->EntryScale > std::numeric_limits<uint32_t>::max())
        continue;
      if (Shape) {
        Ambiguous = true;
        break;
      }
      Shape = *Candidate;
      ShapeIndex = Reg;
    }

    if (Shape && !Ambiguous) {
      Info.setBaseAddr(Shape->TableBase);
      Info.EntrySize = Shape->EntrySize;
      Info.EntryStride = Shape->EntryStride;
      Info.IndexReg = ShapeIndex;
      Info.IsRelative = Shape->Kind == symbolic::DispatchKind::Relative;
      Info.IsSigned = Shape->EntryIsSigned;
      Info.TargetBase = Shape->RelativeBase;
      Info.EntryScale = static_cast<uint32_t>(Shape->EntryScale);
      Recovered = true;
      LLVM_DEBUG(llvm::dbgs() << "  symbolic-dispatch: table=0x"
                              << llvm::utohexstr(Info.BaseAddr)
                              << " entry=" << Info.EntrySize << " index=0x"
                              << llvm::utohexstr(Info.IndexReg) << "\n");
    }
  }

  // Strategy 4: Backward slicing for absolute tables.
  if (!Recovered && !sliceBackForTableBase(Rec, Info))
    return {};

  // A runtime-selected dispatch over two non-adjacent code-pointer tables
  // carries its complete target set explicitly (the union of both runs), which
  // no single-base contiguous read can reconstruct.  Use it verbatim: the
  // guard / normalization / stride / emulation machinery below all assume one
  // contiguous base and would corrupt the two-run layout.  The set is already
  // exact and validated (every entry a resolved in-function code pointer), so
  // the dispatch lowers directly to the merged two-table switch.
  if (!Info.ExplicitTargets.empty()) {
    std::vector<va_t> Targets = Info.ExplicitTargets;
    // Every entry was validated during the run read; a sanity-check truncation
    // would desync the concatenated positional labels, so require it to keep
    // the full set rather than emit a mis-aligned switch.
    if (!sanityCheckTargets(Img, Targets) ||
        Targets.size() != Info.ExplicitTargets.size() ||
        Targets.size() < limits::kMinJumpTableEntries)
      return {};
    ResolvedTableInfo[Rec.Addr] = Info;
    LLVM_DEBUG(llvm::dbgs()
               << "Jump table @ 0x" << llvm::utohexstr(Rec.Addr) << ": "
               << Targets.size() << " entries ("
               << (Info.TwoLevelIndex ? "two-level index-byte"
                                      : "runtime-selected two-table")
               << ", base=0x" << llvm::utohexstr(Info.BaseAddr) << ")\n");
    return Targets;
  }

  // Detect normalization (INT_SUB base, right-shift) so we can
  // pull back guard bounds and recover case labels later.  A reloc-absolute
  // computed-goto table is indexed directly by `tab[idx]` with idx in [0,N), so
  // the case values are the raw indices 0..N-1 — there is no case-label
  // normalization to invert.  Crucially, the shift in an index expression like
  // `(acc >> k) & 3` is part of *computing* the index, not a table
  // normalization, so running the detectors here would mis-read it as NormShift
  // and emit bogus `i << k` case values that no longer match the runtime index.
  if (!Info.RelocAbsolute) {
    detectNormalization(Rec, Info);

    // Detect stride from AND masks on the switch variable.  When
    // the index has known-zero low bits the effective table size is
    // guard_bound / stride.
    detectStride(Rec, Info);
  }

  // Refine entry count: first try CircleRange-based guard analysis for
  // precise modular-arithmetic bounds, then fall back to integer bounds.
  // A reloc-absolute computed-goto table already has its exact entry count from
  // the relocation run and carries no comparison guard, so the guard search is
  // skipped — it could only mis-bound it.
  bool GuardFound = false;
  if (!Info.RelocAbsolute) {
    GuardFound = refineRangeFromGuards(Rec, Info);
    if (!GuardFound)
      GuardFound = inferBoundsFromGuard(Rec, Info) ||
                   inferBoundsFromCFGGuards(Rec, Info) ||
                   inferBoundsFromUnrolledGuard(Rec, Info);
    // Last resort: a guard written against a multi-step normalization of the
    // index (`t = (idx+k) & m; cmp t,N`) that the direct comparison matchers
    // cannot copy-chain back to the index register.  Propagate the guard's
    // value range backward through the count-preserving reshapes onto the
    // index; the size that lands there is the entry count.
    if (!GuardFound)
      GuardFound = inferBoundsFromRangePullback(Rec, Info);
    // Final guard strategy: the guard constrains a *separate reload* of the
    // same spilled switch variable that feeds the index (the -O0 shape where
    // `cmp` and the table index each reload the value from the same stack slot,
    // with no copy chain linking their registers).  Match by exact location
    // equivalence rather than register identity.
    if (!GuardFound)
      GuardFound = inferBoundsFromLoadAliasGuard(Rec, Info);
  }

  // A PIC relative table with no comparison guard at all is a `switch(x % N)`
  // table: the modulus bounds the index, so there is no `cmp` to find.  When
  // the guard search came up empty, trust the relocation run starting at the
  // table base — every entry carries a PC-relative code relocation, so the run
  // length is the exact entry count.  This also discards any spurious
  // normalization the magic-number modulo sequence triggered (its `sub` looks
  // like a NormBase), since a modulo index is the raw value in [0, N).  A
  // genuinely guarded table (signed/normalized switch) keeps its guard-derived
  // bound untouched.
  if (!GuardFound && !Info.RelocAbsolute && Info.IsRelative &&
      Info.HasBaseAddr && Info.EntrySize > 0) {
    uint32_t RelRun = countRelCodeRelocRun(Img, Info.BaseAddr, Info.EntrySize);
    // A second unguarded PIC table placed immediately after this one continues
    // the same RelCodeReloc run, so the raw count over-reads into it; cap the
    // run at the next table's base anchor (its exact end).
    RelRun =
        boundRelRunByNextAnchor(Img, Info.BaseAddr, Info.EntrySize, RelRun);
    if (RelRun >= limits::kMinJumpTableEntries) {
      Info.MaxEntries = RelRun;
      Info.RelocBounded = true;
      Info.NormBase = 0;
      Info.NormShift = 0;
      Info.Stride = 1;
    }
  }

  // A run of absolute code-pointer relocations at the table base proves the
  // entries are absolute code pointers, overriding the backward slice's
  // width-based relative guess.  The slice marks any sub-pointer-width load
  // relative, so an i386 4-byte absolute table (`jmpl *tab(,idx,4)` with
  // R_386_32 entries) would otherwise be decoded as PC-relative offsets and
  // dropped.  This classification is independent of how the table is bounded
  // (a `switch(x & mask)` still has an `and`-derived guard), so it must run
  // regardless of the guard search — decode correctness and entry count are
  // separate concerns.
  bool AbsCodePtrRun =
      !Info.RelocAbsolute && !Info.TwoTableSelect && Info.TargetBase == 0 &&
      Info.HasBaseAddr && Info.EntrySize > 0 &&
      countCodePtrRelocRun(Img, Info.BaseAddr, Info.EntrySize) >=
          limits::kMinJumpTableEntries;
  if (AbsCodePtrRun && Info.IsRelative) {
    Info.IsRelative = false;
    Info.IsSigned = false;
  }

  // An *absolute* computed jump table bounded only by a mask carries no `cmp`
  // range guard — the mask alone confines the index — so the comparison-guard
  // search above found nothing.  Its code-pointer relocation run gives the
  // exact physical entry count, the absolute analogue of the PC-relative
  // RelCodeReloc run above.  This is what turns an unguarded `switch(x & mask)`
  // lowered non-PIC back into a full switch instead of dropping every target.
  // The run length already reflects the real index range (raw masked value,
  // filler slots included), so it supersedes any stride the mask implied — a
  // later `/ Stride` division would wrongly shrink it.
  if (!GuardFound && Info.MaxEntries == 0 && !Info.RelocBounded &&
      AbsCodePtrRun) {
    Info.MaxEntries = countCodePtrRelocRun(Img, Info.BaseAddr, Info.EntrySize);
    Info.RelocBounded = true;
    Info.NormBase = 0;
    Info.NormShift = 0;
    Info.Stride = 1;
    LLVM_DEBUG(llvm::dbgs() << "  abs-reloc-run: bounded absolute table to "
                            << Info.MaxEntries
                            << " entries from code-pointer relocation run\n");
  }

  // A `switch(x % N)` table whose entries carry no relocations (AArch64 compact
  // byte/halfword tables, ARM32 inline `.text` word tables) cannot use the
  // relocation run above and has no `cmp` range guard.  Read the modulus N out
  // of the magic-division remainder that computes the index, which bounds the
  // table exactly and keeps the single-target readonly fallback below (which
  // only fires at MaxEntries == 0) from collapsing it to one entry.
  if (!GuardFound && Info.MaxEntries == 0 && !Info.RelocAbsolute &&
      Info.IsRelative && Info.HasBaseAddr && Info.EntrySize > 0)
    inferBoundsFromModulo(Img, Rec, Info);

  // COND_BR-polarity: `cmp idx,N; ja default` (strict above) makes the table
  // cover [0,N] = N+1 entries, but the CF flag `idx < N` reports only N.  When
  // the guard also consumes the ZF equality `idx == N` it is the ja/jbe family,
  // so recover the inclusive last entry the range analysis dropped.
  if (Info.MaxEntries > 0 && Info.IndexReg != InvalidVA && Info.NormBase == 0 &&
      Info.NormShift == 0 && Info.Stride <= 1 &&
      Info.MaxEntries < limits::kMaxJumpTableEntries &&
      guardUsesInclusiveCompare(Rec, Info.IndexReg, Info.MaxEntries))
    Info.MaxEntries += 1;

  // If a normalization offset is present and the guard bound looks
  // like it was applied to the original (pre-normalization) variable,
  // adjust it down to reflect the actual table size.
  if (Info.MaxEntries > 0 && Info.NormBase > 0) {
    uint32_t Adj = pullBackBound(Info.MaxEntries, Info);
    if (Adj != Info.MaxEntries && Adj >= limits::kMinJumpTableEntries) {
      LLVM_DEBUG(llvm::dbgs()
                 << "  pullback: adjusted bound " << Info.MaxEntries << " -> "
                 << Adj << " (normBase=" << Info.NormBase << ")\n");
      Info.MaxEntries = Adj;
    }
  }

  // Apply stride: when the switch variable has alignment S, a guard
  // bound of N means at most N/S distinct table entries.  A relocation-bounded
  // table already holds the exact entry count (not a raw-variable guard bound),
  // and a pre-scaled computed goto encodes its byte stride here purely for case
  // labels, so the division must not shrink it.
  if (Info.MaxEntries > 0 && Info.Stride > 1 && !Info.RelocBounded) {
    uint32_t Adj = Info.MaxEntries / Info.Stride;
    if (Adj >= limits::kMinJumpTableEntries) {
      LLVM_DEBUG(llvm::dbgs()
                 << "  stride: adjusted bound " << Info.MaxEntries << " -> "
                 << Adj << " (stride=" << Info.Stride << ")\n");
      Info.MaxEntries = Adj;
    }
  }

  // A power-of-two-modulo / masked index (`and $(2^k-1)`, with an optional
  // following `dec` from a peeled iteration) is hard-bounded by the mask, for
  // every table kind (PIC-relative, GOTOFF, absolute).  Two such tables placed
  // back-to-back in rodata form one continuous relocation run / pointer run, so
  // an over-long read runs past the first table into the second — fabricating
  // bogus successor edges (and, with x87 residents, an unbalanced stack the TOP
  // recovery cannot reconcile).  The mask is a hard upper bound on the index,
  // so clamp to it even when a range guard was found: a guard derived from the
  // pre-`dec` mask (`and $7; dec` => index in [-1,6], 7 entries) over-counts by
  // the offset, and min(guard, mask) is always the safe table size.
  // A two-table merge holds 2N entries while the per-table index mask bounds
  // the index to N; the runtime base select supplies the doubling, so the mask
  // must not clamp the merged count.
  if (uint32_t MaskBound = inferBoundsFromMask(Rec, Info);
      !Info.TwoTableSelect && MaskBound > 0 &&
      (Info.MaxEntries == 0 || MaskBound < Info.MaxEntries))
    Info.MaxEntries = MaskBound;

  // Last-resort bound for a `switch(x & M)` table with a *non-contiguous* mask
  // (e.g. `x & 0x1e`) that no other strategy bounded — the shape a fully-linked
  // binary produces, where the absolute entries carry no relocation run to
  // count.  The masked value indexes the table directly, so the table is dense
  // over the raw index (0..coveringmask) with default filler in the unused
  // slots; the covering-mask count is that physical entry count.  Because the
  // raw masked value is the slot index, its trailing-zero "stride" is an
  // artifact of the gaps, not a divisor — force stride 1 so the later `/Stride`
  // shrink does not drop the filler slots and desync the dispatch.  Opt-in
  // (AllowNonContiguous) and gated on no prior bound so it never loosens a
  // table another strategy already sized.
  if (Info.MaxEntries == 0 && !Info.TwoTableSelect && Info.NormShift == 0 &&
      Info.NormBase == 0) {
    if (uint32_t NCMask = inferBoundsFromMask(Rec, Info,
                                              /*AllowNonContiguous=*/true)) {
      Info.MaxEntries = NCMask;
      Info.Stride = 1;
      Info.RelocBounded = true;
      LLVM_DEBUG(llvm::dbgs() << "  covering-mask: bounded table to " << NCMask
                              << " entries from non-contiguous index mask\n");
    }
  }

  if (Info.MaxEntries == 0 || Info.MaxEntries > limits::kMaxJumpTableEntries)
    Info.MaxEntries = 0;

  // Readonly single-value optimisation: when the switch variable is
  // loaded from a readonly segment and no guard constrains it, try
  // reading the value directly from the image to produce a single
  // definite target.  This handles semi-dynamic dispatch vectors
  // whose initial value is baked into the binary.
  //
  // A dispatch vector baked into the binary lives in read-only DATA
  // (.rodata / .data.rel.ro); an *executable* base is instead an inline
  // PC-relative code table (the ARM32 `add rB,pc,#k; ldr rE,[rB,idx,4];
  // add pc,rB,rE` form embedded in .text), which is always a multi-entry
  // table.  Clamping such a table to a single entry here would flip the
  // unbounded "scan until an entry stops decoding to a valid in-function
  // target" read below into a bounded 1-entry read, dropping every other
  // arm and degrading the dispatch to a broken indirect tail call — so the
  // single-value optimisation is restricted to non-executable segments.
  if (Info.MaxEntries == 0 && Info.HasBaseAddr) {
    const auto *BaseSeg = Img.getSegmentFor(Info.BaseAddr);
    if (BaseSeg && !BaseSeg->isWritable() &&
        !Img.isCodeAddress(Info.BaseAddr) && !BaseSeg->Data.empty()) {
      size_t Off = static_cast<size_t>(Info.BaseAddr - BaseSeg->VA);
      if (rangeInBounds(Off, Info.EntrySize, BaseSeg->Data.size())) {
        va_t SingleTarget = decodeTableEntry(
            BaseSeg->Data.data() + Off, Info.EntrySize, Info.IsRelative,
            Info.IsSigned, Info.BaseAddr, Info.TargetBase, Info.EntryScale);
        if (isValidTarget(Img, SingleTarget, CurrentFuncEntry)) {
          Info.MaxEntries = 1;
          LLVM_DEBUG(llvm::dbgs() << "  readonly: single target 0x"
                                  << llvm::utohexstr(SingleTarget)
                                  << " from readonly segment\n");
        }
      }
    }
  }

  std::vector<uint32_t> KeptIdx;
  auto Targets = readTableEntries(Img, Info, &KeptIdx);

  // Post-read sanity check with truncation.
  sanityCheckTargets(Img, Targets);
  if (KeptIdx.size() > Targets.size())
    KeptIdx.resize(Targets.size()); // sanity-check truncates trailing entries

  // Dual-path recovery: when the primary guard analysis fails and we
  // have a base address, check for a default-value path pattern where
  // a COND_BR sends one path to a constant (default) and another to the
  // switch computation (default-value path with an explicit COND_BR split).
  if (Targets.size() < limits::kMinJumpTableEntries && Info.MaxEntries == 0 &&
      Info.HasBaseAddr) {
    if (tryDualPathRecovery(Rec, Info)) {
      auto AltTargets = readTableEntries(Img, Info);
      sanityCheckTargets(Img, AltTargets);
      if (AltTargets.size() > Targets.size()) {
        Targets = std::move(AltTargets);
        KeptIdx.clear(); // dense fallback: positional labels apply
        LLVM_DEBUG(llvm::dbgs() << "  dual-path: recovered " << Targets.size()
                                << " entries via default-value path\n");
      }
    }
  }

  // Multi-stage fallback: if we got a base but the initial read produced
  // too few entries (no guard bound found), retry with relaxed parameters.
  if (Targets.size() < limits::kMinJumpTableEntries && Info.MaxEntries == 0 &&
      Info.HasBaseAddr) {
    for (uint16_t AltSize : {uint16_t(4), uint16_t(8), uint16_t(2)}) {
      if (AltSize == Info.EntrySize)
        continue;
      JumpTableInfo Alt = Info;
      Alt.EntrySize = AltSize;
      Alt.IsRelative = (AltSize < 8);
      Alt.IsSigned = (AltSize < 8);
      auto AltTargets = readTableEntries(Img, Alt);
      sanityCheckTargets(Img, AltTargets);
      if (AltTargets.size() > Targets.size()) {
        Targets = std::move(AltTargets);
        Info = Alt;
        KeptIdx.clear(); // dense fallback: positional labels apply
        LLVM_DEBUG(llvm::dbgs()
                   << "  fallback: retried with entrySize=" << AltSize
                   << ", got " << Targets.size() << " entries\n");
      }
    }
  }

  // Emulation-based fallback: when all static strategies fail, try
  // running the ops through the NdOp emulator for each candidate index.
  if (Targets.size() < limits::kMinJumpTableEntries && Info.MaxEntries > 0 &&
      CurrentImg) {
    auto EmuTargets = tryEmulatedResolution(Img, Rec, Info);
    if (EmuTargets.size() > Targets.size()) {
      Targets = std::move(EmuTargets);
      KeptIdx.clear(); // dense fallback: positional labels apply
      LLVM_DEBUG(llvm::dbgs() << "  emulated: recovered " << Targets.size()
                              << " entries via NdOp emulation\n");
    }
  }

  // Emulation cross-check for a bounded table that decoded *fewer* targets than
  // its known entry count.  The static reader classifies the entry layout
  // (relative/absolute, sign, scale, target-base) before decoding, and a
  // misclassification can truncate an otherwise-valid table part-way — leaving
  // a plausible-but-incomplete target list that the `< kMin` fallback above
  // (which fires only on near-total failure) never revisits.  Re-run the
  // *actual* dispatch arithmetic through the emulator, which reads the real
  // base+index+load and so cannot mis-guess the layout, and adopt its result
  // only when it is strictly more complete AND reproduces the static decode on
  // every shared index.  That prefix-agreement gate makes this monotonic: it
  // can only append cases the static read dropped, never rewrite one it already
  // decoded, so a correctly recovered table is left untouched.  Restricted to a
  // dense static result (no sparse skips) so the two index coordinates align,
  // and bounded by the same MaxEntries so it can never over-read past the
  // guard/reloc bound.
  if (CurrentImg && Info.MaxEntries > 0 &&
      Targets.size() >= limits::kMinJumpTableEntries &&
      Targets.size() < Info.MaxEntries) {
    bool DenseStatic = KeptIdx.size() == Targets.size();
    for (size_t I = 0; DenseStatic && I < KeptIdx.size(); ++I)
      DenseStatic = KeptIdx[I] == I;
    if (DenseStatic) {
      auto EmuTargets = tryEmulatedResolution(Img, Rec, Info);
      bool ExtendsStatic = EmuTargets.size() > Targets.size();
      for (size_t I = 0; ExtendsStatic && I < Targets.size(); ++I)
        ExtendsStatic = EmuTargets[I] == Targets[I];
      if (ExtendsStatic) {
        LLVM_DEBUG(llvm::dbgs() << "  emu-verify: extended bounded table from "
                                << Targets.size() << " to " << EmuTargets.size()
                                << " entries via dispatch emulation\n");
        Targets = std::move(EmuTargets);
        KeptIdx.clear(); // dense positional labels apply
      }
    }
  }

  // Emulation-based recovery for an unbounded table whose static *entry layout*
  // the decoder misclassified.  The recurring resolver failure is not a missing
  // base but a mis-guessed entry format (relative vs absolute, sign, scale,
  // target-base) that makes readTableEntries decode garbage and drop the
  // dispatch to a degenerate indirect tail call.  When no bound was found, the
  // table base lies in read-only memory, and too few targets were read, emulate
  // the *actual* dispatch arithmetic along the branch path rather than
  // re-deriving the layout: emulation runs the real base+index+load, so a
  // mis-guessed format cannot lose the table.  This is as sound as
  // readTableEntries' own unbounded read — same read-only gate, stop-on-invalid
  // and duplicate-run break, plus a distinct-target requirement — and is
  // monotonic: it only fires where the branch would otherwise degrade to a tail
  // call, so it can never shrink an already-recovered table.
  if (Targets.size() < limits::kMinJumpTableEntries && Info.MaxEntries == 0 &&
      Info.HasBaseAddr && CurrentImg) {
    const auto *BaseSeg = Img.getSegmentFor(Info.BaseAddr);
    if (BaseSeg && !BaseSeg->isWritable() && !BaseSeg->Data.empty()) {
      auto EmuTargets =
          tryEmulatedResolution(Img, Rec, Info, /*SelfBounding=*/true);
      if (EmuTargets.size() > Targets.size()) {
        Targets = std::move(EmuTargets);
        KeptIdx.clear(); // dense fallback: positional labels apply
        LLVM_DEBUG(llvm::dbgs()
                   << "  emulated-unbounded: recovered " << Targets.size()
                   << " entries via read-only table emulation\n");
      }
    }
  }

  // Ground-truth cross-check for a plain relative/absolute table: rebuild the
  // targets by executing the *actual* dispatch arithmetic per index instead of
  // trusting the statically classified entry layout (relative-vs-absolute,
  // signedness).  A misclassified layout decodes a full-length but *wrong*
  // target set that still passes the sanity check (every entry lands in the
  // function), a silent miscompile the extend/fallback strategies above never
  // revisit because the count already looks complete.  The emulation reads the
  // same table bytes and applies the same transform the processor would, so
  // when it is fully grounded — every index read the recovered table slot
  // (BaseAddr + i*EntryStride) and produced a valid target — its result is
  // authoritative and supersedes a disagreeing static decode.
  //
  // Guarded to be a no-op wherever the static decode is already trustworthy, so
  // currently-recovered tables keep byte-identical targets: skipped for
  // reloc-bounded / two-table / compact (TargetBase) / pre-scaled tables (whose
  // layout is confirmed by relocations or a dedicated detector), for sparse
  // (gapped) decodes whose positional index would not line up with the emulated
  // slot, and adopted only when the emulation agrees on entry count yet differs
  // on some value.
  bool DenseStatic = KeptIdx.empty() || KeptIdx.size() == Targets.size();
  for (size_t I = 0; DenseStatic && I < KeptIdx.size(); ++I)
    DenseStatic = KeptIdx[I] == I;
  if (CurrentImg && DenseStatic && !Targets.empty() &&
      Info.IndexReg != InvalidVA && Info.HasBaseAddr && Info.TargetBase == 0 &&
      Info.EntryScale == 1 && !Info.PreScaledIndex && !Info.TwoTableSelect &&
      !Info.RelocAbsolute && !Info.RelocBounded &&
      (Info.EntrySize == 1 || Info.EntrySize == 2 || Info.EntrySize == 4 ||
       Info.EntrySize == 8)) {
    bool Grounded = false;
    auto EmuTargets = emulateGroundedTargets(
        Img, Rec, Info, static_cast<uint32_t>(Targets.size()), Grounded);
    if (Grounded && EmuTargets.size() == Targets.size() &&
        EmuTargets != Targets) {
      std::vector<va_t> Check = EmuTargets;
      if (sanityCheckTargets(Img, Check) && Check.size() == EmuTargets.size()) {
        LLVM_DEBUG(llvm::dbgs()
                   << "  emu-ground: corrected " << Targets.size()
                   << " statically-misclassified targets via grounded dispatch "
                      "emulation\n");
        Targets = std::move(EmuTargets);
        KeptIdx.clear(); // dense positional labels apply
      }
    }
  }

  if (Targets.size() < limits::kMinJumpTableEntries)
    return {};

  // Carry the kept slot indices so recoverCaseLabels assigns case values by the
  // real table index (a bounded sparse table skips don't-care slots).  Only
  // useful when the kept indices are *not* the trivial 0..N-1 (a gap exists);
  // an empty vector leaves the positional labelling unchanged.
  {
    bool HasGap = KeptIdx.size() != Targets.size();
    for (size_t I = 0; !HasGap && I < KeptIdx.size(); ++I)
      HasGap = KeptIdx[I] != I;
    Info.EntryIndices = HasGap ? std::move(KeptIdx) : std::vector<uint32_t>{};
  }
  ResolvedTableInfo[Rec.Addr] = Info;

  LLVM_DEBUG({
    llvm::dbgs() << "Jump table @ 0x" << llvm::utohexstr(Rec.Addr) << ": "
                 << Targets.size() << " entries, base=0x"
                 << llvm::utohexstr(Info.BaseAddr)
                 << ", entrySize=" << Info.EntrySize
                 << (Info.IsRelative ? " (relative" : " (absolute")
                 << (Info.IsSigned ? ", signed)" : ")") << "\n";
  });

  return Targets;
}

} // namespace neverd
