//===- RegistrationStateObjectCall.cpp - PE32 exception object borrows ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationStateSolver.h"

#include "neverd/lift/X86/X86Regs.h"

namespace neverd::registration_state {
bool RegistrationStateSolver::bindRuntimeObjectBorrow(
    const Domain &State, const RegistrationCalleeFrameContract &Callee,
    RegistrationCallFrameEffect &Effect) {
  const auto &Object =
      State.RuntimeObject.Registers[x86reg::RCX / x86reg::GeneralRegStride];
  const auto Found = Object.ExceptionObject
                         ? CatchObjects.find(*Object.ExceptionObject)
                         : CatchObjects.end();
  if (!CheckRuntimeObjects ||
      Callee.CalleeKind != RegistrationCalleeFrameContract::Kind::Leaf ||
      Found == CatchObjects.end() || !Found->second.Reference ||
      !Object.Offset || *Object.Offset < 0 ||
      uint32_t(*Object.Offset) > Found->second.ObjectSize ||
      !runtimeObjectIsLive(State, Found->first))
    return false;
  // The CRT owns initialized, pointer-free scalar storage. A borrowed member
  // may mutate that allocation but cannot initialize or alias parent bytes.
  for (const auto *Extents : {&Callee.ECXReads, &Callee.ECXWrites})
    for (const auto &Extent : *Extents) {
      const int64_t Begin = int64_t(*Object.Offset) + Extent.Begin;
      const int64_t End = int64_t(*Object.Offset) + Extent.End;
      if (!charge(1) || Begin < 0 || End <= Begin ||
          uint64_t(End) > Found->second.ObjectSize)
        return false;
    }
  Effect.ECXRuntimeObject = {Found->first.first, Found->first.second,
                             *Object.Offset};
  return true;
}
} // namespace neverd::registration_state
