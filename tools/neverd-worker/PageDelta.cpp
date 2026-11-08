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

/// What \p after adds to each integer of \p before, objects that differ in
/// nothing else.
std::optional<Json> shiftOf(const Json &before, const Json &after) {
  if (!before.is_object() || !after.is_object() ||
      before.size() != after.size())
    return std::nullopt;
  Json by = Json::object();
  for (auto it = before.begin(); it != before.end(); ++it) {
    const auto other = after.find(it.key());
    if (other == after.end())
      return std::nullopt;
    if (*other == *it)
      continue;
    const auto from = integer(*it), to = integer(*other);
    if (!from || !to)
      return std::nullopt;
    by[it.key()] = *to - *from;
  }
  if (by.empty())
    return std::nullopt;
  return by;
}

/// The runs that make the list \p after from \p before, element by element,
/// where fewer than half of \p after's elements are sent again.
std::optional<Json> itemRuns(const Json &before, const Json &after) {
  Json runs = Json::array();
  std::string kind;
  std::size_t count = 0, sent = 0;
  Json by, values = Json::array();
  const auto flush = [&] {
    if (kind == "same")
      runs.push_back(Json::array({"same", count}));
    else if (kind == "shift")
      runs.push_back(Json::array({"shift", count, std::move(by)}));
    else if (kind == "put")
      runs.push_back(Json::array({"put", std::move(values)}));
    kind.clear();
    count = 0;
    by = nullptr;
    values = Json::array();
  };
  std::size_t at = 0;
  for (; at < before.size() && at < after.size(); ++at) {
    const auto &old = before[at], &now = after[at];
    if (old == now) {
      if (kind != "same") {
        flush();
        kind = "same";
      }
      ++count;
    } else if (auto shift = shiftOf(old, now)) {
      if (kind != "shift" || by != *shift) {
        flush();
        kind = "shift";
        by = std::move(*shift);
      }
      ++count;
    } else {
      if (kind != "put") {
        flush();
        kind = "put";
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
      added.push_back(after[at]);
    runs.push_back(Json::array({"new", std::move(added)}));
  }
  if (sent * 2 > after.size())
    return std::nullopt;
  return runs;
}

} // namespace

std::optional<Json> pageDelta(const Json &before, const Json &after) {
  if (!before.is_object() || !after.is_object())
    return std::nullopt;
  Json operations = Json::array();
  for (auto it = after.begin(); it != after.end(); ++it) {
    const auto old = before.find(it.key());
    if (old != before.end() && *old == *it)
      continue;
    if (old != before.end() && old->is_string() && it->is_string())
      if (auto lines = lineChanges(old->get_ref<const std::string &>(),
                                   it->get_ref<const std::string &>())) {
        operations.push_back(Json::array(
            {"lines", it.key(), lines->first, std::move(lines->second)}));
        continue;
      }
    if (old != before.end() && old->is_array() && it->is_array())
      if (auto runs = itemRuns(*old, *it)) {
        operations.push_back(
            Json::array({"items", it.key(), std::move(*runs)}));
        continue;
      }
    operations.push_back(Json::array({"set", it.key(), *it}));
  }
  for (auto it = before.begin(); it != before.end(); ++it)
    if (!after.contains(it.key()))
      operations.push_back(Json::array({"unset", it.key()}));
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
