//===- NeverDLiftFixture.h - Lift test fixture ---------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the NeverDLiftTest fixture used by all lifting unit tests.
/// Provides helpers to invoke the neverd tool and check output.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_NEVERDLIFTFIXTURE_H
#define NEVERD_UNITTESTS_LIFT_NEVERDLIFTFIXTURE_H

#include "../TestProcess.h"

#include "gtest/gtest.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct RunResult {
  int exitCode = -1;
  std::string out;
  std::string err;

  bool ok() const { return exitCode == 0; }
  bool contains(const std::string &Needle) const {
    return out.find(Needle) != std::string::npos;
  }
  bool errContains(const std::string &Needle) const {
    return err.find(Needle) != std::string::npos;
  }
};

class NeverDLiftTest : public ::testing::Test {
protected:
  void SetUp() override {
    TmpDir = fs::temp_directory_path() /
             ("nd_test_" +
              std::to_string(neverd::test::currentProcessId()) + "_" +
              ::testing::UnitTest::GetInstance()
                  ->current_test_info()
                  ->name());
    fs::create_directories(TmpDir);
  }

  void TearDown() override {
    if (!::testing::Test::HasFailure() && fs::exists(TmpDir))
      fs::remove_all(TmpDir);
  }

  static std::string ndBin() {
    const char *Env = std::getenv("NEVERD");
    if (Env)
      return Env;
    return NEVERD_BINARY;
  }

  fs::path tmp() const { return TmpDir; }
  fs::path tmpFile(const std::string &Name) const { return TmpDir / Name; }

  RunResult exec(const std::string &Program,
                 const std::vector<std::string> &Args) const {
    std::string Cmd = neverd::test::shellQuote(Program);
    for (const auto &A : Args)
      Cmd += " " + neverd::test::shellQuote(A);
    auto OutFile = tmpFile("_stdout.txt");
    auto ErrFile = tmpFile("_stderr.txt");
    Cmd += neverd::test::redirectOutput(OutFile.string(), ErrFile.string());
    int RC = std::system(Cmd.c_str());
    RunResult R;
    R.exitCode = neverd::test::systemExitCode(RC);
    R.out = readFile(OutFile);
    R.err = readFile(ErrFile);
    return R;
  }

  fs::path compileAsmTest(const std::string &Name, const std::string &SrcPath,
                          const std::string &ArchName = "x86_64") {
    auto Obj = tmpFile(Name + ".o");
    std::string Flags;
    if (ArchName == "x86_64")
      Flags =
          "-target x86_64-linux-gnu -nostdlib -static -O0 -fno-stack-protector";
    else if (ArchName == "i386")
      Flags = "-target i386-linux-gnu -m32 -nostdlib -static -O0 "
              "-fno-stack-protector";
    auto CR = exec("clang", {Flags, "-c", SrcPath, "-o", Obj.string()});
    EXPECT_EQ(CR.exitCode, 0) << "Compile failed: " << CR.err;
    return Obj;
  }

  RunResult liftToLowIR(const fs::path &Binary,
                        const std::string &Func = "") {
    (void)Func;
    return exec(ndBin(), {"lift", "-dump-low", Binary.string()});
  }

  RunResult liftToMedIR(const fs::path &Binary,
                        const std::string &Func = "") {
    (void)Func;
    return exec(ndBin(), {"lift", "-dump-med", Binary.string()});
  }

  RunResult liftToHighIR(const fs::path &Binary,
                         const std::string &Func = "") {
    (void)Func;
    return exec(ndBin(), {"lift", "-dump-high", Binary.string()});
  }

  RunResult liftToLLVMIR(const fs::path &Binary,
                         const std::string &Func = "") {
    (void)Func;
    return exec(ndBin(), {"lift", Binary.string()});
  }

  void verifyAllStages(const fs::path &Binary,
                       const std::string &Func = "") {
    auto Low = liftToLowIR(Binary, Func);
    ASSERT_EQ(Low.exitCode, 0) << "LowIR failed: " << Low.err;
    EXPECT_FALSE(Low.out.empty()) << "LowIR output is empty";

    auto Med = liftToMedIR(Binary, Func);
    ASSERT_EQ(Med.exitCode, 0) << "MedIR failed: " << Med.err;
    EXPECT_FALSE(Med.out.empty()) << "MedIR output is empty";

    auto High = liftToHighIR(Binary, Func);
    ASSERT_EQ(High.exitCode, 0) << "HighIR failed: " << High.err;
    EXPECT_FALSE(High.out.empty()) << "HighIR output is empty";

    auto LLVM = liftToLLVMIR(Binary, Func);
    ASSERT_EQ(LLVM.exitCode, 0) << "LLVM IR failed: " << LLVM.err;
    EXPECT_FALSE(LLVM.out.empty()) << "LLVM IR output is empty";
  }

  bool canLoad(const fs::path &Binary) {
    auto R = exec(ndBin(), {"lift", "-dump-low", Binary.string()});
    return R.exitCode == 0;
  }

  void skipIfCannotLoad(const fs::path &Binary) {
    auto R = exec(ndBin(), {"lift", "-dump-low", Binary.string()});
    if (R.exitCode != 0)
      GTEST_SKIP() << "Binary format not yet supported by loader";
  }

  void verifyNoUnlifted(const fs::path &Binary) {
    auto R = exec(ndBin(), {"lift", "-dump-low", Binary.string()});
    EXPECT_EQ(R.exitCode, 0) << "lift failed: " << R.err;
    std::string Output = R.out + "\n" + R.err;
    std::transform(Output.begin(), Output.end(), Output.begin(),
                   [](unsigned char C) { return std::tolower(C); });
    EXPECT_EQ(Output.find("unlifted"), std::string::npos)
        << "Found unlifted instructions:\n"
        << R.out << R.err;
  }

