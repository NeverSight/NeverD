//===- HighExprUseCounts.h - Bounded use counts -----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared use-counting support for HighIR copy and control-flow cleanup.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_HIGHEXPRUSECOUNTS_H
#define NEVERD_IR_HIGH_HIGHEXPRUSECOUNTS_H

#include "neverd/ir/high/HighIR.h"

#include "llvm/ADT/DenseMap.h"

#include <cstdint>

namespace neverd {

/// Count a shared expression once per use site, saturating at two. Reuse
/// Visits across read roots in the same immutable graph to bound the complete
/// walk by its nodes and edges without mistaking multiple uses for one.
void countExprVarUsesUpToTwo(const ExprPtr &E, VarKeyMap<int> &Uses,
                             llvm::DenseMap<const HighExpr *, uint8_t> &Visits);

} // namespace neverd

#endif // NEVERD_IR_HIGH_HIGHEXPRUSECOUNTS_H
