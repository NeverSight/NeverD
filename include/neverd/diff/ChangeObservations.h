//===- ChangeObservations.h - Bounded patch review facts --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Local native/SSA changes and observed dependency context. These facts are
/// review leads, not security findings or evidence that a patch is effective.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DIFF_CHANGEOBSERVATIONS_H
#define NEVERD_DIFF_CHANGEOBSERVATIONS_H

#include "neverd/diff/DataFlow.h"
#include "neverd/diff/FunctionDelta.h"

namespace neverd::diff {

inline constexpr llvm::StringLiteral ChangeObservationProfile =
    "native-ssa-changes-v1";

enum ChangeObservationKind : uint32_t {
  ObserveNativeInstruction = 1u << 0,
  ObserveConstant = 1u << 1,
  ObserveOperation = 1u << 2,
  ObserveWidth = 1u << 3,
  ObserveSignedness = 1u << 4,
  ObserveCallArguments = 1u << 5,
};
/// Syntactic data dependencies in the retained graph, not path feasibility,
/// memory effects, error semantics or callee/formal parameter bindings.
enum ChangeObservationContext : uint32_t {
  ContextPredicate = 1u << 0,
  ContextReturn = 1u << 1,
  ContextCallArgument = 1u << 2,
  ContextMemoryWrite = 1u << 3,
};
std::vector<llvm::StringRef> observationKindNames(uint32_t Mask);
std::vector<llvm::StringRef> observationContextNames(uint32_t Mask);

struct ChangeObservationOptions {
  uint32_t MaxRows = limits::diff::kDefaultObservationRows;
  uint32_t MaxObservations = limits::diff::kDefaultObservations;
  uint64_t MaxWork = limits::diff::kDefaultObservationWork;
  DataFlowOptions Graph;
  bool operator==(const ChangeObservationOptions &) const = default;
  std::optional<std::string> validate() const;
};

struct ChangeObservation {
  /// Index into the supplied native delta. Its original instructions remain
  /// the authoritative display and navigation evidence.
  uint32_t Row = NoIndex;
  uint32_t PrimaryInstruction = NoIndex, SecondaryInstruction = NoIndex;
  RowKind NativeKind = RowKind::Identical;
  uint32_t PrimaryNode = NoIndex, SecondaryNode = NoIndex;
  /// Native-origin nodes anchoring a local operation or its unique direct
  /// synthetic operand. Synthetic node addresses are not native attribution.
  uint32_t PrimaryAnchorNode = NoIndex, SecondaryAnchorNode = NoIndex;
  uint32_t Kinds = ObserveNativeInstruction;
  uint32_t PrimaryContext = 0, SecondaryContext = 0;
  /// Review ordering only: neither a security severity nor a probability.
  unsigned Priority = 0;
  bool operator==(const ChangeObservation &) const = default;
};

struct FunctionChangeObservations {
  ComparisonState State = ComparisonState::Complete;
  bool NativeAnalysisComplete = false;
  bool ReferenceComparisonComplete = false;
  bool PrimarySSA = false, SecondarySSA = false;
  uint64_t TotalRows = 0, VisitedRows = 0, Work = 0;
  /// Operation families with more than one candidate at an aligned origin.
  uint64_t AmbiguousOrigins = 0;
  /// Graph-wide coverage/effect boundaries, not effects proved for a row.
  uint32_t PrimaryGaps = 0, SecondaryGaps = 0;
  /// Descending priority, with native row/node IDs breaking ties.
  std::vector<ChangeObservation> Observations;
};

/// Both graphs, when supplied, must bind to the selected native functions and
/// profiles. Aligned native rows, including unchanged instructions, anchor
/// local operands and unique direct synthetic dependencies. Deeper and
/// ambiguous expressions are outside this profile. The completed delta supplies
/// correspondence; this does not improve or confirm its match.
/// Unsupported/ambiguous SSA associations retain native observations. Work
/// counts inspected rows, graph cells and local facts; bounded publication
/// sorting is separate. No input is executed.
FunctionChangeObservations
observeFunctionChanges(const Program &Primary, uint32_t PrimaryFunction,
                       const Program &Secondary, uint32_t SecondaryFunction,
                       const FunctionDelta &Delta,
                       const DataFlowGraph *PrimaryGraph = nullptr,
                       const DataFlowGraph *SecondaryGraph = nullptr,
                       const ChangeObservationOptions &Options = {},
                       const ComparisonControl &Control = {});

} // namespace neverd::diff

#endif // NEVERD_DIFF_CHANGEOBSERVATIONS_H