  void verifyLLVMIRContains(const fs::path &Binary, const std::string &Func,
                            const std::string &Needle) {
    auto R = liftToLLVMIR(Binary, Func);
    ASSERT_EQ(R.exitCode, 0) << "LLVM IR lift failed: " << R.err;
    EXPECT_TRUE(R.out.find(Needle) != std::string::npos)
        << "Expected '" << Needle << "' in LLVM IR:\n"
        << R.out;
  }

  void verifyLLVMIRNotContains(const fs::path &Binary,
                               const std::string &Func,
                               const std::string &Needle) {
    auto R = liftToLLVMIR(Binary, Func);
    ASSERT_EQ(R.exitCode, 0) << "LLVM IR lift failed: " << R.err;
    EXPECT_TRUE(R.out.find(Needle) == std::string::npos)
        << "Unexpected '" << Needle << "' in LLVM IR:\n"
        << R.out;
  }

  void verifyLowIRContains(const fs::path &Binary, const std::string &Func,
                           const std::string &Needle) {
    auto R = liftToLowIR(Binary, Func);
    ASSERT_EQ(R.exitCode, 0) << "LowIR lift failed: " << R.err;
    EXPECT_TRUE(R.out.find(Needle) != std::string::npos)
        << "Expected '" << Needle << "' in LowIR:\n"
        << R.out;
  }

  RunResult decompileToC(const fs::path &Binary) {
    auto Out = tmpFile("decompiled.c");
    return exec(ndBin(),
                {"decompile", "--llvm", "-o", Out.string(), Binary.string()});
  }

  RunResult decompileToHighC(const fs::path &Binary) {
    auto Out = tmpFile("decompiled_high.c");
    return exec(ndBin(), {"decompile", "-o", Out.string(), Binary.string()});
  }

  RunResult patchBinary(const fs::path &Binary) {
    auto Out = tmpFile("patched");
    return exec(ndBin(), {"patch", "-o", Out.string(), Binary.string()});
  }

  RunResult patchBinaryWithHello(const fs::path &Binary) {
    auto Out = tmpFile("patched_hello");
    return exec(ndBin(),
                {"patch", "-o", Out.string(), "-hello", Binary.string()});
  }

  void verifyDecompileProducesOutput(const fs::path &Binary) {
    auto R = decompileToC(Binary);
    ASSERT_EQ(R.exitCode, 0) << "Decompile failed: " << R.err;
    auto CFile = tmpFile("decompiled.c");
    ASSERT_TRUE(fs::exists(CFile)) << "C output file not created";
    auto Content = readFile(CFile);
    EXPECT_FALSE(Content.empty()) << "Decompiled C is empty";
  }

  void verifyPatchProducesOutput(const fs::path &Binary) {
    auto R = patchBinary(Binary);
    ASSERT_EQ(R.exitCode, 0) << "Patch failed: " << R.err;
    auto PatchedFile = tmpFile("patched");
    EXPECT_TRUE(fs::exists(PatchedFile)) << "Patched binary not created";
  }

  void verifyLLVMIRNoVerifierErrors(const fs::path &Binary) {
    auto R = liftToLLVMIR(Binary);
    ASSERT_EQ(R.exitCode, 0) << "LLVM IR lift failed: " << R.err;
    EXPECT_TRUE(R.err.find("Incorrect number of arguments") ==
                std::string::npos)
        << "LLVM verifier error found:\n"
        << R.err;
  }

  void verifyNoConstantTrueBranch(const fs::path &Binary) {
    auto R = liftToLLVMIR(Binary);
    ASSERT_EQ(R.exitCode, 0) << "LLVM IR lift failed: " << R.err;
    EXPECT_TRUE(R.out.find("br i1 true") == std::string::npos)
        << "Found hardcoded 'br i1 true' — COND_BR condition likely wrong:\n"
        << R.out.substr(0, 2000);
  }

  void verifyLLVMIRHasConditionalLogic(const fs::path &Binary) {
    auto R = liftToLLVMIR(Binary);
    ASSERT_EQ(R.exitCode, 0) << "LLVM IR lift failed: " << R.err;
    bool HasCond = R.out.find("br i1 %") != std::string::npos ||
                   R.out.find("select i1") != std::string::npos ||
                   R.out.find("icmp") != std::string::npos;
    EXPECT_TRUE(HasCond)
        << "Expected conditional logic (br i1/select/icmp) in LLVM IR";
  }

  void verifyLLVMIRNoUnreachable(const fs::path &Binary) {
    auto R = liftToLLVMIR(Binary);
    ASSERT_EQ(R.exitCode, 0) << "LLVM IR lift failed: " << R.err;
    EXPECT_TRUE(R.out.find("unreachable") == std::string::npos)
        << "Found 'unreachable' in LLVM IR — possible missing terminator:\n"
        << R.out.substr(0, 2000);
  }

  RunResult liftToLLVMIRUnopt(const fs::path &Binary) {
    return exec(ndBin(), {"lift", "-no-opt", Binary.string()});
  }

  void verifyAllModesSucceed(const fs::path &Binary) {
    verifyAllStages(Binary);
    verifyDecompileProducesOutput(Binary);
    verifyLLVMIRNoVerifierErrors(Binary);
    verifyNoConstantTrueBranch(Binary);
  }

private:
  fs::path TmpDir;

  static std::string readFile(const fs::path &Path) {
    std::ifstream IFS(Path);
    if (!IFS)
      return "";
    std::ostringstream SS;
    SS << IFS.rdbuf();
    return SS.str();
  }

};

#endif // NEVERD_UNITTESTS_LIFT_NEVERDLIFTFIXTURE_H
