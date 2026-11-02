#include "GraphSnapshot.h"

#include "GraphLayout.h"
#include "ResourceLimits.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <queue>
#include <unordered_map>
#include <utility>
#include <vector>

namespace neverd::worker {
namespace {
constexpr std::size_t MaxNodes =
                          ::neverd::limits::worker::kGraphSnapshotMaxNodes,
                      MaxEdges =
                          ::neverd::limits::worker::kGraphSnapshotMaxEdges;
constexpr std::size_t NodePage =
                          ::neverd::limits::worker::kGraphSnapshotNodePage,
                      EdgePage =
                          ::neverd::limits::worker::kGraphSnapshotEdgePage;
constexpr double NodeWidth = 300, NodeHeight = 180;
constexpr double ScaleForText = 0.6;
struct Rect {
  double x = 0, y = 0, width = 0, height = 0;
  bool intersects(const Rect &r) const {
    return x <= r.x + r.width && r.x <= x + width && y <= r.y + r.height &&
           r.y <= y + height;
  }
  void include(const Rect &r) {
    const double right = std::max(x + width, r.x + r.width);
    const double bottom = std::max(y + height, r.y + r.height);
    x = std::min(x, r.x);
    y = std::min(y, r.y);
    width = right - x;
    height = bottom - y;
  }
  Json json() const {
    return {{"x", x}, {"y", y}, {"width", width}, {"height", height}};
  }
};
struct Point {
  double x, y;
};

// Median-split AABB tree: O(N) storage, including long back edges. A tiled
// index would duplicate long edges into an unbounded number of cells.
class Index {
  struct Branch {
    Rect box;
    std::size_t begin, end;
    int left = -1, right = -1;
  };
  std::vector<Branch> branches_;
  std::vector<std::size_t> order_;
  std::vector<Rect> boxes_;
  int build(std::size_t begin, std::size_t end) {
    Rect box = boxes_[order_[begin]];
    for (auto i = begin + 1; i < end; ++i)
      box.include(boxes_[order_[i]]);
    const auto current = static_cast<int>(branches_.size());
    branches_.push_back({box, begin, end});
    if (end - begin > 16) {
      const auto mid = begin + (end - begin) / 2;
      const bool horizontal = box.width >= box.height;
      std::nth_element(order_.begin() + begin, order_.begin() + mid,
                       order_.begin() + end, [&](auto a, auto b) {
                         const auto &ra = boxes_[a];
                         const auto &rb = boxes_[b];
                         const double ca = horizontal ? ra.x + ra.width / 2
                                                      : ra.y + ra.height / 2;
                         const double cb = horizontal ? rb.x + rb.width / 2
                                                      : rb.y + rb.height / 2;
                         return ca != cb ? ca < cb : a < b;
                       });
      const auto left = build(begin, mid), right = build(mid, end);
      branches_[current].left = left;
      branches_[current].right = right;
    }
    return current;
  }

public:
  void reset(std::vector<Rect> boxes) {
    boxes_ = std::move(boxes);
    order_.resize(boxes_.size());
    std::iota(order_.begin(), order_.end(), 0);
    branches_.clear();
    branches_.reserve(boxes_.size() / 4 + 1);
    if (!boxes_.empty())
      build(0, boxes_.size());
  }
  std::vector<std::size_t> query(const Rect &viewport) const {
    std::vector<std::size_t> result;
    if (branches_.empty())
      return result;
    std::vector<int> pending{0};
    while (!pending.empty()) {
      const auto &branch = branches_[pending.back()];
      pending.pop_back();
      if (!branch.box.intersects(viewport))
        continue;
      if (branch.left >= 0) {
        pending.push_back(branch.right);
        pending.push_back(branch.left);
      } else
        for (auto i = branch.begin; i < branch.end; ++i)
          if (boxes_[order_[i]].intersects(viewport))
            result.push_back(order_[i]);
    }
    std::sort(result.begin(),
              result.end()); // Stable independent node/edge cursors.
    return result;
  }
};
std::string id(const Json &value) {
  if (value.is_string()) {
    auto text = value.get<std::string>();
    if (text.empty() ||
        text.size() > ::neverd::limits::worker::kGraphSnapshotIdTextSizeLimit)
      throw Error("invalid_graph", "Invalid graph identifier");
    return text;
  }
  if (value.is_number_integer() || value.is_number_unsigned())
    return value.dump();
  throw Error("invalid_graph", "Graph identifiers must be strings or integers");
}
double number(const Json &p, const char *key, double fallback, double minimum,
              double maximum) {
  auto it = p.find(key);
  if (it == p.end())
    return fallback;
  if (!it->is_number())
    throw Error("invalid_request",
                std::string(key) + " must be a finite number");
  const double result = it->get<double>();
  if (!std::isfinite(result) || result < minimum || result > maximum)
    throw Error("invalid_request",
                std::string(key) + " is outside the viewport budget");
  return result;
}
// Exact intersection for orthogonal edge segments avoids publishing edges just
// because a large L-shaped route's bounding rectangle overlaps the viewport.
bool segmentVisible(Point a, Point b, const Rect &r) {
  return Rect{std::min(a.x, b.x), std::min(a.y, b.y), std::abs(a.x - b.x),
              std::abs(a.y - b.y)}
      .intersects(r);
}
} // namespace

GraphMetrics GraphMetrics::fromRequest(const Json &request) {
  GraphMetrics metrics;
  if (const auto it = request.find("metrics");
      it != request.end() && it->is_object()) {
    const auto number = [&](const char *field) {
      const auto value = it->find(field);
      if (value == it->end())
        return 0.0;
      if (!value->is_number() || !std::isfinite(value->get<double>()) ||
          value->get<double>() < 0 ||
          value->get<double>() > ::neverd::limits::worker::kMaxGraphMetric)
        throw Error("invalid_request",
                    std::string("metrics.") + field + " is out of range");
      return value->get<double>();
    };
    metrics.charWidth = number("char_width");
    metrics.lineHeight = number("line_height");
    metrics.padding = number("padding");
    metrics.titleHeight = number("title_height");
  }
  return metrics;
}

std::string GraphMetrics::key() const {
  return std::to_string(charWidth) + "/" + std::to_string(lineHeight) + "/" +
         std::to_string(padding) + "/" + std::to_string(titleHeight);
}

struct GraphSnapshot::Impl {
  struct Node {
    std::string id, address, end;
    Json lines, rows, comparison;
    std::size_t instructionCount = 0;
    bool linesTruncated = false;
    Rect box;
  };
  struct Edge {
    std::string id, type;
    std::size_t from, to;
    std::vector<Point> points;
    Rect box;
  };
  std::string address, revision;
  std::vector<Node> nodes;
  std::vector<Edge> edges;
  std::size_t totalEdges = 0, unresolvedEdges = 0;
  Rect bounds;
  Index nodeIndex, edgeIndex;

