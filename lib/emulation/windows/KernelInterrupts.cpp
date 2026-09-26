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
    if (!PDO && (!Device.Assigned || !Device.Present))
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
        if (Existing.PDO == Owner && Existing.ResourceIndex == I &&
            Resource.Share != DriverInterruptShare::Shared)
          return interruptError("exclusive interrupt is already connected");
      }
      Connection Candidate;
      Candidate.PDO = Owner;
      Candidate.Epoch = Device.Epoch;
      Candidate.ResourceIndex = I;
      Candidate.IRQL = uint8_t(Resource.TranslatedLevel);
      Candidate.SynchronizeIRQL = Candidate.IRQL;
      Found = Candidate;
    }
  }
  if (!Found)
    return interruptError(
        "tuple does not match a translated interrupt resource");
  return *Found;
}

bool KernelInterrupts::sameLine(const Connection &Left,
                                const Connection &Right) const {
  const auto &L = Resources.find(Left.PDO)->Interrupts[Left.ResourceIndex];
  const auto &R = Resources.find(Right.PDO)->Interrupts[Right.ResourceIndex];
  return L.TranslatedVector == R.TranslatedVector &&
         L.TranslatedLevel == R.TranslatedLevel &&
         L.TranslatedAffinity == R.TranslatedAffinity && L.Mode == R.Mode;
}

llvm::Error KernelInterrupts::canConnect(const Connection &Candidate) const {
  const auto *Device = Resources.find(Candidate.PDO);
  if (!Device || !Device->Assigned || !Device->Present ||
      Device->Epoch != Candidate.Epoch ||
      Candidate.ResourceIndex >= Device->Interrupts.size() ||
      !Candidate.Object || !Candidate.Routine ||
      Candidate.IRQL !=
          Device->Interrupts[Candidate.ResourceIndex].TranslatedLevel)
    return interruptError("connection lost its assigned resource identity");
  const auto &Resource = Device->Interrupts[Candidate.ResourceIndex];
  if ((Resource.Mode == DriverInterruptMode::LevelSensitive &&
       (!Resource.RetriggerAfter100ns || !*Resource.RetriggerAfter100ns ||
        *Resource.RetriggerAfter100ns > INT64_MAX)) ||
      (Resource.Mode == DriverInterruptMode::Latched &&
       Resource.RetriggerAfter100ns) ||
      (Resource.Mode != DriverInterruptMode::LevelSensitive &&
       Resource.Mode != DriverInterruptMode::Latched))
    return interruptError(
        "connection requires a supported mode and sampling period");
  if (bool(Candidate.OutputDeviceBase) != bool(Candidate.OutputDeviceSize) ||
      Candidate.OutputDeviceSize > UINT64_MAX - Candidate.OutputDeviceBase)
    return interruptError("connection has an invalid output device extent");
  if (Candidate.SynchronizeIRQL < Candidate.IRQL ||
      Candidate.SynchronizeIRQL > DriverInterruptMaximumLevel ||
      (!Candidate.SpinLock && Candidate.SynchronizeIRQL != Candidate.IRQL))
    return interruptError("invalid interrupt synchronization IRQL");
  if (Connections.size() >= DriverScenarioInterruptLimit)
    return interruptError("connection capacity exhausted");
  if (std::find(Tokens.begin(), Tokens.end(), Candidate.Object) != Tokens.end())
    return interruptError("connection token cannot be reused");
  for (const auto &[Object, Existing] : Connections) {
    (void)Object;
    const auto &Peer =
        Resources.find(Existing.PDO)->Interrupts[Existing.ResourceIndex];
    if (Peer.TranslatedVector == Resource.TranslatedVector &&
        (!sameLine(Existing, Candidate) ||
         Peer.RetriggerAfter100ns != Resource.RetriggerAfter100ns))
      return interruptError("shared vector requires matching line facts");
    if (sameLine(Existing, Candidate) &&
        (Device->Interrupts[Candidate.ResourceIndex].Share !=
             DriverInterruptShare::Shared ||
         Resources.find(Existing.PDO)
                 ->Interrupts[Existing.ResourceIndex]
                 .Share != DriverInterruptShare::Shared))
      return interruptError("exclusive interrupt is already connected");
    if (Candidate.SpinLock && Existing.SpinLock == Candidate.SpinLock &&
        Existing.SynchronizeIRQL != Candidate.SynchronizeIRQL)
      return interruptError(
          "shared spin lock requires one synchronization IRQL");
  }
  return llvm::Error::success();
}

