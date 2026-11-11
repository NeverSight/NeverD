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

/// What a patch wrote into a view's document, for a page of the document to
/// say what changed of it without the document it was being read beside it.
struct ViewChange {
  /// A span of the text it was that reads otherwise, and how many bytes read
  /// there now.
  struct Span {
    std::size_t begin, end, size;
    bool operator==(const Span &) const = default;
  };
  /// What was added to the begin_byte and the end_byte of so many consecutive
  /// elements of a list of spans; nothing to one an element has not.
  struct Moved {
    std::size_t count;
    std::ptrdiff_t begin, end;
    bool operator==(const Moved &) const = default;
  };
  /// In order, none over a line break.
  std::vector<Span> text;
  /// Of every code name and every source name, in the lists' order; none
  /// where no span of the list moved.
  std::vector<Moved> codeNames, sourceNames;
  /// The rows and the code names it wrote other values of, the library
  /// regions it moved a span of and the variables it named, ascending.
  std::vector<std::size_t> rows, names, regions, variables;
  /// What it wrote as the identity of every local, where it wrote one.
  std::optional<std::string> identity;
  /// It moved the prelude's end.
  bool prelude = false;
};

/// How a document reads against the one a patch made it of.
struct DocumentChange {
  /// That document's id, which its pages are named by.
  std::uint64_t from = 0;
  ViewChange wrote;
  /// The lines whose text the patch wrote, ascending, each with what the bytes
  /// after it moved by, in all (changedLines).
  std::vector<std::pair<std::size_t, std::ptrdiff_t>> lines;
  /// It stated a function's name otherwise.
  bool stated = false;
};

/// The lines \p wrote's spans lie on, in a text of \p bytesBefore bytes whose
/// lines began at \p startsBefore, each with what the bytes after it moved by
/// in all; \p textNow is the text as the patch left it.  Nothing where a span
/// is not within one line, or what reads there now is not.
std::optional<std::vector<std::pair<std::size_t, std::ptrdiff_t>>>
changedLines(const ViewChange &wrote,
             const std::vector<std::size_t> &startsBefore,
             std::size_t bytesBefore, std::string_view textNow);

/// What changed of \p page, the lines [start, end) of \p document, whose lines
/// begin at \p starts, against the same lines of the document a patch made it
/// of, as \p change says the patch wrote it: the operations that make the page
/// from that one.  Nothing where the change does not account for the page.
std::optional<Json> changedPage(const PageSource &page, const Json &document,
                                const std::vector<std::size_t> &starts,
                                const DocumentChange &change, std::size_t start,
                                std::size_t end);

} // namespace neverd::worker
