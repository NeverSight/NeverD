//===- AllPlatform_OptStress143RTTests.cpp - Fisher-Yates / Karatsuba / MTF =//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Green guardrails for three more rodata access SHAPES, all address-independent
// (the folded result depends only on the bytes in the globals + the control
// flow, never on an absolute VA) and all reached by pure index arithmetic from
// the array base (`tab[idx]`, never an interior pointer): the rodata is read
// forward into a stack copy where an in-place pass then walks it, so none
// touches the deferred i386/ARM32 PIC rodata *interior*-pointer model
// (#477/#487) and every probe runs on all four targets.
//
//   * fyshuf - Fisher-Yates in-place shuffle of a rodata-seeded stack array: an
//              LCG draws a partner index in a shrinking range and swaps.  Pins a
//              data-dependent swap permutation (distinct from the fixed
//              transpose/permute in #64 and from any sort).
//   * karat  - Karatsuba multiplication of rodata 16-bit operands: split each
//              into 8-bit halves, form the three sub-products z0/z2/z1 and
//              recombine `z0 + (z1<<8) + (z2<<16)`.  Pins a three-product
//              split-multiply recurrence (distinct from the Booth shift-add in
//              #121 and any single hardware multiply).
//   * mtf    - move-to-front transform over a rodata symbol stream: a small
//              alphabet list is linearly searched for each symbol, its position
//              emitted, then the symbol shifted to the front.  Pins a
//              search-and-shift self-organizing list (distinct from counting
//              sort / histogram scans).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress143RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress143RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress143RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress143RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress143RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress143RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress143RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress143RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress143TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Fisher-Yates in-place shuffle of a rodata-seeded stack array.
    {p+"_fyshuf",
     "static const unsigned char "+p+"_src[24]={\n"
     "17,42,8,99,3,128,55,200, 71,14,233,5,88,160,27,121, 6,250,33,77,140,19,210,64};\n"
     +t+" "+p+"_fyshuf("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned arr[24];\n"
     "    for(int i=0;i<24;i++) arr[i]=(unsigned)"+p+"_src[i];\n"
     "    for(int i=23;i>0;i--){ s=s*1103515245u+12345u; unsigned j=s%(unsigned)(i+1);\n"
     "      unsigned tmp=arr[i]; arr[i]=arr[j]; arr[j]=tmp; acc=acc*131u+arr[i]+j; }\n"
     "    for(int i=0;i<24;i++) acc=acc*131u+arr[i]*(unsigned)(i+1);\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x33u}, "OptStress143", 2},

    // Karatsuba multiply of rodata 16-bit operands (three sub-product recombine).
    {p+"_karat",
     "static const unsigned char "+p+"_xa[8]={0x9e,0x37,0xc1,0x5a,0x2f,0xe8,0x73,0x14};\n"
     "static const unsigned char "+p+"_xb[8]={0xab,0x60,0xdd,0x06,0x99,0x42,0xbf,0x28};\n"
     +t+" "+p+"_karat("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int q=0;q<32;q++){\n"
     "      unsigned x=((unsigned)"+p+"_xa[q&7]<<8)|((s>>(q&7))&0xFFu);\n"
     "      unsigned y=((unsigned)"+p+"_xb[q&7]<<8)|((s>>((q>>1)&7))&0xFFu);\n"
     "      unsigned aL=x&0xFFu, aH=x>>8, bL=y&0xFFu, bH=y>>8;\n"
     "      unsigned z0=aL*bL, z2=aH*bH, z1=(aL+aH)*(bL+bH)-z2-z0;\n"
     "      unsigned prod=z0+(z1<<8)+(z2<<16);\n"
     "      acc=acc*131u+prod+z1; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x5Du}, "OptStress143", 2},

    // move-to-front transform over a rodata symbol stream (search + shift front).
    {p+"_mtf",
     "static const unsigned char "+p+"_in[40]={\n"
     "3,7,1,7,3,0,12,7, 3,1,9,7,3,0,5,7, 1,3,7,0,11,3,7,1, 0,3,7,14,3,1,7,0, 3,7,1,2,0,3,7,1};\n"
     +t+" "+p+"_mtf("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s, sumpos=0u;\n"
     "    unsigned lst[16]; for(int i=0;i<16;i++) lst[i]=(unsigned)i;\n"
     "    for(int i=0;i<40;i++){ unsigned sym=((unsigned)"+p+"_in[i]^((s>>(i&7))&1u))&15u;\n"
     "      unsigned pos=0u; while(pos<16u && lst[pos]!=sym) pos++;\n"
     "      if(pos<16u){ for(unsigned k=pos;k>0u;k--){ lst[k]=lst[k-1u]; acc=acc*131u+lst[k]; } lst[0]=sym; }\n"
     "      sumpos+=pos; acc=acc*131u+pos; }\n"
     "    acc=acc*131u+sumpos; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x72u}, "OptStress143", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress143TC("x64o143", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress143TC("x86o143", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress143TC("a64o143", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress143TC("armo143", "int");

INSTANTIATE_TEST_SUITE_P(OptStress143, X64OptStress143RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress143, X86OptStress143RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress143, A64OptStress143RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress143, ARM32OptStress143RT, ::testing::ValuesIn(kARM), rtTCName);
