//===- AllPlatform_SelectBaseTableRTTests.cpp - select table base -*-C++*-=//
//
// Regression for the #469 fix: a `(cond ? A : B)[i]` access whose two table
// bases are distinct read-only globals lowers to a load whose address is
// INT_ADD(SELECT(baseA, baseB), idx) — the literal-pool/rodata base is a
// conditional select, not one constant.  tryResolveSelectBaseLitTable peels the
// index addends, resolves each select arm's base to its rebuilt rodata global,
// and selects between the two indexed pointers so the table reads are correctly
// relocated instead of left at the original (un-relocated) addresses.
//
// Instantiated for x86-64 / i386 / AArch64 / ARM32.  On x86-64 clang -O2 lowers
// `cond ? A : B` for two adjacent equal-size tables into a branchless bitwise
// blend of the two addresses (`(m&A)|(~m&B)`, the `cmov` lowering); the #470 fix
// teaches tryResolveSelectBaseLitTable to recognize that blend (mask = -zext
// (cond)) and resolve each arm's rip-relative rodata base, in addition to the
// ARM/i386 SELECT-of-literal-pool form.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64SelBaseRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64SelBaseRT, Verify) { roundTripX64(GetParam()); }
class X86SelBaseRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86SelBaseRT, Verify) { roundTripX86(GetParam()); }
class A64SelBaseRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64SelBaseRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32SelBaseRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32SelBaseRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeSelBaseTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Select between two rodata table bases, then runtime index both halves.
    {p+"_tabofsel",
     "static const unsigned char A[12]={3,9,14,21,27,33,40,48,55,61,68,74};\n"
     "static const unsigned char B[12]={7,1,17,5,29,11,23,2,19,8,31,13};\n"
     +t+" "+p+"_tabofsel("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int i=0;i<300;i++){ s=s*1103515245u+12345u;\n"
     "    const unsigned char *T2=((s>>4)&1u)?A:B;\n"
     "    h=h*131u+T2[(s>>6)%12u]+T2[(s>>10)%12u]; h^=h>>13; }\n"
     "  return ("+t+")h; }\n",
     {0x21u}, "SelBaseTable", 2},

    // Select base by a three-way chain mapped to two tables, then index.
    {p+"_tabsel3",
     "static const unsigned short P[8]={101,103,107,109,113,127,131,137};\n"
     "static const unsigned short Q[8]={2,3,5,7,11,13,17,19};\n"
     +t+" "+p+"_tabsel3("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int i=0;i<300;i++){ s=s*1103515245u+12345u;\n"
     "    const unsigned short *T2=((s>>5)%3u==0)?P:Q;\n"
     "    h=h*131u+T2[(s>>8)&7u]; h^=h>>11; }\n"
     "  return ("+t+")h; }\n",
     {0x22u}, "SelBaseTable", 2},
  };
}

// A nested/chained 4-way table select `(c0?(c1?A:B):(c2?C:D))[i]`, which clang
// -O2 lowers to branches plus a *cross-block PHI* base merging the inner
// selects/blends (rather than a flat select) — resolved by the #470
// tryResolveSelectMergeTable uniform anchor.
static std::vector<RoundTripTC> makeNestSelTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    {p+"_nestsel",
     "static const unsigned char A[8]={3,9,14,21,27,33,40,48};\n"
     "static const unsigned char B[8]={7,1,17,5,29,11,23,2};\n"
     "static const unsigned char C[8]={2,4,6,8,10,12,14,16};\n"
     "static const unsigned char D[8]={1,3,5,7,9,11,13,15};\n"
     +t+" "+p+"_nestsel("+t+" a){ unsigned s=(unsigned)a, h=0;\n"
     "  for(int i=0;i<300;i++){ s=s*1103515245u+12345u;\n"
     "    const unsigned char *T2=((s>>4)&1u)?(((s>>5)&1u)?A:B):(((s>>6)&1u)?C:D);\n"
     "    h=h*131u+T2[(s>>8)&7u]; h^=h>>11; }\n"
     "  return ("+t+")h; }\n",
     {0x23u}, "SelBaseTable", 2},
  };
}
// clang-format on

static std::vector<RoundTripTC> concatTC(std::vector<RoundTripTC> A,
                                         const std::vector<RoundTripTC> &B) {
  A.insert(A.end(), B.begin(), B.end());
  return A;
}

// All four targets cover the nested cross-block PHI base (nestsel): x86-64/ARM32
// resolve it through tryResolveSelectMergeTable (uniform anchor over the merged
// rodata run), AArch64 through the flat nested CSEL select path, and i386 through
// getVar resolving each PIC `get-PC + GOTOFF` base spilled to the stack as a
// rebuilt-global pointer.
static const std::vector<RoundTripTC> kX64 =
    concatTC(makeSelBaseTC("x64sb", "long"), makeNestSelTC("x64sb", "long"));
static const std::vector<RoundTripTC> kX86 =
    concatTC(makeSelBaseTC("x86sb", "int"), makeNestSelTC("x86sb", "int"));
static const std::vector<RoundTripTC> kA64 =
    concatTC(makeSelBaseTC("a64sb", "long"), makeNestSelTC("a64sb", "long"));
static const std::vector<RoundTripTC> kARM =
    concatTC(makeSelBaseTC("armsb", "int"), makeNestSelTC("armsb", "int"));

INSTANTIATE_TEST_SUITE_P(SelBaseTable, X64SelBaseRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(SelBaseTable, X86SelBaseRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(SelBaseTable, A64SelBaseRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(SelBaseTable, ARM32SelBaseRT, ::testing::ValuesIn(kARM), rtTCName);
