//===- DriverFiniteFileSystemJSON.h - Guest volume inventory wire form
//----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Guest volume inventory wire form.
///

#ifndef NEVERD_EMULATION_DRIVERFINITEFILESYSTEMJSON_H
#define NEVERD_EMULATION_DRIVERFINITEFILESYSTEMJSON_H
#include "neverd/emulation/DriverFiniteFileSystem.h"

#include "llvm/Support/JSON.h"
namespace neverd::emulation {
llvm::Expected<std::vector<DriverFiniteFileSystemVolume>>
parseDriverFiniteFileSystem(const llvm::json::Value &Value);
llvm::json::Value driverFiniteFileSystemJSON(
    const std::optional<std::vector<DriverFiniteFileSystemVolume>> &Volumes);
} // namespace neverd::emulation
#endif
