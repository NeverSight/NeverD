//===- AllPlatform_PtrTableIdxRTTests.cpp - indexed pointer tables -*-C++*-=//
//
// #456 fixed switch-returning-string POINTER tables, but only the switch-
// dispatch shape (a PHI that merges the default string pointer with the table
// loads).  A `static const char *const W[]` indexed by a runtime value — `W[i]`,
// no switch — is a different shape: the table base is materialized, scaled-
// indexed, the loaded pointer is dereferenced directly (or carried by a string
// walk).  These exercise the directly-indexed `.data.rel.ro` / relative-offset
// pointer table through tryResolveCodePtrTablePtr + the DataPtrRelocSlots
// rebuild, plus shapes the switch path never reached:
//
//   * strtab    - `const char *const W[]`, runtime index, variable-length walk
//                 of W[idx] (forces a real pointer table, not a folded char).
//   * structptr - `struct {int k; const char *s;} T[]`, access T[i].k and the
//                 embedded pointer T[i].s (pointer field at a struct offset).
//   * str2d     - 2D `const char *M[R][C]` indexed by runtime row/col.
//   * ptr2arr   - `const int *const rows[]` whose entries point at other rodata
//                 int arrays: two-level indirection rows[i][j] through globals.
//   * selptr    - `const char *p = c ? A[i] : B[j]; walk p` (a pointer SELECT of
//                 two table loads, then deref).
//   * revptr    - reverse / negative-stride walk over the pointer table.
//
// All integer folds to one return, strings/arrays are file-scope `static const`
// so clang keeps a real `.data.rel.ro` / `.rodata` pointer table (relocated),
// trip counts defeat unrolling, no float / 64-bit-divide helper.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64PtrTblRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64PtrTblRT, Verify) { roundTripX64(GetParam()); }
class X86PtrTblRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86PtrTblRT, Verify) { roundTripX86(GetParam()); }
class A64PtrTblRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64PtrTblRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32PtrTblRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32PtrTblRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makePtrTblTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // const char *const W[]: runtime index, variable-length walk of W[idx].
    {p+"_strtab",
     "static const char *const W[8]={\"alpha\",\"bravo\",\"charlie\",\"delta\","
     "\"echo\",\"foxtrot\",\"golf\",\"hotel\"};\n"
     +t+" "+p+"_strtab("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    const char *w=W[(s>>5)&7u];\n"
     "    for(int k=0;w[k];k++) h=h*131u+(unsigned)(unsigned char)w[k];\n"
     "    h^=h>>13; }\n"
     "  return ("+t+")h; }\n",
     {0x21u}, "PtrTableIdx", 2},

    // struct {int k; const char *s;} T[]: int field + embedded pointer field.
    {p+"_structptr",
     "struct E{int k; const char *s;};\n"
     "static const struct E T[6]={{3,\"red\"},{7,\"green\"},{11,\"blue\"},"
     "{13,\"cyan\"},{17,\"magenta\"},{19,\"yellow\"}};\n"
     +t+" "+p+"_structptr("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    const struct E *e=&T[(s>>6)%6u];\n"
     "    h=h*131u+(unsigned)e->k;\n"
     "    for(int k=0;e->s[k];k++) h=h*31u+(unsigned)(unsigned char)e->s[k];\n"
     "    h^=h>>11; }\n"
     "  return ("+t+")h; }\n",
     {0x22u}, "PtrTableIdx", 2},

    // 2D const char *M[R][C] indexed by runtime row/col.
    {p+"_str2d",
     "static const char *const M[3][3]={"
     "{\"aa\",\"bbb\",\"cccc\"},{\"dddd\",\"ee\",\"fff\"},"
     "{\"ggg\",\"hhhh\",\"ii\"}};\n"
     +t+" "+p+"_str2d("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    const char *w=M[(s>>5)%3u][(s>>9)%3u];\n"
     "    for(int k=0;w[k];k++) h=h*131u+(unsigned)(unsigned char)w[k];\n"
     "    h^=h>>9; }\n"
     "  return ("+t+")h; }\n",
     {0x23u}, "PtrTableIdx", 2},

    // const int *const rows[] pointing into other rodata int arrays: two-level
    // indirection rows[i][j].
    {p+"_ptr2arr",
     "static const int r0[4]={2,3,5,7};\n"
     "static const int r1[4]={11,13,17,19};\n"
     "static const int r2[4]={23,29,31,37};\n"
     "static const int r3[4]={41,43,47,53};\n"
     "static const int *const rows[4]={r0,r1,r2,r3};\n"
     +t+" "+p+"_ptr2arr("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int i=0;i<300;i++){ s=s*1103515245u+12345u;\n"
     "    const int *row=rows[(s>>6)&3u];\n"
     "    h=h*131u+(unsigned)row[(s>>10)&3u]+(unsigned)row[(s>>12)&3u];\n"
     "    h^=h>>7; }\n"
     "  return ("+t+")h; }\n",
     {0x24u}, "PtrTableIdx", 2},

    // const char *p = c ? A[i] : B[j]; walk p (pointer SELECT of two table loads).
    {p+"_selptr",
     "static const char *const A[4]={\"one\",\"two\",\"three\",\"four\"};\n"
     "static const char *const B[4]={\"ALPHA\",\"BE\",\"GAMMA\",\"DT\"};\n"
     +t+" "+p+"_selptr("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    const char *w=((s>>4)&1u)?A[(s>>7)&3u]:B[(s>>11)&3u];\n"
     "    for(int k=0;w[k];k++) h=h*131u+(unsigned)(unsigned char)w[k];\n"
     "    h^=h>>12; }\n"
     "  return ("+t+")h; }\n",
     {0x25u}, "PtrTableIdx", 2},

    // Reverse / negative-stride walk over the pointer table.
    {p+"_revptr",
     "static const char *const V[6]={\"sigma\",\"tau\",\"upsilon\",\"phi\","
     "\"chi\",\"psi\"};\n"
     +t+" "+p+"_revptr("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int i=5;i>=0;i--){ s=s*1103515245u+12345u;\n"
     "    const char *w=V[i];\n"
     "    for(int k=0;w[k];k++) h=h*131u+(unsigned)(unsigned char)w[k]+(unsigned)i;\n"
     "    h^=h>>10; }\n"
     "  return ("+t+")h; }\n",
     {0x26u}, "PtrTableIdx", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makePtrTblTC("x64pt", "long");
static const std::vector<RoundTripTC> kX86 = makePtrTblTC("x86pt", "int");
static const std::vector<RoundTripTC> kA64 = makePtrTblTC("a64pt", "long");
static const std::vector<RoundTripTC> kARM = makePtrTblTC("armpt", "int");

INSTANTIATE_TEST_SUITE_P(PtrTableIdx, X64PtrTblRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(PtrTableIdx, X86PtrTblRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(PtrTableIdx, A64PtrTblRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(PtrTableIdx, ARM32PtrTblRT, ::testing::ValuesIn(kARM), rtTCName);
