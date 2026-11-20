//===- MedMIPSMergeValues.cpp - Complete merge-load value proofs
//-----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Proves that complementary loads overwrite every incoming destination bit
/// without changing either memory access or an observed intermediate value.
//===----------------------------------------------------------------------===//

#include "neverd/ArchSupport.h"
#include "neverd/ir/intrinsics/MIPS/MIPSScalar.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/MIPS/MIPSMergeValues.h"
#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

#include <optional>
#include <tuple>
#include <utility>

namespace neverd {
namespace {

using ValueKey = std::tuple<MedVar::VarKind, int, int>;
ValueKey key(const MedVar &V) { return {V.Kind, V.Id, V.SSAVer}; }

struct Use {
  const MedOp *Op;
  unsigned Input;
};

struct Address {
  std::optional<MedVar> Base;
  uint64_t Offset = 0;
};

std::optional<mips::MergeTransfer> mergeLoad(const MedOp &Op) {
  if (Op.Opcode != NdOp::INTRINSIC || !Op.NumInputs || !Op.Inputs[0].isConst())
    return std::nullopt;
  return mips::mergeLoad(static_cast<Intrinsic>(Op.Inputs[0].ConstVal));
}

const LowOp *physicalMerge(const LowBlock &Block,
                           const LowInstructionBoundary &Boundary,
                           Arch Target) {
  const bool CompressedMerge =
      Boundary.Mode == InstructionMode::MicroMIPS ||
      (Boundary.Mode == InstructionMode::MIPS16 && !isMIPS64(Target));
  if (Boundary.Control != LowInstructionControl::None || Boundary.DelaySlot ||
      (Boundary.Mode != InstructionMode::Default &&
       Boundary.Mode != InstructionMode::MIPSClassic &&
       !(CompressedMerge && Boundary.Size == 4)))
    return nullptr;
  const LowOp *Found = nullptr;
  for (uint64_t I = Boundary.FirstOp; I < Boundary.FirstOp + Boundary.OpCount;
       ++I) {
    const LowOp &Op = Block.Ops[I];
    if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
        Op.Inputs[0].isConst() &&
        mips::mergeLoad(static_cast<Intrinsic>(Op.Inputs[0].Offset))) {
      if (Found)
        return nullptr;
      Found = &Op;
    }
  }
  return Found;
}

} // namespace

