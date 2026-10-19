//===- DriverFirmwareJSON.h - Explicit firmware scenario transport --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit firmware scenario transport.
///

#ifndef NEVERD_EMULATION_DRIVERFIRMWAREJSON_H
#define NEVERD_EMULATION_DRIVERFIRMWAREJSON_H
#include "neverd/emulation/DriverFirmware.h"

#include "llvm/Support/JSON.h"

namespace neverd::emulation {
llvm::Expected<DriverPciMcfg>
parseDriverPciMcfg(const llvm::json::Value &Value);
llvm::json::Object driverPciMcfgJSON(const DriverPciMcfg &Facts);
llvm::Expected<DriverSmbios> parseDriverSmbios(const llvm::json::Value &Value);
llvm::json::Object driverSmbiosJSON(const DriverSmbios &Facts);
llvm::Expected<DriverAcpiTables>
parseDriverAcpiTables(const llvm::json::Value &Value);
llvm::json::Object driverAcpiTablesJSON(const DriverAcpiTables &Facts);
} // namespace neverd::emulation
#endif
