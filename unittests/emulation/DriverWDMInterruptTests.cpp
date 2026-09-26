//===- DriverWDMInterruptTests.cpp - Genuine WDK interrupt execution ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Execute actual WDK interrupt APIs, ISR BOOLEAN and synchronized guest code
/// against explicit single-CPU pulses and independent resource assignments.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/emulation/DriverSession.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
#ifdef NEVERD_WDM_INTERRUPT_FIXTURE
std::vector<const char *> images() {
  std::vector<const char *> Images{NEVERD_WDM_INTERRUPT_FIXTURE};
#ifdef NEVERD_WDM_INTERRUPT_CFG_FIXTURE
  Images.push_back(NEVERD_WDM_INTERRUPT_CFG_FIXTURE);
#endif
  return Images;
}

DriverPnpDevice device(unsigned Index) {
  DriverPnpDevice Device;
  Device.ID = "interrupt" + std::to_string(Index);
  Device.Bus = DriverBusKind::RegisterBank;
  Device.InitialDevicePower = DevicePowerState::D0;
  Device.InitialSystemPower = SystemPowerState::Working;
  DriverMemoryResource Resource;
  Resource.ID = "counter";
  Resource.RawStart = 0x200000000ULL + uint64_t(Index) * 0x10000;
  Resource.TranslatedStart = 0x300000000ULL + uint64_t(Index) * 0x10000;
  Resource.Length = 0x1000;
  Resource.Registers = {{0, 4, DriverRegisterAccess::ReadWrite, 0}};
  Device.Resources.push_back(Resource);
  DriverInterruptResource Interrupt;
  Interrupt.ID = "line0";
  Interrupt.RawVector = 17 + Index;
  Interrupt.RawLevel = 7 + Index;
  Interrupt.RawAffinity = 1;
  Interrupt.TranslatedVector = 0x91 + Index;
  Interrupt.TranslatedLevel = 5 + Index;
  Interrupt.TranslatedAffinity = 1;
  Device.Interrupts.push_back(Interrupt);
  return Device;
}

DriverOptions options(char Mode = 'S') {
  DriverOptions Options;
  Options.ServiceName = std::string("NeverDInterrupts") + Mode;
  Options.LoadAddress = 0x190000000;
  Options.Unload = true;
  Options.PnpDevices.push_back(device(0));
  return Options;
}

DriverRequest pnp(DevicePnpRequest Minor, uint64_t Delay = 0,
                  uint32_t Status = 0, llvm::StringRef ID = "interrupt0") {
  DriverRequest Request;
  Request.Kind = DriverRequestKind::Pnp;
  Request.DeviceID = ID.str();
  Request.Pnp = DriverPnpOperation{Minor, DriverBusCompletion{Status, Delay}};
  return Request;
}

DriverRequest file(DriverRequestKind Kind, uint32_t File = 1,
                   llvm::StringRef ID = "interrupt0",
                   std::optional<uint64_t> Pulse = std::nullopt) {
  DriverRequest Request;
  Request.Kind = Kind;
  Request.File = File;
  if (Kind == DriverRequestKind::Create)
    Request.DeviceID = ID.str();
  if (Kind == DriverRequestKind::DeviceControl) {
    Request.ControlCode = 0x222000;
    Request.OutputSize = 32;
    if (Pulse)
      Request.InterruptEvents.push_back({*Pulse, ID.str(), "line0"});
  }
  return Request;
}

void fileCycle(DriverOptions &Options, uint64_t Pulse = 7, uint32_t File = 1,
               llvm::StringRef ID = "interrupt0") {
  Options.Requests.push_back(file(DriverRequestKind::Create, File, ID));
  Options.Requests.push_back(
      file(DriverRequestKind::DeviceControl, File, ID, Pulse));
  Options.Requests.push_back(file(DriverRequestKind::Cleanup, File, ID));
  Options.Requests.push_back(file(DriverRequestKind::Close, File, ID));
}

void remove(DriverOptions &Options, llvm::StringRef ID = "interrupt0") {
  Options.Requests.push_back(pnp(DevicePnpRequest::QueryRemove, 0, 0, ID));
  Options.Requests.push_back(pnp(DevicePnpRequest::Remove, 3, 0, ID));
}

