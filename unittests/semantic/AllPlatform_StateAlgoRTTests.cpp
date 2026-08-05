//===- AllPlatform_StateAlgoRTTests.cpp - bit/fixed/state kernels -*- C++-*-=//
//
// clang -O2 algorithm-level "high-yield probing", second batch.  Kernels avoid
// rodata table lookups (a known deferred lift gap, see #359) and runtime
// division (no libcall on any arch), focusing on bit manipulation, fixed-point,
// and data-dependent state machines — dense distinct lowerings across
// x64/aarch64/arm32:
//   * bgcd     - binary (Stein) GCD: ctz loops, shifts, swaps, conditional sub.
//   * bitrev   - 32-bit bit reversal via divide-and-conquer mask/shift swaps.
//   * gray     - Gray-code encode + decode (xor/shift accumulation loop).
//   * collatz  - Collatz step counts (data-dependent branch + 3n+1 / n>>1).
//   * xmix     - xorshift PRNG + multiply-xorshift avalanche mix.
//   * modexp   - modular exponentiation, square-and-multiply, CONSTANT modulus
//                (so `% M` lowers to magic-multiply, never a div libcall).
//
// All bounded 16/32-bit, local scalars only, deterministic; the harness runs
// native vs lifted and compares the folded return, so any lowering divergence
// surfaces as a mismatch.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64StateAlgoRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64StateAlgoRT, Verify) { roundTripX64(GetParam()); }

class A64StateAlgoRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64StateAlgoRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32StateAlgoRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32StateAlgoRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeStateTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Binary (Stein) GCD.
    {p+"_bgcd",
     t+" "+p+"_bgcd("+t+" a){\n"
     "  unsigned acc=0;\n"
     "  for(int k=0;k<40;k++){\n"
     "    unsigned u=((unsigned)(a*7u+k*131u))|1u;\n"
     "    unsigned v=((unsigned)(a*13u+k*97u))+2u; if(v==0)v=1u;\n"
     "    int sh=0; while(((u|v)&1u)==0){ u>>=1; v>>=1; sh++; }\n"
     "    while((u&1u)==0) u>>=1;\n"
     "    do{ while((v&1u)==0) v>>=1; if(u>v){unsigned t=u;u=v;v=t;} v-=u; }while(v);\n"
     "    acc=acc*131u+(u<<sh); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x1234567ULL}, "StateAlgo", 2, ""},

    // 32-bit bit reversal.
    {p+"_bitrev",
     t+" "+p+"_bitrev("+t+" a){\n"
     "  unsigned acc=0;\n"
     "  for(int k=0;k<48;k++){\n"
     "    unsigned x=(unsigned)(a*(k+1)+k*7);\n"
     "    x=((x&0x55555555u)<<1)|((x>>1)&0x55555555u);\n"
     "    x=((x&0x33333333u)<<2)|((x>>2)&0x33333333u);\n"
     "    x=((x&0x0F0F0F0Fu)<<4)|((x>>4)&0x0F0F0F0Fu);\n"
     "    x=((x&0x00FF00FFu)<<8)|((x>>8)&0x00FF00FFu);\n"
     "    x=(x<<16)|(x>>16);\n"
     "    acc=acc*131u+x; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x2233445ULL}, "StateAlgo", 2, ""},

    // Gray-code encode then decode.
    {p+"_gray",
     t+" "+p+"_gray("+t+" a){\n"
     "  unsigned acc=0;\n"
     "  for(int k=0;k<48;k++){\n"
     "    unsigned x=(unsigned)(a*(k+1)+k*3);\n"
     "    unsigned g=x^(x>>1), b=g;\n"
     "    for(unsigned m=g>>1;m;m>>=1) b^=m;\n"
     "    acc=acc*131u+(g^b); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x3344556ULL}, "StateAlgo", 2, ""},

    // Collatz step counts (data-dependent control flow).
    {p+"_collatz",
     t+" "+p+"_collatz("+t+" a){\n"
     "  unsigned acc=0;\n"
     "  for(int k=0;k<40;k++){\n"
     "    unsigned n=(((unsigned)(a*7u+k*131u))&0xFFFFu)|1u, steps=0;\n"
     "    while(n!=1u && steps<200u){ if(n&1u) n=3u*n+1u; else n>>=1; steps++; }\n"
     "    acc=acc*131u+steps; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x4455667ULL}, "StateAlgo", 2, ""},

    // xorshift PRNG + avalanche mix.
    {p+"_xmix",
     t+" "+p+"_xmix("+t+" a){\n"
     "  unsigned s=((unsigned)a)|1u, acc=0;\n"
     "  for(int k=0;k<80;k++){\n"
     "    s^=s<<13; s^=s>>17; s^=s<<5;\n"
     "    unsigned h=s; h^=h>>16; h*=0x85EBCA6Bu; h^=h>>13; h*=0xC2B2AE35u; h^=h>>16;\n"
     "    acc+=h; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x5566778ULL}, "StateAlgo", 2, ""},

    // Modular exponentiation (square-and-multiply, constant modulus).
    {p+"_modexp",
     t+" "+p+"_modexp("+t+" a){\n"
     "  unsigned acc=0, M=50021u;\n"
     "  for(int k=0;k<32;k++){\n"
     "    unsigned b=((unsigned)(a*7u+k*131u))%M;\n"
     "    unsigned e=(((unsigned)(a+k*13))&0x3Fu)+1u, r=1u;\n"
     "    while(e){ if(e&1u) r=(r*b)%M; b=(b*b)%M; e>>=1; }\n"
     "    acc=acc*131u+r; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x6677889ULL}, "StateAlgo", 2, ""},
  };
}

static const std::vector<RoundTripTC> kX64S   = makeStateTC("x64s", "long");
static const std::vector<RoundTripTC> kA64S   = makeStateTC("a64s", "long");
static const std::vector<RoundTripTC> kARM32S = makeStateTC("arms", "int");
// clang-format on

INSTANTIATE_TEST_SUITE_P(StateAlgo, X64StateAlgoRT,
                         ::testing::ValuesIn(kX64S), rtTCName);
INSTANTIATE_TEST_SUITE_P(StateAlgo, A64StateAlgoRT,
                         ::testing::ValuesIn(kA64S), rtTCName);
INSTANTIATE_TEST_SUITE_P(StateAlgo, ARM32StateAlgoRT,
                         ::testing::ValuesIn(kARM32S), rtTCName);
