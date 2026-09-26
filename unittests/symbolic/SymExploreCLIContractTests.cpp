//===- SymExploreCLIContractTests.cpp - Recovered entry lookup -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../TestProcess.h"
#include "gtest/gtest.h"

#include "llvm/Support/JSON.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

namespace fs = std::filesystem;

std::string readFile(const fs::path &Path) {
  std::ifstream Stream(Path, std::ios::binary);
  return {std::istreambuf_iterator<char>(Stream),
          std::istreambuf_iterator<char>()};
}

struct Result {
  int ExitCode = -1;
  std::string Output;
  std::string Error;
};

Result explore(const char *Function, const fs::path &Directory) {
  const fs::path Fixture =
      fs::path(SYMBOLIC_FIXTURE_ROOT) / "entry_notype_elf_x64";
  const fs::path Out = Directory / "stdout.json";
  const fs::path Err = Directory / "stderr.txt";
  const std::string Command =
      neverd::test::shellQuote(NEVERD_BINARY) + " sym-explore --func=" +
      neverd::test::shellQuote(Function) + " " +
      neverd::test::shellQuote(Fixture.string()) +
      neverd::test::redirectOutput(Out.string(), Err.string());
  const int ExitCode =
      neverd::test::systemExitCode(neverd::test::runShellCommand(Command));
  return {ExitCode, readFile(Out), readFile(Err)};
}

TEST(SymExploreCLI, FindsRecoveredNotypeEntryByAddressAndName) {
  // This checked-in ELF has an executable entry at 0x401000 and only a NOTYPE
  // _start symbol. Loader-only function queries see no STT_FUNC; the pipeline
  // recovers the function before sym-explore resolves --func.
  const fs::path Directory =
      fs::temp_directory_path() /
      ("neverd-sym-entry-" +
       std::to_string(neverd::test::currentProcessId()));
  fs::create_directories(Directory);
  const Result Address = explore("0x401000", Directory);
  const Result Name = explore("_start", Directory);
  ASSERT_EQ(Address.ExitCode, 0) << Address.Error;
  ASSERT_EQ(Name.ExitCode, 0) << Name.Error;
  EXPECT_EQ(Address.Output, Name.Output);

  auto Parsed = llvm::json::parse(Address.Output);
  ASSERT_TRUE(static_cast<bool>(Parsed));
  const llvm::json::Object *Report = Parsed->getAsObject();
  ASSERT_NE(Report, nullptr);
  EXPECT_EQ(Report->getBoolean("ok"), true);
  EXPECT_EQ(Report->getBoolean("exact"), true);
  EXPECT_EQ(Report->getString("entry"), "0x401000");
  EXPECT_EQ(Report->getInteger("reachablePaths"), 1);

  std::error_code Error;
  fs::remove_all(Directory, Error);
}

} // namespace
