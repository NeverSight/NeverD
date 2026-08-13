//===- AArch64_NativeDecodeParityFPTests.cpp - scalar floating point parity -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "AArch64_NativeDecodeParityTestsDetail.h"

namespace {

using namespace neverd;
using namespace neverd::a64_parity_test;

// FMOV scalar: register (Vd,Vn), general (GPR<->FP), and the 8-bit scalar
// immediate, over ptype (S/D/H + the UNALLOCATED 10), the rmode/opcode selector
// (top-half rmode!=0 declined) and every imm8 for the immediate form.
TEST_F(A64NativeParity, FmovScalarSweep) {
  const va_t A = 0xF00000;
  // Register + general: sweep M(sf), ptype, rmode, opcode, and reg fields.
  for (uint32_t M = 0; M < 2; ++M)
    for (uint32_t Ptype = 0; Ptype < 4; ++Ptype)
      for (uint32_t Rmode = 0; Rmode < 4; ++Rmode)
        for (uint32_t Opcode = 0; Opcode < 8; ++Opcode)
          for (uint32_t Rn : {0u, 1u, 31u})
            for (uint32_t Rd : {0u, 2u, 31u}) {
              uint32_t W = (M << 31) | (0x1Eu << 24) | (Ptype << 22) |
                           (1u << 21) | (Rmode << 19) | (Opcode << 16) |
                           (Rn << 5) | Rd;
              std::string D = checkOne(O, W, A);
              EXPECT_TRUE(D.empty()) << D;
            }
  // FP data-processing 1-source (opcode(20:15)==0, bits[14:10]==0b10000) hits
  // the FMOV-register form; sweep ptype and registers.
  for (uint32_t Ptype = 0; Ptype < 4; ++Ptype)
    for (uint32_t Rn = 0; Rn < 32; ++Rn)
      for (uint32_t Rd : {0u, 31u}) {
        uint32_t W = (0x1Eu << 24) | (Ptype << 22) | (1u << 21) | (0x10u << 10) |
                     (Rn << 5) | Rd;
        std::string D = checkOne(O, W, A);
        EXPECT_TRUE(D.empty()) << D;
      }
  // Scalar immediate: ptype x every imm8 x a couple of Rd (+ a nonzero imm5
  // field, which must be declined).
  for (uint32_t Ptype = 0; Ptype < 4; ++Ptype)
    for (uint32_t Imm8 = 0; Imm8 < 256; ++Imm8)
      for (uint32_t Imm5 : {0u, 1u})
        for (uint32_t Rd : {0u, 5u, 31u}) {
          uint32_t W = (0x1Eu << 24) | (Ptype << 22) | (1u << 21) |
                       (Imm8 << 13) | (0x4u << 10) | (Imm5 << 5) | Rd;
          std::string D = checkOne(O, W, A);
          EXPECT_TRUE(D.empty()) << D;
        }
}
// Scalar FP data-processing: 1-source (FABS/FNEG/FSQRT/FCVT), 2-source
// (FMUL/FDIV/FADD/FSUB/FMAX/FMIN/FMAXNM/FMINNM/FNMUL), and FCMP/FCMPE (register
// and zero forms) across ptype (S/D/H + the UNALLOCATED 10) and the opcode
// fields, so every taken/declined boundary is lifted both ways.
TEST_F(A64NativeParity, FpScalarDataProcSweep) {
  const va_t A = 0xF80000;
  for (uint32_t Ptype = 0; Ptype < 4; ++Ptype) {
    // 1-source: opcode(20:15) 0..0x0F covers FMOV/FABS/FNEG/FSQRT + the FCVT
    // targets (0001TT) and some FRINT declines.
    for (uint32_t Opcode = 0; Opcode < 0x10; ++Opcode)
      for (uint32_t Rn : {0u, 31u})
        for (uint32_t Rd : {0u, 31u}) {
          uint32_t W = (0x1Eu << 24) | (Ptype << 22) | (1u << 21) |
                       (Opcode << 15) | (0x10u << 10) | (Rn << 5) | Rd;
          EXPECT_TRUE(checkOne(O, W, A).empty());
        }
    // 2-source: opcode(15:12) 0..0x0F (9..15 declined).
    for (uint32_t Opcode = 0; Opcode < 0x10; ++Opcode)
      for (uint32_t Rm : {1u, 31u})
        for (uint32_t Rn : {2u, 31u}) {
          uint32_t W = (0x1Eu << 24) | (Ptype << 22) | (1u << 21) | (Rm << 16) |
                       (Opcode << 12) | (2u << 10) | (Rn << 5) | 0u;
          EXPECT_TRUE(checkOne(O, W, A).empty());
        }
    // FP compare: op(15:14) x opcode2(4:0) x Rm x Rn.
    for (uint32_t Op = 0; Op < 2; ++Op)
      for (uint32_t Opcode2 = 0; Opcode2 < 0x20; ++Opcode2)
        for (uint32_t Rm : {0u, 2u})
          for (uint32_t Rn : {0u, 3u}) {
            uint32_t W = (0x1Eu << 24) | (Ptype << 22) | (1u << 21) |
                         (Rm << 16) | (Op << 14) | (0x8u << 10) | (Rn << 5) |
                         Opcode2;
            EXPECT_TRUE(checkOne(O, W, A).empty());
          }
  }
}

} // namespace
