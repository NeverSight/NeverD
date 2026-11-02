//===- FlowFragments.h - Finite SSA fragments -------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Multi-depth structural candidates retain exact local facts and explicit
/// external definition bindings. External definitions are not matched code.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DIFF_FLOWFRAGMENTS_H
#define NEVERD_DIFF_FLOWFRAGMENTS_H

#include "neverd/diff/Control.h"
#include "neverd/diff/FlowMatch.h"

#include <cstdint>
#include <vector>

namespace neverd::diff {
struct FlowFragmentLayer {
  std::vector<uint64_t> Keys;
  std::vector<uint8_t> Admitted;
};
struct FlowFragmentFingerprints {
  ComparisonState State = ComparisonState::Complete;
  uint64_t Work = 0;
  /// Layer zero includes one operation and all of its immediate scalar facts;
  /// node-valued inputs are opaque. Each later layer includes one more level.
  std::vector<FlowFragmentLayer> Layers;
};
FlowFragmentFingerprints
fingerprintFlowFragments(const DataFlowGraph &Graph,
                         const DataFlowOptions &Options = {},
                         const ComparisonControl &Control = {});

/// Validated borrowed immutable graphs and completed per-depth keys. Neither
/// a key nor an opaque external binding establishes equal computed values.
class FlowFragmentInput {
  const DataFlowGraph &Graph;
  const FlowFragmentFingerprints &Keys;
  friend FlowNeighborhoodMatch matchFlowFragment(const FlowFragmentInput &,
                                                 uint32_t,
                                                 const FlowFragmentInput &,
                                                 uint32_t, unsigned, uint64_t,
                                                 const ComparisonControl &);

public:
  /// Validation throws ComparisonInterrupted when the supplied control stops.
  FlowFragmentInput(const DataFlowGraph &Graph,
                    const FlowFragmentFingerprints &Keys,
                    const DataFlowOptions &Options = {},
                    const ComparisonControl &Control = {});
};
/// Depth is the number of included operation levels, from one through the
/// prepared rounds. PHIs, branches, switches, effects and unknown operands are
/// excluded internally. Their node definitions may lie outside the frontier.
/// Immediate constants/types are checked even at the last included level.
FlowNeighborhoodMatch
matchFlowFragment(const FlowFragmentInput &Primary, uint32_t PrimaryRoot,
                  const FlowFragmentInput &Secondary, uint32_t SecondaryRoot,
                  unsigned Depth,
                  uint64_t MaxWork = limits::diff::kDefaultFlowMaxWork,
                  const ComparisonControl &Control = {});
} // namespace neverd::diff

#endif // NEVERD_DIFF_FLOWFRAGMENTS_H
