//===- X64_FcomiRTTests.cpp - x87 FUCOMI/FCOMI flag roundtrip -----*- C++ -*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// x87 `FCOMI`/`FUCOMI`(+ popping `FCOMIP`/`FUCOMIP`) compare ST(0) with another
// x87 register and set EFLAGS DIRECTLY:
//   ST0 > src : ZF=0 PF=0 CF=0
//   ST0 < src : ZF=0 PF=0 CF=1
//   ST0 = src : ZF=1 PF=0 CF=0
//   unordered : ZF=1 PF=1 CF=1   (either operand NaN)
//
// clang reaches these only with the x87 FP stack (here forced via `-mno-sse`);
// doubles are passed as raw bit patterns through the integer arg registers and
// reinterpreted with memcpy so the harness can feed exact values (incl. quiet
// NaN) without an x87 FP ABI.  The probes cover </>/= plus NaN in either / both
// positions (the unordered ZF=PF=CF=1 arm).  Roundtrip compares native vs lifted
// return values, so any FUCOMI flag-modelling divergence surfaces as a mismatch.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64FcomiRT : public SemanticRoundTripFixture,
                   public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64FcomiRT, Verify) { roundTripX64(GetParam()); }

#define FCOMI_FN \
  "long f(long a,long b){\n" \
  "  double x,y; __builtin_memcpy(&x,&a,8); __builtin_memcpy(&y,&b,8);\n" \
  "  int r1=(x<y )?7:11;\n" \
  "  int r2=(x==y)?13:17;\n" \
  "  int r3=(x>y )?19:23;\n" \
  "  int r4=(x>=y)?29:31;\n" \
  "  return (long)(r1*1000000+r2*10000+r3*100+r4);\n" \
  "}\n"

#define D_1   0x3FF0000000000000ULL   //  1.0
#define D_2   0x4000000000000000ULL   //  2.0
#define D_NAN 0x7FF8000000000000ULL   //  quiet NaN
#define D_M1  0xBFF0000000000000ULL   // -1.0

// clang-format off
static const std::vector<RoundTripTC> kX64 = {
  {"fucomi_lt",   FCOMI_FN, {D_1, D_2 }, "Fcomi", 1, "-mno-sse"},
  {"fucomi_gt",   FCOMI_FN, {D_2, D_1 }, "Fcomi", 1, "-mno-sse"},
  {"fucomi_eq",   FCOMI_FN, {D_1, D_1 }, "Fcomi", 1, "-mno-sse"},
  {"fucomi_neg",  FCOMI_FN, {D_M1, D_1}, "Fcomi", 1, "-mno-sse"},
  // ===== NaN operands: the unordered ZF=PF=CF=1 arm. =====
  {"fucomi_nan_x",   FCOMI_FN, {D_NAN, D_1  }, "Fcomi", 1, "-mno-sse"},
  {"fucomi_nan_y",   FCOMI_FN, {D_1,   D_NAN}, "Fcomi", 1, "-mno-sse"},
  {"fucomi_nan_both",FCOMI_FN, {D_NAN, D_NAN}, "Fcomi", 1, "-mno-sse"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(Fcomi, X64FcomiRT, ::testing::ValuesIn(kX64), rtTCName);
