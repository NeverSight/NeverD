//===- LinuxSelect.cpp - Immediate select over owned descriptions --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// LP64 pselect6 admission and non-consuming offline readiness queries.

#include "LinuxFiles.h"
#include "LinuxTime.h"
#include "LinuxUserMemory.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation::linux_model {
std::optional<unsigned>
LinuxFiles::socketReadiness(const OpenSocket &Socket, unsigned Wanted,
                            ProcessResult &Result) const {
  const bool Datagram = (Socket.Flags & 0xf) == 2;
  auto Unknown = [&]() -> std::optional<unsigned> {
    unsupported(Result, "socket select readiness has no offline observation");
    return std::nullopt;
  };
  if ((Datagram ? !Socket.Datagram : !Socket.Connection) || (Wanted & 4))
    return Unknown();
  unsigned Ready = 0;
  if (Wanted & 2) {
    const auto Size = Datagram ? Socket.Datagram->ExpectedSend.size()
                               : Socket.Connection->ExpectedSend.size();
    if (Socket.Sent >= Size)
      return Unknown();
    Ready |= 2;
  }
  if (Wanted & 1) {
    const auto &Events = Datagram ? Socket.Datagram->ReceiveEvents
                                  : Socket.Connection->ReceiveEvents;
    if (Socket.ReceiveEvent < Events.size()) {
      const auto &E = Events[Socket.ReceiveEvent];
      // A syscall completion error is not a poll-state observation. Nor does a
      // future send prerequisite prove absence of unrelated incoming bytes.
      if (Socket.Sent < E.AfterSend || E.Error)
        return Unknown();
      Ready |= 1; // Includes a queued zero-length UDP datagram.
    } else if (!Datagram &&
               (Socket.Received < Socket.Connection->Receive.size() ||
                Socket.Connection->ReceiveEOF)) {
      Ready |= 1;
    } else {
      return Unknown();
    }
  }
  return Ready;
}

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::select(const ProcessServiceEvent &Event, ProcessResult &Result) {
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
  bool Positive = false, Immediate = false;
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
    Positive = Duration.Seconds || Duration.Nanoseconds;
    Immediate = !Positive;
    if (Positive) {
      if (!Clock || !Clock->advancesOnIdle())
        return unsupported(
            Result, "positive select timeout requires virtual idle time");
      if (!Clock->deadline(Duration, Result))
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
  // Only immediate completion is admitted. No host time elapses within a
  // service. The kernel's remaining-time copy still occurs after bitmap output
  // or an ordinary core error; an entirely unwritable timeout is ignored.
  auto Finish = [&](uint64_t Return) -> llvm::Expected<Reply> {
    if (Positive) {
      auto Stored = writeUserMemory(CPU, Layout, A[4], TimeBytes);
      if (!Stored)
        return Stored.takeError();
      if (*Stored == UserWriteResult::MixedAccess)
        return unsupported(Result,
                           "partial select timeout output is unmodeled");
    }
    return Reply(Return);
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
  uint64_t Input[3]{}, Output[3]{};
  const size_t Size = Count ? 8 : 0;
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
  for (unsigned FD = 0; FD < unsigned(Count); ++FD)
    if ((Requested & (uint64_t(1) << FD)) && !Descriptors.count(FD))
      return Finish(uint64_t(0) - BadDescriptor);
  uint64_t Ready = 0;
  for (unsigned FD = 0; FD < unsigned(Count); ++FD) {
    const uint64_t Bit = uint64_t(1) << FD;
    if (!(Requested & Bit))
      continue;
    unsigned Wanted = 0;
    for (unsigned I = 0; I != 3; ++I)
      if (Input[I] & Bit)
        Wanted |= 1u << I;
    const auto &D = *Descriptors.at(FD);
    unsigned Available = 0;
    if (const auto *Socket = std::get_if<OpenSocket>(&D)) {
      auto R = socketReadiness(*Socket, Wanted, Result);
      if (!R)
        return Reply();
      Available = *R;
    } else if (std::holds_alternative<OpenFile>(D)) {
      Available = Wanted & 3;
    } else if (!std::holds_alternative<ProcessDescriptor>(D)) {
      return unsupported(Result,
                         "select readiness for this descriptor is unmodeled");
    }
    for (unsigned I = 0; I != 3; ++I)
      if (Available & (1u << I)) {
        Output[I] |= Bit;
        ++Ready; // Count bits across all sets, not unique descriptors.
      }
  }
  if (!Ready && !Immediate)
    return unsupported(Result,
                       "blocking select waits require a readiness schedule");
  for (unsigned I = 0; I != 3; ++I) {
    if (!A[I + 1])
      continue;
    uint8_t Bytes[8];
    llvm::support::endian::write64le(Bytes, Output[I]);
    auto Stored = writeUserMemory(CPU, Layout, A[I + 1],
                                  llvm::ArrayRef(Bytes).take_front(Size));
    if (!Stored)
      return Stored.takeError();
    if (*Stored == UserWriteResult::MixedAccess)
      return unsupported(Result, "partial select bitmap output is unmodeled");
    if (*Stored == UserWriteResult::BadAddress)
      return Finish(uint64_t(0) - BadAddress);
  }
  return Finish(Ready);
}
} // namespace neverd::emulation::linux_model
