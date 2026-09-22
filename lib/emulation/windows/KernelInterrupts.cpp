//===- KernelInterrupts.cpp - Real interrupt callback and lock lifetime ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bind explicit pulses to the connection and assignment present at submission.
/// ISR and synchronized guest calls retain one nonrecursive interrupt lock.
///
//===----------------------------------------------------------------------===//

#include "KernelInterrupts.h"

#include <algorithm>
#include <tuple>

namespace neverd::emulation {
namespace {
llvm::Error interruptError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "interrupt: " + Message);
}
} // namespace

llvm::Expected<KernelInterrupts::Connection>
KernelInterrupts::match(uint64_t PDO, uint32_t Vector, uint8_t IRQL,
                        uint64_t Affinity, bool LineBased) const {
  std::optional<Connection> Found;
  for (const auto &[Owner, Device] : Resources.devices()) {
    if (PDO && PDO != Owner)
      continue;
    for (size_t I = 0; I < Device.Interrupts.size(); ++I) {
      const auto &Resource = Device.Interrupts[I];
      if (!LineBased && (Resource.TranslatedVector != Vector ||
                         Resource.TranslatedLevel != IRQL ||
                         Resource.TranslatedAffinity != Affinity))
        continue;
      if (Found)
        return interruptError("connection requires one assigned interrupt");
      if (!Device.Assigned || !Device.Present)
        return interruptError(
            "connection requires a present assigned resource");
      for (const auto &[Object, Existing] : Connections) {
        (void)Object;
        if (Existing.PDO == Owner && Existing.ResourceIndex == I)
          return interruptError("exclusive interrupt is already connected");
      }
      Connection Candidate;
      Candidate.PDO = Owner;
      Candidate.Epoch = Device.Epoch;
      Candidate.ResourceIndex = I;
      Candidate.IRQL = uint8_t(Resource.TranslatedLevel);
      Found = Candidate;
    }
  }
  if (!Found)
    return interruptError(
        "tuple does not match a translated interrupt resource");
  return *Found;
}

llvm::Error KernelInterrupts::connect(Connection Candidate) {
  const auto *Device = Resources.find(Candidate.PDO);
  if (!Device || !Device->Assigned || !Device->Present ||
      Device->Epoch != Candidate.Epoch ||
      Candidate.ResourceIndex >= Device->Interrupts.size() ||
      !Candidate.Object || !Candidate.Routine ||
      Candidate.IRQL !=
          Device->Interrupts[Candidate.ResourceIndex].TranslatedLevel)
    return interruptError("connection lost its assigned resource identity");
  if (bool(Candidate.OutputDeviceBase) != bool(Candidate.OutputDeviceSize) ||
      Candidate.OutputDeviceSize > UINT64_MAX - Candidate.OutputDeviceBase)
    return interruptError("connection has an invalid output device extent");
  if (Connections.size() >= DriverScenarioInterruptLimit)
    return interruptError("connection capacity exhausted");
  if (std::find(Tokens.begin(), Tokens.end(), Candidate.Object) != Tokens.end())
    return interruptError("connection token cannot be reused");
  for (const auto &[Object, Existing] : Connections) {
    (void)Object;
    if (Existing.PDO == Candidate.PDO &&
        Existing.ResourceIndex == Candidate.ResourceIndex)
      return interruptError("exclusive interrupt is already connected");
  }
  Tokens.push_back(Candidate.Object);
  Connections.emplace(Candidate.Object, Candidate);
  return llvm::Error::success();
}

const KernelInterrupts::Connection *
KernelInterrupts::connection(uint64_t Object) const {
  const auto It = Connections.find(Object);
  return It == Connections.end() ? nullptr : &It->second;
}

llvm::Error KernelInterrupts::disconnect(uint64_t Object, uint32_t Version) {
  const auto *Connection = connection(Object);
  if (!Connection || Connection->Version != Version)
    return interruptError(
        "disconnect requires its live connection and version");
  if (std::any_of(Holds.begin(), Holds.end(),
                  [&](const Hold &Hold) { return Hold.Object == Object; }) ||
      std::any_of(Calls.begin(), Calls.end(), [&](const auto &Call) {
        return Call.second.Object == Object;
      }))
    return interruptError(
        "disconnect cannot retire an owned interrupt callback");
  // Future external pulses survive disconnect and will report the lost token.
  Connections.erase(Object);
  return llvm::Error::success();
}

