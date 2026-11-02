//===- MedStackDeltaCache.h - Scoped stack offset queries -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Reuse completed entry-relative stack queries within one ABI recovery.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_ABI_MEDSTACKDELTACACHE_H
#define NEVERD_IR_MED_ABI_MEDSTACKDELTACACHE_H

#include "../pass/MedEntryStackOffsets.h"
#include "MedABIPassDetail.h"

#include "llvm/ADT/DenseMap.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <tuple>

namespace neverd {

/// Own the definition index and root answers for one unchanged function and
/// target. Any operation, operand or PHI change requires invalidate(). Only
/// depth-zero queries with a definition are retained, at most one per indexed
/// identity. Other queries use the existing depth-bounded worker unchanged.
class MedStackDeltaCache {
public:
  MedStackDeltaCache(const MedFunc &Func, const TargetRegInfo &TRI,
                     BinaryFormat Format = BinaryFormat::Unknown)
      : Func(Func), TRI(TRI), Format(Format), Definitions(Func),
        EntryOffsets(Func, TRI.TheArch, Format) {}
  MedStackDeltaCache(const MedStackDeltaCache &) = delete;
  MedStackDeltaCache &operator=(const MedStackDeltaCache &) = delete;

  std::optional<int64_t> query(const MedVar &Value, int Depth = 0);
  void invalidate();
  MedDefSiteIndex &definitions() { return Definitions; }

  struct Statistics {
    size_t Queries = 0;
    size_t Hits = 0;
  };
  const Statistics &statistics() const { return Stats; }
  size_t retainedAnswers() const { return Answers.size(); }

private:
  struct Answer {
    uint16_t Size;
    Arch TheArch;
    int16_t RenameTag;
    std::optional<int64_t> Delta;
  };
  const MedFunc &Func;
  const TargetRegInfo &TRI;
  BinaryFormat Format;
  MedDefSiteIndex Definitions;
  MedEntryStackOffsets EntryOffsets;
  llvm::DenseMap<std::tuple<int, int, int, uint64_t>, Answer> Answers;
  Statistics Stats;
};

} // namespace neverd

#endif // NEVERD_IR_MED_ABI_MEDSTACKDELTACACHE_H
