#include "CodeEdits.h"

#include "neverd/support/IntegerLiterals.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <utility>
#include <vector>

using namespace neverd::worker;

namespace {
void check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
Json page(const std::string &text) {
  Json rows = Json::array();
  std::istringstream input(text);
  std::string line;
  while (std::getline(input, line))
    rows.push_back({{"line", rows.size()}, {"addresses", Json::array()}});
  return {{"text", text},
          {"rows", rows},
          {"prelude", {{"end_byte", 0}}},
          {"source_names", Json::array()},
          {"library_regions", Json::array()}};
}
std::optional<std::uint64_t> resolve(const std::string &name) {
  if (name == "target")
    return 0x1000;
  if (name == "callee")
    return 0x1100;
  return std::nullopt;
}
void decorate(Json &view, const Json &row = nullptr,
              const Json &renames = Json::array(),
              const CodeEdits::FunctionAliases &displayNames = {}) {
  CodeEdits::ImageFacts image;
  image.read(renames, Json::array());
  CodeEdits::decorate(view, row, 0x1000, "c", image, displayNames, resolve);
}
Json target(const Json &view, const std::string &original) {
  for (const auto &name : view.at("code_names"))
    if (name.at("original") == original && name.at("kind") == "local")
      return name;
  return nullptr;
}
void refused(const Json &view, const Json &request, const std::string &code) {
  try {
    CodeEdits::validateChange(view, request);
    check(false, "Invalid edit accepted");
  } catch (const Error &error) {
    check(error.code == code, "Wrong edit rejection");
  }
}
Json sourceName(std::size_t begin, std::size_t end) {
  return {{"begin_byte", begin}, {"end_byte", end}, {"address", "0x1100"}};
}
void sourceSpanBoundaries() {
  auto spelling = page("fini fini fini fini fini");
  spelling["identifier_symbols"] = Json::array(
      {{{"identifier", "fini"}, {"symbol", "_fini"}, {"address", "0x1100"}}});
  spelling["source_names"] =
      Json::array({sourceName(6, 7), sourceName(1, 3), sourceName(16, 16),
                   sourceName(9, 10)});
  CodeEdits::spellIdentifierSymbols(spelling);
  std::vector<std::size_t> added;
  for (const auto &name : spelling.at("source_names"))
    if (name.contains("identifier"))
      added.push_back(name.at("begin_byte").get<std::size_t>());
  check(added == std::vector<std::size_t>({10, 20}),
        "Spelling changed partial overlap or touching span boundaries");

  const std::string text = "int target(void) { return callee(); }\n";
  const auto begin = text.find("callee");
  auto adjacent = page(text);
  adjacent["source_names"] = Json::array(
      {sourceName(begin + 3, begin + 6), sourceName(begin, begin + 3)});
  decorate(adjacent);
  const auto &names = adjacent.at("code_names");
  check(names.size() == 4 && names[0].at("original") == "lee" &&
            names[1].at("original") == "cal" &&
            names[3].at("original") == "callee",
        "Adjacent spans were merged or partial coverage hid the full token");

  // Accepted spans may arrive in either order. Containment and duplicate
  // starts must be rejected just like a partial overlap.
  const std::vector<std::pair<std::size_t, std::size_t>> overlaps = {
      {0, 6}, {1, 3}, {3, 8}, {0, 3}};
  for (const auto &[left, right] : overlaps) {
    for (bool reverse : {false, true}) {
      auto invalid = page(text);
      auto a = sourceName(begin, begin + 6);
      auto b = sourceName(begin + left, begin + right);
      invalid["source_names"] =
          reverse ? Json::array({b, a}) : Json::array({a, b});
      try {
        decorate(invalid);
        check(false, "Overlapping explicit source names were accepted");
      } catch (const Error &error) {
        check(error.code == "invalid_engine_result",
              "Overlapping source names produced the wrong error");
      }
    }
  }

  auto ignored = page(text);
  ignored["source_names"] = Json::array({sourceName(begin, begin + 6),
                                         sourceName(begin + 2, begin + 2),
                                         sourceName(begin + 3, begin + 1),
                                         sourceName(begin, text.size() + 1),
                                         {{"begin_byte", begin + 1},
                                          {"end_byte", begin + 4},
                                          {"symbol", "unknown"}}});
  decorate(ignored);
  check(ignored.at("code_names").size() == 2,
        "Invalid or unresolved spans interfered with accepted source names");
}

void manySourceNames() {
  std::string text = "int target(void) {\n";
  Json spans = Json::array();
  for (unsigned i = 0; i < 256; ++i) {
    spans.push_back(sourceName(text.size() + 2, text.size() + 8));
    text += "  callee();\n";
  }
  text += "  return 0;\n}\n";
  std::reverse(spans.begin(), spans.end());
  auto view = page(text);
  view["source_names"] = spans;
  decorate(view, nullptr,
           Json::array({{{"addr", "0x1100"}, {"renamed", "renamed_callee"}}}));
  check(view.at("code_names").size() == 257 &&
            view.at("source_names").size() == 256 &&
            view.at("rows").size() == 259,
        "Unordered source names lost edit targets or source rows");
  const auto &rendered = view.at("text").get_ref<const std::string &>();
  for (const auto &span : view.at("source_names")) {
    const auto begin = span.at("begin_byte").get<std::size_t>();
    const auto end = span.at("end_byte").get<std::size_t>();
    check(end <= rendered.size() && begin < end &&
              rendered.substr(begin, end - begin) == "renamed_callee",
          "A renamed explicit source span lost its displayed edit target");
  }
}

std::vector<std::string> spellings(std::string_view line,
                                   neverd::LiteralSyntax syntax) {
  bool inComment = false;
  std::vector<std::string> texts;
  for (const auto &literal : neverd::integerLiterals(line, syntax, inComment))
    texts.push_back(literal.Text);
  return texts;
}

std::string spelled(std::string_view line, neverd::LiteralRadix radix,
                    neverd::LiteralSyntax syntax = neverd::LiteralSyntax::C) {
  bool inComment = false;
  const auto literals = neverd::integerLiterals(line, syntax, inComment);
  check(literals.size() == 1, "A test line does not hold one literal");
  return neverd::spellIntegerLiteral(literals.front(), radix, syntax)
      .value_or("-");
}

void integerLiterals() {
  using neverd::LiteralRadix;
  using neverd::LiteralSyntax;
  // A number reads in hexadecimal unless it is small or round in decimal.
  for (const std::uint64_t value :
       {0ull, 7ull, 10ull, 99ull, 100ull, 999ull, 5000ull, 1000000ull})
    check(neverd::naturalLiteralRadix(value) == LiteralRadix::Decimal,
          "A small or round number did not read in decimal");
  for (const std::uint64_t value :
       {11ull, 16ull, 19ull, 20ull, 42ull, 240ull, 255ull, 1024ull, 4096ull,
        0x7FFFFFFFull, 0xFFFFFFFFull})
    check(neverd::naturalLiteralRadix(value) == LiteralRadix::Hex,
          "A number with no round decimal reading did not read in hex");
  // Comments, strings and floating literals hold no integer literal, and a
  // block comment carries over to the next line.
  check(spellings("  v = *(uint32_t *)(a1 + 24) & 0xFFFFFFFF; // 99",
                  LiteralSyntax::C) ==
            std::vector<std::string>({"24", "0xFFFFFFFF"}),
        "Literals were missed, or found in a comment");
  check(spellings("s = \"12\"; f = 1e5 + 0x1p3 + 1.5 + 2.0f; t = 'eliF';",
                  LiteralSyntax::C) == std::vector<std::string>({"'eliF'"}),
        "A string or floating literal read as an integer literal");
  bool inComment = false;
  check(neverd::integerLiterals("x = 1; /* 2", LiteralSyntax::C, inComment)
                    .size() == 1 &&
            inComment &&
            neverd::integerLiterals("3 */ y = 4;", LiteralSyntax::C, inComment)
                    .size() == 1 &&
            !inComment,
        "A block comment did not carry over lines");
  // Rust's chars and labels and Go's runes are no integer literals; their
  // suffixes and separators are.
  check(spellings("let x = 24usize + 0x18_u64 + 030 + 'a' as u32 + 1e-5;",
                  LiteralSyntax::Rust) ==
            std::vector<std::string>({"24usize", "0x18_u64", "030"}),
        "Rust literals were misread");
  check(spellings("x := 0x18 + 030 + 0o17 + 'A' + 1_000", LiteralSyntax::Go) ==
            std::vector<std::string>({"0x18", "030", "0o17", "1_000"}),
        "Go literals were misread");

  // Each base keeps the C type the written literal has under LP64 and LLP64.
  check(spelled("24", LiteralRadix::Hex) == "0x18" &&
            spelled("24", LiteralRadix::Octal) == "030" &&
            spelled("24", LiteralRadix::Binary) == "0b11000" &&
            spelled("65", LiteralRadix::Char) == "'A'" &&
            spelled("24", LiteralRadix::Char) == "'\\030'" &&
            spelled("0", LiteralRadix::Char) == "'\\0'",
        "A small literal was spelled wrongly");
  check(spelled("0xFFFFFFFF", LiteralRadix::Decimal) == "4294967295U" &&
            spelled("4294967295u", LiteralRadix::Hex) == "0xFFFFFFFFu" &&
            spelled("3000000000", LiteralRadix::Hex) == "0xB2D05E00LL" &&
            spelled("0x80000000", LiteralRadix::Char) == "-",
        "A literal's type changed with its base");
  check(spelled("'eliF'", LiteralRadix::Hex) == "0x656C6946" &&
            spelled("0x656C6946", LiteralRadix::Char) == "'eliF'" &&
            spelled("'\\xff'", LiteralRadix::Hex) == "-" &&
            spelled("0xFF", LiteralRadix::Char) == "-",
        "Character constants were not read or refused as compilers read them");
  check(spelled("24usize", LiteralRadix::Hex, LiteralSyntax::Rust) ==
                "0x18usize" &&
            spelled("24", LiteralRadix::Octal, LiteralSyntax::Rust) == "0o30" &&
            spelled("24", LiteralRadix::Char, LiteralSyntax::Rust) == "-" &&
            spelled("0x18", LiteralRadix::Octal, LiteralSyntax::Go) == "0o30" &&
            spelled("24", LiteralRadix::Char, LiteralSyntax::Go) == "-",
        "Rust or Go literals were spelled wrongly");
}

void numberFormats() {
  const std::string source = "int32_t target(int32_t value) {\n"
                             "  /* 1\n"
                             "     2 */ value = value + 24; /* 7 */\n"
                             "  return value * 3 + 0x10; // 5\n"
                             "}\n";
  auto original = page(source);
  decorate(original);
  const Json number = {{"address", "0x1000"},
                       {"representation", "c"},
                       {"kind", "number"},
                       {"line", 3},
                       {"anchor", "  return value * 3 + 0x10; // 5"},
                       {"index", 1},
                       {"radix", "decimal"}};
  CodeEdits::validateChange(original, number);
  auto row = CodeEdits::change(nullptr, number);
  // The literal after a comment that ends on the line is its first.
  auto spanned = number;
  spanned["line"] = 2;
  spanned["anchor"] = "     2 */ value = value + 24; /* 7 */";
  spanned["index"] = 0;
  spanned["radix"] = "hex";
  CodeEdits::validateChange(original, spanned);
  row = CodeEdits::change(row, spanned);
  // A local's alias on the same line keeps its span.
  const Json rename = {{"address", "0x1000"},
                       {"representation", "c"},
                       {"kind", "name"},
                       {"original", "value"},
                       {"name", "total_value"},
                       {"identity", target(original, "value").at("identity")}};
  row = CodeEdits::change(row, rename);
  auto edited = page(source);
  decorate(edited, row);
  const auto &text = edited.at("text").get_ref<const std::string &>();
  check(text.find("total_value = total_value + 0x18; /* 7 */") !=
                std::string::npos &&
            text.find("return total_value * 3 + 16; // 5") !=
                std::string::npos &&
            text.find("/* 1\n     2 */") != std::string::npos,
        "The picked bases were not shown, or a comment changed");
  check(edited.at("rows")[3].at("code_number_radixes") ==
                Json({{"1", "decimal"}}) &&
            !edited.at("rows")[1].contains("code_number_radixes"),
        "A row does not say which base its numbers were given");
  for (const auto &name : edited.at("code_names")) {
    const auto begin = name.at("begin_byte").get<std::size_t>();
    const auto end = name.at("end_byte").get<std::size_t>();
    const auto spelling = text.substr(begin, end - begin);
    check(spelling == "total_value" || spelling == "target",
          "A respelled number moved an edit span");
  }

  // The decompiler's own base again, and the bases of a changed line.
  auto reset = number;
  reset["radix"] = "number";
  auto cleared = CodeEdits::change(row, reset);
  cleared = CodeEdits::change(cleared, [&] {
    auto other = spanned;
    other["radix"] = "number";
    return other;
  }());
  check(!cleared.at("views").at("c").contains("numbers"),
        "Giving the numbers back left a base behind");
  auto changed = page("int32_t target(int32_t value) {\n"
                      "  /* 1\n"
                      "     2 */ value = value + 24; /* 7 */\n"
                      "  return value * 3 + 0x20; // 5\n"
                      "}\n");
  decorate(changed, row);
  check(changed.at("text").get<std::string>().find("+ 0x20; // 5") !=
                std::string::npos &&
            !changed.at("rows")[3].contains("code_number_radixes"),
        "A base was applied to a line that reads differently");

  auto missing = number;
  missing["index"] = 2;
  refused(original, missing, "stale_target");
  auto moved = number;
  moved["anchor"] = "  return value;";
  refused(original, moved, "stale_target");
  auto inComment = number;
  inComment["line"] = 1;
  inComment["anchor"] = "  /* 1";
  inComment["index"] = 0;
  refused(original, inComment, "stale_target");
  auto unknown = number;
  unknown["radix"] = "roman";
  refused(original, unknown, "invalid_request");
  auto wide = page("int32_t target(void) {\n  return 0xFFFFFFFF;\n}\n");
  decorate(wide);
  refused(wide,
          {{"address", "0x1000"},
           {"representation", "c"},
           {"kind", "number"},
           {"line", 1},
           {"anchor", "  return 0xFFFFFFFF;"},
           {"index", 0},
           {"radix", "char"}},
          "invalid_request");
  for (const Json &invalid :
       {Json{{"anchor", "x"}, {"radixes", {{"0", "number"}}}},
        Json{{"anchor", "x"}, {"radixes", {{"a", "hex"}}}},
        Json{{"anchor", "x"}, {"radixes", Json::object()}}}) {
    auto bad = row;
    bad["views"]["c"]["numbers"]["3"] = invalid;
    try {
      CodeEdits::validateRow(bad);
      check(false, "An invalid number format was accepted");
    } catch (const Error &error) {
      check(error.code == "invalid_request",
            "An invalid number format produced the wrong error");
    }
  }
}

// A data's new name written into a view decorate wrote reads as decorate
// writes it from the engine text respell gives, or is refused with the view as
// it was: the text, its names and spans, its locals' identity, its rows'
// anchors and the data's variable.
const std::string DataBody = "int target(int value) {\n"
                             "  int local = value + counter;\n"
                             "  counter = local * 3 + 0x10;\n"
                             "  pair.counter = 1;\n"
                             "  return local + 24;\n"
                             "}\n";
Json dataPage(const std::string &body = DataBody) {
  const std::string prelude = "/* neverd.image: 0x2000 */\nint counter;\n";
  auto view = page(prelude + body);
  view["prelude"]["end_byte"] = prelude.size();
  view["rows"][4]["addresses"] = Json::array({"0x1010"});
  return view;
}
CodeEdits::ImageFacts dataFacts(const std::string &name) {
  CodeEdits::ImageFacts image;
  image.read(name.empty() ? Json::array()
                          : Json::array({{{"addr", "0x2000"},
                                          {"original", "counter"},
                                          {"renamed", name}}}),
             Json::array({{{"addr", "0x1010"}, {"text", "kept note"}}}));
  return image;
}
void decorateData(Json &view, const Json &row, const std::string &name) {
  CodeEdits::decorate(view, row, 0x1000, "c", dataFacts(name), {}, resolve);
}
Json dataRenamed(const std::string &name) {
  return Json::array({{{"addr", "0x2000"}, {"name", name}}});
}
/// \p engine decorated under \p row with the data called \p before, then
/// called \p after: whether its new name was taken.
bool takesDataName(const Json &engine, const Json &row,
                   const std::string &before, const std::string &after) {
  auto view = engine;
  decorateData(view, row, before);
  auto patched = view;
  const auto taken = CodeEdits::takeDataNames(
      patched, CodeEdits::engineText(engine), dataRenamed(after), row, "c");
  if (!taken) {
    check(patched == view, "A refused data name changed the view");
    return false;
  }
  auto respelled = engine;
  check(CodeEdits::respell(respelled, dataRenamed(after)),
        "A data name was taken where respell refuses it");
  check(taken->text == respelled.at("text").get_ref<const std::string &>() &&
            taken->bodyStart ==
                respelled.at("prelude").at("end_byte").get<std::size_t>(),
        "A data name taken gives another engine text than respell");
  auto expected = respelled;
  decorateData(expected, row, after);
  check(patched == expected,
        "A data name taken reads otherwise than decorated afresh");
  return true;
}
void dataNames() {
  const auto engine = dataPage();
  check(takesDataName(engine, nullptr, "", "total"),
        "A data's new name was not taken");
  {
    auto view = engine;
    decorateData(view, nullptr, "");
    CodeEdits::takeDataNames(view, CodeEdits::engineText(engine),
                             dataRenamed("total"), nullptr, "c");
    const auto &text = view.at("text").get_ref<const std::string &>();
    check(text.find("int total;") != std::string::npos &&
              text.find("value + total;") != std::string::npos &&
              text.find("pair.counter = 1;") != std::string::npos,
          "A data's new name is not spelled where the data is read, or a "
          "member took it");
  }
  // A local's name stays while the same locals are declared in the same
  // order, and goes stale where the identity alone kept it.
  auto decorated = engine;
  decorateData(decorated, nullptr, "");
  Json rename = {{"address", "0x1000"},
                 {"representation", "c"},
                 {"kind", "name"},
                 {"original", "local"},
                 {"name", "result"},
                 {"identity", target(decorated, "local").at("identity")},
                 {"shape", decorated.at("code_shape")}};
  const auto shaped = CodeEdits::change(nullptr, rename);
  check(takesDataName(engine, shaped, "", "total"),
        "A data's new name was not taken beside a local's name");
  rename.erase("shape");
  check(!takesDataName(engine, CodeEdits::change(nullptr, rename), "", "total"),
        "A data's new name was taken where the locals' names go stale");
  // A number's base or a note anchored on a line the name changes reads it
  // otherwise; on another line it stays.
  const auto anchored = [&](const char *kind, std::size_t line) {
    const auto &text = engine.at("text").get_ref<const std::string &>();
    std::size_t begin = 0;
    for (std::size_t at = 0; at < line; ++at)
      begin = text.find('\n', begin) + 1;
    Json edit = {
        {"address", "0x1000"},
        {"representation", "c"},
        {"kind", kind},
        {"line", line},
        {"anchor", text.substr(begin, text.find('\n', begin) - begin)}};
    if (std::string_view(kind) == "number") {
      edit["index"] = 0;
      edit["radix"] = "hex";
    } else
      edit["text"] = "a note";
    return CodeEdits::change(nullptr, edit);
  };
  check(takesDataName(engine, anchored("number", 6), "", "total") &&
            takesDataName(engine, anchored("comment", 6), "", "total"),
        "A data's new name was not taken beside an edit of another line");
  check(!takesDataName(engine, anchored("number", 4), "", "total") &&
            !takesDataName(engine, anchored("comment", 3), "", "total"),
        "A data's new name was taken over an edit anchored on its line");
  // A name the text spells already, one another variable has, and one an
  // identifier symbol spells, may each mean something else.
  check(!takesDataName(engine, nullptr, "", "value"),
        "A data took a name the text spells");
  auto bound = engine;
  bound["source_variables"] = Json::array({{{"name", "counter"},
                                            {"address", "0x2000"},
                                            {"id", "global:8192"},
                                            {"ambiguous", false},
                                            {"parameter", false}}});
  check(takesDataName(bound, nullptr, "", "total"),
        "A data's new name was not taken by its variable");
  auto shared = bound;
  shared["source_variables"].push_back({{"name", "total"},
                                        {"address", "0x3000"},
                                        {"id", "global:12288"},
                                        {"ambiguous", false},
                                        {"parameter", false}});
  check(!takesDataName(shared, nullptr, "", "total"),
        "A data took a name another variable has");
  auto symbol = engine;
  symbol["identifier_symbols"] = Json::array(
      {{{"identifier", "total"}, {"symbol", "_total"}, {"address", "0x3000"}}});
  check(!takesDataName(symbol, nullptr, "", "total"),
        "A data took a name an identifier symbol spells");
  // A name another is numbered after, in the text or about to be: a render
  // under the new names would number them otherwise.
  const auto body = [](const std::string &line) {
    return "int target(int value) {\n  " + line +
           "\n  counter = value;\n  return counter;\n}\n";
  };
  check(takesDataName(dataPage(body("int other_x = value;")), nullptr, "",
                      "total"),
        "A data's new name was not taken beside a name that numbers nothing");
  check(!takesDataName(dataPage(body("int counter_2 = value;")), nullptr, "",
                       "total"),
        "A data gave up a name another is numbered after");
  check(!takesDataName(dataPage(body("int counter_ = value;")), nullptr, "",
                       "total"),
        "A data gave up a name another is numbered after with no number");
  check(takesDataName(dataPage(body("int counter_x2 = value;")), nullptr, "",
                      "total"),
        "A name that only begins as the data's was read as numbered after it");
  check(!takesDataName(dataPage(body("int total_1 = value;")), nullptr, "",
                       "total"),
        "A data took a name another is numbered after");
  check(takesDataName(dataPage(body("int total_a = value;")), nullptr, "",
                      "total"),
        "A name that only begins as the new one was read as numbered after it");
  {
    // The data's own name numbered after one the text spells.
    const std::string prelude = "/* neverd.image: 0x2000 */\nint value_1;\n";
    auto numbered = page(prelude + "int target(int value) {\n"
                                   "  value_1 = value;\n"
                                   "  return value_1;\n}\n");
    numbered["prelude"]["end_byte"] = prelude.size();
    auto view = numbered;
    CodeEdits::decorate(view, nullptr, 0x1000, "c", CodeEdits::ImageFacts(), {},
                        resolve);
    auto patched = view;
    check(!CodeEdits::takeDataNames(patched, CodeEdits::engineText(numbered),
                                    dataRenamed("total"), nullptr, "c") &&
              patched == view,
          "A data numbered after a name the text spells gave its name up");
    auto respelled = numbered;
    check(!CodeEdits::respell(respelled, dataRenamed("total")) &&
              respelled == numbered,
          "Respell gave up a name numbered after one the text spells");
  }
  // The globals decorate read of the engine text are those a rename reads:
  // kept with what engineText read of the same view, they give what reading
  // them again gives, and it is they that are read.
  {
    auto kept = CodeEdits::engineText(engine);
    check(!kept.globals, "An engine text holds globals no one read");
    auto view = engine;
    CodeEdits::decorate(view, nullptr, 0x1000, "c", dataFacts(""), {}, resolve,
                        nullptr, &kept);
    check(kept.globals && kept.globals->size() == 1 &&
              kept.globals->front().address == 0x2000 &&
              kept.globals->front().name == "counter" &&
              kept.globals->front().plain,
          "Decoration did not keep the globals it read");
    auto fresh = view, reused = view;
    const auto read =
        CodeEdits::takeDataNames(fresh, CodeEdits::engineText(engine),
                                 dataRenamed("total"), nullptr, "c");
    const auto again = CodeEdits::takeDataNames(
        reused, kept, dataRenamed("total"), nullptr, "c");
    check(read && again && fresh == reused && read->text == again->text &&
              read->bodyStart == again->bodyStart &&
              read->sourceNames == again->sourceNames && !again->globals,
          "The globals kept give another view than those read again");
    auto elsewhere = kept;
    elsewhere.globals->front().address = 0x9000;
    auto refused = view;
    check(!CodeEdits::takeDataNames(refused, elsewhere, dataRenamed("total"),
                                    nullptr, "c") &&
              refused == view,
          "A rename read the text's globals again where they were kept");
    // An engine text of another view takes none.
    auto other = CodeEdits::engineText(dataPage(body("int more = value;")));
    auto unrelated = engine;
    CodeEdits::decorate(unrelated, nullptr, 0x1000, "c", dataFacts(""), {},
                        resolve, nullptr, &other);
    check(!other.globals, "An engine text took the globals of another text");
  }
  // Its name again, from the engine text the first gave.
  {
    auto view = engine;
    decorateData(view, nullptr, "");
    const auto first =
        CodeEdits::takeDataNames(view, CodeEdits::engineText(engine),
                                 dataRenamed("total"), nullptr, "c");
    check(first.has_value(), "A data's new name was not taken");
    const auto second = CodeEdits::takeDataNames(
        view, *first, dataRenamed("sum"), nullptr, "c");
    auto respelled = engine;
    check(CodeEdits::respell(respelled, dataRenamed("total")) &&
              CodeEdits::respell(respelled, dataRenamed("sum")),
          "Respell refused a data's names");
    auto expected = respelled;
    decorateData(expected, nullptr, "sum");
    check(second &&
              second->text ==
                  respelled.at("text").get_ref<const std::string &>() &&
              view == expected,
          "A data's second name reads otherwise than decorated afresh");
  }
}
} // namespace

