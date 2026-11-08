//===- PageDelta.cpp - What changed between two pages of a view -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// pageDelta compares two pages field by field: a text by its lines, where
/// both have as many, and a list element by element in order, where an element
/// that differs only in its numbers is shifted rather than sent again.  Where
/// most of a field changed, it is sent whole.
///
//===----------------------------------------------------------------------===//

#include "PageDelta.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neverd::worker {
namespace {

std::vector<std::string_view> splitLines(std::string_view text) {
  std::vector<std::string_view> lines;
  for (std::size_t begin = 0;;) {
    const auto end = text.find('\n', begin);
    if (end == std::string_view::npos) {
      lines.push_back(text.substr(begin));
      return lines;
    }
    lines.push_back(text.substr(begin, end - begin));
    begin = end + 1;
  }
}

/// How many lines \p after has and those of them that differ from \p
/// before's, where both have as many and fewer than half differ.
std::optional<std::pair<std::size_t, Json>>
lineChanges(std::string_view before, std::string_view after) {
  const auto old = splitLines(before), now = splitLines(after);
  if (old.size() != now.size())
    return std::nullopt;
  Json changes = Json::array();
  for (std::size_t line = 0; line < now.size(); ++line)
    if (old[line] != now[line])
      changes.push_back(Json::array({line, std::string(now[line])}));
  if (changes.size() * 2 > now.size())
    return std::nullopt;
  return std::pair{now.size(), std::move(changes)};
}

std::optional<std::int64_t> integer(const Json &value) {
  if (value.is_number_unsigned()) {
    const auto unsignedValue = value.get<std::uint64_t>();
    if (unsignedValue >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
      return std::nullopt;
    return static_cast<std::int64_t>(unsignedValue);
  }
  if (value.is_number_integer())
    return value.get<std::int64_t>();
  return std::nullopt;
}

/// What \p after adds to each integer of \p before, by key in the objects'
/// order, objects that differ in nothing else; empty where they do, or are the
/// same.  The keys are \p before's.
using Shift = std::vector<std::pair<std::string_view, std::int64_t>>;
bool shiftOf(const Json &before, const Json &after, Shift &by) {
  by.clear();
  if (!before.is_object() || !after.is_object() ||
      before.size() != after.size())
    return false;
  for (auto it = before.begin(); it != before.end(); ++it) {
    const auto other = after.find(it.key());
    if (other == after.end())
      return false;
    if (*other == *it)
      continue;
    const auto from = integer(*it), to = integer(*other);
    if (!from || !to)
      return false;
    by.emplace_back(it.key(), *to - *from);
  }
  return !by.empty();
}

Json shiftJson(const Shift &by) {
  Json object = Json::object();
  for (const auto &[key, amount] : by)
    object[std::string(key)] = amount;
  return object;
}

using Items = std::vector<const Json *>;

/// The runs that make the list \p after from \p before, element by element,
/// where fewer than half of \p after's elements are sent again.
std::optional<Json> itemRuns(const Items &before, const Items &after) {
  enum class Run { None, Same, Shift, Put };
  Json runs = Json::array();
  Run kind = Run::None;
  std::size_t count = 0, sent = 0;
  Shift by, next;
  Json values = Json::array();
  const auto flush = [&] {
    if (kind == Run::Same)
      runs.push_back(Json::array({"same", count}));
    else if (kind == Run::Shift)
      runs.push_back(Json::array({"shift", count, shiftJson(by)}));
    else if (kind == Run::Put)
      runs.push_back(Json::array({"put", std::move(values)}));
    kind = Run::None;
    count = 0;
    by.clear();
    values = Json::array();
  };
  std::size_t at = 0;
  for (; at < before.size() && at < after.size(); ++at) {
    const auto &old = *before[at], &now = *after[at];
    if (old == now) {
      if (kind != Run::Same) {
        flush();
        kind = Run::Same;
      }
      ++count;
    } else if (shiftOf(old, now, next)) {
      if (kind != Run::Shift || by != next) {
        flush();
        kind = Run::Shift;
        by.swap(next);
      }
      ++count;
    } else {
      if (kind != Run::Put) {
        flush();
        kind = Run::Put;
      }
      values.push_back(now);
      ++sent;
    }
  }
  flush();
  if (at < before.size())
    runs.push_back(Json::array({"skip", before.size() - at}));
  if (at < after.size()) {
    Json added = Json::array();
    for (; at < after.size(); ++at, ++sent)
      added.push_back(*after[at]);
    runs.push_back(Json::array({"new", std::move(added)}));
  }
  if (sent * 2 > after.size())
    return std::nullopt;
  return runs;
}

using Field = PageSource::Field;

/// The value a field holds, where it holds one as it stands.
const Json *jsonOf(const Field &field) {
  return field.kind == Field::Kind::Value ? field.value
         : field.kind == Field::Kind::Own ? &field.own
                                          : nullptr;
}

std::optional<std::string_view> stringOf(const Field &field) {
  if (field.kind == Field::Kind::Text)
    return field.text;
  if (const Json *value = jsonOf(field); value && value->is_string())
    return std::string_view(value->get_ref<const std::string &>());
  return std::nullopt;
}

/// The elements of a field that is a list, in place.
std::optional<Items> itemsOf(const Field &field) {
  if (field.kind == Field::Kind::Items)
    return field.items;
  const Json *value = jsonOf(field);
  if (!value || !value->is_array())
    return std::nullopt;
  Items items;
  items.reserve(value->size());
  for (const auto &item : *value)
    items.push_back(&item);
  return items;
}

Json materialized(const Field &field) {
  switch (field.kind) {
  case Field::Kind::Text:
    return std::string(field.text);
  case Field::Kind::Items: {
    Json items = Json::array();
    for (const Json *item : field.items)
      items.push_back(*item);
    return items;
  }
  case Field::Kind::Value:
  case Field::Kind::Own:
    break;
  }
  return *jsonOf(field);
}

/// Whether the two fields' page values are equal, as their JSON compares.
bool sameValue(const Field &before, const Field &after) {
  const auto oldText = stringOf(before), newText = stringOf(after);
  if (oldText || newText)
    return oldText && newText && *oldText == *newText;
  if (before.kind == Field::Kind::Items || after.kind == Field::Kind::Items) {
    const auto old = itemsOf(before), now = itemsOf(after);
    if (!old || !now || old->size() != now->size())
      return false;
    for (std::size_t at = 0; at < old->size(); ++at)
      if (*(*old)[at] != *(*now)[at])
        return false;
    return true;
  }
  return *jsonOf(before) == *jsonOf(after);
}

} // namespace

PageSource PageSource::of(const Json &page) {
  PageSource source;
  for (auto it = page.begin(); it != page.end(); ++it)
    source.value(it.key(), *it);
  return source;
}

PageSource::Field &PageSource::field(std::string name) {
  const auto at =
      std::lower_bound(Fields.begin(), Fields.end(), name,
                       [](const Field &field, const std::string &key) {
                         return field.name < key;
                       });
  if (at != Fields.end() && at->name == name) {
    *at = Field();
    at->name = std::move(name);
    return *at;
  }
  Field added;
  added.name = std::move(name);
  return *Fields.insert(at, std::move(added));
}

void PageSource::value(std::string name, const Json &value) {
  auto &set = field(std::move(name));
  set.kind = Field::Kind::Value;
  set.value = &value;
}

void PageSource::items(std::string name, std::vector<const Json *> items) {
  auto &set = field(std::move(name));
  set.kind = Field::Kind::Items;
  set.items = std::move(items);
}

void PageSource::text(std::string name, std::string_view text) {
  auto &set = field(std::move(name));
  set.kind = Field::Kind::Text;
  set.text = text;
}

void PageSource::own(std::string name, Json value) {
  auto &set = field(std::move(name));
  set.kind = Field::Kind::Own;
  set.own = std::move(value);
}

const PageSource::Field *PageSource::find(std::string_view name) const {
  const auto at =
      std::lower_bound(Fields.begin(), Fields.end(), name,
                       [](const Field &field, std::string_view key) {
                         return std::string_view(field.name) < key;
                       });
  return at != Fields.end() && at->name == name ? &*at : nullptr;
}

Json PageSource::json() const {
  Json page = Json::object();
  for (const auto &field : Fields)
    page[field.name] = materialized(field);
  return page;
}

std::optional<Json> pageDelta(const Json &before, const Json &after) {
  if (!before.is_object() || !after.is_object())
    return std::nullopt;
  return pageDelta(PageSource::of(before), PageSource::of(after));
}

Json pageDelta(const PageSource &before, const PageSource &after) {
  Json operations = Json::array();
  for (const auto &now : after.fields()) {
    const Field *old = before.find(now.name);
    if (old && sameValue(*old, now))
      continue;
    if (old)
      if (const auto oldText = stringOf(*old), newText = stringOf(now);
          oldText && newText)
        if (auto lines = lineChanges(*oldText, *newText)) {
          operations.push_back(Json::array(
              {"lines", now.name, lines->first, std::move(lines->second)}));
          continue;
        }
    if (old)
      if (const auto oldItems = itemsOf(*old), newItems = itemsOf(now);
          oldItems && newItems)
        if (auto runs = itemRuns(*oldItems, *newItems)) {
          operations.push_back(
              Json::array({"items", now.name, std::move(*runs)}));
          continue;
        }
    operations.push_back(Json::array({"set", now.name, materialized(now)}));
  }
  for (const auto &old : before.fields())
    if (!after.find(old.name))
      operations.push_back(Json::array({"unset", old.name}));
  return operations;
}

std::optional<Json> applyPageDelta(Json page, const Json &delta) {
  if (!page.is_object() || !delta.is_array())
    return std::nullopt;
  for (const auto &operation : delta) {
    if (!operation.is_array() || operation.size() < 2 ||
        !operation[0].is_string() || !operation[1].is_string())
      return std::nullopt;
    const auto &kind = operation[0].get_ref<const std::string &>();
    const auto &field = operation[1].get_ref<const std::string &>();
    if (kind == "set" && operation.size() == 3) {
      page[field] = operation[2];
      continue;
    }
    if (kind == "unset" && operation.size() == 2) {
      if (!page.erase(field))
        return std::nullopt;
      continue;
    }
    const auto at = page.find(field);
    if (at == page.end())
      return std::nullopt;
    if (kind == "lines" && operation.size() == 4 && at->is_string() &&
        operation[2].is_number_unsigned() && operation[3].is_array()) {
      const auto old = splitLines(at->get_ref<const std::string &>());
      if (old.size() != operation[2].get<std::size_t>())
        return std::nullopt;
      std::vector<std::string> lines(old.begin(), old.end());
      for (const auto &change : operation[3]) {
        if (!change.is_array() || change.size() != 2 ||
            !change[0].is_number_unsigned() || !change[1].is_string() ||
            change[0].get<std::size_t>() >= lines.size())
          return std::nullopt;
        lines[change[0].get<std::size_t>()] = change[1].get<std::string>();
      }
      std::string text;
      for (std::size_t line = 0; line < lines.size(); ++line) {
        if (line)
          text += '\n';
        text += lines[line];
      }
      *at = std::move(text);
      continue;
    }
    if (kind == "items" && operation.size() == 3 && at->is_array() &&
        operation[2].is_array()) {
      const Json &old = *at;
      Json items = Json::array();
      std::size_t used = 0;
      for (const auto &run : operation[2]) {
        if (!run.is_array() || run.size() < 2 || !run[0].is_string())
          return std::nullopt;
        const auto &how = run[0].get_ref<const std::string &>();
        if ((how == "same" || how == "skip" || how == "shift") &&
            run[1].is_number_unsigned()) {
          const auto count = run[1].get<std::size_t>();
          if (count > old.size() - used ||
              (how == "shift") != (run.size() == 3) ||
              (how != "shift" && run.size() != 2) ||
              (how == "shift" && !run[2].is_object()))
            return std::nullopt;
          for (std::size_t item = 0; how != "skip" && item < count; ++item) {
            Json value = old[used + item];
            if (how == "shift") {
              if (!value.is_object())
                return std::nullopt;
              for (auto by = run[2].begin(); by != run[2].end(); ++by) {
                const auto number = value.find(by.key());
                const auto from =
                    number == value.end() ? std::nullopt : integer(*number);
                const auto add = integer(*by);
                if (!from || !add)
                  return std::nullopt;
                const auto to = *from + *add;
                if (number->is_number_unsigned() && to >= 0)
                  *number = static_cast<std::uint64_t>(to);
                else
                  *number = to;
              }
            }
            items.push_back(std::move(value));
          }
          used += count;
        } else if ((how == "put" || how == "new") && run.size() == 2 &&
                   run[1].is_array()) {
          if (how == "put") {
            if (run[1].size() > old.size() - used)
              return std::nullopt;
            used += run[1].size();
          }
          for (const auto &value : run[1])
            items.push_back(value);
        } else {
          return std::nullopt;
        }
      }
      if (used != old.size())
        return std::nullopt;
      *at = std::move(items);
      continue;
    }
    return std::nullopt;
  }
  return page;
}

} // namespace neverd::worker
