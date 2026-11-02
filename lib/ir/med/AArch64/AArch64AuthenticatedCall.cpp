//===- AArch64AuthenticatedCall.cpp - Bound call authentication ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep a private authenticated call target inside its call's ABI contract.
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/AArch64/AArch64AuthenticatedCall.h"

#include "neverd/ir/med/IntrinsicShapes.h"

#include "llvm/ADT/DenseMap.h"

#include <utility>

namespace neverd {
namespace {

bool isCallAuthentication(const MedOp &Op) {
  return !Op.Dead && Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
         Op.Inputs[0].isConst() &&
         Op.Inputs[0].ConstVal ==
             static_cast<uint64_t>(Intrinsic::A64_AuthBranch) &&
         Op.Output.Kind == MedVar::Temp && aarch64PAuthMedShapeIsValid(Op);
}

using Key = std::pair<int, int>;
Key key(const MedVar &Value) { return {Value.Id, Value.SSAVer}; }

} // namespace

llvm::SmallVector<AArch64AuthenticatedCall, 4>
findAArch64AuthenticatedCalls(const MedFunc &Function) {
  llvm::SmallVector<AArch64AuthenticatedCall, 4> Result;
  if (Function.SkippedSSA)
    return Result;
  llvm::DenseMap<Key, unsigned> Uses;
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops)
      if (isCallAuthentication(Op))
        Uses.try_emplace(key(Op.Output), 0);
  if (Uses.empty())
    return Result;

  const auto Observe = [&](const MedVar &Value) {
    if (Value.Kind == MedVar::Temp)
      if (auto It = Uses.find(key(Value)); It != Uses.end())
        ++It->second;
  };
  for (const auto &Block : Function.Blocks) {
    for (const auto &Phi : Block.Phis) {
      for (const auto &[Predecessor, Value] : Phi.Args)
        Observe(Value);
      if (Phi.ExceptionalEntry)
        Observe(*Phi.ExceptionalEntry);
    }
    for (const auto &Op : Block.Ops)
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        Observe(Op.Inputs[I]);
  }
  for (const auto &Call : Function.CallInfos)
    for (const auto &Argument : Call.Args)
      Observe(Argument);
  for (const auto &Clobber : Function.CallClobbers)
    if (Clobber.PreservedPrefixSize)
      Observe(Clobber.PreservedInput);

  for (const auto &Block : Function.Blocks) {
    const MedOp *Pending = nullptr;
    for (const auto &Op : Block.Ops) {
      if (Pending && !Op.Dead && Op.Opcode == NdOp::INDIR_CALL &&
          Op.NumInputs && Op.Inputs[0] == Pending->Output &&
          Op.Inputs[0].Size == kAArch64PAuthPointerBytes) {
        Result.push_back({Pending, &Op});
        Pending = nullptr;
      } else if (isCallAuthentication(Op) && Uses.lookup(key(Op.Output)) == 1) {
        Pending = &Op;
      } else if (Pending) {
        // The BLRAA/B lifter writes the new LR between authentication and
        // transfer. SSA retains the old LR when it supplied either operand.
        // Do not move authentication across a memory effect, another call,
        // control flow, or a mutable/redefined operand.
        if (Op.Opcode != NdOp::COPY || Op.NumInputs != 1 ||
            (Op.Inputs[0].Kind != MedVar::Reg &&
             Op.Inputs[0].Kind != MedVar::Temp &&
             Op.Inputs[0].Kind != MedVar::Param && !Op.Inputs[0].isConst()) ||
            Op.MemoryOrdering != NdMemoryOrdering::None ||
            Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
            (Op.Output.Kind != MedVar::Temp && Op.Output.Kind != MedVar::Reg) ||
            Op.Output == Pending->Inputs[1] || Op.Output == Pending->Inputs[3])
          Pending = nullptr;
      }
    }
  }
  return Result;
}

} // namespace neverd
