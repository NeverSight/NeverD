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
constexpr uint32_t NoSuchDevice = 0xc000000e;
constexpr uint32_t DeletePending = 0xc0000056;
constexpr uint32_t NotReady = 0xc00000a3;

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
                         uint64_t Delay = 0,
                         llvm::StringRef DeviceID = "resource0") {
  DriverRequest Request;
  Request.Kind = DriverRequestKind::Pnp;
  Request.DeviceID = DeviceID.str();
  Request.Pnp = DriverPnpOperation{Minor, DriverBusCompletion{Status, Delay}};
  return Request;
}

DriverRequest fileRequest(DriverRequestKind Kind, uint32_t File = 9,
                          llvm::StringRef DeviceID = {}) {
  DriverRequest Request;
  Request.Kind = Kind;
  Request.File = File;
  Request.DeviceID = DeviceID.str();
  if (Kind == DriverRequestKind::DeviceControl) {
    Request.ControlCode = 0x222000;
    Request.OutputSize = 4;
  } else if (Kind == DriverRequestKind::Read) {
    Request.OutputSize = 4;
  } else if (Kind == DriverRequestKind::Write) {
    Request.Input = {'D', 'A', 'T', 'A'};
  }
  return Request;
}

void appendFileRequests(DriverOptions &Options,
                        llvm::StringRef DeviceID = "resource0",
                        uint32_t File = 9) {
  for (auto Kind : {DriverRequestKind::Create, DriverRequestKind::DeviceControl,
                    DriverRequestKind::Cleanup, DriverRequestKind::Close}) {
    Options.Requests.push_back(fileRequest(
        Kind, File, Kind == DriverRequestKind::Create ? DeviceID : ""));
  }
}

size_t apiCount(const DriverResult &Result, llvm::StringRef Name) {
  return std::count_if(Result.Calls.begin(), Result.Calls.end(),
                       [Name](const auto &Call) { return Call.Name == Name; });
}

void cleanSession(const DriverResult &Result) {
  ASSERT_EQ(Result.Stop, DriverStopReason::Returned) << Result.Diagnostic;
  EXPECT_EQ(Result.NTStatus, 0u);
  EXPECT_TRUE(Result.UnloadCompleted);
  EXPECT_TRUE(Result.Devices.empty());
  for (const auto &Message : Result.Messages)
    EXPECT_EQ(Message.find("WDM PnP: failure"), std::string::npos) << Message;
  for (const auto &Request : Result.Requests)
    EXPECT_TRUE(Request.Completed);
}

void cleanRemoval(const DriverResult &Result, size_t Count = 1,
                   size_t ExtraDevices = 0) {
  cleanSession(Result);
  ASSERT_EQ(Result.PnpDevices.size(), Count);
  for (size_t I = 0; I != Count; ++I) {
    const auto &Device = Result.PnpDevices[I];
    EXPECT_EQ(Device.ID, "resource" + std::to_string(I));
    EXPECT_NE(Device.PDO, 0u);
    EXPECT_EQ(Device.AddDeviceStatus, 0u);
    EXPECT_EQ(Device.PnpState, DevicePnpState::Removed);
    EXPECT_FALSE(Device.ProviderPresent);
    EXPECT_FALSE(Device.Attached);
  }
  EXPECT_EQ(apiCount(Result, "IoAttachDeviceToDeviceStack"), Count);
  EXPECT_EQ(apiCount(Result, "IoDetachDevice"), Count);
  EXPECT_EQ(apiCount(Result, "IoDeleteDevice"), Count + ExtraDevices);
}

void softwareOutput(const DriverRequestResult &Request, bool Control = false) {
  EXPECT_EQ(Request.IOStatus, 0u);
  EXPECT_EQ(Request.Information, 4u);
  EXPECT_EQ(Request.Output, Control ? (std::vector<uint8_t>{'C', 'T', 'L', '!'})
                                   : (std::vector<uint8_t>{'P', 'N', 'P', '!'}));
}

