//===- DriverDeviceProperties.h - Explicit PDO property observations ------===//
///
/// \file
/// Explicit PDO property observations.
///

#ifndef NEVERD_EMULATION_DRIVERDEVICEPROPERTIES_H
#define NEVERD_EMULATION_DRIVERDEVICEPROPERTIES_H
#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace neverd::emulation {
struct DriverDeviceProperty {
  uint32_t Property = 0;
  // Final native query bytes, including string terminators. Null records an
  // observed missing registry property where its error contract is supported.
  std::optional<std::vector<uint8_t>> Bytes;
};
inline constexpr size_t MaxDriverDevicePropertyBytes = 65536;
llvm::Error validateDriverDeviceProperties(
    const std::optional<std::vector<DriverDeviceProperty>> &);
} // namespace neverd::emulation
#endif
