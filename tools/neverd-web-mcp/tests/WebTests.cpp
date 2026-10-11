//===- WebTests.cpp - MCP evidence and shared API parity ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercise real offline analysis through closed MCP schemas, without runtimes.
///
//===----------------------------------------------------------------------===//

#include "../../../unittests/web/BunFixture.h"
#include "../../../unittests/web/SEAFixture.h"
#include "TestSupport.h"
#include "neverd-web-mcp/WebTools.h"

#include <iostream>
#include <memory>
#include <set>

using namespace neverd::mcp;
using namespace neverd::mcp::test;
namespace transport = neverd::transport;
namespace {
template <class F> void invalid(F Run) {
  try {
    Run();
  } catch (const RpcError &E) {
    require(E.Code == -32602, "Tool arguments used the wrong RPC error");
    return;
  }
  throw std::runtime_error("Invalid tool argument was admitted");
}
Json take(const char *Text) {
  std::unique_ptr<const char, decltype(&neverd_free_string)> Owner(
      Text, neverd_free_string);
  require(Text != nullptr, "Missing C API result");
  return transport::parseJson(Text);
}

class Harness {
public:
  WebTools Tools;
  neverd::web_client::Backend Reference;
  const std::vector<std::string> Inputs;
  std::string Revision;
  explicit Harness(std::vector<std::string> Paths)
      : Tools(Paths), Inputs(std::move(Paths)) {}

