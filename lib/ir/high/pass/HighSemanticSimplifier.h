//===- HighSemanticSimplifier.h - Scoped semantic search work -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Reuses identical unsuccessful candidate searches across the semantic
/// cleanup rounds of one HighIR conversion. No reaching definitions,
/// expression pointers or context-local symbolic values cross rounds.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_PASS_HIGHSEMANTICSIMPLIFIER_H
#define NEVERD_IR_HIGH_PASS_HIGHSEMANTICSIMPLIFIER_H

#include "neverd/ir/high/HighIR.h"

#include <memory>
#include <vector>

namespace neverd::detail {

class HighSemanticSimplifier {
  struct State;
  std::unique_ptr<State> Work;

public:
  HighSemanticSimplifier();
  ~HighSemanticSimplifier();

  void simplify(std::vector<HighStmt> &Stmts, unsigned MaxThreads);
};

} // namespace neverd::detail

#endif // NEVERD_IR_HIGH_PASS_HIGHSEMANTICSIMPLIFIER_H
