//===- AndroidNativeTests.cpp - Authored Android shared library workloads ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/Limits.h"
#include "neverd/emulation/ProcessReport.h"
#include "neverd/emulation/ProcessSession.h"
#include "neverd/loader/ELF/ELFLoader.h"
#include "neverd/loader/ELF/ELFProgramLinking.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

namespace neverd::emulation {
namespace {
namespace output_fixture {
#define NEVERD_LINUX_OUTPUT_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_LINUX_OUTPUT_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/LinuxOutputCases.def"
#undef NEVERD_LINUX_OUTPUT_TEXT
#undef NEVERD_LINUX_OUTPUT_VALUE
} // namespace output_fixture
constexpr uint64_t Buffer = 0x20000000;
class AndroidNative : public testing::TestWithParam<const char *> {
protected:
  std::filesystem::path Path;
  ProcessOptions Options;
  void SetUp() override {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
    GTEST_SKIP() << "Clang and ld.lld Android fixtures unavailable";
#else
    Path = std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
           (std::string(GetParam()) + ".so");
    Options.Backend = ExecutionBackendKind::Unicorn;
    ExecutionConfiguration Configuration;
    Configuration.Backend = Options.Backend;
    Configuration.Architecture = GuestArchitecture::AArch64;
    Configuration.Contract = ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(Configuration);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available)
      GTEST_SKIP() << Probe->Reason;
    Options.Android.emplace();
    Options.Android->TraceLimit = 4096;
    Options.Android->Memory.push_back({Buffer, 4096, {}, false});
    Options.Android->ReadMemory.push_back({Buffer, 64});
    Options.InstructionQuantum = 3;
#endif
  }
  ProcessResult run(llvm::StringRef Entry,
                    std::vector<uint64_t> Arguments = {}) {
    Options.Android->EntrySymbol = Entry.str();
    Options.Android->Arguments = std::move(Arguments);
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    if (!R) {
      ADD_FAILURE() << llvm::toString(R.takeError());
      return {ProcessProfile::AndroidNativeAArch64,
              GuestArchitecture::AArch64,
              Options.Backend,
              {}};
    }
    return std::move(*R);
  }
  void returned(const ProcessResult &Result, uint64_t Value) {
    ASSERT_EQ(Result.Stop, ProcessStopReason::Returned) << Result.Diagnostic;
    EXPECT_EQ(Result.ReturnValue, Value);
    EXPECT_FALSE(Result.ExitStatus);
    EXPECT_EQ(Result.Trace.size(), Result.Instructions);
    EXPECT_FALSE(Result.TraceTruncated);
  }
  std::vector<uint8_t> original() {
    auto B = llvm::MemoryBuffer::getFile(Path.string());
    EXPECT_TRUE(bool(B));
    llvm::StringRef Bytes = (*B)->getBuffer();
    return {Bytes.bytes_begin(), Bytes.bytes_end()};
  }
  template <class F> void temporary(llvm::ArrayRef<uint8_t> Bytes, F Callback) {
    int FD;
    llvm::SmallString<128> File;
    ASSERT_FALSE(
        llvm::sys::fs::createTemporaryFile("neverd-android", "so", FD, File));
    auto Cleanup = llvm::make_scope_exit([&] { llvm::sys::fs::remove(File); });
    {
      llvm::raw_fd_ostream OS(FD, true);
      OS.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    }
    auto Saved = Path;
    Path = File.str().str();
    Callback();
    Path = Saved;
  }
};
class AndroidOnce : public AndroidNative {};
TEST_P(AndroidOnce, ConstructorsNestedImportsAndRepeatedCalls) {
  auto R = run("once_values", {Buffer});
  returned(R, 73);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  const std::vector<uint32_t> Expected = {2, 2, 1, 1, 1, 1, 2, 1000,
                                          1, 2, 1, 0, 0, 0, 0, 0};
  for (size_t I = 0; I < Expected.size(); ++I)
    EXPECT_EQ(llvm::support::endian::read32le(
                  R.MemorySnapshots[0].Bytes.data() + I * 4),
              Expected[I]);
  const std::vector<std::string> Names = {"pthread_once", "pthread_once",
                                          "pthread_once", "pthread_once",
                                          "getuid",       "pthread_once"};
  ASSERT_EQ(R.NativeCalls.size(), Names.size());
  for (size_t I = 0; I < Names.size(); ++I) {
    EXPECT_EQ(R.NativeCalls[I].Name, Names[I]);
    EXPECT_EQ(R.NativeCalls[I].Result, I == 4 ? 1000u : 0u);
  }
  EXPECT_TRUE(R.Services.empty());
  Options.Android->Initialize = false;
  R = run("once_values", {Buffer});
  returned(R, 73);
  EXPECT_EQ(
      llvm::support::endian::read32le(R.MemorySnapshots[0].Bytes.data() + 32),
      0u);
}
TEST_P(AndroidOnce, CompletedControlNeedsNoCallbackOrWriteAccess) {
  Options.Android->Initialize = false;
  Options.Android->Memory[0].Bytes = {2, 0, 0, 0};
  auto R = run("once_supplied", {Buffer, 0});
  returned(R, 0);
  ASSERT_EQ(R.NativeCalls.size(), 1u);
  EXPECT_EQ(R.NativeCalls[0].Result, 0u);
  returned(run("once_readonly", {Buffer, 2}), 0);
  R = run("once_readonly", {Buffer, 0});
  EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure);
  EXPECT_NE(R.Diagnostic.find("invalid guest pointer"), std::string::npos);
  EXPECT_FALSE(R.NativeCalls.back().Result);
}
TEST_P(AndroidOnce, DynamicCallbacksRetainTheirProviderAndEvent) {
  Options.Android->Initialize = false;
  Options.Android->Libraries["libinit.so"] = {"pthread_once"};
  auto R = run("once_dynamic", {Buffer});
  returned(R, 0);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  ASSERT_EQ(R.MemorySnapshots[0].Bytes.size(), 64u);
  const std::vector<uint32_t> Expected = {2, 2, 1, 1, 1, 1, 2, 1000};
  for (size_t I = 0; I < Expected.size(); ++I)
    EXPECT_EQ(llvm::support::endian::read32le(
                  R.MemorySnapshots[0].Bytes.data() + I * 4),
              Expected[I]);
  ASSERT_EQ(R.NativeCalls.size(), 7u);
  EXPECT_EQ(R.NativeCalls[1].Name, "dlsym");
  EXPECT_EQ(R.NativeCalls[1].Symbol, "pthread_once");
  for (size_t I : {2u, 5u}) {
    EXPECT_EQ(R.NativeCalls[I].Name, "pthread_once");
    EXPECT_EQ(R.NativeCalls[I].Library, "libinit.so");
    EXPECT_EQ(R.NativeCalls[I].PC, R.NativeCalls[1].Result);
    EXPECT_EQ(R.NativeCalls[I].Result, 0u);
  }
  EXPECT_EQ(R.NativeCalls[3].Name, "pthread_once");
  EXPECT_TRUE(R.NativeCalls[3].Library.empty());
  EXPECT_EQ(R.NativeCalls[3].Result, 0u);
  EXPECT_EQ(R.NativeCalls[4].Name, "getuid");
  EXPECT_EQ(R.NativeCalls[4].Result, 1000u);
}
TEST_P(AndroidOnce, DefaultScopeCallbacksKeepResidentProviderAfterClose) {
  Options.Android->Initialize = false;
  Options.Android->Libraries["libinit.so"] = {"pthread_once"};
  auto R = run("once_default", {Buffer});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  Options.Android->DefaultScope = std::vector<std::string>{};
  returned(run("once_default", {Buffer}), 99);
  Options.Android->DefaultScope = std::vector<std::string>{"libinit.so"};
  R = run("once_default", {Buffer});
  returned(R, 0);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  const std::vector<uint32_t> Expected = {2, 2, 1, 1, 1, 1, 2, 1000};
  for (size_t I = 0; I < Expected.size(); ++I)
    EXPECT_EQ(llvm::support::endian::read32le(
                  R.MemorySnapshots[0].Bytes.data() + I * 4),
              Expected[I]);
  ASSERT_EQ(R.NativeCalls.size(), 7u);
  EXPECT_EQ(R.NativeCalls[0].Name, "dlsym");
  EXPECT_EQ(R.NativeCalls[0].Library, "libinit.so");
  EXPECT_EQ(R.NativeCalls[0].Symbol, "pthread_once");
  EXPECT_EQ(R.NativeCalls[2].Name, "dlclose");
  EXPECT_EQ(R.NativeCalls[2].Result, 0u);
  for (size_t I : {3u, 6u}) {
    EXPECT_EQ(R.NativeCalls[I].Name, "pthread_once");
    EXPECT_EQ(R.NativeCalls[I].Library, "libinit.so");
    EXPECT_EQ(R.NativeCalls[I].PC, R.NativeCalls[0].Result);
    EXPECT_EQ(R.NativeCalls[I].Result, 0u);
  }
  EXPECT_EQ(R.NativeCalls[4].Name, "pthread_once");
  EXPECT_TRUE(R.NativeCalls[4].Library.empty());
  EXPECT_EQ(R.NativeCalls[4].Result, 0u);
  EXPECT_EQ(R.NativeCalls[5].Name, "getuid");
  EXPECT_EQ(R.NativeCalls[5].Result, 1000u);
}
TEST_P(AndroidOnce, InvalidInputsCannotCompleteInitialization) {
  Options.Android->Initialize = false;
  for (auto Args : {std::vector<uint64_t>{0, Buffer},
                    {Buffer + 1, Buffer},
                    {Buffer, 0},
                    {Buffer, Buffer},
                    {Buffer, Buffer + 1}}) {
    auto R = run("once_supplied", Args);
    EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
    EXPECT_FALSE(R.ReturnValue);
    ASSERT_EQ(R.NativeCalls.size(), 1u);
    EXPECT_FALSE(R.NativeCalls[0].Result);
  }
  for (uint32_t State : {1u, 3u, 0xffffffffu}) {
    Options.Android->Memory[0].Bytes.resize(4);
    llvm::support::endian::write32le(Options.Android->Memory[0].Bytes.data(),
                                     State);
    auto R = run("once_supplied", {Buffer, 0});
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_FALSE(R.NativeCalls[0].Result);
    EXPECT_EQ(
        llvm::support::endian::read32le(R.MemorySnapshots[0].Bytes.data()),
        State);
  }
}
TEST_P(AndroidOnce, RecursiveAndFailedCallbacksRemainIncomplete) {
  Options.Android->Initialize = false;
  for (const char *Entry : {"once_recursive", "once_fails"}) {
    auto R = run(Entry, {Buffer});
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
    EXPECT_FALSE(R.ReturnValue);
    EXPECT_EQ(
        llvm::support::endian::read32le(R.MemorySnapshots[0].Bytes.data()), 1u);
    for (const auto &Call : R.NativeCalls)
      EXPECT_FALSE(Call.Result);
    if (std::string(Entry) == "once_fails") {
      EXPECT_EQ(llvm::support::endian::read32le(
                    R.MemorySnapshots[0].Bytes.data() + 4),
                1u);
      EXPECT_EQ(R.NativeCalls.back().Name, "unknown_initializer");
    } else {
      EXPECT_NE(R.Diagnostic.find("already underway"), std::string::npos);
    }
  }
  auto R = run("once_bad_stack", {Buffer});
  EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure);
  EXPECT_NE(R.Diagnostic.find("did not restore its stack"), std::string::npos);
  EXPECT_FALSE(R.NativeCalls.back().Result);
}
TEST_P(AndroidOnce, CallbackConsumesTheOriginalInstructionBudget) {
  Options.Android->Initialize = false;
  Options.Limits.Instructions = 300;
  auto R = run("once_exhausts", {Buffer});
  EXPECT_EQ(R.Stop, ProcessStopReason::InstructionLimit) << R.Diagnostic;
  EXPECT_EQ(R.Instructions, 300u);
  EXPECT_FALSE(R.ReturnValue);
  ASSERT_EQ(R.NativeCalls.size(), 1u);
  EXPECT_FALSE(R.NativeCalls[0].Result);
  EXPECT_EQ(llvm::support::endian::read32le(R.MemorySnapshots[0].Bytes.data()),
            1u);
  EXPECT_GT(
      llvm::support::endian::read32le(R.MemorySnapshots[0].Bytes.data() + 8),
      0u);
}
INSTANTIATE_TEST_SUITE_P(OptimizationAndPacking, AndroidOnce,
                         testing::Values("once-O0-none", "once-O0-android",
                                         "once-O0-relr", "once-O2-none",
                                         "once-O2-android", "once-O2-relr"));

