//===- InsertPending.h - Ordered MedIR insertion --------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Insert pending operations at positions in the original block.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_IR_MED_LOWER_INSERTPENDING_H
#define NEVERD_LIB_IR_MED_LOWER_INSERTPENDING_H

#include "neverd/ir/med/MedIR.h"

#include <cstddef>
#include <utility>
#include <vector>

namespace neverd {
// Puts each pending op before the op of \p Ops at its index, in one pass:
// what inserting them one at a time from the last gives.  \p Pending is in
// index order, with no index past Ops.size(); ops pending at one index keep
// their order.
template <typename PendingOp>
void insertPending(std::vector<MedOp> &Ops, std::vector<PendingOp> &Pending,
                   size_t PendingOp::*Index) {
  if (Pending.empty())
    return;
  std::vector<MedOp> Merged;
  Merged.reserve(Ops.size() + Pending.size());
  size_t Next = 0;
  for (size_t I = 0; I < Ops.size(); ++I) {
    for (; Next < Pending.size() && Pending[Next].*Index == I; ++Next)
      Merged.push_back(std::move(Pending[Next].Op));
    Merged.push_back(std::move(Ops[I]));
  }
  for (; Next < Pending.size(); ++Next)
    Merged.push_back(std::move(Pending[Next].Op));
  Ops = std::move(Merged);
}

} // namespace neverd

#endif // NEVERD_LIB_IR_MED_LOWER_INSERTPENDING_H