llvm::Error KernelInterrupts::canRelease(uint64_t PDO) const {
  for (const auto &[Object, Connection] : Connections) {
    (void)Object;
    if (Connection.PDO == PDO)
      return interruptError("device still owns a connected interrupt");
  }
  for (const auto &[Index, Event] : Events) {
    (void)Index;
    if (Event.PDO == PDO)
      return interruptError("device still owns an explicit interrupt event");
  }
  return llvm::Error::success();
}

llvm::Error KernelInterrupts::canReleaseRange(uint64_t Base,
                                              uint64_t Size) const {
  if (Size > UINT64_MAX - Base)
    return interruptError("overflowing storage release range");
  if (!Size)
    return llvm::Error::success();
  const uint64_t End = Base + Size;
  for (const auto &[Object, Connection] : Connections) {
    (void)Object;
    if (Connection.Context && Base <= Connection.Context &&
        Connection.Context < End)
      return interruptError("storage still owns a connected interrupt context");
    if (Connection.OutputDeviceSize &&
        Base < Connection.OutputDeviceBase + Connection.OutputDeviceSize &&
        Connection.OutputDeviceBase < End)
      return interruptError("device storage still owns a connected interrupt");
  }
  return llvm::Error::success();
}

llvm::Error KernelInterrupts::validateGuestAccess(uint64_t Address,
                                                  uint32_t Size) const {
  for (uint64_t Token : Tokens)
    if (Address < Token + interrupts::TokenSize && Token < Address + Size)
      return interruptError("KINTERRUPT is an opaque model-owned object");
  return llvm::Error::success();
}

llvm::Expected<KernelInterrupts::Event>
KernelInterrupts::resolveEvent(const DriverInterruptEvent &Input,
                               uint64_t Now) const {
  if (Input.After100ns > UINT64_MAX - Now)
    return interruptError("event deadline overflows virtual time");
  for (const auto &[PDO, Device] : Resources.devices()) {
    if (Device.ID != Input.DeviceID)
      continue;
    if (!Device.Present || !Device.Assigned)
      return interruptError(
          "event submission requires an assigned present device");
    for (const auto &[Object, Connection] : Connections)
      if (Connection.PDO == PDO && Connection.Epoch == Device.Epoch &&
          Device.Interrupts[Connection.ResourceIndex].ID == Input.InterruptID)
        return Event{Object, PDO, Device.Epoch, Now + Input.After100ns, 0};
    return interruptError("event submission requires its connected interrupt");
  }
  return interruptError("event names an unknown resource device");
}

llvm::Error
KernelInterrupts::canArm(llvm::ArrayRef<DriverInterruptEvent> Inputs,
                         size_t SourceIndex, uint64_t Now) const {
  if (Inputs.size() > DriverScenarioInterruptEventsPerRequestLimit ||
      Inputs.size() > DriverScenarioInterruptEventLimit -
                          std::min(Result.Interrupts.size(),
                                   DriverScenarioInterruptEventLimit) ||
      SourceIndex > UINT32_MAX)
    return interruptError("event observation capacity exhausted");
  for (const auto &Input : Inputs) {
    auto Event = resolveEvent(Input, Now);
    if (!Event)
      return Event.takeError();
  }
  return llvm::Error::success();
}

