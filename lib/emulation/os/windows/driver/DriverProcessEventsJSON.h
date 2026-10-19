//===- DriverProcessEventsJSON.h - Process lifecycle wire form ------------===//
// NeverD Decompiler
///
/// \file
/// Process lifecycle wire form.
///

#ifndef NEVERD_EMULATION_DRIVERPROCESSEVENTSJSON_H
#define NEVERD_EMULATION_DRIVERPROCESSEVENTSJSON_H
#include "neverd/emulation/DriverProcessEvents.h"

#include "llvm/Support/JSON.h"
namespace neverd::emulation {
llvm::Expected<std::vector<DriverProcessEvent>>
parseDriverProcessEvents(const llvm::json::Value &Value);
llvm::json::Value driverProcessEventsJSON(llvm::ArrayRef<DriverProcessEvent>);
llvm::json::Value
    driverProcessEventResultsJSON(llvm::ArrayRef<DriverProcessEventResult>);
} // namespace neverd::emulation
#endif
