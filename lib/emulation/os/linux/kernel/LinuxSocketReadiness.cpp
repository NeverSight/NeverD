//===- LinuxSocketReadiness.cpp - Explicit virtual socket arrival state ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Non-consuming readiness from the same descriptions used by I/O.

#include "LinuxFiles.h"
#include "LinuxTime.h"

#include "llvm/Support/FormatVariadic.h"

namespace neverd::emulation::linux_model {
std::optional<uint64_t>
LinuxFiles::ReadinessFailure::stop(ProcessResult &Result) const {
  if (!TCPInterest)
    return unsupported(Result, Reason);
  const auto Message =
      llvm::formatv("{0} (TCP connection index {1}, {2} readiness)", Reason,
                    ConnectionIndex, TCPInterest)
          .str();
  return unsupported(Result, Message.c_str());
}

LinuxFiles::Readiness
LinuxFiles::descriptorReadiness(const Descriptor &Description,
                                uint32_t Events) const {
  constexpr unsigned ReadEvents = 0x001 | 0x040;
  constexpr unsigned WriteEvents = 0x004 | 0x100;
  if (std::holds_alternative<OpenFile>(Description) ||
      std::holds_alternative<OpenProcFile>(Description) ||
      std::holds_alternative<OpenDirectory>(Description))
    return {Events & (ReadEvents | WriteEvents), {}, {}};
  if (std::holds_alternative<ProcessDescriptor>(Description))
    return {}; // Admitted live-task observation; no invented exit event.
  const auto *Socket = std::get_if<OpenSocket>(&Description);
  if (!Socket)
    return {0, {}, {PollDescriptorUnsupported}};
  // ERR/HUP/NVAL are result-only bits. A receive error is not evidence of
  // asynchronous SO_ERROR, nor is read EOF evidence of a full socket hangup.
  if (Events & ~(ReadEvents | WriteEvents | 0x008 | 0x010 | 0x020))
    return {0, {}, {"requested socket poll flags are unmodeled"}};
  unsigned Wanted =
      (Events & ReadEvents ? 1 : 0) | (Events & WriteEvents ? 2 : 0);
  auto R = socketReadiness(*Socket, Wanted);
  R.Available = (R.Available & 1 ? Events & ReadEvents : 0) |
                (R.Available & 2 ? Events & WriteEvents : 0);
  return R;
}

LinuxFiles::Readiness LinuxFiles::socketReadiness(const OpenSocket &Socket,
                                                  unsigned Wanted) const {
  const bool Datagram = (Socket.Flags & 0xf) == 2;
  Readiness R;
  auto Unknown = [&](const char *Interest) {
    R.Unknown.Reason = "socket readiness has no offline observation";
    if (Socket.Connection) {
      R.Unknown.TCPInterest = Interest;
      R.Unknown.ConnectionIndex = Socket.ConnectionIndex;
    }
    return R;
  };
  if ((Datagram ? !Socket.Datagram : !Socket.Connection) || (Wanted & 4))
    return Unknown("exceptional");
  if (Wanted & 2) {
    const auto Size = Datagram ? Socket.Datagram->ExpectedSend.size()
                               : Socket.Connection->ExpectedSend.size();
    if (Socket.Sent >= Size)
      return Unknown("write");
    R.Available |= 2;
  }
  if (!(Wanted & 1))
    return R;
  auto At = [&](std::optional<uint64_t> Time) {
    if (Time && (!Clock || !Clock->advancesOnIdle())) {
      R.Unknown = {"timed network input requires virtual idle time"};
    } else if (Time && Clock->elapsed() < *Time) {
      R.Wake = *Time;
    } else {
      R.Available |= 1;
    }
    return R;
  };
  const auto &Events = Datagram ? Socket.Datagram->ReceiveEvents
                                : Socket.Connection->ReceiveEvents;
  if (Socket.ReceiveEvent < Events.size()) {
    const auto &E = Events[Socket.ReceiveEvent];
    // A send prerequisite or completion error alone is no poll observation.
    if (Socket.Sent < E.AfterSend || E.Error)
      return Unknown("read");
    return At(E.ReadyAtNS); // Includes an explicitly queued empty datagram.
  }
  if (!Datagram) {
    if (Socket.Received < Socket.Connection->Receive.size())
      return At({});
    if (Socket.Connection->ReceiveEOF)
      return At(Socket.Connection->ReceiveEOFAtNS);
  }
  auto Quiet = Datagram ? Socket.Datagram->ReceiveQuietUntilNS
                        : Socket.Connection->ReceiveQuietUntilNS;
  if (Socket.ConnectQuietEnd)
    Quiet = Socket.ConnectQuietEnd;
  if (Quiet) {
    if (!Clock || !Clock->advancesOnIdle()) {
      R.Unknown = {"timed network input requires virtual idle time"};
      return R;
    }
    if (Clock->elapsed() <= *Quiet && *Quiet < uint64_t(INT64_MAX)) {
      R.Wake = *Quiet + 1; // First unknown nanosecond, not invented data.
      return R;
    }
  }
  return Unknown("read");
}

} // namespace neverd::emulation::linux_model
