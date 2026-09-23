//===- DriverKMDFPnpTests.cpp - Genuine framework PnP lifecycle ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercise the WDK-linked AddDevice, FDO, provider PnP, queue and automatic
/// removal lifetimes through the real guest entry point.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "windows/WindowsKernelLayout.h"

#include "neverd/emulation/DriverSession.h"

#include <algorithm>

namespace neverd::emulation {
namespace {

#ifdef NEVERD_KMDF_PNP_FIXTURE
constexpr uint32_t TransformIoctl = 0x222000;
constexpr llvm::StringLiteral DeviceID = "kmdf-pdo";

std::vector<const char *> pnpImages() {
  std::vector<const char *> Images{NEVERD_KMDF_PNP_FIXTURE};
#ifdef NEVERD_KMDF_PNP_CFG_FIXTURE
  Images.push_back(NEVERD_KMDF_PNP_CFG_FIXTURE);
#endif
  return Images;
}

DriverRequest pnp(DevicePnpRequest Minor, uint64_t Delay = 0) {
  DriverRequest Request;
  Request.Kind = DriverRequestKind::Pnp;
  Request.DeviceID = DeviceID.str();
  Request.Pnp = DriverPnpOperation{
      Minor, DriverBusCompletion{windows::StatusSuccess, Delay}};
  return Request;
}

DriverRequest file(DriverRequestKind Kind) {
  DriverRequest Request;
  Request.Kind = Kind;
  Request.DeviceID = DeviceID.str();
  Request.File = 7;
  if (Kind == DriverRequestKind::DeviceControl) {
    Request.ControlCode = TransformIoctl;
    Request.Input = {0x7a};
    Request.OutputSize = 4;
  }
  return Request;
}

DriverOptions options(char Mode = 'S') {
  DriverOptions Options;
  Options.ServiceName = std::string("NeverDKmdfPnp") + Mode;
  Options.Unload = true;
  DriverPnpDevice Device;
  Device.ID = DeviceID.str();
  Device.InitialDevicePower = DevicePowerState::D0;
  Device.InitialSystemPower = SystemPowerState::Working;
  Options.PnpDevices.push_back(Device);
  if (Mode != 'F') {
    Options.Requests = {pnp(DevicePnpRequest::Start, 11),
                        file(DriverRequestKind::Create),
                        file(DriverRequestKind::DeviceControl),
                        file(DriverRequestKind::Cleanup),
                        file(DriverRequestKind::Close),
                        pnp(DevicePnpRequest::QueryRemove),
                        pnp(DevicePnpRequest::Remove, 7)};
  }
  return Options;
}

std::vector<std::string> pnpMessages(const DriverResult &Result) {
  std::vector<std::string> Messages;
  for (const auto &Message : Result.Messages)
    if (llvm::StringRef(Message).starts_with("KMDF PnP:"))
      Messages.push_back(Message);
  return Messages;
}

size_t callCount(const DriverResult &Result, llvm::StringRef Name) {
  return std::count_if(Result.Calls.begin(), Result.Calls.end(),
                       [Name](const auto &Call) { return Call.Name == Name; });
}

TEST(DriverKMDFPnp, AddStartIoRemoveAndUnloadFollowRealCallbacks) {
  for (const char *Image : pnpImages())
    for (uint64_t Base : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Base);
      auto Input = options();
      Input.LoadAddress = Base;
      auto Result = emulateDriver(Image, Input);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
      EXPECT_EQ(Result->NTStatus, windows::StatusSuccess);
      EXPECT_TRUE(Result->UnloadCompleted);
      ASSERT_EQ(Result->PnpDevices.size(), 1u);
      EXPECT_EQ(Result->PnpDevices[0].AddDeviceStatus, windows::StatusSuccess);
      EXPECT_EQ(Result->PnpDevices[0].PnpState, DevicePnpState::Removed);
      EXPECT_FALSE(Result->PnpDevices[0].ProviderPresent);
      EXPECT_FALSE(Result->PnpDevices[0].Attached);
      EXPECT_TRUE(Result->Devices.empty());
      ASSERT_EQ(Result->Requests.size(), 7u);
      for (const auto &Request : Result->Requests)
        EXPECT_TRUE(Request.Completed);
      EXPECT_EQ(Result->Requests[2].IOStatus, windows::StatusSuccess);
      EXPECT_EQ(Result->Requests[2].Output,
                (std::vector<uint8_t>{'P', 'N', 'P', 0x7a}));
      EXPECT_EQ(callCount(*Result, "WdfFdoInitWdmGetPhysicalDevice"), 1u);
      EXPECT_EQ(callCount(*Result, "WdfDeviceWdmGetPhysicalDevice"), 1u);
      EXPECT_EQ(callCount(*Result, "WdfDeviceWdmGetAttachedDevice"), 1u);
      EXPECT_EQ(callCount(*Result, "WdfWdmDeviceGetWdfDeviceHandle"), 2u);
      EXPECT_EQ(callCount(*Result, "WdfDeviceGetDriver"), 1u);
      EXPECT_EQ(callCount(*Result, "WdfDeviceCreate"), 1u);
      EXPECT_EQ(pnpMessages(*Result),
                (std::vector<std::string>{"KMDF PnP: device ready\n",
                                          "KMDF PnP: device cleanup\n",
                                          "KMDF PnP: device destroy\n",
                                          "KMDF PnP: driver unload\n"}));
    }
}

TEST(DriverKMDFPnp, FailedAddDeletesFrameworkFdoBeforeProviderRetirement) {
  for (const char *Image : pnpImages()) {
    SCOPED_TRACE(Image);
    auto Result = emulateDriver(Image, options('F'));
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    EXPECT_EQ(Result->NTStatus, windows::StatusSuccess);
    EXPECT_TRUE(Result->UnloadCompleted);
    ASSERT_EQ(Result->PnpDevices.size(), 1u);
    EXPECT_EQ(Result->PnpDevices[0].AddDeviceStatus,
              windows::StatusUnsuccessful);
    EXPECT_FALSE(Result->PnpDevices[0].ProviderPresent);
    EXPECT_FALSE(Result->PnpDevices[0].Attached);
    EXPECT_TRUE(Result->Devices.empty());
    EXPECT_TRUE(Result->Requests.empty());
    EXPECT_EQ(pnpMessages(*Result),
              (std::vector<std::string>{
                  "KMDF PnP: failing AddDevice\n", "KMDF PnP: device cleanup\n",
                  "KMDF PnP: device destroy\n", "KMDF PnP: driver unload\n"}));
  }
}

TEST(DriverKMDFPnp, D0CallbacksBracketResourceFreeStartAndRemoval) {
  for (const char *Image : pnpImages())
    for (uint64_t Base : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Base);
      auto Input = options('P');
      Input.LoadAddress = Base;
      auto Result = emulateDriver(Image, Input);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
      EXPECT_TRUE(Result->UnloadCompleted);
      ASSERT_EQ(Result->Requests.size(), 7u);
      for (const auto &Request : Result->Requests)
        EXPECT_TRUE(Request.Completed);
      EXPECT_EQ(callCount(*Result, "WdfDeviceInitSetPnpPowerEventCallbacks"),
                1u);
      EXPECT_EQ(
          pnpMessages(*Result),
          (std::vector<std::string>{
              "KMDF PnP: device ready\n", "KMDF PnP: D0 entry\n",
              "KMDF PnP: D0 exit\n", "KMDF PnP: device cleanup\n",
              "KMDF PnP: device destroy\n", "KMDF PnP: driver unload\n"}));
    }
}

