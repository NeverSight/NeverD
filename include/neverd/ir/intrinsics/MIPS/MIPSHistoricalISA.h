//===- MIPSHistoricalISA.h - Historical MIPS availability ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared physical ISA admission before decoding aliases or lifting words.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_IR_INTRINSICS_MIPS_MIPSHISTORICALISA_H
#define NEVERD_IR_INTRINSICS_MIPS_MIPSHISTORICALISA_H

#include <array>
#include <cstdint>

namespace neverd::mips {
namespace historical {
enum Group { Major, Special, RegImm, Float, Cop1X, GroupCount };
inline constexpr auto FirstLevel = [] {
  std::array<std::array<uint8_t, 64>, GroupCount> Table{};
#define NEVERD_MIPS_HISTORICAL(Group, Selector, Level)                         \
  Table[Group][Selector] = Level;
#include "neverd/ir/intrinsics/MIPS/MIPSHistoricalISA.def"
  return Table;
}();
} // namespace historical

/// Doubleword integer instructions, including coprocessor moves through a GPR,
/// require the processor's 64-bit operation mode on the R4000.
constexpr bool historicalDoublewordGPRInstruction(uint32_t Word) {
  using namespace historical;
  const unsigned Op = Word >> 26;
  if (Op == 16 || Op == 17) {
    const unsigned RS = Word >> 21 & 31;
    return RS == 1 || RS == 5;
  }
  return Op ? FirstLevel[Major][Op] == 3 : FirstLevel[Special][Word & 63] == 3;
}

/// Width of an integer operation that raises the synchronous Overflow
/// exception.
constexpr unsigned historicalTrappingArithmeticBytes(uint32_t Word) {
  const unsigned Major = Word >> 26, Fn = Word & 63;
  if (Major == 8 || Major == 24)
    return Major == 8 ? 4 : 8;
  if (Major || (Word & 0x7c0))
    return 0;
  return Fn == 32 || Fn == 34 ? 4 : Fn == 44 || Fn == 46 ? 8 : 0;
}

constexpr bool historicalInstructionAvailable(uint32_t Word, unsigned Level) {
  using namespace historical;
  const unsigned Op = Word >> 26, RS = Word >> 21 & 31, RT = Word >> 16 & 31,
                 FS = Word >> 11 & 31, SA = Word >> 6 & 31, Fn = Word & 63;
  const auto Has = [Level](Group G, unsigned Selector) {
    const auto First = FirstLevel[G][Selector];
    return First && First <= Level;
  };
  if (Op == 0) {
    if (((Fn == 16 || Fn == 18) && RS) ||
        ((Fn == 17 || Fn == 19 || (Fn >= 24 && Fn <= 31)) && FS))
      return false; // DSP accumulator banks are not historical HI/LO.
    if ((Fn == 2 || Fn == 58 || Fn == 62) && RS)
      return false; // Release 2 rotates.
    if ((Fn == 6 || Fn == 22) && SA)
      return false;
    if (Fn == 15 && SA)
      return false; // Historical SYNC has no stype variants.
    return Has(Special, Fn);
  }
  if (Op == 1)
    return Has(RegImm, RT);
  if (Op == 16 && Level <= 3) {
    if (Word == 0x42000010)
      return Level <= 2; // RFE on cores with a KU/IE stack.
    if (Word == 0x42000018)
      return Level >= 2; // CW4011 R4000-compatible mode and R4000.
    const bool WordTransfer = RS == 0 || RS == 4;
    const bool DoubleTransfer = Level == 3 && (RS == 1 || RS == 5);
    const bool Address = FS == 8 || FS == 14 || (Level == 3 && FS == 30);
    return !(Word & 0x7ff) && (WordTransfer || DoubleTransfer) &&
           (Address || (WordTransfer && (FS == 12 || FS == 13))) &&
           (!(RS & 4) || FS != 8);
  }
  if (Op == 19)
    return Has(Cop1X, Fn);
  if (Op != 17)
    return Has(Major, Op);
  if (RS < 8) {
    if (RS == 2 || RS == 6)
      return FS == 31 || (RS == 2 && FS == 0) ||
             (Level >= 5 && (FS == 25 || FS == 26 || FS == 28));
    return RS == 0 || RS == 4 || (Level >= 3 && (RS == 1 || RS == 5));
  }
  if (RS == 8)
    return (!(RT & 2) || Level >= 2) && (RT < 4 || Level >= 4);
  if (RS == 22)
    return Level >= 5; // Paired-single operand validity belongs to COP1.
  if (RS != 16 && RS != 17 && RS != 20 && RS != 21)
    return false;
  if (RS == 21 && Level < 3)
    return false;
  if (Fn >= 48)
    return SA < 4 || Level >= 4; // FCC0 only before MIPS IV.
  return Has(Float, Fn);
}
} // namespace neverd::mips

#endif // NEVERD_IR_INTRINSICS_MIPS_MIPSHISTORICALISA_H
