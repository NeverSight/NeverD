//===- LinuxNetwork.cpp - Offline stream contracts -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validated virtual connection attempts and shared stream state.

#include "LinuxNetwork.h"

#include "LinuxFiles.h"

#include "neverd/emulation/Limits.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>

namespace neverd::emulation::linux_model {
namespace {
bool connectError(uint32_t Error) {
  return Error == 0 || Error == 4 || Error == 111 || Error == 110 ||
         Error == 101 || Error == 113 || Error == 13;
}
llvm::Error receiveEvents(llvm::ArrayRef<LinuxNetworkReceive> Events,
                          uint64_t SentLimit, bool Datagram, uint64_t &Bytes) {
  if (Events.size() >
      ::neverd::limits::emulation::kLinuxNetworkReceiveEventsSizeLimit)
    return failure("offline network has too many receive events");
  uint64_t Previous = 0, PreviousTime = 0;
  for (const auto &E : Events) {
    if (E.AfterSend < Previous || E.AfterSend > SentLimit ||
        (E.Error && E.Error != 4 && E.Error != 11) ||
        (E.Error && !E.Bytes.empty()) ||
        (!Datagram && !E.Error && E.Bytes.empty()) ||
        (Datagram && E.Bytes.size() > 65507))
      return failure("invalid offline receive event");
    if (E.ReadyAtNS) {
      if (E.Error || *E.ReadyAtNS > uint64_t(INT64_MAX) ||
          *E.ReadyAtNS < PreviousTime)
        return failure("invalid offline receive event time");
      PreviousTime = *E.ReadyAtNS;
    }
    Previous = E.AfterSend;
    Bytes += E.Bytes.size();
  }
  return llvm::Error::success();
}
llvm::Error receiveEnd(llvm::ArrayRef<LinuxNetworkReceive> Events,
                       std::optional<uint64_t> EOFTime,
                       std::optional<uint64_t> QuietTime) {
  uint64_t Last = 0;
  for (const auto &E : Events)
    if (E.ReadyAtNS)
      Last = *E.ReadyAtNS;
  for (auto T : {EOFTime, QuietTime})
    if (T && (*T < Last || *T > uint64_t(INT64_MAX)))
      return failure("invalid offline receive end time");
  // The quiet interval is inclusive; its first unknown nanosecond must fit.
  if (QuietTime && *QuietTime == uint64_t(INT64_MAX))
    return failure("offline receive quiet interval has no finite end");
  return llvm::Error::success();
}
} // namespace
bool hasTimedReceives(const LinuxNetworkOptions &Options) {
  auto Timed = [](const auto &C) {
    return C.ReceiveQuietUntilNS ||
           llvm::any_of(C.ReceiveEvents,
                        [](const auto &E) { return E.ReadyAtNS.has_value(); });
  };
  return llvm::any_of(
             Options.Connections,
             [&](const auto &C) { return C.ReceiveEOFAtNS || Timed(C); }) ||
         llvm::any_of(Options.DatagramConnections, Timed);
}
uint16_t networkAddressFamily(llvm::ArrayRef<uint8_t> Address) {
  if (Address.size() < 2)
    return 0;
  const auto Family = llvm::support::endian::read16le(Address.data());
  if (Family == 2 && Address.size() == 16 &&
      llvm::all_of(Address.drop_front(8), [](uint8_t V) { return V == 0; }))
    return Family;
  if (Family == 10 && Address.size() == 28)
    return Family;
  return 0;
}
llvm::Error validateNetworkOptions(const LinuxNetworkOptions &Options) {
  using ::neverd::limits::emulation::kMaxNetworkConnections;
  const auto Streams = Options.Connections.size();
  const auto Datagrams = Options.DatagramConnections.size();
  if (Streams > kMaxNetworkConnections ||
      Datagrams > kMaxNetworkConnections - Streams)
    return failure("offline network has too many connection attempts");
  if (const auto &F = Options.TCPConnectFailures) {
    if (!F->Count || !F->Error || !connectError(F->Error))
      return failure("invalid offline TCP connection failure contract");
    // Counted failures retain only a uint32_t cursor, not connection entries.
    // The explicit count and the execution budget bound their consumption.
  }
  uint64_t Bytes = 0;
  for (const auto &C : Options.Connections) {
    if (!networkAddressFamily(C.Peer) || !connectError(C.ConnectErrno) ||
        !C.MaxSend ||
        C.MaxSend > ::neverd::limits::emulation::
                        kLinuxNetworkValidateNetworkOptionsCMaxSendLimit ||
        !C.MaxReceive ||
        C.MaxReceive >
            ::neverd::limits::emulation::
                kLinuxNetworkValidateNetworkOptionsCMaxReceiveLimit ||
        (C.ConnectErrno && (!C.ExpectedSend.empty() || !C.Receive.empty() ||
                            C.ReceiveEOF || !C.ReceiveEvents.empty())) ||
        (!C.Receive.empty() && !C.ReceiveEvents.empty()))
      return failure("invalid offline TCP connection contract");
    if ((C.ReceiveEOFAtNS && !C.ReceiveEOF) ||
        (C.ReceiveQuietUntilNS && C.ReceiveEOF) ||
        (C.ConnectErrno && (C.ReceiveEOFAtNS || C.ReceiveQuietUntilNS)))
      return failure("invalid offline TCP receive schedule");
    if (auto E = receiveEnd(C.ReceiveEvents, C.ReceiveEOFAtNS,
                            C.ReceiveQuietUntilNS))
      return E;
    Bytes += C.ExpectedSend.size() + C.Receive.size();
    if (auto E =
            receiveEvents(C.ReceiveEvents, C.ExpectedSend.size(), false, Bytes))
      return E;
    if (Bytes > ::neverd::limits::emulation::
                    kLinuxNetworkValidateNetworkOptionsBytesLimit)
      return failure("offline network stream byte limit exceeded");
  }
  for (const auto &C : Options.DatagramConnections) {
    if (!networkAddressFamily(C.Peer) || !connectError(C.ConnectErrno) ||
        C.ExpectedSend.size() >
            ::neverd::limits::emulation::
                kLinuxNetworkValidateNetworkOptionsCExpectedSendSizeLimit ||
        (C.ConnectErrno &&
         (!C.ExpectedSend.empty() || !C.ReceiveEvents.empty())))
      return failure("invalid offline UDP connection contract");
    if (networkAddressFamily(C.Peer) == 10 &&
        !llvm::all_of(llvm::ArrayRef(C.Peer).slice(4, 4),
                      [](uint8_t V) { return V == 0; }))
      return failure("offline UDP requires zero IPv6 flow information");
    if (C.ConnectErrno && C.ReceiveQuietUntilNS)
      return failure("invalid offline UDP receive schedule");
    if (auto E = receiveEnd(C.ReceiveEvents, {}, C.ReceiveQuietUntilNS))
      return E;
    for (const auto &Packet : C.ExpectedSend) {
      if (Packet.size() > 65507)
        return failure("offline UDP datagram exceeds the supported size");
      Bytes += Packet.size();
    }
    if (auto E =
            receiveEvents(C.ReceiveEvents, C.ExpectedSend.size(), true, Bytes))
      return E;
    if (Bytes > ::neverd::limits::emulation::
                    kLinuxNetworkValidateNetworkOptionsBytesLimit)
      return failure("offline network byte limit exceeded");
  }
  return llvm::Error::success();
}

bool LinuxFiles::isSocket(uint32_t FD) const {
  auto I = Descriptors.find(FD);
  return I != Descriptors.end() &&
         std::holds_alternative<OpenSocket>(*I->second);
}

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::network(ServiceKind Kind, const ProcessServiceEvent &Event,
                    const std::optional<LinuxNetworkOptions> &Network,
                    ProcessResult &Result, ThreadContext *Thread) {
  using Reply = std::optional<uint64_t>;
  const auto &A = Event.Arguments;
  if (!Options || !Network)
    return unsupported(Result,
                       "socket services require linux_files and linux_network");
  if (Kind == ServiceKind::ReceiveFrom) {
    // recvfrom imports the user extent before looking up the descriptor.
    auto Admitted = receiveBuffer(A[1], A[2], Result);
    if (!Admitted || *Admitted)
      return Admitted;
  }
  if (Kind == ServiceKind::Socket) {
    const uint32_t Family = A[0], Type = A[1], Protocol = A[2];
    const auto BaseType = Type & ~uint32_t(OpenCloseOnExec | OpenNonBlock);
    if ((Family != 2 && Family != 10) || (BaseType != 1 && BaseType != 2) ||
        (Protocol != 0 && Protocol != (BaseType == 1 ? 6u : 17u)))
      return unsupported(Result,
                         "only offline IPv4/IPv6 TCP/UDP sockets are modeled");
    const auto FD = nextDescriptor();
    if (FD == Options->DescriptorLimit)
      return Reply(uint64_t(0) - TooManyFiles);
    Descriptors.emplace(FD,
                        std::make_shared<Descriptor>(OpenSocket{Family, Type}));
    opened(FD, Type);
    return Reply(FD);
  }
  const auto I = Descriptors.find(uint32_t(A[0]));
  if (I == Descriptors.end())
    return Reply(uint64_t(0) - BadDescriptor);
  auto *Socket = std::get_if<OpenSocket>(I->second.get());
  if (!Socket)
    return Reply(uint64_t(0) - 88); // ENOTSOCK
  const bool Datagram = (Socket->Flags & 0xf) == 2;
  if (Kind == ServiceKind::SetSocketOption ||
      Kind == ServiceKind::GetSocketOption) {
    const uint32_t Level = A[1], Name = A[2];
    if (Level != 1 || (Name != 20 && Name != 21))
      return unsupported(Result, "unmodeled socket option");
    auto &Seconds =
        Name == 20 ? Socket->ReceiveTimeoutSeconds : Socket->SendTimeoutSeconds;
    auto Accessible = [&](uint64_t Address, uint64_t Size,
                          unsigned Permissions) -> llvm::Expected<bool> {
      if (Address > Layout.UserLimit || Size > Layout.UserLimit - Address)
        return false;
      return CPU.canAccess(Address, Size, Permissions | UserAccessible);
    };
    uint8_t Timeval[16]{};
    if (Kind == ServiceKind::SetSocketOption) {
      if (int32_t(A[4]) < 16)
        return Reply(uint64_t(0) - InvalidArgument);
      auto Readable = Accessible(A[3], 16, Read);
      if (!Readable)
        return Readable.takeError();
      if (!*Readable)
        return Reply(uint64_t(0) - BadAddress);
      if (auto E = CPU.read(A[3], Timeval))
        return std::move(E);
      const int64_t Sec = llvm::support::endian::read64le(Timeval);
      const int64_t Usec = llvm::support::endian::read64le(Timeval + 8);
      if (Usec < 0 || Usec >= 1000000)
        return Reply(uint64_t(0) - 33); // EDOM
      // Whole seconds avoid inventing the guest kernel's HZ and rounding.
      // Keep negative seconds distinct from zero (immediate vs infinite).
      if (Usec || Sec > ::neverd::limits::emulation::kMaxNetworkTimeoutSeconds)
        return unsupported(
            Result, "socket timeout requires whole seconds up to one day");
      Seconds = Sec;
      return Reply(0);
    }
    auto LengthReadable = Accessible(A[4], 4, Read | Write);
    if (!LengthReadable)
      return LengthReadable.takeError();
    if (!*LengthReadable)
      return Reply(uint64_t(0) - BadAddress);
    uint8_t LengthBytes[4];
    if (auto E = CPU.read(A[4], LengthBytes))
      return std::move(E);
    const int32_t Length = llvm::support::endian::read32le(LengthBytes);
    if (Length < 0)
      return Reply(uint64_t(0) - InvalidArgument);
    const uint32_t Count = std::min<uint32_t>(Length, 16);
    if (Count) {
      auto Writable = Accessible(A[3], Count, Write);
      if (!Writable)
        return Writable.takeError();
      if (!*Writable)
        return Reply(uint64_t(0) - BadAddress);
      llvm::support::endian::write64le(Timeval, std::max<int64_t>(Seconds, 0));
      if (auto E = CPU.write(A[3], llvm::ArrayRef(Timeval).take_front(Count)))
        return std::move(E);
    }
    llvm::support::endian::write32le(LengthBytes, Count);
    if (auto E = CPU.write(A[4], LengthBytes))
      return std::move(E);
    return Reply(0);
  }
  if (Kind == ServiceKind::Connect) {
    if (Socket->Datagram)
      return unsupported(Result, "UDP reconnect requires a new route contract");
    if (Socket->Connection)
      return Reply(uint64_t(0) - 106); // EISCONN
    if (Socket->Failed)
      return unsupported(Result, "a failed socket must be closed before reuse");
    if (!Datagram && (Socket->Flags & OpenNonBlock))
      return unsupported(Result,
                         "asynchronous TCP connection completion is unmodeled");
    const uint32_t Size = A[2];
    if (Size < 2 || Size > ::neverd::limits::emulation::
                               kLinuxNetworkLinuxFilesNetworkSizeLimit)
      return Reply(uint64_t(0) - InvalidArgument);
    if (A[1] > Layout.UserLimit || Size > Layout.UserLimit - A[1])
      return Reply(uint64_t(0) - BadAddress);
    auto Accessible = CPU.canAccess(A[1], Size, Read | UserAccessible);
    if (!Accessible)
      return Accessible.takeError();
    if (!*Accessible)
      return Reply(uint64_t(0) - BadAddress);
    std::vector<uint8_t> Peer(Size);
    if (auto E = CPU.read(A[1], Peer))
      return std::move(E);
    if (networkAddressFamily(Peer) != Socket->Family)
      return unsupported(Result, "peer requires a canonical matching sockaddr");
    if (Datagram) {
      if (NextDatagramConnection == Network->DatagramConnections.size() ||
          Network->DatagramConnections[NextDatagramConnection].Peer != Peer)
        return unsupported(
            Result, "UDP peer has no matching offline route observation");
      const auto &C = Network->DatagramConnections[NextDatagramConnection++];
      if (C.ConnectErrno) {
        // EINTR can be retried on this description. No successful route or
        // packet state is published by a failed attempt.
        Socket->Failed = C.ConnectErrno != 4;
        return Reply(uint64_t(0) - C.ConnectErrno);
      }
      Socket->Datagram = &C;
      return Reply(0);
    }
    if (NextConnection == Network->Connections.size() &&
        Network->TCPConnectFailures) {
      const auto &F = *Network->TCPConnectFailures;
      if (TCPConnectFailuresConsumed == F.Count)
        return unsupported(Result, "TCP connection failure contract exhausted");
      ++TCPConnectFailuresConsumed;
      Socket->Failed = true;
      return Reply(uint64_t(0) - F.Error);
    }
    if (NextConnection == Network->Connections.size() ||
        Network->Connections[NextConnection].Peer != Peer)
      return unsupported(Result,
                         "TCP peer has no matching offline connection attempt");
    const auto &C = Network->Connections[NextConnection++];
    if (C.ConnectErrno) {
      Socket->Failed = true;
      return Reply(uint64_t(0) - C.ConnectErrno);
    }
    Socket->Connection = &C;
    Socket->ConnectionIndex = NextConnection - 1;
    return Reply(0);
  }
  if (Kind == ServiceKind::ReceiveFrom)
    return receiveSocket(I->second, A[1], A[2], Result, Thread, uint32_t(A[3]),
                         A[4], A[5]);
  if (Datagram) {
    if (Kind == ServiceKind::SendTo && A[4])
      return unsupported(
          Result, "UDP explicit destinations require a routing contract");
    return transferDatagram(*Socket, Kind == ServiceKind::SendTo, A[1], A[2],
                            Result, uint32_t(A[3]), A[4], A[5]);
  }
  if ((Kind == ServiceKind::SendTo || Kind == ServiceKind::ReceiveFrom) &&
      (A[4] || A[5]))
    return unsupported(
        Result, "TCP explicit peer output or send destination is unmodeled");
  const uint32_t Flags = A[3];
  // MSG_NOSIGNAL changes only an EPIPE signal; this subset never fabricates
  // that failure. MSG_DONTWAIT cannot invent missing receive readiness.
  if (Flags & ~uint32_t(0x4000 | 0x40))
    return unsupported(Result, "unmodeled TCP transfer flags");
  return transferSocket(*Socket, Kind == ServiceKind::SendTo, A[1], A[2],
                        Result, Flags);
}

std::optional<uint64_t>
LinuxFiles::receiveSocketError(OpenSocket &Socket,
                               const LinuxNetworkReceive &Event,
                               bool MayReturnAgain, ProcessResult &Result) {
  if (Event.Error == 11 && !MayReturnAgain)
    return unsupported(Result,
                       "EAGAIN receive requires nonblocking or timeout state");
  ++Socket.ReceiveEvent;
  return uint64_t(0) - Event.Error;
}

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::transferSocket(OpenSocket &Socket, bool Send, uint64_t Address,
                           uint64_t Size, ProcessResult &Result,
                           uint32_t Flags) {
  using Reply = std::optional<uint64_t>;
  if ((Socket.Flags & 0xf) == 2)
    return transferDatagram(Socket, Send, Address, Size, Result, Flags);
  if (!Socket.Connection)
    return unsupported(Result,
                       "TCP transfer requires a completed offline connection");
  if (Size > MaxSignedIOSize)
    return Reply(uint64_t(0) - InvalidArgument);
  if (!Size)
    return Reply(0);
  const auto &C = *Socket.Connection;
  const LinuxNetworkReceive *Event = nullptr;
  if (!Send && !C.ReceiveEvents.empty() &&
      Socket.ReceiveEvent < C.ReceiveEvents.size()) {
    Event = &C.ReceiveEvents[Socket.ReceiveEvent];
    if (Socket.Sent < Event->AfterSend)
      return unsupported(Result,
                         "TCP receive event awaits declared outgoing bytes");
    assert(!Event->Error && "receive errors complete before data transfer");
  }
  const auto &Bytes = Send ? C.ExpectedSend : Event ? Event->Bytes : C.Receive;
  auto &Position = Send ? Socket.Sent : Socket.Received;
  if (Position == Bytes.size()) {
    if (!Send && C.ReceiveEOF)
      return Reply(0);
    return unsupported(Result,
                       Send ? "outgoing TCP stream exceeds its offline contract"
                            : "TCP receive readiness has no offline input");
  }
  const auto Count = std::min<uint64_t>(Size, Send ? C.MaxSend : C.MaxReceive);
  if (Send && Count > Bytes.size() - Position)
    return unsupported(Result,
                       "outgoing TCP stream exceeds its offline contract");
  const auto Transfer = std::min<uint64_t>(Count, Bytes.size() - Position);
  if (Address > Layout.UserLimit || Transfer > Layout.UserLimit - Address)
    return Reply(uint64_t(0) - BadAddress);
  auto Accessible =
      CPU.canAccess(Address, Transfer, (Send ? Read : Write) | UserAccessible);
  if (!Accessible)
    return Accessible.takeError();
  // Cross-page partial socket transfers are not covered by this atomic
  // offline contract. Do not consume a prefix or claim an errno ordering.
  if (!*Accessible)
    return unsupported(Result,
                       "TCP transfer requires a completely accessible buffer");
  const auto Expected = llvm::ArrayRef(Bytes).slice(Position, Transfer);
  if (Send) {
    std::vector<uint8_t> Actual(Transfer);
    if (auto E = CPU.read(Address, Actual))
      return std::move(E);
    const auto Difference =
        std::mismatch(Actual.begin(), Actual.end(), Expected.begin());
    if (Difference.first != Actual.end()) {
      const auto Offset = Difference.first - Actual.begin();
      const auto Reason =
          llvm::formatv(
              "outgoing TCP bytes differ from the offline contract at "
              "stream offset {0} (buffer offset {1}): expected {2:x2}, "
              "actual {3:x2} (connection index {4})",
              Position + Offset, Offset, *Difference.second, *Difference.first,
              Socket.ConnectionIndex)
              .str();
      return unsupported(Result, Reason.c_str());
    }
  } else if (auto E = CPU.write(Address, Expected))
    return std::move(E);
  Position += Transfer;
  if (Event && Position == Bytes.size()) {
    ++Socket.ReceiveEvent;
    Position = 0;
  }
  return Reply(Transfer);
}
} // namespace neverd::emulation::linux_model
