//===- HighSourceFlow.h - Source flow analysis -------------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_IR_HIGH_HIGHSOURCEFLOW_H
#define NEVERD_IR_HIGH_HIGHSOURCEFLOW_H

#include "neverd/ir/high/HighIR.h"

#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace neverd {
using HighSourceLocalIdentity = std::tuple<int, int, int, int64_t>;
HighSourceLocalIdentity highSourceLocalIdentity(const MedVar &Variable);
bool highSourceFrameBase(const HighFunc &Function, const MedVar &Variable);

enum class HighSourceFlowIssue {
  ControlFlow,
  DefiniteAssignment,
  MalformedExpression,
  Exception,
  Budget
};
struct HighSourceFlowDiagnostic {
  HighSourceFlowIssue Issue;
  std::string Reason;
  va_t StatementAddress = 0;
  const HighExpr *Expression = nullptr;
  va_t RelatedAddress = 0;
};
/// Diagnostics borrow expressions from Function. The function must outlive
/// this report; expression addresses are never persistent identities.
struct HighSourceFlowReport {
  bool Complete = true;
  std::vector<HighSourceFlowDiagnostic> Items;
  void add(HighSourceFlowIssue Issue, std::string Reason, va_t Address = 0,
           const HighExpr *Expression = nullptr, va_t RelatedAddress = 0);
};
/// A node evaluates Test, or the immediate effects of Statement. Structured
/// bodies are separate nodes; a do-while test is a distinct node with no
/// Statement. Node zero is the fallthrough exit. All pointers borrow Function.
struct HighSourceFlowNode {
  const HighStmt *Statement = nullptr;
  ExprPtr Test;
  std::vector<size_t> Successors;
  /// Branch truth for each successor; nullopt denotes an unconditional edge.
  std::vector<std::optional<bool>> SuccessorTruth;
};
struct HighSourceFlowGraph {
  std::vector<HighSourceFlowNode> Nodes;
  size_t Entry = 0;
  HighSourceFlowReport Diagnostics;
};
/// Build the same bounded, guard-refined graph used by source validation.
/// Incomplete diagnostics invalidate the graph, including unresolved gotos.
HighSourceFlowGraph buildHighSourceFlowGraph(const HighFunc &Function);
HighSourceFlowReport analyzeHighSourceFlow(const HighFunc &Function,
                                           bool NeedsReturn);
struct HighSourceUnsignedRangeQuery {
  const HighStmt *Statement = nullptr;
  ExprPtr Value;
};
/// Upper bounds on pure integer values occurring in a source statement.
/// Every reaching context contributes. Writes, including within the queried
/// statement, and address-taken locals
/// invalidate guard facts; unsupported expressions, incomplete flow and budget
/// exhaustion supply no bound. Results borrow no persistent expression state.
std::vector<std::optional<uint64_t>> highSourceUnsignedUpperBounds(
    const HighFunc &Function,
    const std::vector<HighSourceUnsignedRangeQuery> &Queries);
/// Remove side-effect-free PHI copies whose values cannot be observed on any
/// feasible emitted path. Unknown control flow or exhausted analysis budgets
/// leave the function unchanged. Retains addresses used as source labels.
bool eliminateHighDeadPhiCopies(HighFunc &Function);
struct HighSourcePhiCleanup {
  /// Side-effect-free PHI copies whose values no emitted path reads,
  /// including copies that only feed other such copies.
  std::set<const HighStmt *> DeadCopies;
  /// (member, representative) pairs of scalar locals joined by the other PHI
  /// copies that may share one emitted name: no emitted path writes either
  /// local while the other holds a value still to be read, other than the
  /// copy between them. Only locals defined in the function take part, and
  /// every representative is a PHI copy destination.
  std::vector<std::pair<MedVar, MedVar>> Renames;
};
/// Statements sharing one address are resolved the way the emitter groups
/// branch entries. The complete, unrefined graph is used. Unknown control
/// flow or exhausted analysis budgets yield an empty result. Statement
/// pointers borrow Function.
HighSourcePhiCleanup highSourcePhiCleanup(const HighFunc &Function);
} // namespace neverd
#endif
