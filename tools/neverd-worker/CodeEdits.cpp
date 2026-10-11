#include "CodeEdits.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <set>
#include <unordered_map>
#include <utility>

namespace neverd::worker {
namespace {
bool identifier(const std::string &name) {
  if (name.empty() || name.size() > 256 ||
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
    for (const std::string prefix : {"u8", "u", "U", "L"})
      if (text.compare(i, prefix.size() + 2, prefix + "R\"") == 0) {
        raw += prefix.size();
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
  if (!row.contains("views") || !row.at("views").is_object() ||
      row.at("views").size() > 6 || row.dump().size() > 4 * 1024 * 1024)
    throw Error("invalid_request", "Invalid bounded code-edit state");
  for (const auto &[representation, view] : row.at("views").items()) {
    if (!source(representation) || !view.is_object())
      throw Error("invalid_request", "Code edits need a source representation");
    const auto names = view.value("names", Json::object());
    if (!names.empty())
      (void)parseAddress(stringField(view, "identity"));
    const auto comments = view.value("comments", Json::object());
    if (!names.is_object() || !comments.is_object() || names.size() > 2048 ||
        comments.size() > 2048)
      throw Error("invalid_request", "Code-edit entries exceed their budget");
    std::set<std::string> aliases;
    for (const auto &[original, alias] : names.items())
      if (!identifier(original) || !alias.is_string() ||
          !aliasIdentifier(alias.get<std::string>()) ||
          !aliases.insert(alias.get<std::string>()).second)
        throw Error("invalid_request",
                    "Local names must be distinct identifiers");
    for (const auto &[line, note] : comments.items()) {
      if (line.empty() || line.size() > 10 ||
          !std::all_of(line.begin(), line.end(),
                       [](unsigned char c) { return std::isdigit(c); }) ||
          !note.is_object())
        throw Error("invalid_request", "Invalid code-comment anchor");
      (void)stringField(note, "anchor", {}, 65536);
      (void)stringField(note, "text", {}, 65536);
    }
  }
}

Json CodeEdits::change(Json row, const Json &request) {
  const auto representation = stringField(request, "representation", {}, 16);
  if (!source(representation))
    throw Error("invalid_request", "Code edits need a source representation");
  if (row.is_null())
    row = {{"addr", stringField(request, "address")},
           {"views", Json::object()}};
  auto &view = row["views"][representation];
  if (view.is_null())
    view = {{"names", Json::object()}, {"comments", Json::object()}};
  const auto kind = stringField(request, "kind", {}, 16);
  if (kind == "name") {
    const auto identity = stringField(request, "identity", {}, 32);
    (void)parseAddress(identity);
    if (view.value("identity", std::string()) != identity)
      view["names"] = Json::object();
    view["identity"] = identity;
    const auto original = stringField(request, "original", {}, 256);
    const auto name = stringField(request, "name", {}, 256);
    if (!identifier(original) ||
        (!name.empty() && name != original && !aliasIdentifier(name)))
      throw Error("invalid_request", "A local name must be an identifier");
    if (name.empty() || name == original)
      view["names"].erase(original);
    else
      view["names"][original] = name;
  } else if (kind == "comment") {
    const auto line = sizeField(request, "line", 0, 1000000);
    const auto text = stringField(request, "text", {}, 65536);
    const auto key = std::to_string(line);
    if (text.empty())
      view["comments"].erase(key);
    else
      view["comments"][key] = {
          {"anchor", stringField(request, "anchor", {}, 65536)},
          {"text", text}};
  } else
    throw Error("invalid_request", "Unknown code edit kind");
  validateRow(row);
  return row;
}

void CodeEdits::validateChange(const Json &view, const Json &request) {
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
    const auto line = sizeField(request, "line", 0, 1000000);
    const auto anchor = stringField(request, "anchor", {}, 65536);
    for (const auto &row : view.at("rows"))
      if (row.value("line", std::size_t{0}) == line &&
          row.value("code_anchor", std::string()) == anchor)
        return;
    throw Error("stale_target",
                "The selected line no longer belongs to this source view");
  }
}

void CodeEdits::decorate(Json &view, const Json &row, std::uint64_t function,
                         const std::string &representation, const Json &renames,
                         const Json &annotations, const Resolver &resolve) {
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
    while (brace < ts.size() && brace - end <= 32 && ts[brace].text != "{" &&
           ts[brace].text != ";" && ts[brace].text != "=")
      ++brace;
    if (brace < ts.size() && brace - end <= 32 && ts[brace].text == "{") {
      // A prelude can be followed by synthetic helper definitions. Only a
      // resolved image name or an explicit SDK name span identifies this body.
      const auto at = imageAddress(ts[i].text);
      const auto sourceNames = view.value("source_names", Json::array());
      const bool explicitName = std::any_of(
          sourceNames.begin(), sourceNames.end(), [&](const Json &name) {
            return name.value("begin_byte", original.size()) <= ts[i].begin &&
                   name.value("end_byte", std::size_t{0}) >= ts[i].end &&
                   name.value("address", std::string()) == hexAddress(function);
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
  Json edits = Json::object();
  if (!row.is_null())
    edits = row.at("views").value(representation, Json::object());
  const bool stale = !edits.value("names", Json::object()).empty() &&
                     edits.value("identity", std::string()) != identity;
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
  struct Replacement {
    std::size_t begin, end;
    std::string text;
  };
  std::vector<Replacement> replacements;
  Json names = Json::array();
  std::vector<std::pair<std::size_t, std::size_t>> spelled;
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
    if (std::any_of(spelled.begin(), spelled.end(), [&](const auto &span) {
          return begin < span.second && end > span.first;
        }))
      throw Error("invalid_engine_result", "Overlapping source name spans");
    names.push_back({{"begin_byte", begin},
                     {"end_byte", end},
                     {"kind", "address"},
                     {"address", hexAddress(*at)},
                     {"original", original.substr(begin, end - begin)}});
    spelled.emplace_back(begin, end);
    if (imageNames.contains(*at))
      replacements.push_back({begin, end, imageNames.at(*at)});
  }
  for (std::size_t i = 0; i < ts.size(); ++i) {
    const auto &token = ts[i];
    if (std::any_of(spelled.begin(), spelled.end(), [&](const auto &span) {
          return token.begin >= span.first && token.end <= span.second;
        }))
      continue;
    if (!identifier(token.text) || member(ts, i))
      continue;
    Json name = {{"begin_byte", token.begin},
                 {"end_byte", token.end},
                 {"original", token.text}};
    std::string display = token.text;
    if (definition == i) {
      name["address"] = hexAddress(function);
      name["kind"] = "address";
      if (imageNames.contains(function))
        display = imageNames.at(function);
    } else if (i >= body && i < bodyEnd && declarations[token.text] == 1) {
      name["kind"] = "local";
      name["identity"] = identity;
      if (aliases.contains(token.text))
        display = aliases.at(token.text).get<std::string>();
    } else if (globals.contains(token.text)) {
      name["kind"] = "address";
      name["address"] = hexAddress(globals.at(token.text));
      if (imageNames.contains(globals.at(token.text)))
        display = imageNames.at(globals.at(token.text));
    } else if (i + 1 < ts.size() &&
               (ts[i + 1].text == "(" || links.contains(token.text))) {
      const auto at = imageAddress(token.text);
      if (!at)
        continue;
      name["kind"] = "address";
      name["address"] = hexAddress(*at);
      if (imageNames.contains(*at))
        display = imageNames.at(*at);
    } else
      continue;
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
  for (auto &mapping : view["rows"]) {
    const auto line = mapping.value("line", std::size_t{0});
    if (line >= starts.size())
      continue;
    auto end = original.find('\n', starts[line]);
    if (end == std::string::npos)
      end = original.size();
    const auto anchor = original.substr(starts[line], end - starts[line]);
    mapping["code_anchor"] = anchor;
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
