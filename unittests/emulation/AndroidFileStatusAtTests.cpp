//===- AndroidFileStatusAtTests.cpp - Path and descriptor observations ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxFileTestMetadata.h"
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessReport.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
constexpr uint64_t Buffer = 0x20000000;
constexpr uint64_t Name = Buffer + 512;
constexpr uint64_t Output = Buffer + 1024;

class AndroidFileStatusAt : public testing::TestWithParam<const char *> {
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
    ExecutionConfiguration C;
    C.Backend = Options.Backend;
    C.Architecture = GuestArchitecture::AArch64;
    C.Contract = ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(C);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available)
      GTEST_SKIP() << Probe->Reason;
    Options.Android.emplace();
    Options.Android->Initialize = false;
    Options.Android->TraceLimit = 100000;
    Options.Android->Memory.push_back(
        {Buffer, 8192, std::vector<uint8_t>(8192, 0xa5), false});
    Options.Android->ReadMemory.push_back({Buffer, 8192});
    Options.LinuxFiles.emplace();
    Options.LinuxFiles->Files["/fixture/data"] = {0,    0xff, 0x41,
                                                  0x0a, 0x80, 0x5a};
    Options.LinuxFiles->Metadata["/fixture/data"] = fileTestMetadata();
    Options.Limits.Instructions = 100000;
    setPath("/fixture/data");
#endif
  }
  void setPath(llvm::StringRef Text) {
    auto &Bytes = Options.Android->Memory[0].Bytes;
    std::copy(Text.begin(), Text.end(), Bytes.begin() + 512);
    Bytes[512 + Text.size()] = 0;
  }
  ProcessResult run(const char *Entry, std::vector<uint64_t> Args = {}) {
    Options.Android->EntrySymbol = Entry;
    Options.Android->Arguments = std::move(Args);
    return llvm::cantFail(
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options));
  }
  ProcessResult call(uint64_t Route, uint64_t Directory = uint64_t(-100),
                     uint64_t PathAddress = Name, uint64_t Destination = Output,
                     uint64_t Flags = 0) {
    return run("files_status_at_call",
               {Route, Directory, PathAddress, Destination, Flags, Buffer});
  }
};

TEST_P(AndroidFileStatusAt, PathnameAliasesPreserveCompleteBytesAndErrorOrder) {
  Options.Android->Libraries["libfiles.so"] = {"stat", "stat64", "lstat",
                                               "lstat64"};
  struct Case {
    const char *Text;
    uint64_t PathAddress, Destination, Error;
  };
  const Case Cases[] = {{"/fixture/data", Name, Output, 0},
                        {"/fixture/data", Name, Name, 0},
                        {"/fixture/./data", Name, Output, 0},
                        {"/missing", Name, 1, 2},
                        {"", Name, Output, 2},
                        {"/fixture/data/child", Name, 1, 20},
                        {"/fixture/data/", Name, Output, 20},
                        {"/fixture/data", 0, Output, 14},
                        {"/fixture/data", Buffer + 8192, Output, 14},
                        {"/fixture/data", Name, 1, 14}};
  const uint64_t Words[] = {0xfe12cd34,
                            0xfedcba9876543210ULL,
                            0x89abcdef000081a4ULL,
                            0xfedcba9887654321ULL,
                            0,
                            0,
                            0,
                            16384,
                            0x1234567890ULL,
                            0x8000000000000001ULL,
                            123456789,
                            4294967297,
                            987654321,
                            0x7fffffffffffffffULL,
                            999999999,
                            0};
  const char *Names[] = {"stat", "stat64", "lstat", "lstat64"};
  for (uint64_t Alias = 0; Alias != 4; ++Alias) {
    for (uint64_t Dynamic : {0u, 1u}) {
      for (const auto &C : Cases) {
        SCOPED_TRACE(testing::Message() << Names[Alias] << ':' << Dynamic << ':'
                                        << C.Text << ':' << C.Error);
        setPath(C.Text);
        auto Expected = Options.Android->Memory[0].Bytes;
        llvm::support::endian::write64le(Expected.data(),
                                         C.Error ? C.Error : 83);
        if (!C.Error)
          for (unsigned I = 0; I != 16; ++I)
            llvm::support::endian::write64le(
                Expected.data() + C.Destination - Buffer + I * 8, Words[I]);
        auto R = run("files_path_status_call",
                     {Alias, C.PathAddress, C.Destination, Buffer, Dynamic});
        ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
        EXPECT_EQ(R.ReturnValue, C.Error ? UINT64_MAX : 0);
        ASSERT_EQ(R.MemorySnapshots.size(), 1u);
        EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
        // Named libc wrappers retain native events; only actual guest traps
        // publish raw service events.
        EXPECT_TRUE(R.Services.empty());
        unsigned Calls = 0;
        for (const auto &Call : R.NativeCalls)
          if (Call.Name == Names[Alias]) {
            EXPECT_EQ(Call.Arguments[0], C.PathAddress);
            EXPECT_EQ(Call.Arguments[1], C.Destination);
            EXPECT_EQ(Call.Library, Dynamic ? "libfiles.so" : "");
            ++Calls;
          }
        EXPECT_EQ(Calls, 1u);
      }
    }
  }
}

