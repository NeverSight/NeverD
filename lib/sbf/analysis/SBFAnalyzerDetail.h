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

#include "llvm/ADT/Twine.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Error.h"

#include <cstddef>
#include <optional>
#include <string>

namespace neverd::sbf {
namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE analyzer_detail {

llvm::Error analysisError(size_t Slot, va_t Address, llvm::Twine Message);

const Symbol *findFunctionSymbol(const BinaryImage &Image, va_t Address);

std::optional<size_t> addressToSlot(const Metadata &Metadata, va_t Address);

std::string syntheticFunctionName(va_t Address);

struct DecodeContext {
  const BinaryImage &Image;
  const AnalyzeOptions &Options;
  SBFProgram &Program;

  llvm::Error report(size_t Slot, llvm::Twine Message,
                     DiagnosticSeverity Severity = DiagnosticSeverity::Error) {
    const va_t Address = Program.Image.TextVM.Address + Slot * kInstructionSize;
    Program.Low.Diagnostics.push_back({Severity, Slot, Address, Message.str()});
    if (Options.Strict && Severity == DiagnosticSeverity::Error)
      return analysisError(Slot, Address, Message);
    return llvm::Error::success();
  }
};

/// Decode every text slot into LowIR, reporting malformed encodings.
llvm::Error decodeInstructions(DecodeContext &Context);

/// Resolve branch targets and call kinds on the decoded LowIR.
llvm::Error resolveControlFlow(DecodeContext &Context);

/// Partition the resolved instruction stream into basic blocks and edges.
void buildCFG(LowIR &Low);

/// Lower the decoded LowIR into the normalized MedIR instruction stream.
void buildMedIR(SBFProgram &Program);

/// Run the block-level constant register lattice over the MedIR.
void runRegisterDataflow(SBFProgram &Program);

/// Recover functions, call edges, strings, and structured regions.
void recoverHighIR(const BinaryImage &Image, SBFProgram &Program);

} // namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE analyzer_detail
} // namespace neverd::sbf

#endif // NEVERD_SBF_ANALYSIS_SBFANALYZERDETAIL_H
