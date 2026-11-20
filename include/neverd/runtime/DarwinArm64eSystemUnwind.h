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
#include <mach-o/compact_unwind_encoding.h>
#include <unwind.h>

#define NEVERD_SYSTEM_UNWIND_BRIDGE(Name, Symbol, Assembly)                    \
  void Name(void) __asm__(Symbol);                                             \
  __asm__(Assembly);
#define NEVERD_SYSTEM_UNWIND_PERSONALITY(Name, Symbol)                         \
  _Unwind_Reason_Code Name(                                                    \
      int, _Unwind_Action, uint64_t, struct _Unwind_Exception *,               \
      struct _Unwind_Context *) __asm__(Symbol) __attribute__((weak));
#include "neverd/runtime/DarwinArm64eSystemUnwind.def"

// Darwin's level-one unwinder passes its public cursor as _Unwind_Context.
// Advance the artificial bridge before consulting the original personality,
// retaining the real language frame and its exception argument registers.
_Unwind_Reason_Code ndDarwinArm64eLiveLRPersonality(
    int Version, _Unwind_Action Actions, uint64_t ExceptionClass,
    struct _Unwind_Exception *Exception, struct _Unwind_Context *Context) {
  unw_cursor_t *System = (unw_cursor_t *)Context;
  const _Unwind_Reason_Code Failure = Actions & _UA_SEARCH_PHASE
                                          ? _URC_FATAL_PHASE1_ERROR
                                          : _URC_FATAL_PHASE2_ERROR;
  unw_word_t BridgeSP, BridgePC, CallerLR, SystemLR, InterruptedSP,
      VirtualReturn;
  if (unw_get_reg(System, UNW_REG_SP, &BridgeSP) ||
      unw_get_reg(System, UNW_REG_IP, &BridgePC) || unw_step(System) <= 0 ||
      unw_get_reg(System, UNW_ARM64_LR, &SystemLR) ||
      unw_get_reg(System, NDArm64eCallerLRRegister, &CallerLR) ||
      unw_get_reg(System, UNW_REG_SP, &InterruptedSP) ||
      unw_get_reg(System, UNW_REG_IP, &VirtualReturn))
    return Failure;
  unw_proc_info_t Info;
  if (unw_get_proc_info(System, &Info) || !Info.handler)
    return Failure;
  // Reading the SDK's restricted integer authenticates the handler field.
  // Supply the ordinary function-pointer schema without an integer cast to
  // a function pointer, whose signature conversion differs between Clangs.
  const uintptr_t Handler = (uintptr_t)ptrauth_sign_unauthenticated(
      (void *)(uintptr_t)Info.handler, ptrauth_key_function_pointer, 0);
  _Unwind_Personality_Fn Personality;
  memcpy(&Personality, &Handler, sizeof(Personality));
  const _Unwind_Reason_Code Result =
      Personality(Version, Actions, ExceptionClass, Exception, Context);
  if (Result == _URC_HANDLER_FOUND) {
    // Phase one records this SP after the callback; phase two compares it
    // before the callback. Keep the bridge identity for a local catch.
    if (unw_set_reg(System, UNW_REG_SP, BridgeSP) ||
        unw_set_reg(System, UNW_REG_IP, BridgePC) ||
        unw_set_reg(System, UNW_REG_SP, BridgeSP))
      return Failure;
  } else if (Result == _URC_INSTALL_CONTEXT) {
    const uintptr_t LandingPad = _Unwind_GetIP(Context);
    unw_word_t SP;
    if (unw_get_reg(System, UNW_REG_SP, &SP))
      return Failure;
    _Unwind_SetGR(Context, NDArm64eCallerLRRegister, CallerLR);
    _Unwind_SetGR(Context, NDArm64eSystemLRRegister, SystemLR);
    _Unwind_SetGR(Context, NDArm64eInterruptedSPRegister, InterruptedSP);
    _Unwind_SetGR(Context, NDArm64eResumeReturnRegister, VirtualReturn);
    _Unwind_SetGR(Context, NDArm64eLandingPadRegister,
                  (uintptr_t)ptrauth_sign_unauthenticated(
                      (void *)LandingPad, ptrauth_key_function_pointer, 0));
    void *Resume = ptrauth_strip((void *)&ndDarwinArm64eLiveLRResume,
                                 ptrauth_key_function_pointer);
    const uintptr_t PC = (uintptr_t)ptrauth_sign_unauthenticated(
        Resume, ptrauth_key_return_address, SP);
    // Setting IP can apply GNU argument-size adjustment again. The original
    // personality has already selected the landing pad's exact stack state.
    if (unw_set_reg(System, UNW_REG_IP, PC) ||
        unw_set_reg(System, UNW_REG_SP, SP))
      return Failure;
  }
  return Result;
}

