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

#include "neverd/ir/intrinsics/MIPS/MIPSHistoricalTiming.h"
#include "neverd/ir/low/MIPS/MIPSHistoricalCP0.h"

#include <cstdint>
#include <optional>

namespace neverd::mips {

/// The low and high halves name GPR and FPR dependencies, respectively.
/// FPR bits describe architectural word registers in FR=0 and full registers
/// in FR=1. The lifter owns that projection, including merge-load forwarding.
struct HistoricalSchedulingState {
  uint64_t UnreadyRegisters = 0;
  uint8_t FCSRDelay = 0;
  uint8_t FCCDelay = 0;
  uint8_t HIReadDelay = 0;
  uint8_t LOReadDelay = 0;
  uint32_t ForwardableLoad = 0;
  bool PendingProduct = false;
  bool UnknownHI = false;
  bool UnknownLO = false;

  bool operator==(const HistoricalSchedulingState &) const = default;
};

class HistoricalSchedule {
public:
  /// The caller supplies the observed pipeline state. In particular, an
  /// empty snapshot is a quiescent boundary, not an inferred function entry.
  static std::optional<HistoricalSchedule>
  fromSnapshot(unsigned HistoricalISA, HistoricalSchedulingState State,
               std::optional<HistoricalCP0> System = {}) {
    if (HistoricalISA < 1 || HistoricalISA > 3 ||
        (System && !System->supportsLevel(HistoricalISA)) ||
        (State.UnreadyRegisters & 1) || State.FCSRDelay > 1 ||
        State.FCCDelay > 2 || State.HIReadDelay > 2 || State.LOReadDelay > 2 ||
        (State.ForwardableLoad & ~State.UnreadyRegisters) ||
        (State.PendingProduct && (State.UnknownHI || State.UnknownLO)))
      return std::nullopt;
    return HistoricalSchedule(HistoricalISA, State, System);
  }

  const std::optional<HistoricalCP0> &system() const { return System; }

  bool beginPhysical(uint64_t PC, uint32_t Word, bool Paired,
                     std::optional<bool> SlotTaken = {}) {
    if (PC & 3) {
      if (!SlotTaken)
        enterFetchAddressError(PC);
      return false;
    }
    const auto T = historicalInstructionTiming(Word, ISA, Paired);
    if (!T || (System && !System->permitsIssue(Word)) ||
        !begin(PC, T->Reads, T->Delayed, T->Access, T->MergeSource,
               T->ForwardableLoad))
      return false;
    Active->Word = Word;
    Active->SlotTaken = SlotTaken;
    return true;
  }

  std::optional<uint32_t> physicalWord(uint64_t PC) const {
    return Active && Active->PC == PC ? Active->Word : std::nullopt;
  }
  /// A board supplies levels on IP[7:2] at an instruction boundary.
  bool setHardwareInterrupts(uint8_t Lines) {
    if (Active || !System || Lines >= 64)
      return false;
    if (((System->Cause >> 10) & 63) != Lines)
      System->R30xxRecognized.reset();
    System->Cause = (System->Cause & ~0xfc00u) | uint32_t(Lines) << 10;
    return true;
  }
  /// Bind the interrupt controller's sample at this physical boundary when
  /// the R30xx Status transition admits either result. A retry sees the same
  /// sample until retirement, exception entry, or a change to the input lines.
  bool observeInterruptRecognition(bool Taken) {
    if (Active || !System || System->interruptRecognition().has_value())
      return false;
    System->R30xxRecognized = Taken;
    return true;
  }
  bool matchesTransfer(uint64_t PC, unsigned RS, unsigned Register) const {
    return Register < 32 && Active && Active->PC == PC && Active->Word &&
           System &&
           (*Active->Word & 0xffe0ffffu) ==
               (0x40000000u | RS << 21 | Register << 11);
  }
  bool stageCP0Write(uint64_t PC, unsigned Register, uint64_t Value,
                     bool Doubleword) {
    if (!matchesTransfer(PC, Doubleword ? 5 : 4, Register) ||
        Active->Register || !System->canWrite(Register, Value, Doubleword))
      return false;
    Active->Register = Register;
    Active->Value = Value;
    return true;
  }
  std::optional<uint64_t> stageExceptionReturn(uint64_t PC, bool ERET) {
    if (!System || !Active || Active->PC != PC ||
        Active->Word != (ERET ? 0x42000018u : 0x42000010u) ||
        !System->canUseCP0() || System->usesModeStack() == ERET ||
        System->ExceptionReturnDelay || Active->RestoresStatus ||
        (ERET && Active->SlotTaken))
      return std::nullopt;
    const auto Target = System->returnAddress();
    if (!Target)
      return std::nullopt;
    Active->RestoresStatus = true;
    return Target;
  }
  std::optional<uint64_t> retiringERETTarget(uint64_t PC) const {
    if (Active && Active->PC == PC && Active->RestoresStatus &&
        Active->Word == 0x42000018u)
      return System->returnAddress();
    return std::nullopt;
  }
  /// A misaligned target faults on fetch, after the preceding transfer and
  /// its executed slot have retired. The target instruction never issues.
  bool enterFetchAddressError(uint64_t PC) {
    return !Active && System && (PC & 3) &&
           System->enterException(4, PC, {}, PC);
  }
  bool raiseSoftwareException(uint64_t PC, uint8_t Code) {
    if (!Active || !Active->Word ||
        (*Active->Word & 0xfc00003fu) != (Code == 8 ? 12u : 13u) ||
        (Code != 8 && Code != 9))
      return false;
    return enterException(PC, Code);
  }
  bool enterException(uint64_t PC, uint8_t Code,
                      std::optional<uint64_t> FaultAddress = {}) {
    if (!System || !Active || Active->PC != PC || !Active->Word)
      return false;
    const auto Branch =
        Active->SlotTaken ? std::optional(PC - 4) : std::nullopt;
    if (!System->enterException(
            Code, PC, Branch, FaultAddress, Active->SlotTaken.value_or(false),
            Code == 11 ? HistoricalCP0::coprocessorOf(*Active->Word)
                       : std::nullopt))
      return false;
    Active.reset();
    return true;
  }

