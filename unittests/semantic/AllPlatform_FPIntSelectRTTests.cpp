//===- AllPlatform_FPIntSelectRTTests.cpp - FP compare + int cmov *- C++ -*-=//
//
// clang -O2 kernels that put an INTEGER compare and a following FLOATING-POINT
// compare in the *same* straight-line block, each feeding a branchless select
// (cmov / csel).  The FP compare writes the same condition-flag bits (ZF/CF on
// x86 via ucomisd/fucomi's BOOL_OR; NZCV on ARM via fcmp) the int compare just
// wrote, so the MedFlags optimizer must attribute each select to its OWN nearest
// flag writer.  Folding the FP-driven select back to the earlier integer compare
// (whose CMP operands are still in scope) silently swaps the comparison and
// miscompiles -- the non-EQ (CF/ZF combo: below/above/<=/>=) FP conditions are
// the gap the EQ-only Pass-3 guard does not cover.
//
// doubles are integer-valued so every compare is exact and the result folds to
// an exact int.  x64 uses SSE ucomisd; i386 defaults to x87 fucomi; AArch64 uses
// fcmp+csel -- three distinct flag-writing FP-compare lowerings.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64FPISelRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64FPISelRT, Verify) { roundTripX64(GetParam()); }

class X86FPISelRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86FPISelRT, Verify) { roundTripX86(GetParam()); }

class A64FPISelRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64FPISelRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeFPISel(const char *prefix) {
  std::string p = prefix;
  return {
    // signed int `<` (cmovl) then FP `<` (cmovb reading ucomisd CF) in one block.
    {p+"_lt",
     "int "+p+"_lt(int a){ int acc=0;\n"
     "  for(int i=0;i<120;i++){\n"
     "    int p=a+i*3, q=a*i-7;\n"
     "    double x=(double)((a+i)&31), y=(double)((a^(i*5))&31);\n"
     "    int ic=(p<q)?5:9;\n"
     "    int fc=(x<y)?11:22;\n"
     "    acc += ic*100 + fc - i; }\n"
     "  return acc; }\n",
     {0x1234567ULL}, "FPISel", 2, ""},

    // int `==` (cmove ZF) then FP `==` (cmove reading FP ZF) in one block.
    {p+"_eq",
     "int "+p+"_eq(int a){ int acc=0;\n"
     "  for(int i=0;i<120;i++){\n"
     "    int p=a+i, q=(a^i)&255;\n"
     "    double x=(double)((a+i)&7), y=(double)((a-i)&7);\n"
     "    int ic=(p==q)?3:8;\n"
     "    int fc=(x==y)?40:70;\n"
     "    acc += ic + fc*2 - i; }\n"
     "  return acc; }\n",
     {0x2233445ULL}, "FPISel", 2, ""},

    // int `<=` then FP `<=`/`>` (cmovbe/cmova: CF|ZF combos) in one block.
    {p+"_le",
     "int "+p+"_le(int a){ int acc=0;\n"
     "  for(int i=0;i<120;i++){\n"
     "    int p=a*i, q=a+i*9;\n"
     "    double x=(double)((a*i+1)&63), y=(double)((a+i)&63);\n"
     "    int ic=(p<=q)?2:6;\n"
     "    int fc=(x<=y)?15:25;\n"
     "    int gc=(x>y)?1:0;\n"
     "    acc += ic + fc + gc*3 - i; }\n"
     "  return acc; }\n",
     {0x3344556ULL}, "FPISel", 2, ""},

    // unsigned int compare (cmovb CF) then FP `>=` (cmovae reading FP CF).
    {p+"_uge",
     "int "+p+"_uge(int a){ unsigned acc=0;\n"
     "  for(int i=0;i<120;i++){\n"
     "    unsigned p=(unsigned)(a+i)*2654435761u, q=(unsigned)(a*i+7)*40503u;\n"
     "    double x=(double)((a+i)&15), y=(double)((a*2-i)&15);\n"
     "    int ic=(p<q)?4:13;\n"
     "    int fc=(x>=y)?17:29;\n"
     "    acc += (unsigned)(ic*10 + fc) - (unsigned)i; }\n"
     "  return (int)acc; }\n",
     {0x4455667ULL}, "FPISel", 2, ""},

    // Nested: integer compare selects between two FP-compare selects (the FP
    // compares live in the taken arm right after the integer compare).
    {p+"_nest",
     "int "+p+"_nest(int a){ int acc=0;\n"
     "  for(int i=0;i<120;i++){\n"
     "    int p=(a+i)&1023, q=(a*3+i)&1023;\n"
     "    double x=(double)((a+i)&15), y=(double)((a*2)&15);\n"
     "    int t=(p<q)?((x<y)?7:3):((x>=y)?4:8);\n"
     "    acc += t - (i&7); }\n"
     "  return acc; }\n",
     {0x5566778ULL}, "FPISel", 2, ""},

    // float (not double) variants: ucomiss / flds-fucomi / fcmp s-regs.
    {p+"_flt",
     "int "+p+"_flt(int a){ int acc=0;\n"
     "  for(int i=0;i<120;i++){\n"
     "    int p=a-i, q=a*i+5;\n"
     "    float x=(float)((a+i)&31), y=(float)((a^i)&31);\n"
     "    int ic=(p>q)?6:1;\n"
     "    int fc=(x<y)?12:24;\n"
     "    acc += ic*50 + fc - i; }\n"
     "  return acc; }\n",
     {0x6677889ULL}, "FPISel", 2, ""},
  };
}

static const std::vector<RoundTripTC> kX64FPISel = makeFPISel("x64fpis");
static const std::vector<RoundTripTC> kX86FPISel = makeFPISel("x86fpis");
static const std::vector<RoundTripTC> kA64FPISel = makeFPISel("a64fpis");
// clang-format on

INSTANTIATE_TEST_SUITE_P(FPISel, X64FPISelRT, ::testing::ValuesIn(kX64FPISel),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(FPISel, X86FPISelRT, ::testing::ValuesIn(kX86FPISel),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(FPISel, A64FPISelRT, ::testing::ValuesIn(kA64FPISel),
                         rtTCName);
