//===- AllPlatform_OptStress29RTTests.cpp - opt-stress probes --*-C++*-=//
//
// Targets self-written MedIR pass interactions OptStress27/28 did not cover:
// narrow (8/16-bit) values round-tripped through memory then sign/zero-extended
// (store-load forwarding + sub-register width), cross-loop flag liveness (an
// inner loop's exit flag reused by the outer accumulation), multiple early-
// return points returning narrow values extended differently per path, per-byte
// carry ripples driven by conditionals, and switch dispatch in a loop updating
// mixed-width accumulators (MedCFGSimplify + sub-register phis).
//
//   * mixwidthmem - store i32, reload bytes/halfword, sign-extend, recombine.
//   * nestedflag  - inner-loop exit flag reused by the outer loop.
//   * earlyret    - multiple early returns of 8/16-bit values, mixed extension.
//   * bytecarry   - BCD-style per-digit increment with conditional carry.
//   * dispatchacc - switch-in-loop updating 8/16/32-bit accumulators.
//   * sextcmp     - chain of i8->i16->i32 sign-extensions feeding signed compares.
//
// Integer-only, single integer return, no 64-bit divide; all four targets -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress29RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress29RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress29RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress29RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress29RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress29RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress29RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress29RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress29TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Store a word to a local, reload its bytes and halfword with mixed
    // signedness, sign/zero-extend, recombine: store-load forwarding + sub-reg.
    {p+"_mixwidthmem",
     t+" "+p+"_mixwidthmem("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, h=0; unsigned char buf[4];\n"
     "  for(int i=0;i<80;i++){ s=s*1103515245u+12345u;\n"
     "    buf[0]=(unsigned char)s; buf[1]=(unsigned char)(s>>8);\n"
     "    buf[2]=(unsigned char)(s>>16); buf[3]=(unsigned char)(s>>24);\n"
     "    int sb=(signed char)buf[0]; int sh=(short)(buf[1]|(buf[2]<<8));\n"
     "    unsigned ub=buf[3]; \n"
     "    h=h*131u+(unsigned)(sb*3+sh)-ub; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x41ULL}, "OptStress29", 2},

    // Inner loop computes a flag (found?) reused by the outer accumulation, the
    // count of inner iterations also carried out (cross-loop flag liveness).
    {p+"_nestedflag",
     t+" "+p+"_nestedflag("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<40;i++){ s=s*1103515245u+12345u; unsigned target=s&0x3fu;\n"
     "    int found=0, cnt=0; unsigned t=s;\n"
     "    for(int j=0;j<32 && !found;j++){ t=t*1664525u+1013904223u; cnt++;\n"
     "      if((t&0x3fu)==target) found=1; }\n"
     "    if(found) h+=cnt*7u; else h-=cnt; h^=(unsigned)found<<11; h=h*131u+t; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x53ULL}, "OptStress29", 2},

    // Multiple early returns of 8/16-bit values, each extended differently.
    {p+"_earlyret",
     "static int "+p+"_er(unsigned x){\n"
     "  if((x&3u)==0) return (signed char)(x>>4);\n"
     "  if((x&3u)==1) return (unsigned char)(x>>5);\n"
     "  if((x&3u)==2) return (short)(x>>3);\n"
     "  return (int)(unsigned short)(x>>7); }\n"
     +t+" "+p+"_earlyret("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<96;i++){ s=s*1103515245u+12345u;\n"
     "    h=h*131u+(unsigned)"+p+"_er(s); }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x6dULL}, "OptStress29", 2},

    // BCD-style per-digit increment with conditional carry (8-bit ripples).
    {p+"_bytecarry",
     t+" "+p+"_bytecarry("+t+" a){\n"
     "  unsigned char d[6]={0}; unsigned s=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u; unsigned inc=(s&7u)+1u;\n"
     "    int k=0; unsigned carry=inc;\n"
     "    while(carry && k<6){ unsigned v=(unsigned)d[k]+carry;\n"
     "      d[k]=(unsigned char)(v%10u); carry=v/10u; k++; }\n"
     "    unsigned acc=0; for(int j=5;j>=0;j--) acc=acc*10u+d[j];\n"
     "    h=h*131u+acc; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0xb2ULL}, "OptStress29", 2},

    // switch-in-loop updating 8/16/32-bit accumulators (sub-register phis).
    {p+"_dispatchacc",
     t+" "+p+"_dispatchacc("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned char a8=0; unsigned short a16=0; unsigned a32=0;\n"
     "  for(int i=0;i<120;i++){ s=s*1103515245u+12345u;\n"
     "    switch(s&3u){\n"
     "      case 0: a8=(unsigned char)(a8+(s>>3)); break;\n"
     "      case 1: a16=(unsigned short)(a16*3u+(s>>5)); break;\n"
     "      case 2: a32=a32^(s>>1); break;\n"
     "      default: a8=(unsigned char)(a8^(s>>7)); a16=(unsigned short)(a16+a8); break; }\n"
     "  }\n"
     "  return ("+t+")(unsigned)((unsigned)a8 + (unsigned)a16*131u + a32*7u); }\n",
     {0x18ULL}, "OptStress29", 2},

    // i8 -> i16 -> i32 sign-extension chain feeding signed comparisons.
    {p+"_sextcmp",
     t+" "+p+"_sextcmp("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, h=0; int run=0;\n"
     "  for(int i=0;i<96;i++){ s=s*1103515245u+12345u;\n"
     "    signed char c=(signed char)s; short w=(short)((int)c*3+(short)(s>>8));\n"
     "    int e=(int)w - (int)c;\n"
     "    if(c<0 && w<0) run++; else run=0;\n"
     "    if(e< -100) h+=(unsigned)(-e); else if(e>100) h+=(unsigned)e; else h^=(unsigned)e;\n"
     "    h=h*131u+(unsigned)run; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x2fULL}, "OptStress29", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress29TC("x64o29", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress29TC("x86o29", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress29TC("a64o29", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress29TC("armo29", "int");

INSTANTIATE_TEST_SUITE_P(OptStress29, X64OptStress29RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress29, X86OptStress29RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress29, A64OptStress29RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress29, ARM32OptStress29RT, ::testing::ValuesIn(kARM), rtTCName);
