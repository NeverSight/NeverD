//===- AllPlatform_OptStress78RTTests.cpp - 64-bit var shifts --*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Every prior OptStress probe deliberately AVOIDED variable 64-bit shifts on
// the 32-bit targets ("no variable i64 shift" notes throughout) for fear of a
// libcall.  But clang -O2 actually INLINES every one of these on i386 and
// ARM32 (shld/shrd word-swap sequences, lsl/lsr/orr pairs with a count
// crossing the 32-bit boundary) — no __ashldi3/__lshrdi3/__aeabi_llsl.  So the
// whole family is round-trippable and was simply never tested.  These probes
// hammer it:
//
//   * rot64    - variable 64-bit rotate left + right (shld/shrd; ARM rotate).
//   * shmix64  - variable logical 64-bit shifts in an xorshift-style mixer.
//   * sar64    - loop-carried signed 64-bit arithmetic shift right (shrd+sar),
//                amounts crossing 32 so the word-swap path is exercised.
//   * cnt64    - 64-bit clz/ctz/popcount (the i386 popcount is a rodata SWAR
//                load via GOTOFF — a constant-pool-mapping case in a new form).
//
// All shift amounts are masked to [0,63] (no shift-by-bitwidth UB).  Internal
// math is `unsigned long long`/`long long` on every target so the 32-bit ones
// run the real inline 64-bit sequences; folds to one integer return.  No
// 64-bit divide / libcall.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress78RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress78RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress78RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress78RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress78RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress78RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress78RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress78RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress78TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Variable 64-bit rotate left + right (x86 shld/shrd, ARM inline rotate).
    {p+"_rot64",
     t+" "+p+"_rot64("+t+" a){\n"
     "  unsigned long long h=(unsigned long long)a^0x9E3779B97F4A7C15ULL;\n"
     "  unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned r=(s>>5)&63u, q=(s>>11)&63u;\n"
     "    h=(h<<r)|(h>>((64-r)&63u));\n"
     "    h+=0x123456789ABCDEFULL;\n"
     "    h=(h>>q)|(h<<((64-q)&63u));\n"
     "    h^=h>>29; }\n"
     "  return ("+t+")(h^(h>>32)); }\n",
     {0xC1u}, "OptStress78", 2},

    // Variable logical 64-bit shifts (shl/shr) in an xorshift-style mixer.
    {p+"_shmix64",
     t+" "+p+"_shmix64("+t+" a){\n"
     "  unsigned long long h=(unsigned long long)a*0xD1B54A32D192ED03ULL+0x9E3779B9ULL;\n"
     "  unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned k=(s>>6)&63u, j=(s>>17)&63u;\n"
     "    h^=h<<k; h^=h>>j;\n"
     "    h+=(unsigned long long)s<<((s>>3)&31u);\n"
     "    h*=0x100000001B3ULL; }\n"
     "  return ("+t+")(h^(h>>32)); }\n",
     {0xC2u}, "OptStress78", 2},

    // Loop-carried signed 64-bit arithmetic shift right (shrd+sar word-swap).
    {p+"_sar64",
     t+" "+p+"_sar64("+t+" a){\n"
     "  long long acc=(long long)a-0x5A5A5A5A;\n"
     "  unsigned long long h=1; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned sh=(s>>4)&63u;\n"
     "    acc+=(long long)(int)s;\n"
     "    acc=acc>>sh;\n"
     "    acc^=(int)(s>>9);\n"
     "    h^=(unsigned long long)acc; h=(h<<7)|(h>>57); }\n"
     "  return ("+t+")(h^(unsigned long long)acc^((unsigned long long)acc>>32)); }\n",
     {0xC3u}, "OptStress78", 2},

    // 64-bit clz / ctz / popcount (i386 popcount is a rodata SWAR load).
    {p+"_cnt64",
     t+" "+p+"_cnt64("+t+" a){\n"
     "  unsigned long long h=(unsigned long long)a^0xABCDEF0123456789ULL;\n"
     "  unsigned s=(unsigned)a|1u; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned long long v=h|1ULL;\n"
     "    int lz=__builtin_clzll(v);\n"
     "    int tz=__builtin_ctzll(h|0x8000000000000000ULL);\n"
     "    int pc=__builtin_popcountll(h);\n"
     "    acc+=(unsigned)(lz*7+tz*5+pc*3);\n"
     "    h=h*6364136223846793005ULL+1442695040888963407ULL+acc;\n"
     "    h^=h>>(unsigned)((pc&31)+1); }\n"
     "  return ("+t+")(h^((unsigned long long)acc<<8)); }\n",
     {0xC4u}, "OptStress78", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress78TC("x64o78", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress78TC("x86o78", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress78TC("a64o78", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress78TC("armo78", "int");

INSTANTIATE_TEST_SUITE_P(OptStress78, X64OptStress78RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress78, X86OptStress78RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress78, A64OptStress78RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress78, ARM32OptStress78RT, ::testing::ValuesIn(kARM), rtTCName);
