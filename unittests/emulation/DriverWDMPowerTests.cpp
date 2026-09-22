//===- DriverWDMPowerTests.cpp - Genuine WDK power execution -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Execute original WDK power-policy code with explicit power-manager facts,
/// independent child IRPs, per-device notifications and real guest callbacks.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/emulation/DriverSession.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
#ifdef NEVERD_WDM_POWER_FIXTURE
constexpr uint32_t Failed = 0xc0000001;

std::vector<const char *> powerImages() {
  std::vector<const char *> Images{NEVERD_WDM_POWER_FIXTURE};
#ifdef NEVERD_WDM_POWER_CFG_FIXTURE
  Images.push_back(NEVERD_WDM_POWER_CFG_FIXTURE);
#endif
  return Images;
}

DriverOptions powerOptions(char Mode = 'S') {
  DriverOptions Options;
  Options.ServiceName = std::string("NeverDPower") + Mode;
  Options.LoadAddress = 0x190000000;
  Options.Unload = true;
  DriverPnpDevice Device;
  Device.ID = "power0";
  Device.InitialDevicePower = DevicePowerState::D0;
  Device.InitialSystemPower = SystemPowerState::Working;
  Device.InitialReportedDevicePower = DevicePowerState::D3;
  Options.PnpDevices.push_back(Device);
  return Options;
}

DriverRequest pnp(DevicePnpRequest Minor, llvm::StringRef ID = "power0") {
  DriverRequest Request;
  Request.Kind = DriverRequestKind::Pnp;
  Request.DeviceID = ID.str();
  Request.Pnp = DriverPnpOperation{Minor, DriverBusCompletion{0, 0}};
  return Request;
}

DriverPowerOperation operation(DevicePowerRequest Minor, DriverPowerType Type,
                               uint32_t State, uint64_t Delay = 0,
                               uint32_t Status = 0, bool Sleep = false) {
  DriverPowerOperation Operation;
  Operation.Minor = Minor;
  Operation.Type = Type;
  Operation.State = State;
  Operation.BusCompletion = {Status, Delay};
  if (Type == DriverPowerType::System || Sleep) {
    Operation.Action = DriverPowerAction::Sleep;
    // Explicit simple S0/S3 facts. The guest only reads public target/effective
    // bitfields; current state and reserved fields remain system-owned.
    const uint32_t Current = State == 4 ? 1 : 4;
    Operation.SystemContext = (Current << 16) | (State << 12) | (State << 8);
  }
  return Operation;
}

DriverRequest power(DriverPowerOperation Operation,
                    llvm::StringRef ID = "power0") {
  DriverRequest Request;
  Request.Kind = DriverRequestKind::Power;
  Request.DeviceID = ID.str();
  Request.Power = Operation;
  return Request;
}

void remove(DriverOptions &Options, llvm::StringRef ID = "power0") {
  Options.Requests.push_back(pnp(DevicePnpRequest::QueryRemove, ID));
  Options.Requests.push_back(pnp(DevicePnpRequest::Remove, ID));
}

void systemCycle(DriverOptions &Options, uint64_t SystemDelay = 0,
                 uint64_t ChildDelay = 0, size_t Device = 0) {
  const auto ID = Options.PnpDevices[Device].ID;
  for (uint32_t State : {4u, 1u}) {
    Options.Requests.push_back(
        power(operation(DevicePowerRequest::Set, DriverPowerType::System, State,
                        SystemDelay),
              ID));
    Options.PnpDevices[Device].RequestedDevicePower.push_back(
        operation(DevicePowerRequest::Set, DriverPowerType::Device, State,
                  ChildDelay, 0, true));
  }
}

std::vector<const DriverRequestResult *> requests(const DriverResult &Result,
                                                  DriverRequestOrigin Origin) {
  std::vector<const DriverRequestResult *> Selected;
  for (const auto &Request : Result.Requests)
    if (Request.Origin == Origin)
      Selected.push_back(&Request);
  return Selected;
}

