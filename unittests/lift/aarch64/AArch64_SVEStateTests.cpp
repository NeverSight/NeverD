#include "NeverDLiftFixture.h"

#include "neverd/lift/AArch64Regs.h"

class AArch64_SVEState : public NeverDLiftTest {
protected:
  void expectPairedClangSyntax(const fs::path &CFile,
                               const std::string &Source) {
    auto syntax = checkHighCClangSyntax(
        CFile, {"-target", "aarch64-none-elf", "-ffreestanding",
                "-march=armv8.2-a+sve", "-std=gnu11"});
    EXPECT_EQ(syntax.exitCode, 0) << syntax.err << "\n" << Source;
  }
};

static fs::path sveStateObj() {
  return fs::path(TEST_OBJ_DIR) / "test_sve_state_a64.o";
}

static std::string functionIR(const std::string &IR, const std::string &Name) {
  auto NamePos = IR.find("@" + Name + "(");
  if (NamePos == std::string::npos)
    return {};
  auto Begin = IR.rfind("define ", NamePos);
  auto End = IR.find("\n}", NamePos);
  if (Begin == std::string::npos || End == std::string::npos)
    return {};
  return IR.substr(Begin, End + 2 - Begin);
}

static std::string functionC(const std::string &Source,
                             const std::string &Name) {
  auto NamePos = Source.find(Name + "(");
  if (NamePos == std::string::npos)
    return {};
  auto Begin = Source.rfind('\n', NamePos);
  auto End = Source.find("\n}", NamePos);
  if (End == std::string::npos)
    return {};
  Begin = Begin == std::string::npos ? 0 : Begin + 1;
  return Source.substr(Begin, End + 2 - Begin);
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

TEST_F(AArch64_SVEState, IndexPreservesStartStepAndElementWidth) {
  auto r = liftToLLVMIR(sveStateObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  auto Bytes = functionIR(r.out, "test_sve_index_const_last");
  ASSERT_FALSE(Bytes.empty()) << r.out;
  EXPECT_NE(Bytes.find("llvm.aarch64.sve.index.nxv16i8(i8 5, i8 0)"),
            std::string::npos)
      << Bytes;

  auto DWords = functionIR(r.out, "test_sve_index_step_last");
  ASSERT_FALSE(DWords.empty()) << r.out;
  EXPECT_NE(DWords.find("llvm.aarch64.sve.index.nxv2i64(i64 7, i64 3)"),
            std::string::npos)
      << DWords;
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
  EXPECT_EQ(source.find("_BitInt(256)"), std::string::npos) << source;

  expectPairedClangSyntax(cFile, source);
}

TEST_F(AArch64_SVEState, HighCIndexUsesStandardSVEACLEAndCompiles) {
  auto r = decompileToHighC(sveStateObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  auto cFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(cFile));
  std::ifstream input(cFile);
  ASSERT_TRUE(input.good());
  std::string source((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());

  auto Bytes = functionC(source, "test_sve_index_const_last");
  ASSERT_FALSE(Bytes.empty()) << source;
  EXPECT_NE(Bytes.find("svindex_u8("), std::string::npos) << Bytes;
  EXPECT_NE(Bytes.find("(uint8_t)(5)"), std::string::npos) << Bytes;
  EXPECT_NE(Bytes.find("(uint8_t)(0)"), std::string::npos) << Bytes;

  auto DWords = functionC(source, "test_sve_index_step_last");
  ASSERT_FALSE(DWords.empty()) << source;
  EXPECT_NE(DWords.find("svindex_u64("), std::string::npos) << DWords;
  EXPECT_NE(DWords.find("(uint64_t)(7)"), std::string::npos) << DWords;
  EXPECT_NE(DWords.find("(uint64_t)(3)"), std::string::npos) << DWords;

  expectPairedClangSyntax(cFile, source);
}
