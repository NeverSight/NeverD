//===- RustEmitter.h - Solana SBF to safe Rust backend --------*- C++ -*-===//

#ifndef NEVERD_SBF_RUSTEMITTER_H
#define NEVERD_SBF_RUSTEMITTER_H

#include "neverd/sbf/SBFIR.h"

#include "llvm/Support/Error.h"

#include <string>

namespace neverd::sbf {

struct RustEmitterOptions {
  std::string FunctionName = kEntryFunctionName.str();
  bool IncludeAnalysisComments = true;
  bool PreferStructuredControlFlow = true;
};

llvm::Expected<std::string> emitRust(const SBFProgram &Program,
                                     const RustEmitterOptions &Options = {});

} // namespace neverd::sbf

#endif // NEVERD_SBF_RUSTEMITTER_H