size_t countAPI(const DriverResult &Result, llvm::StringRef Name) {
  return std::count_if(Result.Calls.begin(), Result.Calls.end(),
                       [Name](const auto &Call) { return Call.Name == Name; });
}

size_t messageIndex(const DriverResult &Result, llvm::StringRef Text,
                    size_t Begin = 0) {
  for (size_t I = Begin; I < Result.Messages.size(); ++I)
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
  for (const auto &Message : Result.Messages)
    EXPECT_EQ(Message.find("WDM power: failure"), std::string::npos) << Message;
  for (const auto &Request : Result.Requests)
    EXPECT_TRUE(Request.Completed);
}

void successfulChild(const DriverRequestResult &Child, uint32_t Index,
                     uint32_t State, llvm::StringRef ID = "power0") {
  EXPECT_EQ(Child.Kind, DriverRequestKind::Power);
  EXPECT_EQ(Child.Origin, DriverRequestOrigin::PoRequestPowerIrp);
  EXPECT_EQ(Child.ResponseIndex, Index);
  EXPECT_EQ(Child.DeviceID, ID);
  EXPECT_NE(Child.IRP, 0u);
  EXPECT_EQ(Child.IOStatus, 0u);
  EXPECT_EQ(Child.Information, 0u);
  ASSERT_TRUE(Child.Power);
  EXPECT_EQ(Child.Power->Type, DriverPowerType::Device);
  EXPECT_EQ(Child.Power->State, State);
  EXPECT_EQ(Child.Power->BusStatus, 0u);
  EXPECT_TRUE(Child.Power->RequestedDeviceObject);
}

TEST(DriverWDMPower, DirectQueryAndStateCycleExecuteNormalCfgAndRebasedImages) {
  for (const auto *Image : powerImages())
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Address);
      auto Options = powerOptions();
      Options.LoadAddress = Address;
      Options.Requests = {
          pnp(DevicePnpRequest::Start),
          power(
              operation(DevicePowerRequest::Query, DriverPowerType::Device, 4)),
          power(operation(DevicePowerRequest::Set, DriverPowerType::Device, 4)),
          power(
              operation(DevicePowerRequest::Set, DriverPowerType::Device, 1))};
      remove(Options);
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      clean(*Result);
      ASSERT_EQ(Result->Requests.size(), 6u);
      EXPECT_EQ(countAPI(*Result, "PoSetPowerState"), 3u);
      EXPECT_EQ(countAPI(*Result, "PoCallDriver"), 3u);
      EXPECT_EQ(countAPI(*Result, "PoStartNextPowerIrp"), 3u);
      EXPECT_EQ(countAPI(*Result, "PoRequestPowerIrp"), 0u);
      EXPECT_LT(messageIndex(*Result, "notify unit=1 old=4 new=1"),
                Result->Messages.size());
      EXPECT_LT(
          messageIndex(*Result, "notify unit=1 old=1 new=4"),
          messageIndex(*Result, "device-completion unit=1 minor=2 state=4"));
      EXPECT_LT(
          messageIndex(*Result, "device-completion unit=1 minor=2 state=1"),
          messageIndex(*Result, "notify unit=1 old=4 new=1",
                       messageIndex(*Result, "notify unit=1 old=1 new=4")));
      ASSERT_TRUE(Result->Requests[1].Power);
      EXPECT_EQ(Result->Requests[1].Power->DeviceStateAfter,
                DevicePowerState::D0);
      EXPECT_EQ(Result->PnpDevices[0].DevicePower, DevicePowerState::D0);
    }
}