TEST(DriverKMDFPnp, D0CallbacksTrackStopRestartAndFinalRemoval) {
  auto Input = options('P');
  Input.Requests = {
      pnp(DevicePnpRequest::Start),       pnp(DevicePnpRequest::QueryStop),
      pnp(DevicePnpRequest::Stop),        pnp(DevicePnpRequest::Start),
      pnp(DevicePnpRequest::QueryRemove), pnp(DevicePnpRequest::Remove)};
  auto Result = emulateDriver(NEVERD_KMDF_PNP_FIXTURE, Input);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  EXPECT_TRUE(Result->UnloadCompleted);
  EXPECT_EQ(pnpMessages(*Result),
            (std::vector<std::string>{
                "KMDF PnP: device ready\n", "KMDF PnP: D0 entry\n",
                "KMDF PnP: D0 exit\n", "KMDF PnP: D0 entry\n",
                "KMDF PnP: D0 exit\n", "KMDF PnP: device cleanup\n",
                "KMDF PnP: device destroy\n", "KMDF PnP: driver unload\n"}));
}

TEST(DriverKMDFPnp, EmptyResourceListsBracketStartStopAndRestart) {
  for (const char *Image : pnpImages())
    for (uint64_t Base : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Base);
      auto Input = options('H');
      Input.LoadAddress = Base;
      Input.Requests = {
          pnp(DevicePnpRequest::Start, 11),   pnp(DevicePnpRequest::QueryStop),
          pnp(DevicePnpRequest::Stop),        pnp(DevicePnpRequest::Start),
          pnp(DevicePnpRequest::QueryRemove), pnp(DevicePnpRequest::Remove, 7)};
      auto Result = emulateDriver(Image, Input);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
      EXPECT_TRUE(Result->UnloadCompleted);
      ASSERT_EQ(Result->Requests.size(), 6u);
      for (const auto &Request : Result->Requests)
        EXPECT_EQ(Request.IOStatus, windows::StatusSuccess);
      EXPECT_EQ(callCount(*Result, "WdfCmResourceListGetCount"), 6u);
      EXPECT_EQ(callCount(*Result, "WdfCmResourceListGetDescriptor"), 6u);
      EXPECT_EQ(
          pnpMessages(*Result),
          (std::vector<std::string>{
              "KMDF PnP: device ready\n", "KMDF PnP: prepare hardware\n",
              "KMDF PnP: D0 entry\n", "KMDF PnP: D0 exit\n",
              "KMDF PnP: release hardware\n", "KMDF PnP: prepare hardware\n",
              "KMDF PnP: D0 entry\n", "KMDF PnP: D0 exit\n",
              "KMDF PnP: release hardware\n", "KMDF PnP: device cleanup\n",
              "KMDF PnP: device destroy\n", "KMDF PnP: driver unload\n"}));
    }
}

