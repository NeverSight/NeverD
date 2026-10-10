#pragma once

#include "neverd/web/Blob.h"

#include <string>
#include <string_view>

namespace neverd::web {
inline constexpr std::string_view NativeHandoffProfile =
    "immutable-native-handoff-v1";
inline constexpr uint64_t MaxNativeInputBytes = 256ULL * 1024 * 1024;

/// A selected immutable occurrence, retaining its spool independently of the
/// originating web session. Provenance contains only fixed metadata and IDs.
struct NativeInput {
  Blob Content;
  std::string BlobHash, Provenance;
};
} // namespace neverd::web