TEST(DriverWDMPower, InlineChildrenCompleteBeforePoRequestReturnsPending) {
  for (const auto *Image : powerImages()) {
    SCOPED_TRACE(Image);
    auto Options = powerOptions();
    Options.Requests.push_back(pnp(DevicePnpRequest::Start));
    systemCycle(Options);
    remove(Options);
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    const auto Scenario = requests(*Result, DriverRequestOrigin::Scenario);
    const auto Children =
        requests(*Result, DriverRequestOrigin::PoRequestPowerIrp);
    ASSERT_EQ(Scenario.size(), 5u);
    ASSERT_EQ(Children.size(), 2u);
    successfulChild(*Children[0], 0, 4);
    successfulChild(*Children[1], 1, 1);
    EXPECT_EQ(Children[0]->DispatchStatus, 0u);
    EXPECT_NE(Children[0]->IRP, Scenario[1]->IRP);
    EXPECT_NE(Children[1]->IRP, Scenario[2]->IRP);
    EXPECT_EQ(Scenario[1]->DispatchStatus, 0x103u);
    EXPECT_EQ(Scenario[2]->DispatchStatus, 0x103u);
    EXPECT_EQ(Children[0]->Power->RequestedDeviceObject,
              Result->PnpDevices[0].PDO);
    EXPECT_LT(messageIndex(*Result, "device-completion unit=1 minor=2 state=4"),
              messageIndex(*Result, "callback unit=1 minor=2 state=4"));
    EXPECT_LT(messageIndex(*Result, "callback unit=1 minor=2 state=4"),
              messageIndex(*Result, "request child returned pending"));
    EXPECT_EQ(countAPI(*Result, "PoRequestPowerIrp"), 2u);
  }
}

TEST(DriverWDMPower, DelayedSystemAndDevicePacketsKeepIndependentObservations) {
  for (const auto *Image : powerImages()) {
    SCOPED_TRACE(Image);
    auto Options = powerOptions();
    Options.Requests.push_back(pnp(DevicePnpRequest::Start));
    systemCycle(Options, 5, 11);
    remove(Options);
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    auto Scenario = requests(*Result, DriverRequestOrigin::Scenario);
    auto Children = requests(*Result, DriverRequestOrigin::PoRequestPowerIrp);
    ASSERT_EQ(Scenario.size(), 5u);
    ASSERT_EQ(Children.size(), 2u);
    for (size_t I = 0; I != 2; ++I) {
      successfulChild(*Children[I], I, I == 0 ? 4 : 1);
      ASSERT_TRUE(Scenario[I + 1]->Power);
      EXPECT_EQ(Scenario[I + 1]->Power->BusReceivedAt100ns, I * 16);
      EXPECT_EQ(Scenario[I + 1]->Power->BusCompletedAt100ns, I * 16 + 5);
      EXPECT_EQ(Children[I]->Power->BusReceivedAt100ns, I * 16 + 5);
      EXPECT_EQ(Children[I]->Power->BusCompletedAt100ns, (I + 1) * 16);
      EXPECT_EQ(Children[I]->DispatchStatus, 0x103u);
      EXPECT_NE(Children[I]->IRP, Scenario[I + 1]->IRP);
    }
  }
}

TEST(DriverWDMPower, FailedNestedQueryKeepsStatesAndAllowsSubsequentSystemSet) {
  for (const auto *Image : powerImages()) {
    SCOPED_TRACE(Image);
    auto Options = powerOptions();
    Options.Requests = {pnp(DevicePnpRequest::Start),
                        power(operation(DevicePowerRequest::Query,
                                        DriverPowerType::System, 4))};
    Options.PnpDevices[0].RequestedDevicePower.push_back(
        operation(DevicePowerRequest::Query, DriverPowerType::Device, 4, 7,
                  Failed, true));
    systemCycle(Options);
    remove(Options);
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    const auto Scenario = requests(*Result, DriverRequestOrigin::Scenario);
    const auto Children =
        requests(*Result, DriverRequestOrigin::PoRequestPowerIrp);
    ASSERT_EQ(Scenario.size(), 6u);
    ASSERT_EQ(Children.size(), 3u);
    EXPECT_EQ(Scenario[1]->IOStatus, Failed);
    ASSERT_TRUE(Scenario[1]->Power);
    EXPECT_EQ(Scenario[1]->Power->SystemStateAfter, SystemPowerState::Working);
    EXPECT_EQ(Children[0]->IOStatus, Failed);
    EXPECT_EQ(Children[0]->ResponseIndex, 0u);
    ASSERT_TRUE(Children[0]->Power);
    EXPECT_EQ(Children[0]->Power->Minor, DevicePowerRequest::Query);
    EXPECT_EQ(Children[0]->Power->BusStatus, Failed);
    EXPECT_EQ(Children[0]->Power->DeviceStateAfter, DevicePowerState::D0);
    successfulChild(*Children[1], 1, 4);
    successfulChild(*Children[2], 2, 1);
    EXPECT_LT(messageIndex(*Result,
                           "callback unit=1 minor=3 state=4 status=0xc0000001"),
              Result->Messages.size());
    EXPECT_EQ(countAPI(*Result, "PoSetPowerState"), 3u);
  }
}

