#include "JsonReader.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/Session.h"

#include <set>
#include <string>

namespace neverd::web {
namespace {
class Preflight {
  std::string_view Bytes;
  const JsonLimits &Limits;
  size_t Cursor = 0;
  uint64_t Nodes = 0;

  static bool space(char C) {
    return C == ' ' || C == '\t' || C == '\r' || C == '\n';
  }
  void whitespace() {
    while (Cursor < Bytes.size() && space(Bytes[Cursor]))
      ++Cursor;
  }
  bool consume(char C) {
    whitespace();
    if (Cursor == Bytes.size() || Bytes[Cursor] != C)
      return false;
    ++Cursor;
    return true;
  }
  void require(char C) {
    if (!consume(C))
      throw Error("invalid_json");
  }
  void charge() {
    if (++Nodes > Limits.MaxNodes)
      throw Error("json_node_budget_exceeded");
  }
  uint16_t escapedUnit() {
    uint16_t Unit = 0;
    for (unsigned I = 0; I != 4; ++I) {
      if (Cursor == Bytes.size())
        throw Error("invalid_json");
      const char C = Bytes[Cursor++];
      const unsigned Digit = C >= '0' && C <= '9'   ? C - '0'
                             : C >= 'a' && C <= 'f' ? C - 'a' + 10
                             : C >= 'A' && C <= 'F' ? C - 'A' + 10
                                                    : 16;
      if (Digit == 16)
        throw Error("invalid_json");
      Unit = uint16_t((Unit << 4) | Digit);
    }
    return Unit;
  }
  static void primitive(std::string_view Token) {
    if (Token == "true" || Token == "false" || Token == "null")
      return;
    size_t I = 0;
    const auto digit = [&](size_t Index) {
      return Index < Token.size() && Token[Index] >= '0' && Token[Index] <= '9';
    };
    if (I < Token.size() && Token[I] == '-')
      ++I;
    if (!digit(I))
      throw Error("invalid_json");
    if (Token[I] == '0')
      ++I;
    else
      while (digit(I))
        ++I;
    if (I < Token.size() && Token[I] == '.') {
      if (!digit(++I))
        throw Error("invalid_json");
      while (digit(I))
        ++I;
    }
    if (I < Token.size() && (Token[I] == 'e' || Token[I] == 'E')) {
      ++I;
      if (I < Token.size() && (Token[I] == '+' || Token[I] == '-'))
        ++I;
      if (!digit(I))
        throw Error("invalid_json");
      while (digit(I))
        ++I;
    }
    if (I != Token.size())
      throw Error("invalid_json");
  }
  std::string_view string() {
    whitespace();
    const auto Start = Cursor;
    require('"');
    while (Cursor < Bytes.size()) {
      if (Cursor - Start - 1 > Limits.MaxStringBytes)
        throw Error("json_string_budget_exceeded");
      const unsigned char C = Bytes[Cursor++];
      if (C < 0x20)
        throw Error("invalid_json");
      if (C == '"')
        return Bytes.substr(Start, Cursor - Start);
      if (C == '\\') {
        if (Cursor == Bytes.size())
          throw Error("invalid_json");
        const auto Escape = Bytes[Cursor++];
        if (Escape == 'u') {
          const auto Unit = escapedUnit();
          // Preserve evidence: LLVM's JSON parser replaces lone surrogates.
          // This profile rejects them instead of silently changing the text.
          if (Unit >= 0xD800 && Unit <= 0xDBFF) {
            if (Bytes.substr(Cursor, 2) != "\\u")
              throw Error("unsupported_json_surrogate");
            Cursor += 2;
            const auto Low = escapedUnit();
            if (Low < 0xDC00 || Low > 0xDFFF)
              throw Error("unsupported_json_surrogate");
          } else if (Unit >= 0xDC00 && Unit <= 0xDFFF) {
            throw Error("unsupported_json_surrogate");
          }
        } else if (std::string_view("\"\\/bfnrt").find(Escape) ==
                   std::string_view::npos) {
          throw Error("invalid_json");
        }
      }
    }
    throw Error("invalid_json");
  }
  void value(uint64_t Depth) {
    charge();
    whitespace();
    if (Cursor == Bytes.size())
      throw Error("invalid_json");
    if (Bytes[Cursor] == '"') {
      (void)string();
      return;
    }
    if (Bytes[Cursor] == '[' || Bytes[Cursor] == '{') {
      if (Depth >= Limits.MaxDepth)
        throw Error("json_depth_budget_exceeded");
      const bool Object = Bytes[Cursor++] == '{';
      const char End = Object ? '}' : ']';
      if (consume(End))
        return;
      std::set<std::string> Keys;
      do {
        if (Object) {
          charge();
          auto Key = llvm::json::parse(llvm::StringRef(string()));
          if (!Key) {
            llvm::consumeError(Key.takeError());
            throw Error("invalid_json");
          }
          if (!Keys.insert(Key->getAsString()->str()).second)
            throw Error("duplicate_json_key");
          require(':');
        }
        value(Depth + 1);
        if (consume(End))
          return;
        require(',');
      } while (true);
    }
    // Validate spelling here; the DOM parser accepts leading-zero numbers.
    // The DOM parser remains responsible for numeric conversion.
    // No primitive can hide a valid nested object/array from this preflight.
    const auto Start = Cursor;
    while (Cursor < Bytes.size() && !space(Bytes[Cursor]) &&
           Bytes[Cursor] != ',' && Bytes[Cursor] != ']' &&
           Bytes[Cursor] != '}') {
      if (Bytes[Cursor] == '[' || Bytes[Cursor] == '{' ||
          Bytes[Cursor] == '"' || Bytes[Cursor] == ':')
        throw Error("invalid_json");
      ++Cursor;
    }
    if (Start == Cursor)
      throw Error("invalid_json");
    if (Cursor - Start > 128)
      throw Error("json_primitive_budget_exceeded");
    primitive(Bytes.substr(Start, Cursor - Start));
  }

public:
  Preflight(std::string_view Bytes, const JsonLimits &Limits)
      : Bytes(Bytes), Limits(Limits) {}
  void check() {
    value(0);
    whitespace();
    if (Cursor != Bytes.size())
      throw Error("invalid_json");
  }
};
} // namespace

llvm::json::Value parseBoundedJSON(std::string_view Bytes,
                                   const JsonLimits &Limits) {
  if (Bytes.size() > Limits.MaxBytes)
    throw Error("json_byte_budget_exceeded");
  if (!validUtf8(Bytes))
    throw Error("invalid_json_encoding");
  Preflight(Bytes, Limits).check();
  auto Parsed = llvm::json::parse(llvm::StringRef(Bytes));
  if (!Parsed) {
    llvm::consumeError(Parsed.takeError());
    throw Error("invalid_json");
  }
  return std::move(*Parsed);
}
} // namespace neverd::web
