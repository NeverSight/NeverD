//===- AllPlatform_OptStress284RTTests.cpp - overflow/widen-mul probe =====//
//
// -O2 integer kernels stressing the multiply paths and the overflow-flag
// lowering that historically tripped MedFlags: __builtin_mul/add/sub_overflow
// (MUL/IMUL + SETO/SETC on x86, UMULH/SMULH compare on AArch64, UMULL/SMULL +
// compare on ARM32), 32x32->64 widening high-word multiply, and the constant
// multiply strength-reduction (mul -> shift+add) interplay.
//
//   * mulovf   - __builtin_mul_overflow accumulate (unsigned MUL + OF/CF).
//   * addovf   - __builtin_add/sub_overflow carry & borrow chain.
//   * mulhiu   - unsigned 32x32->64 high word (UMULH / EDX:EAX / UMULL).
//   * mulhis   - signed 32x32->64 high word (SMULH / IMUL hi / SMULL).
//   * madd     - multiply-accumulate chains (MADD/MLA, IMUL+ADD).
//   * mulconst - multiply-by-constant strength reduction (shift+add) mix.
//
// Heavy math stays in 32-bit (the 64-bit widening multiply lowers libcall-free
// on i386/ARM32); only the final value is cast to the platform return type.
// No division anywhere.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress284RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress284RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress284RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress284RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress284RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress284RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress284RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress284RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress284TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // __builtin_mul_overflow accumulate (unsigned MUL + overflow flag).
    {p+"_mulovf",
     t+" "+p+"_mulovf("+t+" a){ unsigned h=(unsigned)a|1u; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned pr; int ov=__builtin_mul_overflow(h, 2654435761u, &pr);\n"
     "    acc+=pr; if(ov) acc^=0xDEADBEEFu; acc=acc*131u+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress284", 2},

    // __builtin_add/sub_overflow carry & borrow chain.
    {p+"_addovf",
     t+" "+p+"_addovf("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned s; int c=__builtin_add_overflow(acc, h, &s);\n"
     "    unsigned d; int b=__builtin_sub_overflow(s, (h>>3), &d);\n"
     "    acc=d+(unsigned)c+(unsigned)b*7u+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress284", 2},

    // unsigned 32x32->64 high word (UMULH / EDX:EAX / UMULL).
    {p+"_mulhiu",
     t+" "+p+"_mulhiu("+t+" a){ unsigned h=(unsigned)a|1u; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned x=h, y=h^0x9E3779B9u;\n"
     "    unsigned hi=(unsigned)(((unsigned long long)x*(unsigned long long)y)>>32);\n"
     "    acc=acc*131u+hi+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress284", 2},

    // signed 32x32->64 high word (SMULH / IMUL hi / SMULL).
    {p+"_mulhis",
     t+" "+p+"_mulhis("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    int x=(int)h, y=(int)(h^0x5A5A5A5Au);\n"
     "    int hi=(int)(((long long)x*(long long)y)>>32);\n"
     "    acc=acc*131u+(unsigned)hi+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress284", 2},

    // multiply-accumulate chains (MADD/MLA, IMUL+ADD).
    {p+"_madd",
     t+" "+p+"_madd("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  unsigned b1=0x12345u, b2=0x6789u;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned m1=h*b1+b2; unsigned m2=(h>>5)*0x101u-acc;\n"
     "    acc=m1^m2; b1=b1*5u+1u; b2=b2+h; acc=acc*131u+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress284", 2},

    // multiply-by-constant strength reduction (shift+add) mix.
    {p+"_mulconst",
     t+" "+p+"_mulconst("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<92;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned v=h*3u+h*5u+h*9u+h*15u+h*17u;\n"
     "    unsigned w=(h*0x10001u)^(h*0x7Fu);\n"
     "    acc=acc*131u+v+w+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress284", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress284TC("x64o284", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress284TC("x86o284", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress284TC("a64o284", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress284TC("armo284", "int");

INSTANTIATE_TEST_SUITE_P(OptStress284, X64OptStress284RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress284, X86OptStress284RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress284, A64OptStress284RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress284, ARM32OptStress284RT, ::testing::ValuesIn(kARM), rtTCName);
