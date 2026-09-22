//===- DriverWDMDMAChannelTests.cpp - Genuine WDK DMA channels
//------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Execute channel callback actions and explicit RAM transactions through
/// original drivers built against genuine WDK headers and import libraries.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/emulation/DriverSession.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
#ifdef NEVERD_WDM_DMA_CHANNEL_FIXTURE
constexpr uint64_t LogicalBase = 0x40000000;

std::vector<const char *> images() {
  std::vector<const char *> Images{NEVERD_WDM_DMA_CHANNEL_FIXTURE};
#ifdef NEVERD_WDM_DMA_CHANNEL_CFG_FIXTURE
  Images.push_back(NEVERD_WDM_DMA_CHANNEL_CFG_FIXTURE);
#endif
  return Images;
}

std::vector<uint8_t> bytes(uint8_t Start) {
  std::vector<uint8_t> Result;
  for (unsigned I = 0; I != 32; ++I)
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
      DriverDmaConfig{64, 8192, Registers, 1, LogicalBase, 262144, true};
  return Device;
}

DriverOptions options(char Mode = 'S') {
  DriverOptions Options;
  Options.ServiceName = std::string("NeverDChannel") + Mode;
  Options.LoadAddress = 0x190000000;
  Options.Unload = true;
  Options.PnpDevices.push_back(device(0, Mode == 'Q' ? 3 : 4));
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

DriverDmaEvent event(DriverDmaDirection Direction, uint64_t Delay,
                     uint64_t Address, llvm::ArrayRef<uint8_t> Data,
                     unsigned Index = 0) {
  DriverDmaEvent Event;
  Event.After100ns = Delay;
  Event.DeviceID = "dma" + std::to_string(Index);
  Event.LogicalAddress = Address;
  Event.Direction = Direction;
  Event.Length = uint32_t(Data.size());
  if (Direction == DriverDmaDirection::WriteMemory)
    Event.Data.assign(Data.begin(), Data.end());
  return Event;
}

DriverRequest ioctl(char Mode = 'S', unsigned Index = 0,
                    uint64_t Base = LogicalBase) {
  auto Request = file(DriverRequestKind::DeviceControl, Index);
  Request.ControlCode = Mode == 'J' ? 0x222002 : 0x222000;
  Request.OutputSize = 48;
  if (Mode == 'I' || Mode == 'X')
    return Request;
  auto Direction = Mode == 'R' ? DriverDmaDirection::ReadMemory
                               : DriverDmaDirection::WriteMemory;
  const auto Data = bytes(0xa0);
  auto AddOperation = [&](uint64_t Delay, uint64_t OperationBase,
                          llvm::ArrayRef<uint8_t> Content) {
    Request.InterruptEvents.push_back(
        {Delay, "dma" + std::to_string(Index), "line0"});
    if (Mode == 'N') {
      Request.DmaEvents.push_back(
          event(Direction, Delay, OperationBase + 4088, Content, Index));
    } else {
      Request.DmaEvents.push_back(event(Direction, Delay, OperationBase + 4088,
                                        Content.take_front(8), Index));
      Request.DmaEvents.push_back(event(Direction, Delay, OperationBase + 4096,
                                        Content.drop_front(8), Index));
    }
  };
  if (Mode == 'Q') {
    Request.DmaEvents.push_back(
        event(DriverDmaDirection::ReadMemory, 5, Base,
              llvm::ArrayRef<uint8_t>(Data).take_front(16), Index));
    Base += 8192;
  }
  AddOperation(7, Base, Data);
  if (Mode == 'U')
    AddOperation(14, Base + 8192, bytes(0xb0));
  return Request;
}

void finish(DriverOptions &Options, unsigned Index = 0) {
  Options.Requests.push_back(file(DriverRequestKind::Cleanup, Index));
  Options.Requests.push_back(file(DriverRequestKind::Close, Index));
  Options.Requests.push_back(pnp(DevicePnpRequest::QueryRemove, 0, 0, Index));
  Options.Requests.push_back(pnp(DevicePnpRequest::Remove, 3, 0, Index));
}

DriverOptions basic(char Mode = 'S') {
  auto Options = options(Mode);
  Options.Requests.push_back(pnp(DevicePnpRequest::Start, 11));
  Options.Requests.push_back(file(DriverRequestKind::Create));
  Options.Requests.push_back(ioctl(Mode));
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
    EXPECT_EQ(Message.find("WDM channel: failure"), std::string::npos)
        << Message;
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
              uint32_t Callbacks, uint32_t Interrupts, uint32_t Operations,
              llvm::ArrayRef<uint8_t> Data) {
  std::vector<uint8_t> Expected;
  for (uint32_t Value : {Unit, Callbacks, Interrupts, Operations})
    for (unsigned Shift = 0; Shift != 32; Shift += 8)
      Expected.push_back(uint8_t(Value >> Shift));
  Expected.insert(Expected.end(), Data.begin(), Data.end());
  EXPECT_EQ(Request.DispatchStatus, 0x103u);
  EXPECT_EQ(Request.IOStatus, 0u);
  EXPECT_EQ(Request.Information, 48u);
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

TEST(DriverWDMDMAChannel, ScatterFragmentsExecuteWithNormalCfgAndRebasing) {
  for (const auto *Image : images())
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL})
      for (char Mode : {'S', 'R', 'H'}) {
        SCOPED_TRACE(Image);
        SCOPED_TRACE(Mode);
        auto Options = basic(Mode);
        Options.LoadAddress = Address;
        auto Result = emulateDriver(Image, Options);
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        clean(*Result);
        ASSERT_EQ(Result->Requests.size(), 7u);
        snapshot(Result->Requests[2], 1, 1, 1, 1,
                 bytes(Mode == 'R' ? 0x21 : 0xa0));
        ASSERT_EQ(Result->DmaTransfers.size(), 2u);
        transaction(Result->DmaTransfers[0], 2, 0, LogicalBase + 4088);
        transaction(Result->DmaTransfers[1], 2, 1, LogicalBase + 4096);
        EXPECT_EQ(Result->DmaTransfers[0].Length, 8u);
        EXPECT_EQ(Result->DmaTransfers[1].Length, 24u);
        const auto Data = bytes(Mode == 'R' ? 0x21 : 0xa0);
        EXPECT_EQ(Result->DmaTransfers[0].Data,
                  std::vector<uint8_t>(Data.begin(), Data.begin() + 8));
        EXPECT_EQ(Result->DmaTransfers[1].Data,
                  std::vector<uint8_t>(Data.begin() + 8, Data.end()));
        ASSERT_EQ(Result->Interrupts.size(), 1u);
        EXPECT_EQ(Result->Interrupts[0].ReturnValue, 1u);
        EXPECT_EQ(apiCount(*Result, "AllocateAdapterChannel"), 1u);
        EXPECT_EQ(apiCount(*Result, "MapTransfer"), 2u);
        EXPECT_EQ(apiCount(*Result, "FlushAdapterBuffers"), 1u);
        EXPECT_EQ(apiCount(*Result, "FreeMapRegisters"), 1u);
        EXPECT_EQ(apiCount(*Result, "KeFlushIoBuffers"), 1u);
        EXPECT_LT(messageIndex(*Result, "callback unit=1"),
                  messageIndex(*Result, "allocate returned unit=1"));
        EXPECT_LT(messageIndex(*Result, "flushed unit=1"),
                  messageIndex(*Result, "freed registers unit=1"));
      }
}

