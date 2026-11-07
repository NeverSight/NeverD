//===- HighCAnalysisDispatch.cpp - Unresolved branch annotations ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Show the original indirect instruction beside a tentative dispatch exit.
///
//===----------------------------------------------------------------------===//

#include "../../CAnalysisDispatch.h"
#include "../HighCWriter.h"

namespace neverd {

std::string HighCWriter::analysisDispatchComment(const HighExpr &Call) const {
  if (!Opts.EmitComments || !CurrentFunc || !CurrentFunc->AnalysisOnly ||
      Call.Kind != ExprKind::Call ||
      Call.CallTarget != "__neverd_analysis_indirect_jump" ||
      !Call.DoesNotReturn || Call.Operands.size() != 1)
    return {};

  // Only this rare display exit needs decoding. Ordinary calls and comment-
  // free output never initialize a decoder or inspect image bytes here.
  return neverd::analysisDispatchComment(Opts.Image, Opts.TheArch,
                                         Call.CallSiteAddr);
}

} // namespace neverd
