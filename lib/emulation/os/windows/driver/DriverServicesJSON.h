///
/// \file
/// DriverServicesJSON.
///

#ifndef NEVERD_EMULATION_DRIVERSERVICESJSON_H
#define NEVERD_EMULATION_DRIVERSERVICESJSON_H
#include "neverd/emulation/DriverServices.h"

#include "llvm/Support/JSON.h"
namespace neverd::emulation {
llvm::Expected<std::vector<DriverServiceImage>>
parseDriverServiceImages(const llvm::json::Value &);
llvm::json::Value
driverServiceImagesJSON(const std::vector<DriverServiceImage> &);
} // namespace neverd::emulation
#endif
