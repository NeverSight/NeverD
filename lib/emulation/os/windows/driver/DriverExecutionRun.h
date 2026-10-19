//===- DriverExecutionRun.h - Persistent driver preparation --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Persistent driver preparation.
///

#ifndef NEVERD_EMULATION_DRIVEREXECUTIONRUN_H
#define NEVERD_EMULATION_DRIVEREXECUTIONRUN_H

#include "DriverSessionState.h"

#include "neverd/emulation/ExecutionRun.h"

namespace neverd::emulation {
/// Internal adapter preparation. State is a view owned by Run's environment;
/// it must never outlive Run. Limits must match DriverOptions explicitly. The
/// public factory delegates here. LegacyImportBindings is only the old one-shot
/// facade's unknown-import policy; all execution uses this same environment.
struct PreparedDriverExecution {
  std::unique_ptr<ExecutionRun> Run;
  DriverSessionState *State = nullptr;
  llvm::Error finishStoppedFailure(const ExecutionRunEvent &);

private:
  friend llvm::Expected<PreparedDriverExecution>
  createDriverExecutionRun(const std::filesystem::path &, const DriverOptions &,
                           ExecutionRunOptions,
                           std::shared_ptr<modules::ModuleSet>,
                           DriverSessionState::ResourceInputs, bool);
  std::function<llvm::Error(const ExecutionRunEvent &)> FinishFailure;
};
llvm::Expected<PreparedDriverExecution> createDriverExecutionRun(
    const std::filesystem::path &, const DriverOptions &, ExecutionRunOptions,
    std::shared_ptr<modules::ModuleSet> = {},
    DriverSessionState::ResourceInputs = {}, bool LegacyImportBindings = false);
} // namespace neverd::emulation
#endif
