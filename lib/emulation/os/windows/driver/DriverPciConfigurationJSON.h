///
/// \file
/// DriverPciConfigurationJSON.
///

#ifndef NEVERD_DRIVER_PCI_CONFIGURATION_JSON_H
#define NEVERD_DRIVER_PCI_CONFIGURATION_JSON_H
#include "neverd/emulation/DriverPciConfiguration.h"

#include "llvm/Support/JSON.h"
namespace neverd::emulation {
llvm::Expected<DriverPciConfiguration>
parseDriverPciConfiguration(const llvm::json::Value &);
llvm::json::Value driverPciConfigurationJSON(const DriverPciConfiguration &);
} // namespace neverd::emulation
#endif
