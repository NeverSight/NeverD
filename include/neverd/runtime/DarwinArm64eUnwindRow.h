//===- DarwinArm64eUnwindRow.h - Predecoded stack-walk rows -----*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_RUNTIME_DARWINARM64EUNWINDROW_H
#define NEVERD_RUNTIME_DARWINARM64EUNWINDROW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  NDArm64eSigningUnknown,
  NDArm64eUnsigned,
  NDArm64eSignedIAWithCFA,
  NDArm64eSignedIBWithCFA,
  NDArm64eSignedIAWithCFAPC,
  NDArm64eSignedIBWithCFAPC,
  NDArm64eSignedIAWithCFAPACM,
  NDArm64eSignedIBWithCFAPACM
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
  /// The resolved signing instruction address, including a valid zero address.
  /// Used only by the CFAPC and CFAPACM recipes.
  uintptr_t SigningPC;
} NDDarwinArm64eUnwindRow;

enum {
#define NEVERD_SYSTEM_UNWIND_VALUE(Name, Value) Name = Value,
#include "neverd/runtime/DarwinArm64eSystemUnwind.def"
};

typedef struct {
  unsigned Register; // Architectural X19 through X28.
  ptrdiff_t CFAOffset;
} NDDarwinArm64eSavedGPR;

/// D8-D15 hold the ABI-preserved low halves of V8-V15. A proven Q save
/// also supplies its upper half. Offsets are relative to the caller's CFA.
typedef struct {
  bool Complete;
  unsigned LowMask, HighMask;
  ptrdiff_t LowOffsets[NDArm64ePreservedDCount];
  ptrdiff_t HighOffsets[NDArm64ePreservedDCount];
} NDDarwinArm64eVectorSaves;

/// One predecoded range for delivering a language exception after sigreturn.
/// A protected call keeps its own frame. A forwarding range restores its
/// caller and uses the interrupted callee's real architectural return address.
typedef struct {
  uintptr_t Begin, End;
  bool AtProtectedCall;
} NDDarwinArm64eExceptionSite;

#endif // NEVERD_RUNTIME_DARWINARM64EUNWINDROW_H
