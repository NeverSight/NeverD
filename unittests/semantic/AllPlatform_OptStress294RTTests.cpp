//===- AllPlatform_OptStress294RTTests.cpp - modular/mod-arith probe ======//
//
// -O2 integer kernels stressing modular arithmetic, power-of-2 masking and
// strength-reduced modulo codegen paths:
//
//   * modpow2   - x & (2^n-1) wrap reduction chains.
//   * modadd    - (a+b) mod M via conditional subtract (no DIV).
//   * modmul    - (a*b) mod M via widening multiply + subtract.
//   * ringbuf   - circular buffer index advance + accumulate.
//   * hashmod   - multiply-shift hash then mod 2^k mask.
//   * modinv    - modular inverse by repeated subtract (small modulus).
//
// All moduli are compile-time constants, all indices masked in range, and
// there is no division instruction -- so native and lifted agree bit-for-bit.
// All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress294RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress294RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress294RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress294RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress294RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress294RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress294RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress294RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress294TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // x & (2^n-1) wrap reduction chains.
    {p+"_modpow2",
     t+" "+p+"_modpow2("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned v=(h+i)&127u; v=(v*3u)&255u; v=(v+acc)&511u;\n"
     "    acc=acc*131u+v+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress294", 2},

    // (a+b) mod M via conditional subtract (no DIV).
    {p+"_modadd",
     t+" "+p+"_modadd("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned s=acc+h; if(s>=1000u) s-=1000u;\n"
     "    acc=s+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress294", 2},

    // (a*b) mod M via widening multiply + subtract.
    {p+"_modmul",
     t+" "+p+"_modmul("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned long long pr=(unsigned long long)h*(unsigned long long)(h>>3);\n"
     "    unsigned r=(unsigned)pr; while(r>=997u) r-=997u;\n"
     "    acc=acc*131u+r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress294", 2},

    // circular buffer index advance + accumulate.
    {p+"_ringbuf",
     t+" "+p+"_ringbuf("+t+" a){ unsigned buf[64]; unsigned h=(unsigned)a;\n"
     "  for(int i=0;i<64;i++) buf[i]=(unsigned)(a*(i+1));\n"
     "  unsigned acc=0, idx=0;\n"
     "  for(int i=0;i<200;i++){ h=h*1103515245u+12345u; idx=(idx+1+(h&7u))&63u; acc=acc*131u+buf[idx]+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress294", 2},

    // multiply-shift hash then mod 2^k mask.
    {p+"_hashmod",
     t+" "+p+"_hashmod("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned x=h*2654435761u; x^=x>>16; x&=1023u;\n"
     "    acc=acc*131u+x+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress294", 2},

    // modular inverse by repeated subtract (small modulus).
    {p+"_modinv",
     t+" "+p+"_modinv("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int k=0;k<40;k++){ h=h*1103515245u+12345u;\n"
     "    unsigned x=(h&63u)|1u, m=67u, inv=1u, guard=0;\n"
     "    while((inv*x)%m!=1u && guard<200){ inv=(inv+1u)%m; guard++; }\n"
     "    acc=acc*131u+inv+guard+(unsigned)k; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress294", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress294TC("x64o294", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress294TC("x86o294", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress294TC("a64o294", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress294TC("armo294", "int");

INSTANTIATE_TEST_SUITE_P(OptStress294, X64OptStress294RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress294, X86OptStress294RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress294, A64OptStress294RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress294, ARM32OptStress294RT, ::testing::ValuesIn(kARM), rtTCName);