void addSecondPdo(DriverOptions &Options) {
  auto Device = Options.PnpDevices.front();
  Device.ID = "resource1";
  Options.PnpDevices.push_back(std::move(Device));
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
TEST(DriverWDMPnp, StopAndRestartPreserveSoftwareIoAndOpenFiles) {
  for (const auto *Image : pnpImages()) {
    SCOPED_TRACE(Image);
    auto Options = pnpOptions();
    Options.Requests = {
        pnpRequest(DevicePnpRequest::Start),
        fileRequest(DriverRequestKind::Create, 9, "resource0"),
        pnpRequest(DevicePnpRequest::QueryStop, 0, 5),
        fileRequest(DriverRequestKind::DeviceControl),
        pnpRequest(DevicePnpRequest::Stop, 0, 7),
        fileRequest(DriverRequestKind::DeviceControl),
        fileRequest(DriverRequestKind::Create, 10, "resource0"),
        pnpRequest(DevicePnpRequest::Start, 0, 11),
        fileRequest(DriverRequestKind::DeviceControl),
        fileRequest(DriverRequestKind::Cleanup),
        fileRequest(DriverRequestKind::Close),
        pnpRequest(DevicePnpRequest::QueryRemove),
        pnpRequest(DevicePnpRequest::Remove)};
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    cleanRemoval(*Result);
    ASSERT_EQ(Result->Requests.size(), 13u);
    for (size_t I : {2u, 4u, 7u})
      ASSERT_TRUE(Result->Requests[I].Pnp);
    EXPECT_EQ(Result->Requests[2].Pnp->StateAfter, DevicePnpState::StopPending);
    EXPECT_EQ(Result->Requests[4].Pnp->StateAfter, DevicePnpState::Stopped);
    EXPECT_EQ(Result->Requests[4].DispatchStatus, 0x103u);
    EXPECT_EQ(Result->Requests[4].Pnp->BusCompletedAt100ns, 12u);
    EXPECT_EQ(Result->Requests[7].Pnp->StateBefore, DevicePnpState::Stopped);
    EXPECT_EQ(Result->Requests[7].Pnp->StateAfter, DevicePnpState::Started);
    EXPECT_EQ(Result->Requests[7].Pnp->BusCompletedAt100ns, 23u);
    for (size_t I : {3u, 5u, 8u})
      softwareOutput(Result->Requests[I]);
    EXPECT_EQ(Result->Requests[6].IOStatus, NotReady);
    EXPECT_EQ(Result->Requests[6].DispatchStatus, NotReady);
    EXPECT_NE(Result->Requests[6].IRP, 0u);
    EXPECT_EQ(apiCount(*Result, "KeWaitForSingleObject"), 2u);
  }
}

TEST(DriverWDMPnp, QueryStopFailureAndCancellationRestoreGuestAndModelState) {
  for (const auto *Image : pnpImages())
    for (char Mode : {'S', 'T'}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Mode);
      auto Options = pnpOptions(Mode);
      Options.Requests = {
          pnpRequest(DevicePnpRequest::Start),
          pnpRequest(DevicePnpRequest::QueryStop, Mode == 'S' ? Failed : 0, 10),
          pnpRequest(DevicePnpRequest::CancelStop, 0, 7),
          pnpRequest(DevicePnpRequest::QueryStop, 0, 3),
          pnpRequest(DevicePnpRequest::CancelStop, 0, 5)};
      appendFileRequests(Options);
      Options.Requests.push_back(pnpRequest(DevicePnpRequest::QueryRemove));
      Options.Requests.push_back(pnpRequest(DevicePnpRequest::Remove));
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      cleanRemoval(*Result);
      ASSERT_EQ(Result->Requests.size(), 11u);
      for (size_t I : {1u, 2u, 3u, 4u})
        ASSERT_TRUE(Result->Requests[I].Pnp);
      EXPECT_EQ(Result->Requests[1].IOStatus, Failed);
      EXPECT_EQ(Result->Requests[1].Pnp->StateAfter, DevicePnpState::Started);
      if (Mode == 'S') {
        EXPECT_EQ(Result->Requests[1].Pnp->BusStatus, Failed);
        EXPECT_EQ(Result->Requests[1].Pnp->BusCompletedAt100ns, 10u);
      } else {
        EXPECT_FALSE(Result->Requests[1].Pnp->BusStatus);
        EXPECT_FALSE(Result->Requests[1].Pnp->BusReceivedAt100ns);
        EXPECT_FALSE(Result->Requests[1].Pnp->BusCompletedAt100ns);
      }
      EXPECT_EQ(Result->Requests[2].Pnp->StateAfter, DevicePnpState::Started);
      EXPECT_EQ(Result->Requests[3].Pnp->StateAfter, DevicePnpState::StopPending);
      EXPECT_EQ(Result->Requests[4].Pnp->StateAfter, DevicePnpState::Started);
      softwareOutput(Result->Requests[6]);
    }
}

