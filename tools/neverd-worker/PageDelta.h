//===- PageDelta.h - What changed between two pages of a view -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The operations that make a page of a view from the page a client keeps:
/// fields set and removed, lines of a text replaced, and runs of a list kept,
/// shifted, replaced, dropped or added.  An edit that changes a few lines of a
/// view, and moves the byte offsets of what follows them, changes a page by a
/// few operations, which a client applies instead of reading the page whole.
/// PROTOCOL.md "page_delta" is the wire form.
///
//===----------------------------------------------------------------------===//

#pragma once
#include "Protocol.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neverd::worker {

/// A page read in place from what it is made of: each field a value held
/// elsewhere, the items of a list the page holds, a span of a text, or a value
/// of its own.  A field set again replaces the one set before, and the fields
/// stay in the order of their names, as a page object holds them.  What a field
/// reads must outlive the page.
class PageSource {
public:
  struct Field {
    enum class Kind : std::uint8_t { Value, Items, Text, Own };
    std::string name;
    Kind kind = Kind::Value;
    const Json *value = nullptr;
    std::vector<const Json *> items;
    std::string_view text;
    Json own;
  };

  /// The fields of the page object \p page, read in place.
  static PageSource of(const Json &page);

  void value(std::string name, const Json &value);
  void items(std::string name, std::vector<const Json *> items);
  void text(std::string name, std::string_view text);
  void own(std::string name, Json value);

  const std::vector<Field> &fields() const { return Fields; }
  const Field *find(std::string_view name) const;
  /// The page object.
  Json json() const;

private:
  Field &field(std::string name);
  std::vector<Field> Fields;
};

/// The operations that make \p after from \p before, both page objects, or
/// nothing where they are not objects.
std::optional<Json> pageDelta(const Json &before, const Json &after);
/// The same of pages read in place: the operations pageDelta gives their page
/// objects.
Json pageDelta(const PageSource &before, const PageSource &after);

/// \p page with \p delta applied, or nothing where an operation does not fit
/// it.
std::optional<Json> applyPageDelta(Json page, const Json &delta);

} // namespace neverd::worker