TEST_P(AndroidFileStatusAt, PathnameAliasesKeepUnsupportedCallsPending) {
  Options.Android->Libraries["libfiles.so"] = {"stat", "stat64", "lstat",
                                               "lstat64"};
  Options.LinuxFiles->SymbolicLinks["/link"] = {'/', 'f'};
  const auto Files = Options.LinuxFiles;
  for (unsigned Mode = 0; Mode != 8; ++Mode) {
    Options.LinuxFiles = Files;
    if (Mode == 0)
      Options.LinuxFiles.reset();
    if (Mode == 1)
      Options.LinuxFiles->Metadata.clear();
    setPath(Mode == 2   ? "/fixture"
            : Mode == 3 ? "fixture/data"
            : Mode == 4 ? "/link"
            : Mode == 5 ? "/link/child"
                        : "/fixture/data");
    for (uint64_t Alias = 0; Alias != 4; ++Alias) {
      SCOPED_TRACE(testing::Message() << Mode << ':' << Alias);
      auto R = run("files_path_status_call",
                   {Alias, Name, Mode == 6 ? Buffer + 8192 - 64 : Output,
                    Buffer, Mode == 7 ? 2u : 0u});
      EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
      EXPECT_FALSE(R.ReturnValue);
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, Options.Android->Memory[0].Bytes);
      ASSERT_FALSE(R.NativeCalls.empty());
      EXPECT_FALSE(R.NativeCalls.back().Result);
      if (Mode == 7) {
        EXPECT_EQ(R.NativeCalls.back().Library, "libfiles.so");
        EXPECT_NE(R.Diagnostic.find("inactive dynamic library"),
                  std::string::npos);
      }
    }
  }
}

