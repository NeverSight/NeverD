//===- ARMRegs.h - ARM32 register mapping -----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ARM32 register offset-to-name mapping for the lifter.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIFT_ARM_ARMREGS_H
#define NEVERD_LIFT_ARM_ARMREGS_H

#include "neverd/lift/LiftCommon.h"

#include <capstone/capstone.h>

namespace neverd {

namespace armreg {
/// General purpose registers R0-R15, 4-byte slots
constexpr uint64_t R0 = 0;
constexpr uint64_t R1 = 4;
constexpr uint64_t R2 = 8;
constexpr uint64_t R3 = 12;
constexpr uint64_t R4 = 16;
constexpr uint64_t R5 = 20;
constexpr uint64_t R6 = 24;
constexpr uint64_t R7 = 28;
constexpr uint64_t R8 = 32;
constexpr uint64_t R9 = 36;
constexpr uint64_t R10 = 40;
constexpr uint64_t R11 = 44; // FP
constexpr uint64_t R12 = 48; // IP (scratch)
constexpr uint64_t SP = 52;  // R13
constexpr uint64_t LR = 56;  // R14
constexpr uint64_t PC = 60;  // R15

/// VFP/NEON D registers (64-bit), D0-D15 at offset 0x100, 8-byte each
/// S registers are the low 32 bits of even-numbered D registers:
///   S0 = low32(D0), S1 = high32(D0), S2 = low32(D1), ...
constexpr uint64_t D0 = 0x100;
constexpr uint64_t D(unsigned N) { return D0 + N * 8; }

/// CPSR NZCV bit positions in the word read/written by MRS/MSR APSR.
constexpr unsigned CpsrNBit = 31;
constexpr unsigned CpsrZBit = 30;
constexpr unsigned CpsrCBit = 29;
constexpr unsigned CpsrVBit = 28;
/// GE[3:0], bits 19:16 of that word.
constexpr unsigned CpsrGEBit = 16;

/// CPSR flags (1-byte each, high offset to avoid collision)
constexpr uint64_t NFLAG = 0x200;
constexpr uint64_t ZFLAG = 0x201;
constexpr uint64_t CFLAG = 0x202;
constexpr uint64_t VFLAG = 0x203;

/// FPSCR flags written by VCMP; copied to the CPSR flags by VMRS APSR_nzcv.
/// Kept separate so an intervening second VCMP does not clobber the CPSR flags
/// a pending conditional instruction still needs
/// (vcmp;vmrs;vcmp;cond;vmrs;cond).
constexpr uint64_t FP_NFLAG = 0x210;
constexpr uint64_t FP_ZFLAG = 0x211;
constexpr uint64_t FP_CFLAG = 0x212;
constexpr uint64_t FP_VFLAG = 0x213;

/// APSR.GE[3:0] — the four per-lane "greater-or-equal" flags set by the GE-
/// setting parallel SIMD add/sub (SADD8/UADD16/SSUB8/SASX/...) and read by SEL
/// to pick each result byte from the first (GE==1) or second source.  One
/// 1-byte flag per byte lane.
constexpr uint64_t GE0FLAG = 0x214;
constexpr uint64_t GE1FLAG = 0x215;
constexpr uint64_t GE2FLAG = 0x216;
constexpr uint64_t GE3FLAG = 0x217;
constexpr uint64_t GEFLAG(unsigned N) { return GE0FLAG + N; }

/// A modelled flag and its bit position in the APSR word an MRS reads and an
/// MSR writes (ARMApsrFlags.def).
struct ApsrFlagBit {
  uint64_t Flag;
  unsigned Bit;
};
inline constexpr ApsrFlagBit ApsrFlagBits[] = {
#define NEVERD_ARM_APSR_BIT(Flag, Bit) {Flag, Bit},
#include "neverd/lift/ARM/ARMApsrFlags.def"
};

/// Upper bound (exclusive) of the combined GPR+flag offset range.
constexpr uint64_t RegSpaceEnd = GE3FLAG + 1;

/// The value an instruction at \p Addr reads from PC: its address plus 8 in
/// A32 and plus 4 in T32, also for a 32-bit T32 instruction that begins at
/// the second halfword of a word.
inline uint32_t pcReadValue(va_t Addr, InstructionMode Mode) {
  return static_cast<uint32_t>(Addr) +
         (Mode == InstructionMode::Thumb ? 4u : 8u);
}

/// The base a PC-relative literal access (LDR literal, ADR, VLDR, PLD) adds
/// its offset to: the PC value aligned down to a word, which differs from it
/// only in T32.
inline uint32_t literalBase(va_t Addr, InstructionMode Mode) {
  return pcReadValue(Addr, Mode) & ~uint32_t{3};
}
} // namespace armreg

RegInfo mapCapstoneReg(arm_reg Reg);
const char *getARMRegName(uint64_t Offset, uint16_t Size);

} // namespace neverd

#endif // NEVERD_LIFT_ARM_ARMREGS_H
