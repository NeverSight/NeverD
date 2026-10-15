//===- ResolverDefinitionCache.h - Frozen-graph definition searches -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Memoizes completed backward searches, without retaining value proofs.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_JUMPTABLE_RESOLVERDEFINITIONCACHE_H
#define NEVERD_IR_LOW_JUMPTABLE_RESOLVERDEFINITIONCACHE_H

#include "ResolverLaneView.h"

#include "llvm/ADT/ArrayRef.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>

namespace neverd::detail {

/// Owned by one immutable resolver graph. Block IDs and operations must remain
/// unchanged for its lifetime. Register metadata must be immutable as well.
/// Entries only identify the next possible definition; calls always remain
/// candidates for the caller's ordinary ABI checks. No proof, root, predicate,
/// memory state, or incomplete search is retained.
class ResolverDefinitionCache {
  struct Entry {
    const TargetRegInfo *Registers = nullptr;
    uint64_t Offset = 0;
    int Block = -1;
    int Before = 0;
    int Position = -1;
    uint16_t Size = 0;
    VnodeSpace Space = VnodeSpace::CONST;
  };
  static constexpr size_t Capacity = 4096;
  using Table = std::array<Entry, Capacity>;
  static_assert(sizeof(Table) <= 128 * 1024);
  std::unique_ptr<Table> Entries;

public:
  template <typename Consume>
  std::optional<int> previous(int Block, llvm::ArrayRef<LowOp> Ops, int Before,
                              const NdVar &Value, const ResolverLaneView &Query,
                              const TargetRegInfo &TRI,
                              ResolverLaneViews &Views, Consume &&ConsumeWork) {
    Before = std::min(Before, static_cast<int>(Ops.size()));
    if (Before <= 0)
      return -1;
    // Short prefixes cost little to scan and would displace longer searches.
    // Keep their ordinary walk without allocating or consulting the table.
    const bool Retain = Before >= 16;
    size_t Slot = 0;
    if (Retain)
      Slot = (Value.Offset ^ (Value.Offset >> 32) ^
              (uint64_t{static_cast<unsigned>(Block)} * 2654435761U) ^
              (uint64_t{static_cast<unsigned>(Before)} * 2246822519U) ^
              (uint64_t{Value.Size} * 3266489917U) ^
              (uint64_t{static_cast<unsigned>(Value.Space)} * 668265263U)) %
             Capacity;
    if (Retain && Entries) {
      const auto &Saved = (*Entries)[Slot];
      if (Saved.Registers == &TRI && Saved.Block == Block &&
          Saved.Before == Before && Saved.Offset == Value.Offset &&
          Saved.Size == Value.Size && Saved.Space == Value.Space) {
        // Pay every skipped operation, including the returned candidate. An
        // exhausted replay takes the same incomplete path as the cold walk.
        if (!ConsumeWork(Before - std::max(Saved.Position, 0)))
          return std::nullopt;
        return Saved.Position;
      }
    }

    int Position = -1;
    for (int I = Before - 1; I >= 0; --I) {
      if (!ConsumeWork(1))
        return std::nullopt;
      const LowOp &Op = Ops[I];
      if (Value.isReg() &&
          (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)) {
        Position = I;
        break;
      }
      const auto Output = Views.get(Op.Output);
      if (!Output.Valid || !Query.Valid || Output.Space != Query.Space ||
          Output.Container != Query.Container)
        continue;
      // Keep every writer in the same container. Byte overlap, implicit zero
      // extension and partial-write semantics belong to the consuming proof.
      Position = I;
      break;
    }
    // Fixed storage bounds both retention and lookup work. Hash collisions
    // replace an entry only after the complete search key has been checked.
    if (Retain) {
      if (!Entries)
        Entries = std::make_unique<Table>();
      (*Entries)[Slot] = {&TRI,     Value.Offset, Block,      Before,
                          Position, Value.Size,   Value.Space};
    }
    return Position;
  }
};

} // namespace neverd::detail

#endif
