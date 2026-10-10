//===- ProcessPublicTests.cpp - Real ELF through the shared SDK and CLI --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../TestProcess.h"
#include "DarwinEntropyTestData.h"
#include "DarwinFileTestData.h"
#include "DarwinSystemTestData.h"
#include "DarwinTimeTestData.h"
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

// Pass the original JSON as one argument. cmd.exe splits embedded newlines
// even inside shell quotes and also expands percent signs in guest strings.
int runCLIRequest(llvm::StringRef Path, llvm::StringRef Profile,
                  llvm::StringRef Options, llvm::StringRef Output) {
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
  const llvm::StringRef Args[] = {NEVERD_PROCESS_CLI, process_cli::Command,
                                  Path, ProfileArg, OptionsArg};
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Output,
                                                      llvm::StringRef("")};
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
  const bool Entropy = ModeName.starts_with("entropy-");
  const bool UnknownEntropy = Entropy && ModeName != "entropy-replay";
  const bool Incomplete = UnknownThreadIdentity || UnknownEntropy ||
                          ProtectedLink || UnknownPathConf ||
                          UnknownAttributes || UnknownXattrs || UnknownNames ||
                          UnknownBulk || UnknownXattrMutation ||
                          UnknownHardLinkName || UnknownNonblocking;
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
  if (llvm::StringRef(Mode).starts_with("mach-")) {
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
        "files_filesystem_status", "files_filesystem_status_bionic"}) {
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
} // namespace