DriverRequest power(DevicePowerState State) {
  DriverRequest Request;
  Request.Kind = DriverRequestKind::Power;
  Request.DeviceID = "interrupt0";
  DriverPowerOperation Operation;
  Operation.Minor = DevicePowerRequest::Set;
  Operation.Type = DriverPowerType::Device;
  Operation.State = uint32_t(State);
  Operation.SystemContext = 0;
  Operation.Action = DriverPowerAction::None;
  Operation.BusCompletion = DriverBusCompletion{0, 0};
  Request.Power = Operation;
  return Request;
}

size_t apiCount(const DriverResult &Result, llvm::StringRef Name) {
  return std::count_if(Result.Calls.begin(), Result.Calls.end(),
                       [Name](const auto &Call) { return Call.Name == Name; });
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
  }
  for (const auto &Request : Result.Requests)
    EXPECT_TRUE(Request.Completed);
  for (const auto &Message : Result.Messages)
    EXPECT_EQ(Message.find("WDM interrupts: failure"), std::string::npos)
        << Message;
}

void snapshot(const DriverRequestResult &Request,
              std::initializer_list<uint32_t> Values) {
  std::vector<uint8_t> Bytes;
  for (uint32_t Value : Values)
    for (unsigned Shift = 0; Shift != 32; Shift += 8)
      Bytes.push_back(uint8_t(Value >> Shift));
  EXPECT_EQ(Request.IOStatus, 0u);
  EXPECT_EQ(Request.Information, 32u);
  EXPECT_EQ(Request.Output, Bytes);
}

void delivered(const DriverInterruptResult &Event, uint32_t RequestIndex,
               uint8_t ReturnValue = 1) {
  EXPECT_EQ(Event.SourceRequestIndex, RequestIndex);
  EXPECT_EQ(Event.EventIndex, 0u);
  EXPECT_NE(Event.Epoch, 0u);
  ASSERT_TRUE(Event.OccurredAt100ns);
  ASSERT_TRUE(Event.DeliveredAt100ns);
  ASSERT_TRUE(Event.ReturnedAt100ns);
  EXPECT_GE(*Event.OccurredAt100ns, Event.DueAt100ns);
  EXPECT_GE(*Event.DeliveredAt100ns, *Event.OccurredAt100ns);
  EXPECT_GE(*Event.ReturnedAt100ns, *Event.DeliveredAt100ns);
  ASSERT_TRUE(Event.InterruptObject);
  EXPECT_NE(*Event.InterruptObject, 0u);
  EXPECT_EQ(Event.ReturnValue, ReturnValue);
  EXPECT_FALSE(Event.UndeliveredReason);
}

