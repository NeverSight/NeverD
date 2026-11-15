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

#include "ResourceLimits.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
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

/// How many bytes \p before and \p after share at their start, or with \p
/// atEnd at their end, of the first \p limit of them.
std::size_t sharedBytes(std::string_view before, std::string_view after,
                        std::size_t limit, bool atEnd) {
  std::size_t shared = 0;
  // Blocks first, then what is left of the one that differs.
  for (std::size_t step = 4096; step; step /= 8)
    while (shared + step <= limit &&
           std::memcmp(atEnd ? before.data() + before.size() - shared - step
                             : before.data() + shared,
                       atEnd ? after.data() + after.size() - shared - step
                             : after.data() + shared,
                       step) == 0)
      shared += step;
  return shared;
}

/// How many lines \p after has and those of them that differ from \p
/// before's, where both have as many and fewer than half differ.
std::optional<std::pair<std::size_t, Json>>
lineChanges(std::string_view before, std::string_view after) {
  const auto lineCount = [](std::string_view text) {
    return static_cast<std::size_t>(
               std::count(text.begin(), text.end(), '\n')) +
           1;
  };
  const auto total = lineCount(after);
  if (lineCount(before) != total)
    return std::nullopt;
  // What the texts share at their ends holds the same lines at the same
  // places, the texts having as many: only the lines between them, each read
  // whole, can differ.
  const auto common = std::min(before.size(), after.size());
  const auto prefix = sharedBytes(before, after, common, false);
  const auto suffix = sharedBytes(before, after, common - prefix, true);
  const auto lastBreak = before.substr(0, prefix).rfind('\n');
  const auto start = lastBreak == std::string_view::npos ? 0 : lastBreak + 1;
  auto line = static_cast<std::size_t>(
      std::count(before.begin(), before.begin() + start, '\n'));
  auto endBefore = before.find('\n', before.size() - suffix);
  if (endBefore == std::string_view::npos)
    endBefore = before.size();
  const auto endAfter = after.size() - (before.size() - endBefore);
  Json changes = Json::array();
  for (std::size_t old = start, now = start;; ++line) {
    auto oldEnd = before.find('\n', old), nowEnd = after.find('\n', now);
    oldEnd = std::min(oldEnd, endBefore);
    nowEnd = std::min(nowEnd, endAfter);
    const auto text = after.substr(now, nowEnd - now);
    if (before.substr(old, oldEnd - old) != text)
      changes.push_back(Json::array({line, std::string(text)}));
    if (nowEnd == endAfter)
      break;
    old = oldEnd + 1;
    now = nowEnd + 1;
  }
  if (changes.size() * 2 > total)
    return std::nullopt;
  return std::pair{total, std::move(changes)};
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

/// What an "each" operation sets in every element of a list that has the key,
/// before the list's runs are read: none where it sets nothing.
const Json *eachValue(const Json *each, const std::string &key) {
  if (!each)
    return nullptr;
  const auto value = each->find(key);
  return value != each->end() ? &*value : nullptr;
}

/// How an element of a list reads against the one it was: the same, the same
/// but for integers that moved, or another, which is sent again.
enum class Step { Same, Shift, Put };
/// What \p after adds to each integer of \p before, by key in the objects'
/// order.  The keys are \p before's.
using Shift = std::vector<std::pair<std::string_view, std::int64_t>>;
/// How \p after reads against \p before once \p each's values are set where
/// \p before has their keys; \p by takes what a shift adds, and is empty
/// otherwise.
Step stepOf(const Json &before, const Json &after, Shift &by,
            const Json *each = nullptr) {
  by.clear();
  if (!before.is_object() || !after.is_object() ||
      before.size() != after.size())
    return before == after ? Step::Same : Step::Put;
  // Objects hold their keys in order: as many keys are the same keys where
  // each of one stands at the place of the other's.
  auto other = after.begin();
  for (auto it = before.begin(); it != before.end(); ++it, ++other) {
    if (other.key() != it.key()) {
      by.clear();
      return Step::Put;
    }
    const Json *set = eachValue(each, it.key());
    if (*other == (set ? *set : *it))
      continue;
    const auto from = set ? std::nullopt : integer(*it);
    const auto to = set ? std::nullopt : integer(*other);
    if (!from || !to) {
      by.clear();
      return Step::Put;
    }
    by.emplace_back(it.key(), *to - *from);
  }
  return by.empty() ? Step::Same : Step::Shift;
}

Json shiftJson(const Shift &by) {
  Json object = Json::object();
  for (const auto &[key, amount] : by)
    object[std::string(key)] = amount;
  return object;
}

using Items = std::vector<const Json *>;

/// The text values an "each" operation sets in the list \p before to read as
/// \p after, as many elements as it: each key that some element's text changes
/// at, where every element that has the key has one value for it in \p after
/// and none gains or loses it.  Nothing where no key is so.
std::optional<Json> eachOf(const Items &before, const Items &after) {
  if (before.size() != after.size())
    return std::nullopt;
  Json each = Json::object();
  std::vector<std::string> refused;
  const auto isRefused = [&](const std::string &key) {
    return std::find(refused.begin(), refused.end(), key) != refused.end();
  };
  for (std::size_t at = 0; at < before.size(); ++at) {
    const Json &old = *before[at], &now = *after[at];
    if (!old.is_object() || !now.is_object() || old == now)
      continue;
    for (auto it = old.begin(); it != old.end(); ++it) {
      const auto other = now.find(it.key());
      if (other == now.end() || *other == *it || !it->is_string() ||
          !other->is_string() || isRefused(it.key()))
        continue;
      if (const auto held = each.find(it.key()); held == each.end())
        each[it.key()] = *other;
      else if (*held != *other) {
        each.erase(held);
        refused.push_back(it.key());
      }
    }
  }
  if (each.empty())
    return std::nullopt;
  // Every element that has a key it sets reads that value after, and the
  // others gain none.
  for (std::size_t at = 0; at < before.size() && !each.empty(); ++at) {
    const Json &old = *before[at], &now = *after[at];
    for (auto it = each.begin(); it != each.end();) {
      const bool had = old.is_object() && old.contains(it.key());
      const auto other =
          now.is_object() ? now.find(it.key()) : Json::const_iterator();
      const bool has = now.is_object() && other != now.end();
      if (had != has || (has && *other != *it))
        it = each.erase(it);
      else
        ++it;
    }
  }
  if (each.empty())
    return std::nullopt;
  return each;
}

/// The runs that make the list \p after from \p before, element by element,
/// once \p each's values are set where its elements have their keys, where
/// fewer than half of \p after's elements are sent again.
std::optional<Json> itemRuns(const Items &before, const Items &after,
                             const Json *each = nullptr) {
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
    const auto step = stepOf(old, now, next, each);
    if (step == Step::Same) {
      if (kind != Run::Same) {
        flush();
        kind = Run::Same;
      }
      ++count;
    } else if (step == Step::Shift) {
      if (kind != Run::Shift || by != next) {
        flush();
        kind = Run::Shift;
        by.swap(next);
      }
      ++count;
    } else {
      // Past half of the list sent again, no runs answer: what is left
      // cannot bring the count back.
      if ((sent + 1) * 2 > after.size())
        return std::nullopt;
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

/// How many elements \p runs send again.
std::size_t putCount(const Json &runs) {
  std::size_t sent = 0;
  for (const auto &run : runs)
    if (run[0] == "put" || run[0] == "new")
      sent += run[1].size();
  return sent;
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

/// The elements of a field that is a list, in place: those the field holds,
/// or \p held, which takes them where the field holds its list as a value.
/// Nothing where it is no list.
const Items *itemsOf(const Field &field, Items &held) {
  if (field.kind == Field::Kind::Items)
    return &field.items;
  const Json *value = jsonOf(field);
  if (!value || !value->is_array())
    return nullptr;
  held.clear();
  held.reserve(value->size());
  for (const auto &item : *value)
    held.push_back(&item);
  return &held;
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
    Items heldOld, heldNow;
    const auto *old = itemsOf(before, heldOld), *now = itemsOf(after, heldNow);
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
  Items heldOld, heldNew;
  for (const auto &now : after.fields()) {
    const Field *old = before.find(now.name);
    // Two lists are read once: runs that keep every element say they are the
    // same.
    const Items *oldItems = old ? itemsOf(*old, heldOld) : nullptr;
    const Items *newItems = oldItems ? itemsOf(now, heldNew) : nullptr;
    std::optional<Json> runs;
    if (newItems) {
      runs = itemRuns(*oldItems, *newItems);
      if (runs && oldItems->size() == newItems->size() &&
          (runs->empty() || (runs->size() == 1 && (*runs)[0][0] == "same")))
        continue;
    } else if (old && sameValue(*old, now)) {
      continue;
    }
    if (old)
      if (const auto oldText = stringOf(*old), newText = stringOf(now);
          oldText && newText)
        if (auto lines = lineChanges(*oldText, *newText)) {
          operations.push_back(Json::array(
              {"lines", now.name, lines->first, std::move(lines->second)}));
          continue;
        }
    if (newItems) {
      // Where the runs send many elements again, a value every element
      // that has its key takes, as a document's identity in each of its
      // locals, is set once and the runs read the rest.
      if (!runs ||
          putCount(*runs) >
              ::neverd::limits::worker::kEnginePageDeltaPutsWithoutEach)
        if (const auto each = eachOf(*oldItems, *newItems))
          if (auto eachRuns = itemRuns(*oldItems, *newItems, &*each);
              eachRuns && (!runs || putCount(*eachRuns) < putCount(*runs))) {
            operations.push_back(Json::array({"each", now.name, *each}));
            if (!(eachRuns->size() == 1 && (*eachRuns)[0][0] == "same"))
              operations.push_back(
                  Json::array({"items", now.name, std::move(*eachRuns)}));
            continue;
          }
      if (runs) {
        operations.push_back(
            Json::array({"items", now.name, std::move(*runs)}));
        continue;
      }
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
    if (kind == "each" && operation.size() == 3 && at->is_array() &&
        operation[2].is_object()) {
      for (auto &item : *at) {
        if (!item.is_object())
          return std::nullopt;
        for (auto value = operation[2].begin(); value != operation[2].end();
             ++value)
          if (const auto held = item.find(value.key()); held != item.end())
            *held = *value;
      }
      continue;
    }
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

std::optional<std::vector<std::pair<std::size_t, std::ptrdiff_t>>>
changedLines(const ViewChange &wrote,
             const std::vector<std::size_t> &startsBefore,
             std::size_t bytesBefore, std::string_view textNow) {
  std::vector<std::pair<std::size_t, std::ptrdiff_t>> lines;
  std::ptrdiff_t moved = 0;
  std::size_t last = 0;
  for (const auto &span : wrote.text) {
    // In order, each within the text it was and the one it is.
    if (span.begin > span.end || span.end > bytesBefore || span.begin < last ||
        startsBefore.empty() || startsBefore.front() > span.begin)
      return std::nullopt;
    last = span.end;
    const auto line = static_cast<std::size_t>(
        std::upper_bound(startsBefore.begin(), startsBefore.end(), span.begin) -
        startsBefore.begin() - 1);
    const auto now = static_cast<std::size_t>(
        static_cast<std::ptrdiff_t>(span.begin) + moved);
    // Within its line, the break that ends it not among its bytes, as what
    // reads there now holds none.
    if ((line + 1 < startsBefore.size() &&
         span.end >= startsBefore[line + 1]) ||
        now + span.size > textNow.size() ||
        textNow.substr(now, span.size).find('\n') != std::string_view::npos)
      return std::nullopt;
    moved += static_cast<std::ptrdiff_t>(span.size) -
             static_cast<std::ptrdiff_t>(span.end - span.begin);
    if (!lines.empty() && lines.back().first == line)
      lines.back().second = moved;
    else
      lines.emplace_back(line, moved);
  }
  return lines;
}

std::optional<Json> changedPage(const PageSource &page, const Json &document,
                                const std::vector<std::size_t> &starts,
                                const DocumentChange &change, std::size_t start,
                                std::size_t end) {
  const auto &wrote = change.wrote;
  const auto &value = document;
  const auto &text = value.at("text").get_ref<const std::string &>();
  Json operations = Json::array();
  // The runs that make a page's elements of a list from those the page had:
  // each stays, or its span moved by what the patch added to that place of the
  // list (\p moved, none where it moved no span), or it is sent again where
  // the patch wrote other values of it (\p written, ascending).  With them,
  // how many are sent again; nothing where the patch does not account for an
  // element.
  const auto runsOf = [&](const std::string &name, const Items &items,
                          const std::vector<ViewChange::Moved> &moved,
                          const std::vector<std::size_t> &written)
      -> std::optional<std::pair<Json, std::size_t>> {
    const Json *const first =
        value.at(name).get_ref<const Json::array_t &>().data();
    Json runs = Json::array();
    enum class Run { None, Same, Shift, Put } kind = Run::None;
    std::size_t count = 0, sent = 0, run = 0, before = 0;
    std::ptrdiff_t byBegin = 0, byEnd = 0;
    Json values = Json::array();
    std::optional<std::size_t> last;
    const auto flush = [&] {
      if (kind == Run::Same) {
        runs.push_back(Json::array({"same", count}));
      } else if (kind == Run::Shift) {
        Json by = Json::object();
        if (byBegin)
          by["begin_byte"] = static_cast<std::int64_t>(byBegin);
        if (byEnd)
          by["end_byte"] = static_cast<std::int64_t>(byEnd);
        runs.push_back(Json::array({"shift", count, std::move(by)}));
      } else if (kind == Run::Put) {
        runs.push_back(Json::array({"put", std::move(values)}));
      }
      kind = Run::None;
      count = 0;
      values = Json::array();
    };
    for (const Json *item : items) {
      // A page holds its elements in their list's order.
      const auto place = static_cast<std::size_t>(item - first);
      if (last && place <= *last)
        return std::nullopt;
      last = place;
      std::ptrdiff_t begin = 0, stop = 0;
      if (!moved.empty()) {
        while (run < moved.size() && place >= before + moved[run].count)
          before += moved[run++].count;
        if (run == moved.size())
          return std::nullopt;
        begin = moved[run].begin;
        stop = moved[run].end;
      }
      if (std::binary_search(written.begin(), written.end(), place)) {
        if (kind != Run::Put) {
          flush();
          kind = Run::Put;
        }
        values.push_back(*item);
        ++sent;
      } else if (begin || stop) {
        if (kind != Run::Shift || byBegin != begin || byEnd != stop) {
          flush();
          kind = Run::Shift;
          byBegin = begin;
          byEnd = stop;
        }
        ++count;
      } else {
        if (kind != Run::Same) {
          flush();
          kind = Run::Same;
        }
        ++count;
      }
    }
    flush();
    return std::pair{std::move(runs), sent};
  };
  // A list's runs as pageDelta says them: none where every element stays, the
  // list whole where most of it is sent again.
  const auto say = [&](const std::string &name, const Items &items,
                       std::pair<Json, std::size_t> runs) {
    if (runs.first.empty() ||
        (runs.first.size() == 1 && runs.first[0][0] == "same"))
      return;
    if (runs.second * 2 > items.size()) {
      Json list = Json::array();
      for (const Json *item : items)
        list.push_back(*item);
      operations.push_back(Json::array({"set", name, std::move(list)}));
    } else {
      operations.push_back(Json::array({"items", name, std::move(runs.first)}));
    }
  };
  // A list a page holds whole, as a value: its elements in place.
  Items whole;
  const auto wholeOf = [&](const Field &field) -> const Items & {
    whole.clear();
    for (const auto &item : *field.value)
      whole.push_back(&item);
    return whole;
  };
  static const std::vector<ViewChange::Moved> unmoved;
  static const std::vector<std::size_t> unwritten;
  for (const auto &field : page.fields()) {
    if (field.name == "text" && field.kind == Field::Kind::Text) {
      // The lines the patch wrote that are on the page, each whole.
      Json changes = Json::array();
      for (const auto &[line, moved] : change.lines) {
        if (line < start || line >= end)
          continue;
        if (line >= starts.size())
          return std::nullopt;
        const auto begin = starts[line];
        auto stop = text.find('\n', begin);
        if (stop == std::string::npos)
          stop = text.size();
        changes.push_back(
            Json::array({line - start, text.substr(begin, stop - begin)}));
      }
      if (changes.empty())
        continue;
      const auto total = static_cast<std::size_t>(std::count(
                             field.text.begin(), field.text.end(), '\n')) +
                         1;
      if (changes.size() * 2 > total)
        operations.push_back(
            Json::array({"set", field.name, std::string(field.text)}));
      else
        operations.push_back(
            Json::array({"lines", field.name, total, std::move(changes)}));
    } else if (field.name == "rows" && field.kind == Field::Kind::Items) {
      if (wrote.rows.empty())
        continue;
      auto runs = runsOf(field.name, field.items, unmoved, wrote.rows);
      if (!runs)
        return std::nullopt;
      say(field.name, field.items, std::move(*runs));
    } else if (field.name == "code_names" && field.kind == Field::Kind::Items) {
      if (wrote.identity) {
        // Every element that has an identity reads the one the patch wrote.
        bool held = false;
        for (const Json *item : field.items)
          if (item->is_object())
            if (const auto identity = item->find("identity");
                identity != item->end()) {
              if (*identity != *wrote.identity)
                return std::nullopt;
              held = true;
            }
        if (held)
          operations.push_back(Json::array(
              {"each", field.name, {{"identity", *wrote.identity}}}));
      }
      auto runs = runsOf(field.name, field.items, wrote.codeNames, wrote.names);
      if (!runs)
        return std::nullopt;
      say(field.name, field.items, std::move(*runs));
    } else if (field.name == "source_names" &&
               field.kind == Field::Kind::Items) {
      auto runs = runsOf(field.name, field.items, wrote.sourceNames, unwritten);
      if (!runs)
        return std::nullopt;
      say(field.name, field.items, std::move(*runs));
    } else if (field.name == "byte_offset" && field.kind == Field::Kind::Own) {
      // What the bytes before the page's first line moved by.
      std::ptrdiff_t moved = 0;
      for (const auto &[line, by] : change.lines) {
        if (line >= start)
          break;
        moved = by;
      }
      if (moved)
        operations.push_back(Json::array({"set", field.name, field.own}));
    } else if (field.kind == Field::Kind::Value &&
               ((field.name == "prelude" && wrote.prelude) ||
                (field.name == "function_identity" && change.stated) ||
                (field.name == "library_regions" && change.stated))) {
      operations.push_back(Json::array({"set", field.name, *field.value}));
    } else if (field.kind == Field::Kind::Value && field.value->is_array() &&
               ((field.name == "library_regions" && !wrote.regions.empty()) ||
                (field.name == "source_variables" &&
                 !wrote.variables.empty()))) {
      // A list every page holds whole: the elements the patch wrote.
      const auto &items = wholeOf(field);
      auto runs = runsOf(field.name, items, unmoved,
                         field.name == "library_regions" ? wrote.regions
                                                         : wrote.variables);
      if (!runs)
        return std::nullopt;
      say(field.name, items, std::move(*runs));
    }
  }
  return operations;
}

} // namespace neverd::worker
