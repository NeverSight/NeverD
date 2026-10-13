//===- SymSimplify.h - Symbolic normalization ------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SYMBOLIC_SYMSIMPLIFY_H
#define NEVERD_SYMBOLIC_SYMSIMPLIFY_H

#include "neverd/symbolic/SymExpr.h"

namespace neverd::symbolic {

struct ExpressionSimplifyOptions {
  unsigned Nodes = 4096;
  unsigned Edges = 16384;
  unsigned Work = 65536;
  unsigned Rounds = 3;
};

/// Normalize total bitvector expressions without LLVM IR or path assumptions.
/// Rules preserve the complete value and all independent input identities.
/// Incomplete DAG inventories retain the input; exhausted optional work keeps
/// the last completely rewritten expression. The context owns every result.
SymRef simplifyExpression(SymContext &Context, SymRef Root,
                          ExpressionSimplifyOptions Options = {});

} // namespace neverd::symbolic

#endif
