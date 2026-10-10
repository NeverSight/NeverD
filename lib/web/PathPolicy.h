#pragma once

#include <string>
#include <string_view>

namespace neverd::web {
inline constexpr std::string_view ArchivePathProfile =
    "icu-77.1-nfc-casefold-portable-v1";
bool archivePathsAvailable();
/// Private comparison key only, never a replacement for an evidence name.
/// Validates every component and refuses absolute, device and alias paths.
std::string archivePathKey(std::string_view Path);
} // namespace neverd::web