/// Initialize from the kernel's state before the first instruction. A null
/// RequiredMode selects the recipe from Darwin's registered table.
static inline bool ndDarwinArm64eInitRegisteredEntry(
    unw_cursor_t *System, const ucontext_t *Context,
    const uintptr_t *RequiredEntry, const uint32_t *RequiredMode,
    unsigned *LookupOffset) {
  if (!System || !Context || !Context->uc_mcontext ||
      Context->uc_mcsize != sizeof(*Context->uc_mcontext) || !LookupOffset)
    return false;
  const arm_thread_state64_t *Regs = &Context->uc_mcontext->__ss;
  const uintptr_t Entry = arm_thread_state64_get_pc(*Regs);
  if (!Entry || Entry % NDArm64eInstructionAlignment ||
      (RequiredEntry && Entry != *RequiredEntry))
    return false;
  const uintptr_t SP = arm_thread_state64_get_sp(*Regs);
  const uintptr_t LR = arm_thread_state64_get_lr(*Regs);
  if (!SP || SP % NDArm64eStackAlignment || !LR ||
      LR != (uintptr_t)ptrauth_strip((void *)LR, ptrauth_key_return_address))
    return false;
  unw_context_t Local;
  if (unw_getcontext(&Local) || unw_init_local(System, &Local) ||
      unw_set_reg(System, UNW_REG_SP, SP) ||
      unw_set_reg(System, UNW_ARM64_FP, arm_thread_state64_get_fp(*Regs)))
    return false;
  for (unsigned I = 0; I < NDArm64ePreservedGPRCount; ++I)
    if (unw_set_reg(System, UNW_ARM64_X19 + I,
                    Regs->__x[NDArm64eFirstPreservedGPR + I]))
      return false;
  for (unsigned I = 0; I < NDArm64ePreservedDCount; ++I) {
    unw_fpreg_t Value;
    memcpy(&Value, &Context->uc_mcontext->__ns.__v[NDArm64eFirstPreservedD + I],
           sizeof(Value));
    if (unw_set_fpreg(System, UNW_ARM64_D8 + I, Value))
      return false;
  }
  // Darwin's cursor stores live LR in its return-address representation.
  // Use only the incoming LR from the kernel's verified register state.
  const uintptr_t SystemLR = (uintptr_t)ptrauth_sign_unauthenticated(
      (void *)LR, ptrauth_key_return_address, SP);
  if (unw_set_reg(System, UNW_ARM64_LR, SystemLR))
    return false;
  for (unsigned Offset = 0; Offset <= NDArm64eEntryLookupInteriorByte;
       ++Offset) {
    const uintptr_t IP = (uintptr_t)ptrauth_sign_unauthenticated(
        (void *)(Entry + Offset), ptrauth_key_return_address, SP);
    unw_proc_info_t Info;
    if (unw_set_reg(System, UNW_REG_IP, IP) ||
        unw_set_reg(System, UNW_REG_SP, SP) || unw_get_proc_info(System, &Info))
      return false;
    const uint32_t Mode = Info.format & UNWIND_ARM64_MODE_MASK;
    if (Info.start_ip != Entry || Info.end_ip <= Entry ||
        Info.end_ip - Entry < NDArm64eInstructionAlignment ||
        (RequiredMode && Mode != *RequiredMode) ||
        (Mode != UNWIND_ARM64_MODE_DWARF &&
         Mode != UNWIND_ARM64_MODE_FRAMELESS))
      return false;
    // The personality index and LSDA flag select language metadata. The
    // remaining bits must describe a function start with live LR, no saved
    // registers and no stack adjustment at the interrupted instruction.
    if (Mode == UNWIND_ARM64_MODE_FRAMELESS) {
      if ((Info.format & ~(UNWIND_PERSONALITY_MASK | UNWIND_HAS_LSDA)) !=
          UNWIND_ARM64_MODE_FRAMELESS)
        return false;
      *LookupOffset = Offset;
      return true;
    }
    if (Info.unwind_info && Info.unwind_info_size) {
      *LookupOffset = Offset;
      return true;
    }
    // Some systems return the compact DWARF index with an empty FDE at
    // its inclusive start. The next byte is inside the same A64 instruction.
    if (Info.unwind_info || Info.unwind_info_size)
      return false;
  }
  return false;
}

