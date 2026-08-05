//===- AllPlatform_OptStress56RTTests.cpp - rodata index variety -*-C++*-=//
//
// Follow-on to #464 crc8: that root-caused the modulo-strength-reduced rodata
// induction pointer (`tab[(i+huge)%n]`).  These exercise neighbouring rodata
// index shapes that drive the same induction / indexed-global / constant-pool
// resolvers through different DAGs, to confirm the fix generalizes and to flush
// any remaining base-symbolization gaps:
//
//   * mod2d     - 2D `tab[(i+s>>9)%R][(j+s>>13)%C]`, both indices large-mod.
//   * multitab  - three rodata tables indexed `(i+huge)%n` in one loop.
//   * cpvm      - bytecode VM whose operands are loaded from a rodata constant
//                 pool indexed inside the dispatch loop (VM + rodata table).
//   * strroll   - rolling polynomial hash walking a rodata string via `p++`
//                 induction started at `tab + (huge%len)`.
//   * permute   - pointer-chase `p = perm[p]` through a rodata permutation table.
//   * modacc    - `tab[acc%n]` where acc is a large loop-carried accumulator.
//
// All integer, rodata tables are file-scope `static const`, indices bounded by
// `%n`, trip counts defeat unrolling, no float / 64-bit divide helper.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress56RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress56RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress56RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress56RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress56RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress56RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress56RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress56RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress56TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 2D rodata table, both indices a large-mod of loop var + runtime value.
    {p+"_mod2d",
     "static const unsigned M[5][5]={"
     "{3,1,4,1,5},{9,2,6,5,3},{5,8,9,7,9},{3,2,3,8,4},{6,2,6,4,3}};\n"
     +t+" "+p+"_mod2d("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int i=0;i<220;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned r=(unsigned)(i+(s>>9))%5u, c=(unsigned)(i+(s>>13))%5u;\n"
     "    h=h*131u+M[r][c]; h^=h>>11; }\n"
     "  return ("+t+")h; }\n",
     {0x41u}, "OptStress56", 2},

    // Three rodata tables indexed by the same large-mod induction in one loop.
    {p+"_multitab",
     "static const unsigned A[12]={7,3,11,2,13,5,17,9,19,1,23,8};\n"
     "static const unsigned char B[12]={2,4,6,8,10,12,14,16,18,20,22,24};\n"
     "static const unsigned short C[12]={100,200,300,400,500,600,700,800,"
     "900,110,220,330};\n"
     +t+" "+p+"_multitab("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned j=(unsigned)(i+(s>>8))%12u;\n"
     "    h=h*131u+A[j]+B[j]+C[j]; h^=h>>9; }\n"
     "  return ("+t+")h; }\n",
     {0x42u}, "OptStress56", 2},

    // Bytecode VM whose operands are loaded from a rodata constant pool, the
    // pool indexed by a running value inside the dispatch loop.
    {p+"_cpvm",
     "static const unsigned char prog[24]={"
     "0,1, 4, 0,2, 3, 6, 0,0, 4, 2, 0,3, 5, 6, 4, 0,1, 3, 7, 0,2, 4, 0xFF};\n"
     "static const unsigned pool[8]={5,9,17,33,65,129,257,513};\n"
     +t+" "+p+"_cpvm("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<60;it++){ s=s*1103515245u+12345u;\n"
     "    unsigned acc=s>>5; int pc=0; unsigned k=(s>>3)&7u;\n"
     "    while(pc<23 && prog[pc]!=0xFF){ unsigned char op=prog[pc++];\n"
     "      switch(op){\n"
     "        case 0: acc+=pool[prog[pc++]&7u]; break;\n"
     "        case 2: acc^=pool[k]; k=(k+1)&7u; break;\n"
     "        case 3: acc=(acc<<3)|(acc>>29); break;\n"
     "        case 4: acc*=pool[(acc>>2)&7u]|1u; break;\n"
     "        case 5: acc-=pool[k]; break;\n"
     "        case 6: acc^=acc>>13; break;\n"
     "        default: acc+=pool[7]; break; } }\n"
     "    out=out*131u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x43u}, "OptStress56", 2},

    // Pointer-chase `p = perm[p]` through a rodata permutation table.
    {p+"_permute",
     "static const unsigned char P[16]={"
     "7,12,3,9,0,14,5,11,2,15,8,1,13,6,10,4};\n"
     +t+" "+p+"_permute("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int it=0;it<200;it++){ s=s*1103515245u+12345u;\n"
     "    unsigned x=(s>>5)&15u;\n"
     "    for(int k=0;k<10;k++){ x=P[x]; h=h*131u+x; }\n"
     "    h^=h>>12; }\n"
     "  return ("+t+")h; }\n",
     {0x45u}, "OptStress56", 2},

    // Large loop-carried accumulator drives `tab[acc%n]` each step.
    {p+"_modacc",
     "static const unsigned T[20]={2,3,5,7,11,13,17,19,23,29,31,37,41,43,47,"
     "53,59,61,67,71};\n"
     +t+" "+p+"_modacc("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0, acc=s|1u;\n"
     "  for(int i=0;i<210;i++){ s=s*1103515245u+12345u;\n"
     "    acc=acc*2654435761u+(s>>7);\n"
     "    h=h*131u+T[acc%20u]; h^=h>>8; }\n"
     "  return ("+t+")h; }\n",
     {0x46u}, "OptStress56", 2},
  };
}

