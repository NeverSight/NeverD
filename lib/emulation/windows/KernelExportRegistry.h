//===- KernelExportRegistry.h - Guest kernel export identity --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A session-owned export namespace shared by static and dynamic resolution.
/// Availability is independent of whether an API has an execution model.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_KERNELEXPORTREGISTRY_H
#define NEVERD_EMULATION_KERNELEXPORTREGISTRY_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <map>
#include <string>

namespace neverd::emulation {
struct DriverOptions;
struct DriverImport;

class KernelExportRegistry {
public:
  struct Export {
    std::string Name;
    std::string Module;
    uint64_t Address = 0;
  };

  llvm::Error initialize(const DriverOptions &Options);
  llvm::Expected<uint64_t> bindImport(const DriverImport &Import);
  llvm::Expected<uint64_t> resolve(llvm::StringRef Name) const;
  const Export *lookup(uint64_t Address) const;

private:
  llvm::Expected<uint64_t> insert(llvm::StringRef Name);
  std::map<std::string, uint64_t, std::less<>> Names;
  std::map<uint64_t, Export> Exports;
};
} // namespace neverd::emulation
#endif
