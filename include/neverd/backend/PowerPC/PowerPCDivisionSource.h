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

#include "neverd/ir/intrinsics/PowerPC/PowerPCDivision.h"

#include <string>

namespace neverd::ppc {
inline constexpr char DivisionMarker[] = "neverd.ppc.divide";
inline const char *divisionConstraints(Division D) {
  return D.Record ? "=r,=r,r,r,~{cr0}" : "=r,r,r";
}

inline std::string divisionAssembly(Division D, char Operand) {
  const std::string Q = std::string(1, Operand) + "0";
  if (!D.Record)
    return std::string(D.Mnemonic) + " " + Q + ", " + Operand + "1, " +
           Operand + "2";
  const std::string CR = std::string(1, Operand) + "1";
  return std::string(D.Mnemonic) + ". " + Q + ", " + Operand + "2, " + Operand +
         "3; mfcr " + CR + "; rlwinm " + CR + ", " + CR + ", 0, 0, 2";
}

inline std::string divisionSource(Division D, const std::string &Left,
                                  const std::string &Right,
                                  const std::string &Condition = {}) {
  std::string Text = "(__extension__ ({ __UINTPTR_TYPE__ __nd_quotient";
  if (D.Record)
    Text += ", __nd_condition";
  Text += "; __asm__ volatile (\"" + divisionAssembly(D, '%') +
          "\" : \"=r\" (__nd_quotient)";
  if (D.Record)
    Text += ", \"=r\" (__nd_condition)";
  Text += " : \"r\" ((__UINTPTR_TYPE__)(" + Left +
          ")), \"r\" ((__UINTPTR_TYPE__)(" + Right + "))";
  if (D.Record)
    Text += " : \"cr0\"";
  Text += "); ";
  if (!Condition.empty())
    Text += Condition + " = __nd_condition; ";
  return Text + "__nd_quotient; }))";
}
} // namespace neverd::ppc

#endif // NEVERD_BACKEND_POWERPC_POWERPCDIVISIONSOURCE_H
