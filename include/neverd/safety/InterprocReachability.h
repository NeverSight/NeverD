//===- InterprocReachability.h - Entry reachability ----------*- C++ -*-===//

#ifndef NEVERD_SAFETY_INTERPROCREACHABILITY_H
#define NEVERD_SAFETY_INTERPROCREACHABILITY_H

#include "neverd/safety/SafetyContext.h"
#include "neverd/safety/SafetyTypes.h"

#include <map>

namespace neverd::safety {

class SinkCatalog;

struct FunctionReachability {
  ReachabilityStatus Status = ReachabilityStatus::Unknown;
  va_t RootFunctionVA = 0;
  std::optional<va_t> EntryVA;
  std::string EntryName;
  SafetyEntryKind Kind = SafetyEntryKind::Image;
  std::vector<ReachabilityCall> CallChain;
  std::string Reason;
  bool BudgetHit = false;
};

struct InterprocResult {
  std::map<va_t, FunctionReachability> Functions;
  ParameterFlowMap Parameters;
  bool GraphComplete = true;
  bool BudgetHit = false;
  bool SummaryBudgetHit = false;

  const FunctionReachability *findFunction(va_t Entry) const;
  const ParameterFlow *findParameter(va_t FunctionEntry,
                                     size_t ParameterIndex) const;
  void annotate(Finding &Finding) const;
};

/// Compute structural reachability and a monotone attacker-parameter fixed
/// point over the exact internal call identities in a validated MedIR program.
InterprocResult analyzeInterprocedural(const AnalysisInput &In,
                                       const SinkCatalog &Catalog,
                                       const SafetyBudgets &Budgets);

} // namespace neverd::safety

#endif // NEVERD_SAFETY_INTERPROCREACHABILITY_H
