//===- Protocol.h - Framed worker protocol ---------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Worker framing and request schema over the shared bounded JSON layer.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "common/transport/Json.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace neverd::worker {
using transport::Error;
using transport::Json;
using transport::parseJson;
using transport::sizeField;
using transport::stringField;
inline constexpr std::size_t MaxFrameBytes = transport::MaxJsonBytes;
inline constexpr std::size_t MaxBackendBytes = 32 * 1024 * 1024;
inline constexpr std::size_t MaxQueueBytes = 16 * 1024 * 1024;
inline constexpr std::size_t MaxQueueRequests = 32;

std::string hexAddress(std::uint64_t value);
std::uint64_t parseAddress(std::string_view text);
void validateRequest(const Json &request);
std::uint32_t frameSize(const unsigned char *header);
std::string frame(const Json &message);
} // namespace neverd::worker
