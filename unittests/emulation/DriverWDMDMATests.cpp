//===- DriverWDMDMATests.cpp - Genuine WDK DMA execution ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Execute adapter function pointers, DPC list callbacks, explicit device RAM
/// transactions and independent interrupt completion using real WDK fixtures.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/emulation/DriverSession.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
#ifdef NEVERD_WDM_DMA_FIXTURE
constexpr uint64_t LogicalBase = 0x40000000;

std::vector<const char *> images() {
  std::vector<const char *> Images{NEVERD_WDM_DMA_FIXTURE};
#ifdef NEVERD_WDM_DMA_CFG_FIXTURE
  Images.push_back(NEVERD_WDM_DMA_CFG_FIXTURE);
#endif
  return Images;
}

std::vector<uint8_t> bytes(uint8_t Start) {
  std::vector<uint8_t> Result;
  for (unsigned I = 0; I != 16; ++I)
    Result.push_back(uint8_t(Start + I));
  return Result;
}

DriverPnpDevice device(unsigned Index = 0, uint32_t Registers = 4) {
  DriverPnpDevice Device;
  Device.ID = "dma" + std::to_string(Index);
  Device.Bus = DriverBusKind::RegisterBank;
  Device.InitialDevicePower = DevicePowerState::D0;
  Device.InitialSystemPower = SystemPowerState::Working;
  DriverInterruptResource Interrupt;
  Interrupt.ID = "line0";
  Interrupt.RawVector = 17 + Index;
  Interrupt.RawLevel = 7 + Index;
  Interrupt.RawAffinity = 1;
  Interrupt.TranslatedVector = 145 + Index;
  Interrupt.TranslatedLevel = 5 + Index;
  Interrupt.TranslatedAffinity = 1;
  Device.Interrupts.push_back(Interrupt);
  Device.Dma =
      DriverDmaConfig{64, 4096, Registers, 1, LogicalBase, 65536, true};
  return Device;
}

DriverOptions options(char Mode = 'C') {
  DriverOptions Options;
  Options.ServiceName = std::string("NeverDDma") + Mode;
  Options.LoadAddress = 0x190000000;
  Options.Unload = true;
  Options.PnpDevices.push_back(device(0, Mode == 'Q' ? 1 : 4));
  return Options;
}

DriverRequest pnp(DevicePnpRequest Minor, uint64_t Delay = 0,
                  uint32_t Status = 0, unsigned Index = 0) {
  DriverRequest Request;
  Request.Kind = DriverRequestKind::Pnp;
  Request.DeviceID = "dma" + std::to_string(Index);
  Request.Pnp = DriverPnpOperation{Minor, DriverBusCompletion{Status, Delay}};
  return Request;
}

DriverRequest file(DriverRequestKind Kind, unsigned Index = 0) {
  DriverRequest Request;
  Request.Kind = Kind;
  Request.File = Index + 1;
  if (Kind == DriverRequestKind::Create)
    Request.DeviceID = "dma" + std::to_string(Index);
  return Request;
}

DriverDmaEvent event(DriverDmaDirection Direction, uint64_t Delay = 7,
                     uint64_t Address = LogicalBase, unsigned Index = 0) {
  DriverDmaEvent Event;
  Event.After100ns = Delay;
  Event.DeviceID = "dma" + std::to_string(Index);
  Event.LogicalAddress = Address;
  Event.Direction = Direction;
  Event.Length = 16;
  if (Direction == DriverDmaDirection::WriteMemory)
    Event.Data = bytes(0xa0);
  return Event;
}

DriverRequest ioctl(char Mode = 'C', unsigned Index = 0,
                    uint64_t Address = LogicalBase, bool Events = true) {
  auto Request = file(DriverRequestKind::DeviceControl, Index);
  Request.ControlCode = Mode == 'V' ? 0x222001 : 0x222000;
  Request.OutputSize = 32;
  if (Mode == 'I' || !Events)
    return Request;
  Request.InterruptEvents.push_back(
      {7, "dma" + std::to_string(Index), "line0"});
  if (Mode == 'C' || Mode == 'N') {
    Request.DmaEvents.push_back(
        event(DriverDmaDirection::ReadMemory, 5, Address, Index));
    Request.DmaEvents.push_back(
        event(DriverDmaDirection::WriteMemory, 7, Address, Index));
  } else {
    Request.DmaEvents.push_back(event(Mode == 'R' || Mode == 'Q'
                                          ? DriverDmaDirection::WriteMemory
                                          : DriverDmaDirection::ReadMemory,
                                      7, Address, Index));
  }
  return Request;
}

