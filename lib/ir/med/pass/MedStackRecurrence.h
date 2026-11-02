//===- MedStackRecurrence.h - Counted stack recurrence bounds --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bound a changing stack PHI using its current unsigned loop counter.

#ifndef NEVERD_IR_MED_PASS_MEDSTACKRECURRENCE_H
#define NEVERD_IR_MED_PASS_MEDSTACKRECURRENCE_H

#include "neverd/ir/med/MedIR.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace neverd {

struct CountedStackRecurrence {
  MedVar Base;
  int64_t MinimumChange = 0;
  int64_t MaximumChange = 0;
};

/// This is a bound, never an executable stack value. Every return to the
/// header must cross the unsigned guard and both updates exactly once.
std::optional<CountedStackRecurrence>
countedStackRecurrence(const MedFunc &Func, const PhiNode &Stack,
                       size_t &Remaining);

} // namespace neverd

#endif // NEVERD_IR_MED_PASS_MEDSTACKRECURRENCE_H
