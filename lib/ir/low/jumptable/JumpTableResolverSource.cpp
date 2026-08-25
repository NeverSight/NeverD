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

#include <algorithm>
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
    uint64_t &IndexReg, uint16_t &LoadWidth, uint64_t &Disp, va_t &TableAddr,
    NdVar *LoadOutput, va_t *LoadAddr, int *LoadSeq, NdVar *IndexValue,
    va_t *IndexUseAddr, int *IndexUseSeq) const {
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
    // Resolve the load address through value-preserving transports to its
    // defining INT_ADD.  The i386 lifter zero-extends a complete 32-bit guest
    // address to the host address width immediately before LOAD; that is an
    // address-container conversion, not a different provenance root.
    auto isAddressTransport = [&](const LowOp &Op) {
      if (Op.NumInputs < 1 || (!Op.Inputs[0].isReg() && !Op.Inputs[0].isTemp()))
        return false;
      if (Op.Opcode == NdOp::COPY)
        return true;
      return Op.Opcode == NdOp::INT_ZEXT &&
             Op.Inputs[0].Size == Img.getPointerSize() &&
             Op.Output.Size > Op.Inputs[0].Size;
    };
    int AddIdx = reachingDefIdx(Ops, I - 1, AddrV);
    for (int G = 0; AddIdx >= 0 && isAddressTransport(Ops[AddIdx]) &&
                    G < limits::kMaxQuasiCopyDepth;
         ++G) {
      AddIdx = reachingDefIdx(Ops, AddIdx - 1, Ops[AddIdx].Inputs[0]);
    }
    if (AddIdx < 0 || Ops[AddIdx].Opcode != NdOp::INT_ADD ||
        Ops[AddIdx].NumInputs < 2)
      continue;

    // Two address shapes carry a pre-scaled index:
    //   * plain (x86-64): INT_ADD(table_base_reg, index)
    //   * GOTOFF (i386):  INT_ADD(INT_ADD(got_base_reg, index), disp); the
    //     loader baked the table VA into `disp`, so the table sits there.
    int InnerIdx = AddIdx;
    int DispSide = -1;
    uint64_t LocalDisp = 0;
    for (int Which = 0; Which < 2; ++Which) {
      if (!Ops[AddIdx].Inputs[Which].isConst())
        continue;
      DispSide = Which;
      LocalDisp = Ops[AddIdx].Inputs[Which].Offset;
      int Inner =
          reachingDefIdx(Ops, AddIdx - 1, Ops[AddIdx].Inputs[1 - Which]);
      for (int G = 0; Inner >= 0 && isAddressTransport(Ops[Inner]) &&
                      G < limits::kMaxQuasiCopyDepth;
           ++G) {
        Inner = reachingDefIdx(Ops, Inner - 1, Ops[Inner].Inputs[0]);
      }
      InnerIdx = Inner;
      break;
    }
    if (InnerIdx < 0 || Ops[InnerIdx].Opcode != NdOp::INT_ADD ||
        Ops[InnerIdx].NumInputs < 2 || Ops[InnerIdx].Inputs[0].isConst() ||
        Ops[InnerIdx].Inputs[1].isConst())
      continue;

    va_t LoadInsnAddr = L.Addr;
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
      // On i386 ELF an outer displacement is not an address by numeric value.
      // Require both halves of the relocation contract used by the ordinary
      // scaled path: this exact LowIR input must name the instruction's unique
      // R_386_GOTOFF field, and this exact base input must reach the
      // lifter-authenticated GOT-base-zero model on every path.  A generic
      // R_386_32 field, raw scalar collision, or unrelated literal zero may
      // still point at a relocation run but cannot authorize GOTOFF arithmetic.
      const bool IsI386ELFDisplacement = DispSide >= 0 &&
                                         Img.Arch == Arch::X86 && Img.isELF() &&
                                         Img.getPointerSize() == 4;
      if (IsI386ELFDisplacement &&
          (!isExactI386GOTOFFInput(Ops[AddIdx], DispSide) ||
           !exactI386ModelZeroReaches(Sum, 1 - Side, LocalDisp)))
        continue;
      // Confirm a code-pointer relocation run at the table VA: GOTOFF tables
      // sit at `disp`; plain tables at the folded base register.  This is the
      // verifiable label-table signature no ordinary load shares.
      va_t Table = LocalDisp;
      if (LocalDisp == 0) {
        auto BaseAddrOpt = foldRegConstant(Img, Rec, Base, LoadInsnAddr);
        if (!BaseAddrOpt)
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
      if (LoadOutput)
        *LoadOutput = L.Output;
      if (LoadAddr)
        *LoadAddr = L.Addr;
      if (LoadSeq)
        *LoadSeq = L.Seq;
      if (IndexValue)
        *IndexValue = Sum.Inputs[Side];
      if (IndexUseAddr)
        *IndexUseAddr = Sum.Addr;
      if (IndexUseSeq)
        *IndexUseSeq = Sum.Seq;
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
  NdVar IndexValueAtUse;
  va_t IndexUseAddr = InvalidVA;
  int IndexUseSeq = -1;
  uint16_t LoadWidth = 0;
  bool HasScaledIndex = false;
  bool SawSext = false;
  bool Unscaled = false;
  bool CrossBlockReloc = false;
  int LoadIdx = -1;
  uint64_t Disp = 0;
  va_t CrossBlockAddVA = InvalidVA;
  JumpTableValueOccurrence AddressOccurrence;
  JumpTableFrameAddressUse FrameRuntimeBase;

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
                             HasScaledIndex, Disp, nullptr, &IndexValueAtUse,
                             &IndexUseAddr, &IndexUseSeq, &AddressOccurrence,
                             &FrameRuntimeBase)) {
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
                                 HasScaledIndex, Disp, &CrossBlockAddVA,
                                 &IndexValueAtUse, &IndexUseAddr, &IndexUseSeq,
                                 &AddressOccurrence, &FrameRuntimeBase)) {
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
  NdVar UnscaledLoadOutput;
  va_t UnscaledLoadAddr = InvalidVA;
  int UnscaledLoadSeq = -1;
  if (BaseReg == InvalidVA)
    Unscaled = detectUnscaledRelocTableLoad(
        Img, Rec, BaseReg, IndexReg, LoadWidth, Disp, UnscaledTableAddr,
        &UnscaledLoadOutput, &UnscaledLoadAddr, &UnscaledLoadSeq,
        &IndexValueAtUse, &IndexUseAddr, &IndexUseSeq);

  if (BaseReg == InvalidVA || LoadWidth == 0 || (!HasScaledIndex && !Unscaled))
    return false;

  // The index register at the table load (e.g. RCX in `movslq (rdx,rcx,4)`) is
  // frequently a fresh copy of the original switch variable (`mov ecx,edi`).
  // Trace it back to that source register so the guard `cmp edi,N` is matched
  // and unrelated flag masks (e.g. the parity `and x,1`) are not mistaken for
  // the table bound.  The pre-scaled form already resolved its terminal index
  // register, so skip these block-local refinements for it.
  if (!Unscaled && IndexReg != InvalidVA) {
    IndexReg = LoadIdx > 0 ? traceRegSource(BlockOps, LoadIdx - 1, IndexReg)
                           : IndexReg;
  }

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
  if (!BaseIsFrame) {
    // AArch64/ARM commonly materialise a local table base in a scratch
    // register (`add x8, sp, #off`) before combining it with the scaled index.
    // This preflight only decides whether the full all-path stack-table
    // resolver should run; it does not authorise a source or a target.  Start
    // from the exact input occurrence recorded by analyzeTableLoadAddr and
    // accept only a bounded chain of address-preserving operations that
    // reaches an architectural frame register.
    auto derivedFromFrameAtExactUse = [&]() {
      bool EvidenceComplete = true;
      auto chargeEvidence = [&](size_t Amount = 1) {
        if (EvidenceComplete && consumeStackTableEvidence(Amount))
          return true;
        EvidenceComplete = false;
        StackTableEvidenceIncompleteBranches.insert(Rec.Addr);
        return false;
      };
      auto orderedLookupWork = [](size_t Count) {
        size_t Work = 1;
        for (size_t N = Count; N > 1; N = N / 2 + N % 2)
          ++Work;
        return Work;
      };

      const JumpTableValueOccurrence &Use = FrameRuntimeBase.Use;
      if (Use.Value.Size == 0 || Use.Addr == InvalidVA || Use.Seq < 0 ||
          Use.DefinedAtPoint || (!Use.Value.isReg() && !Use.Value.isTemp()))
        return false;

      if (!chargeEvidence(orderedLookupWork(Insns.size())))
        return false;
      const auto UseInsn = Insns.find(Use.Addr);
      if (UseInsn == Insns.end())
        return false;
      const LowOp *UseOp = nullptr;
      for (const LowOp &Op : UseInsn->second.Ops) {
        if (!chargeEvidence())
          return false;
        if (Op.Addr != Use.Addr || Op.Seq != Use.Seq)
          continue;
        unsigned ExactInputs = 0;
        for (unsigned N = 0; N < Op.NumInputs; ++N) {
          if (!chargeEvidence())
            return false;
          ExactInputs += Op.Inputs[N] == Use.Value;
        }
        if (UseOp || ExactInputs != 1)
          return false;
        UseOp = &Op;
      }
      if (!UseOp)
        return false;

      NdVar Value = Use.Value;
      va_t BeforeAddr = UseOp->Addr;
      int BeforeSeq = UseOp->Seq;
      for (int Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
        if (!chargeEvidence())
          return false;
        if (Value.isReg() && TRIcg.isFrameReg(Value.Offset))
          return true;
        if (!Value.isReg() && !Value.isTemp())
          return false;

        if (!chargeEvidence(orderedLookupWork(Insns.size())))
          return false;
        const LowOp *Def = nullptr;
        auto It = Insns.upper_bound(BeforeAddr);
        while (It != Insns.begin()) {
          --It;
          if (It->first < CurrentFuncEntry)
            break;
          if (!chargeEvidence())
            return false;
          for (auto OpIt = It->second.Ops.rbegin();
               OpIt != It->second.Ops.rend(); ++OpIt) {
            if (!chargeEvidence())
              return false;
            if (OpIt->Addr > BeforeAddr ||
                (OpIt->Addr == BeforeAddr && OpIt->Seq >= BeforeSeq))
              continue;
            if (OpIt->Output == Value) {
              Def = &*OpIt;
              break;
            }
          }
          if (Def)
            break;
        }
        if (!Def)
          return false;

        NdVar Next;
        if (Def->Opcode == NdOp::COPY && Def->NumInputs >= 1 &&
            (Def->Inputs[0].isReg() || Def->Inputs[0].isTemp())) {
          Next = Def->Inputs[0];
        } else if (Def->Opcode == NdOp::INT_ZEXT && Def->NumInputs >= 1 &&
                   Def->Inputs[0].Size == Img.getPointerSize() &&
                   Def->Output.Size > Def->Inputs[0].Size &&
                   (Def->Inputs[0].isReg() || Def->Inputs[0].isTemp())) {
          Next = Def->Inputs[0];
        } else if (Def->Opcode == NdOp::INT_ADD && Def->NumInputs >= 2) {
          const bool LeftScalar =
              Def->Inputs[0].isConst() &&
              Def->Inputs[0].Provenance == ConstantAddressProvenance::Scalar;
          const bool RightScalar =
              Def->Inputs[1].isConst() &&
              Def->Inputs[1].Provenance == ConstantAddressProvenance::Scalar;
          if (LeftScalar == RightScalar)
            return false;
          Next = Def->Inputs[LeftScalar ? 1 : 0];
          if (!Next.isReg() && !Next.isTemp())
            return false;
        } else if (Def->Opcode == NdOp::INT_SUB && Def->NumInputs >= 2 &&
                   Def->Inputs[1].isConst() &&
                   Def->Inputs[1].Provenance ==
                       ConstantAddressProvenance::Scalar &&
                   (Def->Inputs[0].isReg() || Def->Inputs[0].isTemp())) {
          Next = Def->Inputs[0];
        } else {
          return false;
        }
        Value = Next;
        BeforeAddr = Def->Addr;
        BeforeSeq = Def->Seq;
      }
      return false;
    };
    BaseIsFrame = derivedFromFrameAtExactUse();
  }
  if (StackTableEvidenceIncompleteBranches.count(Rec.Addr))
    return false;

  const bool DispIsDataVA = Disp != 0 && Img.getSegmentFor(Disp) != nullptr;
  // Only i386 ELF legitimately reuses a nominal frame register as its GOT
  // base.  A data-looking displacement must not redirect a derived ARM frame
  // base away from the all-path stack proof.
  const bool FrameMayBeI386GOT = BaseIsFrame && Img.Arch == Arch::X86 &&
                                 Img.isELF() && Img.getPointerSize() == 4 &&
                                 DispIsDataVA;
  const bool GotOff = Disp != 0 && (!BaseIsFrame || FrameMayBeI386GOT);
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
    std::vector<JumpTableFrameInitializerChunk> StackInitializers;
    std::vector<JumpTableValueOccurrence> StackStorageConsumers;
    va_t StackSrc = InvalidVA;
    if (BaseIsFrame)
      StackSrc = resolveStackMaterializedTableSource(
          Img, Rec, BlockOps, LoadIdx, BaseReg, LoadWidth,
          FrameRuntimeBase.ByteAddend, &StackTableMutated, &StackInitializers,
          &StackStorageConsumers);
    // Resource exhaustion is not negative table evidence.  Stop this strategy
    // transaction immediately so the same branch cannot be reinterpreted by a
    // cheaper numeric/folding fallback after its stack proof became incomplete.
    if (StackTableEvidenceIncompleteBranches.count(Rec.Addr))
      return false;
    if (StackSrc != InvalidVA) {
      TableAddr = StackSrc;
      if (AddressOccurrence.Value.Size == 0 ||
          AddressOccurrence.Addr == InvalidVA || AddressOccurrence.Seq < 0 ||
          !AddressOccurrence.DefinedAtPoint)
        return false;
      if (FrameRuntimeBase.Use.Value.Size == 0 ||
          FrameRuntimeBase.Use.Addr == InvalidVA ||
          FrameRuntimeBase.Use.Seq < 0 || FrameRuntimeBase.Use.DefinedAtPoint ||
          StackInitializers.empty())
        return false;
      Info.AuthenticatedFrameStorage.RuntimeBase = FrameRuntimeBase;
      Info.AuthenticatedFrameStorage.CompleteAddress = AddressOccurrence;
      Info.AuthenticatedFrameStorage.Initializers =
          std::move(StackInitializers);
      Info.AuthenticatedStorageConsumers = std::move(StackStorageConsumers);
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
  Info.IndexValueAtUse = IndexValueAtUse;
  Info.IndexUseAddr = IndexUseAddr;
  Info.IndexUseSeq = IndexUseSeq;
  if (LoadIdx >= 0) {
    Info.TableLoadAddr = BlockOps[LoadIdx].Addr;
    Info.TableLoadSeq = BlockOps[LoadIdx].Seq;
    Info.TargetLoads = {{BlockOps[LoadIdx].Output, BlockOps[LoadIdx].Addr,
                         BlockOps[LoadIdx].Seq,
                         /*DefinedAtPoint=*/true}};
  } else if (Unscaled && UnscaledLoadAddr != InvalidVA) {
    Info.TargetLoads = {{UnscaledLoadOutput, UnscaledLoadAddr, UnscaledLoadSeq,
                         /*DefinedAtPoint=*/true}};
    Info.TableLoadAddr = UnscaledLoadAddr;
    Info.TableLoadSeq = UnscaledLoadSeq;
  }

  // Reloc-driven absolute table: a run of loader-applied absolute code-pointer
  // relocations starting at the base means the entries are absolute targets;
  // the bounded run length is authenticated physical capacity.  This recovers
  // a computed-goto or threaded dispatch (which has no comparison guard to
  // bound it) and resolves
  // the 4-byte absolute-vs-PIC-relative ambiguity — a PIC switch table carries
  // no relocations on its entries.
  uint32_t RelocRun = countCodePtrRelocRun(Img, TableAddr, LoadWidth);

  // A stack-table source proof is not complete until its exact initializer
  // occurrences have also been reconciled with the independently published
  // relocation anchors below.  Keep that post-processing on the same
  // resolver-stage allowance as the frame/memory proof: every candidate in
  // this graph shares one transactional scan budget.  Ordinary non-stack
  // tables retain their existing behaviour and do not consume this specialized
  // allowance.
  const bool BudgetAuthenticatedSources =
      !Info.AuthenticatedFrameStorage.Initializers.empty();
  bool AuthenticatedSourceEvidenceComplete = true;
  auto chargeAuthenticatedSourceEvidence = [&](size_t Amount = 1) {
    if (!BudgetAuthenticatedSources)
      return true;
    if (AuthenticatedSourceEvidenceComplete &&
        consumeStackTableEvidence(Amount))
      return true;
    AuthenticatedSourceEvidenceComplete = false;
    StackTableEvidenceIncompleteBranches.insert(Rec.Addr);
    return false;
  };

  // currentRelocatedInstructionTableAnchors() visits every published address
  // occurrence once.  Pre-charge that exact traversal so exhaustion happens
  // before it can construct a partial candidate-local anchor set.
  if (!chargeAuthenticatedSourceEvidence(
          RelocatedInstructionAddressOccurrences.size()))
    return false;
  std::set<va_t> CandidateAnchors =
      currentRelocatedInstructionTableAnchors(Img);
  // The source trace above authenticates the exact immutable LOADs that filled
  // this frame table.  Their address materializations are consumers of this
  // candidate's one storage owner, not independent adjacent table bases.  Drop
  // only exact source occurrences whose complete byte spans belong to this
  // candidate.  Loader-field authority remains field keyed; linked AArch64 may
  // instead use a field-less relocation-free dereference certificate.  Adjusted
  // pointers and unrelated anchors, including an adjacent table in the same
  // section, continue to bound the run.
  auto authenticatedSourceForOccurrence =
      [&](const RelocatedInstructionAddressOccurrence &Occurrence)
      -> std::optional<AuthenticatedSourceAnchorExemption> {
    auto Matches = [&](const JumpTableValueOccurrence &Producer, va_t FieldVA,
                       va_t TargetVA, va_t OwnerVA,
                       ConstantAddressProvenance Provenance,
                       va_t EffectiveSourceVA, uint64_t SourceByteCount)
        -> std::optional<AuthenticatedSourceAnchorExemption> {
      if (TargetVA == InvalidVA || OwnerVA == InvalidVA ||
          EffectiveSourceVA == InvalidVA || SourceByteCount == 0 ||
          Provenance != ConstantAddressProvenance::DataAddress ||
          Occurrence.TargetVA != TargetVA ||
          Occurrence.TargetOwnerVA != OwnerVA ||
          Occurrence.Provenance != Provenance ||
          Occurrence.InstructionAddr != Producer.Addr ||
          Occurrence.OpSeq != Producer.Seq || Occurrence.OutputMayDepend)
        return std::nullopt;
      const bool ExactLoaderField =
          Occurrence.Authority ==
              RelocatedInstructionAddressProofKind::LoaderField &&
          FieldVA != InvalidVA && Occurrence.FieldVA == FieldVA;
      const bool ExactRelocationFreeOccurrence =
          Occurrence.Authority == RelocatedInstructionAddressProofKind::
                                      AArch64RelocationFreeDataDereference &&
          FieldVA == InvalidVA && Occurrence.FieldVA == InvalidVA;
      if (!ExactLoaderField && !ExactRelocationFreeOccurrence)
        return std::nullopt;
      const bool ExactProducer =
          Producer.DefinedAtPoint
              ? Occurrence.DefinesOutput &&
                    Occurrence.OutputWitness == Producer.Value
              : !Occurrence.DefinesOutput && Occurrence.InputIndex >= 0 &&
                    Producer.Value.isConst() &&
                    Producer.Value.Offset == TargetVA &&
                    Producer.Value.Provenance == Provenance &&
                    Producer.Value.AddressOwnerVA == OwnerVA;
      if (!ExactProducer)
        return std::nullopt;
      return AuthenticatedSourceAnchorExemption{
          TableAddr, FieldVA,           TargetVA,
          OwnerVA,   EffectiveSourceVA, SourceByteCount};
    };
    std::optional<AuthenticatedSourceAnchorExemption> Exact;
    auto RecordExact =
        [&](std::optional<AuthenticatedSourceAnchorExemption> Candidate) {
          if (!Candidate)
            return true;
          if (Exact)
            return false;
          Exact = std::move(Candidate);
          return true;
        };
    for (const JumpTableFrameInitializerChunk &Initializer :
         Info.AuthenticatedFrameStorage.Initializers) {
      if (!chargeAuthenticatedSourceEvidence())
        return std::nullopt;
      if (Initializer.IsMemcpy &&
          !RecordExact(Matches(
              Initializer.StaticSourceProducer, Initializer.StaticSourceFieldVA,
              Initializer.StaticSourceProducerTargetVA,
              Initializer.StaticSourceOwnerVA,
              Initializer.StaticSourceProvenance,
              Initializer.StaticSourceAddress, Initializer.ByteCount)))
        return std::nullopt;
      for (const auto &Source : Initializer.StaticSources) {
        if (!chargeAuthenticatedSourceEvidence())
          return std::nullopt;
        if (!RecordExact(Matches(
                Source.StaticAddressProducer, Source.StaticAddressFieldVA,
                Source.StaticAddressProducerTargetVA,
                Source.StaticAddressOwnerVA, Source.StaticAddressProvenance,
                Source.StaticAddress, Source.ByteCount)))
          return std::nullopt;
      }
    }
    return Exact;
  };
  // Keep exact decoded identities instead of copying their variable-length
  // proof payloads.  Loader-field exemptions are also passed to the raw
  // loader-anchor helper; relocation-free identities have no FieldVA and can
  // only remove their already-published decoded anchor below.
  struct AuthenticatedOccurrenceSource {
    AuthenticatedSourceAnchorExemption Exemption;
    size_t OccurrenceIndex = 0;
  };
  std::map<va_t, AuthenticatedOccurrenceSource>
      AuthenticatedLoaderOccurrenceSources;
  std::vector<AuthenticatedOccurrenceSource>
      AuthenticatedRelocationFreeOccurrenceSources;
  std::map<va_t, AuthenticatedSourceAnchorExemption> AuthenticatedSources;
  std::set<va_t> AmbiguousAuthenticatedSourceFields;
  if (BudgetAuthenticatedSources) {
    for (size_t OccurrenceIndex = 0;
         OccurrenceIndex < RelocatedInstructionAddressOccurrences.size();
         ++OccurrenceIndex) {
      const RelocatedInstructionAddressOccurrence &Occurrence =
          RelocatedInstructionAddressOccurrences[OccurrenceIndex];
      if (!chargeAuthenticatedSourceEvidence())
        return false;
      std::optional<AuthenticatedSourceAnchorExemption> Exemption =
          authenticatedSourceForOccurrence(Occurrence);
      if (!AuthenticatedSourceEvidenceComplete)
        return false;
      if (!Exemption)
        continue;
      if (Occurrence.Width == 0)
        continue;

      unsigned LoaderFieldMatches = 0;
      bool HasCompleteRawField = false;
      const bool IsLoaderField =
          Occurrence.Authority ==
          RelocatedInstructionAddressProofKind::LoaderField;
      if (IsLoaderField) {
        // First validate the direct source span against the exact target/owner
        // already bound to this decoded operation. A synthetic complete field
        // is used only for the shared span/owner predicate; a separate check
        // below requires exactly one real loader-side field record.
        const RelocatedAddressField ExactDecodedField{
            0, Occurrence.TargetVA, Occurrence.Width,
            Occurrence.TargetOwnerVA};
        if (!authenticatedSourceAnchorExemptionMatches(
                *Exemption, TableAddr, LoadWidth, RelocRun,
                Exemption->FieldVA, ExactDecodedField))
          continue;

        const auto Field =
            Img.DataAddressRelocOperands.find(Exemption->FieldVA);
        if (Field != Img.DataAddressRelocOperands.end() &&
            Field->second.Width == Occurrence.Width &&
            Field->second.TargetOwnerVA == Occurrence.TargetOwnerVA &&
            Field->second.PCRelativeFromInstructionEnd ==
                Occurrence.PCRelativeFromInstructionEnd) {
          if (Field->second.PCRelativeFromInstructionEnd) {
            // The loader cannot know InsnEnd and therefore must not publish an
            // approximate target.  Occurrence.TargetVA is the sole target.
            if (Field->second.TargetVA == InvalidVA)
              ++LoaderFieldMatches;
          } else if (Field->second.TargetVA == Occurrence.TargetVA) {
            ++LoaderFieldMatches;
            HasCompleteRawField = true;
          }
        }
        const auto Materialized =
            Img.InstructionAddressMaterializations.find(Exemption->FieldVA);
        if (Materialized != Img.InstructionAddressMaterializations.end() &&
            Occurrence.DefinesOutput &&
            !Occurrence.PCRelativeFromInstructionEnd &&
            Occurrence.Width == 4 &&
            Materialized->second.TargetVA == Occurrence.TargetVA &&
            Materialized->second.TargetOwnerVA == Occurrence.TargetOwnerVA)
          ++LoaderFieldMatches;
        const auto ARMRelative =
            Img.ARMRelativeLiteralFields.find(Exemption->FieldVA);
        if (ARMRelative != Img.ARMRelativeLiteralFields.end() &&
            Occurrence.DefinesOutput &&
            !Occurrence.PCRelativeFromInstructionEnd &&
            Occurrence.Width == 4 &&
            ARMRelative->second.TargetVA == Occurrence.TargetVA &&
            ARMRelative->second.TargetOwnerVA == Occurrence.TargetOwnerVA)
          ++LoaderFieldMatches;
        if (LoaderFieldMatches != 1 ||
            AmbiguousAuthenticatedSourceFields.count(Exemption->FieldVA))
          continue;
        if (!AuthenticatedLoaderOccurrenceSources
                 .emplace(
                     Exemption->FieldVA,
                     AuthenticatedOccurrenceSource{*Exemption, OccurrenceIndex})
                 .second) {
          AuthenticatedLoaderOccurrenceSources.erase(Exemption->FieldVA);
          AuthenticatedSources.erase(Exemption->FieldVA);
          AmbiguousAuthenticatedSourceFields.insert(Exemption->FieldVA);
          continue;
        }
        if (HasCompleteRawField)
          AuthenticatedSources.emplace(Exemption->FieldVA, *Exemption);
        continue;
      }

      if (Img.Arch != Arch::AArch64 || Img.IsRelocatable ||
          !authenticatedSourceAnchorExemptionMatches(
              *Exemption, TableAddr, LoadWidth, RelocRun, Occurrence))
        continue;
      AuthenticatedRelocationFreeOccurrenceSources.push_back(
          {*Exemption, OccurrenceIndex});
    }

    std::set<va_t> AuthenticatedSourceTargets;
    for (const auto &[FieldVA, Source] : AuthenticatedLoaderOccurrenceSources) {
      if (!chargeAuthenticatedSourceEvidence())
        return false;
      (void)FieldVA;
      AuthenticatedSourceTargets.insert(Source.Exemption.TargetVA);
    }
    for (const AuthenticatedOccurrenceSource &Source :
         AuthenticatedRelocationFreeOccurrenceSources) {
      if (!chargeAuthenticatedSourceEvidence())
        return false;
      AuthenticatedSourceTargets.insert(Source.Exemption.TargetVA);
    }
    std::set<va_t> TargetsWithIndependentOccurrences;
    for (size_t OccurrenceIndex = 0;
         OccurrenceIndex < RelocatedInstructionAddressOccurrences.size();
         ++OccurrenceIndex) {
      const RelocatedInstructionAddressOccurrence &Occurrence =
          RelocatedInstructionAddressOccurrences[OccurrenceIndex];
      if (!chargeAuthenticatedSourceEvidence())
        return false;
      if (!AuthenticatedSourceTargets.count(Occurrence.TargetVA) ||
          !PublishedReachableInsns.count(Occurrence.InstructionAddr) ||
          Occurrence.OutputMayDepend ||
          Occurrence.Provenance != ConstantAddressProvenance::DataAddress ||
          (!Img.RelCodeRelocSlots.count(Occurrence.TargetVA) &&
           !Img.CodePtrRelocSlots.count(Occurrence.TargetVA)))
        continue;
      const size_t AuthenticatedOccurrenceCount =
          AuthenticatedLoaderOccurrenceSources.size() +
          AuthenticatedRelocationFreeOccurrenceSources.size();
      if (!chargeAuthenticatedSourceEvidence(AuthenticatedOccurrenceCount))
        return false;
      auto MatchesAuthenticatedSource = [&](const AuthenticatedOccurrenceSource
                                                &Source) {
        return Source.Exemption.TargetVA == Occurrence.TargetVA &&
               Source.Exemption.TargetOwnerVA == Occurrence.TargetOwnerVA &&
               Source.OccurrenceIndex == OccurrenceIndex;
      };
      const bool IsAuthenticatedSource =
          std::any_of(AuthenticatedLoaderOccurrenceSources.begin(),
                      AuthenticatedLoaderOccurrenceSources.end(),
                      [&](const auto &Item) {
                        return MatchesAuthenticatedSource(Item.second);
                      }) ||
          std::any_of(AuthenticatedRelocationFreeOccurrenceSources.begin(),
                      AuthenticatedRelocationFreeOccurrenceSources.end(),
                      MatchesAuthenticatedSource);
      if (!IsAuthenticatedSource)
        TargetsWithIndependentOccurrences.insert(Occurrence.TargetVA);
    }
    for (va_t Target : AuthenticatedSourceTargets) {
      if (!chargeAuthenticatedSourceEvidence())
        return false;
      if (!TargetsWithIndependentOccurrences.count(Target))
        CandidateAnchors.erase(Target);
    }
  }

  // boundCodePtrRunByNextAnchor() copies both anchor sets, scans every loader
  // address-relocation field once, then walks at most the union of those three
  // inventories.  Charge that provable upper bound before entering the helper;
  // this keeps an exhausted stack candidate transactional without changing the
  // shared helper's non-stack callers.
  if (RelocRun != 0) {
    if (!chargeAuthenticatedSourceEvidence(Img.RelCodeTableAnchors.size()) ||
        !chargeAuthenticatedSourceEvidence(CandidateAnchors.size()) ||
        !chargeAuthenticatedSourceEvidence(
            Img.DataAddressRelocOperands.size()) ||
        !chargeAuthenticatedSourceEvidence(Img.RelCodeTableAnchors.size()) ||
        !chargeAuthenticatedSourceEvidence(CandidateAnchors.size()) ||
        !chargeAuthenticatedSourceEvidence(Img.DataAddressRelocOperands.size()))
      return false;
  }
  RelocRun =
      boundCodePtrRunByNextAnchor(Img, TableAddr, LoadWidth, RelocRun,
                                  CandidateAnchors, AuthenticatedSources);
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
    Info.PhysicalCapacity = RelocRun;
    Info.RelocAbsolute = true;
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
  if (Info.IsRelative && !Info.HasTargetBase) {
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
          Info.setTargetBase(*Anchor);
          Info.EntryScale = 1;
          break;
        }
      }
    }
  }
  return true;
}

} // namespace neverd
