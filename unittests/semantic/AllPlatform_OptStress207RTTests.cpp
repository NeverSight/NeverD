//===- AllPlatform_OptStress207RTTests.cpp - atomic RMW under -O2 ========//
//
// Roundtrip probes for atomic read-modify-write (the __sync builtins) driven in
// loops over writable globals -- x86 `lock xadd`/`lock cmpxchg`/`xchg`, AArch64
// LSE or ldxr/stxr, ARM32 ldrex/strex.  Single-threaded in Unicorn the atomics
// are ordinary RMW, but the lift must model the locked operation and its
// returned old value correctly across the loop-carried dependence.  32-bit (int)
// atomics keep every target inline -- no 64-bit atomic libcall.
//
//   * aadd   - atomic fetch-and-add accumulating a value-driven sequence.
//   * acas   - compare-and-swap accumulation (val_compare_and_swap old value).
//   * axchg  - atomic exchange (lock_test_and_set) threading the prior value.
//   * alogic - atomic and / or / xor folded together.
//   * acasr  - a compare-and-swap retry loop (the lock-free RMW idiom).
//   * amix   - several atomics over two globals interleaved.
//
// Integer add/xor/and/or only, value-driven (LCG), folded to one integer return,
// -O2, all four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress207RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress207RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress207RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress207RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress207RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress207RT, Verify) { roundTripAArch64(GetParam()); }
// ARM32 lowers a __sync RMW to a `dmb; ldrex/strex` retry loop.  The barrier
// must be a side-effect-only intrinsic: it earlier carried a default R0 output
// that shadowed the just-computed RMW operand (`lsr r4,r0,#20`) with a zero, so
// the atomic added/xored 0.  Fixed by emitting `dmb`/`dsb`/`isb`/`clrex` with no
// output (emitVoidIntrinsic), so ARM32 is now covered like the other targets.
class ARM32OptStress207RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress207RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress207TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Atomic fetch-and-add accumulating a value-driven sequence.
    {p+"_aadd",
     "static int "+p+"_ga;\n"
     +t+" "+p+"_aadd("+t+" x){ unsigned s=(unsigned)x; "+p+"_ga=0; "+t+" out=0;\n"
     "  for(int k=0;k<128;k++){ s=s*1103515245u+12345u;\n"
     "    int old=__sync_fetch_and_add(&"+p+"_ga,(int)(s>>20));\n"
     "    out+=("+t+")(old ^ "+p+"_ga); }\n"
     "  return out; }\n",
     {0x1u}, "OptStress207", 2},

    // Compare-and-swap accumulation threading the returned old value.
    {p+"_acas",
     "static int "+p+"_gc;\n"
     +t+" "+p+"_acas("+t+" x){ unsigned s=(unsigned)x; "+p+"_gc=7; "+t+" out=0;\n"
     "  for(int k=0;k<128;k++){ s=s*1103515245u+12345u;\n"
     "    int want=(int)(s>>18);\n"
     "    int old=__sync_val_compare_and_swap(&"+p+"_gc,"+p+"_gc,want);\n"
     "    out+=("+t+")(old + "+p+"_gc); }\n"
     "  return out; }\n",
     {0x2u}, "OptStress207", 2},

    // Atomic exchange threading the prior value.
    {p+"_axchg",
     "static int "+p+"_gx;\n"
     +t+" "+p+"_axchg("+t+" x){ unsigned s=(unsigned)x; "+p+"_gx=3; "+t+" out=0;\n"
     "  for(int k=0;k<128;k++){ s=s*1103515245u+12345u;\n"
     "    int prev=__sync_lock_test_and_set(&"+p+"_gx,(int)(s>>16));\n"
     "    out=(out<<1)^("+t+")prev; }\n"
     "  return out; }\n",
     {0x3u}, "OptStress207", 2},

    // Atomic and / or / xor folded together.
    {p+"_alogic",
     "static int "+p+"_gl;\n"
     +t+" "+p+"_alogic("+t+" x){ unsigned s=(unsigned)x; "+p+"_gl=(int)x|1; "+t+" out=0;\n"
     "  for(int k=0;k<96;k++){ s=s*1103515245u+12345u;\n"
     "    int a=__sync_fetch_and_or(&"+p+"_gl,(int)(s&0xff));\n"
     "    int b=__sync_fetch_and_and(&"+p+"_gl,(int)((s>>8)|0xf));\n"
     "    int c=__sync_fetch_and_xor(&"+p+"_gl,(int)(s>>16));\n"
     "    out+=("+t+")(a^b^c^"+p+"_gl); }\n"
     "  return out; }\n",
     {0x4u}, "OptStress207", 2},

    // A compare-and-swap retry loop (the lock-free read-modify-write idiom).
    {p+"_acasr",
     "static int "+p+"_gr;\n"
     +t+" "+p+"_acasr("+t+" x){ unsigned s=(unsigned)x; "+p+"_gr=0; "+t+" out=0;\n"
     "  for(int k=0;k<64;k++){ s=s*1103515245u+12345u; int add=(int)(s>>22);\n"
     "    int oldv, newv;\n"
     "    do { oldv="+p+"_gr; newv=oldv*3+add; }\n"
     "    while(!__sync_bool_compare_and_swap(&"+p+"_gr,oldv,newv));\n"
     "    out^=("+t+")"+p+"_gr; }\n"
     "  return out; }\n",
     {0x5u}, "OptStress207", 2},

    // Several atomics over two globals interleaved.
    {p+"_amix",
     "static int "+p+"_m0,"+p+"_m1;\n"
     +t+" "+p+"_amix("+t+" x){ unsigned s=(unsigned)x; "+p+"_m0=1; "+p+"_m1=2; "+t+" out=0;\n"
     "  for(int k=0;k<96;k++){ s=s*1103515245u+12345u;\n"
     "    int a=__sync_fetch_and_add(&"+p+"_m0,(int)(s>>20));\n"
     "    int b=__sync_lock_test_and_set(&"+p+"_m1,a);\n"
     "    int c=__sync_fetch_and_xor(&"+p+"_m0,b);\n"
     "    out+=("+t+")(a+b+c+"+p+"_m0+"+p+"_m1); }\n"
     "  return out; }\n",
     {0x6u}, "OptStress207", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress207TC("x64o207", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress207TC("x86o207", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress207TC("a64o207", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress207TC("armo207", "int");

INSTANTIATE_TEST_SUITE_P(OptStress207, X64OptStress207RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress207, X86OptStress207RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress207, A64OptStress207RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress207, ARM32OptStress207RT, ::testing::ValuesIn(kARM), rtTCName);
