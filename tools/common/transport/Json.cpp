//===- Json.cpp - Bounded tool transport JSON -----------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared UTF-8, duplicate-key, depth and encoded-output admission.
///
//===----------------------------------------------------------------------===//

#include "Json.h"

#include "ResourceLimits.h"

#if NEVERD_USE_SIMDJSON
#include "simdjson.h"
#endif

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace neverd::transport {
namespace {
// DOM filter callbacks rescan arrays when objects end. SAX key admission
// followed by ordinary DOM parsing keeps both passes within the same bounds.
class UniqueKeys final : public nlohmann::json_sax<Json> {
  std::vector<std::set<std::string>> Keys;

public:
  bool null() override { return true; }
  bool boolean(bool) override { return true; }
  bool number_integer(Json::number_integer_t) override { return true; }
  bool number_unsigned(Json::number_unsigned_t) override { return true; }
  bool number_float(Json::number_float_t, const Json::string_t &) override {
    return true;
  }
  bool string(Json::string_t &) override { return true; }
  bool binary(Json::binary_t &) override { return true; }
  bool start_object(std::size_t) override {
    Keys.emplace_back();
    return true;
  }
  bool key(Json::string_t &Value) override {
    if (!Keys.back().insert(Value).second)
      throw Error("invalid_json", "Duplicate object key in JSON frame");
    return true;
  }
  bool end_object() override {
    Keys.pop_back();
    return true;
  }
  bool start_array(std::size_t) override { return true; }
  bool end_array() override { return true; }
  bool parse_error(std::size_t, const std::string &,
                   const Json::exception &) override {
    return false;
  }
};

#if NEVERD_USE_SIMDJSON
enum class DuplicateKeys { Reject, KeepLast };
// Copy the validated tape directly into the owned DOM. Object insertion also
// checks decoded duplicate keys, without a separate set or a second text parse.
Json copySIMDValue(simdjson::dom::element Value, bool &NeedsNumberParser,
                   DuplicateKeys Keys) {
  using Type = simdjson::dom::element_type;
  switch (Value.type()) {
  case Type::OBJECT: {
    Json::object_t Result;
    // value_unsafe() refers into a temporary result; retain the view by value.
    const auto Object = Value.get_object().value_unsafe();
    for (const auto Field : Object) {
      auto [It, Inserted] = Result.emplace(std::string(Field.key), nullptr);
      if (!Inserted && Keys == DuplicateKeys::Reject)
        throw Error("invalid_json", "Duplicate object key in JSON frame");
      It->second = copySIMDValue(Field.value, NeedsNumberParser, Keys);
      if (NeedsNumberParser && Keys == DuplicateKeys::KeepLast)
        return nullptr;
    }
    return Json(std::move(Result));
  }
  case Type::ARRAY: {
    const auto Array = Value.get_array().value_unsafe();
    Json::array_t Result;
    Result.reserve(Array.size());
    for (const auto Child : Array) {
      Result.push_back(copySIMDValue(Child, NeedsNumberParser, Keys));
      if (NeedsNumberParser && Keys == DuplicateKeys::KeepLast)
        return nullptr;
    }
    return Json(std::move(Result));
  }
  case Type::STRING:
    return std::string(Value.get_string().value_unsafe());
  case Type::INT64: {
    const auto Number = Value.get_int64().value_unsafe();
    // nlohmann uses unsigned storage for nonnegative integer tokens.
    return Number < 0 ? Json(Number) : Json(static_cast<uint64_t>(Number));
  }
  case Type::UINT64:
    return Value.get_uint64().value_unsafe();
  case Type::BIGINT:
  case Type::DOUBLE:
    // Retain the existing decimal conversion and underflow/rounding contract.
    NeedsNumberParser = true;
    return nullptr;
  case Type::BOOL:
    return Value.get_bool().value_unsafe();
  case Type::NULL_VALUE:
    return nullptr;
  }
  throw Error("invalid_json", "Malformed UTF-8 JSON frame");
}

std::optional<Json> parseWithSIMD(std::string_view Text, DuplicateKeys Keys) {
  // The tape loses the signed integer spelling of -0. Backend documents have
  // no separate duplicate-key admission, so they can go straight to native
  // number parsing. Frames must still finish checking all decoded keys.
  bool NeedsNumberParser = Text.find("-0") != std::string_view::npos;
  if (NeedsNumberParser && Keys == DuplicateKeys::KeepLast)
    return Json::parse(Text);
  // Keep retained storage bounded per transport thread. Larger frames get a
  // temporary parser whose tape is released after constructing the owned DOM.
  constexpr auto RetainedBytes =
      ::neverd::limits::transport::kJsonRetainedParserBytes;
  auto Parse = [&](simdjson::dom::parser &Parser) -> std::optional<Json> {
    simdjson::dom::element Root;
    if (Parser.parse(Text.data(), Text.size()).get(Root))
      return std::nullopt;
    return copySIMDValue(Root, NeedsNumberParser, Keys);
  };
  std::optional<Json> Result;
  if (Text.size() > RetainedBytes) {
    simdjson::dom::parser Parser;
    Result = Parse(Parser);
  } else {
    thread_local simdjson::dom::parser Parser(RetainedBytes);
    Result = Parse(Parser);
  }
  if (Result && NeedsNumberParser) {
    // Release a large tape and any partial owned tree before native parsing.
    Result.reset();
    return Json::parse(Text);
  }
  return Result;
}
#endif

// Feed the existing codec directly: an ostream adds sentry/virtual calls for
// each fragment. The adapter still checks every append before growing output.
class BoundedOutput final
    : public nlohmann::detail::output_adapter_protocol<char> {
  std::size_t Maximum;

public:
  std::string Bytes;
  explicit BoundedOutput(std::size_t Maximum) : Maximum(Maximum) {}
  void write_character(char Character) override {
    if (Bytes.size() == Maximum)
      throw Error("budget_exceeded", "Encoded JSON exceeds the output budget");
    Bytes.push_back(Character);
  }
  void write_characters(const char *Data, std::size_t Count) override {
    if (Count > Maximum - Bytes.size())
      throw Error("budget_exceeded", "Encoded JSON exceeds the output budget");
    Bytes.append(Data, Count);
  }
};
} // namespace

std::string stringField(const Json &Object, const char *Key,
                        std::string Fallback, std::size_t Max) {
  const auto It = Object.find(Key);
  if (It == Object.end())
    return Fallback;
  if (!It->is_string())
    throw Error("invalid_request", std::string(Key) + " must be a string");
  const auto &Value = It->get_ref<const std::string &>();
  if (Value.size() > Max || Value.find('\0') != std::string::npos)
    throw Error("invalid_request",
                std::string(Key) + " is too long or contains NUL");
  return Value;
}

std::size_t sizeField(const Json &Object, const char *Key, std::size_t Fallback,
                      std::size_t Max) {
  const auto It = Object.find(Key);
  if (It == Object.end())
    return Fallback;
  if (!It->is_number_integer() ||
      (!It->is_number_unsigned() && It->get<std::int64_t>() < 0))
    throw Error("invalid_request",
                std::string(Key) + " must be a nonnegative integer");
  const auto Value = It->get<std::uint64_t>();
  if (Value > Max)
    throw Error("budget_exceeded",
                std::string(Key) + " exceeds the operation budget");
  return static_cast<std::size_t>(Value);
}

Json parseJson(std::string_view Text, std::size_t MaxBytes) {
  if (Text.empty() || Text.size() > MaxBytes)
    throw Error("invalid_frame", "Empty or oversized JSON frame");
  unsigned Depth = 0;
  bool Quoted = false, Escaped = false;
  for (const char C : Text) {
    if (Quoted) {
      if (Escaped)
        Escaped = false;
      else if (C == '\\')
        Escaped = true;
      else if (C == '"')
        Quoted = false;
    } else if (C == '"')
      Quoted = true;
    else if (C == '[' || C == '{') {
      if (++Depth > ::neverd::limits::transport::kJsonParseJsonDepthLimit)
        throw Error("invalid_frame", "JSON nesting exceeds 64 levels");
    } else if ((C == ']' || C == '}') && Depth)
      --Depth;
  }
  try {
#if NEVERD_USE_SIMDJSON
    if (auto Parsed = parseWithSIMD(Text, DuplicateKeys::Reject))
      return std::move(*Parsed);
#endif
    // Preserve acceptance and diagnostic precedence for BOMs, large JSON
    // numbers and malformed input that simdjson refuses.
    UniqueKeys Keys;
    if (!Json::sax_parse(Text, &Keys))
      throw Error("invalid_json", "Malformed UTF-8 JSON frame");
    return Json::parse(Text);
  } catch (const Json::exception &) {
    throw Error("invalid_json", "Malformed UTF-8 JSON frame");
  }
}

Json parseBackendJson(std::string_view Text) {
#if NEVERD_USE_SIMDJSON
  if (auto Parsed = parseWithSIMD(Text, DuplicateKeys::KeepLast))
    return std::move(*Parsed);
#endif
  return Json::parse(Text);
}

std::string serializeJson(const Json &Value, std::size_t MaxBytes) {
  auto Output = std::make_shared<BoundedOutput>(MaxBytes);
  nlohmann::detail::serializer<Json> Serializer(Output, ' ');
  Serializer.dump(Value, false, false, 0);
  return std::move(Output->Bytes);
}

std::string
serializeJsonArray(const Json::object_t &Fields, const std::string &ArrayKey,
                   std::size_t Count,
                   const std::function<const Json &(std::size_t)> &Item,
                   std::size_t MaxBytes, Json::error_handler_t Errors) {
  const auto ArrayPosition = Fields.lower_bound(ArrayKey);
  if (ArrayPosition != Fields.end() && ArrayPosition->first == ArrayKey)
    throw Error("invalid_request",
                "Generated array key duplicates an object field");
  auto Output = std::make_shared<BoundedOutput>(MaxBytes);
  nlohmann::detail::serializer<Json> Serializer(Output, ' ', Errors);
  const auto Write = [&](const Json &Value) {
    Serializer.dump(Value, false, false, 0);
  };
  bool HasField = false;
  const auto Key = [&](const std::string &Name) {
    if (HasField)
      Output->write_character(',');
    HasField = true;
    Write(Name);
    Output->write_character(':');
  };
  Output->write_character('{');
  auto Field = Fields.begin();
  for (; Field != ArrayPosition; ++Field) {
    Key(Field->first);
    Write(Field->second);
  }
  Key(ArrayKey);
  Output->write_character('[');
  for (std::size_t I = 0; I < Count; ++I) {
    if (I)
      Output->write_character(',');
    Write(Item(I));
  }
  Output->write_character(']');
  for (; Field != Fields.end(); ++Field) {
    Key(Field->first);
    Write(Field->second);
  }
  Output->write_character('}');
  return std::move(Output->Bytes);
}
} // namespace neverd::transport
