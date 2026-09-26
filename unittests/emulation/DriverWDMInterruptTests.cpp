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

TEST(DriverWDMInterrupt, LegacyElevenArgumentsIsrDpcExecuteNormalCfgAndRebased) {
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

TEST(DriverWDMInterrupt, DpcCriticalSectionsAndManualLocksRestoreBothIrqlViews) {
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

TEST(DriverWDMInterrupt, ExFullySpecifiedGroupZeroAndLineBasedUseSameCallbacks) {
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

TEST(DriverWDMInterrupt, FalseLowAlIgnoresUpperGarbageAndSurvivesCompletedSource) {
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

TEST(DriverWDMInterrupt, StopRestartReplacesConnectionAndKeepsIndependentEpochs) {
  for (const auto *Image : images()) {
    auto Options = options();
    Options.Requests = {
        pnp(DevicePnpRequest::Start), file(DriverRequestKind::Create),
        file(DriverRequestKind::DeviceControl, 1, "interrupt0", 7),
        pnp(DevicePnpRequest::QueryStop), pnp(DevicePnpRequest::Stop),
        pnp(DevicePnpRequest::Start, 3),
        file(DriverRequestKind::DeviceControl, 1, "interrupt0", 7),
        file(DriverRequestKind::Cleanup), file(DriverRequestKind::Close)};
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

TEST(DriverWDMInterrupt, UnassignedVectorLevelAndProcessorDoNotInventConnections) {
  for (const auto *Image : images())
    for (char Mode : {'Q', 'J', 'A', 'Z', 'M', 'P', 'V', 'Y'}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Mode);
      auto Options = options(Mode);
      Options.Requests = {pnp(DevicePnpRequest::Start)};
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      EXPECT_EQ(Result->Stop, DriverStopReason::ModelError) << Result->Diagnostic;
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
      EXPECT_EQ(Result->Stop, DriverStopReason::ModelError) << Result->Diagnostic;
      EXPECT_FALSE(Result->Diagnostic.empty());
      EXPECT_FALSE(Result->UnloadCompleted);
      EXPECT_EQ(apiCount(*Result, "IoDeleteDevice"), 0u);
    }
}

TEST(DriverWDMInterrupt, IsrCannotWaitAtDirql) {
  for (const auto *Image : images()) {
    auto Options = options('B');
    Options.Requests = {pnp(DevicePnpRequest::Start),
                        file(DriverRequestKind::Create),
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
      EXPECT_EQ(Result->Stop, DriverStopReason::ModelError) << Result->Diagnostic;
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
  GTEST_SKIP() << "Set NEVERD_WDM_INTERRUPT_FIXTURE to an original driver built "
                  "with genuine WDK headers/libraries; no substitute stub is used";
}
#endif
} // namespace
} // namespace neverd::emulation
