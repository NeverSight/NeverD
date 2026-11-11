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
#include <tuple>
#include <unordered_map>
#include <unordered_set>
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
bool identifier(std::string_view name) {
  if (name.empty() ||
      name.size() >
          ::neverd::limits::worker::kCodeEditsIdentifierNameSizeLimit ||
      (name[0] >= '0' && name[0] <= '9'))
    return false;
  return std::all_of(name.begin(), name.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
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
/// Whether tokens(text) may hold a token that reads \p name, without making
/// them: a character wherever it is, and an identifier where it ends a word
/// and does not continue one.  A word inside a comment or a string counts too,
/// which tokens() skips.
bool mayHoldToken(const std::string &text, const std::string &name) {
  const auto word = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
  };
  if (name.size() == 1)
    return text.find(name[0]) != std::string::npos;
  if (name.empty() ||
      !(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_') ||
      !std::all_of(name.begin(), name.end(), word))
    return false;
  for (auto at = text.find(name); at != std::string::npos;
       at = text.find(name, at + 1)) {
    const bool continues =
        at > 0 && (std::isalpha(static_cast<unsigned char>(text[at - 1])) ||
                   text[at - 1] == '_');
    const auto end = at + name.size();
    if (!continues && (end == text.size() || !word(text[end])))
      return true;
  }
  return false;
}

/// std::isspace, std::isalpha and std::isalnum as the "C" locale reads them,
/// which the worker never leaves, without a library call for each byte.
constexpr bool spaceByte(unsigned char c) {
  return c == ' ' || (c >= '\t' && c <= '\r');
}
constexpr bool letterByte(unsigned char c) {
  return (c | 0x20) >= 'a' && (c | 0x20) <= 'z';
}
constexpr bool wordByte(unsigned char c) {
  return letterByte(c) || (c >= '0' && c <= '9') || c == '_';
}

/// Calls \p emit with the begin and end of each token of \p text, as tokens()
/// reads them.
template <class Emit> void eachToken(const std::string &text, Emit &&emit) {
  for (std::size_t i = 0; i < text.size();) {
    if (spaceByte(static_cast<unsigned char>(text[i]))) {
      ++i;
      continue;
    }
    if (text[i] == '/' && i + 1 < text.size()) {
      if (text[i + 1] == '/') {
        const auto end = text.find('\n', i);
        i = end == std::string::npos ? text.size() : end;
        continue;
      }
      if (text[i + 1] == '*') {
        const auto end = text.find("*/", i + 2);
        i = end == std::string::npos ? text.size() : end + 2;
        continue;
      }
    }
    // C++ raw strings can contain quotes and identifier-looking text.
    auto raw = i;
    if (text[i] == 'u' || text[i] == 'U' || text[i] == 'L')
      for (const std::string_view prefix : {"u8R\"", "uR\"", "UR\"", "LR\""})
        if (text.compare(i, prefix.size(), prefix) == 0) {
          raw += prefix.size() - 2;
          break;
        }
    if (text[raw] == 'R' && text.compare(raw, 2, "R\"") == 0) {
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
    if (letterByte(static_cast<unsigned char>(text[begin])) ||
        text[begin] == '_')
      while (i < text.size() && wordByte(static_cast<unsigned char>(text[i])))
        ++i;
    emit(begin, i);
  }
}

std::vector<Token> tokens(const std::string &text) {
  std::vector<Token> result;
  eachToken(text, [&](std::size_t begin, std::size_t end) {
    result.push_back({begin, end, text.substr(begin, end - begin)});
  });
  return result;
}

/// A token read in place.
struct TokenView {
  std::size_t begin, end;
  std::string_view text;
};
/// tokens(), read in place.
std::vector<TokenView> tokenViews(const std::string &text) {
  std::vector<TokenView> result;
  eachToken(text, [&](std::size_t begin, std::size_t end) {
    result.push_back(
        {begin, end, std::string_view(text).substr(begin, end - begin)});
  });
  return result;
}

// Find compiler-spelled function linkage using the existing literal/comment
// aware tokens. A regex search across the entire document revisits long
// comments even when there is no asm label anywhere in the source.
template <class Tokens>
std::unordered_map<std::string, std::string>
sourceLinks(const std::string &text, const Tokens &ts) {
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
      links[std::string(ts[i].text)] = std::string(literal);
  }
  return links;
}
bool type(std::string_view name) {
  static const std::set<std::string, std::less<>> types = {
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
template <class Tokens> bool member(const Tokens &ts, std::size_t i) {
  return i && (ts[i - 1].text == "." ||
               (i > 1 && ts[i - 2].text == "-" && ts[i - 1].text == ">") ||
               (i > 1 && ts[i - 2].text == ":" && ts[i - 1].text == ":"));
}
struct Replacement {
  std::size_t begin, end;
  std::string text;
};
/// Where the bytes of a text stand once \p replacements, in order, are made.
class ByteShifts {
  std::vector<std::pair<std::size_t, std::ptrdiff_t>> shifts_;

public:
  explicit ByteShifts(const std::vector<Replacement> &replacements) {
    std::ptrdiff_t delta = 0;
    for (const auto &replacement : replacements) {
      delta += static_cast<std::ptrdiff_t>(replacement.text.size()) -
               static_cast<std::ptrdiff_t>(replacement.end - replacement.begin);
      shifts_.emplace_back(replacement.end, delta);
    }
  }
  std::size_t operator()(std::size_t at) const {
    const auto following =
        std::upper_bound(shifts_.begin(), shifts_.end(), at,
                         [](std::size_t position, const auto &shift) {
                           return position < shift.first;
                         });
    const auto delta =
        following == shifts_.begin() ? 0 : std::prev(following)->second;
    return static_cast<std::size_t>(static_cast<std::ptrdiff_t>(at) + delta);
  }
};
std::string replaced(const std::string &text,
                     const std::vector<Replacement> &replacements) {
  std::string result;
  std::size_t cursor = 0;
  for (const auto &r : replacements) {
    result.append(text, cursor, r.begin - cursor);
    result += r.text;
    cursor = r.end;
  }
  result.append(text, cursor, text.size() - cursor);
  return result;
}
/// The byte spans of a page, other than the names decorating adds.
void remapPageSpans(Json &view, const ByteShifts &shifted) {
  const auto remap = [&](Json &span) {
    for (const char *field : {"begin_byte", "end_byte"})
      if (span.contains(field))
        span[field] = shifted(span[field].get<std::size_t>());
  };
  for (auto &name : view["source_names"])
    remap(name);
  for (auto &region : view["library_regions"])
    for (auto &span : region["spans"])
      remap(span);
  if (view.contains("prelude") && view["prelude"].contains("end_byte"))
    view["prelude"]["end_byte"] =
        shifted(view["prelude"]["end_byte"].get<std::size_t>());
}
/// A global as a source text declares it, by a marker or an extern's trailing
/// address.
using DeclaredGlobal = CodeEdits::EngineText::Global;
std::vector<DeclaredGlobal> declaredGlobals(const std::string &text) {
  static const std::regex imageMarker(
      R"(^\s*/\*\s*neverd\.image:\s*(0x[0-9A-Fa-f]+)\b)");
  static const std::regex trailingAddress(
      R"(;\s*/\*\s*(0x[0-9A-Fa-f]+)\s*\*/\s*$)");
  static const std::regex declared(
      R"(\b([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*[=;])");
  std::vector<DeclaredGlobal> globals;
  std::optional<std::uint64_t> pending;
  bool bare = false;
  for (std::size_t start = 0; start < text.size();) {
    const auto newline = text.find('\n', start);
    // Each line is read in place.
    const char *first = text.data() + start;
    const char *last =
        text.data() + (newline == std::string::npos ? text.size() : newline);
    const std::string_view line(first, static_cast<std::size_t>(last - first));
    std::cmatch match;
    if (line.find("neverd.image:") != std::string_view::npos &&
        std::regex_search(first, last, match, imageMarker,
                          std::regex_constants::match_continuous)) {
      pending = parseAddress(match[1].str());
      // The address alone, or the range a backing holds from it.
      static const std::regex bareRest(
          R"(^(?:\s*\.\.\s*0x[0-9A-Fa-f]+)?\s*\*/\s*$)");
      bare = std::regex_match(match.suffix().str(), bareRest);
    } else {
      auto address = std::exchange(pending, std::nullopt);
      const bool marked = address.has_value();
      if (!address && line.find("extern") != std::string_view::npos &&
          std::regex_search(first, last, match, trailingAddress))
        address = parseAddress(match[1].str());
      if (address && std::regex_search(first, last, match, declared)) {
        auto before = match.prefix().str();
        if (before.starts_with("_Alignas("))
          before.erase(0, before.find(')') + 1);
        globals.push_back(
            {*address, match[1].str(),
             marked && bare && before.find('(') == std::string::npos});
      }
    }
    if (newline == std::string::npos)
      break;
    start = newline + 1;
  }
  return globals;
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

namespace {
/// What respelling a text of C makes of it: the spellings it replaces, the
/// name each address then shows, and each data's name before and after.
struct Respelling {
  std::vector<Replacement> replacements;
  std::unordered_map<std::uint64_t, std::string> shown;
  std::vector<std::tuple<std::uint64_t, std::string, std::string>> moved;
};
/// Respelling \p text, its body from \p bodyStart, its source names covering
/// \p sourceNames and its variables \p sourceVariables, under \p renamed, rows
/// of what data is called now: nothing unless the text shows a spelling is all
/// that moves (CodeEdits::respell).  \p declared, where given, is the globals
/// the text declares, as declaredGlobals reads them of it.
std::optional<Respelling>
respelling(const std::string &text, std::size_t bodyStart,
           const std::vector<std::pair<std::size_t, std::size_t>> &sourceNames,
           const Json &sourceVariables, const Json &renamed,
           const std::vector<DeclaredGlobal> *declared = nullptr) {
  const auto ts = tokenViews(text);
  // The identifiers it spells are asked of its tokens: the few names a rename
  // asks about read them faster than a set of them is made.
  const auto spells = [&](std::string_view name) {
    if (!identifier(name))
      return false;
    for (const auto &token : ts)
      if (token.text == name)
        return true;
    return false;
  };
  // A name another is numbered after: the allocator steps around it, so a
  // render under other names numbers it otherwise.
  const auto numberedAfter = [&](const std::string &name) {
    const auto prefix = name + "_";
    for (const auto &token : ts)
      if (token.text.starts_with(prefix) &&
          token.text.find_first_not_of("0123456789", prefix.size()) ==
              std::string_view::npos &&
          identifier(token.text))
        return true;
    return false;
  };
  const auto numbered = [&](const std::string &name) {
    const auto mark = name.rfind('_');
    return mark != std::string::npos && mark + 1 < name.size() &&
           name.find_first_not_of("0123456789", mark + 1) ==
               std::string::npos &&
           spells(std::string_view(name).substr(0, mark));
  };
  // What the C reads as a name of the function and not this global, which only
  // a render tells apart.
  std::set<std::string> others;
  for (const auto &variable : sourceVariables)
    if (!variable.contains("address"))
      others.insert(variable.value("name", std::string()));
  std::vector<DeclaredGlobal> read;
  if (!declared)
    read = declaredGlobals(text);
  const auto &globals = declared ? *declared : read;
  // How often the text spells \p spelled, an address as hexAddress writes it
  // ("0x" and lower case digits), in either case and not followed by another
  // hexadecimal digit.
  const auto spelledNumbers = [&](const std::string &spelled) {
    const auto at = [&](std::size_t from) {
      for (from = text.find(spelled[0], from);
           from != std::string::npos && from + spelled.size() <= text.size();
           from = text.find(spelled[0], from + 1)) {
        std::size_t k = 1;
        while (k < spelled.size() &&
               std::tolower(static_cast<unsigned char>(text[from + k])) ==
                   static_cast<unsigned char>(spelled[k]))
          ++k;
        if (k == spelled.size())
          return from;
      }
      return std::string::npos;
    };
    std::size_t numbers = 0;
    for (auto found = at(0); found != std::string::npos;
         found = at(found + spelled.size()))
      if (found + spelled.size() == text.size() ||
          !std::isxdigit(
              static_cast<unsigned char>(text[found + spelled.size()])))
        ++numbers;
    return numbers;
  };
  // Each name of data that moves and its new spelling, the first of each.
  std::vector<std::pair<std::string, std::string>> spellings;
  const auto spellingOf = [&](std::string_view name) {
    return std::find_if(spellings.begin(), spellings.end(),
                        [&](const auto &spelling) {
                          return std::string_view(spelling.first) == name;
                        });
  };
  std::unordered_map<std::uint64_t, std::string> shown;
  std::vector<std::tuple<std::uint64_t, std::string, std::string>> movedNames;
  std::set<std::string> taken;
  for (const auto &row : renamed) {
    const auto address = parseAddress(row.at("addr").get<std::string>());
    const auto name = row.at("name").get<std::string>();
    const auto isIt = [&](const DeclaredGlobal &global) {
      return global.address == address;
    };
    const auto global = std::find_if(globals.begin(), globals.end(), isIt);
    if (global == globals.end() || !global->plain ||
        std::count_if(globals.begin(), globals.end(), isIt) != 1 ||
        others.contains(global->name) || numberedAfter(global->name) ||
        numbered(global->name) || !identifier(name) || spells(name) ||
        others.contains(name) || numberedAfter(name) ||
        !taken.insert(name).second)
      return std::nullopt;
    // A constant of the address that printed as a number is one a name would
    // have the code spell as the object.
    if (spelledNumbers(hexAddress(address)) != 1)
      return std::nullopt;
    if (spellingOf(global->name) == spellings.end())
      spellings.emplace_back(global->name, name);
    shown.emplace(address, name);
    movedNames.emplace_back(address, global->name, name);
  }
  std::vector<Replacement> replacements;
  // By spelling: its tokens, and those of them in the prelude.
  std::vector<std::size_t> seen(spellings.size()), inPrelude(spellings.size());
  std::unordered_map<std::string, std::size_t> addressed;
  for (std::size_t i = 0; i < ts.size() && ts[i].begin < bodyStart; ++i)
    if (ts[i].text == "&" && i + 1 < ts.size() && identifier(ts[i + 1].text) &&
        (!i || ts[i - 1].text != "&"))
      addressed.emplace(std::string(ts[i + 1].text), 0);
  if (!addressed.empty()) {
    for (std::size_t i = 0; i < ts.size() && ts[i].begin < bodyStart; ++i)
      if (const auto name = addressed.find(std::string(ts[i].text));
          name != addressed.end())
        ++name->second;
    // An initializer may take the address of an object by a symbol the page
    // declares nothing by, which a user's name of it replaces.
    for (const auto &[name, count] : addressed)
      if (count < 2)
        return std::nullopt;
  }
  for (std::size_t i = 0; i < ts.size(); ++i) {
    const auto &token = ts[i];
    const auto to = spellingOf(token.text);
    if (to == spellings.end())
      continue;
    const auto index = static_cast<std::size_t>(to - spellings.begin());
    ++seen[index];
    if (member(ts, i))
      continue;
    if (token.begin < bodyStart)
      ++inPrelude[index];
    else if (i) {
      // A label, or a declaration of the body's.
      const auto before = ts[i - 1].text;
      if ((identifier(before) && before != "return" && before != "sizeof" &&
           before != "else" && before != "do" && before != "case") ||
          (i + 1 < ts.size() && ts[i + 1].text == ":"))
        return std::nullopt;
    }
    replacements.push_back({token.begin, token.end, to->second});
  }
  const auto word = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
  };
  for (std::size_t index = 0; index < spellings.size(); ++index) {
    const auto &from = spellings[index].first;
    // The declaration alone, and no comment or string that spells it.
    if (inPrelude[index] != 1)
      return std::nullopt;
    std::size_t spelled = 0;
    for (auto at = text.find(from); at != std::string::npos;
         at = text.find(from, at + from.size()))
      if ((at == 0 || !word(text[at - 1])) &&
          (at + from.size() == text.size() || !word(text[at + from.size()])))
        ++spelled;
    if (spelled != seen[index])
      return std::nullopt;
  }
  std::vector<std::pair<std::size_t, std::size_t>> moved;
  moved.reserve(replacements.size());
  for (const auto &replacement : replacements)
    moved.emplace_back(replacement.begin, replacement.end);
  const SourceSpanIndex covered(moved.begin(), moved.end());
  for (const auto &[begin, end] : sourceNames)
    if (covered.overlaps(begin, end))
      return std::nullopt;
  return Respelling{std::move(replacements), std::move(shown),
                    std::move(movedNames)};
}
/// \p view's variables at an address \p shown names, called so.
void renameVariables(
    Json &view, const std::unordered_map<std::uint64_t, std::string> &shown) {
  if (auto variables = view.find("source_variables");
      variables != view.end() && variables->is_array())
    for (auto &variable : *variables)
      if (const auto address = variable.find("address");
          address != variable.end() && address->is_string())
        if (const auto name =
                shown.find(parseAddress(address->get<std::string>()));
            name != shown.end())
          variable["name"] = name->second;
}
/// A list of \p view's, or none where it holds no list there.
const Json &listed(const Json &view, const char *field) {
  static const Json none = Json::array();
  const auto list = view.find(field);
  return list != view.end() && list->is_array() ? *list : none;
}
} // namespace

bool CodeEdits::respell(Json &page, const Json &renamed) {
  const auto found = page.find("text");
  const auto prelude = page.find("prelude");
  if (found == page.end() || !found->is_string() || prelude == page.end() ||
      page.value("dialect", std::string("c")) != "c")
    return false;
  const auto &text = found->get_ref<const std::string &>();
  const std::size_t bodyStart = prelude->value("end_byte", std::size_t{0});
  std::vector<std::pair<std::size_t, std::size_t>> spans;
  for (const auto &name : listed(page, "source_names"))
    spans.emplace_back(name.value("begin_byte", std::size_t{0}),
                       name.value("end_byte", std::size_t{0}));
  const auto respelled = respelling(text, bodyStart, spans,
                                    listed(page, "source_variables"), renamed);
  if (!respelled)
    return false;
  auto spelled = replaced(text, respelled->replacements);
  remapPageSpans(page, ByteShifts(respelled->replacements));
  page["text"] = std::move(spelled);
  renameVariables(page, respelled->shown);
  return true;
}

CodeEdits::EngineText CodeEdits::engineText(const Json &view) {
  EngineText engine;
  if (const auto text = view.find("text");
      text != view.end() && text->is_string())
    engine.text = text->get<std::string>();
  if (const auto prelude = view.find("prelude");
      prelude != view.end() && prelude->is_object())
    engine.bodyStart = prelude->value("end_byte", std::size_t{0});
  for (const auto &name : listed(view, "source_names"))
    engine.sourceNames.emplace_back(name.value("begin_byte", std::size_t{0}),
                                    name.value("end_byte", std::size_t{0}));
  return engine;
}

void CodeEdits::ImageFacts::read(const Json &renames, const Json &annotations) {
  originals.clear();
  names.clear();
  comments.clear();
  // By address, as the engine lists them: the last of a spelling is the one.
  for (const auto &row : renames) {
    const auto address = parseAddress(row.at("addr").get<std::string>());
    originals[row.value("original", std::string())] = address;
    names[address] = row.at("renamed").get<std::string>();
  }
  for (const auto &row : annotations)
    comments[parseAddress(row.at("addr").get<std::string>())] =
        row.at("text").get<std::string>();
}

void CodeEdits::ImageFacts::renamed(std::uint64_t address,
                                    const std::string &original,
                                    const std::string &name) {
  names[address] = name;
  const auto [held, added] = originals.try_emplace(original, address);
  if (!added)
    held->second = std::max(held->second, address);
}

void CodeEdits::ImageFacts::commented(std::uint64_t address,
                                      const std::string &text) {
  if (text.empty())
    comments.erase(address);
  else
    comments[address] = text;
}

namespace {
/// A token of a text decorate reads, in place, with what it asks of each most
/// often: whether it is an identifier, and the byte of a token one byte long.
struct ReadToken {
  std::size_t begin, end;
  std::string_view text;
  bool identifier;
  char byte;
};
std::vector<ReadToken> readTokens(const std::string &text) {
  std::vector<ReadToken> result;
  eachToken(text, [&](std::size_t begin, std::size_t end) {
    const std::string_view token(text.data() + begin, end - begin);
    result.push_back({begin, end, token, identifier(token),
                      token.size() == 1 ? token[0] : '\0'});
  });
  return result;
}
/// member() of tokens read in place.
bool member(const std::vector<ReadToken> &ts, std::size_t i) {
  return i && (ts[i - 1].byte == '.' ||
               (i > 1 && ts[i - 2].byte == '-' && ts[i - 1].byte == '>') ||
               (i > 1 && ts[i - 2].byte == ':' && ts[i - 1].byte == ':'));
}
} // namespace

void CodeEdits::decorate(Json &view, const Json &row, std::uint64_t function,
                         const std::string &representation,
                         const ImageFacts &image,
                         const FunctionAliases &displayNames,
                         const Resolver &resolve,
                         std::optional<std::size_t> *definitionLine,
                         EngineText *engine) {
  if (!source(representation))
    return;
  // A view's lists are read in place: each may hold a row for every line.
  static const Json noRows = Json::array();
  const auto listed = [&view](const char *field) -> const Json & {
    const auto list = view.find(field);
    return list != view.end() ? *list : noRows;
  };
  const auto &original = view.at("text").get_ref<const std::string &>();
  const auto identity = sourceIdentity(original);
  // The tokens are read in place: the text stays as it is until the decorated
  // one replaces it, last.
  const auto ts = readTokens(original);
  const auto is = [&ts](std::size_t at, char byte) {
    return ts[at].byte == byte;
  };
  // The identifiers the text spells, read where a local's alias could collide
  // with one.
  std::vector<std::string_view> identifiers;
  bool identifiersRead = false;
  const auto spellsIdentifier = [&](std::string_view name) {
    if (!identifiersRead) {
      identifiersRead = true;
      for (const auto &token : ts)
        if (token.identifier)
          identifiers.push_back(token.text);
      std::sort(identifiers.begin(), identifiers.end());
      identifiers.erase(std::unique(identifiers.begin(), identifiers.end()),
                        identifiers.end());
    }
    return std::binary_search(identifiers.begin(), identifiers.end(), name);
  };
  std::map<std::string, std::uint64_t, std::less<>> globals;
  {
    auto declared = declaredGlobals(original);
    // The text engineText read is this one: what it declares is read once.
    if (engine && engine->text == original)
      engine->globals = declared;
    for (auto &global : declared)
      globals[std::move(global.name)] = global.address;
  }
  const auto links = sourceLinks(original, ts);
  const auto linked = [&links](std::string_view name) -> const std::string * {
    if (links.empty())
      return nullptr;
    const auto link = links.find(std::string(name));
    return link != links.end() ? &link->second : nullptr;
  };
  const auto &originalAddresses = image.originals;
  std::map<std::string, std::optional<std::uint64_t>, std::less<>> resolved;
  const auto imageAddress =
      [&](std::string_view name) -> std::optional<std::uint64_t> {
    const auto *link = linked(name);
    const std::string_view symbol = link ? std::string_view(*link) : name;
    if (const auto known = resolved.find(symbol); known != resolved.end())
      return known->second;
    std::string key(symbol);
    const auto stated = originalAddresses.find(key);
    const auto at = stated != originalAddresses.end()
                        ? std::optional<std::uint64_t>(stated->second)
                        : resolve(key);
    resolved.emplace(std::move(key), at);
    return at;
  };
  const auto prelude = view.find("prelude");
  const std::size_t bodyStart =
      prelude != view.end() ? prelude->value("end_byte", std::size_t{0}) : 0;
  std::optional<std::size_t> definition;
  std::size_t body = ts.size();
  std::size_t bodyEnd = ts.size();
  for (std::size_t i = 0; i + 1 < ts.size(); ++i) {
    if (ts[i].begin < bodyStart || !ts[i].identifier || !is(i + 1, '('))
      continue;
    int depth = 0;
    auto end = i + 1;
    for (; end < ts.size(); ++end) {
      if (is(end, '('))
        ++depth;
      else if (is(end, ')') && --depth == 0)
        break;
    }
    auto brace = end + 1;
    while (brace < ts.size() &&
           brace - end <= ::neverd::limits::worker::kMaxSourceDecoratorTokens &&
           !is(brace, '{') && !is(brace, ';') && !is(brace, '='))
      ++brace;
    if (brace < ts.size() &&
        brace - end <= ::neverd::limits::worker::kMaxSourceDecoratorTokens &&
        is(brace, '{')) {
      // A prelude can be followed by synthetic helper definitions. Only a
      // resolved image name or an explicit SDK name span identifies this body.
      const auto at = imageAddress(ts[i].text);
      const auto &sourceNames = listed("source_names");
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
          if (is(at, '{'))
            ++braces;
          else if (is(at, '}') && --braces == 0) {
            bodyEnd = at;
            break;
          }
        }
        break;
      }
    }
  }
  std::unordered_map<std::string_view, unsigned> declarations;
  std::set<std::string_view> types;
  for (std::size_t i = 0; i + 1 < ts.size(); ++i) {
    if (!ts[i].identifier)
      continue;
    const auto text = ts[i].text;
    if (text == "struct" || text == "class" || text == "union" ||
        text == "enum" || text == "using") {
      if (ts[i + 1].identifier)
        types.insert(ts[i + 1].text);
    } else if (text == "typedef") {
      auto end = i + 1;
      while (end < ts.size() && !is(end, ';'))
        ++end;
      if (end > i + 1 && ts[end - 1].identifier)
        types.insert(ts[end - 1].text);
    }
  }
  const auto isType = [&](std::string_view name) {
    return type(name) || types.contains(name);
  };
  for (std::size_t i = body; i + 1 < bodyEnd; ++i) {
    if (!ts[i].identifier || member(ts, i))
      continue;
    const bool declares = is(i + 1, '=') || is(i + 1, ';') || is(i + 1, ',') ||
                          is(i + 1, ')') || is(i + 1, '[');
    auto previous = i;
    while (previous && (is(previous - 1, '*') || is(previous - 1, '&')))
      --previous;
    const bool typed = declares && previous && isType(ts[previous - 1].text);
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
  std::unordered_map<std::string_view, std::size_t> parameters;
  std::string shape;
  if (definition) {
    int depth = 0;
    std::size_t position = 0;
    std::optional<std::string_view> named;
    auto at = *definition + 1;
    for (; at < bodyEnd; ++at) {
      if (is(at, '(') && ++depth == 1)
        continue;
      if (is(at, ')') && --depth == 0)
        break;
      if (depth == 1 && is(at, ',')) {
        if (named)
          parameters.emplace(*named, position);
        named.reset();
        ++position;
      } else if (depth == 1 && ts[at].identifier &&
                 declarations.contains(ts[at].text))
        named = ts[at].text;
    }
    if (named)
      parameters.emplace(*named, position);
    std::unordered_set<std::string_view> seen;
    for (auto i = at; i + 1 < bodyEnd; ++i)
      if (ts[i].identifier && declarations.contains(ts[i].text) &&
          !parameters.contains(ts[i].text) && !member(ts, i) &&
          seen.insert(ts[i].text).second) {
        shape += ts[i].text;
        shape += ',';
      }
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
  const auto &imageNames = image.names;
  const auto &imageComments = image.comments;
  // An image name shows the analyst's rename of its address, otherwise the
  // listing's display name for how the engine spelled it, as main for the
  // sub_ name of the function the C runtime's start routine calls.
  const auto shownName =
      [&](std::uint64_t address,
          std::string_view spelling) -> std::optional<std::string> {
    if (const auto rename = imageNames.find(address);
        rename != imageNames.end())
      return rename->second;
    if (displayNames.empty())
      return std::nullopt;
    if (const auto display = displayNames.find(std::string(spelling));
        display != displayNames.end())
      return display->second;
    return std::nullopt;
  };
  std::vector<Replacement> replacements;
  Json names = Json::array();
  std::set<std::pair<std::size_t, std::size_t>> spelled;
  for (const auto &name : listed("source_names")) {
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
    auto spelling = original.substr(begin, end - begin);
    const auto shown = shownName(*at, spelling);
    names.push_back({{"begin_byte", begin},
                     {"end_byte", end},
                     {"kind", "address"},
                     {"address", hexAddress(*at)},
                     {"original", std::move(spelling)}});
    spelled.emplace(begin, end);
    if (shown)
      replacements.push_back({begin, end, *shown});
  }
  const SourceSpanIndex covered(spelled.begin(), spelled.end());
  // A name the view calls or declares as a function names that function at
  // every use, also where the code takes its address: in an initializer, or
  // as an argument.  Each use then shows the name its declaration shows.
  std::unordered_set<std::string_view> functionNames;
  std::map<std::string, Json, std::less<>> sourceVariables;
  std::set<std::string, std::less<>> ambiguousVariables;
  for (const auto &variable : listed("source_variables")) {
    const auto name = variable.value("name", std::string());
    if (variable.value("ambiguous", true) ||
        !sourceVariables.emplace(name, variable).second)
      ambiguousVariables.insert(name);
  }
  for (std::size_t i = 0; i + 1 < ts.size(); ++i)
    if (ts[i].identifier && is(i + 1, '(') && !member(ts, i))
      functionNames.insert(ts[i].text);
  // The address of an object the view names: a global it declares, else a
  // variable the engine placed at one.
  const auto objectAddress =
      [&](std::string_view name) -> std::optional<std::uint64_t> {
    if (const auto global = globals.find(name); global != globals.end())
      return global->second;
    if (const auto variable = sourceVariables.find(name);
        variable != sourceVariables.end() &&
        variable->second.contains("address"))
      return parseAddress(variable->second.at("address").get<std::string>());
    return std::nullopt;
  };
  std::optional<std::string> snapshot;
  const auto variableSnapshot = [&]() -> const std::string & {
    if (!snapshot)
      snapshot = view.value("variable_snapshot", std::string());
    return *snapshot;
  };
  for (std::size_t i = 0; i < ts.size(); ++i) {
    const auto &token = ts[i];
    if (!token.identifier || covered.contains(token.begin, token.end) ||
        member(ts, i))
      continue;
    // Most identifiers name nothing (a hex literal's digits read as one), so
    // a name is filled in only once a kind below claims the identifier.
    Json name;
    std::string display(token.text);
    if (definition == i) {
      name["address"] = hexAddress(function);
      name["kind"] = "address";
      if (const auto shown = shownName(function, token.text))
        display = *shown;
    } else if (i >= body && i < bodyEnd && [&] {
                 const auto declared = declarations.find(token.text);
                 return declared != declarations.end() && declared->second == 1;
               }()) {
      name["kind"] = "local";
      const auto found = sourceVariables.find(token.text);
      const Json *binding = found != sourceVariables.end() &&
                                    !ambiguousVariables.contains(token.text)
                                ? &found->second
                                : nullptr;
      if (binding && !binding->value("parameter", true)) {
        name["variable_id"] = binding->at("id");
        name["snapshot"] = variableSnapshot();
      }
      name["identity"] = identity;
      if (const auto parameter = parameters.find(token.text);
          parameter != parameters.end())
        name["parameter"] = parameter->second;
      if (const auto alias = aliases.find(display); alias != aliases.end())
        display = alias->get<std::string>();
    } else if (const auto object = objectAddress(token.text)) {
      name["kind"] = "address";
      name["address"] = hexAddress(*object);
      name["object"] = true;
      if (const auto shown = shownName(*object, token.text))
        display = *shown;
    } else if ((i + 1 < ts.size() && (is(i + 1, '(') || linked(token.text))) ||
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
        name["snapshot"] = variableSnapshot();
      }
    }
    name["begin_byte"] = token.begin;
    name["end_byte"] = token.end;
    name["original"] = std::string(token.text);
    if (display != token.text) {
      // Never collide a local alias with any other identifier in the view.
      if (name.at("kind") == "local" && spellsIdentifier(display))
        continue;
      replacements.push_back({token.begin, token.end, display});
    }
    names.push_back(std::move(name));
  }
  std::vector<std::size_t> starts{0};
  for (std::size_t i = 0; i + 1 < original.size(); ++i)
    if (original[i] == '\n')
      starts.push_back(i + 1);
  if (definitionLine && definition)
    *definitionLine = static_cast<std::size_t>(
        std::upper_bound(starts.begin(), starts.end(), ts[*definition].begin) -
        starts.begin() - 1);
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
    const std::string_view anchor(original.data() + starts[line],
                                  end - starts[line]);
    mapping["code_anchor"] = std::string(anchor);
    if (const auto entry = numbers.empty() ? numbers.end()
                                           : numbers.find(std::to_string(line));
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
    if (const auto note =
            notes.empty() ? notes.end() : notes.find(std::to_string(line));
        note != notes.end() && note->value("anchor", std::string()) == anchor)
      comment = note->value("text", std::string());
    mapping["code_comment_is_source"] = !comment.empty();
    if (comment.empty() && !imageComments.empty()) {
      if (const auto addresses = mapping.find("addresses");
          addresses != mapping.end())
        for (const auto &at : *addresses) {
          const auto address = parseAddress(at.get<std::string>());
          if (const auto noted = imageComments.find(address);
              noted != imageComments.end()) {
            comment = noted->second;
            break;
          }
        }
      if (comment.empty() && definition &&
          ts[*definition].begin >= starts[line] && ts[*definition].begin <= end)
        if (const auto noted = imageComments.find(function);
            noted != imageComments.end())
          comment = noted->second;
    }
    mapping["code_comment"] = comment;
    if (!comment.empty())
      replacements.push_back({end, end, inlineComment(comment)});
  }
  std::sort(replacements.begin(), replacements.end(),
            [](const auto &a, const auto &b) { return a.begin < b.begin; });
  // Replacements that write what their spans read already, one after another,
  // leave the text and every span as they are.
  bool rewrites = false;
  for (std::size_t at = 0; at < replacements.size() && !rewrites; ++at) {
    const auto &replacement = replacements[at];
    rewrites =
        original.compare(replacement.begin, replacement.end - replacement.begin,
                         replacement.text) != 0 ||
        (at && replacement.begin < replacements[at - 1].end);
  }
  if (rewrites) {
    const ByteShifts shifted(replacements);
    auto remap = [&](Json &span) {
      for (const char *field : {"begin_byte", "end_byte"})
        if (span.contains(field))
          span[field] = shifted(span[field].get<std::size_t>());
    };
    for (auto &name : names)
      remap(name);
    remapPageSpans(view, shifted);
    std::string decorated = replaced(original, replacements);
    view["text"] = std::move(decorated);
  }
  view["code_names"] = std::move(names);
}

bool CodeEdits::takeFunctionNames(Json &view,
                                  const std::vector<std::uint64_t> &renamed,
                                  const ImageFacts &image) {
  const auto text = view.find("text");
  const auto names = view.find("code_names");
  if (text == view.end() || !text->is_string() || names == view.end() ||
      !names->is_array())
    return false;
  const auto &decorated = text->get_ref<const std::string &>();
  // A name given up shows what the engine stated, which the view no longer
  // holds; a new name the text spells may link a word that linked nothing.
  for (const auto address : renamed) {
    const auto name = image.names.find(address);
    if (name == image.names.end() || mayHoldToken(decorated, name->second))
      return false;
  }
  std::vector<Replacement> replacements;
  for (const auto &name : *names) {
    const auto kind = name.find("kind");
    const auto address = name.find("address");
    if (kind == name.end() || !kind->is_string() ||
        kind->get_ref<const std::string &>() != "address" ||
        address == name.end() || !address->is_string())
      continue;
    const auto at = parseAddress(address->get_ref<const std::string &>());
    if (std::find(renamed.begin(), renamed.end(), at) == renamed.end())
      continue;
    const auto begin = name.value("begin_byte", std::size_t{0});
    const auto end = name.value("end_byte", std::size_t{0});
    if (end < begin || end > decorated.size())
      return false;
    const auto &shown = image.names.at(at);
    if (decorated.compare(begin, end - begin, shown) != 0)
      replacements.push_back({begin, end, shown});
  }
  if (replacements.empty())
    return true;
  std::sort(replacements.begin(), replacements.end(),
            [](const auto &a, const auto &b) { return a.begin < b.begin; });
  for (std::size_t i = 1; i < replacements.size(); ++i)
    if (replacements[i].begin < replacements[i - 1].end)
      return false;
  const ByteShifts shifted(replacements);
  for (auto &name : *names)
    for (const char *field : {"begin_byte", "end_byte"})
      if (const auto at = name.find(field); at != name.end())
        *at = shifted(at->get<std::size_t>());
  remapPageSpans(view, shifted);
  view["text"] = replaced(decorated, replacements);
  return true;
}

bool CodeEdits::takeComments(Json &view, std::uint64_t function,
                             std::optional<std::size_t> definitionLine,
                             const ImageFacts &image) {
  const auto text = view.find("text");
  const auto rows = view.find("rows");
  if (text == view.end() || !text->is_string() || rows == view.end() ||
      !rows->is_array())
    return false;
  const auto &decorated = text->get_ref<const std::string &>();
  std::vector<std::size_t> starts{0};
  for (std::size_t i = 0; i + 1 < decorated.size(); ++i)
    if (decorated[i] == '\n')
      starts.push_back(i + 1);
  // Each row's comment as decorate reads it: a source note where the row has
  // one, else the first of its addresses the image comments, else the
  // function's own on its definition's line.
  const auto &imageComments = image.comments;
  std::unordered_map<std::size_t, unsigned> rowsOnLine;
  std::vector<std::pair<Json *, std::string>> changed;
  for (auto &mapping : *rows) {
    const auto line = mapping.value("line", std::size_t{0});
    const auto kept = mapping.find("code_comment");
    if (line >= starts.size() || kept == mapping.end() || !kept->is_string())
      continue;
    ++rowsOnLine[line];
    if (mapping.value("code_comment_is_source", false))
      continue;
    std::string comment;
    if (!imageComments.empty()) {
      if (const auto addresses = mapping.find("addresses");
          addresses != mapping.end())
        for (const auto &at : *addresses) {
          const auto address = parseAddress(at.get<std::string>());
          if (imageComments.contains(address)) {
            comment = imageComments.at(address);
            break;
          }
        }
      if (comment.empty() && definitionLine == line &&
          imageComments.contains(function))
        comment = imageComments.at(function);
    }
    if (comment != kept->get_ref<const std::string &>())
      changed.emplace_back(&mapping, std::move(comment));
  }
  if (changed.empty())
    return true;
  std::vector<Replacement> replacements;
  for (const auto &[mapping, comment] : changed) {
    const auto line = mapping->value("line", std::size_t{0});
    if (rowsOnLine[line] != 1)
      return false;
    auto end = decorated.find('\n', starts[line]);
    if (end == std::string::npos)
      end = decorated.size();
    const auto &before =
        mapping->at("code_comment").get_ref<const std::string &>();
    const auto written = before.empty() ? std::string() : inlineComment(before);
    if (end - starts[line] < written.size() ||
        decorated.compare(end - written.size(), written.size(), written) != 0)
      return false;
    replacements.push_back(
        {end - written.size(), end,
         comment.empty() ? std::string() : inlineComment(comment)});
  }
  std::sort(replacements.begin(), replacements.end(),
            [](const auto &a, const auto &b) { return a.begin < b.begin; });
  const ByteShifts shifted(replacements);
  for (const auto &[mapping, comment] : changed)
    (*mapping)["code_comment"] = comment;
  if (const auto names = view.find("code_names");
      names != view.end() && names->is_array())
    for (auto &name : *names)
      for (const char *field : {"begin_byte", "end_byte"})
        if (name.contains(field))
          name[field] = shifted(name[field].get<std::size_t>());
  remapPageSpans(view, shifted);
  view["text"] = replaced(decorated, replacements);
  return true;
}

std::optional<CodeEdits::EngineText>
CodeEdits::takeDataNames(Json &view, const EngineText &engine,
                         const Json &renamed, const Json &row,
                         const std::string &representation) {
  const auto text = view.find("text");
  const auto names = view.find("code_names");
  const auto rows = view.find("rows");
  if (!source(representation) || text == view.end() || !text->is_string() ||
      names == view.end() || !names->is_array() || rows == view.end() ||
      !rows->is_array() || !view.contains("prelude") ||
      view.value("dialect", std::string("c")) != "c")
    return std::nullopt;
  // An identifier symbol of a new name makes a source name of it where the
  // text spells it (spellIdentifierSymbols).
  std::set<std::string> newNames;
  for (const auto &rename : renamed)
    newNames.insert(rename.at("name").get<std::string>());
  for (const auto &symbol : listed(view, "identifier_symbols"))
    if (symbol.is_object() && symbol.contains("identifier") &&
        symbol["identifier"].is_string() &&
        newNames.contains(symbol["identifier"].get<std::string>()))
      return std::nullopt;
  const auto &variables = listed(view, "source_variables");
  const auto respelled =
      respelling(engine.text, engine.bodyStart, engine.sourceNames, variables,
                 renamed, engine.globals ? &*engine.globals : nullptr);
  if (!respelled)
    return std::nullopt;
  // Each data's names before and after, by its address.
  std::unordered_map<std::uint64_t, std::pair<std::string, std::string>> moves;
  for (const auto &[address, from, to] : respelled->moved)
    moves.emplace(address, std::pair{from, to});
  // Decoration binds a name to the variable of that name alone: the data's
  // own must be called as the text declares it, and no other so.
  for (const auto &variable : variables) {
    const auto name = variable.value("name", std::string());
    const auto address = variable.find("address");
    const auto move =
        address != variable.end() && address->is_string()
            ? moves.find(parseAddress(address->get_ref<const std::string &>()))
            : moves.end();
    if (move != moves.end()) {
      if (name != move->second.first)
        return std::nullopt;
      continue;
    }
    for (const auto &[at, spelled] : moves)
      if (name == spelled.first || name == spelled.second)
        return std::nullopt;
  }
  auto spelled = replaced(engine.text, respelled->replacements);
  // Local names stay or go as the text's identity reads (decorate).
  Json edits = Json::object();
  if (!row.is_null())
    edits = row.at("views").value(representation, Json::object());
  const auto identity = sourceIdentity(spelled);
  const auto shape = view.value("code_shape", std::string());
  const bool stale =
      !edits.value("names", Json::object()).empty() &&
      edits.value("identity", std::string()) != identity &&
      (shape.empty() || edits.value("shape", std::string()) != shape);
  if (stale != view.value("code_edits_stale", false))
    return std::nullopt;
  // The lines the spellings change, whose rows anchor on what they read: a
  // number's base or a note anchored on one reads it otherwise.
  std::vector<std::size_t> starts{0};
  for (std::size_t i = 0; i + 1 < engine.text.size(); ++i)
    if (engine.text[i] == '\n')
      starts.push_back(i + 1);
  std::set<std::size_t> changed;
  std::unordered_map<std::string, std::size_t> spellings;
  for (const auto &replacement : respelled->replacements) {
    changed.insert(static_cast<std::size_t>(
        std::upper_bound(starts.begin(), starts.end(), replacement.begin) -
        starts.begin() - 1));
    ++spellings[engine.text.substr(replacement.begin,
                                   replacement.end - replacement.begin)];
  }
  const auto numbers = edits.value("numbers", Json::object());
  const auto notes = edits.value("comments", Json::object());
  for (const auto line : changed)
    if (numbers.contains(std::to_string(line)) ||
        notes.contains(std::to_string(line)))
      return std::nullopt;
  const ByteShifts engineShifts(respelled->replacements);
  std::unordered_map<std::size_t, std::string> anchors;
  for (const auto line : changed) {
    const auto start = engineShifts(starts[line]);
    auto end = spelled.find('\n', start);
    if (end == std::string::npos)
      end = spelled.size();
    anchors.emplace(line, spelled.substr(start, end - start));
  }
  // Every name of the data shows its new name; one the text spelled takes the
  // new spelling, and they are as many as respell replaces.
  const auto &decorated = text->get_ref<const std::string &>();
  std::vector<Replacement> replacements;
  std::vector<std::pair<Json *, const std::string *>> respelledNames;
  std::unordered_map<std::uint64_t, std::size_t> respelledCount;
  for (auto &name : *names) {
    const auto kind = name.find("kind");
    const auto address = name.find("address");
    if (kind == name.end() || !kind->is_string() ||
        kind->get_ref<const std::string &>() != "address" ||
        address == name.end() || !address->is_string())
      continue;
    const auto move =
        moves.find(parseAddress(address->get_ref<const std::string &>()));
    if (move == moves.end())
      continue;
    const auto begin = name.value("begin_byte", std::size_t{0});
    const auto end = name.value("end_byte", std::size_t{0});
    if (end < begin || end > decorated.size())
      return std::nullopt;
    const auto &[from, to] = move->second;
    if (decorated.compare(begin, end - begin, to) != 0)
      replacements.push_back({begin, end, to});
    if (name.value("original", std::string()) == from) {
      respelledNames.emplace_back(&name, &to);
      ++respelledCount[move->first];
    }
  }
  for (const auto &[address, spelling] : moves)
    if (respelledCount[address] != spellings[spelling.first])
      return std::nullopt;
  std::sort(replacements.begin(), replacements.end(),
            [](const auto &a, const auto &b) { return a.begin < b.begin; });
  for (std::size_t i = 1; i < replacements.size(); ++i)
    if (replacements[i].begin < replacements[i - 1].end)
      return std::nullopt;
  std::vector<Json *> anchored;
  for (auto &mapping : *rows)
    if (anchors.contains(mapping.value("line", std::size_t{0})))
      anchored.push_back(&mapping);
  // Nothing below refuses: the view takes it all.
  const ByteShifts shifted(replacements);
  for (const auto &[name, to] : respelledNames)
    (*name)["original"] = *to;
  for (auto &name : *names) {
    for (const char *field : {"begin_byte", "end_byte"})
      if (const auto at = name.find(field); at != name.end())
        *at = shifted(at->get<std::size_t>());
    if (const auto kind = name.find("kind");
        kind != name.end() && kind->is_string() &&
        kind->get_ref<const std::string &>() == "local" &&
        name.contains("identity"))
      name["identity"] = identity;
  }
  for (auto *mapping : anchored)
    (*mapping)["code_anchor"] =
        anchors.at(mapping->value("line", std::size_t{0}));
  remapPageSpans(view, shifted);
  renameVariables(view, respelled->shown);
  view["text"] = replaced(decorated, replacements);
  EngineText taken{std::move(spelled), engineShifts(engine.bodyStart), {}};
  taken.sourceNames.reserve(engine.sourceNames.size());
  for (const auto &[begin, end] : engine.sourceNames)
    taken.sourceNames.emplace_back(engineShifts(begin), engineShifts(end));
  return taken;
}
} // namespace neverd::worker
