//===- ProcessNetworkJSON.cpp - Offline network wire input ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Strict wire decoding for explicit resolver and stream inputs.

#include "ProcessNetworkJSON.h"

#include "../os/linux/android/AndroidResolver.h"
#include "../os/linux/kernel/LinuxNetwork.h"
#include "ProcessJSONInteger.h"

#include "neverd/emulation/Limits.h"
#include "neverd/emulation/ProcessReportFields.h"

#include "llvm/ADT/StringExtras.h"

namespace neverd::emulation {
namespace {
namespace field = process_report;
llvm::Error invalid(llvm::StringRef Field) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "invalid offline network field: " + Field);
}
llvm::Error bytes(const llvm::json::Value &V, llvm::StringRef Name,
                  std::vector<uint8_t> &Out, uint64_t &Remaining) {
  auto S = V.getAsString();
  if (!S || S->size() % 2 || S->size() / 2 > Remaining ||
      !llvm::all_of(*S, llvm::isHexDigit))
    return invalid(Name);
  auto Decoded = llvm::fromHex(*S);
  Out.assign(Decoded.begin(), Decoded.end());
  Remaining -= Out.size();
  return llvm::Error::success();
}
llvm::Error number(const llvm::json::Value &V, llvm::StringRef Name,
                   uint32_t &Out) {
  auto N = process_json::integer<uint32_t>(V);
  if (!N)
    return invalid(Name);
  Out = *N;
  return llvm::Error::success();
}
llvm::Error text(const llvm::json::Value &V, llvm::StringRef Name,
                 std::optional<std::string> &Out) {
  if (V.getAsNull()) {
    Out.reset();
    return llvm::Error::success();
  }
  auto S = V.getAsString();
  if (!S ||
      S->size() >
          ::neverd::limits::emulation::kProcessNetworkJSONTextSSizeLimit ||
      S->contains('\0'))
    return invalid(Name);
  Out = S->str();
  return llvm::Error::success();
}
llvm::Error timestamp(const llvm::json::Value &V, llvm::StringRef Name,
                      std::optional<uint64_t> &Out) {
  auto N = process_json::integer<uint64_t>(V);
  if (!N || *N > uint64_t(INT64_MAX))
    return invalid(Name);
  Out = *N;
  return llvm::Error::success();
}
llvm::Error receiveEvents(const llvm::json::Value &Value,
                          std::vector<LinuxNetworkReceive> &Out,
                          uint64_t &Remaining) {
  const auto *Events = Value.getAsArray();
  if (!Events || Events->size() > ::neverd::limits::emulation::
                                      kProcessNetworkJSONReceiveEventsSizeLimit)
    return invalid(field::ReceiveEvents);
  for (const auto &Item : *Events) {
    const auto *Event = Item.getAsObject();
    if (!Event || bool(Event->get(field::NetworkReceive)) ==
                      bool(Event->get(field::NetworkErrno)))
      return invalid(field::ReceiveEvents);
    LinuxNetworkReceive E;
    for (const auto &[Key, V] : *Event) {
      llvm::StringRef Name = Key;
      if (Name == field::NetworkReceive) {
        if (auto Error = bytes(V, Name, E.Bytes, Remaining))
          return Error;
      } else if (Name == field::NetworkErrno) {
        if (auto Error = number(V, Name, E.Error))
          return Error;
        if (!E.Error)
          return invalid(Name);
      } else if (Name == field::ReadyAtNS) {
        if (auto Error = timestamp(V, Name, E.ReadyAtNS))
          return Error;
      } else if (Name == field::AfterSend) {
        auto N = process_json::integer<uint64_t>(V);
        if (!N)
          return invalid(Name);
        E.AfterSend = *N;
      } else
        return invalid(Name);
    }
    Out.push_back(std::move(E));
  }
  return llvm::Error::success();
}
} // namespace
llvm::Expected<LinuxNetworkOptions>
linuxNetworkOptionsFromJSON(const llvm::json::Value &Value) {
  const auto *Object = Value.getAsObject();
  if (!Object || Object->empty())
    return invalid(field::LinuxNetwork);
  for (const auto &[Key, V] : *Object) {
    if (Key == field::TCPConnectFailures) {
      if (!V.getAsObject())
        return invalid(Key);
    } else if ((Key != field::Connections &&
                Key != field::DatagramConnections) ||
               !V.getAsArray() ||
               V.getAsArray()->size() >
                   ::neverd::limits::emulation::kMaxNetworkConnections) {
      return invalid(field::LinuxNetwork);
    }
  }
  const llvm::json::Array Empty;
  const auto *Connections = Object->getArray(field::Connections);
  const auto *Datagrams = Object->getArray(field::DatagramConnections);
  LinuxNetworkOptions Out;
  if (const auto *F = Object->getObject(field::TCPConnectFailures)) {
    if (F->size() != 2 || !F->get(field::NetworkErrno) ||
        !F->get(field::NetworkFailureCount))
      return invalid(field::TCPConnectFailures);
    auto &Failure = Out.TCPConnectFailures.emplace();
    if (auto E = number(*F->get(field::NetworkErrno), field::NetworkErrno,
                        Failure.Error))
      return std::move(E);
    if (auto E = number(*F->get(field::NetworkFailureCount),
                        field::NetworkFailureCount, Failure.Count))
      return std::move(E);
  }
  uint64_t Remaining = ::neverd::limits::emulation::
      kProcessNetworkJSONLinuxNetworkOptionsFromJSONRemaining;
  for (const auto &Item : Connections ? *Connections : Empty) {
    const auto *C = Item.getAsObject();
    if (!C || !C->get(field::NetworkPeer) ||
        (C->get(field::NetworkReceive) && C->get(field::ReceiveEvents)))
      return invalid(field::Connections);
    LinuxStreamConnection Connection;
    for (const auto &[Key, V] : *C) {
      llvm::StringRef Name = Key;
      if (Name == field::NetworkPeer || Name == field::ExpectedSend ||
          Name == field::NetworkReceive) {
        auto &Destination = Name == field::NetworkPeer ? Connection.Peer
                            : Name == field::ExpectedSend
                                ? Connection.ExpectedSend
                                : Connection.Receive;
        if (auto E = bytes(V, Name, Destination, Remaining))
          return std::move(E);
      } else if (Name == field::ConnectErrno || Name == field::MaxSend ||
                 Name == field::MaxReceive) {
        auto &Destination = Name == field::ConnectErrno
                                ? Connection.ConnectErrno
                            : Name == field::MaxSend ? Connection.MaxSend
                                                     : Connection.MaxReceive;
        if (auto E = number(V, Name, Destination))
          return std::move(E);
      } else if (Name == field::ReceiveEvents) {
        if (auto E = receiveEvents(V, Connection.ReceiveEvents, Remaining))
          return std::move(E);
      } else if (Name == field::ReceiveEOFAtNS ||
                 Name == field::ReceiveQuietUntilNS) {
        if (auto E = timestamp(V, Name,
                               Name == field::ReceiveEOFAtNS
                                   ? Connection.ReceiveEOFAtNS
                                   : Connection.ReceiveQuietUntilNS))
          return std::move(E);
      } else if (Name == field::ReceiveEOF) {
        auto B = V.getAsBoolean();
        if (!B)
          return invalid(Name);
        Connection.ReceiveEOF = *B;
      } else
        return invalid(Name);
    }
    Out.Connections.push_back(std::move(Connection));
  }
  for (const auto &Item : Datagrams ? *Datagrams : Empty) {
    const auto *C = Item.getAsObject();
    if (!C || !C->get(field::NetworkPeer))
      return invalid(field::DatagramConnections);
    LinuxDatagramConnection Connection;
    for (const auto &[Key, V] : *C) {
      llvm::StringRef Name = Key;
      if (Name == field::NetworkPeer) {
        if (auto E = bytes(V, Name, Connection.Peer, Remaining))
          return std::move(E);
      } else if (Name == field::ConnectErrno) {
        if (auto E = number(V, Name, Connection.ConnectErrno))
          return std::move(E);
      } else if (Name == field::ReceiveQuietUntilNS) {
        if (auto E = timestamp(V, Name, Connection.ReceiveQuietUntilNS))
          return std::move(E);
      } else if (Name == field::ExpectedDatagrams) {
        const auto *Packets = V.getAsArray();
        if (!Packets ||
            Packets->size() >
                ::neverd::limits::emulation::
                    kProcessNetworkJSONLinuxNetworkOptionsFromJSONPacketsSizeLimit)
          return invalid(Name);
        for (const auto &Packet : *Packets) {
          std::vector<uint8_t> Payload;
          if (auto E = bytes(Packet, Name, Payload, Remaining))
            return std::move(E);
          Connection.ExpectedSend.push_back(std::move(Payload));
        }
      } else if (Name == field::ReceiveEvents) {
        if (auto E = receiveEvents(V, Connection.ReceiveEvents, Remaining))
          return std::move(E);
      } else
        return invalid(Name);
    }
    Out.DatagramConnections.push_back(std::move(Connection));
  }
  if (auto E = linux_model::validateNetworkOptions(Out))
    return std::move(E);
  return Out;
}

