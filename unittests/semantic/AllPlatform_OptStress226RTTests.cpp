//===- AllPlatform_OptStress226RTTests.cpp - carry/borrow + overflow =====//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Breadth probes for carry/borrow propagation and the checked-arithmetic
// builtins, whose flag results are consumed BOTH straight-line (a select) and
// across a branch -- exactly the cross-block flag-liveness shape that the
// MedFlags pass has historically mis-killed (#161).  These exercise CF/OF
// generation from add/sub/mul-overflow and a loop-carried carry chain.
//
//   * uaddc  - chained unsigned add-with-carry (__builtin_add_overflow), the
//              carry is loop-carried and folded back in.
//   * usubb  - chained unsigned sub-with-borrow (__builtin_sub_overflow).
//   * smulo  - signed multiply-overflow check selects between product / xor.
//   * addov  - signed add-overflow drives a saturate (branch + select reuse).
//   * mword  - 64-bit value split into hi/lo, a 64-bit compare picks the merge.
//   * cmpsel - one comparison feeds BOTH a select and a branch (the #161 case).
//
// All internal math is 32-bit (or 64-bit add/mul-by-constant only) so no
// 64-bit div/overflow libcall is ever emitted on the 32-bit targets.
// Integer in / integer out, LCG-seeded, folded to one integer return.  All
// four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress226RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress226RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress226RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress226RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress226RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress226RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress226RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress226RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress226TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Chained unsigned add-with-carry; carry is loop-carried.
    {p+"_uaddc",
     t+" "+p+"_uaddc("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0, carry=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned s; unsigned c=__builtin_add_overflow(h, carry, &s);\n"
     "    unsigned s2; c |= __builtin_add_overflow(s, (h>>7), &s2);\n"
     "    carry=c; acc=acc*131u+s2+c+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress226", 2},

    // Chained unsigned sub-with-borrow.
    {p+"_usubb",
     t+" "+p+"_usubb("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0, borrow=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned d; unsigned b=__builtin_sub_overflow(h, borrow, &d);\n"
     "    unsigned d2; b |= __builtin_sub_overflow(d, (h>>9)&0xffu, &d2);\n"
     "    borrow=b; acc=acc*131u+d2-b+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress226", 2},

    // Signed multiply-overflow selects product vs. xor.
    {p+"_smulo",
     t+" "+p+"_smulo("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    int x=(int)(h>>3), y=(int)((h>>17)|1u); int pr;\n"
     "    unsigned v = __builtin_mul_overflow(x, y, &pr) ? (unsigned)(x ^ y) : (unsigned)pr;\n"
     "    acc=acc*131u+v+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress226", 2},

    // Signed add-overflow drives a saturate (branch + select reuse of OF).
    {p+"_addov",
     t+" "+p+"_addov("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    int x=(int)h, y=(int)(h*2654435761u); int s;\n"
     "    unsigned v = __builtin_add_overflow(x,y,&s)\n"
     "      ? (unsigned)((x<0)?(-2147483647-1):2147483647) : (unsigned)s;\n"
     "    acc=acc*131u+v+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress226", 2},

    // 64-bit value split into hi/lo; a 64-bit compare picks the merge value.
    {p+"_mword",
     t+" "+p+"_mword("+t+" a){ unsigned long long h=(unsigned long long)a ^ 0x9E3779B97F4A7C15ULL;\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*6364136223846793005ULL+1442695040888963407ULL;\n"
     "    unsigned long long g=h+((unsigned long long)(unsigned)i<<32);\n"
     "    unsigned hi=(unsigned)(g>>32), lo=(unsigned)g;\n"
     "    unsigned v = (g > 0x8000000000000000ULL) ? (hi^lo) : (hi+lo);\n"
     "    acc=acc*131u+v+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress226", 2},

    // One comparison feeds BOTH a select and a branch (cross-use of flags).
    {p+"_cmpsel",
     t+" "+p+"_cmpsel("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    int x=(int)h, y=(int)(h>>5);\n"
     "    int m = (x<y) ? x : y;\n"
     "    if(x<y) acc+=(unsigned)m; else acc-=(unsigned)m;\n"
     "    unsigned u=h, w=h*2654435761u;\n"
     "    acc += (u<w) ? (u-w) : (w-u);\n"
     "    acc=acc*131u+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress226", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress226TC("x64o226", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress226TC("x86o226", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress226TC("a64o226", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress226TC("armo226", "int");

INSTANTIATE_TEST_SUITE_P(OptStress226, X64OptStress226RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress226, X86OptStress226RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress226, A64OptStress226RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress226, ARM32OptStress226RT, ::testing::ValuesIn(kARM), rtTCName);
