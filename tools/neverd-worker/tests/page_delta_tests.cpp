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
  return 0;
}
