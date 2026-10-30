//===- FlowArgumentAccess.h - Argument addresses -----------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Relate native SSA memory operands to recovered call arguments by their exact
/// computed addresses. No callee write, alias completeness or path is inferred.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DIFF_FLOWARGUMENTACCESS_H
#define NEVERD_DIFF_FLOWARGUMENTACCESS_H

#include "neverd/diff/DataFlow.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd::diff {
inline constexpr llvm::StringLiteral FlowArgumentAccessProfile =
    "med-ssa-argument-accesses-v1";

struct FlowArgumentAccessOptions {
  uint32_t MaxRecords = limits::diff::kDefaultFlowAccessRecords;
  uint64_t MaxWork = limits::diff::kDefaultFlowMaxWork;
  std::optional<std::string> validate() const;
};
struct FlowArgumentAccess {
  uint32_t Call = NoIndex;
  /// Zero-based position in the recovered argument span, not an ABI formal.
  uint32_t Argument = 0;
  uint32_t Access = NoIndex;
  uint16_t Bytes = 0;
  bool Write = false;
  /// A same-width COPY/PHI path connects this read to a RETURN operand.
  /// This records an operand path, not its feasibility or when the read
  /// executes. RETURN inputs may describe control targets; no ABI result
  /// binding is inferred.
  bool ReturnOperandPath = false;
  bool operator==(const FlowArgumentAccess &) const = default;
};
struct FlowArgumentAccesses {
  ComparisonState State = ComparisonState::Complete;
  bool Complete = false;
  uint64_t Work = 0;
  uint32_t Gaps = 0;
  /// Sorted by call, argument, then access node; repeated arguments stay
  /// distinct.
  std::vector<FlowArgumentAccess> Records;
};

/// Index the graph once, then join address operands to call argument positions.
/// Only med-ssa-v2, ordinary LOAD/STORE and equal-width COPY/constant ADD/SUB
/// chains are observed. Equal roots are the same local SSA definition or input
/// parameter; unknown inputs and separate loads never acquire a shared
/// identity. Call/intrinsic outputs are excluded because one node may name many
/// clobbers. Original graph gaps remain intact. Absent observations do not rule
/// out aliases.
FlowArgumentAccesses
observeFlowArgumentAccesses(const DataFlowGraph &Graph,
                            const FlowArgumentAccessOptions &Options = {},
                            const DataFlowOptions &GraphOptions = {},
                            const ComparisonControl &Control = {});
} // namespace neverd::diff

#endif // NEVERD_DIFF_FLOWARGUMENTACCESS_H
