//===- AllPlatform_OptStress208RTTests.cpp - sub-word atomic RMW ========//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip probes for SUB-WORD (8-/16-bit) atomic read-modify-write -- the
// __sync builtins on `unsigned char` / `unsigned short` globals.  These exercise
// a distinct instruction set from OptStress207's word atomics: ARM
// ldrexb/strexb + ldrexh/strexh (each fronted by `dmb`), x86 byte/word
// `lock xadd`/`lock cmpxchg`/`xchg`.  The narrow result must be zero/sign
// modelled correctly across the loop-carried dependence; clang inlines every one
// (no 8/16-bit atomic libcall), so all four targets run.
//
//   * aadd8   - 8-bit fetch-and-add accumulating a value-driven sequence.
//   * axor16  - 16-bit fetch-and-xor threading the returned old value.
//   * acas8   - 8-bit compare-and-swap retry loop (the lock-free idiom).
//   * axchg16 - 16-bit exchange (lock_test_and_set) threading the prior value.
//   * alogic8 - 8-bit and / or / xor folded together.
//   * amix8   - mixed 8- and 16-bit atomics over two globals interleaved.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress208RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress208RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress208RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress208RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress208RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress208RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress208RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress208RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress208TC(const char *prefix) {
  std::string p = prefix;
  return {
    // 8-bit fetch-and-add accumulating a value-driven sequence.
    {p+"_aadd8",
     "static unsigned char "+p+"_g8;\n"
     "int "+p+"_aadd8(int x){ unsigned s=(unsigned)x; "+p+"_g8=0; int out=0;\n"
     "  for(int k=0;k<128;k++){ s=s*1103515245u+12345u;\n"
     "    unsigned char old=__sync_fetch_and_add(&"+p+"_g8,(unsigned char)(s>>22));\n"
     "    out+=(int)(unsigned char)(old ^ "+p+"_g8); }\n"
     "  return out; }\n",
     {0x1u}, "OptStress208", 2},

    // 16-bit fetch-and-xor threading the returned old value.
    {p+"_axor16",
     "static unsigned short "+p+"_g16;\n"
     "int "+p+"_axor16(int x){ unsigned s=(unsigned)x; "+p+"_g16=0xBEEF; int out=0;\n"
     "  for(int k=0;k<128;k++){ s=s*1103515245u+12345u;\n"
     "    unsigned short old=__sync_fetch_and_xor(&"+p+"_g16,(unsigned short)(s>>13));\n"
     "    out+=(int)(unsigned short)(old + "+p+"_g16); }\n"
     "  return out; }\n",
     {0x2u}, "OptStress208", 2},

    // 8-bit compare-and-swap retry loop (the lock-free read-modify-write idiom).
    {p+"_acas8",
     "static unsigned char "+p+"_gc8;\n"
     "int "+p+"_acas8(int x){ unsigned s=(unsigned)x; "+p+"_gc8=0; int out=0;\n"
     "  for(int k=0;k<64;k++){ s=s*1103515245u+12345u; unsigned char add=(unsigned char)(s>>24);\n"
     "    unsigned char oldv, newv;\n"
     "    do { oldv="+p+"_gc8; newv=(unsigned char)(oldv*3+add); }\n"
     "    while(!__sync_bool_compare_and_swap(&"+p+"_gc8,oldv,newv));\n"
     "    out^=(int)(unsigned char)"+p+"_gc8; }\n"
     "  return out; }\n",
     {0x3u}, "OptStress208", 2},

    // 16-bit exchange threading the prior value.
    {p+"_axchg16",
     "static unsigned short "+p+"_gx16;\n"
     "int "+p+"_axchg16(int x){ unsigned s=(unsigned)x; "+p+"_gx16=3; int out=0;\n"
     "  for(int k=0;k<96;k++){ s=s*1103515245u+12345u;\n"
     "    unsigned short prev=__sync_lock_test_and_set(&"+p+"_gx16,(unsigned short)(s>>15));\n"
     "    out=(out<<1)^(int)prev; }\n"
     "  return out; }\n",
     {0x4u}, "OptStress208", 2},

    // 8-bit and / or / xor folded together.
    {p+"_alogic8",
     "static unsigned char "+p+"_gl8;\n"
     "int "+p+"_alogic8(int x){ unsigned s=(unsigned)x; "+p+"_gl8=(unsigned char)(x|1); int out=0;\n"
     "  for(int k=0;k<96;k++){ s=s*1103515245u+12345u;\n"
     "    unsigned char a=__sync_fetch_and_or(&"+p+"_gl8,(unsigned char)(s&0x3f));\n"
     "    unsigned char b=__sync_fetch_and_and(&"+p+"_gl8,(unsigned char)((s>>8)|0xc0));\n"
     "    unsigned char c=__sync_fetch_and_xor(&"+p+"_gl8,(unsigned char)(s>>16));\n"
     "    out+=(int)(unsigned char)(a^b^c^"+p+"_gl8); }\n"
     "  return out; }\n",
     {0x5u}, "OptStress208", 2},

    // Mixed 8- and 16-bit atomics over two globals interleaved.
    {p+"_amix8",
     "static unsigned char "+p+"_m8; static unsigned short "+p+"_m16;\n"
     "int "+p+"_amix8(int x){ unsigned s=(unsigned)x; "+p+"_m8=1; "+p+"_m16=2; int out=0;\n"
     "  for(int k=0;k<96;k++){ s=s*1103515245u+12345u;\n"
     "    unsigned char a=__sync_fetch_and_add(&"+p+"_m8,(unsigned char)(s>>24));\n"
     "    unsigned short b=__sync_fetch_and_add(&"+p+"_m16,(unsigned short)(s>>16));\n"
     "    out+=(int)a+(int)b+"+p+"_m8+"+p+"_m16; }\n"
     "  return out; }\n",
     {0x6u}, "OptStress208", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress208TC("x64o208");
static const std::vector<RoundTripTC> kX86 = makeOptStress208TC("x86o208");
static const std::vector<RoundTripTC> kA64 = makeOptStress208TC("a64o208");
static const std::vector<RoundTripTC> kARM = makeOptStress208TC("armo208");

INSTANTIATE_TEST_SUITE_P(OptStress208, X64OptStress208RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress208, X86OptStress208RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress208, A64OptStress208RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress208, ARM32OptStress208RT, ::testing::ValuesIn(kARM), rtTCName);
