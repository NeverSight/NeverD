#include "NeverDLiftFixture.h"

#include "neverd/lift/AArch64Regs.h"

class AArch64_SVEState : public NeverDLiftTest {
protected:
  void expectPairedClangSyntax(const fs::path &CFile,
                               const std::string &Source) {
    auto stringHeader = tmpFile("string.h");
    std::ofstream shim(stringHeader);
    ASSERT_TRUE(shim.good());
    shim << "void *memcpy(void *, const void *, __SIZE_TYPE__);\n";
    shim.close();

    auto syntax = exec(NEVERD_TEST_CLANG,
                       {"-target", "aarch64-none-elf", "-ffreestanding",
                        "-march=armv8.2-a+sve", "-std=gnu11", "-I",
                        tmp().string(), "-fsyntax-only", CFile.string()});
    EXPECT_EQ(syntax.exitCode, 0) << syntax.err << "\n" << Source;
  }
};

static fs::path sveStateObj() {
  return fs::path(TEST_OBJ_DIR) / "test_sve_state_a64.o";
}

TEST_F(AArch64_SVEState, MapsArchitecturalZPredicateAndFFRState) {
  auto Z0 = neverd::mapCapstoneReg(AARCH64_REG_Z0);
  auto Z31 = neverd::mapCapstoneReg(AARCH64_REG_Z31);
  auto P0 = neverd::mapCapstoneReg(AARCH64_REG_P0);
  auto P15 = neverd::mapCapstoneReg(AARCH64_REG_P15);
  auto FFR = neverd::mapCapstoneReg(AARCH64_REG_FFR);

  EXPECT_EQ(Z0.Size, 256u);
  EXPECT_EQ(Z31.Size, 256u);
  EXPECT_EQ(P0.Size, 32u);
  EXPECT_EQ(P15.Size, 32u);
  EXPECT_EQ(FFR.Size, 32u);
  EXPECT_NE(Z0.Offset, Z31.Offset);
  EXPECT_NE(P0.Offset, P15.Offset);
  EXPECT_NE(P0.Offset, FFR.Offset);
  EXPECT_STREQ(neverd::getAArch64RegName(Z0.Offset, Z0.Size), "Z0");
  EXPECT_STREQ(neverd::getAArch64RegName(P0.Offset, P0.Size), "P0");
  EXPECT_STREQ(neverd::getAArch64RegName(FFR.Offset, FFR.Size), "FFR");
}

TEST_F(AArch64_SVEState, LastbPreservesPredicateAndVectorSemantics) {
  auto r = liftToLLVMIR(sveStateObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;
  EXPECT_NE(r.out.find("llvm.aarch64.sve.ptrue"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("llvm.aarch64.sve.dup.x.nxv16i8"),
            std::string::npos)
      << r.out;
  EXPECT_NE(r.out.find("llvm.aarch64.sve.lastb.nxv16i8"),
            std::string::npos)
      << r.out;
  EXPECT_EQ(r.out.find("ret i64 0"), std::string::npos) << r.out;
}

TEST_F(AArch64_SVEState, HighCUsesStandardSVEACLEAndCompiles) {
  auto r = decompileToHighC(sveStateObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  auto cFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(cFile));
  std::ifstream input(cFile);
  ASSERT_TRUE(input.good());
  std::string source((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
  EXPECT_NE(source.find("svptrue_b8()"), std::string::npos) << source;
  EXPECT_NE(source.find("svdup_n_u8(1)"), std::string::npos) << source;
  EXPECT_NE(source.find("svlastb_u8("), std::string::npos) << source;

  expectPairedClangSyntax(cFile, source);
}
