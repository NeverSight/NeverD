//===- DriverScenario.h - Shared scenario validation ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// One contract for native options and parsed JSON scenarios.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_DRIVERSCENARIO_H
#define NEVERD_EMULATION_DRIVERSCENARIO_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>

namespace neverd::emulation {
struct DriverOptions;
struct DriverRequest;
struct DriverPnpOperation;
struct DriverPowerOperation;
struct DriverPnpDevice;
struct DriverPowerPolicyEvent;

llvm::Error validateDriverPowerPolicyEvent(const DriverPowerPolicyEvent &Event);

llvm::Error validateDriverPnpOperation(const DriverPnpOperation &Operation);

llvm::Error validateDriverPowerOperation(const DriverPowerOperation &Operation,
                                         bool RequireDeviceType = false);

llvm::Error validateDriverD3Cold(const DriverPnpDevice &Device);

llvm::Error validateDriverUsbIdle(llvm::ArrayRef<DriverPnpDevice> Devices);

llvm::Error validateDriverResources(llvm::ArrayRef<DriverPnpDevice> Devices,
                                    uint64_t KernelMemorySize);

llvm::Error validateDriverInterrupts(llvm::ArrayRef<DriverPnpDevice> Devices);

llvm::Error validateDriverDma(llvm::ArrayRef<DriverPnpDevice> Devices,
                              uint64_t KernelMemorySize);

llvm::Error validateDriverUserMemory(const DriverRequest &Request);

llvm::Error validateDriverIRPInitialization(const DriverOptions &Options);

llvm::Error validateDriverScenario(const DriverOptions &Options);

} // namespace neverd::emulation

#endif // NEVERD_EMULATION_DRIVERSCENARIO_H
