//===- BackendRegistry.cpp - Backend selection---------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "BackendRegistry.h"

#include "../arch/x86_64/CheckedX64Backend.h"
#include "../unicorn/UnicornBackend.h"
#include "ExecutionDiagnostics.h"

#include "llvm/Support/ErrorHandling.h"

namespace neverd::emulation {
llvm::Expected<ExecutionBackendKind>
parseExecutionBackend(llvm::StringRef Name) {
#define NEVERD_EXECUTION_BACKEND(ID, Text)                                     \
  if (Name == Text)                                                            \
    return ExecutionBackendKind::ID;
#include "neverd/emulation/ExecutionBackend.def"
#undef NEVERD_EXECUTION_BACKEND
  return diagnostic::error(diagnostic::BackendName);
}

llvm::Expected<ExecutionContract> parseExecutionContract(llvm::StringRef Name) {
#define NEVERD_EXECUTION_CONTRACT(ID, Text)                                    \
  if (Name == Text)                                                            \
    return ExecutionContract::ID;
#include "neverd/emulation/ExecutionBackend.def"
#undef NEVERD_EXECUTION_CONTRACT
  return diagnostic::error(diagnostic::ContractName);
}

const char *executionBackendName(ExecutionBackendKind Kind) {
  switch (Kind) {
#define NEVERD_EXECUTION_BACKEND(ID, Text)                                     \
  case ExecutionBackendKind::ID:                                               \
    return Text;
#include "neverd/emulation/ExecutionBackend.def"
#undef NEVERD_EXECUTION_BACKEND
  }
  return execution::Auto;
}

const char *executionContractName(ExecutionContract Contract) {
  switch (Contract) {
#define NEVERD_EXECUTION_CONTRACT(ID, Text)                                    \
  case ExecutionContract::ID:                                                  \
    return Text;
#include "neverd/emulation/ExecutionBackend.def"
#undef NEVERD_EXECUTION_CONTRACT
  }
  return execution::Legacy;
}

llvm::Expected<BackendSelection>
createExecutionBackend(ExecutionBackendKind Kind, ExecutionContract Contract,
                       uint64_t Limit) {
  std::string Reason = execution::ExplicitSelection;
  if (Kind == ExecutionBackendKind::Auto &&
      Contract == ExecutionContract::Legacy) {
    Kind = ExecutionBackendKind::Unicorn;
    Reason = execution::LegacySelection;
  }
  if (Kind == ExecutionBackendKind::Unicorn) {
    // The checked contract must have one ISA admission policy on every host.
    // A software implementation of that smaller contract is not yet provided.
    if (Contract != ExecutionContract::Legacy)
      return diagnostic::error(diagnostic::SoftwareContract);
    auto CPU = UnicornBackend::create(Limit);
    if (!CPU)
      return CPU.takeError();
    return BackendSelection{std::move(*CPU), Kind, std::move(Reason)};
  }
  if (Contract != ExecutionContract::CheckedX64)
    return diagnostic::error(diagnostic::Contract);
  if (Kind == ExecutionBackendKind::Auto) {
    Reason = execution::HostSelection;
#if defined(__linux__) && defined(__x86_64__)
    Kind = ExecutionBackendKind::KVM;
#elif defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    Kind = ExecutionBackendKind::WHP;
#else
    return diagnostic::unavailable(diagnostic::Unavailable);
#endif
  }
  if (Kind != ExecutionBackendKind::KVM && Kind != ExecutionBackendKind::WHP)
    return diagnostic::error(diagnostic::BackendName);
  auto CPU = CheckedX64Backend::create(Kind, Limit);
  if (!CPU)
    return CPU.takeError();
  return BackendSelection{std::move(*CPU), Kind, std::move(Reason)};
}
} // namespace neverd::emulation
