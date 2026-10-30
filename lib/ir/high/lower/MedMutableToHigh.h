//===- MedMutableToHigh.h - Explicit mutable HighIR lowering ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_LOWER_MEDMUTABLETOHIGH_H
#define NEVERD_IR_HIGH_LOWER_MEDMUTABLETOHIGH_H

#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/med/MedIR.h"

#include <functional>

namespace neverd::detail {

/// Lower the validated mutable scalar subset without SSA substitutions or
/// control-flow restructuring. Unsupported inputs throw before publication.
HighFunc
lowerMedMutableToHigh(const MedFunc &Med, Arch Architecture,
                      const std::function<void(const MedOp &, const ExprPtr &)>
                          &ObserveExpression,
                      const std::function<void(const MedOp &, const HighStmt &)>
                          &ObserveStatement);

} // namespace neverd::detail

#endif
