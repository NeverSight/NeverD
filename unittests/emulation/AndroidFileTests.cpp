//===- AndroidFileTests.cpp - Shared guest file state and boundaries ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxFileTestMetadata.h"
#include "gtest/gtest.h"
#include "os/linux/android/AndroidInternal.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"
#include "neverd/loader/ELF/ELFLoader.h"
#include "neverd/loader/ELF/ELFProgramLinking.h"

#include "llvm/Support/Endian.h"

#include <tuple>

namespace neverd::emulation {
namespace {
class AndroidFiles : public testing::TestWithParam<
                         std::tuple<const char *, ExecutionBackendKind>> {
protected:
  std::filesystem::path Path;
  ProcessOptions Options;
  void SetUp() override {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
    GTEST_SKIP() << "Clang and ld.lld Android fixtures unavailable";
#else
    Path = std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
           (std::string(std::get<0>(GetParam())) + ".so");
    Options.Backend = std::get<1>(GetParam());
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
    Options.Android->ThreadLimit = 2;
    Options.LinuxFiles.emplace();
    Options.LinuxFiles->Files["/fixture/data"] = {0,    0xff, 0x41,
                                                  0x0a, 0x80, 0x5a};
    Options.InstructionQuantum = 31;
    Options.Limits.Instructions = 200000;
#endif
  }
  ProcessResult run(const char *Entry, std::vector<uint64_t> Args = {}) {
    Options.Android->EntrySymbol = Entry;
    Options.Android->Arguments = std::move(Args);
    return llvm::cantFail(
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options));
  }
  void returned(const ProcessResult &R, uint64_t Value = 0) {
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, Value);
  }
};

