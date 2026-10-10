//===- DriverCPUID.h - Explicit driver CPU identification inputs ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_WINDOWS_DRIVERCPUID_H
#define NEVERD_EMULATION_WINDOWS_DRIVERCPUID_H

#include "neverd/emulation/DriverSession.h"

#include "llvm/Support/JSON.h"

namespace neverd::emulation::driver_cpuid {
llvm::Error validate(llvm::ArrayRef<DriverCPUID> Entries);
llvm::Expected<std::vector<DriverCPUID>> parse(const llvm::json::Value &Value);
llvm::json::Array toJSON(llvm::ArrayRef<DriverCPUID> Entries);
llvm::Expected<std::array<uint32_t, 4>>
query(llvm::ArrayRef<DriverCPUID> Entries, uint32_t Leaf, uint32_t Subleaf);
} // namespace neverd::emulation::driver_cpuid

#endif
