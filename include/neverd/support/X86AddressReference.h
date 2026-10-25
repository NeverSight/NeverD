//===- X86AddressReference.h - Canonical x64 code addresses ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared encoding for whole-pointer RIP-relative address materialization.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_X86ADDRESSREFERENCE_H
#define NEVERD_SUPPORT_X86ADDRESSREFERENCE_H

#include "llvm/ADT/ArrayRef.h"

#include <cstddef>
#include <cstdint>

namespace neverd::x86 {

inline constexpr size_t kAddressDisplacementOffset = 3;
inline constexpr size_t kAddressInstructionSize =
    kAddressDisplacementOffset + sizeof(uint32_t);

/// Exactly REX.W LEA r64,[RIP+disp32], including R8-R15 destinations. The
/// caller supplies one complete instruction; extra prefixes, address-size
/// overrides and narrower destination registers are outside this contract.
inline bool isRipRelativeAddressInstruction(llvm::ArrayRef<uint8_t> Bytes) {
  return Bytes.size() == kAddressInstructionSize &&
         (Bytes[0] == 0x48 || Bytes[0] == 0x4c) && Bytes[1] == 0x8d &&
         (Bytes[2] & 0xc7) == 0x05;
}

} // namespace neverd::x86

#endif // NEVERD_SUPPORT_X86ADDRESSREFERENCE_H
