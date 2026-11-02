//===- OperationLimits.h - Machine-operation representation --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Capacity shared by LowIR construction and intrinsic result emission.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_OPERATIONLIMITS_H
#define NEVERD_IR_OPERATIONLIMITS_H

#include <cstddef>

namespace neverd {
#define NEVERD_OPERATION_LIMIT(Name, Value)                                    \
  inline constexpr size_t Name = Value;
#define NEVERD_OPERATION_TEXT(Name, Value) inline constexpr char Name[] = Value;
#include "neverd/ir/OperationLimits.def"
} // namespace neverd

#endif // NEVERD_IR_OPERATIONLIMITS_H
