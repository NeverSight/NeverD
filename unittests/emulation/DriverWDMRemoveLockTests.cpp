//===- DriverWDMRemoveLockTests.cpp - Genuine WDK remove locks -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercise retail and DBG WDK remove-lock macros, asynchronous producers and
/// device storage retention using original compiled guest driver code.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/emulation/DriverSession.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
#ifdef NEVERD_WDM_REMOVE_LOCK_FIXTURE
struct Fixture {
  const char *Path;
  uint32_t LockSize;
};

std::vector<Fixture> images() {
  std::vector<Fixture> Images{{NEVERD_WDM_REMOVE_LOCK_FIXTURE, 32}};
#ifdef NEVERD_WDM_REMOVE_LOCK_CFG_FIXTURE
  Images.push_back({NEVERD_WDM_REMOVE_LOCK_CFG_FIXTURE, 32});
#endif
#ifdef NEVERD_WDM_REMOVE_LOCK_DBG_FIXTURE
  Images.push_back({NEVERD_WDM_REMOVE_LOCK_DBG_FIXTURE, 120});
#endif
#ifdef NEVERD_WDM_REMOVE_LOCK_DBG_CFG_FIXTURE
  Images.push_back({NEVERD_WDM_REMOVE_LOCK_DBG_CFG_FIXTURE, 120});
#endif
  return Images;
}

DriverOptions options(char Mode = 'W') {
  DriverOptions Options;
  Options.ServiceName = std::string("NeverDRemoveLock") + Mode;
  Options.LoadAddress = 0x190000000;
  Options.Unload = true;
  DriverPnpDevice Device;
  Device.ID = "remove0";
  Device.InitialDevicePower = DevicePowerState::D0;
  Device.InitialSystemPower = SystemPowerState::Working;
  Options.PnpDevices.push_back(Device);
  return Options;
}

DriverRequest pnp(DevicePnpRequest Minor, uint64_t Delay = 0,
                  llvm::StringRef ID = "remove0") {
  DriverRequest Request;
  Request.Kind = DriverRequestKind::Pnp;
  Request.DeviceID = ID.str();
  Request.Pnp = DriverPnpOperation{Minor, DriverBusCompletion{0, Delay}};
  return Request;
}

void lifecycle(DriverOptions &Options, uint64_t RemoveDelay = 0,
               llvm::StringRef ID = "remove0") {
  Options.Requests.push_back(pnp(DevicePnpRequest::Start, 0, ID));
  Options.Requests.push_back(pnp(DevicePnpRequest::QueryRemove, 0, ID));
  Options.Requests.push_back(pnp(DevicePnpRequest::Remove, RemoveDelay, ID));
}

size_t apiCount(const DriverResult &Result, llvm::StringRef Name) {
  return std::count_if(Result.Calls.begin(), Result.Calls.end(),
                       [Name](const auto &Call) { return Call.Name == Name; });
}

size_t apiIndex(const DriverResult &Result, llvm::StringRef Name) {
  for (size_t I = 0; I < Result.Calls.size(); ++I)
    if (Result.Calls[I].Name == Name)
      return I;
  return Result.Calls.size();
}

size_t messageIndex(const DriverResult &Result, llvm::StringRef Text) {
  for (size_t I = 0; I < Result.Messages.size(); ++I)
    if (Result.Messages[I].find(Text.str()) != std::string::npos)
      return I;
  return Result.Messages.size();
}

void clean(const DriverResult &Result, size_t Devices = 1) {
  ASSERT_EQ(Result.Stop, DriverStopReason::Returned) << Result.Diagnostic;
  EXPECT_EQ(Result.NTStatus, 0u);
  EXPECT_TRUE(Result.UnloadCompleted);
  EXPECT_TRUE(Result.Devices.empty());
  ASSERT_EQ(Result.PnpDevices.size(), Devices);
  for (const auto &Device : Result.PnpDevices) {
    EXPECT_EQ(Device.AddDeviceStatus, 0u);
    EXPECT_EQ(Device.PnpState, DevicePnpState::Removed);
    EXPECT_FALSE(Device.ProviderPresent);
    EXPECT_FALSE(Device.Attached);
  }
  for (const auto &Request : Result.Requests) {
    EXPECT_TRUE(Request.Completed);
    EXPECT_EQ(Request.IOStatus, 0u);
  }
  for (const auto &Message : Result.Messages)
    EXPECT_EQ(Message.find("Remove lock: failure"), std::string::npos)
        << Message;
  EXPECT_EQ(apiCount(Result, "IoReleaseRemoveLockAndWaitEx"), Devices);
  EXPECT_EQ(apiCount(Result, "IoDeleteDevice"), Devices);
}

