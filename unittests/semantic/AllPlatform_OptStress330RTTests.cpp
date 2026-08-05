//===- AllPlatform_OptStress330RTTests.cpp - CFG/SSA + signed const arith ===//
//
// Coverage guardrails over control-flow / SSA recovery and signed constant
// strength reduction — areas distinct from the value-tracking probes:
//
//   * statemach  - an if-ladder state machine (shared transition blocks, the
//                  state carried across iterations) — CFG edge + PHI recovery.
//   * multiexit  - a loop with three distinct exit conditions, each folding a
//                  different result (multiple back/exit edges).
//   * deeploop   - a 3-level nested loop with a value carried across all levels.
//   * negdivmod  - signed division/modulo by constants with negative dividends
//                  (magic-number strength reduction + sign correction), the
//                  classic off-by-one/sign source; constant divisors keep it
//                  libcall-free on the 32-bit targets.
//   * condaccum  - conditional accumulation whose predicate is recomputed inside
//                  nested branches (flag / select liveness across blocks).
//
// Integer in / integer out, stack-local, LCG-seeded, folded single return;
// 32-bit targets stay libcall-free (constant divisors only, no i64 div/var
// shift).  All four targets, mixed -O2 / -Os.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress330RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress330RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress330RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress330RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress330RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress330RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress330RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress330RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress330TC(const char *prefix,
                                                   const char *T) {
  std::string p = prefix, t = T;
  std::vector<RoundTripTC> v = {
    // If-ladder state machine: the state is carried across iterations and the
    // transitions share blocks (no computed goto / jump table).
    {p+"_statemach",
     t+" "+p+"_statemach("+t+" a){ unsigned w=(unsigned)a|1u, acc=0; int st=0;\n"
     "  for(int i=0;i<160;i++){ w=w*1103515245u+12345u; unsigned k=(w>>7)&3;\n"
     "    if(st==0){ acc+=w; st=(k<2)?1:2; }\n"
     "    else if(st==1){ acc^=w<<1; st=(k==0)?0:3; }\n"
     "    else if(st==2){ acc-=w>>1; st=(k&1)?1:0; }\n"
     "    else { acc+=w*3u; st=(k>2)?2:0; }\n"
     "    acc^=acc>>5; }\n"
     "  return ("+t+")(acc + (unsigned)st); }\n",
     {0x1234u}, "OptStress330", 2, "-O2"},

    // Loop with three distinct exit conditions feeding different folded results.
    {p+"_multiexit",
     t+" "+p+"_multiexit("+t+" a){ unsigned w=(unsigned)a|1u, acc=0; int i=0;\n"
     "  for(;;){ w=w*22695477u+1u; acc+=w; acc^=acc>>6;\n"
     "    if((w&0xff)==0){ acc^=0x11111111u; break; }\n"
     "    if(acc>0x40000000u){ acc-=0x22222222u; break; }\n"
     "    if(++i>=200){ acc+=0x33333333u; break; } }\n"
     "  return ("+t+")(acc ^ (unsigned)i); }\n",
     {0x2345u}, "OptStress330", 2, "-O2"},

    // 3-level nested loop, one accumulator carried across all levels.
    {p+"_deeploop",
     t+" "+p+"_deeploop("+t+" a){ unsigned w=(unsigned)a^0x5au; unsigned acc=0;\n"
     "  for(int i=0;i<12;i++){ w=w*1664525u+1013904223u;\n"
     "    for(int j=0;j<6;j++){ unsigned r=w+(unsigned)j;\n"
     "      for(int kk=0;kk<4;kk++){ r=r*1103515245u+12345u;\n"
     "        acc+=(r>>(kk*3+1))&0x7f; acc^=acc<<3; }\n"
     "      acc-=r>>9; }\n"
     "    acc^=acc>>11; }\n"
     "  return ("+t+")acc; }\n",
     {0x3456u}, "OptStress330", 2, "-O2"},

    // Signed division/modulo by constants with negative dividends — magic-number
    // strength reduction with the signed sign-correction add-back.
    {p+"_negdivmod",
     t+" "+p+"_negdivmod("+t+" a){ unsigned w=(unsigned)a|1u; long long acc=0;\n"
     "  for(int i=0;i<96;i++){ w=w*1103515245u+12345u; int x=(int)w;\n"
     "    acc += x / 7; acc -= x % 7;\n"
     "    acc ^= (long long)(x / -3) << 2;\n"
     "    acc += x % 11; acc -= x / 13;\n"
     "    acc ^= acc>>17; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n",
     {0x4567u}, "OptStress330", 2, "-O2"},

    // Conditional accumulation whose predicate is recomputed inside nested
    // branches — flag/select liveness across the merge.
    {p+"_condaccum",
     t+" "+p+"_condaccum("+t+" a){ unsigned w=(unsigned)a|1u, acc=0;\n"
     "  for(int i=0;i<128;i++){ w=w*214013u+2531011u; int x=(int)w;\n"
     "    int hi = x > 0; int lo = x < -1000;\n"
     "    if(hi){ acc += (lo? 0u : (unsigned)x); if(x&2) acc^=w>>3; }\n"
     "    else { acc -= (lo? (unsigned)(-x) : 1u); if(x&4) acc+=w<<1; }\n"
     "    acc = (acc>>2)|(acc<<30); }\n"
     "  return ("+t+")acc; }\n",
     {0x5678u}, "OptStress330", 2, "-Os"},
  };
  return v;
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress330TC("x64o330", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress330TC("x86o330", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress330TC("a64o330", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress330TC("armo330", "int");

INSTANTIATE_TEST_SUITE_P(OptStress330, X64OptStress330RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress330, X86OptStress330RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress330, A64OptStress330RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress330, ARM32OptStress330RT, ::testing::ValuesIn(kARM), rtTCName);
