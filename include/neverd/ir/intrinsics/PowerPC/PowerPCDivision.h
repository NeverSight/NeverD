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

#include "neverd/Common.h"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include <optional>

namespace neverd::ppc {
struct Division {
  Intrinsic Id;
  unsigned Bytes;
  bool Signed;
  const char *Mnemonic;
  bool Record;

  bool admits(Arch A) const {
    return isPowerPC(A) && (Bytes == 4 || isPowerPC64(A));
  }
  bool matches(Arch A, unsigned Output, unsigned LHS, unsigned RHS) const {
    return admits(A) && Output == (isPowerPC64(A) ? 8u : 4u) && LHS == Output &&
           RHS == Output;
  }
};

inline std::optional<Division> division(Intrinsic Code) {
  switch (Code) {
#define NEVERD_PPC_DIVIDE(Id, Value, RecordId, RecordValue, XO, Bytes, Signed, \
                          Mnemonic)                                            \
  case Intrinsic::Id:                                                          \
    return Division{Intrinsic::Id, Bytes, Signed, Mnemonic, false};            \
  case Intrinsic::RecordId:                                                    \
    return Division{Intrinsic::RecordId, Bytes, Signed, Mnemonic, true};
#include "neverd/ir/intrinsics/PowerPC/PowerPCDivision.def"
  default:
    return std::nullopt;
  }
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
