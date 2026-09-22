//===- KernelDMAChannels.cpp - Channel reservations and aggregate maps ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bus-master channel callbacks retain map-register reservations independently
/// of each operation's pinned bytes. All admission uses the existing PDO FIFO.
///
//===----------------------------------------------------------------------===//

#include "KernelDMA.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
llvm::Error channelError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "DMA channel: " + Message);
}
uint64_t spanPages(uint64_t Backing, uint64_t Length) {
  return ((Backing & (DriverDmaPageSize - 1)) + Length + DriverDmaPageSize -
          1) /
         DriverDmaPageSize;
}
} // namespace

std::optional<KernelDMA::CallbackInfo>
KernelDMA::callbackInfo(uint64_t Object) const {
  if (const auto *Map = callback(Object))
    return CallbackInfo{CallbackKind::ScatterGather,
                        Object,
                        Map->Adapter,
                        Map->PDO,
                        Map->Device,
                        Map->DeviceSize,
                        Map->Routine,
                        Map->Context,
                        0,
                        0,
                        Map->SchedulerID};
  const auto I = Channels.find(Object);
  if (I == Channels.end() || I->second.Returned)
    return std::nullopt;
  const auto &R = I->second.Request;
  return CallbackInfo{CallbackKind::AdapterControl,
                      Object,
                      R.Adapter,
                      I->second.PDO,
                      R.Device,
                      R.DeviceSize,
                      R.Routine,
                      R.Context,
                      R.CurrentIRP,
                      R.IRPSize,
                      R.SchedulerID};
}

const KernelDMA::Channel *KernelDMA::channel(uint64_t Object) const {
  const auto I = Channels.find(Object);
  return I == Channels.end() || I->second.Released ? nullptr : &I->second;
}

llvm::Expected<std::optional<KernelDMA::Channel>>
KernelDMA::planChannel(const ChannelRequest &R) const {
  const auto *Adapter = adapter(R.Adapter);
  if (!Adapter)
    return channelError("allocation requires a live adapter");
  if (!R.Object || R.Object > UINT64_MAX - dma::ChannelTokenSize ||
      Channels.count(R.Object) || Mappings.count(R.Object) ||
      RetiredMappings.count(R.Object) || !R.Device || !R.DeviceSize ||
      R.DeviceSize > UINT64_MAX - R.Device || !R.Routine || !R.Registers ||
      bool(R.CurrentIRP) != bool(R.IRPSize) ||
      R.IRPSize > UINT64_MAX - R.CurrentIRP)
    return channelError("invalid or reused allocation/callback identity");
  for (const auto &[Object, C] : Channels) {
    (void)Object;
    if (C.Begun && !C.Returned)
      return channelError(
          "AllocateAdapterChannel cannot run inside AdapterControl");
    if (!C.Returned && C.Request.Device == R.Device)
      return channelError(
          "device already owns an outstanding allocation callback");
  }
  if (R.Registers > Adapter->MapRegisters ||
      Channels.size() >= dma::MaxChannels)
    return std::optional<Channel>{};
  const auto &Domain = Domains.at(Adapter->PDO);
  const auto &Config = *Resources.find(Adapter->PDO)->Dma;
  Channel Plan;
  Plan.Request = R;
  Plan.PDO = Adapter->PDO;
  Plan.Live = Domain.Waiting.empty() &&
              R.Registers <= Config.MapRegisters - Domain.UsedRegisters;
  return std::optional<Channel>{Plan};
}

llvm::Error KernelDMA::canPublishChannel(const Channel &Plan) const {
  auto Current = planChannel(Plan.Request);
  if (!Current)
    return Current.takeError();
  if (!*Current || !Plan.Request.SchedulerID || Plan.PDO != (**Current).PDO ||
      Plan.Live != (**Current).Live || Plan.Begun || Plan.Returned ||
      Plan.Retained || Plan.Released || Plan.OperationSerial ||
      Plan.InitialVA || Plan.Revision)
    return channelError("allocation plan lost its reserved callback or quota");
  return llvm::Error::success();
}

