//===- AllPlatform_OptStress133RTTests.cpp - integral image / conv / RLE ==//
//
// Green guardrails for three more rodata access SHAPES, all address-independent
// (the folded result depends only on the bytes in the globals + the control
// flow, never on an absolute VA) and all reached by pure index arithmetic from
// the array base (`tab[idx]`, never an interior pointer), so none touches the
// deferred i386/ARM32 PIC rodata *interior*-pointer model (#477/#487) and every
// probe runs on all four targets.
//
//   * integral - 2D summed-area table (integral image) over an 8x8 rodata image
//                with O(1) rectangle-sum queries `A-B-C+D`.  Pins an in-place 2D
//                inclusive-scan with four-corner lookups (distinct from a 1D
//                Fenwick or flat prefix sum).
//   * conv     - 1D convolution: sliding dot product of a rodata signal with a
//                rodata kernel.  Pins a windowed multiply-accumulate over two
//                rodata arrays (distinct from the 2D GEMM in #131).
//   * rle      - run-length detection over a rodata byte stream, folding
//                (value,run) pairs.  Pins a stateful adjacent-equal run collapse
//                (distinct from any table or DP shape).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress133RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress133RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress133RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress133RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress133RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress133RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress133RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress133RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress133TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 2D summed-area table (integral image) over an 8x8 rodata image + rect sums.
    {p+"_integral",
     "static const unsigned char "+p+"_img[64]={\n"
     "3,7,1,9,4,6,2,8, 5,2,8,3,7,1,9,4, 6,9,3,5,1,8,4,7, 2,4,7,1,9,3,6,5,\n"
     "8,1,5,7,2,9,3,6, 4,6,2,8,5,1,7,9, 7,3,9,4,6,2,8,1, 1,8,4,6,3,7,5,2};\n"
     +t+" "+p+"_integral("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned S[64];\n"
     "    for(int i=0;i<8;i++) for(int j=0;j<8;j++){\n"
     "      unsigned v="+p+"_img[i*8+j]^((s>>((i+j)&7))&3u);\n"
     "      unsigned up=(i>0)?S[(i-1)*8+j]:0u, lf=(j>0)?S[i*8+(j-1)]:0u;\n"
     "      unsigned dg=(i>0&&j>0)?S[(i-1)*8+(j-1)]:0u; S[i*8+j]=v+up+lf-dg; }\n"
     "    for(int q=0;q<16;q++){ int r0=(s>>(q&7))&3, c0=(s>>((q+1)&7))&3, r1=r0+3, c1=c0+3;\n"
     "      unsigned A=S[r1*8+c1], B=(r0>0)?S[(r0-1)*8+c1]:0u;\n"
     "      unsigned C=(c0>0)?S[r1*8+(c0-1)]:0u, D=(r0>0&&c0>0)?S[(r0-1)*8+(c0-1)]:0u;\n"
     "      acc=acc*131u+(A-B-C+D); }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x3Eu}, "OptStress133", 2},

    // 1D convolution: sliding dot product of a rodata signal with a rodata kernel.
    {p+"_conv",
     "static const unsigned char "+p+"_sig[32]={\n"
     "4,8,2,6,9,1,7,3, 5,2,8,4,6,1,9,7, 3,6,2,8,5,1,7,4, 9,2,6,3,8,1,5,7};\n"
     "static const unsigned char "+p+"_ker[5]={1,4,6,4,1};\n"
     +t+" "+p+"_conv("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int i=0;i<=27;i++){ unsigned sum=0u;\n"
     "      for(int k=0;k<5;k++) sum+=("+p+"_sig[i+k]^((s>>(k&7))&1u))*"+p+"_ker[k];\n"
     "      acc=acc*131u+sum; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x4Au}, "OptStress133", 2},

    // run-length detection over a rodata byte stream, folding (value,run) pairs.
    {p+"_rle",
     "static const unsigned char "+p+"_seq[40]={\n"
     "1,1,1,2,2,3,3,3, 3,4,5,5,5,2,2,2, 6,6,1,1,1,1,7,7, 3,3,3,8,8,8,8,8, 4,4,9,9,9,1,1,2};\n"
     +t+" "+p+"_rle("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned prev=0x100u, run=0u, runs=0u;\n"
     "    for(int i=0;i<40;i++){ unsigned c=("+p+"_seq[i]^((s>>(i&7))&1u))&7u;\n"
     "      if(c==prev) run++;\n"
     "      else { if(i>0){ acc=acc*131u+prev*16u+run; runs++; } prev=c; run=1u; } }\n"
     "    acc=acc*131u+prev*16u+run+runs; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x71u}, "OptStress133", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress133TC("x64o133", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress133TC("x86o133", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress133TC("a64o133", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress133TC("armo133", "int");

INSTANTIATE_TEST_SUITE_P(OptStress133, X64OptStress133RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress133, X86OptStress133RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress133, A64OptStress133RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress133, ARM32OptStress133RT, ::testing::ValuesIn(kARM), rtTCName);