TEST_P(AndroidNative, ConstructorsStackArgumentsAndUninitializedAnalysis) {
  returned(run("add_arguments", {1, 2, 3, 4, 5, 6, 7, 8, 9, 10}), 402);
  Options.Android->Initialize = false;
  returned(run("add_arguments", {1, 2, 3, 4, 5, 6, 7, 8, 9, 10}), 390);
}
TEST_P(AndroidNative, PropertiesMemoryAndTLS) {
  Options.Android->Properties["test.device"] = "sample";
  Options.Android->Properties["ro.build.version.sdk"] = "28";
  auto R = run("properties", {Buffer});
  returned(R, 28);
  ASSERT_EQ(R.MemorySnapshots.size(), 1);
  EXPECT_EQ(std::string(R.MemorySnapshots[0].Bytes.begin(),
                        R.MemorySnapshots[0].Bytes.begin() + 7),
            std::string("sample\0", 7));
  EXPECT_GE(R.NativeCalls.size(), 5);
  returned(run("tls_slots"), 1);
  R = run("absent_property", {Buffer});
  returned(R, 0);
  ASSERT_EQ(R.MemorySnapshots.size(), 1);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes[0], 0);
}
TEST_P(AndroidNative, DeviceAPILevelUsesOnlyTheSuppliedProperty) {
  Options.Android->Libraries["libplatform.so"] = {
      "android_get_device_api_level"};
  for (uint64_t Mode : {0, 1}) {
    const std::pair<const char *, int32_t> Cases[] = {{nullptr, -1},
                                                      {"", -1},
                                                      {"28", 28},
                                                      {"33", 33},
                                                      {" \t\n+0035suffix", 35},
                                                      {"2147483647", INT32_MAX},
                                                      {"0", -1},
                                                      {"-1", -1},
                                                      {"-2147483648", -1},
                                                      {"no-digits", -1},
                                                      {"+", -1},
                                                      {"+ 33", -1}};
    for (auto [Text, Expected] : Cases) {
      SCOPED_TRACE(Text ? Text : "absent");
      Options.Android->Properties.clear();
      if (Text)
        Options.Android->Properties["ro.build.version.sdk"] = Text;
      auto R = run("device_api_level", {Buffer, Mode});
      returned(R, static_cast<uint64_t>(static_cast<int64_t>(Expected)));
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      for (unsigned Offset : {0, 8}) {
        EXPECT_EQ(llvm::support::endian::read32le(
                      R.MemorySnapshots[0].Bytes.data() + Offset),
                  static_cast<uint32_t>(Expected));
        EXPECT_EQ(llvm::support::endian::read32le(
                      R.MemorySnapshots[0].Bytes.data() + Offset + 4),
                  77u);
      }
    }
  }
}
TEST_P(AndroidNative, DeviceAPILevelRejectsOverflowAndClosedProviders) {
  Options.Android->Libraries["libplatform.so"] = {
      "android_get_device_api_level"};
  for (const char *Text :
       {"2147483648", "-2147483649", "99999999999999999999"}) {
    Options.Android->Properties["ro.build.version.sdk"] = Text;
    for (uint64_t Mode : {0, 1}) {
      auto R = run("device_api_level", {Buffer, Mode});
      EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
      EXPECT_NE(R.Diagnostic.find("out-of-range decimal"), std::string::npos);
      ASSERT_FALSE(R.NativeCalls.empty());
      EXPECT_FALSE(R.NativeCalls.back().Result);
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, std::vector<uint8_t>(64));
    }
  }
  Options.Android->Properties["ro.build.version.sdk"] = "33";
  auto R = run("device_api_level", {Buffer, 2});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_FALSE(R.NativeCalls.back().Result);
  EXPECT_EQ(R.NativeCalls.back().Library, "libplatform.so");
}
TEST_P(AndroidNative, KernelErrorsAndLibcErrnoHaveDifferentContracts) {
  returned(run("libc_error"), 0);
  auto R = run("raw_error");
  returned(R, 0);
  ASSERT_EQ(R.Services.size(), 1);
  EXPECT_EQ(R.Services[0].Result, uint64_t(0) - 9);
}
TEST_P(AndroidNative, PageSizeUsesGuestLayoutAndPreservesErrno) {
  auto R = run("page_size", {Buffer});
  returned(R, 4096);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  std::vector<uint8_t> Expected(64);
  llvm::support::endian::write32le(Expected.data(), 4096);
  llvm::support::endian::write32le(Expected.data() + 4, 77);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  auto Call = llvm::find_if(
      R.NativeCalls, [](const auto &E) { return E.Name == "getpagesize"; });
  ASSERT_NE(Call, R.NativeCalls.end());
  EXPECT_EQ(Call->Result, 4096u);
  EXPECT_TRUE(Call->Library.empty());
  EXPECT_TRUE(R.Services.empty());
}
TEST_P(AndroidNative, SysconfPageAliasesShareGuestLayoutAndAllocationSize) {
  auto R = run("page_queries", {Buffer});
  returned(R, 0);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  std::vector<uint8_t> Expected(64);
  const uint64_t Values[] = {4096, 4096, 4096, 77, 0, 0x5a};
  for (unsigned I = 0; I < std::size(Values); ++I)
    llvm::support::endian::write64le(Expected.data() + I * 8, Values[I]);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  unsigned Queries = 0;
  for (const auto &Call : R.NativeCalls)
    if (Call.Name == "sysconf") {
      EXPECT_EQ(Call.Arguments[0], 0x27u + Queries);
      EXPECT_EQ(Call.Result, 4096u);
      EXPECT_TRUE(Call.Library.empty());
      ++Queries;
    }
  EXPECT_EQ(Queries, 2u);
  for (uint64_t Name :
       {0x27ULL, 0x28ULL, 0xfedcba9800000027ULL, 0xfedcba9800000028ULL}) {
    SCOPED_TRACE(Name);
    returned(run("page_query", {Name}), 4096);
  }
}
TEST_P(AndroidNative, UnmodeledSysconfNamesDoNotFabricateHostConfiguration) {
  for (uint64_t Name : {0ULL, 0x60ULL, 0xffffffffULL}) {
    auto R = run("page_query", {Name});
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
    EXPECT_NE(R.Diagnostic.find("sysconf selector"), std::string::npos);
    EXPECT_FALSE(R.ReturnValue);
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_EQ(R.NativeCalls.back().Name, "sysconf");
    EXPECT_FALSE(R.NativeCalls.back().Result);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, std::vector<uint8_t>(64));
  }
}
TEST_P(AndroidNative, DynamicSysconfKeepsNamesErrnoAndProviderLifetime) {
  Options.Android->Libraries["libpages.so"] = {"sysconf"};
  for (unsigned Name : {0x27, 0x28}) {
    SCOPED_TRACE(Name);
    auto R = run("dynamic_page_query", {Buffer, 0, Name});
    returned(R, 0);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    std::vector<uint8_t> Expected(64);
    llvm::support::endian::write64le(Expected.data(), 4096);
    llvm::support::endian::write64le(Expected.data() + 8, 77);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
    auto Lookup = llvm::find_if(R.NativeCalls, [](const auto &E) {
      return E.Name == "dlsym" && E.Symbol == "sysconf";
    });
    ASSERT_NE(Lookup, R.NativeCalls.end());
    auto Call = llvm::find_if(
        R.NativeCalls, [](const auto &E) { return E.Name == "sysconf"; });
    ASSERT_NE(Call, R.NativeCalls.end());
    EXPECT_EQ(Call->PC, Lookup->Result);
    EXPECT_EQ(Call->Library, "libpages.so");
    EXPECT_EQ(Call->Result, 4096u);
    R = run("dynamic_page_query", {Buffer, 1, Name});
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
    EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_EQ(R.NativeCalls.back().Name, "sysconf");
    EXPECT_FALSE(R.NativeCalls.back().Result);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  }
}
TEST_P(AndroidNative, DynamicPageSizeRetainsNamesAndProviderLifetime) {
  returned(run("dynamic_page_size", {Buffer, 0}), 100);
  Options.Android->Libraries["libpages.so"] = {};
  returned(run("dynamic_page_size", {Buffer, 0}), 101);
  Options.Android->Libraries["libpages.so"] = {"getpagesize"};
  auto R = run("dynamic_page_size", {Buffer, 0});
  returned(R, 0);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  std::vector<uint8_t> Expected(64);
  llvm::support::endian::write32le(Expected.data(), 4096);
  llvm::support::endian::write32le(Expected.data() + 4, 77);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  auto Lookup = llvm::find_if(R.NativeCalls, [](const auto &E) {
    return E.Name == "dlsym" && E.Symbol == "getpagesize";
  });
  ASSERT_NE(Lookup, R.NativeCalls.end());
  ASSERT_TRUE(Lookup->Result);
  EXPECT_EQ(Lookup->Library, "libpages.so");
  auto Call = llvm::find_if(
      R.NativeCalls, [](const auto &E) { return E.Name == "getpagesize"; });
  ASSERT_NE(Call, R.NativeCalls.end());
  EXPECT_EQ(Call->PC, *Lookup->Result);
  EXPECT_EQ(Call->Library, "libpages.so");
  EXPECT_EQ(Call->Result, 4096u);
  EXPECT_TRUE(R.Services.empty());
  R = run("dynamic_page_size", {Buffer, 1});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_EQ(R.NativeCalls.back().Name, "getpagesize");
  EXPECT_EQ(R.NativeCalls.back().Library, "libpages.so");
  EXPECT_FALSE(R.NativeCalls.back().Result);
  Options.Android->DefaultScope = std::vector<std::string>{"libpages.so"};
  R = run("dynamic_page_size", {Buffer, 1});
  returned(R, 0);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  llvm::support::endian::write32le(Expected.data() + 8, 4096);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
}
TEST_P(AndroidNative, MemoryAdviceSharesRawAndBionicRangeErrors) {
  struct Case {
    uint64_t Address, Length, Advice, Error;
  };
  const Case Cases[] = {{Buffer, 4095, 12, 0},
                        {Buffer, 1, 13, 0},
                        {Buffer, 1, (1ULL << 32) | 12, 0},
                        {Buffer + 1, 0, 12, 22},
                        {Buffer, UINT64_MAX, 12, 22},
                        {Buffer, 4097, 12, 12},
                        {0, 0, 12, 0},
                        {0, 1, 13, 12}};
  for (uint64_t Route = 0; Route < 3; ++Route) {
    for (const auto &C : Cases) {
      SCOPED_TRACE(Route);
      SCOPED_TRACE(C.Address);
      SCOPED_TRACE(C.Length);
      const uint64_t Value =
          C.Error ? (Route == 1 ? 0 - C.Error : UINT64_MAX) : 0;
      const uint64_t Errno = C.Error && Route != 1 ? C.Error : 77;
      auto R =
          run("memory_advice", {Buffer, C.Address, C.Length, C.Advice, Route});
      returned(R, Value);
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      std::vector<uint8_t> Expected(64);
      llvm::support::endian::write64le(Expected.data(), Value);
      llvm::support::endian::write64le(Expected.data() + 8, Errno);
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
      if (Route == 1) {
        ASSERT_EQ(R.Services.size(), 1u);
        EXPECT_EQ(R.Services[0].Number, 233u);
        EXPECT_EQ(R.Services[0].Result, Value);
      } else {
        EXPECT_TRUE(R.Services.empty());
        auto Call = llvm::find_if(R.NativeCalls, [&](const auto &E) {
          return E.Name == (Route ? "syscall" : "madvise");
        });
        ASSERT_NE(Call, R.NativeCalls.end());
        EXPECT_EQ(Call->Result, Value);
        EXPECT_TRUE(Call->Library.empty());
      }
    }
  }
}
TEST_P(AndroidNative, MemoryAdviceRejectsUnknownBackingContentChanges) {
  for (uint64_t Route = 0; Route < 3; ++Route) {
    auto R = run("memory_advice", {Buffer, Buffer, 4096, 4, Route});
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_FALSE(R.ReturnValue);
    EXPECT_NE(R.Diagnostic.find("madvise"), std::string::npos);
    if (Route == 1) {
      ASSERT_EQ(R.Services.size(), 1u);
      EXPECT_FALSE(R.Services[0].Result);
    } else {
      ASSERT_FALSE(R.NativeCalls.empty());
      EXPECT_EQ(R.NativeCalls.back().Name, Route ? "syscall" : "madvise");
      EXPECT_FALSE(R.NativeCalls.back().Result);
    }
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, std::vector<uint8_t>(64));
  }
}
TEST_P(AndroidNative, DiscardPagesSharesRawBionicAndDynamicEffects) {
  Options.OutputLimit = 4 * 1024 * 1024;
  Options.Android->Initialize = false;
  Options.Android->Memory[0].Size = 16384;
  Options.Android->ReadMemory = {{Buffer, 16384}};
  Options.Android->Libraries["libadvice.so"] = {"madvise"};
  Options.Android->TraceLimit = 500000;
  Options.Limits.Instructions = 500000;
  Options.Limits.TimeoutMicroseconds = 30000000;
  for (uint64_t Route = 0; Route != 4; ++Route) {
    for (uint64_t Mode = 0; Mode != 7; ++Mode) {
      SCOPED_TRACE(Route);
      SCOPED_TRACE(Mode);
      std::vector<uint8_t> Expected(16384, 0xa5);
      Options.Android->Memory[0].Bytes = Expected;
      const uint64_t Error = Mode == 2 ? 12 : Mode == 3 || Mode == 4 ? 22 : 0;
      const uint64_t Value = Error ? (Route == 1 ? 0 - Error : UINT64_MAX) : 0;
      const uint64_t Errno = Error && Route != 1 ? Error : 733;
      auto R = run("discard_pages", {Buffer, Route, Mode});
      returned(R, Value);
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      const uint64_t Header[] = {73, 0x100000000, Value, Errno, 0};
      for (size_t I = 0; I != 5; ++I)
        llvm::support::endian::write64le(Expected.data() + I * 8, Header[I]);
      for (size_t I = 0; I != 12288; ++I) {
        uint8_t Byte = uint8_t(0x31 + I / 4096);
        if (Mode == 1 || (Mode == 2 && (I < 4096 || I >= 8192)) ||
            ((Mode == 0 || Mode == 6) && I >= 4096 && I < 8192))
          Byte = 0;
        if (Mode == 2 && I >= 4096 && I < 8192)
          Byte = 0xcc;
        Expected[256 + I] = Byte;
      }
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
      if (Route == 1) {
        ASSERT_EQ(R.Services.size(), 1u);
        EXPECT_EQ(R.Services[0].Number, 233u);
        EXPECT_EQ(R.Services[0].Result, Value);
      } else {
        auto C = llvm::find_if(R.NativeCalls, [&](const auto &Call) {
          return Call.Name == (Route == 2 ? "syscall" : "madvise");
        });
        ASSERT_NE(C, R.NativeCalls.end());
        EXPECT_EQ(C->Result, Value);
        EXPECT_EQ(C->Library, Route == 3 ? "libadvice.so" : "");
      }
    }
  }
}
TEST_P(AndroidNative, DiscardClosedProviderPreservesCallerOutput) {
  Options.OutputLimit = 4 * 1024 * 1024;
  Options.Android->Initialize = false;
  Options.Android->Memory[0].Size = 16384;
  Options.Android->ReadMemory = {{Buffer, 16384}};
  Options.Android->Libraries["libadvice.so"] = {"madvise"};
  Options.Android->TraceLimit = 500000;
  std::vector<uint8_t> Bytes(16384, 0xa5);
  Options.Android->Memory[0].Bytes = Bytes;
  auto R = run("discard_pages", {Buffer, 4, 0});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
  EXPECT_FALSE(R.ReturnValue);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  llvm::support::endian::write64le(Bytes.data(), 73);
  llvm::support::endian::write64le(Bytes.data() + 8, 0x100000000);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Bytes);
  // Dynamically allocated pages are not entry-valid ReadMemory snapshots.
  // The stopped call cannot reach the fixture's post-call copy.
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_EQ(R.NativeCalls.back().Name, "madvise");
  EXPECT_FALSE(R.NativeCalls.back().Result);
}
TEST_P(AndroidNative, DynamicMemoryAdviceRetainsProviderLifetimeAndNames) {
  returned(run("dynamic_memory_advice", {Buffer, 4096, 12, 0}), 100);
  Options.Android->Libraries["libadvice.so"] = {};
  returned(run("dynamic_memory_advice", {Buffer, 4096, 12, 0}), 101);
  Options.Android->Libraries["libadvice.so"] = {"madvise"};
  auto R = run("dynamic_memory_advice", {Buffer, 4096, 12, 0});
  returned(R, 0);
  auto Lookup = llvm::find_if(R.NativeCalls, [](const auto &E) {
    return E.Name == "dlsym" && E.Symbol == "madvise";
  });
  ASSERT_NE(Lookup, R.NativeCalls.end());
  ASSERT_TRUE(Lookup->Result);
  auto Call = llvm::find_if(R.NativeCalls,
                            [](const auto &E) { return E.Name == "madvise"; });
  ASSERT_NE(Call, R.NativeCalls.end());
  EXPECT_EQ(Call->PC, *Lookup->Result);
  EXPECT_EQ(Call->Library, "libadvice.so");
  EXPECT_EQ(Call->Result, 0u);
  EXPECT_TRUE(R.Services.empty());
  R = run("dynamic_memory_advice", {Buffer, 4096, 13, 1});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_EQ(R.NativeCalls.back().Name, "madvise");
  EXPECT_EQ(R.NativeCalls.back().Library, "libadvice.so");
  EXPECT_FALSE(R.NativeCalls.back().Result);
  Options.Android->DefaultScope = std::vector<std::string>{"libadvice.so"};
  returned(run("dynamic_memory_advice", {Buffer, 4096, 13, 1}), 0);
}
TEST_P(AndroidNative,
       VectoredOutputSharesKernelSemanticsAndPreservesLibcErrno) {
  auto R = run(output_fixture::AndroidEntry);
  returned(R, 0);
  EXPECT_EQ(R.StandardOutput, std::string(output_fixture::Message,
                                          sizeof(output_fixture::Message) - 1));
  EXPECT_TRUE(R.StandardError.empty());
  ASSERT_EQ(R.Services.size(), 1u);
  EXPECT_EQ(R.Services.front().Number, output_fixture::NativeARMWriteV);
  EXPECT_EQ(R.Services.front().Result,
            uint64_t(0) - output_fixture::ErrorDescriptor);
}
TEST_P(AndroidNative, VectoredOutputBudgetPublishesNothing) {
  Options.OutputLimit = output_fixture::MessageSplit;
  Options.Android->TraceLimit = 0;
  Options.Android->ReadMemory.clear();
  auto R = run(output_fixture::AndroidEntry);
  EXPECT_EQ(R.Stop, ProcessStopReason::OutputLimit) << R.Diagnostic;
  EXPECT_TRUE(R.StandardOutput.empty());
  EXPECT_TRUE(R.StandardError.empty());
  EXPECT_FALSE(R.ReturnValue);
}
TEST_P(AndroidNative, IdentityImportsAndRawServicesAgreeWithoutChangingErrno) {
  auto R = run("identity_queries", {Buffer});
  returned(R, 0);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  const auto &Bytes = R.MemorySnapshots[0].Bytes;
  ASSERT_EQ(Bytes.size(), 64u);
  for (size_t I = 0; I < 6; ++I)
    EXPECT_EQ(llvm::support::endian::read32le(Bytes.data() + I * 4), 1000u);
  for (size_t I = 24; I < Bytes.size(); ++I)
    EXPECT_EQ(Bytes[I], 0);
  const std::vector<std::pair<std::string, uint64_t>> Queries = {
      {"getpid", 172},  {"gettid", 178}, {"getuid", 174},
      {"geteuid", 175}, {"getgid", 176}, {"getegid", 177}};
  ASSERT_EQ(R.Services.size(), Queries.size());
  for (size_t I = 0; I < Queries.size(); ++I) {
    auto Call = llvm::find_if(R.NativeCalls, [&](const auto &E) {
      return E.Name == Queries[I].first;
    });
    ASSERT_NE(Call, R.NativeCalls.end());
    EXPECT_TRUE(Call->Library.empty());
    EXPECT_EQ(Call->Result, 1000u);
    EXPECT_EQ(R.Services[I].Number, Queries[I].second);
    EXPECT_EQ(R.Services[I].Result, Call->Result);
  }
}
TEST_P(AndroidNative,
       DynamicIdentityQueriesPreserveNamesAndRequireOpenLibrary) {
  returned(run("dynamic_identities", {Buffer, 0}), 100);
  Options.Android->Libraries["libidentity.so"] = {"getuid", "geteuid", "getgid",
                                                  "getegid"};
  auto R = run("dynamic_identities", {Buffer, 0});
  returned(R, 0);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  const auto &Bytes = R.MemorySnapshots[0].Bytes;
  ASSERT_EQ(Bytes.size(), 64u);
  for (size_t I = 0; I < 4; ++I)
    EXPECT_EQ(llvm::support::endian::read32le(Bytes.data() + I * 4), 1000u);
  for (size_t I = 16; I < Bytes.size(); ++I)
    EXPECT_EQ(Bytes[I], 0);
  for (const char *Name : {"getuid", "geteuid", "getgid", "getegid"}) {
    auto Lookup = llvm::find_if(R.NativeCalls, [&](const auto &E) {
      return E.Name == "dlsym" && E.Symbol == Name;
    });
    ASSERT_NE(Lookup, R.NativeCalls.end());
    EXPECT_EQ(Lookup->Library, "libidentity.so");
    ASSERT_TRUE(Lookup->Result);
    auto Call = llvm::find_if(R.NativeCalls,
                              [&](const auto &E) { return E.Name == Name; });
    ASSERT_NE(Call, R.NativeCalls.end());
    EXPECT_EQ(Call->PC, *Lookup->Result);
    EXPECT_EQ(Call->Library, "libidentity.so");
    EXPECT_EQ(Call->Result, 1000u);
  }
  EXPECT_TRUE(R.Services.empty());
  R = run("dynamic_identities", {Buffer, 1});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_EQ(R.NativeCalls.back().Name, "getuid");
  EXPECT_EQ(R.NativeCalls.back().Library, "libidentity.so");
  EXPECT_FALSE(R.NativeCalls.back().Result);
}
TEST_P(AndroidNative, IdentityMutationImportRemainsUnsupported) {
  auto R = run("identity_mutation");
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_EQ(R.NativeCalls.back().Name, "setuid");
  EXPECT_FALSE(R.NativeCalls.back().Result);
}
TEST_P(AndroidNative, AllocationsPreserveBytesAndReportExhaustion) {
  auto R = run("allocation", {Buffer});
  returned(R, 0);
  ASSERT_EQ(R.MemorySnapshots.size(), 1);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes[4], 'Z');
  EXPECT_EQ(R.MemorySnapshots[0].Bytes[31], 'A');
}
TEST_P(AndroidNative, UnknownImportsAndForgedTrapsFailExplicitly) {
  auto R = run("unknown_call");
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
  EXPECT_NE(R.Diagnostic.find("unknown_native_function"), std::string::npos);
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_FALSE(R.NativeCalls.back().Result);
  R = run("forged_trap");
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("SVC"), std::string::npos);
}
TEST_P(AndroidNative, InvalidPointersAndStackCorruptionAreNotSuccess) {
  for (const char *Entry : {"bad_pointer", "bad_free", "stack_failure"}) {
    SCOPED_TRACE(Entry);
    auto R = run(Entry);
    EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
    EXPECT_FALSE(R.ReturnValue);
  }
}
TEST_P(AndroidNative, OpaqueStdioNeverExposesInventedStructureContents) {
  returned(run("stdio_identity"), 1);
  auto R = run("stdio_content");
  EXPECT_EQ(R.Stop, ProcessStopReason::CPUFailure) << R.Diagnostic;
  EXPECT_FALSE(R.ReturnValue);
}
TEST_P(AndroidNative, CapturesBinaryOutputAndEnforcesOutputLimit) {
  auto R = run("print_bytes");
  returned(R, 4);
  EXPECT_EQ(R.StandardOutput, std::string("A\0\xff\n", 4));
  Options.Android->TraceLimit = 0;
  Options.Android->ReadMemory.clear();
  Options.OutputLimit = 3;
  R = run("print_bytes");
  EXPECT_EQ(R.Stop, ProcessStopReason::OutputLimit);
  EXPECT_TRUE(R.StandardOutput.empty());
}
TEST_P(AndroidNative, BoundedTraceAndSharedInstructionEventBudgets) {
  Options.Limits.Instructions = 37;
  Options.Android->TraceLimit = 5;
  auto R = run("busy_loop");
  EXPECT_EQ(R.Stop, ProcessStopReason::InstructionLimit) << R.Diagnostic;
  EXPECT_EQ(R.Instructions, 37);
  EXPECT_EQ(R.Trace.size(), 5);
  EXPECT_TRUE(R.TraceTruncated);
  Options.Limits.Instructions = 10000;
  Options.Limits.Events = 1; // The constructor consumes the only return event.
  R = run("tls_slots");
  EXPECT_EQ(R.Stop, ProcessStopReason::EventLimit);
  EXPECT_EQ(R.Events, 1);
}
TEST_P(AndroidNative, ProgramMetadataDoesNotRequireSections) {
  auto Bytes = original();
  // ELF64 e_shoff, e_shentsize, e_shnum, e_shstrndx.
  llvm::support::endian::write64le(Bytes.data() + 40, 0);
  llvm::support::endian::write16le(Bytes.data() + 58, 0);
  llvm::support::endian::write16le(Bytes.data() + 60, 0);
  llvm::support::endian::write16le(Bytes.data() + 62, 0);
  temporary(Bytes, [&] {
    returned(run("add_arguments", {1, 2, 3, 4, 5, 6, 7, 8, 9, 10}), 402);
  });
}
TEST_P(AndroidNative, InvalidRelocationsFailBeforeAnyGuestInstruction) {
  auto Bytes = original();
  ELFLoader Loader;
  auto Image = llvm::cantFail(Loader.load(Path));
  auto Facts = llvm::cantFail(readELFProgramLinking(Image));
  uint64_t RelVA = 0;
  for (const auto &D : Facts.Dynamic)
    if (D.Tag == llvm::ELF::DT_JMPREL)
      RelVA = D.Value;
  ASSERT_NE(RelVA, 0);
  uint64_t Offset = 0;
  for (const auto &P : Image.ELFMetadata->ProgramHeaders)
    if (P.Type == llvm::ELF::PT_LOAD && RelVA >= P.VirtualAddress &&
        RelVA - P.VirtualAddress < P.FileSize)
      Offset = P.FileOffset + RelVA - P.VirtualAddress;
  ASSERT_NE(Offset, 0);
  for (unsigned Mode = 0; Mode < 3; ++Mode) {
    SCOPED_TRACE(Mode);
    auto Bad = Bytes;
    if (Mode == 0) // Unsupported relocation type, preserving the symbol index.
      llvm::support::endian::write32le(Bad.data() + Offset + 8,
                                       llvm::ELF::R_AARCH64_IRELATIVE);
    else if (Mode == 1)
      llvm::support::endian::write32le(Bad.data() + Offset + 12, 0xffffffff);
    else // Destination overflow, even though the table itself is valid.
      llvm::support::endian::write64le(Bad.data() + Offset, UINT64_MAX - 3);
    temporary(Bad, [&] {
      Options.Android->EntrySymbol = "tls_slots";
      auto Result =
          emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
      EXPECT_FALSE(bool(Result));
      llvm::consumeError(Result.takeError());
    });
  }
}
TEST_P(AndroidNative, RejectsContradictoryProgramHeaderMappings) {
  auto Bytes = original();
  uint64_t HeaderOffset = llvm::support::endian::read64le(Bytes.data() + 32);
  uint16_t Count = llvm::support::endian::read16le(Bytes.data() + 56);
  uint64_t Load = 0, Dynamic = 0;
  for (uint16_t I = 0; I < Count; ++I) {
    uint64_t P = HeaderOffset + I * 56;
    uint32_t Type = llvm::support::endian::read32le(Bytes.data() + P);
    if (Type == llvm::ELF::PT_LOAD && !Load)
      Load = P;
    if (Type == llvm::ELF::PT_DYNAMIC)
      Dynamic = P;
  }
  ASSERT_NE(Load, 0);
  ASSERT_NE(Dynamic, 0);
  for (unsigned Mode = 0; Mode < 4; ++Mode) {
    auto Bad = Bytes;
    if (Mode == 0)
      llvm::support::endian::write64le(Bad.data() + Load + 48, 3);
    if (Mode == 1)
      llvm::support::endian::write64le(Bad.data() + Load + 16, 1);
    if (Mode == 2)
      llvm::support::endian::write64le(Bad.data() + Load + 40, 0);
    if (Mode == 3) {
      uint64_t VA = llvm::support::endian::read64le(Bad.data() + Dynamic + 16);
      llvm::support::endian::write64le(Bad.data() + Dynamic + 16, VA + 8);
    }
    temporary(Bad, [&] {
      Options.Android->EntrySymbol = "tls_slots";
      auto Result =
          emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
      EXPECT_FALSE(bool(Result)) << Mode;
      llvm::consumeError(Result.takeError());
    });
  }
}
TEST_P(AndroidNative, InvalidRequestsAreRejectedAndTimeoutIsTyped) {
  Options.Android->EntrySymbol = "tls_slots";
  auto Original = Options;
  auto Rejected = [&] {
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    EXPECT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
    Options = Original;
  };
  Options.Android->EntryAddress = 0;
  Rejected();
  Options.Android->Properties["invalid"] = std::string(92, 'a');
  Rejected();
  Options.Android->Memory.push_back({Buffer, 4096, {}, false});
  Rejected();
  Options.Android->LoadBias = UINT64_MAX;
  Rejected();
  Options.Android->TraceLimit = Options.OutputLimit;
  Rejected();
  Options.Limits.TimeoutMicroseconds = 1;
  auto R = run("busy_loop");
  EXPECT_EQ(R.Stop, ProcessStopReason::Timeout) << R.Diagnostic;
}
TEST_P(AndroidNative, RejectsMalformedOriginalDynamicTables) {
  ELFLoader Loader;
  auto Image = llvm::cantFail(Loader.load(Path));
  auto Facts = readELFProgramLinking(Image);
  ASSERT_TRUE(bool(Facts)) << llvm::toString(Facts.takeError());
  EXPECT_FALSE(Facts->Relocations.empty());
  uint64_t DynamicOffset = 0;
  for (const auto &P : Image.ELFMetadata->ProgramHeaders)
    if (P.Type == llvm::ELF::PT_DYNAMIC)
      DynamicOffset = P.FileOffset;
  ASSERT_NE(DynamicOffset, 0);
  auto Bytes = Image.Raw;
  // Force the string table span to overflow without touching any sections.
  for (uint64_t P = DynamicOffset; P + 16 <= Image.Raw.size(); P += 16) {
    auto Tag = llvm::support::endian::read64le(Image.Raw.data() + P);
    if (Tag == llvm::ELF::DT_NULL)
      break;
    if (Tag == llvm::ELF::DT_STRSZ) {
      llvm::support::endian::write64le(Image.Raw.data() + P + 8, UINT64_MAX);
      break;
    }
  }
  auto Bad = readELFProgramLinking(Image);
  EXPECT_FALSE(bool(Bad));
  llvm::consumeError(Bad.takeError());
}
TEST_P(AndroidNative, DynamicLookupUsesOnlyExplicitGuestSymbols) {
  returned(run("dynamic_lookup"), 100);
  Options.Android->Libraries["libfixture.so"] = {"strlen"};
  auto R = run("dynamic_lookup");
  returned(R, 4);
  auto Lookup = llvm::find_if(R.NativeCalls,
                              [](const auto &E) { return E.Name == "dlsym"; });
  ASSERT_NE(Lookup, R.NativeCalls.end());
  EXPECT_EQ(Lookup->Library, "libfixture.so");
  EXPECT_EQ(Lookup->Symbol, "strlen");
  ASSERT_TRUE(Lookup->Result);
  auto Call = llvm::find_if(R.NativeCalls,
                            [](const auto &E) { return E.Name == "strlen"; });
  ASSERT_NE(Call, R.NativeCalls.end());
  EXPECT_EQ(Call->PC, *Lookup->Result);
  EXPECT_EQ(Call->Library, "libfixture.so");
  EXPECT_EQ(Call->Result, 4);
  auto Report = llvm::json::parse(processResultJSON(R));
  ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError());
  const auto *Android = Report->getAsObject()->getObject("android");
  ASSERT_NE(Android, nullptr);
  bool Named = false;
  for (const auto &V : *Android->getArray("native_calls")) {
    const auto *E = V.getAsObject();
    if (E->getString("name") == "dlsym") {
      EXPECT_EQ(E->getString("library"), "libfixture.so");
      EXPECT_EQ(E->getString("symbol"), "strlen");
      Named = true;
    }
  }
  EXPECT_TRUE(Named);
}
TEST_P(AndroidNative, DynamicHandlesFailuresAndConsumeOnceErrors) {
  Options.Android->Libraries["libfixture.so"] = {"strlen"};
  returned(run("dynamic_lifecycle"), 0);
}
TEST_P(AndroidNative, DynamicProvidersKeepSeparateSymbolNamespaces) {
  Options.Android->Libraries["libfixture.so"] = {"strlen"};
  Options.Android->Libraries["libother.so"] = {"strlen", "only_in_other"};
  returned(run("dynamic_providers"), 5);
}
TEST_P(AndroidNative, DynamicUnknownAndClosedFunctionsStopWithTheirNames) {
  Options.Android->Libraries["libfixture.so"] = {"strlen",
                                                 "unmodeled_fixture_export"};
  auto R = run("dynamic_unknown");
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("unmodeled_fixture_export"), std::string::npos);
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_EQ(R.NativeCalls.back().Name, "unmodeled_fixture_export");
  EXPECT_EQ(R.NativeCalls.back().Library, "libfixture.so");
  EXPECT_FALSE(R.NativeCalls.back().Result);
  R = run("dynamic_closed");
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
  EXPECT_EQ(R.NativeCalls.back().Name, "strlen");
}
TEST_P(AndroidNative, FamilyFallbacksPreserveDiagnosticsAndProviderPrecedence) {
  struct Case {
    const char *Name;
    const char *Diagnostic;
  };
  const Case Cases[] = {
      {"pthread_attr_unmodeled",
       "pthread_attr_unmodeled: unmodeled thread attribute operation"},
      {"pthread_mutexattr_unmodeled",
       "pthread_mutexattr_unmodeled: unmodeled mutex attribute operation"},
      {"pthread_mutex_unmodeled",
       "pthread_mutex_unmodeled: unmodeled mutex operation"},
      {"pthread_mutexextra", "pthread_mutexextra: unmodeled mutex operation"},
      {"pthread_self", "unmodeled Android import: pthread_self"},
      {"unmodeled_fixture_export",
       "unmodeled Android import: unmodeled_fixture_export"}};
  Options.Android->ThreadLimit = 1;
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.Name);
    llvm::StringRef Name(C.Name);
    auto &Bytes = Options.Android->Memory[0].Bytes;
    Bytes.assign(Name.bytes_begin(), Name.bytes_end());
    Bytes.push_back(0);
    Options.Android->Libraries["libfixture.so"] = {C.Name};
    for (uint64_t Closed : {0, 1}) {
      SCOPED_TRACE(Closed);
      auto R = run("dynamic_dispatch", {Buffer, Closed});
      EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
      EXPECT_EQ(R.Diagnostic,
                Closed
                    ? "call through an inactive dynamic library: libfixture.so"
                    : C.Diagnostic);
      EXPECT_FALSE(R.ReturnValue);
      ASSERT_FALSE(R.NativeCalls.empty());
      EXPECT_EQ(R.NativeCalls.back().Name, C.Name);
      EXPECT_EQ(R.NativeCalls.back().Library, "libfixture.so");
      EXPECT_FALSE(R.NativeCalls.back().Result);
    }
  }
}

