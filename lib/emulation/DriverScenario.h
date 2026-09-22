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

#include "llvm/Support/Error.h"

namespace neverd::emulation {
struct DriverOptions;
struct DriverPnpOperation;

llvm::Error validateDriverPnpOperation(const DriverPnpOperation &Operation);

llvm::Error validateDriverScenario(const DriverOptions &Options);

} // namespace neverd::emulation

#endif // NEVERD_EMULATION_DRIVERSCENARIO_H
