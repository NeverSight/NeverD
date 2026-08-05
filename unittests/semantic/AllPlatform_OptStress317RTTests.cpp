//===- AllPlatform_OptStress317RTTests.cpp - indirect i64 THREADED call --===//
//
// The INDIRECT i64-THREADED variant deliberately deferred by OptStress315/316
// (#521 KNOWN-OPEN ②): an `acc = fp(acc, c)` chain where the SAME 64-bit
// accumulator is BOTH passed to AND returned from an indirect-only callee every
// iteration.  OptStress315 already proved the non-threaded directions are sound:
// a fresh i64 ARGUMENT through a function pointer (`fp(x, ...)`) and an i64
// RETURN consumed locally (`r = fp(x, ...)`) both round-trip.  The open question
// this probe asks is the THREADED composition: when the i64 result of one
// indirect call immediately becomes the i64 argument of the next, do both the
// high-half RETURN splice and the high-half ARGUMENT split survive together?
//
// On a 32-bit target a `long long` lives in a register pair (i386 EDX:EAX,
// ARM32 R1:R0) on the way out and a pair/stack-pair on the way in.  A lift that
// drops the high word of an indirect call's i64 RETURN (the indirect form of the
// #311/#441 register-pair return family — direct-call return widening keys on a
// constant CALL target, which an indirect-only callee never is), or that drops
// the high word of the threaded i64 ARGUMENT, corrupts the high 32 bits on
// i386/ARM32 while x86-64/AArch64 (one register carries the whole i64) stay
// correct.
//
//   * thread   - one indirect call per iter, acc threaded as arg AND return.
//   * thread2  - TWO indirect calls per iter, acc threaded through both (stresses
//                first-indirect-call argument recovery + back-to-back threading).
//   * threadmix- threaded i64 acc plus an extra small-int argument (3-arg callee).
//   * threaddir- direct-call control: the pointer is kept live (GOTOFF reloc slot
//                still exists) but the call is DIRECT, so any threaded-i64 fix is
//                proven orthogonal to call form.
//
// Each kernel keeps the function pointer live through `asm("" : "+r"(fp))` so the
// indirect call is genuinely emitted (not devirtualized), and every callee
// genuinely consumes both halves of its i64 argument and materializes both halves
// of its i64 return, so a dropped high word changes the result.  i64 math is
// multiply-by-constant / shift / xor / add only (no 64-bit division) to stay
// libcall-free on the 32-bit targets.  x86-64/AArch64 are controls.  Deterministic
// (LCG-seeded), -O0, all four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress317RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress317RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress317RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress317RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress317RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress317RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress317RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress317RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress317TC(const char *prefix,
                                                   const char *T, int Opt) {
  std::string p = prefix, t = T;
  std::vector<RoundTripTC> v = {
    // acc threaded as BOTH the i64 argument and the i64 return of one indirect
    // call per iteration: r=fp(r,c).  Both halves must survive the round trip.
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
     {0x1234u}, "OptStress317", Opt},

    // Two indirect calls per iteration, acc threaded through BOTH back to back.
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
     {0x2345u}, "OptStress317", Opt},

    // Threaded i64 acc plus an extra small-int argument (3-arg callee).
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
     {0x3456u}, "OptStress317", Opt},

    // Direct-call control: pointer kept live (GOTOFF reloc slot exists) but the
    // call is DIRECT — the threaded-i64 behavior must match the indirect form.
    {p+"_threaddir",
     "static long long "+p+"_td(long long x,int b) __attribute__((noinline));\n"
     +t+" "+p+"_threaddir("+t+" a){ unsigned w=(unsigned)a+0x11u;\n"
     "  long long acc=(long long)(unsigned)a;\n"
     "  long long (*fp)(long long,int) = "+p+"_td; __asm__(\"\" : \"+r\"(fp));\n"
     "  for(int i=0;i<32;i++){ w=w*22695477u+1u;\n"
     "    acc = "+p+"_td(acc,(int)(w>>3)); acc ^= acc>>25; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n"
     "static long long "+p+"_td(long long x,int b){\n"
     "  long long r=x*131 + (long long)b;\n"
     "  r ^= (long long)(unsigned)((unsigned long long)x>>32) << 28;\n"
     "  r += ((long long)b) << 33; return r; }\n",
     {0x6789u}, "OptStress317", Opt},
  };
  return v;
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress317TC("x64o317", "long", 0);
static const std::vector<RoundTripTC> kX86 = makeOptStress317TC("x86o317", "int", 0);
static const std::vector<RoundTripTC> kA64 = makeOptStress317TC("a64o317", "long", 0);
static const std::vector<RoundTripTC> kARM = makeOptStress317TC("armo317", "int", 0);

INSTANTIATE_TEST_SUITE_P(OptStress317, X64OptStress317RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress317, X86OptStress317RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress317, A64OptStress317RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress317, ARM32OptStress317RT, ::testing::ValuesIn(kARM), rtTCName);
