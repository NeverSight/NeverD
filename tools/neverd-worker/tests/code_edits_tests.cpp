#include "CodeEdits.h"

#include <cstdlib>
#include <iostream>
#include <sstream>

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
              const Json &renames = Json::array()) {
  CodeEdits::decorate(view, row, 0x1000, "c", renames, Json::array(), resolve);
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
} // namespace

int main() {
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
  std::cout << "Precise variable/image targets, literal isolation, UTF-8 spans "
               "and stale-edit checks passed\n";
}