TEST(DriverWDMDMAChannel, NonScatterMappingPreservesRequestedWholeLength) {
  for (const auto *Image : images()) {
    auto Options = basic('N');
    Options.PnpDevices[0].Dma->ScatterGather = false;
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    snapshot(Result->Requests[2], 1, 1, 1, 1, bytes(0xa0));
    ASSERT_EQ(Result->DmaTransfers.size(), 1u);
    EXPECT_EQ(Result->DmaTransfers[0].Length, 32u);
    transaction(Result->DmaTransfers[0], 2, 0, LogicalBase + 4088);
    EXPECT_EQ(apiCount(*Result, "MapTransfer"), 1u);
    EXPECT_EQ(apiCount(*Result, "FlushAdapterBuffers"), 1u);
  }
}

TEST(DriverWDMDMAChannel, AggregateFlushAllowsRetainedRegisterReuse) {
  for (const auto *Image : images()) {
    auto Result = emulateDriver(Image, basic('U'));
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    snapshot(Result->Requests[2], 1, 1, 2, 2, bytes(0xb0));
    ASSERT_EQ(Result->DmaTransfers.size(), 4u);
    transaction(Result->DmaTransfers[2], 2, 2, LogicalBase + 12280);
    transaction(Result->DmaTransfers[3], 2, 3, LogicalBase + 12288);
    EXPECT_EQ(Result->DmaTransfers[0].Mapping, Result->DmaTransfers[2].Mapping);
    EXPECT_NE(Result->DmaTransfers[0].LogicalAddress,
              Result->DmaTransfers[2].LogicalAddress);
    EXPECT_EQ(Result->DmaTransfers[0].Adapter, Result->DmaTransfers[2].Adapter);
    EXPECT_EQ(apiCount(*Result, "AllocateAdapterChannel"), 1u);
    EXPECT_EQ(apiCount(*Result, "MapTransfer"), 4u);
    EXPECT_EQ(apiCount(*Result, "FlushAdapterBuffers"), 2u);
    EXPECT_EQ(apiCount(*Result, "FreeMapRegisters"), 1u);
    EXPECT_EQ(apiCount(*Result, "KeFlushIoBuffers"), 2u);
  }
}

