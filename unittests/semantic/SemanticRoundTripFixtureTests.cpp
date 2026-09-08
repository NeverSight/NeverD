//===- SemanticRoundTripFixtureTests.cpp - Differential harness integrity
//---===//
//
// NeverD Decompiler — Semantic Roundtrip Verification Tests
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"
#include "gtest/gtest-spi.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Program.h"

#include <cstdlib>
#include <optional>
#ifdef __APPLE__
#include <crt_externs.h>
#elif !defined(_WIN32)
extern char **environ;
#endif

namespace {

class SemanticRoundTripIntegrity : public SemanticRoundTripFixture {
protected:
  enum class Fault { None, LiftFailure, NoFunctions, DataBearingReplacement };
  Fault InjectedFault = Fault::None;
  bool LiftCalled = false;
  bool FunctionCountCalled = false;

  int liftToObject(neverd_session_t Session, const char *ObjectPath,
                   int NoOpt) override {
    LiftCalled = true;
    if (InjectedFault == Fault::DataBearingReplacement) {
      // The original returns 42 without data. An equivalent function whose
      // result comes from a table makes only the recompiled artifact need a
      // link, after the fixture has already executed the original.
      const std::string SourcePath = std::string(ObjectPath) + ".replacement.c";
      const std::string Replacement =
          std::string(ObjectPath) + ".replacement.o";
      std::ofstream Source(SourcePath);
      Source << "static volatile unsigned values[4] = {42,42,42,42}; "
                "unsigned fixture_link_constant(unsigned a) { "
                "return values[a & 3]; }";
      Source.close();
      if (!Source)
        return 1;
      const std::string Command =
          "clang -target x86_64-linux-gnu -nostdlib -c -O1 "
          "-fno-stack-protector -fno-exceptions -fno-unwind-tables "
          "-fno-asynchronous-unwind-tables -o " +
          neverd::test::shellQuote(Replacement) + " " +
          neverd::test::shellQuote(SourcePath);
      if (neverd::test::systemExitCode(neverd::test::runShellCommand(Command)))
        return 1;
      return SemanticRoundTripFixture::liftToObject(Session,
                                                    Replacement.c_str(), NoOpt);
    }
    if (InjectedFault == Fault::LiftFailure) {
      // Use a real SDK failure, including its native diagnostic. The original
      // object has already compiled and executed before this boundary is hit.
      const std::string MissingPath = std::string(ObjectPath) + ".missing";
      return SemanticRoundTripFixture::liftToObject(Session,
                                                    MissingPath.c_str(), NoOpt);
    }
    return SemanticRoundTripFixture::liftToObject(Session, ObjectPath, NoOpt);
  }

  int roundTripFunctionCount(neverd_session_t Session) override {
    FunctionCountCalled = true;
    if (InjectedFault == Fault::NoFunctions)
      return 0;
    return SemanticRoundTripFixture::roundTripFunctionCount(Session);
  }

  static RoundTripTC sample() {
    RoundTripTC TC;
    TC.Name = "fixture_add";
    TC.CSrc = "unsigned int fixture_add(unsigned int a, unsigned int b) {"
              "  return a + b;"
              "}";
    TC.Args = {17, 25};
    TC.Category = "fixture_integrity";
    TC.OptLevel = 1;
    return TC;
  }

  static RoundTripTC dataSample() {
    RoundTripTC TC;
    TC.Name = "fixture_link_data";
    TC.CSrc = "static volatile unsigned values[4] = {11,29,43,61}; "
              "unsigned fixture_link_data(unsigned a) { "
              "return values[a & 3] + 7; }";
    TC.Args = {2};
    TC.OptLevel = 1;
    return TC;
  }

