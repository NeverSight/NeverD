//===- LinuxDatagram.cpp - Offline UDP packet completions -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Connected UDP packet boundaries, aliases and explicit receive outcomes.

#include "LinuxFiles.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation::linux_model {
llvm::Expected<std::optional<uint64_t>>
LinuxFiles::transferDatagram(OpenSocket &Socket, bool Send, uint64_t Address,
                             uint64_t Size, ProcessResult &Result,
                             uint32_t Flags, uint64_t PeerAddress,
                             uint64_t PeerLength) {
  using Reply = std::optional<uint64_t>;
  constexpr uint32_t Peek = 2, Truncate = 0x20, DontWait = 0x40,
                     NoSignal = 0x4000;
  // Receive flag admission belongs to receiveSocket, before suspension.
  if (Send && (Flags & ~(DontWait | NoSignal)))
    return unsupported(Result, "unmodeled UDP transfer flags");
  if (!Socket.Datagram)
    return unsupported(Result,
                       "UDP transfer requires an explicit connected route");
  const auto &C = *Socket.Datagram;
  const LinuxNetworkReceive *Event = nullptr;
  if (Send) {
    if (Socket.Sent == C.ExpectedSend.size())
      return unsupported(Result, "outgoing UDP datagram has no offline input");
    if (Size != C.ExpectedSend[Socket.Sent].size())
      return unsupported(
          Result, "outgoing UDP datagram length differs from its contract");
  } else {
    if (Socket.ReceiveEvent == C.ReceiveEvents.size())
      return unsupported(Result, "UDP receive readiness has no offline input");
    Event = &C.ReceiveEvents[Socket.ReceiveEvent];
    if (Socket.Sent < Event->AfterSend)
      return unsupported(
          Result, "UDP receive event awaits declared outgoing datagrams");
    assert(!Event->Error && "receive errors complete before data transfer");
  }
  const auto &Bytes = Send ? C.ExpectedSend[Socket.Sent] : Event->Bytes;
  const auto Count = std::min<uint64_t>(Size, Bytes.size());
  auto Accessible = [&](uint64_t Pointer, uint64_t Length,
                        unsigned Permissions) -> llvm::Expected<bool> {
    if (!Length)
      return true;
    if (Pointer > Layout.UserLimit || Length > Layout.UserLimit - Pointer)
      return false;
    return CPU.canAccess(Pointer, Length, Permissions | UserAccessible);
  };
  auto Buffer = Accessible(Address, Count, Send ? Read : Write);
  if (!Buffer)
    return Buffer.takeError();
  if (!*Buffer)
    return unsupported(Result,
                       "UDP transfer requires a completely accessible buffer");

  uint32_t PeerCount = 0;
  uint8_t LengthBytes[4];
  if (!Send && PeerAddress) {
    auto LengthAccess = Accessible(PeerLength, 4, Read | Write);
    if (!LengthAccess)
      return LengthAccess.takeError();
    if (!*LengthAccess)
      return unsupported(Result,
                         "UDP peer output requires accessible length storage");
    if (auto E = CPU.read(PeerLength, LengthBytes))
      return std::move(E);
    const int32_t Length = llvm::support::endian::read32le(LengthBytes);
    if (Length < 0)
      return unsupported(Result,
                         "UDP peer output requires a nonnegative length");
    PeerCount = std::min<uint32_t>(Length, C.Peer.size());
    auto PeerAccess = Accessible(PeerAddress, PeerCount, Write);
    if (!PeerAccess)
      return PeerAccess.takeError();
    if (!*PeerAccess)
      return unsupported(
          Result, "UDP peer output requires a completely accessible buffer");
    // Linux reads addrlen after publishing the payload. Refuse aliases that
    // would change that control input instead of silently reading it early.
    if (Count && Address < PeerLength + 4 && PeerLength < Address + Count)
      return unsupported(Result, "UDP payload aliases its peer output length");
  }
  // In Linux, a copy fault can consume a packet and publish a prefix. This
  // bounded contract refuses such calls before effects instead of inventing
  // a portable EFAULT ordering. Valid calls preserve payload/address/length
  // publication order, including overlap between the two output buffers.
  if (Send) {
    std::vector<uint8_t> Actual(Count);
    if (Count)
      if (auto E = CPU.read(Address, Actual))
        return std::move(E);
    if (Actual != Bytes)
      return unsupported(Result,
                         "outgoing UDP bytes differ from the offline contract");
    ++Socket.Sent;
    return Reply(Count);
  }
  if (Count)
    if (auto E = CPU.write(Address, llvm::ArrayRef(Bytes).take_front(Count)))
      return std::move(E);
  if (PeerAddress) {
    if (PeerCount)
      if (auto E = CPU.write(PeerAddress,
                             llvm::ArrayRef(C.Peer).take_front(PeerCount)))
        return std::move(E);
    llvm::support::endian::write32le(LengthBytes, C.Peer.size());
    if (auto E = CPU.write(PeerLength, LengthBytes))
      return std::move(E);
  }
  if (!(Flags & Peek))
    ++Socket.ReceiveEvent;
  return Reply(Flags & Truncate ? Bytes.size() : Count);
}
} // namespace neverd::emulation::linux_model
