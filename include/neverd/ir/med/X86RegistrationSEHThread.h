//===- X86RegistrationSEHThread.h - PE32 thread receipts --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind current MedIR thread reads to their independently checked source roots.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_X86REGISTRATIONSEHTHREAD_H
#define NEVERD_IR_MED_X86REGISTRATIONSEHTHREAD_H

#include "neverd/ir/RegistrationState.h"

#include <map>
#include <optional>
#include <utility>

namespace neverd {
struct MedFunc;
struct MedOp;

bool isX86RegistrationThreadSelfLoad(const MedOp &Op,
                                     const RegistrationStateAnalysis &States);
using X86RegistrationSEHThreadAccesses =
    std::map<std::pair<va_t, int>, RegistrationSEHThreadAccess>;
std::optional<X86RegistrationSEHThreadAccesses>
getX86RegistrationSEHThreadAccesses(const MedFunc &Function);

} // namespace neverd

#endif // NEVERD_IR_MED_X86REGISTRATIONSEHTHREAD_H
