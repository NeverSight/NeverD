//===- LibCCallDeclaration.h - Call declaration trust ------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared provenance and calling-convention admission for fixed call adapters.
/// Each placement adapter validates the complete parameter and result types.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIBC_LIBCCALLDECLARATION_H
#define NEVERD_LIBC_LIBCCALLDECLARATION_H

#include "neverd/debug/DebugContext.h"
#include "neverd/debug/UserPrototype.h"

namespace neverd::libc {

inline bool
hasFixedPlatformCallDeclaration(const FunctionSym &Function,
                                const DebugContext *NativeDebug = nullptr) {
  if (!Function.PlatformConvention || Function.IsVariadic ||
      !Function.ParamEntryLocations.empty() ||
      Function.CallConv == DebugCallConv::Fastcall ||
      Function.CallConv == DebugCallConv::Thiscall)
    return false;
  if (Function.PrototypeFromHeader || Function.PrototypeFromCLibrary ||
      Function.PrototypeLibrary == UserPrototypeLibrary)
    return true;
  // Names alone and missing native return contracts establish no complete
  // declaration. The caller must resolve the exact entry in this image.
  return NativeDebug && NativeDebug->hasAuthenticatedFunctionSignatures() &&
         NativeDebug->resolveAuthenticatedReturnValueState(Function.Addr)
                 .Kind != AuthenticatedReturnKind::Unknown;
}

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCCALLDECLARATION_H
