//===- X86RegistrationSEHLocalUnwind.h - Local unwind binding -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind checked local-unwind effects to their current source SSA occurrences.

#ifndef NEVERD_IR_MED_X86_X86REGISTRATIONSEHLOCALUNWIND_H
#define NEVERD_IR_MED_X86_X86REGISTRATIONSEHLOCALUNWIND_H

#include "neverd/ir/RegistrationCall.h"

#include <map>
#include <optional>

namespace neverd {
struct MedFunc;
struct MedOp;

struct X86RegistrationSEHLocalUnwind {
  const RegistrationSEHLocalUnwind *Source = nullptr;
  const MedOp *Operation = nullptr;
  int Block = -1;
};

/// This retains source levels. Native partial unwinds still require a mapping
/// to compiler-owned states; runtime identity does not grant that mapping.
std::optional<std::map<va_t, X86RegistrationSEHLocalUnwind>>
getX86RegistrationSEHLocalUnwinds(const MedFunc &Function);

} // namespace neverd

#endif // NEVERD_IR_MED_X86_X86REGISTRATIONSEHLOCALUNWIND_H
