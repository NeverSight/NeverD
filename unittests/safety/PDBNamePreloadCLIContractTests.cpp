//===- PDBNamePreloadCLIContractTests.cpp - PE name hint contract ---------===//

#include "../TestProcess.h"
#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPI.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

namespace fs = std::filesystem;

const fs::path FixtureRoot = SAFETY_FIXTURE_ROOT;
const fs::path PE = FixtureRoot / "safety_cases_pe_x64.exe";
const fs::path PDB = FixtureRoot / "safety_cases_pe_x64.pdb";
constexpr neverd_va_t FunctionVA = 0x140001000;
constexpr const char *FunctionName = "tainted_stack_overflow";

std::string readFile(const fs::path &Path) {
  std::ifstream Stream(Path, std::ios::binary);
  return {std::istreambuf_iterator<char>(Stream),
          std::istreambuf_iterator<char>()};
}

struct CLIResult {
  int ExitCode = -1;
  std::string Output;
  std::string Error;
};

CLIResult runCLI(const std::string &Arguments, const fs::path &Directory) {
  const fs::path Out = Directory / "stdout.txt";
  const fs::path Err = Directory / "stderr.txt";
  const std::string Command =
      neverd::test::shellQuote(NEVERD_BINARY) + " " + Arguments +
      neverd::test::redirectOutput(Out.string(), Err.string());
  const int ExitCode =
      neverd::test::systemExitCode(neverd::test::runShellCommand(Command));
  return {ExitCode, readFile(Out), readFile(Err)};
}

TEST(PDBNamePreload, CAPIReturnsOnlyAuthenticatedExactPublicName) {
  neverd_session_t Session = neverd_session_create();
  ASSERT_NE(Session, nullptr);
  neverd_session_set_pdb_path(Session, PDB.string().c_str());

  EXPECT_EQ(neverd_session_resolve_function_name_before_load(
                Session, PE.string().c_str(), FunctionName),
            FunctionVA);
  EXPECT_EQ(neverd_session_resolve_function_name_before_load(
                Session, PE.string().c_str(), "tainted_stack_overflow_extra"),
            0u);

  const fs::path WrongPDB = FixtureRoot / "safety_cases_pe_arm64.pdb";
  neverd_session_set_pdb_path(Session, WrongPDB.string().c_str());
  EXPECT_EQ(neverd_session_resolve_function_name_before_load(
                Session, PE.string().c_str(), FunctionName),
            0u);
  neverd_session_set_pdb_path(Session, nullptr);
  EXPECT_EQ(neverd_session_resolve_function_name_before_load(
                Session, PE.string().c_str(), FunctionName),
            0u);
  neverd_session_set_pdb_path(Session, PDB.string().c_str());
  neverd_session_set_debug_info_enabled(Session, 0);
  EXPECT_EQ(neverd_session_resolve_function_name_before_load(
                Session, PE.string().c_str(), FunctionName),
            0u);

  neverd_session_destroy(Session);
}

TEST(PDBNamePreload, CLINameMatchesAddressAndMissingNameFailsLoudly) {
  const fs::path Directory =
      fs::temp_directory_path() /
      ("neverd-pdb-name-" + std::to_string(neverd::test::currentProcessId()));
  fs::create_directories(Directory);
  const fs::path ByName = Directory / "by-name.c";
  const fs::path ByAddress = Directory / "by-address.c";
  const std::string Common =
      "decompile --pdb=" + neverd::test::shellQuote(PDB.string()) + " " +
      neverd::test::shellQuote(PE.string());

  const CLIResult Named = runCLI(Common + " --func=" + FunctionName + " -o " +
                                     neverd::test::shellQuote(ByName.string()),
                                 Directory);
  ASSERT_EQ(Named.ExitCode, 0) << Named.Error;
  const CLIResult Addressed =
      runCLI(Common + " --func=0x140001000 -o " +
                 neverd::test::shellQuote(ByAddress.string()),
             Directory);
  ASSERT_EQ(Addressed.ExitCode, 0) << Addressed.Error;
  ASSERT_FALSE(readFile(ByName).empty());
  EXPECT_EQ(readFile(ByName), readFile(ByAddress));

  const CLIResult Missing =
      runCLI(Common + " --func=tainted_stack_overflow_extra", Directory);
  EXPECT_NE(Missing.ExitCode, 0);
  EXPECT_NE(Missing.Error.find("function not found"), std::string::npos);

  const fs::path WrongPDB = FixtureRoot / "safety_cases_pe_arm64.pdb";
  const CLIResult Mismatch =
      runCLI("decompile --pdb=" + neverd::test::shellQuote(WrongPDB.string()) +
                 " --func=" + FunctionName + " " +
                 neverd::test::shellQuote(PE.string()),
             Directory);
  EXPECT_NE(Mismatch.ExitCode, 0);
  EXPECT_NE(Mismatch.Error.find("failed to load"), std::string::npos);
  EXPECT_NE(Mismatch.Error.find("does not match"), std::string::npos);

  std::error_code Error;
  fs::remove_all(Directory, Error);
}

} // namespace
