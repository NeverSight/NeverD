//===- Win64CallStackInput.h - Original outgoing scalar slots ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Identify an unresolved fixed argument by its original direct CALL and
/// pre-CALL RSP displacement. The coordinate supplies no bytes on its own.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_X86_WIN64CALLSTACKINPUT_H
#define NEVERD_IR_MED_X86_WIN64CALLSTACKINPUT_H

#include "neverd/ir/med/MedIR.h"

#include <cstdint>
#include <optional>

namespace neverd {
struct BinaryImage;

struct Win64CallStackInput {
  va_t Site;
  uint32_t Offset;

  bool operator==(const Win64CallStackInput &) const = default;

  static bool validOffset(uint64_t Offset);

  static std::optional<Win64CallStackInput> from(const MedVar &V) {
    if (V.Kind != MedVar::Unspecified || V.SSAVer <= 0 ||
        V.AddressOwnerVA == InvalidVA || V.TheArch != Arch::X64 ||
        V.Size != sizeof(uint64_t) || !validOffset(V.StackOff))
      return std::nullopt;
    return Win64CallStackInput{V.AddressOwnerVA, uint32_t(V.StackOff)};
  }

  /// Each proven occurrence uses a fresh version of the function's
  /// UnspecifiedId. Unbound values retain version zero. The original PC is
  /// carried in AddressOwnerVA, with StackOff relative to that CALL's RSP.
  static void bind(MedVar &V, const BinaryImage *Image, va_t Site, va_t Callee,
                   int64_t Offset, int &LastVersion);
};
} // namespace neverd

#endif // NEVERD_IR_MED_X86_WIN64CALLSTACKINPUT_H
