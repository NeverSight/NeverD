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

TEST(X64FcomiKnownAnswer, UnorderedClearsOfSfAfAndPops) {
  // Seed OF/SF/AF, compare a quiet NaN with 1.0, then materialize both the
  // arithmetic flags and the surviving x87 value.  FUCOMIP must produce the
  // unordered CF/PF/ZF pattern, clear OF/SF/AF, and pop exactly one value.
  static constexpr uint8_t Code[] = {
      0xd9, 0xe8,       // fld1
      0xdd, 0x03,       // fld qword ptr [rbx]
      0xdf, 0xe9,       // fucomip st, st(1)
      0xdb, 0x5b, 0x08, // fistp dword ptr [rbx + 8]
  };
  static constexpr uint64_t QuietNaN = 0x7ff8000000000000ULL;
  static constexpr uint64_t SeededFlags = 0x890;
  static constexpr uint64_t ArithmeticFlags = 0x8d5;

  uc_engine *UC = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &UC), UC_ERR_OK);
  ASSERT_EQ(uc_mem_map(UC, CODE_BASE, 0x1000, UC_PROT_ALL), UC_ERR_OK);
  ASSERT_EQ(uc_mem_map(UC, DATA_BASE, 0x1000, UC_PROT_ALL), UC_ERR_OK);
  ASSERT_EQ(uc_mem_write(UC, CODE_BASE, Code, sizeof(Code)), UC_ERR_OK);
  ASSERT_EQ(uc_mem_write(UC, DATA_BASE, &QuietNaN, sizeof(QuietNaN)),
            UC_ERR_OK);

  uint64_t DataAddress = DATA_BASE;
  uint64_t Flags = SeededFlags;
  ASSERT_EQ(uc_reg_write(UC, UC_X86_REG_RBX, &DataAddress), UC_ERR_OK);
  ASSERT_EQ(uc_reg_write(UC, UC_X86_REG_EFLAGS, &Flags), UC_ERR_OK);
  ASSERT_EQ(uc_emu_start(UC, CODE_BASE, CODE_BASE + sizeof(Code), 0, 0),
            UC_ERR_OK);

  uint32_t RemainingValue = 0;
  ASSERT_EQ(uc_reg_read(UC, UC_X86_REG_EFLAGS, &Flags), UC_ERR_OK);
  ASSERT_EQ(
      uc_mem_read(UC, DATA_BASE + 8, &RemainingValue, sizeof(RemainingValue)),
      UC_ERR_OK);
  EXPECT_EQ(Flags & ArithmeticFlags, 0x45u);
  EXPECT_EQ(RemainingValue, 1u);
  EXPECT_EQ(uc_close(UC), UC_ERR_OK);
}

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
  {"fucomip_flags_clear_pop",
   "unsigned long f(unsigned long bits){\n"
   "  double x; __builtin_memcpy(&x,&bits,8);\n"
   "  unsigned long flags; int marker;\n"
   "  __asm__ volatile(\n"
   "    \"mov $0x890, %%eax\\n\\tpush %%rax\\n\\tpopfq\\n\\t\"\n"
   "    \"fld1\\n\\tfldl %2\\n\\tfucomip %%st(1), %%st\\n\\t\"\n"
   "    \"fistpl %1\\n\\tpushfq\\n\\tpop %0\"\n"
   "    : \"=r\"(flags), \"=m\"(marker) : \"m\"(x)\n"
   "    : \"rax\", \"cc\", \"memory\");\n"
   "  return ((unsigned long)(unsigned)marker << 12) | (flags & 0x8d5ul);\n"
   "}\n",
   {D_NAN}, "Fcomi", 0, "-mno-sse -mfpmath=387"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(Fcomi, X64FcomiRT, ::testing::ValuesIn(kX64), rtTCName);
