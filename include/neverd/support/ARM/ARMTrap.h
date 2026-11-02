//===- ARMTrap.h - ARM exception instruction identities --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_ARM_ARMTRAP_H
#define NEVERD_SUPPORT_ARM_ARMTRAP_H

#include <capstone/capstone.h>
#include <cstdint>
#include <optional>

namespace neverd {

/// Capstone gives compiler UDF encodings their own ids. They still raise an
/// undefined-instruction exception, including Thumb's UDF #249 (__brkdiv0).
inline bool isARMUndefinedInstruction(unsigned Id) {
  return Id == ARM_INS_UDF || Id == ARM_INS_TRAP || Id == ARM_INS___BRKDIV0;
}

/// These instructions can resume through an exception handler or debugger.
/// Stopping discovery at one does not grant it a no-return source contract.
inline bool isARMResumableTrapInstruction(unsigned Id) {
  return Id == ARM_INS_BKPT || Id == ARM_INS_HLT ||
         isARMUndefinedInstruction(Id);
}

/// The immediate of a detailed ARM exception instruction. Compiler UDF
/// aliases omit the operand in Capstone, but retain it in their encoding.
inline std::optional<uint64_t> armExceptionCode(const cs_insn &Insn,
                                                const cs_arm &ARM) {
  if (ARM.op_count >= 1 && ARM.operands[0].type == ARM_OP_IMM)
    return static_cast<uint64_t>(ARM.operands[0].imm) & 0xFFFF;
  if (!isARMUndefinedInstruction(Insn.id))
    return std::nullopt;
  if (Insn.size == 2)
    return Insn.bytes[0];
  if (Insn.size == 4) {
    const uint32_t Word = static_cast<uint32_t>(Insn.bytes[0]) |
                          static_cast<uint32_t>(Insn.bytes[1]) << 8 |
                          static_cast<uint32_t>(Insn.bytes[2]) << 16 |
                          static_cast<uint32_t>(Insn.bytes[3]) << 24;
    return ((Word >> 8) & 0xFFF) << 4 | (Word & 0xF);
  }
  return std::nullopt;
}

} // namespace neverd

#endif
