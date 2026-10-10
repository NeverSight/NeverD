//===- X86Addressing.h - Shared x86 decoded address semantics ----*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_SUPPORT_X86ADDRESSING_H
#define NEVERD_SUPPORT_X86ADDRESSING_H

#include <capstone/x86.h>
#include <cstdint>

namespace neverd {

/// A missing ordinary SIB index, including width-specific decoder aliases.
/// These aliases are not base registers or VSIB vector indices. AddressSize
/// is in bytes; real R12/R12D indices selected by REX.X remain registers.
constexpr bool isNoSibIndex(x86_reg Register, uint16_t AddressSize) {
  return Register == X86_REG_INVALID ||
         (AddressSize == 4 && Register == X86_REG_EIZ) ||
         (AddressSize == 8 && Register == X86_REG_RIZ);
}

} // namespace neverd

#endif // NEVERD_SUPPORT_X86ADDRESSING_H
