//===- DriverExecutionFactory.h - Public persistent WDM factory --*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Public persistent WDM factory.
///

#ifndef NEVERD_EMULATION_DRIVEREXECUTIONFACTORY_H
#define NEVERD_EMULATION_DRIVEREXECUTIONFACTORY_H
#include "neverd/emulation/ExecutionRunFactory.h"

namespace neverd::emulation {
/// Selects only the explicit windows-driver environment. Relative host files
/// require an absolute environment base_directory or the supplied source
/// origin. Uses the same persistent run/model as the one-shot driver API.
/// image_module/kernel_module explicitly select pinned ModuleSet observations;
/// image retains the path form for compatibility. Kernel-specific facts still
/// require the existing authenticated kernel profile, not an arbitrary PDB.
llvm::Expected<ExecutionRunFactoryResult>
createDriverExecution(ExecutionCreationInput,
                      std::shared_ptr<modules::ModuleSet>,
                      const std::filesystem::path &SourceOrigin = {});
} // namespace neverd::emulation
#endif
