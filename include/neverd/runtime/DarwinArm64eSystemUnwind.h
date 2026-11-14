//===- DarwinArm64eSystemUnwind.h - Signal to system cursor -----*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Recover an interrupted frame's authenticated caller for system stack
/// walking or Darwin signal return using a predecoded register-save plan.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_RUNTIME_DARWINARM64ESYSTEMUNWIND_H
#define NEVERD_RUNTIME_DARWINARM64ESYSTEMUNWIND_H

#include "neverd/runtime/DarwinArm64eSignalCursor.h"

#if defined(__APPLE__) && defined(__arm64e__)
#include <libunwind.h>

// Recover from the interrupted stack before changing either native consumer.
static inline bool
ndDarwinArm64eReadCaller(const ucontext_t *Context, NDDarwinArm64eUnwindRow Row,
                         const NDDarwinArm64eSavedGPR *Saved, size_t Count,
                         uintptr_t Low, uintptr_t High, bool HasPAuthLR,
                         NDDarwinArm64eCursor *Caller,
                         uintptr_t Registers[NDArm64ePreservedGPRCount]) {
  if (!Context || !Context->uc_mcontext ||
      Context->uc_mcsize != sizeof(*Context->uc_mcontext))
    return false;
  if (Count > NDArm64ePreservedGPRCount || (Count && !Saved))
    return false;
  *Caller = ndDarwinArm64eSignalCursor(Context);
  if (!ndDarwinArm64eStepFrameWithPAuthLR(Caller, Row, Low, High, HasPAuthLR))
    return false;
  if (Caller->FP &&
      (Caller->FP < Low || Caller->FP > High ||
       NDArm64eFrameRecordWords * sizeof(uintptr_t) > High - Caller->FP))
    return false;
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
        !ndDarwinArm64eStackSlot(Caller->SP, Saved[I].CFAOffset, Low, High,
                                 &Registers[Index]))
      return false;
    Seen |= 1u << Index;
  }
  return true;
}

/// Prepare a genuine SA_SIGINFO context to return from the interrupted call.
/// Return normally from the signal handler after success. Darwin consumes the
/// original context and restores the signal mask, alternate stack and NEON
/// state. The ordinary call ABI requires every D8-D15 low lane to be known.
/// Proven upper-lane saves are restored as well. Other volatile state and FP
/// controls keep their interrupted values. Supply any result registers required
/// by the interrupted call. Language exception matching and cleanup belong to
/// the personality. An invalid plan leaves the kernel context untouched.
static inline bool ndDarwinArm64eResumeSignalCaller(
    ucontext_t *Context, NDDarwinArm64eUnwindRow Row,
    const NDDarwinArm64eSavedGPR *Saved, size_t Count,
    const NDDarwinArm64eVectorSaves *Vectors, uintptr_t Low, uintptr_t High,
    bool HasPAuthLR) {
  NDDarwinArm64eCursor Caller;
  uintptr_t Registers[NDArm64ePreservedGPRCount];
  if (!Vectors || !Vectors->Complete ||
      ((Vectors->LowMask | Vectors->HighMask) >> NDArm64ePreservedDCount) ||
      !ndDarwinArm64eReadCaller(Context, Row, Saved, Count, Low, High,
                                HasPAuthLR, &Caller, Registers) ||
      Caller.SP % NDArm64eStackAlignment ||
      Caller.PC % NDArm64eInstructionAlignment)
    return false;

  arm_neon_state64_t *NEON = &Context->uc_mcontext->__ns;
  __uint128_t Values[NDArm64ePreservedDCount];
  for (unsigned D = 0; D < NDArm64ePreservedDCount; ++D) {
    Values[D] = NEON->__v[NDArm64eFirstPreservedD + D];
    uintptr_t Lanes[2];
    memcpy(Lanes, &Values[D], sizeof(Lanes));
    if (((Vectors->LowMask >> D) & 1u) &&
        !ndDarwinArm64eStackSlot(Caller.SP, Vectors->LowOffsets[D], Low, High,
                                 &Lanes[0]))
      return false;
    if (((Vectors->HighMask >> D) & 1u) &&
        !ndDarwinArm64eStackSlot(Caller.SP, Vectors->HighOffsets[D], Low, High,
                                 &Lanes[1]))
      return false;
    memcpy(&Values[D], Lanes, sizeof(Lanes));
  }

  arm_thread_state64_t *State = &Context->uc_mcontext->__ss;
  for (unsigned I = 0; I < NDArm64ePreservedGPRCount; ++I)
    State->__x[NDArm64eFirstPreservedGPR + I] = Registers[I];
  memcpy(&NEON->__v[NDArm64eFirstPreservedD], Values, sizeof(Values));
  arm_thread_state64_set_sp(*State, Caller.SP);
  arm_thread_state64_set_fp(*State, Caller.FP);
  // The resumed BL has completed. Its architectural LR and the resume PC
  // both name the fallthrough. Darwin signs each field with its own schema.
  void *Target = ptrauth_sign_unauthenticated((void *)Caller.PC,
                                              ptrauth_key_function_pointer, 0);
  arm_thread_state64_set_lr_fptr(*State, Target);
  arm_thread_state64_set_pc_fptr(*State, Target);
  return true;
}

