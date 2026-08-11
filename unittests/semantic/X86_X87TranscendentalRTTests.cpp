//===- X86_X87TranscendentalRTTests.cpp - x87 transcendental ops -*- C++ -*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// x87 transcendental / special instructions (FSIN, FCOS, FSINCOS, FPTAN,
// FPATAN, FYL2X, FYL2XP1, FPREM, FPREM1, F2XM1, FSCALE, FXTRACT) were all
// lifted to a single value-less `X87Op` placeholder -> the emitter returned a
// safe 0, so every one of them was a SILENT NO-OP (the optimizer then folded
// the result to a constant).  They are native Unicorn (QEMU x87) instructions,
// so this is purely a NeverD lift gap (same family as #303/#377/#381).
//
// The fix keeps each as the GENUINE instruction via inline asm (per the
// "keep binary as binary, don't lift to library functions" directive): the lifter emits a
// per-op intrinsic with the correct x87-stack effect and the backend lowers it
// to `fsin`/`fscale`/... so the recompiled object runs the same hardware op as
// the original and roundtrips bit-for-bit through Unicorn.
//
// Each probe drives one instruction through inline asm, folds the 64-bit double
// result into the 32-bit return so it works on both x64 and i386, and the
// fixture compares original-Unicorn vs recompiled-Unicorn.  Before the fix the
// recompiled run returned 0 (placeholder) and diverged.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64X87TransRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64X87TransRT, Verify) { roundTripX64(GetParam()); }

class X86X87TransRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86X87TransRT, Verify) { roundTripX86(GetParam()); }

// Each kernel folds the double result bits to an int so the return fits EAX too.
#define X87_FOLD                                                                \
  "  unsigned long long _b; __builtin_memcpy(&_b,&r,8);\n"                      \
  "  return (int)(_b ^ (_b>>32));\n"

