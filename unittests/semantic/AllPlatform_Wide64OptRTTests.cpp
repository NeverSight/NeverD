//===- AllPlatform_Wide64OptRTTests.cpp - i64 optimizer stress --*-C++*-=//
//
// Optimizer-stress probes built entirely from 64-bit integer arithmetic.  On
// the 32-bit targets (i386 / ARM32) every i64 value legalizes to a register
// *pair*, so the self-written MedIR passes (MedFlags, MedPropagation, MedDCE,
// LowToMed sub-register modelling) run over two-word values — a path far less
// exercised than the 32-bit-internal kernels in OptStress*.  Kernels use only
// ops that legalize inline on 32-bit: i64 add/sub/mul-by-value/and/or/xor/not/
// neg, *constant* i64 shifts and rotates, i64 signed/unsigned compares and
// selects.  They deliberately avoid a variable i64 shift or i64 divide (the only
// i64 ops needing __ashldi3 / __divdi3, which the bare-metal harness cannot
// resolve).  Each folds to a single integer return, compiled -O2, native vs
// lifted on all four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64Wide64OptRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64Wide64OptRT, Verify) { roundTripX64(GetParam()); }
class X86Wide64OptRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86Wide64OptRT, Verify) { roundTripX86(GetParam()); }
class A64Wide64OptRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64Wide64OptRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32Wide64OptRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32Wide64OptRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeW64Opt(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // i64 LCG state, sign-test and unsigned-compare driven conditional accumulate
    // plus an i64 FNV mix: two-word signed/unsigned flag idioms + select.
    {p+"_condacc",
     t+" "+p+"_condacc("+t+" a){\n"
     "  unsigned long long acc=(unsigned long long)a|1ull, h=0;\n"
     "  for(int i=0;i<64;i++){ acc=acc*6364136223846793005ull+1442695040888963407ull;\n"
     "    long long s=(long long)acc; unsigned long long u=acc^(acc>>29);\n"
     "    if(s<0) h+=u; else h-=u;\n"
     "    if(u>acc) h^=(u<<1); else h+=(acc>>7);\n"
     "    h=h*1099511628211ull ^ acc; }\n"
     "  return ("+t+")(unsigned long long)(h^(h>>32)); }\n",
     {0x41ULL}, "Wide64Opt", 2},

    // Running i64 signed min/max via conditional updates (two-word compare+cmov).
    {p+"_minmax",
     t+" "+p+"_minmax("+t+" a){\n"
     "  unsigned long long s=(unsigned long long)a|1ull, h=0;\n"
     "  long long mn=0x7fffffffffffffffll, mx=(long long)0x8000000000000000ull;\n"
     "  for(int i=0;i<64;i++){ s=s*6364136223846793005ull+1442695040888963407ull;\n"
     "    long long x=(long long)s;\n"
     "    if(x<mn)mn=x; if(x>mx)mx=x;\n"
     "    unsigned long long span=(unsigned long long)mx-(unsigned long long)mn;\n"
     "    h=h*131u+(unsigned long long)mn+(unsigned long long)mx+span; }\n"
     "  return ("+t+")(unsigned long long)(h^(h>>32)); }\n",
     {0x53ULL}, "Wide64Opt", 2},

    // 32x32->64 widening multiply accumulated into an i64, both halves extracted
    // (sub-register reads of the two-word result).
    {p+"_muladd",
     t+" "+p+"_muladd("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned long long acc=0, h=0;\n"
     "  for(int i=0;i<64;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned long long pr=(unsigned long long)s*(unsigned long long)(s^0x5a5a5a5au);\n"
     "    acc+=pr; acc^=acc>>31;\n"
     "    unsigned lo=(unsigned)acc, hi=(unsigned)(acc>>32);\n"
     "    h=h*131u+lo+hi; }\n"
     "  return ("+t+")(unsigned long long)(h^(h>>32)); }\n",
     {0x6dULL}, "Wide64Opt", 2},

    // i64 SWAR popcount idiom: i64 mask/add/mul-by-const/constant-shift; stresses
    // constant folding and masking at the full 64-bit width.
    {p+"_popc",
     t+" "+p+"_popc("+t+" a){\n"
     "  unsigned long long s=(unsigned long long)a|1ull, h=0;\n"
     "  for(int i=0;i<64;i++){ s=s*6364136223846793005ull+1ull;\n"
     "    unsigned long long x=s;\n"
     "    x=x-((x>>1)&0x5555555555555555ull);\n"
     "    x=(x&0x3333333333333333ull)+((x>>2)&0x3333333333333333ull);\n"
     "    x=(x+(x>>4))&0x0f0f0f0f0f0f0f0full;\n"
     "    unsigned long long pc=(x*0x0101010101010101ull)>>56;\n"
     "    h=h*131u+pc+(s&0xffull); }\n"
     "  return ("+t+")(unsigned long long)(h^(h>>32)); }\n",
     {0xb2ULL}, "Wide64Opt", 2},

    // Two i64 lanes with add/sub, constant rotate-by-1, xor: two-word carry and
    // borrow chains plus a 64-bit funnel (shld/shrd-style) rotate.
    {p+"_carry",
     t+" "+p+"_carry("+t+" a){\n"
     "  unsigned long long x=((unsigned long long)(unsigned)a<<32)|((unsigned)a^0x9e3779b9u);\n"
     "  unsigned long long y=((unsigned long long)(unsigned)(a^0x55)<<32)|((unsigned)(a*7u));\n"
     "  unsigned long long h=0;\n"
     "  for(int i=0;i<64;i++){ unsigned long long s=x+y, d=x-y;\n"
     "    x=(x<<1)|(x>>63); y=s^d^(y>>1);\n"
     "    h+=s; h^=d; h=h*131u+(h>>32); }\n"
     "  return ("+t+")(unsigned long long)(h^(h>>32)); }\n",
     {0x18ULL}, "Wide64Opt", 2},

    // Branchless i64 select (min) and i64 abs (negate with borrow) reusing one
    // signed compare each iteration.
    {p+"_selabs",
     t+" "+p+"_selabs("+t+" a){\n"
     "  unsigned long long s=(unsigned long long)a|1ull, h=0;\n"
     "  for(int i=0;i<64;i++){ s=s*6364136223846793005ull+1442695040888963407ull;\n"
     "    long long x=(long long)s, y=(long long)(s^0x1234567890abcdefull);\n"
     "    unsigned long long m=(x<y)?(unsigned long long)x:(unsigned long long)y;\n"
     "    unsigned long long n=(x<0)?(unsigned long long)(-x):(unsigned long long)x;\n"
     "    h+=m^n; h=h*131u; }\n"
     "  return ("+t+")(unsigned long long)(h^(h>>32)); }\n",
     {0x2fULL}, "Wide64Opt", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeW64Opt("x64w64o", "long");
static const std::vector<RoundTripTC> kX86 = makeW64Opt("x86w64o", "int");
static const std::vector<RoundTripTC> kA64 = makeW64Opt("a64w64o", "long");
static const std::vector<RoundTripTC> kARM = makeW64Opt("armw64o", "int");

INSTANTIATE_TEST_SUITE_P(Wide64Opt, X64Wide64OptRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(Wide64Opt, X86Wide64OptRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(Wide64Opt, A64Wide64OptRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(Wide64Opt, ARM32Wide64OptRT, ::testing::ValuesIn(kARM), rtTCName);