TEST_P(AndroidFiles,
       RenamePreservesObjectsAcrossRawNamedVariadicAndDynamicCalls) {
  Options.LinuxFiles = fileRenameTestInput();
  Options.LinuxTime.emplace().Clocks[0] = {100, 123};
  Options.Android->ThreadLimit = 1;
  Options.Android->Libraries["libfiles.so"] = {"rename", "renameat"};
  auto Valid = linux_model::validateFileOptions(*Options.LinuxFiles);
  ASSERT_FALSE(bool(Valid)) << llvm::toString(std::move(Valid));
  returned(run("files_rename_raw"));
  returned(run("files_rename_reuse", {0}));
  for (unsigned mode = 0; mode != 7; ++mode) {
    SCOPED_TRACE(mode);
    auto r = run("files_rename_bionic", {mode});
    if (mode < 5)
      returned(r);
    else {
      EXPECT_EQ(r.Stop, ProcessStopReason::UnsupportedService);
      EXPECT_NE(r.Diagnostic.find("inactive dynamic library"),
                std::string::npos);
      ASSERT_FALSE(r.NativeCalls.empty());
      EXPECT_EQ(r.NativeCalls.back().Library, "libfiles.so");
      EXPECT_FALSE(r.NativeCalls.back().Result);
    }
  }
}
TEST_P(AndroidFiles, RenameKeepsUnknownProvidersAndOtherPathCasesPending) {
  Options.LinuxFiles = fileRenameTestInput();
  Options.LinuxTime.emplace().Clocks[0] = {100, 123};
  for (unsigned mode = 0; mode != 6; ++mode) {
    auto r = run("files_rename_pending", {mode});
    EXPECT_EQ(r.Stop, ProcessStopReason::UnsupportedService) << r.Diagnostic;
    ASSERT_FALSE(r.Services.empty());
    EXPECT_FALSE(r.Services.back().Result);
  }
  Options.LinuxFiles->FileRenames.reset();
  auto r = run("files_rename_raw");
  EXPECT_EQ(r.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(r.Diagnostic.find("explicit provider"), std::string::npos);
}

TEST_P(AndroidFiles, RenameKeepsFDInfoBoundToTheOpenFileAfterPathReuse) {
  Options.LinuxFiles = fileRenameTestInput();
  Options.LinuxFiles->DynamicProc = LinuxProcOptions{"fixture", 12};
  Options.LinuxTime.emplace().Clocks[0] = {100, 123};
  Options.Android->ThreadLimit = 1;
  auto Valid = linux_model::validateFileOptions(*Options.LinuxFiles);
  ASSERT_FALSE(bool(Valid)) << llvm::toString(std::move(Valid));
  returned(run("files_rename_fdinfo", {0}));
}

TEST_P(AndroidFiles, UnlinkErrorsShareRawNamedVariadicAndDynamicResults) {
  Options.Limits.TimeoutMicroseconds = 30000000;
  Options.Android->ThreadLimit = 1;
  Options.Android->Libraries["libfiles.so"] = {"unlink", "unlinkat"};
  constexpr uint64_t buffer = 0x20000000;
  Options.Android->Memory.push_back(
      {buffer, 4096, std::vector<uint8_t>(4096, 0xa5), false});
  Options.Android->ReadMemory.push_back({buffer, 128});
  returned(run("files_unlink_errors"));
  for (unsigned mode = 0; mode != 7; ++mode) {
    SCOPED_TRACE(mode);
    auto r = run("files_unlink_bionic", {mode, buffer + 8});
    if (mode < 5)
      returned(r);
    else {
      EXPECT_EQ(r.Stop, ProcessStopReason::UnsupportedService);
      EXPECT_NE(r.Diagnostic.find("inactive dynamic library"),
                std::string::npos);
      ASSERT_FALSE(r.NativeCalls.empty());
      EXPECT_EQ(r.NativeCalls.back().Library, "libfiles.so");
      EXPECT_FALSE(r.NativeCalls.back().Result);
    }
    auto expected = std::vector<uint8_t>(128, 0xa5);
    if (mode < 5)
      for (unsigned i = 0; i != 4; ++i) {
        const uint64_t errors[] = {2, 2, 20, 14};
        llvm::support::endian::write64le(expected.data() + 8 + i * 16,
                                         UINT64_MAX);
        llvm::support::endian::write64le(expected.data() + 16 + i * 16,
                                         errors[i]);
      }
    ASSERT_EQ(r.MemorySnapshots.size(), 1u);
    EXPECT_EQ(r.MemorySnapshots[0].Bytes, expected);
  }
}
TEST_P(AndroidFiles,
       UnlinkPreservesExplicitUnknownObjectAndProviderBoundaries) {
  Options.LinuxFiles->SymbolicLinks["/links/raw"] = {'/', 'x'};
  for (unsigned mode = 0; mode != 6; ++mode) {
    auto r = run("files_unlink_pending", {mode});
    EXPECT_EQ(r.Stop, ProcessStopReason::UnsupportedService) << r.Diagnostic;
    ASSERT_FALSE(r.Services.empty());
    EXPECT_FALSE(r.Services.back().Result);
  }
  Options.LinuxFiles.reset();
  auto r = run("files_unlink_errors");
  EXPECT_EQ(r.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(r.Diagnostic.find("explicit linux_files"), std::string::npos);
}

TEST_P(AndroidFiles, UnlinkClosedFileSharesRawNamedVariadicAndDynamicResults) {
  Options.LinuxFiles = fileUnlinkTestInput();
  Options.LinuxTime.emplace().Clocks[0] = {100, 123};
  Options.Android->ThreadLimit = 1;
  Options.Android->Libraries["libfiles.so"] = {"unlink", "unlinkat"};
  returned(run("files_unlink_pending", {0}));
  for (unsigned Mode = 0; Mode != 7; ++Mode) {
    SCOPED_TRACE(Mode);
    auto R = run("files_unlink_closed_bionic", {Mode});
    if (Mode < 5)
      returned(R);
    else {
      EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
      EXPECT_NE(R.Diagnostic.find("inactive dynamic library"),
                std::string::npos);
      ASSERT_FALSE(R.NativeCalls.empty());
      EXPECT_EQ(R.NativeCalls.back().Library, "libfiles.so");
      EXPECT_FALSE(R.NativeCalls.back().Result);
    }
  }
  Options.LinuxFiles->FileUnlinks.reset();
  auto Refused = run("files_unlink_closed_bionic", {0});
  EXPECT_EQ(Refused.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(Refused.Diagnostic.find("explicit provider"), std::string::npos);
  ASSERT_FALSE(Refused.NativeCalls.empty());
  EXPECT_FALSE(Refused.NativeCalls.back().Result);
}

TEST_P(AndroidFiles, FileCreationSharesRawNamedVariadicDynamicAndStdioOwners) {
  Options.LinuxFiles = fileCreationTestInput();
  Options.LinuxTime.emplace().Clocks[0] = {100, 123};
  Options.Android->ThreadLimit = 1;
  Options.Android->Libraries["libfiles.so"] = {"open", "openat"};
  returned(run("files_creation_raw"));
  for (unsigned Mode = 0; Mode != 8; ++Mode) {
    SCOPED_TRACE(Mode);
    auto R = run("files_creation_bionic", {Mode});
    if (Mode != 5 && Mode != 6)
      returned(R);
    else {
      EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
      EXPECT_NE(R.Diagnostic.find("inactive dynamic library"),
                std::string::npos);
      ASSERT_FALSE(R.NativeCalls.empty());
      EXPECT_FALSE(R.NativeCalls.back().Result);
    }
  }
  Options.LinuxFiles->FileCreations.reset();
  auto Refused = run("files_creation_bionic", {0});
  EXPECT_EQ(Refused.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(Refused.Diagnostic.find("explicit object reservation"),
            std::string::npos);
  ASSERT_FALSE(Refused.NativeCalls.empty());
  EXPECT_FALSE(Refused.NativeCalls.back().Result);
}

TEST_P(AndroidFiles, FileCreationCountsShareEveryCallerRoute) {
  Options.LinuxFiles = fileCreationCountTestInput();
  Options.LinuxTime.emplace().Clocks[0] = {100, 123};
  Options.Android->ThreadLimit = 1;
  Options.Android->Libraries["libfiles.so"] = {"openat"};
  returned(run("files_creation_counted_raw"));
  for (unsigned Mode = 0; Mode != 5; ++Mode) {
    SCOPED_TRACE(Mode);
    returned(run("files_creation_counted_bionic", {Mode}));
  }
  Options.LinuxFiles->FileCreations->Files.at("/fixture/data").Count = 1;
  auto Refused = run("files_creation_counted_bionic", {2});
  EXPECT_EQ(Refused.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(Refused.Diagnostic.find("already consumed"), std::string::npos);
  ASSERT_FALSE(Refused.NativeCalls.empty());
  EXPECT_FALSE(Refused.NativeCalls.back().Result);
}

TEST_P(AndroidFiles, OpensOwnCursorsAndReuseTheLowestAvailableDescriptor) {
  returned(run("files_sequence"));
}

TEST_P(AndroidFiles, ReadChkSharesFileCursorsErrnoAndDynamicProviderLifetime) {
  Options.Android->ThreadLimit = 1;
  Options.Android->Libraries["libfiles.so"] = {"__read_chk"};
  for (uint64_t Mode : {0u, 1u})
    returned(run("files_read_chk_sequence", {Mode}));
  auto R = run("files_read_chk_sequence", {2});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_EQ(R.NativeCalls.back().Name, "__read_chk");
  EXPECT_EQ(R.NativeCalls.back().Library, "libfiles.so");
  EXPECT_FALSE(R.NativeCalls.back().Result);
}

TEST_P(AndroidFiles, ReadChkRejectsExtentsBeforeDescriptorMemoryAndErrno) {
  constexpr uint64_t Buffer = 0x20000000;
  const uint64_t Limit = INT64_MAX;
  const std::vector<std::pair<uint64_t, uint64_t>> Cases = {
      {1, 0},
      {4096, 4095},
      {Limit, Limit - 1},
      {Limit + 1, UINT64_MAX},
      {UINT64_MAX, UINT64_MAX}};
  Options.Android->ThreadLimit = 1;
  Options.Android->Memory.push_back(
      {Buffer, 4096, std::vector<uint8_t>(4096, 0xa5), false});
  Options.Android->ReadMemory = {{Buffer, 128},
                                 {android_model::ErrnoAddress, 4}};
  Options.Android->ObserveMemoryOnFailure = true;
  for (auto [Count, Capacity] : Cases) {
    SCOPED_TRACE(Count);
    for (uint64_t Address : {Buffer, uint64_t(1)}) {
      auto R = run("files_read_chk_bounds", {Address, Count, Capacity});
      EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure);
      EXPECT_NE(R.Diagnostic.find(Count > Limit ? "signed byte-count"
                                                : "destination bounds"),
                std::string::npos);
      ASSERT_FALSE(R.NativeCalls.empty());
      EXPECT_EQ(R.NativeCalls.back().Name, "__read_chk");
      EXPECT_FALSE(R.NativeCalls.back().Result);
      ASSERT_EQ(R.MemorySnapshots.size(), 2u);
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, std::vector<uint8_t>(128, 0xa5));
      EXPECT_EQ(R.MemorySnapshots[1].Bytes,
                (std::vector<uint8_t>{77, 0, 0, 0}));
    }
  }

  // Observe backing memory at the owning libc boundary before the session
  // consumes the terminal error. An invalid descriptor would change errno
  // if the call reached the shared kernel read owner.
  ExecutionConfiguration C;
  C.Backend = Options.Backend;
  C.Architecture = GuestArchitecture::AArch64;
  C.Contract = ExecutionContract::CheckedUserAArch64;
  auto Backend = llvm::cantFail(createExecutionBackend(C, 4 * 1024 * 1024));
  auto &CPU = *Backend.CPU;
  auto &Space = *CPU.addressSpace();
  constexpr uint64_t Page = android_model::PageSize;
  llvm::cantFail(Space.map(Buffer, Page, Read | Write | UserAccessible));
  llvm::cantFail(Space.map(android_model::TLSAddress, Page,
                           Read | Write | UserAccessible));
  const std::vector<uint8_t> Bytes(Page, 0xa5);
  auto TLS = Bytes;
  llvm::support::endian::write32le(
      TLS.data() + android_model::ErrnoAddress - android_model::TLSAddress, 77);
  llvm::cantFail(CPU.write(Buffer, Bytes));
  llvm::cantFail(CPU.write(android_model::TLSAddress, TLS));
  const linux_model::MemoryLayout Layout{linux_model::UserLimitARM64, Page};
  ProcessResult Result{ProcessProfile::AndroidNativeAArch64,
                       GuestArchitecture::AArch64,
                       Options.Backend,
                       {}};
  linux_model::LinuxServices Kernel(CPU, Layout, Buffer + Page, Options,
                                    Result);
  const android_model::LinkedImage Linked{};
  for (auto [Count, Capacity] : Cases) {
    SCOPED_TRACE(Count);
    for (uint64_t Address : {Buffer, uint64_t(1)}) {
      auto Budget = llvm::cantFail(ExecutionBudget::create(Options.Limits));
      android_model::Bionic Model(CPU, Kernel, Layout, Options, Result, *Budget,
                                  Linked);
      NativeCallEvent Call{};
      Call.Name = "__read_chk";
      Call.Arguments = {99, Address, Count, Capacity};
      auto Value = Model.invoke(Call);
      ASSERT_FALSE(bool(Value));
      EXPECT_NE(
          llvm::toString(Value.takeError())
              .find(Count > Limit ? "signed byte-count" : "destination bounds"),
          std::string::npos);
      std::vector<uint8_t> After(Page);
      llvm::cantFail(Space.snapshotBacking(Buffer, After));
      EXPECT_EQ(After, Bytes);
      llvm::cantFail(Space.snapshotBacking(android_model::TLSAddress, After));
      EXPECT_EQ(After, TLS);
    }
  }
}

TEST_P(AndroidFiles, ReadChkAdmitsZeroAndTheExactSignedLimitToTheReadOwner) {
  Options.Android->ThreadLimit = 1;
  Options.Android->ReadMemory = {{android_model::ErrnoAddress, 4}};
  for (auto [Count, Capacity] :
       {std::pair<uint64_t, uint64_t>{0, 0}, {INT64_MAX, UINT64_MAX}}) {
    auto R = run("files_read_chk_bounds", {1, Count, Capacity});
    returned(R, UINT64_MAX);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(
        llvm::support::endian::read32le(R.MemorySnapshots[0].Bytes.data()), 9u);
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_EQ(R.NativeCalls.back().Name, "__read_chk");
    EXPECT_EQ(R.NativeCalls.back().Result, UINT64_MAX);
  }
}
TEST_P(AndroidFiles, ReadlinkCopiesObservedBytesThroughRawAndBionicCalls) {
  // The O0 fixture fills a page before checking the pathname length limit.
  Options.Limits.TimeoutMicroseconds = 30000000;
  Options.LinuxFiles->SymbolicLinks["/links/raw"] = {'/', 'x', 0xff, 'y'};
  returned(run("files_links"));
  returned(run("files_links_bionic"));
  Options.Android->Libraries["libfiles.so"] = {"readlink", "readlinkat"};
  for (uint64_t At : {0u, 1u}) {
    auto R = run("files_links_dynamic", {0, At});
    returned(R);
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_EQ(R.NativeCalls.back().Library, "libfiles.so");
    auto Closed = run("files_links_dynamic", {1, At});
    EXPECT_EQ(Closed.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(Closed.Diagnostic.find("inactive dynamic library"),
              std::string::npos);
    EXPECT_FALSE(Closed.NativeCalls.back().Result);
  }
}
TEST_P(AndroidFiles,
       ReadlinkObservationsGrantNoTraversalOrPartialCopyContract) {
  Options.LinuxFiles->SymbolicLinks["/links/raw"] = {'/', 'x', 0xff, 'y'};
  Options.LinuxKernel.emplace().GKI = AndroidGKIKernel::Android16_6_12;
  Options.Android->ThreadLimit = 1;
  constexpr uint64_t Buffer = 0x20000000;
  Options.Android->Memory.push_back(
      {Buffer, 8192, std::vector<uint8_t>(8192, 0xa5), false});
  Options.Android->ReadMemory.push_back({Buffer, 4096});
  for (uint64_t Mode = 0; Mode != 13; ++Mode) {
    SCOPED_TRACE(Mode);
    auto R = run("files_links_pending", {Mode, Buffer});
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
    ASSERT_FALSE(R.Services.empty());
    EXPECT_FALSE(R.Services.back().Result);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, std::vector<uint8_t>(4096, 0xa5));
  }
  Options.LinuxFiles.reset();
  auto R = run("files_links_pending", {0, Buffer});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("explicit linux_files"), std::string::npos);
}
TEST_P(AndroidFiles, ReleasedGKIOpenFlagsSharePathErrorsAndErrno) {
  Options.LinuxFiles->DescriptorLimit = 4;
  Options.Android->ThreadLimit = 1;
  constexpr AndroidGKIKernel Kernels[] = {
#define NEVERD_GKI_RELEASE_CASE(Name, Label, ThreadFlag, SingleBuffer, Error,  \
                                NameFirst)                                     \
  AndroidGKIKernel::Name,
#include "GKIReleaseCases.def"
#undef NEVERD_GKI_RELEASE_CASE
  };
  for (auto Kernel : Kernels) {
    SCOPED_TRACE(unsigned(Kernel));
    Options.LinuxKernel.emplace().GKI = Kernel;
    returned(run("files_open_observed_errors"));
    returned(run("files_open_observed_errors_bionic"));
  }
}
TEST_P(AndroidFiles, OpenFlagsKeepKnownDirectoryAndDirectIOBoundaries) {
  for (uint64_t Mode = 0; Mode != 3; ++Mode) {
    auto R = run("files_open_unobserved", {Mode});
    if (Mode == 2) {
      returned(R);
      ASSERT_EQ(R.Services.size(), 4u);
      EXPECT_EQ(R.Services[0].Result, 3u);
      EXPECT_EQ(R.Services[1].Result, uint64_t(-21));
      EXPECT_EQ(R.Services[2].Result, 0u);
      EXPECT_EQ(R.Services[3].Result, 0u);
    } else {
      EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
      EXPECT_FALSE(R.Services.back().Result);
      EXPECT_NE(R.Diagnostic.find("direct I/O"), std::string::npos);
    }
  }
}
TEST_P(AndroidFiles, ReleasedGKIXAttrsShareRawAndBionicErrorOrder) {
  struct Case {
    AndroidGKIKernel Kernel;
    bool NameFirst;
  };
  constexpr Case Kernels[] = {
#define NEVERD_GKI_RELEASE_CASE(Name, Label, ThreadFlag, SingleBuffer, Error,  \
                                NameFirst)                                     \
  {AndroidGKIKernel::Name, NameFirst},
#include "GKIReleaseCases.def"
#undef NEVERD_GKI_RELEASE_CASE
  };
  Options.Android->ThreadLimit = 1;
  for (auto K : Kernels) {
    SCOPED_TRACE(unsigned(K.Kernel));
    Options.LinuxKernel.emplace().GKI = K.Kernel;
    returned(run("files_xattr_errors", {K.NameFirst}));
    returned(run("files_xattr_errors_bionic", {K.NameFirst}));
  }
}
TEST_P(AndroidFiles, XAttrsKeepUnknownKernelAndExistingObjectBoundaries) {
  auto R = run("files_xattr_errors", {0});
  ASSERT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
  EXPECT_NE(R.Diagnostic.find("released GKI"), std::string::npos);
  Options.LinuxKernel.emplace().GKI = AndroidGKIKernel::Android17_6_18;
  for (uint64_t Mode = 0; Mode != 4; ++Mode) {
    auto E = run("files_xattr_unobserved", {Mode});
    ASSERT_EQ(E.Stop, ProcessStopReason::UnsupportedService) << E.Diagnostic;
    ASSERT_FALSE(E.Services.empty());
    EXPECT_FALSE(E.Services.back().Result);
    EXPECT_NE(E.Diagnostic.find("extended attributes"), std::string::npos);
    if (Mode < 3) {
      auto B = run("files_xattr_unobserved_bionic", {Mode});
      EXPECT_EQ(B.Stop, ProcessStopReason::UnsupportedService) << B.Diagnostic;
      EXPECT_NE(B.Diagnostic.find("extended attributes"), std::string::npos);
    }
  }
}
struct XAttrImportCase {
  LinuxXAttrQueryImport Import;
  uint64_t NameFirst;
};
constexpr XAttrImportCase XAttrImports[] = {
    {LinuxXAttrQueryImport::TargetBeforeName, 0},
    {LinuxXAttrQueryImport::NameBeforeTarget, 1}};

