//===- SBFAnalysisReport.h - SBF stage and coverage evidence ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared stage identities and byte/CFG coverage counters. The observer sees
/// immutable snapshots; transports decide how to persist them across crashes.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_ANALYSIS_SBFANALYSISREPORT_H
#define NEVERD_SBF_ANALYSIS_SBFANALYSISREPORT_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"

#include <cstdint>
#include <functional>
#include <optional>

namespace neverd::sbf {

struct SBFProgram;

enum class AnalysisStage : uint8_t {
#define SBF_ANALYSIS_STAGE(ID, NAME) ID,
#include "neverd/sbf/analysis/SBFAnalysisStages.def"
};

enum class AnalysisStageState : uint8_t { Started, Passed, Failed };

llvm::StringRef analysisStageName(AnalysisStage Stage);
std::optional<AnalysisStage> parseAnalysisStage(llvm::StringRef Name);
llvm::StringRef analysisStageStateName(AnalysisStageState State);

using AnalysisObserver =
    std::function<void(AnalysisStage, AnalysisStageState, const SBFProgram &)>;

/// Counts describe only materialized results. The completed stage determines
/// which counters are final: CFG and CALLX targets converge together in MedIR.
/// Coverage counts raw slots separately from logical instructions, including
/// unreachable and ambiguously owned code rather than removing it from totals.
llvm::json::Object analysisCoverage(const SBFProgram &Program);

} // namespace neverd::sbf

#endif // NEVERD_SBF_ANALYSIS_SBFANALYSISREPORT_H
