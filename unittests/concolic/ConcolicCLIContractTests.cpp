//===- ConcolicCLIContractTests.cpp - concolic command contracts ---------===//

#include "../TestProcess.h"
#include "gtest/gtest.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#ifndef NEVERD_BINARY
#define NEVERD_BINARY "neverd"
#endif

#ifndef CONCOLIC_FIXTURE_ROOT
#define CONCOLIC_FIXTURE_ROOT ""
#endif

namespace {

namespace fs = std::filesystem;

class ConcolicCLI : public ::testing::Test {
protected:
  struct ProcessResult {
    int ExitCode = -1;
    std::string StandardOutput;
    std::string StandardError;
  };

  void SetUp() override {
    Directory =
        fs::temp_directory_path() /
        ("neverd-concolic-cli-" +
         std::to_string(neverd::test::currentProcessId()) + "-" +
         ::testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::create_directories(Directory);
  }

  void TearDown() override {
    std::error_code Error;
    fs::remove_all(Directory, Error);
  }

  ProcessResult run(llvm::StringRef Arguments) const {
    const fs::path StandardOutput = Directory / "stdout.json";
    const fs::path StandardError = Directory / "stderr.txt";
    const std::string Command =
        neverd::test::shellQuote(NEVERD_BINARY) + " " + Arguments.str() +
        neverd::test::redirectOutput(StandardOutput.string(),
                                     StandardError.string());
    ProcessResult Result;
    Result.ExitCode =
        neverd::test::systemExitCode(neverd::test::runShellCommand(Command));
    Result.StandardOutput = read(StandardOutput);
    Result.StandardError = read(StandardError);
    return Result;
  }

  static std::string fixture(llvm::StringRef Name = "lowir_concolic_elf_x64") {
    return (fs::path(CONCOLIC_FIXTURE_ROOT) / Name.str()).string();
  }

  static std::string
  command(llvm::StringRef Extra = {},
          llvm::StringRef Fixture = "lowir_concolic_elf_x64") {
    std::string Result = "concolic --func concolic_branch --seed arg0:4=0";
    if (!Extra.empty())
      Result += " " + Extra.str();
    Result += " " + neverd::test::shellQuote(fixture(Fixture));
    return Result;
  }

  static llvm::json::Object parseReport(const ProcessResult &Result) {
    llvm::Expected<llvm::json::Value> Parsed =
        llvm::json::parse(Result.StandardOutput);
    EXPECT_TRUE(static_cast<bool>(Parsed))
        << "stdout: " << Result.StandardOutput
        << "\nstderr: " << Result.StandardError;
    if (!Parsed)
      return {};
    const llvm::json::Object *Object = Parsed->getAsObject();
    EXPECT_NE(Object, nullptr);
    return Object ? std::move(*Object) : llvm::json::Object{};
  }

  static void expectError(const ProcessResult &Result,
                          llvm::StringRef ErrorCode = "invalid_arguments") {
    EXPECT_EQ(Result.ExitCode, 1);
    const llvm::json::Object Report = parseReport(Result);
    EXPECT_EQ(Report.getInteger("schema_version"), 1);
    EXPECT_EQ(Report.getString("adapter"), "lowir-concolic-v1");
    EXPECT_EQ(Report.getString("mode"), "concolic");
    EXPECT_EQ(Report.getBoolean("ok"), false);
    EXPECT_EQ(Report.getBoolean("exhaustive"), false);
    EXPECT_EQ(Report.getString("error_code"), ErrorCode);
    EXPECT_TRUE(Result.StandardError.empty());
  }

  fs::path outputPath() const { return Directory / "report.json"; }

  static std::string read(const fs::path &Path) {
    std::ifstream Stream(Path, std::ios::binary);
    return {std::istreambuf_iterator<char>(Stream),
            std::istreambuf_iterator<char>()};
  }

private:
  fs::path Directory;
};

TEST_F(ConcolicCLI, HelpAdvertisesTheDedicatedVersionedSurface) {
  const ProcessResult Result = run("concolic --help");
  EXPECT_EQ(Result.ExitCode, 0);
  EXPECT_TRUE(Result.StandardError.empty());
  EXPECT_NE(Result.StandardOutput.find("--func"), std::string::npos);
  EXPECT_NE(Result.StandardOutput.find("--seed"), std::string::npos);
  EXPECT_NE(Result.StandardOutput.find("--max-loop-iterations"),
            std::string::npos);
  EXPECT_NE(Result.StandardOutput.find("--solver-watch-visits"),
            std::string::npos);
  EXPECT_NE(Result.StandardOutput.find("--solver-gates"), std::string::npos);
}

TEST_F(ConcolicCLI, NameLookupAllBudgetsAndRepeatedRunsProduceOnlyStableJSON) {
  const std::string AllBudgets =
      "--max-steps=65536 --max-block-visits=3 --max-loop-iterations=3 "
      "--max-flip-attempts=64 --max-candidates=64 "
      "--solver-conflicts=262144 --solver-propagations=16777216 "
      "--solver-watch-visits=67108864 --solver-gates=4194304";
  const ProcessResult First = run(command(AllBudgets));
  const ProcessResult Second = run(command(AllBudgets));
  ASSERT_EQ(First.ExitCode, 0) << First.StandardError << First.StandardOutput;
  ASSERT_EQ(Second.ExitCode, 0)
      << Second.StandardError << Second.StandardOutput;
  EXPECT_TRUE(First.StandardError.empty());
  EXPECT_TRUE(Second.StandardError.empty());
  EXPECT_EQ(First.StandardOutput, Second.StandardOutput);
  ASSERT_FALSE(First.StandardOutput.empty());
  EXPECT_EQ(First.StandardOutput.front(), '{');
  EXPECT_EQ(First.StandardOutput.find("=== NeverD"), std::string::npos);

  const llvm::json::Object Report = parseReport(First);
  EXPECT_EQ(Report.getBoolean("ok"), true);
  EXPECT_EQ(Report.getBoolean("exhaustive"), false);
  EXPECT_EQ(Report.getBoolean("trace_complete"), true);
  EXPECT_EQ(Report.getBoolean("trace_exact"), true);
  const llvm::json::Object *Limits = Report.getObject("limits");
  ASSERT_NE(Limits, nullptr);
  EXPECT_EQ(Limits->getInteger("max_steps"), 65536);
  EXPECT_EQ(Limits->getInteger("max_block_visits"), 3);
  EXPECT_EQ(Limits->getInteger("max_loop_iterations"), 3);
  EXPECT_EQ(Limits->getInteger("max_flip_attempts"), 64);
  EXPECT_EQ(Limits->getInteger("max_candidates"), 64);
  EXPECT_EQ(Limits->getInteger("solver_max_conflicts"), 262144);
  EXPECT_EQ(Limits->getInteger("solver_max_propagations"), 16777216);
  EXPECT_EQ(Limits->getInteger("solver_max_watch_visits"), 67108864);
  EXPECT_EQ(Limits->getInteger("solver_max_gates"), 4194304);
}

TEST_F(ConcolicCLI, HexFunctionAndDecimalOrHexNumericSeedsAreEquivalent) {
  const ProcessResult Decimal = run("concolic --func 0x1240 --seed 56:4=0 " +
                                    neverd::test::shellQuote(fixture()));
  const ProcessResult Hex = run("concolic --func 1240 --seed 0x38:0x4=0x0 " +
                                neverd::test::shellQuote(fixture()));
  ASSERT_EQ(Decimal.ExitCode, 0) << Decimal.StandardOutput;
  ASSERT_EQ(Hex.ExitCode, 0) << Hex.StandardOutput;
  EXPECT_EQ(Decimal.StandardOutput, Hex.StandardOutput);
}

TEST_F(ConcolicCLI, ResolvesFullFirstArgumentAliasesForEveryNativeABI) {
  struct Case {
    const char *Fixture;
    const char *Alias;
  };
  const Case Cases[] = {
      {"lowir_concolic_elf_x64", "rdi"},    {"lowir_concolic_macho_x64", "rdi"},
      {"lowir_concolic_pe_x64", "rcx"},     {"lowir_concolic_elf_arm64", "x0"},
      {"lowir_concolic_macho_arm64", "x0"}, {"lowir_concolic_pe_arm64", "x0"},
  };
  for (const Case &Entry : Cases) {
    const ProcessResult Explicit = run(
        "concolic --func concolic_branch --seed " + std::string(Entry.Alias) +
        ":4=0 " + neverd::test::shellQuote(fixture(Entry.Fixture)));
    ASSERT_EQ(Explicit.ExitCode, 0)
        << Entry.Fixture << Explicit.StandardOutput << Explicit.StandardError;
    EXPECT_EQ(parseReport(Explicit).getBoolean("ok"), true);

    const ProcessResult Generic = run(command({}, Entry.Fixture));
    ASSERT_EQ(Generic.ExitCode, 0)
        << Entry.Fixture << Generic.StandardOutput << Generic.StandardError;
    EXPECT_EQ(parseReport(Generic).getBoolean("ok"), true);
  }
}

TEST_F(ConcolicCLI, RejectsMalformedOverflowingAndOutOfWidthSeeds) {
  const std::vector<std::string> BadSeeds = {
      "arg0=0",
      ":4=0",
      "arg0:0=0",
      "arg0:9=0",
      "arg0:1=256",
      "arg0:4=true",
      "18446744073709551616:1=0",
      "18446744073709551615:2=0",
  };
  for (const std::string &Seed : BadSeeds) {
    const ProcessResult Result = run("concolic --func concolic_branch --seed " +
                                     neverd::test::shellQuote(Seed) + " " +
                                     neverd::test::shellQuote(fixture()));
    expectError(Result);
  }
}

TEST_F(ConcolicCLI, RejectsPartialWrongOverlappingAndDuplicateRegisters) {
  const std::vector<std::string> Arguments = {
      "--seed edi:4=0",
      "--seed rcx:4=0",
      "--seed 56:8=0 --seed 60:4=0",
      "--seed arg0:4=0 --seed 56:4=0",
  };
  for (const std::string &Seeds : Arguments) {
    const ProcessResult Result =
        run("concolic --func concolic_branch " + Seeds + " " +
            neverd::test::shellQuote(fixture()));
    expectError(Result);
  }
}

TEST_F(ConcolicCLI, RejectsBooleanOverflowAndDuplicateScalarBudgets) {
  const std::vector<std::string> Arguments = {
      "--max-steps=true",
      "--max-block-visits=4294967296",
      "--max-loop-iterations=-1",
      "--max-flip-attempts=1 --max-flip-attempts=2",
      "--max-candidates=false",
      "--solver-conflicts=true",
      "--solver-propagations=18446744073709551616",
      "--solver-watch-visits=-1",
      "--solver-gates=1 --solver-gates=2",
  };
  for (const std::string &Argument : Arguments)
    expectError(run(command(Argument)));
}

TEST_F(ConcolicCLI, MissingNamedOptionValuesStayInVersionedJSONPath) {
  expectError(run("concolic --func --seed=arg0:4=0 " +
                  neverd::test::shellQuote(fixture())));
  expectError(run("concolic --func=concolic_branch --seed --max-steps=1 " +
                  neverd::test::shellQuote(fixture())));
  expectError(run("concolic --func=concolic_branch --max-steps "
                  "--seed=arg0:4=0 " +
                  neverd::test::shellQuote(fixture())));
}

TEST_F(ConcolicCLI, MachOOnlyRemovesOneABIUnderscoreFromFunctionNames) {
  const ProcessResult ELF =
      run("concolic --func=_concolic_branch --seed=arg0:4=0 " +
          neverd::test::shellQuote(fixture("lowir_concolic_elf_x64")));
  expectError(ELF, "function_not_found");

  const ProcessResult MachO =
      run("concolic --func=__concolic_branch --seed=arg0:4=0 " +
          neverd::test::shellQuote(fixture("lowir_concolic_macho_x64")));
  expectError(MachO, "function_not_found");
}

TEST_F(ConcolicCLI, HexadecimalLookingFunctionNamesWinBeforeAddressParsing) {
  const fs::path Renamed = outputPath().parent_path() / "hex-name-elf-x64";
  std::string Bytes = read(fixture());
  constexpr llvm::StringLiteral Original = "concolic_branch";
  constexpr llvm::StringLiteral RenamedSymbol = "face";
  size_t Replacements = 0;
  for (size_t Position = Bytes.find(Original.str());
       Position != std::string::npos;
       Position = Bytes.find(Original.str(), Position + Original.size())) {
    Bytes.replace(Position, Original.size(), Original.size(), '\0');
    Bytes.replace(Position, RenamedSymbol.size(), RenamedSymbol.data(),
                  RenamedSymbol.size());
    ++Replacements;
  }
  ASSERT_GT(Replacements, 0u);
  {
    std::ofstream Stream(Renamed, std::ios::binary);
    ASSERT_TRUE(Stream.is_open());
    Stream.write(Bytes.data(), static_cast<std::streamsize>(Bytes.size()));
    ASSERT_TRUE(Stream.good());
  }

  const ProcessResult Result = run("concolic --func=face --seed=arg0:4=0 " +
                                   neverd::test::shellQuote(Renamed.string()));
  ASSERT_EQ(Result.ExitCode, 0)
      << Result.StandardOutput << Result.StandardError;
  const llvm::json::Object Report = parseReport(Result);
  ASSERT_EQ(Report.getBoolean("ok"), true);
  const llvm::json::Object *Function = Report.getObject("function");
  ASSERT_NE(Function, nullptr);
  EXPECT_EQ(Function->getString("name"), "face");
}

TEST_F(ConcolicCLI, MissingInputsTargetsAndFilesUseVersionedErrorJSON) {
  expectError(run("concolic --func concolic_branch --seed arg0:4=0"));

  const ProcessResult MissingFile =
      run("concolic --func concolic_branch --seed arg0:4=0 " +
          neverd::test::shellQuote(
              (outputPath().parent_path() / "absent").string()));
  expectError(MissingFile, "load_error");

  const ProcessResult MissingFunction =
      run("concolic --func definitely_absent --seed arg0:4=0 " +
          neverd::test::shellQuote(fixture()));
  expectError(MissingFunction, "function_not_found");
}

TEST_F(ConcolicCLI, OutputFileReceivesTheOnlyJSONAndStdoutStaysEmpty) {
  const fs::path Output = outputPath();
  const ProcessResult Result =
      run(command("-o " + neverd::test::shellQuote(Output.string())));
  ASSERT_EQ(Result.ExitCode, 0)
      << Result.StandardOutput << Result.StandardError;
  EXPECT_TRUE(Result.StandardOutput.empty());
  EXPECT_TRUE(Result.StandardError.empty());
  const std::string FileReport = read(Output);
  ASSERT_FALSE(FileReport.empty());
  llvm::Expected<llvm::json::Value> Parsed = llvm::json::parse(FileReport);
  ASSERT_TRUE(static_cast<bool>(Parsed));
  ASSERT_NE(Parsed->getAsObject(), nullptr);
  EXPECT_EQ(Parsed->getAsObject()->getBoolean("ok"), true);
}

TEST_F(ConcolicCLI, SolverUnknownIsAValidReportAndExitsZero) {
  const ProcessResult Result = run(command("--solver-gates=1"));
  ASSERT_EQ(Result.ExitCode, 0)
      << Result.StandardOutput << Result.StandardError;
  const llvm::json::Object Report = parseReport(Result);
  EXPECT_EQ(Report.getBoolean("ok"), true);
  const llvm::json::Array *Flips = Report.getArray("flips");
  ASSERT_NE(Flips, nullptr);
  ASSERT_FALSE(Flips->empty());
  const llvm::json::Object *Flip = (*Flips)[0].getAsObject();
  ASSERT_NE(Flip, nullptr);
  EXPECT_EQ(Flip->getString("status"), "solver_unknown");
  EXPECT_EQ(Flip->getString("solver_status"), "unknown");
}

} // namespace
