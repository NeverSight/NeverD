//===- AllPlatform_ConstFoldWidthRTTests.cpp - const-fold width mask -C++-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Adversarial probes for the MedIR copy/constant-propagation pass: a narrow
// arithmetic op whose folded constant OVERFLOWS the operand width, then the
// (truncated) result is widened.  At -O0 clang keeps the two constants in
// separate stack slots and emits a real runtime op, so NeverD's propagation is
// what folds it -- if the fold does not mask the result to the output width,
// the widening leaks the overflow bits (e.g. 0x80000000+0x80000001 must be
// 0x1 as u32, not 0x100000001 when zero-extended to u64).  Run on all four
// targets since the propagation pass is architecture-independent.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64ConstFoldWidthRT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64ConstFoldWidthRT, Verify) { roundTripX64(GetParam()); }

class X86ConstFoldWidthRT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86ConstFoldWidthRT, Verify) { roundTripX86(GetParam()); }

class A64ConstFoldWidthRT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64ConstFoldWidthRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32ConstFoldWidthRT : public SemanticRoundTripFixture,
                              public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32ConstFoldWidthRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeCFWTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  // OptLevel 0: clang keeps x,y in distinct slots, emits a runtime op; NeverD's
  // propagation folds the two constants -> exercises width masking on the fold.
  return {
    // u32 add overflow, zero-extend to u64.  (0x80000000+0x80000001)=0x1 as u32.
    {p+"_add32",
     t+" "+p+"_add32("+t+" a){ unsigned x=0x80000000u,y=0x80000001u; unsigned r=x+y;\n"
     "  return ("+t+")(unsigned long long)r + (a&0); }\n",
     {7ULL}, "ConstFoldWidth", 0},
    // u16 add overflow, zero-extend to u32.  (0xF000+0x2000)=0x1000 as u16.
    {p+"_add16",
     t+" "+p+"_add16("+t+" a){ unsigned short x=0xF000,y=0x2000; unsigned short r=(unsigned short)(x+y);\n"
     "  return ("+t+")(unsigned)r + (a&0); }\n",
     {7ULL}, "ConstFoldWidth", 0},
    // u8 add overflow, zero-extend.  (0xF0+0x20)=0x10 as u8.
    {p+"_add8",
     t+" "+p+"_add8("+t+" a){ unsigned char x=0xF0,y=0x20; unsigned char r=(unsigned char)(x+y);\n"
     "  return ("+t+")(unsigned)r + (a&0); }\n",
     {7ULL}, "ConstFoldWidth", 0},
    // u32 multiply overflow, zero-extend to u64.  0x10000*0x10000 = 0 as u32.
    {p+"_mul32",
     t+" "+p+"_mul32("+t+" a){ unsigned x=0x10000u,y=0x10000u; unsigned r=x*y;\n"
     "  return ("+t+")(unsigned long long)r + (a&0); }\n",
     {7ULL}, "ConstFoldWidth", 0},
    // u32 shift-left overflow, zero-extend.  0xFFFFFFFF<<8 = 0xFFFFFF00 as u32.
    {p+"_shl32",
     t+" "+p+"_shl32("+t+" a){ unsigned x=0xFFFFFFFFu,s=8; unsigned r=x<<s;\n"
     "  return ("+t+")(unsigned long long)r + (a&0); }\n",
     {7ULL}, "ConstFoldWidth", 0},
    // u16 sub underflow, zero-extend.  (0x10-0x30)=0xFFE0 as u16.
    {p+"_sub16",
     t+" "+p+"_sub16("+t+" a){ unsigned short x=0x10,y=0x30; unsigned short r=(unsigned short)(x-y);\n"
     "  return ("+t+")(unsigned)r + (a&0); }\n",
     {7ULL}, "ConstFoldWidth", 0},
    // i32 add overflow, SIGN-extend to i64.  (0x7FFFFFFF+1)=-0x80000000 as i32.
    {p+"_sadd32",
     t+" "+p+"_sadd32("+t+" a){ int x=0x7FFFFFFF,y=1; int r=x+y;\n"
     "  return ("+t+")(long long)r + (long)(a&0); }\n",
     {7ULL}, "ConstFoldWidth", 0},
    // i16 add overflow, sign-extend.  (0x7FF0+0x20)=-0x7FF0 region as i16.
    {p+"_sadd16",
     t+" "+p+"_sadd16("+t+" a){ short x=0x7FF0,y=0x20; short r=(short)(x+y);\n"
     "  return ("+t+")(int)r + (int)(a&0); }\n",
     {7ULL}, "ConstFoldWidth", 0},
    // Chained: u8 add overflow -> u16 add overflow -> zext (nested fold).
    {p+"_chain",
     t+" "+p+"_chain("+t+" a){ unsigned char x=0xC0,y=0x80; unsigned char r8=(unsigned char)(x+y);\n"
     "  unsigned short m=0xFF80, r16=(unsigned short)(m+r8);\n"
     "  return ("+t+")(unsigned)r16 + (a&0); }\n",
     {7ULL}, "ConstFoldWidth", 0},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeCFWTC("x64cfw", "long");
static const std::vector<RoundTripTC> kX86 = makeCFWTC("x86cfw", "int");
static const std::vector<RoundTripTC> kA64 = makeCFWTC("a64cfw", "long");
static const std::vector<RoundTripTC> kARM = makeCFWTC("armcfw", "int");

INSTANTIATE_TEST_SUITE_P(ConstFoldWidth, X64ConstFoldWidthRT,
                         ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(ConstFoldWidth, X86ConstFoldWidthRT,
                         ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(ConstFoldWidth, A64ConstFoldWidthRT,
                         ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(ConstFoldWidth, ARM32ConstFoldWidthRT,
                         ::testing::ValuesIn(kARM), rtTCName);
