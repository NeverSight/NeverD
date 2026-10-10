//===- JsonReader.h - Bounded JSON admission ---------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded JSON admission.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "llvm/Support/JSON.h"

#include <cstdint>
#include <string_view>

namespace neverd::web {
struct JsonLimits {
  uint64_t MaxBytes = 32 * 1024 * 1024;
  uint64_t MaxDepth = 64;
  uint64_t MaxNodes = 200000;
  uint64_t MaxStringBytes = 4 * 1024 * 1024;
};

/// Bound structure before constructing a JSON DOM. Duplicate decoded keys
/// are rejected rather than silently overriding evidence. Errors are fixed
/// codes and never contain JSON input, paths or parser diagnostic text.
llvm::json::Value parseBoundedJSON(std::string_view Bytes,
                                   const JsonLimits &Limits = {});
} // namespace neverd::web
