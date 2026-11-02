//===- X64PagedEventDelivery.h - Paged IDT entry ----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Run the shared V6 event semantics over owned paged RAM, including faults
/// during delivery, double faults and architectural shutdown.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_ARCH_X86_64_X64PAGEDEVENTDELIVERY_H
#define NEVERD_ANALYSIS_ARCH_X86_64_X64PAGEDEVENTDELIVERY_H

#include "neverd/analysis/arch/x86_64/X64ContinuationEvent.h"
#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"
#include "neverd/translate/X86/X86PagedMemory.h"

#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>

namespace neverd::analysis {

enum class X64PagedEventStatus : uint8_t {
  Delivered,
  Deferred,
  InvalidState,
  BackingFault,
  Shutdown,
};

struct X64PagedEventResult {
  X64PagedEventStatus Status = X64PagedEventStatus::InvalidState;
  /// The original event passed admission and inhibition before its first IDT
  /// access. A secondary fault does not put that external event back in a
  /// queue.
  bool InputAccepted = false;
  uint8_t DeliveredVector = 0;
  unsigned DeliveryAttempts = 0;
  std::optional<translate::X86MemoryException> LastException;
  std::optional<translate::GuestMemoryFault> BackingFault;
};

/// Legacy IA-32e IDT entry with explicit RAM and control-register bindings.
/// Uses the same descriptor/frame semantics as continuation source emission.
/// System table accesses use implicit supervisor privilege; frame accesses use
/// the handler privilege. CR2 is published whenever #PF is detected, including
/// a fault while delivering #DF. Already completed descriptor/page-table/frame
/// RAM writes remain visible. Shutdown is reported, never implemented as reset.
/// For Software events, State.PC identifies the original INT/INT3 while
/// Event.SavedPC supplies its successful fallthrough. Delivery faults restart
/// State.PC, not that fallthrough; the numeric vector does not imply an error
/// code or a fault exception class.
///
/// This entry stops before executing the handler. It does not provide legacy
/// i386 task gates, FRED/CET entry, VMX event routing, SMM or SGX AEX. A #DF
/// frame selects the original saved PC from the architecturally undefined
/// state; its presence is not a proof that the interrupted program can be
/// resumed.
llvm::Expected<X64PagedEventResult>
deliverX64PagedEvent(X64ContinuationStateV6 &State,
                     translate::X86PagedMemory &Memory,
                     const X64ContinuationEventV1 &Event, uint32_t MXCSRMask);

} // namespace neverd::analysis

#endif // NEVERD_ANALYSIS_ARCH_X86_64_X64PAGEDEVENTDELIVERY_H
