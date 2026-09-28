//===- SymWideArithmetic.h - Proved split-word recovery --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SYMBOLIC_SYMWIDEARITHMETIC_H
#define NEVERD_SYMBOLIC_SYMWIDEARITHMETIC_H

#include "neverd/symbolic/SymExpr.h"
#include "neverd/symbolic/SymSynth.h"

namespace neverd::symbolic {

/// Infer packed arithmetic from a two-word result with up to five reconstructed
/// inputs. Input responses only propose coefficients; an equivalence proof is
/// mandatory. Expressions with more than ten distinct word inputs are left
/// unchanged.
SymRef recoverSplitWordArithmetic(SymContext &Ctx, SymRef Expr,
                                  SynthVerifyFn Verify);

} // namespace neverd::symbolic

#endif // NEVERD_SYMBOLIC_SYMWIDEARITHMETIC_H
