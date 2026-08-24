//===- SBFAnalyzerDetail.h - Private SBF analysis stage helpers -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Internal declarations shared between the translation units that make up
/// the staged SBF analyzer: decoding and call resolution (SBFAnalyzerDecode),
/// CFG and register dataflow construction (SBFAnalyzerCFG), region and HighIR
/// recovery (SBFAnalyzerRegions), and the driver (SBFAnalyzer).
///
/// This header is an implementation detail of the sbf/analysis library and
/// should NOT be included by code outside lib/sbf/analysis/.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_ANALYSIS_SBFANALYZERDETAIL_H
#define NEVERD_SBF_ANALYSIS_SBFANALYZERDETAIL_H

#include "neverd/loader/BinaryImage.h"
#include "neverd/sbf/analysis/SBFAnalyzer.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Error.h"

#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace neverd::sbf {
namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE analyzer_detail {

inline constexpr llvm::StringLiteral kCallTargetInsideWideLoadDiagnostic =
    "resolved call target starts inside a wide load and cannot begin a "
    "function";

llvm::Error analysisError(size_t Slot, va_t Address, llvm::Twine Message);

std::optional<size_t> addressToSlot(const Metadata &Metadata, va_t Address);

std::string syntheticFunctionName(va_t Address);

struct DecodeContext {
  const BinaryImage &Image;
  const AnalyzeOptions &Options;
  SBFProgram &Program;
  llvm::DenseMap<va_t, const Symbol *> FunctionSymbols;
  llvm::DenseMap<va_t, const RelocationEntry *> CallRelocations;
  /// Complete instruction slots trusted as function entries.  This is the
  /// single entry index consumed by CFG partitioning and HighIR recovery.
  llvm::BitVector FunctionEntrySlots;

  DecodeContext(const BinaryImage &Image, const AnalyzeOptions &Options,
                SBFProgram &Program);

  const Symbol *findFunctionSymbol(va_t Address) const;
  const RelocationEntry *findCallRelocation(va_t Address) const;

  llvm::Error report(size_t Slot, llvm::Twine Message,
                     DiagnosticSeverity Severity = DiagnosticSeverity::Error,
                     ValidationRule Rule = ValidationRule::None) {
    const va_t Address = Program.Image.TextVM.Address + Slot * kInstructionSize;
    Program.Low.Diagnostics.push_back(
        {Severity, Slot, Address, Message.str(), Rule});
    if (Options.Strict && Severity == DiagnosticSeverity::Error)
      return analysisError(Slot, Address, Message);
    return llvm::Error::success();
  }
};

/// Decode every text slot into LowIR, reporting malformed encodings.
llvm::Error decodeInstructions(DecodeContext &Context);

/// Resolve branch targets and call kinds on the decoded LowIR.
llvm::Error resolveControlFlow(DecodeContext &Context);

/// Collect complete function entries before any derived IR is constructed.
void collectFunctionEntries(DecodeContext &Context);

/// Partition the resolved instruction stream into basic blocks and edges.
void buildCFG(LowIR &Low, const llvm::BitVector &FunctionEntrySlots);

/// Lower the decoded LowIR into the normalized MedIR instruction stream.
void buildMedIR(SBFProgram &Program);

/// Run the block-level constant register lattice over the MedIR. Trusted
/// function entries are independent roots of the intraprocedural fixed point.
struct RegisterDataflowStatistics {
  size_t BlockCount = 0;
  size_t RootCount = 0;
  size_t FunctionRootCount = 0;
  size_t BoundarySeedCount = 0;
  size_t PeakBoundaryWorkspaceEntryCount = 0;
};

RegisterDataflowStatistics
runRegisterDataflow(SBFProgram &Program,
                    const llvm::BitVector &FunctionEntrySlots);

/// Recompute every CALLX classification from the current register fixed point.
/// Returns true when either a classification changed or a newly discovered
/// candidate target enlarged the conservative function-boundary set.
bool refineCallXTargets(DecodeContext &Context);

/// Compact immediate-dominator index shared with its differential unit test.
/// Construction and all retained indices use O(V + E) storage.
struct DominatorTree {
  size_t Root = std::numeric_limits<size_t>::max();
  std::vector<size_t> IDom;
  std::vector<size_t> Depth;
  std::vector<size_t> Preorder;
  std::vector<size_t> SubtreeEnd;
  std::vector<size_t> ChainHead;
};

DominatorTree
buildDominatorTree(llvm::ArrayRef<std::vector<size_t>> Successors,
                   llvm::ArrayRef<std::vector<size_t>> Predecessors,
                   size_t Root);

bool dominates(const DominatorTree &Tree, size_t A, size_t B);

std::optional<size_t> nearestCommonDominator(const DominatorTree &Tree,
                                             size_t A, size_t B);

/// Recover functions, call edges, strings, and structured regions.
void recoverHighIR(DecodeContext &Context);

} // namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE analyzer_detail
} // namespace neverd::sbf

#endif // NEVERD_SBF_ANALYSIS_SBFANALYZERDETAIL_H
