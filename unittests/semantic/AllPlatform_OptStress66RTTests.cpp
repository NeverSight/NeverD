//===- AllPlatform_OptStress66RTTests.cpp - ABI x FP-scratch ----*-C++*-=//
//
// Calling-convention corners around the #468 i386 scratch-XMM-vs-parameter
// fix: integer/pointer-argument functions whose bodies use the vector register
// file as FP scratch, interleaved with static helpers that DO take FP
// parameters (clang's internal SSE/AAPCS convention).  Stresses the parameter
// recovery's ability to keep genuine FP params while dropping scratch vector
// registers, across argument counts and int/FP interleavings, on all four
// targets.
//
//   * scratch5 - int function, 5 int args, heavy XMM/V scratch FP work.
//   * fphelper - caller passes mixed int+double to a static FP-param helper
//                in an FP-accumulator loop (regression for the #469 fix).
//   * twohelp  - int + FP dual-accumulator loop calling an int and an FP helper
//                (regression for the #469 fix).
//   * fpaccloop- int args + FP accumulator loop (scratch FP, no FP param).
//   * intfpmix - interleave int math and FP scratch sharing the vector bank.
//   * ptrfp    - pointer(rodata) arg walk + per-element FP scratch reduction.
//
// Each kernel folds to one integer return; no libm (Newton for sqrt), no
// 64-bit divide helper.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress66RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress66RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress66RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress66RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress66RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress66RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress66RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress66RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress66TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // int function, 5 int args, heavy XMM/V scratch FP work (no FP param).
    {p+"_scratch5",
     t+" "+p+"_scratch5("+t+" a,"+t+" b,"+t+" c,"+t+" d,"+t+" e){\n"
     "  double acc=0; unsigned s=(unsigned)(a+b+c+d+e);\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    double x=((double)((s>>8)&0xff))-128.0;\n"
     "    double y=((double)((s>>16)&0xff))-128.0;\n"
     "    acc += x*y - x*0.5 + y*0.25; }\n"
     "  return ("+t+")(long long)(acc) + a - b + c - d + e; }\n",
     {0x10u,0x20u,0x30u,0x40u,0x50u}, "OptStress66", 2},

    // Caller passes mixed int+double to a static FP-param helper.
    {p+"_fphelper",
     "static double "+p+"_hm(double,int,double) __attribute__((noinline));\n"
     +t+" "+p+"_fphelper("+t+" a){ unsigned s=(unsigned)a; double acc=0;\n"
     "  for(int i=0;i<150;i++){ s=s*1103515245u+12345u;\n"
     "    double x=((double)((s>>9)&0xff))*0.5; int k=(int)((s>>4)&7);\n"
     "    acc += "+p+"_hm(x, k, x*0.25+1.0); }\n"
     "  return ("+t+")(long long)acc; }\n"
     "static double "+p+"_hm(double a,int k,double c){ return a*c + (double)k*a - c; }\n",
     {0x37u}, "OptStress66", 2},

    // Two static helpers with different FP/int signatures: an int + FP dual-
    // accumulator loop (regression for the #469 fix — the int return must not be
    // dropped when the caller zeroes XMM0 with `xorps x,x` for its own FP work).
    {p+"_twohelp",
     "static int "+p+"_hi(int,int) __attribute__((noinline));\n"
     "static double "+p+"_hd(double,double,double) __attribute__((noinline));\n"
     +t+" "+p+"_twohelp("+t+" a){ unsigned s=(unsigned)a; double acc=0; int iacc=0;\n"
     "  for(int i=0;i<150;i++){ s=s*1103515245u+12345u;\n"
     "    iacc += "+p+"_hi((int)(s>>3)&0xff,(int)(s>>11)&0xff);\n"
     "    acc += "+p+"_hd((double)(s&0xff),(double)((s>>8)&0xff),0.5); }\n"
     "  return ("+t+")((long long)acc + iacc); }\n"
     "static int "+p+"_hi(int a,int b){ return a*b - a + b; }\n"
     "static double "+p+"_hd(double a,double b,double c){ return a*b*c + a - b; }\n",
     {0x2Bu}, "OptStress66", 2},

    // int args + FP accumulator loop (scratch FP, no FP param).
    {p+"_fpaccloop",
     t+" "+p+"_fpaccloop("+t+" a,"+t+" b){\n"
     "  unsigned s=(unsigned)(a^b); double acc=1.0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    double v=((double)((s>>10)&0x3f))+1.0;\n"
     "    acc = acc*0.5 + v*0.125; if(acc>1000.0) acc=acc-1000.0; }\n"
     "  return ("+t+")(long long)(acc*64.0) + a*b; }\n",
     {0x15u,0x26u}, "OptStress66", 2},

    // Interleave int math and FP scratch sharing the vector bank.
    {p+"_intfpmix",
     t+" "+p+"_intfpmix("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0; double f=0;\n"
     "  for(int i=0;i<250;i++){ s=s*1103515245u+12345u;\n"
     "    h = h*131u + ((s>>7)&0xffff);\n"
     "    f = f + ((double)(h&0xff))*0.5 - ((double)((s>>3)&0xff))*0.25;\n"
     "    h ^= (unsigned)(long long)f; h^=h>>13; }\n"
     "  return ("+t+")(h + (unsigned)(long long)f); }\n",
     {0x44u}, "OptStress66", 2},

    // Pointer(rodata) arg walk + per-element FP scratch reduction.  The pointer
    // is materialized inside (so the harness needs no pointer argument) and the
    // body mixes a rodata walk with FP scratch.
    {p+"_ptrfp",
     "static const unsigned char DAT[24]={5,9,2,7,3,8,1,6,4,11,12,10,13,15,"
     "14,0,17,19,16,18,21,23,20,22};\n"
     +t+" "+p+"_ptrfp("+t+" a){\n"
     "  unsigned s=(unsigned)a; double acc=0; unsigned h=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    const unsigned char *q=DAT+((s>>5)%12u);\n"
     "    double x=(double)q[0]+(double)q[1]*0.5+(double)q[2]*0.25;\n"
     "    acc += x*x*0.01; h=h*131u+q[0]+q[1]+q[2]; }\n"
     "  return ("+t+")((long long)acc + h); }\n",
     {0x5Cu}, "OptStress66", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress66TC("x64o66", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress66TC("x86o66", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress66TC("a64o66", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress66TC("armo66", "int");

INSTANTIATE_TEST_SUITE_P(OptStress66, X64OptStress66RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress66, X86OptStress66RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress66, A64OptStress66RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress66, ARM32OptStress66RT, ::testing::ValuesIn(kARM), rtTCName);
