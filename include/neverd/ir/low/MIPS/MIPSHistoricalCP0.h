//===- MIPSHistoricalCP0.h - Early processor exception state ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_MIPS_MIPSHISTORICALCP0_H
#define NEVERD_IR_LOW_MIPS_MIPSHISTORICALCP0_H

#include <array>
#include <cstdint>
#include <optional>

namespace neverd::mips {

/// CW4011 implements MIPS II and has two selectable exception modes.
/// ISA level alone cannot select the mode, or the processor.
enum class HistoricalProcessor : uint8_t {
  R30xx,
  CW4011R3000,
  CW4011R4000,
  R4000
};

/// R30xx interrupt control bits visible at a single physical issue boundary.
struct R30xxInterruptVisibility {
  // Bit ranges [7:0], [8], [10:9] hold IM, IEc and software IP.
  static constexpr uint16_t AllFields = 0x7ff;
  uint16_t Required = 0, Possible = AllFields;
  uint8_t MixedLines = 0; // One of IEc and IM visible before the other.
};

struct HistoricalCP0 {
  HistoricalProcessor Processor;
  uint32_t Status;
  uint32_t Cause;
  uint64_t EPC;
  std::optional<uint64_t> BadVAddr;

  /// Entry PC after the last delivered exception. The handler consumes the
  /// ordinary execution boundary just like a jump target.
  std::optional<uint64_t> ExceptionVector;

  bool supportsLevel(unsigned Level) const;
  bool tracksBranchTaken() const {
    return Processor == HistoricalProcessor::CW4011R3000 ||
           Processor == HistoricalProcessor::CW4011R4000;
  }
  bool canUseCP0() const;
  /// R4000 kernel instructions can use doubleword GPRs even when KX is zero.
  bool permitsDoublewordGPRs() const;
  std::optional<uint64_t> returnAddress() const {
    return !usesModeStack() && (Status & 4) ? ErrorEPC : std::optional(EPC);
  }
  bool usesModeStack() const;
  std::optional<uint64_t> read(unsigned Register, bool Doubleword) const;
  /// Changes may only affect writable fields for which issue hazards are
  /// tracked. Refuse other board-dependent effects before retirement.
  bool canWrite(unsigned Register, uint64_t Value, bool Doubleword) const;
  static std::optional<unsigned> coprocessorOf(uint32_t Word);
  bool coprocessorUsable(unsigned Number) const;
  bool permitsIssue(uint32_t Word) const;
  /// A missing result needs the board's interrupt-recognition observation.
  std::optional<bool> interruptRecognition() const;
  bool canTakeInterrupt() const;
  bool enterException(uint8_t Code, uint64_t PC, std::optional<uint64_t> Branch,
                      std::optional<uint64_t> FaultAddress = {},
                      bool Taken = false,
                      std::optional<unsigned> Coprocessor = {});
  std::optional<uint64_t> ErrorEPC{};
  uint8_t ExceptionReturnDelay = 0;
  uint8_t InterruptDelay = 0;
  std::array<uint8_t, 4> CoprocessorDelay{};
  std::array<uint8_t, 4> ReadDelay{}; // Status, Cause, EPC and ErrorEPC.
  uint8_t ExceptionModeDelay = 0;
  /// In issue order. InterruptDelay counts the active entries on R30xx.
  std::array<R30xxInterruptVisibility, 2> R30xxInterrupts{};
  std::optional<bool> R30xxRecognized{};

private:
  friend class HistoricalSchedule;
  void propagateR30xxInterrupts(uint16_t Before, uint16_t After);
  void restoreR30xxInterruptEnable();
  void restoreExceptionState();
  void retire(std::optional<unsigned> Register, uint64_t Value,
              bool Doubleword);
};

} // namespace neverd::mips

#endif // NEVERD_IR_LOW_MIPS_MIPSHISTORICALCP0_H
