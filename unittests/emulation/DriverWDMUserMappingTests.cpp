//===- DriverWDMUserMappingTests.cpp - Genuine WDK user mapping execution ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "fixtures/driver_user_mapping.h"
#include "gtest/gtest.h"
#include "windows/WindowsKernelLayout.h"

#include "neverd/emulation/DriverSession.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
#ifdef NEVERD_WDM_USER_MAPPING_FIXTURE
std::vector<const char *> images() {
  return {NEVERD_WDM_USER_MAPPING_FIXTURE,
#ifdef NEVERD_WDM_USER_MAPPING_CFG_FIXTURE
          NEVERD_WDM_USER_MAPPING_CFG_FIXTURE
#endif
  };
}

DriverOptions options(uint32_t Action, uint64_t LoadAddress) {
  DriverOptions Options;
  Options.LoadAddress = LoadAddress;
  Options.Unload = true;
  for (auto Kind : {DriverRequestKind::Create, DriverRequestKind::DeviceControl,
                    DriverRequestKind::Cleanup, DriverRequestKind::Close}) {
    DriverRequest Request;
    Request.Kind = Kind;
    Request.Device = "\\Device\\NeverDUserMapping";
    Options.Requests.push_back(std::move(Request));
  }
  auto &Request = Options.Requests[1];
  Request.ControlCode = Action;
  Request.Input.resize(sizeof(DriverUserMappingRequest));
  Request.UserBuffers = {{"report", UserMappingReportSize, {}}};
  Request.UserPointers = {{{DriverUserBufferKind::Input, {}, 0},
                           {DriverUserBufferKind::Memory, "report", 0}}};
  return Options;
}

void expectLifecycle(const DriverResult &Result,
                     llvm::ArrayRef<uint8_t> Expected, bool Revoked = false) {
  ASSERT_EQ(Result.Stop, DriverStopReason::Returned) << Result.Diagnostic;
  ASSERT_EQ(Result.Requests.size(), 4u);
  for (const auto &Request : Result.Requests) {
    EXPECT_TRUE(Request.Completed);
    EXPECT_EQ(Request.IOStatus, windows::StatusSuccess);
  }
  const auto &IO = Result.Requests[1];
  ASSERT_EQ(IO.UserBuffers.size(), 1u);
  EXPECT_EQ(IO.UserBuffers[0].Backing,
            std::vector<uint8_t>(Expected.begin(), Expected.end()));
  EXPECT_EQ(IO.UserBuffers[0].Revoked, Revoked);
  EXPECT_TRUE(Result.UnloadCompleted);
  EXPECT_FALSE(Result.Fault);
}

TEST(DriverWDMUserMapping,
     IndependentAllocatedPagesShareMappingsAndReleaseSeparately) {
  for (const auto *Image : images())
    for (uint64_t LoadAddress : {0x180000000ULL, 0x190000000ULL})
      for (uint32_t Action :
           {UserMappingIndependentPages, UserMappingLegacyPages,
            UserMappingContiguousChunks}) {
        SCOPED_TRACE(Image);
        SCOPED_TRACE(LoadAddress);
        SCOPED_TRACE(Action);
        auto Result = emulateDriver(Image, options(Action, LoadAddress));
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        expectLifecycle(*Result,
                        {1, UserMappingFirstByte, UserMappingSecondByte, 1});
        EXPECT_TRUE(std::any_of(Result->Calls.begin(), Result->Calls.end(),
                                [](const auto &Call) {
                                  return Call.Name == "MmFreePagesFromMdl";
                                }));
        EXPECT_TRUE(std::any_of(Result->Calls.begin(), Result->Calls.end(),
                                [](const auto &Call) {
                                  return Call.Name == "ExFreePool" ||
                                         (Call.Name == "ExFreePoolWithTag" &&
                                          Call.Arguments[1] == 0);
                                }));
      }
}

TEST(DriverWDMUserMapping, KernelPoolProbeKeepsPagedMemoryResidentAtDispatch) {
  for (const auto *Image : images())
    for (uint64_t LoadAddress : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(LoadAddress);
      auto Result =
          emulateDriver(Image, options(UserMappingKernelPoolLock, LoadAddress));
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      expectLifecycle(*Result,
                      {UserMappingFirstByte, UserMappingFirstByte + 1, 1, 1});
    }
}