  void expectFatalFailure(Fault Failure, const char *Diagnostic) {
    InjectedFault = Failure;
    ::testing::TestPartResultArray Results;
    {
      ::testing::ScopedFakeTestPartResultReporter Reporter(
          ::testing::ScopedFakeTestPartResultReporter::
              INTERCEPT_ONLY_CURRENT_THREAD,
          &Results);
      roundTripX64(sample());
    }

    if (!LiftCalled && Results.size() == 1 &&
        Results.GetTestPartResult(0).skipped())
      GTEST_SKIP() << Results.GetTestPartResult(0).message();

    ASSERT_TRUE(LiftCalled)
        << "fixture preparation did not reach the SDK: "
        << (Results.size() ? Results.GetTestPartResult(0).message()
                           : "no diagnostic");
    ASSERT_EQ(Results.size(), 1);
    const auto &Result = Results.GetTestPartResult(0);
    EXPECT_EQ(Result.type(), ::testing::TestPartResult::kFatalFailure)
        << Result.message();
    EXPECT_NE(std::string(Result.message()).find(Diagnostic), std::string::npos)
        << Result.message();
    EXPECT_NE(std::string(Result.message()).find("fixture_add"),
              std::string::npos)
        << Result.message();
    EXPECT_EQ(FunctionCountCalled, Failure == Fault::NoFunctions);
  }
};

TEST_F(SemanticRoundTripIntegrity,
       LiftFailureIsFatalAndRetainsNativeDiagnostic) {
  expectFatalFailure(Fault::LiftFailure, ".missing");
}

TEST_F(SemanticRoundTripIntegrity, ZeroRecoveredFunctionsIsFatal) {
  expectFatalFailure(Fault::NoFunctions, "No functions in roundtrip result");
}

TEST_F(SemanticRoundTripIntegrity, X64StillComparesExecutedRoundtrip) {
  roundTripX64(sample());
  if (IsSkipped() || HasFatalFailure())
    return;
  EXPECT_TRUE(FunctionCountCalled);
}

TEST_F(SemanticRoundTripIntegrity, X86StillComparesExecutedRoundtrip) {
  roundTripX86(sample());
  if (IsSkipped() || HasFatalFailure())
    return;
  EXPECT_TRUE(FunctionCountCalled);
}

TEST_F(SemanticRoundTripIntegrity, AArch64StillComparesExecutedRoundtrip) {
  roundTripAArch64(sample());
  if (IsSkipped() || HasFatalFailure())
    return;
  EXPECT_TRUE(FunctionCountCalled);
}

TEST_F(SemanticRoundTripIntegrity, ARM32StillComparesExecutedRoundtrip) {
  roundTripARM32(sample());
  if (IsSkipped() || HasFatalFailure())
    return;
  EXPECT_TRUE(FunctionCountCalled);
}

TEST_F(SemanticRoundTripIntegrity, DataBearingRoundtripLinksBothArtifacts) {
  roundTripX64(dataSample());
}

TEST_F(SemanticRoundTripIntegrity, OnlyRecompiledArtifactNeedsLinking) {
  InjectedFault = Fault::DataBearingReplacement;
  RoundTripTC TC;
  TC.Name = "fixture_link_constant";
  TC.CSrc = "unsigned fixture_link_constant(unsigned a) { return 42; }";
  TC.Args = {2};
  TC.OptLevel = 1;
  roundTripX64(TC);
}

std::vector<std::string> childEnvironment(const fs::path &ToolDirectory,
                                          const std::string &Compiler,
                                          const std::string &Linker,
                                          const std::string &Failure) {
#ifdef __APPLE__
  char **Environment = *_NSGetEnviron();
#elif defined(_WIN32)
  char **Environment = _environ;
#else
  char **Environment = environ;
#endif
  std::vector<std::string> Result;
  std::string SearchPath = ToolDirectory.string();
  for (char **Entry = Environment; Entry && *Entry; ++Entry) {
    llvm::StringRef Value(*Entry);
    const auto [Key, Text] = Value.split('=');
    if (Key.equals_insensitive("PATH")) {
      llvm::SmallVector<llvm::StringRef> Parts;
      Text.split(Parts, llvm::sys::EnvPathSeparator, -1, false);
      // Keep runtime-library/system-tool paths. Exclude every directory that
      // could supply a real linker so a fresh child's missing-tool test is
      // independent of the developer's installed LLVM distributions.
      for (llvm::StringRef Part : Parts) {
        if (!llvm::sys::findProgramByName("ld.lld", {Part}))
          SearchPath += llvm::sys::EnvPathSeparator + Part.str();
      }
    } else if (!Key.starts_with("GTEST_") &&
               !Key.starts_with("NEVERD_FIXTURE_")) {
      Result.emplace_back(*Entry);
    }
  }
  Result.push_back("PATH=" + SearchPath);
  Result.push_back("NEVERD_FIXTURE_REAL_CLANG=" + Compiler);
  Result.push_back("NEVERD_FIXTURE_REAL_LLD=" + Linker);
  Result.push_back("NEVERD_FIXTURE_LINK_FAILURE=" + Failure);
  return Result;
}

void expectChildLinkOutcome(const char *Case, const char *Failure, bool Missing,
                            const char *Stage) {
  const auto Compiler = llvm::sys::findProgramByName("clang");
  if (!Compiler)
    GTEST_SKIP() << "clang unavailable for fixture process regression";
  const auto Linker = llvm::sys::findProgramByName("ld.lld");
  if (!Missing && !Linker)
    GTEST_SKIP() << "ld.lld unavailable for real linker regression";

  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(
      (fs::temp_directory_path() / "neverd fixture link outcome").string(),
      Directory));
  struct Cleanup {
    fs::path Path;
    ~Cleanup() {
      std::error_code Error;
      fs::remove_all(Path, Error);
    }
  } Guard{Directory.str().str()};
  const fs::path Work = Guard.Path;
  const fs::path Tools = Work / "tools";
  fs::create_directory(Tools);
  const std::string Suffix = neverd::test::executableSuffix();
  fs::copy_file(NEVERD_FIXTURE_TOOL_PROXY, Tools / ("clang" + Suffix));
  if (!Missing)
    fs::copy_file(NEVERD_FIXTURE_TOOL_PROXY, Tools / ("ld.lld" + Suffix));
  const std::string Executable =
      llvm::sys::fs::getMainExecutable("NeverDSemanticFixtureTests", nullptr);
  ASSERT_FALSE(Executable.empty());
  const std::string XML = (Work / "child.xml").string();
  const std::string Out = (Work / "stdout.txt").string();
  const std::string Err = (Work / "stderr.txt").string();
  const std::vector<std::string> Environment =
      childEnvironment(Tools, *Compiler, Linker ? *Linker : "", Failure);
  std::vector<llvm::StringRef> Env;
  for (const std::string &Value : Environment)
    Env.push_back(Value);
  const std::string Filter =
      std::string("--gtest_filter=SemanticRoundTripIntegrity.") + Case;
  const std::string XMLArgument = "--gtest_output=xml:" + XML;
  const std::optional<llvm::StringRef> Redirects[] = {
      llvm::StringRef(""), llvm::StringRef(Out), llvm::StringRef(Err)};
  std::string ExecutionError;
  const int Status = llvm::sys::ExecuteAndWait(
      Executable, {Executable, Filter, XMLArgument, "--gtest_repeat=1"},
      llvm::ArrayRef<llvm::StringRef>(Env), Redirects, 45, 0, &ExecutionError);
  auto Read = [](const std::string &Path) {
    std::ifstream File(Path);
    return std::string(std::istreambuf_iterator<char>(File), {});
  };
  const std::string Stdout = Read(Out), Stderr = Read(Err), Report = Read(XML);
  SCOPED_TRACE(Stdout + Stderr + ExecutionError);
  ASSERT_FALSE(Report.empty());
  EXPECT_EQ(Status, Missing ? 0 : 1);
  if (Missing) {
    EXPECT_NE(Report.find("<skipped"), std::string::npos);
    EXPECT_EQ(Report.find("<failure"), std::string::npos);
    EXPECT_NE(Stdout.find(Stage), std::string::npos);
    EXPECT_NE(Stdout.find("ld.lld"), std::string::npos);
    EXPECT_NE(Stdout.find("unavailable"), std::string::npos);
  } else {
    EXPECT_NE(Report.find("<failure"), std::string::npos);
    EXPECT_EQ(Report.find("<skipped"), std::string::npos);
    EXPECT_NE(Stdout.find(Stage), std::string::npos);
    EXPECT_NE(Stdout.find("Exit status: 1"), std::string::npos);
    EXPECT_NE(Stdout.find("fixture-invalid-object"), std::string::npos);
    EXPECT_NE(Stdout.find("error:"), std::string::npos);
  }
}

TEST(SemanticLinkExecution, RejectedOriginalObjectIsFatalWithLinkerDiagnostic) {
  expectChildLinkOutcome("DataBearingRoundtripLinksBothArtifacts", "original",
                         false, "original link failed");
}

TEST(SemanticLinkExecution,
     RejectedRecompiledObjectIsFatalWithLinkerDiagnostic) {
  expectChildLinkOutcome("DataBearingRoundtripLinksBothArtifacts", "recompiled",
                         false, "recompiled object preparation failed");
}

TEST(SemanticLinkExecution, MissingLinkerIsSkippedAtEitherRequiredLink) {
  expectChildLinkOutcome("DataBearingRoundtripLinksBothArtifacts", "none", true,
                         "original link unavailable");
  expectChildLinkOutcome("OnlyRecompiledArtifactNeedsLinking", "none", true,
                         "recompiled link unavailable");
}

} // namespace
