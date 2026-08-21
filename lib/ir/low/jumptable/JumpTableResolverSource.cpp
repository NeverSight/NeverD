//===- JumpTableResolverSource.cpp - Table base-address detection ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Table base-address ("source") detection strategies for jump-table
/// resolution: recover where a table's entries live in memory when the base is
/// materialized across instructions, including PIC/relative bases set in a
/// prior instruction and pre-scaled computed-goto loads.  Stack materialization
/// lives in JumpTableResolverStack.cpp; composite table layouts live in
/// JumpTableResolverShapes.cpp.
///
/// Part of the CFGBuilder jump-table resolver; see JumpTableResolver.cpp for
/// the top-level dispatch and JumpTableResolverDetail.h for the shared
/// backward-slicing helpers.
///
//===----------------------------------------------------------------------===//

#include "JumpTableResolverDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CFGBuilder.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace neverd {

//===----------------------------------------------------------------------===//
// detectUnscaledRelocTableLoad — pre-scaled computed-goto source
//===----------------------------------------------------------------------===//

// A pre-scaled computed-goto index is `(value >> s) & M`, where the mask M is a
// contiguous bit run shifted left by k (`(2^m - 1) << k`) and 2^k equals the
// pointer-width entry size: the `& M` confines the result to the byte offsets
// {0, size, 2*size, ...} of an `size`-byte entry table.  Returns true when
// `V` (traced through COPY) is defined by exactly that mask op.
static bool isPreScaledIndex(const std::vector<LowOp> &Ops, int FromIdx,
                             NdVar V, uint16_t EntrySize) {
  for (int Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
    if (!V.isReg() && !V.isTemp())
      return false;
    int D = reachingDefIdx(Ops, FromIdx, V);
    if (D < 0)
      return false;
    const LowOp &Op = Ops[D];
    // The masked index is routinely zero-extended to pointer width for the load
    // address (x86-64 `and esi,0x38; <zext esi>`), so follow value-preserving
    // width changes as well as plain copies.
    if ((Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT ||
         Op.Opcode == NdOp::INT_SEXT) &&
        Op.NumInputs >= 1) {
      V = Op.Inputs[0];
      FromIdx = D - 1;
      continue;
    }
    if (Op.Opcode == NdOp::SUBBYTES && Op.NumInputs >= 2 &&
        Op.Inputs[1].isConst() && Op.Inputs[1].Offset == 0) {
      V = Op.Inputs[0];
      FromIdx = D - 1;
      continue;
    }
    if (Op.Opcode == NdOp::INT_AND && Op.NumInputs >= 2) {
      // The mask may be either operand (AND is commutative) and may be
      // materialised in a register — ARM `and rd, rMask, rVal,lsl#s` cannot
      // encode an immediate alongside the shifted value — so resolve each
      // operand to a constant directly or through a COPY chain.
      auto constOf = [&](NdVar X) -> std::optional<uint64_t> {
        for (int G = 0, F = D - 1; G < limits::kMaxQuasiCopyDepth; ++G) {
          if (X.isConst())
            return X.Offset;
          if (!X.isReg() && !X.isTemp())
            return std::nullopt;
          int DD = reachingDefIdx(Ops, F, X);
          if (DD < 0 || Ops[DD].Opcode != NdOp::COPY || Ops[DD].NumInputs < 1)
            return std::nullopt;
          X = Ops[DD].Inputs[0];
          F = DD - 1;
        }
        return std::nullopt;
      };
      for (int W = 0; W < 2; ++W) {
        auto MOpt = constOf(Op.Inputs[W]);
        if (!MOpt || *MOpt == 0)
          continue;
        uint64_t M = *MOpt;
        uint32_t K = 0;
        for (uint64_t T = M; (T & 1) == 0; T >>= 1)
          ++K;
        uint64_t Low = M >> K; // contiguous bit run if (Low & (Low+1)) == 0
        if ((Low & (Low + 1)) == 0 && (1ull << K) == EntrySize)
          return true;
      }
      return false;
    }
    return false;
  }
  return false;
}