TEST_P(AndroidFileStatusAt, PathnameAliasesMatchNativeTransport) {
  Options.Android->Libraries["libfiles.so"] = {"stat", "stat64", "lstat",
                                               "lstat64"};
  bool Compared = false;
  for (auto Backend : {ExecutionBackendKind::KVM, ExecutionBackendKind::WHP,
                       ExecutionBackendKind::HVF}) {
    ExecutionConfiguration C;
    C.Backend = Backend;
    C.Architecture = GuestArchitecture::AArch64;
    C.Contract = ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(C);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available)
      continue;
    for (const char *Text : {"/fixture/data", "/missing", "/fixture"}) {
      setPath(Text);
      for (uint64_t Alias = 0; Alias != 4; ++Alias) {
        SCOPED_TRACE(testing::Message() << Text << ':' << Alias);
        auto Args =
            std::vector<uint64_t>{Alias, Name, Output, Buffer, Alias % 2};
        Options.Backend = ExecutionBackendKind::Unicorn;
        auto Software = run("files_path_status_call", Args);
        ASSERT_EQ(Software.Stop, llvm::StringRef(Text) == "/fixture"
                                     ? ProcessStopReason::UnsupportedService
                                     : ProcessStopReason::Returned);
        ASSERT_FALSE(Software.TraceTruncated);
        Options.Backend = Backend;
        auto Native = run("files_path_status_call", Args);
        Native.SelectedBackend = Software.SelectedBackend;
        Native.BackendSelectionReason = Software.BackendSelectionReason;
        EXPECT_EQ(processResultJSON(Native), processResultJSON(Software));
        Compared = true;
      }
    }
  }
  if (!Compared)
    GTEST_SKIP() << "native AArch64 transports unavailable";
}

TEST_P(AndroidFileStatusAt, ParentWalkCallsPreserveErrnoAndCompleteGuards) {
  Options.LinuxFiles->Directories.push_back("/fixture/sub");
  const char BasePath[] = "/fixture/sub";
  auto &Initial = Options.Android->Memory[0].Bytes;
  std::copy_n(BasePath, sizeof(BasePath), Initial.begin() + 256);
  struct Case {
    const char *Text;
    uint64_t Error;
  };
  for (const auto &C :
       {Case{"../data", 0}, Case{"../../fixture/data", 0},
        Case{"../../../../fixture/data", 0}, Case{"../missing", 2},
        Case{"absent/../../data", 2}, Case{"../data/../data", 20},
        Case{"../data/.", 20}, Case{"/fixture/sub/../data", 0}}) {
    setPath(C.Text);
    for (uint64_t Route : {0u, 1u, 2u, 3u}) {
      SCOPED_TRACE(testing::Message() << C.Text << ':' << Route);
      auto Expected = Options.Android->Memory[0].Bytes;
      llvm::support::endian::write64le(Expected.data(),
                                       C.Error && Route != 3 ? C.Error : 83);
      auto R =
          run("files_open_at_call", {Route, Buffer + 256, Name, 0, 0, Buffer});
      ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
      EXPECT_EQ(R.ReturnValue,
                C.Error ? uint64_t(0) - (Route == 3 ? C.Error : 1) : 4u);
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
    }
  }
}

TEST_P(AndroidFileStatusAt, ParentWalkCallsMatchNativeTransport) {
  Options.LinuxFiles->Directories.push_back("/fixture/sub");
  Options.LinuxFiles->SymbolicLinks["/alias"] = {'/', 'f', 'i', 'x',
                                                 't', 'u', 'r', 'e'};
  const char BasePath[] = "/fixture/sub";
  auto &Initial = Options.Android->Memory[0].Bytes;
  std::copy_n(BasePath, sizeof(BasePath), Initial.begin() + 256);
  bool Compared = false;
  for (auto Backend : {ExecutionBackendKind::KVM, ExecutionBackendKind::WHP,
                       ExecutionBackendKind::HVF}) {
    ExecutionConfiguration C;
    C.Backend = Backend;
    C.Architecture = GuestArchitecture::AArch64;
    C.Contract = ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(C);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available)
      continue;
    for (const char *Text : {"../data", "../missing", "../data/..",
                             "/alias/../fixture/data", "../."}) {
      setPath(Text);
      for (uint64_t Route : {0u, 1u, 2u, 3u}) {
        SCOPED_TRACE(testing::Message() << Text << ':' << Route);
        auto Args =
            std::vector<uint64_t>{Route, Buffer + 256, Name, 0, 0, Buffer};
        Options.Backend = ExecutionBackendKind::Unicorn;
        auto Software = run("files_open_at_call", Args);
        ASSERT_FALSE(Software.TraceTruncated);
        ASSERT_EQ(Software.Stop, llvm::StringRef(Text).starts_with("/alias")
                                     ? ProcessStopReason::UnsupportedService
                                     : ProcessStopReason::Returned)
            << Software.Diagnostic;
        Options.Backend = Backend;
        auto Native = run("files_open_at_call", Args);
        Native.SelectedBackend = Software.SelectedBackend;
        Native.BackendSelectionReason = Software.BackendSelectionReason;
        EXPECT_EQ(processResultJSON(Native), processResultJSON(Software));
        Compared = true;
      }
    }
  }
  if (!Compared)
    GTEST_SKIP() << "native AArch64 transports unavailable";
}

