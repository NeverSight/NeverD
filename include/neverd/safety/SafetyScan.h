//===- SafetyScan.h - Unified native memory-safety report -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Run the selected static analyses over one image and retain each analyzer's
/// evidence and independent coverage status.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SAFETY_SAFETYSCAN_H
#define NEVERD_SAFETY_SAFETYSCAN_H

#include "neverd/safety/Safety.h"

#include "llvm/ADT/STLFunctionalExtras.h"

#include <array>

namespace neverd::safety {

enum class ScanSelection { Both, Audit, Hunt };
enum class ScanState {
  Completed,
  Disabled,
  Canceled,
  Unsupported,
  BudgetExhausted,
  Failed
};

struct AnalyzerResult {
  ScanState State;
  SafetyReport Report;
};

struct ScanReport {
  ScanReport(ScanSelection Selection,
             ScanState Initial = ScanState::Unsupported,
             const std::string &Reason = {});
  std::array<AnalyzerResult, 2> Analyzers;
};

/// Check cancellation between bounded units of work. A stopped or failed
/// analyzer retains evidence already established by this scan.
ScanReport runScan(const AnalysisInput &In, const SinkCatalog &Cat,
                   const SafetyBudgets &Budgets,
                   ScanSelection Selection = ScanSelection::Both,
                   llvm::function_ref<bool(Track)> Canceled = nullptr);

std::string toJson(const ScanReport &Report, bool Pretty = false);

} // namespace neverd::safety

#endif // NEVERD_SAFETY_SAFETYSCAN_H
