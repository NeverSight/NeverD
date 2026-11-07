//===- ResolverExactDefinitions.h - Immutable exact definition index -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_JUMPTABLE_RESOLVEREXACTDEFINITIONS_H
#define NEVERD_IR_LOW_JUMPTABLE_RESOLVEREXACTDEFINITIONS_H

#include "neverd/ir/Limits.h"
#include "neverd/ir/low/LowIR.h"

#include "llvm/ADT/ArrayRef.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <vector>

namespace neverd::detail {

/// Query-local index over an immutable, already admitted operation prefix.
/// This preserves exact Space/Offset matching, not register-lane semantics.
class ResolverExactDefinitions {
  using Entry = std::tuple<VnodeSpace, uint64_t, int>;
  static_assert(sizeof(Entry) <= 32);
  std::vector<Entry> Positions;
  const size_t MaxOps;

public:
  explicit ResolverExactDefinitions(
      size_t MaxOps = ::neverd::limits::ir::kResolverExactDefinitionIndexMaxOps)
      : MaxOps(MaxOps) {}

  template <typename Consume>
  int previous(llvm::ArrayRef<LowOp> Ops, int From, const NdVar &Value,
               Consume &&ConsumeWork) {
    const int Last = std::min(From, static_cast<int>(Ops.size()) - 1);
    if (Last < 0)
      return -1;
    // Pay the first visited operation before allocating any index. In
    // particular, exhausted evidence retains the immediate refusal path.
    if (!ConsumeWork(1))
      return -1;
    const auto Matches = [&](int I) {
      return Ops[I].Output.Space == Value.Space &&
             Ops[I].Output.Offset == Value.Offset;
    };
    if (Matches(Last))
      return Last;
    if (Ops.size() > MaxOps) {
      for (int I = Last - 1; I >= 0; --I) {
        if (!ConsumeWork(1))
          return -1;
        if (Matches(I))
          return I;
      }
      return -1;
    }
    if (Positions.empty()) {
      Positions.reserve(Ops.size());
      for (int I = 0; I < static_cast<int>(Ops.size()); ++I)
        Positions.emplace_back(Ops[I].Output.Space, Ops[I].Output.Offset, I);
      std::sort(Positions.begin(), Positions.end());
    }
    const auto After =
        std::upper_bound(Positions.begin(), Positions.end(),
                         Entry{Value.Space, Value.Offset, Last});
    int Position = -1;
    if (After != Positions.begin()) {
      const auto &[Space, Offset, Index] = After[-1];
      if (Space == Value.Space && Offset == Value.Offset)
        Position = Index;
    }
    // The first charge above also covers the returned definition. Replaying
    // the rest preserves the cold scan's remaining budget and failure point.
    const size_t Rest = static_cast<size_t>(Last - std::max(Position, 0));
    if (Rest != 0 && !ConsumeWork(Rest))
      return -1;
    return Position;
  }
};

} // namespace neverd::detail

#endif
