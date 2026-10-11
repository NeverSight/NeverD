//===- X86RegistrationEntry.h - PE32 physical parameters --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Project checked entry registers and caller stack words onto a signature.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_IR_MED_X86REGISTRATIONENTRY_H
#define NEVERD_IR_MED_X86REGISTRATIONENTRY_H

#include <cstdint>
#include <optional>

namespace neverd {
struct MedFunc;
struct X86RegistrationEntryABI {
  unsigned RegisterCount = 0;
  unsigned StackWords = 0;
  uint16_t PopBytes = 0;
};

std::optional<X86RegistrationEntryABI>
projectX86RegistrationEntry(const MedFunc &Function);

/// Preserve stack words observed only from runtime callbacks, and unused
/// words authenticated by RET cleanup. Existing operand identities stay fixed.
void completeX86RegistrationEntryParameters(MedFunc &Function);
} // namespace neverd

#endif
