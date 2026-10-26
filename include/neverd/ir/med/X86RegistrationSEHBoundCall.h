//===- X86RegistrationSEHBoundCall.h - Source SEH calls --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind preserved SEH helpers and their argument recipes to MedIR operations.

#ifndef NEVERD_IR_MED_X86REGISTRATIONSEHBOUNDCALL_H
#define NEVERD_IR_MED_X86REGISTRATIONSEHBOUNDCALL_H

#include "neverd/ir/RegistrationState.h"

#include <map>
#include <optional>

namespace neverd {
struct MedFunc;
struct MedOp;

struct X86RegistrationSEHBoundCall {
  const RegistrationSEHBoundCall *Source = nullptr;
  const MedOp *Operation = nullptr;
  int Block = -1;
  const MedOp *StackRoot = nullptr;
  /// Authenticated source use of the complete native scalar result.
  bool ObservesScalarReturn = false;
};

/// Bind invocation-specific argument recipes to their exact source operations.
std::optional<std::map<va_t, X86RegistrationSEHBoundCall>>
getX86RegistrationSEHBoundCalls(const MedFunc &Function);

} // namespace neverd

#endif // NEVERD_IR_MED_X86REGISTRATIONSEHBOUNDCALL_H
