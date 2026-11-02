//===- TablePage.h - Address selection in paged tables --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Locate an address in a table's complete filtered and sorted row order.
/// The producer retains ownership of that order and of each row's identity.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_WORKER_TABLE_PAGE_H
#define NEVERD_WORKER_TABLE_PAGE_H

#include "Protocol.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace neverd::worker {

struct TableRowRange {
  std::uint64_t start = 0, size = 1;

  bool contains(std::uint64_t address) const {
    return size && address >= start && address - start < size;
  }
};

struct TablePagePosition {
  struct Selection {
    std::size_t index;
    std::uint64_t address;
  };
  std::size_t offset = 0;
  std::optional<Selection> selected;

  void annotate(Json &page) const {
    if (selected) {
      page["selected_index"] = selected->index;
      page["selected_address"] = hexAddress(selected->address);
    }
  }
};

/// A page containing seek_address, or the requested ordinary offset.
/// AddressAt returns a row's optional range in the final table order.
template <class AddressAt>
TablePagePosition tablePagePosition(const Json &payload, std::size_t total,
                                    std::size_t limit, AddressAt addressAt) {
  const auto offset =
      sizeField(payload, "offset", 0, std::numeric_limits<std::size_t>::max());
  if (!payload.contains("seek_address"))
    return {offset, std::nullopt};
  if (!limit || payload.contains("from"))
    throw Error("invalid_request",
                "Address selection requires a positive limit and no cursor");
  const auto address = parseAddress(stringField(payload, "seek_address"));
  for (std::size_t index = 0; index < total; ++index)
    if (const auto row = addressAt(index); row && row->contains(address))
      return {index / limit * limit,
              TablePagePosition::Selection{index, row->start}};
  throw Error("not_found", "No table row is at " + hexAddress(address));
}

} // namespace neverd::worker

#endif // NEVERD_WORKER_TABLE_PAGE_H
