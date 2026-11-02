//===- X86RegistrationSEHException.h - SEH field identities ------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_X86REGISTRATIONSEHEXCEPTION_H
#define NEVERD_BACKEND_LLVM_X86_X86REGISTRATIONSEHEXCEPTION_H

#include "neverd/ir/RegistrationState.h"

#include <map>
#include <optional>
#include <utility>

namespace neverd {
struct MedFunc;

/// Bind runtime-field receipts to exact, four-byte source loads. The native
/// emitter and independent source replay share occurrence validation; actual
/// generated addresses still require the current filter's runtime bridge.
using X86RegistrationSEHExceptionAccesses =
    std::map<std::pair<va_t, int>, RegistrationSEHExceptionAccess>;
std::optional<X86RegistrationSEHExceptionAccesses>
getX86RegistrationSEHExceptionAccesses(const MedFunc &Function);

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86_X86REGISTRATIONSEHEXCEPTION_H
