//===- X86LocalApic.h - Owned fixed-interrupt APIC banks --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Local APIC fixed arrivals, priority, acknowledgment and ordinary EOI.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TRANSLATE_X86_X86LOCALAPIC_H
#define NEVERD_TRANSLATE_X86_X86LOCALAPIC_H

#include <array>
#include <bit>
#include <cstdint>
#include <optional>

namespace neverd::translate {

struct X86LocalApicFixedState {
  std::array<uint32_t, 8> IRR{};
  std::array<uint32_t, 8> ISR{};
  std::array<uint32_t, 8> TMR{};
  uint8_t TPR = 0;
  bool EOIBroadcastSuppressed = false;

  constexpr bool operator==(const X86LocalApicFixedState &) const = default;
};

struct X86LocalApicEOI {
  uint8_t Vector;
  bool LevelTriggered;
  /// The embedding must route this request to its interrupt controllers.
  /// Returning it does not acknowledge an I/O APIC or deassert a device line.
  bool Broadcast;
};

enum class X86LocalApicArrival : uint8_t {
  RejectedVector,
  IgnoredDeassertion,
  Queued,
  Coalesced,
};

/// An owned bank for an enabled modern APIC using ordinary fixed delivery/EOI
/// and one coalescing IRR bit per vector (not the older P6 rejection profile).
/// The embedding binds that profile, synchronizes original TPR/CR8 writes and
/// serializes access. Arrival timing/routing, APIC mode changes, NMI/ExtINT
/// arbitration, software interrupts, spurious vectors, auto-EOI/SynIC, posted
/// interrupts, timer/device state and CPU IF/shadows/wakeup remain outside this
/// component.
///
/// nextFixedInterrupt is a nonconsuming local candidate. The continuation
/// scheduler owns IF/shadow admission and calls its producer's accepted only
/// after acceptance (including faulting paged delivery). Bridge that callback
/// to acceptFixedInterrupt with no intervening producer-bank/TPR mutation.
/// ISR stays set across handler execution and IRET until an explicit EOI.
/// No object registers itself with a scheduler or adds work to a decoder.
class X86LocalApicFixed {
  X86LocalApicFixedState State;

  explicit constexpr X86LocalApicFixed(X86LocalApicFixedState Initial)
      : State(Initial) {}

  static constexpr std::optional<uint8_t>
  highest(const std::array<uint32_t, 8> &Bank) {
    for (unsigned Word = Bank.size(); Word != 0; --Word)
      if (Bank[Word - 1])
        return uint8_t((Word - 1) * 32 + 31 - std::countl_zero(Bank[Word - 1]));
    return std::nullopt;
  }

public:
  /// Vectors 0..15 are not ordinary fixed interrupts in this profile. Reject
  /// their banks instead of inventing ESR/fault behavior or dropping the bits.
  static constexpr std::optional<X86LocalApicFixed>
  fromState(X86LocalApicFixedState Initial) {
    if ((Initial.IRR[0] | Initial.ISR[0] | Initial.TMR[0]) & 0xffff)
      return std::nullopt;
    return X86LocalApicFixed(Initial);
  }

  constexpr const X86LocalApicFixedState &state() const { return State; }

  /// Receive a fixed message already routed to this enabled APIC. This is
  /// local bank acceptance, not processor delivery or a device-line update.
  /// Edge messages ignore Asserted; level deassertions preserve every bank.
  /// A new request can wait in IRR while the same vector remains in ISR.
  /// Further requests coalesce, but still replace that vector's TMR bit.
  /// Reserved vectors fail without inventing ESR/error-interrupt effects.
  [[nodiscard]] constexpr X86LocalApicArrival
  receiveFixedInterrupt(uint8_t Vector, bool LevelTriggered, bool Asserted) {
    if (Vector < 16)
      return X86LocalApicArrival::RejectedVector;
    if (LevelTriggered && !Asserted)
      return X86LocalApicArrival::IgnoredDeassertion;
    const uint32_t Bit = uint32_t(1) << (Vector % 32);
    const unsigned Word = Vector / 32;
    const bool Pending = State.IRR[Word] & Bit;
    State.IRR[Word] |= Bit;
    if (LevelTriggered)
      State.TMR[Word] |= Bit;
    else
      State.TMR[Word] &= ~Bit;
    return Pending ? X86LocalApicArrival::Coalesced
                   : X86LocalApicArrival::Queued;
  }

  /// A byte write to the actual TPR. This does not synthesize a CR8 write or
  /// infer its completion from a changed CPU state; the producer owns those.
  constexpr void setTaskPriority(uint8_t Value) { State.TPR = Value; }

  constexpr uint8_t processorPriority() const {
    const uint8_t ServiceClass = highest(State.ISR).value_or(0) & 0xf0;
    return (State.TPR & 0xf0) >= ServiceClass ? State.TPR : ServiceClass;
  }

  constexpr std::optional<uint8_t> nextFixedInterrupt() const {
    const auto Vector = highest(State.IRR);
    if (!Vector || (*Vector & 0xf0) <= (processorPriority() & 0xf0))
      return std::nullopt;
    return Vector;
  }

  /// Consume precisely the current candidate. A wrong, blocked or repeated
  /// acknowledgment fails without changing any bank. The producer serializes
  /// arrivals and TPR writes with its selection/acknowledgment boundary.
  constexpr bool acceptFixedInterrupt(uint8_t Vector) {
    if (nextFixedInterrupt() != Vector)
      return false;
    const uint32_t Bit = uint32_t(1) << (Vector % 32);
    State.IRR[Vector / 32] &= ~Bit;
    State.ISR[Vector / 32] |= Bit;
    return true;
  }

  /// Clear the highest in-service vector, not the highest pending vector.
  /// Retain TMR and report the external EOI obligation separately. An empty
  /// ISR is an effect-free EOI; it never consumes an IRR request.
  constexpr std::optional<X86LocalApicEOI> endOfInterrupt() {
    const auto Vector = highest(State.ISR);
    if (!Vector)
      return std::nullopt;
    const uint32_t Bit = uint32_t(1) << (*Vector % 32);
    const bool Level = State.TMR[*Vector / 32] & Bit;
    State.ISR[*Vector / 32] &= ~Bit;
    return X86LocalApicEOI{*Vector, Level,
                           Level && !State.EOIBroadcastSuppressed};
  }
};

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_X86_X86LOCALAPIC_H