/// Use the interrupted PC and the system's original table to select the entry
/// row. On failure discard the cursor. LookupOffset is written on success.
static inline bool ndDarwinArm64eInitInterruptedEntryCursor(
    unw_cursor_t *System, const ucontext_t *Context, unsigned *LookupOffset) {
  return ndDarwinArm64eInitRegisteredEntry(System, Context, NULL, NULL,
                                           LookupOffset);
}

/// Require the original system table to have a specific recipe at Entry.
static inline bool ndDarwinArm64eInitTableEntryCursor(unw_cursor_t *System,
                                                      const ucontext_t *Context,
                                                      uintptr_t Entry,
                                                      uint32_t Mode,
                                                      unsigned *LookupOffset) {
  if (Mode != UNWIND_ARM64_MODE_DWARF && Mode != UNWIND_ARM64_MODE_FRAMELESS)
    return false;
  return ndDarwinArm64eInitRegisteredEntry(System, Context, &Entry, &Mode,
                                           LookupOffset);
}

/// Use the original indexed FDE at an interrupted function entry.
static inline bool
ndDarwinArm64eInitIndexedEntryCursor(unw_cursor_t *System,
                                     const ucontext_t *Context, uintptr_t Entry,
                                     unsigned *LookupOffset) {
  return ndDarwinArm64eInitTableEntryCursor(
      System, Context, Entry, UNWIND_ARM64_MODE_DWARF, LookupOffset);
}

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
/// selects the original protected instruction in the system personality.
/// Prepare Site and the matching row before the signal, while the image is
/// still loaded.
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
    NDDarwinArm64eCursor Caller = ndDarwinArm64eSignalCursor(Context);
    const uintptr_t InterruptedSP = Caller.SP;
    if ((Site.ProtectedReturn &&
         Site.ProtectedReturn % NDArm64eInstructionAlignment) ||
        Site.Begin != PC || Site.End - PC != NDArm64eInstructionAlignment ||
        Caller.SP < Low || Caller.SP > High ||
        Caller.SP % NDArm64eStackAlignment ||
        (!Row.SavedLR && Caller.SP - Low < NDArm64eLiveLRBridgeBytes) ||
        !ndDarwinArm64eStepFrameWithPAuthLR(&Caller, Row, Low, High,
                                            HasPAuthLR))
      return false;
    if (!Row.SavedLR) {
      // Preserve the genuine LR and SP in the signal context. The bridge
      // allocates its frame after sigreturn and restores LR independently of
      // the virtual protected edge. Nothing writes below interrupted SP here.
      State->__x[NDArm64eDeliveryRegister] = (uintptr_t)Deliver;
      // Authenticate with the recovered recipe before adapting the cursor's
      // separate LR column to Darwin's implicit IB/CFA unwind ABI. The bridge
      // retains the original LR for executable landing-pad restoration.
      State->__x[NDArm64eSystemLRRegister] =
          (uintptr_t)ptrauth_sign_unauthenticated(
              (void *)Caller.PC, ptrauth_key_return_address, Caller.SP);
      State->__x[NDArm64eVirtualReturnRegister] =
          (uintptr_t)ptrauth_sign_unauthenticated(
              (void *)(Site.ProtectedReturn ? Site.ProtectedReturn : Site.End),
              ptrauth_key_return_address, InterruptedSP);
      State->__x[0] = (uintptr_t)Payload;
      arm_thread_state64_set_pc_fptr(*State,
                                     (void *)&ndDarwinArm64eLiveLRBridge);
      return true;
    }
    // A spilled original return lets the thunk use LR directly.
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
