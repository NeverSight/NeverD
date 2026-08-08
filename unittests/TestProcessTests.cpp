#include "TestProcess.h"

#include "gtest/gtest.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace neverd::test;

namespace {

namespace fs = std::filesystem;

std::string readFile(const fs::path &Path) {
  std::ifstream Input(Path, std::ios::binary);
  return {std::istreambuf_iterator<char>(Input),
          std::istreambuf_iterator<char>()};
}

struct ScopedDirectory {
  fs::path Path;
  ~ScopedDirectory() {
    std::error_code Error;
    fs::remove_all(Path, Error);
  }
};

} // namespace

TEST(TestProcess, ReportsCurrentProcessId) {
  EXPECT_GT(currentProcessId(), 0ULL);
}

TEST(TestProcess, DecodesSystemExitCode) {
#ifdef _WIN32
  int Status = runShellCommand("cmd.exe /d /s /c \"exit /b 7\"");
#else
  int Status = runShellCommand("sh -c 'exit 7'");
#endif
  EXPECT_EQ(systemExitCode(Status), 7);
  EXPECT_EQ(systemExitCode(-1), -1);
}

TEST(TestProcess, SelectsHostConventions) {
#ifdef _WIN32
  EXPECT_STREQ(nullDevice(), "NUL");
  EXPECT_STREQ(executableSuffix(), ".exe");
  EXPECT_EQ(shellQuote("path with spaces"), "\"path with spaces\"");
#else
  EXPECT_STREQ(nullDevice(), "/dev/null");
  EXPECT_STREQ(executableSuffix(), "");
  EXPECT_EQ(shellQuote("path with spaces"), "'path with spaces'");
#endif
}

TEST(TestProcess, BuildsPortableRedirects) {
  EXPECT_EQ(silenceOutput(), " >" + shellQuote(nullDevice()) + " 2>&1");
  EXPECT_EQ(redirectOutput("out file", "err file"),
            " >" + shellQuote("out file") +
                " 2>" + shellQuote("err file"));
}

TEST(TestProcess, RunsQuotedExecutableWithQuotedRedirects) {
  auto Directory = fs::temp_directory_path() /
                   ("neverd process " + std::to_string(currentProcessId()));
  std::error_code Error;
  fs::remove_all(Directory, Error);
  Error.clear();
  ASSERT_TRUE(fs::create_directories(Directory, Error)) << Error.message();
  ScopedDirectory Cleanup{Directory};

  auto StdoutPath = Directory / "stdout file.txt";
  auto StderrPath = Directory / "stderr file.txt";
#ifdef _WIN32
  const char *CommandProcessor = std::getenv("COMSPEC");
  ASSERT_NE(CommandProcessor, nullptr);
  std::string Command = shellQuote(CommandProcessor) +
                        " /d /c \"exit /b 0\"";
#else
  std::string Command = shellQuote("/usr/bin/true");
#endif
  Command += redirectOutput(StdoutPath.string(), StderrPath.string());

  EXPECT_EQ(systemExitCode(runShellCommand(Command)), 0);
  EXPECT_TRUE(fs::exists(StdoutPath));
  EXPECT_TRUE(fs::exists(StderrPath));
  EXPECT_EQ(readFile(StdoutPath), "");
  EXPECT_EQ(readFile(StderrPath), "");
}