TEST(DriverWDMPower, VistaS0CompletesBeforeDelayedD0ChildWithoutParentReuse) {
  for (const auto *Image : powerImages()) {
    SCOPED_TRACE(Image);
    auto Options = powerOptions('E');
    Options.Requests.push_back(pnp(DevicePnpRequest::Start));
    systemCycle(Options, 0, 13);
    remove(Options);
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    auto Children = requests(*Result, DriverRequestOrigin::PoRequestPowerIrp);
    ASSERT_EQ(Children.size(), 2u);
    successfulChild(*Children[1], 1, 1);
    EXPECT_EQ(Children[1]->Power->BusReceivedAt100ns, 13u);
    EXPECT_EQ(Children[1]->Power->BusCompletedAt100ns, 26u);
    const size_t Parent = messageIndex(*Result, "S0 completed before child");
    EXPECT_LT(Parent, messageIndex(*Result, "callback unit=1 minor=2 state=1"));
    EXPECT_LT(Parent, messageIndex(*Result, "D0 child observed completed S0"));
    EXPECT_EQ(Result->PnpDevices[0].SystemPower, SystemPowerState::Working);
    EXPECT_EQ(Result->PnpDevices[0].DevicePower, DevicePowerState::D0);
  }
}

TEST(DriverWDMPower, FiveArgumentCallbackSnapshotAndLocalStackSurviveWait) {
  for (const auto *Image : powerImages()) {
    SCOPED_TRACE(Image);
    auto Options = powerOptions('W');
    Options.Requests.push_back(pnp(DevicePnpRequest::Start));
    systemCycle(Options, 5, 11);
    remove(Options);
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    auto Children = requests(*Result, DriverRequestOrigin::PoRequestPowerIrp);
    ASSERT_EQ(Children.size(), 2u);
    successfulChild(*Children[0], 0, 4);
    successfulChild(*Children[1], 1, 1);
    EXPECT_EQ(Children[0]->Power->BusCompletedAt100ns, 16u);
    EXPECT_EQ(Children[1]->Power->BusReceivedAt100ns, 28u);
    EXPECT_EQ(Children[1]->Power->BusCompletedAt100ns, 39u);
    EXPECT_EQ(countAPI(*Result, "KeDelayExecutionThread"), 2u);
    const size_t First =
        messageIndex(*Result, "callback snapshot survived wait");
    EXPECT_LT(First, Result->Messages.size());
    EXPECT_LT(
        messageIndex(*Result, "callback snapshot survived wait", First + 1),
        Result->Messages.size());
  }
}

