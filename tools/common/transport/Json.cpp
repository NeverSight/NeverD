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

#include <cstdint>
#include <ostream>
#include <set>
#include <streambuf>
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

class BoundedBuffer final : public std::streambuf {
  std::size_t Maximum;

  std::streamsize xsputn(const char *Data, std::streamsize Count) override {
    if (Count < 0 || static_cast<uint64_t>(Count) > Maximum - Bytes.size())
      throw Error("budget_exceeded", "Encoded JSON exceeds the output budget");
    Bytes.append(Data, static_cast<std::size_t>(Count));
    return Count;
  }
  int_type overflow(int_type Character) override {
    if (traits_type::eq_int_type(Character, traits_type::eof()))
      return traits_type::not_eof(Character);
    const auto C = traits_type::to_char_type(Character);
    xsputn(&C, 1);
    return Character;
  }

public:
  std::string Bytes;
  explicit BoundedBuffer(std::size_t Maximum) : Maximum(Maximum) {}
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
      if (++Depth > 64)
        throw Error("invalid_frame", "JSON nesting exceeds 64 levels");
    } else if ((C == ']' || C == '}') && Depth)
      --Depth;
  }
  try {
    UniqueKeys Keys;
    if (!Json::sax_parse(Text, &Keys))
      throw Error("invalid_json", "Malformed UTF-8 JSON frame");
    return Json::parse(Text);
  } catch (const Json::exception &) {
    throw Error("invalid_json", "Malformed UTF-8 JSON frame");
  }
}

std::string serializeJson(const Json &Value, std::size_t MaxBytes) {
  BoundedBuffer Buffer(MaxBytes);
  std::ostream Output(&Buffer);
  Output.exceptions(std::ios::badbit | std::ios::failbit);
  Output << Value;
  return std::move(Buffer.Bytes);
}
} // namespace neverd::transport
