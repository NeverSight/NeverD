//===- AArch64_HighCIntrinsicSpellingTests.cpp - ACLE names in HighC ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// AArch64 operations with no C operator print as the ACLE intrinsic their
// shape names.  Each test assembles the instructions, decompiles them to
// HighC, and checks the C with Clang for AArch64.
//
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"

#include <fstream>
#include <iterator>

class AArch64_HighCIntrinsicSpelling : public NeverDLiftTest {
protected:
  /// Assemble \p Assembly for \p March and decompile it to HighC.
  std::string highCOf(const std::string &Assembly, const std::string &March) {
    const auto Source = tmpFile("spelling.s");
    const auto Object = tmpFile("spelling.o");
    std::ofstream(Source) << Assembly;
    const auto Assembled = exec(
        NEVERD_TEST_CLANG, {"-target", "aarch64-none-elf", "-march=" + March,
                            "-c", Source.string(), "-o", Object.string()});
    EXPECT_EQ(Assembled.exitCode, 0) << Assembled.err;
    const auto Decompiled = decompileToHighC(Object);
    EXPECT_EQ(Decompiled.exitCode, 0) << Decompiled.err;
    std::ifstream Input(tmpFile("decompiled_high.c"));
    return std::string((std::istreambuf_iterator<char>(Input)),
                       std::istreambuf_iterator<char>());
  }

  /// Expect \p C to compile for AArch64 with no target feature beyond the
  /// base architecture.
  void expectCompiles(const std::string &C) {
    const auto CFile = tmpFile("spelling_high.c");
    std::ofstream(CFile) << C;
    const auto Compiled = checkHighCClangCompile(
        CFile, {"-target", "aarch64-none-elf", "-ffreestanding", "-std=gnu11"});
    EXPECT_EQ(Compiled.exitCode, 0) << Compiled.err << "\n" << C;
  }
};

// The saturating and rounding shifts by a vector of signed amounts, an
// immediate shift as that amount in every lane, and the scalar forms.
TEST_F(AArch64_HighCIntrinsicSpelling, ShiftsByLaneUseTheirNeonIntrinsic) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "assembling AArch64 requires Clang";
  const std::string C = highCOf(R"S(
.text
.global neon_shifts
.type neon_shifts,%function
neon_shifts:
  ldp q0, q1, [x0]
  sqshl v2.4s, v0.4s, v1.4s
  uqshl v3.8h, v0.8h, v1.8h
  sqrshl v4.2s, v0.2s, v1.2s
  uqrshl v5.16b, v0.16b, v1.16b
  srshl v6.2d, v0.2d, v1.2d
  urshl d7, d0, d1
  sqshl s16, s0, s1
  uqrshl h17, h0, h1
  sqshl v18.4s, v0.4s, #3
  stp q2, q3, [x1]
  stp q4, q5, [x1, #32]
  stp q6, q7, [x1, #64]
  stp q16, q17, [x1, #96]
  str q18, [x1, #128]
  ret
.size neon_shifts,.-neon_shifts
)S",
                                "armv8-a");
  for (const char *Name :
       {"vqshlq_s32(", "vqshlq_u16(", "vqrshl_s32(", "vqrshlq_u8(",
        "vrshlq_s64(", "vrshl_u64(", "vqshls_s32(", "vqrshlh_u16("})
    EXPECT_NE(C.find(Name), std::string::npos) << Name << "\n" << C;
  EXPECT_EQ(C.find("unknown"), std::string::npos) << C;
  expectCompiles(C);
}

// The saturating shifts right that narrow each lane to half its width,
// rounding or not, to a signed or an unsigned result.
TEST_F(AArch64_HighCIntrinsicSpelling, NarrowingShiftsUseTheirNeonIntrinsic) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "assembling AArch64 requires Clang";
  const std::string C = highCOf(R"S(
.text
.global neon_narrowing_shifts
.type neon_narrowing_shifts,%function
neon_narrowing_shifts:
  ldp q0, q1, [x0]
  sqshrn v2.4h, v0.4s, #3
  sqrshrn2 v2.8h, v1.4s, #5
  uqshrn v3.8b, v0.8h, #2
  uqrshrn v4.2s, v1.2d, #7
  sqshrun v5.4h, v0.4s, #4
  sqrshrun v6.8b, v1.8h, #1
  stp q2, q3, [x1]
  stp q4, q5, [x1, #32]
  str q6, [x1, #64]
  ret
.size neon_narrowing_shifts,.-neon_narrowing_shifts
)S",
                                "armv8-a");
  for (const char *Name :
       {"vqshrn_n_s32(", "vqrshrn_n_s32(", "vqshrn_n_u16(", "vqrshrn_n_u64(",
        "vqshrun_n_s32(", "vqrshrun_n_s16("})
    EXPECT_NE(C.find(Name), std::string::npos) << Name << "\n" << C;
  EXPECT_EQ(C.find("unknown"), std::string::npos) << C;
  expectCompiles(C);
}

// SQSHLU saturates signed lanes shifted left by an immediate to the unsigned
// range; ACLE has it only with the immediate, which the lifter passes.
TEST_F(AArch64_HighCIntrinsicSpelling, ShiftLeftUnsignedUsesItsImmediate) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "assembling AArch64 requires Clang";
  const std::string C = highCOf(R"S(
.text
.global neon_shift_left_unsigned
.type neon_shift_left_unsigned,%function
neon_shift_left_unsigned:
  ldp q0, q1, [x0]
  sqshlu v2.4s, v0.4s, #3
  sqshlu v3.8b, v1.8b, #7
  sqshlu s16, s0, #5
  sqshlu d17, d1, #9
  stp q2, q3, [x1]
  stp q16, q17, [x1, #32]
  ret
.size neon_shift_left_unsigned,.-neon_shift_left_unsigned
)S",
                                "armv8-a");
  for (const char *Name :
       {"vqshluq_n_s32(", "vqshlu_n_s8(", "vqshlus_n_s32(", "vqshlu_n_s64("})
    EXPECT_NE(C.find(Name), std::string::npos) << Name << "\n" << C;
  for (const char *Shift : {", 3))", ", 7))", ", 5)", ", 9))"})
    EXPECT_NE(C.find(Shift), std::string::npos) << Shift << "\n" << C;
  EXPECT_EQ(C.find("unknown"), std::string::npos) << C;
  expectCompiles(C);
}

// CRC32 and CRC32C of each operand width print as the ACLE intrinsic, the
// function taking the target feature arm_acle.h requires.
TEST_F(AArch64_HighCIntrinsicSpelling, Crc32UsesAcleWithTheCrcFeature) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "assembling AArch64 requires Clang";
  const std::string C = highCOf(R"S(
.text
.global crc_all
.type crc_all,%function
crc_all:
  crc32b w0, w0, w1
  crc32h w0, w0, w1
  crc32w w0, w0, w1
  crc32x w0, w0, x2
  crc32cb w0, w0, w1
  crc32ch w0, w0, w1
  crc32cw w0, w0, w1
  crc32cx w0, w0, x2
  ret
.size crc_all,.-crc_all
)S",
                                "armv8-a+crc");
  for (const char *Name : {"__crc32b(", "__crc32h(", "__crc32w(", "__crc32d(",
                           "__crc32cb(", "__crc32ch(", "__crc32cw(",
                           "__crc32cd(", "__attribute__((target(\"+crc\")))"})
    EXPECT_NE(C.find(Name), std::string::npos) << Name << "\n" << C;
  EXPECT_EQ(C.find("unknown"), std::string::npos) << C;
  expectCompiles(C);
}