void workerScenario(DriverOptions &Options, uint64_t Delay) {
  Options.Requests.push_back(pnp(DevicePnpRequest::Start));
  for (auto Kind : {DriverRequestKind::Create, DriverRequestKind::DeviceControl,
                    DriverRequestKind::Cleanup, DriverRequestKind::Close}) {
    DriverRequest Request;
    Request.Kind = Kind;
    Request.File = 9;
    if (Kind == DriverRequestKind::Create)
      Request.DeviceID = "power0";
    if (Kind == DriverRequestKind::DeviceControl) {
      Request.ControlCode = 0x222000;
      Request.OutputSize = 4;
    }
    Options.Requests.push_back(Request);
  }
  Options.PnpDevices[0].RequestedDevicePower.push_back(
      operation(DevicePowerRequest::Set, DriverPowerType::Device, 4, Delay));
  remove(Options);
}

TEST(DriverWDMPower,
     WorkerRequestsDevicePowerWithoutSystemParentAndKeepsFdoTarget) {
  for (const auto *Image : powerImages()) {
    SCOPED_TRACE(Image);
    auto Options = powerOptions('I');
    workerScenario(Options, 17);
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    auto Scenario = requests(*Result, DriverRequestOrigin::Scenario);
    auto Children = requests(*Result, DriverRequestOrigin::PoRequestPowerIrp);
    ASSERT_EQ(Scenario.size(), 7u);
    ASSERT_EQ(Children.size(), 1u);
    successfulChild(*Children[0], 0, 4);
    EXPECT_NE(Children[0]->Power->RequestedDeviceObject,
              Result->PnpDevices[0].PDO);
    EXPECT_EQ(Children[0]->Power->Action, DriverPowerAction::None);
    EXPECT_EQ(Children[0]->Power->BusCompletedAt100ns, 17u);
    EXPECT_EQ(Scenario[2]->DispatchStatus, 0x103u);
    EXPECT_EQ(Scenario[2]->Output, (std::vector<uint8_t>{'P', 'W', 'R', '!'}));
    EXPECT_EQ(countAPI(*Result, "KeWaitForSingleObject"), 1u);
    EXPECT_EQ(Result->PnpDevices[0].SystemPower, SystemPowerState::Working);
  }
}

TEST(DriverWDMPower, NullPowerCallbackStillCreatesAndRetiresRealChild) {
  for (const auto *Image : powerImages()) {
    SCOPED_TRACE(Image);
    auto Options = powerOptions('N');
    workerScenario(Options, 0);
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    auto Children = requests(*Result, DriverRequestOrigin::PoRequestPowerIrp);
    ASSERT_EQ(Children.size(), 1u);
    successfulChild(*Children[0], 0, 4);
    EXPECT_EQ(Children[0]->DispatchStatus, 0u);
    EXPECT_LT(messageIndex(*Result, "null callback child completed"),
              Result->Messages.size());
    EXPECT_EQ(messageIndex(*Result, "callback unit="), Result->Messages.size());
    EXPECT_EQ(countAPI(*Result, "KeWaitForSingleObject"), 0u);
  }
}

TEST(DriverWDMPower, GuestQueryRejectionDoesNotPreventForcedSetPower) {
  for (const auto *Image : powerImages()) {
    SCOPED_TRACE(Image);
    auto Options = powerOptions('Q');
    Options.Requests = {
        pnp(DevicePnpRequest::Start),
        power(operation(DevicePowerRequest::Query, DriverPowerType::Device, 4)),
        power(operation(DevicePowerRequest::Set, DriverPowerType::Device, 4)),
        power(operation(DevicePowerRequest::Set, DriverPowerType::Device, 1))};
    remove(Options);
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    ASSERT_EQ(Result->Requests.size(), 6u);
    EXPECT_EQ(Result->Requests[1].IOStatus, Failed);
    ASSERT_TRUE(Result->Requests[1].Power);
    EXPECT_FALSE(Result->Requests[1].Power->BusStatus);
    EXPECT_EQ(Result->Requests[1].Power->DeviceStateAfter,
              DevicePowerState::D0);
    EXPECT_EQ(Result->Requests[2].IOStatus, 0u);
    EXPECT_EQ(Result->Requests[2].Power->DeviceStateAfter,
              DevicePowerState::D3);
    EXPECT_EQ(countAPI(*Result, "PoSetPowerState"), 3u);
  }
}