TEST(DriverWDMDMAChannel,
     SharedCommonAndScatterQuotaPreservesQueuedIrpSnapshot) {
  for (const auto *Image : images()) {
    auto Result = emulateDriver(Image, basic('Q'));
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    snapshot(Result->Requests[2], 1, 1, 1, 1, bytes(0xa0));
    EXPECT_LT(messageIndex(*Result, "SG callback unit=1"),
              messageIndex(*Result, "queued with original IRP; field cleared"));
    EXPECT_LT(messageIndex(*Result, "allocate returned unit=1"),
              messageIndex(*Result, "callback unit=1 snapshot=1"));
    ASSERT_EQ(Result->DmaTransfers.size(), 3u);
    const auto Common = bytes(0x80);
    EXPECT_EQ(Result->DmaTransfers[0].Data,
              std::vector<uint8_t>(Common.begin(), Common.begin() + 16));
    transaction(Result->DmaTransfers[0], 2, 0, LogicalBase);
    transaction(Result->DmaTransfers[1], 2, 1, LogicalBase + 12280);
    transaction(Result->DmaTransfers[2], 2, 2, LogicalBase + 12288);
    EXPECT_EQ(apiCount(*Result, "GetScatterGatherList"), 1u);
    EXPECT_EQ(apiCount(*Result, "PutScatterGatherList"), 1u);
    EXPECT_EQ(apiCount(*Result, "AllocateCommonBuffer"), 1u);
    EXPECT_EQ(apiCount(*Result, "FreeCommonBuffer"), 1u);
  }
}

TEST(DriverWDMDMAChannel, CallbackMayCompleteBeforeDeallocateActionReturns) {
  for (const auto *Image : images()) {
    auto Result = emulateDriver(Image, basic('I'));
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    snapshot(Result->Requests[2], 1, 1, 0, 0, bytes(0x21));
    EXPECT_LT(messageIndex(*Result, "callback completed before action"),
              messageIndex(*Result, "allocate returned unit=1"));
    EXPECT_EQ(apiCount(*Result, "MapTransfer"), 0u);
    EXPECT_EQ(apiCount(*Result, "FreeMapRegisters"), 0u);
    EXPECT_EQ(apiCount(*Result, "PutDmaAdapter"), 1u);
    EXPECT_TRUE(Result->DmaTransfers.empty());
  }
}

TEST(DriverWDMDMAChannel, ExcessCountFailsWithoutCallingAdapterControl) {
  for (const auto *Image : images()) {
    auto Result = emulateDriver(Image, basic('X'));
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    snapshot(Result->Requests[2], 1, 0, 0, 0, bytes(0x21));
    EXPECT_EQ(apiCount(*Result, "AllocateAdapterChannel"), 1u);
    EXPECT_EQ(apiCount(*Result, "MapTransfer"), 0u);
    EXPECT_EQ(messageIndex(*Result, "callback unit="), Result->Messages.size());
  }
}

TEST(DriverWDMDMAChannel, LogicalAperturesAreIndependentAcrossPdos) {
  for (const auto *Image : images()) {
    auto Options = options();
    Options.PnpDevices.push_back(device(1));
    for (unsigned Index = 0; Index != 2; ++Index) {
      Options.Requests.push_back(pnp(DevicePnpRequest::Start, 3, 0, Index));
      Options.Requests.push_back(file(DriverRequestKind::Create, Index));
      Options.Requests.push_back(ioctl('S', Index));
      finish(Options, Index);
    }
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result, 2);
    snapshot(Result->Requests[2], 1, 1, 1, 1, bytes(0xa0));
    snapshot(Result->Requests[9], 2, 1, 1, 1, bytes(0xa0));
    ASSERT_EQ(Result->DmaTransfers.size(), 4u);
    EXPECT_EQ(Result->DmaTransfers[0].LogicalAddress,
              Result->DmaTransfers[2].LogicalAddress);
    EXPECT_NE(Result->DmaTransfers[0].Adapter, Result->DmaTransfers[2].Adapter);
    EXPECT_NE(Result->DmaTransfers[0].DeviceID,
              Result->DmaTransfers[2].DeviceID);
  }
}