llvm::Expected<std::vector<AndroidResolverQuery>>
androidResolverQueriesFromJSON(const llvm::json::Value &Value) {
  const auto *Queries = Value.getAsArray();
  if (!Queries ||
      Queries->size() >
          ::neverd::limits::emulation::
              kProcessNetworkJSONAndroidResolverQueriesFromJSONQueriesSizeLimit)
    return invalid(field::ResolverQueries);
  std::vector<AndroidResolverQuery> Out;
  uint64_t Remaining = ::neverd::limits::emulation::
      kProcessNetworkJSONAndroidResolverQueriesFromJSONRemaining;
  for (const auto &Item : *Queries) {
    const auto *Q = Item.getAsObject();
    if (!Q || !Q->get(field::ResolverNode) || !Q->get(field::ResolverService))
      return invalid(field::ResolverQueries);
    AndroidResolverQuery Query;
    for (const auto &[Key, V] : *Q) {
      llvm::StringRef Name = Key;
      if (Name == field::ResolverNode || Name == field::ResolverService) {
        if (auto E =
                text(V, Name,
                     Name == field::ResolverNode ? Query.Node : Query.Service))
          return std::move(E);
      } else if (Name == field::ResolverAddresses) {
        const auto *Addresses = V.getAsArray();
        if (!Addresses ||
            Addresses->size() >
                ::neverd::limits::emulation::
                    kProcessNetworkJSONAndroidResolverQueriesFromJSONAddressesSizeLimit)
          return invalid(Name);
        for (const auto &Entry : *Addresses) {
          const auto *A = Entry.getAsObject();
          if (!A || !A->get(field::SockAddress))
            return invalid(Name);
          AndroidResolverAddress Address;
          for (const auto &[K, Val] : *A) {
            llvm::StringRef N = K;
            if (N == field::SockAddress) {
              if (auto E = bytes(Val, N, Address.Address, Remaining))
                return std::move(E);
            } else if (N == field::CanonicalName) {
              if (auto E = text(Val, N, Address.CanonicalName))
                return std::move(E);
            } else {
              auto *Destination = N == field::SocketType ? &Address.SocketType
                                  : N == field::NetworkProtocol
                                      ? &Address.Protocol
                                      : nullptr;
              if (!Destination)
                return invalid(N);
              if (auto E = number(Val, N, *Destination))
                return std::move(E);
            }
          }
          Query.Addresses.push_back(std::move(Address));
        }
      } else {
        auto *Destination = Name == field::ResolverFlags     ? &Query.Flags
                            : Name == field::NetworkFamily   ? &Query.Family
                            : Name == field::SocketType      ? &Query.SocketType
                            : Name == field::NetworkProtocol ? &Query.Protocol
                            : Name == field::ResolverError   ? &Query.Error
                            : Name == field::ResolverErrno ? &Query.SystemErrno
                                                           : nullptr;
        if (!Destination)
          return invalid(Name);
        if (auto E = number(V, Name, *Destination))
          return std::move(E);
      }
    }
    Out.push_back(std::move(Query));
  }
  if (auto E = android_model::validateResolverQueries(Out))
    return std::move(E);
  return Out;
}
} // namespace neverd::emulation
