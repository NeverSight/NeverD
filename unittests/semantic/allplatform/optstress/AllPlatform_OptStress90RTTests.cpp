//===- AllPlatform_OptStress90RTTests.cpp - 64-bit pair / spill probes -C++-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Corners the prior probes under-hit: 64-bit comparison / select / min-max
// lowering (a REGISTER PAIR on the 32-bit targets) and heavy register pressure
// forcing spill/reload, all feeding control flow so the lifted flag/spill model
// must match bit-for-bit.
//
//   * wcmp64  - signed AND unsigned 64-bit comparison chains driving branches:
//               on i386/ARM32 a `cmp hi; sbb` register-pair compare whose carry/
//               sign result selects a path.
//   * wsel64  - 64-bit conditional select / min / max / abs (a cmov pair / csel
//               pair) carried across a loop.
//   * manylive- many simultaneous live 32-bit values (>register budget) forcing
//               spills and reloads, reassembled every iteration.
//   * deepcond- deeply nested signed/unsigned comparisons producing setcc/cmov
//               that the flag pass must fold without a stale flag.
//
// All integer in / integer out, self-contained, no libcall.  All four targets,
// -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress90RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress90RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress90RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress90RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress90RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress90RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress90RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress90RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress90TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Signed + unsigned 64-bit comparison chains (register-pair cmp/sbb on 32-bit).
    {p+"_wcmp64",
     t+" "+p+"_wcmp64("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; long long acc=(long long)a;\n"
     "  unsigned long long u=(unsigned long long)a^0xF0E1D2C3B4A59687ULL;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u;\n"
     "    long long x=(long long)(((unsigned long long)s<<32)|(s^0x55u));\n"
     "    unsigned long long y=((unsigned long long)(s>>3)<<31)|(s>>1);\n"
     "    if(x<acc) acc=acc-x; else acc=acc+(x>>1);\n"
     "    if(u<y) u=y-u; else u=u^(y>>2);\n"
     "    acc^=(long long)(u>>17); }\n"
     "  return ("+t+")(acc^(long long)u); }\n",
     {0x90u}, "OptStress90", 2},

    // 64-bit conditional select / min / max / abs carried across the loop.
    {p+"_wsel64",
     t+" "+p+"_wsel64("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u;\n"
     "  long long lo=(long long)a, hi=~(long long)a;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u;\n"
     "    long long v=(long long)(((unsigned long long)(s*2654435761u)<<32)|s);\n"
     "    long long mn=(v<lo)?v:lo, mx=(v>hi)?v:hi;\n"
     "    long long ab=(v<0)?-v:v;\n"
     "    lo=mn^(ab>>5); hi=mx+(ab>>9);\n"
     "    lo^=lo>>13; hi^=hi<<7; }\n"
     "  return ("+t+")(lo^hi); }\n",
     {0x91u}, "OptStress90", 2},

    // Many simultaneous live values forcing spill/reload, reassembled per loop.
    {p+"_manylive",
     t+" "+p+"_manylive("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u;\n"
     "  unsigned r0=s,r1=s^1,r2=s^2,r3=s^3,r4=s^4,r5=s^5,r6=s^6,r7=s^7;\n"
     "  unsigned r8=s+8,r9=s+9,ra=s+10,rb=s+11,rc=s+12,rd=s+13,re=s+14,rf=s+15;\n"
     "  for(int i=0;i<96;i++){ s=s*1103515245u+12345u;\n"
     "    r0+=r1^s; r1+=r2; r2+=r3; r3+=r4; r4+=r5; r5+=r6; r6+=r7; r7+=r8;\n"
     "    r8+=r9; r9+=ra; ra+=rb; rb+=rc; rc+=rd; rd+=re; re+=rf; rf+=r0^s;\n"
     "    r0^=r8>>3; r4^=rc<<2; r2^=ra>>1; r6^=re<<1; }\n"
     "  return ("+t+")(r0+r1+r2+r3+r4+r5+r6+r7+r8+r9+ra+rb+rc+rd+re+rf); }\n",
     {0x92u}, "OptStress90", 2},

    // Deeply nested signed/unsigned comparisons (setcc/cmov flag folding).
    {p+"_deepcond",
     t+" "+p+"_deepcond("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int acc=(int)a;\n"
     "  for(int i=0;i<160;i++){ s=s*1103515245u+12345u;\n"
     "    int x=(int)(s>>4), y=(int)(s>>9); unsigned u=s>>6;\n"
     "    int r;\n"
     "    if(x<y){ if(u>1000u) r=x+y; else r=(x<0)?-x:y; }\n"
     "    else { if((unsigned)x<u) r=x^y; else r=(y>x)?y:(x-y); }\n"
     "    acc=(acc>r)?(acc-r):(acc+r); acc^=acc>>7; }\n"
     "  return ("+t+")acc; }\n",
     {0x93u}, "OptStress90", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress90TC("x64o90", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress90TC("x86o90", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress90TC("a64o90", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress90TC("armo90", "int");

INSTANTIATE_TEST_SUITE_P(OptStress90, X64OptStress90RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress90, X86OptStress90RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress90, A64OptStress90RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress90, ARM32OptStress90RT, ::testing::ValuesIn(kARM), rtTCName);
