//===- DriverSectionsJSON.h - Strict named section scenario parser --------===//
// NeverD Decompiler
///
/// \file
/// Strict named section scenario parser.
///

#ifndef NEVERD_DRIVER_SECTIONS_JSON_H
#define NEVERD_DRIVER_SECTIONS_JSON_H
#include "neverd/emulation/DriverSections.h"

#include "llvm/Support/JSON.h"
namespace neverd::emulation {
llvm::Expected<DriverSectionNamespace>
parseDriverSectionNamespace(const llvm::json::Value &V);
llvm::json::Value driverSectionNamespaceJSON(const DriverSectionNamespace &N);
} // namespace neverd::emulation
#endif
