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

#include "neverd/ir/IntegerCallArguments.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/FormattedCall.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace neverd {

/// What every run reads of an image's symbols: the padding-boundary guesses
/// (BinaryImage::boundaryGuessFunctionStarts) and the starts of the other
/// function symbols, sorted and each once.  An image holds tens of thousands
/// of symbols, which each selected-function run read several times.  A cache
/// keeps it for the image its owner leaves unchanged; the tables it was read
/// from guard it, as Impl::matches guards the facts.
struct PipelineAnalysisCache::SymbolInventory {
  explicit SymbolInventory(const BinaryImage &Img);

  /// \p Img's inventory as \p Cache keeps it; one read now when \p Cache
  /// keeps none for \p Img, which it then keeps.
  static std::shared_ptr<const SymbolInventory>
  of(const BinaryImage &Img, PipelineAnalysisCache &Cache);

  /// Whether this was read from \p Img as it stands.
  bool readFrom(const BinaryImage &Img) const;

  /// The name of the function at every address an export or a symbol names
  /// (functionNamesAt), read from \p Img, which this was read from, on the
  /// first request.  Every other address is named `sub_` and its address.
  const std::map<va_t, std::string> &
  functionNames(const BinaryImage &Img) const;

  /// The addresses that name a runtime routine one intrinsic models in
  /// \p Img, which this was read from (runtimeRoutineCallsOf), read on the
  /// first request.
  const std::map<va_t, Intrinsic> &
  runtimeRoutineCalls(const BinaryImage &Img) const;

  /// The catch continuations of \p Img's FH4 handlers that lie in another
  /// function of their record (pipeline_detail::fh4ContinuationCandidates),
  /// read on the first request.  The cache's owner resets it before the
  /// image's exception metadata changes.
  const std::vector<std::pair<va_t, va_t>> &
  fh4ContinuationCandidates(const BinaryImage &Img) const;

  const std::set<va_t> BoundaryGuesses;
  const std::vector<va_t> Starts;
  /// The addresses the image's exports name, sorted and each once.
  const std::vector<va_t> ExportStarts;

private:
  const BinaryImage *Image;
  const Symbol *SymbolData;
  size_t SymbolCount;
  const Export *ExportData;
  size_t ExportCount;
  const std::pair<va_t, va_t> *RangeData;
  size_t RangeCount;
  size_t ContinuationCount;
  mutable std::optional<std::map<va_t, std::string>> FunctionNames;
  mutable std::optional<std::map<va_t, Intrinsic>> RuntimeRoutineCalls;
  mutable std::optional<std::vector<std::pair<va_t, va_t>>>
      FH4ContinuationCandidates;
};

/// The prototype counts of every import of an image, as
/// importPrototypeArguments gives them with the debug context it read.
struct ImageImportCounts {
  const BinaryImage *Image = nullptr;
  const DebugContext *Debug = nullptr;
  const Import *ImportData = nullptr;
  size_t ImportCount = 0;
  const RelocationEntry *RelocationData = nullptr;
  size_t RelocationCount = 0;
  const Export *ExportData = nullptr;
  size_t ExportCount = 0;
  std::map<va_t, int> Counts;
  std::map<va_t, int> StackArgs;
  std::set<va_t> StackSlots;
  std::map<va_t, FloatingCallArguments> FloatingArguments;
  std::map<va_t, FixedIntegerCallArguments> IntegerArguments;
  std::map<va_t, std::vector<uint16_t>> VariadicArguments;
  /// Whether these were read from \p Img as it is, with \p Dbg.
  bool readFrom(const BinaryImage &Img, const DebugContext *Dbg) const {
    return Image == &Img && Debug == Dbg && ImportData == Img.Imports.data() &&
           ImportCount == Img.Imports.size() &&
           RelocationData == Img.Relocations.data() &&
           RelocationCount == Img.Relocations.size() &&
           ExportData == Img.Exports.data() &&
           ExportCount == Img.Exports.size();
  }
};

/// The analysis cache a run of \p Img under \p Opts shares with other runs:
/// a restricted x64, i386, AArch64 or ARM run's.  Other runs keep what they
/// read to themselves.
inline PipelineAnalysisCache *sharedAnalysisCache(const BinaryImage &Img,
                                                  const PipelineOptions &Opts) {
  const bool Restricted =
      !Opts.OnlyFunctionEntries.empty() || Opts.MaxFunctions != 0;
  return Restricted && (Img.Arch == Arch::X64 || Img.Arch == Arch::X86 ||
                        Img.Arch == Arch::AArch64 || Img.Arch == Arch::ARM)
             ? Opts.AnalysisCache
             : nullptr;
}

} // namespace neverd

namespace neverd::pipeline_detail {

/// The catch continuations \p Img's complete FH4 handlers name outside the
/// function that declares them, each with the one other function of the
/// same function information record that holds it past its start, where
/// the continuation is code: pairs of that function's start and the
/// continuation.
std::vector<std::pair<va_t, va_t>>
fh4ContinuationCandidates(const BinaryImage &Img);

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
  std::set<va_t> ProtectedRelocationSlots;
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
  std::map<va_t, std::vector<LowSEHLocalUnwindEvidence>> LocalUnwindsByOwner;
  bool AnalysisComplete = false;
};

/// Focused seam for the exact ABI evidence used to attach disconnected
/// Windows exception continuations to one physical LowIR owner.
WindowsEHContinuationRootTestResult collectWindowsEHContinuationRootsForTesting(
    const BinaryImage &Img, const std::vector<LowFunc> &Funcs,
    const std::set<va_t> &FunctionEntries,
    std::optional<size_t> TestBudget = std::nullopt,
    bool CompleteModule = true);

/// The calls in \p F to a printf-family routine whose format is a constant
/// string the image holds, keyed by the call instruction's address, added
/// to \p Calls.  Only a convention whose formatted calls pass every argument
/// in a register lists them (CallArgumentConvention::FormattedCallArguments).
void collectFormattedCalls(const BinaryImage &Img, const LowFunc &F,
                           std::map<va_t, FormattedCall> &Calls);
void collectFormattedCalls(const BinaryImage &Img,
                           llvm::ArrayRef<LowBlock> Blocks,
                           std::map<va_t, FormattedCall> &Calls,
                           bool HasIndependentRoots = false);

} // namespace neverd::pipeline_detail

#endif // NEVERD_LIB_PIPELINE_PIPELINELOWIRDETAIL_H