void checkABI(const DriverResult &Result, uint32_t LockSize) {
  for (const auto &Call : Result.Calls) {
    if (Call.Name == "IoInitializeRemoveLockEx") {
      ASSERT_EQ(Call.Arguments.size(), 5u);
      EXPECT_EQ(Call.Arguments[1], 0x4c52444eu);
      EXPECT_EQ(Call.Arguments[2], 0u);
      EXPECT_EQ(Call.Arguments[3], 0u);
      EXPECT_EQ(Call.Arguments[4], LockSize);
    } else if (Call.Name == "IoAcquireRemoveLockEx") {
      ASSERT_EQ(Call.Arguments.size(), 5u);
      EXPECT_NE(Call.Arguments[2], 0u);
      EXPECT_EQ(Call.Arguments[4], LockSize);
      if (LockSize == 32)
        EXPECT_EQ(Call.Arguments[3], 1u);
      else
        EXPECT_GT(Call.Arguments[3], 1u);
    } else if (Call.Name == "IoReleaseRemoveLockEx" ||
               Call.Name == "IoReleaseRemoveLockAndWaitEx") {
      ASSERT_EQ(Call.Arguments.size(), 3u);
      EXPECT_EQ(Call.Arguments[2], LockSize);
    }
  }
  EXPECT_LT(apiIndex(Result, "IoInitializeRemoveLockEx"),
            apiIndex(Result, "IoAttachDeviceToDeviceStack"));
}

void workerHandshake(const DriverResult &Result) {
  EXPECT_LT(messageIndex(Result, "worker late acquire delete-pending"),
            messageIndex(Result, "worker last release"));
  EXPECT_LT(messageIndex(Result, "worker last release"),
            messageIndex(Result, "worker waits after release"));
  EXPECT_LT(messageIndex(Result, "worker waits after release"),
            messageIndex(Result, "drain returned"));
  EXPECT_LT(messageIndex(Result, "drain returned"),
            messageIndex(Result, "device deletion requested"));
  EXPECT_LT(messageIndex(Result, "device deletion requested"),
            messageIndex(Result, "worker resumed after deletion"));
  EXPECT_LT(messageIndex(Result, "worker resumed after deletion"),
            Result.Messages.size());
}

TEST(DriverWDMRemoveLock,
     RetailDebugAndCfgMacrosExecuteAtPreferredAndRebasedAddresses) {
  for (const auto &Image : images())
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image.Path);
      SCOPED_TRACE(Address);
      auto Options = options('S');
      Options.LoadAddress = Address;
      lifecycle(Options);
      auto Result = emulateDriver(Image.Path, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      clean(*Result);
      checkABI(*Result, Image.LockSize);
      EXPECT_EQ(apiCount(*Result, "IoAcquireRemoveLockEx"), 4u);
      EXPECT_EQ(apiCount(*Result, "IoReleaseRemoveLockEx"), 2u);
      EXPECT_EQ(apiCount(*Result, "KeWaitForSingleObject"), 0u);
      for (const auto &Request : Result->Requests)
        EXPECT_EQ(Request.DispatchStatus, 0u);
    }
}

