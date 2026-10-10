//===- CxxCleanupScope.h - C++ try cleanup membership -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Share source cleanup ownership between region creation and nested checks.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_IR_HIGH_STRUCTURE_CXXCLEANUPSCOPE_H
#define NEVERD_LIB_IR_HIGH_STRUCTURE_CXXCLEANUPSCOPE_H

#include "neverd/loader/ExceptionInfo.h"

namespace neverd {
inline bool cxxTryOwnsCleanup(const CxxExceptionInfo &Cxx, uint32_t TryIndex,
                              int32_t State) {
  if (TryIndex >= Cxx.TryBlocks.size() || State < 0 ||
      size_t(State) >= Cxx.UnwindMap.size() || !Cxx.UnwindMap[State].ActionVA)
    return false;
  const auto &Try = Cxx.TryBlocks[TryIndex];
  if (State < Try.TryLow || State > Try.TryHigh)
    return false;
  for (const auto &Other : Cxx.TryBlocks)
    if ((Other.TryLow != Try.TryLow || Other.TryHigh != Try.TryHigh) &&
        Other.TryLow >= Try.TryLow && Other.TryHigh <= Try.TryHigh &&
        State >= Other.TryLow && State <= Other.TryHigh)
      return false;
  return true;
}
} // namespace neverd

#endif
