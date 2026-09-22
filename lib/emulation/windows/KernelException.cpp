//===- KernelException.cpp - Typed guest exception delivery ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Guest exceptions retain their Windows status without impersonating an API
/// return value or a host exception.
///
//===----------------------------------------------------------------------===//

#include "KernelException.h"

#include "llvm/ADT/StringExtras.h"

namespace neverd::emulation {
char KernelGuestException::ID = 0;

void KernelGuestException::log(llvm::raw_ostream &OS) const {
  OS << "raised guest exception 0x" << llvm::utohexstr(Code);
}

std::error_code KernelGuestException::convertToErrorCode() const {
  return llvm::inconvertibleErrorCode();
}
} // namespace neverd::emulation
