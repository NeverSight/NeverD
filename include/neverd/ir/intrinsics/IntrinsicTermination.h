//===- IntrinsicTermination.h - Shared architectural termination -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_INTRINSICTERMINATION_H
#define NEVERD_IR_INTRINSICS_INTRINSICTERMINATION_H

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/intrinsics/X86Interrupts.h"

#include <optional>

namespace neverd {

/// Architectural termination, shared by LowIR and MedIR. This grants no
/// source ABI or memory contract to an intrinsic.
inline bool intrinsicNeverReturns(Intrinsic Id,
                                  std::optional<uint64_t> Operand) {
  switch (Id) {
  case Intrinsic::Brk:
  case Intrinsic::Hlt_A64:
  case Intrinsic::Udf_A64:
    return true;
  case Intrinsic::IntN:
    return Operand && isX86NoReturnInterrupt(*Operand);
  default:
    return false;
  }
}

} // namespace neverd

#endif
