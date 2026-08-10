//===- AllPlatform_OptStress110RTTests.cpp - adler / median / lerp shapes --==//
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
//   * adler  - Adler-32 style rolling checksum over a rodata buffer: two coupled
//              accumulators reduced by a CONSTANT modulus (`A=(A+b)%65521;
//              B=(B+A)%65521`).  Pins a two-accumulator modular recurrence whose
//              divisor folds to a magic multiply (no libcall) on all targets.
//   * med3   - 3-tap median filter over a sliding rodata window: the median of
//              each adjacent triple via min/max compares
//              (`max(min(x,y),min(max(x,y),z))`).  Pins an adjacent-triple read
//              plus a branchy order-statistic selection over rodata.
//   * interp - piecewise-linear interpolation on a uniform-grid rodata knot
//              table: a query selects a segment (`xq>>4`) and integer-lerps the
//              two neighbouring rodata samples `y[seg],y[seg+1]`.  Pins a
//              segment index + adjacent-pair rodata read + integer blend.
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress110RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress110RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress110RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress110RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress110RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress110RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress110RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress110RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress110TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Adler-32 style two-accumulator rolling checksum (constant modulus).
    {p+"_adler",
     "static const unsigned char "+p+"_data[48]={\n"
     "0x57,0x12,0xab,0x6e,0xc3,0x09,0xf4,0x82, 0x35,0xd8,0x4b,0x90,0x1f,0x76,0xe2,0x0c,\n"
     "0xbd,0x48,0x91,0x2a,0xdf,0x63,0x05,0x9c, 0x7a,0xe6,0x31,0xc8,0x54,0x0f,0xa3,0x6d,\n"
     "0x18,0x85,0x29,0xf1,0x4c,0xd0,0x67,0x3b, 0xbe,0x02,0x99,0x5a,0xe7,0x14,0x88,0x23};\n"
     +t+" "+p+"_adler("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned A=1u, B=0u;\n"
     "    for(int i=0;i<48;i++){ unsigned b=("+p+"_data[i]^(s>>(i&7)))&0xFFu;\n"
     "      A=(A+b)%65521u; B=(B+A)%65521u; }\n"
     "    acc=acc*131u+((B<<16)|A);\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xADu}, "OptStress110", 2},

    // 3-tap median filter over a sliding rodata window (order statistic).
    {p+"_med3",
     "static const unsigned char "+p+"_sig[48]={\n"
     "30,12,75,44,8,61,27,90, 5,53,18,99,36,71,2,84, 49,14,67,23,95,40,7,58,\n"
     "11,80,33,62,19,88,4,51, 26,73,9,97,42,15,69,3, 56,21,85,38,64,10,77,47};\n"
     +t+" "+p+"_med3("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int i=0;i+3<=48;i++){\n"
     "      unsigned x=("+p+"_sig[i]^(s&0xFFu))&0xFFu;\n"
     "      unsigned y=("+p+"_sig[i+1]^((s>>8)&0xFFu))&0xFFu;\n"
     "      unsigned z=("+p+"_sig[i+2]^((s>>16)&0xFFu))&0xFFu;\n"
     "      unsigned mx=x>y?x:y, mn=x<y?x:y;\n"
     "      unsigned med=z>mx?mx:(z<mn?mn:z);\n"
     "      acc=acc*131u+med; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x33u}, "OptStress110", 2},

    // piecewise-linear interpolation on a uniform-grid rodata knot table.
    {p+"_interp",
     "static const unsigned char "+p+"_yv[17]={\n"
     "10,35,28,60,52,90,75,120, 105,150,130,175,160,200,185,225,210};\n"
     +t+" "+p+"_interp("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int q=0;q<32;q++){ unsigned xq=(s>>(q&15))&0xFFu;\n"
     "      unsigned seg=xq>>4, frac=xq&15u;\n"
     "      int y0=(int)"+p+"_yv[seg], y1=(int)"+p+"_yv[seg+1];\n"
     "      int v=y0 + ((y1-y0)*(int)frac)/16;\n"
     "      acc=acc*131u+(unsigned)v; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x1Du}, "OptStress110", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress110TC("x64o110", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress110TC("x86o110", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress110TC("a64o110", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress110TC("armo110", "int");

INSTANTIATE_TEST_SUITE_P(OptStress110, X64OptStress110RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress110, X86OptStress110RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress110, A64OptStress110RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress110, ARM32OptStress110RT, ::testing::ValuesIn(kARM), rtTCName);
