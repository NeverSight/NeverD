//===- AllPlatform_OptStress299RTTests.cpp - -O0 atomic RMW probe =========//
//
// -O0 sink differential of the OptStress298 atomic kernels.  At -O0 clang
// keeps atomics on a non-escaping stack local (no demotion), so the RMW targets
// a frame slot: `lock xadd [rbp-k]` (x86), `ldxr/stxr [sp+k]` (AArch64),
// `ldrex/strex [sp+k]` (ARM32) -- a stack-relative atomic addressing form not
// exercised by 298 (which used a global to force escape at -O2).
//
//   * atadd / atbits / atxchg / atcas / atsub / atbranch  (see 298).
//
// Integer in / integer out, LCG-seeded, folded to one return value, all 4-byte
// `unsigned` atomics so i386/ARM32 stay libcall-free.  All four targets, -O0.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress299RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress299RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress299RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress299RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress299RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress299RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress299RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress299RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress299TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    {p+"_atadd",
     t+" "+p+"_atadd("+t+" a){ unsigned h=(unsigned)a; unsigned cell=h; unsigned acc=0;\n"
     "  for(int i=0;i<48;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned old=__atomic_fetch_add(&cell,(h>>8)&0xFFu,__ATOMIC_SEQ_CST);\n"
     "    acc=acc*131u+old; }\n"
     "  return ("+t+")(acc + __atomic_load_n(&cell,__ATOMIC_SEQ_CST)); }\n",
     {0x12345u}, "OptStress299", 0},

    {p+"_atbits",
     t+" "+p+"_atbits("+t+" a){ unsigned h=(unsigned)a; unsigned cell=h|1u; unsigned acc=0;\n"
     "  for(int i=0;i<48;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned o1=__atomic_fetch_or(&cell,(h>>5)&0xFFu,__ATOMIC_SEQ_CST);\n"
     "    unsigned o2=__atomic_fetch_and(&cell,~((h>>13)&0xFu),__ATOMIC_SEQ_CST);\n"
     "    unsigned o3=__atomic_fetch_xor(&cell,(h>>17)&0x55u,__ATOMIC_SEQ_CST);\n"
     "    acc=acc*131u+(o1^o2^o3); }\n"
     "  return ("+t+")(acc + __atomic_load_n(&cell,__ATOMIC_SEQ_CST)); }\n",
     {0x23456u}, "OptStress299", 0},

    {p+"_atxchg",
     t+" "+p+"_atxchg("+t+" a){ unsigned h=(unsigned)a; unsigned cell=h; unsigned acc=0;\n"
     "  for(int i=0;i<48;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned old=__atomic_exchange_n(&cell,h^acc,__ATOMIC_SEQ_CST);\n"
     "    acc=acc*131u+old; }\n"
     "  return ("+t+")(acc + __atomic_load_n(&cell,__ATOMIC_SEQ_CST)); }\n",
     {0x34567u}, "OptStress299", 0},

    {p+"_atcas",
     t+" "+p+"_atcas("+t+" a){ unsigned h=(unsigned)a; unsigned cell=0; unsigned acc=0;\n"
     "  for(int i=0;i<48;i++){ h=h*1103515245u+12345u; unsigned v=(h>>3)&0xFFFFu;\n"
     "    unsigned cur=__atomic_load_n(&cell,__ATOMIC_RELAXED);\n"
     "    while(v>cur){\n"
     "      if(__atomic_compare_exchange_n(&cell,&cur,v,0,\n"
     "          __ATOMIC_SEQ_CST,__ATOMIC_RELAXED)) break; }\n"
     "    acc=acc*131u+cur; }\n"
     "  return ("+t+")(acc + __atomic_load_n(&cell,__ATOMIC_SEQ_CST)); }\n",
     {0x45678u}, "OptStress299", 0},

    {p+"_atsub",
     t+" "+p+"_atsub("+t+" a){ unsigned h=(unsigned)a; unsigned cell=0xFFFFFFFFu; unsigned acc=0;\n"
     "  for(int i=0;i<48;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned old=__atomic_fetch_sub(&cell,(h>>9)&0x7Fu,__ATOMIC_SEQ_CST);\n"
     "    acc=acc*131u+(old>>1); }\n"
     "  return ("+t+")(acc + __atomic_load_n(&cell,__ATOMIC_SEQ_CST)); }\n",
     {0x56789u}, "OptStress299", 0},

    {p+"_atbranch",
     t+" "+p+"_atbranch("+t+" a){ unsigned h=(unsigned)a; unsigned cell=h; unsigned acc=0;\n"
     "  for(int i=0;i<56;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned old=__atomic_fetch_add(&cell,(h>>7)&0x3Fu,__ATOMIC_SEQ_CST);\n"
     "    unsigned o;\n"
     "    if((old&3u)==0u) o=(unsigned char)(old>>1);\n"
     "    else if((old&3u)==1u) o=(unsigned short)(old*3u);\n"
     "    else o=old^0x9E37u;\n"
     "    acc=acc*131u+o+(unsigned)i; }\n"
     "  return ("+t+")(acc + __atomic_load_n(&cell,__ATOMIC_SEQ_CST)); }\n",
     {0x6789Au}, "OptStress299", 0},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress299TC("x64o299", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress299TC("x86o299", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress299TC("a64o299", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress299TC("armo299", "int");

INSTANTIATE_TEST_SUITE_P(OptStress299, X64OptStress299RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress299, X86OptStress299RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress299, A64OptStress299RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress299, ARM32OptStress299RT, ::testing::ValuesIn(kARM), rtTCName);