void finish(DriverOptions &Options, unsigned Index = 0) {
  Options.Requests.push_back(file(DriverRequestKind::Cleanup, Index));
  Options.Requests.push_back(file(DriverRequestKind::Close, Index));
  Options.Requests.push_back(pnp(DevicePnpRequest::QueryRemove, 0, 0, Index));
  Options.Requests.push_back(pnp(DevicePnpRequest::Remove, 3, 0, Index));
}

DriverOptions basic(char Mode = 'C') {
  auto Options = options(Mode);
  Options.Requests.push_back(pnp(DevicePnpRequest::Start, 11));
  Options.Requests.push_back(file(DriverRequestKind::Create));
  Options.Requests.push_back(
      ioctl(Mode, 0, LogicalBase + (Mode == 'Q' ? 4096 : 0)));
  finish(Options);
  return Options;
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

void noGuestFailures(const DriverResult &Result) {
  for (const auto &Message : Result.Messages)
    EXPECT_EQ(Message.find("WDM DMA: failure"), std::string::npos) << Message;
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
  noGuestFailures(Result);
}

void snapshot(const DriverRequestResult &Request, uint32_t Unit,
              uint32_t Starts, uint32_t Callbacks, uint32_t Interrupts,
              llvm::ArrayRef<uint8_t> Data) {
  std::vector<uint8_t> Expected;
  for (uint32_t Value : {Unit, Starts, Callbacks, Interrupts})
    for (unsigned Shift = 0; Shift != 32; Shift += 8)
      Expected.push_back(uint8_t(Value >> Shift));
  Expected.insert(Expected.end(), Data.begin(), Data.end());
  EXPECT_EQ(Request.DispatchStatus, 0x103u);
  EXPECT_EQ(Request.IOStatus, 0u);
  EXPECT_EQ(Request.Information, 32u);
  EXPECT_EQ(Request.Output, Expected);
}

void transaction(const DriverDmaResult &Transfer, uint32_t Source,
                 uint32_t EventIndex, uint64_t Address) {
  EXPECT_EQ(Transfer.SourceRequestIndex, Source);
  EXPECT_EQ(Transfer.EventIndex, EventIndex);
  EXPECT_EQ(Transfer.LogicalAddress, Address);
  EXPECT_NE(Transfer.Epoch, 0u);
  ASSERT_TRUE(Transfer.OccurredAt100ns);
  ASSERT_TRUE(Transfer.CompletedAt100ns);
  EXPECT_GE(*Transfer.OccurredAt100ns, Transfer.DueAt100ns);
  EXPECT_GE(*Transfer.CompletedAt100ns, *Transfer.OccurredAt100ns);
  EXPECT_TRUE(Transfer.Mapping);
  EXPECT_TRUE(Transfer.Adapter);
  EXPECT_FALSE(Transfer.FailureReason);
}

TEST(DriverWDMDMA, CommonBufferSharesRamThroughNormalCfgAndRebasedImages) {
  for (const auto *Image : images())
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      auto Options = basic();
      Options.LoadAddress = Address;
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      clean(*Result);
      ASSERT_EQ(Result->Requests.size(), 7u);
      snapshot(Result->Requests[2], 1, 1, 0, 1, bytes(0xa0));
      ASSERT_EQ(Result->DmaTransfers.size(), 2u);
      transaction(Result->DmaTransfers[0], 2, 0, LogicalBase);
      transaction(Result->DmaTransfers[1], 2, 1, LogicalBase);
      EXPECT_EQ(Result->DmaTransfers[0].Data, bytes(0x11));
      EXPECT_EQ(Result->DmaTransfers[0].DueAt100ns, 16u);
      EXPECT_EQ(Result->DmaTransfers[1].DueAt100ns, 18u);
      ASSERT_EQ(Result->Interrupts.size(), 1u);
      EXPECT_EQ(Result->Interrupts[0].ReturnValue, 1u);
      ASSERT_TRUE(Result->Interrupts[0].DeliveredAt100ns);
      EXPECT_LE(*Result->DmaTransfers[1].CompletedAt100ns,
                *Result->Interrupts[0].DeliveredAt100ns);
      EXPECT_EQ(apiCount(*Result, "IoGetDmaAdapter"), 1u);
      EXPECT_EQ(apiCount(*Result, "AllocateCommonBuffer"), 1u);
      EXPECT_EQ(apiCount(*Result, "FreeCommonBuffer"), 1u);
      EXPECT_EQ(apiCount(*Result, "PutDmaAdapter"), 1u);
      EXPECT_LT(messageIndex(*Result, "adapter unit=1"),
                messageIndex(*Result, "started unit=1"));
    }
}

