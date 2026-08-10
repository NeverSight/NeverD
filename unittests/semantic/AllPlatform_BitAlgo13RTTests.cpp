//===- AllPlatform_BitAlgo13RTTests.cpp - scalar loop-carried --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Thirteenth batch of clang -O2 algorithm probes.  Targets SCALAR loop-carried,
// branch-heavy algorithms that clang cannot auto-vectorize (each iteration
// depends on the previous), so the recompiled code stays scalar and stresses
// the custom MedIR optimizer: flag chains across blocks, loop-carried phi nodes,
// sub-register accumulators, and constant propagation.  Stein binary GCD,
// bit-by-bit integer sqrt, modular exponentiation, Adler-32, Collatz, Josephus,
// longest run of set bits, and digit-root.
//
// Every function folds to an exact integer return value.  Arithmetic is 32-bit;
// `/` and `%` are used (the ARM32 path compiles for cortex-a15 which has the
// hardware divide instructions, so no runtime library call is emitted).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64BitAlgo13RT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64BitAlgo13RT, Verify) { roundTripX64(GetParam()); }

class A64BitAlgo13RT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64BitAlgo13RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32BitAlgo13RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32BitAlgo13RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeBit13TC(const char *prefix, const char *T,
                                            int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Stein's binary GCD: shifts, swaps, many branches, loop-carried u/v.
    {p+"_bgcd",
     t+" "+p+"_bgcd("+t+" a) {\n"
     "  int s=0;\n"
     "  for (int i=1;i<=96;i++){ unsigned u=(unsigned)(a*i)&0xFFFFu, v=(unsigned)(a+i*7)&0xFFFFu;\n"
     "    if (u==0){ s+=(int)v; continue; } if (v==0){ s+=(int)u; continue; }\n"
     "    int sh=0; while(((u|v)&1u)==0){ u>>=1; v>>=1; sh++; }\n"
     "    while((u&1u)==0) u>>=1;\n"
     "    do { while((v&1u)==0) v>>=1; if(u>v){ unsigned t=u; u=v; v=t; } v-=u; } while(v);\n"
     "    s += (int)(u<<sh) - i; }\n"
     "  return s;\n"
     "}\n",
     {0x1234567ULL}, "BitAlgo13", opt, fl},

    // Bit-by-bit integer square root: loop-carried n/res/bit, branch per bit.
    {p+"_isqrtb",
     t+" "+p+"_isqrtb("+t+" a) {\n"
     "  int s=0;\n"
     "  for (int i=0;i<128;i++){ unsigned n=(unsigned)a*(unsigned)(i+1), res=0, bit=1u<<30;\n"
     "    while (bit>n) bit>>=2;\n"
     "    while (bit){ if(n>=res+bit){ n-=res+bit; res=(res>>1)+bit; } else res>>=1; bit>>=2; }\n"
     "    s += (int)res - i; }\n"
     "  return s;\n"
     "}\n",
     {0x2233445ULL}, "BitAlgo13", opt, fl},

    // Modular exponentiation by squaring: conditional multiply, mod.
    {p+"_modpow",
     t+" "+p+"_modpow("+t+" a) {\n"
     "  int s=0;\n"
     "  for (int i=1;i<=80;i++){ unsigned base=((unsigned)a+i)%97u, e=(unsigned)(i+3), r=1;\n"
     "    while (e){ if(e&1u) r=(r*base)%97u; base=(base*base)%97u; e>>=1; }\n"
     "    s += (int)r - i; }\n"
     "  return s;\n"
     "}\n",
     {0x3344556ULL}, "BitAlgo13", opt, fl},

    // Adler-32 style dual accumulator mod 65521 (prime).
    {p+"_adler",
     t+" "+p+"_adler("+t+" a) {\n"
     "  unsigned a1=1, b1=0;\n"
     "  for (int i=0;i<160;i++){ unsigned x=((unsigned)a+i*7)&0xFFu;\n"
     "    a1=(a1+x)%65521u; b1=(b1+a1)%65521u; }\n"
     "  return (int)((b1<<16)|a1);\n"
     "}\n",
     {0x4455667ULL}, "BitAlgo13", opt, fl},

    // Collatz: track step count and max value, odd/even branch, loop-carried.
    {p+"_collatz",
     t+" "+p+"_collatz("+t+" a) {\n"
     "  int s=0;\n"
     "  for (int i=1;i<=120;i++){ unsigned n=((unsigned)a+i)|1u; int steps=0; unsigned mx=n;\n"
     "    while (n!=1u && steps<300){ if(n&1u) n=3u*n+1u; else n>>=1; if(n>mx) mx=n; steps++; }\n"
     "    s += steps + (int)(mx&0xFFu) - i; }\n"
     "  return s;\n"
     "}\n",
     {0x5566778ULL}, "BitAlgo13", opt, fl},

    // Josephus survivor (iterative recurrence with modulo).
    {p+"_joseph",
     t+" "+p+"_joseph("+t+" a) {\n"
     "  int s=0;\n"
     "  for (int nn=1;nn<=72;nn++){ int k=(int)((unsigned)a%5u)+2; int r=0;\n"
     "    for (int x=2;x<=nn;x++) r=(r+k)%x;\n"
     "    s += r - nn; }\n"
     "  return s;\n"
     "}\n",
     {0x6677889ULL}, "BitAlgo13", opt, fl},

    // Longest run of consecutive set bits: per-bit branch, loop-carried run.
    {p+"_runones",
     t+" "+p+"_runones("+t+" a) {\n"
     "  int s=0;\n"
     "  for (int i=0;i<128;i++){ unsigned x=(unsigned)a*(unsigned)(i+1); int best=0,cur=0;\n"
     "    for (int b=0;b<32;b++){ if(x&(1u<<b)){ cur++; if(cur>best) best=cur; } else cur=0; }\n"
     "    s += best - i; }\n"
     "  return s;\n"
     "}\n",
     {0x778899AULL}, "BitAlgo13", opt, fl},

    // Digit root: repeated digit-sum until single digit (nested while, div/mod).
    {p+"_digitroot",
     t+" "+p+"_digitroot("+t+" a) {\n"
     "  int s=0;\n"
     "  for (int i=0;i<200;i++){ unsigned n=(unsigned)(a+i*131);\n"
     "    while (n>=10u){ unsigned t=0; while(n){ t+=n%10u; n/=10u; } n=t; }\n"
     "    s += (int)n - i; }\n"
     "  return s;\n"
     "}\n",
     {0x88990ABULL}, "BitAlgo13", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Bit13 =
    makeBit13TC("x64b13", "long", 2, "");
static const std::vector<RoundTripTC> kA64Bit13 =
    makeBit13TC("a64b13", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Bit13 =
    makeBit13TC("armb13", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(BitAlgo13, X64BitAlgo13RT,
                         ::testing::ValuesIn(kX64Bit13), rtTCName);
INSTANTIATE_TEST_SUITE_P(BitAlgo13, A64BitAlgo13RT,
                         ::testing::ValuesIn(kA64Bit13), rtTCName);
INSTANTIATE_TEST_SUITE_P(BitAlgo13, ARM32BitAlgo13RT,
                         ::testing::ValuesIn(kARM32Bit13), rtTCName);