// clang-format off
static std::vector<RoundTripTC> makeX87TC() {
  return {
    // --- 1-operand transcendentals: st0 = f(st0) ---
    {"x87_fsin",
     "int x87_fsin(int a){ double x=(double)a*1e-4, r;\n"
     "  __asm__(\"fsin\":\"=&t\"(r):\"0\"(x):\"cc\");\n" X87_FOLD "}\n",
     {31416}, "X87Trans", 1, ""},
    {"x87_fcos",
     "int x87_fcos(int a){ double x=(double)a*1e-4, r;\n"
     "  __asm__(\"fcos\":\"=&t\"(r):\"0\"(x):\"cc\");\n" X87_FOLD "}\n",
     {31416}, "X87Trans", 1, ""},
    // F2XM1 domain is [-1, 1].
    {"x87_f2xm1",
     "int x87_f2xm1(int a){ double x=(double)(a%1000)*1e-3, r;\n"
     "  __asm__(\"f2xm1\":\"=&t\"(r):\"0\"(x):\"cc\");\n" X87_FOLD "}\n",
     {617}, "X87Trans", 1, ""},

    // --- 2-operand, no pop: st0 = f(st0, st1) ---
    // FSCALE: st0 * 2^trunc(st1).
    {"x87_fscale",
     "int x87_fscale(int a){ double x=(double)a*0.5, n=3.0, r;\n"
     "  __asm__(\"fscale\":\"=&t\"(r):\"0\"(x),\"u\"(n):\"cc\");\n" X87_FOLD "}\n",
     {7}, "X87Trans", 1, ""},
    // FPREM / FPREM1: partial remainder of st0 / st1.
    {"x87_fprem",
     "int x87_fprem(int a){ double x=(double)a, y=7.0, r;\n"
     "  __asm__(\"fprem\":\"=&t\"(r):\"0\"(x),\"u\"(y):\"cc\");\n" X87_FOLD "}\n",
     {100}, "X87Trans", 1, ""},
    {"x87_fprem1",
     "int x87_fprem1(int a){ double x=(double)a, y=7.0, r;\n"
     "  __asm__(\"fprem1\":\"=&t\"(r):\"0\"(x),\"u\"(y):\"cc\");\n" X87_FOLD "}\n",
     {100}, "X87Trans", 1, ""},

    // --- 2-operand, pop: result in st1, st0 popped ---
    // FPATAN: atan2(st1, st0).
    {"x87_fpatan",
     "int x87_fpatan(int a){ double x=(double)a*0.01, y=2.0, r;\n"
     "  __asm__(\"fpatan\":\"=&t\"(r):\"0\"(x),\"u\"(y):\"cc\");\n" X87_FOLD "}\n",
     {150}, "X87Trans", 1, ""},
    // FYL2X: st1 * log2(st0); st0 must be > 0.
    {"x87_fyl2x",
     "int x87_fyl2x(int a){ double x=(double)(a&0x7fff)+1.0, y=3.0, r;\n"
     "  __asm__(\"fyl2x\":\"=&t\"(r):\"0\"(x),\"u\"(y):\"cc\");\n" X87_FOLD "}\n",
     {1023}, "X87Trans", 1, ""},
    // FYL2XP1: st1 * log2(st0 + 1); |st0| < 1 - sqrt(2)/2.
    {"x87_fyl2xp1",
     "int x87_fyl2xp1(int a){ double x=(double)(a%100)*0.001, y=3.0, r;\n"
     "  __asm__(\"fyl2xp1\":\"=&t\"(r):\"0\"(x),\"u\"(y):\"cc\");\n" X87_FOLD "}\n",
     {50}, "X87Trans", 1, ""},

    // --- push variants (balanced by popping the extra result) ---
    // FSINCOS pushes cos; pop it to keep sin.
    {"x87_fsincos",
     "int x87_fsincos(int a){ double x=(double)a*1e-4, r;\n"
     "  __asm__(\"fsincos\\n\\tfstp %%st(0)\":\"=&t\"(r):\"0\"(x):\"cc\");\n"
     X87_FOLD "}\n",
     {31416}, "X87Trans", 1, ""},
    // FPTAN pushes 1.0; pop it to keep tan.
    {"x87_fptan",
     "int x87_fptan(int a){ double x=(double)a*1e-4, r;\n"
     "  __asm__(\"fptan\\n\\tfstp %%st(0)\":\"=&t\"(r):\"0\"(x):\"cc\");\n"
     X87_FOLD "}\n",
     {7853}, "X87Trans", 1, ""},
    // FXTRACT pushes the significand; pop it to keep the exponent.
    {"x87_fxtract_exp",
     "int x87_fxtract_exp(int a){ double x=(double)a+1.0, r;\n"
     "  __asm__(\"fxtract\\n\\tfstp %%st(0)\":\"=&t\"(r):\"0\"(x):\"cc\");\n"
     X87_FOLD "}\n",
     {1000}, "X87Trans", 1, ""},
    // FXTRACT keeping the significand (pop the exponent).
    {"x87_fxtract_sig",
     "int x87_fxtract_sig(int a){ double x=(double)a+1.0, r;\n"
     "  __asm__(\"fxtract\\n\\tfstp %%st(1)\":\"=&t\"(r):\"0\"(x):\"cc\");\n"
     X87_FOLD "}\n",
     {1000}, "X87Trans", 1, ""},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX87 = makeX87TC();

INSTANTIATE_TEST_SUITE_P(X87Trans, X64X87TransRT, ::testing::ValuesIn(kX87),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(X87Trans, X86X87TransRT, ::testing::ValuesIn(kX87),
                         rtTCName);

class X64X87FpremIterativeRT
    : public SemanticRoundTripFixture,
      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64X87FpremIterativeRT, Verify) { roundTripX64(GetParam()); }

static const std::vector<RoundTripTC> kX64X87FpremIterative = {
    {"x87_fprem_iterative",
     "long x87_fprem_iterative(long unused) {\n"
     "  (void)unused;\n"
     "  double x = 0x1.0000000000001p+1023;\n"
     "  double y = 3.0, r;\n"
     "  unsigned short sw;\n"
     "  __asm__ volatile (\n"
     "    \"fldl %[y]\\n\\t\"\n"
     "    \"fldl %[x]\\n\\t\"\n"
     "    \"1: fprem\\n\\t\"\n"
     "    \"fnstsw %%ax\\n\\t\"\n"
     "    \"testb $4, %%ah\\n\\t\"\n"
     "    \"jnz 1b\\n\\t\"\n"
     "    \"fstpl %[r]\\n\\t\"\n"
     "    \"fstp %%st(0)\"\n"
     "    : [r] \"=m\"(r), [sw] \"=a\"(sw)\n"
     "    : [x] \"m\"(x), [y] \"m\"(y)\n"
     "    : \"cc\", \"st\", \"st(1)\");\n"
     "  unsigned long long bits;\n"
     "  __builtin_memcpy(&bits, &r, 8);\n"
     "  return (long)(bits ^ ((unsigned long long)(sw & 0x4700) << 32));\n"
     "}\n",
     {0}, "X87FpremIterative"},
};

INSTANTIATE_TEST_SUITE_P(X87FpremIterative, X64X87FpremIterativeRT,
                         ::testing::ValuesIn(kX64X87FpremIterative), rtTCName);