TEST_P(AndroidFiles, XAttrQueryImportSharesRawAndBionicErrorOrders) {
  Options.Android->ThreadLimit = 1;
  auto &Kernel = Options.LinuxKernel.emplace();
  Kernel.UnavailableSyscalls.insert(LinuxUnavailableSyscall::PidFDOpen);
  for (const auto &C : XAttrImports) {
    SCOPED_TRACE(C.NameFirst);
    Kernel.XAttrQueryImport = C.Import;
    returned(run("files_xattr_errors", {C.NameFirst}));
    returned(run("files_xattr_errors_bionic", {C.NameFirst}));
    Kernel.XAttrQueryImport.reset();
    // Both error fixtures reach a raw getxattr before any named xattr call.
    for (const char *Entry :
         {"files_xattr_errors", "files_xattr_errors_bionic"}) {
      SCOPED_TRACE(Entry);
      auto R = run(Entry, {C.NameFirst});
      EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
      EXPECT_NE(R.Diagnostic.find("xattr_query_import"), std::string::npos);
      EXPECT_FALSE(R.ReturnValue);
      ASSERT_FALSE(R.Services.empty());
      EXPECT_EQ(R.Services.back().Number, 8u);
      EXPECT_FALSE(R.Services.back().Result);
    }
    auto B = run("files_xattr_unobserved_bionic", {0});
    EXPECT_EQ(B.Stop, ProcessStopReason::UnsupportedService) << B.Diagnostic;
    EXPECT_NE(B.Diagnostic.find("xattr_query_import"), std::string::npos);
    EXPECT_FALSE(B.ReturnValue);
    EXPECT_TRUE(B.Services.empty());
    ASSERT_FALSE(B.NativeCalls.empty());
    EXPECT_EQ(B.NativeCalls.back().Name, "getxattr");
    EXPECT_FALSE(B.NativeCalls.back().Result);
  }
}