TEST_P(AndroidFileStatusAt, AbsoluteAndEmptyPathsPreserveStatusAndOpenCursors) {
  Options.LinuxFiles->DescriptorLimit = 4;
  for (const char *Entry : {"files_status_at", "files_status_at_bionic"}) {
    auto R = run(Entry);
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Options.Android->Memory[0].Bytes);
  }
}

TEST_P(AndroidFileStatusAt,
       AllABIsPreserveUntouchedBytesAndTranslateOnlyErrno) {
  struct Case {
    uint64_t PathAddress, Destination, Flags, Error;
    const char *Text;
  };
  const Case Cases[] = {{1, Output, 0x40000000, 22, "/fixture/data"},
                        {1, Output, 0, 14, "/fixture/data"},
                        {Name, 1, 0, 2, "/absent"},
                        {Name, 1, 0, 2, "/./absent"},
                        {Name, 1, 0, 20, "/fixture/data/child"},
                        {Name, 1, 0, 20, "/fixture/data/./child"},
                        {Name, Output, 0, 20, "/fixture/data/"},
                        {Name, Output, 0, 20, "/fixture/data////"},
                        {Name, Output, 0, 2, "/missing/"},
                        {Name, Output, 0, 2, "/missing/child///"},
                        {Name, 1, 0, 2, ""},
                        {Name, 1, 0x1000, 9, ""},
                        {Name, 1, 0, 14, "/fixture/data"},
                        {Name, UINT64_MAX, 0, 14, "/fixture/data"}};
  for (uint64_t Route : {0u, 1u, 2u, 3u}) {
    for (const auto &C : Cases) {
      SCOPED_TRACE(testing::Message()
                   << Route << ':' << C.Error << ':' << C.Text);
      setPath(C.Text);
      auto Expected = Options.Android->Memory[0].Bytes;
      llvm::support::endian::write64le(Expected.data(),
                                       Route == 3 ? 83 : C.Error);
      auto R = call(Route, UINT64_MAX, C.PathAddress, C.Destination, C.Flags);
      ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
      EXPECT_EQ(R.ReturnValue, uint64_t(0) - (Route == 3 ? C.Error : 1));
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
    }
  }
}

TEST_P(AndroidFileStatusAt, CompleteStatusCopyMayOverwriteItsPathname) {
  for (uint64_t Route : {0u, 1u, 2u, 3u}) {
    auto Expected = Options.Android->Memory[0].Bytes;
    llvm::support::endian::write64le(Expected.data(), 83);
    std::fill(Expected.begin() + 512, Expected.begin() + 640, 0);
    const std::pair<unsigned, uint64_t> Words[] = {{0, 0xfe12cd34},
                                                   {8, 0xfedcba9876543210ULL},
                                                   {16, 0x89abcdef000081a4ULL},
                                                   {24, 0xfedcba9887654321ULL},
                                                   {56, 16384},
                                                   {64, 0x1234567890ULL},
                                                   {72, 0x8000000000000001ULL},
                                                   {80, 123456789},
                                                   {88, 4294967297},
                                                   {96, 987654321},
                                                   {104, 0x7fffffffffffffffULL},
                                                   {112, 999999999}};
    for (auto [Offset, Value] : Words)
      llvm::support::endian::write64le(Expected.data() + 512 + Offset, Value);
    auto R = call(Route, UINT64_MAX, Name, Name, 0xabcdef0000000900ULL);
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  }
}

