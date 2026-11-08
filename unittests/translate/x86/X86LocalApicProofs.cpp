//===- X86LocalApicProofs.cpp - Compile-only APIC state proofs -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independently specified bank transitions; no guest or executable runs.
//===----------------------------------------------------------------------===//

#include "neverd/translate/X86/X86LocalApic.h"

using neverd::translate::X86LocalApicFixed;
using neverd::translate::X86LocalApicFixedState;

namespace {

constexpr void set(std::array<uint32_t, 8> &Bank, unsigned Vector) {
  Bank[Vector / 32] |= uint32_t(1) << (Vector % 32);
}

constexpr bool everyVector() {
  for (unsigned V = 16; V < 256; ++V) {
    X86LocalApicFixedState Initial;
    set(Initial.IRR, V);
    if (V & 1)
      set(Initial.TMR, V);
    Initial.EOIBroadcastSuppressed = V & 2;
    auto APIC = X86LocalApicFixed::fromState(Initial);
    if (!APIC || APIC->nextFixedInterrupt() != V || APIC->state() != Initial)
      return false;
    if (APIC->acceptFixedInterrupt(V ^ 1) || APIC->state() != Initial)
      return false;
    if (!APIC->acceptFixedInterrupt(V))
      return false;
    auto Accepted = Initial;
    Accepted.IRR = {};
    set(Accepted.ISR, V);
    if (APIC->state() != Accepted || APIC->nextFixedInterrupt() ||
        APIC->acceptFixedInterrupt(V) || APIC->state() != Accepted)
      return false;
    const auto EOI = APIC->endOfInterrupt();
    Accepted.ISR = {};
    if (!EOI || EOI->Vector != V || EOI->LevelTriggered != bool(V & 1) ||
        EOI->Broadcast != bool((V & 3) == 1) || APIC->state() != Accepted ||
        APIC->endOfInterrupt() || APIC->state() != Accepted)
      return false;
  }
  return true;
}
static_assert(everyVector());

// Partition the full priority-class grid into separate constant expressions,
// keeping the default compiler evaluation limit instead of raising a budget.
template <unsigned TaskClass> constexpr bool priorityClasses() {
  for (unsigned ServiceClass = 0; ServiceClass < 16; ++ServiceClass)
    for (unsigned PendingClass = 1; PendingClass < 16; ++PendingClass) {
      X86LocalApicFixedState Initial;
      Initial.TPR = (TaskClass << 4) | 5;
      if (ServiceClass)
        set(Initial.ISR, (ServiceClass << 4) | 7);
      set(Initial.IRR, (PendingClass << 4) | 15);
      auto APIC = X86LocalApicFixed::fromState(Initial);
      const unsigned PPR =
          TaskClass >= ServiceClass ? Initial.TPR : ServiceClass << 4;
      const bool Eligible =
          PendingClass > TaskClass && PendingClass > ServiceClass;
      if (!APIC || APIC->processorPriority() != PPR ||
          APIC->nextFixedInterrupt().has_value() != Eligible ||
          APIC->state() != Initial)
        return false;
      if (Eligible && APIC->nextFixedInterrupt() != ((PendingClass << 4) | 15))
        return false;
    }
  return true;
}
static_assert(priorityClasses<0>());
static_assert(priorityClasses<1>());
static_assert(priorityClasses<2>());
static_assert(priorityClasses<3>());
static_assert(priorityClasses<4>());
static_assert(priorityClasses<5>());
static_assert(priorityClasses<6>());
static_assert(priorityClasses<7>());
static_assert(priorityClasses<8>());
static_assert(priorityClasses<9>());
static_assert(priorityClasses<10>());
static_assert(priorityClasses<11>());
static_assert(priorityClasses<12>());
static_assert(priorityClasses<13>());
static_assert(priorityClasses<14>());
static_assert(priorityClasses<15>());

constexpr bool nestedServiceAndDuplicateRequest() {
  X86LocalApicFixedState Initial;
  Initial.TPR = 0xf3;
  for (unsigned V : {0x21, 0x81, 0xdf})
    set(Initial.ISR, V);
  for (unsigned V : {0x21, 0x8f, 0xdf})
    set(Initial.IRR, V);
  set(Initial.TMR, 0xdf);
  auto APIC = X86LocalApicFixed::fromState(Initial);
  if (APIC->processorPriority() != 0xf3 || APIC->nextFixedInterrupt() ||
      APIC->acceptFixedInterrupt(0xdf) || APIC->state() != Initial)
    return false;
  APIC->setTaskPriority(0);
  if (APIC->processorPriority() != 0xd0 || APIC->nextFixedInterrupt())
    return false;
  auto EOI = APIC->endOfInterrupt();
  if (!EOI || EOI->Vector != 0xdf || !EOI->Broadcast ||
      APIC->nextFixedInterrupt() != 0xdf || APIC->state().IRR != Initial.IRR ||
      !APIC->acceptFixedInterrupt(0xdf) || APIC->nextFixedInterrupt())
    return false;
  EOI = APIC->endOfInterrupt();
  if (!EOI || EOI->Vector != 0xdf || APIC->nextFixedInterrupt())
    return false; // 0x8f is blocked by 0x81: subpriority cannot preempt.
  EOI = APIC->endOfInterrupt();
  if (!EOI || EOI->Vector != 0x81 || EOI->Broadcast ||
      APIC->nextFixedInterrupt() != 0x8f || !APIC->acceptFixedInterrupt(0x8f))
    return false;
  EOI = APIC->endOfInterrupt();
  if (!EOI || EOI->Vector != 0x8f || APIC->nextFixedInterrupt())
    return false;
  EOI = APIC->endOfInterrupt();
  return EOI && EOI->Vector == 0x21 && APIC->nextFixedInterrupt() == 0x21 &&
         APIC->state().TMR == Initial.TMR;
}
static_assert(nestedServiceAndDuplicateRequest());

constexpr bool rejectReservedAndRetainEmpty() {
  for (unsigned V = 0; V < 16; ++V)
    for (unsigned Bank = 0; Bank < 3; ++Bank) {
      X86LocalApicFixedState Initial;
      set(Bank == 0 ? Initial.IRR : Bank == 1 ? Initial.ISR : Initial.TMR, V);
      if (X86LocalApicFixed::fromState(Initial))
        return false;
    }
  const X86LocalApicFixedState Empty;
  auto APIC = X86LocalApicFixed::fromState(Empty);
  if (!APIC || APIC->nextFixedInterrupt() || APIC->endOfInterrupt() ||
      APIC->acceptFixedInterrupt(0) || APIC->acceptFixedInterrupt(255) ||
      APIC->state() != Empty)
    return false;
  for (unsigned TPR = 0; TPR < 256; ++TPR) {
    APIC->setTaskPriority(TPR);
    if (APIC->processorPriority() != TPR || APIC->nextFixedInterrupt())
      return false;
  }
  return true;
}
static_assert(rejectReservedAndRetainEmpty());

} // namespace
