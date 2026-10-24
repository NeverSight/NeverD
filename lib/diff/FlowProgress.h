//===- FlowProgress.h - Finite execution ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Linear, interruptible proof for acyclic bodies and direct counter self
/// loops.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DIFF_FLOWPROGRESS_H
#define NEVERD_DIFF_FLOWPROGRESS_H

#include "neverd/diff/DataFlow.h"

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <string>

namespace neverd::diff {
/// All graph references must already have passed structural validation.
FlowProgress proveFlowProgress(const DataFlowGraph &Graph,
                               llvm::StringRef Contract,
                               const DataFlowOptions &Options,
                               llvm::function_ref<void(uint64_t)> Charge);
std::optional<std::string>
validateFlowProgress(const DataFlowGraph &Graph, const DataFlowOptions &Options,
                     llvm::function_ref<void(uint64_t)> Charge);
} // namespace neverd::diff

#endif // NEVERD_DIFF_FLOWPROGRESS_H
