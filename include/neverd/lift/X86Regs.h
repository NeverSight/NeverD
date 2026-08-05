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

/// Vector registers.  Each slot is 32 bytes so a full YMM (ymm_n = the 256-bit
/// AVX register) fits contiguously; the XMM_n offset is the low 16 bytes of
/// YMM_n, exactly the architectural xmm/ymm overlap.  The 32-byte stride keeps
/// adjacent vector registers from aliasing (the 16-byte stride used before had
/// no room for the YMM high lane).  mapCapstoneReg maps both X86_REG_XMM_n
/// (size 16) and X86_REG_YMM_n (size 32) to these bases.
constexpr uint64_t XMM0 = 256;
constexpr uint64_t XMM1 = 288;
constexpr uint64_t XMM2 = 320;
constexpr uint64_t XMM3 = 352;
constexpr uint64_t XMM4 = 384;
constexpr uint64_t XMM5 = 416;
constexpr uint64_t XMM6 = 448;
constexpr uint64_t XMM7 = 480;
constexpr uint64_t XMM8 = 512;
constexpr uint64_t XMM9 = 544;
constexpr uint64_t XMM10 = 576;
constexpr uint64_t XMM11 = 608;
constexpr uint64_t XMM12 = 640;
constexpr uint64_t XMM13 = 672;
constexpr uint64_t XMM14 = 704;
constexpr uint64_t XMM15 = 736;

/// x87 stack registers ST0..ST7, modeled at the architectural 80-bit extended
/// precision (`x86_fp80`).  Each slot is laid out 16 bytes apart so the 10-byte
/// value never overlaps the next register in the sub-register aliasing model
/// (the 16-byte stride matches the x86_fp80 ABI storage size); the value itself
/// is 10 bytes (`FPURegSize`).
constexpr uint64_t ST0 = 768;
constexpr uint64_t ST1 = 784;
constexpr uint64_t ST2 = 800;
constexpr uint64_t ST3 = 816;
constexpr uint64_t ST4 = 832;
constexpr uint64_t ST5 = 848;
constexpr uint64_t ST6 = 864;
constexpr uint64_t ST7 = 880;
constexpr uint64_t FPU_SW = 896; // status word (16 bits)
constexpr uint64_t FPU_CW = 898; // control word (16 bits)

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