TEST(DriverWDMInterrupt, MessageTablesAndActualIsrIdsMatchEveryResource) {
  for (const auto *Image : images())
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL})
      for (char Mode : {'M', 'I', 'U'}) {
        SCOPED_TRACE(Image);
        SCOPED_TRACE(Address);
        SCOPED_TRACE(Mode);
        auto Options = options(Mode);
        Options.LoadAddress = Address;
        auto &First = Options.PnpDevices[0].Interrupts.front();
        First.RawLevel = 0;
        First.Share = Mode == 'U' ? DriverInterruptShare::Shared
                                  : DriverInterruptShare::DeviceExclusive;
        First.Messages = {{0xfee01000, 0x123400, 0x91, 5, 1,
                           DriverInterruptPolarity::RisingEdge},
                          {0xfee01000, 0x123401, 0x92, 7, 1,
                           DriverInterruptPolarity::FallingEdge}};
        auto Second = First;
        Second.ID = "message-bank";
        Second.TranslatedVector = 0x93;
        Second.TranslatedLevel = 8;
        Second.Messages = {{0xfee02000, 0x123402, 0x93, 8, 1}};
        Options.PnpDevices[0].Interrupts.push_back(Second);
        Options.Requests.push_back(pnp(DevicePnpRequest::Start));
        for (uint32_t I = 0; I < 3; ++I) {
          fileCycle(Options, 5, I + 1);
          auto &Event = Options.Requests[Options.Requests.size() - 3]
                            .InterruptEvents.front();
          Event.InterruptID = I == 2 ? "message-bank" : "line0";
          Event.MessageID = I == 2 ? 0 : I;
        }
        remove(Options);
        auto Result = emulateDriver(Image, Options);
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        clean(*Result);
        ASSERT_EQ(Result->Interrupts.size(), 3u);
        for (uint32_t I = 0; I < 3; ++I) {
          const auto &Event = Result->Interrupts[I];
          delivered(Event, 2 + I * 4);
          EXPECT_EQ(Event.MessageID, I == 2 ? 0u : I);
          ASSERT_EQ(Event.Handlers.size(), 1u);
          EXPECT_EQ(Event.Handlers[0].MessageID, I);
          const uint32_t IRQL = Mode == 'I'   ? 9
                                : Mode == 'U' ? 8
                                : I == 0      ? 5
                                : I == 1      ? 7
                                              : 8;
          snapshot(Result->Requests[2 + I * 4],
                   {1, 1, I + 1, I + 1, (I + 1) * 2, I + 1, I + 1, IRQL});
        }
        EXPECT_EQ(apiCount(*Result, "IoConnectInterruptEx"), 1u);
        EXPECT_EQ(apiCount(*Result, "IoDisconnectInterruptEx"), 1u);
      }
}

TEST(DriverWDMInterrupt, PassiveIsrWaitsForDpcAndSerializesRepeatedArrivals) {
  for (const auto *Image : images())
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL})
      for (char Mode : {'W', 'X', 'P'}) {
        SCOPED_TRACE(Image);
        SCOPED_TRACE(Address);
        SCOPED_TRACE(Mode);
        auto Options = options(Mode);
        Options.LoadAddress = Address;
        auto &IRQ = Options.PnpDevices[0].Interrupts[0];
        if (Mode == 'X')
          IRQ.TranslatedLevel = 0;
        if (Mode == 'P') {
          IRQ.RawLevel = 0;
          IRQ.Messages = {{0xfee01000, 0x123400, 0x91, 5, 1}};
        }
        Options.Requests.push_back(pnp(DevicePnpRequest::Start));
        fileCycle(Options, 1);
        auto &Events = Options.Requests[2].InterruptEvents;
        if (Mode == 'P')
          Events.front().MessageID = 0;
        auto Again = Events.front();
        Again.After100ns = 3;
        Events.push_back(Again);
        remove(Options);
        auto Result = emulateDriver(Image, Options);
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        clean(*Result);
        ASSERT_EQ(Result->Interrupts.size(), 2u);
        const auto &First = Result->Interrupts[0];
        const auto &Second = Result->Interrupts[1];
        delivered(First, 2);
        EXPECT_EQ(First.OccurredAt100ns, 1u);
        EXPECT_EQ(First.DeliveredAt100ns, 2u);
        EXPECT_EQ(First.ReturnedAt100ns, 7u);
        EXPECT_EQ(Second.OccurredAt100ns, 3u);
        EXPECT_EQ(Second.DeliveredAt100ns, 7u);
        EXPECT_EQ(Second.ReturnValue, 0u);
        snapshot(Result->Requests[2], {1, 1, 1, 1, 1, 1, 0, 0});
        EXPECT_EQ(apiCount(*Result, "KeWaitForSingleObject"), 1u);
        EXPECT_EQ(apiCount(*Result, "KeDelayExecutionThread"), 1u);
        EXPECT_EQ(apiCount(*Result, "KeAcquireInterruptSpinLock"), 0u);
      }
}

