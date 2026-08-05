//===- AllPlatform_PermuteAlgoRTTests.cpp - permute/table kernels -*- C++ -*-===//
//
// clang -O2 permutation / table-lookup / transpose probes (bit-reversal table,
// S-box substitution, 8x8 byte-matrix transpose, planar->packed interleave,
// dynamic byte shuffle, nibble swap).  These are byte-heavy with data-dependent
// indexing — exercising gather/scatter, shuffle/tbl, byte sub-register stores,
// and (via local arrays) VFP/stack spill-reload paths across all three arches.
//
// All arithmetic is 8/32-bit with no libcall-inducing operations.  Original vs
// recompiled are compared.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64PermuteAlgoRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64PermuteAlgoRT, Verify) { roundTripX64(GetParam()); }

class A64PermuteAlgoRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64PermuteAlgoRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32PermuteAlgoRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32PermuteAlgoRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makePermTC(const char *prefix, const char *T,
                                           int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Bit-reversal table build + apply (FFT-style index permutation).
    {p+"_revbits",
     t+" "+p+"_revbits("+t+" a) {\n"
     "  unsigned char tbl[256];\n"
     "  for(int i=0;i<256;i++){ unsigned char r=0,v=(unsigned char)i;\n"
     "    for(int b=0;b<8;b++){ r=(unsigned char)((r<<1)|(v&1)); v>>=1; } tbl[i]=r; }\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<200;i++){ unsigned char x=(unsigned char)(a*(i+1)+i); acc=acc*131+tbl[x]; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x1234567ULL}, "PermuteAlgo", opt, fl},

    // 256-entry S-box substitution chain (data-dependent gather).
    {p+"_sbox",
     t+" "+p+"_sbox("+t+" a) {\n"
     "  unsigned char s[256];\n"
     "  for(int i=0;i<256;i++) s[i]=(unsigned char)((i*167+13)^((i>>3)|(i<<5)));\n"
     "  unsigned acc=(unsigned)a;\n"
     "  for(int i=0;i<150;i++){ acc=s[acc&0xFF]^(acc<<8)^(acc>>24); acc+=(unsigned)i; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x2345678ULL}, "PermuteAlgo", opt, fl},

    // 8x8 byte-matrix transpose (cross-row swaps).
    {p+"_transpose",
     t+" "+p+"_transpose("+t+" a) {\n"
     "  unsigned char m[64];\n"
     "  for(int i=0;i<64;i++) m[i]=(unsigned char)(a*(i+1)+i*i);\n"
     "  for(int r=0;r<8;r++) for(int c=r+1;c<8;c++){\n"
     "    unsigned char tmp=m[r*8+c]; m[r*8+c]=m[c*8+r]; m[c*8+r]=tmp; }\n"
     "  unsigned acc=0; for(int i=0;i<64;i++) acc=acc*131+m[i];\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x3456789ULL}, "PermuteAlgo", opt, fl},

    // Planar (R,G,B separate) -> packed (RGBRGB) interleave.
    {p+"_interleave",
     t+" "+p+"_interleave("+t+" a) {\n"
     "  unsigned char R[20],G[20],B[20],out[60];\n"
     "  for(int i=0;i<20;i++){ R[i]=(unsigned char)(a*(i+1)); G[i]=(unsigned char)(a*3+i); B[i]=(unsigned char)(a*7+i*2); }\n"
     "  for(int i=0;i<20;i++){ out[i*3]=R[i]; out[i*3+1]=G[i]; out[i*3+2]=B[i]; }\n"
     "  unsigned acc=0; for(int i=0;i<60;i++) acc=acc*131+out[i];\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x456789AULL}, "PermuteAlgo", opt, fl},

    // Dynamic byte shuffle: dst[i] = src[idx[i]] (data-dependent gather).
    {p+"_byteperm",
     t+" "+p+"_byteperm("+t+" a) {\n"
     "  unsigned char src[64],idx[64],dst[64];\n"
     "  for(int i=0;i<64;i++){ src[i]=(unsigned char)(a*(i+1)); idx[i]=(unsigned char)((a*7+i*13)&63); }\n"
     "  for(int i=0;i<64;i++) dst[i]=src[idx[i]];\n"
     "  unsigned acc=0; for(int i=0;i<64;i++) acc=acc*131+dst[i];\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x56789ABULL}, "PermuteAlgo", opt, fl},

    // Per-byte nibble swap chain (sub-word permutation).
    {p+"_nibbleswap",
     t+" "+p+"_nibbleswap("+t+" a) {\n"
     "  unsigned acc=(unsigned)a;\n"
     "  for(int i=0;i<100;i++){ unsigned x=acc^(unsigned)(i*0x01010101);\n"
     "    acc=((x&0x0F0F0F0Fu)<<4)|((x&0xF0F0F0F0u)>>4); acc+=(unsigned)i; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x6789ABCULL}, "PermuteAlgo", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Perm = makePermTC("x64p", "long", 2, "");
static const std::vector<RoundTripTC> kA64Perm = makePermTC("a64p", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Perm = makePermTC("armp", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(PermuteAlgo, X64PermuteAlgoRT,
                         ::testing::ValuesIn(kX64Perm), rtTCName);
INSTANTIATE_TEST_SUITE_P(PermuteAlgo, A64PermuteAlgoRT,
                         ::testing::ValuesIn(kA64Perm), rtTCName);
INSTANTIATE_TEST_SUITE_P(PermuteAlgo, ARM32PermuteAlgoRT,
                         ::testing::ValuesIn(kARM32Perm), rtTCName);
