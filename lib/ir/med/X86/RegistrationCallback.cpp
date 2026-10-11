//===- RegistrationCallback.cpp - PE32 callback entry and CFG binding ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/Limits.h"
#include "neverd/ir/med/MedStackAlignment.h"
#include "neverd/ir/med/X86RegistrationCallback.h"
#include "neverd/ir/med/X86RegistrationFrame.h"

#include "llvm/ADT/STLExtras.h"

#include <map>
#include <set>

namespace neverd {
namespace {

bool hasCallbackContract(const MedFunc &Func) {
  if (!Func.ExceptionMetadata || !Func.RegistrationStates || Func.SkippedSSA)
    return false;
  const auto &EH = *Func.ExceptionMetadata;
  const auto &State = *Func.RegistrationStates;
  if (Func.Entry != EH.CodeRange.Begin ||
      EH.ParseStatus != ExceptionParseStatus::Complete || !EH.Registration ||
      EH.Encoding != ExceptionEncoding::X86CxxFuncInfo || !EH.Cxx ||
      EH.Cxx->TryBlocks.size() > limits::kMaxRegistrationEHRecords ||
      !EH.Cxx->hasValidStateGraph() ||
      (EH.Personality != ExceptionPersonality::CxxFrameHandler3 &&
       EH.Personality != ExceptionPersonality::CxxFrameHandlerX86) ||
      !State.Complete || !State.CallbackStatesComplete ||
      !State.RegistrationLifetimeComplete || !State.ChainOperationsComplete ||
      !State.CxxContinuationsComplete ||
      Func.Blocks.size() > limits::kMaxRegistrationEHRecords ||
      State.Blocks.size() > limits::kMaxRegistrationEHRecords ||
      State.CxxContinuations.size() > limits::kMaxRegistrationEHRecords)
    return false;
  return cxxRegistrationFrameCoordinate(EH, &State).has_value();
}

std::optional<std::pair<uint32_t, uint32_t>>
catchIdentity(const CxxExceptionInfo &Cxx, va_t Entry) {
  std::optional<std::pair<uint32_t, uint32_t>> Result;
  size_t Work = 0;
  for (uint32_t I = 0; I < Cxx.TryBlocks.size(); ++I)
    for (uint32_t J = 0; J < Cxx.TryBlocks[I].Handlers.size(); ++J) {
      if (++Work > limits::kMaxRegistrationEHRecords)
        return std::nullopt;
      if (Cxx.TryBlocks[I].Handlers[J].HandlerVA == Entry) {
        if (Result)
          return std::nullopt;
        Result = {I, J};
      }
    }
  return Result;
}

} // namespace

bool isRegistrationCallbackStackRoot(const MedFunc &Func, const MedOp &Op) {
  if (!hasCallbackContract(Func) ||
      Op.RegistrationRoot !=
          MedOp::RegistrationRootKind::CallbackStackPointer ||
      !hasValidRegistrationRootShape(Op) || Op.Dead || Op.Output.Id < 0 ||
      Op.Output.SSAVer <= 0 ||
      !catchIdentity(*Func.ExceptionMetadata->Cxx, Op.Addr))
    return false;
  bool Found = false;
  size_t Work = 0;
  for (const auto &Block : Func.Blocks) {
    for (const auto &Other : Block.Ops) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return false;
      if (Other.Output.Id != Op.Output.Id ||
          Other.Output.SSAVer != Op.Output.SSAVer)
        continue;
      if (&Other != &Op || Found || Block.StartAddr != Op.Addr ||
          !Block.Preds.empty() || Block.ExceptionalPreds.empty() ||
          !llvm::all_of(Block.ExceptionalPreds, [&](const auto &Edge) {
            return Edge.Kind == ExceptionalEdgeKind::CxxCatch &&
                   Edge.TargetVA == Op.Addr;
          }))
        return false;
      Found = true;
    }
    for (const auto &Phi : Block.Phis) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return false;
      if (Phi.Output.Id == Op.Output.Id &&
          Phi.Output.SSAVer == Op.Output.SSAVer)
        return false;
    }
  }
  return Found;
}

