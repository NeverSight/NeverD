//===- X86LocalApicPriorityProofs.cpp - Compile-only CR8/TPR proofs -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Ordered task-priority effects and import coherence, without execution.
//===----------------------------------------------------------------------===//

#include "neverd/translate/X86/X86LocalApic.h"

#include <cstddef>
#include <utility>

using neverd::translate::X86LocalApicFixed;
using neverd::translate::X86LocalApicFixedState;

namespace {

constexpr X86LocalApicFixedState bank(uint8_t TPR) {
  X86LocalApicFixedState Initial;
  Initial.TPR = TPR;
  Initial.IRR = {0x80000000, 1, 0, 0, 0, 0, 2, 0x80};
  Initial.ISR = {0, 0, 0, 0x80000000, 0, 0, 0x80000, 0};
  Initial.TMR = {0, 0x80000000, 0, 2, 0, 0, 0, 0x80};
  Initial.EOIBroadcastSuppressed = TPR & 1;
  return Initial;
}

template <unsigned TPR> constexpr bool readsImportsAndWrites() {
  const auto Initial = bank(TPR);
  auto APIC = X86LocalApicFixed::fromState(Initial, TPR / 16);
  if (!APIC || APIC->readCr8() != TPR / 16 || APIC->state() != Initial)
    return false;
  // Import/read retain subpriority even when in-service priority dominates.
  const unsigned PPR = TPR >= 0xd0 ? TPR : 0xd0;
  if (APIC->processorPriority() != PPR || APIC->state() != Initial)
    return false;
  for (unsigned Value = 0; Value < 16; ++Value) {
    const auto Imported = X86LocalApicFixed::fromState(Initial, Value);
    if (Imported.has_value() != (Value == TPR / 16) ||
        (Imported && Imported->state() != Initial))
      return false;
    APIC->setTaskPriority(TPR);
    if (APIC->state() != Initial || APIC->readCr8() != TPR / 16)
      return false;
    auto Written = Initial;
    Written.TPR = Value * 16;
    if (!APIC->writeCr8(Value) || APIC->state() != Written ||
        APIC->readCr8() != Value)
      return false;
    // A second write is real but idempotent after the low nibble was cleared.
    if (!APIC->writeCr8(Value) || APIC->state() != Written)
      return false;
  }
  return true;
}

template <unsigned TPR> constexpr bool reservedWritesAndImports() {
  const auto Initial = bank(TPR);
  auto APIC = X86LocalApicFixed::fromState(Initial);
  const auto Candidate = APIC->nextFixedInterrupt();
  // Every reserved source bit, with both empty and full legal low nibbles.
  // In particular, values above a byte must not narrow into a valid write.
  for (unsigned Bit = 4; Bit < 64; ++Bit)
    for (uint64_t Low : {0, 15}) {
      const uint64_t Invalid = (uint64_t(1) << Bit) | Low;
      if (APIC->writeCr8(Invalid) || APIC->state() != Initial ||
          APIC->readCr8() != TPR / 16 ||
          APIC->nextFixedInterrupt() != Candidate ||
          X86LocalApicFixed::fromState(Initial, Invalid))
        return false;
    }
  return !APIC->writeCr8(UINT64_MAX) && APIC->state() == Initial &&
         !X86LocalApicFixed::fromState(Initial, UINT64_MAX);
}

// Separate evaluations keep default compiler budgets. All 256 TPR bytes are
// exercised, not only low-nibble-zero states that hide same-value write bugs.
template <unsigned TPR> struct PriorityProof {
  static_assert(readsImportsAndWrites<TPR>());
  static_assert(reservedWritesAndImports<TPR>());
};
template <std::size_t... Values>
constexpr bool allTaskPriorities(std::index_sequence<Values...>) {
  return ((sizeof(PriorityProof<Values>) > 0) && ...);
}
static_assert(allTaskPriorities(std::make_index_sequence<256>{}));

template <unsigned TaskClass> constexpr bool admissionAfterWrite() {
  for (unsigned ServiceClass = 0; ServiceClass < 16; ++ServiceClass)
    for (unsigned PendingClass = 1; PendingClass < 16; ++PendingClass) {
      X86LocalApicFixedState Initial;
      Initial.TPR = 0xff;
      const unsigned Vector = PendingClass * 16 + 7;
      Initial.IRR[Vector / 32] = uint32_t(1) << (Vector % 32);
      Initial.TMR = Initial.IRR;
      if (ServiceClass) {
        const unsigned Service = ServiceClass * 16 + 3;
        Initial.ISR[Service / 32] = uint32_t(1) << (Service % 32);
      }
      auto APIC = X86LocalApicFixed::fromState(Initial, 15);
      if (APIC->nextFixedInterrupt() || !APIC->writeCr8(TaskClass))
        return false;
      const bool Eligible =
          PendingClass > TaskClass && PendingClass > ServiceClass;
      const auto Candidate = APIC->nextFixedInterrupt();
      auto Written = Initial;
      Written.TPR = TaskClass * 16;
      if (APIC->readCr8() != TaskClass || APIC->state() != Written ||
          Candidate.has_value() != Eligible ||
          (Candidate && *Candidate != Vector) ||
          APIC->acceptFixedInterrupt(Vector) != Eligible)
        return false;
      if (Eligible) {
        Written.IRR = {};
        Written.ISR[Vector / 32] |= uint32_t(1) << (Vector % 32);
      }
      if (APIC->state() != Written)
        return false;
    }
  return true;
}
// A separate assertion for each class keeps priority scans within the default
// evaluation limit; the independent expected eligibility uses class ordering.
static_assert(admissionAfterWrite<0>());
static_assert(admissionAfterWrite<1>());
static_assert(admissionAfterWrite<2>());
static_assert(admissionAfterWrite<3>());
static_assert(admissionAfterWrite<4>());
static_assert(admissionAfterWrite<5>());
static_assert(admissionAfterWrite<6>());
static_assert(admissionAfterWrite<7>());
static_assert(admissionAfterWrite<8>());
static_assert(admissionAfterWrite<9>());
static_assert(admissionAfterWrite<10>());
static_assert(admissionAfterWrite<11>());
static_assert(admissionAfterWrite<12>());
static_assert(admissionAfterWrite<13>());
static_assert(admissionAfterWrite<14>());
static_assert(admissionAfterWrite<15>());

constexpr bool pairedImportStillRejectsReservedBanks() {
  for (unsigned Vector = 0; Vector < 16; ++Vector)
    for (unsigned Bank = 0; Bank < 3; ++Bank) {
      auto Initial = bank(0x27);
      auto &Words = Bank == 0   ? Initial.IRR
                    : Bank == 1 ? Initial.ISR
                                : Initial.TMR;
      Words[0] |= uint32_t(1) << Vector;
      if (X86LocalApicFixed::fromState(Initial, 2))
        return false;
    }
  return true;
}
static_assert(pairedImportStillRejectsReservedBanks());

} // namespace
