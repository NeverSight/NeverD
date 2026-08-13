//===- AllPlatform_OptStress48RTTests.cpp - switch lookup-table variants -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Follow-on probes for clang's switch-to-lookup-table after the #456 fix for the
// i386 PIC GOTOFF base that folds to `table - min_case*stride` and lands before
// its rodata segment.  #456's `rangejt` used a 4-byte (i32) value table; these
// exercise the same negative-base form with OTHER element sizes and table kinds,
// which take separate emitter paths:
//   * byte (i8) value table   -> movzbl tab@GOTOFF-min(%got,%idx,1)
//   * half (i16) value table  -> movzwl tab@GOTOFF-2*min(%got,%idx,2)
//   * large min_case          -> a large negative base offset
//   * string-pointer table    -> a `.rodata` table of PC-relative 32-bit offsets
//                                pointing across into `.rodata.str1.1` strings
//                                (`string = table_base + table[idx]`), the
//                                cross-section relative data-pointer table fixed
//                                by the merged-rodata-run embedding (#456).
// Each kernel REUSES the switch value after the switch (folds it into the hash),
// so clang keeps the index unbiased and indexes the table with the negative
// base, and uses NON-linear case results so the switch is a real table (not
// folded to arithmetic).  All four targets, value-dependent loop hash.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress48RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress48RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress48RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress48RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress48RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress48RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress48RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress48RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress48TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // bytetab: non-linear i8 results -> byte value table, index reused (unbiased
    // -> negative GOTOFF base, sub-byte movzbl access).
    {p+"_bytetab",
     t+" "+p+"_bytetab("+t+" a){\n"
     "  unsigned s=(unsigned)a, acc=0;\n"
     "  for(int i=0;i<300;i++){\n"
     "    unsigned c=(s>>4)&15u; int r;\n"
     "    switch(c){\n"
     "      case 4: r=7; break; case 5: r=3; break; case 6: r=11; break;\n"
     "      case 7: r=2; break; case 8: r=13; break; case 9: r=5; break;\n"
     "      case 10: r=17; break; case 11: r=9; break; default: r=1; }\n"
     "    acc=acc*131u+(unsigned)r+c;\n"
     "    s=s*1103515245u+12345u; }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x80u}, "OptStress48", 2},

    // shorttab: non-linear i16 results -> half value table, index reused.
    {p+"_shorttab",
     t+" "+p+"_shorttab("+t+" a){\n"
     "  unsigned s=(unsigned)a, acc=0;\n"
     "  for(int i=0;i<300;i++){\n"
     "    unsigned c=(s>>4)&15u; int r;\n"
     "    switch(c){\n"
     "      case 4: r=777; break; case 5: r=301; break; case 6: r=2500; break;\n"
     "      case 7: r=140; break; case 8: r=4096; break; case 9: r=999; break;\n"
     "      case 10: r=1717; break; case 11: r=512; break; default: r=33; }\n"
     "    acc=acc*131u+(unsigned)r+c;\n"
     "    s=s*1103515245u+12345u; }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x81u}, "OptStress48", 2},

    // bigmin: cases at a large base (100..114) -> large negative GOTOFF offset.
    {p+"_bigmin",
     t+" "+p+"_bigmin("+t+" a){\n"
     "  unsigned s=(unsigned)a, acc=0;\n"
     "  for(int i=0;i<300;i++){\n"
     "    unsigned c=((s>>4)%24u)+100u; int r;\n"
     "    switch(c){\n"
     "      case 100: r=11; break; case 102: r=29; break; case 104: r=7; break;\n"
     "      case 106: r=43; break; case 108: r=19; break; case 110: r=3; break;\n"
     "      case 112: r=37; break; case 114: r=13; break; default: r=2; }\n"
     "    acc=acc*131u+(unsigned)r+c;\n"
     "    s=s*1103515245u+12345u; }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x82u}, "OptStress48", 2},
  };
}

// strsw: switch returns string literals.  The 64-bit targets lower this to a
// `.rodata` table of PC-relative 32-bit offsets pointing across into
// `.rodata.str1.1` (`string = table_base + table[idx]`), fixed by the merged-
// rodata-run embedding + R_AARCH64_PREL32 application; i386 emits an ABSOLUTE
// pointer table in `.data.rel.ro` (R_386_32) fixed by the data-pointer-table
// rebuild (each slot reemitted as `ptrtoint(@recompiled_string)`).  ARM32 also
// emits an absolute `.data.rel.ro` table but merges the switch dispatch and the
// pointer load through a PHI of already-resolved pointers, where the induction-
// pointer resolver still re-anchors the resolved pointer — a separate deferred
// case — so strsw is instantiated for x86-64 / AArch64 / i386 only.
static RoundTripTC makeStrswTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {p+"_strsw",
     t+" "+p+"_strsw("+t+" a){\n"
     "  unsigned s=(unsigned)a, acc=0;\n"
     "  for(int i=0;i<300;i++){\n"
     "    unsigned c=(s>>3)%6u; const char *w;\n"
     "    switch(c){\n"
     "      case 0: w=\"alpha\"; break; case 1: w=\"bravo\"; break;\n"
     "      case 2: w=\"charlie\"; break; case 3: w=\"delta\"; break;\n"
     "      case 4: w=\"echo\"; break; default: w=\"fox\"; }\n"
     "    acc=acc*131u+(unsigned)(unsigned char)w[0]+((unsigned)(unsigned char)w[1]<<8)+c;\n"
     "    s=s*1103515245u+12345u; }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x83u}, "OptStress48", 2};
}
// clang-format on

static std::vector<RoundTripTC> withStrsw(std::vector<RoundTripTC> V,
                                          const char *prefix, const char *T) {
  V.push_back(makeStrswTC(prefix, T));
  return V;
}

// strsw runs on x86-64 / AArch64 (relative offset table) and i386 (absolute
// `.data.rel.ro` pointer table, the case-value index also re-validated so the
// loop bound 200 — which collides with the string VA 0xC8 — is not redirected
// to that string global).  ARM32 is deferred: its switch DEFAULT case ("fox")
// is reached through an ARM literal-pool PC-relative address (`ldr[pc]+pc`)
// merged with the table-loaded pointers via a PHI, and that merged literal-pool
// string base is not yet symbolized (the 32-bit-PIC literal-pool string-base
// specialization, the Unicorn unsupported-instructions doc #473).  The 5 table cases resolve
// correctly through the rebuilt `@__nd_codeptr` table; only the default arm is
// affected.
static const std::vector<RoundTripTC> kX64 =
    withStrsw(makeOptStress48TC("x64o48", "long"), "x64o48", "long");
static const std::vector<RoundTripTC> kX86 =
    withStrsw(makeOptStress48TC("x86o48", "int"), "x86o48", "int");
static const std::vector<RoundTripTC> kA64 =
    withStrsw(makeOptStress48TC("a64o48", "long"), "a64o48", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress48TC("armo48", "int");

INSTANTIATE_TEST_SUITE_P(OptStress48, X64OptStress48RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress48, X86OptStress48RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress48, A64OptStress48RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress48, ARM32OptStress48RT, ::testing::ValuesIn(kARM), rtTCName);
