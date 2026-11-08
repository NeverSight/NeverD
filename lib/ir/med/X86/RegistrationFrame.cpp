//===- RegistrationFrame.cpp - x86 runtime frame coordinates -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedStackAlignment.h"
#include "neverd/ir/med/X86/X86RegistrationFrame.h"
#include "neverd/ir/med/X86/X86RegistrationPhysicalFrame.h"

#include "llvm/ADT/STLExtras.h"

namespace neverd {

bool hasValidRegistrationRootShape(const MedOp &Op) {
  const auto &TRI = getTargetRegInfo(Arch::X86);
  if (Op.Opcode != NdOp::COPY || Op.NumInputs != 1 || Op.OriginSeq != -1 ||
      Op.Output.Kind != MedVar::Reg || Op.Output.TheArch != Arch::X86 ||
      Op.Output.Size != 4 || Op.Inputs[0].Kind != MedVar::Reg ||
      Op.Inputs[0].TheArch != Arch::X86 || Op.Inputs[0].Size != 4 ||
      Op.Inputs[0].RegOff != Op.Output.RegOff ||
      Op.Inputs[0].Id != Op.Output.Id || Op.Inputs[0].SSAVer != 0 ||
      Op.Inputs[0].RenameTag != Op.Output.RenameTag ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      !Op.IntrinsicOutputs.empty() || Op.CallSiteId || Op.SourceCallHint ||
      Op.DoesNotReturn)
    return false;
  switch (Op.RegistrationRoot) {
  case MedOp::RegistrationRootKind::EstablishedFramePointer:
  case MedOp::RegistrationRootKind::RealignedFramePointer:
  case MedOp::RegistrationRootKind::DisplacedFramePointer:
    return Op.Output.RegOff == TRI.FramePointer &&
           Op.RegistrationStackOffset == 0;
  case MedOp::RegistrationRootKind::DisplayFramePointer:
    return Op.Output.RegOff == TRI.FramePointer;
  case MedOp::RegistrationRootKind::CallbackStackPointer:
    return Op.Output.RegOff == TRI.StackPointer &&
           Op.RegistrationStackOffset == 0;
  case MedOp::RegistrationRootKind::RestoredCallbackStackPointer:
    return Op.Output.RegOff == TRI.StackPointer &&
           Op.RegistrationStackOffset <= 0 &&
           Op.RegistrationStackOffset >=
               -int64_t(limits::kMaxRegistrationEHStateWork);
  case MedOp::RegistrationRootKind::RestoredStackPointer:
  case MedOp::RegistrationRootKind::RealignedRestoredStackPointer:
    return Op.Output.RegOff == TRI.StackPointer &&
           Op.RegistrationStackOffset <= -16 &&
           int64_t(Op.RegistrationStackOffset) - 4 >= INT32_MIN;
  case MedOp::RegistrationRootKind::None:
    return false;
  }
  return false;
}

bool isRegistrationDisplayRuntimeReturn(const MedFunc &Func, va_t Address) {
  if (!Func.RegistrationStates || !Func.RegistrationStates->Display ||
      !Func.ExceptionMetadata || !Func.ExceptionMetadata->Registration)
    return false;
  const RegistrationDisplayFacts &Facts = *Func.RegistrationStates->Display;
  if (Facts.FinallyReturns.count(Address))
    return true;
  const auto After =
      llvm::upper_bound(Facts.Stretches, Address,
                        [](va_t A, const RegistrationDisplayFacts::Stretch &S) {
                          return A < S.Range.Begin;
                        });
  if (After == Facts.Stretches.begin())
    return false;
  const auto &Stretch = *std::prev(After);
  return Stretch.Owner && Stretch.Range.contains(Address) &&
         llvm::any_of(Func.ExceptionMetadata->Registration->Scopes,
                      [&](const RegistrationScopeRecord &Scope) {
                        return !Scope.IsFinally &&
                               Scope.FilterVA == Stretch.Owner;
                      });
}

std::optional<int64_t> registrationRootEntryStackOffset(const MedOp &Op) {
  if (!hasValidRegistrationRootShape(Op))
    return std::nullopt;
  switch (Op.RegistrationRoot) {
  case MedOp::RegistrationRootKind::EstablishedFramePointer:
    return -4;
  case MedOp::RegistrationRootKind::DisplayFramePointer:
    return Op.RegistrationStackOffset;
  case MedOp::RegistrationRootKind::RestoredStackPointer:
    return int64_t(Op.RegistrationStackOffset) - 4;
  case MedOp::RegistrationRootKind::CallbackStackPointer:
  case MedOp::RegistrationRootKind::RestoredCallbackStackPointer:
  case MedOp::RegistrationRootKind::RealignedFramePointer:
  case MedOp::RegistrationRootKind::RealignedRestoredStackPointer:
  case MedOp::RegistrationRootKind::None:
  case MedOp::RegistrationRootKind::DisplacedFramePointer:
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<RegistrationFrameCoordinate>
realignedRegistrationFrameCoordinate(const ExceptionFunction &EH,
                                     const RegistrationStateAnalysis *State) {
  if (!EH.Registration || !EH.Registration->RealignedFrame ||
      EH.ParseStatus != ExceptionParseStatus::Complete ||
      EH.Encoding != ExceptionEncoding::X86CxxFuncInfo || !EH.Cxx ||
      !EH.Cxx->hasValidStateGraph() ||
      (EH.Personality != ExceptionPersonality::CxxFrameHandler3 &&
       EH.Personality != ExceptionPersonality::CxxFrameHandlerX86) ||
      !State || !State->Complete || !State->CallbackStatesComplete ||
      !State->RegistrationLifetimeComplete || !State->ChainOperationsComplete ||
      !State->CxxContinuationsComplete)
    return std::nullopt;
  const auto &Chain = *EH.Registration;
  const auto &Frame = *Chain.RealignedFrame;
  const bool StackBased = Frame.BaseRegister == 4;
  if (Chain.RegistrationOffset != -12 || Chain.TryLevelOffset != -4 ||
      Chain.SeededTryLevel != -1 || (!StackBased && Frame.BaseRegister != 6) ||
      Frame.Alignment < limits::kMinRegistrationFrameAlignment ||
      Frame.Alignment > limits::kMaxRegistrationFrameAlignment ||
      (Frame.Alignment & (Frame.Alignment - 1)) ||
      Frame.AllocationBytes > limits::kMaxSourceCallBorrowedBytes)
    return std::nullopt;
  if (StackBased) {
    if (!EH.Cxx->TryBlocks.empty() || EH.CodeRange.Begin > UINT32_MAX - 8 ||
        (Frame.DefinitionVA != EH.CodeRange.Begin + 3 &&
         Frame.DefinitionVA != EH.CodeRange.Begin + 5) ||
        Frame.AllocationBytes < 12 ||
        int64_t(Frame.BaseOffset) != -int64_t(Frame.AllocationBytes) ||
        Frame.SavedParentFrameOffset != 0)
      return std::nullopt;
    return RegistrationFrameCoordinate{-4, Frame.Alignment, 0};
  }
  if (EH.CodeRange.Begin > UINT32_MAX - 17 ||
      Frame.DefinitionVA != EH.CodeRange.Begin + 15 ||
      Frame.AllocationBytes < 20 || Frame.BaseOffset > -20 ||
      int64_t(Frame.BaseOffset) + Frame.AllocationBytes < 0 ||
      Frame.SavedParentFrameOffset != -20)
    return std::nullopt;
  // LowIR authenticated four pushes before the alignment operation. ESI is
  // the allocated base; the runtime's EBP is -BaseOffset bytes above it.
  return RegistrationFrameCoordinate{
      -16, Frame.Alignment,
      int32_t(-int64_t(Frame.AllocationBytes) - Frame.BaseOffset)};
}

static std::optional<RegistrationFrameCoordinate>
describeCxxFrame(const ExceptionFunction &EH,
                 const RegistrationStateAnalysis *State) {
  if (!EH.Registration || !EH.Registration->cxxRuntimeFrameOffset() ||
      EH.ParseStatus != ExceptionParseStatus::Complete ||
      EH.Encoding != ExceptionEncoding::X86CxxFuncInfo || !EH.Cxx ||
      !EH.Cxx->hasValidStateGraph() ||
      (EH.Personality != ExceptionPersonality::CxxFrameHandler3 &&
       EH.Personality != ExceptionPersonality::CxxFrameHandlerX86) ||
      !State || !State->Complete || !State->CallbackStatesComplete ||
      !State->RegistrationLifetimeComplete || !State->ChainOperationsComplete ||
      !State->CxxContinuationsComplete)
    return std::nullopt;
  if (EH.Registration->RealignedFrame)
    return realignedRegistrationFrameCoordinate(EH, State);
  return RegistrationFrameCoordinate{-4, 1, 0};
}

std::optional<RegistrationFrameCoordinate>
cxxRegistrationFrameCoordinate(const ExceptionFunction &EH,
                               const RegistrationStateAnalysis *State) {
  if (State && State->CxxPrologue)
    return std::nullopt;
  return describeCxxFrame(EH, State);
}

std::optional<RegistrationFrameCoordinate>
cxxRegistrationFrameCoordinate(const MedFunc &Function) {
  if (!Function.ExceptionMetadata || !Function.RegistrationStates ||
      !getX86RegistrationCxxChainAccesses(Function))
    return std::nullopt;
  return describeCxxFrame(*Function.ExceptionMetadata,
                          &*Function.RegistrationStates);
}

std::optional<RegistrationFrameCoordinate>
registrationRootFrameCoordinate(const MedFunc &Func, const MedOp &Op) {
  if (!hasValidRegistrationRootShape(Op))
    return std::nullopt;
  if (const auto Offset = registrationRootEntryStackOffset(Op))
    return RegistrationFrameCoordinate{int32_t(*Offset), 1, 0};
  if (Op.RegistrationRoot !=
          MedOp::RegistrationRootKind::RealignedFramePointer &&
      Op.RegistrationRoot !=
          MedOp::RegistrationRootKind::RealignedRestoredStackPointer &&
      Op.RegistrationRoot != MedOp::RegistrationRootKind::DisplacedFramePointer)
    return std::nullopt;
  if (!Func.ExceptionMetadata ||
      Func.Entry != Func.ExceptionMetadata->CodeRange.Begin ||
      !Func.RegistrationStates ||
      Func.RegistrationStates->CxxContinuations.size() >
          limits::kMaxRegistrationEHRecords)
    return std::nullopt;
  auto Coordinate = cxxRegistrationFrameCoordinate(Func);
  if (!Coordinate)
    return std::nullopt;
  const auto &EH = *Func.ExceptionMetadata;
  const bool Displaced =
      Op.RegistrationRoot == MedOp::RegistrationRootKind::DisplacedFramePointer;
  if (Displaced) {
    if (EH.Registration->RealignedFrame ||
        EH.Registration->cxxRuntimeFrameOffset().value_or(0) == 0)
      return std::nullopt;
    Coordinate->AlignedOffset = *EH.Registration->cxxRuntimeFrameOffset();
  } else if (!EH.Registration->RealignedFrame)
    return std::nullopt;
  const bool FramePointer =
      Displaced ||
      Op.RegistrationRoot == MedOp::RegistrationRootKind::RealignedFramePointer;
  bool RuntimeEntry = false;
  if (FramePointer) {
    for (const auto &Try : EH.Cxx->TryBlocks)
      for (const auto &Handler : Try.Handlers)
        RuntimeEntry |= Handler.HandlerVA == Op.Addr;
    for (const auto &Unwind : EH.Cxx->UnwindMap)
      RuntimeEntry |= Unwind.ActionVA == Op.Addr;
  }
  for (const auto &Resume : Func.RegistrationStates->CxxContinuations)
    if (Resume.TargetVA == Op.Addr) {
      if (!FramePointer &&
          (Resume.SavedCallbackVA ||
           Resume.SavedStackOffset != Op.RegistrationStackOffset))
        return std::nullopt;
      RuntimeEntry = true;
    }
  const int64_t Offset =
      int64_t(Coordinate->AlignedOffset) + Op.RegistrationStackOffset;
  if (!RuntimeEntry || Offset < INT32_MIN || Offset > INT32_MAX)
    return std::nullopt;
  Coordinate->AlignedOffset = int32_t(Offset);
  return Coordinate;
}

} // namespace neverd
