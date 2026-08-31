//===- X86Regs.cpp - x86/x64 register mapping ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// x86/x64 register offset-to-name mapping tables.
///
//===----------------------------------------------------------------------===//

#include "neverd/lift/X86Regs.h"

namespace neverd {

RegInfo mapCapstoneReg(x86_reg Reg) {
  // MMX names the low 64 bits of the corresponding modeled x87 data slot.
  // Keep the alias narrow so a read never erases the x87 value's high 16 bits.
  if (Reg >= X86_REG_MM0 && Reg <= X86_REG_MM7)
    return {x86reg::ST0 +
                static_cast<unsigned>(Reg - X86_REG_MM0) *
                    x86reg::FPURegStride,
            8};
  if (Reg >= X86_REG_XMM0 && Reg <= X86_REG_XMM31)
    return {x86reg::vectorReg(static_cast<unsigned>(Reg - X86_REG_XMM0)), 16};
  if (Reg >= X86_REG_YMM0 && Reg <= X86_REG_YMM31)
    return {x86reg::vectorReg(static_cast<unsigned>(Reg - X86_REG_YMM0)), 32};
  if (Reg >= X86_REG_ZMM0 && Reg <= X86_REG_ZMM31)
    return {x86reg::vectorReg(static_cast<unsigned>(Reg - X86_REG_ZMM0)), 64};
  if (Reg >= X86_REG_K0 && Reg <= X86_REG_K7)
    return {x86reg::opmaskReg(static_cast<unsigned>(Reg - X86_REG_K0)), 8};
  if (Reg >= X86_REG_R16 && Reg <= X86_REG_R31)
    return {
        x86reg::extendedGeneralReg(static_cast<unsigned>(Reg - X86_REG_R16)),
        8};
  if (Reg >= X86_REG_R16D && Reg <= X86_REG_R31D)
    return {
        x86reg::extendedGeneralReg(static_cast<unsigned>(Reg - X86_REG_R16D)),
        4};
  if (Reg >= X86_REG_R16W && Reg <= X86_REG_R31W)
    return {
        x86reg::extendedGeneralReg(static_cast<unsigned>(Reg - X86_REG_R16W)),
        2};
  if (Reg >= X86_REG_R16B && Reg <= X86_REG_R31B)
    return {
        x86reg::extendedGeneralReg(static_cast<unsigned>(Reg - X86_REG_R16B)),
        1};
  if (Reg >= X86_REG_TMM0 && Reg <= X86_REG_TMM7)
    return {x86reg::tileReg(static_cast<unsigned>(Reg - X86_REG_TMM0)),
            x86reg::TileRegStride};

  switch (Reg) {
  // 64-bit
  case X86_REG_RAX:
    return {x86reg::RAX, 8};
  case X86_REG_RCX:
    return {x86reg::RCX, 8};
  case X86_REG_RDX:
    return {x86reg::RDX, 8};
  case X86_REG_RBX:
    return {x86reg::RBX, 8};
  case X86_REG_RSP:
    return {x86reg::RSP, 8};
  case X86_REG_RBP:
    return {x86reg::RBP, 8};
  case X86_REG_RSI:
    return {x86reg::RSI, 8};
  case X86_REG_RDI:
    return {x86reg::RDI, 8};
  case X86_REG_R8:
    return {x86reg::R8, 8};
  case X86_REG_R9:
    return {x86reg::R9, 8};
  case X86_REG_R10:
    return {x86reg::R10, 8};
  case X86_REG_R11:
    return {x86reg::R11, 8};
  case X86_REG_R12:
    return {x86reg::R12, 8};
  case X86_REG_R13:
    return {x86reg::R13, 8};
  case X86_REG_R14:
    return {x86reg::R14, 8};
  case X86_REG_R15:
    return {x86reg::R15, 8};
  case X86_REG_RIP:
    return {x86reg::RIP, 8};

  // 32-bit (sub-registers of 64-bit)
  case X86_REG_EAX:
    return {x86reg::RAX, 4};
  case X86_REG_ECX:
    return {x86reg::RCX, 4};
  case X86_REG_EDX:
    return {x86reg::RDX, 4};
  case X86_REG_EBX:
    return {x86reg::RBX, 4};
  case X86_REG_ESP:
    return {x86reg::RSP, 4};
  case X86_REG_EBP:
    return {x86reg::RBP, 4};
  case X86_REG_ESI:
    return {x86reg::RSI, 4};
  case X86_REG_EDI:
    return {x86reg::RDI, 4};
  case X86_REG_R8D:
    return {x86reg::R8, 4};
  case X86_REG_R9D:
    return {x86reg::R9, 4};
  case X86_REG_R10D:
    return {x86reg::R10, 4};
  case X86_REG_R11D:
    return {x86reg::R11, 4};
  case X86_REG_R12D:
    return {x86reg::R12, 4};
  case X86_REG_R13D:
    return {x86reg::R13, 4};
  case X86_REG_R14D:
    return {x86reg::R14, 4};
  case X86_REG_R15D:
    return {x86reg::R15, 4};

  // 16-bit
  case X86_REG_AX:
    return {x86reg::RAX, 2};
  case X86_REG_CX:
    return {x86reg::RCX, 2};
  case X86_REG_DX:
    return {x86reg::RDX, 2};
  case X86_REG_BX:
    return {x86reg::RBX, 2};
  case X86_REG_SP:
    return {x86reg::RSP, 2};
  case X86_REG_BP:
    return {x86reg::RBP, 2};
  case X86_REG_SI:
    return {x86reg::RSI, 2};
  case X86_REG_DI:
    return {x86reg::RDI, 2};
  case X86_REG_R8W:
    return {x86reg::R8, 2};
  case X86_REG_R9W:
    return {x86reg::R9, 2};
  case X86_REG_R10W:
    return {x86reg::R10, 2};
  case X86_REG_R11W:
    return {x86reg::R11, 2};
  case X86_REG_R12W:
    return {x86reg::R12, 2};
  case X86_REG_R13W:
    return {x86reg::R13, 2};
  case X86_REG_R14W:
    return {x86reg::R14, 2};
  case X86_REG_R15W:
    return {x86reg::R15, 2};

  // 8-bit low
  case X86_REG_AL:
    return {x86reg::RAX, 1};
  case X86_REG_CL:
    return {x86reg::RCX, 1};
  case X86_REG_DL:
    return {x86reg::RDX, 1};
  case X86_REG_BL:
    return {x86reg::RBX, 1};
  case X86_REG_SPL:
    return {x86reg::RSP, 1};
  case X86_REG_BPL:
    return {x86reg::RBP, 1};
  case X86_REG_SIL:
    return {x86reg::RSI, 1};
  case X86_REG_DIL:
    return {x86reg::RDI, 1};
  case X86_REG_R8B:
    return {x86reg::R8, 1};
  case X86_REG_R9B:
    return {x86reg::R9, 1};
  case X86_REG_R10B:
    return {x86reg::R10, 1};
  case X86_REG_R11B:
    return {x86reg::R11, 1};
  case X86_REG_R12B:
    return {x86reg::R12, 1};
  case X86_REG_R13B:
    return {x86reg::R13, 1};
  case X86_REG_R14B:
    return {x86reg::R14, 1};
  case X86_REG_R15B:
    return {x86reg::R15, 1};

  // 8-bit high
  case X86_REG_AH:
    return {x86reg::RAX + 1, 1};
  case X86_REG_CH:
    return {x86reg::RCX + 1, 1};
  case X86_REG_DH:
    return {x86reg::RDX + 1, 1};
  case X86_REG_BH:
    return {x86reg::RBX + 1, 1};

  // XMM
  case X86_REG_XMM0:
    return {x86reg::XMM0, 16};
  case X86_REG_XMM1:
    return {x86reg::XMM1, 16};
  case X86_REG_XMM2:
    return {x86reg::XMM2, 16};
  case X86_REG_XMM3:
    return {x86reg::XMM3, 16};
  case X86_REG_XMM4:
    return {x86reg::XMM4, 16};
  case X86_REG_XMM5:
    return {x86reg::XMM5, 16};
  case X86_REG_XMM6:
    return {x86reg::XMM6, 16};
  case X86_REG_XMM7:
    return {x86reg::XMM7, 16};
  case X86_REG_XMM8:
    return {x86reg::XMM8, 16};
  case X86_REG_XMM9:
    return {x86reg::XMM9, 16};
  case X86_REG_XMM10:
    return {x86reg::XMM10, 16};
  case X86_REG_XMM11:
    return {x86reg::XMM11, 16};
  case X86_REG_XMM12:
    return {x86reg::XMM12, 16};
  case X86_REG_XMM13:
    return {x86reg::XMM13, 16};
  case X86_REG_XMM14:
    return {x86reg::XMM14, 16};
  case X86_REG_XMM15:
    return {x86reg::XMM15, 16};

  // YMM (256-bit) — same base as the matching XMM (xmm_n is the low 128 bits of
  // ymm_n); the 32-byte slot spacing in X86Regs.h keeps them from aliasing.
  case X86_REG_YMM0:
    return {x86reg::XMM0, 32};
  case X86_REG_YMM1:
    return {x86reg::XMM1, 32};
  case X86_REG_YMM2:
    return {x86reg::XMM2, 32};
  case X86_REG_YMM3:
    return {x86reg::XMM3, 32};
  case X86_REG_YMM4:
    return {x86reg::XMM4, 32};
  case X86_REG_YMM5:
    return {x86reg::XMM5, 32};
  case X86_REG_YMM6:
    return {x86reg::XMM6, 32};
  case X86_REG_YMM7:
    return {x86reg::XMM7, 32};
  case X86_REG_YMM8:
    return {x86reg::XMM8, 32};
  case X86_REG_YMM9:
    return {x86reg::XMM9, 32};
  case X86_REG_YMM10:
    return {x86reg::XMM10, 32};
  case X86_REG_YMM11:
    return {x86reg::XMM11, 32};
  case X86_REG_YMM12:
    return {x86reg::XMM12, 32};
  case X86_REG_YMM13:
    return {x86reg::XMM13, 32};
  case X86_REG_YMM14:
    return {x86reg::XMM14, 32};
  case X86_REG_YMM15:
    return {x86reg::XMM15, 32};

  case X86_REG_ST0:
    return {x86reg::ST0, x86reg::FPURegSize};
  case X86_REG_ST1:
    return {x86reg::ST1, x86reg::FPURegSize};
  case X86_REG_ST2:
    return {x86reg::ST2, x86reg::FPURegSize};
  case X86_REG_ST3:
    return {x86reg::ST3, x86reg::FPURegSize};
  case X86_REG_ST4:
    return {x86reg::ST4, x86reg::FPURegSize};
  case X86_REG_ST5:
    return {x86reg::ST5, x86reg::FPURegSize};
  case X86_REG_ST6:
    return {x86reg::ST6, x86reg::FPURegSize};
  case X86_REG_ST7:
    return {x86reg::ST7, x86reg::FPURegSize};

  default:
    return {0xFFFF, 0};
  }
}

const char *getX86RegName(uint64_t Offset, uint16_t Size) {
#define X86_VECTOR_REGISTER_NAMES(Prefix)                                      \
  Prefix "0", Prefix "1", Prefix "2", Prefix "3", Prefix "4", Prefix "5",      \
      Prefix "6", Prefix "7", Prefix "8", Prefix "9", Prefix "10",             \
      Prefix "11", Prefix "12", Prefix "13", Prefix "14", Prefix "15",         \
      Prefix "16", Prefix "17", Prefix "18", Prefix "19", Prefix "20",         \
      Prefix "21", Prefix "22", Prefix "23", Prefix "24", Prefix "25",         \
      Prefix "26", Prefix "27", Prefix "28", Prefix "29", Prefix "30",         \
      Prefix "31"
  static constexpr const char *XmmNames[] = {X86_VECTOR_REGISTER_NAMES("XMM")};
  static constexpr const char *YmmNames[] = {X86_VECTOR_REGISTER_NAMES("YMM")};
  static constexpr const char *ZmmNames[] = {X86_VECTOR_REGISTER_NAMES("ZMM")};
#undef X86_VECTOR_REGISTER_NAMES
  static constexpr const char *KNames[] = {"K0", "K1", "K2", "K3",
                                           "K4", "K5", "K6", "K7"};
#define X86_EXTENDED_GPR_NAMES(Suffix)                                         \
  "R16" Suffix, "R17" Suffix, "R18" Suffix, "R19" Suffix, "R20" Suffix,        \
      "R21" Suffix, "R22" Suffix, "R23" Suffix, "R24" Suffix, "R25" Suffix,    \
      "R26" Suffix, "R27" Suffix, "R28" Suffix, "R29" Suffix, "R30" Suffix,    \
      "R31" Suffix
  static constexpr const char *Extended64Names[] = {X86_EXTENDED_GPR_NAMES("")};
  static constexpr const char *Extended32Names[] = {
      X86_EXTENDED_GPR_NAMES("D")};
  static constexpr const char *Extended16Names[] = {
      X86_EXTENDED_GPR_NAMES("W")};
  static constexpr const char *Extended8Names[] = {X86_EXTENDED_GPR_NAMES("B")};
#undef X86_EXTENDED_GPR_NAMES
  static constexpr const char *TileNames[] = {"TMM0", "TMM1", "TMM2", "TMM3",
                                              "TMM4", "TMM5", "TMM6", "TMM7"};
  static constexpr const char *MmxNames[] = {"MM0", "MM1", "MM2", "MM3",
                                             "MM4", "MM5", "MM6", "MM7"};

  if (Offset >= x86reg::VectorBase &&
      Offset < x86reg::vectorReg(x86reg::VectorRegCount) &&
      (Offset - x86reg::VectorBase) % x86reg::VectorRegStride == 0) {
    const unsigned Index = static_cast<unsigned>((Offset - x86reg::VectorBase) /
                                                 x86reg::VectorRegStride);
    if (Size == 16)
      return XmmNames[Index];
    if (Size == 32)
      return YmmNames[Index];
    if (Size == 64)
      return ZmmNames[Index];
  }
  if (Offset >= x86reg::OpmaskBase &&
      Offset < x86reg::opmaskReg(x86reg::OpmaskRegCount) &&
      (Offset - x86reg::OpmaskBase) % x86reg::OpmaskRegStride == 0 &&
      (Size == 1 || Size == 2 || Size == 4 || Size == 8))
    return KNames[(Offset - x86reg::OpmaskBase) / x86reg::OpmaskRegStride];
  if (Offset >= x86reg::ExtendedGPRBase &&
      Offset < x86reg::extendedGeneralReg(x86reg::ExtendedGPRCount) &&
      (Offset - x86reg::ExtendedGPRBase) % x86reg::ExtendedGPRStride == 0) {
    const unsigned Index = static_cast<unsigned>(
        (Offset - x86reg::ExtendedGPRBase) / x86reg::ExtendedGPRStride);
    if (Size == 8)
      return Extended64Names[Index];
    if (Size == 4)
      return Extended32Names[Index];
    if (Size == 2)
      return Extended16Names[Index];
    if (Size == 1)
      return Extended8Names[Index];
  }
  if (Offset >= x86reg::TileBase &&
      Offset < x86reg::tileReg(x86reg::TileRegCount) &&
      (Offset - x86reg::TileBase) % x86reg::TileRegStride == 0 &&
      Size == x86reg::TileRegStride)
    return TileNames[(Offset - x86reg::TileBase) / x86reg::TileRegStride];
  if (Offset == x86reg::TileConfig && Size == x86reg::TileConfigSize)
    return "TILECFG";
  if (Offset >= x86reg::ST0 && Offset <= x86reg::ST7 &&
      (Offset - x86reg::ST0) % x86reg::FPURegStride == 0 && Size == 8)
    return MmxNames[(Offset - x86reg::ST0) / x86reg::FPURegStride];

  if (Size == 8) {
    switch (Offset) {
    case x86reg::RAX:
      return "RAX";
    case x86reg::RCX:
      return "RCX";
    case x86reg::RDX:
      return "RDX";
    case x86reg::RBX:
      return "RBX";
    case x86reg::RSP:
      return "RSP";
    case x86reg::RBP:
      return "RBP";
    case x86reg::RSI:
      return "RSI";
    case x86reg::RDI:
      return "RDI";
    case x86reg::R8:
      return "R8";
    case x86reg::R9:
      return "R9";
    case x86reg::R10:
      return "R10";
    case x86reg::R11:
      return "R11";
    case x86reg::R12:
      return "R12";
    case x86reg::R13:
      return "R13";
    case x86reg::R14:
      return "R14";
    case x86reg::R15:
      return "R15";
    case x86reg::RIP:
      return "RIP";
    }
  }
  if (Size == 4) {
    switch (Offset) {
    case x86reg::RAX:
      return "EAX";
    case x86reg::RCX:
      return "ECX";
    case x86reg::RDX:
      return "EDX";
    case x86reg::RBX:
      return "EBX";
    case x86reg::RSP:
      return "ESP";
    case x86reg::RBP:
      return "EBP";
    case x86reg::RSI:
      return "ESI";
    case x86reg::RDI:
      return "EDI";
    case x86reg::R8:
      return "R8D";
    case x86reg::R9:
      return "R9D";
    case x86reg::R10:
      return "R10D";
    case x86reg::R11:
      return "R11D";
    case x86reg::R12:
      return "R12D";
    case x86reg::R13:
      return "R13D";
    case x86reg::R14:
      return "R14D";
    case x86reg::R15:
      return "R15D";
    }
  }
  if (Size == 2) {
    switch (Offset) {
    case x86reg::RAX:
      return "AX";
    case x86reg::RCX:
      return "CX";
    case x86reg::RDX:
      return "DX";
    case x86reg::RBX:
      return "BX";
    case x86reg::RSP:
      return "SP";
    case x86reg::RBP:
      return "BP";
    case x86reg::RSI:
      return "SI";
    case x86reg::RDI:
      return "DI";
    case x86reg::R8:
      return "R8W";
    case x86reg::R9:
      return "R9W";
    case x86reg::R10:
      return "R10W";
    case x86reg::R11:
      return "R11W";
    case x86reg::R12:
      return "R12W";
    case x86reg::R13:
      return "R13W";
    case x86reg::R14:
      return "R14W";
    case x86reg::R15:
      return "R15W";
    }
  }
  if (Size == 1) {
    switch (Offset) {
    case x86reg::RAX:
      return "AL";
    case x86reg::RCX:
      return "CL";
    case x86reg::RDX:
      return "DL";
    case x86reg::RBX:
      return "BL";
    case x86reg::RSI:
      return "SIL";
    case x86reg::RDI:
      return "DIL";
    case x86reg::R8:
      return "R8B";
    case x86reg::R9:
      return "R9B";
    case x86reg::CF:
      return "CF";
    case x86reg::PF:
      return "PF";
    case x86reg::AF:
      return "AF";
    case x86reg::ZF:
      return "ZF";
    case x86reg::SF:
      return "SF";
    case x86reg::OF:
      return "OF";
    case x86reg::DF:
      return "DF";
    }
  }
  if (Size == x86reg::FPURegSize) {
    switch (Offset) {
    case x86reg::ST0:
      return "ST0";
    case x86reg::ST1:
      return "ST1";
    case x86reg::ST2:
      return "ST2";
    case x86reg::ST3:
      return "ST3";
    case x86reg::ST4:
      return "ST4";
    case x86reg::ST5:
      return "ST5";
    case x86reg::ST6:
      return "ST6";
    case x86reg::ST7:
      return "ST7";
    }
  }
  if (Size == 2) {
    if (Offset == x86reg::FPU_SW)
      return "FPU_SW";
    if (Offset == x86reg::FPU_CW)
      return "FPU_CW";
  }
  return "?";
}

} // namespace neverd
