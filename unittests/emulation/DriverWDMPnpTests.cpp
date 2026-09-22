//===- DriverWDMPnpTests.cpp - Genuine AddDevice and PnP execution ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Execute an optional original WDK function driver against explicit provider
/// responses, preserving real completion waits and removal ownership.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "neverd/emulation/DriverSession.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
#ifdef NEVERD_WDM_PNP_FIXTURE
constexpr uint32_t Failed = 0xc0000001;

std::vector<const char *> pnpImages() {
  std::vector<const char *> Images{NEVERD_WDM_PNP_FIXTURE};
#ifdef NEVERD_WDM_PNP_CFG_FIXTURE
  Images.push_back(NEVERD_WDM_PNP_CFG_FIXTURE);
#endif
  return Images;
}

DriverOptions pnpOptions(char Mode = 'S') {
  DriverOptions Options;
  Options.ServiceName = std::string("NeverDPnp") + Mode;
  Options.LoadAddress = 0x190000000;
  Options.Unload = true;
  DriverPnpDevice Device;
  Device.ID = "resource0";
  Device.InitialDevicePower = DevicePowerState::D0;
  Device.InitialSystemPower = SystemPowerState::Working;
  Options.PnpDevices.push_back(Device);
  return Options;
}

DriverRequest pnpRequest(DevicePnpRequest Minor, uint32_t Status = 0,
                         uint64_t Delay = 0) {
  DriverRequest Request;
  Request.Kind = DriverRequestKind::Pnp;
  Request.DeviceID = "resource0";
  Request.Pnp = DriverPnpOperation{Minor, DriverBusCompletion{Status, Delay}};
  return Request;
}

void appendFileRequests(DriverOptions &Options) {
  for (auto Kind : {DriverRequestKind::Create, DriverRequestKind::DeviceControl,
                    DriverRequestKind::Cleanup, DriverRequestKind::Close}) {
    DriverRequest Request;
    Request.Kind = Kind;
    Request.File = 9;
    if (Kind == DriverRequestKind::Create)
      Request.DeviceID = "resource0";
    if (Kind == DriverRequestKind::DeviceControl) {
      Request.ControlCode = 0x222000;
      Request.OutputSize = 4;
    }
    Options.Requests.push_back(Request);
  }
}

size_t apiCount(const DriverResult &Result, llvm::StringRef Name) {
  return std::count_if(Result.Calls.begin(), Result.Calls.end(),
                       [Name](const auto &Call) { return Call.Name == Name; });
}

void cleanRemoval(const DriverResult &Result) {
  ASSERT_EQ(Result.Stop, DriverStopReason::Returned) << Result.Diagnostic;
  EXPECT_EQ(Result.NTStatus, 0u);
  EXPECT_TRUE(Result.UnloadCompleted);
  EXPECT_TRUE(Result.Devices.empty());
  ASSERT_EQ(Result.PnpDevices.size(), 1u);
  const auto &Device = Result.PnpDevices[0];
  EXPECT_EQ(Device.ID, "resource0");
  EXPECT_NE(Device.PDO, 0u);
  EXPECT_EQ(Device.AddDeviceStatus, 0u);
  EXPECT_EQ(Device.PnpState, DevicePnpState::Removed);
  EXPECT_FALSE(Device.ProviderPresent);
  EXPECT_FALSE(Device.Attached);
  for (const auto &Message : Result.Messages)
    EXPECT_EQ(Message.find("WDM PnP: failure"), std::string::npos) << Message;
  for (const auto &Request : Result.Requests)
    EXPECT_TRUE(Request.Completed);
  EXPECT_EQ(apiCount(Result, "IoAttachDeviceToDeviceStack"), 1u);
  EXPECT_EQ(apiCount(Result, "IoDetachDevice"), 1u);
  EXPECT_EQ(apiCount(Result, "IoDeleteDevice"), 1u);
}

