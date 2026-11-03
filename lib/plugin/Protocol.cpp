//===- Protocol.cpp - Shared provider wire protocol -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Version-one protocol admission for GUI and native headless consumers.
///
//===----------------------------------------------------------------------===//

#include "neverd/plugin/Protocol.h"

#include "Text.h"

#include "neverd/plugin/Config.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace neverd::plugin {
using namespace limits;
namespace {
std::string string(const Json &Object, const char *Key) {
  auto I = Object.find(Key);
  return I != Object.end() && I->is_string() ? I->get<std::string>()
                                             : std::string();
}
bool progress(const Json &Value) {
  if (!Value.is_object() || !Value.contains("label") ||
      !Value["label"].is_string() ||
      detail::textUnits(Value["label"].get_ref<const std::string &>()) >
          kMaxProgressCharacters)
    return false;
  for (auto I = Value.begin(); I != Value.end(); ++I) {
    if (I.key() == "label")
      continue;
    if ((I.key() != "done" && I.key() != "total") || !I->is_number())
      return false;
    double N = I->get<double>();
    if (!std::isfinite(N) || N < 0 || N > INT32_MAX || std::floor(N) != N)
      return false;
  }
  return !Value.contains("total") ||
         (Value.contains("done") &&
          Value["done"].get<double>() <= Value["total"].get<double>());
}
} // namespace

Protocol::Protocol(Tasks &Controller, Json Start,
                   std::vector<std::string> Queries,
                   ProtocolCallbacks Callbacks, ProtocolExtensions Extensions)
    : Controller(Controller), Task(Start.at("task_id")),
      Start(std::move(Start)), Queries(std::move(Queries)),
      Callbacks(std::move(Callbacks)), Extensions(std::move(Extensions)) {}

std::string Protocol::frame(Json Value) {
  Value["protocol"] = 1;
  const auto Bytes = Value.dump();
  if (Bytes.size() > kMaxFrameBytes)
    return {};
  const auto Size = uint32_t(Bytes.size());
  std::string Result;
  for (int Shift : {24, 16, 8, 0})
    Result += char((Size >> Shift) & 0xff);
  return Result + Bytes;
}

void Protocol::fail(std::string Error) {
  if (!Failure.empty())
    return;
  Failure = std::move(Error);
  Callbacks.Failed(Failure);
}
void Protocol::send(Json Value) {
  auto Bytes = frame(std::move(Value));
  if (Bytes.empty() || !Callbacks.Send(std::move(Bytes)))
    fail("Plugin write queue exceeded its limit or transport closed");
}

void Protocol::receive(std::string_view Bytes) {
  if (!Failure.empty())
    return;
  const auto State = Controller.snapshot(Task);
  if (!State || (State->Phase != TaskPhase::Starting &&
                 State->Phase != TaskPhase::Running))
    return;
  ReadBytes += Bytes.size();
  if (ReadBytes > kMaxTransportBytes) {
    fail("Plugin output exceeded its 16 MiB job limit");
    return;
  }
  Input.append(Bytes);
  while (Input.size() >= 4 && Failure.empty()) {
    uint32_t Size = 0;
    for (unsigned I = 0; I < 4; ++I)
      Size = (Size << 8) | static_cast<unsigned char>(Input[I]);
    if (!Size || Size > kMaxFrameBytes) {
      fail("Invalid plugin frame size");
      return;
    }
    if (Input.size() < size_t(Size) + 4)
      return;
    auto Value = Json::parse(Input.begin() + 4, Input.begin() + Size + 4,
                             nullptr, false);
    Input.erase(0, Size + 4);
    if (Value.is_discarded() || !Value.is_object()) {
      fail("Malformed plugin JSON");
      return;
    }
    message(Value);
    const auto State = Controller.snapshot(Task);
    if (!State || State->Phase != TaskPhase::Running)
      return;
  }
}