llvm::Error KernelDMA::publishChannel(const Channel &Plan) {
  if (auto E = canPublishChannel(Plan))
    return E;
  auto &Domain = Domains.at(Plan.PDO);
  if (Plan.Live)
    Domain.UsedRegisters += Plan.Request.Registers;
  else
    Domain.Waiting.push_back({CallbackKind::AdapterControl, Plan.Request.Object,
                              Plan.Request.SchedulerID});
  Channels.emplace(Plan.Request.Object, Plan);
  return llvm::Error::success();
}

llvm::Expected<std::vector<KernelDMA::Promotion>>
KernelDMA::planPromotions(uint64_t PDO, uint32_t Releasing) const {
  const auto &Domain = Domains.at(PDO);
  const auto &Config = *Resources.find(PDO)->Dma;
  if (Releasing > Domain.UsedRegisters)
    return channelError("release exceeds shared register ownership");
  uint64_t Free = Config.MapRegisters - Domain.UsedRegisters + Releasing;
  std::vector<Promotion> Ready;
  for (const auto &Waiter : Domain.Waiting) {
    auto Callback = callbackInfo(Waiter.Object);
    if (!Callback || Callback->Kind != Waiter.Kind || Callback->PDO != PDO ||
        Callback->SchedulerID != Waiter.SchedulerID)
      return channelError("FIFO waiter lost its reserved callback identity");
    uint32_t Registers;
    if (Waiter.Kind == CallbackKind::ScatterGather) {
      const auto *Map = mapping(Waiter.Object);
      if (!Map || Map->Live || Map->Common || Map->Channel ||
          !Physical.find(Map->Owner))
        return channelError("SG waiter lost its pinned physical view");
      Registers = Map->Registers;
    } else {
      const auto *C = channel(Waiter.Object);
      if (!C || C->Live || C->Begun || C->Returned)
        return channelError("channel waiter lost its pending reservation");
      Registers = C->Request.Registers;
    }
    if (Registers > Free)
      break;
    Ready.push_back(Waiter);
    Free -= Registers;
  }
  return Ready;
}

void KernelDMA::promote(uint64_t PDO, llvm::ArrayRef<Promotion> Ready) {
  auto &Domain = Domains.at(PDO);
  for (const auto &Waiter : Ready) {
    if (Waiter.Kind == CallbackKind::ScatterGather) {
      auto &Map = Mappings.at(Waiter.Object);
      Map.Live = true;
      Domain.UsedRegisters += Map.Registers;
      Callbacks.at(Waiter.Object).Arguments.Live = true;
    } else {
      auto &C = Channels.at(Waiter.Object);
      C.Live = true;
      Domain.UsedRegisters += C.Request.Registers;
    }
    Domain.Waiting.pop_front();
  }
}

llvm::Expected<KernelDMA::ChannelReturnPlan>
KernelDMA::planChannelReturn(uint64_t Object, uint32_t Action) const {
  const auto *C = channel(Object);
  if (!C || !C->Live || !C->Begun || C->Returned)
    return channelError("return requires its entered allocation callback");
  if (Action == dma::KeepObject)
    return channelError("KeepObject requires an unmodeled system DMA channel");
  if (Action != dma::DeallocateObject &&
      Action != dma::DeallocateObjectKeepRegisters)
    return channelError("unknown IO_ALLOCATION_ACTION return value");
  ChannelReturnPlan Plan{Object, C->Revision, Action, {}};
  if (Action == dma::DeallocateObject) {
    if (mapping(Object))
      return channelError(
          "DeallocateObject requires flushing its active operation");
    auto Ready = planPromotions(C->PDO, C->Request.Registers);
    if (!Ready)
      return Ready.takeError();
    Plan.Ready = std::move(*Ready);
  }
  return Plan;
}

llvm::Error KernelDMA::finishChannelReturn(const ChannelReturnPlan &Plan) {
  auto Current = planChannelReturn(Plan.Object, Plan.Action);
  if (!Current)
    return Current.takeError();
  if (Current->Ready != Plan.Ready || Current->Revision != Plan.Revision)
    return channelError("return promotions changed after preflight");
  auto &C = Channels.at(Plan.Object);
  C.Returned = true;
  if (Plan.Action == dma::DeallocateObjectKeepRegisters) {
    C.Retained = true;
  } else {
    C.Released = true;
    Domains.at(C.PDO).UsedRegisters -= C.Request.Registers;
    promote(C.PDO, Plan.Ready);
  }
  return llvm::Error::success();
}

