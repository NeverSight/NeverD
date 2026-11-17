//===- VirtualReferences.h - Typed virtual-call references ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Maintain typed virtual-call references and their owning functions.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_WORKER_VIRTUALREFERENCES_H
#define NEVERD_TOOLS_WORKER_VIRTUALREFERENCES_H

#include "Protocol.h"

#include <map>
#include <tuple>
#include <vector>

namespace neverd::worker {

/// Typed virtual-call evidence. Native scans and decode-cache eviction do
/// not own these references; replacing a function or its types does.
class VirtualReferences {
  using Key = std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>;
  using Rows = std::map<Key, Json>;
  Rows incoming_;
  std::map<Key, Rows::const_iterator> outgoing_;
  std::map<std::uint64_t, std::vector<Key>> owners_;

public:
  VirtualReferences() = default;
  VirtualReferences(const VirtualReferences &) = delete;
  VirtualReferences &operator=(const VirtualReferences &) = delete;
  bool contains(std::uint64_t owner) const { return owners_.contains(owner); }
  void clear() {
    outgoing_.clear();
    incoming_.clear();
    owners_.clear();
  }

  /// Validate the engine/cache boundary before replacing this owner's facts.
  bool replace(std::uint64_t owner, const Json &calls) {
    if (!calls.is_array())
      throw Error("invalid_response", "virtual_calls must be an array");
    Rows replacement;
    for (const auto &call : calls) {
      if (!call.is_object() || call.value("resolution", "") != "static_type")
        throw Error("invalid_response", "Invalid virtual-call evidence");
      const auto from = parseAddress(call.at("from").get<std::string>());
      const auto target = parseAddress(call.at("to").get<std::string>());
      const auto slot = parseAddress(call.at("slot").get<std::string>());
      (void)parseAddress(call.at("table").get<std::string>());
      (void)parseAddress(call.at("slot_offset").get<std::string>());
      (void)call.at("symbol").get_ref<const std::string &>();
      (void)call.at("receiver_type").get_ref<const std::string &>();
      for (bool method : {true, false}) {
        const auto to = method ? target : slot;
        Json row = call;
        row["to"] = hexAddress(to);
        row["declared_target"] = hexAddress(target);
        row["kind"] = method ? "vcall" : "vslot";
        row["type"] = method ? "v" : "o";
        row["dispatch"] = "virtual";
        row["caller_function"] = hexAddress(owner);
        replacement.insert_or_assign(Key{to, from, owner}, std::move(row));
      }
    }
    const auto previous = owners_.find(owner);
    if (previous == owners_.end() && replacement.empty())
      return false;
    bool same = previous != owners_.end() &&
                previous->second.size() == replacement.size();
    if (same)
      for (const auto &[key, row] : replacement) {
        const auto old = incoming_.find(key);
        same &= old != incoming_.end() && old->second == row;
      }
    if (same)
      return false;
    if (previous != owners_.end())
      for (const auto &key : previous->second) {
        const auto &[to, from, function] = key;
        outgoing_.erase({from, to, function});
        incoming_.erase(key);
      }
    auto &keys = owners_[owner];
    keys.clear();
    for (auto &[key, row] : replacement) {
      const auto stored = incoming_.emplace(key, std::move(row)).first;
      const auto &[to, from, function] = key;
      outgoing_.emplace(Key{from, to, function}, stored);
      keys.push_back(key);
    }
    if (keys.empty())
      owners_.erase(owner);
    return true;
  }

  Json page(std::uint64_t address, bool to, std::size_t offset,
            std::size_t limit, std::size_t &total) const {
    Json result = Json::array();
    const auto collect = [&](const auto &index, auto read) {
      total = 0;
      for (auto it = index.lower_bound(Key{address, 0, 0});
           it != index.end() && std::get<0>(it->first) == address; ++it) {
        if (total >= offset && result.size() < limit)
          result.push_back(read(it));
        ++total;
      }
    };
    if (to)
      collect(incoming_, [](auto it) -> const Json & { return it->second; });
    else
      collect(outgoing_,
              [](auto it) -> const Json & { return it->second->second; });
    return result;
  }
};
} // namespace neverd::worker

#endif // NEVERD_TOOLS_WORKER_VIRTUALREFERENCES_H