std::optional<RegistrationCallbackRegion>
registrationCallbackRegion(const MedFunc &Func, va_t Entry) {
  if (!hasCallbackContract(Func))
    return std::nullopt;
  const auto &EH = *Func.ExceptionMetadata;
  const auto &State = *Func.RegistrationStates;
  const auto Identity = catchIdentity(*EH.Cxx, Entry);
  if (!Identity)
    return std::nullopt;
  const MedBlock *Root = nullptr;
  std::map<int, const MedBlock *> Blocks;
  using RangeKey = std::pair<va_t, va_t>;
  std::map<RangeKey, const RegistrationBlockState *> States;
  for (const auto &Block : Func.Blocks) {
    if (!Blocks.emplace(Block.Id, &Block).second)
      return std::nullopt;
    if (Block.StartAddr == Entry) {
      if (Root)
        return std::nullopt;
      Root = &Block;
    }
  }
  for (const auto &Block : State.Blocks)
    if (!States.emplace(RangeKey{Block.Range.Begin, Block.Range.End}, &Block)
             .second)
      return std::nullopt;
  if (!Root || !Root->Preds.empty())
    return std::nullopt;
  unsigned Stacks = 0, Frames = 0;
  for (const auto &Op : Root->Ops) {
    if (Op.RegistrationRoot ==
        MedOp::RegistrationRootKind::CallbackStackPointer) {
      if (!isRegistrationCallbackStackRoot(Func, Op))
        return std::nullopt;
      ++Stacks;
    } else if (Op.RegistrationRoot != MedOp::RegistrationRootKind::None) {
      if (!registrationRootFrameCoordinate(Func, Op))
        return std::nullopt;
      ++Frames;
    }
  }
  if (Stacks != 1 || Frames != 1)
    return std::nullopt;

  std::set<int> Reached;
  std::vector<int> Worklist{Root->Id};
  size_t Work = 0;
  RegistrationCallbackRegion Region;
  while (!Worklist.empty()) {
    const int ID = Worklist.back();
    Worklist.pop_back();
    if (!Reached.insert(ID).second)
      continue;
    const auto Found = Blocks.find(ID);
    if (++Work > limits::kMaxRegistrationEHStateWork || Found == Blocks.end())
      return std::nullopt;
    const auto &Block = *Found->second;
    const auto Proof = States.find({Block.StartAddr, Block.EndAddr});
    if (Proof == States.end())
      return std::nullopt;
    const auto &Fact = *Proof->second;
    const ExceptionAddressRange Range{Block.StartAddr, Block.EndAddr};
    if (!Range.isValid() || !EH.ownsCode(Range) || !Fact.Reached ||
        Fact.Unknown || !Fact.CallbackOnly || Fact.Range.Begin != Range.Begin ||
        Fact.Range.End != Range.End ||
        (ID != Root->Id && !Block.ExceptionalPreds.empty()))
      return std::nullopt;
    Region.Ranges.push_back(Range);
    bool Exit = false;
    for (const auto &Op : Block.Ops) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return std::nullopt;
      if (Op.Dead)
        continue;
      if (Exit)
        return std::nullopt;
      if (Op.Opcode == NdOp::RETURN) {
        const auto *Resume = State.cxxContinuation(Op.Addr, Op.OriginSeq);
        if (!Resume || Resume->EndAddress != Block.EndAddr ||
            Resume->TryIndex != Identity->first ||
            Resume->CatchIndex != Identity->second || Op.NumInputs != 1 ||
            Op.Inputs[0].Size != 4 || !Op.Inputs[0].isConst() ||
            Op.Inputs[0].ConstVal != Resume->TargetVA)
          return std::nullopt;
        Exit = true;
      } else if (Op.Opcode == NdOp::CALL && Op.DoesNotReturn) {
        const auto *Call = State.callFrameEffect(Op.Addr, Op.OriginSeq);
        if (!State.CallFrameEffectsComplete || !Call || !Call->DoesNotReturn ||
            Call->EndAddress <= Op.Addr || Call->EndAddress > Block.EndAddr ||
            Op.NumInputs != 1 || !Op.Inputs[0].isConst() ||
            Op.Inputs[0].ConstVal != Call->Target)
          return std::nullopt;
        // The decoded block can include unreachable padding after a newly
        // proved noreturn call. MedIR has already removed its fallthrough;
        // no later live operation may survive in this invocation.
        Exit = true;
      }
    }
    if (Exit != Block.Succs.empty())
      return std::nullopt;
    if (Block.Succs.size() > limits::kMaxRegistrationEHStateWork - Work)
      return std::nullopt;
    Work += Block.Succs.size();
    for (int Succ : Block.Succs) {
      const auto Next = Blocks.find(Succ);
      if (Next == Blocks.end() || !llvm::is_contained(Next->second->Preds, ID))
        return std::nullopt;
    }
    Worklist.insert(Worklist.end(), Block.Succs.begin(), Block.Succs.end());
  }
  for (int ID : Reached)
    for (int Pred : Blocks.at(ID)->Preds)
      if (++Work > limits::kMaxRegistrationEHStateWork ||
          !Reached.count(Pred) ||
          !llvm::is_contained(Blocks.at(Pred)->Succs, ID))
        return std::nullopt;
  llvm::sort(Region.Ranges,
             [](const auto &A, const auto &B) { return A.Begin < B.Begin; });
  for (size_t I = 1; I < Region.Ranges.size(); ++I)
    if (Region.Ranges[I - 1].overlaps(Region.Ranges[I]))
      return std::nullopt;
  return Region;
}

} // namespace neverd
