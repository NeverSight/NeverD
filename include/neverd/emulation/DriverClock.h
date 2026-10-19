//===- DriverClock.h - Explicit guest boot-clock facts --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit guest boot-clock facts.
///

#ifndef NEVERD_EMULATION_DRIVERCLOCK_H
#define NEVERD_EMULATION_DRIVERCLOCK_H

#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>

namespace neverd::emulation {
inline llvm::Error
validateDriverSystemClockIncrement(std::optional<uint32_t> Increment) {
  if (Increment && !*Increment)
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "system_clock_increment_100ns must be a positive uint32");
  return llvm::Error::success();
}
} // namespace neverd::emulation
#endif
