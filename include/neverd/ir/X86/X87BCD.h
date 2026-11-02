//===- X87BCD.h - Packed decimal operand admission -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// FBLD's eighteen digits must be decimal. Hardware does not validate A..F;
/// their numerical result is undefined. Bits 0..6 of the sign byte are ignored.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_X86_X87BCD_H
#define NEVERD_IR_X86_X87BCD_H

#include "neverd/ir/intrinsics/Intrinsics.h"

#include <cstdint>
#include <span>

namespace neverd {

constexpr bool x87PackedBCDIsValid(std::span<const uint8_t> Bytes) {
  if (Bytes.size() != 10)
    return false;
  for (unsigned I = 0; I != 9; ++I)
    if ((Bytes[I] & 0xf) > 9 || (Bytes[I] >> 4) > 9)
      return false;
  return true;
}

constexpr bool isX87BCDIntrinsic(Intrinsic Id) {
  return Id == Intrinsic::X87LoadPackedBCD ||
         Id == Intrinsic::X87StorePackedBCD;
}

} // namespace neverd

#endif // NEVERD_IR_X86_X87BCD_H
