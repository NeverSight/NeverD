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
  if (Architecture == Arch::Unknown)
    return std::nullopt;
  // N32 transfers the complete 64-bit jump register, including its high
  // bits, even though the source-level function pointer occupies four bytes.
  const auto &TRI = getTargetRegInfo(Architecture);
  const auto Width = isMIPS(Architecture) ? TRI.FullRegWidth : TRI.PointerSize;
  if (Operation.Inputs[0].Size != Width)
    return std::nullopt;
  return sourceCallOccurrenceKey(Operation);
}

} // namespace neverd