llvm::Error KernelInterrupts::connect(Connection Candidate) {
  if (auto E = canConnect(Candidate))
    return E;
  Tokens.push_back(Candidate.Object);
  Connections.emplace(Candidate.Object, Candidate);
  return llvm::Error::success();
}

bool KernelInterrupts::usesSpinLock(uint64_t Address) const {
  return std::any_of(Connections.begin(), Connections.end(),
                     [Address](const auto &Entry) {
                       return Entry.second.SpinLock == Address;
                     });
}

uint64_t KernelInterrupts::lockIdentity(uint64_t Object) const {
  const auto *Connection = connection(Object);
  return Connection->SpinLock ? Connection->SpinLock : Object;
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
        if (Call.second.Object == Object)
          return true;
        if (!Call.second.Observation)
          return false;
        const auto &Chain = Call.second.Chain;
        return std::find(Chain.begin(), Chain.end(), Object) != Chain.end();
      }))
    return interruptError(
        "disconnect cannot retire an owned interrupt callback");
  for (const auto &[Vector, Line] : LevelLines)
    for (const auto &[Source, Event] : Line.Sources)
      if (std::find(Event.Chain.begin(), Event.Chain.end(), Object) !=
          Event.Chain.end())
        return interruptError(
            "disconnect cannot retire an asserted interrupt line");
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
  for (const auto &[Vector, Line] : LevelLines)
    for (const auto &[Source, Event] : Line.Sources)
      if (Event.PDO == PDO)
        return interruptError("device still owns an asserted interrupt source");
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
    if (Connection.SpinLock &&
        Base < Connection.SpinLock + profile::PointerSize &&
        Connection.SpinLock < End)
      return interruptError(
          "storage still owns a connected interrupt spin lock");
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
  for (const auto &[Object, Connection] : Connections)
    if (Connection.SpinLock &&
        Address < Connection.SpinLock + profile::PointerSize &&
        Connection.SpinLock < Address + Size)
      return interruptError("connected interrupt spin lock is opaque storage");
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
          Device.Interrupts[Connection.ResourceIndex].ID == Input.InterruptID) {
        const auto &Resource = Device.Interrupts[Connection.ResourceIndex];
        const bool Level = Resource.Mode == DriverInterruptMode::LevelSensitive;
        switch (Input.Action) {
        case DriverInterruptAction::Pulse:
          if (Level)
            return interruptError(
                "level-sensitive sources require assert/deassert events");
          break;
        case DriverInterruptAction::Assert:
        case DriverInterruptAction::Deassert:
          if (!Level)
            return interruptError("latched sources require pulse events");
          break;
        default:
          return interruptError("unsupported interrupt event action");
        }
        if (Level &&
            (!Resource.RetriggerAfter100ns || !*Resource.RetriggerAfter100ns ||
             *Resource.RetriggerAfter100ns > INT64_MAX))
          return interruptError(
              "level-sensitive sources require a positive sampling period");
        Event Event{Object, PDO, Device.Epoch, Now + Input.After100ns, 0};
        Event.ResourceIndex = Connection.ResourceIndex;
        Event.Vector = Resource.TranslatedVector;
        Event.Action = Input.Action;
        Event.RetriggerAfter100ns = Resource.RetriggerAfter100ns.value_or(0);
        for (const auto &[PeerObject, Peer] : Connections)
          if (sameLine(Connection, Peer))
            Event.Chain.push_back(PeerObject);
        return Event;
      }
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
    Observation.Action = Inputs[I].Action;
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
  for (const auto &[Vector, Line] : LevelLines)
    if (!Line.Queued && !Line.Sources.empty() && (!Time || Line.Due < *Time))
      Time = Line.Due;
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
  const auto Plan = planBoundary(Time);
  const uint64_t Count = Plan.Pulses.size() + Plan.Levels.size();
  if (auto E = canPrepareCalls(Count))
    return E;
  if (Count > DriverInterruptDeliveryLimit -
                  std::min<uint64_t>(Deliveries, DriverInterruptDeliveryLimit))
    return interruptError("interrupt delivery limit exhausted");
  for (uint32_t Vector : Plan.Levels)
    if (Plan.Lines.at(Vector).Sources.begin()->second.RetriggerAfter100ns >
        UINT64_MAX - Time)
      return interruptError("level sampling deadline overflows virtual time");
  return Count;
}

