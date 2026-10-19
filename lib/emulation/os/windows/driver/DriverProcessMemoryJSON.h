//===- DriverProcessMemoryJSON.h - Process RAM scenario wire form ---------===//
// NeverD Decompiler
///
/// \file
/// Process RAM scenario wire form.
///

#ifndef NEVERD_DRIVER_PROCESS_MEMORY_JSON_H
#define NEVERD_DRIVER_PROCESS_MEMORY_JSON_H
#include "neverd/emulation/DriverProcessImages.h"
#include "neverd/emulation/DriverProcessMemory.h"

#include "llvm/Support/JSON.h"
namespace neverd::emulation {
llvm::Expected<std::vector<DriverProcessMemoryRegion>>
parseDriverProcessMemory(const llvm::json::Value &V);
llvm::json::Value
driverProcessMemoryJSON(const std::vector<DriverProcessMemoryRegion> &Regions);
llvm::Expected<std::vector<DriverProcessImageMapping>>
parseDriverProcessImages(const llvm::json::Value &V);
llvm::json::Value
driverProcessImagesJSON(const std::vector<DriverProcessImageMapping> &Images);
} // namespace neverd::emulation
#endif
