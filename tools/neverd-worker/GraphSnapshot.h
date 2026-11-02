#pragma once
#include "Protocol.h"

#include <functional>
#include <memory>

namespace neverd::worker {

/// Client text metrics that size each node to its formatted content.
struct GraphMetrics {
  double charWidth = 0, lineHeight = 0, padding = 0, titleHeight = 0;
  bool valid() const { return charWidth > 0 && lineHeight > 0; }
  static GraphMetrics fromRequest(const Json &request);
  std::string key() const;
};
/// Formatted rows of the instructions in [start, end), as a JSON array of
/// {address, kind, text, spans}.
using GraphRows = std::function<Json(std::uint64_t start, std::uint64_t end)>;

// One immutable, validated CFG with deterministic geometry and viewport
// indexes. No engine objects or address-sized floating point values cross this
// boundary.
class GraphSnapshot {
public:
  GraphSnapshot(Json graph, std::string address, std::string revision,
                GraphMetrics metrics = {}, const GraphRows &rows = {});
  ~GraphSnapshot();
  GraphSnapshot(const GraphSnapshot &) = delete;
  GraphSnapshot &operator=(const GraphSnapshot &) = delete;
  Json summary() const;
  Json viewport(const Json &request) const;
  /// Geometry of a block by its stable start address, or null if absent.
  Json locate(const std::string &address) const;
  const std::string &address() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace neverd::worker
