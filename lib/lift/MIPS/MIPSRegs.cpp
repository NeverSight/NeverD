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

#include <array>

namespace neverd {
namespace {

struct RegisterNames {
  const char *O32;
  const char *N64;
};

constexpr auto Names = [] {
  std::array<RegisterNames, mipsreg::PC / mipsreg::SlotBytes + 1> Result{};
#define NEVERD_MIPS_REGISTER(Name, Index, O32, N64) Result[Index] = {O32, N64};
#include "neverd/lift/MIPS/MIPSRegisters.def"
  return Result;
}();

#define NEVERD_MIPS_REGISTER_TEXT(Name, Text) constexpr char Name[] = Text;
#include "neverd/lift/MIPS/MIPSRegisters.def"

} // namespace

const char *getMIPS32RegName(uint64_t Offset, uint16_t Size) {
  return Offset % mipsreg::SlotBytes == 0 && Offset <= mipsreg::PC && Size <= 4
             ? Names[Offset / mipsreg::SlotBytes].O32
             : UnknownRegister;
}

const char *getMIPS64RegName(uint64_t Offset, uint16_t Size) {
  return Offset % mipsreg::SlotBytes == 0 && Offset <= mipsreg::PC && Size <= 8
             ? Names[Offset / mipsreg::SlotBytes].N64
             : UnknownRegister;
}

} // namespace neverd
