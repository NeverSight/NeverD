//===- DriverBootstrapModules.h - Preexisting kernel module ownership -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Preexisting kernel module ownership.
///

#ifndef NEVERD_EMULATION_DRIVERBOOTSTRAPMODULES_H
#define NEVERD_EMULATION_DRIVERBOOTSTRAPMODULES_H

#include "llvm/Support/Error.h"

namespace neverd::emulation {
class DriverSessionState;
class ModuleBindings;
inline constexpr char DriverBootstrapID[] = "$driver";
inline constexpr char KernelReferenceBootstrapID[] = "$kernel-reference";
/// Register existing source/mapping ownership without replaying initialization.
llvm::Error adoptDriverBootstrapModules(DriverSessionState &, ModuleBindings &);
} // namespace neverd::emulation
#endif
