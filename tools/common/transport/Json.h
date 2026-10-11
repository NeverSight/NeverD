//===- Json.h - Bounded tool transport JSON ---------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared JSON admission and fixed transport diagnostics.
///
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

namespace neverd::transport {
using Json = nlohmann::json;
inline constexpr std::size_t MaxJsonBytes = 8 * 1024 * 1024;

struct Error : std::runtime_error {
  std::string code;
  Json detail;
  Error(std::string Code, std::string Message, Json Detail = Json::object())
      : std::runtime_error(std::move(Message)), code(std::move(Code)),
        detail(std::move(Detail)) {}
};

std::string stringField(const Json &Object, const char *Key,
                        std::string Fallback = {}, std::size_t Max = 4096);
std::size_t sizeField(const Json &Object, const char *Key, std::size_t Fallback,
                      std::size_t Max);
Json parseJson(std::string_view Text, std::size_t MaxBytes = MaxJsonBytes);
/// Bound allocation while serializing, including JSON escaping.
std::string serializeJson(const Json &Value,
                          std::size_t MaxBytes = MaxJsonBytes);
} // namespace neverd::transport
