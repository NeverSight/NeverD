//===- ProcessPublicTests.cpp - Real ELF through the shared SDK and CLI --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../TestProcess.h"
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionBackend.h"
#include "neverd/emulation/ExecutionReportFields.h"
#include "neverd/emulation/ProcessCLIStrings.h"
#include "neverd/emulation/ProcessReportFields.h"
#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <filesystem>

namespace {
using namespace neverd;
namespace field = emulation::process_report;
namespace cpu_field = emulation::execution_report;
#define NEVERD_EXECUTION_AVAILABILITY(Name, Text) constexpr char Name[] = Text;
#include "neverd/emulation/ExecutionBackend.def"
#undef NEVERD_EXECUTION_AVAILABILITY
#define NEVERD_PROCESS_TEST_TEXT(Name, Text) constexpr char Name[] = Text;
#include "ProcessReportCases.def"
#undef NEVERD_PROCESS_TEST_TEXT
#define NEVERD_LINUX_FIXTURE_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_LINUX_FIXTURE_TEXT(Name, Text) constexpr char Name[] = Text;
#define NEVERD_LINUX_FIXTURE_MODE(Name, Character, Text)                       \
  constexpr char Name[] = Text;
#include "fixtures/LinuxProcessCases.def"
#undef NEVERD_LINUX_FIXTURE_MODE
#undef NEVERD_LINUX_FIXTURE_TEXT
#undef NEVERD_LINUX_FIXTURE_VALUE
#define NEVERD_PROCESS_PROFILE(Name, Text) constexpr char Name[] = Text;
#include "neverd/emulation/ProcessProfile.def"
#undef NEVERD_PROCESS_PROFILE
namespace tls_fixture {
#define NEVERD_TLS_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_TLS_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/LinuxTLSCases.def"
#undef NEVERD_TLS_VALUE
#undef NEVERD_TLS_TEXT
} // namespace tls_fixture
namespace pie_fixture {
#define NEVERD_PIE_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_PIE_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/LinuxPIECases.def"
#undef NEVERD_PIE_VALUE
#undef NEVERD_PIE_TEXT
} // namespace pie_fixture
namespace memory_fixture {
#define NEVERD_LINUX_MEMORY_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_LINUX_MEMORY_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/LinuxMemoryCases.def"
#undef NEVERD_LINUX_MEMORY_VALUE
#undef NEVERD_LINUX_MEMORY_TEXT
} // namespace memory_fixture

namespace windows_fixture {
#define NEVERD_WINDOWS_TEST_TEXT(Name, Text) constexpr char Name[] = Text;
#include "WindowsProcessTestData.def"
#undef NEVERD_WINDOWS_TEST_TEXT
#define NEVERD_WINDOWS_FIXTURE_VALUE(Name, Value)                              \
  constexpr uint64_t Name = Value;
#define NEVERD_WINDOWS_FIXTURE_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/WindowsProcessCases.def"
#undef NEVERD_WINDOWS_FIXTURE_TEXT
#undef NEVERD_WINDOWS_FIXTURE_VALUE
} // namespace windows_fixture
namespace module_fixture {
#define NEVERD_MODULE_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_MODULE_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/WindowsModuleCases.def"
#undef NEVERD_MODULE_TEXT
#undef NEVERD_MODULE_VALUE
} // namespace module_fixture
std::string takeString(const char *Value) {
  if (!Value)
    return {};
  std::string Copy(Value);
  neverd_free_string(Value);
  return Copy;
}
std::string jsonText(llvm::json::Object Value) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  OS << llvm::json::Value(std::move(Value));
  return Text;
}

