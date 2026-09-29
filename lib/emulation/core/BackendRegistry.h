//===- BackendRegistry.h - Backend selection before execution ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_CORE_BACKENDREGISTRY_H
#define NEVERD_EMULATION_CORE_BACKENDREGISTRY_H
#include "ExecutionBackend.h"

#include "neverd/emulation/ExecutionBackend.h"

namespace neverd::emulation {
struct BackendSelection {
  std::unique_ptr<ExecutionBackend> CPU;
  ExecutionBackendKind Kind;
  std::string Reason;
};
llvm::Expected<BackendSelection>
createExecutionBackend(ExecutionBackendKind Kind, ExecutionContract Contract,
                       uint64_t MemoryLimit);
} // namespace neverd::emulation
#endif
