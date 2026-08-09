//===- SolidityEmitter.h - EVM to recovered Solidity backend --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares emission of a compilable abstract Solidity reconstruction and
/// checked EVM state machine.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EVM_SOLIDITYEMITTER_H
#define NEVERD_EVM_SOLIDITYEMITTER_H

#include "neverd/evm/EVMIR.h"

#include "llvm/Support/Error.h"

#include <string>

namespace neverd::evm {

/// Controls source compatibility, contract naming, tracing, and declarations.
struct SolidityEmitterOptions {
  std::string ContractName = kDefaultContractName.str();
  std::string Pragma = kDefaultSolidityPragma.str();
  bool EmitTraceEvents = true;
  bool EmitRecoveredDeclarations = true;
};

/// Emits Solidity source with explicit host hooks for unrecovered environment
/// semantics.
llvm::Expected<std::string>
emitSolidity(const EVMProgram &Program,
             const SolidityEmitterOptions &Options = {});

} // namespace neverd::evm

#endif // NEVERD_EVM_SOLIDITYEMITTER_H
