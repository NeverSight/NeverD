//===- COFFARMPipelineTests.cpp - Windows ARM lift and decompile tests -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "COFFARMPipelineTestsDetail.h"

namespace {

using namespace neverd;
using namespace neverd::coff_arm_test;

TEST_F(COFFARMPipeline, ARM32ThumbLiftAndDecompile) {
  const fs::path Path = fixture("test_patch_coff_arm.exe");
  if (!fs::exists(Path)) {
    if (!fs::exists(fixture("test_patch_coff_arm.obj")))
      GTEST_SKIP() << "ARM32 PE fixture not built (lld-link unavailable)";
    FAIL() << "ARM32 PE object exists but linked fixture is missing";
    return;
  }

  verifyAllStages(Path);

  RunResult Low = liftToLowIR(Path);
  ASSERT_EQ(Low.exitCode, 0) << Low.err;
  auto Leaf = lowFunctionBody(Low.out, "pe_leaf");
  auto Stacky = lowFunctionBody(Low.out, "pe_stacky");
  ASSERT_TRUE(Leaf.has_value()) << Low.out;
  ASSERT_TRUE(Stacky.has_value()) << Low.out;
  expectLeafSemantics(*Leaf);
  expectStackySemantics(*Stacky);
  EXPECT_NE(Stacky->find("INT_SUB reg:0x34:4 reg:0x34:4 cst:0x8:4"),
            std::string::npos)
      << "wide Thumb push must reserve both saved registers on SP:\n"
      << *Stacky;
  EXPECT_NE(Stacky->find("INT_ADD reg:0x34:4 reg:0x34:4 cst:0x8:4"),
            std::string::npos)
      << "wide Thumb pop must release both saved registers from SP:\n"
      << *Stacky;
  EXPECT_EQ(Stacky->find("INT_SUB reg:0x2C:4 reg:0x2C:4 cst:0x4:4"),
            std::string::npos)
      << "push.w was decoded as a generic STMDB using R11 as its base:\n"
      << *Stacky;
  expectNoOddFunctionAddresses(Low.out);
  verifyNoUnlifted(Path);

  RunResult Med = liftToMedIR(Path);
  ASSERT_EQ(Med.exitCode, 0) << Med.err;
  auto MedStacky = lowFunctionBody(Med.out, "pe_stacky");
  ASSERT_TRUE(MedStacky.has_value()) << Med.out;
  EXPECT_NE(MedStacky->find("cc=3 FrameSize=400"), std::string::npos)
      << *MedStacky;

  RunResult Decompile = decompileToHighC(Path);
  ASSERT_EQ(Decompile.exitCode, 0) << Decompile.err;
  const fs::path CPath = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(CPath));
  std::string C = readTextFile(CPath);
  auto LeafC = cFunctionBody(C, "pe_leaf");
  auto StackyC = cFunctionBody(C, "pe_stacky");
  ASSERT_TRUE(LeafC.has_value()) << C;
  ASSERT_TRUE(StackyC.has_value()) << C;
  EXPECT_NE(StackyC->find("pe_leaf("), std::string::npos) << *StackyC;
  EXPECT_EQ(StackyC->find("arg4"), std::string::npos)
      << "the saved return address must not become a fifth parameter:\n"
      << *StackyC;
  expectLeafCallResultStored(*StackyC);
  expectNoLocalReadBeforeDefinition(*StackyC);
  expectFrameBaseInitializedOnce(*StackyC);

  expectGeneratedCCompiles(CPath, "thumbv7-pc-windows-msvc");
}

TEST_F(COFFARMPipeline, AArch64LiftAndDecompile) {
  const fs::path Path = fixture("test_patch_coff_a64.exe");
  if (!fs::exists(Path)) {
    if (!fs::exists(fixture("test_patch_coff_a64.obj")))
      GTEST_SKIP() << "AArch64 PE fixture not built (lld-link unavailable)";
    FAIL() << "AArch64 PE object exists but linked fixture is missing";
    return;
  }

  verifyAllStages(Path);

  RunResult Low = liftToLowIR(Path);
  ASSERT_EQ(Low.exitCode, 0) << Low.err;
  auto Leaf = lowFunctionBody(Low.out, "pe_leaf");
  auto Stacky = lowFunctionBody(Low.out, "pe_stacky");
  ASSERT_TRUE(Leaf.has_value()) << Low.out;
  ASSERT_TRUE(Stacky.has_value()) << Low.out;
  expectLeafSemantics(*Leaf);
  expectStackySemantics(*Stacky);
  verifyNoUnlifted(Path);

  RunResult Decompile = decompileToHighC(Path);
  ASSERT_EQ(Decompile.exitCode, 0) << Decompile.err;
  const fs::path CPath = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(CPath));
  std::string C = readTextFile(CPath);
  auto LeafC = cFunctionBody(C, "pe_leaf");
  auto StackyC = cFunctionBody(C, "pe_stacky");
  ASSERT_TRUE(LeafC.has_value()) << C;
  ASSERT_TRUE(StackyC.has_value()) << C;
  EXPECT_NE(StackyC->find("pe_leaf("), std::string::npos) << *StackyC;
  expectLeafCallResultStored(*StackyC);
  expectLeafCallUsesParameter(*StackyC, "arg0");
  expectNoLocalReadBeforeDefinition(*StackyC);
  expectFrameBaseInitializedOnce(*StackyC);
  expectGeneratedCCompiles(CPath, "aarch64-pc-windows-msvc");
}

} // namespace
