//===- AllPlatform_OptStress321RTTests.cpp - -O3 wide 64-bit ops ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// -O3 arm of the wide-64 family (#157/#311/#441/#518/#524).  On 32-bit targets
// these lower to register-pair (EDX:EAX / R1:R0) carry chains, 64-bit compares
// (cmp;sbb), widening multiply-accumulate and funnel shifts — the densest lift
// bug area historically — but here under -O3's aggressive scheduling/unrolling
// instead of the -O2/-O0 shapes prior probes used.
//
// No 64-bit division (the only i64 op that becomes a libcall on the bare-metal
// harness); deterministic LCG-seeded inputs folded to one return value.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress321RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress321RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress321RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress321RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress321RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress321RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress321RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress321RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress321TC(const char *prefix,
                                                   const char *T, int Opt) {
  std::string p = prefix, t = T;
  std::vector<RoundTripTC> v = {
    // 64-bit add/sub carry-borrow chain across a loop.
    {p+"_carry64",
     t+" "+p+"_carry64("+t+" a){ unsigned w=(unsigned)a|1u;\n"
     "  unsigned long long acc=0x0123456789abcdefull ^ (unsigned)a;\n"
     "  for(int i=0;i<40;i++){ w=w*1103515245u+12345u;\n"
     "    acc += ((unsigned long long)w<<7) + w;\n"
     "    acc -= ((unsigned long long)(w>>3)<<11); }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n",
     {0x1234u}, "OptStress321", Opt},

    // 64-bit signed and unsigned compares feeding a select (cmp;sbb on 32-bit).
    // Widening multiplies only (no general i64*i64) to stay libcall-free.
    {p+"_cmp64",
     t+" "+p+"_cmp64("+t+" a){ unsigned w=(unsigned)a^0x33u; long long acc=0;\n"
     "  for(int i=0;i<48;i++){ w=w*22695477u+1u;\n"
     "    long long x=(long long)(int)w * (long long)(int)(w>>9);\n"
     "    unsigned long long u=(unsigned long long)w * (unsigned long long)(w>>5);\n"
     "    acc += (x<0)? (x*3) : (x>>1);\n"
     "    acc ^= (u > 0x8000000000000000ull)? (long long)u : -(long long)u; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n",
     {0x2345u}, "OptStress321", Opt},

    // 32x32 -> 64 widening multiply-accumulate (signed and unsigned interleaved).
    {p+"_wmac",
     t+" "+p+"_wmac("+t+" a){ unsigned w=(unsigned)a+0x9u;\n"
     "  long long sacc=0; unsigned long long uacc=0;\n"
     "  for(int i=0;i<56;i++){ w=w*1664525u+1013904223u;\n"
     "    int s=(int)w; unsigned u=w>>1;\n"
     "    sacc += (long long)s*(long long)(int)(w>>11);\n"
     "    uacc += (unsigned long long)u*(unsigned long long)(w>>5); }\n"
     "  return ("+t+")((sacc ^ (sacc>>32)) + (long long)(uacc ^ (uacc>>32))); }\n",
     {0x3456u}, "OptStress321", Opt},

    // 64-bit constant funnel shifts + 32-bit variable shifts widened (no i64
    // variable shift, which would become __ashldi3/__lshrdi3 libcalls on 32-bit).
    {p+"_shift64",
     t+" "+p+"_shift64("+t+" a){ unsigned w=(unsigned)a|7u;\n"
     "  unsigned long long acc=0xfeedface12345678ull ^ (unsigned)a;\n"
     "  for(int i=0;i<40;i++){ w=w*1103515245u+12345u; unsigned sh=(w>>3)&31u;\n"
     "    acc = (acc<<13) | (acc>>51);\n"
     "    acc ^= (unsigned long long)(w << sh) | ((unsigned long long)(w >> ((32-sh)&31)) << 32); }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n",
     {0x4567u}, "OptStress321", Opt},

    // 64-bit running min/max (multi-word compare + select).
    {p+"_minmax64",
     t+" "+p+"_minmax64("+t+" a){ unsigned w=(unsigned)a^0xa5u;\n"
     "  long long mn=0x7fffffffffffffffll, mx=-mn-1;\n"
     "  for(int i=0;i<48;i++){ w=w*22695477u+1u;\n"
     "    long long v=((long long)(int)w<<20) ^ (long long)(int)(w>>7);\n"
     "    if(v<mn) mn=v; if(v>mx) mx=v; }\n"
     "  return ("+t+")((mx ^ (mx>>32)) - (mn ^ (mn>>32))); }\n",
     {0x5678u}, "OptStress321", Opt},

    // Pack/unpack 64-bit halves: build i64 from two i32, then extract & recombine.
    {p+"_pack64",
     t+" "+p+"_pack64("+t+" a){ unsigned w=(unsigned)a+0x55u; unsigned long long acc=0;\n"
     "  for(int i=0;i<48;i++){ w=w*1664525u+1013904223u;\n"
     "    unsigned hi=w, lo=w*2654435761u;\n"
     "    unsigned long long packed=((unsigned long long)hi<<32)|lo;\n"
     "    acc += packed; acc ^= (packed>>17)|(packed<<47);\n"
     "    acc += (unsigned long long)(unsigned)(packed>>32) * (unsigned)packed; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n",
     {0x6789u}, "OptStress321", Opt},
  };
  return v;
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress321TC("x64o321", "long", 3);
static const std::vector<RoundTripTC> kX86 = makeOptStress321TC("x86o321", "int", 3);
static const std::vector<RoundTripTC> kA64 = makeOptStress321TC("a64o321", "long", 3);
static const std::vector<RoundTripTC> kARM = makeOptStress321TC("armo321", "int", 3);

INSTANTIATE_TEST_SUITE_P(OptStress321, X64OptStress321RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress321, X86OptStress321RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress321, A64OptStress321RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress321, ARM32OptStress321RT, ::testing::ValuesIn(kARM), rtTCName);
