//===- LinuxNetworkOptions.h - Explicit offline stream inputs ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bounded offline network contracts and their semantic checks.

#ifndef NEVERD_EMULATION_LINUXNETWORKOPTIONS_H
#define NEVERD_EMULATION_LINUXNETWORKOPTIONS_H

#include <cstdint>
#include <optional>
#include <vector>

namespace neverd::emulation {
/// One explicit receive completion. Error is a positive Linux errno and has
/// no bytes. AfterSend gates availability on accepted stream bytes or UDP
/// datagrams; it does not acknowledge application data.
struct LinuxNetworkReceive {
  std::vector<uint8_t> Bytes;
  uint32_t Error = 0;
  uint64_t AfterSend = 0;
  /// Optional arrival in elapsed virtual nanoseconds since workload start.
  /// Explicitly asserts no earlier read readiness before this data event.
  /// Completion errors cannot carry an arrival observation.
  std::optional<uint64_t> ReadyAtNS;
};
/// One ordered TCP connection attempt. Addresses are Linux sockaddr_in (16
/// bytes) or sockaddr_in6 (28 bytes), with network-order port/address bytes.
/// These are virtual peers; the model never opens a host socket.
struct LinuxStreamConnection {
  std::vector<uint8_t> Peer;
  uint32_t ConnectErrno = 0;
  /// The complete outgoing stream expected from the guest. A mismatch or
  /// exhausted contract stops before consuming any bytes from that call.
  std::vector<uint8_t> ExpectedSend;
  std::vector<uint8_t> Receive;
  /// Explicit peer EOF after Receive. False leaves an unknown waiting boundary.
  bool ReceiveEOF = false;
  /// Explicit per-call transfer ceilings permit short I/O without host timing.
  uint32_t MaxSend = 65536, MaxReceive = 65536;
  /// Ordered short reads/errors, mutually exclusive with the legacy Receive
  /// byte stream. Empty TCP data completions are invalid; EOF stays explicit.
  std::vector<LinuxNetworkReceive> ReceiveEvents;
  /// Optional arrival of the explicitly declared EOF, after all data events.
  std::optional<uint64_t> ReceiveEOFAtNS;
  /// After all receive events, asserts no read readiness through this elapsed
  /// nanosecond (inclusive). Later state stays unknown. Incompatible with EOF.
  std::optional<uint64_t> ReceiveQuietUntilNS;
};
/// One ordered connected UDP route observation. A successful connect selects
/// a default peer, not a handshake or delivery confirmation. Reconnect and
/// unconnected routing require additional contracts and remain unsupported.
struct LinuxDatagramConnection {
  std::vector<uint8_t> Peer;
  uint32_t ConnectErrno = 0;
  std::vector<std::vector<uint8_t>> ExpectedSend;
  /// Every data completion is one whole datagram, including an empty one.
  /// A short buffer discards its remaining bytes unless MSG_PEEK is used.
  std::vector<LinuxNetworkReceive> ReceiveEvents;
  std::optional<uint64_t> ReceiveQuietUntilNS;
};
/// Failure-only TCP attempts after the ordered exact-peer attempts are used.
/// Canonical matching-family peers are admitted without creating a route,
/// byte stream or readiness. A failed description must be closed before reuse.
struct LinuxTCPConnectFailures {
  uint32_t Error = 0;
  /// Finite supplied outcomes, 1..UINT32_MAX; no per-attempt storage is kept.
  uint32_t Count = 0;
};
struct LinuxNetworkOptions {
  /// Attempts are consumed only after the exact peer and guest memory pass
  /// validation. Descriptor aliases share the selected connection and cursors.
  std::vector<LinuxStreamConnection> Connections;
  std::vector<LinuxDatagramConnection> DatagramConnections;
  /// Absence preserves exact-peer refusal. Never bypasses an unconsumed
  /// ordered attempt. Count is independent of the stored TCP/UDP entry bound;
  /// ordinary execution budgets still apply to every consumed failure.
  std::optional<LinuxTCPConnectFailures> TCPConnectFailures;
};
} // namespace neverd::emulation
#endif
