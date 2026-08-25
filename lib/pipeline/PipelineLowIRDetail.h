//===- PipelineLowIRDetail.h - LowIR pipeline internals --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Private helpers shared only by the LowIR pipeline and its focused tests.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_PIPELINE_PIPELINELOWIRDETAIL_H
#define NEVERD_LIB_PIPELINE_PIPELINELOWIRDETAIL_H

#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace neverd::pipeline_detail {

/// A widened logical table root aliases another logical table when their
/// identities agree or any of their physically occupied storage overlaps.
bool tableObjectSummaryMayAlias(
    const BinaryImage &Img, va_t RootIdentity,
    llvm::ArrayRef<JumpTableStorageRange> RootRanges, va_t OwnerIdentity,
    llvm::ArrayRef<JumpTableStorageRange> OwnerRanges,
    size_t *WorkRemaining = nullptr);

struct ModuleJumpTableArbitrationTestResult {
  std::set<va_t> UnsafeBranches;
  bool AnalysisComplete = false;
};

/// Focused test seam for the otherwise private module ownership fixed point.
ModuleJumpTableArbitrationTestResult arbitrateModuleJumpTablesForTesting(
    const BinaryImage &Img, const std::vector<LowFunc> &Funcs,
    std::optional<size_t> TestBudget = std::nullopt);

struct ReturnedCodeEvidenceTestResult {
  std::vector<std::set<va_t>> TargetsByFunction;
  std::vector<bool> CompleteByFunction;
  std::vector<std::vector<LowCxxContinuationExitEvidence>>
      OccurrencesByFunction;
  bool AnalysisComplete = false;
};

/// Focused seam for checking that an occurrence-authenticated code address,
/// rather than a numerically equal scalar, reaches an ordinary return value.
ReturnedCodeEvidenceTestResult collectReturnedCodeEvidenceForTesting(
    const BinaryImage &Img, const std::vector<LowFunc> &Funcs,
    const std::set<va_t> &CandidateTargets,
    std::optional<size_t> TestBudget = std::nullopt);

struct WindowsEHContinuationRootTestResult {
  std::map<va_t, std::set<va_t>> RootsByOwner;
  bool AnalysisComplete = false;
};

/// Focused seam for the exact ABI evidence used to attach disconnected
/// Windows exception continuations to one physical LowIR owner.
WindowsEHContinuationRootTestResult collectWindowsEHContinuationRootsForTesting(
    const BinaryImage &Img, const std::vector<LowFunc> &Funcs,
    const std::set<va_t> &FunctionEntries,
    std::optional<size_t> TestBudget = std::nullopt);

} // namespace neverd::pipeline_detail

#endif // NEVERD_LIB_PIPELINE_PIPELINELOWIRDETAIL_H
