//===- PowerPCFixedPointSource.h - Fixed-point projections -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Project the result and CR0 from the same instruction on the target CPU.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_POWERPC_POWERPCFIXEDPOINTSOURCE_H
#define NEVERD_BACKEND_POWERPC_POWERPCFIXEDPOINTSOURCE_H

#include "neverd/ir/intrinsics/PowerPC/PowerPCFixedPoint.h"

#include <string>

namespace neverd::ppc {
inline constexpr char DivisionMarker[] = "neverd.ppc.divide";
inline constexpr char HighProductMarker[] = "neverd.ppc.high_word_product";

inline const char *fixedPointMarker(FixedPoint D) {
  return D.Kind == FixedPointKind::Divide ? DivisionMarker : HighProductMarker;
}
inline const char *fixedPointConstraints(FixedPoint D) {
  return D.Record ? "=r,=r,r,r,~{cr0}" : "=r,r,r";
}

inline std::string fixedPointAssembly(FixedPoint D, char Operand) {
  const std::string Output = std::string(1, Operand) + "0";
  if (!D.Record)
    return std::string(D.Mnemonic) + " " + Output + ", " + Operand + "1, " +
           Operand + "2";
  const std::string CR = std::string(1, Operand) + "1";
  return std::string(D.Mnemonic) + ". " + Output + ", " + Operand + "2, " +
         Operand + "3; mfcr " + CR + "; rlwinm " + CR + ", " + CR + ", 0, 0, 2";
}

inline std::string fixedPointSource(FixedPoint D, const std::string &Left,
                                    const std::string &Right,
                                    const std::string &Condition = {}) {
  const std::string Result =
      D.Kind == FixedPointKind::Divide ? "__nd_quotient" : "__nd_product";
  std::string Text = "(__extension__ ({ __UINTPTR_TYPE__ " + Result;
  if (D.Record)
    Text += ", __nd_condition";
  Text += "; __asm__ volatile (\"" + fixedPointAssembly(D, '%') +
          "\" : \"=r\" (" + Result + ")";
  if (D.Record)
    Text += ", \"=r\" (__nd_condition)";
  Text += " : \"r\" ((__UINTPTR_TYPE__)(" + Left +
          ")), \"r\" ((__UINTPTR_TYPE__)(" + Right + "))";
  if (D.Record)
    Text += " : \"cr0\"";
  Text += "); ";
  if (!Condition.empty())
    Text += Condition + " = __nd_condition; ";
  return Text + Result + "; }))";
}
} // namespace neverd::ppc

#endif // NEVERD_BACKEND_POWERPC_POWERPCFIXEDPOINTSOURCE_H
