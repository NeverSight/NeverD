//===- FlowEvidence.h - Bounded native SSA collection -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Per-side, address-ordered observations and explicit unavailable records.
/// A complete collection can still contain unsupported functions and effects.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DIFF_FLOWEVIDENCE_H
#define NEVERD_DIFF_FLOWEVIDENCE_H

#include "neverd/diff/Control.h"
#include "neverd/diff/DataFlow.h"

namespace neverd::diff {

struct DataFlowSettings {
  bool Enabled = false;
  unsigned MaxFunctions = limits::diff::kDefaultFlowFunctions;
  unsigned MaxNativeInstructions = limits::diff::kDefaultFlowNativeInstructions;
  uint64_t MaxTotalNodes = limits::diff::kDefaultFlowTotalNodes;
  DataFlowOptions Graph;
  std::string Profile = DataFlowProfile.str();
  bool operator==(const DataFlowSettings &) const = default;
  std::optional<std::string> validate() const;
  std::string describe() const;
};

enum class FlowAvailability : uint8_t {
  Available,
  Unsupported,
  ResourceLimited,
  Failed,
};
llvm::StringRef availabilityName(FlowAvailability State);
std::optional<FlowAvailability> availabilityNamed(llvm::StringRef Name);

struct DataFlowEntry {
  uint64_t Function = 0;
  FlowAvailability Availability = FlowAvailability::Unsupported;
  std::string Reason;
  std::optional<DataFlowGraph> Graph;
  bool operator==(const DataFlowEntry &) const = default;
};

struct DataFlowEvidence {
  ComparisonState State = ComparisonState::Complete;
  /// Implementation/recipe identity in addition to native revision and ABI.
  std::string Recipe;
  uint64_t EligibleFunctions = 0;
  std::vector<DataFlowEntry> Entries;
  uint64_t nodes() const;
  uint64_t retainedItems() const;
  const DataFlowEntry *find(uint64_t Function) const;
};

/// Check scope, native origins, budgets and graph structure. Does not establish
/// that a graph is the actual pipeline output; binding also compares that
/// output.
std::optional<std::string>
validateDataFlowEvidence(const DataFlowEvidence *Evidence,
                         const DataFlowSettings &Settings, const Program &Input,
                         const ComparisonControl &Control = {});
/// A stopped saved collection may retain a verified prefix of a fresh one.
/// LLVM PHI renumbering may be authenticated through an exact complete node
/// bijection with unchanged ordinary instruction order and source metadata.
/// Saved IDs are retained; this does not establish behavioral equivalence.
std::optional<std::string>
bindDataFlowEvidence(const DataFlowEvidence *Saved,
                     const DataFlowEvidence *Fresh,
                     const DataFlowSettings &Settings, const Program &Input,
                     const ComparisonControl &Control = {});

} // namespace neverd::diff

#endif // NEVERD_DIFF_FLOWEVIDENCE_H
