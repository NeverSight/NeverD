//===- AllPlatform_OptStress335RTTests.cpp - MedFlags carry / cross-block -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Adjacent MedFlags guardrails beyond OptStress318 (same-block mixed-width
// compare clusters) and OptStress328 (single-compare fanout).  Targets shapes
// where the flag-reconstruction pass must keep the RIGHT comparison bound to
// each consumer across control-flow joins and carry-chain survival:
//
//   * _xblockcmp : compares live in DIFFERENT if/else arms, merged through a
//                  PHI-like boolean, then consumed again after intervening math
//                  — cross-block compare → select binding (#147/#161 family).
//   * _adccmov   : unsigned-overflow (carry) from a widening add drives a
//                  select, then feeds the next loop-carried add — carry flag must
//                  not be markFlagChainDead-killed while still feeding CMOV
//                  (#431 markFlagChainDead family).
//   * _wide96    : explicit 96-bit add as three 32-bit limbs with per-limb carry
//                  — multi-step CF propagation without __builtin_* overflow.
//   * _ofpick    : classic signed-overflow predicate
//                  `((a^b)>=0 && (a^(a+b))<0)` driving selects (OF / V flag path)
//                  interleaved with an unrelated unsigned compare in the same
//                  iteration (integer vs overflow flag disambiguation).
//
// i64 accumulators use multiply/shift/xor/add with CONSTANT shifts only
// (libcall-free on i386/ARM32).  Deterministic LCG inputs, -O2, all four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress335RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress335RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress335RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress335RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress335RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress335RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress335RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress335RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress335TC(const char *prefix,
                                                   const char *T, int Opt) {
  std::string p = prefix, t = T;
  std::vector<RoundTripTC> v = {
    // Compare in different arms, boolean merged, consumed again after math.
    {p+"_xblockcmp",
     t+" "+p+"_xblockcmp("+t+" a){ unsigned w=(unsigned)a|1u;\n"
     "  long long acc=(long long)(unsigned)a;\n"
     "  for(int i=0;i<48;i++){ w=w*1103515245u+12345u;\n"
     "    int x=(int)w, y=(int)(w>>7); int less;\n"
     "    if(w&3u){ less=(x<y); acc += less ? (long long)x : (long long)y; }\n"
     "    else    { less=((unsigned)x<(unsigned)y); acc += less ? (long long)x*2 : (long long)y*2; }\n"
     "    acc += less ? 7 : 11; acc = acc*131 + (less?3:5); acc ^= acc>>17; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n",
     {0x1234u}, "OptStress335", Opt},

    // Carry from widening add drives a select, then the next add/sub step.
    {p+"_adccmov",
     t+" "+p+"_adccmov("+t+" a){ unsigned w=(unsigned)a^0x5Au;\n"
     "  unsigned long long x=(unsigned long long)(unsigned)a, y=~x; unsigned c=0;\n"
     "  long long acc=(long long)(unsigned)a;\n"
     "  for(int i=0;i<56;i++){ w=w*22695477u+1u;\n"
     "    unsigned long long prev=x, d=((unsigned long long)w<<17)^(unsigned long long)(w>>3);\n"
     "    x=prev+d; int carry=(x<prev);\n"
     "    long long bump = carry ? (long long)d : -(long long)d;\n"
     "    acc += bump + (long long)carry*13;\n"
     "    if(carry){ y+=x; c++; } else { y-=x; c--; }\n"
     "    acc ^= acc>>19; }\n"
     "  return ("+t+")((long long)(x^y^c) + (acc ^ (acc>>32))); }\n",
     {0x2345u}, "OptStress335", Opt},

    // 96-bit add as three 32-bit limbs with explicit carry propagation.
    {p+"_wide96",
     t+" "+p+"_wide96("+t+" a){ unsigned w=(unsigned)a+0x9u;\n"
     "  unsigned lo=(unsigned)a, mid=w^0xdeadbeefu, hi=(unsigned)a^0xa5a5a5a5u;\n"
     "  for(int i=0;i<40;i++){ w=w*1664525u+1013904223u;\n"
     "    unsigned a0=(unsigned)w, a1=(unsigned)(w>>11), a2=(unsigned)(w>>19);\n"
     "    unsigned s0=lo+a0, c0=(s0<lo); lo=s0;\n"
     "    unsigned s1=mid+a1+c0, c1=(s1<mid)||(c0 && s1==mid); mid=s1;\n"
     "    unsigned s2=hi+a2+c1; hi=s2; }\n"
     "  return ("+t+")(lo ^ mid ^ hi); }\n",
     {0x3456u}, "OptStress335", Opt},

    // Signed-overflow predicate selects interleaved with an unsigned compare.
    {p+"_ofpick",
     t+" "+p+"_ofpick("+t+" a){ unsigned w=(unsigned)a|1u;\n"
     "  long long acc=(long long)(unsigned)a;\n"
     "  for(int i=0;i<44;i++){ w=w*1103515245u+12345u;\n"
     "    int x=(int)(w>>3), y=(int)(w>>11);\n"
     "    int sum=x+y;\n"
     "    int ov=((x^y)>=0 && (x^sum)<0);\n"
     "    int ug=((unsigned)x<(unsigned)y);\n"
     "    acc += ov ? (long long)sum*3 : (long long)sum;\n"
     "    acc += ug ? 17 : 19;\n"
     "    acc += ov^ug ? (long long)x : (long long)y;\n"
     "    acc = acc*1000003 + (ov?7:11); acc ^= acc>>21; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n",
     {0x4567u}, "OptStress335", Opt},
  };
  return v;
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress335TC("x64o335", "long", 2);
static const std::vector<RoundTripTC> kX86 = makeOptStress335TC("x86o335", "int", 2);
static const std::vector<RoundTripTC> kA64 = makeOptStress335TC("a64o335", "long", 2);
static const std::vector<RoundTripTC> kARM = makeOptStress335TC("armo335", "int", 2);

INSTANTIATE_TEST_SUITE_P(OptStress335, X64OptStress335RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress335, X86OptStress335RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress335, A64OptStress335RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress335, ARM32OptStress335RT, ::testing::ValuesIn(kARM), rtTCName);
