//===- AllPlatform_OptStress219RTTests.cpp - rodata struct-table index ===//
//
// Constant-pool probes for a `static const` table of STRUCTS (mixed-width fields
// with padding) indexed at runtime — the access is `base + idx*sizeof(struct) +
// offsetof(field)`, a scaled-by-struct-stride index plus a field offset, which
// is a different rodata-redirection shape than the scalar-array lookup tables in
// ConstPool (those scale by the element width alone).  The recompiled image must
// re-embed the whole struct table and re-point the base so every field load
// (byte tag, halfword, word key) lands inside the rebuilt global instead of the
// original (now unmapped) VA.  A `union` type-pun case reads the same rodata
// bytes through two different field widths.
//
//   * stab  - table of {u8,u16,u32} structs; all three fields read per step.
//   * s2d   - struct table indexed by a data-derived row, nested field select.
//   * upun  - union table read as bytes vs as a word (overlapping field offsets).
//
// Integer in / integer out, LCG-seeded, folded to one integer return; no float
// / divide / libcall.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress219RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress219RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress219RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress219RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress219RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress219RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress219RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress219RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress219TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Table of {u8,u16,u32} structs; every field read per iteration.
    {p+"_stab",
     "struct "+p+"_e{ unsigned char tag; unsigned short w; unsigned key; };\n"
     +t+" "+p+"_stab("+t+" a){\n"
     "  static const struct "+p+"_e tbl[8]={\n"
     "    {1,1001,100000001u},{2,2002,200000002u},{3,3003,300000003u},\n"
     "    {4,4004,400000004u},{5,5005,500000005u},{6,6006,600000006u},\n"
     "    {7,7007,700000007u},{8,8008,800000008u}};\n"
     "  unsigned acc=(unsigned)a;\n"
     "  for(int i=0;i<96;i++){ const struct "+p+"_e *e=&tbl[(acc>>3)&7u];\n"
     "    acc=acc*131u+e->key+(unsigned)e->w*7u+(unsigned)e->tag*3u+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x1234567u}, "OptStress219", 2},

    // Struct table indexed by a data-derived row; nested field select via cond.
    {p+"_s2d",
     "struct "+p+"_p{ unsigned lo; unsigned hi; };\n"
     +t+" "+p+"_s2d("+t+" a){\n"
     "  static const struct "+p+"_p m[6]={\n"
     "    {0x11111111u,0x22222222u},{0x33333333u,0x44444444u},\n"
     "    {0x55555555u,0x66666666u},{0x77777777u,0x88888888u},\n"
     "    {0x99999999u,0xAAAAAAAAu},{0xBBBBBBBBu,0xCCCCCCCCu}};\n"
     "  unsigned acc=(unsigned)a;\n"
     "  for(int i=0;i<90;i++){ const struct "+p+"_p *r=&m[(acc>>2)%6u];\n"
     "    unsigned v=(acc&1u)?r->hi:r->lo; acc=acc*131u+v+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x7654321u}, "OptStress219", 2},

    // Union table: read the same rodata bytes as 4 bytes or as 1 word.
    {p+"_upun",
     "union "+p+"_u{ unsigned w; unsigned char b[4]; };\n"
     +t+" "+p+"_upun("+t+" a){\n"
     "  static const union "+p+"_u tbl[6]={\n"
     "    {0x04030201u},{0x08070605u},{0x0C0B0A09u},\n"
     "    {0x100F0E0Du},{0x14131211u},{0x18171615u}};\n"
     "  unsigned acc=(unsigned)a;\n"
     "  for(int i=0;i<88;i++){ const union "+p+"_u *u=&tbl[(acc>>2)%6u];\n"
     "    acc=acc*131u+u->w+(unsigned)u->b[acc&3u]*7u+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x2468ACEu}, "OptStress219", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress219TC("x64o219", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress219TC("x86o219", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress219TC("a64o219", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress219TC("armo219", "int");

INSTANTIATE_TEST_SUITE_P(OptStress219, X64OptStress219RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress219, X86OptStress219RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress219, A64OptStress219RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress219, ARM32OptStress219RT, ::testing::ValuesIn(kARM), rtTCName);
