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
#include <set>
#include <tuple>

namespace neverd::emulation {
namespace {
llvm::Error interruptError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "interrupt: " + Message);
}
} // namespace

DriverInterruptMessage
KernelInterrupts::assignment(const Connection &Connection) const {
  const auto &Resource =
      Resources.find(Connection.PDO)->Interrupts[Connection.ResourceIndex];
  if (Connection.ResourceMessage)
    return Resource.Messages[*Connection.ResourceMessage];
  DriverInterruptMessage Result;
  Result.TranslatedVector = Resource.TranslatedVector;
  Result.TranslatedLevel = Resource.TranslatedLevel;
  Result.TranslatedAffinity = Resource.TranslatedAffinity;
  return Result;
}

llvm::Expected<KernelInterrupts::Connection>
KernelInterrupts::match(uint64_t PDO, uint32_t Vector, uint8_t IRQL,
                        uint64_t Affinity, bool LineBased, bool Passive) const {
  std::optional<Connection> Found;
  for (const auto &[Owner, Device] : Resources.devices()) {
    if (PDO && PDO != Owner)
      continue;
    if (!PDO && (!Device.Assigned || !Device.Present))
      continue;
    for (size_t I = 0; I < Device.Interrupts.size(); ++I) {
      const auto &Resource = Device.Interrupts[I];
      if (LineBased && !Resource.Messages.empty())
        continue;
      for (size_t J = 0; J < std::max(size_t(1), Resource.Messages.size());
           ++J) {
        Connection Candidate;
        Candidate.PDO = Owner;
        Candidate.Epoch = Device.Epoch;
        Candidate.ResourceIndex = I;
        if (!Resource.Messages.empty())
          Candidate.ResourceMessage = uint32_t(J);
        const auto Fact = assignment(Candidate);
        if (!LineBased && (Fact.TranslatedVector != Vector ||
                           (!Passive && Fact.TranslatedLevel != IRQL) ||
                           Fact.TranslatedAffinity != Affinity))
          continue;
        if (Found)
          return interruptError("connection requires one assigned interrupt");
        if (!Device.Assigned || !Device.Present)
          return interruptError(
              "connection requires a present assigned resource");
        for (const auto &[Object, Existing] : Connections)
          if (Existing.PDO == Owner && Existing.ResourceIndex == I &&
              Existing.ResourceMessage == Candidate.ResourceMessage &&
              Resource.Share != DriverInterruptShare::Shared)
            return interruptError("exclusive interrupt is already connected");
        Candidate.IRQL = uint8_t(Fact.TranslatedLevel);
        Candidate.SynchronizeIRQL = Candidate.IRQL;
        Found = Candidate;
      }
    }
  }
  if (!Found)
    return interruptError(
        "tuple does not match a translated interrupt resource");
  return *Found;
}

llvm::Expected<std::vector<KernelInterrupts::Connection>>
KernelInterrupts::matchMessages(uint64_t PDO) const {
  const auto *Device = Resources.find(PDO);
  if (!Device || !Device->Assigned || !Device->Present)
    return interruptError("message connection requires a present assigned PDO");
  std::vector<Connection> Result;
  for (size_t I = 0; I < Device->Interrupts.size(); ++I)
    for (size_t J = 0; J < Device->Interrupts[I].Messages.size(); ++J) {
      Connection Candidate;
      Candidate.PDO = PDO;
      Candidate.Epoch = Device->Epoch;
      Candidate.ResourceIndex = I;
      Candidate.ResourceMessage = uint32_t(J);
      Candidate.MessageID = uint32_t(Result.size());
      Candidate.IRQL = uint8_t(assignment(Candidate).TranslatedLevel);
      Candidate.SynchronizeIRQL = Candidate.IRQL;
      Result.push_back(Candidate);
    }
  return Result;
}

bool KernelInterrupts::sameLine(const Connection &Left,
                                const Connection &Right) const {
  const auto L = assignment(Left), R = assignment(Right);
  return L.TranslatedVector == R.TranslatedVector &&
         L.TranslatedLevel == R.TranslatedLevel &&
         L.TranslatedAffinity == R.TranslatedAffinity &&
         Resources.find(Left.PDO)->Interrupts[Left.ResourceIndex].Mode ==
             Resources.find(Right.PDO)->Interrupts[Right.ResourceIndex].Mode;
}