TEST_P(AndroidFiles, XAttrQueryImportKeepsObjectAndSymbolicLinkBoundaries) {
  for (const auto &C : XAttrImports) {
    SCOPED_TRACE(C.NameFirst);
    Options.LinuxKernel.emplace().XAttrQueryImport = C.Import;
    Options.LinuxFiles.emplace().Files["/fixture/data"] = {0};
    for (uint64_t Mode = 0; Mode != 4; ++Mode) {
      SCOPED_TRACE(Mode);
      auto R = run("files_xattr_unobserved", {Mode});
      EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
      EXPECT_NE(R.Diagnostic.find("extended attributes"), std::string::npos);
      EXPECT_FALSE(R.ReturnValue);
      ASSERT_FALSE(R.Services.empty());
      EXPECT_EQ(R.Services.back().Number, Mode == 0   ? 8u
                                          : Mode == 1 ? 9u
                                                      : 10u);
      EXPECT_FALSE(R.Services.back().Result);
      if (Mode < 3) {
        auto B = run("files_xattr_unobserved_bionic", {Mode});
        EXPECT_EQ(B.Stop, ProcessStopReason::UnsupportedService)
            << B.Diagnostic;
        EXPECT_NE(B.Diagnostic.find("extended attributes"), std::string::npos);
        EXPECT_FALSE(B.ReturnValue);
        EXPECT_TRUE(B.Services.empty());
        ASSERT_FALSE(B.NativeCalls.empty());
        EXPECT_EQ(B.NativeCalls.back().Name, Mode == 0   ? "getxattr"
                                             : Mode == 1 ? "lgetxattr"
                                                         : "fgetxattr");
        EXPECT_FALSE(B.NativeCalls.back().Result);
      }
    }
    Options.LinuxFiles->Files.clear();
    for (uint64_t Mode : {0, 1}) {
      SCOPED_TRACE(Mode);
      Options.LinuxFiles->SymbolicLinks.clear();
      const char *Link = Mode ? "/fixture" : "/fixture/data";
      Options.LinuxFiles->SymbolicLinks[Link] = {'/', 't'};
      for (bool Bionic : {false, true}) {
        const char *Entry =
            Bionic ? "files_xattr_unobserved_bionic" : "files_xattr_unobserved";
        SCOPED_TRACE(Entry);
        auto R = run(Entry, {Mode});
        EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService)
            << R.Diagnostic;
        EXPECT_NE(R.Diagnostic.find("symbolic-link"), std::string::npos);
        EXPECT_FALSE(R.ReturnValue);
        if (Bionic) {
          EXPECT_TRUE(R.Services.empty());
          ASSERT_FALSE(R.NativeCalls.empty());
          EXPECT_EQ(R.NativeCalls.back().Name, Mode ? "lgetxattr" : "getxattr");
          EXPECT_FALSE(R.NativeCalls.back().Result);
        } else {
          ASSERT_FALSE(R.Services.empty());
          EXPECT_EQ(R.Services.back().Number, Mode ? 9u : 8u);
          EXPECT_FALSE(R.Services.back().Result);
        }
      }
    }
  }
}
TEST_P(AndroidFiles, PageFaultsPreserveCopiedBytesAndAdvanceOnlyTheirCursor) {
  returned(run("files_faults"));
}
TEST_P(AndroidFiles, ExhaustionHasNoOpenSideEffectsAndCloseReleasesCapacity) {
  Options.LinuxFiles->DescriptorLimit = 4;
  returned(run("files_capacity"));
}
TEST_P(AndroidFiles, ExistenceQueriesPreserveDescriptorCapacityAndCursors) {
  Options.LinuxFiles->DescriptorLimit = 4;
  returned(run("files_access"));
}
TEST_P(AndroidFiles, ExistenceQueriesImportOnlyTheTerminatedUserPath) {
  returned(run("files_access_faults"));
}
TEST_P(AndroidFiles, TrailingSeparatorsRetainDirectoryAndCreationSemantics) {
  Options.LinuxFiles->DescriptorLimit = 4;
  returned(run("files_trailing_paths"));
  returned(run("files_trailing_paths_bionic"));
}
TEST_P(AndroidFiles, FileSystemQueriesPreserveCursorsAndLookupErrorPrecedence) {
  returned(run("files_filesystem_status"));
  returned(run("files_filesystem_status_bionic"));
}
TEST_P(AndroidFiles, WorkingDirectoryRawAndBionicShareBytesButNotReturnABI) {
  Options.LinuxFiles->Directories.push_back("/fixture");
  Options.LinuxFiles->WorkingDirectory = "/fixture";
  returned(run("files_cwd_bionic"));
}
TEST_P(AndroidFiles, MissingWorkingDirectoryRemainsUnknown) {
  auto R = run("files_cwd_unknown");
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("working directory is not supplied"),
            std::string::npos);
}
TEST_P(AndroidFiles, RepeatedSeparatorsUseTheSharedFileNamespace) {
  returned(run("files_repeated_slashes"));
  returned(run("files_repeated_slashes_bionic"));
}
TEST_P(AndroidFiles, TrailingSeparatorsDoNotInventDirectoryStateOrPermissions) {
  for (uint64_t Mode = 0; Mode < 10; ++Mode) {
    SCOPED_TRACE(Mode);
    auto R = run("files_trailing_unsupported", {Mode});
    ASSERT_EQ(R.Services.size(), 1u);
    if (Mode == 0 || Mode == 6) {
      // A known directory can be opened without inventing its stat or access
      // permissions. Each run starts with only the three standard descriptors.
      returned(R, 3);
      EXPECT_EQ(R.Services.front().Number, 56u);
      EXPECT_EQ(R.Services.front().Result, 3u);
      continue;
    }
    if (Mode == 4) {
      returned(R, uint64_t(-20));
      EXPECT_EQ(R.Services.front().Number, 48u);
      EXPECT_EQ(R.Services.front().Result, uint64_t(-20));
      continue;
    }
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
    EXPECT_FALSE(R.ReturnValue);
    EXPECT_FALSE(R.Services.back().Result);
    if (Mode == 1 || Mode == 7) {
      EXPECT_EQ(R.Services.back().Number, 79u);
      EXPECT_NE(R.Diagnostic.find("directory status has no explicit metadata"),
                std::string::npos);
    } else if (Mode == 2 || Mode == 8) {
      EXPECT_EQ(R.Services.back().Number, 34u);
      EXPECT_NE(R.Diagnostic.find("directory creation"), std::string::npos);
    } else {
      EXPECT_EQ(R.Services.back().Number, 48u);
      EXPECT_NE(
          R.Diagnostic.find(Mode == 3 ? "permission checks" : "path form"),
          std::string::npos);
    }
  }
}
TEST_P(AndroidFiles, EmptyCatalogueContainsOnlyTheRootDirectory) {
  Options.LinuxFiles->Files.clear();
  Options.LinuxFiles->DescriptorLimit = 3;
  returned(run("files_access_empty"));
}
TEST_P(AndroidFiles, AccessImportsAndThreadsShareTheCatalogueWithPrivateErrno) {
  auto R = run("files_access_bionic");
  returned(R);
  bool OtherThreadAccess = false;
  for (const auto &Call : R.NativeCalls)
    if (Call.Name == "access" && Call.ThreadID == 1001) {
      EXPECT_EQ(Call.Result, 0u);
      OtherThreadAccess = true;
    }
  EXPECT_TRUE(OtherThreadAccess);
}
TEST_P(AndroidFiles, AccessRequiresExplicitInputsAndDoesNotInferPermissions) {
  for (unsigned Mode = 0; Mode < 8; ++Mode) {
    SCOPED_TRACE(Mode);
    if (!Mode)
      Options.LinuxFiles.reset();
    else if (!Options.LinuxFiles) {
      Options.LinuxFiles.emplace();
      Options.LinuxFiles->Files["/fixture/data"] = {};
      Options.LinuxFiles->Metadata["/fixture/data"] = fileTestMetadata();
    }
    auto R = run("files_access_unsupported", {Mode});
    if (Mode == 2) {
      returned(R);
      ASSERT_EQ(R.Services.size(), 1u);
      EXPECT_EQ(R.Services.front().Number, 48u);
      EXPECT_EQ(R.Services.front().Result, 0u);
      continue;
    }
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
    EXPECT_FALSE(R.ReturnValue);
    ASSERT_FALSE(R.Services.empty());
    EXPECT_EQ(R.Services.back().Number, 48u);
    EXPECT_FALSE(R.Services.back().Result);
  }
}
TEST_P(AndroidFiles, AccessDynamicBindingsRetainProviderIdentityAndLifetime) {
  Options.Android->Libraries["libfiles.so"] = {"access", "faccessat"};
  for (unsigned At = 0; At < 2; ++At) {
    SCOPED_TRACE(At);
    auto R = run("files_access_dynamic", {0, At});
    returned(R);
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_EQ(R.NativeCalls.back().Name, At ? "faccessat" : "access");
    EXPECT_EQ(R.NativeCalls.back().Library, "libfiles.so");
    R = run("files_access_dynamic", {1, At});
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
    EXPECT_FALSE(R.NativeCalls.back().Result);
  }
}
TEST_P(AndroidFiles, ImportsRawTrapsAndOtherThreadsShareTheSameOpenFile) {
  auto R = run("files_bionic");
  returned(R);
  bool OtherThreadRead = false;
  for (const auto &Call : R.NativeCalls)
    if (Call.Name == "read" && Call.ThreadID == 1001) {
      EXPECT_EQ(Call.Arguments[0], 3u);
      EXPECT_EQ(Call.Result, 2u);
      OtherThreadRead = true;
    }
  EXPECT_TRUE(OtherThreadRead);
  ASSERT_FALSE(R.Services.empty());
  EXPECT_EQ(R.Services.front().Number, 63u);
  EXPECT_EQ(R.Services.front().Result, 1u);
}
TEST_P(AndroidFiles, DirectoryLookupErrorsPreserveTheCatalogueAndOpenCursors) {
  Options.LinuxFiles->DescriptorLimit = 4;
  returned(run("files_directory_errors"));
}
TEST_P(AndroidFiles, DirectoryCreationHasSharedStatAndLifetimeAcrossCallABIs) {
  Options.LinuxFiles->Directories = {"/made"};
  auto &M = Options.LinuxFiles->DirectoryCreation.emplace().Parents["/made"];
  M.Device = 7;
  M.Inode = 91;
  M.Mode = 0042775;
  M.UID = 1000;
  M.GID = 1234;
  M.LinkCount = 2;
  M.Size = 40;
  M.BlockSize = 4096;
  M.AccessTime = {3, 4};
  M.ModificationTime = {5, 6};
  M.ChangeTime = {7, 8};
  Options.LinuxTime.emplace().Clocks[0] = {100, 123};
  for (uint64_t ABI : {0u, 1u, 2u})
    returned(run("files_create_directories", {ABI}));
}
TEST_P(AndroidFiles, DirectoryCreationRequiresMoreThanAnExistingParent) {
  for (unsigned Mode = 0; Mode < 5; ++Mode) {
    SCOPED_TRACE(Mode);
    if (!Mode)
      Options.LinuxFiles.reset();
    else if (!Options.LinuxFiles) {
      Options.LinuxFiles.emplace();
      Options.LinuxFiles->Files["/fixture/data"] = {};
    }
    auto R = run("files_directory_unsupported", {Mode});
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
    EXPECT_FALSE(R.ReturnValue);
    ASSERT_FALSE(R.Services.empty());
    EXPECT_EQ(R.Services.back().Number, 34u);
    EXPECT_FALSE(R.Services.back().Result);
    if (Mode == 1 || Mode == 2)
      EXPECT_NE(R.Diagnostic.find("directory creation"), std::string::npos);
  }
}
TEST_P(AndroidFiles, DirectoryImportsKeepEachThreadsErrnoPrivate) {
  auto R = run("files_directory_bionic");
  returned(R);
  bool OtherThreadCall = false;
  for (const auto &Call : R.NativeCalls)
    if (Call.Name == "mkdir" && Call.ThreadID == 1001) {
      EXPECT_EQ(Call.Result, UINT64_MAX);
      OtherThreadCall = true;
    }
  EXPECT_TRUE(OtherThreadCall);
}
TEST_P(AndroidFiles,
       DirectoryDynamicBindingsRetainProviderIdentityAndLifetime) {
  Options.Android->Libraries["libfiles.so"] = {"mkdir", "mkdirat"};
  for (unsigned At = 0; At < 2; ++At) {
    SCOPED_TRACE(At);
    const char *Name = At ? "mkdirat" : "mkdir";
    auto R = run("files_directory_dynamic", {0, At});
    returned(R);
    auto Call = llvm::find_if(R.NativeCalls,
                              [&](const auto &C) { return C.Name == Name; });
    ASSERT_NE(Call, R.NativeCalls.end());
    EXPECT_EQ(Call->Result, UINT64_MAX);
    EXPECT_EQ(Call->Library, "libfiles.so");
    R = run("files_directory_dynamic", {1, At});
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
    EXPECT_FALSE(R.NativeCalls.back().Result);
  }
}
TEST_P(AndroidFiles, ExplicitStatusPreservesEveryFieldAndDescriptorCursor) {
  Options.LinuxFiles->Metadata["/fixture/data"] = fileTestMetadata();
  returned(run("files_status"));
}
TEST_P(AndroidFiles, StatusImportsAndThreadsShareObservationsWithPrivateErrno) {
  Options.LinuxFiles->Metadata["/fixture/data"] = fileTestMetadata();
  auto R = run("files_status_bionic");
  returned(R);
  bool OtherThreadStatus = false;
  for (const auto &Call : R.NativeCalls)
    if (Call.Name == "fstat64" && Call.ThreadID == 1001) {
      EXPECT_EQ(Call.Arguments[0], 3u);
      EXPECT_EQ(Call.Result, 0u);
      OtherThreadStatus = true;
    }
  EXPECT_TRUE(OtherThreadStatus);
}
TEST_P(AndroidFiles, StatusDynamicBindingsRetainProviderIdentityAndLifetime) {
  Options.LinuxFiles->Metadata["/fixture/data"] = fileTestMetadata();
  Options.Android->Libraries["libfiles.so"] = {"fstat64"};
  auto R = run("files_status_dynamic", {0});
  returned(R);
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_EQ(R.NativeCalls.back().Name, "fstat64");
  EXPECT_EQ(R.NativeCalls.back().Library, "libfiles.so");
  R = run("files_status_dynamic", {1});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
  EXPECT_FALSE(R.NativeCalls.back().Result);
}
TEST_P(AndroidFiles, UnknownStatusAndPartialCopiesHaveNoInventedResult) {
  constexpr uint64_t Buffer = 0x20000000;
  const std::vector<uint8_t> Bytes(8192, 0xa5);
  Options.Android->Memory.push_back({Buffer, 8192, Bytes, false});
  Options.Android->ReadMemory.push_back({Buffer, 8192});
  for (unsigned Mode = 0; Mode < 4; ++Mode) {
    SCOPED_TRACE(Mode);
    if (Mode)
      Options.LinuxFiles->Metadata["/fixture/data"] = fileTestMetadata();
    auto R = run("files_status_unsupported", {Mode, Buffer});
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
    EXPECT_FALSE(R.ReturnValue);
    ASSERT_FALSE(R.Services.empty());
    EXPECT_EQ(R.Services.back().Number, 80u);
    EXPECT_FALSE(R.Services.back().Result);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots.front().Bytes, Bytes);
  }
}
TEST_P(AndroidFiles, AbsentInputsAndUnmodeledFormsCannotFabricateAResult) {
  for (unsigned Mode = 0; Mode <= 7; ++Mode) {
    SCOPED_TRACE(Mode);
    if (!Mode)
      Options.LinuxFiles.reset();
    else if (!Options.LinuxFiles) {
      Options.LinuxFiles.emplace();
      Options.LinuxFiles->Files["/fixture/data"] = {1};
    }
    auto R = run("files_unsupported", {Mode});
    ASSERT_EQ(R.Services.size(), Mode == 6 ? 2u : 1u);
    if (Mode == 1 || Mode == 3 || Mode == 5) {
      // Existing files, resolved parent components and known directories
      // require neither new objects nor inferred filesystem metadata.
      returned(R, 3);
      EXPECT_EQ(R.Services.front().Number, 56u);
      EXPECT_EQ(R.Services.front().Result, 3u);
      EXPECT_EQ(R.Services.front().Arguments[2], Mode == 1 ? 0100u : 0u);
      continue;
    }
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
    EXPECT_FALSE(R.ReturnValue);
    EXPECT_FALSE(R.Services.back().Result);
    EXPECT_EQ(R.Services.back().Number, Mode == 4   ? 63u
                                        : Mode == 6 ? 62u
                                                    : 56u);
    if (Mode == 6) {
      EXPECT_EQ(R.Services.front().Number, 56u);
      EXPECT_EQ(R.Services.front().Result, 3u);
    }
    const char *Boundary = Mode == 0   ? "explicit linux_files"
                           : Mode == 4 ? "standard input"
                           : Mode == 6 ? "SEEK_DATA and SEEK_HOLE"
                           : Mode == 7 ? "creation requires an explicit object"
                                       : "path form";
    EXPECT_NE(R.Diagnostic.find(Boundary), std::string::npos);
  }
}
TEST_P(AndroidFiles, DynamicFileBindingsRetainProviderIdentityAndLifetime) {
  Options.Android->Libraries["libfiles.so"] = {"open"};
  auto R = run("files_dynamic", {0});
  returned(R, 3);
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_EQ(R.NativeCalls.back().Name, "open");
  EXPECT_EQ(R.NativeCalls.back().Library, "libfiles.so");
  R = run("files_dynamic", {1});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
  EXPECT_FALSE(R.NativeCalls.back().Result);
}

