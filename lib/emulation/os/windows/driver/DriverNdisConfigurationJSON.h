///
/// \file
/// DriverNdisConfigurationJSON.
///

#ifndef NEVERD_EMULATION_DRIVERNDISCONFIGURATIONJSON_H
#define NEVERD_EMULATION_DRIVERNDISCONFIGURATIONJSON_H
#include "neverd/emulation/DriverNdisConfiguration.h"

#include "llvm/Support/JSON.h"
namespace neverd::emulation {
llvm::Expected<DriverNdisConfiguration>
parseDriverNdisConfiguration(const llvm::json::Value &);
llvm::json::Value driverNdisConfigurationJSON(const DriverNdisConfiguration &);
} // namespace neverd::emulation
#endif
