//===- AllPlatform_OptStress12RTTests.cpp - opt-stress probes --*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Optimizer-stress roundtrip probes orthogonal to OptStress1-11 / SubRegMix,
// picked to hit value+flag duality and wide-result paths that have repeatedly
// hidden NeverD optimizer / lift semantic bugs:
//
//   * adc64    - 64-bit add-with-carry accumulator fed by a 32-bit stream
//                (on i386/arm32 a real add+adc pair, carry crossing the lane
//                boundary; on x64/a64 a native 64-bit add).
//   * sbb64    - 64-bit subtract-with-borrow accumulator (borrow lane-crossing).
//   * mulhi32  - high 32 bits of a 32x32->64 product (i386 `mul` reads EDX;
//                stresses MULHI / EDX:EAX modeling) blended with the low half.
//   * rotmix   - data-dependent left/right rotate by a [1,31] count (rol/ror,
//                lowered through fshl/fshr) folded into a hash.
//   * clamp    - nested saturating clamp lo<=v<=hi (chained csel/cmov reusing
//                the compare flags) over a running value.
//   * flag2use - one unsigned-add carry compare consumed twice: as a branch
//                (conditional increment) and as an addend value, same flag.
//
// Every kernel is integer-only, folds to a single integer return, and lowers to
// no runtime helper, so all four targets are checked native vs lifted at -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress12RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress12RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress12RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress12RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress12RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress12RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress12RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress12RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress12TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 64-bit add-with-carry accumulator fed by a 32-bit PRNG stream.
    {p+"_adc64",
     t+" "+p+"_adc64("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; unsigned long long acc=0; unsigned h=0;\n"
     "  for(int i=0;i<48;i++){ x=x*1103515245u+12345u;\n"
     "    acc+=((unsigned long long)x<<7)+(x>>3);\n"
     "    h=h*131u+(unsigned)acc+(unsigned)(acc>>32); }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x4cULL}, "OptStress12", 2},

    // 64-bit subtract-with-borrow accumulator (borrow crosses the lane edge).
    {p+"_sbb64",
     t+" "+p+"_sbb64("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; unsigned long long acc=0xF000000000000000ull;\n"
     "  unsigned h=0;\n"
     "  for(int i=0;i<48;i++){ x=x*1103515245u+12345u;\n"
     "    acc-=((unsigned long long)(x>>1)<<5)+(x>>9);\n"
     "    h=h*131u+(unsigned)acc+(unsigned)(acc>>32); }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x9bULL}, "OptStress12", 2},

    // High 32 bits of a 32x32->64 product blended with the low half.
    {p+"_mulhi32",
     t+" "+p+"_mulhi32("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u, y=(unsigned)a^0x9e3779b9u, h=0;\n"
     "  for(int i=0;i<40;i++){ x=x*1103515245u+12345u; y=y*2654435761u+1u;\n"
     "    unsigned long long pr=(unsigned long long)x*(unsigned long long)y;\n"
     "    unsigned hi=(unsigned)(pr>>32), lo=(unsigned)pr;\n"
     "    h=h*131u+hi+(lo>>16); }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0xa7ULL}, "OptStress12", 2},

    // Data-dependent left/right rotate by a [1,31] count.
    {p+"_rotmix",
     t+" "+p+"_rotmix("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<40;i++){ x=x*1103515245u+12345u;\n"
     "    unsigned r=((x>>5)&31u)|1u; unsigned v=x>>7;\n"
     "    unsigned rl=(v<<r)|(v>>(32-r));\n"
     "    unsigned rr=(v>>r)|(v<<(32-r));\n"
     "    h=h*131u+((x&1u)?rl:rr); }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x35ULL}, "OptStress12", 2},

    // Nested saturating clamp lo<=v<=hi over a running value.
    {p+"_clamp",
     t+" "+p+"_clamp("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; int v=0,h=0;\n"
     "  for(int i=0;i<48;i++){ x=x*1103515245u+12345u;\n"
     "    int lo=-(int)((x>>20)&0x3ff), hi=(int)((x>>8)&0x7ff);\n"
     "    v+=(int)(x>>12)-(int)(x>>15);\n"
     "    v=(v<lo)?lo:((v>hi)?hi:v);\n"
     "    h=h*131+v; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x6dULL}, "OptStress12", 2},

    // One carry compare consumed as both a branch and an addend value.
    {p+"_flag2use",
     t+" "+p+"_flag2use("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u, acc=0, cnt=0, h=0;\n"
     "  for(int i=0;i<48;i++){ x=x*1103515245u+12345u;\n"
     "    unsigned old=acc; acc+=(x>>2);\n"
     "    unsigned c=(acc<old);\n"
     "    if(c) cnt++;\n"
     "    h=h*131u+acc+c*7u+cnt; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x13ULL}, "OptStress12", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress12TC("x64o12", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress12TC("x86o12", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress12TC("a64o12", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress12TC("armo12", "int");

INSTANTIATE_TEST_SUITE_P(OptStress12, X64OptStress12RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress12, X86OptStress12RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress12, A64OptStress12RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress12, ARM32OptStress12RT, ::testing::ValuesIn(kARM), rtTCName);
