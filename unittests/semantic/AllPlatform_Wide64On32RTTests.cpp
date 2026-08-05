//===- AllPlatform_Wide64On32RTTests.cpp - 64-bit math on 32-bit ABI -*-C++*-=//
//
// High-yield roundtrip probing of 64-bit integer arithmetic, which on i386 and
// ARM32 decomposes into register-pair operations (ADC/SBB carry chains,
// SHLD/SHRD or lsl/lsr+orr double shifts, 64-bit compares lowered to two
// 32-bit compares, inline 64x64 multiply).  These pair/carry shapes have been a
// recurring source of sub-register and flag bugs (#394/#396), so each kernel
// keeps a 64-bit value live across a loop and folds it down to the 32-bit return
// type.  64-bit division/modulo is avoided (it is a runtime-library call the
// bare-metal harness cannot resolve).  All four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64W64RT : public SemanticRoundTripFixture,
                 public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64W64RT, Verify) { roundTripX64(GetParam()); }
class X86W64RT : public SemanticRoundTripFixture,
                 public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86W64RT, Verify) { roundTripX86(GetParam()); }
class A64W64RT : public SemanticRoundTripFixture,
                 public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64W64RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32W64RT : public SemanticRoundTripFixture,
                   public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32W64RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeW64TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 64-bit add/sub/negate carry chains (ADC/SBB on i386, ADDS/ADCS on ARM32).
    {p+"_carry",
     t+" "+p+"_carry("+t+" a){\n"
     "  unsigned long long u=(unsigned long long)(unsigned)a*0x9E3779B97F4A7C15ull;\n"
     "  unsigned long long acc=u^0x0123456789ABCDEFull;\n"
     "  for(int i=0;i<48;i++){ unsigned long long x=u+(unsigned long long)i*0x100000001ull;\n"
     "    acc+=x; acc-=(x>>1)|(x<<63); acc=(unsigned long long)(-(long long)acc)+acc*3ull; }\n"
     "  return ("+t+")(unsigned long)(unsigned)(acc^(acc>>32)); }\n",
     {0x51ULL}, "W64", 2},

    // 64-bit variable shifts (double-precision SHLD/SHRD / lsl+lsr+orr).
    {p+"_shift64",
     t+" "+p+"_shift64("+t+" a){\n"
     "  unsigned long long acc=((unsigned long long)(unsigned)a<<32)|0x1u;\n"
     "  long long sacc=(long long)acc|1;\n"
     "  for(int i=0;i<56;i++){ unsigned s=(unsigned)(i&63);\n"
     "    acc=(acc<<s)|(acc>>((64u-s)&63u));\n"
     "    sacc=(sacc>>(s&31))+((long long)acc<<(s&15));\n"
     "    acc^=(unsigned long long)sacc+(unsigned long long)i; }\n"
     "  return ("+t+")(unsigned long)(unsigned)(acc^(acc>>32)^(unsigned long long)sacc); }\n",
     {0x7ULL}, "W64", 2},

    // 64-bit signed/unsigned comparisons feeding selects (two-word compares).
    {p+"_cmp64",
     t+" "+p+"_cmp64("+t+" a){\n"
     "  unsigned acc=(unsigned)a;\n"
     "  for(int i=0;i<48;i++){\n"
     "    long long x=(long long)((unsigned long long)(unsigned)a*(unsigned long long)(i+1))-0x4000000000ll;\n"
     "    unsigned long long y=(unsigned long long)x^0xFFFF0000FFFFull;\n"
     "    unsigned r=0;\n"
     "    if(x<0) r+=1u; if(x> (long long)i) r+=2u; if(y< 0x8000000000ull) r+=4u;\n"
     "    if(y>=(unsigned long long)x) r+=8u; if(x==(long long)-i) r+=16u;\n"
     "    acc=acc*131u+r+(unsigned)(y>>40); }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x3ULL}, "W64", 2},

    // Inline 64x64->64 multiply (no libcall) folded into a mix/rotate hash.
    {p+"_mul64",
     t+" "+p+"_mul64("+t+" a){\n"
     "  unsigned long long acc=(unsigned long long)(unsigned)a|1ull;\n"
     "  for(int i=0;i<40;i++){ unsigned long long m=0xC2B2AE3D27D4EB4Full+(unsigned long long)i;\n"
     "    acc*=m; acc^=acc>>29; acc*=0x165667B19E3779F9ull; acc^=acc>>32;\n"
     "    acc+=(acc<<7)|(acc>>57); }\n"
     "  return ("+t+")(unsigned long)(unsigned)(acc^(acc>>32)); }\n",
     {0x9ULL}, "W64", 2},

    // Mixed 64-bit logic + byte swap + count, exercising pair handling broadly.
    {p+"_mix64",
     t+" "+p+"_mix64("+t+" a){\n"
     "  unsigned long long acc=((unsigned long long)(unsigned)a<<16)^0xDEADBEEFCAFEull;\n"
     "  for(int i=0;i<44;i++){ unsigned long long x=acc+(unsigned long long)i*0x1000100010001ull;\n"
     "    unsigned long long sw=__builtin_bswap64(x);\n"
     "    unsigned pc=(unsigned)__builtin_popcountll(x);\n"
     "    unsigned long long m=(x&0xFF00FF00FF00FF00ull)|((~x)&0x00FF00FF00FF00FFull);\n"
     "    acc=acc*0x100000001b3ull+sw+m+(unsigned long long)pc; }\n"
     "  return ("+t+")(unsigned long)(unsigned)(acc^(acc>>32)); }\n",
     {0x2ULL}, "W64", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeW64TC("x64w64", "long");
static const std::vector<RoundTripTC> kX86 = makeW64TC("x86w64", "int");
static const std::vector<RoundTripTC> kA64 = makeW64TC("a64w64", "long");
static const std::vector<RoundTripTC> kARM = makeW64TC("armw64", "int");

INSTANTIATE_TEST_SUITE_P(W64, X64W64RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(W64, X86W64RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(W64, A64W64RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(W64, ARM32W64RT, ::testing::ValuesIn(kARM), rtTCName);
