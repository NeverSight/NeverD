//===- StructuredCFG.h - SBF reducible control-flow plan ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the backend-neutral tree used by source emitters.  Reducible
/// conditionals and natural loops are represented directly; graphs that need
/// indirect dispatch or irreducible edges deliberately return no plan so a
/// backend can retain its exact PC dispatcher.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_STRUCTUREDCFG_H
#define NEVERD_SBF_STRUCTUREDCFG_H

#include "neverd/sbf/SBFIR.h"

#include <optional>
#include <vector>

namespace neverd::sbf {

enum class StructuredNodeKind : uint8_t { Block, If, Loop };

struct StructuredNode {
  StructuredNodeKind Kind = StructuredNodeKind::Block;
  size_t Block = 0;
  /// For loops, selects whether the original taken edge enters the body or
  /// exits it.  If nodes always store the taken edge in Body.
  bool ConditionTrueEntersBody = true;
  std::vector<StructuredNode> Body;
  std::vector<StructuredNode> Alternative;
};

struct StructuredControlFlow {
  std::vector<StructuredNode> Body;
};

/// Builds a tree only when every reachable block can be represented without
/// duplication.  Returning std::nullopt is an expected request for dispatcher
/// fallback, not an analysis error.
std::optional<StructuredControlFlow>
buildStructuredControlFlow(const SBFProgram &Program);

} // namespace neverd::sbf

#endif // NEVERD_SBF_STRUCTUREDCFG_H
