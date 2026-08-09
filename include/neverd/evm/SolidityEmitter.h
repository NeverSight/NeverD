//===- SolidityEmitter.h - EVM to recovered Solidity backend --*- C++ -*-===//

#ifndef NEVERD_EVM_SOLIDITYEMITTER_H
#define NEVERD_EVM_SOLIDITYEMITTER_H

#include "neverd/evm/EVMIR.h"

#include "llvm/Support/Error.h"

#include <string>

namespace neverd::evm {

struct SolidityEmitterOptions {
  std::string ContractName = kDefaultContractName.str();
  std::string Pragma = kDefaultSolidityPragma.str();
  bool EmitTraceEvents = true;
  bool EmitRecoveredDeclarations = true;
};

llvm::Expected<std::string> emitSolidity(const EVMProgram &Program,
                                         SolidityEmitterOptions Options = {});

} // namespace neverd::evm

#endif // NEVERD_EVM_SOLIDITYEMITTER_H