  Json get(const char *Operation, Json Args = Json::object()) {
    return evidence(Tools.call("neverd_" + std::string(Operation), Args));
  }
  Json same(const char *Operation, Json Args) {
    auto Actual = get(Operation, Args);
    Args["schema_version"] = 1;
    require(Actual == Reference.execute(Operation, Args),
            "MCP evidence differs from the shared worker/C API adapter");
    return Actual;
  }
  Json fail(const char *Operation, Json Args, const char *Code) {
    auto Error =
        evidence(Tools.call("neverd_" + std::string(Operation), Args), true);
    require(Error.at("error").at("code") == Code,
            "Wrong tool failure diagnostic");
    return Error;
  }
  Json import(std::size_t Index) {
    const auto Preview = get("web_import_preview", {{"input_index", Index}});
    const auto Other =
        Reference.execute("web_import_preview",
                          {{"schema_version", 1}, {"path", Inputs.at(Index)}});
    const auto Commit = get("web_import_commit",
                            {{"preview_token", Preview.at("preview_token")}});
    require(Commit == Reference.execute(
                          "web_import_commit",
                          {{"schema_version", 1},
                           {"preview_token", Other.at("preview_token")}}),
            "MCP publication differs from the backend");
    Revision = Commit.at("revision").get<std::string>();
    same("web_metadata", Json::object());
    const auto Items = same("web_artifacts", {{"revision", Revision}});
    require(Items.at("items").size() == 1, "Unexpected single-file capture");
    return Items.at("items")[0].at("artifact_id");
  }
};

void discovery(Harness &H, const Json &Caps) {
  std::set<std::string> Names;
  Json Params = Json::object();
  for (;;) {
    const auto Page = H.Tools.list(Params);
    redacted(Page);
    for (const auto &T : Page.at("tools"))
      require(Names.insert(T.at("name").get<std::string>()).second,
              "Discovery duplicated a tool");
    if (!Page.contains("nextCursor"))
      break;
    Params["cursor"] = Page.at("nextCursor");
  }
  require(Names.contains("neverd_web_capabilities"),
          "Capabilities tool missing");
  if (Caps.value("status", "") != "ok") {
    require(Names.size() == 1, "Omitted backend exposed analysis tools");
    return;
  }
  for (const auto &Operation : Caps.at("operations")) {
    const auto Name = Operation.get<std::string>();
    const bool Omitted = Name == "bun_export" || Name == "native_analyze";
    require(Names.contains("neverd_web_" + Name) != Omitted,
            "MCP catalog drifted from the documented backend subset");
  }
}

void sourceAndMaps(Harness &H) {
  const auto Artifact = H.import(0);
  const Json SourceArgs{{"revision", H.Revision},
                        {"artifact_id", Artifact},
                        {"source_type", "module"}};
  const auto Source = H.same("web_source_analyze", SourceArgs);
  const Json Args{{"revision", H.Revision},
                  {"source_id", Source.at("source_id")}};
  H.same("web_source_nodes", Args);
  for (const auto *Op :
       {"web_source_bindings_analyze", "web_source_semantics_analyze",
        "web_source_modules_analyze", "web_source_bundles_analyze",
        "web_source_navigation_analyze", "web_interfaces_analyze"})
    H.same(Op, Args);
  H.same("web_source_semantic_records", Args);
  auto Bindings = Args;
  Bindings["record_kind"] = "bindings";
  H.same("web_source_binding_records", Bindings);
  const auto Preview = H.same("web_source_view_preview", Args);
  const auto View = H.same("web_source_view_commit",
                           {{"revision", H.Revision},
                            {"preview_token", Preview.at("preview_token")}});
  const Json ViewArgs{{"revision", H.Revision},
                      {"view_id", View.at("view_id")}};
  H.same("web_source_view_records", ViewArgs);
  auto Chunk = ViewArgs;
  Chunk["byte_offset"] = "0";
  Chunk["byte_limit"] = 65536;
  H.same("web_source_view_chunk", Chunk);
  auto Anchor = Args;
  Anchor.update({{"byte_offset", "0"},
                 {"byte_length", "5"},
                 {"view_id", View.at("view_id")}});
  H.same("web_source_anchor", Anchor);
  auto Unsafe = Args;
  Unsafe["options"] = {
      {"reviewed_ranges",
       Json::array({{{"byte_offset", "0"}, {"byte_length", "100"}}})}};
  invalid([&] { H.get("web_source_view_preview", Unsafe); });
  auto Stale = Args;
  Stale["revision"] = "0";
  H.fail("web_source_nodes", Stale, "stale_revision");

  const auto MapArtifact = H.import(6);
  const auto Map =
      H.same("web_source_map_analyze",
             {{"revision", H.Revision}, {"artifact_id", MapArtifact}});
  const Json MapArgs{{"revision", H.Revision}, {"map_id", Map.at("map_id")}};
  H.same("web_source_map_sources", MapArgs);
  H.same("web_source_map_segments", MapArgs);
}

void archives(Harness &H, bool HasParser) {
  auto Artifact = H.import(1);
  const Json Args{{"revision", H.Revision},
                  {"artifact_id", Artifact},
                  {"profile", "node-sea-22.15.0-blob-le64-v1"}};
  const auto SEA = H.same("web_sea_extract", Args);
  require(SEA.at("asset_count") == 2, "SEA assets missing");
  auto Page = Json{{"revision", H.Revision},
                   {"extraction_id", SEA.at("extraction_id")},
                   {"limit", 1}};
  const auto First = H.same("web_sea_records", Page);
  require(First.at("items").size() == 1, "SEA records did not paginate");
  Page["offset"] = 1;
  H.same("web_sea_records", Page);
  if (HasParser) {
    const auto Source = H.same("web_source_analyze",
                               {{"revision", H.Revision},
                                {"artifact_id", SEA.at("source_artifact_id")},
                                {"source_type", "commonjs"}});
    H.same("web_source_anchor", {{"revision", H.Revision},
                                 {"source_id", Source.at("source_id")},
                                 {"byte_offset", "0"},
                                 {"byte_length", "6"}});
  }
  Artifact = H.import(2);
  const auto Bun = H.same(
      "web_bun_extract", {{"revision", H.Revision}, {"artifact_id", Artifact}});
  require(Bun.at("module_count") == 3, "Bun modules missing");
  const auto Modules =
      H.same("web_bun_records", {{"revision", H.Revision},
                                 {"extraction_id", Bun.at("extraction_id")},
                                 {"record_kind", "modules"}});
  require(Modules.at("items").size() == 3, "Bun metadata page missing");
  invalid([&] { H.get("web_bun_export", {{"revision", H.Revision}}); });

  Artifact = H.import(3);
  const auto Package =
      H.same("web_packages_analyze", {{"revision", H.Revision},
                                      {"artifact_id", Artifact},
                                      {"input_kind", "package-json"}});
  H.same("web_package_records",
         {{"revision", H.Revision},
          {"analysis_id", Package.at("package_analysis_id")},
          {"record_kind", "scripts"}});
}

void passiveInputs(Harness &H) {
  auto Artifact = H.import(4);
  const auto Preview = H.same(
      "web_har_preview", {{"revision", H.Revision}, {"artifact_id", Artifact}});
  const Json Page{{"revision", H.Revision},
                  {"capture_id", Preview.at("capture_id")}};
  H.fail("web_har_records", Page, "har_capture_not_committed");
  H.same("web_har_commit", {{"revision", H.Revision},
                            {"preview_token", Preview.at("preview_token")}});
  H.same("web_har_records", Page);

  Artifact = H.import(5);
  const auto Stream = H.same("web_stream_preview",
                             {{"revision", H.Revision},
                              {"artifact_id", Artifact},
                              {"profile", "recorded-jsonrpc-2.0-jsonl-v1"}});
  const Json StreamPage{{"revision", H.Revision},
                        {"capture_id", Stream.at("stream_capture_id")}};
  H.fail("web_stream_records", StreamPage, "stream_capture_not_committed");
  H.same("web_stream_commit", {{"revision", H.Revision},
                               {"preview_token", Stream.at("preview_token")}});
  const auto Records = H.same("web_stream_records", StreamPage);
  require(Records.at("items").size() == 2 &&
              Records.at("protocol_negotiation_verified") == false,
          "Stream facts or evidence class changed");
}
} // namespace

