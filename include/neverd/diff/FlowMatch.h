//===- FlowMatch.h - Verify bounded SSA neighborhood candidates -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Fingerprints nominate candidates; exact node/operand and PHI edge checks
/// establish only the observed finite dependency correspondence. Effects and
/// dependencies outside that scope remain boundaries, never equivalence claims.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DIFF_FLOWMATCH_H
#define NEVERD_DIFF_FLOWMATCH_H

#include "neverd/diff/Control.h"
#include "neverd/diff/DataFlow.h"

namespace neverd::diff {
enum class FlowMatchOutcome : uint8_t {
  Matched,
  Different,
  Ambiguous,
  Unsupported
};
struct FlowNeighborhoodMatch {
  ComparisonState State = ComparisonState::Complete;
  FlowMatchOutcome Outcome = FlowMatchOutcome::Different;
  uint64_t Work = 0;
  uint32_t Gaps = 0;
  bool OpenDataFrontier = false;
  /// PHI incoming-edge context is checked within the depth allowance. Other
  /// control dependencies and edges beyond that depth are not compared.
  bool OpenControlFrontier = true;
  std::string Reason;
  /// A partial bijection, sorted by primary node/block ID. Published only after
  /// the entire admitted neighborhood check succeeds.
  std::vector<std::pair<uint32_t, uint32_t>> Nodes, PhiBlocks;
  /// Uninspected external node definitions bound by a fragment's input slots.
  /// These are not matched operations and never contribute to coverage.
  std::vector<std::pair<uint32_t, uint32_t>> Frontier;
};

/// Validated borrowed observations for repeated root checks. Graph and keys
/// must outlive this view and remain immutable. Construct once per graph;
/// candidate checks then visit the admitted neighborhood, not the whole graph.
class FlowMatchInput {
  const DataFlowGraph &Graph;
  const FlowFingerprints &Keys;
  unsigned Rounds;
  friend FlowNeighborhoodMatch
  matchDataFlowNeighborhood(const FlowMatchInput &, uint32_t,
                            const FlowMatchInput &, uint32_t, uint64_t,
                            const ComparisonControl &);

public:
  /// Validation throws ComparisonInterrupted when the supplied control stops.
  FlowMatchInput(const DataFlowGraph &Graph, const FlowFingerprints &Keys,
                 const DataFlowOptions &Options = {},
                 const ComparisonControl &Control = {});
};
FlowNeighborhoodMatch
matchDataFlowNeighborhood(const FlowMatchInput &Primary, uint32_t PrimaryRoot,
                          const FlowMatchInput &Secondary,
                          uint32_t SecondaryRoot,
                          uint64_t MaxWork = limits::diff::kDefaultFlowMaxWork,
                          const ComparisonControl &Control = {});

/// Reuse completed fingerprints for deterministic commutative/PHI ordering.
/// Equal hashes are never sufficient: scalar bits, widths, definitions, shared
/// definitions and PHI predecessor edge positions must also agree. Tied operand
/// keys with distinct possible definitions abstain as ambiguous.
FlowNeighborhoodMatch matchDataFlowNeighborhood(
    const DataFlowGraph &Primary, uint32_t PrimaryRoot,
    const FlowFingerprints &PrimaryKeys, const DataFlowGraph &Secondary,
    uint32_t SecondaryRoot, const FlowFingerprints &SecondaryKeys,
    const DataFlowOptions &Options = {}, const ComparisonControl &Control = {});
} // namespace neverd::diff

#endif // NEVERD_DIFF_FLOWMATCH_H
