//===- DarwinArm64eSignalCursor.h - Signal frame returns -*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Authenticate Darwin signal-context returns using resolved rows and stack
/// bounds.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_RUNTIME_DARWINARM64ESIGNALCURSOR_H
#define NEVERD_RUNTIME_DARWINARM64ESIGNALCURSOR_H

#include "neverd/runtime/DarwinArm64eUnwindRow.h"

#if defined(__APPLE__) && defined(__arm64e__)
#include <mach/arm/thread_status.h>
#include <ptrauth.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/ucontext.h>

/// Register state at the exact interrupted PC, or at an authenticated return.
typedef struct {
  uintptr_t PC;
  uintptr_t SP;
  uintptr_t FP;
  uintptr_t LR;
} NDDarwinArm64eCursor;

#define NEVERD_ARM64E_SIGNER(Name, Asm, Pointer, Modifier, PC, Output, Input)  \
  static inline uintptr_t Name(uintptr_t Address, uintptr_t CFA,               \
                               uintptr_t SigningPC) {                          \
    register uintptr_t Value __asm__(Pointer) = Address;                       \
    register uintptr_t Mod __asm__(Modifier) = CFA;                            \
    register uintptr_t Mod2 __asm__(PC) = SigningPC;                           \
    __asm__ __volatile__(Asm : Output(Value) : Input(Mod), Input(Mod2));       \
    return Value;                                                              \
  }
#include "neverd/runtime/DarwinArm64eSignatures.def"

static inline NDDarwinArm64eCursor
ndDarwinArm64eSignalCursor(const ucontext_t *Context) {
  const arm_thread_state64_t *State = &Context->uc_mcontext->__ss;
  NDDarwinArm64eCursor Cursor = {
      arm_thread_state64_get_pc(*State), arm_thread_state64_get_sp(*State),
      arm_thread_state64_get_fp(*State), arm_thread_state64_get_lr(*State)};
  return Cursor;
}

static inline bool ndDarwinArm64eStackSlot(uintptr_t CFA, ptrdiff_t Offset,
                                           uintptr_t Low, uintptr_t High,
                                           uintptr_t *Value) {
  uintptr_t Address;
  if (__builtin_add_overflow(CFA, Offset, &Address) || Address < Low ||
      Address > High || sizeof(*Value) > High - Address)
    return false;
  memcpy(Value, (const void *)Address, sizeof(*Value));
  return true;
}

/// Step using a predecoded row and the bounds of the mapped interrupted stack.
/// Compare a fresh hardware signature before publishing the canonical PC. A
/// mismatched PAC returns false even on processors that trap on failed AUT.
/// The caller's live LR cannot be recovered from the callee's return address.
/// HasPAuthLR must be established before entering a signal handler. Explicit
/// CFAPC recipes require FEAT_PAuth_LR; PACM recipes retain the original hint
/// semantics on older CPUs. Both require the interrupted thread's PAC keys and
/// enable controls. Never substitute one recipe for the other.
static inline bool
ndDarwinArm64eStepFrameWithPAuthLR(NDDarwinArm64eCursor *Cursor,
                                   NDDarwinArm64eUnwindRow Row, uintptr_t Low,
                                   uintptr_t High, bool HasPAuthLR) {
  uintptr_t CFA;
  if (__builtin_add_overflow(Row.CFAFromFP ? Cursor->FP : Cursor->SP,
                             Row.CFAOffset, &CFA) ||
      CFA < Low || CFA > High)
    return false;
  uintptr_t FP = Cursor->FP, LR = Cursor->LR;
  if (Row.SavedFP &&
      !ndDarwinArm64eStackSlot(CFA, Row.FPOffset, Low, High, &FP))
    return false;
  if (Row.SavedLR &&
      !ndDarwinArm64eStackSlot(CFA, Row.LROffset, Low, High, &LR))
    return false;
  void *PC = ptrauth_strip((void *)LR, ptrauth_key_return_address);
  if (!PC)
    return false;
  uintptr_t Expected;
  switch (Row.Signing) {
  case NDArm64eUnsigned:
    Expected = (uintptr_t)PC;
    break;
  case NDArm64eSignedIAWithCFA:
    Expected =
        (uintptr_t)ptrauth_sign_unauthenticated(PC, ptrauth_key_asia, CFA);
    break;
  case NDArm64eSignedIBWithCFA:
    Expected =
        (uintptr_t)ptrauth_sign_unauthenticated(PC, ptrauth_key_asib, CFA);
    break;
  case NDArm64eSignedIAWithCFAPC:
    if (!HasPAuthLR)
      return false;
    Expected = ndDarwinArm64eSignExplicitIA((uintptr_t)PC, CFA, Row.SigningPC);
    break;
  case NDArm64eSignedIBWithCFAPC:
    if (!HasPAuthLR)
      return false;
    Expected = ndDarwinArm64eSignExplicitIB((uintptr_t)PC, CFA, Row.SigningPC);
    break;
  case NDArm64eSignedIAWithCFAPACM:
    Expected = ndDarwinArm64eSignPACMIA((uintptr_t)PC, CFA, Row.SigningPC);
    break;
  case NDArm64eSignedIBWithCFAPACM:
    Expected = ndDarwinArm64eSignPACMIB((uintptr_t)PC, CFA, Row.SigningPC);
    break;
  default:
    return false;
  }
  if (Expected != LR)
    return false;
  const NDDarwinArm64eCursor Caller = {(uintptr_t)PC, CFA, FP, 0};
  *Cursor = Caller;
  return true;
}

/// Baseline entry point: explicit PAuth_LR is unavailable until qualified.
static inline bool ndDarwinArm64eStepFrame(NDDarwinArm64eCursor *Cursor,
                                           NDDarwinArm64eUnwindRow Row,
                                           uintptr_t Low, uintptr_t High) {
  return ndDarwinArm64eStepFrameWithPAuthLR(Cursor, Row, Low, High, false);
}
#endif // defined(__APPLE__) && defined(__arm64e__)

#endif // NEVERD_RUNTIME_DARWINARM64ESIGNALCURSOR_H
