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
  bool usesModeStack() const;
  std::optional<uint64_t> read(unsigned Register, bool Doubleword) const;
  /// Changes may only affect writable fields for which issue hazards are
  /// tracked. Refuse other board-dependent effects before retirement.
  bool canWrite(unsigned Register, uint64_t Value, bool Doubleword) const;
  static std::optional<unsigned> coprocessorOf(uint32_t Word);
  bool coprocessorUsable(unsigned Number) const;
  bool permitsIssue(uint32_t Word) const;
  bool canTakeInterrupt() const;
  bool enterException(uint8_t Code, uint64_t PC, std::optional<uint64_t> Branch,
                      std::optional<uint64_t> FaultAddress = {},
                      bool Taken = false,
                      std::optional<unsigned> Coprocessor = {});
  std::optional<uint64_t> ErrorEPC{};
  uint8_t ExceptionReturnDelay = 0;
  uint8_t InterruptDelay = 0;
  std::array<uint8_t, 4> CoprocessorDelay{};
  std::array<uint8_t, 3> ReadDelay{}; // Writable Status, Cause and EPC.

private:
  friend class HistoricalSchedule;
  void restoreExceptionState();
  void retire(std::optional<unsigned> Register, uint64_t Value,
              bool Doubleword);
};

} // namespace neverd::mips

#endif // NEVERD_IR_LOW_MIPS_MIPSHISTORICALCP0_H
