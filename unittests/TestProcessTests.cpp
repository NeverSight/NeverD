#include "TestProcess.h"

#include "gtest/gtest.h"

#include <cstdlib>
#include <string>

using namespace neverd::test;

TEST(TestProcess, ReportsCurrentProcessId) {
  EXPECT_GT(currentProcessId(), 0ULL);
}

TEST(TestProcess, DecodesSystemExitCode) {
#ifdef _WIN32
  int Status = std::system("cmd.exe /d /s /c \"exit /b 7\"");
#else
  int Status = std::system("sh -c 'exit 7'");
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
