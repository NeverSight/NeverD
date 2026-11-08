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
  std::array<RegisterNames, mipsreg::DSPControl / mipsreg::SlotBytes + 1>
      Result{};
#define NEVERD_MIPS_REGISTER(Name, Index, O32, N64) Result[Index] = {O32, N64};
#include "neverd/lift/MIPS/MIPSRegisters.def"
  return Result;
}();

#define NEVERD_MIPS_REGISTER_TEXT(Name, Text) constexpr char Name[] = Text;
#include "neverd/lift/MIPS/MIPSRegisters.def"

struct VectorRegisterNames {
  const char *Scalar;
  const char *Vector;
};
constexpr auto VectorNames = [] {
  std::array<VectorRegisterNames, mipsreg::VectorCount> Result{};
#define NEVERD_MIPS_VECTOR_REGISTER(Index, ScalarName, VectorName)             \
  Result[Index] = {ScalarName, VectorName};
#include "neverd/lift/MIPS/MIPSVectorRegisters.def"
  return Result;
}();

const char *vectorName(uint64_t Offset, uint16_t Size) {
  const auto &Name =
      VectorNames[(Offset - mipsreg::VectorBase) / mipsreg::VectorBytes];
  return Size <= 8 ? Name.Scalar : Size == 16 ? Name.Vector : UnknownRegister;
}

} // namespace

const char *getMIPS32RegName(uint64_t Offset, uint16_t Size) {
  if (mipsreg::isVector(Offset))
    return vectorName(Offset, Size);
  return Offset % mipsreg::SlotBytes == 0 && Offset <= mipsreg::DSPControl &&
                 Size <= 4
             ? Names[Offset / mipsreg::SlotBytes].O32
             : UnknownRegister;
}

const char *getMIPS64RegName(uint64_t Offset, uint16_t Size) {
  if (mipsreg::isVector(Offset))
    return vectorName(Offset, Size);
  return Offset % mipsreg::SlotBytes == 0 &&
                 ((Offset <= mipsreg::PC && Size <= 8) ||
                  (Offset == mipsreg::DSPControl && Size <= 4))
             ? Names[Offset / mipsreg::SlotBytes].N64
             : UnknownRegister;
}

} // namespace neverd
