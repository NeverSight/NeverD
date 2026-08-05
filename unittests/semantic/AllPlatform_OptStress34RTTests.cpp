//===- AllPlatform_OptStress34RTTests.cpp - jump-table label probes -*-C++*-=//
//
// Targets the jump-table case-label recovery path (JumpTableResolver:
// detectNormalization / recoverCaseLabels) that OptStress33's `fieldsw` exposed
// on ARM32 (a `(expr >> j) & m` masked index whose pre-mask shift was mis-read
// as a case-label NormShift, scaling labels by the entry size).  These widen
// the coverage to 8-/16-way masked switches, non-power-of-two modulo switches,
// a nested switch (two interacting tables), and a non-trivial pre-mask index
// expression, on all four targets, so the masked-index label recovery is locked
// in and any width/nesting sibling of the fixed bug surfaces.
//
//   * mask8     - switch((s>>4)&7)  : 8-way masked index, distinct case ops.
//   * mask16    - switch((s>>1)&15) : 16-way masked index (ARM TBB byte table).
//   * mod7      - switch(s%7)       : non-power-of-two modulo (no range guard).
//   * mod10     - switch(s%10)      : modulo, ten arms.
//   * nestsw    - switch in a switch : two jump tables that interact per-iter.
//   * exprmask  - switch(((s>>5)^(s>>13))&7) : non-trivial pre-mask index expr.
//
// Integer-only, single integer return, bounded, no 64-bit divide; all four
// targets at -O2, native vs lifted.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress34RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress34RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress34RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress34RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress34RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress34RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress34RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress34RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress34TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 8-way masked index; each arm applies a distinct operation to the carried
    // accumulator so the cases cannot collapse to a data table.
    {p+"_mask8",
     t+" "+p+"_mask8("+t+" a){\n"
     "  unsigned w=(unsigned)a, s=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u;\n"
     "    switch((s>>4)&7u){\n"
     "      case 0: w+=s; break;       case 1: w^=(s>>1); break;\n"
     "      case 2: w=w*3u+1u; break;  case 3: w-=(s>>2); break;\n"
     "      case 4: w=(w<<1)|(w>>31); break; case 5: w&=s; break;\n"
     "      case 6: w|=(s>>5); break;  default: w=~w; break; }\n"
     "    h=h*131u+w; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x41ULL}, "OptStress34", 2},

    // 16-way masked index — wide enough for clang to pick a compact byte table
    // (ARM TBB) where the loaded entry is scaled separately from the index.
    {p+"_mask16",
     t+" "+p+"_mask16("+t+" a){\n"
     "  unsigned w=(unsigned)a, s=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<160;i++){ s=s*1103515245u+12345u;\n"
     "    switch((s>>1)&15u){\n"
     "      case 0: w+=1u; break;  case 1: w+=s; break;  case 2: w^=2u; break;\n"
     "      case 3: w*=3u; break;  case 4: w-=s; break;  case 5: w^=(s>>3); break;\n"
     "      case 6: w+=7u; break;  case 7: w=~w; break;  case 8: w<<=1; break;\n"
     "      case 9: w>>=1; break;  case 10: w|=s; break; case 11: w&=(s>>2); break;\n"
     "      case 12: w*=5u; break; case 13: w+=(s>>7); break; case 14: w^=s; break;\n"
     "      default: w-=1u; break; }\n"
     "    h=h*131u+w; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x53ULL}, "OptStress34", 2},

    // switch(s % 7) — non-power-of-two modulus: clang bounds via the remainder
    // (no `cmp` range guard); the index is the raw remainder in [0,7).
    {p+"_mod7",
     t+" "+p+"_mod7("+t+" a){\n"
     "  unsigned w=(unsigned)a, s=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u;\n"
     "    switch(s%7u){\n"
     "      case 0: w+=s; break;  case 1: w^=(s>>1); break; case 2: w*=3u; break;\n"
     "      case 3: w-=(s>>2); break; case 4: w=(w<<2)|(w>>30); break;\n"
     "      case 5: w|=(s>>4); break; default: w=~w+s; break; }\n"
     "    h=h*131u+w; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x6dULL}, "OptStress34", 2},

    // switch(s % 10) — ten arms, modulo bound.
    {p+"_mod10",
     t+" "+p+"_mod10("+t+" a){\n"
     "  unsigned w=(unsigned)a, s=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<150;i++){ s=s*1103515245u+12345u;\n"
     "    switch(s%10u){\n"
     "      case 0: w+=s; break;   case 1: w^=s; break;   case 2: w*=3u; break;\n"
     "      case 3: w-=(s>>1); break; case 4: w+=(s>>2); break; case 5: w=~w; break;\n"
     "      case 6: w|=(s>>3); break; case 7: w&=(s>>4); break; case 8: w<<=1; break;\n"
     "      default: w>>=1; break; }\n"
     "    h=h*131u+w; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0xb2ULL}, "OptStress34", 2},

    // Nested switch: the outer masked index selects an inner masked switch in two
    // arms, so two jump tables feed the same carried accumulator per iteration.
    {p+"_nestsw",
     t+" "+p+"_nestsw("+t+" a){\n"
     "  unsigned w=(unsigned)a, s=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u;\n"
     "    switch((s>>2)&3u){\n"
     "      case 0:\n"
     "        switch((s>>10)&3u){ case 0: w+=s; break; case 1: w^=s; break;\n"
     "                            case 2: w*=3u; break; default: w-=s; break; }\n"
     "        break;\n"
     "      case 1: w=(w<<3)|(w>>29); break;\n"
     "      case 2:\n"
     "        switch((s>>12)&3u){ case 0: w|=(s>>1); break; case 1: w&=(s>>2); break;\n"
     "                            case 2: w+=(s>>3); break; default: w^=(s>>4); break; }\n"
     "        break;\n"
     "      default: w=~w; break; }\n"
     "    h=h*131u+w; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x18ULL}, "OptStress34", 2},

    // Non-trivial pre-mask index expression: the masked switch index is built
    // from two shifts XORed, so the resolver must stop at the bounding mask and
    // not mistake either feeding shift for a label normalization.
    {p+"_exprmask",
     t+" "+p+"_exprmask("+t+" a){\n"
     "  unsigned w=(unsigned)a, s=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u;\n"
     "    switch(((s>>5)^(s>>13))&7u){\n"
     "      case 0: w+=s; break;  case 1: w^=(s>>1); break; case 2: w*=3u; break;\n"
     "      case 3: w-=(s>>2); break; case 4: w+=(s>>6); break;\n"
     "      case 5: w|=(s>>7); break; case 6: w&=(s>>8); break; default: w=~w; break; }\n"
     "    h=h*131u+w; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x2fULL}, "OptStress34", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress34TC("x64o34", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress34TC("x86o34", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress34TC("a64o34", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress34TC("armo34", "int");

INSTANTIATE_TEST_SUITE_P(OptStress34, X64OptStress34RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress34, X86OptStress34RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress34, A64OptStress34RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress34, ARM32OptStress34RT, ::testing::ValuesIn(kARM), rtTCName);
