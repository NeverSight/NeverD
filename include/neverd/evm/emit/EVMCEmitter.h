//===- EVMCEmitter.h - EVM to standalone C23 backend ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares emission of a checked C23 `_BitInt(256)` EVM state machine.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EVM_EMIT_EVMCEMITTER_H
#define NEVERD_EVM_EMIT_EVMCEMITTER_H

#include "neverd/evm/EVMIR.h"

#include "llvm/Support/Error.h"

#include <string>

namespace neverd::evm {

/// Controls the public entry name and optional explanatory instrumentation.
struct CEmitterOptions {
  std::string FunctionName = kDefaultExecutionFunctionName.str();
  bool EmitTraceHooks = true;
  bool EmitRecoveredComments = true;
};

/// Emits standalone C23 source, delegating environment-dependent semantics to
/// the documented host callback.
llvm::Expected<std::string> emitC(const EVMProgram &Program,
                                  const CEmitterOptions &Options = {});

} // namespace neverd::evm

#endif // NEVERD_EVM_EMIT_EVMCEMITTER_H
