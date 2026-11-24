//===- RISCVCompressed.cpp - Integer RVC expansion --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// C 2.0 integer quadrants. Reserved and floating encodings have no integer
/// expansion.
///
//===----------------------------------------------------------------------===//

#include "RISCVCompressed.h"

#include "llvm/Support/MathExtras.h"

namespace neverd::riscv {
namespace {
uint32_t iType(unsigned Op, unsigned Rd, unsigned Fun, unsigned Rs,
               unsigned Imm) {
  return Op | Rd << 7 | Fun << 12 | Rs << 15 | (Imm & 0xfff) << 20;
}
uint32_t store(unsigned Fun, unsigned Base, unsigned Rs, unsigned Off) {
  return 0x23 | (Off & 31) << 7 | Fun << 12 | Base << 15 | Rs << 20 |
         (Off >> 5) << 25;
}
uint32_t regOp(unsigned Op, unsigned D, unsigned Fun, unsigned A, unsigned B,
               bool Sub = false) {
  return Op | D << 7 | Fun << 12 | A << 15 | B << 20 | uint32_t(Sub) << 30;
}
} // namespace

std::optional<uint32_t> expandCompressed(uint16_t H, bool RV64) {
  const unsigned Q = H & 3, F = H >> 13;
  const unsigned Rd = H >> 7 & 31, Rs = H >> 2 & 31;
  const unsigned R1 = 8 + (Rd & 7), R2 = 8 + (Rs & 7);
  const unsigned High = H >> 12 & 1;
  const unsigned UI = Rs | High << 5;
  const uint32_t SI = llvm::SignExtend32<6>(UI);
  if (Q == 0) {
    if (F == 0) { // ADDI4SPN
      unsigned N = (H >> 7 & 15) << 6 | (H >> 11 & 3) << 4 | (H >> 5 & 1) << 3 |
                   (H >> 6 & 1) << 2;
      if (N)
        return iType(0x13, R2, 0, 2, N);
    }
    const bool Wide = F == 3 || F == 7;
    if (F == 2 || F == 6 || (Wide && RV64)) {
      unsigned Off =
          (H >> 10 & 7) << 3 |
          (Wide ? (H >> 5 & 3) << 6 : (H >> 5 & 1) << 6 | (H >> 6 & 1) << 2);
      return F < 4 ? iType(0x03, R2, Wide ? 3 : 2, R1, Off)
                   : store(Wide ? 3 : 2, R1, R2, Off);
    }
  } else if (Q == 1) {
    if (F == 0 || F == 2)
      return iType(0x13, Rd, 0, F == 0 ? Rd : 0, SI); // ADDI/LI + hints
    if (F == 1 && RV64) {
      if (Rd)
        return iType(0x1b, Rd, 0, Rd, SI); // ADDIW
      return std::nullopt;
    }
    if (F == 3) {
      if (!SI)
        return std::nullopt;
      if (Rd != 2)
        return 0x37u | Rd << 7 | SI << 12;
      const unsigned N = High << 9 | (H >> 6 & 1) << 4 | (H >> 5 & 1) << 6 |
                         (H >> 3 & 3) << 7 | (H >> 2 & 1) << 5;
      return iType(0x13, 2, 0, 2, llvm::SignExtend32<10>(N));
    }
    if (F == 4) {
      unsigned Kind = H >> 10 & 3;
      if (Kind < 2) {
        if (High && !RV64)
          return std::nullopt;
        return iType(0x13, R1, 5, R1, UI | Kind << 10); // SRLI/SRAI
      }
      if (Kind == 2)
        return iType(0x13, R1, 7, R1, SI);
      unsigned Fn = H >> 5 & 3;
      if (High) {
        if (!RV64 || Fn > 1)
          return std::nullopt;
        return regOp(0x3b, R1, 0, R1, R2, Fn == 0); // SUBW/ADDW
      }
      constexpr unsigned ALU[] = {0, 4, 6, 7};
      return regOp(0x33, R1, ALU[Fn], R1, R2, Fn == 0);
    }
    if (F == 1 || F == 5) { // JAL (RV32) / J
      unsigned J = High << 11 | (H >> 11 & 1) << 4 | (H >> 9 & 3) << 8 |
                   (H >> 8 & 1) << 10 | (H >> 7 & 1) << 6 | (H >> 6 & 1) << 7 |
                   (H >> 3 & 7) << 1 | (H >> 2 & 1) << 5;
      J = llvm::SignExtend32<12>(J);
      return 0x6fu | (F == 1 ? 1u << 7 : 0) | (J & 0xff000) | (J & 0x800) << 9 |
             (J & 0x7fe) << 20 | (J & 0x100000) << 11;
    }
    if (F == 6 || F == 7) { // BEQZ/BNEZ
      unsigned B = High << 8 | (H >> 10 & 3) << 3 | (H >> 5 & 3) << 6 |
                   (H >> 3 & 3) << 1 | (H >> 2 & 1) << 5;
      B = llvm::SignExtend32<9>(B);
      return 0x63u | (F - 6) << 12 | R1 << 15 | (B & 0x1e) << 7 |
             (B & 0x7e0) << 20 | (B & 0x800) >> 4 | (B & 0x1000) << 19;
    }
  } else if (Q == 2) {
    if (F == 0 && (!High || RV64))
      return iType(0x13, Rd, 1, Rd, UI);
    if (F == 2 || (F == 3 && RV64)) {
      if (!Rd)
        return std::nullopt;
      unsigned Off =
          High << 5 | (F == 2 ? (H >> 4 & 7) << 2 | (H >> 2 & 3) << 6
                              : (H >> 5 & 3) << 3 | (H >> 2 & 7) << 6);
      return iType(3, Rd, F, 2, Off);
    }
    if (F == 4) {
      if (Rs)
        return regOp(0x33, Rd, 0, High ? Rd : 0, Rs);
      if (Rd)
        return iType(0x67, High, 0, Rd, 0); // JALR/JR
      if (High)
        return 0x00100073; // EBREAK
    }
    if (F == 6 || (F == 7 && RV64)) {
      unsigned Off = F == 6 ? (H >> 9 & 15) << 2 | (H >> 7 & 3) << 6
                            : (H >> 10 & 7) << 3 | (H >> 7 & 7) << 6;
      return store(F - 4, 2, Rs, Off);
    }
  }
  return std::nullopt;
}
} // namespace neverd::riscv
