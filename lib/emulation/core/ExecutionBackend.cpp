//===- ExecutionBackend.cpp - Normalized CPU fault names -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "ExecutionBackend.h"

#include "ExecutionDiagnostics.h"

#include "llvm/Support/ErrorHandling.h"

namespace neverd::emulation {
char BackendUnavailableError::ID;
void BackendUnavailableError::log(llvm::raw_ostream &OS) const { OS << Reason; }

char UnsupportedExecutionError::ID;
void UnsupportedExecutionError::log(llvm::raw_ostream &OS) const {
  OS << diagnostic::Instruction;
}

const char *backendFaultKindName(BackendFaultKind Kind) {
  switch (Kind) {
#define NEVERD_BACKEND_FAULT_KIND(Name, Spelling)                              \
  case BackendFaultKind::Name:                                                 \
    return Spelling;
#include "BackendFaults.def"
#undef NEVERD_BACKEND_FAULT_KIND
  }
  llvm_unreachable(diagnostic::UnknownFault);
}

const char *backendAccessKindName(BackendAccessKind Kind) {
  switch (Kind) {
#define NEVERD_BACKEND_ACCESS_KIND(Name, Spelling)                             \
  case BackendAccessKind::Name:                                                \
    return Spelling;
#include "BackendFaults.def"
#undef NEVERD_BACKEND_ACCESS_KIND
  }
  llvm_unreachable(diagnostic::UnknownAccess);
}

} // namespace neverd::emulation
