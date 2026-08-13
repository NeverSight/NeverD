//===- X86_X87StackArithRTTests.cpp - x87 deep-stack register arith -*- C++ =//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// The register/stack forms of FADD/FSUB/FMUL/FDIV (and the FSUBR/FDIVR reverse
// + the P pop variants) hardcoded st0/st1 and ignored capstone's operand
// register indices.  Any x87 expression that keeps more than two values on the
// stack -- which clang at -O2 -mfpmath=387 routinely does -- emits forms like
// `fmul st(2),st`, `fadd st(4),st`, `faddp st(3),st`, `fdivp st(4),st`; those
// were all computed against the wrong register.
//
// To stay bit-exact despite the 80-bit-vs-64-bit intermediate-precision limit,
// the kernels use small integer-valued doubles and only exact divisions, so
// every intermediate is representable in both precisions and QEMU == NeverD.
// x87 is x86-family only -> x86-64 + i386.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64X87StkRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64X87StkRT, Verify) { roundTripX64(GetParam()); }

class X86X87StkRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86X87StkRT, Verify) { roundTripX86(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeStk(const char *prefix) {
  std::string p = prefix;
  return {
    // Deep mul/add/sub stack: fmul st(2),st / fmulp st(4),st / fsubp st(2),st.
    {p+"_arith",
     "int "+p+"_arith(int s){\n"
     "  double a=(double)((s&7)+1), b=(double)(((s>>3)&7)+1);\n"
     "  double c=(double)(((s>>6)&7)+1), d=(double)(((s>>9)&3)+1);\n"
     "  double r = a*b + c*d - a*c + b*d - a*d;\n"
     "  double t = (a+b)*(c+d) - (a-b)*(c-d);\n"
     "  double u = r*t + a*b*c - d*a;\n"
     "  unsigned long long w; __builtin_memcpy(&w,&u,8);\n"
     "  return (int)(w ^ (w>>32)); }\n",
     {0x5A3ULL}, "X87Stk", 2, "-mno-sse -mfpmath=387"},

    // Deep divide stack: fdiv st(4),st / fdiv st(6),st / fdivp st(4),st.  All
    // divisions are exact (p is a multiple of each factor / their products).
    {p+"_div",
     "int "+p+"_div(int s){\n"
     "  double a=(double)((s&3)+1), b=(double)(((s>>2)&3)+1);\n"
     "  double c=(double)(((s>>4)&3)+1), d=(double)(((s>>6)&3)+1);\n"
     "  double p=a*b*c*d;\n"
     "  double r=(p/a + p/b) - (p/c + p/d) + p/(a*b);\n"
     "  double t=r + p/a/b/c/d;\n"
     "  unsigned long long w; __builtin_memcpy(&w,&t,8);\n"
     "  return (int)(w ^ (w>>32)); }\n",
     {0x39CULL}, "X87Stk", 2, "-mno-sse -mfpmath=387"},

    // Reverse forms via subtraction/division chains that clang lowers to
    // fsubrp/fdivrp with non-st1 indices.
    {p+"_rev",
     "int "+p+"_rev(int s){\n"
     "  double a=(double)((s&3)+2), b=(double)(((s>>2)&3)+2);\n"
     "  double c=(double)(((s>>4)&3)+2), d=(double)(((s>>6)&3)+2);\n"
     "  double q=a*b*c*d;\n"
     "  double r = (a - b*c) + (d - a*b) - (c - a*d);\n"
     "  double t = q/(a*b) - q/(c*d) + r;\n"
     "  unsigned long long w; __builtin_memcpy(&w,&t,8);\n"
     "  return (int)(w ^ (w>>32)); }\n",
     {0x2D7ULL}, "X87Stk", 2, "-mno-sse -mfpmath=387"},
  };
}

static const std::vector<RoundTripTC> kX64Stk = makeStk("x64");
static const std::vector<RoundTripTC> kX86Stk = makeStk("x86");
// clang-format on

INSTANTIATE_TEST_SUITE_P(X87Stk, X64X87StkRT, ::testing::ValuesIn(kX64Stk),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(X87Stk, X86X87StkRT, ::testing::ValuesIn(kX86Stk),
                         rtTCName);
