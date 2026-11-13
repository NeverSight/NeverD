//===- Schema.cpp - Structured action result contracts -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validate declarations once; validate provider values at completion.
//===----------------------------------------------------------------------===//

#include "neverd/plugin/Schema.h"

#include "Text.h"

#include "neverd/plugin/Config.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace neverd::plugin {
namespace {
bool fail(std::string &Error, const std::string &Path, const std::string &Why) {
  Error = Path + ": " + Why;
  return false;
}
bool value(const Json &S, const Json &V, std::string &Error,
           const std::string &Path) {
  const auto Type = S.at("type").get<std::string>();
  const bool Match = Type == "object"    ? V.is_object()
                     : Type == "array"   ? V.is_array()
                     : Type == "string"  ? V.is_string()
                     : Type == "boolean" ? V.is_boolean()
                     : Type == "null"    ? V.is_null()
                     : Type == "integer"
                         ? V.is_number_integer()
                         : V.is_number() && std::isfinite(V.get<double>());
  if (!Match)
    return fail(Error, Path, "expected " + Type);
  if (S.contains("enum") &&
      std::find(S["enum"].begin(), S["enum"].end(), V) == S["enum"].end())
    return fail(Error, Path, "value is outside enum");
  if (V.is_number()) {
    if ((S.contains("minimum") && V < S["minimum"]) ||
        (S.contains("maximum") && V > S["maximum"]))
      return fail(Error, Path, "number is outside bounds");
  } else if (V.is_string() && S.contains("maxLength")) {
    if (detail::codePoints(V.get_ref<const std::string &>()) >
        S["maxLength"].get<size_t>())
      return fail(Error, Path, "string exceeds maxLength");
  } else if (V.is_object()) {
    const auto &Properties = S.at("properties");
    for (const auto &Key : S.value("required", Json::array()))
      if (!V.contains(Key.get<std::string>()))
        return fail(Error, Path, "missing " + Key.get<std::string>());
    for (auto I = V.begin(); I != V.end(); ++I) {
      if (!Properties.contains(I.key())) {
        if (S.at("additionalProperties") == true)
          continue;
        return fail(Error, Path, "unknown property " + I.key());
      }
      if (!value(Properties[I.key()], I.value(), Error, Path + "." + I.key()))
        return false;
    }
  } else if (V.is_array()) {
    if (S.contains("maxItems") && V.size() > S["maxItems"].get<size_t>())
      return fail(Error, Path, "too many items");
    for (size_t I = 0; I < V.size(); ++I)
      if (!value(S.at("items"), V[I], Error,
                 Path + "[" + std::to_string(I) + "]"))
        return false;
  }
  return true;
}
bool schema(const Json &S, std::string &Error, const std::string &Path) {
  if (!S.is_object() || !S.contains("type") || !S["type"].is_string())
    return fail(Error, Path, "schema requires one type");
  const auto Type = S["type"].get<std::string>();
  const std::set<std::string> Types{"object", "array",   "string", "integer",
                                    "number", "boolean", "null"};
  if (!Types.contains(Type))
    return fail(Error, Path, "unsupported type " + Type);
  std::set<std::string> Keys{"type", "description", "enum", "default"};
  if (Type == "object")
    Keys.insert({"properties", "required", "additionalProperties"});
  if (Type == "array")
    Keys.insert({"items", "maxItems"});
  if (Type == "string")
    Keys.insert("maxLength");
  if (Type == "number" || Type == "integer")
    Keys.insert({"minimum", "maximum"});
  for (auto I = S.begin(); I != S.end(); ++I)
    if (!Keys.contains(I.key()))
      return fail(Error, Path, "unsupported keyword " + I.key());
  if (S.contains("description") && !S["description"].is_string())
    return fail(Error, Path, "description must be text");
  if (S.contains("maxLength") &&
      (!S["maxLength"].is_number_integer() || S["maxLength"] < 0 ||
       S["maxLength"] > limits::kMaxTextCharacters))
    return fail(Error, Path,
                "maxLength must be a supported nonnegative length");
  if (Type == "object") {
    if (!S.contains("properties") || !S["properties"].is_object() ||
        !S.contains("additionalProperties") ||
        !S["additionalProperties"].is_boolean())
      return fail(Error, Path,
                  "object requires properties and an explicit "
                  "additionalProperties policy");
    for (auto I = S["properties"].begin(); I != S["properties"].end(); ++I)
      if (!schema(I.value(), Error, Path + "." + I.key()))
        return false;
    if (S.contains("required")) {
      if (!S["required"].is_array())
        return fail(Error, Path, "required must be an array");
      std::set<std::string> Seen;
      for (const auto &Key : S["required"])
        if (!Key.is_string() ||
            !S["properties"].contains(Key.get<std::string>()) ||
            !Seen.insert(Key.get<std::string>()).second)
          return fail(Error, Path, "invalid or repeated required property");
    }
  } else if (Type == "array") {
    if (!S.contains("items") || !schema(S["items"], Error, Path + "[]"))
      return fail(Error, Path, "array requires a supported items schema");
    if (S.contains("maxItems") &&
        (!S["maxItems"].is_number_integer() || S["maxItems"] < 0))
      return fail(Error, Path, "maxItems must be nonnegative");
  }
  for (const auto *Key : {"minimum", "maximum"})
    if (S.contains(Key) &&
        (!S[Key].is_number() || !std::isfinite(S[Key].get<double>())))
      return fail(Error, Path, "numeric bound must be finite");
  if (S.contains("minimum") && S.contains("maximum") &&
      S["minimum"] > S["maximum"])
    return fail(Error, Path, "minimum exceeds maximum");
  if (S.contains("enum")) {
    if (!S["enum"].is_array() || S["enum"].empty())
      return fail(Error, Path, "enum must be a nonempty array");
    for (const auto &Item : S["enum"])
      if (!value(S, Item, Error, Path))
        return false;
  }
  if (S.contains("default") && !value(S, S["default"], Error, Path))
    return false;
  return true;
}
} // namespace

bool validateSchema(const Json &Schema, std::string &Error) {
  Error.clear();
  return schema(Schema, Error, "$");
}
bool validateValue(const Json &Schema, const Json &Value, std::string &Error) {
  Error.clear();
  return value(Schema, Value, Error, "$");
}
Json parameterSchema(const Json &Fields) {
  Json Properties = Json::object(), Required = Json::array();
  for (const auto &Field : Fields) {
    const auto Id = Field.at("id").get<std::string>();
    const auto Type = Field.at("type").get<std::string>();
    Json S{{"type", Type == "choice" ? "string" : Type},
           {"default", Field.at("default")}};
    S["description"] = Field.value("title", Id);
    if (Type == "string") {
      S["maxLength"] =
          Field.value("maxLength", limits::kMaxParameterCharacters);
      if (!Field.contains("maxLength"))
        S["description"] = S["description"].get<std::string>() +
                           "; legacy limit: " +
                           std::to_string(limits::kMaxParameterCharacters) +
                           " UTF-16 code units; no NUL characters";
    }
    if (Type == "choice")
      S["enum"] = Field.at("choices");
    if (Type == "object") {
      S["properties"] = Json::object();
      S["additionalProperties"] = true;
    }
    if (Type == "integer") {
      S["minimum"] = Field.value("minimum", 0);
      S["maximum"] = Field.value("maximum", limits::kDefaultParameterMaximum);
    }
    Properties[Id] = std::move(S);
    // Every v1 parameter has a default; callers may omit explicit overrides.
  }
  return {{"type", "object"},
          {"properties", Properties},
          {"required", Required},
          {"additionalProperties", false}};
}
} // namespace neverd::plugin
