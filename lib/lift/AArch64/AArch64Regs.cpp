//===- AArch64Regs.cpp - AArch64 register mapping ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// AArch64 register offset-to-name mapping tables.
///
//===----------------------------------------------------------------------===//

#include "neverd/lift/AArch64Regs.h"

namespace neverd {

RegInfo mapCapstoneReg(aarch64_reg Reg) {
  // X registers (64-bit)
  if (Reg >= AARCH64_REG_X0 && Reg <= AARCH64_REG_X28)
    return {a64reg::X0 + static_cast<uint64_t>(Reg - AARCH64_REG_X0) * 8, 8};
  if (Reg == AARCH64_REG_X29 || Reg == AARCH64_REG_FP)
    return {a64reg::X29, 8};
  if (Reg == AARCH64_REG_X30 || Reg == AARCH64_REG_LR)
    return {a64reg::X30, 8};
  if (Reg == AARCH64_REG_SP)
    return {a64reg::SP, 8};
  if (Reg == AARCH64_REG_WSP)
    return {a64reg::SP, 4};

  if (Reg >= AARCH64_REG_W0 && Reg <= AARCH64_REG_W30)
    return {a64reg::X0 + static_cast<uint64_t>(Reg - AARCH64_REG_W0) * 8, 4};

  if (Reg == AARCH64_REG_XZR)
    return {a64reg::XZR, 8};
  if (Reg == AARCH64_REG_WZR)
    return {a64reg::XZR, 4};

  if (Reg == AARCH64_REG_NZCV)
    return {a64reg::NFLAG, 4};

  // Q registers (128-bit SIMD)
  if (Reg >= AARCH64_REG_Q0 && Reg <= AARCH64_REG_Q31)
    return {a64reg::V(Reg - AARCH64_REG_Q0), 16};
  // D registers (64-bit FP/SIMD low half of V)
  if (Reg >= AARCH64_REG_D0 && Reg <= AARCH64_REG_D31)
    return {a64reg::V(Reg - AARCH64_REG_D0), 8};
  // S registers (32-bit FP low quarter of V)
  if (Reg >= AARCH64_REG_S0 && Reg <= AARCH64_REG_S31)
    return {a64reg::V(Reg - AARCH64_REG_S0), 4};
  // H registers (16-bit FP half)
  if (Reg >= AARCH64_REG_H0 && Reg <= AARCH64_REG_H31)
    return {a64reg::V(Reg - AARCH64_REG_H0), 2};
  // B registers (8-bit byte lane)
  if (Reg >= AARCH64_REG_B0 && Reg <= AARCH64_REG_B31)
    return {a64reg::V(Reg - AARCH64_REG_B0), 1};

  // SVE vector and predicate state.  The fixed backing widths are the
  // architectural maxima; LLVM emission converts them to scalable values for
  // operations whose active width depends on the runtime VL.
  if (Reg >= AARCH64_REG_Z0 && Reg <= AARCH64_REG_Z31)
    return {a64reg::Z(Reg - AARCH64_REG_Z0), a64reg::ZSize};
  if (Reg >= AARCH64_REG_P0 && Reg <= AARCH64_REG_P15)
    return {a64reg::P(Reg - AARCH64_REG_P0), a64reg::PSize};
  if (Reg == AARCH64_REG_FFR)
    return {a64reg::FFR, a64reg::PSize};
  return {0xFFFF, 0};
}

const char *getAArch64RegName(uint64_t Offset, uint16_t Size) {
  if (Offset == a64reg::SP)
    return Size == 8 ? "SP" : "WSP";
  if (Offset == a64reg::X29)
    return Size == 8 ? "X29" : "W29";
  if (Offset == a64reg::X30)
    return Size == 8 ? "LR" : "W30";
  if (Offset == a64reg::PC)
    return "PC";
  if (Offset == a64reg::XZR)
    return Size == 8 ? "XZR" : "WZR";

  if (Offset == a64reg::NFLAG)
    return "N";
  if (Offset == a64reg::ZFLAG)
    return "Z";
  if (Offset == a64reg::CFLAG)
    return "C";
  if (Offset == a64reg::VFLAG)
    return "V";

  if (Offset >= a64reg::X0 && Offset <= a64reg::X28 + 8) {
    static thread_local char Buf[8];
    int Idx = static_cast<int>((Offset - a64reg::X0) / 8);
    if (Size == 8)
      snprintf(Buf, sizeof(Buf), "X%d", Idx);
    else
      snprintf(Buf, sizeof(Buf), "W%d", Idx);
    return Buf;
  }

  if (Offset >= a64reg::V0 && Offset < a64reg::V0 + 32 * 16) {
    static thread_local char Buf[8];
    int Idx = static_cast<int>((Offset - a64reg::V0) / 16);
    if (Size == 16)
      snprintf(Buf, sizeof(Buf), "Q%d", Idx);
    else if (Size == 8)
      snprintf(Buf, sizeof(Buf), "D%d", Idx);
    else if (Size == 4)
      snprintf(Buf, sizeof(Buf), "S%d", Idx);
    else if (Size == 2)
      snprintf(Buf, sizeof(Buf), "H%d", Idx);
    else
      snprintf(Buf, sizeof(Buf), "B%d", Idx);
    return Buf;
  }

  if (Offset >= a64reg::Z0 && Offset < a64reg::Z0 + 32 * a64reg::ZSize &&
      (Offset - a64reg::Z0) % a64reg::ZSize == 0) {
    static thread_local char Buf[8];
    int Idx = static_cast<int>((Offset - a64reg::Z0) / a64reg::ZSize);
    snprintf(Buf, sizeof(Buf), "Z%d", Idx);
    return Buf;
  }

  if (Offset >= a64reg::P0 && Offset < a64reg::P0 + 16 * a64reg::PSize &&
      (Offset - a64reg::P0) % a64reg::PSize == 0) {
    static thread_local char Buf[8];
    int Idx = static_cast<int>((Offset - a64reg::P0) / a64reg::PSize);
    snprintf(Buf, sizeof(Buf), "P%d", Idx);
    return Buf;
  }

  if (Offset == a64reg::FFR)
    return "FFR";

  return "?";
}

} // namespace neverd
