//===- AllPlatform_OptStress298RTTests.cpp - atomic RMW optimizer probe ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// -O2 integer kernels stressing C11 atomic read-modify-write under the
// optimizer, with the returned OLD value consumed across the loop body and
// control flow.  Atomics operate on a file-scope global so the address escapes
// and clang must emit real atomic instructions (a non-escaping local would be
// demoted to a plain register op at -O2):
//
//   * atadd    - __atomic_fetch_add, old value folded into a hash accumulator.
//   * atbits   - fetch_or / fetch_and / fetch_xor chain, old values mixed.
//   * atxchg   - __atomic_exchange_n swap, old value chained.
//   * atcas    - __atomic_compare_exchange_n running-max loop (lock-free).
//   * atsub    - __atomic_fetch_sub borrow chain.
//   * atbranch - fetch_add then branch on the old value into sub-word writes.
//
// This is the first C11-atomic-builtin coverage for AArch64/ARM32 at -O2:
// x86 lowers to `lock xadd`/`cmpxchg`/`xchg`, while AArch64 (baseline) and
// ARM32 lower to ldxr/stxr / ldrex/strex LL-SC loops.  Single-threaded, so the
// CAS always succeeds first try and every result is deterministic.  All atomics
// are 4-byte `unsigned` so i386/ARM32 stay libcall-free.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress298RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress298RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress298RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress298RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress298RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress298RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress298RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress298RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress298TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // __atomic_fetch_add: old value folded into a hash accumulator.
    {p+"_atadd",
     "static unsigned g_at;\n"
     +t+" "+p+"_atadd("+t+" a){ unsigned h=(unsigned)a; g_at=h; unsigned acc=0;\n"
     "  for(int i=0;i<64;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned old=__atomic_fetch_add(&g_at,(h>>8)&0xFFu,__ATOMIC_SEQ_CST);\n"
     "    acc=acc*131u+old; }\n"
     "  return ("+t+")(acc + __atomic_load_n(&g_at,__ATOMIC_SEQ_CST)); }\n",
     {0x12345u}, "OptStress298", 2},

    // fetch_or / fetch_and / fetch_xor chain, old values mixed.
    {p+"_atbits",
     "static unsigned g_at;\n"
     +t+" "+p+"_atbits("+t+" a){ unsigned h=(unsigned)a; g_at=h|1u; unsigned acc=0;\n"
     "  for(int i=0;i<64;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned o1=__atomic_fetch_or(&g_at,(h>>5)&0xFFu,__ATOMIC_SEQ_CST);\n"
     "    unsigned o2=__atomic_fetch_and(&g_at,~((h>>13)&0xFu),__ATOMIC_SEQ_CST);\n"
     "    unsigned o3=__atomic_fetch_xor(&g_at,(h>>17)&0x55u,__ATOMIC_SEQ_CST);\n"
     "    acc=acc*131u+(o1^o2^o3); }\n"
     "  return ("+t+")(acc + __atomic_load_n(&g_at,__ATOMIC_SEQ_CST)); }\n",
     {0x23456u}, "OptStress298", 2},

    // __atomic_exchange_n swap, old value chained into next swapped-in value.
    {p+"_atxchg",
     "static unsigned g_at;\n"
     +t+" "+p+"_atxchg("+t+" a){ unsigned h=(unsigned)a; g_at=h; unsigned acc=0;\n"
     "  for(int i=0;i<64;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned old=__atomic_exchange_n(&g_at,h^acc,__ATOMIC_SEQ_CST);\n"
     "    acc=acc*131u+old; }\n"
     "  return ("+t+")(acc + __atomic_load_n(&g_at,__ATOMIC_SEQ_CST)); }\n",
     {0x34567u}, "OptStress298", 2},

    // __atomic_compare_exchange_n running-max loop (lock-free CAS).
    {p+"_atcas",
     "static unsigned g_at;\n"
     +t+" "+p+"_atcas("+t+" a){ unsigned h=(unsigned)a; g_at=0; unsigned acc=0;\n"
     "  for(int i=0;i<64;i++){ h=h*1103515245u+12345u; unsigned v=(h>>3)&0xFFFFu;\n"
     "    unsigned cur=__atomic_load_n(&g_at,__ATOMIC_RELAXED);\n"
     "    while(v>cur){\n"
     "      if(__atomic_compare_exchange_n(&g_at,&cur,v,0,\n"
     "          __ATOMIC_SEQ_CST,__ATOMIC_RELAXED)) break; }\n"
     "    acc=acc*131u+cur; }\n"
     "  return ("+t+")(acc + __atomic_load_n(&g_at,__ATOMIC_SEQ_CST)); }\n",
     {0x45678u}, "OptStress298", 2},

    // __atomic_fetch_sub borrow chain from a saturated start.
    {p+"_atsub",
     "static unsigned g_at;\n"
     +t+" "+p+"_atsub("+t+" a){ unsigned h=(unsigned)a; g_at=0xFFFFFFFFu; unsigned acc=0;\n"
     "  for(int i=0;i<64;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned old=__atomic_fetch_sub(&g_at,(h>>9)&0x7Fu,__ATOMIC_SEQ_CST);\n"
     "    acc=acc*131u+(old>>1); }\n"
     "  return ("+t+")(acc + __atomic_load_n(&g_at,__ATOMIC_SEQ_CST)); }\n",
     {0x56789u}, "OptStress298", 2},

    // fetch_add then branch on the old value into distinct sub-word writes.
    {p+"_atbranch",
     "static unsigned g_at;\n"
     +t+" "+p+"_atbranch("+t+" a){ unsigned h=(unsigned)a; g_at=h; unsigned acc=0;\n"
     "  for(int i=0;i<72;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned old=__atomic_fetch_add(&g_at,(h>>7)&0x3Fu,__ATOMIC_SEQ_CST);\n"
     "    unsigned o;\n"
     "    if((old&3u)==0u) o=(unsigned char)(old>>1);\n"
     "    else if((old&3u)==1u) o=(unsigned short)(old*3u);\n"
     "    else o=old^0x9E37u;\n"
     "    acc=acc*131u+o+(unsigned)i; }\n"
     "  return ("+t+")(acc + __atomic_load_n(&g_at,__ATOMIC_SEQ_CST)); }\n",
     {0x6789Au}, "OptStress298", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress298TC("x64o298", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress298TC("x86o298", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress298TC("a64o298", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress298TC("armo298", "int");

INSTANTIATE_TEST_SUITE_P(OptStress298, X64OptStress298RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress298, X86OptStress298RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress298, A64OptStress298RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress298, ARM32OptStress298RT, ::testing::ValuesIn(kARM), rtTCName);
