//===- AllPlatform_OptStress197RTTests.cpp - Kaprekar / happy / Armstrong ==//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Green guardrails for three more rodata access SHAPES.  Each reads its rodata
// through plain forward base+index copies (symbol always referenced at offset 0)
// and folds a result that depends only on the bytes + control flow (never an
// absolute VA), so nothing touches the deferred i386/ARM32 PIC rodata
// *interior*-pointer model (#477/#487); every probe runs on all four targets.
// Every digit split uses a COMPILE-TIME-CONSTANT base (/10, %10, ...), which
// clang lowers to a magic multiply, so no division libcall is emitted.
//
//   * kaprekar  - Kaprekar 6174 routine: sort the four digits up and down, take
//                 the difference, and iterate to the 6174 fixed point, counting
//                 steps.  Pins a digit-sort/subtract fixed-point loop.
//   * happy     - happy-number classification: iterate sum-of-squared-digits and
//                 detect arrival at 1 (happy) or 4 (the unhappy cycle).  Pins a
//                 digit-square recurrence with a known-cycle terminator.
//   * armstrong - narcissistic-number test: a 3-digit value equals the sum of the
//                 cubes of its digits.  Pins a digit-power equality fold.
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress197RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress197RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress197RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress197RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress197RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress197RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress197RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress197RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress197TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Kaprekar 6174 routine: digit-sort/subtract iteration with step count.
    {p+"_kaprekar",
     "static const unsigned char "+p+"_kp[8]={37,12,58,4,29,61,7,44};\n"
     +t+" "+p+"_kaprekar("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned n=1000u+(((((unsigned)"+p+"_kp[s&7u])<<6)^s)%9000u);\n"
     "    unsigned cur=n, steps=0u, last=0u;\n"
     "    for(int iter=0; iter<12 && cur!=6174u; iter++){\n"
     "      unsigned dd[4]; dd[0]=cur%10u; dd[1]=(cur/10u)%10u; dd[2]=(cur/100u)%10u; dd[3]=(cur/1000u)%10u;\n"
     "      for(int i=0;i<4;i++) for(int j=i+1;j<4;j++) if(dd[j]<dd[i]){ unsigned tt=dd[i];dd[i]=dd[j];dd[j]=tt; }\n"
     "      unsigned asc=dd[0]*1000u+dd[1]*100u+dd[2]*10u+dd[3];\n"
     "      unsigned desc=dd[3]*1000u+dd[2]*100u+dd[1]*10u+dd[0];\n"
     "      cur=desc-asc; steps++; last=cur; if(cur==0u) break; }\n"
     "    acc=acc*131u+steps*131u+cur+last; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x71u}, "OptStress197", 2},

    // happy-number classification via sum-of-squared-digits (1 vs cycle-4).
    {p+"_happy",
     "static const unsigned char "+p+"_hp[8]={23,77,19,44,7,61,2,88};\n"
     +t+" "+p+"_happy("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned n=1u+(((((unsigned)"+p+"_hp[s&7u])<<3)^s)%998u);\n"
     "    unsigned cur=n; int happy=0; unsigned steps=0u;\n"
     "    for(int iter=0; iter<50; iter++){ unsigned x=cur, sum=0u;\n"
     "      while(x>0u){ unsigned d=x%10u; sum+=d*d; x/=10u; }\n"
     "      cur=sum; steps++;\n"
     "      if(cur==1u){ happy=1; break; } if(cur==4u){ happy=0; break; } }\n"
     "    acc=acc*131u+(unsigned)happy*1311u+cur+steps+n; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x72u}, "OptStress197", 2},

    // narcissistic (Armstrong) test: 3-digit value vs sum of digit cubes.
    {p+"_armstrong",
     "static const unsigned char "+p+"_am[8]={31,90,7,52,18,66,3,44};\n"
     +t+" "+p+"_armstrong("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned n=100u+(((((unsigned)"+p+"_am[s&7u])<<2)^s)%900u);\n"
     "    unsigned x=n, sum=0u;\n"
     "    while(x>0u){ unsigned d=x%10u; sum+=d*d*d; x/=10u; }\n"
     "    unsigned isArm=(sum==n)?1u:0u;\n"
     "    acc=acc*131u+isArm*1311u+sum+n; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x73u}, "OptStress197", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress197TC("x64o197", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress197TC("x86o197", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress197TC("a64o197", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress197TC("armo197", "int");

INSTANTIATE_TEST_SUITE_P(OptStress197, X64OptStress197RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress197, X86OptStress197RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress197, A64OptStress197RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress197, ARM32OptStress197RT, ::testing::ValuesIn(kARM), rtTCName);
