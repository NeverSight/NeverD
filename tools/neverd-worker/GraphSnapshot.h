#pragma once
#include "Protocol.h"

#include <memory>

namespace neverd::worker {
// One immutable, validated CFG with deterministic geometry and viewport
// indexes. No engine objects or address-sized floating point values cross this
// boundary.
class GraphSnapshot {
public:
  GraphSnapshot(Json graph, std::string address, std::string revision);
  ~GraphSnapshot();
  GraphSnapshot(const GraphSnapshot &) = delete;
  GraphSnapshot &operator=(const GraphSnapshot &) = delete;
  Json summary() const;
  Json viewport(const Json &request) const;
  const std::string &address() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace neverd::worker
