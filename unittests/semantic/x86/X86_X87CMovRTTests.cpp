//===- X86_X87CMovRTTests.cpp - x87 FCMOVcc conditional move ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// x87 conditional moves (FCMOVB/E/BE/U and the N* inverses) move st(i) into
// st(0) when the EFLAGS condition (set by a preceding FUCOMI/FCOMI) holds.
// clang emits them for FP min/max/ternary in x87 mode.  They were all lumped
// into the generic `emitIntrinsic(X87Op)` placeholder, i.e. a no-op: the
// conditional move never happened, so st(0) kept its original value.
//
// x87 is x86-family only (x86-64 + i386).  Each kernel loops over operand pairs
// covering the taken / not-taken branch and folds the result into the int
// return.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64X87CMovRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64X87CMovRT, Verify) { roundTripX64(GetParam()); }

class X86X87CMovRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86X87CMovRT, Verify) { roundTripX86(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeCMov(const char *prefix) {
  std::string p = prefix;
  return {
    // fucomi + fcmovb (CF): st0 = (y<x) ? x : y  == max(x,y).
    {p+"_fcmovb_max",
     "int "+p+"_fcmovb_max(int a){ int acc=0;\n"
     "  for(int i=0;i<16;i++){\n"
     "    double x=(double)((int)((a+i)&15)-8);\n"
     "    double y=(double)((int)(((a*3)>>(i&7))&15)-8);\n"
     "    double r;\n"
     "    __asm__ volatile(\"fldl %1\\n\\tfldl %2\\n\\tfucomi %%st(1),%%st\\n\\t\"\n"
     "      \"fcmovb %%st(1),%%st\\n\\tfstpl %0\\n\\tfstp %%st(0)\"\n"
     "      :\"=m\"(r):\"m\"(x),\"m\"(y):\"st\",\"st(1)\",\"cc\");\n"
     "    acc=acc*131+(int)r; }\n"
     "  return acc; }\n",
     {0x71ULL}, "X87CMov", 0, "-mno-sse -mfpmath=387"},

    // fucomi + fcmovnb (!CF): st0 = (y>=x) ? x : y  == min(x,y).
    {p+"_fcmovnb_min",
     "int "+p+"_fcmovnb_min(int a){ int acc=0;\n"
     "  for(int i=0;i<16;i++){\n"
     "    double x=(double)((int)((a+i*5)&15)-8);\n"
     "    double y=(double)((int)(((a*7)>>(i&7))&15)-8);\n"
     "    double r;\n"
     "    __asm__ volatile(\"fldl %1\\n\\tfldl %2\\n\\tfucomi %%st(1),%%st\\n\\t\"\n"
     "      \"fcmovnb %%st(1),%%st\\n\\tfstpl %0\\n\\tfstp %%st(0)\"\n"
     "      :\"=m\"(r):\"m\"(x),\"m\"(y):\"st\",\"st(1)\",\"cc\");\n"
     "    acc=acc*131+(int)r; }\n"
     "  return acc; }\n",
     {0x53ULL}, "X87CMov", 0, "-mno-sse -mfpmath=387"},

    // fucomi + fcmove (ZF): when x==y move a distinct tag into st0, else keep x.
    // Stack: st0=x st1=y st2=tag; compare x,y then conditionally move tag.
    {p+"_fcmove_eq",
     "int "+p+"_fcmove_eq(int a){ int acc=0;\n"
     "  for(int i=0;i<16;i++){\n"
     "    double x=(double)((int)((a+i)&7));\n"
     "    double y=(double)((int)((a+i*2)&7));\n"   // x==y on some iterations
     "    double tag=(double)(i+100);\n"
     "    double r;\n"
     "    __asm__ volatile(\"fldl %3\\n\\tfldl %2\\n\\tfldl %1\\n\\t\"\n"
     "      \"fucomi %%st(1),%%st\\n\\tfcmove %%st(2),%%st\\n\\t\"\n"
     "      \"fstpl %0\\n\\tfstp %%st(0)\\n\\tfstp %%st(0)\"\n"
     "      :\"=m\"(r):\"m\"(x),\"m\"(y),\"m\"(tag):\"st\",\"st(1)\",\"st(2)\",\"cc\");\n"
     "    acc=acc*131+(int)r; }\n"
     "  return acc; }\n",
     {0x2CULL}, "X87CMov", 0, "-mno-sse -mfpmath=387"},
  };
}

static const std::vector<RoundTripTC> kX64CMov = makeCMov("x64");
static const std::vector<RoundTripTC> kX86CMov = makeCMov("x86");
// clang-format on

INSTANTIATE_TEST_SUITE_P(X87CMov, X64X87CMovRT, ::testing::ValuesIn(kX64CMov),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(X87CMov, X86X87CMovRT, ::testing::ValuesIn(kX86CMov),
                         rtTCName);
