//===- DriverPciConfiguration.h - Owned PCI configuration register state --===//
///
/// \file
/// Owned PCI configuration register state.
///

#ifndef NEVERD_EMULATION_DRIVERPCICONFIGURATION_H
#define NEVERD_EMULATION_DRIVERPCICONFIGURATION_H
#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>
namespace neverd::emulation {
struct DriverOptions;
struct DriverPciFunction {
  uint16_t Segment = 0;
  uint8_t Bus = 0, Device = 0, Function = 0;
  std::vector<uint8_t> Configuration;
  // Both masks are required for a known write policy. Ordinary read-only
  // registers use zero masks. Neither mask grants BAR/device/DMA semantics.
  std::optional<std::vector<uint8_t>> WriteMask, WriteOneToClearMask;
};
struct DriverPciConfiguration {
  // Encoded segment<<8 | bus bounds observed by the 19041 HAL read wrapper.
  uint32_t MinimumBusNumber = 0, MaximumBusNumber = 0;
  std::vector<DriverPciFunction> Devices;
};
inline constexpr size_t DriverPciConfigurationBytes = 4096;
inline constexpr size_t MaxDriverPciFunctions = 32;
llvm::Error validateDriverPciConfiguration(const DriverOptions &);
} // namespace neverd::emulation
#endif
