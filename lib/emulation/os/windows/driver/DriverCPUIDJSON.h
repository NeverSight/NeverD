///
/// \file
/// DriverCPUIDJSON.
///

#ifndef NEVERD_EMULATION_DRIVERCPUIDJSON_H
#define NEVERD_EMULATION_DRIVERCPUIDJSON_H
#include "neverd/emulation/DriverCPUID.h"

#include "llvm/Support/JSON.h"
namespace neverd::emulation {
llvm::Expected<DriverCPUIDPolicy>
parseDriverCPUIDPolicy(const llvm::json::Value &V);
llvm::json::Object driverCPUIDPolicyJSON(const DriverCPUIDPolicy &P);
} // namespace neverd::emulation
#endif