TEST(DriverWDMPnp, AddStartFileRequestsAndOrderlyRemovalExecuteRealGuestCode) {
  for (const auto *Image : pnpImages()) {
    SCOPED_TRACE(Image);
    auto Options = pnpOptions();
    Options.Requests.push_back(pnpRequest(DevicePnpRequest::Start));
    appendFileRequests(Options);
    Options.Requests.push_back(pnpRequest(DevicePnpRequest::QueryRemove));
    Options.Requests.push_back(pnpRequest(DevicePnpRequest::Remove));
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    cleanRemoval(*Result);
    ASSERT_EQ(Result->Requests.size(), 7u);
    EXPECT_EQ(Result->Requests[2].Output,
              (std::vector<uint8_t>{'P', 'N', 'P', '!'}));
    for (const auto &Request : Result->Requests) {
      EXPECT_EQ(Request.IOStatus, 0u);
      EXPECT_EQ(Request.DispatchStatus, 0u);
    }
    ASSERT_TRUE(Result->Requests[0].Pnp);
    EXPECT_EQ(Result->Requests[0].Pnp->StateBefore, DevicePnpState::NotStarted);
    EXPECT_EQ(Result->Requests[0].Pnp->StateAfter, DevicePnpState::Started);
    EXPECT_EQ(apiCount(*Result, "IofCallDriver"), 3u);
    EXPECT_EQ(apiCount(*Result, "KeWaitForSingleObject"), 0u);
  }
}

TEST(DriverWDMPnp, DelayedStartWaitsAndRemoveDeletesFdoBeforeBusCompletion) {
  for (const auto *Image : pnpImages()) {
    SCOPED_TRACE(Image);
    auto Options = pnpOptions();
    Options.Requests = {pnpRequest(DevicePnpRequest::Start, 0, 10),
                        pnpRequest(DevicePnpRequest::QueryRemove),
                        pnpRequest(DevicePnpRequest::Remove, 0, 20)};
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    cleanRemoval(*Result);
    ASSERT_EQ(Result->Requests.size(), 3u);
    ASSERT_TRUE(Result->Requests[0].Pnp);
    ASSERT_TRUE(Result->Requests[2].Pnp);
    EXPECT_EQ(Result->Requests[0].DispatchStatus, 0u);
    EXPECT_EQ(Result->Requests[0].Pnp->BusReceivedAt100ns, 0u);
    EXPECT_EQ(Result->Requests[0].Pnp->BusCompletedAt100ns, 10u);
    EXPECT_EQ(Result->Requests[2].DispatchStatus, 0x103u);
    EXPECT_EQ(Result->Requests[2].Pnp->BusReceivedAt100ns, 10u);
    EXPECT_EQ(Result->Requests[2].Pnp->BusCompletedAt100ns, 30u);
    EXPECT_EQ(apiCount(*Result, "KeWaitForSingleObject"), 1u);
    EXPECT_NE(std::find(Result->Messages.begin(), Result->Messages.end(),
                        "WDM PnP: removed FDO lower=0x00000103\n"),
              Result->Messages.end());
  }
}

TEST(DriverWDMPnp, FailedQueryRollsBackThenCancelAllowsFileIoAndRemoval) {
  for (const auto *Image : pnpImages()) {
    SCOPED_TRACE(Image);
    auto Options = pnpOptions();
    Options.Requests = {pnpRequest(DevicePnpRequest::Start),
                        pnpRequest(DevicePnpRequest::QueryRemove, Failed),
                        pnpRequest(DevicePnpRequest::CancelRemove, 0, 7)};
    appendFileRequests(Options);
    Options.Requests.push_back(pnpRequest(DevicePnpRequest::QueryRemove));
    Options.Requests.push_back(pnpRequest(DevicePnpRequest::Remove));
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    cleanRemoval(*Result);
    ASSERT_EQ(Result->Requests.size(), 9u);
    ASSERT_TRUE(Result->Requests[1].Pnp);
    ASSERT_TRUE(Result->Requests[2].Pnp);
    EXPECT_EQ(Result->Requests[1].IOStatus, Failed);
    EXPECT_EQ(Result->Requests[1].Pnp->BusStatus, Failed);
    EXPECT_EQ(Result->Requests[1].Pnp->StateAfter, DevicePnpState::Started);
    EXPECT_EQ(Result->Requests[2].Pnp->StateAfter, DevicePnpState::Started);
    EXPECT_EQ(Result->Requests[4].Output,
              (std::vector<uint8_t>{'P', 'N', 'P', '!'}));
    EXPECT_EQ(apiCount(*Result, "KeWaitForSingleObject"), 1u);
  }
}

