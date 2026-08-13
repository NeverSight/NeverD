//===- AArch64_NativeDecodeParityTests.cpp - integer data processing parity -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "AArch64_NativeDecodeParityTestsDetail.h"

namespace {

using namespace neverd;
using namespace neverd::a64_parity_test;

TEST_F(A64NativeParity, EdgeEncodings) {
  const va_t A = 0x100000;
  const uint32_t Words[] = {
      0x10000000u, // adr x0, .
      0x10000060u, // adr x0, .+12
      0x70000123u, // adr x3, (immlo=11)
      0x90000000u, // adrp x0, .
      0x90000123u, // adrp x3
      0xB0000123u, // adrp x3 (immlo set)
      0x1000001Fu, // adr xzr
      0xD2800000u, // movz x0, #0
      0xD2800020u, // movz x0, #1
      0xD2A00020u, // movz x0, #1, lsl 16
      0xD2C00020u, // movz x0, #1, lsl 32
      0xD2E00020u, // movz x0, #1, lsl 48
      0x52800020u, // movz w0, #1
      0x52A00020u, // movz w0, #1, lsl 16
      0x92800000u, // movn x0, #0  (-> -1)
      0x92800020u, // movn x0, #1
      0x12800020u, // movn w0, #1
      0xF2800020u, // movk x0, #1
      0xF2A00020u, // movk x0, #1, lsl 16
      0x72800020u, // movk w0, #1
      0x7280001Fu, // movk wzr, #0
      0x529FFFE0u, // movz w0, #0xffff
      0xD29FFFE0u, // movz x0, #0xffff
      0xD503201Fu, // nop
      0x92400400u, // and x0, x0, #3
      0x12000000u, // and w0, w0, #1
      0xF240001Fu, // tst x0, #1  (ands zr alias)
      0x92000000u, // and x0, x0, #0x100000001
  };
  for (uint32_t W : Words) {
    std::string D = checkOne(O, W, A);
    EXPECT_TRUE(D.empty()) << D;
  }
}

TEST_F(A64NativeParity, AdrAdrpSweep) {
  const va_t A = 0x210000; // deliberately not page-aligned
  const uint32_t ImmHiSamples[] = {0u,     1u,      2u,      0x3FFFEu,
                                   0x3FFFFu, 0x40000u, 0x7FFFEu, 0x7FFFFu};
  for (uint32_t Op = 0; Op < 2; ++Op)
    for (uint32_t Rd = 0; Rd < 32; ++Rd)
      for (uint32_t ImmLo = 0; ImmLo < 4; ++ImmLo)
        for (uint32_t ImmHi : ImmHiSamples) {
          uint32_t W = (Op << 31) | (ImmLo << 29) | 0x10000000u |
                       ((ImmHi & 0x7FFFF) << 5) | Rd;
          std::string D = checkOne(O, W, A);
          EXPECT_TRUE(D.empty()) << D;
        }
}

// Exhaustive over the wide-move selector fields (opc, sf, hw, Rd) crossed with
// imm16 samples — covers MOVN/MOVZ/MOVK including the UNALLOCATED 32-bit hw>=2
// forms native must decline.
TEST_F(A64NativeParity, MoveWideSweep) {
  const va_t A = 0x300000;
  const uint32_t Imm16Samples[] = {0u,      1u,      2u,      0x1234u,
                                   0x7FFFu, 0x8000u, 0xABCDu, 0xFFFFu};
  for (uint32_t Opc = 0; Opc < 4; ++Opc)      // 01 must be declined
    for (uint32_t Sf = 0; Sf < 2; ++Sf)
      for (uint32_t Hw = 0; Hw < 4; ++Hw)     // hw>=2 w/ sf=0 must be declined
        for (uint32_t Rd = 0; Rd < 32; ++Rd)
          for (uint32_t Imm : Imm16Samples) {
            uint32_t W = (Sf << 31) | (Opc << 29) | (0x25u << 23) |
                         (Hw << 21) | ((Imm & 0xFFFF) << 5) | Rd;
            std::string D = checkOne(O, W, A);
            EXPECT_TRUE(D.empty()) << D;
          }
}

// Add/subtract immediate, incl. the MOV-to/from-SP, CMP and CMN alias
// boundaries (Rd/Rn==31, imm==0, S/sh bits).
TEST_F(A64NativeParity, AddSubImmSweep) {
  const va_t A = 0x500000;
  const uint32_t Imm12Samples[] = {0u, 1u, 0x30u, 0x7FFu, 0xFFFu};
  for (uint32_t Sf = 0; Sf < 2; ++Sf)
    for (uint32_t Op = 0; Op < 2; ++Op)     // add / sub
      for (uint32_t S = 0; S < 2; ++S)      // flag-setting
        for (uint32_t Sh = 0; Sh < 2; ++Sh) // lsl #12
          for (uint32_t Rd : {0u, 1u, 29u, 30u, 31u})
            for (uint32_t Rn : {0u, 5u, 31u})
              for (uint32_t Imm : Imm12Samples) {
                uint32_t W = (Sf << 31) | (Op << 30) | (S << 29) |
                             (0x11u << 24) | (Sh << 22) | ((Imm & 0xFFF) << 10) |
                             (Rn << 5) | Rd;
                std::string D = checkOne(O, W, A);
                EXPECT_TRUE(D.empty()) << D;
              }
}

// Add/subtract shifted register + logical shifted register, incl. the CMP/CMN,
// NEG/NEGS, MOV/MVN/TST alias boundaries and the ROR/oversized-shift declines.
TEST_F(A64NativeParity, DpRegSweep) {
  const va_t A = 0x600000;
  const uint32_t Imm6Samples[] = {0u, 1u, 4u, 31u, 32u, 63u};
  // Add/sub shifted register: op0=0b01011, bit21=0.
  for (uint32_t Sf = 0; Sf < 2; ++Sf)
    for (uint32_t Op = 0; Op < 2; ++Op)
      for (uint32_t S = 0; S < 2; ++S)
        for (uint32_t Sh = 0; Sh < 4; ++Sh) // 3 (ROR) must be declined
          for (uint32_t Rd : {0u, 1u, 31u})
            for (uint32_t Rn : {0u, 31u})
              for (uint32_t Rm : {1u, 2u})
                for (uint32_t Imm6 : Imm6Samples) {
                  uint32_t W = (Sf << 31) | (Op << 30) | (S << 29) |
                               (0x0Bu << 24) | (Sh << 22) | (Rm << 16) |
                               ((Imm6 & 0x3F) << 10) | (Rn << 5) | Rd;
                  std::string D = checkOne(O, W, A);
                  EXPECT_TRUE(D.empty()) << D;
                }
  // Logical shifted register: op0=0b01010, all shift types + N bit.
  for (uint32_t Sf = 0; Sf < 2; ++Sf)
    for (uint32_t Opc = 0; Opc < 4; ++Opc)
      for (uint32_t Sh = 0; Sh < 4; ++Sh)
        for (uint32_t N = 0; N < 2; ++N)
          for (uint32_t Rd : {0u, 1u, 31u})
            for (uint32_t Rn : {0u, 31u})
              for (uint32_t Rm : {1u, 2u})
                for (uint32_t Imm6 : Imm6Samples) {
                  uint32_t W = (Sf << 31) | (Opc << 29) | (0x0Au << 24) |
                               (Sh << 22) | (N << 21) | (Rm << 16) |
                               ((Imm6 & 0x3F) << 10) | (Rn << 5) | Rd;
                  std::string D = checkOne(O, W, A);
                  EXPECT_TRUE(D.empty()) << D;
                }
}
// Logical immediate (AND/ORR/EOR/ANDS + TST alias): sweep sf/opc/N/immr/imms
// exhaustively over the small field space so every valid bitmask encoding (and
// every UNDEFINED one, which must be declined) is exercised, across a couple of
// Rd/Rn choices that select the plain / ZR / SP register forms.
TEST_F(A64NativeParity, LogicalImmSweep) {
  const va_t A = 0xA00000;
  for (uint32_t Sf = 0; Sf < 2; ++Sf)
    for (uint32_t Opc = 0; Opc < 4; ++Opc)
      for (uint32_t N = 0; N < 2; ++N)
        for (uint32_t Immr = 0; Immr < 64; Immr += 5)
          for (uint32_t Imms = 0; Imms < 64; Imms += 3)
            for (uint32_t Regs : {0u, 0x3E0u /*Rn=31*/, 0x1Fu /*Rd=31*/}) {
              uint32_t W = (Sf << 31) | (Opc << 29) | (0x24u << 23) |
                           (N << 22) | (Immr << 16) | (Imms << 10) | Regs;
              std::string D = checkOne(O, W, A);
              EXPECT_TRUE(D.empty()) << D;
            }
}

// Conditional select + its cset/csetm/cinc/cinv/cneg aliases across all
// conditions and register combinations, plus MADD/MSUB/MUL/MNEG, SMULH/UMULH,
// and SDIV/UDIV.
TEST_F(A64NativeParity, CondSelectAndMulDivSweep) {
  const va_t A = 0xB00000;
  // Conditional select: op(bit30) x op2(bit10) x cond x (Rm,Rn) selecting the
  // canonical / cset / cinc / cneg forms.
  for (uint32_t Sf = 0; Sf < 2; ++Sf)
    for (uint32_t Op = 0; Op < 2; ++Op)
      for (uint32_t Op2 = 0; Op2 < 2; ++Op2)
        for (uint32_t Cond = 0; Cond < 16; ++Cond)
          for (auto RmRn : {std::pair<uint32_t, uint32_t>{2u, 3u},
                            {31u, 31u}, {5u, 5u}, {31u, 7u}}) {
            uint32_t W = (Sf << 31) | (Op << 30) | (0xD4u << 21) |
                         (RmRn.first << 16) | (Cond << 12) | (Op2 << 10) |
                         (RmRn.second << 5) | 1u;
            EXPECT_TRUE(checkOne(O, W, A).empty());
          }
  // Multiply-accumulate (3-source) and the mul/mneg aliases; smulh/umulh.
  for (uint32_t Sf = 0; Sf < 2; ++Sf)
    for (uint32_t Op31 = 0; Op31 < 8; ++Op31)
      for (uint32_t O0 = 0; O0 < 2; ++O0)
        for (uint32_t Ra : {0u, 4u, 31u}) {
          uint32_t W = (Sf << 31) | (0x1Bu << 24) | (Op31 << 21) | (2u << 16) |
                       (O0 << 15) | (Ra << 10) | (1u << 5) | 0u;
          EXPECT_TRUE(checkOne(O, W, A).empty());
        }
  // Data-processing (2-source): sweep opcode to hit SDIV/UDIV (taken) and the
  // shift-variable / CRC forms (declined).
  for (uint32_t Sf = 0; Sf < 2; ++Sf)
    for (uint32_t Op2 = 0; Op2 < 24; ++Op2) {
      uint32_t W = (Sf << 31) | (0x0D6u << 21) | (2u << 16) | (Op2 << 10) |
                   (1u << 5) | 0u;
      EXPECT_TRUE(checkOne(O, W, A).empty());
    }
}

// Bitfield move (SBFM/BFM/UBFM) exhaustively over opc x sf x immr x imms, for
// both a real source register and Rn==31, so every preferred-alias boundary
// (lsl/lsr/asr, uxtb/uxth, sxtb/sxth/sxtw, ubfx/ubfiz, sbfx/sbfiz, bfi/bfxil)
// and every declined case (Rn==31 BFM, 32-bit oversized field) is lifted both
// ways.  This is the class where the lifter reads the alias mnemonic/id, so
// only this lift-parity check (not the field probe) proves it.
TEST_F(A64NativeParity, BitfieldSweep) {
  const va_t A = 0xC00000;
  for (uint32_t Sf = 0; Sf < 2; ++Sf) {
    const uint32_t Hi = Sf ? 64u : 32u;
    for (uint32_t Opc = 0; Opc < 3; ++Opc)
      for (uint32_t Rn : {1u, 31u})
        for (uint32_t Immr = 0; Immr < Hi; ++Immr)
          for (uint32_t Imms = 0; Imms < Hi; ++Imms) {
            uint32_t W = (Sf << 31) | (Opc << 29) | (0x26u << 23) |
                         (Sf << 22) | (Immr << 16) | (Imms << 10) | (Rn << 5) |
                         2u;
            std::string D = checkOne(O, W, A);
            EXPECT_TRUE(D.empty()) << D;
          }
  }
}
// Variable shifts (LSLV/LSRV/ASRV/RORV, via the 2-source opcodes) and EXTR
// (incl. the ROR-immediate alias when Rn==Rm).
TEST_F(A64NativeParity, VarShiftAndExtrSweep) {
  const va_t A = 0xE00000;
  for (uint32_t Sf = 0; Sf < 2; ++Sf) {
    for (uint32_t Op2 = 8; Op2 <= 11; ++Op2)
      for (uint32_t Rm : {0u, 2u, 31u})
        for (uint32_t Rn : {1u, 31u}) {
          uint32_t W = (Sf << 31) | (0x0D6u << 21) | (Rm << 16) | (Op2 << 10) |
                       (Rn << 5) | 3u;
          EXPECT_TRUE(checkOne(O, W, A).empty());
        }
    // EXTR: N==sf, o0==0; sweep imms and Rm (Rn==Rm gives the ror alias).
    for (uint32_t Rm : {1u, 2u})
      for (uint32_t Rn : {1u, 5u})
        for (uint32_t Imms : {0u, 1u, 3u, 31u, 63u}) {
          uint32_t W = (Sf << 31) | (0x27u << 23) | (Sf << 22) | (Rm << 16) |
                       ((Imms & 0x3F) << 10) | (Rn << 5) | 0u;
          EXPECT_TRUE(checkOne(O, W, A).empty());
        }
  }
}

// Add/subtract extended register (bit21==1): every option x imm3(amount) x sf
// x op x S, across register choices that select the plain / SP / ZR forms and
// the CMP/CMN and natural-LSL (dropped-extend) alias boundaries.
TEST_F(A64NativeParity, AddSubExtendedRegSweep) {
  const va_t A = 0x640000;
  for (uint32_t Sf = 0; Sf < 2; ++Sf)
    for (uint32_t Op = 0; Op < 2; ++Op)
      for (uint32_t S = 0; S < 2; ++S)
        for (uint32_t Option = 0; Option < 8; ++Option)
          for (uint32_t Imm3 = 0; Imm3 < 8; ++Imm3) // 5..7 must be declined
            for (uint32_t Rd : {0u, 2u, 31u})
              for (uint32_t Rn : {0u, 31u})
                for (uint32_t Rm : {1u, 3u, 31u}) {
                  uint32_t W = (Sf << 31) | (Op << 30) | (S << 29) |
                               (0x0Bu << 24) | (1u << 21) | (Rm << 16) |
                               (Option << 13) | (Imm3 << 10) | (Rn << 5) | Rd;
                  std::string D = checkOne(O, W, A);
                  EXPECT_TRUE(D.empty()) << D;
                }
}

// Widening multiply-accumulate long (SMADDL/UMADDL/SMSUBL/UMSUBL) and their
// SMULL/UMULL/SMNEGL/UMNEGL (Ra==31) aliases, over sf (sf==0 declined), the
// op31/o0 selector, and the accumulator register.
TEST_F(A64NativeParity, WideningMulAddSweep) {
  const va_t A = 0xB80000;
  for (uint32_t Sf = 0; Sf < 2; ++Sf)      // sf==0 must be declined
    for (uint32_t Op31 = 0; Op31 < 8; ++Op31)
      for (uint32_t O0 = 0; O0 < 2; ++O0)
        for (uint32_t Rm : {0u, 2u, 31u})
          for (uint32_t Ra : {0u, 4u, 31u})
            for (uint32_t Rn : {1u, 31u}) {
              uint32_t W = (Sf << 31) | (0x1Bu << 24) | (Op31 << 21) |
                           (Rm << 16) | (O0 << 15) | (Ra << 10) | (Rn << 5) | 5u;
              std::string D = checkOne(O, W, A);
              EXPECT_TRUE(D.empty()) << D;
            }
}
// ORR (immediate) MOV-bitmask alias: sweep sf/immr/imms/N with Rn==31 (the
// MOV/ORR fork) plus a couple of Rd (SP-form) choices, so every bitmask value
// crosses the MoveWidePreferred boundary that decides mov vs orr rendering.
TEST_F(A64NativeParity, OrrMovBitmaskSweep) {
  const va_t A = 0xA80000;
  for (uint32_t Sf = 0; Sf < 2; ++Sf)
    for (uint32_t N = 0; N < 2; ++N)
      for (uint32_t Immr = 0; Immr < 64; ++Immr)
        for (uint32_t Imms = 0; Imms < 64; ++Imms)
          for (uint32_t Rd : {0u, 5u, 31u}) {
            uint32_t W = (Sf << 31) | (0x01u << 29) | (0x24u << 23) |
                         (N << 22) | (Immr << 16) | (Imms << 10) | (31u << 5) |
                         Rd;
            std::string D = checkOne(O, W, A);
            EXPECT_TRUE(D.empty()) << D;
          }
}

// Data-processing (1 source) integer: RBIT/REV16/REV32/REV/CLZ/CLS over sf and
// opcode (incl. opcode2!=0 and opcode>=6, which must be declined).
TEST_F(A64NativeParity, DataProc1SourceSweep) {
  const va_t A = 0xCC0000;
  for (uint32_t Sf = 0; Sf < 2; ++Sf)
    for (uint32_t Opcode2 = 0; Opcode2 < 2; ++Opcode2) // 1 -> reserved
      for (uint32_t Opcode = 0; Opcode < 8; ++Opcode)  // 6,7 -> declined
        for (uint32_t Rn : {0u, 31u})
          for (uint32_t Rd : {0u, 31u}) {
            uint32_t W = (Sf << 31) | (0x2D6u << 21) | (Opcode2 << 16) |
                         (Opcode << 10) | (Rn << 5) | Rd;
            std::string D = checkOne(O, W, A);
            EXPECT_TRUE(D.empty()) << D;
          }
}

} // namespace
