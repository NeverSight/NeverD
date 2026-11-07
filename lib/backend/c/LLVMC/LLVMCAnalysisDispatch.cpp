//===- LLVMCAnalysisDispatch.cpp - Unresolved display transfers -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Keep the original jump readable beside a private LLVM analysis marker.
///
//===----------------------------------------------------------------------===//

#include "../CAnalysisDispatch.h"
#include "LLVMCWriter.h"

#include "neverd/backend/LLVMAnalysisDispatch.h"

namespace neverd {

bool LLVMCWriter::writeAnalysisDispatchCall(llvm::CallBase &Call, int Indent) {
  const auto Address = llvm_analysis_dispatch::address(Call);
  if (!Address)
    return false;
  emitIndent(Indent);
  OS << functionIdentifier(*Call.getCalledFunction()) << "("
     << valueStr(Call.getArgOperand(0)) << ");";
  if (Opts.EmitComments)
    OS << analysisDispatchComment(Img ? Img : Opts.Image, Opts.TheArch,
                                  *Address);
  OS << "\n";
  return true;
}

} // namespace neverd
