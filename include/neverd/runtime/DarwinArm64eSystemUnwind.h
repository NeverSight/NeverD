//===- DarwinArm64eSystemUnwind.h - Signal to system cursor -----*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Resume system stack walking at an authenticated caller of an interrupted
/// frame. The caller predecodes all CFA and saved callee-register rules.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_RUNTIME_DARWINARM64ESYSTEMUNWIND_H
#define NEVERD_RUNTIME_DARWINARM64ESYSTEMUNWIND_H

#include "neverd/runtime/DarwinArm64eSignalCursor.h"

#if defined(__APPLE__) && defined(__arm64e__)
#include <libunwind.h>

/// Move the system stack walker from a Darwin signal trampoline to the
/// authenticated caller of the interrupted frame. System must be the cursor
/// at that trampoline and Context its genuine signal ucontext. Never copy the
/// opaque cursor; its internal pointers can use address diversity.
/// Saved lists each preserved integer register whose row is not same-value.
/// This supports stack walking, not exception resumption or vector recovery.
/// Invalid rows fail before changing System. Discard it on a libunwind error.
static inline bool ndDarwinArm64eCrossSignalTrampoline(
    unw_cursor_t *System, const ucontext_t *Context,
    NDDarwinArm64eUnwindRow Row, const NDDarwinArm64eSavedGPR *Saved,
    size_t Count, uintptr_t Low, uintptr_t High, bool HasPAuthLR) {
  if (Count > NDArm64ePreservedGPRCount || (Count && !Saved))
    return false;
  NDDarwinArm64eCursor Caller = ndDarwinArm64eSignalCursor(Context);
  if (!ndDarwinArm64eStepFrameWithPAuthLR(&Caller, Row, Low, High, HasPAuthLR))
    return false;
  if (Caller.FP &&
      (Caller.FP < Low || Caller.FP > High ||
       NDArm64eFrameRecordWords * sizeof(uintptr_t) > High - Caller.FP))
    return false;
  uintptr_t Registers[NDArm64ePreservedGPRCount];
  for (unsigned I = 0; I < NDArm64ePreservedGPRCount; ++I)
    Registers[I] =
        Context->uc_mcontext->__ss.__x[NDArm64eFirstPreservedGPR + I];
  unsigned Seen = 0;
  for (size_t I = 0; I < Count; ++I) {
    if (Saved[I].Register < NDArm64eFirstPreservedGPR ||
        Saved[I].Register - NDArm64eFirstPreservedGPR >=
            NDArm64ePreservedGPRCount)
      return false;
    const unsigned Index = Saved[I].Register - NDArm64eFirstPreservedGPR;
    if ((Seen & (1u << Index)) ||
        !ndDarwinArm64eStackSlot(Caller.SP, Saved[I].CFAOffset, Low, High,
                                 &Registers[Index]))
      return false;
    Seen |= 1u << Index;
  }
  // Setting IP can apply the old frame's GNU argument-size adjustment. The
  // interrupted row has already established the exact caller SP.
  unw_proc_info_t Info;
  if (unw_get_proc_info(System, &Info) || Info.gp)
    return false;
  if (unw_set_reg(System, UNW_REG_SP, Caller.SP) ||
      unw_set_reg(System, UNW_ARM64_FP, Caller.FP) ||
      unw_set_reg(System, UNW_ARM64_LR, Caller.LR))
    return false;
  for (unsigned I = 0; I < NDArm64ePreservedGPRCount; ++I)
    if (unw_set_reg(System, UNW_ARM64_X19 + I, Registers[I]))
      return false;
  // Public Darwin PC setters consume a return address signed with IB and the
  // cursor's current SP. A function-pointer signature uses a different schema.
  const void *PC = ptrauth_sign_unauthenticated(
      (void *)Caller.PC, ptrauth_key_return_address, Caller.SP);
  unw_word_t SP;
  if (unw_set_reg(System, UNW_REG_IP, (uintptr_t)PC) ||
      unw_get_reg(System, UNW_REG_SP, &SP) || SP != Caller.SP ||
      unw_get_proc_info(System, &Info))
    return false;
  // Darwin's setter looks up an exact PC. If a call ends at the start of
  // another function, that record cannot unwind the call site.
  return Info.start_ip < Caller.PC && Caller.PC <= Info.end_ip;
}
#endif // defined(__APPLE__) && defined(__arm64e__)

#endif // NEVERD_RUNTIME_DARWINARM64ESYSTEMUNWIND_H
