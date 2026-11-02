//===- FlowContract.h - Exact SSA facts -------------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared structural contracts for finite dependency correspondences. Keys
/// nominate candidates; exact comparisons remain authoritative.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DIFF_FLOWCONTRACT_H
#define NEVERD_DIFF_FLOWCONTRACT_H

#include "neverd/diff/DataFlow.h"
#include "neverd/diff/StableHash.h"

#include <tuple>

namespace neverd::diff::detail {
inline uint64_t flowOperationKey(const FlowNode &N) {
  StableHash H;
  H.add(N.Opcode)
      .add(N.ResultBytes)
      .add(N.MemoryOrdering)
      .add(N.MemoryAddressSpace)
      .add(N.Phi)
      .add(N.Gaps)
      .add(N.CallArgumentBegin)
      .add(N.CallArgumentCount);
  if (!N.Semantics.empty())
    H.add(N.Semantics);
  return H.value();
}
inline bool sameFlowOperation(const FlowNode &X, const FlowNode &Y) {
  return std::tie(X.Opcode, X.Semantics, X.ResultBytes, X.MemoryOrdering,
                  X.MemoryAddressSpace, X.Phi, X.Commutative, X.Gaps,
                  X.CallArgumentBegin, X.CallArgumentCount) ==
         std::tie(Y.Opcode, Y.Semantics, Y.ResultBytes, Y.MemoryOrdering,
                  Y.MemoryAddressSpace, Y.Phi, Y.Commutative, Y.Gaps,
                  Y.CallArgumentBegin, Y.CallArgumentCount);
}
inline bool sameFlowOperandShape(const FlowOperand &X, const FlowOperand &Y) {
  return std::tie(X.Type, X.Bytes, X.Provenance, X.Semantics) ==
         std::tie(Y.Type, Y.Bytes, Y.Provenance, Y.Semantics);
}
} // namespace neverd::diff::detail

#endif // NEVERD_DIFF_FLOWCONTRACT_H
