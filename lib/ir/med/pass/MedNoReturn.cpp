//===- MedNoReturn.cpp - Whole-program no-return propagation ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Proves internal functions do not return without relying on symbol names.
/// A proof requires every reachable path to end at an explicit architectural
/// trap, an already-known no-return call, or a direct call to another proved
/// internal no-return function.  Unknown terminal fragments and unanchored
/// cycles remain conservative.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedNoReturn.h"

#include "neverd/ir/intrinsics/Intrinsics.h"

#include <map>
#include <queue>
#include <set>
#include <vector>

namespace neverd {
namespace {

bool isArchitecturalNoReturn(const MedOp &Op, Arch TheArch) {
  if (Op.Opcode != NdOp::INTRINSIC || Op.NumInputs < 1 ||
      !Op.Inputs[0].isConst())
    return false;

  const auto Id = static_cast<Intrinsic>(Op.Inputs[0].ConstVal);
  if (TheArch == Arch::AArch64)
    return Id == Intrinsic::Brk || Id == Intrinsic::Hlt_A64;
  return false;
}

bool isDirectCallTo(const MedOp &Op, const std::set<va_t> &Targets) {
  return Op.Opcode == NdOp::CALL && Op.NumInputs >= 1 &&
         Op.Inputs[0].isConst() && Targets.count(Op.Inputs[0].ConstVal) != 0;
}

bool provesNoReturn(const MedFunc &Func, Arch TheArch,
                    const std::set<va_t> &KnownNoReturn) {
  if (Func.Blocks.empty())
    return false;

  std::map<int, const MedBlock *> BlocksById;
  for (const MedBlock &Block : Func.Blocks)
    BlocksById.emplace(Block.Id, &Block);

  std::queue<int> Pending;
  std::set<int> Visited;
  Pending.push(Func.Blocks.front().Id);
  bool SawProvenTerminator = false;

  while (!Pending.empty()) {
    const int Id = Pending.front();
    Pending.pop();
    if (!Visited.insert(Id).second)
      continue;

    auto BlockIt = BlocksById.find(Id);
    if (BlockIt == BlocksById.end())
      return false;
    const MedBlock &Block = *BlockIt->second;

    bool PathTerminates = false;
    for (const MedOp &Op : Block.Ops) {
      if (Op.Opcode == NdOp::RETURN)
        return false;
      if ((Op.DoesNotReturn &&
           (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)) ||
          isDirectCallTo(Op, KnownNoReturn) ||
          isArchitecturalNoReturn(Op, TheArch)) {
        SawProvenTerminator = true;
        PathTerminates = true;
        break;
      }
    }

    if (PathTerminates)
      continue;
    if (Block.Succs.empty())
      return false;
    for (int Succ : Block.Succs)
      Pending.push(Succ);
  }

  // A closed cycle with no return may be an infinite loop, but without an
  // explicit terminating fact it is not used as the seed for interprocedural
  // propagation.  This also keeps incomplete cyclic CFGs conservative.
  return SawProvenTerminator;
}

} // namespace

void propagateInternalNoReturn(std::vector<MedFunc> &Funcs, Arch TheArch) {
  std::set<va_t> InternalEntries;
  for (const MedFunc &Func : Funcs)
    if (!Func.Blocks.empty())
      InternalEntries.insert(Func.Entry);

  // A second run follows late ABI remodelling.  Recompute internal markers
  // from the current graph while preserving independently proven external and
  // indirect no-return calls created during LowIR conversion.
  for (MedFunc &Func : Funcs) {
    Func.DoesNotReturn = false;
    for (MedBlock &Block : Func.Blocks)
      for (MedOp &Op : Block.Ops)
        if (Op.Opcode == NdOp::CALL && Op.NumInputs >= 1 &&
            Op.Inputs[0].isConst() &&
            InternalEntries.count(Op.Inputs[0].ConstVal) != 0)
          Op.DoesNotReturn = false;
  }

  std::map<va_t, std::vector<size_t>> CallersByTarget;
  for (size_t FuncIndex = 0; FuncIndex < Funcs.size(); ++FuncIndex)
    for (const MedBlock &Block : Funcs[FuncIndex].Blocks)
      for (const MedOp &Op : Block.Ops)
        if (Op.Opcode == NdOp::CALL && Op.NumInputs >= 1 &&
            Op.Inputs[0].isConst() &&
            InternalEntries.count(Op.Inputs[0].ConstVal) != 0)
          CallersByTarget[Op.Inputs[0].ConstVal].push_back(FuncIndex);

  std::set<va_t> NoReturnEntries;
  std::queue<size_t> Pending;
  std::vector<bool> Queued(Funcs.size(), true);
  for (size_t I = 0; I < Funcs.size(); ++I)
    Pending.push(I);

  while (!Pending.empty()) {
    const size_t FuncIndex = Pending.front();
    Pending.pop();
    Queued[FuncIndex] = false;
    const MedFunc &Func = Funcs[FuncIndex];
    if (NoReturnEntries.count(Func.Entry) != 0 ||
        !provesNoReturn(Func, TheArch, NoReturnEntries))
      continue;

    NoReturnEntries.insert(Func.Entry);
    auto Callers = CallersByTarget.find(Func.Entry);
    if (Callers == CallersByTarget.end())
      continue;
    for (size_t CallerIndex : Callers->second) {
      if (NoReturnEntries.count(Funcs[CallerIndex].Entry) != 0 ||
          Queued[CallerIndex])
        continue;
      Pending.push(CallerIndex);
      Queued[CallerIndex] = true;
    }
  }

  for (MedFunc &Func : Funcs) {
    Func.DoesNotReturn = NoReturnEntries.count(Func.Entry) != 0;
    for (MedBlock &Block : Func.Blocks)
      for (MedOp &Op : Block.Ops)
        if (isDirectCallTo(Op, NoReturnEntries))
          Op.DoesNotReturn = true;
  }
}

} // namespace neverd