TEST(DriverWDMInterrupt, IndependentPassiveIsrsRetainTheirOwnBlockedFrames) {
  for (const auto *Image : images()) {
    auto Options = options('W');
    Options.PnpDevices.push_back(device(1));
    for (unsigned I = 0; I < 2; ++I) {
      const std::string ID = "interrupt" + std::to_string(I);
      Options.Requests.push_back(pnp(DevicePnpRequest::Start, 0, 0, ID));
      Options.Requests.push_back(file(DriverRequestKind::Create, I + 1, ID));
    }
    for (unsigned I = 0; I < 2; ++I) {
      auto Request = file(DriverRequestKind::DeviceControl, I + 1,
                          "interrupt" + std::to_string(I), 0);
      Request.DeferCallbackDrain = I == 0;
      Options.Requests.push_back(std::move(Request));
    }
    for (unsigned I = 0; I < 2; ++I) {
      const std::string ID = "interrupt" + std::to_string(I);
      Options.Requests.push_back(file(DriverRequestKind::Cleanup, I + 1, ID));
      Options.Requests.push_back(file(DriverRequestKind::Close, I + 1, ID));
      remove(Options, ID);
    }
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result, 2);
    ASSERT_EQ(Result->Interrupts.size(), 2u);
    for (unsigned I = 0; I < 2; ++I) {
      const auto &Event = Result->Interrupts[I];
      delivered(Event, 4 + I);
      ASSERT_TRUE(Event.DeliveredAt100ns);
      ASSERT_TRUE(Event.ReturnedAt100ns);
      EXPECT_EQ(*Event.ReturnedAt100ns - *Event.DeliveredAt100ns, 5u);
      snapshot(Result->Requests[4 + I], {I + 1, 1, 1, 1, 1, 1, 0, 0});
    }
  }
}

TEST(DriverWDMInterrupt, MessageVersionFallsBackToLineRegistration) {
  for (const auto *Image : images()) {
    auto Options = options('M');
    Options.Requests.push_back(pnp(DevicePnpRequest::Start));
    fileCycle(Options);
    remove(Options);
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    ASSERT_EQ(Result->Interrupts.size(), 1u);
    delivered(Result->Interrupts[0], 2);
    ASSERT_EQ(Result->Interrupts[0].Handlers.size(), 1u);
    EXPECT_FALSE(Result->Interrupts[0].Handlers[0].MessageID);
    EXPECT_EQ(apiCount(*Result, "IoConnectInterruptEx"), 1u);
    EXPECT_EQ(apiCount(*Result, "IoDisconnectInterruptEx"), 1u);
  }
}

TEST(DriverWDMInterrupt,
     LevelSourceRetriggersAfterClaimUntilExplicitDeassertion) {
  for (const auto *Image : images())
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Address);
      auto Options = options('D');
      Options.LoadAddress = Address;
      auto &Resource = Options.PnpDevices.front().Interrupts.front();
      Resource.Mode = DriverInterruptMode::LevelSensitive;
      Resource.RetriggerAfter100ns = 3;
      Options.Requests.push_back(pnp(DevicePnpRequest::Start));
      fileCycle(Options);
      auto &Request = Options.Requests[2];
      Request.InterruptEvents = {
          {7, "interrupt0", "line0", DriverInterruptAction::Assert},
          {15, "interrupt0", "line0", DriverInterruptAction::Deassert}};
      remove(Options);
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      clean(*Result);
      snapshot(Result->Requests[2], {1, 1, 1, 1, 2, 1, 1, 5});
      ASSERT_EQ(Result->Interrupts.size(), 2u);
      const auto &Assert = Result->Interrupts[0];
      EXPECT_EQ(Assert.Action, DriverInterruptAction::Assert);
      EXPECT_EQ(Assert.DueAt100ns, 7u);
      EXPECT_EQ(Assert.OccurredAt100ns, 7u);
      ASSERT_EQ(Assert.Handlers.size(), 3u);
      for (size_t I = 0; I < Assert.Handlers.size(); ++I) {
        EXPECT_EQ(Assert.Handlers[I].DeliveryIndex, I);
        EXPECT_EQ(Assert.Handlers[I].DeliveredAt100ns, 7 + I * 3);
        EXPECT_EQ(Assert.Handlers[I].ReturnedAt100ns, 7 + I * 3);
        EXPECT_EQ(Assert.Handlers[I].ReturnValue, I == 0 ? 1u : 0u);
      }
      const auto &Deassert = Result->Interrupts[1];
      EXPECT_EQ(Deassert.Action, DriverInterruptAction::Deassert);
      EXPECT_EQ(Deassert.OccurredAt100ns, 15u);
      EXPECT_FALSE(Deassert.DeliveredAt100ns);
      EXPECT_TRUE(Deassert.Handlers.empty());
      EXPECT_FALSE(Deassert.ReturnValue);
      EXPECT_EQ(apiCount(*Result, "IoConnectInterruptEx"), 1u);
      EXPECT_EQ(apiCount(*Result, "IoDisconnectInterruptEx"), 1u);
    }
}

