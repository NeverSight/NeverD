//===- KernelException.h - Typed guest exception delivery -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A modeled API raises this guest exception at a normal CPU stop. It is
/// distinct from model errors and irreversible backend execution faults.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_KERNELEXCEPTION_H
#define NEVERD_EMULATION_KERNELEXCEPTION_H

#include "llvm/Support/Error.h"

#include <cstdint>

namespace neverd::emulation {
namespace exceptions {
#define NEVERD_KERNEL_EXCEPTION_VALUE(Name, Value)                             \
  inline constexpr uint32_t Name = Value;
#include "KernelExceptionValues.def"
#undef NEVERD_KERNEL_EXCEPTION_VALUE
} // namespace exceptions

class KernelGuestException final
    : public llvm::ErrorInfo<KernelGuestException> {
public:
  static char ID;
  explicit KernelGuestException(uint32_t Code) : Code(Code) {}
  uint32_t code() const { return Code; }
  void log(llvm::raw_ostream &OS) const override;
  std::error_code convertToErrorCode() const override;

private:
  uint32_t Code;
};
} // namespace neverd::emulation

#endif // NEVERD_EMULATION_KERNELEXCEPTION_H