/// Schedule a caller-supplied, nonreturning language throw or rethrow thunk.
/// Return normally from SA_SIGINFO after success. The thunk executes with the
/// restored signal mask and stack, outside the handler. Its return address
/// selects the original protected call in the system personality. Prepare Site
/// and the matching row before the signal, while the image is still loaded.
typedef void (*NDDarwinArm64eExceptionDelivery)(void *);
static inline bool ndDarwinArm64eDeliverSignalException(
    ucontext_t *Context, NDDarwinArm64eExceptionSite Site,
    NDDarwinArm64eUnwindRow Row, const NDDarwinArm64eSavedGPR *Saved,
    size_t Count, const NDDarwinArm64eVectorSaves *Vectors, uintptr_t Low,
    uintptr_t High, bool HasPAuthLR, NDDarwinArm64eExceptionDelivery Deliver,
    void *Payload) {
  if (!Context || !Context->uc_mcontext ||
      Context->uc_mcsize != sizeof(*Context->uc_mcontext) || !Deliver)
    return false;
  arm_thread_state64_t *State = &Context->uc_mcontext->__ss;
  const uintptr_t PC = arm_thread_state64_get_pc(*State);
  if (PC < Site.Begin || PC >= Site.End || PC % NDArm64eInstructionAlignment)
    return false;
  if (Site.AtProtectedCall) {
    // The interrupted frame keeps its original signed return in its spill.
    // The thunk can therefore use LR without destroying that frame's caller.
    NDDarwinArm64eCursor Caller = ndDarwinArm64eSignalCursor(Context);
    if ((Site.ProtectedReturn &&
         Site.ProtectedReturn % NDArm64eInstructionAlignment) ||
        !Row.SavedLR || Site.Begin != PC ||
        Site.End - PC != NDArm64eInstructionAlignment || Caller.SP < Low ||
        Caller.SP > High || Caller.SP % NDArm64eStackAlignment ||
        !ndDarwinArm64eStepFrameWithPAuthLR(&Caller, Row, Low, High,
                                            HasPAuthLR))
      return false;
    void *Return = ptrauth_sign_unauthenticated(
        (void *)(Site.ProtectedReturn ? Site.ProtectedReturn : Site.End),
        ptrauth_key_function_pointer, 0);
    arm_thread_state64_set_lr_fptr(*State, Return);
  } else if (!ndDarwinArm64eResumeSignalCaller(
                 Context, Row, Saved, Count, Vectors, Low, High, HasPAuthLR)) {
    return false;
  }
  State->__x[0] = (uintptr_t)Payload;
  arm_thread_state64_set_pc_fptr(*State, (void *)Deliver);
  return true;
}

/// Move the system stack walker from a Darwin signal trampoline to the
/// authenticated caller of the interrupted frame. System must be the cursor
/// at that trampoline and Context its genuine signal ucontext. Never copy the
/// opaque cursor; its internal pointers can use address diversity.
/// Saved lists each preserved integer register whose row is not same-value.
/// Use ndDarwinArm64eResumeSignalCaller for an executable call return.
/// Invalid rows fail before changing System. Discard it on a libunwind error.
static inline bool ndDarwinArm64eCrossSignalTrampoline(
    unw_cursor_t *System, const ucontext_t *Context,
    NDDarwinArm64eUnwindRow Row, const NDDarwinArm64eSavedGPR *Saved,
    size_t Count, uintptr_t Low, uintptr_t High, bool HasPAuthLR) {
  NDDarwinArm64eCursor Caller;
  uintptr_t Registers[NDArm64ePreservedGPRCount];
  if (!ndDarwinArm64eReadCaller(Context, Row, Saved, Count, Low, High,
                                HasPAuthLR, &Caller, Registers))
    return false;
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
