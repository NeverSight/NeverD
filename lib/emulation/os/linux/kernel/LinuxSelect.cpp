//===- LinuxSelect.cpp - Scheduled select over owned descriptions --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// LP64 pselect6 admission, retained waits and ordered completion.

#include "LinuxFiles.h"
#include "LinuxTime.h"
#include "LinuxUserMemory.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation::linux_model {
class LinuxFiles::SelectWait final : public KernelWait {
public:
  explicit SelectWait(LinuxFiles &Files) : Files(Files) {}
  LinuxFiles &Files;
  std::array<uint64_t, 3> Input{}, Addresses{};
  std::map<uint32_t, std::shared_ptr<Descriptor>> Owners;
  uint64_t Timeout = 0;
  std::optional<uint64_t> Deadline;
  size_t Size = 0;
  bool Immediate = false;

  struct Observation {
    std::array<uint64_t, 3> Output{};
    uint64_t Count = 0;
    std::optional<uint64_t> Wake;
    ReadinessFailure Unknown;
  };
  Observation observe() const {
    Observation O;
    for (const auto &[FD, Owner] : Owners) {
      const auto I = Files.Descriptors.find(FD);
      if (I == Files.Descriptors.end() || I->second != Owner) {
        O.Unknown = {"select descriptor changed while waiting"};
        return O;
      }
      const uint64_t Bit = uint64_t(1) << FD;
      unsigned Wanted = 0;
      for (unsigned J = 0; J != 3; ++J)
        if (Input[J] & Bit)
          Wanted |= 1u << J;
      Readiness R;
      if (const auto *Socket = std::get_if<OpenSocket>(Owner.get()))
        R = Files.socketReadiness(*Socket, Wanted);
      else if (std::holds_alternative<OpenFile>(*Owner))
        R.Available = Wanted & 3;
      else if (!std::holds_alternative<ProcessDescriptor>(*Owner))
        R.Unknown = {"select readiness for this descriptor is unmodeled"};
      if (R.Unknown) {
        O.Unknown = R.Unknown;
        return O;
      }
      if (R.Wake && (!O.Wake || *R.Wake < *O.Wake))
        O.Wake = R.Wake;
      for (unsigned J = 0; J != 3; ++J)
        if (R.Available & (1u << J)) {
          O.Output[J] |= Bit;
          ++O.Count; // Count bits across sets, not distinct descriptors.
        }
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
    if (Deadline) {
      const uint64_t Now = Files.Clock->elapsed();
      const uint64_t Remaining = Now < *Deadline ? *Deadline - Now : 0;
      uint8_t Bytes[16];
      llvm::support::endian::write64le(Bytes, Remaining / 1000000000);
      llvm::support::endian::write64le(Bytes + 8, Remaining % 1000000000);
      auto Stored = writeUserMemory(Files.CPU, Files.Layout, Timeout, Bytes);
      if (!Stored)
        return Stored.takeError();
      if (*Stored == UserWriteResult::MixedAccess)
        return unsupported(Result,
                           "partial select timeout output is unmodeled");
      // The kernel ignores an entirely unwritable remaining-time output.
    }
    return std::optional<uint64_t>(Return);
  }
  llvm::Expected<std::optional<uint64_t>>
  interrupt(ProcessResult &Result) override {
    // core_sys_select skips fd-set copy-out when interrupted without ready
    // bits, but pselect6 still updates its remaining timeout.
    return finish(uint64_t(0) - InterruptedCall, Result);
  }
  llvm::Expected<std::optional<uint64_t>>
  resume(ProcessResult &Result) override {
    const auto O = observe();
    if (O.Unknown)
      return O.Unknown.stop(Result);
    if (!O.Count && !Immediate &&
        (!Deadline || Files.Clock->elapsed() < *Deadline))
      return unsupported(Result, "select resumed before a readiness event");
    // Inputs were captured before suspension. Recheck output access now, and
    // retain read/write/except/timeout copy order even when pointers alias.
    for (unsigned I = 0; I != 3; ++I) {
      if (!Addresses[I])
        continue;
      uint8_t Bytes[8];
      llvm::support::endian::write64le(Bytes, O.Output[I]);
      auto Stored = writeUserMemory(Files.CPU, Files.Layout, Addresses[I],
                                    llvm::ArrayRef(Bytes).take_front(Size));
      if (!Stored)
        return Stored.takeError();
      if (*Stored == UserWriteResult::MixedAccess)
        return unsupported(Result, "partial select bitmap output is unmodeled");
      if (*Stored == UserWriteResult::BadAddress)
        return finish(uint64_t(0) - BadAddress, Result);
    }
    return finish(O.Count, Result);
  }
};

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::select(const ProcessServiceEvent &Event, ProcessResult &Result,
                   ThreadContext *Thread) {
  using Reply = std::optional<uint64_t>;
  const auto &A = Event.Arguments;
  auto ReadBytes =
      [&](uint64_t Address,
          llvm::MutableArrayRef<uint8_t> Bytes) -> llvm::Expected<bool> {
    if (Address > Layout.UserLimit || Bytes.size() > Layout.UserLimit - Address)
      return false;
    if (Bytes.empty())
      return true;
    auto Allowed = CPU.canAccess(Address, Bytes.size(), Read | UserAccessible);
    if (!Allowed)
      return Allowed.takeError();
    if (!*Allowed)
      return false;
    if (auto E = CPU.read(Address, Bytes))
      return std::move(E);
    return true;
  };
  uint64_t MaskAddress = 0, MaskSize = 0;
  if (A[5]) {
    uint8_t Wrapper[16];
    auto Readable = ReadBytes(A[5], Wrapper);
    if (!Readable)
      return Readable.takeError();
    if (!*Readable)
      return Reply(uint64_t(0) - BadAddress);
    MaskAddress = llvm::support::endian::read64le(Wrapper);
    MaskSize = llvm::support::endian::read64le(Wrapper + 8);
  }
  uint8_t TimeBytes[16]{};
  auto Pending = std::make_unique<SelectWait>(*this);
  Pending->Timeout = A[4];
  for (unsigned I = 0; I != 3; ++I)
    Pending->Addresses[I] = A[I + 1];
  if (A[4]) {
    auto Readable = ReadBytes(A[4], TimeBytes);
    if (!Readable)
      return Readable.takeError();
    if (!*Readable)
      return Reply(uint64_t(0) - BadAddress);
    const LinuxTimespec Duration{
        int64_t(llvm::support::endian::read64le(TimeBytes)),
        int64_t(llvm::support::endian::read64le(TimeBytes + 8))};
    if (Duration.Seconds < 0 || !isNormalizedTimespec(Duration))
      return Reply(uint64_t(0) - InvalidArgument);
    const bool Positive = Duration.Seconds || Duration.Nanoseconds;
    Pending->Immediate = !Positive;
    if (Positive) {
      if (!Clock || !Clock->advancesOnIdle())
        return unsupported(
            Result, "positive select timeout requires virtual idle time");
      Pending->Deadline = Clock->deadline(Duration, Result);
      if (!Pending->Deadline)
        return Reply();
    }
  }
  if (MaskAddress) {
    if (MaskSize != SignalMaskSize)
      return Reply(uint64_t(0) - InvalidArgument);
    uint8_t Mask[8];
    auto Readable = ReadBytes(MaskAddress, Mask);
    if (!Readable)
      return Readable.takeError();
    if (!*Readable)
      return Reply(uint64_t(0) - BadAddress);
    return unsupported(Result, "temporary select signal masks are unmodeled");
  }
  auto Finish = [&](uint64_t Return) {
    return Pending->finish(Return, Result);
  };
  const int32_t Count = int32_t(A[0]);
  if (Count < 0)
    return Finish(uint64_t(0) - InvalidArgument);
  // Linux clips to its allocated descriptor-table capacity, not RLIMIT_NOFILE.
  // The first LP64 word is always present. Larger table allocation history is
  // not modeled and must not be inferred from the highest currently open FD.
  if (Count > 64)
    return unsupported(
        Result, "select beyond the initial descriptor word is unmodeled");
  const uint64_t Valid = Count == 64 ? UINT64_MAX : (uint64_t(1) << Count) - 1;
  auto &Input = Pending->Input;
  const size_t Size = Pending->Size = Count ? 8 : 0;
  for (unsigned I = 0; I != 3; ++I) {
    if (!A[I + 1])
      continue;
    uint8_t Bytes[8]{};
    auto Readable =
        ReadBytes(A[I + 1], llvm::MutableArrayRef(Bytes).take_front(Size));
    if (!Readable)
      return Readable.takeError();
    if (!*Readable)
      return Finish(uint64_t(0) - BadAddress);
    Input[I] = llvm::support::endian::read64le(Bytes) & Valid;
  }
  const uint64_t Requested = Input[0] | Input[1] | Input[2];
  for (unsigned FD = 0; FD < unsigned(Count); ++FD) {
    if (!(Requested & (uint64_t(1) << FD)))
      continue;
    const auto I = Descriptors.find(FD);
    if (I == Descriptors.end())
      return Finish(uint64_t(0) - BadDescriptor);
    Pending->Owners.emplace(FD, I->second);
  }
  if (Pending->ready())
    return Pending->resume(Result);
  if (Thread) {
    assert(!Thread->Pending && !Thread->Exit);
    Thread->Pending = std::move(Pending);
    return Reply();
  }
  // A single-thread workload is already idle. Threaded workloads let the
  // shared scheduler run every other runnable owner before advancing time.
  if (!Clock || !Clock->advancesOnIdle() || !Pending->nextWake())
    return unsupported(Result,
                       "blocking select waits require a readiness schedule");
  if (!Clock->advanceTo(*Pending->nextWake(), Result))
    return Reply();
  return Pending->resume(Result);
}
} // namespace neverd::emulation::linux_model
