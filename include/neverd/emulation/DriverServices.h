//===- DriverServices.h - Explicit guest driver image inventory -----------===//
///
/// \file
/// Explicit guest driver image inventory.
///

#ifndef NEVERD_EMULATION_DRIVERSERVICES_H
#define NEVERD_EMULATION_DRIVERSERVICES_H
#include "llvm/Support/Error.h"

#include <filesystem>
#include <string>
#include <vector>
namespace neverd::emulation {
struct DriverOptions;
struct DriverServiceImage {
  std::string ServiceName;
  std::string ImagePath;
  std::filesystem::path Source;
  uint64_t LoadAddress = 0;
};
llvm::Error validateDriverServices(const DriverOptions &);
} // namespace neverd::emulation
#endif