int main() {
  sourceSpanBoundaries();
  manySourceNames();
  integerLiterals();
  numberFormats();
  dataNames();
  const std::string source =
      "extern int32_t global_data; /* 0x2000 */\n"
      "int32_t callee(int32_t);\n"
      "int32_t target(int32_t value) {\n"
      "  int32_t temp = value + callee(value);\n"
      "  object.value = temp; // value global_data callee\n"
      "  const char *str = \"value callee global_data\";\n"
      "  const char *raw = u8R\"tag(\" value global_data \" )tag\";\n"
      "  const char *rust_raw = r##\"\" value callee \"##;\n"
      "  return value + global_data;\n"
      "}\n";
  auto original = page(source);
  decorate(original);
  const auto variable = target(original, "value");
  check(!variable.is_null(), "Declared parameter has no edit target");
  const Json rename = {
      {"address", "0x1000"},   {"representation", "c"},
      {"kind", "name"},        {"original", "value"},
      {"name", "input_value"}, {"identity", variable.at("identity")}};
  CodeEdits::validateChange(original, rename);
  auto row = CodeEdits::change(nullptr, rename);
  const Json note = {{"address", "0x1000"},
                     {"representation", "c"},
                     {"kind", "comment"},
                     {"line", 3},
                     {"anchor", "  int32_t temp = value + callee(value);"},
                     {"text", "\xE6\xB3\xA8\xE9\x87\x8A\nsecond */ line"}};
  CodeEdits::validateChange(original, note);
  row = CodeEdits::change(row, note);
  const Json imageNames = Json::array({{{"addr", "0x1100"},
                                        {"original", "callee"},
                                        {"renamed", "renamed_callee"}},
                                       {{"addr", "0x2000"},
                                        {"original", "global_data"},
                                        {"renamed", "renamed_data"}}});
  auto edited = page(source);
  decorate(edited, row, imageNames);
  const auto text = edited.at("text").get<std::string>();
  check(text.find("temp = input_value + renamed_callee(input_value)") !=
            std::string::npos,
        "Variables or called functions did not change");
  check(text.find("return input_value + renamed_data") != std::string::npos,
        "Explicit global address was not renamed");
  check(text.find("object.value = temp; // value global_data callee") !=
                std::string::npos &&
            text.find("\"value callee global_data\"") != std::string::npos &&
            text.find("u8R\"tag(\" value global_data \" )tag\"") !=
                std::string::npos &&
            text.find("r##\"\" value callee \"##") != std::string::npos,
        "A member, comment or string was changed");
  check(text.find("second * / line */") != std::string::npos &&
            std::count(text.begin(), text.end(), '\n') ==
                std::count(source.begin(), source.end(), '\n'),
        "An inline annotation changed the line mapping");
  for (const std::string prefix : {"R", "u8R", "uR", "UR", "LR"}) {
    const auto literal = prefix + "\"tag(\" value callee \" )tag\"";
    auto rawView = page("int target(int value) { const char *str = " + literal +
                        "; return value; }\n");
    decorate(rawView);
    auto rawRename = rename;
    rawRename["identity"] = target(rawView, "value").at("identity");
    const auto rawRow = CodeEdits::change(nullptr, rawRename);
    decorate(rawView, rawRow, imageNames);
    const auto rawText = rawView.at("text").get<std::string>();
    check(rawText.find(literal) != std::string::npos &&
              rawText.find("return input_value") != std::string::npos,
          "A prefixed raw string was mistaken for editable identifiers");
  }
  for (const auto &name : edited.at("code_names")) {
    const auto begin = name.at("begin_byte").get<std::size_t>();
    const auto end = name.at("end_byte").get<std::size_t>();
    check(end > begin && end <= text.size(), "Edit span outside UTF-8 text");
    const auto spelling = text.substr(begin, end - begin);
    check(spelling == "input_value" || spelling == "temp" ||
              spelling == "str" || spelling == "raw" ||
              spelling == "rust_raw" || spelling == "target" ||
              spelling == "renamed_callee" || spelling == "renamed_data",
          "Edit span does not cover the displayed identifier");
  }
  auto conflict = rename;
  conflict["name"] = "temp";
  refused(edited, conflict, "invalid_request");
  auto obsolete = rename;
  obsolete["identity"] = "0x1";
  refused(edited, obsolete, "stale_target");
  auto movedNote = note;
  movedNote["line"] = 4;
  refused(edited, movedNote, "stale_target");
  auto different = page(source + "// regenerated view\n");
  decorate(different, row);
  check(different.at("code_edits_stale") == true &&
            different.at("text").get<std::string>().find("input_value") ==
                std::string::npos,
        "Obsolete aliases were applied to a different rendering");
  // The listing names generic functions after what they are: main for the
  // one the C runtime's start routine calls, start for the image entry.  A
  // source view shows those names too, and the analyst's rename still wins.
  const CodeEdits::FunctionAliases displayNames = {{"target", "main"},
                                                   {"callee", "start"}};
  auto named = page(source);
  decorate(named, nullptr, Json::array(), displayNames);
  const auto namedText = named.at("text").get<std::string>();
  check(namedText.find("int32_t main(int32_t value) {") != std::string::npos &&
            namedText.find("temp = value + start(value)") != std::string::npos,
        "A source view did not show the listing's display names");
  check(namedText.find("// value global_data callee") != std::string::npos,
        "A display name changed a comment");
  auto renamedOverDisplay = page(source);
  decorate(renamedOverDisplay, nullptr, imageNames, displayNames);
  check(renamedOverDisplay.at("text").get<std::string>().find(
            "temp = value + renamed_callee(value)") != std::string::npos,
        "A display name replaced the analyst's rename");
  // A function the code takes the address of, in an image initializer or as
  // an argument, shows the name its declaration shows, or the C names a
  // function nothing declares.
  auto addressed = page("extern int callee();\n"
                        "union g_bytes_words g_bytes = { .w = {\n"
                        "  0x10ull, (uintptr_t)&callee, 0 } };\n"
                        "int target(void) { return install(callee); }\n");
  decorate(addressed, nullptr, Json::array(), displayNames);
  const auto addressedText = addressed.at("text").get<std::string>();
  check(addressedText.find("extern int start();") != std::string::npos &&
            addressedText.find("(uintptr_t)&start, 0") != std::string::npos &&
            addressedText.find("install(start)") != std::string::npos &&
            addressedText.find("callee") == std::string::npos,
        "A function's address kept a name its declaration no longer has");
  auto shadowed =
      page("int target(int value) { { int value = 1; } return value; }\n");
  decorate(shadowed);
  check(target(shadowed, "value").is_null(),
        "Shadowed declarations were guessed to be one variable");
  auto helpers =
      page("int synthetic_helper(int other) { return other; }\n" + source);
  decorate(helpers);
  check(target(helpers, "other").is_null() &&
            !target(helpers, "value").is_null(),
        "A synthetic helper was treated as the requested image function");

  // The source-cache transport fixture has 9,000 rows. Comments must not
  // become linker evidence or enter a whole-document regex search.
  std::string large =
      "extern int shim(int (*callback)(int)) __asm__(\"callee\");\n"
      "int target(void) { return shim(0) + fake(0); }\n";
  for (unsigned i = 0; i < 9000; ++i)
    large += "// " + std::string(512, 'x') +
             " int fake(int) __asm__(\"callee\"); neverd.image: 0x1100\n";
  auto largeView = page(large);
  decorate(largeView, nullptr, imageNames);
  check(largeView.at("text").get<std::string>().find(
            "return renamed_callee(0) + fake(0)") != std::string::npos,
        "Real asm linkage was lost or a comment supplied fake linkage");
  check(largeView.at("rows").size() == 9002 &&
            largeView.at("rows").back().at("code_anchor") ==
                large.substr(large.rfind('\n', large.size() - 2) + 1,
                             large.size() -
                                 large.rfind('\n', large.size() - 2) - 2),
        "Large comment source lost its final row or edit anchor");

  // C calls the start-up function `_fini` fini, as the C runtime defines its
  // own: the definition and the call find the function, and read as its
  // symbol, but the comments that spell them do not. The engine spells the
  // address in upper case.
  auto renamedView = page("/* _fini */\n"
                          "void fini(void) {\n"
                          "  return;\n"
                          "}\n"
                          "void other(void) { fini(); /* fini */ }\n");
  renamedView["identifier_symbols"] = Json::array(
      {{{"identifier", "fini"}, {"symbol", "_fini"}, {"address", "0x10AB"}}});
  CodeEdits::spellIdentifierSymbols(renamedView);
  check(renamedView.at("source_names").size() == 2,
        "An identifier the engine renamed is not a source name where spelled");
  for (const auto &name : renamedView.at("source_names"))
    check(name.at("identifier") == "fini" && name.at("symbol") == "_fini" &&
              name.at("address") == "0x10ab",
          "A renamed identifier lost its symbol or address");
  CodeEdits::ImageFacts renamedImage;
  renamedImage.read(
      Json::array({{{"addr", "0x10ab"}, {"renamed", "shutdown"}}}),
      Json::array());
  CodeEdits::decorate(renamedView, nullptr, 0x10ab, "c", renamedImage, {},
                      resolve);
  const auto &renamedText =
      renamedView.at("text").get_ref<const std::string &>();
  check(renamedText.find("void shutdown(void) {") != std::string::npos &&
            renamedText.find("{ shutdown(); /* fini */ }") !=
                std::string::npos &&
            renamedText.find("/* _fini */") != std::string::npos,
        "A rename of a renamed function does not show where C spells it");
  bool definitionTarget = false;
  for (const auto &name : renamedView.at("code_names"))
    definitionTarget |= name.at("original") == "fini" &&
                        name.value("address", std::string()) == "0x10ab";
  check(definitionTarget, "A renamed definition has no edit target");

  // A function's new name is written where the document links it, unless the
  // text may already read it as a word of its own, which a link would then be
  // ambiguous with: after a digit it is one, inside a longer name it is not.
  const auto linked = [](const std::string &text) {
    const auto begin = text.find("callee");
    return Json{{"text", text},
                {"code_names", Json::array({{{"kind", "address"},
                                             {"address", "0x1100"},
                                             {"begin_byte", begin},
                                             {"end_byte", begin + 6}}})}};
  };
  CodeEdits::ImageFacts freshName;
  freshName.renamed(0x1100, "callee", "fresh");
  for (const auto &[text, written] :
       {std::pair<const char *, bool>{"int f(void) { return callee(); }", true},
        {"int f(void) { return callee() + 9fresh; }", false},
        {"int f(void) { return callee() + refresh; }", true},
        {"int f(void) { return callee() + fresh_one; }", true},
        {"int f(void) { return callee() + fresh; }", false}}) {
    Json view = linked(text);
    const bool took = CodeEdits::takeFunctionNames(view, {0x1100}, freshName);
    check(took == written, "A new name's word is read otherwise");
    if (took)
      check(
          view.at("text").get<std::string>().find("return fresh()") !=
                  std::string::npos &&
              view.at("code_names")[0].at("end_byte") ==
                  view.at("code_names")[0].at("begin_byte").get<std::size_t>() +
                      5,
          "A new name is not written where it is linked");
  }

  std::cout << "Precise variable/image targets, literal isolation, UTF-8 spans "
               "and stale-edit checks passed\n";
}
