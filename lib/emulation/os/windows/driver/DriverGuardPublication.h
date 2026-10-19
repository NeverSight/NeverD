//===- DriverGuardPublication.h - Transactional Native CFG policy -*- C++
//-*-===//
///
/// \file
/// Transactional Native CFG policy.
///

#ifndef NEVERD_EMULATION_DRIVERGUARDPUBLICATION_H
#define NEVERD_EMULATION_DRIVERGUARDPUBLICATION_H
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <memory>
namespace neverd::emulation {
class DriverSessionState;
class ModulePublication;
struct ModuleInstance;
llvm::Expected<std::unique_ptr<ModulePublication>>
prepareDriverGuardPublication(DriverSessionState &,
                              llvm::ArrayRef<ModuleInstance>,
                              std::unique_ptr<ModulePublication>);
} // namespace neverd::emulation
#endif
