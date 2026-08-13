//===- EVMSemantics.h - Shared scalar EVM semantics -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares target-independent evaluation of EVM scalar ALU instructions.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EVM_RUNTIME_EVMSEMANTICS_H
#define NEVERD_EVM_RUNTIME_EVMSEMANTICS_H

#include "neverd/evm/bytecode/EVMOpcodes.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"

#include <optional>

namespace neverd::evm {

/// Evaluate one assigned scalar ALU instruction. Inputs use EVM pop order:
/// element zero is the original stack top. Returns no value for a non-ALU
/// opcode, a mismatched stack contract, or a non-EVM word width.
[[nodiscard]] std::optional<llvm::APInt>
evaluateALU(Opcode Op, llvm::ArrayRef<llvm::APInt> Inputs);

} // namespace neverd::evm

#endif // NEVERD_EVM_RUNTIME_EVMSEMANTICS_H
