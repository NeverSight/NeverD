//===- WindowsProcessLoader.h - Runtime module transactions -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_WINDOWS_PROCESS_LOADER_H
#define NEVERD_EMULATION_WINDOWS_PROCESS_LOADER_H
#include "WindowsProcessLifetime.h"

namespace neverd::emulation::windows_process {
class Loader final {
public:
  struct Operation {
    LoaderRequest Request;
    std::optional<ModuleRef> Root;
    std::vector<ModuleRef> Unload, Created;
    std::vector<std::pair<ModuleRef, ModuleRef>> Edges;
    std::unique_ptr<Lifetime> Notifications;
    uint64_t Value = 0;
    uint32_t Error = 0;
    bool Acquired = false, Initializing = false;
  };
  Loader(Program &Program, VirtualMemory &Virtual, AddressSpace &Memory,
         Environment &Environment, ExecutionBackend &CPU,
         const ExecutionBudget &Budget)
      : P(Program), Virtual(Virtual), Memory(Memory), Env(Environment),
        CPU(CPU), Budget(Budget) {}
  llvm::Expected<Operation> begin(const LoaderRequest &Request);
  /// Complete one callback batch; another batch may be needed for failed
  /// attach.
  llvm::Error complete(Operation &Operation);

private:
  llvm::Expected<Operation> start(const LoaderRequest &Request);
  llvm::Error publish(const ModuleLink &Linked);
  llvm::Expected<std::vector<ModuleRef>> unreferenced() const;
  llvm::Error finishUnload(Operation &Operation);
  Program &P;
  VirtualMemory &Virtual;
  AddressSpace &Memory;
  Environment &Env;
  ExecutionBackend &CPU;
  const ExecutionBudget &Budget;
};
} // namespace neverd::emulation::windows_process
#endif