TEST_P(AndroidFiles, CallableImportEvidenceBelongsToTheExactSymbol) {
  ELFLoader Loader;
  auto Image = llvm::cantFail(Loader.load(Path));
  auto Facts = llvm::cantFail(readELFProgramLinking(Image));
  auto Symbol = llvm::find_if(Facts.Symbols,
                              [](const auto &S) { return S.Name == "open"; });
  ASSERT_NE(Symbol, Facts.Symbols.end());
  const uint64_t Index = Symbol - Facts.Symbols.begin();
  EXPECT_EQ(Symbol->Info & 15, llvm::ELF::STT_NOTYPE);
  auto Offset = [&](uint64_t VA) {
    for (const auto &P : Image.ELFMetadata->ProgramHeaders)
      if (P.Type == llvm::ELF::PT_LOAD && VA >= P.VirtualAddress &&
          VA - P.VirtualAddress < P.FileSize)
        return P.FileOffset + VA - P.VirtualAddress;
    ADD_FAILURE() << "fixture address is outside file-backed PT_LOAD";
    return uint64_t(0);
  };
  uint64_t SymbolTable = 0, PLT = 0, PLTSize = 0;
  for (const auto &D : Facts.Dynamic) {
    if (D.Tag == llvm::ELF::DT_SYMTAB)
      SymbolTable = Offset(D.Value);
    if (D.Tag == llvm::ELF::DT_JMPREL)
      PLT = Offset(D.Value);
    if (D.Tag == llvm::ELF::DT_PLTRELSZ)
      PLTSize = D.Value;
  }
  ASSERT_NE(SymbolTable, 0u);
  ASSERT_NE(PLT, 0u);
  uint64_t CallRelocation = 0;
  for (uint64_t I = PLT; I < PLT + PLTSize; I += 24)
    if (llvm::support::endian::read64le(Image.Raw.data() + I + 8) ==
        (Index << 32 | llvm::ELF::R_AARCH64_JUMP_SLOT))
      CallRelocation = I;
  ASSERT_NE(CallRelocation, 0u);
  EXPECT_TRUE(llvm::any_of(Facts.Relocations, [&](const auto &R) {
    return R.Symbol == Index && R.Type == llvm::ELF::R_AARCH64_GLOB_DAT;
  }));
  Options.Android->EntrySymbol = "files_bionic";
  for (unsigned Mode = 0; Mode < 4; ++Mode) {
    SCOPED_TRACE(Mode);
    auto Modified = Image;
    if (Mode == 1 || Mode == 3)
      llvm::support::endian::write32le(Modified.Raw.data() + CallRelocation + 8,
                                       llvm::ELF::R_AARCH64_GLOB_DAT);
    if (Mode == 2 || Mode == 3)
      Modified.Raw[SymbolTable + Index * 24 + 4] =
          (Symbol->Info & 0xf0) |
          (Mode == 2 ? llvm::ELF::STT_OBJECT : llvm::ELF::STT_FUNC);
    auto RAM = llvm::cantFail(PhysicalMemory::create(Options.MemoryLimit));
    auto Space = llvm::cantFail(AddressSpace::create(RAM, Options.MemoryLimit));
    auto Linked = android_model::loadImage(*Space, Modified, Options);
    if (Mode == 0 || Mode == 3) {
      ASSERT_TRUE(bool(Linked)) << llvm::toString(Linked.takeError());
      unsigned Bindings =
          llvm::count_if(Linked->Imports, [](const auto &Entry) {
            return Entry.second == "open";
          });
      EXPECT_EQ(Bindings, 1u);
    } else {
      ASSERT_FALSE(bool(Linked));
      EXPECT_EQ(llvm::toString(Linked.takeError()),
                "Android native: unmodeled imported data symbol: open");
    }
  }
}

