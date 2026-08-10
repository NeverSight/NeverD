//===- AllPlatform_OptStress336RTTests.cpp - i64 pair × switch dispatch --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// New shape at the intersection of two recently-fragile mechanisms: the i64
// register-PAIR modeling on 32-bit targets (i386 EDX:EAX, ARM32 R1:R0 — the
// #524-#528 family) AND switch/jump-table recovery (#530/#532).  OptStress326
// `swwide` drove a switch selector taken from the LOW half of an i64 register
// pair, but nothing has driven:
//
//   * _hisel   : selector from the HIGH half `(unsigned)(acc>>32)&7` of a
//                LOOP-CARRIED i64 accumulator — the dispatch index lives in the
//                high pair register (EDX/R1) and must be reconstructed past the
//                jump-table resolver's index normalization.
//   * _dense16 : 16-way dense jump table on the high-half selector `&15`, with
//                the i64 acc threaded through every arm — a wider table reached
//                from the high register.
//   * _xorsel  : selector `(unsigned)(acc ^ (acc>>32))&7` mixes BOTH halves, so
//                both pair registers feed the index computation in the dispatch
//                block before the jump.
//   * _armthread : normal u32 selector but each of 16 arms applies a different
//                i64 op to the loop-carried acc — stresses i64 pair PHI/liveness
//                across a multi-way branch + back-edge (the #524 loop-carried-i64
//                story, but across switch arms instead of calls).
//
// All i64 math is libcall-free on i386/ARM32: add/sub/xor/and, CONSTANT shifts,
// and i64×small-const only (no i64 divide, no variable i64 shift).  Power-of-two
// index masks (no `%`/`/`).  Deterministic LCG seed, folds both halves into one
// return so a dropped high word still surfaces.  x64/a64 (single 64-bit reg) are
// controls; i386/ARM32 (register pairs) are the targets.  -O2, all four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress336RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress336RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress336RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress336RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress336RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress336RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress336RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress336RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress336TC(const char *prefix,
                                                   const char *T, int Opt) {
  std::string p = prefix, t = T;
  std::vector<RoundTripTC> v = {
    // Switch selector from the HIGH half of a loop-carried i64 accumulator.
    {p+"_hisel",
     t+" "+p+"_hisel("+t+" a){ unsigned long long acc=(unsigned long long)(unsigned)a|1ull;\n"
     "  unsigned w=(unsigned)a^0x9e3779b9u;\n"
     "  for(int i=0;i<60;i++){ w=w*1664525u+1013904223u;\n"
     "    acc += ((unsigned long long)w<<32) ^ (unsigned long long)(w>>7);\n"
     "    switch((unsigned)(acc>>32)&7u){\n"
     "      case 0: acc ^= (unsigned long long)w<<11; break;\n"
     "      case 1: acc -= (unsigned long long)w; break;\n"
     "      case 2: acc += (unsigned long long)w<<3; break;\n"
     "      case 3: acc ^= acc>>13; break;\n"
     "      case 4: acc += (unsigned long long)(w&0xffffu)<<40; break;\n"
     "      case 5: acc -= (unsigned long long)w<<5; break;\n"
     "      case 6: acc ^= (unsigned long long)w*7u; break;\n"
     "      default: acc += (unsigned long long)w<<17; break; }\n"
     "    acc ^= acc>>29; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n",
     {0x1234567ULL}, "OptStress336", Opt},

    // 16-way dense jump table on the high-half selector, i64 acc threaded.
    {p+"_dense16",
     t+" "+p+"_dense16("+t+" a){ unsigned long long acc=(unsigned long long)(unsigned)a|3ull;\n"
     "  unsigned w=(unsigned)a+0x55u;\n"
     "  for(int i=0;i<56;i++){ w=w*22695477u+1u;\n"
     "    acc += ((unsigned long long)w<<32) + (unsigned long long)i;\n"
     "    switch((unsigned)(acc>>32)&15u){\n"
     "      case 0: acc+=(unsigned long long)w; break;\n"
     "      case 1: acc^=(unsigned long long)w<<2; break;\n"
     "      case 2: acc-=(unsigned long long)w<<4; break;\n"
     "      case 3: acc+=(unsigned long long)w*3u; break;\n"
     "      case 4: acc^=(unsigned long long)w<<6; break;\n"
     "      case 5: acc-=(unsigned long long)w; break;\n"
     "      case 6: acc+=(unsigned long long)w<<8; break;\n"
     "      case 7: acc^=(unsigned long long)w*5u; break;\n"
     "      case 8: acc+=(unsigned long long)w<<10; break;\n"
     "      case 9: acc^=(unsigned long long)(w&0xffu)<<48; break;\n"
     "      case 10: acc-=(unsigned long long)w<<3; break;\n"
     "      case 11: acc+=(unsigned long long)w*9u; break;\n"
     "      case 12: acc^=(unsigned long long)w<<12; break;\n"
     "      case 13: acc-=(unsigned long long)w<<1; break;\n"
     "      case 14: acc+=(unsigned long long)w<<14; break;\n"
     "      default: acc^=acc>>17; break; }\n"
     "    acc ^= acc>>23; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n",
     {0x2345678ULL}, "OptStress336", Opt},

    // Selector mixes BOTH halves: both pair registers feed the dispatch index.
    {p+"_xorsel",
     t+" "+p+"_xorsel("+t+" a){ unsigned long long acc=(unsigned long long)(unsigned)a|1ull;\n"
     "  unsigned w=(unsigned)a^0xa5a5a5a5u;\n"
     "  for(int i=0;i<60;i++){ w=w*1103515245u+12345u;\n"
     "    acc += ((unsigned long long)w<<31) ^ (unsigned long long)w;\n"
     "    switch((unsigned)(acc ^ (acc>>32))&7u){\n"
     "      case 0: acc += (unsigned long long)w<<9; break;\n"
     "      case 1: acc ^= (unsigned long long)w; break;\n"
     "      case 2: acc -= (unsigned long long)w<<2; break;\n"
     "      case 3: acc += (unsigned long long)w*3u; break;\n"
     "      case 4: acc ^= (unsigned long long)w<<19; break;\n"
     "      case 5: acc += (unsigned long long)w<<33; break;\n"
     "      case 6: acc -= (unsigned long long)w; break;\n"
     "      default: acc ^= (unsigned long long)w<<25; break; }\n"
     "    acc += acc<<7; acc ^= acc>>31; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n",
     {0x3456789ULL}, "OptStress336", Opt},

    // Normal u32 selector; every arm threads the loop-carried i64 acc.
    {p+"_armthread",
     t+" "+p+"_armthread("+t+" a){ unsigned long long acc=(unsigned long long)(unsigned)a|7ull;\n"
     "  unsigned w=(unsigned)a^0xdeadbeefu;\n"
     "  for(int i=0;i<56;i++){ w=w*1664525u+1013904223u;\n"
     "    switch((w>>5)&15u){\n"
     "      case 0: acc=acc*3u+(unsigned long long)w; break;\n"
     "      case 1: acc^=(acc<<13)^(unsigned long long)w; break;\n"
     "      case 2: acc-=(unsigned long long)w<<8; break;\n"
     "      case 3: acc+=(acc>>7)^(unsigned long long)w; break;\n"
     "      case 4: acc^=(unsigned long long)w<<32; break;\n"
     "      case 5: acc+=(unsigned long long)w*5u; break;\n"
     "      case 6: acc-=(acc>>11); break;\n"
     "      case 7: acc^=(unsigned long long)w<<21; break;\n"
     "      case 8: acc+=(acc<<3)+(unsigned long long)w; break;\n"
     "      case 9: acc^=(unsigned long long)(w&0xffffu)<<16; break;\n"
     "      case 10: acc-=(unsigned long long)w; break;\n"
     "      case 11: acc+=(unsigned long long)w*9u; break;\n"
     "      case 12: acc^=(acc>>17)^((unsigned long long)w<<4); break;\n"
     "      case 13: acc+=(unsigned long long)w<<40; break;\n"
     "      case 14: acc-=(acc<<5); break;\n"
     "      default: acc^=(unsigned long long)w*131u; break; }\n"
     "    acc ^= acc>>27; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n",
     {0x456789AULL}, "OptStress336", Opt},
  };
  return v;
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress336TC("x64o336", "long", 2);
static const std::vector<RoundTripTC> kX86 = makeOptStress336TC("x86o336", "int", 2);
static const std::vector<RoundTripTC> kA64 = makeOptStress336TC("a64o336", "long", 2);
static const std::vector<RoundTripTC> kARM = makeOptStress336TC("armo336", "int", 2);

INSTANTIATE_TEST_SUITE_P(OptStress336, X64OptStress336RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress336, X86OptStress336RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress336, A64OptStress336RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress336, ARM32OptStress336RT, ::testing::ValuesIn(kARM), rtTCName);
