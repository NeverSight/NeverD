//===- AArch64_FccmpRTTests.cpp - conditional FP compare roundtrip *- C++ -*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// AArch64 `FCCMP Sn,Sm,#nzcv,cond` / `FCCMPE`:
//   if cond:  NZCV = result of FP compare(Sn, Sm)   (incl. the UNORDERED case
//             N=0,Z=0,C=1,V=1 when either operand is NaN)
//   else:     NZCV = #nzcv  (the 4-bit immediate)
//
// clang emits this to flatten short-circuit FP conditionals
// (`a<b && c<d`, `a<=b || c>q`, ...).  These probes drive the conditional FP
// compare across: both-true, the imm-fallback arm (the leading compare fails so
// FCCMP forces NZCV = #imm), the equal case, and — most importantly — NaN
// operands in either position (the unordered NZCV that a naive `FLOAT_LESS`-only
// lift gets wrong).  Doubles are passed as raw bit patterns through the integer
// argument registers (the fixture seeds X0..X3) and reinterpreted with memcpy,
// so the harness can feed exact values (including quiet NaN) without an FP ABI.
//
// The roundtrip compares native vs lifted return values, so any divergence in
// the FCCMP flag modelling (wrong cond gating, dropped imm fallback, or wrong
// unordered flags) surfaces as a mismatch.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64FccmpRT : public SemanticRoundTripFixture,
                   public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64FccmpRT, Verify) { roundTripAArch64(GetParam()); }

// Shared function body: three short-circuit FP conditionals -> three FCCMPs.
#define FCCMP_FN \
  "long f(long a,long b,long c,long d){\n" \
  "  double x,y,p,q;\n" \
  "  __builtin_memcpy(&x,&a,8); __builtin_memcpy(&y,&b,8);\n" \
  "  __builtin_memcpy(&p,&c,8); __builtin_memcpy(&q,&d,8);\n" \
  "  int r1=(x<y  && p<q )?7:11;\n" \
  "  int r2=(x<=y || p>q )?13:17;\n" \
  "  int r3=(x>=y && p!=q)?19:23;\n" \
  "  return (long)(r1*10000+r2*100+r3);\n" \
  "}\n"

// IEEE-754 double bit patterns.
#define D_1   0x3FF0000000000000ULL   //  1.0
#define D_2   0x4000000000000000ULL   //  2.0
#define D_0p5 0x3FE0000000000000ULL   //  0.5
#define D_3   0x4008000000000000ULL   //  3.0
#define D_NAN 0x7FF8000000000000ULL   //  quiet NaN
#define D_M1  0xBFF0000000000000ULL   // -1.0

// clang-format off
static const std::vector<RoundTripTC> kA64 = {
  {"fccmp_lt_lt",  FCCMP_FN, {D_1, D_2, D_0p5, D_3 }, "Fccmp", 2},
  {"fccmp_lt_ge",  FCCMP_FN, {D_1, D_2, D_3,   D_0p5}, "Fccmp", 2},
  {"fccmp_ge_lead",FCCMP_FN, {D_2, D_1, D_0p5, D_3 }, "Fccmp", 2},
  {"fccmp_equal",  FCCMP_FN, {D_1, D_1, D_2,   D_2 }, "Fccmp", 2},
  {"fccmp_neg",    FCCMP_FN, {D_M1, D_1, D_0p5, D_3}, "Fccmp", 2},
  // ===== NaN operands: exercise the UNORDERED flag result inside FCCMP. =====
  {"fccmp_nan_q",  FCCMP_FN, {D_1, D_2, D_0p5, D_NAN}, "Fccmp", 2},
  {"fccmp_nan_p",  FCCMP_FN, {D_1, D_2, D_NAN, D_3 }, "Fccmp", 2},
  {"fccmp_nan_y",  FCCMP_FN, {D_1, D_NAN, D_0p5, D_3}, "Fccmp", 2},
  {"fccmp_nan_x",  FCCMP_FN, {D_NAN, D_2, D_0p5, D_3}, "Fccmp", 2},
  {"fccmp_nan_all",FCCMP_FN, {D_NAN, D_NAN, D_NAN, D_NAN}, "Fccmp", 2},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(Fccmp, A64FccmpRT, ::testing::ValuesIn(kA64), rtTCName);
