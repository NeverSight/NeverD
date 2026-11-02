//===- FlowInvariantBounds.h - Input-derived bounds -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Memoized, interruptible observation of pure input-derived bound DAGs.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DIFF_FLOWINVARIANTBOUNDS_H
#define NEVERD_DIFF_FLOWINVARIANTBOUNDS_H

#include "neverd/diff/DataFlow.h"

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace neverd::diff {
struct FlowInvariantBound {
  uint32_t Node;
  uint64_t Offset;
};

/// Requires structural validation and validateFiniteLLVMFlow admission. This
/// observes invariance, not LLVM metadata authenticity or source provenance.
class FlowInvariantBounds {
  enum class State : uint8_t { Unknown, Active, Invalid, Invariant };
  struct Entry {
    State Status = State::Unknown;
    FlowInvariantBound Bound{NoIndex, 0};
  };
  struct Frame {
    uint32_t Node;
    size_t Next = 0;
  };
  const DataFlowGraph &G;
  llvm::StringRef Contract;
  llvm::function_ref<void(uint64_t)> Charge;
  std::vector<Entry> Cache;
  std::vector<Frame> Stack;

  void visit(uint32_t Node);
  void finish(uint32_t Node);

public:
  FlowInvariantBounds(const DataFlowGraph &Graph, llvm::StringRef Contract,
                      llvm::function_ref<void(uint64_t)> Charge);
  std::optional<FlowInvariantBound> observe(const FlowOperand &Input);
};
} // namespace neverd::diff

#endif // NEVERD_DIFF_FLOWINVARIANTBOUNDS_H