llvm::Error KernelInterrupts::arm(llvm::ArrayRef<DriverInterruptEvent> Inputs,
                                  size_t SourceIndex, uint64_t Now) {
  if (auto E = canArm(Inputs, SourceIndex, Now))
    return E;
  std::vector<Event> Prepared;
  for (const auto &Input : Inputs) {
    auto Event = resolveEvent(Input, Now);
    if (!Event)
      return Event.takeError();
    Prepared.push_back(*Event);
  }
  for (size_t I = 0; I < Inputs.size(); ++I) {
    auto &Event = Prepared[I];
    Event.Observation = Result.Interrupts.size();
    DriverInterruptResult Observation;
    Observation.SourceRequestIndex = uint32_t(SourceIndex);
    Observation.EventIndex = uint32_t(I);
    Observation.DeviceID = Inputs[I].DeviceID;
    Observation.InterruptID = Inputs[I].InterruptID;
    Observation.Epoch = Event.Epoch;
    Observation.DueAt100ns = Event.Due;
    Result.Interrupts.push_back(std::move(Observation));
    Events.emplace(Event.Observation, Event);
  }
  return llvm::Error::success();
}

std::optional<uint64_t> KernelInterrupts::nextEventTime() const {
  std::optional<uint64_t> Time;
  for (const auto &[Index, Event] : Events) {
    (void)Index;
    if (!Event.Queued && (!Time || Event.Due < *Time))
      Time = Event.Due;
  }
  return Time;
}

llvm::Error KernelInterrupts::canPrepareCalls(uint64_t Count) const {
  constexpr size_t Limit =
      DriverScenarioInterruptEventLimit + profile::MaxConcurrentCallbacks;
  if (Count > Limit - std::min(Calls.size(), Limit) ||
      Count > UINT64_MAX - NextCall)
    return interruptError("guest continuation capacity exhausted");
  return llvm::Error::success();
}

llvm::Expected<uint64_t> KernelInterrupts::dueCount(uint64_t Time) const {
  const uint64_t Count =
      std::count_if(Events.begin(), Events.end(), [&](const auto &Entry) {
        return !Entry.second.Queued && Entry.second.Due <= Time;
      });
  if (auto E = canPrepareCalls(Count))
    return E;
  return Count;
}

llvm::Error KernelInterrupts::deliveryError(Event &Event,
                                            const llvm::Twine &Message,
                                            uint64_t Now) {
  auto &Observation = Result.Interrupts[Event.Observation];
  Observation.OccurredAt100ns = Now;
  Observation.UndeliveredReason = Message.str();
  return interruptError(Message);
}

llvm::Expected<std::optional<KernelInterrupts::Delivery>>
KernelInterrupts::queueNextDue(uint64_t Now) {
  auto Selected = Events.end();
  for (auto It = Events.begin(); It != Events.end(); ++It)
    if (!It->second.Queued && It->second.Due <= Now &&
        (Selected == Events.end() ||
         std::tie(It->second.Due, It->first) <
             std::tie(Selected->second.Due, Selected->first)))
      Selected = It;
  if (Selected == Events.end())
    return std::optional<Delivery>{};
  auto &Event = Selected->second;
  const auto *Device = Resources.find(Event.PDO);
  if (!Device || !Device->Assigned || !Device->Present ||
      Device->Epoch != Event.Epoch)
    return deliveryError(Event, "pulse targets an unavailable resource epoch",
                         Now);
  if (Device->Power != DevicePowerState::D0)
    return deliveryError(Event, "pulse requires physical device power D0", Now);
  const auto *Connection = connection(Event.Object);
  if (!Connection || Connection->PDO != Event.PDO ||
      Connection->Epoch != Event.Epoch)
    return deliveryError(Event, "pulse lost its original interrupt connection",
                         Now);
  if (auto E = canPrepareCalls(1))
    return E;
  const uint64_t Token = NextCall++;
  Calls.emplace(Token, Call{Event.Object, Event.Observation});
  Event.Queued = true;
  auto &Observation = Result.Interrupts[Event.Observation];
  Observation.OccurredAt100ns = Now;
  Observation.InterruptObject = Event.Object;
  Delivery Delivery;
  Delivery.PDO = Event.PDO;
  Delivery.IRQL = Connection->IRQL;
  Delivery.Call = {{GuestCallOwner::Interrupt, Token},
                   Connection->Routine,
                   {Event.Object, Connection->Context}};
  return std::optional<KernelInterrupts::Delivery>{std::move(Delivery)};
}

