//===- PowerPCDivisionSource.h - Native divide projections -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Use the target operation for the quotient. A C or LLVM division could
/// introduce undefined behavior when the divisor is zero or MIN / -1.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_POWERPC_POWERPCDIVISIONSOURCE_H
#define NEVERD_BACKEND_POWERPC_POWERPCDIVISIONSOURCE_H

#include "neverd/backend/PowerPC/PowerPCFixedPointSource.h"
#include "neverd/ir/intrinsics/PowerPC/PowerPCDivision.h"

namespace neverd::ppc {
inline const char *divisionConstraints(Division D) {
  return fixedPointConstraints(D);
}

inline std::string divisionAssembly(Division D, char Operand) {
  return fixedPointAssembly(D, Operand);
}

inline std::string divisionSource(Division D, const std::string &Left,
                                  const std::string &Right,
                                  const std::string &Condition = {}) {
  return fixedPointSource(D, Left, Right, Condition);
}
} // namespace neverd::ppc

#endif // NEVERD_BACKEND_POWERPC_POWERPCDIVISIONSOURCE_H
