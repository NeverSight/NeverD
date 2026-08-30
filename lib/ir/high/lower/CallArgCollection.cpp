//===- CallArgCollection.cpp - Call argument collection
//--------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Collects function call arguments by scanning backward from the call site
/// for register writes and stack stores that match the target ABI's argument
/// passing convention.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"

#include <functional>
#include <set>

namespace neverd {

//===----------------------------------------------------------------------===//
// collectCallArgs
//===----------------------------------------------------------------------===//

std::vector<ExprPtr>
MedToHighConverter::collectCallArgs(const MedBlock &CurBlock, size_t CallIdx) {

  const auto &Ops = CurBlock.Ops;
  constexpr int MaxArgs = 8;
  std::vector<ExprPtr> Found(MaxArgs);

  const auto &TRI = getTargetRegInfo(TargetArch);
  uint64_t SpRegOff = TRI.StackPointer;

  auto IsCalleeSave = [&TRI](const MedVar &V) -> bool {
    if (V.Kind != MedVar::Reg)
      return false;
    return TRI.isCalleeSaveReg(V.RegOff);
  };

  // True once the backward scan reaches the block start without hitting an
  // earlier call: only then is an unfound register argument guaranteed to be
  // live-in to the block (a prior call would leave its value indeterminate).
  bool ReachedBlockStart = true;
  for (int J = static_cast<int>(CallIdx) - 1; J >= 0; --J) {
    auto &Prev = Ops[J];
    if (Prev.Opcode == NdOp::CALL || Prev.Opcode == NdOp::INDIR_CALL ||
        Prev.Opcode == NdOp::INTRINSIC) {
      ReachedBlockStart = false;
      break;
    }
    if (Prev.Opcode == NdOp::COPY && Prev.NumInputs >= 1 &&
        Prev.Output.Kind == MedVar::Reg && Prev.Inputs[0].Kind == MedVar::Reg &&
        Prev.Output.RegOff == Prev.Inputs[0].RegOff &&
        Prev.Output.Size == Prev.Inputs[0].Size)
      continue;
    if (Prev.Output.Kind == MedVar::Reg && Prev.Output.Size > 0) {
      int ArgIdx = regToArgIdx(Prev.Output.RegOff);
      if (ArgIdx >= 0 && ArgIdx < MaxArgs && !Found[ArgIdx]) {
        if (Prev.Opcode == NdOp::COPY && Prev.NumInputs >= 1)
          Found[ArgIdx] = medvarToExpr(Prev.Inputs[0]);
        else
          Found[ArgIdx] = medOpToExpr(Prev);
      }
    }
  }

  // A lower-indexed register argument left empty while a higher one is set is a
  // live-in (loop-carried) argument: it was not written in the call's block but
  // arrives via a header PHI (a threaded `acc = f(acc, ...)` keeps the
  // accumulator in its argument register across iterations).  The
  // break-at-first- gap loop below would otherwise drop ALL arguments.  Resolve
  // each gap to the register's reaching definition at the call so the value
  // (not 0) is passed.
  if (ReachedBlockStart) {
    int MaxRegArg = -1;
    for (int K = 0; K < MaxArgs; ++K)
      if (Found[K])
        MaxRegArg = K;
    for (int K = 0; K < MaxRegArg; ++K) {
      if (Found[K] || K >= static_cast<int>(TRI.IntParamRegs.size()))
        continue;
      MedVar LiveIn;
      if (reachingRegAtBlockEntry(CurBlock, TRI.IntParamRegs[K], LiveIn))
        Found[K] = medvarToExpr(LiveIn);
    }
  }

  int FirstStackSlot = 0;
  for (int K = 0; K < MaxArgs; ++K) {
    if (Found[K])
      FirstStackSlot = K + 1;
    else
      break;
  }

  constexpr int StoreWindow = 12;
  int StoreScanStart = std::max(0, static_cast<int>(CallIdx) - StoreWindow);

  for (int J = static_cast<int>(CallIdx) - 1; J >= StoreScanStart; --J) {
    auto &Prev = Ops[J];
    if (Prev.Opcode == NdOp::CALL || Prev.Opcode == NdOp::INDIR_CALL ||
        Prev.Opcode == NdOp::INTRINSIC)
      break;
    if (Prev.Opcode != NdOp::STORE || Prev.NumInputs < 2 ||
        Prev.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      continue;
    if (IsCalleeSave(Prev.Inputs[1]))
      continue;

    auto &AddrVar = Prev.Inputs[0];
    int64_t StackOff = -1;

    if (AddrVar.Kind == MedVar::Reg && AddrVar.RegOff == SpRegOff)
      StackOff = 0;

    if (StackOff < 0 && !AddrVar.isConst()) {
      for (int K = J - 1; K >= 0; --K) {
        auto &DefOp = Ops[K];
        if (DefOp.Output.Id != AddrVar.Id ||
            DefOp.Output.SSAVer != AddrVar.SSAVer)
          continue;
        if (DefOp.Opcode == NdOp::INT_ADD && DefOp.NumInputs >= 2) {
          bool HasSP = false;
          int64_t ConstOff = -1;
          for (uint8_t KI = 0; KI < DefOp.NumInputs; ++KI) {
            if (DefOp.Inputs[KI].Kind == MedVar::Reg &&
                DefOp.Inputs[KI].RegOff == SpRegOff)
              HasSP = true;
            if (DefOp.Inputs[KI].isConst())
              ConstOff = static_cast<int64_t>(DefOp.Inputs[KI].ConstVal);
          }
          if (HasSP && ConstOff >= 0)
            StackOff = ConstOff;
        }
        break;
      }
    }

    if (StackOff < 0 || StackOff >= MaxArgs * 8)
      continue;
    if (StackOff % 8 != 0)
      continue;

    int ArgPos = FirstStackSlot + static_cast<int>(StackOff / 8);
    if (ArgPos >= 0 && ArgPos < MaxArgs && !Found[ArgPos])
      Found[ArgPos] = medvarToExpr(Prev.Inputs[1]);
  }

  std::vector<ExprPtr> Args;
  for (int K = 0; K < MaxArgs; ++K) {
    if (!Found[K])
      break;
    Args.push_back(Found[K]);
  }
  return Args;
}

//===----------------------------------------------------------------------===//
// reachingRegAtBlockEntry
//===----------------------------------------------------------------------===//

bool MedToHighConverter::reachingRegAtBlockEntry(const MedBlock &B,
                                                 uint64_t RegOff,
                                                 MedVar &Out) const {
  if (!CurMed)
    return false;

  auto blockById = [&](int Id) -> const MedBlock * {
    for (const auto &Blk : CurMed->Blocks)
      if (Blk.Id == Id)
        return &Blk;
    return nullptr;
  };

  std::set<int> Visited;
  // entryOf: the value reaching the entry of a block (a PHI there, else the
  // reaching exit value of its predecessors — SSA guarantees the predecessors
  // agree when the block has no PHI for the register).
  // exitOf: the last in-block definition, else the block's entry value.
  std::function<bool(const MedBlock &, MedVar &)> entryOf;
  std::function<bool(const MedBlock &, MedVar &)> exitOf;
  entryOf = [&](const MedBlock &Blk, MedVar &R) -> bool {
    if (!Visited.insert(Blk.Id).second)
      return false;
    for (const auto &Phi : Blk.Phis)
      if (Phi.Output.Kind == MedVar::Reg && Phi.Output.RegOff == RegOff &&
          Phi.Output.Size > 0) {
        R = Phi.Output;
        return true;
      }
    for (int P : Blk.Preds)
      if (const MedBlock *PB = blockById(P))
        if (exitOf(*PB, R))
          return true;
    return false;
  };
  exitOf = [&](const MedBlock &Blk, MedVar &R) -> bool {
    for (auto It = Blk.Ops.rbegin(); It != Blk.Ops.rend(); ++It)
      if (It->Output.Kind == MedVar::Reg && It->Output.RegOff == RegOff &&
          It->Output.Size > 0) {
        R = It->Output;
        return true;
      }
    return entryOf(Blk, R);
  };

  return entryOf(B, Out);
}

//===----------------------------------------------------------------------===//
// regToArgIdx
//===----------------------------------------------------------------------===//

int MedToHighConverter::regToArgIdx(uint64_t RegOff) const {
  return getTargetRegInfo(TargetArch).regToArgIdx(RegOff);
}

} // namespace neverd
