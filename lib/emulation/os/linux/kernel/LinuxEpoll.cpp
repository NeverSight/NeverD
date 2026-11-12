//===- LinuxEpoll.cpp - Workload-owned level-triggered epoll --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared open-description registrations and deterministic event delivery.
#include "LinuxFiles.h"
#include "LinuxTime.h"
#include "LinuxUserMemory.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation::linux_model {
namespace {
constexpr uint32_t OneShot = 1u << 30;
unsigned eventSize(GuestArchitecture ISA) {
  return ISA == GuestArchitecture::X64 ? 12 : 16;
}
} // namespace

struct LinuxFiles::EpollState {
  struct Registration {
    uint64_t ID, Data;
    uint32_t FD, Events;
    std::weak_ptr<Descriptor> Owner;
    bool Enabled = true;
  };
  std::vector<Registration> Entries;
  uint64_t NextID = 1, Cursor = 0;
};

class LinuxFiles::EpollWait final : public KernelWait {
public:
  EpollWait(LinuxFiles &Files, std::shared_ptr<EpollState> State)
      : Files(Files), State(std::move(State)) {}
  LinuxFiles &Files;
  std::shared_ptr<EpollState> State;
  uint64_t Output = 0;
  uint32_t Maximum = 0;
  bool Immediate = false;
  std::optional<uint64_t> Deadline;
  struct ReadyEntry {
    size_t Index;
    uint32_t Events;
  };
  struct Observation {
    std::vector<ReadyEntry> Ready;
    std::optional<uint64_t> Wake;
    ReadinessFailure Unknown;
  };
  Observation observe() const {
    Observation O;
    const auto &Entries = State->Entries;
    size_t Start =
        std::upper_bound(Entries.begin(), Entries.end(), State->Cursor,
                         [](uint64_t ID, const auto &E) { return ID < E.ID; }) -
        Entries.begin();
    for (size_t Offset = 0; Offset < Entries.size(); ++Offset) {
      const auto Index = (Start + Offset) % Entries.size();
      const auto &E = Entries[Index];
      auto Owner = E.Owner.lock();
      if (!Owner || !E.Enabled)
        continue;
      const auto R = Files.descriptorReadiness(*Owner, E.Events & ~OneShot);
      if (R.Unknown) {
        O.Unknown = R.Unknown;
        return O;
      }
      if (R.Available)
        O.Ready.push_back({Index, R.Available});
      if (R.Wake && (!O.Wake || *R.Wake < *O.Wake))
        O.Wake = R.Wake;
    }
    return O;
  }
  bool ready() const override {
    const auto O = observe();
    return O.Unknown || !O.Ready.empty() || Immediate ||
           (Deadline && Files.Clock->elapsed() >= *Deadline);
  }
  std::optional<uint64_t> nextWake() const override {
    const auto O = observe();
    if (O.Unknown)
      return std::nullopt;
    if (Deadline && (!O.Wake || *Deadline < *O.Wake))
      return Deadline;
    return O.Wake;
  }
  llvm::Expected<std::optional<uint64_t>> interrupt(ProcessResult &) override {
    // No event was delivered: preserve output, rotation and one-shot state.
    // epoll waits do not restart after a caught handler.
    return std::optional<uint64_t>(uint64_t(0) - InterruptedCall);
  }
  llvm::Expected<std::optional<uint64_t>>
  resume(ProcessResult &Result) override {
    using Reply = std::optional<uint64_t>;
    const auto O = observe();
    if (O.Unknown)
      return O.Unknown.stop(Result);
    if (O.Ready.empty() && !Immediate &&
        (!Deadline || Files.Clock->elapsed() < *Deadline))
      return Reply();
    uint64_t Done = 0;
    const unsigned Size = eventSize(Files.CPU.architecture());
    for (const auto &Ready : O.Ready) {
      if (Done == Maximum)
        break;
      auto &E = State->Entries[Ready.Index];
      uint8_t Event[4], Data[8];
      llvm::support::endian::write32le(Event, Ready.Events);
      llvm::support::endian::write64le(Data, E.Data);
      // Linux writes events and data separately. ARM64's padding is untouched;
      // earlier completed entries and fields survive a later output fault.
      for (auto [Offset, Bytes] :
           {std::pair{0u, llvm::ArrayRef<uint8_t>(Event)},
            std::pair{Size - 8, llvm::ArrayRef<uint8_t>(Data)}}) {
        auto Stored = writeUserMemory(Files.CPU, Files.Layout,
                                      Output + Done * Size + Offset, Bytes);
        if (!Stored)
          return Stored.takeError();
        if (*Stored == UserWriteResult::MixedAccess)
          return unsupported(Result, "partial epoll event field is unmodeled");
        if (*Stored == UserWriteResult::BadAddress)
          return Reply(Done ? Done : uint64_t(0) - BadAddress);
      }
      ++Done;
      State->Cursor = E.ID;
      if (E.Events & OneShot)
        E.Enabled = false;
    }
    return Reply(Done);
  }
};

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::epoll(ServiceKind Kind, const ProcessServiceEvent &Event,
                  ProcessResult &Result, ThreadContext *Thread) {
  using Reply = std::optional<uint64_t>;
  const auto &A = Event.Arguments;
  if (!Options)
    return unsupported(Result, FileInputsMissing);
  if (Kind == ServiceKind::EpollCreate || Kind == ServiceKind::EpollCreate1) {
    const auto Flags = Kind == ServiceKind::EpollCreate1 ? uint32_t(A[0]) : 0;
    if ((Kind == ServiceKind::EpollCreate && int32_t(A[0]) <= 0) ||
        (Flags & ~OpenCloseOnExec))
      return Reply(uint64_t(0) - InvalidArgument);
    const auto FD = nextDescriptor();
    if (FD == Options->DescriptorLimit)
      return Reply(uint64_t(0) - TooManyFiles);
    Descriptors.emplace(
        FD, std::make_shared<Descriptor>(std::make_shared<EpollState>()));
    opened(FD, Flags);
    return Reply(FD);
  }
  const unsigned Size = eventSize(CPU.architecture());
  auto Readable = [&](uint64_t Address,
                      uint64_t Bytes) -> llvm::Expected<bool> {
    if (Address > Layout.UserLimit || Bytes > Layout.UserLimit - Address)
      return false;
    return CPU.canAccess(Address, Bytes, Read | UserAccessible);
  };
  uint8_t Bytes[16]{};
  if (Kind == ServiceKind::EpollCtl) {
    // The syscall imports ADD/MOD events before descriptor validation; DEL
    // ignores its pointer. Unknown operations still import an event in Linux.
    if (int32_t(A[1]) != 2) {
      auto Access = Readable(A[3], Size);
      if (!Access)
        return Access.takeError();
      if (!*Access)
        return Reply(uint64_t(0) - BadAddress);
      if (auto E =
              CPU.read(A[3], llvm::MutableArrayRef(Bytes).take_front(Size)))
        return std::move(E);
    }
  } else {
    if (Kind == ServiceKind::EpollPwait && A[4]) {
      if (A[5] != SignalMaskSize)
        return Reply(uint64_t(0) - InvalidArgument);
      auto Access = Readable(A[4], SignalMaskSize);
      if (!Access)
        return Access.takeError();
      if (!*Access)
        return Reply(uint64_t(0) - BadAddress);
      return unsupported(Result, "temporary epoll signal masks are unmodeled");
    }
    const int32_t Maximum = int32_t(A[2]);
    if (Maximum <= 0 || uint32_t(Maximum) > INT32_MAX / Size)
      return Reply(uint64_t(0) - InvalidArgument);
    if (A[1] > Layout.UserLimit ||
        uint64_t(Maximum) * Size > Layout.UserLimit - A[1])
      return Reply(uint64_t(0) - BadAddress);
  }
  auto Found = Descriptors.find(uint32_t(A[0]));
  if (Found == Descriptors.end())
    return Reply(uint64_t(0) - BadDescriptor);
  auto *Owner = std::get_if<std::shared_ptr<EpollState>>(Found->second.get());
  if (Kind == ServiceKind::EpollCtl) {
    const auto Target = Descriptors.find(uint32_t(A[2]));
    if (Target == Descriptors.end())
      return Reply(uint64_t(0) - BadDescriptor);
    const auto &Description = *Target->second;
    if (std::holds_alternative<OpenFile>(Description) ||
        std::holds_alternative<OpenProcFile>(Description) ||
        std::holds_alternative<OpenDirectory>(Description))
      return Reply(uint64_t(0) - PermissionDenied);
    if (!Owner || Found->second == Target->second)
      return Reply(uint64_t(0) - InvalidArgument);
    if (!std::holds_alternative<OpenSocket>(Description) &&
        !std::holds_alternative<ProcessDescriptor>(Description))
      return unsupported(Result,
                         "epoll registration for this descriptor is unmodeled");
    auto &State = **Owner;
    std::erase_if(State.Entries,
                  [](const auto &E) { return E.Owner.expired(); });
    auto I = std::find_if(
        State.Entries.begin(), State.Entries.end(), [&](const auto &E) {
          return E.FD == uint32_t(A[2]) && E.Owner.lock() == Target->second;
        });
    const int32_t Operation = int32_t(A[1]);
    if (Operation < 1 || Operation > 3)
      return Reply(uint64_t(0) - InvalidArgument);
    if (Operation == 1 && I != State.Entries.end())
      return Reply(uint64_t(0) - AlreadyExists);
    if (Operation != 1 && I == State.Entries.end())
      return Reply(uint64_t(0) - NoEntry);
    if (Operation == 2) {
      State.Entries.erase(I);
      return Reply(0);
    }
    const uint32_t Events = llvm::support::endian::read32le(Bytes);
    if (Events & (0xf0000000u & ~OneShot))
      return unsupported(
          Result, "edge, exclusive and wakeup epoll modes are unmodeled");
    if (State.Entries.size() >= FileDescriptorLimit && Operation == 1)
      return unsupported(Result, "epoll registration budget exhausted");
    const uint64_t Data = llvm::support::endian::read64le(Bytes + Size - 8);
    if (Operation == 1)
      State.Entries.push_back(
          {State.NextID++, Data, uint32_t(A[2]), Events, Target->second});
    else {
      I->Data = Data;
      I->Events = Events;
      I->Enabled = true;
    }
    return Reply(0);
  }
  if (!Owner)
    return Reply(uint64_t(0) - InvalidArgument);
  auto Pending = std::make_unique<EpollWait>(*this, *Owner);
  Pending->Output = A[1];
  Pending->Maximum = uint32_t(A[2]);
  const int32_t MS = int32_t(A[3]);
  Pending->Immediate = MS == 0;
  if (MS > 0) {
    if (!Clock || !Clock->advancesOnIdle())
      return unsupported(Result,
                         "positive epoll timeout requires virtual idle time");
    Pending->Deadline =
        Clock->deadline({MS / 1000, (MS % 1000) * 1000000}, Result);
    if (!Pending->Deadline)
      return Reply();
  }
  return awaitDescriptors(std::move(Pending), Result, Thread);
}
} // namespace neverd::emulation::linux_model
