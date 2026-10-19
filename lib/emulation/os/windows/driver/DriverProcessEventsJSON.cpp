//===- DriverProcessEventsJSON.cpp - Explicit process lifecycle -----------===//
// NeverD Decompiler
///
/// \file
/// Explicit process lifecycle.
///

#include "DriverProcessEventsJSON.h"

#include "../WindowsUnicode.h"

#include "neverd/emulation/DriverSession.h"

#include "llvm/ADT/StringExtras.h"

#include <map>
#include <set>
namespace neverd::emulation {
namespace {
llvm::Error invalid(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "process_events: " + Text);
}
bool fileRequest(const DriverRequest &R) {
  return R.Kind != DriverRequestKind::Pnp && R.Kind != DriverRequestKind::Power;
}
llvm::json::Object eventJSON(const DriverProcessEvent &E) {
  llvm::json::Object O{
      {"kind", E.Type == DriverProcessEvent::Kind::Create ? "create" : "exit"},
      {"process_id", E.ProcessID},
      {"parent_id", E.ParentID},
      {"before_request", uint64_t(E.BeforeRequest)}};
  if (E.Type == DriverProcessEvent::Kind::Create)
    O["creator_process_id"] = E.CreatorProcessID;
  if (E.ExitStatus)
    O["exit_status"] = *E.ExitStatus;
  if (E.CreateInfo) {
    llvm::json::Object Info{
        {"file_open_name_available", E.CreateInfo->FileOpenNameAvailable},
        {"image_file_name", E.CreateInfo->ImageFileName}};
    Info["command_line"] = E.CreateInfo->CommandLine
                               ? llvm::json::Value(*E.CreateInfo->CommandLine)
                               : llvm::json::Value(nullptr);
    O["create_info"] = std::move(Info);
  }
  return O;
}
} // namespace
llvm::Error validateDriverProcessEvents(
    llvm::ArrayRef<DriverProcessEvent> Events,
    llvm::ArrayRef<DriverRequest> Requests,
    llvm::ArrayRef<uint32_t> InitialProcesses,
    llvm::ArrayRef<DriverRegistryEvent> RegistryEvents) {
  if (Events.size() > MaxDriverProcessEvents)
    return invalid("too many lifecycle events");
  if (Events.empty() && RegistryEvents.empty())
    return llvm::Error::success();
  std::set<uint32_t> Created, Live{4}, Seen{4};
  Live.insert(InitialProcesses.begin(), InitialProcesses.end());
  Seen.insert(InitialProcesses.begin(), InitialProcesses.end());
  std::map<uint32_t, uint32_t> Parents;
  size_t Previous = 0;
  for (const auto &E : Events) {
    if (E.Type != DriverProcessEvent::Kind::Exit && E.ExitStatus)
      return invalid("exit_status is exit-only");
    if (E.CreateInfo) {
      if (E.Type != DriverProcessEvent::Kind::Create)
        return invalid("create_info is create-only");
      for (const auto *Text :
           {&E.CreateInfo->ImageFileName, E.CreateInfo->CommandLine
                                              ? &*E.CreateInfo->CommandLine
                                              : nullptr}) {
        if (!Text)
          continue;
        auto Bytes = windows_unicode::bytes(*Text, true);
        if (!Bytes)
          return Bytes.takeError();
        if (Bytes->size() > 8192 || Text->find('\0') != std::string::npos)
          return invalid("create_info strings require bounded non-null UTF-8");
      }
      if (E.CreateInfo->ImageFileName.empty())
        return invalid("create_info requires an executable image name");
    }
    if (E.BeforeRequest < Previous || E.BeforeRequest > Requests.size())
      return invalid(
          "request boundaries must be ordered and within requests.size()");
    Previous = E.BeforeRequest;
    if (E.Type == DriverProcessEvent::Kind::Create)
      Created.insert(E.ProcessID);
  }
  for (const auto &R : Requests)
    if (fileRequest(R) && R.RequestorProcessID &&
        !Created.contains(R.RequestorProcessID)) {
      Live.insert(R.RequestorProcessID);
      Seen.insert(R.RequestorProcessID);
    }
  size_t Cursor = 0, RegistryCursor = 0;
  for (size_t Boundary = 0; Boundary <= Requests.size(); ++Boundary) {
    while (Cursor < Events.size() && Events[Cursor].BeforeRequest == Boundary) {
      const auto &E = Events[Cursor++];
      if (!E.ProcessID || E.ProcessID == 4 || !E.ParentID ||
          E.ProcessID == E.ParentID)
        return invalid("requires distinct nonzero target and parent IDs; "
                       "System cannot exit");
      if (E.Type == DriverProcessEvent::Kind::Create) {
        if (Seen.contains(E.ProcessID) || !Live.contains(E.ParentID) ||
            !Live.contains(E.CreatorProcessID))
          return invalid(
              "create requires a fresh PID and live parent and creator");
        Seen.insert(E.ProcessID);
        Live.insert(E.ProcessID);
        Parents.emplace(E.ProcessID, E.ParentID);
      } else if (E.Type == DriverProcessEvent::Kind::Exit) {
        if (E.CreatorProcessID != 4 || !Live.contains(E.ProcessID) ||
            (Parents.contains(E.ProcessID) &&
             Parents.at(E.ProcessID) != E.ParentID))
          return invalid("exit requires a live PID and its original parent; "
                         "creator is create-only");
        Live.erase(E.ProcessID);
      } else {
        return invalid("unknown native event kind");
      }
    }
    while (RegistryCursor < RegistryEvents.size() &&
           RegistryEvents[RegistryCursor].BeforeRequest == Boundary) {
      if (!Live.contains(RegistryEvents[RegistryCursor++].ActorProcessID))
        return invalid("registry actor is not live at its request boundary");
    }
    if (Boundary < Requests.size() && fileRequest(Requests[Boundary])) {
      const auto &R = Requests[Boundary];
      if (!Live.contains(R.RequestorProcessID))
        return invalid("requestor is not live at its request boundary");
      if (R.RequestorExitAfterDispatch.value_or(false))
        return invalid(
            "use an explicit exit event when process_events are declared");
    }
  }
  return llvm::Error::success();
}
llvm::Expected<std::vector<DriverProcessEvent>>
parseDriverProcessEvents(const llvm::json::Value &Value) {
  const auto *Array = Value.getAsArray();
  if (!Array || Array->size() > MaxDriverProcessEvents)
    return invalid("requires a bounded array");
  std::vector<DriverProcessEvent> Events;
  for (const auto &V : *Array) {
    const auto *O = V.getAsObject();
    if (!O)
      return invalid("each event must be an object");
    for (const auto &[Key, Ignored] : *O)
      if (Key != "kind" && Key != "process_id" && Key != "parent_id" &&
          Key != "creator_process_id" && Key != "before_request" &&
          Key != "exit_status" && Key != "create_info")
        return invalid("unknown field " + Key.str());
    auto Kind = O->getString("kind");
    if (!Kind || (*Kind != "create" && *Kind != "exit"))
      return invalid("kind must be create or exit");
    DriverProcessEvent E;
    E.Type = *Kind == "create" ? DriverProcessEvent::Kind::Create
                               : DriverProcessEvent::Kind::Exit;
    if (const auto *Raw = O->get("create_info")) {
      const auto *Info = Raw->getAsObject();
      if (!Info || E.Type != DriverProcessEvent::Kind::Create)
        return invalid("create_info requires a create-only object");
      for (const auto &[Key, Ignored] : *Info)
        if (Key != "image_file_name" && Key != "file_open_name_available" &&
            Key != "command_line")
          return invalid("unknown create_info field " + Key.str());
      auto Image = Info->getString("image_file_name");
      auto Full = Info->getBoolean("file_open_name_available");
      if (!Image || !Full)
        return invalid("create_info requires image_file_name and "
                       "file_open_name_available");
      DriverProcessCreateInfo C;
      C.ImageFileName = Image->str();
      C.FileOpenNameAvailable = *Full;
      if (const auto *Line = Info->get("command_line")) {
        if (auto Text = Line->getAsString())
          C.CommandLine = Text->str();
        else if (!Line->getAsNull())
          return invalid("command_line requires a string or null");
      }
      E.CreateInfo = std::move(C);
    }
    if (const auto *Raw = O->get("exit_status")) {
      auto N = Raw->getAsUINT64();
      if (E.Type != DriverProcessEvent::Kind::Exit || !N || *N > UINT32_MAX)
        return invalid(
            "exit_status requires an unsigned 32-bit exit-only value");
      E.ExitStatus = uint32_t(*N);
    }
    for (auto Field :
         {"process_id", "parent_id", "creator_process_id", "before_request"}) {
      const auto *Raw = O->get(Field);
      if (!Raw && (llvm::StringRef(Field) == "process_id" ||
                   llvm::StringRef(Field) == "parent_id"))
        return invalid("process_id and parent_id are required");
      if (!Raw)
        continue;
      auto N = Raw->getAsUINT64();
      if (!N || *N > UINT32_MAX)
        return invalid("IDs and boundaries must be unsigned 32-bit integers");
      if (llvm::StringRef(Field) == "process_id")
        E.ProcessID = *N;
      else if (llvm::StringRef(Field) == "parent_id")
        E.ParentID = *N;
      else if (llvm::StringRef(Field) == "before_request")
        E.BeforeRequest = *N;
      else {
        if (E.Type == DriverProcessEvent::Kind::Exit)
          return invalid("creator_process_id is create-only");
        E.CreatorProcessID = *N;
      }
    }
    Events.push_back(E);
  }
  return Events;
}
llvm::json::Value
driverProcessEventsJSON(llvm::ArrayRef<DriverProcessEvent> Events) {
  llvm::json::Array A;
  for (const auto &E : Events)
    A.push_back(eventJSON(E));
  return A;
}
llvm::json::Value
driverProcessEventResultsJSON(llvm::ArrayRef<DriverProcessEventResult> Events) {
  llvm::json::Array A;
  for (const auto &E : Events) {
    auto O = eventJSON(E.Event);
    O["source_index"] = uint64_t(E.SourceIndex);
    O["actor_process_id"] = E.ActorProcessID;
    O["completed"] = E.Completed;
    O["creation_status"] = E.CreationStatus
                               ? llvm::json::Value(*E.CreationStatus)
                               : llvm::json::Value(nullptr);
    llvm::json::Array Calls;
    for (auto PC : E.DeliveredRoutines)
      Calls.push_back("0x" + llvm::utohexstr(PC));
    O["delivered_routines"] = std::move(Calls);
    A.push_back(std::move(O));
  }
  return A;
}
} // namespace neverd::emulation
