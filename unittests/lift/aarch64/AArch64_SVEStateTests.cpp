//===- AArch64_SVEStateTests.cpp - SVE pipeline state coverage -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check architectural scalable registers and typed LLVM/ACLE projections.
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"

#include "neverd/lift/AArch64/AArch64Regs.h"

#include "llvm/Support/FormatVariadic.h"

#include <fstream>
#include <string>

namespace {
#define NEVERD_SVE_VECTOR_FIXTURE(Type, Name, Value)                           \
  constexpr Type Name = Value;
#include "AArch64SVEVectorFixtures.def"
#define A64_RETURN_ABI_TEXT(Name, Text) constexpr char Name[] = Text;
#include "AArch64ReturnABIFixtures.def"
} // namespace

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
  EXPECT_NE(r.out.find("llvm.aarch64.sve.dup.x.nxv16i8"), std::string::npos)
      << r.out;
  EXPECT_NE(r.out.find("llvm.aarch64.sve.lastb.nxv16i8"), std::string::npos)
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
  EXPECT_NE(source.find(kImmediateDupC), std::string::npos) << source;
  EXPECT_NE(source.find("svlastb_u8("), std::string::npos) << source;
  EXPECT_EQ(source.find("_BitInt(256)"), std::string::npos) << source;
  EXPECT_EQ(source.find("_BitInt(512)"), std::string::npos) << source;

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
  EXPECT_NE(Bytes.find(kImmediateByteStartC), std::string::npos) << Bytes;
  EXPECT_NE(Bytes.find(kImmediateByteStepC), std::string::npos) << Bytes;

  auto DWords = functionC(source, "test_sve_index_step_last");
  ASSERT_FALSE(DWords.empty()) << source;
  EXPECT_NE(DWords.find("svindex_u64("), std::string::npos) << DWords;
  EXPECT_NE(DWords.find(kImmediateWideStartC), std::string::npos) << DWords;
  EXPECT_NE(DWords.find(kImmediateWideStepC), std::string::npos) << DWords;

  expectPairedClangSyntax(cFile, source);
}

TEST_F(AArch64_SVEState, SemanticRefusalsReturnErrorsInSerialAndShardedLift) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << kMissingCompiler;
  for (unsigned Count : {kSerialFunctionCount, kShardedFunctionCount}) {
    SCOPED_TRACE(Count);
    const auto Source = tmpFile(kUnknownStateSource);
    const auto Object = tmpFile(kUnknownStateObject);
    {
      std::ofstream Out(Source);
      ASSERT_TRUE(Out.good());
      for (unsigned I = 0; I != Count; ++I)
        Out << llvm::formatv(kUnknownStateAssembly, I).str();
    }
    const auto Compiled = exec(
        NEVERD_TEST_CLANG, {kTargetOption, kCTarget, kCompileOption,
                            Source.string(), kOutputOption, Object.string()});
    ASSERT_EQ(Compiled.exitCode, 0) << Compiled.err;
    for (bool NoOptimization : {false, true}) {
      SCOPED_TRACE(NoOptimization);
      const auto Result = liftToLLVMIR(Object, {}, NoOptimization);
      EXPECT_EQ(Result.exitCode, kSemanticErrorExit) << Result.err;
      EXPECT_NE(Result.err.find(kUnknownStateText), std::string::npos)
          << Result.err;
      EXPECT_EQ(Result.out.find(kLLVMDefinition), std::string::npos)
          << Result.out;
    }
  }
}

TEST_F(AArch64_SVEState, SignedLeafPreservesLocalScalableOperations) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << kMissingCompiler;
  // MedIR keeps both PAuth operations around the local Z/P state. PACIASP
  // with RETAA is the proved compiler recipe: LLVM states the leaf's A-key
  // policy and its backend signs the new frame. RETAB matches no recipe, so
  // LLVM keeps the explicit operations around the same scalable state.
  const auto Attribute = [](llvm::StringRef Key, llvm::StringRef Value) {
    return llvm::formatv("\"{0}\"=\"{1}\"", Key, Value).str();
  };
  for (const bool Proved : {true, false}) {
    SCOPED_TRACE(Proved);
    const auto Source = tmpFile(kSignedStateSource);
    const auto Object = tmpFile(kSignedStateObject);
    {
      std::ofstream Out(Source);
      ASSERT_TRUE(Out.good());
      Out << (Proved ? kSignedStateAssembly : kUnprovedSignedStateAssembly);
    }
    const auto Compiled = exec(
        NEVERD_TEST_CLANG, {kTargetOption, kCTarget, kCompileOption,
                            Source.string(), kOutputOption, Object.string()});
    ASSERT_EQ(Compiled.exitCode, 0) << Compiled.err;
    for (bool NoOptimization : {false, true}) {
      SCOPED_TRACE(NoOptimization);
      const auto Result = liftToLLVMIR(Object, {}, NoOptimization);
      ASSERT_EQ(Result.exitCode, 0) << Result.err;
      EXPECT_NE(Result.out.find(kByteLastIR), std::string::npos) << Result.out;
      if (Proved) {
        EXPECT_NE(Result.out.find(Attribute(kSigningScope, kSigningAll)),
                  std::string::npos)
            << Result.out;
        EXPECT_NE(Result.out.find(Attribute(kSigningKey, kSigningAKey)),
                  std::string::npos)
            << Result.out;
        EXPECT_EQ(Result.out.find(kSignedStateAuthenticate), std::string::npos)
            << Result.out;
        continue;
      }
      EXPECT_EQ(Result.out.find(kSigningScope), std::string::npos)
          << Result.out;
      EXPECT_NE(Result.out.find(kSignedStatePacia), std::string::npos)
          << Result.out;
      EXPECT_NE(Result.out.find(kSignedStateAuthenticate), std::string::npos)
          << Result.out;
      EXPECT_NE(Result.out.find(kSignedStateTargetUse), std::string::npos)
          << Result.out;
    }
  }
}
