//===- FlowBinding.h - Exact SSA identity under PHI numbering -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate LLVM observations without making PHI numbering semantic.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DIFF_FLOWBINDING_H
#define NEVERD_DIFF_FLOWBINDING_H

#include "neverd/diff/Control.h"
#include "neverd/diff/DataFlow.h"

#include "llvm/ADT/STLFunctionalExtras.h"

#include <cstdint>

namespace neverd::diff {
/// Saved must already be validated. Fresh is validated before following its
/// references. Charge bounds this additional binding pass; graph validation
/// retains its existing independent allowance. Neither graph is mutated.
bool sameLLVMGraphAfterPhiRenumbering(const DataFlowGraph &Saved,
                                      const DataFlowGraph &Fresh,
                                      const DataFlowOptions &Options,
                                      llvm::function_ref<void(uint64_t)> Charge,
                                      const ComparisonControl &Control = {});
} // namespace neverd::diff

#endif // NEVERD_DIFF_FLOWBINDING_H