TEST(DriverWDMUserMapping, IrpCompletionReleasesOnlyItsBorrowedPartialPages) {
  for (const auto *Image : images())
    for (uint64_t LoadAddress : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(LoadAddress);
      auto Result = emulateDriver(
          Image, options(UserMappingIndependentPartial, LoadAddress));
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      expectLifecycle(*Result, {1, UserMappingFirstByte, 1, 1});
      const auto Free = std::find_if(
          Result->Calls.begin(), Result->Calls.end(),
          [](const auto &Call) { return Call.Name == "MmFreePagesFromMdl"; });
      ASSERT_NE(Free, Result->Calls.end());
      ASSERT_NE(Free, Result->Calls.begin());
      EXPECT_EQ(std::prev(Free)->Name, "IofCompleteRequest");
    }
}

TEST(DriverWDMUserMapping, PoolAliasesCatchReadOnlyFaultAndRelockTheSamePages) {
  for (const auto *Image : images())
    for (uint64_t LoadAddress : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(LoadAddress);
      auto Result =
          emulateDriver(Image, options(UserMappingPoolAliases, LoadAddress));
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      expectLifecycle(*Result,
                      {UserMappingFirstByte, UserMappingSecondByte, 1, 7});
      const auto MappingCount = [&](uint8_t Mode) {
        return std::count_if(
            Result->Calls.begin(), Result->Calls.end(), [&](const auto &Call) {
              return Call.Name == "MmMapLockedPagesSpecifyCache" &&
                     uint8_t(Call.Arguments[1]) == Mode;
            });
      };
      EXPECT_EQ(MappingCount(windows::UserMode), 2);
      EXPECT_EQ(MappingCount(windows::KernelMode), 1);
    }
}

TEST(DriverWDMUserMapping, AttachedWorkerMapsAndUnmapsInTheRequestingProcess) {
  for (const auto *Image : images())
    for (uint64_t LoadAddress : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(LoadAddress);
      auto Result =
          emulateDriver(Image, options(UserMappingAttachedWorker, LoadAddress));
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      expectLifecycle(*Result, {UserMappingWorkerByte, 1, 0, 0});
      EXPECT_EQ(Result->Requests[1].DispatchStatus, windows::StatusPending);
      EXPECT_TRUE(std::any_of(Result->Calls.begin(), Result->Calls.end(),
                              [](const auto &Call) {
                                return Call.Name == "KeStackAttachProcess";
                              }));
    }
}

TEST(DriverWDMUserMapping, ProcessExitRevokesViewsButWorkerKeepsLockedReport) {
  for (const auto *Image : images())
    for (uint64_t LoadAddress : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(LoadAddress);
      auto Options = options(UserMappingProcessExit, LoadAddress);
      Options.Requests[1].RequestorExitAfterDispatch = true;
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      expectLifecycle(*Result, {UserMappingWorkerByte, 1, 0, 0}, true);
      EXPECT_EQ(Result->Requests[1].DispatchStatus, windows::StatusPending);
    }
}

TEST(DriverWDMUserMapping, UnattachedWorkerCannotUnmapAnotherProcessesView) {
  for (const auto *Image : images())
    for (uint64_t LoadAddress : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(LoadAddress);
      auto Result = emulateDriver(
          Image, options(UserMappingWrongProcessUnmap, LoadAddress));
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
      EXPECT_FALSE(Result->Requests[1].Completed);
      EXPECT_FALSE(Result->UnloadCompleted);
      ASSERT_FALSE(Result->Calls.empty());
      EXPECT_EQ(Result->Calls.back().Name, "MmUnmapLockedPages");
    }
}

TEST(DriverWDMUserMapping, MappingShortageReachesTheGuestExceptionHandler) {
  for (const auto *Image : images())
    for (uint64_t LoadAddress : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(LoadAddress);
      auto Options = options(UserMappingShortage, LoadAddress);
      Options.MemoryLimit = 4 * 1024 * 1024;
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      expectLifecycle(*Result, {1, 0, 0, 0});
    }
}
TEST(DriverWDMUserMapping, PartialMdlUsesGenuinePrepareAndSafeMappingInlines) {
  for (const auto *Image : images())
    for (uint64_t LoadAddress : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(LoadAddress);
      auto Result =
          emulateDriver(Image, options(UserMappingPartialReuse, LoadAddress));
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      expectLifecycle(*Result,
                      {UserMappingFirstByte, UserMappingSecondByte,
                       PartialOriginalRange | PartialPhysicalPages |
                           PartialPreparedDescriptor | PartialReusedRange,
                       PartialInheritedMapping | PartialInheritedPrepare |
                           PartialSourceReleased | PartialDescendantSurvives});
      auto Calls = [&](llvm::StringRef Name) {
        return std::count_if(
            Result->Calls.begin(), Result->Calls.end(),
            [&](const auto &Call) { return Call.Name == Name; });
      };
      EXPECT_EQ(Calls("IoBuildPartialMdl"), 4);
      // One user view, then two private partial system mappings. Borrowed
      // mappings go through the WDK inline's field fast path.
      EXPECT_EQ(Calls("MmMapLockedPagesSpecifyCache"), 3);
      EXPECT_EQ(Calls("MmUnmapLockedPages"), 3);
      EXPECT_EQ(Calls("MmProbeAndLockPages"), 1);
      EXPECT_EQ(Calls("MmUnlockPages"), 1);
      EXPECT_EQ(Calls("IoFreeMdl"), 4);
    }
}

