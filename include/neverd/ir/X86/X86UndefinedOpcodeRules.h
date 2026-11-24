//===- X86UndefinedOpcodeRules.h - CPU fetch choices -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Processor-specific instruction-fetch priority for undefined opcodes.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_X86_X86UNDEFINEDOPCODERULES_H
#define NEVERD_IR_X86_X86UNDEFINEDOPCODERULES_H

#include <cstdint>

namespace neverd {

/// UD0 raises #UD after its opcode on some CPUs. Others first fetch its
/// encoded ModR/M, SIB and displacement bytes. Neither reads the data operand.
/// The execution environment must bind this choice to the original CPU.
enum class X86UD0FetchProfile : uint8_t {
  OpcodeOnly,
  CompleteModRM,
};

/// The encoded address extent after UD0's two opcode bytes in 32/64-bit
/// addressing. SIB is needed only when x86UD0NeedsSIB32Or64 returns true.
/// This counts instruction-fetch bytes; UD0 never reads the data operand.
constexpr bool x86UD0NeedsSIB32Or64(uint8_t ModRM) {
  return (ModRM >> 6) != 3 && (ModRM & 7) == 4;
}

constexpr unsigned x86UD0AddressBytes32Or64(uint8_t ModRM, uint8_t SIB = 0) {
  const unsigned Mod = ModRM >> 6;
  const unsigned RM = ModRM & 7;
  if (Mod == 3)
    return 1;
  const bool HasSIB = x86UD0NeedsSIB32Or64(ModRM);
  unsigned Disp = 0;
  if (Mod == 1)
    Disp = 1;
  else if (Mod == 2 || (Mod == 0 && (HasSIB ? (SIB & 7) == 5 : RM == 5)))
    Disp = 4;
  return 1 + HasSIB + Disp;
}

} // namespace neverd

#endif // NEVERD_IR_X86_X86UNDEFINEDOPCODERULES_H
