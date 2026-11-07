//===- X86PortIO.h - Confined port device transactions ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared device effects and permission bitmap reads for x86 execution modes.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TRANSLATE_X86_X86PORTIO_H
#define NEVERD_TRANSLATE_X86_X86PORTIO_H

#include "neverd/translate/X86/X86PagedMemory.h"

#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>

namespace neverd::translate {

/// One element per step. Permission checks precede zero-count completion.
/// Input may precede destination faults, or follow all destination checks.
/// No physical device read-ahead, bus timing or KVM batch prefetch is inferred.
enum class X86PortIOProfile : uint8_t {
  InputBeforeWrite,
  InputAfterWriteChecks,
  /// Retain earlier 4 KiB input-store fragments on a later page fault.
  InputBeforeWriteFragments,
};

struct X86PortIORequest {
  uint16_t Port;
  uint8_t Bytes;
  bool Input;
  uint32_t Value; // Truncated output value; zero for input.
};

struct X86PortIOBinding {
  X86PortIOProfile Profile;
  /// A value means exactly one completed transfer. Empty/error guarantees no
  /// device effect. Output success ignores the returned value. The callback
  /// must not throw, reenter, access guest state/RAM or change paging controls.
  /// Backing stops never consume input; a bound guest fault may consume it.
  llvm::Expected<std::optional<uint32_t>> (*Transfer)(void *,
                                                      const X86PortIORequest &);
  void *Context = nullptr;
};

struct X86PortIOResult {
  X86MemoryAccessResult Access;
  bool Completed = false;
  bool RepeatContinue = false;
  bool DeviceTransferCompleted = false;
  bool DeviceUnavailable = false;
  uint8_t InputBytesWritten = 0;
  uint32_t InputValue = 0;
  /// Actual completed device extent, valid when DeviceTransferCompleted.
  uint16_t Port = 0;
  uint8_t TransferBytes = 0;
};

/// Mode owner supplies the checked segment/canonical extent and the explicit
/// alignment order. Fault precedes paging; late alignment follows it.
struct X86PortMemoryOperand {
  X86LinearAccess Access;
  std::optional<X86MemoryException> Fault;
  bool AlignmentFaultAfterPaging = false;
};

llvm::Error validateX86PortIOBinding(const X86PortIOBinding &Binding);

/// Caller authenticates a bitmap-bearing TSS and CPL > IOPL. Read the base and
/// two-byte permission window as implicit supervisor, without port wrapping.
llvm::Expected<X86MemoryAccessResult>
checkX86PortBitmap(X86PagedMemory &Memory, uint64_t Base, uint32_t Limit,
                   uint16_t Port, uint8_t Bytes, bool Wrap32);

/// Caller validates binding, width (1/2/4), fetch, permissions and zero count.
/// Registers, indices and EIP remain the mode owner's staged publication.
llvm::Expected<X86PortIOResult>
transferX86PortIO(X86PagedMemory &Memory, const X86PortIOBinding &Binding,
                  X86PortIORequest Request,
                  const std::optional<X86PortMemoryOperand> &Operand);

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_X86_X86PORTIO_H
