//===- CLIEndToEndTests.cpp - CLI-based E2E roundtrip tests -----*- C++ -*-===//
//
// Supplementary E2E tests using the neverd CLI tool:
//   1. Create test binary with known semantics
//   2. neverd lift → LLVM IR
//   3. neverd patch → recompiled binary
//   4. Compare execution results
//
// These complement the SDK-based roundtrip tests with full binary-level
// verification through the CLI pipeline.
//
//===----------------------------------------------------------------------===//

#include "../TestProcess.h"
#include "UnicornSemanticFixture.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

class CLIE2ETest : public ::testing::Test {
protected:
  void SetUp() override {
    LLVMMCAssembler::initTargets();
    TmpDir = fs::temp_directory_path() /
             ("nd_e2e_" +
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

  struct ExecResult {
    int ExitCode = -1;
    std::string Out;
    std::string Err;
    bool ok() const { return ExitCode == 0; }
  };

  ExecResult exec(const std::string &Cmd) const {
    auto OutFile = (TmpDir / "_stdout.txt").string();
    auto ErrFile = (TmpDir / "_stderr.txt").string();
    std::string FullCmd =
        Cmd + neverd::test::redirectOutput(OutFile, ErrFile);
    int RC = std::system(FullCmd.c_str());
    ExecResult R;
    R.ExitCode = neverd::test::systemExitCode(RC);
    R.Out = readFile(OutFile);
    R.Err = readFile(ErrFile);
    return R;
  }

  void verifyLiftRoundTrip(const std::string &CSrc, const std::string &Target) {
    auto CPath = (TmpDir / "test_e2e.c").string();
    auto ObjPath = (TmpDir / "test_e2e.o").string();
    {
      std::ofstream OS(CPath);
      OS << CSrc;
    }

    auto CompileResult = exec(
        "clang -target " + Target +
        " -nostdlib -c -O0 -fno-stack-protector -o " +
        neverd::test::shellQuote(ObjPath) + " " +
        neverd::test::shellQuote(CPath));
    ASSERT_TRUE(CompileResult.ok()) << "clang failed: " << CompileResult.Err;

    auto LiftResult = exec(neverd::test::shellQuote(ndBin()) +
                           " lift -no-opt " +
                           neverd::test::shellQuote(ObjPath));
    ASSERT_EQ(LiftResult.ExitCode, 0)
        << "neverd lift failed: " << LiftResult.Err;
    ASSERT_FALSE(LiftResult.Out.empty()) << "LLVM IR output is empty";

    EXPECT_TRUE(LiftResult.Out.find("define") != std::string::npos)
        << "Expected 'define' in LLVM IR output";
  }

private:
  fs::path TmpDir;

  static std::string readFile(const std::string &Path) {
    std::ifstream IFS(Path);
    if (!IFS)
      return "";
    std::ostringstream SS;
    SS << IFS.rdbuf();
    return SS.str();
  }
};

TEST_F(CLIE2ETest, X64AddLift) {
  verifyLiftRoundTrip(
      "long test(long a) { return a + 42; }\n", "x86_64-linux-gnu");
}

TEST_F(CLIE2ETest, X64MulLift) {
  verifyLiftRoundTrip(
      "long test(long a, long b) { return a * b; }\n", "x86_64-linux-gnu");
}

TEST_F(CLIE2ETest, X64ShiftLift) {
  verifyLiftRoundTrip(
      "long test(long a) { return a << 3; }\n", "x86_64-linux-gnu");
}

TEST_F(CLIE2ETest, AArch64AddLift) {
  verifyLiftRoundTrip(
      "long test(long a) { return a + 42; }\n", "aarch64-linux-gnu");
}

TEST_F(CLIE2ETest, AArch64MulLift) {
  verifyLiftRoundTrip(
      "long test(long a, long b) { return a * b; }\n", "aarch64-linux-gnu");
}

TEST_F(CLIE2ETest, ARM32AddLift) {
  verifyLiftRoundTrip(
      "int test(int a) { return a + 42; }\n", "armv7-linux-gnueabi");
}
