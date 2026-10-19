///
/// \file
/// DriverNetworkInterfacesJSON.
///

#ifndef NEVERD_EMULATION_DRIVERNETWORKINTERFACESJSON_H
#define NEVERD_EMULATION_DRIVERNETWORKINTERFACESJSON_H
#include "neverd/emulation/DriverNetworkInterfaces.h"

#include "llvm/Support/JSON.h"
namespace neverd::emulation {
llvm::Expected<DriverNetworkInterfaces>
parseDriverNetworkInterfaces(const llvm::json::Value &);
llvm::json::Value driverNetworkInterfacesJSON(const DriverNetworkInterfaces &);
} // namespace neverd::emulation
#endif
