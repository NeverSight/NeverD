//===- TestSupport.h - Inert MCP test inputs --------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Private disposable fixture ownership and assertions shared by C++ tests.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd-web-mcp/Server.h"

#include <chrono>
#include <filesystem>
#include <fstream>

namespace neverd::mcp::test {
inline void require(bool Condition, const char *Message) {
  if (!Condition)
    throw std::runtime_error(Message);
}
inline void redacted(const Json &Reply) {
  require(Reply.dump().find("CANARY") == std::string::npos,
          "MCP output exposed private evidence");
}
inline Json evidence(const Json &Reply, bool Failed = false) {
  redacted(Reply);
  require(Reply.at("isError") == Failed, "Unexpected tool failure state");
  require(Reply.at("content").size() == 1 &&
              Reply.at("content")[0].at("type") == "text",
          "Missing equivalent JSON text result");
  const auto &Value = Reply.at("structuredContent");
  require(transport::parseJson(
              Reply.at("content")[0].at("text").get<std::string>()) == Value,
          "Structured evidence differs from JSON text");
  return Value;
}
inline Json request(const char *Method, Json Params = Json::object(),
                    Json ID = 1) {
  return {{"jsonrpc", "2.0"},
          {"id", ID},
          {"method", Method},
          {"params", std::move(Params)}};
}
inline Json initialize() {
  return request("initialize",
                 {{"protocolVersion", ProtocolVersion},
                  {"capabilities", Json::object()},
                  {"clientInfo", {{"name", "native-test"}, {"version", "1"}}}});
}
struct Fixture {
  std::filesystem::path Root;
  Fixture() {
    const auto Stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned I = 0; I < 100; ++I) {
      Root = std::filesystem::temp_directory_path() /
             ("neverd-mcp-CANARY-" + std::to_string(Stamp) + "-" +
              std::to_string(I));
      if (std::filesystem::create_directory(Root))
        return;
    }
    throw std::runtime_error("Cannot create test directory");
  }
  ~Fixture() {
    std::error_code EC;
    std::filesystem::remove_all(Root, EC);
  }
  std::string path(std::string_view Name = "input") const {
    return (Root / Name).string();
  }
  void write(std::string_view Name, std::string_view Bytes) const {
    std::ofstream Out(Root / Name, std::ios::binary);
    Out.write(Bytes.data(), static_cast<std::streamsize>(Bytes.size()));
    require(Out.good(), "Cannot write test input");
  }
};
} // namespace neverd::mcp::test
