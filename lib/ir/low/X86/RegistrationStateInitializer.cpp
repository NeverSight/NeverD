//===- RegistrationStateInitializer.cpp - PE32 construction effects ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind scalar constructor arguments and normal-return initialization.
//===----------------------------------------------------------------------===//

#include "RegistrationStateSolver.h"

#include "neverd/lift/X86/X86Regs.h"

namespace neverd::registration_state {

bool RegistrationStateSolver::transferInitializer(
    Domain &After, const RegistrationCalleeFrameContract &Callee,
    FrameValue &StackPointer, FrameValue &Object) {
  Object = After.Frame.Registers[x86reg::RCX / x86reg::GeneralRegStride];
  if (!Object.Offset || Object.FrameOnlyFromCall || Object.ReturnPC)
    return false;
  if (!transferScalarArguments(After, Callee, StackPointer, Object.Offset))
    return false;
  // May-writes were already invalidated by transferCall. Only bytes written
  // on every normal return can discharge pointer taint or initialize storage.
  for (const auto &Range : Callee.ECXInitialized) {
    const int32_t Begin = int32_t(int64_t(*Object.Offset) + Range.Begin);
    const int32_t End = int32_t(int64_t(*Object.Offset) + Range.End);
    if (!charge(size_t(End - Begin) + After.Frame.cellCount()))
      return false;
    for (int64_t Byte = Begin; Byte < End; ++Byte)
      After.InitializedFrameBytes.insert(int32_t(Byte));
    for (auto It = After.Frame.Cells.begin(); It != After.Frame.Cells.end();)
      if (Begin <= It->first && int64_t(It->first) + 4 <= End)
        It = After.Frame.Cells.erase(It);
      else
        ++It;
  }
  return true;
}

} // namespace neverd::registration_state
