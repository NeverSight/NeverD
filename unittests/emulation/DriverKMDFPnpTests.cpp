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
#include "windows/KernelFramework.h"
#include "windows/WindowsKernelLayout.h"

#include "neverd/emulation/DriverSession.h"

#include <algorithm>
#include <array>

namespace neverd::emulation {
namespace {

#ifdef NEVERD_KMDF_PNP_FIXTURE
constexpr uint32_t TransformIoctl = 0x222000;
constexpr llvm::StringLiteral DeviceID = "kmdf-pdo";
constexpr uint64_t RawRegisterBase = 0x200000000ULL;
constexpr uint64_t TranslatedRegisterBase = 0x300000000ULL;
constexpr uint32_t RegisterRegionSize = 0x1000;
constexpr uint32_t InitialRegisterValue = 0x12345678;
constexpr uint64_t DelayedFileCompletion100ns = 17;
constexpr uint64_t FileSendTimeout100ns = 5;
constexpr uint64_t LowerBeforeTimeout100ns = 3;
constexpr uint64_t FileContextCleanupDelay100ns = 3;

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

DriverRequest file(DriverRequestKind Kind, uint64_t FileID = 7) {
  DriverRequest Request;
  Request.Kind = Kind;
  Request.DeviceID = DeviceID.str();
  Request.File = FileID;
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

DriverOptions resourceOptions(char Mode) {
  auto Input = options(Mode);
  auto &Device = Input.PnpDevices.front();
  Device.Bus = DriverBusKind::RegisterBank;
  DriverMemoryResource Bank;
  Bank.ID = "registers";
  Bank.RawStart = RawRegisterBase;
  Bank.TranslatedStart = TranslatedRegisterBase;
  Bank.Length = RegisterRegionSize;
  Bank.Registers = {
      {0, 4, DriverRegisterAccess::ReadOnly, InitialRegisterValue}};
  Device.Resources.push_back(Bank);
  return Input;
}

DriverOptions forwardedFileOptions(char Mode) {
  auto Input = options(Mode);
  Input.Requests.erase(
      std::remove_if(Input.Requests.begin(), Input.Requests.end(),
                     [](const DriverRequest &Request) {
                       return Request.Kind == DriverRequestKind::DeviceControl;
                     }),
      Input.Requests.end());
  for (auto &Request : Input.Requests)
    if (Mode != 'n' && (Request.Kind == DriverRequestKind::Create ||
                        Request.Kind == DriverRequestKind::Cleanup ||
                        Request.Kind == DriverRequestKind::Close))
      Request.FileBusCompletion = DriverBusCompletion{windows::StatusSuccess};
  return Input;
}

DriverOptions pendingStopOptions(char Mode) {
  auto Input = options(Mode);
  auto IO = file(DriverRequestKind::DeviceControl);
  IO.DeferCallbackDrain = true;
  Input.Requests = {
      pnp(DevicePnpRequest::Start),       file(DriverRequestKind::Create),
      pnp(DevicePnpRequest::QueryStop),   IO,
      pnp(DevicePnpRequest::Stop),        pnp(DevicePnpRequest::Start),
      file(DriverRequestKind::Cleanup),   file(DriverRequestKind::Close),
      pnp(DevicePnpRequest::QueryRemove), pnp(DevicePnpRequest::Remove)};
  return Input;
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

TEST(DriverKMDFPnp, FileLifecycleRespectsFilterForwardingChoice) {
  for (const char *Image : pnpImages())
    for (uint64_t Base : {0x180000000ULL, 0x190000000ULL}) {
      for (char Mode : {'O', 'o', 'n', 'p', 's', 'a'}) {
        SCOPED_TRACE(Image);
        SCOPED_TRACE(Base);
        SCOPED_TRACE(Mode);
        auto Input = forwardedFileOptions(Mode);
        Input.LoadAddress = Base;
        auto Result = emulateDriver(Image, Input);
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        ASSERT_EQ(Result->Stop, DriverStopReason::Returned)
            << Result->Diagnostic;
        ASSERT_EQ(Result->Requests.size(), Input.Requests.size());
        for (const auto &Request : Result->Requests) {
          EXPECT_TRUE(Request.Completed);
          EXPECT_EQ(Request.IOStatus, windows::StatusSuccess);
        }
        EXPECT_TRUE(Result->UnloadCompleted);
        EXPECT_EQ(callCount(*Result, "WdfFdoInitSetFilter"),
                  Mode == 'o' ? 0u : 1u);
        EXPECT_EQ(callCount(*Result, "WdfDeviceInitSetFileObjectConfig"), 1u);
        EXPECT_EQ(callCount(*Result, "WdfIoQueueCreate"), 0u);
        EXPECT_EQ(callCount(*Result, "WdfDeviceGetIoTarget"),
                  Mode == 'p' || Mode == 's' || Mode == 'a' ? 2u : 0u);
        EXPECT_EQ(callCount(*Result, "WdfRequestSend"),
                  Mode == 'p' || Mode == 's' || Mode == 'a' ? 1u : 0u);
        EXPECT_EQ(callCount(*Result, "WdfRequestGetStatus"),
                  Mode == 's' || Mode == 'a' ? 1u : 0u);
        EXPECT_EQ(callCount(*Result, "WdfRequestFormatRequestUsingCurrentType"),
                  Mode == 'a' ? 1u : 0u);
        EXPECT_EQ(callCount(*Result, "WdfRequestSetCompletionRoutine"),
                  Mode == 'a' ? 1u : 0u);
        EXPECT_EQ(callCount(*Result, "WdfRequestGetCompletionParams"),
                  Mode == 'a' ? 1u : 0u);
        const auto Messages = pnpMessages(*Result);
        EXPECT_EQ(std::count(Messages.begin(), Messages.end(),
                             "KMDF PnP: file order invalid\n"),
                  0);
        auto Cleanup = std::find(Messages.begin(), Messages.end(),
                                 "KMDF PnP: file cleanup\n");
        auto Close = std::find(Messages.begin(), Messages.end(),
                               "KMDF PnP: file close\n");
        ASSERT_NE(Cleanup, Messages.end());
        ASSERT_NE(Close, Messages.end());
        EXPECT_LT(Cleanup, Close);
        auto Create = std::find(Messages.begin(), Messages.end(),
                                "KMDF PnP: file create forwarded\n");
        if (Mode == 'p' || Mode == 's' || Mode == 'a') {
          ASSERT_NE(Create, Messages.end());
          EXPECT_LT(Create, Cleanup);
        } else {
          EXPECT_EQ(Create, Messages.end());
        }
      }
    }
}

TEST(DriverKMDFPnp, OwnedCreateWaitsForDelayedLowerCompletion) {
  for (char Mode : {'a', 's'})
    for (const char *Image : pnpImages())
      for (uint64_t Base : {0x180000000ULL, 0x190000000ULL})
        for (uint32_t Status :
             {windows::StatusSuccess, windows::StatusUnsuccessful}) {
          SCOPED_TRACE(Image);
          SCOPED_TRACE(Base);
          SCOPED_TRACE(Status);
          auto Input = forwardedFileOptions(Mode);
          Input.LoadAddress = Base;
          Input.Requests[1].FileBusCompletion =
              DriverBusCompletion{Status, DelayedFileCompletion100ns};
          if (Status != windows::StatusSuccess)
            Input.Requests.erase(
                std::remove_if(Input.Requests.begin(), Input.Requests.end(),
                               [](const DriverRequest &Request) {
                                 return Request.Kind ==
                                            DriverRequestKind::Cleanup ||
                                        Request.Kind ==
                                            DriverRequestKind::Close;
                               }),
                Input.Requests.end());
          auto Result = emulateDriver(Image, Input);
          ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
          ASSERT_EQ(Result->Stop, DriverStopReason::Returned)
              << Result->Diagnostic;
          EXPECT_EQ(Result->Requests[1].DispatchStatus, windows::StatusPending);
          EXPECT_EQ(Result->Requests[1].IOStatus, Status);
          EXPECT_TRUE(Result->Requests[1].Completed);
          EXPECT_EQ(callCount(*Result, "WdfRequestSend"), 1u);
          EXPECT_EQ(callCount(*Result, "WdfRequestGetCompletionParams"),
                    Mode == 'a' ? 1u : 0u);
          EXPECT_EQ(callCount(*Result, "WdfRequestGetStatus"), 1u);
          ASSERT_TRUE(Result->Requests.front().Pnp);
          ASSERT_TRUE(Result->Requests.front().Pnp->BusCompletedAt100ns);
          const auto &QueryRemove =
              Result->Requests[Status == windows::StatusSuccess ? 4u : 2u];
          ASSERT_TRUE(QueryRemove.Pnp);
          ASSERT_TRUE(QueryRemove.Pnp->BusReceivedAt100ns);
          EXPECT_GE(*QueryRemove.Pnp->BusReceivedAt100ns,
                    *Result->Requests.front().Pnp->BusCompletedAt100ns +
                        DelayedFileCompletion100ns);
          EXPECT_TRUE(Result->UnloadCompleted);
        }
}

TEST(DriverKMDFPnp, OwnedCreateTimeoutCompetesWithLowerCompletion) {
  for (char Mode : {'t', 'q'})
    for (const char *Image : pnpImages())
      for (uint64_t Base : {0x180000000ULL, 0x190000000ULL})
        for (uint64_t Delay : {LowerBeforeTimeout100ns, FileSendTimeout100ns,
                               DelayedFileCompletion100ns}) {
          SCOPED_TRACE(Image);
          SCOPED_TRACE(Base);
          SCOPED_TRACE(Delay);
          const bool TimedOut = Delay > FileSendTimeout100ns;
          auto Input = forwardedFileOptions(Mode);
          Input.LoadAddress = Base;
          Input.Requests[1].FileBusCompletion =
              DriverBusCompletion{windows::StatusSuccess, Delay};
          if (TimedOut)
            Input.Requests.erase(
                std::remove_if(Input.Requests.begin(), Input.Requests.end(),
                               [](const DriverRequest &Request) {
                                 return Request.Kind ==
                                            DriverRequestKind::Cleanup ||
                                        Request.Kind ==
                                            DriverRequestKind::Close;
                               }),
                Input.Requests.end());
          auto Result = emulateDriver(Image, Input);
          ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
          ASSERT_EQ(Result->Stop, DriverStopReason::Returned)
              << Result->Diagnostic;
          EXPECT_EQ(Result->Requests[1].IOStatus, TimedOut
                                                      ? windows::StatusIOTimeout
                                                      : windows::StatusSuccess);
          EXPECT_TRUE(Result->Requests[1].Completed);
          EXPECT_EQ(callCount(*Result, "WdfRequestSend"), 1u);
          EXPECT_EQ(callCount(*Result, "WdfRequestGetStatus"), 1u);
          EXPECT_EQ(callCount(*Result, "WdfRequestGetCompletionParams"),
                    Mode == 't' ? 1u : 0u);
          ASSERT_TRUE(Result->Requests.front().Pnp);
          ASSERT_TRUE(Result->Requests.front().Pnp->BusCompletedAt100ns);
          const auto QueryRemove = std::find_if(
              Result->Requests.begin(), Result->Requests.end(),
              [](const DriverRequestResult &Request) {
                return Request.Pnp &&
                       Request.Pnp->Minor == DevicePnpRequest::QueryRemove;
              });
          ASSERT_NE(QueryRemove, Result->Requests.end());
          ASSERT_TRUE(QueryRemove->Pnp->BusReceivedAt100ns);
          EXPECT_GE(*QueryRemove->Pnp->BusReceivedAt100ns,
                    *Result->Requests.front().Pnp->BusCompletedAt100ns +
                        std::min(Delay, FileSendTimeout100ns));
          EXPECT_TRUE(Result->UnloadCompleted);
        }
}

TEST(DriverKMDFPnp, SendAndForgetCreateAllowsDelayedLowerCompletion) {
  for (const char *Image : pnpImages())
    for (uint64_t Base : {0x180000000ULL, 0x190000000ULL})
      for (uint32_t Status :
           {windows::StatusSuccess, windows::StatusUnsuccessful}) {
        SCOPED_TRACE(Image);
        SCOPED_TRACE(Base);
        SCOPED_TRACE(Status);
        auto Input = forwardedFileOptions('p');
        Input.LoadAddress = Base;
        Input.Requests[1].FileBusCompletion =
            DriverBusCompletion{Status, DelayedFileCompletion100ns};
        if (Status != windows::StatusSuccess)
          Input.Requests.erase(
              std::remove_if(Input.Requests.begin(), Input.Requests.end(),
                             [](const DriverRequest &Request) {
                               return Request.Kind ==
                                          DriverRequestKind::Cleanup ||
                                      Request.Kind == DriverRequestKind::Close;
                             }),
              Input.Requests.end());
        auto Result = emulateDriver(Image, Input);
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        ASSERT_EQ(Result->Stop, DriverStopReason::Returned)
            << Result->Diagnostic;
        EXPECT_EQ(Result->Requests[1].DispatchStatus, windows::StatusPending);
        EXPECT_EQ(Result->Requests[1].IOStatus, Status);
        EXPECT_TRUE(Result->Requests[1].Completed);
        EXPECT_EQ(callCount(*Result, "WdfRequestSend"), 1u);
        EXPECT_EQ(callCount(*Result, "WdfRequestGetStatus"), 0u);
        EXPECT_EQ(callCount(*Result, "WdfRequestGetCompletionParams"), 0u);
        ASSERT_TRUE(Result->Requests.front().Pnp);
        ASSERT_TRUE(Result->Requests.front().Pnp->BusCompletedAt100ns);
        const auto QueryRemove = std::find_if(
            Result->Requests.begin(), Result->Requests.end(),
            [](const DriverRequestResult &Request) {
              return Request.Pnp &&
                     Request.Pnp->Minor == DevicePnpRequest::QueryRemove;
            });
        ASSERT_NE(QueryRemove, Result->Requests.end());
        ASSERT_TRUE(QueryRemove->Pnp->BusReceivedAt100ns);
        EXPECT_GE(*QueryRemove->Pnp->BusReceivedAt100ns,
                  *Result->Requests.front().Pnp->BusCompletedAt100ns +
                      DelayedFileCompletion100ns);
        EXPECT_TRUE(Result->UnloadCompleted);
      }
}

TEST(DriverKMDFPnp,
     AbsoluteSendTimeoutUsesCurrentTimeAndPreservesImmediateWins) {
  for (char Mode : {'c', 'd'})
    for (const char *Image : pnpImages())
      for (uint64_t StartTime : {uint64_t(0), uint64_t(11)})
        for (uint64_t Delay :
             {uint64_t(0), LowerBeforeTimeout100ns, FileSendTimeout100ns,
              DelayedFileCompletion100ns}) {
          SCOPED_TRACE(Mode);
          SCOPED_TRACE(Image);
          SCOPED_TRACE(StartTime);
          SCOPED_TRACE(Delay);
          const uint64_t Interval = StartTime < FileSendTimeout100ns
                                        ? FileSendTimeout100ns - StartTime
                                        : 0;
          const bool TimedOut = Delay > Interval;
          auto Input = forwardedFileOptions(Mode);
          Input.Requests.front().Pnp->BusCompletion.Delay100ns = StartTime;
          Input.Requests[1].FileBusCompletion =
              DriverBusCompletion{windows::StatusSuccess, Delay};
          if (TimedOut)
            std::erase_if(Input.Requests, [](const DriverRequest &Request) {
              return Request.Kind == DriverRequestKind::Cleanup ||
                     Request.Kind == DriverRequestKind::Close;
            });
          auto Result = emulateDriver(Image, Input);
          ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
          ASSERT_EQ(Result->Stop, DriverStopReason::Returned)
              << Result->Diagnostic;
          EXPECT_EQ(Result->Requests[1].IOStatus, TimedOut
                                                      ? windows::StatusIOTimeout
                                                      : windows::StatusSuccess);
          EXPECT_TRUE(Result->Requests[1].Completed);
          EXPECT_EQ(callCount(*Result, "WdfRequestGetCompletionParams"),
                    Mode == 'd' ? 1u : 0u);
          const auto QueryRemove = std::find_if(
              Result->Requests.begin(), Result->Requests.end(),
              [](const DriverRequestResult &Request) {
                return Request.Pnp &&
                       Request.Pnp->Minor == DevicePnpRequest::QueryRemove;
              });
          ASSERT_NE(QueryRemove, Result->Requests.end());
          ASSERT_TRUE(QueryRemove->Pnp->BusReceivedAt100ns);
          EXPECT_EQ(*QueryRemove->Pnp->BusReceivedAt100ns,
                    StartTime + std::min(Delay, Interval));
          EXPECT_TRUE(Result->UnloadCompleted);
        }
}

TEST(DriverKMDFPnp, AutomaticFileForwardingWaitsForLowerCompletion) {
  for (char Mode : {'O', 'o'})
    for (const char *Image : pnpImages())
      for (uint64_t Base : {0x180000000ULL, 0x190000000ULL})
        for (std::optional<DriverRequestKind> Failure :
             {std::optional<DriverRequestKind>{},
              std::optional{DriverRequestKind::Create},
              std::optional{DriverRequestKind::Cleanup},
              std::optional{DriverRequestKind::Close}}) {
          SCOPED_TRACE(Mode);
          SCOPED_TRACE(Image);
          SCOPED_TRACE(Base);
          auto Input = forwardedFileOptions(Mode);
          Input.LoadAddress = Base;
          uint64_t LowerDelay = 0;
          if (Failure == DriverRequestKind::Create)
            std::erase_if(Input.Requests, [](const DriverRequest &Request) {
              return Request.Kind == DriverRequestKind::Cleanup ||
                     Request.Kind == DriverRequestKind::Close;
            });
          for (auto &Request : Input.Requests)
            if (Request.FileBusCompletion) {
              Request.FileBusCompletion = DriverBusCompletion{
                  Failure == Request.Kind ? windows::StatusUnsuccessful
                                          : windows::StatusSuccess,
                  DelayedFileCompletion100ns};
              LowerDelay += DelayedFileCompletion100ns;
            }
          auto Result = emulateDriver(Image, Input);
          ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
          ASSERT_EQ(Result->Stop, DriverStopReason::Returned)
              << Result->Diagnostic;
          ASSERT_EQ(Result->Requests.size(), Input.Requests.size());
          for (size_t I = 0; I < Input.Requests.size(); ++I)
            if (Input.Requests[I].FileBusCompletion) {
              EXPECT_TRUE(Result->Requests[I].Completed);
              EXPECT_EQ(Result->Requests[I].DispatchStatus,
                        windows::StatusPending);
              EXPECT_EQ(Result->Requests[I].IOStatus,
                        Input.Requests[I].FileBusCompletion->Status);
            }
          const auto Messages = pnpMessages(*Result);
          for (llvm::StringRef Invalid : {"KMDF PnP: file storage invalid\n",
                                          "KMDF PnP: file order invalid\n"})
            EXPECT_EQ(std::count(Messages.begin(), Messages.end(), Invalid), 0);
          auto Cleanup = std::find(Messages.begin(), Messages.end(),
                                   "KMDF PnP: file object cleanup\n");
          auto Destroy = std::find(Messages.begin(), Messages.end(),
                                   "KMDF PnP: file object destroy\n");
          ASSERT_NE(Cleanup, Messages.end());
          ASSERT_NE(Destroy, Messages.end());
          EXPECT_LT(Cleanup, Destroy);
          EXPECT_EQ(std::count(Messages.begin(), Messages.end(),
                               "KMDF PnP: file cleanup\n"),
                    Failure == DriverRequestKind::Create ? 0 : 1);
          const auto QueryRemove = std::find_if(
              Result->Requests.begin(), Result->Requests.end(),
              [](const DriverRequestResult &Request) {
                return Request.Pnp &&
                       Request.Pnp->Minor == DevicePnpRequest::QueryRemove;
              });
          ASSERT_NE(QueryRemove, Result->Requests.end());
          ASSERT_TRUE(QueryRemove->Pnp->BusReceivedAt100ns);
          ASSERT_TRUE(Result->Requests.front().Pnp->BusCompletedAt100ns);
          EXPECT_GE(*QueryRemove->Pnp->BusReceivedAt100ns,
                    *Result->Requests.front().Pnp->BusCompletedAt100ns +
                        LowerDelay + FileContextCleanupDelay100ns);
          EXPECT_TRUE(Result->UnloadCompleted);
        }
}

TEST(DriverKMDFPnp, CompletionRoutineCanOverrideLowerCreateStatus) {
  for (const char *Image : pnpImages())
    for (uint64_t Base : {0x180000000ULL, 0x190000000ULL})
      for (uint64_t Delay : {uint64_t{0}, DelayedFileCompletion100ns}) {
        SCOPED_TRACE(Image);
        SCOPED_TRACE(Base);
        SCOPED_TRACE(Delay);
        auto Input = forwardedFileOptions('b');
        Input.LoadAddress = Base;
        Input.Requests.erase(
            std::remove_if(Input.Requests.begin(), Input.Requests.end(),
                           [](const DriverRequest &Request) {
                             return Request.Kind ==
                                        DriverRequestKind::Cleanup ||
                                    Request.Kind == DriverRequestKind::Close;
                           }),
            Input.Requests.end());
        Input.Requests[1].FileBusCompletion =
            DriverBusCompletion{windows::StatusSuccess, Delay};
        auto Result = emulateDriver(Image, Input);
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        ASSERT_EQ(Result->Stop, DriverStopReason::Returned)
            << Result->Diagnostic;
        ASSERT_EQ(Result->Requests.size(), Input.Requests.size());
        EXPECT_EQ(Result->Requests[1].IOStatus, windows::StatusUnsuccessful);
        EXPECT_TRUE(Result->Requests[1].Completed);
        EXPECT_EQ(callCount(*Result, "WdfRequestGetStatus"), 1u);
        EXPECT_EQ(callCount(*Result, "WdfRequestGetCompletionParams"), 1u);
        EXPECT_TRUE(Result->UnloadCompleted);
      }
}

TEST(DriverKMDFPnp, FailedLowerCreateRetiresFrameworkFileObject) {
  for (const char *Image : pnpImages())
    for (char Mode : {'O', 'p', 's', 'a'}) {
      auto Input = forwardedFileOptions(Mode);
      Input.Requests.erase(
          std::remove_if(Input.Requests.begin(), Input.Requests.end(),
                         [](const DriverRequest &Request) {
                           return Request.Kind == DriverRequestKind::Cleanup ||
                                  Request.Kind == DriverRequestKind::Close;
                         }),
          Input.Requests.end());
      Input.Requests[1].FileBusCompletion =
          DriverBusCompletion{windows::StatusUnsuccessful};
      auto Result = emulateDriver(Image, Input);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
      ASSERT_EQ(Result->Requests.size(), Input.Requests.size());
      EXPECT_EQ(Result->Requests[1].IOStatus, windows::StatusUnsuccessful);
      EXPECT_TRUE(Result->Requests[1].Completed);
      EXPECT_EQ(callCount(*Result, "WdfRequestSend"),
                Mode == 'p' || Mode == 's' || Mode == 'a' ? 1u : 0u);
      EXPECT_EQ(callCount(*Result, "WdfRequestGetStatus"),
                Mode == 's' || Mode == 'a' ? 1u : 0u);
      EXPECT_TRUE(Result->UnloadCompleted);
      const auto Messages = pnpMessages(*Result);
      EXPECT_EQ(std::count(Messages.begin(), Messages.end(),
                           "KMDF PnP: file cleanup\n"),
                0);
      EXPECT_EQ(std::count(Messages.begin(), Messages.end(),
                           "KMDF PnP: file close\n"),
                0);
    }
}

TEST(DriverKMDFPnp, ForwardingRequiresItsConfiguredLowerResponse) {
  for (char Mode : {'O', 'p', 's', 'a'}) {
    auto Input = forwardedFileOptions(Mode);
    Input.Requests[1].FileBusCompletion.reset();
    auto Result = emulateDriver(NEVERD_KMDF_PNP_FIXTURE, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
    EXPECT_NE(Result->Diagnostic.find("configured bus response"),
              std::string::npos);
  }

  auto Input = options();
  Input.Requests[1].FileBusCompletion =
      DriverBusCompletion{windows::StatusSuccess};
  auto Result = emulateDriver(NEVERD_KMDF_PNP_FIXTURE, Input);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
  EXPECT_NE(Result->Diagnostic.find("lower file response was not consumed"),
            std::string::npos);
}

TEST(DriverKMDFPnp, ManualFileSendRejectsUnsupportedOptions) {
  for (const char *Image : pnpImages()) {
    auto Input = forwardedFileOptions('u');
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
    EXPECT_NE(
        Result->Diagnostic.find("only default asynchronous, synchronous or "
                                "send-and-forget file forwarding"),
        std::string::npos);
    ASSERT_GE(Result->Requests.size(), 2u);
    EXPECT_FALSE(Result->Requests[1].Completed);
  }
}

TEST(DriverKMDFPnp, ConfiguredDeviceTypeReachesTheWdmDeviceObject) {
  for (const char *Image : pnpImages())
    for (uint64_t Base : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Base);
      auto Input = options('K');
      Input.LoadAddress = Base;
      auto IO = std::find_if(Input.Requests.begin(), Input.Requests.end(),
                             [](const DriverRequest &Request) {
                               return Request.Kind ==
                                      DriverRequestKind::DeviceControl;
                             });
      ASSERT_NE(IO, Input.Requests.end());
      Input.Requests.erase(IO);
      auto Result = emulateDriver(Image, Input);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      EXPECT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
      EXPECT_TRUE(Result->UnloadCompleted);
      ASSERT_EQ(Result->Requests.size(), Input.Requests.size());
      for (const auto &Request : Result->Requests)
        EXPECT_EQ(Request.IOStatus, windows::StatusSuccess);
    }
}

TEST(DriverKMDFPnp, ExclusiveFdoDoesNotMakeTheNamedPdoExclusive) {
  for (const char *Image : pnpImages())
    for (uint64_t Base : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Base);
      auto Input = options('X');
      Input.LoadAddress = Base;
      Input.Requests = {
          pnp(DevicePnpRequest::Start),
          file(DriverRequestKind::Create, 1),
          file(DriverRequestKind::Create, 2),
          file(DriverRequestKind::Cleanup, 1),
          file(DriverRequestKind::Close, 1),
          file(DriverRequestKind::Cleanup, 2),
          file(DriverRequestKind::Close, 2),
          pnp(DevicePnpRequest::QueryRemove),
          pnp(DevicePnpRequest::Remove),
      };
      auto Result = emulateDriver(Image, Input);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
      EXPECT_TRUE(Result->UnloadCompleted);
      EXPECT_EQ(callCount(*Result, "WdfDeviceInitSetExclusive"), 1u);
      ASSERT_EQ(Result->Requests.size(), Input.Requests.size());
      for (const auto &Request : Result->Requests)
        EXPECT_EQ(Request.IOStatus, windows::StatusSuccess);
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

TEST(DriverKMDFPnp, PowerManagedQueuesTrackD0AcrossStopAndRestart) {
  for (const char *Image : pnpImages())
    for (uint64_t Base : {0x180000000ULL, 0x190000000ULL})
      for (char Mode : {'M', 'T'}) {
        SCOPED_TRACE(Image);
        SCOPED_TRACE(Base);
        SCOPED_TRACE(Mode);
        auto Input = options(Mode);
        Input.LoadAddress = Base;
        Input.Requests = {pnp(DevicePnpRequest::Start, 11),
                          file(DriverRequestKind::Create),
                          file(DriverRequestKind::DeviceControl),
                          file(DriverRequestKind::Cleanup),
                          file(DriverRequestKind::Close),
                          pnp(DevicePnpRequest::QueryStop),
                          pnp(DevicePnpRequest::Stop),
                          pnp(DevicePnpRequest::Start),
                          file(DriverRequestKind::Create, 8),
                          file(DriverRequestKind::DeviceControl, 8),
                          file(DriverRequestKind::Cleanup, 8),
                          file(DriverRequestKind::Close, 8),
                          pnp(DevicePnpRequest::QueryRemove),
                          pnp(DevicePnpRequest::Remove, 7)};
        auto Result = emulateDriver(Image, Input);
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        ASSERT_EQ(Result->Stop, DriverStopReason::Returned)
            << Result->Diagnostic;
        EXPECT_TRUE(Result->UnloadCompleted);
        ASSERT_EQ(Result->Requests.size(), Input.Requests.size());
        for (const auto &Request : Result->Requests)
          EXPECT_EQ(Request.IOStatus, windows::StatusSuccess);
        for (size_t Index : {size_t(2), size_t(9)})
          EXPECT_EQ(Result->Requests[Index].Output,
                    (std::vector<uint8_t>{'P', 'N', 'P', 0x7a}));
        EXPECT_EQ(callCount(*Result, "WdfIoQueueGetState"), 7u);
        EXPECT_EQ(
            pnpMessages(*Result),
            (std::vector<std::string>{
                "KMDF PnP: device ready\n", "KMDF PnP: D0 entry\n",
                "KMDF PnP: D0 exit\n", "KMDF PnP: D0 entry\n",
                "KMDF PnP: D0 exit\n", "KMDF PnP: device cleanup\n",
                "KMDF PnP: device destroy\n", "KMDF PnP: driver unload\n"}));
      }
}

TEST(DriverKMDFPnp, IoStopCompletesDriverOwnedRequestBeforeD0Exit) {
  for (const char *Image : pnpImages())
    for (uint64_t Base : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Base);
      auto Input = pendingStopOptions('C');
      Input.LoadAddress = Base;
      auto Result = emulateDriver(Image, Input);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
      EXPECT_TRUE(Result->UnloadCompleted);
      ASSERT_EQ(Result->Requests.size(), Input.Requests.size());
      EXPECT_EQ(Result->Requests[3].IOStatus, framework::RequestCancelled);
      EXPECT_EQ(Result->Requests[4].IOStatus, windows::StatusSuccess);
      EXPECT_EQ(pnpMessages(*Result),
                (std::vector<std::string>{
                    "KMDF PnP: device ready\n", "KMDF PnP: D0 entry\n",
                    "KMDF PnP: I/O stop\n", "KMDF PnP: D0 exit\n",
                    "KMDF PnP: D0 entry\n", "KMDF PnP: D0 exit\n",
                    "KMDF PnP: device cleanup\n", "KMDF PnP: device destroy\n",
                    "KMDF PnP: driver unload\n"}));
    }
}

TEST(DriverKMDFPnp, PowerTransitionWaitsForDriverOwnedWorkerCompletion) {
  for (const char *Image : pnpImages())
    for (uint64_t Base : {0x180000000ULL, 0x190000000ULL})
      for (char Mode : {'B', 'D', 'Y'}) {
        SCOPED_TRACE(Image);
        SCOPED_TRACE(Base);
        SCOPED_TRACE(Mode);
        auto Input = pendingStopOptions(Mode);
        Input.LoadAddress = Base;
        auto Result = emulateDriver(Image, Input);
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        ASSERT_EQ(Result->Stop, DriverStopReason::Returned)
            << Result->Diagnostic;
        EXPECT_TRUE(Result->UnloadCompleted);
        ASSERT_EQ(Result->Requests.size(), Input.Requests.size());
        for (const auto &Request : Result->Requests)
          EXPECT_EQ(Request.IOStatus, windows::StatusSuccess);
        EXPECT_EQ(callCount(*Result, "IoFreeWorkItem"), 1u);
        const auto Messages = pnpMessages(*Result);
        EXPECT_EQ(std::count(Messages.begin(), Messages.end(),
                             "KMDF PnP: I/O stop\n"),
                  Mode == 'D' ? 1 : 0);
      }
}

TEST(DriverKMDFPnp, PowerTransitionWithoutCompletionProducerStalls) {
  for (char Mode : {'E', 'Z'}) {
    SCOPED_TRACE(Mode);
    auto Input = pendingStopOptions(Mode);
    Input.Requests.resize(5);
    auto Result = emulateDriver(NEVERD_KMDF_PNP_FIXTURE, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
    EXPECT_NE(Result->Diagnostic.find("STATUS_PENDING request or blocked wait"),
              std::string::npos);
  }
}

TEST(DriverKMDFPnp, IoStopCanRequeueOrResumeAfterRestart) {
  for (const char *Image : pnpImages())
    for (uint64_t Base : {0x180000000ULL, 0x190000000ULL})
      for (char Mode : {'A', 'V'}) {
        SCOPED_TRACE(Image);
        SCOPED_TRACE(Base);
        SCOPED_TRACE(Mode);
        auto Input = pendingStopOptions(Mode);
        Input.LoadAddress = Base;
        auto Result = emulateDriver(Image, Input);
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        ASSERT_EQ(Result->Stop, DriverStopReason::Returned)
            << Result->Diagnostic;
        EXPECT_TRUE(Result->UnloadCompleted);
        ASSERT_EQ(Result->Requests.size(), Input.Requests.size());
        for (const auto &Request : Result->Requests)
          EXPECT_EQ(Request.IOStatus, windows::StatusSuccess);
        EXPECT_EQ(callCount(*Result, "WdfRequestStopAcknowledge"), 1u);
        EXPECT_EQ(Result->Requests[3].Output.size(), Mode == 'A' ? 4u : 0u);
        const auto Messages = pnpMessages(*Result);
        EXPECT_NE(
            std::find(Messages.begin(), Messages.end(), "KMDF PnP: I/O stop\n"),
            Messages.end());
        EXPECT_EQ(std::count(Messages.begin(), Messages.end(),
                             "KMDF PnP: I/O resume\n"),
                  Mode == 'V' ? 1 : 0);
      }
}

TEST(DriverKMDFPnp, QueuedPowerManagedRequestRunsAfterRestart) {
  for (const char *Image : pnpImages()) {
    SCOPED_TRACE(Image);
    auto Input = options('M');
    auto IO = file(DriverRequestKind::DeviceControl);
    IO.DeferCallbackDrain = true;
    Input.Requests = {pnp(DevicePnpRequest::Start),
                      file(DriverRequestKind::Create),
                      pnp(DevicePnpRequest::QueryStop),
                      pnp(DevicePnpRequest::Stop),
                      IO,
                      pnp(DevicePnpRequest::Start),
                      file(DriverRequestKind::Cleanup),
                      file(DriverRequestKind::Close),
                      pnp(DevicePnpRequest::QueryRemove),
                      pnp(DevicePnpRequest::Remove)};
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    EXPECT_TRUE(Result->UnloadCompleted);
    ASSERT_EQ(Result->Requests.size(), Input.Requests.size());
    for (const auto &Request : Result->Requests)
      EXPECT_EQ(Request.IOStatus, windows::StatusSuccess);
    EXPECT_EQ(Result->Requests[4].Output.size(), 4u);
  }
}

TEST(DriverKMDFPnp, SurpriseRemovalPurgesDriverOwnedRequestBeforeD0Exit) {
  for (const char *Image : pnpImages()) {
    SCOPED_TRACE(Image);
    auto Input = options('G');
    auto IO = file(DriverRequestKind::DeviceControl);
    IO.DeferCallbackDrain = true;
    Input.Requests = {pnp(DevicePnpRequest::Start),
                      file(DriverRequestKind::Create),
                      IO,
                      pnp(DevicePnpRequest::SurpriseRemoval),
                      file(DriverRequestKind::Cleanup),
                      file(DriverRequestKind::Close),
                      pnp(DevicePnpRequest::Remove)};
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    EXPECT_TRUE(Result->UnloadCompleted);
    ASSERT_EQ(Result->Requests.size(), Input.Requests.size());
    EXPECT_EQ(Result->Requests[2].IOStatus, framework::RequestCancelled);
    EXPECT_EQ(Result->Requests[3].IOStatus, windows::StatusSuccess);
    const auto Messages = pnpMessages(*Result);
    const auto Stop =
        std::find(Messages.begin(), Messages.end(), "KMDF PnP: I/O stop\n");
    const auto Suspend = std::find(Messages.begin(), Messages.end(),
                                   "KMDF PnP: self-managed suspend\n");
    const auto Exit =
        std::find(Messages.begin(), Messages.end(), "KMDF PnP: D0 exit\n");
    ASSERT_NE(Stop, Messages.end());
    ASSERT_NE(Suspend, Messages.end());
    ASSERT_NE(Exit, Messages.end());
    EXPECT_LT(Stop, Suspend);
    EXPECT_LT(Suspend, Exit);
  }
}

TEST(DriverKMDFPnp, AssignedMemoryIsVisibleUntilReleaseHardware) {
  for (const char *Image : pnpImages())
    for (uint64_t Base : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Base);
      auto Input = resourceOptions('R');
      Input.LoadAddress = Base;
      Input.Requests = {
          pnp(DevicePnpRequest::Start, 11),   pnp(DevicePnpRequest::QueryStop),
          pnp(DevicePnpRequest::Stop),        pnp(DevicePnpRequest::Start),
          pnp(DevicePnpRequest::QueryRemove), pnp(DevicePnpRequest::Remove, 7)};
      auto Result = emulateDriver(Image, Input);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
      EXPECT_TRUE(Result->UnloadCompleted);
      ASSERT_EQ(Result->Requests.size(), Input.Requests.size());
      for (const auto &Request : Result->Requests)
        EXPECT_EQ(Request.IOStatus, windows::StatusSuccess);
      EXPECT_EQ(callCount(*Result, "WdfCmResourceListGetCount"), 6u);
      EXPECT_EQ(callCount(*Result, "WdfCmResourceListGetDescriptor"), 10u);
      EXPECT_EQ(callCount(*Result, "MmMapIoSpace"), 2u);
      EXPECT_EQ(callCount(*Result, "MmUnmapIoSpace"), 2u);
      EXPECT_EQ(
          pnpMessages(*Result),
          (std::vector<std::string>{
              "KMDF PnP: device ready\n", "KMDF PnP: mapped hardware\n",
              "KMDF PnP: D0 entry\n", "KMDF PnP: D0 exit\n",
              "KMDF PnP: unmapped hardware\n", "KMDF PnP: mapped hardware\n",
              "KMDF PnP: D0 entry\n", "KMDF PnP: D0 exit\n",
              "KMDF PnP: unmapped hardware\n", "KMDF PnP: device cleanup\n",
              "KMDF PnP: device destroy\n", "KMDF PnP: driver unload\n"}));
    }
}

TEST(DriverKMDFPnp, ResourceDescriptorsRejectGuestMutation) {
  auto Input = resourceOptions('W');
  Input.Requests = {pnp(DevicePnpRequest::Start)};
  auto Result = emulateDriver(NEVERD_KMDF_PNP_FIXTURE, Input);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
  EXPECT_NE(Result->Diagnostic.find("read-only framework storage"),
            std::string::npos);
}

TEST(DriverKMDFPnp, StopStillRejectsUnreleasedHardwareMapping) {
  auto Input = resourceOptions('L');
  Input.Requests = {pnp(DevicePnpRequest::Start),
                    pnp(DevicePnpRequest::QueryStop),
                    pnp(DevicePnpRequest::Stop)};
  auto Result = emulateDriver(NEVERD_KMDF_PNP_FIXTURE, Input);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
  EXPECT_NE(Result->Diagnostic.find("device still owns an I/O-space mapping"),
            std::string::npos);
  EXPECT_EQ(callCount(*Result, "WdfCmResourceListGetDescriptor"), 5u);
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

DriverRequest devicePower(DevicePowerRequest Minor, DevicePowerState State,
                          uint64_t Delay = 0) {
  DriverRequest Request;
  Request.Kind = DriverRequestKind::Power;
  Request.DeviceID = DeviceID.str();
  DriverPowerOperation Operation;
  Operation.Minor = Minor;
  Operation.Type = DriverPowerType::Device;
  Operation.State = uint32_t(State);
  Operation.BusCompletion = {windows::StatusSuccess, Delay};
  Request.Power = Operation;
  return Request;
}

TEST(DriverKMDFPnp, DevicePowerRetainsResourcesAndBracketsProviderPowerChange) {
  for (const char *Image : pnpImages())
    for (uint64_t Base : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Base);
      auto Input = resourceOptions('9');
      Input.LoadAddress = Base;
      Input.Requests = {
          pnp(DevicePnpRequest::Start),
          devicePower(DevicePowerRequest::Query, DevicePowerState::D3, 3),
          devicePower(DevicePowerRequest::Set, DevicePowerState::D3, 7),
          devicePower(DevicePowerRequest::Set, DevicePowerState::D0, 11),
          pnp(DevicePnpRequest::QueryRemove),
          pnp(DevicePnpRequest::Remove)};
      auto Result = emulateDriver(Image, Input);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
      EXPECT_TRUE(Result->UnloadCompleted);
      ASSERT_EQ(Result->Requests.size(), Input.Requests.size());
      for (const auto &Request : Result->Requests) {
        EXPECT_TRUE(Request.Completed);
        EXPECT_EQ(Request.IOStatus, windows::StatusSuccess);
      }
      EXPECT_EQ(Result->Requests[1].Power->DeviceStateAfter,
                DevicePowerState::D0);
      EXPECT_EQ(Result->Requests[2].Power->DeviceStateAfter,
                DevicePowerState::D3);
      EXPECT_EQ(Result->Requests[3].Power->DeviceStateAfter,
                DevicePowerState::D0);
      EXPECT_EQ(callCount(*Result, "MmMapIoSpace"), 1u);
      EXPECT_EQ(callCount(*Result, "MmUnmapIoSpace"), 1u);
      EXPECT_EQ(
          pnpMessages(*Result),
          (std::vector<std::string>{"KMDF PnP: device ready\n",
                                    "KMDF PnP: mapped hardware\n",
                                    "KMDF PnP: D0 entry\n",
                                    "KMDF PnP: D0 entry post interrupts\n",
                                    "KMDF PnP: self-managed init\n",
                                    "KMDF PnP: self-managed suspend\n",
                                    "KMDF PnP: D0 exit pre interrupts\n",
                                    "KMDF PnP: D0 exit\n",
                                    "KMDF PnP: D0 entry\n",
                                    "KMDF PnP: D0 entry post interrupts\n",
                                    "KMDF PnP: self-managed restart\n",
                                    "KMDF PnP: self-managed suspend\n",
                                    "KMDF PnP: D0 exit pre interrupts\n",
                                    "KMDF PnP: D0 exit\n",
                                    "KMDF PnP: unmapped hardware\n",
                                    "KMDF PnP: self-managed flush\n",
                                    "KMDF PnP: self-managed cleanup\n",
                                    "KMDF PnP: device cleanup\n",
                                    "KMDF PnP: device destroy\n",
                                    "KMDF PnP: driver unload\n"}));
    }
}

TEST(DriverKMDFPnp, ReassertingDevicePowerDoesNotDuplicateCallbacks) {
  auto Input = options('0');
  Input.Requests = {
      pnp(DevicePnpRequest::Start),
      devicePower(DevicePowerRequest::Set, DevicePowerState::D0, 7),
      pnp(DevicePnpRequest::QueryRemove), pnp(DevicePnpRequest::Remove)};
  auto Result = emulateDriver(NEVERD_KMDF_PNP_FIXTURE, Input);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  EXPECT_TRUE(Result->UnloadCompleted);
  const auto Messages = pnpMessages(*Result);
  EXPECT_EQ(
      std::count(Messages.begin(), Messages.end(), "KMDF PnP: D0 entry\n"), 1);
  EXPECT_EQ(std::count(Messages.begin(), Messages.end(),
                       "KMDF PnP: self-managed init\n"),
            1);
}

DriverRequest systemPower(DevicePowerRequest Minor, SystemPowerState State,
                          uint64_t Delay = 0) {
  auto Request = devicePower(Minor, DevicePowerState::D0, Delay);
  Request.Power->Type = DriverPowerType::System;
  Request.Power->State = uint32_t(State);
  Request.Power->Action = State == SystemPowerState::Working
                              ? DriverPowerAction::None
                              : DriverPowerAction::Sleep;
  return Request;
}

TEST(DriverKMDFPnp, SystemPowerPolicyUsesIndependentExplicitDeviceResponses) {
  for (const char *Image : pnpImages()) {
    auto Input = resourceOptions('9');
    auto Down =
        *devicePower(DevicePowerRequest::Set, DevicePowerState::D3, 7).Power;
    Down.Action = DriverPowerAction::Sleep;
    auto Up =
        *devicePower(DevicePowerRequest::Set, DevicePowerState::D0, 13).Power;
    auto Query = Down;
    Query.Minor = DevicePowerRequest::Query;
    Input.PnpDevices.front().RequestedDevicePower = {Query, Down, Up};
    Input.Requests = {
        pnp(DevicePnpRequest::Start),
        systemPower(DevicePowerRequest::Query, SystemPowerState::Sleeping3, 3),
        systemPower(DevicePowerRequest::Set, SystemPowerState::Sleeping3, 5),
        systemPower(DevicePowerRequest::Set, SystemPowerState::Working, 11),
        pnp(DevicePnpRequest::QueryRemove),
        pnp(DevicePnpRequest::Remove)};
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    EXPECT_TRUE(Result->UnloadCompleted);
    ASSERT_EQ(Result->Requests.size(), Input.Requests.size() + 3);
    for (const auto &Request : Result->Requests) {
      EXPECT_TRUE(Request.Completed);
      EXPECT_EQ(Request.IOStatus, windows::StatusSuccess);
    }
    const auto FindPower = [&](DriverRequestOrigin Origin, DriverPowerType Type,
                               uint32_t State) {
      return std::find_if(Result->Requests.begin(), Result->Requests.end(),
                          [&](const auto &Request) {
                            return Request.Origin == Origin && Request.Power &&
                                   Request.Power->Type == Type &&
                                   Request.Power->Minor ==
                                       DevicePowerRequest::Set &&
                                   Request.Power->State == State;
                          });
    };
    const auto SleepIt =
        FindPower(DriverRequestOrigin::Scenario, DriverPowerType::System,
                  uint32_t(SystemPowerState::Sleeping3));
    const auto WakeIt =
        FindPower(DriverRequestOrigin::Scenario, DriverPowerType::System,
                  uint32_t(SystemPowerState::Working));
    const auto DownIt =
        FindPower(DriverRequestOrigin::FrameworkPowerPolicy,
                  DriverPowerType::Device, uint32_t(DevicePowerState::D3));
    const auto UpIt =
        FindPower(DriverRequestOrigin::FrameworkPowerPolicy,
                  DriverPowerType::Device, uint32_t(DevicePowerState::D0));
    ASSERT_NE(SleepIt, Result->Requests.end());
    ASSERT_NE(WakeIt, Result->Requests.end());
    ASSERT_NE(DownIt, Result->Requests.end());
    ASSERT_NE(UpIt, Result->Requests.end());
    const auto &Sleep = *SleepIt;
    const auto &Wake = *WakeIt;
    const auto &DownResult = *DownIt;
    const auto &UpResult = *UpIt;
    EXPECT_EQ(DownResult.Origin, DriverRequestOrigin::FrameworkPowerPolicy);
    EXPECT_EQ(UpResult.Origin, DriverRequestOrigin::FrameworkPowerPolicy);
    EXPECT_EQ(DownResult.ResponseIndex, 1u);
    EXPECT_EQ(UpResult.ResponseIndex, 2u);
    EXPECT_NE(DownResult.IRP, Sleep.IRP);
    EXPECT_NE(UpResult.IRP, Wake.IRP);
    EXPECT_EQ(DownResult.Power->BusReceivedAt100ns,
              Sleep.Power->BusCompletedAt100ns);
    EXPECT_EQ(UpResult.Power->BusReceivedAt100ns,
              Wake.Power->BusCompletedAt100ns);
    EXPECT_EQ(Sleep.Power->SystemStateAfter, SystemPowerState::Sleeping3);
    EXPECT_EQ(Wake.Power->SystemStateAfter, SystemPowerState::Working);
    EXPECT_EQ(DownResult.Power->DeviceStateAfter, DevicePowerState::D3);
    EXPECT_EQ(UpResult.Power->DeviceStateAfter, DevicePowerState::D0);
    EXPECT_EQ(callCount(*Result, "MmMapIoSpace"), 1u);
    EXPECT_EQ(callCount(*Result, "MmUnmapIoSpace"), 1u);
  }
}

TEST(DriverKMDFPnp, FailedDeviceQueryCompletesSystemQueryWithoutPowerChange) {
  for (const char *Image : pnpImages()) {
    auto Input = options('0');
    auto Query =
        *devicePower(DevicePowerRequest::Query, DevicePowerState::D3, 7).Power;
    Query.Action = DriverPowerAction::Sleep;
    Query.BusCompletion.Status = windows::StatusUnsuccessful;
    Input.PnpDevices.front().RequestedDevicePower = {Query};
    Input.Requests = {
        pnp(DevicePnpRequest::Start),
        systemPower(DevicePowerRequest::Query, SystemPowerState::Sleeping3, 3),
        pnp(DevicePnpRequest::QueryRemove), pnp(DevicePnpRequest::Remove)};
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    EXPECT_TRUE(Result->UnloadCompleted);
    ASSERT_EQ(Result->Requests.size(), Input.Requests.size() + 1);
    unsigned Queries = 0;
    for (const auto &Request : Result->Requests) {
      if (!Request.Power)
        continue;
      ++Queries;
      EXPECT_TRUE(Request.Completed);
      EXPECT_EQ(Request.IOStatus, windows::StatusUnsuccessful);
      EXPECT_EQ(Request.Power->Minor, DevicePowerRequest::Query);
      EXPECT_EQ(Request.Power->DeviceStateAfter, DevicePowerState::D0);
      EXPECT_EQ(Request.Power->SystemStateAfter, SystemPowerState::Working);
      if (Request.Origin == DriverRequestOrigin::FrameworkPowerPolicy)
        EXPECT_EQ(Request.ResponseIndex, 0u);
      else
        EXPECT_EQ(Request.Power->BusStatus, windows::StatusSuccess);
    }
    EXPECT_EQ(Queries, 2u);
  }
}

TEST(DriverKMDFPnp, NonOwnerForwardsSystemPowerWithoutInventingChildren) {
  auto Input = options('8');
  Input.Requests = {
      pnp(DevicePnpRequest::Start),
      systemPower(DevicePowerRequest::Set, SystemPowerState::Sleeping3),
      systemPower(DevicePowerRequest::Set, SystemPowerState::Working),
      pnp(DevicePnpRequest::QueryRemove), pnp(DevicePnpRequest::Remove)};
  auto Result = emulateDriver(NEVERD_KMDF_PNP_FIXTURE, Input);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  EXPECT_TRUE(Result->UnloadCompleted);
  EXPECT_EQ(Result->Requests.size(), Input.Requests.size());
  EXPECT_EQ(callCount(*Result, "WdfDeviceInitSetPowerPolicyOwnership"), 1u);
  const auto Messages = pnpMessages(*Result);
  EXPECT_EQ(
      std::count(Messages.begin(), Messages.end(), "KMDF PnP: D0 entry\n"), 1);
  EXPECT_EQ(std::count(Messages.begin(), Messages.end(),
                       "KMDF PnP: self-managed suspend\n"),
            1);
}

TEST(DriverKMDFPnp, PowerPolicyRejectsMissingAndMismatchedDeviceResponses) {
  for (bool Mismatched : {false, true}) {
    auto Input = options('0');
    Input.Requests = {
        pnp(DevicePnpRequest::Start),
        systemPower(DevicePowerRequest::Set, SystemPowerState::Sleeping3)};
    if (Mismatched)
      Input.PnpDevices.front().RequestedDevicePower = {
          *devicePower(DevicePowerRequest::Set, DevicePowerState::D0).Power};
    auto Result = emulateDriver(NEVERD_KMDF_PNP_FIXTURE, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
    EXPECT_NE(Result->Diagnostic.find("requested_device_power"),
              std::string::npos);
    EXPECT_EQ(Result->Requests.size(), Input.Requests.size());
  }
}

TEST(DriverKMDFPnp, SelfManagedIoInitializesOnceAcrossRebalanceAndRemoval) {
  for (const char *Image : pnpImages())
    for (uint64_t Base : {0x180000000ULL, 0x190000000ULL})
      for (char Mode : {'0', '7'}) {
        SCOPED_TRACE(Image);
        SCOPED_TRACE(Base);
        SCOPED_TRACE(Mode);
        auto Input = options(Mode);
        Input.LoadAddress = Base;
        Input.Requests = {pnp(DevicePnpRequest::Start),
                          pnp(DevicePnpRequest::QueryStop),
                          pnp(DevicePnpRequest::Stop),
                          pnp(DevicePnpRequest::Start),
                          pnp(DevicePnpRequest::QueryRemove),
                          pnp(DevicePnpRequest::Remove)};
        auto Result = emulateDriver(Image, Input);
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        ASSERT_EQ(Result->Stop, DriverStopReason::Returned)
            << Result->Diagnostic;
        EXPECT_TRUE(Result->UnloadCompleted);
        for (const auto &Request : Result->Requests)
          EXPECT_EQ(Request.IOStatus, windows::StatusSuccess);
        std::vector<std::string> Expected{
            "KMDF PnP: device ready\n", "KMDF PnP: prepare hardware\n",
            "KMDF PnP: D0 entry\n", "KMDF PnP: D0 entry post interrupts\n"};
        if (Mode != '7')
          Expected.push_back("KMDF PnP: self-managed init\n");
        Expected.insert(
            Expected.end(),
            {"KMDF PnP: self-managed suspend\n",
             "KMDF PnP: D0 exit pre interrupts\n", "KMDF PnP: D0 exit\n",
             "KMDF PnP: release hardware\n", "KMDF PnP: prepare hardware\n",
             "KMDF PnP: D0 entry\n", "KMDF PnP: D0 entry post interrupts\n",
             "KMDF PnP: self-managed restart\n",
             "KMDF PnP: self-managed suspend\n",
             "KMDF PnP: D0 exit pre interrupts\n", "KMDF PnP: D0 exit\n",
             "KMDF PnP: release hardware\n", "KMDF PnP: self-managed flush\n",
             "KMDF PnP: self-managed cleanup\n", "KMDF PnP: device cleanup\n",
             "KMDF PnP: device destroy\n", "KMDF PnP: driver unload\n"});
        EXPECT_EQ(pnpMessages(*Result), Expected);
      }
}

TEST(DriverKMDFPnp, SelfManagedIoFlushesOnSurpriseAndCleansUpOnRemoval) {
  for (const char *Image : pnpImages()) {
    auto Input = options('0');
    Input.Requests = {pnp(DevicePnpRequest::Start),
                      pnp(DevicePnpRequest::SurpriseRemoval),
                      pnp(DevicePnpRequest::Remove)};
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    EXPECT_TRUE(Result->UnloadCompleted);
    EXPECT_EQ(
        pnpMessages(*Result),
        (std::vector<std::string>{
            "KMDF PnP: device ready\n", "KMDF PnP: prepare hardware\n",
            "KMDF PnP: D0 entry\n", "KMDF PnP: D0 entry post interrupts\n",
            "KMDF PnP: self-managed init\n", "KMDF PnP: self-managed suspend\n",
            "KMDF PnP: D0 exit pre interrupts\n", "KMDF PnP: D0 exit\n",
            "KMDF PnP: release hardware\n", "KMDF PnP: self-managed flush\n",
            "KMDF PnP: self-managed cleanup\n", "KMDF PnP: device cleanup\n",
            "KMDF PnP: device destroy\n", "KMDF PnP: driver unload\n"}));
  }
}

TEST(DriverKMDFPnp, ReleasedResourceListsExpireBeforeSelfManagedFlush) {
  auto Input = options('e');
  Input.Requests = {pnp(DevicePnpRequest::Start),
                    pnp(DevicePnpRequest::QueryRemove),
                    pnp(DevicePnpRequest::Remove)};
  auto Result = emulateDriver(NEVERD_KMDF_PNP_FIXTURE, Input);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
  EXPECT_NE(Result->Diagnostic.find("resource-list query requires a live"),
            std::string::npos)
      << Result->Diagnostic;
  const auto Messages = pnpMessages(*Result);
  EXPECT_NE(std::find(Messages.begin(), Messages.end(),
                      "KMDF PnP: release hardware\n"),
            Messages.end());
  EXPECT_EQ(std::find(Messages.begin(), Messages.end(),
                      "KMDF PnP: self-managed flush\n"),
            Messages.end());
}

TEST(DriverKMDFPnp, FailedSelfManagedInitializationUnwindsBeforeRemoval) {
  for (const char *Image : pnpImages()) {
    auto Input = options('1');
    Input.Requests = {pnp(DevicePnpRequest::Start),
                      pnp(DevicePnpRequest::Remove)};
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    EXPECT_TRUE(Result->UnloadCompleted);
    ASSERT_EQ(Result->Requests.size(), 2u);
    EXPECT_EQ(Result->Requests[0].IOStatus, windows::StatusUnsuccessful);
    EXPECT_EQ(Result->Requests[1].IOStatus, windows::StatusSuccess);
    EXPECT_EQ(
        pnpMessages(*Result),
        (std::vector<std::string>{
            "KMDF PnP: device ready\n", "KMDF PnP: prepare hardware\n",
            "KMDF PnP: D0 entry\n", "KMDF PnP: D0 entry post interrupts\n",
            "KMDF PnP: self-managed init\n",
            "KMDF PnP: D0 exit pre interrupts\n", "KMDF PnP: D0 exit\n",
            "KMDF PnP: release hardware\n", "KMDF PnP: self-managed flush\n",
            "KMDF PnP: self-managed cleanup\n", "KMDF PnP: device cleanup\n",
            "KMDF PnP: device destroy\n", "KMDF PnP: driver unload\n"}));
  }
}

TEST(DriverKMDFPnp, FailedSelfManagedRestartReleasesHardwareAndCleansUpOnce) {
  for (const char *Image : pnpImages()) {
    auto Input = options('3');
    Input.Requests = {pnp(DevicePnpRequest::Start),
                      pnp(DevicePnpRequest::QueryStop),
                      pnp(DevicePnpRequest::Stop), pnp(DevicePnpRequest::Start),
                      pnp(DevicePnpRequest::Remove)};
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    EXPECT_TRUE(Result->UnloadCompleted);
    ASSERT_EQ(Result->Requests.size(), Input.Requests.size());
    EXPECT_EQ(Result->Requests[3].IOStatus, windows::StatusUnsuccessful);
    EXPECT_EQ(Result->Requests[4].IOStatus, windows::StatusSuccess);
    const auto Messages = pnpMessages(*Result);
    const auto Count = [&](const char *Text) {
      return std::count(Messages.begin(), Messages.end(), Text);
    };
    EXPECT_EQ(Count("KMDF PnP: self-managed init\n"), 1);
    EXPECT_EQ(Count("KMDF PnP: self-managed restart\n"), 1);
    EXPECT_EQ(Count("KMDF PnP: self-managed flush\n"), 1);
    EXPECT_EQ(Count("KMDF PnP: self-managed cleanup\n"), 1);
    EXPECT_EQ(Count("KMDF PnP: release hardware\n"), 2);
  }
}

TEST(DriverKMDFPnp, FailedPostInterruptEntryDoesNotInitializeSelfManagedIo) {
  auto Input = options('4');
  Input.Requests = {pnp(DevicePnpRequest::Start),
                    pnp(DevicePnpRequest::Remove)};
  auto Result = emulateDriver(NEVERD_KMDF_PNP_FIXTURE, Input);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  EXPECT_TRUE(Result->UnloadCompleted);
  ASSERT_EQ(Result->Requests.size(), 2u);
  EXPECT_EQ(Result->Requests[0].IOStatus, windows::StatusUnsuccessful);
  EXPECT_EQ(pnpMessages(*Result),
            (std::vector<std::string>{
                "KMDF PnP: device ready\n", "KMDF PnP: prepare hardware\n",
                "KMDF PnP: D0 entry\n", "KMDF PnP: D0 entry post interrupts\n",
                "KMDF PnP: D0 exit pre interrupts\n", "KMDF PnP: D0 exit\n",
                "KMDF PnP: release hardware\n", "KMDF PnP: device cleanup\n",
                "KMDF PnP: device destroy\n", "KMDF PnP: driver unload\n"}));
}

TEST(DriverKMDFPnp, UnmodeledPnpCallbackFailsAtRegistration) {
  auto Result = emulateDriver(NEVERD_KMDF_PNP_FIXTURE, options('U'));
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
  EXPECT_NE(Result->Diagnostic.find("unsupported PnP power event callback"),
            std::string::npos);
  EXPECT_TRUE(Result->Requests.empty());
}

TEST(DriverKMDFPnp, QueryCallbacksRunAndMayWaitBeforeProviderDispatch) {
  for (const char *Image : pnpImages())
    for (uint64_t Base : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Base);
      auto Input = options('v');
      Input.LoadAddress = Base;
      Input.Requests = {pnp(DevicePnpRequest::Start),
                        pnp(DevicePnpRequest::QueryStop),
                        pnp(DevicePnpRequest::CancelStop),
                        pnp(DevicePnpRequest::QueryRemove),
                        pnp(DevicePnpRequest::CancelRemove),
                        pnp(DevicePnpRequest::QueryRemove),
                        pnp(DevicePnpRequest::Remove)};
      auto Result = emulateDriver(Image, Input);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
      ASSERT_EQ(Result->Requests.size(), Input.Requests.size());
      EXPECT_TRUE(Result->UnloadCompleted);
      const std::array<size_t, 3> Queries{1, 3, 5};
      for (size_t Index : Queries) {
        const auto &Query = Result->Requests[Index];
        EXPECT_EQ(Query.DispatchStatus, windows::StatusPending);
        EXPECT_EQ(Query.IOStatus, windows::StatusSuccess);
        ASSERT_TRUE(Query.Pnp->BusReceivedAt100ns);
        ASSERT_TRUE(Result->Requests[Index - 1].Pnp->BusCompletedAt100ns);
        EXPECT_GE(*Query.Pnp->BusReceivedAt100ns,
                  *Result->Requests[Index - 1].Pnp->BusCompletedAt100ns + 3);
      }
      EXPECT_EQ(pnpMessages(*Result),
                (std::vector<std::string>{
                    "KMDF PnP: device ready\n", "KMDF PnP: D0 entry\n",
                    "KMDF PnP: query stop\n", "KMDF PnP: query remove\n",
                    "KMDF PnP: query remove\n", "KMDF PnP: D0 exit\n",
                    "KMDF PnP: device cleanup\n", "KMDF PnP: device destroy\n",
                    "KMDF PnP: driver unload\n"}));
    }
}

TEST(DriverKMDFPnp, QueryVetoDoesNotReachProviderOrDisableDeviceIO) {
  for (const char *Image : pnpImages()) {
    SCOPED_TRACE(Image);
    auto Input = options('x');
    Input.Requests = {pnp(DevicePnpRequest::Start),
                      pnp(DevicePnpRequest::QueryStop),
                      file(DriverRequestKind::Create),
                      file(DriverRequestKind::DeviceControl),
                      file(DriverRequestKind::Cleanup),
                      file(DriverRequestKind::Close),
                      pnp(DevicePnpRequest::QueryRemove),
                      pnp(DevicePnpRequest::QueryRemove),
                      pnp(DevicePnpRequest::Remove)};
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    ASSERT_EQ(Result->Requests.size(), Input.Requests.size());
    for (size_t Index : {1u, 6u}) {
      const auto &Rejected = Result->Requests[Index];
      EXPECT_TRUE(Rejected.Completed);
      EXPECT_EQ(Rejected.IOStatus, windows::StatusUnsuccessful);
      EXPECT_EQ(Rejected.Pnp->StateAfter, DevicePnpState::Started);
      EXPECT_FALSE(Rejected.Pnp->BusReceivedAt100ns);
      EXPECT_FALSE(Rejected.Pnp->BusCompletedAt100ns);
    }
    EXPECT_EQ(Result->Requests[3].Output,
              (std::vector<uint8_t>{'P', 'N', 'P', 0x7a}));
    EXPECT_EQ(Result->Requests[7].IOStatus, windows::StatusSuccess);
    EXPECT_TRUE(Result->UnloadCompleted);
  }
}

TEST(DriverKMDFPnp, SurpriseNotificationRunsOnceForStartedAndStoppedDevices) {
  for (const char *Image : pnpImages())
    for (bool Stopped : {false, true}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Stopped);
      auto Input = options('v');
      Input.Requests = {pnp(DevicePnpRequest::Start)};
      if (Stopped) {
        Input.Requests.push_back(pnp(DevicePnpRequest::QueryStop));
        Input.Requests.push_back(pnp(DevicePnpRequest::Stop));
      }
      Input.Requests.push_back(pnp(DevicePnpRequest::SurpriseRemoval));
      Input.Requests.push_back(pnp(DevicePnpRequest::Remove));
      auto Result = emulateDriver(Image, Input);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
      EXPECT_TRUE(Result->UnloadCompleted);
      auto Messages = pnpMessages(*Result);
      EXPECT_EQ(std::count(Messages.begin(), Messages.end(),
                           "KMDF PnP: surprise removal\n"),
                1);
      auto Surprise = std::find(Messages.begin(), Messages.end(),
                                "KMDF PnP: surprise removal\n");
      auto Exit =
          std::find(Messages.begin(), Messages.end(), "KMDF PnP: D0 exit\n");
      EXPECT_EQ(Surprise < Exit, !Stopped);
    }
}

TEST(DriverKMDFPnp, InvalidQueryCallbackStatusesFailBeforeProviderDispatch) {
  for (char Mode : {'y', 'z'}) {
    SCOPED_TRACE(Mode);
    auto Input = options(Mode);
    Input.Requests = {pnp(DevicePnpRequest::Start),
                      pnp(DevicePnpRequest::QueryStop)};
    auto Result = emulateDriver(NEVERD_KMDF_PNP_FIXTURE, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
    EXPECT_NE(Result->Diagnostic.find(Mode == 'y' ? "STATUS_PENDING"
                                                  : "STATUS_NOT_SUPPORTED"),
              std::string::npos);
    ASSERT_EQ(Result->Requests.size(), 2u);
    EXPECT_FALSE(Result->Requests.back().Pnp->BusReceivedAt100ns);
    EXPECT_FALSE(Result->Requests.back().Completed);
  }
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