TEST(DriverWDMPnp, RemovePendingDeliversSoftwareIoAndGuestRejectsNewCreate) {
  for (const auto *Image : pnpImages()) {
    SCOPED_TRACE(Image);
    auto Options = pnpOptions();
    Options.Requests = {
        pnpRequest(DevicePnpRequest::Start),
        fileRequest(DriverRequestKind::Create, 9, "resource0"),
        pnpRequest(DevicePnpRequest::QueryRemove),
        fileRequest(DriverRequestKind::DeviceControl),
        fileRequest(DriverRequestKind::Create, 10, "resource0"),
        pnpRequest(DevicePnpRequest::CancelRemove),
        fileRequest(DriverRequestKind::Create, 10, "resource0"),
        fileRequest(DriverRequestKind::Cleanup, 10),
        fileRequest(DriverRequestKind::Close, 10),
        fileRequest(DriverRequestKind::Cleanup),
        fileRequest(DriverRequestKind::Close),
        pnpRequest(DevicePnpRequest::QueryRemove),
        pnpRequest(DevicePnpRequest::Remove)};
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    cleanRemoval(*Result);
    ASSERT_EQ(Result->Requests.size(), 13u);
    softwareOutput(Result->Requests[3]);
    EXPECT_EQ(Result->Requests[4].IOStatus, DeletePending);
    EXPECT_EQ(Result->Requests[4].DispatchStatus, DeletePending);
    EXPECT_NE(Result->Requests[4].IRP, 0u);
    EXPECT_EQ(Result->Requests[6].IOStatus, 0u);
    EXPECT_NE(Result->Requests[4].IRP, Result->Requests[6].IRP);
    EXPECT_EQ(Result->Requests[9].IOStatus, 0u);
    EXPECT_EQ(Result->Requests[10].IOStatus, 0u);
  }
}

TEST(DriverWDMPnp, SurpriseRemovalKeepsFdoForGuestFailuresAndFileTeardown) {
  for (const auto *Image : pnpImages()) {
    SCOPED_TRACE(Image);
    auto Options = pnpOptions();
    Options.Requests = {
        pnpRequest(DevicePnpRequest::Start),
        fileRequest(DriverRequestKind::Create, 9, "resource0"),
        pnpRequest(DevicePnpRequest::SurpriseRemoval, 0, 5),
        fileRequest(DriverRequestKind::DeviceControl),
        fileRequest(DriverRequestKind::Create, 10, "resource0"),
        fileRequest(DriverRequestKind::Read),
        fileRequest(DriverRequestKind::Write),
        fileRequest(DriverRequestKind::Cleanup),
        fileRequest(DriverRequestKind::Close),
        pnpRequest(DevicePnpRequest::Remove, 0, 7)};
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    cleanRemoval(*Result);
    ASSERT_EQ(Result->Requests.size(), 10u);
    ASSERT_TRUE(Result->Requests[2].Pnp);
    ASSERT_TRUE(Result->Requests[9].Pnp);
    EXPECT_EQ(Result->Requests[2].Pnp->StateAfter,
              DevicePnpState::SurpriseRemoved);
    EXPECT_EQ(Result->Requests[2].Pnp->BusCompletedAt100ns, 5u);
    EXPECT_EQ(Result->Requests[9].Pnp->StateBefore,
              DevicePnpState::SurpriseRemoved);
    EXPECT_EQ(Result->Requests[9].Pnp->BusCompletedAt100ns, 12u);
    for (size_t I : {3u, 4u, 5u, 6u}) {
      EXPECT_NE(Result->Requests[I].IRP, 0u);
      EXPECT_EQ(Result->Requests[I].DispatchStatus, NoSuchDevice);
      EXPECT_EQ(Result->Requests[I].IOStatus, NoSuchDevice);
      EXPECT_EQ(Result->Requests[I].Information, 0u);
    }
    EXPECT_EQ(Result->Requests[7].IOStatus, 0u);
    EXPECT_EQ(Result->Requests[8].IOStatus, 0u);
    EXPECT_NE(std::find(Result->Messages.begin(), Result->Messages.end(),
                        "WDM PnP: surprise kept FDO unit=1\n"),
              Result->Messages.end());
  }
}

