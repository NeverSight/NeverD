//===- X86TscDeadlineTimer.h - Local APIC TSC deadline ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Per-CPU deadline MSR and timer expiry on an explicitly sampled guest TSC.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TRANSLATE_X86_X86TSCDEADLINETIMER_H
#define NEVERD_TRANSLATE_X86_X86TSCDEADLINETIMER_H

#include "neverd/translate/X86/X86LocalApic.h"

namespace neverd::translate {

/// Bind on a CPU that enumerates CPUID.01H:ECX[24]. The APIC controller
/// supplies changes to its LVT timer. Count-based timers have a separate clock.
class X86TscDeadlineTimer {
  uint64_t Deadline = 0;
  uint8_t Vector = 0;
  bool DeadlineMode = false, Masked = true;

public:
  static constexpr uint32_t MSR = 0x6e0;
  constexpr bool operator==(const X86TscDeadlineTimer &) const = default;

  /// Synchronize an admitted timer-LVT write. Crossing the deadline-mode
  /// boundary disarms the deadline. Updating only the vector or mask preserves
  /// it.
  [[nodiscard]] constexpr bool setLocalVector(bool SelectDeadline,
                                              uint8_t NewVector, bool Mask) {
    if (SelectDeadline && !Mask && NewVector < 16)
      return false; // Reserved-vector error delivery needs an APIC ESR binding.
    if (SelectDeadline != DeadlineMode)
      Deadline = 0;
    DeadlineMode = SelectDeadline;
    Vector = NewVector;
    Masked = Mask;
    return true;
  }
  constexpr uint64_t read() const { return Deadline; }
  constexpr void write(uint64_t Value) {
    if (DeadlineMode)
      Deadline = Value;
  }

  /// Called on serialized boundaries. A masked expiration consumes the
  /// deadline. IF, TPR and ISR eligibility are evaluated after edge arrival.
  constexpr void poll(uint64_t GuestTSC, X86LocalApicFixed &APIC) {
    if (!Deadline || GuestTSC < Deadline)
      return;
    Deadline = 0;
    if (!Masked)
      (void)APIC.receiveFixedInterrupt(Vector, false, true);
  }
};

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_X86_X86TSCDEADLINETIMER_H
