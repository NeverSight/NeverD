//===- AllPlatform_OptStress301RTTests.cpp - 64-bit atomic RMW probe -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// -O2 integer kernels stressing C11 atomic read-modify-write on a 64-bit
// `unsigned long long` global, with the returned OLD value consumed across the
// loop body and control flow.  The point of this round is the 64-bit lowering
// on the 32-bit targets, where a 64-bit atomic uses a *register-pair* atomic
// instruction that #514 (4-byte atomics) never reached:
//
//   * i386  (-march=pentium): every 64-bit RMW becomes a `cmpxchg8b` retry loop
//                             (EDX:EAX = compare, ECX:EBX = desired — and EBX is
//                             also the PIC base, so clang must shuffle it).
//   * arm32 (cortex-a15):     64-bit RMW becomes `ldrexd/strexd` even/odd pairs.
//   * x86-64:                 native `lock xadd`/`cmpxchg`/`xchg` (control).
//   * aarch64 (baseline):     `ldaxr/stlxr` X-form LL-SC loops (control).
//
// The atomic target is a file-scope global so the address escapes and clang must
// emit real atomics.  Every kernel returns a 32-bit value that mixes BOTH halves
// (`old ^ (old>>32)`) of each 64-bit old value, so a high-word lift bug surfaces
// even through the 32-bit i386/arm32 return register (EAX / R0).  Single-thread,
// so each CAS succeeds first try and every result is deterministic.  All 64-bit
// math is multiply/shift/logic only — no 64-bit division — to stay libcall-free
// on the 32-bit targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress301RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress301RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress301RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress301RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress301RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress301RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress301RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress301RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress301TC(const char *prefix,
                                                   const char *T,
                                                   const char *Flags) {
  std::string p = prefix, t = T, fl = Flags;
  std::vector<RoundTripTC> v = {
    // __atomic_fetch_add on a 64-bit global; both halves of old folded in.
    {p+"_qadd",
     "static unsigned long long g_q;\n"
     +t+" "+p+"_qadd("+t+" a){ unsigned long long h=((unsigned long long)(unsigned)a<<32)^0x9E3779B97F4A7C15ULL; g_q=h; unsigned acc=0;\n"
     "  for(int i=0;i<48;i++){ h=h*6364136223846793005ULL+1442695040888963407ULL;\n"
     "    unsigned long long old=__atomic_fetch_add(&g_q,h>>7,__ATOMIC_SEQ_CST);\n"
     "    acc=acc*131u+(unsigned)(old^(old>>32)); }\n"
     "  unsigned long long f=__atomic_load_n(&g_q,__ATOMIC_SEQ_CST);\n"
     "  return ("+t+")(acc+(unsigned)(f^(f>>32))); }\n",
     {0x12345u}, "OptStress301", 2, fl},

    // fetch_or / fetch_and / fetch_xor chain on a 64-bit global.
    {p+"_qbits",
     "static unsigned long long g_q;\n"
     +t+" "+p+"_qbits("+t+" a){ unsigned long long h=((unsigned long long)(unsigned)a<<32)|1ULL; g_q=h; unsigned acc=0;\n"
     "  for(int i=0;i<48;i++){ h=h*6364136223846793005ULL+1442695040888963407ULL;\n"
     "    unsigned long long o1=__atomic_fetch_or(&g_q,(h>>5)&0xFF00FF00FFULL,__ATOMIC_SEQ_CST);\n"
     "    unsigned long long o2=__atomic_fetch_and(&g_q,~((h>>13)&0xF0F0ULL),__ATOMIC_SEQ_CST);\n"
     "    unsigned long long o3=__atomic_fetch_xor(&g_q,(h>>17)&0x5555555555ULL,__ATOMIC_SEQ_CST);\n"
     "    acc=acc*131u+(unsigned)((o1^o2^o3)^((o1^o2^o3)>>32)); }\n"
     "  unsigned long long f=__atomic_load_n(&g_q,__ATOMIC_SEQ_CST);\n"
     "  return ("+t+")(acc+(unsigned)(f^(f>>32))); }\n",
     {0x23456u}, "OptStress301", 2, fl},

    // __atomic_exchange_n swap on a 64-bit global; old chained into next value.
    {p+"_qxchg",
     "static unsigned long long g_q;\n"
     +t+" "+p+"_qxchg("+t+" a){ unsigned long long h=((unsigned long long)(unsigned)a<<32)^0xABCDEF0123456789ULL; g_q=h; unsigned acc=0;\n"
     "  for(int i=0;i<48;i++){ h=h*6364136223846793005ULL+1442695040888963407ULL;\n"
     "    unsigned long long old=__atomic_exchange_n(&g_q,h^((unsigned long long)acc<<29),__ATOMIC_SEQ_CST);\n"
     "    acc=acc*131u+(unsigned)(old^(old>>32)); }\n"
     "  unsigned long long f=__atomic_load_n(&g_q,__ATOMIC_SEQ_CST);\n"
     "  return ("+t+")(acc+(unsigned)(f^(f>>32))); }\n",
     {0x34567u}, "OptStress301", 2, fl},

    // __atomic_compare_exchange_n 64-bit running-max (lock-free CAS -> cmpxchg8b).
    {p+"_qcas",
     "static unsigned long long g_q;\n"
     +t+" "+p+"_qcas("+t+" a){ unsigned long long h=(unsigned long long)(unsigned)a+1; g_q=0; unsigned acc=0;\n"
     "  for(int i=0;i<48;i++){ h=h*6364136223846793005ULL+1442695040888963407ULL; unsigned long long val=h>>16;\n"
     "    unsigned long long cur=__atomic_load_n(&g_q,__ATOMIC_RELAXED);\n"
     "    while(val>cur){\n"
     "      if(__atomic_compare_exchange_n(&g_q,&cur,val,0,\n"
     "          __ATOMIC_SEQ_CST,__ATOMIC_RELAXED)) break; }\n"
     "    acc=acc*131u+(unsigned)(cur^(cur>>32)); }\n"
     "  unsigned long long f=__atomic_load_n(&g_q,__ATOMIC_SEQ_CST);\n"
     "  return ("+t+")(acc+(unsigned)(f^(f>>32))); }\n",
     {0x45678u}, "OptStress301", 2, fl},

    // __atomic_fetch_sub borrow chain from a saturated 64-bit start.
    {p+"_qsub",
     "static unsigned long long g_q;\n"
     +t+" "+p+"_qsub("+t+" a){ unsigned long long h=(unsigned long long)(unsigned)a; g_q=0xFFFFFFFFFFFFFFFFULL; unsigned acc=0;\n"
     "  for(int i=0;i<48;i++){ h=h*6364136223846793005ULL+1442695040888963407ULL;\n"
     "    unsigned long long old=__atomic_fetch_sub(&g_q,(h>>20)&0x7FFFFFFULL,__ATOMIC_SEQ_CST);\n"
     "    acc=acc*131u+(unsigned)((old>>1)^(old>>33)); }\n"
     "  unsigned long long f=__atomic_load_n(&g_q,__ATOMIC_SEQ_CST);\n"
     "  return ("+t+")(acc+(unsigned)(f^(f>>32))); }\n",
     {0x56789u}, "OptStress301", 2, fl},

    // fetch_add then branch on the 64-bit old value into distinct sub-word writes.
    {p+"_qbranch",
     "static unsigned long long g_q;\n"
     +t+" "+p+"_qbranch("+t+" a){ unsigned long long h=((unsigned long long)(unsigned)a<<31)|3ULL; g_q=h; unsigned acc=0;\n"
     "  for(int i=0;i<56;i++){ h=h*6364136223846793005ULL+1442695040888963407ULL;\n"
     "    unsigned long long old=__atomic_fetch_add(&g_q,(h>>23)&0x3FFULL,__ATOMIC_SEQ_CST);\n"
     "    unsigned o;\n"
     "    if((old&3ULL)==0ULL) o=(unsigned char)(old>>9);\n"
     "    else if((old&3ULL)==1ULL) o=(unsigned short)(old>>17);\n"
     "    else o=(unsigned)(old>>32)^0x9E37u;\n"
     "    acc=acc*131u+o+(unsigned)i; }\n"
     "  unsigned long long f=__atomic_load_n(&g_q,__ATOMIC_SEQ_CST);\n"
     "  return ("+t+")(acc+(unsigned)(f^(f>>32))); }\n",
     {0x6789Au}, "OptStress301", 2, fl},
  };
  return v;
}
// clang-format on

// i386 needs -march=pentium so 64-bit atomics inline to cmpxchg8b (the i386
// baseline has no cmpxchg8b); the other targets use their default CPU.
static const std::vector<RoundTripTC> kX64 = makeOptStress301TC("x64o301", "long", "");
static const std::vector<RoundTripTC> kX86 = makeOptStress301TC("x86o301", "int", "-march=pentium");
static const std::vector<RoundTripTC> kA64 = makeOptStress301TC("a64o301", "long", "");
static const std::vector<RoundTripTC> kARM = makeOptStress301TC("armo301", "int", "");

INSTANTIATE_TEST_SUITE_P(OptStress301, X64OptStress301RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress301, X86OptStress301RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress301, A64OptStress301RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress301, ARM32OptStress301RT, ::testing::ValuesIn(kARM), rtTCName);
