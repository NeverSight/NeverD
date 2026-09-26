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
#else
TEST(DriverWDMUserMapping, GenuineWDKFixtureIsOptional) {
  GTEST_SKIP()
      << "set NEVERD_WDM_USER_MAPPING_FIXTURE to the genuine WDK driver";
}
#endif
} // namespace
} // namespace neverd::emulation
