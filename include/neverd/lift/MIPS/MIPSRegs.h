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

inline constexpr uint64_t r(unsigned Index) { return Index * 8; }
inline constexpr uint64_t Zero = r(0);
inline constexpr uint64_t V0 = r(2);
inline constexpr uint64_t V1 = r(3);
inline constexpr uint64_t A0 = r(4);
inline constexpr uint64_t T9 = r(25);
inline constexpr uint64_t GP = r(28);
inline constexpr uint64_t SP = r(29);
inline constexpr uint64_t FP = r(30);
inline constexpr uint64_t RA = r(31);
inline constexpr uint64_t HI = r(32);
inline constexpr uint64_t LO = r(33);
inline constexpr uint64_t PC = r(34);

} // namespace mipsreg

const char *getMIPS32RegName(uint64_t Offset, uint16_t Size);
const char *getMIPS64RegName(uint64_t Offset, uint16_t Size);

} // namespace neverd

#endif // NEVERD_LIFT_MIPS_MIPSREGS_H
