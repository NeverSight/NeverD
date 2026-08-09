//===- CEmitter.h - EVM to standalone C23 backend -------------*- C++ -*-===//

#ifndef NEVERD_EVM_CEMITTER_H
#define NEVERD_EVM_CEMITTER_H

#include "neverd/evm/EVMIR.h"

#include "llvm/Support/Error.h"

#include <string>

namespace neverd::evm {

struct CEmitterOptions {
  std::string FunctionName = kDefaultExecutionFunctionName.str();
  bool EmitTraceHooks = true;
  bool EmitRecoveredComments = true;
};

llvm::Expected<std::string> emitC(const EVMProgram &Program,
                                  CEmitterOptions Options = {});

} // namespace neverd::evm

#endif // NEVERD_EVM_CEMITTER_H
