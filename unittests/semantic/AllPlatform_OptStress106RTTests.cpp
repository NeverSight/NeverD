//===- AllPlatform_OptStress106RTTests.cpp - field / DSP rodata shapes -----==//
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
//   * gf16   - GF(16) multiply via rodata log + exp tables: the index is the
//              SUM of two table reads reduced `% 15` (magic-number division, no
//              libcall), then an exp gather, guarded by `(x&&y)`.  Pins a log-
//              domain field multiply (two-table summed index + modulo + gather).
//   * fir    - 1D FIR convolution: a sliding length-8 window `sig[i+j]` of a
//              rodata signal dotted with a rodata kernel `krn[j]`.  Pins a
//              forward windowed dot product reading two rodata arrays.
//   * bezier - integer quadratic Bezier blend over rodata control-point triples
//              `(u*u*P0 + 2*u*t*P1 + t*t*P2)>>16`.  Pins an adjacent-triple read
//              plus an all-integer second-order polynomial evaluation.
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress106RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress106RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress106RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress106RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress106RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress106RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress106RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress106RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress106TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // GF(16) multiply via rodata log/exp tables: summed index, % 15, exp gather.
    {p+"_gf16",
     "static const unsigned char "+p+"_glog[16]={0,0,1,4,2,8,5,10,3,14,9,7,6,13,11,12};\n"
     "static const unsigned char "+p+"_gexp[16]={1,2,4,8,3,6,12,11,5,10,7,14,15,13,9,1};\n"
     +t+" "+p+"_gf16("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int k=0;k<32;k++){\n"
     "      unsigned x=(s>>(k&15))&15u, y=(s>>((k+5)&15))&15u;\n"
     "      unsigned prod=(x&&y)?"+p+"_gexp[("+p+"_glog[x]+"+p+"_glog[y])%15u]:0u;\n"
     "      acc=acc*131u+prod; acc^=acc>>4; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x16u}, "OptStress106", 2},

    // 1D FIR convolution: sliding rodata signal window dotted with rodata kernel.
    {p+"_fir",
     "static const unsigned char "+p+"_sig[48]={\n"
     "5,12,30,18,7,44,21,9, 33,16,52,3,27,14,40,8, 19,38,2,25,11,47,6,31,\n"
     "13,49,20,4,37,10,55,17, 29,41,1,26,53,22,15,48, 9,34,23,50,7,42,18,36};\n"
     "static const unsigned char "+p+"_krn[8]={1,3,5,7,7,5,3,1};\n"
     +t+" "+p+"_fir("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=0;\n"
     "    for(int i=0;i+8<=48;i++){ unsigned sum=0;\n"
     "      for(int j=0;j<8;j++) sum+=("+p+"_sig[i+j]^((s>>(j&7))&1u))*"+p+"_krn[j];\n"
     "      acc=acc*131u+sum; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xF1u}, "OptStress106", 2},

    // integer quadratic Bezier blend over rodata control-point triples.
    {p+"_bezier",
     "static const unsigned char "+p+"_ctrl[24]={\n"
     "10,200,40, 60,20,180, 5,150,90, 120,30,210,\n"
     "80,170,15, 35,95,140, 220,25,75, 50,130,160};\n"
     +t+" "+p+"_bezier("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int c=0;c<8;c++){ unsigned p0="+p+"_ctrl[c*3], p1="+p+"_ctrl[c*3+1], p2="+p+"_ctrl[c*3+2];\n"
     "      for(int tk=0;tk<=8;tk++){ unsigned tt=(unsigned)tk*32u, u=256u-tt;\n"
     "        unsigned b=(u*u*p0+2u*u*tt*p1+tt*tt*p2)>>16;\n"
     "        acc=acc*131u+b; } }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xBEu}, "OptStress106", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress106TC("x64o106", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress106TC("x86o106", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress106TC("a64o106", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress106TC("armo106", "int");

INSTANTIATE_TEST_SUITE_P(OptStress106, X64OptStress106RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress106, X86OptStress106RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress106, A64OptStress106RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress106, ARM32OptStress106RT, ::testing::ValuesIn(kARM), rtTCName);
