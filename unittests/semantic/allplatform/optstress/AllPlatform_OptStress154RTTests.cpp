//===- AllPlatform_OptStress154RTTests.cpp - matmul / conv1d / window-max =//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Green guardrails for three more rodata access SHAPES.  Each copies its rodata
// operands into a stack buffer with a plain base+index loop and then does all
// indexed work on the stack copy, so the folded result depends only on the bytes
// in the globals + the control flow (never an absolute VA) and nothing touches
// the deferred i386/ARM32 PIC rodata *interior*-pointer model (#477/#487); every
// probe runs on all four targets.
//
//   * matmul   - 3x3 integer matrix multiply of two rodata matrices: the classic
//                triple loop multiply-accumulate over row/column index math.
//                Pins a dense MAC kernel (distinct from the 1-D dot products and
//                vector reductions elsewhere).
//   * conv1d   - 1-D convolution of a rodata signal with a rodata kernel: a
//                sliding length-5 weighted window sum.  Pins an FIR sliding dot
//                product (distinct from the dense matmul above).
//   * slidemax - sliding-window maximum via a monotonic index deque over a rodata
//                array: stale and dominated indices are popped so the front always
//                holds the window max.  Pins a monotonic-deque scan (distinct from
//                the heap/insert/quick sorts that order the whole array).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress154RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress154RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress154RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress154RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress154RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress154RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress154RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress154RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress154TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 3x3 integer matrix multiply of two rodata matrices (triple-loop MAC).
    {p+"_matmul",
     "static const unsigned char "+p+"_ma[9]={3,7,2,5,1,8,4,6,9};\n"
     "static const unsigned char "+p+"_mb[9]={2,4,1,8,3,5,7,9,6};\n"
     +t+" "+p+"_matmul("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned A[9],B[9],C[9];\n"
     "    for(int i=0;i<9;i++){ A[i]=(unsigned)"+p+"_ma[i]^((s>>(i&7))&3u); B[i]=(unsigned)"+p+"_mb[i]^((s>>((i+2)&7))&3u); }\n"
     "    for(int r=0;r<3;r++) for(int c=0;c<3;c++){ unsigned sum=0u; for(int k=0;k<3;k++) sum+=A[r*3+k]*B[k*3+c]; C[r*3+c]=sum; acc=acc*131u+sum; }\n"
     "    for(int i=0;i<9;i++) acc=acc*131u+C[i]*(unsigned)(i+1);\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x1Eu}, "OptStress154", 2},

    // 1-D convolution of a rodata signal with a rodata kernel (FIR window sum).
    {p+"_conv1d",
     "static const unsigned char "+p+"_cs[24]={5,9,12,3,7,14,2,8,11,6,1,15,4,10,13,0,9,5,12,3,7,2,8,6};\n"
     "static const unsigned char "+p+"_ck[5]={1,4,6,4,1};\n"
     +t+" "+p+"_conv1d("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned sig[24],ker[5];\n"
     "    for(int i=0;i<24;i++) sig[i]=(unsigned)"+p+"_cs[i]^((s>>(i&7))&3u);\n"
     "    for(int i=0;i<5;i++) ker[i]=(unsigned)"+p+"_ck[i];\n"
     "    for(int i=0;i+5<=24;i++){ unsigned sum=0u; for(int k=0;k<5;k++) sum+=sig[i+k]*ker[k]; acc=acc*131u+sum; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x2Fu}, "OptStress154", 2},

    // sliding-window maximum via a monotonic index deque over a rodata array.
    {p+"_slidemax",
     "static const unsigned char "+p+"_sm[32]={5,9,12,3,7,14,2,8,11,6,1,15,4,10,13,0,9,5,12,3,7,2,8,6,11,1,14,4,10,13,7,2};\n"
     +t+" "+p+"_slidemax("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned arr[32]; for(int i=0;i<32;i++) arr[i]=(unsigned)"+p+"_sm[i]^((s>>(i&7))&7u);\n"
     "    int dq[32]; int hd=0,tl=0;\n"
     "    for(int i=0;i<32;i++){ while(tl>hd && arr[dq[tl-1]]<=arr[i]) tl--; dq[tl++]=i;\n"
     "      if(dq[hd]<=i-5) hd++; if(i>=4) acc=acc*131u+arr[dq[hd]]; }\n"
     "    acc=acc*131u+(unsigned)(tl-hd); out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x3Cu}, "OptStress154", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress154TC("x64o154", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress154TC("x86o154", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress154TC("a64o154", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress154TC("armo154", "int");

INSTANTIATE_TEST_SUITE_P(OptStress154, X64OptStress154RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress154, X86OptStress154RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress154, A64OptStress154RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress154, ARM32OptStress154RT, ::testing::ValuesIn(kARM), rtTCName);