TEST(DriverWDMInterrupt,
     LegacyElevenArgumentsIsrDpcExecuteNormalCfgAndRebased) {
  for (const auto *Image : images())
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Address);
      auto Options = options();
      Options.LoadAddress = Address;
      Options.Requests.push_back(pnp(DevicePnpRequest::Start, 11));
      fileCycle(Options);
      remove(Options);
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      clean(*Result);
      ASSERT_EQ(Result->Requests.size(), 7u);
      EXPECT_EQ(Result->Requests[2].DispatchStatus, 0x103u);
      snapshot(Result->Requests[2], {1, 1, 1, 1, 2, 1, 1, 5});
      ASSERT_EQ(Result->Interrupts.size(), 1u);
      delivered(Result->Interrupts[0], 2);
      EXPECT_EQ(Result->Interrupts[0].DueAt100ns, 18u);
      EXPECT_EQ(apiCount(*Result, "IoConnectInterrupt"), 1u);
      EXPECT_EQ(apiCount(*Result, "IoDisconnectInterrupt"), 1u);
      EXPECT_EQ(apiCount(*Result, "KeSynchronizeExecution"), 3u);
      for (const auto &Call : Result->Calls)
        if (Call.Name == "IoConnectInterrupt") {
          ASSERT_EQ(Call.Arguments.size(), 11u);
          EXPECT_NE(Call.Arguments[0], 0u);
          EXPECT_NE(Call.Arguments[1], 0u);
          EXPECT_NE(Call.Arguments[2], 0u);
          EXPECT_EQ(Call.Arguments[3], 0u);
          EXPECT_EQ(uint32_t(Call.Arguments[4]), 0x91u);
          EXPECT_EQ(uint8_t(Call.Arguments[5]), 5u);
          EXPECT_EQ(uint8_t(Call.Arguments[6]), 5u);
          EXPECT_EQ(uint32_t(Call.Arguments[7]), 1u);
          EXPECT_EQ(uint8_t(Call.Arguments[8]), 0u);
          EXPECT_EQ(Call.Arguments[9], 1u);
          EXPECT_EQ(uint8_t(Call.Arguments[10]), 0u);
        }
      EXPECT_LT(messageIndex(*Result, "pending unit=1"),
                messageIndex(*Result, "ISR unit=1"));
      EXPECT_LT(messageIndex(*Result, "ISR unit=1"),
                messageIndex(*Result, "DPC unit=1"));
      EXPECT_LT(messageIndex(*Result, "DPC unit=1"),
                messageIndex(*Result, "disconnected unit=1"));
    }
}

TEST(DriverWDMInterrupt,
     DpcCriticalSectionsAndManualLocksRestoreBothIrqlViews) {
  for (const auto *Image : images()) {
    auto Options = options('C');
    Options.Requests.push_back(pnp(DevicePnpRequest::Start));
    fileCycle(Options);
    remove(Options);
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    snapshot(Result->Requests[2], {1, 1, 1, 1, 2, 2, 2, 5});
    EXPECT_EQ(apiCount(*Result, "KeAcquireInterruptSpinLock"), 2u);
    EXPECT_EQ(apiCount(*Result, "KeReleaseInterruptSpinLock"), 2u);
    EXPECT_EQ(apiCount(*Result, "KeSynchronizeExecution"), 4u);
  }
}

