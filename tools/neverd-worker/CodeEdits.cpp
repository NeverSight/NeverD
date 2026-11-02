#include "CodeEdits.h"

#include "ResourceLimits.h"

#include "neverd/support/IntegerLiterals.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace neverd::worker {
namespace {
// A line or literal index as an object key: its decimal digits.
bool indexKey(const std::string &key, std::size_t digits) {
  return !key.empty() && key.size() <= digits &&
         std::all_of(key.begin(), key.end(),
                     [](unsigned char c) { return std::isdigit(c); });
}
// The literal grammar the view's text reads in.
LiteralSyntax literalSyntax(const Json &view,
                            const std::string &representation) {
  const auto dialect = view.value("dialect", representation);
  if (dialect == "rust")
    return LiteralSyntax::Rust;
  if (dialect == "go")
    return LiteralSyntax::Go;
  return LiteralSyntax::C;
}
// Whether each line of \p text that \p wanted lists starts inside a block
// comment, which integerLiterals carries from line to line.
std::unordered_map<std::size_t, bool>
commentStates(std::string_view text, LiteralSyntax syntax,
              std::set<std::size_t> wanted) {
  std::unordered_map<std::size_t, bool> states;
  bool inComment = false;
  std::size_t start = 0;
  for (std::size_t line = 0; !wanted.empty(); ++line) {
    if (wanted.erase(line))
      states.emplace(line, inComment);
    const auto end = text.find('\n', start);
    if (end == std::string_view::npos)
      break;
    (void)integerLiterals(text.substr(start, end - start), syntax, inComment);
    start = end + 1;
  }
  return states;
}
bool identifier(const std::string &name) {
  if (name.empty() ||
      name.size() >
          ::neverd::limits::worker::kCodeEditsIdentifierNameSizeLimit ||
      !(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_'))
    return false;
  return std::all_of(name.begin(), name.end(), [](unsigned char c) {
    return std::isalnum(c) || c == '_';
  });
}
std::string sourceIdentity(const std::string &text) {
  // A portable presentation identity, not an authentication or semantic proof.
  std::uint64_t value = 14695981039346656037ULL;
  for (unsigned char byte : text) {
    value ^= byte;
    value *= 1099511628211ULL;
  }
  return hexAddress(value);
}
bool source(const std::string &name) {
  return name == "c" || name == "llvmc" || name == "source" || name == "cpp" ||
         name == "rust" || name == "go";
}
// Prefix maxima answer exact interval predicates even for unsorted,
// overlapping or empty input spans. Never merge neighbouring spans.
class SourceSpanIndex {
  std::vector<std::pair<std::size_t, std::size_t>> spans_;

public:
  template <class Iterator>
  SourceSpanIndex(Iterator begin, Iterator end) : spans_(begin, end) {
    std::sort(spans_.begin(), spans_.end());
    std::size_t maximum = 0;
    for (auto &span : spans_) {
      maximum = std::max(maximum, span.second);
      span.second = maximum;
    }
  }
  bool overlaps(std::size_t begin, std::size_t end) const {
    const auto after = std::lower_bound(
        spans_.begin(), spans_.end(), end,
        [](const auto &span, std::size_t end) { return span.first < end; });
    return after != spans_.begin() && std::prev(after)->second > begin;
  }
  bool contains(std::size_t begin, std::size_t end) const {
    const auto after = std::upper_bound(
        spans_.begin(), spans_.end(), begin,
        [](std::size_t begin, const auto &span) { return begin < span.first; });
    return after != spans_.begin() && std::prev(after)->second >= end;
  }
};

struct Token {
  std::size_t begin, end;
  std::string text;
};
std::vector<Token> tokens(const std::string &text) {
  std::vector<Token> result;
  for (std::size_t i = 0; i < text.size();) {
    if (std::isspace(static_cast<unsigned char>(text[i]))) {
      ++i;
      continue;
    }
    if (text.compare(i, 2, "//") == 0) {
      const auto end = text.find('\n', i);
      i = end == std::string::npos ? text.size() : end;
      continue;
    }
    if (text.compare(i, 2, "/*") == 0) {
      const auto end = text.find("*/", i + 2);
      i = end == std::string::npos ? text.size() : end + 2;
      continue;
    }
    // C++ raw strings can contain quotes and identifier-looking text.
    auto raw = i;
    if (text[i] == 'u' || text[i] == 'U' || text[i] == 'L')
      for (const std::string_view prefix : {"u8R\"", "uR\"", "UR\"", "LR\""})
        if (text.compare(i, prefix.size(), prefix) == 0) {
          raw += prefix.size() - 2;
          break;
        }
    if (text.compare(raw, 2, "R\"") == 0) {
      const auto open = text.find('(', raw + 2);
      if (open != std::string::npos && open - raw <= 18) {
        const auto delimiter = text.substr(raw + 2, open - raw - 2);
        const auto close = text.find(")" + delimiter + "\"", open + 1);
        if (close != std::string::npos) {
          i = close + delimiter.size() + 2;
          continue;
        }
      }
    }
    // Rust raw strings and Go raw strings likewise contain arbitrary quotes.
    if (text[i] == 'r') {
      auto quote = i + 1;
      while (quote < text.size() && text[quote] == '#')
        ++quote;
      if (quote < text.size() && text[quote] == '"') {
        const auto hashes = text.substr(i + 1, quote - i - 1);
        const auto close = text.find("\"" + hashes, quote + 1);
        i = close == std::string::npos ? text.size()
                                       : close + hashes.size() + 1;
        continue;
      }
    }
    if (text[i] == '`') {
      const auto close = text.find('`', i + 1);
      i = close == std::string::npos ? text.size() : close + 1;
      continue;
    }
    if (text[i] == '\"' || text[i] == '\'') {
      const char quote = text[i++];
      while (i < text.size()) {
        if (text[i++] == '\\' && i < text.size())
          ++i;
        else if (text[i - 1] == quote)
          break;
      }
      continue;
    }
    const auto begin = i++;
    if (std::isalpha(static_cast<unsigned char>(text[begin])) ||
        text[begin] == '_')
      while (
          i < text.size() &&
          (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '_'))
        ++i;
    result.push_back({begin, i, text.substr(begin, i - begin)});
  }
  return result;
}

// Find compiler-spelled function linkage using the existing literal/comment
// aware tokens. A regex search across the entire document revisits long
// comments even when there is no asm label anywhere in the source.
std::unordered_map<std::string, std::string>
sourceLinks(const std::string &text, const std::vector<Token> &ts) {
  std::vector<std::size_t> close(ts.size(), ts.size()), stack;
  for (std::size_t i = 0; i < ts.size(); ++i) {
    if (ts[i].text == "(")
      stack.push_back(i);
    else if (ts[i].text == ")" && !stack.empty()) {
      close[stack.back()] = i;
      stack.pop_back();
    }
  }
  std::unordered_map<std::string, std::string> links;
  for (std::size_t i = 0; i + 1 < ts.size(); ++i) {
    if (!identifier(ts[i].text) || ts[i + 1].text != "(")
      continue;
    const auto end = close[i + 1];
    if (end >= ts.size() || ts.size() - end < 4 ||
        ts[end + 1].text != "__asm__" || ts[end + 2].text != "(" ||
        ts[end + 3].text != ")")
      continue;
    auto literal = std::string_view(text).substr(
        ts[end + 2].end, ts[end + 3].begin - ts[end + 2].end);
    while (!literal.empty() &&
           std::isspace(static_cast<unsigned char>(literal.front())))
      literal.remove_prefix(1);
    while (!literal.empty() &&
           std::isspace(static_cast<unsigned char>(literal.back())))
      literal.remove_suffix(1);
    if (literal.size() < 3 || literal.front() != '"' || literal.back() != '"')
      continue;
    literal.remove_prefix(1);
    literal.remove_suffix(1);
    // Escaped/concatenated labels need a C string decoder; never guess them.
    if (literal.find_first_of("\\\"\r\n") == literal.npos)
      links[ts[i].text] = std::string(literal);
  }
  return links;
}
bool type(const std::string &name) {
  static const std::set<std::string> types = {
      "int",    "char",     "short",  "long",    "float",  "double",  "bool",
      "auto",   "unsigned", "signed", "void",    "size_t", "ssize_t", "i8",
      "i16",    "i32",      "i64",    "u8",      "u16",    "u32",     "u64",
      "usize",  "isize",    "int8",   "int16",   "int32",  "int64",   "uint8",
      "uint16", "uint32",   "uint64", "uintptr", "string"};
  return types.contains(name) || name.ends_with("_t");
}
bool aliasIdentifier(const std::string &name) {
  static const std::set<std::string> keywords = {
      "if",     "else",     "return", "switch",    "case",     "default",
      "for",    "while",    "do",     "break",     "continue", "goto",
      "const",  "volatile", "static", "extern",    "typedef",  "struct",
      "union",  "enum",     "class",  "namespace", "sizeof",   "alignof",
      "new",    "delete",   "throw",  "try",       "catch",    "fn",
      "func",   "let",      "mut",    "var",       "true",     "false",
      "nullptr"};
  return identifier(name) && !type(name) && !keywords.contains(name);
}
bool member(const std::vector<Token> &ts, std::size_t i) {
  return i && (ts[i - 1].text == "." ||
               (i > 1 && ts[i - 2].text == "-" && ts[i - 1].text == ">") ||
               (i > 1 && ts[i - 2].text == ":" && ts[i - 1].text == ":"));
}
std::string inlineComment(std::string text) {
  for (std::size_t at = 0; (at = text.find("*/", at)) != std::string::npos;
       at += 3)
    text.replace(at, 2, "* /");
  for (auto &c : text)
    if (c == '\n' || c == '\r')
      c = ' ';
  return " /* " + text + " */";
}
} // namespace

void CodeEdits::validateRow(const Json &row) {
  (void)parseAddress(stringField(row, "addr"));
  auto typeImage = [](const Json &type) {
    const auto hash = stringField(
        type, "input_sha256", {},
        ::neverd::limits::worker::
            kCodeEditsValidateRowTypeImageHashInputSha256StringFieldMax);
    if (hash.size() != 64 ||
        !std::all_of(hash.begin(), hash.end(),
                     [](unsigned char c) { return std::isxdigit(c); }))
      throw Error("invalid_request",
                  "A source type needs its input image identity");
  };
  if (row.contains("object_type")) {
    const auto &type = row.at("object_type");
    typeImage(type);
    (void)stringField(
        type, "type", {},
        ::neverd::limits::worker::kCodeEditsValidateRowTypeStringFieldMax);
    if (!sizeField(type, "bytes", 0,
                   ::neverd::limits::worker::
                       kCodeEditsValidateRowFloatingBytesSizeFieldMax) ||
        !type.at("floating").is_boolean())
      throw Error("invalid_request", "Invalid global object type");
  }
  if (row.contains("stack_frame")) {
    const auto &Frame = row.at("stack_frame");
    typeImage(Frame);
    if (!Frame.is_object() ||
        Frame.dump().size() >
            ::neverd::limits::worker::kCodeEditsValidateRowFrameDumpSizeLimit ||
        stringField(Frame, "snapshot", {},
                    ::neverd::limits::worker::
                        kCodeEditsValidateRowSnapshotStringFieldMax)
            .empty() ||
        !Frame.value("points", Json::array()).is_array() ||
        Frame.value("points", Json::array()).size() >
            ::neverd::limits::worker::
                kCodeEditsValidateRowFrameValuePointsJsonArraySizeLimit ||
        !Frame.value("variables", Json::array()).is_array() ||
        Frame.value("variables", Json::array()).size() >
            ::neverd::limits::worker::
                kCodeEditsValidateRowFrameValueVariablesJsonArraySizeLimit)
      throw Error("invalid_request", "Invalid stack frame table");
  }
  const auto calls = row.value("call_types", Json::object());
  if (!calls.is_object() ||
      calls.size() >
          ::neverd::limits::worker::kCodeEditsValidateRowCallsSizeLimit ||
      calls.dump().size() >
          ::neverd::limits::worker::kCodeEditsValidateRowCallsDumpSizeLimit)
    throw Error("invalid_request",
                "Invalid or over-budget call-site declaration table");
  for (const auto &[id, type] : calls.items()) {
    if (id.empty() ||
        id.size() > ::neverd::limits::worker::kCodeEditsValidateRowIdSizeLimit)
      throw Error("invalid_request", "Invalid call-site identity");
    typeImage(type);
    if (stringField(type, "snapshot", {},
                    ::neverd::limits::worker::
                        kCodeEditsValidateRowDeclarationSnapshotStringFieldMax)
            .empty() ||
        stringField(
            type, "declaration", {},
            ::neverd::limits::worker::
                kCodeEditsValidateRowDeclarationSnapshotDeclarationStringFieldMax)
            .empty())
      throw Error("invalid_request", "Invalid call-site declaration");
    const auto actuals = type.value("variadic_types", Json::array());
    if (!actuals.is_array() ||
        actuals.size() >
            ::neverd::limits::worker::kCodeEditsValidateRowActualsSizeLimit)
      throw Error("invalid_request", "Invalid actual variadic argument types");
    for (const auto &actual : actuals)
      if (!actual.is_string() ||
          actual.get_ref<const std::string &>().empty() ||
          actual.get_ref<const std::string &>().size() >
              ::neverd::limits::worker::
                  kCodeEditsValidateRowActualGetRefConstStdStringSizeLimit)
        throw Error("invalid_request", "Invalid actual variadic argument type");
  }
  if (!row.contains("views") || !row.at("views").is_object() ||
      row.at("views").size() > ::neverd::limits::worker::kMaxSourceEditViews ||
      row.dump().size() >
          ::neverd::limits::worker::kCodeEditsValidateRowRowDumpSizeLimit)
    throw Error("invalid_request", "Invalid bounded code-edit state");
  for (const auto &[representation, view] : row.at("views").items()) {
    if (!source(representation) || !view.is_object())
      throw Error("invalid_request", "Code edits need a source representation");
    const auto names = view.value("names", Json::object());
    if (!names.empty())
      (void)parseAddress(stringField(view, "identity"));
    if (view.contains("shape"))
      (void)parseAddress(stringField(
          view, "shape", {},
          ::neverd::limits::worker::kCodeEditsValidateRowShapeStringFieldMax));
    const auto comments = view.value("comments", Json::object());
    if (!names.is_object() || !comments.is_object() ||
        names.size() >
            ::neverd::limits::worker::kCodeEditsValidateRowNamesSizeLimit ||
        comments.size() >
            ::neverd::limits::worker::kCodeEditsValidateRowCommentsSizeLimit)
      throw Error("invalid_request", "Code-edit entries exceed their budget");
    std::set<std::string> aliases;
    for (const auto &[original, alias] : names.items())
      if (!identifier(original) || !alias.is_string() ||
          !aliasIdentifier(alias.get<std::string>()) ||
          !aliases.insert(alias.get<std::string>()).second)
        throw Error("invalid_request",
                    "Local names must be distinct identifiers");
    for (const auto &[line, note] : comments.items()) {
      if (!indexKey(line, 10) || !note.is_object())
        throw Error("invalid_request", "Invalid code-comment anchor");
      (void)stringField(
          note, "anchor", {},
          ::neverd::limits::worker::kCodeEditsValidateRowAnchorStringFieldMax);
      (void)stringField(
          note, "text", {},
          ::neverd::limits::worker::kCodeEditsValidateRowTextStringFieldMax);
    }
    // The bases the user picked for the integer literals of a line, by their
    // index on it, while the line reads as its anchor.
    const auto numbers = view.value("numbers", Json::object());
    const auto variableTypes = view.value("variable_types", Json::object());
    const auto variableEdits = view.value("variable_edits", Json::array());
    if (!variableEdits.is_array() ||
        variableEdits.size() > ::neverd::limits::worker::
                                   kCodeEditsValidateRowVariableEditsSizeLimit)
      throw Error("invalid_request", "Invalid variable association table");
    for (const auto &edit : variableEdits) {
      const auto kind =
          stringField(edit, "kind", {},
                      ::neverd::limits::worker::
                          kCodeEditsValidateRowKindKindStringFieldMax);
      if (kind != "map" && kind != "split")
        throw Error("invalid_request", "Invalid variable association kind");
      if (stringField(
              edit, "from", {},
              ::neverd::limits::worker::
                  kCodeEditsValidateRowToMapSnapshotFromFromStringFieldMax)
              .empty() ||
          stringField(
              edit, "snapshot", {},
              ::neverd::limits::worker::
                  kCodeEditsValidateRowToMapSnapshotFromSnapshotStringFieldMax)
              .empty() ||
          (kind == "map" &&
           stringField(
               edit, "to", {},
               ::neverd::limits::worker::
                   kCodeEditsValidateRowToMapSnapshotFromToStringFieldMax)
               .empty()))
        throw Error("invalid_request", "Invalid variable association identity");
      typeImage(edit);
    }
    if (!variableTypes.is_object() ||
        variableTypes.size() > ::neverd::limits::worker::
                                   kCodeEditsValidateRowVariableTypesSizeLimit)
      throw Error("invalid_request", "Invalid source variable type table");
    for (const auto &[id, edit] : variableTypes.items()) {
      if (id.empty() ||
          id.size() >
              ::neverd::limits::worker::kCodeEditsValidateRowIdSizeLimit ||
          !edit.is_object())
        throw Error("invalid_request", "Invalid source variable type identity");
      (void)stringField(edit, "snapshot", {},
                        ::neverd::limits::worker::
                            kCodeEditsValidateRowSnapshotStringFieldMax);
      typeImage(edit);
      (void)stringField(
          edit, "type", {},
          ::neverd::limits::worker::kCodeEditsValidateRowTypeStringFieldMax);
      if (!sizeField(edit, "bytes", 0,
                     ::neverd::limits::worker::
                         kCodeEditsValidateRowBytesSizeFieldMax) ||
          !edit.at("floating").is_boolean())
        throw Error("invalid_request", "Invalid source variable type width");
    }
    if (!numbers.is_object() ||
        numbers.size() >
            ::neverd::limits::worker::kCodeEditsValidateRowNumbersSizeLimit)
      throw Error("invalid_request", "Code-edit entries exceed their budget");
    for (const auto &[line, entry] : numbers.items()) {
      if (!indexKey(line, 10) || !entry.is_object())
        throw Error("invalid_request", "Invalid number-format anchor");
      (void)stringField(
          entry, "anchor", {},
          ::neverd::limits::worker::kCodeEditsValidateRowAnchorStringFieldMax);
      const auto radixes = entry.value("radixes", Json::object());
      if (!radixes.is_object() || radixes.empty() ||
          radixes.size() >
              ::neverd::limits::worker::kCodeEditsValidateRowRadixesSizeLimit)
        throw Error("invalid_request", "Invalid number formats");
      for (const auto &[index, radix] : radixes.items()) {
        const auto base = radix.is_string()
                              ? literalRadixNamed(radix.get<std::string>())
                              : std::nullopt;
        if (!indexKey(index, 4) || !base || *base == LiteralRadix::Number)
          throw Error("invalid_request", "Invalid number format");
      }
    }
  }
}

Json CodeEdits::change(Json row, const Json &request) {
  const auto representation = stringField(
      request, "representation", {},
      ::neverd::limits::worker::kCodeEditsChangeRepresentationStringFieldMax);
  if (!source(representation))
    throw Error("invalid_request", "Code edits need a source representation");
  if (row.is_null())
    row = {{"addr", stringField(request, "address")},
           {"views", Json::object()}};
  auto &view = row["views"][representation];
  if (view.is_null())
    view = {{"names", Json::object()}, {"comments", Json::object()}};
  const auto kind = stringField(
      request, "kind", {},
      ::neverd::limits::worker::kCodeEditsChangeKindKindStringFieldMax);
  if (kind == "stack_frame") {
    if (request.value("reset", false))
      row.erase("stack_frame");
    else
      row["stack_frame"] = request.at("stack_frame");
  } else if (kind == "global_type") {
    if (request.value("reset", false))
      row.erase("object_type");
    else
      row["object_type"] = {
          {"input_sha256",
           stringField(
               request, "input_sha256", {},
               ::neverd::limits::worker::
                   kCodeEditsChangeStackFrameGlobalTypeRowObjectTypeInputSha256StringFieldMax)},
          {"type",
           stringField(
               request, "type", {},
               ::neverd::limits::worker::
                   kCodeEditsChangeStackFrameGlobalTypeRowObjectTypeTypeStringFieldMax)},
          {"bytes",
           sizeField(
               request, "bytes", 0,
               ::neverd::limits::worker::
                   kCodeEditsChangeStackFrameGlobalTypeRowObjectTypeBytesSizeFieldMax)},
          {"floating", request.value("floating", false)}};
  } else if (kind == "name") {
    const auto identity = stringField(
        request, "identity", {},
        ::neverd::limits::worker::
            kCodeEditsChangeStackFrameGlobalTypeNameIdentityStringFieldMax);
    (void)parseAddress(identity);
    // The names stay while the text is the same code's: the same locals
    // in the same order (decorate's code_shape).
    const auto shape = request.value("shape", std::string());
    if (view.value("identity", std::string()) != identity &&
        (shape.empty() || view.value("shape", std::string()) != shape))
      view["names"] = Json::object();
    view["identity"] = identity;
    if (!shape.empty())
      view["shape"] = shape;
    else
      view.erase("shape");
    const auto original = stringField(
        request, "original", {},
        ::neverd::limits::worker::
            kCodeEditsChangeStackFrameGlobalTypeNameOriginalStringFieldMax);
    const auto name = stringField(
        request, "name", {},
        ::neverd::limits::worker::
            kCodeEditsChangeStackFrameGlobalTypeNameNameStringFieldMax);
    if (!identifier(original) ||
        (!name.empty() && name != original && !aliasIdentifier(name)))
      throw Error("invalid_request", "A local name must be an identifier");
    if (name.empty() || name == original)
      view["names"].erase(original);
    else
      view["names"][original] = name;
  } else if (kind == "comment") {
    const auto line = sizeField(
        request, "line", 0,
        ::neverd::limits::worker::
            kCodeEditsChangeStackFrameGlobalTypeNameCommentLineLineSizeFieldMax);
    const auto text = stringField(
        request, "text", {},
        ::neverd::limits::worker::
            kCodeEditsChangeStackFrameGlobalTypeNameCommentTextTextStringFieldMax);
    const auto key = std::to_string(line);
    if (text.empty())
      view["comments"].erase(key);
    else
      view["comments"][key] = {
          {"anchor",
           stringField(
               request, "anchor", {},
               ::neverd::limits::worker::
                   kCodeEditsChangeStackFrameGlobalTypeNameCommentViewCommentsKeyAnchorStringFieldMax)},
          {"text", text}};
  } else if (kind == "number") {
    const auto key = std::to_string(sizeField(
        request, "line", 0,
        ::neverd::limits::worker::
            kCodeEditsChangeStackFrameGlobalTypeNameCommentNumberKeyLineSizeFieldMax));
    const auto index = std::to_string(sizeField(
        request, "index", 0,
        ::neverd::limits::worker::
            kCodeEditsChangeStackFrameGlobalTypeNameCommentNumberIndexSizeFieldMax));
    const auto anchor = stringField(
        request, "anchor", {},
        ::neverd::limits::worker::
            kCodeEditsChangeStackFrameGlobalTypeNameCommentNumberAnchorStringFieldMax);
    const auto radix = literalRadixNamed(stringField(
        request, "radix", {},
        ::neverd::limits::worker::
            kCodeEditsChangeStackFrameGlobalTypeNameCommentNumberRadixStringFieldMax));
    if (!radix)
      throw Error("invalid_request", "Unknown number base");
    if (!view.contains("numbers"))
      view["numbers"] = Json::object();
    auto &numbers = view["numbers"];
    // A line that reads differently now starts without bases.
    if (!numbers.contains(key) ||
        numbers[key].value("anchor", std::string()) != anchor)
      numbers[key] = {{"anchor", anchor}, {"radixes", Json::object()}};
    auto &radixes = numbers[key]["radixes"];
    if (*radix == LiteralRadix::Number)
      radixes.erase(index);
    else
      radixes[index] = std::string(literalRadixName(*radix));
    if (radixes.empty())
      numbers.erase(key);
    if (numbers.empty())
      view.erase("numbers");
  } else if (kind == "call_type") {
    row["call_types"] = request.at("call_types");
  } else if (kind == "variables") {
    view["variable_edits"] = request.at("variable_edits");
  } else if (kind == "type") {
    const auto id = stringField(
        request, "variable_id", {},
        ::neverd::limits::worker::
            kCodeEditsChangeStackFrameGlobalTypeNameCommentNumberCallTypeVariablesTypeIdVariableIdStringFieldMax);
    if (!view.contains("variable_types"))
      view["variable_types"] = Json::object();
    if (request.value("reset", false))
      view["variable_types"].erase(id);
    else
      view["variable_types"][id] = {
          {"input_sha256",
           stringField(
               request, "input_sha256", {},
               ::neverd::limits::worker::
                   kCodeEditsChangeStackFrameGlobalTypeNameCommentNumberCallTypeVariablesTypeViewVariableTypesIdInputSha256StringFieldMax)},
          {"snapshot",
           stringField(
               request, "snapshot", {},
               ::neverd::limits::worker::
                   kCodeEditsChangeStackFrameGlobalTypeNameCommentNumberCallTypeVariablesTypeViewVariableTypesIdSnapshotStringFieldMax)},
          {"type",
           stringField(
               request, "type", {},
               ::neverd::limits::worker::
                   kCodeEditsChangeStackFrameGlobalTypeNameCommentNumberCallTypeVariablesTypeViewVariableTypesIdTypeStringFieldMax)},
          {"bytes",
           sizeField(
               request, "bytes", 0,
               ::neverd::limits::worker::
                   kCodeEditsChangeStackFrameGlobalTypeNameCommentNumberCallTypeVariablesTypeViewVariableTypesIdBytesSizeFieldMax)},
          {"floating", request.value("floating", false)}};
  } else
    throw Error("invalid_request", "Unknown code edit kind");
  validateRow(row);
  return row;
}

Json CodeEdits::analysisState(const Json &rows) {
  std::map<std::uint64_t, Json> ordered;
  for (auto row : rows) {
    validateRow(row);
    auto &views = row.at("views");
    for (auto it = views.begin(); it != views.end();) {
      auto &view = it.value();
      for (const auto *field : {"names", "identity", "shape"})
        view.erase(field);
      if (view.value("comments", Json::object()).empty())
        view.erase("comments");
      if (view.empty())
        it = views.erase(it);
      else
        ++it;
    }
    if (views.empty())
      row.erase("views");
    if (row.size() != 1)
      ordered.emplace(parseAddress(row.at("addr").get<std::string>()),
                      std::move(row));
  }
  Json result = Json::array();
  for (auto &[address, row] : ordered)
    result.push_back(std::move(row));
  return result;
}

void CodeEdits::validateChange(const Json &view, const Json &request) {
  if (request.value("kind", std::string()) == "call_type") {
    // Clearing policies never needs to rebind an occurrence. It must remain
    // available when a stale or over-budget analysis cannot expose its graph.
    if (request.value("action", std::string()) == "clear")
      return;
    if (view.value("call_snapshot", std::string()).empty())
      throw Error("edit_budget",
                  "The original call analysis exceeds the editing budget");
    if (stringField(
            request, "snapshot", {},
            ::neverd::limits::worker::
                kCodeEditsValidateChangeCallTypeKindCallSnapshotSnapshotStringFieldMax) !=
        view.value("call_snapshot", std::string()))
      throw Error("stale_target",
                  "The original call analysis changed; select the call again");
    const auto id = stringField(
        request, "call_id", {},
        ::neverd::limits::worker::
            kCodeEditsValidateChangeCallTypeKindIdCallIdStringFieldMax);
    for (const auto &call : view.value("source_calls", Json::array()))
      if (call.value("id", std::string()) == id)
        return;
    throw Error("stale_target",
                "The engine cannot identify this original call occurrence");
  }
  if (request.value("kind", std::string()) == "type") {
    const auto id = stringField(
        request, "variable_id", {},
        ::neverd::limits::worker::
            kCodeEditsValidateChangeTypeKindIdVariableIdStringFieldMax);
    if (request.value("snapshot", std::string()) !=
        view.value("variable_snapshot", std::string()))
      throw Error("stale_target",
                  "The source analysis changed; select the variable again");
    for (const auto &variable : view.value("source_variables", Json::array()))
      if (variable.value("id", std::string()) == id &&
          !variable.value("ambiguous", true) &&
          !variable.value("parameter", true))
        return;
    throw Error("stale_target",
                "The engine cannot identify this variable unambiguously");
  }
  if (request.value("kind", std::string()) == "name") {
    const auto original = stringField(request, "original");
    const auto identity = stringField(request, "identity");
    const auto name = stringField(request, "name");
    std::vector<std::pair<std::size_t, std::size_t>> occurrences;
    for (const auto &target : view.value("code_names", Json::array()))
      if (target.value("kind", std::string()) == "local" &&
          target.value("original", std::string()) == original &&
          target.value("identity", std::string()) == identity)
        occurrences.emplace_back(target.at("begin_byte").get<std::size_t>(),
                                 target.at("end_byte").get<std::size_t>());
    if (occurrences.empty())
      throw Error(
          "stale_target",
          "The selected variable no longer belongs to this source view");
    if (name.empty() || name == original)
      return;
    for (const auto &token : tokens(view.at("text").get<std::string>()))
      if (token.text == name &&
          std::none_of(
              occurrences.begin(), occurrences.end(), [&](const auto &span) {
                return token.begin == span.first && token.end == span.second;
              }))
        throw Error("invalid_request",
                    "This name already occurs in the function");
  } else if (request.value("kind", std::string()) == "comment") {
    const auto line = sizeField(
        request, "line", 0,
        ::neverd::limits::worker::
            kCodeEditsValidateChangeNameCommentKindLineLineSizeFieldMax);
    const auto anchor = stringField(
        request, "anchor", {},
        ::neverd::limits::worker::
            kCodeEditsValidateChangeNameCommentKindAnchorStringFieldMax);
    for (const auto &row : view.at("rows"))
      if (row.value("line", std::size_t{0}) == line &&
          row.value("code_anchor", std::string()) == anchor)
        return;
    throw Error("stale_target",
                "The selected line no longer belongs to this source view");
  } else if (request.value("kind", std::string()) == "number") {
    const auto line = sizeField(
        request, "line", 0,
        ::neverd::limits::worker::
            kCodeEditsValidateChangeNameCommentNumberKindLineLineSizeFieldMax);
    const auto index = sizeField(
        request, "index", 0,
        ::neverd::limits::worker::
            kCodeEditsValidateChangeNameCommentNumberKindIndexSizeFieldMax);
    const auto anchor = stringField(
        request, "anchor", {},
        ::neverd::limits::worker::
            kCodeEditsValidateChangeNameCommentNumberKindAnchorStringFieldMax);
    const auto radix = literalRadixNamed(stringField(
        request, "radix", {},
        ::neverd::limits::worker::
            kCodeEditsValidateChangeNameCommentNumberKindRadixStringFieldMax));
    if (!radix)
      throw Error("invalid_request", "Unknown number base");
    if (std::none_of(view.at("rows").begin(), view.at("rows").end(),
                     [&](const Json &row) {
                       return row.value("line", std::size_t{0}) == line &&
                              row.value("code_anchor", std::string()) == anchor;
                     }))
      throw Error("stale_target",
                  "The selected line no longer belongs to this source view");
    // The anchor is the line as the engine wrote it; the shown text before
    // it says whether it starts in a comment, which decoration never changes.
    const auto syntax = literalSyntax(
        view,
        stringField(
            request, "representation", {},
            ::neverd::limits::worker::
                kCodeEditsValidateChangeNameCommentNumberKindSyntaxRepresentationStringFieldMax));
    bool inComment = commentStates(
        view.at("text").get_ref<const std::string &>(), syntax, {line})[line];
    const auto literals = integerLiterals(anchor, syntax, inComment);
    if (index >= literals.size())
      throw Error("stale_target",
                  "The selected number no longer belongs to this source view");
    if (*radix != LiteralRadix::Number &&
        !spellIntegerLiteral(literals[index], *radix, syntax))
      throw Error("invalid_request",
                  "This number has no spelling in that base with its type");
  }
}

void CodeEdits::spellIdentifierSymbols(Json &view) {
  const auto listed = view.find("identifier_symbols");
  if (listed == view.end() || !listed->is_array() || listed->empty() ||
      !view.contains("text") || !view["text"].is_string())
    return;
  std::unordered_map<std::string, const Json *> named;
  for (const auto &name : *listed)
    if (name.is_object() && name.contains("identifier") &&
        name["identifier"].is_string() && name.contains("symbol") &&
        name["symbol"].is_string() && name.contains("address") &&
        name["address"].is_string() &&
        identifier(name["identifier"].get<std::string>()))
      named.emplace(name["identifier"].get<std::string>(), &name);
  if (named.empty())
    return;
  if (!view.contains("source_names") || !view["source_names"].is_array())
    view["source_names"] = Json::array();
  auto &names = view["source_names"];
  // A dialect's own names keep their spans.
  std::vector<std::pair<std::size_t, std::size_t>> spelled;
  for (const auto &name : names)
    spelled.emplace_back(name.value("begin_byte", std::size_t{0}),
                         name.value("end_byte", std::size_t{0}));
  const SourceSpanIndex covered(spelled.begin(), spelled.end());
  const auto &text = view["text"].get_ref<const std::string &>();
  for (const auto &token : tokens(text)) {
    const auto found = named.find(token.text);
    if (found == named.end() || covered.overlaps(token.begin, token.end))
      continue;
    const Json &name = *found->second;
    names.push_back({{"begin_byte", token.begin},
                     {"end_byte", token.end},
                     {"identifier", token.text},
                     {"symbol", name["symbol"]},
                     {"address", hexAddress(parseAddress(
                                     name["address"].get<std::string>()))}});
  }
  std::sort(names.begin(), names.end(), [](const Json &a, const Json &b) {
    return a.value("begin_byte", std::size_t{0}) <
           b.value("begin_byte", std::size_t{0});
  });
}

void CodeEdits::decorate(Json &view, const Json &row, std::uint64_t function,
                         const std::string &representation, const Json &renames,
                         const Json &annotations,
                         const FunctionAliases &displayNames,
                         const Resolver &resolve) {
  if (!source(representation))
    return;
  const auto original = view.at("text").get<std::string>();
  const auto identity = sourceIdentity(original);
  const auto ts = tokens(original);
  std::set<std::string> identifiers;
  for (const auto &token : ts)
    if (identifier(token.text))
      identifiers.insert(token.text);
  std::unordered_map<std::string, std::uint64_t> globals;
  static const std::regex imageMarker(
      R"(^\s*/\*\s*neverd\.image:\s*(0x[0-9A-Fa-f]+)\b)");
  static const std::regex trailingAddress(
      R"(;\s*/\*\s*(0x[0-9A-Fa-f]+)\s*\*/\s*$)");
  static const std::regex declared(
      R"(\b([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*[=;])");
  std::optional<std::uint64_t> pending;
  for (std::size_t start = 0; start < original.size();) {
    const auto newline = original.find('\n', start);
    const auto line =
        original.substr(start, newline == std::string::npos ? std::string::npos
                                                            : newline - start);
    std::smatch match;
    if (line.find("neverd.image:") != std::string::npos &&
        std::regex_search(line, match, imageMarker,
                          std::regex_constants::match_continuous))
      pending = parseAddress(match[1].str());
    else {
      auto address = std::exchange(pending, std::nullopt);
      if (!address && line.find("extern") != std::string::npos &&
          std::regex_search(line, match, trailingAddress))
        address = parseAddress(match[1].str());
      if (address && std::regex_search(line, match, declared))
        globals[match[1].str()] = *address;
    }
    if (newline == std::string::npos)
      break;
    start = newline + 1;
  }
  const auto links = sourceLinks(original, ts);
  std::unordered_map<std::string, std::uint64_t> originalAddresses;
  for (const auto &name : renames)
    originalAddresses[name.value("original", std::string())] =
        parseAddress(name.at("addr").get<std::string>());
  std::unordered_map<std::string, std::optional<std::uint64_t>> resolved;
  const auto imageAddress = [&](const std::string &name) {
    const auto symbol = links.contains(name) ? links.at(name) : name;
    if (!resolved.contains(symbol))
      resolved[symbol] =
          originalAddresses.contains(symbol)
              ? std::optional<std::uint64_t>(originalAddresses.at(symbol))
              : resolve(symbol);
    return resolved.at(symbol);
  };
  const auto prelude = view.value("prelude", Json::object());
  const std::size_t bodyStart = prelude.value("end_byte", std::size_t{0});
  std::optional<std::size_t> definition;
  std::size_t body = ts.size();
  std::size_t bodyEnd = ts.size();
  for (std::size_t i = 0; i + 1 < ts.size(); ++i) {
    if (ts[i].begin < bodyStart || !identifier(ts[i].text) ||
        ts[i + 1].text != "(")
      continue;
    int depth = 0;
    auto end = i + 1;
    for (; end < ts.size(); ++end) {
      if (ts[end].text == "(")
        ++depth;
      else if (ts[end].text == ")" && --depth == 0)
        break;
    }
    auto brace = end + 1;
    while (brace < ts.size() &&
           brace - end <= ::neverd::limits::worker::kMaxSourceDecoratorTokens &&
           ts[brace].text != "{" && ts[brace].text != ";" &&
           ts[brace].text != "=")
      ++brace;
    if (brace < ts.size() &&
        brace - end <= ::neverd::limits::worker::kMaxSourceDecoratorTokens &&
        ts[brace].text == "{") {
      // A prelude can be followed by synthetic helper definitions. Only a
      // resolved image name or an explicit SDK name span identifies this body.
      const auto at = imageAddress(ts[i].text);
      const auto sourceNames = view.value("source_names", Json::array());
      // The engine spells addresses in upper case.
      const bool explicitName = std::any_of(
          sourceNames.begin(), sourceNames.end(), [&](const Json &name) {
            return name.value("begin_byte", original.size()) <= ts[i].begin &&
                   name.value("end_byte", std::size_t{0}) >= ts[i].end &&
                   name.contains("address") && name["address"].is_string() &&
                   parseAddress(name["address"].get<std::string>()) == function;
          });
      if (at == function || explicitName) {
        definition = i;
        body = i + 2;
        int braces = 1;
        for (auto at = brace + 1; at < ts.size(); ++at) {
          if (ts[at].text == "{")
            ++braces;
          else if (ts[at].text == "}" && --braces == 0) {
            bodyEnd = at;
            break;
          }
        }
        break;
      }
    }
  }
  std::unordered_map<std::string, unsigned> declarations;
  std::set<std::string> types;
  for (std::size_t i = 0; i + 1 < ts.size(); ++i) {
    if (ts[i].text == "struct" || ts[i].text == "class" ||
        ts[i].text == "union" || ts[i].text == "enum" ||
        ts[i].text == "using") {
      if (identifier(ts[i + 1].text))
        types.insert(ts[i + 1].text);
    } else if (ts[i].text == "typedef") {
      auto end = i + 1;
      while (end < ts.size() && ts[end].text != ";")
        ++end;
      if (end > i + 1 && identifier(ts[end - 1].text))
        types.insert(ts[end - 1].text);
    }
  }
  const auto isType = [&](const std::string &name) {
    return type(name) || types.contains(name);
  };
  for (std::size_t i = body; i + 1 < bodyEnd; ++i) {
    if (!identifier(ts[i].text) || member(ts, i))
      continue;
    auto previous = i;
    while (previous &&
           (ts[previous - 1].text == "*" || ts[previous - 1].text == "&"))
      --previous;
    const bool typed = previous && isType(ts[previous - 1].text) &&
                       (ts[i + 1].text == "=" || ts[i + 1].text == ";" ||
                        ts[i + 1].text == "," || ts[i + 1].text == ")" ||
                        ts[i + 1].text == "[");
    const bool binding =
        i && (ts[i - 1].text == "let" || ts[i - 1].text == "mut" ||
              ts[i - 1].text == "var");
    if ((typed || binding) && ts[i].text != "mut")
      ++declarations[ts[i].text];
  }
  // The function's parameters by position, as its definition declares
  // them, and what its locals are: their names in the order the code
  // declares them, and how many parameters it takes.  A type change, its
  // own or a callee's, rewrites the text but keeps them.
  std::unordered_map<std::string, std::size_t> parameters;
  std::string shape;
  if (definition) {
    int depth = 0;
    std::size_t position = 0;
    std::optional<std::string> named;
    auto at = *definition + 1;
    for (; at < bodyEnd; ++at) {
      const auto &text = ts[at].text;
      if (text == "(" && ++depth == 1)
        continue;
      if (text == ")" && --depth == 0)
        break;
      if (depth == 1 && text == ",") {
        if (named)
          parameters.emplace(*named, position);
        named.reset();
        ++position;
      } else if (depth == 1 && identifier(text) && declarations.contains(text))
        named = text;
    }
    if (named)
      parameters.emplace(*named, position);
    std::set<std::string> seen;
    for (auto i = at; i + 1 < bodyEnd; ++i)
      if (identifier(ts[i].text) && declarations.contains(ts[i].text) &&
          !parameters.contains(ts[i].text) && !member(ts, i) &&
          seen.insert(ts[i].text).second)
        shape += ts[i].text + ",";
    shape = sourceIdentity(shape + "|" + std::to_string(parameters.size()));
  }
  if (!shape.empty())
    view["code_shape"] = shape;
  Json edits = Json::object();
  if (!row.is_null())
    edits = row.at("views").value(representation, Json::object());
  // Names given in another text of the same code still name its locals.
  const bool stale =
      !edits.value("names", Json::object()).empty() &&
      edits.value("identity", std::string()) != identity &&
      (shape.empty() || edits.value("shape", std::string()) != shape);
  const auto aliases =
      stale ? Json::object() : edits.value("names", Json::object());
  view["code_edits_stale"] = stale;
  const auto notes = edits.value("comments", Json::object());
  std::unordered_map<std::uint64_t, std::string> imageNames, imageComments;
  for (const auto &name : renames)
    imageNames[parseAddress(name.at("addr").get<std::string>())] =
        name.at("renamed").get<std::string>();
  for (const auto &note : annotations)
    imageComments[parseAddress(note.at("addr").get<std::string>())] =
        note.at("text").get<std::string>();
  // An image name shows the analyst's rename of its address, otherwise the
  // listing's display name for how the engine spelled it, as main for the
  // sub_ name of the function the C runtime's start routine calls.
  const auto shownName =
      [&](std::uint64_t address,
          const std::string &spelling) -> std::optional<std::string> {
    if (const auto rename = imageNames.find(address);
        rename != imageNames.end())
      return rename->second;
    if (const auto display = displayNames.find(spelling);
        display != displayNames.end())
      return display->second;
    return std::nullopt;
  };
  struct Replacement {
    std::size_t begin, end;
    std::string text;
  };
  std::vector<Replacement> replacements;
  Json names = Json::array();
  std::set<std::pair<std::size_t, std::size_t>> spelled;
  for (const auto &name : view.value("source_names", Json::array())) {
    const auto begin = name.value("begin_byte", std::size_t{0});
    const auto end = name.value("end_byte", std::size_t{0});
    if (end <= begin || end > original.size())
      continue;
    const auto at =
        name.contains("address") && name["address"].is_string()
            ? std::optional<std::uint64_t>(
                  parseAddress(name["address"].get<std::string>()))
            : imageAddress(name.value("symbol",
                                      name.value("identifier", std::string())));
    if (!at)
      continue;
    const auto following = spelled.lower_bound({begin, 0});
    if ((following != spelled.end() && following->first < end) ||
        (following != spelled.begin() && std::prev(following)->second > begin))
      throw Error("invalid_engine_result", "Overlapping source name spans");
    names.push_back({{"begin_byte", begin},
                     {"end_byte", end},
                     {"kind", "address"},
                     {"address", hexAddress(*at)},
                     {"original", original.substr(begin, end - begin)}});
    spelled.emplace(begin, end);
    if (const auto shown = shownName(*at, original.substr(begin, end - begin)))
      replacements.push_back({begin, end, *shown});
  }
  const SourceSpanIndex covered(spelled.begin(), spelled.end());
  // A name the view calls or declares as a function names that function at
  // every use, also where the code takes its address: in an initializer, or
  // as an argument.  Each use then shows the name its declaration shows.
  std::set<std::string> functionNames;
  std::map<std::string, Json> sourceVariables;
  std::set<std::string> ambiguousVariables;
  for (const auto &variable : view.value("source_variables", Json::array())) {
    const auto name = variable.value("name", std::string());
    if (variable.value("ambiguous", true) ||
        !sourceVariables.emplace(name, variable).second)
      ambiguousVariables.insert(name);
  }
  for (std::size_t i = 0; i + 1 < ts.size(); ++i)
    if (identifier(ts[i].text) && ts[i + 1].text == "(" && !member(ts, i))
      functionNames.insert(ts[i].text);
  for (std::size_t i = 0; i < ts.size(); ++i) {
    const auto &token = ts[i];
    if (covered.contains(token.begin, token.end))
      continue;
    if (!identifier(token.text) || member(ts, i))
      continue;
    // Most identifiers name nothing (a hex literal's digits read as one), so
    // a name is filled in only once a kind below claims the identifier.
    Json name = Json::object();
    std::string display = token.text;
    if (definition == i) {
      name["address"] = hexAddress(function);
      name["kind"] = "address";
      if (const auto shown = shownName(function, token.text))
        display = *shown;
    } else if (i >= body && i < bodyEnd && declarations[token.text] == 1) {
      name["kind"] = "local";
      const auto found = sourceVariables.find(token.text);
      const Json *binding = found != sourceVariables.end() &&
                                    !ambiguousVariables.contains(token.text)
                                ? &found->second
                                : nullptr;
      if (binding && !binding->value("parameter", true)) {
        name["variable_id"] = binding->at("id");
        name["snapshot"] = view.value("variable_snapshot", std::string());
      }
      name["identity"] = identity;
      if (const auto parameter = parameters.find(token.text);
          parameter != parameters.end())
        name["parameter"] = parameter->second;
      if (aliases.contains(token.text))
        display = aliases.at(token.text).get<std::string>();
    } else if (globals.contains(token.text) ||
               (sourceVariables.contains(token.text) &&
                sourceVariables.at(token.text).contains("address"))) {
      name["kind"] = "address";
      const auto address = globals.contains(token.text)
                               ? globals.at(token.text)
                               : parseAddress(sourceVariables.at(token.text)
                                                  .at("address")
                                                  .get<std::string>());
      name["address"] = hexAddress(address);
      name["object"] = true;
      if (const auto shown = shownName(address, token.text))
        display = *shown;
    } else if ((i + 1 < ts.size() &&
                (ts[i + 1].text == "(" || links.contains(token.text))) ||
               functionNames.contains(token.text)) {
      const auto at = imageAddress(token.text);
      if (!at)
        continue;
      name["kind"] = "address";
      name["address"] = hexAddress(*at);
      if (const auto shown = shownName(*at, token.text))
        display = *shown;
    } else
      continue;
    if (!name.contains("variable_id")) {
      const auto binding = sourceVariables.find(token.text);
      if (binding != sourceVariables.end() &&
          !ambiguousVariables.contains(token.text) &&
          binding->second.contains("address")) {
        name["variable_id"] = binding->second.at("id");
        name["snapshot"] = view.value("variable_snapshot", std::string());
      }
    }
    name["begin_byte"] = token.begin;
    name["end_byte"] = token.end;
    name["original"] = token.text;
    if (display != token.text) {
      // Never collide a local alias with any other identifier in the view.
      if (name.at("kind") == "local" && identifiers.contains(display))
        continue;
      replacements.push_back({token.begin, token.end, display});
    }
    names.push_back(std::move(name));
  }
  std::vector<std::size_t> starts{0};
  for (std::size_t i = 0; i + 1 < original.size(); ++i)
    if (original[i] == '\n')
      starts.push_back(i + 1);
  // The bases the user picked for numbers, on lines that still read as their
  // anchors.
  const auto numbers = edits.value("numbers", Json::object());
  const auto syntax = literalSyntax(view, representation);
  std::set<std::size_t> formatted;
  for (const auto &[line, entry] : numbers.items())
    formatted.insert(std::stoull(line));
  const auto startsInComment = formatted.empty()
                                   ? std::unordered_map<std::size_t, bool>()
                                   : commentStates(original, syntax, formatted);
  for (auto &mapping : view["rows"]) {
    const auto line = mapping.value("line", std::size_t{0});
    if (line >= starts.size())
      continue;
    auto end = original.find('\n', starts[line]);
    if (end == std::string::npos)
      end = original.size();
    const auto anchor = original.substr(starts[line], end - starts[line]);
    mapping["code_anchor"] = anchor;
    if (const auto entry = numbers.find(std::to_string(line));
        entry != numbers.end() &&
        entry->value("anchor", std::string()) == anchor) {
      const auto state = startsInComment.find(line);
      bool inComment = state != startsInComment.end() && state->second;
      const auto literals = integerLiterals(anchor, syntax, inComment);
      const auto &radixes = entry->at("radixes");
      for (const auto &[index, radix] : radixes.items()) {
        const auto at = std::stoull(index);
        const auto base = literalRadixNamed(radix.get<std::string>());
        if (at >= literals.size() || !base)
          continue;
        const auto &literal = literals[at];
        if (const auto spelled = spellIntegerLiteral(literal, *base, syntax);
            spelled && *spelled != literal.Text)
          replacements.push_back({starts[line] + literal.Begin,
                                  starts[line] + literal.End, *spelled});
      }
      // The GUI shows which base each number of the line was given.
      mapping["code_number_radixes"] = radixes;
    }
    std::string comment;
    if (const auto note = notes.find(std::to_string(line));
        note != notes.end() && note->value("anchor", std::string()) == anchor)
      comment = note->value("text", std::string());
    mapping["code_comment_is_source"] = !comment.empty();
    if (comment.empty()) {
      for (const auto &at : mapping.value("addresses", Json::array())) {
        const auto address = parseAddress(at.get<std::string>());
        if (imageComments.contains(address)) {
          comment = imageComments.at(address);
          break;
        }
      }
      if (comment.empty() && definition &&
          ts[*definition].begin >= starts[line] &&
          ts[*definition].begin <= end && imageComments.contains(function))
        comment = imageComments.at(function);
    }
    mapping["code_comment"] = comment;
    if (!comment.empty())
      replacements.push_back({end, end, inlineComment(comment)});
  }
  std::sort(replacements.begin(), replacements.end(),
            [](const auto &a, const auto &b) { return a.begin < b.begin; });
  std::vector<std::pair<std::size_t, std::ptrdiff_t>> shifts;
  std::ptrdiff_t delta = 0;
  for (const auto &replacement : replacements) {
    delta += static_cast<std::ptrdiff_t>(replacement.text.size()) -
             static_cast<std::ptrdiff_t>(replacement.end - replacement.begin);
    shifts.emplace_back(replacement.end, delta);
  }
  const auto shifted = [&](std::size_t at) {
    const auto following =
        std::upper_bound(shifts.begin(), shifts.end(), at,
                         [](std::size_t position, const auto &shift) {
                           return position < shift.first;
                         });
    const auto delta =
        following == shifts.begin() ? 0 : std::prev(following)->second;
    return static_cast<std::size_t>(static_cast<std::ptrdiff_t>(at) + delta);
  };
  auto remap = [&](Json &span) {
    for (const char *field : {"begin_byte", "end_byte"})
      if (span.contains(field))
        span[field] = shifted(span[field].get<std::size_t>());
  };
  for (auto &name : names)
    remap(name);
  for (auto &name : view["source_names"])
    remap(name);
  for (auto &region : view["library_regions"])
    for (auto &span : region["spans"])
      remap(span);
  if (view.contains("prelude") && view["prelude"].contains("end_byte"))
    view["prelude"]["end_byte"] =
        shifted(view["prelude"]["end_byte"].get<std::size_t>());
  std::string decorated;
  std::size_t cursor = 0;
  for (const auto &r : replacements) {
    decorated.append(original, cursor, r.begin - cursor);
    decorated += r.text;
    cursor = r.end;
  }
  decorated.append(original, cursor, original.size() - cursor);
  view["text"] = std::move(decorated);
  view["code_names"] = std::move(names);
}
} // namespace neverd::worker