class ProcessPublic : public testing::Test {
protected:
  neverd_session_t Session = nullptr;
  std::string Path;
  void SetUp() override {
    Session = neverd_session_create();
    ASSERT_NE(Session, nullptr);
#ifndef NEVERD_PROCESS_FIXTURE_DIR
    GTEST_SKIP() << MissingTools;
#else
    Path =
        (std::filesystem::path(NEVERD_PROCESS_FIXTURE_DIR) / ARMFile).string();
    // This portable public-surface suite is separate from the process tests'
    // six transport/ISA cells; it must not pull a second engine into the DSO.
    auto Text =
        takeString(neverd_cpu_capabilities_json(Session, ProbeOptions, 1));
    ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
    auto Probe = llvm::json::parse(Text);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    const auto *Host = Probe->getAsObject()->getObject(cpu_field::Host);
    ASSERT_NE(Host, nullptr);
    if (Host->getString(cpu_field::Availability) != Available)
      GTEST_SKIP() << Host->getString(cpu_field::Reason)->str();
#endif
  }
  void TearDown() override { neverd_session_destroy(Session); }
  std::string options(const char *Mode) {
    llvm::json::Object Request;
    Request[field::Backend] = emulation::execution::Unicorn;
    Request[field::Arguments] = llvm::json::Array{ExecutableName, Mode};
    Request[field::Environment] = llvm::json::Array{Environment};
    Request[field::InstructionLimit] = 1000;
    return jsonText(std::move(Request));
  }
  llvm::json::Value run(const std::string &Options) {
    auto Text = takeString(neverd_emulate_process_json(
        Session, Path.c_str(), LinuxELF64, Options.c_str()));
    EXPECT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
    return llvm::cantFail(llvm::json::parse(Text));
  }
};

TEST_F(ProcessPublic, ReturnsOutputAndGuestStatusWithoutLoadingAnalysisImage) {
  for (const char *File : {X64File, ARMFile}) {
    SCOPED_TRACE(File);
    Path = (std::filesystem::path(Path).parent_path() / File).string();
    auto Result = run(options(Normal));
    EXPECT_EQ(Result.getAsObject()->getInteger(field::Version),
              NEVERD_PROCESS_SCHEMA_VERSION);
    EXPECT_EQ(Result.getAsObject()->getInteger(field::ExitStatus), ExitStatus);
    EXPECT_EQ(Result.getAsObject()->getString(field::Stderr), BinaryHex);
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  }
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1)
      << takeString(neverd_last_error(Session));
  const auto Before = takeString(neverd_segments_json(Session));
  run(options(Normal));
  EXPECT_EQ(neverd_session_is_loaded(Session), 1);
  EXPECT_EQ(takeString(neverd_segments_json(Session)), Before);
}

TEST_F(ProcessPublic, WindowsPE64RunsThroughSDKAndCLIWithoutAnalysisState) {
#ifndef NEVERD_WINDOWS_PROCESS_FIXTURE_DIR
  GTEST_SKIP() << windows_fixture::MissingTools;
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (const auto *File :
       {windows_fixture::X64File, windows_fixture::AArch64File}) {
    SCOPED_TRACE(File);
    Path = (std::filesystem::path(NEVERD_WINDOWS_PROCESS_FIXTURE_DIR) / File)
               .string();
    llvm::json::Object Request;
    Request[field::Backend] = emulation::execution::Unicorn;
    Request[field::Arguments] =
        llvm::json::Array{windows_fixture::Executable, windows_fixture::Normal};
    auto Options = jsonText(std::move(Request));
    auto Text = takeString(neverd_emulate_process_json(
        Session, Path.c_str(), WindowsPE64, Options.c_str()));
    ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
    auto Result = llvm::cantFail(llvm::json::parse(Text));
    EXPECT_EQ(Result.getAsObject()->getInteger(field::ExitStatus),
              windows_fixture::ExitStatus);
    EXPECT_EQ(Result.getAsObject()->getString(field::Stdout),
              llvm::toHex(std::string(windows_fixture::Message) +
                              windows_fixture::Detached,
                          true));
    ASSERT_NE(Result.getAsObject()->getObject(field::Windows), nullptr);
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    const auto Command = test::shellQuote(NEVERD_PROCESS_CLI) + " " +
                         process_cli::Command + " " + test::shellQuote(Path) +
                         " --" + process_cli::ProfileOption + "=" +
                         WindowsPE64 + " --" + process_cli::OptionsOption +
                         "=" + test::shellQuote(Options) +
                         test::redirectStdout(Output) + test::silenceStderr();
    EXPECT_EQ(test::systemExitCode(test::runShellCommand(Command)),
              process_cli::GuestFailure);
    auto Buffer = llvm::MemoryBuffer::getFile(Output);
    ASSERT_TRUE(bool(Buffer));
    EXPECT_EQ(llvm::cantFail(llvm::json::parse((*Buffer)->getBuffer())),
              Result);
  }
#endif
}

