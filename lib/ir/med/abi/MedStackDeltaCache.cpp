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
    return stackPtrDelta(Func, TRI, Value, Depth, &Definitions, Format);
  const auto Key = std::make_tuple(static_cast<int>(Value.Kind), Value.Id,
                                   Value.SSAVer, Value.RegOff);
  if (const auto It = Answers.find(Key); It != Answers.end()) {
    ++Stats.Hits;
    return It->second;
  }
  const auto Site = Definitions.find(Value);
  const auto Result =
      stackPtrDelta(Func, TRI, Value, Depth, &Definitions, Format);
  // Missing and ambiguous definitions are already cheap indexed answers.
  // Keeping only actual definitions bounds retention by the source graph,
  // even when a caller asks about arbitrary absent value identities.
  if (Site.Op || Site.Phi)
    Answers.try_emplace(Key, Result);
  return Result;
}

void MedStackDeltaCache::invalidate() {
  Answers.clear();
  Definitions.invalidate();
}

} // namespace neverd
