//===- CVaList.h - C variadic list machine transport ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Convert between a target's C va_list and the machine word a call passes.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_CVALIST_H
#define NEVERD_BACKEND_C_CVALIST_H

#include "neverd/Common.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ErrorHandling.h"

#include <stdexcept>
#include <string>

namespace neverd::c_detail {

enum class VaListABI { Pointer, IndirectRecord, PointerRecord };

/// AAPCS64 passes its large record by reference; AAPCS32 passes its one
/// pointer member in a register. Windows and Darwin use their own pointer
/// forms. An x86-64 System V array decays to a pointer at a call.
inline VaListABI vaListABI(Arch TheArch, BinaryFormat Format) {
  const bool OwnABI =
      Format == BinaryFormat::MachO || Format == BinaryFormat::COFF;
  switch (TheArch) {
  case Arch::AArch64:
    return OwnABI ? VaListABI::Pointer : VaListABI::IndirectRecord;
  case Arch::ARM:
    return OwnABI ? VaListABI::Pointer : VaListABI::PointerRecord;
  case Arch::X86:
  case Arch::X64:
  case Arch::MIPS32:
  case Arch::MIPS32EL:
  case Arch::MIPS64:
  case Arch::MIPS64EL:
    return VaListABI::Pointer;
  default:
    throw std::runtime_error("The target's va_list call ABI is unsupported");
  }
}

/// The va_list a header-declared routine takes from a raw machine word.
/// Use the compiler's target type: the word is not always an object pointer.
inline std::string vaListArgument(Arch TheArch, BinaryFormat Format,
                                  llvm::StringRef Word) {
  const std::string Address = "(void *)(uintptr_t)(" + Word.str() + ")";
  switch (vaListABI(TheArch, Format)) {
  case VaListABI::Pointer:
    return Address;
  case VaListABI::IndirectRecord:
    return "*(__builtin_va_list *)" + Address;
  case VaListABI::PointerRecord:
    return "(__builtin_va_list){" + Address + "}";
  }
  llvm_unreachable("All va_list ABIs handled");
}

/// The machine word a C va_list supplies to a runtime-table declaration
/// whose parameter is the ABI carrier, rather than the header's va_list.
inline std::string vaListWord(Arch TheArch, BinaryFormat Format,
                              llvm::StringRef VaList) {
  switch (vaListABI(TheArch, Format)) {
  case VaListABI::Pointer:
    return VaList.str();
  case VaListABI::IndirectRecord:
    return "(void *)&" + VaList.str();
  case VaListABI::PointerRecord:
    return "*(void **)&" + VaList.str();
  }
  llvm_unreachable("All va_list ABIs handled");
}

} // namespace neverd::c_detail

#endif // NEVERD_BACKEND_C_CVALIST_H
