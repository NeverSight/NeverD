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
#include "neverd/lift/ARMRegs.h"

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

  const std::vector<LowOp> &Ops = Rec.Ops;
  int BranchIdx = -1;
  for (int I = static_cast<int>(Ops.size()) - 1; I >= 0; --I)
    if (Ops[I].Opcode == NdOp::INDIR_BR && Ops[I].NumInputs >= 1) {
      BranchIdx = I;
      break;
    }
  if (BranchIdx < 0)
    return false;

  // Follow exactly the TBB/TBH semantic chain emitted by the ARM lifter:
  //   target = architectural_pc + (zext(load(table_addr)) << 1).
  // Anchoring the entry shift through the branch input avoids confusing it
  // with TBH's independent address-index `<< 1`.
  int TargetAddIdx =
      reachingDefIdx(Ops, BranchIdx - 1, Ops[BranchIdx].Inputs[0]);
  if (TargetAddIdx < 0 || Ops[TargetAddIdx].Opcode != NdOp::INT_ADD ||
      Ops[TargetAddIdx].NumInputs < 2)
    return false;

  int EntryShiftIdx = -1;
  int PCOperand = -1;
  for (int Side = 0; Side < 2; ++Side) {
    int D =
        reachingDefIdx(Ops, TargetAddIdx - 1, Ops[TargetAddIdx].Inputs[Side]);
    if (D >= 0 && Ops[D].Opcode == NdOp::INT_LEFT && Ops[D].NumInputs >= 2 &&
        Ops[D].Inputs[1].isConst() && Ops[D].Inputs[1].Offset == 1) {
      EntryShiftIdx = D;
      PCOperand = 1 - Side;
      break;
    }
  }
  if (EntryShiftIdx < 0 || PCOperand < 0)
    return false;

  // TBB/TBH are the Thumb exception to the usual aligned PC read: both their
  // Rn==PC table base and their branch-target base use raw (address + 4).
  // Model the architectural 32-bit wrap explicitly; applying the general
  // Thumb Align(PC, 4) rule to an instruction at address 2 modulo 4 points two
  // bytes into the instruction rather than at the following table.
  const va_t ArchitecturalPC =
      Rec.Mode == InstructionMode::Thumb
          ? static_cast<va_t>(static_cast<uint32_t>(
                static_cast<uint32_t>(Rec.Addr) + uint32_t{4}))
          : Rec.Addr + 8;
  auto foldRegister = [&](uint64_t Reg) -> std::optional<va_t> {
    if (Reg == armreg::PC)
      return ArchitecturalPC;
    return foldRegConstant(Img, Rec, Reg);
  };
  auto foldValue = [&](NdVar V, int From) -> std::optional<va_t> {
    if (V.isConst())
      return V.Offset;
    uint64_t Reg = traceToRegister(Ops, From, V);
    if (Reg == InvalidVA)
      return std::nullopt;
    return foldRegister(Reg);
  };
  std::optional<va_t> PCBase =
      foldValue(Ops[TargetAddIdx].Inputs[PCOperand], TargetAddIdx - 1);
  if (!PCBase || !Img.hasExecutableCodeOwnerAt(*PCBase))
    return false;

  int LoadIdx = -1;
  NdVar Entry = Ops[EntryShiftIdx].Inputs[0];
  int From = EntryShiftIdx - 1;
  for (int Guard = 0; Guard < limits::kMaxSliceDepth; ++Guard) {
    int D = reachingDefIdx(Ops, From, Entry);
    if (D < 0)
      break;
    const LowOp &Def = Ops[D];
    if ((Def.Opcode == NdOp::INT_ZEXT || Def.Opcode == NdOp::COPY) &&
        Def.NumInputs >= 1) {
      Entry = Def.Inputs[0];
      From = D - 1;
      continue;
    }
    if (Def.Opcode == NdOp::LOAD) {
      LoadIdx = D;
      break;
    }
    break;
  }
  if (LoadIdx < 0 || Ops[LoadIdx].NumInputs < 1)
    return false;
  const uint16_t LoadWidth = Ops[LoadIdx].Output.Size;
  if (LoadWidth != 1 && LoadWidth != 2)
    return false;

  const NdVar &Address = Ops[LoadIdx].Inputs[Ops[LoadIdx].NumInputs - 1];
  int AddrAddIdx = reachingDefIdx(Ops, LoadIdx - 1, Address);
  if (AddrAddIdx < 0 || Ops[AddrAddIdx].Opcode != NdOp::INT_ADD ||
      Ops[AddrAddIdx].NumInputs < 2)
    return false;

  struct AddressCandidate {
    uint64_t Reg = InvalidVA;
    NdVar Value;
    va_t UseAddr = InvalidVA;
    int UseSeq = -1;
    std::optional<va_t> Folded;
  } Candidate[2];
  for (int Side = 0; Side < 2; ++Side) {
    const NdVar &V = Ops[AddrAddIdx].Inputs[Side];
    Candidate[Side].Reg = traceToRegister(Ops, AddrAddIdx - 1, V);
    if (Candidate[Side].Reg != InvalidVA) {
      Candidate[Side].Value = V;
      Candidate[Side].UseAddr = Ops[AddrAddIdx].Addr;
      Candidate[Side].UseSeq = Ops[AddrAddIdx].Seq;
    } else {
      Candidate[Side].Reg =
          scaledIndexReg(Ops, AddrAddIdx - 1, V, &Candidate[Side].Value,
                         &Candidate[Side].UseAddr, &Candidate[Side].UseSeq);
    }
    if (Candidate[Side].Reg != InvalidVA)
      Candidate[Side].Folded = foldRegister(Candidate[Side].Reg);
  }

  int BaseSide = -1;
  for (int Side = 0; Side < 2; ++Side) {
    if (!Candidate[Side].Folded)
      continue;
    const Segment *S = Img.getSegmentFor(*Candidate[Side].Folded);
    if (S && !S->Data.empty()) {
      if (BaseSide >= 0)
        return false;
      BaseSide = Side;
    }
  }
  if (BaseSide < 0)
    return false;
  const int IndexSide = 1 - BaseSide;
  if (Candidate[IndexSide].Reg == InvalidVA ||
      (!Candidate[IndexSide].Value.isReg() &&
       !Candidate[IndexSide].Value.isTemp()) ||
      Candidate[IndexSide].Value.Size == 0 ||
      Candidate[IndexSide].UseAddr == InvalidVA ||
      Candidate[IndexSide].UseSeq < 0)
    return false;

  const va_t TableAddr = *Candidate[BaseSide].Folded;
  uint64_t IndexReg = traceRegSource(Ops, AddrAddIdx, Candidate[IndexSide].Reg);

  Info.setBaseAddr(TableAddr);
  Info.EntrySize = LoadWidth;
  Info.IsRelative = true;
  Info.IsSigned = false;
  Info.setTargetBase(*PCBase);
  Info.EntryScale = 2;
  Info.IndexReg = IndexReg;
  Info.IndexValueAtUse = Candidate[IndexSide].Value;
  Info.IndexUseAddr = Candidate[IndexSide].UseAddr;
  Info.IndexUseSeq = Candidate[IndexSide].UseSeq;
  Info.TableLoadAddr = Ops[LoadIdx].Addr;
  Info.TableLoadSeq = Ops[LoadIdx].Seq;
  Info.TargetLoads = {{Ops[LoadIdx].Output, Ops[LoadIdx].Addr, Ops[LoadIdx].Seq,
                       /*DefinedAtPoint=*/true}};
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
  struct IndexCandidate {
    uint64_t Reg = InvalidVA;
    NdVar Value;
    va_t UseAddr = InvalidVA;
    int UseSeq = -1;
    std::vector<JumpTableValueOccurrence> Alternatives;
  };
  IndexCandidate CandA, CandB, IndexCandidateAtLoad;
  uint64_t IndexReg = InvalidVA;
  int TableLoadIdx = -1;
  int AddressAddIdx = -1;
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
        TableLoadIdx = D;
        LoadWidth = O.Output.Size;
        const NdVar &AddrV = (O.NumInputs >= 2) ? O.Inputs[1] : O.Inputs[0];
        int A = reachingDefIdx(Ops, D - 1, AddrV);
        if (A >= 0 && Ops[A].Opcode == NdOp::INT_ADD && Ops[A].NumInputs >= 2) {
          AddressAddIdx = A;
          // Each address operand is either a bare register (the table base, or
          // the index of a byte table `ldrb [base,idx]`) or a scaled index (the
          // index of a halfword table `ldrh [base,idx,lsl #1]`, whose
          // >127-entry form clang selects once byte offsets would overflow).  A
          // bare-COPY trace misses the scaled index, dropping IndexReg and with
          // it the `switch(x % N)` modulo bound — so fall back to the
          // scaled-index trace when the operand is not a plain register.
          auto candReg = [&](const NdVar &In) -> IndexCandidate {
            IndexCandidate Candidate;
            Candidate.Value = In;
            Candidate.UseAddr = Ops[A].Addr;
            Candidate.UseSeq = Ops[A].Seq;
            Candidate.Reg = traceToRegister(Ops, A - 1, In);
            if (Candidate.Reg == InvalidVA)
              Candidate.Reg =
                  scaledIndexReg(Ops, A - 1, In, &Candidate.Value,
                                 &Candidate.UseAddr, &Candidate.UseSeq);
            return Candidate;
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
  auto normalizeLogicalLane = [&](IndexCandidate &Candidate) {
    if (Candidate.Reg == InvalidVA || Candidate.Value.Size == 0 ||
        Candidate.UseAddr == InvalidVA || Candidate.UseSeq < 0)
      return;
    std::vector<JumpTableValueOccurrence> Alternatives;
    for (const LowOp &Def : Ops)
      if (Def.Opcode == NdOp::INT_ZEXT && Def.NumInputs >= 1 &&
          Def.Output == Candidate.Value &&
          Def.Inputs[0].Size != 0 &&
          Def.Inputs[0].Size < Def.Output.Size &&
          (Def.Inputs[0].isReg() || Def.Inputs[0].isTemp()) &&
          (Def.Addr < Candidate.UseAddr ||
           (Def.Addr == Candidate.UseAddr &&
            Def.Seq < Candidate.UseSeq)))
        Alternatives.push_back({Def.Inputs[0], Def.Addr, Def.Seq, false});
    if (Alternatives.empty())
      return;
    auto matches = [&](const std::vector<JumpTableValueOccurrence> &Values) {
      const std::vector<bool> Result = tableValuesMatchAtUses({
          {Candidate.Value, Candidate.UseAddr, Candidate.UseSeq, Values,
           /*AllowZeroExtension=*/true, /*AllowSignExtension=*/false}});
      return !Result.empty() && Result.front();
    };
    if (!matches(Alternatives))
      return;
    // Remove alternatives in batches; each candidate set still needs a full
    // occurrence proof before deletion.
    std::sort(Alternatives.begin(), Alternatives.end(),
              [](const auto &Left, const auto &Right) {
                return std::tie(Left.Addr, Left.Seq) >
                       std::tie(Right.Addr, Right.Seq);
              });
    for (size_t Chunk = Alternatives.size() / 2; Chunk > 0;
         Chunk = Chunk == 1 ? 0 : (Chunk + 1) / 2) {
      for (size_t I = 0; I + Chunk <= Alternatives.size();) {
        std::vector<JumpTableValueOccurrence> Trial = Alternatives;
        Trial.erase(Trial.begin() + static_cast<ptrdiff_t>(I),
                     Trial.begin() + static_cast<ptrdiff_t>(I + Chunk));
        if (!Trial.empty() && matches(Trial))
          Alternatives = std::move(Trial);
        else
          ++I;
      }
    }
    const uint16_t Width = Alternatives.front().Value.Size;
    if (Width == 0 ||
        std::any_of(Alternatives.begin(), Alternatives.end(),
                    [&](const auto &Occurrence) {
                      return Occurrence.Value.Size != Width;
                    }))
      return;
    Candidate.Alternatives = std::move(Alternatives);
    Candidate.Value = Candidate.Alternatives.front().Value;
    Candidate.UseAddr = Candidate.Alternatives.front().Addr;
    Candidate.UseSeq = Candidate.Alternatives.front().Seq;
    if (Candidate.Value.isReg())
      Candidate.Reg = Candidate.Value.Offset;
  };
  std::optional<uint64_t> TableBase;
  for (int Pick = 0; Pick < 2; ++Pick) {
    uint64_t BaseCand = Pick == 0 ? CandA.Reg : CandB.Reg;
    if (BaseCand == InvalidVA)
      continue;
    auto V = foldRegConstant(Img, Rec, BaseCand);
    if (!V || !Img.getSegmentFor(*V))
      continue;
    const auto *S = Img.getSegmentFor(*V);
    if (S && !S->Data.empty()) {
      TableBase = V;
      IndexCandidateAtLoad = Pick == 0 ? CandB : CandA;
      IndexReg = IndexCandidateAtLoad.Reg;
      break;
    }
  }
  if (!TableBase) {
    return false;
  }
  const auto *TSeg = Img.getSegmentFor(*TableBase);
  if (!TSeg || TSeg->Data.empty())
    return false;
  // Compact tables use one scale for the table address and another for the
  // loaded target entry.  Publish the unscaled selector occurrence, not the
  // byte offset used by the LOAD address; otherwise MedIR treats the address
  // arithmetic as the switch selector and the exact-use plan cannot recover
  // the logical index.
  if (LoadWidth > 1 && AddressAddIdx >= 0) {
    int ScaleDef = reachingDefIdx(Ops, AddressAddIdx - 1,
                                  IndexCandidateAtLoad.Value);
    if (ScaleDef >= 0) {
      const LowOp &Scale = Ops[ScaleDef];
      int InputNo = -1;
      for (int I = 0; I < Scale.NumInputs; ++I)
        if (!Scale.Inputs[I].isConst() &&
            ((Scale.Opcode == NdOp::INT_LEFT && I == 0 &&
              Scale.NumInputs >= 2 && Scale.Inputs[1].isConst() &&
              Scale.Inputs[1].Offset < 64 &&
              (uint64_t{1} << Scale.Inputs[1].Offset) == LoadWidth) ||
             (Scale.Opcode == NdOp::INT_MULT && I == 0 &&
              Scale.NumInputs >= 2 && Scale.Inputs[1].isConst() &&
              Scale.Inputs[1].Offset == LoadWidth))) {
          if (InputNo >= 0) {
            InputNo = -1;
            break;
          }
          InputNo = I;
        }
      if (InputNo >= 0) {
        IndexCandidateAtLoad.Value = Scale.Inputs[InputNo];
        IndexCandidateAtLoad.UseAddr = Scale.Addr;
        IndexCandidateAtLoad.UseSeq = Scale.Seq;
        IndexCandidateAtLoad.Reg =
            traceToRegister(Ops, ScaleDef - 1, Scale.Inputs[InputNo]);
        IndexCandidateAtLoad.Alternatives.clear();
      }
    }
  }
  normalizeLogicalLane(IndexCandidateAtLoad);
  IndexReg = IndexCandidateAtLoad.Reg;

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

  if (IndexReg != InvalidVA) {
    // Preserve the existing resolver-facing source trace.  Guard matching uses
    // the separate point-sensitive identity below, so it need not redefine this
    // field and perturb established table classification.
    IndexReg = traceRegSource(Ops, AddIdx, IndexReg);
  }

  Info.setBaseAddr(*TableBase);
  Info.EntrySize = LoadWidth;
  Info.IsRelative = true;
  Info.IsSigned = Signed;
  Info.setTargetBase(*Anchor);
  Info.EntryScale = 1u << Shift;
  Info.IndexReg = IndexReg;
  Info.IndexValueAtUse = IndexCandidateAtLoad.Value;
  Info.IndexUseAddr = IndexCandidateAtLoad.UseAddr;
  Info.IndexUseSeq = IndexCandidateAtLoad.UseSeq;
  Info.IndexValueAlternatives = IndexCandidateAtLoad.Alternatives;
  if (TableLoadIdx >= 0) {
    Info.TableLoadAddr = Ops[TableLoadIdx].Addr;
    Info.TableLoadSeq = Ops[TableLoadIdx].Seq;
    Info.TargetLoads = {{Ops[TableLoadIdx].Output, Ops[TableLoadIdx].Addr,
                         Ops[TableLoadIdx].Seq,
                         /*DefinedAtPoint=*/true}};
  }
  return true;
}

} // namespace neverd