  void layout(const GraphMetrics &metrics) {
    LayoutInput input;
    input.nodes.reserve(nodes.size());
    for (const auto &node : nodes) {
      if (!metrics.valid()) {
        input.nodes.push_back({NodeWidth, NodeHeight});
        continue;
      }
      std::size_t columns = node.address.size();
      for (const auto &row : node.rows)
        columns = std::max(columns, row.value("text", std::string()).size());
      if (node.rows.empty())
        for (const auto &line : node.lines)
          columns =
              std::max(columns, line.get_ref<const std::string &>().size());
      const double width =
          std::min(::neverd::limits::worker::kGraphSnapshotMaxNodeExtent,
                   static_cast<double>(std::max<std::size_t>(columns, 8)) *
                           metrics.charWidth +
                       2 * metrics.padding);
      const double height = std::min(
          ::neverd::limits::worker::kGraphSnapshotMaxNodeExtent,
          static_cast<double>(std::max<std::size_t>(
              node.rows.empty() ? node.lines.size() : node.rows.size(), 1)) *
                  metrics.lineHeight +
              2 * metrics.padding + metrics.titleHeight);
      input.nodes.push_back({width, height});
    }
    for (const auto &edge : edges)
      input.edges.push_back({edge.from, edge.to});
    // The entry block is the node at the requested function address.
    for (std::size_t i = 0; i < nodes.size(); ++i)
      if (nodes[i].address == address) {
        input.entry = i;
        break;
      }
    const auto placed = layoutGraph(input);
    std::vector<Rect> nodeBoxes, edgeBoxes;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      nodes[i].box = {placed.positions[i].x, placed.positions[i].y,
                      input.nodes[i].width, input.nodes[i].height};
      nodeBoxes.push_back(nodes[i].box);
      bounds.include(nodes[i].box);
    }
    for (std::size_t i = 0; i < edges.size(); ++i) {
      auto &edge = edges[i];
      edge.points.clear();
      for (const auto &point : placed.routes[i])
        edge.points.push_back({point.x, point.y});
      if (edge.points.empty()) {
        const auto &a = nodes[edge.from].box, &b = nodes[edge.to].box;
        edge.points = {{a.x + a.width / 2, a.y + a.height},
                       {b.x + b.width / 2, b.y}};
      }
      edge.box = {edge.points.front().x, edge.points.front().y, 0, 0};
      for (auto point : edge.points)
        edge.box.include({point.x, point.y, 0, 0});
      edgeBoxes.push_back(edge.box);
      bounds.include(edge.box);
    }
    bounds.width += 40;
    bounds.height += 40;
    nodeIndex.reset(std::move(nodeBoxes));
    edgeIndex.reset(std::move(edgeBoxes));
  }
};

