//===- AllPlatform_OptStress302RTTests.cpp - 64-bit atomic RMW (-O0) -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// -O0 sink-difference dual of OptStress301: the SAME 64-bit C11 atomic RMW
// kernels compiled at -O0.  Low optimization emits markedly different machine
// code — the 64-bit value is spilled to the stack and reloaded around every
// atomic, the register-pair atomic operates on frame-relative operands, and the
// retry loop / flag chain is left un-folded.  This is exactly where lift bugs
// hide that -O2 cleans up (cf. the -O0 duals in #508/#509/#512).
//
//   * i386  (-march=pentium): cmpxchg8b on stack-spilled EDX:EAX / ECX:EBX.
//   * arm32 (cortex-a15):     ldrexd/strexd even/odd pairs, frame-relative.
//   * x86-64 / aarch64:       native 64-bit atomics, -O0 spill form (controls).
//
// The atomic target is a file-scope 64-bit global; each kernel folds BOTH halves
// (`old ^ (old>>32)`) of every 64-bit old value into a 32-bit return so a high-
// word lift bug surfaces through the 32-bit i386/arm32 return register.  Single-
// thread → every CAS succeeds first try and results are deterministic.  64-bit
// math is multiply/shift/logic only (no 64-bit division) to stay libcall-free.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress302RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress302RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress302RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress302RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress302RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress302RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress302RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress302RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress302TC(const char *prefix,
                                                   const char *T,
                                                   const char *Flags) {
  std::string p = prefix, t = T, fl = Flags;
  std::vector<RoundTripTC> v = {
    // __atomic_fetch_add on a 64-bit global; both halves of old folded in.
    {p+"_qadd",
     "static unsigned long long g_q;\n"
     +t+" "+p+"_qadd("+t+" a){ unsigned long long h=((unsigned long long)(unsigned)a<<32)^0x9E3779B97F4A7C15ULL; g_q=h; unsigned acc=0;\n"
     "  for(int i=0;i<40;i++){ h=h*6364136223846793005ULL+1442695040888963407ULL;\n"
     "    unsigned long long old=__atomic_fetch_add(&g_q,h>>7,__ATOMIC_SEQ_CST);\n"
     "    acc=acc*131u+(unsigned)(old^(old>>32)); }\n"
     "  unsigned long long f=__atomic_load_n(&g_q,__ATOMIC_SEQ_CST);\n"
     "  return ("+t+")(acc+(unsigned)(f^(f>>32))); }\n",
     {0x1A2B3u}, "OptStress302", 0, fl},

    // fetch_or / fetch_and / fetch_xor chain on a 64-bit global.
    {p+"_qbits",
     "static unsigned long long g_q;\n"
     +t+" "+p+"_qbits("+t+" a){ unsigned long long h=((unsigned long long)(unsigned)a<<32)|1ULL; g_q=h; unsigned acc=0;\n"
     "  for(int i=0;i<40;i++){ h=h*6364136223846793005ULL+1442695040888963407ULL;\n"
     "    unsigned long long o1=__atomic_fetch_or(&g_q,(h>>5)&0xFF00FF00FFULL,__ATOMIC_SEQ_CST);\n"
     "    unsigned long long o2=__atomic_fetch_and(&g_q,~((h>>13)&0xF0F0ULL),__ATOMIC_SEQ_CST);\n"
     "    unsigned long long o3=__atomic_fetch_xor(&g_q,(h>>17)&0x5555555555ULL,__ATOMIC_SEQ_CST);\n"
     "    acc=acc*131u+(unsigned)((o1^o2^o3)^((o1^o2^o3)>>32)); }\n"
     "  unsigned long long f=__atomic_load_n(&g_q,__ATOMIC_SEQ_CST);\n"
     "  return ("+t+")(acc+(unsigned)(f^(f>>32))); }\n",
     {0x2B3C4u}, "OptStress302", 0, fl},

    // __atomic_exchange_n swap on a 64-bit global; old chained into next value.
    {p+"_qxchg",
     "static unsigned long long g_q;\n"
     +t+" "+p+"_qxchg("+t+" a){ unsigned long long h=((unsigned long long)(unsigned)a<<32)^0xABCDEF0123456789ULL; g_q=h; unsigned acc=0;\n"
     "  for(int i=0;i<40;i++){ h=h*6364136223846793005ULL+1442695040888963407ULL;\n"
     "    unsigned long long old=__atomic_exchange_n(&g_q,h^((unsigned long long)acc<<29),__ATOMIC_SEQ_CST);\n"
     "    acc=acc*131u+(unsigned)(old^(old>>32)); }\n"
     "  unsigned long long f=__atomic_load_n(&g_q,__ATOMIC_SEQ_CST);\n"
     "  return ("+t+")(acc+(unsigned)(f^(f>>32))); }\n",
     {0x3C4D5u}, "OptStress302", 0, fl},

    // __atomic_compare_exchange_n 64-bit running-max (lock-free CAS -> cmpxchg8b).
    {p+"_qcas",
     "static unsigned long long g_q;\n"
     +t+" "+p+"_qcas("+t+" a){ unsigned long long h=(unsigned long long)(unsigned)a+1; g_q=0; unsigned acc=0;\n"
     "  for(int i=0;i<40;i++){ h=h*6364136223846793005ULL+1442695040888963407ULL; unsigned long long val=h>>16;\n"
     "    unsigned long long cur=__atomic_load_n(&g_q,__ATOMIC_RELAXED);\n"
     "    while(val>cur){\n"
     "      if(__atomic_compare_exchange_n(&g_q,&cur,val,0,\n"
     "          __ATOMIC_SEQ_CST,__ATOMIC_RELAXED)) break; }\n"
     "    acc=acc*131u+(unsigned)(cur^(cur>>32)); }\n"
     "  unsigned long long f=__atomic_load_n(&g_q,__ATOMIC_SEQ_CST);\n"
     "  return ("+t+")(acc+(unsigned)(f^(f>>32))); }\n",
     {0x4D5E6u}, "OptStress302", 0, fl},

    // __atomic_fetch_sub borrow chain from a saturated 64-bit start.
    {p+"_qsub",
     "static unsigned long long g_q;\n"
     +t+" "+p+"_qsub("+t+" a){ unsigned long long h=(unsigned long long)(unsigned)a; g_q=0xFFFFFFFFFFFFFFFFULL; unsigned acc=0;\n"
     "  for(int i=0;i<40;i++){ h=h*6364136223846793005ULL+1442695040888963407ULL;\n"
     "    unsigned long long old=__atomic_fetch_sub(&g_q,(h>>20)&0x7FFFFFFULL,__ATOMIC_SEQ_CST);\n"
     "    acc=acc*131u+(unsigned)((old>>1)^(old>>33)); }\n"
     "  unsigned long long f=__atomic_load_n(&g_q,__ATOMIC_SEQ_CST);\n"
     "  return ("+t+")(acc+(unsigned)(f^(f>>32))); }\n",
     {0x5E6F7u}, "OptStress302", 0, fl},

    // fetch_add then branch on the 64-bit old value into distinct sub-word writes.
    {p+"_qbranch",
     "static unsigned long long g_q;\n"
     +t+" "+p+"_qbranch("+t+" a){ unsigned long long h=((unsigned long long)(unsigned)a<<31)|3ULL; g_q=h; unsigned acc=0;\n"
     "  for(int i=0;i<48;i++){ h=h*6364136223846793005ULL+1442695040888963407ULL;\n"
     "    unsigned long long old=__atomic_fetch_add(&g_q,(h>>23)&0x3FFULL,__ATOMIC_SEQ_CST);\n"
     "    unsigned o;\n"
     "    if((old&3ULL)==0ULL) o=(unsigned char)(old>>9);\n"
     "    else if((old&3ULL)==1ULL) o=(unsigned short)(old>>17);\n"
     "    else o=(unsigned)(old>>32)^0x9E37u;\n"
     "    acc=acc*131u+o+(unsigned)i; }\n"
     "  unsigned long long f=__atomic_load_n(&g_q,__ATOMIC_SEQ_CST);\n"
     "  return ("+t+")(acc+(unsigned)(f^(f>>32))); }\n",
     {0x6F708u}, "OptStress302", 0, fl},
  };
  return v;
}
// clang-format on

// i386 needs -march=pentium so 64-bit atomics inline to cmpxchg8b even at -O0.
static const std::vector<RoundTripTC> kX64 = makeOptStress302TC("x64o302", "long", "");
static const std::vector<RoundTripTC> kX86 = makeOptStress302TC("x86o302", "int", "-march=pentium");
static const std::vector<RoundTripTC> kA64 = makeOptStress302TC("a64o302", "long", "");
static const std::vector<RoundTripTC> kARM = makeOptStress302TC("armo302", "int", "");

INSTANTIATE_TEST_SUITE_P(OptStress302, X64OptStress302RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress302, X86OptStress302RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress302, A64OptStress302RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress302, ARM32OptStress302RT, ::testing::ValuesIn(kARM), rtTCName);
