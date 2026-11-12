//===- LinuxSocketReceive.cpp - Retained offline receive operations -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Direct socket waits share arrival cursors and the kernel wait scheduler.

#include "LinuxFiles.h"
#include "LinuxTime.h"

namespace neverd::emulation::linux_model {
std::optional<uint64_t> LinuxFiles::receiveBuffer(uint64_t Address,
                                                  uint64_t Size,
                                                  ProcessResult &Result) const {
  // The syscall imports its complete user extent before waiting, even at EOF
  // or with a zero count. Page permissions are checked only during the copy.
  // Oversized read/recv extents have different clamp/admission rules; keep
  // those outside this bounded common contract.
  const uint64_t MaxCount = uint64_t(INT32_MAX) & ~(Layout.PageSize - 1);
  if (Size > MaxCount)
    return unsupported(Result, "socket receive extent exceeds MAX_RW_COUNT");
  if (Address > Layout.UserLimit || Size > Layout.UserLimit - Address)
    return uint64_t(0) - BadAddress;
  return 0;
}

class LinuxFiles::ReceiveWait final : public KernelWait {
public:
  ReceiveWait(LinuxFiles &Files, std::shared_ptr<Descriptor> Owner,
              uint64_t Address, uint64_t Size, uint32_t Flags,
              uint64_t PeerAddress, uint64_t PeerLength)
      : Files(Files), Owner(std::move(Owner)), Address(Address), Size(Size),
        Flags(Flags), PeerAddress(PeerAddress), PeerLength(PeerLength) {}
  LinuxFiles &Files;
  // Linux retains the open description during blocking I/O, independently of
  // close/dup2 and reuse of the original descriptor number in another thread.
  std::shared_ptr<Descriptor> Owner;
  uint64_t Address, Size;
  uint32_t Flags;
  uint64_t PeerAddress, PeerLength;
  std::optional<uint64_t> Deadline;
  bool MayReturnAgain = false;

  bool restartOnSignal(const LinuxSignalAction &Action) const override {
    // sock_intr_errno uses the timeout captured on entry, not a subsequently
    // changed SO_RCVTIMEO. This wait has not copied or consumed any bytes.
    return !Deadline && (Action.Flags & 0x10000000); // SA_RESTART
  }
  llvm::Expected<std::optional<uint64_t>> interrupt(ProcessResult &) override {
    return std::optional<uint64_t>(uint64_t(0) - InterruptedCall);
  }

  OpenSocket &socket() const { return std::get<OpenSocket>(*Owner); }
  const LinuxNetworkReceive *event() const {
    const auto &S = socket();
    const auto &Events =
        S.Datagram ? S.Datagram->ReceiveEvents : S.Connection->ReceiveEvents;
    return S.ReceiveEvent < Events.size() ? &Events[S.ReceiveEvent] : nullptr;
  }
  Readiness observe() const {
    // Explicit completion errors are receive outcomes, not poll observations.
    if (const auto *E = event(); E && E->Error && socket().Sent >= E->AfterSend)
      return {1, {}, {}};
    return Files.socketReadiness(socket(), 1);
  }
  bool ready() const override {
    const auto R = observe();
    return R.Unknown || (R.Available & 1) ||
           (Deadline && Files.Clock->elapsed() >= *Deadline);
  }
  std::optional<uint64_t> nextWake() const override {
    const auto R = observe();
    if (R.Unknown)
      return std::nullopt;
    if (Deadline && (!R.Wake || *Deadline < *R.Wake))
      return Deadline;
    return R.Wake;
  }
  llvm::Expected<std::optional<uint64_t>>
  resume(ProcessResult &Result) override {
    const auto R = observe();
    if (R.Unknown)
      return R.Unknown.stop(Result);
    // An arrival at the deadline is available before the timeout check.
    if (!(R.Available & 1)) {
      if (Deadline && Files.Clock->elapsed() >= *Deadline)
        return std::optional<uint64_t>(uint64_t(0) - 11);
      return unsupported(Result, "receive resumed before a readiness event");
    }
    auto &S = socket();
    if (const auto *E = event(); E && E->Error)
      return Files.receiveSocketError(S, *E, MayReturnAgain, Result);
    // The existing transfer owner rechecks guest output memory now, including
    // UDP addrlen. Neither readiness queries nor suspension consume bytes.
    if (S.Datagram)
      return Files.transferDatagram(S, false, Address, Size, Result, Flags,
                                    PeerAddress, PeerLength);
    return Files.transferSocket(S, false, Address, Size, Result, Flags);
  }
};

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::receiveSocket(std::shared_ptr<Descriptor> Owner, uint64_t Address,
                          uint64_t Size, ProcessResult &Result,
                          ThreadContext *Thread, uint32_t Flags,
                          uint64_t PeerAddress, uint64_t PeerLength) {
  using Reply = std::optional<uint64_t>;
  const auto &S = std::get<OpenSocket>(*Owner);
  const bool Datagram = (S.Flags & 0xf) == 2;
  // UDP WAITALL is ignored, and PEEK/TRUNC retain packet semantics. TCP
  // WAITALL/PEEK would need separate partial-progress contracts; refuse them.
  const uint32_t Allowed = Datagram ? 2 | 0x20 | 0x40 | 0x100 : 0x40 | 0x4000;
  if (Flags & ~Allowed)
    return unsupported(Result, "unmodeled socket receive flags");
  if (!Datagram && (PeerAddress || PeerLength))
    return unsupported(Result, "TCP explicit peer output is unmodeled");
  if (Datagram ? !S.Datagram : !S.Connection)
    return unsupported(Result,
                       "receive requires a completed offline connection");
  if (!Datagram && !Size)
    return Reply(0);

  const bool Nonblocking = (Flags & 0x40) || (S.Flags & OpenNonBlock);
  const auto Seconds = S.ReceiveTimeoutSeconds;
  auto Pending = std::make_unique<ReceiveWait>(
      *this, std::move(Owner), Address, Size, Flags, PeerAddress, PeerLength);
  Pending->MayReturnAgain = Nonblocking || Seconds;
  if (Pending->ready())
    return Pending->resume(Result);
  // Known absence permits EAGAIN. An unknown observation above never does.
  if (Nonblocking || Seconds < 0)
    return Reply(uint64_t(0) - 11);
  if (Seconds > 0) {
    if (!Clock || !Clock->advancesOnIdle())
      return unsupported(Result,
                         "blocking receive requires a readiness schedule");
    Pending->Deadline = Clock->deadline({Seconds, 0}, Result);
    if (!Pending->Deadline)
      return Reply();
  }
  if (Thread) {
    assert(!Thread->Pending && !Thread->Exit);
    Thread->Pending = std::move(Pending);
    return Reply();
  }
  // No runnable guest thread remains in a single-thread workload.
  if (!Clock || !Clock->advancesOnIdle() || !Pending->nextWake())
    return unsupported(Result,
                       "blocking receive requires a readiness schedule");
  if (!Clock->advanceTo(*Pending->nextWake(), Result))
    return Reply();
  return Pending->resume(Result);
}
} // namespace neverd::emulation::linux_model
