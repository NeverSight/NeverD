//===- X86_X87CompareRTTests.cpp - x87 FNSTSW/SAHF compare idiom -*- C++ -*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// x87 compares feed their result back through the FPU status word, not EFLAGS:
// `fcom/fucom/ficom/ftst; fnstsw %ax; sahf; setcc` maps C0->CF, C2->PF, C3->ZF.
// clang uses this idiom on every x86 CPU without FCOMI (i486 and the i386
// baseline), so it is the common path for float compares there.  But the FCOM
// family only wrote EFLAGS directly and never populated FPU_SW, so the following
// FNSTSW read a stale status word and SAHF loaded garbage.  FCOMI/FUCOMI (which
// set EFLAGS) were fine, hiding the gap.
//
// x87 is x86-family only (x86-64 + i386).  Each kernel loops over many operand
// pairs so it covers <, ==, > and folds the relations into the int return.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64X87CmpRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64X87CmpRT, Verify) { roundTripX64(GetParam()); }

class X86X87CmpRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86X87CmpRT, Verify) { roundTripX86(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeCmp(const char *prefix) {
  std::string p = prefix;
  return {
    // fucompp + fnstsw + sahf: C0->CF (x<y), C3->ZF (x==y).
    {p+"_fcmp_fnstsw",
     "int "+p+"_fcmp_fnstsw(int a){ int acc=0;\n"
     "  for(int i=0;i<16;i++){\n"
     "    double x=(double)((int)((a+i)&15)-8);\n"
     "    double y=(double)((int)(((a>>(i&7))+i)&15)-8);\n"
     "    unsigned char lt=0,eq=0;\n"
     "    __asm__ volatile(\"fldl %3\\n\\tfldl %2\\n\\tfucompp\\n\\t\"\n"
     "      \"fnstsw %%ax\\n\\tsahf\\n\\tsetb %0\\n\\tsete %1\"\n"
     "      :\"=&q\"(lt),\"=&q\"(eq):\"m\"(x),\"m\"(y):\"ax\",\"cc\");\n"
     "    acc=acc*3+(int)lt+2*(int)eq; }\n"
     "  return acc; }\n",
     {0x6BULL}, "X87Cmp", 0, "-mno-sse -mfpmath=387"},

    // ftst + fnstsw + sahf: sign/zero test of st0 against 0.0.
    {p+"_ftst_fnstsw",
     "int "+p+"_ftst_fnstsw(int a){ int acc=0;\n"
     "  for(int i=0;i<21;i++){\n"
     "    double x=(double)((int)((a+i)%21)-10);\n"
     "    unsigned char neg=0,zero=0;\n"
     "    __asm__ volatile(\"fldl %2\\n\\tftst\\n\\tfnstsw %%ax\\n\\tsahf\\n\\t\"\n"
     "      \"setb %0\\n\\tsete %1\\n\\tfstp %%st(0)\"\n"
     "      :\"=&q\"(neg),\"=&q\"(zero):\"m\"(x):\"ax\",\"cc\");\n"
     "    acc=acc*3+(int)neg+2*(int)zero; }\n"
     "  return acc; }\n",
     {0x35ULL}, "X87Cmp", 0, "-mno-sse -mfpmath=387"},

    // ficoml (compare st0 to int32 memory) + fnstsw + sahf.
    {p+"_ficom_fnstsw",
     "int "+p+"_ficom_fnstsw(int a){ int acc=0;\n"
     "  for(int i=0;i<16;i++){\n"
     "    double x=(double)((int)((a+i)&31)-16);\n"
     "    int y=((int)((a>>(i&7))&31))-16;\n"
     "    unsigned char lt=0,eq=0;\n"
     "    __asm__ volatile(\"fldl %2\\n\\tficoml %3\\n\\tfnstsw %%ax\\n\\tsahf\\n\\t\"\n"
     "      \"setb %0\\n\\tsete %1\\n\\tfstp %%st(0)\"\n"
     "      :\"=&q\"(lt),\"=&q\"(eq):\"m\"(x),\"m\"(y):\"ax\",\"cc\");\n"
     "    acc=acc*3+(int)lt+2*(int)eq; }\n"
     "  return acc; }\n",
     {0x4EULL}, "X87Cmp", 0, "-mno-sse -mfpmath=387"},
  };
}

static const std::vector<RoundTripTC> kX64Cmp = makeCmp("x64");
static const std::vector<RoundTripTC> kX86Cmp = makeCmp("x86");
// clang-format on

INSTANTIATE_TEST_SUITE_P(X87Cmp, X64X87CmpRT, ::testing::ValuesIn(kX64Cmp),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(X87Cmp, X86X87CmpRT, ::testing::ValuesIn(kX86Cmp),
                         rtTCName);
