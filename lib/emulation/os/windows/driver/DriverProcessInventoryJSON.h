//===- DriverProcessInventoryJSON.h - Process metadata wire form ----------===//
///
/// \file
/// Process metadata wire form.
///

#ifndef NEVERD_EMULATION_DRIVERPROCESSINVENTORYJSON_H
#define NEVERD_EMULATION_DRIVERPROCESSINVENTORYJSON_H
#include "neverd/emulation/DriverProcessInventory.h"

#include "llvm/Support/JSON.h"
namespace neverd::emulation {
llvm::Expected<DriverProcessInventory>
parseDriverProcessInventory(const llvm::json::Value &);
llvm::json::Value driverProcessInventoryJSON(const DriverProcessInventory &);
} // namespace neverd::emulation
#endif
