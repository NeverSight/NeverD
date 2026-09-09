#include "GraphSnapshot.h"

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
constexpr std::size_t MaxNodes = 20000, MaxEdges = 100000;
constexpr std::size_t NodePage = 256, EdgePage = 512;
constexpr double NodeWidth = 300, NodeHeight = 180;
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
    if (text.empty() || text.size() > 128)
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

struct GraphSnapshot::Impl {
  struct Node {
    std::string id, address, end;
    Json lines;
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

  void layout() {
    const auto n = nodes.size();
    std::vector<std::vector<std::size_t>> forward(n), reverse(n);
    for (const auto &edge : edges) {
      forward[edge.from].push_back(edge.to);
      reverse[edge.to].push_back(edge.from);
    }
    // Iterative Kosaraju; a 20k block linear function cannot overflow the
    // stack.
    std::vector<bool> visited(n, false);
    std::vector<std::size_t> finished;
    for (std::size_t root = 0; root < n; ++root)
      if (!visited[root]) {
        visited[root] = true;
        std::vector<std::pair<std::size_t, std::size_t>> stack{{root, 0}};
        while (!stack.empty()) {
          auto &[v, next] = stack.back();
          if (next < forward[v].size()) {
            const auto child = forward[v][next++];
            if (!visited[child]) {
              visited[child] = true;
              stack.emplace_back(child, 0);
            }
          } else {
            finished.push_back(v);
            stack.pop_back();
          }
        }
      }
    std::vector<std::size_t> component(n, n);
    std::size_t components = 0;
    for (auto it = finished.rbegin(); it != finished.rend(); ++it)
      if (component[*it] == n) {
        component[*it] = components;
        std::vector<std::size_t> stack{*it};
        while (!stack.empty()) {
          const auto v = stack.back();
          stack.pop_back();
          for (const auto child : reverse[v])
            if (component[child] == n) {
              component[child] = components;
              stack.push_back(child);
            }
        }
        ++components;
      }
    std::vector<std::vector<std::size_t>> dag(components);
    for (const auto &edge : edges)
      if (component[edge.from] != component[edge.to])
        dag[component[edge.from]].push_back(component[edge.to]);
    std::vector<std::size_t> incoming(components, 0), rank(components, 0);
    for (auto &successors : dag) {
      std::sort(successors.begin(), successors.end());
      successors.erase(std::unique(successors.begin(), successors.end()),
                       successors.end());
      for (const auto next : successors)
        ++incoming[next];
    }
    std::priority_queue<std::size_t, std::vector<std::size_t>, std::greater<>>
        ready;
    for (std::size_t i = 0; i < components; ++i)
      if (!incoming[i])
        ready.push(i);
    while (!ready.empty()) {
      const auto current = ready.top();
      ready.pop();
      for (const auto next : dag[current]) {
        rank[next] = std::max(rank[next], rank[current] + 1);
        if (!--incoming[next])
          ready.push(next);
      }
    }
    std::vector<std::vector<std::size_t>> layers(n);
    for (std::size_t i = 0; i < n; ++i)
      layers[rank[component[i]]].push_back(i);
    double y = 40;
    for (const auto &layer : layers)
      if (!layer.empty()) {
        for (std::size_t i = 0; i < layer.size(); ++i)
          nodes[layer[i]].box = {40 + (i % 8) * 380.0, y + (i / 8) * 250.0,
                                 NodeWidth, NodeHeight};
        y += ((layer.size() + 7) / 8) * 250.0;
      }
    std::vector<Rect> nodeBoxes, edgeBoxes;
    for (const auto &node : nodes) {
      nodeBoxes.push_back(node.box);
      bounds.include(node.box);
    }
    for (auto &edge : edges) {
      const auto &a = nodes[edge.from].box, &b = nodes[edge.to].box;
      if (b.y > a.y + a.height) {
        const double mid = (a.y + a.height + b.y) / 2;
        edge.points = {{a.x + a.width / 2, a.y + a.height},
                       {a.x + a.width / 2, mid},
                       {b.x + b.width / 2, mid},
                       {b.x + b.width / 2, b.y}};
      } else {
        const double lane =
            std::max(a.x + a.width, b.x + b.width) + 20 + (edge.from % 4) * 8;
        edge.points = {{a.x + a.width, a.y + a.height / 2},
                       {lane, a.y + a.height / 2},
                       {lane, b.y + 30},
                       {b.x + b.width, b.y + 30}};
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
                             std::string revision)
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
    node.address =
        hexAddress(parseAddress(stringField(source, "start", {}, 32)));
    node.end =
        hexAddress(parseAddress(stringField(source, "end", node.address, 32)));
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
        if (node.lines.size() == 6 || bytes + text.size() > 2048) {
          node.linesTruncated = true;
          break;
        }
        node.lines.push_back(text);
        bytes += text.size();
      }
    }
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
    out.edges.push_back({{},
                         stringField(source, "type", "unconditional", 64),
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
  out.layout();
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
          {"layout", "scc-layered-v1"},
          {"complete", true},
          {"snapshot_complete", true}};
}
Json GraphSnapshot::viewport(const Json &request) const {
  const auto &g = *impl_;
  if (stringField(request, "layout_revision", {}, 256) != g.revision)
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
                  {"lines", scale >= 0.6 ? node.lines : Json::array()},
                  {"lines_truncated", node.linesTruncated},
                  {"insn_count", node.instructionCount}});
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