TEST_F(ProcessPublic, WindowsStartupDLLsUseTheSameCatalogueThroughSDKAndCLI) {
#ifndef NEVERD_WINDOWS_MODULE_FIXTURE_DIR
  GTEST_SKIP() << module_fixture::MissingTools;
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (const auto *File :
       {module_fixture::X64Dir, module_fixture::AArch64Dir}) {
    SCOPED_TRACE(File);
    Path = (std::filesystem::path(NEVERD_WINDOWS_MODULE_FIXTURE_DIR) / File /
            module_fixture::ProgramFile)
               .string();
    llvm::json::Object Request;
    Request[field::Backend] = emulation::execution::Unicorn;
    llvm::json::Array Modules;
    for (const char *Name :
         {module_fixture::LeafFile, module_fixture::MiddleFile})
      Modules.push_back(llvm::json::Object{
          {field::Name, Name},
          {field::Path,
           (std::filesystem::path(NEVERD_WINDOWS_MODULE_FIXTURE_DIR) / File /
            Name)
               .string()}});
    Request[field::Windows] =
        llvm::json::Object{{field::Modules, std::move(Modules)}};
    auto Options = jsonText(std::move(Request));
    auto Text = takeString(neverd_emulate_process_json(
        Session, Path.c_str(), WindowsPE64, Options.c_str()));
    ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
    auto Result = llvm::cantFail(llvm::json::parse(Text));
    EXPECT_EQ(Result.getAsObject()->getInteger(field::ExitStatus),
              module_fixture::ExitStatus);
    EXPECT_EQ(Result.getAsObject()->getString(field::Stdout),
              llvm::toHex(std::string(module_fixture::Message) +
                              module_fixture::Detached,
                          true));
    ASSERT_NE(Result.getAsObject()->getObject(field::Windows), nullptr);
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    const auto Command = test::shellQuote(NEVERD_PROCESS_CLI) + " " +
                         process_cli::Command + " " + test::shellQuote(Path) +
                         " --" + process_cli::ProfileOption + "=" +
                         WindowsPE64 + " --" + process_cli::OptionsOption +
                         "=" + test::shellQuote(Options) +
                         test::redirectStdout(Output) + test::silenceStderr();
    EXPECT_EQ(test::systemExitCode(test::runShellCommand(Command)),
              process_cli::GuestFailure);
    auto Buffer = llvm::MemoryBuffer::getFile(Output);
    ASSERT_TRUE(bool(Buffer));
    EXPECT_EQ(llvm::cantFail(llvm::json::parse((*Buffer)->getBuffer())),
              Result);
  }
#endif
}

