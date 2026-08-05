//===- AllPlatform_OptStress210RTTests.cpp - deep optimizer combos ----===//
//
// Roundtrip probes that combine several optimizer stressors in ways the
// earlier OptStress probes exercised only in isolation, to surface latent
// miscompiles in the self-written MedIR optimizer (call-clobber modeling,
// sub-register SSA, PHI placement, flag folding, rodata induction).
//
//   * callsurv - mixed-width (u8/u16/u32/u64) loop-carried accumulators that
//                must survive a noinline call each iteration (forces them into
//                callee-saved registers across the caller-saved clobber).
//   * deepnest - three nested loops, each loop-carried, with an inner early
//                `break` (PHI placement / liveness through a complex CFG).
//   * swfall   - a switch with fallthrough chains (no break) inside a loop
//                driving three accumulators (a non-jump-table dispatch shape).
//   * mulwide  - 32x32->64 widening multiply with BOTH halves consumed and
//                loop-carried (native mul/umull; no libcall on any target).
//   * negmask  - full-width -(cond) masks + __builtin_add_overflow + an
//                unsigned carry flag woven into nested selects (flag folding).
//   * dualchase- two interleaved induction pointers over a rodata table, each
//                with its own wrap reset and a sub-byte read folded by a call.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress210RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress210RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress210RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress210RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress210RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress210RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress210RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress210RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress210TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Mixed-width loop-carried accumulators survive a noinline call each iter.
    {p+"_callsurv",
     "static unsigned "+p+"_step(unsigned v) __attribute__((noinline));\n"
     +t+" "+p+"_callsurv("+t+" x){ unsigned s=(unsigned)x|1u;\n"
     "  unsigned char a8=(unsigned char)x; unsigned short a16=(unsigned short)(x>>2);\n"
     "  unsigned a32=2654435761u; unsigned long long a64=0x9E3779B97F4A7C15ULL;\n"
     "  for(int k=0;k<200;k++){ s=s*1103515245u+12345u;\n"
     "    unsigned c="+p+"_step(s ^ a32);\n"
     "    a8 =(unsigned char)(a8 + c + (s>>5));\n"
     "    a16=(unsigned short)(a16*3u + (c>>2));\n"
     "    a32=(a32 ^ (c + a8)) + a16;\n"
     "    a64=a64*131u + a16 + c; }\n"
     "  return ("+t+")((unsigned)a8 + a16 + a32 + (unsigned)(a64>>32) + (unsigned)a64); }\n"
     "static unsigned "+p+"_step(unsigned v){ v^=v<<13; v^=v>>17; v^=v<<5; return v; }\n",
     {0x21u}, "OptStress210", 2},

    // Three nested loops, each loop-carried, with an inner early break.
    {p+"_deepnest",
     t+" "+p+"_deepnest("+t+" x){ unsigned s=(unsigned)x|1u; int acc=0;\n"
     "  for(int i=0;i<40;i++){ s=s*1103515245u+12345u; int rowi=0;\n"
     "    for(int j=0;j<8;j++){ unsigned tt=s+(unsigned)j*2654435761u; int coll=0;\n"
     "      for(int k=0;k<8;k++){ tt=tt*1103515245u+12345u;\n"
     "        coll+=(int)((tt>>13)&0x7fu);\n"
     "        if((tt & 0x300000u)==0x300000u){ coll-=5; break; } }\n"
     "      rowi=rowi*31+coll; }\n"
     "    acc=acc*131+rowi; acc^=acc>>11; }\n"
     "  return ("+t+")acc; }\n",
     {0x22u}, "OptStress210", 2},

    // Switch with fallthrough chains inside a loop driving three accumulators.
    {p+"_swfall",
     t+" "+p+"_swfall("+t+" x){ unsigned s=(unsigned)x|1u; int a=0,b=0,c=0;\n"
     "  for(int k=0;k<256;k++){ s=s*1103515245u+12345u;\n"
     "    switch((s>>12)&7u){\n"
     "      case 0: a+=3;\n"
     "      case 1: b+=a+1;\n"
     "      case 2: c^=b; break;\n"
     "      case 3: a-=2;\n"
     "      case 4: b+=(int)(s>>20);\n"
     "      case 5: c+=a-b; break;\n"
     "      case 6: a^=c;\n"
     "      default: b-=c; c+=1; break; }\n"
     "    a^=a>>7; b+=c; }\n"
     "  return ("+t+")(a*7+b*5+c*3); }\n",
     {0x23u}, "OptStress210", 2},

    // 32x32->64 widening multiply, both halves consumed and loop-carried.
    {p+"_mulwide",
     t+" "+p+"_mulwide("+t+" x){ unsigned s=(unsigned)x|1u;\n"
     "  unsigned lo=0,hi=0; unsigned long long acc=0;\n"
     "  for(int k=0;k<200;k++){ s=s*1103515245u+12345u;\n"
     "    unsigned a=s ^ lo, b=(s>>7)+hi+1u;\n"
     "    unsigned long long pr=(unsigned long long)a*b;\n"
     "    lo=(unsigned)pr; hi=(unsigned)(pr>>32);\n"
     "    acc=acc*131u + ((unsigned long long)hi<<1) + lo; }\n"
     "  return ("+t+")((unsigned)(acc>>32) ^ (unsigned)acc ^ lo ^ hi); }\n",
     {0x24u}, "OptStress210", 2},

    // -(cond) masks + add-overflow + an unsigned carry woven into selects.
    {p+"_negmask",
     t+" "+p+"_negmask("+t+" x){ unsigned s=(unsigned)x|1u; int acc=0; unsigned carry=0;\n"
     "  for(int k=0;k<200;k++){ s=s*1103515245u+12345u;\n"
     "    int a=(int)s, b=(int)(s>>3); int sum; int ov=__builtin_add_overflow(a,b,&sum);\n"
     "    unsigned m1=(unsigned)-(a<b);\n"
     "    unsigned m2=(unsigned)-((unsigned)a<(unsigned)b);\n"
     "    unsigned ad=(unsigned)a+(unsigned)b; unsigned nc=ad<(unsigned)a;\n"
     "    int sel = ov ? (int)(m1 ^ m2) : (int)(m1 & (unsigned)sum);\n"
     "    sel = nc ? (sel + (int)carry) : (sel ^ (int)carry);\n"
     "    carry=nc; acc=acc*31+sel; acc^=acc>>9; }\n"
     "  return ("+t+")acc; }\n",
     {0x25u}, "OptStress210", 2},

    // Two interleaved induction pointers over a rodata table, sub-byte reads
    // folded by a noinline call, each pointer with its own wrap reset.
    {p+"_dualchase",
     "static unsigned "+p+"_fold(unsigned h, unsigned char v) __attribute__((noinline));\n"
     +t+" "+p+"_dualchase("+t+" x){\n"
     "  static const unsigned char Tb[]={\n"
     "    17,3,251,89,140,5,200,61,7,33,99,128,255,1,44,76,\n"
     "    18,222,90,11,160,73,8,201,52,131,29,240,6,77,118,205,\n"
     "    9,84,167,40,213,2,156,98,31,122,64,190,15,233,50,101 };\n"
     "  unsigned s=(unsigned)x|1u, h=0; const int N=(int)sizeof(Tb);\n"
     "  for(int it=0; it<120; it++){ s=s*1103515245u+12345u;\n"
     "    const unsigned char *pp=Tb+((s>>6)%(unsigned)N);\n"
     "    const unsigned char *qq=Tb+((s>>13)%(unsigned)N);\n"
     "    for(int k=0;k<10;k++){\n"
     "      h="+p+"_fold(h,(unsigned char)(*pp ^ *qq));\n"
     "      pp++; if(pp>=Tb+N) pp=Tb;\n"
     "      qq+=2; if(qq>=Tb+N) qq=Tb; }\n"
     "    h=h*131u+(unsigned)(s>>20); }\n"
     "  return ("+t+")h; }\n"
     "static unsigned "+p+"_fold(unsigned h, unsigned char v){ return h*31u+(unsigned)v; }\n",
     {0x26u}, "OptStress210", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress210TC("x64o210", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress210TC("x86o210", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress210TC("a64o210", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress210TC("armo210", "int");

INSTANTIATE_TEST_SUITE_P(OptStress210, X64OptStress210RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress210, X86OptStress210RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress210, A64OptStress210RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress210, ARM32OptStress210RT, ::testing::ValuesIn(kARM), rtTCName);