void Protocol::message(const Json &Value) {
  if (!Value.contains("protocol") || Value["protocol"] != 1 ||
      ++Messages > kMaxMessages) {
    fail("Invalid plugin protocol or message budget");
    return;
  }
  const auto Type = string(Value, "type");
  if (Type == "hello" && !Greeted) {
    Greeted = true;
    Controller.hello(Task);
    if (Callbacks.Hello)
      Callbacks.Hello();
    send(Start);
    return;
  }
  if (!Greeted || string(Value, "task_id") != Task || Completed) {
    fail("Unexpected plugin message order or task identity");
    return;
  }
  if (Type == "progress") {
    if (!Value.contains("progress") || !progress(Value["progress"])) {
      fail("Invalid plugin progress");
      return;
    }
    if (Callbacks.Progress)
      Callbacks.Progress(Value["progress"]);
  } else if (Type == "log") {
    const auto Text = string(Value, "text");
    if (!Value.contains("text") || !Value["text"].is_string() ||
        detail::textUnits(Text) > kMaxDiagnosticCharacters) {
      fail("Invalid plugin log");
      return;
    }
    LogBytes += Text.size();
    if (LogBytes > kMaxLogBytes) {
      fail("Plugin log exceeded its 64 KiB limit");
      return;
    }
    if (Callbacks.Log)
      Callbacks.Log(Text);
  } else if (Type == "host") {
    const auto Id = string(Value, "id"), Operation = string(Value, "operation");
    if (!Extensions.HostCapabilities || Id.empty() ||
        detail::textUnits(Id) > kMaxRequestIdCharacters ||
        Pending.contains(Id) || Pending.size() >= kMaxOutstandingQueries ||
        !Value.contains("payload") || !Value["payload"].is_object()) {
      fail("Invalid or unavailable host capability request");
      return;
    }
    Pending.insert(Id);
    HostIds.insert(Id);
    Callbacks.Host(Id, Operation, Value["payload"]);
  } else if (Type == "query" || Type == "command") {
    const auto Id = string(Value, "id"), Operation = string(Value, "operation");
    const bool Command = Type == "command";
    const auto &Allowed = Command ? Extensions.Commands : Queries;
    if (Id.empty() || detail::textUnits(Id) > kMaxRequestIdCharacters ||
        Pending.contains(Id) || Pending.size() >= kMaxOutstandingQueries ||
        std::find(Allowed.begin(), Allowed.end(), Operation) == Allowed.end() ||
        !Value.contains("payload") || !Value["payload"].is_object() ||
        !Start["context"].value("loaded", false)) {
      fail("Plugin query is undeclared, stale or invalid");
      return;
    }
    Pending.insert(Id);
    if (Command) {
      CommandIds.insert(Id);
      if (Callbacks.Command)
        Callbacks.Command(Id, Operation, Value["payload"]);
      else
        answer(Id, {{"status", "unsupported"},
                    {"error",
                     {{"code", "service_unavailable"},
                      {"message", "No project command adapter"}}}});
    } else if (Callbacks.Query)
      Callbacks.Query(Id, Operation, Value["payload"]);
    else
      answer(Id, {{"status", "unsupported"},
                  {"error",
                   {{"code", "service_unavailable"},
                    {"message", "No project service adapter"}}}});
  } else if (Type == "result") {
    std::string Error;
    if (!Pending.empty() || !Value.contains("result") ||
        !validateResult(Value["result"], Error, Extensions.StructuredData)) {
      fail(Error.empty() ? "Invalid result or outstanding queries" : Error);
      return;
    }
    Result = Value["result"];
    Completed = Controller.result(Task);
  } else if (Type == "error") {
    auto Text = string(Value, "message");
    fail(Text.empty()
             ? "Plugin failed"
             : std::string(detail::textPrefix(Text, kMaxDiagnosticCharacters)));
  } else
    fail("Unsupported plugin message");
}

void Protocol::answer(const std::string &Id, const Json &Reply) {
  const auto State = Controller.snapshot(Task);
  if (!State || State->Phase != TaskPhase::Running || !Pending.erase(Id))
    return;
  send({{"type", HostIds.erase(Id)      ? "host_reply"
                 : CommandIds.erase(Id) ? "command_reply"
                                        : "query_reply"},
        {"task_id", Task},
        {"id", Id},
        {"reply", Reply}});
}
void Protocol::event(const Json &Notification) {
  const auto State = Controller.snapshot(Task);
  if (!State || State->Phase != TaskPhase::Running || Completed ||
      !Extensions.HostCapabilities)
    return;
  auto Value = Notification;
  Value["type"] = "event";
  Value["task_id"] = Task;
  send(std::move(Value));
}
std::string Protocol::exitError() const {
  if (!Failure.empty())
    return Failure;
  if (!Completed || !Input.empty() || !Pending.empty())
    return "Provider exited without a complete successful result";
  return {};
}
bool Protocol::diagnostic(std::string_view Bytes) {
  LogBytes += Bytes.size();
  if (LogBytes > kMaxLogBytes) {
    fail("Plugin log and diagnostics exceeded their 64 KiB limit");
    return false;
  }
  return true;
}
} // namespace neverd::plugin