TEST(DriverWDMInterrupt, SharedLineInvokesEachIsrBeforeDpcWithOneCallerLock) {
  for (const auto *Image : images())
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Address);
      auto Options = options('R');
      Options.LoadAddress = Address;
      Options.PnpDevices.push_back(device(1));
      for (auto &Device : Options.PnpDevices) {
        auto &IRQ = Device.Interrupts.front();
        IRQ.Share = DriverInterruptShare::Shared;
        IRQ.TranslatedVector = 0x91;
        IRQ.TranslatedLevel = 5;
      }
      Options.Requests.push_back(pnp(DevicePnpRequest::Start));
      Options.Requests.push_back(
          pnp(DevicePnpRequest::Start, 0, 0, "interrupt1"));
      fileCycle(Options, 7, 1, "interrupt0");
      fileCycle(Options, 7, 2, "interrupt1");
      remove(Options);
      remove(Options, "interrupt1");
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      clean(*Result, 2);
      snapshot(Result->Requests[3], {1, 1, 1, 1, 2, 1, 1, 5});
      snapshot(Result->Requests[7], {2, 1, 2, 1, 2, 1, 1, 5});
      ASSERT_EQ(Result->Interrupts.size(), 2u);
      for (size_t I = 0; I < Result->Interrupts.size(); ++I) {
        const auto &Event = Result->Interrupts[I];
        ASSERT_EQ(Event.Handlers.size(), 2u);
        EXPECT_EQ(Event.ReturnValue, 1u);
        EXPECT_EQ(Event.Handlers[I].ReturnValue, 1u);
        EXPECT_EQ(Event.Handlers[1 - I].ReturnValue, 0u);
        EXPECT_EQ(Event.Handlers[0].ReturnedAt100ns,
                  Event.Handlers[1].DeliveredAt100ns);
      }
      EXPECT_LT(messageIndex(*Result, "ISR unit=2"),
                messageIndex(*Result, "DPC unit=1"));
      EXPECT_EQ(apiCount(*Result, "IoConnectInterruptEx"), 2u);
      EXPECT_EQ(apiCount(*Result, "IoDisconnectInterruptEx"), 2u);
    }
}

TEST(DriverWDMInterrupt,
     ExFullySpecifiedGroupZeroAndLineBasedUseSameCallbacks) {
  for (const auto *Image : images())
    for (char Mode : {'E', 'G', 'L'}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Mode);
      auto Options = options(Mode);
      Options.Requests.push_back(pnp(DevicePnpRequest::Start, 3));
      fileCycle(Options);
      remove(Options);
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      clean(*Result);
      snapshot(Result->Requests[2], {1, 1, 1, 1, 2, 1, 1, 5});
      ASSERT_EQ(Result->Interrupts.size(), 1u);
      delivered(Result->Interrupts[0], 2);
      EXPECT_EQ(apiCount(*Result, "IoConnectInterruptEx"), 1u);
      EXPECT_EQ(apiCount(*Result, "IoDisconnectInterruptEx"), 1u);
      EXPECT_EQ(apiCount(*Result, "IoConnectInterrupt"), 0u);
      for (const auto &Call : Result->Calls)
        if (Call.Name == "IoConnectInterruptEx" ||
            Call.Name == "IoDisconnectInterruptEx")
          EXPECT_EQ(Call.Arguments.size(), 1u);
    }
}

TEST(DriverWDMInterrupt,
     FalseLowAlIgnoresUpperGarbageAndSurvivesCompletedSource) {
  for (const auto *Image : images())
    for (uint64_t Delay : {0u, 7u}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Delay);
      auto Options = options('F');
      Options.Requests.push_back(pnp(DevicePnpRequest::Start));
      fileCycle(Options, Delay);
      remove(Options);
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      clean(*Result);
      ASSERT_EQ(Result->Interrupts.size(), 1u);
      delivered(Result->Interrupts[0], 2, 0);
      EXPECT_EQ(Result->Requests[2].DispatchStatus, 0u);
      EXPECT_EQ(apiCount(*Result, "KeInsertQueueDpc"), 0u);
      EXPECT_NE(messageIndex(*Result, "unclaimed unit=1"),
                Result->Messages.size());
      if (Delay)
        EXPECT_LT(messageIndex(*Result, "foreground complete unit=1"),
                  messageIndex(*Result, "ISR unit=1"));
    }
}

