//===- X86ImportReference.h - COFF import reference fields ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recognizes canonical x86/x64 memory-indirect call/jump and address fields
/// shared by pattern generation and reference checking. Prefixed forms are not
/// interpreted as unprefixed instructions.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SIGS_X86_X86IMPORTREFERENCE_H
#define NEVERD_SIGS_X86_X86IMPORTREFERENCE_H

#include "neverd/support/ISAEncoding.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>

namespace neverd::sigs {

inline constexpr llvm::StringLiteral COFFImportPointerPrefix = "__imp_";

inline bool isX86ReferencePrefix(uint8_t Byte, bool Is64Bit) {
  if (Is64Bit && (Byte & 0xf0) == 0x40)
    return true;
  switch (Byte) {
  case 0xf0:
  case 0xf2:
  case 0xf3:
  case 0x2e:
  case 0x36:
  case 0x3e:
  case 0x26:
  case 0x64:
  case 0x65:
  case 0x66:
  case 0x67:
    return true;
  default:
    return false;
  }
}

/// FF /2 and FF /4 with mod=00, r/m=101: an absolute 32-bit slot in i386,
/// or a RIP-relative slot in x64, also with a single canonical REX.W prefix.
/// Offset names the four-byte address field, including in Win64 tail jumps.
inline bool isX86ImportReferenceField(llvm::ArrayRef<uint8_t> Code,
                                      size_t Offset, bool Is64Bit) {
  if (Offset < x86::kJmpIndirectDispOffset || Offset > Code.size() ||
      Code.size() - Offset < sizeof(uint32_t))
    return false;
  const size_t Opcode = Offset - x86::kJmpIndirectDispOffset;
  constexpr uint8_t CallMemoryModRM = 0x15;
  if (Code[Opcode] != x86::kJmpIndirectOp ||
      (Code[Opcode + 1] != CallMemoryModRM &&
       Code[Opcode + 1] != x86::kJmpIndirectModRM))
    return false;
  if (Opcode == 0)
    return true;
  if (Is64Bit && Code[Opcode - 1] == 0x48)
    return Opcode == 1 || !isX86ReferencePrefix(Code[Opcode - 2], true);
  // With no decoded boundary before FF, even a prefix-looking final byte
  // of the preceding instruction is conservatively left unsupported. In
  // particular, address/segment overrides must not acquire RIP semantics.
  return !isX86ReferencePrefix(Code[Opcode - 1], Is64Bit);
}

/// REX.W LEA r64,[RIP+disp32]. Only the canonical whole-pointer address
/// materialization is supported; address/segment overrides are not inferred.
inline bool isX64AddressReferenceField(llvm::ArrayRef<uint8_t> Code,
                                       size_t Offset) {
  constexpr size_t DisplacementOffset = 3;
  if (Offset < DisplacementOffset || Offset > Code.size() ||
      Code.size() - Offset < sizeof(uint32_t))
    return false;
  const size_t Start = Offset - DisplacementOffset;
  return (Code[Start] == 0x48 || Code[Start] == 0x4c) &&
         Code[Start + 1] == 0x8d && (Code[Start + 2] & 0xc7) == 0x05 &&
         (Start == 0 || !isX86ReferencePrefix(Code[Start - 1], true));
}

} // namespace neverd::sigs

#endif // NEVERD_SIGS_X86_X86IMPORTREFERENCE_H
