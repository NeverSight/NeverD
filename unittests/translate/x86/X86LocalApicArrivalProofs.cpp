//===- X86LocalApicArrivalProofs.cpp - Compile-only APIC arrivals --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Fixed-message truth tables and two-slot service lifetimes; no execution.
//===----------------------------------------------------------------------===//

#include "neverd/translate/X86/X86LocalApic.h"

#include <cstddef>
#include <utility>

using neverd::translate::X86LocalApicArrival;
using neverd::translate::X86LocalApicFixed;
using neverd::translate::X86LocalApicFixedState;

namespace {

constexpr void put(std::array<uint32_t, 8> &Bank, unsigned Vector, bool Value) {
  const uint32_t Bit = uint32_t(1) << (Vector % 32);
  Bank[Vector / 32] = (Bank[Vector / 32] & ~Bit) | (Value ? Bit : 0);
}

// The independently specified table distinguishes the message's level bit
// from trigger mode. Deassertion does not withdraw an already pending request.
struct ArrivalCase {
  bool LevelTriggered, Asserted, WasPending;
  X86LocalApicArrival Result;
  bool Pending;
  int Trigger; // -1 retains the previous TMR bit.
};
constexpr ArrivalCase Cases[] = {
    {false, false, false, X86LocalApicArrival::Queued, true, 0},
    {false, false, true, X86LocalApicArrival::Coalesced, true, 0},
    {false, true, false, X86LocalApicArrival::Queued, true, 0},
    {false, true, true, X86LocalApicArrival::Coalesced, true, 0},
    {true, false, false, X86LocalApicArrival::IgnoredDeassertion, false, -1},
    {true, false, true, X86LocalApicArrival::IgnoredDeassertion, true, -1},
    {true, true, false, X86LocalApicArrival::Queued, true, 1},
    {true, true, true, X86LocalApicArrival::Coalesced, true, 1},
};

constexpr bool arrivalTable(unsigned Vector) {
  for (const auto &Case : Cases)
    for (unsigned Prior = 0; Prior < 4; ++Prior) {
      X86LocalApicFixedState Initial{
          .IRR = {0x98760000, 0xabcdef01, 0x23147658, 0x11111111, 0x80000001,
                  0x01234567, 0xfedcba98, 0x76543210},
          .ISR = {0x12340000, 0x87654321, 0xaabbccdd, 0x22222222, 0x01000080,
                  0x44444444, 0x89abcdef, 0x55555555},
          .TMR = {0x43210000, 0xfedcba98, 0x55555555, 0x80000001, 0x76543210,
                  0x87654321, 0x11111111, 0x2468ace0},
          .TPR = uint8_t(Prior & 1 ? 0xff : 0),
          .EOIBroadcastSuppressed = bool(Prior & 2)};
      put(Initial.IRR, Vector, Case.WasPending);
      put(Initial.ISR, Vector, Prior & 1);
      put(Initial.TMR, Vector, Prior & 2);
      auto Expected = Initial;
      put(Expected.IRR, Vector, Case.Pending);
      if (Case.Trigger >= 0)
        put(Expected.TMR, Vector, Case.Trigger);
      auto APIC = X86LocalApicFixed::fromState(Initial);
      if (!APIC ||
          APIC->receiveFixedInterrupt(Vector, Case.LevelTriggered,
                                      Case.Asserted) != Case.Result ||
          APIC->state() != Expected)
        return false;
    }
  return true;
}

constexpr bool twoServiceSlots(unsigned Vector) {
  for (bool Level : {false, true})
    for (bool Suppressed : {false, true}) {
      X86LocalApicFixedState Initial{.TPR = 0xff,
                                     .EOIBroadcastSuppressed = Suppressed};
      auto APIC = X86LocalApicFixed::fromState(Initial);
      if (APIC->receiveFixedInterrupt(Vector, Level, true) !=
              X86LocalApicArrival::Queued ||
          APIC->nextFixedInterrupt() || APIC->acceptFixedInterrupt(Vector) ||
          APIC->receiveFixedInterrupt(Vector, Level, true) !=
              X86LocalApicArrival::Coalesced)
        return false;
      APIC->setTaskPriority(0);
      if (APIC->nextFixedInterrupt() != Vector ||
          !APIC->acceptFixedInterrupt(Vector) || APIC->nextFixedInterrupt() ||
          APIC->receiveFixedInterrupt(Vector, Level, true) !=
              X86LocalApicArrival::Queued ||
          APIC->receiveFixedInterrupt(Vector, Level, true) !=
              X86LocalApicArrival::Coalesced ||
          APIC->nextFixedInterrupt() || APIC->acceptFixedInterrupt(Vector))
        return false;
      // One request is in service, one pending; no hidden third request.
      auto EOI = APIC->endOfInterrupt();
      if (!EOI || EOI->Vector != Vector || EOI->LevelTriggered != Level ||
          EOI->Broadcast != (Level && !Suppressed) ||
          APIC->nextFixedInterrupt() != Vector ||
          !APIC->acceptFixedInterrupt(Vector) || APIC->nextFixedInterrupt())
        return false;
      EOI = APIC->endOfInterrupt();
      Initial.TPR = 0;
      put(Initial.TMR, Vector, Level);
      if (!EOI || EOI->Vector != Vector || EOI->LevelTriggered != Level ||
          EOI->Broadcast != (Level && !Suppressed) ||
          APIC->nextFixedInterrupt() || APIC->endOfInterrupt() ||
          APIC->state() != Initial)
        return false;
    }
  return true;
}

// Each vector has its own constant evaluation, keeping compiler work bounded
// without increasing a constexpr budget. This covers 7,680 arrival cases and
// 960 complete pending/in-service lifetimes.
template <unsigned Vector> struct VectorProof {
  static_assert(arrivalTable(Vector));
  static_assert(twoServiceSlots(Vector));
  static constexpr bool Valid = true;
};
template <std::size_t... Index>
constexpr bool allVectors(std::index_sequence<Index...>) {
  return (VectorProof<16 + Index>::Valid && ...);
}
static_assert(allVectors(std::make_index_sequence<240>{}));

constexpr bool rejectReservedMessages() {
  const X86LocalApicFixedState Initial{.IRR = {0xffff0000},
                                       .ISR = {0x90000000},
                                       .TMR = {0x55550000},
                                       .TPR = 0xff,
                                       .EOIBroadcastSuppressed = true};
  auto APIC = X86LocalApicFixed::fromState(Initial);
  for (unsigned Vector = 0; Vector < 16; ++Vector)
    for (bool Level : {false, true})
      for (bool Asserted : {false, true})
        if (APIC->receiveFixedInterrupt(Vector, Level, Asserted) !=
                X86LocalApicArrival::RejectedVector ||
            APIC->state() != Initial)
          return false;
  return true;
}
static_assert(rejectReservedMessages());

constexpr bool latestTriggerSurvivesCoalescing() {
  auto APIC = X86LocalApicFixed::fromState({});
  if (APIC->receiveFixedInterrupt(0x81, true, true) !=
          X86LocalApicArrival::Queued ||
      !APIC->acceptFixedInterrupt(0x81) ||
      APIC->receiveFixedInterrupt(0x81, true, true) !=
          X86LocalApicArrival::Queued ||
      APIC->receiveFixedInterrupt(0x81, false, false) !=
          X86LocalApicArrival::Coalesced)
    return false;
  const auto Before = APIC->state();
  if (APIC->receiveFixedInterrupt(0x81, true, false) !=
          X86LocalApicArrival::IgnoredDeassertion ||
      APIC->state() != Before)
    return false;
  auto EOI = APIC->endOfInterrupt();
  if (!EOI || EOI->Vector != 0x81 || EOI->LevelTriggered || EOI->Broadcast ||
      APIC->nextFixedInterrupt() != 0x81 || !APIC->acceptFixedInterrupt(0x81) ||
      APIC->receiveFixedInterrupt(0x81, false, true) !=
          X86LocalApicArrival::Queued ||
      APIC->receiveFixedInterrupt(0x81, true, true) !=
          X86LocalApicArrival::Coalesced)
    return false;
  EOI = APIC->endOfInterrupt();
  return EOI && EOI->Vector == 0x81 && EOI->LevelTriggered && EOI->Broadcast &&
         APIC->nextFixedInterrupt() == 0x81;
}
static_assert(latestTriggerSurvivesCoalescing());

} // namespace
