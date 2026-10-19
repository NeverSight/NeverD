///
/// \file
/// DriverNdisConfiguration.
///

#ifndef NEVERD_EMULATION_DRIVERNDISCONFIGURATION_H
#define NEVERD_EMULATION_DRIVERNDISCONFIGURATION_H
#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
namespace neverd::emulation {
struct DriverOptions;
struct DriverNdisConfigurationBinding {
  // Observed externally owned opaque NDIS identity with lazy registry opening.
  // Its provider lives for the session; no private NDIS object is synthesized.
  uint64_t Handle = 0;
  std::string RegistryPath;
};
struct DriverNdisConfiguration {
  std::vector<DriverNdisConfigurationBinding> Bindings;
  std::optional<uint32_t> Version;
};
llvm::Error validateDriverNdisConfiguration(const DriverOptions &);
} // namespace neverd::emulation
#endif
