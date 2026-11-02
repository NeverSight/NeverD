//===- CallStackOffsets.cpp - Finite incoming stack offsets --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Reuse complete scalar dataflow covers without sampling memory or paths.
///
//===----------------------------------------------------------------------===//

#include "CallStackOffsets.h"

#include "LowScalarTargets.h"

#include "neverd/Limits.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace neverd {

CallStackOffsets finiteCallStackOffsets(CallRegisterBodyView Function) {
  size_t Remaining = limits::kMaxIncomingStackScalarWork;
  LowScalarOperandQueries Queries;
  for (const auto &Block : Function.Blocks) {
    if (Block.Ops.size() > Remaining)
      return {};
    Remaining -= Block.Ops.size();
    for (const auto &Op : Block.Ops) {
      if ((Op.Opcode != NdOp::INT_ADD && Op.Opcode != NdOp::INT_SUB) ||
          Op.NumInputs != 2 || Op.Output.Size != sizeof(uint64_t))
        continue;
      for (unsigned I = 0; I != 2; ++I)
        if (Op.Inputs[I].Size == sizeof(uint64_t) && !Op.Inputs[I].isConst())
          Queries[&Op].push_back(I);
    }
  }
  const auto Values = finiteScalarOperandValues(Function, Queries, Remaining);
  if (!Values)
    return {};
  CallStackOffsets Result;
  for (const auto &[Query, Cover] : *Values) {
    if (Cover.empty())
      continue;
    auto Lo = std::bit_cast<int64_t>(Cover.front()), Hi = Lo;
    for (uint64_t Bits : Cover) {
      const auto Offset = std::bit_cast<int64_t>(Bits);
      Lo = std::min(Lo, Offset);
      Hi = std::max(Hi, Offset);
    }
    Result.emplace(Query, std::pair{Lo, Hi});
  }
  return Result;
}

} // namespace neverd
