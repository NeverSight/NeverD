//===- AllPlatform_OptStress121RTTests.cpp - CDF / Booth / bignum shapes ---==//
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
//   * histeq - histogram equalization of a rodata image: a 16-bin histogram, an
//              in-place prefix-sum CDF, then a remap `cdf[bin]*15/64`.  Pins a
//              histogram -> CDF -> table-remap pipeline reading rodata twice.
//   * booth  - Booth radix-2 signed multiplication of rodata (x,y) pairs: the
//              `(Q0,Q-1)` recoded add/sub plus an arithmetic-shift-right of the
//              {A:Q} register.  Pins a signed shift-add/sub recurrence (distinct
//              from the carry-less GF multiply).
//   * bignum - schoolbook multi-limb (base-256) multiply of two rodata byte
//              vectors with carry propagation `cur=R[i+j]+A[i]*B[j]+carry`.  Pins
//              a limb-wise multiply with carry ripple (distinct from polynomial
//              multiply which has no base reduction).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress121RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress121RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress121RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress121RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress121RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress121RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress121RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress121RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress121TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // histogram equalization: 16-bin histogram -> CDF -> remap over a rodata image.
    {p+"_histeq",
     "static const unsigned char "+p+"_img[64]={\n"
     "10,20,35,50,60,75,85,95, 25,40,55,70,80,90,100,110, 30,45,65,85,95,105,115,120,\n"
     "35,55,75,95,110,125,130,140, 40,60,80,100,115,130,140,150, 45,65,85,105,120,135,145,155,\n"
     "50,70,90,110,125,140,150,160, 55,75,95,115,130,145,155,165};\n"
     +t+" "+p+"_histeq("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned hist[16]; for(int i=0;i<16;i++) hist[i]=0u;\n"
     "    for(int i=0;i<64;i++) hist[(("+p+"_img[i]^((s>>(i&7))&1u))>>4)&15u]++;\n"
     "    unsigned cdf[16], run=0u; for(int i=0;i<16;i++){ run+=hist[i]; cdf[i]=run; }\n"
     "    for(int i=0;i<64;i++){ unsigned q=(("+p+"_img[i]^((s>>(i&7))&1u))>>4)&15u;\n"
     "      unsigned eq=(cdf[q]*15u)/64u; acc=acc*131u+eq; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x4Eu}, "OptStress121", 2},

    // Booth radix-2 signed multiply of rodata (x,y) pairs (shift-add/sub).
    {p+"_booth",
     "static const signed char "+p+"_bx[16]={\n"
     "37,-58,91,-12,46,-73,28,-5, 60,-41,17,-88,53,-26,9,-70};\n"
     "static const signed char "+p+"_by[16]={\n"
     "-19,42,-7,63,-50,11,-84,25, -36,77,-2,48,-61,14,-93,30};\n"
     +t+" "+p+"_booth("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int i=0;i<16;i++){ int m="+p+"_bx[i]+(int)((s>>(i&7))&3u);\n"
     "      unsigned q=((unsigned)"+p+"_by[i])&0xFFu, qm1=0u; int A=0;\n"
     "      for(int k=0;k<8;k++){ unsigned q0=q&1u;\n"
     "        if(q0==1u && qm1==0u) A-=m; else if(q0==0u && qm1==1u) A+=m;\n"
     "        qm1=q0; unsigned nq=((q>>1)|((unsigned)(A&1)<<7))&0xFFu; A=A>>1; q=nq; }\n"
     "      acc=acc*131u+(unsigned)(((A<<8)|(int)q)&0xFFFF); }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xB7u}, "OptStress121", 2},

    // schoolbook multi-limb (base-256) multiply of two rodata byte vectors.
    {p+"_bignum",
     "static const unsigned char "+p+"_nx[8]={0x9a,0x47,0xe3,0x05,0xbd,0x72,0x18,0x8f};\n"
     "static const unsigned char "+p+"_ny[8]={0x23,0xd6,0x4a,0x91,0x0c,0xfe,0x57,0x6b};\n"
     +t+" "+p+"_bignum("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned A[8], B[8], R[16];\n"
     "    for(int i=0;i<8;i++){ A[i]="+p+"_nx[i]^((s>>(i&7))&0xFu); B[i]="+p+"_ny[i]^((s>>((i+3)&7))&0xFu); }\n"
     "    for(int i=0;i<16;i++) R[i]=0u;\n"
     "    for(int i=0;i<8;i++){ unsigned carry=0u;\n"
     "      for(int j=0;j<8;j++){ unsigned cur=R[i+j]+A[i]*B[j]+carry; R[i+j]=cur&0xFFu; carry=cur>>8; }\n"
     "      R[i+8]+=carry; }\n"
     "    for(int i=0;i<16;i++) acc=acc*131u+R[i];\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x6Eu}, "OptStress121", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress121TC("x64o121", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress121TC("x86o121", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress121TC("a64o121", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress121TC("armo121", "int");

INSTANTIATE_TEST_SUITE_P(OptStress121, X64OptStress121RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress121, X86OptStress121RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress121, A64OptStress121RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress121, ARM32OptStress121RT, ::testing::ValuesIn(kARM), rtTCName);