// Rolling polynomial hash over a rodata string via `p++` induction whose start
// is `tab + (huge % len)`, with a `if(!*p) p=W` wrap-around.  clang -O2 unrolls
// the inner walk and lowers the reset to a SELECT/PHI of `&W` and `p+1`; the
// rodata base is recovered by the induction resolver's SELECT-arm + PHI-arg
// scan and DagRodata anchor (#465).  Instantiated for x86-64 / AArch64 only: on
// i386 (GOTOFF) / ARM32 (literal pool) the unrolled walk mixes two addressing
// models for the SAME rodata pointer — the induction resolver anchors the `*p`
// loads to `@run + (val - origVA)` (original-VA model) while the lookahead
// `*(p+1)` loads emit a raw `inttoptr` of the symbolized `&W` pointer
// (recompiled-VA model from getVar/tryResolveGlobalData); the two conventions
// are mutually exclusive for one value, so this remains the next 32-bit PIC-base
// string-walk specialization (orthogonal: the 64-bit path is fully fixed).
static RoundTripTC makeStrrollTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {p+"_strroll",
     "static const char W[]=\"the_quick_brown_fox_jumps_over_the_lazy_dog_0123\";\n"
     +t+" "+p+"_strroll("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0; const int N=(int)(sizeof(W)-1);\n"
     "  for(int it=0;it<160;it++){ s=s*1103515245u+12345u;\n"
     "    const char *p=W+((s>>6)%(unsigned)N); unsigned r=0;\n"
     "    for(int k=0;k<12;k++){ r=r*31u+(unsigned)(unsigned char)*p;\n"
     "      p++; if(!*p) p=W; }\n"
     "    h=h*131u+r; h^=h>>10; }\n"
     "  return ("+t+")h; }\n",
     {0x44u}, "OptStress56", 2};
}
// clang-format on

static std::vector<RoundTripTC> withStrroll(std::vector<RoundTripTC> V,
                                            const char *p, const char *T) {
  V.push_back(makeStrrollTC(p, T));
  return V;
}

static const std::vector<RoundTripTC> kX64 =
    withStrroll(makeOptStress56TC("x64o56", "long"), "x64o56", "long");
static const std::vector<RoundTripTC> kX86 =
    withStrroll(makeOptStress56TC("x86o56", "int"), "x86o56", "int");
static const std::vector<RoundTripTC> kA64 =
    withStrroll(makeOptStress56TC("a64o56", "long"), "a64o56", "long");
static const std::vector<RoundTripTC> kARM =
    withStrroll(makeOptStress56TC("armo56", "int"), "armo56", "int");

INSTANTIATE_TEST_SUITE_P(OptStress56, X64OptStress56RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress56, X86OptStress56RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress56, A64OptStress56RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress56, ARM32OptStress56RT, ::testing::ValuesIn(kARM), rtTCName);
