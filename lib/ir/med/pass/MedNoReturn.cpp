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
/// internal no-return function, or stay in a completely lifted closed cycle.
/// Unknown terminal fragments and incomplete cycles remain conservative.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedNoReturn.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/SourceCallTypeHint.h"
#include "neverd/ir/intrinsics/IntrinsicTermination.h"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include <map>
#include <optional>
#include <queue>
#include <set>
#include <vector>

namespace neverd {

bool isArchitecturalNoReturn(const MedOp &Op) {
  if (Op.Opcode != NdOp::INTRINSIC || Op.NumInputs < 1 ||
      !Op.Inputs[0].isConst())
    return false;
  return intrinsicNeverReturns(
      static_cast<Intrinsic>(Op.Inputs[0].ConstVal),
      Op.NumInputs >= 2 && Op.Inputs[1].isConst()
          ? std::optional<uint64_t>(Op.Inputs[1].ConstVal)
          : std::nullopt);
}

namespace {
bool isDirectCallTo(const MedOp &Op, const std::set<va_t> &Targets) {
  return Op.Opcode == NdOp::CALL && Op.NumInputs >= 1 &&
         Op.Inputs[0].isConst() && Targets.count(Op.Inputs[0].ConstVal) != 0;
}

bool hasIndependentImportNoReturn(const MedOp &Op, Arch TheArch) {
  // Function discovery also inventories import veneers. Their presence must
  // not revoke a call effect established from an imported declaration before
  // this internal fixed point. Require the complete runtime binding to agree;
  // a source hint alone never introduces the fact here, and inferred native
  // summaries still get refreshed.
  if (!Op.DoesNotReturn || !Op.SourceCallHint ||
      !Op.SourceCallHint->DoesNotReturn)
    return false;
  const auto &Hint = *Op.SourceCallHint;
  switch (Hint.CallKind) {
  case SourceCallTypeHint::Kind::ObjCRuntimeCall:
  case SourceCallTypeHint::Kind::DarwinRuntimeCall:
  case SourceCallTypeHint::Kind::SwiftRuntimeCall:
    break;
  default:
    return false;
  }
  std::string Diagnostic;
  return Hint.Signature.Architecture == TheArch &&
         validateSourceABI(Hint.Signature, Diagnostic) &&
         Hint.Signature.ReturnType->Kind == NdTypeKind::Void &&
         Op.NumInputs == sourceABIParameters(Hint.Signature).size() + 1;
}

/// Whether the control-flow graph ends \p Block at its call \p Op, marked as
/// one that does not return: the call is the block's last operation, and no
/// edge leaves the block.
bool endsItsPath(const MedBlock &Block, const MedOp &Op) {
  return Op.DoesNotReturn && Block.Succs.empty() && !Block.Ops.empty() &&
         &Block.Ops.back() == &Op;
}

bool provesNoReturn(const MedFunc &Func, Arch TheArch,
                    const std::set<va_t> &KnownNoReturn) {
  if (Func.Blocks.empty() ||
      hasUnmodeledRegistrationResumption(Func.ExceptionMetadata,
                                         Func.RegistrationStates))
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
          isDirectCallTo(Op, KnownNoReturn) || isArchitecturalNoReturn(Op)) {
        SawProvenTerminator = true;
        PathTerminates = true;
        break;
      }
    }

    // A no-return call stops ordinary execution, but it can still dispatch
    // to a local handler that resumes this function. Prove those paths too;
    // an external or missing handler leaves the return behavior unknown.
    for (const ExceptionalEdge &Edge : Block.ExceptionalSuccs) {
      if (Edge.BlockId < 0)
        return false;
      Pending.push(Edge.BlockId);
    }
    if (PathTerminates)
      continue;
    if (Block.Succs.empty())
      return false;
    for (int Succ : Block.Succs)
      Pending.push(Succ);
  }

  // A closed cycle with no return is an infinite loop only where the lift left
  // nothing out: an unread byte, a path out of the image or a jump table with
  // unproven targets could leave the cycle where this walk cannot see.
  return SawProvenTerminator || (Func.CompleteLiftCoverage &&
                                 Func.UnsafeIndirectBranchAddresses.empty());
}

} // namespace

