//===- DeviceLifecycleTests.cpp - PnP and power contracts ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// PnP transition, power completion and independent removal-lifetime evidence.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "windows/DeviceLifecycle.h"

#include <array>

namespace neverd::emulation {
namespace {

constexpr uint64_t FirstDevice = 0x1000;
constexpr uint64_t SecondDevice = 0x2000;
constexpr uint32_t Success = 0;
constexpr uint32_t Failure = 0xc0000001;
constexpr uint32_t DeletePending = 0xc0000056;
constexpr uint32_t Pending = 0x103;

class DriverDeviceLifecycle : public ::testing::Test {
protected:
  DeviceLifecycle Model;

  void success(llvm::Error E) {
    if (E)
      ADD_FAILURE() << llvm::toString(std::move(E));
  }

  void failure(llvm::Error E, llvm::StringRef Message) {
    ASSERT_TRUE(bool(E));
    EXPECT_NE(llvm::toString(std::move(E)).find(Message.str()),
              std::string::npos);
  }

  template <class T>
  void failure(llvm::Expected<T> Value, llvm::StringRef Message) {
    ASSERT_FALSE(bool(Value));
    failure(Value.takeError(), Message);
  }

  template <class T> T value(llvm::Expected<T> Value) {
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return T{};
    }
    return *Value;
  }

  void SetUp() override {
    success(Model.addDevice(FirstDevice, DevicePowerState::D0,
                            SystemPowerState::Working));
    success(Model.addDevice(SecondDevice, DevicePowerState::D0,
                            SystemPowerState::Working));
  }

  DeviceLifecycleSnapshot state(uint64_t Device = FirstDevice) {
    return value(Model.snapshot(Device));
  }

  DeviceLifecycleTicket begin(DevicePnpRequest Request,
                              uint64_t Device = FirstDevice) {
    return value(Model.beginPnp(Device, Request));
  }

  void pnp(DevicePnpRequest Request, uint32_t Status = Success,
           uint64_t Device = FirstDevice) {
    success(Model.finishPnp(begin(Request, Device), Status));
  }

  void start(uint64_t Device = FirstDevice) {
    pnp(DevicePnpRequest::Start, Success, Device);
  }

  DeviceLifecycleTicket devicePower(DevicePowerRequest Request,
                                    DevicePowerState Target) {
    return value(Model.beginDevicePower(FirstDevice, Request, Target));
  }