KernelInterrupts::Boundary KernelInterrupts::planBoundary(uint64_t Time) const {
  Boundary Plan;
  Plan.Lines = LevelLines;
  for (const auto &[Index, Event] : Events) {
    if (Event.Queued || Event.Due > Time)
      continue;
    (Event.Action == DriverInterruptAction::Pulse ? Plan.Pulses
                                                  : Plan.Transitions)
        .push_back(Index);
  }
  auto Earlier = [&](size_t Left, size_t Right) {
    return std::tie(Events.at(Left).Due, Left) <
           std::tie(Events.at(Right).Due, Right);
  };
  std::sort(Plan.Pulses.begin(), Plan.Pulses.end(), Earlier);
  std::sort(Plan.Transitions.begin(), Plan.Transitions.end(), Earlier);
  for (size_t Index : Plan.Transitions) {
    const auto &Event = Events.at(Index);
    const Source Key{Event.PDO, Event.ResourceIndex};
    if (Event.Action == DriverInterruptAction::Assert) {
      auto &Line = Plan.Lines[Event.Vector];
      if (Line.Sources.empty() && !Line.Queued)
        Line.Due = Event.Due;
      Line.Sources.try_emplace(Key, Event);
    } else if (auto It = Plan.Lines.find(Event.Vector);
               It != Plan.Lines.end()) {
      It->second.Sources.erase(Key);
      if (It->second.Sources.empty() && !It->second.Queued)
        Plan.Lines.erase(It);
    }
  }
  for (const auto &[Vector, Line] : Plan.Lines)
    if (!Line.Queued && !Line.Sources.empty() && Line.Due <= Time)
      Plan.Levels.push_back(Vector);
  std::sort(Plan.Levels.begin(), Plan.Levels.end(),
            [&](uint32_t Left, uint32_t Right) {
              const auto &L = Plan.Lines.at(Left);
              const auto &R = Plan.Lines.at(Right);
              return std::tie(L.Due, L.Sources.begin()->second.Observation) <
                     std::tie(R.Due, R.Sources.begin()->second.Observation);
            });
  return Plan;
}

llvm::Error KernelInterrupts::validateEvent(const Event &Event,
                                            bool CheckPower) const {
  const auto *Device = Resources.find(Event.PDO);
  if (!Device || !Device->Assigned || !Device->Present ||
      Device->Epoch != Event.Epoch)
    return interruptError("pulse targets an unavailable resource epoch");
  if (CheckPower && Device->Power != DevicePowerState::D0)
    return interruptError("pulse requires physical device power D0");
  const auto *Connection = connection(Event.Object);
  if (!Connection || Connection->PDO != Event.PDO ||
      Connection->Epoch != Event.Epoch)
    return interruptError("pulse lost its original interrupt connection");
  for (uint64_t Object : Event.Chain) {
    const auto *Peer = connection(Object);
    const auto *Provider = Peer ? Resources.find(Peer->PDO) : nullptr;
    if (!Peer || !Provider || !Provider->Present || !Provider->Assigned ||
        Provider->Epoch != Peer->Epoch ||
        (CheckPower && Provider->Power != DevicePowerState::D0))
      return interruptError("shared pulse lost a captured live connection");
  }
  return llvm::Error::success();
}

