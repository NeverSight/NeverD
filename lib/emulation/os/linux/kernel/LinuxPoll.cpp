//===- LinuxPoll.cpp - Scheduled poll over owned descriptors -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxFiles.h"
#include "LinuxTime.h"
#include "LinuxUserMemory.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation::linux_model {
class LinuxFiles::PollWait final : public KernelWait {
public:
  explicit PollWait(LinuxFiles &Files) : Files(Files) {}
  LinuxFiles &Files;
  struct Entry {
    int32_t FD;
    uint16_t Events;
    std::shared_ptr<Descriptor> Owner;
  };
  std::vector<Entry> Input;
  uint64_t Output = 0, Timeout = 0;
  std::optional<uint64_t> Deadline;
  bool Immediate = false;
  struct Observation {
    std::vector<uint16_t> Events;
    uint64_t Count = 0;
    std::optional<uint64_t> Wake;
    ReadinessFailure Unknown;
  };
  Observation observe() const {
    Observation O;
    for (const auto &E : Input) {
      Readiness R;
      if (E.FD < 0) {
        O.Events.push_back(0);
        continue;
      }
      if (!E.Owner) {
        R.Available = PollInvalidDescriptor;
      } else {
        const auto I = Files.Descriptors.find(E.FD);
        if (I == Files.Descriptors.end() || I->second != E.Owner) {
          O.Unknown = {"poll descriptor changed while waiting"};
          return O;
        }
        R = Files.descriptorReadiness(*E.Owner, E.Events);
      }
      if (R.Unknown) {
        O.Unknown = R.Unknown;
        return O;
      }
      O.Events.push_back(R.Available);
      O.Count += R.Available != 0;
      if (R.Wake && (!O.Wake || *R.Wake < *O.Wake))
        O.Wake = R.Wake;
    }
    return O;
  }
  bool ready() const override {
    const auto O = observe();
    return O.Unknown || O.Count || Immediate ||
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
  llvm::Expected<std::optional<uint64_t>> finish(uint64_t Return,
                                                 ProcessResult &Result) {
    if (Timeout && Deadline) {
      const auto Now = Files.Clock->elapsed();
      const uint64_t Left = Now < *Deadline ? *Deadline - Now : 0;
      uint8_t Bytes[16];
      llvm::support::endian::write64le(Bytes, Left / 1000000000);
      llvm::support::endian::write64le(Bytes + 8, Left % 1000000000);
      auto Stored = writeUserMemory(Files.CPU, Files.Layout, Timeout, Bytes);
      if (!Stored)
        return Stored.takeError();
      if (*Stored == UserWriteResult::MixedAccess)
        return unsupported(Result, "partial ppoll timeout output is unmodeled");
      // Linux ignores a wholly inaccessible remaining-time output.
    }
    return std::optional<uint64_t>(Return);
  }
  llvm::Expected<std::optional<uint64_t>>
  resume(ProcessResult &Result) override {
    const auto O = observe();
    if (O.Unknown)
      return O.Unknown.stop(Result);
    if (!O.Count && !Immediate &&
        (!Deadline || Files.Clock->elapsed() < *Deadline))
      return std::optional<uint64_t>(); // Another consumer won; rearm.
    return copyResults(O.Events, O.Count, Result);
  }
  llvm::Expected<std::optional<uint64_t>>
  interrupt(ProcessResult &Result) override {
    // do_sys_poll copies zero revents even on EINTR, before ppoll's remaining
    // timeout. Neither poll nor ppoll restarts after a caught handler.
    return copyResults(std::vector<uint16_t>(Input.size()),
                       uint64_t(0) - InterruptedCall, Result);
  }
  llvm::Expected<std::optional<uint64_t>>
  copyResults(llvm::ArrayRef<uint16_t> Events, uint64_t Return,
              ProcessResult &Result) {
    const uint64_t Size = Input.size() * PollDescriptorSize;
    if (Output > Files.Layout.UserLimit ||
        Size > Files.Layout.UserLimit - Output)
      return finish(uint64_t(0) - BadAddress, Result);
    for (size_t I = 0; I < Input.size(); ++I) {
      uint8_t Bytes[2];
      llvm::support::endian::write16le(Bytes, Events[I]);
      auto Stored = writeUserMemory(
          Files.CPU, Files.Layout,
          Output + I * PollDescriptorSize + PollReventsOffset, Bytes);
      if (!Stored)
        return Stored.takeError();
      if (*Stored == UserWriteResult::MixedAccess)
        return unsupported(Result, PollPartialOutput);
      if (*Stored == UserWriteResult::BadAddress)
        return finish(uint64_t(0) - BadAddress, Result);
    }
    return finish(Return, Result);
  }
};

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::awaitDescriptors(std::unique_ptr<KernelWait> Pending,
                             ProcessResult &Result, ThreadContext *Thread) {
  if (Pending->ready())
    return Pending->resume(Result);
  if (Thread) {
    assert(!Thread->Pending && !Thread->Exit);
    Thread->Pending = std::move(Pending);
    return std::optional<uint64_t>();
  }
  if (!Clock || !Clock->advancesOnIdle() || !Pending->nextWake())
    return unsupported(Result,
                       "descriptor wait requires a virtual readiness schedule");
  if (!Clock->advanceTo(*Pending->nextWake(), Result))
    return std::optional<uint64_t>();
  return Pending->resume(Result);
}

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::poll(ServiceKind Kind, const ProcessServiceEvent &Event,
                 ProcessResult &Result, ThreadContext *Thread,
                 bool PreserveTimeout) {
  using Reply = std::optional<uint64_t>;
  const auto &A = Event.Arguments;
  auto Readable = [&](uint64_t Address, uint64_t Size) -> llvm::Expected<bool> {
    if (Address > Layout.UserLimit || Size > Layout.UserLimit - Address)
      return false;
    return CPU.canAccess(Address, Size, Read | UserAccessible);
  };
  auto Pending = std::make_unique<PollWait>(*this);
  Pending->Output = A[0];
  std::optional<LinuxTimespec> Duration;
  if (Kind == ServiceKind::Poll) {
    const int32_t MS = int32_t(A[2]);
    if (MS >= 0)
      Duration = LinuxTimespec{MS / 1000, (MS % 1000) * 1000000};
  } else if (A[2]) {
    auto Access = Readable(A[2], 16);
    if (!Access)
      return Access.takeError();
    if (!*Access)
      return Reply(uint64_t(0) - BadAddress);
    uint8_t Bytes[16];
    if (auto E = CPU.read(A[2], Bytes))
      return std::move(E);
    Duration =
        LinuxTimespec{int64_t(llvm::support::endian::read64le(Bytes)),
                      int64_t(llvm::support::endian::read64le(Bytes + 8))};
    if (Duration->Seconds < 0 || !isNormalizedTimespec(*Duration))
      return Reply(uint64_t(0) - InvalidArgument);
    Pending->Timeout = PreserveTimeout ? 0 : A[2];
  }
  if (Kind == ServiceKind::PPoll && A[3]) {
    if (A[4] != SignalMaskSize)
      return Reply(uint64_t(0) - InvalidArgument);
    auto Access = Readable(A[3], SignalMaskSize);
    if (!Access)
      return Access.takeError();
    if (!*Access)
      return Reply(uint64_t(0) - BadAddress);
    return unsupported(Result, PollSignalMaskUnsupported);
  }
  if (Duration) {
    Pending->Immediate = !Duration->Seconds && !Duration->Nanoseconds;
    if (!Pending->Immediate) {
      if (!Clock || !Clock->advancesOnIdle())
        return unsupported(Result,
                           "positive poll timeout requires virtual idle time");
      Pending->Deadline = Clock->deadline(*Duration, Result);
      if (!Pending->Deadline)
        return Reply();
    }
  }
  if (!Options)
    return unsupported(Result, PollLimitMissing);
  const uint32_t Count = uint32_t(A[1]);
  if (Count > Options->DescriptorLimit)
    return Pending->finish(uint64_t(0) - InvalidArgument, Result);
  const uint64_t Size = uint64_t(Count) * PollDescriptorSize;
  std::vector<uint8_t> Input(Size);
  if (Count) {
    auto Access = Readable(A[0], Size);
    if (!Access)
      return Access.takeError();
    if (!*Access)
      return Pending->finish(uint64_t(0) - BadAddress, Result);
    if (auto E = CPU.read(A[0], Input))
      return std::move(E);
  }
  for (uint32_t I = 0; I < Count; ++I) {
    const auto *P = Input.data() + I * PollDescriptorSize;
    const int32_t FD = int32_t(llvm::support::endian::read32le(P));
    auto Found = Descriptors.find(FD);
    Pending->Input.push_back(
        {FD, llvm::support::endian::read16le(P + 4),
         Found == Descriptors.end() ? nullptr : Found->second});
  }
  return awaitDescriptors(std::move(Pending), Result, Thread);
}
} // namespace neverd::emulation::linux_model
