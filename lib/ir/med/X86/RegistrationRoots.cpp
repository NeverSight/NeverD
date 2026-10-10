//===- RegistrationRoots.cpp - x86 SSA runtime definitions ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationRoots.h"

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/X86RegistrationFrame.h"

#include "llvm/ADT/STLExtras.h"

namespace neverd::x86_registration {

RegistrationRoots::RegistrationRoots(const LowFunc &Low, Arch Architecture,
                                     BinaryFormat Format)
    : Low(Low) {
  if (Architecture != Arch::X86 || !Low.ExceptionMetadata ||
      Low.ExceptionMetadata->ParseStatus != ExceptionParseStatus::Complete)
    return;
  const auto &EH = *Low.ExceptionMetadata;
  HasSEHFrame = EH.Encoding == ExceptionEncoding::X86ScopeTableEH3 ||
                EH.Encoding == ExceptionEncoding::X86ScopeTableEH4;
  HasCxxFrame = Format == BinaryFormat::COFF &&
                EH.Encoding == ExceptionEncoding::X86CxxFuncInfo && EH.Cxx &&
                EH.Cxx->hasValidStateGraph() &&
                (EH.Personality == ExceptionPersonality::CxxFrameHandler3 ||
                 EH.Personality == ExceptionPersonality::CxxFrameHandlerX86);
  if (Format != BinaryFormat::COFF || !hasFrame() || !EH.Registration)
    return;
  const auto &Chain = *EH.Registration;
  const auto *State =
      Low.RegistrationStates ? &*Low.RegistrationStates : nullptr;
  Direct =
      !Chain.RealignedFrame &&
      (HasCxxFrame
           ? (Chain.RegistrationOffset == -12 && Chain.TryLevelOffset == -4) ||
                 cxxRegistrationFrameCoordinate(EH, State).has_value()
           : Chain.RegistrationOffset == -16 && Chain.TryLevelOffset == -4);
  Realigned = HasCxxFrame && Low.Entry == EH.CodeRange.Begin &&
              realignedRegistrationFrameCoordinate(EH, State).has_value();
  if (!(Direct || Realigned) || !HasCxxFrame || !State ||
      !State->CxxContinuationsComplete ||
      State->CxxContinuations.size() > limits::kMaxRegistrationEHRecords)
    return;
  for (const auto &Resume : State->CxxContinuations) {
    const bool Valid =
        EH.CodeRange.contains(Resume.TargetVA) &&
        Resume.SavedStackOffset <= int64_t(*Chain.RegistrationOffset) - 4;
    auto [It, New] =
        RestoredStacks.emplace(Resume.TargetVA, Resume.SavedStackOffset);
    if (!Valid || (!New && It->second != Resume.SavedStackOffset))
      It->second.reset();
  }
}

void RegistrationRoots::disconnectNoReturnFallthroughs(MedFunc &Func) const {
  if (!(Direct || Realigned) || !HasCxxFrame || !Low.RegistrationStates ||
      !Low.RegistrationStates->Complete ||
      !Low.RegistrationStates->CallFrameEffectsComplete ||
      !Low.RegistrationStates->RegistrationLifetimeComplete)
    return;
  for (auto &Block : Func.Blocks) {
    if (Block.Id < 0 || size_t(Block.Id) >= Low.Blocks.size())
      continue;
    const auto &Source = Low.Blocks[Block.Id];
    if (Source.StartAddr != Block.StartAddr || Source.EndAddr != Block.EndAddr)
      continue;
    for (size_t I = 0; I < Block.Ops.size(); ++I) {
      const auto &Op = Block.Ops[I];
      if (Op.Opcode != NdOp::CALL || !Op.DoesNotReturn || Op.NumInputs != 1 ||
          !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 4)
        continue;
      const auto *Effect =
          Low.RegistrationStates->callFrameEffect(Op.Addr, Op.OriginSeq);
      const auto Boundary =
          llvm::find_if(Source.InstructionBoundaries,
                        [&](const auto &B) { return B.Address == Op.Addr; });
      if (!Effect || !Effect->DoesNotReturn ||
          Effect->Target != Op.Inputs[0].ConstVal ||
          Boundary == Source.InstructionBoundaries.end() ||
          Boundary->Control != LowInstructionControl::Call ||
          Boundary->Immediate != Effect->Target ||
          Effect->EndAddress != Boundary->Address + Boundary->Size ||
          Effect->EndAddress <= Op.Addr || Effect->EndAddress > Block.EndAddr ||
          llvm::any_of(Block.Succs, [&](int Succ) {
            return Succ < 0 || size_t(Succ) >= Func.Blocks.size();
          }))
        continue;
      for (int Succ : Block.Succs)
        std::erase(Func.Blocks[Succ].Preds, Block.Id);
      Block.Succs.clear();
      // CFG decoding can retain instructions after a source-proved terminal
      // call, for example LLVM's jump to an unreachable normal destination.
      // They cannot define SSA values or ordinary successors. Keep the source
      // interval, exceptional edges and independently rooted resume blocks.
      Block.Ops.resize(I + 1);
      break;
    }
  }
}

bool RegistrationRoots::isCxxHandler(const MedBlock &Block) const {
  return HasCxxFrame && Block.StartAddr != Low.Entry &&
         !Low.OrdinaryModuleAnalysisRoots.count(Block.StartAddr) &&
         llvm::any_of(Block.ExceptionalPreds, [](const ExceptionalEdge &Edge) {
           return Edge.Kind == ExceptionalEdgeKind::CxxCatch ||
                  Edge.Kind == ExceptionalEdgeKind::CxxCleanup;
         });
}

std::optional<int32_t>
RegistrationRoots::restoredStackOffset(const MedBlock &Block) const {
  if (Block.StartAddr == Low.Entry || !Block.Preds.empty() ||
      Low.OrdinaryModuleAnalysisRoots.count(Block.StartAddr))
    return std::nullopt;
  const auto It = RestoredStacks.find(Block.StartAddr);
  return It == RestoredStacks.end() ? std::nullopt : It->second;
}

void RegistrationRoots::initialize(MedOp &Op, bool RuntimeRoot, bool Callback,
                                   std::optional<int32_t> RestoredSP) const {
  const auto &Value = Op.Output;
  if (!(Direct || Realigned) || !RuntimeRoot || Value.Kind != MedVar::Reg ||
      Value.Size != 4)
    return;
  const auto &TRI = getTargetRegInfo(Arch::X86);
  if (Value.RegOff == TRI.FramePointer) {
    Op.RegistrationRoot =
        Realigned ? MedOp::RegistrationRootKind::RealignedFramePointer
                  : MedOp::RegistrationRootKind::EstablishedFramePointer;
    if (HasCxxFrame &&
        Low.ExceptionMetadata->Registration->cxxRuntimeFrameOffset().value_or(
            0))
      Op.RegistrationRoot = MedOp::RegistrationRootKind::DisplacedFramePointer;
  } else if (Value.RegOff == TRI.StackPointer && RestoredSP) {
    Op.RegistrationRoot =
        Realigned ? MedOp::RegistrationRootKind::RealignedRestoredStackPointer
                  : MedOp::RegistrationRootKind::RestoredStackPointer;
    Op.RegistrationStackOffset = *RestoredSP;
  } else if (Value.RegOff == TRI.StackPointer && Callback)
    Op.RegistrationRoot = MedOp::RegistrationRootKind::CallbackStackPointer;
}

} // namespace neverd::x86_registration
