//===- MIPSHistoricalSchedule.h - Physical instruction hazards -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Caller-bound scheduling state for non-interlocked historical instructions.
/// A branch and its executed slot retire separately. An annulled slot never
/// starts, and an unsuccessful instruction does not age the previous state.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_IR_LOW_MIPS_MIPSHISTORICALSCHEDULE_H
#define NEVERD_IR_LOW_MIPS_MIPSHISTORICALSCHEDULE_H

#include <cstdint>
#include <optional>

namespace neverd::mips {

enum HistoricalTimingAccess : uint16_t {
  ReadFCSR = 1u << 0,
  ReadFCC = 1u << 1,
  ReadHI = 1u << 2,
  ReadLO = 1u << 3,
  WriteHI = 1u << 4,
  WriteLO = 1u << 5,
  WriteFCSR = 1u << 6,
  WriteFCC = 1u << 7,
};

/// The low and high halves name GPR and FPR dependencies, respectively.
/// FPR bits describe architectural word registers in FR=0 and full registers
/// in FR=1. The lifter owns that projection, including merge-load forwarding.
struct HistoricalSchedulingState {
  uint64_t UnreadyRegisters = 0;
  uint8_t FCSRDelay = 0;
  uint8_t FCCDelay = 0;
  uint8_t HIReadDelay = 0;
  uint8_t LOReadDelay = 0;

  bool operator==(const HistoricalSchedulingState &) const = default;
};

class HistoricalSchedule {
public:
  /// The caller supplies the observed pipeline state. In particular, an
  /// empty snapshot is a quiescent boundary, not an inferred function entry.
  static std::optional<HistoricalSchedule>
  fromSnapshot(unsigned HistoricalISA, HistoricalSchedulingState State) {
    if (HistoricalISA < 1 || HistoricalISA > 3 ||
        (State.UnreadyRegisters & 1) || State.FCSRDelay > 1 ||
        State.FCCDelay > 2 || State.HIReadDelay > 2 || State.LOReadDelay > 2)
      return std::nullopt;
    return HistoricalSchedule(HistoricalISA, State);
  }

  const HistoricalSchedulingState &snapshot() const { return Current; }
  unsigned historicalISA() const { return ISA; }
  bool hasActiveInstruction() const { return Active.has_value(); }

  /// Check this instruction before its first architectural effect. The
  /// register masks and access flags describe the physical instruction, not
  /// synthetic CALL operands or one entire branch/slot lowering unit.
  bool begin(uint64_t PC, uint64_t Reads, uint64_t Delayed, uint16_t Access) {
    constexpr uint16_t Known = ReadFCSR | ReadFCC | ReadHI | ReadLO | WriteHI |
                               WriteLO | WriteFCSR | WriteFCC;
    if (Active || PC % 4 || (Delayed & 1) || (Access & ~Known) ||
        (Current.UnreadyRegisters & Reads) ||
        (Current.FCSRDelay && (Access & (ReadFCSR | ReadFCC))) ||
        (Current.FCCDelay && (Access & ReadFCC)) ||
        (Current.HIReadDelay && (Access & WriteHI)) ||
        (Current.LOReadDelay && (Access & WriteLO)))
      return false;

    const auto Age = [](uint8_t Delay) -> uint8_t {
      return Delay ? Delay - 1 : 0;
    };
    auto Next = Current;
    Next.UnreadyRegisters = Delayed;
    Next.FCSRDelay = Access & WriteFCSR ? 1 : Age(Next.FCSRDelay);
    Next.FCCDelay = Access & WriteFCSR  ? 2
                    : Access & WriteFCC ? 1
                                        : Age(Next.FCCDelay);
    Next.HIReadDelay = Access & ReadHI ? 2 : Age(Next.HIReadDelay);
    Next.LOReadDelay = Access & ReadLO ? 2 : Age(Next.LOReadDelay);
    Active = Instruction{PC, Next};
    return true;
  }

  bool retire(uint64_t PC) {
    if (!Active || Active->PC != PC)
      return false;
    Current = Active->Next;
    Active.reset();
    return true;
  }

  /// Cancel a failed attempt without consuming an instruction period. This
  /// does not model exception entry, interrupt delivery, CP0 propagation or
  /// reset; the caller must bind those events to an appropriate new snapshot.
  bool abort() {
    if (!Active)
      return false;
    Active.reset();
    return true;
  }

private:
  HistoricalSchedule(unsigned HistoricalISA, HistoricalSchedulingState State)
      : ISA(HistoricalISA), Current(State) {}
  struct Instruction {
    uint64_t PC;
    HistoricalSchedulingState Next;
  };
  unsigned ISA;
  HistoricalSchedulingState Current;
  std::optional<Instruction> Active;
};

} // namespace neverd::mips

#endif // NEVERD_IR_LOW_MIPS_MIPSHISTORICALSCHEDULE_H
