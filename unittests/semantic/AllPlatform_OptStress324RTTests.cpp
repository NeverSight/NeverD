//===- AllPlatform_OptStress324RTTests.cpp - -Os size-opt shapes --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// First OptStress batch compiled at -Os: every prior probe ran at -O0/-O2/-O3,
// leaving the size-optimizer's distinct code shapes unexercised through the lift
// roundtrip.  At -Os clang prefers smaller code: real idiv/udiv instead of
// magic-multiply division, compact imul-by-constant instead of shift/add/lea
// expansions, cmov/csel/conditional-exec for selects, heavier partial-register
// reuse, and tail-merging of duplicated blocks (shared epilogues reached from
// many predecessors) — all CFG/width recovery paths the lifter has not seen.
//
// All integer, scalar (no arrays / no aggregate init), LCG seeded, folded single
// return; 32-bit targets stay libcall-free (no i64 div, no i64 variable shift —
// only native 32-bit div/mod and 32x32->64 widening multiply).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress324RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress324RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress324RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress324RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress324RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress324RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress324RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress324RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress324TC(const char *prefix,
                                                   const char *T) {
  std::string p = prefix, t = T;
  // -Os via trailing flag: it overrides the fixture's -O2 (clang honors the last
  // -O), so every case below compiles size-optimized.
  std::vector<RoundTripTC> v = {
    // Signed+unsigned div/mod by a runtime divisor: at -Os clang emits real
    // idiv/udiv (vs -O2 magic-multiply).  Quotient and remainder are consumed
    // independently so LLVM cannot fold the div/mod identity away.
    {p+"_divmod",
     t+" "+p+"_divmod("+t+" a){ unsigned w=(unsigned)a|1u; long long acc=0;\n"
     "  for(int i=0;i<48;i++){ w=w*1103515245u+12345u;\n"
     "    int x=(int)w; int d=(int)((w>>3)&0x7fff)+1;\n"
     "    unsigned ux=w; unsigned ud=((w>>5)&0x3fff)+3u;\n"
     "    acc += (long long)(x/d); acc ^= (long long)(x%d);\n"
     "    acc += (long long)(ux/ud); acc -= (long long)(ux%ud);\n"
     "    acc += (long long)(x/7) - (long long)(x%7); }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n",
     {0x1234u}, "OptStress324", 2, "-Os"},

    // Multiply by many constants: at -Os these stay imul (smaller) rather than
    // expanding to shift/add/lea trees; widening 32x32->64 stays native.
    {p+"_mulconst",
     t+" "+p+"_mulconst("+t+" a){ unsigned w=(unsigned)a^0x9e3779b9u; long long acc=0;\n"
     "  for(int i=0;i<64;i++){ w=w*1103515245u+12345u; int x=(int)w;\n"
     "    acc += (long long)x*3 + (long long)x*5 - (long long)x*7;\n"
     "    acc ^= (long long)x*9 + (long long)x*10 + (long long)x*100;\n"
     "    acc += (long long)x*65535 - (long long)x*0x10001; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n",
     {0x2345u}, "OptStress324", 2, "-Os"},

    // Ternary select chains -> cmov/csel/conditional-exec at -Os.
    {p+"_cmovchain",
     t+" "+p+"_cmovchain("+t+" a){ unsigned w=(unsigned)a+0x55u; long long acc=0;\n"
     "  for(int i=0;i<80;i++){ w=w*22695477u+1u; int x=(int)w;\n"
     "    int s1=(x>0)? x : -x;\n"
     "    int s2=(x&1)? s1*3 : s1+7;\n"
     "    int s3=((unsigned)w>0x80000000u)? (s2|1) : (s2&~1);\n"
     "    acc += (x>i)? (long long)s3 : -(long long)s3; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n",
     {0x3456u}, "OptStress324", 2, "-Os"},

    // Running bitset: set/clear/toggle/test at a variable bit index (native
    // 32-bit variable shift), value folded so any shift/mask error shows.
    {p+"_bitset",
     t+" "+p+"_bitset("+t+" a){ unsigned w=(unsigned)a|1u; unsigned bits=(unsigned)a;\n"
     "  long long acc=0;\n"
     "  for(int i=0;i<96;i++){ w=w*1664525u+1013904223u; int b=(int)(w&31);\n"
     "    bits |= (1u<<b);\n"
     "    if(w&0x10000u) bits &= ~(1u<<b);\n"
     "    bits ^= (1u<<((b+7)&31));\n"
     "    int tb=(int)((bits>>b)&1u);\n"
     "    acc += (long long)bits + (long long)tb*131; acc ^= acc>>17; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n",
     {0x4567u}, "OptStress324", 2, "-Os"},

    // Sub-register width mixing: signed/unsigned char & short ext chains, the
    // historical sign/zero-extension + partial-register bug area (#157f), denser
    // at -Os where partial registers are reused aggressively.
    {p+"_widthmix",
     t+" "+p+"_widthmix("+t+" a){ unsigned w=(unsigned)a^0x1234u; long long acc=0;\n"
     "  for(int i=0;i<80;i++){ w=w*1103515245u+12345u;\n"
     "    signed char sb=(signed char)w; unsigned char ub=(unsigned char)(w>>8);\n"
     "    short sh=(short)(w>>3); unsigned short uh=(unsigned short)(w>>11);\n"
     "    acc += (long long)sb - (long long)ub + (long long)sh - (long long)uh;\n"
     "    acc ^= (long long)((sb<0)? (unsigned)sb : (unsigned)ub) << 8;\n"
     "    acc += (long long)(sh*(int)ub) - (long long)((int)uh*(int)sb); }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n",
     {0x5678u}, "OptStress324", 2, "-Os"},

    // Duplicated switch arms + shared select epilogue: -Os tail-merges the
    // identical bodies into blocks reached from multiple predecessors, stressing
    // phi/CFG recovery.
    {p+"_tailmerge",
     t+" "+p+"_tailmerge("+t+" a){ unsigned w=(unsigned)a+0x77u; long long acc=0;\n"
     "  for(int i=0;i<96;i++){ w=w*22695477u+1u; int x=(int)w; long long r;\n"
     "    switch(x&3){\n"
     "    case 0: r=(long long)x*3+1; break;\n"
     "    case 1: r=(long long)x*3+1; break;\n"
     "    case 2: r=(long long)x*5-2; break;\n"
     "    default: r=(long long)x*5-2; }\n"
     "    acc += (x>0)? r : -r; acc ^= acc>>19; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n",
     {0x6789u}, "OptStress324", 2, "-Os"},
  };
  return v;
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress324TC("x64o324", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress324TC("x86o324", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress324TC("a64o324", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress324TC("armo324", "int");

INSTANTIATE_TEST_SUITE_P(OptStress324, X64OptStress324RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress324, X86OptStress324RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress324, A64OptStress324RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress324, ARM32OptStress324RT, ::testing::ValuesIn(kARM), rtTCName);