TEST(DriverWDMDMA, ScatterGatherDirectionsUseRealDpcAndReleaseBeforeCpuRead) {
  for (const auto *Image : images())
    for (char Mode : {'S', 'R'}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Mode);
      auto Result = emulateDriver(Image, basic(Mode));
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      clean(*Result);
      ASSERT_EQ(Result->Requests.size(), 7u);
      snapshot(Result->Requests[2], 1, 1, 1, 1,
               bytes(Mode == 'R' ? 0xa0 : 0x21));
      ASSERT_EQ(Result->DmaTransfers.size(), 1u);
      transaction(Result->DmaTransfers[0], 2, 0, LogicalBase);
      if (Mode == 'S')
        EXPECT_EQ(Result->DmaTransfers[0].Data, bytes(0x21));
      EXPECT_EQ(apiCount(*Result, "GetScatterGatherList"), 1u);
      EXPECT_EQ(apiCount(*Result, "PutScatterGatherList"), 1u);
      EXPECT_LT(messageIndex(*Result, "callback unit=1 packet=0"),
                messageIndex(*Result, "get returned unit=1 packet=0"));
      EXPECT_LT(messageIndex(*Result, "put unit=1 packet=0"),
                messageIndex(*Result, "completed unit=1"));
    }
}

TEST(DriverWDMDMA, RegisterExhaustionQueuesAndPromotesDistinctListCallback) {
  for (const auto *Image : images()) {
    auto Result = emulateDriver(Image, basic('Q'));
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    ASSERT_EQ(Result->Requests.size(), 7u);
    snapshot(Result->Requests[2], 1, 1, 2, 1, bytes(0xa0));
    ASSERT_EQ(Result->DmaTransfers.size(), 1u);
    transaction(Result->DmaTransfers[0], 2, 0, LogicalBase + 4096);
    EXPECT_EQ(apiCount(*Result, "GetScatterGatherList"), 2u);
    EXPECT_EQ(apiCount(*Result, "PutScatterGatherList"), 2u);
    EXPECT_LT(messageIndex(*Result, "get returned unit=1 packet=1"),
              messageIndex(*Result, "second packet queued unit=1"));
    EXPECT_LT(messageIndex(*Result, "second packet queued unit=1"),
              messageIndex(*Result, "callback unit=1 packet=1"));
    EXPECT_LT(messageIndex(*Result, "put unit=1 packet=0"),
              messageIndex(*Result, "callback unit=1 packet=1"));
  }
}

TEST(DriverWDMDMA, InlineListCallbackMayReleaseAdapterAndCompleteBeforeReturn) {
  for (const auto *Image : images()) {
    auto Result = emulateDriver(Image, basic('I'));
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    ASSERT_EQ(Result->Requests.size(), 7u);
    snapshot(Result->Requests[2], 1, 1, 1, 0, bytes(0x21));
    EXPECT_TRUE(Result->DmaTransfers.empty());
    EXPECT_TRUE(Result->Interrupts.empty());
    EXPECT_EQ(apiCount(*Result, "PutDmaAdapter"), 1u);
    EXPECT_LT(messageIndex(*Result, "callback completed before return unit=1"),
              messageIndex(*Result, "get returned unit=1 packet=0"));
  }
}

