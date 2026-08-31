//===- X86Regs.h - x86/x64 register mapping ---------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// x86/x64 register offset-to-name mapping for the lifter.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIFT_X86REGS_H
#define NEVERD_LIFT_X86REGS_H

#include "neverd/lift/LiftCommon.h"

#include <capstone/capstone.h>

namespace neverd {

namespace x86reg {
constexpr uint64_t RAX = 0;
constexpr uint64_t RCX = 8;
constexpr uint64_t RDX = 16;
constexpr uint64_t RBX = 24;
constexpr uint64_t RSP = 32;
constexpr uint64_t RBP = 40;
constexpr uint64_t RSI = 48;
constexpr uint64_t RDI = 56;
constexpr uint64_t R8 = 64;
constexpr uint64_t R9 = 72;
constexpr uint64_t R10 = 80;
constexpr uint64_t R11 = 88;
constexpr uint64_t R12 = 96;
constexpr uint64_t R13 = 104;
constexpr uint64_t R14 = 112;
constexpr uint64_t R15 = 120;
constexpr uint64_t RIP = 128;

/// FLAGS register
constexpr uint64_t CF = 200;
constexpr uint64_t PF = 201;
constexpr uint64_t AF = 202;
constexpr uint64_t ZF = 203;
constexpr uint64_t SF = 204;
constexpr uint64_t OF = 205;
constexpr uint64_t DF = 206;

/// Vector registers.  Every slot owns the complete architectural ZMM register;
/// XMMn and YMMn are its low 16 and 32 bytes.  EVEX exposes 32 registers in
/// 64-bit mode, so adjacent slots must be 64 bytes apart even when an
/// instruction names only a narrower alias.
constexpr uint64_t VectorBase = 256;
constexpr uint64_t VectorRegStride = 64;
constexpr unsigned VectorRegCount = 32;

inline constexpr uint64_t vectorReg(unsigned Index) {
  return VectorBase + static_cast<uint64_t>(Index) * VectorRegStride;
}

constexpr uint64_t XMM0 = vectorReg(0);
constexpr uint64_t XMM1 = vectorReg(1);
constexpr uint64_t XMM2 = vectorReg(2);
constexpr uint64_t XMM3 = vectorReg(3);
constexpr uint64_t XMM4 = vectorReg(4);
constexpr uint64_t XMM5 = vectorReg(5);
constexpr uint64_t XMM6 = vectorReg(6);
constexpr uint64_t XMM7 = vectorReg(7);
constexpr uint64_t XMM8 = vectorReg(8);
constexpr uint64_t XMM9 = vectorReg(9);
constexpr uint64_t XMM10 = vectorReg(10);
constexpr uint64_t XMM11 = vectorReg(11);
constexpr uint64_t XMM12 = vectorReg(12);
constexpr uint64_t XMM13 = vectorReg(13);
constexpr uint64_t XMM14 = vectorReg(14);
constexpr uint64_t XMM15 = vectorReg(15);
constexpr uint64_t XMM16 = vectorReg(16);
constexpr uint64_t XMM17 = vectorReg(17);
constexpr uint64_t XMM18 = vectorReg(18);
constexpr uint64_t XMM19 = vectorReg(19);
constexpr uint64_t XMM20 = vectorReg(20);
constexpr uint64_t XMM21 = vectorReg(21);
constexpr uint64_t XMM22 = vectorReg(22);
constexpr uint64_t XMM23 = vectorReg(23);
constexpr uint64_t XMM24 = vectorReg(24);
constexpr uint64_t XMM25 = vectorReg(25);
constexpr uint64_t XMM26 = vectorReg(26);
constexpr uint64_t XMM27 = vectorReg(27);
constexpr uint64_t XMM28 = vectorReg(28);
constexpr uint64_t XMM29 = vectorReg(29);
constexpr uint64_t XMM30 = vectorReg(30);
constexpr uint64_t XMM31 = vectorReg(31);

/// AVX-512 predicate registers are architecturally 64-bit containers.  The
/// B/W/D forms operate on a low sub-register and clear the unused high bits.
constexpr uint64_t OpmaskBase = vectorReg(VectorRegCount);
constexpr uint64_t OpmaskRegStride = 8;
constexpr unsigned OpmaskRegCount = 8;

inline constexpr uint64_t opmaskReg(unsigned Index) {
  return OpmaskBase + static_cast<uint64_t>(Index) * OpmaskRegStride;
}

constexpr uint64_t K0 = opmaskReg(0);
constexpr uint64_t K1 = opmaskReg(1);
constexpr uint64_t K2 = opmaskReg(2);
constexpr uint64_t K3 = opmaskReg(3);
constexpr uint64_t K4 = opmaskReg(4);
constexpr uint64_t K5 = opmaskReg(5);
constexpr uint64_t K6 = opmaskReg(6);
constexpr uint64_t K7 = opmaskReg(7);

/// x87 stack registers ST0..ST7, modeled at the architectural 80-bit extended
/// precision (`x86_fp80`).  Each slot is laid out 16 bytes apart so the 10-byte
/// value never overlaps the next register in the sub-register aliasing model
/// (the 16-byte stride matches the x86_fp80 ABI storage size); the value itself
/// is 10 bytes (`FPURegSize`).
constexpr uint64_t ST0 = OpmaskBase + OpmaskRegCount * OpmaskRegStride;
constexpr uint64_t ST1 = ST0 + 16;
constexpr uint64_t ST2 = ST0 + 32;
constexpr uint64_t ST3 = ST0 + 48;
constexpr uint64_t ST4 = ST0 + 64;
constexpr uint64_t ST5 = ST0 + 80;
constexpr uint64_t ST6 = ST0 + 96;
constexpr uint64_t ST7 = ST0 + 112;
constexpr uint64_t FPU_SW = ST0 + 128; // status word (16 bits)
constexpr uint64_t FPU_CW = ST0 + 130; // control word (16 bits)

/// APX extended general-purpose registers form a separate state bank.  Keeping
/// it non-contiguous with RAX-R15 preserves every existing architectural
/// offset (including RIP, flags, vectors, and x87) while still giving each
/// R16-R31 alias one stable 64-bit container.
constexpr uint64_t ExtendedGPRBase = (FPU_CW + 2 + 7) & ~UINT64_C(7);
constexpr uint64_t ExtendedGPRStride = 8;
constexpr unsigned ExtendedGPRCount = 16;

inline constexpr uint64_t extendedGeneralReg(unsigned Index) {
  return ExtendedGPRBase + static_cast<uint64_t>(Index) * ExtendedGPRStride;
}

constexpr uint64_t R16 = extendedGeneralReg(0);
constexpr uint64_t R17 = extendedGeneralReg(1);
constexpr uint64_t R18 = extendedGeneralReg(2);
constexpr uint64_t R19 = extendedGeneralReg(3);
constexpr uint64_t R20 = extendedGeneralReg(4);
constexpr uint64_t R21 = extendedGeneralReg(5);
constexpr uint64_t R22 = extendedGeneralReg(6);
constexpr uint64_t R23 = extendedGeneralReg(7);
constexpr uint64_t R24 = extendedGeneralReg(8);
constexpr uint64_t R25 = extendedGeneralReg(9);
constexpr uint64_t R26 = extendedGeneralReg(10);
constexpr uint64_t R27 = extendedGeneralReg(11);
constexpr uint64_t R28 = extendedGeneralReg(12);
constexpr uint64_t R29 = extendedGeneralReg(13);
constexpr uint64_t R30 = extendedGeneralReg(14);
constexpr uint64_t R31 = extendedGeneralReg(15);

/// Intel AMX palette 1 exposes eight independently configured tile registers.
/// Each physical container is 16 rows by 64 bytes (1 KiB); TILECFG determines
/// the active rectangle, but preserving the complete container keeps rows and
/// out-of-range bytes explicit for restartable loads and future palettes.
constexpr uint64_t TileBase = extendedGeneralReg(ExtendedGPRCount);
constexpr uint64_t TileRegStride = 1024;
constexpr unsigned TileRegCount = 8;

inline constexpr uint64_t tileReg(unsigned Index) {
  return TileBase + static_cast<uint64_t>(Index) * TileRegStride;
}

constexpr uint64_t TMM0 = tileReg(0);
constexpr uint64_t TMM1 = tileReg(1);
constexpr uint64_t TMM2 = tileReg(2);
constexpr uint64_t TMM3 = tileReg(3);
constexpr uint64_t TMM4 = tileReg(4);
constexpr uint64_t TMM5 = tileReg(5);
constexpr uint64_t TMM6 = tileReg(6);
constexpr uint64_t TMM7 = tileReg(7);

/// XTILECFG is a distinct 64-byte architectural state component.  Keeping the
/// exact memory image makes palette validation, restart progress, XSAVE-style
/// inspection, and STTILECFG round-trips observable without inventing a
/// decoder-visible general register.
constexpr uint64_t TileConfig = tileReg(TileRegCount);
constexpr uint16_t TileConfigSize = 64;

constexpr uint16_t FPURegSize = 10;   // 80-bit x87 extended-precision value
constexpr uint16_t FPURegStride = 16; // per-register slot spacing
constexpr int FPUStackDepth = 8;

/// x87 status-word condition-code bit positions (set by FCOM/FUCOM/FICOM/FTST,
/// read back by FNSTSW).  The fnstsw+sahf idiom maps C0->CF, C2->PF, C3->ZF.
constexpr uint16_t FPU_SW_C0_BIT = 8;
constexpr uint16_t FPU_SW_C1_BIT = 9;
constexpr uint16_t FPU_SW_C2_BIT = 10;
constexpr uint16_t FPU_SW_C3_BIT = 14;

/// x87 control-word rounding-control (RC) field — bits [11:10] select the FIST/
/// FISTP rounding mode: 00 nearest-even, 01 down, 10 up, 11 toward-zero (the
/// mode clang's `(int)x` cast installs via fnstcw/or 0xC00/fldcw before fistp).
constexpr uint16_t FPU_CW_RC_SHIFT = 10;
constexpr uint16_t FPU_CW_RC_MASK = 3;
constexpr uint16_t FPU_CW_RC_TRUNCATE = 3;

/// x87 constant-load values (FLD1/FLDPI/FLDL2E/FLDL2T/FLDLG2/FLDLN2) as
/// IEEE-754 double bit patterns.  The lifter widens them (FLOAT2FLOAT) into the
/// 80-bit register, so a constant stored straight back to 64-bit (`fstpl`)
/// round-trips exactly; the low-bit difference from the true 80-bit constant
/// only shows when such a constant is stored at full 80-bit width, which the
/// transcendental idioms that load them never do.
constexpr uint64_t FPU_CONST_1 = 0x3FF0000000000000ULL;   // 1.0
constexpr uint64_t FPU_CONST_PI = 0x400921FB54442D18ULL;  // pi
constexpr uint64_t FPU_CONST_L2E = 0x3FF71547652B82FEULL; // log2(e)
constexpr uint64_t FPU_CONST_L2T = 0x400A934F0979A371ULL; // log2(10)
constexpr uint64_t FPU_CONST_LG2 = 0x3FD34413509F79FFULL; // log10(2)
constexpr uint64_t FPU_CONST_LN2 = 0x3FE62E42FEFA39EFULL; // ln(2)

/// Upper bound (exclusive) of the GPR offset range, for zero-extension checks.
constexpr uint64_t GPRSpaceEnd = CF;

inline constexpr bool isGeneralRegOffset(uint64_t Offset) {
  const bool Legacy = Offset <= R15 && Offset % 8 == 0;
  const bool Extended = Offset >= ExtendedGPRBase &&
                        Offset < extendedGeneralReg(ExtendedGPRCount) &&
                        (Offset - ExtendedGPRBase) % ExtendedGPRStride == 0;
  return Legacy || Extended;
}

inline constexpr uint64_t stReg(int idx) {
  return ST0 + static_cast<uint64_t>(idx & 7) * FPURegStride;
}

/// Recover the x87 stack slot index (0..7) from a physical ST register offset.
inline constexpr int stRegIndex(uint64_t off) {
  return static_cast<int>((off - ST0) / FPURegStride);
}
} // namespace x86reg

RegInfo mapCapstoneReg(x86_reg Reg);
const char *getX86RegName(uint64_t Offset, uint16_t Size);

} // namespace neverd

#endif // NEVERD_LIFT_X86REGS_H
