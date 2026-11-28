//===- LowScalarExpression.h - Scalar identities ------------*- C++ -*-===//
//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Prove a numeric result independent of a path's unknown scalar inputs.

#ifndef NEVERD_LIB_IR_LOW_LOWSCALAREXPRESSION_H
#define NEVERD_LIB_IR_LOW_LOWSCALAREXPRESSION_H

#include "neverd/ir/low/LowIR.h"
#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace neverd {

/// Construct scalar semantics for one authenticated straight path. Removed
/// direct edges supply no path predicates. ReadImage may supply only
/// independently proved immutable loads. Unknown writes receive distinct
/// inputs, register bytes overlap, and temporaries are instruction local.
/// The context owns the result; failure supplies no partial expression.
symbolic::SymRef symbolicScalarExpression(
    symbolic::SymContext &Ctx, llvm::ArrayRef<LowOp> Operations,
    const NdVar &Value, va_t QueryInstruction, size_t &RemainingWork,
    llvm::function_ref<std::optional<uint64_t>(const LowOp &, uint64_t)>
        ReadImage = {});

/// Only an exact derivation may publish a constant. The caller authenticates
/// its address role and revalidates changed blocks.
std::optional<uint64_t> constantScalarExpression(symbolic::SymContext &Ctx,
                                                 symbolic::SymRef Expression,
                                                 size_t &RemainingWork);

/// A sorted, complete finite cover of the expression's possible values.
/// Independent child domains may overapproximate correlated operands. No
/// observation or path predicate removes a possible value. The caller must
/// authenticate every covered address and retain the actual runtime selector.
std::optional<std::vector<uint64_t>>
finiteScalarExpression(symbolic::SymContext &Ctx, symbolic::SymRef Expression,
                       size_t &RemainingWork);

} // namespace neverd

#endif // NEVERD_LIB_IR_LOW_LOWSCALAREXPRESSION_H
