//===- AArch64ExceptionReturn.h - Exception-return shape --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// ERET is a terminal transfer to ELR with SPSR as PSTATE, as x86's IRET: its
/// intrinsic reads no operand besides its ID and defines no value.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_AARCH64_AARCH64EXCEPTIONRETURN_H
#define NEVERD_IR_INTRINSICS_AARCH64_AARCH64EXCEPTIONRETURN_H

#include "neverd/ir/intrinsics/Intrinsics.h"

namespace neverd {

inline constexpr char kAArch64ExceptionReturnMalformed[] =
    "AArch64 exception return reads no operand and defines no value";

/// \p NumInputs counts the intrinsic ID. Other intrinsics are not checked.
inline bool aarch64ExceptionReturnShapeIsValid(Intrinsic Id, unsigned NumInputs,
                                               unsigned OutputBytes,
                                               unsigned ExtraOutputs = 0) {
  return Id != Intrinsic::Eret ||
         (NumInputs == 1 && OutputBytes == 0 && ExtraOutputs == 0);
}

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_AARCH64_AARCH64EXCEPTIONRETURN_H
