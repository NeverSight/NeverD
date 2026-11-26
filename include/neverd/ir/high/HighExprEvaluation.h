//===- HighExprEvaluation.h - Source operand evaluation --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Evaluation rules for expressions emitted as structured source.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_HIGHEXPREVALUATION_H
#define NEVERD_IR_HIGH_HIGHEXPREVALUATION_H

#include "neverd/ir/high/HighIR.h"

namespace neverd {
inline bool highOperandMayBeSkipped(const HighExpr &E, size_t Index) {
  return Index != 0 && E.Kind == ExprKind::BinOp &&
         (E.Op == NdOp::SELECT || E.Op == NdOp::BOOL_AND ||
          E.Op == NdOp::BOOL_OR);
}
} // namespace neverd

#endif // NEVERD_IR_HIGH_HIGHEXPREVALUATION_H
