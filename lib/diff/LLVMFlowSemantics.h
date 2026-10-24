//===- LLVMFlowSemantics.h - Finite scalars -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared finite-operation authority for source projection and progress proofs.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DIFF_LLVMFLOWSEMANTICS_H
#define NEVERD_DIFF_LLVMFLOWSEMANTICS_H

#include "neverd/diff/DataFlow.h"

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Intrinsics.h"

#include <cstdint>
#include <optional>
#include <string>

namespace neverd::diff {
enum class LLVMFlowShape {
  Unsupported,
  Binary,
  Compare,
  Select,
  Cast,
  Unary,
  BoolArgument,
  Ternary
};
LLVMFlowShape integerFlowShape(llvm::StringRef Opcode);
LLVMFlowShape intrinsicFlowShape(llvm::Intrinsic::ID ID);
/// Requires structurally validated node/CFG references. This does not
/// authenticate saved source attributes; fresh native binding does that.
std::optional<std::string>
validateFiniteLLVMFlow(const DataFlowGraph &Graph, llvm::StringRef Contract,
                       llvm::function_ref<void(uint64_t)> Charge);
} // namespace neverd::diff

#endif // NEVERD_DIFF_LLVMFLOWSEMANTICS_H
