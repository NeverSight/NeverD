//===- AllPlatform_OptStress319RTTests.cpp - -O2 indirect i64 thread -----===//
//
// The -O2 arm of the #521 ② KNOWN-OPEN that OptStress317 left open: the SAME
// `acc = fp(acc, c)` indirect-call chain (one 64-bit accumulator both passed to
// AND returned from an indirect-only callee each iteration), but compiled at -O2
// where clang keeps `acc` register-threaded across the chain instead of spilling
// it to the stack.  recoverCallAbi's fixpoint width inference truncates the i64
// argument of the chain's tail call (its result also feeds the function's i64
// return), dropping the high 32 bits on i386/ARM32 (register pair) while
// x86-64/AArch64 (one register per i64) stay correct.
//
// Same libcall-free i64 math as OptStress317 (multiply/shift/xor/add, constant
// shifts only); deterministic, all four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress319RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress319RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress319RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress319RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress319RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress319RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress319RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress319RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress319TC(const char *prefix,
                                                   const char *T, int Opt) {
  std::string p = prefix, t = T;
  std::vector<RoundTripTC> v = {
    {p+"_thread",
     "static long long "+p+"_ta(long long x,int b) __attribute__((noinline));\n"
     +t+" "+p+"_thread("+t+" a){ unsigned w=(unsigned)a|1u;\n"
     "  long long acc=(long long)(unsigned)a;\n"
     "  long long (*fp)(long long,int) = "+p+"_ta; __asm__(\"\" : \"+r\"(fp));\n"
     "  for(int i=0;i<40;i++){ w=w*1103515245u+12345u;\n"
     "    acc = fp(acc,(int)(w>>9)); acc ^= acc>>17; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n"
     "static long long "+p+"_ta(long long x,int b){\n"
     "  long long r=x*131 + (long long)b;\n"
     "  r ^= (long long)(unsigned)((unsigned long long)x>>32) << 28;\n"
     "  r += ((long long)b) << 33; return r; }\n",
     {0x1234u}, "OptStress319", Opt},

    {p+"_thread2",
     "static long long "+p+"_tb(long long x,int b) __attribute__((noinline));\n"
     +t+" "+p+"_thread2("+t+" a){ unsigned w=(unsigned)a^0x33u;\n"
     "  long long acc=(long long)(unsigned)a;\n"
     "  long long (*fp)(long long,int) = "+p+"_tb; __asm__(\"\" : \"+r\"(fp));\n"
     "  for(int i=0;i<36;i++){ w=w*22695477u+1u;\n"
     "    acc = fp(acc,(int)w); acc = fp(acc,(int)(w>>11));\n"
     "    acc ^= acc>>23; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n"
     "static long long "+p+"_tb(long long x,int b){\n"
     "  long long r=x*65537 + (long long)b*3;\n"
     "  r ^= (long long)(unsigned)((unsigned long long)x>>32) << 19;\n"
     "  r += ((long long)b) << 34; return r; }\n",
     {0x2345u}, "OptStress319", Opt},

    {p+"_threadmix",
     "static long long "+p+"_tc(long long x,int b,int c) __attribute__((noinline));\n"
     +t+" "+p+"_threadmix("+t+" a){ unsigned w=(unsigned)a+0x9u;\n"
     "  long long acc=(long long)(unsigned)a;\n"
     "  long long (*fp)(long long,int,int) = "+p+"_tc; __asm__(\"\" : \"+r\"(fp));\n"
     "  for(int i=0;i<36;i++){ w=w*1664525u+1013904223u;\n"
     "    acc = fp(acc,(int)w,(int)(w>>13)); acc ^= acc>>21; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n"
     "static long long "+p+"_tc(long long x,int b,int c){\n"
     "  long long r=x*131 + (long long)b - (long long)c*5;\n"
     "  r ^= (long long)(unsigned)((unsigned long long)x>>32) << 26;\n"
     "  r += ((long long)(b^c)) << 32; return r; }\n",
     {0x3456u}, "OptStress319", Opt},
  };
  return v;
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress319TC("x64o319", "long", 2);
static const std::vector<RoundTripTC> kX86 = makeOptStress319TC("x86o319", "int", 2);
static const std::vector<RoundTripTC> kA64 = makeOptStress319TC("a64o319", "long", 2);
static const std::vector<RoundTripTC> kARM = makeOptStress319TC("armo319", "int", 2);

INSTANTIATE_TEST_SUITE_P(OptStress319, X64OptStress319RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress319, X86OptStress319RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress319, A64OptStress319RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress319, ARM32OptStress319RT, ::testing::ValuesIn(kARM), rtTCName);
