//===- AllPlatform_OptStress296RTTests.cpp - interleave/unpack probe ======//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// -O2 integer kernels stressing interleave/deinterleave, odd-even split and
// sub-word pack/unpack codegen paths:
//
//   * deinter  - deinterleave even/odd lanes from array.
//   * inter    - interleave two half-arrays into one.
//   * unzip4   - 4-way byte unzip from 32-bit words.
//   * zip4     - 4-way byte zip into 32-bit words.
//   * halfswap - swap upper/lower 16-bit halves per word.
//   * byterev  - reverse byte order within each 32-bit word (manual).
//
// All indices masked in range, all operations UB-free in unsigned domain,
// and there is no division -- so native and lifted agree bit-for-bit.  Four, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress296RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress296RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress296RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress296RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress296RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress296RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress296RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress296RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress296TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // deinterleave even/odd lanes from array.
    {p+"_deinter",
     t+" "+p+"_deinter("+t+" a){ unsigned buf[32]; unsigned h=(unsigned)a;\n"
     "  for(int i=0;i<32;i++){ h=h*1103515245u+12345u; buf[i]=h; }\n"
     "  unsigned ev=0, od=0;\n"
     "  for(int i=0;i<32;i+=2){ ev=ev*131u+buf[i]; od=od*131u+buf[i+1]; }\n"
     "  return ("+t+")(ev^od); }\n",
     {0x12345u}, "OptStress296", 2},

    // interleave two half-arrays into one.
    {p+"_inter",
     t+" "+p+"_inter("+t+" a){ unsigned lo[16], hi[16]; unsigned h=(unsigned)a;\n"
     "  for(int i=0;i<16;i++){ h=h*1103515245u+12345u; lo[i]=h; hi[i]=h>>8; }\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<16;i++){ acc=acc*131u+(lo[i]<<16)+hi[i]; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress296", 2},

    // 4-way byte unzip from 32-bit words.
    {p+"_unzip4",
     t+" "+p+"_unzip4("+t+" a){ unsigned words[16]; unsigned h=(unsigned)a;\n"
     "  for(int i=0;i<16;i++){ h=h*1103515245u+12345u; words[i]=h; }\n"
     "  unsigned b0=0,b1=0,b2=0,b3=0;\n"
     "  for(int i=0;i<16;i++){ b0+=words[i]&0xFFu; b1+=(words[i]>>8)&0xFFu;\n"
     "    b2+=(words[i]>>16)&0xFFu; b3+=(words[i]>>24)&0xFFu; }\n"
     "  return ("+t+")(b0+b1*131u+b2*40503u+b3*2654435761u); }\n",
     {0x34567u}, "OptStress296", 2},

    // 4-way byte zip into 32-bit words.
    {p+"_zip4",
     t+" "+p+"_zip4("+t+" a){ unsigned char b0[16],b1[16],b2[16],b3[16]; unsigned h=(unsigned)a;\n"
     "  for(int i=0;i<16;i++){ h=h*1103515245u+12345u;\n"
     "    b0[i]=(unsigned char)(h>>0); b1[i]=(unsigned char)(h>>5);\n"
     "    b2[i]=(unsigned char)(h>>10); b3[i]=(unsigned char)(h>>15); }\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<16;i++){ unsigned w=b0[i]|(b1[i]<<8)|(b2[i]<<16)|(b3[i]<<24);\n"
     "    acc=acc*131u+w; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress296", 2},

    // swap upper/lower 16-bit halves per word.
    {p+"_halfswap",
     t+" "+p+"_halfswap("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned lo=h&0xFFFFu, hi=(h>>16)&0xFFFFu;\n"
     "    unsigned sw=(lo<<16)|hi; acc=acc*131u+sw+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress296", 2},

    // reverse byte order within each 32-bit word (manual).
    {p+"_byterev",
     t+" "+p+"_byterev("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned b0=h&0xFFu,b1=(h>>8)&0xFFu,b2=(h>>16)&0xFFu,b3=(h>>24)&0xFFu;\n"
     "    unsigned r=(b0<<24)|(b1<<16)|(b2<<8)|b3; acc=acc*131u+r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress296", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress296TC("x64o296", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress296TC("x86o296", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress296TC("a64o296", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress296TC("armo296", "int");

INSTANTIATE_TEST_SUITE_P(OptStress296, X64OptStress296RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress296, X86OptStress296RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress296, A64OptStress296RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress296, ARM32OptStress296RT, ::testing::ValuesIn(kARM), rtTCName);