TEST(DriverWDMDMA, EqualLogicalAperturesRemainIndependentAcrossPdos) {
  for (const auto *Image : images()) {
    auto Options = options();
    Options.PnpDevices.push_back(device(1));
    Options.Requests = {pnp(DevicePnpRequest::Start),
                        pnp(DevicePnpRequest::Start, 0, 0, 1)};
    for (unsigned Index = 0; Index != 2; ++Index) {
      Options.Requests.push_back(file(DriverRequestKind::Create, Index));
      Options.Requests.push_back(ioctl('C', Index));
      finish(Options, Index);
    }
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result, 2);
    ASSERT_EQ(Result->Requests.size(), 14u);
    snapshot(Result->Requests[3], 1, 1, 0, 1, bytes(0xa0));
    snapshot(Result->Requests[9], 2, 1, 0, 1, bytes(0xa0));
    ASSERT_EQ(Result->DmaTransfers.size(), 4u);
    EXPECT_EQ(Result->DmaTransfers[0].LogicalAddress,
              Result->DmaTransfers[2].LogicalAddress);
    EXPECT_NE(Result->DmaTransfers[0].Mapping, Result->DmaTransfers[2].Mapping);
    EXPECT_NE(Result->DmaTransfers[0].Adapter, Result->DmaTransfers[2].Adapter);
    EXPECT_EQ(Result->DmaTransfers[0].Data, bytes(0x11));
    EXPECT_EQ(Result->DmaTransfers[2].Data, bytes(0x12));
    EXPECT_EQ(Result->DmaTransfers[0].DeviceID, "dma0");
    EXPECT_EQ(Result->DmaTransfers[2].DeviceID, "dma1");
  }
}

TEST(DriverWDMDMA, StopReleasesStorageAndRestartUsesFreshLogicalAddress) {
  for (const auto *Image : images()) {
    auto Options = basic();
    Options.Requests.resize(5);
    Options.Requests.push_back(pnp(DevicePnpRequest::QueryStop));
    Options.Requests.push_back(pnp(DevicePnpRequest::Stop, 3));
    Options.Requests.push_back(pnp(DevicePnpRequest::Start, 5));
    Options.Requests.push_back(file(DriverRequestKind::Create));
    Options.Requests.push_back(ioctl('C', 0, LogicalBase + 4096));
    finish(Options);
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    ASSERT_EQ(Result->Requests.size(), 14u);
    snapshot(Result->Requests[9], 1, 2, 0, 2, bytes(0xa0));
    ASSERT_EQ(Result->DmaTransfers.size(), 4u);
    transaction(Result->DmaTransfers[2], 9, 0, LogicalBase + 4096);
    EXPECT_EQ(Result->DmaTransfers[2].Data, bytes(0x11));
    EXPECT_NE(Result->DmaTransfers[0].Epoch, Result->DmaTransfers[2].Epoch);
    EXPECT_EQ(apiCount(*Result, "AllocateCommonBuffer"), 2u);
    EXPECT_EQ(apiCount(*Result, "FreeCommonBuffer"), 2u);
    EXPECT_EQ(apiCount(*Result, "IoGetDmaAdapter"), 2u);
    EXPECT_EQ(apiCount(*Result, "PutDmaAdapter"), 2u);
  }
}

TEST(DriverWDMDMA, LivePacketMappingRejectsCpuAndPrematureStorageRetirement) {
  for (const auto *Image : images())
    for (char Mode : {'U', 'F', 'G', 'P'}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Mode);
      auto Options = basic(Mode);
      Options.Requests[2] = ioctl(Mode, 0, LogicalBase, false);
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      EXPECT_EQ(Result->Stop, DriverStopReason::ModelError)
          << Result->Diagnostic;
      ASSERT_EQ(Result->Requests.size(), 3u);
      EXPECT_FALSE(Result->Requests[2].Completed);
      EXPECT_EQ(apiCount(*Result, "GetScatterGatherList"), 1u);
      EXPECT_EQ(apiCount(*Result, "PutScatterGatherList"), 0u);
      EXPECT_FALSE(Result->UnloadCompleted);
      EXPECT_LT(messageIndex(*Result, "callback unit=1 packet=0"),
                Result->Messages.size());
    }
}

