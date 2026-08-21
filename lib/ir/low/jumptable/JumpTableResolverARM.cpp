//===- JumpTableResolverARM.cpp - ARM-family jump-table detectors --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ARM-family architecture-gated jump-table detectors:
///
///   - tryARMTableBranch      — ARM32 TBB/TBH table-branch pattern.
///   - tryAArch64CompactTable — AArch64 byte/halfword compact table.
///
/// These are the only architecture-specific table recognizers; the generic
/// resolver framework and every pattern-based (architecture-neutral) strategy
/// are dispatched from JumpTableResolver.cpp (LLVM target-dispatch pattern).
/// x86 switch tables need no dedicated detector — they are handled by the
/// architecture-neutral source and shape recognizers.
///
//===----------------------------------------------------------------------===//

#include "JumpTableResolverDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"

#include <optional>

namespace neverd {

//===----------------------------------------------------------------------===//
// tryARMTableBranch — ARM32 TBB/TBH pattern detection
//===----------------------------------------------------------------------===//

bool CFGBuilder::tryARMTableBranch(const BinaryImage &Img,
                                   const InsnRecord &Rec, JumpTableInfo &Info) {
  if (Img.Arch != Arch::ARM)
    return false;

  // TBB (Table Branch Byte) and TBH (Table Branch Halfword) use a base
  // register (typically PC) plus a small table of 1- or 2-byte entries
  // that are scaled offsets from the table address.
  //
  // Lifted NdOp pattern for TBB:
  //   LOAD [base + index]          -> offset  (1 byte)
  //   INT_ZEXT offset              -> offset_ext
  //   INT_LEFT offset_ext, 1       -> scaled
  //   INT_ADD pc_val, scaled       -> target
  //   INDIR_BR target
  //
  // TBH: same but LOAD produces 2-byte value, shift by 1.

  bool SawLoad = false;
  uint16_t LoadWidth = 0;
  va_t LoadBase = 0;
  bool SawShift = false;
  uint64_t ShiftAmount = 0;
  va_t PCBase = 0;

  for (int I = static_cast<int>(Rec.Ops.size()) - 1; I >= 0; --I) {
    if (Rec.Ops[I].Opcode != NdOp::INDIR_BR)
      continue;

    for (int J = I - 1; J >= 0; --J) {
      auto &Op = Rec.Ops[J];
      switch (Op.Opcode) {
      case NdOp::INT_ADD:
        if (PCBase == 0 && Op.NumInputs >= 2) {
          if (Op.Inputs[0].isConst() && Op.Inputs[0].Offset != 0)
            PCBase = Op.Inputs[0].Offset;
          else if (Op.Inputs[1].isConst() && Op.Inputs[1].Offset != 0)
            PCBase = Op.Inputs[1].Offset;
        }
        break;
      case NdOp::INT_LEFT:
        if (Op.NumInputs >= 2 && Op.Inputs[1].isConst()) {
          SawShift = true;
          ShiftAmount = Op.Inputs[1].Offset;
        }
        break;
      case NdOp::LOAD:
        SawLoad = true;
        LoadWidth = Op.Output.Size;
        if (Op.NumInputs >= 1 && Op.Inputs[0].isConst())
          LoadBase = Op.Inputs[0].Offset;
        break;
      case NdOp::INT_ZEXT:
      case NdOp::INT_SEXT:
        break;
      default:
        break;
      }
    }
    break;
  }

  if (!SawLoad || LoadWidth == 0 || PCBase == 0)
    return false;

  // TBB uses 1-byte entries, TBH uses 2-byte entries. Both shift by 1.
  if (LoadWidth != 1 && LoadWidth != 2)
    return false;
  if (!SawShift || ShiftAmount != 1)
    return false;

  if (!Img.hasExecutableCodeOwnerAt(PCBase))
    return false;

  // The table typically starts right after the branch instruction.
  va_t TableAddr = LoadBase;
  if (TableAddr == 0)
    TableAddr = Rec.Addr + Rec.Size;

  Info.setBaseAddr(TableAddr);
  Info.EntrySize = LoadWidth;
  Info.IsRelative = true;
  Info.IsSigned = false;
  return true;
}

//===----------------------------------------------------------------------===//
// tryAArch64CompactTable — AArch64 byte/halfword compact jump table
//===----------------------------------------------------------------------===//

/// Recognize the AArch64 compact-table dispatch clang emits for medium switch
/// sizes:
///
///   adrp/add  Xt, table          ; byte/halfword offset table base
///   adr       Xa, anchor          ; code anchor (a case body / shared tail)
///   ldrb/ldrh Wo, [Xt, Xk]        ; entry = table[k]   (1- or 2-byte)
///   add       Xa, Xa, Wo, lsl #s  ; target = anchor + entry << s
///   br        Xa
///
/// Unlike the PIC word table the entry base (Xt) and the relative base (Xa)
/// differ and the entry is scaled, so it needs TargetBase + entry*EntryScale.
bool CFGBuilder::tryAArch64CompactTable(const BinaryImage &Img,
                                        const InsnRecord &Rec,
                                        JumpTableInfo &Info) {
  if (!CurrentImg || CurrentImg->Arch != Arch::AArch64)
    return false;

  uint64_t TargetReg = InvalidVA;
  for (auto &Op : Rec.Ops)
    if (Op.Opcode == NdOp::INDIR_BR && Op.NumInputs >= 1 &&
        Op.Inputs[0].isReg()) {
      TargetReg = Op.Inputs[0].Offset;
      break;
    }
  if (TargetReg == InvalidVA)
    return false;

  va_t BlkStart = CurrentFuncEntry;
  auto BIt = BlockStarts.upper_bound(Rec.Addr);
  if (BIt != BlockStarts.begin()) {
    --BIt;
    BlkStart = *BIt;
  }
  std::vector<LowOp> Ops;
  for (auto It = Insns.lower_bound(BlkStart);
       It != Insns.end() && It->first <= Rec.Addr; ++It)
    for (auto &Op : It->second.Ops)
      Ops.push_back(Op);

  // target = INT_ADD(anchor, entry << shift).
  int AddIdx = reachingDefIdx(Ops, static_cast<int>(Ops.size()) - 1,
                              NdVar::reg(TargetReg, 8));
  if (AddIdx < 0 || Ops[AddIdx].Opcode != NdOp::INT_ADD ||
      Ops[AddIdx].NumInputs < 2)
    return false;

  int ShiftIdx = -1, AnchorWhich = -1;
  for (int W = 0; W < 2; ++W) {
    int D = reachingDefIdx(Ops, AddIdx - 1, Ops[AddIdx].Inputs[W]);
    if (D >= 0 && Ops[D].Opcode == NdOp::INT_LEFT && Ops[D].NumInputs >= 2 &&
        Ops[D].Inputs[1].isConst()) {
      ShiftIdx = D;
      AnchorWhich = 1 - W;
      break;
    }
  }
  if (ShiftIdx < 0)
    return false;
  uint32_t Shift = static_cast<uint32_t>(Ops[ShiftIdx].Inputs[1].Offset);
  if (Shift == 0 || Shift > limits::kMaxShiftForEntrySize)
    return false;

  // The shifted value traces through ZEXT/SEXT/COPY to a 1- or 2-byte LOAD.
  // The byte/halfword entries are *unscaled* in the load address (`ldrb
  // [base,idx]`), so unlike the word table the index is not multiplied —
  // extract the two address operands directly rather than via the scaled-index
  // analyzer.
  bool Signed = false;
  uint16_t LoadWidth = 0;
  uint64_t CandA = InvalidVA, CandB = InvalidVA, IndexReg = InvalidVA;
  {
    NdVar V = Ops[ShiftIdx].Inputs[0];
    int CurFrom = ShiftIdx - 1;
    for (int Guard = 0; Guard < limits::kMaxSliceDepth; ++Guard) {
      int D = reachingDefIdx(Ops, CurFrom, V);
      if (D < 0)
        break;
      const LowOp &O = Ops[D];
      if (O.Opcode == NdOp::INT_SEXT && O.NumInputs >= 1) {
        Signed = true;
        V = O.Inputs[0];
        CurFrom = D - 1;
        continue;
      }
      if ((O.Opcode == NdOp::INT_ZEXT || O.Opcode == NdOp::COPY) &&
          O.NumInputs >= 1) {
        V = O.Inputs[0];
        CurFrom = D - 1;
        continue;
      }
      if (O.Opcode == NdOp::LOAD) {
        LoadWidth = O.Output.Size;
        const NdVar &AddrV = (O.NumInputs >= 2) ? O.Inputs[1] : O.Inputs[0];
        int A = reachingDefIdx(Ops, D - 1, AddrV);
        if (A >= 0 && Ops[A].Opcode == NdOp::INT_ADD && Ops[A].NumInputs >= 2) {
          // Each address operand is either a bare register (the table base, or
          // the index of a byte table `ldrb [base,idx]`) or a scaled index (the
          // index of a halfword table `ldrh [base,idx,lsl #1]`, whose >127-entry
          // form clang selects once byte offsets would overflow).  A bare-COPY
          // trace misses the scaled index, dropping IndexReg and with it the
          // `switch(x % N)` modulo bound — so fall back to the scaled-index
          // trace when the operand is not a plain register.
          auto candReg = [&](const NdVar &In) -> uint64_t {
            uint64_t R = traceToRegister(Ops, A - 1, In);
            if (R != InvalidVA)
              return R;
            return scaledIndexReg(Ops, A - 1, In);
          };
          CandA = candReg(Ops[A].Inputs[0]);
          CandB = candReg(Ops[A].Inputs[1]);
        }
      }
      break;
    }
  }
  if (LoadWidth != 1 && LoadWidth != 2)
    return false;

  // Whichever address operand folds to a non-empty data segment is the table
  // base; the other is the switch index register.
  std::optional<uint64_t> TableBase;
  for (int Pick = 0; Pick < 2; ++Pick) {
    uint64_t BaseCand = Pick == 0 ? CandA : CandB;
    if (BaseCand == InvalidVA)
      continue;
    auto V = foldRegConstant(Img, Rec, BaseCand);
    if (!V || !Img.getSegmentFor(*V))
      continue;
    const auto *S = Img.getSegmentFor(*V);
    if (S && !S->Data.empty()) {
      TableBase = V;
      IndexReg = Pick == 0 ? CandB : CandA;
      break;
    }
  }
  if (!TableBase)
    return false;
  const auto *TSeg = Img.getSegmentFor(*TableBase);
  if (!TSeg || TSeg->Data.empty())
    return false;

  // Resolve the anchor operand to a code address (an `adr` lifts to a COPY of
  // the absolute target constant).
  std::optional<uint64_t> Anchor;
  {
    NdVar V = Ops[AddIdx].Inputs[AnchorWhich];
    int CurFrom = AddIdx - 1;
    for (int Guard = 0; Guard < limits::kMaxSliceDepth; ++Guard) {
      if (V.isConst()) {
        Anchor = V.Offset;
        break;
      }
      int D = reachingDefIdx(Ops, CurFrom, V);
      if (D < 0)
        break;
      const LowOp &O = Ops[D];
      if (O.Opcode == NdOp::COPY && O.NumInputs >= 1) {
        if (O.Inputs[0].isConst()) {
          Anchor = O.Inputs[0].Offset;
          break;
        }
        V = O.Inputs[0];
        CurFrom = D - 1;
        continue;
      }
      break;
    }
  }
  if (!Anchor && Ops[AddIdx].Inputs[AnchorWhich].isReg())
    Anchor = foldRegConstant(Img, Rec, Ops[AddIdx].Inputs[AnchorWhich].Offset);
  if (!Anchor)
    return false;
  if (!Img.hasExecutableCodeOwnerAt(*Anchor))
    return false;

  if (IndexReg != InvalidVA)
    IndexReg = traceRegSource(Ops, AddIdx, IndexReg);

  Info.setBaseAddr(*TableBase);
  Info.EntrySize = LoadWidth;
  Info.IsRelative = true;
  Info.IsSigned = Signed;
  Info.TargetBase = *Anchor;
  Info.EntryScale = 1u << Shift;
  Info.IndexReg = IndexReg;
  return true;
}

} // namespace neverd