TEST_F(ProcessPublic, AndroidNativeFunctionReturnsThroughSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  Path = (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) / "android.so")
             .string();
  for (
      const auto &[Request, Expected] :
      std::vector<std::pair<std::string, std::string>>{
          {R"({"backend":"unicorn","android":{"entry_symbol":"add_arguments","arguments":[1,2,3,4,5,6,7,8,9,10],"trace_limit":4096}})",
           "192"},
          {R"({"backend":"unicorn","android":{"entry_symbol":"dynamic_lookup","libraries":{"libfixture.so":["strlen"]}}})",
           "4"}}) {
    auto Text = takeString(neverd_emulate_process_json(
        Session, Path.c_str(), AndroidNativeAArch64, Request.c_str()));
    ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
    auto Parsed = llvm::cantFail(llvm::json::parse(Text));
    EXPECT_EQ(Parsed.getAsObject()->getString(field::Stop), "returned");
    EXPECT_EQ(Parsed.getAsObject()->getString(field::ReturnValue), Expected);
    if (Expected == "4") {
      bool NamedLookup = false;
      const auto *Android = Parsed.getAsObject()->getObject(field::Android);
      ASSERT_NE(Android, nullptr);
      for (const auto &Event : *Android->getArray(field::NativeCalls)) {
        const auto *E = Event.getAsObject();
        if (E->getString(field::Name) == "dlsym") {
          EXPECT_EQ(E->getString(field::Library), "libfixture.so");
          EXPECT_EQ(E->getString(field::Symbol), "strlen");
          NamedLookup = true;
        }
      }
      EXPECT_TRUE(NamedLookup);
    }
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    llvm::SmallString<128> Directory;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
    const std::filesystem::path Root(Directory.str().str());
    auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
    const auto Output = (Root / OutputFile).string();
    const auto Command = test::shellQuote(NEVERD_PROCESS_CLI) + " emulate " +
                         test::shellQuote(Path) +
                         " --profile=" + AndroidNativeAArch64 +
                         " --options=" + test::shellQuote(Request) +
                         test::redirectStdout(Output) + test::silenceStderr();
    EXPECT_EQ(test::systemExitCode(test::runShellCommand(Command)),
              process_cli::Success);
    auto Buffer = llvm::MemoryBuffer::getFile(Output);
    ASSERT_TRUE(bool(Buffer));
    EXPECT_EQ(llvm::cantFail(llvm::json::parse((*Buffer)->getBuffer())),
              Parsed);
  }
#endif
}

