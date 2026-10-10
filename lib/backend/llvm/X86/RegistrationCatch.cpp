//===- RegistrationCatch.cpp - PE32 runtime catch object homes -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/Limits.h"
#include "neverd/backend/llvm/X86RegistrationCatch.h"
#include "neverd/backend/llvm/X86RegistrationLayout.h"
#include "neverd/ir/RegistrationState.h"
#include "neverd/loader/ExceptionInfo.h"

namespace neverd {

std::optional<X86RegistrationCatchPlan>
projectX86RegistrationCatch(const ExceptionFunction &EH,
                            const RegistrationStateAnalysis &State,
                            const X86RegistrationFrameLayout &Frame,
                            uint32_t TryIndex, uint32_t CatchIndex) {
  if (!EH.Registration || !EH.Cxx || TryIndex >= EH.Cxx->TryBlocks.size() ||
      CatchIndex >= EH.Cxx->TryBlocks[TryIndex].Handlers.size() ||
      !State.CxxCatchObjectsComplete || !State.RuntimeObjectAccessesComplete ||
      State.CxxCatchObjects.size() > limits::kMaxRegistrationEHRecords ||
      State.RuntimeObjectAccesses.size() > limits::kMaxRegistrationEHStateWork)
    return std::nullopt;
  auto KnownCatch = [&](const auto &Record) {
    return Record.TryIndex < EH.Cxx->TryBlocks.size() &&
           Record.CatchIndex <
               EH.Cxx->TryBlocks[Record.TryIndex].Handlers.size();
  };
  if (!std::all_of(State.CxxCatchObjects.begin(), State.CxxCatchObjects.end(),
                   KnownCatch) ||
      !std::all_of(State.RuntimeObjectAccesses.begin(),
                   State.RuntimeObjectAccesses.end(), KnownCatch))
    return std::nullopt;
  const auto &Catch = EH.Cxx->TryBlocks[TryIndex].Handlers[CatchIndex];
  const RegistrationCxxCatchObject *Found = nullptr;
  for (const auto &Object : State.CxxCatchObjects) {
    if (Object.TryIndex != TryIndex || Object.CatchIndex != CatchIndex)
      continue;
    if (Found)
      return std::nullopt;
    Found = &Object;
  }
  if (!Catch.CatchObjectOffset) {
    if (Found ||
        std::any_of(State.RuntimeObjectAccesses.begin(),
                    State.RuntimeObjectAccesses.end(), [&](const auto &Access) {
                      return Access.TryIndex == TryIndex &&
                             Access.CatchIndex == CatchIndex;
                    }))
      return std::nullopt;
    return X86RegistrationCatchPlan{};
  }
  if (!Found)
    return std::nullopt;
  const auto &Object = *Found;
  if (!Object.ObjectSize || Object.TypeDescriptorVA != Catch.TypeDescriptorVA ||
      Object.FrameOffset != Catch.CatchObjectOffset ||
      Object.Reference != (Catch.Adjectives == 8))
    return std::nullopt;
  const auto SourceOffset =
      EH.Registration->cxxSourceFrameOffset(Object.FrameOffset);
  if (!SourceOffset)
    return std::nullopt;
  const auto Offset = Frame.runtimeOffset(
      *SourceOffset, Object.Reference ? 4 : Object.ObjectSize);
  if (!Offset)
    return std::nullopt;
  return X86RegistrationCatchPlan{
      X86RegistrationCatchHome{*Offset, Object.ObjectSize, Object.Reference}};
}

} // namespace neverd
