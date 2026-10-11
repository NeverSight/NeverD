//===- Server.cpp - Offline web MCP lifecycle -----------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// MCP 2025-06-18 lifecycle and fixed JSON-RPC diagnostics.
///
//===----------------------------------------------------------------------===//

#include "Server.h"

#include <algorithm>
#include <initializer_list>

namespace neverd::mcp {
namespace {
bool boundedString(const Json &Value, std::size_t Maximum, bool Empty = false) {
  if (!Value.is_string())
    return false;
  const auto &S = Value.get_ref<const std::string &>();
  return (Empty || !S.empty()) && S.size() <= Maximum &&
         S.find('\0') == std::string::npos;
}

bool validID(const Json &Value) {
  return Value.is_number_integer() || boundedString(Value, 128, true);
}

void fields(const Json &Parameters,
            std::initializer_list<std::string_view> Allowed) {
  if (!Parameters.is_object())
    throw RpcError{-32602, "Invalid params"};
  for (const auto &[Key, Value] : Parameters.items()) {
    if (Key == "_meta") {
      if (!Value.is_object())
        throw RpcError{-32602, "Invalid params"};
    } else if (std::find(Allowed.begin(), Allowed.end(), Key) == Allowed.end())
      throw RpcError{-32602, "Invalid params"};
  }
}

const Json &required(const Json &Object, const char *Name) {
  const auto It = Object.find(Name);
  if (It == Object.end())
    throw RpcError{-32602, "Invalid params"};
  return *It;
}
} // namespace

Json errorReply(const Json &ID, int Code, const char *Message) {
  return {{"jsonrpc", "2.0"},
          {"id", ID},
          {"error", {{"code", Code}, {"message", Message}}}};
}

Json Server::request(const std::string &Method, const Json &Parameters) {
  if (Method == "ping") {
    fields(Parameters, {});
    return Json::object();
  }
  if (Method == "initialize") {
    if (Current != State::Uninitialized)
      throw RpcError{-32600, "Already initialized"};
    fields(Parameters, {"protocolVersion", "capabilities", "clientInfo"});
    if (!boundedString(required(Parameters, "protocolVersion"), 32) ||
        !required(Parameters, "capabilities").is_object())
      throw RpcError{-32602, "Invalid params"};
    const auto &Client = required(Parameters, "clientInfo");
    if (!Client.is_object() || !boundedString(required(Client, "name"), 128) ||
        !boundedString(required(Client, "version"), 128) ||
        (Client.contains("title") && !boundedString(Client["title"], 256)))
      throw RpcError{-32602, "Invalid params"};
    Json Result{{"protocolVersion", ProtocolVersion},
                {"capabilities", {{"tools", {{"listChanged", false}}}}},
                {"serverInfo", {{"name", "neverd-web-mcp"}, {"version", "1"}}}};
    Current = State::AwaitInitialized;
    return Result;
  }
  if (Current != State::Ready)
    throw RpcError{-32002, "Server is not initialized"};
  if (Method == "tools/list") {
    fields(Parameters, {"cursor"});
    Json Selected = Json::object();
    if (Parameters.contains("cursor")) {
      if (!boundedString(Parameters["cursor"], 128))
        throw RpcError{-32602, "Invalid params"};
      Selected["cursor"] = Parameters["cursor"];
    }
    return Tools.list(Selected);
  }
  if (Method == "tools/call") {
    fields(Parameters, {"name", "arguments"});
    const auto &Name = required(Parameters, "name");
    if (!boundedString(Name, 128) || (Parameters.contains("arguments") &&
                                      !Parameters["arguments"].is_object()))
      throw RpcError{-32602, "Invalid params"};
    return Tools.call(Name.get<std::string>(),
                      Parameters.value("arguments", Json::object()));
  }
  throw RpcError{-32601, "Method not found"};
}

std::optional<Json> Server::handle(std::string_view Message) {
  Json Envelope;
  try {
    Envelope = transport::parseJson(Message, MaxRequestBytes);
  } catch (...) {
    return errorReply(nullptr, -32700, "Parse error");
  }
  if (!Envelope.is_object() || !Envelope.contains("jsonrpc") ||
      Envelope["jsonrpc"] != "2.0")
    return errorReply(nullptr, -32600, "Invalid Request");
  const bool HasID = Envelope.contains("id");
  // This server sends no requests. Never answer an unsolicited response,
  // including a peer's error with null id, and create a response loop.
  if (!Envelope.contains("method") && HasID &&
      (Envelope.contains("result") || Envelope.contains("error")))
    return std::nullopt;
  if (HasID && !validID(Envelope["id"]))
    return errorReply(nullptr, -32600, "Invalid Request");
  const Json ID = HasID ? Envelope["id"] : Json(nullptr);
  if (!Envelope.contains("method") || !Envelope["method"].is_string())
    return errorReply(ID, -32600, "Invalid Request");
  if (!boundedString(Envelope["method"], 128))
    return HasID ? std::optional(errorReply(ID, -32600, "Invalid Request"))
                 : std::nullopt;
  for (const auto &[Key, Value] : Envelope.items())
    if (Key != "jsonrpc" && Key != "id" && Key != "method" && Key != "params")
      return HasID ? std::optional(errorReply(ID, -32600, "Invalid Request"))
                   : std::nullopt;
  const auto Method = Envelope["method"].get<std::string>();
  const auto Parameters = Envelope.value("params", Json::object());
  if (!HasID) {
    // Valid notifications never execute tools, even if the method names one.
    if (Method == "notifications/initialized" &&
        Current == State::AwaitInitialized) {
      try {
        fields(Parameters, {});
        Current = State::Ready;
      } catch (const RpcError &) {
      }
    }
    return std::nullopt;
  }
  try {
    return Json{{"jsonrpc", "2.0"},
                {"id", ID},
                {"result", request(Method, Parameters)}};
  } catch (const RpcError &E) {
    return errorReply(ID, E.Code, E.Message);
  } catch (...) {
    return errorReply(ID, -32603, "Internal error");
  }
}
} // namespace neverd::mcp
