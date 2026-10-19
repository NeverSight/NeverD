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

#include "neverd/emulation/DriverBugCheck.h"

#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>

namespace neverd::emulation {
namespace exceptions {
#define NEVERD_KERNEL_EXCEPTION_VALUE(Name, Value)                             \
  inline constexpr uint32_t Name = Value;
#include "KernelExceptionValues.def"
#undef NEVERD_KERNEL_EXCEPTION_VALUE
#define NEVERD_KERNEL_EXCEPTION_DIAGNOSTIC(Name, Text)                         \
  inline constexpr const char *Name = Text;
#include "KernelExceptionValues.def"
#undef NEVERD_KERNEL_EXCEPTION_DIAGNOSTIC
std::optional<uint32_t> kernelX64ExceptionStatus(uint32_t Vector);
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
/// A terminal request is not a guest SEH exception and cannot return through
/// its API caller. The session records it before any further guest execution.
class KernelGuestBugCheck final : public llvm::ErrorInfo<KernelGuestBugCheck> {
public:
  static char ID;
  explicit KernelGuestBugCheck(DriverBugCheck Request) : Request(Request) {}
  const DriverBugCheck &request() const { return Request; }
  void log(llvm::raw_ostream &OS) const override;
  std::error_code convertToErrorCode() const override;

private:
  DriverBugCheck Request;
};
} // namespace neverd::emulation

#endif // NEVERD_EMULATION_KERNELEXCEPTION_H
