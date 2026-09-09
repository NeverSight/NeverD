#include "GraphSnapshot.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <set>
#include <stdexcept>
using namespace neverd::worker;
namespace {
void check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <class F> void rejects(F run, const char *code) {
  try {
    run();
  } catch (const Error &e) {
    check(e.code == code, "Wrong error code");
    return;
  }
  throw std::runtime_error("Expected rejection");
}
Json graph(std::size_t count) {
  Json nodes = Json::array(), edges = Json::array();
  constexpr std::uint64_t base = 0xffff000000000000ULL;
  for (std::size_t i = 0; i < count; ++i) {
    nodes.push_back({{"id", i},
                     {"start", hexAddress(base + i * 16)},
                     {"end", hexAddress(base + i * 16 + 8)},
                     {"insn_count", 1},
                     {"disasm", {"exact instruction"}}});
    if (i)
      edges.push_back({{"from", i - 1}, {"to", i}, {"type", "true"}});
    // Cyclic ten-block regions exercise SCCs and back-edge routing.
    if (i % 10 == 9)
      edges.push_back({{"from", i}, {"to", i - 9}, {"type", "false"}});
  }
  edges.push_back({{"from", 0}, {"to", nullptr}, {"type", "indirect"}});
  return {{"nodes", nodes}, {"edges", edges}};
}
Json viewport(const Json &summary) {
  auto p = summary.at("bounds");
  p["layout_revision"] = summary.at("layout_revision");
  p["scale"] = 1;
  return p;
}
} // namespace
int main() {
  try {
    const auto begin = std::chrono::steady_clock::now();
    auto input = graph(10000);
    GraphSnapshot snapshot(input, "0xffff000000000000", "revision-1");
    const auto summary = snapshot.summary();
    check(summary.at("node_count") == 10000, "Large graph missing nodes");
    check(summary.at("unresolved_edge_count") == 1,
          "Unresolved edge not reported");
    auto p = viewport(summary);
    auto page = snapshot.viewport(p);
    check(page.at("nodes").size() == 256 && page.at("edges").size() == 512,
          "Viewport page budget not enforced");
    check(!page.at("complete") && page.at("snapshot_complete"),
          "Partial viewport claims complete");
    check(page.at("nodes")[0].at("address") == "0xffff000000000000",
          "High address was rounded");
    check(frame(page).size() < 1024 * 1024,
          "Viewport response exceeds expected wire budget");
    std::set<std::string> allNodes, allEdges;
    for (;;) {
      for (const auto &node : page.at("nodes"))
        check(allNodes.insert(node.at("id")).second,
              "Duplicate node across pages");
      for (const auto &edge : page.at("edges"))
        check(allEdges.insert(edge.at("id")).second,
              "Duplicate edge across pages");
      if (page.at("complete"))
        break;
      p["node_offset"] = page.at("next_node_offset").is_null()
                             ? summary.at("node_count")
                             : page.at("next_node_offset");
      p["edge_offset"] = page.at("next_edge_offset").is_null()
                             ? summary.at("edge_count")
                             : page.at("next_edge_offset");
      page = snapshot.viewport(p);
    }
    check(allNodes.size() == 10000 && allEdges.size() == 10999,
          "Paging loses graph objects");
    p = {{"layout_revision", "revision-1"},
         {"x", 0},
         {"y", 0},
         {"width", 1100},
         {"height", 700},
         {"scale", 1}};
    const auto local = snapshot.viewport(p);
    check(local.at("nodes").size() < 20,
          "Viewport materializes offscreen nodes");
    check(local.at("visible_node_count") == local.at("nodes").size(),
          "Visible count mismatch");
    p["x"] = 1e8;
    check(snapshot.viewport(p).at("nodes").empty(),
          "Outside viewport contains nodes");
    p["x"] = 0;
    p["scale"] = 0.1;
    check(snapshot.viewport(p).at("nodes")[0].at("lines").empty(),
          "Low zoom transfers instruction text");
    p["layout_revision"] = "old";
    rejects([&] { snapshot.viewport(p); }, "stale_layout");
    p["layout_revision"] = "revision-1";
    p["width"] = -1;
    rejects([&] { snapshot.viewport(p); }, "invalid_request");
    p["width"] = "100";
    rejects([&] { snapshot.viewport(p); }, "invalid_request");
    // Backend iteration order must not change the geometry or identifiers.
    std::reverse(input["nodes"].begin(), input["nodes"].end());
    std::reverse(input["edges"].begin(), input["edges"].end());
    GraphSnapshot reordered(std::move(input), "0xffff000000000000",
                            "revision-1");
    p = {{"layout_revision", "revision-1"},
         {"x", 0},
         {"y", 0},
         {"width", 1100},
         {"height", 700},
         {"scale", 1}};
    check(reordered.viewport(p) == local,
          "Layout changes with backend iteration order");
    auto malformed = graph(2);
    malformed["nodes"][1]["id"] = 0;
    rejects([&] { GraphSnapshot bad(malformed, "0x0", "r"); }, "invalid_graph");
    malformed = graph(2);
    malformed["nodes"][0]["id"] = 0.5;
    rejects([&] { GraphSnapshot bad(malformed, "0x0", "r"); }, "invalid_graph");
    rejects([&] { GraphSnapshot tooBig(graph(20001), "0x0", "r"); },
            "budget_exceeded");
    GraphSnapshot empty({{"nodes", Json::array()}, {"edges", Json::array()}},
                        "0x0", "r");
    check(empty.viewport(viewport(empty.summary())).at("complete"),
          "Empty graph invalid");
    const auto elapsed = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - begin)
                             .count();
    std::cout << "10k CFG layout, paging, culling, cycles, high addresses and "
                 "validation passed in "
              << elapsed << " ms\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