  const HistoricalSchedulingState &snapshot() const { return Current; }
  unsigned historicalISA() const { return ISA; }
  bool hasActiveInstruction() const { return Active.has_value(); }

  /// Check this instruction before its first architectural effect. The
  /// register masks and access flags describe the physical instruction, not
  /// synthetic CALL operands or one entire branch/slot lowering unit.
  bool begin(uint64_t PC, uint64_t Reads, uint64_t Delayed, uint16_t Access,
             uint32_t MergeSource = 0, uint32_t ForwardableLoad = 0) {
    constexpr uint16_t Known = ReadFCSR | ReadFCC | ReadHI | ReadLO | WriteHI |
                               WriteLO | WriteFCSR | WriteFCC;
    // LWL/LWR forward the preceding memory-load destination into their merge
    // operand. The base is an ordinary read, even when it aliases rt. Do not
    // infer that a coprocessor move has the same bypass.
    Reads |= MergeSource & ~Current.ForwardableLoad;
    if (Active || PC % 4 || (Delayed & 1) || (Access & ~Known) ||
        (ForwardableLoad & ~Delayed) || (Current.UnreadyRegisters & Reads) ||
        (Current.UnknownHI && (Access & ReadHI)) ||
        (Current.UnknownLO && (Access & ReadLO)) ||
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
    Next.ForwardableLoad = ForwardableLoad;
    Next.FCSRDelay = Access & WriteFCSR ? 1 : Age(Next.FCSRDelay);
    Next.FCCDelay = Access & WriteFCSR  ? 2
                    : Access & WriteFCC ? 1
                                        : Age(Next.FCCDelay);
    Next.HIReadDelay = Access & ReadHI ? 2 : Age(Next.HIReadDelay);
    Next.LOReadDelay = Access & ReadLO ? 2 : Age(Next.LOReadDelay);
    const auto Writes = Access & (WriteHI | WriteLO);
    if (Writes == (WriteHI | WriteLO)) {
      Next.PendingProduct = true;
      Next.UnknownHI = Next.UnknownLO = false;
    } else if (Writes) {
      if (Current.PendingProduct) {
        Next.UnknownHI = Writes == WriteLO;
        Next.UnknownLO = Writes == WriteHI;
      }
      if (Writes & WriteHI)
        Next.UnknownHI = false;
      if (Writes & WriteLO)
        Next.UnknownLO = false;
      Next.PendingProduct = false;
    } else if (Access & (ReadHI | ReadLO))
      Next.PendingProduct = false;
    Active = Instruction{PC, Next};
    return true;
  }

  bool retire(uint64_t PC) {
    if (!Active || Active->PC != PC)
      return false;
    if (System)
      System->retire(Active->Register, Active->Value,
                     Active->Word && ((*Active->Word >> 21 & 31) == 5));
    if (Active->RestoresStatus)
      System->restoreExceptionState();
    Current = Active->Next;
    Active.reset();
    return true;
  }

  /// Cancel a failed attempt without consuming an instruction period. This
  /// leaves CP0 untouched. Use enterException to deliver a known fault.
  bool abort() {
    if (!Active)
      return false;
    Active.reset();
    return true;
  }

private:
  HistoricalSchedule(unsigned HistoricalISA, HistoricalSchedulingState State,
                     std::optional<HistoricalCP0> System)
      : ISA(HistoricalISA), Current(State), System(System) {}
  struct Instruction {
    uint64_t PC;
    HistoricalSchedulingState Next;
    std::optional<uint32_t> Word{};
    std::optional<bool> SlotTaken{};
    std::optional<unsigned> Register{};
    uint64_t Value = 0;
    bool RestoresStatus = false;
  };
  unsigned ISA;
  HistoricalSchedulingState Current;
  std::optional<HistoricalCP0> System;
  std::optional<Instruction> Active;
};

} // namespace neverd::mips

#endif // NEVERD_IR_LOW_MIPS_MIPSHISTORICALSCHEDULE_H
