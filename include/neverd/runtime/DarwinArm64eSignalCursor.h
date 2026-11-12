//===- DarwinArm64eSignalCursor.h - Darwin signal frame returns -*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_RUNTIME_DARWINARM64ESIGNALCURSOR_H
#define NEVERD_RUNTIME_DARWINARM64ESIGNALCURSOR_H

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

typedef enum {
  NDArm64eSigningUnknown,
  NDArm64eUnsigned,
  NDArm64eSignedIAWithCFA,
  NDArm64eSignedIBWithCFA
} NDDarwinArm64eSigning;

/// A resolved instruction-boundary unwind row. A saved register is loaded
/// from CFA + its offset. Otherwise its interrupted register is used.
typedef struct {
  bool CFAFromFP;
  ptrdiff_t CFAOffset;
  bool SavedFP;
  bool SavedLR;
  ptrdiff_t FPOffset;
  ptrdiff_t LROffset;
  NDDarwinArm64eSigning Signing;
} NDDarwinArm64eUnwindRow;

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
static inline bool ndDarwinArm64eStepFrame(NDDarwinArm64eCursor *Cursor,
                                           NDDarwinArm64eUnwindRow Row,
                                           uintptr_t Low, uintptr_t High) {
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
  void *Expected;
  switch (Row.Signing) {
  case NDArm64eUnsigned:
    Expected = PC;
    break;
  case NDArm64eSignedIAWithCFA:
    Expected = ptrauth_sign_unauthenticated(PC, ptrauth_key_asia, CFA);
    break;
  case NDArm64eSignedIBWithCFA:
    Expected = ptrauth_sign_unauthenticated(PC, ptrauth_key_asib, CFA);
    break;
  default:
    return false;
  }
  if ((uintptr_t)Expected != LR)
    return false;
  const NDDarwinArm64eCursor Caller = {(uintptr_t)PC, CFA, FP, 0};
  *Cursor = Caller;
  return true;
}
#endif // defined(__APPLE__) && defined(__arm64e__)

#endif // NEVERD_RUNTIME_DARWINARM64ESIGNALCURSOR_H