TEST(DriverKMDFPnp, FailedPowerUpReleasesPreparedResources) {
  for (char Mode : {'I', 'J'}) {
    SCOPED_TRACE(Mode);
    auto Input = options(Mode);
    Input.Requests = {pnp(DevicePnpRequest::Start),
                      pnp(DevicePnpRequest::Remove)};
    auto Result = emulateDriver(NEVERD_KMDF_PNP_FIXTURE, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    ASSERT_EQ(Result->Requests.size(), 2u);
    EXPECT_EQ(Result->Requests[0].IOStatus, windows::StatusUnsuccessful);
    EXPECT_EQ(Result->Requests[1].IOStatus, windows::StatusSuccess);
    std::vector<std::string> Expected{"KMDF PnP: device ready\n",
                                      "KMDF PnP: prepare hardware\n"};
    if (Mode == 'J')
      Expected.push_back("KMDF PnP: D0 entry\n");
    Expected.insert(Expected.end(),
                    {"KMDF PnP: release hardware\n",
                     "KMDF PnP: device cleanup\n", "KMDF PnP: device destroy\n",
                     "KMDF PnP: driver unload\n"});
    EXPECT_EQ(pnpMessages(*Result), Expected);
  }
}

TEST(DriverKMDFPnp, FailedD0EntryFailsStartWithoutCallingD0Exit) {
  auto Input = options('Q');
  Input.Requests = {pnp(DevicePnpRequest::Start),
                    pnp(DevicePnpRequest::Remove)};
  auto Result = emulateDriver(NEVERD_KMDF_PNP_FIXTURE, Input);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  ASSERT_EQ(Result->Requests.size(), 2u);
  EXPECT_EQ(Result->Requests[0].IOStatus, windows::StatusUnsuccessful);
  EXPECT_EQ(Result->Requests[1].IOStatus, windows::StatusSuccess);
  EXPECT_EQ(pnpMessages(*Result),
            (std::vector<std::string>{
                "KMDF PnP: device ready\n", "KMDF PnP: D0 entry\n",
                "KMDF PnP: device cleanup\n", "KMDF PnP: device destroy\n",
                "KMDF PnP: driver unload\n"}));
}

TEST(DriverKMDFPnp, UnmodeledPnpCallbackFailsAtRegistration) {
  auto Result = emulateDriver(NEVERD_KMDF_PNP_FIXTURE, options('U'));
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
  EXPECT_NE(Result->Diagnostic.find("unsupported PnP power event callback"),
            std::string::npos);
  EXPECT_TRUE(Result->Requests.empty());
}

TEST(DriverKMDFPnp, PnpDriverCanOmitEvtDriverUnload) {
  auto Result = emulateDriver(NEVERD_KMDF_PNP_FIXTURE, options('N'));
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  EXPECT_TRUE(Result->UnloadCompleted);
  EXPECT_EQ(pnpMessages(*Result),
            (std::vector<std::string>{"KMDF PnP: device ready\n",
                                      "KMDF PnP: device cleanup\n",
                                      "KMDF PnP: device destroy\n"}));
}
#endif

} // namespace
} // namespace neverd::emulation