TEST(DriverWDMDMA, DirectionAndReadLockedMdlCannotGrantDeviceWriteAccess) {
  for (const auto *Image : images()) {
    auto Options = basic('R');
    Options.Requests[2].DmaEvents = {event(DriverDmaDirection::ReadMemory)};
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::ModelError) << Result->Diagnostic;
    ASSERT_EQ(Result->DmaTransfers.size(), 1u);
    EXPECT_TRUE(Result->DmaTransfers[0].FailureReason);
    EXPECT_FALSE(Result->DmaTransfers[0].CompletedAt100ns);
    EXPECT_TRUE(Result->DmaTransfers[0].Data.empty());

    Options = basic('V');
    Options.Requests[2] = ioctl('V', 0, LogicalBase, false);
    Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::ModelError) << Result->Diagnostic;
    EXPECT_NE(Result->Diagnostic.find("write-locked MDL"), std::string::npos);
    EXPECT_EQ(messageIndex(*Result, "callback unit=1 packet=0"),
              Result->Messages.size());
    EXPECT_TRUE(Result->DmaTransfers.empty());
  }
}

TEST(DriverWDMDMA,
     PutRequiresOriginalDirectionAndUnsupportedSlotStaysExplicit) {
  for (const auto *Image : images()) {
    auto Result = emulateDriver(Image, basic('D'));
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::ModelError) << Result->Diagnostic;
    EXPECT_NE(Result->Diagnostic.find("direction"), std::string::npos);
    ASSERT_EQ(Result->DmaTransfers.size(), 1u);
    EXPECT_TRUE(Result->DmaTransfers[0].CompletedAt100ns);
    EXPECT_FALSE(Result->Requests[2].Completed);
    auto Options = basic('T');
    Options.Requests[2] = ioctl('T', 0, LogicalBase, false);
    Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::ModelError) << Result->Diagnostic;
    EXPECT_NE(Result->Diagnostic.find("ReadDmaCounter"), std::string::npos);
    EXPECT_FALSE(Result->Requests[2].Completed);
  }
}

TEST(DriverWDMDMA, StopCannotRetireOutstandingCommonBufferOwnership) {
  for (const auto *Image : images()) {
    auto Options = options('N');
    Options.Requests = {pnp(DevicePnpRequest::Start),
                        pnp(DevicePnpRequest::QueryStop),
                        pnp(DevicePnpRequest::Stop)};
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::ModelError) << Result->Diagnostic;
    EXPECT_FALSE(Result->UnloadCompleted);
    EXPECT_EQ(apiCount(*Result, "FreeCommonBuffer"), 0u);
    EXPECT_EQ(apiCount(*Result, "PutDmaAdapter"), 0u);
    EXPECT_FALSE(Result->Devices.empty());
  }
}

TEST(DriverWDMDMA, InvalidAndPoweredOffTransactionsAreObservedBusFailures) {
  for (const auto *Image : images())
    for (unsigned Case = 0; Case != 3; ++Case) {
      auto Options = basic();
      uint64_t Address = LogicalBase + (Case == 0 ? 32768 : 1048576);
      if (Case == 2) {
        DriverRequest Power;
        Power.Kind = DriverRequestKind::Power;
        Power.DeviceID = "dma0";
        DriverPowerOperation Operation;
        Operation.Minor = DevicePowerRequest::Set;
        Operation.Type = DriverPowerType::Device;
        Operation.State = uint32_t(DevicePowerState::D3);
        Operation.BusCompletion = DriverBusCompletion{0, 0};
        Power.Power = Operation;
        Options.Requests.insert(Options.Requests.begin() + 2, Power);
        Address = LogicalBase;
      }
      Options.Requests[Case == 2 ? 3 : 2].DmaEvents = {
          event(DriverDmaDirection::WriteMemory, 7, Address)};
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      EXPECT_EQ(Result->Stop, DriverStopReason::ModelError)
          << Result->Diagnostic;
      ASSERT_EQ(Result->DmaTransfers.size(), 1u);
      EXPECT_TRUE(Result->DmaTransfers[0].OccurredAt100ns);
      EXPECT_TRUE(Result->DmaTransfers[0].FailureReason);
      EXPECT_FALSE(Result->DmaTransfers[0].CompletedAt100ns);
      EXPECT_TRUE(Result->DmaTransfers[0].Data.empty());
    }
}

