//===- MedMIPSReturnTargets.cpp - Exact entry link identity --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind source return targets to captured incoming RA through complete SSA.
//===----------------------------------------------------------------------===//

#include "../../../../safety/StackSlotFlow.h"
#include "../../abi/MedABIPassDetail.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/med/MIPS/MIPSIntrinsicShapes.h"
#include "neverd/ir/med/MIPS/MIPSReturnTargets.h"
#include "neverd/ir/med/MedStackAlignment.h"
#include "neverd/lift/MIPS/MIPSRegs.h"

#include <functional>
#include <map>
#include <set>
#include <tuple>

namespace neverd {

bool hasMIPSSourceReturnTargets(const LowFunc &Low, const MedFunc &Function,
                                Arch Target, BinaryFormat Format) {
  if (auto Error = validateLowInstructionBoundaries(
          Low, LowInstructionBoundaryRequirement::Required)) {
    llvm::consumeError(std::move(Error));
    return false;
  }
  if (Function.SkippedSSA || !Low.hasCompleteLiftCoverage() ||
      Low.ModuleAnalysisRoots.size() > 1 ||
      Low.OrdinaryModuleAnalysisRoots.size() > 1)
    return false;
  using Site = std::pair<va_t, int>;
  std::set<Site> Required;
  for (const auto &Block : Low.Blocks)
    for (const auto &Boundary : Block.InstructionBoundaries) {
      // A synthetic return does not prove the link supplied to the tail
      // callee. Qualify that pre-transfer identity before admitting tails.
      if (Boundary.Control == LowInstructionControl::TailCall)
        return false;
      for (size_t I = Boundary.FirstOp; I < Boundary.FirstOp + Boundary.OpCount;
           ++I)
        if (const auto &Op = Block.Ops[I]; Op.Opcode == NdOp::RETURN)
          Required.emplace(Op.Addr, Op.Seq);
    }
  if (Required.empty())
    return true;
  using Key = std::tuple<int, int, int, uint64_t, uint16_t>;
  const auto key = [](const MedVar &V) -> Key {
    return {int(V.Kind), V.Id, V.SSAVer, V.RegOff, V.Size};
  };
  std::vector<MedVar> MemoryAddresses;
  std::map<const MedBlock *, int> BlockIndices;
  bool LeafMemory = true;
  for (size_t I = 0; I < Function.Blocks.size(); ++I) {
    const auto &Block = Function.Blocks[I];
    if (!Block.ExceptionalPreds.empty() || !Block.ExceptionalSuccs.empty())
      return false;
    BlockIndices.emplace(&Block, I);
    for (const auto &Op : Block.Ops) {
      const bool NoMemoryEffect =
          Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
          Op.Inputs[0].isConst() &&
          (mips::isDSPControlIntrinsic(
               static_cast<Intrinsic>(Op.Inputs[0].ConstVal)) ||
           mips::isPureScalarValue(
               static_cast<Intrinsic>(Op.Inputs[0].ConstVal))) &&
          mipsScalarMedShapeIsValid(Op, Target);
      if ((Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) && Op.NumInputs)
        MemoryAddresses.push_back(Op.Inputs[0]);
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
          (Op.Opcode == NdOp::INTRINSIC && !NoMemoryEffect &&
           (Op.NumInputs != 3 || !Op.Inputs[0].isConst() ||
            Op.Inputs[0].ConstVal != uint64_t(Intrinsic::RequireAligned))) ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
          Op.MemoryOrdering != NdMemoryOrdering::None)
        LeafMemory = false;
    }
  }
  std::map<Key, std::optional<int64_t>> Offsets;
  if (LeafMemory) {
    const auto Values =
        entryStackOffsets(Function, MemoryAddresses, Target, Format);
    for (size_t I = 0; I < Values.size(); ++I)
      Offsets.emplace(key(MemoryAddresses[I]), Values[I]);
  }
  const auto Offset = [&](const MedVar &V) -> std::optional<int64_t> {
    const auto It = Offsets.find(key(V));
    return It == Offsets.end() ? std::nullopt : It->second;
  };
  MedDefSiteIndex Definitions(Function);
  safety::detail::ExactPhiGraph Graph(Function);
  std::map<Key, bool> Results;
  std::set<Key> Active;
  size_t Remaining = limits::ir::kSourceFrameAnalysisRemaining;
  std::function<bool(const MedVar &)> Prove = [&](const MedVar &Value) {
    if (!Remaining || Value.isConst() || Value.Size != 4 ||
        Active.size() >= limits::ir::kMIPSReturnIdentityDepth)
      return false;
    --Remaining;
    const Key Identity = key(Value);
    if (const auto It = Results.find(Identity); It != Results.end())
      return It->second;
    if (!Active.insert(Identity).second)
      return false;
    const auto Definition = Definitions.find(Value);
    bool Valid = false;
    if (!Definition.Ambiguous && Value.Kind == MedVar::Reg &&
        Value.RegOff == mipsreg::RA &&
        safety::detail::isAuthenticatedEntryRegisterLiveIn(Function, Value)) {
      Valid = true;
    } else if (!Definition.Ambiguous && Definition.Phi &&
               Graph.hasCompleteIncoming(BlockIndices.at(Definition.Block),
                                         *Definition.Phi)) {
      Valid = llvm::all_of(Definition.Phi->Args,
                           [&](const auto &Arg) { return Prove(Arg.second); });
    } else if (!Definition.Ambiguous && Definition.Op) {
      const auto &Op = *Definition.Op;
      const auto Zero = [&](unsigned I) {
        return Op.Inputs[I].isConst() && Op.Inputs[I].ConstVal == 0;
      };
      if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1)
        Valid = Prove(Op.Inputs[0]);
      else if ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_OR ||
                Op.Opcode == NdOp::INT_XOR) &&
               Op.NumInputs == 2)
        Valid = (Zero(0) && Prove(Op.Inputs[1])) ||
                (Zero(1) && Prove(Op.Inputs[0]));
      else if (Op.Opcode == NdOp::LOAD && Op.NumInputs == 1 && LeafMemory) {
        const auto At = Offset(Op.Inputs[0]);
        if (At && *At < 0 && *At <= -4 &&
            *At >= -limits::ir::kSourceFrameAnalysisMaxFrame) {
          const auto Reaching = safety::detail::reachingStackValues(
              Function, Definition.Block->Id,
              int(&Op - Definition.Block->Ops.data()), *At, 4, Offset,
              [](const MedVar &V) { return !V.isConst(); },
              [](const MedVar &) { return true; }, true, false);
          // LeafMemory excludes calls and hidden memory effects. Merely
          // saving another register cannot modify this slot through its
          // value; overlapping or unresolved writes still invalidate it.
          Valid = Reaching.Complete && Reaching.Reachable &&
                  !Reaching.MayBeUninitialized && !Reaching.HasUnknownWrites &&
                  !Reaching.Values.empty() &&
                  llvm::all_of(Reaching.Values, Prove);
        }
      }
    }
    Active.erase(Identity);
    Results.emplace(Identity, Valid);
    return Valid;
  };
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::RETURN) {
        const Site Origin{Op.Addr, Op.OriginSeq};
        if (!Required.erase(Origin) || Op.NumInputs != 1 ||
            !Prove(Op.Inputs[0]))
          return false;
      }
  return Required.empty();
}

} // namespace neverd
