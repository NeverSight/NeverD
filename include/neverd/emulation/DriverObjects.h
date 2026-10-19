//===- DriverObjects.h - Explicit finite named driver namespace -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit finite named driver namespace.
///

#ifndef NEVERD_EMULATION_DRIVEROBJECTS_H
#define NEVERD_EMULATION_DRIVEROBJECTS_H
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <optional>
#include <string>
#include <vector>

namespace neverd::emulation {
struct DriverObjectNamespace {
  // Complete additional named driver inventory. Each listed object explicitly
  // has an empty device list; its unprovided image/dispatch fields stay opaque.
  // The executing driver is always included independently of this inventory.
  std::vector<std::string> EmptyDrivers;
};
bool validDriverObjectName(llvm::StringRef Name);
llvm::Error
validateDriverObjectNamespace(const std::optional<DriverObjectNamespace> &Names,
                              llvm::StringRef OwnServiceName);
} // namespace neverd::emulation
#endif