TEST(DriverWDMRemoveLock, RetiredFileIrpAddressesRemainOpaqueReleaseTags) {
  for (const auto &Image : images()) {
    SCOPED_TRACE(Image.Path);
    auto Options = options('S');
    Options.Requests.push_back(pnp(DevicePnpRequest::Start));
    for (auto Kind :
         {DriverRequestKind::Create, DriverRequestKind::DeviceControl,
          DriverRequestKind::Cleanup, DriverRequestKind::Close}) {
      DriverRequest Request;
      Request.Kind = Kind;
      Request.File = 9;
      if (Kind == DriverRequestKind::Create)
        Request.DeviceID = "remove0";
      if (Kind == DriverRequestKind::DeviceControl) {
        Request.ControlCode = 0x222000;
        Request.OutputSize = 4;
      }
      Options.Requests.push_back(Request);
    }
    Options.Requests.push_back(pnp(DevicePnpRequest::QueryRemove));
    Options.Requests.push_back(pnp(DevicePnpRequest::Remove));
    auto Result = emulateDriver(Image.Path, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    ASSERT_EQ(Result->Requests.size(), 7u);
    EXPECT_EQ(Result->Requests[2].Output,
              (std::vector<uint8_t>{'L', 'O', 'C', 'K'}));
    for (size_t I : {1u, 2u, 3u, 4u}) {
      const auto IRP = Result->Requests[I].IRP;
      auto Complete = std::find_if(
          Result->Calls.begin(), Result->Calls.end(), [IRP](const auto &Call) {
            return Call.Name == "IofCompleteRequest" &&
                   !Call.Arguments.empty() && Call.Arguments[0] == IRP;
          });
      ASSERT_NE(Complete, Result->Calls.end());
      auto Release =
          std::find_if(Complete, Result->Calls.end(), [IRP](const auto &Call) {
            return Call.Name == "IoReleaseRemoveLockEx" &&
                   Call.Arguments.size() == 3 && Call.Arguments[1] == IRP;
          });
      EXPECT_NE(Release, Result->Calls.end());
    }
  }
}

TEST(DriverWDMRemoveLock, LastReleaseWakesRemovalBeforeWorkerCallbackReturns) {
  for (const auto &Image : images()) {
    SCOPED_TRACE(Image.Path);
    auto Options = options();
    lifecycle(Options);
    auto Result = emulateDriver(Image.Path, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    checkABI(*Result, Image.LockSize);
    workerHandshake(*Result);
    ASSERT_EQ(Result->Requests.size(), 3u);
    ASSERT_TRUE(Result->Requests[2].Pnp);
    EXPECT_EQ(Result->Requests[2].Pnp->BusCompletedAt100ns, 0u);
    EXPECT_EQ(Result->Requests[2].DispatchStatus, 0u);
  }
}

TEST(DriverWDMRemoveLock,
     WorkerDrainDoesNotWaitForDelayedLowerRemoveCompletion) {
  for (const auto &Image : images()) {
    SCOPED_TRACE(Image.Path);
    auto Options = options();
    lifecycle(Options, 20);
    auto Result = emulateDriver(Image.Path, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    workerHandshake(*Result);
    ASSERT_EQ(Result->Requests.size(), 3u);
    ASSERT_TRUE(Result->Requests[2].Pnp);
    EXPECT_EQ(Result->Requests[2].Pnp->BusReceivedAt100ns, 0u);
    EXPECT_EQ(Result->Requests[2].Pnp->BusCompletedAt100ns, 20u);
    EXPECT_EQ(Result->Requests[2].DispatchStatus, 0x103u);
  }
}

TEST(DriverWDMRemoveLock,
     ExplicitLowerCompletionWaitPrecedesIndependentLockDrain) {
  for (const auto &Image : images()) {
    SCOPED_TRACE(Image.Path);
    auto Options = options('C');
    lifecycle(Options, 3);
    auto Result = emulateDriver(Image.Path, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    workerHandshake(*Result);
    EXPECT_LT(messageIndex(*Result, "explicit lower completion wait ended"),
              messageIndex(*Result, "worker last release"));
    ASSERT_TRUE(Result->Requests.back().Pnp);
    EXPECT_EQ(Result->Requests.back().Pnp->BusCompletedAt100ns, 3u);
    EXPECT_EQ(Result->Requests.back().DispatchStatus, 0u);
  }
}

TEST(DriverWDMRemoveLock, DpcAcquisitionAndFinalReleaseExecuteAtDispatchLevel) {
  for (const auto &Image : images())
    for (uint64_t Delay : {0u, 20u}) {
      SCOPED_TRACE(Image.Path);
      SCOPED_TRACE(Delay);
      auto Options = options('D');
      lifecycle(Options, Delay);
      auto Result = emulateDriver(Image.Path, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      clean(*Result);
      EXPECT_LT(messageIndex(*Result, "DPC acquired and released at IRQL2"),
                messageIndex(*Result, "DPC last release at IRQL2"));
      EXPECT_LT(messageIndex(*Result, "DPC last release at IRQL2"),
                messageIndex(*Result, "drain returned"));
      EXPECT_EQ(apiCount(*Result, "IoQueueWorkItem"), 0u);
    }
}

TEST(DriverWDMRemoveLock, RepeatedAndNullTagsEachRequireTheirOwnRelease) {
  for (const auto &Image : images())
    for (char Mode : {'N', 'R'}) {
      SCOPED_TRACE(Image.Path);
      SCOPED_TRACE(Mode);
      auto Options = options(Mode);
      lifecycle(Options, 20);
      auto Result = emulateDriver(Image.Path, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      clean(*Result);
      workerHandshake(*Result);
      EXPECT_LT(
          messageIndex(*Result, "first duplicate release kept remover waiting"),
          messageIndex(*Result, "worker last release"));
      if (Mode == 'N') {
        size_t NullReleases = 0;
        for (const auto &Call : Result->Calls)
          if (Call.Name == "IoReleaseRemoveLockEx" && Call.Arguments[1] == 0)
            ++NullReleases;
        EXPECT_EQ(NullReleases, 2u);
      }
    }
}

TEST(DriverWDMRemoveLock, UnusedInitializedLockAllowsFailedAddDeviceCleanup) {
  for (const auto &Image : images()) {
    SCOPED_TRACE(Image.Path);
    auto Result = emulateDriver(Image.Path, options('F'));
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    ASSERT_EQ(Result->PnpDevices.size(), 1u);
    EXPECT_EQ(Result->PnpDevices[0].AddDeviceStatus, 0xc000009au);
    EXPECT_FALSE(Result->PnpDevices[0].ProviderPresent);
    EXPECT_TRUE(Result->Devices.empty());
    EXPECT_TRUE(Result->Requests.empty());
    EXPECT_TRUE(Result->UnloadCompleted);
    EXPECT_EQ(apiCount(*Result, "IoInitializeRemoveLockEx"), 1u);
    EXPECT_EQ(apiCount(*Result, "IoDeleteDevice"), 1u);
    EXPECT_EQ(apiCount(*Result, "IoAttachDeviceToDeviceStack"), 0u);
    EXPECT_EQ(apiCount(*Result, "IoAcquireRemoveLockEx"), 0u);
  }
}

TEST(DriverWDMRemoveLock, IndependentPdoLocksCanReuseTheSameNullTag) {
  for (const auto &Image : images()) {
    SCOPED_TRACE(Image.Path);
    auto Options = options('N');
    auto Second = Options.PnpDevices.front();
    Second.ID = "remove1";
    Options.PnpDevices.push_back(Second);
    lifecycle(Options, 20, "remove0");
    lifecycle(Options, 20, "remove1");
    auto Result = emulateDriver(Image.Path, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result, 2);
    checkABI(*Result, Image.LockSize);
    ASSERT_EQ(Result->Requests.size(), 6u);
    EXPECT_NE(Result->PnpDevices[0].PDO, Result->PnpDevices[1].PDO);
    EXPECT_EQ(apiCount(*Result, "IoInitializeRemoveLockEx"), 2u);
    EXPECT_LT(messageIndex(*Result, "added unit=2 mode=N"),
              Result->Messages.size());
  }
}

TEST(DriverWDMRemoveLock,
     UnreleasedAcquisitionWithoutProducerReportsStalledWait) {
  for (const auto &Image : images()) {
    SCOPED_TRACE(Image.Path);
    auto Options = options('L');
    lifecycle(Options);
    auto Result = emulateDriver(Image.Path, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
    EXPECT_NE(Result->Diagnostic.find("stalled"), std::string::npos)
        << Result->Diagnostic;
    EXPECT_FALSE(Result->UnloadCompleted);
    EXPECT_EQ(apiCount(*Result, "IoDeleteDevice"), 0u);
  }
}

TEST(DriverWDMRemoveLock, AndWaitOutsideRemoveIsRejectedBeforeDeletion) {
  for (const auto &Image : images()) {
    SCOPED_TRACE(Image.Path);
    auto Options = options('I');
    Options.Requests.push_back(pnp(DevicePnpRequest::Start));
    auto Result = emulateDriver(Image.Path, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
    EXPECT_NE(Result->Diagnostic.find("REMOVE"), std::string::npos)
        << Result->Diagnostic;
    EXPECT_EQ(apiCount(*Result, "IoDeleteDevice"), 0u);
  }
}

TEST(DriverWDMRemoveLock, AndWaitAtDispatchLevelIsRejectedBeforeDeletion) {
  for (const auto &Image : images()) {
    SCOPED_TRACE(Image.Path);
    auto Options = options('P');
    lifecycle(Options);
    auto Result = emulateDriver(Image.Path, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
    EXPECT_NE(Result->Diagnostic.find("IRQL"), std::string::npos)
        << Result->Diagnostic;
    EXPECT_EQ(apiCount(*Result, "IoDeleteDevice"), 0u);
  }
}
#else
TEST(DriverWDMRemoveLock, GenuineWdkFixtureRequiresExplicitConfiguration) {
  GTEST_SKIP()
      << "Set NEVERD_WDM_REMOVE_LOCK_FIXTURE and optional CFG/DBG paths "
         "to execute original driver code built with real WDK headers";
}
#endif
} // namespace
} // namespace neverd::emulation
