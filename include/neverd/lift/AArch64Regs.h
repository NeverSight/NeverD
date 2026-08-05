//===- AArch64Regs.h - AArch64 register mapping -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// AArch64 register offset-to-name mapping for the lifter.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIFT_AARCH64REGS_H
#define NEVERD_LIFT_AARCH64REGS_H

#include "neverd/lift/LiftCommon.h"

#include <capstone/capstone.h>

namespace neverd {

namespace a64reg {
/// General purpose registers X0-X30 at offset 0, 8-byte each
constexpr uint64_t X0 = 0;
constexpr uint64_t X1 = 8;
constexpr uint64_t X2 = 16;
constexpr uint64_t X3 = 24;
constexpr uint64_t X4 = 32;
constexpr uint64_t X5 = 40;
constexpr uint64_t X6 = 48;
constexpr uint64_t X7 = 56;
constexpr uint64_t X8 = 64;
constexpr uint64_t X9 = 72;
constexpr uint64_t X10 = 80;
constexpr uint64_t X11 = 88;
constexpr uint64_t X12 = 96;
constexpr uint64_t X13 = 104;
constexpr uint64_t X14 = 112;
constexpr uint64_t X15 = 120;
constexpr uint64_t X16 = 128;
constexpr uint64_t X17 = 136;
constexpr uint64_t X18 = 144;
constexpr uint64_t X19 = 152;
constexpr uint64_t X20 = 160;
constexpr uint64_t X21 = 168;
constexpr uint64_t X22 = 176;
constexpr uint64_t X23 = 184;
constexpr uint64_t X24 = 192;
constexpr uint64_t X25 = 200;
constexpr uint64_t X26 = 208;
constexpr uint64_t X27 = 216;
constexpr uint64_t X28 = 224;
constexpr uint64_t X29 = 232; // FP
constexpr uint64_t X30 = 240; // LR
constexpr uint64_t SP = 248;
constexpr uint64_t PC = 256;
constexpr uint64_t XZR = 264; // zero register

/// SIMD/FP registers V0-V31 at offset 0x200, 16-byte each (128-bit)
/// D0-D31 = low 64 bits, S0-S31 = low 32 bits, H/B = low 16/8 bits
constexpr uint64_t V0 = 0x200;
constexpr uint64_t V(unsigned N) { return V0 + N * 16; }

/// NZCV flags
constexpr uint64_t NFLAG = 0x400;
constexpr uint64_t ZFLAG = 0x401;
constexpr uint64_t CFLAG = 0x402;
constexpr uint64_t VFLAG = 0x403;

/// NZCV system-register bit positions in the word read/written by MRS/MSR NZCV.
constexpr unsigned NzcvNBit = 31;
constexpr unsigned NzcvZBit = 30;
constexpr unsigned NzcvCBit = 29;
constexpr unsigned NzcvVBit = 28;
} // namespace a64reg

RegInfo mapCapstoneReg(aarch64_reg Reg);
const char *getAArch64RegName(uint64_t Offset, uint16_t Size);

} // namespace neverd

#endif // NEVERD_LIFT_AARCH64REGS_H