TEST_P(AndroidNative, DynamicScopesAndInvalidPointersAreNotGuessed) {
  Options.Android->Libraries["libfixture.so"] = {"strlen"};
  for (uint64_t Scope : {uint64_t(0), UINT64_MAX}) {
    auto R = run("dynamic_scope", {Scope});
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(R.Diagnostic.find("process scopes"), std::string::npos);
  }
  auto R = run("dynamic_open", {0, 2});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  R = run("dynamic_open", {1, 2});
  EXPECT_NE(R.Stop, ProcessStopReason::Returned);
  EXPECT_NE(R.Diagnostic.find("invalid guest pointer"), std::string::npos);
  R = run("dynamic_bad_name");
  EXPECT_NE(R.Stop, ProcessStopReason::Returned);
  EXPECT_NE(R.Diagnostic.find("invalid guest pointer"), std::string::npos);
}
TEST_P(AndroidNative, DynamicCatalogueRejectsMalformedCppInputs) {
  for (const auto &Names : std::vector<std::vector<std::string>>{
           {""}, {"strlen", "strlen"}, {std::string("bad\0name", 8)}}) {
    Options.Android->EntrySymbol = "dynamic_lookup";
    Options.Android->Libraries["libfixture.so"] = Names;
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    EXPECT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find("modeled Android symbols"),
              std::string::npos);
  }
}
TEST_P(AndroidNative, DefaultScopeUsesDeclaredOrderAndResidentProviders) {
  Options.Android->Libraries = {{"libfixture.so", {"strlen"}},
                                {"libother.so", {"strlen"}},
                                {"libempty.so", {}}};
  for (const std::string First : {"libother.so", "libfixture.so"}) {
    const std::string Second =
        First == "libother.so" ? "libfixture.so" : "libother.so";
    Options.Android->DefaultScope =
        std::vector<std::string>{"libempty.so", First, Second};
    auto R = run("default_call");
    returned(R, 6);
    ASSERT_EQ(R.NativeCalls.size(), 2u);
    const auto &Lookup = R.NativeCalls[0], &Call = R.NativeCalls[1];
    EXPECT_EQ(Lookup.Name, "dlsym");
    EXPECT_EQ(Lookup.Arguments[0], 0u);
    EXPECT_EQ(Lookup.Symbol, "strlen");
    EXPECT_EQ(Lookup.Library, First);
    ASSERT_TRUE(Lookup.Result);
    EXPECT_EQ(Call.Name, "strlen");
    EXPECT_EQ(Call.Library, First);
    EXPECT_EQ(Call.PC, *Lookup.Result);
  }
  Options.Android->DefaultScope = std::vector<std::string>{"libfixture.so"};
  returned(run("default_resident_lifecycle"), 0);
  // A fresh workload must not inherit this scope or its handles.
  Options.Android->DefaultScope.reset();
  EXPECT_EQ(run("default_call").Stop, ProcessStopReason::UnsupportedService);
}
TEST_P(AndroidNative, EmptyDefaultScopeAndLocalOpensPreserveLookupFailures) {
  Options.Android->Libraries["libfixture.so"] = {"strlen"};
  Options.Android->DefaultScope.emplace();
  returned(run("default_call"), 100);
  returned(run("default_missing"), 0);
  returned(run("default_local_open"), 0);
  Options.Android->DefaultScope = std::vector<std::string>{"libfixture.so"};
  returned(run("default_missing"), 0);
  auto R = run("dynamic_scope", {UINT64_MAX});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("caller identity"), std::string::npos);
  EXPECT_EQ(run("dynamic_open", {0, 2}).Stop,
            ProcessStopReason::UnsupportedService);
}
TEST_P(AndroidNative, DefaultScopeDoesNotImplementUnknownFunctions) {
  Options.Android->Libraries["libfixture.so"] = {"unmodeled_fixture_export"};
  Options.Android->DefaultScope = std::vector<std::string>{"libfixture.so"};
  auto R = run("default_unknown");
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  ASSERT_EQ(R.NativeCalls.size(), 2u);
  ASSERT_TRUE(R.NativeCalls.front().Result);
  EXPECT_EQ(R.NativeCalls.back().PC, *R.NativeCalls.front().Result);
  EXPECT_EQ(R.NativeCalls.back().Name, "unmodeled_fixture_export");
  EXPECT_EQ(R.NativeCalls.back().Library, "libfixture.so");
  EXPECT_FALSE(R.NativeCalls.back().Result);
}
TEST_P(AndroidNative, DynamicFlagsFollowBionicBitmaskContract) {
  Options.Android->Libraries["libfixture.so"] = {"strlen"};
  Options.Android->DefaultScope = std::vector<std::string>{"libfixture.so"};
  for (uint64_t Flags = 0; Flags != 8; ++Flags) {
    SCOPED_TRACE(Flags);
    returned(run("dynamic_flag_combinations", {Flags}), 0);
  }
  for (uint64_t Flags : {8u, 0x80000000u, 0xffffffffu})
    returned(run("dynamic_flag_combinations", {Flags}), 0);
  for (uint64_t Flags : {0x100u, 0x1002u})
    EXPECT_EQ(run("dynamic_flag_combinations", {Flags}).Stop,
              ProcessStopReason::UnsupportedService);
}
TEST_P(AndroidNative, DefaultScopeRejectsUnknownOrRepeatedProviders) {
  Options.Android->EntrySymbol = "default_call";
  Options.Android->Libraries = {{"libfixture.so", {"strlen"}},
                                {"libother.so", {}}};
  for (const auto &Scope : std::vector<std::vector<std::string>>{
           {"missing.so"},
           {""},
           {"libfixture.so", "libfixture.so"},
           {"libfixture.so", "libother.so", "missing.so"},
           {std::string("bad\0name", 8)}}) {
    Options.Android->DefaultScope = Scope;
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    ASSERT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find("default scope"),
              std::string::npos);
  }
}
TEST_P(AndroidNative, ResidencyObservationsShareRoutesBitsAndErrno) {
  Options.Android->Initialize = false;
  Options.LinuxKernel.emplace().ResidencyObservations =
      std::vector<LinuxResidencyObservation>{{0x100000000, {0, 1, 0}},
                                             {0x100000000, {1, 0, 1}}};
  Options.Android->Libraries["libresidency.so"] = {"mincore"};
  for (uint64_t Route : {0u, 1u, 2u, 3u}) {
    auto R = run("residency_observe", {Buffer, Route, 8193, Buffer + 48, 4});
    returned(R, 0);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    std::vector<uint8_t> Expected(64);
    llvm::support::endian::write64le(Expected.data(), 0x100000000);
    llvm::support::endian::write64le(Expected.data() + 16, 733);
    llvm::support::endian::write64le(Expected.data() + 32, 733);
    Expected[49] = 1;
    Expected[56] = 1;
    Expected[58] = 1;
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  }
}
TEST_P(AndroidNative,
       ResidencyMissingInputAndClosedProvidersRemainUnsupported) {
  Options.Android->Initialize = false;
  auto R = run("residency_observe", {Buffer, 0, 12288, Buffer + 48, 0});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("page residency"), std::string::npos);
  Options.LinuxKernel.emplace().ResidencyObservations.emplace();
  R = run("residency_observe", {Buffer, 0, 12288, Buffer + 48, 0});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("exhausted"), std::string::npos);
  Options.LinuxKernel->ResidencyObservations->push_back(
      {0x100000000, {1, 0, 1}});
  Options.Android->Libraries["libresidency.so"] = {"mincore"};
  R = run("residency_observe", {Buffer, 4, 12288, Buffer + 48, 0});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
  EXPECT_FALSE(R.NativeCalls.back().Result);
}
TEST_P(AndroidNative, ResidencyHolesAndCopyFaultsHaveKernelErrorOrdering) {
  Options.Android->Initialize = false;
  Options.LinuxKernel.emplace().ResidencyObservations =
      std::vector<LinuxResidencyObservation>{{0x100000000, {1, 0, 1}}};
  for (uint64_t Route : {0u, 1u, 2u}) {
    auto R = run("residency_observe", {Buffer, Route, 12288, Buffer + 48, 1});
    returned(R, Route == 2 ? uint64_t(-12) : UINT64_MAX);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes[48], 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes[49], 0u);
    EXPECT_EQ(
        llvm::support::endian::read64le(R.MemorySnapshots[0].Bytes.data() + 16),
        Route == 2 ? 733u : 12u);
    R = run("residency_observe", {Buffer, Route, 12288, 0, 0});
    returned(R, Route == 2 ? uint64_t(-14) : UINT64_MAX);
  }
}

