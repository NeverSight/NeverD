//===- AllPlatform_OptStress211RTTests.cpp - leading bit-scan param guard =//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Regression guard for the i386 "phantom regparm argument from a BSR/BSF
// zero-source preserve" miscompile.  On i386 `__builtin_clzll`/`ctzll` lower to
// `bsr`/`bsf`, whose architecturally-undefined zero-source destination NeverD
// models as the preserved old value (`SELECT(src==0, old_dst, computed)`).  When
// that bit-scan is the FIRST write to a regparm register (ECX/EDX), the preserve
// reads the register's incoming value, so it looks live-in; detectRegisterParams
// then mistook it for a fastcall-style regparm argument and injected a phantom
// leading `i64` parameter, shifting the real cdecl stack argument from
// [esp+4] to [esp+0xC] — the recompiled function read an uninitialised slot.
// The fix teaches the scratch detector to recognise the BSR/BSF preserve idiom
// (sibling to the existing `setne %cl` sub-register-merge guard), so these
// probes — which put a 64-bit bit-scan right at function entry — must round-trip.
//
//   * clzlead - 64-bit clz of an arg-derived value as the leading operation.
//   * hiwlead - leading clz feeding a loop-carried 64-bit high word.
//   * ctzlead - 64-bit ctz (BSF) as the leading operation.
//   * cntlead - clz + ctz + popcount all computed before any other arg use.
//
// Integer in / integer out, LCG-seeded, folded to one integer return; no float
// / 64-bit divide / libcall.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress211RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress211RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress211RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress211RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress211RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress211RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress211RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress211RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress211TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 64-bit clz as the leading op (bsr writes a regparm reg first on i386).
    {p+"_clzlead",
     t+" "+p+"_clzlead("+t+" a){\n"
     "  unsigned long long h=(unsigned long long)a^0xABCDEF0123456789ULL;\n"
     "  unsigned acc=0, out=0;\n"
     "  for(int i=0;i<4;i++){ unsigned c=(unsigned)__builtin_clzll(h|1ULL);\n"
     "    out=out*100u+c; acc+=c; h=h*6364136223846793005ULL+1u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xC4u}, "OptStress211", 2},

    // leading clz feeding a loop-carried 64-bit high word.
    {p+"_hiwlead",
     t+" "+p+"_hiwlead("+t+" a){\n"
     "  unsigned long long h=(unsigned long long)a^0xABCDEF0123456789ULL;\n"
     "  unsigned acc=0, out=0;\n"
     "  for(int i=0;i<4;i++){ unsigned c=(unsigned)__builtin_clzll(h|1ULL);\n"
     "    out=out*131u+(unsigned)(h>>32); acc+=c;\n"
     "    h=h*6364136223846793005ULL+1u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xC4u}, "OptStress211", 2},

    // 64-bit ctz (BSF) as the leading op.
    {p+"_ctzlead",
     t+" "+p+"_ctzlead("+t+" a){\n"
     "  unsigned long long h=(unsigned long long)a^0x123456789ABCDEF0ULL;\n"
     "  unsigned acc=0, out=0;\n"
     "  for(int i=0;i<5;i++){ unsigned c=(unsigned)__builtin_ctzll(h|0x8000000000000000ULL);\n"
     "    out=out*100u+c; acc+=c; h=h*6364136223846793005ULL+1442695040888963407ULL+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x37u}, "OptStress211", 2},

    // clz + ctz + popcount all computed before any other use of the argument.
    {p+"_cntlead",
     t+" "+p+"_cntlead("+t+" a){\n"
     "  unsigned long long h=(unsigned long long)a^0xF0E1D2C3B4A59687ULL;\n"
     "  unsigned acc=0, out=0;\n"
     "  for(int i=0;i<5;i++){\n"
     "    unsigned lz=(unsigned)__builtin_clzll(h|1ULL);\n"
     "    unsigned tz=(unsigned)__builtin_ctzll(h|0x8000000000000000ULL);\n"
     "    unsigned pc=(unsigned)__builtin_popcountll(h);\n"
     "    out=out*131u+(lz*7u+tz*5u+pc*3u); acc+=lz+tz+pc;\n"
     "    h=h*6364136223846793005ULL+1u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x9Bu}, "OptStress211", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress211TC("x64o211", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress211TC("x86o211", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress211TC("a64o211", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress211TC("armo211", "int");

INSTANTIATE_TEST_SUITE_P(OptStress211, X64OptStress211RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress211, X86OptStress211RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress211, A64OptStress211RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress211, ARM32OptStress211RT, ::testing::ValuesIn(kARM), rtTCName);
