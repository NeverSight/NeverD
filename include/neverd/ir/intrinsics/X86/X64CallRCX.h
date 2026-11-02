//===- X64CallRCX.h - Native Win64 call result transport -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// One ordinary Win64 scalar call owns RAX and the actual RCX at its return.
/// The caller supplies all four register slots; no stack arguments or hidden
/// convention are admitted. A non-returning or exceptional transfer supplies
/// neither result. This is an explicit native call, not a callee body proof.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_X86_X64CALLRCX_H
#define NEVERD_IR_INTRINSICS_X86_X64CALLRCX_H

#include "neverd/ir/intrinsics/Intrinsics.h"

#include <array>

namespace neverd {
inline constexpr unsigned X64CallRCXArguments = 5;
inline constexpr unsigned X64CallRCXNativeResults = 5;
inline constexpr const char *X64CallRCXDiagnostic =
    "native Win64 RCX capture requires a complete scalar call contract";

struct X64CallRCXShape {
  Arch Architecture = Arch::Unknown;
  bool ValidId = false;
  bool WritableOutput = false;
  bool WritableRCX = false;
  unsigned Arguments = 0;
  uint16_t OutputBytes = 0;
  uint16_t RCXBytes = 0;
  NdMemoryOrdering Ordering = NdMemoryOrdering::None;
  NdMemoryAddressSpace AddressSpace = NdMemoryAddressSpace::Default;
  std::array<uint16_t, X64CallRCXArguments> InputBytes{};
  std::array<bool, X64CallRCXArguments> ScalarInputs{};
};

constexpr bool x64CallRCXShapeIsValid(const X64CallRCXShape &S) {
  if ((S.Architecture != Arch::Unknown && S.Architecture != Arch::X64) ||
      !S.ValidId || !S.WritableOutput || !S.WritableRCX ||
      S.Arguments != X64CallRCXArguments || S.OutputBytes != 8 ||
      S.RCXBytes != 8 || S.Ordering != NdMemoryOrdering::None ||
      S.AddressSpace != NdMemoryAddressSpace::Default)
    return false;
  for (unsigned I = 0; I < X64CallRCXArguments; ++I)
    if (S.InputBytes[I] != 8 || !S.ScalarInputs[I])
      return false;
  return true;
}
} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_X86_X64CALLRCX_H