llvm::Expected<KernelDMA::RegisterReleasePlan>
KernelDMA::planFreeRegisters(uint64_t Adapter, uint64_t Object,
                             uint32_t Registers) const {
  const auto *C = channel(Object);
  if (!C || !adapter(Adapter) || C->Request.Adapter != Adapter ||
      C->Request.Registers != Registers || !C->Returned || !C->Retained)
    return channelError("free requires the exact retained adapter/token/count");
  if (mapping(Object))
    return channelError("FreeMapRegisters requires a complete aggregate flush");
  auto Ready = planPromotions(C->PDO, Registers);
  if (!Ready)
    return Ready.takeError();
  return RegisterReleasePlan{Adapter, Object, C->Revision, Registers,
                             std::move(*Ready)};
}

llvm::Error KernelDMA::freeRegisters(const RegisterReleasePlan &Plan) {
  auto Current = planFreeRegisters(Plan.Adapter, Plan.Object, Plan.Registers);
  if (!Current)
    return Current.takeError();
  if (Current->Ready != Plan.Ready || Current->Revision != Plan.Revision)
    return channelError("free promotions changed after preflight");
  auto &C = Channels.at(Plan.Object);
  C.Released = true;
  Domains.at(C.PDO).UsedRegisters -= C.Request.Registers;
  promote(C.PDO, Plan.Ready);
  return llvm::Error::success();
}

llvm::Expected<KernelDMA::TransferPlan>
KernelDMA::planTransfer(const TransferRequest &R) const {
  const auto *C = channel(R.Object);
  const auto *Adapter = adapter(R.Adapter);
  if (!C || !Adapter || C->Request.Adapter != R.Adapter || !C->Live ||
      !C->Begun || (C->Returned && !C->Retained))
    return channelError("mapping requires delivered or retained registers");
  if (!R.MDL || !R.MDLSize || R.MDLSize > UINT64_MAX - R.MDL || !R.Length ||
      R.Length > DriverDmaMaximumLengthLimit ||
      R.Length > UINT64_MAX - R.CurrentVA ||
      (R.Direction != DriverDmaDirection::ReadMemory &&
       R.Direction != DriverDmaDirection::WriteMemory))
    return channelError("invalid MDL transfer extent or direction");
  const auto *Region = Physical.find(R.Owner);
  if (!Region || R.Offset >= Region->Size || R.Length > Region->Size - R.Offset)
    return channelError("transfer exceeds its existing physical RAM view");
  const uint64_t Backing = Region->Backing + R.Offset;
  if (Backing & (Adapter->Alignment - 1))
    return channelError("transfer fails the adapter alignment");
  TransferPlan Plan;
  Plan.Request = R;
  Plan.Length = Adapter->ScatterGather
                    ? std::min<uint32_t>(
                          R.Length, DriverDmaPageSize -
                                        (Backing & (DriverDmaPageSize - 1)))
                    : R.Length;
  const auto *Map = mapping(R.Object);
  if (Map) {
    if (!Adapter->ScatterGather)
      return channelError(
          "non-SG operation requires a flush before another map");
    if (!Map->Channel || Map->MDL != R.MDL || Map->MDLSize != R.MDLSize ||
        Map->Owner != R.Owner || Map->Direction != R.Direction ||
        Map->Offset + Map->Length != R.Offset ||
        C->InitialVA + Map->Length != R.CurrentVA)
      return channelError(
          "operation maps require one MDL, direction and contiguous indices");
    Plan.TotalLength = Map->Length + Plan.Length;
    Plan.Registers = spanPages(Map->Backing, Plan.TotalLength);
    Plan.Logical = Map->Logical + Map->Length;
    Plan.NextLogical = Map->NextLogical;
    Plan.Serial = C->OperationSerial;
    if (auto E = Physical.canExtendPin(Map->Pin, Plan.TotalLength))
      return E;
  } else {
    if (C->OperationSerial == UINT64_MAX)
      return channelError("operation identity exhausted");
    Plan.First = true;
    Plan.TotalLength = Plan.Length;
    Plan.Registers = spanPages(Backing, Plan.Length);
    Plan.Serial = C->OperationSerial + 1;
    const auto &Domain = Domains.at(C->PDO);
    const auto &Config = *Resources.find(C->PDO)->Dma;
    const uint64_t End = Config.LogicalBase + Config.LogicalLength;
    const uint64_t Bytes = C->Request.Registers * uint64_t(DriverDmaPageSize);
    if (Domain.NextLogical > End || Bytes > End - Domain.NextLogical)
      return channelError("operation exhausted the nonreused logical aperture");
    Plan.Logical = Domain.NextLogical + (Backing & (DriverDmaPageSize - 1));
    Plan.NextLogical = Domain.NextLogical + Bytes;
    if (auto E = Physical.canPin(R.Owner, R.Offset, Plan.Length))
      return E;
  }
  if (Plan.Registers > C->Request.Registers ||
      Plan.TotalLength > Adapter->MaximumLength)
    return channelError(
        "operation exceeds its reserved registers or maximum length");
  return Plan;
}

