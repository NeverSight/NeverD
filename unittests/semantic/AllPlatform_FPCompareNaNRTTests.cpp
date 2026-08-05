//===- AllPlatform_FPCompareNaNRTTests.cpp - FP compare with NaN -*- C++ -*-===//
//
// Roundtrip probes for floating-point comparisons fed a NaN operand.  Existing
// FP-compare tests only use finite values, so the IEEE "unordered" result was
// never exercised: every ordered relation (<, >, <=, >=, ==) must be FALSE when
// either operand is NaN, and != must be TRUE.
//
// On x86 this maps to UCOMISD/COMISD, whose unordered result sets ZF=PF=CF=1;
// modelling ZF as plain FLOAT_EQUAL and CF as plain FLOAT_LESS (both false on
// NaN) makes SETA/SETAE (the ordered </<=/>/>= idioms) wrongly return true.
// AArch64 FCMP and ARM32 VCMP set their carry/overflow flags such that the
// unordered case already comes out right; the probes here guard that too.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64FPCmpNaNRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64FPCmpNaNRT, Verify) { roundTripX64(GetParam()); }

class A64FPCmpNaNRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64FPCmpNaNRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32FPCmpNaNRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32FPCmpNaNRT, Verify) { roundTripARM32(GetParam()); }

// Bit patterns:
//   qNaN  (double) = 0x7FF8000000000000   2.0 (double) = 0x4000000000000000
//   qNaN  (float)  = 0x7FC00000           2.0 (float)  = 0x40000000
#define DNAN "0x7FF8000000000000ULL"
#define D2   "0x4000000000000000ULL"
#define FNAN 0x7FC00000ULL
#define F2   0x40000000ULL

// clang-format off

// One reusable double-compare body; OP is the C relational operator.
#define DCMP_BODY(OP) \
  "long f(long a,long b){double x,y;__builtin_memcpy(&x,&a,8);" \
  "__builtin_memcpy(&y,&b,8);return (x " OP " y)?111:222;}\n"

// Float compare: low 32 bits of each arg carry the float pattern.
#define FCMP_BODY(OP) \
  "long f(long a,long b){int ai=(int)a,bi=(int)b;float x,y;" \
  "__builtin_memcpy(&x,&ai,4);__builtin_memcpy(&y,&bi,4);" \
  "return (x " OP " y)?111:222;}\n"

static const std::vector<RoundTripTC> kX64 = {
  // double, NaN as first operand (every ordered relation must be false -> 222).
  {"x64_d_olt_nan", DCMP_BODY("<"),  {0x7FF8000000000000ULL, 0x4000000000000000ULL}, "FPCmpNaN", 1},
  {"x64_d_ogt_nan", DCMP_BODY(">"),  {0x7FF8000000000000ULL, 0x4000000000000000ULL}, "FPCmpNaN", 1},
  {"x64_d_ole_nan", DCMP_BODY("<="), {0x7FF8000000000000ULL, 0x4000000000000000ULL}, "FPCmpNaN", 1},
  {"x64_d_oge_nan", DCMP_BODY(">="), {0x7FF8000000000000ULL, 0x4000000000000000ULL}, "FPCmpNaN", 1},
  {"x64_d_oeq_nan", DCMP_BODY("=="), {0x7FF8000000000000ULL, 0x4000000000000000ULL}, "FPCmpNaN", 1},
  {"x64_d_one_nan", DCMP_BODY("!="), {0x7FF8000000000000ULL, 0x4000000000000000ULL}, "FPCmpNaN", 1},
  // NaN as second operand.
  {"x64_d_olt_nan2", DCMP_BODY("<"),  {0x4000000000000000ULL, 0x7FF8000000000000ULL}, "FPCmpNaN", 1},
  {"x64_d_oge_nan2", DCMP_BODY(">="), {0x4000000000000000ULL, 0x7FF8000000000000ULL}, "FPCmpNaN", 1},
  // float (UCOMISS).
  {"x64_f_olt_nan", FCMP_BODY("<"),  {FNAN, F2}, "FPCmpNaN", 1},
  {"x64_f_ogt_nan", FCMP_BODY(">"),  {FNAN, F2}, "FPCmpNaN", 1},
  {"x64_f_ole_nan", FCMP_BODY("<="), {FNAN, F2}, "FPCmpNaN", 1},
  {"x64_f_one_nan", FCMP_BODY("!="), {FNAN, F2}, "FPCmpNaN", 1},
  // Finite controls (must stay correct).
  {"x64_d_olt_fin", DCMP_BODY("<"),  {0x4000000000000000ULL, 0x4008000000000000ULL}, "FPCmpNaN", 1},
  {"x64_d_oge_fin", DCMP_BODY(">="), {0x4008000000000000ULL, 0x4000000000000000ULL}, "FPCmpNaN", 1},
  // long double (x87 FUCOMPI) — 0/0 builds an opaque NaN clang cannot fold.
  {"x64_ld_olt_nan",
   "long f(long a,long b){long double da=(long double)a,db=(long double)b;"
   "long double x=da/db;return (x<db)?111:222;}\n",
   {0, 0}, "FPCmpNaN", 1},
  {"x64_ld_oge_nan",
   "long f(long a,long b){long double da=(long double)a,db=(long double)b;"
   "long double x=da/db;return (x>=db)?111:222;}\n",
   {0, 0}, "FPCmpNaN", 1},
  {"x64_ld_olt_fin",
   "long f(long a,long b){long double da=(long double)a,db=(long double)b;"
   "long double x=da/db;return (x<db)?111:222;}\n",
   {6, 2}, "FPCmpNaN", 1},  // 3.0 < 2.0 -> false -> 222
};