TEST(DriverWDMDMAChannel, StopRestartReleasesReservationAndUsesFreshAddresses) {
  for (const auto *Image : images()) {
    auto Options = options();
    Options.Requests = {pnp(DevicePnpRequest::Start),
                        file(DriverRequestKind::Create),
                        ioctl(),
                        pnp(DevicePnpRequest::QueryStop),
                        pnp(DevicePnpRequest::Stop),
                        pnp(DevicePnpRequest::Start),
                        ioctl('S', 0, LogicalBase + 8192)};
    finish(Options);
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    snapshot(Result->Requests[6], 1, 2, 2, 2, bytes(0xa0));
    ASSERT_EQ(Result->DmaTransfers.size(), 4u);
    EXPECT_EQ(Result->DmaTransfers[2].LogicalAddress, LogicalBase + 12280);
    EXPECT_EQ(apiCount(*Result, "IoGetDmaAdapter"), 2u);
    EXPECT_EQ(apiCount(*Result, "PutDmaAdapter"), 2u);
    EXPECT_EQ(apiCount(*Result, "FreeMapRegisters"), 2u);
  }
}

TEST(DriverWDMDMAChannel, InvalidActionsAndCallbackReentryFailExplicitly) {
  for (const auto *Image : images())
    for (char Mode : {'A', 'K', 'F', 'E'}) {
      SCOPED_TRACE(Mode);
      auto Result = emulateDriver(Image, basic(Mode));
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      EXPECT_EQ(Result->Stop, DriverStopReason::ModelError)
          << Result->Diagnostic;
      EXPECT_FALSE(Result->Diagnostic.empty());
      ASSERT_EQ(Result->Requests.size(), 3u);
      EXPECT_FALSE(Result->Requests[2].Completed);
      EXPECT_EQ(apiCount(*Result, "MapTransfer"), 0u);
      noGuestFailures(*Result);
    }
}

TEST(DriverWDMDMAChannel, PartialFlushWrongDirectionAndPartialFreeAreRejected) {
  for (const auto *Image : images())
    for (char Mode : {'P', 'D', 'C'}) {
      SCOPED_TRACE(Mode);
      auto Result = emulateDriver(Image, basic(Mode));
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      EXPECT_EQ(Result->Stop, DriverStopReason::ModelError)
          << Result->Diagnostic;
      ASSERT_EQ(Result->DmaTransfers.size(), 2u);
      for (const auto &Transfer : Result->DmaTransfers) {
        EXPECT_TRUE(Transfer.CompletedAt100ns);
        EXPECT_FALSE(Transfer.FailureReason);
      }
      EXPECT_FALSE(Result->Requests[2].Completed);
      EXPECT_EQ(messageIndex(*Result, "freed registers unit="),
                Result->Messages.size());
      noGuestFailures(*Result);
    }
}

TEST(DriverWDMDMAChannel, LiveMappingProtectsMdlPoolIrpAndCpuOwnership) {
  for (const auto *Image : images())
    for (char Mode : {'M', 'B', 'J', 'V'}) {
      SCOPED_TRACE(Mode);
      auto Result = emulateDriver(Image, basic(Mode));
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      EXPECT_EQ(Result->Stop, DriverStopReason::ModelError)
          << Result->Diagnostic;
      EXPECT_FALSE(Result->Requests[2].Completed);
      EXPECT_EQ(apiCount(*Result, "FlushAdapterBuffers"), 0u);
      noGuestFailures(*Result);
    }
}

TEST(DriverWDMDMAChannel, NonScatterDoesNotSilentlyShortenOversizedOperation) {
  for (const auto *Image : images()) {
    auto Options = basic('O');
    Options.PnpDevices[0].Dma->ScatterGather = false;
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::ModelError) << Result->Diagnostic;
    EXPECT_EQ(apiCount(*Result, "FlushAdapterBuffers"), 0u);
    EXPECT_EQ(messageIndex(*Result, "mapped unit="), Result->Messages.size());
    noGuestFailures(*Result);
  }
}

TEST(DriverWDMDMAChannel, FailedLowerStartDoesNotAllocateOrMapChannel) {
  for (const auto *Image : images()) {
    auto Options = options();
    Options.Requests = {pnp(DevicePnpRequest::Start, 3, 0xc0000001),
                        pnp(DevicePnpRequest::Remove)};
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    EXPECT_EQ(Result->Requests[0].IOStatus, 0xc0000001u);
    EXPECT_EQ(apiCount(*Result, "AllocateAdapterChannel"), 0u);
    EXPECT_EQ(apiCount(*Result, "MapTransfer"), 0u);
    EXPECT_EQ(apiCount(*Result, "PutDmaAdapter"), 1u);
  }
}
#else
TEST(DriverWDMDMAChannel, GenuineFixtureUnavailable) {
  GTEST_SKIP() << "Set NEVERD_WDM_DMA_CHANNEL_FIXTURE to the original driver "
                  "built with genuine WDK headers and libraries";
}
#endif
} // namespace
} // namespace neverd::emulation
