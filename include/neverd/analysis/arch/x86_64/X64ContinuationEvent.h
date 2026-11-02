//===- X64ContinuationEvent.h - Explicit event input -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// An already selected architectural event, separate from instruction decoding.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_ARCH_X86_64_X64CONTINUATIONEVENT_H
#define NEVERD_ANALYSIS_ARCH_X86_64_X64CONTINUATIONEVENT_H

#include <cstdint>

namespace neverd::analysis {

enum class X64ContinuationEventKind : uint8_t {
  Fault = 0,
  Trap = 1,
  Interrupt = 2,
  NMI = 3,
  Software = 4,
  InstructionBreakpoint = 5,
  Abort = 6,
};

/// Input to neverd_x64_deliver_event_v6. The event producer owns selection,
/// priority, instruction restart/partial completion and cause-specific banks
/// (DR6, machine-check MSRs, etc.). SavedPC is the architectural CS-relative
/// return offset, including for a non-flat compatibility source. A fault uses
/// the faulting instruction, a trap uses its completed continuation. The input
/// is immutable and disjoint from state and all accessed descriptor/stack RAM.
///
/// Interrupt eligibility is checked against IF and the supplied STI/MOV-SS
/// shadows; NMI additionally checks State.NMIBlocked. NMIBlockedBySTI binds
/// the original processor's STI behavior. RepeatedString marks an interrupted
/// nonfinal REP iteration (its saved RF is one). Shadow lifetime and APIC/PIC
/// priority belong to the producer. Successful delivery returns Exception;
/// a blocked event returns EventDeferred without effects. Invalid metadata,
/// state or delivery preconditions return InvalidState without effects.
///
/// Only successful ordinary descriptor/TSS/stack RAM accesses are admitted.
/// This interface does not turn a host fault into a guest fault, deliver a
/// second fault during delivery, enter SMM/SGX or enable CET/FRED/VMX routing.
/// Entry is outside RTM/HLE and enclave execution; transaction abort and AEX
/// processing must finish before supplying an event to this IDT delivery path.
struct X64ContinuationEventV1 {
  uint64_t SavedPC = 0;
  uint64_t ErrorCode = 0;
  uint64_t FaultAddress = 0;
  uint8_t Vector = 0;
  X64ContinuationEventKind Kind = X64ContinuationEventKind::Fault;
  uint8_t STIBlocked = 0;
  uint8_t SSBlocked = 0;
  uint8_t NMIBlockedBySTI = 0;
  uint8_t RepeatedString = 0;
  uint8_t Reserved[2] = {};
};

} // namespace neverd::analysis

#endif // NEVERD_ANALYSIS_ARCH_X86_64_X64CONTINUATIONEVENT_H