INSTANTIATE_TEST_SUITE_P(Relocations, AndroidNative,
                         testing::Values("none", "android", "relr"));
TEST(AndroidOptions, StrictWireTypesAndProfiles) {
  for (
      const char *Text :
      {R"({"android":{"typo":1}})", R"({"android":{"arguments":[-1]}})",
       R"({"android":{"entry_address":"0xzz"}})",
       R"({"android":{"memory":[{"address":4096,"size":4096,"bytes_hex":"gg"}]}})",
       R"({"android":{"read_memory":[{"address":4096,"size":8,"executable":true}]}})",
       R"({"android":{"libraries":[]}})",
       R"({"android":{"libraries":{"x":1}}})",
       R"({"android":{"libraries":{"x":["a","a"]}}})",
       R"({"android":{"libraries":{"x":[""]}}})",
       R"({"android":{"libraries":{"x":["bad\u0000name"]}}})",
       R"({"android":{"libraries":{"":[]}}})",
       R"({"android":{"default_scope":null}})",
       R"({"android":{"default_scope":{}}})",
       R"({"android":{"default_scope":[7]}})",
       R"({"android":{"default_scope":["bad\u0000name"]}})"}) {
    auto O = processOptionsFromJSON(Text);
    EXPECT_FALSE(bool(O)) << Text;
    llvm::consumeError(O.takeError());
  }
  auto O = processOptionsFromJSON(
      R"({"android":{"entry_address":"0x1000","arguments":["0xffffffffffffffff",0],"initialize":false,"trace_limit":0,"libraries":{"libfixture.so":["strlen"]}}})");
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  ASSERT_TRUE(O->Android);
  EXPECT_EQ(O->Android->Arguments[0], UINT64_MAX);
  EXPECT_EQ(O->Android->EntryAddress, 4096);
  EXPECT_EQ(O->Android->Libraries.at("libfixture.so"),
            std::vector<std::string>{"strlen"});
  auto Bad = emulateProcess("missing", ProcessProfile::LinuxELF64, *O);
  EXPECT_FALSE(bool(Bad));
  EXPECT_NE(llvm::toString(Bad.takeError()).find("Android"), std::string::npos);
}
TEST(AndroidOptions, DladdrPositiveObservationsAreStrictAndOptional) {
  auto Parse = [](llvm::StringRef Records) {
    return processOptionsFromJSON("{\"android\":{\"entry_symbol\":\"entry\","
                                  "\"dladdr_observations\":" +
                                  Records.str() + "}}");
  };
  auto Empty = Parse("[]");
  ASSERT_TRUE(bool(Empty)) << llvm::toString(Empty.takeError());
  ASSERT_TRUE(Empty->Android->DladdrObservations);
  EXPECT_TRUE(Empty->Android->DladdrObservations->empty());
  const char *Record =
      R"([{"address":"0x40001004","image_base":"0x40000000","image_name":"/guest/fixture.so","symbol_name":null,"symbol_address":0}])";
  auto Good = Parse(Record);
  ASSERT_TRUE(bool(Good)) << llvm::toString(Good.takeError());
  const auto &O = Good->Android->DladdrObservations->front();
  EXPECT_EQ(O.Address, 0x40001004u);
  EXPECT_EQ(O.ImageBase, 0x40000000u);
  EXPECT_EQ(O.ImageName, "/guest/fixture.so");
  EXPECT_FALSE(O.SymbolName);
  EXPECT_EQ(O.SymbolAddress, 0u);
  for (
      const char *Bad :
      {"null", "{}", "[{}]",
       R"([{"address":0,"image_base":0,"image_name":"x","symbol_name":null,"symbol_address":0}])",
       R"([{"address":1,"image_base":2,"image_name":"x","symbol_name":null,"symbol_address":0}])",
       R"([{"address":1,"image_base":0,"image_name":"","symbol_name":null,"symbol_address":0}])",
       R"([{"address":1,"image_base":0,"image_name":"x","symbol_name":"s","symbol_address":0}])",
       R"([{"address":1,"image_base":0,"image_name":"x","symbol_name":null,"symbol_address":1}])",
       R"([{"address":1,"image_base":0,"image_name":"x","symbol_name":false,"symbol_address":0}])",
       R"([{"address":1.5,"image_base":0,"image_name":"x","symbol_name":null,"symbol_address":0}])",
       R"([{"address":1,"image_base":0,"image_name":"x","symbol_name":null,"symbol_address":0,"extra":0}])"}) {
    SCOPED_TRACE(Bad);
    auto Result = Parse(Bad);
    EXPECT_FALSE(bool(Result));
    llvm::consumeError(Result.takeError());
  }
}
TEST_P(AndroidNative, DladdrTypedRecordsRejectInvalidStateBeforeGuestEntry) {
  Options.Android->EntrySymbol = "add_arguments";
  NativeDladdrObservation Good{0x40001004, 0x40000000, "/guest/fixture.so",
                               "symbol", 0x40001000};
  for (unsigned Mode = 0; Mode != 9; ++Mode) {
    SCOPED_TRACE(Mode);
    auto Bad = Good;
    switch (Mode) {
    case 0:
      Bad.Address = 0;
      break;
    case 1:
      Bad.ImageBase = Bad.Address + 1;
      break;
    case 2:
      Bad.ImageName = std::string("x\0y", 3);
      break;
    case 3:
      Bad.ImageName.assign(
          ::neverd::limits::emulation::kAndroidDladdrStringBytes + 1, 'x');
      break;
    case 4:
      Bad.SymbolName = "";
      break;
    case 5:
      Bad.SymbolName.reset();
      break;
    case 6:
      Bad.SymbolAddress = Bad.Address + 1;
      break;
    case 7:
      Bad.SymbolAddress = Bad.ImageBase - 1;
      break;
    case 8:
      break;
    }
    Options.Android->DladdrObservations = std::vector{Bad};
    if (Mode == 8)
      Options.Android->DladdrObservations->push_back(Bad);
    auto Result =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    EXPECT_FALSE(bool(Result));
    EXPECT_NE(llvm::toString(Result.takeError()).find("dladdr"),
              std::string::npos);
  }
}

TEST(AndroidOptions, DefaultScopeDistinguishesUnspecifiedFromEmpty) {
  for (const char *Text :
       {R"({"android":{}})", R"({"android":{"default_scope":[]}})",
        R"({"android":{"default_scope":["b","a"]}})"}) {
    auto O = processOptionsFromJSON(Text);
    ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
    ASSERT_TRUE(O->Android);
    if (llvm::StringRef(Text).contains("[\"b\"")) {
      ASSERT_TRUE(O->Android->DefaultScope);
      EXPECT_EQ(*O->Android->DefaultScope,
                (std::vector<std::string>{"b", "a"}));
    } else if (llvm::StringRef(Text).contains("[]")) {
      ASSERT_TRUE(O->Android->DefaultScope);
      EXPECT_TRUE(O->Android->DefaultScope->empty());
    } else {
      EXPECT_FALSE(O->Android->DefaultScope);
    }
  }
}
} // namespace
} // namespace neverd::emulation
