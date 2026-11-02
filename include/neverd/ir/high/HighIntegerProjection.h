//===- HighIntegerProjection.h - Observed integer bits --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Project an integer expression onto the bits its consumer observes. Changed
/// paths are copied; unobserved calls, memory accesses and trapping operations
/// retain their evaluation. The original expression remains available to
/// consumers that observe more bits.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_HIGHINTEGERPROJECTION_H
#define NEVERD_IR_HIGH_HIGHINTEGERPROJECTION_H

#include "neverd/ir/high/HighIR.h"

#include <cstdint>

namespace neverd {

ExprPtr projectHighIntegerBits(const ExprPtr &Value, uint64_t Demand);

} // namespace neverd

#endif // NEVERD_IR_HIGH_HIGHINTEGERPROJECTION_H