TEST(DriverWDMPnp, MultiplePdosKeepIndependentFdoFileAndLifecycleIdentity) {
  for (const auto *Image : pnpImages()) {
    SCOPED_TRACE(Image);
    auto Options = pnpOptions();
    addSecondPdo(Options);
    auto Unit0 = fileRequest(DriverRequestKind::DeviceControl, 9);
    Unit0.ControlCode = 0x222004;
    auto Unit1 = fileRequest(DriverRequestKind::DeviceControl, 10);
    Unit1.ControlCode = 0x222004;
    Options.Requests = {
        pnpRequest(DevicePnpRequest::Start),
        pnpRequest(DevicePnpRequest::Start, 0, 3, "resource1"),
        fileRequest(DriverRequestKind::Create, 9, "resource0"),
        fileRequest(DriverRequestKind::Create, 10, "resource1"),
        Unit0, Unit1,
        pnpRequest(DevicePnpRequest::QueryStop),
        pnpRequest(DevicePnpRequest::Stop),
        fileRequest(DriverRequestKind::DeviceControl, 9),
        pnpRequest(DevicePnpRequest::SurpriseRemoval, 0, 5, "resource1"),
        fileRequest(DriverRequestKind::DeviceControl, 10),
        fileRequest(DriverRequestKind::DeviceControl, 9),
        fileRequest(DriverRequestKind::Cleanup, 10),
        fileRequest(DriverRequestKind::Close, 10),
        pnpRequest(DevicePnpRequest::Remove, 0, 7, "resource1"),
        pnpRequest(DevicePnpRequest::Start),
        Unit0,
        fileRequest(DriverRequestKind::Cleanup, 9),
        fileRequest(DriverRequestKind::Close, 9),
        pnpRequest(DevicePnpRequest::QueryRemove),
        pnpRequest(DevicePnpRequest::Remove)};
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    cleanRemoval(*Result, 2);
    ASSERT_EQ(Result->Requests.size(), 21u);
    EXPECT_NE(Result->PnpDevices[0].PDO, Result->PnpDevices[1].PDO);
    EXPECT_EQ(Result->Requests[4].Output, (std::vector<uint8_t>{1, 0, 0, 0}));
    EXPECT_EQ(Result->Requests[5].Output, (std::vector<uint8_t>{2, 0, 0, 0}));
    EXPECT_EQ(Result->Requests[16].Output, Result->Requests[4].Output);
    softwareOutput(Result->Requests[8]);
    softwareOutput(Result->Requests[11]);
    EXPECT_EQ(Result->Requests[10].IOStatus, NoSuchDevice);
    EXPECT_EQ(Result->Requests[10].DeviceID, "resource1");
    EXPECT_EQ(Result->Requests[11].DeviceID, "resource0");
    ASSERT_TRUE(Result->Requests[15].Pnp);
    EXPECT_EQ(Result->Requests[15].Pnp->StateBefore, DevicePnpState::Stopped);
  }
}

