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
char KernelGuestBugCheck::ID = 0;

std::optional<uint32_t> exceptions::kernelX64ExceptionStatus(uint32_t Vector) {
  switch (Vector) {
#define NEVERD_KERNEL_X64_EXCEPTION(Vector, Status)                            \
  case Vector:                                                                 \
    return exceptions::Status;
#include "KernelExceptionValues.def"
#undef NEVERD_KERNEL_X64_EXCEPTION
  default:
    return std::nullopt;
  }
}

void KernelGuestException::log(llvm::raw_ostream &OS) const {
  OS << "raised guest exception 0x" << llvm::utohexstr(Code);
}

std::error_code KernelGuestException::convertToErrorCode() const {
  return llvm::inconvertibleErrorCode();
}
void KernelGuestBugCheck::log(llvm::raw_ostream &OS) const {
  OS << "guest requested bug check 0x" << llvm::utohexstr(Request.Code);
}
std::error_code KernelGuestBugCheck::convertToErrorCode() const {
  return llvm::inconvertibleErrorCode();
}
} // namespace neverd::emulation
