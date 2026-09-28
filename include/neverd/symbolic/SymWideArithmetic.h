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

/// Search a two-word result for arithmetic on two reconstructed inputs.
/// The search only proposes candidates: an equivalence proof is mandatory.
/// Expressions with more than four distinct word inputs are left unchanged.
SymRef recoverSplitWordArithmetic(SymContext &Ctx, SymRef Expr,
                                  SynthVerifyFn Verify);

} // namespace neverd::symbolic

#endif // NEVERD_SYMBOLIC_SYMWIDEARITHMETIC_H
