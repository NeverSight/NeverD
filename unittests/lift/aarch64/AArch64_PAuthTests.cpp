//===- AArch64_PAuthTests.cpp - PAuth pipeline coverage ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Verify authentication and call argument recovery through the CLI.
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"

#include "llvm/Support/FormatVariadic.h"

#include <fstream>
#include <string>

namespace {
#define NEVERD_PAUTH_PIPELINE_TEXT(Name, Text) constexpr char Name[] = Text;
#include "AArch64PAuthPipelineFixtures.def"
} // namespace

class AArch64_PAuth : public NeverDLiftTest {
protected:
  void expectPairedClangSyntax(const fs::path &CFile,
                               const std::string &Source) {
    auto syntax = checkHighCClangSyntax(
        CFile, {"-target", "aarch64-none-elf", "-ffreestanding",
                "-march=armv8.3-a+pauth", "-std=gnu11"});
    EXPECT_EQ(syntax.exitCode, 0) << syntax.err << "\n" << Source;
  }
};

static fs::path testObj() {
  return fs::path(TEST_OBJ_DIR) / "test_pauth_a64.o";
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

TEST_F(AArch64_PAuth, PacgaUsesGenericPointerAuthenticationIntrinsic) {
  auto r = liftToLLVMIR(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;
  auto F = functionIR(r.out, "test_pacga_a64");
  ASSERT_FALSE(F.empty()) << r.out;

  EXPECT_NE(F.find("@llvm.ptrauth.sign.generic"), std::string::npos) << F;
  EXPECT_EQ(F.find(" xor i64 "), std::string::npos) << F;
}

TEST_F(AArch64_PAuth, HighCUsesMachineInstructionAndCompiles) {
  auto r = decompileToHighC(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  auto cFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(cFile));
  std::ifstream input(cFile);
  ASSERT_TRUE(input.good());
  std::string source((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
  EXPECT_NE(source.find("pacga %0, %1, %2"), std::string::npos) << source;
  EXPECT_EQ(source.find("__neverd"), std::string::npos) << source;
  EXPECT_EQ(source.find(" ^ "), std::string::npos) << source;

  expectPairedClangSyntax(cFile, source);
}

TEST_F(AArch64_PAuth, CombinedLoadsRetainEarlierCallArguments) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << kMissingCompiler;
  for (const char *Load : {kLoadA, kLoadB, kLoadWritebackA, kLoadWritebackB}) {
    SCOPED_TRACE(Load);
    const auto Source = tmpFile(kSource);
    const auto Object = tmpFile(kObject);
    {
      std::ofstream Out(Source);
      ASSERT_TRUE(Out.good());
      Out << llvm::formatv(kCallAssembly, Load).str();
    }
    const auto Compiled = exec(
        NEVERD_TEST_CLANG, {kTargetOption, kTarget, kMarch, kCompileOption,
                            Source.string(), kOutputOption, Object.string()});
    ASSERT_EQ(Compiled.exitCode, 0) << Compiled.err;
    for (bool NoOptimization : {false, true}) {
      SCOPED_TRACE(NoOptimization);
      const auto Result = liftToLLVMIR(Object, {}, NoOptimization);
      ASSERT_EQ(Result.exitCode, 0) << Result.err;
      EXPECT_NE(Result.out.find(kPreparedCall), std::string::npos)
          << Result.out;
      EXPECT_EQ(Result.out.find(kUnknownValue), std::string::npos)
          << Result.out;
      EXPECT_NE(Result.out.find(kCombinedLoad), std::string::npos)
          << Result.out;
    }
  }
}
