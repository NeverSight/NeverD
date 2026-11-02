//===- AArch64ExceptionReturn.h - Exception-return admission ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exception return needs an ELR/SPSR/PSTATE contract. The legacy intrinsic
/// cannot describe that state transfer and must not reach ordinary emitters.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_AARCH64_AARCH64EXCEPTIONRETURN_H
#define NEVERD_IR_INTRINSICS_AARCH64_AARCH64EXCEPTIONRETURN_H

#include "neverd/ir/intrinsics/Intrinsics.h"

namespace neverd {

#define NEVERD_A64_EXCEPTION_RETURN_TEXT(Name, Text)                           \
  inline constexpr char Name[] = Text;
#include "neverd/ir/intrinsics/AArch64/AArch64ExceptionReturn.def"

inline bool isAArch64ExceptionReturnIntrinsic(Intrinsic Id) {
  switch (Id) {
#define NEVERD_A64_EXCEPTION_RETURN_INTRINSIC(Id, Insn)                        \
  case Intrinsic::Id:                                                          \
    return true;
#include "neverd/ir/intrinsics/AArch64/AArch64ExceptionReturn.def"
  default:
    return false;
  }
}

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_AARCH64_AARCH64EXCEPTIONRETURN_H
