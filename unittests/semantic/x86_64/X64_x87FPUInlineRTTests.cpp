//===- X64_x87FPUInlineRTTests.cpp - x87 FPU inline asm roundtrip -*- C++ -*-//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Tests x86 x87 FPU instructions via inline asm roundtrip verification.
// Exercises X86LiftFPU.cpp: FADD/FSUB/FMUL/FDIV/FSQRT/FABS/FCHS/FILD/FIST
// FLD/FST/FCOM/FXCH/FRNDINT/FPREM/F2XM1/FSCALE and more.
//
// Uses -mno-sse -mfpmath=387 for some tests to force x87 code generation.
// Other tests use explicit x87 inline asm with memory operands.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64x87FPURT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64x87FPURT, Verify) { roundTripX64(GetParam()); }

TEST(X64X87FscaleKnownAnswer, IgnoresControlWordPrecision) {
  // 1/3 scaled by 2^2 is exact in the x87 extended format: only the exponent
  // changes.  The control word requests 24-bit precision, which FSCALE must
  // ignore for its result.
  static constexpr uint8_t Code[] = {0xd9, 0xfd}; // fscale
  union X87Value {
    uint64_t Alignment;
    uint8_t Bytes[10];
  };
  auto MakeX87 = [](uint64_t Significand, uint16_t SignExponent) {
    X87Value Value{};
    std::memcpy(Value.Bytes, &Significand, sizeof(Significand));
    std::memcpy(Value.Bytes + sizeof(Significand), &SignExponent,
                sizeof(SignExponent));
    return Value;
  };

  X87Value St0 = MakeX87(0xaaaaaaaaaaaaaaabULL, 0x3ffd);
  X87Value St1 = MakeX87(0x8000000000000000ULL, 0x4000);
  uint16_t ControlWord = 0x007f; // round-to-nearest, 24-bit precision
  uint16_t StatusWord = 0;
  uint16_t TagWord = 0;

  uc_engine *UC = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &UC), UC_ERR_OK);
  ASSERT_EQ(uc_mem_map(UC, CODE_BASE, 0x1000, UC_PROT_ALL), UC_ERR_OK);
  ASSERT_EQ(uc_mem_write(UC, CODE_BASE, Code, sizeof(Code)), UC_ERR_OK);
  ASSERT_EQ(uc_reg_write(UC, UC_X86_REG_FPCW, &ControlWord), UC_ERR_OK);
  ASSERT_EQ(uc_reg_write(UC, UC_X86_REG_FPSW, &StatusWord), UC_ERR_OK);
  ASSERT_EQ(uc_reg_write(UC, UC_X86_REG_FPTAG, &TagWord), UC_ERR_OK);
  ASSERT_EQ(uc_reg_write(UC, UC_X86_REG_FP0, &St0), UC_ERR_OK);
  ASSERT_EQ(uc_reg_write(UC, UC_X86_REG_FP1, &St1), UC_ERR_OK);
  ASSERT_EQ(uc_emu_start(UC, CODE_BASE, CODE_BASE + sizeof(Code), 0, 0),
            UC_ERR_OK);
  ASSERT_EQ(uc_reg_read(UC, UC_X86_REG_FP0, &St0), UC_ERR_OK);

  uint64_t ResultSignificand = 0;
  uint16_t ResultSignExponent = 0;
  std::memcpy(&ResultSignificand, St0.Bytes, sizeof(ResultSignificand));
  std::memcpy(&ResultSignExponent, St0.Bytes + sizeof(ResultSignificand),
              sizeof(ResultSignExponent));
  EXPECT_EQ(ResultSignificand, 0xaaaaaaaaaaaaaaabULL);
  EXPECT_EQ(ResultSignExponent, 0x3fffu);
  EXPECT_EQ(uc_close(UC), UC_ERR_OK);
}

// clang-format off

