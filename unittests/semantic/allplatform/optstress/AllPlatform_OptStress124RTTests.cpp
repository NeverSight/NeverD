//===- AllPlatform_OptStress124RTTests.cpp - conv / select / bitpack shapes =//
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
//   * conv   - rate-1/2 convolutional encoder over a rodata bit stream: a small
//              shift register tapped by two rodata generator polynomials, each
//              reduced to a parity bit by an xor-fold.  Pins a shift-register +
//              dual-parity output (distinct from the single-output LFSR).
//   * qselect- quickselect of the k-th order statistic in a rodata-seeded stack
//              array (Lomuto partition recursing toward one side only).  Pins a
//              one-sided partition descent (distinct from the full quicksort).
//   * bitpack- variable-width bit packer: fields whose widths come from a rodata
//              table are concatenated into a bit cursor and flushed a byte at a
//              time.  Pins a bit-cursor with rodata-driven variable field widths.
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress124RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress124RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress124RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress124RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress124RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress124RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress124RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress124RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress124TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // rate-1/2 convolutional encoder with two rodata generator polynomials.
    {p+"_conv",
     "static const unsigned char "+p+"_gen[2]={7,5};\n"
     "static const unsigned char "+p+"_in[40]={\n"
     "1,0,1,1,0,0,1,0, 1,1,1,0,0,1,0,1, 0,0,1,1,0,1,1,0, 1,0,0,1,1,0,1,0, 0,1,0,1,1,0,0,1};\n"
     +t+" "+p+"_conv("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s; unsigned reg=0u;\n"
     "    for(int i=0;i<40;i++){ unsigned bit=("+p+"_in[i]^(s>>(i&7)))&1u;\n"
     "      reg=((reg<<1)|bit)&7u;\n"
     "      unsigned m0=reg&"+p+"_gen[0]; m0^=m0>>2; m0^=m0>>1; unsigned o0=m0&1u;\n"
     "      unsigned m1=reg&"+p+"_gen[1]; m1^=m1>>2; m1^=m1>>1; unsigned o1=m1&1u;\n"
     "      acc=acc*131u+(o0<<1)+o1; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xC0u}, "OptStress124", 2},

    // quickselect of the k-th order statistic in a rodata-seeded stack array.
    {p+"_qselect",
     "static const unsigned char "+p+"_data[24]={\n"
     "0x3a,0x91,0x47,0xee,0x12,0x8d,0x5b,0xc6, 0x29,0xf0,0x74,0xa3,0x1e,0x6c,0xd8,0x05,\n"
     "0x9f,0x33,0xb7,0x4a,0xe1,0x58,0x82,0x2d};\n"
     +t+" "+p+"_qselect("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned v[24]; for(int i=0;i<24;i++) v[i]="+p+"_data[i]^((s>>(i&7))&0xFu);\n"
     "    int k=(int)((s>>3)%24u); int lo=0, hi=23; unsigned result=0u;\n"
     "    while(lo<=hi){ unsigned pivot=v[hi]; int i=lo-1;\n"
     "      for(int j=lo;j<hi;j++){ if(v[j]<=pivot){ i++; unsigned tp=v[i]; v[i]=v[j]; v[j]=tp; } }\n"
     "      i++; unsigned tp=v[i]; v[i]=v[hi]; v[hi]=tp;\n"
     "      if(i==k){ result=v[i]; break; } else if(i<k) lo=i+1; else hi=i-1; }\n"
     "    acc=acc*131u+result+(unsigned)k; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x6Bu}, "OptStress124", 2},

    // variable-width bit packer with rodata-driven field widths (byte flush).
    {p+"_bitpack",
     "static const unsigned char "+p+"_wid[16]={3,7,1,5,8,2,6,4, 1,8,3,7,2,5,6,4};\n"
     "static const unsigned char "+p+"_val[16]={\n"
     "0x5a,0x13,0xc8,0x2f,0x96,0x4d,0xe1,0x7b, 0x30,0xa7,0x6c,0x18,0xf3,0x49,0xbd,0x05};\n"
     +t+" "+p+"_bitpack("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned bitbuf=0u, nbits=0u;\n"
     "    for(int i=0;i<16;i++){ unsigned w=("+p+"_wid[i]&7u)+1u;\n"
     "      unsigned v=("+p+"_val[i]^(s>>(i&7)))&((1u<<w)-1u);\n"
     "      bitbuf=(bitbuf<<w)|v; nbits+=w;\n"
     "      while(nbits>=8u){ nbits-=8u; acc=acc*131u+((bitbuf>>nbits)&0xFFu); } }\n"
     "    acc=acc*131u+bitbuf+nbits; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xB7u}, "OptStress124", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress124TC("x64o124", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress124TC("x86o124", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress124TC("a64o124", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress124TC("armo124", "int");

INSTANTIATE_TEST_SUITE_P(OptStress124, X64OptStress124RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress124, X86OptStress124RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress124, A64OptStress124RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress124, ARM32OptStress124RT, ::testing::ValuesIn(kARM), rtTCName);