llvm::Error KernelInterrupts::deliveryError(Event &Event,
                                            const llvm::Twine &Message,
                                            uint64_t Now) {
  auto &Observation = Result.Interrupts[Event.Observation];
  if (!Observation.OccurredAt100ns)
    Observation.OccurredAt100ns = Now;
  Observation.UndeliveredReason = Message.str();
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}

llvm::Expected<std::optional<KernelInterrupts::Delivery>>
KernelInterrupts::queueNextDue(uint64_t Now) {
  auto Count = dueCount(Now);
  if (!Count)
    return Count.takeError();
  auto Plan = planBoundary(Now);
  for (size_t Index : Plan.Transitions) {
    auto &Event = Events.at(Index);
    if (auto E =
            validateEvent(Event, Event.Action == DriverInterruptAction::Assert))
      return deliveryError(Event, llvm::toString(std::move(E)), Now);
  }
  for (size_t Index : Plan.Pulses)
    if (auto E = validateEvent(Events.at(Index), true))
      return deliveryError(Events.at(Index), llvm::toString(std::move(E)), Now);
  for (uint32_t Vector : Plan.Levels) {
    auto &Event = Plan.Lines.at(Vector).Sources.begin()->second;
    if (auto E = validateEvent(Event, true))
      return deliveryError(Event, llvm::toString(std::move(E)), Now);
  }
  for (size_t Index : Plan.Transitions) {
    Result.Interrupts[Index].OccurredAt100ns = Now;
    Events.erase(Index);
  }
  LevelLines = std::move(Plan.Lines);
  if (!*Count)
    return std::optional<Delivery>{};
  std::optional<uint32_t> LineVector;
  if (!Plan.Levels.empty()) {
    const uint32_t Vector = Plan.Levels.front();
    const auto &Line = LevelLines.at(Vector);
    if (Plan.Pulses.empty() ||
        std::tie(Line.Due, Line.Sources.begin()->second.Observation) <
            std::tie(Events.at(Plan.Pulses.front()).Due, Plan.Pulses.front()))
      LineVector = Vector;
  }
  auto &Event = LineVector ? LevelLines.at(*LineVector).Sources.begin()->second
                           : Events.at(Plan.Pulses.front());
  uint32_t DeliveryIndex = 0;
  if (LineVector) {
    auto &Line = LevelLines.at(*LineVector);
    Line.Queued = true;
    DeliveryIndex = Line.Deliveries++;
  } else {
    Event.Queued = true;
  }
  const uint64_t Token = NextCall++;
  ++Deliveries;
  const auto First = Event.Chain.front();
  const auto *Connection = connection(First);
  Calls.emplace(Token, Call{First, Event.Observation, false, 0, Event.Chain,
                            LineVector, DeliveryIndex});
  auto &Observation = Result.Interrupts[Event.Observation];
  if (!Observation.OccurredAt100ns)
    Observation.OccurredAt100ns = Now;
  Observation.ReturnValue.reset();
  Observation.ReturnedAt100ns.reset();
  Observation.InterruptObject = First;
  Delivery Delivery;
  Delivery.PDO = Event.PDO;
  Delivery.IRQL = Connection->SynchronizeIRQL;
  Delivery.Priority = Connection->IRQL;
  Delivery.Call = {{GuestCallOwner::Interrupt, Token},
                   Connection->Routine,
                   {First, Connection->Context}};
  return std::optional<KernelInterrupts::Delivery>{std::move(Delivery)};
}

