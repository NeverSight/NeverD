//===- ProcessPublicTests.cpp - Real ELF through the shared SDK and CLI --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../TestProcess.h"
#include "DarwinAdvisoryLockTestData.h"
#include "DarwinCallbackTestData.h"
#include "DarwinCreatedAdvisoryLockTestData.h"
#include "DarwinCreatedAttributeTestData.h"
#include "DarwinCreatedSynchronizationTestData.h"
#include "DarwinDataSynchronizationTestData.h"
#include "DarwinDirectoryModeTestData.h"
#include "DarwinDirectoryOwnershipTestData.h"
#include "DarwinDirectoryTimestampTestData.h"
#include "DarwinEntropyTestData.h"
#include "DarwinExitTestData.h"
#include "DarwinFileTestData.h"
#include "DarwinFullSynchronizationTestData.h"
#include "DarwinGuardTestData.h"
#include "DarwinLibSystemFileTestData.h"
#include "DarwinLibSystemMemoryTestData.h"
#include "DarwinModeTestData.h"
#include "DarwinOwnershipTestData.h"
#include "DarwinSymbolicLinkFileSystemTestData.h"
#include "DarwinSymbolicOwnershipTestData.h"
#include "DarwinSynchronizationTestData.h"
#include "DarwinSystemTestData.h"
#include "DarwinTimeTestData.h"
#include "DarwinTimestampMutationTestData.h"
#include "LinuxFileTestMetadata.h"
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionBackend.h"
#include "neverd/emulation/ExecutionReportFields.h"
#include "neverd/emulation/ProcessCLIStrings.h"
#include "neverd/emulation/ProcessReportFields.h"
#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <vector>

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
namespace export_fixture {
#define NEVERD_EXPORT_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_EXPORT_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/WindowsExportCases.def"
#undef NEVERD_EXPORT_TEXT
#undef NEVERD_EXPORT_VALUE
} // namespace export_fixture
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

// LLVM response files retain one JSON argument without exceeding Windows'
// CreateProcess command-line limit. The child still runs without a shell.
int runCLIRequest(llvm::StringRef Path, llvm::StringRef Profile,
                  llvm::StringRef Options, llvm::StringRef Output,
                  bool CaptureErrors = false) {
  // LLVM's redirects open an existing file without truncating it. Repeated
  // invocations can produce a shorter JSON report, so remove the old suffix.
  std::error_code OutputError;
  {
    llvm::raw_fd_ostream Truncate(Output, OutputError);
  }
  if (OutputError) {
    ADD_FAILURE() << "cannot truncate CLI output: " << OutputError.message();
    return -1;
  }
  const std::string ProfileArg =
      "--" + std::string(process_cli::ProfileOption) + "=" + Profile.str();
  const std::string OptionsArg =
      "--" + std::string(process_cli::OptionsOption) + "=" + Options.str();
  const std::string ResponsePath = Output.str() + ".rsp";
  auto RemoveResponse =
      llvm::scope_exit([&] { llvm::sys::fs::remove(ResponsePath); });
  std::error_code ResponseError;
  llvm::raw_fd_ostream Response(ResponsePath, ResponseError);
  if (ResponseError) {
    ADD_FAILURE() << "cannot create CLI response file: "
                  << ResponseError.message();
    return -1;
  }
#ifdef _WIN32
  Response << test::shellQuote(OptionsArg);
#else
  // LLVM's GNU response tokenizer consumes escapes even inside single quotes.
  Response << '"';
  for (char C : OptionsArg) {
    if (C == '\\' || C == '"')
      Response << '\\';
    Response << C;
  }
  Response << '"';
#endif
  Response << '\n';
  Response.close();
  if (Response.has_error()) {
    ADD_FAILURE() << "cannot write CLI response file";
    Response.clear_error();
    return -1;
  }
  const std::string ResponseArg = "@" + ResponsePath;
  const llvm::StringRef Args[] = {NEVERD_PROCESS_CLI, process_cli::Command,
                                  Path, ProfileArg, ResponseArg};
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, CaptureErrors ? llvm::StringRef("") : Output,
      CaptureErrors ? Output : llvm::StringRef("")};
  std::string Error;
  const int Status = llvm::sys::ExecuteAndWait(
      NEVERD_PROCESS_CLI, Args, std::nullopt, Redirects, 30, 0, &Error);
  EXPECT_GE(Status, 0) << Error;
  return Status;
}

void expectCLIReport(llvm::StringRef Output,
                     const llvm::json::Value &Expected) {
  auto Buffer = llvm::MemoryBuffer::getFile(Output);
  ASSERT_TRUE(bool(Buffer)) << Buffer.getError().message();
  auto Report = llvm::json::parse((*Buffer)->getBuffer());
  ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError()) << "\n"
                            << (*Buffer)->getBuffer().str();
  EXPECT_EQ(*Report, Expected);
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

TEST_F(ProcessPublic, CLIResponsePreservesLargeJSONAndQuotedEnvironment) {
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  auto Request = llvm::cantFail(llvm::json::parse(options(EchoEnvironment)));
  const std::string Special =
      " spaces 'quotes' \"double\" \\ %PATH%\n\r\t\xc3\xa9";
  const std::string Payload =
      std::string(EchoEnvironmentSize - Special.size(), 'x') + Special;
  (*Request.getAsObject())[field::Environment] = llvm::json::Array{Payload};
  std::string Options;
  llvm::raw_string_ostream Stream(Options);
  Stream << Request;
  ASSERT_GT(Options.size(), 32767u);
  auto Expected = run(Options);
  ASSERT_EQ(Expected.getAsObject()->getString(field::Stop), "exited");
  ASSERT_EQ(Expected.getAsObject()->getInteger(field::ExitStatus), ExitStatus);
  ASSERT_EQ(Expected.getAsObject()->getString(field::Stdout),
            llvm::toHex(Payload, true));
  EXPECT_EQ(runCLIRequest(Path, LinuxELF64, Options, Output),
            process_cli::GuestFailure);
  ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Expected));
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
    const int CLIStatus = runCLIRequest(Path, WindowsPE64, Options, Output);
    EXPECT_EQ(CLIStatus, process_cli::GuestFailure);
    ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Result));
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
    const int CLIStatus = runCLIRequest(Path, WindowsPE64, Options, Output);
    EXPECT_EQ(CLIStatus, process_cli::GuestFailure);
    ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Result));
  }
#endif
}

TEST_F(ProcessPublic, WindowsRuntimeExportsAgreeAcrossSDKAndCLI) {
#ifndef NEVERD_WINDOWS_EXPORT_FIXTURE_DIR
  GTEST_SKIP() << export_fixture::MissingTools;
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (const auto *File :
       {export_fixture::X64Dir, export_fixture::AArch64Dir}) {
    SCOPED_TRACE(File);
    Path = (std::filesystem::path(NEVERD_WINDOWS_EXPORT_FIXTURE_DIR) / File /
            export_fixture::ProgramFile)
               .string();
    llvm::json::Object Request;
    Request[field::Backend] = emulation::execution::Unicorn;
    Request[field::Arguments] = llvm::json::Array{
        export_fixture::ProgramFile, export_fixture::NormalArgument};
    llvm::json::Array Modules;
    for (const char *Name :
         {export_fixture::LeafFile, export_fixture::BridgeFile,
          export_fixture::TopFile})
      Modules.push_back(llvm::json::Object{
          {field::Name, Name},
          {field::Path,
           (std::filesystem::path(NEVERD_WINDOWS_EXPORT_FIXTURE_DIR) / File /
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
              export_fixture::ExitStatus);
    EXPECT_EQ(Result.getAsObject()->getString(field::Stdout),
              llvm::toHex(export_fixture::NormalTrace, true));
    ASSERT_NE(Result.getAsObject()->getObject(field::Windows), nullptr);
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    const int CLIStatus = runCLIRequest(Path, WindowsPE64, Options, Output);
    EXPECT_EQ(CLIStatus, process_cli::GuestFailure);
    ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Result));
  }
#endif
}

TEST_F(ProcessPublic, DarwinProfilesPreserveBSDResultsAcrossSDKAndCLI) {
#ifndef NEVERD_DARWIN_FIXTURE_DIR
  GTEST_SKIP() << "Clang and ld64.lld Darwin fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (const auto &[File, Profile] :
       {std::pair{"macos-arm64", MacOSMachO64},
        std::pair{"macos-x86_64", MacOSMachO64},
        std::pair{"ios-arm64", IOSMachO64},
        std::pair{"ios-simulator-arm64", IOSSimulatorMachO64},
        std::pair{"ios-simulator-x86_64", IOSSimulatorMachO64}}) {
    SCOPED_TRACE(File);
    Path = (std::filesystem::path(NEVERD_DARWIN_FIXTURE_DIR) / File).string();
    const std::string Options =
        R"({"backend":"unicorn","arguments":["guest","normal","argument"],"environment":["MODE=test"]})";
    auto Text = takeString(neverd_emulate_process_json(
        Session, Path.c_str(), Profile, Options.c_str()));
    ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
    auto Report = llvm::json::parse(Text);
    ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError());
    ASSERT_NE(Report->getAsObject(), nullptr);
    EXPECT_EQ(Report->getAsObject()->getString(field::Profile), Profile);
    EXPECT_EQ(Report->getAsObject()->getInteger(field::ExitStatus), 37);
    EXPECT_EQ(Report->getAsObject()->getString(field::Stdout),
              "64617277696e00ff0a");
    auto *Services = Report->getAsObject()->getArray(field::Services);
    ASSERT_NE(Services, nullptr);
    ASSERT_EQ(Services->size(), 4u);
    EXPECT_EQ((*Services)[0].getAsObject()->getBoolean(field::Error), true);
    EXPECT_EQ((*Services)[1].getAsObject()->getBoolean(field::Error), false);
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    const int CLIStatus = runCLIRequest(Path, Profile, Options, Output);
    EXPECT_EQ(CLIStatus, process_cli::GuestFailure);
    ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, *Report));
    auto Wrong = takeString(neverd_emulate_process_json(
        Session, Path.c_str(),
        File == std::string("ios-arm64") ? MacOSMachO64 : IOSMachO64,
        Options.c_str()));
    EXPECT_TRUE(Wrong.empty());
    EXPECT_FALSE(takeString(neverd_last_error(Session)).empty());
  }
#endif
}

struct DarwinPublicCase {
  const char *File, *Profile, *Mode, *Expected;
};
void PrintTo(const DarwinPublicCase &Case, std::ostream *OS) {
  *OS << Case.File << '/' << Case.Mode;
}
std::vector<DarwinPublicCase> darwinPublicCases() {
  std::vector<DarwinPublicCase> Cases;
  for (const auto &[File, Profile] :
       {std::pair{"macos-arm64", MacOSMachO64},
        std::pair{"macos-x86_64", MacOSMachO64},
        std::pair{"ios-arm64", IOSMachO64},
        std::pair{"ios-simulator-arm64", IOSSimulatorMachO64},
        std::pair{"ios-simulator-x86_64", IOSSimulatorMachO64}})
    for (const auto &[Mode, Expected] :
         {std::pair{"files", "66"},
          std::pair{"files-nocancel", "66"},
          std::pair{"nonblocking-descriptors", "4e"},
          std::pair{"nonblocking-flags-unsupported", "4e"},
          std::pair{"common-attributes", "41"},
          std::pair{"common-attributes-values",
                    emulation::darwin_test::CommonAttributesHex},
          std::pair{"common-attributes-unsupported", "41"},
          std::pair{"extended-attributes", "58"},
          std::pair{"extended-attributes-values",
                    emulation::darwin_test::ExtendedAttributesHex},
          std::pair{"extended-attributes-unsupported", "58"},
          std::pair{"symbolic-descriptors", "53"},
          std::pair{"symbolic-descriptors-values",
                    emulation::darwin_test::SymbolicDescriptorHex},
          std::pair{"symbolic-descriptors-name-unsupported", "53"},
          std::pair{"hard-links", "48"},
          std::pair{"hard-links-values", emulation::darwin_test::HardLinksHex},
          std::pair{"hard-links-name-unsupported", "48"},
          std::pair{"hard-links-attributes-unsupported", "48"},
          std::pair{"xattr-mutations", "56"},
          std::pair{"xattr-mutations-values",
                    emulation::darwin_test::XattrMutationsHex},
          std::pair{"xattr-mutations-unsupported", "56"},
          std::pair{"bulk-attributes", "42"},
          std::pair{"bulk-attributes-values",
                    emulation::darwin_test::BulkAttributesHex},
          std::pair{"bulk-attributes-unsupported", "42"},
          std::pair{"attribute-names", "4e"},
          std::pair{"attribute-names-values",
                    emulation::darwin_test::AttributeNamesHex},
          std::pair{"attribute-names-unsupported", "4e"},
          std::pair{"kernel-pathconf", "43"},
          std::pair{"kernel-pathconf-values",
                    emulation::darwin_test::KernelPathConfHex},
          std::pair{"kernel-pathconf-unsupported", "43"},
          std::pair{"writable-files", "303030303665"},
          std::pair{"writable-files-nocancel", "303030303665"},
          std::pair{"virtual-file-metadata",
                    emulation::darwin_test::MutationMetadataHex},
          std::pair{"sparse-file-seek", "73"},
          std::pair{"unlinked-file", "75"},
          std::pair{"created-file", "63"},
          std::pair{"created-file-metadata", "71"},
          std::pair{"mutable-initial-links", "4d"},
          std::pair{"virtual-mutable-initial-links",
                    emulation::darwin_test::InitialSymbolicLinkMetadataHex},
          std::pair{"directory-link-roots", "52"},
          std::pair{"virtual-directory-link-roots",
                    emulation::darwin_test::DirectoryLinkRootMetadataHex},
          std::pair{"initial-directory-metadata", "49"},
          std::pair{"virtual-initial-directory-metadata",
                    emulation::darwin_test::InitialDirectoryMetadataHex},
          std::pair{"directory-enumeration-mutations", "45"},
          std::pair{"virtual-directory-enumeration",
                    emulation::darwin_test::EnumerationMetadataHex},
          std::pair{"created-namespace-metadata", "4e"},
          std::pair{"virtual-created-namespace-metadata",
                    emulation::darwin_test::NamespaceMetadataHex},
          std::pair{"renamed-file", "72"},
          std::pair{"renamed-directory", "64"},
          std::pair{"swapped-directory", "73"},
          std::pair{"vectored-io", "7621"},
          std::pair{"file-access", "61"},
          std::pair{"symbolic-links", "79"},
          std::pair{"symbolic-link-mutations", "7a"},
          std::pair{"directory-mutations", "6d"},
          std::pair{"deleted-directories", "68"},
          std::pair{"initial-directory-removal", "6a"},
          std::pair{"initial-directory-move", "70"},
          std::pair{"initial-directory-swap", "71"},
          std::pair{"virtual-created-metadata",
                    emulation::darwin_test::CreationMetadataHex},
          std::pair{"stdin", "00ff78"},
          std::pair{"output-descriptors", "6f6b"},
          std::pair{"file-status", "73"},
          std::pair{"file-mapping", "6d"},
          std::pair{"directories", "64"},
          std::pair{"directory-entries", "65"},
          std::pair{"time-values", emulation::darwin_test::TimeHex},
          std::pair{"credentials", "6b"},
          std::pair{"virtual-credentials",
                    emulation::darwin_test::CredentialsHex},
          std::pair{"resource-usage", "67"},
          std::pair{"virtual-resource-usage",
                    emulation::darwin_test::ResourceUsageHex},
          std::pair{"resource-limits", "6c"},
          std::pair{"virtual-resource-limits",
                    emulation::darwin_test::ResourceLimitsHex},
          std::pair{"system-info", "69"},
          std::pair{"virtual-system", emulation::darwin_test::SystemHex},
          std::pair{"hostname", "6e"},
          std::pair{"virtual-hostname", emulation::darwin_test::HostNameHex},
          std::pair{"process-observations", "50"},
          std::pair{"virtual-process-observations",
                    emulation::darwin_test::ProcessObservationsHex},
          std::pair{"process-priority", "51"},
          std::pair{"virtual-process-priority",
                    emulation::darwin_test::PriorityHex},
          std::pair{"thread-identity", "54"},
          std::pair{"thread-identity-value",
                    emulation::darwin_test::ThreadIdentityHex},
          std::pair{"thread-identity-missing", "21"},
          std::pair{"mach-self-ports", "4a"},
          std::pair{"owner-queries", "50"},
          std::pair{"ordinary-queries", "47"},
          std::pair{"ordinary-queries-closed-groups", "474e"},
          std::pair{"ordinary-query-unknown", "21"},
          std::pair{"ordinary-query-open", "21"},
          std::pair{"ordinary-query-map", "21"},
          std::pair{"mach-self-port-values",
                    emulation::darwin_test::MachSelfPortsHex},
          std::pair{"mach-self-port-missing-thread", "21"},
          std::pair{"mach-self-port-missing-task", "21"},
          std::pair{"mach-self-port-missing-host", "21"},
          std::pair{"entropy-replay", "52"},
          std::pair{"entropy-missing", "21"},
          std::pair{"entropy-exhausted", "21"},
          std::pair{"entropy-mismatch", "21"},
          std::pair{"entropy-partial", "21"},
          std::pair{"login-buffer", "4c"},
          std::pair{"virtual-login-buffer",
                    emulation::darwin_test::LoginNameHex},
          std::pair{"mach-time", "68"},
          std::pair{"mach-timebase-values",
                    emulation::darwin_test::TimebaseHex},
          std::pair{"mach-clock-values", emulation::darwin_test::MachClockHex},
          std::pair{"symbolic-link-creation", "62"},
          std::pair{"symbolic-link-unlink", "55"},
          std::pair{"symbolic-link-unlink-protected", "55"},
          std::pair{"symbolic-link-rename", "52"},
          std::pair{"symbolic-link-rename-protected", "52"}}) {
      const bool X64 = llvm::StringRef(File).ends_with("x86_64");
      if (X64 && llvm::StringRef(Mode) == "mach-clock-values")
        continue;
      Cases.push_back({File, Profile, Mode, Expected});
    }
  return Cases;
}
class DarwinInputsPublic
    : public ProcessPublic,
      public testing::WithParamInterface<DarwinPublicCase> {};

TEST_P(DarwinInputsPublic, InputsAndMachReturnsAgreeAcrossSDKAndCLI) {
#ifndef NEVERD_DARWIN_FIXTURE_DIR
  GTEST_SKIP() << "Clang and ld64.lld Darwin fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const auto &[File, Profile, Mode, Expected] = GetParam();
  const llvm::StringRef ModeName(Mode);
  const bool UnlinkLinks = ModeName.starts_with("symbolic-link-unlink");
  const bool RenameLinks = ModeName.starts_with("symbolic-link-rename");
  const bool ProtectedLink =
      (UnlinkLinks || RenameLinks) && ModeName.ends_with("-protected");
  const bool UnknownPathConf = ModeName == "kernel-pathconf-unsupported";
  const bool UnknownAttributes = ModeName == "common-attributes-unsupported";
  const bool UnknownXattrMutation = ModeName == "xattr-mutations-unsupported";
  const bool UnknownXattrs = ModeName == "extended-attributes-unsupported";
  const bool UnknownNames = ModeName == "attribute-names-unsupported";
  const bool UnknownBulk = ModeName == "bulk-attributes-unsupported";
  const bool UnknownHardLinkName =
      ModeName == "symbolic-descriptors-name-unsupported" ||
      ModeName == "hard-links-name-unsupported" ||
      ModeName == "hard-links-attributes-unsupported";
  const bool UnknownNonblocking = ModeName == "nonblocking-flags-unsupported";
  const bool ThreadIdentity = ModeName.starts_with("thread-identity");
  const bool UnknownThreadIdentity = ModeName == "thread-identity-missing";
  const bool SelfPorts = ModeName.starts_with("mach-self-port");
  const bool UnknownSelfPort = ModeName.starts_with("mach-self-port-missing-");
  const bool OrdinaryQueries = ModeName.starts_with("ordinary-quer");
  const bool ClosedOrdinary = ModeName == "ordinary-queries-closed-groups";
  const bool UnknownOrdinary =
      OrdinaryQueries && ModeName != "ordinary-queries" && !ClosedOrdinary;
  const bool Entropy = ModeName.starts_with("entropy-");
  const bool UnknownEntropy = Entropy && ModeName != "entropy-replay";
  const bool Incomplete =
      UnknownThreadIdentity || UnknownSelfPort || UnknownEntropy ||
      ProtectedLink || UnknownPathConf || UnknownAttributes || UnknownXattrs ||
      UnknownNames || UnknownBulk || UnknownXattrMutation ||
      UnknownHardLinkName || UnknownNonblocking || UnknownOrdinary;
  const bool X64 = llvm::StringRef(File).ends_with("x86_64");
  SCOPED_TRACE(File);
  Path = (std::filesystem::path(NEVERD_DARWIN_FIXTURE_DIR) / File).string();
  SCOPED_TRACE(Mode);
  // This is a byte/ABI comparison. A separate process suite tests deadline
  // expiry; allow scheduling headroom here without changing product defaults.
  std::string Options =
      std::string(
          R"({"backend":"unicorn","timeout_microseconds":10000000,"darwin_time":)") +
      emulation::darwin_test::TimeJSON + R"(,"darwin_system":)" +
      ((ThreadIdentity && !UnknownThreadIdentity)
           ? emulation::darwin_test::ThreadIdentityJSON
       : (ModeName == "credentials" || ModeName == "virtual-credentials" ||
          ModeName == "created-file-metadata")
           ? emulation::darwin_test::CredentialsJSON
       : (ModeName == "resource-limits" ||
          ModeName == "virtual-resource-limits")
           ? emulation::darwin_test::ResourceLimitsJSON
       : (ModeName == "resource-usage" || ModeName == "virtual-resource-usage")
           ? emulation::darwin_test::ResourceUsageJSON
       : (ModeName == "hostname" || ModeName == "virtual-hostname")
           ? emulation::darwin_test::HostNameJSON
       : (ModeName == "process-observations" ||
          ModeName == "virtual-process-observations")
           ? emulation::darwin_test::ProcessObservationsJSON
       : (ModeName == "process-priority" ||
          ModeName == "virtual-process-priority")
           ? emulation::darwin_test::PriorityJSON
       : (ModeName == "login-buffer" || ModeName == "virtual-login-buffer")
           ? emulation::darwin_test::LoginNameJSON
           : emulation::darwin_test::SystemJSON) +
      R"(,"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839","metadata":)" +
      emulation::darwin_test::MetadataJSON +
      R"(}],"directories":[{"path":"/","contents":)" +
      emulation::darwin_test::DirectoryContentsJSON +
      R"(},{"path":"/empty"}],"working_directory":"/empty","stdin_hex":"00ff78","descriptor_limit":32},"arguments":["guest",")" +
      Mode + R"(","/data"]})";
  if (llvm::StringRef(Mode).starts_with("writable-files") ||
      llvm::StringRef(Mode) == "virtual-file-metadata" ||
      llvm::StringRef(Mode) == "sparse-file-seek" ||
      llvm::StringRef(Mode) == "unlinked-file" ||
      llvm::StringRef(Mode) == "created-file" ||
      llvm::StringRef(Mode) == "directory-mutations" ||
      llvm::StringRef(Mode) == "deleted-directories" ||
      llvm::StringRef(Mode) == "initial-directory-removal" ||
      llvm::StringRef(Mode) == "initial-directory-move" ||
      llvm::StringRef(Mode) == "initial-directory-swap" ||
      llvm::StringRef(Mode) == "created-file-metadata" ||
      llvm::StringRef(Mode) == "virtual-created-metadata" ||
      llvm::StringRef(Mode) == "renamed-file" ||
      llvm::StringRef(Mode) == "renamed-directory" ||
      llvm::StringRef(Mode) == "swapped-directory" ||
      llvm::StringRef(Mode) == "vectored-io") {
    auto Input = llvm::cantFail(llvm::json::parse(Options));
    auto *File = Input.getAsObject()
                     ->getObject(field::DarwinFiles)
                     ->getArray(field::Files)
                     ->front()
                     .getAsObject();
    (*File)[field::FileWritable] = true;
    (*File->getObject(field::FileMetadata))[field::FileFlags] = 0;
    if (llvm::StringRef(Mode) == "virtual-file-metadata" ||
        llvm::StringRef(Mode) == "sparse-file-seek" ||
        llvm::StringRef(Mode) == "unlinked-file" ||
        llvm::StringRef(Mode) == "created-file" ||
        llvm::StringRef(Mode) == "directory-mutations" ||
        llvm::StringRef(Mode) == "deleted-directories" ||
        llvm::StringRef(Mode) == "initial-directory-removal" ||
        llvm::StringRef(Mode) == "initial-directory-move" ||
        llvm::StringRef(Mode) == "initial-directory-swap" ||
        llvm::StringRef(Mode) == "created-file-metadata" ||
        llvm::StringRef(Mode) == "virtual-created-metadata" ||
        llvm::StringRef(Mode) == "renamed-file" ||
        llvm::StringRef(Mode) == "renamed-directory" ||
        llvm::StringRef(Mode) == "swapped-directory") {
      (*File->getObject(field::FileMetadata))[field::FileLinkCount] = 1;
      (*File)[field::FileMutationPolicy] = llvm::cantFail(
          llvm::json::parse(emulation::darwin_test::MutationPolicyJSON));
    }
    if (llvm::StringRef(Mode) == "unlinked-file" ||
        llvm::StringRef(Mode) == "created-file" ||
        llvm::StringRef(Mode) == "directory-mutations" ||
        llvm::StringRef(Mode) == "deleted-directories" ||
        llvm::StringRef(Mode) == "initial-directory-removal" ||
        llvm::StringRef(Mode) == "initial-directory-move" ||
        llvm::StringRef(Mode) == "initial-directory-swap" ||
        llvm::StringRef(Mode) == "created-file-metadata" ||
        llvm::StringRef(Mode) == "virtual-created-metadata" ||
        llvm::StringRef(Mode) == "renamed-file" ||
        llvm::StringRef(Mode) == "renamed-directory" ||
        llvm::StringRef(Mode) == "swapped-directory")
      (*Input.getAsObject()
            ->getObject(field::DarwinFiles)
            ->getArray(field::Directories)
            ->front()
            .getAsObject())[field::DirectoryMutable] = true;
    if (llvm::StringRef(Mode) == "renamed-file" ||
        llvm::StringRef(Mode) == "swapped-directory")
      (*Input.getAsObject()
            ->getObject(field::DarwinFiles)
            ->getArray(field::Directories)
            ->front()
            .getAsObject())[field::DirectorySwapRename] = true;
    if (llvm::StringRef(Mode) == "created-file-metadata" ||
        llvm::StringRef(Mode) == "virtual-created-metadata" ||
        llvm::StringRef(Mode) == "renamed-file" ||
        llvm::StringRef(Mode) == "renamed-directory" ||
        llvm::StringRef(Mode) == "swapped-directory") {
      auto *Files = Input.getAsObject()->getObject(field::DarwinFiles);
      (*Files)[field::FileUmask] = 0027;
      (*Files)[field::FileCreationPolicy] = llvm::cantFail(
          llvm::json::parse(emulation::darwin_test::CreationPolicyJSON));
      auto Parent = llvm::cantFail(
          llvm::json::parse(emulation::darwin_test::MetadataJSON));
      auto *M = Parent.getAsObject();
      (*M)[field::FileInode] = 41;
      (*M)[field::FileMode] = 0040755;
      (*M)[field::FileFlags] = 0;
      (*M)[field::FileLinkCount] = 1;
      (*M)[field::Size] = 0;
      (*M)[field::FileBlocks] = 0;
      (*Files->getArray(field::Directories)
            ->front()
            .getAsObject())[field::FileMetadata] = std::move(Parent);
    }
    if (llvm::StringRef(Mode) == "initial-directory-swap") {
      auto *Directories = Input.getAsObject()
                              ->getObject(field::DarwinFiles)
                              ->getArray(field::Directories);
      (*Directories->front().getAsObject())[field::DirectorySwapRename] = true;
      auto *Directory = Directories->back().getAsObject();
      (*Directory)[field::DirectoryMutable] = true;
      (*Directory)[field::DirectoryExchangeable] = true;
      (*Directory)[field::DirectorySwapRename] = true;
    }
    if (llvm::StringRef(Mode) == "initial-directory-move") {
      auto *Directory = Input.getAsObject()
                            ->getObject(field::DarwinFiles)
                            ->getArray(field::Directories)
                            ->back()
                            .getAsObject();
      (*Directory)[field::DirectoryMutable] = true;
      (*Directory)[field::DirectoryMovable] = true;
    }
    if (llvm::StringRef(Mode) == "initial-directory-removal")
      (*Input.getAsObject()
            ->getObject(field::DarwinFiles)
            ->getArray(field::Directories)
            ->back()
            .getAsObject())[field::DirectoryRemovable] = true;
    Options = llvm::formatv("{0}", Input).str();
  }
  if (ModeName == "created-namespace-metadata" ||
      ModeName == "virtual-created-namespace-metadata" ||
      ModeName == "mutable-initial-links" ||
      ModeName == "virtual-mutable-initial-links" ||
      ModeName == "initial-directory-metadata" ||
      ModeName == "virtual-initial-directory-metadata") {
    auto Input = llvm::cantFail(llvm::json::parse(Options));
    auto *Files = Input.getAsObject()->getObject(field::DarwinFiles);
    (*Input.getAsObject())[field::Quantum] = 1024;
    (*Files)[field::FileUmask] = 0027;
    (*Files)[field::FileCreationPolicy] = llvm::cantFail(
        llvm::json::parse(emulation::darwin_test::NamespaceCreationPolicyJSON));
    auto *M = Files->getArray(field::Files)
                  ->front()
                  .getAsObject()
                  ->getObject(field::FileMetadata);
    (*M)[field::FileFlags] = 0;
    (*M)[field::FileLinkCount] = 1;
    auto Parent =
        llvm::cantFail(llvm::json::parse(emulation::darwin_test::MetadataJSON));
    auto *PM = Parent.getAsObject();
    (*PM)[field::FileMode] = 0040755;
    (*PM)[field::FileInode] = 41;
    (*PM)[field::Size] = 0;
    (*PM)[field::FileBlocks] = 0;
    (*PM)[field::FileFlags] = 0;
    auto *Directory =
        Files->getArray(field::Directories)->front().getAsObject();
    (*Directory)[field::DirectoryMutable] = true;
    (*Directory)[field::DirectorySwapRename] = true;
    (*Directory)[field::FileMetadata] = std::move(Parent);
    Options = llvm::formatv("{0}", Input).str();
  }
  if (ModeName == "mutable-initial-links" ||
      ModeName == "virtual-mutable-initial-links") {
    auto Input = llvm::cantFail(llvm::json::parse(Options));
    auto *Files = Input.getAsObject()->getObject(field::DarwinFiles);
    auto *Root = Files->getArray(field::Directories)->front().getAsObject();
    Root->erase(field::DirectoryContents);
    auto M = *Files->getArray(field::Files)
                  ->front()
                  .getAsObject()
                  ->getObject(field::FileMetadata);
    M[field::FileMode] = 0120777;
    M[field::FileInode] = 57;
    M[field::Size] = 4;
    (*Files)[field::SymbolicLinks] = llvm::json::Array{
        llvm::json::Object{
            {field::Path, "/initial"},
            {field::SymbolicLinkTarget, "64617461"},
            {field::SymbolicLinkMutable, true},
            {field::FileMetadata, std::move(M)},
            {field::SymbolicLinkMutationPolicy,
             llvm::cantFail(llvm::json::parse(
                 emulation::darwin_test::InitialSymbolicLinkPolicyJSON))}},
        llvm::json::Object{{field::Path, "/initial-dir"},
                           {field::SymbolicLinkTarget, "656d707479"},
                           {field::SymbolicLinkMutable, true}}};
    auto Parent = *Root->getObject(field::FileMetadata);
    Parent[field::FileInode] = 42;
    (*Files->getArray(field::Directories)
          ->back()
          .getAsObject())[field::FileMetadata] = std::move(Parent);
    Options = llvm::formatv("{0}", Input).str();
  }
  if (ModeName == "directory-link-roots" ||
      ModeName == "virtual-directory-link-roots") {
    auto Input = llvm::cantFail(llvm::json::parse(Options));
    (*Input.getAsObject())[field::Quantum] = 1024;
    auto *Files = Input.getAsObject()->getObject(field::DarwinFiles);
    (*Files)[field::WorkingDirectory] = "/";
    (*Files)[field::FileUmask] = 0027;
    (*Files)[field::FileCreationPolicy] = llvm::cantFail(
        llvm::json::parse(emulation::darwin_test::NamespaceCreationPolicyJSON));
    auto Metadata = [](uint64_t Inode, uint16_t Mode, uint64_t Size,
                       uint64_t Blocks) {
      auto M = llvm::cantFail(
          llvm::json::parse(emulation::darwin_test::MetadataJSON));
      auto *Record = M.getAsObject();
      (*Record)[field::FileInode] = Inode;
      (*Record)[field::FileMode] = Mode;
      (*Record)[field::Size] = Size;
      (*Record)[field::FileBlocks] = Blocks;
      (*Record)[field::FileLinkCount] = 1;
      (*Record)[field::FileFlags] = 0;
      return M;
    };
    llvm::json::Array Directories;
    uint64_t Inode = 41;
    for (const char *Name : {"/", "/a", "/a/d", "/a/other", "/b", "/b/other"}) {
      const llvm::StringRef PathName(Name);
      llvm::json::Object D{
          {field::Path, Name},
          {field::FileMetadata, Metadata(Inode++, 0040755, 0, 0)}};
      if (PathName == "/" || PathName == "/a" || PathName == "/b" ||
          PathName == "/a/d")
        D[field::DirectoryMutable] = true;
      if (PathName == "/a" || PathName == "/b" || PathName == "/a/d") {
        D[field::DirectoryMovable] = true;
        D[field::DirectorySwapRename] = true;
      }
      if (PathName == "/a/d") {
        D[field::DirectoryExchangeable] = true;
        D[field::DirectoryMutationPolicy] = llvm::cantFail(llvm::json::parse(
            emulation::darwin_test::InitialDirectoryMutationPolicyJSON));
      }
      Directories.push_back(std::move(D));
    }
    (*Files)[field::Directories] = std::move(Directories);
    llvm::json::Array Contents;
    Inode = 101;
    for (const auto &[Name, Bytes] :
         {std::pair{"/a/d/c", "1f"}, std::pair{"/a/other/mark", "29"},
          std::pair{"/a/target", "0b"}, std::pair{"/b/other/mark", "2a"},
          std::pair{"/b/target", "16"}})
      Contents.push_back(llvm::json::Object{
          {field::Path, Name},
          {field::Bytes, Bytes},
          {field::FileMetadata, Metadata(Inode++, 0100644, 1, 8)}});
    (*Files)[field::Files] = std::move(Contents);
    llvm::json::Array Links;
    Inode = 57;
    for (const auto &[Name, Target] :
         {std::pair{"/b/l", "target"}, std::pair{"/b/dang", "missing"},
          std::pair{"/b/dirlink", "other"}, std::pair{"/b/self", "../a/d"},
          std::pair{"/a/d/inside", "../target"}}) {
      const llvm::StringRef RawTarget(Target);
      Links.push_back(llvm::json::Object{
          {field::Path, Name},
          {field::SymbolicLinkTarget, llvm::toHex(RawTarget)},
          {field::SymbolicLinkMutable, true},
          {field::FileMetadata,
           Metadata(Inode++, 0120777, RawTarget.size(), 8)},
          {field::SymbolicLinkMutationPolicy,
           llvm::cantFail(llvm::json::parse(
               emulation::darwin_test::InitialSymbolicLinkPolicyJSON))}});
    }
    (*Files)[field::SymbolicLinks] = std::move(Links);
    Options = llvm::formatv("{0}", Input).str();
  }
  if (ModeName == "initial-directory-metadata" ||
      ModeName == "virtual-initial-directory-metadata") {
    auto Input = llvm::cantFail(llvm::json::parse(Options));
    auto *Directory = Input.getAsObject()
                          ->getObject(field::DarwinFiles)
                          ->getArray(field::Directories)
                          ->front()
                          .getAsObject();
    (*Directory)[field::DirectoryMutationPolicy] =
        llvm::cantFail(llvm::json::parse(
            emulation::darwin_test::InitialDirectoryMutationPolicyJSON));
    Options = llvm::formatv("{0}", Input).str();
  }
  if (ModeName == "directory-enumeration-mutations" ||
      ModeName == "virtual-directory-enumeration") {
    auto Input = llvm::cantFail(llvm::json::parse(Options));
    (*Input.getAsObject())[field::Quantum] = 1024;
    auto *Files = Input.getAsObject()->getObject(field::DarwinFiles);
    (*Files)[field::FileUmask] = 0027;
    (*Files)[field::FileCreationPolicy] = llvm::cantFail(
        llvm::json::parse(emulation::darwin_test::NamespaceCreationPolicyJSON));
    auto *M = Files->getArray(field::Files)
                  ->front()
                  .getAsObject()
                  ->getObject(field::FileMetadata);
    (*M)[field::FileFlags] = 0;
    (*M)[field::FileLinkCount] = 1;
    auto Parent =
        llvm::cantFail(llvm::json::parse(emulation::darwin_test::MetadataJSON));
    auto *PM = Parent.getAsObject();
    (*PM)[field::FileMode] = 0040755;
    (*PM)[field::FileInode] = 41;
    (*PM)[field::Size] = 0;
    (*PM)[field::FileBlocks] = 0;
    (*PM)[field::FileFlags] = 0;
    auto *Directory =
        Files->getArray(field::Directories)->front().getAsObject();
    Directory->erase(field::DirectoryContents);
    (*Directory)[field::DirectoryMutable] = true;
    (*Directory)[field::DirectorySwapRename] = true;
    (*Directory)[field::FileMetadata] = std::move(Parent);
    (*Directory)[field::DirectoryEnumerationPolicy] = llvm::cantFail(
        llvm::json::parse(emulation::darwin_test::EnumerationPolicyJSON));
    Options = llvm::formatv("{0}", Input).str();
  }
  if (ModeName == "symbolic-links") {
    auto Input = llvm::cantFail(llvm::json::parse(Options));
    auto *Files = Input.getAsObject()->getObject(field::DarwinFiles);
    auto Links = llvm::cantFail(
        llvm::json::parse(emulation::darwin_test::SymbolicLinksJSON));
    auto M =
        llvm::cantFail(llvm::json::parse(emulation::darwin_test::MetadataJSON));
    (*M.getAsObject())[field::FileMode] = 0120777;
    (*M.getAsObject())[field::FileInode] = 123;
    (*M.getAsObject())[field::Size] = 4;
    (*Links.getAsArray()->front().getAsObject())[field::FileMetadata] =
        std::move(M);
    (*Files)[field::SymbolicLinks] = std::move(Links);
    (*Files)[field::WorkingDirectory] = "/";
    for (auto &D : *Files->getArray(field::Directories))
      D.getAsObject()->erase(field::DirectoryContents);
    Options = llvm::formatv("{0}", Input).str();
  }
  if (ModeName == "symbolic-link-mutations" ||
      ModeName == "symbolic-link-creation" || UnlinkLinks || RenameLinks) {
    auto Input = llvm::cantFail(llvm::json::parse(Options));
    auto M =
        llvm::cantFail(llvm::json::parse(emulation::darwin_test::MetadataJSON));
    (*M.getAsObject())[field::FileFlags] = 0;
    (*M.getAsObject())[field::FileLinkCount] = 1;
    auto Parent = M;
    (*Parent.getAsObject())[field::FileMode] = 0040755;
    (*Parent.getAsObject())[field::FileInode] = 41;
    (*Parent.getAsObject())[field::Size] = 0;
    (*Parent.getAsObject())[field::FileBlocks] = 0;
    auto Link = M;
    (*Link.getAsObject())[field::FileMode] = 0120777;
    (*Link.getAsObject())[field::FileInode] = 123;
    (*Link.getAsObject())[field::Size] = 12;
    auto Links = llvm::cantFail(
        llvm::json::parse(emulation::darwin_test::MixedSymbolicLinksJSON));
    (*(*Links.getAsArray())[1].getAsObject())[field::FileMetadata] =
        std::move(Link);
    (*Input.getAsObject())[field::DarwinFiles] = llvm::json::Object{
        {field::Files,
         llvm::json::Array{llvm::json::Object{
             {field::Path, "/work/data"},
             {field::Bytes, "30313233343536373839"},
             {field::FileMetadata, std::move(M)},
             {field::FileWritable, true},
             {field::FileMutationPolicy,
              llvm::cantFail(llvm::json::parse(
                  emulation::darwin_test::MutationPolicyJSON))}}}},
        {field::Directories,
         llvm::json::Array{
             llvm::json::Object{{field::Path, "/static"}},
             llvm::json::Object{{field::Path, "/work"},
                                {field::DirectoryMutable, true},
                                {field::FileMetadata, std::move(Parent)}}}},
        {field::SymbolicLinks, std::move(Links)},
        {field::WorkingDirectory, "/"},
        {field::FileUmask, 0027},
        {field::FileCreationPolicy,
         llvm::cantFail(
             llvm::json::parse(emulation::darwin_test::CreationPolicyJSON))}};
    if (RenameLinks)
      (*(*Input.getAsObject()
              ->getObject(field::DarwinFiles)
              ->getArray(field::Directories))[1]
            .getAsObject())[field::DirectorySwapRename] = true;
    (*Input.getAsObject()->getArray(field::Arguments))[2] = "/work/data";
    if (ProtectedLink) {
      auto *Arguments = Input.getAsObject()->getArray(field::Arguments);
      (*Arguments)[1] =
          RenameLinks ? "symbolic-link-rename" : "symbolic-link-unlink";
      Arguments->push_back("protected");
    }
    Options = llvm::formatv("{0}", Input).str();
  }
  if (ModeName.starts_with("common-attributes")) {
    auto Input = llvm::cantFail(llvm::json::parse(Options));
    (*Input.getAsObject())[field::Quantum] = 1024;
    (*Input.getAsObject())[field::DarwinFiles] = llvm::cantFail(
        llvm::json::parse(emulation::darwin_test::CommonAttributesJSON));
    Options = llvm::formatv("{0}", Input).str();
  }
  if (ModeName.starts_with("nonblocking-")) {
    auto Input = llvm::cantFail(llvm::json::parse(Options));
    (*Input.getAsObject())[field::Quantum] = 1024;
    (*Input.getAsObject())[field::DarwinFiles] = llvm::cantFail(
        llvm::json::parse(emulation::darwin_test::NonblockingDescriptorsJSON));
    Options = llvm::formatv("{0}", Input).str();
  }
  if (ModeName.starts_with("symbolic-descriptors")) {
    auto Input = llvm::cantFail(llvm::json::parse(Options));
    (*Input.getAsObject())[field::Quantum] = 1024;
    (*Input.getAsObject())[field::DarwinFiles] = llvm::cantFail(
        llvm::json::parse(emulation::darwin_test::SymbolicDescriptorJSON));
    Options = llvm::formatv("{0}", Input).str();
  }
  if (ModeName.starts_with("hard-links")) {
    auto Input = llvm::cantFail(llvm::json::parse(Options));
    (*Input.getAsObject())[field::Quantum] = 1024;
    (*Input.getAsObject())[field::DarwinFiles] = llvm::cantFail(
        llvm::json::parse(emulation::darwin_test::HardLinksJSON));
    Options = llvm::formatv("{0}", Input).str();
  }
  if (ModeName.starts_with("xattr-mutations")) {
    auto Input = llvm::cantFail(llvm::json::parse(Options));
    (*Input.getAsObject())[field::Quantum] = 1024;
    (*Input.getAsObject())[field::DarwinFiles] = llvm::cantFail(
        llvm::json::parse(emulation::darwin_test::XattrMutationsJSON));
    Options = llvm::formatv("{0}", Input).str();
  }
  if (ModeName.starts_with("bulk-attributes")) {
    auto Input = llvm::cantFail(llvm::json::parse(Options));
    (*Input.getAsObject())[field::Quantum] = 1024;
    (*Input.getAsObject())[field::DarwinFiles] = llvm::cantFail(
        llvm::json::parse(emulation::darwin_test::BulkAttributesJSON));
    Options = llvm::formatv("{0}", Input).str();
  }
  if (ModeName.starts_with("attribute-names")) {
    auto Input = llvm::cantFail(llvm::json::parse(Options));
    (*Input.getAsObject())[field::Quantum] = 1024;
    (*Input.getAsObject())[field::DarwinFiles] = llvm::cantFail(
        llvm::json::parse(emulation::darwin_test::AttributeNamesJSON));
    Options = llvm::formatv("{0}", Input).str();
  }
  if (ModeName.starts_with("extended-attributes")) {
    auto Input = llvm::cantFail(llvm::json::parse(Options));
    (*Input.getAsObject())[field::Quantum] = 1024;
    (*Input.getAsObject())[field::DarwinFiles] = llvm::cantFail(
        llvm::json::parse(emulation::darwin_test::ExtendedAttributesJSON));
    Options = llvm::formatv("{0}", Input).str();
  }
  if (ModeName.starts_with("kernel-pathconf")) {
    auto Input = llvm::cantFail(llvm::json::parse(Options));
    (*Input.getAsObject())[field::Quantum] = 1024;
    (*Input.getAsObject())[field::DarwinFiles] = llvm::cantFail(
        llvm::json::parse(emulation::darwin_test::KernelPathConfJSON));
    Options = llvm::formatv("{0}", Input).str();
  }
  if (OrdinaryQueries) {
    auto Input = llvm::cantFail(llvm::json::parse(Options));
    (*Input.getAsObject())[field::Quantum] = 1024;
    (*Input.getAsObject())[field::DarwinFiles] = llvm::cantFail(
        llvm::json::parse(emulation::darwin_test::OrdinaryQueriesJSON));
    (*Input.getAsObject())[field::DarwinSystem] =
        llvm::cantFail(llvm::json::parse(
            ClosedOrdinary
                ? emulation::darwin_test::ClosedGroupQueryCredentialsJSON
                : emulation::darwin_test::OrdinaryQueryCredentialsJSON));
    if (ClosedOrdinary) {
      auto Extra = llvm::cantFail(llvm::json::parse(
          emulation::darwin_test::ClosedGroupQueriesExtraJSON));
      auto *Files = Input.getAsObject()->getObject(field::DarwinFiles);
      for (const char *Key :
           {field::Files, field::Directories, field::SymbolicLinks})
        for (auto &Entry : *Extra.getAsObject()->getArray(Key))
          Files->getArray(Key)->push_back(std::move(Entry));
    }
    (*Input.getAsObject()->getArray(field::Arguments))[2] = "/unknown";
    Options = llvm::formatv("{0}", Input).str();
  }
  if (ModeName == "owner-queries") {
    auto Input = llvm::cantFail(llvm::json::parse(Options));
    (*Input.getAsObject())[field::Quantum] = 1024;
    (*Input.getAsObject())[field::DarwinFiles] = llvm::cantFail(
        llvm::json::parse(emulation::darwin_test::OwnerQueriesJSON));
    (*Input.getAsObject())[field::DarwinSystem] = llvm::cantFail(
        llvm::json::parse(emulation::darwin_test::OwnerQueryCredentialsJSON));
    Options = llvm::formatv("{0}", Input).str();
  }
  if (SelfPorts) {
    auto Input = llvm::cantFail(llvm::json::parse(Options));
    (*Input.getAsObject())[field::Quantum] = 1024;
    auto System = llvm::cantFail(
        llvm::json::parse(emulation::darwin_test::MachSelfPortsJSON));
    if (UnknownSelfPort) {
      const auto Selection = ModeName.drop_front(
          llvm::StringRef("mach-self-port-missing-").size());
      const auto Selected = Selection == "thread" ? field::SystemThreadSelfPort
                            : Selection == "task" ? field::SystemTaskSelfPort
                                                  : field::SystemHostSelfPort;
      System.getAsObject()->erase(Selected);
      (*System.getAsObject())[field::ThreadID] = "18446744073709551615";
      auto *Arguments = Input.getAsObject()->getArray(field::Arguments);
      (*Arguments)[1] = "mach-self-port-missing";
      (*Arguments)[2] = Selection;
    }
    (*Input.getAsObject())[field::DarwinSystem] = std::move(System);
    Options = llvm::formatv("{0}", Input).str();
  }
  if (ThreadIdentity) {
    auto Input = llvm::cantFail(llvm::json::parse(Options));
    (*Input.getAsObject())[field::Quantum] = 1024;
    Options = llvm::formatv("{0}", Input).str();
  }
  if (Entropy) {
    auto Input = llvm::cantFail(llvm::json::parse(Options));
    auto System = llvm::cantFail(
        llvm::json::parse(emulation::darwin_test::entropyReplayJSON()));
    if (ModeName == "entropy-missing")
      System.getAsObject()->erase(field::SystemEntropyReads);
    if (ModeName == "entropy-exhausted") {
      auto *Reads = System.getAsObject()->getArray(field::SystemEntropyReads);
      while (Reads->size() > 1)
        Reads->pop_back();
    }
    (*Input.getAsObject())[field::DarwinSystem] = std::move(System);
    (*Input.getAsObject())[field::Quantum] = 1024;
    Options = llvm::formatv("{0}", Input).str();
  }
  auto Text = takeString(neverd_emulate_process_json(Session, Path.c_str(),
                                                     Profile, Options.c_str()));
  ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
  auto Report = llvm::json::parse(Text);
  ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError());
  EXPECT_EQ(Report->getAsObject()->getString(field::Stop),
            Incomplete ? "unsupported_service" : "exited");
  if (Incomplete) {
    ASSERT_NE(Report->getAsObject()->get(field::ExitStatus), nullptr);
    EXPECT_EQ(*Report->getAsObject()->get(field::ExitStatus),
              llvm::json::Value(nullptr));
  } else {
    EXPECT_EQ(Report->getAsObject()->getInteger(field::ExitStatus), 37);
  }
  EXPECT_EQ(Report->getAsObject()->getString(field::Stdout), Expected);
  if (OrdinaryQueries) {
    const auto *Events = Report->getAsObject()->getArray(field::Services);
    ASSERT_NE(Events, nullptr);
    EXPECT_EQ(Report->getAsObject()->getString(field::Stderr), "");
    if (UnknownOrdinary) {
      ASSERT_EQ(Events->size(), 2u);
      const bool Membership = ModeName == "ordinary-query-unknown";
      EXPECT_EQ(
          Report->getAsObject()->getString(field::Diagnostic),
          Membership
              ? "Darwin ordinary authorization requires known group membership"
              : "Darwin static ordinary queries do not authorize other vnode "
                "operations");
      const auto *Last = Events->back().getAsObject();
      ASSERT_NE(Last, nullptr);
      const unsigned Number = Membership                  ? 466
                              : ModeName.ends_with("map") ? 197
                                                          : 5;
      EXPECT_EQ(Last->getString(field::Number),
                llvm::utohexstr(Number | (X64 ? 0x2000000 : 0), true));
      EXPECT_EQ(Last->get(field::Error), nullptr);
      ASSERT_NE(Last->get(field::Result), nullptr);
      EXPECT_EQ(*Last->get(field::Result), llvm::json::Value(nullptr));
    } else {
      ASSERT_EQ(Events->size(), ClosedOrdinary ? 173u : 146u);
      constexpr uint64_t Read[] = {0, 13, 13, 13, 0, 13, 13, 13};
      constexpr uint64_t None[] = {0, 13, 13, 13, 13, 13, 13, 13};
      constexpr uint64_t RW[] = {0, 13, 0, 13, 0, 13, 0, 13};
      constexpr uint64_t X[] = {0, 0, 13, 13, 13, 13, 13, 13};
      for (unsigned API = 0; API != 3; ++API)
        for (unsigned Request = 0; Request != 8; ++Request) {
          const uint64_t Expected[] = {
              Read[Request], API == 2 ? RW[Request] : None[Request], X[Request],
              API == 2 ? None[Request] : Read[Request]};
          for (unsigned File = 0; File != 4; ++File) {
            const auto *E =
                (*Events)[API * 32 + Request * 4 + File].getAsObject();
            ASSERT_NE(E, nullptr);
            EXPECT_EQ(E->getString(field::Result),
                      llvm::utohexstr(Expected[File], true));
            EXPECT_EQ(E->getBoolean(field::Error), Expected[File] != 0);
            EXPECT_EQ(E->getString(field::Number),
                      llvm::utohexstr((API ? 466 : 33) | (X64 ? 0x2000000 : 0),
                                      true));
          }
        }
      constexpr uint64_t Remaining[] = {
          0,  13, 13, 13, 0,  13, 13, 13, 0,  13, 13, 13, 0, 13, 13, 13,
          0,  13, 13, 0,  13, 13, 0,  13, 13, 13, 13, 13, 0, 2,  0,  13,
          13, 13, 13, 13, 0,  2,  0,  2,  0,  0,  0,  0,  0, 2};
      for (unsigned I = 0; I != std::size(Remaining); ++I) {
        const auto *E = (*Events)[96 + I].getAsObject();
        ASSERT_NE(E, nullptr);
        EXPECT_EQ(E->getString(field::Result),
                  llvm::utohexstr(Remaining[I], true));
        EXPECT_EQ(E->getBoolean(field::Error), Remaining[I] != 0);
      }
      EXPECT_EQ((*Events)[142].getAsObject()->getString(field::Number),
                X64 ? "1234567802000021" : "1234567800000021");
      if (ClosedOrdinary) {
        for (unsigned I = 0; I != 2; ++I) {
          const auto *E = (*Events)[146 + I].getAsObject();
          EXPECT_EQ(E->getString(field::Number), X64 ? "20001d2" : "1d2");
          EXPECT_EQ(E->getString(field::Result), "d");
          EXPECT_EQ(E->getBoolean(field::Error), true);
          const auto *Args = E->getArray(field::Arguments);
          ASSERT_NE(Args, nullptr);
          EXPECT_EQ((*Args)[2].getAsString(), I ? "6" : "2");
          EXPECT_EQ((*Args)[3].getAsString(), "10");
        }
        constexpr uint64_t Search[] = {2, 0, 0, 0, 13, 13, 0, 0};
        for (unsigned API = 0; API != 3; ++API)
          for (unsigned I = 0; I != 8; ++I) {
            const auto *E = (*Events)[148 + API * 8 + I].getAsObject();
            EXPECT_EQ(E->getString(field::Number),
                      llvm::utohexstr((API ? 466 : 33) | (X64 ? 0x2000000 : 0),
                                      true));
            EXPECT_EQ(E->getString(field::Result),
                      llvm::utohexstr(Search[I], true));
            EXPECT_EQ(E->getBoolean(field::Error), Search[I] != 0);
          }
        EXPECT_EQ((*Events)[172].getAsObject()->getString(field::Result), "1");
      }
      auto Repeated = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), Profile, Options.c_str()));
      auto Again = llvm::json::parse(Repeated);
      ASSERT_TRUE(bool(Again)) << llvm::toString(Again.takeError());
      EXPECT_EQ(*Again, *Report);
      if (ClosedOrdinary) {
        auto StringInput = llvm::cantFail(llvm::json::parse(Options));
        auto *Credential = StringInput.getAsObject()
                               ->getObject(field::DarwinSystem)
                               ->getObject(field::SystemCredentials);
        (*Credential)[field::CredentialGroupMembershipUID] = "4294967195";
        const auto StringOptions = llvm::formatv("{0}", StringInput).str();
        auto StringResult =
            llvm::json::parse(takeString(neverd_emulate_process_json(
                Session, Path.c_str(), Profile, StringOptions.c_str())));
        ASSERT_TRUE(bool(StringResult))
            << llvm::toString(StringResult.takeError());
        EXPECT_EQ(*StringResult, *Report);
        for (auto ID : {0u, uint32_t(INT32_MAX)})
          for (bool String : {false, true}) {
            auto Unknown = llvm::cantFail(llvm::json::parse(Options));
            auto *C = Unknown.getAsObject()
                          ->getObject(field::DarwinSystem)
                          ->getObject(field::SystemCredentials);
            (*C)[field::CredentialGroupMembershipUID] =
                String ? llvm::json::Value(std::to_string(ID))
                       : llvm::json::Value(ID);
            (*Unknown.getAsObject()->getArray(field::Arguments))[1] =
                "ordinary-query-unknown";
            const auto Request = llvm::formatv("{0}", Unknown).str();
            auto Stopped =
                llvm::json::parse(takeString(neverd_emulate_process_json(
                    Session, Path.c_str(), Profile, Request.c_str())));
            ASSERT_TRUE(bool(Stopped)) << llvm::toString(Stopped.takeError());
            EXPECT_EQ(Stopped->getAsObject()->getString(field::Stop),
                      "unsupported_service");
            EXPECT_EQ(Stopped->getAsObject()->getString(field::Stdout), "21");
            EXPECT_EQ(Stopped->getAsObject()->getString(field::Diagnostic),
                      "Darwin ordinary authorization requires known group "
                      "membership");
            const auto *Calls =
                Stopped->getAsObject()->getArray(field::Services);
            ASSERT_NE(Calls, nullptr);
            ASSERT_EQ(Calls->size(), 2u);
            EXPECT_EQ(Calls->back().getAsObject()->get(field::Error), nullptr);
            EXPECT_EQ(*Calls->back().getAsObject()->get(field::Result),
                      llvm::json::Value(nullptr));
            EXPECT_EQ(neverd_session_is_loaded(Session), 0);
          }
      }
    }
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  }
  if (SelfPorts) {
    EXPECT_EQ(Report->getAsObject()->getString(field::Stderr), "");
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    if (UnknownSelfPort) {
      const auto Selection = ModeName.drop_front(
          llvm::StringRef("mach-self-port-missing-").size());
      const unsigned Q = Selection == "thread" ? 0
                         : Selection == "task" ? 1
                                               : 2;
      EXPECT_EQ(Report->getAsObject()->getString(field::Diagnostic),
                "Darwin current-" + Selection.str() +
                    " Mach port observation is not configured");
      const auto *Events = Report->getAsObject()->getArray(field::Services);
      ASSERT_NE(Events, nullptr);
      ASSERT_EQ(Events->size(), 2u);
      const auto *Last = Events->back().getAsObject();
      ASSERT_NE(Last, nullptr);
      EXPECT_EQ(Last->getString(field::Number),
                llvm::utohexstr(X64 ? uint64_t(0x1000000 | (27 + Q))
                                    : uint64_t(-int64_t(27 + Q)),
                                true));
      EXPECT_EQ(Last->get(field::Error), nullptr);
      EXPECT_EQ(Last->get(field::ThreadID), nullptr);
      ASSERT_NE(Last->get(field::Result), nullptr);
      EXPECT_EQ(*Last->get(field::Result), llvm::json::Value(nullptr));
    } else {
      const auto check = [&](const llvm::json::Value &Value,
                             const std::array<llvm::StringRef, 3> &Results) {
        const auto *Events = Value.getAsObject()->getArray(field::Services);
        ASSERT_NE(Events, nullptr);
        ASSERT_EQ(Events->size(), 22u);
        for (unsigned Q = 0; Q != 3; ++Q) {
          const uint32_t Low = X64 ? 0x1000000 | (27 + Q) : 0u - (27 + Q);
          for (unsigned I = 0; I != 7; ++I) {
            const auto *Event = (*Events)[Q * 7 + I].getAsObject();
            ASSERT_NE(Event, nullptr);
            const uint64_t Prefix = !I      ? (X64 ? 0 : 0xffffffff00000000ULL)
                                    : I < 3 ? 0
                                    : I < 5 ? 0x1234567800000000ULL
                                            : 0xffffffff00000000ULL;
            EXPECT_EQ(Event->getString(field::Number),
                      llvm::utohexstr(Prefix | Low, true));
            EXPECT_EQ(Event->getString(field::Result), Results[Q]);
            EXPECT_EQ(Event->get(field::Error), nullptr);
            EXPECT_EQ(Event->get(field::ThreadID), nullptr);
            const auto *Args = Event->getArray(field::Arguments);
            ASSERT_NE(Args, nullptr);
            ASSERT_EQ(Args->size(), 6u);
            EXPECT_EQ((*Args)[0].getAsString(), "ffffffffffffffff");
            EXPECT_EQ((*Args)[X64 ? 2 : 1].getAsString(), "1122334455667788");
            EXPECT_EQ((*Args)[X64 ? 1 : 2].getAsString(), "8877665544332211");
          }
        }
        const auto *Write = Events->back().getAsObject();
        ASSERT_NE(Write, nullptr);
        const auto Size = ModeName == "mach-self-ports" ? "1" : "18";
        EXPECT_EQ(Write->getString(field::Number), X64 ? "2000004" : "4");
        EXPECT_EQ(Write->getString(field::Result), Size);
        EXPECT_EQ(Write->getBoolean(field::Error), false);
        EXPECT_EQ(Write->get(field::ThreadID), nullptr);
        const auto *Args = Write->getArray(field::Arguments);
        ASSERT_NE(Args, nullptr);
        ASSERT_EQ(Args->size(), 6u);
        EXPECT_EQ((*Args)[0].getAsString(), "1");
        EXPECT_EQ((*Args)[2].getAsString(), Size);
      };
      check(*Report, {"ffffffff80000001", "0", "ffffffffffffffff"});
      auto Repeated = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), Profile, Options.c_str()));
      auto Again = llvm::json::parse(Repeated);
      ASSERT_TRUE(bool(Again)) << llvm::toString(Again.takeError());
      EXPECT_EQ(*Again, *Report);
      if (ModeName == "mach-self-port-values") {
        struct Sample {
          const char *Wire, *Hex, *Result;
        };
        for (const auto &S :
             {Sample{"0", "0000000000000000", "0"},
              Sample{"1", "0100000000000000", "1"},
              Sample{"2147483647", "ffffff7f00000000", "7fffffff"},
              Sample{"2147483648", "00000080ffffffff", "ffffffff80000000"},
              Sample{"\"2147483649\"", "01000080ffffffff", "ffffffff80000001"},
              Sample{"4294967295", "ffffffffffffffff", "ffffffffffffffff"},
              Sample{"\"4294967295\"", "ffffffffffffffff",
                     "ffffffffffffffff"}}) {
          SCOPED_TRACE(S.Wire);
          auto Input = llvm::cantFail(llvm::json::parse(Options));
          for (const char *Field :
               {field::SystemThreadSelfPort, field::SystemTaskSelfPort,
                field::SystemHostSelfPort})
            (*Input.getAsObject()->getObject(field::DarwinSystem))[Field] =
                llvm::cantFail(llvm::json::parse(S.Wire));
          const auto Request = llvm::formatv("{0}", Input).str();
          auto Text = takeString(neverd_emulate_process_json(
              Session, Path.c_str(), Profile, Request.c_str()));
          auto Exact = llvm::json::parse(Text);
          ASSERT_TRUE(bool(Exact)) << llvm::toString(Exact.takeError());
          EXPECT_EQ(Exact->getAsObject()->getString(field::Stop), "exited");
          EXPECT_EQ(Exact->getAsObject()->getInteger(field::ExitStatus), 37);
          EXPECT_EQ(Exact->getAsObject()->getString(field::Stdout),
                    std::string(S.Hex) + S.Hex + S.Hex);
          check(*Exact, {S.Result, S.Result, S.Result});
          EXPECT_EQ(runCLIRequest(Path, Profile, Request, Output),
                    process_cli::GuestFailure);
          ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, *Exact));
          EXPECT_EQ(neverd_session_is_loaded(Session), 0);
        }
      }
    }
  }
  if (ThreadIdentity) {
    EXPECT_EQ(Report->getAsObject()->getString(field::Stderr), "");
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    const auto *Services = Report->getAsObject()->getArray(field::Services);
    ASSERT_NE(Services, nullptr);
    if (UnknownThreadIdentity) {
      EXPECT_EQ(Report->getAsObject()->getString(field::Diagnostic),
                "Darwin current-thread identity observation is not configured");
      ASSERT_EQ(Services->size(), 2u);
      const auto *Last = Services->back().getAsObject();
      ASSERT_NE(Last, nullptr);
      EXPECT_EQ(Last->getString(field::Number), X64 ? "2000174" : "174");
      EXPECT_EQ(Last->get(field::Error), nullptr);
      EXPECT_EQ(Last->get(field::ThreadID), nullptr);
      ASSERT_NE(Last->get(field::Result), nullptr);
      EXPECT_EQ(*Last->get(field::Result), llvm::json::Value(nullptr));
    } else {
      const auto check = [&](const llvm::json::Value &Value,
                             llvm::StringRef ExpectedValue) {
        const auto *Events = Value.getAsObject()->getArray(field::Services);
        ASSERT_NE(Events, nullptr);
        ASSERT_EQ(Events->size(), 11u);
        for (unsigned I = 0; I != 10; ++I) {
          const auto *Event = (*Events)[I].getAsObject();
          ASSERT_NE(Event, nullptr);
          EXPECT_EQ(Event->getString(field::Number),
                    I < 4   ? (X64 ? "2000174" : "174")
                    : I < 7 ? (X64 ? "1234567802000174" : "1234567800000174")
                            : (X64 ? "ffffffff02000174" : "ffffffff00000174"));
          EXPECT_EQ(Event->getString(field::Result), ExpectedValue);
          EXPECT_EQ(Event->getBoolean(field::Error), false);
          EXPECT_EQ(Event->get(field::ThreadID), nullptr);
        }
        const auto *Write = Events->back().getAsObject();
        ASSERT_NE(Write, nullptr);
        const auto Size = ModeName == "thread-identity" ? "1" : "8";
        EXPECT_EQ(Write->getString(field::Number), X64 ? "2000004" : "4");
        EXPECT_EQ(Write->getString(field::Result), Size);
        EXPECT_EQ(Write->getBoolean(field::Error), false);
        EXPECT_EQ(Write->get(field::ThreadID), nullptr);
        const auto *WriteArgs = Write->getArray(field::Arguments);
        ASSERT_NE(WriteArgs, nullptr);
        ASSERT_EQ(WriteArgs->size(), 6u);
        EXPECT_EQ((*WriteArgs)[0].getAsString(), "1");
        EXPECT_EQ((*WriteArgs)[2].getAsString(), Size);
        const auto *Arguments =
            Events->front().getAsObject()->getArray(field::Arguments);
        ASSERT_NE(Arguments, nullptr);
        EXPECT_EQ(*Arguments,
                  (llvm::json::Array{"ffffffffffffffff", "8000000000000000",
                                     "1122334455667788", "1",
                                     "ffffffffffffffff", "123456789abcdef0"}));
      };
      check(*Report, "fedcba9876543210");
      auto Repeated = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), Profile, Options.c_str()));
      auto Again = llvm::json::parse(Repeated);
      ASSERT_TRUE(bool(Again)) << llvm::toString(Again.takeError());
      EXPECT_EQ(*Again, *Report);
      if (ModeName == "thread-identity-value") {
        struct Sample {
          const char *Wire, *Hex, *Result;
        };
        for (const auto &S : {Sample{"0", "0000000000000000", "0"},
                              Sample{"\"9223372036854775808\"",
                                     "0000000000000080", "8000000000000000"},
                              Sample{"\"18446744073709551615\"",
                                     "ffffffffffffffff", "ffffffffffffffff"},
                              Sample{"9007199254740991", "ffffffffffff1f00",
                                     "1fffffffffffff"}}) {
          SCOPED_TRACE(S.Wire);
          auto Input = llvm::cantFail(llvm::json::parse(Options));
          (*Input.getAsObject()->getObject(
              field::DarwinSystem))[field::ThreadID] =
              llvm::cantFail(llvm::json::parse(S.Wire));
          const auto Request = llvm::formatv("{0}", Input).str();
          auto Text = takeString(neverd_emulate_process_json(
              Session, Path.c_str(), Profile, Request.c_str()));
          auto Exact = llvm::json::parse(Text);
          ASSERT_TRUE(bool(Exact)) << llvm::toString(Exact.takeError());
          EXPECT_EQ(Exact->getAsObject()->getString(field::Stop), "exited");
          EXPECT_EQ(Exact->getAsObject()->getInteger(field::ExitStatus), 37);
          EXPECT_EQ(Exact->getAsObject()->getString(field::Stdout), S.Hex);
          check(*Exact, S.Result);
          auto Repeat = takeString(neverd_emulate_process_json(
              Session, Path.c_str(), Profile, Request.c_str()));
          auto Repeated = llvm::json::parse(Repeat);
          ASSERT_TRUE(bool(Repeated)) << llvm::toString(Repeated.takeError());
          EXPECT_EQ(*Repeated, *Exact);
          EXPECT_EQ(runCLIRequest(Path, Profile, Request, Output),
                    process_cli::GuestFailure);
          ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, *Exact));
          EXPECT_EQ(neverd_session_is_loaded(Session), 0);
        }
      }
    }
  }
  if (Entropy) {
    EXPECT_EQ(Report->getAsObject()->getString(field::Stderr), "");
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    const auto *Services = Report->getAsObject()->getArray(field::Services);
    ASSERT_NE(Services, nullptr);
    if (UnknownEntropy) {
      const char *Reason =
          ModeName == "entropy-missing"
              ? "Darwin entropy observations are not configured"
          : ModeName == "entropy-exhausted"
              ? "Darwin entropy observations are exhausted"
          : ModeName == "entropy-mismatch"
              ? "Darwin entropy observation length does not match the request"
              : "Darwin partial entropy output is unsupported";
      EXPECT_EQ(Report->getAsObject()->getString(field::Diagnostic), Reason);
      ASSERT_FALSE(Services->empty());
      const auto *Last = Services->back().getAsObject();
      ASSERT_NE(Last, nullptr);
      EXPECT_EQ(Last->getString(field::Number), X64 ? "20001f4" : "1f4");
      EXPECT_EQ(Last->get(field::Error), nullptr);
      ASSERT_NE(Last->get(field::Result), nullptr);
      EXPECT_EQ(*Last->get(field::Result), llvm::json::Value(nullptr));
    } else {
      std::vector<const llvm::json::Object *> Calls;
      for (const auto &Event : *Services) {
        const auto *Call = Event.getAsObject();
        ASSERT_NE(Call, nullptr);
        if (Call->getString(field::Number) == (X64 ? "20001f4" : "1f4"))
          Calls.push_back(Call);
      }
      ASSERT_EQ(Calls.size(), 28u);
      for (unsigned I = 0; I != 28; ++I) {
        const bool Zero = I < 24 && I % 6 == 0;
        EXPECT_EQ(Calls[I]->getString(field::Result), I < 24
                                                          ? (Zero ? "0" : "16")
                                                      : I == 24 ? "e"
                                                                : "0");
        EXPECT_EQ(Calls[I]->getBoolean(field::Error),
                  (I < 24 && !Zero) || I == 24);
      }
      // Reuse the identical public options on the same session. Each run
      // must start at observation zero, including the whole-EFAULT record.
      auto Repeated = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), Profile, Options.c_str()));
      auto Again = llvm::json::parse(Repeated);
      ASSERT_TRUE(bool(Again)) << llvm::toString(Again.takeError());
      EXPECT_EQ(*Again, *Report);
      auto Maximum = llvm::cantFail(llvm::json::parse(Options));
      auto *Reads = Maximum.getAsObject()
                        ->getObject(field::DarwinSystem)
                        ->getArray(field::SystemEntropyReads);
      while (Reads->size() < 256)
        Reads->push_back("00");
      const auto MaximumJSON = llvm::formatv("{0}", Maximum).str();
      auto MaximumText = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), Profile, MaximumJSON.c_str()));
      auto MaximumReport = llvm::json::parse(MaximumText);
      ASSERT_TRUE(bool(MaximumReport))
          << llvm::toString(MaximumReport.takeError());
      EXPECT_EQ(*MaximumReport, *Report);
      // Empty JSON is known exhausted, distinct from omission.
      (*Maximum.getAsObject()->getArray(field::Arguments))[1] =
          "entropy-missing";
      (*Maximum.getAsObject()->getObject(
          field::DarwinSystem))[field::SystemEntropyReads] =
          llvm::json::Array{};
      const auto EmptyJSON = llvm::formatv("{0}", Maximum).str();
      auto EmptyText = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), Profile, EmptyJSON.c_str()));
      auto EmptyReport = llvm::json::parse(EmptyText);
      ASSERT_TRUE(bool(EmptyReport)) << llvm::toString(EmptyReport.takeError());
      EXPECT_EQ(EmptyReport->getAsObject()->getString(field::Stop),
                "unsupported_service");
      EXPECT_EQ(EmptyReport->getAsObject()->getString(field::Diagnostic),
                "Darwin entropy observations are exhausted");
      EXPECT_EQ(EmptyReport->getAsObject()->getString(field::Stdout), "21");
      EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    }
  }
  if (UnknownNonblocking) {
    EXPECT_EQ(Report->getAsObject()->getString(field::Diagnostic),
              "unsupported Darwin fcntl command");
    const auto *Services = Report->getAsObject()->getArray(field::Services);
    ASSERT_NE(Services, nullptr);
    ASSERT_FALSE(Services->empty());
    const auto *Last = Services->back().getAsObject();
    ASSERT_NE(Last, nullptr);
    EXPECT_EQ(Last->getString(field::Number), X64 ? "200005c" : "5c");
    EXPECT_EQ(Last->get(field::Error), nullptr);
    ASSERT_NE(Last->get(field::Result), nullptr);
    EXPECT_EQ(*Last->get(field::Result), llvm::json::Value(nullptr));
  }
  if (UnknownHardLinkName) {
    EXPECT_EQ(Report->getAsObject()->getString(field::Diagnostic),
              "Darwin multiple-name vnode observations are unsupported");
    const auto *Services = Report->getAsObject()->getArray(field::Services);
    ASSERT_NE(Services, nullptr);
    ASSERT_FALSE(Services->empty());
    const auto *Last = Services->back().getAsObject();
    ASSERT_NE(Last, nullptr);
    EXPECT_EQ(Last->getString(field::Number),
              ModeName == "hard-links-name-unsupported" ||
                      ModeName == "symbolic-descriptors-name-unsupported"
                  ? (X64 ? "200005c" : "5c")
                  : (X64 ? "20000e4" : "e4"));
    EXPECT_EQ(Last->get(field::Error), nullptr);
    ASSERT_NE(Last->get(field::Result), nullptr);
    EXPECT_EQ(*Last->get(field::Result), llvm::json::Value(nullptr));
    EXPECT_EQ(Report->getAsObject()->getString(field::Stderr), "");
  }
  if (UnknownAttributes || UnknownNames) {
    EXPECT_EQ(Report->getAsObject()->getString(field::Diagnostic),
              UnknownNames ? "Darwin object name is outside the bounded UTF-8 "
                             "catalogue contract"
                           : "Darwin selected file attributes are not modeled");
    const auto *Services = Report->getAsObject()->getArray(field::Services);
    ASSERT_NE(Services, nullptr);
    ASSERT_FALSE(Services->empty());
    const auto *Last = Services->back().getAsObject();
    ASSERT_NE(Last, nullptr);
    EXPECT_EQ(Last->getString(field::Number), X64 ? "20000dc" : "dc");
    EXPECT_EQ(Last->get(field::Error), nullptr);
    ASSERT_NE(Last->get(field::Result), nullptr);
    EXPECT_EQ(*Last->get(field::Result), llvm::json::Value(nullptr));
    EXPECT_EQ(Report->getAsObject()->getString(field::Stderr), "");
  }
  if (UnknownBulk) {
    EXPECT_EQ(Report->getAsObject()->getString(field::Diagnostic),
              "Darwin selected file attributes are not modeled");
    const auto *Services = Report->getAsObject()->getArray(field::Services);
    ASSERT_NE(Services, nullptr);
    ASSERT_FALSE(Services->empty());
    const auto *Last = Services->back().getAsObject();
    ASSERT_NE(Last, nullptr);
    EXPECT_EQ(Last->getString(field::Number), X64 ? "20001cd" : "1cd");
    EXPECT_EQ(Last->get(field::Error), nullptr);
    ASSERT_NE(Last->get(field::Result), nullptr);
    EXPECT_EQ(*Last->get(field::Result), llvm::json::Value(nullptr));
  }
  if (UnknownXattrMutation) {
    EXPECT_EQ(Report->getAsObject()->getString(field::Diagnostic),
              "Darwin extended-attribute mutation is not authorized");
    const auto *Services = Report->getAsObject()->getArray(field::Services);
    ASSERT_NE(Services, nullptr);
    ASSERT_FALSE(Services->empty());
    const auto *Last = Services->back().getAsObject();
    EXPECT_EQ(Last->getString(field::Number), X64 ? "20000ec" : "ec");
    EXPECT_EQ(Last->get(field::Error), nullptr);
    ASSERT_NE(Last->get(field::Result), nullptr);
    EXPECT_EQ(*Last->get(field::Result), llvm::json::Value(nullptr));
  }
  if (UnknownXattrs) {
    EXPECT_EQ(Report->getAsObject()->getString(field::Diagnostic),
              "Darwin extended-attribute observations are unknown");
    const auto *Services = Report->getAsObject()->getArray(field::Services);
    ASSERT_NE(Services, nullptr);
    ASSERT_FALSE(Services->empty());
    const auto *Last = Services->back().getAsObject();
    ASSERT_NE(Last, nullptr);
    EXPECT_EQ(Last->getString(field::Number), X64 ? "20000ea" : "ea");
    EXPECT_EQ(Last->get(field::Error), nullptr);
    ASSERT_NE(Last->get(field::Result), nullptr);
    EXPECT_EQ(*Last->get(field::Result), llvm::json::Value(nullptr));
  }
  if (UnknownPathConf) {
    const auto *Services = Report->getAsObject()->getArray(field::Services);
    ASSERT_NE(Services, nullptr);
    ASSERT_FALSE(Services->empty());
    const auto *Last = Services->back().getAsObject();
    ASSERT_NE(Last, nullptr);
    EXPECT_EQ(Last->getString(field::Number), X64 ? "20000bf" : "bf");
    EXPECT_EQ(Last->get(field::Error), nullptr);
    ASSERT_NE(Last->get(field::Result), nullptr);
    EXPECT_EQ(*Last->get(field::Result), llvm::json::Value(nullptr));
    EXPECT_EQ(Report->getAsObject()->getString(field::Stderr), "");
  }
  if (UnlinkLinks) {
    // The guest validates target FD/map identity before emitting U. Also
    // require actual removal and repeated-removal errors in the public trace.
    const auto *Services = Report->getAsObject()->getArray(field::Services);
    ASSERT_NE(Services, nullptr);
    bool Removed = false, Missing = false;
    for (const auto &Service : *Services) {
      const auto *Event = Service.getAsObject();
      ASSERT_NE(Event, nullptr);
      const auto Number = Event->getString(field::Number);
      if (Number != (X64 ? "200000a" : "a") &&
          Number != (X64 ? "20001d8" : "1d8"))
        continue;
      Removed |= Event->getBoolean(field::Error) == false &&
                 Event->getString(field::Result) == "0";
      Missing |= Event->getBoolean(field::Error) == true &&
                 Event->getString(field::Result) == "2";
    }
    EXPECT_TRUE(Removed);
    EXPECT_TRUE(Missing);
    EXPECT_EQ(Report->getAsObject()->getString(field::Stderr), "");
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    if (ProtectedLink) {
      ASSERT_FALSE(Services->empty());
      const auto *Last = Services->back().getAsObject();
      ASSERT_NE(Last, nullptr);
      EXPECT_EQ(Last->getString(field::Number), X64 ? "200000a" : "a");
      ASSERT_NE(Last->get(field::Result), nullptr);
      EXPECT_EQ(*Last->get(field::Result), llvm::json::Value(nullptr));
      EXPECT_EQ(Last->get(field::Error), nullptr);
    }
  }
  if (RenameLinks) {
    const auto *Services = Report->getAsObject()->getArray(field::Services);
    ASSERT_NE(Services, nullptr);
    bool Renamed = false, Exclusive = false, NoExpansion = false;
    bool Swapped = false, SwappedNoExpansion = false, SwapMissing = false,
         SwapInvalid = false, SubtreeNonempty = false;
    for (const auto &Service : *Services) {
      const auto *Event = Service.getAsObject();
      ASSERT_NE(Event, nullptr);
      const auto Number = Event->getString(field::Number);
      if (Number != (X64 ? "2000080" : "80") &&
          Number != (X64 ? "20001d1" : "1d1") &&
          Number != (X64 ? "20001e8" : "1e8"))
        continue;
      Renamed |= Event->getBoolean(field::Error) == false &&
                 Event->getString(field::Result) == "0";
      SubtreeNonempty |= Number == (X64 ? "20001d1" : "1d1") &&
                         Event->getBoolean(field::Error) == true &&
                         Event->getString(field::Result) == "42";
      if (Number == (X64 ? "20001e8" : "1e8")) {
        const auto *Arguments = Event->getArray(field::Arguments);
        ASSERT_NE(Arguments, nullptr);
        ASSERT_GE(Arguments->size(), 5u);
        const bool Success = Event->getBoolean(field::Error) == false &&
                             Event->getString(field::Result) == "0";
        Swapped |= Success && (*Arguments)[4].getAsString() == "2";
        SwappedNoExpansion |= Success && (*Arguments)[4].getAsString() == "12";
        SwapMissing |= Event->getBoolean(field::Error) == true &&
                       Event->getString(field::Result) == "2";
        SwapInvalid |= Event->getBoolean(field::Error) == true &&
                       Event->getString(field::Result) == "16";
        Exclusive |= Event->getBoolean(field::Error) == true &&
                     Event->getString(field::Result) == "11";
        NoExpansion |= Event->getBoolean(field::Error) == true &&
                       Event->getString(field::Result) == "3e";
      }
    }
    EXPECT_TRUE(Renamed);
    EXPECT_TRUE(Exclusive);
    EXPECT_TRUE(NoExpansion);
    EXPECT_TRUE(Swapped);
    EXPECT_TRUE(SwappedNoExpansion);
    EXPECT_TRUE(SwapMissing);
    EXPECT_TRUE(SwapInvalid);
    EXPECT_TRUE(SubtreeNonempty);
    EXPECT_EQ(Report->getAsObject()->getString(field::Stderr), "");
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    if (ProtectedLink) {
      ASSERT_FALSE(Services->empty());
      const auto *Last = Services->back().getAsObject();
      ASSERT_NE(Last, nullptr);
      EXPECT_EQ(Last->getString(field::Number), X64 ? "2000080" : "80");
      ASSERT_NE(Last->get(field::Result), nullptr);
      EXPECT_EQ(*Last->get(field::Result), llvm::json::Value(nullptr));
      EXPECT_EQ(Last->get(field::Error), nullptr);
    }
  }
  if (ModeName == "mach-time" || ModeName == "mach-timebase-values" ||
      ModeName == "mach-clock-values") {
    const auto *Services = Report->getAsObject()->getArray(field::Services);
    ASSERT_NE(Services, nullptr);
    const bool Clocks = llvm::StringRef(Mode) == "mach-clock-values";
    ASSERT_GE(Services->size(), Clocks ? 3u : 13u);
    for (unsigned I = 0; I != (Clocks ? 2u : 12u); ++I)
      EXPECT_EQ((*Services)[I].getAsObject()->get(field::Error), nullptr);
    EXPECT_EQ(Services->back().getAsObject()->getBoolean(field::Error), false);
    if (Clocks) {
      EXPECT_EQ((*Services)[0].getAsObject()->getString(field::Result),
                "fedcba9876543210");
      EXPECT_EQ((*Services)[1].getAsObject()->getString(field::Result),
                "ffffffffffffffff");
    } else {
      EXPECT_EQ((*Services)[5].getAsObject()->getString(field::Number),
                X64 ? "1234567801000059" : "12345678ffffffa7");
    }
  }
  const int CLIStatus = runCLIRequest(Path, Profile, Options, Output);
  EXPECT_EQ(CLIStatus,
            Incomplete ? process_cli::Incomplete : process_cli::GuestFailure);
  ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, *Report));
#endif
}
INSTANTIATE_TEST_SUITE_P(
    Platforms, DarwinInputsPublic, testing::ValuesIn(darwinPublicCases()),
    [](const testing::TestParamInfo<DarwinPublicCase> &Info) {
      std::string Name = std::string(Info.param.File) + "_" + Info.param.Mode;
      for (char &C : Name)
        if (C == '-')
          C = '_';
      return Name;
    });

TEST_F(ProcessPublic, DarwinOwnerQueriesRejectContradictionsBeforeLoading) {
  for (const char *Bad :
       {"null", "true", "0", "1.5", "{}", "[]", "\"\"", "\"owner\"",
        "\"StaticOwnerQueries\"", "\"StaticOrdinaryQueries\"",
        "\"static-owner-queries\\u0000\"",
        "\"static-ordinary-queries\\u0000\""}) {
    const auto Request = std::string("{\"darwin_files\":{\"files\":[],") +
                         "\"authorization\":" + Bad + "}}";
    for (const char *Profile :
         {MacOSMachO64, IOSMachO64, IOSSimulatorMachO64}) {
      EXPECT_EQ(neverd_emulate_process_json(Session, "missing.macho", Profile,
                                            Request.c_str()),
                nullptr);
      EXPECT_NE(takeString(neverd_last_error(Session)).find("authorization"),
                std::string::npos);
      EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    }
  }
  for (const char *Authorization :
       {field::StaticOwnerQueries, field::StaticOrdinaryQueries}) {
    for (unsigned Change = 0; Change != 6; ++Change) {
      SCOPED_TRACE(Change);
      auto Files = llvm::cantFail(llvm::json::parse(
          llvm::StringRef(Authorization) == field::StaticOwnerQueries
              ? emulation::darwin_test::OwnerQueriesJSON
              : emulation::darwin_test::OrdinaryQueriesJSON));
      auto &Object = *Files.getAsObject();
      auto *File = (*Object.getArray(field::Files))[0].getAsObject();
      auto *Directory = (*Object.getArray(field::Directories))[0].getAsObject();
      if (Change == 0)
        (*File)[field::FileWritable] = true;
      else if (Change == 1)
        (*Directory)[field::DirectoryMutable] = true;
      else if (Change == 2)
        (*File->getObject(field::FileMetadata))[field::FileFlags] = 1;
      else if (Change == 3)
        (*File->getObject(field::FileMetadata))[field::FileMode] = 35072;
      else if (Change == 4)
        (*File->getObject(field::FileMetadata))[field::FileInode] = 1;
      else
        Object[field::FileCreationPolicy] = llvm::json::Object{};
      llvm::json::Object RequestOptions;
      RequestOptions[field::DarwinFiles] = std::move(Files);
      const auto Request = jsonText(std::move(RequestOptions));
      EXPECT_EQ(neverd_emulate_process_json(Session, "missing.macho",
                                            MacOSMachO64, Request.c_str()),
                nullptr);
      const auto Error = takeString(neverd_last_error(Session));
      EXPECT_TRUE(llvm::StringRef(Error).contains(
          Change == 5 ? field::FileCreationPolicy
          : llvm::StringRef(Authorization) == field::StaticOwnerQueries
              ? "Darwin static owner queries"
              : "Darwin static ordinary queries"))
          << Error;
      EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    }
    llvm::json::Object ValidOptions;
    ValidOptions[field::DarwinFiles] = llvm::cantFail(llvm::json::parse(
        llvm::StringRef(Authorization) == field::StaticOwnerQueries
            ? emulation::darwin_test::OwnerQueriesJSON
            : emulation::darwin_test::OrdinaryQueriesJSON));
    const auto Valid = jsonText(std::move(ValidOptions));
    for (const char *Profile :
         {LinuxELF64, WindowsPE64, AndroidNativeAArch64}) {
      EXPECT_EQ(neverd_emulate_process_json(Session, "missing.macho", Profile,
                                            Valid.c_str()),
                nullptr);
      EXPECT_EQ(takeString(neverd_last_error(Session)),
                field::DarwinFilesProfile);
      EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    }
  }
}

TEST_F(ProcessPublic,
       DarwinMembershipUIDRejectsMalformedCredentialsBeforeLoading) {
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  auto RequestFor = [](const char *Key, const char *Wire) {
    return std::string(
               R"({"darwin_system":{"credentials":{"real_uid":501,"effective_uid":502,"real_gid":30,"effective_gid":20,"groups":[20,40],")") +
           Key + R"(":)" + Wire + "}}}";
  };
  for (const char *Bad : {"null",
                          "true",
                          "false",
                          "{}",
                          "[]",
                          "0.5",
                          "-1",
                          "2147483648",
                          "4294967194",
                          "4294967196",
                          "4294967295",
                          "4294967296",
                          R"("-1")",
                          R"("1.5")",
                          R"("0x10")",
                          R"("x")",
                          R"("")",
                          R"(" 0")",
                          R"("0 ")",
                          R"("2147483648")",
                          R"("4294967194")",
                          R"("4294967196")",
                          R"("4294967295")",
                          R"("4294967296")",
                          R"("1\u0000")"}) {
    SCOPED_TRACE(Bad);
    const auto Request = RequestFor(field::CredentialGroupMembershipUID, Bad);
    for (const char *Profile :
         {MacOSMachO64, IOSMachO64, IOSSimulatorMachO64}) {
      EXPECT_EQ(neverd_emulate_process_json(Session, "missing.macho", Profile,
                                            Request.c_str()),
                nullptr);
      const auto Error = takeString(neverd_last_error(Session));
      EXPECT_TRUE(llvm::StringRef(Error).contains("credentials") ||
                  llvm::StringRef(Error).contains("group membership UID"))
          << Error;
      EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    }
    if (llvm::StringRef(Bad) == "null" ||
        llvm::StringRef(Bad) == "4294967194" ||
        llvm::StringRef(Bad) == R"("4294967296")") {
      EXPECT_EQ(
          runCLIRequest("missing.macho", MacOSMachO64, Request, Output, true),
          process_cli::Error);
      auto Error = llvm::MemoryBuffer::getFile(Output);
      ASSERT_TRUE(bool(Error)) << Error.getError().message();
      EXPECT_TRUE((*Error)->getBuffer().contains("credentials") ||
                  (*Error)->getBuffer().contains("group membership UID"));
    }
  }
  // NONE is an independent membership sentinel, not an ordinary UID/GID.
  for (const char *Key :
       {field::CredentialRealUID, field::CredentialEffectiveUID,
        field::CredentialRealGID, field::CredentialEffectiveGID,
        field::CredentialGroups}) {
    auto System = llvm::cantFail(llvm::json::parse(
        emulation::darwin_test::ClosedGroupQueryCredentialsJSON));
    auto *Credentials =
        System.getAsObject()->getObject(field::SystemCredentials);
    ASSERT_NE(Credentials, nullptr);
    (*Credentials)[Key] =
        llvm::StringRef(Key) == field::CredentialGroups
            ? llvm::json::Value(llvm::json::Array{20, 4294967195LL})
            : llvm::json::Value(4294967195LL);
    llvm::json::Object RequestOptions;
    RequestOptions[field::DarwinSystem] = std::move(System);
    const auto Request = jsonText(std::move(RequestOptions));
    EXPECT_EQ(neverd_emulate_process_json(Session, "missing.macho",
                                          MacOSMachO64, Request.c_str()),
              nullptr);
    EXPECT_NE(takeString(neverd_last_error(Session)).find("Darwin credentials"),
              std::string::npos);
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  }
  for (const char *Key :
       {"GroupMembershipUID", "group_membership_uid\\u0000"}) {
    const auto Request = RequestFor(Key, "4294967195");
    EXPECT_EQ(neverd_emulate_process_json(Session, "missing.macho",
                                          MacOSMachO64, Request.c_str()),
              nullptr);
    EXPECT_NE(takeString(neverd_last_error(Session)).find("credentials"),
              std::string::npos);
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  }
  for (const char *Profile : {LinuxELF64, WindowsPE64, AndroidNativeAArch64}) {
    const auto Request =
        RequestFor(field::CredentialGroupMembershipUID, "4294967195");
    EXPECT_EQ(neverd_emulate_process_json(Session, "missing.macho", Profile,
                                          Request.c_str()),
              nullptr);
    EXPECT_EQ(takeString(neverd_last_error(Session)),
              field::DarwinSystemProfile);
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  }
}

TEST_F(ProcessPublic,
       DarwinMachSelfPortsRejectMalformedObservationsBeforeLoading) {
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (const char *Field :
       {field::SystemThreadSelfPort, field::SystemTaskSelfPort,
        field::SystemHostSelfPort}) {
    for (const char *Bad :
         {"null", "true", "false", "{}", "[]", "1.5", "-1", "4294967296",
          "9007199254740992", "18446744073709551615", "\"-1\"", "\"1.5\"",
          "\"0x10\"", "\"x\"", "\"\"", "\"4294967296\"",
          "\"18446744073709551615\"", "\"1\\u0000\""}) {
      const auto Request =
          std::string("{\"darwin_system\":{\"") + Field + "\":" + Bad + "}}";
      for (const char *Profile :
           {MacOSMachO64, IOSMachO64, IOSSimulatorMachO64}) {
        EXPECT_EQ(neverd_emulate_process_json(Session, "missing.macho", Profile,
                                              Request.c_str()),
                  nullptr);
        EXPECT_NE(takeString(neverd_last_error(Session)).find(Field),
                  std::string::npos);
        EXPECT_EQ(neverd_session_is_loaded(Session), 0);
      }
      EXPECT_EQ(
          runCLIRequest("missing.macho", MacOSMachO64, Request, Output, true),
          process_cli::Error);
      auto Error = llvm::MemoryBuffer::getFile(Output);
      ASSERT_TRUE(bool(Error)) << Error.getError().message();
      EXPECT_TRUE((*Error)->getBuffer().contains(Field));
    }
    // A valid Darwin field still rejects a foreign profile before loading.
    const auto Foreign =
        std::string("{\"darwin_system\":{\"") + Field + "\":0}}";
    for (const char *Profile :
         {LinuxELF64, WindowsPE64, AndroidNativeAArch64}) {
      EXPECT_EQ(neverd_emulate_process_json(Session, "missing.macho", Profile,
                                            Foreign.c_str()),
                nullptr);
      EXPECT_EQ(takeString(neverd_last_error(Session)),
                field::DarwinSystemProfile);
      EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    }
  }
}

TEST_F(ProcessPublic,
       DarwinThreadIdentityRejectsMalformedObservationsBeforeLoading) {
  for (const char *Bad :
       {"null", "true", "false", "{}", "[]", "1.5", "-1", "9007199254740992",
        "18446744073709551616", "\"-1\"", "\"1.5\"", "\"0x10\"", "\"x\"",
        "\"\"", "\"18446744073709551616\"", "\"1\\u0000\""}) {
    const auto Request =
        std::string("{\"darwin_system\":{\"thread_id\":") + Bad + "}}";
    EXPECT_EQ(neverd_emulate_process_json(Session, "missing.macho",
                                          MacOSMachO64, Request.c_str()),
              nullptr);
    EXPECT_NE(takeString(neverd_last_error(Session)).find("thread_id"),
              std::string::npos);
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  }
  for (const char *Profile : {LinuxELF64, WindowsPE64, AndroidNativeAArch64}) {
    EXPECT_EQ(neverd_emulate_process_json(
                  Session, "missing.macho", Profile,
                  R"({"darwin_system":{"thread_id":"18446744073709551615"}})"),
              nullptr);
    EXPECT_EQ(takeString(neverd_last_error(Session)),
              field::DarwinSystemProfile);
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  }
}

TEST_F(ProcessPublic, DarwinEntropyRejectsMalformedReplayBeforeLoading) {
  for (const char *Bad :
       {"null", "true", "{}", "\"00\"", "[null]", "[true]", "[0]", "[\"\"]",
        "[\"0\"]", "[\"gg\"]", "[\"0x01\"]", "[\"00\\u0000\"]"}) {
    const auto Request =
        std::string("{\"darwin_system\":{\"entropy_reads\":") + Bad + "}}";
    EXPECT_EQ(neverd_emulate_process_json(Session, "missing.macho",
                                          MacOSMachO64, Request.c_str()),
              nullptr);
    EXPECT_NE(takeString(neverd_last_error(Session)).find("entropy_reads"),
              std::string::npos);
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  }
  for (const auto &Bad : {"[\"" + std::string(514, '0') + "\"]",
                          "[" + std::string("\"00\",") +
                              [] {
                                std::string Entries;
                                for (unsigned I = 0; I != 256; ++I)
                                  Entries += I ? ",\"00\"" : "\"00\"";
                                return Entries;
                              }() +
                              "]"}) {
    const auto Request = "{\"darwin_system\":{\"entropy_reads\":" + Bad + "}}";
    EXPECT_EQ(neverd_emulate_process_json(Session, "missing.macho",
                                          MacOSMachO64, Request.c_str()),
              nullptr);
    EXPECT_NE(takeString(neverd_last_error(Session)).find("entropy_reads"),
              std::string::npos);
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  }
  for (const char *Profile : {LinuxELF64, WindowsPE64, AndroidNativeAArch64}) {
    EXPECT_EQ(neverd_emulate_process_json(
                  Session, "missing.macho", Profile,
                  R"({"darwin_system":{"entropy_reads":[]}})"),
              nullptr);
    EXPECT_EQ(takeString(neverd_last_error(Session)),
              field::DarwinSystemProfile);
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  }
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
           "4"},
          {R"({"backend":"unicorn","android":{"entry_symbol":"default_call","libraries":{"libfixture.so":["strlen"]},"default_scope":["libfixture.so"]}})",
           "6"},
          {R"({"backend":"unicorn","android":{"entry_symbol":"dynamic_identities","arguments":["0x20000000",0],"memory":[{"address":"0x20000000","size":4096}],"read_memory":[{"address":"0x20000000","size":16}],"libraries":{"libidentity.so":["getuid","geteuid","getgid","getegid"]}}})",
           "0"}}) {
    auto Text = takeString(neverd_emulate_process_json(
        Session, Path.c_str(), AndroidNativeAArch64, Request.c_str()));
    ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
    auto Parsed = llvm::cantFail(llvm::json::parse(Text));
    EXPECT_EQ(Parsed.getAsObject()->getString(field::Stop), "returned");
    EXPECT_EQ(Parsed.getAsObject()->getString(field::ReturnValue), Expected);
    if (Expected == "4" || Expected == "6") {
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
    if (Expected == "0") {
      const auto *Android = Parsed.getAsObject()->getObject(field::Android);
      ASSERT_NE(Android, nullptr);
      for (const char *Name : {"getuid", "geteuid", "getgid", "getegid"}) {
        const llvm::json::Object *Lookup = nullptr, *Call = nullptr;
        for (const auto &Event : *Android->getArray(field::NativeCalls)) {
          const auto *E = Event.getAsObject();
          if (E->getString(field::Name) == "dlsym" &&
              E->getString(field::Symbol) == Name)
            Lookup = E;
          if (E->getString(field::Name) == Name)
            Call = E;
        }
        ASSERT_NE(Lookup, nullptr);
        ASSERT_NE(Call, nullptr);
        EXPECT_EQ(Lookup->getString(field::Library), "libidentity.so");
        EXPECT_EQ(Call->getString(field::Library), "libidentity.so");
        EXPECT_EQ(Call->getString(field::PC), Lookup->getString(field::Result));
        EXPECT_EQ(Call->getString(field::Result), "3e8");
      }
    }
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    llvm::SmallString<128> Directory;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
    const std::filesystem::path Root(Directory.str().str());
    auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
    const auto Output = (Root / OutputFile).string();
    const int CLIStatus =
        runCLIRequest(Path, AndroidNativeAArch64, Request, Output);
    EXPECT_EQ(CLIStatus, process_cli::Success);
    ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Parsed));
  }
#endif
}

TEST_F(ProcessPublic, AndroidTerminalCPUStateMatchesSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  Path = (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) / "android.so")
             .string();
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (const char *Entry : {"add_arguments", "unknown_call"}) {
    SCOPED_TRACE(Entry);
    const bool Returned = llvm::StringRef(Entry) == "add_arguments";
    llvm::json::Value Baseline = nullptr;
    for (bool Observe : {false, true}) {
      llvm::json::Object Options;
      Options[field::Backend] = emulation::execution::Unicorn;
      Options[field::Android] = llvm::json::Object{
          {field::EntrySymbol, Entry},
          {field::Arguments, llvm::json::Array{1, 2, 3, 4, 5, 6, 7, 8, 9, 10}},
          {field::ObserveCPUState, Observe}};
      const auto Request = jsonText(std::move(Options));
      auto Text = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), AndroidNativeAArch64, Request.c_str()));
      ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
      auto Parsed = llvm::cantFail(llvm::json::parse(Text));
      EXPECT_EQ(Parsed.getAsObject()->getString(field::Stop),
                Returned ? "returned" : "unsupported_service");
      EXPECT_EQ(runCLIRequest(Path, AndroidNativeAArch64, Request, Output),
                Returned ? process_cli::Success : process_cli::Incomplete);
      ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Parsed));
      auto *Android = Parsed.getAsObject()->getObject(field::Android);
      ASSERT_NE(Android, nullptr);
      if (!Observe) {
        EXPECT_FALSE(Android->get(field::CPUState));
        Baseline = std::move(Parsed);
        continue;
      }
      const auto *State = Android->getObject(field::CPUState);
      ASSERT_NE(State, nullptr);
      EXPECT_EQ(State->getInteger(field::ThreadID), 1000);
      EXPECT_EQ(State->getString(field::Diagnostic), "");
      const auto *Registers = State->getObject(field::ObservationRegisters);
      ASSERT_NE(Registers, nullptr);
      EXPECT_EQ(Registers->size(), 71u);
      const auto *X0 = Registers->getArray("X0");
      ASSERT_NE(X0, nullptr);
      EXPECT_EQ((*X0)[0].getAsString(), Returned ? "192" : "13");
      EXPECT_EQ((*X0)[1].getAsString(), "0");
      Android->erase(field::CPUState);
      EXPECT_EQ(Parsed, Baseline);
    }
  }
#endif
}

TEST_F(ProcessPublic, AndroidEnvironmentMatchesSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  std::vector<uint8_t> Bytes(4096, 0);
  llvm::support::endian::write64le(Bytes.data() + 256, 0x20000200);
  std::copy_n("COLOR=blue", 11, Bytes.data() + 512);
  std::copy_n("COLOR", 6, Bytes.data() + 1024);
  for (const char *Fixture : {"environment-O0-none", "environment-O2-relr"}) {
    Path = (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
            (std::string(Fixture) + ".so"))
               .string();
    for (unsigned Mode = 0; Mode != 4; ++Mode) {
      SCOPED_TRACE(testing::Message() << Fixture << ":" << Mode);
      llvm::json::Object Native{
          {"entry_symbol", "environment_query"},
          {"initialize", false},
          {"arguments", llvm::json::Array{"0x20000000", "0x20000400", 0}},
          {"memory", llvm::json::Array{llvm::json::Object{
                         {"address", "0x20000000"},
                         {"size", 4096},
                         {"bytes_hex", llvm::toHex(Bytes)}}}},
          {"read_memory", llvm::json::Array{llvm::json::Object{
                              {"address", "0x20000000"}, {"size", 4096}}}},
          {"observe_memory_on_failure", true}};
      if (Mode)
        Native["environment_vector"] = Mode == 1   ? "0"
                                       : Mode == 2 ? "0x20000100"
                                                   : "0xffffffffffffffff";
      const auto Request = jsonText(llvm::json::Object{
          {"backend", "unicorn"}, {"android", std::move(Native)}});
      auto Text = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), AndroidNativeAArch64, Request.c_str()));
      ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
      auto Report = llvm::cantFail(llvm::json::parse(Text));
      const bool Returned = Mode == 1 || Mode == 2;
      EXPECT_EQ(Report.getAsObject()->getString(field::Stop),
                Returned ? "returned"
                : Mode   ? "runtime_failure"
                         : "unsupported_service");
      if (Returned)
        EXPECT_EQ(Report.getAsObject()->getString(field::ReturnValue), "0");
      EXPECT_EQ(runCLIRequest(Path, AndroidNativeAArch64, Request, Output),
                Returned ? process_cli::Success : process_cli::Incomplete);
      ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Report));
    }
  }
#endif
}

TEST_F(ProcessPublic, AndroidDladdrObservationsMatchSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (const char *Opt : {"O0", "O2"})
    for (const char *Packing : {"none", "android", "relr"}) {
      Path = (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
              (std::string("dladdr-") + Opt + "-" + Packing + ".so"))
                 .string();
      SCOPED_TRACE(Path);
      for (bool Known : {false, true}) {
        SCOPED_TRACE(Known);
        llvm::json::Object Request{
            {"backend", "unicorn"},
            {"android",
             llvm::json::Object{
                 {"entry_symbol", "dladdr_call"},
                 {"initialize", false},
                 {"arguments",
                  llvm::json::Array{Known ? "0x40001004" : "0x40001005",
                                    "0x20000000"}},
                 {"memory", llvm::json::Array{llvm::json::Object{
                                {"address", "0x20000000"}, {"size", 4096}}}},
                 {"read_memory", llvm::json::Array{llvm::json::Object{
                                     {"address", "0x20000000"}, {"size", 64}}}},
                 {"dladdr_observations",
                  llvm::json::Array{
                      llvm::json::Object{{"address", "0x40001004"},
                                         {"image_base", "0x40000000"},
                                         {"image_name", "/guest/fixture.so"},
                                         {"symbol_name", "symbol"},
                                         {"symbol_address", "0x40001000"}}}}}}};
        const auto Options = jsonText(std::move(Request));
        auto Text = takeString(neverd_emulate_process_json(
            Session, Path.c_str(), AndroidNativeAArch64, Options.c_str()));
        ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
        auto Report = llvm::cantFail(llvm::json::parse(Text));
        EXPECT_EQ(Report.getAsObject()->getString(field::Stop),
                  Known ? "returned" : "unsupported_service");
        if (Known)
          EXPECT_EQ(Report.getAsObject()->getString(field::ReturnValue), "1");
        EXPECT_EQ(runCLIRequest(Path, AndroidNativeAArch64, Options, Output),
                  Known ? process_cli::Success : process_cli::Incomplete);
        ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Report));
      }
    }
#endif
}

TEST_F(ProcessPublic, AndroidFailureMemoryMatchesSDKAndCLIWithoutChangingStop) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  Path = (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) / "android.so")
             .string();
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (const char *Entry : {"stdio_content", "bad_free"}) {
    SCOPED_TRACE(Entry);
    llvm::json::Value Baseline = nullptr;
    for (bool Observe : {false, true}) {
      llvm::json::Object Request{
          {"backend", "unicorn"},
          {"android",
           llvm::json::Object{
               {"entry_symbol", Entry},
               {"initialize", false},
               {"observe_cpu_state", true},
               {"observe_memory_on_failure", Observe},
               {"memory", llvm::json::Array{llvm::json::Object{
                              {"address", "0x20000000"}, {"size", 4096}}}},
               {"read_memory",
                llvm::json::Array{
                    llvm::json::Object{{"address", "0x23000000"},
                                       {"size", 16},
                                       {"require_mapped_at_entry", false}},
                    llvm::json::Object{{"address", "0x20000000"},
                                       {"size", 64}}}}}}};
      const auto Options = jsonText(std::move(Request));
      auto Text = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), AndroidNativeAArch64, Options.c_str()));
      ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
      auto Report = llvm::cantFail(llvm::json::parse(Text));
      EXPECT_EQ(Report.getAsObject()->getString(field::Stop),
                llvm::StringRef(Entry) == "stdio_content" ? "cpu_failure"
                                                          : "runtime_failure");
      EXPECT_EQ(runCLIRequest(Path, AndroidNativeAArch64, Options, Output),
                process_cli::Incomplete);
      ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Report));
      auto *Android = Report.getAsObject()->getObject(field::Android);
      ASSERT_NE(Android, nullptr);
      auto *Memory = Android->getArray(field::Memory);
      ASSERT_NE(Memory, nullptr);
      if (!Observe) {
        EXPECT_TRUE(Memory->empty());
        Baseline = std::move(Report);
        continue;
      }
      ASSERT_EQ(Memory->size(), 2u);
      const auto *Missing = (*Memory)[0].getAsObject();
      ASSERT_NE(Missing, nullptr);
      EXPECT_EQ(Missing->getString(field::Bytes), "");
      ASSERT_TRUE(Missing->getString(field::Diagnostic));
      EXPECT_FALSE(Missing->getString(field::Diagnostic)->empty());
      const auto *Valid = (*Memory)[1].getAsObject();
      ASSERT_NE(Valid, nullptr);
      EXPECT_EQ(Valid->getString(field::Bytes), std::string(128, '0'));
      EXPECT_FALSE(Valid->get(field::Diagnostic));
      (*Android)[field::Memory] = llvm::json::Array{};
      EXPECT_EQ(Report, Baseline);
    }
  }
#endif
}

TEST_F(ProcessPublic, AndroidFinalizersKeepGuestEffectsAcrossSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  Path = (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
          "finalizers-O2-relr.so")
             .string();
  const std::string Request =
      R"({"backend":"unicorn","android":{"entry_symbol":"cxa_dynamic","arguments":["0x20000000",0],"initialize":false,"memory":[{"address":"0x20000000","size":4096}],"read_memory":[{"address":"0x20000000","size":256}],"libraries":{"libfinalize-model.so":["__cxa_atexit","__cxa_finalize"]}}})";
  auto Text = takeString(neverd_emulate_process_json(
      Session, Path.c_str(), AndroidNativeAArch64, Request.c_str()));
  ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
  auto Parsed = llvm::cantFail(llvm::json::parse(Text));
  EXPECT_EQ(Parsed.getAsObject()->getString(field::Stop), "returned");
  EXPECT_EQ(Parsed.getAsObject()->getString(field::ReturnValue), "49");
  const auto *Android = Parsed.getAsObject()->getObject(field::Android);
  ASSERT_NE(Android, nullptr);
  const llvm::json::Object *Lookup = nullptr, *Call = nullptr;
  for (const auto &Event : *Android->getArray(field::NativeCalls)) {
    const auto *E = Event.getAsObject();
    if (E->getString(field::Name) == "dlsym")
      Lookup = E;
    if (E->getString(field::Name) == "__cxa_finalize")
      Call = E;
  }
  ASSERT_NE(Lookup, nullptr);
  ASSERT_NE(Call, nullptr);
  EXPECT_EQ(Lookup->getString(field::Symbol), "__cxa_finalize");
  EXPECT_EQ(Call->getString(field::Library), "libfinalize-model.so");
  EXPECT_EQ(Call->getString(field::PC), Lookup->getString(field::Result));
  EXPECT_EQ(Call->getString(field::Result), "0");
  std::vector<uint8_t> Expected(256);
  llvm::support::endian::write64le(Expected.data(), 1);
  llvm::support::endian::write64le(Expected.data() + 48, 1000);
  llvm::support::endian::write64le(Expected.data() + 64, 0x100000009);
  const auto *Memory = Android->getArray(field::Memory);
  ASSERT_NE(Memory, nullptr);
  ASSERT_EQ(Memory->size(), 1u);
  EXPECT_EQ(Memory->front().getAsObject()->getString(field::Bytes),
            llvm::toHex(Expected, true));
  EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const int CLIStatus =
      runCLIRequest(Path, AndroidNativeAArch64, Request, Output);
  EXPECT_EQ(CLIStatus, process_cli::Success);
  ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Parsed));
#endif
}

TEST_F(ProcessPublic, AndroidNarrowScanningMatchesSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  struct Case {
    std::string Input, Format;
    uint32_t Return;
    bool Unsupported = false;
  };
  const Case Cases[] = {{" \tab cd!", "%2c%2s%n%c", 3},
                        {"ab", "%5c%n", 1},
                        {std::string(73, 'x'), "%s", 1},
                        {"abcdefghij", "%c%c%c%c%c%c%c%c%c%c", 10},
                        {" \t", "%s", UINT32_MAX},
                        {"xy", "%c%lc", 0, true}};
  for (const char *Variant : {"scan_text-O0-none", "scan_text-O2-relr"}) {
    Path = (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
            (std::string(Variant) + ".so"))
               .string();
    for (unsigned Mode = 0; Mode != 3; ++Mode)
      for (unsigned Index = 0; Index != std::size(Cases); ++Index) {
        SCOPED_TRACE(Variant);
        SCOPED_TRACE(Mode);
        SCOPED_TRACE(Index);
        const auto &Test = Cases[Index];
        auto Request = llvm::cantFail(llvm::json::parse(R"({
          "backend":"unicorn","instruction_quantum":7,
          "instruction_limit":500000,"event_limit":5000,
          "android":{"entry_symbol":"scan_text","initialize":false,
            "observe_memory_on_failure":true,
            "read_memory":[{"address":"0x20000000","size":8192}]}})"));
        auto &OptionsObject = *Request.getAsObject();
        auto &Android = *OptionsObject.getObject(field::Android);
        Android[field::Arguments] =
            llvm::json::Array{"0x20000000", "0x20000400", "0x20000800", Mode};
        std::vector<uint8_t> Expected(8192, 0xa5);
        auto Put = [&](unsigned Offset, llvm::StringRef Text) {
          std::copy(Text.bytes_begin(), Text.bytes_end(),
                    Expected.begin() + Offset);
          Expected[Offset + Text.size()] = 0;
        };
        Put(1024, Test.Input);
        Put(2048, Test.Format);
        Android[field::Memory] = llvm::json::Array{
            llvm::json::Object{{field::Address, "0x20000000"},
                               {field::Size, 8192},
                               {"bytes_hex", llvm::toHex(Expected, true)}}};
        const auto Options = jsonText(OptionsObject);
        auto Text = takeString(neverd_emulate_process_json(
            Session, Path.c_str(), AndroidNativeAArch64, Options.c_str()));
        ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
        auto Report = llvm::cantFail(llvm::json::parse(Text));
        auto *Object = Report.getAsObject();
        ASSERT_NE(Object, nullptr);
        ASSERT_EQ(Object->getString(field::Stop),
                  Test.Unsupported ? "unsupported_service" : "returned")
            << Text;
        if (Test.Unsupported)
          EXPECT_EQ(*Object->get(field::ReturnValue),
                    llvm::json::Value(nullptr));
        else
          EXPECT_EQ(Object->getString(field::ReturnValue),
                    llvm::utohexstr(Test.Return, true));
        if (Index == 0) {
          Expected[0] = ' ';
          Expected[1] = '\t';
          Put(64, "ab");
          llvm::support::endian::write32le(Expected.data() + 128, 4);
          Expected[192] = ' ';
        } else if (Index == 1) {
          Expected[0] = 'a';
          Expected[1] = 'b';
          llvm::support::endian::write32le(Expected.data() + 64, 2);
        } else if (Index == 2) {
          Put(0, Test.Input);
        } else if (Index == 3) {
          for (unsigned I = 0; I != 10; ++I)
            Expected[I * 64] = 'a' + I;
        }
        const auto *Observed = Object->getObject(field::Android);
        ASSERT_NE(Observed, nullptr);
        const auto *Memory = Observed->getArray(field::Memory);
        ASSERT_NE(Memory, nullptr);
        ASSERT_EQ(Memory->size(), 1u);
        EXPECT_EQ(Memory->front().getAsObject()->getString(field::Bytes),
                  llvm::toHex(Expected, true));
        EXPECT_EQ(runCLIRequest(Path, AndroidNativeAArch64, Options, Output),
                  Test.Unsupported ? process_cli::Incomplete
                                   : process_cli::Success);
        ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Report));
      }
  }
#endif
}

TEST_F(ProcessPublic, AndroidFormattingKeepsDynamicNamesAcrossSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  Path =
      (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) / "format-O2-relr.so")
          .string();
  const std::string Request =
      R"({"backend":"unicorn","android":{"entry_symbol":"format_dynamic","arguments":["0x20000000",0],"initialize":false,"memory":[{"address":"0x20000000","size":4096}],"read_memory":[{"address":"0x20000000","size":64}],"libraries":{"libformat-model.so":["snprintf"]}}})";
  auto Text = takeString(neverd_emulate_process_json(
      Session, Path.c_str(), AndroidNativeAArch64, Request.c_str()));
  ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
  auto Parsed = llvm::cantFail(llvm::json::parse(Text));
  EXPECT_EQ(Parsed.getAsObject()->getString(field::Stop), "returned");
  EXPECT_EQ(Parsed.getAsObject()->getString(field::ReturnValue), "12");
  const auto *Android = Parsed.getAsObject()->getObject(field::Android);
  ASSERT_NE(Android, nullptr);
  const llvm::json::Object *Lookup = nullptr, *Call = nullptr;
  for (const auto &Event : *Android->getArray(field::NativeCalls)) {
    const auto *E = Event.getAsObject();
    if (E->getString(field::Name) == "dlsym")
      Lookup = E;
    if (E->getString(field::Name) == "snprintf")
      Call = E;
  }
  ASSERT_NE(Lookup, nullptr);
  ASSERT_NE(Call, nullptr);
  EXPECT_EQ(Lookup->getString(field::Symbol), "snprintf");
  EXPECT_EQ(Call->getString(field::Library), "libformat-model.so");
  EXPECT_EQ(Call->getString(field::PC), Lookup->getString(field::Result));
  EXPECT_EQ(Call->getString(field::Result), "12");
  EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const int CLIStatus =
      runCLIRequest(Path, AndroidNativeAArch64, Request, Output);
  EXPECT_EQ(CLIStatus, process_cli::Success);
  ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Parsed));
#endif
}

TEST_F(ProcessPublic, AndroidFortifyFormattingReportsMatchSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  Path =
      (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) / "format-O2-relr.so")
          .string();
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const char *Names[] = {"__snprintf_chk", "__vsnprintf_chk", "__sprintf_chk",
                         "__vsprintf_chk"};
  for (unsigned Mode = 0; Mode < std::size(Names); ++Mode) {
    SCOPED_TRACE(Names[Mode]);
    for (bool Fails : {false, true}) {
      SCOPED_TRACE(Fails);
      std::vector<uint8_t> Expected(64, 0xa5);
      llvm::json::Object Request{
          {"backend", "unicorn"},
          {"android",
           llvm::json::Object{
               {"entry_symbol", "format_fortify_dynamic"},
               {"initialize", false},
               {"arguments",
                llvm::json::Array{"0x20000000", Mode, 0, Fails ? 17 : 64}},
               {"memory", llvm::json::Array{llvm::json::Object{
                              {"address", "0x20000000"},
                              {"size", 4096},
                              {"bytes_hex", llvm::toHex(Expected, true)}}}},
               {"read_memory", llvm::json::Array{llvm::json::Object{
                                   {"address", "0x20000000"}, {"size", 64}}}},
               {"libraries",
                llvm::json::Object{
                    {"libformat-model.so", llvm::json::Array{Names[Mode]}}}}}}};
      auto Options = jsonText(std::move(Request));
      auto Text = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), AndroidNativeAArch64, Options.c_str()));
      ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
      auto Parsed = llvm::cantFail(llvm::json::parse(Text));
      const auto *Report = Parsed.getAsObject();
      ASSERT_NE(Report, nullptr);
      EXPECT_EQ(Report->getString(field::Stop),
                Fails ? "unsupported_service" : "returned");
      const auto *Android = Report->getObject(field::Android);
      ASSERT_NE(Android, nullptr);
      const auto *Calls = Android->getArray(field::NativeCalls);
      ASSERT_NE(Calls, nullptr);
      const llvm::json::Object *Lookup = nullptr, *Call = nullptr;
      for (const auto &Event : *Calls) {
        const auto *E = Event.getAsObject();
        if (E->getString(field::Name) == "dlsym")
          Lookup = E;
        if (E->getString(field::Name) == Names[Mode])
          Call = E;
      }
      ASSERT_NE(Lookup, nullptr);
      ASSERT_NE(Call, nullptr);
      EXPECT_EQ(Lookup->getString(field::Symbol), Names[Mode]);
      EXPECT_EQ(Call->getString(field::Library), "libformat-model.so");
      EXPECT_EQ(Call->getString(field::PC), Lookup->getString(field::Result));
      if (Fails) {
        EXPECT_EQ(*Report->get(field::ReturnValue), llvm::json::Value(nullptr));
        EXPECT_EQ(*Call->get(field::Result), llvm::json::Value(nullptr));
        ASSERT_TRUE(Report->getString(field::Diagnostic));
        EXPECT_NE(Report->getString(field::Diagnostic)
                      ->find("fortify buffer-size failure"),
                  llvm::StringRef::npos);
      } else {
        EXPECT_EQ(Report->getString(field::ReturnValue), "12");
        EXPECT_EQ(Call->getString(field::Result), "12");
        const char Bytes[] = "symbol=0x10000000a";
        std::copy_n(Bytes, sizeof(Bytes), Expected.begin());
      }
      const auto *Memory = Android->getArray(field::Memory);
      ASSERT_NE(Memory, nullptr);
      ASSERT_EQ(Memory->size(), 1u);
      EXPECT_EQ(Memory->front().getAsObject()->getString(field::Bytes),
                llvm::toHex(Expected, true));
      EXPECT_EQ(neverd_session_is_loaded(Session), 0);
      const int CLIStatus =
          runCLIRequest(Path, AndroidNativeAArch64, Options, Output);
      EXPECT_EQ(CLIStatus,
                Fails ? process_cli::Incomplete : process_cli::Success);
      ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Parsed));
    }
  }
#endif
}

TEST_F(ProcessPublic, AndroidFortifyLengthReportsMatchSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  Path =
      (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) / "length-O2-relr.so")
          .string();
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (unsigned Case = 0; Case < 3; ++Case) {
    SCOPED_TRACE(Case);
    const std::vector<uint8_t> Expected = {0x80, 'b', 0xff, 0, 0xa5};
    llvm::json::Object Request{
        {"backend", "unicorn"},
        {"android",
         llvm::json::Object{
             {"entry_symbol", "length_dynamic"},
             {"initialize", false},
             {"arguments",
              llvm::json::Array{Case == 2 ? "0xffffffffffffffff" : "0x20000000",
                                Case == 0   ? "0x100000000"
                                : Case == 1 ? "3"
                                            : "0",
                                0}},
             {"memory", llvm::json::Array{llvm::json::Object{
                            {"address", "0x20000000"},
                            {"size", 4096},
                            {"bytes_hex", llvm::toHex(Expected, true)}}}},
             {"read_memory", llvm::json::Array{llvm::json::Object{
                                 {"address", "0x20000000"}, {"size", 5}}}},
             {"libraries",
              llvm::json::Object{{"liblength-model.so",
                                  llvm::json::Array{"__strlen_chk"}}}}}}};
    const auto Options = jsonText(std::move(Request));
    const auto Text = takeString(neverd_emulate_process_json(
        Session, Path.c_str(), AndroidNativeAArch64, Options.c_str()));
    ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
    const auto Parsed = llvm::cantFail(llvm::json::parse(Text));
    const auto *Report = Parsed.getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getString(field::Stop),
              Case ? "runtime_failure" : "returned");
    const auto *Android = Report->getObject(field::Android);
    ASSERT_NE(Android, nullptr);
    const auto *Calls = Android->getArray(field::NativeCalls);
    ASSERT_NE(Calls, nullptr);
    const llvm::json::Object *Lookup = nullptr, *Call = nullptr;
    for (const auto &Event : *Calls) {
      const auto *E = Event.getAsObject();
      if (E->getString(field::Name) == "dlsym")
        Lookup = E;
      if (E->getString(field::Name) == "__strlen_chk")
        Call = E;
    }
    ASSERT_NE(Lookup, nullptr);
    ASSERT_NE(Call, nullptr);
    EXPECT_EQ(Lookup->getString(field::Symbol), "__strlen_chk");
    EXPECT_EQ(Call->getString(field::Library), "liblength-model.so");
    EXPECT_EQ(Call->getString(field::PC), Lookup->getString(field::Result));
    const auto *Memory = Android->getArray(field::Memory);
    ASSERT_NE(Memory, nullptr);
    if (Case) {
      EXPECT_EQ(*Report->get(field::ReturnValue), llvm::json::Value(nullptr));
      EXPECT_EQ(*Call->get(field::Result), llvm::json::Value(nullptr));
      ASSERT_TRUE(Report->getString(field::Diagnostic));
      EXPECT_NE(Report->getString(field::Diagnostic)
                    ->find(Case == 1 ? "FORTIFY" : "invalid guest pointer"),
                llvm::StringRef::npos);
      EXPECT_TRUE(Memory->empty());
    } else {
      EXPECT_EQ(Report->getString(field::ReturnValue), "3");
      EXPECT_EQ(Call->getString(field::Result), "3");
      ASSERT_EQ(Memory->size(), 1u);
      EXPECT_EQ(Memory->front().getAsObject()->getString(field::Bytes),
                llvm::toHex(Expected, true));
    }
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    const int CLIStatus =
        runCLIRequest(Path, AndroidNativeAArch64, Options, Output);
    EXPECT_EQ(CLIStatus, Case ? process_cli::Incomplete : process_cli::Success);
    ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Parsed));
  }
#endif
}

TEST_F(ProcessPublic, AndroidFortifyCopyReportsMatchSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  Path = (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) / "copy-O2-relr.so")
             .string();
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const char *Names[] = {"__strncpy_chk", "__strncpy_chk2"};
  for (unsigned Mode : {0u, 1u}) {
    for (unsigned Case = 0; Case < 3; ++Case) {
      SCOPED_TRACE(testing::Message() << Mode << ":" << Case);
      const bool Fails = Case == 1 || (Case == 2 && Mode == 1);
      std::vector<uint8_t> Expected(96, 0xa5);
      Expected[64] = 0x80;
      Expected[65] = 'b';
      Expected[66] = 0xff;
      Expected[67] = 0;
      llvm::json::Object Request{
          {"backend", "unicorn"},
          {"android",
           llvm::json::Object{
               {"entry_symbol", "copy_checked_dynamic"},
               {"initialize", false},
               {"arguments", llvm::json::Array{"0x20000000", "0x20000040", 7,
                                               Case == 1 ? 6 : 7,
                                               Case == 2 ? 3 : 4, Mode, 0}},
               {"memory", llvm::json::Array{llvm::json::Object{
                              {"address", "0x20000000"},
                              {"size", 4096},
                              {"bytes_hex", llvm::toHex(Expected, true)}}}},
               {"read_memory", llvm::json::Array{llvm::json::Object{
                                   {"address", "0x20000000"}, {"size", 96}}}},
               {"libraries",
                llvm::json::Object{
                    {"libcopy-checked.so", llvm::json::Array{Names[Mode]}}}}}}};
      const auto Options = jsonText(std::move(Request));
      const auto Text = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), AndroidNativeAArch64, Options.c_str()));
      ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
      const auto Parsed = llvm::cantFail(llvm::json::parse(Text));
      const auto *Report = Parsed.getAsObject();
      ASSERT_NE(Report, nullptr);
      EXPECT_EQ(Report->getString(field::Stop),
                Fails ? "runtime_failure" : "returned");
      const auto *Android = Report->getObject(field::Android);
      ASSERT_NE(Android, nullptr);
      const auto *Calls = Android->getArray(field::NativeCalls);
      ASSERT_NE(Calls, nullptr);
      const llvm::json::Object *Lookup = nullptr, *Call = nullptr;
      for (const auto &Event : *Calls) {
        const auto *E = Event.getAsObject();
        if (E->getString(field::Name) == "dlsym")
          Lookup = E;
        if (E->getString(field::Name) == Names[Mode])
          Call = E;
      }
      ASSERT_NE(Lookup, nullptr);
      ASSERT_NE(Call, nullptr);
      EXPECT_EQ(Lookup->getString(field::Symbol), Names[Mode]);
      EXPECT_EQ(Call->getString(field::Library), "libcopy-checked.so");
      EXPECT_EQ(Call->getString(field::PC), Lookup->getString(field::Result));
      const auto *Memory = Android->getArray(field::Memory);
      ASSERT_NE(Memory, nullptr);
      if (Fails) {
        EXPECT_EQ(*Report->get(field::ReturnValue), llvm::json::Value(nullptr));
        EXPECT_EQ(*Call->get(field::Result), llvm::json::Value(nullptr));
        ASSERT_TRUE(Report->getString(field::Diagnostic));
        EXPECT_NE(
            Report->getString(field::Diagnostic)
                ->find(Case == 1 ? "FORTIFY destination" : "FORTIFY read"),
            llvm::StringRef::npos);
        EXPECT_TRUE(Memory->empty());
      } else {
        EXPECT_EQ(Report->getString(field::ReturnValue), "20000000");
        EXPECT_EQ(Call->getString(field::Result), "20000000");
        std::copy_n(Expected.begin() + 64, 4, Expected.begin());
        std::fill(Expected.begin() + 4, Expected.begin() + 7, 0);
        ASSERT_EQ(Memory->size(), 1u);
        EXPECT_EQ(Memory->front().getAsObject()->getString(field::Bytes),
                  llvm::toHex(Expected, true));
      }
      const int CLIStatus =
          runCLIRequest(Path, AndroidNativeAArch64, Options, Output);
      EXPECT_EQ(CLIStatus,
                Fails ? process_cli::Incomplete : process_cli::Success);
      ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Parsed));
    }
  }
#endif
}

TEST_F(ProcessPublic, AndroidMemoryFilesShareStateThroughCAPIAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  Path =
      (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) / "files-O2-relr.so")
          .string();
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (const char *Entry :
       {"files_sequence", "files_faults", "files_bionic", "files_status",
        "files_status_bionic", "files_status_at", "files_status_at_bionic",
        "files_access", "files_access_faults", "files_access_bionic",
        "files_directory_errors", "files_directory_bionic",
        "files_trailing_paths", "files_trailing_paths_bionic",
        "files_filesystem_status", "files_filesystem_status_bionic",
        "files_links", "files_links_bionic"}) {
    SCOPED_TRACE(Entry);
    llvm::json::Object Request{
        {"backend", "unicorn"},
        {"instruction_quantum", 31},
        {"android", llvm::json::Object{{"entry_symbol", Entry},
                                       {"initialize", false},
                                       {"thread_limit", 2}}},
        {"linux_files",
         llvm::json::Object{{"files", llvm::json::Array{llvm::json::Object{
                                          {"path", "/fixture/data"},
                                          {"bytes_hex", "00ff410a805a"}}}}}}};
    if (llvm::StringRef(Entry).starts_with("files_links"))
      (*Request.getObject("linux_files"))["symbolic_links"] =
          llvm::json::Array{llvm::json::Object{{"path", "/links/raw"},
                                               {"target_hex", "2f78ff79"}}};
    if (llvm::StringRef(Entry).starts_with("files_status"))
      (*Request.getObject("linux_files")
            ->getArray("files")
            ->front()
            .getAsObject())["metadata"] =
          llvm::cantFail(llvm::json::parse(emulation::FileTestMetadataJSON));
    if (llvm::StringRef(Entry) == "files_access" ||
        llvm::StringRef(Entry) == "files_directory_errors" ||
        llvm::StringRef(Entry) == "files_trailing_paths")
      (*Request.getObject("linux_files"))["descriptor_limit"] = 4;
    auto Options = jsonText(std::move(Request));
    auto Text = takeString(neverd_emulate_process_json(
        Session, Path.c_str(), AndroidNativeAArch64, Options.c_str()));
    ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
    auto Report = llvm::cantFail(llvm::json::parse(Text));
    EXPECT_EQ(Report.getAsObject()->getString(field::Stop), "returned");
    EXPECT_EQ(Report.getAsObject()->getString(field::ReturnValue), "0");
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    const int CLIStatus =
        runCLIRequest(Path, AndroidNativeAArch64, Options, Output);
    EXPECT_EQ(CLIStatus, process_cli::Success);
    ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Report));
  }
#endif
}

TEST_F(ProcessPublic, AndroidLocalMemoryInputMatchesSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  Path = (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) / "android.so")
             .string();
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Input = (Root / "input.bin").string();
  std::vector<uint8_t> Bytes(65539);
  uint64_t Hash = 14695981039346656037ull;
  for (size_t I = 0; I < Bytes.size(); ++I)
    Bytes[I] = static_cast<uint8_t>(I * 37 + (I >> 8));
  for (size_t I = 0; I < Bytes.size();) {
    uint64_t Word = 0;
    size_t Width = Bytes.size() - I >= 8 ? 8 : 1;
    for (size_t J = 0; J < Width; ++J)
      Word |= uint64_t(Bytes[I++]) << (8 * J);
    Hash = (Hash ^ Word) * 1099511628211ull;
  }
  {
    std::error_code E;
    llvm::raw_fd_ostream OS(Input, E);
    ASSERT_FALSE(E);
    OS.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
  }
  llvm::json::Object Options{
      {field::Backend, "unicorn"},
      {field::InstructionLimit, 1000000},
      {field::Timeout, 20000000},
      {field::Android,
       llvm::json::Object{
           {field::EntrySymbol, "inspect_memory_input"},
           {field::Arguments, llvm::json::Array{0x20000000, Bytes.size()}},
           {field::Memory,
            llvm::json::Array{llvm::json::Object{{field::Address, 0x20000000},
                                                 {field::Size, 18 * 4096},
                                                 {field::Path, Input}}}},
           {field::ReadMemory,
            llvm::json::Array{llvm::json::Object{{field::Address, 0x20000000},
                                                 {field::Size, 18 * 4096}}}}}}};
  std::string Request;
  llvm::raw_string_ostream(Request) << llvm::json::Value(std::move(Options));
  ASSERT_LT(Request.size(), field::JSONLimit);
  auto Text = takeString(neverd_emulate_process_json(
      Session, Path.c_str(), AndroidNativeAArch64, Request.c_str()));
  ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
  auto Parsed = llvm::cantFail(llvm::json::parse(Text));
  EXPECT_EQ(Parsed.getAsObject()->getString(field::Stop), "returned");
  EXPECT_EQ(Parsed.getAsObject()->getString(field::ReturnValue),
            llvm::utohexstr(Hash, true));
  const auto *Android = Parsed.getAsObject()->getObject(field::Android);
  ASSERT_NE(Android, nullptr);
  auto Mutated = Bytes;
  Mutated[0] ^= 255;
  Mutated.push_back(165);
  Mutated.resize(18 * 4096, 0);
  ASSERT_EQ(Android->getArray(field::Memory)->size(), 1u);
  EXPECT_EQ(Android->getArray(field::Memory)
                ->front()
                .getAsObject()
                ->getString(field::Bytes),
            llvm::toHex(Mutated, true));
  const auto Output = (Root / OutputFile).string();
  const int CLIStatus =
      runCLIRequest(Path, AndroidNativeAArch64, Request, Output);
  EXPECT_EQ(CLIStatus, process_cli::Success);
  auto Report = llvm::MemoryBuffer::getFile(Output);
  ASSERT_TRUE(bool(Report));
  EXPECT_EQ(llvm::cantFail(llvm::json::parse((*Report)->getBuffer())), Parsed);
  auto Original = llvm::MemoryBuffer::getFile(Input);
  ASSERT_TRUE(bool(Original));
  EXPECT_EQ((*Original)->getBuffer(),
            llvm::StringRef(reinterpret_cast<const char *>(Bytes.data()),
                            Bytes.size()));
#endif
}

TEST_F(ProcessPublic, AndroidInitializersReturnThroughSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  Path = (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) / "once-O2-relr.so")
             .string();
  const std::string Request =
      R"({"backend":"unicorn","android":{"entry_symbol":"once_values","arguments":["0x20000000"],"memory":[{"address":"0x20000000","size":4096}],"read_memory":[{"address":"0x20000000","size":44}]}})";
  auto Text = takeString(neverd_emulate_process_json(
      Session, Path.c_str(), AndroidNativeAArch64, Request.c_str()));
  ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
  auto Parsed = llvm::cantFail(llvm::json::parse(Text));
  ASSERT_EQ(Parsed.getAsObject()->getString(field::Stop), "returned");
  EXPECT_EQ(Parsed.getAsObject()->getString(field::ReturnValue), "49");
  const auto *Android = Parsed.getAsObject()->getObject(field::Android);
  ASSERT_NE(Android, nullptr);
  const auto *Calls = Android->getArray(field::NativeCalls);
  ASSERT_NE(Calls, nullptr);
  ASSERT_EQ(Calls->size(), 6u);
  for (size_t I = 0; I < Calls->size(); ++I) {
    EXPECT_EQ((*Calls)[I].getAsObject()->getString(field::Name),
              I == 4 ? "getuid" : "pthread_once");
    EXPECT_EQ((*Calls)[I].getAsObject()->getString(field::Result),
              I == 4 ? "3e8" : "0");
  }
  const auto *Memory = Android->getArray(field::Memory);
  ASSERT_NE(Memory, nullptr);
  ASSERT_EQ(Memory->size(), 1u);
  EXPECT_EQ((*Memory)[0].getAsObject()->getString(field::Bytes),
            "02000000020000000100000001000000010000000100000002000000"
            "e8030000010000000200000001000000");
  EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const int CLIStatus =
      runCLIRequest(Path, AndroidNativeAArch64, Request, Output);
  EXPECT_EQ(CLIStatus, process_cli::Success);
  ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Parsed));
#endif
}

TEST_F(ProcessPublic, AndroidFortifiedSearchReportsMatchSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  Path =
      (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) / "search-O2-relr.so")
          .string();
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (bool Fails : {false, true}) {
    const std::string Request =
        Fails
            ? R"({"backend":"unicorn","android":{"entry_symbol":"search_supplied","initialize":false,"arguments":[1,58,0,2]}})"
            : R"({"backend":"unicorn","android":{"entry_symbol":"search_dynamic","initialize":false,"arguments":["0x20000000",0,0],"memory":[{"address":"0x20000000","size":4096}],"read_memory":[{"address":"0x20000000","size":24}],"libraries":{"libsearch.so":["__strchr_chk"]}}})";
    auto Text = takeString(neverd_emulate_process_json(
        Session, Path.c_str(), AndroidNativeAArch64, Request.c_str()));
    ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
    auto Parsed = llvm::cantFail(llvm::json::parse(Text));
    const auto *Report = Parsed.getAsObject();
    ASSERT_NE(Report, nullptr);
    ASSERT_EQ(Report->getString(field::Stop),
              Fails ? "runtime_failure" : "returned");
    const auto *Android = Report->getObject(field::Android);
    ASSERT_NE(Android, nullptr);
    const auto *Calls = Android->getArray(field::NativeCalls);
    ASSERT_NE(Calls, nullptr);
    ASSERT_FALSE(Calls->empty());
    if (Fails) {
      EXPECT_NE(Report->getString(field::Diagnostic)->find("FORTIFY"),
                llvm::StringRef::npos);
      const auto *Last = Calls->back().getAsObject();
      EXPECT_EQ(Last->getString(field::Name), "__strchr_chk");
      EXPECT_EQ(*Last->get(field::Result), llvm::json::Value(nullptr));
    } else {
      EXPECT_EQ(Report->getString(field::ReturnValue), "0");
      const llvm::json::Object *Lookup = nullptr;
      unsigned Searches = 0;
      for (const auto &Event : *Calls) {
        const auto *Call = Event.getAsObject();
        if (Call->getString(field::Name) == "dlsym") {
          Lookup = Call;
          EXPECT_EQ(Call->getString(field::Symbol), "__strchr_chk");
        }
        if (Call->getString(field::Name) == "__strchr_chk") {
          ASSERT_NE(Lookup, nullptr);
          EXPECT_EQ(Call->getString(field::Library), "libsearch.so");
          EXPECT_EQ(Call->getString(field::PC),
                    Lookup->getString(field::Result));
          ++Searches;
        }
      }
      EXPECT_EQ(Searches, 2u);
      const auto *Memory = Android->getArray(field::Memory);
      ASSERT_NE(Memory, nullptr);
      ASSERT_EQ(Memory->size(), 1u);
      EXPECT_EQ((*Memory)[0].getAsObject()->getString(field::Bytes),
                "04000000000000000f00000000000000dd02000000000000");
    }
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    const int CLIStatus =
        runCLIRequest(Path, AndroidNativeAArch64, Request, Output);
    EXPECT_EQ(CLIStatus,
              Fails ? process_cli::Incomplete : process_cli::Success);
    ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Parsed));
  }
#endif
}

TEST_F(ProcessPublic,
       AndroidPropertyCallbacksAndProviderLifetimeMatchSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  Path = (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
          "properties-O2-relr.so")
             .string();
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (bool Close : {false, true}) {
    const std::string Request =
        std::string(
            R"({"backend":"unicorn","android":{"entry_symbol":"property_dynamic","initialize":false,"arguments":["0x20000000",")") +
        (Close ? "1" : "0") +
        R"("],"memory":[{"address":"0x20000000","size":4096}],"read_memory":[{"address":"0x20000000","size":80}],"properties":{"test.alpha":"one"},"libraries":{"libproperties.so":["__system_property_find","__system_property_read","__system_property_read_callback","__system_property_foreach"]}}})";
    auto Text = takeString(neverd_emulate_process_json(
        Session, Path.c_str(), AndroidNativeAArch64, Request.c_str()));
    ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
    auto Parsed = llvm::cantFail(llvm::json::parse(Text));
    ASSERT_EQ(Parsed.getAsObject()->getString(field::Stop),
              Close ? "unsupported_service" : "returned");
    const auto *Android = Parsed.getAsObject()->getObject(field::Android);
    ASSERT_NE(Android, nullptr);
    const auto *Calls = Android->getArray(field::NativeCalls);
    ASSERT_NE(Calls, nullptr);
    if (Close) {
      ASSERT_FALSE(Calls->empty());
      EXPECT_EQ(Calls->back().getAsObject()->getString(field::Name),
                "__system_property_read");
      EXPECT_EQ(Calls->back().getAsObject()->getString(field::Library),
                "libproperties.so");
      EXPECT_TRUE(Calls->back().getAsObject()->get(field::Result)->getAsNull());
    } else {
      EXPECT_EQ(Parsed.getAsObject()->getString(field::ReturnValue), "0");
      const auto *Memory = Android->getArray(field::Memory);
      ASSERT_NE(Memory, nullptr);
      ASSERT_EQ(Memory->size(), 1u);
      const uint64_t Words[] = {
          1, 0xece42d7a4bc366ee, 0x1d4996, 0x3000000, 0x3000000,
          3, 0x1d4996,           1,        1,         0x3000000};
      std::vector<uint8_t> Expected(sizeof(Words));
      for (size_t I = 0; I < std::size(Words); ++I)
        llvm::support::endian::write64le(Expected.data() + I * 8, Words[I]);
      EXPECT_EQ(Memory->front().getAsObject()->getString(field::Bytes),
                llvm::toHex(Expected, true));
      unsigned Named = 0;
      for (const auto &Event : *Calls) {
        const auto &Call = *Event.getAsObject();
        if (Call.getString(field::Name)->starts_with("__system_property_") &&
            Call.getString(field::Library) == "libproperties.so") {
          ++Named;
          EXPECT_FALSE(Call.get(field::Result)->getAsNull());
        }
      }
      EXPECT_EQ(Named, 4u);
    }
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    EXPECT_EQ(runCLIRequest(Path, AndroidNativeAArch64, Request, Output),
              Close ? process_cli::Incomplete : process_cli::Success);
    ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Parsed));
  }
#endif
}

TEST_F(ProcessPublic, AndroidStdioStateAndProviderLifetimeMatchSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  Path =
      (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) / "stdio-O2-relr.so")
          .string();
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (bool Close : {false, true}) {
    auto Metadata =
        llvm::cantFail(llvm::json::parse(emulation::FileTestMetadataJSON));
    (*Metadata.getAsObject())["block_size"] = 8;
    llvm::json::Object Request{
        {"backend", "unicorn"},
        {"android",
         llvm::json::Object{
             {"entry_symbol", "stdio_dynamic"},
             {"initialize", false},
             {"arguments", llvm::json::Array{0x20000000, Close ? 1 : 0}},
             {"memory", llvm::json::Array{llvm::json::Object{
                            {"address", "0x20000000"}, {"size", 4096}}}},
             {"read_memory", llvm::json::Array{llvm::json::Object{
                                 {"address", "0x20000000"}, {"size", 24}}}},
             {"libraries",
              llvm::json::Object{
                  {"libstreams.so",
                   llvm::json::Array{"fopen", "fgetc", "fclose"}}}}}},
        {"linux_files",
         llvm::json::Object{
             {"files", llvm::json::Array{llvm::json::Object{
                           {"path", "/fixture/input"},
                           {"bytes_hex", "616c7068610a626574610a6c617374"},
                           {"metadata", std::move(Metadata)}}}}}}};
    const auto Options = jsonText(std::move(Request));
    const auto Text = takeString(neverd_emulate_process_json(
        Session, Path.c_str(), AndroidNativeAArch64, Options.c_str()));
    ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
    auto Report = llvm::cantFail(llvm::json::parse(Text));
    ASSERT_EQ(Report.getAsObject()->getString(field::Stop),
              Close ? "unsupported_service" : "returned");
    const auto &Android = *Report.getAsObject()->getObject(field::Android);
    const auto &Calls = *Android.getArray(field::NativeCalls);
    ASSERT_FALSE(Calls.empty());
    if (Close) {
      EXPECT_EQ(Calls.back().getAsObject()->getString(field::Name), "fgetc");
      EXPECT_EQ(Calls.back().getAsObject()->getString(field::Library),
                "libstreams.so");
      EXPECT_TRUE(Calls.back().getAsObject()->get(field::Result)->getAsNull());
    } else {
      EXPECT_EQ(Report.getAsObject()->getString(field::ReturnValue), "0");
      ASSERT_EQ(Android.getArray(field::Memory)->size(), 1u);
      EXPECT_EQ(Android.getArray(field::Memory)
                    ->front()
                    .getAsObject()
                    ->getString(field::Bytes),
                "610000000000000001000000000000000000000000000000");
    }
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    EXPECT_EQ(runCLIRequest(Path, AndroidNativeAArch64, Options, Output),
              Close ? process_cli::Incomplete : process_cli::Success);
    ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Report));
  }
#endif
}

TEST_F(ProcessPublic, AndroidMutableFilesAndLiveProcMatchSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android fixtures unavailable";
#else
  Path =
      (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) / "stdio-O2-relr.so")
          .string();
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (const char *Entry : {"filesystem_shared", "filesystem_proc"}) {
    llvm::json::Object Request{
        {"backend", "unicorn"},
        {"android",
         llvm::json::Object{
             {"entry_symbol", Entry},
             {"initialize", false},
             {"arguments", llvm::json::Array{0x20000000}},
             {"memory", llvm::json::Array{llvm::json::Object{
                            {"address", 0x20000000}, {"size", 4096}}}},
             {"read_memory", llvm::json::Array{llvm::json::Object{
                                 {"address", 0x20000000}, {"size", 88}}}}}},
        {"linux_files",
         llvm::json::Object{
             {"files", llvm::json::Array{llvm::json::Object{
                           {"path", "/fixture/input"},
                           {"bytes_hex", "616c7068610a626574610a6c617374"},
                           {"write_limit", 64}}}},
             {"directories", llvm::json::Array{"/fixture/sub"}},
             {"dynamic_proc_fds", true}}}};
    const auto Options = jsonText(std::move(Request));
    const auto Text = takeString(neverd_emulate_process_json(
        Session, Path.c_str(), AndroidNativeAArch64, Options.c_str()));
    ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
    auto Report = llvm::cantFail(llvm::json::parse(Text));
    ASSERT_EQ(Report.getAsObject()->getString(field::Stop), "returned");
    EXPECT_EQ(Report.getAsObject()->getString(field::ReturnValue), "0");
    EXPECT_EQ(runCLIRequest(Path, AndroidNativeAArch64, Options, Output),
              process_cli::Success);
    ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Report));
  }
#endif
}

TEST_F(ProcessPublic, AndroidFileLifecycleMatchesSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const auto Input = llvm::cantFail(llvm::json::parse(R"({
    "backend":"unicorn",
    "linux_time":{"clocks":[{"id":0,"seconds":100,"nanoseconds":123}]},
    "android":{"initialize":false,"thread_limit":1,
      "libraries":{"libfiles.so":["open","openat","rename","renameat","unlink","unlinkat"]}},
    "linux_files":{"dynamic_proc_fds":true,"directories":["/fixture"],
      "files":[{"path":"/fixture/data","bytes_hex":"00ff410a805a",
        "write_limit":8192,"track_write_metadata":true,
        "metadata":{"device":7,"inode":42,"mode":33188,"link_count":1,
          "uid":1000,"gid":1000,"size":6,"block_size":4096,"blocks":8,
          "access_time":{"seconds":3,"nanoseconds":4},
          "modification_time":{"seconds":5,"nanoseconds":6},
          "change_time":{"seconds":7,"nanoseconds":8}}}],
      "directory_creation":{"umask":18,"limit":64,"parents":[{
        "path":"/fixture","metadata":{"device":7,"inode":91,
          "mode":16877,"link_count":2,"uid":1000,"gid":1000,"size":60,
          "block_size":4096,"blocks":0,
          "access_time":{"seconds":3,"nanoseconds":4},
          "modification_time":{"seconds":5,"nanoseconds":6},
          "change_time":{"seconds":7,"nanoseconds":8}}}]},
      "file_renames":{"parents":["/fixture"],"limit":2},
      "file_unlinks":{"parents":["/fixture"],"limit":2},
      "file_creations":[{"path":"/fixture/data","write_limit":8192,"count":1}]}})"));
  for (const char *Fixture : {"files-O0-none", "files-O2-relr"}) {
    Path = (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
            (std::string(Fixture) + ".so"))
               .string();
    for (const char *Entry :
         {"files_rename_bionic", "files_unlink_closed_bionic",
          "files_creation_bionic", "files_creation_counted_bionic"}) {
      for (unsigned Route : {0u, 4u, 5u}) {
        SCOPED_TRACE(testing::Message()
                     << Fixture << ':' << Entry << ':' << Route);
        auto Request = Input;
        auto &Native = *Request.getAsObject()->getObject("android");
        Native["entry_symbol"] = Entry;
        Native["arguments"] = llvm::json::Array{Route == 5 ? 0 : Route};
        auto &Files = *Request.getAsObject()->getObject("linux_files");
        auto &Reservation =
            *Files.getArray("file_creations")->front().getAsObject();
        if (llvm::StringRef(Entry) == "files_creation_counted_bionic") {
          Reservation["count"] = 2;
          Reservation["write_limit"] = 3;
        }
        if (Route == 5) {
          if (llvm::StringRef(Entry) == "files_rename_bionic")
            Files.erase("file_renames");
          else if (llvm::StringRef(Entry) == "files_unlink_closed_bionic")
            Files.erase("file_unlinks");
          else
            Files.erase("file_creations");
        }
        const auto Options = jsonText(*Request.getAsObject());
        auto Text = takeString(neverd_emulate_process_json(
            Session, Path.c_str(), AndroidNativeAArch64, Options.c_str()));
        ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
        auto Report = llvm::cantFail(llvm::json::parse(Text));
        EXPECT_EQ(Report.getAsObject()->getString(field::Stop),
                  Route == 5 ? "unsupported_service" : "returned")
            << Text;
        if (Route != 5)
          EXPECT_EQ(Report.getAsObject()->getString(field::ReturnValue), "0");
        EXPECT_EQ(runCLIRequest(Path, AndroidNativeAArch64, Options, Output),
                  Route == 5 ? process_cli::Incomplete : process_cli::Success);
        ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Report));
      }
    }
  }
#endif
}

TEST_F(ProcessPublic, AndroidLibcCompatibilityMatchesSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  std::vector<uint8_t> Bytes(8192, 0xa5);
  std::copy_n("/fixture/data", 14, Bytes.begin() + 512);
  std::copy_n("abc", 4, Bytes.begin() + 4096);
  struct Case {
    const char *Fixture, *Entry, *Arguments, *Stop, *Value;
  };
  const Case Cases[] = {
      {"copy", "copy_sized", "[536870928,536875008,7,0,536872960]", "returned",
       "3"},
      {"copy", "copy_sized", "[536870928,536875008,1,1,536872960]", "returned",
       "3"},
      {"copy", "copy_sized", "[0,536875008,0,0,536872960]", "returned", "3"},
      {"copy", "copy_sized", "[536870928,0,0,0,536872960]", "runtime_failure",
       nullptr},
      {"files", "files_read_chk_sequence", "[0]", "returned", "0"},
      {"files", "files_read_chk_sequence", "[1]", "returned", "0"},
      {"files", "files_read_chk_bounds", "[536870912,1,0]", "runtime_failure",
       nullptr},
      {"files", "files_read_chk_bounds",
       "[1,\"9223372036854775808\",\"18446744073709551615\"]",
       "runtime_failure", nullptr},
      {"files", "files_path_status_call", "[0,536871424,536871936,536870912,0]",
       "returned", "0"},
      {"files", "files_path_status_call", "[1,536871424,536871936,536870912,1]",
       "returned", "0"},
      {"files", "files_path_status_call", "[2,536871424,536871936,536870912,0]",
       "returned", "0"},
      {"files", "files_path_status_call", "[3,536871424,536871936,536870912,1]",
       "returned", "0"}};
  for (const char *Variant : {"O0-none", "O2-relr"}) {
    for (const auto &C : Cases) {
      SCOPED_TRACE(testing::Message()
                   << Variant << ':' << C.Entry << ':' << C.Arguments);
      Path = (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
              (std::string(C.Fixture) + '-' + Variant + ".so"))
                 .string();
      const auto Options = jsonText(llvm::json::Object{
          {"backend", "unicorn"},
          {"android",
           llvm::json::Object{
               {"entry_symbol", C.Entry},
               {"initialize", false},
               {"thread_limit", 1},
               {"arguments", llvm::cantFail(llvm::json::parse(C.Arguments))},
               {"observe_memory_on_failure", true},
               {"memory", llvm::json::Array{llvm::json::Object{
                              {"address", "0x20000000"},
                              {"size", 8192},
                              {"bytes_hex", llvm::toHex(Bytes)}}}},
               {"read_memory", llvm::json::Array{llvm::json::Object{
                                   {"address", "0x20000000"}, {"size", 8192}}}},
               {"libraries",
                llvm::json::Object{
                    {"libcopy.so", llvm::json::Array{"strlcpy"}},
                    {"libfiles.so",
                     llvm::json::Array{"__read_chk", "stat", "stat64", "lstat",
                                       "lstat64"}}}}}},
          {"linux_files",
           llvm::json::Object{
               {"files",
                llvm::json::Array{llvm::json::Object{
                    {"path", "/fixture/data"},
                    {"bytes_hex", "00ff410a805a"},
                    {"metadata", llvm::cantFail(llvm::json::parse(
                                     emulation::FileTestMetadataJSON))}}}}}}});
      auto Text = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), AndroidNativeAArch64, Options.c_str()));
      ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
      auto Report = llvm::cantFail(llvm::json::parse(Text));
      EXPECT_EQ(Report.getAsObject()->getString(field::Stop), C.Stop) << Text;
      if (C.Value)
        EXPECT_EQ(Report.getAsObject()->getString(field::ReturnValue), C.Value);
      EXPECT_EQ(runCLIRequest(Path, AndroidNativeAArch64, Options, Output),
                C.Value ? process_cli::Success : process_cli::Incomplete);
      ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Report));
    }
  }
#endif
}

TEST_F(ProcessPublic, AndroidConnectionFailuresMatchSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (const char *Variant : {"network-O0-none", "network-O2-relr"}) {
    Path = (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
            (std::string(Variant) + ".so"))
               .string();
    for (unsigned Mode : {0u, 1u, 2u}) {
      SCOPED_TRACE(Variant);
      SCOPED_TRACE(Mode);
      auto Request = llvm::cantFail(llvm::json::parse(
          R"({"backend":"unicorn","linux_files":{"files":[]},"linux_network":{"connections":[{"peer_hex":"020023297f0000010000000000000000"}],"tcp_connect_failures":{"errno":101,"count":4}},"android":{"entry_symbol":"network_connect_failures","initialize":false,"read_memory":[{"address":"0x20000000","size":4096}],"libraries":{"libc.so":["connect"]}}})"));
      auto &Android = *Request.getAsObject()->getObject(field::Android);
      Android[field::Arguments] = llvm::json::Array{"0x20000000", Mode};
      std::vector<uint8_t> Expected(4096, 0xa5);
      Android[field::Memory] = llvm::json::Array{
          llvm::json::Object{{field::Address, "0x20000000"},
                             {field::Size, 4096},
                             {"bytes_hex", llvm::toHex(Expected, true)}}};
      const auto Options = jsonText(std::move(*Request.getAsObject()));
      auto Text = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), AndroidNativeAArch64, Options.c_str()));
      ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
      auto Parsed = llvm::cantFail(llvm::json::parse(Text));
      const auto *Report = Parsed.getAsObject();
      ASSERT_NE(Report, nullptr);
      EXPECT_EQ(Report->getString(field::Stop),
                Mode ? "unsupported_service" : "returned");
      if (Mode) {
        EXPECT_EQ(*Report->get(field::ReturnValue), llvm::json::Value(nullptr));
        ASSERT_TRUE(Report->getString(field::Diagnostic));
        EXPECT_NE(Report->getString(field::Diagnostic)
                      ->find(Mode == 1 ? "failure contract exhausted"
                                       : "closed before reuse"),
                  llvm::StringRef::npos);
      } else {
        EXPECT_EQ(Report->getString(field::ReturnValue), "0");
      }
      llvm::support::endian::write64le(Expected.data(), 0);
      for (unsigned I = 0; I != (Mode == 2 ? 1u : 4u); ++I) {
        llvm::support::endian::write64le(Expected.data() + 8 * (1 + 2 * I),
                                         I == 3 ? uint64_t(-101) : UINT64_MAX);
        llvm::support::endian::write64le(Expected.data() + 8 * (2 + 2 * I),
                                         I == 3 ? 73 : 101);
      }
      const auto *Observed = Report->getObject(field::Android);
      ASSERT_NE(Observed, nullptr);
      const auto *Memory = Observed->getArray(field::Memory);
      ASSERT_NE(Memory, nullptr);
      ASSERT_EQ(Memory->size(), 1u);
      EXPECT_EQ(Memory->front().getAsObject()->getString(field::Bytes),
                llvm::toHex(Expected, true));
      EXPECT_EQ(runCLIRequest(Path, AndroidNativeAArch64, Options, Output),
                Mode ? process_cli::Incomplete : process_cli::Success);
      ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Parsed));
    }
  }
#endif
}

TEST_F(ProcessPublic, AndroidResidencyObservationsMatchSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (const char *Packing : {"none", "android", "relr"}) {
    Path = (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
            (std::string(Packing) + ".so"))
               .string();
    for (unsigned Route : {0u, 1u, 2u, 3u}) {
      for (unsigned Mode : {0u, 1u, 4u}) {
        SCOPED_TRACE(Packing);
        SCOPED_TRACE(Route);
        SCOPED_TRACE(Mode);
        auto Request = llvm::cantFail(llvm::json::parse(
            R"({"backend":"unicorn","linux_kernel":{"residency_observations":[{"address":"4294967296","pages":[1,0,1]},{"address":"4294967296","pages":[0,1,0]}]},"android":{"entry_symbol":"residency_observe","initialize":false,"memory":[{"address":"0x20000000","size":4096}],"read_memory":[{"address":"0x20000000","size":64}],"libraries":{"libresidency.so":["mincore"]}}})"));
        (*Request.getAsObject()->getObject(field::Android))[field::Arguments] =
            llvm::json::Array{"0x20000000", Route, 8193, "0x20000030", Mode};
        const auto Options = jsonText(std::move(*Request.getAsObject()));
        auto Text = takeString(neverd_emulate_process_json(
            Session, Path.c_str(), AndroidNativeAArch64, Options.c_str()));
        ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
        auto Parsed = llvm::cantFail(llvm::json::parse(Text));
        auto *Report = Parsed.getAsObject();
        ASSERT_NE(Report, nullptr);
        ASSERT_EQ(Report->getString(field::Stop), "returned");
        const uint64_t Result =
            Mode == 1 ? (Route == 2 ? uint64_t(-12) : UINT64_MAX) : 0;
        EXPECT_EQ(Report->getString(field::ReturnValue),
                  llvm::utohexstr(Result, true));
        std::vector<uint8_t> Expected(64);
        llvm::support::endian::write64le(Expected.data(), 0x100000000);
        llvm::support::endian::write64le(Expected.data() + 8, Result);
        llvm::support::endian::write64le(Expected.data() + 16,
                                         Mode == 1 && Route != 2 ? 12 : 733);
        Expected[48] = 1;
        if (Mode != 1)
          Expected[50] = 1;
        if (Mode == 4) {
          llvm::support::endian::write64le(Expected.data() + 32, 733);
          Expected[57] = 1;
        }
        const auto *Android = Report->getObject(field::Android);
        ASSERT_NE(Android, nullptr);
        const auto *Memory = Android->getArray(field::Memory);
        ASSERT_NE(Memory, nullptr);
        ASSERT_EQ(Memory->size(), 1u);
        EXPECT_EQ(Memory->front().getAsObject()->getString(field::Bytes),
                  llvm::toHex(Expected, true));
        EXPECT_EQ(runCLIRequest(Path, AndroidNativeAArch64, Options, Output),
                  process_cli::Success);
        ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Parsed));
      }
    }
  }
#endif
}

TEST_F(ProcessPublic, AndroidAnonymousDiscardMatchesSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (const char *Packing : {"none", "relr"}) {
    Path = (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
            (std::string(Packing) + ".so"))
               .string();
    for (unsigned Route = 0; Route != 5; ++Route) {
      for (unsigned Mode = 0; Mode != (Route == 4 ? 1u : 3u); ++Mode) {
        SCOPED_TRACE(Packing);
        SCOPED_TRACE(Route);
        SCOPED_TRACE(Mode);
        auto Request = llvm::cantFail(llvm::json::parse(
            R"({"backend":"unicorn","instruction_limit":500000,"output_limit":4194304,"android":{"entry_symbol":"discard_pages","initialize":false,"observe_memory_on_failure":true,"trace_limit":500000,"libraries":{"libadvice.so":["madvise"]},"read_memory":[{"address":"0x20000000","size":16384}]}})"));
        auto &Android = *Request.getAsObject()->getObject(field::Android);
        Android[field::Arguments] =
            llvm::json::Array{"0x20000000", Route, Mode};
        std::vector<uint8_t> Expected(16384, 0xa5);
        Android[field::Memory] = llvm::json::Array{
            llvm::json::Object{{field::Address, "0x20000000"},
                               {field::Size, 16384},
                               {"bytes_hex", llvm::toHex(Expected, true)}}};
        const auto Options = jsonText(std::move(*Request.getAsObject()));
        auto Text = takeString(neverd_emulate_process_json(
            Session, Path.c_str(), AndroidNativeAArch64, Options.c_str()));
        ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
        auto Parsed = llvm::cantFail(llvm::json::parse(Text));
        const auto *Report = Parsed.getAsObject();
        ASSERT_NE(Report, nullptr);
        const bool Closed = Route == 4;
        ASSERT_EQ(Report->getString(field::Stop),
                  Closed ? "unsupported_service" : "returned");
        llvm::support::endian::write64le(Expected.data(), 73);
        llvm::support::endian::write64le(Expected.data() + 8, 0x100000000);
        if (Closed) {
          EXPECT_EQ(*Report->get(field::ReturnValue),
                    llvm::json::Value(nullptr));
          ASSERT_TRUE(Report->getString(field::Diagnostic));
          EXPECT_NE(Report->getString(field::Diagnostic)
                        ->find("inactive dynamic library"),
                    llvm::StringRef::npos);
        } else {
          const uint64_t Result =
              Mode == 2 ? (Route == 1 ? uint64_t(-12) : UINT64_MAX) : 0;
          EXPECT_EQ(Report->getString(field::ReturnValue),
                    llvm::utohexstr(Result, true));
          llvm::support::endian::write64le(Expected.data() + 16, Result);
          llvm::support::endian::write64le(Expected.data() + 24,
                                           Mode == 2 && Route != 1 ? 12 : 733);
          llvm::support::endian::write64le(Expected.data() + 32, 0);
          for (size_t I = 0; I != 12288; ++I) {
            uint8_t Byte = uint8_t(0x31 + I / 4096);
            if (Mode == 1 || (Mode == 2 && (I < 4096 || I >= 8192)) ||
                (Mode == 0 && I >= 4096 && I < 8192))
              Byte = 0;
            if (Mode == 2 && I >= 4096 && I < 8192)
              Byte = 0xcc;
            Expected[256 + I] = Byte;
          }
        }
        const auto *Observed = Report->getObject(field::Android);
        ASSERT_NE(Observed, nullptr);
        const auto *Memory = Observed->getArray(field::Memory);
        ASSERT_NE(Memory, nullptr);
        ASSERT_EQ(Memory->size(), 1u);
        EXPECT_EQ(Memory->front().getAsObject()->getString(field::Bytes),
                  llvm::toHex(Expected, true));
        EXPECT_EQ(runCLIRequest(Path, AndroidNativeAArch64, Options, Output),
                  Closed ? process_cli::Incomplete : process_cli::Success);
        ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Parsed));
      }
    }
  }
#endif
}

TEST_F(ProcessPublic, AndroidErrorTextMatchesSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  struct Case {
    const char *Fixture, *Entry, *Arguments, *Stop;
  };
  const Case Cases[] = {
      {"errors", "error_query", "[536870912,2,0]", "returned"},
      {"errors", "error_query", "[536870912,101,1]", "returned"},
      {"errors", "error_query", "[536870912,\"0xffffffffffffffff\",1]",
       "returned"},
      {"errors", "error_query", "[536870912,\"0xffffffff00000002\",0]",
       "returned"},
      {"errors", "error_query", "[536870912,2,2]", "unsupported_service"},
      {"errors", "error_known_unmapped", "[]", "returned"},
      {"errors", "error_lifetime", "[536870912]", "returned"},
      {"errors", "error_threads", "[536870912]", "returned"},
      {"errors", "error_readonly_tls", "[]", "runtime_failure"},
      {"errors", "error_write_known", "[]", "cpu_failure"},
      {"errors", "error_reentrant_not_admitted", "[536870912]",
       "unsupported_service"},
      {"errors_dynamic", "error_query", "[536870912,2,1]", "returned"}};
  for (const char *Variant : {"O0-none", "O2-relr"}) {
    for (const auto &C : Cases) {
      SCOPED_TRACE(testing::Message()
                   << Variant << ':' << C.Entry << ':' << C.Arguments);
      Path = (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
              (std::string(C.Fixture) + '-' + Variant + ".so"))
                 .string();
      auto Request = llvm::cantFail(llvm::json::parse(R"({
        "backend":"unicorn", "instruction_quantum":7,
        "android":{"initialize":false,"thread_limit":3,
          "observe_memory_on_failure":true,
          "libraries":{"liberrors.so":["strerror"]},
          "read_memory":[{"address":"0x20000000","size":8192}]}})"));
      auto &Android = *Request.getAsObject()->getObject("android");
      Android["entry_symbol"] = C.Entry;
      Android["arguments"] = llvm::cantFail(llvm::json::parse(C.Arguments));
      Android["memory"] = llvm::json::Array{llvm::json::Object{
          {"address", "0x20000000"},
          {"size", 8192},
          {"bytes_hex", llvm::toHex(std::vector<uint8_t>(8192, 0xa5))}}};
      const auto Options = jsonText(std::move(*Request.getAsObject()));
      auto Text = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), AndroidNativeAArch64, Options.c_str()));
      ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
      auto Report = llvm::cantFail(llvm::json::parse(Text));
      EXPECT_EQ(Report.getAsObject()->getString(field::Stop), C.Stop) << Text;
      const bool Returned = llvm::StringRef(C.Stop) == "returned";
      if (Returned)
        EXPECT_EQ(Report.getAsObject()->getString(field::ReturnValue), "0");
      EXPECT_EQ(runCLIRequest(Path, AndroidNativeAArch64, Options, Output),
                Returned ? process_cli::Success : process_cli::Incomplete);
      ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Report));
    }
  }
#endif
}

TEST_F(ProcessPublic, AndroidApplicationValuesMatchSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  struct Case {
    const char *Entry, *Arguments, *Snapshot, *Diagnostic;
  };
  const Case Cases[] = {
      {"jni_content_resolver", "[0]", R"({"phase":"application"})", nullptr},
      {"jni_content_resolver", "[1]", R"({"phase":"application"})",
       "ContentResolver.getType"},
      {"jni_string_builder", "[0]", R"({"phase":"application"})", nullptr},
      {"jni_secure_settings", "[0]", R"({"phase":"application"})",
       "cache is not supplied"},
      {"jni_secure_settings", "[0]",
       R"({"phase":"application","secure_settings_cache":{"android_id":"fixture"}})",
       nullptr},
      {"jni_secure_settings", "[1]",
       R"({"phase":"application","secure_settings_cache":{"android_id":null}})",
       nullptr},
      {"jni_settings_int", "[2147483648,0]",
       R"({"phase":"application","secure_settings_cache":{"android_id":"99"},"global_settings_cache":{"adb_enabled":"-2147483648"}})",
       nullptr},
      {"jni_settings_int", "[4294967273,0]",
       R"({"phase":"application","secure_settings_cache":{"android_id":"99"},"global_settings_cache":{"adb_enabled":null}})",
       nullptr},
      {"jni_application_info", "[0,0]",
       R"({"phase":"application","application_info":{"source_dir":"/test/base.apk"}})",
       nullptr},
      {"jni_application_info", "[0,1]",
       R"({"phase":"application","application_info":{"source_dir":"/test/base.apk"}})",
       "nativeLibraryDir is not supplied"},
      {"jni_application_info", "[1,1]",
       R"({"phase":"application","application_info":{"source_dir":null,"native_library_dir":null}})",
       nullptr},
      {"jni_package_manager", "[2]",
       R"({"phase":"application","package_manager_available":false})", nullptr},
      {"jni_package_application_info", "[0]",
       R"({"phase":"application","package_manager_available":true,"application_info":{"source_dir":"/context.apk"},"application_info_queries":[{"package_name":"test.app","flags":"0","info":{"source_dir":"/query.apk","native_library_dir":null}}]})",
       nullptr},
      {"jni_package_application_info", "[0]",
       R"({"phase":"application","package_manager_available":true,"application_info":{"source_dir":"/context.apk"}})",
       "query is not supplied"},
      {"jni_application_label", "[0]",
       R"({"phase":"application","package_manager_available":true,"package_item_force_safe_labels":false,"application_info":{"source_dir":null,"non_localized_label":"  fixture  "}})",
       nullptr},
      {"jni_application_label", "[0]",
       R"({"phase":"application","package_manager_available":true,"package_item_force_safe_labels":true,"application_info":{"source_dir":null}})",
       "safe application labels are not modeled"},
      {"jni_package_resource_path", "[0]",
       R"({"phase":"application","application_info":{"source_dir":"/mutable-source.apk"},"package_resource_path":"/public//fixture.apk"})",
       nullptr},
      {"jni_package_resource_path", "[1]",
       R"({"phase":"application","application_info":{"source_dir":"/mutable-source.apk"},"package_resource_path":null})",
       nullptr},
      {"jni_package_resource_path", "[0]", R"({"phase":"application"})",
       "package resource path is not supplied"},
      {"jni_package_info", "[0]",
       R"({"phase":"application","package_manager_available":true,"package_info_queries":[{"package_name":"test.app","flags":"0","info":{"package_name":"result.app","version_name":"fixture-version","version_code":"7"}}]})",
       nullptr},
      {"jni_package_info", "[0]",
       R"({"phase":"application","package_manager_available":true})",
       "package info query is not supplied"},
      {"jni_target_sdk", "[0]",
       R"({"phase":"application","package_manager_available":true,"application_info":{"source_dir":null,"target_sdk_version":"28"},"application_info_queries":[{"package_name":"test.app","flags":"0","info":{"source_dir":null,"target_sdk_version":"29"}}]})",
       nullptr},
      {"jni_target_sdk", "[0]", R"({"phase":"application"})",
       "targetSdkVersion is not supplied"},
      {"jni_package_application", "[0]",
       R"({"phase":"application","package_manager_available":true,"application_info":{"source_dir":"context.apk"},)"
       R"("package_info_queries":[{"package_name":"test.app","flags":"0","info":{"application_info":{"source_dir":"parcel.apk"}}}]})",
       nullptr},
      {"jni_package_application", "[2]",
       R"({"phase":"application","package_manager_available":true,"application_info":{"source_dir":"context.apk"},)"
       R"("package_info_queries":[{"package_name":"test.app","flags":"0","info":{"application_info":null}}]})",
       nullptr},
      {"jni_package_application", "[0]",
       R"({"phase":"application","package_manager_available":true,"application_info":{"source_dir":"context.apk"},)"
       R"("package_info_queries":[{"package_name":"test.app","flags":"0","info":{}}]})",
       "applicationInfo is not supplied"},
      {"jni_split_source_dirs", "[0]",
       R"({"phase":"application","package_manager_available":true,"application_info":{"source_dir":"base.apk","split_source_dirs":["first.apk",null,)"
       R"("","first.apk"],"split_public_source_dirs":["other.apk"]},"application_info_queries":[{"package_name":"test.app","flags":"0",)"
       R"("info":{"source_dir":"base.apk","split_source_dirs":["first.apk",null,"","first.apk"],"split_public_source_dirs":["other.apk"]}}]})",
       nullptr},
      {"jni_split_source_dirs", "[2]",
       R"({"phase":"application","application_info":{"source_dir":"base.apk","split_source_dirs":null}})",
       nullptr},
      {"jni_split_source_dirs", "[3]",
       R"({"phase":"application","application_info":{"source_dir":"base.apk","split_source_dirs":[]}})",
       nullptr},
      {"jni_split_source_dirs", "[0]",
       R"({"phase":"application","application_info":{"source_dir":"base.apk"}})",
       "splitSourceDirs is not supplied"},
      {"jni_split_public_source_dirs", "[0]",
       R"({"phase":"application","package_manager_available":true,"application_info":{"source_dir":"base.apk","split_public_source_dirs":["first.apk",)"
       R"(null,"","first.apk"],"split_source_dirs":["other.apk"]},"application_info_queries":[{"package_name":"test.app","flags":"0",)"
       R"("info":{"source_dir":"base.apk","split_public_source_dirs":["first.apk",null,"","first.apk"],"split_source_dirs":["other.apk"]}}]})",
       nullptr},
      {"jni_split_public_source_dirs", "[2]",
       R"({"phase":"application","application_info":{"source_dir":"base.apk","split_public_source_dirs":null}})",
       nullptr},
      {"jni_split_public_source_dirs", "[3]",
       R"({"phase":"application","application_info":{"source_dir":"base.apk","split_public_source_dirs":[]}})",
       nullptr},
      {"jni_split_public_source_dirs", "[0]",
       R"({"phase":"application","application_info":{"source_dir":"base.apk"}})",
       "splitPublicSourceDirs is not supplied"},
      {"jni_default_locale", "[0]",
       R"({"phase":"unattached","default_locale":{"language":"en","country":"US"}})",
       nullptr},
      {"jni_default_locale", "[1]",
       R"({"phase":"unattached","default_locale":{"language":"","country":""}})",
       nullptr},
      {"jni_default_locale", "[0]", R"({"phase":"unattached"})",
       "default Locale is not supplied"},
      {"jni_default_locale", "[0]",
       R"({"phase":"unattached","default_locale":{}})",
       "Locale language is not supplied"},
      {"jni_classloader_paths", "[0]",
       R"({"phase":"application","application_loader_paths":{"dex_zip_files":["/one.apk"],"native_library_directories":["/app/lib"],)"
       R"("system_native_library_directories":["/system/lib"]},"system_loader_paths":{"dex_zip_files":[],"native_library_directories":[],)"
       R"("system_native_library_directories":["/system/lib"]}})",
       nullptr},
      {"jni_classloader_paths", "[0]", R"({"phase":"application"})",
       "application loader paths are not supplied"},
      {"jni_classloader_paths", "[1]",
       R"({"phase":"application","application_loader_paths":{"dex_zip_files":["/one.apk"],"native_library_directories":["/app/lib"],)"
       R"("system_native_library_directories":["/system/lib"]},"system_loader_paths":{"dex_zip_files":[],"native_library_directories":[],)"
       R"("system_native_library_directories":["/system/lib"]}})",
       "unmodeled Android PathClassLoader instance"},
      {"jni_classloader_paths", "[2]",
       R"({"phase":"application","application_loader_paths":{"dex_zip_files":["/one.apk"],"native_library_directories":["/app/lib"],)"
       R"("system_native_library_directories":["/system/lib"]}})",
       "system loader paths are not supplied"},
  };
  for (const char *Variant : {"jni-O0-none", "jni-O2-relr"}) {
    Path = (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
            (std::string(Variant) + ".so"))
               .string();
    for (const auto &Test : Cases) {
      SCOPED_TRACE(Variant);
      SCOPED_TRACE(Test.Entry);
      SCOPED_TRACE(Test.Snapshot);
      auto Request = llvm::cantFail(llvm::json::parse(
          R"({"backend":"unicorn","instruction_quantum":83,"android":{"initialize":false,"thread_limit":2,"jni":true}})"));
      auto &Android = *Request.getAsObject()->getObject(field::Android);
      Android[field::EntrySymbol] = Test.Entry;
      Android[field::Arguments] =
          llvm::cantFail(llvm::json::parse(Test.Arguments));
      Android[field::ApplicationSnapshot] =
          llvm::cantFail(llvm::json::parse(Test.Snapshot));
      const auto Options = jsonText(*Request.getAsObject());
      const auto Text = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), AndroidNativeAArch64, Options.c_str()));
      ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
      auto Report = llvm::cantFail(llvm::json::parse(Text));
      auto *Object = Report.getAsObject();
      ASSERT_NE(Object, nullptr);
      ASSERT_EQ(Object->getString(field::Stop),
                Test.Diagnostic ? "unsupported_service" : "returned")
          << Text;
      if (Test.Diagnostic) {
        EXPECT_EQ(*Object->get(field::ReturnValue), llvm::json::Value(nullptr));
        ASSERT_TRUE(Object->getString(field::Diagnostic));
        EXPECT_NE(Object->getString(field::Diagnostic)->find(Test.Diagnostic),
                  llvm::StringRef::npos);
      } else {
        EXPECT_EQ(Object->getString(field::ReturnValue), "0");
      }
      EXPECT_EQ(runCLIRequest(Path, AndroidNativeAArch64, Options, Output),
                Test.Diagnostic ? process_cli::Incomplete
                                : process_cli::Success);
      ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Report));
    }
  }
#endif
}

TEST_F(ProcessPublic, AndroidExactClassQueriesMatchSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (const char *Variant : {"jni-O0-none", "jni-O2-relr"}) {
    Path = (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
            (std::string(Variant) + ".so"))
               .string();
    for (unsigned Case = 0; Case != 5; ++Case) {
      SCOPED_TRACE(Variant);
      SCOPED_TRACE(Case);
      auto Request = llvm::cantFail(llvm::json::parse(R"({
        "backend":"unicorn","instruction_quantum":83,
        "instruction_limit":200000,"event_limit":5000,
        "android":{"entry_symbol":"jni_loader_exact_query","initialize":false,
          "jni":true,"thread_limit":2,"observe_java_state":true,
          "application_snapshot":{"phase":"attached","class_lookup_failures":[
            {"loader":"boot","exact_query":"fixture.Optional ",
             "message":"declared missing"}]}}})"));
      auto &Android = *Request.getAsObject()->getObject(field::Android);
      Android[field::Arguments] = llvm::json::Array{Case == 4 ? 0 : Case};
      if (Case == 4)
        Android.getObject(field::ApplicationSnapshot)
            ->erase(field::ApplicationClassLookupFailures);
      const auto Options = jsonText(*Request.getAsObject());
      const auto Text = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), AndroidNativeAArch64, Options.c_str()));
      ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
      auto Report = llvm::cantFail(llvm::json::parse(Text));
      auto *Object = Report.getAsObject();
      ASSERT_NE(Object, nullptr);
      ASSERT_EQ(Object->getString(field::Stop), Case == 0 ? "returned"
                                                : Case == 2
                                                    ? "runtime_failure"
                                                    : "unsupported_service")
          << Text;
      if (Case) {
        EXPECT_EQ(*Object->get(field::ReturnValue), llvm::json::Value(nullptr));
        ASSERT_TRUE(Object->getString(field::Diagnostic));
        EXPECT_NE(Object->getString(field::Diagnostic)
                      ->find(Case == 2
                                 ? "unmodeled class: fixture/Optional "
                                 : "no supplied declaration in this loader"),
                  llvm::StringRef::npos);
      } else {
        EXPECT_EQ(Object->getString(field::ReturnValue), "0");
      }
      EXPECT_EQ(runCLIRequest(Path, AndroidNativeAArch64, Options, Output),
                Case ? process_cli::Incomplete : process_cli::Success);
      ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Report));
    }
  }
#endif
}

TEST_F(ProcessPublic, AndroidMessageQueuesMatchSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const char *Diagnostics[] = {nullptr,
                               "native callback state is not empty or known",
                               "native callback state is not empty or known",
                               "blocking wait is unmodeled",
                               "MessageQueue state is not supplied",
                               "Message pool state is not supplied",
                               "no explicit linux_time input",
                               "MessageQueue.nativeWake is unmodeled"};
  for (const char *Variant :
       {"message_queue-O0-none", "message_queue-O2-relr"}) {
    Path = (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
            (std::string(Variant) + ".so"))
               .string();
    for (unsigned Case = 0; Case != 8; ++Case) {
      SCOPED_TRACE(Variant);
      SCOPED_TRACE(Case);
      auto Request = llvm::cantFail(llvm::json::parse(R"({
        "backend":"unicorn","instruction_quantum":7,
        "instruction_limit":50000,"event_limit":5000,
        "linux_time":{"advance_on_idle":true,
          "clocks":[{"id":1,"seconds":10,"nanoseconds":123456789}]},
        "android":{"entry_symbol":"message_queue","initialize":false,"jni":true,
          "observe_java_state":true,"observe_memory_on_failure":true,
          "read_memory":[{"address":"0x20000000","size":4096}],
          "application_snapshot":{"phase":"unattached","main_looper_prepared":true,
            "initial_message_pool_empty":true,
            "initial_main_queue":{"blocked":false,"native_callbacks_empty":true}}}})"));
      auto &OptionsObject = *Request.getAsObject();
      auto &Android = *OptionsObject.getObject(field::Android);
      auto &Snapshot = *Android.getObject(field::ApplicationSnapshot);
      auto &Initial = *Snapshot.getObject(field::ApplicationInitialMainQueue);
      if (Case == 1)
        Initial.erase(field::ApplicationQueueNativeCallbacksEmpty);
      if (Case == 2)
        Initial[field::ApplicationQueueNativeCallbacksEmpty] = false;
      if (Case == 5)
        Snapshot.erase(field::ApplicationMessagePoolEmpty);
      if (Case == 6)
        OptionsObject.erase(field::LinuxTime);
      if (Case == 7)
        Initial["blocked"] = true;
      Android[field::Arguments] =
          llvm::json::Array{"0x20000000", Case == 3   ? 1
                                          : Case == 4 ? 2
                                                      : 0};
      std::vector<uint8_t> Expected(4096, 0xa5);
      Android[field::Memory] = llvm::json::Array{
          llvm::json::Object{{field::Address, "0x20000000"},
                             {field::Size, 4096},
                             {"bytes_hex", llvm::toHex(Expected, true)}}};
      const auto Options = jsonText(OptionsObject);
      auto Text = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), AndroidNativeAArch64, Options.c_str()));
      ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
      auto Parsed = llvm::cantFail(llvm::json::parse(Text));
      const auto *Report = Parsed.getAsObject();
      ASSERT_NE(Report, nullptr);
      ASSERT_EQ(Report->getString(field::Stop),
                Case ? "unsupported_service" : "returned");
      if (Case) {
        EXPECT_EQ(*Report->get(field::ReturnValue), llvm::json::Value(nullptr));
        ASSERT_TRUE(Report->getString(field::Diagnostic));
        EXPECT_NE(Report->getString(field::Diagnostic)->find(Diagnostics[Case]),
                  llvm::StringRef::npos);
      } else {
        EXPECT_EQ(Report->getString(field::ReturnValue), "0");
      }
      if (Case == 0 || Case == 3)
        for (unsigned I = 0; I != 8; ++I)
          llvm::support::endian::write64le(Expected.data() + I * 8,
                                           I == 7 ? 73 : 1);
      const auto *Observed = Report->getObject(field::Android);
      ASSERT_NE(Observed, nullptr);
      const auto *Memory = Observed->getArray(field::Memory);
      ASSERT_NE(Memory, nullptr);
      ASSERT_EQ(Memory->size(), 1u);
      EXPECT_EQ(Memory->front().getAsObject()->getString(field::Bytes),
                llvm::toHex(Expected, true));
      const auto *Java = Observed->getObject(field::JavaState);
      ASSERT_NE(Java, nullptr);
      const auto *Framework = Java->getObject(field::JavaFramework);
      ASSERT_NE(Framework, nullptr);
      const auto *Queue = Framework->getObject(field::JavaMainMessageQueue);
      ASSERT_NE(Queue, nullptr);
      EXPECT_EQ(Queue->getBoolean("complete"), true);
      EXPECT_EQ(Queue->getBoolean("blocked"), Case == 7);
      const auto *Messages = Queue->getArray("messages");
      ASSERT_NE(Messages, nullptr);
      EXPECT_EQ(Messages->size(), Case == 1 || Case == 2 || Case == 4 ? 2u
                                  : Case == 7                         ? 1u
                                                                      : 0u);
      for (const auto &Message : *Messages) {
        const auto *Value = Message.getAsObject();
        ASSERT_NE(Value, nullptr);
        EXPECT_EQ(Value->getString("callback"), "0");
        EXPECT_EQ(Value->getInteger("flags"), 1);
        EXPECT_EQ(Value->getInteger("when"), 10123);
      }
      EXPECT_EQ(runCLIRequest(Path, AndroidNativeAArch64, Options, Output),
                Case ? process_cli::Incomplete : process_cli::Success);
      ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Parsed));
    }
  }
#endif
}

TEST_F(ProcessPublic, AndroidURLValuesMatchSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (const char *Variant : {"jni-O0-none", "jni-O2-relr"}) {
    Path = (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
            (std::string(Variant) + ".so"))
               .string();
    for (unsigned Case = 0; Case != 17; ++Case) {
      SCOPED_TRACE(Variant);
      SCOPED_TRACE(Case);
      auto Request = llvm::cantFail(llvm::json::parse(R"({
        "backend":"unicorn","instruction_quantum":83,
        "instruction_limit":200000,"event_limit":5000,
        "android":{"entry_symbol":"jni_url_values","initialize":false,
          "jni":true,"observe_java_state":true}})"));
      auto &OptionsObject = *Request.getAsObject();
      auto &Android = *OptionsObject.getObject(field::Android);
      const char *Stop = "returned";
      const char *Diagnostic = nullptr;
      if (Case < 7) {
        Android[field::Arguments] = llvm::json::Array{Case};
        if (Case == 1 || Case == 2 || Case == 3) {
          Stop = "unsupported_service";
          Diagnostic = Case == 1   ? "bounded HTTP value contract"
                       : Case == 2 ? "URL.openConnection"
                                   : "handler-dependent equality";
        } else if (Case == 6) {
          Stop = "runtime_failure";
          Diagnostic = "nonvirtual method ID does not match class lookup";
        }
      } else {
        Android[field::EntrySymbol] = "jni_url_handler";
        const unsigned Mode = Case == 16 ? 0 : (Case - 7) / 3;
        Android[field::Arguments] = llvm::json::Array{Mode, (Case - 7) % 3};
        llvm::json::Value Message = nullptr;
        if (Mode == 1)
          Message = "";
        else if (Mode == 2)
          Message = std::string("local\0", 6) + "离线";
        llvm::json::Array Handlers{
            llvm::json::Object{{"scheme", "http"}, {"message", Message}}};
        if (Case != 16)
          Handlers.push_back(
              llvm::json::Object{{"scheme", "https"}, {"message", Message}});
        else {
          Stop = "unsupported_service";
          Diagnostic = "handler state is not supplied";
        }
        Android[field::ApplicationSnapshot] = llvm::json::Object{
            {"phase", "unattached"},
            {field::ApplicationURLFailureHandlers, std::move(Handlers)}};
      }
      const auto Options = jsonText(OptionsObject);
      auto Text = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), AndroidNativeAArch64, Options.c_str()));
      ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
      auto Report = llvm::cantFail(llvm::json::parse(Text));
      auto *Object = Report.getAsObject();
      ASSERT_NE(Object, nullptr);
      ASSERT_EQ(Object->getString(field::Stop), Stop) << Text;
      if (Diagnostic) {
        EXPECT_EQ(*Object->get(field::ReturnValue), llvm::json::Value(nullptr));
        ASSERT_TRUE(Object->getString(field::Diagnostic));
        EXPECT_NE(Object->getString(field::Diagnostic)->find(Diagnostic),
                  llvm::StringRef::npos);
      } else {
        EXPECT_EQ(Object->getString(field::ReturnValue), "0");
      }
      EXPECT_EQ(runCLIRequest(Path, AndroidNativeAArch64, Options, Output),
                Diagnostic ? process_cli::Incomplete : process_cli::Success);
      ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Report));
    }
  }
#endif
}

TEST_F(ProcessPublic, AndroidNamedUTF8ArraysMatchSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (const char *Variant :
       {"string_getbytes-O0-none", "string_getbytes-O2-relr"}) {
    Path = (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
            (std::string(Variant) + ".so"))
               .string();
    for (unsigned Mode = 0; Mode != 3; ++Mode) {
      SCOPED_TRACE(Variant);
      SCOPED_TRACE(Mode);
      auto Request = llvm::cantFail(llvm::json::parse(R"({
        "backend":"unicorn","instruction_quantum":83,
        "instruction_limit":200000,"event_limit":5000,
        "android":{"entry_symbol":"string_getbytes","initialize":false,
          "jni":true,"observe_java_state":true,"observe_memory_on_failure":true,
          "read_memory":[{"address":"0x20000000","size":4096}]}})"));
      auto &OptionsObject = *Request.getAsObject();
      auto &Android = *OptionsObject.getObject(field::Android);
      Android[field::Arguments] = llvm::json::Array{"0x20000000", Mode};
      std::vector<uint8_t> Expected(4096, 0xa5);
      Android[field::Memory] = llvm::json::Array{
          llvm::json::Object{{field::Address, "0x20000000"},
                             {field::Size, 4096},
                             {"bytes_hex", llvm::toHex(Expected, true)}}};
      const auto Options = jsonText(OptionsObject);
      auto Text = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), AndroidNativeAArch64, Options.c_str()));
      ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
      auto Report = llvm::cantFail(llvm::json::parse(Text));
      auto *Object = Report.getAsObject();
      ASSERT_NE(Object, nullptr);
      ASSERT_EQ(Object->getString(field::Stop),
                Mode == 2 ? "unsupported_service" : "returned")
          << Text;
      if (Mode == 2) {
        EXPECT_EQ(*Object->get(field::ReturnValue), llvm::json::Value(nullptr));
        ASSERT_TRUE(Object->getString(field::Diagnostic));
        EXPECT_NE(
            Object->getString(field::Diagnostic)->find("charset is unmodeled"),
            llvm::StringRef::npos);
      } else {
        EXPECT_EQ(Object->getString(field::ReturnValue), "0");
      }
      if (Mode == 0)
        for (unsigned I = 0; I != 6; ++I)
          llvm::support::endian::write64le(Expected.data() + I * 8, 1);
      if (Mode == 1)
        llvm::support::endian::write64le(Expected.data(), 17);
      const auto *Observed = Object->getObject(field::Android);
      ASSERT_NE(Observed, nullptr);
      const auto *Memory = Observed->getArray(field::Memory);
      ASSERT_NE(Memory, nullptr);
      ASSERT_EQ(Memory->size(), 1u);
      EXPECT_EQ(Memory->front().getAsObject()->getString(field::Bytes),
                llvm::toHex(Expected, true));
      EXPECT_EQ(runCLIRequest(Path, AndroidNativeAArch64, Options, Output),
                Mode == 2 ? process_cli::Incomplete : process_cli::Success);
      ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Report));
    }
  }
#endif
}

TEST_F(ProcessPublic, AndroidJNIObjectsAndThreadsMatchSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android fixtures unavailable";
#else
  Path = (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) / "jni-O2-relr.so")
             .string();
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  auto Request = llvm::cantFail(llvm::json::parse(R"({
    "backend":"unicorn", "android":{"entry_symbol":"jni_vm_lifecycle",
    "initialize":false,"thread_limit":2,"jni":true}})"));
  for (const char *Entry :
       {"jni_vm_lifecycle", "jni_string_values", "jni_arrays", "jni_references",
        "jni_threads", "jni_art_string_edges", "jni_method_values",
        "jni_method_exceptions"}) {
    (*Request.getAsObject()->getObject("android"))["entry_symbol"] = Entry;
    const auto Options = jsonText(*Request.getAsObject());
    const auto Text = takeString(neverd_emulate_process_json(
        Session, Path.c_str(), AndroidNativeAArch64, Options.c_str()));
    ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
    auto Report = llvm::cantFail(llvm::json::parse(Text));
    ASSERT_EQ(Report.getAsObject()->getString(field::Stop), "returned") << Text;
    EXPECT_EQ(Report.getAsObject()->getString(field::ReturnValue), "0");
    EXPECT_EQ(runCLIRequest(Path, AndroidNativeAArch64, Options, Output),
              process_cli::Success);
    ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Report));
  }
  (*Request.getAsObject()->getObject("android"))["jni"] = 1;
  EXPECT_TRUE(takeString(neverd_emulate_process_json(
                             Session, Path.c_str(), AndroidNativeAArch64,
                             jsonText(*Request.getAsObject()).c_str()))
                  .empty());
#endif
}

TEST_F(ProcessPublic, AndroidBinderProtocolMatchesSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android fixtures unavailable";
#else
  Path =
      (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) / "binder-O2-relr.so")
          .string();
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  auto Request = llvm::cantFail(llvm::json::parse(R"({
    "backend":"unicorn", "android":{"entry_symbol":"binder_control",
    "initialize":false,"thread_limit":5},
    "linux_files":{"files":[],"devices":[{"path":"/dev/binder",
      "access_mode":2,"open_errno":0,"ioctls":[],"binder":true}]}})"));
  for (const char *Entry :
       {"binder_control", "binder_descriptions", "binder_threads",
        "binder_service_transactions", "binder_missing_targets"}) {
    (*Request.getAsObject()->getObject("android"))["entry_symbol"] = Entry;
    const auto Options = jsonText(*Request.getAsObject());
    const auto Text = takeString(neverd_emulate_process_json(
        Session, Path.c_str(), AndroidNativeAArch64, Options.c_str()));
    ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
    auto Report = llvm::cantFail(llvm::json::parse(Text));
    ASSERT_EQ(Report.getAsObject()->getString(field::Stop), "returned") << Text;
    EXPECT_EQ(Report.getAsObject()->getString(field::ReturnValue), "0");
    EXPECT_EQ(runCLIRequest(Path, AndroidNativeAArch64, Options, Output),
              process_cli::Success);
    ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Report));
  }
  (*Request.getAsObject()
        ->getObject("linux_files")
        ->getArray("devices")
        ->front()
        .getAsObject())["binder"] = 1;
  EXPECT_TRUE(takeString(neverd_emulate_process_json(
                             Session, Path.c_str(), AndroidNativeAArch64,
                             jsonText(*Request.getAsObject()).c_str()))
                  .empty());
#endif
}

TEST_F(ProcessPublic, AndroidGPUCommandsMatchSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android fixtures unavailable";
#else
  Path =
      (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) / "devices-O2-relr.so")
          .string();
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  auto Request = llvm::cantFail(llvm::json::parse(R"({
    "backend":"unicorn","android":{"entry_symbol":"devices_gpu_commands",
      "initialize":false,"arguments":[0],"libraries":{"libdevice.so":["ioctl","mmap","dup","dup3"]}},
    "linux_files":{"files":[],"devices":[{"path":"/fixture/control",
      "access_mode":2,"open_errno":0,"ioctls":[],
      "kgsl_contexts":{"memstore_offset":4096,"context_limit":3,"synchronous_a3xx":true},
      "read_only_mappings":[{"offset":4096,"bytes_hex":""}]}]}})"));
  auto *Device = Request.getAsObject()
                     ->getObject("linux_files")
                     ->getArray("devices")
                     ->front()
                     .getAsObject();
  (*Device->getArray("read_only_mappings"))[0].getAsObject()->operator[](
      "bytes_hex") = std::string(8192, '0');
  (*Device->getObject("kgsl_contexts"))["memory_objects"] = true;
  for (const char *Entry :
       {"devices_gpu_commands", "devices_gpu_objects", "devices_gpu_legacy"}) {
    (*Request.getAsObject()->getObject("android"))["entry_symbol"] = Entry;
    for (unsigned Route = 0; Route != 4; ++Route) {
      SCOPED_TRACE(Route);
      (*Request.getAsObject()->getObject("android"))["arguments"] =
          llvm::json::Array{Route, 1};
      const auto Options = jsonText(*Request.getAsObject());
      const auto Text = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), AndroidNativeAArch64, Options.c_str()));
      ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
      auto Report = llvm::cantFail(llvm::json::parse(Text));
      ASSERT_EQ(Report.getAsObject()->getString(field::Stop), "returned")
          << Text;
      EXPECT_EQ(Report.getAsObject()->getString(field::ReturnValue), "0");
      EXPECT_EQ(runCLIRequest(Path, AndroidNativeAArch64, Options, Output),
                process_cli::Success);
      ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Report));
    }
  }
#endif
}

TEST_F(ProcessPublic, AndroidLiveProcAndMaintainedStatMatchSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android fixtures unavailable";
#else
  Path = (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) / "proc-O2-relr.so")
             .string();
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  auto Request = llvm::cantFail(llvm::json::parse(R"({
    "backend":"unicorn", "linux_time":{"clocks":[{"id":0,"seconds":100,"nanoseconds":7}]},
    "android":{"entry_symbol":"proc_write_stat","initialize":false,"thread_limit":2},
    "linux_files":{"dynamic_proc":{"name":"fixture","parent_pid":1},
      "files":[{"path":"/fixture/value","bytes_hex":"616263","write_limit":16384,
        "track_write_metadata":true,
        "metadata":{"device":0,"inode":42,"mode":33188,"link_count":1,
          "uid":0,"gid":0,"size":3,"block_size":4096,"blocks":8,
          "access_time":{"seconds":0,"nanoseconds":0},
          "modification_time":{"seconds":0,"nanoseconds":0},
          "change_time":{"seconds":0,"nanoseconds":0}}}]}})"));
  for (const char *Entry :
       {"proc_write_stat", "proc_live_maps", "proc_thread_lifecycle"}) {
    (*Request.getAsObject()->getObject("android"))["entry_symbol"] = Entry;
    const auto Options = jsonText(*Request.getAsObject());
    const auto Text = takeString(neverd_emulate_process_json(
        Session, Path.c_str(), AndroidNativeAArch64, Options.c_str()));
    ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
    auto Report = llvm::cantFail(llvm::json::parse(Text));
    ASSERT_EQ(Report.getAsObject()->getString(field::Stop), "returned") << Text;
    EXPECT_EQ(Report.getAsObject()->getString(field::ReturnValue), "0");
    EXPECT_EQ(runCLIRequest(Path, AndroidNativeAArch64, Options, Output),
              process_cli::Success);
    ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Report));
  }
#endif
}

TEST_F(ProcessPublic, AndroidNegativePrctlMatchesSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const uint64_t Values[] = {0x80000000ULL, 0xffffffffULL,
                             0xffffffff80000000ULL, 0x1234567880000001ULL,
                             0x800000000000000fULL};
  for (const char *Variant : {"proc-O0-none", "proc-O2-relr"}) {
    Path = (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
            (std::string(Variant) + ".so"))
               .string();
    for (unsigned Route = 0; Route != 5; ++Route) {
      for (unsigned Case = 0; Case != 5; ++Case) {
        if (Route == 4 && Case)
          continue;
        SCOPED_TRACE(Variant);
        SCOPED_TRACE(Route);
        SCOPED_TRACE(Case);
        auto Request = llvm::cantFail(llvm::json::parse(R"({
          "backend":"unicorn","instruction_quantum":17,
          "android":{"initialize":false,"entry_symbol":"proc_prctl_option",
            "memory":[{"address":"0x20000000","size":4096}],
            "read_memory":[{"address":"0x20000000","size":4096}],
            "libraries":{"libprctl.so":["prctl"]}}})"));
        auto &Android = *Request.getAsObject()->getObject(field::Android);
        Android[field::Arguments] = llvm::json::Array{
            "0x20000000", "0x" + llvm::utohexstr(Values[Case]), Route};
        std::vector<uint8_t> Expected(4096, 0xa5);
        (*Android.getArray(field::Memory)
              ->front()
              .getAsObject())[field::Bytes] = llvm::toHex(Expected, true);
        const auto Options = jsonText(*Request.getAsObject());
        const auto Text = takeString(neverd_emulate_process_json(
            Session, Path.c_str(), AndroidNativeAArch64, Options.c_str()));
        ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
        auto Report = llvm::cantFail(llvm::json::parse(Text));
        auto *Object = Report.getAsObject();
        ASSERT_NE(Object, nullptr);
        const bool Refused = Route == 4 || Case == 4;
        ASSERT_EQ(Object->getString(field::Stop),
                  Refused ? "unsupported_service" : "returned")
            << Text;
        if (Refused) {
          EXPECT_EQ(*Object->get(field::ReturnValue),
                    llvm::json::Value(nullptr));
          ASSERT_TRUE(Object->getString(field::Diagnostic));
          EXPECT_NE(Object->getString(field::Diagnostic)
                        ->find(Route == 4 ? "inactive" : "prctl operation"),
                    llvm::StringRef::npos);
        } else {
          EXPECT_EQ(Object->getString(field::ReturnValue), "0");
          llvm::support::endian::write64le(
              Expected.data(), Route == 1 ? uint64_t(0) - 22 : UINT64_MAX);
          llvm::support::endian::write64le(Expected.data() + 8,
                                           Route == 1 ? 733 : 22);
        }
        auto *State = Object->getObject(field::Android);
        ASSERT_NE(State, nullptr);
        auto *Memory = State->getArray(field::Memory);
        ASSERT_NE(Memory, nullptr);
        ASSERT_EQ(Memory->size(), 1u);
        ASSERT_NE(Memory->front().getAsObject(), nullptr);
        EXPECT_EQ(Memory->front().getAsObject()->getString(field::Bytes),
                  llvm::toHex(Expected, true));
        EXPECT_EQ(runCLIRequest(Path, AndroidNativeAArch64, Options, Output),
                  Refused ? process_cli::Incomplete : process_cli::Success);
        ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Report));
      }
    }
  }
#endif
}

TEST_F(ProcessPublic, AndroidPrctlNamesAndThreadRetirementMatchSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  auto Request = llvm::cantFail(llvm::json::parse(R"({
    "backend":"unicorn",
    "android":{"initialize":false,"thread_limit":2},
    "linux_kernel":{"anonymous_vma_names":"android-user-pointer"},
    "linux_files":{"files":[],"dynamic_proc":{"name":"fixture","parent_pid":1}}})"));
  for (const char *Fixture : {"proc-O0-none.so", "proc-O2-relr.so"}) {
    SCOPED_TRACE(Fixture);
    Path =
        (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) / Fixture).string();
    for (const char *Entry :
         {"proc_prctl_names", "proc_prctl_thread_retirement"}) {
      SCOPED_TRACE(Entry);
      (*Request.getAsObject()->getObject("android"))["entry_symbol"] = Entry;
      const auto Options = jsonText(*Request.getAsObject());
      const auto Text = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), AndroidNativeAArch64, Options.c_str()));
      ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
      auto Report = llvm::cantFail(llvm::json::parse(Text));
      ASSERT_EQ(Report.getAsObject()->getString(field::Stop), "returned")
          << Text;
      EXPECT_EQ(Report.getAsObject()->getString(field::ReturnValue), "0");
      EXPECT_EQ(runCLIRequest(Path, AndroidNativeAArch64, Options, Output),
                process_cli::Success);
      ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Report));
    }
  }
#endif
}

TEST_F(ProcessPublic, AndroidTokenNamesAndMutationsMatchSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  Path =
      (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) / "token-O2-relr.so")
          .string();
  const std::string Request =
      R"({"backend":"unicorn","android":{"entry_symbol":"token_dynamic","initialize":false,"arguments":["0x20000000",0],"memory":[{"address":"0x20000000","size":4096}],"read_memory":[{"address":"0x20000000","size":40}],"libraries":{"libtokens.so":["strtok_r"]}}})";
  auto Text = takeString(neverd_emulate_process_json(
      Session, Path.c_str(), AndroidNativeAArch64, Request.c_str()));
  ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
  auto Parsed = llvm::cantFail(llvm::json::parse(Text));
  ASSERT_EQ(Parsed.getAsObject()->getString(field::Stop), "returned");
  EXPECT_EQ(Parsed.getAsObject()->getString(field::ReturnValue), "0");
  const auto *Android = Parsed.getAsObject()->getObject(field::Android);
  ASSERT_NE(Android, nullptr);
  const auto *Calls = Android->getArray(field::NativeCalls);
  ASSERT_NE(Calls, nullptr);
  const llvm::json::Object *Lookup = nullptr;
  unsigned Tokens = 0;
  for (const auto &Event : *Calls) {
    const auto *Call = Event.getAsObject();
    if (Call->getString(field::Name) == "dlsym") {
      Lookup = Call;
      EXPECT_EQ(Call->getString(field::Symbol), "strtok_r");
      EXPECT_EQ(Call->getString(field::Library), "libtokens.so");
    }
    if (Call->getString(field::Name) == "strtok_r") {
      ASSERT_NE(Lookup, nullptr);
      EXPECT_EQ(Call->getString(field::Library), "libtokens.so");
      EXPECT_EQ(Call->getString(field::PC), Lookup->getString(field::Result));
      ++Tokens;
    }
  }
  EXPECT_EQ(Tokens, 2u);
  const auto *Memory = Android->getArray(field::Memory);
  ASSERT_NE(Memory, nullptr);
  ASSERT_EQ(Memory->size(), 1u);
  EXPECT_EQ((*Memory)[0].getAsObject()->getString(field::Bytes),
            "000000000000000006000000000000000000000000000000"
            "06000000000000000000000000000000");
  EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const int CLIStatus =
      runCLIRequest(Path, AndroidNativeAArch64, Request, Output);
  EXPECT_EQ(CLIStatus, process_cli::Success);
  auto Report = llvm::MemoryBuffer::getFile(Output);
  ASSERT_TRUE(bool(Report));
  EXPECT_EQ(llvm::cantFail(llvm::json::parse((*Report)->getBuffer())), Parsed);
#endif
}

TEST_F(ProcessPublic, IndependentProcessCPUClocksMatchSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  struct ClockCase {
    const char *Kernel;
    int32_t ID;
    bool Sample, Missing, Invalid;
  };
  const ClockCase Cases[] = {
      {R"({"tasks":[]})", -6, false, false, false},
      {R"({"tasks":[]})", -16006, false, false, true},
      {R"({"tasks":[{"id":2000,"group_leader":true}]})", -16006, true, false,
       false},
      {R"({"tasks":[{"id":2000,"group_leader":true}]})", -16006, false, true,
       false},
      {R"({"tasks":[{"id":2000,"group_leader":false}]})", -16006, false, false,
       true},
      {R"({"unavailable_syscalls":[]})", -6, false, true, false}};
  for (const char *Fixture : {"time-O0-none.so", "time-O2-relr.so"}) {
    Path =
        (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) / Fixture).string();
    for (const auto &Case : Cases) {
      for (unsigned Op : {2u, 4u}) {
        SCOPED_TRACE(testing::Message() << Fixture << ':' << Case.Kernel << ':'
                                        << Case.ID << ':' << Op);
        auto Request = llvm::cantFail(llvm::json::parse(R"({
          "backend":"unicorn","linux_time":{"clocks":[
            {"id":2,"seconds":3,"nanoseconds":4}]},
          "android":{"entry_symbol":"time_call","initialize":false,
            "memory":[{"address":"0x20000000","size":4096}],
            "read_memory":[{"address":"0x20000000","size":64}]}})"));
        auto &Object = *Request.getAsObject();
        Object["linux_kernel"] = llvm::cantFail(llvm::json::parse(Case.Kernel));
        if (Case.Sample)
          Object.getObject("linux_time")
              ->getArray("clocks")
              ->push_back(llvm::json::Object{
                  {"id", -16006}, {"seconds", 7}, {"nanoseconds", 9}});
        auto &Android = *Object.getObject("android");
        Android["arguments"] =
            llvm::json::Array{Op, 0x1234567800000000ULL | uint32_t(Case.ID),
                              "0x20000000", "0x20000020", 0};
        std::vector<uint8_t> Expected(64, 0xa5);
        (*Android.getArray("memory"))[0].getAsObject()->operator[](
            "bytes_hex") = llvm::toHex(Expected, true);
        const auto Options = jsonText(Object);
        const auto Text = takeString(neverd_emulate_process_json(
            Session, Path.c_str(), AndroidNativeAArch64, Options.c_str()));
        ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
        auto Report = llvm::cantFail(llvm::json::parse(Text));
        const auto &Result = *Report.getAsObject();
        ASSERT_EQ(Result.getString(field::Stop),
                  Case.Missing ? "unsupported_service" : "returned")
            << Text;
        if (!Case.Missing) {
          EXPECT_EQ(Result.getString(field::ReturnValue),
                    Case.Invalid
                        ? (Op == 2 ? "ffffffffffffffff" : "ffffffffffffffea")
                        : "0");
          if (!Case.Invalid) {
            llvm::support::endian::write64le(Expected.data(),
                                             Case.Sample ? 7 : 3);
            llvm::support::endian::write64le(Expected.data() + 8,
                                             Case.Sample ? 9 : 4);
          }
          llvm::support::endian::write32le(Expected.data() + 32,
                                           Case.Invalid && Op == 2 ? 22 : 73);
        }
        EXPECT_EQ(Result.getObject(field::Android)
                      ->getArray(field::Memory)
                      ->front()
                      .getAsObject()
                      ->getString(field::Bytes),
                  llvm::toHex(Expected, true));
        EXPECT_EQ(runCLIRequest(Path, AndroidNativeAArch64, Options, Output),
                  Case.Missing ? process_cli::Incomplete
                               : process_cli::Success);
        ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Report));
      }
    }
  }
#endif
}

TEST_F(ProcessPublic, RawMonotonicEpochAndMissingInputMatchSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (const char *Fixture :
       {"time-O0-none.so", "time-O0-android.so", "time-O0-relr.so",
        "time-O2-none.so", "time-O2-android.so", "time-O2-relr.so"}) {
    Path =
        (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) / Fixture).string();
    for (bool Observed : {false, true}) {
      auto Request = llvm::cantFail(llvm::json::parse(R"({
        "backend":"unicorn","linux_time":{"advance_on_idle":true,
          "clocks":[{"id":1,"seconds":123,"nanoseconds":456789}]},
        "android":{"entry_symbol":"raw_clock_sequence","initialize":false,
          "arguments":["0x20000000"],
          "libraries":{"libclock-model.so":["clock_gettime"]},
          "memory":[{"address":"0x20000000","size":4096}],
          "read_memory":[{"address":"0x20000000","size":136}]}})"));
      auto &Object = *Request.getAsObject();
      if (Observed)
        Object.getObject("linux_time")
            ->getArray("clocks")
            ->push_back(llvm::json::Object{{"id", 4},
                                           {"seconds", int64_t(4294967301)},
                                           {"nanoseconds", 999999996}});
      std::vector<uint8_t> Expected(136, 0xa5);
      Object.getObject("android")->getArray("memory")->front().getAsObject()->
      operator[]("bytes_hex") = llvm::toHex(Expected, true);
      const auto Options = jsonText(Object);
      const auto Text = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), AndroidNativeAArch64, Options.c_str()));
      ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
      auto Report = llvm::cantFail(llvm::json::parse(Text));
      const auto &Result = *Report.getAsObject();
      ASSERT_EQ(Result.getString(field::Stop),
                Observed ? "returned" : "unsupported_service")
          << Text;
      if (Observed) {
        EXPECT_EQ(Result.getString(field::ReturnValue), "0");
        const uint64_t Words[] = {
            4294967301, 999999996, 4294967301, 999999996, 4294967301, 999999996,
            123,        456789,    4294967303, 3,         4294967303, 3,
            4294967303, 3,         124,        456796,    73};
        for (size_t I = 0; I != std::size(Words); ++I)
          llvm::support::endian::write64le(Expected.data() + I * 8, Words[I]);
      }
      EXPECT_EQ(Result.getObject(field::Android)
                    ->getArray(field::Memory)
                    ->front()
                    .getAsObject()
                    ->getString(field::Bytes),
                llvm::toHex(Expected, true));
      EXPECT_EQ(runCLIRequest(Path, AndroidNativeAArch64, Options, Output),
                Observed ? process_cli::Success : process_cli::Incomplete);
      ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Report));
    }
  }
#endif
}

TEST_F(ProcessPublic, RelativeSleepCompletesRawEventAndClockThroughSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  Path =
      (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) / "sleep-O2-relr.so")
          .string();
  const std::string Request = R"({"backend":"unicorn","linux_time":{
    "advance_on_idle":true,"clocks":[{"id":0,"seconds":"4294967297","nanoseconds":999999998}]},
    "android":{"entry_symbol":"sleep_call","initialize":false,"thread_limit":2,
      "arguments":[2,"0x20000000","0x20000000","0x20000020"],
      "memory":[{"address":"0x20000000","size":4096,"bytes_hex":"00000000000000000500000000000000"}],
      "read_memory":[{"address":"0x20000000","size":64}]}})";
  auto Text = takeString(neverd_emulate_process_json(
      Session, Path.c_str(), AndroidNativeAArch64, Request.c_str()));
  ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
  auto Parsed = llvm::cantFail(llvm::json::parse(Text));
  ASSERT_EQ(Parsed.getAsObject()->getString(field::Stop), "returned");
  EXPECT_EQ(Parsed.getAsObject()->getString(field::ReturnValue), "0");
  const auto *Services = Parsed.getAsObject()->getArray(field::Services);
  ASSERT_NE(Services, nullptr);
  ASSERT_EQ(Services->size(), 1u);
  EXPECT_EQ(Services->front().getAsObject()->getString(field::Result), "0");
  const auto *Android = Parsed.getAsObject()->getObject(field::Android);
  ASSERT_NE(Android, nullptr);
  EXPECT_EQ(Android->getArray(field::Memory)
                ->front()
                .getAsObject()
                ->getString(field::Bytes),
            "0000000000000000050000000000000000000000000000000000000000000000"
            "4900000000000000020000000100000003000000000000000200000001000000");
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const int CLIStatus =
      runCLIRequest(Path, AndroidNativeAArch64, Request, Output);
  EXPECT_EQ(CLIStatus, process_cli::Success);
  ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Parsed));
#endif
}

TEST_F(ProcessPublic, AndroidSecondSleepReportsMatchSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  Path =
      (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) / "sleep-O2-relr.so")
          .string();
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (unsigned Threads : {1, 2}) {
    for (unsigned Case = 0; Case < 5; ++Case) {
      SCOPED_TRACE(testing::Message() << Threads << ":" << Case);
      const bool Fails = Case >= 3;
      std::vector<uint8_t> Expected(64, 0xa5);
      llvm::json::Object Request{
          {"backend", "unicorn"},
          {"linux_time",
           llvm::json::Object{{"advance_on_idle", Case != 3},
                              {"clocks", llvm::json::Array{llvm::json::Object{
                                             {"id", 0},
                                             {"seconds", "4294967297"},
                                             {"nanoseconds", 999999998}}}}}},
          {"android",
           llvm::json::Object{
               {"entry_symbol", "sleep_seconds"},
               {"initialize", false},
               {"thread_limit", Threads},
               {"arguments",
                llvm::json::Array{Case == 0   ? "0"
                                  : Case == 2 ? "4294967295"
                                              : "3",
                                  Case == 4 ? 2 : 1, "0x20000000"}},
               {"memory", llvm::json::Array{llvm::json::Object{
                              {"address", "0x20000000"},
                              {"size", 4096},
                              {"bytes_hex", llvm::toHex(Expected, true)}}}},
               {"read_memory", llvm::json::Array{llvm::json::Object{
                                   {"address", "0x20000000"}, {"size", 64}}}},
               {"libraries",
                llvm::json::Object{
                    {"libsleep-model.so", llvm::json::Array{"sleep"}}}}}}};
      const auto Options = jsonText(std::move(Request));
      const auto Text = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), AndroidNativeAArch64, Options.c_str()));
      ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
      const auto Parsed = llvm::cantFail(llvm::json::parse(Text));
      const auto *Report = Parsed.getAsObject();
      ASSERT_NE(Report, nullptr);
      EXPECT_EQ(Report->getString(field::Stop),
                Fails ? "unsupported_service" : "returned");
      const auto *Android = Report->getObject(field::Android);
      ASSERT_NE(Android, nullptr);
      const auto *Calls = Android->getArray(field::NativeCalls);
      ASSERT_NE(Calls, nullptr);
      const llvm::json::Object *Lookup = nullptr, *Call = nullptr;
      for (const auto &Event : *Calls) {
        const auto *E = Event.getAsObject();
        if (E->getString(field::Name) == "dlsym")
          Lookup = E;
        if (E->getString(field::Name) == "sleep")
          Call = E;
      }
      ASSERT_NE(Lookup, nullptr);
      ASSERT_NE(Call, nullptr);
      EXPECT_EQ(Lookup->getString(field::Symbol), "sleep");
      EXPECT_EQ(Call->getString(field::Library), "libsleep-model.so");
      EXPECT_EQ(Call->getString(field::PC), Lookup->getString(field::Result));
      if (Fails) {
        EXPECT_EQ(*Report->get(field::ReturnValue), llvm::json::Value(nullptr));
        EXPECT_EQ(*Call->get(field::Result), llvm::json::Value(nullptr));
        ASSERT_TRUE(Report->getString(field::Diagnostic));
        EXPECT_NE(Report->getString(field::Diagnostic)
                      ->find(Case == 3 ? "advance_on_idle"
                                       : "inactive dynamic library"),
                  llvm::StringRef::npos);
      } else {
        const char *Left = Case == 2 ? "80000000" : "0";
        EXPECT_EQ(Report->getString(field::ReturnValue), Left);
        EXPECT_EQ(Call->getString(field::Result), Left);
        const uint64_t Seconds = 4294967297 + (Case == 0   ? 0
                                               : Case == 1 ? 3
                                                           : 2147483647);
        const uint64_t Values[] = {73, Seconds, 999999998, Seconds};
        for (size_t I = 0; I < std::size(Values); ++I)
          llvm::support::endian::write64le(Expected.data() + I * 8, Values[I]);
      }
      EXPECT_EQ(Android->getArray(field::Memory)
                    ->front()
                    .getAsObject()
                    ->getString(field::Bytes),
                llvm::toHex(Expected, true));
      EXPECT_EQ(runCLIRequest(Path, AndroidNativeAArch64, Options, Output),
                Fails ? process_cli::Incomplete : process_cli::Success);
      ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Parsed));
    }
  }
#endif
}

TEST_F(ProcessPublic, ExplicitClockValuesAndNamesMatchSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  Path = (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) / "time-O2-relr.so")
             .string();
  const std::string Request = R"({"backend":"unicorn","linux_time":{
    "clocks":[{"id":0,"seconds":"4294967297","nanoseconds":987654321},
              {"id":1,"seconds":123,"nanoseconds":456789}]},
    "android":{"entry_symbol":"time_dynamic","initialize":false,
      "arguments":["0x20000000",0],"memory":[{"address":"0x20000000","size":4096}],
      "read_memory":[{"address":"0x20000000","size":40}],
      "libraries":{"libclock-model.so":["time","clock_gettime","gettimeofday"]}}})";
  auto Text = takeString(neverd_emulate_process_json(
      Session, Path.c_str(), AndroidNativeAArch64, Request.c_str()));
  ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
  auto Parsed = llvm::cantFail(llvm::json::parse(Text));
  ASSERT_EQ(Parsed.getAsObject()->getString(field::Stop), "returned");
  EXPECT_EQ(Parsed.getAsObject()->getString(field::ReturnValue), "0");
  const auto *Android = Parsed.getAsObject()->getObject(field::Android);
  ASSERT_NE(Android, nullptr);
  for (const char *Name : {"time", "clock_gettime", "gettimeofday"}) {
    const llvm::json::Object *Lookup = nullptr, *Call = nullptr;
    for (const auto &Event : *Android->getArray(field::NativeCalls)) {
      const auto *E = Event.getAsObject();
      if (E->getString(field::Name) == "dlsym" &&
          E->getString(field::Symbol) == Name)
        Lookup = E;
      if (E->getString(field::Name) == Name)
        Call = E;
    }
    ASSERT_NE(Lookup, nullptr);
    ASSERT_NE(Call, nullptr);
    EXPECT_EQ(Call->getString(field::Library), "libclock-model.so");
    EXPECT_EQ(Call->getString(field::PC), Lookup->getString(field::Result));
    EXPECT_EQ(Call->getString(field::Result),
              std::string(Name) == "time" ? "100000001" : "0");
  }
  EXPECT_EQ(Android->getArray(field::Memory)
                ->front()
                .getAsObject()
                ->getString(field::Bytes),
            "01000000010000007b0000000000000055f8060000000000010000000100000006"
            "120f0000000000");
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const int CLIStatus =
      runCLIRequest(Path, AndroidNativeAArch64, Request, Output);
  EXPECT_EQ(CLIStatus, process_cli::Success);
  ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Parsed));
#endif
}

TEST_F(ProcessPublic, AndroidMutexNamesMatchSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  Path =
      (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) / "mutex-O2-relr.so")
          .string();
  const std::string Request =
      R"({"backend":"unicorn","android":{"entry_symbol":"mutex_dynamic","initialize":false,"arguments":["0x20000000",0],"memory":[{"address":"0x20000000","size":4096}],"read_memory":[{"address":"0x20000000","size":24}],"libraries":{"libpthread-model.so":["pthread_mutex_lock","pthread_mutex_unlock"]}}})";
  auto Text = takeString(neverd_emulate_process_json(
      Session, Path.c_str(), AndroidNativeAArch64, Request.c_str()));
  ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
  auto Parsed = llvm::cantFail(llvm::json::parse(Text));
  EXPECT_EQ(Parsed.getAsObject()->getString(field::Stop), "returned");
  EXPECT_EQ(Parsed.getAsObject()->getString(field::ReturnValue), "0");
  const auto *Android = Parsed.getAsObject()->getObject(field::Android);
  ASSERT_NE(Android, nullptr);
  for (const char *Name : {"pthread_mutex_lock", "pthread_mutex_unlock"}) {
    const llvm::json::Object *Lookup = nullptr, *Call = nullptr;
    for (const auto &Event : *Android->getArray(field::NativeCalls)) {
      const auto *E = Event.getAsObject();
      if (E->getString(field::Name) == "dlsym" &&
          E->getString(field::Symbol) == Name)
        Lookup = E;
      if (E->getString(field::Name) == Name)
        Call = E;
    }
    ASSERT_NE(Lookup, nullptr);
    ASSERT_NE(Call, nullptr);
    EXPECT_EQ(Call->getString(field::Library), "libpthread-model.so");
    EXPECT_EQ(Call->getString(field::PC), Lookup->getString(field::Result));
    EXPECT_EQ(Call->getString(field::Result), "0");
  }
  EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const int CLIStatus =
      runCLIRequest(Path, AndroidNativeAArch64, Request, Output);
  EXPECT_EQ(CLIStatus, process_cli::Success);
  ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Parsed));
#endif
}

TEST_F(ProcessPublic, AndroidGuestThreadsMatchSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  Path =
      (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) / "threads-O2-relr.so")
          .string();
  const std::string Request =
      R"({"backend":"unicorn","instruction_quantum":7,"android":{"entry_symbol":"threads_dynamic","thread_limit":4,"initialize":false,"trace_limit":1000,"arguments":["0x20000000",0],"memory":[{"address":"0x20000000","size":4096}],"read_memory":[{"address":"0x20000000","size":64}],"libraries":{"libthread-model.so":["pthread_create","pthread_join"]}}})";
  auto Text = takeString(neverd_emulate_process_json(
      Session, Path.c_str(), AndroidNativeAArch64, Request.c_str()));
  ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
  auto Parsed = llvm::cantFail(llvm::json::parse(Text));
  EXPECT_EQ(Parsed.getAsObject()->getString(field::Stop), "returned");
  EXPECT_EQ(Parsed.getAsObject()->getString(field::ReturnValue), "4b");
  const auto *Android = Parsed.getAsObject()->getObject(field::Android);
  ASSERT_NE(Android, nullptr);
  const auto *Threads = Android->getArray(field::Threads);
  ASSERT_NE(Threads, nullptr);
  ASSERT_EQ(Threads->size(), 2u);
  EXPECT_EQ((*Threads)[1].getAsObject()->getBoolean(field::Finished), true);
  EXPECT_EQ((*Threads)[1].getAsObject()->getBoolean(field::Retired), true);
  ASSERT_NE(Android->getArray(field::TraceThreads), nullptr);
  EXPECT_GT(Android->getArray(field::TraceThreads)->size(), 1u);
  for (const auto &Event : *Android->getArray(field::NativeCalls)) {
    const auto *Call = Event.getAsObject();
    if (Call->getString(field::Name) == "pthread_create" ||
        Call->getString(field::Name) == "pthread_join") {
      EXPECT_EQ(Call->getString(field::Library), "libthread-model.so");
      EXPECT_EQ(Call->getInteger(field::ThreadID), 1000);
      EXPECT_EQ(Call->getString(field::Result), "0");
    }
  }
  EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const int CLIStatus =
      runCLIRequest(Path, AndroidNativeAArch64, Request, Output);
  EXPECT_EQ(CLIStatus, process_cli::Success);
  ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Parsed));
#endif
}

TEST_F(ProcessPublic, AndroidThreadAttributesMatchSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  Path = (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
          "thread-attributes-O2-relr.so")
             .string();
  const std::string Request =
      R"({"backend":"unicorn","android":{"entry_symbol":"thread_attribute_dynamic","initialize":false,"arguments":["0x20000000",0],"memory":[{"address":"0x20000000","size":4096}],"read_memory":[{"address":"0x20000000","size":80}],"libraries":{"libthread-model.so":["pthread_attr_init","pthread_attr_getstacksize"]}}})";
  auto Text = takeString(neverd_emulate_process_json(
      Session, Path.c_str(), AndroidNativeAArch64, Request.c_str()));
  ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
  auto Parsed = llvm::cantFail(llvm::json::parse(Text));
  EXPECT_EQ(Parsed.getAsObject()->getString(field::Stop), "returned");
  EXPECT_EQ(Parsed.getAsObject()->getString(field::ReturnValue), "0");
  const auto *Android = Parsed.getAsObject()->getObject(field::Android);
  ASSERT_NE(Android, nullptr);
  for (const char *Name : {"pthread_attr_init", "pthread_attr_getstacksize"}) {
    const llvm::json::Object *Lookup = nullptr, *Call = nullptr;
    for (const auto &Event : *Android->getArray(field::NativeCalls)) {
      const auto *E = Event.getAsObject();
      if (E->getString(field::Name) == "dlsym" &&
          E->getString(field::Symbol) == Name)
        Lookup = E;
      if (E->getString(field::Name) == Name)
        Call = E;
    }
    ASSERT_NE(Lookup, nullptr);
    ASSERT_NE(Call, nullptr);
    EXPECT_EQ(Call->getString(field::Library), "libthread-model.so");
    EXPECT_EQ(Call->getString(field::PC), Lookup->getString(field::Result));
    EXPECT_EQ(Call->getString(field::Result), "0");
  }
  const auto *Memory = Android->getArray(field::Memory);
  ASSERT_NE(Memory, nullptr);
  ASSERT_EQ(Memory->size(), 1u);
  auto Hex = Memory->front().getAsObject()->getString(field::Bytes);
  ASSERT_TRUE(Hex);
  EXPECT_EQ(Hex->substr(32, 16), "00c00f0000000000");
  EXPECT_EQ(Hex->substr(56, 8), "a5a5a5a5");
  EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const int CLIStatus =
      runCLIRequest(Path, AndroidNativeAArch64, Request, Output);
  EXPECT_EQ(CLIStatus, process_cli::Success);
  ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Parsed));
#endif
}

TEST_F(ProcessPublic, AndroidSyscallNamesMatchSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  Path =
      (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) / "syscall-O2-relr.so")
          .string();
  const std::string Request =
      R"({"backend":"unicorn","android":{"entry_symbol":"syscall_dynamic","initialize":false,"arguments":[0],"libraries":{"libservice.so":["syscall"]}}})";
  auto Text = takeString(neverd_emulate_process_json(
      Session, Path.c_str(), AndroidNativeAArch64, Request.c_str()));
  ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
  auto Parsed = llvm::cantFail(llvm::json::parse(Text));
  EXPECT_EQ(Parsed.getAsObject()->getString(field::Stop), "returned");
  EXPECT_EQ(Parsed.getAsObject()->getString(field::ReturnValue), "0");
  const auto *Android = Parsed.getAsObject()->getObject(field::Android);
  ASSERT_NE(Android, nullptr);
  const llvm::json::Object *Lookup = nullptr, *Call = nullptr;
  for (const auto &Event : *Android->getArray(field::NativeCalls)) {
    const auto *E = Event.getAsObject();
    if (E->getString(field::Name) == "dlsym" &&
        E->getString(field::Symbol) == "syscall")
      Lookup = E;
    if (E->getString(field::Name) == "syscall")
      Call = E;
  }
  ASSERT_NE(Lookup, nullptr);
  ASSERT_NE(Call, nullptr);
  EXPECT_EQ(Call->getString(field::Library), "libservice.so");
  EXPECT_EQ(Call->getString(field::PC), Lookup->getString(field::Result));
  EXPECT_EQ(Call->getString(field::Result), "3e8");
  EXPECT_EQ((*Call->getArray(field::Arguments))[0].getAsString(), "b2");
  EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const int CLIStatus =
      runCLIRequest(Path, AndroidNativeAArch64, Request, Output);
  EXPECT_EQ(CLIStatus, process_cli::Success);
  ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Parsed));
#endif
}

TEST_F(ProcessPublic, DarwinSymbolicLinksRejectMalformedOptionsBeforeLoading) {
  for (const char *Target : {"", "00", "61f", "zz"}) {
    const auto Request =
        std::string(
            R"({"darwin_files":{"files":[],"symbolic_links":[{"path":"/link","target_hex":")") +
        Target + R"("}]}})";
    for (const char *Profile :
         {MacOSMachO64, IOSMachO64, IOSSimulatorMachO64}) {
      EXPECT_EQ(neverd_emulate_process_json(Session,
                                            "missing-symbolic-link.macho",
                                            Profile, Request.c_str()),
                nullptr);
      const auto Error = takeString(neverd_last_error(Session));
      EXPECT_TRUE(Error.find("target_hex") != std::string::npos ||
                  Error.find("symbolic link targets") != std::string::npos)
          << Error;
      EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    }
  }
  for (const char *Profile : {LinuxELF64, WindowsPE64, AndroidNativeAArch64}) {
    EXPECT_EQ(
        neverd_emulate_process_json(
            Session, "missing-symbolic-link.macho", Profile,
            R"({"darwin_files":{"files":[],"symbolic_links":[{"path":"/link","target_hex":"61"}]}})"),
        nullptr);
    EXPECT_NE(
        takeString(neverd_last_error(Session)).find("darwin_files requires"),
        std::string::npos);
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  }
}

TEST_F(ProcessPublic, DarwinMixedLinksRejectMutableAncestorsBeforeLoading) {
  for (const char *Parent : {"/", "/static", "/static/implicit"}) {
    const auto Request =
        std::string(R"({"darwin_files":{"files":[],"directories":[{"path":")") +
        Parent +
        R"(","mutable":true}],"symbolic_links":[{"path":"/static/implicit/link","target_hex":"2f776f726b"}]}})";
    for (const char *Profile :
         {MacOSMachO64, IOSMachO64, IOSSimulatorMachO64}) {
      EXPECT_EQ(neverd_emulate_process_json(Session, "missing-mixed-link.macho",
                                            Profile, Request.c_str()),
                nullptr);
      EXPECT_NE(takeString(neverd_last_error(Session))
                    .find("mutable directories cannot contain"),
                std::string::npos);
      EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    }
  }
}

TEST_F(ProcessPublic, DarwinNiceRejectsMalformedOptionsBeforeLoading) {
  for (const char *Bad :
       {"null", "true", "false", "0.5", "[]", "{}", "21", "-21", "2147483648",
        "-2147483649", R"("1e1")", R"(" 0")", R"("0 ")", R"("-21")"}) {
    const auto Request =
        std::string(R"({"darwin_system":{"nice":)") + Bad + "}}";
    for (const char *Profile :
         {MacOSMachO64, IOSMachO64, IOSSimulatorMachO64}) {
      EXPECT_EQ(neverd_emulate_process_json(Session, "missing.macho", Profile,
                                            Request.c_str()),
                nullptr);
      EXPECT_NE(takeString(neverd_last_error(Session)).find("nice"),
                std::string::npos);
      EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    }
  }
  for (const char *Profile : {LinuxELF64, WindowsPE64, AndroidNativeAArch64}) {
    EXPECT_EQ(neverd_emulate_process_json(Session, "missing.macho", Profile,
                                          R"({"darwin_system":{"nice":-1}})"),
              nullptr);
    EXPECT_EQ(takeString(neverd_last_error(Session)),
              field::DarwinSystemProfile);
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  }
}

TEST_F(ProcessPublic, DarwinLoginBufferRejectsMalformedOptionsBeforeLoading) {
  for (const auto &Bad :
       {std::string("null"), std::string("true"), std::string("false"),
        std::string("255"), std::string("[]"), std::string("{}"),
        std::string("\"\""), "\"" + std::string(508, '0') + "\"",
        "\"" + std::string(509, '0') + "\"",
        "\"" + std::string(512, '0') + "\"",
        "\"" + std::string(509, '0') + "g\"",
        "\"" + std::string(509, '0') + "\\u0000\""}) {
    const auto Request =
        std::string(R"({"darwin_system":{"login_name_hex":)") + Bad + "}}";
    for (const char *Profile :
         {MacOSMachO64, IOSMachO64, IOSSimulatorMachO64}) {
      EXPECT_EQ(neverd_emulate_process_json(Session, "missing.macho", Profile,
                                            Request.c_str()),
                nullptr);
      EXPECT_NE(takeString(neverd_last_error(Session)).find("login_name_hex"),
                std::string::npos);
      EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    }
  }
  const auto Request = std::string(R"({"darwin_system":)") +
                       emulation::darwin_test::LoginNameJSON + "}";
  for (const char *Profile : {LinuxELF64, WindowsPE64, AndroidNativeAArch64}) {
    EXPECT_EQ(neverd_emulate_process_json(Session, "missing.macho", Profile,
                                          Request.c_str()),
              nullptr);
    EXPECT_EQ(takeString(neverd_last_error(Session)),
              field::DarwinSystemProfile);
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  }
}

TEST_F(ProcessPublic,
       DarwinProcessObservationsRejectMalformedOptionsBeforeLoading) {
  for (const char *Field :
       {"process_group_id", "session_id", "process_tainted"}) {
    const bool Boolean = llvm::StringRef(Field) == "process_tainted";
    for (const char *Bad : {"null", "0", "-1", "0.5", "[]", "{}", "2147483648",
                            R"("4294967296")", R"("true")"}) {
      const auto Request =
          std::string(R"({"darwin_system":{")") + Field + "\":" + Bad + "}}";
      for (const char *Profile :
           {MacOSMachO64, IOSMachO64, IOSSimulatorMachO64}) {
        EXPECT_EQ(neverd_emulate_process_json(Session, "missing.macho", Profile,
                                              Request.c_str()),
                  nullptr);
        EXPECT_NE(takeString(neverd_last_error(Session)).find(Field),
                  std::string::npos);
        EXPECT_EQ(neverd_session_is_loaded(Session), 0);
      }
    }
    if (Boolean)
      for (const char *Bad : {"1", R"("false")"}) {
        const auto Request =
            std::string(R"({"darwin_system":{"process_tainted":)") + Bad + "}}";
        EXPECT_EQ(neverd_emulate_process_json(Session, "missing.macho",
                                              MacOSMachO64, Request.c_str()),
                  nullptr);
        EXPECT_NE(takeString(neverd_last_error(Session)).find(Field),
                  std::string::npos);
      }
  }
  for (const char *Profile : {LinuxELF64, WindowsPE64, AndroidNativeAArch64}) {
    EXPECT_EQ(neverd_emulate_process_json(
                  Session, "missing.macho", Profile,
                  R"({"darwin_system":{"process_tainted":false}})"),
              nullptr);
    EXPECT_EQ(takeString(neverd_last_error(Session)),
              field::DarwinSystemProfile);
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  }
}

TEST_F(ProcessPublic, DarwinHostNameRejectsMalformedOptionsBeforeLoading) {
  for (const auto &Bad :
       {std::string("null"), std::string("true"), std::string("1"),
        std::string("[]"), std::string("{}"), std::string("\"x\\u0000y\""),
        "\"" + std::string(256, 'x') + "\""}) {
    auto Request = std::string(R"({"darwin_system":{"hostname":)") + Bad + "}}";
    EXPECT_EQ(neverd_emulate_process_json(Session, "missing.macho",
                                          MacOSMachO64, Request.c_str()),
              nullptr);
    EXPECT_NE(takeString(neverd_last_error(Session)).find("hostname"),
              std::string::npos);
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  }
  for (const char *Profile : {LinuxELF64, WindowsPE64, AndroidNativeAArch64}) {
    EXPECT_EQ(
        neverd_emulate_process_json(Session, "missing.macho", Profile,
                                    R"({"darwin_system":{"hostname":""}})"),
        nullptr);
    EXPECT_EQ(takeString(neverd_last_error(Session)),
              field::DarwinSystemProfile);
  }
}
TEST_F(ProcessPublic, DarwinDescriptorCapRejectsMalformedOptionsBeforeLoading) {
  for (const char *Bad :
       {"null", "true", "-1", "0.5", "2147483648", R"("4294967296")"}) {
    auto Request =
        std::string("{\"darwin_system\":{\"max_files_per_process\":") + Bad +
        "}}";
    EXPECT_EQ(neverd_emulate_process_json(Session, "missing.macho",
                                          MacOSMachO64, Request.c_str()),
              nullptr);
    EXPECT_NE(
        takeString(neverd_last_error(Session)).find("max_files_per_process"),
        std::string::npos);
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  }
  for (const char *Profile : {LinuxELF64, WindowsPE64, AndroidNativeAArch64}) {
    EXPECT_EQ(neverd_emulate_process_json(
                  Session, "missing.macho", Profile,
                  R"({"darwin_system":{"max_files_per_process":0}})"),
              nullptr);
    EXPECT_EQ(takeString(neverd_last_error(Session)),
              field::DarwinSystemProfile);
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  }
}

TEST_F(ProcessPublic, DexWireAllowanceReachesSharedValidation) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  Path = (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) / "jni-O2-relr.so")
             .string();
  const std::string Request =
      std::string(
          R"({"backend":"unicorn","android":{"entry_symbol":"jni_vm_lifecycle","initialize":false,"jni":true,"dex_images":[")") +
      std::string(80000, '0') + R"("]}})";
  ASSERT_GT(Request.size(), NEVERD_PROCESS_OPTIONS_JSON_METADATA_LIMIT);
  ASSERT_LT(Request.size(), NEVERD_PROCESS_OPTIONS_JSON_LIMIT);
  // Structurally admitted hex must reach the DEX validator;
  // the larger wire allowance must not accept a fabricated executable.
  EXPECT_EQ(neverd_emulate_process_json(Session, Path.c_str(),
                                        AndroidNativeAArch64, Request.c_str()),
            nullptr);
  EXPECT_NE(takeString(neverd_last_error(Session)).find("requires DEX version"),
            std::string::npos);
  const auto Padded = "{}" + std::string(65536, ' ');
  EXPECT_EQ(neverd_emulate_process_json(Session, Path.c_str(),
                                        AndroidNativeAArch64, Padded.c_str()),
            nullptr);
  EXPECT_EQ(takeString(neverd_last_error(Session)), field::TooLarge);
#endif
}

TEST_F(ProcessPublic, AndroidLargeFilePayloadsMatchSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const std::string Options = std::string(R"({"backend":"unicorn","android":{
        "entry_symbol":"jni_vm_lifecycle","initialize":false,"jni":true},
        "linux_files":{"files":[{"path":"/payload","bytes_hex":")") +
                              std::string(3u * 1024u * 1024u, 'a') + R"("}]}})";
  ASSERT_GT(Options.size(), 2112u * 1024u);
  for (const char *Variant : {"jni-O0-none", "jni-O2-relr"}) {
    SCOPED_TRACE(Variant);
    Path = (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
            (std::string(Variant) + ".so"))
               .string();
    auto Text = takeString(neverd_emulate_process_json(
        Session, Path.c_str(), AndroidNativeAArch64, Options.c_str()));
    ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
    auto Report = llvm::cantFail(llvm::json::parse(Text));
    ASSERT_EQ(Report.getAsObject()->getString(field::Stop), "returned") << Text;
    EXPECT_EQ(Report.getAsObject()->getString(field::ReturnValue), "0");
    EXPECT_EQ(runCLIRequest(Path, AndroidNativeAArch64, Options, Output),
              process_cli::Success);
    ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Report));
  }
#endif
}

TEST_F(ProcessPublic, LinuxFileWireAllowanceReachesSharedValidation) {
  // More than the former complete wire limit: admission must pass both the
  // C-string scan and the common parser before profile validation refuses it.
  const std::string Request =
      std::string(R"({"linux_files":{"files":[{"path":"/f","bytes_hex":")") +
      std::string(3u * 1024u * 1024u, 'a') + R"("}]}})";
  ASSERT_GT(Request.size(), 2112u * 1024u);
  ASSERT_LT(Request.size(), NEVERD_PROCESS_OPTIONS_JSON_LIMIT);
  EXPECT_EQ(neverd_emulate_process_json(Session, MissingPath, WindowsPE64,
                                        Request.c_str()),
            nullptr);
  EXPECT_EQ(takeString(neverd_last_error(Session)), field::LinuxFilesProfile);
  const std::string Padded = Request + std::string(65536, ' ');
  EXPECT_EQ(neverd_emulate_process_json(Session, MissingPath, WindowsPE64,
                                        Padded.c_str()),
            nullptr);
  EXPECT_EQ(takeString(neverd_last_error(Session)), field::TooLarge);
  EXPECT_EQ(neverd_session_is_loaded(Session), 0);
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

TEST_F(ProcessPublic, XAttrQueryImportMatchesSDKAndCLI) {
#ifndef NEVERD_PROCESS_FIXTURE_DIR
  GTEST_SKIP() << MissingTools;
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (const char *ISA : {"X64", "AArch64"}) {
    SCOPED_TRACE(ISA);
    for (const char *Opt : {"O0", "O2"}) {
      SCOPED_TRACE(Opt);
      Path = (std::filesystem::path(NEVERD_PROCESS_FIXTURE_DIR) /
              (std::string(ISA) + "-files-" + Opt + ".elf"))
                 .string();
      for (bool NameFirst : {false, true}) {
        SCOPED_TRACE(NameFirst);
        llvm::json::Object Request{
            {"backend", "unicorn"},
            {"arguments", llvm::json::Array{"files", NameFirst ? "Y" : "X"}},
            {"linux_files",
             llvm::json::Object{{"files", llvm::json::Array{llvm::json::Object{
                                              {"path", "/fixture/data"},
                                              {"bytes_hex", "00ff"}}}}}},
            {"linux_kernel",
             llvm::json::Object{
                 {"xattr_query_import",
                  NameFirst ? "name-before-target" : "target-before-name"}}}};
        const auto Options = jsonText(std::move(Request));
        auto Text = takeString(neverd_emulate_process_json(
            Session, Path.c_str(), LinuxELF64, Options.c_str()));
        ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
        auto Report = llvm::cantFail(llvm::json::parse(Text));
        EXPECT_EQ(Report.getAsObject()->getString(field::Stop), "exited");
        EXPECT_EQ(Report.getAsObject()->getInteger(field::ExitStatus), 0);
        EXPECT_EQ(runCLIRequest(Path, LinuxELF64, Options, Output),
                  process_cli::Success);
        ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, Report));
      }
    }
  }
#endif
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
    const int CLIStatus = runCLIRequest(Path, LinuxELF64, Options, Output);
    EXPECT_EQ(CLIStatus, process_cli::Success);
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
    const int CLIStatus = runCLIRequest(Path, LinuxELF64, Options, Output);
    EXPECT_EQ(CLIStatus, Mode == Normal ? process_cli::GuestFailure
                                        : process_cli::Incomplete);
    auto Buffer = llvm::MemoryBuffer::getFile(Output);
    ASSERT_TRUE(bool(Buffer));
    auto Report = llvm::json::parse((*Buffer)->getBuffer());
    ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError());
    EXPECT_EQ(*Report, run(Options));
  }
}
TEST_F(ProcessPublic, DarwinImmediatePollPreservesRawResultsAcrossCAndCLI) {
#ifndef NEVERD_DARWIN_FIXTURE_DIR
  GTEST_SKIP() << "Clang and ld64.lld Darwin fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (const auto &[File, Profile] :
       {std::pair{"macos-arm64", MacOSMachO64},
        std::pair{"macos-x86_64", MacOSMachO64},
        std::pair{"ios-arm64", IOSMachO64},
        std::pair{"ios-simulator-arm64", IOSSimulatorMachO64},
        std::pair{"ios-simulator-x86_64", IOSSimulatorMachO64}}) {
    Path = (std::filesystem::path(NEVERD_DARWIN_FIXTURE_DIR) / File).string();
    const bool X64 = llvm::StringRef(File).ends_with("x86_64");
    const uint64_t Class = X64 ? 0x2000000 : 0;
    for (bool Known : {true, false}) {
      const std::string Options =
          std::string(
              R"({"backend":"unicorn","instruction_quantum":1024,"timeout_microseconds":5000000,"arguments":["guest",")") +
          (Known ? "immediate-poll" : "immediate-poll-missing") +
          R"(","/data"],"darwin_files":{"files":[{"path":"/data","bytes_hex":"616200ff6566"}]},"darwin_system":)" +
          (Known
               ? R"({"resource_limits":[{"resource":8,"current":10240,"maximum":10240}]})"
               : "{}") +
          "}";
      auto Text = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), Profile, Options.c_str()));
      ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
      auto Report = llvm::json::parse(Text);
      ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError());
      const auto *Object = Report->getAsObject();
      ASSERT_NE(Object, nullptr);
      EXPECT_EQ(Object->getString(field::Stop),
                Known ? "exited" : "unsupported_service");
      EXPECT_EQ(Object->getString(field::Stdout), Known ? "70" : "21");
      EXPECT_EQ(Object->getString(field::Stderr), "");
      const auto *Calls = Object->getArray(field::Services);
      ASSERT_NE(Calls, nullptr);
      ASSERT_EQ(Calls->size(), Known ? 86u : 2u);
      if (Known) {
        EXPECT_EQ(Object->getInteger(field::ExitStatus), 37);
        constexpr uint64_t Values[] = {0, 0, 1, 0, 1, 1, 2, 2, 0, 2, 22, 14};
        constexpr uint64_t High[] = {0, 0x1234567800000000ULL,
                                     0xffffffff00000000ULL};
        for (unsigned A = 0; A != 2; ++A)
          for (unsigned H = 0; H != 3; ++H)
            for (unsigned I = 0; I != 12; ++I) {
              const auto *E = (*Calls)[7 + (A * 3 + H) * 12 + I].getAsObject();
              ASSERT_NE(E, nullptr);
              EXPECT_EQ(
                  E->getString(field::Number),
                  llvm::utohexstr(High[H] | Class | (A ? 417 : 230), true));
              EXPECT_EQ(E->getString(field::Result),
                        llvm::utohexstr(Values[I], true));
              EXPECT_EQ(E->getBoolean(field::Error), I >= 10);
            }
      } else {
        EXPECT_EQ(Object->getString(field::Diagnostic),
                  "Darwin poll NOFILE resource observation is not configured");
        const auto *E = Calls->back().getAsObject();
        ASSERT_NE(E, nullptr);
        EXPECT_EQ(E->getString(field::Number),
                  llvm::utohexstr(Class | 230, true));
        EXPECT_EQ(E->get(field::Error), nullptr);
        ASSERT_NE(E->get(field::Result), nullptr);
        EXPECT_EQ(*E->get(field::Result), llvm::json::Value(nullptr));
      }
      EXPECT_EQ(neverd_session_is_loaded(Session), 0);
      EXPECT_EQ(runCLIRequest(Path, Profile, Options, Output),
                Known ? process_cli::GuestFailure : process_cli::Incomplete);
      ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, *Report));
    }
  }
#endif
}

TEST_F(ProcessPublic, DarwinImmediateSelectPreservesRawResultsAcrossCAndCLI) {
#ifndef NEVERD_DARWIN_FIXTURE_DIR
  GTEST_SKIP() << "Clang and ld64.lld Darwin fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (const auto &[File, Profile] :
       {std::pair{"macos-arm64", MacOSMachO64},
        std::pair{"macos-x86_64", MacOSMachO64},
        std::pair{"ios-arm64", IOSMachO64},
        std::pair{"ios-simulator-arm64", IOSSimulatorMachO64},
        std::pair{"ios-simulator-x86_64", IOSSimulatorMachO64}}) {
    Path = (std::filesystem::path(NEVERD_DARWIN_FIXTURE_DIR) / File).string();
    const uint64_t Class =
        llvm::StringRef(File).ends_with("x86_64") ? 0x2000000 : 0;
    for (const char *Mode : {"immediate-select", "immediate-select-wait",
                             "immediate-select-capacity"}) {
      const bool Admitted = llvm::StringRef(Mode) == "immediate-select";
      const bool Capacity =
          llvm::StringRef(Mode) == "immediate-select-capacity";
      const std::string Options =
          std::string(
              R"({"backend":"unicorn","instruction_quantum":1024,"timeout_microseconds":5000000,"arguments":["guest",")") +
          Mode +
          R"(","/data"],"darwin_files":{"files":[{"path":"/data","bytes_hex":"616200ff6566"}]}})";
      auto Text = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), Profile, Options.c_str()));
      ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
      auto Report = llvm::json::parse(Text);
      ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError());
      const auto *Object = Report->getAsObject();
      ASSERT_NE(Object, nullptr);
      EXPECT_EQ(Object->getString(field::Stop),
                Admitted ? "exited" : "unsupported_service");
      EXPECT_EQ(Object->getString(field::Stdout), Admitted ? "73" : "21");
      EXPECT_EQ(Object->getString(field::Stderr), "");
      const auto *Calls = Object->getArray(field::Services);
      ASSERT_NE(Calls, nullptr);
      ASSERT_EQ(Calls->size(), Admitted ? 115u : 2u);
      if (Admitted) {
        EXPECT_EQ(Object->getInteger(field::ExitStatus), 37);
        constexpr uint64_t Values[] = {0,  3,  6,  3,  6,  9,  9, 12,
                                       22, 14, 22, 22, 14, 14, 9};
        constexpr uint64_t High[] = {0, 0x1234567800000000ULL,
                                     0xffffffff00000000ULL};
        for (unsigned A = 0; A != 2; ++A)
          for (unsigned H = 0; H != 3; ++H)
            for (unsigned I = 0; I != 15; ++I) {
              const auto *E = (*Calls)[9 + (A * 3 + H) * 15 + I].getAsObject();
              ASSERT_NE(E, nullptr);
              EXPECT_EQ(
                  E->getString(field::Number),
                  llvm::utohexstr(High[H] | Class | (A ? 407 : 93), true));
              EXPECT_EQ(E->getString(field::Result),
                        llvm::utohexstr(Values[I], true));
              EXPECT_EQ(E->getBoolean(field::Error), I >= 8);
            }
        for (const auto &[Index, Value] :
             {std::pair{99u, 6u}, std::pair{100u, 4u}, std::pair{106u, 3u},
              std::pair{107u, 0u}, std::pair{108u, 6u}}) {
          const auto *E = (*Calls)[Index].getAsObject();
          ASSERT_NE(E, nullptr);
          EXPECT_EQ(E->getString(field::Result), llvm::utohexstr(Value, true));
          EXPECT_EQ(E->getBoolean(field::Error), false);
        }
      } else {
        EXPECT_EQ(Object->getString(field::Diagnostic),
                  Capacity
                      ? "Darwin select descriptor table capacity is not modeled"
                      : "Darwin select waiting is not implemented");
        const auto *E = Calls->back().getAsObject();
        ASSERT_NE(E, nullptr);
        EXPECT_EQ(E->getString(field::Number),
                  llvm::utohexstr(Class | (Capacity ? 407 : 93), true));
        EXPECT_EQ(E->get(field::Error), nullptr);
        ASSERT_NE(E->get(field::Result), nullptr);
        EXPECT_EQ(*E->get(field::Result), llvm::json::Value(nullptr));
      }
      EXPECT_EQ(neverd_session_is_loaded(Session), 0);
      EXPECT_EQ(runCLIRequest(Path, Profile, Options, Output),
                Admitted ? process_cli::GuestFailure : process_cli::Incomplete);
      ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, *Report));
    }
  }
#endif
}

TEST_F(ProcessPublic,
       DarwinImmediatePselectPreservesTimespecAndMaskScopeAcrossCAndCLI) {
#ifndef NEVERD_DARWIN_FIXTURE_DIR
  GTEST_SKIP() << "Clang and ld64.lld Darwin fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (const auto &[File, Profile] :
       {std::pair{"macos-arm64", MacOSMachO64},
        std::pair{"macos-x86_64", MacOSMachO64},
        std::pair{"ios-arm64", IOSMachO64},
        std::pair{"ios-simulator-arm64", IOSSimulatorMachO64},
        std::pair{"ios-simulator-x86_64", IOSSimulatorMachO64}}) {
    Path = (std::filesystem::path(NEVERD_DARWIN_FIXTURE_DIR) / File).string();
    const uint64_t Class =
        llvm::StringRef(File).ends_with("x86_64") ? 0x2000000 : 0;
    for (const char *Mode :
         {"immediate-pselect", "immediate-pselect-wait",
          "immediate-pselect-capacity", "immediate-pselect-mask"}) {
      const bool Admitted = llvm::StringRef(Mode) == "immediate-pselect";
      const bool Capacity =
          llvm::StringRef(Mode) == "immediate-pselect-capacity";
      const bool Mask = llvm::StringRef(Mode) == "immediate-pselect-mask";
      const std::string Options =
          std::string(
              R"({"backend":"unicorn","instruction_quantum":1024,"timeout_microseconds":5000000,"arguments":["guest",")") +
          Mode +
          R"(","/data"],"darwin_files":{"files":[{"path":"/data","bytes_hex":"616200ff6566"}]}})";
      auto Text = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), Profile, Options.c_str()));
      ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
      auto Report = llvm::json::parse(Text);
      ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError());
      const auto *Object = Report->getAsObject();
      ASSERT_NE(Object, nullptr);
      EXPECT_EQ(Object->getString(field::Stop),
                Admitted ? "exited" : "unsupported_service");
      EXPECT_EQ(Object->getString(field::Stdout), Admitted ? "71" : "21");
      EXPECT_EQ(Object->getString(field::Stderr), "");
      const auto *Calls = Object->getArray(field::Services);
      ASSERT_NE(Calls, nullptr);
      ASSERT_EQ(Calls->size(), Admitted ? 139u : 2u);
      if (Admitted) {
        EXPECT_EQ(Object->getInteger(field::ExitStatus), 37);
        constexpr uint64_t Values[] = {0,  3,  6,  3,  6,  9,  9,  12, 22, 14,
                                       22, 22, 22, 22, 22, 14, 14, 14, 9};
        constexpr uint64_t High[] = {0, 0x1234567800000000ULL,
                                     0xffffffff00000000ULL};
        for (unsigned A = 0; A != 2; ++A)
          for (unsigned H = 0; H != 3; ++H)
            for (unsigned I = 0; I != 19; ++I) {
              const auto *E = (*Calls)[9 + (A * 3 + H) * 19 + I].getAsObject();
              ASSERT_NE(E, nullptr);
              EXPECT_EQ(
                  E->getString(field::Number),
                  llvm::utohexstr(High[H] | Class | (A ? 395 : 394), true));
              EXPECT_EQ(E->getString(field::Result),
                        llvm::utohexstr(Values[I], true));
              EXPECT_EQ(E->getBoolean(field::Error), I >= 8);
            }
        for (const auto &[Index, Value] :
             {std::pair{123u, 6u}, std::pair{124u, 4u}, std::pair{130u, 3u},
              std::pair{131u, 0u}, std::pair{132u, 6u}}) {
          const auto *E = (*Calls)[Index].getAsObject();
          ASSERT_NE(E, nullptr);
          EXPECT_EQ(E->getString(field::Result), llvm::utohexstr(Value, true));
          EXPECT_EQ(E->getBoolean(field::Error), false);
        }
      } else {
        EXPECT_EQ(Object->getString(field::Diagnostic),
                  Capacity
                      ? "Darwin select descriptor table capacity is not modeled"
                  : Mask ? "Darwin pselect signal-mask state is not implemented"
                         : "Darwin pselect waiting is not implemented");
        const auto *E = Calls->back().getAsObject();
        ASSERT_NE(E, nullptr);
        EXPECT_EQ(E->getString(field::Number),
                  llvm::utohexstr(Class | (Capacity ? 395 : 394), true));
        EXPECT_EQ(E->get(field::Error), nullptr);
        ASSERT_NE(E->get(field::Result), nullptr);
        EXPECT_EQ(*E->get(field::Result), llvm::json::Value(nullptr));
      }
      EXPECT_EQ(neverd_session_is_loaded(Session), 0);
      EXPECT_EQ(runCLIRequest(Path, Profile, Options, Output),
                Admitted ? process_cli::GuestFailure : process_cli::Incomplete);
      ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, *Report));
    }
  }
#endif
}

TEST_F(ProcessPublic, DarwinOrdinaryModesPreserveStrictAuthorityAcrossCAndCLI) {
#ifndef NEVERD_DARWIN_FIXTURE_DIR
  GTEST_SKIP() << "Clang and ld64.lld Darwin fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (const auto &[File, Profile] :
       {std::pair{"macos-arm64", MacOSMachO64},
        std::pair{"macos-x86_64", MacOSMachO64},
        std::pair{"ios-arm64", IOSMachO64},
        std::pair{"ios-simulator-arm64", IOSSimulatorMachO64},
        std::pair{"ios-simulator-x86_64", IOSSimulatorMachO64}}) {
    Path = (std::filesystem::path(NEVERD_DARWIN_FIXTURE_DIR) / File).string();
    const uint64_t Class =
        llvm::StringRef(File).ends_with("x86_64") ? 0x2000000 : 0;
    for (bool Granted : {true, false}) {
      llvm::json::Value Input = llvm::json::Object{
          {"backend", "unicorn"},
          {"instruction_quantum", 1024},
          {"timeout_microseconds", 5000000},
          {"arguments",
           llvm::json::Array{"guest", "ordinary-mode-mutations", "/data"}},
          {"darwin_files", llvm::cantFail(llvm::json::parse(
                               emulation::darwin_test::ModeFilesJSON))},
          {"darwin_system", llvm::cantFail(llvm::json::parse(
                                emulation::darwin_test::ModeCredentialsJSON))}};
      if (!Granted)
        (*Input.getAsObject()
              ->getObject(field::DarwinFiles)
              ->getObject(field::FileModeMutations))[field::FileModeObjects] =
            llvm::json::Array{};
      const auto Options = llvm::formatv("{0}", Input).str();
      auto Text = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), Profile, Options.c_str()));
      ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
      auto Report = llvm::json::parse(Text);
      ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError());
      const auto *Object = Report->getAsObject();
      ASSERT_NE(Object, nullptr);
      EXPECT_EQ(Object->getString(field::Stop),
                Granted ? "exited" : "unsupported_service");
      EXPECT_EQ(Object->getString(field::Stdout), Granted ? "4d" : "");
      EXPECT_EQ(Object->getString(field::Stderr), "");
      const auto *Calls = Object->getArray(field::Services);
      ASSERT_NE(Calls, nullptr);
      ASSERT_EQ(Calls->size(), Granted ? 54u : 5u);
      if (Granted) {
        EXPECT_EQ(Object->getInteger(field::ExitStatus), 37);
        constexpr uint64_t High[] = {0, 0x1234567800000000ULL,
                                     0xffffffff00000000ULL};
        constexpr uint64_t Numbers[] = {15, 124, 467};
        for (unsigned API = 0; API != 3; ++API)
          for (unsigned H = 0; H != 3; ++H) {
            const auto *E = (*Calls)[4 + (API * 3 + H) * 4].getAsObject();
            ASSERT_NE(E, nullptr);
            EXPECT_EQ(E->getString(field::Number),
                      llvm::utohexstr(Class | High[H] | Numbers[API], true));
            EXPECT_EQ(E->getString(field::Result), "0");
            EXPECT_EQ(E->getBoolean(field::Error), false);
          }
        constexpr uint64_t Tail[] = {14, 9, 22, 9};
        for (unsigned I = 0; I != 4; ++I) {
          const auto *E = (*Calls)[40 + I].getAsObject();
          ASSERT_NE(E, nullptr);
          EXPECT_EQ(E->getString(field::Result),
                    llvm::utohexstr(Tail[I], true));
          EXPECT_EQ(E->getBoolean(field::Error), true);
        }
      } else {
        EXPECT_EQ(Object->getString(field::Diagnostic),
                  "Darwin object mode provider is not configured");
        const auto *E = Calls->back().getAsObject();
        ASSERT_NE(E, nullptr);
        EXPECT_EQ(E->getString(field::Number),
                  llvm::utohexstr(Class | 15, true));
        EXPECT_EQ(*E->get(field::Result), llvm::json::Value(nullptr));
        EXPECT_EQ(E->get(field::Error), nullptr);
      }
      EXPECT_EQ(neverd_session_is_loaded(Session), 0);
      EXPECT_EQ(runCLIRequest(Path, Profile, Options, Output),
                Granted ? process_cli::GuestFailure : process_cli::Incomplete);
      ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, *Report));
    }
  }
  for (
      const char *Modes :
      {R"({"objects":[],"extra":true})", R"({"objects":"unknown"})",
       R"({"objects":[{"path":"/data","time":{"seconds":71,"nanoseconds":72},"extra":0}]})",
       R"({"objects":[{"path":"/data","time":{"seconds":1.5,"nanoseconds":72}}]})",
       R"({"objects":[{"path":"/data","time":{"seconds":71,"nanoseconds":1000000000}}]})",
       R"({"objects":[{"path":"/data","time":{"seconds":71,"nanoseconds":72}},{"path":"/data","time":{"seconds":71,"nanoseconds":72}}]})"}) {
    auto Input = llvm::cantFail(
        llvm::json::parse(std::string(R"({"darwin_files":)") +
                          emulation::darwin_test::ModeFilesJSON + "}"));
    (*Input.getAsObject()->getObject(
        field::DarwinFiles))[field::FileModeMutations] =
        llvm::cantFail(llvm::json::parse(Modes));
    const auto Options = llvm::formatv("{0}", Input).str();
    EXPECT_TRUE(takeString(neverd_emulate_process_json(Session, Path.c_str(),
                                                       IOSSimulatorMachO64,
                                                       Options.c_str()))
                    .empty());
    EXPECT_FALSE(takeString(neverd_last_error(Session)).empty());
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  }
#endif
}

TEST_F(ProcessPublic,
       DarwinOrdinaryOwnershipPreserveStrictAuthorityAcrossCAndCLI) {
#ifndef NEVERD_DARWIN_FIXTURE_DIR
  GTEST_SKIP() << "Clang and ld64.lld Darwin fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (const auto &[File, Profile] :
       {std::pair{"macos-arm64", MacOSMachO64},
        std::pair{"macos-x86_64", MacOSMachO64},
        std::pair{"ios-arm64", IOSMachO64},
        std::pair{"ios-simulator-arm64", IOSSimulatorMachO64},
        std::pair{"ios-simulator-x86_64", IOSSimulatorMachO64}}) {
    Path = (std::filesystem::path(NEVERD_DARWIN_FIXTURE_DIR) / File).string();
    const uint64_t Class =
        llvm::StringRef(File).ends_with("x86_64") ? 0x2000000 : 0;
    for (bool Granted : {true, false}) {
      llvm::json::Value Input = llvm::json::Object{
          {"backend", "unicorn"},
          {"instruction_quantum", 1024},
          {"timeout_microseconds", 5000000},
          {"arguments",
           llvm::json::Array{"guest", "ordinary-ownership-mutations", "/data"}},
          {"darwin_files", llvm::cantFail(llvm::json::parse(
                               emulation::darwin_test::OwnershipFilesJSON))},
          {"darwin_system",
           llvm::cantFail(llvm::json::parse(
               emulation::darwin_test::OwnershipCredentialsJSON))}};
      if (!Granted)
        (*Input.getAsObject()
              ->getObject(field::DarwinFiles)
              ->getObject(
                  field::FileOwnershipMutations))[field::FileOwnershipObjects] =
            llvm::json::Array{};
      const auto Options = llvm::formatv("{0}", Input).str();
      auto Text = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), Profile, Options.c_str()));
      ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
      auto Report = llvm::json::parse(Text);
      ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError());
      const auto *Object = Report->getAsObject();
      ASSERT_NE(Object, nullptr);
      EXPECT_EQ(Object->getString(field::Stop),
                Granted ? "exited" : "unsupported_service");
      EXPECT_EQ(Object->getString(field::Stdout), Granted ? "4f" : "");
      EXPECT_EQ(Object->getString(field::Stderr), "");
      const auto *Calls = Object->getArray(field::Services);
      ASSERT_NE(Calls, nullptr);
      ASSERT_EQ(Calls->size(), Granted ? 77u : 7u);
      if (Granted) {
        EXPECT_EQ(Object->getInteger(field::ExitStatus), 38);
        constexpr uint64_t High[] = {0, 0x1234567800000000ULL,
                                     0xffffffff00000000ULL};
        constexpr uint64_t Numbers[] = {16, 123, 468};
        for (unsigned API = 0; API != 3; ++API)
          for (unsigned H = 0; H != 3; ++H) {
            const auto *E = (*Calls)[6 + (API * 3 + H) * 6].getAsObject();
            ASSERT_NE(E, nullptr);
            EXPECT_EQ(E->getString(field::Number),
                      llvm::utohexstr(Class | High[H] | Numbers[API], true));
            EXPECT_EQ(E->getString(field::Result), "0");
            EXPECT_EQ(E->getBoolean(field::Error), false);
          }
        constexpr uint64_t Tail[] = {14, 9, 22, 22, 9};
        for (unsigned I = 0; I != 5; ++I) {
          const auto *E = (*Calls)[60 + I].getAsObject();
          ASSERT_NE(E, nullptr);
          EXPECT_EQ(E->getString(field::Result),
                    llvm::utohexstr(Tail[I], true));
          EXPECT_EQ(E->getBoolean(field::Error), true);
        }
      } else {
        EXPECT_EQ(Object->getString(field::Diagnostic),
                  "Darwin object ownership provider is not configured");
        const auto *E = Calls->back().getAsObject();
        ASSERT_NE(E, nullptr);
        EXPECT_EQ(E->getString(field::Number),
                  llvm::utohexstr(Class | 16, true));
        EXPECT_EQ(*E->get(field::Result), llvm::json::Value(nullptr));
        EXPECT_EQ(E->get(field::Error), nullptr);
      }
      EXPECT_EQ(neverd_session_is_loaded(Session), 0);
      EXPECT_EQ(runCLIRequest(Path, Profile, Options, Output),
                Granted ? process_cli::GuestFailure : process_cli::Incomplete);
      ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, *Report));
    }
  }
  for (
      const char *Owners :
      {R"({"objects":[],"extra":true})", R"({"objects":"unknown"})",
       R"({"objects":[{"path":"/data","time":{"seconds":71,"nanoseconds":72},"extra":0}]})",
       R"({"objects":[{"path":"/data","time":{"seconds":1.5,"nanoseconds":72}}]})",
       R"({"objects":[{"path":"/data","time":{"seconds":71,"nanoseconds":1000000000}}]})",
       R"({"objects":[{"path":"/data","time":{"seconds":71,"nanoseconds":72}},{"path":"/data","time":{"seconds":71,"nanoseconds":72}}]})"}) {
    auto Input = llvm::cantFail(
        llvm::json::parse(std::string(R"({"darwin_files":)") +
                          emulation::darwin_test::OwnershipFilesJSON + "}"));
    (*Input.getAsObject()->getObject(
        field::DarwinFiles))[field::FileOwnershipMutations] =
        llvm::cantFail(llvm::json::parse(Owners));
    const auto Options = llvm::formatv("{0}", Input).str();
    EXPECT_TRUE(takeString(neverd_emulate_process_json(Session, Path.c_str(),
                                                       IOSSimulatorMachO64,
                                                       Options.c_str()))
                    .empty());
    EXPECT_FALSE(takeString(neverd_last_error(Session)).empty());
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  }
#endif
}

TEST_F(ProcessPublic,
       DarwinOrdinaryAdvisoryLocksPreserveProviderAcrossCAndCLI) {
#ifndef NEVERD_DARWIN_FIXTURE_DIR
  GTEST_SKIP() << "Clang and ld64.lld Darwin fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (const auto &[File, Profile] :
       {std::pair{"macos-arm64", MacOSMachO64},
        std::pair{"macos-x86_64", MacOSMachO64},
        std::pair{"ios-arm64", IOSMachO64},
        std::pair{"ios-simulator-arm64", IOSSimulatorMachO64},
        std::pair{"ios-simulator-x86_64", IOSSimulatorMachO64}}) {
    Path = (std::filesystem::path(NEVERD_DARWIN_FIXTURE_DIR) / File).string();
    const uint64_t Class =
        llvm::StringRef(File).ends_with("x86_64") ? 0x2000000 : 0;
    for (bool Granted : {true, false}) {
      llvm::json::Value Input = llvm::json::Object{
          {"backend", "unicorn"},
          {"instruction_quantum", 1024},
          {"timeout_microseconds", 5000000},
          {"arguments",
           llvm::json::Array{"guest", "ordinary-advisory-locks", "/data"}},
          {"darwin_files",
           llvm::cantFail(llvm::json::parse(
               emulation::darwin_test::AdvisoryLockFilesJSON))}};
      if (!Granted)
        (*Input.getAsObject()
              ->getObject(field::DarwinFiles)
              ->getObject(
                  field::FileAdvisoryLocks))[field::FileAdvisoryLockObjects] =
            llvm::json::Array{};
      const auto Options = llvm::formatv("{0}", Input).str();
      auto Text = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), Profile, Options.c_str()));
      ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
      auto Report = llvm::json::parse(Text);
      ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError());
      const auto *Object = Report->getAsObject();
      ASSERT_NE(Object, nullptr);
      EXPECT_EQ(Object->getString(field::Stop),
                Granted ? "exited" : "unsupported_service");
      EXPECT_EQ(Object->getString(field::Stdout), Granted ? "4b" : "");
      EXPECT_EQ(Object->getString(field::Stderr), "");
      const auto *Calls = Object->getArray(field::Services);
      ASSERT_NE(Calls, nullptr);
      ASSERT_EQ(Calls->size(), Granted ? 77u : 6u);
      if (Granted) {
        EXPECT_EQ(Object->getInteger(field::ExitStatus), 39);
        constexpr uint64_t High[] = {0, 0x1234567800000000ULL,
                                     0xffffffff00000000ULL};
        constexpr uint64_t FDs[] = {3, 5, 4, 5, 4, 3, 3, 4, 4, 3, 4, 4},
                           Actions[] = {2, 6, 5, 8, 5, 5, 6, 8, 6, 8, 6, 8},
                           Values[] = {0, 0, 35, 0, 0, 0, 35, 0, 35, 0, 0, 0};
        for (unsigned H = 0; H != 3; ++H)
          for (unsigned I = 0; I != 12; ++I) {
            const auto *E = (*Calls)[5 + H * 17 + I].getAsObject();
            ASSERT_NE(E, nullptr);
            EXPECT_EQ(E->getString(field::Number),
                      llvm::utohexstr(Class | High[H] | 131, true));
            EXPECT_EQ(E->getString(field::Result),
                      llvm::utohexstr(Values[I], true));
            EXPECT_EQ(E->getBoolean(field::Error), bool(Values[I]));
            const auto *A = E->getArray(field::Arguments);
            ASSERT_NE(A, nullptr);
            ASSERT_EQ(A->size(), 6u);
            EXPECT_EQ((*A)[0].getAsString(),
                      llvm::utohexstr(High[H] | FDs[I], true));
            EXPECT_EQ((*A)[1].getAsString(),
                      llvm::utohexstr(High[H] | Actions[I], true));
          }
        constexpr uint64_t ValuesTail[] = {9, 9, 9, 0, 0, 0, 35, 0, 9};
        for (unsigned I = 0; I != 9; ++I) {
          const auto *E = (*Calls)[56 + I].getAsObject();
          ASSERT_NE(E, nullptr);
          EXPECT_EQ(E->getString(field::Result),
                    llvm::utohexstr(ValuesTail[I], true));
          EXPECT_EQ(E->getBoolean(field::Error), bool(ValuesTail[I]));
        }
      } else {
        EXPECT_EQ(
            Object->getString(field::Diagnostic),
            "Darwin regular-object advisory lock provider is not configured");
        const auto *E = Calls->back().getAsObject();
        ASSERT_NE(E, nullptr);
        EXPECT_EQ(E->getString(field::Number),
                  llvm::utohexstr(Class | 131, true));
        EXPECT_EQ(*E->get(field::Result), llvm::json::Value(nullptr));
        EXPECT_EQ(E->get(field::Error), nullptr);
      }
      EXPECT_EQ(neverd_session_is_loaded(Session), 0);
      EXPECT_EQ(runCLIRequest(Path, Profile, Options, Output),
                Granted ? process_cli::GuestFailure : process_cli::Incomplete);
      ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, *Report));
    }
  }
  for (const char *Locks :
       {R"({})", R"({"objects":[],"extra":true})", R"({"objects":"unknown"})",
        R"({"objects":[3]})", R"({"objects":[{"path":"/data"}]})",
        R"({"objects":["/data","/data"]})", R"({"objects":["relative"]})",
        R"({"objects":["/missing"]})"}) {
    auto Input = llvm::cantFail(
        llvm::json::parse(std::string(R"({"darwin_files":)") +
                          emulation::darwin_test::AdvisoryLockFilesJSON + "}"));
    (*Input.getAsObject()->getObject(
        field::DarwinFiles))[field::FileAdvisoryLocks] =
        llvm::cantFail(llvm::json::parse(Locks));
    const auto Options = llvm::formatv("{0}", Input).str();
    EXPECT_TRUE(takeString(neverd_emulate_process_json(Session, Path.c_str(),
                                                       IOSSimulatorMachO64,
                                                       Options.c_str()))
                    .empty());
    EXPECT_FALSE(takeString(neverd_last_error(Session)).empty());
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  }
#endif
}

TEST_F(ProcessPublic,
       DarwinFileSystemSnapshotsKeepIndependentProvidersAcrossCAndCLI) {
#ifndef NEVERD_DARWIN_FIXTURE_DIR
  GTEST_SKIP() << "Clang and ld64.lld Darwin fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const auto Common = emulation::darwin_test::fileSystemServices(),
             Virtual = emulation::darwin_test::createdFileSystemServices();
  const auto Bytes = emulation::darwin_test::createdFileSystemRecords();
  const std::string Literal(reinterpret_cast<const char *>(Bytes.data()),
                            Bytes.size());
  for (const auto &[File, Profile] :
       {std::pair{"macos-arm64", MacOSMachO64},
        std::pair{"macos-x86_64", MacOSMachO64},
        std::pair{"ios-arm64", IOSMachO64},
        std::pair{"ios-simulator-arm64", IOSSimulatorMachO64},
        std::pair{"ios-simulator-x86_64", IOSSimulatorMachO64}}) {
    Path = (std::filesystem::path(NEVERD_DARWIN_FIXTURE_DIR) / File).string();
    const uint64_t Class =
        llvm::StringRef(File).ends_with("x86_64") ? 0x2000000 : 0;
    for (bool Created : {false, true})
      for (unsigned Variant = 0; Variant != (Created ? 4u : 3u); ++Variant) {
        auto Files = llvm::cantFail(
            llvm::json::parse(emulation::darwin_test::FileSystemFilesJSON));
        auto &F = *Files.getAsObject();
        if (Created) {
          F["umask"] = 18;
          auto &Parent = *(*F.getArray("directories"))[0].getAsObject();
          Parent["mutable"] = true;
          Parent["mutation_policy"] = llvm::json::Object{
              {"directory_entry_size", 16},
              {"mutation_time",
               llvm::json::Object{{"seconds", 91}, {"nanoseconds", 92}}}};
          F["creation_policy"] = llvm::cantFail(llvm::json::parse(
              R"({"first_inode":100,"block_size":512,"generation":9,"creation_time":{"seconds":41,"nanoseconds":42},"mutation_policy":{"allocation_unit":512,"mutation_time":{"seconds":101,"nanoseconds":102}}})"));
          (*F.getObject("filesystem_observations"))["created_regular_objects"] =
              true;
        }
        if (Variant == 1)
          F.erase("filesystem_observations");
        if (Variant == 2)
          F["filesystem_observations"] =
              llvm::json::Object{{"objects", llvm::json::Array{}}};
        llvm::json::Value Input = llvm::json::Object{
            {"backend", "unicorn"},
            {"instruction_quantum", 1024},
            {"timeout_microseconds", 5000000},
            {"arguments",
             llvm::json::Array{"guest",
                               Created ? "virtual-filesystem-creation"
                                       : "filesystem-observations",
                               "/data"}},
            {"darwin_system",
             llvm::cantFail(llvm::json::parse(
                 emulation::darwin_test::ModeCredentialsJSON))},
            {"darwin_files", std::move(Files)}};
        if (Variant == 3)
          (*Input.getAsObject()
                ->getObject("darwin_files")
                ->getObject(
                    "filesystem_observations"))["created_regular_objects"] =
              false;
        const auto Options = llvm::formatv("{0}", Input).str();
        auto Text = takeString(neverd_emulate_process_json(
            Session, Path.c_str(), Profile, Options.c_str()));
        ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
        auto Report = llvm::json::parse(Text);
        ASSERT_TRUE(bool(Report));
        const auto *O = Report->getAsObject();
        ASSERT_NE(O, nullptr);
        EXPECT_EQ(O->getString(field::Stop),
                  Variant ? "unsupported_service" : "exited");
        EXPECT_EQ(O->getString(field::Stderr), "");
        const auto *Calls = O->getArray(field::Services);
        ASSERT_NE(Calls, nullptr);
        if (!Variant) {
          const auto &Wanted = Created ? Virtual : Common;
          ASSERT_EQ(Calls->size(), Wanted.size());
          EXPECT_EQ(O->getInteger(field::ExitStatus), Created ? 51 : 50);
          EXPECT_EQ(O->getString(field::Stdout),
                    Created ? llvm::toHex(Literal, true) : "57");
          for (unsigned I = 0; I != Wanted.size(); ++I) {
            const auto *E = (*Calls)[I].getAsObject();
            ASSERT_NE(E, nullptr);
            const auto &W = Wanted[I];
            EXPECT_EQ(E->getString(field::Number),
                      llvm::utohexstr(Class | W.Number, true));
            EXPECT_EQ(E->getString(field::Result),
                      llvm::utohexstr(W.Value, true));
            EXPECT_EQ(E->getBoolean(field::Error), W.Error);
            const auto *A = E->getArray(field::Arguments);
            ASSERT_NE(A, nullptr);
            ASSERT_EQ(A->size(), 6u);
            if (W.Object)
              EXPECT_EQ((*A)[0].getAsString(),
                        llvm::utohexstr(*W.Object, true));
            if (W.Output)
              EXPECT_EQ((*A)[1].getAsString(),
                        llvm::utohexstr(*W.Output, true));
            const auto N = W.Number & 0xffffff;
            if (N == 345 || N == 346)
              for (unsigned J = 2; J != 6; ++J)
                EXPECT_EQ((*A)[J].getAsString(), "ffffffffffffffff");
          }
        } else {
          ASSERT_EQ(Calls->size(), Created ? 2u : 8u);
          const auto *Last = Calls->back().getAsObject();
          ASSERT_NE(Last, nullptr);
          EXPECT_EQ(*Last->get(field::Result), llvm::json::Value(nullptr));
          EXPECT_EQ(Last->get(field::Error), nullptr);
          EXPECT_EQ(O->getString(field::Diagnostic),
                    Variant == 1 ? "Darwin filesystem observation environment "
                                   "is not configured"
                                 : "Darwin object filesystem snapshot is not "
                                   "configured");
        }
        EXPECT_EQ(neverd_session_is_loaded(Session), 0);
        EXPECT_EQ(runCLIRequest(Path, Profile, Options, Output),
                  Variant ? process_cli::Incomplete
                          : process_cli::GuestFailure);
        ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, *Report));
      }
  }
#endif
}

class DarwinSymbolicFileSystemPublic
    : public ProcessPublic,
      public testing::WithParamInterface<
          std::pair<const char *, const char *>> {};
TEST_P(DarwinSymbolicFileSystemPublic,
       PreserveOwnAndFollowedSnapshotsAcrossCAndCLI) {
#ifndef NEVERD_DARWIN_FIXTURE_DIR
  GTEST_SKIP() << "Clang and ld64.lld Darwin fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const auto &[File, Profile] = GetParam();
  Path = (std::filesystem::path(NEVERD_DARWIN_FIXTURE_DIR) / File).string();
  const uint64_t Class =
      llvm::StringRef(File).ends_with("x86_64") ? 0x2000000 : 0;
  const char *Modes[] = {"symbolic-filesystem-observations",
                         "created-symbolic-filesystem",
                         "virtual-symbolic-filesystem"};
  for (unsigned Variant = 0; Variant != 6; ++Variant) {
    const bool Missing = Variant >= 3;
    const unsigned Kind = Missing ? 1 : Variant;
    auto Files =
        emulation::darwin_test::symbolicLinkFileSystemFilesJSON(Kind == 1);
    auto &F = *Files.getAsObject();
    auto &Env = *F.getObject("filesystem_observations");
    if (Kind == 2)
      (*(*Env.getArray("objects"))[2].getAsObject()->getObject(
          "snapshot"))["owner"] = 17;
    if (Variant == 3)
      F.erase("filesystem_observations");
    else if (Variant == 4)
      Env["objects"] = llvm::json::Array{};
    else if (Variant == 5) {
      Env["created_symbolic_link_objects"] = false;
      Env["created_regular_objects"] = Env["created_directory_objects"] = true;
    }
    const auto Options =
        llvm::formatv(
            "{0}",
            llvm::json::Value(llvm::json::Object{
                {"backend", "unicorn"},
                {"instruction_quantum", 1024},
                {"timeout_microseconds", 5000000},
                {"arguments", llvm::json::Array{"guest", Modes[Kind], "/link"}},
                {"darwin_files", std::move(Files)},
                {"darwin_system",
                 llvm::cantFail(llvm::json::parse(
                     emulation::darwin_test::ModeCredentialsJSON))}}))
            .str();
    for (unsigned Repeat = 0; Repeat != (Missing ? 1u : 2u); ++Repeat) {
      auto Text = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), Profile, Options.c_str()));
      ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
      auto Report = llvm::json::parse(Text);
      ASSERT_TRUE(bool(Report));
      const auto *O = Report->getAsObject();
      ASSERT_NE(O, nullptr);
      EXPECT_EQ(O->getString(field::Stop),
                Missing ? "unsupported_service" : "exited");
      EXPECT_EQ(
          O->getString(field::Stdout),
          Missing ? ""
                  : llvm::toHex(
                        emulation::darwin_test::symbolicFileSystemOutput(Kind),
                        true));
      EXPECT_EQ(O->getString(field::Stderr), "");
      const auto *Calls = O->getArray(field::Services);
      ASSERT_NE(Calls, nullptr);
      if (Missing) {
        EXPECT_EQ(
            O->getString(field::Diagnostic),
            Variant == 3
                ? "Darwin filesystem observation environment is not configured"
                : "Darwin object filesystem snapshot is not configured");
        ASSERT_EQ(Calls->size(), 4u);
        const auto *Last = Calls->back().getAsObject();
        ASSERT_NE(Last, nullptr);
        EXPECT_EQ(Last->getString(field::Number),
                  llvm::utohexstr(Class | 346, true));
        EXPECT_EQ(*Last->get(field::Result), llvm::json::Value(nullptr));
        EXPECT_EQ(Last->get(field::Error), nullptr);
      } else {
        EXPECT_EQ(O->getInteger(field::ExitStatus), 57);
        const auto Wanted =
            emulation::darwin_test::symbolicFileSystemServices(Kind);
        ASSERT_EQ(Calls->size(), Wanted.size());
        for (unsigned I = 0; I != Wanted.size(); ++I) {
          SCOPED_TRACE(I);
          const auto *E = (*Calls)[I].getAsObject();
          ASSERT_NE(E, nullptr);
          EXPECT_EQ(E->getString(field::Number),
                    llvm::utohexstr(Class | Wanted[I].Number, true));
          EXPECT_EQ(E->getString(field::Result),
                    llvm::utohexstr(Wanted[I].Value, true));
          EXPECT_EQ(E->getBoolean(field::Error), Wanted[I].Error);
          const auto *Args = E->getArray(field::Arguments);
          ASSERT_NE(Args, nullptr);
          for (unsigned J = 0; J != 2; ++J) {
            const auto W = J ? Wanted[I].Output : Wanted[I].Object;
            if (W)
              EXPECT_EQ((*Args)[J].getAsString(), llvm::utohexstr(*W, true));
          }
          const auto N = Wanted[I].Number & 0xffffff;
          if (N == 345 || N == 346)
            for (unsigned J = 2; J != 6; ++J)
              EXPECT_EQ((*Args)[J].getAsString(), "ffffffffffffffff");
        }
      }
      EXPECT_EQ(neverd_session_is_loaded(Session), 0);
      if (!Repeat) {
        EXPECT_EQ(runCLIRequest(Path, Profile, Options, Output),
                  Missing ? process_cli::Incomplete
                          : process_cli::GuestFailure);
        ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, *Report));
      }
    }
  }
#endif
}
INSTANTIATE_TEST_SUITE_P(
    Platforms, DarwinSymbolicFileSystemPublic,
    testing::Values(std::pair{"macos-arm64", MacOSMachO64},
                    std::pair{"macos-x86_64", MacOSMachO64},
                    std::pair{"ios-arm64", IOSMachO64},
                    std::pair{"ios-simulator-arm64", IOSSimulatorMachO64},
                    std::pair{"ios-simulator-x86_64", IOSSimulatorMachO64}),
    [](const testing::TestParamInfo<std::pair<const char *, const char *>>
           &Info) {
      std::string Name = Info.param.first;
      std::replace(Name.begin(), Name.end(), '-', '_');
      return Name;
    });

class DarwinCreatedDirectoryFileSystemPublic
    : public ProcessPublic,
      public testing::WithParamInterface<
          std::pair<const char *, const char *>> {};
TEST_P(DarwinCreatedDirectoryFileSystemPublic,
       PreserveCreatedSnapshotsAcrossCAndCLI) {
#ifndef NEVERD_DARWIN_FIXTURE_DIR
  GTEST_SKIP() << "Clang and ld64.lld Darwin fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const auto &[File, Profile] = GetParam();
  Path = (std::filesystem::path(NEVERD_DARWIN_FIXTURE_DIR) / File).string();
  const uint64_t Class =
      llvm::StringRef(File).ends_with("x86_64") ? 0x2000000 : 0;
  for (unsigned Variant = 0; Variant != 6; ++Variant) {
    const bool Literal = Variant == 1, Missing = Variant >= 2;
    auto Files = emulation::darwin_test::createdDirectoryFileSystemFilesJSON();
    auto &F = *Files.getAsObject();
    if (Variant == 2)
      F.erase("filesystem_observations");
    else if (Variant == 3)
      (*F.getObject("filesystem_observations"))["objects"] =
          llvm::json::Array{};
    else if (Variant >= 4) {
      auto &P = *F.getObject("filesystem_observations");
      P["created_directory_objects"] = false;
      P["created_regular_objects"] = Variant == 4;
    }
    const auto Options =
        llvm::formatv("{0}",
                      llvm::json::Value(llvm::json::Object{
                          {"backend", "unicorn"},
                          {"instruction_quantum", 1024},
                          {"timeout_microseconds", 5000000},
                          {"arguments",
                           llvm::json::Array{
                               "guest",
                               Literal ? "virtual-created-directory-filesystem"
                                       : "created-directory-filesystem",
                               "/child"}},
                          {"darwin_files", std::move(Files)},
                          {"darwin_system",
                           llvm::cantFail(llvm::json::parse(
                               emulation::darwin_test::ModeCredentialsJSON))}}))
            .str();
    for (unsigned Repeat = 0; Repeat != (Missing ? 1u : 2u); ++Repeat) {
      auto Text = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), Profile, Options.c_str()));
      ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
      auto Report = llvm::json::parse(Text);
      ASSERT_TRUE(bool(Report));
      const auto *O = Report->getAsObject();
      ASSERT_NE(O, nullptr);
      EXPECT_EQ(O->getString(field::Stop),
                Missing ? "unsupported_service" : "exited");
      EXPECT_EQ(
          O->getString(field::Stdout),
          Missing
              ? ""
              : llvm::toHex(
                    emulation::darwin_test::createdDirectoryFileSystemOutput(
                        Literal),
                    true));
      EXPECT_EQ(O->getString(field::Stderr), "");
      const auto *Calls = O->getArray(field::Services);
      ASSERT_NE(Calls, nullptr);
      if (Missing) {
        EXPECT_EQ(
            O->getString(field::Diagnostic),
            Variant == 2
                ? "Darwin filesystem observation environment is not configured"
                : "Darwin object filesystem snapshot is not configured");
        ASSERT_EQ(Calls->size(), 4u);
        const auto *Last = Calls->back().getAsObject();
        ASSERT_NE(Last, nullptr);
        EXPECT_EQ(Last->getString(field::Number),
                  llvm::utohexstr(Class | 346, true));
        EXPECT_EQ(*Last->get(field::Result), llvm::json::Value(nullptr));
        EXPECT_EQ(Last->get(field::Error), nullptr);
      } else {
        EXPECT_EQ(O->getInteger(field::ExitStatus), 56);
        const auto Wanted =
            emulation::darwin_test::createdDirectoryFileSystemServices(Literal);
        ASSERT_EQ(Calls->size(), Wanted.size());
        for (unsigned I = 0; I != Wanted.size(); ++I) {
          SCOPED_TRACE(I);
          const auto *E = (*Calls)[I].getAsObject();
          ASSERT_NE(E, nullptr);
          EXPECT_EQ(E->getString(field::Number),
                    llvm::utohexstr(Class | Wanted[I].Number, true));
          EXPECT_EQ(E->getString(field::Result),
                    llvm::utohexstr(Wanted[I].Value, true));
          EXPECT_EQ(E->getBoolean(field::Error), Wanted[I].Error);
          const auto *Args = E->getArray(field::Arguments);
          ASSERT_NE(Args, nullptr);
          for (unsigned J = 0; J != 2; ++J) {
            const auto W = J ? Wanted[I].Output : Wanted[I].Object;
            if (W)
              EXPECT_EQ((*Args)[J].getAsString(), llvm::utohexstr(*W, true));
          }
          if ((Wanted[I].Number & 0xffffff) == 345 ||
              (Wanted[I].Number & 0xffffff) == 346)
            for (unsigned J = 2; J != 6; ++J)
              EXPECT_EQ((*Args)[J].getAsString(), "ffffffffffffffff");
        }
      }
      EXPECT_EQ(neverd_session_is_loaded(Session), 0);
      if (!Repeat) {
        EXPECT_EQ(runCLIRequest(Path, Profile, Options, Output),
                  Missing ? process_cli::Incomplete
                          : process_cli::GuestFailure);
        ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, *Report));
      }
    }
  }
#endif
}
INSTANTIATE_TEST_SUITE_P(
    Platforms, DarwinCreatedDirectoryFileSystemPublic,
    testing::Values(std::pair{"macos-arm64", MacOSMachO64},
                    std::pair{"macos-x86_64", MacOSMachO64},
                    std::pair{"ios-arm64", IOSMachO64},
                    std::pair{"ios-simulator-arm64", IOSSimulatorMachO64},
                    std::pair{"ios-simulator-x86_64", IOSSimulatorMachO64}),
    [](const testing::TestParamInfo<std::pair<const char *, const char *>>
           &Info) {
      std::string Name = Info.param.first;
      std::replace(Name.begin(), Name.end(), '-', '_');
      return Name;
    });

class DarwinLibSystemPublic : public ProcessPublic,
                              public testing::WithParamInterface<
                                  std::pair<const char *, const char *>> {};
TEST_P(DarwinLibSystemPublic, ImportedCallsKeepErrnoAndReportsAcrossCAndCLI) {
#ifndef NEVERD_DARWIN_FIXTURE_DIR
  GTEST_SKIP() << "Clang and ld64.lld Darwin fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const auto &[File, Profile] = GetParam();
  const char *Names[] = {"___error", "___error", "_getpid", "_getpid", "_write",
                         "_write",   "_read",    "_write",  "_read",   "_close",
                         "_close",   "_read",    "_write",  "_close"};
  const unsigned Counts[] = {0, 0, 0, 0, 3, 3, 3, 3, 3, 1, 1, 3, 3, 1};
  for (const char *Kind : {"classic", "chained"}) {
    SCOPED_TRACE(Kind);
    Path = (std::filesystem::path(NEVERD_DARWIN_FIXTURE_DIR) /
            (std::string(File) + "-libsystem-" + Kind))
               .string();
    for (bool Exit : {false, true}) {
      llvm::json::Array Args{"libsystem"};
      if (Exit)
        Args.push_back("exit");
      const auto Options = jsonText(llvm::json::Object{
          {"backend", "unicorn"},
          {"instruction_quantum", 1024},
          {"timeout_microseconds", 5000000},
          {"arguments", std::move(Args)},
          {"darwin_files", llvm::json::Object{{"files", llvm::json::Array{}},
                                              {"stdin_hex", "6162006364"}}}});
      for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
        auto Text = takeString(neverd_emulate_process_json(
            Session, Path.c_str(), Profile, Options.c_str()));
        ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
        auto Report = llvm::json::parse(Text);
        ASSERT_TRUE(bool(Report));
        const auto *O = Report->getAsObject();
        ASSERT_NE(O, nullptr);
        EXPECT_EQ(O->getInteger(field::Version), 1);
        EXPECT_EQ(O->getString(field::Stop), "exited")
            << O->getString(field::Diagnostic).value_or("").str();
        EXPECT_EQ(O->getInteger(field::ExitStatus), Exit ? 35 : 58);
        EXPECT_EQ(O->getString(field::Stdout), Exit ? "58" : "4100ff4221");
        EXPECT_EQ(O->getString(field::Stderr), "");
        EXPECT_EQ(O->getString(field::Backend), "unicorn");
        ASSERT_NE(O->getArray(field::Services), nullptr);
        EXPECT_TRUE(O->getArray(field::Services)->empty());
        const auto *Darwin = O->getObject(field::Darwin);
        ASSERT_NE(Darwin, nullptr);
        const auto *Calls = Darwin->getArray(field::NativeCalls);
        ASSERT_NE(Calls, nullptr);
        ASSERT_EQ(Calls->size(), Exit ? 2u : 14u);
        for (unsigned I = 0; I != Calls->size(); ++I) {
          SCOPED_TRACE(I);
          const auto *Call = (*Calls)[I].getAsObject();
          ASSERT_NE(Call, nullptr);
          EXPECT_EQ(Call->getString(field::Module),
                    "/usr/lib/libSystem.B.dylib");
          EXPECT_EQ(Call->getString(field::Name),
                    Exit ? (I ? "__exit" : "_write") : Names[I]);
          const auto *Arguments = Call->getArray(field::Arguments);
          ASSERT_NE(Arguments, nullptr);
          EXPECT_EQ(Arguments->size(), Exit ? (I ? 1u : 3u) : Counts[I]);
        }
        if (Exit) {
          EXPECT_EQ((*Calls)[1].getAsObject()->get(field::Result)->kind(),
                    llvm::json::Value::Null);
        } else {
          EXPECT_EQ((*Calls)[0].getAsObject()->getString(field::Result),
                    (*Calls)[1].getAsObject()->getString(field::Result));
          EXPECT_EQ((*(*Calls)[12].getAsObject()->getArray(field::Arguments))[0]
                        .getAsString(),
                    "1234567800000001");
          EXPECT_EQ((*(*Calls)[13].getAsObject()->getArray(field::Arguments))[0]
                        .getAsString(),
                    "12345678ffffffff");
          EXPECT_EQ((*Calls)[13].getAsObject()->getString(field::Result),
                    "ffffffffffffffff");
        }
        EXPECT_EQ(neverd_session_is_loaded(Session), 0);
        if (!Repeat) {
          EXPECT_EQ(runCLIRequest(Path, Profile, Options, Output),
                    process_cli::GuestFailure);
          ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, *Report));
        }
      }
    }
  }
#endif
}
TEST_P(DarwinLibSystemPublic,
       ClassicLazyCallsKeepSlotsErrnoAndReportsAcrossCAndCLI) {
#ifndef NEVERD_DARWIN_FIXTURE_DIR
  GTEST_SKIP() << "Clang and ld64.lld Darwin fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const auto &[File, Profile] = GetParam();
  Path = (std::filesystem::path(NEVERD_DARWIN_FIXTURE_DIR) /
          (std::string(File) + "-libsystem-lazy"))
             .string();
  const char *Names[] = {"___error", "___error", "_getpid", "_getpid", "_write",
                         "_write",   "_read",    "_close",  "_read",   "_close",
                         "_getpid",  "_write",   "__exit"};
  for (bool Terminal : {false, true}) {
    llvm::json::Array Args{"lazy"};
    if (Terminal)
      Args.push_back("exit");
    const auto Options = jsonText(llvm::json::Object{
        {"backend", "unicorn"},
        {"instruction_quantum", 1024},
        {"timeout_microseconds", 5000000},
        {"arguments", std::move(Args)},
        {"darwin_files", llvm::json::Object{{"files", llvm::json::Array{}},
                                            {"stdin_hex", ""}}}});
    for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
      auto Text = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), Profile, Options.c_str()));
      ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
      auto Report = llvm::json::parse(Text);
      ASSERT_TRUE(bool(Report));
      const auto *O = Report->getAsObject();
      ASSERT_NE(O, nullptr);
      EXPECT_EQ(O->getInteger(field::Version), 1);
      EXPECT_EQ(O->getString(field::Stop), "exited")
          << O->getString(field::Diagnostic).value_or("").str();
      EXPECT_EQ(O->getInteger(field::ExitStatus), Terminal ? 35 : 83);
      EXPECT_EQ(O->getString(field::Stdout), Terminal ? "58" : "41005a53");
      EXPECT_EQ(O->getString(field::Stderr), "");
      EXPECT_EQ(O->getString(field::Backend), "unicorn");
      ASSERT_NE(O->getArray(field::Services), nullptr);
      EXPECT_TRUE(O->getArray(field::Services)->empty());
      const auto *Darwin = O->getObject(field::Darwin);
      ASSERT_NE(Darwin, nullptr);
      const auto *Calls = Darwin->getArray(field::NativeCalls);
      ASSERT_NE(Calls, nullptr);
      ASSERT_EQ(Calls->size(), Terminal ? 2u : 13u);
      for (unsigned I = 0; I != Calls->size(); ++I) {
        const auto *Call = (*Calls)[I].getAsObject();
        ASSERT_NE(Call, nullptr);
        EXPECT_EQ(Call->getString(field::Module), "/usr/lib/libSystem.B.dylib");
        EXPECT_EQ(Call->getString(field::Name),
                  Terminal ? (I ? "__exit" : "_write") : Names[I]);
      }
      EXPECT_EQ(Calls->back().getAsObject()->get(field::Result)->kind(),
                llvm::json::Value::Null);
      if (!Terminal) {
        EXPECT_EQ((*Calls)[0].getAsObject()->getString(field::Result),
                  (*Calls)[1].getAsObject()->getString(field::Result));
        EXPECT_EQ((*Calls)[5].getAsObject()->getString(field::Result),
                  "ffffffffffffffff");
        EXPECT_EQ((*Calls)[6].getAsObject()->getString(field::Result), "0");
      }
      EXPECT_EQ(neverd_session_is_loaded(Session), 0);
      if (!Repeat) {
        EXPECT_EQ(runCLIRequest(Path, Profile, Options, Output),
                  process_cli::GuestFailure);
        ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, *Report));
      }
    }
  }
#endif
}
TEST_P(DarwinLibSystemPublic,
       FileCallsKeepOptionalModeAndOffsetsAcrossCAndCLI) {
#ifndef NEVERD_DARWIN_FIXTURE_DIR
  GTEST_SKIP() << "Clang and ld64.lld Darwin fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const auto &[File, Profile] = GetParam();
  for (const char *Kind : {"classic", "chained"}) {
    SCOPED_TRACE(Kind);
    Path = (std::filesystem::path(NEVERD_DARWIN_FIXTURE_DIR) /
            (std::string(File) + "-libsystem-files-" + Kind))
               .string();
    const auto Options = jsonText(llvm::json::Object{
        {"backend", "unicorn"},
        {"instruction_quantum", 1024},
        {"timeout_microseconds", 5000000},
        {"arguments",
         llvm::json::Array{"files", "/data", "/created", "/missing"}},
        {"darwin_files", llvm::cantFail(llvm::json::parse(
                             emulation::darwin_test::LibSystemFilesJSON))}});
    for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
      auto Text = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), Profile, Options.c_str()));
      ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
      auto Report = llvm::json::parse(Text);
      ASSERT_TRUE(bool(Report));
      const auto *O = Report->getAsObject();
      ASSERT_NE(O, nullptr);
      EXPECT_EQ(O->getInteger(field::Version), 1);
      EXPECT_EQ(O->getString(field::Stop), "exited")
          << O->getString(field::Diagnostic).value_or("").str();
      EXPECT_EQ(O->getInteger(field::ExitStatus), 73);
      EXPECT_EQ(O->getString(field::Stdout), "460044");
      EXPECT_EQ(O->getString(field::Stderr), "");
      EXPECT_EQ(O->getInteger(field::Events), 41);
      ASSERT_NE(O->getArray(field::Services), nullptr);
      EXPECT_TRUE(O->getArray(field::Services)->empty());
      const auto *Darwin = O->getObject(field::Darwin);
      ASSERT_NE(Darwin, nullptr);
      const auto *Calls = Darwin->getArray(field::NativeCalls);
      ASSERT_NE(Calls, nullptr);
      ASSERT_EQ(Calls->size(),
                std::size(emulation::darwin_test::LibSystemFileCalls));
      for (unsigned I = 0; I != Calls->size(); ++I) {
        const auto *Call = (*Calls)[I].getAsObject();
        ASSERT_NE(Call, nullptr);
        const auto &Expected = emulation::darwin_test::LibSystemFileCalls[I];
        EXPECT_EQ(Call->getString(field::Name), Expected.Name);
        EXPECT_EQ(Call->getString(field::Module), "/usr/lib/libSystem.B.dylib");
        ASSERT_NE(Call->getArray(field::Arguments), nullptr);
        EXPECT_EQ(Call->getArray(field::Arguments)->size(), Expected.Count);
        EXPECT_TRUE(Call->getString(field::Result));
      }
      EXPECT_EQ((*Calls)[20].getAsObject()->getString(field::Result),
                "100000007");
      EXPECT_EQ((*Calls)[24].getAsObject()->getString(field::Result),
                "ffffffffffffffff");
      auto Mode = (*(*Calls)[31].getAsObject()->getArray(field::Arguments))[2]
                      .getAsString();
      ASSERT_TRUE(Mode);
      uint64_t Carrier = 0;
      ASSERT_FALSE(Mode->getAsInteger(16, Carrier));
      EXPECT_EQ(uint32_t(Carrier), 0x123401a1u);
      EXPECT_EQ(neverd_session_is_loaded(Session), 0);
      if (!Repeat) {
        EXPECT_EQ(runCLIRequest(Path, Profile, Options, Output),
                  process_cli::GuestFailure);
        ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, *Report));
      }
    }
  }
#endif
}
TEST_P(DarwinLibSystemPublic,
       MemoryCallsKeepCChecksAndMappingLifetimeAcrossCAndCLI) {
#ifndef NEVERD_DARWIN_FIXTURE_DIR
  GTEST_SKIP() << "Clang and ld64.lld Darwin fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const auto &[File, Profile] = GetParam();
  const auto Page =
      std::string(File).find("arm64") != std::string::npos ? 16384u : 4096u;
  const auto Input = emulation::darwin_test::libSystemMemoryOptions(Page);
  const auto &Bytes = Input.Files.at("/data");
  const auto FileHex = llvm::toHex(llvm::ArrayRef<uint8_t>(Bytes), true);
  for (const char *Kind : {"classic", "chained"}) {
    SCOPED_TRACE(Kind);
    Path = (std::filesystem::path(NEVERD_DARWIN_FIXTURE_DIR) /
            (std::string(File) + "-libsystem-memory-" + Kind))
               .string();
    const auto Options = jsonText(llvm::json::Object{
        {"backend", "unicorn"},
        {"instruction_quantum", 1024},
        {"timeout_microseconds", 5000000},
        {"arguments",
         llvm::json::Array{"memory", std::to_string(Page), "/data"}},
        {"darwin_files",
         llvm::json::Object{
             {"files", llvm::json::Array{llvm::json::Object{
                           {"path", "/data"}, {"bytes_hex", FileHex}}}}}}});
    for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
      auto Text = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), Profile, Options.c_str()));
      ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
      auto Report = llvm::json::parse(Text);
      ASSERT_TRUE(bool(Report));
      const auto *O = Report->getAsObject();
      ASSERT_NE(O, nullptr);
      EXPECT_EQ(O->getInteger(field::Version), 1);
      EXPECT_EQ(O->getString(field::Stop), "exited")
          << O->getString(field::Diagnostic).value_or("").str();
      EXPECT_EQ(O->getInteger(field::ExitStatus), 79);
      EXPECT_EQ(O->getString(field::Stdout), "4d0050");
      EXPECT_EQ(O->getString(field::Stderr), "");
      EXPECT_EQ(O->getInteger(field::Events), 19);
      ASSERT_NE(O->getArray(field::Services), nullptr);
      EXPECT_TRUE(O->getArray(field::Services)->empty());
      const auto *Darwin = O->getObject(field::Darwin);
      ASSERT_NE(Darwin, nullptr);
      const auto *Calls = Darwin->getArray(field::NativeCalls);
      ASSERT_NE(Calls, nullptr);
      ASSERT_EQ(Calls->size(),
                std::size(emulation::darwin_test::LibSystemMemoryCalls));
      for (unsigned I = 0; I != Calls->size(); ++I) {
        const auto *Call = (*Calls)[I].getAsObject();
        ASSERT_NE(Call, nullptr);
        const auto &Expected = emulation::darwin_test::LibSystemMemoryCalls[I];
        EXPECT_EQ(Call->getString(field::Name), Expected.Name);
        EXPECT_EQ(Call->getString(field::Module), "/usr/lib/libSystem.B.dylib");
        ASSERT_NE(Call->getArray(field::Arguments), nullptr);
        EXPECT_EQ(Call->getArray(field::Arguments)->size(), Expected.Count);
        EXPECT_TRUE(Call->getString(field::Result));
      }
      for (unsigned I : {1u, 2u, 5u, 9u, 10u})
        EXPECT_EQ((*Calls)[I].getAsObject()->getString(field::Result),
                  "ffffffffffffffff");
      EXPECT_EQ((*(*Calls)[3].getAsObject()->getArray(field::Arguments))[3]
                    .getAsString(),
                "1002");
      EXPECT_EQ(neverd_session_is_loaded(Session), 0);
      if (!Repeat) {
        EXPECT_EQ(runCLIRequest(Path, Profile, Options, Output),
                  process_cli::GuestFailure);
        ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, *Report));
      }
    }
  }
#endif
}
TEST_P(DarwinLibSystemPublic, StartupDylibsAndTLSKeepIdentityAcrossCAndCLI) {
#ifndef NEVERD_DARWIN_FIXTURE_DIR
  GTEST_SKIP() << "Clang and ld64.lld Darwin fixtures unavailable";
#else
  llvm::SmallString<128> Temporary;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Temporary));
  const std::filesystem::path Root(Temporary.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const auto &[File, Profile] = GetParam();
  for (const char *Kind : {"classic", "chained"}) {
    SCOPED_TRACE(Kind);
    const auto Directory = std::filesystem::path(NEVERD_DARWIN_FIXTURE_DIR) /
                           (std::string(File) + "-dylib-" + Kind);
    Path = (Directory / "main").string();
    llvm::json::Array Modules;
    for (const char *Name : {"libneverd-base.dylib", "libneverd-leaf.dylib"})
      Modules.push_back(
          llvm::json::Object{{"name", std::string("@loader_path/") + Name},
                             {"path", (Directory / Name).string()}});
    const auto Options = jsonText(llvm::json::Object{
        {"backend", "unicorn"},
        {"instruction_quantum", 1024},
        {"timeout_microseconds", 5000000},
        {"darwin", llvm::json::Object{{"modules", std::move(Modules)}}}});
    for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
      auto Text = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), Profile, Options.c_str()));
      ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
      auto Report = llvm::json::parse(Text);
      ASSERT_TRUE(bool(Report));
      const auto *O = Report->getAsObject();
      ASSERT_NE(O, nullptr);
      EXPECT_EQ(O->getInteger(field::Version), 1);
      EXPECT_EQ(O->getString(field::Stop), "exited")
          << O->getString(field::Diagnostic).value_or("").str();
      EXPECT_EQ(O->getInteger(field::ExitStatus), 83);
      EXPECT_EQ(O->getString(field::Stdout), "440054");
      EXPECT_EQ(O->getString(field::Stderr), "");
      ASSERT_NE(O->getArray(field::Services), nullptr);
      EXPECT_TRUE(O->getArray(field::Services)->empty());
      const auto *Darwin = O->getObject(field::Darwin);
      ASSERT_NE(Darwin, nullptr);
      const auto *Calls = Darwin->getArray(field::NativeCalls);
      ASSERT_NE(Calls, nullptr);
      ASSERT_GT(Calls->size(), 10u);
      EXPECT_EQ(O->getInteger(field::Events), Calls->size() + 1);
      unsigned GetPID = 0;
      for (unsigned I = 0; I != Calls->size(); ++I) {
        const auto *Call = (*Calls)[I].getAsObject();
        ASSERT_NE(Call, nullptr);
        const bool Write = I + 1 == Calls->size();
        const bool PID = Call->getString(field::Name) == "_getpid";
        GetPID += PID;
        EXPECT_EQ(Call->getString(field::Name), Write ? "_write"
                                                : PID ? "_getpid"
                                                      : "__tlv_get_addr");
        EXPECT_EQ(Call->getString(field::Module), "/usr/lib/libSystem.B.dylib");
        ASSERT_NE(Call->getArray(field::Arguments), nullptr);
        EXPECT_EQ(Call->getArray(field::Arguments)->size(), Write ? 3u
                                                            : PID ? 0u
                                                                  : 1u);
        EXPECT_TRUE(Call->getString(field::Result));
        if (PID)
          EXPECT_EQ(Call->getString(field::Result), "3e8");
      }
      EXPECT_EQ(GetPID, 2u);
      EXPECT_EQ(neverd_session_is_loaded(Session), 0);
      if (!Repeat) {
        EXPECT_EQ(runCLIRequest(Path, Profile, Options, Output),
                  process_cli::GuestFailure);
        ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, *Report));
      }
    }
  }
#endif
}
TEST_P(DarwinLibSystemPublic,
       DylibAddendsAndTLSPointersKeepIdentityAcrossCAndCLI) {
#ifndef NEVERD_DARWIN_FIXTURE_DIR
  GTEST_SKIP() << "Clang and ld64.lld Darwin fixtures unavailable";
#else
  llvm::SmallString<128> Temporary;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Temporary));
  const std::filesystem::path Root(Temporary.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const auto &[File, Profile] = GetParam();
  for (const char *Kind : {"classic", "chained"}) {
    SCOPED_TRACE(Kind);
    const auto Directory = std::filesystem::path(NEVERD_DARWIN_FIXTURE_DIR) /
                           (std::string(File) + "-dylib-addends-" + Kind);
    Path = (Directory / "main").string();
    const auto Options = jsonText(llvm::json::Object{
        {"backend", "unicorn"},
        {"instruction_quantum", 1024},
        {"timeout_microseconds", 5000000},
        {"darwin",
         llvm::json::Object{
             {"modules",
              llvm::json::Array{llvm::json::Object{
                  {"name", "@loader_path/libneverd-addends.dylib"},
                  {"path",
                   (Directory / "libneverd-addends.dylib").string()}}}}}}});
    for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
      auto Text = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), Profile, Options.c_str()));
      ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
      auto Report = llvm::json::parse(Text);
      ASSERT_TRUE(bool(Report));
      const auto *O = Report->getAsObject();
      ASSERT_NE(O, nullptr);
      EXPECT_EQ(O->getInteger(field::Version), 1);
      EXPECT_EQ(O->getString(field::Stop), "exited")
          << O->getString(field::Diagnostic).value_or("").str();
      EXPECT_EQ(O->getInteger(field::ExitStatus), 78);
      EXPECT_EQ(O->getString(field::Stdout), "");
      EXPECT_EQ(O->getString(field::Stderr), "");
      ASSERT_NE(O->getArray(field::Services), nullptr);
      EXPECT_TRUE(O->getArray(field::Services)->empty());
      const auto *Darwin = O->getObject(field::Darwin);
      ASSERT_NE(Darwin, nullptr);
      const auto *Calls = Darwin->getArray(field::NativeCalls);
      ASSERT_NE(Calls, nullptr);
      unsigned PID = 0, TLS = 0;
      for (const auto &V : *Calls) {
        const auto *Call = V.getAsObject();
        ASSERT_NE(Call, nullptr);
        const bool IsPID = Call->getString(field::Name) == "_getpid";
        PID += IsPID;
        TLS += Call->getString(field::Name) == "__tlv_get_addr";
        EXPECT_EQ(Call->getString(field::Module), "/usr/lib/libSystem.B.dylib");
        ASSERT_NE(Call->getArray(field::Arguments), nullptr);
        EXPECT_EQ(Call->getArray(field::Arguments)->size(), IsPID ? 0u : 1u);
        EXPECT_TRUE(Call->getString(field::Result));
        if (IsPID)
          EXPECT_EQ(Call->getString(field::Result), "3e8");
      }
      EXPECT_EQ(PID, 1u);
      EXPECT_GT(TLS, 0u);
      EXPECT_EQ(Calls->size(), PID + TLS);
      EXPECT_EQ(O->getInteger(field::Events), Calls->size() + 1);
      EXPECT_EQ(neverd_session_is_loaded(Session), 0);
      if (!Repeat) {
        EXPECT_EQ(runCLIRequest(Path, Profile, Options, Output),
                  process_cli::GuestFailure);
        ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, *Report));
      }
    }
  }
#endif
}
TEST_P(DarwinLibSystemPublic,
       ConstructorsAndTLSCallbacksKeepLifetimeAcrossCAndCLI) {
#ifndef NEVERD_DARWIN_FIXTURE_DIR
  GTEST_SKIP() << "Clang and ld64.lld Darwin fixtures unavailable";
#else
  llvm::SmallString<128> Temporary;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Temporary));
  const std::filesystem::path Root(Temporary.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const auto &[File, Profile] = GetParam();
  for (const char *Kind : {"classic", "chained"}) {
    const auto Directory = std::filesystem::path(NEVERD_DARWIN_FIXTURE_DIR) /
                           (std::string(File) + "-callbacks-" + Kind);
    Path = (Directory / "main").string();
    for (const auto &Test : neverd::emulation::darwin_test::CallbackCases) {
      SCOPED_TRACE(std::string(Kind) + ":" + Test.Mode);
      llvm::json::Array Modules;
      for (const char *Name : {"libleaf.dylib", "libmiddle.dylib"})
        Modules.push_back(
            llvm::json::Object{{"name", std::string("@loader_path/") + Name},
                               {"path", (Directory / Name).string()}});
      const auto Options = jsonText(llvm::json::Object{
          {"backend", "unicorn"},
          {"instruction_quantum", 1024},
          {"timeout_microseconds", 5000000},
          {"arguments", llvm::json::Array{"different/argv-zero", Test.Mode}},
          {"environment", llvm::json::Array{"NEVERD_CALLBACK_SCOPE=one"}},
          {"darwin", llvm::json::Object{{"modules", std::move(Modules)}}}});
      for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
        auto Text = takeString(neverd_emulate_process_json(
            Session, Path.c_str(), Profile, Options.c_str()));
        ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
        auto Report = llvm::json::parse(Text);
        ASSERT_TRUE(bool(Report));
        const auto *O = Report->getAsObject();
        ASSERT_NE(O, nullptr);
        EXPECT_EQ(O->getInteger(field::Version), 1);
        EXPECT_EQ(O->getString(field::Stop), "exited")
            << O->getString(field::Diagnostic).value_or("").str();
        EXPECT_EQ(O->getInteger(field::ExitStatus), Test.Status);
        EXPECT_EQ(O->getString(field::Stdout), llvm::toHex(Test.Output, true));
        EXPECT_EQ(O->getString(field::Stderr), "");
        ASSERT_NE(O->getArray(field::Services), nullptr);
        EXPECT_TRUE(O->getArray(field::Services)->empty());
        const auto *Darwin = O->getObject(field::Darwin);
        ASSERT_NE(Darwin, nullptr);
        const auto *Calls = Darwin->getArray(field::NativeCalls);
        ASSERT_NE(Calls, nullptr);
        unsigned Registrations = 0;
        for (const auto &V : *Calls) {
          const auto *Call = V.getAsObject();
          ASSERT_NE(Call, nullptr);
          if (Call->getString(field::Name) != "__tlv_atexit")
            continue;
          ++Registrations;
          ASSERT_NE(Call->getArray(field::Arguments), nullptr);
          EXPECT_EQ(Call->getArray(field::Arguments)->size(), 2u);
          ASSERT_NE(Call->get(field::Result), nullptr);
          EXPECT_EQ(*Call->get(field::Result), llvm::json::Value(nullptr));
        }
        EXPECT_GE(Registrations, 2u);
        EXPECT_EQ(neverd_session_is_loaded(Session), 0);
        if (!Repeat) {
          EXPECT_EQ(runCLIRequest(Path, Profile, Options, Output),
                    process_cli::GuestFailure);
          ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, *Report));
        }
      }
    }
  }
#endif
}
TEST_P(DarwinLibSystemPublic,
       GlobalAndStaticDestructorsKeepLifetimeAcrossCAndCLI) {
#ifndef NEVERD_DARWIN_FIXTURE_DIR
  GTEST_SKIP() << "Clang and ld64.lld Darwin fixtures unavailable";
#else
  llvm::SmallString<128> Temporary;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Temporary));
  const std::filesystem::path Root(Temporary.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const auto &[File, Profile] = GetParam();
  for (const char *Kind : {"classic", "chained"}) {
    const auto Directory = std::filesystem::path(NEVERD_DARWIN_FIXTURE_DIR) /
                           (std::string(File) + "-exit-" + Kind);
    Path = (Directory / "main").string();
    for (const auto &Test : neverd::emulation::darwin_test::GlobalExitCases) {
      SCOPED_TRACE(std::string(Kind) + ":" + Test.Mode);
      llvm::json::Array Modules;
      for (const char *Name : {"libleaf.dylib", "libparent.dylib"})
        Modules.push_back(
            llvm::json::Object{{"name", std::string("@loader_path/") + Name},
                               {"path", (Directory / Name).string()}});
      const auto Options = jsonText(llvm::json::Object{
          {"backend", "unicorn"},
          {"instruction_quantum", 1024},
          {"timeout_microseconds", 5000000},
          {"arguments", llvm::json::Array{"different/argv-zero", Test.Mode}},
          {"darwin", llvm::json::Object{{"modules", std::move(Modules)}}}});
      for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
        auto Text = takeString(neverd_emulate_process_json(
            Session, Path.c_str(), Profile, Options.c_str()));
        ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
        auto Report = llvm::json::parse(Text);
        ASSERT_TRUE(bool(Report));
        const auto *O = Report->getAsObject();
        ASSERT_NE(O, nullptr);
        EXPECT_EQ(O->getInteger(field::Version), 1);
        EXPECT_EQ(O->getString(field::Stop), "exited")
            << O->getString(field::Diagnostic).value_or("").str();
        EXPECT_EQ(O->getInteger(field::ExitStatus), Test.Status);
        EXPECT_EQ(O->getString(field::Stdout), llvm::toHex(Test.Output, true));
        EXPECT_EQ(O->getString(field::Stderr), "");
        ASSERT_NE(O->getArray(field::Services), nullptr);
        EXPECT_TRUE(O->getArray(field::Services)->empty());
        const auto *Darwin = O->getObject(field::Darwin);
        ASSERT_NE(Darwin, nullptr);
        const auto *Calls = Darwin->getArray(field::NativeCalls);
        ASSERT_NE(Calls, nullptr);
        unsigned C = 0, Cxx = 0;
        bool HighObject = false, NullObject = false;
        for (const auto &V : *Calls) {
          const auto *Call = V.getAsObject();
          ASSERT_NE(Call, nullptr);
          EXPECT_EQ(Call->getString(field::Module),
                    "/usr/lib/libSystem.B.dylib");
          const bool Object = Call->getString(field::Name) == "___cxa_atexit";
          if (!Object && Call->getString(field::Name) != "_atexit")
            continue;
          Cxx += Object;
          C += !Object;
          const auto *Args = Call->getArray(field::Arguments);
          ASSERT_NE(Args, nullptr);
          ASSERT_EQ(Args->size(), Object ? 3u : 1u);
          EXPECT_EQ(Call->getString(field::Result), "0");
          if (Object) {
            HighObject |= (*Args)[1].getAsString() == "fedcba9876543210" &&
                          (*Args)[2].getAsString() == "ffffffffffffffff";
            NullObject |= (*Args)[1].getAsString() == "0" &&
                          (*Args)[2].getAsString() == "fedcba9876543210";
          }
        }
        EXPECT_GE(C, 3u);
        EXPECT_GE(Cxx, 2u);
        EXPECT_EQ(HighObject, llvm::StringRef(Test.Mode) == "opaque");
        EXPECT_EQ(NullObject, llvm::StringRef(Test.Mode) == "opaque");
        EXPECT_EQ(neverd_session_is_loaded(Session), 0);
        if (!Repeat) {
          EXPECT_EQ(runCLIRequest(Path, Profile, Options, Output),
                    process_cli::GuestFailure);
          ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, *Report));
        }
      }
    }
  }
#endif
}
TEST_P(DarwinLibSystemPublic, DefaultStaticGuardsKeepLifetimeAcrossCAndCLI) {
#ifndef NEVERD_DARWIN_FIXTURE_DIR
  GTEST_SKIP() << "Clang and ld64.lld Darwin fixtures unavailable";
#else
  llvm::SmallString<128> Temporary;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Temporary));
  const std::filesystem::path Root(Temporary.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const auto &[File, Profile] = GetParam();
  for (const char *Kind : {"classic", "chained"}) {
    const auto Directory = std::filesystem::path(NEVERD_DARWIN_FIXTURE_DIR) /
                           (std::string(File) + "-guard-" + Kind);
    Path = (Directory / "main").string();
    for (const auto &Test : neverd::emulation::darwin_test::GuardCases) {
      SCOPED_TRACE(std::string(Kind) + ':' + Test.Mode);
      llvm::json::Array Modules;
      for (const char *Name : {"libleaf.dylib", "libparent.dylib"})
        Modules.push_back(
            llvm::json::Object{{"name", std::string("@loader_path/") + Name},
                               {"path", (Directory / Name).string()}});
      const auto Options = jsonText(llvm::json::Object{
          {"backend", "unicorn"},
          {"instruction_quantum", 1024},
          {"timeout_microseconds", 5000000},
          {"arguments", llvm::json::Array{"different/argv-zero", Test.Mode}},
          {"darwin_system",
           llvm::json::Object{{"thread_self_port", 0xf1234567LL}}},
          {"darwin", llvm::json::Object{{"modules", std::move(Modules)}}}});
      for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
        auto Text = takeString(neverd_emulate_process_json(
            Session, Path.c_str(), Profile, Options.c_str()));
        ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
        auto Report = llvm::json::parse(Text);
        ASSERT_TRUE(bool(Report));
        const auto *O = Report->getAsObject();
        ASSERT_NE(O, nullptr);
        EXPECT_EQ(O->getInteger(field::Version), 1);
        ASSERT_EQ(O->getString(field::Stop), "exited")
            << O->getString(field::Diagnostic).value_or("").str();
        EXPECT_EQ(O->getInteger(field::ExitStatus), Test.Status);
        EXPECT_EQ(O->getString(field::Stdout), llvm::toHex(Test.Output, true));
        EXPECT_EQ(O->getString(field::Stderr), "");
        ASSERT_NE(O->getArray(field::Services), nullptr);
        EXPECT_TRUE(O->getArray(field::Services)->empty());
        const auto *Darwin = O->getObject(field::Darwin);
        ASSERT_NE(Darwin, nullptr);
        const auto *Calls = Darwin->getArray(field::NativeCalls);
        ASSERT_NE(Calls, nullptr);
        unsigned Guards = 0, Aborts = 0, Zero = 0;
        for (const auto &V : *Calls) {
          const auto *Call = V.getAsObject();
          ASSERT_NE(Call, nullptr);
          const auto Name = Call->getString(field::Name).value_or("");
          const bool Guard = Name.starts_with("___cxa_guard_");
          EXPECT_EQ(Call->getString(field::Module),
                    Guard ? "/usr/lib/libc++.1.dylib"
                          : "/usr/lib/libSystem.B.dylib");
          if (!Guard)
            continue;
          ++Guards;
          const auto *Args = Call->getArray(field::Arguments);
          ASSERT_NE(Args, nullptr);
          EXPECT_EQ(Args->size(), 1u);
          if (Name == "___cxa_guard_acquire") {
            const auto Value = Call->getString(field::Result);
            EXPECT_TRUE(Value == "0" || Value == "1");
            Zero += Value == "0";
          } else {
            ASSERT_NE(Call->get(field::Result), nullptr);
            EXPECT_EQ(*Call->get(field::Result), llvm::json::Value(nullptr));
            Aborts += Name == "___cxa_guard_abort";
          }
        }
        const bool Constructor =
            llvm::StringRef(Test.Mode).starts_with("ctor-");
        EXPECT_GE(Guards, 6u);
        EXPECT_EQ(Aborts, Constructor ? 0u : 1u);
        EXPECT_EQ(Zero, Constructor ? 0u : 1u);
        EXPECT_EQ(neverd_session_is_loaded(Session), 0);
        if (!Repeat) {
          EXPECT_EQ(runCLIRequest(Path, Profile, Options, Output),
                    process_cli::GuestFailure);
          ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, *Report));
        }
      }
    }
  }
#endif
}
INSTANTIATE_TEST_SUITE_P(
    Platforms, DarwinLibSystemPublic,
    testing::Values(std::pair{"macos-arm64", MacOSMachO64},
                    std::pair{"macos-x86_64", MacOSMachO64},
                    std::pair{"ios-arm64", IOSMachO64},
                    std::pair{"ios-simulator-arm64", IOSSimulatorMachO64},
                    std::pair{"ios-simulator-x86_64", IOSSimulatorMachO64}),
    [](const testing::TestParamInfo<std::pair<const char *, const char *>>
           &Info) {
      std::string Name = Info.param.first;
      std::replace(Name.begin(), Name.end(), '-', '_');
      return Name;
    });

class DarwinSymbolicOwnershipPublic
    : public ProcessPublic,
      public testing::WithParamInterface<
          std::pair<const char *, const char *>> {};
TEST_P(DarwinSymbolicOwnershipPublic, PreserveVnodeOwnershipAcrossCAndCLI) {
#ifndef NEVERD_DARWIN_FIXTURE_DIR
  GTEST_SKIP() << "Clang and ld64.lld Darwin fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto OutputFilePath = (Root / OutputFile).string();
  const auto &[File, Profile] = GetParam();
  Path = (std::filesystem::path(NEVERD_DARWIN_FIXTURE_DIR) / File).string();
  const uint64_t Class =
      llvm::StringRef(File).ends_with("x86_64") ? 0x2000000 : 0;
  constexpr const char *Modes[] = {
      "symbolic-ownership-mutations", "virtual-symbolic-ownership",
      "created-symbolic-ownership", "virtual-created-symbolic-ownership"};
  for (unsigned Variant = 0; Variant != 7; ++Variant) {
    SCOPED_TRACE(Variant);
    const unsigned Kind = Variant < 4 ? Variant : Variant == 5 ? 2 : 1;
    const bool Missing = Variant >= 4, Late = Variant == 6;
    auto Files = emulation::darwin_test::symbolicOwnershipFilesJSON(true);
    auto &M = *Files.getAsObject()->getObject("ownership_mutations");
    if (Variant == 4)
      M["objects"] = llvm::json::Array{llvm::json::Object{
          {"path", "/data"},
          {"time", llvm::json::Object{{"seconds", 81}, {"nanoseconds", 82}}}}};
    else if (Missing)
      M["created_symbolic_link_objects"] = false;
    llvm::json::Value Request(llvm::json::Object{
        {"backend", "unicorn"},
        {"instruction_quantum", 1024},
        {"timeout_microseconds", 5000000},
        {"arguments", llvm::json::Array{"guest", Modes[Kind],
                                        Kind >= 2 ? "/dir/new" : "/dir/link"}},
        {"darwin_files", std::move(Files)},
        {"darwin_system",
         llvm::cantFail(llvm::json::parse(
             emulation::darwin_test::SymbolicOwnershipCredentialsJSON))}});
    const auto Options = llvm::formatv("{0}", Request).str();
    const auto Wanted = emulation::darwin_test::symbolicOwnershipServices(Kind);
    auto ExpectedOutput = emulation::darwin_test::symbolicOwnershipOutput(Kind);
    unsigned Last = Wanted.size();
    if (Missing) {
      Last = 0;
      while (Last != Wanted.size() &&
             !(Wanted[Last].Number == 123 &&
               Wanted[Last].Arguments[0] == (Late ? 8 : 3)))
        ++Last;
      ASSERT_NE(Last, Wanted.size());
      ExpectedOutput = Late ? ExpectedOutput.substr(0, 6 * 160) : "";
    }
    for (unsigned Repeat = 0; Repeat != (Missing ? 1u : 2u); ++Repeat) {
      auto Text = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), Profile, Options.c_str()));
      ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
      auto Report = llvm::json::parse(Text);
      ASSERT_TRUE(bool(Report));
      const auto *O = Report->getAsObject();
      ASSERT_NE(O, nullptr);
      EXPECT_EQ(O->getString(field::Stop),
                Missing ? "unsupported_service" : "exited");
      EXPECT_EQ(O->getString(field::Stdout), llvm::toHex(ExpectedOutput, true));
      EXPECT_EQ(O->getString(field::Stderr), "");
      EXPECT_EQ(O->getString(field::Backend), "unicorn");
      if (!Missing)
        EXPECT_EQ(O->getInteger(field::ExitStatus), 58);
      else
        EXPECT_EQ(O->getString(field::Diagnostic),
                  "Darwin object ownership provider is not configured");
      const auto *Calls = O->getArray(field::Services);
      ASSERT_NE(Calls, nullptr);
      ASSERT_EQ(Calls->size(), Missing ? Last + 1 : Wanted.size());
      for (unsigned I = 0; I != Calls->size(); ++I) {
        SCOPED_TRACE(I);
        const auto *E = (*Calls)[I].getAsObject();
        ASSERT_NE(E, nullptr);
        EXPECT_EQ(E->getString(field::Number),
                  llvm::utohexstr(Class | Wanted[I].Number, true));
        const auto *Args = E->getArray(field::Arguments);
        ASSERT_NE(Args, nullptr);
        ASSERT_EQ(Args->size(), 6u);
        for (unsigned J = 0; J != 6; ++J)
          if (Wanted[I].Arguments[J])
            EXPECT_EQ((*Args)[J].getAsString(),
                      llvm::utohexstr(*Wanted[I].Arguments[J], true));
        if (Missing && I == Last) {
          EXPECT_EQ(*E->get(field::Result), llvm::json::Value(nullptr));
          EXPECT_EQ(E->get(field::Error), nullptr);
        } else {
          EXPECT_EQ(E->getString(field::Result),
                    llvm::utohexstr(Wanted[I].Value, true));
          EXPECT_EQ(E->getBoolean(field::Error), Wanted[I].Error);
        }
      }
      EXPECT_EQ(neverd_session_is_loaded(Session), 0);
      if (!Repeat) {
        EXPECT_EQ(runCLIRequest(Path, Profile, Options, OutputFilePath),
                  Missing ? process_cli::Incomplete
                          : process_cli::GuestFailure);
        ASSERT_NO_FATAL_FAILURE(expectCLIReport(OutputFilePath, *Report));
      }
    }
    (*Request.getAsObject()
          ->getObject("darwin_files")
          ->getObject("ownership_mutations"))["created_symbolic_link_objects"] =
        1;
    const auto Malformed = llvm::formatv("{0}", Request).str();
    EXPECT_EQ(neverd_emulate_process_json(Session, Path.c_str(), Profile,
                                          Malformed.c_str()),
              nullptr);
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  }
#endif
}
TEST_P(DarwinSymbolicOwnershipPublic,
       LchownPreservesVnodeOwnershipAcrossCAndCLI) {
#ifndef NEVERD_DARWIN_FIXTURE_DIR
  GTEST_SKIP() << "Clang and ld64.lld Darwin fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto OutputFilePath = (Root / OutputFile).string();
  const auto &[File, Profile] = GetParam();
  Path = (std::filesystem::path(NEVERD_DARWIN_FIXTURE_DIR) / File).string();
  const uint64_t Class =
      llvm::StringRef(File).ends_with("x86_64") ? 0x2000000 : 0;
  constexpr const char *Modes[] = {"lchown-ownership-mutations",
                                   "virtual-lchown-ownership"};
  for (unsigned Variant = 0; Variant != 4; ++Variant) {
    SCOPED_TRACE(Variant);
    const unsigned Kind = Variant < 2 ? Variant : 1;
    const bool Missing = Variant >= 2, Late = Variant == 3;
    auto Files = emulation::darwin_test::symbolicOwnershipFilesJSON(true);
    auto &M = *Files.getAsObject()->getObject("ownership_mutations");
    if (Variant == 2)
      M["objects"] = llvm::json::Array{llvm::json::Object{
          {"path", "/data"},
          {"time", llvm::json::Object{{"seconds", 81}, {"nanoseconds", 82}}}}};
    else if (Missing)
      M["created_symbolic_link_objects"] = false;
    llvm::json::Value Request(llvm::json::Object{
        {"backend", "unicorn"},
        {"instruction_quantum", 1024},
        {"timeout_microseconds", 5000000},
        {"arguments", llvm::json::Array{"guest", Modes[Kind],
                                        Kind >= 2 ? "/dir/new" : "/dir/link"}},
        {"darwin_files", std::move(Files)},
        {"darwin_system",
         llvm::cantFail(llvm::json::parse(
             emulation::darwin_test::SymbolicOwnershipCredentialsJSON))}});
    const auto Options = llvm::formatv("{0}", Request).str();
    const auto BaseWanted =
        emulation::darwin_test::symbolicOwnershipServices(Kind);
    const auto Wanted =
        emulation::darwin_test::symbolicOwnershipServices(Kind, true);
    auto ExpectedOutput = emulation::darwin_test::symbolicOwnershipOutput(Kind);
    unsigned Last = Wanted.size();
    if (Missing) {
      Last = 0;
      while (Last != Wanted.size() &&
             !(BaseWanted[Last].Number == 123 &&
               BaseWanted[Last].Arguments[0] == (Late ? 8 : 3)))
        ++Last;
      ASSERT_NE(Last, Wanted.size());
      ExpectedOutput = Late ? ExpectedOutput.substr(0, 6 * 160) : "";
    }
    for (unsigned Repeat = 0; Repeat != (Missing ? 1u : 2u); ++Repeat) {
      auto Text = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), Profile, Options.c_str()));
      ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
      auto Report = llvm::json::parse(Text);
      ASSERT_TRUE(bool(Report));
      const auto *O = Report->getAsObject();
      ASSERT_NE(O, nullptr);
      EXPECT_EQ(O->getString(field::Stop),
                Missing ? "unsupported_service" : "exited");
      EXPECT_EQ(O->getString(field::Stdout), llvm::toHex(ExpectedOutput, true));
      EXPECT_EQ(O->getString(field::Stderr), "");
      EXPECT_EQ(O->getString(field::Backend), "unicorn");
      if (!Missing)
        EXPECT_EQ(O->getInteger(field::ExitStatus), 58);
      else
        EXPECT_EQ(O->getString(field::Diagnostic),
                  "Darwin object ownership provider is not configured");
      const auto *Calls = O->getArray(field::Services);
      ASSERT_NE(Calls, nullptr);
      ASSERT_EQ(Calls->size(), Missing ? Last + 1 : Wanted.size());
      for (unsigned I = 0; I != Calls->size(); ++I) {
        SCOPED_TRACE(I);
        const auto *E = (*Calls)[I].getAsObject();
        ASSERT_NE(E, nullptr);
        EXPECT_EQ(E->getString(field::Number),
                  llvm::utohexstr(Class | Wanted[I].Number, true));
        const auto *Args = E->getArray(field::Arguments);
        ASSERT_NE(Args, nullptr);
        ASSERT_EQ(Args->size(), 6u);
        for (unsigned J = 0; J != 6; ++J)
          if (Wanted[I].Arguments[J])
            EXPECT_EQ((*Args)[J].getAsString(),
                      llvm::utohexstr(*Wanted[I].Arguments[J], true));
        if (Missing && I == Last) {
          EXPECT_EQ(*E->get(field::Result), llvm::json::Value(nullptr));
          EXPECT_EQ(E->get(field::Error), nullptr);
        } else {
          EXPECT_EQ(E->getString(field::Result),
                    llvm::utohexstr(Wanted[I].Value, true));
          EXPECT_EQ(E->getBoolean(field::Error), Wanted[I].Error);
        }
      }
      EXPECT_EQ(neverd_session_is_loaded(Session), 0);
      if (!Repeat) {
        EXPECT_EQ(runCLIRequest(Path, Profile, Options, OutputFilePath),
                  Missing ? process_cli::Incomplete
                          : process_cli::GuestFailure);
        ASSERT_NO_FATAL_FAILURE(expectCLIReport(OutputFilePath, *Report));
      }
    }
    (*Request.getAsObject()
          ->getObject("darwin_files")
          ->getObject("ownership_mutations"))["created_symbolic_link_objects"] =
        1;
    const auto Malformed = llvm::formatv("{0}", Request).str();
    EXPECT_EQ(neverd_emulate_process_json(Session, Path.c_str(), Profile,
                                          Malformed.c_str()),
              nullptr);
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  }
#endif
}
INSTANTIATE_TEST_SUITE_P(
    Platforms, DarwinSymbolicOwnershipPublic,
    testing::Values(std::pair{"macos-arm64", MacOSMachO64},
                    std::pair{"macos-x86_64", MacOSMachO64},
                    std::pair{"ios-arm64", IOSMachO64},
                    std::pair{"ios-simulator-arm64", IOSSimulatorMachO64},
                    std::pair{"ios-simulator-x86_64", IOSSimulatorMachO64}),
    [](const testing::TestParamInfo<std::pair<const char *, const char *>>
           &Info) {
      std::string Name = Info.param.first;
      std::replace(Name.begin(), Name.end(), '-', '_');
      return Name;
    });

class DarwinDirectoryOwnershipPublic
    : public ProcessPublic,
      public testing::WithParamInterface<
          std::pair<const char *, const char *>> {};
TEST_P(DarwinDirectoryOwnershipPublic,
       PreserveCompleteDirectoryOwnershipRecordsAcrossCAndCLI) {
#ifndef NEVERD_DARWIN_FIXTURE_DIR
  GTEST_SKIP() << "Clang and ld64.lld Darwin fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const auto &[File, Profile] = GetParam();
  Path = (std::filesystem::path(NEVERD_DARWIN_FIXTURE_DIR) / File).string();
  const uint64_t Class =
      llvm::StringRef(File).ends_with("x86_64") ? 0x2000000 : 0;
  for (unsigned Variant = 0; Variant != 5; ++Variant) {
    const bool Created = Variant == 2 || Variant == 4, Missing = Variant >= 3;
    auto Files = emulation::darwin_test::directoryOwnershipFilesJSON(Created);
    if (Missing) {
      auto &M = *Files.getAsObject()->getObject("ownership_mutations");
      M["objects"] = llvm::json::Array{};
      M["created_directory_objects"] = false;
      // A regular creation grant cannot authorize the new directory.
      M["created_regular_objects"] = Created;
    }
    const auto Options =
        llvm::formatv("{0}",
                      llvm::json::Value(llvm::json::Object{
                          {"backend", "unicorn"},
                          {"instruction_quantum", 1024},
                          {"timeout_microseconds", 5000000},
                          {"arguments",
                           llvm::json::Array{
                               "guest",
                               Created        ? "created-directory-ownership"
                               : Variant == 0 ? "directory-ownership-mutations"
                                              : "virtual-directory-ownership",
                               Created ? "/child" : "/dir"}},
                          {"darwin_files", std::move(Files)},
                          {"darwin_system",
                           llvm::cantFail(llvm::json::parse(
                               emulation::darwin_test::ModeCredentialsJSON))}}))
            .str();
    auto Text = takeString(neverd_emulate_process_json(
        Session, Path.c_str(), Profile, Options.c_str()));
    ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
    auto Report = llvm::json::parse(Text);
    ASSERT_TRUE(bool(Report));
    const auto *O = Report->getAsObject();
    ASSERT_NE(O, nullptr);
    EXPECT_EQ(O->getString(field::Stop),
              Missing ? "unsupported_service" : "exited");
    EXPECT_EQ(O->getString(field::Stdout), Missing ? "" : "4f");
    EXPECT_EQ(O->getString(field::Stderr), "");
    const auto *Calls = O->getArray(field::Services);
    ASSERT_NE(Calls, nullptr);
    if (Missing) {
      EXPECT_EQ(O->getString(field::Diagnostic),
                "Darwin object ownership provider is not configured");
      ASSERT_EQ(Calls->size(), Created ? 6u : 5u);
      const auto *Last = Calls->back().getAsObject();
      ASSERT_NE(Last, nullptr);
      EXPECT_EQ(Last->getString(field::Number),
                llvm::utohexstr(Class | 16, true));
      EXPECT_EQ(*Last->get(field::Result), llvm::json::Value(nullptr));
      EXPECT_EQ(Last->get(field::Error), nullptr);
    } else {
      EXPECT_EQ(O->getInteger(field::ExitStatus), 55);
      const auto Wanted =
          emulation::darwin_test::directoryOwnershipServices(Created);
      ASSERT_EQ(Calls->size(), Wanted.size());
      for (unsigned I = 0; I != Wanted.size(); ++I) {
        const auto *E = (*Calls)[I].getAsObject();
        ASSERT_NE(E, nullptr);
        EXPECT_EQ(E->getString(field::Number),
                  llvm::utohexstr(Class | Wanted[I].Number, true));
        EXPECT_EQ(E->getString(field::Result),
                  llvm::utohexstr(Wanted[I].Value, true));
        EXPECT_EQ(E->getBoolean(field::Error), Wanted[I].Error);
      }
    }
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    EXPECT_EQ(runCLIRequest(Path, Profile, Options, Output),
              Missing ? process_cli::Incomplete : process_cli::GuestFailure);
    ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, *Report));
  }
#endif
}
INSTANTIATE_TEST_SUITE_P(
    Platforms, DarwinDirectoryOwnershipPublic,
    testing::Values(std::pair{"macos-arm64", MacOSMachO64},
                    std::pair{"macos-x86_64", MacOSMachO64},
                    std::pair{"ios-arm64", IOSMachO64},
                    std::pair{"ios-simulator-arm64", IOSSimulatorMachO64},
                    std::pair{"ios-simulator-x86_64", IOSSimulatorMachO64}),
    [](const testing::TestParamInfo<std::pair<const char *, const char *>>
           &Info) {
      std::string Name = Info.param.first;
      std::replace(Name.begin(), Name.end(), '-', '_');
      return Name;
    });

class DarwinDirectoryTimestampPublic
    : public ProcessPublic,
      public testing::WithParamInterface<
          std::pair<const char *, const char *>> {};
TEST_P(DarwinDirectoryTimestampPublic,
       PreserveCompleteDirectoryTimestampRecordsAcrossCAndCLI) {
#ifndef NEVERD_DARWIN_FIXTURE_DIR
  GTEST_SKIP() << "Clang and ld64.lld Darwin fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const auto &[File, Profile] = GetParam();
  Path = (std::filesystem::path(NEVERD_DARWIN_FIXTURE_DIR) / File).string();
  const uint64_t Class =
      llvm::StringRef(File).ends_with("x86_64") ? 0x2000000 : 0;
  for (unsigned Variant = 0; Variant != 5; ++Variant) {
    const bool Created = Variant == 2 || Variant == 4, Missing = Variant >= 3;
    auto Files = emulation::darwin_test::directoryTimestampFilesJSON(Created);
    if (Missing) {
      auto &M = *Files.getAsObject()->getObject("timestamp_mutations");
      M["objects"] = llvm::json::Array{};
      M["created_directory_objects"] = false;
      // A regular creation grant cannot authorize the new directory.
      M["created_regular_objects"] = Created;
    }
    const auto Options =
        llvm::formatv("{0}",
                      llvm::json::Value(llvm::json::Object{
                          {"backend", "unicorn"},
                          {"instruction_quantum", 1024},
                          {"timeout_microseconds", 5000000},
                          {"arguments",
                           llvm::json::Array{
                               "guest",
                               Created        ? "created-directory-timestamps"
                               : Variant == 0 ? "directory-timestamp-mutations"
                                              : "virtual-directory-timestamps",
                               Created ? "/child" : "/dir"}},
                          {"darwin_files", std::move(Files)},
                          {"darwin_time",
                           llvm::json::Object{
                               {"time_of_day",
                                llvm::json::Object{{"seconds", 21},
                                                   {"microseconds", 22}}}}},
                          {"darwin_system",
                           llvm::cantFail(llvm::json::parse(
                               emulation::darwin_test::ModeCredentialsJSON))}}))
            .str();
    auto Text = takeString(neverd_emulate_process_json(
        Session, Path.c_str(), Profile, Options.c_str()));
    ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
    auto Report = llvm::json::parse(Text);
    ASSERT_TRUE(bool(Report));
    const auto *O = Report->getAsObject();
    ASSERT_NE(O, nullptr);
    EXPECT_EQ(O->getString(field::Stop),
              Missing ? "unsupported_service" : "exited");
    EXPECT_EQ(O->getString(field::Stdout), Missing ? "" : "54");
    EXPECT_EQ(O->getString(field::Stderr), "");
    const auto *Calls = O->getArray(field::Services);
    ASSERT_NE(Calls, nullptr);
    if (Missing) {
      EXPECT_EQ(O->getString(field::Diagnostic),
                "Darwin object timestamp provider is not configured");
      ASSERT_EQ(Calls->size(), Created ? 6u : 5u);
      const auto *Last = Calls->back().getAsObject();
      ASSERT_NE(Last, nullptr);
      EXPECT_EQ(Last->getString(field::Number),
                llvm::utohexstr(Class | 138, true));
      EXPECT_EQ(*Last->get(field::Result), llvm::json::Value(nullptr));
      EXPECT_EQ(Last->get(field::Error), nullptr);
    } else {
      EXPECT_EQ(O->getInteger(field::ExitStatus), 54);
      const auto Wanted =
          emulation::darwin_test::directoryTimestampServices(Created);
      ASSERT_EQ(Calls->size(), Wanted.size());
      for (unsigned I = 0; I != Wanted.size(); ++I) {
        const auto *E = (*Calls)[I].getAsObject();
        ASSERT_NE(E, nullptr);
        EXPECT_EQ(E->getString(field::Number),
                  llvm::utohexstr(Class | Wanted[I].Number, true));
        EXPECT_EQ(E->getString(field::Result),
                  llvm::utohexstr(Wanted[I].Value, true));
        EXPECT_EQ(E->getBoolean(field::Error), Wanted[I].Error);
      }
    }
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    EXPECT_EQ(runCLIRequest(Path, Profile, Options, Output),
              Missing ? process_cli::Incomplete : process_cli::GuestFailure);
    ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, *Report));
  }
#endif
}
INSTANTIATE_TEST_SUITE_P(
    Platforms, DarwinDirectoryTimestampPublic,
    testing::Values(std::pair{"macos-arm64", MacOSMachO64},
                    std::pair{"macos-x86_64", MacOSMachO64},
                    std::pair{"ios-arm64", IOSMachO64},
                    std::pair{"ios-simulator-arm64", IOSSimulatorMachO64},
                    std::pair{"ios-simulator-x86_64", IOSSimulatorMachO64}),
    [](const testing::TestParamInfo<std::pair<const char *, const char *>>
           &Info) {
      std::string Name = Info.param.first;
      std::replace(Name.begin(), Name.end(), '-', '_');
      return Name;
    });

class DarwinDirectoryModePublic : public ProcessPublic,
                                  public testing::WithParamInterface<
                                      std::pair<const char *, const char *>> {};
TEST_P(DarwinDirectoryModePublic,
       PreserveLiteralDirectoryRecordsAcrossCAndCLI) {
#ifndef NEVERD_DARWIN_FIXTURE_DIR
  GTEST_SKIP() << "Clang and ld64.lld Darwin fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const auto &[File, Profile] = GetParam();
  Path = (std::filesystem::path(NEVERD_DARWIN_FIXTURE_DIR) / File).string();
  const uint64_t Class =
      llvm::StringRef(File).ends_with("x86_64") ? 0x2000000 : 0;
  for (unsigned Variant = 0; Variant != 5; ++Variant) {
    const bool Created = Variant == 2 || Variant == 4, Missing = Variant >= 3;
    auto Files = emulation::darwin_test::directoryModeFilesJSON(Created);
    if (Missing) {
      auto &M = *Files.getAsObject()->getObject("mode_mutations");
      M["objects"] = llvm::json::Array{};
      M["created_directory_objects"] = false;
      // A regular creation grant cannot authorize the new directory.
      M["created_regular_objects"] = Created;
    }
    const auto Options =
        llvm::formatv(
            "{0}",
            llvm::json::Value(llvm::json::Object{
                {"backend", "unicorn"},
                {"instruction_quantum", 1024},
                {"timeout_microseconds", 5000000},
                {"arguments",
                 llvm::json::Array{"guest",
                                   Created        ? "created-directory-mode"
                                   : Variant == 0 ? "directory-mode-mutations"
                                                  : "virtual-directory-mode",
                                   Created ? "/child" : "/dir"}},
                {"darwin_files", std::move(Files)},
                {"darwin_system",
                 llvm::cantFail(llvm::json::parse(
                     emulation::darwin_test::ModeCredentialsJSON))}}))
            .str();
    auto Text = takeString(neverd_emulate_process_json(
        Session, Path.c_str(), Profile, Options.c_str()));
    ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
    auto Report = llvm::json::parse(Text);
    ASSERT_TRUE(bool(Report));
    const auto *O = Report->getAsObject();
    ASSERT_NE(O, nullptr);
    EXPECT_EQ(O->getString(field::Stop),
              Missing ? "unsupported_service" : "exited");
    EXPECT_EQ(O->getString(field::Stdout), Missing ? "" : "51");
    EXPECT_EQ(O->getString(field::Stderr), "");
    const auto *Calls = O->getArray(field::Services);
    ASSERT_NE(Calls, nullptr);
    if (Missing) {
      EXPECT_EQ(O->getString(field::Diagnostic),
                "Darwin object mode provider is not configured");
      ASSERT_EQ(Calls->size(), Created ? 6u : 5u);
      const auto *Last = Calls->back().getAsObject();
      ASSERT_NE(Last, nullptr);
      EXPECT_EQ(Last->getString(field::Number),
                llvm::utohexstr(Class | 15, true));
      EXPECT_EQ(*Last->get(field::Result), llvm::json::Value(nullptr));
      EXPECT_EQ(Last->get(field::Error), nullptr);
    } else {
      EXPECT_EQ(O->getInteger(field::ExitStatus), 53);
      const auto Wanted =
          emulation::darwin_test::directoryModeServices(Created);
      ASSERT_EQ(Calls->size(), Wanted.size());
      for (unsigned I = 0; I != Wanted.size(); ++I) {
        const auto *E = (*Calls)[I].getAsObject();
        ASSERT_NE(E, nullptr);
        EXPECT_EQ(E->getString(field::Number),
                  llvm::utohexstr(Class | Wanted[I].Number, true));
        EXPECT_EQ(E->getString(field::Result),
                  llvm::utohexstr(Wanted[I].Value, true));
        EXPECT_EQ(E->getBoolean(field::Error), Wanted[I].Error);
      }
    }
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    EXPECT_EQ(runCLIRequest(Path, Profile, Options, Output),
              Missing ? process_cli::Incomplete : process_cli::GuestFailure);
    ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, *Report));
  }
#endif
}
INSTANTIATE_TEST_SUITE_P(
    Platforms, DarwinDirectoryModePublic,
    testing::Values(std::pair{"macos-arm64", MacOSMachO64},
                    std::pair{"macos-x86_64", MacOSMachO64},
                    std::pair{"ios-arm64", IOSMachO64},
                    std::pair{"ios-simulator-arm64", IOSSimulatorMachO64},
                    std::pair{"ios-simulator-x86_64", IOSSimulatorMachO64}),
    [](const testing::TestParamInfo<std::pair<const char *, const char *>>
           &Info) {
      std::string Name = Info.param.first;
      std::replace(Name.begin(), Name.end(), '-', '_');
      return Name;
    });

class DarwinMountEnumerationPublic
    : public ProcessPublic,
      public testing::WithParamInterface<
          std::pair<const char *, const char *>> {};
TEST_P(DarwinMountEnumerationPublic,
       PreserveCacheAndLiteralRecordsAcrossCAndCLI) {
#ifndef NEVERD_DARWIN_FIXTURE_DIR
  GTEST_SKIP() << "Clang and ld64.lld Darwin fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const auto &[File, Profile] = GetParam();
  Path = (std::filesystem::path(NEVERD_DARWIN_FIXTURE_DIR) / File).string();
  const uint64_t Class =
      llvm::StringRef(File).ends_with("x86_64") ? 0x2000000 : 0;
  for (unsigned Variant = 0; Variant != 3; ++Variant) {
    const bool Virtual = Variant == 1, Missing = Variant == 2;
    auto Files = llvm::cantFail(
        llvm::json::parse(emulation::darwin_test::mountEnumerationFilesJSON()));
    if (Missing)
      Files.getAsObject()->erase("filesystem_enumeration");
    const auto Options =
        llvm::formatv(
            "{0}", llvm::json::Value(llvm::json::Object{
                       {"backend", "unicorn"},
                       {"instruction_quantum", 1024},
                       {"timeout_microseconds", 5000000},
                       {"arguments",
                        llvm::json::Array{"guest",
                                          Virtual ? "virtual-mount-enumeration"
                                                  : "mount-enumeration"}},
                       {"darwin_files", std::move(Files)}}))
            .str();
    auto Text = takeString(neverd_emulate_process_json(
        Session, Path.c_str(), Profile, Options.c_str()));
    ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
    auto Report = llvm::json::parse(Text);
    ASSERT_TRUE(bool(Report));
    const auto *O = Report->getAsObject();
    ASSERT_NE(O, nullptr);
    EXPECT_EQ(O->getString(field::Stop),
              Missing ? "unsupported_service" : "exited");
    EXPECT_EQ(O->getString(field::Stderr), "");
    const auto *Calls = O->getArray(field::Services);
    ASSERT_NE(Calls, nullptr);
    if (!Missing) {
      const auto Wanted =
          emulation::darwin_test::mountEnumerationServices(Virtual);
      ASSERT_EQ(Calls->size(), Wanted.size());
      EXPECT_EQ(O->getInteger(field::ExitStatus), Virtual ? 53 : 52);
      EXPECT_EQ(
          O->getString(field::Stdout),
          Virtual ? llvm::toHex(
                        emulation::darwin_test::mountEnumerationOutput(), true)
                  : "4d");
      for (unsigned I = 0; I != Wanted.size(); ++I) {
        const auto *E = (*Calls)[I].getAsObject();
        ASSERT_NE(E, nullptr);
        const auto &W = Wanted[I];
        EXPECT_EQ(E->getString(field::Number),
                  llvm::utohexstr(Class | W.Number, true));
        EXPECT_EQ(E->getString(field::Result), llvm::utohexstr(W.Value, true));
        EXPECT_EQ(E->getBoolean(field::Error), W.Error);
        const auto *A = E->getArray(field::Arguments);
        ASSERT_NE(A, nullptr);
        ASSERT_EQ(A->size(), 6u);
        for (unsigned J = 0; J != 6; ++J)
          if (W.Arguments[J])
            EXPECT_EQ((*A)[J].getAsString(),
                      llvm::utohexstr(*W.Arguments[J], true));
      }
    } else {
      EXPECT_EQ(O->getString(field::Stdout), "");
      EXPECT_EQ(O->getString(field::Diagnostic),
                "Darwin mount enumeration environment is not configured");
      ASSERT_EQ(Calls->size(), 1u);
      const auto *Last = Calls->back().getAsObject();
      ASSERT_NE(Last, nullptr);
      EXPECT_EQ(*Last->get(field::Result), llvm::json::Value(nullptr));
      EXPECT_EQ(Last->get(field::Error), nullptr);
    }
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    EXPECT_EQ(runCLIRequest(Path, Profile, Options, Output),
              Missing ? process_cli::Incomplete : process_cli::GuestFailure);
    ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, *Report));
  }
#endif
}
INSTANTIATE_TEST_SUITE_P(
    Platforms, DarwinMountEnumerationPublic,
    testing::Values(std::pair{"macos-arm64", MacOSMachO64},
                    std::pair{"macos-x86_64", MacOSMachO64},
                    std::pair{"ios-arm64", IOSMachO64},
                    std::pair{"ios-simulator-arm64", IOSSimulatorMachO64},
                    std::pair{"ios-simulator-x86_64", IOSSimulatorMachO64}),
    [](const testing::TestParamInfo<std::pair<const char *, const char *>>
           &Info) {
      std::string Name = Info.param.first;
      std::replace(Name.begin(), Name.end(), '-', '_');
      return Name;
    });

TEST_F(ProcessPublic,
       DarwinTimestampMutationsKeepIndependentProvidersAcrossCAndCLI) {
#ifndef NEVERD_DARWIN_FIXTURE_DIR
  GTEST_SKIP() << "Clang and ld64.lld Darwin fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const auto Common = emulation::darwin_test::timestampServices(),
             Virtual = emulation::darwin_test::createdTimestampServices();
  const auto Bytes = emulation::darwin_test::createdTimestampRecords();
  const std::string Literal(reinterpret_cast<const char *>(Bytes.data()),
                            Bytes.size());
  for (const auto &[File, Profile] :
       {std::pair{"macos-arm64", MacOSMachO64},
        std::pair{"macos-x86_64", MacOSMachO64},
        std::pair{"ios-arm64", IOSMachO64},
        std::pair{"ios-simulator-arm64", IOSSimulatorMachO64},
        std::pair{"ios-simulator-x86_64", IOSSimulatorMachO64}}) {
    Path = (std::filesystem::path(NEVERD_DARWIN_FIXTURE_DIR) / File).string();
    const uint64_t Class =
        llvm::StringRef(File).ends_with("x86_64") ? 0x2000000 : 0;
    for (bool Created : {false, true})
      for (unsigned Variant = 0; Variant != (Created ? 4u : 3u); ++Variant) {
        auto Files = llvm::cantFail(
            llvm::json::parse(emulation::darwin_test::TimestampFilesJSON));
        auto &F = *Files.getAsObject();
        if (Created) {
          F["umask"] = 18;
          auto &Parent = *(*F.getArray("directories"))[0].getAsObject();
          Parent["mutable"] = true;
          Parent["mutation_policy"] = llvm::json::Object{
              {"directory_entry_size", 16},
              {"mutation_time",
               llvm::json::Object{{"seconds", 91}, {"nanoseconds", 92}}}};
          F["creation_policy"] = llvm::cantFail(llvm::json::parse(
              R"({"first_inode":100,"block_size":512,"generation":9,"creation_time":{"seconds":41,"nanoseconds":42},"mutation_policy":{"allocation_unit":512,"mutation_time":{"seconds":101,"nanoseconds":102}}})"));
          F["timestamp_mutations"] =
              llvm::json::Object{{"objects", llvm::json::Array{}},
                                 {"created_regular_objects", true}};
        }
        if (Variant == 1)
          F.erase("timestamp_mutations");
        if (Variant == 2)
          F["timestamp_mutations"] =
              llvm::json::Object{{"objects", llvm::json::Array{}}};
        llvm::json::Value Input = llvm::json::Object{
            {"backend", "unicorn"},
            {"instruction_quantum", 1024},
            {"timeout_microseconds", 5000000},
            {"arguments",
             llvm::json::Array{"guest",
                               Created ? "virtual-timestamp-creation"
                                       : "ordinary-timestamp-mutations",
                               "/data"}},
            {"darwin_system",
             llvm::cantFail(llvm::json::parse(
                 emulation::darwin_test::ModeCredentialsJSON))},
            {"darwin_files", std::move(Files)}};
        if (Variant != 3)
          (*Input.getAsObject())["darwin_time"] = llvm::json::Object{
              {"time_of_day",
               llvm::json::Object{{"seconds", 0}, {"microseconds", 0}}}};
        const auto Options = llvm::formatv("{0}", Input).str();
        auto Text = takeString(neverd_emulate_process_json(
            Session, Path.c_str(), Profile, Options.c_str()));
        ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
        auto Report = llvm::json::parse(Text);
        ASSERT_TRUE(bool(Report));
        const auto *O = Report->getAsObject();
        ASSERT_NE(O, nullptr);
        EXPECT_EQ(O->getString(field::Stop),
                  Variant ? "unsupported_service" : "exited");
        EXPECT_EQ(O->getString(field::Stderr), "");
        const auto *Calls = O->getArray(field::Services);
        ASSERT_NE(Calls, nullptr);
        if (!Variant) {
          const auto &Wanted = Created ? Virtual : Common;
          ASSERT_EQ(Calls->size(), Wanted.size());
          EXPECT_EQ(O->getInteger(field::ExitStatus), Created ? 49 : 48);
          EXPECT_EQ(O->getString(field::Stdout),
                    Created ? llvm::toHex(Literal, true) : "54");
          for (unsigned I = 0; I != Wanted.size(); ++I) {
            const auto *E = (*Calls)[I].getAsObject();
            ASSERT_NE(E, nullptr);
            const auto &W = Wanted[I];
            EXPECT_EQ(E->getString(field::Number),
                      llvm::utohexstr(Class | W.Number, true));
            EXPECT_EQ(E->getString(field::Result),
                      llvm::utohexstr(W.Value, true));
            EXPECT_EQ(E->getBoolean(field::Error), W.Error);
            const auto *A = E->getArray(field::Arguments);
            ASSERT_NE(A, nullptr);
            ASSERT_EQ(A->size(), 6u);
            if (W.Object)
              EXPECT_EQ((*A)[0].getAsString(),
                        llvm::utohexstr(*W.Object, true));
            if (W.Input)
              EXPECT_EQ((*A)[1].getAsString(), llvm::utohexstr(*W.Input, true));
            const auto N = W.Number & 0xffffff;
            if (N == 138 || N == 139)
              for (unsigned J = 2; J != 6; ++J)
                EXPECT_EQ((*A)[J].getAsString(), "ffffffffffffffff");
          }
        } else {
          ASSERT_EQ(Calls->size(), Variant == 3 ? 5u : Created ? 3u : 8u);
          const auto *Last = Calls->back().getAsObject();
          ASSERT_NE(Last, nullptr);
          EXPECT_EQ(*Last->get(field::Result), llvm::json::Value(nullptr));
          EXPECT_EQ(Last->get(field::Error), nullptr);
          EXPECT_EQ(
              O->getString(field::Diagnostic),
              Variant == 3 ? "Darwin time_of_day is not configured"
              : Variant == 1
                  ? "Darwin ordinary timestamp environment is not configured"
                  : "Darwin object timestamp provider is not "
                    "configured");
        }
        EXPECT_EQ(neverd_session_is_loaded(Session), 0);
        EXPECT_EQ(runCLIRequest(Path, Profile, Options, Output),
                  Variant ? process_cli::Incomplete
                          : process_cli::GuestFailure);
        ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, *Report));
      }
  }
#endif
}

TEST_F(
    ProcessPublic,
    DarwinFullBarrierSynchronizationPreservesIndependentProvidersAcrossCAndCLI) {
#ifndef NEVERD_DARWIN_FIXTURE_DIR
  GTEST_SKIP() << "Clang and ld64.lld Darwin fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const auto Common = emulation::darwin_test::fullSynchronizationServices();
  const auto Virtual =
      emulation::darwin_test::createdFullSynchronizationServices();
  const auto Bytes = emulation::darwin_test::createdFullSynchronizationRecord();
  std::string Literal(reinterpret_cast<const char *>(Bytes.data()),
                      Bytes.size());
  Literal += Literal;
  for (const auto &[File, Profile] :
       {std::pair{"macos-arm64", MacOSMachO64},
        std::pair{"macos-x86_64", MacOSMachO64},
        std::pair{"ios-arm64", IOSMachO64},
        std::pair{"ios-simulator-arm64", IOSSimulatorMachO64},
        std::pair{"ios-simulator-x86_64", IOSSimulatorMachO64}}) {
    Path = (std::filesystem::path(NEVERD_DARWIN_FIXTURE_DIR) / File).string();
    const uint64_t Class =
        llvm::StringRef(File).ends_with("x86_64") ? 0x2000000 : 0;
    for (bool Created : {false, true})
      for (unsigned Variant = 0; Variant != 5; ++Variant) {
        auto Files = llvm::cantFail(llvm::json::parse(
            emulation::darwin_test::FullSynchronizationFilesJSON));
        auto &F = *Files.getAsObject();
        if (Created) {
          F["umask"] = 18;
          auto &Parent = *(*F.getArray("directories"))[0].getAsObject();
          Parent["mutable"] = true;
          Parent["mutation_policy"] = llvm::json::Object{
              {"directory_entry_size", 16},
              {"mutation_time",
               llvm::json::Object{{"seconds", 91}, {"nanoseconds", 92}}}};
          F["creation_policy"] = llvm::cantFail(llvm::json::parse(
              R"({"first_inode":100,"block_size":512,"generation":9,"creation_time":{"seconds":41,"nanoseconds":42},"mutation_policy":{"allocation_unit":512,"mutation_time":{"seconds":101,"nanoseconds":102}}})"));
          for (const char *P :
               {"full_synchronization", "barrier_synchronization"}) {
            (*F.getObject(P))["objects"] = llvm::json::Array{};
            (*F.getObject(P))["created_regular_objects"] = true;
          }
        }
        const char *Selected =
            Variant <= 2 ? "full_synchronization" : "barrier_synchronization";
        if (Variant == 1 || Variant == 3)
          F.erase(Selected);
        if (Variant == 2 || Variant == 4) {
          (*F.getObject(Selected))["objects"] = llvm::json::Array{};
          (*F.getObject(Selected))["created_regular_objects"] = false;
        }
        llvm::json::Value Input = llvm::json::Object{
            {"backend", "unicorn"},
            {"instruction_quantum", 1024},
            {"timeout_microseconds", 5000000},
            {"arguments",
             llvm::json::Array{"guest",
                               Created
                                   ? "virtual-full-barrier-creation"
                                   : "ordinary-full-barrier-synchronization",
                               "/data"}},
            {"darwin_system",
             llvm::cantFail(llvm::json::parse(
                 emulation::darwin_test::ModeCredentialsJSON))},
            {"darwin_files", std::move(Files)}};
        const auto Options = llvm::formatv("{0}", Input).str();
        auto Text = takeString(neverd_emulate_process_json(
            Session, Path.c_str(), Profile, Options.c_str()));
        ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
        auto Report = llvm::json::parse(Text);
        ASSERT_TRUE(bool(Report));
        const auto *O = Report->getAsObject();
        ASSERT_NE(O, nullptr);
        EXPECT_EQ(O->getString(field::Stop),
                  Variant ? "unsupported_service" : "exited");
        EXPECT_EQ(O->getString(field::Stderr), "");
        const auto *Calls = O->getArray(field::Services);
        ASSERT_NE(Calls, nullptr);
        if (!Variant) {
          const auto Wanted =
              Created ? llvm::ArrayRef(Virtual) : llvm::ArrayRef(Common);
          ASSERT_EQ(Calls->size(), Wanted.size());
          EXPECT_EQ(O->getInteger(field::ExitStatus), Created ? 47 : 46);
          EXPECT_EQ(O->getString(field::Stdout),
                    Created ? llvm::toHex(Literal, true) : "46");
          for (unsigned I = 0; I != Wanted.size(); ++I) {
            const auto *E = (*Calls)[I].getAsObject();
            ASSERT_NE(E, nullptr);
            const auto &W = Wanted[I];
            EXPECT_EQ(E->getString(field::Number),
                      llvm::utohexstr(Class | W.Number, true));
            EXPECT_EQ(E->getString(field::Result),
                      llvm::utohexstr(W.Value, true));
            EXPECT_EQ(E->getBoolean(field::Error), W.Error);
            const auto *Args = E->getArray(field::Arguments);
            ASSERT_NE(Args, nullptr);
            ASSERT_EQ(Args->size(), 6u);
            if (W.FD)
              EXPECT_EQ((*Args)[0].getAsString(), llvm::utohexstr(*W.FD, true));
            if (W.Command)
              EXPECT_EQ((*Args)[1].getAsString(),
                        llvm::utohexstr(*W.Command, true));
            if (W.Argument)
              EXPECT_EQ((*Args)[2].getAsString(),
                        llvm::utohexstr(*W.Argument, true));
          }
        } else {
          const bool Full = Variant <= 2,
                     Missing = Variant == 1 || Variant == 3;
          const unsigned Prefix = Created ? (Full ? 2 : 3) : (Full ? 6 : 17);
          ASSERT_EQ(Calls->size(), Prefix + 1);
          const auto *Last = Calls->back().getAsObject();
          ASSERT_NE(Last, nullptr);
          EXPECT_EQ(*Last->get(field::Result), llvm::json::Value(nullptr));
          EXPECT_EQ(Last->get(field::Error), nullptr);
          EXPECT_EQ(O->getString(field::Diagnostic),
                    std::string("Darwin ") +
                        (Missing
                             ? (Full ? "full synchronization environment is "
                                       "not configured"
                                     : "barrier synchronization environment is "
                                       "not configured")
                             : (Full ? "regular-object full synchronization "
                                       "provider is not configured"
                                     : "regular-object barrier synchronization "
                                       "provider is not configured")));
        }
        EXPECT_EQ(neverd_session_is_loaded(Session), 0);
        EXPECT_EQ(runCLIRequest(Path, Profile, Options, Output),
                  Variant ? process_cli::Incomplete
                          : process_cli::GuestFailure);
        ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, *Report));
      }
  }
#endif
}

TEST_F(ProcessPublic,
       DarwinOrdinaryFileSynchronizationPreservesProviderAcrossCAndCLI) {
#ifndef NEVERD_DARWIN_FIXTURE_DIR
  GTEST_SKIP() << "Clang and ld64.lld Darwin fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (const auto &[File, Profile] :
       {std::pair{"macos-arm64", MacOSMachO64},
        std::pair{"macos-x86_64", MacOSMachO64},
        std::pair{"ios-arm64", IOSMachO64},
        std::pair{"ios-simulator-arm64", IOSSimulatorMachO64},
        std::pair{"ios-simulator-x86_64", IOSSimulatorMachO64}}) {
    Path = (std::filesystem::path(NEVERD_DARWIN_FIXTURE_DIR) / File).string();
    const uint64_t Class =
        llvm::StringRef(File).ends_with("x86_64") ? 0x2000000 : 0;
    for (bool Granted : {true, false}) {
      llvm::json::Value Input = llvm::json::Object{
          {"backend", "unicorn"},
          {"instruction_quantum", 1024},
          {"timeout_microseconds", 5000000},
          {"arguments",
           llvm::json::Array{"guest", "ordinary-file-synchronization",
                             "/data"}},
          {"darwin_files",
           llvm::cantFail(llvm::json::parse(
               emulation::darwin_test::SynchronizationFilesJSON))}};
      if (!Granted)
        (*Input.getAsObject()
              ->getObject(field::DarwinFiles)
              ->getObject(field::FileSynchronization))
            [field::FileSynchronizationObjects] = llvm::json::Array{};
      const auto Options = llvm::formatv("{0}", Input).str();
      auto Text = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), Profile, Options.c_str()));
      ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
      auto Report = llvm::json::parse(Text);
      ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError());
      const auto *Object = Report->getAsObject();
      ASSERT_NE(Object, nullptr);
      EXPECT_EQ(Object->getString(field::Stop),
                Granted ? "exited" : "unsupported_service");
      EXPECT_EQ(Object->getString(field::Stdout), Granted ? "53" : "");
      EXPECT_EQ(Object->getString(field::Stderr), "");
      const auto *Calls = Object->getArray(field::Services);
      ASSERT_NE(Calls, nullptr);
      ASSERT_EQ(Calls->size(), Granted ? 68u : 6u);
      if (Granted) {
        EXPECT_EQ(Object->getInteger(field::ExitStatus), 40);
        constexpr uint64_t High[] = {0, 0x1234567800000000ULL,
                                     0xffffffff00000000ULL},
                           FDs[] = {3, 5, 4, 0xffffffffULL},
                           Values[] = {0, 0, 0, 9};
        for (unsigned H = 0; H != 3; ++H)
          for (unsigned N = 0; N != 2; ++N)
            for (unsigned I = 0; I != 4; ++I) {
              const auto *E = (*Calls)[5 + H * 18 + N * 9 + I].getAsObject();
              ASSERT_NE(E, nullptr);
              EXPECT_EQ(
                  E->getString(field::Number),
                  llvm::utohexstr(Class | High[H] | (N ? 408 : 95), true));
              EXPECT_EQ(E->getString(field::Result),
                        llvm::utohexstr(Values[I], true));
              EXPECT_EQ(E->getBoolean(field::Error), I == 3);
              const auto *A = E->getArray(field::Arguments);
              ASSERT_NE(A, nullptr);
              ASSERT_EQ(A->size(), 6u);
              EXPECT_EQ((*A)[0].getAsString(),
                        llvm::utohexstr(High[H] | FDs[I], true));
            }
        constexpr uint64_t TailNumbers[] = {6, 95, 95, 6, 408, 408, 339, 6, 4},
                           TailValues[] = {0, 9, 0, 0, 9, 0, 0, 0, 1};
        for (unsigned I = 0; I != 9; ++I) {
          const auto *E = (*Calls)[59 + I].getAsObject();
          ASSERT_NE(E, nullptr);
          EXPECT_EQ(E->getString(field::Number),
                    llvm::utohexstr(Class | TailNumbers[I], true));
          EXPECT_EQ(E->getString(field::Result),
                    llvm::utohexstr(TailValues[I], true));
          EXPECT_EQ(E->getBoolean(field::Error), I == 1 || I == 4);
        }
      } else {
        EXPECT_EQ(Object->getString(field::Diagnostic),
                  "Darwin regular-object file integrity completion provider is "
                  "not configured");
        const auto *E = Calls->back().getAsObject();
        ASSERT_NE(E, nullptr);
        EXPECT_EQ(E->getString(field::Number),
                  llvm::utohexstr(Class | 95, true));
        EXPECT_EQ(*E->get(field::Result), llvm::json::Value(nullptr));
        EXPECT_EQ(E->get(field::Error), nullptr);
      }
      EXPECT_EQ(neverd_session_is_loaded(Session), 0);
      EXPECT_EQ(runCLIRequest(Path, Profile, Options, Output),
                Granted ? process_cli::GuestFailure : process_cli::Incomplete);
      ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, *Report));
    }
  }
  for (const char *Completion :
       {R"({})", R"({"objects":[],"extra":true})", R"({"objects":"unknown"})",
        R"({"objects":[3]})", R"({"objects":[{"path":"/data"}]})",
        R"({"objects":["/data","/data"]})", R"({"objects":["relative"]})",
        R"({"objects":["/missing"]})", R"({"objects":null})",
        R"({"objects":["/data\u0000"]})"}) {
    auto Input = llvm::cantFail(llvm::json::parse(
        std::string(R"({"darwin_files":)") +
        emulation::darwin_test::SynchronizationFilesJSON + "}"));
    (*Input.getAsObject()->getObject(
        field::DarwinFiles))[field::FileSynchronization] =
        llvm::cantFail(llvm::json::parse(Completion));
    const auto Options = llvm::formatv("{0}", Input).str();
    EXPECT_TRUE(takeString(neverd_emulate_process_json(Session, Path.c_str(),
                                                       IOSSimulatorMachO64,
                                                       Options.c_str()))
                    .empty());
    EXPECT_FALSE(takeString(neverd_last_error(Session)).empty());
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  }
#endif
}

struct DarwinAttributeProfile {
  const char *File, *Profile;
};
void PrintTo(const DarwinAttributeProfile &Case, std::ostream *OS) {
  *OS << Case.File;
}
class DarwinCreatedAttributesPublic
    : public ProcessPublic,
      public testing::WithParamInterface<DarwinAttributeProfile> {};

TEST_P(DarwinCreatedAttributesPublic, PreserveProvidersAcrossCAndCLI) {
#ifndef NEVERD_DARWIN_FIXTURE_DIR
  GTEST_SKIP() << "Clang and ld64.lld Darwin fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const auto &[File, Profile] = GetParam();
  Path = (std::filesystem::path(NEVERD_DARWIN_FIXTURE_DIR) / File).string();
  const uint64_t Class =
      llvm::StringRef(File).ends_with("x86_64") ? 0x2000000 : 0;
  for (bool Virtual : {false, true})
    for (unsigned Variant = 0; Variant != 9; ++Variant) {
      const bool Owner = Variant > 4;
      const unsigned Change = Owner ? Variant - 4 : Variant;
      const char *Purpose = Owner ? "ownership_mutations" : "mode_mutations";
      auto Files = llvm::cantFail(
          llvm::json::parse(emulation::darwin_test::CreatedAttributeFilesJSON));
      if (Variant) {
        auto &Provider = *Files.getAsObject()->getObject(Purpose);
        if (Change == 1 || Change == 4)
          Provider["created_regular_objects"] = false;
        if (Change == 2)
          Provider.erase("created_regular_objects");
        if (Change == 3)
          Files.getAsObject()->erase(Purpose);
        if (Change == 4)
          Provider["objects"] = llvm::json::Array{llvm::json::Object{
              {"path", "/data"},
              {"time",
               llvm::json::Object{{"seconds", 71}, {"nanoseconds", 72}}}}};
      }
      llvm::json::Value Input = llvm::json::Object{
          {"backend", "unicorn"},
          {"instruction_quantum", 1024},
          {"timeout_microseconds", 5000000},
          {"arguments",
           llvm::json::Array{"guest",
                             Virtual ? "virtual-created-regular-attributes"
                                     : "created-regular-attributes",
                             "/data"}},
          {"darwin_files", std::move(Files)},
          {"darwin_system",
           llvm::cantFail(llvm::json::parse(
               emulation::darwin_test::CreatedAttributeCredentialsJSON))}};
      const auto Options = llvm::formatv("{0}", Input).str();
      auto Text = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), Profile, Options.c_str()));
      ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
      auto Report = llvm::json::parse(Text);
      ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError());
      const auto *Object = Report->getAsObject();
      ASSERT_NE(Object, nullptr);
      EXPECT_EQ(Object->getString(field::Stop),
                Variant ? "unsupported_service" : "exited");
      EXPECT_EQ(Object->getString(field::Stdout),
                Variant ? ""
                : Virtual
                    ? emulation::darwin_test::VirtualCreatedAttributeStdoutHex
                    : "41");
      EXPECT_EQ(Object->getString(field::Stderr), "");
      const auto *Calls = Object->getArray(field::Services);
      ASSERT_NE(Calls, nullptr);
      const auto Wanted =
          Virtual ? llvm::ArrayRef(
                        emulation::darwin_test::VirtualCreatedAttributeServices)
                  : llvm::ArrayRef(
                        emulation::darwin_test::CreatedAttributeServices);
      const unsigned Count =
          Variant ? (Virtual ? (Owner ? 4 : 3) : (Owner ? 52 : 12))
                  : Wanted.size();
      ASSERT_EQ(Calls->size(), Count);
      for (unsigned I = 0; I != Count; ++I) {
        const auto *E = (*Calls)[I].getAsObject();
        ASSERT_NE(E, nullptr);
        EXPECT_EQ(E->getString(field::Number),
                  llvm::utohexstr(Class | Wanted[I].Number, true));
        if (Variant && I + 1 == Count) {
          EXPECT_EQ(*E->get(field::Result), llvm::json::Value(nullptr));
          EXPECT_EQ(E->get(field::Error), nullptr);
        } else {
          EXPECT_EQ(E->getString(field::Result),
                    llvm::utohexstr(Wanted[I].Value, true));
          EXPECT_EQ(E->getBoolean(field::Error), Wanted[I].Error);
          if (Wanted[I].FD) {
            const auto *Args = E->getArray(field::Arguments);
            ASSERT_NE(Args, nullptr);
            EXPECT_EQ((*Args)[0].getAsString(),
                      llvm::utohexstr(*Wanted[I].FD, true));
          }
        }
      }
      if (!Variant)
        EXPECT_EQ(Object->getInteger(field::ExitStatus), Virtual ? 45 : 44);
      else
        EXPECT_EQ(Object->getString(field::Diagnostic),
                  Owner ? (Change == 3 ? "Darwin ordinary ownership "
                                         "environment is not configured"
                                       : "Darwin object ownership "
                                         "provider is not configured")
                        : (Change == 3 ? "Darwin ordinary mode mutation "
                                         "environment is not configured"
                                       : "Darwin object mode "
                                         "provider is not configured"));
      EXPECT_EQ(neverd_session_is_loaded(Session), 0);
      EXPECT_EQ(runCLIRequest(Path, Profile, Options, Output),
                Variant ? process_cli::Incomplete : process_cli::GuestFailure);
      ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, *Report));
    }
#endif
}
INSTANTIATE_TEST_SUITE_P(
    Platforms, DarwinCreatedAttributesPublic,
    testing::Values(
        DarwinAttributeProfile{"macos-arm64", MacOSMachO64},
        DarwinAttributeProfile{"macos-x86_64", MacOSMachO64},
        DarwinAttributeProfile{"ios-arm64", IOSMachO64},
        DarwinAttributeProfile{"ios-simulator-arm64", IOSSimulatorMachO64},
        DarwinAttributeProfile{"ios-simulator-x86_64", IOSSimulatorMachO64}),
    [](const testing::TestParamInfo<DarwinAttributeProfile> &Info) {
      std::string Name = Info.param.File;
      std::replace(Name.begin(), Name.end(), '-', '_');
      return Name;
    });

TEST_F(ProcessPublic,
       DarwinCreatedRegularAttributesRejectMalformedProvidersBeforeLoading) {
#ifndef NEVERD_DARWIN_FIXTURE_DIR
  GTEST_SKIP() << "Clang and ld64.lld Darwin fixtures unavailable";
#else
  Path = (std::filesystem::path(NEVERD_DARWIN_FIXTURE_DIR) /
          "ios-simulator-x86_64")
             .string();
  for (const char *Purpose : {"mode_mutations", "ownership_mutations"})
    for (const char *Bad : {"null", "1", "\"true\"", "[]", "{}"}) {
      auto Files = llvm::cantFail(
          llvm::json::parse(emulation::darwin_test::CreatedAttributeFilesJSON));
      (*Files.getAsObject()->getObject(Purpose))["created_regular_objects"] =
          llvm::cantFail(llvm::json::parse(Bad));
      const auto Options =
          llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                                   {"darwin_files", std::move(Files)}}))
              .str();
      EXPECT_TRUE(takeString(neverd_emulate_process_json(Session, Path.c_str(),
                                                         IOSSimulatorMachO64,
                                                         Options.c_str()))
                      .empty());
      EXPECT_FALSE(takeString(neverd_last_error(Session)).empty());
      EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    }
#endif
}

TEST_F(
    ProcessPublic,
    DarwinCreatedRegularAdvisoryLocksPreserveProviderAndStatusAcrossCAndCLI) {
#ifndef NEVERD_DARWIN_FIXTURE_DIR
  GTEST_SKIP() << "Clang and ld64.lld Darwin fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (const auto &[File, Profile] :
       {std::pair{"macos-arm64", MacOSMachO64},
        std::pair{"macos-x86_64", MacOSMachO64},
        std::pair{"ios-arm64", IOSMachO64},
        std::pair{"ios-simulator-arm64", IOSSimulatorMachO64},
        std::pair{"ios-simulator-x86_64", IOSSimulatorMachO64}}) {
    Path = (std::filesystem::path(NEVERD_DARWIN_FIXTURE_DIR) / File).string();
    const uint64_t Class =
        llvm::StringRef(File).ends_with("x86_64") ? 0x2000000 : 0;
    for (unsigned Variant = 0; Variant != 5; ++Variant) {
      auto Files = llvm::cantFail(
          llvm::json::parse(emulation::darwin_test::CreatedAdvisoryFilesJSON));
      auto &Provider =
          *Files.getAsObject()->getObject(field::FileAdvisoryLocks);
      if (Variant == 1)
        Provider[field::FileSynchronizationCreatedRegularObjects] = false;
      if (Variant == 2)
        Provider.erase(field::FileSynchronizationCreatedRegularObjects);
      if (Variant == 3)
        Files.getAsObject()->erase(field::FileAdvisoryLocks);
      if (Variant == 4) {
        Provider[field::FileSynchronizationCreatedRegularObjects] = false;
        Provider[field::FileAdvisoryLockObjects] = llvm::json::Array{"/data"};
      }
      llvm::json::Value Input = llvm::json::Object{
          {"backend", "unicorn"},
          {"instruction_quantum", 1024},
          {"timeout_microseconds", 5000000},
          {"arguments",
           llvm::json::Array{"guest", "created-regular-advisory-locks",
                             "/data"}},
          {"darwin_files", std::move(Files)}};
      const auto Options = llvm::formatv("{0}", Input).str();
      auto Text = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), Profile, Options.c_str()));
      ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
      auto Report = llvm::json::parse(Text);
      ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError());
      const auto *Object = Report->getAsObject();
      ASSERT_NE(Object, nullptr);
      EXPECT_EQ(Object->getString(field::Stop),
                Variant == 0 ? "exited" : "unsupported_service");
      EXPECT_EQ(Object->getString(field::Stdout), Variant == 0 ? "4c" : "");
      EXPECT_EQ(Object->getString(field::Stderr), "");
      const auto *Calls = Object->getArray(field::Services);
      ASSERT_NE(Calls, nullptr);
      ASSERT_EQ(Calls->size(), Variant == 0 ? 131u : 11u);
      if (Variant == 0) {
        EXPECT_EQ(Object->getInteger(field::ExitStatus), 43);
        for (unsigned I = 0;
             I != std::size(emulation::darwin_test::CreatedAdvisoryServices);
             ++I) {
          SCOPED_TRACE(I);
          const auto &Wanted =
              emulation::darwin_test::CreatedAdvisoryServices[I];
          const auto *E = (*Calls)[I].getAsObject();
          ASSERT_NE(E, nullptr);
          EXPECT_EQ(E->getString(field::Number),
                    llvm::utohexstr(Class | Wanted.Number, true));
          EXPECT_EQ(E->getString(field::Result),
                    llvm::utohexstr(Wanted.Value, true));
          EXPECT_EQ(E->getBoolean(field::Error), Wanted.Error);
          const auto *Args = E->getArray(field::Arguments);
          ASSERT_NE(Args, nullptr);
          if (Wanted.FD != UINT64_MAX)
            EXPECT_EQ((*Args)[0].getAsString(),
                      llvm::utohexstr(Wanted.FD, true));
          if (Wanted.How != UINT64_MAX)
            EXPECT_EQ((*Args)[1].getAsString(),
                      llvm::utohexstr(Wanted.How, true));
        }
      } else {
        const auto *E = Calls->back().getAsObject();
        ASSERT_NE(E, nullptr);
        EXPECT_EQ(E->getString(field::Number),
                  llvm::utohexstr(Class | 131u, true));
        EXPECT_EQ(*E->get(field::Result), llvm::json::Value(nullptr));
        EXPECT_EQ(E->get(field::Error), nullptr);
      }
      EXPECT_EQ(neverd_session_is_loaded(Session), 0);
      EXPECT_EQ(runCLIRequest(Path, Profile, Options, Output),
                Variant == 0 ? process_cli::GuestFailure
                             : process_cli::Incomplete);
      ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, *Report));
    }
  }
  for (const char *Bad : {"null", "1", "\"true\"", "[]", "{}"}) {
    auto Files = llvm::cantFail(
        llvm::json::parse(emulation::darwin_test::CreatedAdvisoryFilesJSON));
    (*Files.getAsObject()->getObject(field::FileAdvisoryLocks))
        [field::FileSynchronizationCreatedRegularObjects] =
            llvm::cantFail(llvm::json::parse(Bad));
    const auto Options =
        llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                                 {"darwin_files", std::move(Files)}}))
            .str();
    EXPECT_TRUE(takeString(neverd_emulate_process_json(Session, Path.c_str(),
                                                       IOSSimulatorMachO64,
                                                       Options.c_str()))
                    .empty());
    EXPECT_FALSE(takeString(neverd_last_error(Session)).empty());
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  }
#endif
}

TEST_F(ProcessPublic,
       DarwinCreatedRegularCompletionPreservesProvidersAcrossCAndCLI) {
#ifndef NEVERD_DARWIN_FIXTURE_DIR
  GTEST_SKIP() << "Clang and ld64.lld Darwin fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (const auto &[File, Profile] :
       {std::pair{"macos-arm64", MacOSMachO64},
        std::pair{"macos-x86_64", MacOSMachO64},
        std::pair{"ios-arm64", IOSMachO64},
        std::pair{"ios-simulator-arm64", IOSSimulatorMachO64},
        std::pair{"ios-simulator-x86_64", IOSSimulatorMachO64}}) {
    Path = (std::filesystem::path(NEVERD_DARWIN_FIXTURE_DIR) / File).string();
    const uint64_t Class =
        llvm::StringRef(File).ends_with("x86_64") ? 0x2000000 : 0;
    for (unsigned Variant = 0; Variant != 5; ++Variant) {
      auto Files = llvm::cantFail(llvm::json::parse(
          emulation::darwin_test::CreatedSynchronizationFilesJSON));
      const bool FileGranted = Variant == 0 || Variant == 1,
                 DataGranted = Variant == 0 || Variant == 2;
      (*Files.getAsObject()->getObject(field::FileSynchronization))
          [field::FileSynchronizationCreatedRegularObjects] = FileGranted;
      (*Files.getAsObject()->getObject(field::FileDataSynchronization))
          [field::FileSynchronizationCreatedRegularObjects] = DataGranted;
      if (Variant == 4) {
        Files.getAsObject()->erase(field::FileSynchronization);
        Files.getAsObject()->erase(field::FileDataSynchronization);
      }
      llvm::json::Value Input = llvm::json::Object{
          {"backend", "unicorn"},
          {"instruction_quantum", 1024},
          {"timeout_microseconds", 5000000},
          {"arguments",
           llvm::json::Array{"guest", "created-regular-synchronization",
                             "/data"}},
          {"darwin_files", std::move(Files)}};
      const auto Options = llvm::formatv("{0}", Input).str();
      auto Text = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), Profile, Options.c_str()));
      ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
      auto Report = llvm::json::parse(Text);
      ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError());
      const auto *Object = Report->getAsObject();
      ASSERT_NE(Object, nullptr);
      EXPECT_EQ(Object->getString(field::Stop),
                Variant == 0 ? "exited" : "unsupported_service");
      EXPECT_EQ(Object->getString(field::Stdout), Variant == 0 ? "43" : "");
      EXPECT_EQ(Object->getString(field::Stderr), "");
      const auto *Calls = Object->getArray(field::Services);
      ASSERT_NE(Calls, nullptr);
      ASSERT_EQ(Calls->size(), Variant == 0 ? 147u : Variant == 1 ? 34u : 10u);
      if (Variant == 0) {
        EXPECT_EQ(Object->getInteger(field::ExitStatus), 42);
        for (unsigned I = 0; I != 9; ++I) {
          const auto *E = (*Calls)[I].getAsObject();
          ASSERT_NE(E, nullptr);
          EXPECT_EQ(E->getString(field::Number),
                    llvm::utohexstr(
                        Class | emulation::darwin_test::
                                    CreatedSynchronizationPrefixNumbers[I],
                        true));
          EXPECT_EQ(
              E->getString(field::Result),
              llvm::utohexstr(
                  emulation::darwin_test::CreatedSynchronizationPrefixValues[I],
                  true));
          EXPECT_EQ(E->getBoolean(field::Error), false);
        }
        constexpr uint64_t Highs[] = {0, 0x1234567800000000ULL,
                                      0xffffffff00000000ULL},
                           Numbers[] = {95, 408, 187},
                           FDs[] = {3, 5, 10, 20, 4, 0xffffffffULL};
        for (unsigned H = 0; H != 3; ++H)
          for (unsigned N = 0; N != 3; ++N)
            for (unsigned I = 0; I != 12; ++I) {
              const auto *E = (*Calls)[9 + H * 36 + N * 12 + I].getAsObject();
              ASSERT_NE(E, nullptr);
              const uint64_t Number = I < 6    ? Highs[H] | Numbers[N]
                                      : I < 9  ? 339
                                      : I < 11 ? 199
                                               : 92,
                             Value = I < 5     ? 0
                                     : I == 5  ? 9
                                     : I < 9   ? 0
                                     : I == 9  ? 10
                                     : I == 10 ? 3
                                               : 0x10002;
              EXPECT_EQ(E->getString(field::Number),
                        llvm::utohexstr(Class | Number, true));
              EXPECT_EQ(E->getString(field::Result),
                        llvm::utohexstr(Value, true));
              EXPECT_EQ(E->getBoolean(field::Error), I == 5);
              if (I < 6) {
                const auto *A = E->getArray(field::Arguments);
                ASSERT_NE(A, nullptr);
                EXPECT_EQ((*A)[0].getAsString(),
                          llvm::utohexstr(Highs[H] | FDs[I], true));
              }
            }
        for (unsigned I = 0; I != 30; ++I) {
          const auto *E = (*Calls)[117 + I].getAsObject();
          ASSERT_NE(E, nullptr);
          const auto Value =
              emulation::darwin_test::CreatedSynchronizationTailValues[I];
          EXPECT_EQ(
              E->getString(field::Number),
              llvm::utohexstr(Class | emulation::darwin_test::
                                          CreatedSynchronizationTailNumbers[I],
                              true));
          EXPECT_EQ(E->getString(field::Result), llvm::utohexstr(Value, true));
          EXPECT_EQ(E->getBoolean(field::Error), Value == 9);
        }
      } else {
        const auto *E = Calls->back().getAsObject();
        ASSERT_NE(E, nullptr);
        EXPECT_EQ(E->getString(field::Number),
                  llvm::utohexstr(Class | (Variant == 1 ? 187u : 95u), true));
        EXPECT_EQ(*E->get(field::Result), llvm::json::Value(nullptr));
        EXPECT_EQ(E->get(field::Error), nullptr);
      }
      EXPECT_EQ(neverd_session_is_loaded(Session), 0);
      EXPECT_EQ(runCLIRequest(Path, Profile, Options, Output),
                Variant == 0 ? process_cli::GuestFailure
                             : process_cli::Incomplete);
      ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, *Report));
    }
  }
  for (const auto Purpose :
       {field::FileSynchronization, field::FileDataSynchronization})
    for (const char *Bad : {"null", "1", "\"true\"", "[]", "{}"}) {
      auto Files = llvm::cantFail(llvm::json::parse(
          emulation::darwin_test::CreatedSynchronizationFilesJSON));
      (*Files.getAsObject()->getObject(
          Purpose))[field::FileSynchronizationCreatedRegularObjects] =
          llvm::cantFail(llvm::json::parse(Bad));
      const auto Options =
          llvm::formatv("{{\"darwin_files\":{0}}}", Files).str();
      EXPECT_TRUE(takeString(neverd_emulate_process_json(Session, Path.c_str(),
                                                         IOSSimulatorMachO64,
                                                         Options.c_str()))
                      .empty());
      EXPECT_FALSE(takeString(neverd_last_error(Session)).empty());
      EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    }
#endif
}

TEST_F(ProcessPublic,
       DarwinOrdinaryDataSynchronizationPreservesProviderAcrossCAndCLI) {
#ifndef NEVERD_DARWIN_FIXTURE_DIR
  GTEST_SKIP() << "Clang and ld64.lld Darwin fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (const auto &[File, Profile] :
       {std::pair{"macos-arm64", MacOSMachO64},
        std::pair{"macos-x86_64", MacOSMachO64},
        std::pair{"ios-arm64", IOSMachO64},
        std::pair{"ios-simulator-arm64", IOSSimulatorMachO64},
        std::pair{"ios-simulator-x86_64", IOSSimulatorMachO64}}) {
    Path = (std::filesystem::path(NEVERD_DARWIN_FIXTURE_DIR) / File).string();
    const uint64_t Class =
        llvm::StringRef(File).ends_with("x86_64") ? 0x2000000 : 0;
    for (bool Granted : {true, false}) {
      llvm::json::Value Input = llvm::json::Object{
          {"backend", "unicorn"},
          {"instruction_quantum", 1024},
          {"timeout_microseconds", 5000000},
          {"arguments",
           llvm::json::Array{"guest", "ordinary-data-synchronization",
                             "/data"}},
          {"darwin_files",
           llvm::cantFail(llvm::json::parse(
               emulation::darwin_test::DataSynchronizationFilesJSON))}};
      if (!Granted)
        (*Input.getAsObject()
              ->getObject(field::DarwinFiles)
              ->getObject(field::FileDataSynchronization))
            [field::FileDataSynchronizationObjects] = llvm::json::Array{};
      const auto Options = llvm::formatv("{0}", Input).str();
      auto Text = takeString(neverd_emulate_process_json(
          Session, Path.c_str(), Profile, Options.c_str()));
      ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
      auto Report = llvm::json::parse(Text);
      ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError());
      const auto *Object = Report->getAsObject();
      ASSERT_NE(Object, nullptr);
      EXPECT_EQ(Object->getString(field::Stop),
                Granted ? "exited" : "unsupported_service");
      EXPECT_EQ(Object->getString(field::Stdout), Granted ? "44" : "");
      EXPECT_EQ(Object->getString(field::Stderr), "");
      const auto *Calls = Object->getArray(field::Services);
      ASSERT_NE(Calls, nullptr);
      ASSERT_EQ(Calls->size(), Granted ? 41u : 6u);
      if (Granted) {
        EXPECT_EQ(Object->getInteger(field::ExitStatus), 41);
        constexpr uint64_t High[] = {0, 0x1234567800000000ULL,
                                     0xffffffff00000000ULL},
                           FDs[] = {3, 5, 4, 0xffffffffULL},
                           Values[] = {0, 0, 0, 9};
        for (unsigned H = 0; H != 3; ++H)
          for (unsigned I = 0; I != 4; ++I) {
            const auto *E = (*Calls)[5 + H * 9 + I].getAsObject();
            ASSERT_NE(E, nullptr);
            EXPECT_EQ(E->getString(field::Number),
                      llvm::utohexstr(Class | High[H] | 187, true));
            EXPECT_EQ(E->getString(field::Result),
                      llvm::utohexstr(Values[I], true));
            EXPECT_EQ(E->getBoolean(field::Error), I == 3);
            const auto *A = E->getArray(field::Arguments);
            ASSERT_NE(A, nullptr);
            ASSERT_EQ(A->size(), 6u);
            EXPECT_EQ((*A)[0].getAsString(),
                      llvm::utohexstr(High[H] | FDs[I], true));
          }
        constexpr uint64_t TailNumbers[] = {6,   187, 187, 6, 187,
                                            187, 339, 6,   4},
                           TailValues[] = {0, 9, 0, 0, 9, 0, 0, 0, 1};
        for (unsigned I = 0; I != 9; ++I) {
          const auto *E = (*Calls)[32 + I].getAsObject();
          ASSERT_NE(E, nullptr);
          EXPECT_EQ(E->getString(field::Number),
                    llvm::utohexstr(Class | TailNumbers[I], true));
          EXPECT_EQ(E->getString(field::Result),
                    llvm::utohexstr(TailValues[I], true));
          EXPECT_EQ(E->getBoolean(field::Error), I == 1 || I == 4);
        }
      } else {
        EXPECT_EQ(Object->getString(field::Diagnostic),
                  "Darwin regular-object data integrity completion provider is "
                  "not configured");
        const auto *E = Calls->back().getAsObject();
        ASSERT_NE(E, nullptr);
        EXPECT_EQ(E->getString(field::Number),
                  llvm::utohexstr(Class | 187, true));
        EXPECT_EQ(*E->get(field::Result), llvm::json::Value(nullptr));
        EXPECT_EQ(E->get(field::Error), nullptr);
      }
      EXPECT_EQ(neverd_session_is_loaded(Session), 0);
      EXPECT_EQ(runCLIRequest(Path, Profile, Options, Output),
                Granted ? process_cli::GuestFailure : process_cli::Incomplete);
      ASSERT_NO_FATAL_FAILURE(expectCLIReport(Output, *Report));
    }
  }
  for (const char *Completion :
       {R"({})", R"({"objects":[],"extra":true})", R"({"objects":"unknown"})",
        R"({"objects":[3]})", R"({"objects":[{"path":"/data"}]})",
        R"({"objects":["/data","/data"]})", R"({"objects":["relative"]})",
        R"({"objects":["/missing"]})", R"({"objects":null})",
        R"({"objects":["/data\u0000"]})"}) {
    auto Input = llvm::cantFail(llvm::json::parse(
        std::string(R"({"darwin_files":)") +
        emulation::darwin_test::DataSynchronizationFilesJSON + "}"));
    (*Input.getAsObject()->getObject(
        field::DarwinFiles))[field::FileDataSynchronization] =
        llvm::cantFail(llvm::json::parse(Completion));
    const auto Options = llvm::formatv("{0}", Input).str();
    EXPECT_TRUE(takeString(neverd_emulate_process_json(Session, Path.c_str(),
                                                       IOSSimulatorMachO64,
                                                       Options.c_str()))
                    .empty());
    EXPECT_FALSE(takeString(neverd_last_error(Session)).empty());
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  }
#endif
}

} // namespace
