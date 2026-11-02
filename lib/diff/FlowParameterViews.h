//===- FlowParameterViews.h - Native parameter view contracts ----*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared signatures and bounded native-site admission for projection and
/// saved graph validation. Source widths never disappear from provenance.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_DIFF_FLOWPARAMETERVIEWS_H
#define NEVERD_LIB_DIFF_FLOWPARAMETERVIEWS_H

#include "neverd/diff/DataFlow.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd::diff {
std::string parameterViewSemantics(const FlowParameterView &View);
std::vector<uint8_t>
unambiguousParameterSites(llvm::ArrayRef<FlowParameterView> Views,
                          llvm::function_ref<void(uint64_t)> Charge);
std::optional<std::string>
validateParameterViews(const DataFlowGraph &Graph,
                       const DataFlowOptions &Options,
                       llvm::function_ref<void(uint64_t)> Charge);
} // namespace neverd::diff

#endif // NEVERD_LIB_DIFF_FLOWPARAMETERVIEWS_H