bool CFGBuilder::detectUnscaledRelocTableLoad(
    const BinaryImage &Img, const InsnRecord &Rec, uint64_t &BaseReg,
    uint64_t &IndexReg, uint16_t &LoadWidth, uint64_t &Disp,
    va_t &TableAddr) const {
  // Flatten the whole function prefix up to the dispatch so the pre-scaling
  // mask (often hoisted into a register in the loop preheader) is in scope.
  std::vector<LowOp> Ops;
  for (auto It = Insns.lower_bound(CurrentFuncEntry);
       It != Insns.end() && It->first <= Rec.Addr; ++It)
    for (auto &Op : It->second.Ops)
      Ops.push_back(Op);

  for (int I = static_cast<int>(Ops.size()) - 1; I >= 0; --I) {
    const LowOp &L = Ops[I];
    if (L.Opcode != NdOp::LOAD || L.NumInputs < 1)
      continue;
    // A computed-goto label table holds pointer-width absolute targets.
    uint16_t W = L.Output.Size;
    if (W != 4 && W != 8)
      continue;
    const NdVar &AddrV = (L.NumInputs >= 2) ? L.Inputs[1] : L.Inputs[0];
    if (!AddrV.isReg() && !AddrV.isTemp())
      continue;
    // Resolve the load address (through COPY) to its defining INT_ADD.
    int AddIdx = reachingDefIdx(Ops, I - 1, AddrV);
    for (int G = 0;
         AddIdx >= 0 && Ops[AddIdx].Opcode == NdOp::COPY &&
         Ops[AddIdx].NumInputs >= 1 &&
         (Ops[AddIdx].Inputs[0].isReg() || Ops[AddIdx].Inputs[0].isTemp()) &&
         G < limits::kMaxQuasiCopyDepth;
         ++G)
      AddIdx = reachingDefIdx(Ops, AddIdx - 1, Ops[AddIdx].Inputs[0]);
    if (AddIdx < 0 || Ops[AddIdx].Opcode != NdOp::INT_ADD ||
        Ops[AddIdx].NumInputs < 2)
      continue;

    // Two address shapes carry a pre-scaled index:
    //   * plain (x86-64): INT_ADD(table_base_reg, index)
    //   * GOTOFF (i386):  INT_ADD(INT_ADD(got_base_reg, index), disp); the
    //     loader baked the table VA into `disp`, so the table sits there.
    int InnerIdx = AddIdx;
    uint64_t LocalDisp = 0;
    for (int Which = 0; Which < 2; ++Which) {
      if (!Ops[AddIdx].Inputs[Which].isConst())
        continue;
      LocalDisp = Ops[AddIdx].Inputs[Which].Offset;
      int Inner =
          reachingDefIdx(Ops, AddIdx - 1, Ops[AddIdx].Inputs[1 - Which]);
      for (int G = 0;
           Inner >= 0 && Ops[Inner].Opcode == NdOp::COPY &&
           Ops[Inner].NumInputs >= 1 &&
           (Ops[Inner].Inputs[0].isReg() || Ops[Inner].Inputs[0].isTemp()) &&
           G < limits::kMaxQuasiCopyDepth;
           ++G)
        Inner = reachingDefIdx(Ops, Inner - 1, Ops[Inner].Inputs[0]);
      InnerIdx = Inner;
      break;
    }
    if (InnerIdx < 0 || Ops[InnerIdx].Opcode != NdOp::INT_ADD ||
        Ops[InnerIdx].NumInputs < 2 || Ops[InnerIdx].Inputs[0].isConst() ||
        Ops[InnerIdx].Inputs[1].isConst())
      continue;

    va_t LoadAddr = L.Addr;
    const LowOp &Sum = Ops[InnerIdx];
    for (int Side = 0; Side < 2; ++Side) {
      // The pre-scaling mask, whose stride equals the entry size, identifies
      // the index unambiguously; the other operand is the table / GOT base.
      if (!isPreScaledIndex(Ops, InnerIdx - 1, Sum.Inputs[Side], W))
        continue;
      uint64_t Idx = traceToRegister(Ops, InnerIdx - 1, Sum.Inputs[Side]);
      uint64_t Base = traceToRegister(Ops, InnerIdx - 1, Sum.Inputs[1 - Side]);
      if (Idx == InvalidVA || Base == InvalidVA)
        continue;
      // Confirm a code-pointer relocation run at the table VA: GOTOFF tables
      // sit at `disp`; plain tables at the folded base register.  This is the
      // verifiable label-table signature no ordinary load shares.
      va_t Table = LocalDisp;
      if (LocalDisp == 0) {
        auto BaseAddrOpt = foldRegConstant(Img, Rec, Base, LoadAddr);
        if (!BaseAddrOpt || *BaseAddrOpt == 0)
          continue;
        Table = *BaseAddrOpt;
      }
      if (countCodePtrRelocRun(Img, Table, W) < limits::kMinJumpTableEntries)
        continue;
      BaseReg = Base;
      IndexReg = Idx;
      LoadWidth = W;
      Disp = LocalDisp;
      TableAddr = Table;
      return true;
    }
  }
  return false;
}

