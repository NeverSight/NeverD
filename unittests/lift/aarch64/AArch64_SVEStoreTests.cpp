#include "NeverDLiftFixture.h"

class AArch64_SVEStore : public NeverDLiftTest {
protected:
  void expectPairedClangSyntax(const fs::path &CFile,
                               const std::string &Source) {
    auto Syntax = checkHighCClangSyntax(
        CFile, {"-target", "aarch64-none-elf", "-ffreestanding",
                "-march=armv8.2-a+sve", "-std=gnu11"});
    EXPECT_EQ(Syntax.exitCode, 0) << Syntax.err << "\n" << Source;
  }
};

static fs::path sveStoreObj() {
  return fs::path(TEST_OBJ_DIR) / "test_sve_store_a64.o";
}

TEST_F(AArch64_SVEStore, UsesEffectiveAddressAndPredicatedVectorValue) {
  auto LLVM = liftToLLVMIR(sveStoreObj());
  ASSERT_EQ(LLVM.exitCode, 0) << LLVM.err;
  EXPECT_NE(LLVM.out.find("llvm.aarch64.sve.ptrue"), std::string::npos)
      << LLVM.out;
  EXPECT_NE(LLVM.out.find("llvm.aarch64.sve.dup.x.nxv16i8"), std::string::npos)
      << LLVM.out;
  EXPECT_NE(LLVM.out.find("llvm.aarch64.sve.st1"), std::string::npos)
      << LLVM.out;
  EXPECT_EQ(LLVM.out.find("load i64, ptr %memptr"), std::string::npos)
      << LLVM.out;
}

TEST_F(AArch64_SVEStore, HighCUsesStandardSVEACLEAndCompiles) {
  auto Result = decompileToHighC(sveStoreObj());
  ASSERT_EQ(Result.exitCode, 0) << Result.err;

  auto CFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(CFile));
  std::ifstream Input(CFile);
  ASSERT_TRUE(Input.good());
  std::string Source((std::istreambuf_iterator<char>(Input)),
                     std::istreambuf_iterator<char>());
  EXPECT_NE(Source.find("#include <arm_sve.h>"), std::string::npos) << Source;
  EXPECT_NE(Source.find("svst1_u8("), std::string::npos) << Source;
  EXPECT_EQ(Source.find("__neverd"), std::string::npos) << Source;

  expectPairedClangSyntax(CFile, Source);
}
