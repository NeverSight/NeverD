//===- DriverRegistryEventsJSON.cpp - Explicit registry event inputs ------===//
// NeverD Decompiler
///
/// \file
/// Explicit registry event inputs.
///

#include "DriverRegistryEventsJSON.h"

#include "neverd/emulation/DriverSession.h"

#include "llvm/ADT/StringExtras.h"

#include <map>
#include <set>

namespace neverd::emulation {
namespace {
using Kind = DriverRegistryEvent::Kind;
llvm::Error invalid(const llvm::Twine &T) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "registry_events: " + T);
}
const char *name(Kind K) {
  switch (K) {
  case Kind::Open:
    return "open";
  case Kind::SetValue:
    return "set_value";
  case Kind::QueryValue:
    return "query_value";
  case Kind::Close:
    return "close";
  }
  llvm_unreachable("invalid registry event kind");
}
llvm::json::Object eventJSON(const DriverRegistryEvent &E) {
  llvm::json::Object O{{"kind", name(E.Operation)},
                       {"before_request", uint64_t(E.BeforeRequest)},
                       {"handle", E.Handle},
                       {"actor_process_id", E.ActorProcessID}};
  if (E.Operation == Kind::Open) {
    O["path"] = E.Path;
    O["desired_access"] = E.DesiredAccess;
    if (E.RootHandle)
      O["root_handle"] = E.RootHandle;
  }
  if (E.Operation == Kind::SetValue || E.Operation == Kind::QueryValue)
    O["value_name"] = E.ValueName;
  if (E.ValueType) {
    O["type"] = *E.ValueType;
    O["data"] = llvm::toHex(E.Data, true);
  }
  if (E.Operation == Kind::QueryValue)
    O["output_size"] = E.OutputSize;
  return O;
}
} // namespace

llvm::Error validateDriverRegistryEvents(const DriverOptions &O) {
  if (O.RegistryEvents.size() > MaxDriverRegistryEvents)
    return invalid("too many events");
  if (O.RegistryEvents.empty())
    return llvm::Error::success();
  if (!O.Registry || !O.Version || O.Version->Major != 10 || O.Version->Minor ||
      O.Version->Build != 19041)
    return invalid("requires explicit registry and Windows 10.0.19041");
  size_t Previous = 0, DataBytes = 0;
  std::set<uint32_t> Seen, Live;
  std::map<uint32_t, std::string> Paths;
  for (const auto &E : O.RegistryEvents) {
    if (E.BeforeRequest < Previous || E.BeforeRequest > O.Requests.size())
      return invalid("boundaries must be ordered and within requests.size()");
    Previous = E.BeforeRequest;
    if (!E.Handle)
      return invalid("handle identity must be nonzero");
    if (E.Operation == Kind::Open) {
      std::string Path = E.Path;
      if (E.RootHandle) {
        if (!Live.contains(E.RootHandle) || E.Path.empty() ||
            E.Path.front() == '\\')
          return invalid("relative open requires a preceding live root_handle "
                         "and a relative name");
        Path = Paths.at(E.RootHandle) + "\\" + E.Path;
      }
      if (!Seen.insert(E.Handle).second || !E.DesiredAccess)
        return invalid("open requires a fresh handle identity and access mask");
      Live.insert(E.Handle);
      Paths.emplace(E.Handle, Path);
      if (auto Err = validateDriverRegistry(
              std::vector<DriverRegistryKey>{{Path, {}}}))
        return Err;
    } else if (E.Operation == Kind::SetValue ||
               E.Operation == Kind::QueryValue || E.Operation == Kind::Close) {
      if (!Live.contains(E.Handle))
        return invalid(
            "operation requires a preceding, unclosed open identity");
      if (E.Operation == Kind::Close)
        Live.erase(E.Handle);
    } else
      return invalid("unknown native event kind");
    if (E.Operation != Kind::Open &&
        (!E.Path.empty() || E.DesiredAccess || E.RootHandle))
      return invalid("path, root_handle and desired_access are open-only");
    if (E.Operation != Kind::SetValue && (E.ValueType || !E.Data.empty()))
      return invalid("type and data are set_value-only");
    if (E.Operation == Kind::SetValue && !E.ValueType)
      return invalid("set_value requires an explicit type");
    if (E.Operation != Kind::SetValue && E.Operation != Kind::QueryValue &&
        !E.ValueName.empty())
      return invalid("value_name requires set_value or query_value");
    if (E.Operation != Kind::QueryValue && E.OutputSize)
      return invalid("output_size is query_value-only");
    if (E.OutputSize > MaxRegistryValueBytes + 12)
      return invalid("query output exceeds the partial-information bound");
    if (auto Err = validateDriverRegistry(std::vector<DriverRegistryKey>{
            {"\\Registry\\Machine\\Software\\NeverD",
             {{E.ValueName, E.ValueType.value_or(0), E.Data}}}}))
      return Err;
    DataBytes += E.Data.size();
    if (DataBytes > MaxRegistryTotalBytes)
      return invalid("event payload exceeds the total byte bound");
  }
  if (!Live.empty())
    return invalid("each declared handle requires a close event");
  return validateDriverProcessEvents(
      O.ProcessEvents, O.Requests,
      initialDriverProcessIDs(O.ProcessEvents, O.Requests, O.ProcessInventory),
      O.RegistryEvents);
}