TEST(DriverWDMInterrupt,
     StopRestartReplacesConnectionAndKeepsIndependentEpochs) {
  for (const auto *Image : images()) {
    auto Options = options();
    Options.Requests = {
        pnp(DevicePnpRequest::Start),
        file(DriverRequestKind::Create),
        file(DriverRequestKind::DeviceControl, 1, "interrupt0", 7),
        pnp(DevicePnpRequest::QueryStop),
        pnp(DevicePnpRequest::Stop),
        pnp(DevicePnpRequest::Start, 3),
        file(DriverRequestKind::DeviceControl, 1, "interrupt0", 7),
        file(DriverRequestKind::Cleanup),
        file(DriverRequestKind::Close)};
    remove(Options);
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    snapshot(Result->Requests[2], {1, 1, 1, 1, 2, 1, 1, 5});
    snapshot(Result->Requests[6], {1, 2, 2, 2, 4, 2, 2, 5});
    ASSERT_EQ(Result->Interrupts.size(), 2u);
    delivered(Result->Interrupts[0], 2);
    delivered(Result->Interrupts[1], 6);
    EXPECT_NE(Result->Interrupts[0].Epoch, Result->Interrupts[1].Epoch);
    EXPECT_NE(Result->Interrupts[0].InterruptObject,
              Result->Interrupts[1].InterruptObject);
    EXPECT_EQ(apiCount(*Result, "IoConnectInterrupt"), 2u);
    EXPECT_EQ(apiCount(*Result, "IoDisconnectInterrupt"), 2u);
  }
}

TEST(DriverWDMInterrupt, TwoPdoVectorsInvokeOnlyTheirOwnGuestState) {
  for (const auto *Image : images()) {
    auto Options = options();
    Options.PnpDevices.push_back(device(1));
    Options.Requests = {pnp(DevicePnpRequest::Start),
                        pnp(DevicePnpRequest::Start, 0, 0, "interrupt1")};
    fileCycle(Options, 7, 1, "interrupt0");
    fileCycle(Options, 7, 2, "interrupt1");
    remove(Options, "interrupt0");
    remove(Options, "interrupt1");
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result, 2);
    snapshot(Result->Requests[3], {1, 1, 1, 1, 2, 1, 1, 5});
    snapshot(Result->Requests[7], {2, 1, 1, 1, 2, 1, 1, 6});
    ASSERT_EQ(Result->Interrupts.size(), 2u);
    EXPECT_EQ(Result->Interrupts[0].DeviceID, "interrupt0");
    EXPECT_EQ(Result->Interrupts[1].DeviceID, "interrupt1");
    EXPECT_NE(Result->Interrupts[0].InterruptObject,
              Result->Interrupts[1].InterruptObject);
  }
}

TEST(DriverWDMInterrupt, LowerStartFailureCannotCreateAnInterruptConnection) {
  for (const auto *Image : images()) {
    auto Options = options();
    Options.Unload = false;
    Options.Requests = {pnp(DevicePnpRequest::Start, 3, 0xc0000001)};
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    EXPECT_EQ(Result->Requests[0].IOStatus, 0xc0000001u);
    EXPECT_EQ(apiCount(*Result, "IoConnectInterrupt"), 0u);
    EXPECT_EQ(apiCount(*Result, "MmMapIoSpace"), 0u);
    EXPECT_TRUE(Result->Interrupts.empty());
  }
}

TEST(DriverWDMInterrupt, StopWithoutDisconnectFailsBeforeDeviceRetirement) {
  for (const auto *Image : images()) {
    auto Options = options('N');
    Options.Requests = {pnp(DevicePnpRequest::Start),
                        pnp(DevicePnpRequest::QueryStop),
                        pnp(DevicePnpRequest::Stop)};
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::ModelError) << Result->Diagnostic;
    EXPECT_FALSE(Result->Diagnostic.empty());
    EXPECT_EQ(apiCount(*Result, "IoConnectInterrupt"), 1u);
    EXPECT_EQ(apiCount(*Result, "IoDisconnectInterrupt"), 0u);
    EXPECT_EQ(apiCount(*Result, "IoDeleteDevice"), 0u);
  }
}