TEST(DriverWDMDMA, LateEventSurvivesSourceCompletionAndCannotReuseReleasedMap) {
  for (const auto *Image : images()) {
    auto Options = basic('I');
    Options.Requests[2].DmaEvents = {event(DriverDmaDirection::ReadMemory)};
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::ModelError) << Result->Diagnostic;
    ASSERT_GE(Result->Requests.size(), 3u);
    EXPECT_TRUE(Result->Requests[2].Completed);
    EXPECT_EQ(Result->Requests[2].IOStatus, 0u);
    ASSERT_EQ(Result->DmaTransfers.size(), 1u);
    EXPECT_TRUE(Result->DmaTransfers[0].FailureReason);
    EXPECT_FALSE(Result->DmaTransfers[0].CompletedAt100ns);
    EXPECT_TRUE(Result->DmaTransfers[0].Data.empty());
    EXPECT_LT(messageIndex(*Result, "callback completed before return unit=1"),
              messageIndex(*Result, "get returned unit=1 packet=0"));
  }
}

TEST(DriverWDMDMA, FailedLowerStartNeverAllocatesCommonStorage) {
  for (const auto *Image : images()) {
    auto Options = options();
    Options.Requests = {pnp(DevicePnpRequest::Start, 0, 0xc0000001),
                        pnp(DevicePnpRequest::Remove)};
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    ASSERT_EQ(Result->Requests.size(), 2u);
    EXPECT_EQ(Result->Requests[0].IOStatus, 0xc0000001u);
    EXPECT_EQ(apiCount(*Result, "IoGetDmaAdapter"), 1u);
    EXPECT_EQ(apiCount(*Result, "AllocateCommonBuffer"), 0u);
    EXPECT_EQ(apiCount(*Result, "PutDmaAdapter"), 1u);
  }
}
TEST(DriverWDMDMA, FailedAddDeviceMustReleaseAdapterBeforeProviderRetirement) {
  for (const auto *Image : images()) {
    auto Result = emulateDriver(Image, options('A'));
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::ModelError) << Result->Diagnostic;
    EXPECT_NE(Result->Diagnostic.find("DMA adapter"), std::string::npos);
    ASSERT_EQ(Result->PnpDevices.size(), 1u);
    EXPECT_EQ(Result->PnpDevices[0].AddDeviceStatus, 0xc0000001u);
    EXPECT_TRUE(Result->PnpDevices[0].ProviderPresent);
    EXPECT_TRUE(Result->Devices.empty());
    EXPECT_FALSE(Result->UnloadCompleted);
    EXPECT_EQ(apiCount(*Result, "IoGetDmaAdapter"), 1u);
    EXPECT_EQ(apiCount(*Result, "PutDmaAdapter"), 0u);
    noGuestFailures(*Result);

    Result = emulateDriver(Image, options('B'));
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    ASSERT_EQ(Result->PnpDevices.size(), 1u);
    EXPECT_EQ(Result->PnpDevices[0].AddDeviceStatus, 0xc0000001u);
    EXPECT_FALSE(Result->PnpDevices[0].ProviderPresent);
    EXPECT_TRUE(Result->Devices.empty());
    EXPECT_TRUE(Result->UnloadCompleted);
    EXPECT_EQ(apiCount(*Result, "PutDmaAdapter"), 1u);
    noGuestFailures(*Result);
  }
}
#else
TEST(DriverWDMDMA, GenuineFixtureUnavailable) {
  GTEST_SKIP() << "Set NEVERD_WDM_DMA_FIXTURE to an original driver built with "
                  "genuine WDK headers/libraries; no substitute stub is used";
}
#endif
} // namespace
} // namespace neverd::emulation
