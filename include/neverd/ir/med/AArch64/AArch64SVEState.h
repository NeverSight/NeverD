//===- AArch64SVEState.h - Local scalable state admission ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared MedIR admission for local SVE values. This proves complete local
/// definitions and permitted transport, not a native scalable calling ABI.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_AARCH64_AARCH64SVESTATE_H
#define NEVERD_IR_MED_AARCH64_AARCH64SVESTATE_H

#include "neverd/Common.h"

namespace neverd {
struct MedFunc;

/// Null means that no unsupported local scalable state was found. Both source
/// conversion and native LLVM emission must check before consuming MedIR.
const char *validateAArch64SVEState(const MedFunc &Func, Arch Architecture);

} // namespace neverd

#endif // NEVERD_IR_MED_AARCH64_AARCH64SVESTATE_H