llvm::Error KernelInterrupts::canHold(uint64_t Object,
                                      uint8_t CurrentIRQL) const {
  const auto *Connection = connection(Object);
  if (!Connection)
    return interruptError("lock requires a live interrupt object");
  if (CurrentIRQL > Connection->IRQL)
    return interruptError("caller IRQL exceeds interrupt synchronization IRQL");
  if (std::any_of(Holds.begin(), Holds.end(),
                  [&](const Hold &Hold) { return Hold.Object == Object; }))
    return interruptError("interrupt spin lock is nonrecursive");
  return llvm::Error::success();
}

llvm::Expected<KernelGuestCall>
KernelInterrupts::synchronize(uint64_t Object, uint64_t Routine,
                              uint64_t Context) {
  if (!connection(Object) || !Routine)
    return interruptError(
        "synchronization requires a live interrupt and routine");
  if (auto E = canPrepareCalls(1))
    return E;
  const uint64_t Token = NextCall++;
  Calls.emplace(Token, Call{Object, std::nullopt});
  return KernelGuestCall{
      {GuestCallOwner::Interrupt, Token}, Routine, {Context}};
}

llvm::Expected<uint8_t>
KernelInterrupts::beginCall(uint64_t Token, uint8_t CallerIRQL, uint64_t Now) {
  auto It = Calls.find(Token);
  if (It == Calls.end() || It->second.Begun)
    return interruptError("callback entry lost its prepared continuation");
  auto &Call = It->second;
  if (auto E = canHold(Call.Object, CallerIRQL))
    return E;
  Holds.push_back({Call.Object, Token, CallerIRQL, HoldKind::Callback});
  Call.Begun = true;
  if (Call.Observation)
    Result.Interrupts[*Call.Observation].DeliveredAt100ns = Now;
  return connection(Call.Object)->IRQL;
}

llvm::Expected<KernelInterrupts::CallbackReturn>
KernelInterrupts::finishCall(uint64_t Token, uint64_t Value,
                             uint8_t CurrentIRQL, uint64_t Now) {
  auto It = Calls.find(Token);
  if (It == Calls.end() || !It->second.Begun || Holds.empty() ||
      Holds.back().Kind != HoldKind::Callback || Holds.back().Owner != Token ||
      Holds.back().Object != It->second.Object ||
      CurrentIRQL != connection(It->second.Object)->IRQL)
    return interruptError("callback return lost its interrupt lock or IRQL");
  const CallbackReturn Return{uint8_t(Value), Holds.back().OldIRQL};
  if (It->second.Observation) {
    const size_t Index = *It->second.Observation;
    auto &Observation = Result.Interrupts[Index];
    Observation.ReturnValue = Return.Value;
    Observation.ReturnedAt100ns = Now;
    Events.erase(Index);
  }
  Holds.pop_back();
  Calls.erase(It);
  return Return;
}

llvm::Expected<uint8_t> KernelInterrupts::acquire(uint64_t Object,
                                                  uint64_t Execution,
                                                  uint8_t CurrentIRQL) {
  if (auto E = canHold(Object, CurrentIRQL))
    return E;
  Holds.push_back({Object, Execution, CurrentIRQL, HoldKind::Manual});
  return CurrentIRQL;
}

llvm::Expected<uint8_t> KernelInterrupts::release(uint64_t Object,
                                                  uint64_t Execution,
                                                  uint8_t OldIRQL,
                                                  uint8_t CurrentIRQL) {
  const auto *Connection = connection(Object);
  if (!Connection || Holds.empty() || Holds.back().Kind != HoldKind::Manual ||
      Holds.back().Object != Object || Holds.back().Owner != Execution ||
      Holds.back().OldIRQL != OldIRQL || CurrentIRQL != Connection->IRQL)
    return interruptError(
        "release requires the owning execution and saved IRQL");
  Holds.pop_back();
  return OldIRQL;
}

llvm::Error
KernelInterrupts::validateExecutionReturn(uint64_t Execution) const {
  if (std::any_of(Holds.begin(), Holds.end(), [&](const Hold &Hold) {
        return Hold.Kind == HoldKind::Manual && Hold.Owner == Execution;
      }))
    return interruptError("guest return retains a manual interrupt spin lock");
  return llvm::Error::success();
}
} // namespace neverd::emulation
