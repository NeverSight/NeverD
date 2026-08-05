//===- AllPlatform_ShiftedRegOperandRTTests.cpp - ALU shifted ops *- C++ -*-==//
//
// ALU instructions with a shifted/rotated/extended register operand:
//   AArch64  add/sub/and/orr/eor/bic xD,xN,xM,{lsl|lsr|asr|ror} #s
//            add/sub/cmp xD,xN,wM,{sxtb|sxth|sxtw|uxtb|uxth} #s
//   ARM32    add/sub/and/orr/eor rD,rN,rM,{lsl|lsr|asr|ror} #s
// clang -O2 folds `(x<<k)`, `(x>>k)`, sign/zero-extended sub-words, and the
// `(x>>k)|(x<<(W-k))` rotate idiom straight into these operand forms.  A lifter
// that drops the modifier (the AArch64 ROR `default`-branch bug fixed in #378)
// or mis-sizes the extend silently corrupts the operand, so each kernel chains
// several forms and folds the result into the return for native-vs-lifted
// comparison.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64ShRegRT : public SemanticRoundTripFixture,
                   public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64ShRegRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32ShRegRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32ShRegRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kA64 = {
  // Logical ops with shifted/rotated register operands (ROR is the #378 fix).
  {"a64_logsh",
   "long a64_logsh(long a,long b){ unsigned long x=(unsigned long)a, y=(unsigned long)b;\n"
   "  unsigned long r=x & (y<<5);\n"
   "  r ^= (y>>7);\n"
   "  r |= ((y>>13)|(y<<51));\n"      // eor/orr ..., ror #13
   "  r &= ~(y<<3);\n"               // bic ..., lsl #3
   "  r ^= ((y>>29)|(y<<35));\n"      // ror #29
   "  return (long)r; }\n",
   {0x123456789ABCDEFLL, 0x0FEDCBA987654321LL}, "ShiftedReg", 2, ""},

  // Arithmetic ops with logical/arith shifted register operands.
  {"a64_arithsh",
   "long a64_arithsh(long a,long b){ long r=a + (b<<4);\n"
   "  r = r - ((unsigned long)b>>6);\n"   // sub ..., lsr #6
   "  r = r + (b>>3);\n"                  // add ..., asr #3 (signed)
   "  r = r - (b<<11);\n"
   "  return r; }\n",
   {0x55AA55AA55ULL, (uint64_t)-0x1234567LL}, "ShiftedReg", 2, ""},

  // Extended-register operands: sign/zero-extend a sub-word then optional lsl.
  {"a64_extreg",
   "long a64_extreg(long a,long b){ long r=a;\n"
   "  r += (long)(signed char)b << 2;\n"   // sxtb #2
   "  r += (long)(short)b << 1;\n"         // sxth #1
   "  r += (long)(int)b << 3;\n"           // sxtw #3
   "  r += (long)(unsigned char)b << 2;\n" // uxtb #2
   "  r -= (long)(unsigned short)b;\n"     // uxth
   "  return r; }\n",
   {0x7777777777LL, (long)0xFFFFFF93A5C781LL}, "ShiftedReg", 2, ""},
};

static const std::vector<RoundTripTC> kARM = {
  // ARM32 logical/arith ops with shifted/rotated register operands.
  {"arm_logsh",
   "int arm_logsh(int a,int b){ unsigned x=(unsigned)a, y=(unsigned)b;\n"
   "  unsigned r=x & (y<<5);\n"
   "  r ^= (y>>7);\n"
   "  r |= ((y>>13)|(y<<19));\n"      // ror #13 (32-bit)
   "  r &= ~(y<<3);\n"               // bic ..., lsl #3
   "  r ^= ((y>>27)|(y<<5));\n"       // ror #27
   "  return (int)r; }\n",
   {0x1A2B3C4D, 0x55AA33CC}, "ShiftedReg", 2, ""},

  {"arm_arithsh",
   "int arm_arithsh(int a,int b){ int r=a + (b<<4);\n"
   "  r = r - ((unsigned)b>>6);\n"   // lsr #6
   "  r = r + (b>>3);\n"             // asr #3 (signed)
   "  r = r - (b<<11);\n"
   "  return r; }\n",
   {0x33445566ULL, (uint64_t)-0x12345LL}, "ShiftedReg", 2, ""},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(ShiftedReg, A64ShRegRT, ::testing::ValuesIn(kA64),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(ShiftedReg, ARM32ShRegRT, ::testing::ValuesIn(kARM),
                         rtTCName);
