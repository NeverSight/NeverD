//===- WindowsProcessModules.h - Guest startup linking ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_WINDOWS_PROCESS_MODULES_H
#define NEVERD_EMULATION_WINDOWS_PROCESS_MODULES_H
#include "WindowsProcess.h"

namespace neverd::emulation::windows_process {
struct Module {
  Image Loaded;
  std::map<std::string, uint64_t> Names;
  std::map<uint32_t, uint64_t> Ordinals;
};
struct Program {
  std::vector<Module> Modules;
  std::vector<ModuleIdentity> Identities;
  std::vector<size_t> InitializationOrder;
  /// One exact provider/name gate per process, independent of the caller image.
  std::vector<Import> Gates;
};
llvm::Expected<Program> loadProgram(const std::filesystem::path &Path,
                                    const ProcessOptions &Options,
                                    const ExecutionBudget &Budget,
                                    VirtualMemory &Memory);
} // namespace neverd::emulation::windows_process
#endif
