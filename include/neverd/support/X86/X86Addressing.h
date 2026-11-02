//===- X86Addressing.h - Shared x86 address semantics ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared decoded address roles, independent of operand access and execution.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_SUPPORT_X86_X86ADDRESSING_H
#define NEVERD_SUPPORT_X86_X86ADDRESSING_H

#include <capstone/x86.h>
#include <cstdint>

namespace neverd {

/// The original base selects SS before any explicit segment override. R12/R13
/// remain data-default despite sharing the low encoding bits with SP/BP. This
/// is not the final fault class: mode and segment overrides also matter.
constexpr bool isStackDefaultBase(x86_reg Register) {
  return Register == X86_REG_RSP || Register == X86_REG_RBP ||
         Register == X86_REG_ESP || Register == X86_REG_EBP ||
         Register == X86_REG_BP;
}

/// A missing ordinary SIB index, including width-specific decoder aliases.
/// These aliases are not base registers or VSIB vector indices. AddressSize
/// is in bytes; real R12/R12D indices selected by REX.X remain registers.
constexpr bool isNoSibIndex(x86_reg Register, uint16_t AddressSize) {
  return Register == X86_REG_INVALID ||
         (AddressSize == 4 && Register == X86_REG_EIZ) ||
         (AddressSize == 8 && Register == X86_REG_RIZ);
}

} // namespace neverd

#endif // NEVERD_SUPPORT_X86_X86ADDRESSING_H
