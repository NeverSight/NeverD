//===- ProtocolTests.cpp - MCP lifecycle and framing contracts ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Inert C++ tool service exercises protocol behavior without an engine.
///
//===----------------------------------------------------------------------===//

#include "neverd-web-mcp/Server.h"
#include "neverd-web-mcp/Stdio.h"

#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

using namespace neverd::mcp;

namespace {
void require(bool Condition, const char *Message) {
  if (!Condition)
    throw std::runtime_error(Message);
}

struct InertTools final : ToolService {
  unsigned Lists = 0, Calls = 0;
  Json LastParams, LastArguments;
  Json ListReply{{"tools", Json::array()}};

  Json list(const Json &Params) override {
    ++Lists;
    LastParams = Params;
    return ListReply;
  }
  Json call(const std::string &Name, const Json &Arguments) override {
    if (Name != "known")
      throw RpcError{-32602, "Unknown tool"};
    ++Calls;
    LastArguments = Arguments;
    return {{"content", Json::array()},
            {"isError", Arguments.contains("fail")}};
  }
};

Json request(const char *Method, Json Parameters = Json::object(),
             Json ID = 1) {
  return {{"jsonrpc", "2.0"},
          {"id", ID},
          {"method", Method},
          {"params", Parameters}};
}

Json initialize() {
  return request("initialize",
                 {{"protocolVersion", "2025-06-18"},
                  {"capabilities", Json::object()},
                  {"clientInfo", {{"name", "inert-test"}, {"version", "1"}}}});
}

Json call(Server &S, const Json &Message) {
  const auto Reply = S.handle(Message.dump());
  require(Reply.has_value(), "Missing response");
  return *Reply;
}

void ready(Server &S) {
  require(call(S, initialize()).contains("result"), "Initialization failed");
  require(
      !S.handle(R"({"jsonrpc":"2.0","method":"notifications/initialized"})"),
      "Notification produced a response");
}

void error(Server &S, const Json &Message, int Code) {
  const auto Reply = call(S, Message);
  require(Reply.contains("error") && Reply["error"]["code"] == Code,
          "Unexpected error classification");
}

void lifecycle() {
  InertTools T;
  Server S(T);
  require(call(S, request("ping"))["result"].empty(), "Pre-init ping failed");
  error(S, request("tools/list"), -32002);
  auto Init = initialize();
  Init["params"].erase("capabilities");
  error(S, Init, -32602);
  Init = initialize();
  Init["params"]["protocolVersion"] = "2024-11-05";
  Init["params"]["clientInfo"]["title"] = "Test client";
  const auto Reply = call(S, Init);
  require(Reply["result"]["protocolVersion"] == "2025-06-18",
          "Wrong version fallback");
  error(S, request("tools/list"), -32002);
  error(S, initialize(), -32600);
  S.handle(
      R"({"jsonrpc":"2.0","method":"notifications/initialized","params":{"bad":true}})");
  error(S, request("tools/list"), -32002);
  S.handle(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
  require(call(S, request("tools/list")).contains("result"),
          "Ready server refused tools");
  error(S, initialize(), -32600);
  require(call(S, request("tools/list")).contains("result") && T.Lists == 2,
          "Repeated initialize altered state");
}

void identifiersAndEnvelopes() {
  InertTools T;
  Server S(T);
  for (const Json &ID :
       std::vector<Json>{0, -7, std::string("请求"), "",
                         std::uint64_t(9223372036854775813ULL)}) {
    const auto Reply = call(S, request("ping", Json::object(), ID));
    require(Reply["id"] == ID, "Request ID changed");
  }
  for (const auto &ID : {Json(nullptr), Json(true), Json(1.5), Json::array(),
                         Json(std::string(129, 'x'))}) {
    const auto Reply = call(S, request("ping", Json::object(), ID));
    require(Reply["id"].is_null() && Reply["error"]["code"] == -32600,
            "Invalid ID accepted");
  }
  error(S, Json::array({request("ping")}), -32600);
  auto Message = request("ping");
  Message["jsonrpc"] = "1.0";
  error(S, Message, -32600);
  Message = request("ping");
  Message["extra"] = "CANARY";
  error(S, Message, -32600);
  error(S, request("ping", Json::array()), -32602);
}

void notificationsNeverExecute() {
  InertTools T;
  Server S(T);
  auto Message = initialize();
  Message.erase("id");
  require(!S.handle(Message.dump()), "Initialize notification replied");
  error(S, request("tools/list"), -32002);
  ready(S);
  for (const char *Method :
       {"tools/list", "tools/call", "unknown", "notifications/cancelled"}) {
    Message = request(Method, {{"name", "known"}});
    Message.erase("id");
    require(!S.handle(Message.dump()), "Notification replied");
  }
  require(T.Lists == 0 && T.Calls == 0, "Notification executed a tool");
  for (const auto &Method :
       {std::string(129, 'a'), std::string("a\0b", 3), std::string()}) {
    Message["method"] = Method;
    require(!S.handle(Message.dump()),
            "Method budget produced a notification reply");
  }
  require(!S.handle(R"({"jsonrpc":"2.0","id":1,"result":{}})"),
          "Unsolicited result replied");
  require(!S.handle(R"({"jsonrpc":"2.0","id":null,"error":{"code":-32700}})"),
          "Unsolicited error replied");
}

void metadataAndErrors() {
  InertTools T;
  Server S(T);
  ready(S);
  const Json Meta{{"progressToken", "CANARY_PRIVATE_METADATA"}};
  auto Reply = call(S, request("tools/list", {{"_meta", Meta}}));
  require(T.LastParams.empty(), "MCP metadata reached tool listing");
  require(Reply.dump().find("CANARY") == std::string::npos, "Metadata leaked");
  Reply = call(S, request("tools/call", {{"name", "known"}, {"_meta", Meta}}));
  require(T.LastArguments.empty(), "MCP metadata reached tool arguments");
  require(Reply["result"]["isError"] == false, "Known tool failed");
  error(S, request("tools/call", {{"name", "unknown"}}), -32602);
  error(
      S,
      request("tools/call", {{"name", "known"}, {"arguments", Json::array()}}),
      -32602);
  error(S, request("tools/list", {{"_meta", 7}}), -32602);
  error(S, request("unknown"), -32601);
  require(T.Calls == 1, "Invalid params executed a tool");
  Reply = call(S, request("tools/call", {{"name", "known"},
                                         {"arguments", {{"fail", true}}}}));
  require(Reply["result"]["isError"] == true && !Reply.contains("error"),
          "Tool failure became a protocol error");
}

void hostileJson() {
  InertTools T;
  Server S(T);
  for (const auto &Text : std::vector<std::string>{
           "", "{CANARY", R"({"method":"ping","\u006dethod":"tools/call"})",
           std::string(65, '[') + "0" + std::string(65, ']'),
           std::string("{\"x\":\"") + char(0xff) + "\"}",
           std::string(MaxRequestBytes + 1, ' ')}) {
    const auto Reply = S.handle(Text);
    require(Reply && (*Reply)["error"]["code"] == -32700,
            "Hostile JSON admitted");
    require(Reply->dump().find("CANARY") == std::string::npos,
            "Parse error leaked text");
  }
  require(T.Lists == 0 && T.Calls == 0, "Bad JSON executed a tool");
}

void encodedBudget() {
  using neverd::transport::serializeJson;
  require(serializeJson(nullptr, 4) == "null", "Exact output bound failed");
  bool Refused = false;
  try {
    (void)serializeJson(nullptr, 3);
  } catch (...) {
    Refused = true;
  }
  require(Refused, "Small output bound ignored");
  const Json Escaped = std::string(1000, '\1');
  require(serializeJson(Escaped, 6002).size() == 6002,
          "Escaped output bound changed");
  Refused = false;
  try {
    (void)serializeJson(Escaped, 6001);
  } catch (...) {
    Refused = true;
  }
  require(Refused, "Escaping bypassed the output bound");
}

void stdioFraming() {
  InertTools T;
  Server S(T);
  std::istringstream Input(request("ping").dump() + "\r\n" +
                           request("ping", Json::object(), 2).dump() + "\n");
  std::ostringstream Output;
  require(runStdio(Input, Output, S) == 0, "Complete stdio stream failed");
  std::istringstream Replies(Output.str());
  std::string Line;
  unsigned Count = 0;
  while (std::getline(Replies, Line)) {
    require(Json::parse(Line)["id"] == ++Count, "Stdio replies changed order");
  }
  require(Count == 2, "Unexpected stdio output");
  std::istringstream Truncated(request("ping").dump());
  std::ostringstream Refusal;
  require(runStdio(Truncated, Refusal, S) == 2, "Unterminated frame executed");
  require(Json::parse(Refusal.str())["error"]["code"] == -32700,
          "Wrong EOF error");
  std::istringstream Large(std::string(MaxRequestBytes + 1, 'x'));
  std::ostringstream LargeReply;
  require(runStdio(Large, LargeReply, S) == 2, "Oversized frame did not stop");
  require(LargeReply.str().size() < 256,
          "Oversized input leaked or grew a reply");
}

void atomicResponseBound() {
  InertTools T;
  Server S(T);
  ready(S);
  T.ListReply = {{"tools", std::string(MaxResponseBytes + 1, 'x')}};
  std::istringstream Input(
      request("tools/list", Json::object(), "bounded").dump() + "\n");
  std::ostringstream Output;
  require(runStdio(Input, Output, S) == 0, "Bounded response broke transport");
  const auto Reply = Json::parse(Output.str());
  require(Reply["id"] == "bounded" && Reply["error"]["code"] == -32603,
          "Oversized result produced partial JSON");
  require(Output.str().size() < 256, "Oversized result reached stdout");
}
} // namespace

int main() {
  const std::vector<std::pair<const char *, std::function<void()>>> Cases{
      {"lifecycle", lifecycle},
      {"identifiers", identifiersAndEnvelopes},
      {"notifications", notificationsNeverExecute},
      {"metadata", metadataAndErrors},
      {"hostile-json", hostileJson},
      {"encoded-budget", encodedBudget},
      {"stdio-framing", stdioFraming},
      {"atomic-response", atomicResponseBound}};
  for (const auto &[Name, Run] : Cases) {
    try {
      Run();
    } catch (const std::exception &E) {
      std::cerr << Name << ": " << E.what() << '\n';
      return 1;
    }
  }
  std::cout << Cases.size() << " MCP protocol contracts passed\n";
  return 0;
}