GraphSnapshot::GraphSnapshot(Json graph, std::string address,
                             std::string revision, GraphMetrics metrics,
                             const GraphRows &rows)
    : impl_(std::make_unique<Impl>()) {
  auto &out = *impl_;
  out.address = std::move(address);
  out.revision = std::move(revision);
  if (!graph.is_object() || !graph.contains("nodes") ||
      !graph["nodes"].is_array() || !graph.contains("edges") ||
      !graph["edges"].is_array())
    throw Error("invalid_graph", "Engine graph is missing node or edge arrays");
  if (graph["nodes"].size() > MaxNodes || graph["edges"].size() > MaxEdges)
    throw Error("budget_exceeded", "Graph exceeds 20000 nodes / 100000 edges");
  for (const auto &source : graph["nodes"]) {
    Impl::Node node;
    node.id = id(source.at("id"));
    node.address = hexAddress(parseAddress(stringField(
        source, "start", {},
        ::neverd::limits::worker::
            kGraphSnapshotGraphSnapshotNodeAddressStartStringFieldMax)));
    node.end = hexAddress(parseAddress(
        stringField(source, "end", node.address,
                    ::neverd::limits::worker::
                        kGraphSnapshotGraphSnapshotNodeEndEndStringFieldMax)));
    node.instructionCount = sizeField(source, "insn_count", 0,
                                      std::numeric_limits<std::size_t>::max());
    node.lines = Json::array();
    if (auto it = source.find("disasm"); it != source.end()) {
      if (!it->is_array())
        throw Error("invalid_graph", "Node disassembly must be an array");
      std::size_t bytes = 0;
      for (const auto &line : *it) {
        if (!line.is_string())
          throw Error("invalid_graph",
                      "Node disassembly lines must be strings");
        const auto &text = line.get_ref<const std::string &>();
        if (node.lines.size() == ::neverd::limits::worker::kMaxGraphNodeLines ||
            bytes + text.size() >
                ::neverd::limits::worker::
                    kGraphSnapshotGraphSnapshotBytesTextSizeLimit) {
          node.linesTruncated = true;
          break;
        }
        node.lines.push_back(text);
        bytes += text.size();
      }
    }
    node.rows = rows ? rows(parseAddress(node.address), parseAddress(node.end))
                     : Json::array();
    node.comparison = source.value("comparison", Json::object());
    if (!node.comparison.is_object())
      throw Error("invalid_graph", "Invalid comparison block evidence");
    out.nodes.push_back(std::move(node));
  }
  // Stable layout does not depend on backend container iteration order.
  std::sort(
      out.nodes.begin(), out.nodes.end(), [](const auto &a, const auto &b) {
        const auto aa = parseAddress(a.address), ba = parseAddress(b.address);
        return aa != ba ? aa < ba : a.id < b.id;
      });
  std::unordered_map<std::string, std::size_t> byId;
  for (std::size_t i = 0; i < out.nodes.size(); ++i)
    if (!byId.emplace(out.nodes[i].id, i).second)
      throw Error("invalid_graph", "Duplicate graph node identifier");
  out.totalEdges = graph["edges"].size();
  for (const auto &source : graph["edges"]) {
    const auto from = id(source.at("from"));
    const auto to =
        source.at("to").is_null() ? std::string() : id(source.at("to"));
    const auto f = byId.find(from), t = byId.find(to);
    if (f == byId.end() || t == byId.end()) {
      ++out.unresolvedEdges;
      continue;
    }
    out.edges.push_back(
        {{},
         stringField(source, "type", "unconditional",
                     ::neverd::limits::worker::
                         kGraphSnapshotGraphSnapshotTypeStringFieldMax),
         f->second,
         t->second,
         {},
         {}});
  }
  std::sort(
      out.edges.begin(), out.edges.end(), [](const auto &a, const auto &b) {
        return std::tie(a.from, a.to, a.type) < std::tie(b.from, b.to, b.type);
      });
  for (std::size_t i = 0; i < out.edges.size(); ++i)
    out.edges[i].id = "edge:" + std::to_string(i);
  out.layout(metrics);
}
GraphSnapshot::~GraphSnapshot() = default;
const std::string &GraphSnapshot::address() const { return impl_->address; }
Json GraphSnapshot::summary() const {
  const auto &g = *impl_;
  return {{"address", g.address},
          {"layout_revision", g.revision},
          {"bounds", g.bounds.json()},
          {"node_count", g.nodes.size()},
          {"edge_count", g.totalEdges},
          {"resolved_edge_count", g.edges.size()},
          {"unresolved_edge_count", g.unresolvedEdges},
          {"node_limit", NodePage},
          {"edge_limit", EdgePage},
          {"layout", "layered-v2"},
          {"complete", true},
          {"snapshot_complete", true}};
}
Json GraphSnapshot::locate(const std::string &address) const {
  for (const auto &node : impl_->nodes) {
    if (node.address != address)
      continue;
    auto out = node.box.json();
    out["address"] = address;
    return out;
  }
  return nullptr;
}
Json GraphSnapshot::viewport(const Json &request) const {
  const auto &g = *impl_;
  if (stringField(request, "layout_revision", {},
                  ::neverd::limits::worker::
                      kGraphSnapshotViewportLayoutRevisionStringFieldMax) !=
      g.revision)
    throw Error("stale_layout",
                "Graph layout changed; request cfg_summary again");
  for (const auto *field : {"x", "y", "width", "height"})
    if (!request.contains(field))
      throw Error("invalid_request", std::string("Viewport requires ") + field);
  Rect rect{number(request, "x", 0, -1e9, 1e9),
            number(request, "y", 0, -1e9, 1e9),
            number(request, "width", 0, 0.001, 1e9),
            number(request, "height", 0, 0.001, 1e9)};
  const double scale = number(request, "scale", 1, 0.000001, 1000);
  const auto nodeOffset = sizeField(request, "node_offset", 0, MaxNodes);
  const auto edgeOffset = sizeField(request, "edge_offset", 0, MaxEdges);
  auto nodes = g.nodeIndex.query(rect), edges = g.edgeIndex.query(rect);
  std::erase_if(edges, [&](auto index) {
    const auto &points = g.edges[index].points;
    for (std::size_t i = 1; i < points.size(); ++i)
      if (segmentVisible(points[i - 1], points[i], rect))
        return false;
    return true;
  });
  const auto nodeEnd =
      std::min(nodes.size(), std::min(nodeOffset, nodes.size()) + NodePage);
  const auto edgeEnd =
      std::min(edges.size(), std::min(edgeOffset, edges.size()) + EdgePage);
  Json visibleNodes = Json::array(), visibleEdges = Json::array();
  for (auto i = nodeOffset; i < nodeEnd; ++i) {
    const auto &node = g.nodes[nodes[i]];
    auto value = node.box.json();
    value.update({{"id", node.id},
                  {"address", node.address},
                  {"start", node.address},
                  {"end", node.end},
                  {"label", node.address},
                  {"lines", scale >= ScaleForText ? node.lines : Json::array()},
                  {"rows", scale >= ScaleForText ? node.rows : Json::array()},
                  {"lines_truncated", node.linesTruncated},
                  {"insn_count", node.instructionCount}});
    if (!node.comparison.empty())
      value["comparison"] = node.comparison;
    visibleNodes.push_back(std::move(value));
  }
  for (auto i = edgeOffset; i < edgeEnd; ++i) {
    const auto &edge = g.edges[edges[i]];
    Json points = Json::array();
    for (auto point : edge.points)
      points.push_back({{"x", point.x}, {"y", point.y}});
    visibleEdges.push_back({{"id", edge.id},
                            {"from", g.nodes[edge.from].id},
                            {"to", g.nodes[edge.to].id},
                            {"type", edge.type},
                            {"points", std::move(points)}});
  }
  auto result = summary();
  result.update(
      {{"nodes", std::move(visibleNodes)},
       {"edges", std::move(visibleEdges)},
       {"viewport", rect.json()},
       {"scale", scale},
       {"visible_node_count", nodes.size()},
       {"visible_edge_count", edges.size()},
       {"node_offset", nodeOffset},
       {"edge_offset", edgeOffset},
       {"next_node_offset",
        nodeEnd < nodes.size() ? Json(nodeEnd) : Json(nullptr)},
       {"next_edge_offset",
        edgeEnd < edges.size() ? Json(edgeEnd) : Json(nullptr)},
       {"nodes_truncated", nodeOffset > 0 || nodeEnd < nodes.size()},
       {"edges_truncated", edgeOffset > 0 || edgeEnd < edges.size()},
       {"complete", nodeEnd == nodes.size() && edgeEnd == edges.size()}});
  return result;
}
} // namespace neverd::worker
