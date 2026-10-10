//===- RegistrationCatch.cpp - PE32 runtime catch object homes -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/X86RegistrationCatch.h"
#include "neverd/backend/llvm/X86RegistrationLayout.h"
#include "neverd/ir/RegistrationState.h"
#include "neverd/loader/ExceptionInfo.h"

namespace neverd {

std::optional<X86RegistrationCatchPlan>
projectX86RegistrationCatch(const ExceptionFunction &EH,
                            const RegistrationStateAnalysis &State,
                            const X86RegistrationFrameLayout &Frame) {
  if (!EH.Registration || !EH.Cxx || EH.Cxx->TryBlocks.size() != 1 ||
      EH.Cxx->TryBlocks[0].Handlers.size() != 1 ||
      !State.CxxCatchObjectsComplete || !State.RuntimeObjectAccessesComplete)
    return std::nullopt;
  const auto &Catch = EH.Cxx->TryBlocks[0].Handlers[0];
  if (!Catch.CatchObjectOffset) {
    if (!State.CxxCatchObjects.empty() || !State.RuntimeObjectAccesses.empty())
      return std::nullopt;
    return X86RegistrationCatchPlan{};
  }
  if (State.CxxCatchObjects.size() != 1)
    return std::nullopt;
  const auto &Object = State.CxxCatchObjects[0];
  if (Object.TryIndex || Object.CatchIndex || !Object.ObjectSize ||
      Object.TypeDescriptorVA != Catch.TypeDescriptorVA ||
      Object.FrameOffset != Catch.CatchObjectOffset ||
      Object.Reference != (Catch.Adjectives == 8))
    return std::nullopt;
  const auto Offset = Frame.runtimeOffset(
      Object.FrameOffset, Object.Reference ? 4 : Object.ObjectSize);
  if (!Offset)
    return std::nullopt;
  return X86RegistrationCatchPlan{
      X86RegistrationCatchHome{*Offset, Object.ObjectSize, Object.Reference}};
}

} // namespace neverd
