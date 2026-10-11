//===- WebTools.cpp - MCP adapter for offline web evidence ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Restrict the transport surface while retaining C API evidence and policy.
///
//===----------------------------------------------------------------------===//

#include "WebTools.h"

namespace neverd::mcp {
namespace {
std::vector<std::string> validateInputs(std::vector<std::string> Inputs) {
  if (Inputs.size() > MaxInputs)
    throw std::invalid_argument("Too many configured inputs");
  for (const auto &Path : Inputs)
    if (Path.empty() || Path.size() > 32768 ||
        Path.find('\0') != std::string::npos)
      throw std::invalid_argument("Invalid configured input");
  return Inputs;
}

Json toolResult(Json Value, bool Failed = false) {
  auto Text = transport::serializeJson(Value, MaxToolJsonBytes);
  return {
      {"content", Json::array({{{"type", "text"}, {"text", std::move(Text)}}})},
      {"structuredContent", std::move(Value)},
      {"isError", Failed}};
}
} // namespace

WebTools::WebTools(std::vector<std::string> Selected)
    : Inputs(validateInputs(std::move(Selected))),
      BackendCapabilities(web_client::Backend::capabilities()),
      Available(BackendCapabilities, Inputs.size()) {}

Json WebTools::capabilities() const {
  return {{"schema_version", 1},
          {"status", "ok"},
          {"backend", BackendCapabilities},
          {"mcp",
           {{"profile", "neverd-offline-web-mcp-v1"},
            {"protocol_version", ProtocolVersion},
            {"input_count", Inputs.size()},
            {"tools", Available.names()},
            {"limits",
             {{"request_bytes", MaxRequestBytes},
              {"response_bytes", MaxResponseBytes},
              {"tool_json_bytes", MaxToolJsonBytes},
              {"input_count", MaxInputs}}},
            {"execution", "serial"},
            {"preemptive_cancellation", false},
            {"hard_backend_deadline", false},
            {"omitted",
             {"arbitrary_input_paths", "source_review_options", "raw_exports",
              "native_deep_analysis", "http", "resources", "prompts"}}}}};
}

Json WebTools::list(const Json &Parameters) {
  return Available.list(Parameters);
}

Json WebTools::call(const std::string &Name, const Json &Arguments) {
  const auto &Selected = Available.find(Name);
  auto Payload = Selected.payload(Arguments);
  if (Selected.Dispatch == Tool::Adapter::InputPreview) {
    const auto Index = Payload.at("input_index").get<std::size_t>();
    Payload.erase("input_index");
    Payload["path"] = Inputs.at(Index);
  }
  try {
    return toolResult(Selected.Dispatch == Tool::Adapter::Capabilities
                          ? capabilities()
                          : Backend.execute(Selected.Operation, Payload));
  } catch (const transport::Error &E) {
    if (E.code == "invalid_request" || E.code == "unsupported_schema")
      throw RpcError{-32602, "Invalid tool arguments"};
    // C API codes are fixed diagnostics. Never expose exception text, error
    // detail, configured paths or the tool's arguments in a failure result.
    return toolResult({{"schema_version", 1},
                       {"status", "error"},
                       {"error", {{"code", E.code}}}},
                      true);
  }
}
} // namespace neverd::mcp