INSTANTIATE_TEST_SUITE_P(
    Compiled, AndroidFiles,
    testing::Combine(testing::Values("files-O0-none", "files-O0-android",
                                     "files-O0-relr", "files-O2-none",
                                     "files-O2-android", "files-O2-relr"),
                     testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::HVF,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP)));
TEST(AndroidFileMemory,
     UnterminatedPathsAndRejectedOpensDoNotConsumeDescriptors) {
  ExecutionConfiguration Configuration;
  Configuration.Backend = ExecutionBackendKind::Unicorn;
  Configuration.Architecture = GuestArchitecture::AArch64;
  Configuration.Contract = ExecutionContract::CheckedUserAArch64;
  auto Probe = probeExecutionBackend(Configuration);
  ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
  if (Probe->Availability != BackendAvailability::Available)
    GTEST_SKIP() << Probe->Reason;
  auto Backend = createExecutionBackend(Configuration, 4 * 1024 * 1024);
  ASSERT_TRUE(bool(Backend)) << llvm::toString(Backend.takeError());
  auto &CPU = *Backend->CPU;
  constexpr uint64_t Buffer = 0x20000000;
  ASSERT_EQ(llvm::toString(CPU.addressSpace()->map(
                Buffer, 4096, Read | Write | UserAccessible)),
            "");
  ASSERT_EQ(llvm::toString(CPU.write(Buffer, std::vector<uint8_t>(4096, 'x'))),
            "");
  ProcessOptions Options;
  Options.LinuxFiles.emplace();
  Options.LinuxFiles->DescriptorLimit = 4;
  Options.LinuxFiles->Files["/data"] = {};
  const linux_model::MemoryLayout Layout{linux_model::UserLimitARM64, 4096};
  ProcessResult Result{ProcessProfile::AndroidNativeAArch64,
                       GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn,
                       {}};
  linux_model::LinuxServices Kernel(CPU, Layout, Buffer + 4096, Options,
                                    Result);
  auto Open = [&] {
    return llvm::cantFail(
        Kernel.handle(linux_model::ServiceKind::OpenAt,
                      {0, 56, {uint64_t(-100), Buffer, 0, 0, 0, 0}, {}}));
  };
  EXPECT_EQ(Open(), uint64_t(-36));
  ASSERT_EQ(llvm::toString(CPU.write(Buffer, {'/', 'd', 'a', 't', 'a', 0})),
            "");
  EXPECT_EQ(Open(), 3u);
  EXPECT_EQ(Open(), uint64_t(-24));
  EXPECT_EQ(llvm::cantFail(Kernel.handle(linux_model::ServiceKind::Read,
                                         {0, 63, {3, 1, 99, 0, 0, 0}, {}})),
            0u);
}
} // namespace
} // namespace neverd::emulation
