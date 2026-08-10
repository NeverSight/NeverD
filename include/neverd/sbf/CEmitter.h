//===- CEmitter.h - Solana SBF to portable C backend ----------*- C++ -*-===//

#ifndef NEVERD_SBF_CEMITTER_H
#define NEVERD_SBF_CEMITTER_H

#include "neverd/sbf/SBFIR.h"

#include "llvm/Support/Error.h"

#include <string>

namespace neverd::sbf {

struct CEmitterOptions {
  std::string FunctionName = kEntryFunctionName.str();
  bool IncludeAnalysisComments = true;
  bool PreferStructuredControlFlow = true;
};

llvm::Expected<std::string> emitC(const SBFProgram &Program,
                                  const CEmitterOptions &Options = {});

} // namespace neverd::sbf

#endif // NEVERD_SBF_CEMITTER_H
