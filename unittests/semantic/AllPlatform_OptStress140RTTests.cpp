//===- AllPlatform_OptStress140RTTests.cpp - isqrt / cont-frac / Horner ==//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Green guardrails for three more rodata access SHAPES, all address-independent
// (the folded result depends only on the bytes in the globals + the control
// flow, never on an absolute VA) and all reached by pure index arithmetic from
// the array base (`tab[idx]`, never an interior pointer), so none touches the
// deferred i386/ARM32 PIC rodata *interior*-pointer model (#477/#487) and every
// probe runs on all four targets.
//
//   * isqrt  - integer square root by Newton's iteration `x=(x+n/x)/2` over
//              rodata-seeded operands.  Pins a self-correcting divide-refine
//              recurrence (distinct from any additive or bit-shift estimate).
//   * cfrac  - continued-fraction expansion: the repeated Euclidean quotient
//              sequence `q=p/d; (p,d)=(d,p%d)` over rodata numerator/denominator
//              pairs.  Pins a quotient-emitting gcd (distinct from the
//              sign-tracking gcd in #128 jacobi).
//   * horner - polynomial evaluation by Horner's rule `r=r*x+c[k]` over rodata
//              coefficients at many points.  Pins a nested multiply-add fold
//              (distinct from the sliding-window convolution in #133).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress140RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress140RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress140RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress140RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress140RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress140RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress140RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress140RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress140TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // integer square root by Newton's iteration over rodata-seeded operands.
    {p+"_isqrt",
     "static const unsigned char "+p+"_n[16]={\n"
     "0x12,0x9a,0x4f,0xe3,0x27,0xb1,0x76,0xc8, 0x3d,0xa5,0x68,0xf0,0x1b,0x84,0x52,0xd9};\n"
     +t+" "+p+"_isqrt("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int q=0;q<32;q++){ unsigned n=((unsigned)"+p+"_n[q&15]<<8)|((s>>(q&7))&0xFFu);\n"
     "      if(n==0u){ acc=acc*131u; continue; }\n"
     "      unsigned x=n, y=(x+1u)/2u; while(y<x){ x=y; y=(x+n/x)/2u; }\n"
     "      acc=acc*131u+x; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x2Au}, "OptStress140", 2},

    // continued-fraction (repeated Euclidean quotient) over rodata num/den pairs.
    {p+"_cfrac",
     "static const unsigned char "+p+"_num[8]={233,144,89,55,178,211,127,199};\n"
     "static const unsigned char "+p+"_den[8]={144,89,55,34,121,98,80,53};\n"
     +t+" "+p+"_cfrac("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int qi=0;qi<16;qi++){ unsigned pp=((unsigned)"+p+"_num[qi&7]<<4)+((s>>(qi&7))&15u)+1u;\n"
     "      unsigned dd=((unsigned)"+p+"_den[qi&7]<<2)+((s>>2)&3u)+1u; unsigned cf=0u;\n"
     "      for(int k=0;k<14 && dd;k++){ unsigned quo=pp/dd, rem=pp%dd;\n"
     "        acc=acc*131u+quo; pp=dd; dd=rem; cf++; }\n"
     "      acc=acc*131u+cf; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x4Cu}, "OptStress140", 2},

    // Horner polynomial evaluation over rodata coefficients at many points.
    {p+"_horner",
     "static const unsigned char "+p+"_co[8]={7,3,11,5,9,2,13,6};\n"
     +t+" "+p+"_horner("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int q=0;q<32;q++){ unsigned x=(s>>(q&15))&15u, r=0u;\n"
     "      for(int k=0;k<8;k++) r=(r*x+("+p+"_co[k]^((s>>(k&7))&1u)))&0xFFFFFu;\n"
     "      acc=acc*131u+r; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x69u}, "OptStress140", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress140TC("x64o140", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress140TC("x86o140", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress140TC("a64o140", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress140TC("armo140", "int");

INSTANTIATE_TEST_SUITE_P(OptStress140, X64OptStress140RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress140, X86OptStress140RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress140, A64OptStress140RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress140, ARM32OptStress140RT, ::testing::ValuesIn(kARM), rtTCName);
