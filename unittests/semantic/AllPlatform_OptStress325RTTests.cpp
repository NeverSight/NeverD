//===- AllPlatform_OptStress325RTTests.cpp - -Os/-Oz i64 call threading -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// The i64-threaded-through-calls hot spot (#311/#441/#518/#524) under the size
// optimizer.  #524 fixed "-O2 register-threaded indirect i64 call chains"; this
// re-exercises the same `acc = f(acc, c)` threading where one 64-bit accumulator
// is simultaneously a call argument and the call's return, but at -Os and -Oz —
// whose register allocation, call scheduling and (at -Oz) MachineOutliner-driven
// code shapes differ from every -O0/-O2/-O3 call probe so far.
//
// 32-bit targets return i64 in a register pair (EDX:EAX / R1:R0); callees use
// only 32x32->64 widening multiply + shifts/xor so both halves are real and the
// bare-metal harness stays libcall-free (no i64 div, no i64 variable shift).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress325RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress325RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress325RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress325RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress325RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress325RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress325RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress325RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress325TC(const char *prefix,
                                                   const char *T) {
  std::string p = prefix, t = T;
  std::vector<RoundTripTC> v = {
    // Single noinline call per iter, i64 acc threaded (arg + return) at -Os.
    {p+"_thread",
     "static long long "+p+"_ta(long long acc,int c) __attribute__((noinline));\n"
     +t+" "+p+"_thread("+t+" a){ unsigned w=(unsigned)a|1u; long long acc=(unsigned)a;\n"
     "  for(int i=0;i<40;i++){ w=w*1103515245u+12345u; acc="+p+"_ta(acc,(int)w); }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n"
     "static long long "+p+"_ta(long long acc,int c){\n"
     "  unsigned lo=(unsigned)acc, hi=(unsigned)(acc>>32);\n"
     "  long long m=(long long)(int)lo*(long long)c;\n"
     "  return m ^ ((long long)hi<<32) ^ (acc<<1); }\n",
     {0x1234u}, "OptStress325", 2, "-Os"},

    // Back-to-back threading acc=f(f(acc)) at -Os: stresses "second call arg vs
    // first call's return register pair" on 32-bit.
    {p+"_thread2",
     "static long long "+p+"_tb(long long acc,int c) __attribute__((noinline));\n"
     +t+" "+p+"_thread2("+t+" a){ unsigned w=(unsigned)a^0x33u; long long acc=(unsigned)a|7u;\n"
     "  for(int i=0;i<32;i++){ w=w*22695477u+1u;\n"
     "    acc="+p+"_tb("+p+"_tb(acc,(int)w),(int)(w>>8)); }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n"
     "static long long "+p+"_tb(long long acc,int c){\n"
     "  unsigned lo=(unsigned)acc; long long m=(long long)(int)lo*(long long)c;\n"
     "  return (acc>>3) ^ m ^ ((long long)(unsigned)(acc>>32)<<32); }\n",
     {0x2345u}, "OptStress325", 2, "-Os"},

    // Threaded i64 acc + extra int args (3-arg callee) at -Os.
    {p+"_threadmix",
     "static long long "+p+"_tc(int x,long long acc,int y) __attribute__((noinline));\n"
     +t+" "+p+"_threadmix("+t+" a){ unsigned w=(unsigned)a+0x9u; long long acc=(unsigned)a;\n"
     "  for(int i=0;i<40;i++){ w=w*1664525u+1013904223u;\n"
     "    acc="+p+"_tc((int)w,acc,(int)(w>>11)); }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n"
     "static long long "+p+"_tc(int x,long long acc,int y){\n"
     "  long long m=(long long)x*(long long)y;\n"
     "  return (acc^m) + ((long long)(unsigned)(acc>>32)<<32) + (acc<<5); }\n",
     {0x3456u}, "OptStress325", 2, "-Os"},

    // Indirect call (fn ptr) threading i64 at -Os (the #518/#524 indirect arm).
    {p+"_indthread",
     "static long long "+p+"_td(long long acc,int c) __attribute__((noinline));\n"
     +t+" "+p+"_indthread("+t+" a){ unsigned w=(unsigned)a+0x55u; long long acc=(unsigned)a|1u;\n"
     "  long long (*fp)(long long,int)="+p+"_td; __asm__(\"\":\"+r\"(fp));\n"
     "  for(int i=0;i<40;i++){ w=w*1103515245u+12345u; acc=fp(acc,(int)w); }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n"
     "static long long "+p+"_td(long long acc,int c){\n"
     "  unsigned lo=(unsigned)acc; long long m=(long long)(int)lo*(long long)c;\n"
     "  return m ^ (acc>>7) ^ ((long long)(unsigned)(acc>>32)<<32); }\n",
     {0x4567u}, "OptStress325", 2, "-Os"},

    // Same single-thread shape at -Oz (MachineOutliner + minsize scheduling).
    {p+"_thread_oz",
     "static long long "+p+"_te(long long acc,int c) __attribute__((noinline));\n"
     +t+" "+p+"_thread_oz("+t+" a){ unsigned w=(unsigned)a|1u; long long acc=(unsigned)a;\n"
     "  for(int i=0;i<40;i++){ w=w*1103515245u+12345u; acc="+p+"_te(acc,(int)w); }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n"
     "static long long "+p+"_te(long long acc,int c){\n"
     "  unsigned lo=(unsigned)acc; long long m=(long long)(int)lo*(long long)c;\n"
     "  return m ^ (acc<<1) ^ ((long long)(unsigned)(acc>>32)<<32); }\n",
     {0x5678u}, "OptStress325", 2, "-Oz"},

    // Indirect i64 threading at -Oz.
    {p+"_indthread_oz",
     "static long long "+p+"_tf(long long acc,int c) __attribute__((noinline));\n"
     +t+" "+p+"_indthread_oz("+t+" a){ unsigned w=(unsigned)a^0xa5u; long long acc=(unsigned)a|3u;\n"
     "  long long (*fp)(long long,int)="+p+"_tf; __asm__(\"\":\"+r\"(fp));\n"
     "  for(int i=0;i<32;i++){ w=w*22695477u+1u; acc=fp(acc,(int)w); }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n"
     "static long long "+p+"_tf(long long acc,int c){\n"
     "  unsigned lo=(unsigned)acc; long long m=(long long)(int)lo*(long long)c;\n"
     "  return (acc>>5) ^ m ^ ((long long)(unsigned)(acc>>32)<<32); }\n",
     {0x6789u}, "OptStress325", 2, "-Oz"},
  };
  return v;
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress325TC("x64o325", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress325TC("x86o325", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress325TC("a64o325", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress325TC("armo325", "int");

INSTANTIATE_TEST_SUITE_P(OptStress325, X64OptStress325RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress325, X86OptStress325RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress325, A64OptStress325RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress325, ARM32OptStress325RT, ::testing::ValuesIn(kARM), rtTCName);