TEST(DriverWDMInterrupt,
     UnassignedVectorLevelAndProcessorDoNotInventConnections) {
  for (const auto *Image : images())
    for (char Mode : {'Q', 'J', 'A', 'Z', 'V', 'Y'}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Mode);
      auto Options = options(Mode);
      Options.Requests = {pnp(DevicePnpRequest::Start)};
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      EXPECT_EQ(Result->Stop, DriverStopReason::ModelError)
          << Result->Diagnostic;
      EXPECT_FALSE(Result->Diagnostic.empty());
      EXPECT_EQ(messageIndex(*Result, "connected unit=1"),
                Result->Messages.size());
    }
}

TEST(DriverWDMInterrupt, RecursiveLockWrongRestoreAndOpaqueStaleTokensFail) {
  for (const auto *Image : images())
    for (char Mode : {'H', 'K', 'O', 'T'}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Mode);
      auto Options = options(Mode);
      Options.Requests = {pnp(DevicePnpRequest::Start),
                          file(DriverRequestKind::Create),
                          file(DriverRequestKind::DeviceControl)};
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      EXPECT_EQ(Result->Stop, DriverStopReason::ModelError)
          << Result->Diagnostic;
      EXPECT_FALSE(Result->Diagnostic.empty());
      EXPECT_FALSE(Result->UnloadCompleted);
      EXPECT_EQ(apiCount(*Result, "IoDeleteDevice"), 0u);
    }
}

TEST(DriverWDMInterrupt, IsrCannotWaitAtDirql) {
  for (const auto *Image : images()) {
    auto Options = options('B');
    Options.Requests = {
        pnp(DevicePnpRequest::Start), file(DriverRequestKind::Create),
        file(DriverRequestKind::DeviceControl, 1, "interrupt0", 7)};
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::ModelError) << Result->Diagnostic;
    EXPECT_FALSE(Result->Requests[2].Completed);
    EXPECT_EQ(apiCount(*Result, "KeInsertQueueDpc"), 0u);
    ASSERT_EQ(Result->Interrupts.size(), 1u);
    EXPECT_TRUE(Result->Interrupts[0].DeliveredAt100ns);
    EXPECT_FALSE(Result->Interrupts[0].ReturnValue);
  }
}

TEST(DriverWDMInterrupt, DeviceD3AndSurpriseCannotDeliverDeclaredPulse) {
  for (const auto *Image : images())
    for (bool Surprise : {false, true}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Surprise);
      auto Options = options('F');
      Options.Requests = {pnp(DevicePnpRequest::Start),
                          file(DriverRequestKind::Create)};
      Options.Requests.push_back(Surprise
                                     ? pnp(DevicePnpRequest::SurpriseRemoval)
                                     : power(DevicePowerState::D3));
      Options.Requests.push_back(
          file(DriverRequestKind::DeviceControl, 1, "interrupt0", 7));
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      EXPECT_EQ(Result->Stop, DriverStopReason::ModelError)
          << Result->Diagnostic;
      EXPECT_FALSE(Result->Diagnostic.empty());
      EXPECT_EQ(messageIndex(*Result, "ISR unit=1"), Result->Messages.size());
      EXPECT_EQ(apiCount(*Result, "KeInsertQueueDpc"), 0u);
      if (Surprise) {
        // Submission cannot bind a pulse to an already absent assignment.
        // No event observation is invented for a rejected request.
        EXPECT_TRUE(Result->Interrupts.empty());
      } else {
        ASSERT_EQ(Result->Interrupts.size(), 1u);
        EXPECT_TRUE(Result->Interrupts[0].UndeliveredReason);
        EXPECT_FALSE(Result->Interrupts[0].DeliveredAt100ns);
      }
    }
}
#else
TEST(DriverWDMInterrupt, GenuineFixtureUnavailable) {
  GTEST_SKIP()
      << "Set NEVERD_WDM_INTERRUPT_FIXTURE to an original driver built "
         "with genuine WDK headers/libraries; no substitute stub is used";
}
#endif
} // namespace
} // namespace neverd::emulation
