//===- X86_X87FlagFoldRTTests.cpp - FP compare flag-fold robustness *- C++ -*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Stress the MedFlags optimizer's flag-folding against the #381b class of bug:
// an integer op writes ZF (INT_EQUAL), then an x87 FP compare overwrites ZF
// (BOOL_OR over FLOAT_EQUAL), then a SETcc reads ZF.  The fold must attribute
// the read to the *nearest* (FP) writer, not walk back to the integer compare.
// #381b fixed the SELECT (FCMOVE/CMOVE) path; these guard the SETcc (Pass 2)
// and COND_BR (Pass 1) paths under the same "integer ZF then FP ZF then read"
// pattern, on both the x87 (FUCOMI) and SSE (UCOMISD) compares.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64X87FoldRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64X87FoldRT, Verify) { roundTripX64(GetParam()); }

class X86X87FoldRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86X87FoldRT, Verify) { roundTripX86(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeFold(const char *prefix) {
  std::string p = prefix;
  return {
    // integer `and` (ZF) -> fucomi (ZF) -> sete: the byte must reflect x==y,
    // not the integer AND result.
    {p+"_int_fucomi_sete",
     "int "+p+"_int_fucomi_sete(int a){ int acc=0;\n"
     "  for(int i=0;i<16;i++){\n"
     "    double x=(double)((int)((a+i)&7));\n"
     "    double y=(double)((int)((a+i*2)&7));\n"
     "    int g=(a+i*5)&15; unsigned char e;\n"
     "    __asm__ volatile(\"andl $7,%1\\n\\tfldl %3\\n\\tfldl %2\\n\\t\"\n"
     "      \"fucomi %%st(1),%%st\\n\\tsete %0\\n\\tfstp %%st(0)\\n\\tfstp %%st(0)\"\n"
     "      :\"=q\"(e),\"+r\"(g):\"m\"(x),\"m\"(y):\"st\",\"st(1)\",\"cc\");\n"
     "    acc=acc*131+(int)e+(g&1); }\n"
     "  return acc; }\n",
     {0x3FULL}, "X87Fold", 0, "-mno-sse -mfpmath=387"},

    // Same shape with setne (NE branch of the same fold guard).
    {p+"_int_fucomi_setne",
     "int "+p+"_int_fucomi_setne(int a){ int acc=0;\n"
     "  for(int i=0;i<16;i++){\n"
     "    double x=(double)((int)((a+i)&7));\n"
     "    double y=(double)((int)((a+i*3)&7));\n"
     "    int g=(a+i*7)&15; unsigned char e;\n"
     "    __asm__ volatile(\"andl $7,%1\\n\\tfldl %3\\n\\tfldl %2\\n\\t\"\n"
     "      \"fucomi %%st(1),%%st\\n\\tsetne %0\\n\\tfstp %%st(0)\\n\\tfstp %%st(0)\"\n"
     "      :\"=q\"(e),\"+r\"(g):\"m\"(x),\"m\"(y):\"st\",\"st(1)\",\"cc\");\n"
     "    acc=acc*131+(int)e+(g&1); }\n"
     "  return acc; }\n",
     {0x29ULL}, "X87Fold", 0, "-mno-sse -mfpmath=387"},

    // integer `and` (ZF) -> fucomi (ZF) -> jne: COND_BR (Pass 1) must read the
    // FP ZF, not fold to the integer compare.
    {p+"_int_fucomi_jne",
     "int "+p+"_int_fucomi_jne(int a){ int acc=0;\n"
     "  for(int i=0;i<16;i++){\n"
     "    double x=(double)((int)((a+i)&7));\n"
     "    double y=(double)((int)((a+i*2)&7));\n"
     "    int g=(a+i*5)&15, r;\n"
     "    __asm__ volatile(\"andl $7,%1\\n\\tfldl %3\\n\\tfldl %2\\n\\t\"\n"
     "      \"fucomi %%st(1),%%st\\n\\tfstp %%st(0)\\n\\tfstp %%st(0)\\n\\t\"\n"
     "      \"movl $111,%0\\n\\tjne 1f\\n\\tmovl $222,%0\\n\\t1:\\n\\t\"\n"
     "      :\"=&r\"(r),\"+r\"(g):\"m\"(x),\"m\"(y):\"st\",\"st(1)\",\"cc\");\n"
     "    acc=acc*131+r+(g&1); }\n"
     "  return acc; }\n",
     {0x4BULL}, "X87Fold", 0, "-mno-sse -mfpmath=387"},

    // Same fold guard on the common SSE path: integer `and` (ZF) -> ucomisd
    // (ZF via the same FLOAT_EQUAL/BOOL_OR) -> setne / jne.  No -mno-sse here.
    {p+"_int_ucomisd_setne",
     "int "+p+"_int_ucomisd_setne(int a){ int acc=0;\n"
     "  for(int i=0;i<16;i++){\n"
     "    double x=(double)((int)((a+i)&7));\n"
     "    double y=(double)((int)((a+i*3)&7));\n"
     "    int g=(a+i*7)&15; unsigned char e;\n"
     "    __asm__ volatile(\"andl $7,%1\\n\\tucomisd %3,%2\\n\\tsetne %0\"\n"
     "      :\"=q\"(e),\"+r\"(g):\"x\"(x),\"x\"(y):\"cc\");\n"
     "    acc=acc*131+(int)e+(g&1); }\n"
     "  return acc; }\n",
     {0x63ULL}, "X87Fold", 0, ""},

    {p+"_int_ucomisd_jne",
     "int "+p+"_int_ucomisd_jne(int a){ int acc=0;\n"
     "  for(int i=0;i<16;i++){\n"
     "    double x=(double)((int)((a+i)&7));\n"
     "    double y=(double)((int)((a+i*2)&7));\n"
     "    int g=(a+i*5)&15, r;\n"
     "    __asm__ volatile(\"andl $1,%1\\n\\tucomisd %3,%2\\n\\t\"\n"
     "      \"movl $111,%0\\n\\tjne 1f\\n\\tmovl $222,%0\\n\\t1:\\n\\t\"\n"
     "      :\"=&r\"(r),\"+r\"(g):\"x\"(x),\"x\"(y):\"cc\");\n"
     "    acc=acc*131+r+(g&1); }\n"
     "  return acc; }\n",
     {0x71ULL}, "X87Fold", 0, ""},
  };
}

static const std::vector<RoundTripTC> kX64Fold = makeFold("x64");
static const std::vector<RoundTripTC> kX86Fold = makeFold("x86");
// clang-format on

INSTANTIATE_TEST_SUITE_P(X87Fold, X64X87FoldRT, ::testing::ValuesIn(kX64Fold),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(X87Fold, X86X87FoldRT, ::testing::ValuesIn(kX86Fold),
                         rtTCName);
