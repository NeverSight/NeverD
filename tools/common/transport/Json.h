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

#include "ResourceLimits.h"

#include <cstddef>
#include <functional>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

namespace neverd::transport {
using Json = nlohmann::json;

struct Error : std::runtime_error {
  std::string code;
  Json detail;
  Error(std::string Code, std::string Message, Json Detail = Json::object())
      : std::runtime_error(std::move(Message)), code(std::move(Code)),
        detail(std::move(Detail)) {}
};

std::string
stringField(const Json &Object, const char *Key, std::string Fallback = {},
            std::size_t Max = ::neverd::limits::transport::kJsonMax);
std::size_t sizeField(const Json &Object, const char *Key, std::size_t Fallback,
                      std::size_t Max);
Json parseJson(
    std::string_view Text,
    std::size_t MaxBytes = ::neverd::limits::transport::kJsonMaxJsonBytes);
/// Parse SDK-owned output with Json::parse's number and last-duplicate-key
/// semantics, without imposing transport frame limits. Frames and sidecar files
/// must use parseJson instead. Syntax failures remain Json::exception values.
Json parseBackendJson(std::string_view Text);
/// Bound allocation while serializing, including JSON escaping.
std::string serializeJson(
    const Json &Value,
    std::size_t MaxBytes = ::neverd::limits::transport::kJsonMaxJsonBytes);
/// Encode an object with one generated array in ordinary sorted key order.
/// Fields must not contain ArrayKey. Item is called synchronously in ascending
/// index order; each borrowed value must survive until the next invocation
/// or until serialization returns.
/// Retains the existing codec and checks each encoded append against MaxBytes.
std::string serializeJsonArray(
    const Json::object_t &Fields, const std::string &ArrayKey,
    std::size_t Count, const std::function<const Json &(std::size_t)> &Item,
    std::size_t MaxBytes = ::neverd::limits::transport::kJsonMaxJsonBytes,
    Json::error_handler_t Errors = Json::error_handler_t::strict);
} // namespace neverd::transport

// Preserve interface spellings; resource values are defined by Limits.h.
namespace neverd::transport {
inline constexpr std::size_t MaxJsonBytes =
    ::neverd::limits::transport::kJsonMaxJsonBytes;
}
