#include "NeverDLiftFixture.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/lift/AArch64Regs.h"

class AArch64_WSP : public NeverDLiftTest {
protected:
  void expectPairedClangSyntax(const fs::path &CFile,
                               const std::string &Source) {
    auto Syntax = exec(NEVERD_TEST_CLANG,
                       {"-target", "aarch64-none-elf", "-ffreestanding",
                        "-std=gnu11", "-fsyntax-only", CFile.string()});
    EXPECT_EQ(Syntax.exitCode, 0) << Syntax.err << "\n" << Source;
  }
};

static fs::path wspObj() {
  return fs::path(TEST_OBJ_DIR) / "test_wsp_a64.o";
}

TEST_F(AArch64_WSP, MapsWSPAsZeroExtendingSPSubregister) {
  auto WSP = neverd::mapCapstoneReg(AARCH64_REG_WSP);
  EXPECT_EQ(WSP.Offset, neverd::a64reg::SP);
  EXPECT_EQ(WSP.Size, 4u);

  const auto &TRI = neverd::getTargetRegInfo(neverd::Arch::AArch64);
  EXPECT_TRUE(TRI.writeZeroExtends(neverd::a64reg::SP, 4));
  EXPECT_EQ(TRI.findWideReg(neverd::a64reg::SP, 4),
            std::make_pair(neverd::a64reg::SP, uint16_t{8}));
}

TEST_F(AArch64_WSP, LiftPreservesWSPWriteAndZeroExtension) {
  auto Low = liftToLowIR(wspObj());
  ASSERT_EQ(Low.exitCode, 0) << Low.err;
  EXPECT_NE(Low.out.find("COPY reg:0xF8:4 reg:0xF8:4"), std::string::npos)
      << Low.out;
  EXPECT_NE(Low.out.find("INTRINSIC reg:0xF8:8"), std::string::npos)
      << Low.out;

  auto LLVM = liftToLLVMIR(wspObj());
  ASSERT_EQ(LLVM.exitCode, 0) << LLVM.err;
  EXPECT_NE(LLVM.out.find("llvm.aarch64.wsp.write"), std::string::npos)
      << LLVM.out;
  EXPECT_NE(LLVM.out.find("llvm.aarch64.wsp.read"), std::string::npos)
      << LLVM.out;
}

TEST_F(AArch64_WSP, HighCUsesCompilerBuiltinAndCompiles) {
  auto Result = decompileToHighC(wspObj());
  ASSERT_EQ(Result.exitCode, 0) << Result.err;

  auto CFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(CFile));
  std::ifstream Input(CFile);
  ASSERT_TRUE(Input.good());
  std::string Source((std::istreambuf_iterator<char>(Input)),
                     std::istreambuf_iterator<char>());
  EXPECT_NE(Source.find("__builtin_arm_wsp_zero_extend("), std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("__neverd"), std::string::npos) << Source;
  expectPairedClangSyntax(CFile, Source);
}
