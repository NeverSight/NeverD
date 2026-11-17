//===- DWARFCallingConvention.h - DWARF function conventions ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared declaration conventions for DWARF debug types and local types.

#ifndef NEVERD_TYPES_DWARFCALLINGCONVENTION_H
#define NEVERD_TYPES_DWARFCALLINGCONVENTION_H

#include "neverd/types/CallingConvention.h"

#include "llvm/BinaryFormat/Dwarf.h"

#include <optional>

namespace neverd::types {

inline std::optional<CallingConvention> dwarfCallingConvention(uint64_t Code) {
  switch (Code) {
  case llvm::dwarf::DW_CC_normal:
    return CallingConvention::Unspecified;
  case llvm::dwarf::DW_CC_LLVM_vectorcall:
    return CallingConvention::Vectorcall;
  case llvm::dwarf::DW_CC_LLVM_Win64:
    return CallingConvention::Win64;
  case llvm::dwarf::DW_CC_LLVM_X86_64SysV:
    return CallingConvention::SysV;
  default:
    return std::nullopt;
  }
}

} // namespace neverd::types

#endif // NEVERD_TYPES_DWARFCALLINGCONVENTION_H