llvm::Error KernelInterrupts::canHold(uint64_t Object,
                                      uint8_t CurrentIRQL) const {
  const auto *Connection = connection(Object);
  if (!Connection)
    return interruptError("lock requires a live interrupt object");
  if (CurrentIRQL > Connection->SynchronizeIRQL)
    return interruptError("caller IRQL exceeds interrupt synchronization IRQL");
  if (std::any_of(Holds.begin(), Holds.end(), [&](const Hold &Hold) {
        return lockIdentity(Hold.Object) == lockIdentity(Object);
      }))
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
  if (Call.Observation) {
    auto &Observation = Result.Interrupts[*Call.Observation];
    if (!Observation.DeliveredAt100ns)
      Observation.DeliveredAt100ns = Now;
    Observation.Handlers.push_back(
        {Call.Object, Now, {}, {}, Call.DeliveryIndex});
  }
  return connection(Call.Object)->SynchronizeIRQL;
}

llvm::Expected<KernelInterrupts::CallbackReturn>
KernelInterrupts::finishCall(uint64_t Token, uint64_t Value,
                             uint8_t CurrentIRQL, uint64_t Now) {
  auto It = Calls.find(Token);
  if (It == Calls.end() || !It->second.Begun || Holds.empty() ||
      Holds.back().Kind != HoldKind::Callback || Holds.back().Owner != Token ||
      Holds.back().Object != It->second.Object ||
      CurrentIRQL != connection(It->second.Object)->SynchronizeIRQL)
    return interruptError("callback return lost its interrupt lock or IRQL");
  CallbackReturn Return{uint8_t(Value), Holds.back().OldIRQL, {}};
  const auto &Pending = It->second;
  if (Pending.Line &&
      (Return.Value || Pending.Handler + 1 == Pending.Chain.size())) {
    const auto &Line = LevelLines.at(*Pending.Line);
    if (!Line.Sources.empty() &&
        Line.Sources.begin()->second.RetriggerAfter100ns > UINT64_MAX - Now)
      return interruptError("level sampling deadline overflows virtual time");
  }
  if (It->second.Observation) {
    const size_t Index = *It->second.Observation;
    auto &Observation = Result.Interrupts[Index];
    Observation.Handlers.back().ReturnValue = Return.Value;
    Observation.Handlers.back().ReturnedAt100ns = Now;
    if (!Observation.ReturnValue || !*Observation.ReturnValue)
      Observation.ReturnValue = Return.Value;
    auto &Call = It->second;
    const auto &Chain = Call.Chain;
    if (++Call.Handler < Chain.size() && (!Call.Line || !Return.Value)) {
      Holds.pop_back();
      Call.Object = Chain[Call.Handler];
      Call.Begun = false;
      const auto *Connection = connection(Call.Object);
      Return.Next = KernelGuestCall{{GuestCallOwner::Interrupt, Token},
                                    Connection->Routine,
                                    {Call.Object, Connection->Context}};
      return Return;
    }
    Return.Value = *Observation.ReturnValue;
    Observation.ReturnedAt100ns = Now;
    if (Call.Line) {
      auto Line = LevelLines.find(*Call.Line);
      if (Line->second.Sources.empty())
        LevelLines.erase(Line);
      else {
        Line->second.Due =
            Now + Line->second.Sources.begin()->second.RetriggerAfter100ns;
        Line->second.Queued = false;
      }
    } else {
      Events.erase(Index);
    }
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
      Holds.back().OldIRQL != OldIRQL ||
      CurrentIRQL != Connection->SynchronizeIRQL)
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

std::optional<uint8_t>
KernelInterrupts::manualHoldIRQL(uint64_t Execution) const {
  std::optional<uint8_t> Required;
  for (const Hold &Hold : Holds)
    if (Hold.Kind == HoldKind::Manual && Hold.Owner == Execution)
      if (const auto *Connection = connection(Hold.Object))
        Required = std::max(Required.value_or(0), Connection->SynchronizeIRQL);
  return Required;
}
} // namespace neverd::emulation
