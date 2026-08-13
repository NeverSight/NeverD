//===- AllPlatform_OptStress318RTTests.cpp - mixed-width flag cluster ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Stresses MedFlags' per-use comparison-source recovery (the #147/#148/#149
// family): several INDEPENDENT comparisons of DIFFERENT widths and signedness
// land in one basic block, each feeding its own select/cmov.  At -O2 clang
// clusters the `cmp`/`subs` + `setcc`/`cset`/`cmov`/`csel` sequence into a
// single block, so the flag-reconstruction pass must bind every conditional to
// the comparison that actually produced its NZCV/EFLAGS — not the nearest, the
// widest, or a shared global one.  A mis-bound flag (signed read as unsigned,
// an 8/16-bit compare matched to a 64-bit one, or an `==` matched to a `<`)
// diverges the folded accumulator.
//
// One i64 accumulator threads every iteration so a single wrong select on any
// platform corrupts the final return.  i64 math is multiply/shift/xor/add with
// CONSTANT shifts only (libcall-free on i386/ARM32).  x86-64/AArch64 carry the
// whole i64 in one register; i386/ARM32 split it across a pair — both must
// reconstruct the same flags.  Deterministic (LCG-seeded), -O2, all four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress318RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress318RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress318RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress318RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress318RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress318RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress318RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress318RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress318TC(const char *prefix,
                                                   const char *T, int Opt) {
  std::string p = prefix, t = T;
  std::vector<RoundTripTC> v = {
    // Five mixed-width / mixed-sign compares per iteration, each driving its own
    // select, all in the loop body's single block.
    {p+"_flagmix",
     t+" "+p+"_flagmix("+t+" a){ unsigned w=(unsigned)a|1u;\n"
     "  long long acc=(long long)(unsigned)a;\n"
     "  for(int i=0;i<48;i++){ w=w*1103515245u+12345u;\n"
     "    int x=(int)w, y=(int)(w>>7);\n"
     "    short s=(short)(w>>3); unsigned char uc=(unsigned char)(w>>11);\n"
     "    long long tt=0;\n"
     "    tt += (x<y) ? x : y;\n"
     "    tt += ((unsigned)x<(unsigned)y) ? 3 : 5;\n"
     "    tt += (s>(short)acc) ? (long long)s : acc;\n"
     "    tt += (uc<=(unsigned char)acc) ? 7 : 9;\n"
     "    tt += ((long long)x==(long long)acc) ? acc : (long long)y;\n"
     "    acc = acc*131 + tt; acc ^= acc>>17; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n",
     {0x1234u}, "OptStress318", Opt},

    // Greater/less-or-equal mix plus a not-equal, all unsigned-vs-signed paired
    // on the SAME operands so a sign mismatch flips exactly one select.
    {p+"_signpair",
     t+" "+p+"_signpair("+t+" a){ unsigned w=(unsigned)a^0x5Au;\n"
     "  long long acc=(long long)(unsigned)a;\n"
     "  for(int i=0;i<44;i++){ w=w*22695477u+1u;\n"
     "    int x=(int)w, y=(int)(w>>9);\n"
     "    long long tt=0;\n"
     "    tt += (x>=y) ? x : -y;\n"
     "    tt += ((unsigned)x>=(unsigned)y) ? 11 : 13;\n"
     "    tt += (x!=y) ? (long long)(x^y) : acc;\n"
     "    tt += ((unsigned)x>(unsigned)y) ? acc : (long long)x;\n"
     "    acc = acc*1000003 + tt; acc ^= acc>>23; }\n"
     "  return ("+t+")(acc + (acc>>32)); }\n",
     {0x2345u}, "OptStress318", Opt},

    // Compares whose RESULT (not a select) is summed: setcc booleans clustered,
    // so the 0/1 materialization of each flag must use the right condition.
    {p+"_boolsum",
     t+" "+p+"_boolsum("+t+" a){ unsigned w=(unsigned)a+0x9u;\n"
     "  long long acc=(long long)(unsigned)a;\n"
     "  for(int i=0;i<40;i++){ w=w*1664525u+1013904223u;\n"
     "    int x=(int)w, y=(int)(w>>5);\n"
     "    short s=(short)(w>>1);\n"
     "    long long b = (x<y) + 2*((unsigned)x<(unsigned)y)\n"
     "                + 4*(x==y) + 8*(s<0) + 16*((long long)x>acc);\n"
     "    acc = acc*131 + b*0x100000001LL; acc ^= acc>>29; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n",
     {0x3456u}, "OptStress318", Opt},
  };
  return v;
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress318TC("x64o318", "long", 2);
static const std::vector<RoundTripTC> kX86 = makeOptStress318TC("x86o318", "int", 2);
static const std::vector<RoundTripTC> kA64 = makeOptStress318TC("a64o318", "long", 2);
static const std::vector<RoundTripTC> kARM = makeOptStress318TC("armo318", "int", 2);

INSTANTIATE_TEST_SUITE_P(OptStress318, X64OptStress318RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress318, X86OptStress318RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress318, A64OptStress318RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress318, ARM32OptStress318RT, ::testing::ValuesIn(kARM), rtTCName);