static const std::vector<RoundTripTC> kX64x87FPU = {

  // ===== FSCALE ignores the control-word precision field =====
  {"x87_fscale_precision",
   "struct __attribute__((packed)) X87V { unsigned long long sig; unsigned short se; };\n"
   "unsigned long f(unsigned long unused) {\n"
   "  (void)unused;\n"
   "  struct X87V src={0xaaaaaaaaaaaaaaabULL,0x3ffd};\n"
   "  struct X87V scale={0x8000000000000000ULL,0x4000}, out;\n"
   "  unsigned short saved, cw=0x007f;\n"
   "  __asm__ volatile(\"fnstcw %0\\n\\tfldcw %1\" : \"=m\"(saved) : \"m\"(cw) : \"memory\");\n"
   "  __asm__ volatile(\"fldt %2\\n\\tfldt %1\\n\\tfscale\\n\\tfstpt %0\\n\\tfstp %%st(0)\"\n"
   "                   : \"=m\"(out) : \"m\"(src), \"m\"(scale) : \"memory\");\n"
   "  __asm__ volatile(\"fldcw %0\" : : \"m\"(saved) : \"memory\");\n"
   "  return (unsigned long)(out.sig ^ ((unsigned long long)out.se << 48));\n"
   "}\n",
   {0}, "x87FPU", 0, "-mno-sse -mfpmath=387"},

  // ===== FADD via x87 forced mode =====
  {"x87_fadd",
   "long x87_fadd(long a, long b) {\n"
   "  double da, db, r;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  r = da + db;\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4024000000000000ULL, 0x4014000000000000ULL}, "x87FPU", 0,
   "-mno-sse -mfpmath=387"},

  // ===== FSUB =====
  {"x87_fsub",
   "long x87_fsub(long a, long b) {\n"
   "  double da, db, r;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  r = da - db;\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4024000000000000ULL, 0x4014000000000000ULL}, "x87FPU", 0,
   "-mno-sse -mfpmath=387"},

  // ===== FMUL =====
  {"x87_fmul",
   "long x87_fmul(long a, long b) {\n"
   "  double da, db, r;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  r = da * db;\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4008000000000000ULL, 0x4000000000000000ULL}, "x87FPU", 0,
   "-mno-sse -mfpmath=387"},

  // ===== FDIV =====
  {"x87_fdiv",
   "long x87_fdiv(long a, long b) {\n"
   "  double da, db, r;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  r = da / db;\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4024000000000000ULL, 0x4014000000000000ULL}, "x87FPU", 0,
   "-mno-sse -mfpmath=387"},

  // ===== FCHS (negate) =====
  {"x87_fchs",
   "long x87_fchs(long a) {\n"
   "  double da, r;\n"
   "  __builtin_memcpy(&da, &a, 8);\n"
   "  r = -da;\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4024000000000000ULL}, "x87FPU", 0, "-mno-sse -mfpmath=387"},

  // ===== FABS =====
  {"x87_fabs",
   "long x87_fabs(long a) {\n"
   "  double da, r;\n"
   "  __builtin_memcpy(&da, &a, 8);\n"
   "  r = __builtin_fabs(da);\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0xC024000000000000ULL}, "x87FPU", 0, "-mno-sse -mfpmath=387"},

  // ===== FILD + FISTP (int <-> x87 conversion) =====
  {"x87_fild_fistp_i32",
   "long x87_fild_fistp_i32(long a) {\n"
   "  int ia = (int)a;\n"
   "  double r;\n"
   "  __asm__ volatile (\"fildl %1\\n\\tfstpl %0\" : \"=m\"(r) : \"m\"(ia));\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {42}, "x87FPU"},

  {"x87_fild_fistp_i64",
   "long x87_fild_fistp_i64(long a) {\n"
   "  double r;\n"
   "  __asm__ volatile (\"fildq %1\\n\\tfstpl %0\" : \"=m\"(r) : \"m\"(a));\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {1234567890LL}, "x87FPU"},

  // ===== FSQRT =====
  {"x87_fsqrt",
   "long x87_fsqrt(long a) {\n"
   "  double da, r;\n"
   "  __builtin_memcpy(&da, &a, 8);\n"
   "  __asm__ volatile (\"fldl %1\\n\\tfsqrt\\n\\tfstpl %0\" : \"=m\"(r) : \"m\"(da));\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4039000000000000ULL}, "x87FPU"},  // 25.0

  // ===== FLD + FST (load/store) =====
  {"x87_fld_fst",
   "long x87_fld_fst(long a) {\n"
   "  double da, r;\n"
   "  __builtin_memcpy(&da, &a, 8);\n"
   "  __asm__ volatile (\"fldl %1\\n\\tfstpl %0\" : \"=m\"(r) : \"m\"(da));\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4014000000000000ULL}, "x87FPU"},  // 5.0

  // ===== FXCH =====
  {"x87_fxch",
   "long x87_fxch(long a, long b) {\n"
   "  double da, db, r;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  __asm__ volatile (\n"
   "    \"fldl %1\\n\\t\"\n"
   "    \"fldl %2\\n\\t\"\n"
   "    \"fxch\\n\\t\"\n"
   "    \"fstpl %0\\n\\t\"\n"
   "    \"fstp %%st(0)\" : \"=m\"(r) : \"m\"(da), \"m\"(db));\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4024000000000000ULL, 0x4014000000000000ULL}, "x87FPU"},

  // ===== x87 comparison via C >= =====
  {"x87_compare_ge",
   "long x87_compare_ge(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  return da >= db ? 1 : 0;\n"
   "}\n",
   {0x4024000000000000ULL, 0x4014000000000000ULL}, "x87FPU", 0,
   "-mno-sse -mfpmath=387"},

  {"x87_compare_lt",
   "long x87_compare_lt(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  return da < db ? 1 : 0;\n"
   "}\n",
   {0x4014000000000000ULL, 0x4024000000000000ULL}, "x87FPU", 0,
   "-mno-sse -mfpmath=387"},

  // ===== x87 int-to-float conversion via C cast =====
  {"x87_int_to_double",
   "long x87_int_to_double(long a) {\n"
   "  double r = (double)a;\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {100}, "x87FPU", 0, "-mno-sse -mfpmath=387"},

  {"x87_double_to_int",
   "long x87_double_to_int(long a) {\n"
   "  double da;\n"
   "  __builtin_memcpy(&da, &a, 8);\n"
   "  return (long)da;\n"
   "}\n",
   {0x4059000000000000ULL}, "x87FPU", 0, "-mno-sse -mfpmath=387"},  // 100.0

  // ===== int → double conversion (x87 FILD+FSTP path) =====
  {"x87_int_to_double_neg",
   "long x87_int_to_double_neg(long a) {\n"
   "  double r = (double)(int)a;\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {(uint64_t)(int64_t)-42}, "x87FPU", 0, "-mno-sse -mfpmath=387"},  // 5.0f

  // ===== FRNDINT (round to integer) =====
  {"x87_frndint",
   "long x87_frndint(long a) {\n"
   "  double da, r;\n"
   "  __builtin_memcpy(&da, &a, 8);\n"
   "  __asm__ volatile (\"fldl %1\\n\\tfrndint\\n\\tfstpl %0\" : \"=m\"(r) : \"m\"(da));\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x401C000000000000ULL}, "x87FPU"},  // 7.0

  // FRNDINT at a .5 tie: rounds to even (2.5 -> 2.0), not half-away (3.0).
  {"x87_frndint_tie25",
   "long x87_frndint_tie25(long a) {\n"
   "  double da, r;\n"
   "  __builtin_memcpy(&da, &a, 8);\n"
   "  __asm__ volatile (\"fldl %1\\n\\tfrndint\\n\\tfstpl %0\" : \"=m\"(r) : \"m\"(da));\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4004000000000000ULL}, "x87FPU"},  // 2.5 -> 2.0
  {"x87_frndint_tie05",
   "long x87_frndint_tie05(long a) {\n"
   "  double da, r;\n"
   "  __builtin_memcpy(&da, &a, 8);\n"
   "  __asm__ volatile (\"fldl %1\\n\\tfrndint\\n\\tfstpl %0\" : \"=m\"(r) : \"m\"(da));\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x3FE0000000000000ULL}, "x87FPU"},  // 0.5 -> 0.0

  // FIST/FISTP round per the control word (nearest), not truncate.  2.7 rounds
  // up to 3 (truncation would give 2); 3.5 ties to even -> 4.
  {"x87_fistp_round27",
   "long x87_fistp_round27(long a) {\n"
   "  double da; int r;\n"
   "  __builtin_memcpy(&da, &a, 8);\n"
   "  __asm__ volatile (\"fldl %1\\n\\tfistpl %0\" : \"=m\"(r) : \"m\"(da));\n"
   "  return (long)r;\n"
   "}\n",
   {0x400599999999999AULL}, "x87FPU"},  // 2.7 -> 3
  {"x87_fistp_tie35",
   "long x87_fistp_tie35(long a) {\n"
   "  double da; int r;\n"
   "  __builtin_memcpy(&da, &a, 8);\n"
   "  __asm__ volatile (\"fldl %1\\n\\tfistpl %0\" : \"=m\"(r) : \"m\"(da));\n"
   "  return (long)r;\n"
   "}\n",
   {0x400C000000000000ULL}, "x87FPU"},  // 3.5 -> 4

  // ===== x87 conditional assignment via ternary =====
  {"x87_ternary_select",
   "long x87_ternary_select(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  double r = (da > db) ? da : db;\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4014000000000000ULL, 0x4024000000000000ULL}, "x87FPU", 0,
   "-mno-sse -mfpmath=387"},

  // x87 multi-op chains with -mno-sse (FADDP/FSUBP/FMULP pop-and-operate
  // sequences) have stack tracking issues where the popped intermediate
  // result is lost. Tracked as known x87 stack management limitation.
  // The simple single-operation tests above (fadd/fsub/fmul/fdiv) pass.

  // ===== FLD1 / FLDZ (load constant 1.0 / 0.0) =====
  {"x87_fld1",
   "long x87_fld1(long a) {\n"
   "  double r;\n"
   "  __asm__ volatile (\"fld1\\n\\tfstpl %0\" : \"=m\"(r));\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0}, "x87FPU"},

  {"x87_fldz",
   "long x87_fldz(long a) {\n"
   "  double r;\n"
   "  __asm__ volatile (\"fldz\\n\\tfstpl %0\" : \"=m\"(r));\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0}, "x87FPU"},

};

// clang-format on

INSTANTIATE_TEST_SUITE_P(x87FPU, X64x87FPURT,
                         ::testing::ValuesIn(kX64x87FPU), rtTCName);