llvm::Expected<std::vector<DriverRegistryEvent>>
parseDriverRegistryEvents(const llvm::json::Value &V) {
  const auto *A = V.getAsArray();
  if (!A || A->size() > MaxDriverRegistryEvents)
    return invalid("requires a bounded array");
  std::vector<DriverRegistryEvent> Events;
  for (const auto &Item : *A) {
    const auto *O = Item.getAsObject();
    if (!O)
      return invalid("each event must be an object");
    auto K = O->getString("kind");
    DriverRegistryEvent E;
    if (!K)
      return invalid("requires kind");
    if (*K == "open")
      E.Operation = Kind::Open;
    else if (*K == "set_value")
      E.Operation = Kind::SetValue;
    else if (*K == "query_value")
      E.Operation = Kind::QueryValue;
    else if (*K == "close")
      E.Operation = Kind::Close;
    else
      return invalid("unknown kind");
    auto Integer = [&](llvm::StringRef Key) -> llvm::Expected<uint32_t> {
      auto N = O->getInteger(Key);
      if (!N || *N < 0 || uint64_t(*N) > UINT32_MAX)
        return invalid(Key + " requires a uint32 integer");
      return uint32_t(*N);
    };
    auto Boundary = Integer("before_request");
    if (!Boundary)
      return Boundary.takeError();
    auto Handle = Integer("handle");
    if (!Handle)
      return Handle.takeError();
    E.BeforeRequest = *Boundary;
    E.Handle = *Handle;
    std::set<llvm::StringRef> Allowed{"kind", "before_request", "handle",
                                      "actor_process_id"};
    if (O->get("actor_process_id")) {
      auto Actor = Integer("actor_process_id");
      if (!Actor)
        return Actor.takeError();
      E.ActorProcessID = *Actor;
    }
    if (E.Operation == Kind::Open) {
      Allowed.insert("path");
      Allowed.insert("desired_access");
      Allowed.insert("root_handle");
      if (O->get("root_handle")) {
        auto Root = Integer("root_handle");
        if (!Root)
          return Root.takeError();
        if (!*Root)
          return invalid("root_handle must be nonzero when present");
        E.RootHandle = *Root;
      }
      auto P = O->getString("path");
      auto Access = Integer("desired_access");
      if (!P)
        return invalid("open requires path");
      if (!Access)
        return Access.takeError();
      E.Path = P->str();
      E.DesiredAccess = *Access;
    }
    if (E.Operation == Kind::SetValue || E.Operation == Kind::QueryValue) {
      Allowed.insert("value_name");
      auto N = O->getString("value_name");
      if (!N)
        return invalid("requires value_name, including an empty default name");
      E.ValueName = N->str();
    }
    if (E.Operation == Kind::SetValue) {
      Allowed.insert("type");
      Allowed.insert("data");
      auto T = Integer("type");
      if (!T)
        return T.takeError();
      E.ValueType = *T;
      auto D = O->getString("data");
      if (!D || D->size() > MaxRegistryValueBytes * 2 || (D->size() & 1) ||
          !llvm::all_of(*D, [](char C) { return llvm::isHexDigit(C); }))
        return invalid("data requires bounded hexadecimal bytes");
      std::string Bytes = llvm::fromHex(*D);
      E.Data.assign(Bytes.begin(), Bytes.end());
    }
    if (E.Operation == Kind::QueryValue) {
      Allowed.insert("output_size");
      auto N = Integer("output_size");
      if (!N)
        return N.takeError();
      E.OutputSize = *N;
    }
    for (const auto &[Key, Ignored] : *O)
      if (!Allowed.contains(Key))
        return invalid("unknown or inapplicable field " + Key.str());
    Events.push_back(std::move(E));
  }
  return Events;
}
llvm::json::Array
driverRegistryEventsJSON(llvm::ArrayRef<DriverRegistryEvent> E) {
  llvm::json::Array A;
  for (const auto &Item : E)
    A.push_back(eventJSON(Item));
  return A;
}
llvm::json::Array
driverRegistryEventResultsJSON(llvm::ArrayRef<DriverRegistryEventResult> E) {
  llvm::json::Array A;
  for (const auto &Item : E) {
    auto O = eventJSON(Item.Event);
    O["source_index"] = uint64_t(Item.SourceIndex);
    O["actor_process_id"] = Item.Event.ActorProcessID;
    O["completed"] = Item.Completed;
    O["status"] = Item.Status ? llvm::json::Value(*Item.Status)
                              : llvm::json::Value(nullptr);
    O["required_size"] = Item.RequiredSize
                             ? llvm::json::Value(*Item.RequiredSize)
                             : llvm::json::Value(nullptr);
    O["output_hex"] = llvm::toHex(Item.Output, true);
    A.push_back(std::move(O));
  }
  return A;
}
} // namespace neverd::emulation
