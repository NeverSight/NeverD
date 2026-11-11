// What changed between two pages of a view: applying it to the first gives the
// second, a comment's line and the offsets it moves stay small, and an
// operation that does not fit the page it is applied to is refused.
#include "PageDelta.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace neverd::worker;

namespace {
void check(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

/// A page of \p lines lines whose every line names one function, with a
/// comment of \p comment on its second line.
Json page(int lines, const std::string &comment) {
  std::string text;
  Json rows = Json::array(), names = Json::array();
  for (int line = 0; line < lines; ++line) {
    const std::string call = "callee_" + std::to_string(line);
    if (line)
      text += '\n';
    const std::size_t begin = text.size() + 2;
    text += "  " + call + "();";
    names.push_back({{"begin_byte", begin},
                     {"end_byte", begin + call.size()},
                     {"kind", "address"},
                     {"address", "0x14000" + std::to_string(1000 + line)},
                     {"original", call}});
    Json row = {{"line", line},
                {"addresses", Json::array({"0x14000" + std::to_string(line)})},
                {"code_comment", line == 1 ? comment : ""}};
    if (line == 1 && !comment.empty())
      text += " // " + comment;
    rows.push_back(std::move(row));
  }
  return {{"text", text},     {"rows", rows},          {"code_names", names},
          {"offset", 0},      {"byte_offset", 0},      {"total_lines", lines},
          {"complete", true}, {"next_offset", nullptr}};
}

void roundTrip(const Json &before, const Json &after, const std::string &name,
               std::size_t smallerThan = 0) {
  const auto delta = pageDelta(before, after);
  check(delta.has_value(), name + ": no delta");
  const auto applied = applyPageDelta(before, *delta);
  check(applied.has_value(), name + ": its own delta refused");
  check(*applied == after, name + ": applying the delta gives another page");
  if (smallerThan)
    check(delta->dump().size() < smallerThan,
          name + ": delta of " + std::to_string(delta->dump().size()) +
              " bytes");
}
} // namespace

int main() {
  const Json plain = page(200, "");
  const Json noted = page(200, "a note here");
  // A comment on one line: that line, that row, and the names after it shifted.
  roundTrip(plain, noted, "comment", plain.dump().size() / 20);
  roundTrip(noted, plain, "comment taken away", plain.dump().size() / 20);
  roundTrip(plain, plain, "same page", 16);
  // A name spelled longer moves the names after it alone.
  Json renamed = plain;
  {
    auto text = renamed["text"].get<std::string>();
    const auto at = text.find("callee_5");
    text.replace(at, 8, "a_longer_name_5");
    renamed["text"] = text;
    auto &names = renamed["code_names"];
    names[5]["end_byte"] = names[5]["end_byte"].get<std::size_t>() + 7;
    for (std::size_t i = 6; i < names.size(); ++i)
      for (const char *field : {"begin_byte", "end_byte"})
        names[i][field] = names[i][field].get<std::size_t>() + 7;
  }
  roundTrip(plain, renamed, "rename", plain.dump().size() / 20);
  // A value every element that has its key takes, as a document's identity in
  // each of its locals, is set once, and the runs read the rest.
  const auto identified = [](Json view, const std::string &identity) {
    auto &names = view["code_names"];
    for (std::size_t i = 0; i < names.size(); i += 2) {
      names[i]["kind"] = "local";
      names[i]["identity"] = identity;
    }
    return view;
  };
  const Json local = identified(plain, "0xaaa");
  const Json respelled = identified(renamed, "0xbbb");
  roundTrip(local, respelled, "identity", local.dump().size() / 20);
  {
    const auto delta = *pageDelta(local, respelled);
    bool each = false;
    for (const auto &operation : delta)
      each |= operation[0] == "each" && operation[1] == "code_names" &&
              operation[2] == Json({{"identity", "0xbbb"}});
    check(each, "identity: a value every local takes is not set once");
  }
  // A value only some of the elements that have its key take is no such value.
  {
    Json some = respelled;
    some["code_names"][4]["identity"] = "0xaaa";
    roundTrip(local, some, "identity of some");
    const auto delta = *pageDelta(local, some);
    for (const auto &operation : delta)
      check(operation[0] != "each", "identity of some: set in every element");
  }
  check(*applyPageDelta(
            local, Json::array({Json::array(
                       {"each", "code_names", {{"identity", "0xccc"}}})})) ==
            identified(plain, "0xccc"),
        "each: a value set where the key is not, or not set where it is");
  check(!applyPageDelta(plain, Json::array({Json::array(
                                   {"each", "text", {{"identity", "0x1"}}})})),
        "each applied to a text");
  // Fields added and taken away, lists grown and cut, a different line count.
  Json other = plain;
  other["revision_note"] = "new";
  other.erase("next_offset");
  other["rows"].push_back({{"line", 200}, {"addresses", Json::array()}});
  other["code_names"].erase(other["code_names"].begin());
  other["text"] = other["text"].get<std::string>() + "\nextra line";
  roundTrip(plain, other, "fields and lengths");
  roundTrip(Json::object(), plain, "from nothing");
  // A delta applies only to the page it was made from.
  const auto delta = *pageDelta(plain, noted);
  check(!applyPageDelta(page(150, ""), delta),
        "delta applied to a shorter page");
  check(!applyPageDelta(Json::array(), delta), "delta applied to a list");
  check(!applyPageDelta(plain, Json::array({Json::array(
                                   {"lines", "text", 3, Json::array()})})),
        "lines of another count applied");
  check(!applyPageDelta(
            plain,
            Json::array({Json::array(
                {"items", "rows", Json::array({Json::array({"same", 5})})})})),
        "items left unread applied");
  check(!applyPageDelta(plain, Json::array({Json::array({"unknown", "text"})})),
        "unknown operation applied");
  check(
      !applyPageDelta(plain, Json::array({Json::array({"unset", "missing"})})),
      "unset of a missing field applied");

  // Pages read in place from their documents give the operations their page
  // objects give: a text's span, the items of its lists on the page, values it
  // holds and values of the page's own that replace them.
  const auto window = [](const Json &document, std::size_t from,
                         std::size_t to) {
    PageSource source;
    for (auto it = document.begin(); it != document.end(); ++it)
      if (it.key() != "text" && it.key() != "rows" && it.key() != "code_names")
        source.value(it.key(), *it);
    const auto &text = document["text"].get_ref<const std::string &>();
    std::size_t begin = 0, end = text.size(), line = 0;
    for (std::size_t at = 0; at < text.size(); ++at)
      if (text[at] == '\n') {
        ++line;
        if (line == from)
          begin = at + 1;
        if (line == to) {
          end = at;
          break;
        }
      }
    source.text("text", std::string_view(text).substr(begin, end - begin));
    std::vector<const Json *> rows, names;
    for (const auto &row : document["rows"])
      if (row["line"] >= from && row["line"] < to)
        rows.push_back(&row);
    for (const auto &name : document["code_names"])
      if (name["begin_byte"] >= begin && name["end_byte"] <= end)
        names.push_back(&name);
    source.items("rows", std::move(rows));
    source.items("code_names", std::move(names));
    source.own("offset", from);
    source.own("byte_offset", begin);
    return source;
  };
  for (const auto &[name, later] :
       {std::pair<const char *, const Json *>{"comment", &noted},
        {"same", &plain},
        {"rename", &renamed},
        {"fields and lengths", &other}})
    for (const auto &[from, to] :
         {std::pair<std::size_t, std::size_t>{0, 200}, {0, 3}, {4, 120}}) {
      const auto before = window(plain, from, to);
      const auto after = window(*later, from, to);
      const Json made = pageDelta(before, after);
      const auto read = std::string(name) + " [" + std::to_string(from) + ", " +
                        std::to_string(to) + ")";
      check(made == *pageDelta(before.json(), after.json()),
            read + ": in place gives other operations than the pages");
      const auto applied = applyPageDelta(before.json(), made);
      check(applied && *applied == after.json(),
            read + ": applying the delta gives another page");
    }
  // Only the lines between what two texts share at their ends are read: each
  // changed line is named by its place among all of them, whole, whatever the
  // texts share around it, and a text of another line count is sent whole.
  const auto textDelta = [](const std::string &before,
                            const std::string &after) {
    const Json old = {{"text", before}}, now = {{"text", after}};
    const auto delta = pageDelta(old, now);
    check(delta.has_value(), "text: no delta");
    const auto applied = applyPageDelta(old, *delta);
    check(applied && *applied == now,
          "text: applying the delta gives another text: " + after);
    return *delta;
  };
  const auto lines = [](std::size_t count, Json changes) {
    return Json::array(
        {Json::array({"lines", "text", count, std::move(changes)})});
  };
  const auto changed = [](std::size_t line, const char *text) {
    return Json::array({line, text});
  };
  // Lines that repeat: what the ends share runs past the changed line's own
  // bytes, and the line is still read whole and counted from the first.
  check(textDelta("ab\nab\nab\nab\nab", "ab\nab\nabab\nab\nab") ==
            lines(5, Json::array({changed(2, "abab")})),
        "text: a line among lines that repeat");
  check(textDelta("ab\nab\nab\nab\nab", "ab\nab\nb\nab\nab") ==
            lines(5, Json::array({changed(2, "b")})),
        "text: a line cut among lines that repeat");
  check(textDelta("one\ntwo\nthree\nfour\nfive",
                  "one!\ntwo\nthree\nfour\nfive") ==
            lines(5, Json::array({changed(0, "one!")})),
        "text: the first line");
  check(textDelta("one\ntwo\nthree\nfour\nfive",
                  "one\ntwo\nthree\nfour\nfive!") ==
            lines(5, Json::array({changed(4, "five!")})),
        "text: the last line");
  check(textDelta("one\ntwo\nthree\nfour\nfive\n",
                  "one\ntwo\nTHREE\nfour\nfive\n") ==
            lines(6, Json::array({changed(2, "THREE")})),
        "text: a line of a text that ends in a break");
  check(textDelta("one\ntwo\nthree\nfour\nfive\nsix",
                  "one\nTWO\nthree\nfour\nFIVE\nsix") ==
            lines(6, Json::array({changed(1, "TWO"), changed(4, "FIVE")})),
        "text: two lines apart");
  check(textDelta("a\n\n\n\nb", "a\n\nx\n\nb") ==
            lines(5, Json::array({changed(2, "x")})),
        "text: an empty line written");
  check(textDelta("a\nb\nc\nd", "a\nb\nc") ==
            Json::array({Json::array({"set", "text", "a\nb\nc"})}),
        "text: a line fewer is no change of lines");
  check(textDelta("a\nb", "c\nd") ==
            Json::array({Json::array({"set", "text", "c\nd"})}),
        "text: more than half of its lines changed is sent whole");
  {
    // A long text, changed far from both ends.
    std::string longText, longAfter;
    for (int line = 0; line < 3000; ++line) {
      const auto text = "  statement_" + std::to_string(line % 7) + "();\n";
      longText += text;
      longAfter += line == 1777 ? "  another();\n" : text;
    }
    check(textDelta(longText, longAfter) ==
              lines(3001, Json::array({changed(1777, "  another();")})),
          "text: a line far from both ends of a long text");
  }
  // Elements of as many keys that are not the same keys are sent again; ones
  // whose integers alone moved are shifted by what each moved.
  {
    const Json before = {{"items", Json::array({{{"a", 1}, {"b", 2}},
                                                {{"a", 3}, {"b", 4}},
                                                {{"a", 5}, {"b", 6}},
                                                {{"a", 7}, {"b", 8}},
                                                {{"a", 9}, {"b", 10}}})}};
    Json after = before;
    after["items"][1] = {{"a", 3}, {"c", 4}};
    after["items"][3] = {{"a", 17}, {"b", 8}};
    after["items"][4] = {{"a", 19}, {"b", 11}};
    const auto delta = *pageDelta(before, after);
    check(
        delta ==
            Json::array({Json::array(
                {"items", "items",
                 Json::array(
                     {Json::array({"same", 1}),
                      Json::array({"put", Json::array({{{"a", 3}, {"c", 4}}})}),
                      Json::array({"same", 1}),
                      Json::array({"shift", 1, {{"a", 10}}}),
                      Json::array({"shift", 1, {{"a", 10}, {"b", 1}}})})})}),
        "items: " + delta.dump());
    check(*applyPageDelta(before, delta) == after,
          "items: applying the delta gives another list");
    check(pageDelta(before, before)->empty(), "items: the same list changed");
  }
  for (const auto &[from, to] :
       {std::pair<std::size_t, std::size_t>{0, 200}, {0, 3}, {4, 120}}) {
    const auto before = window(local, from, to);
    const auto after = window(respelled, from, to);
    const Json made = pageDelta(before, after);
    check(made == *pageDelta(before.json(), after.json()),
          "identity: in place gives other operations than the pages");
    const auto applied = applyPageDelta(before.json(), made);
    check(applied && *applied == after.json(),
          "identity: applying the delta gives another page");
  }
  return 0;
}