llvm::Error KernelDMA::commitTransfer(const TransferPlan &Plan) {
  auto Current = planTransfer(Plan.Request);
  if (!Current)
    return Current.takeError();
  if (*Current != Plan)
    return channelError("transfer plan changed before publication");
  auto &C = Channels.at(Plan.Request.Object);
  if (Plan.First) {
    auto Pin =
        Physical.pin(Plan.Request.Owner, Plan.Request.Offset, Plan.Length);
    if (!Pin)
      return Pin.takeError();
    Mapping Map;
    Map.Object = Plan.Request.Object;
    Map.Adapter = Plan.Request.Adapter;
    Map.PDO = C.PDO;
    Map.Owner = Plan.Request.Owner;
    Map.Offset = Plan.Request.Offset;
    Map.Backing = Physical.find(Map.Owner)->Backing + Map.Offset;
    Map.Pin = *Pin;
    Map.Logical = Plan.Logical;
    Map.NextLogical = Plan.NextLogical;
    Map.Length = Plan.TotalLength;
    Map.Registers = Plan.Registers;
    Map.Channel = Map.Live = true;
    Map.Direction = Plan.Request.Direction;
    Map.MDL = Plan.Request.MDL;
    Map.MDLSize = Plan.Request.MDLSize;
    Mappings.emplace(Map.Object, Map);
    C.OperationSerial = Plan.Serial;
    C.InitialVA = Plan.Request.CurrentVA;
    Domains.at(C.PDO).NextLogical = Plan.NextLogical;
  } else {
    auto &Map = Mappings.at(Plan.Request.Object);
    if (auto E = Physical.extendPin(Map.Pin, Plan.TotalLength))
      return E;
    Map.Length = Plan.TotalLength;
    Map.Registers = Plan.Registers;
  }
  ++C.Revision;
  return llvm::Error::success();
}

llvm::Expected<KernelDMA::FlushPlan>
KernelDMA::planFlush(uint64_t Adapter, uint64_t Object, uint64_t MDL,
                     uint64_t CurrentVA, uint32_t Length,
                     DriverDmaDirection Direction) const {
  const auto *C = channel(Object);
  const auto *Map = mapping(Object);
  if (!C || !adapter(Adapter) || C->Request.Adapter != Adapter || !Map ||
      !Map->Channel || Map->MDL != MDL || C->InitialVA != CurrentVA ||
      Map->Length != Length || Map->Direction != Direction)
    return channelError(
        "flush requires the complete original MDL/index/length/direction");
  return FlushPlan{Adapter, Object,   MDL, CurrentVA, C->OperationSerial,
                   Length,  Direction};
}

llvm::Error KernelDMA::flush(const FlushPlan &Plan) {
  auto Current = planFlush(Plan.Adapter, Plan.Object, Plan.MDL, Plan.CurrentVA,
                           Plan.Length, Plan.Direction);
  if (!Current)
    return Current.takeError();
  if (*Current != Plan)
    return channelError("flush operation changed before publication");
  if (auto E = Physical.unpin(Mappings.at(Plan.Object).Pin))
    return E;
  Mappings.erase(Plan.Object);
  ++Channels.at(Plan.Object).Revision;
  return llvm::Error::success();
}
} // namespace neverd::emulation