static const std::vector<RoundTripTC> kA64 = {
  {"a64_d_olt_nan", DCMP_BODY("<"),  {0x7FF8000000000000ULL, 0x4000000000000000ULL}, "FPCmpNaN", 1},
  {"a64_d_ogt_nan", DCMP_BODY(">"),  {0x7FF8000000000000ULL, 0x4000000000000000ULL}, "FPCmpNaN", 1},
  {"a64_d_ole_nan", DCMP_BODY("<="), {0x7FF8000000000000ULL, 0x4000000000000000ULL}, "FPCmpNaN", 1},
  {"a64_d_oge_nan", DCMP_BODY(">="), {0x7FF8000000000000ULL, 0x4000000000000000ULL}, "FPCmpNaN", 1},
  {"a64_d_oeq_nan", DCMP_BODY("=="), {0x7FF8000000000000ULL, 0x4000000000000000ULL}, "FPCmpNaN", 1},
  {"a64_d_one_nan", DCMP_BODY("!="), {0x7FF8000000000000ULL, 0x4000000000000000ULL}, "FPCmpNaN", 1},
  {"a64_f_olt_nan", FCMP_BODY("<"),  {FNAN, F2}, "FPCmpNaN", 1},
  {"a64_f_oge_nan", FCMP_BODY(">="), {FNAN, F2}, "FPCmpNaN", 1},
  {"a64_d_olt_fin", DCMP_BODY("<"),  {0x4000000000000000ULL, 0x4008000000000000ULL}, "FPCmpNaN", 1},
};

static const std::vector<RoundTripTC> kArm32 = {
  {"arm_d_olt_nan", DCMP_BODY("<"),  {0x7FF8000000000000ULL, 0x4000000000000000ULL}, "FPCmpNaN", 1},
  {"arm_d_ogt_nan", DCMP_BODY(">"),  {0x7FF8000000000000ULL, 0x4000000000000000ULL}, "FPCmpNaN", 1},
  {"arm_d_ole_nan", DCMP_BODY("<="), {0x7FF8000000000000ULL, 0x4000000000000000ULL}, "FPCmpNaN", 1},
  {"arm_d_oge_nan", DCMP_BODY(">="), {0x7FF8000000000000ULL, 0x4000000000000000ULL}, "FPCmpNaN", 1},
  {"arm_d_oeq_nan", DCMP_BODY("=="), {0x7FF8000000000000ULL, 0x4000000000000000ULL}, "FPCmpNaN", 1},
  {"arm_d_one_nan", DCMP_BODY("!="), {0x7FF8000000000000ULL, 0x4000000000000000ULL}, "FPCmpNaN", 1},
  {"arm_f_olt_nan", FCMP_BODY("<"),  {FNAN, F2}, "FPCmpNaN", 1},
  {"arm_f_oge_nan", FCMP_BODY(">="), {FNAN, F2}, "FPCmpNaN", 1},
  {"arm_d_olt_fin", DCMP_BODY("<"),  {0x4000000000000000ULL, 0x4008000000000000ULL}, "FPCmpNaN", 1},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(FPCmpNaN, X64FPCmpNaNRT, ::testing::ValuesIn(kX64),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(FPCmpNaN, A64FPCmpNaNRT, ::testing::ValuesIn(kA64),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(FPCmpNaN, ARM32FPCmpNaNRT, ::testing::ValuesIn(kArm32),
                         rtTCName);
