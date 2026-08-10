//===- AllPlatform_BitAlgo11RTTests.cpp - bit-manipulation algos -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Eleventh batch of clang -O2 algorithm probes.  Earlier batches targeted
// data-parallel SIMD; this one targets SCALAR bit-manipulation idioms that
// stress the optimizer's weak spots: shift-by-amount handling (funnel shifts,
// variable rotates), loop-carried state through xor/shift chains, sub-register
// aliasing, flag folding, and constant propagation through wide masks.
//
// Every function loops over inputs so all paths are exercised and folds to an
// exact integer return value.  All internal arithmetic is unsigned (no signed
// overflow UB) and 32-bit (identical low bits regardless of host word size, and
// no 64-bit divide/multiply that would lower to a runtime library call).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64BitAlgo11RT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64BitAlgo11RT, Verify) { roundTripX64(GetParam()); }

class A64BitAlgo11RT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64BitAlgo11RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32BitAlgo11RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32BitAlgo11RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeBit11TC(const char *prefix, const char *T,
                                            int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Morton / Z-order interleave: spread two 16-bit values into a 32-bit
    // code (shift + mask spreading magic constants).
    {p+"_morton",
     t+" "+p+"_morton("+t+" a) {\n"
     "  int s = 0;\n"
     "  for (int i=0;i<64;i++){\n"
     "    unsigned mx=(unsigned)(a+i)&0xFFFF, my=(unsigned)(a+i*3)&0xFFFF;\n"
     "    mx=(mx|(mx<<8))&0x00FF00FFu; mx=(mx|(mx<<4))&0x0F0F0F0Fu;\n"
     "    mx=(mx|(mx<<2))&0x33333333u; mx=(mx|(mx<<1))&0x55555555u;\n"
     "    my=(my|(my<<8))&0x00FF00FFu; my=(my|(my<<4))&0x0F0F0F0Fu;\n"
     "    my=(my|(my<<2))&0x33333333u; my=(my|(my<<1))&0x55555555u;\n"
     "    unsigned z=mx|(my<<1); s += (int)(z>>3) - i; }\n"
     "  return s;\n"
     "}\n",
     {0x1234567ULL}, "BitAlgo11", opt, fl},

    // xorshift32 PRNG: loop-carried state through three shift+xor steps.
    {p+"_xorshift",
     t+" "+p+"_xorshift("+t+" a) {\n"
     "  unsigned x=((unsigned)a)|1u; int s=0;\n"
     "  for (int i=0;i<96;i++){\n"
     "    x^=x<<13; x^=x>>17; x^=x<<5;\n"
     "    s += (int)(x&0xFFFF) - i; }\n"
     "  return s;\n"
     "}\n",
     {0x2233445ULL}, "BitAlgo11", opt, fl},

    // Galois LFSR: loop-carried state with conditional feedback xor.
    {p+"_lfsr",
     t+" "+p+"_lfsr("+t+" a) {\n"
     "  unsigned r=((unsigned)a&0xFFFFu)|1u; int s=0;\n"
     "  for (int i=0;i<100;i++){\n"
     "    unsigned lsb=r&1u; r>>=1; if (lsb) r^=0xB400u;\n"
     "    s += (int)(r&0xFF) ^ i; }\n"
     "  return s;\n"
     "}\n",
     {0x3344556ULL}, "BitAlgo11", opt, fl},

    // Funnel shift: (hi<<i)|(lo>>(32-i)) and (lo>>i)|(hi<<(32-i)).  clang
    // lowers these to SHLD/SHRD on x86 and ORR+shift on ARM.
    {p+"_funnel",
     t+" "+p+"_funnel("+t+" a) {\n"
     "  unsigned hi=(unsigned)a, lo=(unsigned)(a*5+1); int s=0;\n"
     "  for (int i=1;i<32;i++){\n"
     "    unsigned l=(hi<<i)|(lo>>(32-i));\n"
     "    unsigned r=(lo>>i)|(hi<<(32-i));\n"
     "    s += (int)((l^r)>>8) + i; }\n"
     "  return s;\n"
     "}\n",
     {0x4455667ULL}, "BitAlgo11", opt, fl},

    // SWAR bit reverse: swap adjacent bit groups of widths 1/2/4/8/16.
    {p+"_revswar",
     t+" "+p+"_revswar("+t+" a) {\n"
     "  int s=0;\n"
     "  for (int i=0;i<64;i++){\n"
     "    unsigned r=(unsigned)(a+i*7);\n"
     "    r=((r&0xAAAAAAAAu)>>1)|((r&0x55555555u)<<1);\n"
     "    r=((r&0xCCCCCCCCu)>>2)|((r&0x33333333u)<<2);\n"
     "    r=((r&0xF0F0F0F0u)>>4)|((r&0x0F0F0F0Fu)<<4);\n"
     "    r=((r&0xFF00FF00u)>>8)|((r&0x00FF00FFu)<<8);\n"
     "    r=(r>>16)|(r<<16);\n"
     "    s += (int)(r>>20) - i; }\n"
     "  return s;\n"
     "}\n",
     {0x5566778ULL}, "BitAlgo11", opt, fl},

    // Bitfield pack/unpack: pack four fields into 32 bits then extract.
    {p+"_pack",
     t+" "+p+"_pack("+t+" a) {\n"
     "  int s=0;\n"
     "  for (int i=0;i<80;i++){\n"
     "    unsigned f0=(unsigned)(a+i)&0x7u, f1=(unsigned)(a*2+i)&0x1Fu;\n"
     "    unsigned f2=(unsigned)(a+i*3)&0xFFu, f3=(unsigned)(a^i)&0xFFFFu;\n"
     "    unsigned w=f0|(f1<<3)|(f2<<8)|(f3<<16);\n"
     "    unsigned g0=w&0x7u, g1=(w>>3)&0x1Fu, g2=(w>>8)&0xFFu, g3=(w>>16)&0xFFFFu;\n"
     "    s += (int)(g0+g1+g2+g3) - i; }\n"
     "  return s;\n"
     "}\n",
     {0x6677889ULL}, "BitAlgo11", opt, fl},

    // Variable rotate mix: rotate-left and rotate-right by a loop-varying
    // amount, recombined into loop-carried state.
    {p+"_rotmix",
     t+" "+p+"_rotmix("+t+" a) {\n"
     "  unsigned x=(unsigned)a|1u; int s=0;\n"
     "  for (int i=0;i<96;i++){\n"
     "    unsigned r=(unsigned)(i&31);\n"
     "    unsigned rl=(x<<r)|(x>>((32-r)&31));\n"
     "    unsigned rr=(x>>r)|(x<<((32-r)&31));\n"
     "    x=rl^(rr+(unsigned)i);\n"
     "    s += (int)(x&0xFFF) - i; }\n"
     "  return s;\n"
     "}\n",
     {0x778899AULL}, "BitAlgo11", opt, fl},

    // Parity fold + ternary: XOR-reduce all bits, branch on the parity bit.
    {p+"_parity",
     t+" "+p+"_parity("+t+" a) {\n"
     "  int s=0;\n"
     "  for (int i=0;i<128;i++){\n"
     "    unsigned x=(unsigned)a*(unsigned)i+(unsigned)i;\n"
     "    x^=x>>16; x^=x>>8; x^=x>>4; x^=x>>2; x^=x>>1;\n"
     "    s += (x&1u) ? i : -i; }\n"
     "  return s;\n"
     "}\n",
     {0x88990ABULL}, "BitAlgo11", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Bit11 =
    makeBit11TC("x64b11", "long", 2, "");
static const std::vector<RoundTripTC> kA64Bit11 =
    makeBit11TC("a64b11", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Bit11 =
    makeBit11TC("armb11", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(BitAlgo11, X64BitAlgo11RT,
                         ::testing::ValuesIn(kX64Bit11), rtTCName);
INSTANTIATE_TEST_SUITE_P(BitAlgo11, A64BitAlgo11RT,
                         ::testing::ValuesIn(kA64Bit11), rtTCName);
INSTANTIATE_TEST_SUITE_P(BitAlgo11, ARM32BitAlgo11RT,
                         ::testing::ValuesIn(kARM32Bit11), rtTCName);