TEST_P(AndroidFileStatusAt, InteriorDotPathsPreserveCompleteStatusAndGuards) {
  auto Canonical = call(0);
  ASSERT_EQ(Canonical.Stop, ProcessStopReason::Returned)
      << Canonical.Diagnostic;
  ASSERT_EQ(Canonical.MemorySnapshots.size(), 1u);
  const auto &CanonicalBytes = Canonical.MemorySnapshots[0].Bytes;
  for (const char *Text :
       {"/./fixture/data", "/fixture/./data", "//.//fixture//.//data"}) {
    setPath(Text);
    auto Expected = Options.Android->Memory[0].Bytes;
    llvm::support::endian::write64le(Expected.data(), 83);
    std::copy_n(CanonicalBytes.begin() + 1024, 128, Expected.begin() + 1024);
    for (uint64_t Route : {0u, 1u, 2u, 3u}) {
      SCOPED_TRACE(testing::Message() << Text << ':' << Route);
      auto R = call(Route, UINT64_MAX);
      ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
      EXPECT_EQ(R.ReturnValue, 0u);
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
    }
  }
}

TEST_P(AndroidFileStatusAt, InteriorDotPathsMatchNativeTransport) {
  bool Compared = false;
  for (auto Backend : {ExecutionBackendKind::KVM, ExecutionBackendKind::WHP,
                       ExecutionBackendKind::HVF}) {
    ExecutionConfiguration C;
    C.Backend = Backend;
    C.Architecture = GuestArchitecture::AArch64;
    C.Contract = ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(C);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available)
      continue;
    for (const char *Text : {"/./fixture/./data", "/fixture/./missing",
                             "/fixture/data/./child", "/fixture/."}) {
      setPath(Text);
      for (uint64_t Route : {0u, 1u, 2u, 3u}) {
        SCOPED_TRACE(testing::Message() << Text << ':' << Route);
        Options.Backend = ExecutionBackendKind::Unicorn;
        auto Software = call(Route);
        ASSERT_FALSE(Software.TraceTruncated);
        if (llvm::StringRef(Text) == "/fixture/.")
          ASSERT_EQ(Software.Stop, ProcessStopReason::UnsupportedService);
        else
          ASSERT_EQ(Software.Stop, ProcessStopReason::Returned)
              << Software.Diagnostic;
        Options.Backend = Backend;
        auto Native = call(Route);
        Native.SelectedBackend = Software.SelectedBackend;
        Native.BackendSelectionReason = Software.BackendSelectionReason;
        EXPECT_EQ(processResultJSON(Native), processResultJSON(Software));
        Compared = true;
      }
    }
  }
  if (!Compared)
    GTEST_SKIP() << "native AArch64 transports unavailable";
}