TEST(DriverWDMUserMapping,
     ReusedUserAddressCatchesStaleAndReadOnlyAccessWithoutRetargetingLocks) {
  for (const auto *Image : images())
    for (uint64_t LoadAddress : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(LoadAddress);
      auto Result =
          emulateDriver(Image, options(UserMappingAddressReuse, LoadAddress));
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      expectLifecycle(*Result,
                      {UserMappingFirstByte, UserMappingSecondByte,
                       UserRemappingUnmappedFault | UserRemappingReadOnlyFault |
                           UserRemappingReusedAddress |
                           UserRemappingPreservedLockedPages,
                       1});
      unsigned RequestedMappings = 0;
      for (const auto &Call : Result->Calls)
        if (Call.Name == "MmMapLockedPagesSpecifyCache" &&
            uint8_t(Call.Arguments[1]) == windows::UserMode &&
            Call.Arguments[3])
          ++RequestedMappings;
      EXPECT_EQ(RequestedMappings, 2u);
    }
}

TEST(DriverWDMUserMapping,
     ConcurrentProcessesShareAVirtualAddressWithoutSharingPagesOrPermissions) {
  for (const auto *Image : images())
    for (uint64_t LoadAddress : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(LoadAddress);
      auto Options = options(UserMappingRetainProcessView, LoadAddress);
      auto Second = Options.Requests[1];
      Second.ControlCode = UserMappingConcurrentProcessView;
      Second.RequestorProcessID = DriverRequest::DefaultRequestorProcessID + 1;
      Options.Requests.insert(Options.Requests.begin() + 2, std::move(Second));
      auto ReleaseFirst = Options.Requests[1];
      ReleaseFirst.ControlCode = UserMappingReleaseFirstProcessView;
      auto ReleaseSecond = Options.Requests[2];
      ReleaseSecond.ControlCode = UserMappingReleaseSecondProcessView;
      Options.Requests.insert(Options.Requests.begin() + 3,
                              std::move(ReleaseFirst));
      Options.Requests.insert(Options.Requests.begin() + 4,
                              std::move(ReleaseSecond));
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
      ASSERT_EQ(Result->Requests.size(), 7u);
      for (const auto &Request : Result->Requests) {
        EXPECT_TRUE(Request.Completed);
        EXPECT_EQ(Request.IOStatus, windows::StatusSuccess);
      }
      ASSERT_EQ(Result->Requests[1].UserBuffers.size(), 1u);
      ASSERT_EQ(Result->Requests[2].UserBuffers.size(), 1u);
      ASSERT_EQ(Result->Requests[3].UserBuffers.size(), 1u);
      ASSERT_EQ(Result->Requests[4].UserBuffers.size(), 1u);
      EXPECT_EQ(Result->Requests[1].UserBuffers[0].Backing,
                (std::vector<uint8_t>{1, 0, 0, 0}));
      EXPECT_EQ(Result->Requests[2].UserBuffers[0].Backing,
                (std::vector<uint8_t>{1, 0, 0, 0}));
      EXPECT_EQ(Result->Requests[3].UserBuffers[0].Backing,
                (std::vector<uint8_t>{UserMappingFirstByte,
                                      UserMappingSecondByte, 1, 1}));
      EXPECT_EQ(Result->Requests[4].UserBuffers[0].Backing,
                (std::vector<uint8_t>{UserMappingSecondByte, 1, 1, 0}));
      EXPECT_TRUE(Result->UnloadCompleted);
      EXPECT_FALSE(Result->Fault);
    }
}
#else
TEST(DriverWDMUserMapping, GenuineWDKFixtureIsOptional) {
  GTEST_SKIP()
      << "set NEVERD_WDM_USER_MAPPING_FIXTURE to the genuine WDK driver";
}
#endif
} // namespace
} // namespace neverd::emulation
