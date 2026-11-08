//===- MIPSRegs.cpp - Classic MIPS register names ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ABI-aware names for the classic MIPS integer register bank.
///
//===----------------------------------------------------------------------===//

#include "neverd/lift/MIPS/MIPSRegs.h"

namespace neverd {
namespace {

constexpr const char *O32Names[] = {
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3",
    "t4",   "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8",   "t9", "k0", "k1", "gp", "sp", "fp", "ra", "hi", "lo", "pc"};
constexpr const char *N64Names[] = {
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7",
    "t0",   "t1", "t2", "t3", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8",   "t9", "k0", "k1", "gp", "sp", "fp", "ra", "hi", "lo", "pc"};

} // namespace

const char *getMIPS32RegName(uint64_t Offset, uint16_t Size) {
  return Offset % 8 == 0 && Offset <= mipsreg::PC && Size <= 4
             ? O32Names[Offset / 8]
             : "unknown_mips_register";
}

const char *getMIPS64RegName(uint64_t Offset, uint16_t Size) {
  return Offset % 8 == 0 && Offset <= mipsreg::PC && Size <= 8
             ? N64Names[Offset / 8]
             : "unknown_mips_register";
}

} // namespace neverd
