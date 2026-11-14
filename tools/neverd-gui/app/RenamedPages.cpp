//===- RenamedPages.cpp - A source text's pages under a new name ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Reads the pages of a source text as the worker writes a function's new name
/// into the text they are of.
//===----------------------------------------------------------------------===//

#include "RenamedPages.h"

#include <QJsonArray>
#include <algorithm>
#include <utility>
#include <vector>

namespace neverd::gui {
namespace {

// As the worker's "C" locale reads a byte: one beyond ASCII is neither.
bool isLetter(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
bool isWordByte(char c) {
  return isLetter(c) || (c >= '0' && c <= '9') || c == '_';
}

/// Whether \p text may hold \p name as a word of its own, which a worker asks
/// before it writes a name into a text: a word that linked nothing may link
/// the name's address once the name is that word (mayHoldToken, CodeEdits.cpp).
bool mayHoldWord(QByteArrayView text, QByteArrayView name) {
  if (name.size() == 1)
    return text.contains(name[0]);
  if (name.isEmpty() || !(isLetter(name[0]) || name[0] == '_') ||
      !std::all_of(name.begin(), name.end(), isWordByte))
    return false;
  for (qsizetype at = text.indexOf(name); at >= 0;
       at = text.indexOf(name, at + 1)) {
    const bool continues =
        at > 0 && (isLetter(text[at - 1]) || text[at - 1] == '_');
    const qsizetype end = at + name.size();
    if (!continues && (end == text.size() || !isWordByte(text[end])))
      return true;
  }
  return false;
}

/// Whether \p name is one the engine gives by itself, a prefix and the
/// address in hexadecimal such as `sub_1234` or `qword_3FB8`, which no worker
/// takes as a user's name (parseDummyName, OperandFormat.cpp; the prefixes
/// are the engine's own lists).
bool isAutomaticName(QByteArrayView name) {
  static constexpr QByteArrayView prefixes[] = {
#define NEVERD_AUTOMATIC_NAME_PREFIX(Prefix) QByteArrayView(Prefix),
#include "neverd/AutomaticNames.def"
#define NEVERD_DATA_SIZE_NAME(SizeKeyword, Bytes, Prefix)                      \
  QByteArrayView(Prefix),
#define NEVERD_DATA_ITEM_NAME(Id, Prefix) QByteArrayView(Prefix),
#include "neverd/DataNames.def"
  };
  for (const QByteArrayView prefix : prefixes) {
    if (!name.startsWith(prefix) || name.size() == prefix.size() ||
        name.size() - prefix.size() > 16)
      continue;
    const QByteArrayView digits = name.sliced(prefix.size());
    if (std::all_of(digits.begin(), digits.end(), [](char c) {
          return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                 (c >= 'A' && c <= 'F');
        }))
      return true;
  }
  return false;
}

/// Where the bytes of a text stand once names were written over some: each
/// place a name ended, and what the names up to it added in all.
class Moved {
public:
  /// A name of \p length bytes was written over the \p was bytes that end
  /// at \p end, after every place said before it.
  void wrote(qint64 end, qint64 was, qint64 length) {
    added_ += length - was;
    ends_.emplace_back(end, added_);
  }
  /// A byte at or after the end of a place moves with the names before it;
  /// the beginning of a place stays under the name written there.
  qint64 operator()(qint64 at) const {
    const auto after = std::upper_bound(
        ends_.begin(), ends_.end(), at,
        [](qint64 byte, const auto &end) { return byte < end.first; });
    return after == ends_.begin() ? at : at + std::prev(after)->second;
  }

private:
  std::vector<std::pair<qint64, qint64>> ends_;
  qint64 added_ = 0;
};

/// \p span with its begin_byte and its end_byte where \p moved puts them,
/// whichever of the two it has.
QJsonValue movedSpan(const QJsonValue &span, const Moved &moved) {
  if (!span.isObject())
    return span;
  QJsonObject object = span.toObject();
  for (const auto field :
       {QLatin1String("begin_byte"), QLatin1String("end_byte")})
    if (const auto held = object.constFind(field);
        held != object.constEnd() && held->isDouble())
      if (const qint64 was = held->toInteger(), now = moved(was); now != was)
        object.insert(field, now);
  return object;
}

/// A list of spans, each where \p moved puts it.
QJsonArray movedSpans(const QJsonArray &spans, const Moved &moved) {
  QJsonArray result;
  for (const auto &span : spans)
    result.append(movedSpan(span, moved));
  return result;
}

} // namespace

std::optional<PagesRenaming>
PagesRenaming::read(const QVector<QJsonObject> &pages, Address address,
                    const QString &name) {
  // A worker takes no name that is empty or holds a space or a control
  // character, nor one of the engine's own.
  const QByteArray spelled = name.toUtf8();
  if (spelled.isEmpty() || pages.isEmpty() ||
      std::any_of(spelled.begin(), spelled.end(),
                  [](char c) {
                    return static_cast<unsigned char>(c) <= ' ' || c == 0x7f;
                  }) ||
      isAutomaticName(spelled))
    return std::nullopt;
  PagesRenaming renaming;
  renaming.texts_.reserve(size_t(pages.size()));
  for (const QJsonObject &page : pages) {
    const QJsonValue text = page.value(QLatin1String("text"));
    const QJsonValue names = page.value(QLatin1String("code_names"));
    const qint64 offset =
        page.value(QLatin1String("byte_offset")).toInteger(-1);
    if (!text.isString() || !names.isArray() || offset < 0)
      return std::nullopt;
    // The pages are one text: each begins where the one before it ended.
    if (!renaming.texts_.empty() &&
        offset != renaming.offsets_.back() + renaming.texts_.back().size())
      return std::nullopt;
    const PageBytes &bytes = renaming.texts_.emplace_back(text.toString());
    renaming.offsets_.append(offset);
    if (mayHoldWord(bytes.bytes(), spelled))
      return std::nullopt;
    for (const auto &value : names.toArray()) {
      const QJsonObject linked = value.toObject();
      if (linked.value(QLatin1String("kind")).toString() !=
              QLatin1String("address") ||
          addressValue(linked.value(QLatin1String("address"))) != address)
        continue;
      const qint64 begin =
          linked.value(QLatin1String("begin_byte")).toInteger(-1);
      const qint64 end = linked.value(QLatin1String("end_byte")).toInteger(-1);
      if (begin < offset || end < begin || end > offset + bytes.size())
        return std::nullopt;
      if (bytes.bytes().sliced(begin - offset, end - begin) == spelled)
        continue;
      renaming.places_.append({begin, end});
      renaming.data_ =
          renaming.data_ || linked.value(QLatin1String("object")).toBool();
    }
  }
  if (renaming.places_.isEmpty())
    return std::nullopt;
  // In the text's order, and none over another.
  std::sort(renaming.places_.begin(), renaming.places_.end(),
            [](const Place &a, const Place &b) { return a.begin < b.begin; });
  for (qsizetype i = 1; i < renaming.places_.size(); ++i)
    if (renaming.places_[i].begin < renaming.places_[i - 1].end)
      return std::nullopt;
  renaming.pages_ = pages;
  renaming.address_ = address;
  renaming.name_ = name;
  renaming.nameBytes_ = spelled.size();
  return renaming;
}

QVector<QJsonObject> PagesRenaming::pages() const {
  Moved moved;
  for (const Place &place : places_)
    moved.wrote(place.end, place.end - place.begin, nameBytes_);
  // What a page states of a function, the one it is of or one a library
  // operation calls: the user's name for it, where it is the one renamed
  // (currentNamesInView in the worker).
  const auto stated = [this](const QJsonValue &identity) -> QJsonValue {
    if (!identity.isObject() || addressValue(identity.toObject().value(
                                    QLatin1String("addr"))) != address_)
      return identity;
    QJsonObject named = identity.toObject();
    for (const auto field :
         {QLatin1String("name"), QLatin1String("display_name")})
      named.insert(field, name_);
    for (const auto field :
         {QLatin1String("name_origin"), QLatin1String("display_origin")})
      named.insert(field, QLatin1String("user"));
    return named;
  };
  QVector<QJsonObject> renamed(pages_.size());
  qsizetype place = places_.size();
  for (qsizetype at = pages_.size(); at-- > 0;) {
    QJsonObject page = pages_[at];
    const PageBytes &bytes = texts_[size_t(at)];
    const qint64 offset = offsets_[at];
    // The name, in the page's places from the last to the first: each leaves
    // the characters before it where they are.
    QString text = page.value(QLatin1String("text")).toString();
    bool written = false;
    for (; place > 0 && places_[place - 1].begin >= offset; --place) {
      const int first = bytes.charsBefore(places_[place - 1].begin - offset);
      const int last = bytes.charsBefore(places_[place - 1].end - offset);
      text.replace(first, last - first, name_);
      written = true;
    }
    if (written)
      page.insert(QLatin1String("text"), text);
    if (const qint64 now = moved(offset); now != offset)
      page.insert(QLatin1String("byte_offset"), now);
    for (const auto field :
         {QLatin1String("code_names"), QLatin1String("source_names")})
      if (const auto spans = page.constFind(field);
          spans != page.constEnd() && spans->isArray())
        page.insert(field, movedSpans(spans->toArray(), moved));
    if (const auto regions = page.constFind(QLatin1String("library_regions"));
        regions != page.constEnd() && regions->isArray()) {
      QJsonArray result;
      for (const auto &value : regions->toArray()) {
        if (!value.isObject()) {
          result.append(value);
          continue;
        }
        QJsonObject region = value.toObject();
        if (const QJsonValue spans = region.value(QLatin1String("spans"));
            spans.isArray())
          region.insert(QLatin1String("spans"),
                        movedSpans(spans.toArray(), moved));
        if (const auto callee =
                region.constFind(QLatin1String("callee_identity"));
            callee != region.constEnd())
          region.insert(QLatin1String("callee_identity"), stated(*callee));
        result.append(region);
      }
      page.insert(QLatin1String("library_regions"), result);
    }
    if (const auto identity =
            page.constFind(QLatin1String("function_identity"));
        identity != page.constEnd())
      page.insert(QLatin1String("function_identity"), stated(*identity));
    if (const QJsonValue prelude = page.value(QLatin1String("prelude"));
        prelude.isObject())
      if (const QJsonValue end =
              prelude.toObject().value(QLatin1String("end_byte"));
          end.isDouble())
        if (const qint64 was = end.toInteger(), now = moved(was); now != was) {
          QJsonObject bounds = prelude.toObject();
          bounds.insert(QLatin1String("end_byte"), now);
          page.insert(QLatin1String("prelude"), bounds);
        }
    renamed[at] = page;
  }
  return renamed;
}

std::optional<RenamedPages> pagesRenamed(const QVector<QJsonObject> &pages,
                                         Address address, const QString &name) {
  const auto renaming = PagesRenaming::read(pages, address, name);
  if (!renaming)
    return std::nullopt;
  return RenamedPages{renaming->pages(), renaming->places(), renaming->data()};
}

} // namespace neverd::gui
