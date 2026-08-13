//===- AllPlatform_OptStress101RTTests.cpp - sub-byte / deep-indirect rodata -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Green guardrails for three more rodata access SHAPES, all address-independent
// (the folded result depends only on the bytes in the globals + control flow,
// never on an absolute VA) and all walked forward from the array base, so none
// touches the deferred i386/ARM32 PIC rodata *interior*-pointer model.
//
//   * nib  - a nibble-packed rodata table read as `nt[idx>>1]` then the high or
//            low 4-bit field extracted by `(idx&1)?(b>>4):(b&15)`.  Pins
//            sub-byte (half-byte) field extraction off an indexed rodata load.
//   * tri  - THREE-level rodata indirection `A[B[C[i]]]`: each value read drives
//            the next table's index, one level deeper than OptStress82 `dblidx`.
//            Stresses the chained `base + value_from_prior_load` resolver depth.
//   * b64  - base64-style 6-bit slicing of a rodata byte triple into four
//            symbols, each used to GATHER from a 64-entry rodata C-string charset
//            (`cs[s]`).  Bit-slice + small-table gather off a rodata *string*.
//
// Integer in / integer out, file-scope const (rodata) arrays + one rodata C
// string, LCG-seeded, folded to one integer return; no float / 64-bit divide /
// libcall.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress101RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress101RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress101RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress101RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress101RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress101RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress101RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress101RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress101TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // nibble-packed rodata table: index a half-byte, extract high/low 4 bits.
    {p+"_nib",
     "static const unsigned char "+p+"_nt[32]={\n"
     "0x1F,0x37,0x4A,0x29,0x56,0x83,0x6C,0x9E, 0xB1,0x05,0xD7,0x42,0x3E,0x88,0xAC,0x7B,\n"
     "0x14,0x6F,0x90,0x2D,0x53,0xC6,0x71,0x08, 0xEA,0x35,0x59,0x9C,0x47,0xB2,0x60,0xFD};\n"
     +t+" "+p+"_nib("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, out=0;\n"
     "  for(int it=0;it<64;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int k=0;k<48;k++){\n"
     "      unsigned idx=((s>>(k&15))^(unsigned)k)&63u;\n"
     "      unsigned b="+p+"_nt[idx>>1];\n"
     "      unsigned v=(idx&1u)?(b>>4):(b&15u);\n"
     "      acc=acc*131u+v; acc^=acc>>7; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xA1u}, "OptStress101", 2},

    // three-level rodata indirection A[B[C[i]]] (one deeper than dblidx).
    {p+"_tri",
     "static const unsigned char "+p+"_C[32]={\n"
     "5,18,2,27,11,30,7,21, 14,1,25,9,16,3,29,12,\n"
     "8,23,0,19,6,28,13,4, 31,10,17,26,15,22,20,24};\n"
     "static const unsigned char "+p+"_B[32]={\n"
     "12,0,25,7,19,3,28,14, 5,22,9,30,1,17,11,26,\n"
     "8,20,4,31,2,15,23,6, 27,13,18,10,29,16,24,21};\n"
     "static const unsigned char "+p+"_A[32]={\n"
     "3,29,11,47,5,17,23,2, 41,7,13,53,19,31,37,61,\n"
     "43,67,71,73,79,83,89,97, 101,103,107,109,113,127,131,137};\n"
     +t+" "+p+"_tri("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u;\n"
     "    unsigned i=(s>>5)&31u;\n"
     "    unsigned c="+p+"_C[i]&31u;\n"
     "    unsigned b="+p+"_B[c]&31u;\n"
     "    unsigned v="+p+"_A[b];\n"
     "    out=out*131u+v+(b<<3)+(c<<1); }\n"
     "  return ("+t+")out; }\n",
     {0xB2u}, "OptStress101", 2},

    // base64-style 6-bit slice of a rodata byte triple, gather a rodata charset.
    {p+"_b64",
     "static const unsigned char "+p+"_msg[36]={\n"
     "0x4D,0x61,0x6E,0x20,0x69,0x73,0x20,0x64, 0x69,0x73,0x74,0x69,0x6E,0x67,0x75,0x69,\n"
     "0x73,0x68,0x65,0x64,0x2C,0x20,0x6E,0x6F, 0x74,0x20,0x6F,0x6E,0x6C,0x79,0x20,0x62,\n"
     "0x79,0x20,0x68,0x65};\n"
     "static const unsigned char "+p+"_cs[65]=\n"
     "\"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/\";\n"
     +t+" "+p+"_b64("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<48;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int g=0;g+2<36;g+=3){\n"
     "      unsigned x0="+p+"_msg[g]^(s&0xFFu), x1="+p+"_msg[g+1]^((s>>8)&0xFFu), x2="+p+"_msg[g+2]^((s>>16)&0xFFu);\n"
     "      unsigned w=(x0<<16)|(x1<<8)|x2;\n"
     "      unsigned s0=(w>>18)&63u, s1=(w>>12)&63u, s2=(w>>6)&63u, s3=w&63u;\n"
     "      acc=acc*131u+"+p+"_cs[s0]+"+p+"_cs[s1]*3u+"+p+"_cs[s2]*5u+"+p+"_cs[s3]*7u; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x64u}, "OptStress101", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress101TC("x64o101", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress101TC("x86o101", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress101TC("a64o101", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress101TC("armo101", "int");

INSTANTIATE_TEST_SUITE_P(OptStress101, X64OptStress101RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress101, X86OptStress101RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress101, A64OptStress101RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress101, ARM32OptStress101RT, ::testing::ValuesIn(kARM), rtTCName);
