//===- MobileDalvikSmali.cpp - Bounded native smali reader
//-----------------===//
#include "MobileDalvik.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/Regex.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <tuple>

namespace neverd::mobile::dalvik {
namespace {
[[noreturn]] void fail(std::string message) { throw Error(std::move(message)); }
bool space(unsigned char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
std::string trim(std::string_view text) {
  while (!text.empty() && space(uint8_t(text.front())))
    text.remove_prefix(1);
  while (!text.empty() && space(uint8_t(text.back())))
    text.remove_suffix(1);
  return std::string(text);
}
std::vector<std::string> words(std::string_view text) {
  std::vector<std::string> result;
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && space(uint8_t(text[i])))
      ++i;
    size_t start = i;
    while (i < text.size() && !space(uint8_t(text[i])))
      ++i;
    if (i > start)
      result.emplace_back(text.substr(start, i - start));
  }
  return result;
}
using Match = std::vector<std::string>;
// The patterns are fixed parser grammar, never input-supplied. LLVM's POSIX
// engine avoids the input-proportional C++ call stacks of std::regex. Map the
// handful of noncapturing groups and character shorthands explicitly.
bool match(const std::string &text, Match &result, const char *pattern) {
  std::string converted = "^(";
  std::vector<unsigned> groups{0};
  unsigned group = 1;
  bool character_class = false;
  std::string_view source(pattern);
  for (size_t i = 0; i < source.size(); ++i) {
    char c = source[i];
    if (c == '\\' && i + 1 < source.size()) {
      char next = source[++i];
      if (next == 'd')
        converted += character_class ? "0-9" : "[0-9]";
      else if (next == 's')
        converted += character_class ? "[:space:]" : "[[:space:]]";
      else if (next == 'S')
        converted += "[^[:space:]]";
      else {
        converted += '\\';
        converted += next;
      }
    } else {
      if (c == '[')
        character_class = true;
      else if (c == ']')
        character_class = false;
      else if (c == '(' && !character_class) {
        ++group;
        if (i + 2 < source.size() && source.substr(i + 1, 2) == "?:")
          i += 2;
        else
          groups.push_back(group);
      }
      converted += c;
    }
  }
  converted += ")$";
  llvm::Regex expression(converted);
  std::string error;
  if (!expression.isValid(error))
    fail("invalid internal smali grammar: " + error);
  llvm::SmallVector<llvm::StringRef, 8> captures;
  if (!expression.match(text, &captures))
    return false;
  result.clear();
  for (unsigned index : groups) {
    if (index >= captures.size())
      fail("inconsistent internal smali grammar capture");
    result.push_back(captures[index].str());
  }
  return true;
}
bool match(const std::string &text, const char *pattern) {
  Match ignored;
  return match(text, ignored, pattern);
}
uint32_t codepoint(std::string_view text, size_t &at) {
  if (at >= text.size())
    fail("truncated UTF-8 smali input");
  unsigned first = uint8_t(text[at++]);
  if (first < 0x80)
    return first;
  unsigned count;
  uint32_t value, minimum;
  if (first >= 0xc2 && first <= 0xdf) {
    count = 1;
    value = first & 31;
    minimum = 0x80;
  } else if (first >= 0xe0 && first <= 0xef) {
    count = 2;
    value = first & 15;
    minimum = 0x800;
  } else if (first >= 0xf0 && first <= 0xf4) {
    count = 3;
    value = first & 7;
    minimum = 0x10000;
  } else
    fail("invalid UTF-8 smali input");
  for (unsigned i = 0; i < count; ++i) {
    if (at >= text.size())
      fail("truncated UTF-8 smali input");
    unsigned byte = uint8_t(text[at++]);
    if ((byte & 0xc0) != 0x80)
      fail("invalid UTF-8 smali continuation");
    value = (value << 6) | (byte & 63);
  }
  if (value < minimum || value > 0x10ffff)
    fail("invalid UTF-8 smali code point");
  return value;
}
void appendUTF8(std::string &text, uint32_t cp) {
  if (cp < 0x80)
    text += char(cp);
  else if (cp < 0x800) {
    text += char(0xc0 | (cp >> 6));
    text += char(0x80 | (cp & 63));
  } else if (cp < 0x10000) {
    text += char(0xe0 | (cp >> 12));
    text += char(0x80 | ((cp >> 6) & 63));
    text += char(0x80 | (cp & 63));
  } else {
    text += char(0xf0 | (cp >> 18));
    text += char(0x80 | ((cp >> 12) & 63));
    text += char(0x80 | ((cp >> 6) & 63));
    text += char(0x80 | (cp & 63));
  }
}
bool nameValid(std::string_view name) {
  if (name.empty())
    return false;
  for (unsigned char c : name)
    if (space(c) || std::string_view(":;()/[]\"',{}=").find(char(c)) !=
                        std::string_view::npos)
      return false;
  return true;
}
bool labelValid(std::string_view label) {
  if (label.size() < 2 || label.front() != ':')
    return false;
  for (size_t i = 1; i < label.size();) {
    auto cp = codepoint(label, i);
    if (cp < 128 &&
        !((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
          (cp >= '0' && cp <= '9') ||
          std::string_view("_$.-").find(char(cp)) != std::string_view::npos))
      return false;
    if (cp >= 0xd800 && cp <= 0xdfff)
      return false;
  }
  return true;
}
std::string uncomment(std::string_view line) {
  char quote = 0;
  bool escape = false;
  for (size_t i = 0; i < line.size(); ++i) {
    char c = line[i];
    if (quote) {
      if (escape)
        escape = false;
      else if (c == '\\')
        escape = true;
      else if (c == quote)
        quote = 0;
    } else if (c == '\"' || c == '\'')
      quote = c;
    else if (c == '#')
      return trim(line.substr(0, i));
  }
  if (quote)
    fail("unterminated quoted smali literal");
  return trim(line);
}
std::vector<std::string> parts(std::string_view text) {
  std::vector<std::string> result;
  size_t start = 0;
  int level = 0;
  char quote = 0;
  bool escape = false;
  for (size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (quote) {
      if (escape)
        escape = false;
      else if (c == '\\')
        escape = true;
      else if (c == quote)
        quote = 0;
    } else if (c == '\"' || c == '\'')
      quote = c;
    else if (c == '{')
      ++level;
    else if (c == '}') {
      if (--level < 0)
        fail("unbalanced smali operand list");
    } else if (c == ',' && !level) {
      result.push_back(trim(text.substr(start, i - start)));
      start = i + 1;
    }
  }
  if (quote || level)
    fail("unbalanced smali operand list");
  result.push_back(trim(text.substr(start)));
  for (auto &part : result)
    if (part.empty())
      fail("empty smali operand");
  return result;
}
std::string decodeQuoted(std::string_view value, char quote = '\"') {
  if (value.size() < 2 || value.front() != quote || value.back() != quote)
    fail("expected a quoted smali literal");
  std::string result;
  size_t at = 1, units = 0;
  while (at < value.size() - 1) {
    uint32_t cp = codepoint(value, at);
    if (cp == unsigned(quote) || cp < 32)
      fail("invalid character in smali literal");
    if (cp == '\\') {
      if (at >= value.size() - 1)
        fail("unfinished smali escape");
      char escape = value[at++];
      if (escape == 'u') {
        if (value.size() - 1 - at < 4)
          fail("invalid smali Unicode escape");
        cp = 0;
        for (unsigned j = 0; j < 4; ++j) {
          char c = value[at++];
          unsigned n = c >= '0' && c <= '9'   ? c - '0'
                       : c >= 'a' && c <= 'f' ? c - 'a' + 10
                       : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                              : 99;
          if (n > 15)
            fail("invalid smali Unicode escape");
          cp = (cp << 4) | n;
        }
      } else {
        switch (escape) {
        case 'b':
          cp = '\b';
          break;
        case 't':
          cp = '\t';
          break;
        case 'n':
          cp = '\n';
          break;
        case 'f':
          cp = '\f';
          break;
        case 'r':
          cp = '\r';
          break;
        case '\'':
          cp = '\'';
          break;
        case '\"':
          cp = '\"';
          break;
        case '\\':
          cp = '\\';
          break;
        default:
          fail("unsupported smali escape");
        }
      }
    }
    appendUTF8(result, cp);
    units += cp > 0xffff ? 2 : 1;
  }
  if (at != value.size() - 1 || (quote == '\'' && units != 1))
    fail("smali character literal is not one UTF-16 code unit");
  return result;
}
int64_t integer(const std::string &text, unsigned bits = 64,
                bool is_signed = true) {
  uint64_t magnitude = 0;
  bool negative = false;
  unsigned suffix_bits = 0;
  if (text.starts_with('\'')) {
    auto value = decodeQuoted(text, '\'');
    size_t at = 0;
    magnitude = codepoint(value, at);
  } else {
    Match match_value;
    if (!match(text, match_value,
               R"(([+-]?)(0[xX][0-9a-fA-F]+|[0-9]+)([tTsSlL]?))"))
      fail("invalid or oversized smali integer literal");
    std::string digits = match_value[2];
    if (digits.size() > 32)
      fail("invalid or oversized smali integer literal");
    bool hex = digits.size() > 1 && digits[0] == '0' &&
               (digits[1] == 'x' || digits[1] == 'X');
    if (digits.size() > 1 && digits[0] == '0' && !hex)
      fail("ambiguous leading-zero smali integer literal");
    unsigned base = hex ? 16 : 10;
    for (size_t i = hex ? 2 : 0; i < digits.size(); ++i) {
      char c = digits[i];
      unsigned n = c >= '0' && c <= '9'   ? c - '0'
                   : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                          : c - 'A' + 10;
      if (magnitude > (UINT64_MAX - n) / base)
        fail("smali integer literal exceeds its operand width");
      magnitude = magnitude * base + n;
    }
    negative = match_value[1] == "-";
    std::string suffix = match_value[3];
    if (!suffix.empty()) {
      char c = suffix[0];
      suffix_bits = c == 't' || c == 'T' ? 8 : c == 's' || c == 'S' ? 16 : 64;
    }
  }
  // Integer -0 has the same unsigned value as 0; only floating carriers retain
  // a separate sign bit for zero.
  if (!magnitude)
    negative = false;
  auto fits = [&](unsigned n) {
    return negative ? magnitude <= (uint64_t(1) << (n - 1))
                    : n == 64 || magnitude < (uint64_t(1) << n);
  };
  if (suffix_bits) {
    if (!fits(suffix_bits))
      fail("smali literal exceeds its declared suffix width");
    if (!negative && (magnitude & (uint64_t(1) << (suffix_bits - 1)))) {
      negative = true;
      magnitude = suffix_bits == 64 ? uint64_t(0) - magnitude
                                    : (uint64_t(1) << suffix_bits) - magnitude;
    }
  }
  if (!fits(bits) || (!is_signed && negative))
    fail("smali integer literal exceeds its operand width");
  uint64_t raw = negative ? uint64_t(0) - magnitude : magnitude;
  if (is_signed && bits < 64 && !negative &&
      (raw & (uint64_t(1) << (bits - 1))))
    raw |= (~uint64_t(0)) << bits;
  return std::bit_cast<int64_t>(raw);
}
uint64_t floatingBits(const std::string &text, unsigned size) {
  if (text.empty())
    fail("invalid smali floating literal");
  auto core = text;
  if (std::string_view("fFdD").find(core.back()) != std::string_view::npos)
    core.pop_back();
  if (!match(
          core,
          R"([+-]?(?:(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?|0[xX][0-9a-fA-F]+(?:\.[0-9a-fA-F]*)?[pP][+-]?\d+|Infinity|NaN))"))
    fail("invalid smali floating literal");
  // LLVM's numeric parser is locale independent on all supported hosts.
  const auto &semantics =
      size == 4 ? llvm::APFloat::IEEEsingle() : llvm::APFloat::IEEEdouble();
  bool negative = core.starts_with('-');
  auto special = core;
  if (special.starts_with('+') || negative)
    special.erase(0, 1);
  if (special == "Infinity")
    return llvm::APFloat::getInf(semantics, negative)
        .bitcastToAPInt()
        .getZExtValue();
  if (special == "NaN")
    return llvm::APFloat::getQNaN(semantics, negative)
        .bitcastToAPInt()
        .getZExtValue();
  // Parse into the requested IEEE format once. A double intermediate can
  // erase which side of a single-precision halfway value the literal is on.
  llvm::APFloat number(semantics);
  auto status =
      number.convertFromString(core, llvm::APFloat::rmNearestTiesToEven);
  if (!status) {
    llvm::consumeError(status.takeError());
    fail("invalid smali floating literal");
  }
  if (size == 4 && (*status & llvm::APFloat::opOverflow))
    fail("smali floating literal exceeds its width");
  return number.bitcastToAPInt().getZExtValue();
}
int64_t bits(const std::string &text, unsigned size) {
  bool looks = text.find('.') != std::string::npos ||
               text.find_first_of("pP") != std::string::npos ||
               llvm::Regex("[eE][+-]?[0-9]").match(text) ||
               match(text, R"([+-]?\d+[fFdD])") ||
               text.find("NaN") != std::string::npos ||
               text.find("Infinity") != std::string::npos;
  if (looks && !match(text, R"([+-]?0[xX][0-9a-fA-F]+[tTsSlL]?)")) {
    if (size != 4 && size != 8)
      fail("floating smali literal has a nonfloating width");
    unsigned literal_width =
        !text.empty() && (text.back() == 'f' || text.back() == 'F') ? 4 : 8;
    if (literal_width != size)
      fail(
          "smali floating literal suffix disagrees with its raw carrier width");
    uint64_t raw = floatingBits(text, size);
    if (size == 4 && (raw & 0x80000000))
      raw |= 0xffffffff00000000ULL;
    return std::bit_cast<int64_t>(raw);
  }
  return integer(text, size * 8);
}
std::string classType(std::string_view text) {
  auto type = descriptor(text);
  if (!type.starts_with('L'))
    fail("smali declaration requires a class descriptor");
  return type;
}
const std::map<std::string, Access> &allowedAccess() {
  static const std::map<std::string, Access> value{
      {"class",
       {"public", "private", "protected", "static", "final", "interface",
        "abstract", "synthetic", "annotation", "enum"}},
      {"field",
       {"public", "private", "protected", "static", "final", "volatile",
        "transient", "synthetic", "enum"}},
      {"method",
       {"public", "private", "protected", "static", "final", "synchronized",
        "bridge", "varargs", "native", "abstract", "strictfp", "synthetic",
        "constructor", "declared-synchronized"}}};
  return value;
}
Access access(const std::vector<std::string> &tokens, const std::string &kind) {
  Access result;
  unsigned visibility = 0;
  for (auto &token : tokens) {
    if (!allowedAccess().at(kind).contains(token) ||
        !result.insert(token).second)
      fail("unknown or duplicate smali access flag");
    visibility +=
        token == "public" || token == "private" || token == "protected";
  }
  if (visibility > 1)
    fail("conflicting smali visibility flags");
  return result;
}
bool any(const Access &flags, std::initializer_list<const char *> values) {
  for (auto v : values)
    if (has(flags, v))
      return true;
  return false;
}
struct Raw {
  uint32_t pc;
  std::string text;
  size_t line;
};
struct Payload {
  std::string kind;
  std::vector<int32_t> keys;
  std::vector<std::string> targets;
  std::vector<uint64_t> data;
  unsigned element_width = 0;
};
struct ParsedInstruction {
  Instruction instruction;
  std::optional<std::string> label;
};
class Reader {
  Budget &budget;
  std::string source_id;
  std::vector<std::pair<size_t, std::string>> lines;
  size_t position = 0;
  std::string take() {
    if (position == lines.size())
      fail("unexpected end of smali input");
    auto value = lines[position++];
    line = value.first;
    budget.tick();
    return value.second;
  }
  std::string peek() const {
    return position < lines.size() ? lines[position].second : std::string();
  }
  void sourceAnnotation(const std::string &header,
                        std::optional<std::string> &signature,
                        std::optional<std::vector<std::string>> *throws_types,
                        bool &deprecated, std::set<std::string> &seen,
                        const std::string &declaration) {
    Match match_result;
    if (!match(header, match_result,
               R"(\.annotation (build|runtime|system) (L[^\s]+;))"))
      fail("unsupported source annotation visibility on " + declaration);
    auto type = classType(match_result[2]);
    if (type == "Ljava/lang/Deprecated;") {
      if (match_result[1] != "runtime")
        fail("Deprecated annotation on " + declaration +
             " requires runtime visibility");
      if (!seen.insert(type).second || deprecated)
        fail("duplicate Deprecated annotation on " + declaration);
      if (peek() != ".end annotation")
        fail("Deprecated annotation on " + declaration +
             " must have no elements");
      take();
      deprecated = true;
      return;
    }
    if (match_result[1] != "system")
      fail("unsupported source annotation visibility on " + declaration);
    bool is_signature = type == "Ldalvik/annotation/Signature;";
    bool is_throws = type == "Ldalvik/annotation/Throws;" && throws_types;
    if (!is_signature && !is_throws)
      fail("unsupported annotation " + type + " on " + declaration);
    if (!seen.insert(type).second)
      fail("duplicate source annotation on " + declaration);
    std::string body;
    while (peek() != ".end annotation") {
      if (!body.empty())
        body += ' ';
      body += take();
    }
    take();
    if (!match(body, match_result, R"(value\s*=\s*\{(.*)\})"))
      fail("invalid source annotation value on " + declaration);
    auto contents = trim(match_result[1]);
    auto values =
        contents.empty() ? std::vector<std::string>{} : parts(contents);
    if (is_signature) {
      std::string joined;
      for (const auto &value : values) {
        budget.tick();
        auto piece = decodeQuoted(value);
        budget.tick(1 + piece.size() / 16);
        if (piece.size() > budget.limits.max_bytes - joined.size())
          fail("Signature exceeds byte budget on " + declaration);
        joined += piece;
      }
      signature = std::move(joined);
    } else {
      std::vector<std::string> types;
      for (const auto &value : values) {
        budget.tick();
        types.push_back(classType(value));
      }
      *throws_types = std::move(types);
    }
  }
  bool annotationMetadata(const std::string &type,
                          const std::string &visibility, Class &cls,
                          std::set<std::string> &seen) {
    bool retention = type == "Ljava/lang/annotation/Retention;";
    bool target = type == "Ljava/lang/annotation/Target;";
    bool documented = type == "Ljava/lang/annotation/Documented;";
    bool inherited = type == "Ljava/lang/annotation/Inherited;";
    if (!retention && !target && !documented && !inherited)
      return false;
    if (visibility != "runtime")
      fail("annotation metadata " + type + " on " + cls.name +
           " requires runtime visibility");
    if (!seen.insert(type).second)
      fail("duplicate annotation metadata " + type);
    auto &metadata = cls.annotation_metadata;
    if (documented || inherited) {
      if (peek() != ".end annotation")
        fail("annotation metadata " + type + " must have no elements");
      take();
      (documented ? metadata.documented : metadata.inherited) = true;
      return true;
    }
    std::string body;
    while (peek() != ".end annotation") {
      if (!body.empty())
        body += ' ';
      body += take();
    }
    take();
    Match value;
    auto enumValue = [&](const std::string &text, const std::string &owner) {
      budget.tick();
      Match item;
      if (!match(text, item, R"(\.enum\s+(\S+))"))
        fail("annotation metadata " + type + " requires an enum value");
      auto ref = fieldRef(item[1]);
      if (ref.owner != owner || ref.type != owner)
        fail("annotation metadata " + type + " has an invalid enum identity");
      return ref.name;
    };
    if (retention) {
      if (!match(body, value, R"(value\s*=\s*(.*))"))
        fail("Retention annotation requires exactly value");
      metadata.retention =
          enumValue(trim(value[1]), "Ljava/lang/annotation/RetentionPolicy;");
    } else {
      if (!match(body, value, R"(value\s*=\s*\{(.*)\})"))
        fail("Target annotation requires an enum array");
      std::vector<std::string> targets;
      auto contents = trim(value[1]);
      if (!contents.empty())
        for (const auto &item : parts(contents))
          targets.push_back(
              enumValue(item, "Ljava/lang/annotation/ElementType;"));
      metadata.targets = std::move(targets);
    }
    return true;
  }
  void annotation(const std::string &header, Class &cls,
                  std::set<std::string> &seen) {
    Match m;
    if (!match(header, m, R"(\.annotation (build|runtime|system) (L[^\s]+;))"))
      fail("unsupported smali annotation visibility or declaration");
    auto type = classType(m[2]);
    if (type == "Ldalvik/annotation/Signature;" ||
        type == "Ljava/lang/Deprecated;") {
      sourceAnnotation(header, cls.generic_signature, nullptr, cls.deprecated,
                       seen, "class " + cls.name);
      return;
    }
    if (annotationMetadata(type, m[1], cls, seen))
      return;
    if (type != "Ldalvik/annotation/InnerClass;" &&
        type != "Ldalvik/annotation/EnclosingClass;" &&
        type != "Ldalvik/annotation/EnclosingMethod;" &&
        type != "Ldalvik/annotation/MemberClasses;") {
      if (m[1] == "system" || peek() != ".end annotation")
        fail("unsupported annotation " + type + " on class " + cls.name);
      if (!seen.insert(type).second)
        fail("duplicate marker annotation on class " + cls.name);
      take();
      cls.marker_annotations.push_back({type, m[1] == "runtime" ? 1u : 0u});
      return;
    }
    if (m[1] != "system")
      fail("unsupported smali annotation visibility or declaration");
    if (!seen.insert(type).second)
      fail("duplicate smali structural annotation");
    std::vector<std::string> body;
    while (peek() != ".end annotation")
      body.push_back(take());
    take();
    if (type.ends_with("/InnerClass;")) {
      cls.inner_class_present = true;
      std::map<std::string, std::string> values;
      for (auto &part : body) {
        auto at = part.find('=');
        if (at == std::string::npos)
          fail("invalid InnerClass annotation member");
        auto key = trim(std::string_view(part).substr(0, at)),
             value = trim(std::string_view(part).substr(at + 1));
        if ((key != "name" && key != "accessFlags") ||
            !values.emplace(key, value).second)
          fail("invalid InnerClass annotation member");
      }
      if (values.size() != 2)
        fail("incomplete InnerClass annotation");
      if (values.at("name") != "null")
        cls.inner_name = decodeQuoted(values.at("name"));
      cls.inner_access =
          accessFlags(uint32_t(integer(values.at("accessFlags"), 32, false)));
      for (auto &flag : cls.inner_access)
        if (!allowedAccess().at("class").contains(flag))
          fail("invalid InnerClass access flags");
    } else {
      std::string joined;
      for (auto &part : body) {
        if (!joined.empty())
          joined += ' ';
        joined += part;
      }
      if (type.ends_with("/EnclosingClass;")) {
        if (cls.enclosing_method)
          fail("conflicting enclosing annotations for " + cls.name);
        if (!match(joined, m, R"(value\s*=\s*(\S+))"))
          fail("invalid EnclosingClass annotation");
        cls.enclosing = classType(m[1]);
        if (cls.enclosing == cls.name)
          fail("class cannot enclose itself");
      } else if (type.ends_with("/EnclosingMethod;")) {
        if (cls.enclosing)
          fail("conflicting enclosing annotations for " + cls.name);
        if (!match(joined, m, R"(value\s*=\s*(\S+))"))
          fail("invalid EnclosingMethod annotation for " + cls.name);
        MethodRef ref;
        try {
          ref = methodRef(m[1]);
        } catch (const Error &error) {
          fail("invalid EnclosingMethod reference for " + cls.name + ": " +
               error.what());
        }
        if (!ref.owner.starts_with('L') || !nameValid(ref.name))
          fail("invalid EnclosingMethod reference for " + cls.name);
        cls.enclosing_method = std::move(ref);
      } else {
        if (!match(joined, m, R"(value\s*=\s*\{(.*)\})"))
          fail("invalid MemberClasses annotation");
        std::set<std::string> values;
        auto inner = trim(m[1]);
        if (!inner.empty())
          for (auto &part : parts(inner)) {
            budget.tick();
            auto value = classType(part);
            if (value == cls.name || !values.insert(value).second)
              fail("invalid MemberClasses identities");
          }
      }
    }
  }
  Field field(const std::string &line, const std::string &owner) {
    auto body = line.substr(7);
    auto at = body.find('=');
    auto tokens = words(std::string_view(body).substr(0, at));
    if (tokens.empty() || tokens.back().find(':') == std::string::npos)
      fail("invalid smali field declaration");
    auto ref = fieldRef(owner + "->" + tokens.back());
    if (!nameValid(ref.name))
      fail("invalid smali field name");
    tokens.pop_back();
    auto flags = access(tokens, "field");
    FieldValue value;
    if (at != std::string::npos) {
      auto text = trim(std::string_view(body).substr(at + 1));
      if (!has(flags, "static"))
        fail("instance field cannot carry a smali encoded initializer");
      auto &typ = ref.type;
      if (typ.starts_with('L') || typ.starts_with('[')) {
        if (text != "null") {
          if (typ != "Ljava/lang/String;")
            fail("unsupported reference field initializer");
          value = decodeQuoted(text);
        }
      } else if (typ == "Z") {
        if (text != "true" && text != "false" && text != "0" && text != "1" &&
            text != "0x0" && text != "0x1")
          fail("invalid boolean field initializer");
        value = text == "true" || text == "1" || text == "0x1";
      } else if (typ == "F" || typ == "D")
        value = FloatBits{floatingBits(text, typ == "F" ? 4 : 8), typ == "D"};
      else {
        unsigned count = typ == "B"                 ? 8
                         : typ == "C" || typ == "S" ? 16
                         : typ == "I"               ? 32
                                                    : 64;
        value = integer(text, count, typ != "C");
      }
    }
    Field result{std::move(ref), std::move(flags), std::move(value)};
    std::set<std::string> seen;
    while (peek().starts_with(".annotation"))
      sourceAnnotation(take(), result.generic_signature, nullptr,
                       result.deprecated, seen,
                       "field " + owner + "->" + result.reference.name + ":" +
                           result.reference.type);
    if (peek() == ".end field")
      take();
    return result;
  }
  bool debug(const std::string &line) {
    if (line == ".prologue" || line == ".epilogue" || line == ".end param")
      return true;
    if (line.starts_with(".source ")) {
      decodeQuoted(trim(std::string_view(line).substr(8)));
      return true;
    }
    if (match(line, R"(\.line \d+)") ||
        match(line, R"(\.(?:end|restart) local [pv]\d+)"))
      return true;
    if (line.starts_with(".param ")) {
      auto args = parts(std::string_view(line).substr(7));
      if ((args.size() != 1 && args.size() != 2) ||
          !match(args[0], R"([pv]\d+)"))
        fail("invalid smali parameter debug directive");
      if (args.size() == 2)
        decodeQuoted(args[1]);
      return true;
    }
    if (line.starts_with(".local ")) {
      auto args = parts(std::string_view(line).substr(7));
      Match m;
      if ((args.size() != 2 && args.size() != 3) ||
          !match(args[0], R"([pv]\d+)"))
        fail("invalid smali local debug directive");
      if (!match(args[1], m, R"xx((null|"(?:\\.|[^"\\])*"):(\S+))xx"))
        fail("invalid smali local type/name");
      if (m[1] != "null")
        decodeQuoted(m[1]);
      descriptor(m[2]);
      if (args.size() == 3)
        decodeQuoted(args[2]);
      return true;
    }
    return false;
  }
  Payload payload(const std::string &header) {
    auto tokens = words(header);
    auto kind = tokens[0].substr(1);
    int64_t start = 0;
    if (kind == "packed-switch" && tokens.size() == 2)
      start = integer(tokens[1], 32);
    else if (kind == "sparse-switch" && tokens.size() == 1) {
    } else if (kind == "array-data" && tokens.size() == 2) {
      start = integer(tokens[1], 8, false);
      if (start != 1 && start != 2 && start != 4 && start != 8)
        fail("invalid smali array-data element width");
    } else
      fail("invalid smali payload header");
    Payload result;
    result.kind = kind == "array-data" ? "fill-array-data" : kind;
    result.element_width = kind == "array-data" ? unsigned(start) : 0;
    while (peek() != ".end " + kind) {
      auto text = take();
      if (kind == "array-data") {
        for (auto &part : parts(text)) {
          budget.tick();
          result.data.push_back(uint64_t(bits(part, unsigned(start))));
        }
      } else if (kind == "packed-switch") {
        if (!labelValid(text) ||
            start + int64_t(result.keys.size()) > INT32_MAX)
          fail("invalid packed-switch entry or key overflow");
        result.keys.push_back(int32_t(start + int64_t(result.keys.size())));
        result.targets.push_back(text);
      } else {
        auto at = text.find("->");
        if (at == std::string::npos)
          fail("invalid sparse-switch entry");
        auto key_text = trim(std::string_view(text).substr(0, at)),
             label = trim(std::string_view(text).substr(at + 2));
        if (!labelValid(label) || words(key_text).size() != 1)
          fail("invalid sparse-switch entry");
        int32_t key = int32_t(integer(key_text, 32));
        if (!result.keys.empty() && key <= result.keys.back())
          fail("sparse-switch keys are duplicate or unsorted");
        result.keys.push_back(key);
        result.targets.push_back(label);
      }
      if (result.keys.size() > 65535)
        fail("smali switch exceeds its case-count limit");
    }
    take();
    return result;
  }
  ParsedInstruction instruction(const std::string &text, uint32_t pc,
                                const Method &method) {
    size_t at = 0;
    while (at < text.size() && !space(uint8_t(text[at])))
      ++at;
    std::string opcode = text.substr(0, at),
                operand_text = trim(std::string_view(text).substr(at));
    auto args =
        operand_text.empty() ? std::vector<std::string>{} : parts(operand_text);
    ParsedInstruction parsed;
    auto &ins = parsed.instruction;
    ins.pc = pc;
    ins.opcode = opcode;
    auto &registers = ins.registers;
    auto reg = [&](const std::string &value, unsigned count = 8,
                   bool wide = false) {
      if (value.size() < 2 || (value[0] != 'p' && value[0] != 'v') ||
          value.size() > 6 ||
          !std::all_of(value.begin() + 1, value.end(),
                       [](unsigned char c) { return c >= '0' && c <= '9'; }))
        fail("invalid smali register operand");
      unsigned number = 0;
      for (size_t i = 1; i < value.size(); ++i)
        number = number * 10 + unsigned(value[i] - '0');
      if (value[0] == 'p') {
        if (number >= method.incomingWords())
          fail("smali parameter register is out of range");
        number += method.registers - method.incomingWords();
      }
      if (number >= method.registers || number >= (1u << count) ||
          (wide && number + 1 >= method.registers))
        fail("smali register exceeds its frame or instruction format");
      return number;
    };
    auto regs = [&](size_t count, unsigned bits = 8,
                    const std::set<size_t> &wide = std::set<size_t>{}) {
      if (args.size() != count)
        fail("wrong smali instruction operand count");
      for (size_t i = 0; i < count; ++i)
        registers.push_back(reg(args[i], bits, wide.contains(i)));
    };
    auto is_binary = [](std::string_view name) {
      auto at = name.find('-');
      if (at == std::string_view::npos)
        return false;
      auto operation = name.substr(0, at), type = name.substr(at + 1);
      bool scalar = type == "int" || type == "long";
      if (!scalar && type != "float" && type != "double")
        return false;
      for (auto op : {"add", "sub", "mul", "div", "rem"})
        if (operation == op)
          return true;
      if (scalar)
        for (auto op : {"and", "or", "xor", "shl", "shr", "ushr"})
          if (operation == op)
            return true;
      return false;
    };
    auto is_field = [](std::string_view name) {
      for (auto base : {"iget", "iput", "sget", "sput"})
        for (auto suffix :
             {"", "-wide", "-object", "-boolean", "-byte", "-char", "-short"})
          if (name == std::string(base) + suffix)
            return true;
      return false;
    };
    auto is_array = [](std::string_view name) {
      for (auto base : {"aget", "aput"})
        for (auto suffix :
             {"", "-wide", "-object", "-boolean", "-byte", "-char", "-short"})
          if (name == std::string(base) + suffix)
            return true;
      return false;
    };
    auto is_invoke = [](std::string_view name) {
      return name == "invoke-virtual" || name == "invoke-super" ||
             name == "invoke-direct" || name == "invoke-static" ||
             name == "invoke-interface";
    };
    auto base = opcode.ends_with("/range") ? opcode.substr(0, opcode.size() - 6)
                                           : opcode;
    auto two_addr = opcode.ends_with("/2addr")
                        ? opcode.substr(0, opcode.size() - 6)
                        : opcode;
    bool conversion =
        match(opcode,
              R"((?:int|long|float|double)-to-(?:int|long|float|double))") ||
        opcode == "int-to-byte" || opcode == "int-to-char" ||
        opcode == "int-to-short";
    if (conversion) {
      auto split = opcode.find("-to-");
      if (opcode.substr(0, split) == opcode.substr(split + 4))
        conversion = false;
    }
    if (opcode == "nop" || opcode == "return-void")
      regs(0);
    else if (opcode == "move-result" || opcode == "move-result-wide" ||
             opcode == "move-result-object" || opcode == "move-exception" ||
             opcode == "return" || opcode == "return-wide" ||
             opcode == "return-object" || opcode == "monitor-enter" ||
             opcode == "monitor-exit" || opcode == "throw")
      regs(1, 8,
           opcode.ends_with("-wide") ? std::set<size_t>{0}
                                     : std::set<size_t>{});
    else if (match(opcode, R"(move(?:-wide|-object)?(?:/from16|/16)?)")) {
      if (args.size() != 2)
        fail("wrong smali move operand count");
      unsigned dst = opcode.ends_with("/16")       ? 16
                     : opcode.ends_with("/from16") ? 8
                                                   : 4;
      unsigned src = opcode.find('/') != std::string::npos ? 16 : 4;
      registers = {
          reg(args[0], dst, opcode.find("-wide") != std::string::npos),
          reg(args[1], src, opcode.find("-wide") != std::string::npos)};
    } else if (conversion || opcode == "neg-int" || opcode == "not-int" ||
               opcode == "neg-long" || opcode == "not-long" ||
               opcode == "neg-float" || opcode == "neg-double" ||
               opcode == "array-length") {
      std::set<size_t> wide;
      if (conversion) {
        auto split = opcode.find("-to-");
        auto src = opcode.substr(0, split), dst = opcode.substr(split + 4);
        if (src == "long" || src == "double")
          wide.insert(1);
        if (dst == "long" || dst == "double")
          wide.insert(0);
      } else if (opcode.ends_with("long") || opcode.ends_with("double"))
        wide = {0, 1};
      regs(2, 4, wide);
    } else if (is_binary(two_addr)) {
      unsigned count = opcode.ends_with("/2addr") ? 2 : 3;
      auto type = two_addr.substr(two_addr.find('-') + 1);
      std::set<size_t> wide;
      if (type == "long" || type == "double")
        for (unsigned i = 0; i < count; ++i)
          wide.insert(i);
      if (type == "long" &&
          (opcode.starts_with("shl-") || opcode.starts_with("shr-") ||
           opcode.starts_with("ushr-")))
        wide.erase(count - 1);
      regs(count, count == 2 ? 4 : 8, wide);
    } else if (opcode == "cmp-long" || opcode == "cmpl-float" ||
               opcode == "cmpg-float" || opcode == "cmpl-double" ||
               opcode == "cmpg-double")
      regs(3, 8,
           opcode.ends_with("long") || opcode.ends_with("double")
               ? std::set<size_t>{1, 2}
               : std::set<size_t>{});
    else if (is_array(opcode))
      regs(3, 8,
           opcode.ends_with("-wide") ? std::set<size_t>{0}
                                     : std::set<size_t>{});
    else if (is_field(opcode)) {
      unsigned count = opcode[0] == 's' ? 2 : 3;
      if (args.size() != count)
        fail("wrong smali field operand count");
      auto ref = fieldRef(args.back());
      classType(ref.owner);
      if (!nameValid(ref.name))
        fail("invalid smali field reference name");
      auto dash = opcode.find('-');
      auto suffix = dash == std::string::npos ? "" : opcode.substr(dash + 1);
      bool valid = false;
      if (suffix == "wide")
        valid = ref.type == "J" || ref.type == "D";
      else if (suffix == "object")
        valid = ref.type.starts_with('L') || ref.type.starts_with('[');
      else if (suffix == "boolean")
        valid = ref.type == "Z";
      else if (suffix == "byte")
        valid = ref.type == "B";
      else if (suffix == "char")
        valid = ref.type == "C";
      else if (suffix == "short")
        valid = ref.type == "S";
      else
        valid = ref.type == "I" || ref.type == "F";
      if (!valid)
        fail("smali field opcode disagrees with its referenced type");
      for (unsigned i = 0; i < count - 1; ++i)
        registers.push_back(reg(args[i], count == 2 ? 8 : 4,
                                i == 0 && opcode.ends_with("-wide")));
      ins.reference = std::move(ref);
    } else if (opcode == "const/4" || opcode == "const/16" ||
               opcode == "const" || opcode == "const/high16" ||
               opcode == "const-wide/16" || opcode == "const-wide/32" ||
               opcode == "const-wide" || opcode == "const-wide/high16" ||
               opcode == "const-string" || opcode == "const-string/jumbo" ||
               opcode == "const-class" || opcode == "const-method-type") {
      if (args.size() != 2)
        fail("wrong smali constant operand count");
      bool wide = opcode.starts_with("const-wide");
      registers = {reg(args[0], opcode == "const/4" ? 4 : 8, wide)};
      if (opcode.starts_with("const-string"))
        ins.literal = decodeQuoted(args[1]);
      else if (opcode == "const-class")
        ins.reference = descriptor(args[1]);
      else if (opcode == "const-method-type") {
        prototype(args[1]);
        ins.reference = args[1];
      } else {
        int64_t value = bits(args[1], wide ? 8 : 4);
        if (opcode.ends_with("/high16")) {
          if (uint64_t(value) & ((uint64_t(1) << (wide ? 48 : 16)) - 1))
            fail("smali high16 constant has nonzero low bits");
        } else {
          unsigned count = opcode.ends_with("/4")             ? 4
                           : opcode.ends_with("/16")          ? 16
                           : opcode.ends_with("/32") || !wide ? 32
                                                              : 64;
          if (count < 64 && (value < -(int64_t(1) << (count - 1)) ||
                             value >= (int64_t(1) << (count - 1))))
            fail("smali constant exceeds its instruction format");
        }
        ins.literal = value;
      }
    } else if (opcode == "new-instance" || opcode == "check-cast" ||
               opcode == "instance-of" || opcode == "new-array") {
      unsigned count = opcode == "instance-of" || opcode == "new-array" ? 3 : 2;
      if (args.size() != count)
        fail("wrong smali type operand count");
      auto ref = descriptor(args.back());
      if (opcode == "new-instance")
        classType(ref);
      else if (opcode == "new-array" && !ref.starts_with('['))
        fail("new-array requires an array descriptor");
      else if ((opcode == "check-cast" || opcode == "instance-of") &&
               !ref.starts_with('L') && !ref.starts_with('['))
        fail("smali reference operation has a primitive type");
      for (unsigned i = 0; i < count - 1; ++i)
        registers.push_back(reg(args[i], count == 3 ? 4 : 8));
      ins.reference = std::move(ref);
    } else if (is_invoke(base) || base == "filled-new-array") {
      if (args.size() != 2 || !args[0].starts_with('{') ||
          !args[0].ends_with('}'))
        fail("invalid smali invoke register list");
      auto contents =
          trim(std::string_view(args[0]).substr(1, args[0].size() - 2));
      if (opcode.ends_with("/range") && !contents.empty()) {
        Match m;
        if (!match(contents, m, R"(([pv]\d+)\s*\.\.\s*([pv]\d+))"))
          fail("invalid smali register range");
        unsigned first = reg(m[1], 16), last = reg(m[2], 16);
        if (last < first || last - first >= 255)
          fail("invalid or oversized smali register range");
        for (unsigned i = first; i <= last; ++i)
          registers.push_back(i);
      } else {
        if (!contents.empty())
          for (auto &part : parts(contents))
            registers.push_back(reg(part, 4));
        if (registers.size() > 5)
          fail("smali invoke needs a range format for more than five words");
      }
      if (base == "filled-new-array") {
        auto ref = descriptor(args[1]);
        if (!ref.starts_with('[') || ref.substr(1) == "J" ||
            ref.substr(1) == "D")
          fail("invalid filled-new-array element type");
        ins.reference = std::move(ref);
      } else {
        auto ref = methodRef(args[1]);
        if ((!ref.owner.starts_with('L') && !ref.owner.starts_with('[')) ||
            !nameValid(ref.name))
          fail("invalid smali method reference");
        if (ref.name == "<init>") {
          if (base != "invoke-direct" || ref.returns != "V" ||
              !ref.owner.starts_with('L'))
            fail("invalid smali constructor invocation");
        } else if (ref.name.find_first_of("<>") != std::string::npos)
          fail("invalid special smali invocation name");
        size_t expected = base != "invoke-static";
        for (auto &p : ref.parameters)
          expected += width(p);
        if (registers.size() != expected)
          fail("smali invoke word count disagrees with its prototype");
        size_t at = base != "invoke-static";
        for (auto &typ : ref.parameters) {
          if (width(typ) == 2 && registers[at + 1] != registers[at] + 1)
            fail("wide smali invoke argument is not an adjacent register pair");
          at += width(typ);
        }
        ins.reference = std::move(ref);
      }
    } else if (opcode == "goto" || opcode == "goto/16" || opcode == "goto/32") {
      if (args.size() != 1 || !labelValid(args[0]))
        fail("invalid smali goto target");
      parsed.label = args[0];
    } else if (match(opcode, R"(if-(?:eq|ne|lt|ge|gt|le)z?)")) {
      unsigned count = opcode.ends_with('z') ? 2 : 3;
      if (args.size() != count || !labelValid(args.back()))
        fail("invalid smali conditional branch");
      for (unsigned i = 0; i < count - 1; ++i)
        registers.push_back(reg(args[i], count == 2 ? 8 : 4));
      parsed.label = args.back();
    } else if (opcode == "packed-switch" || opcode == "sparse-switch" ||
               opcode == "fill-array-data") {
      if (args.size() != 2 || !labelValid(args[1]))
        fail("invalid smali payload reference");
      registers = {reg(args[0])};
      parsed.label = args[1];
    } else if (
        opcode == "rsub-int" ||
        match(
            opcode,
            R"((?:add|mul|div|rem|and|or|xor)-int/lit16|(?:add|rsub|mul|div|rem|and|or|xor|shl|shr|ushr)-int/lit8)")) {
      if (args.size() != 3)
        fail("invalid smali literal arithmetic operands");
      unsigned count = opcode.ends_with("/lit8") ? 8 : 16;
      registers = {reg(args[0], count == 8 ? 8 : 4),
                   reg(args[1], count == 8 ? 8 : 4)};
      ins.literal = integer(args[2], count);
    } else
      fail("unsupported smali opcode: " + opcode);
    if (opcode == "return" || opcode == "return-wide" ||
        opcode == "return-object" || opcode == "return-void") {
      auto &typ = method.reference.returns;
      auto expected = typ == "V"                 ? "return-void"
                      : typ == "J" || typ == "D" ? "return-wide"
                      : typ.starts_with('L') || typ.starts_with('[')
                          ? "return-object"
                          : "return";
      if (opcode != expected)
        fail("smali return opcode disagrees with its method prototype");
    }
    return parsed;
  }
  Method method(const std::string &header, const std::string &owner) {
    auto tokens = words(std::string_view(header).substr(8));
    if (tokens.empty() || tokens.back().find('(') == std::string::npos)
      fail("invalid smali method declaration");
    Method result;
    result.reference = methodRef(owner + "->" + tokens.back());
    tokens.pop_back();
    auto &ref = result.reference;
    auto &flags = result.access;
    flags = access(tokens, "method");
    if (!nameValid(ref.name))
      fail("invalid smali method name");
    if (ref.name == "<init>" || ref.name == "<clinit>") {
      if (ref.returns != "V" ||
          ((ref.name == "<clinit>") != has(flags, "static")) ||
          (ref.name == "<clinit>" && !ref.parameters.empty()))
        fail("invalid smali constructor signature");
    } else if (has(flags, "constructor") ||
               ref.name.find_first_of("<>") != std::string::npos)
      fail("invalid smali constructor declaration");
    if (has(flags, "abstract") &&
        any(flags, {"native", "static", "private", "final"}))
      fail("invalid abstract smali method flags");
    std::optional<unsigned> total;
    std::vector<Raw> raw;
    std::map<std::string, uint32_t> labels;
    std::set<uint32_t> label_positions;
    std::map<uint32_t, Payload> payloads;
    std::vector<std::tuple<std::optional<std::string>, std::string, std::string,
                           std::string>>
        catches;
    uint32_t pc = 0;
    std::set<std::string> seen_annotations;
    bool parameter_annotation_scope = false;
    while (true) {
      auto text = take();
      if (text == ".end method")
        break;
      if (text.starts_with(".annotation")) {
        if (parameter_annotation_scope)
          fail("parameter annotations are not represented in the source model");
        sourceAnnotation(text, result.generic_signature,
                         &result.declared_throws, result.deprecated,
                         seen_annotations, "method " + ref.identity());
      } else if (text.starts_with(".locals ") ||
                 text.starts_with(".registers ")) {
        if (total || !raw.empty() || !payloads.empty())
          fail("duplicate or late smali register declaration");
        auto count = words(text);
        if (count.size() != 2)
          fail("invalid smali register declaration");
        uint64_t value = uint64_t(integer(count[1], 16, false));
        if (count[0] == ".locals")
          value += result.incomingWords();
        if (value < result.incomingWords() || value > 65535)
          fail("invalid smali register frame");
        total = unsigned(value);
        result.registers = *total;
      } else if (text.starts_with(':')) {
        if (!labelValid(text) || !labels.emplace(text, pc).second)
          fail("invalid or duplicate smali label");
        label_positions.insert(pc);
      } else if (text.starts_with(".catch ") ||
                 text.starts_with(".catchall ")) {
        bool all = text.starts_with(".catchall ");
        auto body = trim(std::string_view(text).substr(all ? 10 : 7));
        auto begin = body.find('{'), end = body.find('}');
        if (begin == std::string::npos || end == std::string::npos ||
            end <= begin)
          fail("invalid smali catch directive");
        auto typ = trim(std::string_view(body).substr(0, begin));
        auto range =
            trim(std::string_view(body).substr(begin + 1, end - begin - 1));
        auto target = trim(std::string_view(body).substr(end + 1));
        auto dots = range.find("..");
        if ((all && !typ.empty()) || (!all && typ.empty()) ||
            dots == std::string::npos || !labelValid(target))
          fail("invalid smali catch directive");
        auto start = trim(std::string_view(range).substr(0, dots)),
             stop = trim(std::string_view(range).substr(dots + 2));
        if (!labelValid(start) || !labelValid(stop))
          fail("invalid smali catch directive");
        catches.emplace_back(all ? std::nullopt
                                 : std::optional<std::string>(classType(typ)),
                             start, stop, target);
      } else if (text.starts_with(".packed-switch ") ||
                 text.starts_with(".sparse-switch") ||
                 text.starts_with(".array-data ")) {
        if (!label_positions.contains(pc))
          fail("smali payload requires a preceding label");
        if (!raw.empty() && raw.back().pc == pc - 1) {
          auto op = words(raw.back().text)[0];
          if (!op.starts_with("return") && !op.starts_with("throw") &&
              !op.starts_with("goto"))
            fail("normal execution would fall through into a smali payload");
        }
        payloads[pc++] = payload(text);
      } else if (debug(text)) {
        if (text.starts_with(".param "))
          parameter_annotation_scope = true;
        else if (text == ".end param")
          parameter_annotation_scope = false;
      } else if (text.starts_with('.'))
        fail("unsupported smali method directive");
      else
        raw.push_back({pc++, text, line});
    }
    bool no_code = any(flags, {"abstract", "native"});
    if (no_code) {
      if (total || !raw.empty() || !labels.empty() || !payloads.empty() ||
          !catches.empty())
        fail("abstract/native smali method has a body");
      return result;
    }
    if (!total || raw.empty() || raw.front().pc != 0)
      fail("concrete smali method has no register frame or body");
    std::set<uint32_t> executable, used_payloads;
    for (auto &entry : raw)
      executable.insert(entry.pc);
    for (auto &entry : raw) {
      budget.tick();
      line = entry.line;
      auto parsed = instruction(entry.text, entry.pc, result);
      auto &ins = parsed.instruction;
      if (parsed.label) {
        auto label = labels.find(*parsed.label);
        if (label == labels.end())
          fail("undefined smali branch or payload label");
        uint32_t target = label->second;
        ins.target = target;
        if (ins.opcode == "packed-switch" || ins.opcode == "sparse-switch" ||
            ins.opcode == "fill-array-data") {
          auto found = payloads.find(target);
          if (found == payloads.end())
            fail("smali instruction does not target a data payload");
          auto &p = found->second;
          if (p.kind != ins.opcode)
            fail("smali payload kind disagrees with its instruction");
          used_payloads.insert(target);
          ins.keys = p.keys;
          ins.data = p.data;
          ins.element_width = p.element_width;
          for (auto &name : p.targets) {
            budget.tick();
            auto at = labels.find(name);
            if (at == labels.end() || !executable.contains(at->second))
              fail("smali switch case has no executable target");
            ins.targets.push_back(at->second);
          }
        } else if (!executable.contains(target))
          fail("smali branch targets a nonexecutable boundary");
      }
      result.instructions.push_back(std::move(ins));
    }
    if (used_payloads.size() != payloads.size())
      fail("unreferenced smali data payload cannot be preserved");
    std::map<std::pair<uint32_t, uint32_t>, size_t> grouped;
    for (auto &[typ, start, end, handler] : catches) {
      budget.tick();
      if (!labels.contains(start) || !labels.contains(end) ||
          !labels.contains(handler))
        fail("undefined smali exception boundary");
      uint32_t a = labels.at(start), b = labels.at(end),
               target = labels.at(handler);
      if (!executable.contains(a) || !executable.contains(target) || a >= b ||
          b > pc)
        fail("invalid smali exception region");
      auto key = std::pair{a, b};
      if (!grouped.contains(key)) {
        grouped[key] = result.tries.size();
        result.tries.push_back({a, b, {}});
      }
      auto &handlers = result.tries[grouped.at(key)].handlers;
      for (auto &entry : handlers)
        if (entry.type == typ || !entry.type)
          fail("duplicate or unreachable smali exception handler");
      handlers.push_back({typ, target});
    }
    result.code_end = pc;
    budget.tick();
    return result;
  }

public:
  size_t line = 1;
  Reader(std::string_view text, std::string_view source_id, Budget &budget)
      : budget(budget), source_id(source_id) {
    budget.tick(text.size() + 1);
    if (text.size() > budget.limits.max_bytes ||
        text.find('\0') != std::string_view::npos)
      fail("smali input is invalid or exceeds its byte budget");
    size_t start = 0, at = 0, number = 1;
    while (at < text.size()) {
      size_t before = at;
      uint32_t cp = codepoint(text, at);
      if (cp == '\n' || cp == '\r' || cp == '\v' || cp == '\f' ||
          (cp >= 0x1c && cp <= 0x1e) || cp == 0x85 || cp == 0x2028 ||
          cp == 0x2029) {
        line = number;
        auto clean = uncomment(text.substr(start, before - start));
        if (!clean.empty())
          lines.emplace_back(number, std::move(clean));
        if (cp == '\r' && at < text.size() && text[at] == '\n')
          ++at;
        start = at;
        ++number;
      }
    }
    if (start < text.size()) {
      line = number;
      auto clean = uncomment(text.substr(start));
      if (!clean.empty())
        lines.emplace_back(number, std::move(clean));
    }
    line = 1;
  }
  Class parse() {
    auto header = words(take());
    if (header.size() < 2 || header[0] != ".class")
      fail("smali input must begin with one class declaration");
    Class cls;
    cls.name = classType(header.back());
    header.pop_back();
    header.erase(header.begin());
    cls.access = access(header, "class");
    cls.source_id = source_id;
    std::set<std::string> seen_annotations, methods, interfaces;
    std::set<std::pair<std::string, std::string>> fields;
    bool superclass = false, source = false;
    while (!peek().empty()) {
      auto text = take();
      if (text.starts_with(".super ")) {
        if (superclass)
          fail("duplicate smali superclass");
        cls.superclass = classType(trim(std::string_view(text).substr(7)));
        if (cls.superclass == cls.name)
          fail("class cannot extend itself");
        superclass = true;
      } else if (text.starts_with(".implements ")) {
        auto typ = classType(trim(std::string_view(text).substr(12)));
        if (!interfaces.insert(typ).second)
          fail("duplicate smali interface");
        cls.interfaces.push_back(typ);
      } else if (text.starts_with(".source ")) {
        if (source)
          fail("duplicate smali source directive");
        decodeQuoted(trim(std::string_view(text).substr(8)));
        source = true;
      } else if (text.starts_with(".annotation "))
        annotation(text, cls, seen_annotations);
      else if (text.starts_with(".field ")) {
        auto item = field(text, cls.name);
        if (!fields.emplace(item.reference.name, item.reference.type).second)
          fail("duplicate smali field");
        cls.fields.push_back(std::move(item));
      } else if (text.starts_with(".method ")) {
        auto item = method(text, cls.name);
        if (!methods.insert(item.reference.identity()).second)
          fail("duplicate smali method");
        cls.methods.push_back(std::move(item));
      } else
        fail("unknown or misplaced smali class syntax");
    }
    if (!superclass && cls.name != "Ljava/lang/Object;")
      fail("smali class has no superclass declaration");
    if (cls.inner_class_present != bool(cls.enclosing || cls.enclosing_method))
      fail("incomplete inner class metadata for " + cls.name);
    budget.tick();
    return cls;
  }
};
} // namespace
Class parseSmali(std::string_view text, std::string_view input_id,
                 Budget &budget) {
  size_t line = 1;
  try {
    Reader reader(text, input_id, budget);
    try {
      return reader.parse();
    } catch (const Error &) {
      line = reader.line;
      throw;
    }
  } catch (const Error &error) {
    throw Error("smali " + std::string(input_id) + ":" + std::to_string(line) +
                ": " + error.what());
  }
}
} // namespace neverd::mobile::dalvik
