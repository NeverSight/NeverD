#include "MobileCommon.h"
#include "gtest/gtest.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <limits>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif
#ifndef NEVERD_MOBILE_PROCESS_FIXTURE
#error "Register MobileProcessFixture and define NEVERD_MOBILE_PROCESS_FIXTURE"
#endif
using namespace neverd::mobile;
namespace {
std::string hex(const std::string &S) {
  constexpr char Digits[] = "0123456789abcdef";
  std::string Out;
  for (unsigned char C : S) {
    Out += Digits[C >> 4];
    Out += Digits[C & 15];
  }
  return Out;
}
std::string rejection(const std::function<void()> &Run,
                      std::string_view Fragment) {
  try {
    Run();
    ADD_FAILURE() << "backend operation unexpectedly succeeded";
  } catch (const std::runtime_error &E) {
    EXPECT_NE(std::string(E.what()).find(Fragment), std::string::npos)
        << E.what();
    return E.what();
  }
  return {};
}
class MobileProcessTests : public testing::Test {
protected:
  fs::path Root;
  const std::string Fixture = NEVERD_MOBILE_PROCESS_FIXTURE;
  void SetUp() override {
    ASSERT_TRUE(fs::is_regular_file(pathFromUTF8(Fixture))) << Fixture;
    static std::atomic<uint64_t> Sequence{0};
#ifdef _WIN32
    auto PID = GetCurrentProcessId();
#else
    auto PID = getpid();
#endif
    Root = fs::temp_directory_path() /
           ("neverd-mobile-process-" + std::to_string(PID) + "-" +
            std::to_string(Sequence++));
    ASSERT_TRUE(fs::create_directory(Root));
  }
  void TearDown() override {
    std::error_code EC;
    fs::remove_all(Root, EC);
  }
  std::vector<std::string>
  command(std::initializer_list<std::string> Args) const {
    std::vector<std::string> Out{Fixture};
    Out.insert(Out.end(), Args.begin(), Args.end());
    return Out;
  }
  std::string log(const std::string &Name = "log") const {
    return readFile(Root / Name, UINT64_C(16) * 1024 * 1024);
  }
};
TEST_F(MobileProcessTests,
       ArgumentsAreLiteralIncludingQuotesEmptyUnicodeAndTrailingBackslashes) {
  std::vector<std::string> Args{"a b;$(touch evil)`test`&%PATH%",
                                "",
                                "quoted\"value",
                                "trailing\\",
                                "\\\\",
                                "line\nbreak",
                                "\xce\xbb\xf0\x9f\x98\x80"};
  auto Command = command({"echo"});
  Command.insert(Command.end(), Args.begin(), Args.end());
  runTool(Command, Root / "log", 5);
  std::string Expected;
  for (const auto &Arg : Args)
    Expected += hex(Arg) + "\n";
  EXPECT_EQ(log(), Expected);
}
TEST_F(MobileProcessTests,
       StandardInputIsClosedAndBothOutputStreamsAreCaptured) {
  runTool(command({"stdio"}), Root / "log", 5);
  EXPECT_EQ(log(), "closed\nstdout\nstderr\n");
}
TEST_F(MobileProcessTests,
       CompletedLeaderWithNoLiveGroupMembersIsReapedSuccessfully) {
  // In particular, Darwin killpg returns EPERM for the unreaped zombie alone.
  // It must be distinguished from failure to terminate a live descendant.
  for (unsigned I = 0; I < 3; ++I)
    runTool(command({"echo"}), Root / ("completed" + std::to_string(I)), 5);
}
TEST_F(MobileProcessTests, EnvironmentOverridesAndUnsetArePerChild) {
  const char *Parent = std::getenv("PATH");
  std::optional<std::string> Before =
      Parent ? std::optional<std::string>(Parent) : std::nullopt;
  std::string Value = "literal;= value \xce\xbb";
  runTool(command({"environment", "NEVERD_PROCESS_TEST"}), Root / "set", 5, {},
          {}, {{"NEVERD_PROCESS_TEST", Value}});
  EXPECT_EQ(log("set"), "set:" + hex(Value) + "\n");
  runTool(command({"environment", "PATH"}), Root / "unset", 5, {}, {},
          {{"PATH", std::nullopt}});
  EXPECT_EQ(log("unset"), "unset\n");
  Parent = std::getenv("PATH");
  EXPECT_EQ(Parent ? std::optional<std::string>(Parent) : std::nullopt, Before);
}
TEST_F(MobileProcessTests, BareExecutableResolutionUsesTheChildPath) {
  fs::path Installed = Root / "tools";
  fs::create_directory(Installed);
#ifdef _WIN32
  std::string Name = "owned-fixture.exe";
#else
  std::string Name = "owned-fixture";
#endif
  fs::copy_file(pathFromUTF8(Fixture), Installed / Name);
#ifndef _WIN32
  fs::permissions(Installed / Name, fs::perms::owner_exec,
                  fs::perm_options::add);
#endif
  runTool({Name, "echo", "from child path"}, Root / "log", 5, {}, {},
          {{"PATH", pathText(Installed)}});
  EXPECT_EQ(log(), hex("from child path") + "\n");
}
TEST_F(MobileProcessTests, ExistingLogsAreNeverOverwritten) {
  writeFile(Root / "log", "keep");
  rejection([&] { runTool(command({"echo", "changed"}), Root / "log", 5); },
            "exclusively create");
  EXPECT_EQ(log(), "keep");
}
TEST_F(MobileProcessTests, MissingExecutableHasALaunchDiagnostic) {
  rejection(
      [&] { runTool({pathText(Root / "does-not-exist")}, Root / "log", 5); },
      "cannot execute");
}
TEST_F(MobileProcessTests,
       InvalidArgumentsAndEnvironmentFailBeforeLogCreation) {
  rejection([&] { runTool({}, Root / "empty", 5); }, "invalid backend");
  rejection([&] { runTool(command({"echo"}), Root / "timeout", 0); },
            "invalid backend");
  rejection(
      [&] {
        runTool(command({"echo"}), Root / "overflow",
                uint64_t(std::numeric_limits<int64_t>::max() / 1000000000));
      },
      "invalid backend timeout");
  rejection(
      [&] {
        runTool(command({"echo", std::string("a\0b", 3)}), Root / "nul", 5);
      },
      "NUL");
  rejection(
      [&] {
        runTool(command({"echo"}), Root / "env", 5, {}, {},
                {{"bad=name", "value"}});
      },
      "environment");
  EXPECT_TRUE(fs::is_empty(Root));
}
TEST_F(MobileProcessTests, NonzeroExitUsesOnlyABoundedDiagnosticTail) {
  auto Error =
      rejection([&] { runTool(command({"fail", "12000"}), Root / "log", 5); },
                "status 7:");
  EXPECT_NE(Error.find("problem"), std::string::npos);
  EXPECT_LT(Error.size(), 4200u);
  EXPECT_GT(log().size(), 12000u);
}
TEST_F(MobileProcessTests, TimeoutStopsTheChildWithinTheConfiguredBudget) {
  auto Begin = std::chrono::steady_clock::now();
  auto Error =
      rejection([&] { runTool(command({"sleep", "10000"}), Root / "log", 1); },
                "timed out");
  EXPECT_LT(std::chrono::steady_clock::now() - Begin, std::chrono::seconds(5));
  EXPECT_EQ(Error, "backend timed out after 1 seconds");
  EXPECT_TRUE(log().empty());
}
TEST_F(MobileProcessTests, TimeoutRetainsOnlyTheCapturedBoundedDiagnosticTail) {
  auto Error = rejection(
      [&] {
        runTool(command({"log-sleep", "12000", "10000"}), Root / "log", 1);
      },
      "timed out");
  EXPECT_EQ(log(), "first-line\n" + std::string(12000, 'x') +
                       "\nlast-out\nlast-error\n");
  // The ASCII capture ends in 21 suffix bytes. Its last 4096 bytes contain
  // 4075 fill bytes; the existing tail formatter trims the final newline.
  EXPECT_EQ(Error, "backend timed out after 1 seconds: " +
                       std::string(4075, 'x') + "\nlast-out\nlast-error");
  EXPECT_EQ(Error.find("first-line"), std::string::npos);
  EXPECT_LT(Error.size(), 4200u);
}
TEST_F(MobileProcessTests, DiagnosticCapIsExactEvenForAFastWriter) {
  auto Error = rejection(
      [&] {
        runTool(command({"spam", std::to_string(17 * 1024 * 1024)}),
                Root / "log", 10);
      },
      "diagnostic output");
  EXPECT_EQ(Error, "backend diagnostic output exceeded 16 MiB");
  EXPECT_EQ(fs::file_size(Root / "log"), UINT64_C(16) * 1024 * 1024);
}
TEST_F(MobileProcessTests, SuccessfulWrapperCannotLeaveADescendantWriting) {
  fs::path Marker = Root / "late";
  runTool(command({"spawn", pathText(Marker), "0", "0", "1000"}), Root / "log",
          5);
  EXPECT_NE(log().find("spawned"), std::string::npos);
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  EXPECT_FALSE(fs::exists(Marker));
}
TEST_F(MobileProcessTests, FailingWrapperCannotLeaveADescendantWriting) {
  fs::path Marker = Root / "late";
  rejection(
      [&] {
        runTool(command({"spawn", pathText(Marker), "7", "0", "1000"}),
                Root / "log", 5);
      },
      "status 7:");
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  EXPECT_FALSE(fs::exists(Marker));
}
TEST_F(MobileProcessTests, TimeoutAlsoStopsDescendants) {
  fs::path Marker = Root / "late";
  auto Error = rejection(
      [&] {
        runTool(command({"spawn", pathText(Marker), "0", "10000", "2200"}),
                Root / "log", 1);
      },
      "timed out");
  EXPECT_EQ(Error, "backend timed out after 1 seconds: spawned");
  EXPECT_EQ(log(), "spawned\n");
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  EXPECT_FALSE(fs::exists(Marker));
}
TEST_F(MobileProcessTests, LiveWorkspaceUsesThreeTimesTheConfiguredByteBudget) {
  fs::path Work = Root / "work";
  fs::create_directory(Work);
  Limits L;
  L.max_bytes = 100;
  rejection(
      [&] {
        runTool(command({"files", pathText(Work), "1", "500", "10000"}),
                Root / "large", 5, Work, L);
      },
      "limits");
  fs::remove_all(Work);
  fs::create_directory(Work);
  runTool(command({"files", pathText(Work), "1", "250", "0"}), Root / "within",
          5, Work, L);
  EXPECT_EQ(fs::file_size(Work / "file0"), 250u);
}
TEST_F(MobileProcessTests, FinalWorkspaceScanRejectsFastFileCountOverflow) {
  fs::path Work = Root / "work";
  fs::create_directory(Work);
  Limits L;
  L.max_files = 1;
  rejection(
      [&] {
        runTool(command({"files", pathText(Work), "4", "1", "0"}), Root / "log",
                5, Work, L);
      },
      "file-count");
}
TEST_F(MobileProcessTests, LiveWorkspaceAllowsRetiringTemporaryEntries) {
  fs::path Work = Root / "work";
  fs::create_directory(Work);
  runTool(command({"churn", pathText(Work)}), Root / "log", 5, Work);
  EXPECT_TRUE(fs::is_empty(Work));
}
#ifndef _WIN32
TEST_F(MobileProcessTests, WorkspaceLinksAndLogLinksAreRejected) {
  fs::path Work = Root / "work";
  fs::create_directory(Work);
  rejection(
      [&] {
        runTool(command({"link", pathText(Work / "link"), pathText(Root)}),
                Root / "log", 5, Work);
      },
      "link");
  writeFile(Root / "keep", "preserve");
  fs::create_symlink(Root / "keep", Root / "linked-log");
  rejection([&] { runTool(command({"echo"}), Root / "linked-log", 5); },
            "exclusively create");
  EXPECT_EQ(readFile(Root / "keep", 100), "preserve");
}
#else
TEST_F(MobileProcessTests, WindowsBatchLaunchersCannotReachACommandShell) {
  for (const auto *Name : {"launcher.bat", "launcher.CMD"})
    rejection([&] { runTool({pathText(Root / Name)}, Root / Name, 5); },
              "batch launchers");
  EXPECT_TRUE(fs::is_empty(Root));
}
#endif
} // namespace
