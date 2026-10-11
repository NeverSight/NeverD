//===- CatalogTests.cpp - MCP tool admission contracts --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Capability filtering and argument boundaries independent of analysis.
///
//===----------------------------------------------------------------------===//

#include "neverd-web-mcp/Catalog.h"

#include <functional>
#include <iostream>
#include <set>

using namespace neverd::mcp;
namespace {
void require(bool Condition, const char *Message) {
  if (!Condition)
    throw std::runtime_error(Message);
}
template <class F> void refuses(F Run) {
  try {
    Run();
  } catch (const RpcError &E) {
    require(E.Code == -32602, "Wrong catalog refusal classification");
    return;
  }
  throw std::runtime_error("Invalid tool request was admitted");
}
Json capabilities(Json Operations) {
  return {{"status", "ok"}, {"operations", std::move(Operations)}};
}
void capabilitySubset() {
  for (const auto &Caps : {Json::object(), Json{{"status", "error"}}}) {
    Catalog Disabled(Caps, 1);
    require(Disabled.names() == Json::array({"neverd_web_capabilities"}),
            "Disabled backend advertised unavailable tools");
  }
  auto Caps = capabilities({"import_preview", "source_map_analyze",
                            "source_analyze", "native_analyze", "bun_export",
                            "unknown_future_operation"});
  Catalog C(Caps, 0);
  require(C.names() == Json::array({"neverd_web_capabilities",
                                    "neverd_web_source_analyze",
                                    "neverd_web_source_map_analyze"}),
          "Catalog exposed unconfigured, unknown or excluded operations");
  Catalog Inputs(Caps, 2);
  const auto &Import = Inputs.find("neverd_web_import_preview");
  require(Import.payload({{"input_index", 1}}).at("schema_version") == 1,
          "Launch input selection failed");
  refuses([&] { Import.payload({{"input_index", 2}}); });
  refuses([&] { Import.payload({{"path", "CANARY_PATH"}}); });
}
void closedArguments() {
  Catalog C(capabilities({"source_view_preview", "source_view_chunk",
                          "import_commit", "electron_ipc_analyze"}),
            0);
  const auto &View = C.find("neverd_web_source_view_preview");
  const Json Valid{{"revision", "1"}, {"source_id", "s"}};
  require(View.payload(Valid).at("revision") == "1", "Valid view refused");
  for (const auto *Field : {"options", "reviewed_ranges", "path", "_meta"}) {
    auto Arguments = Valid;
    Arguments[Field] = Json::object();
    refuses([&] { View.payload(Arguments); });
  }
  for (const auto &Schema : {Json(2), Json(true), Json("1"), Json(1.0)}) {
    auto Arguments = Valid;
    Arguments["schema_version"] = Schema;
    refuses([&] { View.payload(Arguments); });
  }
  refuses([&] { View.payload({{"source_id", "s"}}); });
  refuses([&] { C.find("neverd_web_bun_export"); });
  refuses([&] { C.find("neverd_web_native_analyze"); });
  const auto &IPC = C.find("neverd_web_electron_ipc_analyze");
  Json Selection{{"revision", "1"},
                 {"manifest_artifact_id", "a"},
                 {"source_ids", Json::array({"s"})}};
  IPC.payload(Selection);
  for (const auto &IDs :
       {Json::array(), Json::array({1}), Json::array({std::string("s\0x", 3)}),
        Json(std::vector<std::string>(17, "s"))}) {
    Selection["source_ids"] = IDs;
    refuses([&] { IPC.payload(Selection); });
  }
}
void coordinateBounds() {
  Catalog C(capabilities({"source_anchor", "source_view_chunk", "artifacts"}),
            0);
  const auto &Anchor = C.find("neverd_web_source_anchor");
  Json Args{{"revision", "1"},
            {"source_id", "s"},
            {"byte_offset", "0"},
            {"byte_length", "18446744073709551615"}};
  Anchor.payload(Args);
  for (const auto &Offset : {"01", "-1", "+1", "1.0", "18446744073709551616"}) {
    Args["byte_offset"] = Offset;
    refuses([&] { Anchor.payload(Args); });
  }
  const auto &Page = C.find("neverd_web_artifacts");
  for (const auto &Limit :
       {Json(0), Json(-1), Json(true), Json(513), Json(1.0)})
    refuses([&] { Page.payload({{"revision", "1"}, {"limit", Limit}}); });
  Page.payload({{"revision", "1"}, {"limit", 512}});
  refuses([&] {
    C.find("neverd_web_source_view_chunk")
        .payload({{"revision", "1"},
                  {"view_id", "v"},
                  {"byte_offset", "0"},
                  {"byte_limit", 65537}});
  });
  Args["byte_offset"] = "0";
  Args["source_id"] = std::string(63, 's') + "中";
  refuses([&] { Anchor.payload(Args); });
}
void discoveryPages() {
  Catalog C(
      capabilities({"import_commit", "metadata", "artifacts", "bun_extract",
                    "bun_records", "source_analyze", "source_nodes",
                    "source_map_analyze", "source_map_sources",
                    "source_map_segments", "source_map_lookup",
                    "source_view_preview", "source_view_commit",
                    "source_view_records", "source_view_chunk", "native_open",
                    "native_metadata", "sea_extract", "sea_records"}),
      0);
  Json Params = Json::object();
  Json Names = Json::array();
  unsigned Pages = 0;
  do {
    const auto Page = C.list(Params);
    require(Page.at("tools").size() <= 16, "Discovery page exceeded bound");
    for (const auto &Tool : Page.at("tools")) {
      Names.push_back(Tool.at("name"));
      const auto &Schema = Tool.at("inputSchema");
      require(Schema.at("type") == "object" &&
                  Schema.at("additionalProperties") == false &&
                  Schema.at("properties").at("schema_version").at("const") == 1,
              "Catalog omitted the closed argument contract");
      require(Tool.at("annotations").at("openWorldHint") == false,
              "Offline tool declared external access");
    }
    ++Pages;
    if (!Page.contains("nextCursor"))
      break;
    Params["cursor"] = Page.at("nextCursor");
    require(Pages < 8, "Discovery cursor did not advance");
  } while (true);
  require(Names == C.names() && Pages == 2,
          "Discovery lost or duplicated tools");
  for (const auto &Cursor : {"", "web-tools-v1:0", "web-tools-v1:016",
                             "web-tools-v1:1", "web-tools-v1:32", "CANARY"})
    refuses([&] { C.list({{"cursor", Cursor}}); });
}
} // namespace

int main() {
  try {
    capabilitySubset();
    closedArguments();
    coordinateBounds();
    discoveryPages();
    std::cout << "4 MCP catalog contracts passed\n";
    return 0;
  } catch (const std::exception &E) {
    std::cerr << E.what() << '\n';
    return 1;
  }
}
