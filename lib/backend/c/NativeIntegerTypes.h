//===- NativeIntegerTypes.h - Compiler integer type availability --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Share native integer type availability between the C source emitters.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_NATIVEINTEGERTYPES_H
#define NEVERD_BACKEND_C_NATIVEINTEGERTYPES_H

#include "neverd/Common.h"
#include "neverd/ir/TargetRegInfo.h"

namespace neverd {

/// GNU-compatible C targets provide __int128 with 64-bit pointers, and also
/// with the n32 ABI's 64-bit MIPS registers despite its 32-bit pointers.
inline bool cTargetHasInt128(Arch Target) {
  return isMIPSN32(Target) || getTargetRegInfo(Target).PointerSize >= 8;
}

} // namespace neverd

#endif // NEVERD_BACKEND_C_NATIVEINTEGERTYPES_H
