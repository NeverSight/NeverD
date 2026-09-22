//===- KernelDMAEvents.cpp - Explicit device transactions on guest RAM ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// External transactions capture the submitted resource epoch, then resolve a
/// complete live logical mapping at delivery. Bytes stay in physical backing;
/// transactions neither synthesize interrupts nor complete guest requests.
///
//===----------------------------------------------------------------------===//

#include "KernelDMA.h"

#include <algorithm>
#include <tuple>

namespace neverd::emulation {
namespace {
llvm::Error dmaEventError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "DMA event: " + Message);
}
} // namespace

llvm::Expected<KernelDMA::Event>
KernelDMA::resolveEvent(const DriverDmaEvent &Input, uint64_t Now) const {
  if (Input.After100ns > INT64_MAX || Input.After100ns > UINT64_MAX - Now)
    return dmaEventError("deadline exceeds the virtual time range");
  if (!Input.Length || Input.Length > DriverDmaMaximumLengthLimit ||
      Input.Length > UINT64_MAX - Input.LogicalAddress)
    return dmaEventError("invalid logical byte extent");
  switch (Input.Direction) {
  case DriverDmaDirection::ReadMemory:
    if (!Input.Data.empty())
      return dmaEventError("read transaction cannot provide bytes");
    break;
  case DriverDmaDirection::WriteMemory:
    if (Input.Data.size() != Input.Length)
      return dmaEventError("write transaction requires its exact byte count");
    break;
  default:
    return dmaEventError("unknown device transaction direction");
  }
  for (const auto &[PDO, Device] : Resources.devices()) {
    if (Device.ID != Input.DeviceID)
      continue;
    if (!Device.Dma || !Device.Assigned || !Device.Present)
      return dmaEventError(
          "submission requires an assigned present DMA device");
    return Event{PDO, Device.Epoch, Input, Now + Input.After100ns, 0};
  }
  return dmaEventError("transaction names an unknown DMA device");
}

llvm::Error KernelDMA::canArm(llvm::ArrayRef<DriverDmaEvent> Inputs,
                              size_t SourceIndex, uint64_t Now) const {
  if (SourceIndex > UINT32_MAX ||
      Inputs.size() > DriverScenarioDmaEventsPerRequestLimit ||
      Inputs.size() >
          DriverScenarioDmaEventLimit -
              std::min(Result.DmaTransfers.size(), DriverScenarioDmaEventLimit))
    return dmaEventError("transaction observation capacity exhausted");
  uint64_t Bytes = ObservedBytes;
  for (const auto &Input : Inputs) {
    auto Event = resolveEvent(Input, Now);
    if (!Event)
      return Event.takeError();
    if (Bytes > DriverScenarioDmaBytesLimit ||
        Input.Length > DriverScenarioDmaBytesLimit - Bytes)
      return dmaEventError("transaction observation byte limit exhausted");
    Bytes += Input.Length;
  }
  return llvm::Error::success();
}

llvm::Error KernelDMA::arm(llvm::ArrayRef<DriverDmaEvent> Inputs,
                           size_t SourceIndex, uint64_t Now) {
  if (auto E = canArm(Inputs, SourceIndex, Now))
    return E;
  std::vector<Event> Prepared;
  Prepared.reserve(Inputs.size());
  for (const auto &Input : Inputs) {
    auto Event = resolveEvent(Input, Now);
    if (!Event)
      return Event.takeError();
    Prepared.push_back(std::move(*Event));
  }
  for (size_t I = 0; I < Prepared.size(); ++I) {
    auto &Event = Prepared[I];
    Event.Observation = Result.DmaTransfers.size();
    DriverDmaResult Observation;
    Observation.SourceRequestIndex = uint32_t(SourceIndex);
    Observation.EventIndex = uint32_t(I);
    Observation.DeviceID = Event.Input.DeviceID;
    Observation.Epoch = Event.Epoch;
    Observation.DueAt100ns = Event.Due;
    Observation.LogicalAddress = Event.Input.LogicalAddress;
    Observation.Length = Event.Input.Length;
    Observation.Direction = Event.Input.Direction;
    Result.DmaTransfers.push_back(std::move(Observation));
    ObservedBytes += Event.Input.Length;
    Events.emplace(Event.Observation, std::move(Event));
  }
  return llvm::Error::success();
}

std::optional<uint64_t> KernelDMA::nextEventTime() const {
  std::optional<uint64_t> Time;
  for (const auto &[Index, Event] : Events) {
    (void)Index;
    if (!Time || Event.Due < *Time)
      Time = Event.Due;
  }
  return Time;
}

llvm::Error KernelDMA::executeEvent(Event &Event, uint64_t Now) {
  auto &Observation = Result.DmaTransfers[Event.Observation];
  if (Observation.FailureReason)
    return dmaEventError(*Observation.FailureReason);
  Observation.OccurredAt100ns = Now;
  auto Fail = [&](const llvm::Twine &Message) {
    Observation.FailureReason = Message.str();
    return dmaEventError(Message);
  };
  const auto *Device = Resources.find(Event.PDO);
  if (!Device || !Device->Assigned || !Device->Present ||
      Device->Epoch != Event.Epoch)
    return Fail("transaction targets an unavailable resource epoch");
  if (Device->Power != DevicePowerState::D0)
    return Fail("transaction requires physical device power D0");

  const auto &Input = Event.Input;
  const Mapping *Selected = nullptr;
  for (const auto &[Object, Map] : Mappings) {
    (void)Object;
    if (Map.PDO != Event.PDO || !Map.Live || Input.LogicalAddress < Map.Logical)
      continue;
    const uint64_t Offset = Input.LogicalAddress - Map.Logical;
    if (Offset >= Map.Length || Input.Length > Map.Length - Offset)
      continue;
    if (Selected)
      return Fail("logical interval resolves to ambiguous DMA mappings");
    Selected = &Map;
  }
  if (!Selected)
    return Fail("transaction requires one complete live logical mapping");
  Observation.Mapping = Selected->Object;
  Observation.Adapter = Selected->Adapter;
  if (!adapter(Selected->Adapter))
    return Fail("transaction lost its live DMA adapter");
  if (!Selected->Common && Selected->Direction != Input.Direction)
    return Fail("transaction direction does not match its packet DMA map");

  const uint64_t Offset = Input.LogicalAddress - Selected->Logical;
  std::vector<uint8_t> Bytes(Input.Length);
  if (Input.Direction == DriverDmaDirection::ReadMemory) {
    if (auto E = Physical.read(Selected->Pin, Offset, Bytes))
      return Fail(llvm::toString(std::move(E)));
  } else {
    if (auto E = Physical.write(Selected->Pin, Offset, Input.Data))
      return Fail(llvm::toString(std::move(E)));
    Bytes = Input.Data;
  }
  Observation.Data = std::move(Bytes);
  Observation.CompletedAt100ns = Now;
  return llvm::Error::success();
}

llvm::Error KernelDMA::processEvents(uint64_t Now) {
  while (true) {
    auto Selected = Events.end();
    for (auto I = Events.begin(); I != Events.end(); ++I)
      if (I->second.Due <= Now &&
          (Selected == Events.end() ||
           std::tie(I->second.Due, I->first) <
               std::tie(Selected->second.Due, Selected->first)))
        Selected = I;
    if (Selected == Events.end())
      return llvm::Error::success();
    if (auto E = executeEvent(Selected->second, Now))
      return E;
    Events.erase(Selected);
  }
}

} // namespace neverd::emulation
