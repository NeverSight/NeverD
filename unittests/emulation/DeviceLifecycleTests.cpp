//===- DeviceLifecycleTests.cpp - PnP, power and removal contracts
//---------===//
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

TEST_F(DriverDeviceLifecycle, StartFailureAndQueriesRollbackWithoutStartingIo) {
  EXPECT_FALSE(state().CanStartIo);
  failure(Model.beginIo(FirstDevice, 1), "not ready");
  const auto FailedStart = begin(DevicePnpRequest::Start);
  failure(Model.finishPnp(FailedStart, Pending), "not a final");
  EXPECT_EQ(state().PnpOperation, FailedStart);
  success(Model.finishPnp(FailedStart, Failure));
  EXPECT_EQ(state().Pnp, DevicePnpState::NotStarted);
  EXPECT_FALSE(state().PnpOperation);
  start();
  EXPECT_TRUE(state().CanStartIo);
  const auto QueryStop = begin(DevicePnpRequest::QueryStop);
  EXPECT_EQ(state().Pnp, DevicePnpState::StopPending);
  failure(Model.beginIo(FirstDevice, 1), "not ready");
  success(Model.finishPnp(QueryStop, Failure));
  EXPECT_EQ(state().Pnp, DevicePnpState::Started);
  EXPECT_TRUE(state().CanStartIo);
  pnp(DevicePnpRequest::QueryRemove, Failure);
  EXPECT_EQ(state().Pnp, DevicePnpState::Started);
  EXPECT_TRUE(state().CanStartIo);
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
  EXPECT_FALSE(state().CanStartIo);
  pnp(DevicePnpRequest::Start, Failure);
  EXPECT_EQ(state().Pnp, DevicePnpState::Stopped);
  start();
  EXPECT_EQ(state().Pnp, DevicePnpState::Started);
  EXPECT_TRUE(state().CanStartIo);
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
  EXPECT_FALSE(state().CanStartIo);
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
  EXPECT_TRUE(state(BootDevice).CanStartIo);
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
  for (auto Minor : {DevicePnpRequest::CancelRemove, DevicePnpRequest::Remove}) {
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

TEST_F(DriverDeviceLifecycle, SurpriseRemovalDoesNotDiscardIoOrLockOwnership) {
  start();
  constexpr uint64_t Lock = 0x7000;
  constexpr uint64_t Irp = 0x8000;
  success(Model.initializeRemoveLock(FirstDevice, Lock));
  success(Model.acquireRemoveLock(FirstDevice, Lock, Irp));
  success(Model.beginIo(FirstDevice, Irp));
  const auto Surprise = begin(DevicePnpRequest::SurpriseRemoval);
  EXPECT_EQ(state().Pnp, DevicePnpState::SurpriseRemoved);
  EXPECT_EQ(state().OutstandingIo, 1u);
  EXPECT_EQ(state().RemoveLockReferences, 1u);
  failure(Model.beginIo(FirstDevice, Irp + 1), "not ready");
  failure(Model.finishPnp(Surprise, Failure), "must not fail");
  success(Model.finishPnp(Surprise, Success));
  const auto Remove = begin(DevicePnpRequest::Remove);
  failure(Model.finishPnp(Remove, Success), "outstanding operations");
  success(Model.acquireRemoveLock(FirstDevice, Lock, 0x9000));
  EXPECT_FALSE(
      value(Model.releaseRemoveLockAndWait(FirstDevice, Lock, 0x9000)));
  failure(Model.acquireRemoveLock(FirstDevice, Lock, 0xa000),
          "no longer accepts");
  success(Model.finishIo(FirstDevice, Irp));
  EXPECT_EQ(state().OutstandingIo, 0u);
  EXPECT_EQ(state().RemoveLockReferences, 1u);
  failure(Model.finishPnp(Remove, Success), "outstanding operations");
  success(Model.releaseRemoveLock(FirstDevice, Lock, Irp));
  EXPECT_TRUE(value(Model.removeLockDrained(FirstDevice, Lock)));
  success(Model.finishPnp(Remove, Success));
  EXPECT_EQ(state().Pnp, DevicePnpState::Removed);
  failure(Model.initializeRemoveLock(FirstDevice, Lock), "during removal");
  failure(Model.addDevice(FirstDevice, DevicePowerState::D0,
                          SystemPowerState::Working),
          "already exists");
}

TEST_F(DriverDeviceLifecycle, RemoveLocksCountRepeatedTagsAndRequireDrain) {
  constexpr uint64_t Lock = 0x7000;
  success(Model.initializeRemoveLock(FirstDevice, Lock));
  success(Model.acquireRemoveLock(FirstDevice, Lock, 42));
  success(Model.acquireRemoveLock(FirstDevice, Lock, 42));
  EXPECT_EQ(state().RemoveLockReferences, 2u);
  EXPECT_EQ(state().OutstandingIo, 0u);
  failure(Model.releaseRemoveLock(FirstDevice, Lock, 43), "does not hold");
  failure(Model.releaseRemoveLockAndWait(FirstDevice, Lock, 42),
          "requires a remove");
  EXPECT_EQ(state().RemoveLockReferences, 2u);
  success(Model.releaseRemoveLock(FirstDevice, Lock, 42));
  success(Model.releaseRemoveLock(FirstDevice, Lock, 42));
  EXPECT_FALSE(value(Model.removeLockDrained(FirstDevice, Lock)));
  const auto Remove = begin(DevicePnpRequest::Remove);
  failure(Model.finishPnp(Remove, Success), "undrained");
  success(Model.acquireRemoveLock(FirstDevice, Lock, 0));
  EXPECT_TRUE(value(Model.releaseRemoveLockAndWait(FirstDevice, Lock, 0)));
  failure(Model.releaseRemoveLockAndWait(FirstDevice, Lock, 0),
          "already draining");
  failure(Model.releaseRemoveLock(FirstDevice, Lock, 0), "does not hold");
  success(Model.finishPnp(Remove, Success));
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
  success(Model.beginIo(FirstDevice, 0x5000));
  failure(Model.beginIo(SecondDevice, 0x5000), "already has");
  failure(Model.finishIo(SecondDevice, 0x5000), "not outstanding");
  failure(Model.beginIo(FirstDevice, 0), "nonzero");
  EXPECT_EQ(state().OutstandingIo, 1u);
  EXPECT_EQ(state(SecondDevice).OutstandingIo, 0u);
  success(Model.finishIo(FirstDevice, 0x5000));
  success(Model.beginIo(SecondDevice, 0x5000));
  success(Model.initializeRemoveLock(FirstDevice, 0x6000));
  failure(Model.initializeRemoveLock(SecondDevice, 0x6000),
          "already initialized");
  failure(Model.acquireRemoveLock(SecondDevice, 0x6000, 1), "not owned");
  EXPECT_EQ(state().RemoveLocks, 1u);
  EXPECT_EQ(state(SecondDevice).RemoveLocks, 0u);
}

TEST_F(DriverDeviceLifecycle, StartingDoesNotInventHardwarePowerTransitions) {
  constexpr uint64_t SleepingDevice = 0x3000;
  success(Model.addDevice(SleepingDevice, DevicePowerState::D3,
                          SystemPowerState::Working));
  start(SleepingDevice);
  EXPECT_EQ(state(SleepingDevice).DevicePower, DevicePowerState::D3);
  EXPECT_FALSE(state(SleepingDevice).CanStartIo);
  const auto Power = value(Model.beginDevicePower(
      SleepingDevice, DevicePowerRequest::Set, DevicePowerState::D0));
  EXPECT_EQ(state(SleepingDevice).DevicePower, DevicePowerState::D3);
  success(Model.finishDevicePower(Power, Success));
  EXPECT_TRUE(state(SleepingDevice).CanStartIo);
}

TEST_F(DriverDeviceLifecycle, DevicePowerQueriesAndIdleTransitions) {
  start();
  auto Query = devicePower(DevicePowerRequest::Query, DevicePowerState::D3);
  EXPECT_FALSE(state().CanStartIo);
  EXPECT_EQ(state().DevicePower, DevicePowerState::D0);
  success(Model.finishDevicePower(Query, Failure));
  EXPECT_TRUE(state().CanStartIo);
  Query = devicePower(DevicePowerRequest::Query, DevicePowerState::D3);
  success(Model.finishDevicePower(Query, Success));
  EXPECT_TRUE(state().DevicePowerQueryAccepted);
  EXPECT_FALSE(state().CanStartIo);
  // A same-state set releases a query; a prior query is never required to set.
  auto Set = devicePower(DevicePowerRequest::Set, DevicePowerState::D0);
  success(Model.finishDevicePower(Set, Success));
  EXPECT_TRUE(state().CanStartIo);
  for (DevicePowerState Target : {DevicePowerState::D1, DevicePowerState::D2,
                                  DevicePowerState::D3, DevicePowerState::D0}) {
    Set = devicePower(DevicePowerRequest::Set, Target);
    success(Model.finishDevicePower(Set, Success));
    EXPECT_EQ(state().DevicePower, Target);
    EXPECT_EQ(state().CanStartIo, Target == DevicePowerState::D0);
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
  EXPECT_TRUE(state().CanStartIo);
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
  EXPECT_TRUE(state().CanStartIo);
}

TEST_F(DriverDeviceLifecycle, StartedDeviceCanRestartWithChangedResources) {
  start();
  // PNP_RESOURCE_REQUIREMENTS_CHANGED can trigger START without a STOP.
  const auto Restart = begin(DevicePnpRequest::Start);
  EXPECT_FALSE(state().CanStartIo);
  success(Model.finishPnp(Restart, Success));
  EXPECT_EQ(state().Pnp, DevicePnpState::Started);
  EXPECT_TRUE(state().CanStartIo);
  pnp(DevicePnpRequest::Start, Failure);
  EXPECT_EQ(state().Pnp, DevicePnpState::Started);
  EXPECT_TRUE(state().CanStartIo);
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
