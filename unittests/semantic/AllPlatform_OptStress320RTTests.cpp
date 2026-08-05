//===- AllPlatform_OptStress320RTTests.cpp - -O3 wide reductions ---------===//
//
// First full-C OptStress batch compiled at -O3: nearly every prior OptStress
// probe ran at -O0/-O2, leaving the aggressive -O3 code shapes (loop unrolling,
// auto-vectorized integer reductions, loop distribution) unexercised through
// the lift roundtrip.  These shapes feed 64-bit accumulators that, on 32-bit
// targets, lower to register-pair (EDX:EAX / R1:R0) math — the historical
// hot spot for wide-value lift bugs (#157/#311/#441/#518/#524).
//
// All integer, stack-local arrays only, no 64-bit division and no aggregate
// init, so i386/ARM32 stay libcall-free on the bare-metal harness; deterministic
// LCG-seeded inputs folded to a single return value.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress320RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress320RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress320RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress320RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress320RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress320RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress320RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress320RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress320TC(const char *prefix,
                                                   const char *T, int Opt) {
  std::string p = prefix, t = T;
  std::vector<RoundTripTC> v = {
    // i64 add/xor reduction over a loop-filled array (auto-vectorized at -O3).
    {p+"_red64",
     t+" "+p+"_red64("+t+" a){ unsigned s=(unsigned)a|1u; int buf[64];\n"
     "  for(int i=0;i<64;i++) buf[i]=(int)((unsigned)a*2654435761u+(unsigned)i*40503u+s);\n"
     "  long long acc=0;\n"
     "  for(int i=0;i<64;i++){ acc += (long long)buf[i]; acc ^= (long long)buf[i]<<1; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n",
     {0x1234u}, "OptStress320", Opt},

    // Widening signed dot product: (int64)x[i]*(int64)y[i] -> single imul/smull,
    // summed into an i64 accumulator (register pair on 32-bit).
    {p+"_dot64",
     t+" "+p+"_dot64("+t+" a){ unsigned s=(unsigned)a^0x9e3779b9u; int x[48],y[48];\n"
     "  for(int i=0;i<48;i++){ s=s*1103515245u+12345u; x[i]=(int)s;\n"
     "    s=s*1103515245u+12345u; y[i]=(int)s; }\n"
     "  long long acc=0;\n"
     "  for(int i=0;i<48;i++) acc += (long long)x[i]*(long long)y[i];\n"
     "  return ("+t+")(acc + (acc>>31)); }\n",
     {0x2345u}, "OptStress320", Opt},

    // Running signed min/max reduction (vectorized to pminsd/pmaxsd / smin/smax),
    // both extremes folded into the result.
    {p+"_minmax",
     t+" "+p+"_minmax("+t+" a){ unsigned s=(unsigned)a+0x55u; int buf[80];\n"
     "  for(int i=0;i<80;i++){ s=s*22695477u+1u; buf[i]=(int)(s^(s>>13)); }\n"
     "  int mn=buf[0], mx=buf[0];\n"
     "  for(int i=1;i<80;i++){ if(buf[i]<mn) mn=buf[i]; if(buf[i]>mx) mx=buf[i]; }\n"
     "  return ("+t+")((long long)mx*131 - (long long)mn); }\n",
     {0x3456u}, "OptStress320", Opt},

    // Per-element branch reduction (vectorized to a blend/select) with two
    // accumulators selected by the element's sign.
    {p+"_branchred",
     t+" "+p+"_branchred("+t+" a){ unsigned s=(unsigned)a|3u; int buf[72];\n"
     "  for(int i=0;i<72;i++){ s=s*1664525u+1013904223u; buf[i]=(int)s; }\n"
     "  long long pos=0, neg=0;\n"
     "  for(int i=0;i<72;i++){ if(buf[i]>=0) pos += (long long)buf[i];\n"
     "    else neg -= (long long)buf[i]; }\n"
     "  return ("+t+")((pos ^ (pos>>32)) + (neg*7 ^ (neg>>32))); }\n",
     {0x4567u}, "OptStress320", Opt},

    // Fixed-trip mixed-op loop, fully unrolled at -O3; shift/mul/xor chain.
    {p+"_unrollmix",
     t+" "+p+"_unrollmix("+t+" a){ unsigned w=(unsigned)a^0xa5a5u;\n"
     "  long long acc=(long long)(unsigned)a;\n"
     "  for(int i=0;i<32;i++){ w=w*1103515245u+12345u;\n"
     "    acc = acc*0x100000001ull + (long long)(int)w;\n"
     "    acc ^= acc>>29; acc += (long long)((w>>7)&0xff)<<24; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n",
     {0x5678u}, "OptStress320", Opt},

    // Gather from a loop-filled array by a computed index, reduced; the index
    // wraps with a mask so -O3 cannot prove it constant.
    {p+"_gather",
     t+" "+p+"_gather("+t+" a){ unsigned s=(unsigned)a+0x77u; int buf[64];\n"
     "  for(int i=0;i<64;i++){ s=s*1664525u+1013904223u; buf[i]=(int)(s^(s<<7)); }\n"
     "  long long acc=0; unsigned idx=(unsigned)a;\n"
     "  for(int i=0;i<128;i++){ idx=(idx*31u+(unsigned)i)&63u;\n"
     "    acc += (long long)buf[idx]; acc ^= (long long)buf[idx]>>1; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n",
     {0x6789u}, "OptStress320", Opt},
  };
  return v;
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress320TC("x64o320", "long", 3);
static const std::vector<RoundTripTC> kX86 = makeOptStress320TC("x86o320", "int", 3);
static const std::vector<RoundTripTC> kA64 = makeOptStress320TC("a64o320", "long", 3);
static const std::vector<RoundTripTC> kARM = makeOptStress320TC("armo320", "int", 3);

INSTANTIATE_TEST_SUITE_P(OptStress320, X64OptStress320RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress320, X86OptStress320RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress320, A64OptStress320RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress320, ARM32OptStress320RT, ::testing::ValuesIn(kARM), rtTCName);
