//===- AllPlatform_OptStress328RTTests.cpp - subreg/flag/narrow probes ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Coverage guardrails over the historically bug-prone NeverD value-tracking
// areas — sub-register aliasing, sign/zero-extension merged through a PHI,
// one comparison fanned out to several consumers, load/store width narrowing
// (union type-punning), and variable shift/mask sub-word math — exercised at
// the size/speed optimizer levels where the self-written MedIR passes (SSA,
// MedFlags, MedDCE, LowToMed sub-register rewrite) see the densest value flow.
//
//   * extmerge   - a byte is sign-extended on one path and zero-extended on
//                  another, merged at a PHI, then consumed 64-bit wide.
//   * subwordmix - byte/halfword writes into a wide rotating accumulator.
//   * flagfanout - one `x < k` compare feeds a cmov, a setcc-zext, and a branch.
//   * narrowstore- store a 64-bit value, reload its int/short/signed-char slices
//                  (memory width narrowing + signed/unsigned sub-width loads).
//   * shiftmaskmix- variable rotate + variable low-bit mask folded wide.
//
// Integer in / integer out, stack-local, LCG-seeded, folded single return; the
// 32-bit targets stay libcall-free (no i64 div, no i64 variable shift — only
// constant i64 shifts, 32x32 widening and small-constant i64 multiply).  All
// four targets, mixed -O2 / -Os / -Oz.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress328RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress328RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress328RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress328RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress328RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress328RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress328RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress328RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress328TC(const char *prefix,
                                                   const char *T) {
  std::string p = prefix, t = T;
  std::vector<RoundTripTC> v = {
    // A sub-byte sign-extended on one path, zero-extended on another, merged at
    // a PHI, then consumed 64-bit wide — stresses sub-register + extension + PHI.
    {p+"_extmerge",
     t+" "+p+"_extmerge("+t+" a){ unsigned w=(unsigned)a|1u; long long acc=0;\n"
     "  for(int i=0;i<64;i++){ w=w*1103515245u+12345u; long long v;\n"
     "    if(w&1) v=(long long)(signed char)w;\n"
     "    else    v=(long long)(unsigned char)(w>>8);\n"
     "    acc += v; acc ^= acc>>13; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n",
     {0x1234u}, "OptStress328", 2, "-O2"},

    // Byte / halfword writes into a wide rotating accumulator (sub-register
    // partial writes preserved across rotates).
    {p+"_subwordmix",
     t+" "+p+"_subwordmix("+t+" a){ unsigned w=(unsigned)a^0xa5u, acc=0;\n"
     "  for(int i=0;i<80;i++){ w=w*22695477u+1u;\n"
     "    unsigned char b=(unsigned char)(w>>3);\n"
     "    unsigned short h=(unsigned short)(w>>11);\n"
     "    acc=(acc<<8)|b; acc^=((unsigned)h<<3); acc=(acc>>5)|(acc<<27); }\n"
     "  return ("+t+")acc; }\n",
     {0x2345u}, "OptStress328", 2, "-Os"},

    // One `x < k` comparison fanned out to a cmov, a setcc-zext, and a branch.
    {p+"_flagfanout",
     t+" "+p+"_flagfanout("+t+" a){ unsigned w=(unsigned)a|1u; long long acc=0;\n"
     "  for(int i=0;i<64;i++){ w=w*1103515245u+12345u; int x=(int)w;\n"
     "    int c = x < 1000;\n"
     "    acc += c ? x : -x;\n"
     "    acc += (long long)c * 7;\n"
     "    if(c) acc ^= 0x5a5a5a5a;\n"
     "    acc ^= acc>>17; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n",
     {0x3456u}, "OptStress328", 2, "-O2"},

    // Store a 64-bit value, reload its int / short / signed-char slices —
    // memory width narrowing plus signed/unsigned sub-width loads.
    {p+"_narrowstore",
     t+" "+p+"_narrowstore("+t+" a){ unsigned w=(unsigned)a+0x9u; long long acc=0;\n"
     "  union { long long q; int i[2]; short s[4]; signed char c[8]; } u;\n"
     "  for(int i=0;i<48;i++){ w=w*1664525u+1013904223u;\n"
     "    u.q=(long long)(int)w*3+i;\n"
     "    acc+=u.i[0]; acc-=(long long)u.s[1]; acc^=(long long)u.c[3];\n"
     "    acc ^= acc>>23; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n",
     {0x4567u}, "OptStress328", 2, "-Os"},

    // Variable rotate + variable low-bit mask folded into a wide accumulator
    // (shift amounts kept in range so no shift-by-bitwidth UB).
    {p+"_shiftmaskmix",
     t+" "+p+"_shiftmaskmix("+t+" a){ unsigned w=(unsigned)a|1u, acc=0;\n"
     "  for(int i=0;i<80;i++){ w=w*214013u+2531011u;\n"
     "    unsigned s=(w>>5)&31;\n"
     "    acc += (w<<s)|(w>>((32-s)&31));\n"
     "    acc ^= (w & ((1u<<(s&15))-1u));\n"
     "    acc=(acc>>3)|(acc<<29); }\n"
     "  return ("+t+")acc; }\n",
     {0x5678u}, "OptStress328", 2, "-Oz"},
  };
  return v;
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress328TC("x64o328", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress328TC("x86o328", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress328TC("a64o328", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress328TC("armo328", "int");

INSTANTIATE_TEST_SUITE_P(OptStress328, X64OptStress328RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress328, X86OptStress328RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress328, A64OptStress328RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress328, ARM32OptStress328RT, ::testing::ValuesIn(kARM), rtTCName);
