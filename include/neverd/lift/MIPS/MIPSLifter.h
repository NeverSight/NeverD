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
  explicit MIPSLifter(Arch Target) : Target(Target) {}

  void lift(const cs_insn &Instruction, std::vector<LowOp> &Ops) const;
  LowDelaySlot liftUnit(const cs_insn &Head, const cs_insn &Slot,
                        std::vector<LowOp> &Ops) const;
  uint32_t word(const cs_insn &Instruction) const;
  static bool hasDelaySlot(uint32_t Word);
  bool isReturn(const cs_insn &Instruction) const;
  bool isFunctionTerminator(const cs_insn &Instruction) const;
  va_t directCallTarget(const cs_insn &Instruction) const;

private:
  struct State : LiftStateBase {
    uint16_t Width;
    State(va_t Address, std::vector<LowOp> &Ops, uint16_t Width)
        : LiftStateBase(Address, 4, Ops), Width(Width) {}

    NdVar read(unsigned Register, uint16_t Bytes = 0);
    NdVar resize(NdVar Value, uint16_t Bytes, bool SignExtend = false);
    void write(unsigned Register, NdVar Value, bool SignExtend = true);
    NdVar binary(NdOp Opcode, NdVar A, NdVar B, uint16_t Bytes = 0);
  };

  bool liftInteger(State &S, uint32_t Word) const;
  bool liftMemory(State &S, uint32_t Word) const;
  Arch Target;
};

} // namespace neverd

#endif // NEVERD_LIFT_MIPS_MIPSLIFTER_H