  DeviceLifecycleTicket systemPower(DevicePowerRequest Request,
                                    SystemPowerState Target) {
    return value(Model.beginSystemPower(FirstDevice, Request, Target));
  }
};

TEST_F(DriverDeviceLifecycle, PowerRequestCodesMatchWindowsMinorFunctions) {
  EXPECT_EQ(static_cast<uint8_t>(DevicePowerRequest::Set), 0x02);
  EXPECT_EQ(static_cast<uint8_t>(DevicePowerRequest::Query), 0x03);
}

TEST_F(DriverDeviceLifecycle,
       StartFailureAndQueriesRollbackWithoutChoosingIoPolicy) {
  success(Model.trackIo(FirstDevice, 1));
  success(Model.finishIo(FirstDevice, 1));
  const auto FailedStart = begin(DevicePnpRequest::Start);
  failure(Model.finishPnp(FailedStart, Pending), "not a final");
  EXPECT_EQ(state().PnpOperation, FailedStart);
  success(Model.finishPnp(FailedStart, Failure));
  EXPECT_EQ(state().Pnp, DevicePnpState::NotStarted);
  EXPECT_FALSE(state().PnpOperation);
  start();
  const auto QueryStop = begin(DevicePnpRequest::QueryStop);
  EXPECT_EQ(state().Pnp, DevicePnpState::StopPending);
  success(Model.trackIo(FirstDevice, 1));
  success(Model.finishIo(FirstDevice, 1));
  success(Model.finishPnp(QueryStop, Failure));
  EXPECT_EQ(state().Pnp, DevicePnpState::Started);
  pnp(DevicePnpRequest::QueryRemove, Failure);
  EXPECT_EQ(state().Pnp, DevicePnpState::Started);
}

TEST_F(DriverDeviceLifecycle, RebalanceCancelsStopsAndRestarts) {
  start();
  pnp(DevicePnpRequest::QueryStop);
  pnp(DevicePnpRequest::CancelStop);
  EXPECT_EQ(state().Pnp, DevicePnpState::Started);
  // Spurious cancels after an upper driver's failed query must succeed.
  pnp(DevicePnpRequest::CancelStop);
  pnp(DevicePnpRequest::CancelRemove);
  pnp(DevicePnpRequest::QueryStop);
  pnp(DevicePnpRequest::Stop);
  EXPECT_EQ(state().Pnp, DevicePnpState::Stopped);
  pnp(DevicePnpRequest::Start, Failure);
  EXPECT_EQ(state().Pnp, DevicePnpState::Stopped);
  start();
  EXPECT_EQ(state().Pnp, DevicePnpState::Started);
}

TEST_F(DriverDeviceLifecycle, RemoveCancellationRestoresStoppedAndUnstarted) {
  pnp(DevicePnpRequest::QueryRemove);
  pnp(DevicePnpRequest::CancelRemove);
  EXPECT_EQ(state().Pnp, DevicePnpState::NotStarted);
  start();
  pnp(DevicePnpRequest::QueryStop);
  pnp(DevicePnpRequest::Stop);
  pnp(DevicePnpRequest::QueryRemove, Failure);
  EXPECT_EQ(state().Pnp, DevicePnpState::Stopped);
  pnp(DevicePnpRequest::QueryRemove);
  EXPECT_EQ(state().Pnp, DevicePnpState::RemovePending);
  pnp(DevicePnpRequest::CancelRemove);
  EXPECT_EQ(state().Pnp, DevicePnpState::Stopped);
}

TEST_F(DriverDeviceLifecycle, BootConfiguredDevicesCanRebalanceBeforeStart) {
  constexpr uint64_t BootDevice = 0x3000;
  success(Model.addDevice(BootDevice, DevicePowerState::D0,
                          SystemPowerState::Working, true));
  failure(Model.beginPnp(FirstDevice, DevicePnpRequest::QueryStop),
          "current state");
  pnp(DevicePnpRequest::QueryStop, Success, BootDevice);
  pnp(DevicePnpRequest::CancelStop, Success, BootDevice);
  EXPECT_EQ(state(BootDevice).Pnp, DevicePnpState::NotStarted);
  pnp(DevicePnpRequest::QueryStop, Success, BootDevice);
  pnp(DevicePnpRequest::Stop, Success, BootDevice);
  EXPECT_EQ(state(BootDevice).Pnp, DevicePnpState::Stopped);
  start(BootDevice);
}

TEST_F(DriverDeviceLifecycle,
       MandatorySuccessFailureDoesNotCommitTheOperation) {
  start();
  pnp(DevicePnpRequest::QueryStop);
  const auto Cancel = begin(DevicePnpRequest::CancelStop);
  failure(Model.finishPnp(Cancel, Failure), "must not fail");
  EXPECT_EQ(state().Pnp, DevicePnpState::StopPending);
  EXPECT_EQ(state().PnpOperation, Cancel);
  success(Model.finishPnp(Cancel, Success));
  pnp(DevicePnpRequest::QueryStop);
  const auto Stop = begin(DevicePnpRequest::Stop);
  failure(Model.finishPnp(Stop, Failure), "must not fail");
  EXPECT_EQ(state().Pnp, DevicePnpState::StopPending);
  success(Model.finishPnp(Stop, Success));
  const auto Remove = begin(DevicePnpRequest::Remove);
  failure(Model.finishPnp(Remove, Failure), "must not fail");
  EXPECT_EQ(state().Pnp, DevicePnpState::Removing);
  success(Model.finishPnp(Remove, Success));
  EXPECT_EQ(state().Pnp, DevicePnpState::Removed);
}

TEST_F(DriverDeviceLifecycle, RemoveAndCancelRequireExactSuccess) {
  for (auto Minor :
       {DevicePnpRequest::CancelRemove, DevicePnpRequest::Remove}) {
    const auto Ticket = begin(Minor);
    const auto Before = state().Pnp;
    failure(Model.validatePnpCompletion(Ticket, 1), "STATUS_SUCCESS");
    failure(Model.finishPnp(Ticket, 0x40000000), "STATUS_SUCCESS");
    EXPECT_EQ(state().Pnp, Before);
    EXPECT_EQ(state().PnpOperation, Ticket);
    success(Model.finishPnp(Ticket, Success));
  }
  EXPECT_EQ(state().Pnp, DevicePnpState::Removed);
}

TEST_F(DriverDeviceLifecycle, StopCancelAndSurpriseRequireExactSuccess) {
  start();
  pnp(DevicePnpRequest::QueryStop);
  for (auto Minor : {DevicePnpRequest::CancelStop, DevicePnpRequest::Stop,
                     DevicePnpRequest::SurpriseRemoval}) {
    const auto Ticket = begin(Minor);
    const auto Before = state().Pnp;
    failure(Model.validatePnpCompletion(Ticket, 1), "STATUS_SUCCESS");
    failure(Model.finishPnp(Ticket, Failure), "must not fail");
    EXPECT_EQ(state().Pnp, Before);
    EXPECT_EQ(state().PnpOperation, Ticket);
    success(Model.finishPnp(Ticket, Success));
    if (Minor == DevicePnpRequest::CancelStop)
      pnp(DevicePnpRequest::QueryStop);
  }
  EXPECT_EQ(state().Pnp, DevicePnpState::SurpriseRemoved);
}

TEST_F(DriverDeviceLifecycle, ResourceRequeryStatusKeepsQueryStopUncommitted) {
  start();
  const auto Query = begin(DevicePnpRequest::QueryStop);
  failure(Model.validatePnpCompletion(Query, 0x119), "resource requery");
  failure(Model.finishPnp(Query, 0x119), "resource requery");
  EXPECT_EQ(state().Pnp, DevicePnpState::StopPending);
  EXPECT_EQ(state().PnpOperation, Query);
  success(Model.finishPnp(Query, Failure));
  EXPECT_EQ(state().Pnp, DevicePnpState::Started);
}

TEST_F(DriverDeviceLifecycle,
       IoLifetimeSurvivesStopRestartAndPowerTransactions) {
  // Dispatch acceptance does not infer whether an IRP touches hardware.
  success(Model.trackIo(FirstDevice, 1));
  start();
  pnp(DevicePnpRequest::QueryStop);
  success(Model.trackIo(FirstDevice, 2));
  pnp(DevicePnpRequest::Stop);
  EXPECT_EQ(state().OutstandingIo, 2u);
  success(Model.trackIo(FirstDevice, 3));
  success(Model.finishIo(FirstDevice, 1));
  start();
  success(Model.finishIo(FirstDevice, 2));
  success(Model.finishIo(FirstDevice, 3));
  auto Query = devicePower(DevicePowerRequest::Query, DevicePowerState::D3);
  success(Model.trackIo(FirstDevice, 4));
  success(Model.finishDevicePower(Query, Success));
  success(Model.trackIo(FirstDevice, 5));
  auto Set = devicePower(DevicePowerRequest::Set, DevicePowerState::D3);
  success(Model.finishDevicePower(Set, Success));
  success(Model.trackIo(FirstDevice, 6));
  auto Sleep =
      systemPower(DevicePowerRequest::Set, SystemPowerState::Sleeping3);
  success(Model.finishSystemPower(Sleep, Success));
  success(Model.trackIo(FirstDevice, 7));
  for (uint64_t IRP : {4, 5, 6, 7})
    success(Model.finishIo(FirstDevice, IRP));
  EXPECT_EQ(state().OutstandingIo, 0u);
  EXPECT_EQ(state().DevicePower, DevicePowerState::D3);
  EXPECT_EQ(state().SystemPower, SystemPowerState::Sleeping3);
}

TEST_F(DriverDeviceLifecycle, PnpTransitionGridRejectsOnlyInvalidSequences) {
  static constexpr std::array Requests = {
      DevicePnpRequest::Start,      DevicePnpRequest::QueryRemove,
      DevicePnpRequest::Remove,     DevicePnpRequest::CancelRemove,
      DevicePnpRequest::Stop,       DevicePnpRequest::QueryStop,
      DevicePnpRequest::CancelStop, DevicePnpRequest::SurpriseRemoval};
  struct Row {
    DevicePnpState State;
    // Independent expected table in request-code order above.
    std::array<bool, Requests.size()> Allowed;
  };
  constexpr std::array Rows = {
      Row{DevicePnpState::NotStarted,
          {true, true, true, true, false, false, false, true}},
      Row{DevicePnpState::Started,
          {true, true, true, true, false, true, true, true}},
      Row{DevicePnpState::StopPending,
          {false, false, true, false, true, false, true, true}},
      Row{DevicePnpState::Stopped,
          {true, true, true, true, false, false, false, true}},
      Row{DevicePnpState::RemovePending,
          {false, false, true, true, false, false, false, true}},
      Row{DevicePnpState::SurpriseRemoved,
          {false, false, true, false, false, false, false, false}},
      Row{DevicePnpState::Removed,
          {false, false, false, false, false, false, false, false}}};
  for (const auto &Row : Rows) {
    for (size_t I = 0; I < Requests.size(); ++I) {
      SCOPED_TRACE(static_cast<unsigned>(Row.State));
      SCOPED_TRACE(static_cast<unsigned>(Requests[I]));
      DeviceLifecycle Case;
      success(Case.addDevice(FirstDevice, DevicePowerState::D0,
                             SystemPowerState::Working));
      auto Apply = [&](DevicePnpRequest Request) {
        success(Case.finishPnp(value(Case.beginPnp(FirstDevice, Request)), 0));
      };
      if (Row.State == DevicePnpState::Started ||
          Row.State == DevicePnpState::StopPending ||
          Row.State == DevicePnpState::Stopped)
        Apply(DevicePnpRequest::Start);
      if (Row.State == DevicePnpState::StopPending ||
          Row.State == DevicePnpState::Stopped)
        Apply(DevicePnpRequest::QueryStop);
      if (Row.State == DevicePnpState::Stopped)
        Apply(DevicePnpRequest::Stop);
      if (Row.State == DevicePnpState::RemovePending)
        Apply(DevicePnpRequest::QueryRemove);
      if (Row.State == DevicePnpState::SurpriseRemoved)
        Apply(DevicePnpRequest::SurpriseRemoval);
      if (Row.State == DevicePnpState::Removed)
        Apply(DevicePnpRequest::Remove);
      ASSERT_EQ(value(Case.snapshot(FirstDevice)).Pnp, Row.State);
      auto Ticket = Case.beginPnp(FirstDevice, Requests[I]);
      EXPECT_EQ(bool(Ticket), Row.Allowed[I]);
      if (!Ticket) {
        llvm::consumeError(Ticket.takeError());
        EXPECT_EQ(value(Case.snapshot(FirstDevice)).Pnp, Row.State);
      }
    }
  }
}

TEST_F(DriverDeviceLifecycle, TicketsCannotCrossDevicesDomainsOrCompletions) {
  const auto First = begin(DevicePnpRequest::Start);
  const auto Second = begin(DevicePnpRequest::Start, SecondDevice);
  failure(Model.beginPnp(FirstDevice, DevicePnpRequest::Remove),
          "already active");
  failure(Model.finishPnp({SecondDevice, First.Sequence}, 0), "mismatched");
  failure(Model.finishDevicePower(First, 0), "mismatched");
  success(Model.finishPnp(Second, 0));
  success(Model.finishPnp(First, 0));
  failure(Model.finishPnp(First, 0), "stale");
  const auto Query =
      devicePower(DevicePowerRequest::Query, DevicePowerState::D3);
  failure(Model.finishPnp(Query, 0), "mismatched");
  failure(Model.finishSystemPower(Query, 0), "mismatched");
  success(Model.finishDevicePower(Query, Failure));
}

TEST_F(DriverDeviceLifecycle, SurpriseRemovalPreservesOutstandingIo) {
  start();
  constexpr uint64_t Irp = 0x8000;
  success(Model.trackIo(FirstDevice, Irp));
  const auto Surprise = begin(DevicePnpRequest::SurpriseRemoval);
  EXPECT_EQ(state().Pnp, DevicePnpState::SurpriseRemoved);
  EXPECT_EQ(state().OutstandingIo, 1u);
  success(Model.trackIo(FirstDevice, Irp + 1));
  success(Model.finishIo(FirstDevice, Irp + 1));
  failure(Model.finishPnp(Surprise, Failure), "must not fail");
  success(Model.finishPnp(Surprise, Success));
  const auto Remove = begin(DevicePnpRequest::Remove);
  failure(Model.trackIo(FirstDevice, Irp + 1), "after REMOVE begins");
  failure(Model.finishPnp(Remove, Success), "outstanding operations");
  success(Model.finishIo(FirstDevice, Irp));
  EXPECT_EQ(state().OutstandingIo, 0u);
  success(Model.finishPnp(Remove, Success));
  EXPECT_EQ(state().Pnp, DevicePnpState::Removed);
  failure(Model.trackIo(FirstDevice, Irp + 1), "after REMOVE begins");
  failure(Model.addDevice(FirstDevice, DevicePowerState::D0,
                          SystemPowerState::Working),
          "already exists");
}

TEST_F(DriverDeviceLifecycle, DeviceAndIrpIdentityFailuresAreAtomic) {
  failure(Model.addDevice(0, DevicePowerState::D0, SystemPowerState::Working),
          "nonzero");
  failure(Model.snapshot(0x9000), "unknown device");
  failure(Model.beginPnp(FirstDevice, static_cast<DevicePnpRequest>(255)),
          "unsupported");
  failure(Model.addDevice(0x9000, static_cast<DevicePowerState>(0),
                          SystemPowerState::Working),
          "invalid initial");
  failure(Model.addDevice(0x9000, DevicePowerState::D0,
                          static_cast<SystemPowerState>(7)),
          "invalid initial");
  start();
  start(SecondDevice);
  success(Model.trackIo(FirstDevice, 0x5000));
  failure(Model.trackIo(SecondDevice, 0x5000), "already has");
  failure(Model.finishIo(SecondDevice, 0x5000), "not outstanding");
  failure(Model.trackIo(FirstDevice, 0), "nonzero");
  EXPECT_EQ(state().OutstandingIo, 1u);
  EXPECT_EQ(state(SecondDevice).OutstandingIo, 0u);
  success(Model.finishIo(FirstDevice, 0x5000));
  success(Model.trackIo(SecondDevice, 0x5000));
}

TEST_F(DriverDeviceLifecycle, StartingDoesNotInventHardwarePowerTransitions) {
  constexpr uint64_t SleepingDevice = 0x3000;
  success(Model.addDevice(SleepingDevice, DevicePowerState::D3,
                          SystemPowerState::Working));
  start(SleepingDevice);
  EXPECT_EQ(state(SleepingDevice).DevicePower, DevicePowerState::D3);
  const auto Power = value(Model.beginDevicePower(
      SleepingDevice, DevicePowerRequest::Set, DevicePowerState::D0));
  EXPECT_EQ(state(SleepingDevice).DevicePower, DevicePowerState::D3);
  success(Model.finishDevicePower(Power, Success));
}

TEST_F(DriverDeviceLifecycle, DevicePowerQueriesAndIdleTransitions) {
  start();
  auto Query = devicePower(DevicePowerRequest::Query, DevicePowerState::D3);
  EXPECT_EQ(state().DevicePower, DevicePowerState::D0);
  success(Model.finishDevicePower(Query, Failure));
  Query = devicePower(DevicePowerRequest::Query, DevicePowerState::D3);
  success(Model.finishDevicePower(Query, Success));
  EXPECT_TRUE(state().DevicePowerQueryAccepted);
  // A same-state set releases a query; a prior query is never required to set.
  auto Set = devicePower(DevicePowerRequest::Set, DevicePowerState::D0);
  success(Model.finishDevicePower(Set, Success));
  for (DevicePowerState Target : {DevicePowerState::D1, DevicePowerState::D2,
                                  DevicePowerState::D3, DevicePowerState::D0}) {
    Set = devicePower(DevicePowerRequest::Set, Target);
    success(Model.finishDevicePower(Set, Success));
    EXPECT_EQ(state().DevicePower, Target);
  }
}

TEST_F(DriverDeviceLifecycle,
       SystemPowerNestsDevicePowerWithoutImplicitMapping) {
  start();
  auto System =
      systemPower(DevicePowerRequest::Query, SystemPowerState::Sleeping3);
  failure(Model.beginSystemPower(FirstDevice, DevicePowerRequest::Set,
                                 SystemPowerState::Sleeping3),
          "already active");
  failure(Model.beginDevicePower(FirstDevice, DevicePowerRequest::Set,
                                 DevicePowerState::D3),
          "cannot satisfy a system query");
  auto Device = devicePower(DevicePowerRequest::Query, DevicePowerState::D3);
  failure(Model.finishSystemPower(System, Success), "device power operation");
  success(Model.finishDevicePower(Device, Success));
  success(Model.finishSystemPower(System, Success));
  EXPECT_EQ(state().DevicePower, DevicePowerState::D0);
  EXPECT_EQ(state().SystemPower, SystemPowerState::Working);
  System = systemPower(DevicePowerRequest::Set, SystemPowerState::Sleeping3);
  Device = devicePower(DevicePowerRequest::Set, DevicePowerState::D2);
  success(Model.finishDevicePower(Device, Success));
  success(Model.finishSystemPower(System, Success));
  EXPECT_EQ(state().DevicePower, DevicePowerState::D2);
  EXPECT_EQ(state().SystemPower, SystemPowerState::Sleeping3);
  System = systemPower(DevicePowerRequest::Set, SystemPowerState::Working);
  Device = devicePower(DevicePowerRequest::Set, DevicePowerState::D0);
  success(Model.finishDevicePower(Device, Success));
  success(Model.finishSystemPower(System, Success));
}

TEST_F(DriverDeviceLifecycle, SystemResumeCanFinishBeforeDeviceD0Completes) {
  start();
  auto Device = devicePower(DevicePowerRequest::Set, DevicePowerState::D3);
  success(Model.finishDevicePower(Device, Success));
  auto System =
      systemPower(DevicePowerRequest::Set, SystemPowerState::Sleeping3);
  success(Model.finishSystemPower(System, Success));
  System = systemPower(DevicePowerRequest::Set, SystemPowerState::Working);
  Device = devicePower(DevicePowerRequest::Set, DevicePowerState::D0);
  success(Model.validateSystemPowerCompletion(System, Success));
  EXPECT_EQ(state().SystemPower, SystemPowerState::Sleeping3);
  EXPECT_EQ(state().DevicePowerOperation, Device);
  success(Model.finishSystemPower(System, Success));
  EXPECT_EQ(state().SystemPower, SystemPowerState::Working);
  EXPECT_EQ(state().DevicePower, DevicePowerState::D3);
  EXPECT_FALSE(state().SystemPowerOperation);
  EXPECT_EQ(state().DevicePowerOperation, Device);
  success(Model.validateDevicePowerCompletion(Device, Success));
  EXPECT_EQ(state().DevicePower, DevicePowerState::D3);
  success(Model.finishDevicePower(Device, Success));
  EXPECT_EQ(state().DevicePower, DevicePowerState::D0);
}

TEST_F(DriverDeviceLifecycle,
       PowerDownAndQueryPreflightRetainBothTransactions) {
  start();
  for (auto Minor : {DevicePowerRequest::Query, DevicePowerRequest::Set}) {
    const auto System = systemPower(Minor, SystemPowerState::Sleeping3);
    const auto Device = devicePower(Minor, DevicePowerState::D3);
    failure(Model.validateSystemPowerCompletion(System, Success),
            "device power operation");
    EXPECT_EQ(state().SystemPowerOperation, System);
    EXPECT_EQ(state().DevicePowerOperation, Device);
    EXPECT_EQ(state().SystemPower, SystemPowerState::Working);
    EXPECT_EQ(state().DevicePower, DevicePowerState::D0);
    const auto Status = Minor == DevicePowerRequest::Query ? Failure : Success;
    success(Model.finishDevicePower(Device, Status));
    success(Model.finishSystemPower(System, Status));
  }
  EXPECT_EQ(state().SystemPower, SystemPowerState::Sleeping3);
  EXPECT_EQ(state().DevicePower, DevicePowerState::D3);
}

TEST_F(DriverDeviceLifecycle, FailedSystemQueryAllowsAnySubsequentSet) {
  start();
  for (SystemPowerState Target :
       {SystemPowerState::Working, SystemPowerState::Sleeping1,
        SystemPowerState::Sleeping2, SystemPowerState::Sleeping3,
        SystemPowerState::Hibernate, SystemPowerState::Shutdown}) {
    auto Query =
        systemPower(DevicePowerRequest::Query, SystemPowerState::Sleeping3);
    success(Model.finishSystemPower(Query, Failure));
    const auto Set = systemPower(DevicePowerRequest::Set, Target);
    success(Model.finishSystemPower(Set, Success));
    EXPECT_EQ(state().SystemPower, Target);
    EXPECT_EQ(state().DevicePower, DevicePowerState::D0);
  }
}

TEST_F(DriverDeviceLifecycle, PowerFailuresDoNotCommitInvalidTransitions) {
  start();
  auto Device = devicePower(DevicePowerRequest::Set, DevicePowerState::D3);
  failure(Model.finishDevicePower(Device, Pending), "not a final");
  failure(Model.finishDevicePower(Device, Failure), "must not fail");
  EXPECT_EQ(state().DevicePower, DevicePowerState::D0);
  EXPECT_EQ(state().DevicePowerOperation, Device);
  success(Model.finishDevicePower(Device, Success));
  auto System =
      systemPower(DevicePowerRequest::Set, SystemPowerState::Hibernate);
  failure(Model.finishSystemPower(System, Failure), "must not fail");
  EXPECT_EQ(state().SystemPower, SystemPowerState::Working);
  success(Model.finishSystemPower(System, Success));
  failure(Model.beginSystemPower(FirstDevice, DevicePowerRequest::Query,
                                 SystemPowerState::Working),
          "does not use a query");
  failure(Model.beginDevicePower(FirstDevice,
                                 static_cast<DevicePowerRequest>(0),
                                 DevicePowerState::D0),
          "unsupported");
  failure(Model.beginDevicePower(FirstDevice, DevicePowerRequest::Set,
                                 static_cast<DevicePowerState>(5)),
          "unsupported");
}

TEST_F(DriverDeviceLifecycle,
       RemovalWaitsForPowerAndCannotBeReversedByCompletion) {
  start();
  const auto System =
      systemPower(DevicePowerRequest::Set, SystemPowerState::Sleeping3);
  const auto Device =
      devicePower(DevicePowerRequest::Set, DevicePowerState::D3);
  pnp(DevicePnpRequest::SurpriseRemoval);
  EXPECT_EQ(state().DevicePower, DevicePowerState::D0);
  failure(Model.finishDevicePower(Device, Success), "cannot revive");
  failure(Model.beginDevicePower(FirstDevice, DevicePowerRequest::Set,
                                 DevicePowerState::D0),
          "being removed");
  const auto Remove = begin(DevicePnpRequest::Remove);
  failure(Model.finishPnp(Remove, Success), "outstanding operations");
  success(Model.finishDevicePower(Device, DeletePending));
  failure(Model.finishPnp(Remove, Success), "outstanding operations");
  success(Model.finishSystemPower(System, DeletePending));
  success(Model.finishPnp(Remove, Success));
  EXPECT_EQ(state().Pnp, DevicePnpState::Removed);
  EXPECT_EQ(state().SystemPower, SystemPowerState::Working);
  EXPECT_EQ(state().DevicePower, DevicePowerState::D0);
}

TEST_F(DriverDeviceLifecycle,
       BusPowerUpFailureDuringQueryRemoveKeepsPowerState) {
  start();
  auto Power = devicePower(DevicePowerRequest::Set, DevicePowerState::D3);
  success(Model.finishDevicePower(Power, Success));
  pnp(DevicePnpRequest::QueryRemove);
  Power = devicePower(DevicePowerRequest::Set, DevicePowerState::D0);
  success(Model.finishDevicePower(Power, DeletePending));
  EXPECT_EQ(state().DevicePower, DevicePowerState::D3);
  EXPECT_EQ(state().Pnp, DevicePnpState::RemovePending);
  EXPECT_FALSE(state().DevicePowerOperation);
  pnp(DevicePnpRequest::CancelRemove);
  Power = devicePower(DevicePowerRequest::Set, DevicePowerState::D0);
  success(Model.finishDevicePower(Power, Success));
}

TEST_F(DriverDeviceLifecycle, StartedDeviceCanRestartWithChangedResources) {
  start();
  // PNP_RESOURCE_REQUIREMENTS_CHANGED can trigger START without a STOP.
  const auto Restart = begin(DevicePnpRequest::Start);
  success(Model.finishPnp(Restart, Success));
  EXPECT_EQ(state().Pnp, DevicePnpState::Started);
  pnp(DevicePnpRequest::Start, Failure);
  EXPECT_EQ(state().Pnp, DevicePnpState::Started);
  // Informational success is distinct from pending and failure.
  pnp(DevicePnpRequest::QueryStop, 0x104);
  EXPECT_EQ(state().Pnp, DevicePnpState::StopPending);
  pnp(DevicePnpRequest::CancelStop);
}

TEST_F(DriverDeviceLifecycle, ExplicitDeviceLimitLeavesExistingObjectsIntact) {
  constexpr size_t Limit = 4096;
  DeviceLifecycle Many;
  for (size_t I = 1; I <= Limit; ++I)
    success(Many.addDevice(I, DevicePowerState::D0, SystemPowerState::Working));
  failure(Many.addDevice(Limit + 1, DevicePowerState::D0,
                         SystemPowerState::Working),
          "count exceeds");
  EXPECT_EQ(value(Many.snapshot(Limit)).Device, Limit);
  failure(Many.snapshot(Limit + 1), "unknown device");
}

} // namespace
} // namespace neverd::emulation