int main() {
  try {
    Fixture F;
    std::vector<std::string> Inputs;
    for (const auto *Name :
         {"source", "sea", "bun", "package", "har", "stream", "map"})
      Inputs.push_back(F.path(Name));
    // The configured paths deliberately do not exist yet: construction must
    // neither capture nor reject them before an explicit preview request.
    Harness H(Inputs);
    const auto Caps = H.get("web_capabilities");
    auto Backend = Caps.at("backend");
    discovery(H, Backend);
    if (Backend.value("status", "") != "ok") {
      invalid([&] { H.get("web_import_preview", {{"input_index", 0}}); });
      std::cout << "MCP backend-omitted capabilities passed\n";
      return 0;
    }
    Backend.erase("operation_prefix");
    Backend.erase("payload_schema_version");
    require(Backend == take(neverd_web_capabilities_json()),
            "MCP changed C API capabilities");
    if (Backend.value("input_reader", "") == "unavailable")
      return 77;
    H.fail("web_metadata", Json::object(), "no_project");
    H.fail("web_import_preview", {{"input_index", 0}}, "input_unavailable");
    invalid([&] {
      H.get("web_import_preview", {{"input_index", 0}, {"path", "CANARY"}});
    });
    invalid([&] { H.get("web_import_preview", {{"input_index", 7}}); });
    invalid([&] {
      H.get("web_native_analyze",
            {{"revision", "1"}, {"handoff_id", "CANARY"}});
    });
    F.write("source", "fetch('https://CANARY/a?token=CANARY'); const "
                      "privateName = 'CANARY'; while(true) {};");
    F.write("sea", neverd::web::sea_test::blob(12));
    F.write("bun", neverd::web::test::BunFixture().Bytes);
    F.write(
        "package",
        R"({"name":"CANARY","version":"1.0.0","scripts":{"postinstall":"CANARY"}})");
    F.write(
        "har",
        R"({"log":{"version":"1.2","entries":[{"request":{"method":"GET","url":"https://CANARY/a?token=CANARY","headers":[{"name":"Authorization","value":"CANARY"}]},"response":{"status":200,"content":{"text":"CANARY"}}}]}})");
    F.write(
        "stream",
        R"({"session":"CANARY","direction":"client_to_server","message":{"jsonrpc":"2.0","id":"CANARY","method":"CANARY","params":{"key":"CANARY"}}})"
        "\n"
        R"({"session":"CANARY","direction":"server_to_client","message":{"jsonrpc":"2.0","id":"CANARY","result":"CANARY"}})"
        "\n");
    F.write(
        "map",
        R"({"version":3,"sources":["CANARY.js"],"sourcesContent":["throw 'CANARY';"],"mappings":"AAAA"})");
    const auto &Ops = Backend.at("operations");
    const bool HasParser =
        std::find(Ops.begin(), Ops.end(), "source_analyze") != Ops.end();
    if (HasParser)
      sourceAndMaps(H);
    else
      invalid([&] { H.get("web_source_analyze", Json::object()); });
    archives(H, HasParser);
    passiveInputs(H);
    WebTools Isolated(Inputs);
    const auto Error =
        evidence(Isolated.call("neverd_web_metadata", Json::object()), true);
    require(Error.at("error").at("code") == "no_project",
            "Sessions shared publication state");
    std::cout << "MCP capability, evidence, redaction, parity and session "
                 "contracts passed; parser="
              << HasParser << '\n';
    return 0;
  } catch (const std::exception &E) {
    std::cerr << E.what() << '\n';
  } catch (const RpcError &E) {
    std::cerr << E.Code << ": " << E.Message << '\n';
  }
  return 1;
}
