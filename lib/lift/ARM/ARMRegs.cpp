//===- ARMRegs.cpp - ARM32 register mapping ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ARM32 register offset-to-name mapping tables.
///
//===----------------------------------------------------------------------===//

#include "neverd/lift/ARMRegs.h"

namespace neverd {

RegInfo mapCapstoneReg(arm_reg Reg) {
  switch (Reg) {
  case ARM_REG_R0:
    return {armreg::R0, 4};
  case ARM_REG_R1:
    return {armreg::R1, 4};
  case ARM_REG_R2:
    return {armreg::R2, 4};
  case ARM_REG_R3:
    return {armreg::R3, 4};
  case ARM_REG_R4:
    return {armreg::R4, 4};
  case ARM_REG_R5:
    return {armreg::R5, 4};
  case ARM_REG_R6:
    return {armreg::R6, 4};
  case ARM_REG_R7:
    return {armreg::R7, 4};
  case ARM_REG_R8:
    return {armreg::R8, 4};
  case ARM_REG_R9:
    return {armreg::R9, 4};
  case ARM_REG_R10:
    return {armreg::R10, 4};
  case ARM_REG_R11:
    return {armreg::R11, 4};
  case ARM_REG_R12:
    return {armreg::R12, 4};
  case ARM_REG_SP:
    return {armreg::SP, 4};
  case ARM_REG_LR:
    return {armreg::LR, 4};
  case ARM_REG_PC:
    return {armreg::PC, 4};
  case ARM_REG_CPSR:
    return {armreg::NFLAG, 4};
  default:
    break;
  }

  // VFP Q registers (128-bit) — Q0-Q15 map to D(2n):D(2n+1)
  if (Reg >= ARM_REG_Q0 && Reg <= ARM_REG_Q15)
    return {armreg::D((Reg - ARM_REG_Q0) * 2), 16};
  // VFP D registers (64-bit) — D0-D31
  if (Reg >= ARM_REG_D0 && Reg <= ARM_REG_D31)
    return {armreg::D(Reg - ARM_REG_D0), 8};
  // VFP S registers (32-bit) — S0/S1 share D0, S2/S3 share D1, etc.
  if (Reg >= ARM_REG_S0 && Reg <= ARM_REG_S31) {
    unsigned Idx = Reg - ARM_REG_S0;
    unsigned DIdx = Idx / 2;
    unsigned Offset = (Idx & 1) ? 4 : 0;
    return {armreg::D(DIdx) + Offset, 4};
  }

  return {0xFFFF, 0};
}

const char *getARMRegName(uint64_t Offset, uint16_t Size) {
  if (Offset == armreg::SP)
    return "SP";
  if (Offset == armreg::LR)
    return "LR";
  if (Offset == armreg::PC)
    return "PC";
  if (Offset == armreg::R11)
    return "R11";
  if (Offset == armreg::R12)
    return "R12";

  if (Offset == armreg::NFLAG)
    return "N";
  if (Offset == armreg::ZFLAG)
    return "Z";
  if (Offset == armreg::CFLAG)
    return "C";
  if (Offset == armreg::VFLAG)
    return "V";

  if (Offset >= armreg::R0 && Offset <= armreg::R10) {
    static thread_local char Buf[8];
    int Idx = static_cast<int>(Offset / 4);
    snprintf(Buf, sizeof(Buf), "R%d", Idx);
    return Buf;
  }

  if (Offset >= armreg::D0 && Offset < armreg::D0 + 32 * 8) {
    static thread_local char Buf[8];
    int Idx = static_cast<int>((Offset - armreg::D0) / 8);
    if (Size == 8)
      snprintf(Buf, sizeof(Buf), "D%d", Idx);
    else
      snprintf(Buf, sizeof(Buf), "S%d",
               Idx * 2 + ((Offset - armreg::D(Idx)) > 0 ? 1 : 0));
    return Buf;
  }

  return "?";
}

} // namespace neverd
