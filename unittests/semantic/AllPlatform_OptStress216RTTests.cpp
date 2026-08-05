//===- AllPlatform_OptStress216RTTests.cpp - global pointer indirection ==//
//
// Symbolization probes for data-pointer tables whose entries are themselves
// addresses of OTHER globals — the layer above a plain rodata lookup table.
// clang -O2 lowers each table entry to a pointer relocation (`.data.rel.ro`
// with R_*_RELATIVE / R_*_64, or a GOTOFF pair on i386/ARM32 PIC), so the
// recompiled image must re-point every slot at the *rebuilt* global, not the
// original (now unmapped) VA.  These shapes stress slots the earlier
// pointer-table fixes (#483 gpstab/gpcswap) did not pin together:
//
//   * ptab  - `static const int *const tabs[3]` selecting one of three global
//             int[] arrays at runtime, then indexing the chosen array (a global
//             pointer table pointing at global data, double-indirect).
//   * srec  - a `struct { const int *p; unsigned k; }[]` table: each record
//             carries a global pointer field AND a scalar, so the pointer slot
//             is symbolized while the neighbouring scalar stays a plain value.
//   * pdiff - difference of two pointers into the SAME global (the absolute VA
//             must cancel: a correct lift keeps the index delta regardless of
//             where the rebuilt global lands).
//
// Integer in / integer out, LCG-seeded, folded to one integer return; no float
// / 64-bit divide / libcall.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress216RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress216RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress216RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress216RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress216RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress216RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress216RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress216RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress216TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Global pointer table -> global int[] arrays, runtime double-indirect.
    {p+"_ptab",
     t+" "+p+"_ptab("+t+" a){\n"
     "  static const int A[8]={11,22,33,44,55,66,77,88};\n"
     "  static const int B[8]={2,3,5,7,11,13,17,19};\n"
     "  static const int C[8]={101,202,303,404,505,606,707,808};\n"
     "  static const int *const tabs[3]={A,B,C};\n"
     "  unsigned acc=(unsigned)a;\n"
     "  for(int i=0;i<96;i++){ const int *tp=tabs[acc%3u];\n"
     "    acc=acc*131u+(unsigned)tp[(acc>>2)&7u]+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x1234567u}, "OptStress216", 2},

    // Struct table mixing a global pointer field and a scalar field.
    {p+"_srec",
     "struct "+p+"_rec{ const int *q; unsigned k; };\n"
     +t+" "+p+"_srec("+t+" a){\n"
     "  static const int A[6]={3,9,27,81,243,729};\n"
     "  static const int B[6]={5,25,125,625,3125,15625};\n"
     "  static const struct "+p+"_rec recs[2]={{A,7u},{B,11u}};\n"
     "  unsigned acc=(unsigned)a;\n"
     "  for(int i=0;i<90;i++){ const struct "+p+"_rec *r=&recs[(acc>>1)&1u];\n"
     "    acc=acc*131u+(unsigned)r->q[acc%6u]+r->k*(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x7654321u}, "OptStress216", 2},

    // Difference of two pointers into the same global: VA must cancel.
    {p+"_pdiff",
     t+" "+p+"_pdiff("+t+" a){\n"
     "  static const int arr[16]={\n"
     "    1,4,9,16,25,36,49,64,81,100,121,144,169,196,225,256};\n"
     "  unsigned acc=(unsigned)a;\n"
     "  for(int i=0;i<88;i++){ const int *pp=&arr[(acc>>2)&15u];\n"
     "    const int *qq=&arr[(acc>>5)&15u];\n"
     "    long d=pp-qq; acc=acc*131u+(unsigned)(d+16)+(unsigned)*pp; }\n"
     "  return ("+t+")acc; }\n",
     {0x2468ACEu}, "OptStress216", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress216TC("x64o216", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress216TC("x86o216", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress216TC("a64o216", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress216TC("armo216", "int");

INSTANTIATE_TEST_SUITE_P(OptStress216, X64OptStress216RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress216, X86OptStress216RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress216, A64OptStress216RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress216, ARM32OptStress216RT, ::testing::ValuesIn(kARM), rtTCName);