llvm::Error KernelInterrupts::canShare(const Connection &Left,
                                       const Connection &Right) const {
  const auto &L = Resources.find(Left.PDO)->Interrupts[Left.ResourceIndex];
  const auto &R = Resources.find(Right.PDO)->Interrupts[Right.ResourceIndex];
  if (assignment(Left).TranslatedVector == assignment(Right).TranslatedVector &&
      (!sameLine(Left, Right) ||
       L.RetriggerAfter100ns != R.RetriggerAfter100ns))
    return interruptError("shared vector requires matching line facts");
  if (sameLine(Left, Right) && (L.Share != DriverInterruptShare::Shared ||
                                R.Share != DriverInterruptShare::Shared))
    return interruptError("exclusive interrupt is already connected");
  if (Left.SpinLock && Left.SpinLock == Right.SpinLock &&
      Left.SynchronizeIRQL != Right.SynchronizeIRQL)
    return interruptError("shared spin lock requires one synchronization IRQL");
  return llvm::Error::success();
}

llvm::Error KernelInterrupts::canConnect(const Connection &Candidate) const {
  const auto *Device = Resources.find(Candidate.PDO);
  if (!Device || !Device->Assigned || !Device->Present ||
      Device->Epoch != Candidate.Epoch ||
      Candidate.ResourceIndex >= Device->Interrupts.size() ||
      !Candidate.Object || !Candidate.Routine)
    return interruptError("connection lost its assigned resource identity");
  const auto &Resource = Device->Interrupts[Candidate.ResourceIndex];
  if (Resource.Messages.empty()
          ? Candidate.ResourceMessage.has_value()
          : (!Candidate.ResourceMessage ||
             *Candidate.ResourceMessage >= Resource.Messages.size()))
    return interruptError("connection lost its message assignment identity");
  if (Candidate.IRQL != assignment(Candidate).TranslatedLevel ||
      (Candidate.MessageID &&
       (!Candidate.ResourceMessage ||
        (Candidate.Version != interrupts::MessageBased &&
         Candidate.Version != interrupts::MessageBasedPassive))))
    return interruptError("connection has an invalid message service identity");
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
  if (Candidate.Passive &&
      (Candidate.SpinLock || Candidate.SynchronizeIRQL || !Candidate.Version))
    return interruptError(
        "passive interrupt requires a private synchronization event");
  if (!Candidate.Passive &&
      (Candidate.SynchronizeIRQL < Candidate.IRQL ||
       Candidate.SynchronizeIRQL > DriverInterruptMaximumLevel ||
       (!Candidate.SpinLock && !Candidate.MessageID &&
        Candidate.SynchronizeIRQL != Candidate.IRQL)))
    return interruptError("invalid interrupt synchronization IRQL");
  if (Connections.size() >= DriverScenarioInterruptLimit)
    return interruptError("connection capacity exhausted");
  if (std::find(Tokens.begin(), Tokens.end(), Candidate.Object) != Tokens.end())
    return interruptError("connection token cannot be reused");
  for (const auto &[Object, Existing] : Connections) {
    (void)Object;
    if (auto E = canShare(Existing, Candidate))
      return E;
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

llvm::Error KernelInterrupts::canConnectMessages(
    uint64_t Table, llvm::ArrayRef<Connection> Candidates) const {
  if (!Table || Candidates.empty() ||
      Candidates.size() > DriverScenarioInterruptMessageLimit ||
      Candidates.size() > DriverScenarioInterruptLimit - Connections.size() ||
      MessageGroups.contains(Table))
    return interruptError("invalid message table or connection capacity");
  auto Assigned = matchMessages(Candidates.front().PDO);
  if (!Assigned)
    return Assigned.takeError();
  if (Assigned->size() != Candidates.size())
    return interruptError("message table must connect every assigned message");
  for (size_t I = 0; I < Candidates.size(); ++I) {
    const auto &Candidate = Candidates[I];
    if (Candidate.ResourceIndex != (*Assigned)[I].ResourceIndex ||
        Candidate.ResourceMessage != (*Assigned)[I].ResourceMessage)
      return interruptError("message table does not follow resource order");
    if (Candidate.MessageID != I || Candidate.PDO != Candidates.front().PDO ||
        (Candidate.Version != interrupts::MessageBased &&
         Candidate.Version != interrupts::MessageBasedPassive))
      return interruptError(
          "message table requires ordered PDO-wide identities");
    if (auto E = canConnect(Candidate))
      return E;
    for (size_t J = 0; J < I; ++J) {
      if (Candidates[J].Object == Candidate.Object)
        return interruptError("message objects must be distinct");
      if (auto E = canShare(Candidates[J], Candidate))
        return E;
    }
  }
  return llvm::Error::success();
}

llvm::Error
KernelInterrupts::connectMessages(uint64_t Table,
                                  llvm::ArrayRef<Connection> Candidates) {
  if (auto E = canConnectMessages(Table, Candidates))
    return E;
  MessageGroup Group{interrupts::MessageTableHeaderSize +
                         Candidates.size() * interrupts::MessageEntrySize,
                     {}};
  for (const auto &Candidate : Candidates) {
    Group.Objects.push_back(Candidate.Object);
    Tokens.push_back(Candidate.Object);
    Connections.emplace(Candidate.Object, Candidate);
  }
  MessageGroups.emplace(Table, std::move(Group));
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

llvm::Error KernelInterrupts::canDisconnect(uint64_t Object,
                                            uint32_t Version) const {
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

  return llvm::Error::success();
}

llvm::Error KernelInterrupts::disconnect(uint64_t Object, uint32_t Version) {
  if (Version == interrupts::MessageBased ||
      Version == interrupts::MessageBasedPassive) {
    auto Group = MessageGroups.find(Object);
    if (Group == MessageGroups.end() || !Group->second.Live)
      return interruptError("disconnect requires its live message table");
    for (uint64_t Member : Group->second.Objects)
      if (auto E = canDisconnect(Member, Version))
        return E;
    for (uint64_t Member : Group->second.Objects)
      Connections.erase(Member);
    Group->second.Live = false;
    return llvm::Error::success();
  }
  if (auto E = canDisconnect(Object, Version))
    return E;
  // Future external pulses survive disconnect and report their lost tokens.
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
                                                  uint32_t Size,
                                                  bool IsWrite) const {
  for (const auto &[Table, Group] : MessageGroups)
    if (Address < Table + Group.Size && Table < Address + Size &&
        (!Group.Live || IsWrite))
      return interruptError(
          "message table is read-only live connection storage");
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
          Device.Interrupts[Connection.ResourceIndex].ID == Input.InterruptID &&
          Connection.ResourceMessage == Input.MessageID) {
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
        Event.Vector = assignment(Connection).TranslatedVector;
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
    Observation.MessageID = Inputs[I].MessageID;
    Observation.Epoch = Event.Epoch;
    Observation.DueAt100ns = Event.Due;
    Result.Interrupts.push_back(std::move(Observation));
    Events.emplace(Event.Observation, Event);
  }
  return llvm::Error::success();
}

bool KernelInterrupts::passiveAvailable(uint64_t Object, uint64_t Token) const {
  const auto *Connection = connection(Object);
  if (!Connection || !Connection->Passive)
    return true;
  const uint64_t Identity = lockIdentity(Object);
  for (const auto &Hold : Holds)
    if (lockIdentity(Hold.Object) == Identity &&
        !(Token && Hold.Kind == HoldKind::Callback && Hold.Owner == Token))
      return false;
  for (const auto &[ID, Call] : Calls) {
    if (Token && ID >= Token)
      continue;
    if (lockIdentity(Call.Object) == Identity)
      return false;
    for (uint64_t Member : Call.Chain)
      if (lockIdentity(Member) == Identity)
        return false;
  }
  return true;
}

bool KernelInterrupts::runnable(const Event &Event) const {
  return std::all_of(Event.Chain.begin(), Event.Chain.end(),
                     [&](uint64_t Object) { return passiveAvailable(Object); });
}

std::optional<uint64_t> KernelInterrupts::nextEventTime() const {
  std::optional<uint64_t> Time;
  for (const auto &[Index, Event] : Events) {
    (void)Index;
    if (!Event.Queued &&
        (Event.Action != DriverInterruptAction::Pulse || !Event.Observed ||
         runnable(Event)) &&
        (!Time || Event.Due < *Time))
      Time = Event.Due;
  }
  for (const auto &[Vector, Line] : LevelLines)
    if (!Line.Queued && !Line.Sources.empty() &&
        runnable(Line.Sources.begin()->second) && (!Time || Line.Due < *Time))
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
    if (Event.Queued || Event.Due > Time ||
        (Event.Action == DriverInterruptAction::Pulse && !runnable(Event)))
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
  std::set<uint64_t> Claimed;
  auto Redundant = [&](size_t Index) {
    const auto &Event = Events.at(Index);
    for (uint64_t Object : Event.Chain)
      if (const auto *Connection = connection(Object);
          Connection && Connection->Passive &&
          Claimed.count(lockIdentity(Object)))
        return true;
    for (uint64_t Object : Event.Chain)
      if (const auto *Connection = connection(Object);
          Connection && Connection->Passive)
        Claimed.insert(lockIdentity(Object));
    return false;
  };
  Plan.Pulses.erase(
      std::remove_if(Plan.Pulses.begin(), Plan.Pulses.end(), Redundant),
      Plan.Pulses.end());
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
    if (!Line.Queued && !Line.Sources.empty() && Line.Due <= Time &&
        runnable(Line.Sources.begin()->second))
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
  std::vector<size_t> Arrivals;
  for (auto &[Index, Event] : Events)
    if (!Event.Observed && Event.Action == DriverInterruptAction::Pulse &&
        Event.Due <= Now) {
      if (auto E = validateEvent(Event, true))
        return deliveryError(Event, llvm::toString(std::move(E)), Now);
      Arrivals.push_back(Index);
    }
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
  for (size_t Index : Arrivals) {
    Events.at(Index).Observed = true;
    Result.Interrupts[Index].OccurredAt100ns = Now;
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
  Delivery.Call = serviceCall(Token, *Connection);
  return std::optional<KernelInterrupts::Delivery>{std::move(Delivery)};
}

KernelGuestCall
KernelInterrupts::serviceCall(uint64_t Token,
                              const Connection &Connection) const {
  KernelGuestCall Call{{GuestCallOwner::Interrupt, Token},
                       Connection.Routine,
                       {Connection.Object, Connection.Context}};
  if (Connection.MessageID)
    Call.Arguments.push_back(*Connection.MessageID);
  return Call;
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

llvm::Expected<bool> KernelInterrupts::reserveSynchronization(uint64_t Token) {
  auto It = Calls.find(Token);
  if (It == Calls.end() || It->second.Observation || It->second.Begun ||
      !connection(It->second.Object)->Passive)
    return interruptError("passive synchronization lost its waiting callback");
  auto &Call = It->second;
  if (Call.Reserved)
    return true;
  if (!passiveAvailable(Call.Object, Token))
    return false;
  Holds.push_back({Call.Object, Token, 0, HoldKind::Callback});
  Call.Reserved = true;
  return true;
}

llvm::Expected<uint8_t>
KernelInterrupts::beginCall(uint64_t Token, uint8_t CallerIRQL, uint64_t Now) {
  auto It = Calls.find(Token);
  if (It == Calls.end() || It->second.Begun)
    return interruptError("callback entry lost its prepared continuation");
  auto &Call = It->second;
  if (!Call.Reserved) {
    if (!passiveAvailable(Call.Object, Token))
      return interruptError("passive interrupt synchronization event is owned");
    if (auto E = canHold(Call.Object, CallerIRQL))
      return E;
    Holds.push_back({Call.Object, Token, CallerIRQL, HoldKind::Callback});
  } else if (CallerIRQL) {
    return interruptError("passive synchronization requires PASSIVE_LEVEL");
  }
  Call.Begun = true;
  if (Call.Observation) {
    auto &Observation = Result.Interrupts[*Call.Observation];
    if (!Observation.DeliveredAt100ns)
      Observation.DeliveredAt100ns = Now;
    Observation.Handlers.push_back({Call.Object,
                                    Now,
                                    {},
                                    {},
                                    Call.DeliveryIndex,
                                    connection(Call.Object)->MessageID});
  }
  return connection(Call.Object)->SynchronizeIRQL;
}

llvm::Expected<KernelInterrupts::CallbackReturn>
KernelInterrupts::finishCall(uint64_t Token, uint64_t Value,
                             uint8_t CurrentIRQL, uint64_t Now) {
  auto It = Calls.find(Token);
  auto Owned = std::find_if(Holds.begin(), Holds.end(), [&](const Hold &Hold) {
    return Hold.Kind == HoldKind::Callback && Hold.Owner == Token;
  });
  if (It == Calls.end() || !It->second.Begun || Owned == Holds.end() ||
      Owned->Object != It->second.Object ||
      (!connection(It->second.Object)->Passive &&
       std::next(Owned) != Holds.end()) ||
      CurrentIRQL != connection(It->second.Object)->SynchronizeIRQL)
    return interruptError("callback return lost its interrupt lock or IRQL");
  CallbackReturn Return{uint8_t(Value), Owned->OldIRQL, {}};
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
      Holds.erase(Owned);
      Call.Object = Chain[Call.Handler];
      Call.Begun = false;
      Call.Reserved = false;
      const auto *Connection = connection(Call.Object);
      Return.Next = serviceCall(Token, *Connection);
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
  Holds.erase(Owned);
  Calls.erase(It);
  return Return;
}

llvm::Expected<uint8_t> KernelInterrupts::acquire(uint64_t Object,
                                                  uint64_t Execution,
                                                  uint8_t CurrentIRQL) {
  if (const auto *Connection = connection(Object);
      Connection && Connection->Passive)
    return interruptError(
        "passive interrupts do not have an interrupt spin lock");
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
