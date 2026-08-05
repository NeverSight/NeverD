//===- AllPlatform_OptStress79RTTests.cpp - funnel/subreg/ptr --*-C++*-=//
//
// Four unrelated aggressive integer corners the prior probes never hit head-on:
//
//   * funnel64 - a genuine TWO-operand funnel shift `(hi<<n)|(lo>>(64-n))` with
//                distinct hi/lo and a runtime count (real shld/shrd, not the
//                single-operand rotate OptStress78 covered).  On the 32-bit
//                targets this is a word-crossing inline sequence for two
//                independent 64-bit operands.
//   * mixacc   - a 64-bit accumulator read back as 8/16/32-bit pieces and
//                reassembled every iteration: heavy loop-carried sub-register
//                aliasing (the RAX/AL optimizer-aliasing family).
//   * negwalk  - a runtime-filled global int[] walked BACKWARD via a pointer
//                (p=arr+N-1; p>=arr; p--) plus a reverse-index pass: a computed
//                global base address + a pointer comparison against the base.
//   * adcchain - loop-carried 64-bit conditional add/sub carry chains (adc/sbb
//                survival across a branch on the 32-bit register-pair targets).
//
// All integer, fold to one return, no float / 64-bit divide / libcall.  All
// four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress79RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress79RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress79RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress79RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress79RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress79RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress79RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress79RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress79TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Genuine two-operand funnel shift (shld/shrd), runtime count in [1,63].
    {p+"_funnel64",
     t+" "+p+"_funnel64("+t+" a){\n"
     "  unsigned long long hi=(unsigned long long)a^0x9E3779B97F4A7C15ULL;\n"
     "  unsigned long long lo=(unsigned long long)a*0xD1B54A32D192ED03ULL+1;\n"
     "  unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned n=1u+((s>>5)%63u);\n"
     "    unsigned long long f=(hi<<n)|(lo>>(64-n));\n"
     "    lo=hi; hi=f^(f>>31); }\n"
     "  return ("+t+")(hi^lo^(hi>>32)); }\n",
     {0xD1u}, "OptStress79", 2},

    // 64-bit accumulator read back as 8/16/32-bit pieces and reassembled.
    {p+"_mixacc",
     t+" "+p+"_mixacc("+t+" a){\n"
     "  unsigned long long acc=(unsigned long long)a^0x1122334455667788ULL;\n"
     "  unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<160;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned char b=(unsigned char)(acc+s);\n"
     "    unsigned short w=(unsigned short)((acc>>8)+s);\n"
     "    unsigned d=(unsigned)(acc>>16)^s;\n"
     "    acc=((unsigned long long)d<<32)|((unsigned long long)w<<8)|b;\n"
     "    acc+=(unsigned long long)b*w+d; acc^=acc>>23; }\n"
     "  return ("+t+")(acc^(acc>>32)); }\n",
     {0xD2u}, "OptStress79", 2},

    // Runtime-filled global int[] walked backward by pointer + reverse index.
    {p+"_negwalk",
     "static int "+p+"_arr[64];\n"
     +t+" "+p+"_negwalk("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  for(int i=0;i<64;i++){ s=s*1103515245u+12345u; "+p+"_arr[i]=(int)(s>>9); }\n"
     "  for(int *q="+p+"_arr+63; q>="+p+"_arr; q--){ sum=sum*131+*q; sum^=sum>>7; }\n"
     "  for(int i=63;i>=0;i--) sum+="+p+"_arr[i]*(i+1);\n"
     "  return ("+t+")sum; }\n",
     {0xD3u}, "OptStress79", 2},

    // Loop-carried 64-bit conditional add/sub carry chains (adc/sbb).
    {p+"_adcchain",
     t+" "+p+"_adcchain("+t+" a){\n"
     "  unsigned long long x=(unsigned long long)a, y=~x, c=0;\n"
     "  unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned long long d=((unsigned long long)s<<32)|(s^0x55555555u);\n"
     "    if(s&1u){ unsigned long long n=x+d; c+=(n<x); x=n; }\n"
     "    else    { unsigned long long n=x-d; c+=(n>x); x=n; }\n"
     "    y^=x+c; y=(y<<1)|(y>>63); }\n"
     "  return ("+t+")(x^y^c); }\n",
     {0xD4u}, "OptStress79", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress79TC("x64o79", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress79TC("x86o79", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress79TC("a64o79", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress79TC("armo79", "int");

INSTANTIATE_TEST_SUITE_P(OptStress79, X64OptStress79RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress79, X86OptStress79RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress79, A64OptStress79RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress79, ARM32OptStress79RT, ::testing::ValuesIn(kARM), rtTCName);
