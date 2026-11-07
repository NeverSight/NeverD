//===- X86LegacyBMI.cpp - Protected VEX integer instructions -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Dword BMI values and explicit undefined flags, independent of host CPUID.
//===----------------------------------------------------------------------===//

#include "X86LegacyBMI.h"

#include <bit>

namespace neverd::translate::detail {

using Op = X86LegacyBMIOperation;

bool X86LegacyBMIInstruction::enabled(
    const X86LegacyIntegerExtensions &Features) const {
  switch (Operation) {
  case Op::AndNot:
  case Op::Extract:
  case Op::IsolateLowest:
  case Op::MaskLowest:
  case Op::ResetLowest:
    return Features.BMI1;
  default:
    return Features.BMI2;
  }
}

uint32_t X86LegacyBMIInstruction::undefinedFlagsMask() const {
  switch (Operation) {
  case Op::Extract:
    return kX86LegacyBEXTRUndefinedFlagsMask;
  case Op::AndNot:
  case Op::IsolateLowest:
  case Op::MaskLowest:
  case Op::ResetLowest:
  case Op::ZeroHigh:
    return kX86LegacyBMIUndefinedFlagsMask;
  default:
    return 0;
  }
}

std::optional<X86LegacyBMIInstruction> decodeLegacyBMI(X86LegacyEncoding &R) {
  if (R.Group1Prefix || R.OperandOverride)
    return std::nullopt;
  const uint8_t P0 = R.byte(), P1 = R.byte(), Opcode = R.byte();
  // R/X must be set to distinguish VEX from LES in protected mode. W, B and
  // the high vvvv bit do not extend these non-64-bit operands. L is reserved.
  if ((P0 & 0xc0) != 0xc0 || (P1 & 4))
    return std::nullopt;
  const unsigned Map = P0 & 0x1f, PP = P1 & 3;
  X86LegacyBMIInstruction F{Op::AndNot};
  F.VRegister = (~P1 >> 3) & 7;
  const unsigned Reg = R.modRM(F.Source);
  F.Destination = Reg;
  if (Map == 2 && PP == 0 && Opcode == 0xf2) {
    F.Operation = Op::AndNot;
  } else if (Map == 2 && PP == 0 && Opcode == 0xf3 && Reg >= 1 && Reg <= 3) {
    F.Operation = Reg == 1   ? Op::ResetLowest
                  : Reg == 2 ? Op::MaskLowest
                             : Op::IsolateLowest;
    F.Destination = F.VRegister;
  } else if (Map == 2 && Opcode == 0xf5 && PP != 1) {
    F.Operation = PP == 0 ? Op::ZeroHigh : PP == 2 ? Op::Gather : Op::Deposit;
  } else if (Map == 2 && Opcode == 0xf6 && PP == 3) {
    F.Operation = Op::Multiply;
  } else if (Map == 2 && Opcode == 0xf7) {
    constexpr Op Operations[]{Op::Extract, Op::ShiftLeft, Op::SignedShiftRight,
                              Op::ShiftRight};
    F.Operation = Operations[PP];
  } else if (Map == 3 && Opcode == 0xf0 && PP == 3 && (P1 & 0x78) == 0x78) {
    F.Operation = Op::RotateRight;
    F.Immediate = R.byte();
  } else {
    return std::nullopt;
  }
  return F;
}

void executeLegacyBMI(const X86LegacyBMIInstruction &F,
                      std::array<uint32_t, 8> &GPR, uint32_t &Flags,
                      uint32_t Source, uint32_t UndefinedFlags) {
  const uint32_t V = GPR[F.VRegister];
  uint32_t Result = 0;
  bool Carry = false;
  switch (F.Operation) {
  case Op::AndNot:
    Result = ~V & Source;
    break;
  case Op::Extract: {
    const unsigned Start = uint8_t(V), Length = uint8_t(V >> 8);
    if (Start < 32 && Length) {
      Result = Source >> Start;
      if (Length < 32)
        Result &= (uint32_t{1} << Length) - 1;
    }
    break;
  }
  case Op::IsolateLowest:
    Result = (0u - Source) & Source;
    Carry = Source != 0;
    break;
  case Op::MaskLowest:
    Result = (Source - 1) ^ Source;
    Carry = Source == 0;
    break;
  case Op::ResetLowest:
    Result = (Source - 1) & Source;
    Carry = Source == 0;
    break;
  case Op::ZeroHigh: {
    const unsigned Index = uint8_t(V);
    Carry = Index >= 32;
    Result = Carry ? Source : Source & ((uint32_t{1} << Index) - 1);
    break;
  }
  case Op::Multiply: {
    const uint64_t Product = uint64_t(GPR[2]) * Source;
    GPR[F.VRegister] = uint32_t(Product);
    Result = uint32_t(Product >> 32);
    break;
  }
  case Op::Deposit:
  case Op::Gather: {
    // At most one iteration per selected mask bit, with unsigned wrap even
    // when all 32 bits are selected. No host BMI instruction is required.
    uint32_t Packed = 1;
    while (Source) {
      const uint32_t Bit = (0u - Source) & Source;
      if (F.Operation == Op::Deposit) {
        if (V & Packed)
          Result |= Bit;
      } else if (V & Bit) {
        Result |= Packed;
      }
      Source &= Source - 1;
      Packed <<= 1;
    }
    break;
  }
  case Op::RotateRight:
    Result = std::rotr(Source, int(F.Immediate & 31));
    break;
  case Op::SignedShiftRight:
    Result = uint32_t(int32_t(Source) >> (V & 31));
    break;
  case Op::ShiftLeft:
    Result = Source << (V & 31);
    break;
  case Op::ShiftRight:
    Result = Source >> (V & 31);
    break;
  }
  GPR[F.Destination] = Result;
  if (F.undefinedFlagsMask()) {
    // BEXTR leaves SF undefined; all other flag-writing BMI forms compute it.
    const uint32_t Sign =
        F.Operation == Op::Extract ? 0 : (Result >> 24) & 0x80;
    Flags = (Flags & ~kX86LegacyArithmeticFlagsMask) | UndefinedFlags | Sign |
            (!Result ? 64 : 0) | Carry;
  }
}

} // namespace neverd::translate::detail