TEST(DriverWDMPnp, FailedStartStillRunsRemoveAndPreservesDistinctBusResult) {
  for (const auto *Image : pnpImages())
    for (char Mode : {'S', 'F'}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Mode);
      auto Options = pnpOptions(Mode);
      Options.Requests = {
          pnpRequest(DevicePnpRequest::Start, Mode == 'S' ? Failed : 0, 10),
          pnpRequest(DevicePnpRequest::Remove)};
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      cleanRemoval(*Result);
      ASSERT_EQ(Result->Requests.size(), 2u);
      ASSERT_TRUE(Result->Requests[0].Pnp);
      EXPECT_EQ(Result->Requests[0].IOStatus, Failed);
      EXPECT_EQ(Result->Requests[0].Pnp->BusStatus, Mode == 'S' ? Failed : 0u);
      EXPECT_EQ(Result->Requests[0].Pnp->StateAfter, DevicePnpState::NotStarted);
    }
}

TEST(DriverWDMPnp, FailedAddDeviceRunsGuestCleanupAndRetiresOnlyProvider) {
  for (const auto *Image : pnpImages()) {
    SCOPED_TRACE(Image);
    auto Result = emulateDriver(Image, pnpOptions('A'));
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    ASSERT_EQ(Result->PnpDevices.size(), 1u);
    EXPECT_EQ(Result->PnpDevices[0].AddDeviceStatus, 0xc000009au);
    EXPECT_FALSE(Result->PnpDevices[0].ProviderPresent);
    EXPECT_FALSE(Result->PnpDevices[0].Attached);
    EXPECT_TRUE(Result->Devices.empty());
    EXPECT_TRUE(Result->Requests.empty());
    EXPECT_TRUE(Result->UnloadCompleted);
    EXPECT_EQ(apiCount(*Result, "IoDetachDevice"), 1u);
    EXPECT_EQ(apiCount(*Result, "IoDeleteDevice"), 1u);
  }
}

TEST(DriverWDMPnp, FailedAddDeviceLeakStopsWithoutFabricatedCleanup) {
  for (const auto *Image : pnpImages()) {
    SCOPED_TRACE(Image);
    auto Result = emulateDriver(Image, pnpOptions('L'));
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
    EXPECT_NE(Result->Diagnostic.find("live guest device"), std::string::npos)
        << Result->Diagnostic;
    ASSERT_EQ(Result->PnpDevices.size(), 1u);
    EXPECT_EQ(Result->PnpDevices[0].AddDeviceStatus, 0xc000009au);
    EXPECT_TRUE(Result->PnpDevices[0].ProviderPresent);
    EXPECT_TRUE(Result->PnpDevices[0].Attached);
    EXPECT_EQ(Result->Devices.size(), 1u);
    EXPECT_FALSE(Result->UnloadCompleted);
    EXPECT_EQ(apiCount(*Result, "IoDeleteDevice"), 0u);
  }
}
#else
TEST(DriverWDMPnp, OptionalGenuineFixtureIsConfigured) {
  GTEST_SKIP() << "NEVERD_WDM_PNP_FIXTURE requires a genuine WDK-linked fixture";
}
#endif
} // namespace
} // namespace neverd::emulation
