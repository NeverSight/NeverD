//===- PowerPCDivision.h - PowerPC integer divide forms ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// PowerPC division defines its low quotient on ordinary inputs and leaves
/// parts of the destination unspecified on other inputs or processor levels.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_POWERPC_POWERPCDIVISION_H
#define NEVERD_IR_INTRINSICS_POWERPC_POWERPCDIVISION_H

#include "neverd/ir/intrinsics/PowerPC/PowerPCFixedPoint.h"

namespace neverd::ppc {
using Division = FixedPoint;

inline std::optional<Division> division(Intrinsic Code) {
  const auto Form = fixedPoint(Code);
  if (Form && Form->Kind == FixedPointKind::Divide)
    return Form;
  return std::nullopt;
}

inline std::optional<Division> divisionByOpcode(unsigned Opcode,
                                                bool Record = false) {
  switch (Opcode) {
#define NEVERD_PPC_DIVIDE(Id, Value, RecordId, RecordValue, XO, ...)           \
  case XO:                                                                     \
    return division(Record ? Intrinsic::RecordId : Intrinsic::Id);
#include "neverd/ir/intrinsics/PowerPC/PowerPCDivision.def"
  default:
    return std::nullopt;
  }
}
} // namespace neverd::ppc

#endif // NEVERD_IR_INTRINSICS_POWERPC_POWERPCDIVISION_H