TEST(DriverWDMPnp, FailedFirstAddDeviceLeavesSecondPdoHealthy) {
  for (const auto *Image : pnpImages()) {
    SCOPED_TRACE(Image);
    auto Options = pnpOptions('M');
    addSecondPdo(Options);
    Options.Requests.push_back(
        pnpRequest(DevicePnpRequest::Start, 0, 5, "resource1"));
    appendFileRequests(Options, "resource1", 10);
    Options.Requests.push_back(
        pnpRequest(DevicePnpRequest::QueryRemove, 0, 0, "resource1"));
    Options.Requests.push_back(
        pnpRequest(DevicePnpRequest::Remove, 0, 7, "resource1"));
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    cleanSession(*Result);
    ASSERT_EQ(Result->PnpDevices.size(), 2u);
    ASSERT_EQ(Result->Requests.size(), 7u);
    EXPECT_EQ(Result->PnpDevices[0].AddDeviceStatus, 0xc000009au);
    EXPECT_EQ(Result->PnpDevices[0].PnpState, DevicePnpState::NotStarted);
    EXPECT_FALSE(Result->PnpDevices[0].Attached);
    EXPECT_FALSE(Result->PnpDevices[0].ProviderPresent);
    EXPECT_EQ(Result->PnpDevices[1].AddDeviceStatus, 0u);
    EXPECT_EQ(Result->PnpDevices[1].PnpState, DevicePnpState::Removed);
    EXPECT_FALSE(Result->PnpDevices[1].ProviderPresent);
    softwareOutput(Result->Requests[2]);
    for (const auto &Request : Result->Requests)
      EXPECT_EQ(Request.DeviceID, "resource1");
    EXPECT_EQ(apiCount(*Result, "IoAttachDeviceToDeviceStack"), 2u);
    EXPECT_EQ(apiCount(*Result, "IoDetachDevice"), 2u);
    EXPECT_EQ(apiCount(*Result, "IoDeleteDevice"), 2u);
  }
}

TEST(DriverWDMPnp, IndependentControlDeviceOutlivesStoppedAndRemovedPdo) {
  for (const auto *Image : pnpImages()) {
    SCOPED_TRACE(Image);
    auto Options = pnpOptions('C');
    auto OpenControl = fileRequest(DriverRequestKind::Create, 90);
    OpenControl.Device = "\\Device\\NeverDPnpControl";
    Options.Requests = {
        OpenControl,
        fileRequest(DriverRequestKind::DeviceControl, 90),
        pnpRequest(DevicePnpRequest::Start),
        fileRequest(DriverRequestKind::Create, 9, "resource0"),
        pnpRequest(DevicePnpRequest::QueryStop),
        pnpRequest(DevicePnpRequest::Stop),
        fileRequest(DriverRequestKind::DeviceControl, 9),
        fileRequest(DriverRequestKind::DeviceControl, 90),
        pnpRequest(DevicePnpRequest::SurpriseRemoval),
        fileRequest(DriverRequestKind::DeviceControl, 9),
        fileRequest(DriverRequestKind::DeviceControl, 90),
        fileRequest(DriverRequestKind::Cleanup, 9),
        fileRequest(DriverRequestKind::Close, 9),
        pnpRequest(DevicePnpRequest::Remove),
        fileRequest(DriverRequestKind::DeviceControl, 90),
        fileRequest(DriverRequestKind::Cleanup, 90),
        fileRequest(DriverRequestKind::Close, 90)};
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    cleanRemoval(*Result, 1, 1);
    ASSERT_EQ(Result->Requests.size(), 17u);
    for (size_t I : {1u, 7u, 10u, 14u}) {
      softwareOutput(Result->Requests[I], true);
      EXPECT_TRUE(Result->Requests[I].DeviceID.empty());
      EXPECT_EQ(Result->Requests[I].Device, "\\Device\\NeverDPnpControl");
    }
    softwareOutput(Result->Requests[6]);
    EXPECT_EQ(Result->Requests[9].IOStatus, NoSuchDevice);
    EXPECT_NE(std::find(Result->Messages.begin(), Result->Messages.end(),
                        "WDM PnP: deleted independent control device\n"),
              Result->Messages.end());
  }
}

#else
TEST(DriverWDMPnp, OptionalGenuineFixtureIsConfigured) {
  GTEST_SKIP() << "NEVERD_WDM_PNP_FIXTURE requires a genuine WDK-linked fixture";
}
#endif
} // namespace
} // namespace neverd::emulation
