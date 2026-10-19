///
/// \file
/// DriverRegistryHivesJSON.
///

#ifndef NEVERD_EMULATION_DRIVERREGISTRYHIVESJSON_H
#define NEVERD_EMULATION_DRIVERREGISTRYHIVESJSON_H
#include "neverd/emulation/DriverRegistryHives.h"

#include "llvm/Support/JSON.h"
namespace neverd::emulation {
llvm::Expected<std::vector<DriverRegistryHive>>
parseDriverRegistryHives(const llvm::json::Value &);
llvm::json::Value
driverRegistryHivesJSON(const std::optional<std::vector<DriverRegistryHive>> &);
} // namespace neverd::emulation
#endif
