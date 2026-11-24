//===- Arm64eCursorModel.c - Model execution of the Darwin frame cursor ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Run the public arm64e cursor with the Apple SDK and the active EL's stack.
//===----------------------------------------------------------------------===//

#include "AArch64SignalCursorOracle.def"
#include "Arm64eReturnModel.def"

int compareCursorModes(int Enabled, uintptr_t High) {
  const uintptr_t Low = High - RETURN_MODEL_STACK_BYTES;
  for (unsigned Explicit = 0; Explicit != 2; ++Explicit) {
    CursorResults Result = {.Checks = 0};
    const uintptr_t StackAddress = (uintptr_t)&Result;
    check(&Result, StackAddress >= Low && StackAddress < High);
    checkStackRows(&Result);
    checkRecipes(&Result, Explicit != 0, Low, High);
    for (unsigned Key = 0; Key != 2; ++Key) {
      const bool UsesPC = Explicit || Enabled;
      check(&Result, (Result.PCRejected[Key] != 0) == UsesPC);
      check(&Result, (Result.RecipeRejected[Key] != 0) == UsesPC);
    }
    if (Result.FirstFailure)
      return Result.FirstFailure;
  }
  return 0;
}
