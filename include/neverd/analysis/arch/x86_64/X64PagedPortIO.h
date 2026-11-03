//===- X64PagedPortIO.h - Explicit port device transactions ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind scalar and string port transfers to an isolated synchronous device.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_ARCH_X86_64_X64PAGEDPORTIO_H
#define NEVERD_ANALYSIS_ARCH_X86_64_X64PAGEDPORTIO_H

#include "neverd/analysis/arch/x86_64/X64PagedEventDelivery.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>

namespace neverd::analysis {

/// These profiles check I/O permissions before the zero-count REP check and
/// transfer one element per step. INS either retains one read on a destination
/// fault, or checks the entire destination before reading. None describes
/// device read-ahead, physical bus timing, or KVM's batch prefetch.
enum class X64PagedPortIOProfile : uint8_t {
  InputBeforeWrite,
  InputAfterWriteChecks,
  /// InputBeforeWrite with earlier 4 KiB write fragments retained on a later
  /// page fault. Matches the observed split input store of KVM's emulator.
  InputBeforeWriteFragments,
};

struct X64PortIORequest {
  uint16_t Port;
  uint8_t Bytes;
  bool Input;
  uint32_t Value; // Output value, already truncated; zero for input.
};

struct X64PagedPortIOBinding {
  X64PagedPortIOProfile Profile;
  /// A value means exactly one device transfer completed. Empty means no
  /// effect and a retryable device stop. Error also guarantees no effect.
  /// Output success returns any value (ignored). The callback must not throw,
  /// reenter execution, access guest state/RAM, or change the paging binding.
  /// The confined runtime may preflight RAM before invoking it: operand backing
  /// stops therefore never consume input, even with InputBeforeWrite. A guest
  /// fault under that profile DOES consume input and publishes
  /// DeviceTransferCompleted.
  llvm::Expected<std::optional<uint32_t>> (*Transfer)(void *,
                                                      const X64PortIORequest &);
  void *Context = nullptr;
};

struct X64PagedPortIOResult {
  translate::X86MemoryAccessResult Access;
  bool Completed = false;
  bool RepeatContinue = false;
  bool DeviceTransferCompleted = false;
  bool DeviceUnavailable = false;
  uint8_t InputBytesWritten = 0;
};

/// Caller authenticates instruction fetch and admission. Success publishes one
/// element's registers and ordinary RAM together; a guest fault may retain an
/// input device read and, with the fragment profile, an earlier RAM fragment.
/// Index/count publication still requires completion of the entire element.
/// Fault delivery and budget accounting belong to X64PagedExecution. No hidden
/// progress state is retained.
llvm::Expected<X64PagedPortIOResult> executeX64PagedPortIO(
    X64ContinuationStateV6 &State, translate::X86PagedMemory &Memory,
    llvm::ArrayRef<uint8_t> Bytes, X64PagedAlignmentPriority AlignmentPriority,
    const X64PagedPortIOBinding &Binding);

} // namespace neverd::analysis

#endif // NEVERD_ANALYSIS_ARCH_X86_64_X64PAGEDPORTIO_H
