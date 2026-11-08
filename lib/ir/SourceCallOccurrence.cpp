//===- SourceCallOccurrence.cpp - Architecture-qualified call identity ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks complete target widths without coupling structural LowIR validation
/// to the aggregate IR library's architecture register descriptions.
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/SourceCallOccurrence.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowIR.h"

namespace neverd {

std::optional<SourceCallOccurrenceKey>
sourceCallOccurrenceKey(const LowOp &Operation, Arch Architecture) {
  if (Architecture == Arch::Unknown ||
      Operation.Inputs[0].Size != getTargetRegInfo(Architecture).PointerSize)
    return std::nullopt;
  return sourceCallOccurrenceKey(Operation);
}

} // namespace neverd
