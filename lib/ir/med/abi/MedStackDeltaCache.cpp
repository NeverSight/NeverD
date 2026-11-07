//===- MedStackDeltaCache.cpp - Scoped stack offset queries ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Cache root answers without lending depth allowance to nested queries.
//===----------------------------------------------------------------------===//

#include "MedStackDeltaCache.h"

namespace neverd {

std::optional<int64_t> MedStackDeltaCache::query(const MedVar &Value,
                                                 int Depth) {
  ++Stats.Queries;
  if (Depth != 0 || Value.isConst())
    return stackPtrDelta(Func, TRI, Value, Depth, &Definitions, Format,
                         &EntryOffsets);
  const auto Key = std::make_tuple(static_cast<int>(Value.Kind), Value.Id,
                                   Value.SSAVer, Value.RegOff);
  // The entry-offset fallback validates the exact value occurrence, even
  // when the local ABI walk only uses its SSA definition identity.
  if (const auto It = Answers.find(Key);
      It != Answers.end() && It->second.Size == Value.Size &&
      It->second.TheArch == Value.TheArch &&
      It->second.RenameTag == Value.RenameTag) {
    ++Stats.Hits;
    return It->second.Delta;
  }
  const auto Site = Definitions.find(Value);
  const auto Result = stackPtrDelta(Func, TRI, Value, Depth, &Definitions,
                                    Format, &EntryOffsets);
  // Missing and ambiguous definitions are already cheap indexed answers.
  // Keeping only actual definitions bounds retention by the source graph,
  // even when a caller asks about arbitrary absent value identities.
  if (Site.Op || Site.Phi)
    Answers.insert_or_assign(
        Key, Answer{Value.Size, Value.TheArch, Value.RenameTag, Result});
  return Result;
}

void MedStackDeltaCache::invalidate() {
  Answers.clear();
  Definitions.invalidate();
  EntryOffsets.invalidate();
}

} // namespace neverd
