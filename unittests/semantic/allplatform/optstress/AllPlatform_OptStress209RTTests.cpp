//===- AllPlatform_OptStress209RTTests.cpp - explicit-order atomics =====//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip probes for the C11 `__atomic_*` builtins with EXPLICIT memory
// orders (relaxed / acquire / release / acq_rel / seq_cst) -- a distinct
// instruction set from OptStress207/208's seq_cst `__sync_*`: AArch64
// acquire/release exclusives ldaxr/stlxr and load-acquire ldar / store-release
// stlr; ARM32 dmb-fronted ldrex/strex and dmb;ldr;dmb atomic load/store; x86
// `lock xadd`/`cmpxchg`/`xchg` (a relaxed RMW is the same locked op there).
// Each is value-driven and folds to one int return; the lift must keep the
// barrier/ordering side-effects from clobbering the loop-carried value.
//
//   * aldst    - seq_cst atomic load+store ping-pong on one global.
//   * aacqrel  - acq_rel fetch-add + acquire load (AArch64 ldaxr/stlxr/ldar).
//   * arelax   - relaxed fetch-xor (no barriers emitted).
//   * acasord  - compare-exchange retry, acq_rel success / acquire failure.
//   * axchgord - acq_rel exchange threading the prior value.
//   * amixord  - several orders over two globals interleaved.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress209RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress209RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress209RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress209RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress209RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress209RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress209RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress209RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress209TC(const char *prefix) {
  std::string p = prefix;
  return {
    // seq_cst atomic load + store ping-pong on one global.
    {p+"_aldst",
     "static int "+p+"_g;\n"
     "int "+p+"_aldst(int x){ unsigned s=(unsigned)x;\n"
     "  __atomic_store_n(&"+p+"_g,0,__ATOMIC_SEQ_CST); int out=0;\n"
     "  for(int k=0;k<96;k++){ s=s*1103515245u+12345u;\n"
     "    int v=__atomic_load_n(&"+p+"_g,__ATOMIC_SEQ_CST);\n"
     "    __atomic_store_n(&"+p+"_g,v+(int)(s>>20),__ATOMIC_SEQ_CST);\n"
     "    out += __atomic_load_n(&"+p+"_g,__ATOMIC_SEQ_CST) ^ v; }\n"
     "  return out; }\n",
     {0x1u}, "OptStress209", 2},

    // acq_rel fetch-add + acquire load (AArch64 ldaxr/stlxr/ldar).
    {p+"_aacqrel",
     "static int "+p+"_ga;\n"
     "int "+p+"_aacqrel(int x){ unsigned s=(unsigned)x;\n"
     "  __atomic_store_n(&"+p+"_ga,7,__ATOMIC_RELAXED); int out=0;\n"
     "  for(int k=0;k<128;k++){ s=s*1103515245u+12345u;\n"
     "    int old=__atomic_fetch_add(&"+p+"_ga,(int)(s>>19),__ATOMIC_ACQ_REL);\n"
     "    out += old ^ __atomic_load_n(&"+p+"_ga,__ATOMIC_ACQUIRE); }\n"
     "  return out; }\n",
     {0x2u}, "OptStress209", 2},

    // relaxed fetch-xor (no barriers emitted).
    {p+"_arelax",
     "static int "+p+"_gr;\n"
     "int "+p+"_arelax(int x){ unsigned s=(unsigned)x;\n"
     "  __atomic_store_n(&"+p+"_gr,0,__ATOMIC_RELAXED); int out=0;\n"
     "  for(int k=0;k<128;k++){ s=s*1103515245u+12345u;\n"
     "    int old=__atomic_fetch_xor(&"+p+"_gr,(int)(s>>17),__ATOMIC_RELAXED);\n"
     "    out += old + __atomic_load_n(&"+p+"_gr,__ATOMIC_RELAXED); }\n"
     "  return out; }\n",
     {0x3u}, "OptStress209", 2},

    // compare-exchange retry, acq_rel success / acquire failure.
    {p+"_acasord",
     "static int "+p+"_gc;\n"
     "int "+p+"_acasord(int x){ unsigned s=(unsigned)x;\n"
     "  __atomic_store_n(&"+p+"_gc,0,__ATOMIC_RELAXED); int out=0;\n"
     "  for(int k=0;k<64;k++){ s=s*1103515245u+12345u; int add=(int)(s>>22);\n"
     "    int oldv=__atomic_load_n(&"+p+"_gc,__ATOMIC_RELAXED), newv;\n"
     "    do { newv=oldv*3+add; }\n"
     "    while(!__atomic_compare_exchange_n(&"+p+"_gc,&oldv,newv,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE));\n"
     "    out ^= __atomic_load_n(&"+p+"_gc,__ATOMIC_RELAXED); }\n"
     "  return out; }\n",
     {0x4u}, "OptStress209", 2},

    // acq_rel exchange threading the prior value.
    {p+"_axchgord",
     "static int "+p+"_gx;\n"
     "int "+p+"_axchgord(int x){ unsigned s=(unsigned)x;\n"
     "  __atomic_store_n(&"+p+"_gx,3,__ATOMIC_RELAXED); int out=0;\n"
     "  for(int k=0;k<96;k++){ s=s*1103515245u+12345u;\n"
     "    int prev=__atomic_exchange_n(&"+p+"_gx,(int)(s>>16),__ATOMIC_ACQ_REL);\n"
     "    out=(out<<1)^prev; }\n"
     "  return out; }\n",
     {0x5u}, "OptStress209", 2},

    // Several memory orders over two globals interleaved.
    {p+"_amixord",
     "static int "+p+"_m0,"+p+"_m1;\n"
     "int "+p+"_amixord(int x){ unsigned s=(unsigned)x;\n"
     "  __atomic_store_n(&"+p+"_m0,1,__ATOMIC_SEQ_CST); __atomic_store_n(&"+p+"_m1,2,__ATOMIC_RELAXED); int out=0;\n"
     "  for(int k=0;k<96;k++){ s=s*1103515245u+12345u;\n"
     "    int a=__atomic_fetch_or(&"+p+"_m0,(int)(s&0xff),__ATOMIC_ACQ_REL);\n"
     "    int b=__atomic_fetch_and(&"+p+"_m1,(int)((s>>8)|0xf),__ATOMIC_RELAXED);\n"
     "    int c=__atomic_exchange_n(&"+p+"_m0,b,__ATOMIC_SEQ_CST);\n"
     "    out += a+b+c+__atomic_load_n(&"+p+"_m0,__ATOMIC_ACQUIRE)+__atomic_load_n(&"+p+"_m1,__ATOMIC_SEQ_CST); }\n"
     "  return out; }\n",
     {0x6u}, "OptStress209", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress209TC("x64o209");
static const std::vector<RoundTripTC> kX86 = makeOptStress209TC("x86o209");
static const std::vector<RoundTripTC> kA64 = makeOptStress209TC("a64o209");
static const std::vector<RoundTripTC> kARM = makeOptStress209TC("armo209");

INSTANTIATE_TEST_SUITE_P(OptStress209, X64OptStress209RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress209, X86OptStress209RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress209, A64OptStress209RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress209, ARM32OptStress209RT, ::testing::ValuesIn(kARM), rtTCName);