TEST_P(AndroidFileStatusAt,
       UnmodeledObjectsAndVersionSpecificInputsStayPending) {
  struct Case {
    const char *Text;
    uint64_t Directory, PathAddress, Destination, Flags;
    bool Catalogue, Metadata;
  };
  const Case Cases[] = {
      {"/fixture/data", uint64_t(-100), Name, Output, 0, false, false},
      {"/fixture/data", uint64_t(-100), Name, Output, 0, true, false},
      {"/fixture", uint64_t(-100), Name, Output, 0, true, true},
      {"/", uint64_t(-100), Name, Output, 0, true, true},
      {"////", uint64_t(-100), Name, Output, 0, true, true},
      {"/fixture///", uint64_t(-100), Name, Output, 0, true, true},
      {"fixture/data", uint64_t(-100), Name, Output, 0, true, true},
      {"/fixture/../data", uint64_t(-100), Name, Output, 0, true, true},
      {"", uint64_t(-100), Name, Output, 0x1000, true, true},
      {"", 1, Name, Output, 0x1000, true, true},
      {"", 1, 0, Output, 0x1000, true, true},
      {"/fixture/data", uint64_t(-100), Name, Output, 0x2000, true, true},
      {"/fixture/data", uint64_t(-100), Name, Output, 0x4000, true, true},
      {"/fixture/data", uint64_t(-100), Name, Buffer + 8192 - 64, 0, true,
       true}};
  auto Files = Options.LinuxFiles;
  for (uint64_t Route : {0u, 1u, 2u, 3u}) {
    for (const auto &C : Cases) {
      SCOPED_TRACE(testing::Message()
                   << Route << ':' << C.Text << ':' << C.Flags);
      Options.LinuxFiles = Files;
      if (!C.Catalogue)
        Options.LinuxFiles.reset();
      else if (!C.Metadata)
        Options.LinuxFiles->Metadata.clear();
      setPath(C.Text);
      auto R = call(Route, C.Directory, C.PathAddress, C.Destination, C.Flags);
      EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
      EXPECT_FALSE(R.ReturnValue);
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, Options.Android->Memory[0].Bytes);
      if (Route == 3) {
        ASSERT_FALSE(R.Services.empty());
        EXPECT_FALSE(R.Services.back().Result);
      } else {
        ASSERT_FALSE(R.NativeCalls.empty());
        EXPECT_FALSE(R.NativeCalls.back().Result);
      }
    }
  }
}

TEST_P(AndroidFileStatusAt, DynamicAliasesRetainProviderIdentityAndLifetime) {
  Options.Android->Libraries["libfiles.so"] = {"fstatat", "fstatat64"};
  for (uint64_t Alias : {0u, 1u}) {
    auto R = run("files_status_at_dynamic", {0, Alias});
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_EQ(R.NativeCalls.back().Name, Alias ? "fstatat64" : "fstatat");
    EXPECT_EQ(R.NativeCalls.back().Library, "libfiles.so");
    R = run("files_status_at_dynamic", {1, Alias});
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
    EXPECT_FALSE(R.NativeCalls.back().Result);
  }
}

TEST_P(AndroidFileStatusAt, NativeTransportMatchesCompleteReports) {
  bool Compared = false;
  for (auto Backend : {ExecutionBackendKind::KVM, ExecutionBackendKind::WHP,
                       ExecutionBackendKind::HVF}) {
    ExecutionConfiguration C;
    C.Backend = Backend;
    C.Architecture = GuestArchitecture::AArch64;
    C.Contract = ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(C);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available)
      continue;
    Options.LinuxFiles->DescriptorLimit = 4;
    for (const char *Entry :
         {"files_status_at", "files_status_at_bionic", "files_trailing_paths",
          "files_trailing_paths_bionic"}) {
      Options.Backend = ExecutionBackendKind::Unicorn;
      auto Software = run(Entry);
      ASSERT_EQ(Software.Stop, ProcessStopReason::Returned)
          << Software.Diagnostic;
      ASSERT_FALSE(Software.TraceTruncated);
      Options.Backend = Backend;
      auto Native = run(Entry);
      ASSERT_EQ(Native.Stop, ProcessStopReason::Returned) << Native.Diagnostic;
      Native.SelectedBackend = Software.SelectedBackend;
      Native.BackendSelectionReason = Software.BackendSelectionReason;
      EXPECT_EQ(processResultJSON(Native), processResultJSON(Software));
      Compared = true;
    }
  }
  if (!Compared)
    GTEST_SKIP() << "native AArch64 transports unavailable";
}

INSTANTIATE_TEST_SUITE_P(OptimizationAndPacking, AndroidFileStatusAt,
                         testing::Values("files-O0-none", "files-O0-android",
                                         "files-O0-relr", "files-O2-none",
                                         "files-O2-android", "files-O2-relr"));
} // namespace
} // namespace neverd::emulation