//===----------------------------------------------------------------------===//
// tryCrossInstrRelativeTable — table base materialized in a prior instruction
//===----------------------------------------------------------------------===//

bool CFGBuilder::tryCrossInstrRelativeTable(const BinaryImage &Img,
                                            const InsnRecord &Rec,
                                            JumpTableInfo &Info) {
  // A memory-indirect jump (`jmp *(base,idx,8)`, the computed-goto / threaded
  // dispatch shape) lifts to a LOAD feeding an INDIR_BR whose input is a
  // *temp*; a register-indirect jump (`jmp *rax`) feeds a register.  The table
  // analysis below works off the LOAD either way, so only require that an
  // indirect branch with an input is present.
  bool HasIndBranch = false;
  for (auto &Op : Rec.Ops)
    if (Op.Opcode == NdOp::INDIR_BR && Op.NumInputs >= 1) {
      HasIndBranch = true;
      break;
    }
  if (!HasIndBranch)
    return false;

  // Flatten the INDIR_BR block ops; the table base may be a block live-in.
  va_t BlkStart = CurrentFuncEntry;
  auto BIt = BlockStarts.upper_bound(Rec.Addr);
  if (BIt != BlockStarts.begin()) {
    --BIt;
    BlkStart = *BIt;
  }
  std::vector<LowOp> BlockOps;
  for (auto It = Insns.lower_bound(BlkStart);
       It != Insns.end() && It->first <= Rec.Addr; ++It)
    for (auto &Op : It->second.Ops)
      BlockOps.push_back(Op);

  uint64_t BaseReg = InvalidVA;
  uint64_t IndexReg = InvalidVA;
  uint16_t LoadWidth = 0;
  bool HasScaledIndex = false;
  bool SawSext = false;
  bool Unscaled = false;
  bool CrossBlockReloc = false;
  int LoadIdx = -1;
  uint64_t Disp = 0;
  va_t CrossBlockAddVA = InvalidVA;

  for (int I = static_cast<int>(BlockOps.size()) - 1; I >= 0; --I) {
    auto &L = BlockOps[I];
    if (L.Opcode != NdOp::LOAD || L.NumInputs < 1)
      continue;
    uint16_t W = L.Output.Size;
    if (W != 1 && W != 2 && W != 4 && W != 8)
      continue;
    const NdVar &AddrV = (L.NumInputs >= 2) ? L.Inputs[1] : L.Inputs[0];
    if (!AddrV.isReg() && !AddrV.isTemp())
      continue;
    if (analyzeTableLoadAddr(BlockOps, I - 1, AddrV, BaseReg, IndexReg,
                             HasScaledIndex, Disp)) {
      LoadWidth = W;
      LoadIdx = I;
      for (int K = I + 1; K < static_cast<int>(BlockOps.size()); ++K)
        if (BlockOps[K].Opcode == NdOp::INT_SEXT)
          SawSext = true;
      break;
    }
  }

  // -O0 shared/decoupled dispatch (`&&label` computed goto): the indirect
  // branch reloads a spilled target (`ldr xT,[sp,#k]; br xT`) whose scaled
  // table load sits in a *predecessor* goto-site block
  // (`... ldr xT,[base,idx,scale]; str xT,[sp,#k]; b dispatch`).  The own-block
  // scan above sees only the frame reload, so widen the op window to the
  // single-predecessor path and rescan for the predecessor's scaled table load.
  // Acceptance is gated on an absolute code-pointer reloc run below, so a
  // register-indirect function-pointer tail call (no reloc table) is never
  // misread as a jump table.
  if (BaseReg == InvalidVA) {
    std::vector<LowOp> PathOps = collectPathOps(BlkStart, Rec.Addr);
    if (PathOps.size() > BlockOps.size()) {
      for (int I = static_cast<int>(PathOps.size()) - 1; I >= 0; --I) {
        auto &L = PathOps[I];
        if (L.Opcode != NdOp::LOAD || L.NumInputs < 1)
          continue;
        uint16_t W = L.Output.Size;
        if (W != 1 && W != 2 && W != 4 && W != 8)
          continue;
        const NdVar &AddrV = (L.NumInputs >= 2) ? L.Inputs[1] : L.Inputs[0];
        if (!AddrV.isReg() && !AddrV.isTemp())
          continue;
        if (analyzeTableLoadAddr(PathOps, I - 1, AddrV, BaseReg, IndexReg,
                                 HasScaledIndex, Disp, &CrossBlockAddVA)) {
          LoadWidth = W;
          LoadIdx = I;
          for (int K = I + 1; K < static_cast<int>(PathOps.size()); ++K)
            if (PathOps[K].Opcode == NdOp::INT_SEXT)
              SawSext = true;
          BlockOps = std::move(PathOps);
          CrossBlockReloc = true;
          break;
        }
      }
    }
  }

  // No scaled index found: try the pre-scaled (scale-1) computed-goto form,
  // where the size optimizer folded the entry-size scale into the index itself.
  va_t UnscaledTableAddr = 0;
  if (BaseReg == InvalidVA)
    Unscaled = detectUnscaledRelocTableLoad(Img, Rec, BaseReg, IndexReg,
                                            LoadWidth, Disp, UnscaledTableAddr);

  if (BaseReg == InvalidVA || LoadWidth == 0 || (!HasScaledIndex && !Unscaled))
    return false;

  // The index register at the table load (e.g. RCX in `movslq (rdx,rcx,4)`) is
  // frequently a fresh copy of the original switch variable (`mov ecx,edi`).
  // Trace it back to that source register so the guard `cmp edi,N` is matched
  // and unrelated flag masks (e.g. the parity `and x,1`) are not mistaken for
  // the table bound.  The pre-scaled form already resolved its terminal index
  // register, so skip these block-local refinements for it.
  if (!Unscaled && IndexReg != InvalidVA && LoadIdx > 0)
    IndexReg = traceRegSource(BlockOps, LoadIdx - 1, IndexReg);

  // At -O0 the guarded switch variable is spilled before the dispatch block and
  // reloaded into the index register (`str rX,[sp,#k]; ... ldr rIdx,[sp,#k]`),
  // so the `cmp`/`and` bound sits on `rX`, not the reloaded index.  Forward the
  // index through the stack slot to `rX` so the bound analysis connects — the
  // .text-embedded ARM PC-relative table carries no relocation run to bound it,
  // so without this it stays unbounded and the dispatch degrades to a tail
  // call.
  if (!Unscaled && IndexReg != InvalidVA && LoadIdx > 0)
    IndexReg =
        forwardIndexThroughStackSpill(BlockOps, LoadIdx, IndexReg, BlkStart);

  // The register added back to each loaded entry is the relative base for the
  // targets.  Three shapes occur:
  //
  //   * Plain PIC table (x64 `lea table(%rip),%rN; movslq (%rN,%idx,4),%r8;
  //     add %rN,%r8`): the register *is* the table, so Disp == 0 and the target
  //     is table_base + entry.  Fold the register to the table address.
  //
  //   * i386 PIC GOTOFF table (`mov disp(%ecx,%idx,4),%r; add %ecx,%r`): the
  //     register is the GOT base, which the loader treats as the image base 0,
  //     and the loader already baked that base into the resolved GOTOFF Disp.
  //     The GOT base is routinely spilled to a stack slot and reloaded, so a
  //     prefix-emulation fold of it is unreliable; use the image-base GOT (0)
  //     so the table sits at Disp and the target is 0 + entry.
  //
  //   * Pre-scaled computed goto: detectUnscaledRelocTableLoad already resolved
  //     the table VA (folded base, or GOTOFF disp) and confirmed its reloc run.
  uint64_t TableAddr;
  // A frame-register base (SP/FP) with a load displacement that is a *frame
  // offset* (not a data-segment VA) marks a stack-materialised local table: the
  // x86-64/i386 -O0 frame offset rides in the load displacement (`mov
  // (%rbp,%idx,8),-0x30`), so route it to the stack resolver (threading Disp in
  // as the frame offset).  But i386 -O2 PIC repurposes EBP as the GOT base with
  // a GOTOFF displacement that *is* a real data-segment table VA (`mov
  // 0x9c(%ebp,%idx,4),%r`) — that is a regular relative table, not a stack one
  // — so only divert a frame base when the displacement does not resolve to a
  // data segment.  (A genuine non-frame GOT-base table keeps GotOff
  // regardless.)
  const TargetRegInfo &TRIcg = getTargetRegInfo(Img.Arch);
  bool BaseIsFrame = BaseReg != InvalidVA && TRIcg.isFrameReg(BaseReg);
  bool DispIsDataVA = Disp != 0 && Img.getSegmentFor(Disp) != nullptr;
  bool GotOff = Disp != 0 && (!BaseIsFrame || DispIsDataVA);
  if (Unscaled) {
    TableAddr = UnscaledTableAddr;
  } else if (GotOff) {
    TableAddr = Disp;
  } else {
    // Fold the base register at the table load, not at the branch: the base may
    // be reused after the target is computed (`add %r11,%r10; mov %edx,%r11d;
    // jmp *%r10`), and emulating past that clobber would read the wrong value.
    // For a cross-block decoupled dispatch the base register is also clobbered
    // *before* the load (ARM `add rB,rB,idx,lsl#k; ldr [rB]`), so fold it at
    // the base+index add — before the index is folded in — to read the pure
    // base.
    va_t LoadAddr = (LoadIdx >= 0) ? BlockOps[LoadIdx].Addr : InvalidVA;
    va_t FoldAt = (CrossBlockReloc && CrossBlockAddVA != InvalidVA)
                      ? CrossBlockAddVA
                      : LoadAddr;
    // A local (non-`static`) computed-goto table is materialised on the stack
    // at -O0: the base register is a frame slot (SP/FP + const) into which the
    // read-only initializer run was copied.  Such a base folds to a bogus
    // low/stack address rather than a data segment, so try the stack-table
    // resolver first — it is gated on a frame-register base *and* a
    // code-pointer reloc run at the traced init source, returning InvalidVA for
    // any non-stack base, so a normal PIC/lea base falls through to the fold
    // below.
    bool StackTableMutated = false;
    va_t StackSrc = resolveStackMaterializedTableSource(
        Img, Rec, BlockOps, LoadIdx, BaseReg, LoadWidth,
        static_cast<int64_t>(Disp), &StackTableMutated);
    if (StackSrc != InvalidVA) {
      TableAddr = StackSrc;
      // A runtime-permuted stack table keeps its (static) targets so the
      // dispatch retains successors, but is marked so the emitter traps rather
      // than dispatching on the now-stale index->target map.
      Info.MutatedUnsafe = StackTableMutated;
    } else {
      std::optional<uint64_t> RelBaseOpt =
          foldRegConstant(Img, Rec, BaseReg, FoldAt);
      if (!RelBaseOpt || !Img.getSegmentFor(*RelBaseOpt))
        return false;
      TableAddr = *RelBaseOpt;
    }
  }

  const auto *Seg = Img.getSegmentFor(TableAddr);
  if (!Seg || Seg->Data.empty())
    return false;

  Info.setBaseAddr(TableAddr);
  Info.EntrySize = LoadWidth;
  Info.IndexReg = IndexReg;

  // Reloc-driven absolute table: a run of loader-applied absolute code-pointer
  // relocations starting at the base means the entries are absolute targets and
  // the run length is the exact entry count.  This recovers a computed-goto /
  // threaded dispatch (which has no comparison guard to bound it) and resolves
  // the 4-byte absolute-vs-PIC-relative ambiguity — a PIC switch table carries
  // no relocations on its entries.
  uint32_t RelocRun = countCodePtrRelocRun(Img, TableAddr, LoadWidth);
  // The pre-scaled form is only sound when a relocation run confirms the table;
  // without one there is no valid decoding for a bare base+index load.
  if (Unscaled && RelocRun < limits::kMinJumpTableEntries)
    return false;
  // A cross-block (decoupled-dispatch) table is trusted only when an absolute
  // code-pointer reloc run confirms it; otherwise the widened op window could
  // latch a coincidental scaled load on a non-jump-table indirect branch.
  if (CrossBlockReloc && RelocRun < limits::kMinJumpTableEntries)
    return false;
  if (RelocRun >= limits::kMinJumpTableEntries) {
    Info.IsRelative = false;
    Info.IsSigned = false;
    Info.MaxEntries = RelocRun;
    Info.RelocAbsolute = true;
    Info.RelocBounded = true;
    // A pre-scaled index already byte-scales the entry (`table + entry*size`),
    // so its case values are the byte offsets {0, size, 2*size, ...} rather
    // than 0..N-1; record the stride so recoverCaseLabels emits matching
    // labels.
    if (Unscaled) {
      Info.Stride = LoadWidth;
      Info.PreScaledIndex = true;
    }
  } else if (GotOff) {
    // GOTOFF entries are target - GOT_base; with GOT_base = 0 they equal the
    // absolute target addresses.
    Info.IsRelative = false;
    Info.IsSigned = false;
  } else {
    Info.IsRelative = (LoadWidth < limits::kMaxEntryBytes);
    Info.IsSigned = SawSext || (LoadWidth < limits::kMaxEntryBytes);
  }

  // AArch64 -O0 word tables add the signed entry to a separate `adr` anchor
  // rather than to the table base (`adr xA,.L; ldrsw xO,[tbl,idx,4]; add xA,xA,
  // xO; br xA`), so the target is anchor+entry, not table+entry.  Detect the
  // add feeding the branch whose other operand is a pure code-address constant
  // (an `adr`, traced through COPY only — never a LOAD, which is the
  // entry/offset) distinct from the (rodata) table, and use it as the target
  // base.  x64 PIC (added operand is the non-executable rodata table) and ARM32
  // (anchor *is* the table base) do not match, so they keep table-relative
  // targets.
  if (Info.IsRelative && Info.TargetBase == 0) {
    uint64_t BrReg = InvalidVA;
    for (auto &Op : Rec.Ops)
      if (Op.Opcode == NdOp::INDIR_BR && Op.NumInputs >= 1 &&
          Op.Inputs[0].isReg()) {
        BrReg = Op.Inputs[0].Offset;
        break;
      }
    int AddD = -1;
    if (BrReg != InvalidVA)
      for (int I = static_cast<int>(BlockOps.size()) - 1; I >= 0; --I)
        if (BlockOps[I].Opcode == NdOp::INT_ADD && BlockOps[I].Output.isReg() &&
            BlockOps[I].Output.Offset == BrReg && BlockOps[I].NumInputs >= 2) {
          AddD = I;
          break;
        }
    if (AddD >= 0) {
      va_t AddAddr = BlockOps[AddD].Addr;
      auto traceConstAnchor = [&](NdVar V,
                                  int From) -> std::optional<uint64_t> {
        for (int G = 0; G < limits::kMaxSliceDepth; ++G) {
          if (V.isConst())
            return V.Offset;
          if (!V.isReg() && !V.isTemp())
            return std::nullopt;
          int D = reachingDefIdx(BlockOps, From, V);
          if (D < 0) {
            if (V.isReg()) {
              auto F = foldRegConstant(Img, Rec, V.Offset, AddAddr);
              if (F && Img.getSegmentFor(*F))
                return *F;
            }
            return std::nullopt;
          }
          const LowOp &O = BlockOps[D];
          if (O.Opcode != NdOp::COPY || O.NumInputs < 1)
            return std::nullopt;
          if (O.Inputs[0].isConst())
            return O.Inputs[0].Offset;
          V = O.Inputs[0];
          From = D - 1;
        }
        return std::nullopt;
      };
      for (int W = 0; W < 2; ++W) {
        auto Anchor = traceConstAnchor(BlockOps[AddD].Inputs[W], AddD - 1);
        if (!Anchor || *Anchor == TableAddr)
          continue;
        if (Img.hasExecutableCodeOwnerAt(*Anchor)) {
          Info.TargetBase = *Anchor;
          Info.EntryScale = 1;
          break;
        }
      }
    }
  }
  return true;
}

} // namespace neverd
