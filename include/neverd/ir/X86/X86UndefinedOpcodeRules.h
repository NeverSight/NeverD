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

#include <cstddef>
#include <cstdint>
#include <optional>

namespace neverd {

/// UD0 raises #UD after its opcode on some CPUs. Others first fetch its
/// encoded ModR/M, SIB and displacement bytes. Neither reads the data operand.
/// The execution environment must bind this choice to the original CPU.
enum class X86UD0FetchProfile : uint8_t {
  OpcodeOnly,
  CompleteModRM,
};

/// Qualified code64 UD0 prefixes exclude LOCK. The original P/E-core oracle
/// covers zero, one or two such prefixes; Capstone reports the opcode without
/// the ModR/M address tail for every admitted combination.
inline constexpr unsigned X86PagedCode64UD0QualifiedPrefixLimit = 2;

constexpr bool x86PagedCode64UD0Prefix(uint8_t Byte) {
  if (Byte >= 0x40 && Byte <= 0x4f)
    return true; // REX
  switch (Byte) {
  case 0x26:
  case 0x2e:
  case 0x36:
  case 0x3e:
  case 0x64:
  case 0x65: // Segment override.
  case 0x66: // Operand size.
  case 0x67: // Address size.
  case 0xf2:
  case 0xf3: // REP/REPNZ.
    return true;
  default:
    return false;
  }
}

constexpr std::optional<unsigned>
x86PagedCode64UD0OpcodeOffset(const uint8_t *Bytes, size_t Size) {
  unsigned Offset = 0;
  while (Offset < X86PagedCode64UD0QualifiedPrefixLimit && Offset < Size &&
         x86PagedCode64UD0Prefix(Bytes[Offset]))
    ++Offset;
  if (Size < Offset + 2 || Bytes[Offset] != 0x0f || Bytes[Offset + 1] != 0xff)
    return std::nullopt;
  return Offset;
}

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
