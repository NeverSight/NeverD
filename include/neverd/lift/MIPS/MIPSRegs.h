//===- MIPSRegs.h - Classic MIPS register storage ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Stable register storage offsets shared by MIPS32 and MIPS64. Values occupy
/// four or eight low bytes of each slot, independently of memory byte order.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIFT_MIPS_MIPSREGS_H
#define NEVERD_LIFT_MIPS_MIPSREGS_H

#include <cstdint>

namespace neverd {
namespace mipsreg {

inline constexpr uint64_t SlotBytes = 8;
inline constexpr uint64_t r(unsigned Index) { return Index * SlotBytes; }
#define NEVERD_MIPS_REGISTER(Name, Index, O32, N64)                            \
  inline constexpr uint64_t Name = r(Index);
#include "neverd/lift/MIPS/MIPSRegisters.def"

inline constexpr uint64_t accumulatorHI(unsigned Bank) {
  return Bank ? AC1HI + (Bank - 1) * 2 * SlotBytes : HI;
}
inline constexpr uint64_t accumulatorLO(unsigned Bank) {
  return Bank ? AC1LO + (Bank - 1) * 2 * SlotBytes : LO;
}
inline constexpr bool isAccumulator(uint64_t Offset) {
  return Offset == HI || Offset == LO ||
         (Offset >= AC1HI && Offset <= AC3LO && Offset % SlotBytes == 0);
}
inline constexpr uint64_t VectorBase = 0x200;
inline constexpr uint16_t VectorBytes = 16;
inline constexpr unsigned VectorCount = 32;
inline constexpr uint64_t w(unsigned Index) {
  return VectorBase + Index * VectorBytes;
}
inline constexpr bool isVector(uint64_t Offset) {
  return Offset >= VectorBase && Offset < w(VectorCount) &&
         (Offset - VectorBase) % VectorBytes == 0;
}

} // namespace mipsreg

const char *getMIPS32RegName(uint64_t Offset, uint16_t Size);
const char *getMIPS64RegName(uint64_t Offset, uint16_t Size);

} // namespace neverd

#endif // NEVERD_LIFT_MIPS_MIPSREGS_H