bool hasProvenNoReturnExit(const MedFunc &Func, Arch TheArch) {
  return provesNoReturn(Func, TheArch, {});
}

void propagateInternalNoReturn(std::vector<MedFunc> &Funcs, Arch TheArch,
                               const std::map<va_t, bool> &Stated,
                               const std::set<va_t> &ImportVeneers) {
  std::set<va_t> InternalEntries;
  for (const MedFunc &Func : Funcs)
    if (!Func.Blocks.empty() && !ImportVeneers.count(Func.Entry))
      InternalEntries.insert(Func.Entry);

  // A second run follows late ABI remodelling.  Recompute internal markers
  // from the current graph while preserving independently proven external and
  // indirect no-return calls created during LowIR conversion.  A call the
  // control-flow graph ends its path at keeps its marker too: whatever the
  // callee proves, nothing of this function follows it (a call that ends the
  // function's code, after which the compiler placed another path's code).
  for (MedFunc &Func : Funcs) {
    Func.DoesNotReturn = false;
    for (MedBlock &Block : Func.Blocks)
      for (MedOp &Op : Block.Ops)
        if (Op.Opcode == NdOp::CALL && Op.NumInputs >= 1 &&
            Op.Inputs[0].isConst() &&
            InternalEntries.count(Op.Inputs[0].ConstVal) != 0 &&
            !hasIndependentImportNoReturn(Op, TheArch) &&
            !endsItsPath(Block, Op))
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

  // Calls to NoReturnEntries end their blocks; ProvenEntries' bodies never
  // return.  What the user states decides the former, as in the CFG.
  std::set<va_t> NoReturnEntries, ProvenEntries;
  for (const auto &[Entry, Never] : Stated)
    if (Never && InternalEntries.count(Entry))
      NoReturnEntries.insert(Entry);
  const auto statedToReturn = [&](va_t Entry) {
    const auto It = Stated.find(Entry);
    return It != Stated.end() && !It->second;
  };
  std::queue<size_t> Pending;
  std::vector<bool> Queued(Funcs.size(), true);
  for (size_t I = 0; I < Funcs.size(); ++I)
    Pending.push(I);

  while (!Pending.empty()) {
    const size_t FuncIndex = Pending.front();
    Pending.pop();
    Queued[FuncIndex] = false;
    const MedFunc &Func = Funcs[FuncIndex];
    if (ProvenEntries.count(Func.Entry) != 0 ||
        !provesNoReturn(Func, TheArch, NoReturnEntries))
      continue;

    ProvenEntries.insert(Func.Entry);
    if (statedToReturn(Func.Entry) ||
        !NoReturnEntries.insert(Func.Entry).second)
      continue;
    auto Callers = CallersByTarget.find(Func.Entry);
    if (Callers == CallersByTarget.end())
      continue;
    for (size_t CallerIndex : Callers->second) {
      if (ProvenEntries.count(Funcs[CallerIndex].Entry) != 0 ||
          Queued[CallerIndex])
        continue;
      Pending.push(CallerIndex);
      Queued[CallerIndex] = true;
    }
  }

  for (MedFunc &Func : Funcs) {
    Func.DoesNotReturn = ProvenEntries.count(Func.Entry) != 0;
    for (MedBlock &Block : Func.Blocks)
      for (MedOp &Op : Block.Ops) {
        // A following trap belongs to this caller; it cannot establish that
        // the call itself never returns or authorize dropping that trap.
        if (isDirectCallTo(Op, NoReturnEntries))
          Op.DoesNotReturn = true;
        if (Op.Opcode == NdOp::CALL && Op.NumInputs && Op.Inputs[0].isConst() &&
            Op.SourceCallHint &&
            Op.SourceCallHint->CallKind == SourceCallTypeHint::Kind::Native &&
            Op.SourceCallHint->TargetAddress == Op.Inputs[0].ConstVal &&
            InternalEntries.count(Op.Inputs[0].ConstVal)) {
          // HighIR's source-flow owner consumes the bound call effect. Clone
          // it so a later proof refresh cannot mutate another call's contract.
          auto Hint = std::make_shared<SourceCallTypeHint>(*Op.SourceCallHint);
          Hint->DoesNotReturn = Op.DoesNotReturn;
          Op.SourceCallHint = std::move(Hint);
        }
      }
  }
}

} // namespace neverd
