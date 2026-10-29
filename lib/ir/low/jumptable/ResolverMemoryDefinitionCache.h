//===- ResolverMemoryDefinitionCache.h - Frozen memory scans -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_JUMPTABLE_RESOLVERMEMORYDEFINITIONCACHE_H
#define NEVERD_IR_LOW_JUMPTABLE_RESOLVERMEMORYDEFINITIONCACHE_H

#include "neverd/ir/Limits.h"
#include "neverd/ir/low/LowIR.h"

#include "llvm/ADT/ArrayRef.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>

namespace neverd::detail {

enum class ResolverMemoryCandidate {
  None,
  Store,
  Atomic,
  Call,
  Intrinsic,
  OrderedAccess
};

/// Classifies possible writers and ordered accesses. Operand shapes, aliasing,
/// intrinsic effects and call contracts remain the consuming proof's decisions.
inline ResolverMemoryCandidate resolverMemoryCandidate(const LowOp &Op) {
  if (Op.MemoryOrdering != NdMemoryOrdering::None)
    return ResolverMemoryCandidate::OrderedAccess;
  switch (Op.Opcode) {
  case NdOp::STORE:
    return ResolverMemoryCandidate::Store;
  case NdOp::ATOMIC_XCHG:
  case NdOp::ATOMIC_ADD:
  case NdOp::ATOMIC_CMPXCHG:
    return ResolverMemoryCandidate::Atomic;
  case NdOp::CALL:
  case NdOp::INDIR_CALL:
    return ResolverMemoryCandidate::Call;
  case NdOp::INTRINSIC:
    return ResolverMemoryCandidate::Intrinsic;
  default:
    return ResolverMemoryCandidate::None;
  }
}

/// One immutable graph owns this table. It remembers completed structural
/// scans, never memory values or proof assumptions. A query with authenticated
/// memcpy occurrences must use its ordinary scan: even a non-call occurrence
/// must reach the consumer's rejection and per-operation inventory charge.
class ResolverMemoryDefinitionCache {
  struct Entry {
    int Block = -1;
    int Before = 0;
    int Position = -1;
  };
  static constexpr size_t Capacity =
      ::neverd::limits::ir::kResolverMemoryDefinitionCacheCapacity;
  using Table = std::array<Entry, Capacity>;
  static_assert(sizeof(Table) <= 12 * 1024);
  std::unique_ptr<Table> Entries;

public:
  template <typename Consume>
  std::optional<int> previous(int Block, llvm::ArrayRef<LowOp> Ops, int Before,
                              Consume &&ConsumeWork) {
    Before = std::min(Before, static_cast<int>(Ops.size()));
    if (Before <= 0)
      return -1;
    const bool Retain = Before >= 16;
    const size_t Slot =
        ((uint64_t{static_cast<unsigned>(Block)} * 2654435761U) ^
         (uint64_t{static_cast<unsigned>(Before)} * 2246822519U)) %
        Capacity;
    if (Retain && Entries) {
      const auto &Saved = (*Entries)[Slot];
      if (Saved.Block == Block && Saved.Before == Before) {
        if (!ConsumeWork(Before - std::max(Saved.Position, 0)))
          return std::nullopt;
        return Saved.Position;
      }
    }
    int Position = -1;
    for (int I = Before - 1; I >= 0; --I) {
      if (!ConsumeWork(1))
        return std::nullopt;
      if (resolverMemoryCandidate(Ops[I]) != ResolverMemoryCandidate::None) {
        Position = I;
        break;
      }
    }
    // Retain only walks long enough to amortize a lookup; dense writers and
    // short prefixes do not pay for allocating the fixed table.
    if (Retain && Before - std::max(Position, 0) >= 4) {
      if (!Entries)
        Entries = std::make_unique<Table>();
      (*Entries)[Slot] = {Block, Before, Position};
    }
    return Position;
  }
};

} // namespace neverd::detail

#endif
