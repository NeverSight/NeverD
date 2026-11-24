//===- CLogicalCondition.h - Group composed C conditions --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Join printed branch conditions after forwarding or inversion.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_CLOGICALCONDITION_H
#define NEVERD_BACKEND_C_CLOGICALCONDITION_H

#include <string>
#include <utility>

namespace neverd {

inline std::string joinLogicalConditions(std::string Left, std::string Right,
                                         bool IsAnd) {
  const char *Nested = IsAnd ? "||" : "&&";
  if (Left.find(Nested) != std::string::npos)
    Left = "(" + std::move(Left) + ")";
  if (Right.find(Nested) != std::string::npos)
    Right = "(" + std::move(Right) + ")";
  return std::move(Left) + (IsAnd ? " && " : " || ") + Right;
}

} // namespace neverd

#endif // NEVERD_BACKEND_C_CLOGICALCONDITION_H
