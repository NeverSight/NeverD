#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

namespace neverd::worker {
using Json = nlohmann::json;
inline constexpr std::size_t MaxFrameBytes = 8 * 1024 * 1024;
inline constexpr std::size_t MaxBackendBytes = 32 * 1024 * 1024;
inline constexpr std::size_t MaxQueueBytes = 16 * 1024 * 1024;
inline constexpr std::size_t MaxQueueRequests = 32;

struct Error : std::runtime_error {
  std::string code;
  Json detail;
  Error(std::string code, std::string message, Json detail = Json::object())
      : std::runtime_error(std::move(message)), code(std::move(code)),
        detail(std::move(detail)) {}
};

std::string hexAddress(std::uint64_t value);
std::uint64_t parseAddress(std::string_view text);
std::string stringField(const Json &object, const char *key,
                        std::string fallback = {}, std::size_t max = 4096);
std::size_t sizeField(const Json &object, const char *key, std::size_t fallback,
                      std::size_t max);
Json parseJson(std::string_view text, std::size_t maxBytes = MaxFrameBytes);
void validateRequest(const Json &request);
std::uint32_t frameSize(const unsigned char *header);
std::string frame(const Json &message);
} // namespace neverd::worker
