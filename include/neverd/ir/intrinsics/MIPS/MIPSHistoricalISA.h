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
