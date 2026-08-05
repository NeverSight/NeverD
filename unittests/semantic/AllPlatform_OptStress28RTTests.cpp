//===- AllPlatform_OptStress28RTTests.cpp - opt-stress probes --*-C++*-=//
//
// A second, harder optimizer probe targeting the self-written MedIR passes from
// angles OptStress27 did not cover: loop-carried narrow (8-bit) accumulators
// with overflow detection (i8 phi + carry flag), __builtin_*_overflow carry
// chains, signed bitfields read with sign-extension, variable-count rotates
// (funnel shift -> rol/ror), and a comparison result reused by both a select
// and a hash (cross-iteration flag liveness).
//
//   * i8carry  - loop-carried u8 accumulator, 8-bit carry + i8 sign test.
//   * ovf      - __builtin_add/mul_overflow chain accumulating overflow count.
//   * sbf      - signed bitfields, sign-extended reads in arithmetic.
//   * crossflag- one sign test driving a select and folded into the hash.
//   * minmax   - running min/max via conditional updates (cmov/csel reuse).
//   * rotmix   - variable-count 32-bit and 16-bit rotates (funnel shift).
//
// Integer-only, single integer return, no 64-bit divide; all four targets at -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress28RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress28RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress28RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress28RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress28RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress28RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress28RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress28RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress28TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Loop-carried u8 accumulator: 8-bit carry detect + i8 sign test.
    {p+"_i8carry",
     t+" "+p+"_i8carry("+t+" a){\n"
     "  unsigned char acc=(unsigned char)a, hi=0; unsigned h=0, s=(unsigned)a|1u;\n"
     "  for(int i=0;i<80;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned char d=(unsigned char)(s>>7), na=acc+d;\n"
     "    if(na<acc) hi++; acc=na;\n"
     "    if((signed char)acc<0) acc=(unsigned char)(~acc);\n"
     "    h=h*131u+acc+hi; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x41ULL}, "OptStress28", 2},

    // __builtin_add/mul_overflow chain accumulating an overflow count.
    {p+"_ovf",
     t+" "+p+"_ovf("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, acc=0, h=0, ovc=0;\n"
     "  for(int i=0;i<64;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned r; unsigned o1=__builtin_add_overflow(acc,s,&r)?1u:0u; acc=r;\n"
     "    unsigned m; unsigned o2=__builtin_mul_overflow(acc,3u,&m)?1u:0u; acc=m;\n"
     "    ovc+=o1+o2; h=h*131u+acc+ovc; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x6dULL}, "OptStress28", 2},

    // Signed bitfields read with sign-extension into arithmetic.
    {p+"_sbf",
     t+" "+p+"_sbf("+t+" a){\n"
     "  struct { int x:5, y:11, z:16; } v;\n"
     "  unsigned s=(unsigned)a|1u, h=0;\n"
     "  v.x=(int)s; v.y=(int)(s>>3); v.z=(int)(s>>9);\n"
     "  for(int i=0;i<64;i++){ s=s*1103515245u+12345u;\n"
     "    v.x+=(int)(s&7); v.y-=(int)((s>>4)&3); v.z^=(int)(s>>7);\n"
     "    int e=v.x*2+v.y-v.z; h=h*131u+(unsigned)e; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0xb2ULL}, "OptStress28", 2},

    // One sign test driving a select and folded into the hash.
    {p+"_crossflag",
     t+" "+p+"_crossflag("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, h=0; int run=0;\n"
     "  for(int i=0;i<96;i++){ s=s*1103515245u+12345u; int x=(int)s; int neg=x<0;\n"
     "    run = neg ? (run+1) : 0;\n"
     "    unsigned pick = neg ? (s>>2) : (s<<1);\n"
     "    h=h*131u+pick+(unsigned)run+(unsigned)neg; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x18ULL}, "OptStress28", 2},

    // Running min/max via conditional updates (cmov/csel reuse).
    {p+"_minmax",
     t+" "+p+"_minmax("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, h=0; int mn=2000000000, mx=-2000000000;\n"
     "  for(int i=0;i<96;i++){ s=s*1103515245u+12345u; int x=(int)s;\n"
     "    if(x<mn)mn=x; if(x>mx)mx=x; unsigned span=(unsigned)mx-(unsigned)mn;\n"
     "    h=h*131u+(unsigned)mn+(unsigned)mx+span; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x53ULL}, "OptStress28", 2},

    // Variable-count 32-bit and 16-bit rotates (funnel shift -> rol/ror).
    {p+"_rotmix",
     t+" "+p+"_rotmix("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<80;i++){ s=s*1103515245u+12345u; unsigned n=(unsigned)i&31u;\n"
     "    unsigned r=(s<<n)|(s>>((32u-n)&31u));\n"
     "    unsigned short w=(unsigned short)r; unsigned wn=(unsigned)i&15u;\n"
     "    w=(unsigned short)((w<<wn)|(w>>((16u-wn)&15u)));\n"
     "    h=h*131u+r+w; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x2fULL}, "OptStress28", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress28TC("x64o28", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress28TC("x86o28", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress28TC("a64o28", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress28TC("armo28", "int");

INSTANTIATE_TEST_SUITE_P(OptStress28, X64OptStress28RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress28, X86OptStress28RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress28, A64OptStress28RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress28, ARM32OptStress28RT, ::testing::ValuesIn(kARM), rtTCName);