TEST(DriverWDMPower, TwoPdosKeepNotificationSeedsAndResponseFifosIndependent) {
  for (const auto *Image : powerImages()) {
    SCOPED_TRACE(Image);
    auto Options = powerOptions('T');
    auto Second = Options.PnpDevices[0];
    Second.ID = "power1";
    Second.InitialReportedDevicePower = DevicePowerState::D0;
    Options.PnpDevices.push_back(Second);
    Options.Requests = {pnp(DevicePnpRequest::Start),
                        pnp(DevicePnpRequest::Start, "power1")};
    systemCycle(Options, 0, 7, 0);
    systemCycle(Options, 0, 11, 1);
    remove(Options);
    remove(Options, "power1");
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result, 2);
    auto Children = requests(*Result, DriverRequestOrigin::PoRequestPowerIrp);
    ASSERT_EQ(Children.size(), 4u);
    for (size_t I = 0; I != 4; ++I) {
      successfulChild(*Children[I], I % 2, I % 2 == 0 ? 4 : 1,
                      I < 2 ? "power0" : "power1");
      EXPECT_EQ(Children[I]->Power->RequestedDeviceObject,
                Result->PnpDevices[I / 2].PDO);
    }
    EXPECT_EQ(Children[1]->Power->BusCompletedAt100ns, 14u);
    EXPECT_EQ(Children[2]->Power->BusReceivedAt100ns, 14u);
    EXPECT_EQ(Children[3]->Power->BusCompletedAt100ns, 36u);
    EXPECT_LT(messageIndex(*Result, "notify unit=1 old=4 new=1"),
              Result->Messages.size());
    EXPECT_LT(messageIndex(*Result, "notify unit=2 old=1 new=1"),
              Result->Messages.size());
    EXPECT_EQ(countAPI(*Result, "PoSetPowerState"), 6u);
  }
}

TEST(DriverWDMPower, MissingNotificationSeedStopsAtRealPoSetWithoutGuessing) {
  for (const auto *Image : powerImages()) {
    SCOPED_TRACE(Image);
    auto Options = powerOptions();
    Options.PnpDevices[0].InitialReportedDevicePower.reset();
    Options.Requests.push_back(pnp(DevicePnpRequest::Start));
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
    EXPECT_FALSE(Result->UnloadCompleted);
    EXPECT_NE(Result->Diagnostic.find("initial_reported_device_power"),
              std::string::npos)
        << Result->Diagnostic;
  }
}

TEST(DriverWDMPower, MissingOrMismatchedChildResponseNeverInventsCompletion) {
  for (const auto *Image : powerImages())
    for (bool Mismatch : {false, true}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Mismatch);
      auto Options = powerOptions();
      Options.Requests = {pnp(DevicePnpRequest::Start),
                          power(operation(DevicePowerRequest::Set,
                                          DriverPowerType::System, 4))};
      if (Mismatch)
        Options.PnpDevices[0].RequestedDevicePower.push_back(operation(
            DevicePowerRequest::Set, DriverPowerType::Device, 1, 0, 0, true));
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
      EXPECT_FALSE(Result->UnloadCompleted);
      EXPECT_TRUE(
          requests(*Result, DriverRequestOrigin::PoRequestPowerIrp).empty());
      EXPECT_EQ(messageIndex(*Result, "callback unit="),
                Result->Messages.size());
      EXPECT_EQ(messageIndex(*Result, "request child returned pending"),
                Result->Messages.size());
    }
}

#else
TEST(DriverWDMPower, GenuineWdkFixtureNotConfigured) {
  GTEST_SKIP()
      << "Set NEVERD_WDM_POWER_FIXTURE to an original genuine-WDK power "
         "driver image; optional NEVERD_WDM_POWER_CFG_FIXTURE also "
         "executes its active CFG variant.";
}
#endif
} // namespace
} // namespace neverd::emulation
