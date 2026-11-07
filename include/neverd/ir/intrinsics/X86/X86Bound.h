//===- X86Bound.h - Native signed bounds contracts -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Native signed bounds contracts.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_X86_X86BOUND_H
#define NEVERD_IR_INTRINSICS_X86_X86BOUND_H

#include "neverd/ir/intrinsics/Intrinsics.h"

#include <array>
#include <cstdint>
#include <optional>

namespace neverd {

inline constexpr const char X86BoundMalformed[] = "invalid x86 BOUND contract";

constexpr bool x86BoundAddressIsValid(uint64_t Bits, uint64_t Prefix) {
  return (Bits == 16 || Bits == 32) &&
         (Prefix == 0 || Prefix == 0x26 || Prefix == 0x2e || Prefix == 0x36 ||
          Prefix == 0x3e || Prefix == 0x64 || Prefix == 0x65);
}

/// Inputs are index, normalized segment offset, address bits and segment byte.
/// Keep the pair inside BOUND: two ordinary loads change observable faults.
inline bool x86BoundShapeIsValid(Arch Target, unsigned Arguments,
                                 unsigned OutputBytes,
                                 const std::array<unsigned, 4> &Sizes,
                                 std::optional<uint64_t> Bits,
                                 std::optional<uint64_t> Prefix,
                                 NdMemoryAddressSpace Space,
                                 NdMemoryOrdering Ordering) {
  return (Target == Arch::Unknown || Target == Arch::X86) && Arguments == 4 &&
         !OutputBytes && (Sizes[0] == 2 || Sizes[0] == 4) && Sizes[1] == 8 &&
         Sizes[2] == 4 && Sizes[3] == 4 && Bits && Prefix &&
         x86BoundAddressIsValid(*Bits, *Prefix) &&
         Space == NdMemoryAddressSpace::Default &&
         Ordering == NdMemoryOrdering::None;
}

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_X86_X86BOUND_H
