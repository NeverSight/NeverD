//===- PowerPCFixedPoint.h - Native integer arithmetic forms ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Identify PowerPC operations whose register result and recorded condition
/// must come from one execution of the target instruction.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_POWERPC_POWERPCFIXEDPOINT_H
#define NEVERD_IR_INTRINSICS_POWERPC_POWERPCFIXEDPOINT_H

#include "neverd/Common.h"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include <optional>

namespace neverd::ppc {
enum class FixedPointKind { Divide, HighWordProduct };

struct FixedPoint {
  Intrinsic Id;
  unsigned Bytes;
  bool Signed;
  const char *Mnemonic;
  bool Record;
  FixedPointKind Kind = FixedPointKind::Divide;

  bool admits(Arch A) const {
    return isPowerPC64(A) ||
           (Kind == FixedPointKind::Divide && Bytes == 4 && isPowerPC(A));
  }
  bool matches(Arch A, unsigned Output, unsigned LHS, unsigned RHS) const {
    return admits(A) && Output == (isPowerPC64(A) ? 8u : 4u) && LHS == Output &&
           RHS == Output;
  }
};

inline std::optional<FixedPoint> fixedPoint(Intrinsic Code) {
  switch (Code) {
#define NEVERD_PPC_FIXED_POINT(Kind, Id, Value, RecordId, RecordValue, Bytes,  \
                               Signed, Mnemonic)                               \
  case Intrinsic::Id:                                                          \
  case Intrinsic::RecordId:                                                    \
    return FixedPoint{Code,                                                    \
                      Bytes,                                                   \
                      Signed,                                                  \
                      Mnemonic,                                                \
                      Code == Intrinsic::RecordId,                             \
                      FixedPointKind::Kind};
#include "neverd/ir/intrinsics/PowerPC/PowerPCFixedPoint.def"
  default:
    return std::nullopt;
  }
}
} // namespace neverd::ppc

#endif // NEVERD_IR_INTRINSICS_POWERPC_POWERPCFIXEDPOINT_H
