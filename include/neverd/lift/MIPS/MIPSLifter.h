//===- MIPSLifter.h - Classic MIPS integer lifting -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Strict MIPS32r2/MIPS64r2 semantics, independent of Capstone alias spelling.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIFT_MIPS_MIPSLIFTER_H
#define NEVERD_LIFT_MIPS_MIPSLIFTER_H

#include "neverd/Common.h"
#include "neverd/lift/LiftCommon.h"

#include "capstone/capstone.h"

#include <cstdint>
#include <vector>

namespace neverd {

class MIPSLifter {
public:
  explicit MIPSLifter(Arch Target,
                      InstructionMode Mode = InstructionMode::Default)
      : Target(Target), Mode(Mode) {}

  void lift(const cs_insn &Instruction, std::vector<LowOp> &Ops) const;
  LowDelaySlot liftUnit(const cs_insn &Head, const cs_insn &Slot,
                        std::vector<LowOp> &Ops) const;
  uint32_t word(const cs_insn &Instruction) const;
  static bool hasDelaySlot(uint32_t Word);
  /// Zero: no slot; one: either physical width; two/four: required width.
  unsigned delaySlotBytes(const cs_insn &Instruction) const;
  LowInstructionTargetMode controlTargetMode(const cs_insn &Instruction) const;
  bool isReturn(const cs_insn &Instruction) const;
  bool isAlignmentNop(const cs_insn &Instruction) const;
  bool isFunctionTerminator(const cs_insn &Instruction) const;
  va_t directCallTarget(const cs_insn &Instruction) const;
  void setDSPEnabled(bool Enabled) { HasDSP = Enabled; }
  void setMSAEnabled(bool Enabled) { HasMSA = Enabled; }
  void setMIPS16Enabled(bool Enabled) { HasMIPS16 = Enabled; }
  void setMIPS16E2Enabled(bool Enabled) { HasMIPS16E2 = Enabled; }

private:
  struct State : LiftStateBase {
    uint16_t Width;
    State(va_t Address, std::vector<LowOp> &Ops, uint16_t Width,
          uint16_t Size = 4)
        : LiftStateBase(Address, Size, Ops), Width(Width) {}

    NdVar read(unsigned Register, uint16_t Bytes = 0);
    void requireWord(unsigned Register);
    NdVar resize(NdVar Value, uint16_t Bytes, bool SignExtend = false);
    void write(unsigned Register, NdVar Value, bool SignExtend = true);
    NdVar binary(NdOp Opcode, NdVar A, NdVar B, uint16_t Bytes = 0);
  };

  bool liftInteger(State &S, uint32_t Word) const;
  bool liftBitfield(State &S, uint32_t Word) const;
  bool liftMemory(State &S, uint32_t Word,
                  std::optional<NdVar> BaseOverride = std::nullopt) const;
  bool liftPCRelative(State &S, uint32_t Word, va_t BasePC) const;
  void liftMIPS16Frame(State &S, uint32_t Word) const;
  bool liftDSP(State &S, uint32_t Word) const;
  bool liftMSA(State &S, uint32_t Word) const;
  struct Encoding {
    uint32_t Word;
    unsigned SlotBytes = 0;
    bool Unconditional = false;
    bool PCRelative = false;
    bool Frame = false;
  };
  bool liftNonControl(State &S, Encoding Code, va_t BasePC) const;
  std::optional<Encoding> encoding(const cs_insn &Instruction) const;
  std::optional<Encoding> microEncoding(const cs_insn &Instruction) const;
  std::optional<Encoding> mips16JumpEncoding(const cs_insn &Instruction) const;
  std::optional<Encoding> mips16Encoding(const cs_insn &Instruction) const;
  LowDelaySlot liftControl(const cs_insn &Head, const cs_insn *Slot,
                           Encoding Code, std::vector<LowOp> &Ops) const;
  Arch Target;
  InstructionMode Mode;
  bool HasDSP = false;
  bool HasMSA = false;
  bool HasMIPS16 = false;
  bool HasMIPS16E2 = false;
};

} // namespace neverd

#endif // NEVERD_LIFT_MIPS_MIPSLIFTER_H
