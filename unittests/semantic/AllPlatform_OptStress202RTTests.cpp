//===- AllPlatform_OptStress202RTTests.cpp - mixed optimizer stress ======//
//
// Fresh roundtrip guardrails for self-written MedIR passes / ABI / CFG recovery
// in combinations the existing OptStress probes do not reach.  Every kernel is
// LCG-driven (no constant folding), folds to one integer return, and uses only
// add/sub/mul/shift/divide-by-runtime-value (no float, no 64-bit divide on the
// 32-bit targets, no libcall).  All four targets, -O2.
//
//   * swmulti  - a switch in a loop updating FOUR distinct loop-carried
//                accumulators (one jump table feeding several PHIs at once).
//   * boolsc   - short-circuit boolean chains (&&/||) combining several
//                comparisons that drive branchy accumulation (flag folding).
//   * negidx   - a writable global array walked by a signed (can-go-negative)
//                step with clamp, read-modify-written at a runtime index.
//   * bytepack - bytes assembled into words then re-extracted (sub-word
//                store-to-load forwarding through a local buffer).
//   * clampmix - signed clamp + unsigned clamp + abs + sign in one body
//                (mixed signed/unsigned compare folding, lone sign flag).
//   * divmod   - quotient AND remainder of one runtime division both consumed
//                (one idiv recovered, RDX:RAX / EDX:EAX both read; no libcall).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress202RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress202RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress202RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress202RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress202RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress202RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress202RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress202RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress202TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // A switch in a loop updating four distinct loop-carried accumulators.
    {p+"_swmulti",
     "static const unsigned char "+p+"_sm[24]={\n"
     "0,2,1,3,2,0,3,1,1,2,0,3,2,1,3,0,0,1,2,3,3,2,1,0};\n"
     +t+" "+p+"_swmulti("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, a0=0,a1=1,a2=2,a3=3;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u;\n"
     "    unsigned op="+p+"_sm[it%24], v=s>>8;\n"
     "    switch(op&3u){\n"
     "      case 0: a0+=v; a1^=a0; break;\n"
     "      case 1: a1=(a1<<3)|(a1>>29); a2+=a1; break;\n"
     "      case 2: a2-=v; a3^=a2; break;\n"
     "      default: a3+=a0^a1; a0-=a3; break; } }\n"
     "  return ("+t+")(a0*31u+a1*131u+a2*17u+a3); }\n",
     {0x1234u}, "OptStress202", 2},

    // Short-circuit boolean chains combining several comparisons.
    {p+"_boolsc",
     t+" "+p+"_boolsc("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<160;it++){ s=s*1103515245u+12345u;\n"
     "    int b=(int)(s>>24), c=(int)((s>>16)&0xff);\n"
     "    int d=(int)((s>>8)&0xff), e=(int)(s&0xff);\n"
     "    if((b>c && d<e) || (b==e))      out+=(unsigned)(b+d);\n"
     "    else if(b<c && (d>e || c!=e))   out^=(unsigned)(c*7+e);\n"
     "    else                            out-=(unsigned)(d-b);\n"
     "    out=(out<<1)|(out>>31); }\n"
     "  return ("+t+")out; }\n",
     {0x9e3779b9u}, "OptStress202", 2},

    // A writable global array walked by a signed step (clamped) and RMW'd.
    {p+"_negidx",
     "static int "+p+"_g[32]={\n"
     "3,5,7,11,13,17,19,23,29,31,37,41,43,47,53,59,\n"
     "61,67,71,73,79,83,89,97,101,103,107,109,113,127,131,137};\n"
     +t+" "+p+"_negidx("+t+" a){\n"
     "  unsigned s=(unsigned)a; "+t+" acc=0; int idx=16;\n"
     "  for(int it=0;it<200;it++){ s=s*1103515245u+12345u;\n"
     "    int step=(int)(s%7u)-3;\n"
     "    idx+=step; if(idx<0)idx=0; if(idx>31)idx=31;\n"
     "    "+p+"_g[idx]+=(int)(s>>28);\n"
     "    acc+=("+t+")"+p+"_g[idx]; }\n"
     "  return acc; }\n",
     {0x2468u}, "OptStress202", 2},

    // Bytes assembled into words then re-extracted (sub-word forwarding).
    {p+"_bytepack",
     t+" "+p+"_bytepack("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0; unsigned char buf[8];\n"
     "  for(int it=0;it<128;it++){\n"
     "    for(int k=0;k<8;k++){ s=s*1103515245u+12345u; buf[k]=(unsigned char)(s>>20); }\n"
     "    unsigned w0=(unsigned)buf[0]|((unsigned)buf[1]<<8)|\n"
     "                ((unsigned)buf[2]<<16)|((unsigned)buf[3]<<24);\n"
     "    unsigned w1=(unsigned)buf[4]|((unsigned)buf[5]<<8)|\n"
     "                ((unsigned)buf[6]<<16)|((unsigned)buf[7]<<24);\n"
     "    unsigned mix=w0^(w1*2654435761u);\n"
     "    out+=(mix&0xff)+((mix>>8)&0xff)+((mix>>16)&0xff)+((mix>>24)&0xff);\n"
     "    out=(out<<3)|(out>>29); }\n"
     "  return ("+t+")out; }\n",
     {0x55aau}, "OptStress202", 2},

    // Signed clamp + unsigned clamp + abs + sign in one body.
    {p+"_clampmix",
     t+" "+p+"_clampmix("+t+" a){\n"
     "  unsigned s=(unsigned)a; "+t+" out=0;\n"
     "  for(int it=0;it<160;it++){ s=s*1103515245u+12345u;\n"
     "    int x=(int)s; unsigned u=s>>1;\n"
     "    if(x<-1000)x=-1000; if(x>1000)x=1000;\n"
     "    unsigned uc=u>2000000000u?2000000000u:u;\n"
     "    int m=x<0?-x:x; int sgn=x<0?-1:1;\n"
     "    out+=("+t+")((unsigned)(m*sgn)^uc); }\n"
     "  return out; }\n",
     {0x7fffu}, "OptStress202", 2},

    // Quotient AND remainder of one runtime division both consumed.
    {p+"_divmod",
     t+" "+p+"_divmod("+t+" a){\n"
     "  unsigned s=(unsigned)a; "+t+" out=0;\n"
     "  for(int it=0;it<200;it++){ s=s*1103515245u+12345u;\n"
     "    "+t+" n=("+t+")(int)s; "+t+" d=("+t+")((s&0x3fu)+1u);\n"
     "    "+t+" q=n/d, r=n%d;\n"
     "    out+=q^(r*7); }\n"
     "  return out; }\n",
     {0x13579u}, "OptStress202", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress202TC("x64o202", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress202TC("x86o202", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress202TC("a64o202", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress202TC("armo202", "int");

INSTANTIATE_TEST_SUITE_P(OptStress202, X64OptStress202RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress202, X86OptStress202RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress202, A64OptStress202RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress202, ARM32OptStress202RT, ::testing::ValuesIn(kARM), rtTCName);
