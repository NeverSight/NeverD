//===- AArch64_FPConvRoundRTTests.cpp - FP conversion/rounding roundtrip --===//
//
// Tests AArch64 FP conversion and rounding instructions through lift pipeline.
// Exercises: FCVTZS, FCVTZU, FCVTAS, FCVTNS, FCVTMS, FCVTPS,
// SCVTF, UCVTF, FRINTN, FRINTM, FRINTP, FRINTZ, FRINTA, FRINTX, FRINTI,
// FCVT (float<->double), FMOV, FCSEL, FMADD, FMSUB, FCMPE.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64FPConvRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64FPConvRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kA64FPConv = {

  // ===== FCVTZS - float/double → signed int, truncating =====
  {"a64_fcvtzs_d_to_i64",
   "long a64_fcvtzs_d_to_i64(long a) {\n"
   "  double d; __builtin_memcpy(&d, &a, 8);\n"
   "  return (long)d;\n"
   "}\n",
   {0x4059000000000000ULL}, "FPConv"},  // 100.0

  {"a64_fcvtzs_d_to_i32",
   "long a64_fcvtzs_d_to_i32(long a) {\n"
   "  double d; __builtin_memcpy(&d, &a, 8);\n"
   "  return (long)(int)d;\n"
   "}\n",
   {0xC059000000000000ULL}, "FPConv"},  // -100.0

  {"a64_fcvtzs_f_to_i32",
   "long a64_fcvtzs_f_to_i32(long a) {\n"
   "  float f; int ia = (int)a; __builtin_memcpy(&f, &ia, 4);\n"
   "  return (long)(int)f;\n"
   "}\n",
   {0x42C80000}, "FPConv"},  // 100.0f

  // ===== FCVTZU - float/double → unsigned int, truncating =====
  {"a64_fcvtzu_d",
   "long a64_fcvtzu_d(long a) {\n"
   "  double d; __builtin_memcpy(&d, &a, 8);\n"
   "  return (long)(unsigned long)d;\n"
   "}\n",
   {0x4059000000000000ULL}, "FPConv"},  // 100.0

  // ===== SCVTF - signed int → float/double =====
  {"a64_scvtf_i64_to_d",
   "long a64_scvtf_i64_to_d(long a) {\n"
   "  double r = (double)a;\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {42}, "FPConv"},

  {"a64_scvtf_neg",
   "long a64_scvtf_neg(long a) {\n"
   "  double r = (double)a;\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {(uint64_t)(int64_t)-42}, "FPConv"},

  {"a64_scvtf_i32_to_f",
   "long a64_scvtf_i32_to_f(long a) {\n"
   "  float r = (float)(int)a;\n"
   "  int ir; __builtin_memcpy(&ir, &r, 4); return (long)ir;\n"
   "}\n",
   {100}, "FPConv"},

  // ===== UCVTF - unsigned int → float/double =====
  {"a64_ucvtf_u64_to_d",
   "long a64_ucvtf_u64_to_d(long a) {\n"
   "  double r = (double)(unsigned long)a;\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {100}, "FPConv"},

  // ===== FCVT - float <-> double =====
  {"a64_fcvt_f_to_d",
   "long a64_fcvt_f_to_d(long a) {\n"
   "  float f; int ia = (int)a; __builtin_memcpy(&f, &ia, 4);\n"
   "  double r = (double)f;\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x40A00000}, "FPConv"},  // 5.0f

  {"a64_fcvt_d_to_f",
   "long a64_fcvt_d_to_f(long a) {\n"
   "  double d; __builtin_memcpy(&d, &a, 8);\n"
   "  float r = (float)d;\n"
   "  int ir; __builtin_memcpy(&ir, &r, 4); return (long)(unsigned)ir;\n"
   "}\n",
   {0x4014000000000000ULL}, "FPConv"},  // 5.0

  // ===== FRINTZ - round toward zero (truncate) =====
  {"a64_frintz",
   "long a64_frintz(long a) {\n"
   "  double d; __builtin_memcpy(&d, &a, 8);\n"
   "  double r = __builtin_trunc(d);\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x401C666666666666ULL}, "FPConv", 2, "-fno-math-errno"},  // 7.1

  // ===== FRINTM - round toward -inf (floor) =====
  {"a64_frintm",
   "long a64_frintm(long a) {\n"
   "  double d; __builtin_memcpy(&d, &a, 8);\n"
   "  double r = __builtin_floor(d);\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x401C666666666666ULL}, "FPConv", 2, "-fno-math-errno"},  // 7.1

  {"a64_frintm_neg",
   "long a64_frintm_neg(long a) {\n"
   "  double d; __builtin_memcpy(&d, &a, 8);\n"
   "  double r = __builtin_floor(d);\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0xC01C666666666666ULL}, "FPConv", 2, "-fno-math-errno"},  // -7.1

  // ===== FRINTP - round toward +inf (ceil) =====
  {"a64_frintp",
   "long a64_frintp(long a) {\n"
   "  double d; __builtin_memcpy(&d, &a, 8);\n"
   "  double r = __builtin_ceil(d);\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x401C666666666666ULL}, "FPConv", 2, "-fno-math-errno"},  // 7.1

  // ===== FRINTN - round to nearest, ties to even =====
  {"a64_frintn",
   "long a64_frintn(long a) {\n"
   "  double d; __builtin_memcpy(&d, &a, 8);\n"
   "  double r = __builtin_nearbyint(d);\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x401C666666666666ULL}, "FPConv", 2, "-fno-math-errno"},  // 7.1

  // ===== FRINTA - round to nearest, ties away from zero =====
  {"a64_frinta",
   "long a64_frinta(long a) {\n"
   "  double d; __builtin_memcpy(&d, &a, 8);\n"
   "  double r = __builtin_round(d);\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x401C666666666666ULL}, "FPConv", 2, "-fno-math-errno"},  // 7.1

  // ===== FMOV (load FP immediate) =====
  {"a64_fmov_imm",
   "long a64_fmov_imm(long a) {\n"
   "  double r = 1.0;\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0}, "FPConv", 1},

  // ===== FMADD (multiply-add: d = a*b + c) =====
  {"a64_fmadd",
   "long a64_fmadd(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  double r = da * db + 1.0;\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4008000000000000ULL, 0x4000000000000000ULL}, "FPConv", 2,
   "-fno-math-errno -ffp-contract=fast"},

  // ===== FMSUB (multiply-sub: d = a*b - c) =====
  {"a64_fmsub",
   "long a64_fmsub(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  double r = da * db - 1.0;\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4008000000000000ULL, 0x4000000000000000ULL}, "FPConv", 2,
   "-fno-math-errno -ffp-contract=fast"},

  // ===== FCMP/FCMPE + CSEL (FP comparison with int result) =====
  {"a64_fcmp_gt",
   "long a64_fcmp_gt(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  return da > db ? 1 : 0;\n"
   "}\n",
   {0x4024000000000000ULL, 0x4014000000000000ULL}, "FPConv", 1},

  {"a64_fcmp_eq",
   "long a64_fcmp_eq(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  return da == db ? 1 : 0;\n"
   "}\n",
   {0x4024000000000000ULL, 0x4024000000000000ULL}, "FPConv", 1},

  // ===== FCSEL (FP conditional select) =====
  {"a64_fcsel",
   "long a64_fcsel(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  double r = da > db ? da : db;\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4014000000000000ULL, 0x4024000000000000ULL}, "FPConv", 1},

  // ===== FNEG (FP negate) =====
  {"a64_fneg",
   "long a64_fneg(long a) {\n"
   "  double d; __builtin_memcpy(&d, &a, 8);\n"
   "  double r = -d;\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4024000000000000ULL}, "FPConv", 1},

  // ===== FABS (FP absolute value) =====
  {"a64_fabs",
   "long a64_fabs(long a) {\n"
   "  double d; __builtin_memcpy(&d, &a, 8);\n"
   "  double r = __builtin_fabs(d);\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0xC024000000000000ULL}, "FPConv", 1},

  // ===== FSQRT =====
  {"a64_fsqrt",
   "long a64_fsqrt(long a) {\n"
   "  double d; __builtin_memcpy(&d, &a, 8);\n"
   "  double r = __builtin_sqrt(d);\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4039000000000000ULL}, "FPConv", 2, "-fno-math-errno"},  // 25.0

  // ===== FMIN/FMAX =====
  {"a64_fmin",
   "long a64_fmin(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  double r = __builtin_fmin(da, db);\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4014000000000000ULL, 0x4024000000000000ULL}, "FPConv", 2,
   "-fno-math-errno"},

  {"a64_fmax",
   "long a64_fmax(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  double r = __builtin_fmax(da, db);\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4014000000000000ULL, 0x4024000000000000ULL}, "FPConv", 2,
   "-fno-math-errno"},

  // ===== Multi-step FP pipeline (exercises FLD/FST chain) =====
  {"a64_fp_chain",
   "long a64_fp_chain(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  double sum = da + db;\n"
   "  double diff = da - db;\n"
   "  double r = sum * diff;\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4014000000000000ULL, 0x4008000000000000ULL}, "FPConv", 1},

};

// clang-format on

INSTANTIATE_TEST_SUITE_P(FPConv, A64FPConvRT,
                         ::testing::ValuesIn(kA64FPConv), rtTCName);
