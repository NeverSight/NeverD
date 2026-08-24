//===- SBFStructuredCFG.h - SBF reducible control-flow plan -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the backend-neutral control-flow arena used by source emitters.
/// Reducible conditionals and natural loops are represented directly; graphs
/// that need indirect dispatch or irreducible edges deliberately return no
/// plan so a backend can retain its exact PC dispatcher.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_ANALYSIS_SBFSTRUCTUREDCFG_H
#define NEVERD_SBF_ANALYSIS_SBFSTRUCTUREDCFG_H

#include "neverd/sbf/SBFIR.h"

#include <limits>
#include <optional>
#include <vector>

namespace neverd::sbf {

enum class StructuredNodeKind : uint8_t { Block, If, Loop };

struct StructuredNode {
  static constexpr size_t NoNode = std::numeric_limits<size_t>::max();

  StructuredNodeKind Kind = StructuredNodeKind::Block;
  size_t Block = 0;
  /// For loops, selects whether the original taken edge enters the body or
  /// exits it.  If nodes always store the taken edge in Body.
  bool ConditionTrueEntersBody = true;
  /// Indices in StructuredControlFlow::Nodes. Sequences are linked by Next;
  /// Body and Alternative start nested sequences. An index arena avoids
  /// recursive ownership and makes destruction independent of CFG depth.
  size_t Next = NoNode;
  size_t Body = NoNode;
  size_t Alternative = NoNode;
};

struct StructuredControlFlow {
  size_t Entry = StructuredNode::NoNode;
  std::vector<StructuredNode> Nodes;
  /// Deepest structured node in the arena. Backends use this to select an
  /// exact dispatcher when nested syntax would exceed their portable parser
  /// policy; building and owning the arena itself remains depth-independent.
  size_t MaximumDepth = 0;
};

/// Builds an arena only when every reachable block can be represented without
/// duplication. Returning std::nullopt is an expected request for dispatcher
/// fallback, not an analysis error. Construction uses an explicit work stack,
/// so host stack depth is independent of input nesting.
std::optional<StructuredControlFlow>
buildStructuredControlFlow(const SBFProgram &Program);

} // namespace neverd::sbf

#endif // NEVERD_SBF_ANALYSIS_SBFSTRUCTUREDCFG_H