TEST_F(ProcessPublic, InvalidSetupDoesNotPoisonTheReusableSession) {
  EXPECT_EQ(
      neverd_emulate_process_json(nullptr, Path.c_str(), LinuxELF64, nullptr),
      nullptr);
  EXPECT_EQ(neverd_emulate_process_json(Session, nullptr, LinuxELF64, nullptr),
            nullptr);
  EXPECT_EQ(takeString(neverd_last_error(Session)), field::PathRequired);
  EXPECT_EQ(
      neverd_emulate_process_json(Session, Path.c_str(), nullptr, nullptr),
      nullptr);
  EXPECT_EQ(takeString(neverd_last_error(Session)), field::ProfileRequired);
  EXPECT_EQ(
      neverd_emulate_process_json(Session, MissingPath, LinuxELF64, nullptr),
      nullptr);
  EXPECT_FALSE(takeString(neverd_last_error(Session)).empty());
  EXPECT_EQ(neverd_emulate_process_json(Session, Path.c_str(), UnknownProfile,
                                        nullptr),
            nullptr);
#define NEVERD_PROCESS_INVALID_JSON(Name, Text)                                \
  {                                                                            \
    SCOPED_TRACE(#Name);                                                       \
    EXPECT_EQ(                                                                 \
        neverd_emulate_process_json(Session, Path.c_str(), LinuxELF64, Text),  \
        nullptr);                                                              \
    EXPECT_FALSE(takeString(neverd_last_error(Session)).empty());              \
  }
#include "ProcessReportCases.def"
#undef NEVERD_PROCESS_INVALID_JSON
  const std::string Large(NEVERD_PROCESS_OPTIONS_JSON_LIMIT + 1, ' ');
  EXPECT_EQ(neverd_emulate_process_json(Session, Path.c_str(), LinuxELF64,
                                        Large.c_str()),
            nullptr);
  EXPECT_EQ(takeString(neverd_last_error(Session)), field::TooLarge);
  run(options(Normal));
  EXPECT_TRUE(takeString(neverd_last_error(Session)).empty());
}

TEST_F(ProcessPublic, CompilerStartupRunsThroughTheSharedSDKAndCLI) {
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  struct Fixture {
    const char *File, *Message;
    uint64_t Limit, Status;
  };
  const Fixture Fixtures[] = {
      {tls_fixture::X64File, tls_fixture::Message,
       tls_fixture::InstructionLimit, tls_fixture::ExitStatus},
      {tls_fixture::ARMFile, tls_fixture::Message,
       tls_fixture::InstructionLimit, tls_fixture::ExitStatus},
      {pie_fixture::X64File, pie_fixture::Message,
       pie_fixture::InstructionLimit, pie_fixture::ExitStatus},
      {pie_fixture::ARMFile, pie_fixture::Message,
       pie_fixture::InstructionLimit, pie_fixture::ExitStatus},
      {memory_fixture::X64File, memory_fixture::Message,
       memory_fixture::InstructionLimit, memory_fixture::ExitStatus},
      {memory_fixture::ARMFile, memory_fixture::Message,
       memory_fixture::InstructionLimit, memory_fixture::ExitStatus}};
  for (const auto &Fixture : Fixtures) {
    SCOPED_TRACE(Fixture.File);
    Path = (std::filesystem::path(Path).parent_path() / Fixture.File).string();
    auto Request = llvm::cantFail(llvm::json::parse(options(Normal)));
    (*Request.getAsObject())[field::InstructionLimit] = Fixture.Limit;
    const auto Options = jsonText(std::move(*Request.getAsObject()));
    const auto Result = run(Options);
    EXPECT_EQ(Result.getAsObject()->getInteger(field::ExitStatus),
              Fixture.Status);
    EXPECT_EQ(Result.getAsObject()->getString(field::Stdout),
              llvm::toHex(Fixture.Message, true));
    const auto Command = test::shellQuote(NEVERD_PROCESS_CLI) + " " +
                         process_cli::Command + " " + test::shellQuote(Path) +
                         " --" + process_cli::ProfileOption + "=" + LinuxELF64 +
                         " --" + process_cli::OptionsOption + "=" +
                         test::shellQuote(Options) +
                         test::redirectStdout(Output) + test::silenceStderr();
    EXPECT_EQ(test::systemExitCode(test::runShellCommand(Command)),
              process_cli::Success);
    auto Buffer = llvm::MemoryBuffer::getFile(Output);
    ASSERT_TRUE(bool(Buffer));
    auto Report = llvm::json::parse((*Buffer)->getBuffer());
    ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError());
    EXPECT_EQ(*Report, Result);
  }
}

TEST_F(ProcessPublic,
       CLIHasTheSameReportAndSeparatesGuestFailureFromIncomplete) {
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (const char *Mode : {Normal, Loop, Fault, Unknown}) {
    SCOPED_TRACE(Mode);
    const auto Options = options(Mode);
    const auto Command = test::shellQuote(NEVERD_PROCESS_CLI) + " " +
                         process_cli::Command + " " + test::shellQuote(Path) +
                         " --" + process_cli::ProfileOption + "=" + LinuxELF64 +
                         " --" + process_cli::OptionsOption + "=" +
                         test::shellQuote(Options) +
                         test::redirectStdout(Output) + test::silenceStderr();
    EXPECT_EQ(test::systemExitCode(test::runShellCommand(Command)),
              Mode == Normal ? process_cli::GuestFailure
                             : process_cli::Incomplete);
    auto Buffer = llvm::MemoryBuffer::getFile(Output);
    ASSERT_TRUE(bool(Buffer));
    auto Report = llvm::json::parse((*Buffer)->getBuffer());
    ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError());
    EXPECT_EQ(*Report, run(Options));
  }
}
} // namespace
