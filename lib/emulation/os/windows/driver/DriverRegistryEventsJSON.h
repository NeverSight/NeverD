//===- DriverRegistryEventsJSON.h - Registry event serialization ----------===//
// NeverD Decompiler
///
/// \file
/// Registry event serialization.
///

#ifndef NEVERD_DRIVERREGISTRYEVENTSJSON_H
#define NEVERD_DRIVERREGISTRYEVENTSJSON_H
#include "neverd/emulation/DriverRegistryEvents.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/JSON.h"
namespace neverd::emulation {
llvm::Expected<std::vector<DriverRegistryEvent>>
parseDriverRegistryEvents(const llvm::json::Value &);
llvm::json::Array driverRegistryEventsJSON(llvm::ArrayRef<DriverRegistryEvent>);
llvm::json::Array
    driverRegistryEventResultsJSON(llvm::ArrayRef<DriverRegistryEventResult>);
} // namespace neverd::emulation
#endif