unsigned discardUnobservedMIPSMergeInputs(MedFunc &Function,
                                          const LowFunc &Physical,
                                          Arch Target) {
  if (!isMIPS(Target) || Function.Blocks.size() != 1 ||
      Physical.Blocks.size() != 1 || !Function.CallInfos.empty())
    return 0;
  auto &Block = Function.Blocks.front();
  const auto &Low = Physical.Blocks.front();
  if (!Block.Phis.empty() || !Block.Preds.empty() || !Block.Succs.empty() ||
      !Block.ExceptionalPreds.empty() || !Block.ExceptionalSuccs.empty() ||
      !Low.ExceptionalPreds.empty() || !Low.ExceptionalSuccs.empty())
    return 0;

  // Bound candidates to original occurrences before allocating SSA indexes.
  using Occurrence = std::pair<va_t, int>;
  llvm::SmallDenseMap<Occurrence, NdOp, 2> Calls;
  llvm::SmallVector<std::pair<const LowOp *, const LowOp *>, 4> Candidates;
  const LowOp *Previous = nullptr;
  va_t PreviousEnd = InvalidVA;
  auto PreviousMode = InstructionMode::Default;
  for (const auto &Boundary : Low.InstructionBoundaries) {
    if (Boundary.Control == LowInstructionControl::Call) {
      const LowOp *Call = nullptr;
      for (const auto &Op :
           llvm::ArrayRef(Low.Ops).slice(Boundary.FirstOp, Boundary.OpCount)) {
        if (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL)
          continue;
        if (Call)
          return 0;
        Call = &Op;
      }
      if (!Call || Call->Addr != Boundary.Address ||
          !Calls.try_emplace({Call->Addr, Call->Seq}, Call->Opcode).second)
        return 0;
    } else if (Boundary.Control != LowInstructionControl::None &&
               Boundary.Control != LowInstructionControl::Return) {
      return 0;
    }
    const LowOp *Current = physicalMerge(Low, Boundary, Target);
    const auto Mode = Boundary.Mode == InstructionMode::Default
                          ? InstructionMode::MIPSClassic
                          : Boundary.Mode;
    if (Previous && Current && PreviousEnd == Boundary.Address &&
        PreviousMode == Mode)
      Candidates.push_back({Previous, Current});
    Previous = Current;
    PreviousMode = Mode;
    PreviousEnd = Boundary.Address + Boundary.Size;
  }
  if (Candidates.empty() || (Calls.empty() && !Function.CallClobbers.empty()))
    return 0;

  llvm::DenseMap<Occurrence, MedOp *> Merges;
  llvm::DenseMap<ValueKey, llvm::SmallVector<Use, 2>> Uses;
  llvm::DenseMap<ValueKey, Address> Addresses;
  // A later ABI pass may read a physical register at a call or return even
  // when the current MedOp has no explicit operand for that register.
  llvm::DenseMap<std::pair<uint64_t, unsigned>, ValueKey> RegisterViews;
  llvm::DenseSet<ValueKey> Observed;
  const auto ObserveRegisters = [&] {
    for (const auto &View : RegisterViews)
      Observed.insert(View.second);
  };
  const unsigned Width = isMIPS64(Target) ? 8 : 4;
  const uint64_t Mask = Width == 8 ? UINT64_MAX : UINT32_MAX;
  const auto Resolve = [&](const MedVar &V) -> Address {
    if (V.isConst() && V.Provenance == ConstantAddressProvenance::Scalar)
      return {std::nullopt, V.ConstVal & Mask};
    if (!V.isConst())
      if (const auto It = Addresses.find(key(V)); It != Addresses.end())
        return It->second;
    // An address or address fragment may be a shared symbolic base, never a
    // numeric displacement. Distinct relocated constants cannot be subtracted.
    return {V, 0};
  };
  for (auto &Op : Block.Ops) {
    if (Op.Dead)
      continue;
    if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
      const auto Call = Calls.find({Op.Addr, Op.OriginSeq});
      if (!Op.SourceCallHint || !Op.SourceCallHint->Signature.MIPSRecords ||
          Call == Calls.end() || Call->second != Op.Opcode)
        return 0;
      Calls.erase(Call);
      ObserveRegisters();
    } else if (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR ||
               Op.Opcode == NdOp::INDIR_BR) {
      return 0;
    }
    if (Op.Opcode == NdOp::INTRINSIC) {
      const auto Id = static_cast<Intrinsic>(Op.Inputs[0].ConstVal);
      if (!mips::isScalarIntrinsic(Id) && Id != Intrinsic::RequireAligned)
        return 0; // Other implicit machine-state effects are not proved here.
    }
    if (mergeLoad(Op))
      Merges.try_emplace(Occurrence{Op.Addr, Op.OriginSeq}, &Op);
    for (unsigned I = 0; I < Op.NumInputs; ++I)
      if (!Op.Inputs[I].isConst())
        Uses[key(Op.Inputs[I])].push_back({&Op, I});
    if (Op.Output.Kind == MedVar::Reg && Op.Output.Size)
      RegisterViews[{Op.Output.RegOff, Op.Output.Size}] = key(Op.Output);
    if (Op.Output.Size != Width)
      continue;
    if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
        Op.Inputs[0].Size == Width) {
      Addresses[key(Op.Output)] = Resolve(Op.Inputs[0]);
      continue;
    }
    if ((Op.Opcode != NdOp::INT_ADD && Op.Opcode != NdOp::INT_SUB) ||
        Op.NumInputs != 2 || Op.Inputs[0].Size != Width ||
        Op.Inputs[1].Size != Width)
      continue;
    unsigned Constant = 1;
    if (Op.Opcode == NdOp::INT_ADD && Op.Inputs[0].isConst())
      Constant = 0;
    const auto &C = Op.Inputs[Constant];
    if (!C.isConst() || C.Provenance != ConstantAddressProvenance::Scalar)
      continue;
    auto A = Resolve(Op.Inputs[1 - Constant]);
    A.Offset = (Op.Opcode == NdOp::INT_SUB ? A.Offset - C.ConstVal
                                           : A.Offset + C.ConstVal) &
               Mask;
    Addresses[key(Op.Output)] = A;
  }

  if (!Calls.empty())
    return 0;
  ObserveRegisters();
  for (const auto &Clobber : Function.CallClobbers)
    if (Clobber.PreservedPrefixSize)
      Observed.insert(key(Clobber.PreservedInput));

  unsigned Changed = 0;
  for (const auto &[FirstLow, SecondLow] : Candidates) {
    const auto FirstIt = Merges.find({FirstLow->Addr, FirstLow->Seq});
    const auto SecondIt = Merges.find({SecondLow->Addr, SecondLow->Seq});
    if (FirstIt == Merges.end() || SecondIt == Merges.end())
      continue;
    MedOp &First = *FirstIt->second;
    const MedOp &Second = *SecondIt->second;
    const auto F = *mergeLoad(First);
    const auto S = *mergeLoad(Second);
    if (F.Bytes != S.Bytes || F.Left == S.Left ||
        (First.Inputs[2].isConst() && First.Inputs[2].ConstVal == 0))
      continue;
    const auto Left = Resolve((F.Left ? First : Second).Inputs[1]);
    const auto Right = Resolve((F.Left ? Second : First).Inputs[1]);
    if (Left.Base != Right.Base)
      continue;
    const auto Delta = archLittleEndian(Target) ? Left.Offset - Right.Offset
                                                : Right.Offset - Left.Offset;
    if ((Delta & Mask) != F.Bytes - 1u)
      continue;

    // Every use of the first result must either transport its low N bytes or
    // be the second core's old value. The second core still needs the bytes
    // read by the first one. A store, address, return or arithmetic use rejects
    // the rewrite, even if another use completes a complementary pair.
    llvm::SmallVector<MedVar, 4> Pending{First.Output};
    llvm::DenseSet<ValueKey> Seen;
    bool Complete = false;
    bool Safe = true;
    while (!Pending.empty() && Safe) {
      const auto Value = Pending.pop_back_val();
      if (!Seen.insert(key(Value)).second)
        continue;
      if (Observed.contains(key(Value))) {
        Safe = false;
        break;
      }
      const auto It = Uses.find(key(Value));
      if (It == Uses.end())
        continue;
      for (const auto &Use : It->second) {
        const MedOp &Op = *Use.Op;
        if (&Op == &Second && Use.Input == 2 && Op.Inputs[2].Size == F.Bytes) {
          Complete = true;
          continue;
        }
        const bool Copy = Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
                          Op.Output.Size == Op.Inputs[0].Size;
        const bool Slice = Op.Opcode == NdOp::SUBBYTES && Op.NumInputs == 2 &&
                           Op.Inputs[1].isConst() && !Op.Inputs[1].ConstVal &&
                           Op.Output.Size <= Op.Inputs[0].Size;
        const bool Extend = Op.Opcode == NdOp::INT_SEXT && Op.NumInputs == 1 &&
                            Op.Output.Size >= Op.Inputs[0].Size;
        if (Use.Input != 0 || (!Copy && !Slice && !Extend) ||
            Op.Inputs[0].Size < F.Bytes || Op.Output.Size < F.Bytes ||
            Op.Output.Size > Width) {
          Safe = false;
          break;
        }
        Pending.push_back(Op.Output);
      }
    }
    if (Safe && Complete) {
      First.Inputs[2] =
          MedVar::makeConst(0, F.Bytes, ConstantAddressProvenance::Scalar);
      ++Changed;
    }
  }
  return Changed;
}

} // namespace neverd
